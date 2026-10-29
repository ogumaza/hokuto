// SPDX-License-Identifier: MIT

// Checks the edits of notes (src/edit/notes.h) on a sound archive of the Wii, the 3DS, the Wii U or the Switch. The
// tool imports the archive into a Hokuto file the way the program does. It reads each sequence sound with ntdlib's
// performer. It then edits every note command that a sound plays, in one version of the file. The commands take turns
// at four edits:
//
// - The note moves a tick later, or a tick earlier near the end of its block. A note moved onto the tick at which its
//   track ends would stop at once.
// - Its velocity goes one down, or one up from 1.
// - The note goes.
// - The note becomes a tick longer.
//
// A note that can't move or change its length on its track changes its velocity instead. So does a note whose sequence
// refuses its edit, such as the removal of a note that makes its track wait until the track's notes have ended. The
// tool lists and counts each refusal. It then exports the edited file and checks two things:
//
// - The archive, imported again, gives back the edited sequences. Each edited command holds its edit.
// - Each sound plays the notes that it played before, each with its command's edit, and no other notes. The performer
//   reads the edited archive.
//
// The second check counts some differences apart. A note's length can come from something other than its command: the
// end of a one-shot wave, a note that cuts it, or the end of its track. A velocity can come from the track too. A note
// that another note cuts in the tick it starts doesn't show in the arrangement. It shows once an edit moves or removes
// the other note. A move can take a note past a command that changes it, such as a transpose or a program change at its
// old or new tick. The note then plays with another key or length. A note that played no instrument can sound after
// such a move. When the performer marks another pass of a loop after the edits, the check compares only the notes
// before the end of the earlier loop. Where the loops don't lie whole passes apart, it compares the notes before the
// earlier end of a first pass, or of a read without a loop. On a track that ties its notes, or plays one note at a
// time, a note that starts while the one before it still sounds joins that note. After a move or a longer length, two
// notes can join that didn't before. Only the notes of the tracks that ran a command that turns on ties or monophonic
// play count apart that way. Where a tick is shorter than the console's frame, as at times on the 3DS, a note of a tick
// or two can end in the frame where it starts, and the arrangement leaves it out. A move can then show or hide such a
// note, and a note of length 0 that becomes a tick long can vanish. The tool counts apart every note of 2 ticks or
// fewer that differs, on every console and at every tempo. A track can wait for its notes' ends: with the note wait on,
// a note of length 0 waits until the track's notes have ended. A note of a length that the sequence decides as it plays
// waits for an unknown time. An edit of an earlier note's tick, length or velocity can change when the notes end, and
// with it the ticks of the later notes. Only the notes of such tracks count apart that way. A sound that draws random
// numbers draws others after an edit that removes or moves a draw. A sound whose tempo comes from a variable can stand
// still at its variables' starting values: a note that a move puts after a wait never plays. The tool lists such
// sounds, but doesn't count them as differing.
//
// With --timelines, the tool opens the file's timelines as the program does (edit/timelines.h). Each edit of a timeline
// then takes the converter's layout as the program settles it (edit::SettleTimelines). An edit that the layout can't
// hold counts as a refusal. The sounds play from the archive that the program plays, with every bar in its track. The
// archive that a saved file holds, with the converter's subroutines, has to play the same notes. The file that it
// imports into, with the timelines' hints, has to give back the settled sequences.
//
// The tool prints a line for each sound that differs, and a summary. It returns 1 when a sound differs, a command lacks
// its edit, or an edit, a read or the export fails. With a sound's name, it checks that sound alone.
//
// With --model, the file goes into the model form before the edits (format::InModelForm), and the archive that the
// model builds has to be the game's. Each file that the edits make then has to save in both forms and open again as it
// was (tools/checked_archive.h).
//
// usage: hokuto_check_edits [--model] [--timelines] <archive> [sound]

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/commands/units.h"
#include "xsflib/console.h"
#include "xsflib/files.h"

#include "checked_archive.h"
#include "edit/bytecode.h"
#include "edit/commands.h"
#include "edit/notes.h"
#include "edit/timelines.h"
#include "format/archive.h"
#include "format/file.h"
#include "loop_passes.h"
#include "song/archive.h"
#include "song/sequence.h"

