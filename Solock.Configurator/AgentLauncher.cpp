#include "pch.h"
#include "AgentLauncher.h"

#include <TlHelp32.h>

#include <vector>

namespace
{
    struct RunningAgentProcess
    {
        DWORD processId = 0;
        std::wstring processName;
        std::wstring imagePath;
    };

    constexpr const wchar_t* kAgentCandidateNames[] =
    {
        L"Solock.Agent.exe"
    };
    constexpr wchar_t kSolutionFileName[] = L"Solock.sln";
    constexpr wchar_t kPreferredPlatformDirectory[] = L"x64";
    constexpr wchar_t kPreferredConfigurationDirectory[] = L"Release";

    std::wstring BuildChildPath(const std::wstring& dir, const wchar_t* fileName)
    {
        if (dir.empty() || fileName == nullptr || *fileName == L'\0')
        {
            return L"";
        }

        return dir + L"\\" + fileName;
    }

    bool IsCandidateProcessName(const std::wstring& processName)
    {
        for (const auto* candidateName : kAgentCandidateNames)
        {
            if (_wcsicmp(processName.c_str(), candidateName) == 0)
            {
                return true;
            }
        }

        return false;
    }

    std::wstring TryQueryProcessImagePath(const DWORD processId)
    {
        std::wstring result;
        HANDLE processHandle = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
        if (processHandle == nullptr)
        {
            return result;
        }

        std::wstring buffer(32768, L'\0');
        DWORD size = static_cast<DWORD>(buffer.size());
        if (::QueryFullProcessImageNameW(processHandle, 0, buffer.data(), &size) != FALSE)
        {
            buffer.resize(size);
            result = std::move(buffer);
        }

        ::CloseHandle(processHandle);
        return result;
    }

    std::wstring FindAncestorContainingFile(std::wstring current, const wchar_t* childName)
    {
        for (int depth = 0; depth < 8 && !current.empty(); ++depth)
        {
            if (GetFileAttributesW(BuildChildPath(current, childName).c_str()) != INVALID_FILE_ATTRIBUTES)
            {
                return current;
            }

            const size_t separator = current.find_last_of(L"\\/");
            if (separator == std::wstring::npos)
            {
                break;
            }

            current = current.substr(0, separator);
        }

        return L"";
    }

    bool EnumerateRunningAgentProcesses(std::vector<RunningAgentProcess>& matches)
    {
        matches.clear();

        HANDLE snapshotHandle = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshotHandle == INVALID_HANDLE_VALUE)
        {
            return false;
        }

        PROCESSENTRY32W entry = {};
        entry.dwSize = sizeof(entry);
        if (::Process32FirstW(snapshotHandle, &entry) == FALSE)
        {
            ::CloseHandle(snapshotHandle);
            return false;
        }

        do
        {
            const std::wstring processName = entry.szExeFile;
            if (!IsCandidateProcessName(processName))
            {
                continue;
            }

            RunningAgentProcess match;
            match.processId = entry.th32ProcessID;
            match.processName = processName;
            match.imagePath = TryQueryProcessImagePath(entry.th32ProcessID);
            matches.push_back(std::move(match));
        } while (::Process32NextW(snapshotHandle, &entry) != FALSE);

        ::CloseHandle(snapshotHandle);
        return true;
    }
}

namespace solock_configurator
{
    std::wstring AgentLauncher::GetCurrentExeDirectory()
    {
        std::wstring buffer(32768, L'\0');
        const DWORD length = ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        buffer.resize(length);
        return GetParentDirectory(buffer);
    }

    std::wstring AgentLauncher::GetParentDirectory(const std::wstring& path)
    {
        const size_t pos = path.find_last_of(L"\\/");
        if (pos == std::wstring::npos)
        {
            return L"";
        }

        return path.substr(0, pos);
    }

    bool AgentLauncher::FileExists(const std::wstring& path)
    {
        if (path.empty())
        {
            return false;
        }

        const DWORD attributes = ::GetFileAttributesW(path.c_str());
        return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
    }

    std::wstring AgentLauncher::FindAgentPath() const
    {
        const std::wstring currentExeDirectory = GetCurrentExeDirectory();
        const std::wstring solutionRoot = FindAncestorContainingFile(currentExeDirectory, kSolutionFileName);
        if (!solutionRoot.empty())
        {
            const std::wstring preferredDirectory = BuildChildPath(
                BuildChildPath(solutionRoot, kPreferredPlatformDirectory),
                kPreferredConfigurationDirectory);

            for (const auto* candidateName : kAgentCandidateNames)
            {
                const std::wstring candidate = BuildChildPath(preferredDirectory, candidateName);
                if (FileExists(candidate))
                {
                    return candidate;
                }
            }

            return L"";
        }

        std::vector<std::wstring> searchDirectories;
        std::wstring current = currentExeDirectory;
        for (int depth = 0; depth < 5 && !current.empty(); ++depth)
        {
            searchDirectories.push_back(current);
            current = GetParentDirectory(current);
        }

        for (const auto& dir : searchDirectories)
        {
            for (const auto* candidateName : kAgentCandidateNames)
            {
                const std::wstring candidate = BuildChildPath(dir, candidateName);
                if (FileExists(candidate))
                {
                    return candidate;
                }
            }
        }

        return L"";
    }

