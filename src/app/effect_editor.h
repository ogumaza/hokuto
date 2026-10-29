// SPDX-License-Identifier: MIT

// The effects on the aux buses of a Hokuto file, and the window that edits an effect. A Switch file's effects are
// Nintendo's I3DL2 reverb. Its window shows the reverb's parameters in the units of the I3DL2 guidelines, with the
// gains in dB rather than millibels and the delays in milliseconds. A Wii U file's effects are nw::snd's high and
// standard reverbs, chorus and delay. A Wii file's are the high and standard reverbs, the chorus and the delay of the
// SDK's AXFX library, with the parameters of NW4R SND's classes in the file's build. A 3DS file's are the reverbs and
// delays of nw::snd and nn::snd. Their windows show the parameters of the classes, with the reverbs' modes and the
// 3DS's switches in menus. The window of a 3DS reverb shows the filters' sizes as times.

#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "app/views.h"
#include "edit/effects.h"
#include "format/file.h"

namespace hokuto::app
{

// The effects on the aux buses A to C.
using BusEffects = edit::BusEffects;

// The FX channels of `engine`, one for each aux bus of its console: A and B on the 3DS, and A to C on the other
// consoles.
std::size_t FxChannelsOf(format::Engine engine);

// The kinds of effect that an empty insert of `file` offers by their names, for a sound on `engine`: the kinds that the
// engine's console runs (edit::RunsEffect). NewEffect makes an effect of the kind at `kind`. A new I3DL2 reverb starts
// from nn::audio's first preset. A new effect of the Wii U or the Wii starts from Hokuto's starting values
// (song::WiiUStartingParameters and song::WiiStartingParameters), and a new effect of the 3DS from the libraries'
// defaults (edit::CtrStartingParameters). A file of the Wii offers the classes of NW4R SND's build: the reverbs of 2008
// from that build on, and the older ones before.
std::vector<std::string> EffectKindNames(const format::File& file, format::Engine engine);
format::Effect NewEffect(const format::File& file, format::Engine engine, std::size_t kind);

// The name of an effect's kind (edit::EffectName). The mixer's inserts show it.
const char* EffectName(const format::Effect& effect);

// The name of the effect at `place`: the channel, the insert's number and the effect, such as FX A: Ins. 1 - I3DL2
// Reverb. The effect's window takes it as its title.
std::string EffectTitle(const EffectPlace& place, const format::Effect& effect);

// What the user did to an effect in its window.
enum class EffectEdit
{
    kNone,
    kChanging, // a drag or a typed value is under way
    kChanged,  // a drag or a typed value ended
    kPreset,   // the user chose a preset
    kRemoved,  // the user removed the effect
};

// An edit's name and details in the Undo History.
struct ActionNames
{
    std::string name;
    std::string details;
};

// The names of `edit`. The edit changed the effect at `place` from `before` to `after`. A preset is Load Preset, with
// the preset's name. A change of one parameter is Change Parameter, with the parameter's name. Any other change is
// Change Parameters.
ActionNames EffectChangeNames(const EffectPlace& place, EffectEdit edit, const format::Effect& before,
                              const format::Effect& after);

// Draws the window of `effect`, the effect at `place`. The window's close button clears `*open`. A drag changes the
// parameters while it lasts. Returns what the user did to the effect.
EffectEdit DrawEffectWindow(const EffectPlace& place, format::Effect& effect, bool* open);

} // namespace hokuto::app
