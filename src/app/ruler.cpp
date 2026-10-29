// SPDX-License-Identifier: MIT

#include "app/ruler.h"

#include <algorithm>
#include <cmath>

namespace hokuto::app
{
namespace
{

float XOf(const RulerGeometry& geometry, double quarters)
{
    return geometry.origin + static_cast<float>(quarters) * geometry.beat_width;
}

double QuartersAt(const RulerGeometry& geometry, float x)
{
    return (x - geometry.origin) / geometry.beat_width;
}

// The point for a locator at `x`: the start or the end of the song's loop when one is near, or else the nearest beat.
double Snap(const RulerGeometry& geometry, float x)
{
    for (const double point : {geometry.loop_start, geometry.loop_end})
    {
        if (point >= 0.0 && std::abs(XOf(geometry, point) - x) <= geometry.snap)
        {
            return point;
        }
    }

    return std::max(std::round(QuartersAt(geometry, x)), 0.0);
}

} // namespace

bool Ruler::Update(const RulerGeometry& geometry, const RulerMouse& mouse, Transport& transport)
{
    if (mouse.pressed)
    {
        Press(geometry, mouse, transport);
    }
    if (drag_ != Drag::kNone)
    {
        Move(geometry, mouse, transport);
    }

    // The mouse shows a locator's grab while it drags one, and while it's over one.
    const bool dragging = drag_ == Drag::kLeftLocator || drag_ == Drag::kRightLocator;
    return dragging || (mouse.over && drag_ == Drag::kNone && LocatorAt(geometry, mouse, transport) != Drag::kNone);
}

bool Ruler::MovingPosition() const
{
    return drag_ == Drag::kPosition;
}

Ruler::Drag Ruler::LocatorAt(const RulerGeometry& geometry, const RulerMouse& mouse, const Transport& transport)
{
    const float to_left = std::abs(mouse.x - XOf(geometry, transport.CycleStart()));
    const float to_right = std::abs(mouse.x - XOf(geometry, transport.CycleEnd()));
    if (mouse.y >= geometry.middle || std::min(to_left, to_right) > geometry.grab)
    {
        return Drag::kNone;
    }

    return to_left < to_right ? Drag::kLeftLocator : Drag::kRightLocator;
}

void Ruler::Press(const RulerGeometry& geometry, const RulerMouse& mouse, Transport& transport)
{
    // A click with Command or Option sets a locator, and starts no drag.
    if (mouse.command || mouse.option)
    {
        const double at = Snap(geometry, mouse.x);
        transport.SetLocators(mouse.command ? at : transport.CycleStart(), mouse.command ? transport.CycleEnd() : at);
        return;
    }

    drag_ = LocatorAt(geometry, mouse, transport);
    if (drag_ != Drag::kNone)
    {
        return;
    }

    drag_ = mouse.y < geometry.middle ? Drag::kRange : Drag::kPosition;
    range_from_ = Snap(geometry, mouse.x);
    press_x_ = mouse.x;
    press_on_range_ = mouse.x > XOf(geometry, transport.CycleStart()) && mouse.x < XOf(geometry, transport.CycleEnd());
    drawing_ = false;
}

void Ruler::Move(const RulerGeometry& geometry, const RulerMouse& mouse, Transport& transport)
{
    // The release ends the drag. A press on the range that didn't draw turns the cycle on or off.
    if (!mouse.down)
    {
        if (drag_ == Drag::kRange && !drawing_ && press_on_range_)
        {
            transport.SetCycle(!transport.Cycle());
        }
        drag_ = Drag::kNone;
        return;
    }

    if (drag_ == Drag::kPosition)
    {
        transport.SetPosition(QuartersAt(geometry, mouse.x));
    }
    else if (drag_ == Drag::kRange)
    {
        drawing_ = drawing_ || std::abs(mouse.x - press_x_) > geometry.drag;
        if (drawing_)
        {
            transport.SetLocators(range_from_, Snap(geometry, mouse.x));
            transport.SetCycle(true);
        }
    }
    else
    {
        // A locator dragged past the other one takes the other one's place.
        const bool left = drag_ == Drag::kLeftLocator;
        const double at = Snap(geometry, mouse.x);
        const double other = left ? transport.CycleEnd() : transport.CycleStart();
        transport.SetLocators(at, other);
        if (left ? at > other : at < other)
        {
            drag_ = left ? Drag::kRightLocator : Drag::kLeftLocator;
        }
    }
}

} // namespace hokuto::app
