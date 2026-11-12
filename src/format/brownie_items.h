// SPDX-License-Identifier: MIT

// The bytes of the commands of Brownie Brown's GBA driver in the form that the encoder of ntdlib writes
// (ntdlib/brownie/commands.h): the place of each item of a sequence, and whether the bytes of the revision hold a
// command. The format layer, the song layer and the edit layer share these rules.
//
// A note, a hold (FE), a rest (EC or ED) and a release (E0) wait for the length of the channel. The last length byte
// (80-DF) chose that length from the table of the driver. The bytes of those commands hold no number of frames. A
// Hokuto file stores in each of them the frames that every channel that reads it waits there: a release in its time,
// and the others in their value (FillBrownieWaits). The length bytes stay items of their own.

#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <set>
#include <vector>

#include "ntdlib/brownie/commands.h"
#include "ntdlib/brownie/driver.h"
#include "ntdlib/brownie/song.h"
#include "ntdlib/commands/sequence.h"
#include "ntdlib/commands/units.h"

namespace hokuto::format
{

// Whether `item` is a word of a list or of a table: an entry that opens a track, a jump through an address word, or an
// end with index 1.
bool BrownieIsWord(const ntdlib::commands::Command& item);

// The byte offset of each item of `sequence` in the bytes of `revision`, and the size of the bytes after them. A word
// of a list or of a table starts on a multiple of 4 in the ROM. The items before the first word of a sequence keep the
// layout of the ROM: the first word requires no zeros before it. Throws std::invalid_argument with the reason for a
// command that the revision's bytes can't hold (CheckBrownieCommand).
std::vector<uint32_t> BrownieItemOffsets(const ntdlib::commands::Sequence& sequence,
                                         ntdlib::brownie::Revision revision);

// Whether `item` waits for the length of its channel: a note, a hold, a rest or a release.
bool BrownieWaits(const ntdlib::commands::Command& item);

// The byte of `item` when it's a length byte (80-DF), or nothing.
std::optional<uint8_t> BrownieLengthByte(const ntdlib::commands::Command& item);

// The length set that `item` chooses in `revision` (EE), or nothing for another command. Revision 1's EE does nothing.
std::optional<int> BrownieLengthSet(const ntdlib::commands::Command& item, ntdlib::brownie::Revision revision);

// The frames that `item`, a command that waits, waits in a Hokuto file: a release's time, or the others' value. 0 where
// the file stores none.
uint32_t BrownieWaitOf(const ntdlib::commands::Command& item);

// Sets `item`, a command that waits, to wait `frames` frames, or none for 0.
void SetBrownieWait(ntdlib::commands::Command& item, uint32_t frames);

// The frames that length byte `byte` selects with length set `set`: 1 to 256.
using BrownieLength = std::function<uint32_t(uint8_t byte, int set)>;

// What the channels of a sequence read: for each item, the lengths and the length sets in force when a channel reaches
// it. A command that waits at the item waits for those lengths. An item that no channel reaches has neither.
struct BrownieReadings
{
    std::vector<std::set<uint32_t>> lengths;
    std::vector<std::set<int>> sets;
};

// Follows every channel of `sequence` from the entries of each list. The driver reads the channels the same way: a
// channel starts with a length of 256 frames and length set 0, and its loops, calls, returns and jumps take it on. A
// loop's end leads both back and on. `length` returns the frames of each length byte.
BrownieReadings ReadBrownieLengths(const ntdlib::commands::Sequence& sequence, ntdlib::brownie::Revision revision,
                                   const BrownieLength& length);

// Sets each command of `sequence` that waits to the frames that every channel waits there (BrownieWaitOf), or to none
// where the channels wait for other lengths, or no channel reaches it.
void FillBrownieWaits(ntdlib::commands::Sequence& sequence, ntdlib::brownie::Revision revision,
                      const BrownieLength& length);

} // namespace hokuto::format
