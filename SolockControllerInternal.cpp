#define _WIN32_DCOM
#include "SolockControllerInternal.h"

#include <algorithm>
#include <cwchar>
#include <iostream>
#include <sstream>

namespace
{
#ifdef _DEBUG
    std::wstring FormatStatusCode(const DWORD status)
    {
        wchar_t buffer[16] = {};
        swprintf_s(buffer, L"0x%08lX", status);
        return buffer;
    }

    std::wstring FormatHResult(const HRESULT status)
    {
        wchar_t buffer[16] = {};
        swprintf_s(buffer, L"0x%08lX", static_cast<unsigned long>(status));
        return buffer;
    }
#endif
}

namespace solock::internal
{
    int ClampMinuteOfDay(const int hour, const int minute)
    {
        return std::clamp(hour * 60 + minute, 0, kMaxMinuteOfDay);
    }

    bool EqualsIgnoreCase(const std::wstring& left, const std::wstring& right)
    {
        return _wcsicmp(left.c_str(), right.c_str()) == 0;
    }

#ifdef _DEBUG
    void DebugLog(const std::wstring& message)
    {
        std::wcout << L"[DEBUG] " << message << std::endl;
    }

    void DebugLogStatus(const wchar_t* step, const DWORD status)
    {
        std::wostringstream out;
        out << L"[WFP] " << step << L" returned "
            << FormatStatusCode(status) << L" (" << status << L")";
        DebugLog(out.str());
    }

    void DebugLogHResult(const wchar_t* step, const HRESULT status)
    {
        std::wostringstream out;
        out << L"[HRESULT] " << step << L" returned "
            << FormatHResult(status) << L" (" << status << L")";
        DebugLog(out.str());
    }
#else
    void DebugLog(const std::wstring&)
    {
    }

    void DebugLogStatus(const wchar_t*, const DWORD)
    {
    }

    void DebugLogHResult(const wchar_t*, const HRESULT)
    {
    }
#endif
}
