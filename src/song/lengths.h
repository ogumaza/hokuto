// SPDX-License-Identifier: MIT

// The lengths of a sequence sound for players: how long a player plays the sound before its fade, and how long the fade
// lasts. ntdlib's length analysis (ntdlib/nwsnd/length_model.h) plays the sound on ntdlib's model of its console, frame
// by frame, until the sound has played its loop twice, ends, or stays silent for 180 s, for 600 s at most. A sound that
// loops then fades over 10 s. A sound that ends gets 0.5 s after its last sound, and no fade.

#pragma once

#include <cstdint>
#include <optional>

#include "ntdlib/rsar/formats.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sound_build.h"

#include "song/sequence.h"
#include "song/sequence_model.h"

namespace hokuto::song
{

// How many times a sound plays its loop before its fade. xsflib's rip::kLengthLoops is the same.
inline constexpr int kLengthLoops = 2;

// The lengths of a sound, in milliseconds. A sound that loops plays its loop `loops` times before the fade, and `loop`
// gives the loop that the arrangement shows, if it shows one (ReadSequence). A sound that plays on past 600 s fades
// there, with no whole loops. A sound that ends has neither loops nor a fade. A silent sound never makes a sound.
struct Length
{
    friend bool operator==(const Length&, const Length&) = default;

    uint32_t length = 0;
    uint32_t fade = 0;
    int loops = 0;
    bool silent = false;
    std::optional<Loop> loop;
};

// Measures sequence sound `sound` of `archive`, with `effects` on the aux buses at `volumes`. The 3DS plays without
// effects. The sound's variables start at -1. Returns nothing for a sound that isn't a sequence sound or doesn't start,
// and when the model can't play it, such as with a damaged bank. Running out of memory throws std::bad_alloc.
std::optional<Length> MeasureLength(const ntdlib::sar::SoundArchive& archive, uint32_t sound,
                                    const AuxEffects& effects = {}, const AuxVolumes& volumes = kFullVolumes);

// The same for sequence sound `sound` of the Wii's `archive`, on the model of `build` of NW4R SND. The archive has to
// be read by ntdlib::rvl::snd::kRsarOptions. The sound's global variables start at 0. The game starts them there too.
// The model mixes as AX's microcode of December 2006 does, or as that of October 2006 does for the build that comes
// with it. So does the program.
std::optional<Length> MeasureLength(const ntdlib::rsar::SoundArchive& archive, const ntdlib::SndBuild& build,
                                    uint32_t sound, const AuxEffects& effects = {},
                                    const AuxVolumes& volumes = kFullVolumes);

} // namespace hokuto::song
