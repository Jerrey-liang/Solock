#define _WIN32_DCOM
#include "SolockControllerInternal.h"

#include <winrt/Windows.Networking.Connectivity.h>

#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <sstream>

using namespace winrt::Windows::Networking::Connectivity;

namespace
{
    using solock::internal::ClampMinuteOfDay;
    using solock::internal::DebugLog;
}

SolockController::SolockController(const Options& options)
    : m_options(options),
      m_eveningIdleLockApplied(false),
      m_eveningHotspotAliasSource(),
      m_eveningHotspotAlias(),
      m_customBlockStates(),
      m_customBlockConfigSignature(),
      m_wfpEngine(nullptr),
      m_blockedAppFiltersInstalled(false),
      m_audioDeviceChangeEvent(nullptr),
      m_audioDeviceEnumerator(nullptr),
      m_audioNotificationClient(nullptr)
{
}

SolockController::~SolockController()
{
    ShutdownAudioVolumeMonitoring();
    CloseWfpEngine();
    ClearKeepSystemAwake();
}

int SolockController::Run()
{
    if (m_options.autoRegisterScheduledTask)
    {
        EnsureStartupTaskRegistered(m_options.scheduledTaskName);
    }

    InitializeAudioVolumeMonitoring();
    return RunWithSchedule();
}

int SolockController::RunAllFeaturesAcceleratedDebug()
{
    struct DebugStepResult
    {
        std::wstring name;
        bool ok;
    };

    std::vector<DebugStepResult> stepResults;
    auto recordStep = [&](const std::wstring& name, const bool ok)
    {
        stepResults.push_back({ name, ok });
        DebugLog(std::wstring(L"[ACCEL] ") + name + L": " + (ok ? L"ok" : L"failed"));
        return ok;
    };

    auto sleepBetweenSteps = [&]()
    {
        const int delayMilliseconds = std::clamp(m_options.debugStepDelayMilliseconds, 0, 5000);
        if (delayMilliseconds > 0)
        {
            ::Sleep(static_cast<DWORD>(delayMilliseconds));
        }
    };

    DebugLog(L"[ACCEL] full feature debug started.");

    recordStep(L"audio / InitializeAudioVolumeMonitoring", InitializeAudioVolumeMonitoring());
    sleepBetweenSteps();

    if (m_options.autoRegisterScheduledTask)
    {
        recordStep(L"task / EnsureStartupTaskRegistered", EnsureStartupTaskRegistered(m_options.scheduledTaskName));
        sleepBetweenSteps();
    }

    recordStep(L"system / AssertKeepSystemAwake", AssertKeepSystemAwake());
    sleepBetweenSteps();

    recordStep(L"audio / EnsureAudioVolumeMatchesPhase(ScheduledBlocks)", EnsureAudioVolumeMatchesPhase(Phase::ScheduledBlocks));
    sleepBetweenSteps();

    ResetEveningHotspotAlias();
    recordStep(L"hotspot / EnsurePreActionHotspot(initial)", EnsurePreActionHotspot());
    sleepBetweenSteps();

    recordStep(L"network / EnsureTargetAppsNetworkingEnabled(initial)", EnsureTargetAppsNetworkingEnabled());
    sleepBetweenSteps();

    recordStep(L"network / EnsureTargetAppsNetworkingBlocked", EnsureTargetAppsNetworkingBlocked());
    sleepBetweenSteps();

    recordStep(L"audio / EnsureAudioVolumeMatchesPhase(MiddayIdleShutdown)", EnsureAudioVolumeMatchesPhase(Phase::MiddayIdleShutdown));
    sleepBetweenSteps();

    if (m_options.debugForceIdleState)
    {
        recordStep(L"power / ShutdownMachineNow", ShutdownMachineNow());
        sleepBetweenSteps();
    }

    ResetEveningHotspotAlias();
    recordStep(L"network+hotspot / EnsureEveningPostActionState", EnsureEveningPostActionState());
    sleepBetweenSteps();

    recordStep(L"audio / EnsureAudioVolumeMatchesPhase(EveningPostAction)", EnsureAudioVolumeMatchesPhase(Phase::EveningPostAction));
    sleepBetweenSteps();

    recordStep(L"session / ApplyEveningIdleLockIfNeeded", ApplyEveningIdleLockIfNeeded());
    sleepBetweenSteps();

    recordStep(L"hotspot / EnsurePreActionHotspot(restore)", EnsurePreActionHotspot());
    sleepBetweenSteps();

    recordStep(L"network / EnsureTargetAppsNetworkingEnabled(cleanup)", EnsureTargetAppsNetworkingEnabled());
    ResetEveningHotspotAlias();
    ClearKeepSystemAwake();

    int failureCount = 0;
    std::wostringstream summary;
    summary << L"[ACCEL] full feature debug completed with ";
    for (const auto& step : stepResults)
    {
        if (!step.ok)
        {
            ++failureCount;
        }
    }

    summary << failureCount << L" failure(s) across " << stepResults.size() << L" step(s).";
    if (failureCount > 0)
    {
        summary << L" Failed steps:";
        for (const auto& step : stepResults)
        {
            if (!step.ok)
            {
                summary << L" [" << step.name << L"]";
            }
        }
    }

    DebugLog(summary.str());
    system("pause");
    return failureCount == 0 ? 0 : 40;
}

