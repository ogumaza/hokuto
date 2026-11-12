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
#include "edit/drivers.h"
#include "edit/engine_edits.h"
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

// Another sound that plays the file: its place in the sound table, the item where its track 0 opens, and what its
// commands reach. Track 0 opens where the sound starts. On the DS, an allocation of tracks there runs before track 0,
// and track 0 opens at the item after it. A song of a GBA ROM starts each track at an item of its own instead, and so
// does a sound of the GBA's other drivers.
struct OtherSound
{
    uint32_t sound = 0;
    uint32_t opening = 0;
    std::shared_ptr<const Reachable> reachable;
    std::vector<uint32_t> track_starts;
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
        throw std::invalid_argument("This sound has no sequence that Hokuto can edit.");
    }

    return places[sound]->sequence;
}

// Sound `other`, whose tracks start at the items `starts` of `commands` and whose list opens the channels of `listed`,
// with what its commands reach (OthersOf).
OtherSound SoundOfTracks(uint32_t other, const ntdlib::commands::Sequence& commands, std::span<const uint32_t> starts,
                         std::span<const std::pair<int, uint32_t>> listed)
{
    Reachable found{std::vector<bool>(commands.items.size()), {}};
    OtherSound song{other, 0, nullptr, {}};
    for (const uint32_t start : starts)
    {
        song.track_starts.push_back(start);
        const std::vector<bool> items = Reached(commands, start, true);
        for (std::size_t item = 0; item < items.size() && item < found.items.size(); item++)
        {
            found.items[item] = found.items[item] || items[item];
        }
    }

    for (const auto& [channel, entry] : listed)
    {
        found.openings.emplace(channel, entry);
    }

    // A command of Nintendo R&D2's driver can start another track (F8).
    for (uint32_t item = 0; item < found.items.size(); item++)
    {
        const Command& command = commands.items[item];
        if (found.items[item] && command.kind == Kind::kOpenTrack && !command.target_outside)
        {
            found.openings.emplace(command.index, command.target);
        }
    }

    song.reachable = std::make_shared<const Reachable>(std::move(found));

    return song;
}

