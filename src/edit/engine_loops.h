// SPDX-License-Identifier: MIT

// The counts of the commands without a way out for each family of engines without a driver (bytecode.cpp). The classes
// of the families (edit/engine_edits.h) call them for CommandsWithoutWayOut (edit/bytecode.h). Only engine_edits.cpp
// and bytecode.cpp include this file.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "ntdlib/commands/sequence.h"

namespace hokuto::edit::loop_detail
{

// CommandsWithoutWayOut on the engines whose tracks loop through jumps and labels: the sound archives and
// MusicPlayer2000. `sound_starts` contains the items where sounds start. With `ds`, a note waits for its length where
// its track can have the note wait on, as on NITRO-Composer.
std::size_t CommandsWithoutWayOutByJumps(const ntdlib::commands::Sequence& sequence,
                                         std::span<const uint32_t> sound_starts, bool ds);

// CommandsWithoutWayOut on DSE.
std::size_t DseCommandsWithoutWayOut(const ntdlib::commands::Sequence& sequence);

} // namespace hokuto::edit::loop_detail