int SolockController::RunBlockedAppNetworkingDebug()
{
    DebugLog(L"[WFP] standalone debug entry started.");
    const auto beforeProcesses = ResolveRunningBlockedProcesses();
    DebugLogRunningBlockedProcesses(L"before blocking", beforeProcesses);

    if (beforeProcesses.empty())
    {
        DebugLog(L"[WFP] debug run aborted because no matching target process is currently running.");
        return 33;
    }

    const bool startupOk = EnsureTargetAppsNetworkingEnabled();
    DebugLog(std::wstring(L"[WFP] startup unblock check result: ") + (startupOk ? L"success" : L"failure"));
    if (!startupOk)
    {
        return 31;
    }

    const bool ok = EnsureTargetAppsNetworkingBlocked();
    DebugLog(std::wstring(L"[WFP] standalone debug result: ") + (ok ? L"success" : L"failure"));
    const auto afterProcesses = ResolveRunningBlockedProcesses();
    DebugLogRunningBlockedProcesses(L"after blocking", afterProcesses);

#ifdef _DEBUG
    if (ok)
    {
        std::wcout << L"[DEBUG] WFP dynamic filters are active." << std::endl;
        std::wcout << L"[DEBUG] Press Enter to remove the filters and end the debug run." << std::endl;
        std::wstring line;
        std::getline(std::wcin, line);

        const bool cleanupOk = EnsureTargetAppsNetworkingEnabled();
        DebugLog(std::wstring(L"[WFP] debug cleanup unblock result: ") + (cleanupOk ? L"success" : L"failure"));
        return cleanupOk ? 0 : 32;
    }
#endif

    return ok ? 0 : 30;
}

std::chrono::system_clock::time_point SolockController::LocalAtOnSameDay(
    const std::chrono::system_clock::time_point& referenceNow,
    const int hour,
    const int minute,
    const int second)
{
    const std::time_t timeValue = std::chrono::system_clock::to_time_t(referenceNow);

    std::tm localTm = {};
    localtime_s(&localTm, &timeValue);

    localTm.tm_hour = hour;
    localTm.tm_min = minute;
    localTm.tm_sec = second;

    return std::chrono::system_clock::from_time_t(std::mktime(&localTm));
}

SolockController::ScheduleTimes SolockController::GetScheduleTimesFor(
    const std::chrono::system_clock::time_point& now) const
{
    const int middayStartMinuteOfDay =
        ClampMinuteOfDay(m_options.middayShutdownStartHour, m_options.middayShutdownStartMinute);
    const int middayEndMinuteOfDay =
        ClampMinuteOfDay(m_options.middayShutdownEndHour, m_options.middayShutdownEndMinute);
    const int eveningStartMinuteOfDay =
        ClampMinuteOfDay(m_options.eveningPostActionStartHour, m_options.eveningPostActionStartMinute);

    return
    {
        LocalAtOnSameDay(now, middayStartMinuteOfDay / 60, middayStartMinuteOfDay % 60, 0),
        LocalAtOnSameDay(now, middayEndMinuteOfDay / 60, middayEndMinuteOfDay % 60, 0),
        LocalAtOnSameDay(now, eveningStartMinuteOfDay / 60, eveningStartMinuteOfDay % 60, 0)
    };
}

SolockController::Phase SolockController::GetPhaseAt(
    const std::chrono::system_clock::time_point& now,
    const ScheduleTimes& scheduleTimes)
{
    if (now >= scheduleTimes.eveningPostActionStartTime)
    {
        return Phase::EveningPostAction;
    }

    if (now >= scheduleTimes.middayShutdownStartTime &&
        now < scheduleTimes.middayShutdownEndTime)
    {
        return Phase::MiddayIdleShutdown;
    }

    return Phase::ScheduledBlocks;
}

