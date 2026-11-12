// SPDX-License-Identifier: MIT

// Checks the edits of notes (src/edit/notes.h) on a sound archive of the Wii, the 3DS, the Wii U, the Switch or the DS,
// or on a GBA ROM with another of the GBA's drivers, such as Konami's. The tool imports the archive into a Hokuto file
// the way the program does. A DS cartridge image gives its largest sound archive, or its DSE files when it contains
// none (tools/checked_archive.h). It reads each sequence sound with ntdlib's performer. It then edits every note
// command that a sound plays, in one version of the file. The commands take turns at four edits:
//
// - The note moves a tick later, or a tick earlier near the end of its block. A note moved onto the tick at which its
//   track ends would stop at once.
// - Its velocity goes one down, or one up from 1.
// - The note goes.
// - The note becomes a tick longer.
//
// A note that can't move or change its length on its track changes its velocity instead. So does a note whose sequence
// refuses its edit, such as the removal of a note that makes its track wait until the track's notes have ended. The
// tool lists and counts each refusal. Edits that a sequence takes alone can clash together: on Ubisoft Milan's driver,
// two moves can put commands one tick apart. Such a sequence takes its edits one at a time, in the order of their
// commands, and an edit that the ones before it refuse changes the note's velocity instead. The program works out a
// note's limits as the Key Editor does. A note whose limits the program can't work out gets no edit and fails. The tool
// then exports the edited file and checks two things:
//
// - The archive, imported again, gives back the edited sequences. A sequence compares with the import's sequence that
//   plays the same sounds. Each edited command holds its edit.
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
// earlier end of a first pass, or of a read without a loop. A moved note at or past the end of the loop that the read
// after the edits finds counts apart. On a track that ties its notes, or plays one note at a time, a note that starts
// while the one before it still sounds joins that note. After a move or a longer length, two notes can join that didn't
// before. Only the notes of the tracks that ran a command that turns on ties or monophonic play count apart that way.
// Where a tick is shorter than the console's frame, as at times on the 3DS, a note of a tick or two can end in the
// frame where it starts, and the arrangement leaves it out. A move can then show or hide such a note, and a note of
// length 0 that becomes a tick long can vanish. The tool counts apart every note of 2 ticks or fewer that differs, on
// every console and at every tempo. A track can wait for its notes' ends: with the note wait on, a note of length 0
// waits until the track's notes have ended. A note of a length that the sequence decides as it plays waits for an
// unknown time. An edit of an earlier note's tick, length or velocity can change when the notes end, and with it the
// ticks of the later notes. Only the notes of such tracks count apart that way. A sound that draws random numbers draws
// others after an edit that removes or moves a draw. A sound whose tempo comes from a variable can stand still at its
// variables' starting values: a note that a move puts after a wait never plays. The tool lists such sounds, but doesn't
// count them as differing. On the DS, a note that finds every channel that its track may take holding a note of a
// higher priority gets none. The performer lists it among the notes that the driver never started
// (song::Track::unstarted). An edit can free a channel or take one, and such a note can then come or go. The tool
// counts apart a note that comes where the read before the edits lists it among those notes, at its tick or a tick
// away, and a note that goes where the read after the edits lists it so. On DSE, a note that finds no voice is such a
// note too. A track of DSE can also run past the end of its chunk, into the bytes that follow it. The driver reads
// those bytes as events, and the notes that it finds there take the octave that the track's last notes leave. An edit
// can then change them. The tool counts apart a note whose command falls past the sequence's commands. On Konami's
// driver, a note sounds until the track's next note or rest, and the tool counts each length that an edit of another
// note changes apart. A note that moves onto another note's tick cuts that note or falls to it, and counts apart among
// the notes that the arrangement hides. On the revisions of that driver with a pan, a PSG track off centre plays its
// right side on its channel and its left on the next, in place of the next track. An edit can move such a note from one
// track of the arrangement to the other, and the tool counts it apart. A command of that driver, such as a volume, can
// start a note again. The note then names a note command that ran before it, on its track or on the track before. A
// move of that command moves only the note that the command starts. The others stay at the ticks where their commands
// start them, and the tool counts them apart. On Nintendo R&D2's driver, a note gets no voice while each voice plays a
// note of higher priority, and the arrangement doesn't show it. An edit can free a voice or take one. The tool counts
// apart a note that comes where its track ran a note command at its tick before the edits, and a note that goes where
// its track runs one there after them (tools::NoteCommandTicks). On that driver, a note of one byte gives its values in
// its bytes after an edit of the track's last note (edit/rd2.h), and still counts as the same command. On Brownie
// Brown's driver, a note holds the frames up to its channel's next command. A move or a longer length of a note
// therefore changes the frames of the command, not a length in it. The channel's envelope can turn a note off for a
// frame and on again. The arrangement then shows one run of the command as more than one note, the next starting
// within a tick of the first one's end. A command whose notes follow each other that closely changes its velocity
// instead of moving or growing (open work, item 361), and the tool counts it. An edit can also change the frames where
// the envelope turns a note off. A note that comes or goes within a tick of the end of a note of the same key on its
// track counts apart too. On Rare's driver, the performer gives each key of a MIDI channel one note at a time. A note
// starts a tick late where another note of its key on its channel still sounds, or ends at its tick on a later track.
// An edit of the other note can then move the note's start by a tick, and its end stays. The tool counts such a note
// apart. A key that two voices of a channel start in one frame is one note there as loud as both. The note is on the
// track whose voice started first. An edit can put a note in the frame of another track's note of its key. The note
// then joins that note, and the tool counts it apart. A new velocity of the first voice then leaves the note louder
// than the plan gives it. The driver also plays its ticks in frames. A note moved by a tick can start on the tick that
// its frame gives, and keep its new end. A note that starts while another note of its key sounds starts a tick late,
// and the other note's release can end it there. The tool counts those notes apart too (RareFrameNote).
// On Quintet's driver, a removed note leaves a rest, and a note has no velocity: its velocity's edit leaves the command
// as it was. The conversion numbers the noise channel's drums in the order that they first play. A removal can then
// give the later drums other keys. The tool counts apart a note of the noise channel that comes back at another key,
// with its start, length and velocity.
// On Krawall, a note sits on its row: it can't move or change its length, and it changes its velocity instead. A
// note has no velocity, and its velocity's edit leaves the command as it was. A removed note becomes a note off of
// its channel, in the note's place. An effect of a row without a note can start a note too: a retrigger, or a note
// delay after a note off. Such a note has no note command, and the Key Editor can't edit it. The tool plans no edit
// for it, and counts it apart. The effect plays the channel's last note again, and it plays nothing once that note is
// removed.
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
// With --filler, the songs of a ROM that fills the GBA's 32 MiB go into its filler runs where they outgrow their
// places.
//
// usage: hokuto_check_edits [--model] [--filler] [--timelines] <archive> [sound]

