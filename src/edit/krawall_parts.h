// SPDX-License-Identifier: MIT

// What krawall.cpp and krawall_patterns.cpp share: the effect of an item, the header of a Krawall module, and the walk
// through the patterns that finds whether a channel's next note names no instrument. Only those files include it.

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

#include "ntdlib/commands/sequence.h"

#include "format/file.h"

namespace hokuto::edit::krawall_parts
{

using ntdlib::commands::Command;
using ntdlib::commands::Sequence;

// The number of the effect that `command` contains, or nothing for an item that isn't an effect.
std::optional<uint8_t> EffectOf(const Command& command);

// The operand of an effect.
uint8_t OperandOf(const Command& command);

// What the edits read of a module's header. The header is in a run of the file, and its table of patterns contains the
// fields that name the patterns' starts (ntdlib/krawall/layout.h).
struct ModuleHeader
{
    bool instruments = false; // the module uses XM's instruments: its first flag (kModuleFlags)
    uint8_t order_count = 0;
    uint8_t restart = 0;
    std::array<uint8_t, 256> orders{};
    std::array<uint8_t, 64> song_starts{};
    std::vector<std::optional<uint32_t>> patterns; // the item where each pattern of the table starts
};

// The header of the module whose patterns sequence `sequence` of `file` contains. Nothing where no field of a table of
// patterns names the sequence, or where the header isn't in the field's run.
std::optional<ModuleHeader> HeaderOf(const format::File& file, uint32_t sequence);

// Whether, in a module with instruments, a note of channel `channel` that can play after the note at item `from` of
// `commands` names no instrument. That note plays the instrument of the note at `from`, and its sample, volume and pan
// (Player::PlayEntry). Where the rows break, jump or loop before the channel's next note, the walk visits every row of
// the patterns that they can lead to.
bool NextNoteLacksInstrument(const Sequence& commands, const ModuleHeader& module, uint32_t from, uint8_t channel);

} // namespace hokuto::edit::krawall_parts
