#define _WIN32_DCOM
#include "SolockControllerInternal.h"

#include <fwpmu.h>
#include <TlHelp32.h>

#include <algorithm>
#include <cwchar>
#include <sstream>

#pragma comment(lib, "fwpuclnt.lib")

namespace
{
    using solock::internal::DebugLog;
    using solock::internal::DebugLogStatus;
    using solock::internal::EqualsIgnoreCase;
    using solock::internal::kMaxMinuteOfDay;

    constexpr wchar_t kWfpSessionName[] = L"Solock Dynamic WFP Session";
    constexpr wchar_t kWfpSublayerName[] = L"Solock App Block Sublayer";
    constexpr GUID kSolockWfpSublayerKey =
    { 0x618ab011, 0xcb31, 0x4b31, { 0x92, 0xfd, 0x86, 0x40, 0xe0, 0x25, 0x84, 0xfa } };

    FWPM_DISPLAY_DATA0 MakeDisplayData(const wchar_t* name, const wchar_t* description = nullptr)
    {
        FWPM_DISPLAY_DATA0 displayData = {};
        displayData.name = const_cast<wchar_t*>(name);
        displayData.description = const_cast<wchar_t*>(description);
        return displayData;
    }

    std::wstring GetFileNameFromPath(const std::wstring& path)
    {
        const size_t pos = path.find_last_of(L"\\/");
        if (pos == std::wstring::npos)
        {
            return path;
        }

        return path.substr(pos + 1);
    }

    std::wstring BuildBlockedAppFilterName(const std::wstring& appPath, const wchar_t* addressFamily)
    {
        return L"Solock block " + GetFileNameFromPath(appPath) + L" " + addressFamily + L" outbound";
    }

    bool ContainsStringIgnoreCase(const std::vector<std::wstring>& values, const std::wstring& expected)
    {
        return std::any_of(values.begin(), values.end(), [&](const std::wstring& value)
        {
            return EqualsIgnoreCase(value, expected);
        });
    }

    bool HaveSamePathSetIgnoreCase(
        const std::vector<std::wstring>& leftValues,
        const std::vector<std::wstring>& rightValues)
    {
        if (leftValues.size() != rightValues.size())
        {
            return false;
        }

        std::vector<std::wstring> left = leftValues;
        std::vector<std::wstring> right = rightValues;
        auto comparer = [](const std::wstring& a, const std::wstring& b)
        {
            return _wcsicmp(a.c_str(), b.c_str()) < 0;
        };

        std::sort(left.begin(), left.end(), comparer);
        std::sort(right.begin(), right.end(), comparer);

        for (size_t i = 0; i < left.size(); ++i)
        {
            if (!EqualsIgnoreCase(left[i], right[i]))
            {
                return false;
            }
        }

        return true;
    }

    bool AddUniquePathIgnoreCase(std::vector<std::wstring>& values, const std::wstring& path)
    {
        if (path.empty() || ContainsStringIgnoreCase(values, path))
        {
            return false;
        }

        values.push_back(path);
        return true;
    }

    DWORD AddBlockedAppFilter(
        HANDLE engineHandle,
        const GUID& layerKey,
        const wchar_t* filterName,
        FWP_BYTE_BLOB* appId,
        UINT64* filterId)
    {
        FWPM_FILTER_CONDITION0 condition = {};
        condition.fieldKey = FWPM_CONDITION_ALE_APP_ID;
        condition.matchType = FWP_MATCH_EQUAL;
        condition.conditionValue.type = FWP_BYTE_BLOB_TYPE;
        condition.conditionValue.byteBlob = appId;

        FWPM_FILTER0 filter = {};
        filter.displayData = MakeDisplayData(filterName);
        filter.layerKey = layerKey;
        filter.subLayerKey = kSolockWfpSublayerKey;
        filter.weight.type = FWP_EMPTY;
        filter.numFilterConditions = 1;
        filter.filterCondition = &condition;
        filter.action.type = FWP_ACTION_BLOCK;

        return ::FwpmFilterAdd0(engineHandle, &filter, nullptr, filterId);
    }
}

