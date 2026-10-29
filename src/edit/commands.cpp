// SPDX-License-Identifier: MIT

#include "edit/commands.h"

#include <algorithm>
#include <cstddef>
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
#include "ntdlib/nwsnd/commands.h"
#include "ntdlib/nwsnd/units.h"

#include "edit/bytecode.h"
#include "edit/notes.h"
#include "format/command_values.h"
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
using ntdlib::nwsnd::Scale;

// Why a command that the console's player skips can't go in (song::Runs): the 3DS's player skips some commands, and a
// build of NW4R SND on the Wii lacks some.
std::string SkippedMessage(const song::Console& console)
{
    if (console.wii)
    {
        return "The file's build of NW4R SND has no such command. The Wii has no bank select and no phase, curve or "
               "period of an LFO. Older builds lack some commands of later ones.";
    }

    return "The 3DS's sequence player skips such a command. Its tracks have one LFO, without a curve, a phase or a "
           "period, and send to FX A and FX B only.";
}

// The highest tempo that the players of the 3DS, the Wii U and the Switch play, and the Wii's from March 2008 on.
constexpr int64_t kHighestTempo = 1023;

// The item at offset `offset`, by the offsets that Offsets gives. Throws for an offset where no command starts.
uint32_t CommandItem(const std::vector<uint32_t>& offsets, uint32_t offset)
{
    const std::optional<uint32_t> item = ItemAtOffset(offsets, offset);
    if (!item)
    {
        throw std::invalid_argument("The command isn't where the List Editor shows it.");
    }

    return *item;
}

// The offsets of the items of `sequence` on `console`. Throws with a message for the user where the console's bytes
// can't hold a command.
std::vector<uint32_t> CheckedOffsets(const ntdlib::commands::Sequence& sequence, const song::Console& console)
{
    try
    {
        return Offsets(sequence, console);
    }
    catch (const std::invalid_argument&)
    {
        throw std::invalid_argument("The console's sequences can't hold this command's values.");
    }
}

// `argument` at the nearest value that the console holds on `scale`. A variable's number stays as it is.
void ToNearest(Argument& argument, const Scale& scale)
{
    if (argument.source == Source::kVariable)
    {
        return;
    }

    argument.value = ntdlib::nwsnd::ToUnit(ntdlib::nwsnd::FromUnit(argument.value, scale), scale);
    if (argument.source == Source::kRandom)
    {
        argument.high = ntdlib::nwsnd::ToUnit(ntdlib::nwsnd::FromUnit(argument.high, scale), scale);
    }
}

// How far `command` moves its track on, in ticks. With `note_wait`, the track's notes wait for their length. Nothing
// for a time that the bytecode doesn't give: a wait or a note whose length comes from a random number, a variable or a
// comparison, a note of length 0 that waits for the track's notes to end, and bytes that no command reads.
std::optional<uint32_t> TicksOf(const Command& command, std::optional<bool> note_wait)
{
    const auto exact = [&command]() -> std::optional<uint32_t>
    {
        const Argument& value = command.value;
        if (command.conditional || value.source != Source::kValue || value.value < 0 || value.value % kOne != 0)
        {
            return std::nullopt;
        }

        return static_cast<uint32_t>(value.value / kOne);
    };
    switch (command.kind)
    {
    case Kind::kWait:
        return exact();

    case Kind::kNote:
        {
            if (!note_wait)
            {
                return std::nullopt;
            }

            const std::optional<uint32_t> length = exact();
            if (!*note_wait)
            {
                return 0;
            }

            return length && *length > 0 ? length : std::nullopt;
        }

    case Kind::kData:
        return std::nullopt;

    default:
        return 0;
    }
}

// Whether a change from `before` to `after` changes only a note's key, velocity or length, in whole ticks.
bool NoteChange(const Command& before, const Command& after)
{
    const auto whole = [](const Argument& length)
    {
        return length.source == Source::kValue && length.value >= 0 && length.value % kOne == 0;
    };

    Command same = after;
    same.key = before.key;
    same.velocity = before.velocity;
    same.value = before.value;

    return before.kind == Kind::kNote && same == before && whole(before.value) && whole(after.value);
}

