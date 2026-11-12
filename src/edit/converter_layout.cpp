// SPDX-License-Identifier: MIT

// The layout of a converted song by the converter's rules (edit/converter.h): the blocks of each track, the calls, and
// the writing of the sequence.

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/commands/units.h"

#include "edit/converted_song.h"
#include "edit/converter.h"

namespace hokuto::edit
{

namespace
{

using converter_detail::Anchor;
using converter_detail::Block;
using converter_detail::BlocksOf;
using converter_detail::CalledBlocks;
using converter_detail::HintsOfTrack;
using converter_detail::kLabelPrefix;
using converter_detail::kPlacePrefix;
using converter_detail::LayOut;
using converter_detail::ReadSong;
using converter_detail::Song;
using converter_detail::Track;
using format::BarRun;
using ntdlib::commands::Command;
using ntdlib::commands::Kind;
using ntdlib::commands::kOne;
using ntdlib::commands::Label;
using ntdlib::commands::Sequence;

// The fewest commands of a bar that the converter calls. A block that takes no time counts one more, such as the rest
// that the converter took out of it.
constexpr std::size_t kFewestCalled = 8;

Command Wait(uint32_t ticks)
{
    Command wait;
    wait.kind = Kind::kWait;
    wait.value.value = static_cast<int64_t>(ticks) * kOne;

    return wait;
}

// A key for `commands` that distinguishes any two lists of commands, for maps of bodies.
std::string KeyOf(const std::vector<Command>& commands)
{
    std::string key;
    const auto add = [&key](const auto& value)
    {
        key.append(reinterpret_cast<const char*>(&value), sizeof(value));
    };
    for (const Command& command : commands)
    {
        add(command.kind);
        add(command.conditional);
        add(command.key);
        add(command.velocity);
        add(command.index);
        add(command.value.source);
        add(command.value.value);
        add(command.value.high);
        add(command.time.has_value());
        if (command.time)
        {
            add(command.time->source);
            add(command.time->value);
            add(command.time->high);
        }

        add(command.target);
        add(command.target_outside);
        add(command.encoding.opcode);
        add(command.encoding.value_bytes);
        add(command.encoding.unused_time);
        add(command.encoding.unused_random);
        add(command.encoding.unused_variable);
        add(command.bytes.size());
        key.append(command.bytes.begin(), command.bytes.end());
    }

    return key;
}

// The ticks at which the bars of `hints` start, up to and including the first one at or after `end`.
std::vector<uint32_t> BarStarts(const ConverterHints& hints, uint32_t end)
{
    std::vector<uint32_t> starts;
    uint32_t tick = 0;
    for (const BarRun& run : hints.bars)
    {
        for (uint32_t bar = 0; bar < run.count; ++bar)
        {
            starts.push_back(tick);
            if (tick >= end)
            {
                return starts;
            }

            tick += run.length;
        }
    }

    const uint32_t length = hints.bars.empty() ? 0 : hints.bars.back().length;
    if (length == 0)
    {
        starts.push_back(tick);
        if (tick < end)
        {
            starts.push_back(end);
        }

        return starts;
    }
    while (true)
    {
        starts.push_back(tick);
        if (tick >= end)
        {
            return starts;
        }

        tick += length;
    }
}

} // namespace

namespace converter_detail
{

const TrackHints& HintsOfTrack(const ConverterHints& hints, uint8_t track)
{
    static const TrackHints kNone;
    for (const TrackHints& candidate : hints.tracks)
    {
        if (candidate.track == track)
        {
            return candidate;
        }
    }

    return kNone;
}

} // namespace converter_detail

namespace
{

// The index of the bar that holds `tick`: the last one that starts at or before it.
std::size_t BarAt(const std::vector<uint32_t>& starts, uint32_t tick)
{
    const auto found = std::upper_bound(starts.begin(), starts.end(), tick);
    return static_cast<std::size_t>(std::max<std::ptrdiff_t>(found - starts.begin() - 1, 0));
}

} // namespace

namespace converter_detail
{

// The blocks of `track`: the blocks before the song's start, then its bars.
std::vector<Block> BlocksOf(const Track& track, const TrackHints& hints, const ConverterHints& song)
{
    std::vector<Block> blocks;

    // The blocks before the song's start, at tick 0.
    std::size_t event = 0;
    for (const uint32_t count : hints.opening)
    {
        if (event + count > track.events.size() || (count > 0 && track.events[event + count - 1].tick != 0))
        {
            throw std::invalid_argument("an opening past the commands at tick 0");
        }

        Block block;
        block.first = event;
        block.last = event + count;
        block.timeless = true;
        for (std::size_t i = block.first; i < block.last; ++i)
        {
            if (!track.events[i].split)
            {
                block.body.push_back(track.events[i].command);
            }
        }

        blocks.push_back(std::move(block));
        event += count;
    }

    // The bars, each up to the next bar line or the track's end.
    const uint32_t last_tick = track.events.empty() ? 0 : track.events.back().tick;
    const std::vector<uint32_t> starts = BarStarts(song, std::max(track.end_tick, last_tick + 1));
    for (std::size_t bar = 0; bar + 1 < starts.size() && starts[bar] <= std::max(track.end_tick, last_tick); ++bar)
    {
        Block block;
        block.start = starts[bar];
        block.end = std::min(starts[bar + 1], std::max(track.end_tick, block.start));
        block.bar = static_cast<uint32_t>(bar);
        block.short_bar = bar == 0 && starts.size() > 2 && starts[1] - starts[0] < starts[2] - starts[1];
        block.first = event;
        while (event < track.events.size() && track.events[event].tick < starts[bar + 1])
        {
            ++event;
        }

        block.last = event;

        uint32_t tick = block.start;
        for (std::size_t i = block.first; i < block.last; ++i)
        {
            if (track.events[i].tick > tick)
            {
                block.body.push_back(Wait(track.events[i].tick - tick));
                tick = track.events[i].tick;
            }

            if (!track.events[i].split)
            {
                block.body.push_back(track.events[i].command);
            }
        }
        if (block.end > tick)
        {
            block.body.push_back(Wait(block.end - tick));
        }

        blocks.push_back(std::move(block));
    }
    if (event != track.events.size())
    {
        throw std::invalid_argument("events after the last bar");
    }

    // The bars that hold a label or the jump. A label or a jump between two blocks belongs to the later one.
    for (const Anchor& anchor : track.anchors)
    {
        const std::size_t bar = BarAt(starts, anchor.tick);
        for (Block& block : blocks)
        {
            if (block.bar == bar || (block.timeless && anchor.position > block.first && anchor.position < block.last))
            {
                block.marked = true;
            }
        }
    }

    return blocks;
}

// Whether the converter calls each block of `blocks`: the blocks that the rules let it call and that repeat, and those
// that `hints` names. A block that holds a label or the jump stays in its track, whatever the hints say. An edit of the
// loop can move the label or the jump into a block that the hints name.
std::vector<bool> CalledBlocks(const std::vector<Block>& blocks, const TrackHints& hints)
{
    const auto listed = [](const std::vector<uint32_t>& list, const Block& block)
    {
        return block.bar && std::binary_search(list.begin(), list.end(), *block.bar);
    };

    std::vector<std::string> keys;
    std::map<std::string, int> copies;
    std::vector<bool> may(blocks.size(), false);
    for (std::size_t i = 0; i < blocks.size(); ++i)
    {
        const Block& block = blocks[i];
        keys.push_back(KeyOf(block.body));
        const std::size_t counted = block.body.size() + (block.end == block.start ? 1 : 0);
        may[i] = !block.marked && !block.short_bar && counted >= kFewestCalled && !listed(hints.kept_inline, block);
        if (listed(hints.called, block) && !block.marked)
        {
            may[i] = true;
            copies[keys[i]] += 2;
        }
        if (may[i])
        {
            ++copies[keys[i]];
        }
    }

    std::vector<bool> called(blocks.size(), false);
    for (std::size_t i = 0; i < blocks.size(); ++i)
    {
        called[i] = may[i] && copies[keys[i]] >= 2;
    }

    return called;
}

} // namespace converter_detail

namespace
{

// The rests in the track's code join across bar lines. A call, a label and the jump split them.
class TrackWriter
{
public:
    TrackWriter(LabelWriter& writer, const Track& track) : writer_(writer), track_(track)
    {
    }

