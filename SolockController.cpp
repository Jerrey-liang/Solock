#define _WIN32_DCOM
#include "SolockController.h"

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Networking.Connectivity.h>
#include <winrt/Windows.Networking.NetworkOperators.h>

#include <Windows.h>
#include <TlHelp32.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <fwpmu.h>
#include <ShlObj.h>
#include <comdef.h>
#include <taskschd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <cwctype>
#include <cwchar>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "windowsapp.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "taskschd.lib")
#pragma comment(lib, "comsuppw.lib")
#pragma comment(lib, "fwpuclnt.lib")

using namespace winrt;
using namespace Windows::Networking::Connectivity;
using namespace Windows::Networking::NetworkOperators;

namespace
{
    constexpr int kTetheringOperationalStateOn = 1;
    constexpr int kMaxMinuteOfDay = 23 * 60 + 59;
    constexpr wchar_t kWfpSessionName[] = L"Solock Dynamic WFP Session";
    constexpr wchar_t kWfpSublayerName[] = L"Solock App Block Sublayer";
    constexpr wchar_t kHotspotAndBlockConfigFileName[] = L"hotspot_and_block.ini";
    constexpr wchar_t kSeewoPrefix[] = L"seewo-";
    constexpr GUID kSolockWfpSublayerKey =
    { 0x618ab011, 0xcb31, 0x4b31, { 0x92, 0xfd, 0x86, 0x40, 0xe0, 0x25, 0x84, 0xfa } };

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
        std::wstring repeatCount;

        bool HasAnyValue() const
        {
            return !start.empty() || !durationMinutes.empty() || !repeatCount.empty();
        }
    };

    int ClampMinuteOfDay(const int hour, const int minute)
    {
        return std::clamp(hour * 60 + minute, 0, kMaxMinuteOfDay);
    }

    class AudioOutputNotificationClient final : public IMMNotificationClient
    {
    public:
        explicit AudioOutputNotificationClient(HANDLE deviceChangeEvent)
            : m_referenceCount(1),
              m_deviceChangeEvent(deviceChangeEvent)
        {
        }

        ULONG STDMETHODCALLTYPE AddRef() override
        {
            return static_cast<ULONG>(::InterlockedIncrement(&m_referenceCount));
        }

        ULONG STDMETHODCALLTYPE Release() override
        {
            const ULONG count = static_cast<ULONG>(::InterlockedDecrement(&m_referenceCount));
            if (count == 0)
            {
                delete this;
            }

            return count;
        }

        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, VOID** object) override
        {
            if (object == nullptr)
            {
                return E_POINTER;
            }

            if (riid == IID_IUnknown || riid == __uuidof(IMMNotificationClient))
            {
                *object = static_cast<IMMNotificationClient*>(this);
                AddRef();
                return S_OK;
            }

            *object = nullptr;
            return E_NOINTERFACE;
        }

        HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override
        {
            SignalDeviceChange();
            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override
        {
            SignalDeviceChange();
            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override
        {
            SignalDeviceChange();
            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole, LPCWSTR) override
        {
            if (flow == eRender)
            {
                SignalDeviceChange();
            }
            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override
        {
            return S_OK;
        }

    private:
        void SignalDeviceChange() const
        {
            if (m_deviceChangeEvent != nullptr)
            {
                ::SetEvent(m_deviceChangeEvent);
            }
        }

        LONG m_referenceCount;
        HANDLE m_deviceChangeEvent;
    };

    std::wstring ToWString(const hstring& value)
    {
        return std::wstring(value.c_str());
    }

    std::wstring ToWString(const std::string& value)
    {
        return std::wstring(value.begin(), value.end());
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

    bool EqualsIgnoreCase(const std::wstring& left, const std::wstring& right);

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

    bool IsTetheringOn(const NetworkOperatorTetheringManager& manager)
    {
        return static_cast<int>(manager.TetheringOperationalState()) == kTetheringOperationalStateOn;
    }

    FWPM_DISPLAY_DATA0 MakeDisplayData(const wchar_t* name, const wchar_t* description = nullptr)
    {
        FWPM_DISPLAY_DATA0 displayData = {};
        displayData.name = const_cast<wchar_t*>(name);
        displayData.description = const_cast<wchar_t*>(description);
        return displayData;
    }

    std::wstring GetFileNameFromPath(const std::wstring& path)
    {
        const size_t pos = path.find_last_of(L"\\/");
        if (pos == std::wstring::npos)
        {
            return path;
        }

        return path.substr(pos + 1);
    }

    std::wstring BuildBlockedAppFilterName(const std::wstring& appPath, const wchar_t* addressFamily)
    {
        return L"Solock block " + GetFileNameFromPath(appPath) + L" " + addressFamily + L" outbound";
    }

    bool EqualsIgnoreCase(const std::wstring& left, const std::wstring& right)
    {
        return _wcsicmp(left.c_str(), right.c_str()) == 0;
    }

    bool ContainsStringIgnoreCase(const std::vector<std::wstring>& values, const std::wstring& expected)
    {
        return std::any_of(values.begin(), values.end(), [&](const std::wstring& value)
        {
            return EqualsIgnoreCase(value, expected);
        });
    }

    bool HaveSamePathSetIgnoreCase(
        const std::vector<std::wstring>& leftValues,
        const std::vector<std::wstring>& rightValues)
    {
        if (leftValues.size() != rightValues.size())
        {
            return false;
        }

        std::vector<std::wstring> left = leftValues;
        std::vector<std::wstring> right = rightValues;
        auto comparer = [](const std::wstring& a, const std::wstring& b)
        {
            return _wcsicmp(a.c_str(), b.c_str()) < 0;
        };

        std::sort(left.begin(), left.end(), comparer);
        std::sort(right.begin(), right.end(), comparer);

        for (size_t i = 0; i < left.size(); ++i)
        {
            if (!EqualsIgnoreCase(left[i], right[i]))
            {
                return false;
            }
        }

        return true;
    }

    bool AddUniquePathIgnoreCase(std::vector<std::wstring>& values, const std::wstring& path)
    {
        if (path.empty() || ContainsStringIgnoreCase(values, path))
        {
            return false;
        }

        values.push_back(path);
        return true;
    }

    bool StartsWithIgnoreCase(const std::wstring& value, const wchar_t* prefix)
    {
        if (prefix == nullptr)
        {
            return false;
        }

        const size_t prefixLength = std::wcslen(prefix);
        return value.size() >= prefixLength &&
            _wcsnicmp(value.c_str(), prefix, prefixLength) == 0;
    }

    std::mt19937& GetRandomGenerator()
    {
        static thread_local std::mt19937 generator(std::random_device{}());
        return generator;
    }

    std::wstring ExtractRandomizableSsidCharacters(const std::wstring& sourceSsid)
    {
        std::wstring seed = sourceSsid;
        if (StartsWithIgnoreCase(sourceSsid, kSeewoPrefix))
        {
            seed = sourceSsid.substr(std::wcslen(kSeewoPrefix));
        }

        std::wstring filtered;
        filtered.reserve(seed.size());
        for (const wchar_t ch : seed)
        {
            if (std::iswalnum(static_cast<wint_t>(ch)))
            {
                filtered.push_back(ch);
            }
        }

        if (!filtered.empty())
        {
            return filtered;
        }

        filtered.reserve(sourceSsid.size());
        for (const wchar_t ch : sourceSsid)
        {
            if (std::iswalnum(static_cast<wint_t>(ch)))
            {
                filtered.push_back(ch);
            }
        }

        return filtered;
    }

    std::wstring PickRandomHotspotToken(const std::wstring& sourceSsid)
    {
        std::wstring characterPool = ExtractRandomizableSsidCharacters(sourceSsid);
        if (characterPool.empty())
        {
            characterPool = L"wifi";
        }

        auto& generator = GetRandomGenerator();
        std::vector<wchar_t> shuffledPool(characterPool.begin(), characterPool.end());
        std::shuffle(shuffledPool.begin(), shuffledPool.end(), generator);

        std::wstring token;
        token.reserve(4);
        const size_t directCount = std::min<size_t>(4, shuffledPool.size());
        for (size_t i = 0; i < directCount; ++i)
        {
            token.push_back(shuffledPool[i]);
        }

        if (!shuffledPool.empty())
        {
            std::uniform_int_distribution<size_t> pickIndex(0, shuffledPool.size() - 1);
            while (token.size() < 4)
            {
                token.push_back(shuffledPool[pickIndex(generator)]);
            }
        }

        std::shuffle(token.begin(), token.end(), generator);
        std::bernoulli_distribution uppercaseSwitch(0.5);
        for (wchar_t& ch : token)
        {
            if (std::iswalpha(static_cast<wint_t>(ch)))
            {
                ch = uppercaseSwitch(generator)
                    ? std::towupper(static_cast<wint_t>(ch))
                    : std::towlower(static_cast<wint_t>(ch));
            }
        }

        return token;
    }

    std::wstring BuildCarrierStyleHotspotAlias(const std::wstring& sourceSsid)
    {
        static const std::vector<std::wstring> prefixes =
        {
            L"CMCC-",
            L"ChinaNet-",
            L"ChinaUnicom-",
            L"CUNET_",
            L"TP-LINK_",
            L"MERCURY_",
            L"H3C_",
            L"Tenda_"
        };

        auto& generator = GetRandomGenerator();
        std::uniform_int_distribution<size_t> pickPrefix(0, prefixes.size() - 1);
        return prefixes[pickPrefix(generator)] + PickRandomHotspotToken(sourceSsid);
    }

#ifdef _DEBUG
    std::wstring FormatStatusCode(const DWORD status)
    {
        wchar_t buffer[16] = {};
        swprintf_s(buffer, L"0x%08lX", status);
        return buffer;
    }

    std::wstring FormatHResult(const HRESULT status)
    {
        wchar_t buffer[16] = {};
        swprintf_s(buffer, L"0x%08lX", static_cast<unsigned long>(status));
        return buffer;
    }

    void DebugLog(const std::wstring& message)
    {
        std::wcout << L"[DEBUG] " << message << std::endl;
    }

    void DebugLogStatus(const wchar_t* step, const DWORD status)
    {
        std::wostringstream out;
        out << L"[WFP] " << step << L" returned "
            << FormatStatusCode(status) << L" (" << status << L")";
        DebugLog(out.str());
    }

    void DebugLogHResult(const wchar_t* step, const HRESULT status)
    {
        std::wostringstream out;
        out << L"[HRESULT] " << step << L" returned "
            << FormatHResult(status) << L" (" << status << L")";
        DebugLog(out.str());
    }
#else
    void DebugLog(const std::wstring&)
    {
    }

    void DebugLogStatus(const wchar_t*, const DWORD)
    {
    }

    void DebugLogHResult(const wchar_t*, const HRESULT)
    {
    }
#endif

    DWORD AddBlockedAppFilter(
        HANDLE engineHandle,
        const GUID& layerKey,
        const wchar_t* filterName,
        FWP_BYTE_BLOB* appId,
        UINT64* filterId)
    {
        FWPM_FILTER_CONDITION0 condition = {};
        condition.fieldKey = FWPM_CONDITION_ALE_APP_ID;
        condition.matchType = FWP_MATCH_EQUAL;
        condition.conditionValue.type = FWP_BYTE_BLOB_TYPE;
        condition.conditionValue.byteBlob = appId;

        FWPM_FILTER0 filter = {};
        filter.displayData = MakeDisplayData(filterName);
        filter.layerKey = layerKey;
        filter.subLayerKey = kSolockWfpSublayerKey;
        filter.weight.type = FWP_EMPTY;
        filter.numFilterConditions = 1;
        filter.filterCondition = &condition;
        filter.action.type = FWP_ACTION_BLOCK;

        return ::FwpmFilterAdd0(engineHandle, &filter, nullptr, filterId);
    }

    bool EnableShutdownPrivilege()
    {
        HANDLE token = nullptr;
        if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token))
        {
            return false;
        }

        TOKEN_PRIVILEGES tp = {};
        tp.PrivilegeCount = 1;

        if (!::LookupPrivilegeValueW(nullptr, SE_SHUTDOWN_NAME, &tp.Privileges[0].Luid))
        {
            ::CloseHandle(token);
            return false;
        }

        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

        if (!::AdjustTokenPrivileges(token, FALSE, &tp, sizeof(tp), nullptr, nullptr))
        {
            ::CloseHandle(token);
            return false;
        }

        const DWORD ec = ::GetLastError();
        ::CloseHandle(token);
        return ec == ERROR_SUCCESS;
    }

    bool LaunchShutdownExeFallback()
    {
        std::wstring commandLine = L"shutdown.exe /s /f /t 0 /d p:0:0";
        STARTUPINFOW si = {};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi = {};

        const BOOL ok = ::CreateProcessW(
            nullptr,
            commandLine.data(),
            nullptr,
            nullptr,
            FALSE,
            CREATE_NO_WINDOW,
            nullptr,
            nullptr,
            &si,
            &pi);

        if (!ok)
        {
            return false;
        }

        ::CloseHandle(pi.hThread);

        const DWORD waitStatus = ::WaitForSingleObject(pi.hProcess, 15000);
        DWORD exitCode = STILL_ACTIVE;
        const bool gotExitCode = ::GetExitCodeProcess(pi.hProcess, &exitCode) != FALSE;
        ::CloseHandle(pi.hProcess);

        if (waitStatus == WAIT_TIMEOUT)
        {
            return true;
        }

        return waitStatus == WAIT_OBJECT_0 && gotExitCode && exitCode == 0;
    }

    NetworkOperatorTetheringManager CreateTetheringManager()
    {
        const ConnectionProfile profile = NetworkInformation::GetInternetConnectionProfile();
        if (!profile)
        {
            throw std::runtime_error("No internet connection profile was found.");
        }

        const auto capability =
            NetworkOperatorTetheringManager::GetTetheringCapabilityFromConnectionProfile(profile);

        if (capability != TetheringCapability::Enabled)
        {
            throw std::runtime_error("Tethering capability is not enabled.");
        }

        return NetworkOperatorTetheringManager::CreateFromConnectionProfile(profile);
    }

    std::wstring GetUsablePassphrase(const NetworkOperatorTetheringManager& manager)
    {
        std::wstring passphrase = ToWString(manager.GetCurrentAccessPointConfiguration().Passphrase());
        if (passphrase.size() < 8)
        {
            passphrase = L"12345678";
        }
        return passphrase;
    }

    template <typename T>
    void SafeRelease(T*& p)
    {
        if (p != nullptr)
        {
            p->Release();
            p = nullptr;
        }
    }
}