bool SolockController::EnsureTargetAppsNetworkingBlocked()
{
    bool hasConfiguredTarget = false;
    for (const auto& processName : m_options.blockedProcessNames)
    {
        if (!processName.empty())
        {
            hasConfiguredTarget = true;
            break;
        }
    }

    if (!hasConfiguredTarget)
    {
        DebugLog(L"[WFP] blockedProcessNames is empty, skipping filter installation.");
        return true;
    }

    const std::vector<RunningBlockedProcess> runningProcesses = ResolveRunningBlockedProcesses();
    DebugLogRunningBlockedProcesses(L"resolved for blocking", runningProcesses);
    std::vector<std::wstring> resolvedAppPaths;
    resolvedAppPaths.reserve(runningProcesses.size());
    for (const auto& process : runningProcesses)
    {
        AddUniquePathIgnoreCase(resolvedAppPaths, process.appPath);
    }

    if (resolvedAppPaths.empty())
    {
        if (m_blockedAppFiltersInstalled && m_wfpEngine != nullptr && !m_installedBlockedAppFilters.empty())
        {
            DebugLog(L"[WFP] no matching running process was found; keeping existing block filters.");
            return true;
        }

        DebugLog(L"[WFP] no matching running process was found for blockedProcessNames.");
        return true;
    }

    if (m_blockedAppFiltersInstalled && m_wfpEngine != nullptr)
    {
        std::vector<std::wstring> installedAppPaths;
        installedAppPaths.reserve(m_installedBlockedAppFilters.size());
        for (const auto& filter : m_installedBlockedAppFilters)
        {
            installedAppPaths.push_back(filter.appPath);
        }

        if (HaveSamePathSetIgnoreCase(installedAppPaths, resolvedAppPaths))
        {
            DebugLog(L"[WFP] block filters already installed for the current target process paths.");
            return true;
        }
    }

    if (m_wfpEngine != nullptr || !m_installedBlockedAppFilters.empty())
    {
        CloseWfpEngine();
    }

    FWPM_SESSION0 session = {};
    auto cleanupOnFailure = [this]()
    {
        CloseWfpEngine();
    };

    DebugLog(L"[WFP] opening dynamic WFP session.");
    session.flags = FWPM_SESSION_FLAG_DYNAMIC;
    session.displayData = MakeDisplayData(kWfpSessionName);

    const DWORD openStatus = ::FwpmEngineOpen0(
        nullptr,
        RPC_C_AUTHN_WINNT,
        nullptr,
        &session,
        &m_wfpEngine);
    DebugLogStatus(L"FwpmEngineOpen0", openStatus);
    if (openStatus != ERROR_SUCCESS)
    {
        return false;
    }

    FWPM_SUBLAYER0 subLayer = {};
    subLayer.subLayerKey = kSolockWfpSublayerKey;
    subLayer.displayData = MakeDisplayData(kWfpSublayerName);
    subLayer.weight = 0x100;

    const DWORD subLayerStatus = ::FwpmSubLayerAdd0(m_wfpEngine, &subLayer, nullptr);
    DebugLogStatus(L"FwpmSubLayerAdd0", subLayerStatus);
    if (subLayerStatus != ERROR_SUCCESS && subLayerStatus != FWP_E_ALREADY_EXISTS)
    {
        cleanupOnFailure();
        return false;
    }

    m_installedBlockedAppFilters.clear();

    for (const auto& appPath : resolvedAppPaths)
    {
        FWP_BYTE_BLOB* appId = nullptr;
        DebugLog(L"[WFP] resolving ALE_APP_ID for " + appPath);
        const DWORD appIdStatus = ::FwpmGetAppIdFromFileName0(appPath.c_str(), &appId);
        DebugLogStatus(L"FwpmGetAppIdFromFileName0", appIdStatus);
        if (appIdStatus != ERROR_SUCCESS || appId == nullptr)
        {
            if (appId != nullptr)
            {
                ::FwpmFreeMemory0(reinterpret_cast<void**>(&appId));
            }
            cleanupOnFailure();
            return false;
        }

        const std::wstring ipv4FilterName = BuildBlockedAppFilterName(appPath, L"IPv4");
        UINT64 ipv4FilterId = 0;
        const DWORD ipv4Status = AddBlockedAppFilter(
            m_wfpEngine,
            FWPM_LAYER_ALE_AUTH_CONNECT_V4,
            ipv4FilterName.c_str(),
            appId,
            &ipv4FilterId);
        DebugLogStatus(L"FwpmFilterAdd0 IPv4", ipv4Status);
        if (ipv4Status != ERROR_SUCCESS)
        {
            ::FwpmFreeMemory0(reinterpret_cast<void**>(&appId));
            cleanupOnFailure();
            return false;
        }

        const std::wstring ipv6FilterName = BuildBlockedAppFilterName(appPath, L"IPv6");
        UINT64 ipv6FilterId = 0;
        const DWORD ipv6Status = AddBlockedAppFilter(
            m_wfpEngine,
            FWPM_LAYER_ALE_AUTH_CONNECT_V6,
            ipv6FilterName.c_str(),
            appId,
            &ipv6FilterId);
        DebugLogStatus(L"FwpmFilterAdd0 IPv6", ipv6Status);
        if (ipv6Status != ERROR_SUCCESS)
        {
            ::FwpmFreeMemory0(reinterpret_cast<void**>(&appId));
            cleanupOnFailure();
            return false;
        }

        ::FwpmFreeMemory0(reinterpret_cast<void**>(&appId));
        m_installedBlockedAppFilters.push_back({ appPath, ipv4FilterId, ipv6FilterId });
    }

    if (m_installedBlockedAppFilters.empty())
    {
        CloseWfpEngine();
        return true;
    }

    m_blockedAppFiltersInstalled = true;

#ifdef _DEBUG
    {
        std::wostringstream out;
        out << L"[WFP] installed block filters for " << m_installedBlockedAppFilters.size() << L" target app(s):";
        for (const auto& filter : m_installedBlockedAppFilters)
        {
            out << L" [" << GetFileNameFromPath(filter.appPath)
                << L" IPv4=" << filter.ipv4FilterId
                << L", IPv6=" << filter.ipv6FilterId << L"]";
        }
        DebugLog(out.str());
    }
#endif

    return true;
}

