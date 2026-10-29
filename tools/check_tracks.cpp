// SPDX-License-Identifier: MIT

// Checks the edits of a sound's tracks (src/edit/tracks.h) on a sound archive of the Wii, the 3DS, the Wii U or the
// Switch. The arrangement's Project menu makes these edits. The tool imports the archive into a Hokuto file the way the
// program does, and ntdlib's performer reads each sequence sound. Up to four sounds of each sequence file then take
// part in four rounds, spread over the sounds that play the file. Each round makes its edits in one version of the
// file. ntdlib's performer then reads every sequence sound again from the exported archive.
//
// - Each sound gains a new track, with the instrument of its first note. The new track has to run without notes, and
//   the sound's other tracks have to play their notes as before. A sound that holds until the game stops it is
//   counted apart: the performer ends it 10 s after its last change, and the new track's end is a change. A note then
//   goes into the new track of the first such sound of each sequence file, at the loop's start or at the start, with
//   the key of the sound's first note. The sound has to play it there, and its other notes as before. The arrangement
//   ends a held note where the performer stops, and the new note's release can move that place. A note that lasts to
//   the end of a sound without a loop therefore matches whatever its length.
// - Each sound gains a copy of its track with the most notes. The copy has to play that track's notes, and the sound's
//   other tracks have to play theirs as before. A track whose commands draw random numbers, use variables, run the
//   game's code or run only if a comparison held is counted apart. Its copy draws, compares and changes them again.
// - Each sound loses its last track with notes, other than track 0. The sound's other tracks have to start their
//   notes as before, with their keys and velocities, up to the earlier end. A track that ran longer than the others or
//   looped decided where the sound ends or loops, and the last notes then last otherwise. A track that sets the tempo,
//   the timebase, the sound's volume or a variable, draws random numbers or runs the game's code is counted apart. The
//   others can play otherwise without it.
// - Each sound takes a paste of the track with the most notes of another sound: the next sound taking part that plays
//   another sequence file. The pasted track has to play that track's notes at the same ticks, with the same banks and
//   programs, up to the earlier end. The sound's other tracks have to play their notes as before, up to the earlier
//   end. A track whose commands draw random numbers, use variables, run the game's code or run only if a comparison
//   held is counted apart. So is a track that its sound starts more than once. The paste plays it once. So is a track
//   with a note of length 0: its wave sets how long the note lasts, and its ticks then follow the tempo. A sound of the
//   Wii plays one bank, and refuses a paste from a sound of another bank.
//
// A note of a tick can end in the frame where it starts, and the arrangement then leaves it out. A frame of the 3DS
// lasts 4.89 ms, and a tick can be shorter. A copy or a paste plays on another track, and the frames can show such a
// note there and hide it on the original track. The tool counts apart the copies and pastes that differ only in notes
// shorter than 2 ticks.
//
// The performer marks the loop on the first pass that the next pass repeats. A new track or a copy can change the state
// that it compares, and the loop can then start a whole number of passes earlier or later. The notes then have to match
// up to the earlier loop's end. A note that lasts to the loop's end in both reads matches whatever its length. The
// arrangement ends a note there when it still sounds where the performer stops. In each round, every sound that the
// round doesn't edit has to play as before: its tracks, its notes and its loop. The edits refuse some sounds and
// tracks, such as a sound whose commands can't move, a sound without a free track and a track that opens other tracks.
// The tool counts the refusals by their messages. It prints a line for each problem, and a summary. It returns 1 when a
// sound plays otherwise, or when an edit, a read or the export fails. With a sound's name, only that sound takes part.
//
// With --model, the file goes into the model form before the edits (format::InModelForm), and the archive that the
// model builds has to be the game's. Each file that the edits make then has to save in both forms and open again as it
// was (tools/checked_archive.h).
//
// usage: hokuto_check_tracks [--model] <archive> [sound]

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sequence_export/performance.h"
#include "xsflib/console.h"
#include "xsflib/files.h"

#include "checked_archive.h"
#include "edit/bytecode.h"
#include "edit/notes.h"
#include "edit/replace.h"
#include "edit/tracks.h"
#include "format/file.h"
#include "song/sequence.h"

