// SPDX-License-Identifier: MIT

// The Key Editor's window: the toolbar, the info line, the keys, the lane menu and the canvas. key_editor_draw.cpp
// draws the canvas. key_editor_mouse.cpp handles the mouse.

#include "app/key_editor.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "ntdlib/commands/sequence.h"

#include "app/key_canvas.h"
#include "app/key_edits.h"
#include "app/lanes.h"
#include "app/list_edits.h"
#include "app/theme.h"
#include "app/time_text.h"
#include "app/transport.h"
#include "app/views.h"
#include "app/widgets.h"
#include "edit/commands.h"

namespace hokuto::app
{
namespace
{

using key_canvas::Canvas;
using key_canvas::ChannelBarAt;
using key_canvas::ChannelBarText;
using key_canvas::ChannelRowHeight;
using key_canvas::ChannelRows;
using key_canvas::CutText;
using key_canvas::DrawAddLane;
using key_canvas::DrawChannelLabel;
using key_canvas::DrawChannelLane;
using key_canvas::DrawControlLabel;
using key_canvas::DrawControlLane;
using key_canvas::DrawGrid;
using key_canvas::DrawKeyboard;
using key_canvas::DrawLane;
using key_canvas::DrawLaneLabel;
using key_canvas::DrawNotes;
using key_canvas::DrawOverview;
using key_canvas::DrawPlayhead;
using key_canvas::DrawRows;
using key_canvas::DrawRuler;
using key_canvas::HandleMouse;
using key_canvas::kAddSize;
using key_canvas::KeyAt;
using key_canvas::kKeyboardWidth;
using key_canvas::kLaneHeight;
using key_canvas::kLeastLaneHeight;
using key_canvas::kLeastNotesHeight;
using key_canvas::kMaxBeatWidth;
using key_canvas::kMaxKeyHeight;
using key_canvas::kMinBeatWidth;
using key_canvas::kMinKeyHeight;
using key_canvas::kOverviewHeight;
using key_canvas::kRulerHeight;
using key_canvas::kZoomStep;
using key_canvas::LanesBottom;
using key_canvas::NoteAt;
using key_canvas::PointAt;
using key_canvas::PointText;
using key_canvas::TickAt;
using key_canvas::UnstartedAt;
using key_canvas::UnstartedText;
using key_canvas::XOf;
using key_canvas::ZoomKeys;
using key_canvas::ZoomTime;
using ntdlib::commands::Source;
using theme::Px;

// The grid's note values that the toolbar offers.
constexpr std::array<int, 5> kGrids = {4, 8, 16, 32, 64};

// Acoustic Feedback and the tools, from `x` in the toolbar at `top`. Returns the left edge of the next control.
float DrawToolButtons(KeyEditorState& state, float x, float top)
{
    struct Tool
    {
        KeyTool tool;
        Icon icon;
        const char* tip;
    };
    static constexpr std::array<Tool, 3> kTools = {{
        {KeyTool::kSelect, Icon::kPointer, "Object Selection (1)"},
        {KeyTool::kDraw, Icon::kPencil, "Draw (8)"},
        {KeyTool::kErase, Icon::kEraser, "Erase (5)"},
    }};
    const ImVec2 button(Px(kToolSize), Px(kToolSize));
    PlaceInToolbar(x, top, button.y);
    if (IconButton("##feedback", Icon::kSound, button, state.feedback, theme::kAccent, "Acoustic Feedback"))
    {
        state.feedback = !state.feedback;
    }
    x += button.x + Px(14.0f);
    for (const Tool& tool : kTools)
    {
        PlaceInToolbar(x, top, button.y);
        ImGui::PushID(static_cast<int>(tool.tool));
        if (IconButton("##tool", tool.icon, button, state.tool == tool.tool, theme::kAccent, tool.tip))
        {
            state.tool = tool.tool;
        }
        ImGui::PopID();
        x += button.x + Px(2.0f);
    }

    return x;
}

// The snap, and the grid that it snaps to, from `x` in the toolbar at `top`. Returns the left edge of the grid's menu.
float DrawSnapControls(KeyEditorState& state, float x, float top)
{
    const ImVec2 button(Px(kToolSize), Px(kToolSize));
    x += Px(14.0f);
    PlaceInToolbar(x, top, button.y);
    if (IconButton("##snap", Icon::kSnap, button, state.snap, theme::kAccent, "Snap On/Off"))
    {
        state.snap = !state.snap;
    }
    x += button.x + Px(6.0f);
    PlaceInToolbar(x, top, ImGui::GetFrameHeight());
    ImGui::SetNextItemWidth(Px(76.0f));
    const std::string grid = "1/" + std::to_string(state.grid);
    if (ImGui::BeginCombo("##grid", grid.c_str()))
    {
        for (const int value : kGrids)
        {
            const std::string name = "1/" + std::to_string(value);
            if (ImGui::Selectable(name.c_str(), value == state.grid))
            {
                state.grid = value;
            }
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
    {
        WrappedTooltip("Quantize");
    }

    return x;
}

// The menu of the notes' colours, after the control that ends at `x`, in the toolbar at `top`. Returns the left edge
// of the menu.
float DrawColoursCombo(KeyEditorState& state, ImDrawList* draw, float x, float top)
{
    x = ToolbarCaps(draw, x + Px(64.0f) + Px(20.0f), top, "COLOURS");
    PlaceInToolbar(x, top, ImGui::GetFrameHeight());
    ImGui::SetNextItemWidth(Px(96.0f));
    static constexpr std::array<const char*, 2> kColourNames = {"Track", "Velocity"};
    const auto chosen = static_cast<std::size_t>(state.colours);
    if (ImGui::BeginCombo("##colours", kColourNames[chosen]))
    {
        for (std::size_t i = 0; i < kColourNames.size(); i++)
        {
            if (ImGui::Selectable(kColourNames[i], i == chosen))
            {
                state.colours = static_cast<NoteColours>(i);
            }
        }
        ImGui::EndCombo();
    }

    return x;
}

// The key and the position under the mouse, at the right end of the toolbar at `origin`, `width` wide, where they fit
// after the control at `x`.
void DrawPointerReadout(const KeyEditorState& state, ImDrawList* draw, ImVec2 origin, float width, float x)
{
    ImFont* figures = theme::GetFonts().figures;
    const std::string value = KeyName(state.pointer->first) + "   " + PositionText(state.pointer->second, kBeatsPerBar);
    const ImVec2 size = TextSize(figures, theme::kTextSize, value);
    const float right = origin.x + width - Px(14.0f);
    const float middle = origin.y + Px(kToolbarHeight) * 0.5f;
    const ImVec2 mouse_caps = CapsSize("MOUSE");
    const float left = right - size.x - Px(8.0f) - mouse_caps.x;
    if (left > x + Px(96.0f) + Px(20.0f))
    {
        DrawCaps(draw, ImVec2(left, middle - mouse_caps.y * 0.5f), theme::kTextMuted, "MOUSE");
        DrawText(draw, figures, theme::kTextSize, ImVec2(right - size.x, middle - size.y * 0.5f), theme::kText, value);
    }
}

// The toolbar: Acoustic Feedback, the tools, the snap and its grid, and the velocity of drawn notes.
void DrawToolbar(KeyEditorState& state)
{
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const Toolbar bar = BeginToolbar(draw);
    const ImVec2 origin = bar.origin;
    const float width = bar.width;

    float x = DrawToolButtons(state, origin.x + Px(10.0f), origin.y);

    // The snap, and the grid that it snaps to.
    x = DrawSnapControls(state, x, origin.y);

    // The velocity of the notes that the Draw tool makes.
    x = ToolbarCaps(draw, x + Px(76.0f) + Px(20.0f), origin.y, "INSERT VELOCITY");
    PlaceInToolbar(x, origin.y, ImGui::GetFrameHeight());
    ValueSlider("##velocity", &state.insert_velocity, SliderRange{1.0f, kTopVelocity}, 100.0f, "%.0f",
                ImVec2(Px(64.0f), ImGui::GetFrameHeight()));
    state.insert_velocity = std::clamp(std::round(state.insert_velocity), 1.0f, static_cast<float>(kTopVelocity));

    // The notes' colours.
    x = DrawColoursCombo(state, draw, x, origin.y);

    // The key and the position under the mouse, at the bar's right end.
    if (state.pointer)
    {
        DrawPointerReadout(state, draw, origin, width, x);
    }

    EndToolbar(bar);
}

// The edit of `text`, typed as the value of the point of `track.controls[index]`. The point takes the typed value, and
// the other selected points of its setting change by the same amount (LaneValueEdit). A random value or a variable's
// value goes to the point alone. When the console can't hold the value, the edit says why. Text that the field can't
// read is refused with what the field takes.
std::optional<KeyEdit> TypedControlEdit(const KeyTrack& track, const KeyEditorState& state, std::size_t index,
                                        std::string_view text)
{
    const KeyControl& control = track.controls[index];
    const std::optional<TypedArgument> typed = TypedValueOf(track.console, track.number, control.command, text);
    if (!typed)
    {
        return Unreadable(text, "a value", ValueHint());
    }
    if (typed->command.value == control.command.value)
    {
        return std::nullopt;
    }

    KeyEdit edit;
    if (!typed->refused.empty())
    {
        edit.refused = typed->refused;
        return edit;
    }
    // A typed value makes a Set Main Value.
    if (typed->command.value.source == Source::kValue && control.command.value.source == Source::kValue)
    {
        std::optional<KeyEdit> changed =
            LaneValueEdit(track, state, index, edit::Nearest(track.console, typed->command).value.value);
        if (changed)
        {
            changed->name = "Set Main Value";
        }
        return changed;
    }

    edit.name = "Set Main Value";
    edit.changed_controls.emplace(control.offset, typed->command);

    return edit;
}

// The info line of a point of a controller lane: its start, and its value after the setting's name. A click on the
// start or the value types a new one, except while `busy`. A typed start moves the point (LaneStartEdit), and a typed
// value changes it (TypedControlEdit). Text that a field can't read is refused with what the field takes. Returns the
// edit of a typed start or value.
// A field that the user opened before the program turned busy stays open, and its text waits (BusyInfoEntry).
std::optional<KeyEdit> DrawControlInfo(const KeyTrack& track, KeyEditorState& state, std::size_t index,
                                       const ShownItem& item, bool busy, InfoLine& line)
{
    const KeyControl& control = track.controls[index];
    const double quarter = std::max(track.ticks_per_quarter, 1);
    const std::string start = PositionText(control.tick / quarter, kBeatsPerBar);
    const std::string label = LaneName(SettingOf(control.command).value_or(LaneSetting{}));
    const std::string value = ArgumentText(track.console, control.command.kind, control.command.value);
    std::optional<KeyEdit> edit;
    const std::optional<std::string> typed_start =
        BusyInfoEntry(line, "START", start, state.typing, state.typed_waiting, item, busy);
    if (typed_start && *typed_start != start)
    {
        if (const std::optional<double> quarters = ParsePosition(*typed_start, kBeatsPerBar))
        {
            edit = LaneStartEdit(track, state, index, std::llround(*quarters * quarter));
        }
        else
        {
            edit = Unreadable(*typed_start, "a start", PositionHint("A start"));
        }
    }
    const std::optional<std::string> typed_value =
        BusyInfoEntry(line, label.c_str(), value, state.typing, state.typed_waiting, item, busy);
    if (typed_value && *typed_value != value)
    {
        edit = TypedControlEdit(track, state, index, *typed_value);
    }

    return edit;
}

// The info line shows the values of the first selected note. A click on a value types a new one (TypedEdit), except
// while `busy`. Without a selected note, the line shows the first selected point of the controller lanes. Without a
// selection, the line says so. Returns the edit of a typed value.
// A field that the user opened before the program turned busy stays open, and its text waits (BusyInfoEntry).
std::optional<KeyEdit> DrawInfoLine(const KeyTrack& track, KeyEditorState& state, const ShownItem& shown, bool busy)
{
    InfoLine line = BeginInfoLine();
    const auto first = std::find_if(track.notes.begin(), track.notes.end(),
                                    [&state](const KeyNote& note) { return state.selected.contains(note.command); });
    const auto control = std::find_if(track.controls.begin(), track.controls.end(), [&state](const KeyControl& point)
                                      { return state.selected_controls.contains(point.offset); });

    // The fields show the first selected note, or else the first selected point. A value typed for another item drops.
    constexpr uint64_t kNoteItem = 1;
    constexpr uint64_t kPointItem = 2;
    if (first == track.notes.end() && control != track.controls.end())
    {
        const ShownItem item = shown.At({shown.place[0], kPointItem, control->offset});
        std::optional<KeyEdit> edit =
            DrawControlInfo(track, state, static_cast<std::size_t>(control - track.controls.begin()), item, busy, line);
        EndInfoLine(line);
        return edit;
    }
    if (first == track.notes.end())
    {
        NoObjectSelected(line);
        EndInfoLine(line);
        state.typing.field.clear();
        return std::nullopt;
    }

    const double quarter = track.ticks_per_quarter;
    const std::array<std::tuple<InfoValue, const char*, std::string>, 5> fields = {{
        {InfoValue::kStart, "START", PositionText(first->start / quarter, kBeatsPerBar)},
        {InfoValue::kEnd, "END", PositionText((first->start + first->length) / quarter, kBeatsPerBar)},
        {InfoValue::kLength, "LENGTH", LengthText(first->length / quarter, kBeatsPerBar)},
        {InfoValue::kPitch, "PITCH", KeyName(first->key)},
        {InfoValue::kVelocity, "VELOCITY", std::to_string(first->velocity)},
    }};
    const ShownItem item = shown.At({shown.place[0], kNoteItem, first->command});
    std::optional<KeyEdit> edit;
    for (const auto& [value, label, text] : fields)
    {
        const std::optional<std::string> typed =
            BusyInfoEntry(line, label, text, state.typing, state.typed_waiting, item, busy);
        if (typed && *typed != text)
        {
            edit = TypedEdit(track, state, value, *typed);
        }
    }
    EndInfoLine(line);

    return edit;
}

// The editor's keys while its window has the focus. Delete and Backspace delete the selected notes, or the selected
// points of the controller lanes. The up and down arrows move the notes a semitone, and with Shift an octave. Escape
// selects nothing. The keys 1, 8 and 5 choose the tools. The Edit menu's Select All selects every note (RequestedEdit).
std::optional<KeyEdit> HandleKeys(const KeyTrack& track, KeyEditorState& state)
{
    if (ImGui::GetIO().WantTextInput)
    {
        return std::nullopt;
    }

    // A transposition sounds the first selected note at its new key.
    const auto selected = [&state](const KeyNote& note)
    {
        return state.selected.contains(note.command);
    };
    const auto transpose = [&track, &state, &selected](int semitones)
    {
        std::optional<KeyEdit> edit = Transposition(track, state, semitones);
        const auto first = std::find_if(track.notes.begin(), track.notes.end(), selected);
        if (edit && edit->refused.empty() && first != track.notes.end())
        {
            state.transposed = *first;
            state.transposed->key += semitones;
        }

        return edit;
    };

    const int octave = ImGui::GetIO().KeyShift ? 12 : 1;
    const bool remove = ImGui::IsKeyPressed(ImGuiKey_Delete, false) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false);
    if (remove && !state.selected.empty())
    {
        return Removal(state.selected);
    }
    if (remove && !state.selected_controls.empty())
    {
        return LaneRemoval(state.selected_controls);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow))
    {
        return transpose(octave);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow))
    {
        return transpose(-octave);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
    {
        state.selected.clear();
        state.selected_controls.clear();
    }
    if (ImGui::IsKeyPressed(ImGuiKey_1, false))
    {
        state.tool = KeyTool::kSelect;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_8, false))
    {
        state.tool = KeyTool::kDraw;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_5, false))
    {
        state.tool = KeyTool::kErase;
    }