// An edit that leaves sequence `sequence` of `file` as it is.
Edited Unchanged(const format::File& file, uint32_t sequence)
{
    Edited edited{file, {}, {}};
    for (uint32_t item = 0; item < file.sequences[sequence]->items.size(); item++)
    {
        edited.items.emplace_back(item);
    }

    return edited;
}

} // namespace

std::vector<CommandRow> CommandRows(const format::File& file, uint32_t sequence,
                                    std::span<const song::Stretch> stretches, std::span<const song::Note> notes,
                                    std::optional<uint32_t> end)
{
    const ntdlib::commands::Sequence& commands = SequenceToEdit(file, sequence);
    return CommandRows(commands, Offsets(commands, ConsoleOf(file)), stretches, notes, end);
}

std::vector<CommandRow> CommandRows(const ntdlib::commands::Sequence& commands, const std::vector<uint32_t>& offsets,
                                    std::span<const song::Stretch> stretches, std::span<const song::Note> notes,
                                    std::optional<uint32_t> end)
{
    // The arrangement gives the start of each note that a stretch ran.
    std::map<std::pair<uint32_t, uint32_t>, uint32_t> starts;
    for (const song::Note& note : notes)
    {
        starts.emplace(std::pair(note.command, note.stretch), note.start);
    }

    // Each stretch runs its commands from its tick on. Waits move the track on, and so do notes while the track's notes
    // wait for their length.
    std::vector<CommandRow> rows;
    for (uint32_t index = 0; index < stretches.size(); index++)
    {
        const song::Stretch& stretch = stretches[index];
        const std::optional<uint32_t> first = ItemAtOffset(offsets, stretch.first);
        const std::optional<uint32_t> last = ItemAtOffset(offsets, stretch.last);
        if (end && stretch.tick >= *end)
        {
            break;
        }
        if (!first || !last || *last < *first || *last >= commands.items.size())
        {
            continue;
        }

        std::optional<uint32_t> tick = stretch.tick;
        std::optional<bool> note_wait = stretch.note_wait;
        const std::size_t begin = rows.size();
        std::vector<std::optional<uint32_t>> lengths;
        std::optional<uint32_t> after;
        bool whole = true;
        for (uint32_t item = *first; item <= *last; item++)
        {
            const Command& command = commands.items[item];
            const auto start = starts.find(std::pair(offsets[item], index));
            if (command.kind == Kind::kNote && start != starts.end())
            {
                tick = start->second;
            }
            if (end && tick && *tick > *end)
            {
                after = tick;
                whole = false;
                break;
            }

            rows.push_back({offsets[item], index, tick});
            if (command.kind == Kind::kNoteWait)
            {
                const bool known = !command.conditional && command.value.source == Source::kValue;
                note_wait = known ? std::optional<bool>(command.value.value != 0) : std::nullopt;
            }

            lengths.push_back(TicksOf(command, note_wait));
            tick = tick && lengths.back() ? std::optional<uint32_t>(*tick + *lengths.back()) : std::nullopt;
        }

        // A command after a wait of unknown length, such as one that runs only if the last comparison held, ran as many
        // ticks before the next known tick as the commands between them take. The stretch's last command starts the
        // next stretch at once.
        if (whole && index + 1 < stretches.size())
        {
            after = stretches[index + 1].tick;
        }
        for (std::size_t row = rows.size(); row-- > begin;)
        {
            const std::optional<uint32_t> length = lengths[row - begin];
            if (rows[row].tick)
            {
                after = rows[row].tick;
            }
            else if (after && length && *length <= *after)
            {
                rows[row].tick = *after - *length;
                after = rows[row].tick;
            }
            else
            {
                after = std::nullopt;
            }
        }

        // A command that ran after the loop's end stays out, and so do the commands after it.
        const auto late = [end](const CommandRow& row)
        {
            return end && row.tick && *row.tick > *end;
        };
        const auto first_late = std::find_if(rows.begin() + static_cast<std::ptrdiff_t>(begin), rows.end(), late);
        if (first_late != rows.end())
        {
            rows.erase(first_late, rows.end());
            break;
        }
    }

    return rows;
}