namespace
{

using hokuto::edit::NoteEdit;
using hokuto::format::File;
using hokuto::song::Note;
using hokuto::song::Sequence;
using hokuto::tools::CheckedArchive;
using hokuto::tools::EarlierEnd;
using ntdlib::commands::Command;
using ntdlib::commands::Kind;
using ntdlib::commands::kOne;

// What an edit does to a note command.
enum class Change
{
    kMove,
    kVelocity,
    kRemove,
    kLonger,
};

// The edit of a note command, and what it should do to the notes that the command plays.
struct Plan
{
    Change change = Change::kVelocity;
    NoteEdit edit;
};

// What the check counted, for the summary.
struct Totals
{
    std::size_t sounds = 0;
    std::size_t notes = 0;
    std::size_t commands = 0;
    std::size_t moved = 0;
    std::size_t velocities = 0;
    std::size_t removed = 0;
    std::size_t longer = 0;
    std::size_t refused = 0;
    std::size_t wrong_commands = 0;
    std::size_t differ = 0;
    std::size_t wrong_notes = 0;
    std::size_t unread = 0;
    std::size_t legato = 0;
    std::size_t waits = 0; // sounds with a track that waits for its notes' ends, and that play other notes
    std::size_t hidden = 0;
    std::size_t changed_moves = 0;
    std::size_t ticks = 0;  // notes of a tick or two that the frames show or hide
    std::size_t random = 0; // sounds that draw random numbers and play other notes
    std::size_t tempo = 0;  // sounds whose tempo a variable decides, and that play other notes
    std::size_t other_passes = 0;
    std::size_t other_lengths = 0;
    std::size_t other_velocities = 0;
    std::size_t saved = 0;
};

// A note as the check compares it: its track, start, key, length and velocity.
using Played = std::tuple<int, uint32_t, uint32_t, uint32_t, uint32_t>;

// The item at each offset of a sequence's bytecode on `console`.
std::map<uint32_t, uint32_t> ItemsByOffset(const ntdlib::commands::Sequence& sequence,
                                           const hokuto::song::Console& console)
{
    const std::vector<uint32_t> offsets = hokuto::edit::Offsets(sequence, console);
    std::map<uint32_t, uint32_t> items;
    for (uint32_t i = 0; i + 1 < offsets.size(); ++i)
    {
        items[offsets[i]] = i;
    }

    return items;
}

// A plan that changes the velocity of `command` in place of `plan`: one down, or one up from 1.
Plan VelocityPlan(const Command& command, const Plan& plan)
{
    Plan changed;
    changed.edit.command = plan.edit.command;
    changed.edit.stretch = plan.edit.stretch;
    const int velocity = static_cast<int>(command.velocity / kOne);
    changed.edit.velocity = velocity <= 1 ? velocity + 1 : velocity - 1;

    return changed;
}

// The plan for `command`, the note command of `note`, in sequence `sequence` of `file`, run in `stretch`. The commands
// take turns by `turn`.
Plan PlanFor(const File& file, uint32_t sequence, const Note& note, const hokuto::song::Stretch& stretch,
             const Command& command, std::size_t turn)
{
    Plan plan;
    plan.edit.command = note.command;
    plan.edit.stretch = stretch;
    const std::optional<hokuto::edit::ShiftRange> range =
        hokuto::edit::ShiftRangeOf(file, sequence, note.command, stretch);
    switch (turn % 4)
    {
    case 0:
        if (range && (range->later > 1 || range->earlier > 0))
        {
            plan.change = Change::kMove;
            plan.edit.shift = range->later > 1 ? 1 : -1;
            return plan;
        }
        break;

    case 2:
        plan.change = Change::kRemove;
        plan.edit.remove = true;
        return plan;

    case 3:
        if (range && command.value.source == ntdlib::commands::Source::kValue && command.value.value % kOne == 0)
        {
            plan.change = Change::kLonger;
            plan.edit.length = static_cast<uint32_t>(command.value.value / kOne) + 1;
            return plan;
        }
        break;

    default:
        break;
    }

    // Otherwise the velocity changes.
    return VelocityPlan(command, plan);
}

// The edits of `plans`, in the order of their commands.
std::vector<NoteEdit> EditsOf(const std::map<uint32_t, Plan>& plans)
{
    std::vector<NoteEdit> edits;
    for (const auto& [command, plan] : plans)
    {
        edits.push_back(plan.edit);
    }

    return edits;
}

// The file after the program makes `edits` to sequence `sequence` of `file`: an edit of a timeline then takes the
// converter's layout. `Edited::items` gives the new index of each old item in that layout.
hokuto::edit::Edited EditAsProgram(const File& file, uint32_t sequence, std::span<const NoteEdit> edits)
{
    hokuto::edit::Edited edited = hokuto::edit::EditNotes(file, sequence, edits);
    if (!hokuto::edit::IsTimeline(file, sequence))
    {
        return edited;
    }

    hokuto::edit::Settled settled = hokuto::edit::SettleTimelines(file, std::move(edited.file));
    const auto laid = settled.items.find(sequence);
    if (laid != settled.items.end())
    {
        for (std::optional<uint32_t>& item : edited.items)
        {
            item = item ? laid->second.at(*item) : std::nullopt;
        }
    }
    edited.file = std::move(settled.file);

    return edited;
}

// The message with which sequence `sequence` of `file` refuses `edit`. Nothing if it takes the edit.
std::optional<std::string> Refusal(const File& file, uint32_t sequence, const NoteEdit& edit)
{
    try
    {
        EditAsProgram(file, sequence, std::span(&edit, 1));
    }
    catch (const std::invalid_argument& error)
    {
        return error.what();
    }

    return std::nullopt;
}

// The name of `change`, for a line about it.
const char* NameOf(Change change)
{
    switch (change)
    {
    case Change::kMove:
        return "move";
    case Change::kVelocity:
        return "velocity";
    case Change::kRemove:
        return "removal";
    case Change::kLonger:
        return "longer length";
    }

    return "";
}

// The length of note command `command`, for a line about it.
std::string LengthOf(const Command& command)
{
    switch (command.value.source)
    {
    case ntdlib::commands::Source::kValue:
        return std::to_string(static_cast<double>(command.value.value) / kOne) + " ticks";
    case ntdlib::commands::Source::kRandom:
        return "a random length";
    case ntdlib::commands::Source::kVariable:
        return "the length in variable " + std::to_string(command.value.value);
    }

    return "";
}

// The notes of `sequence` that start before `end`, in the form that the check compares. `commands` takes the offset of
// each note's command.
std::vector<Played> NotesOf(const Sequence& sequence, uint32_t end, std::map<Played, uint32_t>& commands)
{
    std::vector<Played> notes;
    for (const hokuto::song::Track& track : sequence.tracks)
    {
        for (const Note& note : track.notes)
        {
            if (note.start < end)
            {
                const Played& played =
                    notes.emplace_back(track.number, note.start, note.key, note.length, note.velocity);
                commands.emplace(played, note.command);
            }
        }
    }
    std::sort(notes.begin(), notes.end());

    return notes;
}

// The longest note, in ticks, that the frames can show or hide where a tick is shorter than a frame.
constexpr uint32_t kShortNote = 2;

// Whether a command that `song` ran up to the end of the loop's first pass sets the tempo from a variable or a random
// number. Sequence `sequence` of `file` played `song`.
bool TempoUndecided(const File& file, uint32_t sequence, const Sequence& song)
{
    const ntdlib::commands::Sequence& commands = *file.sequences[sequence];
    const std::vector<uint32_t> offsets = hokuto::edit::Offsets(commands, hokuto::edit::ConsoleOf(file));
    const std::optional<uint32_t> end = song.loop ? std::optional<uint32_t>(song.loop->end) : std::nullopt;
    for (const hokuto::song::Track& track : song.tracks)
    {
        for (const hokuto::edit::CommandRow& row :
             hokuto::edit::CommandRows(commands, offsets, track.stretches, track.notes, end))
        {
            const std::optional<uint32_t> item = hokuto::edit::ItemAtOffset(offsets, row.command);
            if (item && commands.items[*item].kind == Kind::kTempo &&
                commands.items[*item].value.source != ntdlib::commands::Source::kValue)
            {
                return true;
            }
        }
    }

    return false;
}

// The notes that start before `end` when `before` plays after the plans of its sequence's commands. `edits` takes the
// edit of each note, for a line about it. `moves` takes the notes that move, and `ticks` the notes of a command of
// length 0 that becomes a tick long.
std::vector<Played> Expected(const Sequence& before, const std::map<uint32_t, Plan>& plans, uint32_t end,
                             std::map<Played, std::string>& edits, std::set<Played>& moves, std::set<Played>& ticks)
{
    std::vector<Played> notes;
    for (const hokuto::song::Track& track : before.tracks)
    {
        for (const Note& note : track.notes)
        {
            Played played{track.number, note.start, note.key, note.length, note.velocity};
            const auto plan = plans.find(note.command);
            const char* what = "unedited";
            if (plan != plans.end())
            {
                switch (plan->second.change)
                {
                case Change::kMove:
                    std::get<1>(played) =
                        static_cast<uint32_t>(static_cast<int64_t>(note.start) + plan->second.edit.shift);
                    moves.insert(played);
                    what = "moved";
                    break;

                case Change::kVelocity:
                    std::get<4>(played) = static_cast<uint32_t>(*plan->second.edit.velocity);
                    what = "with a new velocity";
                    break;

                case Change::kRemove:
                    continue;

                case Change::kLonger:
                    std::get<3>(played) = note.length + 1;
                    what = "longer";
                    if (plan->second.edit.length == 1u)
                    {
                        ticks.insert(played);
                    }
                    break;
                }
            }

            if (std::get<1>(played) < end)
            {
                edits[played] = std::string(what) + ", from tick " + std::to_string(note.start) + ", command at " +
                                std::to_string(note.command);
                notes.push_back(played);
            }
        }
    }
    std::sort(notes.begin(), notes.end());

    return notes;
}

// `note` without its length, to compare.
Played WithoutLength(Played note)
{
    std::get<3>(note) = 0;

    return note;
}

// `note` without its velocity, to compare.
Played WithoutVelocity(Played note)
{
    std::get<4>(note) = 0;

    return note;
}

// The notes of `notes` that no note of `others` matches, but for the part that `strip` takes out.
std::vector<Played> Unmatched(const std::vector<Played>& notes, const std::vector<Played>& others,
                              Played (*strip)(Played))
{
    std::vector<Played> unmatched;
    for (const Played& note : notes)
    {
        const auto same = [&](const Played& other)
        {
            return strip(other) == strip(note);
        };
        if (std::none_of(others.begin(), others.end(), same))
        {
            unmatched.push_back(note);
        }
    }

    return unmatched;
}

// The notes of `a` that `b` lacks. Both are sorted.
std::vector<Played> Without(const std::vector<Played>& a, const std::vector<Played>& b)
{
    std::vector<Played> result;
    std::set_difference(a.begin(), a.end(), b.begin(), b.end(), std::back_inserter(result));

    return result;
}

// The loop and the length of `sequence`, for a line about it.
std::string Shape(const Sequence& sequence)
{
    const std::string loop =
        sequence.loop ? "loop " + std::to_string(sequence.loop->start) + " to " + std::to_string(sequence.loop->end)
                      : "no loop";
    return loop + ", " + std::to_string(sequence.length) + " ticks";
}

// The tick where `sequence` ends the first pass of its loop, or its end without a loop.
uint32_t FirstPassEnd(const Sequence& sequence)
{
    return sequence.loop ? std::min(sequence.loop->end, sequence.length) : sequence.length;
}

// The tracks of `song`, a sound that plays `sequence` on `console`, that ran a command that turns on ties or monophonic
// play. A note of such a track that starts while the one before it still sounds joins that note.
std::set<int> LegatoTracks(const ntdlib::commands::Sequence& sequence, const hokuto::song::Console& console,
                           const Sequence& song)
{
    const std::vector<uint32_t> offsets = hokuto::edit::Offsets(sequence, console);
    std::set<int> tracks;
    for (const hokuto::song::Track& track : song.tracks)
    {
        for (const hokuto::song::Stretch& stretch : track.stretches)
        {
            const std::optional<uint32_t> first = hokuto::edit::ItemAtOffset(offsets, stretch.first);
            const std::optional<uint32_t> last = hokuto::edit::ItemAtOffset(offsets, stretch.last);
            if (!first || !last)
            {
                continue;
            }

            for (uint32_t item = *first; item <= *last; ++item)
            {
                const Command& command = sequence.items[item];
                const bool on = command.value.source != ntdlib::commands::Source::kValue || command.value.value != 0;
                if ((command.kind == Kind::kTie || command.kind == Kind::kMonophonic) && on)
                {
                    tracks.insert(track.number);
                }
            }
        }
    }

    return tracks;
}

// The tracks of `song`, a sound that plays `sequence` on `console`, that ran a note with the note wait on whose wait
// the bytecode doesn't give: a note of length 0 waits until the track's notes have ended, and a note of a length from a
// variable or a random number waits for a length that the sequence decides as it plays. A comparison can decide whether
// a note waits. An edit of an earlier note's tick, length or velocity can then move the track's later notes.
std::set<int> UnknownWaitTracks(const ntdlib::commands::Sequence& sequence, const hokuto::song::Console& console,
                                const Sequence& song)
{
    const std::vector<uint32_t> offsets = hokuto::edit::Offsets(sequence, console);
    std::set<int> tracks;
    for (const hokuto::song::Track& track : song.tracks)
    {
        for (const hokuto::song::Stretch& stretch : track.stretches)
        {
            const std::optional<uint32_t> first = hokuto::edit::ItemAtOffset(offsets, stretch.first);
            const std::optional<uint32_t> last = hokuto::edit::ItemAtOffset(offsets, stretch.last);
            if (!first || !last)
            {
                continue;
            }

            bool note_wait = stretch.note_wait;
            for (uint32_t item = *first; item <= *last; ++item)
            {
                const Command& command = sequence.items[item];
                const bool known = command.value.source == ntdlib::commands::Source::kValue && !command.conditional;
                if (command.kind == Kind::kNoteWait)
                {
                    note_wait = !known || command.value.value != 0;
                }
                if (command.kind == Kind::kNote && note_wait &&
                    (!known || command.value.value <= 0 || command.value.value % kOne != 0))
                {
                    tracks.insert(track.number);
                }
            }
        }
    }

    return tracks;
}

// Whether `edited`, the command that `old` became, holds `plan`'s edit. A removed note leaves no command, or a wait.
bool HoldsEdit(const Command& old, const Plan& plan, const Command* edited)
{
    if (plan.change == Change::kRemove)
    {
        return edited == nullptr || edited->kind == Kind::kWait;
    }

    Command expected = old;
    if (plan.edit.velocity)
    {
        expected.velocity = *plan.edit.velocity * kOne;
    }
    if (plan.edit.length)
    {
        expected.value.value = static_cast<int64_t>(*plan.edit.length) * kOne;
        expected.encoding.value_bytes = 0;
    }

    return edited != nullptr && *edited == expected;
}

} // namespace

