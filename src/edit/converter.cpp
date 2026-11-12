// SPDX-License-Identifier: MIT

// The timelines' edits of a converted song (edit/converter.h): the settled layout, the items that an edit's commands
// become, and the empty song. The reading, the layout, the hints, the header and the loops are in converted_song.cpp
// and the converter_*.cpp files.

#include "edit/converter.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/commands/units.h"

#include "edit/converted_song.h"

namespace hokuto::edit
{

namespace
{

using converter_detail::Anchor;
using converter_detail::BlocksOf;
using converter_detail::Event;
using converter_detail::kLabelPrefix;
using converter_detail::LayOut;
using converter_detail::ReadSong;
using converter_detail::Song;
using converter_detail::TicksOf;
using converter_detail::Track;
using ntdlib::commands::Command;
using ntdlib::commands::Kind;
using ntdlib::commands::kOne;
using ntdlib::commands::Label;
using ntdlib::commands::Sequence;

// Keeps the splits of `track` that the track of the same number in `old`, the song before the edit, held at the same
// ticks (SettledLayout).
void KeepOldSplits(Track& track, const Song& old)
{
    // The splits that the track held before the edit, by their ticks.
    std::multiset<uint32_t> kept;
    for (const Track& previous : old.tracks)
    {
        if (previous.number != track.number)
        {
            continue;
        }

        for (const Event& event : previous.events)
        {
            if (event.split)
            {
                kept.insert(event.tick);
            }
        }
    }

    // The other splits go. An anchor counts the events before it without them.
    std::vector<Event> events;
    std::vector<std::size_t> gone_before = {0};
    for (const Event& event : track.events)
    {
        const auto found = event.split ? kept.find(event.tick) : kept.end();
        const bool gone = event.split && found == kept.end();
        if (found != kept.end())
        {
            kept.erase(found);
        }

        if (!gone)
        {
            events.push_back(event);
        }

        gone_before.push_back(gone_before.back() + (gone ? 1 : 0));
    }

    for (Anchor& anchor : track.anchors)
    {
        anchor.position -= gone_before[anchor.position];
    }

    track.events = std::move(events);
}

// Clears the opening of each track of `hints` whose blocks don't fit the commands of its track in `song` at tick 0
// (SettledLayout).
void DropUnfitOpenings(const Song& song, ConverterHints& hints)
{
    // A track's blocks before the song's start have to fit its commands at tick 0. A track without hints has no entry.
    for (TrackHints& track_hints : hints.tracks)
    {
        for (const Track& track : song.tracks)
        {
            if (track.number != track_hints.track || track_hints.opening.empty())
            {
                continue;
            }

            try
            {
                BlocksOf(track, track_hints, hints);
            }
            catch (const std::invalid_argument&)
            {
                track_hints.opening.clear();
            }
        }
    }
}

} // namespace

Sequence SettledLayout(const Sequence& before, const Sequence& edited, ConverterHints& hints)
{
    Song song = ReadSong(edited);
    const Song old = ReadSong(before);
    for (Track& track : song.tracks)
    {
        KeepOldSplits(track, old);
    }

    DropUnfitOpenings(song, hints);

    const auto empty = [](const TrackHints& track_hints)
    {
        return track_hints.opening.empty() && track_hints.kept_inline.empty() && track_hints.called.empty();
    };
    std::erase_if(hints.tracks, empty);

    // The song has to take its subroutines.
    LayOut(song, hints, true);

    return LayOut(song, hints, false);
}

std::vector<std::optional<uint32_t>> ItemsAfterJoins(const Sequence& split, const Sequence& joined)
{
    std::vector<std::optional<uint32_t>> items(split.items.size() + 1);

    const auto ticks = [](const Command& command)
    {
        return command.kind == Kind::kWait ? std::optional<uint32_t>(TicksOf(command.value)) : std::nullopt;
    };

    // The targets move with the joins.
    const auto same = [](Command a, const Command& b)
    {
        a.target = b.target;
        a.target_outside = b.target_outside;

        return a == b;
    };

    std::size_t j = 0;
    for (std::size_t i = 0; i < split.items.size();)
    {
        if (j < joined.items.size() && same(split.items[i], joined.items[j]))
        {
            items[i++] = static_cast<uint32_t>(j++);
            continue;
        }

        // A wait of 0 ticks that joined no other wait is left out. It leads to the next item.
        if (ticks(split.items[i]) == 0u && !(j < joined.items.size() && ticks(joined.items[j])))
        {
            items[i++] = static_cast<uint32_t>(j);
            continue;
        }

        // A run of waits joined into one.
        const std::optional<uint32_t> whole = j < joined.items.size() ? ticks(joined.items[j]) : std::nullopt;
        uint64_t sum = 0;
        while (whole && i < split.items.size() && sum < *whole && ticks(split.items[i]))
        {
            sum += *ticks(split.items[i]);
            items[i++] = static_cast<uint32_t>(j);
        }
        if (!whole || sum != *whole)
        {
            throw std::invalid_argument("the layouts differ in more than their waits");
        }

        ++j;
    }
    if (j != joined.items.size())
    {
        throw std::invalid_argument("the layouts differ in more than their waits");
    }

    items.back() = static_cast<uint32_t>(j);

    return items;
}

std::vector<std::optional<uint32_t>> MatchingItems(const Sequence& from, const Sequence& to)
{
    // The header is the same in both layouts: the items before the first track. The end matches the end.
    const std::size_t header = std::min(ReadSong(from).header.size(), ReadSong(to).header.size());
    std::vector<std::optional<uint32_t>> items(from.items.size() + 1);
    for (std::size_t item = 0; item < header; ++item)
    {
        items[item] = static_cast<uint32_t>(item);
    }

    items.back() = static_cast<uint32_t>(to.items.size());

    // A label that both layouts hold.
    std::map<std::string, uint32_t> labels;
    for (const Label& label : to.labels)
    {
        labels.emplace(label.name, label.item);
    }

    for (const Label& label : from.labels)
    {
        const auto found = labels.find(label.name);
        if (label.item >= header && label.item < from.items.size() && found != labels.end())
        {
            items[label.item] = found->second;
        }
    }

    return items;
}

std::string LabelPart(const std::string& name)
{
    std::string part = name.empty() ? std::string("song") : name;
    for (char& c : part)
    {
        const auto u = static_cast<unsigned char>(c);
        if (!(std::isalnum(u) && u < 0x80) && c != '_')
        {
            c = '_';
        }
    }

    return part;
}

std::string SongLabelPrefix(const std::string& name)
{
    return std::string(kLabelPrefix) + LabelPart(name) + "_";
}

Sequence EmptySong(const std::string& name)
{
    const std::string prefix = SongLabelPrefix(name);

    const auto command = [](Kind kind, int64_t value)
    {
        Command made;
        made.kind = kind;
        made.value.value = value * kOne;

        return made;
    };

    Sequence song;
    song.items = {command(Kind::kAllocateTracks, 1), command(Kind::kNoteWait, 0), command(Kind::kTempo, 120),
                  command(Kind::kEnd, 0), command(Kind::kEnd, 0)};
    song.labels = {{prefix + "Begin", 0},
                   {prefix + "End", 4},
                   {prefix + "Start", kEmptySongStart},
                   {prefix + "Track_0", kEmptySongStart}};

    return song;
}

} // namespace hokuto::edit
