// SPDX-License-Identifier: MIT

#include "edit/reach.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

#include "ntdlib/commands/sequence.h"

#include "edit/bytecode.h"
#include "edit/instruments.h"
#include "edit/mix.h"

namespace hokuto::edit
{
namespace
{

using ntdlib::commands::Command;
using ntdlib::commands::Kind;

// What the commands from a start reach: the items, and the openings of tracks among them, by the track's number and the
// item where the track opens.
struct Reachable
{
    std::vector<bool> items;
    std::set<std::pair<int, uint32_t>> openings;
};

// Another sound that plays the file: its place in the sound table, the item where it starts, and what its commands
// reach.
struct OtherSound
{
    uint32_t sound = 0;
    uint32_t start = 0;
    std::shared_ptr<const Reachable> reachable;
};

// What the commands from item `start` of `commands` reach, through the track starts too.
Reachable ReachableFrom(const ntdlib::commands::Sequence& commands, uint32_t start)
{
    Reachable reachable{Reached(commands, start, true), {}};
    for (uint32_t item = 0; item < reachable.items.size(); item++)
    {
        const Command& command = commands.items[item];
        if (reachable.items[item] && command.kind == Kind::kOpenTrack && !command.target_outside)
        {
            reachable.openings.emplace(command.index, command.target);
        }
    }

    return reachable;
}

// The sequence that sound `sound` plays. Throws std::invalid_argument with a message for the user for a sound that
// plays no sequence of the file.
uint32_t SequenceOf(std::span<const std::optional<SoundPlace>> places, uint32_t sound)
{
    if (sound >= places.size() || !places[sound])
    {
        throw std::invalid_argument("The sound plays no sequence that Hokuto can edit.");
    }

    return places[sound]->sequence;
}

// The sounds of `file` besides sound `sound` that play its sequence `commands`, and what their commands reach. Sounds
// that start at the same item share what they reach.
std::vector<OtherSound> OthersOf(const format::File& file, std::span<const std::optional<SoundPlace>> places,
                                 uint32_t sound, const ntdlib::commands::Sequence& commands)
{
    std::map<uint32_t, std::shared_ptr<const Reachable>> reachable;
    std::vector<OtherSound> others;
    for (const uint32_t other : SharingFile(places, sound))
    {
        const std::optional<uint32_t>& field = places[other]->start_field;
        const uint32_t start = field ? file.native->fields.at(*field).to.offset : 0;
        std::shared_ptr<const Reachable>& found = reachable[start];
        if (!found)
        {
            found = std::make_shared<const Reachable>(ReachableFrom(commands, start));
        }
        others.push_back({other, start, found});
    }

    return others;
}

// The items that a track ran in `stretches`. `offsets` gives the offset of each item (Offsets).
std::set<uint32_t> ItemsRun(const std::vector<uint32_t>& offsets, std::span<const song::Stretch> stretches)
{
    std::set<uint32_t> items;
    for (const song::Stretch& stretch : stretches)
    {
        const std::optional<uint32_t> first = ItemAtOffset(offsets, stretch.first);
        const std::optional<uint32_t> last = ItemAtOffset(offsets, stretch.last);
        if (!first || !last)
        {
            continue;
        }

        for (uint32_t item = *first; item <= *last; item++)
        {
            items.insert(item);
        }
    }

    return items;
}

// Where an edit of `setters` on track `index` of `tracks` reaches besides the track. `runs` gives the items that each
// track ran. `first` is the track's first item where its opening leads, if it has one. Track 0 opens where its sound
// starts, and track starts open the other tracks.
Reach ReachOf(const Setters& setters, std::span<const std::set<uint32_t>> runs, std::span<const song::Track> tracks,
              std::size_t index, std::optional<uint32_t> first, std::span<const OtherSound> others)
{
    // The sound's other tracks that ran the commands.
    Reach reach;
    for (std::size_t i = 0; i < tracks.size(); i++)
    {
        const auto ran = [&runs, i](uint32_t item)
        {
            return runs[i].contains(item);
        };
        if (i != index && std::any_of(setters.items.begin(), setters.items.end(), ran))
        {
            reach.tracks.push_back(tracks[i].number);
        }
    }

    // The other sounds whose commands reach them, or the track's opening where a new command would go.
    const int number = tracks[index].number;
    for (const OtherSound& other : others)
    {
        const std::vector<bool>& items = other.reachable->items;
        const auto reached = [&items](uint32_t item)
        {
            return item < items.size() && items[item];
        };
        bool reaches = std::any_of(setters.items.begin(), setters.items.end(), reached);
        if (!reaches && setters.start && first && number == 0)
        {
            reaches = other.start == *first;
        }
        else if (!reaches && setters.start && first)
        {
            reaches = other.reachable->openings.contains({number, *first});
        }
        if (reaches)
        {
            reach.sounds.push_back(other.sound);
        }
    }

    return reach;
}

} // namespace

std::vector<TrackReach> ReachOfTracks(const format::File& file, std::span<const std::optional<SoundPlace>> places,
                                      uint32_t sound, const song::Sequence& read)
{
    const ntdlib::commands::Sequence& commands = SequenceToEdit(file, SequenceOf(places, sound));
    const std::vector<uint32_t> offsets = Offsets(commands, ConsoleOf(file));
    const std::vector<OtherSound> others = OthersOf(file, places, sound, commands);

    // The items that each track of the sound ran.
    std::vector<std::set<uint32_t>> runs;
    for (const song::Track& track : read.tracks)
    {
        runs.push_back(ItemsRun(offsets, track.stretches));
    }

    // What an edit of each setting of each track reaches.
    std::vector<TrackReach> reach(read.tracks.size());
    for (std::size_t i = 0; i < read.tracks.size(); i++)
    {
        const song::Track& track = read.tracks[i];
        const bool opens =
            !track.stretches.empty() && track.stretches.front().entry == ntdlib::sequence_export::Entry::kOpen;
        const std::optional<uint32_t> first =
            opens ? ItemAtOffset(offsets, track.stretches.front().first) : std::nullopt;
        try
        {
            for (std::size_t setting = 0; setting < reach[i].mix.size(); setting++)
            {
                const auto mix = static_cast<song::Mix>(setting);
                const Setters setters = MixSetters(commands, offsets, track.stretches, mix);
                reach[i].mix[setting] = ReachOf(setters, runs, read.tracks, i, first, others);
            }
            if (!track.notes.empty())
            {
                const Program instrument{track.notes.front().bank_slot, static_cast<int>(track.notes.front().program)};
                const Setters setters = InstrumentSetters(commands, offsets, track.stretches, instrument);
                reach[i].instrument = ReachOf(setters, runs, read.tracks, i, first, others);
            }
        }
        catch (const std::invalid_argument&)
        {
            reach[i] = TrackReach{};
        }
    }

    return reach;
}

std::vector<std::vector<uint32_t>> SoundsReaching(const format::File& file,
                                                  std::span<const std::optional<SoundPlace>> places, uint32_t sound,
                                                  std::span<const uint32_t> commands)
{
    const ntdlib::commands::Sequence& sequence = SequenceToEdit(file, SequenceOf(places, sound));
    const std::vector<uint32_t> offsets = Offsets(sequence, ConsoleOf(file));
    const std::vector<OtherSound> others = OthersOf(file, places, sound, sequence);

    std::vector<std::vector<uint32_t>> reaching;
    for (const uint32_t command : commands)
    {
        std::vector<uint32_t>& sounds = reaching.emplace_back();
        const std::optional<uint32_t> item = ItemAtOffset(offsets, command);
        for (const OtherSound& other : others)
        {
            const std::vector<bool>& items = other.reachable->items;
            if (item && *item < items.size() && items[*item])
            {
                sounds.push_back(other.sound);
            }
        }
    }

    return reaching;
}

} // namespace hokuto::edit
