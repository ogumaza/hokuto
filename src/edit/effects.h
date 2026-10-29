// SPDX-License-Identifier: MIT

// The effects on the aux buses of a Hokuto file as ntdlib's models play them. A Switch file's effects are Nintendo's
// I3DL2 reverb, a Wii U file's are nw::snd's high and standard reverbs, chorus and delay, and a Wii file's are the
// effects of the SDK's AXFX library as NW4R SND's classes set them up. A 3DS file's are the reverbs and delays of
// nw::snd and nn::snd. The file's mixing modes take the models' form too.

#pragma once

#include <array>
#include <optional>
#include <vector>

#include "ntdlib/cafe/snd/effect_setup.h"
#include "ntdlib/rvl/snd/effect_setup.h"

#include "format/file.h"
#include "song/sequence_model.h"

namespace hokuto::edit
{

// The effects on the aux buses A to C.
using BusEffects = std::array<std::vector<format::Effect>, format::kAuxBuses>;

// The nw::snd class of one of the Wii U's kinds, or nothing for a kind of another console.
std::optional<ntdlib::cafe::snd::EffectType> WiiUTypeOf(format::EffectKind kind);

// The class of one of the Wii's kinds, or nothing for a kind of another console.
std::optional<ntdlib::rvl::snd::EffectType> WiiTypeOf(format::EffectKind kind);

// The name of an effect's kind in the mixer: the name of its library and its class. No two kinds share a name. The
// 3DS's and the Wii U's nw::snd both have a delay, and each delay's name gives its console.
const char* EffectName(format::EffectKind kind);

// Whether a sound on `engine` plays effects of `kind`. The Switch runs the I3DL2 reverb, the Wii U nw::snd's four
// classes, the 3DS the reverbs and delays of nw::snd and nn::snd, and the Wii the classes of the AXFX library. Each
// console's model leaves the other kinds out.
bool RunsEffect(format::Engine engine, format::EffectKind kind);

// The parameters that a new effect of the 3DS of `kind` starts from, in a file's order: the defaults of its class's
// Param, the same in nw::snd and nn::snd. Another kind gives none.
std::vector<float> CtrStartingParameters(format::EffectKind kind);

// The effects in the form that the model plays. An effect whose parameters don't suit its kind is left out.
song::AuxEffects ModelEffects(const BusEffects& effects);

// A mixing mode in the form that the model plays.
song::Mixing ModelMixing(format::Mixing mixing);

} // namespace hokuto::edit
