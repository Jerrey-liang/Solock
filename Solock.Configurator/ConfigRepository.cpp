#include "pch.h"
#include "ConfigRepository.h"

#include <ShlObj.h>

#include <algorithm>
#include <cwctype>
#include <fstream>
#include <sstream>
#include <vector>

#pragma comment(lib, "shell32.lib")

namespace
{
    constexpr wchar_t kConfigFileName[] = L"config.cfg";
    constexpr wchar_t kLegacyConfigFileName[] = L"hotspot_and_block.ini";
    constexpr wchar_t kLegacyStateFileName[] = L"original_hotspot_ssid.txt";

    enum class TextEncoding
    {
        Utf8,
        Utf8Bom,
        Utf16Le,
        Utf16Be,
        Ansi
    };

    struct TextFileContent
    {
        bool exists = false;
        TextEncoding encoding = TextEncoding::Utf8Bom;
        std::wstring newline = L"\r\n";
        std::wstring text;
    };

    bool EqualsIgnoreCase(const std::wstring& left, const std::wstring& right)
    {
        return _wcsicmp(left.c_str(), right.c_str()) == 0;
    }

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

    bool FileExists(const std::wstring& path)
    {
        if (path.empty())
        {
            return false;
        }

        const DWORD attributes = ::GetFileAttributesW(path.c_str());
        return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
    }

    std::wstring BuildFilePath(const std::wstring& dir, const wchar_t* fileName)
    {
        if (dir.empty() || fileName == nullptr || *fileName == L'\0')
        {
            return L"";
        }

        return dir + L"\\" + fileName;
    }

    bool EnsurePreferredConfigFileAvailable(const std::wstring& preferredPath, const std::wstring& legacyPath)
    {
        if (preferredPath.empty())
        {
            return false;
        }

        if (FileExists(preferredPath))
        {
            return true;
        }

        if (legacyPath.empty() || !FileExists(legacyPath))
        {
            return true;
        }

        if (::CopyFileW(legacyPath.c_str(), preferredPath.c_str(), TRUE) == FALSE)
        {
            return false;
        }

        ::DeleteFileW(legacyPath.c_str());
        return true;
    }

    std::wstring ResolveConfigFilePathForRead(const std::wstring& preferredPath, const std::wstring& legacyPath)
    {
        if (EnsurePreferredConfigFileAvailable(preferredPath, legacyPath) && FileExists(preferredPath))
        {
            return preferredPath;
        }

        if (FileExists(preferredPath))
        {
            return preferredPath;
        }

        if (FileExists(legacyPath))
        {
            return legacyPath;
        }

        return preferredPath;
    }

    bool TryDecodeMultiByteString(const std::string& input, const UINT codePage, const DWORD flags, std::wstring& output)
    {
        output.clear();
        if (input.empty())
        {
            return true;
        }

        const int needed = ::MultiByteToWideChar(codePage, flags, input.data(), static_cast<int>(input.size()), nullptr, 0);
        if (needed <= 0)
        {
            return false;
        }

        output.resize(static_cast<size_t>(needed));
        const int written = ::MultiByteToWideChar(codePage, flags, input.data(), static_cast<int>(input.size()), output.data(), needed);
        if (written != needed)
        {
            output.clear();
            return false;
        }

        return true;
    }

    bool TryEncodeWideString(const std::wstring& input, const UINT codePage, std::string& output)
    {
        output.clear();
        if (input.empty())
        {
            return true;
        }

        const int needed = ::WideCharToMultiByte(codePage, 0, input.data(), static_cast<int>(input.size()), nullptr, 0, nullptr, nullptr);
        if (needed <= 0)
        {
            return false;
        }

        output.resize(static_cast<size_t>(needed));
        const int written = ::WideCharToMultiByte(codePage, 0, input.data(), static_cast<int>(input.size()), output.data(), needed, nullptr, nullptr);
        if (written != needed)
        {
            output.clear();
            return false;
        }

        return true;
    }

