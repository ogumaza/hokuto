// SPDX-License-Identifier: MIT

#include "app/widgets.h"

#include <imgui_internal.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "app/theme.h"

namespace hokuto::app
{
namespace
{

using theme::Px;

// The fader's range in dB, and the curve that gives the levels near 0 dB more of its travel.
constexpr float kFaderTop = 6.0f;
constexpr float kFaderBottom = -60.0f;
constexpr float kFaderCurve = 1.5f;

// The colours of a meter's sections, and the levels where they start.
constexpr float kMeterMidLevel = -18.0f;
constexpr float kMeterHighLevel = -6.0f;

// Half the width of the square of a point of a line of values, before the display's scale.
constexpr float kPointSize = 3.5f;

// The info line's height, before the display's scale.
constexpr float kInfoLineHeight = 28.0f;

// The level in dB for a height on a fader, from 0 to 1 (the inverse of FaderPosition).
float FaderLevel(float position)
{
    if (position <= 0.0f)
    {
        return kSilence;
    }

    return kFaderBottom + std::pow(std::min(position, 1.0f), 1.0f / kFaderCurve) * (kFaderTop - kFaderBottom);
}

// The point of an icon of `size` around `centre` at (`x`, `y`) times its size, on the display's pixels. An icon's edges
// then lie on pixels.
ImVec2 At(ImVec2 centre, float size, float x, float y)
{
    return theme::OnPixels(ImVec2(centre.x + size * x, centre.y + size * y));
}

// The whole pixels of the display that a stroke of `width` takes, one at least. A part of a pixel rounds up only from
// three quarters: a small icon's strokes stay thin on a display with one pixel to the point.
float StrokePixels(float width)
{
    const float density = std::max(ImGui::GetIO().DisplayFramebufferScale.x, 1.0f);
    return std::max(std::floor(width * density + 0.25f), 1.0f);
}

// A stroke's width in whole pixels of the display.
float StrokeWidth(float width)
{
    return StrokePixels(width) / std::max(ImGui::GetIO().DisplayFramebufferScale.x, 1.0f);
}

// A curve's width rounded to the nearest whole pixels of the display, one at least. A curve one pixel wide breaks up
// where it slants. A curve therefore rounds up sooner than a level or upright stroke.
float CurveWidth(float width)
{
    const float density = std::max(ImGui::GetIO().DisplayFramebufferScale.x, 1.0f);
    return std::max(std::round(width * density), 1.0f) / density;
}

// The middle of a stroke near `place`, for a stroke whose `width` is whole pixels of the display. A stroke an odd
// number of pixels wide has its middle in the middle of a pixel. An even one has its middle on the edge between two. A
// level or upright stroke there has its edges on pixels.
float StrokeMiddle(float place, float width)
{
    const float density = std::max(ImGui::GetIO().DisplayFramebufferScale.x, 1.0f);
    const auto pixels = static_cast<int>(std::round(width * density));
    const float middle = pixels % 2 == 1 ? std::floor(place * density) + 0.5f : std::round(place * density);
    return middle / density;
}

ImVec2 StrokeMiddle(ImVec2 point, float width)
{
    return ImVec2(StrokeMiddle(point.x, width), StrokeMiddle(point.y, width));
}

// Dear ImGui draws a line half a unit to the right of and below the places that it's given. This is the place to give
// for a level or upright stroke of `width` whose middle lies near `place`. The stroke's edges then lie on pixels.
float StrokePlace(float place, float width)
{
    return StrokeMiddle(place, StrokeWidth(width)) - 0.5f;
}

// The radius nearest `radius` for a ring whose `width` is whole pixels of the display, around a centre on the corner of
// a pixel. The ring's edges then lie on pixels at its top, bottom, left and right.
float RingRadius(float radius, float width)
{
    const float density = std::max(ImGui::GetIO().DisplayFramebufferScale.x, 1.0f);
    const float half = width * density * 0.5f;
    return (std::max(std::round(radius * density - half), 0.0f) + half) / density;
}

// Draws a stroke of an icon from `a` to `b`. A level or upright stroke lies on pixels. A slanted stroke is smoothed
// whatever its place, and keeps its width.
void IconLine(ImDrawList* draw, ImVec2 a, ImVec2 b, ImU32 colour, float width)
{
    if (a.x == b.x)
    {
        const float x = StrokePlace(a.x, width);
        draw->AddLine(ImVec2(x, a.y - 0.5f), ImVec2(x, b.y - 0.5f), colour, StrokeWidth(width));
    }
    else if (a.y == b.y)
    {
        const float y = StrokePlace(a.y, width);
        draw->AddLine(ImVec2(a.x - 0.5f, y), ImVec2(b.x - 0.5f, y), colour, StrokeWidth(width));
    }
    else
    {
        draw->AddLine(a, b, colour, width);
    }
}

// Draws the border of a rectangle of an icon, inside the rectangle from `min` to `max`. Dear ImGui centres a border
// half a unit inside the corners that it's given. The border's edges lie on pixels.
void IconFrame(ImDrawList* draw, ImVec2 min, ImVec2 max, ImU32 colour, float rounding, float width)
{
    const float stroke = StrokeWidth(width);
    const float inset = stroke * 0.5f;
    draw->AddRect(ImVec2(min.x + inset - 0.5f, min.y + inset - 0.5f),
                  ImVec2(max.x - inset + 0.49f, max.y - inset + 0.49f), colour, rounding, stroke);
}

// An arc from angle `from` to angle `to`, in radians clockwise from the right, with an arrowhead at `to`. `centre` has
// to lie on the corner of a pixel.
void ArcArrow(ImDrawList* draw, ImVec2 centre, float radius, float from, float to, float stroke, ImU32 colour)
{
    const float width = CurveWidth(stroke);
    radius = RingRadius(radius, width);
    draw->PathArcTo(centre, radius, from, to, 20);
    draw->PathStroke(colour, width);

    const ImVec2 point(centre.x + radius * std::cos(to), centre.y + radius * std::sin(to));
    const ImVec2 along(-std::sin(to), std::cos(to));
    const ImVec2 out(std::cos(to), std::sin(to));
    const float head = stroke * 2.0f;
    draw->AddTriangleFilled(ImVec2(point.x + along.x * head * 1.2f, point.y + along.y * head * 1.2f),
                            ImVec2(point.x - out.x * head, point.y - out.y * head),
                            ImVec2(point.x + out.x * head, point.y + out.y * head), colour);
}

std::string Capitals(std::string_view text)
{
    std::string capitals(text);
    for (char& c : capitals)
    {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }

    return capitals;
}

// `font`, set to draw at the display's density. Dear ImGui sets a font's density only while the font is current, and
// most of the program's text comes from fonts that never are. A font left at the density of one pixel to the point
// comes out blurred on a Retina display.
ImFont* AtDisplayDensity(ImFont* font)
{
    font->CurrentRasterizerDensity = ImGui::GetCurrentContext()->FontRasterizerDensity;

    return font;
}

// Where `value` lies on a slider of `range`, from 0 at the left to 1 at the right. A value outside the range lies at
// the nearer end.
float SliderPosition(float value, const SliderRange& range)
{
    const float position = range.logarithmic ? std::log(value / range.low) / std::log(range.high / range.low)
                                             : (value - range.low) / (range.high - range.low);
    return position > 0.0f ? std::min(position, 1.0f) : 0.0f;
}

// The value at `position` on a slider of `range`.
float SliderValue(float position, const SliderRange& range)
{
    if (range.logarithmic)
    {
        return range.low * std::pow(range.high / range.low, position);
    }

    return range.low + (range.high - range.low) * position;
}

// Lets Dear ImGui's text field grow the std::string that `data` types into.
int GrowText(ImGuiInputTextCallbackData* data)
{
    if (data->EventFlag == ImGuiInputTextFlags_CallbackResize)
    {
        auto* text = static_cast<std::string*>(data->UserData);
        text->resize(static_cast<std::size_t>(data->BufTextLen));
        data->Buf = text->data();
    }

    return 0;
}

// The text of a tooltip that the caller has begun, with its lines wrapped at a width that reads well.
void WrappedText(std::string_view text)
{
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
    ImGui::TextUnformatted(text.data(), text.data() + text.size());
    ImGui::PopTextWrapPos();
}

// The lines of `text` in `font` at `size` points, broken between words to fit `width`.
std::vector<std::string_view> WrappedLines(ImFont* font, float size, std::string_view text, float width)
{
    std::vector<std::string_view> lines;
    const char* begin = text.data();
    const char* const end = text.data() + text.size();
    while (begin < end)
    {
        const char* line_end = AtDisplayDensity(font)->CalcWordWrapPosition(theme::FontPx(size), begin, end, width);
        if (line_end == begin)
        {
            line_end = begin + 1;
        }
        std::string_view line(begin, static_cast<std::size_t>(line_end - begin));
        while (!line.empty() && (line.back() == ' ' || line.back() == '\n'))
        {
            line.remove_suffix(1);
        }
        lines.push_back(line);
        begin = line_end;
        while (begin < end && (*begin == ' ' || *begin == '\n'))
        {
            begin++;
        }
    }

    return lines;
}

} // namespace

// Dear ImGui smooths the edges of a filled shape only when the shape's points run clockwise on the screen. The edges of
// a shape whose points run the other way come out jagged, and half a pixel further out. Every shape here lists its
// points clockwise.
void DrawIcon(ImDrawList* draw, Icon icon, ImVec2 centre, float size, ImU32 colour)
{
    // The point at (`x`, `y`) times the icon's size from its centre, on the display's pixels (At).
    const auto at = [centre, size](float x, float y)
    {
        return At(centre, size, x, y);
    };

    const float stroke = std::max(1.2f, size * 0.1f);
    switch (icon)
    {
    case Icon::kGoToStart:
        draw->AddRectFilled(at(-0.36f, -0.3f), at(-0.25f, 0.3f), colour, 1.0f);
        draw->AddTriangleFilled(at(0.34f, -0.3f), at(0.34f, 0.3f), at(-0.2f, 0.0f), colour);
        break;

    case Icon::kRewind:
        draw->AddTriangleFilled(at(0.02f, -0.27f), at(0.02f, 0.27f), at(-0.4f, 0.0f), colour);
        draw->AddTriangleFilled(at(0.42f, -0.27f), at(0.42f, 0.27f), at(0.0f, 0.0f), colour);
        break;

    case Icon::kForward:
        draw->AddTriangleFilled(at(-0.42f, -0.27f), at(0.0f, 0.0f), at(-0.42f, 0.27f), colour);
        draw->AddTriangleFilled(at(-0.02f, -0.27f), at(0.4f, 0.0f), at(-0.02f, 0.27f), colour);
        break;

    case Icon::kStop:
        draw->AddRectFilled(at(-0.27f, -0.27f), at(0.27f, 0.27f), colour, size * 0.07f);
        break;

    case Icon::kPlay:
        draw->AddTriangleFilled(at(-0.26f, -0.34f), at(0.38f, 0.0f), at(-0.26f, 0.34f), colour);
        break;

    case Icon::kCycle:
        ArcArrow(draw, at(0.0f, 0.0f), size * 0.3f, IM_PI * 1.05f, IM_PI * 1.85f, stroke, colour);
        ArcArrow(draw, at(0.0f, 0.0f), size * 0.3f, IM_PI * 0.05f, IM_PI * 0.85f, stroke, colour);
        break;

    // A window with the zone filled in: the inspector on the left, and the browser on the right.
    case Icon::kInspector:
        IconFrame(draw, at(-0.42f, -0.34f), at(0.42f, 0.34f), colour, size * 0.1f, stroke);
        draw->AddRectFilled(at(-0.42f, -0.34f), at(-0.1f, 0.34f), colour, size * 0.1f, ImDrawFlags_RoundCornersLeft);
        break;

    case Icon::kBrowser:
        IconFrame(draw, at(-0.42f, -0.34f), at(0.42f, 0.34f), colour, size * 0.1f, stroke);
        draw->AddRectFilled(at(0.1f, -0.34f), at(0.42f, 0.34f), colour, size * 0.1f, ImDrawFlags_RoundCornersRight);
        break;

    // Two switches: one on, with its knob on the right, and one off.
    case Icon::kVariables:
        IconFrame(draw, at(-0.4f, -0.36f), at(0.4f, -0.06f), colour, size * 0.15f, stroke);
        draw->AddCircleFilled(at(0.25f, -0.21f), size * 0.09f, colour);
        IconFrame(draw, at(-0.4f, 0.06f), at(0.4f, 0.36f), colour, size * 0.15f, stroke);
        draw->AddCircleFilled(at(-0.25f, 0.21f), size * 0.09f, colour);
        break;

    case Icon::kMixer:
        {
            constexpr std::array<float, 3> kCaps = {0.14f, -0.18f, 0.04f};
            for (int i = 0; i < 3; i++)
            {
                const float x = static_cast<float>(i - 1) * 0.3f;
                IconLine(draw, at(x, -0.38f), at(x, 0.38f), colour, stroke);
                draw->AddRectFilled(at(x - 0.11f, kCaps[i] - 0.07f), at(x + 0.11f, kCaps[i] + 0.07f), colour, 1.0f);
            }
            break;
        }

    case Icon::kHistory:
        ArcArrow(draw, at(0.0f, 0.0f), size * 0.36f, IM_PI * 1.45f, IM_PI * 3.05f, stroke, colour);
        IconLine(draw, at(0.0f, 0.0f), at(0.0f, -0.2f), colour, stroke);
        IconLine(draw, at(0.0f, 0.0f), at(0.15f, 0.08f), colour, stroke);
        break;

    case Icon::kSetup:
        IconLine(draw, at(-0.4f, -0.17f), at(0.4f, -0.17f), colour, stroke);
        IconLine(draw, at(-0.4f, 0.17f), at(0.4f, 0.17f), colour, stroke);
        draw->AddCircleFilled(at(0.14f, -0.17f), size * 0.11f, colour);
        draw->AddCircleFilled(at(-0.16f, 0.17f), size * 0.11f, colour);
        break;

    case Icon::kSearch:
        {
            // Dear ImGui draws a ring half a unit inside the radius that it's given.
            const float width = CurveWidth(stroke);
            draw->AddCircle(at(-0.07f, -0.07f), RingRadius(size * 0.25f, width) + 0.5f, colour, 0, width);
            IconLine(draw, at(0.12f, 0.12f), at(0.34f, 0.34f), colour, stroke);
            break;
        }

    // A chevron's strokes slant at 45 degrees from its tip. The tip lies where a stroke of the chevron's width has its
    // middle.
    case Icon::kChevronRight:
        {
            const float width = CurveWidth(stroke);
            const float leg = theme::OnPixels(size * 0.24f);
            const ImVec2 tip = StrokeMiddle(at(0.14f, 0.0f), width);
            const std::array<ImVec2, 3> points = {ImVec2(tip.x - leg, tip.y - leg), tip,
                                                  ImVec2(tip.x - leg, tip.y + leg)};
            draw->AddPolyline(points.data(), 3, colour, width);
            break;
        }

    case Icon::kChevronDown:
        {
            const float width = CurveWidth(stroke);
            const float leg = theme::OnPixels(size * 0.24f);
            const ImVec2 tip = StrokeMiddle(at(0.0f, 0.14f), width);
            const std::array<ImVec2, 3> points = {ImVec2(tip.x - leg, tip.y - leg), tip,
                                                  ImVec2(tip.x + leg, tip.y - leg)};
            draw->AddPolyline(points.data(), 3, colour, width);
            break;
        }

    case Icon::kSound:
        {
            draw->AddRectFilled(at(-0.38f, -0.12f), at(-0.2f, 0.12f), colour, 1.0f);
            draw->AddQuadFilled(at(-0.2f, -0.12f), at(0.04f, -0.34f), at(0.04f, 0.34f), at(-0.2f, 0.12f), colour);
            // The waves keep a pixel between them at least.
            const float width = CurveWidth(stroke);
            const float inner = RingRadius(size * 0.2f, width);
            const float pixel = 1.0f / std::max(ImGui::GetIO().DisplayFramebufferScale.x, 1.0f);
            const float outer = std::max(RingRadius(size * 0.36f, width), inner + width + pixel);
            draw->PathArcTo(at(0.04f, 0.0f), inner, -0.8f, 0.8f, 8);
            draw->PathStroke(colour, width);
            draw->PathArcTo(at(0.04f, 0.0f), outer, -0.8f, 0.8f, 10);
            draw->PathStroke(colour, width);
            break;
        }

    case Icon::kSequence:
        draw->AddRectFilled(at(-0.4f, -0.32f), at(0.02f, -0.15f), colour, 1.0f);
        draw->AddRectFilled(at(-0.12f, -0.08f), at(0.4f, 0.09f), colour, 1.0f);
        draw->AddRectFilled(at(-0.32f, 0.16f), at(0.12f, 0.33f), colour, 1.0f);
        break;

    case Icon::kBank:
        for (int i = 0; i < 4; i++)
        {
            const float x = i % 2 == 0 ? -0.36f : 0.04f;
            const float y = i / 2 == 0 ? -0.36f : 0.04f;
            draw->AddRectFilled(at(x, y), at(x + 0.32f, y + 0.32f), colour, size * 0.06f);
        }
        break;

    case Icon::kWave:
        {
            std::array<ImVec2, 13> points;
            for (std::size_t i = 0; i < points.size(); i++)
            {
                const float t = static_cast<float>(i) / static_cast<float>(points.size() - 1);
                points[i] = at(-0.42f + 0.84f * t, -0.28f * std::sin(t * 2.0f * IM_PI));
            }
            draw->AddPolyline(points.data(), static_cast<int>(points.size()), colour, stroke);
            break;
        }

    // The mouse's arrow: a head that points up and to the left, and a tail.
    case Icon::kPointer:
        draw->AddTriangleFilled(at(-0.24f, -0.42f), at(0.24f, 0.08f), at(-0.24f, 0.24f), colour);
        draw->AddQuadFilled(at(0.04f, 0.02f), at(0.15f, 0.35f), at(0.04f, 0.4f), at(-0.08f, 0.06f), colour);
        break;

    // A pencil that slants up to the right, with its point at the bottom left.
    case Icon::kPencil:
        draw->AddTriangleFilled(at(-0.38f, 0.38f), at(-0.29f, 0.12f), at(-0.12f, 0.29f), colour);
        draw->AddQuadFilled(at(-0.25f, 0.08f), at(0.16f, -0.33f), at(0.33f, -0.16f), at(-0.08f, 0.25f), colour);
        break;

    // An eraser that slants up to the right. Its rubber end at the bottom left is filled.
    case Icon::kEraser:
        draw->AddQuad(at(-0.38f, 0.09f), at(-0.13f, 0.34f), at(0.38f, -0.17f), at(0.13f, -0.42f), colour, stroke);
        draw->AddQuadFilled(at(-0.38f, 0.09f), at(-0.17f, -0.12f), at(0.08f, 0.13f), at(-0.13f, 0.34f), colour);
        break;

    // A magnet: a U with its poles at the top.
    case Icon::kSnap:
        {
            const float width = CurveWidth(stroke * 1.8f);
            const ImVec2 centre = at(0.0f, 0.04f);
            const float radius = RingRadius(size * 0.24f, width);
            const float top = at(0.0f, -0.22f).y;
            draw->PathLineTo(ImVec2(centre.x - radius, top));
            draw->PathArcTo(centre, radius, IM_PI, 0.0f, 12);
            draw->PathLineTo(ImVec2(centre.x + radius, top));
            draw->PathStroke(colour, width);

            // The poles sit on the legs, a little wider than them.
            const float pole = width * 0.5f + theme::OnPixels(std::max(size * 0.1f - width * 0.5f, 0.0f));
            const float pole_top = at(0.0f, -0.42f).y;
            const float pole_bottom = at(0.0f, -0.28f).y;
            for (const float leg : {centre.x - radius, centre.x + radius})
            {
                draw->AddRectFilled(ImVec2(leg - pole, pole_top), ImVec2(leg + pole, pole_bottom), colour);
            }
            break;
        }

    // A plus.
    case Icon::kAdd:
        IconLine(draw, at(-0.3f, 0.0f), at(0.3f, 0.0f), colour, stroke * 1.4f);
        IconLine(draw, at(0.0f, -0.3f), at(0.0f, 0.3f), colour, stroke * 1.4f);
        break;
    }
}

void DrawText(ImDrawList* draw, ImFont* font, float size, ImVec2 pos, ImU32 colour, std::string_view text)
{
    draw->AddText(AtDisplayDensity(font), theme::FontPx(size), ImFloor(pos), colour, text.data(),
                  text.data() + text.size());
}

ImVec2 TextSize(ImFont* font, float size, std::string_view text)
{
    return AtDisplayDensity(font)->CalcTextSizeA(theme::FontPx(size), FLT_MAX, 0.0f, text.data(),
                                                 text.data() + text.size());
}

float Ascent(ImFont* font, float size)
{
    const ImFontBaked* baked = AtDisplayDensity(font)->GetFontBaked(theme::FontPx(size));
    return baked->Ascent * theme::FontPx(size) / baked->Size;
}

void DrawCaps(ImDrawList* draw, ImVec2 pos, ImU32 colour, std::string_view text)
{
    DrawText(draw, theme::GetFonts().caps, theme::kCapsSize, pos, colour, Capitals(text));
}

ImVec2 CapsSize(std::string_view text)
{
    return TextSize(theme::GetFonts().caps, theme::kCapsSize, Capitals(text));
}

bool IconButton(const char* id, Icon icon, ImVec2 size, bool on, ImU32 on_colour, const char* tooltip)
{
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton(id, size);
    const bool hovered = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();

    // A lit button keeps a tint of its colour. An unlit one shows a frame only under the mouse.
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 end(pos.x + size.x, pos.y + size.y);
    if (on)
    {
        draw->AddRectFilled(pos, end, theme::WithAlpha(on_colour, held ? 0.3f : hovered ? 0.24f : 0.16f), Px(6.0f));
    }
    else if (held || hovered)
    {
        draw->AddRectFilled(pos, end, held ? theme::kControlActive : theme::kControlHover, Px(6.0f));
    }
    const ImU32 colour = on ? on_colour : hovered ? theme::kText : theme::kTextMuted;
    DrawIcon(draw, icon, ImVec2(pos.x + size.x * 0.5f, pos.y + size.y * 0.5f), std::min(size.x, size.y) * 0.5f, colour);

    if (tooltip != nullptr && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
    {
        WrappedTooltip(tooltip);
    }

    return clicked;
}

void DrawLetterButton(ImDrawList* draw, ImVec2 min, ImVec2 max, const char* letter, bool on, ImU32 on_colour,
                      bool hovered)
{
    const ImU32 fill = on ? on_colour : hovered ? theme::kControlHover : theme::kControl;
    draw->AddRectFilled(min, max, fill, Px(4.0f));

    ImFont* font = theme::GetFonts().strong;
    const ImVec2 size = TextSize(font, theme::kSmallSize, letter);
    const ImVec2 pos((min.x + max.x - size.x) * 0.5f, (min.y + max.y - size.y) * 0.5f);
    DrawText(draw, font, theme::kSmallSize, pos, on ? theme::kTextOnColour : theme::kText, letter);
}

bool Fader(const char* id, float* level, ImVec2 size)
{
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, size);
    const bool hovered = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();
    const ImGuiIO& io = ImGui::GetIO();

    // The fader follows the mouse from where it is. It doesn't jump to the click.
    const float cap_height = Px(24.0f);
    const float travel = std::max(size.y - cap_height, 1.0f);
    bool changed = false;
    if (ImGui::IsItemActivated() && io.KeyCtrl)
    {
        *level = 0.0f;
        changed = true;
    }
    else if (held && io.MouseDelta.y != 0.0f && !io.KeyCtrl)
    {
        const float step = io.MouseDelta.y / travel * (io.KeyShift ? 0.1f : 1.0f);
        *level = FaderLevel(std::clamp(FaderPosition(*level) - step, 0.0f, 1.0f));
        changed = true;
    }

    // The slot, a mark at 0 dB, and the cap.
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float centre = std::floor(pos.x + size.x * 0.5f);
    const float top = pos.y + cap_height * 0.5f;
    draw->AddRectFilled(ImVec2(centre - Px(2.0f), top), ImVec2(centre + Px(2.0f), top + travel), theme::kDisplay,
                        Px(2.0f));
    const float unity = std::floor(top + travel * (1.0f - FaderPosition(0.0f)));
    draw->AddRectFilled(ImVec2(centre - Px(9.0f), unity), ImVec2(centre - Px(4.0f), unity + Px(1.0f)), theme::kTextDim);
    draw->AddRectFilled(ImVec2(centre + Px(4.0f), unity), ImVec2(centre + Px(9.0f), unity + Px(1.0f)), theme::kTextDim);

    const float cap_top = std::floor(pos.y + travel * (1.0f - FaderPosition(*level)));
    const float half_width = std::min(size.x * 0.5f, Px(14.0f));
    const ImU32 cap = held ? theme::kFaderCapHeld : hovered ? theme::kFaderCapHover : theme::kFaderCap;
    draw->AddRectFilled(ImVec2(centre - half_width, cap_top), ImVec2(centre + half_width, cap_top + cap_height), cap,
                        Px(4.0f));
    draw->AddRectFilled(ImVec2(centre - half_width + Px(3.0f), cap_top + cap_height * 0.5f - Px(1.0f)),
                        ImVec2(centre + half_width - Px(3.0f), cap_top + cap_height * 0.5f), theme::kDisplay);

    return changed;
}

bool PanKnob(const char* id, float* pan, float radius)
{
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, ImVec2(radius * 2.0f, radius * 2.0f));
    const bool hovered = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();
    const ImGuiIO& io = ImGui::GetIO();

