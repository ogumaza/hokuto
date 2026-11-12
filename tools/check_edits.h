// SPDX-License-Identifier: MIT

// The parts of hokuto_check_edits that its files share: the plans of the edits, the totals, the notes as the
// comparisons take them, and the helpers. check_edits.cpp describes the checks.

#pragma once

#include <algorithm>
#include <cmath>
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
#include "ntdlib/quintet/song.h"
#include "xsflib/console.h"
#include "xsflib/files.h"

#include "check_support.h"
#include "checked_archive.h"
#include "checked_saves.h"
#include "edit/bytecode.h"
#include "edit/commands.h"
#include "edit/konami.h"
#include "edit/krawall.h"
#include "edit/notes.h"
#include "edit/quintet.h"
#include "edit/rare.h"
#include "edit/timelines.h"
#include "edit/ubimilan.h"
#include "format/archive.h"
#include "format/file.h"
#include "loop_passes.h"
#include "notes_apart.h"
#include "song/archive.h"
#include "song/sequence.h"
#include "song/ubimilan.h"

namespace hokuto::tools::edits
{

using hokuto::edit::NoteEdit;
using hokuto::format::File;
using hokuto::song::Note;
using hokuto::song::Sequence;
using hokuto::tools::CheckedArchive;
using hokuto::tools::EarlierEnd;
using hokuto::tools::RareFrameTicks;
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
    uint32_t longer = 0;           // the ticks that a longer note gains
    std::optional<uint32_t> heard; // the velocity that a read shows after a new velocity, where it isn't the edit's
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
    std::size_t together = 0; // sequences that refuse their edits together, and take them one at a time
    std::size_t wrong_commands = 0;
    std::size_t differ = 0;
    std::size_t started_again = 0; // notes of Konami's driver that follow a note of the same command
    std::size_t joined = 0;        // notes of Konami's driver that join a PSG note or leave one
    std::size_t silenced = 0;      // PSG notes of Konami's driver at volume 1
    std::size_t off_centre = 0;    // PSG notes of Konami's driver that a track off centre plays on the next channel
    std::size_t wrong_notes = 0;
    std::size_t unread = 0;
    std::size_t unplanned = 0; // notes whose limits the program can't work out
    std::size_t legato = 0;
    std::size_t waits = 0; // sounds with a track that waits for its notes' ends, and that play other notes
    std::size_t hidden = 0;
    std::size_t changed_moves = 0;
    std::size_t past_loop = 0;
    std::size_t ticks = 0;       // notes of a tick or two that the frames show or hide
    std::size_t shifted = 0;     // Rare's notes that start a tick away for another note of their key on their channel
    std::size_t rare_joined = 0; // Rare's notes that join a note of their key on another track in a frame
    std::size_t rare_framed = 0; // Rare's notes that their frame makes louder, or starts a tick away
    std::size_t channels =
        0; // notes that the voices or channels of the DS, DSE or R&D2's driver let through or keep out
    std::size_t past_end = 0; // DSE's notes from the bytes after a track's end
    std::size_t random = 0;   // sounds that draw random numbers and play other notes
    std::size_t tempo = 0;    // sounds whose tempo a variable decides, and that play other notes
    std::size_t other_passes = 0;
    std::size_t other_lengths = 0;
    std::size_t other_velocities = 0;
    std::size_t drums = 0;       // notes of Quintet's noise channel whose drum takes another key
    std::size_t row_effects = 0; // Krawall's notes that an effect of a row starts without a note command
    std::size_t saved = 0;
    std::size_t split = 0; // commands of Brownie Brown's driver whose sound shows as more than one note
    std::size_t runs = 0;  // notes of Brownie Brown's driver that continue a note of the same key within a tick
};

// A note as the check compares it: its track, start, key, length and velocity.
using Played = std::tuple<int, uint32_t, uint32_t, uint32_t, uint32_t>;

// The item at each offset of a sequence's bytecode on `console`.
std::map<uint32_t, uint32_t> ItemsByOffset(const ntdlib::commands::Sequence& sequence,
                                           const hokuto::song::Console& console);

// Whether `console` follows Brownie Brown's driver.
bool Brownie(const hokuto::song::Console& console);

// A plan that changes the velocity of `command` in place of `plan`: one down, or one up from 1. On the GBA's other
// drivers, the command contains the driver volume, and the velocity takes the volume's value on the Key Editor's scale.
Plan VelocityPlan(const File& file, const Command& command, const Plan& plan);

