// SPDX-License-Identifier: MIT

// The canvas of the arrangement and the parts that its files share (arrangement.cpp, arrangement_tracks.cpp,
// arrangement_markers.cpp and arrangement_tempo.cpp). The names are internal to the arrangement.

#pragma once

#include <imgui.h>
#include <imgui_internal.h>

#include <array>
#include <cstddef>
#include <cstdint>

#include "app/arrangement.h"
#include "app/song_view.h"
#include "app/transport.h"

namespace hokuto::app::arrangement_canvas
{

// The arrangement's sizes, before the display's scale. The marker row lies under the ruler. A drag of the track list's
// edge takes hold in a band kEdgeGrab wide, centred on the edge.
inline constexpr float kEdgeGrab = 6.0f;
inline constexpr float kRulerHeight = 30.0f;
inline constexpr float kMarkerHeight = 22.0f;
inline constexpr float kMinBeatWidth = 2.0f;
inline constexpr float kMaxBeatWidth = 160.0f;
inline constexpr float kZoomStep = 1.25f;
inline constexpr float kWheelStep = 60.0f;

// The heights that a vertical zoom steps through. A track shorter than kTwoLineHeight shows its name without the bank
// and program. A track shorter than kThreeLineHeight shows them on one line under the name, and a taller one on two.
inline constexpr std::array<float, 9> kTrackHeights = {30.0f, 38.0f, 46.0f, 54.0f, 66.0f, 80.0f, 98.0f, 120.0f, 148.0f};
inline constexpr float kTwoLineHeight = 46.0f;
inline constexpr float kThreeLineHeight = 62.0f;

// The marker row's menu, and the most commands that the tooltip of a place where the tracks read variables or branch
// lists.
inline constexpr char kMarkerMenu[] = "##markers";
inline constexpr std::size_t kMostMarkerLines = 12;

// The tempo row's sizes, before the display's scale: the row's height, the room above and below its line, and the
// distance within which a point takes the mouse. A drag changes a point's tempo by the row's span over the line's room
// for each point of travel, and by a tenth of that with Shift.
inline constexpr float kTempoHeight = 56.0f;
inline constexpr float kTempoMargin = 8.0f;
inline constexpr float kTempoGrab = 7.0f;
inline constexpr double kFineTempoDrag = 0.1;
inline constexpr float kTempoMoveDistance = 3.0f;

// The track list's context menu, and the names of its actions in the order of TrackAction.
inline constexpr char kTrackMenu[] = "##tracks";
inline constexpr std::array<const char*, kTrackActions> kTrackActionNames = {
    "Add Track", "Duplicate Tracks", "Remove Selected Tracks", "Copy Track", "Paste Track"};

// Where the arrangement's parts are on the screen, and its scale. The headers, the ruler and the rows under it stay put
// while the lanes scroll under them. `tempo` is empty while the tempo row is hidden.
struct Canvas
{
    ImRect ruler;
    ImRect markers;
    ImRect tempo;
    ImRect headers;
    ImRect lanes;
    float beat_width = 0.0f;
    float row_height = 0.0f;
    float scroll_x = 0.0f;
    float scroll_y = 0.0f;
    int beats_per_bar = 4;
    int ticks_per_quarter = 48;
};

// The x across the lanes of the place `quarters` quarter notes from the song's start.
float XOf(const Canvas& canvas, double quarters);

// The quarter notes from the song's start at `x` across the lanes.
double QuartersAt(const Canvas& canvas, float x);

// The top of the row of track `track` in the lanes.
float RowTop(const Canvas& canvas, std::size_t track);

// Draws the grid over the lanes, by bars or by beats (kShowBars).
void DrawGrid(ImDrawList* draw, const Canvas& canvas);

// The marker row under the ruler. A sequence with a loop shows its intro and its loop there as regions. Flags over the
// regions show the places where the tracks read variables or branch, and the changes of the variables. The edges of a
// timeline's loop take the mouse first, then the flags, and then the regions under them. A change's flag comes before
// the other flags. A drag of an edge of a timeline's loop moves it. A double-click on a region sets the locators round
// it, and Alt with a double-click zooms to it. A double-click on a place where the tracks read variables or branch
// opens its commands in the List Editor.
void DrawMarkers(ImDrawList* draw, const Canvas& canvas, const SongView& song, Transport& transport,
                 ArrangementState& state);

// The marker row's menu. Locate moves the position to a region's start or to a flag. Cycle sets the locators round a
// region, and Zoom fits a region into the lanes.
void DrawMarkerMenu(const SongView& song, Transport& transport, ArrangementState& state);

// The tick where the tempo row's line ends: the song's end, or the last point where a track sets the tempo after it.
int32_t TempoRowEnd(const SongView& song);

// The tempo row under the marker row: the tempo as ntdlib's performer played it, as a line with a step at each change,
// and a point at each command that sets the tempo. A drag of a point up or down changes its tempo in whole beats a
// minute, from 1 up, finer with Shift. A drag to the side moves the command to the nearest beat, or to the nearest tick
// with Shift, as far as the command can move. A double-click on a point opens its command in the List Editor.
void DrawTempo(ImDrawList* draw, const Canvas& canvas, const SongView& song, ArrangementState& state);

// Whether `mouse` is over a point of the tempo row of `song`, inside the row. A point at the song's start is on the
// edge of the track list, and the point receives the press there.
bool OverTempoPoint(const Canvas& canvas, const SongView& song, ImVec2 mouse);

// The tempo row's header: its name, and the tempos at the top and the bottom of the row's line.
void DrawTempoHeader(ImDrawList* draw, const ImRect& header, const SongView& song);

// Draws the tracks' headers and follows their clicks. A right-click selects the track under the mouse and opens the
// list's context menu. While the user drags the track list's edge, or the mouse is over it, `takes_clicks` is false.
void DrawTrackHeaders(ImDrawList* draw, const Canvas& canvas, SongView& song, ArrangementState& state,
                      bool takes_clicks);

// Draws the lanes: the rows of the tracks, the grid, the cycle's range, the loop's lines and the parts. A click on a
// part selects it, and a double-click opens it in the Key Editor.
void DrawLanes(ImDrawList* draw, const Canvas& canvas, SongView& song, const Transport& transport,
               ArrangementState& state);

} // namespace hokuto::app::arrangement_canvas