    bool LoadTextFile(const std::wstring& path, TextFileContent& content)
    {
        content = {};
        if (path.empty())
        {
            return false;
        }

        const DWORD attributes = ::GetFileAttributesW(path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES)
        {
            const DWORD error = ::GetLastError();
            if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
            {
                return true;
            }

            return false;
        }

        if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
        {
            return false;
        }

        std::ifstream input(path, std::ios::binary);
        if (!input.is_open())
        {
            return false;
        }

        const std::string bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        content.exists = true;
        if (bytes.empty())
        {
            return true;
        }

        if (bytes.size() >= 2 &&
            static_cast<unsigned char>(bytes[0]) == 0xFF &&
            static_cast<unsigned char>(bytes[1]) == 0xFE)
        {
            content.encoding = TextEncoding::Utf16Le;
            for (size_t i = 2; i + 1 < bytes.size(); i += 2)
            {
                const unsigned char low = static_cast<unsigned char>(bytes[i]);
                const unsigned char high = static_cast<unsigned char>(bytes[i + 1]);
                content.text.push_back(static_cast<wchar_t>(low | (high << 8)));
            }
        }
        else if (bytes.size() >= 2 &&
            static_cast<unsigned char>(bytes[0]) == 0xFE &&
            static_cast<unsigned char>(bytes[1]) == 0xFF)
        {
            content.encoding = TextEncoding::Utf16Be;
            for (size_t i = 2; i + 1 < bytes.size(); i += 2)
            {
                const unsigned char high = static_cast<unsigned char>(bytes[i]);
                const unsigned char low = static_cast<unsigned char>(bytes[i + 1]);
                content.text.push_back(static_cast<wchar_t>(low | (high << 8)));
            }
        }
        else
        {
            std::string textBytes = bytes;
            if (bytes.size() >= 3 &&
                static_cast<unsigned char>(bytes[0]) == 0xEF &&
                static_cast<unsigned char>(bytes[1]) == 0xBB &&
                static_cast<unsigned char>(bytes[2]) == 0xBF)
            {
                content.encoding = TextEncoding::Utf8Bom;
                textBytes.erase(0, 3);
            }
            else
            {
                content.encoding = TextEncoding::Utf8;
            }

            if (!TryDecodeMultiByteString(textBytes, CP_UTF8, MB_ERR_INVALID_CHARS, content.text))
            {
                content.encoding = TextEncoding::Ansi;
                if (!TryDecodeMultiByteString(textBytes, CP_ACP, 0, content.text))
                {
                    return false;
                }
            }
        }

        if (content.text.find(L"\r\n") != std::wstring::npos)
        {
            content.newline = L"\r\n";
        }
        else if (content.text.find(L'\n') != std::wstring::npos)
        {
            content.newline = L"\n";
        }

        return true;
    }

    bool SaveTextFile(const std::wstring& path, const TextEncoding encoding, const std::wstring& text)
    {
        if (path.empty())
        {
            return false;
        }

        std::string bytes;
        switch (encoding)
        {
        case TextEncoding::Utf8Bom:
        case TextEncoding::Utf8:
            if (!TryEncodeWideString(text, CP_UTF8, bytes))
            {
                return false;
            }
            if (encoding == TextEncoding::Utf8Bom)
            {
                bytes.insert(bytes.begin(), { static_cast<char>(0xEF), static_cast<char>(0xBB), static_cast<char>(0xBF) });
            }
            break;

        case TextEncoding::Utf16Le:
            bytes.reserve(2 + text.size() * 2);
            bytes.push_back(static_cast<char>(0xFF));
            bytes.push_back(static_cast<char>(0xFE));
            for (const wchar_t ch : text)
            {
                bytes.push_back(static_cast<char>(ch & 0xFF));
                bytes.push_back(static_cast<char>((ch >> 8) & 0xFF));
            }
            break;

        case TextEncoding::Utf16Be:
            bytes.reserve(2 + text.size() * 2);
            bytes.push_back(static_cast<char>(0xFE));
            bytes.push_back(static_cast<char>(0xFF));
            for (const wchar_t ch : text)
            {
                bytes.push_back(static_cast<char>((ch >> 8) & 0xFF));
                bytes.push_back(static_cast<char>(ch & 0xFF));
            }
            break;

        case TextEncoding::Ansi:
            if (!TryEncodeWideString(text, CP_ACP, bytes))
            {
                return false;
            }
            break;
        }

        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!output.is_open())
        {
            return false;
        }

        if (!bytes.empty())
        {
            output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        }

