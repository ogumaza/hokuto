// SPDX-License-Identifier: MIT

// Tests what the Key Editor works out about its notes (src/app/key_edits.h), without a window: Quantize's edit, the
// clipboard, Select All, the names of the keys, the edits of typed values, the arrow keys' moves, the instrument at a
// tick, the gestures that have lost their notes or their lanes, the selection in a new version, the note that Acoustic
// Feedback sounds for each gesture, and the position that a click on the ruler places.

#include "app/key_edits.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "xsflib/testing/check.h"

#include "app/key_editor.h"
#include "edit/notes.h"
#include "format/shared.h"

using hokuto::app::CopiedNotes;
using hokuto::app::FollowedSelection;
using hokuto::app::GestureGone;
using hokuto::app::InfoValue;
using hokuto::app::InstrumentNoteAt;
using hokuto::app::KeyChange;
using hokuto::app::KeyEdit;
using hokuto::app::KeyEditorState;
using hokuto::app::KeyGesture;
using hokuto::app::KeyName;
using hokuto::app::KeyNote;
using hokuto::app::KeyRequest;
using hokuto::app::KeySelection;
using hokuto::app::KeyTrack;
using hokuto::app::ParseKeyName;
using hokuto::app::Pasting;
using hokuto::app::Quantization;
using hokuto::app::RequestedEdit;
using hokuto::app::RulerPosition;
using hokuto::app::SoundingNote;
using hokuto::app::Transposition;
using hokuto::app::TypedEdit;
using hokuto::app::WithInstrumentAt;
using hokuto::edit::NoteLimits;
using hokuto::edit::ShiftRange;

