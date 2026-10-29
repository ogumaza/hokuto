// SPDX-License-Identifier: MIT

#include "edit/notes.h"

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
#include "ntdlib/sar/layout.h"

#include "edit/bytecode.h"
#include "edit/timelines.h"
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

// A sequence's commands as an edit finds them: each item's offset in the bytecode, then the end, and the first item of
// each block, then the number of items. `fixed` is set for a sequence whose bytes must keep their places: one with a
// target that's a byte offset, or with bytes that no command was decoded from.
struct Code
{
    const std::vector<Command>* items = nullptr;
    std::vector<uint32_t> offsets;
    std::vector<uint32_t> starts;
    bool fixed = false;
};

// Whether `command` moves the track elsewhere, or may: a jump, a call, a return, a loop's start or end, or the end. The
// command after it starts a block.
bool EndsBlock(const Command& command)
{
    switch (command.kind)
    {
    case Kind::kJump:
    case Kind::kCall:
    case Kind::kReturn:
    case Kind::kLoopStart:
    case Kind::kLoopEnd:
    case Kind::kEnd:
        return true;
    default:
        return false;
    }
}

// The ticks of a wait whose length the bytecode gives, or nothing for another command.
std::optional<uint32_t> WaitTicks(const Command& command)
{
    if (command.kind != Kind::kWait || command.conditional || command.value.source != Source::kValue ||
        command.value.value < 0 || command.value.value % kOne != 0)
    {
        return std::nullopt;
    }

    return static_cast<uint32_t>(command.value.value / kOne);
}

// Whether a track that runs the commands of `code` from item `from` on ends in the same tick: an end that runs always
// comes before any command that takes time or moves the track elsewhere. A note that starts there doesn't sound.
// SequenceTrack::Close (0x022B6158) releases the track's channels in the tick where the track ends.
bool EndsAt(const Code& code, uint32_t from)
{
    for (uint32_t i = from; i < code.items->size(); ++i)
    {
        const Command& command = (*code.items)[i];
        if (command.kind == Kind::kEnd)
        {
            return !command.conditional;
        }
        if (command.kind == Kind::kNote || command.kind == Kind::kData || EndsBlock(command) ||
            (command.kind == Kind::kWait && WaitTicks(command) != 0))
        {
            return false;
        }
    }

    return false;
}

// Whether `command` determines the track's timing in a way that a block can't hold: a wait whose length comes from a
// random number, a variable or a comparison, a change of the note wait, and bytes that no command was decoded from.
// Such a command is a block of its own.
bool Barrier(const Command& command)
{
    return (command.kind == Kind::kWait && !WaitTicks(command)) || command.kind == Kind::kNoteWait ||
           command.kind == Kind::kData;
}

// The commands of sequence `sequence` of `file`, with its blocks.
Code Examine(const format::File& file, uint32_t sequence)
{
    Code code;
    const ntdlib::commands::Sequence& commands = SequenceToEdit(file, sequence);
    const std::vector<Command>& items = commands.items;
    code.items = &items;
    code.offsets = Offsets(commands, ConsoleOf(file));
    code.fixed = Fixed(commands);

    // Other code enters a sequence at a target, a label, a sound's start, and the command after a jump or a call. A
    // barrier stands alone.
    std::set<uint32_t> starts = {0, static_cast<uint32_t>(items.size())};
    for (uint32_t i = 0; i < items.size(); ++i)
    {
        const Command& item = items[i];
        if (ntdlib::commands::HasTarget(item.kind) && !item.target_outside)
        {
            starts.insert(item.target);
        }
        if (EndsBlock(item))
        {
            starts.insert(i + 1);
        }
        if (Barrier(item))
        {
            starts.insert(i);
            starts.insert(i + 1);
        }
    }
    for (const ntdlib::commands::Label& label : commands.labels)
    {
        starts.insert(label.item);
    }
    ForEachItemPlace(*file.native, sequence,
                     [&starts](const ntdlib::sar::Place& place) { starts.insert(place.offset); });
    std::erase_if(starts, [&items](uint32_t start) { return start > items.size(); });
    code.starts.assign(starts.begin(), starts.end());

    return code;
}

// The item at `offset` in the bytecode. Throws for an offset where no item starts.
uint32_t ItemAt(const Code& code, uint32_t offset)
{
    const std::optional<uint32_t> item = ItemAtOffset(code.offsets, offset);
    if (!item)
    {
        throw std::invalid_argument("The note's command isn't where the arrangement shows it.");
    }

    return *item;
}

// The note command at `offset`.
uint32_t NoteAt(const Code& code, uint32_t offset)
{
    const uint32_t item = ItemAt(code, offset);
    if ((*code.items)[item].kind != Kind::kNote)
    {
        throw std::invalid_argument("The note's command isn't where the arrangement shows it.");
    }

    return item;
}

// Whether `command` is a setting: a command that neither plays a note, waits, moves the track elsewhere nor determines
// the track's timing (Barrier). A new setting goes in at a tick (AddSettings).
bool IsSetting(const Command& command)
{
    return command.kind != Kind::kNote && command.kind != Kind::kWait && command.kind != Kind::kOther &&
           !EndsBlock(command) && !Barrier(command) && !ntdlib::commands::HasTarget(command.kind);
}

// Whether `command` reads or changes a variable, compares one, or runs the game's code.
bool UsesVariables(const Command& command)
{
    return (command.kind >= Kind::kSetVariable && command.kind <= Kind::kUserProcedure) ||
           command.kind == Kind::kPrintVariable || command.value.source == Source::kVariable ||
           (command.time && command.time->source == Source::kVariable);
}

// The item of the command at `offset` that an edit names: a note's command, or with `settings`, a setting's
// (MoveSettings). Throws std::invalid_argument with a message for the user for a setting that can't move.
uint32_t EditedAt(const Code& code, uint32_t offset, bool settings)
{
    if (!settings)
    {
        return NoteAt(code, offset);
    }

    const uint32_t item = ItemAt(code, offset);
    if (const std::optional<std::string> why = WhyFixedInTime((*code.items)[item]))
    {
        throw std::invalid_argument(*why);
    }

    return item;
}

// The block that holds `item`.
std::size_t BlockOf(const Code& code, uint32_t item)
{
    return static_cast<std::size_t>(std::upper_bound(code.starts.begin(), code.starts.end(), item) -
                                    code.starts.begin()) -
           1;
}

// Whether the track's notes wait for their length when `item` runs in `stretch`. Nothing when the item lies outside the
// stretch, or a command before it in the stretch changes the note wait by a random number, a variable or a comparison.
std::optional<bool> NoteWaitAt(const Code& code, const song::Stretch& stretch, uint32_t item)
{
    const uint32_t first = ItemAt(code, stretch.first);
    const uint32_t last = ItemAt(code, stretch.last);
    if (item < first || item > last)
    {
        return std::nullopt;
    }

    bool note_wait = stretch.note_wait;
    for (uint32_t i = first; i < item; ++i)
    {
        const Command& command = (*code.items)[i];
        if (command.kind == Kind::kData)
        {
            return std::nullopt;
        }
        if (command.kind == Kind::kNoteWait)
        {
            if (command.conditional || command.value.source != Source::kValue)
            {
                return std::nullopt;
            }
            note_wait = command.value.value != 0;
        }
    }

    return note_wait;
}

