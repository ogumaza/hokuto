// SPDX-License-Identifier: MIT

// The blocks of a sequence's commands that the note edits read (edit/note_blocks.h).

#include "edit/note_blocks.h"

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
#include "edit/engine_edits.h"
#include "edit/konami.h"
#include "song/archive.h"
#include "song/driver.h"

namespace hokuto::edit
{

namespace
{

using note_detail::Addition;
using note_detail::Barrier;
using note_detail::BlockOf;
using note_detail::BlockStarts;
using note_detail::BlockTicks;
using note_detail::Code;
using note_detail::EndsAt;
using note_detail::EndsBlockIn;
using note_detail::ItemAt;
using note_detail::kDseLoopPoint;
using note_detail::kDseRepeatExit;
using note_detail::kDseWaitForVoices;
using note_detail::kWaitOff;
using note_detail::kWaitOn;
using note_detail::NoteAt;
using note_detail::NoteTicks;
using note_detail::NoteWaitAt;
using note_detail::Run;
using note_detail::RunsOf;
using note_detail::Spot;
using note_detail::TailOf;
using note_detail::WaitTicks;
using note_detail::WrittenWait;
using ntdlib::commands::Argument;
using ntdlib::commands::Command;
using ntdlib::commands::Kind;
using ntdlib::commands::kOne;
using ntdlib::commands::Source;

// Whether `command` moves the track elsewhere, or may: a jump, a call, a return, a loop's start or end, the end, and on
// the GBA a repeat and a jump after a comparison. The command after it starts a block.
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
    case Kind::kRepeat:
    case Kind::kJumpIfEqual:
    case Kind::kJumpIfNotEqual:
    case Kind::kJumpIfGreater:
    case Kind::kJumpIfGreaterOrEqual:
    case Kind::kJumpIfLessOrEqual:
    case Kind::kJumpIfLess:
        return true;
    default:
        return false;
    }
}

} // namespace

namespace note_detail
{

std::optional<uint32_t> WaitTicks(const Command& command)
{
    if (command.kind != Kind::kWait || command.conditional)
    {
        return std::nullopt;
    }

    return WholeTicks(command.value);
}

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

bool Barrier(const Command& command)
{
    return (command.kind == Kind::kWait && !WaitTicks(command)) || command.kind == Kind::kNoteWait ||
           command.kind == Kind::kData;
}

bool Barrier(const Code& code, const Command& command)
{
    return Barrier(command) ||
           (code.dse && command.kind == Kind::kOther && command.encoding.opcode == kDseWaitForVoices);
}

bool EndsBlockIn(const Code& code, const Command& command)
{
    return EndsBlock(command) ||
           (code.dse && command.kind == Kind::kOther &&
            (command.encoding.opcode == kDseLoopPoint || command.encoding.opcode == kDseRepeatExit));
}

} // namespace note_detail

namespace
{

// Whether the bytes of `driver`, one of the GBA's other drivers, hold the command that turns the note wait off and on.
// Quintet's driver has none: its notes always wait for their lengths.
bool WritesNoteWait(const song::DriverRules& driver)
{
    Command off;
    off.kind = Kind::kNoteWait;
    try
    {
        driver.ItemOffsets(driver.Alone(off));
    }
    catch (const std::invalid_argument&)
    {
        return false;
    }

    return true;
}

} // namespace