SolockController::SolockController(const Options& options)
    : m_options(options),
      m_eveningIdleLockApplied(false),
      m_eveningHotspotAliasSource(),
      m_eveningHotspotAlias(),
      m_customBlockStates(),
      m_customBlockConfigSignature(),
      m_wfpEngine(nullptr),
      m_blockedAppFiltersInstalled(false),
      m_audioDeviceChangeEvent(nullptr),
      m_audioDeviceEnumerator(nullptr),
      m_audioNotificationClient(nullptr)
{
}

SolockController::~SolockController()
{
    ShutdownAudioVolumeMonitoring();
    CloseWfpEngine();
    ClearKeepSystemAwake();
}

int SolockController::Run()
{
    if (m_options.autoRegisterScheduledTask)
    {
        EnsureStartupTaskRegistered(m_options.scheduledTaskName);
    }

    InitializeAudioVolumeMonitoring();
    return RunWithSchedule();
}

int SolockController::RunAllFeaturesAcceleratedDebug()
{
    struct DebugStepResult
    {
        std::wstring name;
        bool ok;
    };

    std::vector<DebugStepResult> stepResults;
    auto recordStep = [&](const std::wstring& name, const bool ok)
    {
        stepResults.push_back({ name, ok });
        DebugLog(std::wstring(L"[ACCEL] ") + name + L": " + (ok ? L"ok" : L"failed"));
        return ok;
    };

    auto sleepBetweenSteps = [&]()
    {
        const int delayMilliseconds = std::clamp(m_options.debugStepDelayMilliseconds, 0, 5000);
        if (delayMilliseconds > 0)
        {
            ::Sleep(static_cast<DWORD>(delayMilliseconds));
        }
    };

    DebugLog(L"[ACCEL] hotspot-only debug started.");

    ResetEveningHotspotAlias();
    recordStep(L"hotspot / EnsurePreActionHotspot(initial)", EnsurePreActionHotspot());
    sleepBetweenSteps();

    ResetEveningHotspotAlias();
    recordStep(L"hotspot / EnsureEveningHotspotState", EnsureEveningHotspotState());
    sleepBetweenSteps();

    recordStep(L"hotspot / EnsurePreActionHotspot(restore)", EnsurePreActionHotspot());
    ResetEveningHotspotAlias();

    int failureCount = 0;
    std::wostringstream summary;
    summary << L"[ACCEL] hotspot-only debug completed with ";
    for (const auto& step : stepResults)
    {
        if (!step.ok)
        {
            ++failureCount;
        }
    }

    summary << failureCount << L" failure(s) across " << stepResults.size() << L" step(s).";
    if (failureCount > 0)
    {
        summary << L" Failed steps:";
        for (const auto& step : stepResults)
        {
            if (!step.ok)
            {
                summary << L" [" << step.name << L"]";
            }
        }
    }

    DebugLog(summary.str());
    system("pause");
    return failureCount == 0 ? 0 : 40;
}