// The item where the commands at the end of block `block` go in: before its last command when that command moves the
// track elsewhere, and else at the next block's start (Rewrite).
uint32_t TailOf(const Code& code, std::size_t block)
{
    const uint32_t end = code.starts[block + 1];
    return end > code.starts[block] && EndsBlock((*code.items)[end - 1]) ? end - 1 : end;
}

// The tick of each command of block `block` from the block's start, then the block's length, with the note wait off.
std::vector<uint32_t> BlockTicks(const Code& code, std::size_t block)
{
    std::vector<uint32_t> ticks;
    uint32_t tick = 0;
    for (uint32_t i = code.starts[block]; i < code.starts[block + 1]; ++i)
    {
        ticks.push_back(tick);
        tick += WaitTicks((*code.items)[i]).value_or(0);
    }
    ticks.push_back(tick);

    return ticks;
}

// The ticks of a note that waits for its length, or nothing for a length that the bytecode doesn't give.
std::optional<uint32_t> NoteTicks(const Command& note)
{
    if (note.value.source != Source::kValue || note.value.value <= 0 || note.value.value % kOne != 0)
    {
        return std::nullopt;
    }

    return static_cast<uint32_t>(note.value.value / kOne);
}

// The ticks that a note waits for on a track whose notes wait for their length, when a wait of its own can take its
// place: the note runs every time, and the bytecode gives its length.
std::optional<uint32_t> WrittenWait(const Command& note)
{
    return note.conditional ? std::nullopt : NoteTicks(note);
}

// Why a note that waits for its length can't take an edit. `action` names the edit, such as "remove".
std::string WaitRefusal(const Command& note, const std::string& action)
{
    if (note.value.source != Source::kValue)
    {
        return "This note's track waits for a length that the sequence decides as it plays. Hokuto can't " + action +
               " the note.";
    }
    if (!NoteTicks(note))
    {
        return "This note makes its track wait until the track's notes have ended. Hokuto can't " + action + " it.";
    }

    return "Whether this note's track waits for it depends on a comparison. Hokuto can't " + action + " it.";
}

// The states of the note wait that a track can have. Each is a bit.
constexpr int kWaitOff = 1;
constexpr int kWaitOn = 2;

// The blocks of `code` that a track can go to from block `block`: a jump's or a call's target, and the next block
// unless the block's last command always moves the track elsewhere. A call's return and a loop's end going back aren't
// among them.
std::vector<std::size_t> NextBlocks(const Code& code, std::size_t block)
{
    const Command& last = (*code.items)[code.starts[block + 1] - 1];
    std::vector<std::size_t> next;
    if (last.kind == Kind::kData)
    {
        return next;
    }

    if (ntdlib::commands::HasTarget(last.kind) && last.kind != Kind::kOpenTrack && !last.target_outside)
    {
        next.push_back(BlockOf(code, last.target));
    }
    const bool moves =
        last.kind == Kind::kJump || last.kind == Kind::kCall || last.kind == Kind::kReturn || last.kind == Kind::kEnd;
    if ((!moves || last.conditional) && block + 2 < code.starts.size())
    {
        next.push_back(block + 1);
    }

    return next;
}

// Which blocks of `code` a track can reach from block `from`: a flag for each block. A call comes back to the block
// after it, and a return to the block after any call. `returns` holds the blocks after the calls. Without a loop's end
// going back, a track still reaches every block that it can: it ran the loop's start first.
std::vector<bool> Reach(const Code& code, std::size_t from, const std::vector<std::size_t>& returns)
{
    std::vector<bool> reached(code.starts.size() - 1, false);
    std::vector<std::size_t> pending = {from};
    while (!pending.empty())
    {
        const std::size_t block = pending.back();
        pending.pop_back();
        if (reached[block])
        {
            continue;
        }

        reached[block] = true;
        const std::vector<std::size_t> next = NextBlocks(code, block);
        pending.insert(pending.end(), next.begin(), next.end());
        const Command& last = (*code.items)[code.starts[block + 1] - 1];
        if (last.kind == Kind::kCall && block + 2 < code.starts.size())
        {
            pending.push_back(block + 1);
        }
        if (last.kind == Kind::kReturn)
        {
            pending.insert(pending.end(), returns.begin(), returns.end());
        }
    }

    return reached;
}

// What a call to block `target` of `code` can do to the note wait: the blocks with a return that the subroutine can end
// at, and whether a command that it can run changes the note wait. The calls that the subroutine makes count too.
struct Subroutine
{
    std::vector<std::size_t> returns;
    bool changes = false;
};

Subroutine SubroutineAt(const Code& code, std::size_t target)
{
    const std::vector<Command>& items = *code.items;
    Subroutine subroutine;
    const std::vector<bool> reached = Reach(code, target, {});
    for (std::size_t block = 0; block < reached.size(); ++block)
    {
        if (!reached[block])
        {
            continue;
        }

        for (uint32_t i = code.starts[block]; i < code.starts[block + 1]; ++i)
        {
            subroutine.changes = subroutine.changes || items[i].kind == Kind::kNoteWait;
        }
        if (items[code.starts[block + 1] - 1].kind == Kind::kReturn)
        {
            subroutine.returns.push_back(block);
        }
    }

    return subroutine;
}

// The states of the note wait at the end of each block of `code`, from the states with which tracks enter the blocks.
std::vector<int> LeavingStates(const Code& code, const std::vector<int>& entering)
{
    std::vector<int> leaving(entering.size(), 0);
    for (std::size_t block = 0; block < entering.size(); ++block)
    {
        int state = entering[block];
        for (uint32_t i = code.starts[block]; i < code.starts[block + 1] && state != 0; ++i)
        {
            const Command& command = (*code.items)[i];
            if (command.kind == Kind::kNoteWait)
            {
                const int set = command.value.source != Source::kValue ? kWaitOff | kWaitOn
                                : command.value.value != 0             ? kWaitOn
                                                                       : kWaitOff;
                state = command.conditional ? state | set : set;
            }
        }
        leaving[block] = state;
    }

    return leaving;
}