int SolockController::RunWithSchedule()
{
    const int middayStartMinuteOfDay =
        ClampMinuteOfDay(m_options.middayShutdownStartHour, m_options.middayShutdownStartMinute);
    const int middayEndMinuteOfDay =
        ClampMinuteOfDay(m_options.middayShutdownEndHour, m_options.middayShutdownEndMinute);
    const int eveningStartMinuteOfDay =
        ClampMinuteOfDay(m_options.eveningPostActionStartHour, m_options.eveningPostActionStartMinute);
    if (middayEndMinuteOfDay < middayStartMinuteOfDay ||
        eveningStartMinuteOfDay < middayEndMinuteOfDay)
    {
        return 20;
    }

    if (!EnsureTargetAppsNetworkingEnabled())
    {
        return 24;
    }

    const auto now = std::chrono::system_clock::now();
    const auto initialPhase = GetPhaseAt(now, GetScheduleTimesFor(now));

    AssertKeepSystemAwake();
    EnsureAudioVolumeMatchesPhase(initialPhase);

    if (initialPhase == Phase::EveningPostAction)
    {
        EnsureEveningPostActionState();
    }
    else
    {
        if (initialPhase == Phase::ScheduledBlocks)
        {
            EnsureTargetAppsNetworkingMatchesSchedule(now);
            WaitForSystemAndNetworkStability();
        }
        else if (initialPhase == Phase::MiddayIdleShutdown)
        {
            EnsureTargetAppsNetworkingBlocked();
        }

        EnsurePreActionHotspot();
    }

    const auto idleThreshold = std::chrono::minutes(std::max(1, m_options.inactivityThresholdMinutes));
    const int heartbeatSeconds = std::max(1, m_options.heartbeatSeconds);
    while (true)
    {
        const auto currentNow = std::chrono::system_clock::now();
        const auto scheduleTimes = GetScheduleTimesFor(currentNow);
        const auto phase = GetPhaseAt(currentNow, scheduleTimes);

        AssertKeepSystemAwake();
        EnsureAudioVolumeMatchesPhase(phase);

        if (phase != Phase::EveningPostAction)
        {
            m_eveningIdleLockApplied = false;
            ResetEveningHotspotAlias();
        }

        switch (phase)
        {
        case Phase::ScheduledBlocks:
            EnsurePreActionHotspot();
            EnsureTargetAppsNetworkingMatchesSchedule(currentNow);
            break;

        case Phase::MiddayIdleShutdown:
            EnsurePreActionHotspot();
            EnsureTargetAppsNetworkingBlocked();
            if (IsInputIdleForAtLeast(idleThreshold))
            {
                return ShutdownMachineNow() ? 0 : 21;
            }
            break;

        case Phase::EveningPostAction:
            EnsureEveningPostActionState();
            ApplyEveningIdleLockIfNeeded();
            break;
        }

        WaitForHeartbeatOrAudioEvent(heartbeatSeconds);
    }
}

bool SolockController::WaitForSystemAndNetworkStability()
{
    int stable = 0;
    const int maxWait = std::max(1, m_options.startupMaxWaitSeconds);
    const int needStable = std::max(1, m_options.startupStableSeconds);

    for (int waited = 0; waited < maxWait; ++waited)
    {
        if (IsNetworkUsableNow())
        {
            ++stable;
            if (stable >= needStable)
            {
                return true;
            }
        }
        else
        {
            stable = 0;
        }

        ::Sleep(1000);
    }

    return false;
}

bool SolockController::IsNetworkUsableNow() const
{
    try
    {
        const ConnectionProfile profile = NetworkInformation::GetInternetConnectionProfile();
        if (!profile)
        {
            return false;
        }

        return profile.GetNetworkConnectivityLevel() == NetworkConnectivityLevel::InternetAccess;
    }
    catch (...)
    {
        return false;
    }
}

bool SolockController::IsInputIdleForAtLeast(const std::chrono::milliseconds idleThreshold) const
{
#ifdef _DEBUG
    if (m_options.debugForceIdleState)
    {
        return true;
    }
#endif

    LASTINPUTINFO lastInputInfo = {};
    lastInputInfo.cbSize = sizeof(lastInputInfo);
    if (!::GetLastInputInfo(&lastInputInfo))
    {
        return false;
    }

    const ULONGLONG nowTicks = ::GetTickCount64();
    const ULONGLONG idleMilliseconds = nowTicks - static_cast<ULONGLONG>(lastInputInfo.dwTime);
    return std::chrono::milliseconds(
        static_cast<std::chrono::milliseconds::rep>(idleMilliseconds)) >= idleThreshold;
}

bool SolockController::AssertKeepSystemAwake()
{
    return ::SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED) != 0;
}

void SolockController::ClearKeepSystemAwake()
{
    ::SetThreadExecutionState(ES_CONTINUOUS);
}
