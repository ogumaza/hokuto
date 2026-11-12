// SPDX-License-Identifier: MIT

// What the zones show of a sequence of a sound archive: its tracks with their parts and notes, each track's place in
// the mixer, and the variables that a game can set. The sequence sets the mixer's values, and they change as it plays.
// A move of a control changes the sequence's commands (src/edit/mix.h).

#pragma once

#include <cstdint>
#include <map>
#include <string>

#include "app/song_view.h"
#include "song/sequence.h"

namespace hokuto::app
{

// The view of `sequence`, with the mixer's values at its start, and its variables at the values that it starts with.
// `labels` names the subroutines of the calls' parts, by their offsets (PartLabels).
SongView MakeSongView(const song::Sequence& sequence, const std::map<uint32_t, std::string>& labels);

// Sets the mixer's values in `view` to those that `sequence` has at `ticks`. A control that the user holds keeps the
// value that the user gives it.
void ShowMixerAt(SongView& view, const song::Sequence& sequence, double ticks);

// The setting of a track's mix that `control` shows.
song::Mix MixOf(MixControl control);

// The value of the setting that `control` shows, for the strip's value `shown`, inside the setting's range. `gba` is
// true for a song of the GBA's drivers. `most` is the volume at 0 dB: a track of Konami's driver can hold volumes up to
// 15.
int MixValue(MixControl control, float shown, bool gba = false, int most = 127);

// The strip's value for `value` of the setting that `control` shows. A pan's value runs from -1 at -64, the left end of
// the pan, to 1 at 63, the right end. `gba` is true for a song of the GBA's drivers, and `most` the volume at 0 dB.
float ShownValue(MixControl control, int value, bool gba = false, int most = 127);

// The level in dB of a track's volume, from 0 to 255. nn::atk squares the volume's fraction of 127. The level is
// therefore 40 log10(volume / 127): MIDI's curve for its volume controller. With `gba`, the volume runs from 0 to 127,
// and MP2K scales the level by its fraction of 127: 20 log10(volume / 127). Konami's driver scales it by its fraction
// of `most`, the track's largest volume, in the same way.
float VolumeLevel(int volume, bool gba = false, int most = 127);

} // namespace hokuto::app