const Command& CommandAt(const format::File& file, uint32_t sequence, uint32_t command)
{
    const ntdlib::commands::Sequence& commands = SequenceToEdit(file, sequence);
    const uint32_t item = CommandItem(Offsets(commands, ConsoleOf(file)), command);
    if (item >= commands.items.size())
    {
        throw std::invalid_argument("The command isn't where the List Editor shows it.");
    }

    return commands.items[item];
}

bool DrawsRandom(const format::File& file, uint32_t sequence, const song::Sequence& song)
{
    // The List Editor's rows: the commands that each track ran, up to the end of the loop's first pass.
    const ntdlib::commands::Sequence& commands = SequenceToEdit(file, sequence);
    const std::vector<uint32_t> offsets = Offsets(commands, ConsoleOf(file));
    const std::optional<uint32_t> end = song.loop ? std::optional<uint32_t>(song.loop->end) : std::nullopt;
    for (const song::Track& track : song.tracks)
    {
        for (const CommandRow& row : CommandRows(commands, offsets, track.stretches, track.notes, end))
        {
            const uint32_t item = CommandItem(offsets, row.command);
            if (item >= commands.items.size())
            {
                continue;
            }

            const Command& command = commands.items[item];
            if (command.kind == Kind::kRandomVariable || command.value.source == Source::kRandom ||
                (command.time && command.time->source == Source::kRandom))
            {
                return true;
            }
        }
    }

    return false;
}

std::vector<LabelPlace> LabelsOf(const format::File& file, uint32_t sequence)
{
    const ntdlib::commands::Sequence& commands = SequenceToEdit(file, sequence);
    const std::vector<uint32_t> offsets = Offsets(commands, ConsoleOf(file));
    std::vector<LabelPlace> labels;
    for (const ntdlib::commands::Label& label : commands.labels)
    {
        if (label.item < offsets.size())
        {
            labels.push_back({label.name, offsets[label.item]});
        }
    }

    return labels;
}

uint32_t ItemOf(const format::File& file, uint32_t sequence, uint32_t command)
{
    const ntdlib::commands::Sequence& commands = SequenceToEdit(file, sequence);
    const std::vector<uint32_t> offsets = Offsets(commands, ConsoleOf(file));
    if (command == offsets.back())
    {
        return static_cast<uint32_t>(commands.items.size());
    }

    return CommandItem(offsets, command);
}

std::optional<ValueRange> ArgumentRange(const song::Console& console, const Command& command, Source source, bool time)
{
    if (command.kind == Kind::kData || command.kind == Kind::kOther ||
        (!time && !ntdlib::commands::UnitOf(command.kind)) || !song::Runs(console, command.kind, command.index))
    {
        return std::nullopt;
    }

    // The values that the encoder writes and the player reads back.
    const ntdlib::nwsnd::Grammar grammar = song::GrammarOf(console);
    const Scale scale = time ? ntdlib::nwsnd::kTicksScale : ntdlib::nwsnd::ScaleOf(command.kind, grammar);
    std::optional<ValueRange> range = format::HeldRange(grammar, command, source, time);

    // A tempo or a timebase of 0 stops the player's clock. A negative tempo plays as 0 on every console but the Wii
    // before March 2008. The same players hold a tempo to 1023 (ntdlib's TempoOf).
    const bool clock = command.kind == Kind::kTempo || command.kind == Kind::kTimebase;
    if (range && clock && !time && source != Source::kVariable)
    {
        range->lowest = std::max(range->lowest, ntdlib::nwsnd::ToUnit(1, scale));
        if (command.kind == Kind::kTempo && (!console.wii || console.wii->SinceMarch2008()))
        {
            range->highest = std::min(range->highest, ntdlib::nwsnd::ToUnit(kHighestTempo, scale));
        }
    }

    return range;
}

