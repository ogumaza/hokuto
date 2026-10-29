// SPDX-License-Identifier: MIT

#include "edit/subroutines.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/nwsnd/console.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sar/layout.h"

#include "edit/bytecode.h"
#include "edit/timelines.h"

namespace hokuto::edit
{
namespace
{

using ntdlib::commands::Command;
using ntdlib::commands::Kind;
using ntdlib::commands::Label;
using ntdlib::commands::Sequence;

// The commands of sequence `sequence` of `file` for an edit of its labels or subroutines. Throws with a message for the
// user for a timeline.
const Sequence& ProgramToEdit(const format::File& file, uint32_t sequence)
{
    const Sequence& commands = SequenceToEdit(file, sequence);
    if (IsTimeline(file, sequence))
    {
        throw std::invalid_argument(
            "A timeline's labels and subroutines come from the converter. The Program button of the inspector's "
            "Sequence section lets the sequence take this edit.");
    }

    return commands;
}

// The same commands, for an edit that moves them. Throws with a message for the user for commands that can't move.
const Sequence& MovableProgram(const format::File& file, uint32_t sequence)
{
    const Sequence& commands = ProgramToEdit(file, sequence);
    if (Fixed(commands))
    {
        throw std::invalid_argument("This sequence's commands can't move. Hokuto can't change its subroutines.");
    }

    return commands;
}

// The item at offset `offset` of `commands` on `console`, or the end of the bytecode. Throws with a message for the
// user for an offset where no item starts.
uint32_t ItemOrEnd(const Sequence& commands, const song::Console& console, uint32_t offset)
{
    const std::vector<uint32_t> offsets = Offsets(commands, console);
    if (offset == offsets.back())
    {
        return static_cast<uint32_t>(commands.items.size());
    }

    const std::optional<uint32_t> item = ItemAtOffset(offsets, offset);
    if (!item)
    {
        throw std::invalid_argument("The command isn't where the List Editor shows it.");
    }

    return *item;
}

// The label of `commands` named `name`. Throws with a message for the user when the sequence has none.
std::size_t LabelNamed(const Sequence& commands, const std::string& name)
{
    const auto found = std::find_if(commands.labels.begin(), commands.labels.end(),
                                    [&name](const Label& label) { return label.name == name; });
    if (found == commands.labels.end())
    {
        throw std::invalid_argument("The sequence has no label named " + name + ".");
    }

    return static_cast<std::size_t>(found - commands.labels.begin());
}

// Throws with a message for the user for a name that LabelRefusal refuses.
void CheckName(const Sequence& commands, std::string_view name)
{
    if (const std::optional<std::string> refusal = LabelRefusal(commands, name))
    {
        throw std::invalid_argument(*refusal);
    }
}

// Puts `label` into `labels` before the first label whose name comes after its name. Labels in the order of their names
// stay in that order.
void PutLabel(std::vector<Label>& labels, Label label)
{
    const auto after =
        std::find_if(labels.begin(), labels.end(), [&label](const Label& other) { return label.name < other.name; });
    labels.insert(after, std::move(label));
}

// An edit that leaves every item of `commands` in its place.
std::vector<std::optional<uint32_t>> SameItems(const Sequence& commands)
{
    std::vector<std::optional<uint32_t>> items;
    for (uint32_t item = 0; item < commands.items.size(); item++)
    {
        items.emplace_back(item);
    }

    return items;
}

// `file` with sequence `sequence` as `result`, and the new item of each item in `items`.
Edited WithSequence(const format::File& file, uint32_t sequence, Sequence result,
                    std::vector<std::optional<uint32_t>> items, std::vector<uint32_t> added = {})
{
    Edited edited{file, std::move(items), std::move(added)};
    edited.file.sequences[sequence] = std::move(result);

    return edited;
}

// Gives each target and label of `commands` the item that `entries` gives its item: the item that code which entered at
// the old item enters at now. `entries` holds an item for each old item and the end.
void MoveEntries(std::vector<Command>& items, std::vector<Label>& labels, const std::vector<uint32_t>& entries)
{
    for (Command& command : items)
    {
        if (ntdlib::commands::HasTarget(command.kind) && !command.target_outside && command.target < entries.size())
        {
            command.target = entries[command.target];
        }
    }
    for (Label& label : labels)
    {
        label.item = entries.at(label.item);
    }
}

// Whether a jump, a call, a track start, a label or a sound's start goes to an item from `first` to `last` of sequence
// `sequence` of `file`.
bool EnteredBetween(const format::File& file, uint32_t sequence, uint32_t first, uint32_t last)
{
    const Sequence& commands = *file.sequences[sequence];

    const auto inside = [first, last](uint32_t item)
    {
        return item >= first && item <= last;
    };
    for (const Command& command : commands.items)
    {
        if (ntdlib::commands::HasTarget(command.kind) && !command.target_outside && inside(command.target))
        {
            return true;
        }
    }
    for (const Label& label : commands.labels)
    {
        if (inside(label.item))
        {
            return true;
        }
    }
    bool entered = false;
    ForEachItemPlace(*file.native, sequence,
                     [&](const ntdlib::sar::Place& place) { entered = entered || inside(place.offset); });

    return entered;
}

// The calls and loops that the stack of a track of `console` holds at most.
int StackDepthOf(const song::Console& console)
{
    if (console.wii)
    {
        return ntdlib::nwsnd::RevolutionPolicy::kCallStackDepth;
    }

    switch (console.flavor)
    {
    case ntdlib::sar::Flavor::kCafe:
        return ntdlib::nwsnd::CafePolicy::kCallStackDepth;
    case ntdlib::sar::Flavor::kCtr:
        return ntdlib::nwsnd::CtrPolicy::kCallStackDepth;
    case ntdlib::sar::Flavor::kNx:
        break;
    }

    return ntdlib::nwsnd::NxPolicy::kCallStackDepth;
}

// The item of the call at offset `call` of `commands`, and the item of its subroutine. Throws with a message for the
// user for a command that isn't a call, or a call to a byte offset.
std::pair<uint32_t, uint32_t> CallAt(const Sequence& commands, const song::Console& console, uint32_t call)
{
    const uint32_t item = ItemOrEnd(commands, console, call);
    if (item >= commands.items.size() || commands.items[item].kind != Kind::kCall)
    {
        throw std::invalid_argument("The command isn't a call.");
    }

    const Command& command = commands.items[item];
    if (command.target_outside || command.target >= commands.items.size())
    {
        throw std::invalid_argument("The call doesn't go to a command of the sequence.");
    }

    return {item, command.target};
}

// The return that ends the subroutine at item `first` of `commands`: the first command that the subroutine reaches
// without a jump, an end or a return that runs only if the last comparison held. Every loop that the subroutine opens
// closes before it. Throws with a message for the user for any other subroutine.
uint32_t StraightReturn(const Sequence& commands, uint32_t first)
{
    int loops = 0;
    for (uint32_t item = first; item < commands.items.size(); item++)
    {
        const Command& command = commands.items[item];
        switch (command.kind)
        {
        case Kind::kReturn:
            if (command.conditional || loops > 0)
            {
                throw std::invalid_argument(
                    "The subroutine returns only if a comparison held, or from inside a loop. Its commands can't take "
                    "the call's place.");
            }
            return item;

        case Kind::kJump:
        case Kind::kEnd:
        case Kind::kData:
        case Kind::kAllocateTracks:
            throw std::invalid_argument(
                "The subroutine jumps, ends or holds bytes that no command reads before its "
                "return. Its commands can't take the call's place.");

        case Kind::kLoopStart:
        case Kind::kLoopEnd:
            if (command.conditional || (command.kind == Kind::kLoopEnd && loops == 0))
            {
                throw std::invalid_argument(
                    "The subroutine ends a loop that it doesn't open, or opens or ends a loop "
                    "only if a comparison held. Its commands can't take the call's place.");
            }
            loops += command.kind == Kind::kLoopStart ? 1 : -1;
            break;

        default:
            break;
        }
    }

    throw std::invalid_argument("The subroutine runs on to the end of the sequence without a return.");
}

// The last item of the subroutine at item `first` of `commands`: the subroutine reaches each item from `first` to it,
// and no other, through its jumps and loops. A call goes on to the command after it, and a return or an end stops the
// subroutine. Throws with a message for the user for a subroutine whose commands aren't in one piece.
uint32_t LastOfSubroutine(const Sequence& commands, uint32_t first)
{
    std::vector<bool> reached(commands.items.size(), false);
    std::vector<uint32_t> next = {first};
    while (!next.empty())
    {
        const uint32_t item = next.back();
        next.pop_back();
        if (item >= commands.items.size())
        {
            throw std::invalid_argument("The subroutine runs on to the end of the sequence without a return.");
        }
        if (reached[item])
        {
            continue;
        }

        reached[item] = true;
        const Command& command = commands.items[item];
        if (command.kind == Kind::kData)
        {
            throw std::invalid_argument("The subroutine reaches bytes that no command reads. Hokuto can't copy it.");
        }
        if (command.kind == Kind::kJump)
        {
            if (command.target_outside)
            {
                throw std::invalid_argument("The subroutine jumps to a byte offset. Hokuto can't copy it.");
            }
            next.push_back(command.target);
        }
        if (RunsOn(command))
        {
            next.push_back(item + 1);
        }
    }

    const auto last = static_cast<uint32_t>(reached.rend() - std::find(reached.rbegin(), reached.rend(), true) - 1);
    if (!std::all_of(reached.begin() + first, reached.begin() + last + 1, [](bool item) { return item; }) ||
        std::any_of(reached.begin(), reached.begin() + first, [](bool item) { return item; }))
    {
        throw std::invalid_argument("The subroutine's commands aren't in one piece. Hokuto can't copy it.");
    }

    return last;
}

} // namespace

std::optional<std::string> LabelRefusal(const Sequence& commands, std::string_view name)
{
    const auto letter = [](char c)
    {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
    };
    const auto letter_or_digit = [&letter](char c)
    {
        return letter(c) || (c >= '0' && c <= '9');
    };
    if (name.empty() || !letter(name.front()) || !std::all_of(name.begin(), name.end(), letter_or_digit))
    {
        return "A label's name starts with a letter or an underscore, and holds only letters, digits and underscores.";
    }
    const auto same = [name](const Label& label)
    {
        return label.name == name;
    };
    if (std::any_of(commands.labels.begin(), commands.labels.end(), same))
    {
        return "The sequence has a label named " + std::string(name) + " already.";
    }

    return std::nullopt;
}

Edited AddLabel(const format::File& file, uint32_t sequence, uint32_t command, const std::string& name)
{
    const Sequence& commands = ProgramToEdit(file, sequence);
    const uint32_t item = ItemOrEnd(commands, ConsoleOf(file), command);
    CheckName(commands, name);

    Sequence result = commands;
    PutLabel(result.labels, Label{name, item});

    return WithSequence(file, sequence, std::move(result), SameItems(commands));
}

Edited RenameLabel(const format::File& file, uint32_t sequence, const std::string& name, const std::string& new_name)
{
    const Sequence& commands = ProgramToEdit(file, sequence);
    const std::size_t index = LabelNamed(commands, name);
    if (new_name == name)
    {
        return WithSequence(file, sequence, commands, SameItems(commands));
    }
    CheckName(commands, new_name);

    // The label takes its place among the others by its new name.
    Sequence result = commands;
    Label label = result.labels[index];
    label.name = new_name;
    result.labels.erase(result.labels.begin() + static_cast<std::ptrdiff_t>(index));
    PutLabel(result.labels, std::move(label));

    return WithSequence(file, sequence, std::move(result), SameItems(commands));
}

Edited MoveLabel(const format::File& file, uint32_t sequence, const std::string& name, uint32_t command)
{
    const Sequence& commands = ProgramToEdit(file, sequence);
    const std::size_t index = LabelNamed(commands, name);
    const uint32_t from = commands.labels[index].item;
    const uint32_t to = ItemOrEnd(commands, ConsoleOf(file), command);
    if (from == to)
    {
        return WithSequence(file, sequence, commands, SameItems(commands));
    }

    Sequence result = commands;
    result.labels[index].item = to;
    const auto stays = [&](const Label& label)
    {
        return label.item == from && label.name != name;
    };
    if (std::any_of(commands.labels.begin(), commands.labels.end(), stays))
    {
        return WithSequence(file, sequence, std::move(result), SameItems(commands));
    }

    // The jumps, calls and track starts that go to the label's command name the label, and go with it. So do the sounds
    // that start there. A field that measures a sound's start from the bytecode's first item measures from it still,
    // and a field that ends at the end of the bytecode stays there.
    for (Command& item : result.items)
    {
        if (ntdlib::commands::HasTarget(item.kind) && !item.target_outside && item.target == from)
        {
            item.target = to;
        }
    }
    Edited edited = WithSequence(file, sequence, std::move(result), SameItems(commands));
    if (from < commands.items.size())
    {
        const auto move_starts = [&](format::NativeArchive& native)
        {
            for (ntdlib::sar::Field& field : native.fields)
            {
                if (NamesItemOf(native, field.to, sequence) && field.to.offset == from)
                {
                    field.to.offset = to;
                }
            }
        };
        edited.file.native.Edit(move_starts);
    }

    return edited;
}

Edited RemoveLabel(const format::File& file, uint32_t sequence, const std::string& name)
{
    const Sequence& commands = ProgramToEdit(file, sequence);
    const std::size_t index = LabelNamed(commands, name);

    Sequence result = commands;
    result.labels.erase(result.labels.begin() + static_cast<std::ptrdiff_t>(index));

    return WithSequence(file, sequence, std::move(result), SameItems(commands));
}

std::vector<uint32_t> CallsTo(const Sequence& commands, uint32_t item)
{
    std::vector<uint32_t> calls;
    for (uint32_t i = 0; i < commands.items.size(); i++)
    {
        const Command& command = commands.items[i];
        if (command.kind == Kind::kCall && !command.target_outside && command.target == item)
        {
            calls.push_back(i);
        }
    }

    return calls;
}

Edited ToSubroutine(const format::File& file, uint32_t sequence, std::span<const uint32_t> selected, int depth,
                    const std::string& name)
{
    const Sequence& commands = MovableProgram(file, sequence);
    const song::Console console = ConsoleOf(file);
    CheckName(commands, name);
    if (selected.empty())
    {
        throw std::invalid_argument("Select the commands that become the subroutine.");
    }

    // The commands follow each other in the bytecode.
    const std::vector<uint32_t> offsets = Offsets(commands, console);
    std::vector<uint32_t> chosen;
    for (const uint32_t offset : selected)
    {
        const std::optional<uint32_t> item = ItemAtOffset(offsets, offset);
        if (!item)
        {
            throw std::invalid_argument("The command isn't where the List Editor shows it.");
        }
        chosen.push_back(*item);
    }
    std::sort(chosen.begin(), chosen.end());
    chosen.erase(std::unique(chosen.begin(), chosen.end()), chosen.end());
    const uint32_t first = chosen.front();
    const uint32_t last = chosen.back();
    if (last - first + 1 != chosen.size())
    {
        throw std::invalid_argument("The commands of a subroutine have to follow each other in the sequence.");
    }

    // Nothing but the call leads into the commands, and they run on to the command after them.
    int loops = 0;
    for (uint32_t item = first; item <= last; item++)
    {
        const Command& command = commands.items[item];
        switch (command.kind)
        {
        case Kind::kJump:
        case Kind::kReturn:
        case Kind::kEnd:
        case Kind::kAllocateTracks:
        case Kind::kData:
            throw std::invalid_argument(
                "A subroutine can't hold a jump, a return, an end, an allocation of tracks or "
                "bytes that no command reads.");

        case Kind::kLoopStart:
        case Kind::kLoopEnd:
            if (command.conditional || (command.kind == Kind::kLoopEnd && loops == 0))
            {
                throw std::invalid_argument(
                    "A subroutine has to hold the start and the end of each of its loops, and "
                    "neither can run only if a comparison held.");
            }
            loops += command.kind == Kind::kLoopStart ? 1 : -1;
            break;

        default:
            break;
        }
    }
    if (loops != 0)
    {
        throw std::invalid_argument(
            "A subroutine has to hold the start and the end of each of its loops, and neither "
            "can run only if a comparison held.");
    }
    if (last > first && EnteredBetween(file, sequence, first + 1, last))
    {
        throw std::invalid_argument(
            "A jump, a call, a track start, a label or a sound's start goes into the middle of "
            "these commands. Only their first command can take the call's place.");
    }
    if (depth + 1 > StackDepthOf(console))
    {
        throw std::invalid_argument("The call would take the track's stack of calls and loops past the " +
                                    std::to_string(StackDepthOf(console)) + " that the console holds.");
    }

    // The call takes the place of the commands. They go after the sequence's commands, with a return after them. Code
    // that entered at the first command enters at the call.
    const auto size = static_cast<uint32_t>(commands.items.size());
    const uint32_t count = last - first + 1;
    const uint32_t start = size - count + 1;
    std::vector<uint32_t> entries(size + 1);
    std::vector<std::optional<uint32_t>> items(size);
    for (uint32_t item = 0; item < size; item++)
    {
        const bool inside = item >= first && item <= last;
        items[item] = item < first ? item : inside ? start + (item - first) : item - count + 1;
        entries[item] = item == first ? first : *items[item];
    }
    entries[size] = size + 2;

    Sequence result;
    result.items.assign(commands.items.begin(), commands.items.begin() + first);
    Command call;
    call.kind = Kind::kCall;
    call.target = start;
    result.items.push_back(call);
    result.items.insert(result.items.end(), commands.items.begin() + last + 1, commands.items.end());
    result.items.insert(result.items.end(), commands.items.begin() + first, commands.items.begin() + last + 1);
    Command back;
    back.kind = Kind::kReturn;
    result.items.push_back(back);
    result.labels = commands.labels;
    MoveEntries(result.items, result.labels, entries);
    result.items[first].target = start;
    PutLabel(result.labels, Label{name, start});

    Edited edited = WithSequence(file, sequence, std::move(result), std::move(items), {first});
    MoveItemPlaces(edited.file, sequence, [&entries](uint32_t item) { return entries.at(item); });
    Offsets(*edited.file.sequences[sequence], console);

    return edited;
}

Edited DissolveCall(const format::File& file, uint32_t sequence, uint32_t call)
{
    const Sequence& commands = MovableProgram(file, sequence);
    const song::Console console = ConsoleOf(file);
    const auto [at, first] = CallAt(commands, console, call);
    if (commands.items[at].conditional)
    {
        throw std::invalid_argument(
            "The call runs only if the last comparison held. Its subroutine's commands can't "
            "take its place.");
    }
    const uint32_t end = StraightReturn(commands, first);
    if (at >= first && at <= end)
    {
        throw std::invalid_argument("The call is inside the subroutine that it calls.");
    }

    // A copy of the subroutine's commands takes the call's place. Code that entered at the call enters at the copy.
    const auto size = static_cast<uint32_t>(commands.items.size());
    const uint32_t count = end - first;
    std::vector<uint32_t> entries(size + 1);
    std::vector<std::optional<uint32_t>> items(size);
    for (uint32_t item = 0; item <= size; item++)
    {
        entries[item] = item <= at ? item : item + count - 1;
        if (item < size && item != at)
        {
            items[item] = entries[item];
        }
    }

    Sequence result;
    result.items.assign(commands.items.begin(), commands.items.begin() + at);
    result.items.insert(result.items.end(), commands.items.begin() + first, commands.items.begin() + end);
    result.items.insert(result.items.end(), commands.items.begin() + at + 1, commands.items.end());
    result.labels = commands.labels;
    MoveEntries(result.items, result.labels, entries);
    std::vector<uint32_t> added;
    for (uint32_t i = 0; i < count; i++)
    {
        added.push_back(at + i);
    }

    Edited edited = WithSequence(file, sequence, std::move(result), std::move(items), std::move(added));
    MoveItemPlaces(edited.file, sequence, [&entries](uint32_t item) { return entries.at(item); });
    Offsets(*edited.file.sequences[sequence], console);

    return edited;
}

Edited CopySubroutine(const format::File& file, uint32_t sequence, uint32_t call)
{
    const Sequence& commands = MovableProgram(file, sequence);
    const song::Console console = ConsoleOf(file);
    const auto [at, first] = CallAt(commands, console, call);
    const uint32_t last = LastOfSubroutine(commands, first);

    // The copy goes after the sequence's commands. What goes to a command of the subroutine from inside it goes to the
    // command's copy. Code that entered at the end of the bytecode enters at the end still.
    const auto size = static_cast<uint32_t>(commands.items.size());
    const uint32_t count = last - first + 1;
    const auto entered = [size, count](uint32_t item)
    {
        return item == size ? size + count : item;
    };

    Sequence result = commands;
    for (Command& command : result.items)
    {
        if (ntdlib::commands::HasTarget(command.kind) && !command.target_outside)
        {
            command.target = entered(command.target);
        }
    }
    for (Label& label : result.labels)
    {
        label.item = entered(label.item);
    }
    for (uint32_t item = first; item <= last; item++)
    {
        Command copy = commands.items[item];
        if (ntdlib::commands::HasTarget(copy.kind) && !copy.target_outside)
        {
            const bool inside = copy.target >= first && copy.target <= last;
            copy.target = inside ? size + (copy.target - first) : entered(copy.target);
        }
        result.items.push_back(std::move(copy));
    }
    result.items[at].target = size;

    // Each label of the subroutine names its command's copy too, with a number after the name.
    bool named = false;
    for (const Label& label : commands.labels)
    {
        if (label.item >= first && label.item <= last)
        {
            PutLabel(result.labels, Label{UniqueLabel(result.labels, label.name), size + (label.item - first)});
            named = named || label.item == first;
        }
    }
    if (!named)
    {
        PutLabel(result.labels, Label{UniqueLabel(result.labels, "sub"), size});
    }

    std::vector<uint32_t> added;
    for (uint32_t i = 0; i < count; i++)
    {
        added.push_back(size + i);
    }
    Edited edited = WithSequence(file, sequence, std::move(result), SameItems(commands), std::move(added));
    MoveItemPlaces(edited.file, sequence, [size, count](uint32_t item) { return item == size ? size + count : item; });
    Offsets(*edited.file.sequences[sequence], console);

    return edited;
}

} // namespace hokuto::edit
