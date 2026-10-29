// SPDX-License-Identifier: MIT

#include "edit/tracks.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/commands/units.h"
#include "ntdlib/sar/formats.h"

#include "edit/bytecode.h"
#include "edit/converter.h"
#include "edit/timelines.h"

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

// A sequence's tracks are 0 to 15, and a sound's bank slots 0 to 3. A sound of the Wii plays one bank, in slot 0.
constexpr int kTracks = 16;
constexpr int kBankSlots = 4;
constexpr int kWiiBankSlots = 1;

// Where a sound plays: its sequence, and the item where it starts.
struct SoundStart
{
    uint32_t sequence = 0;
    uint32_t item = 0;
};

// Where sound `sound` of `file` plays. A sound whose entry leaves its start out starts at the first item. Throws
// std::invalid_argument for a sound that plays no sequence of the file.
SoundStart StartOf(const format::File& file, std::span<const std::optional<SoundPlace>> places, uint32_t sound)
{
    if (sound >= places.size() || !places[sound])
    {
        throw std::invalid_argument(
            "The sound plays no sequence that Hokuto can edit. Its sequence file holds bytes that no command reaches, "
            "or it isn't a sequence sound.");
    }

    const SoundPlace& place = *places[sound];
    return {place.sequence, place.start_field ? file.native->fields.at(*place.start_field).to.offset : 0};
}

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

// The items of the track starts of track `track` among the commands of a sound that starts at item `start` of
// `sequence`.
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

// Throws std::invalid_argument with a message for the user when a track can't go into `sequence`, sound `sound`'s
// sequence in `file`: the file lacks the archive's tables, or the sequence's commands can't move.
void CheckRoom(const format::File& file, const Sequence& sequence, uint32_t sound)
{
    if (!file.native->tables || sound >= file.native->tables->sounds.size())
    {
        throw std::invalid_argument(
            "Hokuto can't read this archive's tables, and so can't make the sound allocate another track.");
    }
    if (Fixed(sequence))
    {
        throw std::invalid_argument(
            "The sound's sequence file holds targets that are byte offsets, or bytes that no command reaches. Its "
            "commands can't move to make room for a track start.");
    }
}

// With a track start before each item of `before`, the lowest track from 1 that the commands of sound `sound` don't
// open. Another sound that reaches such an item may run the track start. It mustn't allocate the track either. Throws
// std::invalid_argument when every track is taken.
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
            used |= FlagsOf(*file.native->tables, other) | AllocatedAt(sequence, from);
        }
    }

    for (int track = 1; track < kTracks; track++)
    {
        if ((used >> track & 1) == 0)
        {
            return track;
        }
    }

    throw std::invalid_argument("The sound's commands open all 16 tracks. A sound has no more.");
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

// The item before which a new track start goes in the commands of a sound that starts at item `start` of `sequence`:
// after the last track start or allocation among the commands that run there before any time passes. Those stop at a
// note, a wait, a branch and an end. Without a track start or an allocation there, the sound's first command.
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

// A command to put in front of item `before` of a sequence. With `takes_start`, the labels and the sounds' starts at
// the item name the new command. The targets of jumps, calls and track starts keep naming the item.
struct Insertion
{
    uint32_t before = 0;
    Command command;
    bool takes_start = false;
};

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

// A command of `kind` with the whole number `value`.
Command CommandOf(Kind kind, int64_t value)
{
    Command command;
    command.kind = kind;
    command.value.value = value * kOne;

    return command;
}

