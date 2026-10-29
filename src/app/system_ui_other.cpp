// SPDX-License-Identifier: MIT

// Systems other than macOS and Windows have no menu bar of their own. The program draws its menus in the window, and
// the system draws the title bar. The C library writes times of day.

#include "app/system_ui.h"

#include <array>
#include <clocale>
#include <ctime>

namespace hokuto::app
{

bool InstallMenuBar(std::span<const Menu> /*menus*/, MenuTarget& /*target*/)
{
    return false;
}

void SetTitleBarColour(SDL_Window* /*window*/, ImU32 /*colour*/)
{
}

void WakeOnQuitSignals(uint32_t /*event_type*/)
{
}

std::string TimeOfDay(std::chrono::system_clock::time_point time)
{
    // %X writes the time in the format of the user's locale for times. The program takes that locale the first time.
    static const bool kLocale = std::setlocale(LC_TIME, "") != nullptr;
    static_cast<void>(kLocale);

    const std::time_t seconds = std::chrono::system_clock::to_time_t(time);
    std::tm local{};
    localtime_r(&seconds, &local);
    std::array<char, 64> text{};
    const std::size_t length = std::strftime(text.data(), text.size(), "%X", &local);
    return std::string(text.data(), length);
}

std::string SystemPath(const std::string& path)
{
    return path;
}

} // namespace hokuto::app
