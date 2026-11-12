// SPDX-License-Identifier: MIT

// The note edits on a track whose notes wait for their length: the runs that turn the note wait off, and the edit after
// them (edit/note_edits.h).

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/commands/units.h"
#include "ntdlib/sequence_export/performance.h"

#include "edit/bytecode.h"
#include "edit/note_blocks.h"
#include "edit/note_edits.h"
#include "edit/notes.h"
#include "song/driver.h"

namespace hokuto::edit
{

namespace
{

using note_detail::Addition;
using note_detail::BlockOf;
using note_detail::Code;
using note_detail::EditedAt;
using note_detail::EditNotesIn;
using note_detail::EntryStates;
using note_detail::ItemAt;
using note_detail::kTimedAddition;
using note_detail::kTimedCommand;
using note_detail::kTimedNote;
using note_detail::kWaitOn;
using note_detail::NoteWaitAt;
using note_detail::Run;
using note_detail::RunOf;
using note_detail::RunsOf;
using note_detail::Spot;
using note_detail::SpotOf;
using note_detail::WaitOf;
using note_detail::WaitRefusal;
using note_detail::WithItems;
using note_detail::WrittenWait;
using ntdlib::commands::Command;
using ntdlib::commands::Kind;

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
    const Command on = song::ValueCommand(Kind::kNoteWait, 1);
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

} // namespace

namespace note_detail
{

namespace
{

// Adds to `runs` the run of each edit of `named` that moves or resizes a note that waits for its length (RunsToWrite).
// `entering` and `starting` give the note wait with which code enters a block of `code`. Throws std::invalid_argument
// with a message for the user for a note whose wait can't move.
template <class Entering, class Starting>
void AddEditRuns(const Code& code, const std::map<uint32_t, const NoteEdit*>& named, const Entering& entering,
                 const Starting& starting, std::map<uint32_t, Run>& runs)
{
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
            throw std::invalid_argument(timing ? "Another part of the sequence may run this note with note wait on. "
                                                 "Can't move or resize it."
                                               : "Another part of the sequence may run this note with note wait on. "
                                                 "Can't remove it.");
        }
        if (!timing || note_wait != true)
        {
            continue;
        }

        if (!code.writes_note_wait)
        {
            throw std::invalid_argument(note.kind == Kind::kNote ? std::string(kTimedNote)
                                                                 : std::string(kTimedCommand));
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
                    ? "Another part of the sequence may run this note with note wait off. Can't move or resize it."
                    : "Another part of the sequence may run this command with note wait off. Can't move it.");
        }

        Run run = *RunOf(code, block, item);
        runs.emplace(run.begin, std::move(run));
    }
}

// Adds to `runs` the run that contains the tick of each of `additions` where the notes wait for their length
// (RunsToWrite). `entering` and `starting` give the note wait with which code enters a block of `code`. Throws
// std::invalid_argument with a message for the user for a new command that the note wait can't take.
template <class Entering, class Starting>
void AddAdditionRuns(const Code& code, std::span<const Addition> additions, const Entering& entering,
                     const Starting& starting, std::map<uint32_t, Run>& runs)
{
    for (const Addition& addition : additions)
    {
        const std::optional<Spot> spot = SpotOf(code, addition);
        if (spot && spot->note_wait == false && addition.command.kind == Kind::kNote &&
            (starting(spot->block) & kWaitOn) != 0)
        {
            throw std::invalid_argument(
                "Another part of the sequence may run the commands at this tick with note wait on. Can't add a "
                "note there.");
        }

        if (!spot || spot->note_wait != true || entering(spot->block) != kWaitOn)
        {
            continue;
        }
        if (!code.writes_note_wait)
        {
            throw std::invalid_argument(std::string(kTimedAddition));
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
}

} // namespace

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
    AddEditRuns(code, named, entering, starting, runs);

    // A new command goes into the run that holds its tick.
    AddAdditionRuns(code, additions, entering, starting, runs);

    return runs;
}

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

} // namespace note_detail

} // namespace hokuto::edit
