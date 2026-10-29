// SPDX-License-Identifier: MIT

// Systems other than macOS and Windows have no implementation yet. Dear ImGui's font draws the text there.

#include "app/system_fonts.h"

namespace hokuto::app
{

theme::Fonts AddSystemFonts(ImFontAtlas& /*atlas*/)
{
    return {};
}

} // namespace hokuto::app
