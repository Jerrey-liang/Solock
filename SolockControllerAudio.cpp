#define _WIN32_DCOM
#include "SolockControllerInternal.h"

#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <WtsApi32.h>

#include <algorithm>
#include <cmath>
#include <new>

#pragma comment(lib, "wtsapi32.lib")

namespace
{
    using solock::internal::EqualsIgnoreCase;

    class AudioOutputNotificationClient final : public IMMNotificationClient
    {
    public:
        explicit AudioOutputNotificationClient(HANDLE deviceChangeEvent)
            : m_referenceCount(1),
              m_deviceChangeEvent(deviceChangeEvent)
        {
        }

        ULONG STDMETHODCALLTYPE AddRef() override
        {
            return static_cast<ULONG>(::InterlockedIncrement(&m_referenceCount));
        }

        ULONG STDMETHODCALLTYPE Release() override
        {
            const ULONG count = static_cast<ULONG>(::InterlockedDecrement(&m_referenceCount));
            if (count == 0)
            {
                delete this;
            }

            return count;
        }

        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, VOID** object) override
        {
            if (object == nullptr)
            {
                return E_POINTER;
            }

            if (riid == IID_IUnknown || riid == __uuidof(IMMNotificationClient))
            {
                *object = static_cast<IMMNotificationClient*>(this);
                AddRef();
                return S_OK;
            }

            *object = nullptr;
            return E_NOINTERFACE;
        }

        HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override
        {
            SignalDeviceChange();
            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override
        {
            SignalDeviceChange();
            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override
        {
            SignalDeviceChange();
            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole, LPCWSTR) override
        {
            if (flow == eRender)
            {
                SignalDeviceChange();
            }

            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override
        {
            return S_OK;
        }

    private:
        void SignalDeviceChange() const
        {
            if (m_deviceChangeEvent != nullptr)
            {
                ::SetEvent(m_deviceChangeEvent);
            }
        }

        LONG m_referenceCount;
        HANDLE m_deviceChangeEvent;
    };

    bool TryGetCurrentSessionLockedState(bool& locked)
    {
        locked = false;

        LPWSTR rawInfo = nullptr;
        DWORD bytes = 0;
        if (!::WTSQuerySessionInformationW(
            WTS_CURRENT_SERVER_HANDLE,
            WTS_CURRENT_SESSION,
            WTSSessionInfoEx,
            &rawInfo,
            &bytes) ||
            rawInfo == nullptr ||
            bytes < sizeof(WTSINFOEXW))
        {
            if (rawInfo != nullptr)
            {
                ::WTSFreeMemory(rawInfo);
            }
            return false;
        }

        const WTSINFOEXW* info = reinterpret_cast<const WTSINFOEXW*>(rawInfo);
        bool ok = false;
        if (info->Level == 1)
        {
            const LONG sessionFlags = info->Data.WTSInfoExLevel1.SessionFlags;
            if (sessionFlags == WTS_SESSIONSTATE_LOCK)
            {
                locked = true;
                ok = true;
            }
            else if (sessionFlags == WTS_SESSIONSTATE_UNLOCK)
            {
                locked = false;
                ok = true;
            }
        }

        ::WTSFreeMemory(rawInfo);
        return ok;
    }

    bool TryGetCurrentInputDesktopName(std::wstring& desktopName)
    {
        desktopName.clear();

        HDESK desktop = ::OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
        if (desktop == nullptr)
        {
            return false;
        }

        DWORD requiredBytes = 0;
        ::GetUserObjectInformationW(desktop, UOI_NAME, nullptr, 0, &requiredBytes);
        if (requiredBytes < sizeof(wchar_t))
        {
            ::CloseDesktop(desktop);
            return false;
        }

        std::wstring buffer(requiredBytes / sizeof(wchar_t), L'\0');
        const BOOL ok = ::GetUserObjectInformationW(
            desktop,
            UOI_NAME,
            buffer.data(),
            requiredBytes,
            &requiredBytes);
        ::CloseDesktop(desktop);
        if (!ok || requiredBytes < sizeof(wchar_t))
        {
            return false;
        }

        const size_t length = (requiredBytes / sizeof(wchar_t)) - 1;
        buffer.resize(length);
        desktopName = std::move(buffer);
        return !desktopName.empty();
    }
}

float SolockController::GetDesiredVolumePercentForPhase(const Phase phase) const
{
    const ExternalOverrides overrides = LoadExternalOverrides();
    if (phase == Phase::MiddayIdleShutdown || phase == Phase::EveningPostAction)
    {
        const float configuredVolume = overrides.hasReducedVolumePercent
            ? overrides.reducedVolumePercent
            : m_options.reducedVolumePercent;
        return std::clamp(configuredVolume, 0.0f, 100.0f);
    }

    const float configuredVolume = overrides.hasNormalVolumePercent
        ? overrides.normalVolumePercent
        : m_options.normalVolumePercent;
    return std::clamp(configuredVolume, 0.0f, 100.0f);
}

bool SolockController::ShouldMuteAudioForPhase(const Phase phase) const
{
    return phase == Phase::EveningPostAction && !IsCurrentSessionUnlockedOnDesktop();
}

bool SolockController::IsCurrentSessionUnlockedOnDesktop() const
{
    bool locked = false;
    if (TryGetCurrentSessionLockedState(locked) && locked)
    {
        return false;
    }

    std::wstring desktopName;
    if (TryGetCurrentInputDesktopName(desktopName))
    {
        return EqualsIgnoreCase(desktopName, L"Default");
    }

    return !locked;
}

bool SolockController::InitializeAudioVolumeMonitoring()
{
    if (m_audioDeviceChangeEvent != nullptr &&
        m_audioDeviceEnumerator != nullptr &&
        m_audioNotificationClient != nullptr)
    {
        return true;
    }

    HANDLE deviceChangeEvent = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (deviceChangeEvent == nullptr)
    {
        return false;
    }

    IMMDeviceEnumerator* enumerator = nullptr;
    const HRESULT createStatus = ::CoCreateInstance(
        __uuidof(MMDeviceEnumerator),
        nullptr,
        CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator),
        reinterpret_cast<void**>(&enumerator));
    if (FAILED(createStatus) || enumerator == nullptr)
    {
        ::CloseHandle(deviceChangeEvent);
        return false;
    }

    IMMNotificationClient* notificationClient = new (std::nothrow) AudioOutputNotificationClient(deviceChangeEvent);
    if (notificationClient == nullptr)
    {
        enumerator->Release();
        ::CloseHandle(deviceChangeEvent);
        return false;
    }

    const HRESULT registerStatus = enumerator->RegisterEndpointNotificationCallback(notificationClient);
    if (FAILED(registerStatus))
    {
        notificationClient->Release();
        enumerator->Release();
        ::CloseHandle(deviceChangeEvent);
        return false;
    }

    m_audioDeviceChangeEvent = deviceChangeEvent;
    m_audioDeviceEnumerator = enumerator;
    m_audioNotificationClient = notificationClient;
    return true;
}

void SolockController::ShutdownAudioVolumeMonitoring()
{
    if (m_audioDeviceEnumerator != nullptr && m_audioNotificationClient != nullptr)
    {
        m_audioDeviceEnumerator->UnregisterEndpointNotificationCallback(m_audioNotificationClient);
    }

    if (m_audioNotificationClient != nullptr)
    {
        m_audioNotificationClient->Release();
        m_audioNotificationClient = nullptr;
    }

    if (m_audioDeviceEnumerator != nullptr)
    {
        m_audioDeviceEnumerator->Release();
        m_audioDeviceEnumerator = nullptr;
    }

    if (m_audioDeviceChangeEvent != nullptr)
    {
        ::CloseHandle(m_audioDeviceChangeEvent);
        m_audioDeviceChangeEvent = nullptr;
    }
}

bool SolockController::EnsureAudioVolumeMatchesPhase(const Phase phase) const
{
    IMMDeviceEnumerator* enumerator = m_audioDeviceEnumerator;
    IMMDeviceEnumerator* localEnumerator = nullptr;
    if (enumerator == nullptr)
    {
        const HRESULT createStatus = ::CoCreateInstance(
            __uuidof(MMDeviceEnumerator),
            nullptr,
            CLSCTX_ALL,
            __uuidof(IMMDeviceEnumerator),
            reinterpret_cast<void**>(&localEnumerator));
        if (FAILED(createStatus) || localEnumerator == nullptr)
        {
            return false;
        }

        enumerator = localEnumerator;
    }

    IMMDevice* device = nullptr;
    const HRESULT deviceStatus = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    if (deviceStatus == E_NOTFOUND || device == nullptr)
    {
        if (device != nullptr)
        {
            device->Release();
        }
        if (localEnumerator != nullptr)
        {
            localEnumerator->Release();
        }
        return true;
    }

    if (FAILED(deviceStatus))
    {
        if (localEnumerator != nullptr)
        {
            localEnumerator->Release();
        }
        return false;
    }

    IAudioEndpointVolume* endpointVolume = nullptr;
    const HRESULT activateStatus = device->Activate(
        __uuidof(IAudioEndpointVolume),
        CLSCTX_ALL,
        nullptr,
        reinterpret_cast<void**>(&endpointVolume));
    device->Release();
    if (FAILED(activateStatus) || endpointVolume == nullptr)
    {
        if (endpointVolume != nullptr)
        {
            endpointVolume->Release();
        }
        if (localEnumerator != nullptr)
        {
            localEnumerator->Release();
        }
        return false;
    }

    const float desiredScalar = GetDesiredVolumePercentForPhase(phase) / 100.0f;
    const bool shouldMute = ShouldMuteAudioForPhase(phase);
    bool ok = true;

    BOOL currentMute = FALSE;
    if (FAILED(endpointVolume->GetMute(&currentMute)))
    {
        ok = false;
    }
    else if ((currentMute != FALSE) != shouldMute)
    {
        ok = SUCCEEDED(endpointVolume->SetMute(shouldMute ? TRUE : FALSE, nullptr));
    }

    if (ok && !shouldMute)
    {
        float currentScalar = 0.0f;
        const HRESULT getVolumeStatus = endpointVolume->GetMasterVolumeLevelScalar(&currentScalar);
        ok = SUCCEEDED(getVolumeStatus);
        if (ok && std::fabs(currentScalar - desiredScalar) > 0.01f)
        {
            ok = SUCCEEDED(endpointVolume->SetMasterVolumeLevelScalar(desiredScalar, nullptr));
        }
    }

    endpointVolume->Release();
    if (localEnumerator != nullptr)
    {
        localEnumerator->Release();
    }

    return ok;
}

void SolockController::WaitForHeartbeatOrAudioEvent(const int heartbeatSeconds) const
{
    const DWORD waitMilliseconds = static_cast<DWORD>(std::max(1, heartbeatSeconds) * 1000);
    if (m_audioDeviceChangeEvent != nullptr)
    {
        ::WaitForSingleObject(m_audioDeviceChangeEvent, waitMilliseconds);
        return;
    }

    ::Sleep(waitMilliseconds);
}
