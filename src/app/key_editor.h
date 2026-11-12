// SPDX-License-Identifier: MIT

// The Key Editor: a track's notes on a grid of keys and time. A toolbar holds the tools and the snap, and an info line
// shows the selected note. A keyboard runs down the left side, a ruler across the top with a miniature of the track
// under it, and a lane of velocities along the bottom. Controller lanes under the velocity lane show the track's
// settings, such as its volume or its pan (lanes.h). The editor shows the notes of one track, and scrolls to the part
// that the user opened.
//
// A note leads to the command that played it, and the editor selects notes by their commands. Notes that one command
// plays, such as those of each call of a subroutine, are selected together, and an edit changes them all. Each gesture
// that ends makes one edit. The program turns it into an edit of the commands (src/edit/notes.h).

#pragma once

#include <imgui.h>

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/commands/units.h"

#include "app/transport.h"
#include "app/widgets.h"
#include "edit/notes.h"
#include "song/archive.h"

namespace hokuto::app
{

// A note as the Key Editor shows it, with the instrument that it plays: a slot of the sound's banks and a program
// (song::Note).
struct KeyNote
{
    uint32_t start = 0; // ticks from the song's start
    uint32_t length = 0;
    int key = 60;
    int velocity = 100;
    uint32_t command = 0; // the offset of the note's command (song::Note::command)
    int bank_slot = 0;
    uint32_t program = 0;
    int psg = 0;      // on the GBA and the DS, the PSG channel of the note's voice, or 0 (song::Note::psg)
    bool cut = false; // on the GBA and the DS, a later note took the note's channel while it sounded (song::Note::cut)
    std::optional<int8_t> sample_semitones = {}; // on Konami's driver, a sample's note (song::Note::sample_semitones)
};

// A note of any track of the song on a PSG channel of the console, for the lane of channels in the Key Editor: the
// channel (song::Note::psg), the ticks and the key of the note, the name and the colour of its track, and whether it
// belongs to the shown track, a later note cut it short, or the driver never started it.
struct ChannelBar
{
    int channel = 1;
    uint32_t start = 0;
    uint32_t length = 0;
    int key = 60;
    std::string track;
    ImU32 colour = 0;
    bool shown = false;
    bool cut = false;
    bool unstarted = false;
};

// A command of a setting that the controller lanes show, at a place where the track ran it: the command, its offset in
// the sequence's bytecode, the tick at which it ran, and the stretch of the track's commands that ran it, by its index
// among the track's stretches (song::Track::stretches).
struct KeyControl
{
    ntdlib::commands::Command command;
    uint32_t offset = 0;
    uint32_t tick = 0;
    uint32_t stretch = 0;
};

// The track that the Key Editor shows: its name and colour, its notes in the order that they start, the console of its
// sequence, and the commands of its settings in the order that the track ran them (ControlsOf).
struct KeyTrack
{
    std::string name;
    ImU32 colour = 0;
    int ticks_per_quarter = 48;
    uint32_t song_length = 0; // ticks
    std::vector<KeyNote> notes;
    song::Console console;
    int number = 0; // the track's number in its sequence
    std::vector<KeyControl> controls;

    // The instrument that a note plays on a track without notes. Without a slot, the note keeps its instrument.
    int first_slot = -1;
    uint32_t first_program = 0;

    // On the GBA and the DS: the track's notes that the driver never started (song::Track::unstarted), and every
    // track's notes on the PSG channels, for the lane of channels. The lane shows only where a song plays a PSG voice.
    std::vector<KeyNote> unstarted;
    std::vector<ChannelBar> channels;
};

// A setting that a controller lane shows: a kind of command, and the bus or the LFO that the command addresses.
struct LaneSetting
{
    friend bool operator==(const LaneSetting&, const LaneSetting&) = default;

    ntdlib::commands::Kind kind = ntdlib::commands::Kind::kVolume;
    uint8_t index = 0;
};

// The Key Editor's tools. The keys 1, 8 and 5 choose them.
enum class KeyTool
{
    kSelect,
    kDraw,
    kErase,
};

// The Edit menu's commands that the Key Editor carries out in its next frame.
enum class KeyRequest
{
    kNone,
    kQuantize,
    kCut,
    kCopy,
    kPaste,
    kSelectAll,
};

// What colours the Key Editor's notes: the track's colour, or each note's velocity.
enum class NoteColours
{
    kTrack,
    kVelocity,
};

// A gesture under way in the Key Editor: a press that may become a drag, a drag that moves or sizes notes, a drag in
// the velocity lane, a selection box, a note being drawn, a press on the keyboard, or a press in the miniature of the
// track under the ruler. In a controller lane: a drag of a point, a selection box, or a point being drawn.
struct KeyGesture
{
    enum class Kind
    {
        kNone,
        kPress,
        kMove,
        kSize,
        kVelocity,
        kBox,
        kDraw,
        kKeyboard,
        kOverview,
        kControl,
        kLaneBox,
        kLaneDraw,
    };

    Kind kind = Kind::kNone;
    ImVec2 from;          // where the press was
    std::size_t note = 0; // the note pressed
    bool at_end = false;  // a press on the note's end sizes the note
    int32_t shift = 0;    // ticks
    int transpose = 0;    // semitones
    int32_t resize = 0;   // ticks added to each length
    int velocity = 0;     // the pressed note's new velocity
    KeyNote drawn;        // the note being drawn
    KeyNote played;       // the note that the keyboard plays
    std::string stopped;  // why a move or a size stops short of the mouse, in the words of the drag tooltip

