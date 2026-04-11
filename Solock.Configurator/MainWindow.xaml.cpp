#include "pch.h"
#include "MainWindow.xaml.h"
#if __has_include("MainWindow.g.cpp")
#include "MainWindow.g.cpp"
#endif

#include <microsoft.ui.xaml.window.h>

#include <algorithm>
#include <chrono>
#include <cwctype>
#include <iomanip>
#include <sstream>
#include <string_view>
#include <vector>

using namespace winrt;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;

namespace winrt::Solock_Configurator::implementation
{
    namespace
    {
        constexpr wchar_t kCustomBlock24HourClock[] = L"24HourClock";
        constexpr int kDefaultEveningHotspotStartMinute = 17 * 60 + 40;
        constexpr int kDefaultMiddayShutdownStartMinute = 12 * 60 + 10;
        constexpr int kDefaultMiddayShutdownEndMinute = 12 * 60 + 50;
        constexpr int kDefaultEveningShutdownStartMinute = 17 * 60 + 50;
        constexpr double kDefaultNormalPercent = 60.0;
        constexpr double kDefaultReducedPercent = 35.0;

        std::wstring TrimWhitespace(const std::wstring& value)
        {
            size_t start = 0;
            while (start < value.size() && std::iswspace(static_cast<wint_t>(value[start])))
            {
                ++start;
            }

            size_t end = value.size();
            while (end > start && std::iswspace(static_cast<wint_t>(value[end - 1])))
            {
                --end;
            }

            return value.substr(start, end - start);
        }

        winrt::Windows::Foundation::TimeSpan MinuteOfDayToTimeSpan(const int minuteOfDay)
        {
            const int clampedMinute = std::clamp(minuteOfDay, 0, (24 * 60) - 1);
            return std::chrono::duration_cast<winrt::Windows::Foundation::TimeSpan>(std::chrono::minutes(clampedMinute));
        }

        std::wstring FormatMinuteOfDay(const int minuteOfDay)
        {
            const int totalMinutes = ((minuteOfDay % (24 * 60)) + (24 * 60)) % (24 * 60);
            const int hour = totalMinutes / 60;
            const int minute = totalMinutes % 60;

            wchar_t buffer[6] = {};
            swprintf_s(buffer, L"%02d:%02d", hour, minute);
            return buffer;
        }

        std::wstring FormatTimeSpan(const winrt::Windows::Foundation::TimeSpan& value)
        {
            const auto totalMinutes = static_cast<int>(
                std::chrono::duration_cast<std::chrono::minutes>(value).count());
            return FormatMinuteOfDay(totalMinutes);
        }

        std::wstring FormatCompactDouble(const double value)
        {
            std::wostringstream output;
            output << std::fixed << std::setprecision(2) << value;

            std::wstring text = output.str();
            while (!text.empty() && text.back() == L'0')
            {
                text.pop_back();
            }

            if (!text.empty() && text.back() == L'.')
            {
                text.pop_back();
            }

            return text.empty() ? L"0" : text;
        }

        bool IsCheckBoxChecked(CheckBox const& checkBox)
        {
            if (const auto value = checkBox.IsChecked())
            {
                return value.Value();
            }

            return false;
        }

        winrt::Windows::Foundation::IReference<bool> BoxBoolean(const bool value)
        {
            return winrt::box_value(value).as<winrt::Windows::Foundation::IReference<bool>>();
        }

        winrt::Windows::Foundation::IReference<winrt::Windows::UI::Color> BoxColor(
            const winrt::Windows::UI::Color& value)
        {
            return winrt::box_value(value).as<winrt::Windows::Foundation::IReference<winrt::Windows::UI::Color>>();
        }

        Microsoft::UI::Xaml::Media::SolidColorBrush MakeBrush(const winrt::Windows::UI::Color& color)
        {
            Microsoft::UI::Xaml::Media::SolidColorBrush brush;
            brush.Color(color);
            return brush;
        }

        bool LooksTrue(const std::wstring& value)
        {
            return !_wcsicmp(value.c_str(), L"1") ||
                !_wcsicmp(value.c_str(), L"true") ||
                !_wcsicmp(value.c_str(), L"yes") ||
                !_wcsicmp(value.c_str(), L"on");
        }

        bool LooksFalse(const std::wstring& value)
        {
            return !_wcsicmp(value.c_str(), L"0") ||
                !_wcsicmp(value.c_str(), L"false") ||
                !_wcsicmp(value.c_str(), L"no") ||
                !_wcsicmp(value.c_str(), L"off");
        }

        bool TryParseStrictInt(const std::wstring& value, int& parsedValue)
        {
            const std::wstring trimmed = TrimWhitespace(value);
            if (trimmed.empty())
            {
                return false;
            }

            std::wistringstream input(trimmed);
            int result = 0;
            wchar_t trailing = L'\0';
            if (!(input >> result) || (input >> trailing))
            {
                return false;
            }

            parsedValue = result;
            return true;
        }

        bool TryParseStrictFloat(const std::wstring& value, float& parsedValue)
        {
            const std::wstring trimmed = TrimWhitespace(value);
            if (trimmed.empty())
            {
                return false;
            }

            std::wistringstream input(trimmed);
            float result = 0.0f;
            wchar_t trailing = L'\0';
            if (!(input >> result) || (input >> trailing))
            {
                return false;
            }

            parsedValue = result;
            return true;
        }

        bool TryParseMinuteOfDay(const std::wstring& value, int& minuteOfDay)
        {
            const std::wstring trimmed = TrimWhitespace(value);
            const size_t separator = trimmed.find(L':');
            if (separator == std::wstring::npos || trimmed.find(L':', separator + 1) != std::wstring::npos)
            {
                return false;
            }

            int hour = 0;
            int minute = 0;
            if (!TryParseStrictInt(trimmed.substr(0, separator), hour) ||
                !TryParseStrictInt(trimmed.substr(separator + 1), minute))
            {
                return false;
            }

            if (hour < 0 || hour > 23 || minute < 0 || minute > 59)
            {
                return false;
            }

            minuteOfDay = hour * 60 + minute;
            return true;
        }
    }

    MainWindow::MainWindow()
    {
        LoadUiPreferences();

        m_controlsReady = false;
        m_isUpdatingForm = true;
        InitializeComponent();
        ApplyThemePreference();
        ApplyUiPreferencesToControls();
        m_isUpdatingForm = false;
    }

    void MainWindow::RootContent_Loaded(IInspectable const&, RoutedEventArgs const&)
    {
        if (m_initialLoadCompleted)
        {
            return;
        }

        m_initialLoadCompleted = true;
        m_controlsReady = true;

        try
        {
            ApplyUiPreferencesToControls();
            ApplyThemePreference();
            InitializeWindowChrome();
            m_actualThemeChangedToken = WindowRootGrid().ActualThemeChanged({ this, &MainWindow::WindowRootThemeChanged });
            RefreshWallpaperTheme(false);
            ApplyLocalization();
            LoadConfiguration();
        }
        catch (winrt::hresult_error const& ex)
        {
            const std::wstring message =
                Translate(L"Window initialization failed.\r\n\r\n", L"") +
                std::wstring(ex.message().c_str());
            ::MessageBoxW(
                nullptr,
                message.c_str(),
                Translate(L"Solock Configurator", L"").c_str(),
                MB_OK | MB_ICONERROR);
        }
    }

    void MainWindow::SetStatus(const std::wstring& message)
    {
        if (!m_controlsReady)
        {
            return;
        }

        StatusTextBlock().Text(hstring{ message });
    }

    void MainWindow::SetValidationMessage(TextBlock const& target, std::wstring const& message)
    {
        if (!m_controlsReady)
        {
            return;
        }

        target.Text(hstring{ message });
        target.Visibility(message.empty() ? Visibility::Collapsed : Visibility::Visible);
    }

    void MainWindow::ClearValidationMessages()
    {
        SetValidationMessage(MiddayShutdownStartErrorTextBlock(), L"");
        SetValidationMessage(MiddayShutdownEndErrorTextBlock(), L"");
        SetValidationMessage(EveningHotspotStartErrorTextBlock(), L"");
        SetValidationMessage(EveningShutdownStartErrorTextBlock(), L"");
        SetValidationMessage(NormalPercentErrorTextBlock(), L"");
        SetValidationMessage(ReducedPercentErrorTextBlock(), L"");
        SetValidationMessage(CustomBlocksErrorTextBlock(), L"");
    }

    void MainWindow::LoadUiPreferences()
    {
        std::wstring error;
        m_uiPreferencesRepository.Load(m_uiPreferences, error);
    }

    void MainWindow::ApplyUiPreferencesToControls()
    {
        m_isUpdatingForm = true;
        ThemeModeComboBox().SelectedIndex(static_cast<int32_t>(m_uiPreferences.theme));
        LanguageModeComboBox().SelectedIndex(static_cast<int32_t>(m_uiPreferences.language));
        m_isUpdatingForm = false;
    }

    void MainWindow::ApplyThemePreference()
    {
        switch (m_uiPreferences.theme)
        {
        case solock_configurator::ThemePreference::Light:
            WindowRootGrid().RequestedTheme(ElementTheme::Light);
            break;
        case solock_configurator::ThemePreference::Dark:
            WindowRootGrid().RequestedTheme(ElementTheme::Dark);
            break;
        default:
            WindowRootGrid().RequestedTheme(ElementTheme::Default);
            break;
        }
    }

    bool MainWindow::IsDarkModeActive()
    {
        return WindowRootGrid().ActualTheme() == ElementTheme::Dark;
    }

