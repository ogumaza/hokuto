// SPDX-License-Identifier: MIT

// The loops of a converted song (edit/converter.h): the loop of a timeline, the loops of its tracks, and a moved loop.

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
using converter_detail::Event;
using converter_detail::LayOut;
using converter_detail::ReadSong;
using converter_detail::Song;
using converter_detail::Track;
using ntdlib::commands::Command;
using ntdlib::commands::Kind;
using ntdlib::commands::kOne;
using ntdlib::commands::Sequence;
using ntdlib::commands::Source;

// The anchors of the loop of `track`: the label that a jump that runs always goes back to, and the jump. Nothing for a
// track without such a jump.
std::optional<std::pair<std::size_t, std::size_t>> LoopAnchors(const Track& track)
{
    for (std::size_t jump = 0; jump < track.anchors.size(); ++jump)
    {
        const Anchor& anchor = track.anchors[jump];
        if (!anchor.command || anchor.command->kind != Kind::kJump || anchor.command->conditional)
        {
            continue;
        }

        for (std::size_t label = 0; label < jump; ++label)
        {
            if (!track.anchors[label].command && track.anchors[label].label == anchor.target)
            {
                return std::pair(label, jump);
            }
        }
    }

    return std::nullopt;
}

// The number of `track`'s events before `tick`.
std::size_t EventsBefore(const Track& track, uint32_t tick)
{
    const auto before = [tick](const Event& event)
    {
        return event.tick < tick;
    };
    return static_cast<std::size_t>(std::find_if_not(track.events.begin(), track.events.end(), before) -
                                    track.events.begin());
}

// The tick at which the last note of `track` ends, or the tick of its last event.
uint32_t LastSound(const Track& track)
{
    uint32_t last = 0;
    for (const Event& event : track.events)
    {
        last = std::max(last, event.tick);
        if (event.command.kind == Kind::kNote && event.command.value.source == Source::kValue &&
            event.command.value.value > 0 && event.command.value.value % kOne == 0)
        {
            last = static_cast<uint32_t>(
                std::max<uint64_t>(last, event.tick + static_cast<uint64_t>(event.command.value.value / kOne)));
        }
    }

    return last;
}

} // namespace

std::optional<ConvertedLoop> LoopOfConverted(const Sequence& timeline)
{
    std::optional<ConvertedLoop> loop;
    for (const Track& track : ReadSong(timeline).tracks)
    {
        const std::optional<std::pair<std::size_t, std::size_t>> anchors = LoopAnchors(track);
        if (!anchors)
        {
            continue;
        }

        const ConvertedLoop found{track.anchors[anchors->first].tick, track.anchors[anchors->second].tick};
        if (loop && *loop != found)
        {
            return std::nullopt;
        }

        loop = found;
    }

    return loop;
}

std::optional<std::vector<ConvertedLoop>> SongLoopsOf(const Sequence& timeline)
{
    std::optional<std::map<std::string, uint32_t>> starts;
    std::vector<ConvertedLoop> loops;
    for (const Track& track : ReadSong(timeline).tracks)
    {
        const std::optional<std::pair<std::size_t, std::size_t>> anchors = LoopAnchors(track);
        if (!anchors)
        {
            continue;
        }

        if (!starts)
        {
            starts = TrackStartsFrom(timeline, 0);
        }
        if (!starts)
        {
            return std::nullopt;
        }

        const auto start = starts->find(track.label);
        if (start == starts->end())
        {
            return std::nullopt;
        }

        loops.push_back(
            {track.anchors[anchors->first].tick + start->second, track.anchors[anchors->second].tick + start->second});
    }

    return loops;
}

namespace
{

// Moves the loop of `track`, whose loop start and loop end are its anchors `anchors`, to the ticks of `counted`
// (WithLoop). The track's end follows.
void LoopTrack(Track& track, std::pair<std::size_t, std::size_t> anchors, ConvertedLoop counted)
{
    const uint32_t old_end = track.anchors[anchors.second].tick;
    std::vector<std::pair<std::size_t, uint32_t>> moves = {{anchors.first, counted.start}};
    for (std::size_t anchor = anchors.second; anchor < track.anchors.size(); ++anchor)
    {
        const Anchor& jump = track.anchors[anchor];
        if (jump.tick == old_end && jump.command && jump.command->kind == Kind::kJump && !jump.command->conditional &&
            jump.target == track.anchors[anchors.first].label)
        {
            moves.emplace_back(anchor, counted.end);
        }
    }

    for (const auto& [anchor, tick] : moves)
    {
        track.anchors[anchor].tick = tick;
        track.anchors[anchor].position = EventsBefore(track, tick);
    }

    std::stable_sort(track.anchors.begin(), track.anchors.end(),
                     [](const Anchor& a, const Anchor& b) { return a.tick < b.tick; });

    // Commands after the jump end with an end, at the tick where the track ended. Without them, the track ends with
    // its jump.
    if (EventsBefore(track, counted.end) < track.events.size())
    {
        if (!track.end)
        {
            track.end = Command{.kind = Kind::kEnd};
            track.end_tick = std::max({old_end, track.end_tick, LastSound(track)});
        }
    }
    else
    {
        track.end.reset();
        track.after_end.clear();
        track.end_tick = counted.end;
    }

    track.end_tick = std::max(track.end_tick, counted.end);
}

} // namespace

Sequence WithLoop(const Sequence& timeline, const ConverterHints& hints, ConvertedLoop loop,
                  const std::map<std::string, uint32_t>& starts)
{
    if (loop.end <= loop.start)
    {
        throw std::invalid_argument("The loop end must come after the loop start.");
    }

    Song song = ReadSong(timeline);
    bool looped = false;
    for (Track& track : song.tracks)
    {
        const std::optional<std::pair<std::size_t, std::size_t>> anchors = LoopAnchors(track);
        if (!anchors)
        {
            continue;
        }

        // The track counts the loop's ticks from its start.
        const auto start = starts.find(track.label);
        const uint32_t offset = start != starts.end() ? start->second : 0;
        if (loop.start < offset)
        {
            throw std::invalid_argument("The loop can't start before the tracks start.");
        }

        const ConvertedLoop counted{loop.start - offset, loop.end - offset};

        // The label and the jump take their new ticks, each before the commands at its tick. A track can jump back to
        // the label more than once at the loop's end. Each of those jumps takes the new end. The anchors stay in the
        // order of their ticks.
        looped = true;
        LoopTrack(track, *anchors, counted);
    }
    if (!looped)
    {
        throw std::invalid_argument("The song has no loop.");
    }

    // A loop without a note sounds nothing. ntdlib's performer then finds no loop, and the arrangement shows none.
    const auto sounds = [&loop, &starts](const Track& track)
    {
        const auto start = starts.find(track.label);
        const uint32_t offset = start != starts.end() ? start->second : 0;
        const uint32_t from = loop.start > offset ? loop.start - offset : 0;
        const uint32_t to = loop.end > offset ? loop.end - offset : 0;
        const auto sounds_in_loop = [from, to](const Event& event)
        {
            return event.command.kind == Kind::kNote && event.command.velocity > 0 && event.tick >= from &&
                   event.tick < to;
        };
        return std::ranges::any_of(track.events, sounds_in_loop);
    };
    if (std::ranges::none_of(song.tracks, sounds))
    {
        throw std::invalid_argument("The loop would contain no notes. The sound would go silent after its last note.");
    }

    return LayOut(song, hints, false);
}

} // namespace hokuto::edit