// The states of the note wait with which a track can enter each block of `code`, sequence `sequence` of `file`. A track
// opens with the note wait on: at a sound's start, where a command opens it, and with `labels` at a label and at the
// first item too. A subroutine starts with the state at its call. A call comes back with the state at the call when its
// subroutine leaves the note wait alone, and else with any state at the subroutine's returns. A loop's end goes back
// with its state to each loop's start from which a track can reach it. A block that no track reaches has no states.
std::vector<int> EntryStates(const format::File& file, uint32_t sequence, const Code& code, bool labels = true)
{
    const std::vector<Command>& items = *code.items;
    const std::size_t blocks = code.starts.size() - 1;
    std::vector<int> states(blocks, 0);
    const auto open = [&](uint32_t item)
    {
        if (item < items.size())
        {
            states[BlockOf(code, item)] |= kWaitOn;
        }
    };
    if (labels)
    {
        for (const ntdlib::commands::Label& label : file.sequences[sequence]->labels)
        {
            open(label.item);
        }
        ForEachItemPlace(*file.native, sequence, [&open](const ntdlib::sar::Place& place) { open(place.offset); });
    }
    else
    {
        // A field that names an item measures a sound's start from the first item.
        for (const ntdlib::sar::Field& field : file.native->fields)
        {
            if (NamesItemOf(*file.native, field.to, sequence))
            {
                open(field.to.offset);
            }
        }
    }
    std::vector<std::size_t> loops;
    std::vector<std::size_t> returns;
    for (uint32_t i = 0; i < items.size(); ++i)
    {
        if (items[i].kind == Kind::kOpenTrack && !items[i].target_outside)
        {
            open(items[i].target);
        }
        if (items[i].kind == Kind::kLoopStart && i + 1 < items.size())
        {
            loops.push_back(BlockOf(code, i + 1));
        }
        if (items[i].kind == Kind::kCall && i + 1 < items.size())
        {
            returns.push_back(BlockOf(code, i + 1));
        }
    }

    // Each subroutine that a call goes to, once, and the loops' starts that each loop's end can go back to: those from
    // which a track can reach the end.
    std::map<std::size_t, Subroutine> subroutines;
    for (const Command& item : items)
    {
        if (item.kind == Kind::kCall && !item.target_outside)
        {
            const std::size_t target = BlockOf(code, item.target);
            if (!subroutines.contains(target))
            {
                subroutines.emplace(target, SubroutineAt(code, target));
            }
        }
    }
    std::map<std::size_t, std::vector<std::size_t>> backs;
    for (const std::size_t loop : loops)
    {
        const std::vector<bool> reached = Reach(code, loop, returns);
        for (std::size_t block = 0; block < blocks; ++block)
        {
            if (reached[block] && items[code.starts[block + 1] - 1].kind == Kind::kLoopEnd)
            {
                backs[block].push_back(loop);
            }
        }
    }

    // The states flow from block to block until they stop changing.
    bool changed = true;
    while (changed)
    {
        changed = false;

        const auto flow = [&](std::size_t block, int entering)
        {
            if ((states[block] | entering) != states[block])
            {
                states[block] |= entering;
                changed = true;
            }
        };

        const std::vector<int> leaving = LeavingStates(code, states);
        for (std::size_t block = 0; block < blocks; ++block)
        {
            if (leaving[block] == 0)
            {
                continue;
            }

            for (const std::size_t next : NextBlocks(code, block))
            {
                flow(next, leaving[block]);
            }
            if (const auto back = backs.find(block); back != backs.end())
            {
                for (const std::size_t loop : back->second)
                {
                    flow(loop, leaving[block]);
                }
            }
            const Command& last = items[code.starts[block + 1] - 1];
            if (last.kind != Kind::kCall || block + 1 >= blocks)
            {
                continue;
            }

            int back = kWaitOff | kWaitOn;
            if (!last.target_outside)
            {
                const Subroutine& subroutine = subroutines.at(BlockOf(code, last.target));
                back = subroutine.changes ? 0 : leaving[block];
                if (subroutine.changes)
                {
                    for (const std::size_t end : subroutine.returns)
                    {
                        back |= leaving[end];
                    }
                }
            }
            flow(block + 1, back);
        }
    }

    return states;
}

// A run of a block's commands on a track whose notes wait for their length: items [`begin`, `end`) between the notes
// that a wait can't take the place of (WrittenWait). `start` is the run's tick from the block's start, or nothing when
// a note before it waits for an unknown time. `ticks` holds the tick of each item from the run's start, then the run's
// length.
struct Run
{
    uint32_t begin = 0;
    uint32_t end = 0;
    std::optional<uint32_t> start;
    std::vector<uint32_t> ticks;
};

// The runs of block `block`, in order. The block's last command stays out of them when it moves the track elsewhere.
std::vector<Run> RunsOf(const Code& code, std::size_t block)
{
    const std::vector<Command>& items = *code.items;
    const uint32_t begin = code.starts[block];
    const uint32_t end = code.starts[block + 1];
    const uint32_t tail = end > begin && EndsBlock(items[end - 1]) ? end - 1 : end;
    std::vector<Run> runs;
    Run run{begin, begin, 0, {}};
    uint32_t tick = 0;
    const auto close = [&](uint32_t at)
    {
        if (at > run.begin)
        {
            run.end = at;
            run.ticks.push_back(tick);
            runs.push_back(std::move(run));
        }
    };
    for (uint32_t i = begin; i < tail; ++i)
    {
        const Command& command = items[i];
        if (command.kind == Kind::kNote && !WrittenWait(command))
        {
            // The time after the note is unknown.
            close(i);
            run = Run{i + 1, i + 1, std::nullopt, {}};
            tick = 0;
            continue;
        }

        run.ticks.push_back(tick);
        tick += command.kind == Kind::kNote ? *WrittenWait(command) : WaitTicks(command).value_or(0);
    }
    close(tail);

    return runs;
}

// The run of block `block` that holds `item`, or nothing.
std::optional<Run> RunOf(const Code& code, std::size_t block, uint32_t item)
{
    for (Run& run : RunsOf(code, block))
    {
        if (item >= run.begin && item < run.end)
        {
            return std::move(run);
        }
    }

    return std::nullopt;
}

// The tick at which each block that `stretch` runs starts. The blocks stop where the time becomes unknown: at a wait
// that the bytecode doesn't give, at a change of the note wait that it doesn't give, and at a note that waits for a
// length that it doesn't give.
std::map<std::size_t, uint32_t> BlockStarts(const Code& code, const song::Stretch& stretch)
{
    const uint32_t first = ItemAt(code, stretch.first);
    const uint32_t last = ItemAt(code, stretch.last);
    std::map<std::size_t, uint32_t> starts;
    uint32_t tick = stretch.tick;
    bool note_wait = stretch.note_wait;
    for (std::size_t block = BlockOf(code, first); block + 1 < code.starts.size() && code.starts[block] <= last;
         ++block)
    {
        starts[block] = tick;
        for (uint32_t i = std::max(code.starts[block], first); i < code.starts[block + 1]; ++i)
        {
            const Command& command = (*code.items)[i];
            const bool waiting_note = command.kind == Kind::kNote && note_wait;
            const bool unknown_wait = command.kind == Kind::kNoteWait
                                          ? command.conditional || command.value.source != Source::kValue
                                          : Barrier(command);
            if (unknown_wait || (waiting_note && !NoteTicks(command)))
            {
                return starts;
            }

            note_wait = command.kind == Kind::kNoteWait ? command.value.value != 0 : note_wait;
            tick += waiting_note ? *NoteTicks(command) : WaitTicks(command).value_or(0);
        }
    }

    return starts;
}

// A wait of `ticks` in place of `wait`, with the encoder's usual bytes for its length.
Command WaitOf(const Command& wait, uint32_t ticks)
{
    Command command = wait;
    command.value = Argument{Source::kValue, static_cast<int64_t>(ticks) * kOne, 0};
    command.encoding.value_bytes = 0;

    return command;
}

