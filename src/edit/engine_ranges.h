// SPDX-License-Identifier: MIT

// The ranges of the values of commands for each family of engines without a driver (command_ranges.cpp). The classes
// of the families (edit/engine_edits.h) call them for ArgumentRange (edit/commands.h). Only engine_edits.cpp and
// command_ranges.cpp include this file.

#pragma once

#include <optional>

#include "ntdlib/commands/sequence.h"

#include "edit/commands.h"
#include "song/archive.h"

namespace hokuto::edit::range_detail
{

// The values that MusicPlayer2000, NITRO-Composer and DSE hold in the value of `command` from `source`. `time` asks for
// the range of the command's time. `note_arguments` is the number of bytes that a note of MusicPlayer2000 reads.
std::optional<ValueRange> Mp2kRange(const ntdlib::commands::Command& command, ntdlib::commands::Source source,
                                    bool time, int note_arguments);
std::optional<ValueRange> NtrRange(const ntdlib::commands::Command& command, ntdlib::commands::Source source,
                                   bool time);
std::optional<ValueRange> DseRange(const ntdlib::commands::Command& command, ntdlib::commands::Source source,
                                   bool time);

// The values that a sequence of the later consoles or the Wii can store in the value of `command` from `source`, by the
// grammar of `console`.
std::optional<ValueRange> ArchiveRange(const song::Console& console, const ntdlib::commands::Command& command,
                                       ntdlib::commands::Source source, bool time);

} // namespace hokuto::edit::range_detail
