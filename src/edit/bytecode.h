// SPDX-License-Identifier: MIT

// What the edits of a sequence share: its commands as the console's bytecode holds them, and the places in the native
// archive's tables that name its commands.

#pragma once

#include <cstdint>
#include <initializer_list>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/sar/layout.h"

#include "format/file.h"
#include "song/archive.h"

namespace hokuto::edit
{

// For `file` with a native layout, the console whose player reads its sequences: the archive's flavor, or on the Wii
// the build of NW4R SND that the archive's version gives (format::BuildOfWiiArchive). Throws std::invalid_argument for
// a file without a native layout, or a Wii archive without its header.
song::Console ConsoleOf(const format::File& file);

// The commands of sequence `sequence` of `file`. Throws std::invalid_argument with a message for the user for a file
// without a native layout, or a sequence that the file lacks.
const ntdlib::commands::Sequence& SequenceToEdit(const format::File& file, uint32_t sequence);

// A file after an edit of a sequence, the new index of each of the sequence's items, and the index of each new note. An
// item that the edit removed has no index.
struct Edited
{
    format::File file;
    std::vector<std::optional<uint32_t>> items;
    std::vector<uint32_t> added;
};

// The commands of a sequence that set a part of a track's mix or instrument, by their items, and whether the track's
// start sets it for notes before them. An edit of the setting changes the commands. Where the track's start sets it,
// the edit can put a new command at the track's opening (InsertAtOpening).
struct Setters
{
    std::set<uint32_t> items;
    bool start = false;
};

// The offset of each item of `sequence` in its bytecode on `console`, then the end. The offsets come from the encoder
// that the export uses.
std::vector<uint32_t> Offsets(const ntdlib::commands::Sequence& sequence, const song::Console& console);

// The item that starts at `offset`, by the offsets that Offsets gives, or nothing where no item starts.
std::optional<uint32_t> ItemAtOffset(const std::vector<uint32_t>& offsets, uint32_t offset);

// Whether the bytes of `sequence` must keep their places: it has a target that's a byte offset, or bytes that no
// command was decoded from.
bool Fixed(const ntdlib::commands::Sequence& sequence);

// Whether a track runs on from `command` to the next item. Every command does but a jump, a return or an end that
// always runs.
bool RunsOn(const ntdlib::commands::Command& command);

// Whether the player can run on from the item before item `item` of sequence `sequence` of `file` into it: the item
// before runs on (RunsOn), or it's a return that a sound of the file reaches outside every call. Such a return has no
// call to return to, and the player runs on past it.
bool RunsInto(const format::File& file, uint32_t sequence, uint32_t item);

// Where InsertAtOpening put new items: the first of them, and their number.
struct OpeningInsertion
{
    uint32_t first = 0;
    uint32_t count = 0;
};

// Puts `commands` before item `first` of sequence `sequence` of `file`, the first command of track `track`. Only the
// track's opening leads to the new commands: the sound's start for track 0, and the commands that open the track for
// the others. Another sound or track may run on into `first`. Where the player can run on from the item before `first`
// (RunsInto), a jump before the new commands takes that path past them. Every other target, label and start that leads
// to `first` or past it moves with the items. On track 0, a label at `first` stays with the sound's start, unless a
// jump or a call reaches it: it then goes on naming the command that they reach. A sound's start counts from the
// bytecode's first item, whichever command that is. In a timeline (edit/timelines.h), the commands go in where
// NintendoWare's converter writes the first settings of the song's track that the player runs from `first` instead
// (TrackRunFrom, TrackOpening), and need no jump. Returns where the new items went: the commands, and the jump before
// them.
OpeningInsertion InsertAtOpening(format::File& file, uint32_t sequence, uint32_t first, int track,
                                 std::vector<ntdlib::commands::Command> commands);

// The items of `sequence` that the player reaches from item `start`: through jumps, calls and loops, and with
// `track_starts` through the track starts into the tracks that they open. A return inside a call, an end and bytes that
// no command reaches stop a path. A path goes on after a call, and after a command that runs only if the last
// comparison held. A return outside every call has no call to return to, and a path goes on past it.
std::vector<bool> Reached(const ntdlib::commands::Sequence& sequence, uint32_t start, bool track_starts);

// `name`, or `name` with the first number from 2 after it that `labels` don't hold.
std::string UniqueLabel(const std::vector<ntdlib::commands::Label>& labels, const std::string& name);

// Whether `place` names an item of sequence `sequence` of `native`: an item of a piece of the sequence, or an item of
// the sequence itself (ntdlib::sar::Place::sequence).
bool NamesItemOf(const format::NativeArchive& native, const ntdlib::sar::Place& place, uint32_t sequence);

// Moves the places of the archive's tables that name the items of sequence `sequence` of `file` by `moved`. `moved`
// gives the new index of an old item, or of the end. A field that measures a sound's start from the bytecode's first
// item measures from the first item still.
template <typename Moved>
void MoveItemPlaces(format::File& file, uint32_t sequence, Moved moved)
{
    const auto move = [&](format::NativeArchive& native)
    {
        for (ntdlib::sar::Field& field : native.fields)
        {
            if (NamesItemOf(native, field.from, sequence) && field.from.offset != 0)
            {
                field.from.offset = moved(field.from.offset);
            }
            if (NamesItemOf(native, field.to, sequence))
            {
                field.to.offset = moved(field.to.offset);
            }
        }
    };
    file.native.Edit(move);
}

// Calls `visit` with each place of `native`'s fields that names an item of sequence `sequence`. `Native` is
// format::NativeArchive or a const one. A const one gives const places.
template <typename Native, typename Visit>
void ForEachItemPlace(Native& native, uint32_t sequence, Visit visit)
{
    for (auto& field : native.fields)
    {
        for (auto* place : {&field.from, &field.to})
        {
            if (NamesItemOf(native, *place, sequence))
            {
                visit(*place);
            }
        }
    }
}

} // namespace hokuto::edit