// What an edit does to a block: the notes that it takes out, by item, and the commands that it puts in, at their ticks
// from the block's start. A moved note's item comes with it, and a new command's place among the edit's new commands.
struct BlockChange
{
    struct Placed
    {
        uint32_t tick = 0;
        Command command;
        std::optional<uint32_t> item;
        std::optional<std::size_t> addition;
    };

    std::set<uint32_t> removed;
    std::vector<Placed> placed;
    bool extend = false; // a note goes past the end of a timeline's track, and the track lasts to the note's end
};

// Rewrites block [`begin`, `end`) of `items` with `change` onto the end of `out`. `edited` takes the new index of each
// item that stays, of each moved note and of each new command. A note goes in before the wait at its tick, after the
// other commands at that tick. A new setting goes in before the first note at its tick. Inside a wait, a new command
// splits the wait in two. At the block's end, it goes before a last command that moves the track elsewhere.
void Rewrite(const std::vector<Command>& items, uint32_t begin, uint32_t end, BlockChange change,
             std::vector<Command>& out, Edited& edited)
{
    std::stable_sort(change.placed.begin(), change.placed.end(),
                     [](const BlockChange::Placed& a, const BlockChange::Placed& b) { return a.tick < b.tick; });
    std::vector<std::optional<uint32_t>>& moved = edited.items;
    std::size_t next = 0;
    const auto place = [&]
    {
        const BlockChange::Placed& placed = change.placed[next++];
        if (placed.item)
        {
            moved[*placed.item] = static_cast<uint32_t>(out.size());
        }
        if (placed.addition)
        {
            edited.added[*placed.addition] = static_cast<uint32_t>(out.size());
        }
        out.push_back(placed.command);
    };

    // The commands up to the last, with the notes at their ticks.
    const uint32_t tail = end > begin && EndsBlock(items[end - 1]) ? end - 1 : end;
    uint32_t tick = 0;
    for (uint32_t i = begin; i < tail; ++i)
    {
        if (change.removed.contains(i))
        {
            continue;
        }

        const std::optional<uint32_t> wait = WaitTicks(items[i]);
        if (!wait || *wait == 0)
        {
            while (items[i].kind == Kind::kNote && next < change.placed.size() && change.placed[next].tick == tick &&
                   change.placed[next].command.kind != Kind::kNote)
            {
                place();
            }
            moved[i] = static_cast<uint32_t>(out.size());
            out.push_back(items[i]);
            continue;
        }

        uint32_t from = tick;
        const uint32_t to = tick + *wait;
        while (next < change.placed.size() && change.placed[next].tick < to)
        {
            if (change.placed[next].tick > from)
            {
                out.push_back(WaitOf(items[i], change.placed[next].tick - from));
                from = change.placed[next].tick;
            }
            place();
        }
        moved[i] = static_cast<uint32_t>(out.size());
        out.push_back(from == tick ? items[i] : WaitOf(items[i], to - from));
        tick = to;
    }

    // The notes at the block's end, then its last command. A note past the end of a timeline's track goes in after a
    // wait, and the track then lasts to the note's end (BlockChange::extend).
    Command rest;
    rest.kind = Kind::kWait;
    uint32_t until = tick;
    while (next < change.placed.size())
    {
        const BlockChange::Placed& placed = change.placed[next];
        if (change.extend && placed.tick > tick)
        {
            out.push_back(WaitOf(rest, placed.tick - tick));
            tick = placed.tick;
        }
        if (change.extend && placed.command.kind == Kind::kNote)
        {
            until = std::max(until, placed.tick + NoteTicks(placed.command).value_or(0));
        }
        place();
    }
    if (until > tick)
    {
        out.push_back(WaitOf(rest, until - tick));
    }
    for (uint32_t i = tail; i < end; ++i)
    {
        moved[i] = static_cast<uint32_t>(out.size());
        out.push_back(items[i]);
    }
}

// Checks a key or a velocity of a note command.
int CheckedByte(int value, const char* what)
{
    if (value < 0 || value > 127)
    {
        throw std::invalid_argument(std::string("A note's ") + what + " has to stay from 0 to 127.");
    }

    return value;
}

// The length of block `block` in ticks on a track whose notes wait for their length, or nothing when a note of the
// block waits for a time that the bytecode doesn't give.
std::optional<uint32_t> WaitingLength(const Code& code, std::size_t block)
{
    uint32_t ticks = 0;
    for (uint32_t i = code.starts[block]; i < code.starts[block + 1]; ++i)
    {
        const Command& command = (*code.items)[i];
        const std::optional<uint32_t> wait = command.kind == Kind::kNote ? WrittenWait(command) : WaitTicks(command);
        if (command.kind == Kind::kNote && !wait)
        {
            return std::nullopt;
        }

        ticks += wait.value_or(0);
    }

    return ticks;
}

// A command that an edit puts in at `tick` of `stretch`: a new note, or a new setting (NewSetting).
struct Addition
{
    song::Stretch stretch;
    uint32_t tick = 0;
    Command command;
};

// Where a new command goes: `tick` ticks into block `block`, and whether the track's notes wait for their length there.
struct Spot
{
    std::size_t block = 0;
    uint32_t tick = 0;
    std::optional<bool> note_wait;
};

// Whether block `block` runs a note at `tick` that a new setting can go in before. The block starts at tick `start`.
// With `note_wait`, its notes wait for their length, and only a note whose wait a wait of its own can take the place of
// counts. The new setting goes into the run of such a note (RunsToWrite).
bool RunsNoteAt(const Code& code, std::size_t block, uint32_t start, bool note_wait, uint32_t tick)
{
    uint32_t at = start;
    for (uint32_t i = code.starts[block]; i < code.starts[block + 1] && at <= tick; ++i)
    {
        const Command& command = (*code.items)[i];
        if (command.kind == Kind::kNote && at == tick)
        {
            return !note_wait || WrittenWait(command);
        }

        const bool waiting = command.kind == Kind::kNote && note_wait;
        const std::optional<uint32_t> wait = waiting ? WrittenWait(command) : WaitTicks(command);
        if (waiting && !wait)
        {
            return false;
        }

        at += wait.value_or(0);
    }

    return false;
}

// Where `addition` goes. Of the blocks that its stretch runs at its tick, the last one whose notes don't wait for their
// length takes it, or else the last one. A new setting goes into the first of them that runs a note at its tick
// instead. It goes in before the note, and the note plays with it. A barrier takes no command.
std::optional<Spot> SpotOf(const Code& code, const Addition& addition)
{
    std::optional<Spot> found;
    for (const auto& [block, start] : BlockStarts(code, addition.stretch))
    {
        if (Barrier((*code.items)[code.starts[block]]) || addition.tick < start)
        {
            continue;
        }

        const std::optional<bool> note_wait = NoteWaitAt(code, addition.stretch, code.starts[block]);
        const std::optional<uint32_t> length =
            note_wait == true ? WaitingLength(code, block) : BlockTicks(code, block).back();
        if (length && addition.tick > start + *length)
        {
            continue;
        }

        if (addition.command.kind == Kind::kNote && length && addition.tick == start + *length &&
            EndsAt(code, TailOf(code, block)))
        {
            continue;
        }

        const Spot spot{block, addition.tick - start, note_wait};
        if (addition.command.kind != Kind::kNote && RunsNoteAt(code, block, start, note_wait == true, addition.tick))
        {
            return spot;
        }
        if (note_wait == false || !found || found->note_wait != false)
        {
            found = spot;
        }
    }

    return found;
}