    bool MainWindow::UseChineseUi()
    {
        switch (m_uiPreferences.language)
        {
        case solock_configurator::LanguagePreference::English:
            return false;
        case solock_configurator::LanguagePreference::ChineseSimplified:
            return true;
        default:
            return PRIMARYLANGID(::GetUserDefaultUILanguage()) == LANG_CHINESE;
        }
    }

    std::wstring MainWindow::Translate(wchar_t const* english, wchar_t const*)
    {
        if (english == nullptr)
        {
            return L"";
        }

        if (!UseChineseUi())
        {
            return english;
        }

        const std::wstring_view key{ english };
        using translation_t = std::pair<std::wstring_view, std::wstring_view>;
        static constexpr translation_t translations[] =
        {
            { L"Solock Configurator", L"Solock \u914d\u7f6e\u5668" },
            { L"Edit Solock config in separate pages, validate before save, and manage Solock.Agent.exe from one window.",
              L"\u5728\u72ec\u7acb\u9875\u9762\u4e2d\u7f16\u8f91 Solock \u914d\u7f6e\uff0c\u4fdd\u5b58\u524d\u5148\u6821\u9a8c\uff0c\u5e76\u5728\u540c\u4e00\u7a97\u53e3\u91cc\u7ba1\u7406 Solock.Agent.exe\u3002" },
            { L"Validation summary", L"\u6821\u9a8c\u6458\u8981" },
            { L"Reload config", L"\u91cd\u65b0\u52a0\u8f7d\u914d\u7f6e" },
            { L"Save config", L"\u4fdd\u5b58\u914d\u7f6e" },
            { L"Overview", L"\u6982\u89c8" },
            { L"Use the other pages to edit schedule, volume, custom blocks, and agent actions. Saving still writes config.cfg in one atomic step.",
              L"\u4f7f\u7528\u5176\u4ed6\u9875\u9762\u7f16\u8f91\u8ba1\u5212\u3001\u97f3\u91cf\u3001\u81ea\u5b9a\u4e49\u65f6\u6bb5\u548c\u4ee3\u7406\u7a0b\u5e8f\u64cd\u4f5c\u3002\u4fdd\u5b58\u65f6\u4ecd\u4f1a\u4ee5\u4e00\u6b21\u539f\u5b50\u5199\u5165\u66f4\u65b0 config.cfg\u3002" },
            { L"Config file", L"\u914d\u7f6e\u6587\u4ef6" },
            { L"Original hotspot SSID (read-only)", L"\u539f\u59cb\u70ed\u70b9 SSID\uff08\u53ea\u8bfb\uff09" },
            { L"Activity", L"\u6d3b\u52a8" },
            { L"Appearance", L"\u5916\u89c2" },
            { L"Choose light or dark mode, let the app follow the system, and derive the accent color from the current wallpaper.",
              L"\u53ef\u9009\u62e9\u4eae\u8272\u6216\u6697\u8272\u6a21\u5f0f\uff0c\u4e5f\u53ef\u8ddf\u968f\u7cfb\u7edf\uff0c\u5e76\u4ece\u5f53\u524d\u58c1\u7eb8\u4e2d\u63d0\u53d6\u4e3b\u9898\u8272\u3002" },
            { L"Theme mode", L"\u4e3b\u9898\u6a21\u5f0f" },
            { L"Follow system", L"\u8ddf\u968f\u7cfb\u7edf" },
            { L"Light", L"\u6d45\u8272" },
            { L"Dark", L"\u6df1\u8272" },
            { L"Language", L"\u8bed\u8a00" },
            { L"Chinese (Simplified)", L"\u7b80\u4f53\u4e2d\u6587" },
            { L"Aero background", L"Aero \u80cc\u666f" },
            { L"Wallpaper theme color", L"\u58c1\u7eb8\u4e3b\u9898\u8272" },
            { L"Refresh wallpaper theme", L"\u5237\u65b0\u58c1\u7eb8\u4e3b\u9898\u8272" },
            { L"Schedule", L"\u8ba1\u5212" },
            { L"Evening Hotspot", L"\u665a\u95f4\u70ed\u70b9" },
            { L"Turn off the default option only when you want to override the agent schedule.",
              L"\u53ea\u6709\u5728\u9700\u8981\u8986\u76d6\u4ee3\u7406\u7a0b\u5e8f\u9ed8\u8ba4\u8ba1\u5212\u65f6\uff0c\u624d\u5173\u95ed\u201c\u4f7f\u7528\u9ed8\u8ba4\u503c\u201d\u3002" },
            { L"Evening hotspot mode", L"\u665a\u95f4\u70ed\u70b9\u6a21\u5f0f" },
            { L"Use default", L"\u4f7f\u7528\u9ed8\u8ba4\u503c" },
            { L"Enabled", L"\u542f\u7528" },
            { L"Disabled", L"\u7981\u7528" },
            { L"Use agent default", L"\u4f7f\u7528\u4ee3\u7406\u9ed8\u8ba4\u503c" },
            { L"Evening hotspot start", L"\u665a\u95f4\u70ed\u70b9\u5f00\u59cb\u65f6\u95f4" },
            { L"Midday Shutdown", L"\u4e2d\u5348\u5173\u673a" },
            { L"Configure the temporary shutdown window around midday.", L"\u914d\u7f6e\u4e2d\u5348\u65f6\u6bb5\u7684\u4e34\u65f6\u5173\u673a\u7a97\u53e3\u3002" },
            { L"Midday shutdown start", L"\u4e2d\u5348\u5173\u673a\u5f00\u59cb\u65f6\u95f4" },
            { L"Midday shutdown end", L"\u4e2d\u5348\u5173\u673a\u7ed3\u675f\u65f6\u95f4" },
            { L"Evening Shutdown", L"\u665a\u95f4\u5173\u673a" },
            { L"Use this section to configure the evening shutdown start time.", L"\u5728\u8fd9\u91cc\u914d\u7f6e\u665a\u95f4\u5173\u673a\u7684\u5f00\u59cb\u65f6\u95f4\u3002" },
            { L"Evening shutdown start", L"\u665a\u95f4\u5173\u673a\u5f00\u59cb\u65f6\u95f4" },
            { L"Volume", L"\u97f3\u91cf" },
            { L"Volume Levels", L"\u97f3\u91cf\u7ea7\u522b" },
            { L"Tune the normal and reduced volume targets independently. Turn off the default option to set an explicit percentage with the slider.",
              L"\u53ef\u5206\u522b\u8c03\u6574\u6b63\u5e38\u97f3\u91cf\u548c\u964d\u4f4e\u97f3\u91cf\u7684\u76ee\u6807\u503c\u3002\u5173\u95ed\u9ed8\u8ba4\u9009\u9879\u540e\uff0c\u53ef\u4ee5\u7528\u6ed1\u5757\u6307\u5b9a\u660e\u786e\u767e\u5206\u6bd4\u3002" },
            { L"Normal volume percent", L"\u6b63\u5e38\u97f3\u91cf\u767e\u5206\u6bd4" },
            { L"Reduced volume percent", L"\u964d\u4f4e\u97f3\u91cf\u767e\u5206\u6bd4" },
            { L"Custom Blocks", L"\u81ea\u5b9a\u4e49\u65f6\u6bb5" },
            { L"Add each block as a separate row. Use the time picker for the start time. Duration, interval, and repeat count remain optional positive integers.",
              L"\u6bcf\u4e2a\u65f6\u6bb5\u4f7f\u7528\u5355\u72ec\u4e00\u884c\u3002\u5f00\u59cb\u65f6\u95f4\u4f7f\u7528\u65f6\u95f4\u9009\u62e9\u5668\uff1b\u6301\u7eed\u65f6\u95f4\u3001\u95f4\u9694\u548c\u91cd\u590d\u6b21\u6570\u4ecd\u7136\u662f\u53ef\u9009\u7684\u6b63\u6574\u6570\u3002" },
            { L"Add custom block", L"\u6dfb\u52a0\u81ea\u5b9a\u4e49\u65f6\u6bb5" },
            { L"No custom blocks configured.", L"\u5f53\u524d\u6ca1\u6709\u81ea\u5b9a\u4e49\u65f6\u6bb5\u3002" },
            { L"Agent", L"\u4ee3\u7406\u7a0b\u5e8f" },
            { L"Refresh agent status", L"\u5237\u65b0\u4ee3\u7406\u72b6\u6001" },
            { L"Start Solock.Agent", L"\u542f\u52a8 Solock.Agent" },
            { L"Kill Solock.Agent", L"\u7ed3\u675f Solock.Agent" },
            { L"Discovered Solock.Agent.exe", L"\u53d1\u73b0\u7684 Solock.Agent.exe" },
            { L"Running agent process", L"\u6b63\u5728\u8fd0\u884c\u7684\u4ee3\u7406\u8fdb\u7a0b" },
            { L"All editable fields are valid. You can save the current overrides from any page.",
              L"\u6240\u6709\u53ef\u7f16\u8f91\u5b57\u6bb5\u90fd\u6709\u6548\uff0c\u53ef\u4ee5\u4ece\u4efb\u610f\u9875\u9762\u4fdd\u5b58\u5f53\u524d\u8986\u76d6\u914d\u7f6e\u3002" },
            { L"Some inputs are invalid. Review the highlighted fields on the affected pages before saving.",
              L"\u90e8\u5206\u8f93\u5165\u65e0\u6548\u3002\u8bf7\u5148\u68c0\u67e5\u5bf9\u5e94\u9875\u9762\u4e2d\u9ad8\u4eae\u63d0\u793a\u7684\u5b57\u6bb5\uff0c\u518d\u6267\u884c\u4fdd\u5b58\u3002" },
            { L"Derived from wallpaper: ", L"\u53d6\u81ea\u5f53\u524d\u58c1\u7eb8: " },
            { L"Wallpaper analysis failed. Using the system accent instead. ", L"\u58c1\u7eb8\u5206\u6790\u5931\u8d25\uff0c\u5df2\u6539\u7528\u7cfb\u7edf\u5f3a\u8c03\u8272\u3002" },
            { L"Using the current system accent color.", L"\u6b63\u5728\u4f7f\u7528\u5f53\u524d\u7cfb\u7edf\u5f3a\u8c03\u8272\u3002" },
            { L"Refresh the accent after changing your wallpaper.", L"\u66f4\u6362\u58c1\u7eb8\u540e\uff0c\u53ef\u4ee5\u5728\u8fd9\u91cc\u91cd\u65b0\u63d0\u53d6\u4e3b\u9898\u8272\u3002" },
            { L"Desktop acrylic is active behind the window and follows the current theme colors.",
              L"\u7a97\u53e3\u6b63\u5728\u4f7f\u7528\u684c\u9762 Acrylic \u80cc\u666f\u6548\u679c\uff0c\u5e76\u4f1a\u8ddf\u968f\u5f53\u524d\u4e3b\u9898\u914d\u8272\u3002" },
            { L"Desktop acrylic was unavailable, so the window is using a Mica fallback.",
              L"\u5f53\u524d\u7cfb\u7edf\u65e0\u6cd5\u542f\u7528\u684c\u9762 Acrylic\uff0c\u7a97\u53e3\u5df2\u81ea\u52a8\u56de\u9000\u5230 Mica \u6548\u679c\u3002" },
            { L"Backdrop effects are unavailable on this system, so the app uses translucent theme brushes instead.",
              L"\u5f53\u524d\u7cfb\u7edf\u65e0\u6cd5\u542f\u7528\u7a97\u53e3\u6750\u8d28\u6548\u679c\uff0c\u7a0b\u5e8f\u4f1a\u6539\u7528\u534a\u900f\u660e\u4e3b\u9898\u753b\u5237\u3002" },
            { L"Wallpaper theme refreshed.", L"\u58c1\u7eb8\u4e3b\u9898\u8272\u5df2\u5237\u65b0\u3002" },
            { L"Wallpaper analysis failed. Using the system accent instead: ", L"\u58c1\u7eb8\u5206\u6790\u5931\u8d25\uff0c\u5df2\u6539\u7528\u7cfb\u7edf\u5f3a\u8c03\u8272: " },
            { L"Using the current system accent color because the wallpaper could not be analyzed.",
              L"\u65e0\u6cd5\u5206\u6790\u5f53\u524d\u58c1\u7eb8\uff0c\u5df2\u6539\u7528\u7cfb\u7edf\u5f3a\u8c03\u8272\u3002" },
            { L"All fields are valid.", L"\u6240\u6709\u5b57\u6bb5\u90fd\u6709\u6548\u3002" },
            { L"Fix the highlighted inputs before saving.", L"\u8bf7\u5148\u4fee\u6b63\u9ad8\u4eae\u63d0\u793a\u7684\u8f93\u5165\u9879\uff0c\u518d\u6267\u884c\u4fdd\u5b58\u3002" },
            { L"Using agent default.", L"\u6b63\u5728\u4f7f\u7528\u4ee3\u7406\u9ed8\u8ba4\u503c\u3002" },
            { L"Loaded value needs review.", L"\u5df2\u52a0\u8f7d\u7684\u503c\u9700\u8981\u4eba\u5de5\u786e\u8ba4\u3002" },
            { L"Selected value: ", L"\u5f53\u524d\u9009\u62e9: " },
            { L"Block ", L"\u65f6\u6bb5 " },
            { L"Start time", L"\u5f00\u59cb\u65f6\u95f4" },
            { L"Remove", L"\u5220\u9664" },
            { L"Duration (minutes)", L"\u6301\u7eed\u65f6\u95f4\uff08\u5206\u949f\uff09" },
            { L"Optional", L"\u53ef\u9009" },
            { L"Interval (minutes)", L"\u95f4\u9694\uff08\u5206\u949f\uff09" },
            { L"Repeat count", L"\u91cd\u590d\u6b21\u6570" },
            { L"duration", L"\u6301\u7eed\u65f6\u95f4" },
            { L"interval", L"\u95f4\u9694" },
            { L"repeat count", L"\u91cd\u590d\u6b21\u6570" },
            { L" is invalid in the loaded config. Pick a new time or use the default option.",
              L" \u5728\u5df2\u52a0\u8f7d\u914d\u7f6e\u4e2d\u65e0\u6548\u3002\u8bf7\u9009\u62e9\u65b0\u7684\u65f6\u95f4\uff0c\u6216\u6539\u7528\u9ed8\u8ba4\u503c\u3002" },
            { L" is invalid in the loaded config. Move the slider or use the default option.",
              L" \u5728\u5df2\u52a0\u8f7d\u914d\u7f6e\u4e2d\u65e0\u6548\u3002\u8bf7\u62d6\u52a8\u6ed1\u5757\uff0c\u6216\u6539\u7528\u9ed8\u8ba4\u503c\u3002" },
            { L" must be an integer greater than 0.", L" \u5fc5\u987b\u662f\u5927\u4e8e 0 \u7684\u6574\u6570\u3002" },
            { L" has an invalid start time. Pick a new time.", L" \u7684\u5f00\u59cb\u65f6\u95f4\u65e0\u6548\u3002\u8bf7\u9009\u62e9\u65b0\u7684\u65f6\u95f4\u3002" },
            { L"Agent is running from a path outside the preferred Release location.",
              L"\u4ee3\u7406\u7a0b\u5e8f\u6b63\u5728\u4ece\u9884\u671f Release \u76ee\u5f55\u4e4b\u5916\u7684\u8def\u5f84\u8fd0\u884c\u3002" },
            { L"Use Kill Solock.Agent to stop the current process, or build D:\\C++\\Projects\\Solock\\x64\\Release\\Solock.Agent.exe.",
              L"\u53ef\u4f7f\u7528\u201c\u7ed3\u675f Solock.Agent\u201d\u505c\u6b62\u5f53\u524d\u8fdb\u7a0b\uff0c\u6216\u5148\u6784\u5efa D:\\C++\\Projects\\Solock\\x64\\Release\\Solock.Agent.exe\u3002" },
            { L"Agent status: running from an unexpected path.", L"\u4ee3\u7406\u72b6\u6001\uff1a\u6b63\u5728\u4ece\u975e\u9884\u671f\u8def\u5f84\u8fd0\u884c\u3002" },
            { L"Agent executable not found.", L"\u672a\u627e\u5230\u4ee3\u7406\u7a0b\u5e8f\u53ef\u6267\u884c\u6587\u4ef6\u3002" },
            { L"Build D:\\C++\\Projects\\Solock\\x64\\Release\\Solock.Agent.exe first.",
              L"\u8bf7\u5148\u6784\u5efa D:\\C++\\Projects\\Solock\\x64\\Release\\Solock.Agent.exe\u3002" },
            { L"Agent status: executable not found.", L"\u4ee3\u7406\u72b6\u6001\uff1a\u672a\u627e\u5230\u53ef\u6267\u884c\u6587\u4ef6\u3002" },
            { L"Agent is currently running.", L"\u4ee3\u7406\u7a0b\u5e8f\u5f53\u524d\u6b63\u5728\u8fd0\u884c\u3002" },
            { L"The agent keeps reloading config.cfg on its heartbeat. Saving here should take effect without a restart. Use Kill Solock.Agent to stop it immediately.",
              L"\u4ee3\u7406\u7a0b\u5e8f\u4f1a\u5728\u5fc3\u8df3\u5468\u671f\u5185\u6301\u7eed\u91cd\u8f7d config.cfg\u3002\u8fd9\u91cc\u4fdd\u5b58\u540e\u901a\u5e38\u65e0\u9700\u91cd\u542f\u5373\u53ef\u751f\u6548\uff1b\u5982\u9700\u7acb\u5373\u505c\u6b62\uff0c\u53ef\u4f7f\u7528\u201c\u7ed3\u675f Solock.Agent\u201d\u3002" },
            { L"Agent status: running.", L"\u4ee3\u7406\u72b6\u6001\uff1a\u8fd0\u884c\u4e2d\u3002" },
            { L"Agent is not running.", L"\u4ee3\u7406\u7a0b\u5e8f\u5f53\u524d\u672a\u8fd0\u884c\u3002" },
            { L"Use Start Solock.Agent to launch the discovered executable.", L"\u53ef\u4f7f\u7528\u201c\u542f\u52a8 Solock.Agent\u201d\u542f\u52a8\u5df2\u53d1\u73b0\u7684\u53ef\u6267\u884c\u6587\u4ef6\u3002" },
            { L"Agent status: not running.", L"\u4ee3\u7406\u72b6\u6001\uff1a\u672a\u8fd0\u884c\u3002" },
            { L"Load failed: ", L"\u52a0\u8f7d\u5931\u8d25: " },
            { L"Loaded ", L"\u5df2\u52a0\u8f7d " },
            { L"Validation failed: ", L"\u6821\u9a8c\u5931\u8d25: " },
            { L"Save failed: ", L"\u4fdd\u5b58\u5931\u8d25: " },
            { L"Config saved.", L"\u914d\u7f6e\u5df2\u4fdd\u5b58\u3002" },
            { L"Launch failed: ", L"\u542f\u52a8\u5931\u8d25: " },
            { L"Agent started: ", L"\u4ee3\u7406\u5df2\u542f\u52a8: " },
            { L"Kill failed: ", L"\u7ed3\u675f\u5931\u8d25: " },
            { L"Appearance updated, but UI settings could not be saved: ", L"\u5916\u89c2\u5df2\u66f4\u65b0\uff0c\u4f46\u754c\u9762\u8bbe\u7f6e\u65e0\u6cd5\u4fdd\u5b58: " },
            { L"Appearance updated.", L"\u5916\u89c2\u8bbe\u7f6e\u5df2\u66f4\u65b0\u3002" },
            { L"Window initialization failed.\r\n\r\n", L"\u7a97\u53e3\u521d\u59cb\u5316\u5931\u8d25\u3002\r\n\r\n" }
        };

        for (const auto& translation : translations)
        {
            if (translation.first == key)
            {
                return std::wstring{ translation.second };
            }
        }

        return english;
    }

