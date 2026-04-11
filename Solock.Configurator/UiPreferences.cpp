#include "pch.h"
#include "UiPreferences.h"

#include "ConfigRepository.h"

#include <cwctype>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace
{
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

    std::wstring ToString(const solock_configurator::ThemePreference preference)
    {
        switch (preference)
        {
        case solock_configurator::ThemePreference::Light:
            return L"light";
        case solock_configurator::ThemePreference::Dark:
            return L"dark";
        default:
            return L"system";
        }
    }

    std::wstring ToString(const solock_configurator::LanguagePreference preference)
    {
        switch (preference)
        {
        case solock_configurator::LanguagePreference::English:
            return L"en";
        case solock_configurator::LanguagePreference::ChineseSimplified:
            return L"zh-CN";
        default:
            return L"system";
        }
    }

    void ParseValue(const std::wstring& key, const std::wstring& value, solock_configurator::UiPreferences& preferences)
    {
        if (_wcsicmp(key.c_str(), L"theme") == 0)
        {
            if (_wcsicmp(value.c_str(), L"light") == 0)
            {
                preferences.theme = solock_configurator::ThemePreference::Light;
            }
            else if (_wcsicmp(value.c_str(), L"dark") == 0)
            {
                preferences.theme = solock_configurator::ThemePreference::Dark;
            }
            else
            {
                preferences.theme = solock_configurator::ThemePreference::FollowSystem;
            }
        }
        else if (_wcsicmp(key.c_str(), L"language") == 0)
        {
            if (_wcsicmp(value.c_str(), L"en") == 0 || _wcsicmp(value.c_str(), L"en-US") == 0)
            {
                preferences.language = solock_configurator::LanguagePreference::English;
            }
            else if (_wcsicmp(value.c_str(), L"zh") == 0 || _wcsicmp(value.c_str(), L"zh-CN") == 0)
            {
                preferences.language = solock_configurator::LanguagePreference::ChineseSimplified;
            }
            else
            {
                preferences.language = solock_configurator::LanguagePreference::FollowSystem;
            }
        }
    }
}

namespace solock_configurator
{
    std::wstring UiPreferencesRepository::GetSettingsFilePath()
    {
        const std::wstring stateDirectory = ConfigRepository::GetStateDirectoryPath();
        if (stateDirectory.empty())
        {
            return L"";
        }

        return stateDirectory + L"\\configurator_ui.cfg";
    }

    bool UiPreferencesRepository::Load(UiPreferences& preferences, std::wstring& error) const
    {
        error.clear();
        preferences = {};

        const std::wstring path = GetSettingsFilePath();
        if (path.empty())
        {
            error = L"Unable to resolve the UI settings path.";
            return false;
        }

        std::wifstream input{ std::filesystem::path(path) };
        if (!input.is_open())
        {
            return true;
        }

        std::wstring line;
        while (std::getline(input, line))
        {
            const std::wstring trimmed = TrimWhitespace(line);
            if (trimmed.empty() || trimmed[0] == L'#' || trimmed[0] == L';')
            {
                continue;
            }

            const size_t separator = trimmed.find(L'=');
            if (separator == std::wstring::npos)
            {
                continue;
            }

            ParseValue(
                TrimWhitespace(trimmed.substr(0, separator)),
                TrimWhitespace(trimmed.substr(separator + 1)),
                preferences);
        }

        return true;
    }

    bool UiPreferencesRepository::Save(UiPreferences const& preferences, std::wstring& error) const
    {
        error.clear();

        const std::wstring path = GetSettingsFilePath();
        if (path.empty())
        {
            error = L"Unable to resolve the UI settings path.";
            return false;
        }

        std::error_code filesystemError;
        std::filesystem::create_directories(std::filesystem::path(path).parent_path(), filesystemError);
        if (filesystemError)
        {
            error = L"Unable to create the UI settings directory.";
            return false;
        }

        std::wofstream output{ std::filesystem::path(path), std::ios::trunc };
        if (!output.is_open())
        {
            error = L"Unable to write the UI settings file.";
            return false;
        }

        output << L"theme=" << ToString(preferences.theme) << L"\n";
        output << L"language=" << ToString(preferences.language) << L"\n";
        return true;
    }
}