Command Nearest(const song::Console& console, Command command)
{
    if (command.kind == Kind::kData)
    {
        return command;
    }

    const ntdlib::nwsnd::Grammar grammar = song::GrammarOf(console);
    command.velocity = ntdlib::nwsnd::ToUnit(ntdlib::nwsnd::FromUnit(command.velocity, ntdlib::nwsnd::kLevelScale),
                                             ntdlib::nwsnd::kLevelScale);
    ToNearest(command.value, ntdlib::nwsnd::ScaleOf(command.kind, grammar));
    if (command.time)
    {
        ToNearest(*command.time, ntdlib::nwsnd::kTicksScale);
    }

    return command;
}

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
        throw std::invalid_argument("A command keeps its kind. Delete it, and insert a command of the other kind.");
    }
    if (command.kind == Kind::kData || command.kind == Kind::kOther)
    {
        throw std::invalid_argument("Hokuto can't change bytes that the console reads as no command.");
    }
    if (command.index != before.index && !song::Runs(console, command.kind, command.index))
    {
        throw std::invalid_argument(SkippedMessage(console));
    }

    command = Nearest(console, std::move(command));
    if (command == before)
    {
        return Unchanged(file, sequence);
    }

    // A note's key, velocity and length change as the Key Editor changes them.
    if (NoteChange(before, command))
    {
        NoteEdit edit{.command = place.command, .stretch = place.stretch};
        edit.transpose = command.key - before.key;
        if (command.velocity != before.velocity)
        {
            edit.velocity = static_cast<int>(command.velocity / kOne);
        }
        if (command.value != before.value)
        {
            edit.length = static_cast<uint32_t>(command.value.value / kOne);
        }

        return EditNotes(file, sequence, std::span(&edit, 1));
    }

    if (ntdlib::commands::HasTarget(command.kind) && !command.target_outside && command.target > commands.items.size())
    {
        throw std::invalid_argument("A jump, a call or a track start has to go to a command of the sequence.");
    }

    // The other commands keep their bytes. In a sequence whose bytes keep their places, the command keeps its size.
    ntdlib::commands::Sequence changed = commands;
    changed.items[item] = std::move(command);
    if (CheckedOffsets(changed, console) != offsets && Fixed(commands))
    {
        throw std::invalid_argument(
            "This sequence's commands can't move. A change has to keep the command's size in bytes.");
    }

    Edited edited = Unchanged(file, sequence);
    edited.file.sequences[sequence] = std::move(changed);

    return edited;
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
            throw std::invalid_argument("A command keeps its kind. Delete it, and insert a command of the other kind.");
        }
        if (old.kind == Kind::kNote || old.kind == Kind::kData || old.kind == Kind::kOther)
        {
            throw std::invalid_argument(
                "A note, or bytes that the console reads as no command, can't change together with other commands.");
        }
        if (commands[i].index != old.index && !song::Runs(console, old.kind, commands[i].index))
        {
            throw std::invalid_argument(SkippedMessage(console));
        }
        if (ntdlib::commands::HasTarget(old.kind) && !commands[i].target_outside &&
            commands[i].target > before.items.size())
        {
            throw std::invalid_argument("A jump, a call or a track start has to go to a command of the sequence.");
        }

        changed.items[item] = Nearest(console, commands[i]);
    }
    if (changed == before)
    {
        return Unchanged(file, sequence);
    }
    if (CheckedOffsets(changed, console) != offsets && Fixed(before))
    {
        throw std::invalid_argument(
            "This sequence's commands can't move. A change has to keep the command's size in bytes.");
    }

    Edited edited = Unchanged(file, sequence);
    edited.file.sequences[sequence] = std::move(changed);

    return edited;
}