// The plan for `command`, the note command of `note`, in sequence `sequence` of `file`, run in `stretch`. The commands
// take turns by `turn`.
Plan PlanFor(const File& file, uint32_t sequence, const Note& note, const hokuto::song::Stretch& stretch,
             const Command& command, std::size_t turn);

// The edits of `plans`, in the order of their commands.
std::vector<NoteEdit> EditsOf(const std::map<uint32_t, Plan>& plans);

// The file after the program makes `edits` to sequence `sequence` of `file`: an edit of a timeline then takes the
// converter's layout. `Edited::items` gives the new index of each old item in that layout.
hokuto::edit::Edited EditAsProgram(const File& file, uint32_t sequence, std::span<const NoteEdit> edits);

// The message with which sequence `sequence` of `file` refuses `edit`. Nothing if it takes the edit.
std::optional<std::string> Refusal(const File& file, uint32_t sequence, const NoteEdit& edit);

// The name of `change`, for a line about it.
const char* NameOf(Change change);

// The length of note command `command`, for a line about it.
std::string LengthOf(const Command& command);

// The notes of `sequence` that start before `end`, in the form that the check compares. `commands` takes the offset of
// each note's command.
std::vector<Played> NotesOf(const Sequence& sequence, uint32_t end, std::map<Played, uint32_t>& commands);

// Whether `note`, a note that is missing from a read of Rare's driver after the edits, joins a note of another track in
// `after`: a note of its key that starts a frame of the driver before it or less, sounds at its start and isn't among
// `before`. A key that two voices of a MIDI channel start in one frame is one note as loud as both (ntdlib's conversion
// of the driver). The note is on the track whose voice started first.
bool JoinsRareNote(const std::vector<Played>& before, const std::vector<Played>& after, uint32_t frame,
                   const Played& note);

// Whether `note`, a note that is missing from a read of Rare's driver after the edits, plays otherwise for the driver's
// frames. `after` contains a note of its track and key that comes from one of three things. A key that two voices of a
// MIDI channel start in one frame is one note as loud as both (ntdlib's conversion of the driver). An edit of one
// voice's velocity leaves that note louder than the plan, at its tick and within a tick of its length. A move of a tick
// can stay in its frame. The note then starts a tick early, at its velocity, and ends where the plan ends it. A note
// that starts while another note of its key on its track sounds starts a tick late, at its velocity, and that note's
// release can end it. It then ends no later than a tick after the plan's end.
bool RareFrameNote(const std::vector<Played>& after, const Played& note);

// Whether the command at `offset` of `sequence` is past the sequence's commands. `by_offset` maps offsets to the items
// of the sequence. A track of DSE that has no end reads the bytes after its chunk as commands. Those bytes are data
// here.
bool PastCommands(const ntdlib::commands::Sequence& sequence, const std::map<uint32_t, uint32_t>& by_offset,
                  uint32_t offset);

// Gives the notes of `read`, a song that the commands of `commands` play, keys that the check can compare. On the
// revision of Konami's driver without a mixer, the song's samples take keys in their order. A sample that an edit takes
// out of the song therefore moves the others. The check compares the sample there. On any revision, key 255 of a PSG
// note points past the driver's table of pitches. The sound of such a note doesn't come from its command alone.
void WithComparableKeys(Sequence& read, const ntdlib::commands::Sequence& commands,
                        const hokuto::song::Console& console);

// Whether `note` of `read`, a song of Konami's driver, follows a note of the same command on its track. Other records
// of the driver change the registers of a PSG channel. The channel can then start a note again while one command plays.
// After a note's removal, the note before it sounds on, and starts again at those ticks. A PSG track off centre plays
// notes of the track before it on the channel. Such a note can stand between the two, and doesn't count: the note
// before is the nearest earlier note that the commands of the same track play.
// The two notes also have to be in one stretch. A loop or a call that plays a note's command again starts a new
// stretch, and its note is no restart.
bool StartedAgain(const Sequence& read, const Played& note);

// Whether another command starts `note` of `read`, a read of sequence `sequence` of `file` on Konami's driver: the
// note's command doesn't run on the note's track at the note's tick. A volume can start a note again, and a PSG track
// off centre plays notes of the track before. `rows` keeps each track's rows (edit::CommandRows) between calls.
bool StartedElsewhere(const File& file, uint32_t sequence, const Sequence& read, const Played& note,
                      std::map<int, std::set<std::pair<uint32_t, uint32_t>>>& rows);

