// SPDX-License-Identifier: MIT

// The inspector's Start rows of the selected track: what the sound's start gives the track alone (format::SoundStart).
// Mute at Start, then Volume and a row for each track variable that the start sets on the track. Each row has a box
// that sets or clears its value. Volume without a value shows none. A variable's row shows only while the start sets
// the variable, and its box clears it.

#pragma once

#include <imgui.h>

#include "app/arrangement.h"
#include "app/song_view.h"

namespace hokuto::app
{

// Draws the Start rows of `track`, the track in row `row`, from `pos`, `width` wide. A change sets `state.start_mute`
// or `state.track_start`. Returns the bottom of the last row.
float DrawTrackStart(ImDrawList* draw, ImVec2 pos, float width, const TrackView& track, int row,
                     ArrangementState& state);

} // namespace hokuto::app