// How far a note of a timeline can move past the end of a track that ends there.
constexpr uint32_t kFarLater = 1u << 24;

// The end of a stretch of a timeline's track, from the ticks of its blocks (BlockStarts): its last block, the tick
// where that block starts and the tick where it ends. A track that loops jumps back to the loop's start there, and one
// that ends there ends.
struct StretchEnd
{
    std::size_t block = 0;
    uint32_t start = 0;
    uint32_t tick = 0;
    bool loops = false;
    bool ends = false;
};

StretchEnd EndOf(const Code& code, const std::map<std::size_t, uint32_t>& starts)
{
    const auto& [block, start] = *starts.rbegin();
    const uint32_t tail = TailOf(code, block);
    const bool jump =
        tail < code.starts[block + 1] && (*code.items)[tail].kind == Kind::kJump && !(*code.items)[tail].conditional;
    return {block, start, start + BlockTicks(code, block).back(), jump, EndsAt(code, tail)};
}

// Where a command of a timeline goes at `addition`'s tick: in the block of its stretch that runs at the tick (SpotOf),
// in any block of the track before the loop's end. A note past the end of a track that ends there goes after the
// stretch's last block, and the track then lasts to the note's end (the second value). `song_end` gives the tick where
// the song's looping tracks have each gone back to their loop's start once (TimelineSongEnd). A track that ends takes
// no command at or after it. Where the header doesn't give that tick, a note of a track that ends stays inside it.
// Throws std::invalid_argument with a message for the user for a tick before the stretch's start, at or after the
// loop's end, past the end of a track that ends where the song's end is unknown, or where no block of the stretch takes
// the command.
std::pair<Spot, bool> TimelineSpotOf(const Code& code, const Addition& addition, const SongEnd& song_end)
{
    const bool note = addition.command.kind == Kind::kNote;
    const std::map<std::size_t, uint32_t> starts = BlockStarts(code, addition.stretch);
    if (starts.empty() || addition.tick < addition.stretch.tick)
    {
        throw std::invalid_argument(note ? "A note can't go before the start of its track."
                                         : "A command can't go before the start of its track.");
    }
    const StretchEnd end = EndOf(code, starts);
    if (end.loops && addition.tick >= end.tick)
    {
        throw std::invalid_argument(note ? "A note can't start at or after the loop's end. The track jumps back to the "
                                           "loop's start there."
                                         : "A command can't go at or after the loop's end. The track jumps back to the "
                                           "loop's start there.");
    }
    if (!end.loops && song_end.tick && addition.tick >= *song_end.tick)
    {
        throw std::invalid_argument(note ? "A note can't start at or after the loop's end. The song goes back to the "
                                           "loop's start there."
                                         : "A command can't go at or after the loop's end. The song goes back to the "
                                           "loop's start there.");
    }
    if (const std::optional<Spot> spot = SpotOf(code, addition); spot && spot->note_wait == false)
    {
        return {*spot, false};
    }
    if (note && end.ends && addition.tick >= end.tick)
    {
        if (song_end.loops && !song_end.tick)
        {
            throw std::invalid_argument(
                "A note can't go past the end of its track in this song. Hokuto can't tell "
                "where the song goes back to its loop's start.");
        }
        return {Spot{end.block, addition.tick - end.start, false}, true};
    }

    throw std::invalid_argument(note ? "A new note can't go at that tick on this track."
                                     : "A new command can't go at that tick on this track.");
}

// The file with `rewritten` in place of the items of sequence `sequence`. Block `i` of `code` now starts at item
// `new_starts[i]`, and the last of `new_starts` is the end of the items. The targets, the labels and the sounds' starts
// move with the blocks that they start.
format::File WithItems(const format::File& file, uint32_t sequence, const Code& code, std::vector<Command> rewritten,
                       const std::vector<uint32_t>& new_starts)
{
    const auto renumber = [&](uint32_t item)
    {
        const auto found = std::lower_bound(code.starts.begin(), code.starts.end(), item);
        if (found == code.starts.end() || *found != item)
        {
            throw std::logic_error("an item that other code enters starts no block");
        }

        return new_starts[static_cast<std::size_t>(found - code.starts.begin())];
    };
    for (Command& item : rewritten)
    {
        if (ntdlib::commands::HasTarget(item.kind) && !item.target_outside)
        {
            item.target = renumber(item.target);
        }
    }
    ntdlib::commands::Sequence result{std::move(rewritten), file.sequences[sequence]->labels};
    for (ntdlib::commands::Label& label : result.labels)
    {
        label.item = renumber(label.item);
    }

    format::File edited = file;
    edited.sequences[sequence] = std::move(result);
    const auto renumber_place = [&renumber](ntdlib::sar::Place& place)
    {
        place.offset = renumber(place.offset);
    };
    edited.native.Edit([&](format::NativeArchive& native) { ForEachItemPlace(native, sequence, renumber_place); });

    return edited;
}

// A sequence after WriteWaits: the edit, and for each item, the item that code entering there now reaches.
struct Written
{
    Edited edited;
    std::vector<uint32_t> entries;
};

// The file after runs `runs` of sequence `sequence`, by the item where each begins, turn the note wait off. Each run
// turns the note wait off at its start and back on at its end, and each of its notes waits out its length with a wait
// of its own. Every command keeps its tick. Code that entered a run at its start now enters where the note wait turns
// off.
Written WriteWaits(const format::File& file, uint32_t sequence, const Code& code, const std::map<uint32_t, Run>& runs)
{
    const std::vector<Command>& items = *code.items;
    Command off;
    off.kind = Kind::kNoteWait;
    Command on = off;
    on.value = Argument{Source::kValue, kOne, 0};
    Command wait;
    wait.kind = Kind::kWait;

    Written written;
    std::vector<std::optional<uint32_t>>& moved = written.edited.items;
    moved.resize(items.size());
    std::vector<Command> rewritten;
    std::vector<uint32_t> new_starts;
    std::size_t next = 0;
    for (uint32_t i = 0; i < items.size(); ++i)
    {
        if (code.starts[next] == i)
        {
            new_starts.push_back(static_cast<uint32_t>(rewritten.size()));
            ++next;
        }

        const auto run = runs.find(i);
        if (run == runs.end())
        {
            moved[i] = static_cast<uint32_t>(rewritten.size());
            rewritten.push_back(items[i]);
            continue;
        }

        rewritten.push_back(off);
        for (; i < run->second.end; ++i)
        {
            moved[i] = static_cast<uint32_t>(rewritten.size());
            rewritten.push_back(items[i]);
            if (items[i].kind == Kind::kNote)
            {
                rewritten.push_back(WaitOf(wait, *WrittenWait(items[i])));
            }
        }
        rewritten.push_back(on);
        --i;
    }
    new_starts.push_back(static_cast<uint32_t>(rewritten.size()));

    written.entries.resize(items.size());
    for (uint32_t i = 0; i < items.size(); ++i)
    {
        written.entries[i] = *moved[i];
    }
    for (std::size_t block = 0; block + 1 < code.starts.size(); ++block)
    {
        written.entries[code.starts[block]] = new_starts[block];
    }
    written.edited.file = WithItems(file, sequence, code, std::move(rewritten), new_starts);

    return written;
}

