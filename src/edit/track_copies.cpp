// SPDX-License-Identifier: MIT

// The copies of a track in edit/tracks.h, with the rules of NintendoWare and of the DS: a duplicate of a track in its
// sound, and a track copied from one sound and pasted into another. The rules of the other engines are in
// gba_tracks.cpp and dse_tracks.cpp.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
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
#include "edit/drivers.h"
#include "edit/engine_tracks.h"
#include "edit/track_common.h"
#include "edit/tracks.h"
#include "edit/tracks_parts.h"
#include "format/archive.h"
#include "song/ds.h"

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
using track_detail::CopyOf;
using track_detail::InTimebase;
using track_detail::kTracks;
using track_detail::kWholeSound;
using track_detail::kWiiBankSlots;
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

// The bank that sound `sound` of `file`, a DS file, plays (song::DsBankOf), in the archive that the file builds.
std::optional<uint32_t> DsBankOfSound(const format::File& file, uint32_t sound)
{
    return song::DsBankOf(*song::LoadDsArchive(format::ExportArchive(file)), sound);
}

// Throws std::invalid_argument with a message for the user when sound `sound` of `file`, a DS file, can't accept
// `copied`: a track of a sound that plays another bank. A DS sequence plays the one bank of its entry in the archive.
void CheckDsBank(const format::File& file, uint32_t sound, const CopiedTrack& copied)
{
    const std::shared_ptr<const song::DsArchive> archive = song::LoadDsArchive(format::ExportArchive(file));
    const std::optional<uint32_t> bank = song::DsBankOf(*archive, sound);
    if (bank && copied.banks.size() == 1 && copied.banks.front() == *bank)
    {
        return;
    }

    const std::string from = copied.banks.size() == 1 ? song::DsBankName(*archive, copied.banks.front()) : "no bank";
    const std::string to = bank ? song::DsBankName(*archive, *bank) : "no bank";
    throw std::invalid_argument("The track uses " + from + ", but the sound uses " + to +
                                ". A DS sequence can use only one bank.");
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
    const uint32_t index = ntdlib::sar::ItemNumber(bank);
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
                "The track selects its bank slot with a variable or a random number, and slot " + std::to_string(slot) +
                " of the sound already contains a different bank.");
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
            throw std::invalid_argument("The track uses " + BankNamed(tables, bank) + ", but the sound uses " +
                                        BankNamed(tables, BankIn(slots.banks, 0)) +
                                        ". A Wii sound can use only one bank.");
        }
        if (!to)
        {
            throw std::invalid_argument("The track uses " + BankNamed(tables, bank) +
                                        ", and the sound has no free bank slot for it.");
        }

        take(*to, bank);
        slots.moved[slot] = *to;
    }

    return slots;
}

// Throws std::invalid_argument with a message for the user when `copied` can't be pasted into a sound of
// NintendoWare, or of the DS with `ds` (PasteTrack).
void CheckPasteable(const CopiedTrack& copied, bool ds)
{
    if (copied.gba)
    {
        throw std::invalid_argument("The track comes from a GBA song. It can be pasted only into a GBA song.");
    }
    if (copied.ds && !ds)
    {
        throw std::invalid_argument("The track comes from a DS sequence. It can be pasted only into a DS sequence.");
    }
    if (copied.dse)
    {
        throw std::invalid_argument("The track comes from a DSE sound. It can be pasted only into a DSE sound.");
    }
    if (copied.driver)
    {
        throw std::invalid_argument(
            "The track comes from a song of another of the GBA's drivers. A sound of another "
            "driver can't take it.");
    }
    if (ds && !copied.ds)
    {
        throw std::invalid_argument("Only a track from a DS sequence can be pasted into a DS sequence.");
    }
}

// Converts `commands` and `tick`, the commands and the start of `copied`, to the ticks of `song` (PasteTrack). Throws
// std::invalid_argument with a message for the user for times that come from variables or fall between the ticks.
void RetimeCopy(const CopiedTrack& copied, const song::Sequence& song, std::optional<std::vector<Command>>& commands,
                int64_t& tick)
{
    if (song.timebase != copied.timebase)
    {
        const bool counted = song.timebase > 0 && copied.timebase > 0;
        const int64_t product = tick * song.timebase;
        commands = counted ? InTimebase(copied.commands, copied.timebase, song.timebase) : std::nullopt;
        if (!commands || product % copied.timebase != 0)
        {
            throw std::invalid_argument("The track uses " + std::to_string(copied.timebase) +
                                        " ticks per quarter note, but the sound uses " + std::to_string(song.timebase) +
                                        ". Some of its times come from variables or would fall between the sound's "
                                        "ticks.");
        }

        tick = product / copied.timebase;
    }
}

// The slots of sound `sound` of `file` for the banks of `copied`, whose commands in the sound's ticks are `commands`
// (PasteTrack). The sound starts at item `start` of `sequence`, and `ds` marks a DS sequence. Throws
// std::invalid_argument with a message for the user when the slots can't take the banks.
Slots SlotsOfCopy(const format::File& file, uint32_t sound, const Sequence& sequence, uint32_t start,
                  const std::vector<Command>& commands, const CopiedTrack& copied, bool ds)
{
    // The sound's slots take the track's banks. A DS sequence plays the one bank of its entry, and has no slots.
    Slots slots;
    if (ds)
    {
        CheckDsBank(file, sound, copied);
    }
    else
    {
        const format::NativeTables& tables = *file.native->tables;
        const std::vector<bool> runs(commands.size(), true);
        const int slot_count = ConsoleOf(file).wii ? kWiiBankSlots : kBankSlots;
        slots = MapSlots(tables, sound, slot_count, SlotsSelected(sequence.items, Reached(sequence, start, true)),
                         SlotsSelected(commands, runs), copied.banks);
        CheckBankSlots(tables, slots.banks);
    }

    return slots;
}

