#include "pch.h"
#include "WallpaperTheme.h"

#include <gdiplus.h>

#include <algorithm>
#include <cmath>
#include <map>

#pragma comment(lib, "gdiplus.lib")

namespace
{
    struct HslColor
    {
        double hue = 0.0;
        double saturation = 0.0;
        double lightness = 0.0;
    };

    struct SampleBucket
    {
        double weight = 0.0;
        double red = 0.0;
        double green = 0.0;
        double blue = 0.0;
    };

    double Clamp01(const double value)
    {
        if (value < 0.0)
        {
            return 0.0;
        }

        if (value > 1.0)
        {
            return 1.0;
        }

        return value;
    }

    BYTE ClampByte(const double value)
    {
        return static_cast<BYTE>(std::round(std::clamp(value, 0.0, 255.0)));
    }

    winrt::Windows::UI::Color MakeColor(const BYTE alpha, const BYTE red, const BYTE green, const BYTE blue)
    {
        winrt::Windows::UI::Color color{};
        color.A = alpha;
        color.R = red;
        color.G = green;
        color.B = blue;
        return color;
    }

    double ComputeLuminance(const BYTE red, const BYTE green, const BYTE blue)
    {
        const double r = red / 255.0;
        const double g = green / 255.0;
        const double b = blue / 255.0;
        return 0.2126 * r + 0.7152 * g + 0.0722 * b;
    }

    double ComputeSaturation(const BYTE red, const BYTE green, const BYTE blue)
    {
        const double r = red / 255.0;
        const double g = green / 255.0;
        const double b = blue / 255.0;
        const double maxValue = std::max({ r, g, b });
        const double minValue = std::min({ r, g, b });
        return maxValue - minValue;
    }

    HslColor ToHsl(const winrt::Windows::UI::Color& color)
    {
        const double red = color.R / 255.0;
        const double green = color.G / 255.0;
        const double blue = color.B / 255.0;
        const double maxValue = std::max({ red, green, blue });
        const double minValue = std::min({ red, green, blue });
        const double delta = maxValue - minValue;

        HslColor hsl;
        hsl.lightness = (maxValue + minValue) * 0.5;
        if (delta <= 0.0001)
        {
            return hsl;
        }

        hsl.saturation = hsl.lightness > 0.5
            ? delta / (2.0 - maxValue - minValue)
            : delta / (maxValue + minValue);

        if (maxValue == red)
        {
            hsl.hue = (green - blue) / delta + (green < blue ? 6.0 : 0.0);
        }
        else if (maxValue == green)
        {
            hsl.hue = (blue - red) / delta + 2.0;
        }
        else
        {
            hsl.hue = (red - green) / delta + 4.0;
        }

        hsl.hue /= 6.0;
        return hsl;
    }

    double HueToRgb(const double p, const double q, double t)
    {
        if (t < 0.0)
        {
            t += 1.0;
        }

        if (t > 1.0)
        {
            t -= 1.0;
        }

        if (t < 1.0 / 6.0)
        {
            return p + (q - p) * 6.0 * t;
        }

        if (t < 1.0 / 2.0)
        {
            return q;
        }

        if (t < 2.0 / 3.0)
        {
            return p + (q - p) * (2.0 / 3.0 - t) * 6.0;
        }

        return p;
    }

    winrt::Windows::UI::Color FromHsl(const HslColor& hsl)
    {
        double red = hsl.lightness;
        double green = hsl.lightness;
        double blue = hsl.lightness;

        if (hsl.saturation > 0.0001)
        {
            const double q = hsl.lightness < 0.5
                ? hsl.lightness * (1.0 + hsl.saturation)
                : hsl.lightness + hsl.saturation - hsl.lightness * hsl.saturation;
            const double p = 2.0 * hsl.lightness - q;
            red = HueToRgb(p, q, hsl.hue + 1.0 / 3.0);
            green = HueToRgb(p, q, hsl.hue);
            blue = HueToRgb(p, q, hsl.hue - 1.0 / 3.0);
        }

        return MakeColor(255, ClampByte(red * 255.0), ClampByte(green * 255.0), ClampByte(blue * 255.0));
    }

