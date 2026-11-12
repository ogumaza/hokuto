// SPDX-License-Identifier: MIT

// Checks the edits of a program's labels and subroutines (src/edit/subroutines.h) on a sound archive of the Wii, the
// 3DS, the Wii U or the Switch, on a GBA ROM with MusicPlayer2000 or Konami's driver, or on DSE files. The List Editor
// makes these edits. The tool imports the archive into a Hokuto file the way the program does, but without timelines:
// it edits every sequence as a program. ntdlib's performer reads each sequence sound. Five rounds follow:
//
// - Labels. Up to 8 labels of each sequence take another name and then their name again. Up to 8 new labels go in at
//   commands spread over the sequence, and go again. Up to 8 labels move to the next command that nothing goes to, and
//   back. The file has to come back as it was after each pair of edits. Then every label of every sequence takes
//   another name, in one version of the file. The archive that the version exports, imported again, has to hold the
//   new names at the same offsets.
// - Convert to Subroutine. One run of commands of each sequence file becomes a subroutine, in one version of the file.
//   The run is chosen on the first sound that plays the file, from the rows that the List Editor shows of its track
//   with the most rows. The run takes up to 6 rows at the depth of its first row, and the deeper rows between them: the
//   runs of calls and the commands inside loops. It starts a third of the way through the rows, or at other places when
//   the edit refuses that. The List Editor's SubroutineEdit gives the commands, the depth of the stack and the name.
//   Every sound that plays an edited file has to play its notes as before from the exported archive.
// - The same run becomes a subroutine by itself, and its call then gives way to the subroutine's commands. The
//   sequence's commands have to come back as they were, and the subroutine stays after them. The places in the
//   archive's tables have to come back too.
// - Dissolve Call. The first call that the first sound of each file ran, and that the edit takes, gives way to its
//   subroutine's commands, in one version of the file. Every sound of an edited file has to play its notes as before.
// - Convert to Real Copy. The same call takes a copy of its subroutine, in one version of the file. Every sound of an
//   edited file has to play its notes as before.
//
// A sound plays its notes as before when each note starts at the same tick, with the same key, velocity and length.
// ntdlib's performer finds a loop where every track's state repeats, the stack of calls included. An edit can change
// that state, and the loop can then start a whole number of passes earlier or later. Up to the earlier loop's end, the
// notes then have to start as before. The tool counts such sounds apart. It counts the refusals by their messages. It
// prints a line for each problem, and a summary. It returns 1 when a sequence or a sound differs, or when an edit, a
// read or the export fails.
//
// A GBA ROM follows the rules of MusicPlayer2000: patterns three deep, and repeats that go back inside a pattern. The
// ROM keeps no labels, and an import adds none. In the first round, up to 8 labels therefore go into each song first,
// at commands spread over it. After the new names, the export has to give the ROM of the file without labels, byte for
// byte, and the file has to save and open again with the new names in both forms. On a ROM without room for a song that
// outgrows its place (tools::Roomless), a round leaves out the edits of such a song, and counts them apart. With
// --filler, such a song moves into the filler runs of the ROM instead (format::NativeArchive::gba_filler). A ROM with
// Konami's driver has the same rounds, by the driver's rules.
//
// A set of DSE's files keeps no labels either. Each sequence's first label names its file, and the export has to give
// the set's bytes after the new names. DSE's tracks have no calls. The second round's edit refuses a subroutine. A ROM
// of another of the GBA's drivers keeps no labels either. On Brownie Brown's driver, a label names each sound's list,
// and the export has to give the ROM's bytes after the new names.
//
// With --model, the file goes into the model form before the edits (format::InModelForm), and the archive that the
// model builds has to be the game's. Each file that the edits make then has to save in both forms and open again as it
// was (tools/checked_archive.h).
//
// usage: hokuto_check_subroutines [--model] [--filler] <archive>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/sar/layout.h"
#include "xsflib/console.h"
#include "xsflib/files.h"

#include "app/list_edits.h"
#include "check_support.h"
#include "checked_archive.h"
#include "checked_saves.h"
#include "edit/bytecode.h"
#include "edit/commands.h"
#include "edit/replace.h"
#include "edit/subroutines.h"
#include "format/archive.h"
#include "format/file.h"
#include "loop_passes.h"
#include "notes_apart.h"
#include "song/sequence.h"

