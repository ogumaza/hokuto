// SPDX-License-Identifier: MIT

// The List Editor's rows: the tracks of a sequence with their labels, the rows that the engine skips, the runs
// of a subroutine, and the labels of the arrangement's parts.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/sequence_export/performance.h"

#include "app/list_edits.h"
#include "app/list_edits_parts.h"
#include "edit/bytecode.h"
#include "edit/commands.h"
#include "edit/quintet.h"
#include "edit/timelines.h"
#include "format/file.h"
#include "song/archive.h"
#include "song/driver.h"
#include "song/sequence.h"

namespace hokuto::app
{
namespace
{

using ntdlib::commands::Command;
using ntdlib::commands::Kind;

// What the List Editor shows of track `number` of `song` before the sequence's commands: its number, its name and its
// stretches, the timebase, every track that ran commands, with notes or without, for the menu, and where the calls of
// every track started runs.
ListTrack TrackOf(const song::Sequence& song, int number)
{
    ListTrack track;
    track.number = number;
    track.name = "Track " + std::to_string(number);
    track.ticks_per_quarter = song.timebase;
    for (const song::Track& played : song.tracks)
    {
        track.tracks.push_back(played.number);
        if (played.number == number)
        {
            track.name = played.name;
            track.stretches = played.stretches;
        }
        for (const song::Stretch& stretch : played.stretches)
        {
            if (stretch.entry == ntdlib::sequence_export::Entry::kCall)
            {
                track.runs.insert(stretch.first);
            }
        }
    }

    return track;
}

// Gives `track` the console of `file`, the offsets of `commands`, the labels at each item and what goes to each.
void AddSequence(ListTrack& track, const format::File& file, const ntdlib::commands::Sequence& commands)
{
    track.console = edit::ConsoleOf(file);
    track.offsets = edit::Offsets(commands, track.console);
    for (const ntdlib::commands::Label& label : commands.labels)
    {
        std::string& names = track.labels[label.item];
        names += (names.empty() ? "" : ", ") + label.name;
        track.targets.emplace_back(label.name, label.item);
    }
    for (const Command& command : commands.items)
    {
        if (!ntdlib::commands::HasTarget(command.kind) || command.target_outside)
        {
            continue;
        }

        Entries& entries = track.entries[command.target];
        switch (command.kind)
        {
        case Kind::kCall:
            entries.calls++;
            break;
        case Kind::kOpenTrack:
            entries.starts++;
            break;
        case Kind::kRepeat:
            entries.repeats++;
            break;
        default:
            entries.jumps++;
            break;
        }
    }
}

// The key that the note of `row` plays among `notes`: the note of the row's command in the row's stretch, or else the
// first note of the command. Nothing where the note doesn't sound.
std::optional<int> PlayedKey(std::span<const song::Note> notes, const edit::CommandRow& row)
{
    auto found = std::ranges::find_if(
        notes, [&row](const song::Note& note) { return note.command == row.command && note.stretch == row.stretch; });
    if (found == notes.end())
    {
        found = std::ranges::find_if(notes, [&row](const song::Note& note) { return note.command == row.command; });
    }

    return found != notes.end() ? std::optional<int>(found->key) : std::nullopt;
}

// Gives `track` its rows of `commands` in the order that `song` ran them, with `track`'s offsets. The track entered the
// sequence at each stretch's first command. On Quintet's driver, the row of a note contains the key that the note plays
// (ListRow::played_key).
void AddRows(ListTrack& track, const ntdlib::commands::Sequence& commands, const song::Sequence& song)
{
    std::span<const song::Note> notes;
    for (const song::Track& played : song.tracks)
    {
        if (played.number == track.number)
        {
            notes = played.notes;
        }
    }

    // A row takes the depth of its stretch. Within the stretch, a loop's start puts the loop on the stack, and the
    // loop's end takes it off where the track runs on.
    const std::optional<uint32_t> end = song.loop ? std::optional<uint32_t>(song.loop->end) : std::nullopt;
    const std::vector<edit::CommandRow> rows =
        edit::CommandRows(commands, track.offsets, track.stretches, notes, end, &track.console);
    const bool quintet = edit::IsQuintet(track.console);
    std::optional<int> key_offset;
    int depth = 0;
    for (std::size_t i = 0; i < rows.size(); i++)
    {
        const uint32_t item = *edit::ItemAtOffset(track.offsets, rows[i].command);
        ListRow& row = track.rows.emplace_back();
        row.row = rows[i];
        row.command = commands.items.at(item);
        if (quintet && row.command.kind == Kind::kNote)
        {
            // A note that a tie joins to the note before it plays no note of its own. It uses the distance from pitch
            // to key of the last note that played: the channel's octave and transposes. A note that plays the last note
            // again (index 1) has no pitch.
            row.played_key = PlayedKey(notes, rows[i]);
            if (!row.played_key && key_offset)
            {
                row.played_key = row.command.key + *key_offset;
            }
            if (row.played_key && row.command.index != 1)
            {
                key_offset = *row.played_key - row.command.key;
            }
        }
        row.entered = i == 0 || rows[i - 1].stretch != rows[i].stretch;
        depth = row.entered ? track.stretches[rows[i].stretch].depth : depth;
        row.depth = depth;
        if (!row.command.conditional && row.command.kind == Kind::kLoopStart)
        {
            depth++;
        }
        else if (!row.command.conditional && row.command.kind == Kind::kLoopEnd && depth > 0)
        {
            depth--;
        }
    }
}

} // namespace

namespace list_edits_parts
{

std::string EntriesText(const ListTrack& track, uint32_t item)
{
    const auto found = track.entries.find(item);
    if (found == track.entries.end())
    {
        return {};
    }

    std::string text;
    const auto add = [&text](int count, std::string_view one, std::string_view many)
    {
        if (count > 0)
        {
            text += (text.empty() ? "" : ", ") + std::to_string(count) + " " + std::string(count == 1 ? one : many);
        }
    };
    add(found->second.calls, "call", "calls");
    add(found->second.jumps, "jump", "jumps");
    add(found->second.starts, "track start", "track starts");
    add(found->second.repeats, "repeat", "repeats");

    return text;
}

ntdlib::commands::Sequence LabelNames(const ListTrack& track)
{
    ntdlib::commands::Sequence names;
    for (const auto& [name, item] : track.targets)
    {
        names.labels.push_back({name, item});
    }

    return names;
}

std::optional<uint32_t> ItemOfRow(const ListTrack& track, std::size_t index)
{
    return index < track.rows.size() ? edit::ItemAtOffset(track.offsets, track.rows[index].row.command) : std::nullopt;
}

} // namespace list_edits_parts

ListTrack MakeListTrack(const format::File& file, uint32_t sequence, const song::Sequence& song, int number)
{
    ListTrack track = TrackOf(song, number);
    try
    {
        const ntdlib::commands::Sequence& commands = edit::SequenceToEdit(file, sequence);
        AddSequence(track, file, commands);
        AddRows(track, commands, song);
        track.timeline = edit::IsTimeline(file, sequence);
    }
    catch (const std::exception& error)
    {
        track.rows.clear();
        track.unavailable = error.what();
    }

    return track;
}

std::vector<ListTrack> MakeListTracks(const format::File& file, uint32_t sequence, const song::Sequence& song)
{
    std::vector<ListTrack> tracks;
    for (const song::Track& played : song.tracks)
    {
        tracks.push_back(TrackOf(song, played.number));
    }

    // The offsets and the labels, once for every track.
    const ntdlib::commands::Sequence* commands = nullptr;
    ListTrack shared;
    try
    {
        commands = &edit::SequenceToEdit(file, sequence);
        AddSequence(shared, file, *commands);
    }
    catch (const std::exception& error)
    {
        for (ListTrack& track : tracks)
        {
            track.unavailable = error.what();
        }
        return tracks;
    }

    for (ListTrack& track : tracks)
    {
        track.console = shared.console;
        track.offsets = shared.offsets;
        track.labels = shared.labels;
        track.targets = shared.targets;
        track.entries = shared.entries;
        track.timeline = edit::IsTimeline(file, sequence);
        try
        {
            AddRows(track, *commands, song);
        }
        catch (const std::exception& error)
        {
            track.rows.clear();
            track.unavailable = error.what();
        }
    }

    return tracks;
}

ListTrack WithLabelRows(ListTrack track)
{
    std::vector<ListRow> rows;
    for (ListRow& row : track.rows)
    {
        const std::optional<uint32_t> item = edit::ItemAtOffset(track.offsets, row.row.command);
        for (const auto& [name, at] : track.targets)
        {
            if (item == at)
            {
                ListRow& label = rows.emplace_back(row);
                label.label = name;
            }
        }
        rows.push_back(std::move(row));
    }
    track.rows = std::move(rows);
    track.label_rows = true;

    return track;
}

bool Skipped(const ListTrack& track, const ListRow& row)
{
    return !IsLabel(row) &&
           (!song::Runs(track.console, row.command) || (track.engine && !song::Runs(*track.engine, row.command)));
}

const std::string& SkippingEngine(const ListTrack& track, const ListRow& row)
{
    return track.engine && song::Runs(track.console, row.command) ? track.engine_name : track.file_engine_name;
}

std::vector<ListCall> SubroutineRuns(const song::Sequence& song, uint32_t first)
{
    // The rows stop at the end of the loop's first pass (MakeListTrack). So do the runs.
    std::vector<ListCall> runs;
    for (const song::Track& played : song.tracks)
    {
        for (uint32_t i = 0; i < played.stretches.size(); i++)
        {
            const song::Stretch& stretch = played.stretches[i];
            if (song.loop && stretch.tick >= song.loop->end)
            {
                break;
            }
            if (stretch.entry == ntdlib::sequence_export::Entry::kCall && stretch.first == first)
            {
                runs.push_back({played.number, i, stretch.tick});
            }
        }
    }

    return runs;
}

std::optional<std::size_t> RowOfPart(const ListTrack& track, const song::Track& played, std::size_t first,
                                     int32_t start)
{
    // The call that started a run ends the stretch before it.
    if (first < played.notes.size())
    {
        const song::Note& note = played.notes[first];
        const std::optional<std::size_t> call = song::OutermostCall(played, note.stretch);
        for (std::size_t i = 0; i < track.rows.size(); i++)
        {
            const ListRow& row = track.rows[i];
            const bool found = call ? row.command.kind == Kind::kCall && row.row.stretch + 1 == *call
                                    : row.row.command == note.command && row.row.stretch == note.stretch;
            if (!IsLabel(row) && found)
            {
                return i;
            }
        }
    }
    for (std::size_t i = 0; i < track.rows.size(); i++)
    {
        const std::optional<uint32_t> tick = track.rows[i].row.tick;
        if (!IsLabel(track.rows[i]) && tick && static_cast<int64_t>(*tick) >= start)
        {
            return i;
        }
    }

    return std::nullopt;
}

int CallsOfRuns(const song::Sequence& song, std::span<const ListCall> runs)
{
    // The call that started a run ends the stretch before it.
    std::set<uint32_t> calls;
    for (const ListCall& run : runs)
    {
        const auto played = std::find_if(song.tracks.begin(), song.tracks.end(),
                                         [&run](const song::Track& track) { return track.number == run.track; });
        if (played != song.tracks.end() && run.stretch > 0 && run.stretch <= played->stretches.size())
        {
            calls.insert(played->stretches[run.stretch - 1].last);
        }
    }

    return static_cast<int>(calls.size());
}

std::size_t SharedLabelStart(const std::map<uint32_t, std::string>& labels)
{
    if (labels.size() < 2)
    {
        return 0;
    }

    std::string_view shared = labels.begin()->second;
    for (const auto& [offset, name] : labels)
    {
        const auto [end, other] = std::mismatch(shared.begin(), shared.end(), name.begin(), name.end());
        shared = shared.substr(0, static_cast<std::size_t>(end - shared.begin()));
    }

    const std::size_t underscore = shared.rfind('_');
    return underscore == std::string_view::npos ? 0 : underscore + 1;
}

std::string SubroutineName(const std::map<uint32_t, std::string>& labels, std::size_t shared, uint32_t offset)
{
    const auto label = labels.find(offset);
    if (label == labels.end())
    {
        return OffsetText(offset);
    }

    return label->second.size() > shared ? label->second.substr(shared) : label->second;
}

std::map<uint32_t, std::string> PartLabels(const song::Sequence& song, const format::File& file, uint32_t sequence)
{
    if (!song.labels.empty() || !file.native || sequence >= file.sequences.size())
    {
        return song.labels;
    }

    // The song's notes lead to their commands by the offsets that the List Editor shows (edit::Offsets).
    std::map<uint32_t, std::string> labels;
    try
    {
        const ntdlib::commands::Sequence& commands = *file.sequences[sequence];
        const std::vector<uint32_t> offsets = edit::Offsets(commands, edit::ConsoleOf(file));
        for (const ntdlib::commands::Label& label : commands.labels)
        {
            if (label.item < offsets.size())
            {
                labels.emplace(offsets[label.item], label.name);
            }
        }
    }
    catch (const std::exception&)
    {
        return {};
    }

    return labels;
}

ListTrack WithSubroutine(ListTrack track, ListSubroutine subroutine)
{
    // While the call stays on the stack, the run takes the stretches from the one that the call started.
    const ListCall* run = subroutine.shown < subroutine.runs.size() ? &subroutine.runs[subroutine.shown] : nullptr;
    track.subroutine = std::move(subroutine);
    if (run == nullptr || run->track != track.number || run->stretch >= track.stretches.size())
    {
        track.rows.clear();
        return track;
    }

    const int depth = track.stretches[run->stretch].depth;
    uint32_t end = run->stretch + 1;
    while (end < track.stretches.size() && track.stretches[end].depth >= depth)
    {
        end++;
    }
    std::erase_if(track.rows,
                  [&](const ListRow& row) { return row.row.stretch < run->stretch || row.row.stretch >= end; });
    for (ListRow& row : track.rows)
    {
        row.depth = std::max(row.depth - depth, 0);
    }

    return track;
}

} // namespace hokuto::app