    // Dragging up turns the knob right.
    bool changed = false;
    if (ImGui::IsItemActivated() && io.KeyCtrl)
    {
        *pan = 0.0f;
        changed = true;
    }
    else if (held && io.MouseDelta.y != 0.0f && !io.KeyCtrl)
    {
        *pan = std::clamp(*pan - io.MouseDelta.y * (io.KeyShift ? 0.001f : 0.01f), -1.0f, 1.0f);
        changed = true;
    }

    // The knob turns through 270 degrees. The centre position is at the top. The arc runs from the centre to the value.
    // The knob's centre lies on the corner of a pixel, and its edges and its track's edges on pixels.
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 centre = theme::OnPixels(ImVec2(pos.x + radius, pos.y + radius));
    draw->AddCircleFilled(centre, theme::OnPixels(radius), hovered || held ? theme::kControlHover : theme::kControl,
                          32);
    const float track = CurveWidth(Px(2.5f));
    const float track_radius = RingRadius(radius - Px(2.5f), track);
    draw->PathArcTo(centre, track_radius, IM_PI * 0.75f, IM_PI * 2.25f, 32);
    draw->PathStroke(theme::kDisplay, track);
    const float angle = IM_PI * 1.5f + *pan * IM_PI * 0.75f;
    const ImVec2 inner(centre.x + std::cos(angle) * radius * 0.2f, centre.y + std::sin(angle) * radius * 0.2f);
    const ImVec2 outer(centre.x + std::cos(angle) * radius * 0.62f, centre.y + std::sin(angle) * radius * 0.62f);
    if (std::abs(*pan) > 0.005f)
    {
        draw->PathArcTo(centre, track_radius, std::min(angle, IM_PI * 1.5f), std::max(angle, IM_PI * 1.5f), 24);
        draw->PathStroke(theme::kAccent, track);
        draw->AddLine(inner, outer, theme::kText, Px(2.0f));
    }
    else
    {
        IconLine(draw, ImVec2(centre.x, theme::OnPixels(outer.y)), ImVec2(centre.x, theme::OnPixels(inner.y)),
                 theme::kText, Px(2.0f));
    }

