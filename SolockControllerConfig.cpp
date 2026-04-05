#define _WIN32_DCOM
#include "SolockControllerInternal.h"

#include <ShlObj.h>

#include <algorithm>
#include <cwctype>
#include <fstream>
#include <sstream>

#pragma comment(lib, "shell32.lib")

namespace
{
    using solock::internal::EqualsIgnoreCase;

    constexpr wchar_t kConfigFileName[] = L"config.cfg";
    constexpr wchar_t kLegacyHotspotAndBlockConfigFileName[] = L"hotspot_and_block.ini";

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

    struct RawCustomBlockConfig
    {
        std::wstring start;
        std::wstring durationMinutes;
        std::wstring intervalMinutes;
        std::wstring repeatCount;

        bool HasAnyValue() const
        {
            return !start.empty() || !durationMinutes.empty() || !intervalMinutes.empty() || !repeatCount.empty();
        }
    };

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

    bool EnsurePreferredConfigFileAvailable(
        const std::wstring& preferredPath,
        const std::wstring& legacyPath)
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

    std::wstring ResolveConfigFilePathForRead(
        const std::wstring& preferredPath,
        const std::wstring& legacyPath)
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

    bool TryDecodeMultiByteString(
        const std::string& input,
        const UINT codePage,
        const DWORD flags,
        std::wstring& output)
    {
        output.clear();
        if (input.empty())
        {
            return true;
        }

        const int needed = ::MultiByteToWideChar(
            codePage,
            flags,
            input.data(),
            static_cast<int>(input.size()),
            nullptr,
            0);
        if (needed <= 0)
        {
            return false;
        }

        output.resize(static_cast<size_t>(needed));
        const int written = ::MultiByteToWideChar(
            codePage,
            flags,
            input.data(),
            static_cast<int>(input.size()),
            output.data(),
            needed);
        if (written != needed)
        {
            output.clear();
            return false;
        }

        return true;
    }

    bool TryEncodeWideString(
        const std::wstring& input,
        const UINT codePage,
        std::string& output)
    {
        output.clear();
        if (input.empty())
        {
            return true;
        }

        const int needed = ::WideCharToMultiByte(
            codePage,
            0,
            input.data(),
            static_cast<int>(input.size()),
            nullptr,
            0,
            nullptr,
            nullptr);
        if (needed <= 0)
        {
            return false;
        }

        output.resize(static_cast<size_t>(needed));
        const int written = ::WideCharToMultiByte(
            codePage,
            0,
            input.data(),
            static_cast<int>(input.size()),
            output.data(),
            needed,
            nullptr,
            nullptr);
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

        const std::string bytes(
            (std::istreambuf_iterator<char>(input)),
            std::istreambuf_iterator<char>());

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
            content.text.reserve((bytes.size() - 2) / 2);
            for (size_t i = 2; i + 1 < bytes.size(); i += 2)
            {
                const unsigned char low = static_cast<unsigned char>(bytes[i]);
                const unsigned char high = static_cast<unsigned char>(bytes[i + 1]);
                content.text.push_back(static_cast<wchar_t>(low | (high << 8)));
            }
        }
        else if (
            bytes.size() >= 2 &&
            static_cast<unsigned char>(bytes[0]) == 0xFE &&
            static_cast<unsigned char>(bytes[1]) == 0xFF)
        {
            content.encoding = TextEncoding::Utf16Be;
            content.text.reserve((bytes.size() - 2) / 2);
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

    bool SaveTextFile(
        const std::wstring& path,
        const TextEncoding encoding,
        const std::wstring& text)
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
        {
            if (!TryEncodeWideString(text, CP_UTF8, bytes))
            {
                return false;
            }

            if (encoding == TextEncoding::Utf8Bom)
            {
                bytes.insert(bytes.begin(), { static_cast<char>(0xEF), static_cast<char>(0xBB), static_cast<char>(0xBF) });
            }
            break;
        }
        case TextEncoding::Utf16Le:
        {
            bytes.reserve(2 + text.size() * 2);
            bytes.push_back(static_cast<char>(0xFF));
            bytes.push_back(static_cast<char>(0xFE));
            for (const wchar_t ch : text)
            {
                bytes.push_back(static_cast<char>(ch & 0xFF));
                bytes.push_back(static_cast<char>((ch >> 8) & 0xFF));
            }
            break;
        }
        case TextEncoding::Utf16Be:
        {
            bytes.reserve(2 + text.size() * 2);
            bytes.push_back(static_cast<char>(0xFE));
            bytes.push_back(static_cast<char>(0xFF));
            for (const wchar_t ch : text)
            {
                bytes.push_back(static_cast<char>((ch >> 8) & 0xFF));
                bytes.push_back(static_cast<char>(ch & 0xFF));
            }
            break;
        }
        case TextEncoding::Ansi:
        {
            if (!TryEncodeWideString(text, CP_ACP, bytes))
            {
                return false;
            }
            break;
        }
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

    std::vector<std::wstring> SplitLines(const std::wstring& text)
    {
        std::vector<std::wstring> lines;
        size_t start = 0;
        while (start < text.size())
        {
            size_t end = text.find(L'\n', start);
            if (end == std::wstring::npos)
            {
                end = text.size();
            }

            std::wstring line = text.substr(start, end - start);
            if (!line.empty() && line.back() == L'\r')
            {
                line.pop_back();
            }
            lines.push_back(std::move(line));

            if (end == text.size())
            {
                break;
            }

            start = end + 1;
        }

        return lines;
    }

    std::wstring JoinLines(
        const std::vector<std::wstring>& lines,
        const std::wstring& newline)
    {
        std::wstring joined;
        for (size_t i = 0; i < lines.size(); ++i)
        {
            if (i > 0)
            {
                joined += newline;
            }

            joined += lines[i];
        }

        if (!joined.empty())
        {
            joined += newline;
        }

        return joined;
    }

    bool TryParseIniSectionHeader(
        const std::wstring& line,
        std::wstring& sectionName)
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

    bool TryParseIniKeyValue(
        const std::wstring& line,
        std::wstring& key,
        std::wstring& value)
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

    bool TryReadIniValue(
        const std::wstring& text,
        const wchar_t* section,
        const wchar_t* key,
        std::wstring& value)
    {
        value.clear();
        if (section == nullptr || key == nullptr)
        {
            return false;
        }

        std::wstring currentSection;
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

            std::wstring parsedSection;
            if (TryParseIniSectionHeader(trimmed, parsedSection))
            {
                currentSection = parsedSection;
                continue;
            }

            if (!EqualsIgnoreCase(currentSection, section))
            {
                continue;
            }

            std::wstring parsedKey;
            std::wstring parsedValue;
            if (TryParseIniKeyValue(line, parsedKey, parsedValue) &&
                EqualsIgnoreCase(parsedKey, key))
            {
                value = parsedValue;
                return !value.empty();
            }
        }

        return false;
    }

