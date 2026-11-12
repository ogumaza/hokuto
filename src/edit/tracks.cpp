// SPDX-License-Identifier: MIT

// The track edits of edit/tracks.h, with the rules of NintendoWare and of the DS. The rules of the other engines are in
// gba_tracks.cpp, dse_tracks.cpp, konami_tracks.cpp and rd2_tracks.cpp.

#include "edit/tracks.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/commands/units.h"

#include "edit/bytecode.h"
#include "edit/converter.h"
#include "edit/drivers.h"
#include "edit/engine_tracks.h"
#include "edit/timelines.h"
#include "edit/track_common.h"
#include "edit/tracks_parts.h"

namespace hokuto::edit
{
namespace
{

using ntdlib::commands::Command;
using ntdlib::commands::Kind;
using ntdlib::commands::kOne;
using ntdlib::commands::Label;
using ntdlib::commands::Sequence;
using ntdlib::commands::Source;
using track_detail::CommandOf;
using track_detail::kTracks;
using track_detail::SoundStart;
using track_detail::StartOf;
using tracks_parts::AllocationOf;
using tracks_parts::CheckRoom;
using tracks_parts::FreeTrack;
using tracks_parts::Insertion;
using tracks_parts::StartsOfTrack;
using tracks_parts::TrackLabel;
using tracks_parts::TrackStartPlace;
using tracks_parts::WithNewTrack;

// The tracks that an allocation at item `start` of `sequence` allocates, a bit each, or nothing without one. ntdlib's
// performer allocates them besides the tracks of the sound's entry.
uint32_t AllocatedAt(const Sequence& sequence, uint32_t start)
{
    if (start >= sequence.items.size())
    {
        return 0;
    }

    const Command& first = sequence.items[start];
    const bool decided = first.kind == Kind::kAllocateTracks && first.value.source == Source::kValue;
    return decided ? static_cast<uint32_t>(first.value.value / kOne) : 0;
}

// The tracks that the entry of sound `sound` of `tables` allocates, a bit each.
uint32_t FlagsOf(const format::NativeTables& tables, uint32_t sound)
{
    return sound < tables.sounds.size() ? tables.sounds[sound].tracks : 0;
}

// "track 2", "tracks 2 and 3" or "tracks 2, 3 and 5", for the tracks of `tracks`, a bit each.
std::string TracksNamed(uint32_t tracks)
{
    std::vector<int> numbers;
    for (int track = 0; track < kTracks; track++)
    {
        if ((tracks >> track & 1) != 0)
        {
            numbers.push_back(track);
        }
    }

    std::string named = numbers.size() == 1 ? "track " : "tracks ";
    for (std::size_t i = 0; i < numbers.size(); i++)
    {
        if (i > 0)
        {
            named += i + 1 == numbers.size() ? " and " : ", ";
        }

        named += std::to_string(numbers[i]);
    }

    return named;
}

// Whether a command of `kind` ends the commands at the start of a track that run before any time passes.
bool EndsStart(Kind kind)
{
    switch (kind)
    {
    case Kind::kNote:
    case Kind::kWait:
    case Kind::kJump:
    case Kind::kCall:
    case Kind::kReturn:
    case Kind::kLoopStart:
    case Kind::kLoopEnd:
    case Kind::kEnd:
    case Kind::kData:
        return true;
    default:
        return false;
    }
}

// `sequence` with `insertions`. Their targets name items of `sequence`. `starts` takes the new index of each item and
// of the end for the labels and the sounds' starts. A label of `staying` stays at its item where an insertion takes the
// item's start.
Sequence Inserted(const Sequence& sequence, std::vector<Insertion> insertions, std::vector<uint32_t>& starts,
                  std::span<const std::string> staying = {})
{
    const auto by_place = [](const Insertion& a, const Insertion& b)
    {
        return a.before < b.before;
    };
    std::stable_sort(insertions.begin(), insertions.end(), by_place);

    // Each item goes after the commands in front of it.
    const auto count = static_cast<uint32_t>(sequence.items.size());
    std::vector<uint32_t> items(count + 1);
    starts.assign(count + 1, 0);
    Sequence result;
    std::size_t next = 0;
    for (uint32_t item = 0; item <= count; item++)
    {
        const auto first = static_cast<uint32_t>(result.items.size());
        bool takes = false;
        for (; next < insertions.size() && insertions[next].before == item; next++)
        {
            takes = takes || insertions[next].takes_start;
            result.items.push_back(insertions[next].command);
        }

        items[item] = static_cast<uint32_t>(result.items.size());
        starts[item] = takes ? first : items[item];
        if (item < count)
        {
            result.items.push_back(sequence.items[item]);
        }
    }

    // The targets keep their items, and the labels go with the starts.
    for (Command& command : result.items)
    {
        if (ntdlib::commands::HasTarget(command.kind) && !command.target_outside)
        {
            command.target = items.at(command.target);
        }
    }

    for (const Label& label : sequence.labels)
    {
        const bool stays = std::ranges::find(staying, label.name) != staying.end();
        result.labels.push_back({label.name, stays ? items.at(label.item) : starts.at(label.item)});
    }

    return result;
}

// `sequence` with `track` after its items, and `labels` at the new items, counted from the first. A target in `track`
// counts from its first item too. A label at the end of `sequence` names the new end. A name that the labels hold
// already takes a number after it.
Sequence WithTrack(const Sequence& sequence, std::span<const Command> track, std::span<const Label> labels)
{
    const auto count = static_cast<uint32_t>(sequence.items.size());
    Sequence result = sequence;
    for (Label& label : result.labels)
    {
        if (label.item == count)
        {
            label.item += static_cast<uint32_t>(track.size());
        }
    }

    for (Command command : track)
    {
        if (ntdlib::commands::HasTarget(command.kind) && !command.target_outside)
        {
            command.target += count;
        }

        result.items.push_back(std::move(command));
    }

    for (const Label& label : labels)
    {
        result.labels.push_back({UniqueLabel(result.labels, label.name), label.item + count});
    }

    return result;
}

// The part of label `name` that names a place in a track: what follows the last Track_, a number and _ in it. A track's
// label gives Start, and a name without a track gives itself. A number takes Label in front of it: a name of Track_,
// two numbers and _ between them names a subroutine in a timeline.
std::string PlaceInTrack(const std::string& name)
{
    std::string place = name;
    for (std::size_t at = name.find("Track_"); at != std::string::npos; at = name.find("Track_", at + 1))
    {
        std::size_t digits = at + 6;
        while (digits < name.size() && name[digits] >= '0' && name[digits] <= '9')
        {
            digits++;
        }
        if (digits > at + 6 && digits == name.size())
        {
            place = "Start";
        }
        else if (digits > at + 6 && name[digits] == '_')
        {
            place = name.substr(digits + 1);
        }
    }

    const auto digit = [](char c)
    {
        return c >= '0' && c <= '9';
    };
    return place.empty() || std::ranges::all_of(place, digit) ? "Label" + place : place;
}

// `sequence`, a timeline, with track `number`. The new track follows the layout of NintendoWare's MIDI converter for a
// song of more tracks. The track's code goes after the code of the tracks of lower numbers. Its first label takes the
// converter's name of the track, and its other labels that name and their places (PlaceInTrack). `track`, `labels` and
// `insertions` follow WithTrack and Inserted. The labels of the song's tracks stay at their items. `starts` takes the
// new index of each item of `sequence` and of the end for the labels and the sounds' starts.
Sequence WithTimelineTrack(const Sequence& sequence, int number, std::span<const Command> track,
                           std::span<const Label> labels, std::vector<Insertion> insertions,
                           std::vector<uint32_t>& starts)
{
    const ConvertedTrackPlace place = NewTrackPlace(sequence, static_cast<uint8_t>(number));
    const auto count = static_cast<uint32_t>(sequence.items.size());
    const auto size = static_cast<uint32_t>(track.size());
    const auto moved = [&place, size](uint32_t item)
    {
        return item < place.before ? item : item + size;
    };

    const auto retarget = [](Command& command, const auto& to)
    {
        if (ntdlib::commands::HasTarget(command.kind) && !command.target_outside)
        {
            command.target = to(command.target);
        }
    };

    // The track's code goes in front of the item that the converter puts after it.
    Sequence placed;
    for (uint32_t item = 0; item <= count; item++)
    {
        if (item == place.before)
        {
            for (Command command : track)
            {
                retarget(command, [&place](uint32_t target) { return target + place.before; });
                placed.items.push_back(std::move(command));
            }
        }

        if (item < count)
        {
            Command command = sequence.items[item];
            retarget(command, moved);
            placed.items.push_back(std::move(command));
        }
    }

    for (const Label& label : sequence.labels)
    {
        placed.labels.push_back({label.name, moved(label.item)});
    }

    for (std::size_t i = 0; i < labels.size(); i++)
    {
        const std::string name = i == 0 ? place.label : place.label + "_" + PlaceInTrack(labels[i].name);
        placed.labels.push_back({UniqueLabel(placed.labels, name), labels[i].item + place.before});
    }

    // The track starts open the track's code where it went.
    for (Insertion& insertion : insertions)
    {
        insertion.before = moved(insertion.before);
        retarget(insertion.command, [&place, count](uint32_t target) { return target - count + place.before; });
    }

    std::vector<uint32_t> placed_starts;
    Sequence result = Inserted(placed, std::move(insertions), placed_starts, place.tracks);
    starts.resize(count + 1);
    for (uint32_t item = 0; item <= count; item++)
    {
        starts[item] = placed_starts.at(moved(item));
    }

    return result;
}

} // namespace

namespace tracks_parts
{

uint32_t AllocationOf(const format::File& file, const Sequence& sequence, uint32_t sound, uint32_t start)
{
    if (ConsoleOf(file).engine == format::Engine::kDs)
    {
        return AllocatedAt(sequence, start) | 1;
    }

    return (file.native->tables ? FlagsOf(*file.native->tables, sound) : 0) | AllocatedAt(sequence, start);
}

std::string TrackLabel(const format::File& file, uint32_t sound, int number)
{
    const std::string track = "Track_" + std::to_string(number);
    return ConsoleOf(file).engine == format::Engine::kDs ? track
                                                         : file.native->tables->sounds[sound].name + "_" + track;
}

std::vector<uint32_t> StartsOfTrack(const Sequence& sequence, uint32_t start, int track)
{
    const std::vector<bool> reached = Reached(sequence, start, true);
    std::vector<uint32_t> starts;
    for (uint32_t item = 0; item < sequence.items.size(); item++)
    {
        const Command& command = sequence.items[item];
        if (reached[item] && command.kind == Kind::kOpenTrack && command.index == track && !command.target_outside)
        {
            starts.push_back(item);
        }
    }

    return starts;
}

void CheckRoom(const format::File& file, const Sequence& sequence, uint32_t sound)
{
    if (ConsoleOf(file).engine != format::Engine::kDs &&
        (!file.native->tables || sound >= file.native->tables->sounds.size()))
    {
        throw std::invalid_argument("Adding a track requires the archive tables, and Hokuto can't read them.");
    }

    if (Fixed(sequence))
    {
        throw std::invalid_argument(
            "The sequence file contains jumps to byte offsets, or bytes that no command reaches. Its commands can't "
            "move to make room for a new track.");
    }
}

int FreeTrack(const format::File& file, std::span<const std::optional<SoundPlace>> places, uint32_t sound,
              std::span<const uint32_t> before)
{
    const SoundStart start = StartOf(file, places, sound);
    const Sequence& sequence = *file.sequences[start.sequence];
    uint32_t used = TracksOpened(sequence, start.item) | 1;
    for (uint32_t other = 0; other < places.size(); other++)
    {
        if (other == sound || !places[other] || places[other]->sequence != start.sequence)
        {
            continue;
        }

        const uint32_t from = StartOf(file, places, other).item;
        const std::vector<bool> reached = Reached(sequence, from, true);
        const auto runs = [&reached](uint32_t item)
        {
            return item < reached.size() && reached[item];
        };
        if (std::any_of(before.begin(), before.end(), runs))
        {
            used |= AllocationOf(file, sequence, other, from);
        }
    }

    for (int track = 1; track < kTracks; track++)
    {
        if ((used >> track & 1) == 0)
        {
            return track;
        }
    }

    throw std::invalid_argument("The sound already uses all 16 tracks.");
}

uint32_t TrackStartPlace(const Sequence& sequence, uint32_t start)
{
    uint32_t place = start;
    for (uint32_t item = start; item < sequence.items.size(); item++)
    {
        const Command& command = sequence.items[item];
        if (EndsStart(command.kind))
        {
            break;
        }

        if (command.kind == Kind::kOpenTrack || command.kind == Kind::kAllocateTracks)
        {
            place = item + 1;
        }
    }

    return place;
}

TrackEdit WithNewTrack(const format::File& file, SoundStart start, uint32_t sound, int number,
                       std::span<const Command> track, std::span<const Label> labels, std::vector<Insertion> insertions)
{
    const bool ds = ConsoleOf(file).engine == format::Engine::kDs;
    Sequence sequence = *file.sequences[start.sequence];
    if (ds && start.item < sequence.items.size() && sequence.items[start.item].kind == Kind::kAllocateTracks)
    {
        sequence.items[start.item].value.value = int64_t{AllocatedAt(sequence, start.item) | 1u << number} * kOne;
    }
    else if (ds)
    {
        insertions.insert(insertions.begin(), {start.item, CommandOf(Kind::kAllocateTracks, 1 | 1 << number), true});
    }

    // The new commands, and where the old items went.
    const auto count = static_cast<uint32_t>(sequence.items.size());
    std::vector<uint32_t> starts;
    Sequence result = IsTimeline(file, start.sequence)
                          ? WithTimelineTrack(sequence, number, track, labels, std::move(insertions), starts)
                          : Inserted(WithTrack(sequence, track, labels), std::move(insertions), starts);
    try
    {
        Offsets(result, ConsoleOf(file));
    }
    catch (const std::exception&)
    {
        throw std::invalid_argument("This console's sequence format can't store the commands of the new track.");
    }

    // The sounds' starts and the end move with their items. The sound's entry allocates the track.
    TrackEdit edit{file, number};
    edit.file.sequences[start.sequence] = std::move(result);
    const auto moved = [&starts, count](uint32_t item)
    {
        return item == count ? starts.back() : starts.at(item);
    };
    MoveItemPlaces(edit.file, start.sequence, moved);
    const auto allocate = [sound, number](format::NativeArchive& native)
    {
        native.tables->sounds[sound].tracks |= uint32_t{1} << number;
    };
    if (!ds)
    {
        edit.file.native.Edit(allocate);
    }

    return edit;
}

} // namespace tracks_parts

namespace track_detail
{

TrackEdit AddArchiveTrack(const format::File& file, std::span<const std::optional<SoundPlace>> places, uint32_t sound,
                          const song::Sequence& song, std::optional<Program> instrument)
{
    const SoundStart start = StartOf(file, places, sound);
    const Sequence& sequence = SequenceToEdit(file, start.sequence);
    CheckRoom(file, sequence, sound);

    // A timeline's track starts with the song's tracks, after the waits of the song's header. Its ticks count from
    // there.
    const std::optional<ConvertedStart> song_start =
        IsTimeline(file, start.sequence) ? SongStartFrom(sequence, start.item) : std::nullopt;
    const uint32_t place = TrackStartPlace(sequence, song_start ? song_start->item : start.item);
    const uint32_t from = song_start ? song_start->tick : 0;
    const int number = FreeTrack(file, places, sound, std::array{place});

    // The track turns the note wait off and selects its instrument.
    const std::string name = TrackLabel(file, sound, number);
    std::vector<Command> track = {CommandOf(Kind::kNoteWait, 0)};
    std::vector<Label> labels = {{name, 0}};
    if (instrument && instrument->slot != 0)
    {
        track.push_back(CommandOf(Kind::kBankSelect, instrument->slot));
    }

    if (instrument)
    {
        track.push_back(CommandOf(Kind::kProgram, instrument->number));
    }

    // It waits to the loop's start and then loops with the sound. Without a loop, it waits to the end of the sound's
    // notes.
    if (song.loop && song.loop->end > song.loop->start)
    {
        if (song.loop->start > from)
        {
            track.push_back(CommandOf(Kind::kWait, song.loop->start - from));
        }

        const auto loop = static_cast<uint32_t>(track.size());
        labels.push_back({name + "_Loop", loop});
        track.push_back(CommandOf(Kind::kWait, song.loop->end - song.loop->start));
        Command jump = CommandOf(Kind::kJump, 0);
        jump.target = loop;
        track.push_back(jump);
    }
    else
    {
        const uint32_t end = song.notes_end > 0 ? song.notes_end : song.length;
        track.push_back(CommandOf(Kind::kWait, end > from ? end - from : 4 * static_cast<uint32_t>(song.timebase)));
        track.push_back(CommandOf(Kind::kEnd, 0));
    }

    // Track 0 opens it where the sound starts, or where a timeline's header starts the song's tracks.
    Command opening = CommandOf(Kind::kOpenTrack, 0);
    opening.index = static_cast<uint8_t>(number);
    opening.target = static_cast<uint32_t>(sequence.items.size());

    return WithNewTrack(file, start, sound, number, track, labels, {{place, opening, place == start.item}});
}

} // namespace track_detail

TrackEdit AddTrack(const format::File& file, std::span<const std::optional<SoundPlace>> places, uint32_t sound,
                   const song::Sequence& song, std::optional<Program> instrument)
{
    return EditsOf(ConsoleOf(file)).AddTrack(file, places, sound, song, instrument);
}

namespace track_detail
{

format::File RemoveArchiveTrack(const format::File& file, std::span<const std::optional<SoundPlace>> places,
                                uint32_t sound, int track)
{
    const SoundStart start = StartOf(file, places, sound);
    const Sequence& sequence = SequenceToEdit(file, start.sequence);
    const bool ds = ConsoleOf(file).engine == format::Engine::kDs;
    if (!ds && (!file.native->tables || sound >= file.native->tables->sounds.size()))
    {
        throw std::invalid_argument(
            "Changing the tracks that the sound allocates requires the archive tables, and Hokuto can't read them.");
    }
    if (track == 0)
    {
        throw std::invalid_argument("Track 0 starts the other tracks of the sound. Can't remove it.");
    }

    // The sound's entry, and an allocation at its start, stop allocating the track. A DS sound has no entry, and keeps
    // track 0.
    const uint32_t bit = track > 0 && track < kTracks ? uint32_t{1} << track : 0;
    const uint32_t flags = ds ? 1 : FlagsOf(*file.native->tables, sound);
    const uint32_t at_start = AllocatedAt(sequence, start.item);
    if (((flags | at_start) & bit) == 0)
    {
        throw std::invalid_argument("The sound doesn't allocate that track.");
    }
    if ((flags & ~bit) == 0)
    {
        throw std::invalid_argument("A sound must keep at least one track.");
    }

    // The tracks that the track opens would stop with it.
    uint32_t opened = 0;
    for (const uint32_t item : StartsOfTrack(sequence, start.item, track))
    {
        const std::vector<bool> reached = Reached(sequence, sequence.items[item].target, false);
        for (uint32_t other = 0; other < sequence.items.size(); other++)
        {
            const Command& command = sequence.items[other];
            if (reached[other] && command.kind == Kind::kOpenTrack && command.index < kTracks)
            {
                opened |= uint32_t{1} << command.index;
            }
        }
    }

    opened &= (flags | at_start) & ~bit;
    if (opened != 0)
    {
        const std::string named = TracksNamed(opened);
        throw std::invalid_argument("Track " + std::to_string(track) + " opens " + named + ". Remove " +
                                    (std::popcount(opened) == 1 ? named : "them") + " first.");
    }

    format::File removed = file;
    const auto release = [sound, bit](format::NativeArchive& native)
    {
        native.tables->sounds[sound].tracks &= ~bit;
    };
    if (!ds)
    {
        removed.native.Edit(release);
    }

    if ((at_start & bit) != 0)
    {
        Sequence changed = sequence;
        changed.items[start.item].value.value = int64_t{at_start & ~bit} * kOne;
        removed.sequences[start.sequence] = std::move(changed);
    }

    return removed;
}

} // namespace track_detail

format::File RemoveTrack(const format::File& file, std::span<const std::optional<SoundPlace>> places, uint32_t sound,
                         int track)
{
    return EditsOf(ConsoleOf(file)).RemoveTrack(file, places, sound, track);
}

} // namespace hokuto::edit
