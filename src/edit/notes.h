// SPDX-License-Identifier: MIT

// Edits of a sequence's notes. A note in the arrangement leads to the note command that started it (song::Note). An
// edit changes that command. Every pass that runs the command plays the change: each call of a subroutine and each pass
// of a loop.
//
// A note's key, velocity and length belong to its command alone. A move takes the note out from between the waits
// around it and puts it back at its new tick. The wait that the tick falls in splits in two. Every other command keeps
// its tick. A new note goes in at its tick the same way, and so does a new setting, such as a volume. A setting goes in
// before the notes at its tick. Those notes play with it. A move stays inside the note's block: a run of commands that
// other code enters only at its start. Jumps, calls, returns, loop starts and labels begin blocks. Commands that move a
// track elsewhere end them. So do a wait whose length comes from a random number, a variable or a comparison and a
// command that turns the note wait on or off. A note can't move or go in at the tick where its track ends. The console
// releases the track's notes in that tick. The note wouldn't sound.
//
// A timeline (edit/timelines.h) holds a MIDI file's tracks. Its notes and settings move to any tick of their track
// before the loop's end, across the blocks. A note at or past the end of a track that ends there moves the track's end.
// The track then lasts to the note's end. In a song that loops, such a note still starts before the loop's end.
//
// On a track whose notes wait for their length (MML notewait), a move, a new length, a new note or a new setting first
// turns the note wait off for its run: the commands of its block between the notes that wait for an unknown time. A
// wait of its own then takes the place of each note's wait in the run, and the note wait comes back on after the run.
// Every command keeps its tick, and a note moves only inside its run. Every track that can enter the block has to enter
// it with the note wait on. A note of length 0 waits until the track's notes have ended, and a note whose length the
// sequence sets as it plays waits for an unknown time. Neither can move or change its length, and neither can a note
// whose wait depends on a comparison. A note removed on such a track leaves a wait of its length. A note of length 0
// can't go.
//
// A setting, such as a volume or a tempo, moves to another tick the same way: inside its block, and on a track whose
// notes wait for their length, inside its run.
//
// A Wii sequence takes these edits by the rules of the file's build of NW4R SND.

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "ntdlib/commands/sequence.h"

#include "edit/bytecode.h"
#include "format/file.h"
#include "song/sequence.h"

namespace hokuto::edit
{

// A change to a note. A field without a value leaves the note's command as it is.
struct NoteEdit
{
    uint32_t command = 0;  // the offset of the note's command in the sequence's bytecode (song::Note::command)
    song::Stretch stretch; // the stretch of the track's commands that ran it
    int32_t shift = 0;     // ticks to move the note by, later for more than 0
    int transpose = 0;     // semitones to move the note's key by
    std::optional<int> velocity;
    std::optional<uint32_t> length; // in ticks
    bool remove = false;
};

// A note to add, in `stretch`, at `tick` in the sequence's ticks. The key is the command's key, before any transpose.
struct NewNote
{
    song::Stretch stretch;
    uint32_t tick = 0;
    int key = 60;
    int velocity = 100;
    uint32_t length = 48;
};

// A command of a setting to add, in `stretch`, at `tick` in the sequence's ticks, such as a new volume or pan.
struct NewSetting
{
    song::Stretch stretch;
    uint32_t tick = 0;
    ntdlib::commands::Command command;
};

// A move of a setting's command to another tick: the offset of the command in the sequence's bytecode, the stretch of
// the track's commands that ran it, and the ticks to move it by, later for more than 0.
struct SettingMove
{
    uint32_t command = 0;
    song::Stretch stretch;
    int32_t shift = 0;
};

// How far a note can move: the most ticks that it can move earlier or later.
struct ShiftRange
{
    friend bool operator==(const ShiftRange&, const ShiftRange&) = default;

