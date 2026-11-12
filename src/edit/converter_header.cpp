// SPDX-License-Identifier: MIT

// The header of a converted song and the starts of its tracks (edit/converter.h): where a track's new settings, a new
// track and a sound's start go.

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

using converter_detail::HintsOfTrack;
using converter_detail::kLabelPrefix;
using converter_detail::ReadSong;
using converter_detail::Song;
using converter_detail::Track;
using ntdlib::commands::Command;
using ntdlib::commands::Kind;
using ntdlib::commands::kOne;
using ntdlib::commands::Label;
using ntdlib::commands::Sequence;
using ntdlib::commands::Source;

// The tracks of `song` by the items where their labels in `timeline` start them. Where two tracks start at one item,
// the first keeps it.
std::map<uint32_t, const Track*> TracksByItem(const Song& song, const Sequence& timeline)
{
    std::map<uint32_t, const Track*> tracks;
    for (const Track& track : song.tracks)
    {
        for (const Label& label : timeline.labels)
        {
            if (label.name == track.label)
            {
                tracks.emplace(label.item, &track);
            }
        }
    }

    return tracks;
}

} // namespace

std::optional<uint32_t> TrackOpening(const Sequence& timeline, const ConverterHints& hints, uint8_t track)
{
    const Song song = ReadSong(timeline);
    for (const Track& candidate : song.tracks)
    {
        if (candidate.number != track)
        {
            continue;
        }

        // After the track's label and the commands that open it, and after the blocks before the song's start.
        std::size_t opening = 0;
        for (const uint32_t count : HintsOfTrack(hints, track).opening)
        {
            opening += count;
        }
        if (opening > 0 && opening <= candidate.events.size())
        {
            return candidate.events[opening - 1].item + 1;
        }

        for (const Label& label : timeline.labels)
        {
            if (label.name == candidate.label)
            {
                return label.item + static_cast<uint32_t>(candidate.prefix.size());
            }
        }
    }

    return std::nullopt;
}

std::optional<uint8_t> TrackRunFrom(const Sequence& timeline, uint32_t first)
{
    const Song song = ReadSong(timeline);
    const std::map<uint32_t, const Track*> tracks = TracksByItem(song, timeline);

    // A path through the header. A return there has no call to return to, and the player runs on past it.
    const auto header_end = static_cast<uint32_t>(song.header.size());
    std::vector<bool> visited(header_end, false);
    uint32_t item = first;
    while (item < header_end)
    {
        const Command& command = timeline.items[item];
        if (visited[item] || (command.kind == Kind::kEnd && !command.conditional) ||
            (command.kind == Kind::kJump && !command.conditional && command.target_outside))
        {
            return std::nullopt;
        }

        visited[item] = true;
        item = command.kind == Kind::kJump && !command.conditional ? command.target : item + 1;
    }

    const auto track = tracks.find(item);
    return track == tracks.end() ? std::nullopt : std::optional<uint8_t>(track->second->number);
}

std::optional<ConvertedStart> SongStartFrom(const Sequence& timeline, uint32_t first)
{
    const Song song = ReadSong(timeline);
    const std::map<uint32_t, const Track*> tracks = TracksByItem(song, timeline);

    // A path through the header up to the first command that starts a track. TrackRunFrom follows the same path.
    const auto header_end = static_cast<uint32_t>(song.header.size());
    if (first > header_end)
    {
        return std::nullopt;
    }

    std::vector<bool> visited(header_end, false);
    ConvertedStart start{first, 0};
    while (start.item < header_end)
    {
        const Command& command = timeline.items[start.item];
        const bool jumps = command.kind == Kind::kJump && !command.conditional;
        if ((command.kind == Kind::kOpenTrack && !command.conditional && !command.target_outside &&
             tracks.contains(command.target)) ||
            (jumps && !command.target_outside && tracks.contains(command.target)))
        {
            return start;
        }

        if (visited[start.item] || (command.kind == Kind::kEnd && !command.conditional) ||
            (jumps && command.target_outside))
        {
            return std::nullopt;
        }

        if (command.kind == Kind::kWait)
        {
            if (command.conditional || command.value.source != Source::kValue || command.value.value < 0)
            {
                return std::nullopt;
            }

            start.tick += static_cast<uint32_t>(command.value.value / kOne);
        }

        visited[start.item] = true;
        start.item = jumps ? command.target : start.item + 1;
    }

    return tracks.contains(start.item) ? std::optional(start) : std::nullopt;
}

std::optional<std::map<std::string, uint32_t>> TrackStartsFrom(const Sequence& timeline, uint32_t first)
{
    const Song song = ReadSong(timeline);
    const std::map<uint32_t, const Track*> tracks = TracksByItem(song, timeline);

    // A track that the path doesn't start takes the tick where the header starts the song's first track.
    std::map<std::string, uint32_t> starts;
    const auto finish = [&]() -> std::optional<std::map<std::string, uint32_t>>
    {
        const std::optional<ConvertedStart> song_start = SongStartFrom(timeline, first);
        if (starts.empty() || !song_start)
        {
            return std::nullopt;
        }

        for (const Track& track : song.tracks)
        {
            starts.emplace(track.label, song_start->tick);
        }

        return starts;
    };

    // A path through the header, on past the commands that open tracks. SongStartFrom follows the same path.
    const auto header_end = static_cast<uint32_t>(song.header.size());
    if (first > header_end)
    {
        return std::nullopt;
    }

    std::vector<bool> visited(header_end, false);
    uint32_t item = first;
    uint32_t tick = 0;
    while (item < header_end)
    {
        const Command& command = timeline.items[item];
        const bool jumps = command.kind == Kind::kJump && !command.conditional;
        const bool opens = command.kind == Kind::kOpenTrack && !command.conditional;
        const auto track = (jumps || opens) && !command.target_outside ? tracks.find(command.target) : tracks.end();
        if (track != tracks.end())
        {
            starts.emplace(track->second->label, tick);
            if (jumps)
            {
                return finish();
            }
        }

        if (visited[item] || (command.kind == Kind::kEnd && !command.conditional) || (jumps && command.target_outside))
        {
            return finish();
        }

        if (command.kind == Kind::kWait)
        {
            if (command.conditional || command.value.source != Source::kValue || command.value.value < 0)
            {
                return std::nullopt;
            }

            tick += static_cast<uint32_t>(command.value.value / kOne);
        }

        visited[item] = true;
        item = jumps ? command.target : item + 1;
    }
    if (const auto run_into = tracks.find(item); run_into != tracks.end())
    {
        starts.emplace(run_into->second->label, tick);
    }

    return finish();
}

ConvertedTrackPlace NewTrackPlace(const Sequence& timeline, uint8_t number)
{
    const Song song = ReadSong(timeline);
    const std::string prefix = std::string(kLabelPrefix) + song.name + "_";
    ConvertedTrackPlace place{prefix + "Track_" + std::to_string(number), 0, {}};
    std::map<std::string, uint32_t> items;
    for (const Label& label : timeline.labels)
    {
        items.emplace(label.name, label.item);
    }

    std::optional<uint32_t> next;
    for (const Track& track : song.tracks)
    {
        if (track.number == number)
        {
            throw std::invalid_argument("The timeline already has a track " + std::to_string(number) +
                                        ", but the sound doesn't start it.");
        }

        place.tracks.push_back(track.label);
        if (!next && track.number > number)
        {
            next = items.at(track.label);
        }
    }

    place.before = next ? *next : items.at(prefix + "End");

    return place;
}

} // namespace hokuto::edit
