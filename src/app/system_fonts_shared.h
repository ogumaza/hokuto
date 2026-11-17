// SPDX-License-Identifier: MIT

// The rules that the font loaders of Windows and of the systems with fontconfig share. system_fonts_windows.cpp draws
// the glyphs with DirectWrite, and system_fonts_other.cpp with FreeType. Both draw the text at the type size of the
// Mac, with digits of one width in the figures. Both add the contrast of Windows text drawn without ClearType. Both use
// the baseline of the first font and the same roles. system_fonts_mac.cpp draws the text with Core Text, by other
// rules.

#pragma once

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <functional>
#include <optional>

#include "app/theme.h"

namespace hokuto::app::system_fonts
{

// The type size for each unit of a font's size. Dear ImGui sizes a font by its line. The Mac's interface font has a
// line of 1.18 times its type size.
inline constexpr float kTypePerSize = 1.0f / 1.18f;

// The figure space, as wide as a digit. Times pad their fields with it.
inline constexpr ImWchar kFigureSpace = 0x2007;

// What a loader keeps for each size of a source: the width of a digit of the figures, in pixels.
struct BakedSource
{
    float digit_pixels = 0.0f;
};

// The table from a pixel's coverage to its alpha with `contrast`, as Windows adds it to text drawn without ClearType.
// The contrast thickens the thin strokes that grayscale coverage gives light text on a dark background.
std::array<unsigned char, 256> ContrastTable(float contrast);

// The advance of the widest digit, in pixels. `advance_of` gives the advance of a character in whole pixels, or
// nothing if the face has no glyph for it. Every digit of the figures has this advance.
template <typename AdvanceOf>
float WidestDigitPixels(const AdvanceOf& advance_of)
{
    float widest = 0.0f;
    for (ImWchar digit = '0'; digit <= '9'; digit++)
    {
        widest = std::max(widest, advance_of(digit).value_or(0.0f));
    }

    return widest;
}

// Sets the baseline of `baked` where it divides the font's line at `ascent_per_line`. A merged source leaves the
// baseline that the first source set.
void PlaceBaseline(const ImFontConfig& src, ImFontBaked& baked, float ascent_per_line);

// How a source draws a glyph: the advance of the glyph in whole pixels, and the shift of the bitmap to the right in
// pixels. The figure space of the figures is blank.
struct GlyphFit
{
    float advance_pixels = 0.0f;
    float shift_pixels = 0.0f;
    bool figure_space = false;
};

// The fit of `codepoint` in a source. `figures` marks the figures' source, and `digit_pixels` is the width of its
// digits. `advance_pixels` is the advance of the face's glyph in whole pixels, or nothing if the face has no glyph for
// `codepoint`. Returns nothing if the source draws no glyph for it.
//
// In the figures' source, a digit narrower than the widest receives the widest one's advance, and is centred in it.
// The figure space is a blank glyph as wide as a digit, where the face has none or one of another width.
std::optional<GlyphFit> FitGlyph(bool figures, float digit_pixels, ImWchar codepoint,
                                 std::optional<float> advance_pixels);

// Completes a load of the glyph of `fit` that requires no bitmap: a request for the advance alone (`out_advance_x`),
// or the blank figure space. Returns false where the glyph requires a bitmap from the loader. The advance is in whole
// pixels at `density` pixels to the unit. Dear ImGui starts each line of text on a pixel. Every glyph then starts on
// one, and none is blurred by falling between pixels.
bool LoadWithoutBitmap(const GlyphFit& fit, ImWchar codepoint, float density, ImFontGlyph* out_glyph,
                       float* out_advance_x);

// Adds a face to the atlas as a source of a role, and returns its font, or nothing if the face can't be added. `family`
// is the face's family in the list of interface families, or nothing for the Japanese font. With `strong`, the face
// is the family's face for the strong text, where the family has one. With `merge`, the face fills in the glyphs that
// are missing from the font added before it. With `figures`, the source draws the figures. `spacing` is the space added
// after each glyph, in points.
using FaceAdder =
    std::function<ImFont*(std::optional<std::size_t> family, bool strong, bool merge, bool figures, float spacing)>;

// Adds the fonts of the roles through `add_face`, from `families` interface families. Each role is the first family
// whose face can be added, then the others for the characters that are missing from it, then the Japanese font where
// `japanese` is set. Every source draws on the first family's baseline. The strong text and the caps are added only
// where `strong` is set: where the first family has a face for the strong text. Japanese stays regular in the strong
// text, and the caps have no Japanese font. The figures are the text's font, with digits of one width and a figure
// space of that width. Every role is left out where the text has no font.
theme::Fonts AddRoles(std::size_t families, bool strong, bool japanese, const FaceAdder& add_face);

} // namespace hokuto::app::system_fonts
