// SPDX-License-Identifier: MIT

// The comparisons of hokuto_check_tracks: the notes of two reads of a sound, track by track.

#include "check_tracks.h"
#include "loop_passes.h"

namespace hokuto::tools::tracks
{

namespace
{

// The time of a frame of DSE's driver.
constexpr double kDseFrameSeconds = ntdlib::dse::kTickMs / 1000.0;

// Whether the driver of `sound`'s console shares its channels or voices among the tracks: on the GBA, on the DS, on DSE
// and on Nintendo R&D2's driver.
bool SharesChannels(const song::Sequence& sound)
{
    return sound.gba || g_ds || g_dse || g_rd2;
}

} // namespace

uint32_t FrameTicks(const song::Sequence& before, const song::Sequence& after)
{
    if (!g_ds && !g_dse)
    {
        return 0;
    }

    double fastest = 0.0;
    for (const song::Sequence* sound : {&before, &after})
    {
        const double scale = g_dse ? sound->timebase * kDseFrameSeconds / 60.0 : 1.0 / 240.0;
        for (const song::Tempo& tempo : sound->tempo)
        {
            fastest = std::max(fastest, tempo.bpm * scale);
        }
    }

    return static_cast<uint32_t>(std::max(1.0, std::ceil(fastest)));
}

namespace
{

// Whether two lengths are `frame` ticks apart or less.
bool Within(uint32_t length, uint32_t other, uint32_t frame)
{
    return std::max(length, other) - std::min(length, other) <= frame;
}

// The lengths in the comparisons of a note that a later note cut, and of one that the driver never started, on the GBA
// and the DS. A cut note ends where a later note took its channel while the driver held the note (song::Note::cut). A
// note that never started lasts the length of its command, and the same note lasts until its sound stops once it
// starts. The driver's channels decide both lengths, and the comparisons leave them out.
constexpr uint32_t kCutLength = std::numeric_limits<uint32_t>::max();
constexpr uint32_t kUnstartedLength = kCutLength - 1;

} // namespace

std::vector<song::Note> CommandNotes(const song::Sequence& sound, const song::Track& track)
{
    if (!SharesChannels(sound))
    {
        return track.notes;
    }

    std::vector<song::Note> notes = track.notes;
    for (song::Note note : track.unstarted)
    {
        note.program = kNoProgram;
        notes.push_back(note);
    }
    std::ranges::stable_sort(notes, {},
                             [](const song::Note& note) { return std::tuple(note.start, note.key, note.velocity); });

    return notes;
}

namespace
{

// The length of `note` of CommandNotes in the comparisons: on the GBA and the DS, kCutLength for a note that a later
// note cut, and kUnstartedLength for one that the driver never started.
uint32_t LengthOf(const song::Sequence& sound, const song::Note& note)
{
    return !SharesChannels(sound)                       ? note.length
           : g_any_length || note.program == kNoProgram ? kUnstartedLength
           : note.cut                                   ? kCutLength
                                                        : note.length;
}

// Whether the length of a note of NotesOf counts in the comparisons.
bool LengthCounts(uint32_t length)
{
    return length != kCutLength && length != kUnstartedLength;
}

// How the notes of `before` and `after` pair up by `same`. On the GBA, a note that the driver never started
// (kUnstartedLength) can stand alone. ntdlib's performer keeps such a note, and leaves out a note that started and
// stayed silent. A copy of a track can then hold a quiet note that never started where the original's note started
// and stayed silent. Such lists hold their notes in the order of their starts, keys and velocities (CommandNotes).
// The earlier of two notes that don't pair can't pair with a later note of the other list.
template <typename Same>
Pairing Pair(const std::vector<TrackNote>& before, const std::vector<TrackNote>& after, Same same)
{
    const auto order = [](const TrackNote& note)
    {
        return std::tuple(std::get<0>(note), std::get<1>(note), std::get<3>(note));
    };
    const auto unstarted = [](const TrackNote& note)
    {
        return std::get<2>(note) == kUnstartedLength;
    };
    Pairing pairing;
    std::size_t i = 0;
    std::size_t j = 0;
    while (i < before.size() || j < after.size())
    {
        if (i < before.size() && j < after.size() && same(before[i], after[j]))
        {
            pairing.pairs.emplace_back(i++, j++);
            continue;
        }

        const bool first = j == after.size() || (i < before.size() && order(before[i]) <= order(after[j]));
        if (first && unstarted(before[i]))
        {
            i++;
        }
        else if (!first && unstarted(after[j]))
        {
            j++;
        }
        else
        {
            pairing.failed = {i, j};
            return pairing;
        }
        ++pairing.alone;
    }

    // Under VoicesDecide, every note can stand alone. A pair of lists with notes then has to pair one note at least.
    if (g_any_length && !before.empty() && !after.empty() && pairing.pairs.empty())
    {
        pairing.failed = {0, 0};
        return pairing;
    }

    g_alone += pairing.alone;
    return pairing;
}

} // namespace

std::vector<TrackNote> NotesOf(const song::Sequence& sound, int number, uint32_t end, uint32_t shortest)
{
    std::vector<TrackNote> notes;
    for (const song::Track& track : sound.tracks)
    {
        if (track.number != number)
        {
            continue;
        }

        for (const song::Note& note : CommandNotes(sound, track))
        {
            if (note.start < end && note.length >= shortest)
            {
                notes.emplace_back(note.start, note.key, LengthOf(sound, note), note.velocity);
            }
        }
    }

    return notes;
}

std::string NoteDifference(const std::vector<TrackNote>& before, const std::vector<TrackNote>& after)
{
    const auto same = [](const TrackNote& first, const TrackNote& second)
    {
        const auto& [start, key, length, velocity] = first;
        const auto& [other_start, other_key, other_length, other_velocity] = second;
        return start == other_start && key == other_key && velocity == other_velocity &&
               (length == other_length || !LengthCounts(length) || !LengthCounts(other_length));
    };
    const std::size_t alone = g_alone;
    const Pairing pairing = Pair(before, after, same);
    g_alone = alone;
    const auto [first, other] = pairing.failed.value_or(std::pair(before.size(), after.size()));

    const auto text = [](const std::vector<TrackNote>& notes, std::size_t index)
    {
        if (index >= notes.size())
        {
            return std::string("none");
        }

        const auto& [start, key, length, velocity] = notes[index];
        const std::string held = length == kCutLength         ? ", cut"
                                 : length == kUnstartedLength ? ", never started"
                                                              : " for " + std::to_string(length);
        return "key " + std::to_string(key) + " at " + std::to_string(start) + held + ", velocity " +
               std::to_string(velocity);
    };

    return std::to_string(before.size()) + " notes before and " + std::to_string(after.size()) + " after, note " +
           std::to_string(first) + " " + text(before, first) + " before and note " + std::to_string(other) + " " +
           text(after, other) + " after";
}

std::set<int> NumbersOf(const song::Sequence& sound)
{
    std::set<int> numbers;
    for (const song::Track& track : sound.tracks)
    {
        numbers.insert(track.number);
    }

    return numbers;
}

std::set<int> ComparedNumbersOf(const song::Sequence& before, const song::Sequence& after, const std::set<int>& numbers)
{
    std::set<int> compared;
    for (const song::Track& track : after.tracks)
    {
        const bool shown =
            before.length == 0 && !numbers.contains(track.number) && track.notes.empty() && track.unstarted.empty();
        if (!shown)
        {
            compared.insert(track.number);
        }
    }

    return compared;
}

song::Sequence WithoutQuietPsgNotes(song::Sequence sound)
{
    constexpr int kQuietLevel = 64 * 127;
    for (song::Track& track : sound.tracks)
    {
        const auto quiet = [&track](const song::Note& note)
        {
            return note.psg != 0 && note.velocity * song::MixAt(track, song::Mix::kVolume, note.start) <= kQuietLevel;
        };
        std::erase_if(track.notes, quiet);
        std::erase_if(track.unstarted, quiet);
    }

    return sound;
}

std::string NumbersText(const std::set<int>& numbers)
{
    std::string text;
    for (const int number : numbers)
    {
        text += (text.empty() ? "" : ", ") + std::to_string(number);
    }

    return text.empty() ? "none" : text;
}

namespace
{

// Whether track `number` of `before` and track `other` of `after` play the same notes before tick `end`: the same
// starts, keys, velocities and lengths. The arrangement ends a note that still sounds where the performer stops at the
// loop's end. A note that lasts to the loop's end in both sounds therefore matches whatever its lengths. So does a note
// that lasts to the earlier loop's end in its sound, beside a note that lasts at least that long in the other. With
// `ends`, so does a note that lasts to the end of a sound without a loop in both. The arrangement ends a held note
// where the performer stops (kHeldTicks in ntdlib's nwsnd/sequence.h). Notes shorter than `shortest` ticks don't count.
// On the GBA and the DS, the length of a note that a later note cut, or that never started, doesn't count either, and a
// note that never started can stand alone (Pair). On the DS, a length can differ by a frame's ticks (FrameTicks).
bool SameTrackNotes(const song::Sequence& before, int number, const song::Sequence& after, int other, uint32_t end,
                    uint32_t shortest = 0, bool ends = false)
{
    const uint32_t frame = FrameTicks(before, after);
    const auto same = [&before, &after, ends, frame](const TrackNote& first, const TrackNote& second)
    {
        const auto& [start, key, length, velocity] = first;
        const auto& [other_start, other_key, other_length, other_velocity] = second;
        const bool both = before.loop && after.loop;
        const uint64_t earlier = both ? std::min(before.loop->end, after.loop->end) : 0;
        const uint64_t until = uint64_t{start} + length;
        const uint64_t other_until = uint64_t{other_start} + other_length;
        const bool looped = both && ((until == before.loop->end && other_until == after.loop->end) ||
                                     (until == earlier && before.loop->end == earlier && other_until >= earlier) ||
                                     (other_until == earlier && after.loop->end == earlier && until >= earlier));
        const bool ended = ends && !before.loop && !after.loop && uint64_t{start} + length == before.length &&
                           uint64_t{other_start} + other_length == after.length;
        const bool held = looped || ended;
        return start == other_start && key == other_key && velocity == other_velocity &&
               (Within(length, other_length, frame) || held || !LengthCounts(length) || !LengthCounts(other_length));
    };

    return !Pair(NotesOf(before, number, end, shortest), NotesOf(after, other, end, shortest), same).failed;
}

} // namespace

Pairing PairNotes(const std::vector<TrackNote>& before, const std::vector<TrackNote>& after, uint32_t end, bool lengths,
                  uint32_t frame)
{
    const auto same = [end, lengths, frame](const TrackNote& first, const TrackNote& second)
    {
        const auto& [start, key, length, velocity] = first;
        const auto& [other_start, other_key, other_length, other_velocity] = second;
        const bool counts = LengthCounts(length) && LengthCounts(other_length);
        const bool ended = uint64_t{start} + length < end || uint64_t{other_start} + other_length < end;
        return start == other_start && key == other_key && velocity == other_velocity &&
               (!lengths || !counts || !ended || Within(length, other_length, frame));
    };

    return Pair(before, after, same);
}

bool SameNotes(const std::vector<TrackNote>& before, const std::vector<TrackNote>& after, uint32_t end, bool lengths,
               uint32_t frame)
{
    return !PairNotes(before, after, end, lengths, frame).failed;
}

Comparison Compare(const song::Sequence& before, const song::Sequence& after, const std::set<int>& numbers,
                   std::optional<int> copy, int original, std::optional<int> empty, uint32_t shortest, bool ends)
{
    Comparison comparison;
    uint32_t end = std::numeric_limits<uint32_t>::max();
    if (after.loop != before.loop)
    {
        const std::optional<uint32_t> earlier = EarlierEnd(before, after);
        if (!earlier)
        {
            comparison.difference =
                "another loop: " + LoopText(before.loop) + " before and " + LoopText(after.loop) + " after";
            return comparison;
        }

        comparison.moved = true;
        end = *earlier;
    }

    if (ComparedNumbersOf(before, after, numbers) != numbers)
    {
        comparison.difference = "tracks " + NumbersText(NumbersOf(after)) + " in place of " + NumbersText(numbers);
        return comparison;
    }
    for (const int number : numbers)
    {
        // A song of Konami's driver keeps all its tracks, and a copy is placed on a track without notes.
        const bool kept = NumbersOf(before).contains(number) && number != copy;
        if (kept && !SameTrackNotes(before, number, after, number, end, shortest, ends))
        {
            comparison.difference = "other notes on track " + std::to_string(number) + ": " +
                                    NoteDifference(NotesOf(before, number, end), NotesOf(after, number, end));
            return comparison;
        }
    }
    if (copy && !SameTrackNotes(before, original, after, *copy, end, shortest))
    {
        comparison.difference = "other notes on the copy, track " + std::to_string(*copy) + ": " +
                                NoteDifference(NotesOf(before, original, end), NotesOf(after, *copy, end));
        return comparison;
    }
    if (empty && !NotesOf(after, *empty, end).empty())
    {
        comparison.difference = "notes on the new track " + std::to_string(*empty);
    }

    return comparison;
}

uint32_t EndOf(const song::Sequence& sound)
{
    return sound.loop ? sound.loop->end : sound.length;
}

void CheckOthers(const Archive& archive, const std::map<uint32_t, song::Sequence>& after,
                 const std::set<uint32_t>& edited, const std::string& round, Totals& totals)
{
    // An edited GBA song of each sequence, and the edited songs of Konami's driver or Nintendo R&D2's driver, by their
    // sequences.
    std::map<uint32_t, uint32_t> headers;
    std::map<uint32_t, std::vector<uint32_t>> driver_songs;
    for (const uint32_t sound : edited)
    {
        if (archive.archive.gba)
        {
            headers.emplace(archive.places[sound]->sequence, sound);
        }
        if (archive.archive.Konami() || archive.archive.Rd2())
        {
            driver_songs[archive.places[sound]->sequence].push_back(sound);
        }
    }

    for (const auto& [sound, read] : after)
    {
        if (edited.contains(sound))
        {
            continue;
        }

        // A song's other numbers play the song with the edit. An edited number of the same stage plays it the same way.
        // The versions of a song of Nintendo R&D2's driver play the song with the edit, on their terms.
        std::optional<uint32_t> alike;
        const auto konami = driver_songs.find(archive.places[sound]->sequence);
        if (konami != driver_songs.end() && archive.archive.Rd2())
        {
            ++totals.shared;
            continue;
        }
        if (konami != driver_songs.end())
        {
            const bool live = song::KonamiSoundIsLive(*archive.archive.Konami(), sound);
            const auto same_stage = [&archive, live](uint32_t other)
            {
                return song::KonamiSoundIsLive(*archive.archive.Konami(), other) == live;
            };
            const auto found = std::ranges::find_if(konami->second, same_stage);
            if (found == konami->second.end())
            {
                ++totals.shared;
                continue;
            }
            alike = *found;
        }

        const auto header = headers.find(archive.places[sound]->sequence);
        if (header != headers.end())
        {
            alike = header->second;
        }
        if (alike)
        {
            ++totals.shared;
            const auto played = after.find(*alike);
            const Comparison comparison = played != after.end()
                                              ? Compare(played->second, read, NumbersOf(played->second))
                                              : Comparison{"no read of the song that it plays", false};
            if (comparison.difference || comparison.moved)
            {
                std::printf("%s: %s plays %s against %s (the sound whose song it plays) after the %s\n",
                            archive.name.c_str(), read.name.c_str(),
                            comparison.difference.value_or("another loop").c_str(),
                            archive.songs.at(*alike).name.c_str(), round.c_str());
                ++totals.problems;
            }
            continue;
        }

        const song::Sequence& before = archive.songs.at(sound);
        ++totals.others;
        const Comparison comparison = Compare(before, read, NumbersOf(before));
        if (comparison.difference || comparison.moved)
        {
            const std::string difference = comparison.difference.value_or(
                "another loop: " + LoopText(before.loop) + " before and " + LoopText(read.loop) + " after");
            std::printf("%s: %s plays %s after the %s of other sounds' tracks\n", archive.name.c_str(),
                        before.name.c_str(), difference.c_str(), round.c_str());
            ++totals.problems;
        }
    }
}

} // namespace hokuto::tools::tracks