int SolockController::RunBlockedAppNetworkingDebug()
{
    DebugLog(L"[WFP] standalone debug entry started.");
    const auto beforeProcesses = ResolveRunningBlockedProcesses();
    DebugLogRunningBlockedProcesses(L"before blocking", beforeProcesses);

    if (beforeProcesses.empty())
    {
        DebugLog(L"[WFP] debug run aborted because no matching target process is currently running.");
        return 33;
    }

    const bool startupOk = EnsureTargetAppsNetworkingEnabled();
    DebugLog(std::wstring(L"[WFP] startup unblock check result: ") + (startupOk ? L"success" : L"failure"));
    if (!startupOk)
    {
        return 31;
    }

    const bool ok = EnsureTargetAppsNetworkingBlocked();
    DebugLog(std::wstring(L"[WFP] standalone debug result: ") + (ok ? L"success" : L"failure"));
    const auto afterProcesses = ResolveRunningBlockedProcesses();
    DebugLogRunningBlockedProcesses(L"after blocking", afterProcesses);

#ifdef _DEBUG
    if (ok)
    {
        std::wcout << L"[DEBUG] WFP dynamic filters are active." << std::endl;
        std::wcout << L"[DEBUG] Press Enter to remove the filters and end the debug run." << std::endl;
        std::wstring line;
        std::getline(std::wcin, line);

        const bool cleanupOk = EnsureTargetAppsNetworkingEnabled();
        DebugLog(std::wstring(L"[WFP] debug cleanup unblock result: ") + (cleanupOk ? L"success" : L"failure"));
        return cleanupOk ? 0 : 32;
    }
#endif

    return ok ? 0 : 30;
}

std::chrono::system_clock::time_point SolockController::LocalAtOnSameDay(
    const std::chrono::system_clock::time_point& referenceNow,
    int hour,
    int minute,
    int second)
{
    const std::time_t t = std::chrono::system_clock::to_time_t(referenceNow);

    std::tm localTm = {};
    localtime_s(&localTm, &t);

    localTm.tm_hour = hour;
    localTm.tm_min = minute;
    localTm.tm_sec = second;

    return std::chrono::system_clock::from_time_t(std::mktime(&localTm));
}

SolockController::ScheduleTimes SolockController::GetScheduleTimesFor(
    const std::chrono::system_clock::time_point& now) const
{
    const int middayStartMinuteOfDay =
        ClampMinuteOfDay(m_options.middayShutdownStartHour, m_options.middayShutdownStartMinute);
    const int middayEndMinuteOfDay =
        ClampMinuteOfDay(m_options.middayShutdownEndHour, m_options.middayShutdownEndMinute);
    const int eveningStartMinuteOfDay =
        ClampMinuteOfDay(m_options.eveningPostActionStartHour, m_options.eveningPostActionStartMinute);

    return
    {
        LocalAtOnSameDay(now, middayStartMinuteOfDay / 60, middayStartMinuteOfDay % 60, 0),
        LocalAtOnSameDay(now, middayEndMinuteOfDay / 60, middayEndMinuteOfDay % 60, 0),
        LocalAtOnSameDay(now, eveningStartMinuteOfDay / 60, eveningStartMinuteOfDay % 60, 0)
    };
}

SolockController::Phase SolockController::GetPhaseAt(
    const std::chrono::system_clock::time_point& now,
    const ScheduleTimes& scheduleTimes)
{
    if (now >= scheduleTimes.eveningPostActionStartTime)
    {
        return Phase::EveningPostAction;
    }

    if (now >= scheduleTimes.middayShutdownStartTime &&
        now < scheduleTimes.middayShutdownEndTime)
    {
        return Phase::MiddayIdleShutdown;
    }

    return Phase::ScheduledBlocks;
}

int SolockController::RunWithSchedule()
{
    const int middayStartMinuteOfDay =
        ClampMinuteOfDay(m_options.middayShutdownStartHour, m_options.middayShutdownStartMinute);
    const int middayEndMinuteOfDay =
        ClampMinuteOfDay(m_options.middayShutdownEndHour, m_options.middayShutdownEndMinute);
    const int eveningStartMinuteOfDay =
        ClampMinuteOfDay(m_options.eveningPostActionStartHour, m_options.eveningPostActionStartMinute);
    if (middayEndMinuteOfDay < middayStartMinuteOfDay ||
        eveningStartMinuteOfDay < middayEndMinuteOfDay)
    {
        return 20;
    }

    if (!EnsureTargetAppsNetworkingEnabled())
    {
        return 24;
    }

    const auto now = std::chrono::system_clock::now();
    const auto initialPhase = GetPhaseAt(now, GetScheduleTimesFor(now));

    AssertKeepSystemAwake();
    EnsureAudioVolumeMatchesPhase(initialPhase);

    if (initialPhase == Phase::EveningPostAction)
    {
        EnsureEveningPostActionState();
    }
    else
    {
        if (initialPhase == Phase::ScheduledBlocks)
        {
            EnsureTargetAppsNetworkingMatchesSchedule(now);
            WaitForSystemAndNetworkStability();
        }
        else if (initialPhase == Phase::MiddayIdleShutdown)
        {
            EnsureTargetAppsNetworkingBlocked();
        }

        EnsurePreActionHotspot();
    }

    const auto idleThreshold = std::chrono::minutes(std::max(1, m_options.inactivityThresholdMinutes));
    const int heartbeatSeconds = std::max(1, m_options.heartbeatSeconds);
    while (true)
    {
        const auto currentNow = std::chrono::system_clock::now();
        const auto scheduleTimes = GetScheduleTimesFor(currentNow);
        const auto phase = GetPhaseAt(currentNow, scheduleTimes);

        AssertKeepSystemAwake();
        EnsureAudioVolumeMatchesPhase(phase);

        if (phase != Phase::EveningPostAction)
        {
            m_eveningIdleLockApplied = false;
            ResetEveningHotspotAlias();
        }

        switch (phase)
        {
        case Phase::ScheduledBlocks:
            EnsurePreActionHotspot();
            EnsureTargetAppsNetworkingMatchesSchedule(currentNow);
            break;

        case Phase::MiddayIdleShutdown:
            EnsurePreActionHotspot();
            EnsureTargetAppsNetworkingBlocked();
            if (IsInputIdleForAtLeast(idleThreshold))
            {
                return ShutdownMachineNow() ? 0 : 21;
            }
            break;

        case Phase::EveningPostAction:
            EnsureEveningPostActionState();
            ApplyEveningIdleLockIfNeeded();
            break;
        }

        WaitForHeartbeatOrAudioEvent(heartbeatSeconds);
    }
}

bool SolockController::WaitForSystemAndNetworkStability()
{
    int stable = 0;
    const int maxWait = std::max(1, m_options.startupMaxWaitSeconds);
    const int needStable = std::max(1, m_options.startupStableSeconds);

    for (int waited = 0; waited < maxWait; ++waited)
    {
        if (IsNetworkUsableNow())
        {
            ++stable;
            if (stable >= needStable)
            {
                return true;
            }
        }
        else
        {
            stable = 0;
        }

        ::Sleep(1000);
    }

    return false;
}

