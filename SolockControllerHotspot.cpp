#define _WIN32_DCOM
#include "SolockControllerInternal.h"

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Networking.Connectivity.h>
#include <winrt/Windows.Networking.NetworkOperators.h>

#include <algorithm>
#include <cwchar>
#include <cwctype>
#include <random>
#include <stdexcept>

#pragma comment(lib, "windowsapp.lib")

using namespace winrt;
using namespace Windows::Networking::Connectivity;
using namespace Windows::Networking::NetworkOperators;

namespace
{
    using solock::internal::DebugLog;
    using solock::internal::EqualsIgnoreCase;

    constexpr int kTetheringOperationalStateOn = 1;
    constexpr wchar_t kSeewoPrefix[] = L"seewo-";

    std::wstring ToWString(const hstring& value)
    {
        return std::wstring(value.c_str());
    }

    std::wstring ToWString(const std::string& value)
    {
        return std::wstring(value.begin(), value.end());
    }

    bool IsTetheringOn(const NetworkOperatorTetheringManager& manager)
    {
        return static_cast<int>(manager.TetheringOperationalState()) == kTetheringOperationalStateOn;
    }

    bool StartsWithIgnoreCase(const std::wstring& value, const wchar_t* prefix)
    {
        if (prefix == nullptr)
        {
            return false;
        }

        const size_t prefixLength = std::wcslen(prefix);
        return value.size() >= prefixLength &&
            _wcsnicmp(value.c_str(), prefix, prefixLength) == 0;
    }

    std::mt19937& GetRandomGenerator()
    {
        static thread_local std::mt19937 generator(std::random_device{}());
        return generator;
    }

    std::wstring ExtractRandomizableSsidCharacters(const std::wstring& sourceSsid)
    {
        std::wstring seed = sourceSsid;
        if (StartsWithIgnoreCase(sourceSsid, kSeewoPrefix))
        {
            seed = sourceSsid.substr(std::wcslen(kSeewoPrefix));
        }

        std::wstring filtered;
        filtered.reserve(seed.size());
        for (const wchar_t ch : seed)
        {
            if (std::iswalnum(static_cast<wint_t>(ch)))
            {
                filtered.push_back(ch);
            }
        }

        if (!filtered.empty())
        {
            return filtered;
        }

        filtered.reserve(sourceSsid.size());
        for (const wchar_t ch : sourceSsid)
        {
            if (std::iswalnum(static_cast<wint_t>(ch)))
            {
                filtered.push_back(ch);
            }
        }

        return filtered;
    }

    std::wstring PickRandomHotspotToken(const std::wstring& sourceSsid)
    {
        std::wstring characterPool = ExtractRandomizableSsidCharacters(sourceSsid);
        if (characterPool.empty())
        {
            characterPool = L"wifi";
        }

        auto& generator = GetRandomGenerator();
        std::vector<wchar_t> shuffledPool(characterPool.begin(), characterPool.end());
        std::shuffle(shuffledPool.begin(), shuffledPool.end(), generator);

        std::wstring token;
        token.reserve(4);
        const size_t directCount = std::min<size_t>(4, shuffledPool.size());
        for (size_t i = 0; i < directCount; ++i)
        {
            token.push_back(shuffledPool[i]);
        }

        if (!shuffledPool.empty())
        {
            std::uniform_int_distribution<size_t> pickIndex(0, shuffledPool.size() - 1);
            while (token.size() < 4)
            {
                token.push_back(shuffledPool[pickIndex(generator)]);
            }
        }

        std::shuffle(token.begin(), token.end(), generator);
        std::bernoulli_distribution uppercaseSwitch(0.5);
        for (wchar_t& ch : token)
        {
            if (std::iswalpha(static_cast<wint_t>(ch)))
            {
                ch = uppercaseSwitch(generator)
                    ? std::towupper(static_cast<wint_t>(ch))
                    : std::towlower(static_cast<wint_t>(ch));
            }
        }

        return token;
    }

    std::wstring BuildCarrierStyleHotspotAlias(const std::wstring& sourceSsid)
    {
        static const std::vector<std::wstring> prefixes =
        {
            L"CMCC-",
            L"ChinaNet-",
            L"ChinaUnicom-",
            L"CUNET_",
            L"TP-LINK_",
            L"MERCURY_",
            L"H3C_",
            L"Tenda_"
        };

        auto& generator = GetRandomGenerator();
        std::uniform_int_distribution<size_t> pickPrefix(0, prefixes.size() - 1);
        return prefixes[pickPrefix(generator)] + PickRandomHotspotToken(sourceSsid);
    }

