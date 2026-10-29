// SPDX-License-Identifier: MIT

// Tests the ruler's gestures (src/app/ruler.h) on a transport, without a window. The lower half of the ruler moves the
// position. The upper half draws the cycle's range, moves its locators, and turns the cycle on or off. Command and
// Option set the locators. Locators snap to beats and to the song's loop.

#include "app/ruler.h"

#include <cstdint>

#include "xsflib/testing/check.h"

#include "app/transport.h"

using hokuto::app::Ruler;
using hokuto::app::RulerGeometry;
using hokuto::app::RulerMouse;
using hokuto::app::Transport;

namespace
{

// A point in the upper half of the test's ruler, and one in the lower half.
constexpr float kUpper = 7.0f;
constexpr float kLower = 22.0f;

// The test's ruler: 30 pixels tall, with the song's start at x = 100 and 20 pixels a beat.
RulerGeometry Geometry()
{
    RulerGeometry geometry;
    geometry.origin = 100.0f;
    geometry.beat_width = 20.0f;
    geometry.middle = 15.0f;

    return geometry;
}

// The x of quarter note `quarters`.
float XAt(double quarters)
{
    return 100.0f + static_cast<float>(quarters) * 20.0f;
}

// The mouse over the ruler at (`x`, `y`).
RulerMouse MouseAt(float x, float y, bool pressed, bool down)
{
    RulerMouse mouse;
    mouse.x = x;
    mouse.y = y;
    mouse.over = true;
    mouse.pressed = pressed;
    mouse.down = down;

    return mouse;
}

// A press at `from`, a drag through the point halfway to `to`, and a release at `to`.
void Drag(Ruler& ruler, Transport& transport, float y, float from, float to)
{
    const RulerGeometry geometry = Geometry();
    ruler.Update(geometry, MouseAt(from, y, true, true), transport);
    ruler.Update(geometry, MouseAt((from + to) / 2.0f, y, false, true), transport);
    ruler.Update(geometry, MouseAt(to, y, false, true), transport);
    ruler.Update(geometry, MouseAt(to, y, false, false), transport);
}

// A press and a release at `x`, with Command or Option held.
void Click(Ruler& ruler, Transport& transport, float x, float y, bool command = false, bool option = false)
{
    const RulerGeometry geometry = Geometry();
    RulerMouse press = MouseAt(x, y, true, true);
    press.command = command;
    press.option = option;
    ruler.Update(geometry, press, transport);
    ruler.Update(geometry, MouseAt(x, y, false, false), transport);
}

// A press in the lower half moves the position, and a drag moves it on, without snapping. The release ends the drag.
// The locators and the cycle stay.
void TestMovePosition()
{
    const RulerGeometry geometry = Geometry();
    Ruler ruler;
    Transport transport;
    transport.SetLocators(8.0, 16.0);

    ruler.Update(geometry, MouseAt(XAt(3.5), kLower, true, true), transport);
    const double pressed_at = transport.Position();
    const bool moving = ruler.MovingPosition();
    ruler.Update(geometry, MouseAt(XAt(6.25), kLower, false, true), transport);
    const double dragged_to = transport.Position();
    ruler.Update(geometry, MouseAt(XAt(6.25), kLower, false, false), transport);

    SEQLIB_CHECK(pressed_at == 3.5 && dragged_to == 6.25);
    SEQLIB_CHECK(moving && !ruler.MovingPosition());
    SEQLIB_CHECK(transport.CycleStart() == 8.0 && transport.CycleEnd() == 16.0 && !transport.Cycle());
}

// A drag in the upper half draws a range from beat to beat, in either direction, and turns the cycle on. The position
// stays. A drag shorter than the distance draws nothing.
void TestDrawRange()
{
    Ruler ruler;
    Transport transport;
    transport.SetLocators(8.0, 16.0);
    const uint64_t moves = transport.Moves();

    Drag(ruler, transport, kUpper, XAt(20.0), XAt(20.0) + 2.0f);
    const bool drew_short = transport.CycleStart() != 8.0 || transport.CycleEnd() != 16.0 || transport.Cycle();
    Drag(ruler, transport, kUpper, XAt(10.2), XAt(4.6));

    SEQLIB_CHECK(!drew_short);
    SEQLIB_CHECK(transport.CycleStart() == 5.0 && transport.CycleEnd() == 10.0 && transport.Cycle());
    SEQLIB_CHECK(transport.Moves() == moves && !ruler.MovingPosition());
}

// A click on the range turns the cycle on, and another turns it off. A slip of the mouse shorter than the distance
// still counts as a click. A click in the upper half outside the range changes nothing.
void TestClickRange()
{
    Ruler ruler;
    Transport transport;
    transport.SetLocators(8.0, 16.0);

    Click(ruler, transport, XAt(12.0), kUpper);
    const bool on = transport.Cycle();
    Drag(ruler, transport, kUpper, XAt(12.0), XAt(12.0) + 2.0f);
    const bool off = !transport.Cycle();
    Click(ruler, transport, XAt(20.0), kUpper);

    SEQLIB_CHECK(on && off);
    SEQLIB_CHECK(!transport.Cycle() && transport.CycleStart() == 8.0 && transport.CycleEnd() == 16.0);
}

// A press near a locator in the upper half grabs it, and a drag moves it to the nearest beat. A locator dragged past
// the other one takes the other one's place, and goes on following the mouse. The cycle stays off.
void TestMoveLocator()
{
    Ruler ruler;
    Transport transport;
    transport.SetLocators(8.0, 16.0);

    Drag(ruler, transport, kUpper, XAt(8.0) + 3.0f, XAt(5.3));
    const bool left_moved = transport.CycleStart() == 5.0 && transport.CycleEnd() == 16.0;
    const RulerGeometry geometry = Geometry();
    ruler.Update(geometry, MouseAt(XAt(16.0) - 2.0f, kUpper, true, true), transport);
    ruler.Update(geometry, MouseAt(XAt(3.2), kUpper, false, true), transport);
    const bool crossed = transport.CycleStart() == 3.0 && transport.CycleEnd() == 5.0;
    ruler.Update(geometry, MouseAt(XAt(4.4), kUpper, false, true), transport);
    ruler.Update(geometry, MouseAt(XAt(4.4), kUpper, false, false), transport);

    SEQLIB_CHECK(left_moved && crossed);
    SEQLIB_CHECK(transport.CycleStart() == 4.0 && transport.CycleEnd() == 5.0);
    SEQLIB_CHECK(!transport.Cycle());
}

// Command-click sets the left locator and Option-click the right one, in either half, to the nearest beat. The locators
// stay in order. Neither click moves the position.
void TestModifierClicks()
{
    Ruler ruler;
    Transport transport;
    transport.SetLocators(8.0, 16.0);
    const uint64_t moves = transport.Moves();

    Click(ruler, transport, XAt(11.6), kLower, true, false);
    const bool left_set = transport.CycleStart() == 12.0 && transport.CycleEnd() == 16.0;
    Click(ruler, transport, XAt(4.4), kUpper, false, true);

    SEQLIB_CHECK(left_set);
    SEQLIB_CHECK(transport.CycleStart() == 4.0 && transport.CycleEnd() == 12.0);
    SEQLIB_CHECK(transport.Moves() == moves && !ruler.MovingPosition());
}

// Near the song's loop, a locator snaps to the loop's start or end. Neither has to be on a beat. Further away, a
// locator snaps to the nearest beat.
void TestSnapToLoop()
{
    RulerGeometry geometry = Geometry();
    geometry.loop_start = 3.3;
    geometry.loop_end = 9.7;
    Ruler ruler;
    Transport transport;
    const auto draw = [&](float from, float to)
    {
        ruler.Update(geometry, MouseAt(from, kUpper, true, true), transport);
        ruler.Update(geometry, MouseAt(to, kUpper, false, true), transport);
        ruler.Update(geometry, MouseAt(to, kUpper, false, false), transport);
    };

    draw(XAt(3.3) + 6.0f, XAt(9.7) - 7.0f);
    const bool on_loop = transport.CycleStart() == 3.3 && transport.CycleEnd() == 9.7;
    draw(XAt(3.3) + 10.0f, XAt(9.7) + 9.0f);

    SEQLIB_CHECK(on_loop);
    SEQLIB_CHECK(transport.CycleStart() == 4.0 && transport.CycleEnd() == 10.0);
}

// The mouse shows a locator's grab over a locator in the upper half, and while it drags one. It doesn't in the lower
// half, away from the locators, or outside the ruler.
void TestGrabCursor()
{
    const RulerGeometry geometry = Geometry();
    Ruler ruler;
    Transport transport;
    transport.SetLocators(8.0, 16.0);
    RulerMouse outside = MouseAt(XAt(8.0), kUpper, false, false);
    outside.over = false;

    const bool on_left = ruler.Update(geometry, MouseAt(XAt(8.0) + 4.0f, kUpper, false, false), transport);
    const bool on_right = ruler.Update(geometry, MouseAt(XAt(16.0) - 4.0f, kUpper, false, false), transport);
    const bool lower = ruler.Update(geometry, MouseAt(XAt(8.0), kLower, false, false), transport);
    const bool away = ruler.Update(geometry, MouseAt(XAt(12.0), kUpper, false, false), transport);
    const bool off_ruler = ruler.Update(geometry, outside, transport);
    ruler.Update(geometry, MouseAt(XAt(16.0), kUpper, true, true), transport);
    const bool dragging = ruler.Update(geometry, MouseAt(XAt(30.0), kLower, false, true), transport);

    SEQLIB_CHECK(on_left && on_right);
    SEQLIB_CHECK(!lower && !away && !off_ruler);
    SEQLIB_CHECK(dragging);
}

} // namespace

int main()
{
    TestMovePosition();
    TestDrawRange();
    TestClickRange();
    TestMoveLocator();
    TestModifierClicks();
    TestSnapToLoop();
    TestGrabCursor();

    return xsflib::testing::Summary("ruler");
}
