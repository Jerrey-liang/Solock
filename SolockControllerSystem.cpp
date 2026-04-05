#define _WIN32_DCOM
#include "SolockControllerInternal.h"

#include <comdef.h>
#include <taskschd.h>

#include <string>

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "taskschd.lib")
#pragma comment(lib, "comsuppw.lib")

namespace
{
    using solock::internal::DebugLog;
    using solock::internal::DebugLogHResult;
    using solock::internal::SafeRelease;

    bool EnableShutdownPrivilege()
    {
        HANDLE token = nullptr;
        if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token))
        {
            return false;
        }

        TOKEN_PRIVILEGES privileges = {};
        privileges.PrivilegeCount = 1;

        if (!::LookupPrivilegeValueW(nullptr, SE_SHUTDOWN_NAME, &privileges.Privileges[0].Luid))
        {
            ::CloseHandle(token);
            return false;
        }

        privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

        if (!::AdjustTokenPrivileges(token, FALSE, &privileges, sizeof(privileges), nullptr, nullptr))
        {
            ::CloseHandle(token);
            return false;
        }

        const DWORD error = ::GetLastError();
        ::CloseHandle(token);
        return error == ERROR_SUCCESS;
    }

    bool LaunchShutdownExeFallback()
    {
        std::wstring commandLine = L"shutdown.exe /s /f /t 0 /d p:0:0";
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
            nullptr,
            &startupInfo,
            &processInfo);
        if (!ok)
        {
            return false;
        }

        ::CloseHandle(processInfo.hThread);

        const DWORD waitStatus = ::WaitForSingleObject(processInfo.hProcess, 15000);
        DWORD exitCode = STILL_ACTIVE;
        const bool gotExitCode = ::GetExitCodeProcess(processInfo.hProcess, &exitCode) != FALSE;
        ::CloseHandle(processInfo.hProcess);

        if (waitStatus == WAIT_TIMEOUT)
        {
            return true;
        }

        return waitStatus == WAIT_OBJECT_0 && gotExitCode && exitCode == 0;
    }
}

bool SolockController::ShouldSkipDestructiveActions() const
{
#ifdef _DEBUG
    return m_options.debugSkipDestructiveActions;
#else
    return false;
#endif
}

bool SolockController::LockCurrentSession()
{
    if (ShouldSkipDestructiveActions())
    {
        DebugLog(L"[SYSTEM] debug skip enabled; LockWorkStation bypassed.");
        return true;
    }

    return ::LockWorkStation() != FALSE;
}

bool SolockController::TurnOffDisplay()
{
    if (ShouldSkipDestructiveActions())
    {
        DebugLog(L"[SYSTEM] debug skip enabled; display power-off broadcast bypassed.");
        return true;
    }

    ::SendMessageW(
        HWND_BROADCAST,
        WM_SYSCOMMAND,
        static_cast<WPARAM>(SC_MONITORPOWER),
        static_cast<LPARAM>(2));
    return true;
}

bool SolockController::ShutdownMachineNow()
{
    if (ShouldSkipDestructiveActions())
    {
        DebugLog(L"[SYSTEM] debug skip enabled; shutdown path treated as successful.");
        return true;
    }

    for (;;)
    {
        if (EnableShutdownPrivilege() &&
            ::ExitWindowsEx(EWX_POWEROFF | EWX_FORCEIFHUNG, SHTDN_REASON_FLAG_PLANNED))
        {
            return true;
        }

        if (LaunchShutdownExeFallback())
        {
            return true;
        }

        ::Sleep(1000);
    }
}

std::wstring SolockController::GetCurrentExePath()
{
    std::wstring buffer(32768, L'\0');
    const DWORD length = ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    buffer.resize(length);
    return buffer;
}

std::wstring SolockController::GetCurrentExeDirectory()
{
    std::wstring path = GetCurrentExePath();
    const size_t pos = path.find_last_of(L"\\/");
    if (pos == std::wstring::npos)
    {
        return L"";
    }

    return path.substr(0, pos);
}

