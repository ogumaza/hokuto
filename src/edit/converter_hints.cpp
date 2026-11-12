// SPDX-License-Identifier: MIT

// The hints of a converted song (edit/converter.h): the bar lines and the exceptions that lay the song out as the
// sequence holds it.

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "ntdlib/commands/units.h"

#include "edit/converted_song.h"
#include "edit/converter.h"

namespace hokuto::edit
{

namespace
{

using converter_detail::Block;
using converter_detail::BlocksOf;
using converter_detail::Call;
using converter_detail::CalledBlocks;
using converter_detail::LayOut;
using converter_detail::ReadSong;
using converter_detail::Song;
using converter_detail::Track;
using format::BarRun;
using ntdlib::commands::Command;
using ntdlib::commands::Kind;
using ntdlib::commands::kOne;
using ntdlib::commands::Sequence;
using ntdlib::commands::Source;

// The timebase of a song without a timebase command, in ticks a quarter note: NintendoWare's default.
constexpr uint32_t kDefaultTimebase = 48;

// The lengths of bar to try for a song without calls, in quarter notes, the likeliest first.
constexpr uint32_t kBarsInQuarterNotes[] = {4, 3, 2, 6, 5, 7, 1, 8};

// The most rounds of exceptions that a track takes before its calls settle.
constexpr int kRounds = 8;

// How a candidate's bars fill a stretch between two calls that no length of the song's calls fills.
enum class Fill
{
    kLongFirst,  // bars of the commonest length, then a shorter one
    kShortFirst, // a shorter bar, then bars of the commonest length
    kOneBar,     // one bar
};

// Bar lines to try for a song: the starts of its bars, from tick 0. `whole` marks one bar for a whole song without
// calls.
struct BarCandidate
{
    friend bool operator==(const BarCandidate&, const BarCandidate&) = default;