// The runs that `named` and `additions` require to turn the note wait off, by the item where each begins. They're those
// of a move, a new length and a new command on a track whose notes wait for their length. Throws std::invalid_argument
// for a note whose wait no wait of its own can take the place of, and for a note that other code may run with the note
// wait off.
std::map<uint32_t, Run> RunsToWrite(const format::File& file, uint32_t sequence, const Code& code,
                                    const std::map<uint32_t, const NoteEdit*>& named,
                                    std::span<const Addition> additions)
{
    std::optional<std::vector<int>> states;
    const auto entering = [&](std::size_t block)
    {
        if (!states)
        {
            states = EntryStates(file, sequence, code);
        }

        return (*states)[block];
    };

    std::optional<std::vector<int>> started;
    const auto starting = [&](std::size_t block)
    {
        if (!started)
        {
            started = EntryStates(file, sequence, code, false);
        }

        return (*started)[block];
    };

    std::map<uint32_t, Run> runs;
    for (const auto& [item, edit] : named)
    {
        const bool timing = edit->length || edit->shift != 0;
        if (!timing && !edit->remove)
        {
            continue;
        }

        // A note that the stretch runs with the note wait off waits for its length on a track that runs it with the
        // note wait on, such as a sound that starts inside the stretch.
        const Command& note = (*code.items)[item];
        const std::optional<bool> note_wait = NoteWaitAt(code, edit->stretch, item);
        if (note_wait == false && note.kind == Kind::kNote && (starting(BlockOf(code, item)) & kWaitOn) != 0)
        {
            throw std::invalid_argument(timing ? "Other code may run this note with the note wait on. Hokuto can't "
                                                 "move or resize it."
                                               : "Other code may run this note with the note wait on. Hokuto can't "
                                                 "remove it.");
        }
        if (!timing || note_wait != true)
        {
            continue;
        }

        if (note.kind == Kind::kNote && !WrittenWait(note))
        {
            throw std::invalid_argument(WaitRefusal(note, "move or resize"));
        }
        const std::size_t block = BlockOf(code, item);
        if (entering(block) != kWaitOn)
        {
            throw std::invalid_argument(
                note.kind == Kind::kNote
                    ? "Other code may run this note with the note wait off. Hokuto can't move or resize it."
                    : "Other code may run this command with the note wait off. Hokuto can't move it.");
        }

        Run run = *RunOf(code, block, item);
        runs.emplace(run.begin, std::move(run));
    }

    // A new command goes into the run that holds its tick.
    for (const Addition& addition : additions)
    {
        const std::optional<Spot> spot = SpotOf(code, addition);
        if (spot && spot->note_wait == false && addition.command.kind == Kind::kNote &&
            (starting(spot->block) & kWaitOn) != 0)
        {
            throw std::invalid_argument(
                "Other code may run the commands at this tick with the note wait on. Hokuto can't put a note in "
                "there.");
        }
        if (!spot || spot->note_wait != true || entering(spot->block) != kWaitOn)
        {
            continue;
        }

        std::optional<Run> found;
        for (Run& run : RunsOf(code, spot->block))
        {
            if (run.start && spot->tick >= *run.start && spot->tick <= *run.start + run.ticks.back())
            {
                found = std::move(run);
            }
        }
        if (found)
        {
            const uint32_t begin = found->begin;
            runs.emplace(begin, std::move(*found));
        }
    }

    return runs;
}

// The edit of EditNotes, AddSettings and MoveSettings. With `settings`, the edits name settings, and only move them.
// Unless `written`, a move, a new length or a new command on a track whose notes wait for their length first turns the
// note wait off in the run that it changes (WriteWaits).
Edited EditNotesIn(const format::File& file, uint32_t sequence, std::span<const NoteEdit> edits,
                   std::span<const Addition> additions, bool written, bool settings);

// The edit of EditNotesIn after the runs `runs` of `code`, sequence `sequence` of `file`, turn the note wait off
// (WriteWaits). The edits and the new commands follow their commands and stretches into the new version. The new index
// of each item follows both steps.
Edited EditWritten(const format::File& file, uint32_t sequence, const Code& code, const std::map<uint32_t, Run>& runs,
                   std::span<const NoteEdit> edits, std::span<const Addition> additions, bool settings)
{
    const Written written = WriteWaits(file, sequence, code, runs);
    const format::File& middle = written.edited.file;
    const std::vector<uint32_t> offsets = Offsets(*middle.sequences[sequence], ConsoleOf(middle));
    const auto stretch_after = [&](const song::Stretch& stretch)
    {
        song::Stretch after = stretch;
        after.first = offsets[written.entries[ItemAt(code, stretch.first)]];
        after.last = offsets[*written.edited.items[ItemAt(code, stretch.last)]];
        return after;
    };

    std::vector<NoteEdit> moved_edits(edits.begin(), edits.end());
    for (NoteEdit& edit : moved_edits)
    {
        edit.command = offsets[*written.edited.items[EditedAt(code, edit.command, settings)]];
        edit.stretch = stretch_after(edit.stretch);
    }
    std::vector<Addition> moved_additions(additions.begin(), additions.end());
    for (Addition& addition : moved_additions)
    {
        addition.stretch = stretch_after(addition.stretch);
    }

    Edited edited = EditNotesIn(middle, sequence, moved_edits, moved_additions, true, settings);
    std::vector<std::optional<uint32_t>> items(code.items->size());
    for (std::size_t i = 0; i < items.size(); ++i)
    {
        items[i] = edited.items[*written.edited.items[i]];
    }
    edited.items = std::move(items);

    return edited;
}

