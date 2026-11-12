// SPDX-License-Identifier: MIT

// What command_rows.cpp, command_changes.cpp and command_removals.cpp share: the item at an offset, the offsets
// that a console's bytes can hold, an edit as the driver's bytes hold it, and an edit that changes nothing.
// command_changes.cpp defines them. Only those files include it.

#pragma once

#include <cstdint>
#include <vector>

#include "ntdlib/commands/sequence.h"

#include "edit/bytecode.h"
#include "format/file.h"
#include "song/archive.h"

namespace hokuto::edit::commands_parts
{

// The item at offset `offset`, by the offsets that Offsets gives. Throws for an offset where no command starts.
uint32_t CommandItem(const std::vector<uint32_t>& offsets, uint32_t offset);

// The offsets of the items of `sequence` on `console`. Throws with a message for the user where the console's bytes
// can't hold a command.
std::vector<uint32_t> CheckedOffsets(const ntdlib::commands::Sequence& sequence, const song::Console& console);

// `edited`, an edit of sequence `sequence` of `before`. On the GBA's drivers, the edit as the driver's bytes can hold
// it (Writable). Throws std::invalid_argument with a message for the user where they can't. The other consoles' edits
// check their bytes before.
Edited Held(const format::File& before, Edited edited, uint32_t sequence);

// An edit that leaves sequence `sequence` of `file` as it is.
Edited Unchanged(const format::File& file, uint32_t sequence);

// The item of `commands`, whose items start at `offsets`, at offset `command` (ItemOf).
uint32_t ItemAt(const ntdlib::commands::Sequence& commands, const std::vector<uint32_t>& offsets, uint32_t command);

} // namespace hokuto::edit::commands_parts