    std::vector<uint32_t> starts;
    bool whole = false;
};

// Whether the converter called each of `blocks`, from the calls of `track`: a call plays a block's events over the
// block's ticks. Nothing when a call plays no block.
std::optional<std::vector<bool>> ActualCalls(const Track& track, const std::vector<Block>& blocks)
{
    std::map<std::tuple<uint32_t, uint32_t, std::size_t, std::size_t>, std::size_t> places;
    for (std::size_t i = 0; i < blocks.size(); ++i)
    {
        places.emplace(std::tuple(blocks[i].start, blocks[i].end, blocks[i].first, blocks[i].last), i);
    }

    std::vector<bool> called(blocks.size(), false);
    for (const Call& call : track.calls)
    {
        const auto found =
            places.find(std::tuple(call.tick, call.tick + call.length, call.first, call.first + call.count));
        if (found == places.end())
        {
            return std::nullopt;
        }

        called[found->second] = true;
    }

    return called;
}

// The blocks before the song's start that the calls of `track` show, by the numbers of their commands. They hold the
// commands at tick 0 before a call of the first bar, or else up to the last call at tick 0 that takes no time. Each
// call that takes no time is a block, and so is each run of commands around them. Nothing when no call shows a block.
std::vector<uint32_t> OpeningOf(const Track& track)
{
    std::vector<std::pair<std::size_t, std::size_t>> timeless;
    std::size_t end = 0;
    for (const Call& call : track.calls)
    {
        if (call.tick != 0)
        {
            break;
        }

        if (call.length > 0)
        {
            end = call.first;
            break;
        }

        timeless.emplace_back(call.first, call.count);
        end = call.first + call.count;
    }

    std::vector<uint32_t> opening;
    std::size_t event = 0;
    for (const auto& [first, count] : timeless)
    {
        if (first > event)
        {
            opening.push_back(static_cast<uint32_t>(first - event));
        }

        opening.push_back(static_cast<uint32_t>(count));
        event = first + count;
    }
    if (end > event)
    {
        opening.push_back(static_cast<uint32_t>(end - event));
    }

    return opening;
}

// The hints with which the rules give the calls of `track` on the bar lines of `song`: the track's opening, and the
// bars that the rules would call, or leave in the track, otherwise. Nothing when a call plays no block on those bar
// lines, or when a block before the song's start would require an exception.
std::optional<TrackHints> TrackHintsOn(const Track& track, const ConverterHints& song)
{
    TrackHints hints;
    hints.track = track.number;
    hints.opening = OpeningOf(track);
    std::vector<Block> blocks;
    try
    {
        blocks = BlocksOf(track, hints, song);
    }
    catch (const std::invalid_argument&)
    {
        return std::nullopt;
    }

    const std::optional<std::vector<bool>> actual = ActualCalls(track, blocks);
    if (!actual)
    {
        return std::nullopt;
    }

    // An exception can make another copy of a bar qualify. The rounds go on until the calls settle.
    for (int round = 0; round < kRounds; ++round)
    {
        const std::vector<bool> ruled = CalledBlocks(blocks, hints);
        bool settled = true;
        for (std::size_t i = 0; i < blocks.size(); ++i)
        {
            if (ruled[i] == (*actual)[i])
            {
                continue;
            }
            if (!blocks[i].bar)
            {
                return std::nullopt;
            }

            std::vector<uint32_t>& list = (*actual)[i] ? hints.called : hints.kept_inline;
            list.insert(std::upper_bound(list.begin(), list.end(), *blocks[i].bar), *blocks[i].bar);
            settled = false;
        }
        if (settled)
        {
            return hints;
        }
    }

    return std::nullopt;
}

// The runs of the bars that start at `starts`, from tick 0. The bars of the last run go on past the starts, and the run
// keeps one bar.
std::vector<BarRun> RunsOf(const std::vector<uint32_t>& starts)
{
    std::vector<BarRun> runs;
    for (std::size_t i = 0; i + 1 < starts.size(); ++i)
    {
        const uint32_t length = starts[i + 1] - starts[i];
        if (!runs.empty() && runs.back().length == length)
        {
            ++runs.back().count;
        }
        else
        {
            runs.push_back({length, 1});
        }
    }
    if (!runs.empty())
    {
        runs.back().count = 1;
    }

    return runs;
}

// The hints that give `song`'s calls on the bars that start at `starts`. Nothing when the bars don't fit the calls.
std::optional<ConverterHints> HintsOn(const Song& song, const std::vector<uint32_t>& starts)
{
    ConverterHints hints;
    hints.bars = RunsOf(starts);
    for (const Track& track : song.tracks)
    {
        std::optional<TrackHints> track_hints = TrackHintsOn(track, hints);
        if (!track_hints)
        {
            return std::nullopt;
        }

        if (!track_hints->opening.empty() || !track_hints->kept_inline.empty() || !track_hints->called.empty())
        {
            hints.tracks.push_back(std::move(*track_hints));
        }
    }

    // The song's tracks follow their code. The hints follow the tracks' numbers.
    std::ranges::sort(hints.tracks, {}, &TrackHints::track);

    return hints;
}

// The number of bars that `hints` names as exceptions to the rules.
std::size_t ExceptionsOf(const ConverterHints& hints)
{
    std::size_t exceptions = 0;
    for (const TrackHints& track : hints.tracks)
    {
        exceptions += track.kept_inline.size() + track.called.size();
    }

    return exceptions;
}

// The song's timebase, in ticks a quarter note.
uint32_t TimebaseOf(const Song& song)
{
    for (const Command& command : song.header)
    {
        if (command.kind == Kind::kTimebase && command.value.source == Source::kValue && command.value.value >= kOne &&
            command.value.value % kOne == 0)
        {
            return static_cast<uint32_t>(command.value.value / kOne);
        }
    }

    return kDefaultTimebase;
}

// The starts of bars of `length` ticks from tick 0, up to and including the first one at or after `end`.
std::vector<uint32_t> EvenBars(uint32_t length, uint32_t end)
{
    std::vector<uint32_t> starts;
    for (uint32_t tick = 0;; tick += length)
    {
        starts.push_back(tick);
        if (tick >= end)
        {
            return starts;
        }
    }
}

// The starts of the bars that the calls of a song fix. `lines` holds the calls' starts and ends, and `lengths` their
// lengths, the commonest first. `fill` gives the bars of a stretch between two lines that no length fills, and
// `tail_bar` puts one bar after the last line instead of bars of the commonest length. The starts reach `end` or past
// it.
std::vector<uint32_t> CalledBars(const std::set<uint32_t>& lines, const std::vector<uint32_t>& lengths, uint32_t end,
                                 Fill fill, bool tail_bar)
{
    const uint32_t bar = lengths.front();
    std::vector<uint32_t> starts = {0};
    const auto add = [&starts](uint32_t tick)
    {
        if (tick > starts.back())
        {
            starts.push_back(tick);
        }
    };

    // Bars of the commonest length lead up to the first line, after a short first bar.
    const uint32_t first = *lines.begin();
    for (uint32_t tick = first % bar; tick < first; tick += bar)
    {
        add(tick);
    }

    add(first);

    // The stretches between the lines.
    uint32_t previous = first;
    for (const uint32_t line : lines)
    {
        if (line <= previous)
        {
            continue;
        }

        const uint32_t span = line - previous;
        const auto fits =
            std::find_if(lengths.begin(), lengths.end(), [span](uint32_t length) { return span % length == 0; });
        const uint32_t step = fits != lengths.end() ? *fits : bar;
        uint32_t from = previous + step;
        if (fits == lengths.end() && fill == Fill::kShortFirst)
        {
            from = previous + span % bar;
        }

        if (fits != lengths.end() || fill != Fill::kOneBar)
        {
            for (uint32_t tick = from; tick < line; tick += step)
            {
                add(tick);
            }
        }

        add(line);
        previous = line;
    }

    // After the last line, bars of the commonest length or one bar reach the end.
    if (tail_bar && previous < end)
    {
        add(end);
    }
    else
    {
        for (uint32_t tick = previous + bar;; tick += bar)
        {
            add(tick);
            if (tick >= end)
            {
                break;
            }
        }
    }

    return starts;
}

// The bar lines to try for `song`, as the starts of its bars from tick 0 to past its end. Each call plays one bar, and
// the calls' starts and ends are bar lines. Bars of the calls' commonest length fill the stretches between them where
// they fit, or else bars of another length of the calls. In a stretch that none of these lengths fills, the bars have
// lengths that no call shows. The candidates differ there: bars of the commonest length with a shorter one at the end
// or at the start, or one bar for the whole stretch. For a song without calls, the candidates are bars of several
// lengths, then one bar for the whole song.
std::vector<BarCandidate> BarCandidates(const Song& song)
{
    // The song's end, and the bar lines and the lengths of its calls.
    uint32_t end = 1;
    std::set<uint32_t> lines;
    std::map<uint32_t, std::size_t> counts;
    for (const Track& track : song.tracks)
    {
        end = std::max(end, track.end_tick);
        if (!track.events.empty())
        {
            end = std::max(end, track.events.back().tick + 1);
        }

        for (const Call& call : track.calls)
        {
            lines.insert(call.tick);
            if (call.length > 0)
            {
                lines.insert(call.tick + call.length);
                ++counts[call.length];
            }
        }
    }

    std::vector<BarCandidate> candidates;
    if (counts.empty())
    {
        const uint32_t timebase = TimebaseOf(song);
        for (const uint32_t quarters : kBarsInQuarterNotes)
        {
            candidates.push_back({EvenBars(quarters * timebase, end), false});
        }

        candidates.push_back({{0, end}, true});
        return candidates;
    }

    std::vector<uint32_t> lengths;
    for (const auto& [length, count] : counts)
    {
        lengths.push_back(length);
    }

    const auto commoner = [&counts](uint32_t a, uint32_t b)
    {
        return counts.at(a) > counts.at(b);
    };
    std::stable_sort(lengths.begin(), lengths.end(), commoner);

    for (const auto& [fill, tail_bar] : {std::pair(Fill::kLongFirst, false), std::pair(Fill::kShortFirst, false),
                                         std::pair(Fill::kOneBar, false), std::pair(Fill::kOneBar, true)})
    {
        BarCandidate candidate{CalledBars(lines, lengths, end, fill, tail_bar), false};
        if (std::find(candidates.begin(), candidates.end(), candidate) == candidates.end())
        {
            candidates.push_back(std::move(candidate));
        }
    }

    return candidates;
}

// A short text of `command`, such as "wait 96", for the messages.
std::string TextOf(const Command& command)
{
    std::string text(ntdlib::commands::Name(command.kind));
    if (command.kind == Kind::kNote)
    {
        text += " " + std::to_string(command.key);
    }

    if (command.value.source == Source::kValue && command.value.value % kOne == 0)
    {
        text += " " + std::to_string(command.value.value / kOne);
    }

    return text;
}

// Where `layout` first differs from `sequence`, in words: the first command that differs, then the first target, then
// the first label.
std::string FirstDifference(const Sequence& sequence, const Sequence& layout)
{
    const std::size_t count = std::min(sequence.items.size(), layout.items.size());
    for (std::size_t i = 0; i < count; ++i)
    {
        Command held = sequence.items[i];
        Command laid = layout.items[i];
        held.target = laid.target;
        held.target_outside = laid.target_outside;
        if (!(held == laid))
        {
            return "item " + std::to_string(i) + ": " + TextOf(held) + " in the sequence, " + TextOf(laid) +
                   " in the layout";
        }
    }

    if (sequence.items.size() != layout.items.size())
    {
        return std::to_string(sequence.items.size()) + " items in the sequence, " +
               std::to_string(layout.items.size()) + " in the layout";
    }

    for (std::size_t i = 0; i < count; ++i)
    {
        if (!(sequence.items[i] == layout.items[i]))
        {
            return "item " + std::to_string(i) + ": " + TextOf(sequence.items[i]) + " to item " +
                   std::to_string(sequence.items[i].target) + " in the sequence, to item " +
                   std::to_string(layout.items[i].target) + " in the layout";
        }
    }

    for (std::size_t i = 0; i < std::min(sequence.labels.size(), layout.labels.size()); ++i)
    {
        if (!(sequence.labels[i] == layout.labels[i]))
        {
            return "label " + sequence.labels[i].name + " at item " + std::to_string(sequence.labels[i].item) +
                   ", the layout's " + layout.labels[i].name + " at item " + std::to_string(layout.labels[i].item);
        }
    }

    return std::to_string(sequence.labels.size()) + " labels in the sequence, " + std::to_string(layout.labels.size()) +
           " in the layout";
}

} // namespace

ConvertedSong ReadConverted(const Sequence& sequence)
{
    Song song;
    try
    {
        song = ReadSong(sequence);
    }
    catch (const std::invalid_argument& error)
    {
        return {std::nullopt, error.what()};
    }

    // The hints of each candidate's bars, with the fewest exceptions first. One bar for a whole song without calls
    // comes last: it fits any such song, and it shows nothing of the song's bars.
    std::vector<std::pair<std::pair<bool, std::size_t>, ConverterHints>> ranked;
    for (const BarCandidate& candidate : BarCandidates(song))
    {
        std::optional<ConverterHints> hints = HintsOn(song, candidate.starts);
        if (hints)
        {
            ranked.emplace_back(std::pair(candidate.whole, ExceptionsOf(*hints)), std::move(*hints));
        }
    }

    const auto fewer = [](const auto& a, const auto& b)
    {
        return a.first < b.first;
    };
    std::stable_sort(ranked.begin(), ranked.end(), fewer);

    // The first hints that lay the song out as the sequence holds it.
    std::string problem = ranked.empty() ? "no bar lines fit the calls in the song" : std::string();
    for (const auto& [rank, hints] : ranked)
    {
        try
        {
            const Sequence layout = LayOut(song, hints, true);
            if (layout == sequence)
            {
                return {hints, {}};
            }

            if (problem.empty())
            {
                problem = FirstDifference(sequence, layout);
            }
        }
        catch (const std::invalid_argument& error)
        {
            if (problem.empty())
            {
                problem = error.what();
            }
        }
    }

    return {std::nullopt, problem};
}

} // namespace hokuto::edit