    return changed;
}

bool SendSlider(const char* id, const char* bus, float* level, ImVec2 size)
{
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, size);
    const bool hovered = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();
    const ImGuiIO& io = ImGui::GetIO();

    // Dragging right raises the level. The slider follows the mouse from where it is.
    bool changed = false;
    if (ImGui::IsItemActivated() && io.KeyCtrl)
    {
        *level = 0.0f;
        changed = true;
    }
    else if (held && io.MouseDelta.x != 0.0f && !io.KeyCtrl)
    {
        const float step = io.MouseDelta.x / std::max(size.x, 1.0f) * (io.KeyShift ? 0.1f : 1.0f);
        *level = FaderLevel(std::clamp(FaderPosition(*level) + step, 0.0f, 1.0f));
        changed = true;
    }

    // The slot, a bar in the FX channels' colour up to the level, the bus on the left and the level on the right.
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 max(pos.x + size.x, pos.y + size.y);
    draw->AddRectFilled(pos, max, hovered || held ? theme::kControlHover : theme::kControl, Px(4.0f));
    const float fill = std::floor(pos.x + size.x * FaderPosition(*level));
    if (fill > pos.x)
    {
        const ImDrawFlags corners = fill >= max.x ? ImDrawFlags_RoundCornersAll : ImDrawFlags_RoundCornersLeft;
        draw->AddRectFilled(pos, ImVec2(fill, max.y), theme::Mix(theme::kAccent, theme::kRaised, 0.45f), Px(4.0f),
                            corners);
    }
    ImFont* text = theme::GetFonts().text;
    ImFont* figures = theme::GetFonts().figures;
    const std::string value = LevelText(*level);
    const ImVec2 bus_size = TextSize(text, theme::kSmallSize, bus);
    const ImVec2 value_size = TextSize(figures, theme::kSmallSize, value);
    draw->PushClipRect(pos, max, true);
    DrawText(draw, text, theme::kSmallSize, ImVec2(pos.x + Px(6.0f), std::floor(pos.y + (size.y - bus_size.y) * 0.5f)),
             theme::kText, bus);
    DrawText(draw, figures, theme::kSmallSize,
             ImVec2(max.x - Px(6.0f) - value_size.x, std::floor(pos.y + (size.y - value_size.y) * 0.5f)),
             theme::kTextMuted, value);
    draw->PopClipRect();

    return changed;
}

