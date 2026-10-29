// SPDX-License-Identifier: MIT

// The properties of a wave or a stream: a window that doesn't edit the samples. It shows the waveform with the loop,
// and edits the loop, the rate and the encoding (edit/waves.h and edit/streams.h). A drag of a loop's marker moves it,
// and the snap to zero crossings puts it where the channels lie nearest 0. A loop in DSP-ADPCM starts at a frame of 14
// samples, and a stream's loop at a block. Only there can the consoles' files keep the encoded samples as they are. The
// rate changes the pitch, and the samples stay. Play plays the wave round its loop until Stop. A stream's properties
// add its tracks and its regions. Replace… and Export… open the windows that the browser's menu opens, for WAV files
// among others. The window says what else an edit changes.

#pragma once

#include <optional>
#include <string>

#include "app/item_views.h"
#include "app/widgets.h"

namespace hokuto::app
{

// What the user did in the window: new values, with the action's name for the Undo History, or a click on Play, Stop,
// Replace… or Export…. `refused` holds a message for a typed value that the item can't take, such as a rate of 0.
struct WaveEdit
{
    std::optional<WaveValues> values;
    std::string name;
    std::string refused;
    bool play = false;
    bool stop = false;
    bool replace = false;
    bool export_item = false;
};

// The marker of the loop that a drag moves: its start, or its end. Without a loop, the end is the wave's end.
enum class LoopMarker
{
    kStart,
    kEnd,
};

// The window's state between frames: the first sample in view and the samples across the view, or 0 for the whole item,
// whether the markers snap to zero crossings, the marker that the user drags with the values during the drag, the
// typing of a value, and whether the item plays. While `top` is set, the facts scroll to their top, and the window
// clears it. A new state sets it: the facts keep their name from one item to the next, and Dear ImGui keeps their
// scroll with the name. `loop_start_off` keeps the start of a loop that the switch turned off, for the switch to put
// back.
struct WaveWindowState
{
    bool top = true;
    std::optional<uint32_t> loop_start_off;
    double first = 0.0;
    double across = 0.0;
    bool snap = true;
    std::optional<LoopMarker> dragging;
    std::optional<WaveValues> held;
    TextTyping typing;
    bool playing = false;
};

// Draws the window for `view`. The close button and Escape clear `*open`. While `busy`, the window takes no edits.
WaveEdit DrawWaveWindow(const WaveView& view, WaveWindowState& state, bool busy, bool* open);

} // namespace hokuto::app
