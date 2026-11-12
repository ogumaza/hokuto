// SPDX-License-Identifier: MIT

// The arrangement: the ruler, the grid, the canvas with its zooms and the mouse, the playhead, and the info line
// of the selected part. The beat labels of the rulers are here too.

#include "app/arrangement.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "app/arrangement_canvas.h"
#include "app/ruler.h"
#include "app/song_view.h"
#include "app/theme.h"
#include "app/time_text.h"
#include "app/transport.h"
#include "app/views.h"
#include "app/widgets.h"
#include "app/zone_parts.h"

namespace hokuto::app
{
namespace
{

using arrangement_canvas::Canvas;
using arrangement_canvas::DrawGrid;
using arrangement_canvas::DrawLanes;
using arrangement_canvas::DrawMarkerMenu;
using arrangement_canvas::DrawMarkers;
using arrangement_canvas::DrawTempo;
using arrangement_canvas::DrawTempoHeader;
using arrangement_canvas::DrawTrackHeaders;
using arrangement_canvas::kEdgeGrab;
using arrangement_canvas::kMarkerHeight;
using arrangement_canvas::kMarkerMenu;
using arrangement_canvas::kMaxBeatWidth;
using arrangement_canvas::kMinBeatWidth;
using arrangement_canvas::kRulerHeight;
using arrangement_canvas::kTempoHeight;
using arrangement_canvas::kTrackActionNames;
using arrangement_canvas::kTrackHeights;
using arrangement_canvas::kTrackMenu;
using arrangement_canvas::kWheelStep;
using arrangement_canvas::kZoomStep;
using arrangement_canvas::OverTempoPoint;
using arrangement_canvas::QuartersAt;
using arrangement_canvas::TempoRowEnd;
using arrangement_canvas::XOf;
using theme::Px;
using zone_parts::BeginZone;
using zone_parts::DrawPalette;
using zone_parts::kColourPopup;

// The length of the empty song that the arrangement shows with no song open, in bars.
constexpr int kEmptyBars = 64;

} // namespace

namespace arrangement_canvas
{

float XOf(const Canvas& canvas, double quarters)
{
    return canvas.lanes.Min.x + static_cast<float>(quarters) * canvas.beat_width - canvas.scroll_x;
}

double QuartersAt(const Canvas& canvas, float x)
{
    return (x - canvas.lanes.Min.x + canvas.scroll_x) / canvas.beat_width;
}

float RowTop(const Canvas& canvas, std::size_t track)
{
    return canvas.lanes.Min.y + static_cast<float>(track) * canvas.row_height - canvas.scroll_y;
}

} // namespace arrangement_canvas

namespace
{

// The height in kTrackHeights that lies `steps` steps above `height`, or below it for a negative `steps`.
float StepTrackHeight(float height, int steps)
{
    std::size_t nearest = 0;
    for (std::size_t i = 0; i < kTrackHeights.size(); i++)
    {
        if (std::abs(kTrackHeights[i] - height) < std::abs(kTrackHeights[nearest] - height))
        {
            nearest = i;
        }
    }

    const int index = std::clamp(static_cast<int>(nearest) + steps, 0, static_cast<int>(kTrackHeights.size()) - 1);
    return kTrackHeights[static_cast<std::size_t>(index)];
}

// The number of bars from one label on the ruler to the next. The labels then stay apart.
int BarStep(const Canvas& canvas)
{
    const float bar_width = canvas.beat_width * static_cast<float>(canvas.beats_per_bar);
    int step = 1;
    while (bar_width * static_cast<float>(step) < Px(44.0f))
    {
        step *= 2;
    }

    return step;
}

// Zooms in or out by `factor`. The time at `anchor_x` stays where it is on the screen.
void Zoom(Canvas& canvas, ArrangementState& state, float factor, float anchor_x)
{
    const double anchor = QuartersAt(canvas, anchor_x);
    state.beat_width = std::clamp(state.beat_width * factor, kMinBeatWidth, kMaxBeatWidth);
    canvas.beat_width = Px(state.beat_width);
    canvas.scroll_x = std::max(static_cast<float>(anchor) * canvas.beat_width - (anchor_x - canvas.lanes.Min.x), 0.0f);
    ImGui::SetScrollX(canvas.scroll_x);
}

// The mouse in the arrangement. The wheel scrolls. With Shift, the wheel scrolls sideways. Command or Ctrl with the
// wheel zooms around the mouse. The ruler moves the position and sets the cycle.
void HandleMouse(Canvas& canvas, const SongView* song, Transport& transport, ArrangementState& state)
{
    const ImGuiIO& io = ImGui::GetIO();
    const bool hovered = ImGui::IsWindowHovered();
    if (hovered && io.KeyCtrl && io.MouseWheel != 0.0f)
    {
        Zoom(canvas, state, std::pow(kZoomStep, io.MouseWheel), io.MousePos.x);
    }
    else if (hovered && (io.MouseWheel != 0.0f || io.MouseWheelH != 0.0f))
    {
        // macOS turns the wheel with Shift into the horizontal wheel. Other systems leave it vertical.
        const float sideways = io.KeyShift ? io.MouseWheel + io.MouseWheelH : io.MouseWheelH;
        const float down = io.KeyShift ? 0.0f : io.MouseWheel;
        ImGui::SetScrollX(canvas.scroll_x - sideways * Px(kWheelStep));
        ImGui::SetScrollY(canvas.scroll_y - down * Px(kWheelStep));
    }

    // The ruler, with the song's loop for the locators to snap to. The mouse shows where a press grabs a locator.
    RulerGeometry geometry;
    geometry.origin = canvas.lanes.Min.x - canvas.scroll_x;
    geometry.beat_width = canvas.beat_width;
    geometry.middle = canvas.ruler.GetCenter().y;
    geometry.grab = Px(kLocatorGrab);
    geometry.snap = Px(kLocatorSnap);
    geometry.drag = Px(kRangeDragDistance);
    if (song != nullptr && song->loop_start >= 0)
    {
        geometry.loop_start = static_cast<double>(song->loop_start) / song->ticks_per_quarter;
        geometry.loop_end = static_cast<double>(song->loop_end) / song->ticks_per_quarter;
    }
    RulerMouse mouse;
    mouse.x = io.MousePos.x;
    mouse.y = io.MousePos.y;
    mouse.over = hovered && canvas.ruler.Contains(io.MousePos);
    mouse.pressed = mouse.over && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !state.resizing_track_list;
    mouse.down = ImGui::IsMouseDown(ImGuiMouseButton_Left);
    mouse.command = io.KeyCtrl;
    mouse.option = io.KeyAlt;
    if (state.ruler.Update(geometry, mouse, transport))
    {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    }
}

// A line at each bar, and at each beat when the beats are far enough apart. Zoomed far out, bars thin out too.
void DrawBarGrid(ImDrawList* draw, const Canvas& canvas, int first, int last)
{
    const bool beats = canvas.beat_width >= Px(9.0f);
    const int bar_step =
        canvas.beat_width * static_cast<float>(canvas.beats_per_bar) >= Px(10.0f) ? 1 : BarStep(canvas);
    for (int quarter = first; quarter <= last; quarter++)
    {
        const bool bar = quarter % canvas.beats_per_bar == 0;
        if (bar ? quarter / canvas.beats_per_bar % bar_step != 0 : !beats)
        {
            continue;
        }

        const float x = std::floor(XOf(canvas, quarter));
        draw->AddRectFilled(ImVec2(x, canvas.lanes.Min.y), ImVec2(x + Px(1.0f), canvas.lanes.Max.y),
                            bar ? theme::kGridBar : theme::kGridBeat);
    }
}

// A line at each beat, or at each labelled beat when the beats are close together. No line stands out. Nothing says
// where a bar starts.
void DrawBeatGrid(ImDrawList* draw, const Canvas& canvas, int first, int last)
{
    const bool every_beat = canvas.beat_width >= Px(9.0f);
    const int step = BeatStep(canvas.beat_width);
    for (int quarter = first; quarter <= last; quarter++)
    {
        if (!every_beat && !LabelledBeat(quarter, step))
        {
            continue;
        }

        const float x = std::floor(XOf(canvas, quarter));
        draw->AddRectFilled(ImVec2(x, canvas.lanes.Min.y), ImVec2(x + Px(1.0f), canvas.lanes.Max.y),
                            theme::kGridBeatOnly);
    }
}

} // namespace

namespace arrangement_canvas
{

void DrawGrid(ImDrawList* draw, const Canvas& canvas)
{
    const int first = std::max(static_cast<int>(std::floor(QuartersAt(canvas, canvas.lanes.Min.x))), 0);
    const int last = static_cast<int>(std::ceil(QuartersAt(canvas, canvas.lanes.Max.x)));
    if (kShowBars)
    {
        DrawBarGrid(draw, canvas, first, last);
    }
    else
    {
        DrawBeatGrid(draw, canvas, first, last);
    }
}

} // namespace arrangement_canvas

namespace
{

// A long mark on the ruler at `x`, with `number` beside it.
void DrawRulerLabel(ImDrawList* draw, const ImRect& ruler, float x, int number)
{
    std::array<char, 16> label{};
    std::snprintf(label.data(), label.size(), "%d", number);
    draw->AddRectFilled(ImVec2(x, ruler.Max.y - Px(13.0f)), ImVec2(x + Px(1.0f), ruler.Max.y), theme::kTextDim);
    DrawText(draw, theme::GetFonts().figures, theme::kSmallSize, ImVec2(x + Px(4.0f), ruler.Max.y - Px(16.0f)),
             theme::kTextMuted, label.data());
}

// A short mark on the ruler at `x`, `height` points tall.
void DrawRulerMark(ImDrawList* draw, const ImRect& ruler, float x, float height)
{
    draw->AddRectFilled(ImVec2(x, ruler.Max.y - Px(height)), ImVec2(x + Px(1.0f), ruler.Max.y), theme::kBorderStrong);
}

// A number and a long mark at each labelled bar, and short marks at the beats.
void DrawBarMarks(ImDrawList* draw, const Canvas& canvas)
{
    const ImRect& ruler = canvas.ruler;
    const int beats = canvas.beats_per_bar;
    const int step = BarStep(canvas);
    const int first = std::max(static_cast<int>(std::floor(QuartersAt(canvas, ruler.Min.x) / beats)) - 1, 0);
    const int last = static_cast<int>(std::ceil(QuartersAt(canvas, ruler.Max.x) / beats));
    for (int bar = first; bar <= last; bar++)
    {
        const float x = std::floor(XOf(canvas, static_cast<double>(bar) * beats));
        if (bar % step == 0)
        {
            DrawRulerLabel(draw, ruler, x, bar + 1);
        }
        else
        {
            DrawRulerMark(draw, ruler, x, 6.0f);
        }
        for (int beat = 1; beat < beats && canvas.beat_width >= Px(9.0f); beat++)
        {
            DrawRulerMark(draw, ruler, std::floor(XOf(canvas, static_cast<double>(bar) * beats + beat)), 4.0f);
        }
    }
}

// A number and a long mark at each labelled beat, and short marks at the others when they're far enough apart.
void DrawBeatMarks(ImDrawList* draw, const Canvas& canvas)
{
    const ImRect& ruler = canvas.ruler;
    const int step = BeatStep(canvas.beat_width);
    const int first = std::max(static_cast<int>(std::floor(QuartersAt(canvas, ruler.Min.x))) - 1, 0);
    const int last = static_cast<int>(std::ceil(QuartersAt(canvas, ruler.Max.x)));
    for (int quarter = first; quarter <= last; quarter++)
    {
        const float x = std::floor(XOf(canvas, quarter));
        if (LabelledBeat(quarter, step))
        {
            DrawRulerLabel(draw, ruler, x, quarter + 1);
        }
        else if (canvas.beat_width >= Px(4.0f))
        {
            DrawRulerMark(draw, ruler, x, 5.0f);
        }
    }
}

void DrawRuler(ImDrawList* draw, const Canvas& canvas, const Transport& transport)
{
    const ImRect& ruler = canvas.ruler;
    draw->AddRectFilled(ruler.Min, ruler.Max, theme::kRaised);

    // The range between the locators, in the ruler's upper half. The range lights up while the cycle is on.
    const ImU32 range =
        transport.Cycle() ? theme::WithAlpha(theme::kCycle, 0.9f) : theme::WithAlpha(theme::kTextDim, 0.4f);
    draw->AddRectFilled(ImVec2(XOf(canvas, transport.CycleStart()), ruler.Min.y + Px(3.0f)),
                        ImVec2(XOf(canvas, transport.CycleEnd()), ruler.Min.y + Px(11.0f)), range, Px(3.0f));

    if (kShowBars)
    {
        DrawBarMarks(draw, canvas);
    }
    else
    {
        DrawBeatMarks(draw, canvas);
    }
    draw->AddRectFilled(ImVec2(ruler.Min.x, ruler.Max.y - Px(1.0f)), ruler.Max, theme::kBorder);
}

void DrawPlayhead(ImDrawList* draw, const Canvas& canvas, const Transport& transport)
{
    const float x = std::floor(XOf(canvas, transport.Position()));
    draw->PushClipRect(ImVec2(canvas.lanes.Min.x - Px(7.0f), canvas.ruler.Min.y), canvas.lanes.Max, true);
    draw->AddRectFilled(ImVec2(x, canvas.ruler.Max.y - Px(2.0f)), ImVec2(x + Px(1.0f), canvas.lanes.Max.y),
                        theme::WithAlpha(theme::kText, 0.85f));
    draw->AddTriangleFilled(ImVec2(x - Px(5.5f), canvas.ruler.Max.y - Px(11.0f)),
                            ImVec2(x + Px(6.5f), canvas.ruler.Max.y - Px(11.0f)),
                            ImVec2(x + Px(0.5f), canvas.ruler.Max.y - Px(2.0f)), theme::kText);
    draw->PopClipRect();
}

// Sets the rectangles of the ruler, the marker row, the tempo row, the headers and the lanes in the canvas's `inner`
// rectangle. The rows above the lanes end at `top`.
void PlaceCanvasRects(Canvas& canvas, const ImRect& inner, float header_width, float ruler_height, float marker_height,
                      float top)
{
    canvas.ruler =
        ImRect(ImVec2(inner.Min.x + header_width, inner.Min.y), ImVec2(inner.Max.x, inner.Min.y + ruler_height));
    canvas.markers = ImRect(ImVec2(inner.Min.x + header_width, inner.Min.y + ruler_height),
                            ImVec2(inner.Max.x, inner.Min.y + ruler_height + marker_height));
    canvas.tempo =
        ImRect(ImVec2(inner.Min.x + header_width, canvas.markers.Max.y), ImVec2(inner.Max.x, inner.Min.y + top));
    canvas.headers = ImRect(ImVec2(inner.Min.x, inner.Min.y + top), ImVec2(inner.Min.x + header_width, inner.Max.y));
    canvas.lanes = ImRect(ImVec2(inner.Min.x + header_width, inner.Min.y + top), inner.Max);
}

// Handles a drag of the track list's edge in the canvas's `inner` rectangle, with the list `header_width` wide. Returns
// whether the pointer is over the edge. With `to_point`, the pointer is over a point of the tempo row, and the edge
// leaves the press to the point.
bool HandleTrackListEdge(const ImRect& inner, float header_width, bool to_point, ArrangementState& state)
{
    // A drag of the track list's edge sets the list's width. The band lies across the line at the list's right end, the
    // list's last pixel. A press in the band goes to the edge, not to the ruler, the rows or the lanes. The edge keeps
    // the press's distance from the line. A press alone then doesn't move it. A tempo point at the song's start is on
    // the line. Its half inside the row receives the press.
    const ImGuiIO& io = ImGui::GetIO();
    const float line = inner.Min.x + header_width - Px(0.5f);
    const ImRect edge(ImVec2(line - Px(kEdgeGrab) * 0.5f, inner.Min.y),
                      ImVec2(line + Px(kEdgeGrab) * 0.5f, inner.Max.y));
    const bool over_edge = ImGui::IsWindowHovered() && edge.Contains(io.MousePos) && !to_point;
    if (over_edge && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
    {
        state.resizing_track_list = true;
        state.track_list_grab = io.MousePos.x - (inner.Min.x + header_width);
    }
    if (state.resizing_track_list && !ImGui::IsMouseDown(ImGuiMouseButton_Left))
    {
        state.resizing_track_list = false;
    }
    if (state.resizing_track_list)
    {
        state.track_list_width = std::clamp((io.MousePos.x - state.track_list_grab - inner.Min.x) / Px(1.0f),
                                            kMinTrackListWidth, kMaxTrackListWidth);
    }
    if (over_edge || state.resizing_track_list)
    {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    }

    return over_edge;
}

// Applies the zooms that the keyboard and the menus asked for to `canvas`: a zoom by steps, Zoom Full over the song's
// `length` in quarters, and a zoom to a range.
void ApplyZooms(Canvas& canvas, const Transport& transport, ArrangementState& state, double length)
{
    // A zoom from the keyboard keeps the playhead in place. With the playhead out of sight, the middle stays in place.
    if (state.zoom_steps != 0)
    {
        const float playhead = XOf(canvas, transport.Position());
        const bool visible = playhead >= canvas.lanes.Min.x && playhead <= canvas.lanes.Max.x;
        Zoom(canvas, state, std::pow(kZoomStep, static_cast<float>(state.zoom_steps)),
             visible ? playhead : (canvas.lanes.Min.x + canvas.lanes.Max.x) * 0.5f);
        state.zoom_steps = 0;
    }

    // Zoom Full shows the song from its start to its end.
    if (state.zoom_full)
    {
        const float width = canvas.lanes.GetWidth() / Px(1.0f);
        if (length > 0.0 && width > 0.0f)
        {
            state.beat_width = std::clamp(width / static_cast<float>(length), kMinBeatWidth, kMaxBeatWidth);
            canvas.beat_width = Px(state.beat_width);
        }
        canvas.scroll_x = 0.0f;
        ImGui::SetScrollX(0.0f);
        state.zoom_full = false;
    }

    // A zoom to a range fits the range into the lanes.
    if (state.zoom_range)
    {
        const auto [start, end] = *state.zoom_range;
        const float width = canvas.lanes.GetWidth() / Px(1.0f);
        if (end > start && width > 0.0f)
        {
            state.beat_width = std::clamp(width / static_cast<float>(end - start), kMinBeatWidth, kMaxBeatWidth);
            canvas.beat_width = Px(state.beat_width);
        }
        canvas.scroll_x = std::max(static_cast<float>(start) * canvas.beat_width, 0.0f);
        ImGui::SetScrollX(canvas.scroll_x);
        state.zoom_range.reset();
    }
}

// The popups of the track list: the palette of a track's colour strip and the list's context menu.
void DrawTrackPopups(SongView* song, ArrangementState& state)
{
    // The palette that a click on a track's colour strip offers.
    if (ImGui::BeginPopup(kColourPopup))
    {
        const auto track = static_cast<std::size_t>(state.colour_popup_track);
        if (song != nullptr && state.colour_popup_track >= 0 && track < song->tracks.size())
        {
            if (std::optional<TrackColourPick> pick = DrawPalette(state.colour_popup_track, song->tracks[track].colour))
            {
                state.colour_pick = pick;
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndPopup();
    }

    // The track list's context menu. A line divides Copy Track and Paste Track from the other actions.
    if (ImGui::BeginPopup(kTrackMenu))
    {
        for (std::size_t i = 0; i < kTrackActions; i++)
        {
            if (i == static_cast<std::size_t>(TrackAction::kCopy))
            {
                ImGui::Separator();
            }

            if (ImGui::MenuItem(kTrackActionNames[i], nullptr, false, state.track_actions[i]))
            {
                state.track_action = static_cast<TrackAction>(i);
            }
        }
        ImGui::EndPopup();
    }
}

// The corner above the headers, with the number of tracks.
void DrawCorner(ImDrawList* draw, const ImRect& inner, float header_width, float ruler_height, std::size_t tracks)
{
    const ImRect corner(inner.Min, ImVec2(inner.Min.x + header_width, inner.Min.y + ruler_height));
    draw->AddRectFilled(corner.Min, corner.Max, theme::kRaised);
    draw->AddRectFilled(ImVec2(corner.Min.x, corner.Max.y - Px(1.0f)), corner.Max, theme::kBorder);
    draw->AddRectFilled(ImVec2(corner.Max.x - Px(1.0f), corner.Min.y), corner.Max, theme::kBorder);
    std::array<char, 32> count{};
    if (tracks == 0)
    {
        std::snprintf(count.data(), count.size(), "No tracks");
    }
    else
    {
        std::snprintf(count.data(), count.size(), tracks == 1 ? "%zu track" : "%zu tracks", tracks);
    }
    // The count sits on the baseline of the ruler's numbers.
    DrawText(draw, theme::GetFonts().text, theme::kSmallSize,
             ImVec2(corner.Min.x + Px(14.0f), corner.Max.y - Px(16.0f)), theme::kTextMuted, count.data());
}

// The marker row's header, under the corner, with the marker row's menu.
void DrawMarkerHeader(ImDrawList* draw, const ImRect& inner, const Canvas& canvas, float header_width, SongView& song,
                      Transport& transport, ArrangementState& state)
{
    const ImRect header(ImVec2(inner.Min.x, canvas.markers.Min.y),
                        ImVec2(inner.Min.x + header_width, canvas.markers.Max.y));
    draw->AddRectFilled(header.Min, header.Max, theme::kRaised);
    draw->AddRectFilled(ImVec2(header.Min.x, header.Max.y - Px(1.0f)), header.Max, theme::kBorder);
    draw->AddRectFilled(ImVec2(header.Max.x - Px(1.0f), header.Min.y), header.Max, theme::kBorder);
    const ImVec2 caps = CapsSize("MARKERS");
    DrawCaps(draw, ImVec2(header.Min.x + Px(14.0f), header.GetCenter().y - caps.y * 0.5f), theme::kTextMuted,
             "MARKERS");

    // A click on the header opens the marker row's menu. A chevron at the header's end shows that it opens. The
    // track list's edge stays free for a drag.
    ImGui::SetCursorScreenPos(header.Min);
    ImGui::InvisibleButton("##marker header", ImVec2(header.GetWidth() - Px(kEdgeGrab) * 0.5f, header.GetHeight()));
    const bool hovered = ImGui::IsItemHovered();
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
    {
        ImGui::OpenPopup(kMarkerMenu);
    }
    const float chevron = Px(10.0f);
    DrawIcon(draw, Icon::kChevronDown, ImVec2(header.Max.x - Px(12.0f) - chevron * 0.5f, header.GetCenter().y), chevron,
             hovered ? theme::kText : theme::kTextMuted);
    if (hovered && !ImGui::IsPopupOpen(kMarkerMenu))
    {
        WrappedTooltip("Locate a marker, set the locators around a region, or zoom to it");
    }
    DrawMarkerMenu(song, transport, state);
}

// Turns the page of `canvas` when the playhead leaves it, unless the program set the scroll in this frame
// (`scrolled`).
void TurnPage(const Canvas& canvas, const Transport& transport, ArrangementState& state, bool scrolled)
{
    const bool jumped = state.jumps.Jumped(transport) && !state.ruler.MovingPosition() && !scrolled;
    const PageView page{canvas.scroll_x, canvas.lanes.GetWidth(), Px(16.0f), Px(32.0f)};
    const float playhead = static_cast<float>(transport.Position()) * canvas.beat_width;
    if (const std::optional<float> turned = PageTurn(page, playhead, transport.Playing(), jumped))
    {
        ImGui::SetScrollX(*turned);
    }
}

// Scrolls the canvas where the program asked (`state.scroll_to`), and keeps the canvas scroll in `state`. Returns
// whether the program asked for a scroll.
bool ApplyScrollTo(Canvas& canvas, ArrangementState& state)
{
    // A scroll that the program sets, such as the one that a file keeps.
    const std::optional<std::pair<double, float>> scroll_to = std::exchange(state.scroll_to, std::nullopt);
    if (scroll_to)
    {
        canvas.scroll_x = std::max(static_cast<float>(scroll_to->first) * canvas.beat_width, 0.0f);
        canvas.scroll_y = std::max(scroll_to->second * canvas.row_height, 0.0f);
        ImGui::SetScrollX(canvas.scroll_x);
        ImGui::SetScrollY(canvas.scroll_y);
    }
    state.left_quarter = canvas.scroll_x / canvas.beat_width;
    state.top_track = canvas.scroll_y / canvas.row_height;

    return scroll_to.has_value();
}

// Draws the rows of the canvas of `song`, or of an empty song without one: the lanes, the headers of the tracks, the
// ruler, the marker row and the `tempo_row`, and the playhead. The headers take clicks while `headers_take_clicks`.
void DrawCanvasLayers(ImDrawList* draw, const Canvas& canvas, SongView* song, Transport& transport,
                      ArrangementState& state, bool headers_take_clicks, bool tempo_row)
{
    // The lanes, then the headers and the ruler over them, then the playhead over everything.
    draw->PushClipRect(canvas.lanes.Min, canvas.lanes.Max, true);
    if (song != nullptr)
    {
        DrawLanes(draw, canvas, *song, transport, state);
    }
    else
    {
        DrawGrid(draw, canvas);
    }
    draw->PopClipRect();
    draw->PushClipRect(canvas.headers.Min, canvas.headers.Max, true);
    draw->AddRectFilled(canvas.headers.Min, canvas.headers.Max, theme::kPanel);
    if (song != nullptr)
    {
        DrawTrackHeaders(draw, canvas, *song, state, headers_take_clicks && !state.resizing_track_list);
    }
    draw->PopClipRect();

    DrawTrackPopups(song, state);
    draw->PushClipRect(canvas.ruler.Min, canvas.ruler.Max, true);
    DrawRuler(draw, canvas, transport);
    draw->PopClipRect();
    if (song != nullptr)
    {
        draw->PushClipRect(canvas.markers.Min, canvas.markers.Max, true);
        DrawMarkers(draw, canvas, *song, transport, state);
        draw->PopClipRect();
    }
    if (tempo_row)
    {
        draw->PushClipRect(canvas.tempo.Min, canvas.tempo.Max, true);
        DrawTempo(draw, canvas, *song, state);
        draw->PopClipRect();
    }
    DrawPlayhead(draw, canvas, transport);
}

void DrawCanvas(SongView* song, Transport& transport, ArrangementState& state)
{
    // A width or a height from a file takes the nearest that the arrangement offers.
    state.beat_width = std::clamp(state.beat_width, kMinBeatWidth, kMaxBeatWidth);
    state.track_height = StepTrackHeight(state.track_height, 0);

    // A vertical zoom from the keyboard changes the height of every track.
    const float old_height = state.track_height;
    if (state.vertical_zoom_steps != 0)
    {
        state.track_height = StepTrackHeight(state.track_height, state.vertical_zoom_steps);
        state.vertical_zoom_steps = 0;
    }

    // The song's scale, or the scale of an empty song.
    Canvas canvas;
    canvas.beats_per_bar = song != nullptr ? song->beats_per_bar : transport.BeatsPerBar();
    canvas.ticks_per_quarter = song != nullptr ? song->ticks_per_quarter : 48;
    canvas.beat_width = Px(state.beat_width);
    canvas.row_height = Px(state.track_height);
    // With the tempo row, the song runs on to the row's last point.
    const bool tempo_row = song != nullptr && state.tempo_row;
    const double length =
        song != nullptr ? static_cast<double>(tempo_row ? TempoRowEnd(*song) : song->length) / song->ticks_per_quarter
                        : static_cast<double>(kEmptyBars) * canvas.beats_per_bar;
    const std::size_t tracks = song != nullptr ? song->tracks.size() : 0;

    // The content: the headers and the song across, and the ruler, the rows under it and the tracks down. Some room
    // follows the song's end. A hidden tempo row ends its drag.
    const float header_width = Px(state.track_list_width);
    const float ruler_height = Px(kRulerHeight);
    const float marker_height = song != nullptr ? Px(kMarkerHeight) : 0.0f;
    const float tempo_height = tempo_row ? Px(kTempoHeight) : 0.0f;
    const float top = ruler_height + marker_height + tempo_height;
    if (!tempo_row)
    {
        state.tempo_drag.reset();
    }
    ImGui::SetNextWindowContentSize(
        ImVec2(header_width + static_cast<float>(length + canvas.beats_per_bar * 8.0) * canvas.beat_width,
               top + static_cast<float>(tracks) * canvas.row_height + Px(80.0f)));
    ImGui::BeginChild("##canvas", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None,
                      ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    const ImRect inner = ImGui::GetCurrentWindow()->InnerRect;
    canvas.scroll_x = ImGui::GetScrollX();
    canvas.scroll_y = ImGui::GetScrollY();
    PlaceCanvasRects(canvas, inner, header_width, ruler_height, marker_height, top);

    const bool to_point = tempo_row && OverTempoPoint(canvas, *song, ImGui::GetIO().MousePos);
    const bool over_edge = HandleTrackListEdge(inner, header_width, to_point, state);

    // The track at the top of the lanes stays there through a vertical zoom.
    if (state.track_height != old_height)
    {
        canvas.scroll_y *= state.track_height / old_height;
        ImGui::SetScrollY(canvas.scroll_y);
    }

    HandleMouse(canvas, song, transport, state);
    ApplyZooms(canvas, transport, state, length);

    const bool scrolled = ApplyScrollTo(canvas, state);

    ImDrawList* draw = ImGui::GetWindowDrawList();
    DrawCanvasLayers(draw, canvas, song, transport, state, !over_edge, tempo_row);

    // The corner above the headers.
    DrawCorner(draw, inner, header_width, ruler_height, tracks);

    // The marker row's header, under the corner.
    if (song != nullptr)
    {
        DrawMarkerHeader(draw, inner, canvas, header_width, *song, transport, state);
    }

    // The tempo row's header, under the marker row's.
    if (tempo_row)
    {
        DrawTempoHeader(
            draw,
            ImRect(ImVec2(inner.Min.x, canvas.tempo.Min.y), ImVec2(inner.Min.x + header_width, canvas.tempo.Max.y)),
            *song);
    }

    if (song == nullptr)
    {
        EmptyState(draw, canvas.lanes.Min, canvas.lanes.Max, "No sequence is open", state.empty_hint);
    }

    // Playback that runs off the right edge turns the page. So does a jump of the position out of the lanes, such as Go
    // to Project Start or another song from its start. A drag in the ruler turns no page. Nor does a jump in the frame
    // of a scroll that the program sets, such as the one that a file keeps.
    TurnPage(canvas, transport, state, scrolled);
    ImGui::EndChild();
}

} // namespace

int BeatStep(float beat_width)
{
    int power = 1;
    int step = 1;
    while (beat_width * static_cast<float>(step) < Px(44.0f))
    {
        if (step == power)
        {
            step = power * 2;
        }
        else if (step == power * 2)
        {
            step = power * 5;
        }
        else
        {
            power *= 10;
            step = power;
        }
    }

    return step;
}

bool LabelledBeat(int quarter, int step)
{
    return quarter == 0 || (quarter + 1) % step == 0;
}

namespace
{

// A whole number of semitones or of velocity typed in the info line, with a sign or without one, from -127 to 127.
std::optional<int32_t> ParseAmount(std::string_view text)
{
    text = Trimmed(text);
    if (!text.empty() && text.front() == '+')
    {
        text.remove_prefix(1);
    }

    int32_t amount = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), amount);
    if (text.empty() || error != std::errc() || end != text.data() + text.size() || amount < -127 || amount > 127)
    {
        return std::nullopt;
    }

    return amount;
}

// The arrangement's info line shows the selected part: its name, start, end, length and notes. Without a selection, the
// line says so. A start typed there moves the part's notes. A transposition or a velocity typed there changes them by
// that much, and the field shows 0 again. Hokuto has nowhere to keep a part's transposition and velocity apart from its
// notes. Text that the line can't read is refused with a message. `shown` is the version and the sound of `song`.
void DrawPartInfoLine(const SongView* song, ArrangementState& state, const ShownItem& shown, bool busy)
{
    InfoLine line = BeginInfoLine();
    const bool selected =
        song != nullptr && state.selected_part_track >= 0 &&
        state.selected_part_track < static_cast<int>(song->tracks.size()) && state.selected_part >= 0 &&
        state.selected_part <
            static_cast<int>(song->tracks[static_cast<std::size_t>(state.selected_part_track)].parts.size());

    if (!selected)
    {
        NoObjectSelected(line);
        EndInfoLine(line);
        return;
    }

    const TrackView& track = song->tracks[static_cast<std::size_t>(state.selected_part_track)];
    const PartView& part = track.parts[static_cast<std::size_t>(state.selected_part)];
    const double quarter = std::max(song->ticks_per_quarter, 1);

    // A value typed for another part drops. The transposition and the velocity show 0 for every part.
    const ShownItem item =
        shown.At({static_cast<uint64_t>(state.selected_part_track), static_cast<uint64_t>(state.selected_part), 0});
    const auto typed = [&](const char* label, const std::string& value) -> std::optional<std::string>
    {
        std::optional<std::string> text =
            BusyInfoEntry(line, label, value, state.part_typing, state.part_waiting, item, busy);
        return text && *text != value ? text : std::nullopt;
    };

    const auto ask = [&](PartValue value, int64_t amount)
    {
        if (amount != 0 && amount >= std::numeric_limits<int32_t>::min() &&
            amount <= std::numeric_limits<int32_t>::max())
        {
            state.part_edit =
                PartEdit{state.selected_part_track, state.selected_part, value, static_cast<int32_t>(amount), {}};
        }
    };

    const auto refuse = [&](std::string refused)
    {
        state.part_edit = PartEdit{.refused = std::move(refused)};
    };

    InfoField(line, "NAME", part.name.empty() ? track.name : part.name);
    if (const std::optional<std::string> start =
            typed("START", PositionText(part.start / quarter, song->beats_per_bar)))
    {
        if (const std::optional<double> quarters = ParsePosition(*start, song->beats_per_bar))
        {
            ask(PartValue::kStart, std::llround(*quarters * quarter) - int64_t{part.start});
        }
        else
        {
            refuse("\"" + *start + "\" isn't a valid start. " + PositionHint("A start"));
        }
    }
    InfoField(line, "END", PositionText((part.start + part.length) / quarter, song->beats_per_bar));
    InfoField(line, "LENGTH", LengthText(part.length / quarter, song->beats_per_bar));
    InfoField(line, "NOTES", std::to_string(part.notes.size() + part.unstarted.size()));
    if (const std::optional<std::string> semitones = typed("TRANSPOSE", "0"))
    {
        if (const std::optional<int32_t> amount = ParseAmount(*semitones))
        {
            ask(PartValue::kTranspose, *amount);
        }
        else
        {
            refuse("\"" + *semitones + "\" isn't a valid transposition. Enter a number of semitones from -127 to 127.");
        }
    }
    if (const std::optional<std::string> velocity = typed("VELOCITY", "0"))
    {
        if (const std::optional<int32_t> amount = ParseAmount(*velocity))
        {
            ask(PartValue::kVelocity, *amount);
        }
        else
        {
            refuse("\"" + *velocity + "\" isn't a valid velocity change. Enter a number from -127 to 127.");
        }
    }
    if (part.linked)
    {
        InfoField(line, "LINKED", "Yes");
    }
    EndInfoLine(line);
}

} // namespace

void DrawArrangement(SongView* song, Transport& transport, ArrangementState& state, const ShownItem& shown, bool busy)
{
    if (BeginZone(kArrangement, nullptr))
    {
        std::string detail;
        if (song != nullptr)
        {
            detail = song->name;
        }
        ZoneHeader("Arrangement", detail);
        DrawPartInfoLine(song, state, shown, busy);
        DrawCanvas(song, transport, state);
    }
    ImGui::End();
}

} // namespace hokuto::app