// Whether `read`, a song of Konami's driver, plays `note` on a PSG track of the Eternal Duelist or Captain Tsubasa
// revision inside a longer note. Those revisions start a PSG note again only for a new volume. Otherwise the note that
// sounds takes the new pitch. An edit of a volume, or a note's removal, can therefore join two notes or part them.
bool JoinsPsgNote(const Sequence& read, const Played& note, const hokuto::song::Console& console);

// Whether `note` of `read`, a song of Konami's driver, comes from the track before it. In the revisions whose records
// hold a pan, a PSG track off centre plays its left side on the next channel, in place of the next track. The note then
// holds a command of that track, or plays that track's key at that track's tick. An edit of either track's notes can
// decide which of them sounds there.
bool OffCentrePsgNote(const Sequence& read, const Played& note, const hokuto::song::Console& console);

// Whether `read` lists `note` among the notes that the driver never started (song::Track::unstarted): a note of its
// track and key at its tick or a tick away. A move takes a note a tick away.
bool Unstarted(const Sequence& read, const Played& note);

// The longest note, in ticks, that the frames can show or hide where a tick is shorter than a frame.
constexpr uint32_t kShortNote = 2;

// Whether a command that `song` ran up to the end of the loop's first pass sets the tempo from a variable or a random
// number. Sequence `sequence` of `file` played `song`.
bool TempoUndecided(const File& file, uint32_t sequence, const Sequence& song);

// The notes that start before `end` when `before` plays after the plans of its sequence's commands. `edits` takes the
// edit of each note, for a line about it. `moves` takes the notes that move, and `origins` the note before the move of
// each. `ticks` takes the notes of a command of length 0 that becomes a tick long. On Krawall, a note of `replays`, the
// commands of the notes that an effect of a row starts, plays the track's last note again. It goes with that note when
// that note's plan removes it.
std::vector<Played> Expected(const Sequence& before, const std::map<uint32_t, Plan>& plans,
                             const std::set<uint32_t>& replays, uint32_t end, std::map<Played, std::string>& edits,
                             std::set<Played>& moves, std::map<Played, Played>& origins, std::set<Played>& ticks);

// `note` without its length, to compare.
Played WithoutLength(Played note);

// `note` without its velocity, to compare.
Played WithoutVelocity(Played note);

// The notes of `notes` that no note of `others` matches, but for the part that `strip` takes out.
std::vector<Played> Unmatched(const std::vector<Played>& notes, const std::vector<Played>& others,
                              Played (*strip)(Played));

// The notes of `a` that `b` lacks. Both are sorted.
std::vector<Played> Without(const std::vector<Played>& a, const std::vector<Played>& b);

// The loop and the length of `sequence`, for a line about it.
std::string Shape(const Sequence& sequence);

// The tick where `sequence` ends the first pass of its loop, or its end without a loop.
uint32_t FirstPassEnd(const Sequence& sequence);

// The tracks of `song`, a sound that plays `sequence` on `console`, that ran a command that turns on ties or monophonic
// play. A note of such a track that starts while the one before it still sounds joins that note.
std::set<int> LegatoTracks(const ntdlib::commands::Sequence& sequence, const hokuto::song::Console& console,
                           const Sequence& song);

// The tracks of `song`, a sound that plays `sequence` on `console`, that ran a note with the note wait on whose wait
// the bytecode doesn't give: a note of length 0 waits until the track's notes have ended, and a note of a length from a
// variable or a random number waits for a length that the sequence decides as it plays. A comparison can decide whether
// a note waits. An edit of an earlier note's tick, length or velocity can then move the track's later notes.
std::set<int> UnknownWaitTracks(const ntdlib::commands::Sequence& sequence, const hokuto::song::Console& console,
                                const Sequence& song);

// Whether `edited`, the command that `old` became, holds `plan`'s edit. A removed note leaves no command, or a wait. On
// Quintet's driver, it leaves a rest of the note's value, and a note has no velocity. On Krawall, it leaves a note off
// of the note's channel, and a note has no velocity. On the GBA's other drivers, the command contains the driver
// volume. On Konami's, Ubisoft Milan's and Rare's drivers, it has no length. On Nintendo R&D2's driver, a note of one
// byte that would read other values gives them in its bytes after the edit.
bool HoldsEdit(const Command& old, const Plan& plan, const Command* edited, const hokuto::song::Console& console);