    bool UpdateIniValue(
        const std::wstring& path,
        const wchar_t* section,
        const wchar_t* key,
        const std::wstring* value)
    {
        if (path.empty() || section == nullptr || key == nullptr)
        {
            return false;
        }

        TextFileContent content;
        if (!LoadTextFile(path, content))
        {
            return false;
        }

        std::vector<std::wstring> lines = SplitLines(content.text);
        bool sectionFound = false;
        size_t sectionStart = 0;
        size_t sectionEnd = lines.size();

        for (size_t i = 0; i < lines.size(); ++i)
        {
            std::wstring parsedSection;
            if (!TryParseIniSectionHeader(lines[i], parsedSection))
            {
                continue;
            }

            if (!sectionFound && EqualsIgnoreCase(parsedSection, section))
            {
                sectionFound = true;
                sectionStart = i;
                sectionEnd = lines.size();
                continue;
            }

            if (sectionFound)
            {
                sectionEnd = i;
                break;
            }
        }

        std::vector<size_t> keyLines;
        if (sectionFound)
        {
            for (size_t i = sectionStart + 1; i < sectionEnd; ++i)
            {
                std::wstring parsedKey;
                std::wstring parsedValue;
                if (TryParseIniKeyValue(lines[i], parsedKey, parsedValue) &&
                    EqualsIgnoreCase(parsedKey, key))
                {
                    keyLines.push_back(i);
                }
            }
        }

        if (value != nullptr)
        {
            const std::wstring replacementLine = std::wstring(key) + L"=" + *value;
            if (!keyLines.empty())
            {
                lines[keyLines.front()] = replacementLine;
                for (size_t index = keyLines.size(); index > 1; --index)
                {
                    lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(keyLines[index - 1]));
                }
            }
            else if (sectionFound)
            {
                lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(sectionEnd), replacementLine);
            }
            else
            {
                if (!lines.empty() && !TrimWhitespace(lines.back()).empty())
                {
                    lines.push_back(L"");
                }

                lines.push_back(std::wstring(L"[") + section + L"]");
                lines.push_back(replacementLine);
            }
        }
        else if (!keyLines.empty())
        {
            for (size_t index = keyLines.size(); index > 0; --index)
            {
                lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(keyLines[index - 1]));
            }
        }

        if (!content.exists)
        {
            content.encoding = TextEncoding::Utf8Bom;
            content.newline = L"\r\n";
        }

        return SaveTextFile(path, content.encoding, JoinLines(lines, content.newline));
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
}

std::wstring SolockController::GetStateDirectoryPath()
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