// The file after `insertions` go into sequence `start.sequence` of `file` once `track` and its `labels` go after the
// sequence's items, and sound `sound` allocates track `number`. An insertion's target counts the new items after the
// sequence's. A timeline takes the track as NintendoWare's MIDI converter lays out a song of more tracks
// (WithTimelineTrack). Throws std::invalid_argument for commands that the console's bytecode can't hold.
TrackEdit WithNewTrack(const format::File& file, SoundStart start, uint32_t sound, int number,
                       std::span<const Command> track, std::span<const Label> labels, std::vector<Insertion> insertions)
{
    // The new commands, and where the old items went.
    const Sequence& sequence = *file.sequences[start.sequence];
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
        throw std::invalid_argument("The console's sequences can't hold the new track's commands.");
    }

    // The sounds' starts and the end move with their items. The sound allocates the track.
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
    edit.file.native.Edit(allocate);

    return edit;
}

// The kinds of command that a copy of a track leaves out: those that act on other tracks, and those that set the whole
// sound's tempo, timebase and volume. In the same sound, the track still sets those. A copy that set them again would
// run after other tracks in a tick, and the last setting wins there.
constexpr std::array kWholeSound = {Kind::kOpenTrack, Kind::kAllocateTracks, Kind::kTempo, Kind::kTimebase,
                                    Kind::kMainVolume};

// The commands that a copy of a track runs: those that `sequence` runs from the items of `starts`, without commands of
// the kinds of `left_out`. `moved` takes the index in the copy of each item that the copy runs or leaves out, and of
// the end. An item that the copy leaves out leads to the item after it.
std::vector<Command> CopyOf(const Sequence& sequence, std::span<const uint32_t> starts,
                            std::map<uint32_t, uint32_t>& moved, std::span<const Kind> left_out)
{
    // The items that the track runs.
    const auto count = static_cast<uint32_t>(sequence.items.size());
    std::vector<bool> runs(count, false);
    for (const uint32_t start : starts)
    {
        const std::vector<bool> reached = Reached(sequence, start, false);
        for (uint32_t item = 0; item < count; item++)
        {
            runs[item] = runs[item] || reached[item];
        }
    }

    // The copy keeps them in their order. An item left out leads to the next one that the copy keeps.
    std::vector<Command> copy;
    std::vector<uint32_t> skipped;
    for (uint32_t item = 0; item < count; item++)
    {
        const Command& command = sequence.items[item];
        if (!runs[item])
        {
            continue;
        }

        if (std::ranges::find(left_out, command.kind) != left_out.end())
        {
            skipped.push_back(item);
            continue;
        }
        for (const uint32_t out : skipped)
        {
            moved[out] = static_cast<uint32_t>(copy.size());
        }
        skipped.clear();
        moved[item] = static_cast<uint32_t>(copy.size());
        copy.push_back(command);
    }
    for (const uint32_t out : skipped)
    {
        moved[out] = static_cast<uint32_t>(copy.size());
    }
    moved[count] = static_cast<uint32_t>(copy.size());

    // The copy's jumps and calls go to its items.
    for (Command& command : copy)
    {
        if (ntdlib::commands::HasTarget(command.kind) && !command.target_outside)
        {
            command.target = moved.at(command.target);
        }
    }

    return copy;
}

// The bank slots that the commands of `commands` that `runs` marks can select: slot 0, where every track starts, and
// the slot of each bank select. Nothing when a variable or a random number selects a slot.
std::optional<std::set<int>> SlotsSelected(std::span<const Command> commands, const std::vector<bool>& runs)
{
    std::set<int> slots = {0};
    for (std::size_t item = 0; item < commands.size(); item++)
    {
        const Command& command = commands[item];
        if (!runs[item] || command.kind != Kind::kBankSelect)
        {
            continue;
        }

        if (command.value.source != Source::kValue)
        {
            return std::nullopt;
        }
        slots.insert(static_cast<int>(command.value.value / kOne));
    }

    return slots;
}

// The bank in slot `slot` of `banks`, or none.
uint32_t BankIn(std::span<const uint32_t> banks, int slot)
{
    const bool held = slot >= 0 && static_cast<std::size_t>(slot) < banks.size();
    return held ? banks[static_cast<std::size_t>(slot)] : ntdlib::sar::kNoItem;
}

