// SPDX-License-Identifier: MIT

// The arrangement's state between frames, the edits that its info line and its track list ask for, and the beat
// labels of the rulers. DrawArrangement in views.h draws the zone.

#pragma once

#include <imgui.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

#include "app/ruler.h"
#include "app/song_view.h"
#include "app/transport.h"
#include "app/widgets.h"

namespace hokuto::app
{

// The arrangement's zoom on a first run and after Reset Layout, in points. A track of the default height shows its
// name, its bank and its program on three lines.
inline constexpr float kDefaultBeatWidth = 18.0f;
inline constexpr float kDefaultTrackHeight = 66.0f;

// The width of the track list on a first run and after Reset Layout, and the narrowest and widest that a drag of its
// edge makes it, in points.
inline constexpr float kDefaultTrackListWidth = 240.0f;
inline constexpr float kMinTrackListWidth = 160.0f;
inline constexpr float kMaxTrackListWidth = 600.0f;

// A colour that the user picked for a track, from the colour strip of the track's header or the inspector: a colour of
// theme::kTrackPalette, or nothing for the track's automatic colour.
struct TrackColourPick
{
    int track = 0; // the track's index in the song
    std::optional<ImU32> colour;
};

// A value of the selected part that the arrangement's info line takes: a new start, a transposition, or a change of
// velocity.
enum class PartValue
{
    kStart,
    kTranspose,
    kVelocity,
};

// An edit of a part's notes that the arrangement's info line asks for: the ticks that the notes move by, the semitones
// that they move by, or the change of their velocities. `refused` says why the line can't take the typed text, and the
// edit then changes nothing.
struct PartEdit
{
    int track = 0; // the track's index in the song
    int part = 0;
    PartValue value = PartValue::kStart;
    int32_t amount = 0;
    std::string refused;
};

// What the track list's context menu offers, in its order: Add Track, Duplicate Tracks and Remove Selected Tracks, then
// Copy Track and Paste Track. Copy Track keeps the selected track for a paste into another sound.
enum class TrackAction
{
    kAdd,
    kDuplicate,
    kRemove,
    kCopy,
    kPaste,
};

inline constexpr std::size_t kTrackActions = 5;

// A place where a song's tracks read variables or branch: a set of commands, by its index in SongView::logic, and one
// of the set's ticks.
struct MarkerPlace
{
    std::size_t marker = 0;
    int32_t tick = 0;
};

struct ArrangementState
{
    float beat_width = kDefaultBeatWidth;
    float track_height = kDefaultTrackHeight;
    float track_list_width = kDefaultTrackListWidth;
    bool resizing_track_list = false;
    float track_list_grab = 0.0f;
    int selected_track = 0;
    int selected_part_track = -1;
    int selected_part = -1;
    Ruler ruler;

    // The zooms asked for since the last frame: in for each step above 0, and out for each step below. Zoom Full fits
    // the whole song into the lanes.
    int zoom_steps = 0;
    int vertical_zoom_steps = 0;
    bool zoom_full = false;

    // A range to fit into the lanes in the next frame, in quarter notes. Alt with a double-click on a region of the
    // marker row sets it, and so does the row's Zoom menu.
    std::optional<std::pair<double, double>> zoom_range;

    // The scroll in the last frame: the quarter note at the left edge of the lanes, and the track at their top, in
    // tracks from the first. A file keeps them (format::EditorView). `scroll_to` asks for a scroll to such a place in
    // the next frame.
    double left_quarter = 0.0;
    float top_track = 0.0f;
    std::optional<std::pair<double, float>> scroll_to;

    // The jumps of the position that the lanes follow (PageTurn).
    JumpWatch jumps;

    // A double-click on a place where the tracks read variables or branch sets `open_marker`. The program then opens
    // the place's commands in the List Editor, and clears it.
    std::optional<MarkerPlace> open_marker;

    // Whether the tempo row shows under the marker row. Project > Tempo Track shows it and hides it.
    bool tempo_row = false;

    // The drag of a point of the tempo row, while the mouse holds the point. Its tempo `to` follows the mouse,
    // unrounded, and so does its shift. A drag that ends at another whole tempo or another tick sets `tempo_move`. The
    // program then gives the point's command the tempo and the tick, and clears it. A double-click on a point sets
    // `open_tempo`. The program then opens the point's command in the List Editor, and clears it.
    std::optional<TempoMove> tempo_drag;
    std::optional<TempoMove> tempo_move;
    std::optional<TempoPointView> open_tempo;

    // The drag of an edge of a timeline's loop in the marker row, while the mouse holds the edge. The edge follows the
    // mouse on the beats, or on the ticks with Shift. A drag that ends at another tick sets `loop_move`. The program
    // then moves the loop, and clears it.
    std::optional<LoopMove> loop_drag;
    std::optional<LoopMove> loop_move;

    // A double-click on a part sets `open_editor`. The program then opens the part in the Key Editor, and clears it. A
    // double-click on a track's lane away from its parts sets it too, with `open_beat` at the beat under the mouse. The
    // program then opens the track there. A track without notes has no parts.
    bool open_editor = false;
    std::optional<double> open_beat;

    // While the tracks' colours are Custom, a click on a track's colour strip, or on its colour in the inspector,
    // offers the palette. A pick sets `colour_pick`. The program then gives the track the colour, and clears it.
    bool custom_colours = false;
    int colour_popup_track = -1;
    std::optional<TrackColourPick> colour_pick;

    // A right-click on the track list selects the track under the mouse and opens the list's context menu. The program
    // sets which of the menu's actions can run, in the order of TrackAction. A choice sets `track_action`. The program
    // then runs it, and clears it.
    std::array<bool, kTrackActions> track_actions = {};
    std::optional<TrackAction> track_action;

    // The text of the search field of the inspector's menus of a track's bank and program.
    std::array<char, 128> instrument_filter{};

    // A value typed in the arrangement's info line sets `part_edit`. The program then edits the selected part's notes,
    // and clears it.
    InfoTyping part_typing;
    std::optional<PartEdit> part_edit;

    // The text of a field of the info line that the user ended while the program was busy (BusyInfoEntry).
    WaitingValues<std::string> part_waiting;

    // The lanes' hint while no sequence shows: the way to one that the file offers. The program sets it.
    std::string empty_hint;
};

// The number of beats from one label on a ruler to the next while Hokuto counts beats, for beats `beat_width` pixels
// apart: 1, 2 or 5 times a power of 10. The labels then stay apart.
int BeatStep(float beat_width);

// Whether a ruler labels the beat at `quarter`: the first beat, and every `step` beats, counting from 1.
bool LabelledBeat(int quarter, int step);

} // namespace hokuto::app