namespace note_detail
{

Code Examine(const format::File& file, uint32_t sequence)
{
    Code code;
    const ntdlib::commands::Sequence& commands = SequenceToEdit(file, sequence);
    const std::vector<Command>& items = commands.items;
    code.items = &items;
    code.offsets = Offsets(commands, ConsoleOf(file));
    // A target outside a GBA song is an address, and MusicPlayer2000 reads none of the song's data: its bytes can move.
    code.fixed = KeepsPlaces(file, commands);
    code.dse = ConsoleOf(file).engine == format::Engine::kDse;
    code.konami = KonamiOf(ConsoleOf(file));
    code.driver = ConsoleOf(file).driver;
    code.writes_note_wait = code.driver == nullptr || WritesNoteWait(*code.driver);

    // Other code enters a sequence at a target, a label, a sound's start, and the command after a jump or a call. A
    // barrier stands alone. On the DS, an allocation of tracks ends its block too. The driver reads it before track 0
    // runs, and track 0's first stretch starts after it. A run's new commands then go after it. On the later consoles,
    // the allocation is track 0's first command.
    const bool ds = ConsoleOf(file).engine == format::Engine::kDs;
    std::set<uint32_t> starts = {0, static_cast<uint32_t>(items.size())};
    for (uint32_t i = 0; i < items.size(); ++i)
    {
        const Command& item = items[i];
        if (ntdlib::commands::HasTarget(item.kind) && !item.target_outside)
        {
            starts.insert(item.target);
        }

        if (EndsBlockIn(code, item) || (ds && item.kind == Kind::kAllocateTracks))
        {
            starts.insert(i + 1);
        }

        if (Barrier(code, item))
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

    // A call of Konami's driver plays a count of commands. Nothing moves into its commands from after them.
    if (code.konami)
    {
        for (const KonamiCallRange& range : KonamiCallRanges(commands))
        {
            starts.insert(range.end);
        }
    }

    std::erase_if(starts, [&items](uint32_t start) { return start > items.size(); });
    code.starts.assign(starts.begin(), starts.end());

    return code;
}

uint32_t ItemAt(const Code& code, uint32_t offset)
{
    const std::optional<uint32_t> item = ItemAtOffset(code.offsets, offset);
    if (!item)
    {
        throw std::invalid_argument("Can't find the command of this note in the sequence.");
    }

    return *item;
}

uint32_t NoteAt(const Code& code, uint32_t offset)
{
    const uint32_t item = ItemAt(code, offset);
    if ((*code.items)[item].kind != Kind::kNote)
    {
        throw std::invalid_argument("Can't find the command of this note in the sequence.");
    }

    return item;
}

bool IsSetting(const Command& command)
{
    return command.kind != Kind::kNote && command.kind != Kind::kWait && command.kind != Kind::kOther &&
           !EndsBlock(command) && !Barrier(command) && !ntdlib::commands::HasTarget(command.kind);
}

bool UsesVariables(const Command& command)
{
    return (command.kind >= Kind::kSetVariable && command.kind <= Kind::kUserProcedure) ||
           command.kind == Kind::kPrintVariable || command.value.source == Source::kVariable ||
           (command.time && command.time->source == Source::kVariable);
}

uint32_t EditedAt(const Code& code, uint32_t offset, bool settings)
{
    if (!settings)
    {
        // A rest of Konami's driver moves with its note (EditKonamiNotes). A volume command can start the note that
        // names it (KonamiSilentNote).
        const Command& command = (*code.items)[ItemAt(code, offset)];
        if (code.konami && (command.kind == Kind::kNoteOff || (command.kind == Kind::kVolume && command.index == 0)))
        {
            return ItemAt(code, offset);
        }

        return NoteAt(code, offset);
    }

    const uint32_t item = ItemAt(code, offset);
    if (const std::optional<std::string> why = WhyFixedInTime((*code.items)[item]))
    {
        throw std::invalid_argument(*why);
    }

    return item;
}

std::size_t BlockOf(const Code& code, uint32_t item)
{
    return static_cast<std::size_t>(std::upper_bound(code.starts.begin(), code.starts.end(), item) -
                                    code.starts.begin()) -
           1;
}

std::optional<uint32_t> KonamiSilentNote(const Code& code, uint32_t item)
{
    const std::vector<Command>& items = *code.items;
    if (!code.konami || items[item].kind != Kind::kVolume || items[item].index != 0)
    {
        return std::nullopt;
    }

    for (uint32_t i = item; i > code.starts[BlockOf(code, item)]; --i)
    {
        const Command& before = items[i - 1];
        if (before.kind == Kind::kNote)
        {
            return !before.sample && before.velocity == 0 ? std::optional(i - 1) : std::nullopt;
        }
    }

    return std::nullopt;
}

bool KonamiStartsAlone(const Code& code, uint32_t silent)
{
    const std::vector<Command>& items = *code.items;
    std::size_t volumes = 0;
    for (uint32_t i = silent + 1; i < code.starts[BlockOf(code, silent) + 1] && items[i].kind != Kind::kNote; ++i)
    {
        volumes += items[i].kind == Kind::kVolume && items[i].index == 0 ? 1 : 0;
    }

    return volumes == 1;
}

bool KonamiKeyEndsInBlock(const Code& code, uint32_t silent)
{
    const std::vector<Command>& items = *code.items;
    const auto is_note = [](const Command& command)
    {
        return command.kind == Kind::kNote;
    };

    return std::any_of(items.begin() + silent + 1, items.begin() + code.starts[BlockOf(code, silent) + 1], is_note);
}

std::string KonamiSharedKeyRefusal(const std::string& action)
{
    return "This note takes its key from the same command as another note. Can't " + action + " it.";
}

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

uint32_t TailOf(const Code& code, std::size_t block)
{
    const uint32_t end = code.starts[block + 1];
    return end > code.starts[block] && EndsBlockIn(code, (*code.items)[end - 1]) ? end - 1 : end;
}

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

bool IsTie(const Command& command)
{
    return command.kind == Kind::kNote && command.value.source == Source::kValue && command.value.value == 0;
}

std::optional<uint32_t> NoteTicks(const Command& note)
{
    const std::optional<uint32_t> ticks = WholeTicks(note.value);

    return ticks && *ticks > 0 ? ticks : std::nullopt;
}

std::optional<uint32_t> WrittenWait(const Command& note)
{
    return note.conditional ? std::nullopt : NoteTicks(note);
}

std::string WaitRefusal(const Command& note, const std::string& action)
{
    if (note.value.source != Source::kValue)
    {
        return "The track waits for this note, and the sequence sets its length during playback. Can't " + action +
               " it.";
    }
    if (!NoteTicks(note))
    {
        return "This note makes the track wait until all its notes have ended. Can't " + action + " it.";
    }

    return "A comparison decides whether the track waits for this note. Can't " + action + " it.";
}

} // namespace note_detail

namespace
{

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

