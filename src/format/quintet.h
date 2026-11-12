// SPDX-License-Identifier: MIT

// The revision of Quintet's driver in a Hokuto file. ntdlib reads a driver's commands by two traits
// (ntdlib::quintet::Traits): whether a note is a byte for its pitch and a byte for its note value (as in the S
// revision), and the driver's table of the 13 note values that a note's high nibble picks. Each driver whose notes use
// the table has the same table. A file's revision of the driver is 1 for the byte notes, and 0 for that table.
//
// ntdlib's decoder returns the length of a note, a rest or a wait in the driver's ticks (ntdlib/quintet/commands.h).
// ntdlib's performer counts 37 ticks to one tick of the driver (ntdlib/sequence_export/quintet/performance.h). A file
// stores the lengths in the performer's ticks, the ticks of the Key Editor. The library hokuto_driver_rules contains
// the functions (quintet_rules.cpp). The song layer links it without the format layer.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/commands/units.h"
#include "ntdlib/quintet/commands.h"

namespace hokuto::format
{

// The last revision: the byte notes.
inline constexpr uint8_t kQuintetMostRevision = 1;

// The note values that the A and J revisions hold in the driver's table.
inline constexpr std::array<uint8_t, 13> kQuintetLengths = {1, 2, 3, 4, 6, 8, 12, 16, 24, 32, 48, 64, 128};

// The revision of `traits`, or nothing for a table of note values that no revision uses.
std::optional<uint8_t> QuintetRevision(const ntdlib::quintet::Traits& traits);

// The performer's ticks to one tick of the driver.
inline constexpr int64_t kQuintetSongTicks = 37;

// Whether a command of `kind` has a length in ticks: a note, a rest or a wait.
bool QuintetTimed(ntdlib::commands::Kind kind);

// The scale of a command's value in a file: the performer's ticks for a length, and the driver's units otherwise.
ntdlib::commands::Scale QuintetScale(ntdlib::commands::Kind kind);

// The dot: D1, or 9B in the revision of notes of two bytes, in `encoding.opcode` of ntdlib's kOther item. A dot right
// after a note, a rest or a wait adds half the item's length again, rounded down (ntdlib's Sequencer::NoteTicks). A
// file folds each such dot into the item before it: the item has the whole length, and kQuintetDot in its
// `encoding.opcode`. The arrangement's note then lasts for the length of its command, and each later command starts at
// its tick.
inline constexpr uint16_t kQuintetDot = 0xD1;

// Whether item `item` of `sequence`, a sequence from ntdlib's decoder, is a dot that the item before it folds.
bool QuintetFoldsDot(const ntdlib::commands::Sequence& sequence, std::size_t item);

// The index in the file's sequence of each item of `sequence`, a sequence from ntdlib's decoder, and of its end. A dot
// gets the index of the item that folds it.
std::vector<uint32_t> QuintetFoldedItems(const ntdlib::commands::Sequence& sequence);

// The index of each item of a file's sequence, and of its end, in the items that QuintetDriverTicks returns.
std::vector<uint32_t> QuintetUnfoldedItems(const ntdlib::commands::Sequence& sequence);

// `sequence` from ntdlib's decoder, with each length in the performer's ticks and each dot folded into the item before
// it.
ntdlib::commands::Sequence QuintetTicks(ntdlib::commands::Sequence sequence);

// `sequence` with each length in the driver's ticks and each folded dot after its item, for ntdlib's encoder. Throws
// std::invalid_argument for a length that isn't a whole number of the driver's ticks, and for a dotted length that
// isn't a length plus half of it again.
ntdlib::commands::Sequence QuintetDriverTicks(ntdlib::commands::Sequence sequence);

// The offset of each item of a file's sequence `sequence`, and of its end, from `offsets`: the offsets of the items
// of QuintetDriverTicks(sequence). A folded dot's byte belongs to its item.
std::vector<uint32_t> QuintetFoldedOffsets(const ntdlib::commands::Sequence& sequence,
                                           const std::vector<uint32_t>& offsets);

ntdlib::quintet::Traits QuintetTraits(uint8_t revision);

} // namespace hokuto::format