    std::wstring MainWindow::GetCustomBlockLabel(const size_t blockIndex)
    {
        return Translate(L"Block ", L"") + std::to_wstring(blockIndex);
    }

    void MainWindow::UpdateBrushColor(wchar_t const* resourceKey, winrt::Windows::UI::Color const& color)
    {
        const auto resources = Application::Current().Resources();
        const auto key = box_value(hstring{ resourceKey });

        if (resources.HasKey(key))
        {
            if (const auto existingBrush = resources.Lookup(key).try_as<Microsoft::UI::Xaml::Media::SolidColorBrush>())
            {
                existingBrush.Color(color);
                return;
            }
        }

        resources.Insert(key, MakeBrush(color));
    }

    void MainWindow::UpdateColorResource(wchar_t const* resourceKey, winrt::Windows::UI::Color const& color)
    {
        const auto resources = Application::Current().Resources();
        resources.Insert(box_value(hstring{ resourceKey }), box_value(color));
    }

    void MainWindow::ApplyThemePalette()
    {
        if (m_wallpaperAccentColor.A == 0)
        {
            m_wallpaperAccentColor = solock_configurator::GetSystemAccentColorFallback();
        }

        const auto palette = solock_configurator::BuildThemePalette(m_wallpaperAccentColor, IsDarkModeActive());

        UpdateBrushColor(L"AppAccentBrush", palette.accent);
        UpdateBrushColor(L"AppAccentForegroundBrush", palette.accentForeground);
        UpdateBrushColor(L"AppWindowBackgroundBrush", palette.windowBackground);
        UpdateBrushColor(L"AppCardBackgroundBrush", palette.cardBackground);
        UpdateBrushColor(L"AppCardBorderBrush", palette.cardBorder);
        UpdateBrushColor(L"AppMutedTextBrush", palette.mutedText);
        UpdateBrushColor(L"AppErrorBrush", palette.error);

        UpdateColorResource(L"SystemAccentColor", palette.accent);
        UpdateColorResource(L"SystemAccentColorLight1", palette.accentLight);
        UpdateColorResource(L"SystemAccentColorLight2", palette.accentLight);
        UpdateColorResource(L"SystemAccentColorLight3", palette.accentLight);
        UpdateColorResource(L"SystemAccentColorDark1", palette.accentDark);
        UpdateColorResource(L"SystemAccentColorDark2", palette.accentDark);
        UpdateColorResource(L"SystemAccentColorDark3", palette.accentDark);

        AppTitleTextBlock().Foreground(MakeBrush(palette.appTitle));
        AppSubtitleTextBlock().Foreground(MakeBrush(palette.appSubtitle));
        ApplyCustomBlockRowPalette();

        if (m_appWindow && Microsoft::UI::Windowing::AppWindowTitleBar::IsCustomizationSupported())
        {
            const auto titleBar = m_appWindow.TitleBar();
            titleBar.BackgroundColor(BoxColor(palette.titleBarBackground));
            titleBar.ForegroundColor(BoxColor(palette.titleBarForeground));
            titleBar.InactiveBackgroundColor(BoxColor(palette.titleBarBackground));
            titleBar.InactiveForegroundColor(BoxColor(palette.mutedText));
            titleBar.ButtonBackgroundColor(BoxColor(palette.titleBarBackground));
            titleBar.ButtonForegroundColor(BoxColor(palette.titleBarForeground));
            titleBar.ButtonInactiveBackgroundColor(BoxColor(palette.titleBarBackground));
            titleBar.ButtonInactiveForegroundColor(BoxColor(palette.mutedText));
            titleBar.ButtonHoverBackgroundColor(BoxColor(palette.titleBarButtonHoverBackground));
            titleBar.ButtonHoverForegroundColor(BoxColor(palette.titleBarForeground));
            titleBar.ButtonPressedBackgroundColor(BoxColor(palette.titleBarButtonPressedBackground));
            titleBar.ButtonPressedForegroundColor(BoxColor(palette.titleBarForeground));
        }
    }