bool SolockController::IsNetworkUsableNow() const
{
    try
    {
        const ConnectionProfile profile = NetworkInformation::GetInternetConnectionProfile();
        if (!profile)
        {
            return false;
        }

        return profile.GetNetworkConnectivityLevel() == NetworkConnectivityLevel::InternetAccess;
    }
    catch (...)
    {
        return false;
    }
}

bool SolockController::IsInputIdleForAtLeast(const std::chrono::milliseconds idleThreshold) const
{
    LASTINPUTINFO lastInputInfo = {};
    lastInputInfo.cbSize = sizeof(lastInputInfo);
    if (!::GetLastInputInfo(&lastInputInfo))
    {
        return false;
    }

    const ULONGLONG nowTicks = ::GetTickCount64();
    const ULONGLONG idleMilliseconds = nowTicks - static_cast<ULONGLONG>(lastInputInfo.dwTime);
    return std::chrono::milliseconds(
        static_cast<std::chrono::milliseconds::rep>(idleMilliseconds)) >= idleThreshold;
}

float SolockController::GetDesiredVolumePercentForPhase(const Phase phase) const
{
    if (phase == Phase::MiddayIdleShutdown || phase == Phase::EveningPostAction)
    {
        return std::clamp(m_options.reducedVolumePercent, 0.0f, 100.0f);
    }

    return std::clamp(m_options.normalVolumePercent, 0.0f, 100.0f);
}

bool SolockController::InitializeAudioVolumeMonitoring()
{
    if (m_audioDeviceChangeEvent != nullptr &&
        m_audioDeviceEnumerator != nullptr &&
        m_audioNotificationClient != nullptr)
    {
        return true;
    }

    HANDLE deviceChangeEvent = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (deviceChangeEvent == nullptr)
    {
        return false;
    }

    IMMDeviceEnumerator* enumerator = nullptr;
    const HRESULT createStatus = ::CoCreateInstance(
        __uuidof(MMDeviceEnumerator),
        nullptr,
        CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator),
        reinterpret_cast<void**>(&enumerator));
    if (FAILED(createStatus) || enumerator == nullptr)
    {
        ::CloseHandle(deviceChangeEvent);
        return false;
    }

    IMMNotificationClient* notificationClient = new (std::nothrow) AudioOutputNotificationClient(deviceChangeEvent);
    if (notificationClient == nullptr)
    {
        enumerator->Release();
        ::CloseHandle(deviceChangeEvent);
        return false;
    }

    const HRESULT registerStatus = enumerator->RegisterEndpointNotificationCallback(notificationClient);
    if (FAILED(registerStatus))
    {
        notificationClient->Release();
        enumerator->Release();
        ::CloseHandle(deviceChangeEvent);
        return false;
    }

    m_audioDeviceChangeEvent = deviceChangeEvent;
    m_audioDeviceEnumerator = enumerator;
    m_audioNotificationClient = notificationClient;
    return true;
}

void SolockController::ShutdownAudioVolumeMonitoring()
{
    if (m_audioDeviceEnumerator != nullptr && m_audioNotificationClient != nullptr)
    {
        m_audioDeviceEnumerator->UnregisterEndpointNotificationCallback(m_audioNotificationClient);
    }

    if (m_audioNotificationClient != nullptr)
    {
        m_audioNotificationClient->Release();
        m_audioNotificationClient = nullptr;
    }

    if (m_audioDeviceEnumerator != nullptr)
    {
        m_audioDeviceEnumerator->Release();
        m_audioDeviceEnumerator = nullptr;
    }

    if (m_audioDeviceChangeEvent != nullptr)
    {
        ::CloseHandle(m_audioDeviceChangeEvent);
        m_audioDeviceChangeEvent = nullptr;
    }
}

bool SolockController::EnsureAudioVolumeMatchesPhase(const Phase phase) const
{
    IMMDeviceEnumerator* enumerator = m_audioDeviceEnumerator;
    IMMDeviceEnumerator* localEnumerator = nullptr;
    if (enumerator == nullptr)
    {
        const HRESULT createStatus = ::CoCreateInstance(
            __uuidof(MMDeviceEnumerator),
            nullptr,
            CLSCTX_ALL,
            __uuidof(IMMDeviceEnumerator),
            reinterpret_cast<void**>(&localEnumerator));
        if (FAILED(createStatus) || localEnumerator == nullptr)
        {
            return false;
        }

        enumerator = localEnumerator;
    }

    IMMDevice* device = nullptr;
    const HRESULT deviceStatus = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    if (deviceStatus == E_NOTFOUND || device == nullptr)
    {
        if (device != nullptr)
        {
            device->Release();
        }
        if (localEnumerator != nullptr)
        {
            localEnumerator->Release();
        }
        return true;
    }

    if (FAILED(deviceStatus))
    {
        if (localEnumerator != nullptr)
        {
            localEnumerator->Release();
        }
        return false;
    }

    IAudioEndpointVolume* endpointVolume = nullptr;
    const HRESULT activateStatus = device->Activate(
        __uuidof(IAudioEndpointVolume),
        CLSCTX_ALL,
        nullptr,
        reinterpret_cast<void**>(&endpointVolume));
    device->Release();
    if (FAILED(activateStatus) || endpointVolume == nullptr)
    {
        if (endpointVolume != nullptr)
        {
            endpointVolume->Release();
        }
        if (localEnumerator != nullptr)
        {
            localEnumerator->Release();
        }
        return false;
    }

    const float desiredScalar = GetDesiredVolumePercentForPhase(phase) / 100.0f;
    float currentScalar = 0.0f;
    const HRESULT getVolumeStatus = endpointVolume->GetMasterVolumeLevelScalar(&currentScalar);
    bool ok = SUCCEEDED(getVolumeStatus);
    if (ok && std::fabs(currentScalar - desiredScalar) > 0.01f)
    {
        ok = SUCCEEDED(endpointVolume->SetMasterVolumeLevelScalar(desiredScalar, nullptr));
    }

    endpointVolume->Release();
    if (localEnumerator != nullptr)
    {
        localEnumerator->Release();
    }

    return ok;
}

void SolockController::WaitForHeartbeatOrAudioEvent(const int heartbeatSeconds) const
{
    const DWORD waitMilliseconds = static_cast<DWORD>(std::max(1, heartbeatSeconds) * 1000);
    if (m_audioDeviceChangeEvent != nullptr)
    {
        ::WaitForSingleObject(m_audioDeviceChangeEvent, waitMilliseconds);
        return;
    }

    ::Sleep(waitMilliseconds);
}

bool SolockController::AssertKeepSystemAwake()
{
    return ::SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED) != 0;
}

void SolockController::ClearKeepSystemAwake()
{
    ::SetThreadExecutionState(ES_CONTINUOUS);
}

bool SolockController::ShouldSkipHotspotActions() const
{
#ifdef _DEBUG
    return m_options.debugSkipHotspotActions;
#else
    return false;
#endif
}

bool SolockController::EnsurePreActionHotspot()
{
    if (ShouldSkipHotspotActions())
    {
        DebugLog(L"[HOTSPOT] debug skip enabled; pre-action hotspot step bypassed.");
        return true;
    }

    try
    {
        auto manager = CreateTetheringManager();
        const auto config = manager.GetCurrentAccessPointConfiguration();
        const std::wstring currentSsid = ToWString(config.Ssid());
        DebugLog(L"[HOTSPOT] EnsurePreActionHotspot current SSID: " +
            (currentSsid.empty() ? std::wstring(L"<empty>") : currentSsid));

        std::wstring originalSsid;
        const bool hasOriginalSsid = TryLoadOriginalSsid(originalSsid) && !originalSsid.empty();
        if (hasOriginalSsid && !EqualsIgnoreCase(originalSsid, currentSsid))
        {
            DebugLog(L"[HOTSPOT] restoring original SSID for pre-action phase: " + originalSsid);
            const bool restored = EnsureHotspotOnWithSsid(originalSsid);
            if (restored)
            {
                ClearOriginalSsid();
            }

            return restored;
        }

        if (hasOriginalSsid && EqualsIgnoreCase(originalSsid, currentSsid))
        {
            ClearOriginalSsid();
        }

        DebugLog(L"[HOTSPOT] using current hotspot configuration for pre-action phase.");
        return EnsureHotspotOnWithCurrentConfig();
    }
    catch (const std::exception& ex)
    {
        DebugLog(L"[HOTSPOT] EnsurePreActionHotspot failed: " + ToWString(std::string(ex.what())));
        return false;
    }
    catch (...)
    {
        DebugLog(L"[HOTSPOT] EnsurePreActionHotspot failed with an unknown exception.");
        return false;
    }
}

