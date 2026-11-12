// SPDX-License-Identifier: MIT

// The edits of a tune of Rare's GBA driver, by the rules of the driver's bytes (ntdlib/rare/commands.h). A tune's
// header names its tracks, and each track is a run of commands from its start to its end, or to the end of its loop
// after the start of its loop. Each command that plays names one of 16 MIDI channels. A delay sets the ticks to the
// next command.
//
// A note sounds until a note-off of its key on its channel: the note-off ends the channel's first note of the key that
// sounds. The edits place the commands of a track at their ticks:
// - A move shifts the note and its note-off to the new ticks. A new length moves the note-off. A note whose note-off
//   isn't on the same track supports neither edit.
// - A removal deletes the note and its note-off.
// - A new note is added with a note-off at the end of its length, on the channel of the track's nearest note.
// - A transpose changes the key of the note and of its note-off.
// Within a tick, a moved note-off comes first and a moved note last. The end of the track, or the end of its loop,
// keeps its tick, and no command can move past it.
//
// The track list and the count of tracks are in the tune's header, and keep their places. A tune can't gain a track. A
// removal deletes the notes and note-offs of the track, and its other commands stay.

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "ntdlib/commands/sequence.h"

#include "edit/drivers.h"
#include "song/archive.h"
#include "song/sequence.h"

namespace hokuto::edit
{

// The edits of Rare's driver (edit/drivers.h).
const DriverEdits& RareEdits();

// Whether `console` follows the rules of Rare's driver.
bool IsRare(const song::Console& console);

// The channel of the first command that names one in the run of commands from item `first` of `sequence` to the
// track's end, or nothing when none does.
std::optional<uint8_t> RareTrackChannel(const ntdlib::commands::Sequence& sequence, uint32_t first);

// The MIDI channel whose program an edit of the instrument of the track that ran `stretches` changes: the channel of
// the track's notes and program changes. The driver keeps one program for each channel, and each track that plays on
// the channel plays it. `offsets` gives the offset of each item of `sequence` (Offsets). Throws std::invalid_argument
// with a message for the user where the track plays on more than one channel, or where a command outside the track
// plays a note or changes the program on its channel. Returns nothing where the track names no channel.
std::optional<uint8_t> RareInstrumentChannel(const ntdlib::commands::Sequence& sequence,
                                             const std::vector<uint32_t>& offsets,
                                             std::span<const song::Stretch> stretches);

} // namespace hokuto::edit
