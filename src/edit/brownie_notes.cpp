// SPDX-License-Identifier: MIT

// The note edits of edit/brownie.h on Brownie Brown's driver (EditBrownieNotes): the edits by block, the new keys, the
// notes that move and their ends, and the new notes in the blocks that run at their ticks.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

#include "ntdlib/brownie/driver.h"
#include "ntdlib/commands/sequence.h"

#include "edit/brownie_parts.h"
#include "edit/bytecode.h"
#include "edit/notes.h"
#include "format/file.h"

namespace hokuto::edit
{
namespace
{

namespace brownie = ntdlib::brownie;
using brownie_parts::BlockAt;
using brownie_parts::BlockOf;
using brownie_parts::Code;
using brownie_parts::EndOf;
using brownie_parts::Examine;
using brownie_parts::Hold;
using brownie_parts::Item;
using brownie_parts::ItemAt;
using brownie_parts::Place;
using brownie_parts::Placing;
using brownie_parts::Rewrite;
using brownie_parts::Segment;
using brownie_parts::SegmentOf;
using brownie_parts::StartOf;
using brownie_parts::Timeline;
using brownie_parts::TimelineOf;
using ntdlib::commands::Command;
using ntdlib::commands::Kind;

// A rest of a channel that `channels` names: ED on the PSG's channels, and EC on the sample channels. Throws
// std::invalid_argument with a message for the user when channels of both kinds read the commands.
Command RestFor(const std::set<int>& channels)
{
    const bool psg = std::ranges::any_of(channels, [](int channel) { return channel < brownie::kFirstSampleChannel; });
    const bool samples =
        std::ranges::any_of(channels, [](int channel) { return channel >= brownie::kFirstSampleChannel; });
    if (psg && samples)
    {
        throw std::invalid_argument(
            "A channel of the PSG and a sample channel both read these commands. Their rests differ, and Hokuto can't "
            "write one for both.");
    }

    Command rest;
    rest.kind = Kind::kNoteOff;
    rest.index = psg ? 1 : 0;
    return rest;
}

// The key byte of a note command after `transpose` semitones. Throws std::invalid_argument with a message for the user
// past the driver's notes.
uint8_t Transposed(const Command& note, int transpose)
{
    const int key = note.key + transpose;
    if (key < 0 || key > 0x7F)
    {
        throw std::invalid_argument("A note of Brownie Brown's driver holds a byte from 0 to 127.");
    }

    return static_cast<uint8_t>(key);
}

// The timeline of block `block` of `code` in `timelines`. `timelines` gains the timeline the first time
// (EditBrownieNotes).
Timeline& TimelineAt(const Code& code, std::map<std::size_t, Timeline>& timelines, std::size_t block)
{
    auto found = timelines.find(block);
    if (found == timelines.end())
    {
        found = timelines.emplace(block, TimelineOf(code, block)).first;
    }

    return found->second;
}

// The edits of `edits` by the blocks of `code` that their commands sit in. Throws std::invalid_argument with a message
// for the user for an edit that the driver can't take.
std::map<std::size_t, std::vector<std::pair<uint32_t, const NoteEdit*>>> NamedEdits(const Code& code,
                                                                                    std::span<const NoteEdit> edits)
{
    const std::vector<Command>& items = code.commands->items;

    // A command that several edits name takes the first of them.
    std::set<uint32_t> named;
    std::map<std::size_t, std::vector<std::pair<uint32_t, const NoteEdit*>>> by_block;
    for (const NoteEdit& edit : edits)
    {
        const uint32_t item = ItemAt(code, edit.command);
        if (items[item].kind != Kind::kNote)
        {
            throw std::invalid_argument("The note's command isn't where the arrangement shows it.");
        }
        if (edit.velocity && *edit.velocity != 127)
        {
            throw std::invalid_argument(
                "A note of Brownie Brown's driver holds no velocity. Command F2 sets its channel's volume.");
        }

        if (named.insert(item).second)
        {
            by_block[BlockOf(code, item)].emplace_back(item, &edit);
        }
    }

    return by_block;
}

// The new keys of the notes that `block_edits` name in `timeline`, a timeline of `code`, and the rests that take the
// place of the notes that they remove (EditBrownieNotes).
void EditKeys(const Code& code, Timeline& timeline, std::span<const std::pair<uint32_t, const NoteEdit*>> block_edits)
{
    for (const auto& [item, edit] : block_edits)
    {
        const std::size_t index = *SegmentOf(timeline, item);
        Segment& segment = timeline.segments[index];
        segment.wait.command.key = Transposed(segment.wait.command, edit->transpose);
        if (!edit->remove)
        {
            continue;
        }

        // A release (E0) steps the envelope in the next frame. The step writes NRx2 and starts the PSG channel
        // again, at the frequency of the channel's last note. After the rest, the note's releases would sound the
        // note before it. The edit removes them with the note, and holds fill their frames.
        segment.wait = {RestFor(code.channels[item]), std::nullopt, std::nullopt};
        for (std::size_t next = index + 1; next < timeline.segments.size(); next++)
        {
            Segment& later = timeline.segments[next];
            if (later.vacant || later.wait.command.kind == Kind::kWait)
            {
                continue;
            }
            if (later.wait.command.kind != Kind::kRelease)
            {
                break;
            }

            later.vacant = true;
            later.fill = Hold();
        }
    }
}

// Adds to `placings` the notes of block `block` that `block_edits` move or resize in `timeline`, a timeline of `code`
// (EditBrownieNotes). Their places in the timeline become vacant.
void MoveNotes(const Code& code, Timeline& timeline, std::size_t block,
               std::span<const std::pair<uint32_t, const NoteEdit*>> block_edits,
               std::map<std::size_t, std::vector<Placing>>& placings)
{
    for (const auto& [item, edit] : block_edits)
    {
        if (edit->remove || (edit->shift == 0 && !edit->length))
        {
            continue;
        }

        const std::size_t index = *SegmentOf(timeline, item);
        const uint32_t tick = timeline.segments[index].start;
        const std::optional<std::size_t> end = EndOf(timeline, index);
        const uint32_t length = edit->length.value_or(StartOf(timeline, end) - tick);
        if (length == 0)
        {
            throw std::invalid_argument("A note lasts a frame at least.");
        }

        const int64_t moved = int64_t{tick} + edit->shift;
        if (moved < 0)
        {
            throw std::invalid_argument("A note can't move out of its part of the track's commands.");
        }

        // The rest or the release that ends the note moves with it, and so does a rest at a new end.
        Placing placing;
        placing.note = timeline.segments[index].wait;
        placing.tick = static_cast<uint32_t>(moved);
        placing.end = static_cast<uint32_t>(moved) + length;
        placing.ends = Item{RestFor(code.channels[item]), std::nullopt, std::nullopt};
        placing.strict = edit->shift == 0;
        placing.moves = edit->shift != 0;
        const bool own_end = end && timeline.segments[*end].wait.command.kind != Kind::kNote &&
                             (placing.moves || StartOf(timeline, end) < *placing.end);
        if (own_end)
        {
            Segment& ending = timeline.segments[*end];
            placing.ends = ending.wait;
            ending.vacant = true;
            ending.fill = Hold();
        }

        if (!placing.moves && !own_end && end && *placing.end > StartOf(timeline, end))
        {
            throw std::invalid_argument(
                "A note sounds until the track's next note at most. Move that note, or remove it, first.");
        }

        if (placing.moves && !end)
        {
            placing.end.reset();
        }

        if (placing.moves)
        {
            Segment& vacated = timeline.segments[index];
            vacated.vacant = true;
            vacated.fill = RestFor(code.channels[item]);
        }

        placings[block].push_back(std::move(placing));
    }
}

// Adds `additions`, the new notes, to `placings` in the blocks of `code` that run at their ticks, and the timelines of
// their blocks to `timelines` (EditBrownieNotes).
void PlaceAdditions(const Code& code, std::span<const NewNote> additions, std::map<std::size_t, Timeline>& timelines,
                    std::map<std::size_t, std::vector<Placing>>& placings)
{
    for (std::size_t index = 0; index < additions.size(); index++)
    {
        const NewNote& addition = additions[index];
        const auto [block, start] = BlockAt(code, addition.stretch, addition.tick);
        if (addition.key < 0 || addition.key > 0x7F)
        {
            throw std::invalid_argument("A note of Brownie Brown's driver holds a byte from 0 to 127.");
        }
        if (addition.length == 0)
        {
            throw std::invalid_argument("A note lasts a frame at least.");
        }

        Command note;
        note.kind = Kind::kNote;
        note.key = static_cast<uint8_t>(addition.key);
        Placing placing;
        placing.note = {note, std::nullopt, index};
        placing.tick = addition.tick - start;
        placing.end = placing.tick + addition.length;
        std::set<int> channels;
        for (uint32_t item = code.starts[block]; item < code.starts[block + 1]; item++)
        {
            channels.insert(code.channels[item].begin(), code.channels[item].end());
        }

        placing.ends = Item{RestFor(channels), std::nullopt, std::nullopt};
        TimelineAt(code, timelines, block);
        placings[block].push_back(std::move(placing));
    }
}

} // namespace

namespace brownie_parts
{

Edited EditBrownieNotes(const format::File& file, uint32_t sequence, std::span<const NoteEdit> edits,
                        std::span<const NewNote> additions)
{
    const Code code = Examine(file, sequence);
    std::map<std::size_t, Timeline> timelines;

    const std::map<std::size_t, std::vector<std::pair<uint32_t, const NoteEdit*>>> by_block = NamedEdits(code, edits);

    // Each block's keys and removals first, then the notes that move, then their ends.
    std::map<std::size_t, std::vector<Placing>> placings;
    for (const auto& [block, block_edits] : by_block)
    {
        Timeline& timeline = TimelineAt(code, timelines, block);
        EditKeys(code, timeline, block_edits);
        MoveNotes(code, timeline, block, block_edits, placings);
    }

    // The new notes go into the blocks that run at their ticks.
    PlaceAdditions(code, additions, timelines, placings);

    for (auto& [block, block_placings] : placings)
    {
        Timeline& timeline = timelines.at(block);
        for (Placing& placing : block_placings)
        {
            Place(timeline, std::move(placing));
        }
    }

    return Rewrite(file, sequence, code, std::move(timelines), additions.size());
}

} // namespace brownie_parts

} // namespace hokuto::edit