    // Writes a rest from the current tick up to `to`, when `to` is later.
    void RestTo(uint32_t to)
    {
        if (to > tick_)
        {
            writer_.Item(Wait(to - tick_));
            tick_ = to;
        }
    }

    // Writes the track's next anchors at or before event `position` and tick `up_to_tick`, each after a rest to its
    // tick.
    void AnchorsTo(std::size_t position, uint32_t up_to_tick)
    {
        while (anchor_ < track_.anchors.size() && track_.anchors[anchor_].position <= position &&
               track_.anchors[anchor_].tick <= up_to_tick)
        {
            const Anchor& next = track_.anchors[anchor_];
            RestTo(next.tick);
            if (next.command)
            {
                writer_.Item(*next.command, next.target);
            }
            else
            {
                writer_.Label(next.label);
            }

            ++anchor_;
        }
    }

    // Moves the current tick on to `end` after a call, unless it is later already.
    void CallTo(uint32_t end)
    {
        tick_ = std::max(tick_, end);
    }

    bool AllAnchorsWritten() const
    {
        return anchor_ == track_.anchors.size();
    }

private:
    LabelWriter& writer_;
    const Track& track_;
    uint32_t tick_ = 0;
    std::size_t anchor_ = 0;
};

// Writes the blocks of `track`'s code: a call for each block in `called`, the events of the others. Returns the
// bodies of the subroutines, in the order of their first calls.
std::vector<const Block*> WriteBlocks(LabelWriter& writer, TrackWriter& code, const Track& track,
                                      const std::vector<Block>& blocks, const std::vector<bool>& called)
{
    std::map<std::string, std::string> names;
    std::vector<const Block*> bodies;
    for (std::size_t i = 0; i < blocks.size(); ++i)
    {
        const Block& block = blocks[i];
        if (called[i])
        {
            code.AnchorsTo(block.first, block.start);
            code.RestTo(block.start);
            auto [name, added] = names.emplace(KeyOf(block.body), std::string());
            if (added)
            {
                name->second = track.label + "_" + std::to_string(bodies.size());
                bodies.push_back(&block);
            }

            writer.Item(Command{.kind = Kind::kCall}, name->second);
            code.CallTo(block.end);
            continue;
        }

        for (std::size_t event = block.first; event < block.last; ++event)
        {
            code.AnchorsTo(event, track.events[event].tick);
            code.RestTo(track.events[event].tick);
            if (!track.events[event].split)
            {
                writer.Item(track.events[event].command);
            }
        }
    }

    return bodies;
}

// Writes the subroutines of `track` with `bodies`, after its code.
void WriteSubroutines(LabelWriter& writer, const Track& track, const std::vector<const Block*>& bodies)
{
    // The subroutines, in the order of their first calls.
    for (std::size_t i = 0; i < bodies.size(); ++i)
    {
        writer.Label(track.label + "_" + std::to_string(i));
        for (const Command& command : bodies[i]->body)
        {
            writer.Item(command);
        }

        writer.Item(Command{.kind = Kind::kReturn});
    }
}

// Writes `track`'s code and its subroutines.
void WriteTrack(LabelWriter& writer, const Track& track, const TrackHints& hints, const ConverterHints& song,
                bool subroutines)
{
    const std::vector<Block> blocks = BlocksOf(track, hints, song);
    const std::vector<bool> called = subroutines ? CalledBlocks(blocks, hints) : std::vector<bool>(blocks.size());

    writer.Label(track.label);
    for (const std::string& label : track.start_labels)
    {
        writer.Label(label);
    }

    for (const Command& command : track.prefix)
    {
        writer.Item(command);
    }

    TrackWriter code(writer, track);
    const std::vector<const Block*> bodies = WriteBlocks(writer, code, track, blocks, called);
    code.AnchorsTo(track.events.size(), track.end_tick);
    code.RestTo(track.end_tick);
    if (!code.AllAnchorsWritten())
    {
        throw std::invalid_argument("a label or jump after the end of the track");
    }

    if (track.end)
    {
        writer.Item(*track.end);
    }

    for (const Command& command : track.after_end)
    {
        writer.Item(command);
    }

    WriteSubroutines(writer, track, bodies);
}

} // namespace

namespace converter_detail
{

Sequence LayOut(const Song& song, const ConverterHints& hints, bool subroutines)
{
    Sequence sequence;
    LabelWriter writer(sequence);

    for (std::size_t i = 0; i < song.header.size(); ++i)
    {
        for (const Label& label : song.header_labels)
        {
            if (label.item == i)
            {
                writer.Label(label.name);
            }
        }

        writer.Item(song.header[i], song.header_targets[i]);
    }

    for (const Track& track : song.tracks)
    {
        WriteTrack(writer, track, HintsOfTrack(hints, track.number), hints, subroutines);
    }

    writer.Label(std::string(kLabelPrefix) + song.name + "_End");
    writer.Item(song.end);

    const auto after = static_cast<uint32_t>(sequence.items.size());
    for (std::size_t i = 0; i < song.after.size(); ++i)
    {
        for (const Label& label : song.after_labels)
        {
            if (label.item == i)
            {
                writer.Label(label.name);
            }
        }

        Command command = song.after[i];
        if (ntdlib::commands::HasTarget(command.kind) && !command.target_outside)
        {
            command.target += after;
        }

        writer.Item(command);
    }

    writer.Finish();

    // The labels of places go, and the others are sorted by their names.
    const auto place = [](const ntdlib::commands::Label& label)
    {
        return label.name.starts_with(kPlacePrefix);
    };
    std::erase_if(sequence.labels, place);
    const auto by_name = [](const ntdlib::commands::Label& a, const ntdlib::commands::Label& b)
    {
        return a.name < b.name;
    };
    std::stable_sort(sequence.labels.begin(), sequence.labels.end(), by_name);

    return sequence;
}

} // namespace converter_detail

Sequence LayOutConverted(const Sequence& sequence, const ConverterHints& hints, bool subroutines)
{
    return LayOut(ReadSong(sequence), hints, subroutines);
}

LabelWriter::LabelWriter(Sequence& sequence) : sequence_(sequence)
{
}

void LabelWriter::Label(const std::string& name)
{
    pending_.push_back(name);
}

void LabelWriter::Item(const Command& command, const std::string& target)
{
    for (std::string& name : pending_)
    {
        sequence_.labels.push_back({std::move(name), static_cast<uint32_t>(sequence_.items.size())});
    }

    pending_.clear();
    if (!target.empty())
    {
        targets_.emplace_back(static_cast<uint32_t>(sequence_.items.size()), target);
    }

    sequence_.items.push_back(command);
}

void LabelWriter::Finish()
{
    if (!pending_.empty())
    {
        throw std::invalid_argument("a label after the last command");
    }

    std::map<std::string, uint32_t> items;
    for (const ntdlib::commands::Label& label : sequence_.labels)
    {
        items[label.name] = label.item;
    }

    for (const auto& [item, target] : targets_)
    {
        const auto found = items.find(target);
        if (found == items.end())
        {
            throw std::invalid_argument("a jump target with no label");
        }

        sequence_.items[item].target = found->second;
        sequence_.items[item].target_outside = false;
    }
}

} // namespace hokuto::edit
