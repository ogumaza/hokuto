// SPDX-License-Identifier: MIT

#include "app/system_fonts_shared.h"

#include <imgui_internal.h>

#include <cmath>

namespace hokuto::app::system_fonts
{

std::array<unsigned char, 256> ContrastTable(float contrast)
{
    std::array<unsigned char, 256> table{};
    for (std::size_t i = 0; i < table.size(); i++)
    {
        const float coverage = static_cast<float>(i) / 255.0f;
        const float alpha = coverage * (contrast + 1.0f) / (coverage * contrast + 1.0f);
        table[i] = static_cast<unsigned char>(std::lround(std::clamp(alpha, 0.0f, 1.0f) * 255.0f));
    }

    return table;
}

void PlaceBaseline(const ImFontConfig& src, ImFontBaked& baked, float ascent_per_line)
{
    if (!src.MergeMode)
    {
        baked.Ascent = IM_ROUND(baked.Size * ascent_per_line);
        baked.Descent = baked.Ascent - baked.Size;
    }
}

std::optional<GlyphFit> FitGlyph(bool figures, float digit_pixels, ImWchar codepoint,
                                 std::optional<float> advance_pixels)
{
    if (figures && codepoint == kFigureSpace && digit_pixels > 0.0f && advance_pixels != digit_pixels)
    {
        return GlyphFit{digit_pixels, 0.0f, true};
    }
    if (!advance_pixels)
    {
        return std::nullopt;
    }

    GlyphFit fit{*advance_pixels, 0.0f, false};
    if (figures && codepoint >= '0' && codepoint <= '9' && fit.advance_pixels < digit_pixels)
    {
        fit.shift_pixels = std::floor((digit_pixels - fit.advance_pixels) * 0.5f);
        fit.advance_pixels = digit_pixels;
    }

    return fit;
}

bool LoadWithoutBitmap(const GlyphFit& fit, ImWchar codepoint, float density, ImFontGlyph* out_glyph,
                       float* out_advance_x)
{
    const float advance_x = fit.advance_pixels / density;
    if (out_advance_x != nullptr)
    {
        *out_advance_x = advance_x;
        return true;
    }
    if (!fit.figure_space)
    {
        return false;
    }

    out_glyph->Codepoint = codepoint;
    out_glyph->AdvanceX = advance_x;
    return true;
}

theme::Fonts AddRoles(std::size_t families, bool strong, bool japanese, const FaceAdder& add_face)
{
    const auto add_role = [&](bool strong_role, bool figures, bool with_japanese, float spacing)
    {
        ImFont* font = nullptr;
        for (std::size_t i = 0; i < families; i++)
        {
            ImFont* added = add_face(i, strong_role, font != nullptr, figures && font == nullptr, spacing);
            font = font != nullptr ? font : added;
        }
        if (font != nullptr && with_japanese && japanese)
        {
            add_face(std::nullopt, false, true, false, 0.0f);
        }

        return font;
    };

    theme::Fonts fonts;
    fonts.text = add_role(false, false, true, 0.0f);
    if (fonts.text == nullptr)
    {
        return fonts;
    }
    if (strong)
    {
        fonts.strong = add_role(true, false, true, 0.0f);
        fonts.caps = add_role(true, false, false, 1.0f);
    }
    fonts.figures = add_role(false, true, true, 0.0f);

    return fonts;
}

} // namespace hokuto::app::system_fonts
