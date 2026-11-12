// SPDX-License-Identifier: MIT

// What brownie.cpp, brownie_notes.cpp, brownie_timeline.cpp and brownie_lengths.cpp share: the driver's tables of a
// ROM, the kinds of item that end a block, a wait, the value ranges of the commands, the length bytes of an edit, the
// note edits and their timelines. Only those files include it.

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <utility>
#include <vector>

#include "ntdlib/brownie/driver.h"
#include "ntdlib/commands/sequence.h"

#include "edit/bytecode.h"
#include "edit/commands.h"
#include "edit/notes.h"
#include "format/brownie_items.h"
#include "format/file.h"
#include "song/sequence.h"

namespace hokuto::edit::brownie_parts
{

namespace brownie = ntdlib::brownie;
using ntdlib::commands::Command;
using ntdlib::commands::Sequence;
using ntdlib::commands::Source;

// The frames that each length byte selects with each length set, in the ROM that a file builds.
struct Tables
{
    brownie::Revision revision = brownie::Revision::kSwordOfMana;
    std::vector<uint16_t> frames; // 96 for each set: 80 to DF
    int base = 0;                 // the MIDI key of the PSG's lowest key

    uint32_t Frames(uint8_t byte, int set) const
    {
        const std::size_t index = static_cast<std::size_t>(set) * 0x60 + (byte - 0x80u);
        return index < frames.size() ? frames[index] : frames[byte - 0x80u];
    }

    format::BrownieLength Length() const
    {
        return [this](uint8_t byte, int set)
        {
            return Frames(byte, set);
        };
    }
};

// The tables of the ROM that `file` builds. A file's versions share the pieces of its ROM, and the driver's tables are
// in them. The tables of the last few ROMs stay. Throws std::invalid_argument with a message for the user when Hokuto
// can't read the driver's tables there.
std::shared_ptr<const Tables> TablesOf(const format::File& file);

// Whether `item` moves a channel elsewhere, or may, or starts one: the command after it starts a block.
bool EndsBlock(const Command& item);

// Whether `item` uses a time that the edits can't count: bytes that no channel reads, the command that brings a sound
// back after a fade, and a command that waits for other lengths on the ways of its channels. Such an item is a block of
// its own.
bool Barrier(const Command& item);

// A wait command without a value.
Command Hold();

// The values that the driver's bytes hold in the value of `command` from `source` (ntdlib/brownie/commands.h). A note,
// a hold and a rest wait for their frames, and a release for its time. A command splits into the lengths that the
// length set of its channel has (Writable). A transpose and a tune take a signed byte, and the other values a byte. No
// command takes a random value or a variable.
std::optional<ValueRange> BrownieRange(const Command& command, Source source, bool time, brownie::Revision revision);

// `edited`, an edit of sequence `sequence` of `before`, with the length bytes that the frames of its commands require
// (brownie.h). Throws std::invalid_argument with a message for the user for a command whose frames Hokuto can't write,
// and for commands that the driver wouldn't read as the items say.
Edited BrownieWritable(const format::File& before, Edited edited, uint32_t sequence);

// EditNotes on the driver, before Writable (see the top of brownie.h).
Edited EditBrownieNotes(const format::File& file, uint32_t sequence, std::span<const NoteEdit> edits,
                        std::span<const NewNote> additions);

// A sequence's commands as an edit finds them: each item's offset, then the end, and the first item of each block,
// then the number of items.
struct Code
{
    const Sequence* commands = nullptr;
    brownie::Revision revision = brownie::Revision::kSwordOfMana;
    std::vector<uint32_t> offsets;
    std::vector<uint32_t> starts;
    std::vector<std::set<int>> channels;
};

// The commands of sequence `sequence` of `file`, with its blocks. Code enters a sequence at a target, a label, a
// sound's list, the item after a command that moves a channel elsewhere, and a barrier.
Code Examine(const format::File& file, uint32_t sequence);

// The item at `offset`. Throws std::invalid_argument with a message for the user where no item starts.
uint32_t ItemAt(const Code& code, uint32_t offset);

std::size_t BlockOf(const Code& code, uint32_t item);

// An item of a block as an edit rewrites it, with its index before the edit, or the new note or setting that it is.
struct Item
{
    Command command;
    std::optional<uint32_t> old;
    std::optional<std::size_t> added;
};

// A command that waits, at `start` in the block, with the commands that come before it. A vacant command stands where a
// moved note or the command that ended it was: `fill`, a rest or a hold, replaces it in the end.
struct Segment
{
    std::vector<Item> before;
    Item wait;
    uint32_t start = 0;
    uint32_t length = 0;
    bool vacant = false;
    Command fill;
};

// The commands of a block: the commands that wait, and the commands after the last of them.
struct Timeline
{
    std::vector<Segment> segments;
    std::vector<Item> after;
    uint32_t length = 0;
};

Timeline TimelineOf(const Code& code, std::size_t block);

// The index of the segment that starts at `tick`, or the number of segments at the block's end. The segment that the
// tick falls in splits in two. The second part contains what the first part continued: a hold, or a vacant command.
std::size_t SplitAt(Timeline& timeline, uint32_t tick);

// The segment whose command is item `old` from before the edit.
std::optional<std::size_t> SegmentOf(const Timeline& timeline, uint32_t old);

// The segment after `index` that ends the note there: the next note, rest or release. Nothing up to the block's end.
std::optional<std::size_t> EndOf(const Timeline& timeline, std::size_t index);

uint32_t StartOf(const Timeline& timeline, std::optional<std::size_t> index);

// A note inserted at `tick` that sounds up to `end`, where `ends` is inserted unless a note, a rest or a release comes
// sooner. A lengthened note (`strict`) moves the rests and releases before its end, and can't pass a note.
struct Placing
{
    Item note;
    uint32_t tick = 0;
    std::optional<uint32_t> end;
    std::optional<Item> ends;
    bool strict = false;
    bool moves = true;
};

// Puts `placing` in `timeline`. Throws std::invalid_argument with a message for the user for a note at or past the
// block's end, at the tick of another note, or past the next note.
void Place(Timeline& timeline, Placing placing);

// `file` with sequence `sequence` rewritten: each block of `timelines` as its timeline describes it, and every other
// block as it was. The targets, the labels and the places of the tables move with the blocks' starts. `added` counts
// the new notes or settings.
Edited Rewrite(const format::File& file, uint32_t sequence, const Code& code, std::map<std::size_t, Timeline> timelines,
               std::size_t added);

// The block of `stretch` whose ticks include `tick`, and the tick where it starts. A tick where a block ends and the
// next starts belongs to the next. Throws std::invalid_argument with a message for the user for a tick outside the
// stretch's blocks.
std::pair<std::size_t, uint32_t> BlockAt(const Code& code, const song::Stretch& stretch, uint32_t tick);

// The tick at which `block` starts in `stretch`. Throws std::invalid_argument with a message for the user for a block
// that the stretch doesn't run.
uint32_t BlockStart(const Code& code, const song::Stretch& stretch, std::size_t block);

} // namespace hokuto::edit::brownie_parts
