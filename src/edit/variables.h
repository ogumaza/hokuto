// SPDX-License-Identifier: MIT

// The variables of a sequence that comes in from a file: Replace… of a sequence and File > Import > Sequence…
// (edit/replace.h, edit/new_items.h). Nothing in an archive declares a variable. The game's code sets the variables
// that a sound reads. The target game's code may never set a variable that the new commands read, or may set it for
// another purpose. The Replace and Import windows list each variable that the new commands read before they set it.
// Each one keeps its number, moves to another number, or takes a fixed value (the maintainer, 2026-10-16).
//
// A move gives the variable its new number in every command that the player reaches from the sound's start. A fixed
// value goes in as a command at the sound's start that sets the variable. The sound then plays as a scene with that
// value would. A loop back to the start passes the new command. The player reads an allocation of tracks at the sound's
// start, and the new commands go after it.
//
// The game's code sets a sound's variables, 0 to 15, through the sound's handle. The global variables, 16 to 31, hold
// one value for every sound that plays. A command that set a global variable would change it for every sound. A fixed
// variable therefore takes one of the sound's variables that the new commands don't use for another variable, and that
// the game's code doesn't set: its number where it can, or else the lowest such number. The variables that the replaced
// commands read are the ones that the game's code sets.

#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <vector>

#include "ntdlib/commands/sequence.h"

#include "edit/replace.h"
#include "format/file.h"
#include "song/sequence.h"

namespace hokuto::edit
{

// The variables of a sound that a game can set through the sound's handle: 0 to 15. The global ones follow, up to
// song::kGameVariables.
inline constexpr int kSoundVariables = 16;

// What becomes of the variables of new commands. A variable that the map leaves out keeps its number.
struct VariableMap
{
    friend bool operator==(const VariableMap&, const VariableMap&) = default;

    std::map<int, int> moves; // a variable's number, and its new number
    std::map<int, int> fixed; // a variable's number, and its value
};

// The variables that a sound's commands read and set (UsesOf), a bit each for the numbers 0 to 63.
struct VariableUses
{
    uint64_t reads = 0;
    uint64_t sets = 0;
};

// The variables that the commands that the player reaches from item `start` of `sequence` read, and those that they
// set. A command that works out a variable's new value from its value reads it as well as setting it.
VariableUses UsesOf(const ntdlib::commands::Sequence& sequence, uint32_t start);

// Puts `commands` at the opening of the sound that starts at item `start` of `sequence`: before the item, or after an
// allocation of tracks there. Only the start leads to the new commands. A jump to the start, and a path that runs on
// into it, go past them. A label at the start names the sound's start then. Returns the item where the sound starts.
uint32_t InsertAtStart(ntdlib::commands::Sequence& sequence, uint32_t start,
                       std::vector<ntdlib::commands::Command> commands);

// A sequence after a variable map: its commands, the item where its sound starts, and the variable that holds each
// fixed value, by the fixed variable's number.
struct MappedVariables
{
    ntdlib::commands::Sequence sequence;
    uint32_t start = 0;
    std::map<int, int> holders;
};

// `sequence` with the moves and the fixed values of `variables` in the commands that the player reaches from item
// `start`. A fixed value's variable avoids those of `taken`, a bit each: the variables that the game's code sets. A
// fixed variable that the commands don't read takes no command. Throws std::invalid_argument with a message for the
// user for a variable past 31, a variable that both moves and takes a fixed value, a value past -32768 to 32767, moves
// that give two variables one number, a fixed value for which none of the sound's variables is free, and commands that
// can't move to make room for the new ones.
MappedVariables WithVariables(const ntdlib::commands::Sequence& sequence, uint32_t start, const VariableMap& variables,
                              uint32_t taken = 0);

// The sounds of `file` whose commands read each variable that a game can set, by the variable's number: the commands
// that the player reaches from each sound's start. `places` gives where each sound plays (SoundPlaces).
std::array<std::vector<uint32_t>, song::kGameVariables> VariableReaders(
    const format::File& file, std::span<const std::optional<SoundPlace>> places);

} // namespace hokuto::edit
