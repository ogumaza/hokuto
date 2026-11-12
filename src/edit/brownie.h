// SPDX-License-Identifier: MIT

// The edits of the sounds of Brownie Brown's GBA driver, by the rules of its bytes (ntdlib/brownie/commands.h). A note,
// a hold, a rest and a release each wait for the channel's length. The last length byte chose that length. A Hokuto
// file stores for each of them the frames that it waits (format/brownie_items.h). After an edit, a length byte is
// inserted before each such command whose channels would read another length there. A command whose frames match no
// length byte in the length set of the channel splits into the command and holds of lengths that the set has.
//
// A note sounds until the next note, rest or release of its channel. A hold continues what sounds. A note's length in
// the edits is the time up to that command, and the sound can fade sooner. Every other note, rest and release keeps its
// tick:
// - A removed note leaves a rest of its frames.
// - A move removes the note, and a rest replaces it. The rest or the release that ends the note moves with it. The note
//   is inserted at its new tick, where the command that waits there splits in two. It sounds as long as before, unless
//   a note, a rest or a release comes sooner.
// - A shorter length puts a rest in at the note's new end. A longer one moves the rest or the release that ends the
//   note to the new end, with holds in its place. It can't take the note past the next note.
// - A new note is inserted as a moved note is, with a rest after its length.
// - A new setting is inserted before the command that waits at its tick. A setting moves the same way.
// As on the other consoles, the edits stay inside a block (edit/notes.h). A note's bytes hold no velocity.
//
// A sound's list names its channels: for each, a channel's number, its commands and its table of the places that its
// calls and jumps go to. A channel is free when no channel of the list plays on its part of the hardware. A new track
// uses a free channel of the kind of the track whose first note plays the chosen instrument, with the settings that the
// track has there. Without one, it uses a free sample channel, or else a free channel of the PSG. A copy and a pasted
// track read the commands of the track that they copy, from their place in the list. A removal deletes the channel from
// the list. The commands stay in the sequence.

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "ntdlib/brownie/driver.h"
#include "ntdlib/commands/sequence.h"

#include "edit/bytecode.h"
#include "edit/drivers.h"
#include "edit/notes.h"
#include "edit/tracks.h"
#include "format/file.h"
#include "song/archive.h"

namespace hokuto::edit
{

// The edits of Brownie Brown's driver (edit/drivers.h).
const DriverEdits& BrownieEdits();

// The revision of Brownie Brown's driver whose bytes `console` follows, or nothing for another driver or console.
std::optional<ntdlib::brownie::Revision> BrownieOf(const song::Console& console);

// AddSettings, MoveSettings and LimitsOf on Brownie Brown's driver (edit/notes.h).
Edited AddBrownieSettings(const format::File& file, uint32_t sequence, std::span<const NewSetting> settings);
Edited MoveBrownieSettings(const format::File& file, uint32_t sequence, std::span<const SettingMove> moves);
std::vector<NoteLimits> BrownieLimitsOf(const format::File& file, uint32_t sequence, std::span<const NotePlace> notes);

// The items where the channels of the list at item `list` of `sequence` start, by their channels. Nothing for an item
// that isn't a list.
std::vector<std::pair<int, uint32_t>> BrownieListStarts(const ntdlib::commands::Sequence& sequence, uint32_t list);

// The frames that a command which waits at item `item` of sequence `sequence` of `file` would wait: the length in force
// where the channels reach the item. 0 where they reach it with other lengths, or don't reach it.
uint32_t BrownieLengthAt(const format::File& file, uint32_t sequence, uint32_t item);

// The byte of a note command that shows at key `shown` on track `track` of `file`, with the channel's transpose
// `transpose` in semitones (song::BrownieShownKey). Nothing for a key that no byte shows at.
std::optional<int> BrownieCommandKey(const format::File& file, int track, int transpose, int shown);

} // namespace hokuto::edit