namespace
{

namespace edit = hokuto::edit;
namespace sar = ntdlib::sar;
namespace song = hokuto::song;
using hokuto::format::File;
using hokuto::tools::CheckedArchive;
using hokuto::tools::SpreadIndices;
using ntdlib::commands::Command;
using ntdlib::commands::Label;
using ntdlib::commands::Sequence;

// The labels and the new labels of each sequence that the first round edits, and the rows of a subroutine's run.
constexpr std::size_t kMostLabels = 8;
constexpr std::size_t kMostRows = 6;

// What the check counted, for the summary.
struct Totals
{
    std::size_t sequences = 0;
    std::size_t sounds = 0;
    std::size_t unreadable = 0;
    std::size_t renamed = 0;
    std::size_t added = 0;
    std::size_t moved = 0;
    std::size_t names_read = 0;
    std::size_t subroutines = 0;
    std::size_t subroutine_commands = 0;
    std::size_t dissolved = 0;
    std::size_t copied = 0;
    std::size_t reads = 0;
    std::size_t loops_moved = 0;
    std::size_t problems = 0;
    std::size_t given = 0;    // the labels that went into a GBA song first
    std::size_t outgrown = 0; // the edits of songs that outgrew their places in a GBA ROM without room
    std::map<std::string, std::size_t> refusals;
};

// The archive as the tool reads it: the file, where each sound plays in it, each sequence sound as ntdlib's performer
// read it, and the sounds of each sequence.
struct Archive
{
    std::string name;
    CheckedArchive archive;
    File file;
    std::vector<std::optional<edit::SoundPlace>> places;
    std::map<uint32_t, song::Sequence> songs;
    std::map<uint32_t, std::vector<uint32_t>> by_sequence;
};

// Prints problem `message` of `archive`.
void Problem(const Archive& archive, Totals& totals, const std::string& message)
{
    std::printf("%s: %s\n", archive.name.c_str(), message.c_str());
    ++totals.problems;
}

// The notes of a sound as the tool compares them: each one's track, start, key, velocity and length.
using Played = std::tuple<int, uint32_t, uint8_t, uint8_t, uint32_t>;

// The notes of `sequence` that start before `end`, in order, with their lengths or without them.
std::vector<Played> NotesOf(const song::Sequence& sequence, uint32_t end, bool lengths)
{
    std::vector<Played> notes;
    for (const song::Track& track : sequence.tracks)
    {
        for (const song::Note& note : track.notes)
        {
            if (note.start < end)
            {
                notes.emplace_back(track.number, note.start, note.key, note.velocity, lengths ? note.length : 0);
            }
        }
    }
    std::sort(notes.begin(), notes.end());

    return notes;
}

// Whether `after` plays the notes of `before`: each note at its start, with its key, velocity and length. A loop that
// starts a whole number of passes earlier or later counts too: up to the earlier loop's end, the notes then have to
// start as before, with their keys and velocities. `moved` becomes true then.
bool SamePlaying(const song::Sequence& before, const song::Sequence& after, bool& moved)
{
    moved = after.loop != before.loop;
    if (!moved)
    {
        return NotesOf(after, std::numeric_limits<uint32_t>::max(), true) ==
               NotesOf(before, std::numeric_limits<uint32_t>::max(), true);
    }
    const std::optional<uint32_t> end = hokuto::tools::EarlierEnd(before, after);
    if (!end)
    {
        return false;
    }

    return NotesOf(after, *end, false) == NotesOf(before, *end, false);
}

// The items of sequence `sequence` of `file` that a jump, a call, a track start, a label or a place in the archive's
// tables goes to.
std::set<uint32_t> EnteredItems(const File& file, uint32_t sequence)
{
    const Sequence& commands = *file.sequences[sequence];
    std::set<uint32_t> entered;
    for (const Command& command : commands.items)
    {
        if (ntdlib::commands::HasTarget(command.kind) && !command.target_outside)
        {
            entered.insert(command.target);
        }
    }
    for (const Label& label : commands.labels)
    {
        entered.insert(label.item);
    }
    const auto enter = [&entered](const sar::Place& place)
    {
        entered.insert(place.offset);
    };
    edit::ForEachItemPlace(*file.native, sequence, enter);

    return entered;
}

// Whether `after` holds sequence `sequence` of `before` as it was, and the same places in the archive's tables.
bool SameSequence(const File& before, const File& after, uint32_t sequence)
{
    return *before.sequences[sequence] == *after.sequences[sequence] && before.native->fields == after.native->fields;
}

// The first round's pairs of edits on sequence `sequence`: new names, new labels and moves, each undone by a second
// edit. A sequence whose command offsets can't be found counts as a problem, and receives no edits.
void LabelPairs(const Archive& archive, uint32_t sequence, Totals& totals)
{
    const File& file = archive.file;
    const Sequence& commands = *file.sequences[sequence];
    const std::string where = "sequence " + std::to_string(sequence) + ": ";
    std::vector<uint32_t> offsets;
    try
    {
        offsets = edit::Offsets(commands, edit::ConsoleOf(file));
    }
    catch (const std::exception& error)
    {
        Problem(archive, totals, where + "the offsets of the commands can't be found: " + error.what());
        return;
    }
    const auto check = [&](const File& back, const std::string& what)
    {
        if (!SameSequence(file, back, sequence))
        {
            Problem(archive, totals, where + what + " didn't come back");
        }
    };

    for (const std::size_t index : SpreadIndices(commands.labels.size(), kMostLabels))
    {
        const std::string& name = commands.labels[index].name;
        try
        {
            const std::string other = edit::UniqueLabel(commands.labels, name + "_r");
            const File renamed = edit::RenameLabel(file, sequence, name, other).file;
            check(edit::RenameLabel(renamed, sequence, other, name).file, "the new name of label " + name);
            ++totals.renamed;
        }
        catch (const std::exception& error)
        {
            Problem(archive, totals, where + "a new name of label " + name + ": " + error.what());
        }
    }

    for (const std::size_t item : SpreadIndices(commands.items.size(), kMostLabels))
    {
        const std::string name = edit::UniqueLabel(commands.labels, "check_label");
        try
        {
            const File added = edit::AddLabel(file, sequence, offsets[item], name).file;
            check(edit::RemoveLabel(added, sequence, name).file, "a new label at item " + std::to_string(item));
            ++totals.added;
        }
        catch (const std::exception& error)
        {
            Problem(archive, totals, where + "a new label at item " + std::to_string(item) + ": " + error.what());
        }
    }

    // Up to kMostLabels labels, spread over the file, move to the next command that nothing goes to, and back.
    const std::set<uint32_t> entered = EnteredItems(file, sequence);
    for (const std::size_t index : SpreadIndices(commands.labels.size(), kMostLabels))
    {
        const Label& label = commands.labels[index];
        uint32_t to = label.item + 1;
        while (to < commands.items.size() && entered.contains(to))
        {
            to++;
        }
        if (to >= commands.items.size())
        {
            continue;
        }

        try
        {
            const File moved = edit::MoveLabel(file, sequence, label.name, offsets[to]).file;
            check(edit::MoveLabel(moved, sequence, label.name, offsets[label.item]).file, "the move of " + label.name);
            ++totals.moved;
        }
        catch (const std::exception& error)
        {
            Problem(archive, totals, where + "the move of " + label.name + ": " + error.what());
        }
    }
}

// `file` with up to kMostLabels new labels in each sequence, at commands spread over it. A GBA song has no labels. A
// sequence whose command offsets can't be found counts as a problem, and receives no labels.
File WithLabels(const Archive& archive, File file, Totals& totals)
{
    for (uint32_t sequence = 0; sequence < file.sequences.size(); sequence++)
    {
        std::vector<uint32_t> offsets;
        try
        {
            offsets = edit::Offsets(*file.sequences[sequence], edit::ConsoleOf(file));
        }
        catch (const std::exception& error)
        {
            Problem(archive, totals,
                    "sequence " + std::to_string(sequence) +
                        ": the offsets of the commands can't be found: " + error.what());
            continue;
        }
        for (const std::size_t item : SpreadIndices(file.sequences[sequence]->items.size(), kMostLabels))
        {
            const std::string name = edit::UniqueLabel(file.sequences[sequence]->labels, "check_label");
            try
            {
                file = edit::AddLabel(file, sequence, offsets[item], name).file;
                ++totals.given;
            }
            catch (const std::exception& error)
            {
                Problem(archive, totals,
                        "sequence " + std::to_string(sequence) + ": a first label at item " + std::to_string(item) +
                            ": " + error.what());
            }
        }
    }

    return file;
}

// The first round's version: every label of every sequence takes another name. The exported archive, imported again,
// has to hold the new names. On the GBA, with any of its drivers, and on DSE, the files keep no labels. The export has
// to give the bytes of `unlabeled`, and the saved file has to hold the new names.
void RenameAll(const Archive& archive, const File& unlabeled, Totals& totals)
{
    File version = archive.file;
    for (uint32_t sequence = 0; sequence < version.sequences.size(); sequence++)
    {
        const std::vector<Label> labels = version.sequences[sequence]->labels;
        for (const Label& label : labels)
        {
            try
            {
                const std::string other = edit::UniqueLabel(version.sequences[sequence]->labels, label.name + "_r");
                version = edit::RenameLabel(version, sequence, label.name, other).file;
            }
            catch (const std::exception& error)
            {
                Problem(archive, totals,
                        "sequence " + std::to_string(sequence) + ": a new name of label " + label.name + ": " +
                            error.what());
            }
        }
    }

    if (archive.archive.gba || archive.archive.driver || archive.archive.dse)
    {
        try
        {
            if (hokuto::format::ExportArchive(version) != hokuto::format::ExportArchive(unlabeled))
            {
                Problem(archive, totals, "the labels changed the bytes of the export");
            }
            if (!hokuto::tools::SavesAndOpens(version))
            {
                Problem(archive, totals, "the file with the new names opens otherwise");
                return;
            }
            for (const auto& sequence : version.sequences)
            {
                totals.names_read += sequence->labels.size();
            }
        }
        catch (const std::exception& error)
        {
            Problem(archive, totals, std::string("the save with the new names failed: ") + error.what());
        }
        return;
    }

    try
    {
        hokuto::tools::CheckSaved(version);
        const File again = hokuto::tools::ImportChecked(hokuto::format::ExportArchive(version), archive.archive,
                                                        hokuto::format::ExportGroupFiles(version));
        for (uint32_t sequence = 0; sequence < version.sequences.size(); sequence++)
        {
            if (sequence >= again.sequences.size() ||
                edit::LabelsOf(again, sequence) != edit::LabelsOf(version, sequence))
            {
                Problem(archive, totals,
                        "sequence " + std::to_string(sequence) + ": the new names didn't come back from the export");
                continue;
            }

            totals.names_read += version.sequences[sequence]->labels.size();
        }
    }
    catch (const std::exception& error)
    {
        Problem(archive, totals, std::string("the export with the new names failed: ") + error.what());
    }
}

// The run of the second round: the offsets of its commands, the depth and the name that the List Editor gives.
struct Run
{
    std::vector<uint32_t> commands;
    int depth = 0;
    std::string name;
};

// The run of sequence `sequence`, chosen on its first sound. Nothing where the edit refuses every candidate run: the
// first refusal counts.
std::optional<Run> RunOf(const Archive& archive, uint32_t sequence, Totals& totals)
{
    const song::Sequence& first = archive.songs.at(archive.by_sequence.at(sequence).front());
    std::optional<hokuto::app::ListTrack> busiest;
    for (const song::Track& track : first.tracks)
    {
        hokuto::app::ListTrack listed = hokuto::app::MakeListTrack(archive.file, sequence, first, track.number);
        if (!busiest || listed.rows.size() > busiest->rows.size())
        {
            busiest = std::move(listed);
        }
    }
    if (!busiest || busiest->rows.empty())
    {
        ++totals.refusals["Convert to Subroutine: a track without rows"];
        return std::nullopt;
    }

    // The longest run that the edit takes, from a third of the way through the rows, then from other places. The run
    // counts the rows at the depth of its first row.
    const std::vector<hokuto::app::ListRow>& rows = busiest->rows;
    std::optional<std::string> refusal;
    for (const std::size_t start : {rows.size() / 3, rows.size() / 2, 2 * rows.size() / 3, rows.size() / 6})
    {
        const int depth = rows[start].depth;
        for (std::size_t count = kMostRows; count > 0; count--)
        {
            std::set<uint32_t> selected;
            std::size_t level = 0;
            for (std::size_t i = start; i < rows.size() && level < count && rows[i].depth >= depth; i++)
            {
                selected.insert(rows[i].row.command);
                level += rows[i].depth == depth ? 1 : 0;
            }
            const std::optional<hokuto::app::ListEdit> made = hokuto::app::SubroutineEdit(*busiest, selected);
            if (!made)
            {
                continue;
            }
            if (!made->refused.empty())
            {
                refusal = refusal.value_or(made->refused);
                continue;
            }

            Run run{{}, made->depth, made->label};
            for (const std::size_t row : made->rows)
            {
                run.commands.push_back(rows.at(row).row.command);
            }
            try
            {
                edit::ToSubroutine(archive.file, sequence, run.commands, run.depth, run.name);
                return run;
            }
            catch (const std::invalid_argument& error)
            {
                refusal = refusal.value_or(error.what());
            }
        }
    }

    ++totals.refusals["Convert to Subroutine: " + refusal.value_or("no rows")];

    return std::nullopt;
}

// Checks that the sounds that play the sequences of `edited` in `version` play their notes as before. Counts the reads
// and the sounds whose loop moved.
void CompareSounds(const Archive& archive, const File& version, const std::set<uint32_t>& edited,
                   const std::string& round, Totals& totals)
{
    CheckedArchive built;
    try
    {
        hokuto::tools::CheckSaved(version);
        built = hokuto::tools::BuildChecked(version, archive.name);
    }
    catch (const std::exception& error)
    {
        Problem(archive, totals, round + ": the export failed: " + error.what());
        return;
    }

    for (const uint32_t sequence : edited)
    {
        for (const uint32_t sound : archive.by_sequence.at(sequence))
        {
            const song::Sequence& before = archive.songs.at(sound);
            try
            {
                const song::Sequence after = built.Read(sound);
                bool moved = false;
                ++totals.reads;
                if (!SamePlaying(before, after, moved))
                {
                    Problem(archive, totals, round + ": " + before.name + " plays otherwise");
                }
                totals.loops_moved += moved ? 1 : 0;
            }
            catch (const std::exception& error)
            {
                Problem(archive, totals, round + ": " + before.name + " can't be read: " + error.what());
            }
        }
    }
}

// The third round's check on sequence `sequence`: `run` becomes a subroutine, and its call gives way to its commands.
// The commands before the subroutine have to be those of the sequence, and the archive's places those of the file. A
// target, a label or a place at the end of the bytecode goes to the new end.
void SubroutineAndBack(const Archive& archive, uint32_t sequence, const Run& run, Totals& totals)
{
    const File& file = archive.file;
    const Sequence& before = *file.sequences[sequence];
    const std::string where = "sequence " + std::to_string(sequence) + ": ";
    try
    {
        const edit::Edited made = edit::ToSubroutine(file, sequence, run.commands, run.depth, run.name);
        const uint32_t call = edit::Offsets(*made.file.sequences[sequence], edit::ConsoleOf(file)).at(made.added.at(0));
        const File back = edit::DissolveCall(made.file, sequence, call).file;
        const Sequence& after = *back.sequences[sequence];

        const auto size = static_cast<uint32_t>(before.items.size());
        const auto end = static_cast<uint32_t>(after.items.size());
        std::vector<Command> expected = before.items;
        for (Command& command : expected)
        {
            if (ntdlib::commands::HasTarget(command.kind) && !command.target_outside && command.target == size)
            {
                command.target = end;
            }
        }
        const bool same_items =
            after.items.size() >= size && std::equal(expected.begin(), expected.end(), after.items.begin());

        std::vector<Label> labels;
        for (const Label& label : after.labels)
        {
            if (label.name != run.name)
            {
                labels.push_back({label.name, label.item == end ? size : label.item});
            }
        }

        File places = file;
        const auto to_end = [size, end](uint32_t item)
        {
            return item == size ? end : item;
        };
        edit::MoveItemPlaces(places, sequence, to_end);
        if (!same_items || labels != before.labels || places.native->fields != back.native->fields)
        {
            Problem(archive, totals, where + "the subroutine's call didn't give the commands back");
        }
    }
    catch (const std::exception& error)
    {
        Problem(archive, totals, where + "the subroutine and back: " + error.what());
    }
}

// The calls that the first sound of sequence `sequence` ran, by their offsets, in the order of its tracks.
std::vector<uint32_t> CallsRun(const Archive& archive, uint32_t sequence)
{
    const song::Sequence& first = archive.songs.at(archive.by_sequence.at(sequence).front());
    std::vector<uint32_t> calls;
    for (const song::Track& track : first.tracks)
    {
        for (std::size_t i = 1; i < track.stretches.size(); i++)
        {
            const uint32_t call = track.stretches[i - 1].last;
            if (track.stretches[i].entry == ntdlib::sequence_export::Entry::kCall &&
                std::find(calls.begin(), calls.end(), call) == calls.end())
            {
                calls.push_back(call);
            }
        }
    }

    return calls;
}

// The fourth and fifth rounds: the first call of each sequence that `edit_call` takes, all in one version of the file.
// Every sound of an edited sequence then has to play as before. `count` counts the edits.
template <typename EditCall>
void CallRound(const Archive& archive, const std::string& round, std::size_t& count, Totals& totals, EditCall edit_call)
{
    File version = archive.file;
    std::set<uint32_t> edited;
    for (const auto& [sequence, sounds] : archive.by_sequence)
    {
        std::optional<std::string> refusal;
        for (const uint32_t call : CallsRun(archive, sequence))
        {
            try
            {
                File made = edit_call(version, sequence, call).file;
                if (hokuto::tools::Outgrows(archive.archive, made, sequence))
                {
                    ++totals.outgrown;
                    refusal.reset();
                    break;
                }
                version = std::move(made);
                edited.insert(sequence);
                ++count;
                refusal.reset();
                break;
            }
            catch (const std::invalid_argument& error)
            {
                refusal = refusal.value_or(error.what());
            }
            catch (const std::runtime_error& error)
            {
                Problem(archive, totals, "sequence " + std::to_string(sequence) + ": " + round + ": " + error.what());
                refusal.reset();
                break;
            }
        }
        if (refusal)
        {
            ++totals.refusals[round + ": " + *refusal];
        }
    }

    CompareSounds(archive, version, edited, round, totals);
}

// The first round: the pairs of edits on each sequence of `archive` (LabelPairs), then the version in which every label
// receives another name (RenameAll). A GBA song receives some labels first.
void LabelAll(const Archive& archive, Totals& totals)
{
    if (archive.archive.gba || archive.archive.driver)
    {
        Archive labeled = archive;
        labeled.file = WithLabels(archive, archive.file, totals);
        for (uint32_t sequence = 0; sequence < labeled.file.sequences.size(); sequence++)
        {
            LabelPairs(labeled, sequence, totals);
        }
        RenameAll(labeled, archive.file, totals);
    }
    else
    {
        for (uint32_t sequence = 0; sequence < archive.file.sequences.size(); sequence++)
        {
            LabelPairs(archive, sequence, totals);
        }
        RenameAll(archive, archive.file, totals);
    }
}

// Makes a run of each sequence of `archive` a subroutine, all in one version, and then by itself and back, and checks
// that the sounds play as before.
void MakeSubroutines(const Archive& archive, Totals& totals)
{
    File version = archive.file;
    std::set<uint32_t> edited;
    for (const auto& [sequence, sounds] : archive.by_sequence)
    {
        const std::optional<Run> run = RunOf(archive, sequence, totals);
        if (!run)
        {
            continue;
        }

        try
        {
            File made = edit::ToSubroutine(version, sequence, run->commands, run->depth, run->name).file;
            if (hokuto::tools::Outgrows(archive.archive, made, sequence))
            {
                ++totals.outgrown;
                SubroutineAndBack(archive, sequence, *run, totals);
                continue;
            }
            version = std::move(made);
            edited.insert(sequence);
            ++totals.subroutines;
            totals.subroutine_commands += run->commands.size();
        }
        catch (const std::exception& error)
        {
            Problem(archive, totals,
                    "sequence " + std::to_string(sequence) + ": the subroutine failed in the version: " + error.what());
        }
        SubroutineAndBack(archive, sequence, *run, totals);
    }
    CompareSounds(archive, version, edited, "Convert to Subroutine", totals);
}

} // namespace

