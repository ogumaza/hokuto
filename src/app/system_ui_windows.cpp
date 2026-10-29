// SPDX-License-Identifier: MIT

// Windows has no menu bar of its own at the top of the screen. The program draws its menus in the window. The Desktop
// Window Manager draws the title bar, in the colour that the program gives it. The C library writes times of day, and
// the shell's dialogs give paths that the program brings to one form.

// clang-format off
// Windows' headers come first: dwmapi.h requires its types.
#include <windows.h>
// clang-format on

#include "app/system_ui.h"

#include <dwmapi.h>

#include <array>
#include <clocale>
#include <ctime>
#include <string>
#include <string_view>

namespace hokuto::app
{
namespace
{

// The window attributes of the Desktop Window Manager that colour a title bar. The caption's and the border's colours
// hold from Windows 11 on. Earlier systems refuse them, and keep the dark title bar alone.
constexpr DWORD kUseImmersiveDarkMode = 20; // DWMWA_USE_IMMERSIVE_DARK_MODE
constexpr DWORD kBorderColour = 34;         // DWMWA_BORDER_COLOR
constexpr DWORD kCaptionColour = 35;        // DWMWA_CAPTION_COLOR

// Win32's prefixes of a path past MAX_PATH, on a drive and on a share.
constexpr std::wstring_view kLongPrefix = L"\\\\?\\";
constexpr std::wstring_view kLongSharePrefix = L"\\\\?\\UNC\\";

std::wstring Widened(const std::string& text)
{
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), length);

    return wide;
}

std::string Narrowed(const std::wstring& wide)
{
    const int length =
        WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    std::string text(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), text.data(), length, nullptr, nullptr);

    return text;
}

// `path` without Win32's prefix of a long path.
std::wstring Unprefixed(const std::wstring& path)
{
    if (path.starts_with(kLongSharePrefix))
    {
        return L"\\\\" + path.substr(kLongSharePrefix.size());
    }

    if (path.starts_with(kLongPrefix))
    {
        return path.substr(kLongPrefix.size());
    }

    return path;
}

// `path` with Win32's prefix of a long path. A relative path stays as it is: the prefix holds for full paths alone.
std::wstring Prefixed(const std::wstring& path)
{
    if (path.starts_with(L"\\\\"))
    {
        return std::wstring(kLongSharePrefix) + path.substr(2);
    }

    if (path.size() >= 3 && path[1] == L':' && (path[2] == L'\\' || path[2] == L'/'))
    {
        return std::wstring(kLongPrefix) + path;
    }

    return path;
}

// The long form of each folder and name of an existing `path`. Nothing when the system can't give it.
std::wstring LongForm(const std::wstring& path)
{
    const std::wstring prefixed = Prefixed(path);
    const DWORD length = GetLongPathNameW(prefixed.c_str(), nullptr, 0);
    if (length == 0)
    {
        return {};
    }

    std::wstring result(length, L'\0');
    const DWORD written = GetLongPathNameW(prefixed.c_str(), result.data(), length);
    if (written == 0 || written >= length)
    {
        return {};
    }

    result.resize(written);

    return Unprefixed(result);
}

} // namespace

bool InstallMenuBar(std::span<const Menu> /*menus*/, MenuTarget& /*target*/)
{
    return false;
}

void SetTitleBarColour(SDL_Window* window, ImU32 colour)
{
    auto* hwnd = static_cast<HWND>(
        SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
    if (hwnd == nullptr)
    {
        return;
    }

    // The dark mode gives the title and the controls their light colours. Dear ImGui keeps a colour as 0xAABBGGRR, and
    // Windows as 0x00BBGGRR.
    const BOOL kDark = TRUE;
    DwmSetWindowAttribute(hwnd, kUseImmersiveDarkMode, &kDark, sizeof kDark);
    const COLORREF bar = colour & 0x00FFFFFFu;
    DwmSetWindowAttribute(hwnd, kCaptionColour, &bar, sizeof bar);
    DwmSetWindowAttribute(hwnd, kBorderColour, &bar, sizeof bar);
}

void WakeOnQuitSignals(uint32_t /*event_type*/)
{
}

std::string TimeOfDay(std::chrono::system_clock::time_point time)
{
    // %X writes the time in the format of the user's locale for times. The program takes that locale the first time,
    // with UTF-8 as its code page: Dear ImGui reads UTF-8. The locale's ANSI code page would write some locales' marks
    // otherwise, such as the Korean mark of the afternoon. Windows takes ".UTF8" from Windows 10 version 1803 on. On an
    // older system, the C locale stays, and the time takes its 24-hour form.
    static const bool kLocale = std::setlocale(LC_TIME, ".UTF8") != nullptr;
    static_cast<void>(kLocale);

    const std::time_t seconds = std::chrono::system_clock::to_time_t(time);
    std::tm local{};
    localtime_s(&local, &seconds);
    std::array<char, 64> text{};
    const std::size_t length = std::strftime(text.data(), text.size(), "%X", &local);
    return std::string(text.data(), length);
}

std::string SystemPath(const std::string& path)
{
    // The shell's dialogs give a path past MAX_PATH in two forms: with Win32's prefix of a long path, or with the short
    // 8.3 names of its folders and file. A new file of a save has no short name yet, but its folder can.
    const std::wstring wide = Unprefixed(Widened(path));
    std::wstring full = LongForm(wide);
    if (full.empty())
    {
        const std::size_t slash = wide.find_last_of(L"\\/");
        if (slash != std::wstring::npos)
        {
            const std::wstring folder = LongForm(wide.substr(0, slash));
            if (!folder.empty())
            {
                full = folder + wide.substr(slash);
            }
        }
    }

    if (full.empty())
    {
        full = wide;
    }

    // Past MAX_PATH, only the prefix reaches the file, unless the system allows long paths.
    return Narrowed(full.size() >= MAX_PATH ? Prefixed(full) : full);
}

} // namespace hokuto::app