    uint32_t earlier = 0;
    uint32_t later = 0;
};

// A note of a sequence as an editor finds it, or a setting: the offset of its command (song::Note::command), and the
// stretch of the track's commands that ran it.
struct NotePlace
{
    uint32_t command = 0;
    song::Stretch stretch;
};

// What an edit can change in a note: how far the note can move, and the length of its command in ticks. Each is nothing
// when the note can't move, or when its command's length isn't a number of ticks. A note that can't move can't change
// its length either. A setting has no length.
struct NoteLimits
{
    std::optional<ShiftRange> shift;
    std::optional<uint32_t> length;
};

// The file after `edits` and `additions` to sequence `sequence` of `file`. A command that several edits name takes the
// first of them. The labels, the targets of jumps, calls and track starts, and the sounds' starts move with the
// commands. Throws std::invalid_argument with a message for the user for an edit that the sequence can't take: a
// command that isn't a note, a key or velocity outside 0 to 127, a move or an addition outside its block or at the tick
// where its track ends, and the changes of timing above that a track can't take.
Edited EditNotes(const format::File& file, uint32_t sequence, std::span<const NoteEdit> edits,
                 std::span<const NewNote> additions = {});

// The file after `settings` go into sequence `sequence` of `file`, each at its tick. `Edited::added` holds the item of
// each new command, in order. The labels, the targets and the sounds' starts move with the commands. EditNotes moves
// them the same way.
// Throws std::invalid_argument with a message for the user for a command that waits, moves the track elsewhere or
// changes the note wait, for a tick outside every block of its stretch, for a command that the console's player skips
// (song::Runs), and for values that the console's bytes can't hold.
Edited AddSettings(const format::File& file, uint32_t sequence, std::span<const NewSetting> settings);

// The file after `moves` take settings of sequence `sequence` of `file` to other ticks. A setting moves as a note does,
// and goes in before the notes at its new tick. They play with it. Every other command keeps its tick. A command that
// several moves name takes the first of them, and a move of 0 ticks leaves its command where it is. Throws
// std::invalid_argument with a message for the user for a command that isn't a setting, the timebase, a command that
// runs only if the last comparison held, one that reads or changes a variable, and a move past what ShiftRangeOf gives.
Edited MoveSettings(const format::File& file, uint32_t sequence, std::span<const SettingMove> moves);

// The stretch of `stretches` (song::Track::stretches) that a new note or a new setting at `tick` goes into: the last
// that starts by the tick. At the start of a call, that's the call. Nothing when every stretch starts later.
std::optional<song::Stretch> StretchAt(std::span<const song::Stretch> stretches, uint32_t tick);

// How far the note or the setting whose command is at `command` in sequence `sequence` of `file` can move, in the block
// that `stretch` ran it in. Nothing for one that can't move.
std::optional<ShiftRange> ShiftRangeOf(const format::File& file, uint32_t sequence, uint32_t command,
                                       const song::Stretch& stretch);

// The limits of each note or setting of `notes` in sequence `sequence` of `file`, in order. One look at the sequence
// serves them all. Any other command, and a setting that MoveSettings refuses, can't move. A timeline's note moves up
// to a tick before the loop's end, and past the end of a track that ends: up to the song's end (TimelineSongEnd), or
// far past it in a song without a loop. Where the header doesn't give that tick, the note stays inside its track. Where
// the song loops, `loop_end` gives the end of the loop that the arrangement shows (TimelineRead). No note or setting of
// a timeline then moves to it or past it. The editors show nothing there. A track pasted from a song with another loop
// can loop over a pass of another length. The arrangement then shows the loop that ntdlib's performer finds. That loop
// can end before the pasted track's loop does. Throws std::invalid_argument for an offset where no command starts.
std::vector<NoteLimits> LimitsOf(const format::File& file, uint32_t sequence, std::span<const NotePlace> notes,
                                 std::optional<uint32_t> loop_end = std::nullopt);

// Why MoveSettings can't move `command`, for the user: a command that isn't a setting, the timebase, a command that
// runs only if the last comparison held, or one that reads or changes a variable. Nothing for a setting that can move.
std::optional<std::string> WhyFixedInTime(const ntdlib::commands::Command& command);

// The key of the note command at `command` in sequence `sequence` of `file`, before any transpose. Throws
// std::invalid_argument for a command that isn't a note.
int CommandKey(const format::File& file, uint32_t sequence, uint32_t command);

// The note commands that `edited`, an edit of sequence `sequence` of `file`, made of the commands at `commands`, then
// the new notes' commands: the offset of each one in the edited sequence. A note that the edit removed is left out.
// Throws std::invalid_argument for an offset where no command starts.
std::vector<uint32_t> CommandsAfter(const format::File& file, uint32_t sequence, const Edited& edited,
                                    std::span<const uint32_t> commands);

} // namespace hokuto::edit