// The commands of a pasted track: its opening, then `commands`. `commands` start at tick `tick` and enter at item
// `entry`. `slots` moves the slots that the commands select (PasteTrack).
std::vector<Command> PastedCommands(const std::vector<Command>& commands, int64_t tick, uint32_t entry,
                                    const Slots& slots)
{
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

    if (entry != 0)
    {
        track.push_back(CommandOf(Kind::kJump, 0));
    }

    const auto lead = static_cast<uint32_t>(track.size());
    if (entry != 0)
    {
        track.back().target = lead + entry;
    }

    for (Command command : commands)
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

    return track;
}

} // namespace

namespace track_detail
{

TrackEdit DuplicateArchiveTrack(const format::File& file, std::span<const std::optional<SoundPlace>> places,
                                uint32_t sound, int track)
{
    const SoundStart start = StartOf(file, places, sound);
    const Sequence& sequence = SequenceToEdit(file, start.sequence);
    CheckRoom(file, sequence, sound);
    const uint32_t allocated = AllocationOf(file, sequence, sound, start.item) | 1;
    if (track < 0 || track >= kTracks || (allocated >> track & 1) == 0)
    {
        throw std::invalid_argument("The sound has no such track.");
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
        throw std::invalid_argument("The sound's commands never open that track.");
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
    const std::string name = TrackLabel(file, sound, number);
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

} // namespace track_detail

TrackEdit DuplicateTrack(const format::File& file, std::span<const std::optional<SoundPlace>> places, uint32_t sound,
                         int track)
{
    return EditsOf(ConsoleOf(file)).DuplicateTrack(file, places, sound, track);
}

namespace track_detail
{

CopiedTrack CopyArchiveTrack(const format::File& file, std::span<const std::optional<SoundPlace>> places,
                             uint32_t sound, const song::Sequence& song, int track)
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
        throw std::invalid_argument("The sound has no such track.");
    }

    const std::optional<uint32_t> first =
        ItemAtOffset(Offsets(sequence, ConsoleOf(file)), played->stretches.front().first);
    if (!first)
    {
        throw std::invalid_argument("The track starts where the sequence has no command.");
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
        throw std::invalid_argument("The track jumps to a byte offset in its sequence file. Can't copy it.");
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

    if (ConsoleOf(file).engine == format::Engine::kDs)
    {
        const std::optional<uint32_t> bank = DsBankOfSound(file, sound);
        copied.banks = bank ? std::vector<uint32_t>{*bank} : std::vector<uint32_t>{};
        copied.ds = true;
    }

    copied.sound = sound;
    copied.track = track;

    return copied;
}

} // namespace track_detail

CopiedTrack CopyTrack(const format::File& file, std::span<const std::optional<SoundPlace>> places, uint32_t sound,
                      const song::Sequence& song, int track)
{
    return EditsOf(ConsoleOf(file)).CopyTrack(file, places, sound, song, track);
}

namespace track_detail
{

TrackEdit PasteArchiveTrack(const format::File& file, std::span<const std::optional<SoundPlace>> places, uint32_t sound,
                            const song::Sequence& song, const CopiedTrack& copied)
{
    const bool ds = ConsoleOf(file).engine == format::Engine::kDs;
    CheckPasteable(copied, ds);

    const SoundStart start = StartOf(file, places, sound);
    const Sequence& sequence = SequenceToEdit(file, start.sequence);
    CheckRoom(file, sequence, sound);
    const uint32_t place = TrackStartPlace(sequence, start.item);
    const int number = FreeTrack(file, places, sound, std::array{place});

    // The track's times take the sound's ticks.
    std::optional<std::vector<Command>> commands = copied.commands;
    int64_t tick = copied.tick;
    RetimeCopy(copied, song, commands, tick);

    const Slots slots = SlotsOfCopy(file, sound, sequence, start.item, *commands, copied, ds);
    const std::vector<Command> track = PastedCommands(*commands, tick, copied.entry, slots);
    const auto lead = static_cast<uint32_t>(track.size() - commands->size());

    // The track's name labels its first command. The copied labels name theirs.
    const std::string name = TrackLabel(file, sound, number);
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
    if (!ds && slots.banks != file.native->tables->sounds[sound].banks)
    {
        const auto take = [sound, &slots](format::NativeArchive& native)
        {
            native.tables->sounds[sound].banks = slots.banks;
        };
        edit.file.native.Edit(take);
    }

    return edit;
}

} // namespace track_detail

TrackEdit PasteTrack(const format::File& file, std::span<const std::optional<SoundPlace>> places, uint32_t sound,
                     const song::Sequence& song, const CopiedTrack& copied)
{
    return EditsOf(ConsoleOf(file)).PasteTrack(file, places, sound, song, copied);
}

} // namespace hokuto::edit