namespace
{

namespace edit = hokuto::edit;
namespace sar = ntdlib::sar;
namespace song = hokuto::song;
using hokuto::format::File;
using hokuto::tools::CheckedArchive;
using ntdlib::commands::Command;
using ntdlib::commands::Kind;
using ntdlib::commands::Source;

// The sounds of a sequence file that take part in the rounds.
constexpr std::size_t kMostSounds = 4;

// The velocity of the note that goes into a new track.
constexpr int kVelocity = 100;

// What the check counted, for the summary.
struct Totals
{
    std::size_t sequences = 0;
    std::size_t sounds = 0;
    std::size_t unreadable = 0;
    std::size_t taking_part = 0;
    std::size_t added = 0;
    std::size_t added_holding = 0;
    std::size_t added_differing = 0;
    std::size_t notes = 0;
    std::size_t copied = 0;
    std::size_t copied_apart = 0;
    std::size_t copied_differing = 0;
    std::size_t removed = 0;
    std::size_t removed_apart = 0;
    std::size_t removed_differing = 0;
    std::size_t removed_ending = 0;
    std::size_t pasted = 0;
    std::size_t pasted_apart = 0;
    std::size_t pasted_differing = 0;
    std::size_t pasted_late = 0;
    std::size_t pasted_slots = 0;
    std::size_t pasted_ending = 0;
    std::size_t others = 0;
    std::size_t moved_loops = 0;
    std::size_t tick_notes = 0;
    std::size_t problems = 0;
    std::map<std::string, std::size_t> refusals;
};

// What the tool reads of the archive: its sounds, where each plays in the file, and each sequence sound as ntdlib's
// performer read it.
struct Archive
{
    std::string name;
    CheckedArchive archive;
    File file;
    std::vector<std::optional<edit::SoundPlace>> places;
    std::map<uint32_t, song::Sequence> songs;
};

// A note of a track: its start, key, length and velocity.
using TrackNote = std::tuple<uint32_t, uint8_t, uint32_t, uint8_t>;

// The fewest ticks that a note lasts in the comparisons that let a copy or a paste differ in notes of a tick.
constexpr uint32_t kTickNotes = 2;

// The notes of the track numbered `number` of `sound` that start before tick `end` and last `shortest` ticks or more,
// in their order.
std::vector<TrackNote> NotesOf(const song::Sequence& sound, int number,
                               uint32_t end = std::numeric_limits<uint32_t>::max(), uint32_t shortest = 0)
{
    std::vector<TrackNote> notes;
    for (const song::Track& track : sound.tracks)
    {
        if (track.number != number)
        {
            continue;
        }

        for (const song::Note& note : track.notes)
        {
            if (note.start < end && note.length >= shortest)
            {
                notes.emplace_back(note.start, note.key, note.length, note.velocity);
            }
        }
    }

    return notes;
}

// How `after` differs from `before`, two lists of notes: their counts, and the first note that differs.
std::string NoteDifference(const std::vector<TrackNote>& before, const std::vector<TrackNote>& after)
{
    std::size_t first = 0;
    while (first < before.size() && first < after.size() && before[first] == after[first])
    {
        first++;
    }

    const auto text = [](const std::vector<TrackNote>& notes, std::size_t index)
    {
        if (index >= notes.size())
        {
            return std::string("none");
        }

        const auto& [start, key, length, velocity] = notes[index];
        return "key " + std::to_string(key) + " at " + std::to_string(start) + " for " + std::to_string(length) +
               ", velocity " + std::to_string(velocity);
    };

    return std::to_string(before.size()) + " notes before and " + std::to_string(after.size()) + " after, note " +
           std::to_string(first) + " " + text(before, first) + " before and " + text(after, first) + " after";
}

// The numbers of the tracks of `sound`.
std::set<int> NumbersOf(const song::Sequence& sound)
{
    std::set<int> numbers;
    for (const song::Track& track : sound.tracks)
    {
        numbers.insert(track.number);
    }

    return numbers;
}

// How a sound plays after an edit, against how it played before: what differs, or nothing, and whether the loop moved a
// whole number of passes.
struct Comparison
{
    std::optional<std::string> difference;
    bool moved = false;
};

// The loop from `start` to `end`, or "none".
std::string LoopText(const std::optional<song::Loop>& loop)
{
    return loop ? std::to_string(loop->start) + " to " + std::to_string(loop->end) : std::string("none");
}

// The numbers of `numbers`, such as "0, 1 and 3".
std::string NumbersText(const std::set<int>& numbers)
{
    std::string text;
    for (const int number : numbers)
    {
        text += (text.empty() ? "" : ", ") + std::to_string(number);
    }

    return text.empty() ? "none" : text;
}

// Whether track `number` of `before` and track `other` of `after` play the same notes before tick `end`: the same
// starts, keys, velocities and lengths. The arrangement ends a note that still sounds where the performer stops at the
// loop's end. A note that lasts to the loop's end in both sounds therefore matches whatever its lengths. With `ends`,
// so does a note that lasts to the end of a sound without a loop in both. The arrangement ends a held note where the
// performer stops (kHeldTicks in ntdlib's nwsnd/sequence.h). Notes shorter than `shortest` ticks don't count.
bool SameTrackNotes(const song::Sequence& before, int number, const song::Sequence& after, int other, uint32_t end,
                    uint32_t shortest = 0, bool ends = false)
{
    const auto same = [&before, &after, ends](const TrackNote& first, const TrackNote& second)
    {
        const auto& [start, key, length, velocity] = first;
        const auto& [other_start, other_key, other_length, other_velocity] = second;
        const bool looped = before.loop && after.loop && uint64_t{start} + length == before.loop->end &&
                            uint64_t{other_start} + other_length == after.loop->end;
        const bool ended = ends && !before.loop && !after.loop && uint64_t{start} + length == before.length &&
                           uint64_t{other_start} + other_length == after.length;
        const bool held = looped || ended;
        return start == other_start && key == other_key && velocity == other_velocity &&
               (length == other_length || held);
    };

    return std::ranges::equal(NotesOf(before, number, end, shortest), NotesOf(after, other, end, shortest), same);
}

// How `after` compares with `before`. `after` has to play the tracks `numbers`, and each track of `before` among them
// with its notes. Track `copy` has to play the notes of track `original` of `before`, and a new track `empty` no notes.
// ntdlib's performer marks the loop on the first pass that the next pass repeats. An edit can change the state that it
// compares, and the loop can then start a whole number of passes earlier or later. The notes then have to match up to
// the earlier loop's end (SameTrackNotes). The comparison gives the first difference that it finds. Notes shorter than
// `shortest` ticks don't count. With `ends`, a note that lasts to the end of a sound without a loop in both matches
// whatever its lengths.
Comparison Compare(const song::Sequence& before, const song::Sequence& after, const std::set<int>& numbers,
                   std::optional<int> copy = std::nullopt, int original = 0, std::optional<int> empty = std::nullopt,
                   uint32_t shortest = 0, bool ends = false)
{
    Comparison comparison;
    uint32_t end = std::numeric_limits<uint32_t>::max();
    if (after.loop != before.loop)
    {
        const bool both = before.loop && after.loop;
        const uint32_t period = both ? before.loop->end - before.loop->start : 0;
        const uint32_t shift = !both                                    ? 0
                               : after.loop->start > before.loop->start ? after.loop->start - before.loop->start
                                                                        : before.loop->start - after.loop->start;
        if (!both || period == 0 || after.loop->end - after.loop->start != period || shift % period != 0)
        {
            comparison.difference =
                "another loop: " + LoopText(before.loop) + " before and " + LoopText(after.loop) + " after";
            return comparison;
        }

        comparison.moved = true;
        end = std::min(before.loop->end, after.loop->end);
    }

    if (NumbersOf(after) != numbers)
    {
        comparison.difference = "tracks " + NumbersText(NumbersOf(after)) + " in place of " + NumbersText(numbers);
        return comparison;
    }
    for (const int number : numbers)
    {
        const bool kept = NumbersOf(before).contains(number);
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

// The start of the sound at `place` in `file`: its item in its sequence.
uint32_t StartOf(const File& file, const edit::SoundPlace& place)
{
    return place.start_field ? file.native->fields.at(*place.start_field).to.offset : 0;
}

// The items that track `track` of a sound that starts at item `start` of `sequence` runs.
std::vector<bool> ItemsOfTrack(const ntdlib::commands::Sequence& sequence, uint32_t start, int track)
{
    if (track == 0)
    {
        return edit::Reached(sequence, start, false);
    }

    std::vector<bool> items(sequence.items.size(), false);
    const std::vector<bool> reached = edit::Reached(sequence, start, true);
    for (uint32_t item = 0; item < sequence.items.size(); item++)
    {
        const Command& command = sequence.items[item];
        if (!reached[item] || command.kind != Kind::kOpenTrack || command.index != track || command.target_outside)
        {
            continue;
        }

        const std::vector<bool> runs = edit::Reached(sequence, command.target, false);
        for (std::size_t i = 0; i < items.size(); i++)
        {
            items[i] = items[i] || runs[i];
        }
    }

    return items;
}

// Whether a command draws a random number, uses a variable, runs the game's code or runs only if a comparison held. A
// copy of a track with such a command can play otherwise than the track.
bool Chancy(const Command& command)
{
    const bool variables = command.kind >= Kind::kSetVariable && command.kind <= Kind::kCompareNotEqual;
    const bool argument =
        command.value.source != Source::kValue || (command.time && command.time->source != Source::kValue);
    return variables || argument || command.conditional || command.kind == Kind::kUserProcedure;
}

// Whether a command changes what other tracks play: the tempo, the timebase, the sound's volume, a variable, the random
// numbers that the others draw next, or the game's code.
bool Shared(const Command& command)
{
    const bool writes = command.kind >= Kind::kSetVariable && command.kind <= Kind::kModuloVariable;
    const bool draws =
        command.value.source == Source::kRandom || (command.time && command.time->source == Source::kRandom);
    return writes || draws || command.kind == Kind::kTempo || command.kind == Kind::kTimebase ||
           command.kind == Kind::kMainVolume || command.kind == Kind::kUserProcedure;
}

// The tick at which the arrangement of `sound` ends: the loop's end, or the sound's length.
uint32_t EndOf(const song::Sequence& sound)
{
    return sound.loop ? sound.loop->end : sound.length;
}

// The start, key and velocity of each note of track `number` of `sound` that starts before tick `end`.
std::vector<std::tuple<uint32_t, uint8_t, uint8_t>> StartsOf(const song::Sequence& sound, int number, uint32_t end)
{
    std::vector<std::tuple<uint32_t, uint8_t, uint8_t>> starts;
    for (const auto& [start, key, length, velocity] : NotesOf(sound, number, end))
    {
        starts.emplace_back(start, key, velocity);
    }

    return starts;
}

// Whether a command among `items` of `sequence` meets `test`.
template <typename Test>
bool Any(const ntdlib::commands::Sequence& sequence, const std::vector<bool>& items, Test test)
{
    for (std::size_t item = 0; item < items.size(); item++)
    {
        if (items[item] && test(sequence.items[item]))
        {
            return true;
        }
    }

    return false;
}

// The track of `sound` with the most notes, the lowest first. Nothing for a sound without notes.
std::optional<int> BusiestTrack(const song::Sequence& sound)
{
    std::optional<int> busiest;
    std::size_t most = 0;
    for (const song::Track& track : sound.tracks)
    {
        if (track.notes.size() > most)
        {
            most = track.notes.size();
            busiest = track.number;
        }
    }

    return busiest;
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

// The first note of `sound`: the first of its first track with notes. Nothing for a sound without notes.
std::optional<song::Note> FirstNote(const song::Sequence& sound)
{
    for (const song::Track& track : sound.tracks)
    {
        if (!track.notes.empty())
        {
            return track.notes.front();
        }
    }

    return std::nullopt;
}

// Every sequence sound of `version` read by ntdlib's performer, by its index. A sound whose read fails is missing, and
// counts as a problem.
std::map<uint32_t, song::Sequence> ReadAll(const Archive& archive, const File& version, const std::string& round,
                                           Totals& totals)
{
    std::map<uint32_t, song::Sequence> songs;
    CheckedArchive built;
    try
    {
        hokuto::tools::CheckSaved(version);
        built = hokuto::tools::BuildChecked(version, archive.name);
    }
    catch (const std::exception& error)
    {
        std::printf("%s: the file after the %s can't be exported: %s\n", archive.name.c_str(), round.c_str(),
                    error.what());
        ++totals.problems;
        return songs;
    }

    for (const auto& [sound, before] : archive.songs)
    {
        try
        {
            songs.emplace(sound, built.Read(sound));
        }
        catch (const std::exception& error)
        {
            std::printf("%s: %s can't be read after the %s: %s\n", archive.name.c_str(), before.name.c_str(),
                        round.c_str(), error.what());
            ++totals.problems;
        }
    }

    return songs;
}

// Checks that every sound of `after` that the round didn't edit plays as before.
void CheckOthers(const Archive& archive, const std::map<uint32_t, song::Sequence>& after,
                 const std::set<uint32_t>& edited, const std::string& round, Totals& totals)
{
    for (const auto& [sound, read] : after)
    {
        if (edited.contains(sound))
        {
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

// The first round: a new track for each sound of `chosen`, then a note in the new track of the first such sound of each
// sequence.
void AddTracks(const Archive& archive, const std::vector<uint32_t>& chosen, Totals& totals)
{
    File version = archive.file;
    std::map<uint32_t, int> added;
    std::map<uint32_t, std::optional<song::Note>> firsts;
    for (const uint32_t sound : chosen)
    {
        const song::Sequence& before = archive.songs.at(sound);
        const std::optional<song::Note> first = FirstNote(before);
        const std::optional<edit::Program> instrument =
            first ? std::optional<edit::Program>(edit::Program{first->bank_slot, static_cast<int>(first->program)})
                  : std::nullopt;
        try
        {
            edit::TrackEdit edited = edit::AddTrack(version, archive.places, sound, before, instrument);
            version = std::move(edited.file);
            added.emplace(sound, edited.track);
            firsts.emplace(sound, first);
            ++totals.added;
        }
        catch (const std::invalid_argument& error)
        {
            ++totals.refusals[std::string("new track: ") + error.what()];
        }
    }

    // The sounds with new tracks play their notes as before, and the others play as before.
    const std::map<uint32_t, song::Sequence> after = ReadAll(archive, version, "new tracks", totals);
    std::set<uint32_t> edited;
    for (const auto& [sound, track] : added)
    {
        edited.insert(sound);
        const auto read = after.find(sound);
        if (read == after.end())
        {
            continue;
        }

        const song::Sequence& before = archive.songs.at(sound);
        std::set<int> numbers = NumbersOf(before);
        if (numbers.contains(track))
        {
            std::printf("%s: %s gains track %d but plays it already\n", archive.name.c_str(), before.name.c_str(),
                        track);
            ++totals.problems;
            continue;
        }

        numbers.insert(track);
        const Comparison comparison = Compare(before, read->second, numbers, std::nullopt, 0, track);
        const std::optional<std::string>& difference = comparison.difference;
        totals.moved_loops += comparison.moved ? 1 : 0;
        if (before.holds)
        {
            ++totals.added_holding;
            totals.added_differing += difference ? 1 : 0;
        }
        else if (difference)
        {
            std::printf("%s: %s plays %s with a new track %d\n", archive.name.c_str(), before.name.c_str(),
                        difference->c_str(), track);
            ++totals.problems;
        }
    }
    CheckOthers(archive, after, edited, "new tracks", totals);

    // A note in the new track of the first sound of each sequence, at the loop's start or at the start.
    File noted = version;
    std::set<uint32_t> sequences;
    std::map<uint32_t, std::tuple<int, uint32_t, uint8_t>> notes;
    for (const uint32_t sound : chosen)
    {
        const auto track = added.find(sound);
        const auto read = after.find(sound);
        const std::optional<song::Note>& first = firsts[sound];
        const uint32_t sequence = archive.places[sound]->sequence;
        if (track == added.end() || read == after.end() || !first || sequences.contains(sequence))
        {
            continue;
        }

        const auto numbered = [&track](const song::Track& candidate)
        {
            return candidate.number == track->second;
        };
        const auto played = std::find_if(read->second.tracks.begin(), read->second.tracks.end(), numbered);
        const song::Sequence& original = archive.songs.at(sound);
        const uint32_t tick = original.loop ? original.loop->start : 0;
        const std::optional<song::Stretch> stretch =
            played != read->second.tracks.end() ? edit::StretchAt(played->stretches, tick) : std::nullopt;
        if (!stretch)
        {
            std::printf("%s: %s's new track %d has no stretch at tick %u\n", archive.name.c_str(),
                        read->second.name.c_str(), track->second, tick);
            ++totals.problems;
            continue;
        }

        const edit::NewNote note{*stretch, tick, first->key, kVelocity, std::max<uint32_t>(first->length, 1)};
        try
        {
            noted = edit::EditNotes(noted, sequence, {}, {&note, 1}).file;
            sequences.insert(sequence);
            notes.emplace(sound, std::tuple<int, uint32_t, uint8_t>{track->second, tick, first->key});
        }
        catch (const std::exception& error)
        {
            std::printf("%s: a note can't go into %s's new track %d: %s\n", archive.name.c_str(),
                        read->second.name.c_str(), track->second, error.what());
            ++totals.problems;
        }
    }

    // Each sound plays its new note, and its other notes as before.
    const std::map<uint32_t, song::Sequence> played = ReadAll(archive, noted, "new notes", totals);
    for (const auto& [sound, note] : notes)
    {
        const auto read = played.find(sound);
        if (read == played.end())
        {
            continue;
        }

        // The note plays at its tick. In a loop, it plays in each pass that the arrangement shows.
        const auto& [track, tick, key] = note;
        const std::vector<TrackNote> heard = NotesOf(read->second, track);
        const auto same = [key](const TrackNote& other)
        {
            return std::get<1>(other) == key && std::get<3>(other) == kVelocity;
        };
        const bool plays = !heard.empty() && std::get<0>(heard[0]) == tick && std::ranges::all_of(heard, same);
        song::Sequence others = read->second;
        for (song::Track& other : others.tracks)
        {
            if (other.number == track)
            {
                other.notes.clear();
            }
        }
        const Comparison comparison =
            Compare(after.at(sound), others, NumbersOf(after.at(sound)), std::nullopt, 0, std::nullopt, 0, true);
        const std::optional<std::string>& difference = comparison.difference;
        totals.moved_loops += comparison.moved ? 1 : 0;
        if (!plays || difference)
        {
            std::printf("%s: %s doesn't play its new note on track %d at tick %u alone: %zu notes there%s%s\n",
                        archive.name.c_str(), read->second.name.c_str(), track, tick, heard.size(),
                        difference ? ", and " : "", difference ? difference->c_str() : "");
            ++totals.problems;
        }
        ++totals.notes;
    }
}

// The second round: a copy of the track with the most notes of each sound of `chosen`.
void CopyTracks(const Archive& archive, const std::vector<uint32_t>& chosen, Totals& totals)
{
    File version = archive.file;
    std::map<uint32_t, std::pair<int, int>> copies;
    std::set<uint32_t> apart;
    for (const uint32_t sound : chosen)
    {
        const song::Sequence& before = archive.songs.at(sound);
        const std::optional<int> track = BusiestTrack(before);
        if (!track)
        {
            continue;
        }

        const edit::SoundPlace& place = *archive.places[sound];
        const ntdlib::commands::Sequence& sequence = *archive.file.sequences[place.sequence];
        try
        {
            edit::TrackEdit edited = edit::DuplicateTrack(version, archive.places, sound, *track);
            version = std::move(edited.file);
            copies.emplace(sound, std::pair<int, int>{*track, edited.track});
            if (Any(sequence, ItemsOfTrack(sequence, StartOf(archive.file, place), *track), Chancy))
            {
                apart.insert(sound);
            }
            ++totals.copied;
        }
        catch (const std::invalid_argument& error)
        {
            ++totals.refusals[std::string("copy: ") + error.what()];
        }
    }

    const std::map<uint32_t, song::Sequence> after = ReadAll(archive, version, "copies", totals);
    std::set<uint32_t> edited;
    for (const auto& [sound, tracks] : copies)
    {
        edited.insert(sound);
        const auto read = after.find(sound);
        if (read == after.end())
        {
            continue;
        }

        const auto& [original, copy] = tracks;
        const song::Sequence& before = archive.songs.at(sound);
        std::set<int> numbers = NumbersOf(before);
        numbers.insert(copy);
        const Comparison comparison = Compare(before, read->second, numbers, copy, original);
        const std::optional<std::string>& difference = comparison.difference;
        totals.moved_loops += comparison.moved ? 1 : 0;
        if (apart.contains(sound))
        {
            ++totals.copied_apart;
            totals.copied_differing += difference ? 1 : 0;
        }
        else if (difference &&
                 !Compare(before, read->second, numbers, copy, original, std::nullopt, kTickNotes).difference)
        {
            ++totals.tick_notes;
        }
        else if (difference)
        {
            std::printf("%s: %s plays %s with a copy of track %d\n", archive.name.c_str(), before.name.c_str(),
                        difference->c_str(), original);
            ++totals.problems;
        }
    }
    CheckOthers(archive, after, edited, "copies", totals);
}

// The third round: the removal of the last track with notes, other than track 0, of each sound of `chosen`.
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

        const edit::SoundPlace& place = *archive.places[sound];
        const ntdlib::commands::Sequence& sequence = *archive.file.sequences[place.sequence];
        try
        {
            version = edit::RemoveTrack(version, archive.places, sound, *track);
            removed.emplace(sound, *track);
            if (Any(sequence, ItemsOfTrack(sequence, StartOf(archive.file, place), *track), Shared))
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
        const song::Sequence& before = archive.songs.at(sound);
        std::set<int> numbers = NumbersOf(before);
        numbers.erase(track);
        std::optional<std::string> difference;
        if (NumbersOf(read->second) != numbers)
        {
            difference = "tracks " + NumbersText(NumbersOf(read->second)) + " in place of " + NumbersText(numbers);
        }
        const uint32_t end = std::min(EndOf(before), EndOf(read->second));
        for (const int number : numbers)
        {
            if (!difference && StartsOf(read->second, number, end) != StartsOf(before, number, end))
            {
                difference = "other notes on track " + std::to_string(number) + ": " +
                             NoteDifference(NotesOf(before, number, end), NotesOf(read->second, number, end));
            }
        }
        totals.removed_ending += read->second.loop != before.loop || read->second.length != before.length ? 1 : 0;
        if (apart.contains(sound))
        {
            ++totals.removed_apart;
            totals.removed_differing += difference ? 1 : 0;
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

// A paste of a track into a sound: the sound that the track came from, its number there, its number in the sound, and
// whether the tool counts it apart.
struct Paste
{
    uint32_t from = 0;
    int original = 0;
    int track = 0;
    bool apart = false;
};

// The sound of `chosen` that gives sound `chosen[index]` a track: the next one that plays another sequence file, or
// else the next one. Nothing when `chosen` holds no other sound. A sound of the Wii plays one bank, and takes a track
// only from a sound of the same bank: the next one that plays another sequence file first.
std::optional<uint32_t> SourceOf(const Archive& archive, const std::vector<uint32_t>& chosen, std::size_t index)
{
    const uint32_t sound = chosen[index];
    const uint32_t sequence = archive.places[sound]->sequence;

    const auto takes = [&archive, sound](uint32_t other)
    {
        return !archive.archive.wii || archive.archive.BanksOf(other) == archive.archive.BanksOf(sound);
    };
    for (const bool same_file : {false, true})
    {
        for (std::size_t step = 1; step < chosen.size(); step++)
        {
            const uint32_t other = chosen[(index + step) % chosen.size()];
            if ((archive.places[other]->sequence == sequence) == same_file && takes(other))
            {
                return other;
            }
        }
    }

    return chosen.size() > 1 ? std::optional<uint32_t>(chosen[(index + 1) % chosen.size()]) : std::nullopt;
}

// The bank of each note of track `number` of `sound` that starts before tick `end` and lasts `shortest` ticks or more,
// from the sound's slots `banks`, with the note's program.
std::vector<std::pair<uint32_t, uint32_t>> InstrumentsOf(const song::Sequence& sound, int number, uint32_t end,
                                                         const std::vector<uint32_t>& banks, uint32_t shortest = 0)
{
    std::vector<std::pair<uint32_t, uint32_t>> instruments;
    for (const song::Track& track : sound.tracks)
    {
        for (const song::Note& note : track.notes)
        {
            if (track.number == number && note.start < end && note.length >= shortest)
            {
                const uint32_t bank = note.bank_slot < banks.size() ? banks[note.bank_slot] : sar::kNoItem;
                instruments.emplace_back(bank, note.program);
            }
        }
    }

    return instruments;
}

// Whether two lists of notes that start before tick `end` match: their starts, keys and velocities, and with `lengths`
// the lengths of the notes that end before `end`. A note that lasts to the end of the performance lasts as long as the
// sound.
bool SameNotes(const std::vector<TrackNote>& before, const std::vector<TrackNote>& after, uint32_t end, bool lengths)
{
    const auto same = [end, lengths](const TrackNote& first, const TrackNote& second)
    {
        const auto& [start, key, length, velocity] = first;
        const auto& [other_start, other_key, other_length, other_velocity] = second;
        const bool ended = uint64_t{start} + length < end || uint64_t{other_start} + other_length < end;
        return start == other_start && key == other_key && velocity == other_velocity &&
               (!lengths || !ended || length == other_length);
    };

    return std::ranges::equal(before, after, same);
}

// `sound` with its notes' times, its length and its loop in ticks of a quarter note of `to` ticks.
song::Sequence InTicksOf(song::Sequence sound, int to)
{
    const int from = sound.timebase > 0 ? sound.timebase : to;
    const auto scale = [from, to](uint32_t& ticks)
    {
        ticks = static_cast<uint32_t>(uint64_t{ticks} * static_cast<uint64_t>(to) / static_cast<uint64_t>(from));
    };
    for (song::Track& track : sound.tracks)
    {
        for (song::Note& note : track.notes)
        {
            scale(note.start);
            scale(note.length);
        }
    }
    scale(sound.length);
    if (sound.loop)
    {
        scale(sound.loop->start);
        scale(sound.loop->end);
    }
    sound.timebase = to;

    return sound;
}

// Whether two sounds each keep one tempo, the same. The engine sets some lengths, such as a one-shot wave's. Such a
// length lasts other ticks at another tempo.
bool SameTempo(const song::Sequence& first, const song::Sequence& second)
{
    const auto bpm = [](const song::Sequence& sound)
    {
        return sound.tempo.empty() ? 120.0 : sound.tempo.front().bpm;
    };
    const auto steady = [&bpm](const song::Sequence& sound)
    {
        const auto same = [&bpm, &sound](const song::Tempo& tempo)
        {
            return tempo.bpm == bpm(sound);
        };
        return std::ranges::all_of(sound.tempo, same);
    };

    return steady(first) && steady(second) && bpm(first) == bpm(second);
}

// The fourth round: each sound of `chosen` takes a paste of the track with the most notes of another sound.
void PasteTracks(const Archive& archive, const std::vector<uint32_t>& chosen, Totals& totals)
{
    File version = archive.file;
    std::map<uint32_t, Paste> pastes;
    for (std::size_t index = 0; index < chosen.size(); index++)
    {
        const uint32_t sound = chosen[index];
        const std::optional<uint32_t> from = SourceOf(archive, chosen, index);
        const std::optional<int> track = from ? BusiestTrack(archive.songs.at(*from)) : std::nullopt;
        if (!track)
        {
            continue;
        }

        // A track whose commands draw random numbers, use variables, run the game's code or run only if a comparison
        // held is counted apart. So is a track that its sound starts more than once: the paste plays it once. So is a
        // track with a note of length 0. Its wave sets how long the note lasts, and with the note wait, when the track
        // goes on. Its ticks then follow the tempo.
        const song::Sequence& source = archive.songs.at(*from);
        const auto numbered = [&track](const song::Track& candidate)
        {
            return candidate.number == *track;
        };
        const song::Track& played = *std::ranges::find_if(source.tracks, numbered);
        const auto opens = [](const song::Stretch& stretch)
        {
            return stretch.entry == ntdlib::sequence_export::Entry::kOpen;
        };
        try
        {
            const edit::CopiedTrack copied = edit::CopyTrack(archive.file, archive.places, *from, source, *track);
            edit::TrackEdit edited = edit::PasteTrack(version, archive.places, sound, archive.songs.at(sound), copied);
            const bool slots =
                edited.file.native->tables->sounds[sound].banks != version.native->tables->sounds[sound].banks;
            version = std::move(edited.file);
            const auto waits = [](const Command& command)
            {
                return command.kind == Kind::kNote && command.value.value == 0;
            };
            const bool apart = std::ranges::any_of(copied.commands, Chancy) ||
                               std::ranges::count_if(played.stretches, opens) > 1 ||
                               std::ranges::any_of(copied.commands, waits);
            pastes.emplace(sound, Paste{*from, *track, edited.track, apart});
            totals.pasted_late += copied.tick > 0 ? 1 : 0;
            totals.pasted_slots += slots ? 1 : 0;
            ++totals.pasted;
        }
        catch (const std::invalid_argument& error)
        {
            ++totals.refusals[std::string("paste: ") + error.what()];
        }
    }

    const std::map<uint32_t, song::Sequence> after = ReadAll(archive, version, "pastes", totals);
    std::set<uint32_t> edited;
    for (const auto& [sound, paste] : pastes)
    {
        edited.insert(sound);
        const auto read = after.find(sound);
        if (read == after.end())
        {
            continue;
        }

        // The sound plays its tracks as before, up to the earlier end. A track that ran longer than the others or
        // looped sets where the sound ends or loops, and the pasted track can. A note that lasts to the end of the
        // performance then lasts otherwise.
        const song::Sequence& before = archive.songs.at(sound);
        const song::Sequence& source = archive.songs.at(paste.from);
        std::set<int> numbers = NumbersOf(before);
        numbers.insert(paste.track);
        std::optional<std::string> difference;
        if (NumbersOf(read->second) != numbers)
        {
            difference = "tracks " + NumbersText(NumbersOf(read->second)) + " in place of " + NumbersText(numbers);
        }
        const uint32_t end = std::min(EndOf(before), EndOf(read->second));
        for (const int number : NumbersOf(before))
        {
            if (!difference && !SameNotes(NotesOf(before, number, end), NotesOf(read->second, number, end), end, true))
            {
                difference = "other notes on track " + std::to_string(number) + ": " +
                             NoteDifference(NotesOf(before, number, end), NotesOf(read->second, number, end));
            }
        }

        // The pasted track plays the notes of the other sound's track in the sound's ticks, up to the earlier end, with
        // its banks and programs. Its lengths count where both sounds keep one tempo, the same, and count the same
        // ticks to a quarter note. The engine sets some lengths, such as a one-shot wave's, in frames.
        const song::Sequence counted = InTicksOf(source, before.timebase);
        const uint32_t shared = std::min(EndOf(counted), EndOf(read->second));
        const std::vector<TrackNote> original = NotesOf(counted, paste.original, shared);
        const std::vector<TrackNote> copy = NotesOf(read->second, paste.track, shared);
        const bool lengths = SameTempo(source, before) && source.timebase == before.timebase;
        bool ticks = false;
        if (!difference && !SameNotes(original, copy, shared, lengths))
        {
            ticks = SameNotes(NotesOf(counted, paste.original, shared, kTickNotes),
                              NotesOf(read->second, paste.track, shared, kTickNotes), shared, lengths);
            difference = "other notes on the pasted track " + std::to_string(paste.track) + " from " + source.name +
                         "'s track " + std::to_string(paste.original) + ": " + NoteDifference(original, copy);
        }
        const std::vector<uint32_t>& banks = version.native->tables->sounds[sound].banks;
        const std::vector<uint32_t>& source_banks = archive.file.native->tables->sounds[paste.from].banks;
        if (!difference && InstrumentsOf(read->second, paste.track, shared, banks) !=
                               InstrumentsOf(counted, paste.original, shared, source_banks))
        {
            difference = "other banks or programs on the pasted track " + std::to_string(paste.track);
        }

        totals.pasted_ending += read->second.loop != before.loop || read->second.length != before.length ? 1 : 0;
        if (paste.apart)
        {
            ++totals.pasted_apart;
            totals.pasted_differing += difference ? 1 : 0;
        }
        else if (ticks && InstrumentsOf(read->second, paste.track, shared, banks, kTickNotes) ==
                              InstrumentsOf(counted, paste.original, shared, source_banks, kTickNotes))
        {
            ++totals.tick_notes;
        }
        else if (difference)
        {
            std::printf("%s: %s plays %s with a paste of %s's track %d\n", archive.name.c_str(), before.name.c_str(),
                        difference->c_str(), source.name.c_str(), paste.original);
            ++totals.problems;
        }
    }
    CheckOthers(archive, after, edited, "pastes", totals);
}

} // namespace

int main(int argc, char** argv)
{
    std::vector<std::string> arguments = xsflib::console::Arguments(argc, argv);
    hokuto::tools::TakeModelOption(arguments);
    if (arguments.size() < 2 || arguments.size() > 3)
    {
        std::fprintf(stderr, "usage: hokuto_check_tracks [--model] <archive> [sound]\n");
        return 2;
    }
    const std::string only = arguments.size() > 2 ? arguments[2] : std::string();

    // The archive and the group files beside it make a Hokuto file.
    const std::filesystem::path path = xsflib::PathFromUtf8(arguments[1]);
    Archive archive;
    archive.name = xsflib::Utf8(path.filename());
    try
    {
        std::vector<uint8_t> bytes;
        xsflib::ReadFile(path, bytes);
        archive.archive = hokuto::tools::OpenChecked(path, bytes);
        archive.file = hokuto::tools::CheckedForm(hokuto::tools::ImportChecked(bytes, archive.archive), bytes);
        archive.places = hokuto::tools::PlacesOfChecked(archive.file, bytes, archive.archive);
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "%s: %s\n", archive.name.c_str(), error.what());
        return 2;
    }

    // The sequence sounds with a place, by their sequence.
    Totals totals;
    std::map<uint32_t, std::vector<uint32_t>> by_sequence;
    for (uint32_t sound = 0; sound < archive.places.size(); sound++)
    {
        if (!archive.places[sound])
        {
            continue;
        }

        try
        {
            archive.songs.emplace(sound, archive.archive.Read(sound));
            by_sequence[archive.places[sound]->sequence].push_back(sound);
            ++totals.sounds;
        }
        catch (const std::exception&)
        {
            ++totals.unreadable;
        }
    }

    // Up to four sounds of each sequence, spread over the sounds that play it, or the named sound.
    std::vector<uint32_t> chosen;
    for (const auto& [sequence, sounds] : by_sequence)
    {
        ++totals.sequences;
        const std::size_t count = std::min(sounds.size(), kMostSounds);
        for (std::size_t i = 0; i < count; i++)
        {
            const uint32_t sound = sounds[i * sounds.size() / count];
            if (only.empty() || archive.archive.NameOf(sound) == only)
            {
                chosen.push_back(sound);
            }
        }
    }
    if (!only.empty() && chosen.empty())
    {
        for (const auto& [sound, read] : archive.songs)
        {
            if (read.name == only)
            {
                chosen.push_back(sound);
            }
        }
    }
    totals.taking_part = chosen.size();

    AddTracks(archive, chosen, totals);
    CopyTracks(archive, chosen, totals);
    RemoveTracks(archive, chosen, totals);
    PasteTracks(archive, chosen, totals);

    for (const auto& [message, count] : totals.refusals)
    {
        std::printf("%s: refused %zu times: %s\n", archive.name.c_str(), count, message.c_str());
    }
    hokuto::tools::PrintSaves(archive.name);
    std::printf(
        "%s: %zu sequences, %zu sounds read, %zu unreadable, %zu taking part. %zu new tracks, %zu of them in sounds "
        "that hold, %zu of those playing otherwise, and %zu new notes in them. %zu copies, %zu of them counted "
        "apart, %zu of those playing otherwise. %zu removals, %zu of them counted apart, %zu of those playing "
        "otherwise, and %zu sounds ending or looping otherwise. %zu pastes, %zu of them counted apart, %zu of those "
        "playing otherwise, %zu starting after the sound's start, %zu with banks in other slots, and %zu sounds ending "
        "or looping otherwise. %zu reads of other sounds. %zu loops moved a whole number of passes. %zu copies and "
        "pastes played otherwise only in notes of a tick. %zu problems.\n",
        archive.name.c_str(), totals.sequences, totals.sounds, totals.unreadable, totals.taking_part, totals.added,
        totals.added_holding, totals.added_differing, totals.notes, totals.copied, totals.copied_apart,
        totals.copied_differing, totals.removed, totals.removed_apart, totals.removed_differing, totals.removed_ending,
        totals.pasted, totals.pasted_apart, totals.pasted_differing, totals.pasted_late, totals.pasted_slots,
        totals.pasted_ending, totals.others, totals.moved_loops, totals.tick_notes, totals.problems);

    return totals.problems == 0 ? 0 : 1;
}
