// SPDX-License-Identifier: MIT

// The track edits of a song of Nintendo R&D2's GBA driver (edit/rd2.h).

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/commands/units.h"
#include "ntdlib/rd2/commands.h"
#include "ntdlib/rd2/sequencer.h"

#include "edit/bytecode.h"
#include "edit/mp2k.h"
#include "edit/rd2.h"
#include "edit/track_common.h"
#include "edit/tracks.h"
#include "format/drivers.h"

namespace hokuto::edit
{

namespace
{

using ntdlib::commands::Command;
using ntdlib::commands::Kind;
using ntdlib::commands::kOne;
using ntdlib::commands::Sequence;
using track_detail::CommandOf;
using track_detail::InsertedBeforeData;
using track_detail::SoundStart;
using track_detail::StartOf;
using track_detail::TrailingDataStart;

// A song of Nintendo R&D2's driver counts 24 ticks to a quarter note.
constexpr int kRd2TicksPerQuarter = 24;

// The places of a song's header of Nintendo R&D2's driver: item 0 contains the count of tracks and a byte of 0, and the
// items after it each track's start (ntdlib/rd2/commands.h). Throws std::invalid_argument with a message for the user
// for a sound effects' sequence.
uint32_t Rd2TrackCount(const Sequence& sequence)
{
    if (IsRd2EffectSequence(sequence))
    {
        throw std::invalid_argument(
            "A sound effect of Nintendo R&D2's driver plays one track of its sequence. Hokuto changes the tracks of a "
            "song only.");
    }
    if (sequence.items.empty() || sequence.items[0].kind != Kind::kData || sequence.items[0].bytes.size() != 2)
    {
        throw std::invalid_argument("Hokuto finds no header of the song's tracks.");
    }

    const int count = static_cast<int8_t>(sequence.items[0].bytes[0]);
    if (count <= 0 || static_cast<std::size_t>(count) >= sequence.items.size())
    {
        throw std::invalid_argument("Hokuto finds no header of the song's tracks.");
    }

    for (int entry = 1; entry <= count; entry++)
    {
        const Command& item = sequence.items[static_cast<std::size_t>(entry)];
        if (item.kind != Kind::kOpenTrack || item.encoding.opcode != ntdlib::rd2::kHeaderEntry)
        {
            throw std::invalid_argument("Hokuto finds no header of the song's tracks.");
        }
    }

    return static_cast<uint32_t>(count);
}

// The item of the header that starts track `track`, or nothing.
std::optional<uint32_t> Rd2HeaderEntry(const Sequence& sequence, uint32_t count, int track)
{
    for (uint32_t entry = 1; entry <= count; entry++)
    {
        const Command& item = sequence.items[entry];
        if (item.index == track && !item.target_outside)
        {
            return entry;
        }
    }

    return std::nullopt;
}

// Whether a command of a track (F8) starts track `track`.
bool Rd2OpensTrack(const Sequence& sequence, int track)
{
    return std::ranges::any_of(sequence.items,
                               [track](const Command& item)
                               {
                                   return item.kind == Kind::kOpenTrack &&
                                          item.encoding.opcode != ntdlib::rd2::kHeaderEntry && item.index == track;
                               });
}

// The item where track `track` starts: its entry of the header, or else the target of the first command that starts it.
uint32_t Rd2EntryOf(const Sequence& sequence, int track)
{
    const uint32_t count = Rd2TrackCount(sequence);
    if (const std::optional<uint32_t> entry = Rd2HeaderEntry(sequence, count, track))
    {
        return sequence.items[*entry].target;
    }

    for (const Command& item : sequence.items)
    {
        if (item.kind == Kind::kOpenTrack && item.index == track && !item.target_outside)
        {
            return item.target;
        }
    }

    throw std::invalid_argument("The song doesn't play that track.");
}

// The items that the track that starts at `entry` runs, in their order: through its jumps and calls, up to its ends and
// returns. Throws std::invalid_argument with a message for the user for a track that runs into bytes that no track
// reads, or that jumps out of its sequence. The message names `action`, the edit's verb, such as "copy".
std::vector<uint32_t> Rd2TrackItems(const Sequence& sequence, uint32_t entry, std::string_view action)
{
    std::vector<bool> seen(sequence.items.size());
    std::vector<uint32_t> pending = {entry};
    while (!pending.empty())
    {
        const uint32_t item = pending.back();
        pending.pop_back();
        if (item >= sequence.items.size() || seen[item])
        {
            continue;
        }

        const Command& command = sequence.items[item];
        if (command.kind == Kind::kData)
        {
            throw std::invalid_argument("This track runs into bytes that no track reads. Hokuto can't " +
                                        std::string(action) + " it.");
        }

        seen[item] = true;
        if (ntdlib::commands::HasTarget(command.kind) && command.kind != Kind::kOpenTrack)
        {
            if (command.target_outside)
            {
                throw std::invalid_argument("This track jumps out of its sequence. Hokuto can't " +
                                            std::string(action) + " it.");
            }

            pending.push_back(command.target);
        }

        if (command.kind != Kind::kJump && command.kind != Kind::kEnd && command.kind != Kind::kReturn)
        {
            pending.push_back(item + 1);
        }
    }

    std::vector<uint32_t> items;
    for (uint32_t item = 0; item < seen.size(); item++)
    {
        if (seen[item])
        {
            items.push_back(item);
        }
    }

    return items;
}

// The commands of the track that starts at `entry`, with targets that count from the first command, and the item of
// the track's start among them. The copy leaves out the commands that start other tracks.
std::pair<std::vector<Command>, uint32_t> Rd2TrackCopy(const Sequence& sequence, uint32_t entry)
{
    const std::vector<uint32_t> items = Rd2TrackItems(sequence, entry, "copy");
    const auto kept = [&sequence](uint32_t item)
    {
        return sequence.items[item].kind != Kind::kOpenTrack;
    };

    // Each item's place in the copy: the place of the first command at or after it that the copy keeps.
    std::map<uint32_t, uint32_t> places;
    uint32_t next = 0;
    for (const uint32_t item : items)
    {
        places[item] = next;
        next += kept(item) ? 1 : 0;
    }

    std::vector<Command> copy;
    for (const uint32_t item : items)
    {
        if (!kept(item))
        {
            continue;
        }

        Command command = sequence.items[item];
        if (ntdlib::commands::HasTarget(command.kind))
        {
            command.target = places.at(command.target);
        }

        copy.push_back(std::move(command));
    }

    return {std::move(copy), places.at(entry)};
}

// The last bank slot that a command of `commands` selects, and 0 at least: a track starts with slot 0.
int Rd2LastSlot(std::span<const Command> commands)
{
    int last = 0;
    for (const Command& command : commands)
    {
        if (command.kind == Kind::kBankSelect)
        {
            last = std::max(last, static_cast<int>(command.value.value / kOne));
        }
    }

    return last;
}

// `file` once track `track` of the song of sequence `sequence` runs `commands`. The commands go before the bytes at the
// sequence's end that no track reads. The track gets its place of the header, or the place after its last. A target in
// `commands` counts from its first item, and the track starts at item `entry` of them.
TrackEdit WithRd2Track(const format::File& file, uint32_t sequence, int track, std::span<const Command> commands,
                       uint32_t entry)
{
    const Sequence& before = *file.sequences[sequence];
    const uint32_t count = Rd2TrackCount(before);
    const uint32_t at = TrailingDataStart(before);
    const auto size = static_cast<uint32_t>(commands.size());

    // A new place of the header is added after its last, and the commands follow the tracks.
    const bool new_place = static_cast<uint32_t>(track) >= count;
    const uint32_t place = count + 1;
    const auto moved = [at, size, new_place, place](uint32_t item)
    {
        const uint32_t after_commands = item < at ? item : item + size;
        return new_place && after_commands >= place ? after_commands + 1 : after_commands;
    };
    const uint32_t first_added = at + (new_place ? 1 : 0);

    Sequence after = InsertedBeforeData(before, commands, at, first_added, moved);

    const uint32_t start = first_added + entry;
    if (new_place)
    {
        Command header = before.items[1];
        header.index = static_cast<uint8_t>(track);
        header.target = start;
        header.target_outside = false;
        after.items.insert(after.items.begin() + place, header);
        after.items[0].bytes[0] = static_cast<uint8_t>(count + 1);
    }
    else
    {
        for (uint32_t item = 1; item <= count; item++)
        {
            Command& header = after.items[item];
            if (header.index == track)
            {
                header.target = start;
                header.target_outside = false;
            }
        }
    }

    TrackEdit edit{file, track};
    edit.file.sequences[sequence] = std::move(after);
    MoveItemPlaces(edit.file, sequence, moved);
    edit.file = Writable(file, Edited{std::move(edit.file), {}, {}}, sequence).file;

    return edit;
}

// The number that a new track of the song gets: a place of the header without a track, or else the place after its
// last. No command of a track may start it. Throws std::invalid_argument with a message for the user where none is
// free.
int FreeRd2Track(const Sequence& sequence)
{
    const uint32_t count = Rd2TrackCount(sequence);
    for (uint32_t entry = 1; entry <= count; entry++)
    {
        const Command& item = sequence.items[entry];
        if (item.target_outside && !Rd2OpensTrack(sequence, item.index))
        {
            return item.index;
        }
    }

    if (count < static_cast<uint32_t>(ntdlib::rd2::kPlayerTracks) && !Rd2OpensTrack(sequence, static_cast<int>(count)))
    {
        return static_cast<int>(count);
    }

    throw std::invalid_argument("A song of Nintendo R&D2's driver plays " + std::to_string(ntdlib::rd2::kPlayerTracks) +
                                " tracks at most, and this one has no track free.");
}

// The commands that wait as long as `ticks`, in waits that the driver's bytes hold.
std::vector<Command> Rd2Waits(uint32_t ticks)
{
    constexpr uint32_t kLongestWait = 0x7FFF;
    std::vector<Command> waits;
    while (ticks > 0)
    {
        const uint32_t wait = std::min(ticks, kLongestWait);
        waits.push_back(CommandOf(Kind::kWait, wait));
        ticks -= wait;
    }

    return waits;
}

} // namespace

// AddTrack on Nintendo R&D2's driver. The new track selects `instrument`, waits as long as the song, and loops with it.
// A track starts on slot 0 of the song's list of banks and on instrument 0 of that bank. A bank select also selects
// instrument 0, and the program change therefore follows it.
TrackEdit AddRd2Track(const format::File& file, std::span<const std::optional<SoundPlace>> places, uint32_t sound,
                      const song::Sequence& song, std::optional<Program> instrument)
{
    const SoundStart start = StartOf(file, places, sound);
    const Sequence& sequence = SequenceToEdit(file, start.sequence);
    const int track = FreeRd2Track(sequence);

    std::vector<Command> commands;
    if (instrument && instrument->slot != 0)
    {
        commands.push_back(CommandOf(Kind::kBankSelect, instrument->slot));
    }

    if (instrument && instrument->number != 0)
    {
        commands.push_back(CommandOf(Kind::kProgram, instrument->number));
    }

    if (song.loop && song.loop->end > song.loop->start)
    {
        const std::vector<Command> intro = Rd2Waits(song.loop->start);
        commands.insert(commands.end(), intro.begin(), intro.end());
        const auto loop = static_cast<uint32_t>(commands.size());
        const std::vector<Command> waits = Rd2Waits(song.loop->end - song.loop->start);
        commands.insert(commands.end(), waits.begin(), waits.end());
        Command jump = CommandOf(Kind::kJump, 0);
        jump.target = loop;
        commands.push_back(jump);
    }
    else
    {
        const uint32_t end = song.notes_end > 0 ? song.notes_end : song.length;
        const std::vector<Command> waits = Rd2Waits(end > 0 ? end : 4 * static_cast<uint32_t>(song.timebase));
        commands.insert(commands.end(), waits.begin(), waits.end());
        commands.push_back(CommandOf(Kind::kEnd, 0));
    }

    return WithRd2Track(file, start.sequence, track, commands, 0);
}

// DuplicateTrack on Nintendo R&D2's driver. The copy becomes a new track.
TrackEdit DuplicateRd2Track(const format::File& file, std::span<const std::optional<SoundPlace>> places, uint32_t sound,
                            int track)
{
    const SoundStart start = StartOf(file, places, sound);
    const Sequence& sequence = SequenceToEdit(file, start.sequence);
    const int free_track = FreeRd2Track(sequence);
    const auto [commands, entry] = Rd2TrackCopy(sequence, Rd2EntryOf(sequence, track));
    return WithRd2Track(file, start.sequence, free_track, commands, entry);
}

// CopyTrack on Nintendo R&D2's driver. `banks` contains the revision, then the driver's bank of each slot that the
// track can select.
CopiedTrack CopyRd2Track(const format::File& file, std::span<const std::optional<SoundPlace>> places, uint32_t sound,
                         int track)
{
    const SoundStart start = StartOf(file, places, sound);
    const Sequence& sequence = SequenceToEdit(file, start.sequence);
    auto [commands, entry] = Rd2TrackCopy(sequence, Rd2EntryOf(sequence, track));

    CopiedTrack copied;
    copied.banks = Rd2BanksOf(file, start.sequence, Rd2LastSlot(commands));
    copied.banks.insert(copied.banks.begin(), static_cast<uint32_t>(*Rd2Of(ConsoleOf(file))));
    copied.commands = std::move(commands);
    copied.entry = entry;
    copied.timebase = kRd2TicksPerQuarter;
    copied.sound = sound;
    copied.track = track;
    copied.driver = format::Engine::kRd2;

    return copied;
}

// PasteTrack on Nintendo R&D2's driver. The pasted track becomes a new track. Each bank select selects the slot of the
// song's list that contains the bank that the track played. A track that played another bank than the one in the first
// slot of the song selects that bank first.
TrackEdit PasteRd2Track(const format::File& file, std::span<const std::optional<SoundPlace>> places, uint32_t sound,
                        const CopiedTrack& copied)
{
    if (copied.driver != format::Engine::kRd2 || copied.banks.empty() ||
        copied.banks.front() != static_cast<uint32_t>(*Rd2Of(ConsoleOf(file))))
    {
        throw std::invalid_argument(
            "The track comes from a sound of another driver. A song of Nintendo R&D2's driver takes a track of the "
            "same build only.");
    }

    const SoundStart start = StartOf(file, places, sound);
    const Sequence& sequence = SequenceToEdit(file, start.sequence);
    const int free_track = FreeRd2Track(sequence);
    const std::vector<uint32_t> source(copied.banks.begin() + 1, copied.banks.end());
    const std::vector<uint32_t> banks = Rd2BanksOf(file, start.sequence, Rd2LastSlot(sequence.items));
    const auto slot_of = [&banks](uint32_t bank)
    {
        const auto found = std::ranges::find(banks, bank);
        if (found == banks.end())
        {
            throw std::invalid_argument("The track plays bank " + std::to_string(bank) +
                                        ". The song selects no slot of that bank. A pasted track of Nintendo R&D2's "
                                        "driver plays the banks that the song's commands select.");
        }

        return static_cast<int64_t>(found - banks.begin());
    };

    std::vector<Command> commands = copied.commands;
    uint32_t entry = copied.entry;
    for (Command& command : commands)
    {
        if (command.kind == Kind::kBankSelect)
        {
            const auto slot = static_cast<std::size_t>(command.value.value / kOne);
            command.value.value = slot_of(source.at(slot)) * kOne;
        }
    }

    // A jump back to the track's start keeps the bank that the track selected since.
    if (!source.empty() && slot_of(source.front()) != 0)
    {
        for (Command& command : commands)
        {
            if (ntdlib::commands::HasTarget(command.kind) && !command.target_outside && command.target >= entry)
            {
                command.target++;
            }
        }

        commands.insert(commands.begin() + entry, CommandOf(Kind::kBankSelect, slot_of(source.front())));
    }

    return WithRd2Track(file, start.sequence, free_track, commands, entry);
}

// RemoveTrack on Nintendo R&D2's driver. The track's place of the header stays without a track. A track that sets the
// song's tempo or its volume, or that starts other tracks, stays, and so does the song's last track.
format::File RemoveRd2Track(const format::File& file, std::span<const std::optional<SoundPlace>> places, uint32_t sound,
                            int track)
{
    const SoundStart start = StartOf(file, places, sound);
    const Sequence& sequence = SequenceToEdit(file, start.sequence);
    const uint32_t count = Rd2TrackCount(sequence);
    const std::optional<uint32_t> entry = Rd2HeaderEntry(sequence, count, track);
    if (!entry)
    {
        throw std::invalid_argument(
            "Another track starts this one. Hokuto removes only a track that the song's header starts.");
    }

    uint32_t others = 0;
    for (uint32_t item = 1; item <= count; item++)
    {
        others += item != *entry && !sequence.items[item].target_outside ? 1 : 0;
    }
    if (others == 0)
    {
        throw std::invalid_argument("A song keeps one track at least.");
    }

    for (const uint32_t item : Rd2TrackItems(sequence, sequence.items[*entry].target, "remove"))
    {
        const Kind kind = sequence.items[item].kind;
        if (kind == Kind::kTempo || kind == Kind::kMainVolume)
        {
            throw std::invalid_argument(
                "This track sets the song's tempo or its volume. Without it, the song would play otherwise.");
        }
        if (kind == Kind::kOpenTrack)
        {
            throw std::invalid_argument("This track starts other tracks. They would stop with it.");
        }
    }

    Sequence after = sequence;
    after.items[*entry].target = 0;
    after.items[*entry].target_outside = true;
    format::File removed = file;
    removed.sequences[start.sequence] = std::move(after);

    return Writable(file, Edited{std::move(removed), {}, {}}, start.sequence).file;
}

} // namespace hokuto::edit
