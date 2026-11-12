// SPDX-License-Identifier: MIT

// The sizes of the Key Editor, the layout of the canvas, and the parts that key_editor.cpp, key_editor_draw.cpp and
// key_editor_mouse.cpp share. The names are internal to the Key Editor.

#pragma once

#include <imgui.h>
#include <imgui_internal.h>

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/commands/units.h"

#include "app/key_editor.h"
#include "app/transport.h"
#include "edit/commands.h"
#include "song/archive.h"

namespace hokuto::app::key_canvas
{

using ntdlib::commands::Fixed;

// The editor's sizes, before the display's scale. The lanes shrink to keep room for the notes, down to their least
// height.
inline constexpr float kKeyboardWidth = 80.0f;
inline constexpr float kRulerHeight = 30.0f;
inline constexpr float kOverviewHeight = 18.0f;
inline constexpr float kLaneHeight = 88.0f;
inline constexpr float kLeastLaneHeight = 56.0f;
inline constexpr float kLeastNotesHeight = 96.0f;
inline constexpr float kChannelRowHeight = 15.0f; // a row of the lane of channels: the lane doesn't grow or shrink
inline constexpr float kAddSize = 18.0f;
inline constexpr float kEndGrab = 6.0f;
inline constexpr float kDragDistance = 3.0f;
inline constexpr float kLineGrab = 5.0f;
inline constexpr float kMinBeatWidth = 8.0f;
inline constexpr float kMaxBeatWidth = 640.0f;
inline constexpr float kMinKeyHeight = 6.0f;
inline constexpr float kMaxKeyHeight = 30.0f;
inline constexpr float kZoomStep = 1.25f;
inline constexpr float kWheelStep = 60.0f;

// Where the editor's regions are on the screen, and its scale. The ruler and the miniature under it stay at the top,
// and the lanes at the bottom. The notes scroll between them. The keyboard scrolls with the notes, up and down only.
// `lane` is the velocity lane, and `lanes` holds the controller lanes under it, in the order of KeyEditorState::lanes.
// Each lane has its label beside it, under the keyboard. `add` is the button under the labels that adds a lane.
struct Canvas
{
    ImRect ruler;
    ImRect overview;
    ImRect keyboard;
    ImRect notes;
    ImRect lane;
    ImRect lane_label;
    bool has_channels = false; // the lane of channels shows, above the velocity lane
    ImRect channels;
    ImRect channels_label;
    std::vector<ImRect> lanes;
    std::vector<ImRect> lane_labels;
    ImRect add;
    float beat_width = 0.0f;
    float key_height = 0.0f;
    float scroll_x = 0.0f;
    float scroll_y = 0.0f;
    int ticks_per_quarter = 48;
};

// The x of `tick` across the notes.
float XOf(const Canvas& canvas, double tick);

// The tick at `x` across the notes.
double TickAt(const Canvas& canvas, float x);

// The top of the row of `key`. The highest key is at the top.
float RowTop(const Canvas& canvas, int key);

// The key of the row at `y`. The highest key is at the top.
int KeyAt(const Canvas& canvas, float y);

// The rectangle of `note`, moved by `shift` ticks and `transpose` keys, and `resize` ticks longer. A note stays at
// least a tick long, and wide enough to see.
ImRect NoteRect(const Canvas& canvas, const KeyNote& note, int32_t shift, int transpose, int32_t resize);

// The note under `pos`: the last one drawn there. Nothing when there's none.
std::optional<std::size_t> NoteAt(const Canvas& canvas, const KeyTrack& track, ImVec2 pos);

// The note that the driver of the GBA or the DS never started under `pos` (KeyTrack::unstarted), or nothing.
std::optional<std::size_t> UnstartedAt(const Canvas& canvas, const KeyTrack& track, ImVec2 pos);

// The number of the rows of the lane of channels of `console`: the GBA's 4 PSG channels, or the DS's channels 8 to 15.
int ChannelRows(const song::Console& console);

// What the tooltip of a note of `console` that a later note cut short says.
std::string CutText(const song::Console& console, const KeyNote& note);

// What the tooltip of a note of `console` that the driver never started says.
std::string UnstartedText(const song::Console& console, const KeyNote& note);

// The height of a row of the lane of channels.
float ChannelRowHeight();

// The rectangle of a note in the lane of channels, in the row of its channel. A note stays wide enough to see.
ImRect ChannelBarRect(const Canvas& canvas, const ChannelBar& bar);

// The note of the lane of channels under `pos`: the last one drawn there. Nothing when there's none.
std::optional<std::size_t> ChannelBarAt(const Canvas& canvas, const KeyTrack& track, ImVec2 pos);

// What the tooltip of a note in the lane of channels says: its track, its key and its channel, and whether a later
// note cuts it short or the driver never starts it.
std::string ChannelBarText(const song::Console& console, const ChannelBar& bar);

// The note whose line in the velocity lane lies nearest `x`, within reach of the mouse. A selected note comes first.
std::optional<std::size_t> LineAt(const Canvas& canvas, const KeyTrack& track, const KeyEditorState& state, float x);

// The heights of the top and the bottom of the values in `lane`, a controller lane or the velocity lane.
float LaneTop(const ImRect& lane);

// The height of the bottom of the values in `lane`. LaneTop returns the top.
float LaneBottom(const ImRect& lane);

// The height of `value` in a controller lane that spans `range`.
float ValueY(const ImRect& lane, const edit::ValueRange& range, Fixed value);

// The value at height `y` in a controller lane of `setting` that spans `range`: the nearest that the console holds.
Fixed ValueAt(const ImRect& lane, const KeyTrack& track, LaneSetting setting, const edit::ValueRange& range, float y);

// The heights that a point of `control` covers in a controller lane that spans `range`: its value's height, a random
// value's range, or the whole lane for a variable's value.
std::pair<float, float> PointHeights(const ImRect& lane, const edit::ValueRange& range, const KeyControl& control);

// The point of controller lane `lane` under `pos`, by its command's index in `track.controls`: the nearest in time
// within reach of the mouse, and of those at the same time the nearest in height. A selected point comes first. Nothing
// when there's none.
std::optional<std::size_t> PointAt(const Canvas& canvas, const KeyTrack& track, const KeyEditorState& state,
                                   std::size_t lane, ImVec2 pos);

// Zooms the time by `factor`. The time at `anchor_x` stays where it is on the screen.
void ZoomTime(Canvas& canvas, KeyEditorState& state, float factor, float anchor_x);

// Zooms the keys by `factor`. The key at `anchor_y` stays where it is on the screen.
void ZoomKeys(Canvas& canvas, KeyEditorState& state, float factor, float anchor_y);

// The rows of the keys, with a line under each B. Black keys' rows are darker.
void DrawRows(ImDrawList* draw, const Canvas& canvas);

// A line across `area` at each beat, and fainter lines at the grid's steps when they're far enough apart.
void DrawGrid(ImDrawList* draw, const Canvas& canvas, const KeyEditorState& state, const ImRect& area);

// The ruler matches the arrangement's: the range between the locators in its upper half, and the beats numbered from
// 1. A strip in the track's colour runs along its bottom.
void DrawRuler(ImDrawList* draw, const Canvas& canvas, const Transport& transport, ImU32 colour);

// The keyboard down the left side, drawn as a piano's: a white key reaches into the rows of the black keys beside it.
// The keys of the selected notes and the key that sounds light up in the track's colour. Each C has its name. A key
// past the keyboard, such as a key above 127 on DSE or on Konami's driver, lights nothing.
void DrawKeyboard(ImDrawList* draw, const Canvas& canvas, const KeyTrack& track, const KeyEditorState& state);

// The ticks that the miniature spans: the song, and any note that lasts past the song's end.
double OverviewTicks(const KeyTrack& track);

// The miniature of the track under the ruler: every note over the whole song, and a frame round the part of the song in
// view. Selected notes are light. The notes fill the strip's height, from the track's lowest key to its highest.
void DrawOverview(ImDrawList* draw, const Canvas& canvas, const KeyTrack& track, const KeyEditorState& state);

// The notes, with the gesture's changes to the selected ones. The notes of a move leave outlines where they were. A
// size shows each selected command's new length. On the GBA and the DS, the notes that the driver never started show
// under them, and a note that a later note cut short shows a mark at its end.
void DrawNotes(ImDrawList* draw, const Canvas& canvas, const KeyTrack& track, const KeyEditorState& state);

// The velocity lane, on the grid. Each note has a line at its start, and the line's height gives the velocity. A drag
// changes the velocities.
void DrawLane(ImDrawList* draw, const Canvas& canvas, const KeyTrack& track, const KeyEditorState& state);

// The lane of channels, on the grid: a row for each of the console's PSG channels, with every track's notes that hold
// it. The shown track's notes take its colour, and the other tracks' notes a dimmed colour. A note that a later note
// cut short ends in a mark, and a note that the driver never started shows as an outline.
void DrawChannelLane(ImDrawList* draw, const Canvas& canvas, const KeyTrack& track, const KeyEditorState& state);

// The label of the lane of channels: the channels' names beside their rows.
void DrawChannelLabel(ImDrawList* draw, const Canvas& canvas, const song::Console& console);

// The lane's label, beside the lane under the keyboard.
void DrawLaneLabel(ImDrawList* draw, const Canvas& canvas);

// What the tooltip of a point of a controller lane says: the setting and its value, the command's start, the time over
// which the value moves, and whether the command runs only if the last comparison held.
std::string PointText(const KeyTrack& track, const KeyControl& control);

// Controller lane `lane`, on the grid. The line of the setting's values runs through the points, filled below it. The
// value that the console starts a track with is fainter. A value from a random number or a variable shades the lane up
// to the next point, and names its source where the name fits.
void DrawControlLane(ImDrawList* draw, const Canvas& canvas, const KeyTrack& track, const KeyEditorState& state,
                     std::size_t lane);

// The label of controller lane `lane`, beside it under the keyboard: the setting's name and a chevron for its menu, and
// the values at the top and the bottom of the lane.
void DrawControlLabel(ImDrawList* draw, const Canvas& canvas, const KeyTrack& track, const KeyEditorState& state,
                      std::size_t lane, bool hovered);

// The button under the lanes' labels that adds a controller lane.
void DrawAddLane(ImDrawList* draw, const Canvas& canvas, bool hovered);

// The bottom of the lowest lane.
float LanesBottom(const Canvas& canvas);

// Draws the playhead down the ruler, the notes and the lanes, at the transport's position.
void DrawPlayhead(ImDrawList* draw, const Canvas& canvas, const Transport& transport);

// The mouse in the canvas. The wheel scrolls through the keys, and with Shift through time. Command or Ctrl with the
// wheel zooms the time around the mouse, and Option or Alt the keys. A click on the ruler moves the position.
std::optional<KeyEdit> HandleMouse(Canvas& canvas, const KeyTrack& track, KeyEditorState& state, Transport& transport,
                                   bool busy);

} // namespace hokuto::app::key_canvas
