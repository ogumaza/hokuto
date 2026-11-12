// SPDX-License-Identifier: MIT

// The changes of commands: a new command's values, a change of one command or of several, and the moves of
// settings with their changes.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/commands/units.h"

#include "edit/bytecode.h"
#include "edit/commands.h"
#include "edit/commands_parts.h"
#include "edit/drivers.h"
#include "edit/mp2k.h"
#include "edit/notes.h"
#include "song/archive.h"

namespace hokuto::edit
{
namespace
{

using ntdlib::commands::Argument;
using ntdlib::commands::Command;
using ntdlib::commands::Kind;
using ntdlib::commands::kOne;
using ntdlib::commands::Source;

using commands_parts::CheckedOffsets;
using commands_parts::CommandItem;
using commands_parts::Held;
using commands_parts::ItemAt;
using commands_parts::Unchanged;

// Whether a change from `before` to `after` changes only a note's key, velocity or length, in whole ticks.
bool NoteChange(const Command& before, const Command& after)
{
    const auto whole = [](const Argument& length)
    {
        return WholeTicks(length).has_value();
    };

    Command same = after;
    same.key = before.key;
    same.velocity = before.velocity;
    same.value = before.value;

    return before.kind == Kind::kNote && same == before && whole(before.value) && whole(after.value);
}

} // namespace

namespace commands_parts
{

uint32_t CommandItem(const std::vector<uint32_t>& offsets, uint32_t offset)
{
    const std::optional<uint32_t> item = ItemAtOffset(offsets, offset);
    if (!item)
    {
        throw std::invalid_argument("The command isn't at the position that the List Editor shows.");
    }

    return *item;
}

std::vector<uint32_t> CheckedOffsets(const ntdlib::commands::Sequence& sequence, const song::Console& console)
{
    try
    {
        return Offsets(sequence, console);
    }
    catch (const std::invalid_argument&)
    {
        throw std::invalid_argument("These values are out of range for this console.");
    }
}

Edited Held(const format::File& before, Edited edited, uint32_t sequence)
{
    if (ConsoleOf(edited.file).engine == format::Engine::kGba || ConsoleOf(edited.file).driver)
    {
        edited = Writable(before, std::move(edited), sequence);
        CheckedOffsets(*edited.file.sequences[sequence], ConsoleOf(edited.file));
    }

    return edited;
}

Edited Unchanged(const format::File& file, uint32_t sequence)
{
    Edited edited{file, {}, {}};
    for (uint32_t item = 0; item < file.sequences[sequence]->items.size(); item++)
    {
        edited.items.emplace_back(item);
    }

    return edited;
}

uint32_t ItemAt(const ntdlib::commands::Sequence& commands, const std::vector<uint32_t>& offsets, uint32_t command)
{
    if (command == offsets.back())
    {
        return static_cast<uint32_t>(commands.items.size());
    }

    return CommandItem(offsets, command);
}

} // namespace commands_parts

Command NewCommand(Kind kind, int timebase, uint32_t target)
{
    // The values, if any, that the console gives a track before its commands set them.
    Command command;
    command.kind = kind;
    ntdlib::commands::Fixed value = 0;
    switch (kind)
    {
    case Kind::kNote:
        command.key = 60;
        command.velocity = 100 * kOne;
        value = int64_t{timebase} * kOne;
        break;

    case Kind::kWait:
        value = int64_t{timebase} * kOne;
        break;

    case Kind::kOpenTrack:
        command.index = 1;
        command.target = target;
        break;

    case Kind::kJump:
    case Kind::kCall:
        command.target = target;
        break;

    case Kind::kLoopStart:
        value = 2 * kOne;
        break;

    case Kind::kAllocateTracks:
        value = kOne;
        break;

    case Kind::kTimebase:
        value = 48 * kOne;
        break;

    case Kind::kVolume:
    case Kind::kMainVolume:
    case Kind::kVolume2:
    case Kind::kMainSend:
    case Kind::kVelocityRange:
        value = 127 * kOne;
        break;

    case Kind::kBendRange:
        value = 200 * kOne;
        break;

    case Kind::kPriority:
        value = 64 * kOne;
        break;

    case Kind::kPortamentoFrom:
        value = 60 * kOne;
        break;

    case Kind::kTempo:
        value = 120 * kOne;
        break;

    default:
        break;
    }

    command.value = Argument{Source::kValue, value, 0};

    return command;
}

Edited ChangeCommand(const format::File& file, uint32_t sequence, const CommandPlace& place, Command command)
{
    const ntdlib::commands::Sequence& commands = SequenceToEdit(file, sequence);
    const song::Console console = ConsoleOf(file);
    const std::vector<uint32_t> offsets = Offsets(commands, console);
    const uint32_t item = CommandItem(offsets, place.command);
    const Command& before = commands.items.at(item);
    if (command.kind != before.kind)
    {
        throw std::invalid_argument("Can't change the type of a command. Delete the command and insert a new one.");
    }
    if (command.kind == Kind::kData || command.kind == Kind::kOther)
    {
        throw std::invalid_argument("Can't edit bytes that aren't a command.");
    }
    if (command.index != before.index && !song::Runs(console, command.kind, command.index))
    {
        throw std::invalid_argument(SkippedMessage(console, "command"));
    }

    command = Nearest(console, std::move(command));
    if (command == before)
    {
        return Unchanged(file, sequence);
    }

    // A note's key, velocity and length change as the Key Editor changes them. A note of Brownie Brown's driver has no
    // length: its value is its wait. Like a wait's value, that wait moves the commands after the note.
    const DriverEdits* driver_edits = console.driver ? &EditsOf(*console.driver) : nullptr;
    if (NoteChange(before, command) && (driver_edits == nullptr || driver_edits->NotesHoldLengths()))
    {
        NoteEdit edit{.command = place.command, .stretch = place.stretch};
        edit.transpose = command.key - before.key;
        if (command.velocity != before.velocity)
        {
            edit.velocity = static_cast<int>(command.velocity / kOne);
        }

        // Konami's driver stores the semitones of a sample as a signed byte. A note edit of the GBA's other drivers
        // takes a velocity.
        if (console.driver)
        {
            edit.transpose = driver_edits->SampleTranspose(before, command).value_or(edit.transpose);
            edit.velocity = edit.velocity
                                ? std::optional(console.driver->VelocityOf(*edit.velocity, before.sample.has_value()))
                                : std::nullopt;
        }

        if (command.value != before.value)
        {
            edit.length = static_cast<uint32_t>(command.value.value / kOne);
        }

        return EditNotes(file, sequence, std::span(&edit, 1));
    }

    if (ntdlib::commands::HasTarget(command.kind) && !command.target_outside && command.target > commands.items.size())
    {
        throw std::invalid_argument("A jump, call or track start must point to a command in this sequence.");
    }

    // The other commands keep their bytes. In a sequence whose bytes keep their places, the command keeps its size.
    ntdlib::commands::Sequence changed = commands;
    changed.items[item] = std::move(command);
    if (console.engine != format::Engine::kGba && !console.driver && CheckedOffsets(changed, console) != offsets &&
        KeepsPlaces(file, commands))
    {
        throw std::invalid_argument(
            "Commands in this sequence must keep their positions. A change must keep the size of the command.");
    }

    Edited edited = Unchanged(file, sequence);
    edited.file.sequences[sequence] = std::move(changed);

    return Held(file, std::move(edited), sequence);
}

Edited ChangeCommands(const format::File& file, uint32_t sequence, std::span<const CommandPlace> places,
                      std::span<const Command> commands)
{
    if (places.size() != commands.size())
    {
        throw std::logic_error("a change for each place");
    }

    // Each command takes its new values in its item. The items keep their places, and only their bytes can move.
    const ntdlib::commands::Sequence& before = SequenceToEdit(file, sequence);
    const song::Console console = ConsoleOf(file);
    const std::vector<uint32_t> offsets = Offsets(before, console);
    ntdlib::commands::Sequence changed = before;
    for (std::size_t i = 0; i < places.size(); i++)
    {
        const uint32_t item = CommandItem(offsets, places[i].command);
        const Command& old = before.items.at(item);
        if (commands[i].kind != old.kind)
        {
            throw std::invalid_argument("Can't change the type of a command. Delete the command and insert a new one.");
        }
        if (old.kind == Kind::kNote || old.kind == Kind::kData || old.kind == Kind::kOther)
        {
            throw std::invalid_argument(
                "Notes and bytes that aren't a command can't be changed together with other commands.");
        }
        if (commands[i].index != old.index && !song::Runs(console, old.kind, commands[i].index))
        {
            throw std::invalid_argument(SkippedMessage(console, "command"));
        }

        if (ntdlib::commands::HasTarget(old.kind) && !commands[i].target_outside &&
            commands[i].target > before.items.size())
        {
            throw std::invalid_argument("A jump, call or track start must point to a command in this sequence.");
        }

        changed.items[item] = Nearest(console, commands[i]);
    }

    if (changed == before)
    {
        return Unchanged(file, sequence);
    }

    if (console.engine != format::Engine::kGba && !console.driver && CheckedOffsets(changed, console) != offsets &&
        KeepsPlaces(file, before))
    {
        throw std::invalid_argument(
            "Commands in this sequence must keep their positions. A change must keep the size of the command.");
    }

    Edited edited = Unchanged(file, sequence);
    edited.file.sequences[sequence] = std::move(changed);

    return Held(file, std::move(edited), sequence);
}

Edited MoveAndChangeCommands(const format::File& file, uint32_t sequence, std::span<const SettingMove> moves,
                             std::span<const CommandPlace> places, std::span<const Command> commands,
                             std::span<const song::Stretch> rows)
{
    const Edited moved = MoveSettings(file, sequence, moves, rows);
    if (places.empty())
    {
        return moved;
    }

    // The changes find their commands where the moves left them. A move keeps every setting's item.
    const ntdlib::commands::Sequence& middle = *moved.file.sequences[sequence];
    const std::vector<uint32_t> offsets = Offsets(middle, ConsoleOf(moved.file));
    const ntdlib::commands::Sequence& first = SequenceToEdit(file, sequence);
    const std::vector<uint32_t> first_offsets = Offsets(first, ConsoleOf(file));
    std::vector<CommandPlace> after;
    for (const CommandPlace& place : places)
    {
        const std::optional<uint32_t> item = moved.items.at(ItemAt(first, first_offsets, place.command));
        if (!item || *item >= middle.items.size())
        {
            throw std::invalid_argument("The command isn't at the position that the List Editor shows.");
        }

        after.push_back({offsets[*item], place.stretch});
    }

    // The changes keep every item where the moves left it. On the GBA, a wait can split into the codes' waits.
    const Edited changed = ChangeCommands(moved.file, sequence, after, commands);
    Edited edited{changed.file, moved.items, moved.added};
    for (std::optional<uint32_t>& item : edited.items)
    {
        if (item)
        {
            item = changed.items.at(*item);
        }
    }

    for (uint32_t& item : edited.added)
    {
        item = changed.items.at(item).value();
    }

    return edited;
}

} // namespace hokuto::edit