#include "check_edits.h"

namespace
{

using hokuto::format::File;
using hokuto::tools::CheckedArchive;
using hokuto::tools::edits::Brownie;
using hokuto::tools::edits::Change;
using hokuto::tools::edits::CheckSoundAgain;
using hokuto::tools::edits::CountPlans;
using hokuto::tools::edits::EditAsProgram;
using hokuto::tools::edits::EditsOf;
using hokuto::tools::edits::EditsRun;
using hokuto::tools::edits::FindSplitRuns;
using hokuto::tools::edits::HoldsEdit;
using hokuto::tools::edits::ItemsByOffset;
using hokuto::tools::edits::PlanNotes;
using hokuto::tools::edits::TakeEditsOneAtATime;
using hokuto::tools::edits::Totals;
using hokuto::tools::edits::VelocitiesForRefused;
using hokuto::tools::edits::VelocitiesForSplitRuns;
using hokuto::tools::edits::WithComparableKeys;
using ntdlib::commands::Command;
using ntdlib::commands::Kind;

// Opens the archive at `path` into `run`: the archive, the Hokuto file that it imports into, and the archive that the
// program plays. Throws when one of them fails.
void OpenEditsFile(const std::filesystem::path& path, EditsRun& run)
{
    std::vector<uint8_t> bytes;
    xsflib::ReadFile(path, bytes);
    run.archive = hokuto::tools::OpenChecked(path, bytes);
    run.file = hokuto::tools::CheckedForm(hokuto::tools::ImportChecked(bytes, run.archive), bytes);
    run.sequences = hokuto::tools::SequencesOfChecked(run.file, bytes, run.archive);
    run.playing = run.archive;
    if (run.timelines)
    {
        run.file = hokuto::edit::OpenTimelines(std::move(run.file));
        run.playing = hokuto::tools::BuildChecked(run.file, run.name);
    }
}

// Prints the first sequence of `run`'s file that contains commands without a way out, and returns whether one does.
bool HoldsCommandsWithoutWayOut(const EditsRun& run)
{
    for (std::size_t sequence = 0; sequence < run.file.sequences.size(); ++sequence)
    {
        const std::size_t count =
            hokuto::edit::CommandsWithoutWayOut(*run.file.sequences[sequence], hokuto::edit::ConsoleOf(run.file));
        if (count != 0)
        {
            std::printf("%s: sequence %zu holds %zu commands without a way out\n", run.name.c_str(), sequence, count);
            return true;
        }
    }

    return false;
}

// The sequence of `run`'s file that `sound` plays, if any.
std::optional<uint32_t> SequenceOf(const EditsRun& run, uint32_t sound)
{
    const uint32_t file_id = run.archive.FileOf(sound);
    return file_id < run.sequences.size() ? run.sequences[file_id] : std::nullopt;
}

// Reads each sequence sound of `run` before the edits, or only the sound named `only` when it isn't empty, and plans
// an edit of each note command from the first sound that plays it. `brownie` says whether the file plays on Brownie
// Brown's driver.
void PlanEdits(EditsRun& run, const std::string& only, bool brownie, Totals& totals)
{
    run.before.resize(run.archive.SoundCount());
    std::map<uint32_t, std::set<uint32_t>> split;
    std::size_t turn = 0;
    for (uint32_t sound = 0; sound < run.archive.SoundCount(); ++sound)
    {
        const std::string& sound_name = run.archive.NameOf(sound);
        if (!run.archive.IsSequence(sound) || (!only.empty() && sound_name != only))
        {
            continue;
        }

        ++totals.sounds;
        try
        {
            run.before[sound] = run.playing.Read(sound);
            if (const std::optional<uint32_t> sequence = SequenceOf(run, sound))
            {
                WithComparableKeys(*run.before[sound], *run.file.sequences[*sequence],
                                   hokuto::edit::ConsoleOf(run.file));
            }
        }
        catch (const std::exception& error)
        {
            std::printf("%s: %s can't be read before the edits: %s\n", run.name.c_str(), sound_name.c_str(),
                        error.what());
            ++totals.unread;
            continue;
        }

        const std::optional<uint32_t> sequence = SequenceOf(run, sound);
        if (!sequence)
        {
            continue;
        }

        std::map<uint32_t, uint32_t>& by_offset = run.items[*sequence];
        if (by_offset.empty())
        {
            by_offset = ItemsByOffset(*run.file.sequences[*sequence], hokuto::edit::ConsoleOf(run.file));
        }
        // On Brownie Brown's driver, the arrangement can show one run of a note's command as more than one note: the
        // channel's envelope can turn it off for a frame and on again (open work, item 361). The next note of the same
        // command and stretch on the track then starts within a tick of the first one's end. Such a command takes no
        // move and no longer length.
        if (brownie)
        {
            FindSplitRuns(*run.before[sound], *sequence, split, totals);
        }

        // As in the Key Editor, a longer note can't pass its track's end.
        PlanNotes(run.name, run.file, *sequence, *run.before[sound], sound_name, by_offset, run.plans, run.replays,
                  turn, totals);
    }

    VelocitiesForSplitRuns(run.file, split, run.items, run.plans);
}

// Makes the edits of `run`'s plans in `run.edited`, and keeps where each sequence's items went. Returns whether a
// sequence can't take its edits.
bool EditSequences(EditsRun& run, Totals& totals)
{
    run.edited = run.file;
    bool failed = false;
    for (auto& [sequence, by_command] : run.plans)
    {
        std::optional<hokuto::edit::Edited> result;
        try
        {
            result = EditAsProgram(run.edited, sequence, EditsOf(by_command));
        }
        catch (const std::invalid_argument&)
        {
            VelocitiesForRefused(run.name, run.file, run.edited, sequence, run.items, by_command, totals);
        }
        catch (const std::exception& error)
        {
            std::printf("%s: sequence %u can't take its edits: %s\n", run.name.c_str(), sequence, error.what());
            failed = true;
            continue;
        }

        // Edits that a sequence takes alone can clash together. On Ubisoft Milan's driver, two moves can put commands
        // one tick apart. The edits then go in one at a time, in the order of their commands. Each edit that the edits
        // before it refuse changes the note's velocity instead.
        if (!result)
        {
            try
            {
                result = EditAsProgram(run.edited, sequence, EditsOf(by_command));
            }
            catch (const std::invalid_argument& error)
            {
                std::printf("%s: sequence %u refuses its edits together, and takes them one at a time: %s\n",
                            run.name.c_str(), sequence, error.what());
                ++totals.together;
                TakeEditsOneAtATime(run.file, run.edited, sequence, run.items, by_command, totals);
            }
        }

        try
        {
            if (!result)
            {
                result = EditAsProgram(run.edited, sequence, EditsOf(by_command));
            }
            run.edited = std::move(result->file);
            run.moved[sequence] = std::move(result->items);
        }
        catch (const std::exception& error)
        {
            std::printf("%s: sequence %u can't take its edits: %s\n", run.name.c_str(), sequence, error.what());
            failed = true;
        }
    }

    return failed;
}

// Exports `run.edited`, and builds from it the archive that a saved file contains, the archive that the program plays,
// and the file that the first imports into again. That file takes the timelines' hints that a saved file keeps. Throws
// when the export or an import fails.
void BuildAgain(EditsRun& run)
{
    const File folded = hokuto::edit::FoldTimelines(run.edited);
    hokuto::tools::CheckSaved(folded);
    const std::vector<uint8_t> exported = hokuto::format::ExportArchive(folded);
    run.saved = hokuto::tools::BuildChecked(folded, run.name);
    run.after = run.timelines ? hokuto::tools::BuildChecked(run.edited, run.name) : run.saved;
    run.again = hokuto::tools::ImportChecked(exported, run.archive, hokuto::format::ExportGroupFiles(folded));
    if (run.timelines)
    {
        run.again.timelines = run.edited.timelines;
        run.again = hokuto::edit::OpenTimelines(std::move(run.again));
    }

    // The sequence of `again` that plays each edited sequence's sounds. An import numbers a ROM's sequences in the
    // order of their places on Nintendo R&D2's driver, and an export moves a song that grows to another place. The
    // import of any other archive keeps the numbers. Its sequences compare by number.
    const std::vector<std::optional<uint32_t>> again_sequences =
        run.archive.gba || run.archive.driver ? hokuto::tools::SequencesOfChecked(run.again, exported, run.archive)
                                              : std::vector<std::optional<uint32_t>>();
    for (std::size_t sound = 0; sound < std::min(run.sequences.size(), again_sequences.size()); ++sound)
    {
        if (run.sequences[sound] && again_sequences[sound])
        {
            run.again_of.emplace(*run.sequences[sound], *again_sequences[sound]);
        }
    }
}

// Whether `now` is the command `then` after the edits of `archive`. On Nintendo R&D2's driver, a note of one byte gives
// its values in its bytes after an edit of the track's last note (edit/rd2.h). Its other fields stay.
bool Unchanged(const CheckedArchive& archive, const Command& now, const Command& then)
{
    Command respelled = then;
    if (archive.Rd2())
    {
        respelled.encoding = now.encoding;
        respelled.bytes = now.bytes;
    }
    return now == respelled;
}

// Checks that `run.again`, the import of the edited archive, gives back each sequence of `run.edited` that `run.moved`
// lists, and that each edited command contains its edit. `run.hidden` takes the offsets after the edits of the note
// commands that no plan names, and `run.moved_notes` the offsets of the moved notes. Returns whether a sequence doesn't
// come back.
bool CheckImportedSequences(EditsRun& run, Totals& totals)
{
    bool failed = false;
    for (const auto& [sequence, by_command] : run.plans)
    {
        if (!run.moved.contains(sequence))
        {
            continue;
        }

        // On DSE, the bytes come back: a note that sets its octave comes back as two commands. On Konami's driver,
        // zeros can follow a song that shrank (tools::HoldsSequence).
        const auto mapped = run.again_of.find(sequence);
        const uint32_t again_sequence = mapped != run.again_of.end() ? mapped->second : sequence;
        if (run.again.sequences.size() != run.edited.sequences.size() ||
            (run.archive.dse || run.archive.driver
                 ? !hokuto::tools::HoldsSequence(run.archive, *run.edited.sequences[sequence],
                                                 *run.again.sequences[again_sequence])
                 : run.again.sequences[again_sequence] != run.edited.sequences[sequence]))
        {
            std::printf("%s: sequence %u doesn't come back from the edited archive\n", run.name.c_str(), sequence);
            failed = true;
        }
        const std::vector<Command>& commands = run.edited.sequences[sequence]->items;
        const std::vector<uint32_t> offsets =
            hokuto::edit::Offsets(*run.edited.sequences[sequence], hokuto::edit::ConsoleOf(run.edited));
        for (const auto& [offset, plan] : by_command)
        {
            const uint32_t old = run.items[sequence].at(offset);
            const std::optional<uint32_t> item = run.moved[sequence].at(old);
            if (!HoldsEdit(run.file.sequences[sequence]->items[old], plan, item ? &commands.at(*item) : nullptr,
                           hokuto::edit::ConsoleOf(run.file)))
            {
                std::printf("%s: sequence %u: the command at %u lacks its edit\n", run.name.c_str(), sequence, offset);
                ++totals.wrong_commands;
            }
            if (item && plan.change == Change::kMove)
            {
                run.moved_notes[sequence].insert(offsets[*item]);
            }
        }

        for (const auto& [offset, old] : run.items[sequence])
        {
            const std::optional<uint32_t> item = run.moved[sequence].at(old);
            const Command& command = run.file.sequences[sequence]->items[old];
            if (item && command.kind == Kind::kNote && !by_command.contains(offset) &&
                Unchanged(run.archive, commands.at(*item), command))
            {
                run.hidden[sequence].insert(offsets[*item]);
            }
        }
    }

    return failed;
}

// Prints the summary of `run`. `brownie` says whether the file plays on Brownie Brown's driver.
void PrintSummary(const EditsRun& run, bool brownie, const Totals& totals)
{
    std::size_t timeline_count = 0;
    for (uint32_t sequence = 0; sequence < run.file.sequences.size(); ++sequence)
    {
        timeline_count += hokuto::edit::IsTimeline(run.file, sequence) ? 1 : 0;
    }
    const std::string saved_line = run.timelines ? " " + std::to_string(timeline_count) +
                                                       " sequences are timelines, and " + std::to_string(totals.saved) +
                                                       " sounds play otherwise from the saved archive."
                                                 : "";
    std::printf(
        "%s: %zu sequence sounds, %zu notes from %zu commands: %zu moved, %zu with a new velocity, %zu removed and %zu "
        "longer. %zu edits were refused, in %zu sequences that took them one at a time too. %zu notes can't be edited. "
        "%zu commands are missing their edits. %zu sounds differ by %zu notes, and %zu can't be read. Apart: %zu "
        "sounds with legatos that differ, %zu with a track that waits for its notes' ends and differ, %zu that draw "
        "random numbers and differ, %zu whose tempo a variable decides and differ, %zu with the loop on another pass, "
        "%zu notes that the arrangement hid before the edits, %zu moved notes that other commands change, %zu moved "
        "notes at the loop's end, %zu notes of a tick or two that the frames show or hide, "
        "%zu notes that the voices or channels of the DS, DSE or Nintendo R&D2's driver let through or keep out, %zu "
        "notes from the bytes after a track's end, %zu notes of Krawall that an effect of a row starts without a note "
        "command, %zu notes that a command of Konami's driver starts again, %zu notes "
        "that join a PSG note of that driver "
        "or leave one, %zu PSG notes of that driver at volume 1, %zu PSG notes of that driver from a track off centre, "
        "%zu notes of Rare's driver that start a tick away for another note of their key, %zu that join a note of "
        "their key on another track, %zu that their frames make louder or start a tick away, %zu lengths and %zu "
        "velocities that don't come from the command, and %zu notes of Quintet's noise channel whose drum takes "
        "another key.%s\n",
        run.name.c_str(), totals.sounds, totals.notes, totals.commands, totals.moved, totals.velocities, totals.removed,
        totals.longer, totals.refused, totals.together, totals.unplanned, totals.wrong_commands, totals.differ,
        totals.wrong_notes, totals.unread, totals.legato, totals.waits, totals.random, totals.tempo,
        totals.other_passes, totals.hidden, totals.changed_moves, totals.past_loop, totals.ticks, totals.channels,
        totals.past_end, totals.row_effects, totals.started_again, totals.joined, totals.silenced, totals.off_centre,
        totals.shifted, totals.rare_joined, totals.rare_framed, totals.other_lengths, totals.other_velocities,
        totals.drums, saved_line.c_str());
    if (brownie)
    {
        std::printf(
            "%s: on Brownie Brown's driver, %zu commands show as more than one note in a run, and took no move "
            "and no longer length. %zu notes came or went where they continue a note of the same key within a "
            "tick.\n",
            run.name.c_str(), totals.split, totals.runs);
    }
    hokuto::tools::PrintSaves(run.name);
}

} // namespace