std::wstring SolockController::GetLegacyOriginalSsidStateFilePath()
{
    const std::wstring dir = GetStateDirectoryPath();
    if (dir.empty())
    {
        return L"";
    }

    return dir + L"\\original_hotspot_ssid.txt";
}

std::wstring SolockController::GetConfigFilePath()
{
    const std::wstring dir = GetStateDirectoryPath();
    if (dir.empty())
    {
        return L"";
    }

    return BuildFilePath(dir, kConfigFileName);
}

bool SolockController::EnsureStateDirectoryExists()
{
    const std::wstring dir = GetStateDirectoryPath();
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

bool SolockController::ClearOriginalSsid()
{
    const std::wstring dir = GetStateDirectoryPath();
    const std::wstring path = BuildFilePath(dir, kConfigFileName);
    const std::wstring legacyPath = BuildFilePath(dir, kLegacyHotspotAndBlockConfigFileName);
    if (path.empty())
    {
        return false;
    }

    if (!EnsurePreferredConfigFileAvailable(path, legacyPath))
    {
        return false;
    }

    bool updated = true;
    TextFileContent content;
    if (LoadTextFile(path, content) && content.exists)
    {
        updated = UpdateIniValue(path, L"state", L"original_hotspot_ssid", nullptr);
    }

    const std::wstring legacyStatePath = GetLegacyOriginalSsidStateFilePath();
    if (!legacyStatePath.empty() && ::DeleteFileW(legacyStatePath.c_str()) == FALSE)
    {
        const DWORD error = ::GetLastError();
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)
        {
            updated = false;
        }
    }

    return updated;
}

bool SolockController::SaveOriginalSsid(const std::wstring& ssid)
{
    if (ssid.empty())
    {
        return false;
    }

    if (!EnsureStateDirectoryExists())
    {
        return false;
    }

    const std::wstring dir = GetStateDirectoryPath();
    const std::wstring path = BuildFilePath(dir, kConfigFileName);
    const std::wstring legacyPath = BuildFilePath(dir, kLegacyHotspotAndBlockConfigFileName);
    if (path.empty())
    {
        return false;
    }

    if (!EnsurePreferredConfigFileAvailable(path, legacyPath))
    {
        return false;
    }

    if (!UpdateIniValue(path, L"state", L"original_hotspot_ssid", &ssid))
    {
        return false;
    }

    const std::wstring legacyStatePath = GetLegacyOriginalSsidStateFilePath();
    if (!legacyStatePath.empty())
    {
        ::DeleteFileW(legacyStatePath.c_str());
    }

    return true;
}

bool SolockController::TryLoadOriginalSsid(std::wstring& ssid)
{
    ssid.clear();

    const std::wstring dir = GetStateDirectoryPath();
    const std::wstring path = BuildFilePath(dir, kConfigFileName);
    const std::wstring legacyPath = BuildFilePath(dir, kLegacyHotspotAndBlockConfigFileName);
    const std::wstring readPath = ResolveConfigFilePathForRead(path, legacyPath);
    if (!readPath.empty())
    {
        TextFileContent content;
        if (LoadTextFile(readPath, content) &&
            content.exists &&
            TryReadIniValue(content.text, L"state", L"original_hotspot_ssid", ssid) &&
            !ssid.empty())
        {
            return true;
        }
    }

    const std::wstring legacyStatePath = GetLegacyOriginalSsidStateFilePath();
    if (legacyStatePath.empty())
    {
        return false;
    }

    TextFileContent legacyContent;
    if (!LoadTextFile(legacyStatePath, legacyContent) || !legacyContent.exists)
    {
        return false;
    }

    const std::vector<std::wstring> lines = SplitLines(legacyContent.text);
    if (lines.empty())
    {
        return false;
    }

    ssid = TrimWhitespace(lines.front());
    if (ssid.empty())
    {
        return false;
    }

    SaveOriginalSsid(ssid);
    ::DeleteFileW(legacyStatePath.c_str());
    return true;
}

