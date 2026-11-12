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
#include "song/driver.h"

namespace hokuto::edit
{
namespace
{

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

namespace
{

// Throws std::invalid_argument with a message for the user for a move or a fixed value of `variables` that
// WithVariables refuses.
void CheckMoves(const VariableMap& variables)
{
    const auto game = [](int number)
    {
        return number >= 0 && number < song::kGameVariables;
    };
    for (const auto& [from, to] : variables.moves)
    {
        if (!game(from) || !game(to))
        {
            throw std::invalid_argument("Only variables 0–31 can be moved. Those are the variables that a game sets.");
        }
    }

    for (const auto& [number, value] : variables.fixed)
    {
        const std::string name = "Variable " + std::to_string(number);
        if (!game(number))
        {
            throw std::invalid_argument(
                "Only variables 0–31 can be given a fixed value. Those are the variables that a game sets.");
        }
        if (variables.moves.contains(number))
        {
            throw std::invalid_argument(name + " can't be both moved and given a fixed value.");
        }
        if (value < -32768 || value > 32767)
        {
            throw std::invalid_argument("Variable values must be from -32768 to 32767.");
        }
    }
}

// Stores in `numbers` the number that each moved variable of `variables` moves to, and returns the numbers, a bit each,
// that the variables of `used` occupy after the moves (WithVariables). Throws std::invalid_argument with a message for
// the user for moves that give two of them one number.
uint64_t MovedNumbers(const VariableMap& variables, uint64_t used, std::map<int, int>& numbers)
{
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
            throw std::invalid_argument("After these moves, two variables of the new commands would both be variable " +
                                        std::to_string(to) + ".");
        }

        renumbered |= (used & Bit(number)) != 0 ? Bit(to) : 0;
    }

    return renumbered;
}

// The commands that set each fixed variable of `variables` that the commands read (`reads`, a bit each), in the sound
// variable that stores it (WithVariables). `holders_of` gains each holder, and `numbers` each holder that isn't the
// variable. A holder avoids `taken` and `renumbered`. Throws std::invalid_argument with a message for the user when no
// sound variable is free.
std::vector<Command> AssignFixed(const VariableMap& variables, uint64_t reads, uint32_t taken, uint64_t renumbered,
                                 std::map<int, int>& numbers, std::map<int, int>& holders_of)
{
    std::vector<Command> setters;
    uint64_t holders = 0;
    for (const auto& [number, value] : variables.fixed)
    {
        if ((reads & Bit(number)) == 0)
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
            throw std::invalid_argument("Can't give variable " + std::to_string(number) +
                                        " a fixed value. The new commands and the game code already use all of the "
                                        "sound variables 0–15.");
        }

        holders |= Bit(*holder);
        holders_of[number] = *holder;
        if (*holder != number)
        {
            numbers[number] = *holder;
        }

        setters.push_back(song::ValueCommand(Kind::kSetVariable, value, static_cast<uint8_t>(*holder)));
    }

    return setters;
}

} // namespace

MappedVariables WithVariables(const Sequence& sequence, uint32_t start, const VariableMap& variables, uint32_t taken)
{
    if (start >= sequence.items.size())
    {
        throw std::invalid_argument("The start is past the end of the new sequence.");
    }

    CheckMoves(variables);

    // The variables that the commands use, and their numbers after the moves. Two of them can't take one number.
    const std::vector<bool> reached = Reached(sequence, start, true);
    const VariableUses uses = UsesOf(sequence, start);
    const uint64_t used = uses.reads | uses.sets;
    std::map<int, int> numbers;
    const uint64_t renumbered = MovedNumbers(variables, used, numbers);

    // Each fixed variable that the commands read takes one of the sound's variables. It keeps its number where it can.
    MappedVariables mapped{sequence, start, {}};
    std::vector<Command> setters = AssignFixed(variables, uses.reads, taken, renumbered, numbers, mapped.holders);

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
                "Can't make room for the commands that set the fixed variables. The new commands must keep their "
                "positions.");
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
