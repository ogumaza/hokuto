// SPDX-License-Identifier: MIT

#include "edit/wii_programs.h"

#include <algorithm>
#include <compare>
#include <limits>
#include <set>
#include <utility>
#include <vector>

#include "ntdlib/commands/units.h"

#include "song/track_flow.h"

namespace hokuto::edit
{
namespace
{

using ntdlib::commands::Command;
using ntdlib::commands::Kind;
using ntdlib::commands::kOne;
using ntdlib::commands::Sequence;
using ntdlib::commands::Source;

// The programs that a program change selects: a number below 0x10000. A larger number leaves the program as it was (the
// 0x81 case of MmlParser::CommandProc, 0x8003bf4c in Wii Sports' main.dol, and the later consoles' players in ntdlib's
// models).
constexpr int64_t kPrograms = 0x10000;

// A slot that a variable or a random number selects.
constexpr int kUnknownSlot = -1;

// The item of a held program that is a number.
constexpr uint32_t kNumber = std::numeric_limits<uint32_t>::max();

// A program that a track holds: a number, or the value that the program change at item `item` read from a variable or
// drew at random, plus `number`.
struct Held
{
    auto operator<=>(const Held&) const = default;

    uint32_t item = kNumber;
    int64_t number = 0;
};

// A track's slot and program on the console that the sequence comes from, and the program that the Wii's player holds.
struct TrackState
{
    auto operator<=>(const TrackState&) const = default;

