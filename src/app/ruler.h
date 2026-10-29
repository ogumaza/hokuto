// SPDX-License-Identifier: MIT

// The arrangement's ruler. A press in its lower half moves the position, and a drag moves it on. In the upper half, a
// press on a locator moves it. A drag elsewhere in the upper half draws a new range between the locators and turns the
// cycle on. A click on the range turns the cycle on or off. Command-click sets the left locator and Option-click the
// right one, anywhere in the ruler. On Windows, Ctrl and Alt take their places. A locator snaps to the nearest beat, or
// to the start or the end of the song's loop when the mouse is near one.

#pragma once

#include "app/transport.h"

namespace hokuto::app
{

// The ruler's distances before the display's scale: how near a locator a press grabs it, how near the song's loop a
// locator snaps to it, and how far the mouse moves before a press in the upper half draws a range.
inline constexpr float kLocatorGrab = 5.0f;
inline constexpr float kLocatorSnap = 8.0f;
inline constexpr float kRangeDragDistance = 3.0f;

// Where the ruler is on the screen, in the display's pixels.
struct RulerGeometry
{
    float origin = 0.0f;     // the x of the song's start, after the scroll
    float beat_width = 1.0f; // the pixels of a quarter note
    float middle = 0.0f;     // the y between the ruler's halves
    float grab = kLocatorGrab;
    float snap = kLocatorSnap;
    float drag = kRangeDragDistance;
    double loop_start = -1.0; // in quarter notes, or -1 for a song without a loop
    double loop_end = -1.0;
};

// The mouse in a frame.
struct RulerMouse
{
    float x = 0.0f;
    float y = 0.0f;
    bool over = false;    // over the ruler
    bool pressed = false; // the left button went down over the ruler in this frame
    bool down = false;    // the left button is down
    bool command = false; // Command on the Mac, Ctrl on Windows
    bool option = false;  // Option on the Mac, Alt on Windows
};

class Ruler
{
public:
    // Follows the mouse for a frame, and changes the transport. Returns whether the mouse should show that it moves a
    // locator.
    bool Update(const RulerGeometry& geometry, const RulerMouse& mouse, Transport& transport);

    // Whether a drag moves the position. Playback starts again from the new position once the drag ends.
    bool MovingPosition() const;

private:
    // What the drag in progress does.
    enum class Drag
    {
        kNone,
        kPosition,
        kRange,
        kLeftLocator,
        kRightLocator,
    };

    // The locator that a press at the mouse would grab, if any.
    static Drag LocatorAt(const RulerGeometry& geometry, const RulerMouse& mouse, const Transport& transport);

    void Press(const RulerGeometry& geometry, const RulerMouse& mouse, Transport& transport);
    void Move(const RulerGeometry& geometry, const RulerMouse& mouse, Transport& transport);

    // The drag, and for a range: its first end, in quarter notes, where the press was, whether the press was on the
    // range, and whether the mouse has moved far enough to draw.
    Drag drag_ = Drag::kNone;
    double range_from_ = 0.0;
    float press_x_ = 0.0f;
    bool press_on_range_ = false;
    bool drawing_ = false;
};

} // namespace hokuto::app