    // A repeat of a count of 0 goes back for ever.
    const bool moves = last.kind == Kind::kJump || last.kind == Kind::kCall || last.kind == Kind::kReturn ||
                       last.kind == Kind::kEnd ||
                       (last.kind == Kind::kRepeat && last.value.source == Source::kValue && last.value.value == 0);
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

// The states of the note wait at the blocks of `code` where tracks open (EntryStates), the blocks where loops start,
// and the blocks after calls.
struct Openings
{
    std::vector<int> states;
    std::vector<std::size_t> loops;
    std::vector<std::size_t> returns;
};

Openings BlockOpenings(const format::File& file, uint32_t sequence, const Code& code, bool labels,
                       const song::Console& console, int opening)
{
    const std::vector<Command>& items = *code.items;
    std::vector<int> states(code.starts.size() - 1, 0);
    const auto open = [&](uint32_t item)
    {
        if (item < items.size())
        {
            states[BlockOf(code, item)] |= opening;
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

    if (console.engine == format::Engine::kDs)
    {
        open(0);
    }

    std::vector<std::size_t> loops;
    std::vector<std::size_t> returns;
    for (uint32_t i = 0; i < items.size(); ++i)
    {
        if (items[i].kind == Kind::kOpenTrack && !items[i].target_outside)
        {
            open(items[i].target);
        }

        if (console.engine == format::Engine::kDse && items[i].kind == Kind::kData)
        {
            open(i + 1);
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

    return {std::move(states), std::move(loops), std::move(returns)};
}

// The subroutines of `code` by the blocks where they start, and the loops' starts that each loop's end goes back to
// (EntryStates).
struct CallsAndLoops
{
    std::map<std::size_t, Subroutine> subroutines;
    std::map<std::size_t, std::vector<std::size_t>> backs;
};

CallsAndLoops CallsAndLoopsOf(const Code& code, std::span<const std::size_t> loops,
                              const std::vector<std::size_t>& returns)
{
    const std::vector<Command>& items = *code.items;
    const std::size_t blocks = code.starts.size() - 1;

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

    return {std::move(subroutines), std::move(backs)};
}

// Merges the states with which a track leaves block `block` of `code` into `states` of the blocks that it goes on to
// (EntryStates). `leaving` contains the states at the ends of the blocks. Returns whether a state changed.
bool MergeState(const Code& code, std::size_t block, std::span<const int> leaving, const CallsAndLoops& paths,
                std::vector<int>& states)
{
    const std::vector<Command>& items = *code.items;
    const std::size_t blocks = code.starts.size() - 1;
    if (leaving[block] == 0)
    {
        return false;
    }

    const auto& [subroutines, backs] = paths;
    bool changed = false;
    const auto flow = [&](std::size_t to, int entering)
    {
        if ((states[to] | entering) != states[to])
        {
            states[to] |= entering;
            changed = true;
        }
    };

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
        return changed;
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

    return changed;
}

} // namespace

namespace note_detail
{

std::vector<int> EntryStates(const format::File& file, uint32_t sequence, const Code& code, bool labels)
{
    const std::size_t blocks = code.starts.size() - 1;
    const song::Console console = ConsoleOf(file);
    const int opening = EditsOf(console).NoteWaitOpens(*file.sequences[sequence]) ? kWaitOn : kWaitOff;
    Openings openings = BlockOpenings(file, sequence, code, labels, console, opening);
    const CallsAndLoops paths = CallsAndLoopsOf(code, openings.loops, openings.returns);
    std::vector<int> states = std::move(openings.states);

    // The states flow from block to block until they stop changing.
    bool changed = true;
    while (changed)
    {
        changed = false;

        const std::vector<int> leaving = LeavingStates(code, states);
        for (std::size_t block = 0; block < blocks; ++block)
        {
            changed = MergeState(code, block, leaving, paths, states) || changed;
        }
    }

    return states;
}

std::vector<Run> RunsOf(const Code& code, std::size_t block)
{
    const std::vector<Command>& items = *code.items;
    const uint32_t begin = code.starts[block];
    const uint32_t end = code.starts[block + 1];
    const uint32_t tail = end > begin && EndsBlockIn(code, items[end - 1]) ? end - 1 : end;
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
                                          : Barrier(code, command);
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

Command WaitOf(const Command& wait, uint32_t ticks)
{
    Command command = wait;
    command.value = Argument{Source::kValue, static_cast<int64_t>(ticks) * kOne, 0};
    command.encoding.value_bytes = 0;

    return command;
}

int CheckedByte(int value, const char* what)
{
    if (value < 0 || value > 127)
    {
        throw std::invalid_argument(std::string("The ") + what + " must be between 0 and 127.");
    }

    return value;
}

} // namespace note_detail

namespace
{

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

} // namespace

namespace note_detail
{

std::optional<Spot> SpotOf(const Code& code, const Addition& addition)
{
    std::optional<Spot> found;
    for (const auto& [block, start] : BlockStarts(code, addition.stretch))
    {
        if (Barrier(code, (*code.items)[code.starts[block]]) || addition.tick < start)
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

} // namespace note_detail

} // namespace hokuto::edit
