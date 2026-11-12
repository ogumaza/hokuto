// SPDX-License-Identifier: MIT

// Mouse input in the Key Editor: the zooms, the presses, drags and releases on the notes and the lanes, and the edits
// that they make.

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ntdlib/commands/sequence.h"

#include "app/key_canvas.h"
#include "app/key_editor.h"
#include "app/key_edits.h"
#include "app/lanes.h"
#include "app/list_edits.h"
#include "app/theme.h"
#include "app/time_text.h"
#include "app/transport.h"
#include "app/widgets.h"
#include "edit/commands.h"
#include "edit/notes.h"

namespace hokuto::app
{
namespace
{

using key_canvas::Canvas;
using key_canvas::kDragDistance;
using key_canvas::kEndGrab;
using key_canvas::KeyAt;
using key_canvas::LaneBottom;
using key_canvas::LaneTop;
using key_canvas::LineAt;
using key_canvas::NoteAt;
using key_canvas::NoteRect;
using key_canvas::OverviewTicks;
using key_canvas::PointAt;
using key_canvas::PointHeights;
using key_canvas::TickAt;
using key_canvas::ValueAt;
using key_canvas::ValueY;
using key_canvas::XOf;
using ntdlib::commands::Source;
using theme::Px;

} // namespace

namespace key_canvas
{

void ZoomTime(Canvas& canvas, KeyEditorState& state, float factor, float anchor_x)
{
    const double anchor = TickAt(canvas, anchor_x);
    state.beat_width = std::clamp(state.beat_width * factor, kMinBeatWidth, kMaxBeatWidth);
    canvas.beat_width = Px(state.beat_width);
    canvas.scroll_x = std::max(
        static_cast<float>(anchor / canvas.ticks_per_quarter) * canvas.beat_width - (anchor_x - canvas.notes.Min.x),
        0.0f);
    ImGui::SetScrollX(canvas.scroll_x);
}

void ZoomKeys(Canvas& canvas, KeyEditorState& state, float factor, float anchor_y)
{
    const float rows = (anchor_y - canvas.notes.Min.y + canvas.scroll_y) / canvas.key_height;
    state.key_height = std::clamp(state.key_height * factor, kMinKeyHeight, kMaxKeyHeight);
    canvas.key_height = Px(state.key_height);
    canvas.scroll_y = std::max(rows * canvas.key_height - (anchor_y - canvas.notes.Min.y), 0.0f);
    ImGui::SetScrollY(canvas.scroll_y);
}

} // namespace key_canvas

namespace
{

// Scrolls the notes to put the tick under `x` in the miniature in the middle of the view.
void ScrollToOverview(Canvas& canvas, const KeyTrack& track, float x)
{
    const ImRect& strip = canvas.overview;
    const double tick = std::clamp((x - strip.Min.x) / strip.GetWidth(), 0.0f, 1.0f) * OverviewTicks(track);
    const float scroll =
        static_cast<float>(tick / canvas.ticks_per_quarter) * canvas.beat_width - canvas.notes.GetWidth() * 0.5f;
    canvas.scroll_x = std::clamp(scroll, 0.0f, ImGui::GetScrollMaxX());
    ImGui::SetScrollX(canvas.scroll_x);
}

// Starts a gesture at a press in controller lane `lane`. With the Object Selection tool, a press on a point selects it
// and starts a drag of its value and its tick, and a press elsewhere starts a box. Shift adds to the selection. With
// the Draw tool, a press on a point drags it, and a press elsewhere starts a new point at the grid's line before the
// mouse. The Erase tool deletes the point under the mouse. A press in a controller lane clears the selection of notes.
// Returns an edit that a press makes at once.
std::optional<KeyEdit> LanePress(const Canvas& canvas, const KeyTrack& track, KeyEditorState& state, std::size_t lane,
                                 ImVec2 mouse)
{
    KeyGesture& gesture = state.gesture;
    gesture.lane = lane;
    state.selected.clear();
    const bool shift = ImGui::GetIO().KeyShift;
    const std::optional<std::size_t> hit = PointAt(canvas, track, state, lane, mouse);
    if (state.tool == KeyTool::kErase)
    {
        return hit ? std::optional<KeyEdit>(LaneRemoval({track.controls[*hit].offset})) : std::nullopt;
    }

    if (hit)
    {
        const uint32_t offset = track.controls[*hit].offset;
        if (state.tool == KeyTool::kSelect && shift && state.selected_controls.contains(offset))
        {
            state.selected_controls.erase(offset);
            return std::nullopt;
        }
        if (!shift && !state.selected_controls.contains(offset))
        {
            state.selected_controls.clear();
        }
        state.selected_controls.insert(offset);
        gesture.kind = KeyGesture::Kind::kControl;
        gesture.control = *hit;
        gesture.value = track.controls[*hit].command.value.value;
        return std::nullopt;
    }

    const LaneSetting setting = state.lanes[lane];
    if (!Holds(track.console, setting))
    {
        return std::nullopt;
    }
    if (state.tool == KeyTool::kDraw)
    {
        const int64_t tick = Snapped(state, canvas.ticks_per_quarter, TickAt(canvas, mouse.x), true);
        gesture.kind = KeyGesture::Kind::kLaneDraw;
        gesture.tick = static_cast<uint32_t>(std::max<int64_t>(tick, 0));
        gesture.value = ValueAt(canvas.lanes[lane], track, setting, LaneRange(track, setting), mouse.y);
        return std::nullopt;
    }

    if (!shift)
    {
        state.selected_controls.clear();
    }
    gesture.kind = KeyGesture::Kind::kLaneBox;

    return std::nullopt;
}

// A press of the Select tool on note `hit`, or among the notes where `hit` is nothing. A press on a note selects it,
// or with `shift` adds it to the selection or takes it out. A press among the notes starts a box.
void SelectPress(const Canvas& canvas, const KeyTrack& track, KeyEditorState& state, std::optional<std::size_t> hit,
                 ImVec2 mouse, bool shift)
{
    KeyGesture& gesture = state.gesture;
    if (!hit)
    {
        if (!shift)
        {
            state.selected.clear();
        }
        gesture.kind = KeyGesture::Kind::kBox;
        return;
    }
    const uint32_t command = track.notes[*hit].command;
    if (shift && state.selected.contains(command))
    {
        state.selected.erase(command);
        return;
    }
    if (!shift && !state.selected.contains(command))
    {
        state.selected.clear();
    }
    state.selected.insert(command);
    const ImRect rect = NoteRect(canvas, track.notes[*hit], 0, 0, 0);
    gesture.kind = KeyGesture::Kind::kPress;
    gesture.note = *hit;
    gesture.at_end = mouse.x >= rect.Max.x - Px(kEndGrab) && rect.GetWidth() >= Px(kEndGrab * 2.0f);
}

// A press of the Draw tool among the notes. It starts a note at the snapped tick and the key under `mouse`.
void StartDrawnNote(const Canvas& canvas, const KeyTrack& track, KeyEditorState& state, ImVec2 mouse)
{
    KeyGesture& gesture = state.gesture;
    const double step = state.snap ? GridTicks(state, canvas.ticks_per_quarter) : canvas.ticks_per_quarter / 4.0;
    const int64_t start = Snapped(state, canvas.ticks_per_quarter, TickAt(canvas, mouse.x), true);
    gesture.kind = KeyGesture::Kind::kDraw;
    gesture.drawn.start = static_cast<uint32_t>(std::max<int64_t>(start, 0));
    gesture.drawn.length = static_cast<uint32_t>(std::max<int64_t>(std::llround(step), 1));
    gesture.drawn.key = KeyAt(canvas, mouse.y);
    gesture.drawn.velocity = static_cast<int>(state.insert_velocity);
    gesture.drawn = WithInstrumentAt(track, gesture.drawn.start, gesture.drawn);
}

// Starts a gesture at a press of the mouse. A press with the Object Selection tool selects the note under it, or starts
// a box. Shift adds to the selection. A press with the Draw tool starts a note, or deletes the note under it. A press
// with the Erase tool deletes the note under it. A press among the notes or in the velocity lane clears the selection
// of the controller lanes. Returns an edit that a press makes at once.
std::optional<KeyEdit> Press(const Canvas& canvas, const KeyTrack& track, KeyEditorState& state, ImVec2 mouse)
{
    KeyGesture& gesture = state.gesture;
    gesture = KeyGesture();
    gesture.from = mouse;
    const bool shift = ImGui::GetIO().KeyShift;
    for (std::size_t lane = 0; lane < canvas.lanes.size(); lane++)
    {
        if (canvas.lanes[lane].Contains(mouse))
        {
            return LanePress(canvas, track, state, lane, mouse);
        }
    }
    state.selected_controls.clear();

    // The velocity lane.
    if (canvas.lane.Contains(mouse))
    {
        if (const std::optional<std::size_t> line = LineAt(canvas, track, state, mouse.x))
        {
            gesture.kind = KeyGesture::Kind::kVelocity;
            gesture.note = *line;
            gesture.velocity = track.notes[*line].velocity;
        }
        return std::nullopt;
    }

    const std::optional<std::size_t> hit = NoteAt(canvas, track, mouse);
    switch (state.tool)
    {
    case KeyTool::kSelect:
        SelectPress(canvas, track, state, hit, mouse, shift);
        return std::nullopt;

    case KeyTool::kDraw:
        if (hit)
        {
            return Removal({track.notes[*hit].command});
        }
        StartDrawnNote(canvas, track, state, mouse);
        return std::nullopt;

    case KeyTool::kErase:
        if (hit)
        {
            return Removal({track.notes[*hit].command});
        }
        return std::nullopt;
    }

    return std::nullopt;
}

// The pixels across the canvas of `ticks`.
double PixelsOf(const Canvas& canvas, double ticks)
{
    return ticks * canvas.beat_width / canvas.ticks_per_quarter;
}

// How far a drag of notes goes to the side before it says why the notes stop short: three times the distance that
// turns a press into a drag.
double StopDistance()
{
    return Px(kDragDistance * 3.0f);
}

// A drag of the pressed note and the selection by `moved` ticks and by the keys from the press to `mouse`. A note that
// can't move says why in a tooltip.
void DragMove(const Canvas& canvas, const KeyTrack& track, KeyEditorState& state, ImVec2 mouse, double moved)
{
    KeyGesture& gesture = state.gesture;
    const KeyNote& pressed = track.notes[gesture.note];
    const std::optional<edit::ShiftRange> range = SharedRange(state, state.selected);
    const int64_t start = Snapped(state, canvas.ticks_per_quarter, pressed.start + moved, false);
    const int64_t shift = start - static_cast<int64_t>(pressed.start);
    gesture.shift =
        range ? static_cast<int32_t>(std::clamp<int64_t>(shift, -int64_t{range->earlier}, int64_t{range->later})) : 0;
    const auto [down, up] = TransposeRange(track, state);
    const int keys = KeyAt(canvas, mouse.y) - KeyAt(canvas, gesture.from.y);
    gesture.transpose = std::clamp(keys, down, up);

    // A note that can't move says why, and so does a move that stops at the note's room. A release that changes
    // nothing then says it again (ReleasedNoteEdit). A press that wobbles says nothing.
    const std::string limit =
        ShiftLimitText("This note", int64_t{pressed.start} + gesture.shift, shift < 0, canvas.ticks_per_quarter);
    // A drag up or down counts once the pointer leaves the pressed key's row.
    const double rise = keys != 0 ? static_cast<double>(mouse.y - gesture.from.y) : 0.0;
    gesture.stopped =
        StoppedText(track, state, true, PixelsOf(canvas, moved),
                    PixelsOf(canvas, static_cast<double>(shift - gesture.shift)), StopDistance(), limit, rise);
    if (!gesture.stopped.empty())
    {
        WrappedTooltip(gesture.stopped);
    }
}

// A drag of the end of the pressed note by `moved` ticks. It sizes the selected notes.
void DragSize(const Canvas& canvas, const KeyTrack& track, KeyEditorState& state, double moved)
{
    KeyGesture& gesture = state.gesture;
    const std::optional<std::map<uint32_t, uint32_t>> lengths = BaseLengths(track, state);
    if (!lengths)
    {
        // As with a move, only a drag to the side says why.
        gesture.resize = 0;
        gesture.stopped = StoppedText(track, state, false, PixelsOf(canvas, moved), 0.0, StopDistance(), {});
        if (!gesture.stopped.empty())
        {
            WrappedTooltip(gesture.stopped);
        }
        return;
    }

    // The pressed note's command takes the length up to the snapped end. The note can sound for less than its
    // command's length (Sizing).
    const KeyNote& pressed = track.notes[gesture.note];
    const uint32_t end = pressed.start + pressed.length;
    const int64_t snapped = Snapped(state, canvas.ticks_per_quarter, end + moved, false);
    const auto base = lengths->find(pressed.command);
    const uint32_t length = base != lengths->end() ? base->second : pressed.length;
    int64_t resize = snapped - (int64_t{pressed.start} + length);
    for (const auto& [command, other_length] : *lengths)
    {
        resize = std::max<int64_t>(resize, 1 - static_cast<int64_t>(other_length));
    }
    gesture.resize = static_cast<int32_t>(resize);
}

// Follows a drag. A press that has moved far enough becomes a move or a size. A move snaps the pressed note's start to
// the grid, and a size its end. The notes move together. The note with the least room limits the move.
void Drag(const Canvas& canvas, const KeyTrack& track, KeyEditorState& state, ImVec2 mouse)
{
    // A drag along the keyboard plays the keys under the mouse in turn.
    KeyGesture& gesture = state.gesture;
    if (gesture.kind == KeyGesture::Kind::kKeyboard)
    {
        gesture.played.key = KeyAt(canvas, mouse.y);
        return;
    }

    if (gesture.kind == KeyGesture::Kind::kPress)
    {
        const ImVec2 moved(mouse.x - gesture.from.x, mouse.y - gesture.from.y);
        if (moved.x * moved.x + moved.y * moved.y < Px(kDragDistance) * Px(kDragDistance))
        {
            return;
        }
        gesture.kind = gesture.at_end ? KeyGesture::Kind::kSize : KeyGesture::Kind::kMove;
    }

    const double moved = TickAt(canvas, mouse.x) - TickAt(canvas, gesture.from.x);
    switch (gesture.kind)
    {
    case KeyGesture::Kind::kMove:
        DragMove(canvas, track, state, mouse, moved);
        break;

    case KeyGesture::Kind::kSize:
        DragSize(canvas, track, state, moved);
        break;

    case KeyGesture::Kind::kVelocity:
        {
            const float top = LaneTop(canvas.lane);
            const float bottom = LaneBottom(canvas.lane);
            const float level = (bottom - mouse.y) / (bottom - top);
            gesture.velocity = std::clamp(static_cast<int>(std::lround(level * kTopVelocity)), 1, kTopVelocity);
            break;
        }

    case KeyGesture::Kind::kDraw:
        {
            const double step = state.snap ? GridTicks(state, canvas.ticks_per_quarter) : 1.0;
            const int64_t end = Snapped(state, canvas.ticks_per_quarter, TickAt(canvas, mouse.x), false);
            const int64_t length = end - static_cast<int64_t>(gesture.drawn.start);
            gesture.drawn.length =
                static_cast<uint32_t>(std::max<int64_t>(length, std::max<int64_t>(std::llround(step), 1)));
            break;
        }

    default:
        break;
    }
}

// Follows a drag in a controller lane. A point moves with the mouse once the mouse has moved far enough up or down,
// from the height where the press found it. It moves in time once the mouse has moved far enough to the side: to the
// grid's nearest line, as far as the commands that move with it allow (DraggedControls). A point being drawn takes the
// value at the mouse's height. Both show their values in a tooltip, and a moved point its start.
void LaneDrag(const Canvas& canvas, const KeyTrack& track, KeyEditorState& state, ImVec2 mouse)
{
    KeyGesture& gesture = state.gesture;
    if (gesture.lane >= canvas.lanes.size())
    {
        return;
    }

    const ImRect& lane = canvas.lanes[gesture.lane];
    const LaneSetting setting = state.lanes[gesture.lane];
    const edit::ValueRange range = LaneRange(track, setting);
    if (gesture.kind == KeyGesture::Kind::kLaneDraw)
    {
        gesture.value = ValueAt(lane, track, setting, range, mouse.y);
        WrappedTooltip(LaneName(setting) + " " + ValueText(track.console, setting.kind, gesture.value));
        return;
    }
    if (gesture.kind != KeyGesture::Kind::kControl)
    {
        return;
    }

    // The move in time.
    const KeyControl& pressed = track.controls[gesture.control];
    const float across = mouse.x - gesture.from.x;
    std::string refused;
    if (std::abs(across) >= Px(kDragDistance) || gesture.shift != 0)
    {
        const std::optional<edit::ShiftRange> room = SharedRange(state, DraggedControls(track, state, gesture.control));
        const double moved = TickAt(canvas, mouse.x) - TickAt(canvas, gesture.from.x);
        const int64_t shift =
            Snapped(state, canvas.ticks_per_quarter, pressed.tick + moved, false) - int64_t{pressed.tick};
        gesture.shift =
            room ? static_cast<int32_t>(std::clamp<int64_t>(shift, -int64_t{room->earlier}, int64_t{room->later})) : 0;
        if (!room && std::abs(across) >= Px(kDragDistance * 3.0f))
        {
            refused = WhyPointsStay(track, state, gesture.control);
        }
    }

    // The change of the value. A value from a random number or a variable has no one height to drag.
    const float upward = mouse.y - gesture.from.y;
    const Source source = pressed.command.value.source;
    if (source != Source::kValue && std::abs(upward) >= Px(kDragDistance))
    {
        refused =
            std::string(source == Source::kRandom ? "This value is random." : "This value comes from a variable.") +
            " Edit it in the List Editor.";
    }
    else if (source == Source::kValue &&
             (std::abs(upward) >= Px(kDragDistance) || gesture.value != pressed.command.value.value))
    {
        gesture.value = ValueAt(lane, track, setting, range, ValueY(lane, range, pressed.command.value.value) + upward);
    }

    // The tooltip names the value and a moved point's start, or why the point stays.
    std::string text =
        LaneName(setting) + " " +
        (source == Source::kValue ? ValueText(track.console, setting.kind, gesture.value)
                                  : ArgumentText(track.console, pressed.command.kind, pressed.command.value));
    if (gesture.shift != 0)
    {
        const double start = static_cast<double>(int64_t{pressed.tick} + gesture.shift);
        text += "\nStart " + WithoutFigureSpaces(PositionText(start / canvas.ticks_per_quarter, kBeatsPerBar));
    }
    WrappedTooltip(refused.empty() ? text : refused);
}

// The edit of the velocity lane that a release of a drag of a note's line makes.
std::optional<KeyEdit> VelocityEdit(const KeyTrack& track, const KeyEditorState& state, const KeyGesture& gesture)
{
    const KeyNote& pressed = track.notes[gesture.note];
    if (gesture.velocity == pressed.velocity)
    {
        return std::nullopt;
    }

    // A line of a selected note changes every selected note by the same amount. The Undo History names a change
    // in the velocity lane Edit Controller.
    KeyEdit edit;
    edit.name = "Edit Controller";
    const int change = gesture.velocity - pressed.velocity;
    std::map<uint32_t, int> velocities = {{pressed.command, pressed.velocity}};
    if (state.selected.contains(pressed.command))
    {
        velocities = Velocities(track, state);
    }
    for (const auto& [command, velocity] : velocities)
    {
        edit.changes.push_back({.command = command, .velocity = std::clamp(velocity + change, 1, kTopVelocity)});
    }

    return edit;
}

// Selects each note that the box from the start of `gesture` to `mouse` overlaps.
void SelectInBox(const Canvas& canvas, const KeyTrack& track, KeyEditorState& state, const KeyGesture& gesture,
                 ImVec2 mouse)
{
    const ImRect box(ImMin(gesture.from, mouse), ImMax(gesture.from, mouse));
    for (const KeyNote& note : track.notes)
    {
        if (box.Overlaps(NoteRect(canvas, note, 0, 0, 0)))
        {
            state.selected.insert(note.command);
        }
    }
}

// The box selects each point of the lane whose heights it reaches.
void SelectLanePointsInBox(const Canvas& canvas, const KeyTrack& track, KeyEditorState& state,
                           const KeyGesture& gesture, ImVec2 mouse)
{
    if (gesture.lane >= canvas.lanes.size())
    {
        return;
    }

    const ImRect box(ImMin(gesture.from, mouse), ImMax(gesture.from, mouse));
    const ImRect& lane = canvas.lanes[gesture.lane];
    const LaneSetting setting = state.lanes[gesture.lane];
    const edit::ValueRange range = LaneRange(track, setting);
    for (const LanePoint& point : LaneOf(track, setting).points)
    {
        const KeyControl& control = track.controls[point.control];
        const float x = XOf(canvas, point.tick);
        const auto [top, bottom] = PointHeights(lane, range, control);
        if (x >= box.Min.x && x <= box.Max.x && bottom >= box.Min.y && top <= box.Max.y)
        {
            state.selected_controls.insert(control.offset);
        }
    }
}

// Ends a gesture when the mouse is released. Returns the edit that it makes.
std::optional<KeyEdit> Release(const Canvas& canvas, const KeyTrack& track, KeyEditorState& state, ImVec2 mouse)
{
    const KeyGesture gesture = std::exchange(state.gesture, KeyGesture());
    switch (gesture.kind)
    {
    case KeyGesture::Kind::kMove:
    case KeyGesture::Kind::kSize:
        return ReleasedNoteEdit(track, state, gesture);

    case KeyGesture::Kind::kVelocity:
        return VelocityEdit(track, state, gesture);

    case KeyGesture::Kind::kBox:
        SelectInBox(canvas, track, state, gesture, mouse);
        return std::nullopt;

    case KeyGesture::Kind::kDraw:
        {
            // The Undo History names a drawn note Insert Note.
            KeyEdit edit;
            edit.name = "Insert Note";
            edit.added = {gesture.drawn};
            return edit;
        }

    case KeyGesture::Kind::kControl:
        return gesture.control < track.controls.size()
                   ? LanePointEdit(track, state, gesture.control, gesture.shift, gesture.value)
                   : std::nullopt;

    case KeyGesture::Kind::kLaneDraw:
        if (gesture.lane < state.lanes.size())
        {
            return LaneDrawEdit(track, state.lanes[gesture.lane], gesture.tick, gesture.value);
        }
        return std::nullopt;

    case KeyGesture::Kind::kLaneBox:
        SelectLanePointsInBox(canvas, track, state, gesture, mouse);
        return std::nullopt;

    default:
        return std::nullopt;
    }
}

} // namespace

namespace key_canvas
{

namespace
{

// Zooms or scrolls the view by a turn of the wheel over the window.
void HandleWheel(Canvas& canvas, KeyEditorState& state, bool hovered)
{
    const ImGuiIO& io = ImGui::GetIO();
    if (hovered && io.KeyCtrl && io.MouseWheel != 0.0f)
    {
        ZoomTime(canvas, state, std::pow(kZoomStep, io.MouseWheel), io.MousePos.x);
    }
    else if (hovered && io.KeyAlt && io.MouseWheel != 0.0f)
    {
        ZoomKeys(canvas, state, std::pow(kZoomStep, io.MouseWheel), io.MousePos.y);
    }
    else if (hovered && (io.MouseWheel != 0.0f || io.MouseWheelH != 0.0f))
    {
        // macOS turns the wheel with Shift into the horizontal wheel. Other systems leave it vertical.
        const float sideways = io.KeyShift ? io.MouseWheel + io.MouseWheelH : io.MouseWheelH;
        const float down = io.KeyShift ? 0.0f : io.MouseWheel;
        ImGui::SetScrollX(canvas.scroll_x - sideways * Px(kWheelStep));
        ImGui::SetScrollY(canvas.scroll_y - down * Px(kWheelStep));
    }
}

// Handles a press in the miniature and a drag from it. Returns true when the mouse acts there.
bool HandleOverview(Canvas& canvas, const KeyTrack& track, KeyEditorState& state, bool hovered, ImVec2 mouse)
{
    // A press in the miniature, and a drag from it, move the view.
    const bool overview = state.gesture.kind == KeyGesture::Kind::kOverview;
    if (hovered && canvas.overview.Contains(mouse))
    {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }
    if (hovered && canvas.overview.Contains(mouse) && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
    {
        state.gesture = KeyGesture();
        state.gesture.kind = KeyGesture::Kind::kOverview;
        ScrollToOverview(canvas, track, mouse.x);
        return true;
    }
    if (overview && ImGui::IsMouseDown(ImGuiMouseButton_Left))
    {
        ScrollToOverview(canvas, track, mouse.x);
        return true;
    }
    if (overview)
    {
        state.gesture = KeyGesture();
        return true;
    }

    return false;
}

// Handles a click on a controller lane's label and on the button that adds a lane. Returns true when the click acts.
bool HandleLaneLabels(const Canvas& canvas, const KeyTrack& track, KeyEditorState& state, bool hovered, ImVec2 mouse)
{
    // A click on a controller lane's label opens its menu, and a click on the button under the labels adds a lane.
    const bool idle = state.gesture.kind == KeyGesture::Kind::kNone;
    for (std::size_t lane = 0; lane < canvas.lane_labels.size(); lane++)
    {
        if (idle && hovered && canvas.lane_labels[lane].Contains(mouse) && !canvas.add.Contains(mouse) &&
            ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            state.lane_menu = lane;
            ImGui::OpenPopup("##lane");
            return true;
        }
    }
    if (idle && hovered && canvas.add.Contains(mouse))
    {
        WrappedTooltip("Create Controller Lane");
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            state.lanes.push_back(NextLane(track, state.lanes));
            return true;
        }
    }

    return false;
}

// The mouse shows where a press sizes a note.
void ShowSizeCursor(const Canvas& canvas, const KeyTrack& track, const KeyEditorState& state, bool hovered,
                    ImVec2 mouse)
{
    if (hovered && state.tool == KeyTool::kSelect && state.gesture.kind == KeyGesture::Kind::kNone &&
        canvas.notes.Contains(mouse))
    {
        if (const std::optional<std::size_t> hit = NoteAt(canvas, track, mouse))
        {
            const ImRect rect = NoteRect(canvas, track.notes[*hit], 0, 0, 0);
            if (mouse.x >= rect.Max.x - Px(kEndGrab) && rect.GetWidth() >= Px(kEndGrab * 2.0f))
            {
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
            }
        }
    }
    if (state.gesture.kind == KeyGesture::Kind::kSize)
    {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    }
}

} // namespace

std::optional<KeyEdit> HandleMouse(Canvas& canvas, const KeyTrack& track, KeyEditorState& state, Transport& transport,
                                   bool busy)
{
    const ImGuiIO& io = ImGui::GetIO();
    const bool hovered = ImGui::IsWindowHovered();
    HandleWheel(canvas, state, hovered);

    const ImVec2 mouse = io.MousePos;
    if (hovered && canvas.ruler.Contains(mouse) && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
    {
        transport.SetPosition(RulerPosition(state, canvas.ticks_per_quarter, TickAt(canvas, mouse.x)));
        return std::nullopt;
    }

    if (HandleOverview(canvas, track, state, hovered, mouse))
    {
        return std::nullopt;
    }

    if (HandleLaneLabels(canvas, track, state, hovered, mouse))
    {
        return std::nullopt;
    }

    // While the program reads the last edit's result, the notes and the lanes take no gestures.
    bool in_canvas = canvas.notes.Contains(mouse) || canvas.lane.Contains(mouse);
    for (const ImRect& lane : canvas.lanes)
    {
        in_canvas = in_canvas || lane.Contains(mouse);
    }
    if (busy)
    {
        state.gesture = KeyGesture();
        if (hovered && in_canvas)
        {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Wait);
        }
        return std::nullopt;
    }

    ShowSizeCursor(canvas, track, state, hovered, mouse);

    // A press on the keyboard plays its key at the velocity of drawn notes, with the instrument at the position.
    if (hovered && canvas.keyboard.Contains(mouse) && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
    {
        const double position = transport.Position() * canvas.ticks_per_quarter;
        state.gesture = KeyGesture();
        state.gesture.kind = KeyGesture::Kind::kKeyboard;
        state.gesture.played.key = KeyAt(canvas, mouse.y);
        state.gesture.played.velocity = static_cast<int>(state.insert_velocity);
        state.gesture.played = WithInstrumentAt(track, position, state.gesture.played);
    }
    else if (hovered && in_canvas && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
    {
        if (std::optional<KeyEdit> edit = Press(canvas, track, state, mouse))
        {
            return edit;
        }
    }
    else if (InLane(state.gesture) && ImGui::IsMouseDown(ImGuiMouseButton_Left))
    {
        LaneDrag(canvas, track, state, mouse);
    }
    else if (state.gesture.kind != KeyGesture::Kind::kNone && ImGui::IsMouseDown(ImGuiMouseButton_Left))
    {
        Drag(canvas, track, state, mouse);
    }
    else if (state.gesture.kind != KeyGesture::Kind::kNone)
    {
        return Release(canvas, track, state, mouse);
    }

    return std::nullopt;
}

} // namespace key_canvas
} // namespace hokuto::app