bool SolockController::EnsureHotspotOnWithCurrentConfig()
{
    if (ShouldSkipHotspotActions())
    {
        DebugLog(L"[HOTSPOT] debug skip enabled; hotspot start with current config bypassed.");
        return true;
    }

    try
    {
        auto manager = CreateTetheringManager();

        if (IsTetheringOn(manager))
        {
            DebugLog(L"[HOTSPOT] hotspot already enabled with current configuration.");
            return true;
        }

        const auto result = manager.StartTetheringAsync().get();
        const int status = static_cast<int>(result.Status());
        DebugLog(L"[HOTSPOT] StartTetheringAsync(current config) status=" + std::to_wstring(status));
        return status == 0 || status == 9;
    }
    catch (const std::exception& ex)
    {
        DebugLog(L"[HOTSPOT] EnsureHotspotOnWithCurrentConfig failed: " + ToWString(std::string(ex.what())));
        return false;
    }
    catch (...)
    {
        DebugLog(L"[HOTSPOT] EnsureHotspotOnWithCurrentConfig failed with an unknown exception.");
        return false;
    }
}

bool SolockController::EnsureHotspotOnWithSsid(const std::wstring& desiredSsid)
{
    if (ShouldSkipHotspotActions())
    {
        DebugLog(L"[HOTSPOT] debug skip enabled; hotspot SSID switch bypassed: " + desiredSsid);
        return true;
    }

    try
    {
        auto manager = CreateTetheringManager();
        const auto currentConfig = manager.GetCurrentAccessPointConfiguration();
        const std::wstring currentSsid = ToWString(currentConfig.Ssid());
        const bool hotspotOn = IsTetheringOn(manager);
        DebugLog(L"[HOTSPOT] EnsureHotspotOnWithSsid current SSID=" +
            (currentSsid.empty() ? std::wstring(L"<empty>") : currentSsid) +
            L", desired SSID=" + desiredSsid);

        // Heartbeats call this repeatedly while the release-mode schedule is active.
        // Reconfiguring a healthy hotspot forces clients to reconnect.
        if (hotspotOn && currentSsid == desiredSsid)
        {
            DebugLog(L"[HOTSPOT] hotspot already enabled with the desired SSID.");
            return true;
        }

        if (currentSsid != desiredSsid)
        {
            NetworkOperatorTetheringAccessPointConfiguration config;
            config.Ssid(desiredSsid);
            config.Passphrase(GetUsablePassphrase(manager));

            manager.ConfigureAccessPointAsync(config).get();
            DebugLog(L"[HOTSPOT] hotspot access point reconfigured.");
        }

        const auto result = manager.StartTetheringAsync().get();
        const int status = static_cast<int>(result.Status());
        DebugLog(L"[HOTSPOT] StartTetheringAsync(desired SSID) status=" + std::to_wstring(status));
        return status == 0 || status == 9;
    }
    catch (const std::exception& ex)
    {
        DebugLog(L"[HOTSPOT] EnsureHotspotOnWithSsid failed: " + ToWString(std::string(ex.what())));
        return false;
    }
    catch (...)
    {
        DebugLog(L"[HOTSPOT] EnsureHotspotOnWithSsid failed with an unknown exception.");
        return false;
    }
}

void SolockController::ResetEveningHotspotAlias()
{
    m_eveningHotspotAliasSource.clear();
    m_eveningHotspotAlias.clear();
}

std::wstring SolockController::GetEveningHotspotAlias(const std::wstring& sourceSsid)
{
    const std::wstring effectiveSource = sourceSsid.empty() ? m_options.postActionSsid : sourceSsid;
    if (m_eveningHotspotAlias.empty() || !EqualsIgnoreCase(m_eveningHotspotAliasSource, effectiveSource))
    {
        m_eveningHotspotAliasSource = effectiveSource;
        m_eveningHotspotAlias = BuildRandomizedHotspotAlias(effectiveSource);
        DebugLog(L"[HOTSPOT] generated evening alias SSID from " +
            (effectiveSource.empty() ? std::wstring(L"<empty>") : effectiveSource) +
            L" to " + m_eveningHotspotAlias);
    }

    return m_eveningHotspotAlias;
}

std::wstring SolockController::BuildRandomizedHotspotAlias(const std::wstring& sourceSsid)
{
    return BuildCarrierStyleHotspotAlias(sourceSsid);
}

bool SolockController::EnsureTargetAppsNetworkingBlocked()
{
    bool hasConfiguredTarget = false;
    for (const auto& processName : m_options.blockedProcessNames)
    {
        if (!processName.empty())
        {
            hasConfiguredTarget = true;
            break;
        }
    }

    if (!hasConfiguredTarget)
    {
        DebugLog(L"[WFP] blockedProcessNames is empty, skipping filter installation.");
        return true;
    }

    const std::vector<RunningBlockedProcess> runningProcesses = ResolveRunningBlockedProcesses();
    DebugLogRunningBlockedProcesses(L"resolved for blocking", runningProcesses);
    std::vector<std::wstring> resolvedAppPaths;
    resolvedAppPaths.reserve(runningProcesses.size());
    for (const auto& process : runningProcesses)
    {
        AddUniquePathIgnoreCase(resolvedAppPaths, process.appPath);
    }

    if (resolvedAppPaths.empty())
    {
        if (m_blockedAppFiltersInstalled && m_wfpEngine != nullptr && !m_installedBlockedAppFilters.empty())
        {
            DebugLog(L"[WFP] no matching running process was found; keeping existing block filters.");
            return true;
        }

        DebugLog(L"[WFP] no matching running process was found for blockedProcessNames.");
        return true;
    }

    if (m_blockedAppFiltersInstalled && m_wfpEngine != nullptr)
    {
        std::vector<std::wstring> installedAppPaths;
        installedAppPaths.reserve(m_installedBlockedAppFilters.size());
        for (const auto& filter : m_installedBlockedAppFilters)
        {
            installedAppPaths.push_back(filter.appPath);
        }

        if (HaveSamePathSetIgnoreCase(installedAppPaths, resolvedAppPaths))
        {
            DebugLog(L"[WFP] block filters already installed for the current target process paths.");
            return true;
        }
    }

    if (m_wfpEngine != nullptr || !m_installedBlockedAppFilters.empty())
    {
        CloseWfpEngine();
    }

    FWPM_SESSION0 session = {};
    auto cleanupOnFailure = [this]()
    {
        CloseWfpEngine();
    };

    DebugLog(L"[WFP] opening dynamic WFP session.");
    session.flags = FWPM_SESSION_FLAG_DYNAMIC;
    session.displayData = MakeDisplayData(kWfpSessionName);

    const DWORD openStatus = ::FwpmEngineOpen0(
        nullptr,
        RPC_C_AUTHN_WINNT,
        nullptr,
        &session,
        &m_wfpEngine);
    DebugLogStatus(L"FwpmEngineOpen0", openStatus);
    if (openStatus != ERROR_SUCCESS)
    {
        return false;
    }

    FWPM_SUBLAYER0 subLayer = {};
    subLayer.subLayerKey = kSolockWfpSublayerKey;
    subLayer.displayData = MakeDisplayData(kWfpSublayerName);
    subLayer.weight = 0x100;

    const DWORD subLayerStatus = ::FwpmSubLayerAdd0(m_wfpEngine, &subLayer, nullptr);
    DebugLogStatus(L"FwpmSubLayerAdd0", subLayerStatus);
    if (subLayerStatus != ERROR_SUCCESS && subLayerStatus != FWP_E_ALREADY_EXISTS)
    {
        cleanupOnFailure();
        return false;
    }

    m_installedBlockedAppFilters.clear();

    for (const auto& appPath : resolvedAppPaths)
    {
        FWP_BYTE_BLOB* appId = nullptr;
        DebugLog(L"[WFP] resolving ALE_APP_ID for " + appPath);
        const DWORD appIdStatus = ::FwpmGetAppIdFromFileName0(appPath.c_str(), &appId);
        DebugLogStatus(L"FwpmGetAppIdFromFileName0", appIdStatus);
        if (appIdStatus != ERROR_SUCCESS || appId == nullptr)
        {
            if (appId != nullptr)
            {
                ::FwpmFreeMemory0(reinterpret_cast<void**>(&appId));
            }
            cleanupOnFailure();
            return false;
        }

        const std::wstring ipv4FilterName = BuildBlockedAppFilterName(appPath, L"IPv4");
        UINT64 ipv4FilterId = 0;
        const DWORD ipv4Status = AddBlockedAppFilter(
            m_wfpEngine,
            FWPM_LAYER_ALE_AUTH_CONNECT_V4,
            ipv4FilterName.c_str(),
            appId,
            &ipv4FilterId);
        DebugLogStatus(L"FwpmFilterAdd0 IPv4", ipv4Status);
        if (ipv4Status != ERROR_SUCCESS)
        {
            ::FwpmFreeMemory0(reinterpret_cast<void**>(&appId));
            cleanupOnFailure();
            return false;
        }

        const std::wstring ipv6FilterName = BuildBlockedAppFilterName(appPath, L"IPv6");
        UINT64 ipv6FilterId = 0;
        const DWORD ipv6Status = AddBlockedAppFilter(
            m_wfpEngine,
            FWPM_LAYER_ALE_AUTH_CONNECT_V6,
            ipv6FilterName.c_str(),
            appId,
            &ipv6FilterId);
        DebugLogStatus(L"FwpmFilterAdd0 IPv6", ipv6Status);
        if (ipv6Status != ERROR_SUCCESS)
        {
            ::FwpmFreeMemory0(reinterpret_cast<void**>(&appId));
            cleanupOnFailure();
            return false;
        }

        ::FwpmFreeMemory0(reinterpret_cast<void**>(&appId));
        m_installedBlockedAppFilters.push_back({ appPath, ipv4FilterId, ipv6FilterId });
    }

    if (m_installedBlockedAppFilters.empty())
    {
        CloseWfpEngine();
        return true;
    }

    m_blockedAppFiltersInstalled = true;

#ifdef _DEBUG
    {
        std::wostringstream out;
        out << L"[WFP] installed block filters for " << m_installedBlockedAppFilters.size() << L" target app(s):";
        for (const auto& filter : m_installedBlockedAppFilters)
        {
            out << L" [" << GetFileNameFromPath(filter.appPath)
                << L" IPv4=" << filter.ipv4FilterId
                << L", IPv6=" << filter.ipv6FilterId << L"]";
        }
        DebugLog(out.str());
    }
#endif

    return true;
}

