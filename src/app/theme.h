// SPDX-License-Identifier: MIT

// The program's look: its colours, its sizes and its fonts. The surfaces are a dark navy charcoal with one blue accent.
// Green marks playback, and amber marks the cycle.

#pragma once

#include <imgui.h>

namespace hokuto::app::theme
{

// The surfaces, from the darkest to the lightest. The window's colour shows in the gaps between the zones.
inline constexpr ImU32 kWindow = IM_COL32(9, 12, 17, 255);
inline constexpr ImU32 kPanel = IM_COL32(16, 20, 27, 255);
inline constexpr ImU32 kPanelAlt = IM_COL32(19, 24, 32, 255);
inline constexpr ImU32 kRaised = IM_COL32(23, 29, 38, 255);
inline constexpr ImU32 kRaisedHover = IM_COL32(30, 37, 48, 255);
inline constexpr ImU32 kControl = IM_COL32(33, 41, 53, 255);
inline constexpr ImU32 kControlHover = IM_COL32(42, 51, 65, 255);
inline constexpr ImU32 kControlActive = IM_COL32(52, 62, 78, 255);
inline constexpr ImU32 kBorder = IM_COL32(34, 41, 52, 255);
inline constexpr ImU32 kBorderStrong = IM_COL32(48, 57, 71, 255);
inline constexpr ImU32 kDisplay = IM_COL32(6, 9, 13, 255);
inline constexpr ImU32 kGridBar = IM_COL32(255, 255, 255, 20);
inline constexpr ImU32 kGridBeat = IM_COL32(255, 255, 255, 8);

// A fader's cap, light like the caps of a mixing desk, and lighter under the mouse and while the mouse holds it.
inline constexpr ImU32 kFaderCap = IM_COL32(190, 197, 209, 255);
inline constexpr ImU32 kFaderCapHover = IM_COL32(220, 225, 233, 255);
inline constexpr ImU32 kFaderCapHeld = IM_COL32(236, 239, 244, 255);

// The grid's line at each beat while Hokuto counts beats only. No line stands out from the others.
inline constexpr ImU32 kGridBeatOnly = IM_COL32(255, 255, 255, 14);

// The text, from the brightest to the dimmest, and the text on a track's colour. Text that the user reads is kTextMuted
// or brighter. kTextDim greys out a label or a value that doesn't apply, and draws icons and lines.
inline constexpr ImU32 kText = IM_COL32(231, 234, 240, 255);
inline constexpr ImU32 kTextMuted = IM_COL32(180, 189, 204, 255);
inline constexpr ImU32 kTextDim = IM_COL32(130, 141, 159, 255);
inline constexpr ImU32 kTextOnColour = IM_COL32(9, 12, 17, 230);
inline constexpr ImU32 kTextOnAccent = IM_COL32(255, 255, 255, 255); // a primary button's label, and a check mark

// The accents.
inline constexpr ImU32 kAccent = IM_COL32(84, 141, 255, 255);
inline constexpr ImU32 kPlay = IM_COL32(52, 211, 153, 255);
inline constexpr ImU32 kCycle = IM_COL32(245, 181, 68, 255);
inline constexpr ImU32 kMute = IM_COL32(242, 169, 59, 255);
inline constexpr ImU32 kSky = IM_COL32(56, 189, 248, 255); // the browser's sounds and wave sound files
inline constexpr ImU32 kSolo = IM_COL32(240, 82, 82, 255);
inline constexpr ImU32 kMeterMid = IM_COL32(234, 200, 70, 255);
inline constexpr ImU32 kMeterHigh = IM_COL32(240, 82, 82, 255);

// The colours that tracks take, in order.
inline constexpr ImU32 kTrackColours[] = {
    IM_COL32(74, 127, 224, 255), IM_COL32(47, 181, 200, 255), IM_COL32(60, 184, 120, 255),
    IM_COL32(155, 197, 61, 255), IM_COL32(226, 185, 59, 255), IM_COL32(232, 128, 58, 255),
    IM_COL32(224, 82, 90, 255),  IM_COL32(211, 95, 168, 255), IM_COL32(142, 106, 216, 255),
};

// The colour of every track while the tracks' colours are Standard.
inline constexpr ImU32 kStandardTrackColour = IM_COL32(122, 134, 153, 255);

// The colours that the user picks a track's from: the tracks' colours, the same 40% of the way to white, and the
// Standard colour.
inline constexpr ImU32 kTrackPalette[] = {
    IM_COL32(74, 127, 224, 255),  IM_COL32(47, 181, 200, 255),  IM_COL32(60, 184, 120, 255),
    IM_COL32(155, 197, 61, 255),  IM_COL32(226, 185, 59, 255),  IM_COL32(232, 128, 58, 255),
    IM_COL32(224, 82, 90, 255),   IM_COL32(211, 95, 168, 255),  IM_COL32(142, 106, 216, 255),
    IM_COL32(146, 178, 236, 255), IM_COL32(130, 211, 222, 255), IM_COL32(138, 212, 174, 255),
    IM_COL32(195, 220, 139, 255), IM_COL32(238, 213, 137, 255), IM_COL32(241, 179, 137, 255),
    IM_COL32(236, 151, 156, 255), IM_COL32(229, 159, 203, 255), IM_COL32(187, 166, 232, 255),
    kStandardTrackColour,
};

// Text sizes, before the display's scale. Dear ImGui sizes a font by the height of its line, from its ascent to its
// descent. It rounds a size to whole points, and it stretches the glyphs to any other size. Stretched glyphs come out
// blurred. A line of the Mac's interface font is 1.18 times its type size. macOS draws most of its text at 13 points,
// and 15 is the nearest size.
inline constexpr float kTextSize = 15.0f;     // 12.7-point type
inline constexpr float kSmallSize = 13.0f;    // 11 points
inline constexpr float kCapsSize = 12.0f;     // 10.2 points
inline constexpr float kTitleSize = 18.0f;    // 15.3 points
inline constexpr float kPositionSize = 21.0f; // 17.8 points, the control bar's position

// The space between items, across and down, before the display's scale. Menus take it too.
inline constexpr float kItemSpacing = 8.0f;

// The padding inside windows, menus and tooltips, and inside controls, before the display's scale.
inline constexpr ImVec2 kWindowPadding(14.0f, 12.0f);
inline constexpr ImVec2 kFramePadding(10.0f, 6.0f);

// The padding inside a floating window: a dialog, an editor's window or an effect's window.
inline constexpr ImVec2 kDialogPadding(20.0f, 16.0f);

// The fonts. A font that didn't load is the interface font instead.
struct Fonts
{
    ImFont* text = nullptr;    // the interface font, with Japanese
    ImFont* strong = nullptr;  // the interface font in semibold, for titles and buttons
    ImFont* caps = nullptr;    // the semibold font spaced out, for labels in capitals
    ImFont* figures = nullptr; // a font whose digits share one width, for times and levels
};

const Fonts& GetFonts();
void SetFonts(const Fonts& loaded);

// Sets Dear ImGui's colours and sizes for the display's scale.
void Apply(ImGuiStyle& style, float scale);

// A length in points at the display's scale, or a pair of them.
float Px(float points);
ImVec2 Px(ImVec2 points);

// A font's size in points at the display's scale, rounded to a whole size. Dear ImGui draws a font's glyphs at whole
// sizes. It stretches them to any other size, and stretched glyphs come out blurred.
float FontPx(float points);

// A place or a length rounded to the display's pixels, or a pair of them. A shape whose edges lie on pixels comes out
// sharp. On a display with two pixels to the point, a pixel is half a point.
float OnPixels(float units);
ImVec2 OnPixels(ImVec2 units);

// `colour` with its alpha set to `alpha`, from 0 to 1.
ImU32 WithAlpha(ImU32 colour, float alpha);

// `from` moved toward `to` by `amount`, from 0 to 1.
ImU32 Mix(ImU32 from, ImU32 to, float amount);

// The text colour with the most contrast on `background`: kText or kTextOnColour.
ImU32 TextOn(ImU32 background);

} // namespace hokuto::app::theme
