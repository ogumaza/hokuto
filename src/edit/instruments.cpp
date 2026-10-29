// SPDX-License-Identifier: MIT

#include "edit/instruments.h"

#include <algorithm>
#include <cstdint>
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
#include "ntdlib/sequence_export/performance.h"

#include "edit/bytecode.h"
#include "edit/commands.h"

namespace hokuto::edit
{
namespace
{

using ntdlib::commands::Command;
using ntdlib::commands::Kind;
using ntdlib::commands::kOne;
using ntdlib::commands::Source;

// A sound has four bank slots.
constexpr int kBankSlots = 4;

// The message for the user where a variable, a random number or a comparison gives the instrument of a note.
constexpr const char* kDecided =
    "A variable, a random number or a comparison decides the program or the bank slot of some of this track's notes. "
    "The List Editor changes the track's commands: Edit > Open List Editor.";

// A command that set a part of a track's instrument: an item of the sequence, or nothing for the track's start.
using Setter = std::optional<uint32_t>;

// A part of a track's instrument as the track runs, its program or its slot: the commands that may have set it last,
// and its value. The value is missing where it comes from a variable, a random number or a comparison. A command that
// runs only if the last comparison held may have set it, or the one before.
struct Selection
{
    std::set<Setter> setters = {std::nullopt};
    std::optional<int64_t> value = 0;
};

// The instruments of the notes that each command that sets a part of the instrument sets it for. Nothing stands for a
// note whose instrument comes from a variable, a random number or a comparison.
using Uses = std::map<Setter, std::set<std::optional<Program>>>;

// `selection` after `command`, a program change or a bank select at item `item`.
void Select(Selection& selection, const Command& command, uint32_t item)
{
    if (!command.conditional)
    {
        selection.setters.clear();
    }
    selection.setters.insert(item);
    const bool fixed = command.value.source == Source::kValue && !command.conditional;
    selection.value = fixed ? std::optional<int64_t>(command.value.value / kOne) : std::nullopt;
}

// The commands that set the bank slot and the program of each note of a track, by the commands that set them.
struct Walk
{
    Uses slots;
    Uses programs;
};

// The commands of `commands` that set the instrument of each note of a track that ran `stretches`, in the order that
// they ran. `offsets` gives the offset of each item (Offsets). Throws std::invalid_argument with a message for the user
// where the track's commands aren't where `stretches` shows them.
Walk WalkTrack(const ntdlib::commands::Sequence& commands, const std::vector<uint32_t>& offsets,
               std::span<const song::Stretch> stretches)
{
    Walk walk;
    Selection slot;
    Selection program;
    for (const song::Stretch& stretch : stretches)
    {
        const std::optional<uint32_t> first = ItemAtOffset(offsets, stretch.first);
        const std::optional<uint32_t> last = ItemAtOffset(offsets, stretch.last);
        if (!first || !last || *last < *first)
        {
            throw std::invalid_argument("The track's commands aren't where the inspector shows them.");
        }

        for (uint32_t i = *first; i <= *last; ++i)
        {
            const Command& command = commands.items[i];
            if (command.kind == Kind::kBankSelect)
            {
                Select(slot, command, i);
            }
            else if (command.kind == Kind::kProgram)
            {
                Select(program, command, i);
            }
            else if (command.kind == Kind::kNote)
            {
                std::optional<Program> played;
                if (slot.value && program.value)
                {
                    played = Program{static_cast<int>(*slot.value), static_cast<int>(*program.value)};
                }
                for (const Setter& setter : slot.setters)
                {
                    walk.slots[setter].insert(played);
                }
                for (const Setter& setter : program.setters)
                {
                    walk.programs[setter].insert(played);
                }
            }
        }
    }

    return walk;
}

// The setters in `uses` of the notes that play `instrument`.
std::set<Setter> SettersOf(const Uses& uses, const Program& instrument)
{
    std::set<Setter> setters;
    for (const auto& [setter, played] : uses)
    {
        if (played.contains(instrument))
        {
            setters.insert(setter);
        }
    }

    return setters;
}

// Throws std::invalid_argument with a message for the user unless each setter of `setters` sets its part of the
// instrument for notes of one instrument alone, and decides it. `other` says what else the notes play.
void CheckAlone(const Uses& uses, const std::set<Setter>& setters, const char* other)
{
    for (const Setter& setter : setters)
    {
        const std::set<std::optional<Program>>& played = uses.at(setter);
        if (played.contains(std::nullopt))
        {
            throw std::invalid_argument(kDecided);
        }
        if (played.size() > 1)
        {
            throw std::invalid_argument(std::string(other) +
                                        " The List Editor changes the track's commands: Edit > Open List Editor.");
        }
    }
}

} // namespace

Setters InstrumentSetters(const ntdlib::commands::Sequence& commands, const std::vector<uint32_t>& offsets,
                          std::span<const song::Stretch> stretches, Program instrument)
{
    const Walk walk = WalkTrack(commands, offsets, stretches);
    Setters setters;
    for (const Uses* uses : {&walk.programs, &walk.slots})
    {
        for (const Setter& setter : SettersOf(*uses, instrument))
        {
            if (setter)
            {
                setters.items.insert(*setter);
            }
            else
            {
                setters.start = true;
            }
        }
    }

    return setters;
}

Edited EditInstrument(const format::File& file, uint32_t sequence, int track, std::span<const song::Stretch> stretches,
                      Program from, Program to)
{
    if (to.slot < 0 || to.slot >= kBankSlots || to.number < 0)
    {
        throw std::invalid_argument("A sound's bank slots are 0 to 3, and a bank's programs count from 0.");
    }

    const song::Console console = ConsoleOf(file);
    if (to.slot != from.slot && !song::Runs(console, Kind::kBankSelect, 0))
    {
        throw std::invalid_argument("A sound of the Wii plays one bank. Its sequences select no other bank slot.");
    }

    // The track's commands in the order that they ran. Each note plays the instrument that its track has selected.
    const ntdlib::commands::Sequence& commands = SequenceToEdit(file, sequence);
    const std::vector<uint32_t> offsets = Offsets(commands, console);
    const Walk walk = WalkTrack(commands, offsets, stretches);
    const Uses& slot_uses = walk.slots;
    const Uses& program_uses = walk.programs;

    // Where no note is found that plays `from`, a note whose instrument isn't decided may play it.
    if (SettersOf(program_uses, from).empty())
    {
        const auto undecided = [](const auto& use)
        {
            return use.second.contains(std::nullopt);
        };
        const bool decided = std::none_of(program_uses.begin(), program_uses.end(), undecided);
        throw std::invalid_argument(decided ? "No note of this track plays that instrument." : kDecided);
    }

    // The commands that set the instrument of the notes that play `from`. Each has to set it for them alone.
    const std::set<Setter> programs = to.number != from.number ? SettersOf(program_uses, from) : std::set<Setter>();
    const std::set<Setter> slots = to.slot != from.slot ? SettersOf(slot_uses, from) : std::set<Setter>();
    CheckAlone(program_uses, programs,
               "Notes from another bank slot share the program change that this instrument's notes play with.");
    CheckAlone(slot_uses, slots,
               "Notes of another program share the bank slot that this instrument's notes play from.");

    // The commands take the new values, and keep their items.
    std::vector<CommandPlace> places;
    std::vector<Command> changed;
    const auto change = [&](const std::set<Setter>& setters, int value)
    {
        for (const Setter& setter : setters)
        {
            if (setter)
            {
                Command command = commands.items[*setter];
                command.value.value = int64_t{value} * kOne;
                places.push_back({offsets[*setter], {}});
                changed.push_back(command);
            }
        }
    };
    change(programs, to.number);
    change(slots, to.slot);
    Edited result = ChangeCommands(file, sequence, places, changed);

    // Where the track's start sets the instrument, new commands at the track's start set the new one. The items from
    // the track's first command on move past them.
    std::vector<Command> opening;
    if (slots.contains(std::nullopt))
    {
        opening.push_back(Command{.kind = Kind::kBankSelect, .value = {Source::kValue, int64_t{to.slot} * kOne, 0}});
    }
    if (programs.contains(std::nullopt))
    {
        opening.push_back(Command{.kind = Kind::kProgram, .value = {Source::kValue, int64_t{to.number} * kOne, 0}});
    }
    if (!opening.empty())
    {
        if (stretches.empty() || stretches.front().entry != ntdlib::sequence_export::Entry::kOpen)
        {
            throw std::invalid_argument("The track's commands aren't where the inspector shows them.");
        }
        if (Fixed(commands))
        {
            throw std::invalid_argument("This sequence's commands can't move. Hokuto can't add a command to it.");
        }

        const uint32_t first = *ItemAtOffset(offsets, stretches.front().first);
        const OpeningInsertion added = InsertAtOpening(result.file, sequence, first, track, std::move(opening));
        for (std::optional<uint32_t>& item : result.items)
        {
            if (item && *item >= added.first)
            {
                *item += added.count;
            }
        }
    }

    return result;
}

std::optional<Program> OpeningInstrument(const format::File& file, uint32_t sequence, const song::Track& track)
{
    std::optional<int64_t> slot;
    std::optional<int64_t> program;
    for (const CommandRow& row : CommandRows(file, sequence, track.stretches, track.notes, std::nullopt))
    {
        const Command& command = CommandAt(file, sequence, row.command);
        if (row.stretch != 0 || command.kind == Kind::kWait || command.kind == Kind::kNote ||
            command.kind == Kind::kEnd)
        {
            break;
        }
        if (command.kind != Kind::kProgram && command.kind != Kind::kBankSelect)
        {
            continue;
        }
        if (command.conditional || command.value.source != Source::kValue)
        {
            return std::nullopt;
        }
        (command.kind == Kind::kProgram ? program : slot) = command.value.value / kOne;
    }
    if (!program && !slot)
    {
        return std::nullopt;
    }

    return Program{static_cast<int>(slot.value_or(0)), static_cast<int>(program.value_or(0))};
}

} // namespace hokuto::edit