    NetworkOperatorTetheringManager CreateTetheringManager()
    {
        const ConnectionProfile profile = NetworkInformation::GetInternetConnectionProfile();
        if (!profile)
        {
            throw std::runtime_error("No internet connection profile was found.");
        }

        const auto capability =
            NetworkOperatorTetheringManager::GetTetheringCapabilityFromConnectionProfile(profile);
        if (capability != TetheringCapability::Enabled)
        {
            throw std::runtime_error("Tethering capability is not enabled.");
        }

        return NetworkOperatorTetheringManager::CreateFromConnectionProfile(profile);
    }

    std::wstring GetUsablePassphrase(const NetworkOperatorTetheringManager& manager)
    {
        std::wstring passphrase = ToWString(manager.GetCurrentAccessPointConfiguration().Passphrase());
        if (passphrase.size() < 8)
        {
            passphrase = L"12345678";
        }

        return passphrase;
    }
}

bool SolockController::ShouldSkipHotspotActions() const
{
#ifdef _DEBUG
    return m_options.debugSkipHotspotActions;
#else
    return false;
#endif
}

bool SolockController::EnsurePreActionHotspot()
{
    if (ShouldSkipHotspotActions())
    {
        DebugLog(L"[HOTSPOT] debug skip enabled; pre-action hotspot step bypassed.");
        return true;
    }

    try
    {
        auto manager = CreateTetheringManager();
        const auto config = manager.GetCurrentAccessPointConfiguration();
        const std::wstring currentSsid = ToWString(config.Ssid());
        DebugLog(L"[HOTSPOT] EnsurePreActionHotspot current SSID: " +
            (currentSsid.empty() ? std::wstring(L"<empty>") : currentSsid));

        std::wstring originalSsid;
        const bool hasOriginalSsid = TryLoadOriginalSsid(originalSsid) && !originalSsid.empty();
        if (hasOriginalSsid && !EqualsIgnoreCase(originalSsid, currentSsid))
        {
            DebugLog(L"[HOTSPOT] restoring original SSID for pre-action phase: " + originalSsid);
            const bool restored = EnsureHotspotOnWithSsid(originalSsid);
            if (restored)
            {
                ClearOriginalSsid();
            }

            return restored;
        }

        if (hasOriginalSsid && EqualsIgnoreCase(originalSsid, currentSsid))
        {
            ClearOriginalSsid();
        }

        DebugLog(L"[HOTSPOT] using current hotspot configuration for pre-action phase.");
        return EnsureHotspotOnWithCurrentConfig();
    }
    catch (const std::exception& ex)
    {
        DebugLog(L"[HOTSPOT] EnsurePreActionHotspot failed: " + ToWString(std::string(ex.what())));
        return false;
    }
    catch (...)
    {
        DebugLog(L"[HOTSPOT] EnsurePreActionHotspot failed with an unknown exception.");
        return false;
    }
}

bool SolockController::EnsureHotspotOnWithCurrentConfig()
{
    if (ShouldSkipHotspotActions())
    {
        DebugLog(L"[HOTSPOT] debug skip enabled; hotspot start with current config bypassed.");
        return true;
    }

    try
    {
        auto manager = CreateTetheringManager();

        if (IsTetheringOn(manager))
        {
            DebugLog(L"[HOTSPOT] hotspot already enabled with current configuration.");
            return true;
        }

        const auto result = manager.StartTetheringAsync().get();
        const int status = static_cast<int>(result.Status());
        DebugLog(L"[HOTSPOT] StartTetheringAsync(current config) status=" + std::to_wstring(status));
        return status == 0 || status == 9;
    }
    catch (const std::exception& ex)
    {
        DebugLog(L"[HOTSPOT] EnsureHotspotOnWithCurrentConfig failed: " + ToWString(std::string(ex.what())));
        return false;
    }
    catch (...)
    {
        DebugLog(L"[HOTSPOT] EnsureHotspotOnWithCurrentConfig failed with an unknown exception.");
        return false;
    }
}

