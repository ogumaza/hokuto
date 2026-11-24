// SPDX-License-Identifier: MIT

// The calls through a sound's handle that give a sequence player of ntdlib's models of the 3DS, the Wii U and the
// Switch a sound's start (format::SoundStart), after the sound's start and before its first tick. Playback
// (console_ctr.cpp and console_switch_wiiu.cpp) and the measure of lengths (lengths.cpp) make the same calls. The
// caller sets the variables of every track before these calls, and the 3DS's caller applies the start's volume to the
// output (ntdlib's ApplySequenceStart).

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "ntdlib/cafe/snd/voice.h"
#include "ntdlib/ctr/snd/sequence.h"
#include "ntdlib/ctr/snd/voice.h"
#include "ntdlib/nwsnd/util.h"
#include "ntdlib/nx/atk/voice.h"

#include "format/starts.h"
#include "song/starts.h"

namespace hokuto::song::start_handle
{

// `sample` of the 3DS's output at the start's `volume`: rounded to the nearest integer, halves away from zero, and
// within 16 bits. The 3DS's model plays the start's volume this way, and the measure hears it the same way.
inline int16_t AtVolume(int16_t sample, float volume)
{
    const float scaled = std::round(static_cast<float>(sample) * volume);
    return static_cast<int16_t>(std::clamp(scaled, -32768.0f, 32767.0f));
}

// The sends and the volume of `start` on the player's outputs of the 3DS: the sends on buses A and B. The 3DS's
// players take no volume there.
inline void SetOutputs(ntdlib::ctr::snd::Outputs& outputs, const format::SoundStart& start)
{
    if (start.sends)
    {
        outputs.fx_send = {(*start.sends)[0], (*start.sends)[1]};
    }
}

// The same on the outputs of the Wii U: the TV's and each GamePad's.
inline void SetOutputs(ntdlib::cafe::snd::Outputs& outputs, const format::SoundStart& start)
{
    const auto set = [&start](ntdlib::cafe::snd::OutputParam& param)
    {
        param.volume = start.volume.value_or(param.volume);
        if (start.sends)
        {
            param.fx_send = *start.sends;
        }
    };
    set(outputs.tv);
    for (ntdlib::cafe::snd::OutputParam& drc : outputs.drc)
    {
        set(drc);
    }
}

// The same on the outputs of the Switch.
inline void SetOutputs(ntdlib::nx::atk::OutputParam& outputs, const format::SoundStart& start)
{
    outputs.volume = start.volume.value_or(outputs.volume);
    if (start.sends)
    {
        outputs.fx_send = *start.sends;
    }
}

// Gives the 3DS's `player` of `engine` the start. A muted track starts none of its notes, as with SEQ_MUTE_NO_STOP
// (SeqTrack::SetMute's mode 1), and a silenced track starts them at a volume of 0 (ntdlib's ApplyMutesAndSeed).
template <class Engine, class Player>
void ApplyCtr(Engine& engine, Player& player, const format::SoundStart& start)
{
    ntdlib::ctr::snd::ApplyMutesAndSeed(engine, player, static_cast<uint16_t>(start.muted & 0xFFFF),
                                        static_cast<uint16_t>(start.silenced & 0xFFFF), start.seed);
    SetOutputs(player.PlayerOutputs(), start);
    ApplyHandleStart(player, start);
}

// Sets variable `number` of the Wii U's or the Switch's `player` of `engine` to `value`, as a game does through the
// sound's handle. nw::snd and nn::atk start the player a frame after the sound, and allocate its tracks then. Before
// that, a track variable of every track (32 to 47) goes to each of the 16 tracks (SetTrackVariable), and the tracks
// take the values in order as they open. ntdlib's SetVariable writes only the tracks that the player has allocated.
template <class Engine, class Player>
void SetNwVariable(Engine& engine, Player& player, int number, int16_t value)
{
    constexpr int kTracks = 16;
    const bool waiting = !player.IsActive() && !player.IsFinished();
    if (waiting && number >= format::kFirstTrackVariable && number < format::kStartVariables)
    {
        for (int track = 0; track < kTracks; ++track)
        {
            player.SetTrackVariable(track, number - format::kFirstTrackVariable, value);
        }

        return;
    }

    player.SetVariable(engine, number, value);
}

// Gives the Wii U's or the Switch's `player` of `engine` the start. nw::snd and nn::atk start the player a frame after
// the sound, and open its tracks then. A game sets the volume of the quieted tracks to 0 through the sound's handle
// before that, and the tracks take it as they open. A muted track plays the same way: its notes take their voices, at a
// volume of 0.
template <class Engine, class Player>
void ApplyNw(Engine& engine, Player& player, const format::SoundStart& start)
{
    if (const uint16_t quieted = QuietedSequenceTracks(start); quieted != 0)
    {
        player.SetTrackVolume(quieted, 0.0f);
    }
    if (start.seed)
    {
        engine.Rng() = ntdlib::nwsnd::Random(*start.seed);
    }

    SetOutputs(player.PlayerOutputs(), start);
    ApplyHandleStart(player, start);
}

} // namespace hokuto::song::start_handle