// A sound's bank slots after it takes a pasted track, and the slot of the sound that takes each slot that the track
// selects.
struct Slots
{
    std::vector<uint32_t> banks;
    std::map<int, int> moved;
};

// The name of bank `bank` of `tables`, an item ID.
std::string BankNamed(const format::NativeTables& tables, uint32_t bank)
{
    const uint32_t index = bank & 0x00FFFFFF;
    return index < tables.banks.size() ? tables.banks[index] : "a bank";
}

// The slots of sound `sound` of `tables` once it takes a track whose commands select the slots of `wanted`, or any slot
// without it. The sound has `slot_count` slots. `from` gives the banks in the slots of the track's sound. The sound's
// commands select the slots of `taken`, or any slot without it. A slot of the sound that holds the bank of a wanted
// slot takes the wanted slot. Otherwise an empty slot that the sound's commands never select takes the bank: the wanted
// slot itself first. A track whose commands can select any slot keeps its slots. Throws std::invalid_argument with a
// message for the user when the sound's slots can't take the banks.
Slots MapSlots(const format::NativeTables& tables, uint32_t sound, int slot_count,
               const std::optional<std::set<int>>& taken, const std::optional<std::set<int>>& wanted,
               std::span<const uint32_t> from)
{
    Slots slots{tables.sounds[sound].banks, {}};
    const auto free = [&slots, &taken, slot_count](int slot)
    {
        return slot >= 0 && slot < slot_count && BankIn(slots.banks, slot) == ntdlib::sar::kNoItem && taken &&
               !taken->contains(slot);
    };

    const auto take = [&slots](int slot, uint32_t bank)
    {
        slots.banks.resize(std::max(slots.banks.size(), static_cast<std::size_t>(slot) + 1), ntdlib::sar::kNoItem);
        slots.banks[static_cast<std::size_t>(slot)] = bank;
    };

    for (const int slot : wanted.value_or(std::set<int>{0, 1, 2, 3}))
    {
        const uint32_t bank = BankIn(from, slot);
        if (bank == ntdlib::sar::kNoItem || BankIn(slots.banks, slot) == bank)
        {
            slots.moved[slot] = slot;
            continue;
        }

        // A variable or a random number can select any slot. The bank has to keep its slot.
        if (!wanted && !free(slot))
        {
            throw std::invalid_argument(
                "A variable or a random number selects the track's bank slot, and the sound's slot " +
                std::to_string(slot) + " holds another bank than the track's.");
        }
        if (!wanted)
        {
            take(slot, bank);
            slots.moved[slot] = slot;
            continue;
        }

        // A slot that holds the bank, or else an empty slot that the sound never selects.
        std::optional<int> to;
        for (int other = 0; other < slot_count && !to; other++)
        {
            if (BankIn(slots.banks, other) == bank)
            {
                to = other;
            }
        }
        if (!to && free(slot))
        {
            to = slot;
        }
        for (int other = 0; other < slot_count && !to; other++)
        {
            if (free(other))
            {
                to = other;
            }
        }
        if (!to && slot_count == kWiiBankSlots && BankIn(slots.banks, 0) != ntdlib::sar::kNoItem)
        {
            throw std::invalid_argument("The track plays " + BankNamed(tables, bank) + ", and the sound plays " +
                                        BankNamed(tables, BankIn(slots.banks, 0)) +
                                        ". A sound of the Wii plays one bank.");
        }
        if (!to)
        {
            throw std::invalid_argument("The track plays " + BankNamed(tables, bank) +
                                        ", and the sound has no free bank slot for it.");
        }

        take(*to, bank);
        slots.moved[slot] = *to;
    }

    return slots;
}

