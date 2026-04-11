#pragma once

#include <string>
#include <vector>

namespace solock_configurator
{
    struct CustomBlockEntry
    {
        std::wstring start;
        std::wstring durationMinutes;
        std::wstring intervalMinutes;
        std::wstring repeatCount;
    };

    struct ConfigSnapshot
    {
        std::wstring configFilePath;
        std::wstring originalHotspotSsid;
        std::wstring enableEveningHotspot;
        std::wstring middayShutdownStart;
        std::wstring middayShutdownEnd;
        std::wstring eveningHotspotStart;
        std::wstring eveningShutdownStart;
        std::wstring normalPercent;
        std::wstring reducedPercent;
        std::vector<CustomBlockEntry> customBlocks;
    };

    class ConfigRepository
    {
    public:
        static std::wstring GetStateDirectoryPath();
        static std::wstring GetConfigFilePath();

        bool Load(ConfigSnapshot& snapshot, std::wstring& error) const;
        bool Save(const ConfigSnapshot& snapshot, std::wstring& error) const;
    };
}
