// SPDX-License-Identifier: MIT

// What the Key Editor works out about its notes without a window: the grid, how far the selected notes can move, the
// edits of the keys and the menu, the selection in a new version, and the note that sounds. Their tests run without a
// window.

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "app/key_editor.h"
#include "edit/notes.h"

namespace hokuto::app
{

// The keys of the MIDI range, and the highest velocity.
inline constexpr int kKeys = 128;
inline constexpr int kTopVelocity = 127;

// Nintendo's sequences have no time signature. While bars show, the ruler and the info line count bars of 4/4.
inline constexpr int kBeatsPerBar = 4;

// The name of `key`, with middle C, key 60, as C3. ParseKeyName reads a name that the user typed, such as C3, c#3, Db3
// or C-2. A sharp is # or ♯, and a flat b or ♭. Nothing for any other text, or for a key outside 0 to 127.
std::string KeyName(int key);
std::optional<int> ParseKeyName(std::string_view text);

// A position for a message: the display's text of `quarters` without its figure spaces.
std::string PlainPosition(double quarters);

// The grid's step in ticks.
double GridTicks(const KeyEditorState& state, int ticks_per_quarter);

// `tick` on the grid: the nearest line, or with `down` the line at or before it. Without the snap, the nearest tick.
int64_t Snapped(const KeyEditorState& state, int ticks_per_quarter, double tick, bool down);

// The position in quarter notes that a click on the Key Editor's ruler at `tick` gives. With the snap on, the position
// goes to the grid's nearest line. Without the snap, it stays at the click. The arrangement's ruler places it there
// too. It never goes before the song's start.
double RulerPosition(const KeyEditorState& state, int ticks_per_quarter, double tick);

// How far the commands of `commands` can move together: the least room earlier and later among them
// (KeyEditorState::limits). Nothing when one of them can't move. The selected notes move as far as the range for
// `state.selected` allows.
std::optional<edit::ShiftRange> SharedRange(const KeyEditorState& state, const std::set<uint32_t>& commands);

// What the user reads when a setting stays at its tick though edit::WhyFixedInTime finds nothing against its command.
// The notes of its track then leave it no room (edit::LimitsOf).
inline constexpr std::string_view kTimedByNotes =
    "This track's notes set its timing here. Hokuto can't move the command in time yet.";

// The length that a size starts from for each selected command: the command's length, or the length that a note of
// length 0 sounds. Nothing when one of the commands can't change its length.
std::optional<std::map<uint32_t, uint32_t>> BaseLengths(const KeyTrack& track, const KeyEditorState& state);

// The velocity of each selected command, from the first note that it plays.
std::map<uint32_t, int> Velocities(const KeyTrack& track, const KeyEditorState& state);

// How far the selected notes can move up and down together. Every key has to stay from 0 to 127.
std::pair<int, int> TransposeRange(const KeyTrack& track, const KeyEditorState& state);

// The edit that removes the notes of `commands`.
KeyEdit Removal(const std::set<uint32_t>& commands);

// The edit that moves the selected notes `semitones` up, or down for a negative number. Its name is Up or Down. Nothing
// when a key would leave the range.
std::optional<KeyEdit> Transposition(const KeyTrack& track, const KeyEditorState& state, int semitones);

// The edit of Edit > Quantize. It moves each selected note to the nearest line of the toolbar's grid, or every note
// while none is selected, whether Snap is on or off. A command moves by its first note's distance from the line. A note
// that the commands around it keep from the line stays where it is. When no note can move, the edit says why. Nothing
// when every note is on the grid.
std::optional<KeyEdit> Quantization(const KeyTrack& track, const KeyEditorState& state);

// The notes that Cut and Copy take: the first note of each selected command, in the order that they start, from tick 0
// at the first. The notes keep their keys, velocities, lengths and instruments. A command that plays several notes,
// such as a note in a subroutine with two calls, gives its first one.
std::vector<KeyNote> CopiedNotes(const KeyTrack& track, const KeyEditorState& state);

// The edit of Edit > Paste: the clipboard's notes from `tick` on. Each note lies as far from `tick` as it lay from the
// first copied note. Nothing for an empty clipboard.
std::optional<KeyEdit> Pasting(const std::vector<KeyNote>& clipboard, uint32_t tick);

// Carries out the Edit menu's command in `state.request`, and clears it. `position` is the transport's position, in
// ticks. Cut and Copy fill the clipboard. Returns the edit that the command makes: Quantize's, Cut's removal of the
// selected notes, or Paste's new notes at the position.
std::optional<KeyEdit> RequestedEdit(const KeyTrack& track, KeyEditorState& state, double position);

// The fields of the Key Editor's info line that can be typed in.
enum class InfoValue
{
    kStart,
    kEnd,
    kLength,
    kPitch,
    kVelocity,
};

// The edit of `text`, typed into the info line's field of `value`. The first selected note takes the typed value, and
// the other selected notes change by the same amount. A start moves the notes, an end or a length sizes them, a pitch
// transposes them, and a velocity changes their velocities. A typed end or length gives the first note's command that
// length: a note can sound for less, such as a note of a one-shot wave. The edits take the names Move, Size, Set Note
// Pitch and Set Note Velocity. When the notes can't take the change, the edit says why. Text that the field can't read
// is refused with what the field takes. Nothing for the value that the first note has already, or without a selected
// note.
std::optional<KeyEdit> TypedEdit(const KeyTrack& track, const KeyEditorState& state, InfoValue value,
                                 std::string_view text);

// The edit that refuses `text` when a field of the info line can't read it as `what`, such as "a start". `hint` says
// what the field takes.
KeyEdit Unreadable(std::string_view text, std::string_view what, std::string_view hint);

// The note whose instrument plays at `tick`: the last note that starts by then, or else the first note. Nothing on a
// track without notes. WithInstrumentAt gives `note` that instrument.
const KeyNote* InstrumentNoteAt(const KeyTrack& track, double tick);
KeyNote WithInstrumentAt(const KeyTrack& track, double tick, KeyNote note);

// Whether `gesture` goes on in a controller lane: a drag of a point, a selection box or a point being drawn.
bool InLane(const KeyGesture& gesture);

// Whether `gesture` has lost what it acts on in the present view of `track` with `lanes` controller lanes: its note,
// its point or its lane. A gesture that holds none of them, such as a note being drawn on a track without notes, never
// has.
bool GestureGone(const KeyGesture& gesture, const KeyTrack& track, std::size_t lanes);

// A selection of the Key Editor's notes: the commands of the selected notes on track `track`, in `sequence`, the
// version of the shown sequence that the commands belong to. The commands of another version can lie at other offsets.
struct KeySelection
{
    format::Shared<ntdlib::commands::Sequence> sequence;
    int track = -1;
    std::set<uint32_t> commands;
};

// The selection once the Key Editor shows `shown`, a version of the shown sequence. `current` is the selection so far.
// `waiting` is the selection that an edit, an undo or a redo left for its version. The current selection stays while
// the sequence stays the same. In another version, the waiting selection takes its place when it names the commands of
// `shown` on the current track. Otherwise nothing stays selected.
std::set<uint32_t> FollowedSelection(KeySelection current, std::optional<KeySelection> waiting,
                                     const format::Shared<ntdlib::commands::Sequence>& shown);

// The note that sounds while the gestures hold it, or nothing.
//
// With Acoustic Feedback on, a note sounds while the mouse holds it: a note pressed with Object Selection, at the key
// that a move takes it to, and a note being drawn. A press on a note's end sizes the note and plays nothing. Notes that
// the arrows move sound while the arrow is held: the first selected note, at its new key. A press on the keyboard plays
// its key whether Acoustic Feedback is on or off, with the velocity of drawn notes. A drawn note plays the instrument
// of the last note that starts by the drawn note's start, and a key of the keyboard that of the last note that starts
// by the position. Before the first note, the first note's instrument plays.
std::optional<KeyNote> SoundingNote(const KeyTrack& track, const KeyEditorState& state);

} // namespace hokuto::app