std::string PanText(float pan)
{
    const int value = static_cast<int>(std::lround(std::abs(pan) * 100.0f));
    if (value == 0)
    {
        return "C";
    }

    return (pan < 0.0f ? "L" : "R") + std::to_string(value);
}

std::string LevelText(float level)
{
    if (level < -59.95f)
    {
        return "-∞";
    }

    std::array<char, 16> text{};
    const float rounded = std::round(level * 10.0f) / 10.0f;
    std::snprintf(text.data(), text.size(), rounded > 0.0f ? "+%.1f" : "%.1f", rounded == 0.0f ? 0.0f : rounded);

    return text.data();
}

bool ValueSlider(const char* id, float* value, const SliderRange& range, float reset, const char* format, ImVec2 size,
                 const SliderRange* typed)
{
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    const ImGuiID item = window->GetID(id);
    const ImVec2 pos = window->DC.CursorPos;
    const ImRect box(pos, ImVec2(pos.x + size.x, pos.y + size.y));
    ImGui::ItemSize(box);
    if (!ImGui::ItemAdd(box, item, nullptr, ImGuiItemFlags_Inputable))
    {
        return false;
    }

    // A double-click types a value, and so does Tab from the field before. Dear ImGui's text input takes the slider's
    // place until Return, Escape or a click elsewhere closes it.
    const ImGuiContext& context = *ImGui::GetCurrentContext();
    const bool typing = ImGui::TempInputIsActive(item);
    bool hovered = false;
    bool held = false;
    if (!typing)
    {
        ImGui::ButtonBehavior(box, item, &hovered, &held);
    }
    const bool tabbed =
        context.NavActivateId == item && (context.NavActivateFlags & ImGuiActivateFlags_PreferInput) != 0;
    if (typing || tabbed || (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)))
    {
        const SliderRange& limits = typed != nullptr ? *typed : range;
        return ImGui::TempInputScalar(box, item, id, ImGuiDataType_Float, value, format, &limits.low, &limits.high);
    }

    // A drag moves the slider from where the drag started. The position stays unrounded in Dear ImGui's storage while
    // the caller rounds the value. A slow drag then still moves the value.
    const ImGuiIO& io = ImGui::GetIO();
    ImGuiStorage* storage = ImGui::GetStateStorage();
    bool changed = false;
    if (ImGui::IsItemActivated())
    {
        if (io.KeyCtrl)
        {
            *value = reset;
            changed = true;
        }
        storage->SetFloat(item, SliderPosition(*value, range));
    }
    else if (held && io.MouseDelta.x != 0.0f && !io.KeyCtrl)
    {
        const float step = io.MouseDelta.x / std::max(size.x, 1.0f) * (io.KeyShift ? 0.1f : 1.0f);
        const float position = std::clamp(storage->GetFloat(item) + step, 0.0f, 1.0f);
        storage->SetFloat(item, position);
        *value = SliderValue(position, range);
        changed = true;
    }
    if (changed)
    {
        ImGui::MarkItemEdited(item);
    }

    // The track, filled from the left up to the value, with a line at the value's end. Inside a disabled block, the
    // style's alpha fades it.
    ImDrawList* draw = window->DrawList;
    const float rounding = Px(4.0f);
    draw->AddRectFilled(box.Min, box.Max, ImGui::GetColorU32(theme::kDisplay), rounding);
    const float end = std::floor(box.Min.x + size.x * SliderPosition(*value, range));
    if (end > box.Min.x)
    {
        const float alpha = held ? 0.5f : hovered ? 0.42f : 0.34f;
        const ImDrawFlags corners = end >= box.Max.x ? ImDrawFlags_RoundCornersAll : ImDrawFlags_RoundCornersLeft;
        draw->AddRectFilled(box.Min, ImVec2(end, box.Max.y), ImGui::GetColorU32(theme::kAccent, alpha), rounding,
                            corners);
    }
    const float line = std::clamp(end, box.Min.x + Px(2.0f), box.Max.x - Px(2.0f));
    draw->AddRectFilled(ImVec2(line - Px(1.0f), box.Min.y + Px(3.0f)), ImVec2(line + Px(1.0f), box.Max.y - Px(3.0f)),
                        ImGui::GetColorU32(theme::kAccent));
    draw->AddRect(box.Min, box.Max, ImGui::GetColorU32(held || hovered ? theme::kBorderStrong : theme::kBorder),
                  rounding, Px(1.0f));

    // A value too long for the slider, such as one far outside its range, is cut off at the slider's ends.
    char text[64];
    ImGui::DataTypeFormatString(text, IM_COUNTOF(text), ImGuiDataType_Float, value, format);
    ImFont* font = theme::GetFonts().figures;
    const ImVec2 text_size = TextSize(font, theme::kTextSize, text);
    draw->PushClipRect(box.Min, box.Max, true);
    DrawText(draw, font, theme::kTextSize,
             ImVec2((box.Min.x + box.Max.x - text_size.x) * 0.5f, (box.Min.y + box.Max.y - text_size.y) * 0.5f),
             ImGui::GetColorU32(theme::kText), text);
    draw->PopClipRect();

    return changed;
}