    return std::nullopt;
}

// The menu of the controller lane whose label the user clicked: the settings in groups, and Remove This Lane. A setting
// that the track sets has an asterisk, and so does its group. A setting that is missing from the console's sequences is
// off, and so is a group of such settings.
void DrawLaneMenu(const KeyTrack& track, KeyEditorState& state)
{
    if (!ImGui::BeginPopup("##lane"))
    {
        return;
    }

    const std::size_t lane = state.lane_menu.value_or(state.lanes.size());
    if (lane < state.lanes.size())
    {
        for (const LaneGroup& group : LaneGroups())
        {
            const bool set = std::any_of(group.settings.begin(), group.settings.end(),
                                         [&track](LaneSetting setting) { return Sets(track, setting); });
            const bool held = std::any_of(group.settings.begin(), group.settings.end(),
                                          [&track](LaneSetting setting) { return Holds(track.console, setting); });
            const std::string group_name = std::string(group.name) + (set ? " *" : "");
            if (!ImGui::BeginMenu(group_name.c_str(), held))
            {
                continue;
            }

            for (const LaneSetting setting : group.settings)
            {
                const std::string name = LaneName(setting) + (Sets(track, setting) ? " *" : "");
                if (ImGui::MenuItem(name.c_str(), nullptr, state.lanes[lane] == setting, Holds(track.console, setting)))
                {
                    state.lanes[lane] = setting;
                    state.selected_controls.clear();
                }
            }
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Remove This Lane"))
        {
            state.lanes.erase(state.lanes.begin() + static_cast<std::ptrdiff_t>(lane));
            state.selected_controls.clear();
            state.lane_menu.reset();
        }
    }
    ImGui::EndPopup();
}

// Applies the scrolls that the program asked for: to the start of the part that the user opened, to its keys, and to
// a place that a file keeps.
void ApplyCanvasScrolls(Canvas& canvas, KeyEditorState& state)
{
    if (state.scroll_tick)
    {
        canvas.scroll_x = std::clamp(
            static_cast<float>(*state.scroll_tick) / canvas.ticks_per_quarter * canvas.beat_width - Px(24.0f), 0.0f,
            ImGui::GetScrollMaxX());
        ImGui::SetScrollX(canvas.scroll_x);
        state.scroll_tick.reset();
    }
    if (state.scroll_key)
    {
        canvas.scroll_y = std::clamp(
            static_cast<float>(kKeys - 1 - *state.scroll_key) * canvas.key_height - canvas.notes.GetHeight() * 0.5f,
            0.0f, ImGui::GetScrollMaxY());
        ImGui::SetScrollY(canvas.scroll_y);
        state.scroll_key.reset();
    }

    // A scroll to a place that a file keeps.
    if (const std::optional<std::pair<double, float>> scroll = std::exchange(state.scroll_to, std::nullopt))
    {
        canvas.scroll_x = std::max(static_cast<float>(scroll->first) * canvas.beat_width, 0.0f);
        canvas.scroll_y = std::max((static_cast<float>(kKeys - 1) - scroll->second) * canvas.key_height, 0.0f);
        ImGui::SetScrollX(canvas.scroll_x);
        ImGui::SetScrollY(canvas.scroll_y);
    }
}

// Applies the zooms from the menu to `canvas`: by steps across and down, and Zoom Full over the song's `quarters`.
void ApplyCanvasZooms(Canvas& canvas, KeyEditorState& state, const Transport& transport, double quarters)
{
    if (state.zoom_steps != 0)
    {
        const float playhead = XOf(canvas, transport.Position() * canvas.ticks_per_quarter);
        const bool visible = playhead >= canvas.notes.Min.x && playhead <= canvas.notes.Max.x;
        ZoomTime(canvas, state, std::pow(kZoomStep, static_cast<float>(state.zoom_steps)),
                 visible ? playhead : canvas.notes.GetCenter().x);
        state.zoom_steps = 0;
    }
    if (state.vertical_zoom_steps != 0)
    {
        ZoomKeys(canvas, state, std::pow(kZoomStep, static_cast<float>(state.vertical_zoom_steps)),
                 canvas.notes.GetCenter().y);
        state.vertical_zoom_steps = 0;
    }
    if (std::exchange(state.zoom_full, false) && quarters > 0.0)
    {
        const float width = canvas.notes.GetWidth() / Px(1.0f);
        state.beat_width = std::clamp(width / static_cast<float>(quarters), kMinBeatWidth, kMaxBeatWidth);
        canvas.beat_width = Px(state.beat_width);
        canvas.scroll_x = 0.0f;
        ImGui::SetScrollX(0.0f);
    }
}

// Keeps the key and the tick under the `mouse` for the toolbar's next frame, and shows the tooltips of the points, the
// notes and the channels under it.
void ShowCanvasTooltips(const Canvas& canvas, const KeyTrack& track, KeyEditorState& state, ImVec2 mouse)
{
    state.pointer.reset();
    if (ImGui::IsWindowHovered() && canvas.notes.Contains(mouse))
    {
        state.pointer =
            std::make_pair(KeyAt(canvas, mouse.y), std::max(TickAt(canvas, mouse.x), 0.0) / canvas.ticks_per_quarter);
    }

    // A point of a controller lane under the mouse names its value and its start.
    for (std::size_t i = 0; i < canvas.lanes.size(); i++)
    {
        if (!ImGui::IsWindowHovered() || state.gesture.kind != KeyGesture::Kind::kNone ||
            !canvas.lanes[i].Contains(mouse))
        {
            continue;
        }

        if (const std::optional<std::size_t> point = PointAt(canvas, track, state, i, mouse))
        {
            WrappedTooltip(PointText(track, track.controls[*point]));
        }
    }

    // On the GBA and the DS, a note that a later note cut short, or that the driver never started, says so under the
    // mouse. So does a note in the lane of channels.
    if (ImGui::IsWindowHovered() && state.gesture.kind == KeyGesture::Kind::kNone)
    {
        if (canvas.notes.Contains(mouse))
        {
            const std::optional<std::size_t> hit = NoteAt(canvas, track, mouse);
            const std::optional<std::size_t> unstarted = hit ? std::nullopt : UnstartedAt(canvas, track, mouse);
            if (hit && track.notes[*hit].cut)
            {
                WrappedTooltip(CutText(track.console, track.notes[*hit]));
            }
            else if (unstarted)
            {
                WrappedTooltip(UnstartedText(track.console, track.unstarted[*unstarted]));
            }
        }
        else if (canvas.has_channels && canvas.channels.Contains(mouse))
        {
            if (const std::optional<std::size_t> bar = ChannelBarAt(canvas, track, mouse))
            {
                WrappedTooltip(ChannelBarText(track.console, track.channels[*bar]));
            }
        }
    }
}

// Draws the canvas's layers in `inner`, with the keyboard `keyboard` wide and the ruler and the overview `top` tall.
void DrawCanvasLayers(const Canvas& canvas, const KeyTrack& track, KeyEditorState& state, Transport& transport,
                      ImVec2 mouse, const ImRect& inner, float keyboard, float top)
{
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->PushClipRect(canvas.notes.Min, canvas.notes.Max, true);
    DrawRows(draw, canvas);
    DrawGrid(draw, canvas, state, canvas.notes);
    DrawNotes(draw, canvas, track, state);
    if (state.gesture.kind == KeyGesture::Kind::kBox)
    {
        const ImVec2 min = ImMin(state.gesture.from, mouse);
        const ImVec2 max = ImMax(state.gesture.from, mouse);
        draw->AddRectFilled(min, max, theme::WithAlpha(theme::kAccent, 0.12f));
        draw->AddRect(min, max, theme::WithAlpha(theme::kAccent, 0.7f), 0.0f, Px(1.0f));
    }
    draw->PopClipRect();
    if (canvas.has_channels)
    {
        draw->PushClipRect(canvas.channels.Min, canvas.channels.Max, true);
        DrawChannelLane(draw, canvas, track, state);
        draw->PopClipRect();
        draw->PushClipRect(canvas.channels_label.Min, canvas.channels_label.Max, true);
        DrawChannelLabel(draw, canvas, track.console);
        draw->PopClipRect();
    }
    draw->PushClipRect(canvas.lane.Min, canvas.lane.Max, true);
    DrawLane(draw, canvas, track, state);
    draw->PopClipRect();
    for (std::size_t i = 0; i < canvas.lanes.size(); i++)
    {
        draw->PushClipRect(canvas.lanes[i].Min, canvas.lanes[i].Max, true);
        DrawControlLane(draw, canvas, track, state, i);
        if (state.gesture.kind == KeyGesture::Kind::kLaneBox && state.gesture.lane == i)
        {
            const ImVec2 min = ImMin(state.gesture.from, mouse);
            const ImVec2 max = ImMax(state.gesture.from, mouse);
            draw->AddRectFilled(min, max, theme::WithAlpha(theme::kAccent, 0.12f));
            draw->AddRect(min, max, theme::WithAlpha(theme::kAccent, 0.7f), 0.0f, Px(1.0f));
        }
        draw->PopClipRect();
    }
    draw->PushClipRect(canvas.lane_label.Min, canvas.lane_label.Max, true);
    DrawLaneLabel(draw, canvas);
    draw->PopClipRect();
    const bool hovered = ImGui::IsWindowHovered();
    const bool on_add = hovered && canvas.add.Contains(mouse);
    for (std::size_t i = 0; i < canvas.lane_labels.size(); i++)
    {
        const ImRect& label = canvas.lane_labels[i];
        draw->PushClipRect(label.Min, label.Max, true);
        DrawControlLabel(draw, canvas, track, state, i, hovered && !on_add && label.Contains(mouse));
        draw->PopClipRect();
    }
    DrawAddLane(draw, canvas, on_add);
    draw->PushClipRect(canvas.keyboard.Min, canvas.keyboard.Max, true);
    DrawKeyboard(draw, canvas, track, state);
    draw->PopClipRect();
    draw->PushClipRect(canvas.ruler.Min, canvas.ruler.Max, true);
    DrawRuler(draw, canvas, transport, track.colour);
    draw->PopClipRect();
    const ImRect corner(inner.Min, ImVec2(inner.Min.x + keyboard, inner.Min.y + top));
    draw->AddRectFilled(corner.Min, corner.Max, theme::kRaised);
    draw->AddRectFilled(ImVec2(corner.Min.x, corner.Max.y - Px(1.0f)), corner.Max, theme::kBorder);
    draw->PushClipRect(ImVec2(canvas.notes.Min.x - Px(7.0f), canvas.ruler.Min.y),
                       ImVec2(canvas.lane.Max.x, LanesBottom(canvas)), true);
    DrawPlayhead(draw, canvas, transport);
    draw->PopClipRect();
    draw->PushClipRect(canvas.overview.Min, canvas.overview.Max, true);
    DrawOverview(draw, canvas, track, state);
    draw->PopClipRect();
}

// Turns the page of `canvas` when the playhead leaves it, unless the program set the scroll in this frame
// (`program_scroll`).
void TurnPage(const Canvas& canvas, const Transport& transport, KeyEditorState& state, bool program_scroll)
{
    const bool jumped = state.jumps.Jumped(transport) && !program_scroll;
    if (state.gesture.kind == KeyGesture::Kind::kNone)
    {
        const PageView page{canvas.scroll_x, canvas.notes.GetWidth(), Px(16.0f), Px(32.0f)};
        const float playhead = static_cast<float>(transport.Position()) * canvas.beat_width;
        if (const std::optional<float> turned = PageTurn(page, playhead, transport.Playing(), jumped))
        {
            ImGui::SetScrollX(*turned);
        }
    }
}

// Places the rectangles of `canvas` in `inner`: the ruler `ruler` high and the overview under it down to `top`, the
// keyboard `keyboard` wide, the notes, the lane of channels `channels` high, the velocity lane and `lane_count`
// controller lanes, each `lane` high, and the button that adds a lane.
void PlaceCanvasRects(Canvas& canvas, const ImRect& inner, float keyboard, float ruler, float top, float channels,
                      float lane, std::size_t lane_count)
{
    const float lanes_top = inner.Max.y - channels - lane * static_cast<float>(lane_count + 1);
    canvas.ruler = ImRect(ImVec2(inner.Min.x + keyboard, inner.Min.y), ImVec2(inner.Max.x, inner.Min.y + ruler));
    canvas.overview =
        ImRect(ImVec2(inner.Min.x + keyboard, inner.Min.y + ruler), ImVec2(inner.Max.x, inner.Min.y + top));
    canvas.keyboard = ImRect(ImVec2(inner.Min.x, inner.Min.y + top), ImVec2(inner.Min.x + keyboard, lanes_top));
    canvas.notes = ImRect(ImVec2(inner.Min.x + keyboard, inner.Min.y + top), ImVec2(inner.Max.x, lanes_top));
    // The lane of channels comes first, above the velocity lane.
    const float velocity_top = lanes_top + channels;
    if (canvas.has_channels)
    {
        canvas.channels = ImRect(ImVec2(inner.Min.x + keyboard, lanes_top), ImVec2(inner.Max.x, velocity_top));
        canvas.channels_label = ImRect(ImVec2(inner.Min.x, lanes_top), ImVec2(inner.Min.x + keyboard, velocity_top));
    }
    canvas.lane = ImRect(ImVec2(inner.Min.x + keyboard, velocity_top), ImVec2(inner.Max.x, velocity_top + lane));
    canvas.lane_label = ImRect(ImVec2(inner.Min.x, velocity_top), ImVec2(inner.Min.x + keyboard, velocity_top + lane));
    for (std::size_t i = 1; i <= lane_count; i++)
    {
        const float lane_top = velocity_top + lane * static_cast<float>(i);
        canvas.lanes.emplace_back(ImVec2(inner.Min.x + keyboard, lane_top), ImVec2(inner.Max.x, lane_top + lane));
        canvas.lane_labels.emplace_back(ImVec2(inner.Min.x, lane_top), ImVec2(inner.Min.x + keyboard, lane_top + lane));
    }
    const float add = Px(kAddSize);
    canvas.add = ImRect(ImVec2(inner.Min.x + Px(6.0f), inner.Max.y - Px(6.0f) - add),
                        ImVec2(inner.Min.x + Px(6.0f) + add, inner.Max.y - Px(6.0f)));
}

// The canvas: the ruler, the keyboard, the notes, the velocity lane and the controller lanes. Returns the edit of a
// gesture that ended.
std::optional<KeyEdit> DrawCanvas(const KeyTrack& track, KeyEditorState& state, Transport& transport, bool busy)
{
    // A width or a height from a file takes the nearest that the editor offers.
    state.beat_width = std::clamp(state.beat_width, kMinBeatWidth, kMaxBeatWidth);
    state.key_height = std::clamp(state.key_height, kMinKeyHeight, kMaxKeyHeight);

    // The content: the keyboard and the song across, and the ruler, the keys and the lanes down. Some room follows the
    // song's end. The lanes shrink to leave the notes some room.
    Canvas canvas;
    canvas.ticks_per_quarter = std::max(track.ticks_per_quarter, 1);
    canvas.beat_width = Px(state.beat_width);
    canvas.key_height = Px(state.key_height);
    const double quarters = static_cast<double>(track.song_length) / canvas.ticks_per_quarter;
    const float keyboard = Px(kKeyboardWidth);
    const float ruler = Px(kRulerHeight);
    const float top = ruler + Px(kOverviewHeight);
    canvas.has_channels = !track.channels.empty();
    const float channels =
        canvas.has_channels ? Px(1.0f) + static_cast<float>(ChannelRows(track.console)) * ChannelRowHeight() : 0.0f;
    const auto lanes = static_cast<float>(state.lanes.size() + 1);
    const float room =
        ImGui::GetContentRegionAvail().y - top - ImGui::GetStyle().ScrollbarSize - Px(kLeastNotesHeight) - channels;
    const float lane = std::clamp(room / lanes, Px(kLeastLaneHeight), Px(kLaneHeight));
    ImGui::SetNextWindowContentSize(
        ImVec2(keyboard + static_cast<float>(quarters + 16.0) * canvas.beat_width,
               top + static_cast<float>(kKeys) * canvas.key_height + channels + lane * lanes));
    ImGui::BeginChild("##keys", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None,
                      ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    const ImRect inner = ImGui::GetCurrentWindow()->InnerRect;
    canvas.scroll_x = ImGui::GetScrollX();
    canvas.scroll_y = ImGui::GetScrollY();
    PlaceCanvasRects(canvas, inner, keyboard, ruler, top, channels, lane, state.lanes.size());

    // A scroll that the program asked for: to the start of the part that the user opened, and to its keys.
    const bool program_scroll = state.scroll_tick || state.scroll_to;
    ApplyCanvasScrolls(canvas, state);
    state.left_quarter = canvas.scroll_x / canvas.beat_width;
    state.top_key = static_cast<float>(kKeys - 1) - canvas.scroll_y / canvas.key_height;

    // A zoom from the menu keeps the playhead in place. With the playhead out of sight, the middle stays in place. Zoom
    // Full shows the song from its start.
    ApplyCanvasZooms(canvas, state, transport, quarters);

    std::optional<KeyEdit> edit = HandleMouse(canvas, track, state, transport, busy);

    // The key and the tick under the mouse, for the toolbar's next frame.
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    ShowCanvasTooltips(canvas, track, state, mouse);

    // The rows and the grid, then the notes and the box of a selection over them. The lane, the keyboard and the ruler
    // go round them, and the playhead over them. The miniature covers the playhead's line.
    DrawCanvasLayers(canvas, track, state, transport, mouse, inner, keyboard, top);

    // Playback that runs off either edge turns the page. So does a jump of the position out of the notes, such as Go to
    // Project Start. A jump in the frame of a scroll that the program sets turns no page, such as the scroll to an
    // opened part's start.
    TurnPage(canvas, transport, state, program_scroll);
    DrawLaneMenu(track, state);
    ImGui::EndChild();

    return edit;
}

} // namespace

std::optional<KeyEdit> DrawKeyEditor(const KeyTrack& track, KeyEditorState& state, Transport& transport,
                                     const ShownItem& track_item, bool busy, bool* open)
{
    // A zone that docks with the mixer in the lower zone, and takes its place while it's open (see kKeyEditor). Its
    // header names the track. The track's parts take its name too. Opening a track brings the editor to the front.
    if (state.scroll_tick)
    {
        ImGui::SetNextWindowFocus();
    }
    ImGuiWindowClass zone;
    zone.DockNodeFlagsOverrideSet = ImGuiDockNodeFlags_NoTabBar;
    ImGui::SetNextWindowClass(&zone);
    DockWithMixer(kKeyEditor);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::ColorConvertU32ToFloat4(theme::kPanel));
    const bool visible =
        ImGui::Begin(kKeyEditor, open,
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    if (!visible)
    {
        ImGui::End();
        state.jumps.Jumped(transport);
        state.focused = false;
        state.gesture = KeyGesture();
        state.transposed.reset();
        state.sounding.reset();
        state.request = KeyRequest::kNone;
        state.typing = InfoTyping();
        return std::nullopt;
    }
    state.focused =
        ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) || (state.focused && MenuBarFocused());

    // The selection keeps the commands that the track still shows. A gesture whose note, point or lane has gone ends.
    std::set<uint32_t> shown;
    for (const KeyNote& note : track.notes)
    {
        shown.insert(note.command);
    }
    std::erase_if(state.selected, [&shown](uint32_t command) { return !shown.contains(command); });
    std::set<uint32_t> points;
    for (const KeyControl& control : track.controls)
    {
        points.insert(control.offset);
    }
    std::erase_if(state.selected_controls, [&points](uint32_t offset) { return !points.contains(offset); });
    if (GestureGone(state.gesture, track, state.lanes.size()))
    {
        state.gesture = KeyGesture();
    }

    ZoneHeader("Key Editor", track.name);
    DrawToolbar(state);
    std::optional<KeyEdit> edit = DrawInfoLine(track, state, track_item, busy);
    std::optional<KeyEdit> drawn = DrawCanvas(track, state, transport, busy);
    if (!edit)
    {
        edit = std::move(drawn);
    }
    if (!ImGui::IsKeyDown(ImGuiKey_UpArrow) && !ImGui::IsKeyDown(ImGuiKey_DownArrow))
    {
        state.transposed.reset();
    }
    if (!edit && !busy && state.focused && !MenuBarFocused())
    {
        edit = HandleKeys(track, state);
    }
    if (!edit && !busy && state.request != KeyRequest::kNone)
    {
        edit = RequestedEdit(track, state, transport.Position() * track.ticks_per_quarter);
    }
    state.sounding = SoundingNote(track, state);
    ImGui::End();

    return edit;
}

} // namespace hokuto::app
