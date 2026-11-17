// SPDX-License-Identifier: MIT

// Adds the system's interface fonts to Dear ImGui's font atlas. The program draws its text with them, Japanese
// included. system_fonts_mac.cpp draws them with Core Text, and system_fonts_windows.cpp with DirectWrite. On other
// systems, system_fonts_other.cpp finds them through fontconfig, and FreeType draws them. Where the system has none
// of them, Dear ImGui's font draws the text.

#pragma once

#include <imgui.h>

#include "app/theme.h"

namespace hokuto::app
{

// Adds a font for each of the program's roles to `atlas`. Leaves out a role that the system has no font for.
theme::Fonts AddSystemFonts(ImFontAtlas& atlas);

} // namespace hokuto::app
