// SPDX-License-Identifier: MIT

// The timelines of the note edits of edit/brownie.h: a sequence's commands as an edit finds them, the blocks between
// the commands that move a channel, the timeline of a block in ticks, the placing of a note in a timeline, and the
// sequence that the timelines write.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <utility>
#include <vector>

#include "ntdlib/commands/sequence.h"

#include "edit/brownie.h"
#include "edit/brownie_parts.h"
#include "edit/bytecode.h"
#include "format/brownie_items.h"

namespace hokuto::edit
{
namespace
{

using brownie_parts::Barrier;
using brownie_parts::BlockAt;
using brownie_parts::BlockOf;
using brownie_parts::BlockStart;
using brownie_parts::Code;
using brownie_parts::EndOf;
using brownie_parts::EndsBlock;
using brownie_parts::Examine;
using brownie_parts::Hold;
using brownie_parts::Item;
using brownie_parts::ItemAt;
using brownie_parts::Place;
using brownie_parts::Placing;
using brownie_parts::Rewrite;
using brownie_parts::Segment;
using brownie_parts::SegmentOf;
using brownie_parts::SplitAt;
using brownie_parts::StartOf;
using brownie_parts::Timeline;
using brownie_parts::TimelineOf;
using ntdlib::commands::Command;
using ntdlib::commands::Kind;
using ntdlib::commands::Sequence;

// Adds `channel` to `channels` at each item of `sequence` that the channel reads from item `start`: through jumps and
// calls, up to a word, data, a return or an end.
void MarkChannel(const Sequence& sequence, int channel, uint32_t start, std::vector<std::set<int>>& channels)
{
    std::vector<bool> seen(sequence.items.size());
    std::vector<uint32_t> pending = {start};
    while (!pending.empty())
    {
        uint32_t at = pending.back();
        pending.pop_back();
        while (at < sequence.items.size() && !seen[at])
        {
            seen[at] = true;
            channels[at].insert(channel);
            const Command& item = sequence.items[at];
            if (format::BrownieIsWord(item) || item.kind == Kind::kData || item.kind == Kind::kReturn ||
                item.kind == Kind::kEnd)
            {
                break;
            }

            if (item.kind == Kind::kCall && !item.target_outside)
            {
                pending.push_back(item.target);
            }

            if (item.kind == Kind::kJump)
            {
                if (!item.target_outside)
                {
                    pending.push_back(item.target);
                }
                break;
            }

            at++;
        }
    }
}

// The channels that read each item of `sequence`, by the lists' channels.
std::vector<std::set<int>> ChannelsOf(const Sequence& sequence)
{
    std::vector<std::set<int>> channels(sequence.items.size());
    for (const Command& list : sequence.items)
    {
        if (list.kind != Kind::kOpenTrack || list.target_outside)
        {
            continue;
        }

        MarkChannel(sequence, list.index, list.target, channels);
    }

    return channels;
}

// The frames that the items of `code` from `first` up to `end` wait.
uint32_t FramesOf(const Code& code, uint32_t first, uint32_t end)
{
    uint32_t frames = 0;
    for (uint32_t i = first; i < end && i < code.commands->items.size(); i++)
    {
        const Command& item = code.commands->items[i];
        if (format::BrownieWaits(item))
        {
            frames += format::BrownieWaitOf(item);
        }
    }

    return frames;
}

// The blocks that `stretch` runs, with the tick at which each starts. Throws std::invalid_argument with a message for
// the user for a stretch whose commands aren't where the arrangement shows them.
std::vector<std::pair<std::size_t, uint32_t>> BlocksOf(const Code& code, const song::Stretch& stretch)
{
    const uint32_t first = ItemAt(code, stretch.first);
    const uint32_t last = ItemAt(code, stretch.last);
    std::vector<std::pair<std::size_t, uint32_t>> blocks;
    uint32_t tick = stretch.tick;
    for (std::size_t block = BlockOf(code, first); block + 1 < code.starts.size() && code.starts[block] <= last;
         block++)
    {
        const uint32_t from = std::max(code.starts[block], first);
        blocks.emplace_back(block, tick - FramesOf(code, code.starts[block], from));
        tick += FramesOf(code, from, code.starts[block + 1]);
    }

    return blocks;
}

// Whether `segment` changes what its channel plays: a note, a rest or a release. A hold continues what sounds.
bool IsEvent(const Segment& segment)
{
    return !segment.vacant && segment.wait.command.kind != Kind::kWait;
}

// Writes `timeline` into `out`, with the new index of each old item in `places` and of each new note or setting in
// `added`.
void Write(Timeline timeline, std::vector<Command>& out, std::vector<std::optional<uint32_t>>& places,
           std::vector<uint32_t>& added)
{
    const auto put = [&](Item item)
    {
        const auto index = static_cast<uint32_t>(out.size());
        if (item.old)
        {
            places[*item.old] = index;
        }

        if (item.added)
        {
            added[*item.added] = index;
        }

        out.push_back(std::move(item.command));
    };
    for (Segment& segment : timeline.segments)
    {
        for (Item& item : segment.before)
        {
            put(std::move(item));
        }

        if (segment.vacant)
        {
            segment.wait = {segment.fill, std::nullopt, std::nullopt};
        }

        format::SetBrownieWait(segment.wait.command, segment.length);
        put(std::move(segment.wait));
    }

    for (Item& item : timeline.after)
    {
        put(std::move(item));
    }
}

} // namespace

namespace brownie_parts
{

Code Examine(const format::File& file, uint32_t sequence)
{
    Code code;
    const Sequence& commands = SequenceToEdit(file, sequence);
    code.commands = &commands;
    code.revision = *BrownieOf(ConsoleOf(file));
    code.offsets = format::BrownieItemOffsets(commands, code.revision);
    code.channels = ChannelsOf(commands);
    const auto size = static_cast<uint32_t>(commands.items.size());
    std::set<uint32_t> starts = {0, size};
    for (uint32_t i = 0; i < size; i++)
    {
        const Command& item = commands.items[i];
        if (ntdlib::commands::HasTarget(item.kind) && !item.target_outside)
        {
            starts.insert(item.target);
        }

        if (EndsBlock(item))
        {
            starts.insert(i + 1);
        }

        if (Barrier(item) || format::BrownieIsWord(item))
        {
            starts.insert(i);
            starts.insert(i + 1);
        }
    }

    for (const ntdlib::commands::Label& label : commands.labels)
    {
        starts.insert(label.item);
    }

    ForEachItemPlace(*file.native, sequence,
                     [&starts](const ntdlib::sar::Place& place) { starts.insert(place.offset); });
    std::erase_if(starts, [size](uint32_t start) { return start > size; });
    code.starts.assign(starts.begin(), starts.end());

    return code;
}

uint32_t ItemAt(const Code& code, uint32_t offset)
{
    const std::optional<uint32_t> item = ItemAtOffset(code.offsets, offset);
    if (!item)
    {
        throw std::invalid_argument("The command isn't where the arrangement shows it.");
    }

    return *item;
}

std::size_t BlockOf(const Code& code, uint32_t item)
{
    return static_cast<std::size_t>(std::upper_bound(code.starts.begin(), code.starts.end(), item) -
                                    code.starts.begin()) -
           1;
}

Timeline TimelineOf(const Code& code, std::size_t block)
{
    Timeline timeline;
    std::vector<Item> before;
    for (uint32_t i = code.starts[block]; i < code.starts[block + 1]; i++)
    {
        const Command& item = code.commands->items[i];
        if (!format::BrownieWaits(item))
        {
            before.push_back({item, i, std::nullopt});
            continue;
        }

        const uint32_t frames = format::BrownieWaitOf(item);
        timeline.segments.push_back({std::move(before), {item, i, std::nullopt}, timeline.length, frames, false, {}});
        before.clear();
        timeline.length += frames;
    }

    timeline.after = std::move(before);

    return timeline;
}

std::size_t SplitAt(Timeline& timeline, uint32_t tick)
{
    if (tick >= timeline.length)
    {
        return timeline.segments.size();
    }

    for (std::size_t i = 0; i < timeline.segments.size(); i++)
    {
        Segment& segment = timeline.segments[i];
        if (segment.start == tick)
        {
            return i;
        }

        if (tick < segment.start + segment.length)
        {
            Segment second;
            second.wait = {segment.vacant ? segment.fill : Hold(), std::nullopt, std::nullopt};
            second.start = tick;
            second.length = segment.start + segment.length - tick;
            second.vacant = segment.vacant;
            second.fill = segment.fill;
            segment.length = tick - segment.start;
            timeline.segments.insert(timeline.segments.begin() + static_cast<std::ptrdiff_t>(i) + 1, std::move(second));
            return i + 1;
        }
    }

    return timeline.segments.size();
}

std::optional<std::size_t> SegmentOf(const Timeline& timeline, uint32_t old)
{
    for (std::size_t i = 0; i < timeline.segments.size(); i++)
    {
        if (timeline.segments[i].wait.old == old)
        {
            return i;
        }
    }

    return std::nullopt;
}

std::optional<std::size_t> EndOf(const Timeline& timeline, std::size_t index)
{
    for (std::size_t i = index + 1; i < timeline.segments.size(); i++)
    {
        if (IsEvent(timeline.segments[i]))
        {
            return i;
        }
    }

    return std::nullopt;
}

uint32_t StartOf(const Timeline& timeline, std::optional<std::size_t> index)
{
    return index ? timeline.segments[*index].start : timeline.length;
}

void Place(Timeline& timeline, Placing placing)
{
    std::size_t at = 0;
    if (placing.moves)
    {
        if (placing.tick >= timeline.length)
        {
            throw std::invalid_argument("A note can't go at or past the end of its part of the track's commands.");
        }

        at = SplitAt(timeline, placing.tick);
        Segment& segment = timeline.segments[at];
        if (!segment.vacant && segment.wait.command.kind == Kind::kNote)
        {
            throw std::invalid_argument(
                "Another note starts at that tick on this track. A channel plays one note at a time.");
        }

        segment.wait = std::move(placing.note);
        segment.vacant = false;
    }
    else
    {
        at = *SegmentOf(timeline, *placing.note.old);
    }

    if (!placing.end)
    {
        return;
    }

    const uint32_t end = *placing.end;
    if (placing.strict && end > timeline.length)
    {
        throw std::invalid_argument("A note can't last past its part of the track's commands.");
    }

    for (std::size_t i = at + 1; i < timeline.segments.size() && timeline.segments[i].start < end; i++)
    {
        Segment& segment = timeline.segments[i];
        if (segment.vacant)
        {
            segment.wait = {Hold(), std::nullopt, std::nullopt};
            segment.vacant = false;
            continue;
        }

        if (!IsEvent(segment))
        {
            continue;
        }
        if (!placing.strict)
        {
            return;
        }
        if (segment.wait.command.kind == Kind::kNote)
        {
            throw std::invalid_argument(
                "A note sounds until the track's next note at most. Move that note, or remove it, first.");
        }

        segment.wait = {Hold(), std::nullopt, std::nullopt};
    }

    if (end >= timeline.length || !placing.ends)
    {
        return;
    }

    const std::size_t split = SplitAt(timeline, end);
    Segment& segment = timeline.segments[split];
    if (!IsEvent(segment))
    {
        segment.wait = std::move(*placing.ends);
        segment.vacant = false;
    }
}

Edited Rewrite(const format::File& file, uint32_t sequence, const Code& code, std::map<std::size_t, Timeline> timelines,
               std::size_t added)
{
    const Sequence& before = *code.commands;
    const std::size_t size = before.items.size();
    Edited edited;
    edited.added.resize(added);
    std::vector<std::optional<uint32_t>> places(size + 1);
    std::vector<uint32_t> firsts(size + 1);
    Sequence after;
    after.labels = before.labels;
    for (std::size_t block = 0; block + 1 < code.starts.size(); block++)
    {
        firsts[code.starts[block]] = static_cast<uint32_t>(after.items.size());
        const auto timeline = timelines.find(block);
        if (timeline != timelines.end())
        {
            Write(std::move(timeline->second), after.items, places, edited.added);
            continue;
        }

        for (uint32_t i = code.starts[block]; i < code.starts[block + 1]; i++)
        {
            places[i] = static_cast<uint32_t>(after.items.size());
            after.items.push_back(before.items[i]);
        }
    }

    firsts[size] = static_cast<uint32_t>(after.items.size());
    places[size] = firsts[size];

    // Code enters only at the blocks' starts.
    MoveTargets(after, [&firsts](uint32_t item) { return firsts.at(item); });

    edited.items.assign(places.begin(), places.end() - 1);
    edited.file = file;
    edited.file.sequences[sequence] = std::move(after);
    MoveItemPlaces(edited.file, sequence, [&firsts](uint32_t item) { return firsts.at(item); });

    return edited;
}

std::pair<std::size_t, uint32_t> BlockAt(const Code& code, const song::Stretch& stretch, uint32_t tick)
{
    std::optional<std::pair<std::size_t, uint32_t>> found;
    for (const auto& [block, start] : BlocksOf(code, stretch))
    {
        const uint32_t length = FramesOf(code, code.starts[block], code.starts[block + 1]);
        if (tick >= start && tick <= start + length)
        {
            found = std::pair(block, start);
            if (tick < start + length)
            {
                break;
            }
        }
    }
    if (!found)
    {
        throw std::invalid_argument("A new command can't go at that tick on this track.");
    }

    return *found;
}

uint32_t BlockStart(const Code& code, const song::Stretch& stretch, std::size_t block)
{
    for (const auto& [found, start] : BlocksOf(code, stretch))
    {
        if (found == block)
        {
            return start;
        }
    }

    throw std::invalid_argument("The command isn't where the arrangement shows it.");
}

} // namespace brownie_parts

} // namespace hokuto::edit
