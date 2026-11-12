// SPDX-License-Identifier: MIT

// The rows of the List Editor: a track's commands in the order that it ran them, with their ticks, the notes
// that the driver never started, the labels, and the item at an offset.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

#include "ntdlib/commands/sequence.h"

#include "edit/bytecode.h"
#include "edit/commands.h"
#include "edit/commands_parts.h"
#include "edit/drivers.h"
#include "edit/notes.h"

namespace hokuto::edit
{
namespace
{

using ntdlib::commands::Command;
using ntdlib::commands::Kind;
using ntdlib::commands::Source;

using commands_parts::CommandItem;
using commands_parts::ItemAt;

// How far `command` moves its track on, in ticks. With `note_wait`, the track's notes wait for their length. Nothing
// for a time that the bytecode doesn't give: a wait or a note whose length comes from a random number, a variable or a
// comparison, a note of length 0 that waits for the track's notes to end, and bytes that no command reads. A rest of
// Quintet's driver is a note-off with a length, and it waits like a note. Every other note-off has no length.
std::optional<uint32_t> TicksOf(const Command& command, std::optional<bool> note_wait)
{
    const auto exact = [&command]() -> std::optional<uint32_t>
    {
        return command.conditional ? std::nullopt : WholeTicks(command.value);
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

    case Kind::kNoteOff:
        if (command.value.value == 0)
        {
            return 0;
        }
        if (!note_wait)
        {
            return std::nullopt;
        }

        return *note_wait ? exact() : 0;

    case Kind::kData:
        return std::nullopt;

    default:
        return 0;
    }
}

} // namespace

std::vector<CommandRow> CommandRows(const format::File& file, uint32_t sequence,
                                    std::span<const song::Stretch> stretches, std::span<const song::Note> notes,
                                    std::optional<uint32_t> end)
{
    const ntdlib::commands::Sequence& commands = SequenceToEdit(file, sequence);
    const song::Console console = ConsoleOf(file);
    return CommandRows(commands, Offsets(commands, console), stretches, notes, end, &console);
}

namespace
{

// The start of each note of `notes` whose start determines its tick, by the note's command and stretch (CommandRows).
std::map<std::pair<uint32_t, uint32_t>, uint32_t> NoteStarts(std::span<const song::Note> notes)
{
    std::map<std::pair<uint32_t, uint32_t>, uint32_t> starts;
    for (const song::Note& note : notes)
    {
        if (note.start_gives_tick)
        {
            starts.emplace(std::pair(note.command, note.stretch), note.start);
        }
    }

    return starts;
}

// Sets the tick of each row of `rows` from `begin` on that has no tick: the next known tick, less the ticks that the
// rows between use by `lengths` (CommandRows). `after` is the known tick after the last row.
void FillUnknownTicks(std::vector<CommandRow>& rows, std::size_t begin,
                      const std::vector<std::optional<uint32_t>>& lengths, std::optional<uint32_t> after)
{
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
}

// Drops the first row of `rows` from `begin` on that ran after the loop's end at `end`, and every row after it
// (CommandRows). Returns whether any row went.
bool DropLate(std::vector<CommandRow>& rows, std::size_t begin, std::optional<uint32_t> end)
{
    // A command that ran after the loop's end stays out, and so do the commands after it.
    const auto late = [end](const CommandRow& row)
    {
        return end && row.tick && *row.tick > *end;
    };
    const auto first_late = std::find_if(rows.begin() + static_cast<std::ptrdiff_t>(begin), rows.end(), late);
    if (first_late == rows.end())
    {
        return false;
    }

    rows.erase(first_late, rows.end());
    return true;
}

} // namespace

std::vector<CommandRow> CommandRows(const ntdlib::commands::Sequence& commands, const std::vector<uint32_t>& offsets,
                                    std::span<const song::Stretch> stretches, std::span<const song::Note> notes,
                                    std::optional<uint32_t> end, const song::Console* console)
{
    // On some drivers, such as Brownie Brown's driver, a command moves its track on by other rules
    // (DriverEdits::RowTicks).
    const DriverEdits* edits = console != nullptr && console->driver != nullptr ? &EditsOf(*console->driver) : nullptr;
    const auto ticks_of = [edits](const Command& command, std::optional<bool> note_wait) -> std::optional<uint32_t>
    {
        const std::optional<uint32_t> ticks = TicksOf(command, note_wait);
        return edits != nullptr ? edits->RowTicks(command, ticks) : ticks;
    };

    // The arrangement gives the start of each note that a stretch ran.
    const std::map<std::pair<uint32_t, uint32_t>, uint32_t> starts = NoteStarts(notes);

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

            lengths.push_back(ticks_of(command, note_wait));
            tick = tick && lengths.back() ? std::optional<uint32_t>(*tick + *lengths.back()) : std::nullopt;
        }

        // A command after a wait of unknown length, such as one that runs only if the last comparison held, ran as many
        // ticks before the next known tick as the commands between them take. The stretch's last command starts the
        // next stretch at once.
        if (whole && index + 1 < stretches.size())
        {
            after = stretches[index + 1].tick;
        }

        FillUnknownTicks(rows, begin, lengths, after);
        if (DropLate(rows, begin, end))
        {
            break;
        }
    }

    return rows;
}

std::vector<std::optional<NotePlace>> UnstartedPlaces(const format::File& file, uint32_t sequence,
                                                      const song::Track& track)
{
    // The first stretch that ran each command at each tick.
    std::map<std::pair<uint32_t, uint32_t>, uint32_t> ran;
    for (const CommandRow& row : CommandRows(file, sequence, track.stretches, track.notes, std::nullopt))
    {
        if (row.tick && row.stretch < track.stretches.size())
        {
            ran.emplace(std::pair(row.command, *row.tick), row.stretch);
        }
    }

    std::vector<std::optional<NotePlace>> places;
    places.reserve(track.unstarted.size());
    for (const song::Note& note : track.unstarted)
    {
        const auto found = ran.find(std::pair(note.command, note.start));
        places.push_back(found != ran.end() ? std::optional<NotePlace>(NotePlace{
                                                  .command = note.command, .stretch = track.stretches[found->second]})
                                            : std::nullopt);
    }

    return places;
}

const Command& CommandAt(const format::File& file, uint32_t sequence, uint32_t command)
{
    const ntdlib::commands::Sequence& commands = SequenceToEdit(file, sequence);
    const uint32_t item = CommandItem(Offsets(commands, ConsoleOf(file)), command);
    if (item >= commands.items.size())
    {
        throw std::invalid_argument("The command isn't at the position that the List Editor shows.");
    }

    return commands.items[item];
}

bool DrawsRandom(const format::File& file, uint32_t sequence, const song::Sequence& song)
{
    // The List Editor's rows: the commands that each track ran, up to the end of the loop's first pass.
    const ntdlib::commands::Sequence& commands = SequenceToEdit(file, sequence);
    const song::Console console = ConsoleOf(file);
    const std::vector<uint32_t> offsets = Offsets(commands, console);
    const std::optional<uint32_t> end = song.loop ? std::optional<uint32_t>(song.loop->end) : std::nullopt;
    for (const song::Track& track : song.tracks)
    {
        for (const CommandRow& row : CommandRows(commands, offsets, track.stretches, track.notes, end, &console))
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

    return ItemAt(commands, Offsets(commands, ConsoleOf(file)), command);
}

} // namespace hokuto::edit