bool SolockController::EnsureTargetAppsNetworkingEnabled()
{
    if (m_wfpEngine == nullptr && !m_blockedAppFiltersInstalled && m_installedBlockedAppFilters.empty())
    {
        return true;
    }

    DebugLog(L"[WFP] removing dynamic block filters to ensure target apps can network.");
    CloseWfpEngine();
    return m_wfpEngine == nullptr && !m_blockedAppFiltersInstalled && m_installedBlockedAppFilters.empty();
}

std::vector<SolockController::RunningBlockedProcess> SolockController::ResolveRunningBlockedProcesses() const
{
    std::vector<std::wstring> targetProcessNames;
    targetProcessNames.reserve(m_options.blockedProcessNames.size());
    for (const auto& configuredName : m_options.blockedProcessNames)
    {
        if (configuredName.empty())
        {
            continue;
        }

        const std::wstring fileName = GetFileNameFromPath(configuredName);
        if (!fileName.empty() && !ContainsStringIgnoreCase(targetProcessNames, fileName))
        {
            targetProcessNames.push_back(fileName);
        }
    }

    if (targetProcessNames.empty())
    {
        return {};
    }

    HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
    {
        return {};
    }

    std::vector<RunningBlockedProcess> resolvedProcesses;
    PROCESSENTRY32W entry = {};
    entry.dwSize = sizeof(entry);
    if (::Process32FirstW(snapshot, &entry))
    {
        do
        {
            if (!ContainsStringIgnoreCase(targetProcessNames, entry.szExeFile))
            {
                continue;
            }

            HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
            if (process == nullptr)
            {
                continue;
            }

            std::wstring path(32768, L'\0');
            DWORD pathLength = static_cast<DWORD>(path.size());
            if (::QueryFullProcessImageNameW(process, 0, path.data(), &pathLength))
            {
                path.resize(pathLength);
                if (!path.empty())
                {
                    resolvedProcesses.push_back({ entry.th32ProcessID, path });
                }
            }

            ::CloseHandle(process);
        }
        while (::Process32NextW(snapshot, &entry));
    }

    ::CloseHandle(snapshot);
    return resolvedProcesses;
}

bool SolockController::EnsureTargetAppsNetworkingMatchesSchedule(
    const std::chrono::system_clock::time_point& now)
{
    if (ShouldBlockTargetAppsAt(now))
    {
        return EnsureTargetAppsNetworkingBlocked();
    }

    return EnsureTargetAppsNetworkingEnabled();
}

bool SolockController::ShouldBlockTargetAppsAt(const std::chrono::system_clock::time_point& now)
{
    const ExternalOverrides overrides = LoadExternalOverrides();
    if (IsCustomBlockActiveAt(now, overrides))
    {
        return true;
    }

    const int durationMinutes = std::max(1, m_options.scheduledBlockDurationMinutes);
    for (const int startMinuteOfDay : m_options.scheduledBlockStartMinutesOfDay)
    {
        const int safeStartMinuteOfDay = std::clamp(startMinuteOfDay, 0, kMaxMinuteOfDay);
        const auto blockStart = LocalAtOnSameDay(now, safeStartMinuteOfDay / 60, safeStartMinuteOfDay % 60, 0);
        const auto blockEnd = blockStart + std::chrono::minutes(durationMinutes);
        if (now >= blockStart && now < blockEnd)
        {
            return true;
        }
    }

    return false;
}

bool SolockController::IsCustomBlockActiveAt(
    const std::chrono::system_clock::time_point& now,
    const ExternalOverrides& overrides)
{
    if (overrides.signature != m_customBlockConfigSignature)
    {
        m_customBlockConfigSignature = overrides.signature;
        m_customBlockStates.assign(overrides.customBlocks.size(), CustomBlockRuntimeState{});
    }
    else if (m_customBlockStates.size() != overrides.customBlocks.size())
    {
        m_customBlockStates.resize(overrides.customBlocks.size());
    }

    for (size_t index = 0; index < overrides.customBlocks.size(); ++index)
    {
        const CustomBlockWindow& block = overrides.customBlocks[index];
        if (!block.hasStart)
        {
            continue;
        }

        CustomBlockRuntimeState& state = m_customBlockStates[index];
        if (!state.activated)
        {
            const auto customBlockStart = LocalAtOnSameDay(
                now,
                block.startMinutesOfDay / 60,
                block.startMinutesOfDay % 60,
                0);
            if (now < customBlockStart)
            {
                continue;
            }

            state.activated = true;
            state.activationTime = customBlockStart;
        }

        if (!block.hasCustomBlockDurationMinutes)
        {
            return true;
        }

        const int repeatCount = block.hasCustomBlockRepeatCount
            ? std::max(1, block.customBlockRepeatCount)
            : 1;
        const auto totalDuration = std::chrono::minutes(
            static_cast<std::chrono::minutes::rep>(block.customBlockDurationMinutes) * repeatCount);
        if (now < state.activationTime + totalDuration)
        {
            return true;
        }
    }

    return false;
}

void SolockController::CloseWfpEngine()
{
    if (m_wfpEngine != nullptr)
    {
        DebugLog(L"[WFP] closing dynamic WFP session.");
        ::FwpmEngineClose0(m_wfpEngine);
        m_wfpEngine = nullptr;
    }

    m_blockedAppFiltersInstalled = false;
    m_installedBlockedAppFilters.clear();
}