// The sounds of `file` besides sound `sound` that play its sequence `commands`, and what their commands reach. Sounds
// that start at the same item share what they reach.
std::vector<OtherSound> OthersOf(const format::File& file, std::span<const std::optional<SoundPlace>> places,
                                 uint32_t sound, const ntdlib::commands::Sequence& commands)
{
    const song::Console console = ConsoleOf(file);
    const bool ds = console.engine == format::Engine::kDs;
    std::map<uint32_t, std::shared_ptr<const Reachable>> reachable;
    std::vector<OtherSound> others;
    for (const uint32_t other : SharingFile(places, sound))
    {
        // A GBA song's tracks start at the fields of its header. A sound of another of the GBA's drivers starts its
        // tracks at the fields of the song table, or at the entries of the table of tracks at the start of its
        // sequence. No command opens them.
        std::vector<uint32_t> starts;
        for (const uint32_t field : places[other]->track_fields)
        {
            starts.push_back(file.native->fields.at(field).to.offset);
        }

        if (console.driver != nullptr && !places[other]->track_entries.empty())
        {
            starts = EditsOf(*console.driver).EntryStarts(commands, places[other]->track_entries);
        }

        // A sound of some drivers, such as Brownie Brown's driver, starts at a list of its channels, each a track of
        // the same number.
        std::vector<std::pair<int, uint32_t>> listed;
        if (console.driver != nullptr && places[other]->start_field)
        {
            listed = EditsOf(*console.driver)
                         .ListedStarts(commands, file.native->fields.at(*places[other]->start_field).to.offset);
            for (const auto& [channel, entry] : listed)
            {
                starts.push_back(entry);
            }
        }
        if (!places[other]->track_fields.empty() || !places[other]->track_entries.empty() || !listed.empty())
        {
            others.push_back(SoundOfTracks(other, commands, starts, listed));
            continue;
        }

        const std::optional<uint32_t>& field = places[other]->start_field;
        const uint32_t start = field ? file.native->fields.at(*field).to.offset : 0;
        std::shared_ptr<const Reachable>& found = reachable[start];
        if (!found)
        {
            found = std::make_shared<const Reachable>(ReachableFrom(commands, start));
        }

        const bool allocates =
            ds && start < commands.items.size() && commands.items[start].kind == Kind::kAllocateTracks;
        others.push_back({other, allocates ? start + 1 : start, found, {}});
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
// starts, and track starts open the other tracks. On the GBA, the fields of a song's header open its tracks. Where a
// track that the command of a track starts gets the settings of the starting track, `started` contains the tracks that
// the commands of each track started, and is empty otherwise.
Reach ReachOf(const Setters& setters, std::span<const std::set<uint32_t>> runs, std::span<const song::Track> tracks,
              std::size_t index, std::optional<uint32_t> first, std::span<const OtherSound> others,
              std::span<const std::set<int>> started)
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

    // A track that the edited track or such a track starts gets their settings, and so on.
    if (!started.empty() && (setters.start || !setters.items.empty()))
    {
        std::set<int> taking(reach.tracks.begin(), reach.tracks.end());
        taking.insert(tracks[index].number);
        for (bool grew = true; grew;)
        {
            grew = false;
            for (std::size_t i = 0; i < tracks.size(); i++)
            {
                if (!taking.contains(tracks[i].number))
                {
                    continue;
                }

                for (const int number : started[i])
                {
                    grew = taking.insert(number).second || grew;
                }
            }
        }

        for (const int number : taking)
        {
            if (number != tracks[index].number &&
                std::find(reach.tracks.begin(), reach.tracks.end(), number) == reach.tracks.end())
            {
                reach.tracks.push_back(number);
            }
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
        if (!reaches && setters.start && first && !other.track_starts.empty())
        {
            reaches =
                std::find(other.track_starts.begin(), other.track_starts.end(), *first) != other.track_starts.end() ||
                other.reachable->openings.contains({number, *first});
        }
        else if (!reaches && setters.start && first && number == 0)
        {
            reaches = other.opening == *first;
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
    const song::Console console = ConsoleOf(file);
    const std::vector<uint32_t> offsets = Offsets(commands, console);
    const std::vector<OtherSound> others = OthersOf(file, places, sound, commands);

    // The items that each track of the sound ran, and the tracks that its commands started where those take its
    // settings.
    std::vector<std::set<uint32_t>> runs;
    for (const song::Track& track : read.tracks)
    {
        runs.push_back(ItemsRun(offsets, track.stretches));
    }

    std::vector<std::set<int>> started;
    if (EditsOf(console).StartedTracksTakeSettings())
    {
        for (const std::set<uint32_t>& run : runs)
        {
            std::set<int>& numbers = started.emplace_back();
            for (const uint32_t item : run)
            {
                const Command& command = commands.items[item];
                if (command.kind == Kind::kOpenTrack && !command.target_outside)
                {
                    numbers.insert(command.index);
                }
            }
        }
    }

    // What an edit of each setting of each track reaches.
    const DriverEdits* driver_edits = console.driver != nullptr ? &EditsOf(*console.driver) : nullptr;
    const bool notes_set_volume = driver_edits != nullptr && driver_edits->NotesSetVolume();
    std::vector<TrackReach> reach(read.tracks.size());
    for (std::size_t i = 0; i < read.tracks.size(); i++)
    {
        const song::Track& track = read.tracks[i];
        const bool opens =
            !track.stretches.empty() && track.stretches.front().entry == ntdlib::sequence_export::Entry::kOpen;
        const std::optional<uint32_t> first =
            opens ? ItemAtOffset(offsets, track.stretches.front().first) : std::nullopt;

        // On some drivers, such as Konami's driver, a note of the track before this one can play on this track.
        std::span<const song::Stretch> previous;
        if (driver_edits != nullptr && driver_edits->NotesPlayOnNextTrack())
        {
            const auto before = std::ranges::find(read.tracks, track.number - 1, &song::Track::number);
            previous = before != read.tracks.end() ? std::span<const song::Stretch>(before->stretches) : previous;
        }

        try
        {
            for (std::size_t setting = 0; setting < reach[i].mix.size(); setting++)
            {
                const auto mix = static_cast<song::Mix>(setting);
                const Setters setters = MixSetters(commands, offsets, song::MixStretches(read, track), mix,
                                                   console.engine == format::Engine::kDse, notes_set_volume, previous);
                reach[i].mix[setting] = ReachOf(setters, runs, read.tracks, i, first, others, started);

                // On DSE, the other tracks of the channel show the settings (song::Track::channel).
                for (const song::Track& other : read.tracks)
                {
                    std::vector<int>& listed = reach[i].mix[setting].tracks;
                    if (track.channel >= 0 && other.channel == track.channel && other.number != track.number &&
                        std::find(listed.begin(), listed.end(), other.number) == listed.end())
                    {
                        listed.push_back(other.number);
                    }
                }
            }

            if (!track.notes.empty())
            {
                // The walk leaves DSE's slots out (InstrumentSetters). A DSE track runs its commands alone.
                const Program instrument{console.engine == format::Engine::kDse ? 0 : track.notes.front().bank_slot,
                                         static_cast<int>(track.notes.front().program)};
                const Setters setters =
                    InstrumentSetters(commands, offsets, track.stretches, instrument, console, previous, track.number);
                reach[i].instrument = ReachOf(setters, runs, read.tracks, i, first, others, started);
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