SolockController::ExternalOverrides SolockController::LoadExternalOverrides()
{
    ExternalOverrides overrides;
    const std::wstring dir = GetStateDirectoryPath();
    const std::wstring path = BuildFilePath(dir, kConfigFileName);
    const std::wstring legacyPath = BuildFilePath(dir, kLegacyHotspotAndBlockConfigFileName);
    const std::wstring readPath = ResolveConfigFilePathForRead(path, legacyPath);
    if (readPath.empty())
    {
        return overrides;
    }

    TextFileContent content;
    if (!LoadTextFile(readPath, content) || !content.exists)
    {
        return overrides;
    }

    RawCustomBlockConfig rawBlock;
    std::wstring currentSection;
    bool inCustomBlockSection = false;
    auto finalizeCustomBlock = [&]()
    {
        if (!rawBlock.HasAnyValue())
        {
            return;
        }

        CustomBlockWindow block;
        int parsedStartMinutesOfDay = 0;
        if (TryParseMinuteOfDay(rawBlock.start, parsedStartMinutesOfDay))
        {
            block.hasStart = true;
            block.startMinutesOfDay = parsedStartMinutesOfDay;
        }

        int parsedDurationMinutes = 0;
        if (TryParseStrictInt(rawBlock.durationMinutes, parsedDurationMinutes) && parsedDurationMinutes > 0)
        {
            block.hasCustomBlockDurationMinutes = true;
            block.customBlockDurationMinutes = parsedDurationMinutes;
        }

        int parsedIntervalMinutes = 0;
        if (TryParseStrictInt(rawBlock.intervalMinutes, parsedIntervalMinutes) && parsedIntervalMinutes > 0)
        {
            block.hasCustomBlockIntervalMinutes = true;
            block.customBlockIntervalMinutes = parsedIntervalMinutes;
        }

        int parsedRepeatCount = 0;
        if (TryParseStrictInt(rawBlock.repeatCount, parsedRepeatCount) && parsedRepeatCount > 0)
        {
            block.hasCustomBlockRepeatCount = true;
            block.customBlockRepeatCount = parsedRepeatCount;
        }

        block.signature =
            L"start=" + rawBlock.start +
            L"|duration=" + rawBlock.durationMinutes +
            L"|interval=" + rawBlock.intervalMinutes +
            L"|repeat=" + rawBlock.repeatCount;

        overrides.customBlocks.push_back(std::move(block));
    };

    std::wistringstream input(content.text);
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
                rawBlock = RawCustomBlockConfig{};
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
                rawBlock.start = value;
            }
            else if (EqualsIgnoreCase(key, L"duration_minutes"))
            {
                rawBlock.durationMinutes = value;
            }
            else if (EqualsIgnoreCase(key, L"interval_minutes"))
            {
                rawBlock.intervalMinutes = value;
            }
            else if (EqualsIgnoreCase(key, L"repeat_count"))
            {
                rawBlock.repeatCount = value;
            }
        }
        else if (EqualsIgnoreCase(currentSection, L"schedule"))
        {
            int parsedMinuteOfDay = 0;
            bool parsedFlag = false;
            if (EqualsIgnoreCase(key, L"midday_shutdown_start") &&
                TryParseMinuteOfDay(value, parsedMinuteOfDay))
            {
                overrides.hasMiddayShutdownStartMinutesOfDay = true;
                overrides.middayShutdownStartMinutesOfDay = parsedMinuteOfDay;
            }
            else if (EqualsIgnoreCase(key, L"midday_shutdown_end") &&
                TryParseMinuteOfDay(value, parsedMinuteOfDay))
            {
                overrides.hasMiddayShutdownEndMinutesOfDay = true;
                overrides.middayShutdownEndMinutesOfDay = parsedMinuteOfDay;
            }
            else if (EqualsIgnoreCase(key, L"enable_evening_hotspot") &&
                TryParseStrictBool(value, parsedFlag))
            {
                overrides.hasEveningHotspotEnabled = true;
                overrides.eveningHotspotEnabled = parsedFlag;
            }
            else if (EqualsIgnoreCase(key, L"evening_hotspot_start") &&
                TryParseMinuteOfDay(value, parsedMinuteOfDay))
            {
                overrides.hasEveningHotspotStartMinutesOfDay = true;
                overrides.eveningHotspotStartMinutesOfDay = parsedMinuteOfDay;
            }
            else if (EqualsIgnoreCase(key, L"evening_shutdown_start") &&
                TryParseMinuteOfDay(value, parsedMinuteOfDay))
            {
                overrides.hasEveningIdleShutdownStartMinutesOfDay = true;
                overrides.eveningIdleShutdownStartMinutesOfDay = parsedMinuteOfDay;
            }
        }
        else if (EqualsIgnoreCase(currentSection, L"volume"))
        {
            float parsedVolume = 0.0f;
            if (!TryParseStrictFloat(value, parsedVolume))
            {
                continue;
            }

            parsedVolume = std::clamp(parsedVolume, 0.0f, 100.0f);
            if (EqualsIgnoreCase(key, L"normal_percent"))
            {
                overrides.hasNormalVolumePercent = true;
                overrides.normalVolumePercent = parsedVolume;
            }
            else if (EqualsIgnoreCase(key, L"reduced_percent"))
            {
                overrides.hasReducedVolumePercent = true;
                overrides.reducedVolumePercent = parsedVolume;
            }
        }
    }

    if (inCustomBlockSection)
    {
        finalizeCustomBlock();
    }

    for (const auto& block : overrides.customBlocks)
    {
        if (!overrides.customBlockSignature.empty())
        {
            overrides.customBlockSignature += L"||";
        }

        overrides.customBlockSignature += block.signature;
    }

    return overrides;
}