bool SolockController::EnsureEveningHotspotState()
{
    if (ShouldSkipHotspotActions())
    {
        DebugLog(L"[HOTSPOT] debug skip enabled; evening hotspot-only step bypassed.");
        return true;
    }

    bool hotspotOk = false;
    std::wstring desiredSsid;
    try
    {
        auto manager = CreateTetheringManager();
        const auto currentConfig = manager.GetCurrentAccessPointConfiguration();
        const std::wstring currentSsid = ToWString(currentConfig.Ssid());
        std::wstring originalSsid;
        const bool hasOriginalSsid = TryLoadOriginalSsid(originalSsid) && !originalSsid.empty();

        if (!currentSsid.empty() && !hasOriginalSsid)
        {
            SaveOriginalSsid(currentSsid);
            originalSsid = currentSsid;
        }

        const std::wstring aliasSource =
            !originalSsid.empty()
                ? originalSsid
                : (!currentSsid.empty() ? currentSsid : m_options.postActionSsid);
        desiredSsid = GetEveningHotspotAlias(aliasSource);

        DebugLog(L"[HOTSPOT] evening alias source SSID=" +
            (aliasSource.empty() ? std::wstring(L"<empty>") : aliasSource) +
            L", desired SSID=" + desiredSsid);
    }
    catch (const std::exception& ex)
    {
        DebugLog(L"[HOTSPOT] EnsureEveningPostActionState snapshot failed: " + ToWString(std::string(ex.what())));
    }
    catch (...)
    {
        DebugLog(L"[HOTSPOT] EnsureEveningPostActionState snapshot failed with an unknown exception.");
    }

    if (desiredSsid.empty())
    {
        desiredSsid = GetEveningHotspotAlias(m_options.postActionSsid);
    }

    hotspotOk = EnsureHotspotOnWithSsid(desiredSsid);
    return hotspotOk;
}

bool SolockController::EnsureEveningPostActionState()
{
    DebugLog(L"[WFP] evening post-action started, ensuring outbound block filters are installed.");
    const bool appBlockOk = EnsureTargetAppsNetworkingBlocked();
    DebugLog(std::wstring(L"[WFP] outbound block result: ") + (appBlockOk ? L"success" : L"failure"));

    const bool hotspotOk = EnsureEveningHotspotState();
    return hotspotOk && appBlockOk;
}

bool SolockController::ApplyEveningIdleLockIfNeeded()
{
    if (m_eveningIdleLockApplied)
    {
        return true;
    }

    const auto idleThreshold = std::chrono::minutes(std::max(1, m_options.inactivityThresholdMinutes));
    if (!IsInputIdleForAtLeast(idleThreshold))
    {
        return true;
    }

    const bool lockOk = LockCurrentSession();
    ::Sleep(1200);
    const bool displayOk = TurnOffDisplay();
    if (lockOk && displayOk)
    {
        m_eveningIdleLockApplied = true;
    }

    return lockOk && displayOk;
}

void SolockController::DebugLogRunningBlockedProcesses(
    const wchar_t* stage,
    const std::vector<RunningBlockedProcess>& runningProcesses) const
{
    std::wostringstream out;
    out << L"[WFP] " << (stage != nullptr ? stage : L"process snapshot")
        << L": " << runningProcesses.size() << L" running target process(es)";

    for (const auto& process : runningProcesses)
    {
        out << L" [PID=" << process.processId << L", Path=" << process.appPath << L"]";
    }

    DebugLog(out.str());
}

bool SolockController::LockCurrentSession()
{
    return ::LockWorkStation() != FALSE;
}

bool SolockController::TurnOffDisplay()
{
    ::SendMessageW(
        HWND_BROADCAST,
        WM_SYSCOMMAND,
        static_cast<WPARAM>(SC_MONITORPOWER),
        static_cast<LPARAM>(2));
    return true;
}

bool SolockController::ShutdownMachineNow()
{
    for (;;)
    {
        if (EnableShutdownPrivilege() &&
            ::ExitWindowsEx(EWX_POWEROFF | EWX_FORCEIFHUNG, SHTDN_REASON_FLAG_PLANNED))
        {
            return true;
        }

        if (LaunchShutdownExeFallback())
        {
            return true;
        }

        ::Sleep(1000);
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

std::wstring SolockController::GetHotspotAndBlockConfigFilePath()
{
    const std::wstring dir = GetStateDirectoryPath();
    if (dir.empty())
    {
        return L"";
    }

    return dir + L"\\" + kHotspotAndBlockConfigFileName;
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
    const std::wstring path = GetHotspotAndBlockConfigFilePath();
    if (path.empty())
    {
        return false;
    }

    bool updated = true;
    TextFileContent content;
    if (LoadTextFile(path, content) && content.exists)
    {
        updated = UpdateIniValue(path, L"state", L"original_hotspot_ssid", nullptr);
    }

    const std::wstring legacyPath = GetLegacyOriginalSsidStateFilePath();
    if (!legacyPath.empty() && ::DeleteFileW(legacyPath.c_str()) == FALSE)
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

    const std::wstring path = GetHotspotAndBlockConfigFilePath();
    if (path.empty())
    {
        return false;
    }

    if (!UpdateIniValue(path, L"state", L"original_hotspot_ssid", &ssid))
    {
        return false;
    }

    const std::wstring legacyPath = GetLegacyOriginalSsidStateFilePath();
    if (!legacyPath.empty())
    {
        ::DeleteFileW(legacyPath.c_str());
    }

    return true;
}

bool SolockController::TryLoadOriginalSsid(std::wstring& ssid)
{
    ssid.clear();

    const std::wstring path = GetHotspotAndBlockConfigFilePath();
    if (!path.empty())
    {
        TextFileContent content;
        if (LoadTextFile(path, content) &&
            content.exists &&
            TryReadIniValue(content.text, L"state", L"original_hotspot_ssid", ssid) &&
            !ssid.empty())
        {
            return true;
        }
    }

    const std::wstring legacyPath = GetLegacyOriginalSsidStateFilePath();
    if (legacyPath.empty())
    {
        return false;
    }

    TextFileContent legacyContent;
    if (!LoadTextFile(legacyPath, legacyContent) || !legacyContent.exists)
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
    ::DeleteFileW(legacyPath.c_str());
    return true;
}

SolockController::ExternalOverrides SolockController::LoadExternalOverrides()
{
    ExternalOverrides overrides;
    const std::wstring path = GetHotspotAndBlockConfigFilePath();
    if (path.empty())
    {
        return overrides;
    }

    TextFileContent content;
    if (!LoadTextFile(path, content) || !content.exists)
    {
        return overrides;
    }

    RawCustomBlockConfig rawBlock;
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

        int parsedRepeatCount = 0;
        if (TryParseStrictInt(rawBlock.repeatCount, parsedRepeatCount) && parsedRepeatCount > 0)
        {
            block.hasCustomBlockRepeatCount = true;
            block.customBlockRepeatCount = parsedRepeatCount;
        }

        block.signature =
            L"start=" + rawBlock.start +
            L"|duration=" + rawBlock.durationMinutes +
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

            inCustomBlockSection = EqualsIgnoreCase(sectionName, L"custom_block");
            continue;
        }

        if (!inCustomBlockSection)
        {
            continue;
        }

        std::wstring key;
        std::wstring value;
        if (!TryParseIniKeyValue(line, key, value))
        {
            continue;
        }

        if (EqualsIgnoreCase(key, L"start"))
        {
            rawBlock.start = value;
        }
        else if (EqualsIgnoreCase(key, L"duration_minutes"))
        {
            rawBlock.durationMinutes = value;
        }
        else if (EqualsIgnoreCase(key, L"repeat_count"))
        {
            rawBlock.repeatCount = value;
        }
    }

    if (inCustomBlockSection)
    {
        finalizeCustomBlock();
    }

    for (const auto& block : overrides.customBlocks)
    {
        if (!overrides.signature.empty())
        {
            overrides.signature += L"||";
        }

        overrides.signature += block.signature;
    }

    return overrides;
}

std::wstring SolockController::GetCurrentExePath()
{
    std::wstring buffer(32768, L'\0');
    const DWORD len = ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    buffer.resize(len);
    return buffer;
}

std::wstring SolockController::GetCurrentExeDirectory()
{
    std::wstring path = GetCurrentExePath();
    const size_t pos = path.find_last_of(L"\\/");
    if (pos == std::wstring::npos)
    {
        return L"";
    }

    return path.substr(0, pos);
}