        return output.good();
    }

    bool TryParseIniSectionHeader(const std::wstring& line, std::wstring& sectionName)
    {
        sectionName.clear();
        const std::wstring trimmed = TrimWhitespace(line);
        if (trimmed.size() < 3 || trimmed.front() != L'[' || trimmed.back() != L']')
        {
            return false;
        }

        sectionName = TrimWhitespace(trimmed.substr(1, trimmed.size() - 2));
        return !sectionName.empty();
    }

    bool TryParseIniKeyValue(const std::wstring& line, std::wstring& key, std::wstring& value)
    {
        key.clear();
        value.clear();

        const size_t separator = line.find(L'=');
        if (separator == std::wstring::npos)
        {
            return false;
        }

        key = TrimWhitespace(line.substr(0, separator));
        if (key.empty())
        {
            return false;
        }

        value = TrimWhitespace(line.substr(separator + 1));
        return true;
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

    bool TryParseStrictBool(const std::wstring& value, bool& parsedValue)
    {
        const std::wstring trimmed = TrimWhitespace(value);
        if (trimmed.empty())
        {
            return false;
        }

        if (EqualsIgnoreCase(trimmed, L"1") ||
            EqualsIgnoreCase(trimmed, L"true") ||
            EqualsIgnoreCase(trimmed, L"yes") ||
            EqualsIgnoreCase(trimmed, L"on"))
        {
            parsedValue = true;
            return true;
        }

        if (EqualsIgnoreCase(trimmed, L"0") ||
            EqualsIgnoreCase(trimmed, L"false") ||
            EqualsIgnoreCase(trimmed, L"no") ||
            EqualsIgnoreCase(trimmed, L"off"))
        {
            parsedValue = false;
            return true;
        }

        return false;
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

    std::wstring FormatMinuteOfDay(const int minuteOfDay)
    {
        std::wostringstream output;
        output.fill(L'0');
        output.width(2);
        output << (minuteOfDay / 60);
        output << L":";
        output.width(2);
        output << (minuteOfDay % 60);
        return output.str();
    }

    std::wstring NormalizeOptionalMinuteOfDay(const std::wstring& value, const wchar_t* fieldName, std::wstring& error)
    {
        const std::wstring trimmed = TrimWhitespace(value);
        if (trimmed.empty())
        {
            return L"";
        }

        int minuteOfDay = 0;
        if (!TryParseMinuteOfDay(trimmed, minuteOfDay))
        {
            error = std::wstring(fieldName) + L" must use HH:MM format.";
            return L"";
        }

        return FormatMinuteOfDay(minuteOfDay);
    }

    std::wstring NormalizeOptionalBool(const std::wstring& value, const wchar_t* fieldName, std::wstring& error)
    {
        const std::wstring trimmed = TrimWhitespace(value);
        if (trimmed.empty())
        {
            return L"";
        }

        bool parsed = false;
        if (!TryParseStrictBool(trimmed, parsed))
        {
            error = std::wstring(fieldName) + L" must be true or false.";
            return L"";
        }

        return parsed ? L"true" : L"false";
    }

    std::wstring NormalizeOptionalPercent(const std::wstring& value, const wchar_t* fieldName, std::wstring& error)
    {
        const std::wstring trimmed = TrimWhitespace(value);
        if (trimmed.empty())
        {
            return L"";
        }

        float parsed = 0.0f;
        if (!TryParseStrictFloat(trimmed, parsed))
        {
            error = std::wstring(fieldName) + L" must be a number between 0 and 100.";
            return L"";
        }

        if (parsed < 0.0f || parsed > 100.0f)
        {
            error = std::wstring(fieldName) + L" must be between 0 and 100.";
            return L"";
        }

        return trimmed;
    }

    std::wstring NormalizeOptionalPositiveInt(const std::wstring& value, const wchar_t* fieldName, std::wstring& error)
    {
        const std::wstring trimmed = TrimWhitespace(value);
        if (trimmed.empty())
        {
            return L"";
        }

        int parsed = 0;
        if (!TryParseStrictInt(trimmed, parsed) || parsed <= 0)
        {
            error = std::wstring(fieldName) + L" must be an integer greater than 0.";
            return L"";
        }

        return std::to_wstring(parsed);
    }

    bool TryReadLegacyOriginalSsid(const std::wstring& path, std::wstring& ssid)
    {
        ssid.clear();
        TextFileContent content;
        if (!LoadTextFile(path, content) || !content.exists)
        {
            return false;
        }

        std::wistringstream input(content.text);
        std::wstring line;
        if (!std::getline(input, line))
        {
            return false;
        }

        ssid = TrimWhitespace(line);
        return !ssid.empty();
    }

    void ParseConfigText(
        const std::wstring& text,
        solock_configurator::ConfigSnapshot& snapshot,
        std::wstring& preservedState)
    {
        snapshot.customBlocks.clear();
        preservedState.clear();

        std::wstring currentSection;
        bool inCustomBlockSection = false;
        solock_configurator::CustomBlockEntry currentBlock;
        auto finalizeCustomBlock = [&]()
        {
            if (!TrimWhitespace(currentBlock.start).empty() ||
                !TrimWhitespace(currentBlock.durationMinutes).empty() ||
                !TrimWhitespace(currentBlock.intervalMinutes).empty() ||
                !TrimWhitespace(currentBlock.repeatCount).empty())
            {
                snapshot.customBlocks.push_back(currentBlock);
            }
            currentBlock = {};
        };

        std::wistringstream input(text);
        std::wstring line;
        while (std::getline(input, line))
        {
            if (!line.empty() && line.back() == L'\r')
            {
                line.pop_back();
            }

            const std::wstring trimmed = TrimWhitespace(line);
            if (trimmed.empty() || trimmed.front() == L';' || trimmed.front() == L'#')
            {
                continue;
            }

            std::wstring sectionName;
            if (TryParseIniSectionHeader(trimmed, sectionName))
            {
                if (inCustomBlockSection)
                {
                    finalizeCustomBlock();
                }

                currentSection = sectionName;
                inCustomBlockSection = EqualsIgnoreCase(sectionName, L"custom_block");
                continue;
            }

            std::wstring key;
            std::wstring value;
            if (!TryParseIniKeyValue(line, key, value))
            {
                continue;
            }

            if (inCustomBlockSection)
            {
                if (EqualsIgnoreCase(key, L"start"))
                {
                    currentBlock.start = value;
                }
                else if (EqualsIgnoreCase(key, L"duration_minutes"))
                {
                    currentBlock.durationMinutes = value;
                }
                else if (EqualsIgnoreCase(key, L"interval_minutes"))
                {
                    currentBlock.intervalMinutes = value;
                }
                else if (EqualsIgnoreCase(key, L"repeat_count"))
                {
                    currentBlock.repeatCount = value;
                }
            }
            else if (EqualsIgnoreCase(currentSection, L"state"))
            {
                if (EqualsIgnoreCase(key, L"original_hotspot_ssid"))
                {
                    snapshot.originalHotspotSsid = value;
                    preservedState = value;
                }
            }
            else if (EqualsIgnoreCase(currentSection, L"schedule"))
            {
                if (EqualsIgnoreCase(key, L"enable_evening_hotspot"))
                {
                    snapshot.enableEveningHotspot = value;
                }
                else if (EqualsIgnoreCase(key, L"midday_shutdown_start"))
                {
                    snapshot.middayShutdownStart = value;
                }
                else if (EqualsIgnoreCase(key, L"midday_shutdown_end"))
                {
                    snapshot.middayShutdownEnd = value;
                }
                else if (EqualsIgnoreCase(key, L"evening_hotspot_start"))
                {
                    snapshot.eveningHotspotStart = value;
                }
                else if (EqualsIgnoreCase(key, L"evening_shutdown_start"))
                {
                    snapshot.eveningShutdownStart = value;
                }
            }
            else if (EqualsIgnoreCase(currentSection, L"volume"))
            {
                if (EqualsIgnoreCase(key, L"normal_percent"))
                {
                    snapshot.normalPercent = value;
                }
                else if (EqualsIgnoreCase(key, L"reduced_percent"))
                {
                    snapshot.reducedPercent = value;
                }
            }
        }

        if (inCustomBlockSection)
        {
            finalizeCustomBlock();
        }
    }

    bool EnsureStateDirectoryExists(const std::wstring& dir)
    {
        if (dir.empty())
        {
            return false;
        }

        if (::CreateDirectoryW(dir.c_str(), nullptr) != FALSE)
        {
            return true;
        }

        return ::GetLastError() == ERROR_ALREADY_EXISTS;
    }

    bool NormalizeSnapshot(
        const solock_configurator::ConfigSnapshot& source,
        const std::wstring& preservedState,
        solock_configurator::ConfigSnapshot& normalized,
        std::wstring& error)
    {
        error.clear();
        normalized = {};
        normalized.configFilePath = source.configFilePath;
        normalized.originalHotspotSsid = TrimWhitespace(preservedState.empty() ? source.originalHotspotSsid : preservedState);

        normalized.enableEveningHotspot = NormalizeOptionalBool(
            source.enableEveningHotspot,
            L"enable_evening_hotspot",
            error);
        if (!error.empty())
        {
            return false;
        }

        normalized.middayShutdownStart = NormalizeOptionalMinuteOfDay(
            source.middayShutdownStart,
            L"midday_shutdown_start",
            error);
        if (!error.empty())
        {
            return false;
        }

        normalized.middayShutdownEnd = NormalizeOptionalMinuteOfDay(
            source.middayShutdownEnd,
            L"midday_shutdown_end",
            error);
        if (!error.empty())
        {
            return false;
        }

        normalized.eveningHotspotStart = NormalizeOptionalMinuteOfDay(
            source.eveningHotspotStart,
            L"evening_hotspot_start",
            error);
        if (!error.empty())
        {
            return false;
        }

        normalized.eveningShutdownStart = NormalizeOptionalMinuteOfDay(
            source.eveningShutdownStart,
            L"evening_shutdown_start",
            error);
        if (!error.empty())
        {
            return false;
        }

        normalized.normalPercent = NormalizeOptionalPercent(
            source.normalPercent,
            L"normal_percent",
            error);
        if (!error.empty())
        {
            return false;
        }

        normalized.reducedPercent = NormalizeOptionalPercent(
            source.reducedPercent,
            L"reduced_percent",
            error);
        if (!error.empty())
        {
            return false;
        }

        normalized.customBlocks.reserve(source.customBlocks.size());
        for (size_t i = 0; i < source.customBlocks.size(); ++i)
        {
            const auto& block = source.customBlocks[i];
            solock_configurator::CustomBlockEntry normalizedBlock;

            const std::wstring startValue = TrimWhitespace(block.start);
            if (startValue.empty())
            {
                error = L"custom_block line " + std::to_wstring(i + 1) + L" requires start.";
                return false;
            }

            {
                const std::wstring field = L"custom_block start on line " + std::to_wstring(i + 1);
                normalizedBlock.start = NormalizeOptionalMinuteOfDay(startValue, field.c_str(), error);
                if (!error.empty())
                {
                    return false;
                }
            }

            {
                const std::wstring field = L"custom_block duration_minutes on line " + std::to_wstring(i + 1);
                normalizedBlock.durationMinutes = NormalizeOptionalPositiveInt(block.durationMinutes, field.c_str(), error);
                if (!error.empty())
                {
                    return false;
                }
            }

            {
                const std::wstring field = L"custom_block interval_minutes on line " + std::to_wstring(i + 1);
                normalizedBlock.intervalMinutes = NormalizeOptionalPositiveInt(block.intervalMinutes, field.c_str(), error);
                if (!error.empty())
                {
                    return false;
                }
            }

            {
                const std::wstring field = L"custom_block repeat_count on line " + std::to_wstring(i + 1);
                normalizedBlock.repeatCount = NormalizeOptionalPositiveInt(block.repeatCount, field.c_str(), error);
                if (!error.empty())
                {
                    return false;
                }
            }

            normalized.customBlocks.push_back(std::move(normalizedBlock));
        }

        return true;
    }

    std::wstring BuildConfigText(const solock_configurator::ConfigSnapshot& snapshot, const std::wstring& newline)
    {
        std::wstring text;
        const std::wstring lineEnding = newline.empty() ? L"\r\n" : newline;

        auto appendLine = [&](const std::wstring& line)
        {
            text += line;
            text += lineEnding;
        };

        appendLine(L"[state]");
        appendLine(L"original_hotspot_ssid=" + snapshot.originalHotspotSsid);
        appendLine(L"");

        appendLine(L"[schedule]");
        appendLine(L"enable_evening_hotspot=" + snapshot.enableEveningHotspot);
        appendLine(L"midday_shutdown_start=" + snapshot.middayShutdownStart);
        appendLine(L"midday_shutdown_end=" + snapshot.middayShutdownEnd);
        appendLine(L"evening_hotspot_start=" + snapshot.eveningHotspotStart);
        appendLine(L"evening_shutdown_start=" + snapshot.eveningShutdownStart);
        appendLine(L"");

        appendLine(L"[volume]");
        appendLine(L"normal_percent=" + snapshot.normalPercent);
        appendLine(L"reduced_percent=" + snapshot.reducedPercent);

        for (const auto& block : snapshot.customBlocks)
        {
            appendLine(L"");
            appendLine(L"[custom_block]");
            appendLine(L"start=" + block.start);
            appendLine(L"duration_minutes=" + block.durationMinutes);
            appendLine(L"interval_minutes=" + block.intervalMinutes);
            appendLine(L"repeat_count=" + block.repeatCount);
        }

        return text;
    }
}

namespace solock_configurator
{
    std::wstring ConfigRepository::GetStateDirectoryPath()
    {
        PWSTR rawPath = nullptr;
        std::wstring result;

        if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &rawPath)) && rawPath != nullptr)
        {
            result = rawPath;
            result += L"\\Solock";
            ::CoTaskMemFree(rawPath);
        }

        return result;
    }

    std::wstring ConfigRepository::GetConfigFilePath()
    {
        const std::wstring dir = GetStateDirectoryPath();
        if (dir.empty())
        {
            return L"";
        }

        return BuildFilePath(dir, kConfigFileName);
    }

    bool ConfigRepository::Load(ConfigSnapshot& snapshot, std::wstring& error) const
    {
        error.clear();
        snapshot = {};
        snapshot.configFilePath = GetConfigFilePath();

        const std::wstring stateDir = GetStateDirectoryPath();
        if (stateDir.empty())
        {
            error = L"Unable to resolve %LocalAppData%\\Solock.";
            return false;
        }

        const std::wstring preferredPath = BuildFilePath(stateDir, kConfigFileName);
        const std::wstring legacyPath = BuildFilePath(stateDir, kLegacyConfigFileName);
        const std::wstring readPath = ResolveConfigFilePathForRead(preferredPath, legacyPath);

        TextFileContent content;
        if (!readPath.empty() && !LoadTextFile(readPath, content))
        {
            error = L"Unable to read " + readPath + L".";
            return false;
        }

        std::wstring preservedState;
        if (content.exists)
        {
            ParseConfigText(content.text, snapshot, preservedState);
            snapshot.configFilePath = preferredPath;
        }

        if (preservedState.empty())
        {
            const std::wstring legacyStatePath = BuildFilePath(stateDir, kLegacyStateFileName);
            std::wstring legacySsid;
            if (TryReadLegacyOriginalSsid(legacyStatePath, legacySsid))
            {
                snapshot.originalHotspotSsid = legacySsid;
            }
        }

        if (snapshot.configFilePath.empty())
        {
            snapshot.configFilePath = preferredPath;
        }

        return true;
    }

    bool ConfigRepository::Save(const ConfigSnapshot& snapshot, std::wstring& error) const
    {
        error.clear();

        const std::wstring stateDir = GetStateDirectoryPath();
        if (stateDir.empty())
        {
            error = L"Unable to resolve %LocalAppData%\\Solock.";
            return false;
        }

        if (!EnsureStateDirectoryExists(stateDir))
        {
            error = L"Unable to create %LocalAppData%\\Solock.";
            return false;
        }

        const std::wstring preferredPath = BuildFilePath(stateDir, kConfigFileName);
        const std::wstring legacyPath = BuildFilePath(stateDir, kLegacyConfigFileName);
        if (!EnsurePreferredConfigFileAvailable(preferredPath, legacyPath))
        {
            error = L"Unable to prepare config.cfg.";
            return false;
        }

        TextFileContent existingContent;
        if (!LoadTextFile(preferredPath, existingContent))
        {
            error = L"Unable to read existing config.cfg.";
            return false;
        }

        std::wstring preservedState;
        if (existingContent.exists)
        {
            ConfigSnapshot existingSnapshot;
            ParseConfigText(existingContent.text, existingSnapshot, preservedState);
        }

        if (preservedState.empty())
        {
            const std::wstring legacyStatePath = BuildFilePath(stateDir, kLegacyStateFileName);
            std::wstring legacySsid;
            if (TryReadLegacyOriginalSsid(legacyStatePath, legacySsid))
            {
                preservedState = legacySsid;
            }
        }

        ConfigSnapshot normalizedSnapshot;
        if (!NormalizeSnapshot(snapshot, preservedState, normalizedSnapshot, error))
        {
            return false;
        }

        const std::wstring text = BuildConfigText(normalizedSnapshot, existingContent.newline);
        const TextEncoding encoding = existingContent.exists ? existingContent.encoding : TextEncoding::Utf8Bom;
        const std::wstring tempPath = preferredPath + L".tmp";
        if (!SaveTextFile(tempPath, encoding, text))
        {
            error = L"Unable to write temporary config file.";
            return false;
        }

        if (::MoveFileExW(tempPath.c_str(), preferredPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == FALSE)
        {
            ::DeleteFileW(tempPath.c_str());
            error = L"Unable to replace config.cfg.";
            return false;
        }

        const std::wstring legacyStatePath = BuildFilePath(stateDir, kLegacyStateFileName);
        if (!legacyStatePath.empty())
        {
            ::DeleteFileW(legacyStatePath.c_str());
        }

        return true;
    }
}