void DrawMeter(ImDrawList* draw, ImVec2 min, ImVec2 max, float level)
{
    draw->AddRectFilled(min, max, theme::kDisplay, Px(1.5f));
    const float position = FaderPosition(level);
    if (position <= 0.0f)
    {
        return;
    }

    // Fill from the bottom, one section at a time.
    const float height = max.y - min.y;
    const float top = max.y - height * position;
    const float mid = max.y - height * FaderPosition(kMeterMidLevel);
    const float high = max.y - height * FaderPosition(kMeterHighLevel);
    draw->AddRectFilled(ImVec2(min.x, std::max(top, mid)), max, theme::kPlay);
    if (top < mid)
    {
        draw->AddRectFilled(ImVec2(min.x, std::max(top, high)), ImVec2(max.x, mid), theme::kMeterMid);
    }
    if (top < high)
    {
        draw->AddRectFilled(ImVec2(min.x, top), ImVec2(max.x, high), theme::kMeterHigh);
    }
}

float FaderPosition(float level)
{
    if (level <= kFaderBottom)
    {
        return 0.0f;
    }

    return std::pow((std::min(level, kFaderTop) - kFaderBottom) / (kFaderTop - kFaderBottom), kFaderCurve);
}

void DrawHorizontalMeter(ImDrawList* draw, ImVec2 min, ImVec2 max, float level)
{
    draw->AddRectFilled(min, max, theme::kDisplay, Px(1.5f));
    const float position = FaderPosition(level);
    if (position <= 0.0f)
    {
        return;
    }

    // Fill from the left, one section at a time.
    const float width = max.x - min.x;
    const float end = min.x + width * position;
    const float mid = min.x + width * FaderPosition(kMeterMidLevel);
    const float high = min.x + width * FaderPosition(kMeterHighLevel);
    draw->AddRectFilled(min, ImVec2(std::min(end, mid), max.y), theme::kPlay);
    if (end > mid)
    {
        draw->AddRectFilled(ImVec2(mid, min.y), ImVec2(std::min(end, high), max.y), theme::kMeterMid);
    }
    if (end > high)
    {
        draw->AddRectFilled(ImVec2(high, min.y), ImVec2(end, max.y), theme::kMeterHigh);
    }
}

void DrawPoint(ImDrawList* draw, float x, float top, float bottom, ImU32 colour, bool selected, bool conditional)
{
    const ImU32 fill = selected ? theme::kText : colour;
    if (bottom > top)
    {
        draw->AddRectFilled(ImVec2(x - Px(1.0f), top), ImVec2(x + Px(1.0f), bottom), theme::WithAlpha(fill, 0.7f));
    }

    const float y = std::floor((top + bottom) * 0.5f);
    const float size = Px(kPointSize);
    const ImVec2 min(x - size, y - size);
    const ImVec2 max(x + size, y + size);
    draw->AddRectFilled(min, max, conditional ? theme::kPanel : fill);
    draw->AddRect(min, max, selected ? colour : fill, 0.0f, Px(1.0f));
}

std::string ShortcutText(ImGuiKeyChord keys)
{
    std::string text;
#ifdef __APPLE__
    if ((keys & ImGuiMod_Super) != 0)
    {
        text += "⌃"; // Control
    }
    if ((keys & ImGuiMod_Alt) != 0)
    {
        text += "⌥"; // Option
    }
    if ((keys & ImGuiMod_Shift) != 0)
    {
        text += "⇧"; // Shift
    }
    if ((keys & ImGuiMod_Ctrl) != 0)
    {
        text += "⌘"; // Command
    }
#else
    if ((keys & ImGuiMod_Ctrl) != 0)
    {
        text += "Ctrl+";
    }
    if ((keys & ImGuiMod_Shift) != 0)
    {
        text += "Shift+";
    }
    if ((keys & ImGuiMod_Alt) != 0)
    {
        text += "Alt+";
    }
#endif

    // The names of the keypad's keys. Dear ImGui calls them Keypad0, KeypadEnter and so on.
    const auto key = static_cast<ImGuiKey>(keys & ~ImGuiMod_Mask_);
    if (key >= ImGuiKey_Keypad0 && key <= ImGuiKey_Keypad9)
    {
        return text + "Num " + static_cast<char>('0' + (key - ImGuiKey_Keypad0));
    }
    switch (key)
    {
    case ImGuiKey_KeypadDecimal:
        return text + "Num .";
    case ImGuiKey_KeypadDivide:
        return text + "Num /";
    case ImGuiKey_KeypadMultiply:
        return text + "Num *";
    case ImGuiKey_KeypadSubtract:
        return text + "Num -";
    case ImGuiKey_KeypadAdd:
        return text + "Num +";
    case ImGuiKey_KeypadEnter:
        return text + "Num Enter";
    default:
        return text + ImGui::GetKeyName(key);
    }
}

InfoLine BeginInfoLine()
{
    InfoLine line;
    line.draw = ImGui::GetWindowDrawList();
    line.origin = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    line.bottom = line.origin.y + Px(kInfoLineHeight);
    line.middle = (line.origin.y + line.bottom) * 0.5f;
    line.x = line.origin.x + Px(14.0f);
    line.right = line.origin.x + width - Px(14.0f);
    line.draw->AddRectFilled(line.origin, ImVec2(line.origin.x + width, line.bottom), theme::kPanelAlt);
    line.draw->AddRectFilled(ImVec2(line.origin.x, line.bottom - Px(1.0f)), ImVec2(line.origin.x + width, line.bottom),
                             theme::kBorder);

    return line;
}

void InfoField(InfoLine& line, const char* label, std::string_view value)
{
    const ImVec2 caps = CapsSize(label);
    ImFont* figures = theme::GetFonts().figures;
    const ImVec2 size = TextSize(figures, theme::kTextSize, value);
    if (line.x + caps.x + Px(8.0f) + size.x > line.right)
    {
        line.x = line.right;
        return;
    }
    DrawCaps(line.draw, ImVec2(line.x, line.middle - caps.y * 0.5f), theme::kTextMuted, label);
    DrawText(line.draw, figures, theme::kTextSize, ImVec2(line.x + caps.x + Px(8.0f), line.middle - size.y * 0.5f),
             theme::kText, value);
    line.x += caps.x + Px(8.0f) + size.x + Px(22.0f);
}

std::string FitText(ImFont* font, float size, std::string_view text, float width)
{
    if (TextSize(font, size, text).x <= width)
    {
        return std::string(text);
    }

    // Drop the last character, with its continuation bytes, until the rest and the ellipsis fit. No space comes before
    // the ellipsis.
    std::string cut(text);
    while (!cut.empty())
    {
        while (!cut.empty() && (static_cast<unsigned char>(cut.back()) & 0xC0) == 0x80)
        {
            cut.pop_back();
        }
        if (!cut.empty())
        {
            cut.pop_back();
        }
        while (!cut.empty() && cut.back() == ' ')
        {
            cut.pop_back();
        }

        const std::string fitted = cut + "…";
        if (TextSize(font, size, fitted).x <= width)
        {
            return fitted;
        }
    }

    return {};
}