    int slot = 0;
    Held program;
    Held wii;
};

using States = std::vector<TrackState>;

// What a program change or a bank select becomes on the Wii: a program change into a slot's range, from the range's
// first program, or a bank select that becomes a program change into `program`.
struct Change
{
    std::optional<uint32_t> first;
    std::optional<int64_t> program;
};

// The program that the program change `command` at `item` gives a track, or nothing for a number that leaves the
// program as it was.
std::optional<Held> ProgramOf(const Command& command, uint32_t item)
{
    if (command.value.source != Source::kValue)
    {
        return Held{.item = item, .number = 0};
    }

    const int64_t number = command.value.value / kOne;
    if (number < 0 || number >= kPrograms)
    {
        return std::nullopt;
    }

    return Held{.item = kNumber, .number = number};
}

// The states after `command` at `item` from the states `before`. With `changes`, the Wii's program follows the commands
// as they go to the Wii. Without them, it stays as it was.
States After(const Command& command, uint32_t item, const States& before, const std::vector<Change>* changes)
{
    States after;
    if (command.kind == Kind::kBankSelect)
    {
        const int slot = command.value.source == Source::kValue ? static_cast<int>((command.value.value / kOne) & 0xFF)
                                                                : kUnknownSlot;
        const std::optional<int64_t> program = changes ? (*changes)[item].program : std::nullopt;
        for (TrackState state : before)
        {
            state.slot = slot;
            if (program)
            {
                state.wii = Held{.item = kNumber, .number = *program};
            }
            after.push_back(state);
        }
    }
    else if (command.kind == Kind::kProgram)
    {
        const std::optional<Held> program = ProgramOf(command, item);
        if (!program)
        {
            return before;
        }

        Held wii = *program;
        if (changes && (*changes)[item].first)
        {
            wii.number += *(*changes)[item].first;
        }
        for (TrackState state : before)
        {
            state.program = *program;
            if (changes)
            {
                state.wii = wii;
            }
            after.push_back(state);
        }
    }
    else
    {
        return before;
    }
    std::sort(after.begin(), after.end());
    after.erase(std::unique(after.begin(), after.end()), after.end());

    return after;
}

// The programs that the notes of each known slot select, from the states before the notes: one past the highest. A
// random program counts its highest value. Slot 0 is always there.
std::map<int, int64_t> TopsOf(const Sequence& sequence, const std::vector<States>& states, bool& unknown_slots)
{
    std::map<int, int64_t> tops = {{0, 0}};
    for (uint32_t item = 0; item < sequence.items.size(); ++item)
    {
        if (sequence.items[item].kind != Kind::kNote)
        {
            continue;
        }

        for (const TrackState& state : states[item])
        {
            if (state.slot == kUnknownSlot)
            {
                unknown_slots = true;
                continue;
            }

            int64_t& top = tops[state.slot];
            if (state.program.item == kNumber)
            {
                top = std::max(top, state.program.number + 1);
                continue;
            }
            const Command& setter = sequence.items[state.program.item];
            if (setter.value.source == Source::kRandom)
            {
                const int64_t end = std::max(setter.value.value, setter.value.high) / kOne + 1;
                if (end <= kPrograms)
                {
                    top = std::max(top, end);
                }
            }
        }
    }

    return tops;
}

// The programs that the notes of each slot play (WiiPrograms::played), from the states before the notes.
std::map<int, std::optional<std::set<uint32_t>>> PlayedOf(const Sequence& sequence, const std::vector<States>& states)
{
    std::map<int, std::optional<std::set<uint32_t>>> played;
    std::set<std::pair<int, uint32_t>> drawn; // the slots and the random program changes counted already
    for (uint32_t item = 0; item < sequence.items.size(); ++item)
    {
        if (sequence.items[item].kind != Kind::kNote)
        {
            continue;
        }

        for (const TrackState& state : states[item])
        {
            std::optional<std::set<uint32_t>>& programs =
                played.try_emplace(state.slot, std::set<uint32_t>{}).first->second;
            if (!programs)
            {
                continue;
            }

            if (state.program.item == kNumber)
            {
                programs->insert(static_cast<uint32_t>(state.program.number));
                continue;
            }
            if (!drawn.insert({state.slot, state.program.item}).second)
            {
                continue;
            }
            const Command& setter = sequence.items[state.program.item];
            if (setter.value.source != Source::kRandom)
            {
                programs.reset();
                continue;
            }
            const int64_t low = std::max<int64_t>(std::min(setter.value.value, setter.value.high) / kOne, 0);
            const int64_t high =
                std::min<int64_t>(std::max(setter.value.value, setter.value.high) / kOne, kPrograms - 1);
            for (int64_t program = low; program <= high; ++program)
            {
                programs->insert(static_cast<uint32_t>(program));
            }
        }
    }

    return played;
}

} // namespace

std::optional<WiiPrograms> WiiProgramsOf(const Sequence& sequence, uint32_t start, std::span<const uint32_t> programs,
                                         std::size_t most)
{
    // The slots and programs before each item on the console that the sequence comes from.
    const std::vector<uint32_t> starts = {start};
    const auto after = [](const Command& command, uint32_t item, const States& before)
    {
        return After(command, item, before, nullptr);
    };
    const auto source = song::TrackStates(sequence, starts, TrackState{}, after, most);
    if (!source)
    {
        return std::nullopt;
    }

    // Each slot that the notes play takes the programs of its bank, and as many more as its notes select.
    WiiPrograms result;
    const std::map<int, int64_t> tops = TopsOf(sequence, *source, result.unknown_slots);
    result.played = PlayedOf(sequence, *source);
    int64_t next = 0;
    for (const auto& [slot, top] : tops)
    {
        const int64_t held =
            static_cast<std::size_t>(slot) < programs.size() ? programs[static_cast<std::size_t>(slot)] : 0;
        const int64_t count = std::max(held, top);
        if (next + count > kPrograms)
        {
            return std::nullopt;
        }
        result.slots[slot] = {.first = static_cast<uint32_t>(next), .count = static_cast<uint32_t>(count)};
        next += count;
    }

    // A program change takes the range of the one slot that it can run with. A bank select becomes a program change
    // where it can run with one program, a number.
    std::vector<Change> changes(sequence.items.size());
    if (result.slots.size() > 1)
    {
        for (uint32_t item = 0; item < sequence.items.size(); ++item)
        {
            const Command& command = sequence.items[item];
            const States& before = (*source)[item];
            if (before.empty())
            {
                continue;
            }

            if (command.kind == Kind::kProgram && command.value.source != Source::kVariable)
            {
                std::set<int> slots;
                for (const TrackState& state : before)
                {
                    slots.insert(state.slot);
                }
                const auto range = slots.size() == 1 ? result.slots.find(*slots.begin()) : result.slots.end();
                if (range == result.slots.end() || range->second.first == 0)
                {
                    continue;
                }
                const int64_t highest =
                    std::max(command.value.value,
                             command.value.source == Source::kRandom ? command.value.high : command.value.value) /
                    kOne;
                if (highest < kPrograms && highest + range->second.first < kPrograms)
                {
                    changes[item].first = range->second.first;
                }
            }
            else if (command.kind == Kind::kBankSelect && command.value.source == Source::kValue)
            {
                const auto range = result.slots.find(static_cast<int>((command.value.value / kOne) & 0xFF));
                std::set<Held> held;
                for (const TrackState& state : before)
                {
                    held.insert(state.program);
                }
                if (range == result.slots.end() || held.size() != 1 || held.begin()->item != kNumber)
                {
                    continue;
                }
                const int64_t program = range->second.first + held.begin()->number;
                if (program < kPrograms)
                {
                    changes[item].program = program;
                }
            }
        }
    }

    // The notes that the Wii may play with another instrument.
    const auto after_changes = [&changes](const Command& command, uint32_t item, const States& before)
    {
        return After(command, item, before, &changes);
    };
    const auto wii = song::TrackStates(sequence, starts, TrackState{}, after_changes, most);
    if (!wii)
    {
        return std::nullopt;
    }
    for (uint32_t item = 0; item < sequence.items.size(); ++item)
    {
        if (sequence.items[item].kind != Kind::kNote || (*wii)[item].empty())
        {
            continue;
        }

        const auto in_range = [&result](const TrackState& state)
        {
            const auto range = result.slots.find(state.slot);
            return range != result.slots.end() &&
                   state.wii == Held{.item = state.program.item, .number = state.program.number + range->second.first};
        };
        const bool sure = std::ranges::all_of((*wii)[item], in_range);
        result.unsure_notes += sure ? 0 : 1;
    }

    // The sequence with its changes.
    result.sequence = sequence;
    for (uint32_t item = 0; item < sequence.items.size(); ++item)
    {
        Command& command = result.sequence.items[item];
        if (const std::optional<uint32_t> first = changes[item].first)
        {
            command.value.value += *first * kOne;
            if (command.value.source == Source::kRandom)
            {
                command.value.high += *first * kOne;
            }
            ++result.programs;
        }
        else if (const std::optional<int64_t> program = changes[item].program)
        {
            command = Command{.kind = Kind::kProgram,
                              .conditional = command.conditional,
                              .value = {.source = Source::kValue, .value = *program * kOne, .high = 0}};
            ++result.bank_selects;
        }
    }

    return result;
}

} // namespace hokuto::edit
