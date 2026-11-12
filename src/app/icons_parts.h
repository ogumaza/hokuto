// SPDX-License-Identifier: MIT

// The strokes of the icons that the other controls draw too (icons.cpp and faders.cpp). The names are internal to
// those files.

#pragma once

#include <imgui.h>

namespace hokuto::app::icon_parts
{

// A curve's width rounded to the nearest whole pixels of the display, one at least. A curve one pixel wide breaks up
// where it slants. A curve therefore rounds up sooner than a level or upright stroke.
float CurveWidth(float width);

// The radius nearest `radius` for a ring whose `width` is whole pixels of the display, around a centre on the corner of
// a pixel. The ring's edges then lie on pixels at its top, bottom, left and right.
float RingRadius(float radius, float width);

// Draws a stroke of an icon from `a` to `b`. A level or upright stroke lies on pixels. A slanted stroke is smoothed
// whatever its place, and keeps its width.
void IconLine(ImDrawList* draw, ImVec2 a, ImVec2 b, ImU32 colour, float width);

} // namespace hokuto::app::icon_parts
