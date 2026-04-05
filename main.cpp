#include "SolockController.h"

#include <Windows.h>
#include <comdef.h>
#include <winrt/base.h>

#include <exception>
#include <iostream>
#include <sstream>
#include <string>

namespace
{
    std::wstring ToWide(const char* value)
    {
        if (value == nullptr || value[0] == '\0')
        {
            return L"<empty>";
        }

        return winrt::to_hstring(value).c_str();
    }

    std::wstring FormatHex(const HRESULT hr)
    {
        wchar_t buffer[16] = {};
        swprintf_s(buffer, L"0x%08lX", static_cast<unsigned long>(hr));
        return buffer;
    }

    std::wstring DescribeCurrentException()
    {
        try
        {
            throw;
        }
        catch (const winrt::hresult_error& ex)
        {
            std::wostringstream out;
            out << L"Unhandled WinRT exception: " << ex.message().c_str()
                << L" (" << FormatHex(ex.code()) << L")";
            return out.str();
        }
        catch (const _com_error& ex)
        {
            const HRESULT hr = ex.Error();
            std::wostringstream out;
            out << L"Unhandled COM exception: " << ex.ErrorMessage()
                << L" (" << FormatHex(hr) << L")";
            return out.str();
        }
        catch (const std::exception& ex)
        {
            return L"Unhandled std::exception: " + ToWide(ex.what());
        }
        catch (...)
        {
            return L"Unhandled unknown exception.";
        }
    }

    void ReportFatalError(const std::wstring& message)
    {
        const std::wstring fullMessage = L"Solock startup failed.\n\n" + message;
        ::OutputDebugStringW((fullMessage + L"\n").c_str());
        std::wcerr << fullMessage << std::endl;

#ifdef _DEBUG
        ::MessageBoxW(
            nullptr,
            fullMessage.c_str(),
            L"Solock Debug Failure",
            MB_OK | MB_ICONERROR | MB_TOPMOST);
        system("pause");
#endif
    }
}

int main()
{
    try
    {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);

        SolockController::Options options;

#ifdef _DEBUG
        options.debugSkipHotspotActions = false;
        options.debugSkipDestructiveActions = true;
        options.debugForceIdleState = true;
        options.debugStepDelayMilliseconds = 150;
#endif

        SolockController app(options);

#ifdef _DEBUG
        return app.RunAllFeaturesAcceleratedDebug();
#else
        return app.Run();
#endif
    }
    catch (...)
    {
        ReportFatalError(DescribeCurrentException());
        return 1;
    }
}