    void MainWindow::ApplyCustomBlockRowPalette()
    {
        if (!m_controlsReady || m_wallpaperAccentColor.A == 0)
        {
            return;
        }

        const auto palette = solock_configurator::BuildThemePalette(m_wallpaperAccentColor, IsDarkModeActive());
        const auto children = CustomBlocksPanel().Children();
        for (uint32_t index = 0; index < children.Size(); ++index)
        {
            const auto rowBorder = children.GetAt(index).as<Border>();
            rowBorder.Background(MakeBrush(palette.cardBackground));
            rowBorder.BorderBrush(MakeBrush(palette.cardBorder));
        }
    }

    void MainWindow::InitializeWindowChrome()
    {
        m_backdropKind = BackdropKind::None;

        try
        {
            if (const auto windowNative = this->try_as<::IWindowNative>())
            {
                HWND windowHandle = nullptr;
                check_hresult(windowNative->get_WindowHandle(&windowHandle));
                if (windowHandle != nullptr)
                {
                    m_appWindow = Microsoft::UI::Windowing::AppWindow::GetFromWindowId(
                        Microsoft::UI::GetWindowIdFromWindow(windowHandle));
                }
            }
        }
        catch (...)
        {
            m_appWindow = nullptr;
        }

        try
        {
            SystemBackdrop(Microsoft::UI::Xaml::Media::DesktopAcrylicBackdrop{});
            m_backdropKind = BackdropKind::DesktopAcrylic;
        }
        catch (...)
        {
            try
            {
                SystemBackdrop(Microsoft::UI::Xaml::Media::MicaBackdrop{});
                m_backdropKind = BackdropKind::Mica;
            }
            catch (...)
            {
                SystemBackdrop(Microsoft::UI::Xaml::Media::SystemBackdrop{ nullptr });
                m_backdropKind = BackdropKind::None;
            }
        }
    }

    void MainWindow::UpdateWallpaperThemeTexts()
    {
        if (!m_controlsReady)
        {
            return;
        }

        if (m_wallpaperAccentFromWallpaper && !m_wallpaperPath.empty())
        {
            WallpaperThemeSourceTextBlock().Text(hstring{
                Translate(L"Derived from wallpaper: ", L"") + m_wallpaperPath });
        }
        else if (!m_wallpaperThemeError.empty())
        {
            WallpaperThemeSourceTextBlock().Text(hstring{
                Translate(L"Wallpaper analysis failed. Using the system accent instead. ", L"") +
                m_wallpaperThemeError });
        }
        else
        {
            WallpaperThemeSourceTextBlock().Text(hstring{
                Translate(L"Using the current system accent color.", L"") });
        }

        WallpaperThemeHintTextBlock().Text(hstring{
            Translate(
                L"Refresh the accent after changing your wallpaper.",
                L"") });
    }

    void MainWindow::UpdateAeroEffectText()
    {
        if (!m_controlsReady)
        {
            return;
        }

        switch (m_backdropKind)
        {
        case BackdropKind::DesktopAcrylic:
            AeroEffectHintTextBlock().Text(hstring{
                Translate(
                    L"Desktop acrylic is active behind the window and follows the current theme colors.",
                    L"") });
            return;
        case BackdropKind::Mica:
            AeroEffectHintTextBlock().Text(hstring{
                Translate(
                    L"Desktop acrylic was unavailable, so the window is using a Mica fallback.",
                    L"") });
            return;
        default:
            AeroEffectHintTextBlock().Text(hstring{
                Translate(
                    L"Backdrop effects are unavailable on this system, so the app uses translucent theme brushes instead.",
                    L"") });
            return;
        }
    }

    void MainWindow::RefreshWallpaperTheme(const bool updateStatus)
    {
        std::wstring wallpaperPath;
        std::wstring wallpaperError;
        winrt::Windows::UI::Color accentColor = solock_configurator::GetSystemAccentColorFallback();

        m_wallpaperAccentFromWallpaper = solock_configurator::TryGetWallpaperAccentColor(
            accentColor,
            wallpaperPath,
            wallpaperError);

        m_wallpaperAccentColor = accentColor;
        m_wallpaperPath = wallpaperPath;
        m_wallpaperThemeError = wallpaperError;

        ApplyThemePalette();
        UpdateWallpaperThemeTexts();

        if (!updateStatus)
        {
            return;
        }

        if (m_wallpaperAccentFromWallpaper)
        {
            SetStatus(Translate(L"Wallpaper theme refreshed.", L""));
            return;
        }

        if (!m_wallpaperThemeError.empty())
        {
            SetStatus(
                Translate(L"Wallpaper analysis failed. Using the system accent instead: ",
                          L"") +
                m_wallpaperThemeError);
            return;
        }

        SetStatus(Translate(
            L"Using the current system accent color because the wallpaper could not be analyzed.",
            L""));
    }