// `commands` with their times counted in ticks of a quarter note of `to` ticks in place of `from`: the lengths of the
// notes, the waits, and the times over which settings move. A time that changes drops the padding of its bytes. Nothing
// when a time comes from a variable or falls between the new ticks.
std::optional<std::vector<Command>> InTimebase(std::vector<Command> commands, int from, int to)
{
    const auto scale = [from, to](ntdlib::commands::Argument& argument)
    {
        if (argument.source == Source::kVariable)
        {
            return false;
        }

        for (int64_t* value : {&argument.value, &argument.high})
        {
            const int64_t product = *value * to;
            if (product % (int64_t{from} * kOne) != 0)
            {
                return false;
            }
            *value = product / from;
        }

        return true;
    };

    for (Command& command : commands)
    {
        const Command before = command;
        const bool ticks = ntdlib::commands::UnitOf(command.kind) == ntdlib::commands::Unit::kTicks;
        if ((ticks && !scale(command.value)) || (command.time && !scale(*command.time)))
        {
            return std::nullopt;
        }
        if (command.value != before.value || command.time != before.time)
        {
            command.encoding.value_bytes = 0;
        }
    }

    return commands;
}

} // namespace

TrackEdit AddTrack(const format::File& file, std::span<const std::optional<SoundPlace>> places, uint32_t sound,
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
    const std::string name = file.native->tables->sounds[sound].name + "_Track_" + std::to_string(number);
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

TrackEdit DuplicateTrack(const format::File& file, std::span<const std::optional<SoundPlace>> places, uint32_t sound,
                         int track)
{
    const SoundStart start = StartOf(file, places, sound);
    const Sequence& sequence = SequenceToEdit(file, start.sequence);
    CheckRoom(file, sequence, sound);
    const uint32_t allocated = FlagsOf(*file.native->tables, sound) | AllocatedAt(sequence, start.item) | 1;
    if (track < 0 || track >= kTracks || (allocated >> track & 1) == 0)
    {
        throw std::invalid_argument("The sound doesn't play that track.");
    }

    // Track 0 starts where the sound starts. The track starts that the sound reaches open the others. The copy's track
    // starts go in after them.
    std::vector<uint32_t> targets;
    std::vector<Insertion> insertions;
    if (track == 0)
    {
        targets.push_back(start.item);
        const uint32_t place = TrackStartPlace(sequence, start.item);
        insertions.push_back({place, CommandOf(Kind::kOpenTrack, 0), place == start.item});
    }
    else
    {
        for (const uint32_t item : StartsOfTrack(sequence, start.item, track))
        {
            const Command& command = sequence.items[item];
            targets.push_back(command.target);
            Command opening = CommandOf(Kind::kOpenTrack, 0);
            opening.conditional = command.conditional;
            insertions.push_back({item + 1, opening, false});
        }
    }
    if (targets.empty())
    {
        throw std::invalid_argument("The sound's commands don't open that track.");
    }

    // The copy runs the track's commands. Its track starts open it at the copies of the track's starts.
    std::vector<uint32_t> before;
    for (const Insertion& insertion : insertions)
    {
        before.push_back(insertion.before);
    }
    const int number = FreeTrack(file, places, sound, before);
    std::map<uint32_t, uint32_t> moved;
    const std::vector<Command> copy = CopyOf(sequence, targets, moved, kWholeSound);
    const auto count = static_cast<uint32_t>(sequence.items.size());
    for (std::size_t i = 0; i < insertions.size(); i++)
    {
        insertions[i].command.index = static_cast<uint8_t>(number);
        insertions[i].command.target = count + moved.at(targets[i]);
    }

    // The copy's labels: the track's name at its first start, and the others' names with a number after them.
    const std::string name = file.native->tables->sounds[sound].name + "_Track_" + std::to_string(number);
    std::vector<Label> labels = {{name, moved.at(targets.front())}};
    for (const Label& label : sequence.labels)
    {
        const auto found = moved.find(label.item);
        if (found != moved.end() && label.item < count && found->second != labels.front().item)
        {
            labels.push_back({label.name, found->second});
        }
    }

    return WithNewTrack(file, start, sound, number, copy, labels, std::move(insertions));
}

CopiedTrack CopyTrack(const format::File& file, std::span<const std::optional<SoundPlace>> places, uint32_t sound,
                      const song::Sequence& song, int track)
{
    // The sound first started the track where the track's first stretch starts.
    const SoundStart start = StartOf(file, places, sound);
    const Sequence& sequence = SequenceToEdit(file, start.sequence);
    const auto numbered = [track](const song::Track& played)
    {
        return played.number == track;
    };
    const auto played = std::ranges::find_if(song.tracks, numbered);
    if (played == song.tracks.end() || played->stretches.empty())
    {
        throw std::invalid_argument("The sound doesn't play that track.");
    }
    const std::optional<uint32_t> first =
        ItemAtOffset(Offsets(sequence, ConsoleOf(file)), played->stretches.front().first);
    if (!first)
    {
        throw std::invalid_argument("The track starts where the sound's sequence holds no command.");
    }

    // The commands that the track runs from there, and the labels among them.
    CopiedTrack copied;
    std::map<uint32_t, uint32_t> moved;
    copied.commands = CopyOf(sequence, std::array{*first}, moved, kWholeSound);
    const auto outside = [](const Command& command)
    {
        return ntdlib::commands::HasTarget(command.kind) && command.target_outside;
    };
    if (std::ranges::any_of(copied.commands, outside))
    {
        throw std::invalid_argument("The track jumps to a byte offset of its sequence file. Hokuto can't copy it.");
    }
    for (const Label& label : sequence.labels)
    {
        const auto found = moved.find(label.item);
        if (found != moved.end() && label.item < sequence.items.size())
        {
            copied.labels.push_back({label.name, found->second});
        }
    }

    // Where and when the track starts, the sound's quarter note and its banks.
    copied.entry = moved.at(*first);
    copied.tick = played->stretches.front().tick;
    copied.timebase = song.timebase;
    if (file.native->tables && sound < file.native->tables->sounds.size())
    {
        copied.banks = file.native->tables->sounds[sound].banks;
    }
    copied.sound = sound;
    copied.track = track;

    return copied;
}

TrackEdit PasteTrack(const format::File& file, std::span<const std::optional<SoundPlace>> places, uint32_t sound,
                     const song::Sequence& song, const CopiedTrack& copied)
{
    const SoundStart start = StartOf(file, places, sound);
    const Sequence& sequence = SequenceToEdit(file, start.sequence);
    CheckRoom(file, sequence, sound);
    const uint32_t place = TrackStartPlace(sequence, start.item);
    const int number = FreeTrack(file, places, sound, std::array{place});

    // The track's times take the sound's ticks.
    std::optional<std::vector<Command>> commands = copied.commands;
    int64_t tick = copied.tick;
    if (song.timebase != copied.timebase)
    {
        const bool counted = song.timebase > 0 && copied.timebase > 0;
        const int64_t product = tick * song.timebase;
        commands = counted ? InTimebase(copied.commands, copied.timebase, song.timebase) : std::nullopt;
        if (!commands || product % copied.timebase != 0)
        {
            throw std::invalid_argument("The track counts " + std::to_string(copied.timebase) +
                                        " ticks to a quarter note, and the sound " + std::to_string(song.timebase) +
                                        ". Some of the track's times come from variables or fall between the "
                                        "sound's ticks.");
        }
        tick = product / copied.timebase;
    }

    // The sound's slots take the track's banks.
    const format::NativeTables& tables = *file.native->tables;
    const std::vector<bool> runs(commands->size(), true);
    const int slot_count = ConsoleOf(file).wii ? kWiiBankSlots : kBankSlots;
    const Slots slots =
        MapSlots(tables, sound, slot_count, SlotsSelected(sequence.items, Reached(sequence, start.item, true)),
                 SlotsSelected(*commands, runs), copied.banks);
    CheckBankSlots(tables, slots.banks);

    const auto slot_of = [&slots](int slot)
    {
        const auto found = slots.moved.find(slot);
        return found != slots.moved.end() ? found->second : slot;
    };

    // The track selects the slot of its first notes, and waits to the tick at which its sound started it. It then jumps
    // to the command where the copied track started, when commands that the track reaches come before that one. Its
    // bank selects then select the sound's slots.
    std::vector<Command> track;
    if (slot_of(0) != 0)
    {
        track.push_back(CommandOf(Kind::kBankSelect, slot_of(0)));
    }
    if (tick > 0)
    {
        track.push_back(CommandOf(Kind::kWait, tick));
    }
    if (copied.entry != 0)
    {
        track.push_back(CommandOf(Kind::kJump, 0));
    }
    const auto lead = static_cast<uint32_t>(track.size());
    if (copied.entry != 0)
    {
        track.back().target = lead + copied.entry;
    }
    for (Command command : *commands)
    {
        if (ntdlib::commands::HasTarget(command.kind) && !command.target_outside)
        {
            command.target += lead;
        }
        const int slot = static_cast<int>(command.value.value / kOne);
        if (command.kind == Kind::kBankSelect && command.value.source == Source::kValue && slot_of(slot) != slot)
        {
            command.value.value = int64_t{slot_of(slot)} * kOne;
        }
        track.push_back(std::move(command));
    }

    // The track's name labels its first command. The copied labels name theirs.
    const std::string name = tables.sounds[sound].name + "_Track_" + std::to_string(number);
    std::vector<Label> labels = {{name, 0}};
    for (const Label& label : copied.labels)
    {
        if (label.item + lead != 0)
        {
            labels.push_back({label.name, label.item + lead});
        }
    }

    // Track 0 opens it where a new track starts, and the sound's entry takes the banks.
    Command opening = CommandOf(Kind::kOpenTrack, 0);
    opening.index = static_cast<uint8_t>(number);
    opening.target = static_cast<uint32_t>(sequence.items.size());
    TrackEdit edit = WithNewTrack(file, start, sound, number, track, labels, {{place, opening, place == start.item}});
    if (slots.banks != tables.sounds[sound].banks)
    {
        const auto take = [sound, &slots](format::NativeArchive& native)
        {
            native.tables->sounds[sound].banks = slots.banks;
        };
        edit.file.native.Edit(take);
    }

    return edit;
}

format::File RemoveTrack(const format::File& file, std::span<const std::optional<SoundPlace>> places, uint32_t sound,
                         int track)
{
    const SoundStart start = StartOf(file, places, sound);
    const Sequence& sequence = SequenceToEdit(file, start.sequence);
    if (!file.native->tables || sound >= file.native->tables->sounds.size())
    {
        throw std::invalid_argument(
            "Hokuto can't read this archive's tables, and so can't change the tracks that the sound allocates.");
    }
    if (track == 0)
    {
        throw std::invalid_argument("Track 0 starts the sound's other tracks. Hokuto can't remove it.");
    }

    // The sound's entry, and an allocation at its start, stop allocating the track.
    const uint32_t bit = track > 0 && track < kTracks ? uint32_t{1} << track : 0;
    const uint32_t flags = FlagsOf(*file.native->tables, sound);
    const uint32_t at_start = AllocatedAt(sequence, start.item);
    if (((flags | at_start) & bit) == 0)
    {
        throw std::invalid_argument("The sound doesn't allocate that track.");
    }
    if ((flags & ~bit) == 0)
    {
        throw std::invalid_argument("A sound keeps one track at least.");
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
    removed.native.Edit(release);
    if ((at_start & bit) != 0)
    {
        Sequence changed = sequence;
        changed.items[start.item].value.value = int64_t{at_start & ~bit} * kOne;
        removed.sequences[start.sequence] = std::move(changed);
    }

    return removed;
}

} // namespace hokuto::edit
