// SPDX-License-Identifier: MIT

// The rewrite of a block by a note edit, and the end of a timeline or a program's track that a note can't last past
// (edit/note_edits.h).

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/sar/layout.h"
#include "ntdlib/sequence_export/performance.h"

#include "edit/bytecode.h"
#include "edit/note_blocks.h"
#include "edit/note_edits.h"
#include "edit/timelines.h"

namespace hokuto::edit
{

namespace
{

using note_detail::Addition;
using note_detail::Barrier;
using note_detail::BlockChange;
using note_detail::BlockOf;
using note_detail::BlockStarts;
using note_detail::BlockTicks;
using note_detail::Code;
using note_detail::EndOf;
using note_detail::EndsAt;
using note_detail::EndsBlockIn;
using note_detail::IsTie;
using note_detail::ItemAt;
using note_detail::LastsTo;
using note_detail::NoteTicks;
using note_detail::ProgramEnd;
using note_detail::ProgramEndOf;
using note_detail::Spot;
using note_detail::SpotOf;
using note_detail::StretchEnd;
using note_detail::TailOf;
using note_detail::TimelineSpot;
using note_detail::WaitOf;
using note_detail::WaitTicks;
using ntdlib::commands::Command;
using ntdlib::commands::Kind;
using ntdlib::commands::Source;

} // namespace

namespace note_detail
{

void Rewrite(const Code& code, const std::vector<Command>& items, uint32_t begin, uint32_t end, BlockChange change,
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
    const uint32_t tail = end > begin && EndsBlockIn(code, items[end - 1]) ? end - 1 : end;
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
    uint32_t until = std::max(tick, change.until);
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

StretchEnd EndOf(const Code& code, const std::map<std::size_t, uint32_t>& starts)
{
    const auto& [block, start] = *starts.rbegin();
    const uint32_t tail = TailOf(code, block);
    const bool jump =
        tail < code.starts[block + 1] && (*code.items)[tail].kind == Kind::kJump && !(*code.items)[tail].conditional;
    return {block, start, start + BlockTicks(code, block).back(), jump, EndsAt(code, tail)};
}

std::optional<std::pair<std::size_t, uint32_t>> LastsTo(const StretchEnd& end, uint32_t at, const Command& note,
                                                        const SongEnd& song_end)
{
    const std::optional<uint32_t> length = NoteTicks(note);
    if (!end.ends || !length || at + *length <= end.tick || (song_end.loops && !song_end.tick))
    {
        return std::nullopt;
    }

    return std::pair(end.block, at + *length - end.start);
}

namespace
{

// The notes of a stretch that may sound past its end (ProgramEndOf): each tie whose end hasn't come, with its key, and
// each note with the tick where its length ends.
struct OpenNotes
{
    std::vector<std::pair<uint32_t, uint8_t>> ties;
    std::vector<std::pair<uint32_t, uint32_t>> lengths;
};

// Keeps `command`, item `item` at tick `tick`, in `open` where it starts a tie or a note with a length, or in `held`
// where it is a note whose length the bytecode doesn't give. A note-off ends the latest open tie of its key.
void KeepNote(const Command& command, uint32_t item, uint32_t tick, OpenNotes& open, std::set<uint32_t>& held)
{
    if (command.kind == Kind::kNote && IsTie(command))
    {
        open.ties.emplace_back(item, command.key);
    }
    else if (command.kind == Kind::kNote && NoteTicks(command))
    {
        open.lengths.emplace_back(item, tick + *NoteTicks(command));
    }
    else if (command.kind == Kind::kNote)
    {
        held.insert(item);
    }

    if (command.kind == Kind::kNoteOff)
    {
        const auto tie =
            std::ranges::find(open.ties.rbegin(), open.ties.rend(), command.key, &std::pair<uint32_t, uint8_t>::second);
        if (tie != open.ties.rend())
        {
            open.ties.erase(std::next(tie).base());
        }
    }
}

// Adds to `held` the notes of `open` that sound past tick `end`: the notes that end later and the ties that don't end.
void AddHeld(const OpenNotes& open, uint32_t end, std::set<uint32_t>& held)
{
    for (const auto& [item, until] : open.lengths)
    {
        if (until > end)
        {
            held.insert(item);
        }
    }

    for (const auto& [item, key] : open.ties)
    {
        held.insert(item);
    }
}

} // namespace

ProgramEnd ProgramEndOf(const Code& code, const song::Stretch& stretch)
{
    const std::vector<Command>& items = *code.items;
    const uint32_t first = ItemAt(code, stretch.first);
    const uint32_t last = ItemAt(code, stretch.last);
    ProgramEnd end;
    end.first = first;
    if (last < first)
    {
        return end;
    }

    end.block = BlockOf(code, last);
    end.fixed = stretch.entry != ntdlib::sequence_export::Entry::kOpen || code.starts[end.block] < first;
    OpenNotes open;
    uint32_t tick = stretch.tick;
    bool note_wait = stretch.note_wait;
    for (uint32_t i = first; i <= last; ++i)
    {
        const Command& command = items[i];
        const bool waiting_note = command.kind == Kind::kNote && note_wait;
        const bool unknown_wait = command.kind == Kind::kNoteWait
                                      ? command.conditional || command.value.source != Source::kValue
                                      : Barrier(code, command);
        if (unknown_wait || (waiting_note && !NoteTicks(command)))
        {
            return end;
        }

        if (i == code.starts[end.block])
        {
            end.start = tick;
        }

        end.ticks.push_back(tick);
        KeepNote(command, i, tick, open, end.held);

        const bool legato = (command.kind == Kind::kTie || command.kind == Kind::kMonophonic) &&
                            (command.value.source != Source::kValue || command.value.value != 0);
        end.fixed = end.fixed || legato || (waiting_note && i >= code.starts[end.block]);
        note_wait = command.kind == Kind::kNoteWait ? command.value.value != 0 : note_wait;
        tick += waiting_note ? *NoteTicks(command) : WaitTicks(command).value_or(0);
    }

    end.ends = items[last].kind == Kind::kEnd && !items[last].conditional;
    end.tick = end.ticks.back();
    AddHeld(open, end.tick, end.held);

    return end;
}

std::optional<uint32_t> TrackEndIn(const Code& code, std::span<const song::Stretch> stretches)
{
    const ProgramEnd end = ProgramEndOf(code, stretches.back());
    if (end.ends)
    {
        return end.tick;
    }

    const std::vector<Command>& items = *code.items;
    std::vector<uint32_t> returns;
    for (const song::Stretch& stretch : stretches.first(stretches.size() - 1))
    {
        const uint32_t item = ItemAt(code, stretch.last);
        const Command& command = items[item];
        if ((command.kind == Kind::kCall || command.kind == Kind::kReturn) && command.conditional)
        {
            return std::nullopt;
        }

        if (command.kind == Kind::kCall)
        {
            returns.push_back(item + 1);
        }
        else if (command.kind == Kind::kReturn)
        {
            if (returns.empty())
            {
                return std::nullopt;
            }

            returns.pop_back();
        }
    }

    const uint32_t last = ItemAt(code, stretches.back().last);
    const bool returned = items[last].kind == Kind::kReturn && !items[last].conditional;
    if (!returned || returns.empty() || end.ticks.size() != last - end.first + 1 || !EndsAt(code, returns.back()))
    {
        return std::nullopt;
    }

    return end.ticks.back();
}

TimelineSpot TimelineSpotOf(const Code& code, const Addition& addition, const SongEnd& song_end)
{
    const bool note = addition.command.kind == Kind::kNote;
    const std::map<std::size_t, uint32_t> starts = BlockStarts(code, addition.stretch);
    if (starts.empty() || addition.tick < addition.stretch.tick)
    {
        throw std::invalid_argument(note ? "Can't place a note before the start of the track."
                                         : "Can't place a command before the start of the track.");
    }

    const StretchEnd end = EndOf(code, starts);
    if (end.loops && addition.tick >= end.tick)
    {
        throw std::invalid_argument(note ? "A note can't start at or after the loop end. The track jumps back to the "
                                           "loop start there."
                                         : "Can't place a command at or after the loop end. The track jumps back to "
                                           "the loop start there.");
    }
    if (!end.loops && song_end.tick && addition.tick >= *song_end.tick)
    {
        throw std::invalid_argument(note ? "A note can't start at or after the loop end. The song jumps back to the "
                                           "loop start there."
                                         : "Can't place a command at or after the loop end. The song jumps back to "
                                           "the loop start there.");
    }
    if (const std::optional<Spot> spot = SpotOf(code, addition); spot && spot->note_wait == false)
    {
        return {*spot, note ? LastsTo(end, addition.tick, addition.command, song_end) : std::nullopt};
    }

    if (note && end.ends && addition.tick >= end.tick)
    {
        if (song_end.loops && !song_end.tick)
        {
            throw std::invalid_argument(
                "Can't place a note past the end of the track in this song. Hokuto can't tell "
                "where the song jumps back to the loop start.");
        }

        const uint32_t tick = addition.tick - end.start;
        return {Spot{end.block, tick, false}, std::pair(end.block, tick + NoteTicks(addition.command).value_or(0))};
    }

    throw std::invalid_argument(note ? "Can't add a note at that tick on this track."
                                     : "Can't add a command at that tick on this track.");
}

void Last(std::map<std::size_t, BlockChange>& blocks, const std::optional<std::pair<std::size_t, uint32_t>>& lasts)
{
    if (!lasts)
    {
        return;
    }

    BlockChange& change = blocks[lasts->first];
    change.extend = true;
    change.until = std::max(change.until, lasts->second);
}

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

} // namespace note_detail

} // namespace hokuto::edit
