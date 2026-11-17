// SPDX-License-Identifier: MIT

// The families that draw the program's text where the system's interface fonts are missing, as under Wine on Linux.
// system_fonts_windows.cpp tries them through DirectWrite when Windows' fonts are missing, and system_fonts_other.cpp
// through fontconfig on Linux. Every name is a family that the user's system provides. The program bundles no font.

#pragma once

#include <array>
#include <string_view>

namespace hokuto::app::font_families
{

// The interface's families, best first. The first one that the system has draws the text. The others that it has fill
// in the characters that the first lacks, in this order, before the Japanese family does: a Japanese font's signs,
// such as its sharp and its infinity sign, are a full em wide.
inline constexpr std::array<std::string_view, 3> kInterface = {
    "Noto Sans",       // the default of Ubuntu, Fedora, openSUSE and KDE. Lacks U+221E and the music signs.
    "DejaVu Sans",     // the default of Debian. Has every character that the program draws.
    "Liberation Sans", // Debian's and Ubuntu's packages of Wine require it.
};

// The Japanese families, best first. Only the first one that the system has is used. Droid Sans Fallback comes last:
// it draws the characters that Japanese shares with Chinese in their Chinese forms.
inline constexpr std::array<std::string_view, 7> kJapanese = {
    "Noto Sans CJK JP", "Noto Sans JP", "Source Han Sans JP",  "IPAPGothic",
    "VL PGothic",       "TakaoPGothic", "Droid Sans Fallback",
};

// Where the baseline divides a line: Segoe UI's ascent of 2210 units over its line of 2724. The families of the lists
// take this division in place of their own, so that their text lies on the rows of Segoe UI's.
inline constexpr float kAscentPerLine = 2210.0f / 2724.0f;

// The lightest weight that draws the text, and the lightest that draws the strong text (OS/2 weight classes). Where a
// family's nearest face is lighter, such as in a family of thin faces alone, the next family draws the text.
inline constexpr int kLightestRegular = 400;
inline constexpr int kLightestStrong = 600;

} // namespace hokuto::app::font_families
