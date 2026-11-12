// SPDX-License-Identifier: MIT

// The arrangement's edits of hokuto_check_converter (--edits). check_converter.cpp describes the checks.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "check_converter.h"
#include "checked_archive.h"
#include "checked_saves.h"
#include "edit/converter.h"
#include "edit/notes.h"
#include "edit/timelines.h"
#include "format/archive.h"
#include "format/file.h"
#include "song/sequence.h"

namespace hokuto::tools::converter
{

namespace
{

// A note as an edit leaves it: its track, start, key and velocity. The engine sets the length of many notes, such as a
// note of a one-shot wave, and an edit can change it.
struct HeardNote
{
    friend auto operator<=>(const HeardNote&, const HeardNote&) = default;

    int track = 0;
    uint32_t start = 0;
    int key = 0;
    int velocity = 0;
    uint32_t command = 0; // the note's command, in the archive that the program plays
};

// The notes of `song` that start before `cut`, in order, without their commands.
std::vector<HeardNote> HeardNotes(const hokuto::song::Sequence& song, uint32_t cut, bool commands = false)
{
    std::vector<HeardNote> notes;
    for (const hokuto::song::Track& track : song.tracks)
    {
        for (const hokuto::song::Note& note : track.notes)
        {
            if (note.start < cut)
            {
                notes.push_back({track.number, note.start, note.key, note.velocity, commands ? note.command : 0});
            }
        }
    }
    std::sort(notes.begin(), notes.end());

    return notes;
}

// An edit of the arrangement: its name, the version that it makes, and the notes that the saved archive has to play
// before `cut`. A moved loop has to play from its new start to its new end. An edit of a part changes the notes of
// `changed`, in order, on track `track`. A moved loop changes no note, and its track is -1.
struct ArrangementEdit
{
    std::string name;
    hokuto::format::File file;
    std::vector<HeardNote> expected;
    uint32_t cut = 0;
    std::optional<hokuto::song::Loop> loop;
    std::vector<HeardNote> changed;
    int track = -1;
};

// Counts an edit `name` that the sequence refuses with `error`, by its reason.
void Refuse(const std::string& name, const std::exception& error, Totals& totals)
{
    ++totals.refused[name + ": " + WithoutNumbers(error.what())];
}

// The edits that the arrangement's info line makes of the busiest part of the busiest track of `shown`, the first sound
// of timeline `sequence` of `opened`. `before` is the sound as the game's archive plays it, and the sound ends at tick
// `cut`. An edit that the sequence refuses counts apart, by its reason.
std::vector<ArrangementEdit> BusiestPartEdits(const hokuto::format::File& opened, uint32_t sequence,
                                              const hokuto::song::Sequence& shown, const hokuto::song::Sequence& before,
                                              uint32_t cut, Totals& totals)
{
    // The busiest part. The arrangement splits parts on beats, at gaps of two bars of 4/4.
    const auto busiest = std::max_element(shown.tracks.begin(), shown.tracks.end(),
                                          [](const auto& a, const auto& b) { return a.notes.size() < b.notes.size(); });
    std::vector<ArrangementEdit> edits;
    if (busiest == shown.tracks.end() || busiest->notes.empty())
    {
        return edits;
    }

    const std::vector<hokuto::song::Part> parts = hokuto::song::SplitParts(shown, *busiest, 1, 8);
    const auto part =
        std::max_element(parts.begin(), parts.end(), [](const auto& a, const auto& b) { return a.count < b.count; });
    std::set<uint32_t> commands;
    for (std::size_t n = part->first; n < part->first + part->count; ++n)
    {
        commands.insert(busiest->notes[n].command);
    }

    // Each command takes the edit once. Every note that the command plays changes the same way, in the part and
    // elsewhere, such as in a pass of a loop inside the part.
    const auto edit_part = [&](const std::string& name, auto change, auto expect)
    {
        std::vector<hokuto::edit::NoteEdit> note_edits;
        std::set<uint32_t> done;
        for (std::size_t n = part->first; n < part->first + part->count; ++n)
        {
            const hokuto::song::Note& note = busiest->notes[n];
            if (done.insert(note.command).second)
            {
                hokuto::edit::NoteEdit& note_edit = note_edits.emplace_back(
                    hokuto::edit::NoteEdit{.command = note.command, .stretch = busiest->stretches[note.stretch]});
                change(note_edit, note);
            }
        }
        try
        {
            ArrangementEdit made{.name = name,
                                 .file = hokuto::edit::EditNotes(opened, sequence, note_edits, {}).file,
                                 .expected = HeardNotes(shown, cut, true),
                                 .cut = cut,
                                 .loop = before.loop,
                                 .track = busiest->number};
            for (HeardNote& note : made.expected)
            {
                const bool changed = note.track == busiest->number && commands.contains(note.command);
                note.command = 0;
                if (changed)
                {
                    expect(note);
                    made.changed.push_back(note);
                }
            }
            for (std::vector<HeardNote>* notes : {&made.expected, &made.changed})
            {
                std::erase_if(*notes, [cut](const HeardNote& note) { return note.start >= cut; });
                std::sort(notes->begin(), notes->end());
            }
            edits.push_back(std::move(made));
        }
        catch (const std::invalid_argument& error)
        {
            Refuse(name, error, totals);
        }
    };

    const uint32_t beat = static_cast<uint32_t>(std::max(shown.timebase, 1));
    const auto shift = [beat](hokuto::edit::NoteEdit& note_edit, const auto&)
    {
        note_edit.shift = static_cast<int32_t>(beat);
    };
    edit_part("Move", shift, [beat](HeardNote& note) { note.start += beat; });
    const auto transpose = [](hokuto::edit::NoteEdit& note_edit, const auto&)
    {
        note_edit.transpose = 1;
    };
    edit_part("Transpose", transpose, [](HeardNote& note) { ++note.key; });
    const auto soften = [](hokuto::edit::NoteEdit& note_edit, const hokuto::song::Note& note)
    {
        note_edit.velocity = std::max(note.velocity - 10, 1);
    };
    const auto softer = [](HeardNote& note)
    {
        note.velocity = std::max(note.velocity - 10, 1);
    };
    edit_part("Change Part Velocity", soften, softer);

    return edits;
}

// The moves of the loop's edges of timeline `sequence` of `opened`, a beat of `shown` in and a bar out. `before` is the
// sound as the game's archive plays it. A move that the sequence refuses counts apart, by its reason.
std::vector<ArrangementEdit> LoopEdgeEdits(const hokuto::format::File& opened, uint32_t sequence,
                                           const hokuto::song::Sequence& shown, const hokuto::song::Sequence& before,
                                           Totals& totals)
{
    // The loop's edges, a beat in and a bar out.
    std::vector<ArrangementEdit> edits;
    const std::optional<hokuto::edit::ConvertedLoop> loop = hokuto::edit::TimelineLoop(opened, sequence);
    if (loop && before.loop && before.loop->start == loop->start && before.loop->end == loop->end)
    {
        const uint32_t beat = static_cast<uint32_t>(std::max(shown.timebase, 1));
        std::vector<std::pair<std::string, hokuto::edit::ConvertedLoop>> moves;
        if (loop->start + beat < loop->end)
        {
            moves.push_back({"Size: start later", {loop->start + beat, loop->end}});
            moves.push_back({"Size: end earlier", {loop->start, loop->end - beat}});
        }
        if (loop->start >= beat)
        {
            moves.push_back({"Size: start earlier", {loop->start - beat, loop->end}});
        }
        moves.push_back({"Size: end later", {loop->start, loop->end + 4 * beat}});
        for (const auto& [name, moved] : moves)
        {
            try
            {
                const uint32_t moved_cut = std::min(loop->end, moved.end);
                edits.push_back({.name = name,
                                 .file = hokuto::edit::MoveLoop(opened, sequence, moved).file,
                                 .expected = HeardNotes(before, moved_cut),
                                 .cut = moved_cut,
                                 .loop = hokuto::song::Loop{moved.start, moved.end}});
            }
            catch (const std::invalid_argument& error)
            {
                Refuse(name, error, totals);
            }
        }
    }

    return edits;
}

// The edits that the arrangement's info line makes of the busiest part of the busiest track of `shown`, the first sound
// of timeline `sequence` of `opened`, and the moves of the loop's edges. `before` is the sound as the game's archive
// plays it. An edit that the sequence refuses counts apart, by its reason.
std::vector<ArrangementEdit> ArrangementEdits(const hokuto::format::File& opened, uint32_t sequence,
                                              const hokuto::song::Sequence& shown, const hokuto::song::Sequence& before,
                                              Totals& totals)
{
    // A note at the sequence's end starts in the tick where the sound ends, and doesn't sound.
    const uint32_t cut = before.loop ? before.loop->end : before.length;

    std::vector<ArrangementEdit> edits = BusiestPartEdits(opened, sequence, shown, before, cut, totals);
    std::vector<ArrangementEdit> loop_edits = LoopEdgeEdits(opened, sequence, shown, before, totals);
    edits.insert(edits.end(), std::make_move_iterator(loop_edits.begin()), std::make_move_iterator(loop_edits.end()));

    return edits;
}

// Checks that `after`, the sound after `edit` as the saved archive plays it, plays the notes that `edit` expects, and
// returns whether the performer found the loop a whole number of passes away. `where` names the sound.
bool CheckHeardNotes(const std::string& where, const ArrangementEdit& edit, const hokuto::song::Sequence& after,
                     Totals& totals)
{
    // An instrument can lack a region for a changed note's new key or velocity. A changed note can cut
    // other notes of its track in its tick through a key group, or stop cutting them. The performer leaves
    // out a note that doesn't sound. ntdlib's performer can find a loop a whole number of passes earlier or
    // later. Every track's state repeats there. The performer stops at the end of the loop that it finds.
    const uint32_t length = edit.loop ? edit.loop->end - edit.loop->start : 0;
    const bool apart =
        edit.loop && after.loop && after.loop != edit.loop && length > 0 &&
        after.loop->end - after.loop->start == length &&
        (std::max(after.loop->start, edit.loop->start) - std::min(after.loop->start, edit.loop->start)) % length == 0;
    const uint32_t cut = apart ? std::min(edit.cut, after.loop->end) : edit.cut;
    std::vector<HeardNote> expected = edit.expected;
    std::vector<HeardNote> changed = edit.changed;
    for (std::vector<HeardNote>* notes : {&expected, &changed})
    {
        std::erase_if(*notes, [cut](const HeardNote& note) { return note.start >= cut; });
    }
    const std::vector<HeardNote> heard = HeardNotes(after, cut);
    std::set<uint32_t> ticks;
    for (const HeardNote& note : changed)
    {
        ticks.insert(note.start);
    }
    std::vector<HeardNote> fewer;
    std::vector<HeardNote> more;
    std::ranges::set_difference(expected, heard, std::back_inserter(fewer));
    std::ranges::set_difference(heard, expected, std::back_inserter(more));
    const auto left_out = [&edit, &changed, &ticks](const HeardNote& note)
    {
        return std::ranges::binary_search(changed, note) || (note.track == edit.track && ticks.contains(note.start));
    };
    const auto added = [&edit](const HeardNote& note)
    {
        return note.track == edit.track;
    };
    const bool allowed = std::ranges::all_of(fewer, left_out) && std::ranges::all_of(more, added);
    totals.fewer += allowed && !fewer.empty() ? 1 : 0;
    totals.more += allowed && !more.empty() ? 1 : 0;
    if (!allowed)
    {
        // The first note that differs.
        const auto [played, wanted] = std::mismatch(heard.begin(), heard.end(), expected.begin(), expected.end());

        const auto text = [](const std::vector<HeardNote>& notes, auto note)
        {
            return note == notes.end()
                       ? std::string("none")
                       : "track " + std::to_string(note->track) + " at " + std::to_string(note->start) + ", key " +
                             std::to_string(note->key) + ", velocity " + std::to_string(note->velocity);
        };
        Problem(totals, where,
                edit.name + ": the saved archive plays other notes: " + text(heard, played) + " in the place of " +
                    text(expected, wanted) + ", of " + std::to_string(heard.size()) + " and " +
                    std::to_string(expected.size()));
    }
    totals.loops_apart += apart ? 1 : 0;

    return apart;
}

// Checks that `after`, the sound after `edit`, plays the loop of `edit`, unless the performer found it elsewhere
// (`apart`) or the changed notes left it silent. `settled` is the settled file, and `sequence` the timeline.
void CheckLoopKept(const std::string& where, const ArrangementEdit& edit, const hokuto::song::Sequence& after,
                   const hokuto::format::File& settled, uint32_t sequence, bool apart, Totals& totals)
{
    // The changed notes can leave the loop silent once their waves end. The sequence keeps its loop.
    const std::optional<hokuto::edit::ConvertedLoop> kept = hokuto::edit::TimelineLoop(settled, sequence);
    const bool silenced = edit.track >= 0 && edit.loop && !after.loop && kept && kept->start == edit.loop->start &&
                          kept->end == edit.loop->end;
    totals.silenced += silenced ? 1 : 0;
    if (edit.loop && after.loop != edit.loop && !apart && !silenced)
    {
        const auto text = [](const std::optional<hokuto::song::Loop>& loop)
        {
            return loop ? std::to_string(loop->start) + " to " + std::to_string(loop->end) : std::string("none");
        };
        Problem(totals, where,
                edit.name + ": the saved archive plays another loop: " + text(after.loop) + " in the place of " +
                    text(edit.loop));
    }
}

// Checks that `saved`, the archive that `settled` saves with its `group_files`, opens again as the same timeline
// `sequence`, with its hints and without them. `archive` is the saved archive as a check reads it.
void CheckReopens(const std::string& where, const ArrangementEdit& edit, const std::vector<uint8_t>& saved,
                  const std::vector<std::vector<uint8_t>>& group_files, const CheckedArchive& archive,
                  const hokuto::format::File& settled, uint32_t sequence, Totals& totals)
{
    // The saved file opens again as the same timeline, with its hints and without them. Without them, the
    // converter's rules find the hints again.
    hokuto::format::File imported = hokuto::tools::ImportChecked(saved, archive, group_files);
    const hokuto::edit::ConverterHints& hints = settled.timelines->at(sequence);
    const hokuto::edit::ConvertedSong found = hokuto::edit::ReadConverted(*imported.sequences[sequence]);
    imported.timelines = std::map<uint32_t, hokuto::edit::ConverterHints>{{sequence, hints}};
    const hokuto::format::File decided = hokuto::edit::OpenTimelines(imported);
    if (!hokuto::edit::IsTimeline(decided, sequence))
    {
        Problem(totals, where, edit.name + ": the saved file doesn't open as a timeline");
    }
    else if (*decided.sequences[sequence] != *settled.sequences[sequence])
    {
        Problem(totals, where, edit.name + ": the saved file opens as another timeline");
    }
    if (!found.hints || hokuto::edit::LayOutConverted(*imported.sequences[sequence], *found.hints, false) !=
                            *settled.sequences[sequence])
    {
        Problem(totals, where,
                edit.name + ": the saved file without its hints opens as another layout" +
                    (found.hints ? std::string() : ": " + found.problem));
    }
}

// Checks `edit`, an edit of timeline `sequence` of `opened` that sound `sound` plays: the program settles and saves it,
// the saved archive plays it as expected, and the saved file opens again. Returns whether the edit leaves a timeline.
// `name` names the archive, and `where` the sound.
bool CheckArrangementEdit(const std::string& name, const std::string& where, const hokuto::format::File& opened,
                          uint32_t sound, uint32_t sequence, const ArrangementEdit& edit, Totals& totals)
{
    // The program settles the edit, and saves it with the timeline's subroutines.
    const hokuto::edit::Settled settled = hokuto::edit::SettleTimelines(opened, edit.file);
    if (!hokuto::edit::IsTimeline(settled.file, sequence))
    {
        Problem(totals, where, edit.name + ": the edit leaves no timeline");
        return false;
    }
    const hokuto::format::File folded = hokuto::edit::FoldTimelines(settled.file);
    hokuto::tools::CheckSaved(folded, true);
    const std::vector<uint8_t> saved = hokuto::format::ExportArchive(folded);
    const std::vector<std::vector<uint8_t>> group_files = hokuto::format::ExportGroupFiles(folded);
    const CheckedArchive archive = hokuto::tools::BuildChecked(folded, name);
    const hokuto::song::Sequence after = archive.Read(sound);

    // The settled edit plays as the edit plays before the program settles it.
    const hokuto::song::Sequence unsettled = hokuto::tools::BuildChecked(edit.file, name).Read(sound);
    if (NotesOf(after) != NotesOf(unsettled) || after.length != unsettled.length || after.loop != unsettled.loop)
    {
        Problem(totals, where, edit.name + ": the settled edit plays otherwise");
    }

    const bool apart = CheckHeardNotes(where, edit, after, totals);

    // The changed notes can leave the loop silent once their waves end. The sequence keeps its loop.
    CheckLoopKept(where, edit, after, settled.file, sequence, apart, totals);

    // The saved file opens again as the same timeline, with its hints and without them. Without them, the
    // converter's rules find the hints again.
    CheckReopens(where, edit, saved, group_files, archive, settled.file, sequence, totals);

    return true;
}

} // namespace

void CheckEdits(const std::string& name, const hokuto::format::File& opened, const CheckedArchive& game,
                const CheckedArchive& unfolded, const std::vector<std::optional<uint32_t>>& sequences, Totals& totals)
{
    std::set<uint32_t> done;
    int edited = 0;
    int edits = 0;
    for (uint32_t sound = 0; sound < game.SoundCount(); ++sound)
    {
        const std::optional<uint32_t> sequence = SequenceOfSound(game, sound, sequences);
        if (!sequence || !hokuto::edit::IsTimeline(opened, *sequence) || !done.insert(*sequence).second)
        {
            continue;
        }

        const std::string where = name + " " + game.NameOf(sound);
        try
        {
            const hokuto::song::Sequence before = game.Read(sound);
            const hokuto::song::Sequence shown = unfolded.Read(sound);
            for (const ArrangementEdit& edit : ArrangementEdits(opened, *sequence, shown, before, totals))
            {
                if (CheckArrangementEdit(name, where, opened, sound, *sequence, edit, totals))
                {
                    ++edits;
                }
            }
            ++edited;
        }
        catch (const std::exception& error)
        {
            Problem(totals, where, error.what());
        }
    }
    totals.edited += edited;
    totals.edits += edits;
    std::printf("%s: %d timelines took %d edits\n", name.c_str(), edited, edits);
}

} // namespace hokuto::tools::converter
