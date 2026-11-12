// SPDX-License-Identifier: MIT

// The icons. Their strokes and shapes lie on the display's pixels. They stay sharp at every scale.

#include "app/icons.h"

#include <imgui_internal.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <initializer_list>

#include "app/icons_parts.h"
#include "app/theme.h"

namespace hokuto::app
{
namespace
{

// The point of an icon of `size` around `centre` at (`x`, `y`) times its size, on the display's pixels. An icon's edges
// then lie on pixels.
ImVec2 At(ImVec2 centre, float size, float x, float y)
{
    return theme::OnPixels(ImVec2(centre.x + size * x, centre.y + size * y));
}

// The display's pixels to a unit of Dear ImGui's coordinates, one at least.
float Density()
{
    return std::max(ImGui::GetIO().DisplayFramebufferScale.x, 1.0f);
}

// The whole pixels of the display that a stroke of `width` takes, one at least. A part of a pixel rounds up only from
// three quarters: a small icon's strokes stay thin on a display with one pixel to the point.
float StrokePixels(float width)
{
    const float density = Density();
    return std::max(std::floor(width * density + 0.25f), 1.0f);
}

// A stroke's width in whole pixels of the display.
float StrokeWidth(float width)
{
    return StrokePixels(width) / Density();
}

// The middle of a stroke near `place`, for a stroke whose `width` is whole pixels of the display. A stroke an odd
// number of pixels wide has its middle in the middle of a pixel. An even one has its middle on the edge between two. A
// level or upright stroke there has its edges on pixels.
float StrokeMiddle(float place, float width)
{
    const float density = Density();
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

} // namespace

namespace icon_parts
{

float CurveWidth(float width)
{
    const float density = Density();
    return std::max(std::round(width * density), 1.0f) / density;
}

float RingRadius(float radius, float width)
{
    const float density = Density();
    const float half = width * density * 0.5f;
    return (std::max(std::round(radius * density - half), 0.0f) + half) / density;
}

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

} // namespace icon_parts

namespace
{

using icon_parts::CurveWidth;
using icon_parts::IconLine;
using icon_parts::RingRadius;

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
            const float pixel = 1.0f / Density();
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

} // namespace hokuto::app