bool SolockController::EnsureTargetAppsNetworkingEnabled()
{
    if (m_wfpEngine == nullptr && !m_blockedAppFiltersInstalled && m_installedBlockedAppFilters.empty())
    {
        return true;
    }

    DebugLog(L"[WFP] removing dynamic block filters to ensure target apps can network.");
    CloseWfpEngine();
    return m_wfpEngine == nullptr && !m_blockedAppFiltersInstalled && m_installedBlockedAppFilters.empty();
}

std::vector<SolockController::RunningBlockedProcess> SolockController::ResolveRunningBlockedProcesses() const
{
    std::vector<std::wstring> targetProcessNames;
    targetProcessNames.reserve(m_options.blockedProcessNames.size());
    for (const auto& configuredName : m_options.blockedProcessNames)
    {
        if (configuredName.empty())
        {
            continue;
        }

        const std::wstring fileName = GetFileNameFromPath(configuredName);
        if (!fileName.empty() && !ContainsStringIgnoreCase(targetProcessNames, fileName))
        {
            targetProcessNames.push_back(fileName);
        }
    }

    if (targetProcessNames.empty())
    {
        return {};
    }

    HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
    {
        return {};
    }

    std::vector<RunningBlockedProcess> resolvedProcesses;
    PROCESSENTRY32W entry = {};
    entry.dwSize = sizeof(entry);
    if (::Process32FirstW(snapshot, &entry))
    {
        do
        {
            if (!ContainsStringIgnoreCase(targetProcessNames, entry.szExeFile))
            {
                continue;
            }

            HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
            if (process == nullptr)
            {
                continue;
            }

            std::wstring path(32768, L'\0');
            DWORD pathLength = static_cast<DWORD>(path.size());
            if (::QueryFullProcessImageNameW(process, 0, path.data(), &pathLength))
            {
                path.resize(pathLength);
                if (!path.empty())
                {
                    resolvedProcesses.push_back({ entry.th32ProcessID, path });
                }
            }

            ::CloseHandle(process);
        }
        while (::Process32NextW(snapshot, &entry));
    }

    ::CloseHandle(snapshot);
    return resolvedProcesses;
}

bool SolockController::EnsureTargetAppsNetworkingMatchesSchedule(
    const std::chrono::system_clock::time_point& now)
{
    if (ShouldBlockTargetAppsAt(now))
    {
        return EnsureTargetAppsNetworkingBlocked();
    }

    return EnsureTargetAppsNetworkingEnabled();
}

bool SolockController::ShouldBlockTargetAppsAt(const std::chrono::system_clock::time_point& now)
{
    const ExternalOverrides overrides = LoadExternalOverrides();
    if (IsCustomBlockActiveAt(now, overrides))
    {
        return true;
    }

    const int durationMinutes = std::max(1, m_options.scheduledBlockDurationMinutes);
    for (const int startMinuteOfDay : m_options.scheduledBlockStartMinutesOfDay)
    {
        const int safeStartMinuteOfDay = std::clamp(startMinuteOfDay, 0, kMaxMinuteOfDay);
        const auto blockStart = LocalAtOnSameDay(now, safeStartMinuteOfDay / 60, safeStartMinuteOfDay % 60, 0);
        const auto blockEnd = blockStart + std::chrono::minutes(durationMinutes);
        if (now >= blockStart && now < blockEnd)
        {
            return true;
        }
    }

    return false;
}

