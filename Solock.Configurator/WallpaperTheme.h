#pragma once

#include <string>

#include <winrt/Windows.UI.h>

namespace solock_configurator
{
    struct ThemePalette
    {
        winrt::Windows::UI::Color accent;
        winrt::Windows::UI::Color accentLight;
        winrt::Windows::UI::Color accentDark;
        winrt::Windows::UI::Color windowBackground;
        winrt::Windows::UI::Color cardBackground;
        winrt::Windows::UI::Color cardBorder;
        winrt::Windows::UI::Color mutedText;
        winrt::Windows::UI::Color appTitle;
        winrt::Windows::UI::Color appSubtitle;
        winrt::Windows::UI::Color error;
        winrt::Windows::UI::Color accentForeground;
        winrt::Windows::UI::Color titleBarBackground;
        winrt::Windows::UI::Color titleBarForeground;
        winrt::Windows::UI::Color titleBarButtonHoverBackground;
        winrt::Windows::UI::Color titleBarButtonPressedBackground;
    };

    bool TryGetWallpaperAccentColor(
        winrt::Windows::UI::Color& accentColor,
        std::wstring& wallpaperPath,
        std::wstring& error);

    winrt::Windows::UI::Color GetSystemAccentColorFallback();

    ThemePalette BuildThemePalette(
        winrt::Windows::UI::Color accentColor,
        bool darkMode);
}