    void MainWindow::ApplyLocalization()
    {
        if (!m_controlsReady)
        {
            return;
        }

        const auto setText = [&](auto const& control, wchar_t const* english, wchar_t const* chinese)
        {
            control.Text(hstring{ Translate(english, chinese) });
        };

        const auto setContent = [&](auto const& control, wchar_t const* english, wchar_t const* chinese)
        {
            control.Content(box_value(hstring{ Translate(english, chinese) }));
        };

        const auto setHeader = [&](auto const& control, wchar_t const* english, wchar_t const* chinese)
        {
            control.Header(box_value(hstring{ Translate(english, chinese) }));
        };

        Title(hstring{ Translate(L"Solock Configurator", L"") });

        setText(AppTitleTextBlock(), L"Solock Configurator", L"");
        setText(
            AppSubtitleTextBlock(),
            L"Edit Solock config in separate pages, validate before save, and manage Solock.Agent.exe from one window.",
            L"");
        setText(ValidationSummaryLabelTextBlock(), L"Validation summary", L"");

        setContent(LoadButton(), L"Reload config", L"");
        setContent(SaveButton(), L"Save config", L"");

        OverviewTabViewItem().Header(box_value(hstring{ Translate(L"Overview", L"") }));
        setText(OverviewHeaderTextBlock(), L"Overview", L"");
        setText(
            OverviewDescriptionTextBlock(),
            L"Use the other pages to edit schedule, volume, custom blocks, and agent actions. Saving still writes config.cfg in one atomic step.",
            L"");
        setHeader(ConfigPathTextBox(), L"Config file", L"");
        setHeader(OriginalSsidTextBox(), L"Original hotspot SSID (read-only)", L"");
        setText(ActivityLabelTextBlock(), L"Activity", L"");

        AppearanceTabViewItem().Header(box_value(hstring{ Translate(L"Appearance", L"") }));
        setText(AppearanceHeaderTextBlock(), L"Appearance", L"");
        setText(
            AppearanceDescriptionTextBlock(),
            L"Choose light or dark mode, let the app follow the system, and derive the accent color from the current wallpaper.",
            L"");
        setText(ThemeModeLabelTextBlock(), L"Theme mode", L"");
        setContent(ThemeModeSystemComboBoxItem(), L"Follow system", L"");
        setContent(ThemeModeLightComboBoxItem(), L"Light", L"");
        setContent(ThemeModeDarkComboBoxItem(), L"Dark", L"");
        setText(LanguageModeLabelTextBlock(), L"Language", L"");
        setContent(LanguageModeSystemComboBoxItem(), L"Follow system", L"");
        setContent(LanguageModeEnglishComboBoxItem(), L"English", L"");
        setContent(LanguageModeChineseComboBoxItem(), L"Chinese (Simplified)", L"");
        setText(AeroEffectLabelTextBlock(), L"Aero background", L"");
        setText(WallpaperThemeLabelTextBlock(), L"Wallpaper theme color", L"");
        setContent(RefreshWallpaperThemeButton(), L"Refresh wallpaper theme", L"");

        ScheduleTabViewItem().Header(box_value(hstring{ Translate(L"Schedule", L"") }));
        setText(EveningHotspotHeaderTextBlock(), L"Evening Hotspot", L"");
        setText(
            EveningHotspotDescriptionTextBlock(),
            L"Turn off the default option only when you want to override the agent schedule.",
            L"");
        setText(EveningHotspotModeLabelTextBlock(), L"Evening hotspot mode", L"");
        setContent(EveningHotspotModeDefaultComboBoxItem(), L"Use default", L"");
        setContent(EveningHotspotModeEnabledComboBoxItem(), L"Enabled", L"");
        setContent(EveningHotspotModeDisabledComboBoxItem(), L"Disabled", L"");
        setContent(EveningHotspotStartDefaultCheckBox(), L"Use agent default", L"");
        setHeader(EveningHotspotStartTimePicker(), L"Evening hotspot start", L"");

        setText(MiddayShutdownHeaderTextBlock(), L"Midday Shutdown", L"");
        setText(
            MiddayShutdownDescriptionTextBlock(),
            L"Configure the temporary shutdown window around midday.",
            L"");
        setContent(MiddayShutdownStartDefaultCheckBox(), L"Use agent default", L"");
        setHeader(MiddayShutdownStartTimePicker(), L"Midday shutdown start", L"");
        setContent(MiddayShutdownEndDefaultCheckBox(), L"Use agent default", L"");
        setHeader(MiddayShutdownEndTimePicker(), L"Midday shutdown end", L"");

        setText(EveningShutdownHeaderTextBlock(), L"Evening Shutdown", L"");
        setText(
            EveningShutdownDescriptionTextBlock(),
            L"Use this section to configure the evening shutdown start time.",
            L"");
        setContent(EveningShutdownStartDefaultCheckBox(), L"Use agent default", L"");
        setHeader(EveningShutdownStartTimePicker(), L"Evening shutdown start", L"");

        VolumeTabViewItem().Header(box_value(hstring{ Translate(L"Volume", L"") }));
        setText(VolumeHeaderTextBlock(), L"Volume Levels", L"");
        setText(
            VolumeDescriptionTextBlock(),
            L"Tune the normal and reduced volume targets independently. Turn off the default option to set an explicit percentage with the slider.",
            L"");
        setText(NormalVolumeLabelTextBlock(), L"Normal volume percent", L"");
        setContent(NormalPercentDefaultCheckBox(), L"Use agent default", L"");
        setText(ReducedVolumeLabelTextBlock(), L"Reduced volume percent", L"");
        setContent(ReducedPercentDefaultCheckBox(), L"Use agent default", L"");

        CustomBlocksTabViewItem().Header(box_value(hstring{ Translate(L"Custom Blocks", L"") }));
        setText(CustomBlocksHeaderTextBlock(), L"Custom Blocks", L"");
        setText(
            CustomBlocksDescriptionTextBlock(),
            L"Add each block as a separate row. Use the time picker for the start time. Duration, interval, and repeat count remain optional positive integers.",
            L"");
        setContent(AddCustomBlockButton(), L"Add custom block", L"");
        setText(CustomBlocksEmptyTextBlock(), L"No custom blocks configured.", L"");

        AgentTabViewItem().Header(box_value(hstring{ Translate(L"Agent", L"") }));
        setText(AgentHeaderTextBlock(), L"Agent", L"");
        setContent(RefreshAgentStatusButton(), L"Refresh agent status", L"");
        setContent(LaunchAgentButton(), L"Start Solock.Agent", L"");
        setContent(KillAgentButton(), L"Kill Solock.Agent", L"");
        setHeader(DetectedAgentPathTextBox(), L"Discovered Solock.Agent.exe", L"");
        setHeader(RunningAgentPathTextBox(), L"Running agent process", L"");

        RefreshCustomBlockRowHeaders();
        UpdateCustomBlockEmptyState();
        RefreshVolumeValueLabels();
        UpdateWallpaperThemeTexts();
        UpdateAeroEffectText();
        ValidateForm();
        RefreshAgentStatus();
    }

    void MainWindow::UpdateOverviewState(const bool formIsValid)
    {
        if (formIsValid)
        {
            FormStateTextBlock().Text(hstring{
                Translate(
                    L"All editable fields are valid. You can save the current overrides from any page.",
                    L"") });
        }
        else
        {
            FormStateTextBlock().Text(hstring{
                Translate(
                    L"Some inputs are invalid. Review the highlighted fields on the affected pages before saving.",
                    L"") });
        }
    }

    void MainWindow::SetOptionalTimeControl(
        CheckBox const& checkBox,
        TimePicker const& timePicker,
        std::wstring const& rawValue,
        const int fallbackMinuteOfDay)
    {
        const std::wstring trimmed = TrimWhitespace(rawValue);
        if (trimmed.empty())
        {
            checkBox.IsChecked(BoxBoolean(true));
            timePicker.Time(MinuteOfDayToTimeSpan(fallbackMinuteOfDay));
            timePicker.Tag(nullptr);
            return;
        }

        checkBox.IsChecked(BoxBoolean(false));

        int minuteOfDay = 0;
        if (TryParseMinuteOfDay(trimmed, minuteOfDay))
        {
            timePicker.Time(MinuteOfDayToTimeSpan(minuteOfDay));
            timePicker.Tag(nullptr);
            return;
        }

        timePicker.Time(MinuteOfDayToTimeSpan(fallbackMinuteOfDay));
        timePicker.Tag(box_value(hstring{ trimmed }));
    }

    void MainWindow::SetOptionalSliderControl(
        CheckBox const& checkBox,
        Slider const& slider,
        std::wstring const& rawValue,
        const double fallbackValue)
    {
        const std::wstring trimmed = TrimWhitespace(rawValue);
        if (trimmed.empty())
        {
            checkBox.IsChecked(BoxBoolean(true));
            slider.Value(fallbackValue);
            slider.Tag(nullptr);
            return;
        }

        checkBox.IsChecked(BoxBoolean(false));

        float parsedValue = 0.0f;
        if (TryParseStrictFloat(trimmed, parsedValue) && parsedValue >= 0.0f && parsedValue <= 100.0f)
        {
            slider.Value(parsedValue);
            slider.Tag(nullptr);
            return;
        }

        slider.Value(fallbackValue);
        slider.Tag(box_value(hstring{ trimmed }));
    }

    void MainWindow::UpdateOptionalTimeControlState(CheckBox const& checkBox, TimePicker const& timePicker)
    {
        timePicker.IsEnabled(!IsCheckBoxChecked(checkBox));
    }

    void MainWindow::UpdateOptionalSliderControlState(CheckBox const& checkBox, Slider const& slider)
    {
        slider.IsEnabled(!IsCheckBoxChecked(checkBox));
    }