    winrt::Windows::UI::Color Blend(
        const winrt::Windows::UI::Color& first,
        const winrt::Windows::UI::Color& second,
        const double secondWeight)
    {
        const double clampedWeight = Clamp01(secondWeight);
        const double firstWeight = 1.0 - clampedWeight;

        return MakeColor(
            255,
            ClampByte(first.R * firstWeight + second.R * clampedWeight),
            ClampByte(first.G * firstWeight + second.G * clampedWeight),
            ClampByte(first.B * firstWeight + second.B * clampedWeight));
    }

    winrt::Windows::UI::Color WithAlpha(const winrt::Windows::UI::Color& color, const BYTE alpha)
    {
        return MakeColor(alpha, color.R, color.G, color.B);
    }

    winrt::Windows::UI::Color NormalizeAccent(const winrt::Windows::UI::Color& source)
    {
        HslColor hsl = ToHsl(source);
        hsl.saturation = std::max(hsl.saturation, 0.42);
        hsl.lightness = std::clamp(hsl.lightness, 0.36, 0.60);
        return FromHsl(hsl);
    }

    std::wstring ResolveWallpaperPath()
    {
        wchar_t path[MAX_PATH] = {};
        if (!::SystemParametersInfoW(SPI_GETDESKWALLPAPER, MAX_PATH, path, 0))
        {
            return L"";
        }

        return path;
    }

    class GdiplusSession
    {
    public:
        GdiplusSession()
        {
            Gdiplus::GdiplusStartupInput input;
            m_status = Gdiplus::GdiplusStartup(&m_token, &input, nullptr);
        }

        ~GdiplusSession()
        {
            if (m_status == Gdiplus::Ok)
            {
                Gdiplus::GdiplusShutdown(m_token);
            }
        }

        [[nodiscard]] bool IsReady() const
        {
            return m_status == Gdiplus::Ok;
        }

    private:
        ULONG_PTR m_token = 0;
        Gdiplus::Status m_status = Gdiplus::GenericError;
    };

    winrt::Windows::UI::Color PickAccentFromBitmap(
        Gdiplus::Bitmap& bitmap,
        std::wstring& error)
    {
        const UINT width = bitmap.GetWidth();
        const UINT height = bitmap.GetHeight();
        if (width == 0 || height == 0)
        {
            error = L"The wallpaper image is empty.";
            return MakeColor(255, 76, 139, 245);
        }

        const UINT stepX = std::max(1u, width / 96u);
        const UINT stepY = std::max(1u, height / 96u);
        std::map<unsigned int, SampleBucket> buckets;

        for (UINT y = 0; y < height; y += stepY)
        {
            for (UINT x = 0; x < width; x += stepX)
            {
                Gdiplus::Color pixel;
                if (bitmap.GetPixel(x, y, &pixel) != Gdiplus::Ok)
                {
                    continue;
                }

                if (pixel.GetAlpha() < 200)
                {
                    continue;
                }

                const BYTE red = pixel.GetRed();
                const BYTE green = pixel.GetGreen();
                const BYTE blue = pixel.GetBlue();
                const double saturation = ComputeSaturation(red, green, blue);
                const double luminance = ComputeLuminance(red, green, blue);
                const double luminanceWeight = 1.0 - std::min(1.0, std::abs(luminance - 0.55) / 0.55);
                const double weight = 0.25 + saturation * 1.65 + luminanceWeight * 0.85;

                const unsigned int key =
                    ((red >> 4) << 8) |
                    ((green >> 4) << 4) |
                    (blue >> 4);

                auto& bucket = buckets[key];
                bucket.weight += weight;
                bucket.red += red * weight;
                bucket.green += green * weight;
                bucket.blue += blue * weight;
            }
        }

        if (buckets.empty())
        {
            error = L"Unable to sample colors from the wallpaper.";
            return MakeColor(255, 76, 139, 245);
        }

        const auto bestBucket = std::max_element(
            buckets.begin(),
            buckets.end(),
            [](auto const& left, auto const& right)
            {
                return left.second.weight < right.second.weight;
            });

        const SampleBucket& bucket = bestBucket->second;
        const winrt::Windows::UI::Color accent = MakeColor(
            255,
            ClampByte(bucket.red / bucket.weight),
            ClampByte(bucket.green / bucket.weight),
            ClampByte(bucket.blue / bucket.weight));
        return NormalizeAccent(accent);
    }
}