Edited EditNotesIn(const format::File& file, uint32_t sequence, std::span<const NoteEdit> edits,
                   std::span<const Addition> additions, bool written, bool settings)
{
    const Code code = Examine(file, sequence);
    std::vector<Command> items = *code.items;
    const bool timeline = IsTimeline(file, sequence);
    const SongEnd song_end = TimelineSongEnd(file, sequence);

    // Each command once, with the first edit that names it.
    std::map<uint32_t, const NoteEdit*> named;
    for (const NoteEdit& edit : edits)
    {
        named.emplace(EditedAt(code, edit.command, settings), &edit);
    }

    // On a track whose notes wait for their length, a move, a new length and a new command first turn the note wait off
    // in the runs that they change.
    if (!written)
    {
        const std::map<uint32_t, Run> runs = RunsToWrite(file, sequence, code, named, additions);
        if (!runs.empty())
        {
            if (code.fixed)
            {
                throw std::invalid_argument(
                    "This sequence's commands can't move. Only their keys and velocities can change.");
            }

            return EditWritten(file, sequence, code, runs, edits, additions, settings);
        }
    }

    // The changes to each command. A move takes the note out of its block and puts it back at its new tick.
    std::map<std::size_t, BlockChange> blocks;
    bool resized = false;
    for (const auto& [item, edit] : named)
    {
        Command& note = items[item];
        if (edit->transpose != 0)
        {
            note.key = static_cast<uint8_t>(CheckedByte(note.key + edit->transpose, "key"));
        }
        if (edit->velocity)
        {
            note.velocity = static_cast<int64_t>(CheckedByte(*edit->velocity, "velocity")) * kOne;
        }
        if (!edit->length && edit->shift == 0 && !edit->remove)
        {
            continue;
        }

        // A note that waits for its length leaves a wait of that length when it goes. The wait keeps the note's
        // condition: a note whose comparison failed doesn't wait.
        const std::optional<bool> note_wait = NoteWaitAt(code, edit->stretch, item);
        if (note_wait.value_or(true) && settings)
        {
            throw std::invalid_argument(note_wait ? "Other code may run this command with the note wait on. Hokuto "
                                                    "can't move it."
                                                  : "A command before this one changes the note wait as the track "
                                                    "plays. Hokuto can't move it.");
        }
        if (note_wait.value_or(true))
        {
            if (edit->remove && !edit->length && edit->shift == 0 && note_wait && NoteTicks(note))
            {
                Command wait;
                wait.kind = Kind::kWait;
                wait.conditional = note.conditional;
                note = WaitOf(wait, *NoteTicks(note));
                resized = true;
                continue;
            }

            const bool removal = edit->remove && !edit->length && edit->shift == 0;
            throw std::invalid_argument(note_wait ? WaitRefusal(note, removal ? "remove" : "move or resize")
                                                  : "A command before this note changes the note wait as the track "
                                                    "plays. Hokuto can't move or resize the note.");
        }

        resized = true;
        if (edit->length)
        {
            note.value = Argument{Source::kValue, static_cast<int64_t>(*edit->length) * kOne, 0};
            note.encoding.value_bytes = 0;
        }

        const std::size_t block = BlockOf(code, item);
        if (edit->remove)
        {
            blocks[block].removed.insert(item);
            continue;
        }
        if (edit->shift != 0)
        {
            const std::vector<uint32_t> ticks = BlockTicks(code, block);
            const int64_t tick = static_cast<int64_t>(ticks[item - code.starts[block]]) + edit->shift;
            const std::map<std::size_t, uint32_t> starts =
                timeline ? BlockStarts(code, edit->stretch) : std::map<std::size_t, uint32_t>();
            if (const auto start = starts.find(block); start != starts.end())
            {
                // A timeline's note moves to any tick of its track before the loop's end (TimelineSpotOf).
                const int64_t at = static_cast<int64_t>(start->second) + tick;
                if (at < 0)
                {
                    throw std::invalid_argument(settings ? "A command can't go before the start of its track."
                                                         : "A note can't go before the start of its track.");
                }
                const auto [spot, past_end] =
                    TimelineSpotOf(code, Addition{edit->stretch, static_cast<uint32_t>(at), note}, song_end);
                blocks[block].removed.insert(item);
                BlockChange& change = blocks[spot.block];
                change.placed.push_back({spot.tick, note, item, std::nullopt});
                change.extend = change.extend || past_end;
                continue;
            }
            if (tick < 0 || tick > static_cast<int64_t>(ticks.back()))
            {
                throw std::invalid_argument(settings ? "A command can't move out of its part of the track's commands."
                                                     : "A note can't move out of its part of the track's commands.");
            }
            if (!settings && tick == static_cast<int64_t>(ticks.back()) && EndsAt(code, TailOf(code, block)))
            {
                throw std::invalid_argument("A note can't start in the tick where its track ends. It wouldn't sound.");
            }

            blocks[block].removed.insert(item);
            blocks[block].placed.push_back({static_cast<uint32_t>(tick), note, item, std::nullopt});
        }
    }

    // Each new command goes into the block that runs at its tick (SpotOf).
    for (std::size_t index = 0; index < additions.size(); ++index)
    {
        const Addition& addition = additions[index];
        if (timeline)
        {
            const auto [spot, past_end] = TimelineSpotOf(code, addition, song_end);
            BlockChange& change = blocks[spot.block];
            change.placed.push_back({spot.tick, addition.command, std::nullopt, index});
            change.extend = change.extend || past_end;
            resized = true;
            continue;
        }
        const std::optional<Spot> spot = SpotOf(code, addition);
        if (!spot || spot->note_wait != false)
        {
            throw std::invalid_argument(addition.command.kind == Kind::kNote
                                            ? "A new note can't go at that tick on this track."
                                            : "A new command can't go at that tick on this track.");
        }

        blocks[spot->block].placed.push_back({spot->tick, addition.command, std::nullopt, index});
        resized = true;
    }

    if (resized && code.fixed)
    {
        throw std::invalid_argument("This sequence's commands can't move. Only their keys and velocities can change.");
    }

    // The blocks in order, each one rewritten or as it was.
    Edited edited;
    edited.items.resize(items.size());
    edited.added.resize(additions.size());
    std::vector<Command> rewritten;
    std::vector<uint32_t> new_starts;
    for (std::size_t block = 0; block + 1 < code.starts.size(); ++block)
    {
        new_starts.push_back(static_cast<uint32_t>(rewritten.size()));
        const auto change = blocks.find(block);
        if (change != blocks.end())
        {
            Rewrite(items, code.starts[block], code.starts[block + 1], std::move(change->second), rewritten, edited);
            continue;
        }

        for (uint32_t i = code.starts[block]; i < code.starts[block + 1]; ++i)
        {
            edited.items[i] = static_cast<uint32_t>(rewritten.size());
            rewritten.push_back(items[i]);
        }
    }
    new_starts.push_back(static_cast<uint32_t>(rewritten.size()));

    edited.file = WithItems(file, sequence, code, std::move(rewritten), new_starts);

    return edited;
}

} // namespace

Edited EditNotes(const format::File& file, uint32_t sequence, std::span<const NoteEdit> edits,
                 std::span<const NewNote> additions)
{
    std::vector<Addition> notes;
    for (const NewNote& addition : additions)
    {
        Command note;
        note.kind = Kind::kNote;
        note.key = static_cast<uint8_t>(CheckedByte(addition.key, "key"));
        note.velocity = static_cast<int64_t>(CheckedByte(addition.velocity, "velocity")) * kOne;
        note.value = Argument{Source::kValue, static_cast<int64_t>(addition.length) * kOne, 0};
        notes.push_back({addition.stretch, addition.tick, std::move(note)});
    }

    return EditNotesIn(file, sequence, edits, notes, false, false);
}

Edited AddSettings(const format::File& file, uint32_t sequence, std::span<const NewSetting> settings)
{
    std::vector<Addition> additions;
    for (const NewSetting& setting : settings)
    {
        if (!IsSetting(setting.command))
        {
            throw std::invalid_argument("Only a setting, such as a volume, goes in at a tick.");
        }
        if (file.native && !song::Runs(ConsoleOf(file), setting.command.kind, setting.command.index))
        {
            throw std::invalid_argument(
                "The 3DS's sequence player skips such a setting. Its tracks have one LFO, without a curve, a phase or "
                "a period, and send to FX A and FX B only.");
        }

        additions.push_back({setting.stretch, setting.tick, setting.command});
    }

    Edited edited = EditNotesIn(file, sequence, {}, additions, false, false);

    // The console's bytes have to hold the new commands.
    try
    {
        Offsets(*edited.file.sequences[sequence], ConsoleOf(edited.file));
    }
    catch (const std::invalid_argument&)
    {
        throw std::invalid_argument("The console's sequences can't hold this command's values.");
    }

    return edited;
}

