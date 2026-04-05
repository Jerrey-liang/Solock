#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>

#include <chrono>
#include <string>
#include <vector>

struct IMMDeviceEnumerator;
struct IMMNotificationClient;

class SolockController
{
public:
    struct Options
    {
        int startupStableSeconds = 8;
        int startupMaxWaitSeconds = 45;
        std::vector<int> scheduledBlockStartMinutesOfDay =
        {
            8 * 60 + 40,
            9 * 60 + 30,
            10 * 60 + 20,
            11 * 60 + 10,
            15 * 60 + 10,
            16 * 60 + 0,
            16 * 60 + 50
        };
        int scheduledBlockDurationMinutes = 10;
        int middayShutdownStartHour = 12;
        int middayShutdownStartMinute = 10;
        int middayShutdownEndHour = 12;
        int middayShutdownEndMinute = 50;
        int eveningPostActionStartHour = 17;
        int eveningPostActionStartMinute = 40;
        int eveningIdleShutdownStartHour = 17;
        int eveningIdleShutdownStartMinute = 50;
        int inactivityThresholdMinutes = 10;
        int heartbeatSeconds = 15;
        float normalVolumePercent = 60.0f;
        float reducedVolumePercent = 35.0f;
        std::wstring postActionSsid = L"CMCC-24dF";
        std::vector<std::wstring> blockedProcessNames =
        {
            L"SeewoHugoLauncher.exe",
            L"SeewoServiceAssistant.exe"
        };

        bool autoRegisterScheduledTask = true;
        std::wstring scheduledTaskName = L"Solock AutoStart";
        bool debugForceIdleState = true;
        bool debugSkipDestructiveActions = true;
        bool debugSkipHotspotActions = true;
        int debugStepDelayMilliseconds = 250;
    };

    explicit SolockController(const Options& options = Options());
    ~SolockController();

    int Run();
    int RunAllFeaturesAcceleratedDebug();
    int RunBlockedAppNetworkingDebug();

private:
    struct CustomBlockWindow
    {
        bool hasStart = false;
        int startMinutesOfDay = 0;
        bool hasCustomBlockDurationMinutes = false;
        int customBlockDurationMinutes = 0;
        bool hasCustomBlockIntervalMinutes = false;
        int customBlockIntervalMinutes = 0;
        bool hasCustomBlockRepeatCount = false;
        int customBlockRepeatCount = 0;
        std::wstring signature;
    };

    struct ScheduleTimes
    {
        std::chrono::system_clock::time_point middayShutdownStartTime;
        std::chrono::system_clock::time_point middayShutdownEndTime;
        std::chrono::system_clock::time_point eveningPostActionStartTime;
        std::chrono::system_clock::time_point eveningIdleShutdownStartTime;
    };

    struct InstalledBlockedAppFilter
    {
        std::wstring appPath;
        UINT64 ipv4FilterId;
        UINT64 ipv6FilterId;
    };

    struct RunningBlockedProcess
    {
        DWORD processId;
        std::wstring appPath;
    };

    struct ExternalOverrides
    {
        std::vector<CustomBlockWindow> customBlocks;
        std::wstring customBlockSignature;
        bool hasMiddayShutdownStartMinutesOfDay = false;
        int middayShutdownStartMinutesOfDay = 0;
        bool hasMiddayShutdownEndMinutesOfDay = false;
        int middayShutdownEndMinutesOfDay = 0;
        bool hasEveningHotspotEnabled = false;
        bool eveningHotspotEnabled = true;
        bool hasEveningHotspotStartMinutesOfDay = false;
        int eveningHotspotStartMinutesOfDay = 0;
        bool hasEveningIdleShutdownStartMinutesOfDay = false;
        int eveningIdleShutdownStartMinutesOfDay = 0;
        bool hasNormalVolumePercent = false;
        float normalVolumePercent = 0.0f;
        bool hasReducedVolumePercent = false;
        float reducedVolumePercent = 0.0f;
    };

    struct CustomBlockRuntimeState
    {
        bool activated = false;
        std::chrono::system_clock::time_point activationTime;
    };

    enum class Phase
    {
        ScheduledBlocks,
        MiddayIdleShutdown,
        EveningIdleShutdown,
        EveningPostAction,
    };

