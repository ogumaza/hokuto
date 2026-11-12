// SPDX-License-Identifier: MIT

// The output's channel and the FX channels of the mixer. DrawMixer in views.h draws the zone.

#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "app/widgets.h"

namespace hokuto::app
{

// The output's channel in the mixer. The fader sets the output's level.
struct MasterBus
{
    float volume = 0.0f;                               // dB
    std::array<float, 2> meter = {kSilence, kSilence}; // dB, left and right
    float peak = kSilence;                             // dB, the loudest level since the user cleared it
};

// The mixer's FX channels, one for each of the aux buses A to C.
inline constexpr std::size_t kFxChannels = 3;

// An insert of an FX channel: the channel, from 0 for A, and the insert's place among the channel's effects.
struct EffectPlace
{
    friend bool operator==(const EffectPlace&, const EffectPlace&) = default;

    std::size_t channel = 0;
    std::size_t index = 0;
};

// A move of an FX channel's fader: the channel, and its level before and after the move, in dB.
struct FxMove
{
    std::size_t channel = 0;
    float from = 0.0f;
    float to = 0.0f;
};

// The FX channels' inserts, by the names of their effects. An insert's tooltip gives its effect's whole name: the
// insert cuts a long name short. An effect that `idle` marks shows grey, and its tooltip adds `idle_note`: the engine
// that plays the shown sound doesn't run it. A click on an effect sets `clicked` to its place. A click on a channel's
// empty insert opens a menu of `kinds`. A choice there sets `clicked` to the empty insert's place, and `kind` to the
// kind's index. The program then adds or opens the effect, and clears `clicked`. The insert at `open` shows that its
// window is open.
//
// Like a track's fader, each channel's fader sets the volume of its bus: `volumes`, in dB. The meters and the peaks
// show what each bus adds to the output. `held` names the fader that the user holds, with its level when the hold
// began. A hold that ends with a new level becomes `moved`. The program then makes the edit, and clears `moved`. The
// mixer shows the first `count` channels: the buses that both the file's console and the engine have.
struct FxChannels
{
    std::size_t count = kFxChannels;
    std::array<std::vector<std::string>, kFxChannels> effects;
    std::array<std::vector<bool>, kFxChannels> idle;
    std::string idle_note;
    std::vector<std::string> kinds;
    std::optional<EffectPlace> clicked;
    std::size_t kind = 0;
    std::optional<EffectPlace> open;
    std::array<float, kFxChannels> volumes{};
    std::array<std::array<float, 2>, kFxChannels> meters = {
        {{kSilence, kSilence}, {kSilence, kSilence}, {kSilence, kSilence}}}; // dB, left and right
    std::array<float, kFxChannels> peaks = {kSilence, kSilence, kSilence}; // dB, the loudest since the user cleared it
    std::optional<FxMove> held;
    std::optional<FxMove> moved;
};

} // namespace hokuto::app