Edited MoveSettings(const format::File& file, uint32_t sequence, std::span<const SettingMove> moves)
{
    std::vector<NoteEdit> edits;
    for (const SettingMove& move : moves)
    {
        edits.push_back({.command = move.command, .stretch = move.stretch, .shift = move.shift});
    }

    return EditNotesIn(file, sequence, edits, {}, false, true);
}

std::optional<song::Stretch> StretchAt(std::span<const song::Stretch> stretches, uint32_t tick)
{
    std::optional<song::Stretch> found;
    for (const song::Stretch& stretch : stretches)
    {
        if (stretch.tick <= tick)
        {
            found = stretch;
        }
    }

    return found;
}

std::optional<ShiftRange> ShiftRangeOf(const format::File& file, uint32_t sequence, uint32_t command,
                                       const song::Stretch& stretch)
{
    const NotePlace place{command, stretch};
    return LimitsOf(file, sequence, std::span(&place, 1)).front().shift;
}

std::vector<NoteLimits> LimitsOf(const format::File& file, uint32_t sequence, std::span<const NotePlace> notes,
                                 std::optional<uint32_t> loop_end)
{
    const Code code = Examine(file, sequence);
    const bool timeline = IsTimeline(file, sequence);
    const SongEnd song_end = TimelineSongEnd(file, sequence);
    std::optional<std::vector<int>> states;
    std::optional<std::vector<int>> started;
    std::vector<NoteLimits> limits;
    for (const NotePlace& note : notes)
    {
        // A note moves inside its block. On a track whose notes wait for their length, it moves inside its run, once
        // waits of their own take the place of the notes' waits (WriteWaits). Every track has to enter the block with
        // the note wait on. Otherwise no track may run the note with the note wait on: its wait would follow its
        // length. A setting moves the same way, whatever the note wait of other tracks.
        NoteLimits& limit = limits.emplace_back();
        const uint32_t item = ItemAt(code, note.command);
        const Command& command = (*code.items)[item];
        const std::optional<bool> note_wait = NoteWaitAt(code, note.stretch, item);
        if (code.fixed || !note_wait || (command.kind != Kind::kNote && WhyFixedInTime(command)))
        {
            continue;
        }

        const std::size_t block = BlockOf(code, item);
        if (!*note_wait && command.kind == Kind::kNote)
        {
            if (!started)
            {
                started = EntryStates(file, sequence, code, false);
            }
            if (((*started)[block] & kWaitOn) != 0)
            {
                continue;
            }
        }
        std::vector<uint32_t> ticks;
        uint32_t tick = 0;
        uint32_t tail = TailOf(code, block);
        if (*note_wait)
        {
            if (!states)
            {
                states = EntryStates(file, sequence, code);
            }
            std::optional<Run> run = (*states)[block] == kWaitOn ? RunOf(code, block, item) : std::nullopt;
            if (!run)
            {
                continue;
            }

            tick = run->ticks[item - run->begin];
            tail = run->end;
            ticks = std::move(run->ticks);
        }
        else
        {
            ticks = BlockTicks(code, block);
            tick = ticks[item - code.starts[block]];
        }

        // A note can't start in the tick where its track ends (EndsAt).
        const bool ends = command.kind == Kind::kNote && ticks.back() > tick && EndsAt(code, tail);
        limit.shift = ShiftRange{tick, ticks.back() - tick - (ends ? 1 : 0)};

        // A timeline's note moves to any tick of its track before the loop's end, and past the end of a track that
        // ends, up to the song's end (TimelineSongEnd and TimelineSpotOf). Where the header doesn't give that tick, the
        // note stays before the tick where its track ends. Nothing moves to the end of the loop that the arrangement
        // shows, or past it.
        const std::map<std::size_t, uint32_t> starts =
            timeline && !*note_wait ? BlockStarts(code, note.stretch) : std::map<std::size_t, uint32_t>();
        if (const auto start = starts.find(block); start != starts.end())
        {
            const uint32_t at = start->second + tick;
            const StretchEnd end = EndOf(code, starts);
            const bool past_end = command.kind == Kind::kNote && end.ends && !(song_end.loops && !song_end.tick);
            uint32_t later = end.tick > at ? end.tick - at : 0;
            if (end.loops || (command.kind == Kind::kNote && end.ends && !past_end))
            {
                later = later > 0 ? later - 1 : 0;
            }
            else if (past_end)
            {
                later = kFarLater;
            }
            if (!end.loops && song_end.tick)
            {
                later = std::min(later, *song_end.tick > at ? *song_end.tick - at - 1 : 0);
            }
            if (loop_end)
            {
                later = std::min(later, *loop_end > at ? *loop_end - at - 1 : 0);
            }
            limit.shift = ShiftRange{at - note.stretch.tick, later};
        }
        const Argument& length = command.value;
        if (command.kind == Kind::kNote && length.source == Source::kValue && length.value >= 0 &&
            length.value % kOne == 0)
        {
            limit.length = static_cast<uint32_t>(length.value / kOne);
        }
    }

    return limits;
}

std::optional<std::string> WhyFixedInTime(const Command& command)
{
    if (!IsSetting(command) || command.kind == Kind::kTimebase || command.kind == Kind::kAllocateTracks)
    {
        return "Only a setting, such as a volume, moves to another tick.";
    }
    if (command.conditional)
    {
        return "Hokuto can't move a command that runs only if the last comparison held.";
    }
    if (UsesVariables(command))
    {
        return "Hokuto can't move a command that reads or changes a variable.";
    }

    return std::nullopt;
}

int CommandKey(const format::File& file, uint32_t sequence, uint32_t command)
{
    const Code code = Examine(file, sequence);
    return (*code.items)[NoteAt(code, command)].key;
}

std::vector<uint32_t> CommandsAfter(const format::File& file, uint32_t sequence, const Edited& edited,
                                    std::span<const uint32_t> commands)
{
    const Code code = Examine(file, sequence);
    const std::vector<Command>& items = edited.file.sequences.at(sequence)->items;
    const std::vector<uint32_t> offsets = Offsets(*edited.file.sequences[sequence], ConsoleOf(edited.file));

    // A removed note on a track whose notes wait for their length leaves a wait in its place.
    std::vector<uint32_t> after;
    const auto add = [&](std::optional<uint32_t> item)
    {
        if (item && items[*item].kind == Kind::kNote)
        {
            after.push_back(offsets[*item]);
        }
    };
    for (const uint32_t command : commands)
    {
        add(edited.items.at(ItemAt(code, command)));
    }
    for (const uint32_t item : edited.added)
    {
        add(item);
    }

    return after;
}

} // namespace hokuto::edit