    Options m_options;
    bool m_eveningIdleLockApplied;
    std::wstring m_eveningHotspotAliasSource;
    std::wstring m_eveningHotspotAlias;
    std::vector<CustomBlockRuntimeState> m_customBlockStates;
    std::wstring m_customBlockConfigSignature;
    HANDLE m_wfpEngine;
    bool m_blockedAppFiltersInstalled;
    std::vector<InstalledBlockedAppFilter> m_installedBlockedAppFilters;
    HANDLE m_audioDeviceChangeEvent;
    IMMDeviceEnumerator* m_audioDeviceEnumerator;
    IMMNotificationClient* m_audioNotificationClient;

    // Core schedule flow.
    static std::chrono::system_clock::time_point LocalAtOnSameDay(
        const std::chrono::system_clock::time_point& referenceNow,
        int hour,
        int minute,
        int second = 0);
    ScheduleTimes GetScheduleTimesFor(
        const std::chrono::system_clock::time_point& now,
        const ExternalOverrides& overrides) const;
    static Phase GetPhaseAt(
        const std::chrono::system_clock::time_point& now,
        const ScheduleTimes& scheduleTimes,
        const ExternalOverrides& overrides);
    static bool IsEveningHotspotEnabled(const ExternalOverrides& overrides);

    int RunWithSchedule();
    bool WaitForSystemAndNetworkStability();
    bool IsNetworkUsableNow() const;
    bool IsInputIdleForAtLeast(std::chrono::milliseconds idleThreshold) const;

    // Audio state.
    bool AssertKeepSystemAwake();
    void ClearKeepSystemAwake();
    float GetDesiredVolumePercentForPhase(Phase phase) const;
    bool ShouldMuteAudioForPhase(Phase phase) const;
    bool IsCurrentSessionUnlockedOnDesktop() const;
    bool InitializeAudioVolumeMonitoring();
    void ShutdownAudioVolumeMonitoring();
    bool EnsureAudioVolumeMatchesPhase(Phase phase) const;
    void WaitForHeartbeatOrAudioEvent(int heartbeatSeconds) const;

    // Hotspot management.
    bool EnsurePreActionHotspot();
    bool EnsureHotspotOnWithCurrentConfig();
    bool EnsureHotspotOnWithSsid(const std::wstring& desiredSsid);
    void ResetEveningHotspotAlias();
    std::wstring GetEveningHotspotAlias(const std::wstring& sourceSsid);
    static std::wstring BuildRandomizedHotspotAlias(const std::wstring& sourceSsid);
    bool EnsureEveningHotspotState();
    bool ShouldSkipHotspotActions() const;

    // App networking block.
    bool EnsureTargetAppsNetworkingBlocked();
    bool EnsureTargetAppsNetworkingEnabled();
    std::vector<RunningBlockedProcess> ResolveRunningBlockedProcesses() const;
    bool EnsureTargetAppsNetworkingMatchesSchedule(const std::chrono::system_clock::time_point& now);
    bool ShouldBlockTargetAppsAt(const std::chrono::system_clock::time_point& now);
    bool IsCustomBlockActiveAt(
        const std::chrono::system_clock::time_point& now,
        const ExternalOverrides& overrides);
    void CloseWfpEngine();
    bool EnsureEveningPostActionState();
    bool ApplyEveningIdleLockIfNeeded();
    void DebugLogRunningBlockedProcesses(
        const wchar_t* stage,
        const std::vector<RunningBlockedProcess>& runningProcesses) const;

    // Session and power actions.
    bool LockCurrentSession();
    bool TurnOffDisplay();
    bool ShutdownMachineNow();

    // Local state and external overrides.
    static std::wstring GetStateDirectoryPath();
    static std::wstring GetLegacyOriginalSsidStateFilePath();
    static std::wstring GetConfigFilePath();
    static bool EnsureStateDirectoryExists();
    static bool ClearOriginalSsid();
    static bool SaveOriginalSsid(const std::wstring& ssid);
    static bool TryLoadOriginalSsid(std::wstring& ssid);
    static ExternalOverrides LoadExternalOverrides();

    // Startup task registration.
    static std::wstring GetCurrentExePath();
    static std::wstring GetCurrentExeDirectory();
    static std::wstring GetCurrentUserTaskUserId();
    static bool EnsureStartupTaskRegistered(const std::wstring& taskName);
};
