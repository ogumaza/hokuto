// SPDX-License-Identifier: MIT

// How playback and the exports apply a sound's start (format::SoundStart): the variables that it sets become the
// sound's starting values and changes, and the rest becomes the performer's options or the calls that a game makes
// through the sound's handle.

#pragma once

#include <cstdint>

#include "ntdlib/sequence_export/performance.h"
#include "ntdlib/sequence_export/settings.h"

#include "format/starts.h"
#include "song/sequence.h"

namespace hokuto::song
{

// The values that `start` gives variables before the sound's first tick: the player's and the global variables, and
// the track variables of every track as 32 to 47. A variable of a single track isn't among them.
StartValues StartValuesOf(const format::SoundStart& start);

// The variables that `start` changes after the sound's start, in the order of their times.
Changes ChangesOf(const format::SoundStart& start);

// `start` with the variables of `values` and `changes` in place of its variables other than those of a single track.
// The variables of a single track stay.
format::SoundStart WithVariables(format::SoundStart start, const StartValues& values, const Changes& changes);

// The tracks of a sequence that `start` quiets, bit N for track N.
uint16_t QuietedSequenceTracks(const format::SoundStart& start);

// Sets the options of ntdlib's performer to what `start` sets besides the variables of StartValuesOf and ChangesOf: the
// tracks that play, the tracks' volumes, the tempo ratio, the pitch, the volume, the sends, the banks, the random
// generator's state, the low-pass and the variables of single tracks. A start's state of the generator holds at the
// first tick, with no steps before it. The variables of every track take their values before those of a single track.
void ApplyStart(ntdlib::sequence_export::PerformOptions& options, ntdlib::sequence_export::Settings& settings,
                const format::SoundStart& start);

// Gives `player`, a sequence player of nw::snd or nn::atk, what a game sets through the sound's handle as `start` gives
// it, after the sound's start and before its first tick: the tracks' volumes, the tempo ratio, the pitch, the low-pass
// and the variables of single tracks. The caller quiets the tracks, sets the volume, the sends and the generator's
// state, and sets the variables of every track before these.
template <class Player>
void ApplyHandleStart(Player& player, const format::SoundStart& start)
{
    const uint32_t quieted = format::QuietedTracks(start);
    for (const auto& [track, volume] : start.track_volumes)
    {
        if ((quieted >> track & 1) == 0)
        {
            player.SetTrackVolume(1u << track, volume);
        }
    }

    if (start.tempo_ratio)
    {
        player.SetTempoRatio(*start.tempo_ratio);
    }
    if (start.pitch)
    {
        player.SetPitch(*start.pitch);
    }
    if (start.low_pass)
    {
        player.SetLpfFreq(*start.low_pass);
    }

    for (const format::StartVariable& variable : start.variables)
    {
        if (variable.track && variable.time == 0)
        {
            player.SetTrackVariable(*variable.track, variable.variable - format::kFirstTrackVariable, variable.value);
        }
    }
}

} // namespace hokuto::song