int main(int argc, char** argv)
{
    std::vector<std::string> arguments = xsflib::console::Arguments(argc, argv);
    hokuto::tools::TakeModelOption(arguments);
    const bool timelines = arguments.size() > 1 && arguments[1] == "--timelines";
    const std::size_t first = timelines ? 2 : 1;
    if (arguments.size() < first + 1 || arguments.size() > first + 2)
    {
        std::fprintf(stderr, "usage: hokuto_check_edits [--model] [--timelines] <archive> [sound]\n");
        return 2;
    }
    const std::string only = arguments.size() > first + 1 ? arguments[first + 1] : std::string();

    // The archive and the group files beside it make a Hokuto file. `playing` is the archive that the program plays.
    const std::filesystem::path path = xsflib::PathFromUtf8(arguments[first]);
    const std::string name = xsflib::Utf8(path.filename());
    std::vector<uint8_t> bytes;
    CheckedArchive archive;
    CheckedArchive playing;
    File file;
    std::vector<std::optional<uint32_t>> sequences;
    try
    {
        xsflib::ReadFile(path, bytes);
        archive = hokuto::tools::OpenChecked(path, bytes);
        file = hokuto::tools::CheckedForm(hokuto::tools::ImportChecked(bytes, archive), bytes);
        sequences = hokuto::tools::SequencesOfChecked(file, bytes, archive);
        playing = archive;
        if (timelines)
        {
            file = hokuto::edit::OpenTimelines(std::move(file));
            playing = hokuto::tools::BuildChecked(file, name);
        }
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "%s: %s\n", name.c_str(), error.what());
        return 2;
    }

    // Each sequence sound before the edits, and the plan of each note command, from the first sound that plays it.
    Totals totals;
    std::vector<std::optional<Sequence>> before(archive.SoundCount());
    std::map<uint32_t, std::map<uint32_t, Plan>> plans;
    std::map<uint32_t, std::map<uint32_t, uint32_t>> items;

    const auto sequence_of = [&](uint32_t sound)
    {
        const uint32_t file_id = archive.FileOf(sound);
        return file_id < sequences.size() ? sequences[file_id] : std::nullopt;
    };

    std::size_t turn = 0;
    for (uint32_t sound = 0; sound < archive.SoundCount(); ++sound)
    {
        const std::string& sound_name = archive.NameOf(sound);
        if (!archive.IsSequence(sound) || (!only.empty() && sound_name != only))
        {
            continue;
        }

        ++totals.sounds;
        try
        {
            before[sound] = playing.Read(sound);
        }
        catch (const std::exception& error)
        {
            std::printf("%s: %s can't be read before the edits: %s\n", name.c_str(), sound_name.c_str(), error.what());
            ++totals.unread;
            continue;
        }

        const std::optional<uint32_t> sequence = sequence_of(sound);
        if (!sequence)
        {
            continue;
        }

        std::map<uint32_t, uint32_t>& by_offset = items[*sequence];
        if (by_offset.empty())
        {
            by_offset = ItemsByOffset(*file.sequences[*sequence], hokuto::edit::ConsoleOf(file));
        }
        for (const hokuto::song::Track& track : before[sound]->tracks)
        {
            for (const Note& note : track.notes)
            {
                ++totals.notes;
                if (plans[*sequence].contains(note.command) || !by_offset.contains(note.command))
                {
                    continue;
                }

                const Command& command = file.sequences[*sequence]->items[by_offset.at(note.command)];
                plans[*sequence][note.command] =
                    PlanFor(file, *sequence, note, track.stretches.at(note.stretch), command, turn++);
            }
        }
    }

    // The edits, sequence by sequence, in one version of the file, and where each sequence's items went. When a
    // sequence refuses its edits, each edit alone shows which ones it refuses. Those change the note's velocity
    // instead.
    File edited = file;
    bool failed = false;
    std::map<uint32_t, std::vector<std::optional<uint32_t>>> moved;
    for (auto& [sequence, by_command] : plans)
    {
        std::optional<hokuto::edit::Edited> result;
        try
        {
            result = EditAsProgram(edited, sequence, EditsOf(by_command));
        }
        catch (const std::invalid_argument&)
        {
            for (auto& [offset, plan] : by_command)
            {
                if (const std::optional<std::string> refusal = Refusal(edited, sequence, plan.edit))
                {
                    const Command& command = file.sequences[sequence]->items[items[sequence].at(offset)];
                    std::printf(
                        "%s: sequence %u refuses the %s of the note at %u, of %s, with the note wait %s at the "
                        "start of its stretch: %s\n",
                        name.c_str(), sequence, NameOf(plan.change), offset, LengthOf(command).c_str(),
                        plan.edit.stretch.note_wait ? "on" : "off", refusal->c_str());
                    ++totals.refused;
                    plan = VelocityPlan(command, plan);
                }
            }
        }
        catch (const std::exception& error)
        {
            std::printf("%s: sequence %u can't take its edits: %s\n", name.c_str(), sequence, error.what());
            failed = true;
            continue;
        }

        try
        {
            if (!result)
            {
                result = EditAsProgram(edited, sequence, EditsOf(by_command));
            }
            edited = std::move(result->file);
            moved[sequence] = std::move(result->items);
        }
        catch (const std::exception& error)
        {
            std::printf("%s: sequence %u can't take its edits: %s\n", name.c_str(), sequence, error.what());
            failed = true;
        }
    }

    // The plans as they stand after the refusals.
    for (const auto& [sequence, by_command] : plans)
    {
        for (const auto& [command, plan] : by_command)
        {
            ++totals.commands;
            totals.moved += plan.change == Change::kMove ? 1 : 0;
            totals.velocities += plan.change == Change::kVelocity ? 1 : 0;
            totals.removed += plan.change == Change::kRemove ? 1 : 0;
            totals.longer += plan.change == Change::kLonger ? 1 : 0;
        }
    }

    // The edited archive that a saved file holds, the archive that the program plays, and the file that the first
    // imports into again. That file takes the timelines' hints that a saved file keeps.
    CheckedArchive saved;
    CheckedArchive after;
    File again;
    try
    {
        const File folded = hokuto::edit::FoldTimelines(edited);
        hokuto::tools::CheckSaved(folded);
        const std::vector<uint8_t> exported = hokuto::format::ExportArchive(folded);
        saved = hokuto::tools::BuildChecked(folded, name);
        after = timelines ? hokuto::tools::BuildChecked(edited, name) : saved;
        again = hokuto::tools::ImportChecked(exported, archive, hokuto::format::ExportGroupFiles(folded));
        if (timelines)
        {
            again.timelines = edited.timelines;
            again = hokuto::edit::OpenTimelines(std::move(again));
        }
    }
    catch (const std::exception& error)
    {
        std::printf("%s: the edited file can't be exported: %s\n", name.c_str(), error.what());
        return 1;
    }

    // The import gives back the edited sequences, and each edited command holds its edit. The note commands that no
    // plan names are the ones that the arrangement hid. `hidden` takes their offsets after the edits, and `moved_notes`
    // the offsets of the moved notes.
    std::map<uint32_t, std::set<uint32_t>> hidden;
    std::map<uint32_t, std::set<uint32_t>> moved_notes;
    for (const auto& [sequence, by_command] : plans)
    {
        if (!moved.contains(sequence))
        {
            continue;
        }

        if (again.sequences.size() != edited.sequences.size() ||
            again.sequences[sequence] != edited.sequences[sequence])
        {
            std::printf("%s: sequence %u doesn't come back from the edited archive\n", name.c_str(), sequence);
            failed = true;
        }
        const std::vector<Command>& commands = edited.sequences[sequence]->items;
        const std::vector<uint32_t> offsets =
            hokuto::edit::Offsets(*edited.sequences[sequence], hokuto::edit::ConsoleOf(edited));
        for (const auto& [offset, plan] : by_command)
        {
            const uint32_t old = items[sequence].at(offset);
            const std::optional<uint32_t> item = moved[sequence].at(old);
            if (!HoldsEdit(file.sequences[sequence]->items[old], plan, item ? &commands.at(*item) : nullptr))
            {
                std::printf("%s: sequence %u: the command at %u lacks its edit\n", name.c_str(), sequence, offset);
                ++totals.wrong_commands;
            }
            if (item && plan.change == Change::kMove)
            {
                moved_notes[sequence].insert(offsets[*item]);
            }
        }

        for (const auto& [offset, old] : items[sequence])
        {
            const std::optional<uint32_t> item = moved[sequence].at(old);
            const Command& command = file.sequences[sequence]->items[old];
            if (item && command.kind == Kind::kNote && !by_command.contains(offset) && commands.at(*item) == command)
            {
                hidden[sequence].insert(offsets[*item]);
            }
        }
    }

    // Each sound again, from the edited archive.
    for (uint32_t sound = 0; sound < archive.SoundCount(); ++sound)
    {
        const std::string& sound_name = archive.NameOf(sound);
        const std::optional<uint32_t> sequence = sequence_of(sound);
        if (!before[sound] || !sequence)
        {
            continue;
        }

        std::optional<Sequence> read;
        try
        {
            read = after.Read(sound);
            if (timelines)
            {
                // The saved archive plays the same notes, with the same loop and length.
                const Sequence from_saved = saved.Read(sound);
                std::map<Played, uint32_t> saved_commands;
                std::map<Played, uint32_t> commands;
                if (NotesOf(from_saved, UINT32_MAX, saved_commands) != NotesOf(*read, UINT32_MAX, commands) ||
                    from_saved.loop != read->loop || from_saved.length != read->length)
                {
                    std::printf("%s: %s plays otherwise from the saved archive\n", name.c_str(), sound_name.c_str());
                    ++totals.saved;
                }
            }
        }
        catch (const std::exception& error)
        {
            std::printf("%s: %s can't be read after the edits: %s\n", name.c_str(), sound_name.c_str(), error.what());
            ++totals.unread;
            continue;
        }

        // When the loop moves to another pass, the notes compare up to the earlier loop's end where the performer finds
        // the loop a whole number of passes away. Otherwise they compare up to the earlier end of a first pass, or of a
        // read without a loop. A difference only in a note's length or only in its velocity counts apart.
        const bool other_pass = read->loop != before[sound]->loop;
        const uint32_t first_passes = std::min(FirstPassEnd(*read), FirstPassEnd(*before[sound]));
        const uint32_t end = other_pass ? EarlierEnd(*before[sound], *read).value_or(first_passes) : UINT32_MAX;
        std::map<Played, std::string> edits;
        std::set<Played> moves;
        std::set<Played> ticks;
        const std::vector<Played> expected = Expected(*before[sound], plans[*sequence], end, edits, moves, ticks);
        std::map<Played, uint32_t> commands;
        const std::vector<Played> actual = NotesOf(*read, end, commands);
        const std::vector<Played> missing = Without(expected, actual);
        const std::vector<Played> extra = Without(actual, expected);
        std::vector<Played> unexpected;
        for (const Played& note : Unmatched(Unmatched(extra, missing, WithoutLength), missing, WithoutVelocity))
        {
            // A move can take a note that played no instrument past a command that gives it one. No moved note that
            // played before stands at the same tick then.
            const auto moved_here = [&note, &moves](const Played& other)
            {
                return moves.contains(other) && std::get<0>(other) == std::get<0>(note) &&
                       std::get<1>(other) == std::get<1>(note);
            };
            if (hidden[*sequence].contains(commands.at(note)))
            {
                ++totals.hidden;
            }
            else if (moved_notes[*sequence].contains(commands.at(note)) && std::ranges::none_of(missing, moved_here))
            {
                ++totals.changed_moves;
            }
            else if (std::get<3>(note) <= kShortNote)
            {
                ++totals.ticks;
            }
            else
            {
                unexpected.push_back(note);
            }
        }

        // A moved note that another command changes plays at its new tick with another key or length.
        std::vector<Played> wrong;
        for (const Played& note : Unmatched(Unmatched(missing, extra, WithoutLength), extra, WithoutVelocity))
        {
            const auto same_tick = [&note](const Played& other)
            {
                return std::get<0>(other) == std::get<0>(note) && std::get<1>(other) == std::get<1>(note);
            };
            const auto changed = std::find_if(unexpected.begin(), unexpected.end(), same_tick);
            if (moves.contains(note) && changed != unexpected.end())
            {
                unexpected.erase(changed);
                ++totals.changed_moves;
            }
            else if ((ticks.contains(note) || std::get<3>(note) <= kShortNote) && changed == unexpected.end())
            {
                ++totals.ticks;
            }
            else
            {
                wrong.push_back(note);
            }
        }
        totals.other_lengths += missing.size() - Unmatched(missing, extra, WithoutLength).size();
        totals.other_velocities += missing.size() - Unmatched(missing, extra, WithoutVelocity).size();
        totals.other_passes += other_pass ? 1 : 0;
        if (wrong.empty() && unexpected.empty())
        {
            continue;
        }

        // Only the notes of tracks that tie their notes or play one at a time can join others.
        const std::set<int> legato_tracks =
            LegatoTracks(*file.sequences[*sequence], hokuto::edit::ConsoleOf(file), *before[sound]);
        const auto joins = [&legato_tracks](const Played& note)
        {
            return legato_tracks.contains(std::get<0>(note));
        };
        const bool legato = std::ranges::all_of(wrong, joins) && std::ranges::all_of(unexpected, joins);

        // A track that waits for its notes' ends can play its later notes at other ticks after an edit.
        const std::set<int> waiting_tracks =
            UnknownWaitTracks(*file.sequences[*sequence], hokuto::edit::ConsoleOf(file), *before[sound]);
        const auto waits_for_ends = [&waiting_tracks](const Played& note)
        {
            return waiting_tracks.contains(std::get<0>(note));
        };
        const bool waits =
            !legato && std::ranges::all_of(wrong, waits_for_ends) && std::ranges::all_of(unexpected, waits_for_ends);
        const bool random = !legato && !waits && hokuto::edit::DrawsRandom(file, *sequence, *before[sound]);
        const bool tempo = !legato && !waits && !random && TempoUndecided(file, *sequence, *before[sound]);
        totals.legato += legato ? 1 : 0;
        totals.waits += waits ? 1 : 0;
        totals.random += random ? 1 : 0;
        totals.tempo += tempo ? 1 : 0;
        const bool apart = legato || waits || random || tempo;
        totals.differ += apart ? 0 : 1;
        totals.wrong_notes += apart ? 0 : wrong.size() + unexpected.size();
        std::printf("%s: %s%s: %zu of %zu notes differ, and %zu notes are new. Before: %s. After: %s.\n", name.c_str(),
                    sound_name.c_str(),
                    legato   ? " (legato)"
                    : waits  ? " (waits for its notes' ends)"
                    : random ? " (draws random numbers)"
                    : tempo  ? " (tempo from a variable)"
                             : "",
                    wrong.size(), expected.size(), unexpected.size(), Shape(*before[sound]).c_str(),
                    Shape(*read).c_str());
        for (const auto& [notes, what] :
             {std::pair(&std::as_const(wrong), "missing"), std::pair(&std::as_const(unexpected), "new")})
        {
            for (std::size_t i = 0; i < std::min<std::size_t>(notes->size(), 4); ++i)
            {
                const Played& note = (*notes)[i];
                const auto& [track, start, key, length, velocity] = note;
                std::printf("    %s: track %d, tick %u, key %u, %u ticks, velocity %u (%s)\n", what, track, start, key,
                            length, velocity, edits.contains(note) ? edits.at(note).c_str() : "in no plan");
            }
        }
    }

    std::size_t timeline_count = 0;
    for (uint32_t sequence = 0; sequence < file.sequences.size(); ++sequence)
    {
        timeline_count += hokuto::edit::IsTimeline(file, sequence) ? 1 : 0;
    }
    const std::string saved_line = timelines ? " " + std::to_string(timeline_count) + " sequences are timelines, and " +
                                                   std::to_string(totals.saved) +
                                                   " sounds play otherwise from the saved archive."
                                             : "";
    std::printf(
        "%s: %zu sequence sounds, %zu notes from %zu commands: %zu moved, %zu with a new velocity, %zu removed and %zu "
        "longer. %zu edits were refused. %zu commands lack their edits. %zu sounds differ by %zu notes, and %zu can't "
        "be read. Apart: %zu sounds with legatos that differ, %zu with a track that waits for its notes' ends and "
        "differ, %zu that draw random numbers and differ, %zu whose tempo a variable decides and differ, %zu with the "
        "loop on another pass, %zu notes that the arrangement hid before the edits, %zu moved notes that other "
        "commands change, %zu notes of a tick or two that the frames show or hide, and %zu lengths and %zu velocities "
        "that don't come from the command.%s\n",
        name.c_str(), totals.sounds, totals.notes, totals.commands, totals.moved, totals.velocities, totals.removed,
        totals.longer, totals.refused, totals.wrong_commands, totals.differ, totals.wrong_notes, totals.unread,
        totals.legato, totals.waits, totals.random, totals.tempo, totals.other_passes, totals.hidden,
        totals.changed_moves, totals.ticks, totals.other_lengths, totals.other_velocities, saved_line.c_str());
    hokuto::tools::PrintSaves(name);

    const bool passed =
        totals.differ == 0 && totals.wrong_commands == 0 && totals.unread == 0 && totals.saved == 0 && !failed;
    return passed ? 0 : 1;
}