std::string FitCaps(std::string_view text, float width)
{
    ImFont* caps = theme::GetFonts().caps;
    const std::string capitals = Capitals(text);
    const std::size_t space = capitals.rfind(' ');
    if (CapsSize(capitals).x <= width || space == std::string::npos || space == 0 || capitals.size() - space != 2)
    {
        return FitText(caps, theme::kCapsSize, capitals, width);
    }

    // The last word tells the label from its neighbours, such as Volume 2 from Volume.
    const std::string last = capitals.substr(space);
    const float room = width - TextSize(caps, theme::kCapsSize, last).x;
    return FitText(caps, theme::kCapsSize, capitals.substr(0, space), room) + last;
}

std::optional<std::string> InfoEntry(InfoLine& line, const char* label, std::string_view value, InfoTyping& typing)
{
    // A field that doesn't fit doesn't show, and typing in it ends.
    const ImVec2 caps = CapsSize(label);
    ImFont* figures = theme::GetFonts().figures;
    const ImVec2 size = TextSize(figures, theme::kTextSize, value);
    if (line.x + caps.x + Px(8.0f) + size.x > line.right)
    {
        line.x = line.right;
        if (typing.field == label)
        {
            typing.field.clear();
        }
        return std::nullopt;
    }
    DrawCaps(line.draw, ImVec2(line.x, line.middle - caps.y * 0.5f), theme::kTextMuted, label);
    const ImVec2 at(line.x + caps.x + Px(8.0f), line.middle - size.y * 0.5f);
    line.x = at.x + size.x + Px(22.0f);
    ImGui::PushID(label);

    // Until a click, the value shows as text. A box behind it shows that it can be typed in.
    std::optional<std::string> typed;
    const ImVec2 pad(Px(4.0f), Px(3.0f));
    if (typing.field != label)
    {
        ImGui::SetCursorScreenPos(ImVec2(at.x - pad.x, at.y - pad.y));
        ImGui::InvisibleButton("##value", ImVec2(size.x + pad.x * 2.0f, size.y + pad.y * 2.0f));
        if (ImGui::IsItemHovered())
        {
            line.draw->AddRectFilled(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), theme::kRaised, Px(3.0f));
            ImGui::SetMouseCursor(ImGuiMouseCursor_TextInput);
        }
        DrawText(line.draw, figures, theme::kTextSize, at, theme::kText, value);
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
        {
            typing.field = label;
            typing.text = {};
            value.copy(typing.text.data(), typing.text.size() - 1);
            typing.focus = true;
        }
        ImGui::PopID();
        return std::nullopt;
    }

    // The text field takes the value's place. Its ID differs from the value's: a tap on a trackpad releases the button
    // in the next frame, and the release would end a field that took the value's ID. A click elsewhere ends the typing
    // before anything else takes the click. A field that has lost the focus without ending ends too.
    const float height = ImGui::GetFontSize() + pad.y * 2.0f;
    ImGui::SetCursorScreenPos(ImVec2(at.x - pad.x, line.middle - height * 0.5f));
    ImGui::SetNextItemWidth(std::max(size.x + Px(28.0f), Px(72.0f)));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, pad);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImGui::ColorConvertU32ToFloat4(theme::kDisplay));
    const bool focusing = std::exchange(typing.focus, false);
    if (focusing)
    {
        ImGui::SetKeyboardFocusHere();
    }
    PushFigures();
    ImGui::InputText("##typing", typing.text.data(), typing.text.size(), ImGuiInputTextFlags_AutoSelectAll);
    ImGui::PopFont();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    const bool active = ImGui::IsItemActive();
    const bool away = active && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsItemHovered();
    if (ImGui::IsItemDeactivated() || away)
    {
        typed = std::string(typing.text.data());
        typing.field.clear();
    }
    else if (!active && !focusing)
    {
        typing.field.clear();
    }
    ImGui::PopID();

    return typed;
}

std::optional<std::string> TextEntry(const char* id, std::string_view text, ImVec2 pos, float width, TextTyping& typing)
{
    ImGui::PushID(id);
    const ImGuiID box = ImGui::GetID("##box");
    const ImVec2 size(width, ImGui::GetFrameHeight());
    const float rounding = ImGui::GetStyle().FrameRounding;
    const ImVec2 pad = ImGui::GetStyle().FramePadding;
    const ImGuiID previous = std::exchange(typing.last, box);

    // A field that Tab or Shift+Tab ended passes the typing on, in the frame where it ended or the next. Tab passes it
    // to the next box drawn, and Shift+Tab to the box drawn before the field.
    const bool recent = ImGui::GetFrameCount() - typing.passed <= 1;
    if (typing.box == 0 && recent && (typing.onward || typing.back == box))
    {
        typing.box = box;
        typing.text = std::string(text);
        typing.focus = true;
    }
    if (typing.box != 0 || !recent)
    {
        typing.onward = false;
        typing.back = 0;
    }

    // Until a click, the text shows in a box like a text field's. A line break shows as a space.
    if (typing.box != box)
    {
        ImGui::SetCursorScreenPos(pos);
        ImGui::InvisibleButton("##box", size);
        const bool hovered = ImGui::IsItemHovered();
        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y),
                            hovered ? theme::kControlHover : theme::kControl, rounding);
        std::string line(text);
        std::replace(line.begin(), line.end(), '\n', ' ');
        ImFont* font = theme::GetFonts().text;
        const std::string shown = FitText(font, theme::kTextSize, line, width - pad.x * 2.0f);
        const ImVec2 shown_size = TextSize(font, theme::kTextSize, shown);
        DrawText(draw, font, theme::kTextSize,
                 ImVec2(pos.x + pad.x, pos.y + std::floor((size.y - shown_size.y) * 0.5f)), theme::kText, shown);
        if (hovered)
        {
            ImGui::SetMouseCursor(ImGuiMouseCursor_TextInput);
        }
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
        {
            typing.box = box;
            typing.text = std::string(text);
            typing.focus = true;
        }
        ImGui::PopID();
        return std::nullopt;
    }

    // The text field takes the box's place. Its ID differs from the box's: a tap on a trackpad releases the button in
    // the next frame, and the release would end a field that took the box's ID. A click elsewhere ends the typing
    // before anything else takes the click. A field that has lost the focus without ending ends too.
    std::optional<std::string> typed;
    ImGui::SetCursorScreenPos(pos);
    ImGui::SetNextItemWidth(width);
    const bool focusing = std::exchange(typing.focus, false);
    if (focusing)
    {
        ImGui::SetKeyboardFocusHere();
    }
    ImGui::InputText("##field", typing.text.data(), typing.text.capacity() + 1,
                     ImGuiInputTextFlags_AutoSelectAll | ImGuiInputTextFlags_CallbackResize, GrowText, &typing.text);
    // Dear ImGui's Tab moves only among text fields, and the other boxes aren't fields until a click. Tab ends the
    // field here, and passes the typing on.
    const bool active = ImGui::IsItemActive();
    const bool away = active && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsItemHovered();
    const bool tabbed = active && ImGui::IsKeyPressed(ImGuiKey_Tab, false);
    if (ImGui::IsItemDeactivated() || away || tabbed)
    {
        typed = !tabbed && ImGui::IsKeyPressed(ImGuiKey_Escape) ? std::string(text) : typing.text;
        typing.box = 0;
        if (tabbed)
        {
            typing.onward = !ImGui::GetIO().KeyShift;
            typing.back = ImGui::GetIO().KeyShift ? previous : 0;
            typing.passed = ImGui::GetFrameCount();
        }
    }
    else if (!active && !focusing)
    {
        typing.box = 0;
    }
    ImGui::PopID();

    return typed;
}

void NoObjectSelected(InfoLine& line)
{
    ImFont* text = theme::GetFonts().text;
    const ImVec2 size = TextSize(text, theme::kSmallSize, "No Object Selected");
    DrawText(line.draw, text, theme::kSmallSize, ImVec2(line.x, line.middle - size.y * 0.5f), theme::kTextMuted,
             "No Object Selected");
}

void EndInfoLine(const InfoLine& line)
{
    ImGui::SetCursorScreenPos(ImVec2(line.origin.x, line.bottom));
}