Edited MoveAndChangeCommands(const format::File& file, uint32_t sequence, std::span<const SettingMove> moves,
                             std::span<const CommandPlace> places, std::span<const Command> commands)
{
    const Edited moved = MoveSettings(file, sequence, moves);
    if (places.empty())
    {
        return moved;
    }

    // The changes find their commands where the moves left them. A move keeps every setting's item.
    const ntdlib::commands::Sequence& middle = *moved.file.sequences[sequence];
    const std::vector<uint32_t> offsets = Offsets(middle, ConsoleOf(moved.file));
    std::vector<CommandPlace> after;
    for (const CommandPlace& place : places)
    {
        const std::optional<uint32_t> item = moved.items.at(ItemOf(file, sequence, place.command));
        if (!item || *item >= middle.items.size())
        {
            throw std::invalid_argument("The command isn't where the List Editor shows it.");
        }

        after.push_back({offsets[*item], place.stretch});
    }

    // The changes keep every item where the moves left it.
    return Edited{ChangeCommands(moved.file, sequence, after, commands).file, moved.items, moved.added};
}

Edited InsertCommand(const format::File& file, uint32_t sequence, const CommandPlace& place, bool entered,
                     Command command)
{
    const ntdlib::commands::Sequence& commands = SequenceToEdit(file, sequence);
    const song::Console console = ConsoleOf(file);
    const uint32_t at = CommandItem(Offsets(commands, console), place.command);
    if (at >= commands.items.size())
    {
        throw std::invalid_argument("The command isn't where the List Editor shows it.");
    }
    if (command.kind == Kind::kData || command.kind == Kind::kOther)
    {
        throw std::invalid_argument("Hokuto can't add bytes that the console reads as no command.");
    }
    if (!song::Runs(console, command.kind, command.index))
    {
        throw std::invalid_argument(SkippedMessage(console));
    }
    if (Fixed(commands))
    {
        throw std::invalid_argument("This sequence's commands can't move. Hokuto can't add a command to it.");
    }
    if (ntdlib::commands::HasTarget(command.kind) && !command.target_outside && command.target > commands.items.size())
    {
        throw std::invalid_argument("A jump, a call or a track start has to go to a command of the sequence.");
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
    for (Command& item : result.items)
    {
        if (ntdlib::commands::HasTarget(item.kind) && !item.target_outside)
        {
            item.target = moved(item.target);
        }
    }
    for (ntdlib::commands::Label& label : result.labels)
    {
        label.item = moved(label.item);
    }
    if (ntdlib::commands::HasTarget(command.kind) && !command.target_outside && command.target >= at)
    {
        command.target++;
    }
    result.items.insert(result.items.begin() + at, Nearest(console, std::move(command)));
    CheckedOffsets(result, console);

    Edited edited{file, {}, {at}};
    edited.file.sequences[sequence] = std::move(result);
    MoveItemPlaces(edited.file, sequence, moved);
    for (uint32_t item = 0; item < commands.items.size(); item++)
    {
        edited.items.emplace_back(item < at ? item : item + 1);
    }

    return edited;
}

Edited RemoveCommands(const format::File& file, uint32_t sequence, std::span<const CommandPlace> places)
{
    const ntdlib::commands::Sequence& commands = SequenceToEdit(file, sequence);
    const std::vector<uint32_t> offsets = Offsets(commands, ConsoleOf(file));
    if (Fixed(commands))
    {
        throw std::invalid_argument("This sequence's commands can't move. Hokuto can't remove a command from it.");
    }

    // The notes go first. The Key Editor removes them the same way.
    std::vector<NoteEdit> notes;
    std::set<uint32_t> others;
    for (const CommandPlace& place : places)
    {
        const uint32_t item = CommandItem(offsets, place.command);
        if (item >= commands.items.size())
        {
            throw std::invalid_argument("The command isn't where the List Editor shows it.");
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
    const Edited first = notes.empty() ? Unchanged(file, sequence) : EditNotes(file, sequence, notes);

    // Then the other commands. Code that entered at a removed command enters at the next command that stays, or at the
    // end.
    const ntdlib::commands::Sequence& middle = *first.file.sequences[sequence];
    std::set<uint32_t> removed;
    for (const uint32_t item : others)
    {
        removed.insert(*first.items[item]);
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

    return edited;
}

} // namespace hokuto::edit