int main(int argc, char** argv)
{
    const std::optional<hokuto::tools::EditOptions> options = hokuto::tools::ParseEditOptions(
        xsflib::console::Arguments(argc, argv),
        "usage: hokuto_check_edits [--model] [--filler] [--timelines] <archive> [sound]\n");
    if (!options)
    {
        return 2;
    }

    // The archive and the group files beside it make a Hokuto file. `playing` is the archive that the program plays.
    EditsRun run;
    run.name = xsflib::Utf8(options->path.filename());
    run.timelines = options->timelines;
    try
    {
        OpenEditsFile(options->path, run);
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "%s: %s\n", run.name.c_str(), error.what());
        return 2;
    }

    // The driver hangs on a loop without a wait. A game's DSE sequence therefore contains none. A count above 0 here is
    // a fault in the count, and would refuse edits of that sequence wrongly.
    if (run.archive.dse && HoldsCommandsWithoutWayOut(run))
    {
        return 1;
    }

    // Each sequence sound before the edits, and the plan of each note command, from the first sound that plays it.
    Totals totals;
    const bool brownie = Brownie(hokuto::edit::ConsoleOf(run.file));
    PlanEdits(run, options->only, brownie, totals);

    // The edits, sequence by sequence, in one version of the file, and where each sequence's items went. When a
    // sequence refuses its edits, each edit alone shows which ones it refuses. Those change the note's velocity
    // instead.
    bool failed = EditSequences(run, totals);

    // The plans as they stand after the refusals.
    CountPlans(run.plans, totals);

    // The edited archive that a saved file holds, the archive that the program plays, and the file that the first
    // imports into again. That file takes the timelines' hints that a saved file keeps.
    try
    {
        BuildAgain(run);
    }
    catch (const std::exception& error)
    {
        std::printf("%s: the edited file can't be exported: %s\n", run.name.c_str(), error.what());
        return 1;
    }

    // The import gives back the edited sequences, and each edited command holds its edit. The note commands that no
    // plan names are the ones that the arrangement hid. `hidden` takes their offsets after the edits, and `moved_notes`
    // the offsets of the moved notes.
    if (CheckImportedSequences(run, totals))
    {
        failed = true;
    }

    // Each sound again, from the edited archive.
    for (uint32_t sound = 0; sound < run.archive.SoundCount(); ++sound)
    {
        const std::optional<uint32_t> sequence = SequenceOf(run, sound);
        if (run.before[sound] && sequence)
        {
            CheckSoundAgain(run, sound, *sequence, totals);
        }
    }

    PrintSummary(run, brownie, totals);

    const bool passed = totals.differ == 0 && totals.wrong_commands == 0 && totals.unread == 0 &&
                        totals.unplanned == 0 && totals.saved == 0 && !failed;
    return passed ? 0 : 1;
}
