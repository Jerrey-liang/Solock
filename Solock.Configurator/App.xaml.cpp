#include "pch.h"
#include "App.xaml.h"
#include "MainWindow.xaml.h"

using namespace winrt;
using namespace Microsoft::UI::Xaml;

// To learn more about WinUI, the WinUI project structure,
// and more about our project templates, see: http://aka.ms/winui-project-info.

namespace winrt::Solock_Configurator::implementation
{
    namespace
    {
        std::wstring FormatHex(const HRESULT hr)
        {
            wchar_t buffer[16] = {};
            swprintf_s(buffer, L"0x%08lX", static_cast<unsigned long>(hr));
            return buffer;
        }

        void ReportResourceInitializationFailure(const wchar_t* phase, const winrt::hresult_error& ex)
        {
            std::wstring message = phase;
            message += L"\r\n\r\n";
            message += ex.message().c_str();
            message += L"\r\nHRESULT: ";
            message += FormatHex(ex.code());

            ::OutputDebugStringW((message + L"\r\n").c_str());
            ::MessageBoxW(nullptr, message.c_str(), L"Solock Configurator", MB_OK | MB_ICONERROR);
        }
    }

    /// <summary>
    /// Initializes the singleton application object.  This is the first line of authored code
    /// executed, and as such is the logical equivalent of main() or WinMain().
    /// </summary>
    App::App()
    {
        try
        {
            InitializeComponent();
        }
        catch (winrt::hresult_error const& ex)
        {
            ReportResourceInitializationFailure(L"Failed to initialize App.xaml resources. Continuing without app-level XAML resources.", ex);
        }

#if defined _DEBUG && !defined DISABLE_XAML_GENERATED_BREAK_ON_UNHANDLED_EXCEPTION
        UnhandledException([](IInspectable const&, UnhandledExceptionEventArgs const& e)
        {
            if (IsDebuggerPresent())
            {
                auto errorMessage = e.Message();
                __debugbreak();
            }
        });
#endif
    }

    /// <summary>
    /// Invoked when the application is launched.
    /// </summary>
    /// <param name="e">Details about the launch request and process.</param>
    void App::OnLaunched([[maybe_unused]] LaunchActivatedEventArgs const& e)
    {
        try
        {
            window = make<MainWindow>();
            window.Activate();
        }
        catch (winrt::hresult_error const& ex)
        {
            const std::wstring message =
                L"Failed to create the main window.\r\n\r\n" +
                std::wstring(ex.message().c_str()) +
                L"\r\nHRESULT: 0x" + std::to_wstring(static_cast<uint32_t>(ex.code()));
            ::MessageBoxW(nullptr, message.c_str(), L"Solock Configurator", MB_OK | MB_ICONERROR);
            return;
        }
    }
}