    void MainWindow::RefreshVolumeValueLabels()
    {
        const auto updateLabel = [&](CheckBox const& checkBox, Slider const& slider, TextBlock const& label)
        {
            if (IsCheckBoxChecked(checkBox))
            {
                label.Text(hstring{ Translate(L"Using agent default.", L"") });
                return;
            }

            const std::wstring invalidValue = unbox_value_or<hstring>(slider.Tag(), hstring{}).c_str();
            if (!invalidValue.empty())
            {
                label.Text(hstring{ Translate(L"Loaded value needs review.", L"") });
                return;
            }

            label.Text(hstring{
                Translate(L"Selected value: ", L"") +
                FormatCompactDouble(slider.Value()) +
                L"%" });
        };

        updateLabel(NormalPercentDefaultCheckBox(), NormalPercentSlider(), NormalPercentValueTextBlock());
        updateLabel(ReducedPercentDefaultCheckBox(), ReducedPercentSlider(), ReducedPercentValueTextBlock());
    }

    void MainWindow::UpdateOptionalInputStates()
    {
        UpdateOptionalTimeControlState(EveningHotspotStartDefaultCheckBox(), EveningHotspotStartTimePicker());
        UpdateOptionalTimeControlState(MiddayShutdownStartDefaultCheckBox(), MiddayShutdownStartTimePicker());
        UpdateOptionalTimeControlState(MiddayShutdownEndDefaultCheckBox(), MiddayShutdownEndTimePicker());
        UpdateOptionalTimeControlState(EveningShutdownStartDefaultCheckBox(), EveningShutdownStartTimePicker());
        UpdateOptionalSliderControlState(NormalPercentDefaultCheckBox(), NormalPercentSlider());
        UpdateOptionalSliderControlState(ReducedPercentDefaultCheckBox(), ReducedPercentSlider());
        RefreshVolumeValueLabels();
    }

    bool MainWindow::TryReadOptionalTimeControl(
        CheckBox const& checkBox,
        TimePicker const& timePicker,
        std::wstring& value,
        std::wstring& error,
        std::wstring const& fieldName)
    {
        value.clear();
        error.clear();

        if (IsCheckBoxChecked(checkBox))
        {
            return true;
        }

        const std::wstring invalidValue = unbox_value_or<hstring>(timePicker.Tag(), hstring{}).c_str();
        if (!invalidValue.empty())
        {
            error = fieldName + Translate(
                L" is invalid in the loaded config. Pick a new time or use the default option.",
                L"");
            return false;
        }

        value = FormatTimeSpan(timePicker.Time());
        return true;
    }

    bool MainWindow::TryReadOptionalSliderControl(
        CheckBox const& checkBox,
        Slider const& slider,
        std::wstring& value,
        std::wstring& error,
        std::wstring const& fieldName)
    {
        value.clear();
        error.clear();

        if (IsCheckBoxChecked(checkBox))
        {
            return true;
        }

        const std::wstring invalidValue = unbox_value_or<hstring>(slider.Tag(), hstring{}).c_str();
        if (!invalidValue.empty())
        {
            error = fieldName + Translate(
                L" is invalid in the loaded config. Move the slider or use the default option.",
                L"");
            return false;
        }

        value = FormatCompactDouble(slider.Value());
        return true;
    }

    void MainWindow::PopulateCustomBlockRows(const std::vector<solock_configurator::CustomBlockEntry>& blocks)
    {
        auto children = CustomBlocksPanel().Children();
        children.Clear();

        for (const auto& block : blocks)
        {
            AddCustomBlockRow(block);
        }

        RefreshCustomBlockRowHeaders();
        UpdateCustomBlockEmptyState();
        ApplyCustomBlockRowPalette();
    }

    void MainWindow::AddCustomBlockRow()
    {
        AddCustomBlockRow(solock_configurator::CustomBlockEntry{ L"00:00", L"", L"", L"" });
    }

    void MainWindow::AddCustomBlockRow(const solock_configurator::CustomBlockEntry& block)
    {
        Border rowBorder;
        rowBorder.BorderThickness(ThicknessHelper::FromUniformLength(1.0));
        rowBorder.CornerRadius(CornerRadiusHelper::FromUniformRadius(8.0));
        rowBorder.Padding(ThicknessHelper::FromUniformLength(12.0));

        StackPanel rowPanel;
        rowPanel.Spacing(10);

        TextBlock headerText;
        headerText.FontWeight(Microsoft::UI::Text::FontWeights::SemiBold());
        rowPanel.Children().Append(headerText);

        Grid startRow;
        startRow.ColumnSpacing(12.0);
        startRow.ColumnDefinitions().Append(ColumnDefinition{});
        ColumnDefinition removeButtonColumn;
        removeButtonColumn.Width(GridLengthHelper::Auto());
        startRow.ColumnDefinitions().Append(removeButtonColumn);

        TimePicker startTimePicker;
        startTimePicker.Header(box_value(hstring{ Translate(L"Start time", L"") }));
        startTimePicker.ClockIdentifier(kCustomBlock24HourClock);
        startTimePicker.MinuteIncrement(1);

        int minuteOfDay = 0;
        if (TryParseMinuteOfDay(block.start, minuteOfDay))
        {
            startTimePicker.Time(MinuteOfDayToTimeSpan(minuteOfDay));
        }
        else
        {
            rowBorder.Tag(box_value(hstring{ block.start }));
            startTimePicker.Time(MinuteOfDayToTimeSpan(0));
        }

        Button removeButton;
        removeButton.Content(box_value(hstring{ Translate(L"Remove", L"") }));
        removeButton.HorizontalAlignment(HorizontalAlignment::Right);

        Grid::SetColumn(removeButton, 1);
        startRow.Children().Append(startTimePicker);
        startRow.Children().Append(removeButton);

        Grid numericRow;
        numericRow.ColumnSpacing(12.0);
        numericRow.ColumnDefinitions().Append(ColumnDefinition{});
        numericRow.ColumnDefinitions().Append(ColumnDefinition{});
        numericRow.ColumnDefinitions().Append(ColumnDefinition{});

        TextBox durationTextBox;
        durationTextBox.Header(box_value(hstring{ Translate(L"Duration (minutes)", L"") }));
        durationTextBox.PlaceholderText(hstring{ Translate(L"Optional", L"") });
        durationTextBox.Text(hstring{ block.durationMinutes });

        TextBox intervalTextBox;
        intervalTextBox.Header(box_value(hstring{ Translate(L"Interval (minutes)", L"") }));
        intervalTextBox.PlaceholderText(hstring{ Translate(L"Optional", L"") });
        intervalTextBox.Text(hstring{ block.intervalMinutes });
        Grid::SetColumn(intervalTextBox, 1);

        TextBox repeatTextBox;
        repeatTextBox.Header(box_value(hstring{ Translate(L"Repeat count", L"") }));
        repeatTextBox.PlaceholderText(hstring{ Translate(L"Optional", L"") });
        repeatTextBox.Text(hstring{ block.repeatCount });
        Grid::SetColumn(repeatTextBox, 2);

        numericRow.Children().Append(durationTextBox);
        numericRow.Children().Append(intervalTextBox);
        numericRow.Children().Append(repeatTextBox);

        rowPanel.Children().Append(startRow);
        rowPanel.Children().Append(numericRow);
        rowBorder.Child(rowPanel);

        removeButton.Tag(rowBorder);
        removeButton.Click({ this, &MainWindow::RemoveCustomBlockButton_Click });

        startTimePicker.Tag(rowBorder);
        startTimePicker.TimeChanged({ this, &MainWindow::CustomBlockTimeChanged });
        durationTextBox.TextChanged({ this, &MainWindow::TextInputChanged });
        intervalTextBox.TextChanged({ this, &MainWindow::TextInputChanged });
        repeatTextBox.TextChanged({ this, &MainWindow::TextInputChanged });

        CustomBlocksPanel().Children().Append(rowBorder);
        RefreshCustomBlockRowHeaders();
        ApplyCustomBlockRowPalette();
    }

    void MainWindow::RefreshCustomBlockRowHeaders()
    {
        auto children = CustomBlocksPanel().Children();
        for (uint32_t index = 0; index < children.Size(); ++index)
        {
            const auto rowBorder = children.GetAt(index).as<Border>();
            const auto rowPanel = rowBorder.Child().as<StackPanel>();
            const auto headerText = rowPanel.Children().GetAt(0).as<TextBlock>();
            headerText.Text(hstring{ GetCustomBlockLabel(index + 1) });

            const auto startRow = rowPanel.Children().GetAt(1).as<Grid>();
            const auto startTimePicker = startRow.Children().GetAt(0).as<TimePicker>();
            const auto removeButton = startRow.Children().GetAt(1).as<Button>();
            startTimePicker.Header(box_value(hstring{ Translate(L"Start time", L"") }));
            removeButton.Content(box_value(hstring{ Translate(L"Remove", L"") }));

            const auto numericRow = rowPanel.Children().GetAt(2).as<Grid>();
            const auto durationTextBox = numericRow.Children().GetAt(0).as<TextBox>();
            const auto intervalTextBox = numericRow.Children().GetAt(1).as<TextBox>();
            const auto repeatTextBox = numericRow.Children().GetAt(2).as<TextBox>();

            durationTextBox.Header(box_value(hstring{ Translate(L"Duration (minutes)", L"") }));
            durationTextBox.PlaceholderText(hstring{ Translate(L"Optional", L"") });
            intervalTextBox.Header(box_value(hstring{ Translate(L"Interval (minutes)", L"") }));
            intervalTextBox.PlaceholderText(hstring{ Translate(L"Optional", L"") });
            repeatTextBox.Header(box_value(hstring{ Translate(L"Repeat count", L"") }));
            repeatTextBox.PlaceholderText(hstring{ Translate(L"Optional", L"") });
        }
    }

    void MainWindow::UpdateCustomBlockEmptyState()
    {
        CustomBlocksEmptyTextBlock().Visibility(
            CustomBlocksPanel().Children().Size() == 0 ? Visibility::Visible : Visibility::Collapsed);
    }