namespace
{

// A note of command `command` at `start`, a quarter note long, with program `program` of bank slot 0.
KeyNote NoteAt(uint32_t start, uint32_t command, int key = 60, uint32_t program = 0)
{
    KeyNote note;
    note.start = start;
    note.length = 48;
    note.key = key;
    note.velocity = 100;
    note.command = command;
    note.program = program;

    return note;
}

// A track at 48 ticks a quarter note with `notes`, in the order that they start.
KeyTrack TrackOf(std::vector<KeyNote> notes)
{
    KeyTrack track;
    track.name = "Track 0";
    track.ticks_per_quarter = 48;
    track.song_length = 48 * 16;
    track.notes = std::move(notes);

    return track;
}

// A state with a grid of 1/16, 12 ticks at 48 ticks a quarter note, and limits that let each of `commands` move 24
// ticks either way.
KeyEditorState StateFor(const std::vector<uint32_t>& commands)
{
    KeyEditorState state;
    state.grid = 16;
    for (const uint32_t command : commands)
    {
        state.limits[command] = NoteLimits{ShiftRange{24, 24}, 48};
    }

    return state;
}

// Whether `change` only moves `command` by `shift` ticks.
bool IsShift(const KeyChange& change, uint32_t command, int32_t shift)
{
    return change.command == command && change.shift == shift && change.transpose == 0 && !change.velocity &&
           !change.length && !change.remove;
}

// Whether `change` only gives `command` the length `length`.
bool IsLength(const KeyChange& change, uint32_t command, uint32_t length)
{
    return change.command == command && change.shift == 0 && change.transpose == 0 && !change.velocity &&
           change.length == length && !change.remove;
}

// Quantize moves each selected note to the nearest line of the grid: 3 to 0 and 25 to 24. A note on a line and a note
// that isn't selected stay. The edit is named Quantize in the Undo History.
void TestQuantizeSelection()
{
    const KeyTrack track = TrackOf({NoteAt(3, 10), NoteAt(25, 11), NoteAt(48, 12), NoteAt(55, 13)});
    KeyEditorState state = StateFor({10, 11, 12, 13});
    state.selected = {10, 11, 12};

    const std::optional<KeyEdit> edit = Quantization(track, state);

    SEQLIB_REQUIRE(edit.has_value());
    XSFLIB_CHECK(edit->name == "Quantize" && edit->refused.empty() && edit->added.empty());
    SEQLIB_REQUIRE_EQ(edit->changes.size(), 2);
    XSFLIB_CHECK(IsShift(edit->changes[0], 10, -3));
    XSFLIB_CHECK(IsShift(edit->changes[1], 11, -1));
}

// Without a selection, every note moves. A note halfway between two lines goes to the later one. Snap needn't be on,
// and the toolbar's grid sets the lines: 1/8 is 24 ticks.
void TestQuantizeEverything()
{
    const KeyTrack track = TrackOf({NoteAt(3, 10), NoteAt(18, 11), NoteAt(36, 12)});
    KeyEditorState state = StateFor({10, 11, 12});
    state.snap = false;
    state.grid = 8;

    const std::optional<KeyEdit> edit = Quantization(track, state);

    SEQLIB_REQUIRE(edit.has_value());
    SEQLIB_REQUIRE_EQ(edit->changes.size(), 3);
    XSFLIB_CHECK(IsShift(edit->changes[0], 10, -3));
    XSFLIB_CHECK(IsShift(edit->changes[1], 11, 6));
    XSFLIB_CHECK(IsShift(edit->changes[2], 12, 12));
}

// A command that plays two notes, such as a note in a subroutine with two calls, moves once, by its first note's
// distance from the grid.
void TestQuantizeFirstNote()
{
    const KeyTrack track = TrackOf({NoteAt(3, 10), NoteAt(52, 10)});
    const KeyEditorState state = StateFor({10});

    const std::optional<KeyEdit> edit = Quantization(track, state);

    SEQLIB_REQUIRE(edit.has_value());
    SEQLIB_REQUIRE_EQ(edit->changes.size(), 1);
    XSFLIB_CHECK(IsShift(edit->changes[0], 10, -3));
}

// A note that the commands around it keep from the line stays. The others move. When no note can move, the edit changes
// nothing and says why: a track whose notes set its timing can't move them, and a block keeps a note between the jumps,
// calls and loop points around it. Notes on the grid make no edit.
void TestQuantizeLimits()
{
    const KeyTrack track = TrackOf({NoteAt(3, 10), NoteAt(10, 11)});
    KeyEditorState some = StateFor({10, 11});
    some.limits[11].shift = ShiftRange{24, 1};
    KeyEditorState kept = some;
    kept.selected = {11};
    KeyEditorState timed = StateFor({10, 11});
    timed.limits[10].shift.reset();
    timed.limits[11].shift.reset();
    const KeyTrack on_grid = TrackOf({NoteAt(0, 10), NoteAt(12, 11)});

    const std::optional<KeyEdit> partly = Quantization(track, some);
    const std::optional<KeyEdit> refused = Quantization(track, kept);
    const std::optional<KeyEdit> fixed = Quantization(track, timed);
    const std::optional<KeyEdit> nothing = Quantization(on_grid, some);

    SEQLIB_REQUIRE(partly && partly->changes.size() == 1);
    XSFLIB_CHECK(IsShift(partly->changes[0], 10, -3) && partly->refused.empty());
    SEQLIB_REQUIRE(refused.has_value());
    XSFLIB_CHECK(refused->changes.empty() &&
                 refused->refused.find("jumps, calls and loop points") != std::string::npos);
    SEQLIB_REQUIRE(fixed.has_value());
    XSFLIB_CHECK(fixed->changes.empty() && fixed->refused.find("set its timing") != std::string::npos);
    XSFLIB_CHECK(!nothing.has_value());
}

// A sequence that Hokuto can't edit has no limits. Its notes go into the edit all the same: the note at 3 moves to 0.
void TestQuantizeWithoutLimits()
{
    const KeyTrack track = TrackOf({NoteAt(3, 10)});
    const KeyEditorState state;

    const std::optional<KeyEdit> edit = Quantization(track, state);

    SEQLIB_REQUIRE(edit && edit->changes.size() == 1);
    XSFLIB_CHECK(IsShift(edit->changes[0], 10, -3));
}

// Copy takes the first note of each selected command, from tick 0 at the first, with its key, velocity, length and
// instrument. A command that plays two notes gives its first.
void TestCopiedNotes()
{
    KeyNote played_twice = NoteAt(30, 11, 64, 5);
    played_twice.velocity = 70;
    played_twice.length = 6;
    played_twice.bank_slot = 1;
    const KeyTrack track = TrackOf({NoteAt(12, 10), NoteAt(20, 12, 62), played_twice, NoteAt(60, 13), NoteAt(78, 11)});
    KeyEditorState state;
    state.selected = {10, 11};

    const std::vector<KeyNote> copied = CopiedNotes(track, state);

    SEQLIB_REQUIRE_EQ(copied.size(), 2);
    XSFLIB_CHECK(copied[0].start == 0 && copied[0].command == 10 && copied[0].key == 60);
    XSFLIB_CHECK(copied[1].start == 18 && copied[1].command == 11 && copied[1].key == 64);
    XSFLIB_CHECK(copied[1].velocity == 70 && copied[1].length == 6 && copied[1].bank_slot == 1 &&
                 copied[1].program == 5);
}

// Paste puts the clipboard's notes at the position. They keep their distances. Cut takes the selected notes into the
// clipboard and removes them in an edit named Delete. The Undo History names a cut that way. Copy fills the clipboard
// and makes no edit. Paste rounds the position to a tick, and an empty clipboard pastes nothing. Each command clears
// the request.
void TestClipboard()
{
    const KeyTrack track = TrackOf({NoteAt(12, 10), NoteAt(30, 11, 64), NoteAt(60, 12)});
    KeyEditorState state;
    state.selected = {10, 11};
    KeyEditorState empty;

    state.request = KeyRequest::kCopy;
    const std::optional<KeyEdit> copy = RequestedEdit(track, state, 0.0);
    const std::vector<KeyNote> copied = state.clipboard;
    state.request = KeyRequest::kPaste;
    const std::optional<KeyEdit> paste = RequestedEdit(track, state, 95.6);
    state.clipboard.clear();
    state.request = KeyRequest::kCut;
    const std::optional<KeyEdit> cut = RequestedEdit(track, state, 0.0);
    empty.request = KeyRequest::kPaste;
    const std::optional<KeyEdit> nothing = RequestedEdit(track, empty, 48.0);

    XSFLIB_CHECK(!copy.has_value() && copied.size() == 2);
    SEQLIB_REQUIRE(paste.has_value());
    XSFLIB_CHECK(paste->name == "Paste" && paste->changes.empty());
    SEQLIB_REQUIRE(paste->added.size() == 2);
    XSFLIB_CHECK(paste->added[0].start == 96 && paste->added[0].key == 60);
    XSFLIB_CHECK(paste->added[1].start == 114 && paste->added[1].key == 64);
    SEQLIB_REQUIRE(cut.has_value());
    XSFLIB_CHECK(cut->name == "Delete" && cut->changes.size() == 2 && cut->changes[0].remove && cut->changes[1].remove);
    XSFLIB_CHECK(state.clipboard.size() == 2 && state.request == KeyRequest::kNone);
    XSFLIB_CHECK(!nothing.has_value() && empty.request == KeyRequest::kNone);
    XSFLIB_CHECK(!Pasting({}, 0).has_value());
}

// Select All selects every note's command, a command that plays two notes once, and no point of the controller lanes.
// It makes no edit, and clears the request.
void TestSelectAll()
{
    const KeyTrack track = TrackOf({NoteAt(12, 10), NoteAt(30, 11, 64), NoteAt(42, 11, 64), NoteAt(60, 12)});
    KeyEditorState state;
    state.selected = {11};
    state.selected_controls = {3};
    state.request = KeyRequest::kSelectAll;

    const std::optional<KeyEdit> edit = RequestedEdit(track, state, 0.0);

    XSFLIB_CHECK(!edit.has_value() && state.request == KeyRequest::kNone);
    XSFLIB_CHECK((state.selected == std::set<uint32_t>{10, 11, 12}) && state.selected_controls.empty());
}

// A key's name puts middle C, key 60, at C3. A typed name reads back every key, in either case. A sharp is # or ♯, and
// a flat b or ♭. A key outside 0 to 127, a letter after G, a missing octave and anything else can't be read.
void TestKeyNames()
{
    XSFLIB_CHECK(KeyName(60) == "C3" && KeyName(0) == "C-2" && KeyName(127) == "G8" && KeyName(61) == "C#3");
    for (int key = 0; key < 128; key++)
    {
        XSFLIB_CHECK(ParseKeyName(KeyName(key)) == key);
    }
    XSFLIB_CHECK(ParseKeyName(" c3 ") == 60);
    XSFLIB_CHECK(ParseKeyName("Db3") == 61 && ParseKeyName("D♭3") == 61 && ParseKeyName("C♯3") == 61);
    XSFLIB_CHECK(ParseKeyName("bb3") == 70 && ParseKeyName("B#2") == 60 && ParseKeyName("Cb3") == 59);
    for (const char* text : {"", "H3", "C", "C#", "3", "G#8", "Cb-2", "C3.5", "C 3", "C##3", "C+3", "C100", "-C3"})
    {
        XSFLIB_CHECK(!ParseKeyName(text).has_value());
    }
}

// A typed start moves the selected notes together, by the first note's distance to it. A typed value goes into the
// track's ticks: 48 to the quarter note here. A start past the room of a note's commands refuses, and so does a track
// whose notes set its timing. The first note's start makes no edit. Text that isn't a position is refused with an
// example of one. Without limits, the edit goes ahead, and the edit of the commands says why it can't.
void TestTypedStart()
{
    const KeyTrack track = TrackOf({NoteAt(48, 10), NoteAt(96, 11, 64)});
    KeyEditorState state = StateFor({10, 11});
    state.selected = {10, 11};
    state.limits[11].shift = ShiftRange{96, 12};
    KeyEditorState timed = state;
    timed.limits[10].shift.reset();
    KeyEditorState unknown = state;
    unknown.limits.clear();

    const std::optional<KeyEdit> moved = TypedEdit(track, state, InfoValue::kStart, "2. 2");
    const std::optional<KeyEdit> same = TypedEdit(track, state, InfoValue::kStart, "2");
    const std::optional<KeyEdit> unread = TypedEdit(track, state, InfoValue::kStart, "2. 5");
    const std::optional<KeyEdit> late = TypedEdit(track, state, InfoValue::kStart, "2. 3. 60");
    const std::optional<KeyEdit> early = TypedEdit(track, state, InfoValue::kStart, "1");
    const std::optional<KeyEdit> fixed = TypedEdit(track, timed, InfoValue::kStart, "2. 2");
    const std::optional<KeyEdit> passed = TypedEdit(track, unknown, InfoValue::kStart, "2. 2");

    SEQLIB_REQUIRE(moved && moved->changes.size() == 2);
    XSFLIB_CHECK(moved->name == "Move" && moved->refused.empty());
    XSFLIB_CHECK(IsShift(moved->changes[0], 10, 12) && IsShift(moved->changes[1], 11, 12));
    XSFLIB_CHECK(!same.has_value());
    SEQLIB_REQUIRE(unread && late && early && fixed && passed);
    XSFLIB_CHECK(unread->changes.empty() &&
                 unread->refused == "Hokuto can't read \"2. 5\" as a start. A start is a position, such as 2. 1. 0.");
    XSFLIB_CHECK(late->changes.empty() && late->refused.find("no later than 2. 2. 0.") != std::string::npos);
    XSFLIB_CHECK(early->changes.empty() && early->refused.find("no earlier than 1. 3. 0.") != std::string::npos);
    XSFLIB_CHECK(fixed->changes.empty() && fixed->refused.find("set its timing") != std::string::npos);
    XSFLIB_CHECK(passed->changes.size() == 2 && passed->refused.empty() && IsShift(passed->changes[1], 11, 12));
}

// A typed end or length sizes the selected notes together, by the first note's change. A note lasts a tick at least: an
// end at the note's start refuses. So does a track whose notes set its timing. The first note's length makes no edit.
// Without limits, each command's first note gives its length. Text that isn't a position or a length is refused with an
// example of one.
void TestTypedLength()
{
    KeyNote longer = NoteAt(96, 11);
    longer.length = 96;
    const KeyTrack track = TrackOf({NoteAt(48, 10), longer});
    KeyEditorState state = StateFor({10, 11});
    state.selected = {10, 11};
    state.limits[11].length = 96;
    KeyEditorState timed = state;
    timed.limits[11].length.reset();
    KeyEditorState unknown = state;
    unknown.limits.clear();

    const std::optional<KeyEdit> ended = TypedEdit(track, state, InfoValue::kEnd, "3. 3");
    const std::optional<KeyEdit> shortened = TypedEdit(track, state, InfoValue::kLength, "0. 1. 60");
    const std::optional<KeyEdit> same = TypedEdit(track, state, InfoValue::kLength, "1");
    const std::optional<KeyEdit> empty = TypedEdit(track, state, InfoValue::kEnd, "2");
    const std::optional<KeyEdit> fixed = TypedEdit(track, timed, InfoValue::kLength, "2");
    const std::optional<KeyEdit> passed = TypedEdit(track, unknown, InfoValue::kLength, "2");
    const std::optional<KeyEdit> unread_end = TypedEdit(track, state, InfoValue::kEnd, "3. 5");
    const std::optional<KeyEdit> unread_length = TypedEdit(track, state, InfoValue::kLength, "0. 4");

    SEQLIB_REQUIRE(ended && ended->changes.size() == 2 && shortened && shortened->changes.size() == 2);
    XSFLIB_CHECK(ended->name == "Size" && ended->refused.empty());
    XSFLIB_CHECK(IsLength(ended->changes[0], 10, 72) && IsLength(ended->changes[1], 11, 120));
    XSFLIB_CHECK(IsLength(shortened->changes[0], 10, 18) && IsLength(shortened->changes[1], 11, 66));
    XSFLIB_CHECK(!same.has_value());
    SEQLIB_REQUIRE(empty && fixed && passed);
    XSFLIB_CHECK(empty->changes.empty() && empty->refused.find("a tick at least") != std::string::npos);
    XSFLIB_CHECK(fixed->changes.empty() && fixed->refused.find("set its timing") != std::string::npos);
    SEQLIB_REQUIRE_EQ(passed->changes.size(), 2);
    XSFLIB_CHECK(IsLength(passed->changes[0], 10, 96) && IsLength(passed->changes[1], 11, 144));
    SEQLIB_REQUIRE(unread_end && unread_length);
    XSFLIB_CHECK(unread_end->changes.empty() &&
                 unread_end->refused == "Hokuto can't read \"3. 5\" as an end. An end is a position, such as 2. 1. 0.");
    XSFLIB_CHECK(unread_length->changes.empty() &&
                 unread_length->refused ==
                     "Hokuto can't read \"0. 4\" as a length. A length is a number of beats, "
                     "sixteenths and ticks, such as 0. 1. 0.");
}

// A typed pitch transposes the selected notes together, and a typed velocity changes their velocities by the same
// amount, from 1 to 127. A key outside the MIDI range refuses. Text that isn't a key's name, and a velocity outside 1
// to 127, are refused with what the field takes. Nothing changes without a selection.
void TestTypedPitchAndVelocity()
{
    KeyNote softer = NoteAt(96, 11, 64);
    softer.velocity = 80;
    const KeyTrack track = TrackOf({NoteAt(48, 10), softer});
    KeyEditorState state = StateFor({10, 11});
    state.selected = {10, 11};

    const std::optional<KeyEdit> transposed = TypedEdit(track, state, InfoValue::kPitch, "d3");
    const std::optional<KeyEdit> too_high = TypedEdit(track, state, InfoValue::kPitch, "G8");
    const std::optional<KeyEdit> same_key = TypedEdit(track, state, InfoValue::kPitch, "C3");
    const std::optional<KeyEdit> louder = TypedEdit(track, state, InfoValue::kVelocity, " 127 ");
    const std::optional<KeyEdit> softest = TypedEdit(track, state, InfoValue::kVelocity, "1");
    const std::optional<KeyEdit> unread_key = TypedEdit(track, state, InfoValue::kPitch, "H3");

    SEQLIB_REQUIRE(transposed && transposed->changes.size() == 2);
    XSFLIB_CHECK(transposed->name == "Set Note Pitch" && transposed->changes[0].transpose == 2 &&
                 transposed->changes[1].transpose == 2);
    SEQLIB_REQUIRE(too_high.has_value());
    XSFLIB_CHECK(too_high->name == "Set Note Pitch" && too_high->changes.empty() &&
                 too_high->refused.find("C-2 to G8") != std::string::npos);
    XSFLIB_CHECK(!same_key.has_value());
    SEQLIB_REQUIRE(unread_key.has_value());
    XSFLIB_CHECK(
        unread_key->changes.empty() &&
        unread_key->refused ==
            "Hokuto can't read \"H3\" as a pitch. A pitch is a key's name from C-2 to G8, such as C3 for middle C.");
    SEQLIB_REQUIRE(louder && louder->changes.size() == 2 && softest && softest->changes.size() == 2);
    XSFLIB_CHECK(louder->name == "Set Note Velocity" && louder->changes[0].velocity == 127 &&
                 louder->changes[1].velocity == 107);
    XSFLIB_CHECK(softest->changes[0].velocity == 1 && softest->changes[1].velocity == 1);
    for (const std::string text : {"0", "128", "-1", "1.5", "abc", ""})
    {
        const std::optional<KeyEdit> unread = TypedEdit(track, state, InfoValue::kVelocity, text);
        SEQLIB_REQUIRE(unread.has_value());
        XSFLIB_CHECK(unread->changes.empty() &&
                     unread->refused ==
                         "Hokuto can't read \"" + text + "\" as a velocity. A velocity is a number from 1 to 127.");
    }
    XSFLIB_CHECK(!TypedEdit(track, KeyEditorState(), InfoValue::kVelocity, "64").has_value());
}

// The arrow keys move the selected notes up or down, and the Undo History names the move Up or Down. A move past the
// MIDI range gives nothing.
void TestTransposition()
{
    const KeyTrack track = TrackOf({NoteAt(48, 10), NoteAt(96, 11, 64)});
    KeyEditorState state = StateFor({10, 11});
    state.selected = {10, 11};

    const std::optional<KeyEdit> up = Transposition(track, state, 12);
    const std::optional<KeyEdit> down = Transposition(track, state, -1);

    SEQLIB_REQUIRE(up && up->changes.size() == 2 && down && down->changes.size() == 2);
    XSFLIB_CHECK(up->name == "Up" && up->changes[0].transpose == 12 && up->changes[1].transpose == 12);
    XSFLIB_CHECK(down->name == "Down" && down->changes[0].transpose == -1 && down->changes[1].transpose == -1);
    XSFLIB_CHECK(!Transposition(track, state, 64).has_value() && !Transposition(track, state, -61).has_value());
    XSFLIB_CHECK(!Transposition(track, KeyEditorState(), 1).has_value());
}

// The instrument at a tick is the last note's that starts by then, or before the first note, the first note's.
void TestInstrumentAt()
{
    const KeyTrack track = TrackOf({NoteAt(24, 10, 60, 1), NoteAt(96, 11, 62, 2), NoteAt(96, 12, 64, 3)});
    const KeyTrack empty = TrackOf({});
    KeyNote drawn = NoteAt(100, 0, 67);
    drawn.bank_slot = 2;

    const KeyNote* before = InstrumentNoteAt(track, 0.0);
    const KeyNote* first = InstrumentNoteAt(track, 95.5);
    const KeyNote* last = InstrumentNoteAt(track, 96.0);
    const KeyNote given = WithInstrumentAt(track, 50.0, drawn);
    const KeyNote kept = WithInstrumentAt(empty, 50.0, drawn);

    XSFLIB_CHECK(before == &track.notes[0] && first == &track.notes[0] && last == &track.notes[2]);
    XSFLIB_CHECK(InstrumentNoteAt(empty, 0.0) == nullptr);
    XSFLIB_CHECK(given.program == 1 && given.bank_slot == 0 && given.key == 67 && given.start == 100);
    XSFLIB_CHECK(kept.program == 0 && kept.bank_slot == 2);
}

// A gesture that holds a note ends once the track has lost the note. A note being drawn, a box and a press on the
// keyboard hold none: on a track without notes, the pencil draws the first one. A gesture in a lane ends once its lane
// has gone, and a drag of a point once its point has.
void TestGoneGestures()
{
    const KeyTrack empty = TrackOf({});
    const KeyTrack track = TrackOf({NoteAt(0, 10, 60)});

    const auto gesture = [](KeyGesture::Kind kind, std::size_t index)
    {
        KeyGesture made;
        made.kind = kind;
        made.note = index;
        made.lane = index;
        made.control = index;

        return made;
    };

    XSFLIB_CHECK(!GestureGone(gesture(KeyGesture::Kind::kDraw, 0), empty, 0));
    XSFLIB_CHECK(!GestureGone(gesture(KeyGesture::Kind::kBox, 0), empty, 0));
    XSFLIB_CHECK(!GestureGone(gesture(KeyGesture::Kind::kKeyboard, 0), empty, 0));
    XSFLIB_CHECK(!GestureGone(gesture(KeyGesture::Kind::kNone, 0), empty, 0));
    XSFLIB_CHECK(GestureGone(gesture(KeyGesture::Kind::kPress, 0), empty, 0));
    XSFLIB_CHECK(!GestureGone(gesture(KeyGesture::Kind::kMove, 0), track, 0));
    XSFLIB_CHECK(GestureGone(gesture(KeyGesture::Kind::kSize, 1), track, 0));
    XSFLIB_CHECK(GestureGone(gesture(KeyGesture::Kind::kVelocity, 1), track, 0));
    XSFLIB_CHECK(!GestureGone(gesture(KeyGesture::Kind::kLaneDraw, 0), empty, 1));
    XSFLIB_CHECK(GestureGone(gesture(KeyGesture::Kind::kLaneBox, 1), empty, 1));
    XSFLIB_CHECK(GestureGone(gesture(KeyGesture::Kind::kControl, 0), empty, 1));
}

// The selection stays while the sequence stays the same, whatever waits. In a new version, the selection that waits for
// that version on the same track takes its place. A selection that waits for another version or another track leaves
// nothing selected, and so does a new version without one. Two versions with equal commands are still two.
void TestFollowedSelection()
{
    using Sequence = hokuto::format::Shared<ntdlib::commands::Sequence>;
    const Sequence kOldVersion = ntdlib::commands::Sequence{};
    const Sequence kNewVersion = ntdlib::commands::Sequence{};
    const KeySelection kCurrent{kOldVersion, 1, {10, 20}};
    const KeySelection kWaiting{kNewVersion, 1, {12, 22}};

    const std::set<uint32_t> kept = FollowedSelection(kCurrent, kWaiting, kOldVersion);
    const std::set<uint32_t> followed = FollowedSelection(kCurrent, kWaiting, kNewVersion);
    const std::set<uint32_t> other_version =
        FollowedSelection(kCurrent, KeySelection{kOldVersion, 1, {30}}, kNewVersion);
    const std::set<uint32_t> other_track = FollowedSelection(kCurrent, KeySelection{kNewVersion, 2, {12}}, kNewVersion);
    const std::set<uint32_t> none = FollowedSelection(kCurrent, std::nullopt, kNewVersion);

    XSFLIB_CHECK(kept == (std::set<uint32_t>{10, 20}));
    XSFLIB_CHECK(followed == (std::set<uint32_t>{12, 22}));
    XSFLIB_CHECK(other_version.empty() && other_track.empty() && none.empty());
}

// With Acoustic Feedback on, a pressed note sounds, at the key that a move takes it to, and so does a note being drawn.
// A press on a note's end sounds nothing. With it off, only the keyboard sounds. A note that the arrows moved sounds
// while no gesture is under way. The velocity lane, a box and a size sound nothing.
void TestSoundingNote()
{
    const KeyTrack track = TrackOf({NoteAt(0, 10, 60), NoteAt(48, 11, 126)});
    KeyEditorState state;
    state.gesture.note = 1;
    state.gesture.transpose = 3;
    state.gesture.drawn = NoteAt(96, 0, 70);
    state.gesture.played = NoteAt(0, 0, 72);
    state.transposed = NoteAt(0, 10, 61);

    // The note that sounds during a gesture of `kind`.
    const auto sounding = [&track, &state](KeyGesture::Kind kind)
    {
        state.gesture.kind = kind;

        return SoundingNote(track, state);
    };

    const std::optional<KeyNote> moved = sounding(KeyGesture::Kind::kMove);
    const std::optional<KeyNote> drawn = sounding(KeyGesture::Kind::kDraw);
    const std::optional<KeyNote> played = sounding(KeyGesture::Kind::kKeyboard);
    const std::optional<KeyNote> transposed = sounding(KeyGesture::Kind::kNone);
    const bool others_silent = !sounding(KeyGesture::Kind::kVelocity) && !sounding(KeyGesture::Kind::kBox) &&
                               !sounding(KeyGesture::Kind::kSize);
    state.gesture.at_end = true;
    const bool end_silent = !sounding(KeyGesture::Kind::kPress);
    state.gesture.at_end = false;
    state.gesture.transpose = 0;
    const std::optional<KeyNote> pressed = sounding(KeyGesture::Kind::kPress);
    state.feedback = false;
    const bool off_silent = !sounding(KeyGesture::Kind::kPress) && !sounding(KeyGesture::Kind::kMove) &&
                            !sounding(KeyGesture::Kind::kDraw) && !sounding(KeyGesture::Kind::kNone);
    const std::optional<KeyNote> played_off = sounding(KeyGesture::Kind::kKeyboard);

    XSFLIB_CHECK(moved && moved->command == 11 && moved->key == 127);
    XSFLIB_CHECK(drawn && drawn->key == 70);
    XSFLIB_CHECK(played && played->key == 72);
    XSFLIB_CHECK(transposed && transposed->key == 61);
    XSFLIB_CHECK(others_silent && end_silent && off_silent);
    XSFLIB_CHECK(pressed && pressed->command == 11 && pressed->key == 126);
    XSFLIB_CHECK(played_off && played_off->key == 72);
}

// A click on the ruler places the position on the grid's nearest line with the snap on. 1/8 is 24 ticks at 48 ticks to
// a quarter note: tick 20 goes to 24, 35 to 24 and 37 to 48. Without the snap, the position stays at the click. A click
// before the song's start places it at the start.
void TestRulerPosition()
{
    KeyEditorState state;
    state.grid = 8;
    state.snap = true;
    const double near_line = RulerPosition(state, 48, 20.0);
    const double between = RulerPosition(state, 48, 35.0);
    const double later = RulerPosition(state, 48, 37.0);
    const double before = RulerPosition(state, 48, -10.0);
    state.snap = false;

    XSFLIB_CHECK(near_line == 0.5 && between == 0.5 && later == 1.0 && before == 0.0);
    XSFLIB_CHECK(RulerPosition(state, 48, 20.0) == 20.0 / 48.0 && RulerPosition(state, 48, -10.0) == 0.0);
}

} // namespace

int main()
{
    TestQuantizeSelection();
    TestQuantizeEverything();
    TestQuantizeFirstNote();
    TestQuantizeLimits();
    TestQuantizeWithoutLimits();
    TestCopiedNotes();
    TestClipboard();
    TestSelectAll();
    TestKeyNames();
    TestTypedStart();
    TestTypedLength();
    TestTypedPitchAndVelocity();
    TestTransposition();
    TestInstrumentAt();
    TestGoneGestures();
    TestFollowedSelection();
    TestSoundingNote();
    TestRulerPosition();

    return xsflib::testing::Summary("key edits");
}