std::wstring SolockController::GetCurrentUserTaskUserId()
{
    auto GetEnv = [](const wchar_t* name) -> std::wstring
    {
        const DWORD needed = ::GetEnvironmentVariableW(name, nullptr, 0);
        if (needed == 0)
        {
            return L"";
        }

        std::wstring value(needed, L'\0');
        const DWORD written = ::GetEnvironmentVariableW(name, value.data(), needed);
        if (written == 0)
        {
            return L"";
        }

        value.resize(written);
        return value;
    };

    const std::wstring domain = GetEnv(L"USERDOMAIN");
    const std::wstring user = GetEnv(L"USERNAME");

    if (!domain.empty() && !user.empty())
    {
        return domain + L"\\" + user;
    }

    DWORD needed = 0;
    ::GetUserNameW(nullptr, &needed);
    if (needed > 0)
    {
        std::wstring name(needed, L'\0');
        if (::GetUserNameW(name.data(), &needed))
        {
            if (!name.empty() && name.back() == L'\0')
            {
                name.pop_back();
            }
            return name;
        }
    }

    return L"";
}

bool SolockController::EnsureStartupTaskRegistered(const std::wstring& taskName)
{
    if (taskName.empty())
    {
        DebugLog(L"[TASK] registration skipped because taskName is empty.");
        return false;
    }

    const std::wstring exePath = GetCurrentExePath();
    const std::wstring exeDir = GetCurrentExeDirectory();
    const std::wstring userId = GetCurrentUserTaskUserId();

    if (exePath.empty() || userId.empty())
    {
        DebugLog(L"[TASK] registration failed because exePath or userId is empty.");
        return false;
    }

    HRESULT hr = ::CoInitializeSecurity(
        nullptr,
        -1,
        nullptr,
        nullptr,
        RPC_C_AUTHN_LEVEL_PKT_PRIVACY,
        RPC_C_IMP_LEVEL_IMPERSONATE,
        nullptr,
        0,
        nullptr);

    if (FAILED(hr) && hr != RPC_E_TOO_LATE)
    {
        return false;
    }

    ITaskService* service = nullptr;
    ITaskFolder* rootFolder = nullptr;
    ITaskDefinition* task = nullptr;
    IRegistrationInfo* registrationInfo = nullptr;
    IPrincipal* principal = nullptr;
    ITaskSettings* settings = nullptr;
    ITriggerCollection* triggers = nullptr;
    ITrigger* trigger = nullptr;
    ILogonTrigger* logonTrigger = nullptr;
    IActionCollection* actions = nullptr;
    IAction* action = nullptr;
    IExecAction* execAction = nullptr;
    IRegisteredTask* registeredTask = nullptr;

    bool ok = false;
    const wchar_t* failureStep = nullptr;
    HRESULT failureHr = S_OK;

    do
    {
        hr = ::CoCreateInstance(
            CLSID_TaskScheduler,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_ITaskService,
            reinterpret_cast<void**>(&service));

        if (FAILED(hr) || service == nullptr)
        {
            failureStep = L"CoCreateInstance(CLSID_TaskScheduler)";
            failureHr = hr;
            break;
        }

        hr = service->Connect(_variant_t(), _variant_t(), _variant_t(), _variant_t());
        if (FAILED(hr))
        {
            failureStep = L"ITaskService::Connect";
            failureHr = hr;
            break;
        }

        hr = service->GetFolder(_bstr_t(L"\\"), &rootFolder);
        if (FAILED(hr) || rootFolder == nullptr)
        {
            failureStep = L"ITaskService::GetFolder";
            failureHr = hr;
            break;
        }

        hr = service->NewTask(0, &task);
        if (FAILED(hr) || task == nullptr)
        {
            failureStep = L"ITaskService::NewTask";
            failureHr = hr;
            break;
        }

        hr = task->get_RegistrationInfo(&registrationInfo);
        if (FAILED(hr) || registrationInfo == nullptr)
        {
            failureStep = L"ITaskDefinition::get_RegistrationInfo";
            failureHr = hr;
            break;
        }

        registrationInfo->put_Author(_bstr_t(L"Solock"));
        registrationInfo->put_Description(_bstr_t(L"Start Solock automatically when the current user logs on."));

        hr = task->get_Principal(&principal);
        if (FAILED(hr) || principal == nullptr)
        {
            failureStep = L"ITaskDefinition::get_Principal";
            failureHr = hr;
            break;
        }

        principal->put_UserId(_bstr_t(userId.c_str()));
        principal->put_LogonType(TASK_LOGON_INTERACTIVE_TOKEN);
        principal->put_RunLevel(TASK_RUNLEVEL_HIGHEST);

        hr = task->get_Settings(&settings);
        if (FAILED(hr) || settings == nullptr)
        {
            failureStep = L"ITaskDefinition::get_Settings";
            failureHr = hr;
            break;
        }

        settings->put_StartWhenAvailable(VARIANT_TRUE);
        settings->put_DisallowStartIfOnBatteries(VARIANT_FALSE);
        settings->put_StopIfGoingOnBatteries(VARIANT_FALSE);
        settings->put_MultipleInstances(TASK_INSTANCES_IGNORE_NEW);
        settings->put_AllowDemandStart(VARIANT_TRUE);
        settings->put_Enabled(VARIANT_TRUE);
        settings->put_Hidden(VARIANT_FALSE);
        settings->put_ExecutionTimeLimit(_bstr_t(L"PT0S"));

        hr = task->get_Triggers(&triggers);
        if (FAILED(hr) || triggers == nullptr)
        {
            failureStep = L"ITaskDefinition::get_Triggers";
            failureHr = hr;
            break;
        }

        hr = triggers->Create(TASK_TRIGGER_LOGON, &trigger);
        if (FAILED(hr) || trigger == nullptr)
        {
            failureStep = L"ITriggerCollection::Create";
            failureHr = hr;
            break;
        }

        hr = trigger->QueryInterface(IID_ILogonTrigger, reinterpret_cast<void**>(&logonTrigger));
        if (FAILED(hr) || logonTrigger == nullptr)
        {
            failureStep = L"ITrigger::QueryInterface(ILogonTrigger)";
            failureHr = hr;
            break;
        }

        logonTrigger->put_Id(_bstr_t(L"LogonTrigger"));
        logonTrigger->put_UserId(_bstr_t(userId.c_str()));

        hr = task->get_Actions(&actions);
        if (FAILED(hr) || actions == nullptr)
        {
            failureStep = L"ITaskDefinition::get_Actions";
            failureHr = hr;
            break;
        }

        hr = actions->Create(TASK_ACTION_EXEC, &action);
        if (FAILED(hr) || action == nullptr)
        {
            failureStep = L"IActionCollection::Create";
            failureHr = hr;
            break;
        }

        hr = action->QueryInterface(IID_IExecAction, reinterpret_cast<void**>(&execAction));
        if (FAILED(hr) || execAction == nullptr)
        {
            failureStep = L"IAction::QueryInterface(IExecAction)";
            failureHr = hr;
            break;
        }

        execAction->put_Path(_bstr_t(exePath.c_str()));
        execAction->put_WorkingDirectory(_bstr_t(exeDir.c_str()));

        hr = rootFolder->RegisterTaskDefinition(
            _bstr_t(taskName.c_str()),
            task,
            TASK_CREATE_OR_UPDATE,
            _variant_t(),
            _variant_t(),
            TASK_LOGON_INTERACTIVE_TOKEN,
            _variant_t(L""),
            &registeredTask);

        if (FAILED(hr) || registeredTask == nullptr)
        {
            failureStep = L"ITaskFolder::RegisterTaskDefinition";
            failureHr = hr;
            break;
        }

        ok = true;
    }
    while (false);

    SafeRelease(registeredTask);
    SafeRelease(execAction);
    SafeRelease(action);
    SafeRelease(actions);
    SafeRelease(logonTrigger);
    SafeRelease(trigger);
    SafeRelease(triggers);
    SafeRelease(settings);
    SafeRelease(principal);
    SafeRelease(registrationInfo);
    SafeRelease(task);
    SafeRelease(rootFolder);
    SafeRelease(service);

    if (!ok)
    {
        if (failureStep != nullptr)
        {
            DebugLogHResult(failureStep, failureHr);
        }
        else
        {
            DebugLog(L"[TASK] registration failed before a COM step reported an HRESULT.");
        }
    }

    return ok;
}