    bool MainWindow::TryCollectCustomBlocks(std::vector<solock_configurator::CustomBlockEntry>& blocks, std::wstring& error)
    {
        blocks.clear();
        error.clear();

        const auto validateOptionalPositiveInteger =
            [&](const std::wstring& rawValue, const wchar_t* fieldName, const size_t blockIndex) -> bool
        {
            const std::wstring trimmed = TrimWhitespace(rawValue);
            if (trimmed.empty())
            {
                return true;
            }

            int parsedValue = 0;
            if (!TryParseStrictInt(trimmed, parsedValue) || parsedValue <= 0)
            {
                error = GetCustomBlockLabel(blockIndex) + L" " + fieldName + Translate(
                    L" must be an integer greater than 0.",
                    L"");
                return false;
            }

            return true;
        };

        const auto children = CustomBlocksPanel().Children();
        for (uint32_t index = 0; index < children.Size(); ++index)
        {
            const auto rowBorder = children.GetAt(index).as<Border>();
            const std::wstring invalidStart = unbox_value_or<hstring>(rowBorder.Tag(), hstring{}).c_str();
            if (!invalidStart.empty())
            {
                error = GetCustomBlockLabel(index + 1) + Translate(
                    L" has an invalid start time. Pick a new time.",
                    L"");
                return false;
            }

            const auto rowPanel = rowBorder.Child().as<StackPanel>();
            const auto startRow = rowPanel.Children().GetAt(1).as<Grid>();
            const auto numericRow = rowPanel.Children().GetAt(2).as<Grid>();

            solock_configurator::CustomBlockEntry block;
            block.start = FormatTimeSpan(startRow.Children().GetAt(0).as<TimePicker>().Time());
            block.durationMinutes = TrimWhitespace(numericRow.Children().GetAt(0).as<TextBox>().Text().c_str());
            block.intervalMinutes = TrimWhitespace(numericRow.Children().GetAt(1).as<TextBox>().Text().c_str());
            block.repeatCount = TrimWhitespace(numericRow.Children().GetAt(2).as<TextBox>().Text().c_str());

            if (!validateOptionalPositiveInteger(
                    block.durationMinutes,
                    Translate(L"duration", L"").c_str(),
                    index + 1) ||
                !validateOptionalPositiveInteger(
                    block.intervalMinutes,
                    Translate(L"interval", L"").c_str(),
                    index + 1) ||
                !validateOptionalPositiveInteger(
                    block.repeatCount,
                    Translate(L"repeat count", L"").c_str(),
                    index + 1))
            {
                return false;
            }

            blocks.push_back(std::move(block));
        }

        return true;
    }

    void MainWindow::PopulateForm(const solock_configurator::ConfigSnapshot& snapshot)
    {
        if (!m_controlsReady)
        {
            return;
        }

        m_isUpdatingForm = true;

        ConfigPathTextBox().Text(hstring{ snapshot.configFilePath });
        OriginalSsidTextBox().Text(hstring{ snapshot.originalHotspotSsid });
        SetOptionalTimeControl(
            MiddayShutdownStartDefaultCheckBox(),
            MiddayShutdownStartTimePicker(),
            snapshot.middayShutdownStart,
            kDefaultMiddayShutdownStartMinute);
        SetOptionalTimeControl(
            MiddayShutdownEndDefaultCheckBox(),
            MiddayShutdownEndTimePicker(),
            snapshot.middayShutdownEnd,
            kDefaultMiddayShutdownEndMinute);
        SetOptionalTimeControl(
            EveningHotspotStartDefaultCheckBox(),
            EveningHotspotStartTimePicker(),
            snapshot.eveningHotspotStart,
            kDefaultEveningHotspotStartMinute);
        SetOptionalTimeControl(
            EveningShutdownStartDefaultCheckBox(),
            EveningShutdownStartTimePicker(),
            snapshot.eveningShutdownStart,
            kDefaultEveningShutdownStartMinute);
        SetOptionalSliderControl(
            NormalPercentDefaultCheckBox(),
            NormalPercentSlider(),
            snapshot.normalPercent,
            kDefaultNormalPercent);
        SetOptionalSliderControl(
            ReducedPercentDefaultCheckBox(),
            ReducedPercentSlider(),
            snapshot.reducedPercent,
            kDefaultReducedPercent);
        PopulateCustomBlockRows(snapshot.customBlocks);

        const std::wstring toggle = TrimWhitespace(snapshot.enableEveningHotspot);
        int32_t selectedIndex = 0;
        if (LooksTrue(toggle))
        {
            selectedIndex = 1;
        }
        else if (LooksFalse(toggle))
        {
            selectedIndex = 2;
        }

        EnableEveningHotspotComboBox().SelectedIndex(selectedIndex);
        UpdateOptionalInputStates();

        m_isUpdatingForm = false;
        ValidateForm();
    }

    bool MainWindow::ValidateForm()
    {
        if (!m_controlsReady)
        {
            return false;
        }

        ClearValidationMessages();

        bool isValid = true;
        auto validateOptionalTime =
            [&](CheckBox const& checkBox, TimePicker const& timePicker, TextBlock const& errorBlock, std::wstring const& fieldName)
        {
            std::wstring value;
            std::wstring error;
            if (!TryReadOptionalTimeControl(checkBox, timePicker, value, error, fieldName))
            {
                isValid = false;
                SetValidationMessage(errorBlock, error);
            }
        };

        auto validateOptionalPercent =
            [&](CheckBox const& checkBox, Slider const& slider, TextBlock const& errorBlock, std::wstring const& fieldName)
        {
            std::wstring value;
            std::wstring error;
            if (!TryReadOptionalSliderControl(checkBox, slider, value, error, fieldName))
            {
                isValid = false;
                SetValidationMessage(errorBlock, error);
            }
        };

        validateOptionalTime(
            MiddayShutdownStartDefaultCheckBox(),
            MiddayShutdownStartTimePicker(),
            MiddayShutdownStartErrorTextBlock(),
            Translate(L"Midday shutdown start", L""));
        validateOptionalTime(
            MiddayShutdownEndDefaultCheckBox(),
            MiddayShutdownEndTimePicker(),
            MiddayShutdownEndErrorTextBlock(),
            Translate(L"Midday shutdown end", L""));
        validateOptionalTime(
            EveningHotspotStartDefaultCheckBox(),
            EveningHotspotStartTimePicker(),
            EveningHotspotStartErrorTextBlock(),
            Translate(L"Evening hotspot start", L""));
        validateOptionalTime(
            EveningShutdownStartDefaultCheckBox(),
            EveningShutdownStartTimePicker(),
            EveningShutdownStartErrorTextBlock(),
            Translate(L"Evening shutdown start", L""));
        validateOptionalPercent(
            NormalPercentDefaultCheckBox(),
            NormalPercentSlider(),
            NormalPercentErrorTextBlock(),
            Translate(L"Normal volume percent", L""));
        validateOptionalPercent(
            ReducedPercentDefaultCheckBox(),
            ReducedPercentSlider(),
            ReducedPercentErrorTextBlock(),
            Translate(L"Reduced volume percent", L""));

        std::vector<solock_configurator::CustomBlockEntry> parsedBlocks;
        std::wstring customBlockError;
        if (!TryCollectCustomBlocks(parsedBlocks, customBlockError))
        {
            isValid = false;
            SetValidationMessage(CustomBlocksErrorTextBlock(), customBlockError);
        }

        SaveButton().IsEnabled(isValid);
        ValidationSummaryTextBlock().Text(isValid ?
            hstring{ Translate(L"All fields are valid.", L"") } :
            hstring{ Translate(L"Fix the highlighted inputs before saving.", L"") });

        UpdateOverviewState(isValid);
        return isValid;
    }

    void MainWindow::RefreshAgentStatus()
    {
        if (!m_controlsReady)
        {
            return;
        }

        std::wstring installedPath;
        const bool hasInstalledAgent = m_agentLauncher.TryGetInstalledAgentPath(installedPath);
        DetectedAgentPathTextBox().Text(hstring{ hasInstalledAgent ? installedPath : L"" });

        std::wstring runningPath;
        const bool isRunning = m_agentLauncher.TryGetRunningAgentPath(runningPath);
        RunningAgentPathTextBox().Text(hstring{ isRunning ? runningPath : L"" });

        LaunchAgentButton().IsEnabled(hasInstalledAgent && !isRunning);
        KillAgentButton().IsEnabled(isRunning);

        if (!hasInstalledAgent && isRunning)
        {
            AgentStatusTextBlock().Text(hstring{
                Translate(
                    L"Agent is running from a path outside the preferred Release location.",
                    L"") });
            AgentHintTextBlock().Text(hstring{
                Translate(
                    L"Use Kill Solock.Agent to stop the current process, or build D:\\C++\\Projects\\Solock\\x64\\Release\\Solock.Agent.exe.",
                    L"") });
            AgentStateSummaryTextBlock().Text(hstring{
                Translate(
                    L"Agent status: running from an unexpected path.",
                    L"") });
            return;
        }

        if (!hasInstalledAgent)
        {
            AgentStatusTextBlock().Text(hstring{
                Translate(L"Agent executable not found.", L"") });
            AgentHintTextBlock().Text(hstring{
                Translate(
                    L"Build D:\\C++\\Projects\\Solock\\x64\\Release\\Solock.Agent.exe first.",
                    L"") });
            AgentStateSummaryTextBlock().Text(hstring{
                Translate(
                    L"Agent status: executable not found.",
                    L"") });
            return;
        }

        if (isRunning)
        {
            AgentStatusTextBlock().Text(hstring{
                Translate(L"Agent is currently running.", L"") });
            AgentHintTextBlock().Text(hstring{
                Translate(
                    L"The agent keeps reloading config.cfg on its heartbeat. Saving here should take effect without a restart. Use Kill Solock.Agent to stop it immediately.",
                    L"") });
            AgentStateSummaryTextBlock().Text(hstring{
                Translate(L"Agent status: running.", L"") });
            return;
        }

        AgentStatusTextBlock().Text(hstring{
            Translate(L"Agent is not running.", L"") });
        AgentHintTextBlock().Text(hstring{
            Translate(
                L"Use Start Solock.Agent to launch the discovered executable.",
                L"") });
        AgentStateSummaryTextBlock().Text(hstring{
            Translate(L"Agent status: not running.", L"") });
    }

