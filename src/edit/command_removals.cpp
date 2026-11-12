// SPDX-License-Identifier: MIT

// The insertions and removals of commands, with the checks that a command can go in or out.

#include <cstdint>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/commands/sequence.h"

#include "edit/bytecode.h"
#include "edit/commands.h"
#include "edit/commands_parts.h"
#include "edit/drivers.h"
#include "edit/notes.h"
#include "song/archive.h"

namespace hokuto::edit
{
namespace
{

using ntdlib::commands::Command;
using ntdlib::commands::Kind;

using commands_parts::CheckedOffsets;
using commands_parts::CommandItem;
using commands_parts::Held;
using commands_parts::Unchanged;

// The other end of the counted repeat that item `item` of `sequence` starts or ends: the first end after a start, or
// the last start before an end, past the repeats that close between them. A track reads the items between them one
// after another (RunsOn). Nothing where the repeat has no other end.
std::optional<uint32_t> OtherEndOfRepeat(const ntdlib::commands::Sequence& sequence, uint32_t item)
{
    const Kind kind = sequence.items[item].kind;
    const bool forward = kind == Kind::kLoopStart;
    const Kind other = forward ? Kind::kLoopEnd : Kind::kLoopStart;
    int depth = 0;
    for (uint32_t at = item; forward ? at + 1 < sequence.items.size() : at > 0;)
    {
        // A track runs on from the earlier item of the two into the later.
        const uint32_t next = forward ? at + 1 : at - 1;
        if (!RunsOn(sequence.items[forward ? at : next]) || sequence.items[next].kind == Kind::kData)
        {
            return std::nullopt;
        }

        at = next;
        if (sequence.items[at].kind == kind)
        {
            depth++;
        }
        else if (sequence.items[at].kind == other && depth-- == 0)
        {
            return at;
        }
    }

    return std::nullopt;
}

} // namespace

namespace
{

// Throws std::invalid_argument with a message for the user when `command` can't go before item `at` of `commands`,
// sequence of `file`, on `console` (InsertCommand).
void CheckInsertable(const format::File& file, const ntdlib::commands::Sequence& commands, const song::Console& console,
                     uint32_t at, const Command& command)
{
    if (command.kind == Kind::kData || command.kind == Kind::kOther)
    {
        throw std::invalid_argument("Can't insert bytes that aren't a command.");
    }
    if (!song::Runs(console, command.kind, command.index))
    {
        throw std::invalid_argument(SkippedMessage(console, "command"));
    }
    if (KeepsPlaces(file, commands))
    {
        throw std::invalid_argument("Can't insert a command. Commands in this sequence must keep their positions.");
    }
    if (console.engine == format::Engine::kDs && at == 0 && commands.items[0].kind == Kind::kAllocateTracks)
    {
        throw std::invalid_argument(
            "The track allocation must be the first command of a DS sequence. Can't insert a command "
            "before it.");
    }
    if (console.engine == format::Engine::kDs && at != 0 && command.kind == Kind::kAllocateTracks)
    {
        throw std::invalid_argument(
            "The track allocation must be the first command of a DS sequence. Can't insert one here.");
    }
    if (console.engine == format::Engine::kDse && commands.items[at].kind == Kind::kData)
    {
        throw std::invalid_argument("Can't insert a command before the start of a DSE track.");
    }
}

} // namespace

Edited InsertCommand(const format::File& file, uint32_t sequence, const CommandPlace& place, bool entered,
                     Command command)
{
    const ntdlib::commands::Sequence& commands = SequenceToEdit(file, sequence);
    const song::Console console = ConsoleOf(file);
    const uint32_t at = CommandItem(Offsets(commands, console), place.command);
    if (at >= commands.items.size())
    {
        throw std::invalid_argument("The command isn't at the position that the List Editor shows.");
    }

    CheckInsertable(file, commands, console, at, command);
    if (console.driver)
    {
        EditsOf(*console.driver).PrepareInsert(file, sequence, commands, at, command);
    }

    if (ntdlib::commands::HasTarget(command.kind) && !command.target_outside && command.target > commands.items.size())
    {
        throw std::invalid_argument("A jump, call or track start must point to a command in this sequence.");
    }

    // The items from `at` on move one on. Code that entered at `at` enters at the new command when the track entered
    // there. A sound without a start of its own in the tables starts at the bytecode's first item, whatever that is.
    // The new command's target names an item before the edit.
    const bool takes_entries = entered || at == 0;
    const auto moved = [at, takes_entries](uint32_t item)
    {
        if (item < at)
        {
            return item;
        }

        return item == at && takes_entries ? at : item + 1;
    };

    ntdlib::commands::Sequence result = commands;
    MoveTargets(result, moved);
    if (ntdlib::commands::HasTarget(command.kind) && !command.target_outside && command.target >= at)
    {
        command.target++;
    }
    // On the GBA, Held checks the bytes once Writable has written the edit as the driver's bytes hold it. A new wait
    // there can join the wait beside it.
    result.items.insert(result.items.begin() + at, Nearest(console, std::move(command)));
    if (console.engine != format::Engine::kGba && !console.driver)
    {
        CheckedOffsets(result, console);
    }

    Edited edited{file, {}, {at}};
    edited.file.sequences[sequence] = std::move(result);
    MoveItemPlaces(edited.file, sequence, moved);
    for (uint32_t item = 0; item < commands.items.size(); item++)
    {
        edited.items.emplace_back(item < at ? item : item + 1);
    }

    return Held(file, std::move(edited), sequence);
}

namespace
{

// Sorts the commands of `commands`, sequence of `file` at `offsets`, that `places` names into `notes`, the edits that
// remove the notes, and `others`, the items of the other commands (RemoveCommands). Throws std::invalid_argument with
// a message for the user for a command that the List Editor doesn't show there, and for the start or the end of a track
// that `driver`, or DSE, requires.
void SortRemovals(const format::File& file, const ntdlib::commands::Sequence& commands,
                  const std::vector<uint32_t>& offsets, const song::DriverRules* driver,
                  std::span<const CommandPlace> places, std::vector<NoteEdit>& notes, std::set<uint32_t>& others)
{
    for (const CommandPlace& place : places)
    {
        const uint32_t item = CommandItem(offsets, place.command);
        if (item >= commands.items.size())
        {
            throw std::invalid_argument("The command isn't at the position that the List Editor shows.");
        }

        if (ConsoleOf(file).engine == format::Engine::kDse &&
            (commands.items[item].kind == Kind::kData || commands.items[item].kind == Kind::kEnd))
        {
            throw std::invalid_argument(
                "Can't remove the start or the end of a DSE track. Without its end, the driver "
                "would run into the next track.");
        }

        if (driver != nullptr && (commands.items[item].kind == Kind::kData || commands.items[item].kind == Kind::kEnd ||
                                  (commands.items[item].kind == Kind::kLoopEnd && !EditsOf(*driver).CountsRepeats())))
        {
            throw std::invalid_argument("Can't remove the end of a track on " + std::string(driver->Title()) +
                                        ". Without it, the driver would run into other data.");
        }

        if (commands.items[item].kind == Kind::kNote)
        {
            NoteEdit& note = notes.emplace_back(NoteEdit{.command = place.command, .stretch = place.stretch});
            note.remove = true;
        }
        else
        {
            others.insert(item);
        }
    }
}

// Throws std::invalid_argument with a message for the user for the start or the end of a repeat among `others`, items
// of `commands`, without its other end, on a `driver` whose loops are counted repeats (RemoveCommands).
void CheckRepeatPairs(const ntdlib::commands::Sequence& commands, const song::DriverRules* driver,
                      const std::set<uint32_t>& others)
{
    for (const uint32_t item : others)
    {
        const Kind kind = commands.items[item].kind;
        if (driver == nullptr || !EditsOf(*driver).CountsRepeats() ||
            (kind != Kind::kLoopStart && kind != Kind::kLoopEnd))
        {
            continue;
        }

        const std::optional<uint32_t> other = OtherEndOfRepeat(commands, item);
        if (other && !others.contains(*other))
        {
            throw std::invalid_argument(
                kind == Kind::kLoopStart
                    ? "Can't remove the start of a repeat without its end. Select its Loop End too."
                    : "Can't remove the end of a repeat without its start. Select its Loop Start too.");
        }
    }
}

} // namespace

Edited RemoveCommands(const format::File& file, uint32_t sequence, std::span<const CommandPlace> places)
{
    const ntdlib::commands::Sequence& commands = SequenceToEdit(file, sequence);
    const std::vector<uint32_t> offsets = Offsets(commands, ConsoleOf(file));
    const song::DriverRules* driver = ConsoleOf(file).driver;
    if (KeepsPlaces(file, commands))
    {
        throw std::invalid_argument("Can't remove a command. Commands in this sequence must keep their positions.");
    }

    // The notes go first. The Key Editor removes them the same way.
    std::vector<NoteEdit> notes;
    std::set<uint32_t> others;
    SortRemovals(file, commands, offsets, driver, places, notes, others);

    // On a driver whose loops are counted repeats, a repeat loses its start and its end together, or keeps both.
    CheckRepeatPairs(commands, driver, others);

    const Edited first = notes.empty() ? Unchanged(file, sequence) : EditNotes(file, sequence, notes);

    // Then the other commands. Code that entered at a removed command enters at the next command that stays, or at the
    // end. On Krawall, a removed note can take other commands with it.
    const ntdlib::commands::Sequence& middle = *first.file.sequences[sequence];
    std::set<uint32_t> removed;
    for (const uint32_t item : others)
    {
        if (first.items[item])
        {
            removed.insert(*first.items[item]);
        }
    }

    std::vector<uint32_t> moved(middle.items.size() + 1);
    uint32_t kept = 0;
    for (uint32_t item = 0; item <= middle.items.size(); item++)
    {
        moved[item] = kept;
        kept += removed.contains(item) ? 0 : 1;
    }

    ntdlib::commands::Sequence result;
    for (uint32_t item = 0; item < middle.items.size(); item++)
    {
        if (removed.contains(item))
        {
            continue;
        }

        Command& command = result.items.emplace_back(middle.items[item]);
        if (ntdlib::commands::HasTarget(command.kind) && !command.target_outside)
        {
            command.target = moved.at(command.target);
        }
    }

    result.labels = middle.labels;
    for (ntdlib::commands::Label& label : result.labels)
    {
        label.item = moved.at(label.item);
    }

    Edited edited{first.file, {}, {}};
    edited.file.sequences[sequence] = std::move(result);
    MoveItemPlaces(edited.file, sequence, [&moved](uint32_t item) { return moved.at(item); });
    for (const std::optional<uint32_t>& item : first.items)
    {
        edited.items.push_back(item && !removed.contains(*item) ? std::optional<uint32_t>(moved[*item]) : std::nullopt);
    }

    return Held(file, std::move(edited), sequence);
}

} // namespace hokuto::edit