std::wstring SolockController::GetCurrentUserTaskUserId()
{
    auto getEnvironmentVariable = [](const wchar_t* name) -> std::wstring
    {
        const DWORD needed = ::GetEnvironmentVariableW(name, nullptr, 0);
        if (needed == 0)
        {
            return L"";
        }

        std::wstring value(needed, L'\0');
        const DWORD written = ::GetEnvironmentVariableW(name, value.data(), needed);
        if (written == 0)
        {
            return L"";
        }

        value.resize(written);
        return value;
    };

    const std::wstring domain = getEnvironmentVariable(L"USERDOMAIN");
    const std::wstring user = getEnvironmentVariable(L"USERNAME");

    if (!domain.empty() && !user.empty())
    {
        return domain + L"\\" + user;
    }

    DWORD needed = 0;
    ::GetUserNameW(nullptr, &needed);
    if (needed > 0)
    {
        std::wstring name(needed, L'\0');
        if (::GetUserNameW(name.data(), &needed))
        {
            if (!name.empty() && name.back() == L'\0')
            {
                name.pop_back();
            }

            return name;
        }
    }

    return L"";
}

bool SolockController::EnsureStartupTaskRegistered(const std::wstring& taskName)
{
    if (taskName.empty())
    {
        DebugLog(L"[TASK] registration skipped because taskName is empty.");
        return false;
    }

    const std::wstring exePath = GetCurrentExePath();
    const std::wstring exeDir = GetCurrentExeDirectory();
    const std::wstring userId = GetCurrentUserTaskUserId();

    if (exePath.empty() || userId.empty())
    {
        DebugLog(L"[TASK] registration failed because exePath or userId is empty.");
        return false;
    }

    HRESULT hr = ::CoInitializeSecurity(
        nullptr,
        -1,
        nullptr,
        nullptr,
        RPC_C_AUTHN_LEVEL_PKT_PRIVACY,
        RPC_C_IMP_LEVEL_IMPERSONATE,
        nullptr,
        0,
        nullptr);
    if (FAILED(hr) && hr != RPC_E_TOO_LATE)
    {
        return false;
    }

    ITaskService* service = nullptr;
    ITaskFolder* rootFolder = nullptr;
    ITaskDefinition* task = nullptr;
    IRegistrationInfo* registrationInfo = nullptr;
    IPrincipal* principal = nullptr;
    ITaskSettings* settings = nullptr;
    ITriggerCollection* triggers = nullptr;
    ITrigger* trigger = nullptr;
    ILogonTrigger* logonTrigger = nullptr;
    IActionCollection* actions = nullptr;
    IAction* action = nullptr;
    IExecAction* execAction = nullptr;
    IRegisteredTask* registeredTask = nullptr;

    bool ok = false;
    const wchar_t* failureStep = nullptr;
    HRESULT failureHr = S_OK;

    do
    {
        hr = ::CoCreateInstance(
            CLSID_TaskScheduler,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_ITaskService,
            reinterpret_cast<void**>(&service));
        if (FAILED(hr) || service == nullptr)
        {
            failureStep = L"CoCreateInstance(CLSID_TaskScheduler)";
            failureHr = hr;
            break;
        }

        hr = service->Connect(_variant_t(), _variant_t(), _variant_t(), _variant_t());
        if (FAILED(hr))
        {
            failureStep = L"ITaskService::Connect";
            failureHr = hr;
            break;
        }

        hr = service->GetFolder(_bstr_t(L"\\"), &rootFolder);
        if (FAILED(hr) || rootFolder == nullptr)
        {
            failureStep = L"ITaskService::GetFolder";
            failureHr = hr;
            break;
        }

        hr = service->NewTask(0, &task);
        if (FAILED(hr) || task == nullptr)
        {
            failureStep = L"ITaskService::NewTask";
            failureHr = hr;
            break;
        }

        hr = task->get_RegistrationInfo(&registrationInfo);
        if (FAILED(hr) || registrationInfo == nullptr)
        {
            failureStep = L"ITaskDefinition::get_RegistrationInfo";
            failureHr = hr;
            break;
        }

        registrationInfo->put_Author(_bstr_t(L"Solock"));
        registrationInfo->put_Description(_bstr_t(L"Start Solock automatically when the current user logs on."));

        hr = task->get_Principal(&principal);
        if (FAILED(hr) || principal == nullptr)
        {
            failureStep = L"ITaskDefinition::get_Principal";
            failureHr = hr;
            break;
        }

        principal->put_UserId(_bstr_t(userId.c_str()));
        principal->put_LogonType(TASK_LOGON_INTERACTIVE_TOKEN);
        principal->put_RunLevel(TASK_RUNLEVEL_HIGHEST);

        hr = task->get_Settings(&settings);
        if (FAILED(hr) || settings == nullptr)
        {
            failureStep = L"ITaskDefinition::get_Settings";
            failureHr = hr;
            break;
        }

        settings->put_StartWhenAvailable(VARIANT_TRUE);
        settings->put_DisallowStartIfOnBatteries(VARIANT_FALSE);
        settings->put_StopIfGoingOnBatteries(VARIANT_FALSE);
        settings->put_MultipleInstances(TASK_INSTANCES_IGNORE_NEW);
        settings->put_AllowDemandStart(VARIANT_TRUE);
        settings->put_Enabled(VARIANT_TRUE);
        settings->put_Hidden(VARIANT_FALSE);
        settings->put_ExecutionTimeLimit(_bstr_t(L"PT0S"));

        hr = task->get_Triggers(&triggers);
        if (FAILED(hr) || triggers == nullptr)
        {
            failureStep = L"ITaskDefinition::get_Triggers";
            failureHr = hr;
            break;
        }

        hr = triggers->Create(TASK_TRIGGER_LOGON, &trigger);
        if (FAILED(hr) || trigger == nullptr)
        {
            failureStep = L"ITriggerCollection::Create";
            failureHr = hr;
            break;
        }

        hr = trigger->QueryInterface(IID_ILogonTrigger, reinterpret_cast<void**>(&logonTrigger));
        if (FAILED(hr) || logonTrigger == nullptr)
        {
            failureStep = L"ITrigger::QueryInterface(ILogonTrigger)";
            failureHr = hr;
            break;
        }

        logonTrigger->put_Id(_bstr_t(L"LogonTrigger"));
        logonTrigger->put_UserId(_bstr_t(userId.c_str()));

        hr = task->get_Actions(&actions);
        if (FAILED(hr) || actions == nullptr)
        {
            failureStep = L"ITaskDefinition::get_Actions";
            failureHr = hr;
            break;
        }

        hr = actions->Create(TASK_ACTION_EXEC, &action);
        if (FAILED(hr) || action == nullptr)
        {
            failureStep = L"IActionCollection::Create";
            failureHr = hr;
            break;
        }

        hr = action->QueryInterface(IID_IExecAction, reinterpret_cast<void**>(&execAction));
        if (FAILED(hr) || execAction == nullptr)
        {
            failureStep = L"IAction::QueryInterface(IExecAction)";
            failureHr = hr;
            break;
        }

        execAction->put_Path(_bstr_t(exePath.c_str()));
        execAction->put_WorkingDirectory(_bstr_t(exeDir.c_str()));

        hr = rootFolder->RegisterTaskDefinition(
            _bstr_t(taskName.c_str()),
            task,
            TASK_CREATE_OR_UPDATE,
            _variant_t(),
            _variant_t(),
            TASK_LOGON_INTERACTIVE_TOKEN,
            _variant_t(L""),
            &registeredTask);
        if (FAILED(hr) || registeredTask == nullptr)
        {
            failureStep = L"ITaskFolder::RegisterTaskDefinition";
            failureHr = hr;
            break;
        }

        ok = true;
    }
    while (false);

    SafeRelease(registeredTask);
    SafeRelease(execAction);
    SafeRelease(action);
    SafeRelease(actions);
    SafeRelease(logonTrigger);
    SafeRelease(trigger);
    SafeRelease(triggers);
    SafeRelease(settings);
    SafeRelease(principal);
    SafeRelease(registrationInfo);
    SafeRelease(task);
    SafeRelease(rootFolder);
    SafeRelease(service);

    if (!ok)
    {
        if (failureStep != nullptr)
        {
            DebugLogHResult(failureStep, failureHr);
        }
        else
        {
            DebugLog(L"[TASK] registration failed before a COM step reported an HRESULT.");
        }
    }

    return ok;
}