    // In a controller lane: the lane, the point pressed (KeyTrack::controls), the pressed point's new value or the
    // drawn point's value, and the drawn point's tick. A drag of a point moves it by `shift` too.
    std::size_t lane = 0;
    std::size_t control = 0;
    ntdlib::commands::Fixed value = 0;
    uint32_t tick = 0;
};

// The Key Editor's state between frames.
struct KeyEditorState
{
    // A tick and a key to scroll to in the next frame. The window takes the focus with them.
    std::optional<uint32_t> scroll_tick;
    std::optional<int> scroll_key;

    // The scroll in the last frame: the quarter note at the left edge of the notes, and the key at their top edge. The
    // key counts down from 127 by the rows scrolled past: at 120.5, the edge cuts key 121's row in half. A file keeps
    // both (format::EditorView). `scroll_to` asks for a scroll to such a place in the next frame.
    double left_quarter = 0.0;
    float top_key = 0.0f;
    std::optional<std::pair<double, float>> scroll_to;

    // The jumps of the position that the notes follow (PageTurn). The editor keeps its scroll through the jumps while
    // it's hidden.
    JumpWatch jumps;

    // The zoom, in points: the width of a beat and the height of a key. The menu's zoom commands add steps. Zoom Full
    // fits the whole song into the window.
    float beat_width = 64.0f;
    float key_height = 12.0f;
    int zoom_steps = 0;
    int vertical_zoom_steps = 0;
    bool zoom_full = false;

    // A command of the Edit menu that waits for the next frame, and the notes that Cut and Copy took last. The
    // clipboard stays while the editor shows other tracks.
    KeyRequest request = KeyRequest::kNone;
    std::vector<KeyNote> clipboard;

    // The tool, the snap and its grid's note value, from 4 for a quarter note to 64, and the velocity of drawn notes.
    KeyTool tool = KeyTool::kSelect;
    bool snap = true;
    int grid = 16;
    float insert_velocity = 100.0f;

    // What colours the notes, and the key and the position in quarter notes under the mouse in the last frame, for the
    // toolbar's Mouse Note Position.
    NoteColours colours = NoteColours::kTrack;
    std::optional<std::pair<int, double>> pointer;

    // The selected notes, by their commands, what an edit can change in each command of a note or of a controller
    // lane's point, and the field of the info line that the user types in.
    std::set<uint32_t> selected;
    std::map<uint32_t, edit::NoteLimits> limits;
    InfoTyping typing;

    // The text of a field of the info line that the user ended while the program was busy (BusyInfoEntry).
    WaitingValues<std::string> typed_waiting;

    // Whether the editor's window had the keyboard focus in the last frame, and the gesture under way.
    bool focused = false;
    KeyGesture gesture;

    // The controller lanes under the velocity lane, from the top, and the selected commands that they show, by their
    // offsets. A selection of notes clears the lanes' selection, and the other way round. `lane_menu` is the lane whose
    // menu a click on its label opened.
    std::vector<LaneSetting> lanes;
    std::set<uint32_t> selected_controls;
    std::optional<std::size_t> lane_menu;

    // Whether notes sound as the gestures move them: Acoustic Feedback. While an arrow is held, `transposed` is the
    // first selected note at the key that the arrows last moved it to. `sounding` is the note that sounds after this
    // frame, or nothing.
    bool feedback = true;
    std::optional<KeyNote> transposed;
    std::optional<KeyNote> sounding;
};

// What an edit does to the notes of one command.
struct KeyChange
{
    uint32_t command = 0;
    int32_t shift = 0;
    int transpose = 0;
    std::optional<int> velocity;
    std::optional<uint32_t> length; // in ticks
    bool remove = false;
};

// The edit that a gesture or a command makes: its name in the Undo History, the changes to the notes' commands, and new
// notes. An edit in the controller lanes gives new forms of the settings' commands, by their offsets, the ticks that
// commands move by, new commands at their ticks, or the commands that go, by their offsets. The commands move first,
// and then take their new forms. An edit that the commands can't take says why in `refused`, and changes nothing.
struct KeyEdit
{
    std::string name;
    std::vector<KeyChange> changes;
    std::vector<KeyNote> added;
    std::map<uint32_t, ntdlib::commands::Command> changed_controls;
    std::map<uint32_t, int32_t> moved_controls;
    std::vector<std::pair<uint32_t, ntdlib::commands::Command>> added_controls;
    std::set<uint32_t> removed_controls;
    std::string refused;
};

// Draws the Key Editor while `*open` is set. A click on the ruler moves the transport's position, to the grid with the
// snap on (RulerPosition). `busy` holds the gestures off while the program reads the result of the last edit. Returns
// the edit of a gesture that ended, or of a command of the Edit menu (see RequestedEdit). `state.sounding` then gives
// the note that sounds (see SoundingNote). `track_item` is the version, the sound and the track (the first number of
// its place) that `track` comes from. A value typed into the info line while busy waits for its note or point there.
std::optional<KeyEdit> DrawKeyEditor(const KeyTrack& track, KeyEditorState& state, Transport& transport,
                                     const ShownItem& track_item, bool busy, bool* open);

} // namespace hokuto::app