void ZoneHeader(std::string_view title, std::string_view detail)
{
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const float height = Px(32.0f);
    draw->AddRectFilled(pos, ImVec2(pos.x + width, pos.y + height), theme::kRaised);
    draw->AddRectFilled(ImVec2(pos.x, pos.y + height - Px(1.0f)), ImVec2(pos.x + width, pos.y + height),
                        theme::kBorder);

    // The title in semibold. The detail sits on the title's baseline.
    ImFont* strong = theme::GetFonts().strong;
    const ImVec2 title_size = TextSize(strong, theme::kTextSize, title);
    const float title_top = std::floor(pos.y + (height - title_size.y) * 0.5f);
    DrawText(draw, strong, theme::kTextSize, ImVec2(pos.x + Px(14.0f), title_top), theme::kText, title);
    if (!detail.empty())
    {
        ImFont* font = theme::GetFonts().text;
        const float room = width - Px(14.0f) - title_size.x - Px(12.0f) - Px(14.0f);
        const std::string shown = FitText(font, theme::kSmallSize, detail, room);
        const ImVec2 size = TextSize(font, theme::kSmallSize, shown);
        const float baseline = title_top + Ascent(strong, theme::kTextSize);
        DrawText(draw, font, theme::kSmallSize,
                 ImVec2(pos.x + width - Px(14.0f) - size.x, baseline - Ascent(font, theme::kSmallSize)),
                 theme::kTextMuted, shown);
    }

    // A dummy item takes the header's place. The zone's content then starts below it.
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
    ImGui::Dummy(ImVec2(width, height));
    ImGui::PopStyleVar();
}

void EmptyState(ImDrawList* draw, ImVec2 min, ImVec2 max, std::string_view title, std::string_view text)
{
    // In a narrow zone, the card's sides lose padding first. The text then wraps, and the title ends in an ellipsis.
    ImFont* font = theme::GetFonts().text;
    const ImVec2 padding(Px(28.0f), Px(20.0f));
    const float gap = Px(6.0f);
    const float width = max.x - min.x;
    const float room = std::max(width - Px(16.0f) - Px(24.0f), Px(40.0f));
    const std::string shown_title = FitText(font, theme::kTitleSize, title, room);
    const ImVec2 title_size = TextSize(font, theme::kTitleSize, shown_title);
    const std::vector<std::string_view> lines = WrappedLines(font, theme::kTextSize, text, room);
    const float line_height = TextSize(font, theme::kTextSize, "A").y;
    float text_width = 0.0f;
    for (const std::string_view line : lines)
    {
        text_width = std::max(text_width, TextSize(font, theme::kTextSize, line).x);
    }
    const float text_height = line_height * static_cast<float>(lines.size());
    const float top = std::floor(min.y + (max.y - min.y - title_size.y - gap - text_height) * 0.45f);

    // A card behind the text. Web pages draw their empty states that way.
    const float card_width = std::min(std::max(title_size.x, text_width) + padding.x * 2.0f, width - Px(16.0f));
    const float card_left = std::floor(min.x + (width - card_width) * 0.5f);
    const ImVec2 card_min(card_left, top - padding.y);
    const ImVec2 card_max(card_left + card_width, top + title_size.y + gap + text_height + padding.y);
    draw->PushClipRect(min, max, true);
    draw->AddRectFilled(card_min, card_max, theme::kRaised, Px(10.0f));
    draw->AddRect(card_min, card_max, theme::kBorder, Px(10.0f), Px(1.0f));
    DrawText(draw, font, theme::kTitleSize, ImVec2(min.x + (width - title_size.x) * 0.5f, top), theme::kTextMuted,
             shown_title);
    float y = top + title_size.y + gap;
    for (const std::string_view line : lines)
    {
        const float line_width = TextSize(font, theme::kTextSize, line).x;
        DrawText(draw, font, theme::kTextSize, ImVec2(min.x + (width - line_width) * 0.5f, y), theme::kTextMuted, line);
        y += line_height;
    }
    draw->PopClipRect();
}

void SettingLabel(const char* label)
{
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme::kTextMuted), "%s", label);
    ImGui::TableNextColumn();
}

ImVec2 CentreInside(ImVec2 centre, ImVec2 size, float margin)
{
    const ImGuiViewport* viewport = ImGui::GetMainViewport();

    const auto inside = [margin](float middle, float length, float start, float room)
    {
        const float low = start + margin + length * 0.5f;
        const float high = start + room - margin - length * 0.5f;
        return low > high ? start + room * 0.5f : std::clamp(middle, low, high);
    };
    return ImVec2(inside(centre.x, size.x, viewport->WorkPos.x, viewport->WorkSize.x),
                  inside(centre.y, size.y, viewport->WorkPos.y, viewport->WorkSize.y));
}

bool PrimaryButton(const char* label)
{
    // A disabled button takes the plain look, faded. The accent colour faded still reads as a button that can be
    // clicked.
    if ((ImGui::GetCurrentContext()->CurrentItemFlags & ImGuiItemFlags_Disabled) != 0)
    {
        return ImGui::Button(label);
    }

    ImGui::PushStyleColor(ImGuiCol_Button, ImGui::ColorConvertU32ToFloat4(theme::kAccent));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                          ImGui::ColorConvertU32ToFloat4(theme::Mix(theme::kAccent, theme::kText, 0.15f)));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                          ImGui::ColorConvertU32ToFloat4(theme::Mix(theme::kAccent, theme::kWindow, 0.15f)));
    ImGui::PushStyleColor(ImGuiCol_Border, ImGui::ColorConvertU32ToFloat4(theme::kAccent));
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(theme::kTextOnAccent));
    const bool clicked = ImGui::Button(label);
    ImGui::PopStyleColor(5);

    return clicked;
}

bool CheckBox(const char* label, bool* on)
{
    ImFont* font = theme::GetFonts().text;
    const ImVec2 text_size = TextSize(font, theme::kTextSize, label);
    const float box = Px(18.0f);
    const float gap = Px(8.0f);
    const float height = ImGui::GetFrameHeight();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton(label, ImVec2(box + gap + text_size.x, height));
    if (clicked)
    {
        *on = !*on;
    }

    // A box in the accent colour with a check mark when it's on, and an empty box when it's off. Inside a disabled
    // block, the style's alpha fades it.
    const bool hovered = ImGui::IsItemHovered();
    const float alpha = ImGui::GetStyle().Alpha;
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 min(pos.x, std::floor(pos.y + (height - box) * 0.5f));
    const ImVec2 max(min.x + box, min.y + box);
    if (*on)
    {
        const ImU32 fill = hovered ? theme::Mix(theme::kAccent, theme::kText, 0.15f) : theme::kAccent;
        draw->AddRectFilled(min, max, theme::WithAlpha(fill, alpha), Px(4.0f));
        const std::array<ImVec2, 3> check = {ImVec2(min.x + box * 0.26f, min.y + box * 0.53f),
                                             ImVec2(min.x + box * 0.43f, min.y + box * 0.7f),
                                             ImVec2(min.x + box * 0.75f, min.y + box * 0.33f)};
        draw->AddPolyline(check.data(), static_cast<int>(check.size()), theme::WithAlpha(theme::kTextOnAccent, alpha),
                          Px(2.0f));
    }
    else
    {
        draw->AddRectFilled(min, max, theme::WithAlpha(hovered ? theme::kControlHover : theme::kControl, alpha),
                            Px(4.0f));
        draw->AddRect(min, max, theme::WithAlpha(theme::kBorderStrong, alpha), Px(4.0f), Px(1.0f));
    }
    DrawText(draw, font, theme::kTextSize, ImVec2(max.x + gap, std::floor(pos.y + (height - text_size.y) * 0.5f)),
             theme::WithAlpha(theme::kText, alpha), label);

    return clicked;
}

void ItemTooltip(std::string_view text)
{
    if (text.empty())
    {
        return;
    }

    // Dear ImGui reads the padding as the tooltip begins. The code around it may have pushed other padding.
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, Px(theme::kWindowPadding));
    const bool shown = ImGui::BeginItemTooltip();
    ImGui::PopStyleVar();
    if (!shown)
    {
        return;
    }

    WrappedText(text);
    ImGui::EndTooltip();
}

void PushFigures()
{
    ImGui::PushFont(theme::GetFonts().figures, 0.0f);
}

void PushMenuSpacing()
{
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(Px(theme::kItemSpacing), Px(theme::kItemSpacing)));
}

