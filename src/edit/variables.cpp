// SPDX-License-Identifier: MIT

#include "edit/variables.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/commands/units.h"

#include "edit/bytecode.h"

namespace hokuto::edit
{
namespace
{

using ntdlib::commands::Argument;
using ntdlib::commands::Command;
using ntdlib::commands::Kind;
using ntdlib::commands::Sequence;
using ntdlib::commands::Source;

// The bit of variable `number` in a set of variables, or 0 for a number past 63.
uint64_t Bit(int64_t number)
{
    return number >= 0 && number < 64 ? uint64_t{1} << number : 0;
}

// Whether a command of `kind` addresses a variable by its index: it sets the variable, works out its new value,
// compares it or prints it.
bool AddressesVariable(Kind kind)
{
    return (kind >= Kind::kSetVariable && kind <= Kind::kCompareNotEqual) || kind == Kind::kPrintVariable;
}

// Whether a command of `kind` gives its variable a new value.
bool SetsVariable(Kind kind)
{
    return kind >= Kind::kSetVariable && kind <= Kind::kModuloVariable;
}

// The variables that `command` reads and those that it sets. nn::atk's set, random and not commands give their variable
// a value without reading it.
VariableUses UsesOfCommand(const Command& command)
{
    VariableUses uses;
    if (command.value.source == Source::kVariable)
    {
        uses.reads |= Bit(command.value.value);
    }
    if (command.time && command.time->source == Source::kVariable)
    {
        uses.reads |= Bit(command.time->value);
    }
    if (AddressesVariable(command.kind))
    {
        const bool overwrites = command.kind == Kind::kSetVariable || command.kind == Kind::kRandomVariable ||
                                command.kind == Kind::kNotVariable;
        uses.sets |= SetsVariable(command.kind) ? Bit(command.index) : 0;
        uses.reads |= overwrites ? 0 : Bit(command.index);
    }

    return uses;
}

// Gives the variables of `command` the numbers of `numbers`. The map `numbers` gives each variable's new number.
void Renumber(Command& command, const std::map<int, int>& numbers)
{
    const auto renumbered = [&numbers](int64_t number)
    {
        const auto found = Bit(number) != 0 ? numbers.find(static_cast<int>(number)) : numbers.end();
        return found != numbers.end() ? int64_t{found->second} : number;
    };
    if (command.value.source == Source::kVariable)
    {
        command.value.value = renumbered(command.value.value);
    }
    if (command.time && command.time->source == Source::kVariable)
    {
        command.time->value = renumbered(command.time->value);
    }
    if (AddressesVariable(command.kind))
    {
        command.index = static_cast<uint8_t>(renumbered(command.index));
    }
}

} // namespace

VariableUses UsesOf(const Sequence& sequence, uint32_t start)
{
    VariableUses uses;
    if (start >= sequence.items.size())
    {
        return uses;
    }

    const std::vector<bool> reached = Reached(sequence, start, true);
    for (std::size_t item = 0; item < sequence.items.size(); item++)
    {
        if (reached[item])
        {
            const VariableUses command = UsesOfCommand(sequence.items[item]);
            uses.reads |= command.reads;
            uses.sets |= command.sets;
        }
    }

    return uses;
}

uint32_t InsertAtStart(Sequence& sequence, uint32_t start, std::vector<Command> commands)
{
    const Command& opening = sequence.items[start];
    const bool allocation = opening.kind == Kind::kAllocateTracks && !opening.conditional;
    const uint32_t jumps = start > 0 && RunsOn(sequence.items[start - 1]) ? 1 : 0;
    const auto count = static_cast<uint32_t>(commands.size());

    // Where each old item goes, and where a jump to it goes. An allocation stays first at the start.
    const auto moved = [&](uint32_t item)
    {
        if (item < start)
        {
            return item;
        }

        return allocation && item == start ? start + jumps : item + jumps + count;
    };
    const auto target = [&](uint32_t item)
    {
        return allocation && item == start ? moved(start + 1) : moved(item);
    };
    for (Command& item : sequence.items)
    {
        if (ntdlib::commands::HasTarget(item.kind) && !item.target_outside)
        {
            item.target = target(item.target);
        }
    }
    for (ntdlib::commands::Label& label : sequence.labels)
    {
        label.item = label.item == start ? start + jumps : moved(label.item);
    }

    // The new commands, after the allocation, and the jump before them.
    const uint32_t at = allocation ? start + 1 : start;
    sequence.items.insert(sequence.items.begin() + at, std::make_move_iterator(commands.begin()),
                          std::make_move_iterator(commands.end()));
    if (jumps != 0)
    {
        sequence.items.insert(sequence.items.begin() + start, Command{.kind = Kind::kJump, .target = target(start)});
    }

    return start + jumps;
}

MappedVariables WithVariables(const Sequence& sequence, uint32_t start, const VariableMap& variables, uint32_t taken)
{
    if (start >= sequence.items.size())
    {
        throw std::invalid_argument("The start lies past the end of the new sequence.");
    }

    const auto game = [](int number)
    {
        return number >= 0 && number < song::kGameVariables;
    };
    for (const auto& [from, to] : variables.moves)
    {
        if (!game(from) || !game(to))
        {
            throw std::invalid_argument("A game sets the variables 0 to 31. Hokuto moves only those.");
        }
    }
    for (const auto& [number, value] : variables.fixed)
    {
        const std::string name = "Variable " + std::to_string(number);
        if (!game(number))
        {
            throw std::invalid_argument("A game sets the variables 0 to 31. Hokuto fixes only those.");
        }
        if (variables.moves.contains(number))
        {
            throw std::invalid_argument(name + " can't both move and take a fixed value.");
        }
        if (value < -32768 || value > 32767)
        {
            throw std::invalid_argument("A variable holds a value from -32768 to 32767.");
        }
    }

    // The variables that the commands use, and their numbers after the moves. Two of them can't take one number.
    const std::vector<bool> reached = Reached(sequence, start, true);
    const VariableUses uses = UsesOf(sequence, start);
    const uint64_t used = uses.reads | uses.sets;
    std::map<int, int> numbers;
    for (const auto& [from, to] : variables.moves)
    {
        if (from != to)
        {
            numbers[from] = to;
        }
    }
    uint64_t renumbered = 0;
    for (int number = 0; number < 64; number++)
    {
        const auto found = numbers.find(number);
        const int to = found != numbers.end() ? found->second : number;
        if ((used & Bit(number)) != 0 && (renumbered & Bit(to)) != 0)
        {
            throw std::invalid_argument("The moves give two of the new commands' variables number " +
                                        std::to_string(to) + ".");
        }
        renumbered |= (used & Bit(number)) != 0 ? Bit(to) : 0;
    }

    // Each fixed variable that the commands read takes one of the sound's variables. It keeps its number where it can.
    MappedVariables mapped{sequence, start, {}};
    std::vector<Command> setters;
    uint64_t holders = 0;
    for (const auto& [number, value] : variables.fixed)
    {
        if ((uses.reads & Bit(number)) == 0)
        {
            continue;
        }

        const auto free = [&](int candidate)
        {
            return (taken >> candidate & 1u) == 0 && (holders & Bit(candidate)) == 0 &&
                   (candidate == number || (renumbered & Bit(candidate)) == 0);
        };

        std::optional<int> holder;
        if (number < kSoundVariables && free(number))
        {
            holder = number;
        }
        for (int candidate = 0; !holder && candidate < kSoundVariables; candidate++)
        {
            if (free(candidate))
            {
                holder = candidate;
            }
        }
        if (!holder)
        {
            throw std::invalid_argument("Variable " + std::to_string(number) +
                                        " can't take a fixed value. The new commands and the game's code use every "
                                        "one of the sound's variables, 0 to 15.");
        }
        holders |= Bit(*holder);
        mapped.holders[number] = *holder;
        if (*holder != number)
        {
            numbers[number] = *holder;
        }
        setters.push_back(Command{.kind = Kind::kSetVariable,
                                  .index = static_cast<uint8_t>(*holder),
                                  .value = Argument{Source::kValue, int64_t{value} * ntdlib::commands::kOne, 0}});
    }

    // The commands that the player reaches take the new numbers, and the sound's start the fixed values.
    for (std::size_t item = 0; item < mapped.sequence.items.size(); item++)
    {
        if (reached[item])
        {
            Renumber(mapped.sequence.items[item], numbers);
        }
    }
    if (!setters.empty())
    {
        if (Fixed(mapped.sequence))
        {
            throw std::invalid_argument(
                "The new commands can't move to make room for the commands that set the fixed variables.");
        }
        mapped.start = InsertAtStart(mapped.sequence, start, std::move(setters));
    }

    return mapped;
}

std::array<std::vector<uint32_t>, song::kGameVariables> VariableReaders(
    const format::File& file, std::span<const std::optional<SoundPlace>> places)
{
    // Sounds that start at the same item of a sequence read the same variables.
    std::array<std::vector<uint32_t>, song::kGameVariables> readers;
    std::map<std::pair<uint32_t, uint32_t>, uint64_t> reads;
    for (std::size_t sound = 0; sound < places.size(); sound++)
    {
        if (!places[sound])
        {
            continue;
        }

        const SoundPlace& place = *places[sound];
        const uint32_t start = place.start_field ? file.native->fields.at(*place.start_field).to.offset : 0;
        const auto [found, added] = reads.try_emplace({place.sequence, start}, 0);
        if (added)
        {
            try
            {
                found->second = UsesOf(SequenceToEdit(file, place.sequence), start).reads;
            }
            catch (const std::invalid_argument&)
            {
                // A sequence that can't be edited reads no variable that an edit can reach.
            }
        }
        for (int number = 0; number < song::kGameVariables; number++)
        {
            if ((found->second & Bit(number)) != 0)
            {
                readers[static_cast<std::size_t>(number)].push_back(static_cast<uint32_t>(sound));
            }
        }
    }

    return readers;
}

} // namespace hokuto::edit