    bool AgentLauncher::TryGetInstalledAgentPath(std::wstring& agentPath) const
    {
        agentPath = FindAgentPath();
        return !agentPath.empty();
    }

    bool AgentLauncher::TryGetRunningAgentPath(std::wstring& runningPath) const
    {
        runningPath.clear();

        std::vector<RunningAgentProcess> matches;
        if (!EnumerateRunningAgentProcesses(matches) || matches.empty())
        {
            return false;
        }

        const RunningAgentProcess& match = matches.front();
        runningPath = match.imagePath.empty() ? match.processName : match.imagePath;
        return true;
    }

    bool AgentLauncher::KillRunningAgents(std::wstring& result, std::wstring& error) const
    {
        result.clear();
        error.clear();

        std::vector<RunningAgentProcess> matches;
        if (!EnumerateRunningAgentProcesses(matches))
        {
            error = L"Unable to enumerate Solock.Agent.exe processes. Win32 error: " + std::to_wstring(::GetLastError());
            return false;
        }

        if (matches.empty())
        {
            error = L"Solock.Agent.exe is not running.";
            return false;
        }

        size_t terminatedCount = 0;
        std::wstring firstDisplayName;
        for (const auto& match : matches)
        {
            HANDLE processHandle = ::OpenProcess(
                PROCESS_TERMINATE | SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
                FALSE,
                match.processId);
            if (processHandle == nullptr)
            {
                error = L"Unable to open Solock.Agent.exe for termination. Win32 error: " + std::to_wstring(::GetLastError());
                return false;
            }

            const std::wstring displayName = match.imagePath.empty() ? match.processName : match.imagePath;
            if (firstDisplayName.empty())
            {
                firstDisplayName = displayName;
            }

            if (::TerminateProcess(processHandle, 0) == FALSE)
            {
                error = L"Unable to kill Solock.Agent.exe. Win32 error: " + std::to_wstring(::GetLastError());
                ::CloseHandle(processHandle);
                return false;
            }

            const DWORD waitResult = ::WaitForSingleObject(processHandle, 5000);
            ::CloseHandle(processHandle);
            if (waitResult == WAIT_TIMEOUT)
            {
                error = L"Timed out while waiting for Solock.Agent.exe to exit.";
                return false;
            }

            if (waitResult == WAIT_FAILED)
            {
                error = L"Unable to confirm Solock.Agent.exe exit. Win32 error: " + std::to_wstring(::GetLastError());
                return false;
            }

            ++terminatedCount;
        }

        if (terminatedCount == 1)
        {
            result = L"Agent killed: " + firstDisplayName;
        }
        else
        {
            result = L"Killed " + std::to_wstring(terminatedCount) + L" Solock.Agent.exe processes.";
        }

        return true;
    }

    bool AgentLauncher::Launch(std::wstring& launchedPath, std::wstring& error) const
    {
        launchedPath.clear();
        error.clear();

        const std::wstring agentPath = FindAgentPath();
        if (agentPath.empty())
        {
            error = L"Unable to find x64\\Release\\Solock.Agent.exe. Build the Release agent first.";
            return false;
        }

        std::wstring commandLine = L"\"" + agentPath + L"\"";
        const std::wstring workingDirectory = GetParentDirectory(agentPath);

        STARTUPINFOW startupInfo = {};
        startupInfo.cb = sizeof(startupInfo);
        PROCESS_INFORMATION processInfo = {};

        const BOOL ok = ::CreateProcessW(
            nullptr,
            commandLine.data(),
            nullptr,
            nullptr,
            FALSE,
            CREATE_NO_WINDOW,
            nullptr,
            workingDirectory.empty() ? nullptr : workingDirectory.c_str(),
            &startupInfo,
            &processInfo);
        if (!ok)
        {
            error = L"Unable to start Solock.Agent.exe. Win32 error: " + std::to_wstring(::GetLastError());
            return false;
        }

        ::CloseHandle(processInfo.hThread);
        ::CloseHandle(processInfo.hProcess);

        launchedPath = agentPath;
        return true;
    }
}
