#pragma once

#include <string>

namespace solock_configurator
{
    class AgentLauncher
    {
    public:
        bool KillRunningAgents(std::wstring& result, std::wstring& error) const;
        bool Launch(std::wstring& launchedPath, std::wstring& error) const;
        bool TryGetInstalledAgentPath(std::wstring& agentPath) const;
        bool TryGetRunningAgentPath(std::wstring& runningPath) const;

    private:
        static std::wstring GetCurrentExeDirectory();
        static std::wstring GetParentDirectory(const std::wstring& path);
        static bool FileExists(const std::wstring& path);
        std::wstring FindAgentPath() const;
    };
}
