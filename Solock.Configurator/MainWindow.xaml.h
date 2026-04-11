#pragma once

#include "MainWindow.g.h"
#include "AgentLauncher.h"
#include "ConfigRepository.h"
#include "UiPreferences.h"
#include "WallpaperTheme.h"

namespace winrt::Solock_Configurator::implementation
{
    struct MainWindow : MainWindowT<MainWindow>
    {
        MainWindow();

        void LoadButton_Click(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void RootContent_Loaded(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void SaveButton_Click(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void RefreshAgentStatusButton_Click(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void LaunchAgentButton_Click(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void KillAgentButton_Click(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void CheckBoxChanged(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void ScheduleTimeChanged(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::Controls::TimePickerValueChangedEventArgs const& args);
        void VolumeSliderChanged(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::Controls::Primitives::RangeBaseValueChangedEventArgs const& args);
        void AddCustomBlockButton_Click(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void RemoveCustomBlockButton_Click(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void CustomBlockTimeChanged(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::Controls::TimePickerValueChangedEventArgs const& args);
        void TextInputChanged(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::Controls::TextChangedEventArgs const& args);
        void ComboSelectionChanged(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const& args);
        void AppearanceSelectionChanged(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const& args);
        void RefreshWallpaperThemeButton_Click(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);

    private:
        enum class BackdropKind
        {
            None,
            DesktopAcrylic,
            Mica
        };

        void LoadConfiguration();
        void PopulateForm(const solock_configurator::ConfigSnapshot& snapshot);
        void SetOptionalTimeControl(
            winrt::Microsoft::UI::Xaml::Controls::CheckBox const& checkBox,
            winrt::Microsoft::UI::Xaml::Controls::TimePicker const& timePicker,
            std::wstring const& rawValue,
            int fallbackMinuteOfDay);
        void SetOptionalSliderControl(
            winrt::Microsoft::UI::Xaml::Controls::CheckBox const& checkBox,
            winrt::Microsoft::UI::Xaml::Controls::Slider const& slider,
            std::wstring const& rawValue,
            double fallbackValue);
        void UpdateOptionalInputStates();
        void UpdateOptionalTimeControlState(
            winrt::Microsoft::UI::Xaml::Controls::CheckBox const& checkBox,
            winrt::Microsoft::UI::Xaml::Controls::TimePicker const& timePicker);
        void UpdateOptionalSliderControlState(
            winrt::Microsoft::UI::Xaml::Controls::CheckBox const& checkBox,
            winrt::Microsoft::UI::Xaml::Controls::Slider const& slider);
        void RefreshVolumeValueLabels();
        void LoadUiPreferences();
        void ApplyUiPreferencesToControls();
        void ApplyThemePreference();
        void ApplyLocalization();
        void RefreshWallpaperTheme(bool updateStatus);
        void UpdateWallpaperThemeTexts();
        void UpdateAeroEffectText();
        void ApplyThemePalette();
        void ApplyCustomBlockRowPalette();
        void InitializeWindowChrome();
        void WindowRootThemeChanged(
            winrt::Microsoft::UI::Xaml::FrameworkElement const& sender,
            winrt::Windows::Foundation::IInspectable const& args);
        bool IsDarkModeActive();
        bool UseChineseUi();
        std::wstring Translate(wchar_t const* english, wchar_t const* chinese);
        std::wstring GetCustomBlockLabel(size_t blockIndex);
        void UpdateBrushColor(
            wchar_t const* resourceKey,
            winrt::Windows::UI::Color const& color);
        void UpdateColorResource(
            wchar_t const* resourceKey,
            winrt::Windows::UI::Color const& color);
        bool TryReadOptionalTimeControl(
            winrt::Microsoft::UI::Xaml::Controls::CheckBox const& checkBox,
            winrt::Microsoft::UI::Xaml::Controls::TimePicker const& timePicker,
            std::wstring& value,
            std::wstring& error,
            std::wstring const& fieldName);
        bool TryReadOptionalSliderControl(
            winrt::Microsoft::UI::Xaml::Controls::CheckBox const& checkBox,
            winrt::Microsoft::UI::Xaml::Controls::Slider const& slider,
            std::wstring& value,
            std::wstring& error,
            std::wstring const& fieldName);
        void PopulateCustomBlockRows(const std::vector<solock_configurator::CustomBlockEntry>& blocks);
        void AddCustomBlockRow();
        void AddCustomBlockRow(const solock_configurator::CustomBlockEntry& block);
        void RefreshCustomBlockRowHeaders();
        void UpdateCustomBlockEmptyState();
        bool TryCollectCustomBlocks(std::vector<solock_configurator::CustomBlockEntry>& blocks, std::wstring& error);
        bool TryCaptureSnapshot(solock_configurator::ConfigSnapshot& snapshot, std::wstring& error);
        bool ValidateForm();
        void ClearValidationMessages();
        void SetValidationMessage(
            winrt::Microsoft::UI::Xaml::Controls::TextBlock const& target,
            std::wstring const& message);
        void UpdateOverviewState(bool formIsValid);
        void RefreshAgentStatus();
        void SetStatus(const std::wstring& message);

        solock_configurator::ConfigRepository m_repository;
        solock_configurator::AgentLauncher m_agentLauncher;
        solock_configurator::UiPreferencesRepository m_uiPreferencesRepository;
        solock_configurator::UiPreferences m_uiPreferences;
        winrt::Microsoft::UI::Windowing::AppWindow m_appWindow{ nullptr };
        winrt::event_token m_actualThemeChangedToken{};
        winrt::Windows::UI::Color m_wallpaperAccentColor{};
        std::wstring m_wallpaperPath;
        std::wstring m_wallpaperThemeError;
        BackdropKind m_backdropKind = BackdropKind::None;
        bool m_wallpaperAccentFromWallpaper = false;
        bool m_controlsReady = false;
        bool m_initialLoadCompleted = false;
        bool m_isUpdatingForm = false;
    };
}

namespace winrt::Solock_Configurator::factory_implementation
{
    struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow>
    {
    };
}
