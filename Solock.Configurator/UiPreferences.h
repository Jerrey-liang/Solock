#pragma once

#include <string>

namespace solock_configurator
{
    enum class ThemePreference
    {
        FollowSystem,
        Light,
        Dark
    };

    enum class LanguagePreference
    {
        FollowSystem,
        English,
        ChineseSimplified
    };

    struct UiPreferences
    {
        ThemePreference theme = ThemePreference::FollowSystem;
        LanguagePreference language = LanguagePreference::FollowSystem;
    };

    class UiPreferencesRepository
    {
    public:
        static std::wstring GetSettingsFilePath();

        bool Load(UiPreferences& preferences, std::wstring& error) const;
        bool Save(UiPreferences const& preferences, std::wstring& error) const;
    };
}