void TextWrappedAtSpaces(std::string_view text)
{
    // The lines go out as one item. Their spacing is that of Dear ImGui's TextWrapped. An item for each line would put
    // the spacing between items between the lines too.
    const float width = ImGui::GetContentRegionAvail().x;
    std::string lines;
    std::size_t begin = 0;
    while (begin < text.size())
    {
        // The words from `begin` that fit on the line, and one word at least.
        std::size_t end = text.find(' ', begin);
        end = end == std::string_view::npos ? text.size() : end;
        while (end < text.size())
        {
            std::size_t next = text.find(' ', end + 1);
            next = next == std::string_view::npos ? text.size() : next;
            if (ImGui::CalcTextSize(text.data() + begin, text.data() + next).x > width)
            {
                break;
            }
            end = next;
        }
        lines.append(lines.empty() ? "" : "\n").append(text.substr(begin, end - begin));
        begin = text.find_first_not_of(' ', end);
        begin = begin == std::string_view::npos ? text.size() : begin;
    }
    if (!lines.empty())
    {
        // A wrap position that the window sets would break the lines again, after a full stop too.
        ImGui::PushTextWrapPos(-1.0f);
        ImGui::TextUnformatted(lines.data(), lines.data() + lines.size());
        ImGui::PopTextWrapPos();
    }
}

void WrappedTooltip(std::string_view text)
{
    if (text.empty())
    {
        return;
    }

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, Px(theme::kWindowPadding));
    const bool shown = ImGui::BeginTooltip();
    ImGui::PopStyleVar();
    if (!shown)
    {
        return;
    }

    WrappedText(text);
    ImGui::EndTooltip();
}

void RectTooltip(ImVec2 min, ImVec2 max, std::string_view text)
{
    // Dear ImGui shows an item's tooltip once the mouse has rested over the item for a moment.
    const ImGuiContext& context = *ImGui::GetCurrentContext();
    if (ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(min, max) && !ImGui::IsAnyMouseDown() &&
        context.MouseStationaryTimer >= ImGui::GetStyle().HoverStationaryDelay)
    {
        WrappedTooltip(text);
    }
}

std::string FittedText(const std::string& text, float width)
{
    if (ImGui::CalcTextSize(text.c_str()).x <= width)
    {
        return text;
    }

    // Each step drops a character, with its continuation bytes, and the spaces before the ellipsis.
    std::size_t end = text.size();
    while (end > 0)
    {
        do
        {
            end--;
        } while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0u) == 0x80u);
        std::size_t kept = end;
        while (kept > 0 && text[kept - 1] == ' ')
        {
            kept--;
        }
        const std::string cut = text.substr(0, kept) + "…";
        if (ImGui::CalcTextSize(cut.c_str()).x <= width)
        {
            return cut;
        }
    }

    return "…";
}

bool Matches(std::string_view item, std::string_view filter)
{
    const auto same = [](char a, char b)
    {
        return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
    };
    return std::search(item.begin(), item.end(), filter.begin(), filter.end(), same) != item.end();
}

bool FilteredCombo(const char* id, const std::vector<std::string>& items, int& chosen, const char* none,
                   std::array<char, 128>& filter, float width)
{
    const bool listed = chosen >= 0 && static_cast<std::size_t>(chosen) < items.size();
    const char* preview = listed ? items[static_cast<std::size_t>(chosen)].c_str() : none != nullptr ? none : "";
    ImGui::SetNextItemWidth(width);
    if (!ImGui::BeginCombo(id, preview, ImGuiComboFlags_HeightLargest))
    {
        return false;
    }

    // The field takes the keys as the menu opens, empty.
    if (ImGui::IsWindowAppearing())
    {
        filter.fill('\0');
        ImGui::SetKeyboardFocusHere();
    }
    // A compact row of controls may have pushed tighter padding. The field takes the theme's.
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, Px(theme::kFramePadding));
    ImGui::InputTextWithHint("##filter", "Search", filter.data(), filter.size());
    ImGui::PopStyleVar();
    std::vector<int> shown;
    if (none != nullptr && Matches(none, filter.data()))
    {
        shown.push_back(-1);
    }
    for (std::size_t i = 0; i < items.size(); i++)
    {
        if (Matches(items[i], filter.data()))
        {
            shown.push_back(static_cast<int>(i));
        }
    }
    if (shown.empty())
    {
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme::kTextMuted), "No matches");
    }

    // The items that hold the filter's text scroll under the field. A choice closes the menu, a choice of the chosen
    // item too. The items sit in a child window, and Dear ImGui closes the menu only on a click in the menu's window.
    bool changed = false;
    const float rows = ImGui::GetTextLineHeightWithSpacing() * static_cast<float>(shown.size());
    ImGui::BeginChild("##items", ImVec2(0.0f, std::min(rows, Px(260.0f))));
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(shown.size()));
    while (clipper.Step())
    {
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++)
        {
            const int i = shown[static_cast<std::size_t>(row)];
            ImGui::PushID(i);
            const char* label = i < 0 ? none : items[static_cast<std::size_t>(i)].c_str();
            if (ImGui::Selectable(label, i == chosen))
            {
                changed = i != chosen;
                chosen = i;
                ImGui::CloseCurrentPopup();
            }
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
    ImGui::EndCombo();

    return changed;
}

int SegmentedSwitch(const char* id, std::span<const std::string> labels, std::span<const uint8_t> lit, ImVec2 size,
                    std::span<const uint8_t> disabled)
{
    // Inside a disabled block, the style's alpha fades the strip, its switches and their labels.
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const ImVec2 end(pos.x + size.x, pos.y + size.y);
    const float inset = Px(2.0f);
    const float segment = (size.x - inset * 2.0f) / static_cast<float>(std::max<std::size_t>(labels.size(), 1));
    draw->AddRectFilled(pos, end, ImGui::GetColorU32(theme::kDisplay), Px(6.0f));
    draw->AddRect(pos, end, ImGui::GetColorU32(theme::kBorder), Px(6.0f), Px(1.0f));

    // Each switch, with its label in the middle.
    ImFont* figures = theme::GetFonts().figures;
    int clicked = -1;
    ImGui::PushID(id);
    for (std::size_t i = 0; i < labels.size(); i++)
    {
        const ImVec2 min(std::floor(pos.x + inset + segment * static_cast<float>(i)), pos.y + inset);
        const ImVec2 max(std::floor(pos.x + inset + segment * static_cast<float>(i + 1)), end.y - inset);
        ImGui::SetCursorScreenPos(min);
        ImGui::PushID(static_cast<int>(i));
        const bool off = i < disabled.size() && disabled[i] != 0;
        ImGui::BeginDisabled(off);
        if (ImGui::InvisibleButton("##switch", ImVec2(max.x - min.x, max.y - min.y)))
        {
            clicked = static_cast<int>(i);
        }
        const bool hovered = ImGui::IsItemHovered();
        const bool held = ImGui::IsItemActive();
        ImGui::EndDisabled();
        ImGui::PopID();

        const bool on = i < lit.size() && lit[i] != 0;
        if (on)
        {
            const float alpha = held ? 0.42f : hovered ? 0.36f : 0.3f;
            draw->AddRectFilled(min, max, ImGui::GetColorU32(theme::kAccent, alpha), Px(4.0f));
        }
        else if (hovered || held)
        {
            draw->AddRectFilled(min, max, ImGui::GetColorU32(held ? theme::kControlActive : theme::kControlHover),
                                Px(4.0f));
        }
        const ImVec2 text = TextSize(figures, theme::kTextSize, labels[i]);
        const ImVec2 at(std::floor((min.x + max.x - text.x) * 0.5f), std::floor((min.y + max.y - text.y) * 0.5f));
        const ImU32 colour = on || hovered ? theme::kText : off ? theme::kTextDim : theme::kTextMuted;
        DrawText(draw, figures, theme::kTextSize, at, ImGui::GetColorU32(colour), labels[i]);
    }
    ImGui::PopID();

    return clicked;
}

bool EscapeCloses()
{
    // At the window's start, a field that's being typed in is still the active item. It ends at the same Escape, later
    // in the frame.
    return ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
           ImGui::IsKeyPressed(ImGuiKey_Escape, false) && ImGui::GetInputTextState(ImGui::GetActiveID()) == nullptr;
}

bool MenuBarFocused()
{
    // A menu opens as a popup of the bar, and a submenu as a popup of its menu.
    for (const ImGuiWindow* window = ImGui::GetCurrentContext()->NavWindow; window != nullptr;
         window = window->ParentWindow)
    {
        if (std::string_view(window->Name) == "##MainMenuBar")
        {
            return true;
        }
    }

    return false;
}

} // namespace hokuto::app