int main(int argc, char** argv)
{
    std::vector<std::string> arguments = xsflib::console::Arguments(argc, argv);
    hokuto::tools::TakeModelOption(arguments);
    hokuto::tools::TakeFillerOption(arguments);
    if (arguments.size() != 2)
    {
        std::fprintf(stderr, "usage: hokuto_check_subroutines [--model] [--filler] <archive>\n");
        return 2;
    }

    // The archive and the group files beside it make a Hokuto file.
    const std::filesystem::path path = xsflib::PathFromUtf8(arguments[1]);
    Archive archive;
    archive.name = xsflib::Utf8(path.filename());
    try
    {
        std::vector<uint8_t> bytes;
        xsflib::ReadFile(path, bytes);
        archive.archive = hokuto::tools::OpenChecked(path, bytes);
        archive.file = hokuto::tools::CheckedForm(hokuto::tools::ImportChecked(bytes, archive.archive), bytes);
        archive.places = hokuto::tools::PlacesOfChecked(archive.file, bytes, archive.archive);
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "%s: %s\n", archive.name.c_str(), error.what());
        return 2;
    }

    // The sequence sounds with a place, by their sequence.
    Totals totals;
    hokuto::tools::SequenceSounds sequence_sounds =
        hokuto::tools::ReadSequenceSounds(archive.archive, archive.places, {}, archive.songs);
    archive.by_sequence = std::move(sequence_sounds.by_sequence);
    totals.sounds = sequence_sounds.read;
    totals.unreadable = sequence_sounds.unreadable;
    totals.sequences = archive.by_sequence.size();

    // The labels. A GBA song receives some first.
    LabelAll(archive, totals);

    // A run of each sequence becomes a subroutine, all in one version, and then by itself and back.
    MakeSubroutines(archive, totals);

    // The calls.
    const auto dissolve = [](const File& file, uint32_t sequence, uint32_t call)
    {
        return edit::DissolveCall(file, sequence, call);
    };
    const auto copy = [](const File& file, uint32_t sequence, uint32_t call)
    {
        return edit::CopySubroutine(file, sequence, call);
    };
    CallRound(archive, "Dissolve Call", totals.dissolved, totals, dissolve);
    CallRound(archive, "Convert to Real Copy", totals.copied, totals, copy);

    for (const auto& [message, count] : totals.refusals)
    {
        std::printf("%s: refused %zu times: %s\n", archive.name.c_str(), count, message.c_str());
    }
    hokuto::tools::PrintSaves(archive.name);
    std::printf(
        "%s: %zu sequences, %zu sounds read, %zu unreadable. %zu labels took new names, %zu new labels went in and %zu "
        "labels moved, and back. %zu new names read back. %zu subroutines of %zu commands. %zu calls dissolved and %zu "
        "copied. %zu reads, %zu with the loop on another pass. %zu problems.\n",
        archive.name.c_str(), totals.sequences, totals.sounds, totals.unreadable, totals.renamed, totals.added,
        totals.moved, totals.names_read, totals.subroutines, totals.subroutine_commands, totals.dissolved,
        totals.copied, totals.reads, totals.loops_moved, totals.problems);
    if (archive.archive.gba || archive.archive.driver)
    {
        std::printf(
            "%s: on the GBA: %zu labels went in first: the ROM keeps none. %zu times, an edit of a song stayed "
            "out: the song outgrew its place, and the ROM has no room for it.\n",
            archive.name.c_str(), totals.given, totals.outgrown);
    }

    return totals.problems == 0 ? 0 : 1;
}