namespace solock_configurator
{
    bool TryGetWallpaperAccentColor(
        winrt::Windows::UI::Color& accentColor,
        std::wstring& wallpaperPath,
        std::wstring& error)
    {
        error.clear();
        wallpaperPath = ResolveWallpaperPath();
        if (wallpaperPath.empty())
        {
            error = L"Unable to resolve the current desktop wallpaper.";
            return false;
        }

        GdiplusSession gdiplus;
        if (!gdiplus.IsReady())
        {
            error = L"Unable to initialize GDI+ for wallpaper analysis.";
            return false;
        }

        Gdiplus::Bitmap bitmap(wallpaperPath.c_str());
        if (bitmap.GetLastStatus() != Gdiplus::Ok)
        {
            error = L"Unable to open the current wallpaper image.";
            return false;
        }

        accentColor = PickAccentFromBitmap(bitmap, error);
        return error.empty();
    }

    winrt::Windows::UI::Color GetSystemAccentColorFallback()
    {
        const winrt::Windows::UI::ViewManagement::UISettings settings;
        return settings.GetColorValue(winrt::Windows::UI::ViewManagement::UIColorType::Accent);
    }

    ThemePalette BuildThemePalette(
        winrt::Windows::UI::Color accentColor,
        const bool darkMode)
    {
        accentColor = NormalizeAccent(accentColor);

        const winrt::Windows::UI::Color white = MakeColor(255, 255, 255, 255);
        const winrt::Windows::UI::Color black = MakeColor(255, 0, 0, 0);
        const winrt::Windows::UI::Color accentLight = Blend(accentColor, white, 0.26);
        const winrt::Windows::UI::Color accentDark = Blend(accentColor, black, 0.24);

        ThemePalette palette{};
        palette.accent = accentColor;
        palette.accentLight = accentLight;
        palette.accentDark = accentDark;
        palette.error = darkMode
            ? MakeColor(255, 255, 122, 122)
            : MakeColor(255, 186, 26, 26);
        palette.accentForeground = ComputeLuminance(accentColor.R, accentColor.G, accentColor.B) > 0.53
            ? MakeColor(255, 28, 28, 28)
            : white;

        if (darkMode)
        {
            palette.windowBackground = WithAlpha(Blend(accentColor, black, 0.88), 228);
            palette.cardBackground = WithAlpha(Blend(accentColor, black, 0.80), 186);
            palette.cardBorder = WithAlpha(Blend(accentColor, white, 0.30), 112);
            palette.mutedText = MakeColor(255, 196, 203, 215);
            palette.appTitle = accentLight;
            palette.appSubtitle = MakeColor(255, 218, 223, 233);
            palette.titleBarBackground = WithAlpha(Blend(accentColor, black, 0.82), 215);
            palette.titleBarForeground = white;
            palette.titleBarButtonHoverBackground = WithAlpha(accentLight, 72);
            palette.titleBarButtonPressedBackground = WithAlpha(accentLight, 96);
        }
        else
        {
            palette.windowBackground = WithAlpha(Blend(accentColor, white, 0.92), 238);
            palette.cardBackground = WithAlpha(Blend(accentColor, white, 0.96), 220);
            palette.cardBorder = WithAlpha(Blend(accentColor, black, 0.12), 92);
            palette.mutedText = MakeColor(255, 86, 96, 110);
            palette.appTitle = accentDark;
            palette.appSubtitle = MakeColor(255, 60, 69, 82);
            palette.titleBarBackground = WithAlpha(Blend(accentColor, white, 0.85), 232);
            palette.titleBarForeground = MakeColor(255, 27, 31, 38);
            palette.titleBarButtonHoverBackground = WithAlpha(accentColor, 40);
            palette.titleBarButtonPressedBackground = WithAlpha(accentColor, 64);
        }

        return palette;
    }
}
