#pragma once

#include "SolockController.h"

#include <Windows.h>

#include <string>

namespace solock::internal
{
    inline constexpr int kMaxMinuteOfDay = 23 * 60 + 59;

    int ClampMinuteOfDay(int hour, int minute);
    bool EqualsIgnoreCase(const std::wstring& left, const std::wstring& right);

    void DebugLog(const std::wstring& message);
    void DebugLogStatus(const wchar_t* step, DWORD status);
    void DebugLogHResult(const wchar_t* step, HRESULT status);

    template <typename T>
    void SafeRelease(T*& value)
    {
        if (value != nullptr)
        {
            value->Release();
            value = nullptr;
        }
    }
}
