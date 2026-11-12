// SPDX-License-Identifier: MIT

// The round of hokuto_check_tracks that removes tracks.

#include "check_tracks.h"

namespace hokuto::tools::tracks
{

namespace
{

// Whether a command changes what other tracks play: the tempo, the timebase, the sound's volume, a variable, the random
// numbers that the others draw next, or the game's code.
bool Shared(const Command& command)
{
    const bool writes = command.kind >= Kind::kSetVariable && command.kind <= Kind::kModuloVariable;
    const bool draws =
        command.value.source == Source::kRandom || (command.time && command.time->source == Source::kRandom);
    return writes || draws || DrawsDseNumbers(command) || command.kind == Kind::kTempo ||
           command.kind == Kind::kTimebase || command.kind == Kind::kMainVolume || command.kind == Kind::kUserProcedure;
}

// The last track of `sound` with notes, other than track 0. Nothing for a sound without one.
std::optional<int> LastTrack(const song::Sequence& sound)
{
    std::optional<int> last;
    for (const song::Track& track : sound.tracks)
    {
        if (track.number != 0 && !track.notes.empty())
        {
            last = track.number;
        }
    }

    return last;
}

// The number that track `number` of a sound has once the sound loses track `removed`. The tracks after it move up a
// number on the GBA and on DSE. The other consoles' tracks keep their numbers.
int NumberAfter(const Archive& archive, int number, int removed)
{
    return (archive.archive.gba || archive.archive.dse) && number > removed ? number - 1 : number;
}

// Whether a removed track stays in its sound without notes: on the drivers of Konami, Rare and Quintet, and on Krawall.
// A song of those drivers keeps all its tracks.
bool KeepsRemovedTracks(const Archive& archive)
{
    return archive.archive.Konami() || archive.archive.Rare() || archive.archive.Krawall() ||
           (archive.archive.driver &&
            archive.archive.driver->Rules().DriverEngine() == hokuto::format::Engine::kQuintet);
}

// The most channels of a module of Krawall whose notes keep their silent ends in ntdlib's conversion: the MIDI channels
// other than the drum channel.
constexpr std::size_t kKrawallWholeNotes = 15;

// The tracks of `sound` that play notes.
std::size_t TracksWithNotes(const song::Sequence& sound)
{
    return static_cast<std::size_t>(
        std::ranges::count_if(sound.tracks, [](const song::Track& track) { return !track.notes.empty(); }));
}

// Whether the removal of a track with notes from `before`, a song of Krawall, leaves 15 tracks or fewer with notes in
// `now`. The notes of the other tracks then keep their silent ends, and their silent notes show.
bool KeepsSilentEnds(const Archive& archive, const song::Sequence& before, const song::Sequence& now)
{
    return archive.archive.Krawall() && TracksWithNotes(now) <= kKrawallWholeNotes &&
           TracksWithNotes(before) > TracksWithNotes(now);
}

// Whether `after` contains each note of `before` at its start and key, with its velocity and as long or longer. The
// other notes of `after` stand alone.
bool LongerNotes(std::vector<TrackNote> before, std::vector<TrackNote> after)
{
    std::ranges::sort(before);
    std::ranges::sort(after);
    std::size_t j = 0;
    for (const auto& [start, key, length, velocity] : before)
    {
        while (j < after.size() && std::tie(std::get<0>(after[j]), std::get<1>(after[j])) < std::tie(start, key))
        {
            ++j;
        }
        if (j == after.size() || std::get<0>(after[j]) != start || std::get<1>(after[j]) != key ||
            std::get<3>(after[j]) != velocity || std::get<2>(after[j]) < length)
        {
            return false;
        }
        ++j;
    }

    return true;
}

// Whether another track of `sound` plays on the driver channel of track `number`, on DSE.
bool HasChannelMate(const song::Sequence& sound, int number)
{
    const auto of_number = [number](const song::Track& track)
    {
        return track.number == number;
    };
    const auto track = std::ranges::find_if(sound.tracks, of_number);
    return track != sound.tracks.end() &&
           std::ranges::any_of(sound.tracks, [&track, number](const song::Track& other)
                               { return other.number != number && other.channel == track->channel; });
}

} // namespace

bool KonamiPsgMate(const Archive& archive, int number)
{
    return archive.archive.Konami() && ntdlib::konami::RecordHasPan(archive.archive.Konami()->info.revision) &&
           ntdlib::konami::IsPsgTrack(number);
}

namespace
{

// The MIDI channels that the notes and note-offs among `items` of `sequence` name, on Rare's driver.
std::set<uint8_t> RareNoteChannels(const ntdlib::commands::Sequence& sequence, const std::vector<bool>& items)
{
    std::set<uint8_t> channels;
    for (std::size_t item = 0; item < items.size() && item < sequence.items.size(); item++)
    {
        const Command& command = sequence.items[item];
        if (items[item] && (command.kind == Kind::kNote || command.kind == Kind::kNoteOff))
        {
            channels.insert(command.index);
        }
    }

    return channels;
}

// Whether another track of the sound at `place` in `file` plays notes on a MIDI channel of track `number`'s notes, on
// Rare's driver.
bool HasRareChannelMate(const File& file, const edit::SoundPlace& place, int number)
{
    const ntdlib::commands::Sequence& sequence = *file.sequences[place.sequence];
    const std::set<uint8_t> channels = RareNoteChannels(sequence, ItemsOfTrack(file, place, number));
    for (std::size_t other = 0; other < place.track_fields.size(); other++)
    {
        if (static_cast<int>(other) == number)
        {
            continue;
        }

        const std::set<uint8_t> others = RareNoteChannels(sequence, ItemsOfTrack(file, place, static_cast<int>(other)));
        if (std::ranges::any_of(others, [&channels](uint8_t channel) { return channels.contains(channel); }))
        {
            return true;
        }
    }

    return false;
}

// A tune of Rare's driver that plays the run of a removed track: its track whose items fall within the run, whether the
// track runs only part of the run or more, and the edited tune.
struct RareSharer
{
    int track = 0;
    bool partly = false;
    uint32_t sound = 0;
};

// The tunes that play the runs of the removed tracks of `removed`, a track for each edited tune, besides the edited
// tunes, on Rare's driver.
std::map<uint32_t, RareSharer> RareSharers(const Archive& archive, const std::map<uint32_t, int>& removed)
{
    std::map<uint32_t, RareSharer> sharers;
    if (!archive.archive.Rare())
    {
        return sharers;
    }

    for (const auto& [sound, track] : removed)
    {
        const edit::SoundPlace& place = *archive.places[sound];
        const std::vector<bool> run = ItemsOfTrack(archive.file, place, track);
        for (const auto& entry : archive.songs)
        {
            const uint32_t other = entry.first;
            if (removed.contains(other) || sharers.contains(other) || other >= archive.places.size() ||
                !archive.places[other] || archive.places[other]->sequence != place.sequence)
            {
                continue;
            }

            const edit::SoundPlace& other_place = *archive.places[other];
            for (std::size_t number = 0; number < other_place.track_fields.size(); number++)
            {
                const std::vector<bool> items = ItemsOfTrack(archive.file, other_place, static_cast<int>(number));
                bool overlaps = false;
                for (std::size_t i = 0; i < items.size() && i < run.size() && !overlaps; i++)
                {
                    overlaps = items[i] && run[i];
                }
                if (overlaps)
                {
                    sharers.emplace(other, RareSharer{static_cast<int>(number), items != run, sound});
                    break;
                }
            }
        }
    }

    return sharers;
}

} // namespace

void RemoveTracks(const Archive& archive, const std::vector<uint32_t>& chosen, Totals& totals)
{
    File version = archive.file;
    std::map<uint32_t, int> removed;
    std::set<uint32_t> apart;
    for (const uint32_t sound : chosen)
    {
        const song::Sequence& before = archive.songs.at(sound);
        const std::optional<int> track = LastTrack(before);
        if (!track)
        {
            continue;
        }

        // A removal from a GBA song deletes a field of the native layout. The fields after it move.
        const edit::SoundPlace& place = *archive.places[sound];
        const ntdlib::commands::Sequence& sequence = *archive.file.sequences[place.sequence];
        try
        {
            const std::vector<std::optional<edit::SoundPlace>> places =
                archive.archive.gba ? hokuto::tools::PlacesOfChecked(version, {}, archive.archive) : archive.places;
            version = edit::RemoveTrack(version, places, sound, *track);
            removed.emplace(sound, *track);
            if (Any(sequence, ItemsOfTrack(archive.file, place, *track), Shared) ||
                (archive.archive.dse && HasChannelMate(before, *track)) || KonamiPsgMate(archive, *track) ||
                (archive.archive.Rare() && HasRareChannelMate(archive.file, place, *track)))
            {
                apart.insert(sound);
            }
            ++totals.removed;
        }
        catch (const std::invalid_argument& error)
        {
            ++totals.refusals[std::string("removal: ") + error.what()];
        }
    }

    for (const auto& [sharer, sound] : Sharers(archive, removed))
    {
        removed.emplace(sharer, removed.at(sound));
        if (apart.contains(sound))
        {
            apart.insert(sharer);
        }
        totals.module_songs += archive.archive.Krawall() ? 1 : 0;
    }
    for (const auto& [sharer, shared] : RareSharers(archive, removed))
    {
        removed.emplace(sharer, shared.track);
        if (shared.partly || apart.contains(shared.sound) ||
            HasRareChannelMate(archive.file, *archive.places[sharer], shared.track))
        {
            apart.insert(sharer);
        }
    }
    const std::map<uint32_t, song::Sequence> after = ReadAll(archive, version, "removals", totals);
    std::set<uint32_t> edited;
    for (const auto& [sound, track] : removed)
    {
        edited.insert(sound);
        const auto read = after.find(sound);
        if (read == after.end())
        {
            continue;
        }

        // The other tracks start their notes as before, up to the earlier end. A track that ran longer than the others
        // or looped decided where the sound ends or loops. The last notes then last otherwise.
        const song::Sequence before = ComparableRead(archive, archive.file, sound, archive.songs.at(sound));
        const song::Sequence now = ComparableRead(archive, version, sound, read->second);
        std::set<int> numbers;
        for (const int number : NumbersOf(before))
        {
            // A song of the drivers of Konami, Rare or Quintet, or of Krawall, keeps all its tracks. The removed track
            // stays, without notes.
            if (number != track || KeepsRemovedTracks(archive))
            {
                numbers.insert(NumberAfter(archive, number, track));
            }
        }

        // A GBA song's header can list more tracks than its music player plays. The player leaves the last ones out.
        // The removal then brings the first of them into play as the player's last track.
        const edit::SoundPlace& place = *archive.places[sound];
        const int revealed = static_cast<int>(place.player_tracks) - 1;
        if (archive.archive.gba && place.player_tracks > 0 && place.track_fields.size() > place.player_tracks &&
            !numbers.contains(revealed) && NumbersOf(now).contains(revealed))
        {
            numbers.insert(revealed);
            ++totals.revealed;
        }
        std::optional<std::string> difference;
        if (NumbersOf(now) != numbers)
        {
            difference = "tracks " + NumbersText(NumbersOf(now)) + " in place of " + NumbersText(numbers);
        }
        const uint32_t end = std::min(EndOf(before), EndOf(now));
        const bool silent_ends = KeepsSilentEnds(archive, before, now);
        bool longer = false;
        const auto other_notes = [&]() -> std::optional<std::string>
        {
            for (const int number : NumbersOf(before))
            {
                const int after_number = NumberAfter(archive, number, track);
                if (number != track && !SameNotes(NotesOf(before, number, end), NotesOf(now, after_number, end), end,
                                                  false, FrameTicks(before, now)))
                {
                    if (silent_ends && LongerNotes(NotesOf(before, number, end), NotesOf(now, after_number, end)))
                    {
                        longer = true;
                        continue;
                    }
                    return "other notes on track " + std::to_string(number) + ": " +
                           NoteDifference(NotesOf(before, number, end), NotesOf(now, after_number, end));
                }
            }

            return std::nullopt;
        };
        if (!difference)
        {
            difference = other_notes();
        }
        if (!difference && KeepsRemovedTracks(archive) && !NotesOf(now, track, end).empty())
        {
            difference = "notes on the removed track";
        }
        totals.silent_ends += longer ? 1 : 0;
        totals.removed_ending += now.loop != before.loop || now.length != before.length ? 1 : 0;
        if (apart.contains(sound))
        {
            ++totals.removed_apart;
            totals.removed_differing += difference ? 1 : 0;
        }
        else if (difference && NumbersOf(now) == numbers && VoicesDecide([&] { return !other_notes(); }))
        {
            ++totals.voice_notes;
        }
        else if (difference)
        {
            std::printf("%s: %s plays %s without track %d\n", archive.name.c_str(), before.name.c_str(),
                        difference->c_str(), track);
            ++totals.problems;
        }
    }
    CheckOthers(archive, after, edited, "removals", totals);
}

} // namespace hokuto::tools::tracks