// Gives each edit of `by_command` that sequence `sequence` of `edited` refuses alone a new velocity instead, and prints
// the refusal under `name`. `file` is the file before the edits, and `items` give each sequence's commands by offset.
void VelocitiesForRefused(const std::string& name, const File& file, const File& edited, uint32_t sequence,
                          std::map<uint32_t, std::map<uint32_t, uint32_t>>& items, std::map<uint32_t, Plan>& by_command,
                          Totals& totals);

// Puts the edits of `by_command` into sequence `sequence` of `edited` one at a time, in the order of their commands.
// Each edit that the edits before it refuse gives the note a new velocity instead. `file` is the file before the
// edits, and `items` give each sequence's commands by offset.
void TakeEditsOneAtATime(const File& file, const File& edited, uint32_t sequence,
                         std::map<uint32_t, std::map<uint32_t, uint32_t>>& items, std::map<uint32_t, Plan>& by_command,
                         Totals& totals);

// Adds to `split` each note command of `song`, a sound of `sequence`, whose run the arrangement shows as more than one
// note, and counts it in `totals`.
void FindSplitRuns(const Sequence& song, uint32_t sequence, std::map<uint32_t, std::set<uint32_t>>& split,
                   Totals& totals);

// Plans an edit of each note command of `song`, a sound of sequence `sequence` named `sound_name`, that `plans` has no
// plan for. `by_offset` maps offsets to the items of the sequence. `turn` is the edit that the next plan takes.
// Krawall's row effects go into `replays`.
void PlanNotes(const std::string& name, const File& file, uint32_t sequence, const Sequence& song,
               const std::string& sound_name, const std::map<uint32_t, uint32_t>& by_offset,
               std::map<uint32_t, std::map<uint32_t, Plan>>& plans, std::map<uint32_t, std::set<uint32_t>>& replays,
               std::size_t& turn, Totals& totals);

// Gives each command of `split` a new velocity in place of a move or a longer length.
void VelocitiesForSplitRuns(const File& file, const std::map<uint32_t, std::set<uint32_t>>& split,
                            std::map<uint32_t, std::map<uint32_t, uint32_t>>& items,
                            std::map<uint32_t, std::map<uint32_t, Plan>>& plans);

// Counts the plans by their change in `totals`.
void CountPlans(const std::map<uint32_t, std::map<uint32_t, Plan>>& plans, Totals& totals);

// One run of the check on the archive `name`, from the file before the edits to the archives that the edited file
// builds.
struct EditsRun
{
    std::string name;
    bool timelines = false;

    // The archive, the archive that the program plays, the Hokuto file, and the sequence of each of the archive's
    // files.
    CheckedArchive archive;
    CheckedArchive playing;
    File file;
    std::vector<std::optional<uint32_t>> sequences;

    // Each sequence sound before the edits, and the plan of each note command by sequence and offset. `items` contains
    // the items of each sequence by offset, and `replays` the commands of Krawall's notes that an effect of a row
    // starts.
    std::vector<std::optional<Sequence>> before;
    std::map<uint32_t, std::map<uint32_t, Plan>> plans;
    std::map<uint32_t, std::map<uint32_t, uint32_t>> items;
    std::map<uint32_t, std::set<uint32_t>> replays;

    // The file after the edits, and where each sequence's items went.
    File edited;
    std::map<uint32_t, std::vector<std::optional<uint32_t>>> moved;

    // The edited archive that a saved file holds, the archive that the program plays, and the file that the first
    // imports into again. `again_of` gives the sequence of `again` that plays each edited sequence's sounds.
    CheckedArchive saved;
    CheckedArchive after;
    File again;
    std::map<uint32_t, uint32_t> again_of;

    // By sequence, the offsets after the edits of the note commands that no plan names, and of the moved notes.
    std::map<uint32_t, std::set<uint32_t>> hidden;
    std::map<uint32_t, std::set<uint32_t>> moved_notes;
};

// Reads sound `sound` of sequence `sequence` from the edited archive of `run`, and compares the notes that it plays
// with the notes before the edits and their plans. Prints a line for a sound that differs, and counts in `totals`.
void CheckSoundAgain(EditsRun& run, uint32_t sound, uint32_t sequence, Totals& totals);

} // namespace hokuto::tools::edits