bool SolockController::IsCustomBlockActiveAt(
    const std::chrono::system_clock::time_point& now,
    const ExternalOverrides& overrides)
{
    if (overrides.customBlockSignature != m_customBlockConfigSignature)
    {
        m_customBlockConfigSignature = overrides.customBlockSignature;
        m_customBlockStates.assign(overrides.customBlocks.size(), CustomBlockRuntimeState{});
    }
    else if (m_customBlockStates.size() != overrides.customBlocks.size())
    {
        m_customBlockStates.resize(overrides.customBlocks.size());
    }

    for (size_t index = 0; index < overrides.customBlocks.size(); ++index)
    {
        const CustomBlockWindow& block = overrides.customBlocks[index];
        if (!block.hasStart)
        {
            continue;
        }

        CustomBlockRuntimeState& state = m_customBlockStates[index];
        if (!state.activated)
        {
            const auto customBlockStart = LocalAtOnSameDay(
                now,
                block.startMinutesOfDay / 60,
                block.startMinutesOfDay % 60,
                0);
            if (now < customBlockStart)
            {
                continue;
            }

            state.activated = true;
            state.activationTime = customBlockStart;
        }

        if (!block.hasCustomBlockDurationMinutes)
        {
            return true;
        }

        const int repeatCount = block.hasCustomBlockRepeatCount
            ? std::max(1, block.customBlockRepeatCount)
            : 1;
        const auto segmentDuration = std::chrono::minutes(block.customBlockDurationMinutes);
        const auto segmentInterval = std::chrono::minutes(
            block.hasCustomBlockIntervalMinutes
                ? std::max(0, block.customBlockIntervalMinutes)
                : 0);
        const auto cycleDuration = segmentDuration + segmentInterval;

        for (int repeatIndex = 0; repeatIndex < repeatCount; ++repeatIndex)
        {
            const auto segmentStart = state.activationTime + (cycleDuration * repeatIndex);
            const auto segmentEnd = segmentStart + segmentDuration;
            if (now >= segmentStart && now < segmentEnd)
            {
                return true;
            }
        }
    }

    return false;
}

void SolockController::CloseWfpEngine()
{
    if (m_wfpEngine != nullptr)
    {
        DebugLog(L"[WFP] closing dynamic WFP session.");
        ::FwpmEngineClose0(m_wfpEngine);
        m_wfpEngine = nullptr;
    }

    m_blockedAppFiltersInstalled = false;
    m_installedBlockedAppFilters.clear();
}

bool SolockController::EnsureEveningPostActionState()
{
    DebugLog(L"[WFP] evening post-action started, ensuring outbound block filters are installed.");
    const bool appBlockOk = EnsureTargetAppsNetworkingBlocked();
    DebugLog(std::wstring(L"[WFP] outbound block result: ") + (appBlockOk ? L"success" : L"failure"));

    const bool hotspotOk = EnsureEveningHotspotState();
    return hotspotOk && appBlockOk;
}

bool SolockController::ApplyEveningIdleLockIfNeeded()
{
    if (m_eveningIdleLockApplied)
    {
        return true;
    }

    const auto idleThreshold = std::chrono::minutes(std::max(1, m_options.inactivityThresholdMinutes));
    if (!IsInputIdleForAtLeast(idleThreshold))
    {
        return true;
    }

    const bool lockOk = LockCurrentSession();
    ::Sleep(1200);
    const bool displayOk = TurnOffDisplay();
    if (lockOk && displayOk)
    {
        m_eveningIdleLockApplied = true;
        EnsureAudioVolumeMatchesPhase(Phase::EveningPostAction);
    }

    return lockOk && displayOk;
}

void SolockController::DebugLogRunningBlockedProcesses(
    const wchar_t* stage,
    const std::vector<RunningBlockedProcess>& runningProcesses) const
{
    std::wostringstream out;
    out << L"[WFP] " << (stage != nullptr ? stage : L"process snapshot")
        << L": " << runningProcesses.size() << L" running target process(es)";

    for (const auto& process : runningProcesses)
    {
        out << L" [PID=" << process.processId << L", Path=" << process.appPath << L"]";
    }

    DebugLog(out.str());
}
