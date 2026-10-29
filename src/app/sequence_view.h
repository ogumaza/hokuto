// SPDX-License-Identifier: MIT

// What the zones show of a sequence of a sound archive: its tracks with their parts and notes, each track's place in
// the mixer, and the variables that a game can set. The sequence sets the mixer's values, and they change as it plays.
// A move of a control changes the sequence's commands (src/edit/mix.h).

#pragma once

#include "app/song_view.h"
#include "song/sequence.h"

namespace hokuto::app
{

// The view of `sequence`, with the mixer's values at its start, and its variables at the values that it starts with.
SongView MakeSongView(const song::Sequence& sequence);

// Sets the mixer's values in `view` to those that `sequence` has at `ticks`. A control that the user holds keeps the
// value that the user gives it.
void ShowMixerAt(SongView& view, const song::Sequence& sequence, double ticks);

// The setting of a track's mix that `control` shows.
song::Mix MixOf(MixControl control);

// The value of the setting that `control` shows, for the strip's value `shown`, inside the setting's range.
int MixValue(MixControl control, float shown);

// The strip's value for `value` of the setting that `control` shows. A pan's value runs from -1 at -64, the left end of
// the pan, to 1 at 63, the right end.
float ShownValue(MixControl control, int value);

// The level in dB of a track's volume, from 0 to 255. nn::atk squares the volume's fraction of 127. The level is
// therefore 40 log10(volume / 127): MIDI's curve for its volume controller.
float VolumeLevel(int volume);

} // namespace hokuto::app