    void MainWindow::LoadConfiguration()
    {
        solock_configurator::ConfigSnapshot snapshot;
        std::wstring error;
        if (!m_repository.Load(snapshot, error))
        {
            SetStatus(Translate(L"Load failed: ", L"") + error);
            return;
        }

        PopulateForm(snapshot);
        RefreshAgentStatus();
        SetStatus(Translate(L"Loaded ", L"") + snapshot.configFilePath);
    }

    bool MainWindow::TryCaptureSnapshot(solock_configurator::ConfigSnapshot& snapshot, std::wstring& error)
    {
        error.clear();

        if (!ValidateForm())
        {
            error = Translate(L"Fix the highlighted inputs before saving.", L"");
            return false;
        }

        solock_configurator::ConfigSnapshot currentSnapshot;
        if (!m_repository.Load(currentSnapshot, error))
        {
            return false;
        }

        snapshot = currentSnapshot;
        snapshot.enableEveningHotspot.clear();
        switch (EnableEveningHotspotComboBox().SelectedIndex())
        {
        case 1:
            snapshot.enableEveningHotspot = L"true";
            break;
        case 2:
            snapshot.enableEveningHotspot = L"false";
            break;
        default:
            break;
        }

        if (!TryReadOptionalTimeControl(
            MiddayShutdownStartDefaultCheckBox(),
                MiddayShutdownStartTimePicker(),
                snapshot.middayShutdownStart,
                error,
                Translate(L"Midday shutdown start", L"")))
        {
            return false;
        }

        if (!TryReadOptionalTimeControl(
            MiddayShutdownEndDefaultCheckBox(),
                MiddayShutdownEndTimePicker(),
                snapshot.middayShutdownEnd,
                error,
                Translate(L"Midday shutdown end", L"")))
        {
            return false;
        }

        if (!TryReadOptionalTimeControl(
            EveningHotspotStartDefaultCheckBox(),
                EveningHotspotStartTimePicker(),
                snapshot.eveningHotspotStart,
                error,
                Translate(L"Evening hotspot start", L"")))
        {
            return false;
        }

        if (!TryReadOptionalTimeControl(
            EveningShutdownStartDefaultCheckBox(),
                EveningShutdownStartTimePicker(),
                snapshot.eveningShutdownStart,
                error,
                Translate(L"Evening shutdown start", L"")))
        {
            return false;
        }

        if (!TryReadOptionalSliderControl(
            NormalPercentDefaultCheckBox(),
                NormalPercentSlider(),
                snapshot.normalPercent,
                error,
                Translate(L"Normal volume percent", L"")))
        {
            return false;
        }

        if (!TryReadOptionalSliderControl(
            ReducedPercentDefaultCheckBox(),
                ReducedPercentSlider(),
                snapshot.reducedPercent,
                error,
                Translate(L"Reduced volume percent", L"")))
        {
            return false;
        }

        if (!TryCollectCustomBlocks(snapshot.customBlocks, error))
        {
            return false;
        }

        return true;
    }

    void MainWindow::LoadButton_Click(IInspectable const&, RoutedEventArgs const&)
    {
        LoadConfiguration();
    }

    void MainWindow::SaveButton_Click(IInspectable const&, RoutedEventArgs const&)
    {
        solock_configurator::ConfigSnapshot snapshot;
        std::wstring error;
        if (!TryCaptureSnapshot(snapshot, error))
        {
            SetStatus(Translate(L"Validation failed: ", L"") + error);
            return;
        }

        if (!m_repository.Save(snapshot, error))
        {
            SetStatus(Translate(L"Save failed: ", L"") + error);
            return;
        }

        LoadConfiguration();
        SetStatus(Translate(L"Config saved.", L""));
    }

    void MainWindow::RefreshAgentStatusButton_Click(IInspectable const&, RoutedEventArgs const&)
    {
        RefreshAgentStatus();
    }

    void MainWindow::LaunchAgentButton_Click(IInspectable const&, RoutedEventArgs const&)
    {
        std::wstring launchedPath;
        std::wstring error;
        if (!m_agentLauncher.Launch(launchedPath, error))
        {
            SetStatus(Translate(L"Launch failed: ", L"") + error);
            RefreshAgentStatus();
            return;
        }

        RefreshAgentStatus();
        SetStatus(Translate(L"Agent started: ", L"") + launchedPath);
    }

    void MainWindow::KillAgentButton_Click(IInspectable const&, RoutedEventArgs const&)
    {
        std::wstring result;
        std::wstring error;
        if (!m_agentLauncher.KillRunningAgents(result, error))
        {
            SetStatus(Translate(L"Kill failed: ", L"") + error);
            RefreshAgentStatus();
            return;
        }

        RefreshAgentStatus();
        SetStatus(result);
    }

    void MainWindow::CheckBoxChanged(IInspectable const&, RoutedEventArgs const&)
    {
        if (!m_controlsReady || m_isUpdatingForm)
        {
            return;
        }

        UpdateOptionalInputStates();
        ValidateForm();
    }

    void MainWindow::ScheduleTimeChanged(IInspectable const& sender, TimePickerValueChangedEventArgs const&)
    {
        if (!m_controlsReady || m_isUpdatingForm)
        {
            return;
        }

        sender.as<TimePicker>().Tag(nullptr);
        ValidateForm();
    }

    void MainWindow::VolumeSliderChanged(
        IInspectable const& sender,
        Microsoft::UI::Xaml::Controls::Primitives::RangeBaseValueChangedEventArgs const&)
    {
        if (!m_controlsReady || m_isUpdatingForm)
        {
            return;
        }

        sender.as<Slider>().Tag(nullptr);
        RefreshVolumeValueLabels();
        ValidateForm();
    }

    void MainWindow::AddCustomBlockButton_Click(IInspectable const&, RoutedEventArgs const&)
    {
        m_isUpdatingForm = true;
        AddCustomBlockRow();
        RefreshCustomBlockRowHeaders();
        UpdateCustomBlockEmptyState();
        m_isUpdatingForm = false;
        ValidateForm();
    }

    void MainWindow::RemoveCustomBlockButton_Click(IInspectable const& sender, RoutedEventArgs const&)
    {
        const auto button = sender.as<Button>();
        const auto rowBorder = button.Tag().try_as<Border>();
        if (!rowBorder)
        {
            return;
        }

        auto children = CustomBlocksPanel().Children();
        for (uint32_t index = 0; index < children.Size(); ++index)
        {
            if (children.GetAt(index) == rowBorder)
            {
                m_isUpdatingForm = true;
                children.RemoveAt(index);
                RefreshCustomBlockRowHeaders();
                UpdateCustomBlockEmptyState();
                m_isUpdatingForm = false;
                ValidateForm();
                return;
            }
        }
    }

    void MainWindow::CustomBlockTimeChanged(IInspectable const& sender, TimePickerValueChangedEventArgs const&)
    {
        const auto timePicker = sender.as<TimePicker>();
        const auto rowBorder = timePicker.Tag().try_as<Border>();
        if (rowBorder)
        {
            rowBorder.Tag(nullptr);
        }

        if (!m_controlsReady || m_isUpdatingForm)
        {
            return;
        }

        ValidateForm();
    }

    void MainWindow::TextInputChanged(IInspectable const&, TextChangedEventArgs const&)
    {
        if (!m_controlsReady || m_isUpdatingForm)
        {
            return;
        }

        ValidateForm();
    }

    void MainWindow::ComboSelectionChanged(IInspectable const&, SelectionChangedEventArgs const&)
    {
        if (!m_controlsReady || m_isUpdatingForm)
        {
            return;
        }

        ValidateForm();
    }

    void MainWindow::AppearanceSelectionChanged(IInspectable const&, SelectionChangedEventArgs const&)
    {
        if (!m_controlsReady || m_isUpdatingForm)
        {
            return;
        }

        const int32_t themeIndex = std::clamp(ThemeModeComboBox().SelectedIndex(), 0, 2);
        const int32_t languageIndex = std::clamp(LanguageModeComboBox().SelectedIndex(), 0, 2);
        m_uiPreferences.theme = static_cast<solock_configurator::ThemePreference>(themeIndex);
        m_uiPreferences.language = static_cast<solock_configurator::LanguagePreference>(languageIndex);

        ApplyThemePreference();
        ApplyLocalization();
        ApplyThemePalette();

        std::wstring error;
        if (!m_uiPreferencesRepository.Save(m_uiPreferences, error))
        {
            SetStatus(Translate(
                          L"Appearance updated, but UI settings could not be saved: ",
                          L"") + error);
            return;
        }

        SetStatus(Translate(L"Appearance updated.", L""));
    }

    void MainWindow::RefreshWallpaperThemeButton_Click(IInspectable const&, RoutedEventArgs const&)
    {
        RefreshWallpaperTheme(true);
    }

    void MainWindow::WindowRootThemeChanged(FrameworkElement const&, IInspectable const&)
    {
        ApplyThemePalette();
    }

}