bool SolockController::EnsureHotspotOnWithSsid(const std::wstring& desiredSsid)
{
    if (ShouldSkipHotspotActions())
    {
        DebugLog(L"[HOTSPOT] debug skip enabled; hotspot SSID switch bypassed: " + desiredSsid);
        return true;
    }

    try
    {
        auto manager = CreateTetheringManager();
        const auto currentConfig = manager.GetCurrentAccessPointConfiguration();
        const std::wstring currentSsid = ToWString(currentConfig.Ssid());
        const bool hotspotOn = IsTetheringOn(manager);
        DebugLog(L"[HOTSPOT] EnsureHotspotOnWithSsid current SSID=" +
            (currentSsid.empty() ? std::wstring(L"<empty>") : currentSsid) +
            L", desired SSID=" + desiredSsid);

        if (hotspotOn && currentSsid == desiredSsid)
        {
            DebugLog(L"[HOTSPOT] hotspot already enabled with the desired SSID.");
            return true;
        }

        if (currentSsid != desiredSsid)
        {
            NetworkOperatorTetheringAccessPointConfiguration config;
            config.Ssid(desiredSsid);
            config.Passphrase(GetUsablePassphrase(manager));

            manager.ConfigureAccessPointAsync(config).get();
            DebugLog(L"[HOTSPOT] hotspot access point reconfigured.");
        }

        const auto result = manager.StartTetheringAsync().get();
        const int status = static_cast<int>(result.Status());
        DebugLog(L"[HOTSPOT] StartTetheringAsync(desired SSID) status=" + std::to_wstring(status));
        return status == 0 || status == 9;
    }
    catch (const std::exception& ex)
    {
        DebugLog(L"[HOTSPOT] EnsureHotspotOnWithSsid failed: " + ToWString(std::string(ex.what())));
        return false;
    }
    catch (...)
    {
        DebugLog(L"[HOTSPOT] EnsureHotspotOnWithSsid failed with an unknown exception.");
        return false;
    }
}

void SolockController::ResetEveningHotspotAlias()
{
    m_eveningHotspotAliasSource.clear();
    m_eveningHotspotAlias.clear();
}

std::wstring SolockController::GetEveningHotspotAlias(const std::wstring& sourceSsid)
{
    const std::wstring effectiveSource = sourceSsid.empty() ? m_options.postActionSsid : sourceSsid;
    if (m_eveningHotspotAlias.empty() || !EqualsIgnoreCase(m_eveningHotspotAliasSource, effectiveSource))
    {
        m_eveningHotspotAliasSource = effectiveSource;
        m_eveningHotspotAlias = BuildRandomizedHotspotAlias(effectiveSource);
        DebugLog(L"[HOTSPOT] generated evening alias SSID from " +
            (effectiveSource.empty() ? std::wstring(L"<empty>") : effectiveSource) +
            L" to " + m_eveningHotspotAlias);
    }

    return m_eveningHotspotAlias;
}

std::wstring SolockController::BuildRandomizedHotspotAlias(const std::wstring& sourceSsid)
{
    return BuildCarrierStyleHotspotAlias(sourceSsid);
}

bool SolockController::EnsureEveningHotspotState()
{
    if (ShouldSkipHotspotActions())
    {
        DebugLog(L"[HOTSPOT] debug skip enabled; evening hotspot-only step bypassed.");
        return true;
    }

    bool hotspotOk = false;
    std::wstring desiredSsid;
    try
    {
        auto manager = CreateTetheringManager();
        const auto currentConfig = manager.GetCurrentAccessPointConfiguration();
        const std::wstring currentSsid = ToWString(currentConfig.Ssid());
        std::wstring originalSsid;
        const bool hasOriginalSsid = TryLoadOriginalSsid(originalSsid) && !originalSsid.empty();

        if (!currentSsid.empty() && !hasOriginalSsid)
        {
            SaveOriginalSsid(currentSsid);
            originalSsid = currentSsid;
        }

        const std::wstring aliasSource =
            !originalSsid.empty()
                ? originalSsid
                : (!currentSsid.empty() ? currentSsid : m_options.postActionSsid);
        desiredSsid = GetEveningHotspotAlias(aliasSource);

        DebugLog(L"[HOTSPOT] evening alias source SSID=" +
            (aliasSource.empty() ? std::wstring(L"<empty>") : aliasSource) +
            L", desired SSID=" + desiredSsid);
    }
    catch (const std::exception& ex)
    {
        DebugLog(L"[HOTSPOT] EnsureEveningPostActionState snapshot failed: " + ToWString(std::string(ex.what())));
    }
    catch (...)
    {
        DebugLog(L"[HOTSPOT] EnsureEveningPostActionState snapshot failed with an unknown exception.");
    }

    if (desiredSsid.empty())
    {
        desiredSsid = GetEveningHotspotAlias(m_options.postActionSsid);
    }

    hotspotOk = EnsureHotspotOnWithSsid(desiredSsid);
    return hotspotOk;
}
