// SPDX-License-Identifier: MIT

#include "app/key_editor.h"

#include <imgui_internal.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <map>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/commands/units.h"

#include "app/key_edits.h"
#include "app/lanes.h"
#include "app/list_edits.h"
#include "app/tempo_row.h"
#include "app/theme.h"
#include "app/views.h"
#include "app/widgets.h"
#include "edit/commands.h"

namespace hokuto::app
{
namespace
{

using ntdlib::commands::Fixed;
using ntdlib::commands::Source;
using theme::Px;

// The editor's sizes, before the display's scale. The lanes shrink to keep room for the notes, down to their least
// height.
constexpr float kToolbarHeight = 40.0f;
constexpr float kKeyboardWidth = 80.0f;
constexpr float kRulerHeight = 30.0f;
constexpr float kOverviewHeight = 18.0f;
constexpr float kLaneHeight = 88.0f;
constexpr float kLeastLaneHeight = 56.0f;
constexpr float kLeastNotesHeight = 96.0f;
constexpr float kAddSize = 18.0f;
constexpr float kToolSize = 28.0f;
constexpr float kEndGrab = 6.0f;
constexpr float kDragDistance = 3.0f;
constexpr float kLineGrab = 5.0f;
constexpr float kMinBeatWidth = 8.0f;
constexpr float kMaxBeatWidth = 640.0f;
constexpr float kMinKeyHeight = 6.0f;
constexpr float kMaxKeyHeight = 30.0f;
constexpr float kZoomStep = 1.25f;
constexpr float kWheelStep = 60.0f;

// The grid's note values that the toolbar offers.
constexpr std::array<int, 5> kGrids = {4, 8, 16, 32, 64};

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
    std::vector<ImRect> lanes;
    std::vector<ImRect> lane_labels;
    ImRect add;
    float beat_width = 0.0f;
    float key_height = 0.0f;
    float scroll_x = 0.0f;
    float scroll_y = 0.0f;
    int ticks_per_quarter = 48;
};

float XOf(const Canvas& canvas, double tick)
{
    return canvas.notes.Min.x + static_cast<float>(tick / canvas.ticks_per_quarter) * canvas.beat_width -
           canvas.scroll_x;
}

double TickAt(const Canvas& canvas, float x)
{
    return static_cast<double>(x - canvas.notes.Min.x + canvas.scroll_x) / canvas.beat_width * canvas.ticks_per_quarter;
}

// The top of the row of `key`. The highest key is at the top.
float RowTop(const Canvas& canvas, int key)
{
    return canvas.notes.Min.y + static_cast<float>(kKeys - 1 - key) * canvas.key_height - canvas.scroll_y;
}

int KeyAt(const Canvas& canvas, float y)
{
    const auto row = static_cast<int>(std::floor((y - canvas.notes.Min.y + canvas.scroll_y) / canvas.key_height));
    return std::clamp(kKeys - 1 - row, 0, kKeys - 1);
}

bool IsBlack(int key)
{
    const int pitch = key % 12;
    return pitch == 1 || pitch == 3 || pitch == 6 || pitch == 8 || pitch == 10;
}

// The rectangle of `note`, moved by `shift` ticks and `transpose` keys, and `resize` ticks longer. A note stays at
// least a tick long, and wide enough to see.
ImRect NoteRect(const Canvas& canvas, const KeyNote& note, int32_t shift, int transpose, int32_t resize)
{
    const double start = static_cast<double>(note.start) + shift;
    const double length = std::max(static_cast<double>(note.length) + resize, 1.0);
    const float left = std::floor(XOf(canvas, start));
    const float right = std::max(std::floor(XOf(canvas, start + length)), left + Px(3.0f));
    const float top = RowTop(canvas, note.key + transpose);
    return ImRect(ImVec2(left, top + Px(1.0f)), ImVec2(right, top + canvas.key_height - Px(1.0f)));
}

// The note under `pos`: the last one drawn there. Nothing when there's none.
std::optional<std::size_t> NoteAt(const Canvas& canvas, const KeyTrack& track, ImVec2 pos)
{
    for (std::size_t i = track.notes.size(); i-- > 0;)
    {
        if (NoteRect(canvas, track.notes[i], 0, 0, 0).Contains(pos))
        {
            return i;
        }
    }

    return std::nullopt;
}

// The note whose line in the velocity lane lies nearest `x`, within reach of the mouse. A selected note comes first.
std::optional<std::size_t> LineAt(const Canvas& canvas, const KeyTrack& track, const KeyEditorState& state, float x)
{
    std::optional<std::size_t> nearest;
    float best = Px(kLineGrab);
    for (std::size_t i = 0; i < track.notes.size(); i++)
    {
        const float distance = std::abs(XOf(canvas, track.notes[i].start) - x);
        const bool selected = state.selected.contains(track.notes[i].command);
        const bool nearest_selected = nearest && state.selected.contains(track.notes[*nearest].command);
        if (distance < best || (distance <= best && selected && !nearest_selected))
        {
            nearest = i;
            best = distance;
        }
    }

    return nearest;
}

// The heights of the top and the bottom of the values in `lane`, a controller lane or the velocity lane.
float LaneTop(const ImRect& lane)
{
    return lane.Min.y + Px(10.0f);
}

float LaneBottom(const ImRect& lane)
{
    return lane.Max.y - Px(4.0f);
}

// The height of `value` in a controller lane that spans `range`.
float ValueY(const ImRect& lane, const edit::ValueRange& range, Fixed value)
{
    const double amount = static_cast<double>(value - range.lowest) /
                          static_cast<double>(std::max<Fixed>(range.highest - range.lowest, 1));
    return LaneBottom(lane) - (LaneBottom(lane) - LaneTop(lane)) * static_cast<float>(std::clamp(amount, 0.0, 1.0));
}

// The value at height `y` in a controller lane of `setting` that spans `range`: the nearest that the console holds.
Fixed ValueAt(const ImRect& lane, const KeyTrack& track, LaneSetting setting, const edit::ValueRange& range, float y)
{
    const double amount = static_cast<double>((LaneBottom(lane) - y) / (LaneBottom(lane) - LaneTop(lane)));
    const double value = static_cast<double>(range.lowest) + amount * static_cast<double>(range.highest - range.lowest);
    return NearestValue(track.console, setting, static_cast<Fixed>(std::llround(value)), range);
}

// The heights that a point of `control` covers in a controller lane that spans `range`: its value's height, a random
// value's range, or the whole lane for a variable's value.
std::pair<float, float> PointHeights(const ImRect& lane, const edit::ValueRange& range, const KeyControl& control)
{
    const ntdlib::commands::Argument& value = control.command.value;
    switch (value.source)
    {
    case Source::kValue:
        return {ValueY(lane, range, value.value), ValueY(lane, range, value.value)};
    case Source::kRandom:
        return {ValueY(lane, range, value.high), ValueY(lane, range, value.value)};
    case Source::kVariable:
        break;
    }

    return {LaneTop(lane), LaneBottom(lane)};
}

// The point of controller lane `lane` under `pos`, by its command's index in `track.controls`: the nearest in time
// within reach of the mouse, and of those at the same time the nearest in height. A selected point comes first. Nothing
// when there's none.
std::optional<std::size_t> PointAt(const Canvas& canvas, const KeyTrack& track, const KeyEditorState& state,
                                   std::size_t lane, ImVec2 pos)
{
    const LaneSetting setting = state.lanes[lane];
    const edit::ValueRange range = LaneRange(track, setting);
    std::optional<std::size_t> nearest;
    float best_x = Px(kLineGrab);
    float best_y = std::numeric_limits<float>::max();
    bool best_selected = false;
    for (const LanePoint& point : LaneOf(track, setting).points)
    {
        const std::size_t i = point.control;
        const KeyControl& control = track.controls[i];
        const float x = std::abs(XOf(canvas, point.tick) - pos.x);
        const auto [top, bottom] = PointHeights(canvas.lanes[lane], range, control);
        const float y = pos.y < top ? top - pos.y : std::max(pos.y - bottom, 0.0f);
        const bool selected = state.selected_controls.contains(control.offset);
        const bool closer = x < best_x || (x == best_x && (y < best_y || (y == best_y && selected && !best_selected)));
        if (x <= Px(kLineGrab) && (!nearest || closer))
        {
            nearest = i;
            best_x = x;
            best_y = y;
            best_selected = selected;
        }
    }

    return nearest;
}

// Places the next control of the toolbar at `x`, in the middle of the bar's height.
void PlaceInToolbar(float x, float top, float height)
{
    ImGui::SetCursorScreenPos(ImVec2(x, top + (Px(kToolbarHeight) - height) * 0.5f));
}

// The toolbar: Acoustic Feedback, the tools, the snap and its grid, and the velocity of drawn notes.
void DrawToolbar(KeyEditorState& state)
{
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const float bottom = origin.y + Px(kToolbarHeight);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(origin, ImVec2(origin.x + width, bottom), theme::kRaised);
    draw->AddRectFilled(ImVec2(origin.x, bottom - Px(1.0f)), ImVec2(origin.x + width, bottom), theme::kBorder);

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
    float x = origin.x + Px(10.0f);
    PlaceInToolbar(x, origin.y, button.y);
    if (IconButton("##feedback", Icon::kSound, button, state.feedback, theme::kAccent, "Acoustic Feedback"))
    {
        state.feedback = !state.feedback;
    }
    x += button.x + Px(14.0f);
    for (const Tool& tool : kTools)
    {
        PlaceInToolbar(x, origin.y, button.y);
        ImGui::PushID(static_cast<int>(tool.tool));
        if (IconButton("##tool", tool.icon, button, state.tool == tool.tool, theme::kAccent, tool.tip))
        {
            state.tool = tool.tool;
        }
        ImGui::PopID();
        x += button.x + Px(2.0f);
    }

    // The snap, and the grid that it snaps to.
    x += Px(14.0f);
    PlaceInToolbar(x, origin.y, button.y);
    if (IconButton("##snap", Icon::kSnap, button, state.snap, theme::kAccent, "Snap On/Off"))
    {
        state.snap = !state.snap;
    }
    x += button.x + Px(6.0f);
    PlaceInToolbar(x, origin.y, ImGui::GetFrameHeight());
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

    // The velocity of the notes that the Draw tool makes.
    x += Px(76.0f) + Px(20.0f);
    const ImVec2 caps = CapsSize("INSERT VELOCITY");
    DrawCaps(draw, ImVec2(x, origin.y + (Px(kToolbarHeight) - caps.y) * 0.5f), theme::kTextMuted, "INSERT VELOCITY");
    x += caps.x + Px(8.0f);
    PlaceInToolbar(x, origin.y, ImGui::GetFrameHeight());
    ValueSlider("##velocity", &state.insert_velocity, SliderRange{1.0f, kTopVelocity}, 100.0f, "%.0f",
                ImVec2(Px(64.0f), ImGui::GetFrameHeight()));
    state.insert_velocity = std::clamp(std::round(state.insert_velocity), 1.0f, static_cast<float>(kTopVelocity));

    // The notes' colours.
    x += Px(64.0f) + Px(20.0f);
    const ImVec2 colours_caps = CapsSize("COLOURS");
    DrawCaps(draw, ImVec2(x, origin.y + (Px(kToolbarHeight) - colours_caps.y) * 0.5f), theme::kTextMuted, "COLOURS");
    x += colours_caps.x + Px(8.0f);
    PlaceInToolbar(x, origin.y, ImGui::GetFrameHeight());
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

    // The key and the position under the mouse, at the bar's right end.
    if (state.pointer)
    {
        ImFont* figures = theme::GetFonts().figures;
        const std::string value =
            KeyName(state.pointer->first) + "   " + PositionText(state.pointer->second, kBeatsPerBar);
        const ImVec2 size = TextSize(figures, theme::kTextSize, value);
        const float right = origin.x + width - Px(14.0f);
        const float middle = origin.y + Px(kToolbarHeight) * 0.5f;
        const ImVec2 mouse_caps = CapsSize("MOUSE");
        const float left = right - size.x - Px(8.0f) - mouse_caps.x;
        if (left > x + Px(96.0f) + Px(20.0f))
        {
            DrawCaps(draw, ImVec2(left, middle - mouse_caps.y * 0.5f), theme::kTextMuted, "MOUSE");
            DrawText(draw, figures, theme::kTextSize, ImVec2(right - size.x, middle - size.y * 0.5f), theme::kText,
                     value);
        }
    }

    ImGui::SetCursorScreenPos(ImVec2(origin.x, bottom));
}

// The edit of `text`, typed as the value of the point of `track.controls[index]`. The point takes the typed value, and
// the other selected points of its setting change by the same amount (LaneValueEdit). A random value or a variable's
// value goes to the point alone. When the console can't hold the value, the edit says why. Text that the field can't
// read is refused with what the field takes.
std::optional<KeyEdit> TypedControlEdit(const KeyTrack& track, const KeyEditorState& state, std::size_t index,
                                        std::string_view text)
{
    const KeyControl& control = track.controls[index];
    const std::optional<TypedArgument> typed = TypedValueOf(track.console, control.command, text);
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
std::optional<KeyEdit> DrawControlInfo(const KeyTrack& track, KeyEditorState& state, std::size_t index, bool busy,
                                       InfoLine& line)
{
    const KeyControl& control = track.controls[index];
    const double quarter = std::max(track.ticks_per_quarter, 1);
    const std::string start = PositionText(control.tick / quarter, kBeatsPerBar);
    const std::string label = LaneName(SettingOf(control.command).value_or(LaneSetting{}));
    const std::string value = ArgumentText(control.command.kind, control.command.value);
    if (busy)
    {
        InfoField(line, "START", start);
        InfoField(line, label.c_str(), value);
        return std::nullopt;
    }

    std::optional<KeyEdit> edit;
    const std::optional<std::string> typed_start = InfoEntry(line, "START", start, state.typing);
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
    const std::optional<std::string> typed_value = InfoEntry(line, label.c_str(), value, state.typing);
    if (typed_value && *typed_value != value)
    {
        edit = TypedControlEdit(track, state, index, *typed_value);
    }

    return edit;
}

// The info line shows the values of the first selected note. A click on a value types a new one (TypedEdit), except
// while `busy`. Without a selected note, the line shows the first selected point of the controller lanes. Without a
// selection, the line says so. Returns the edit of a typed value.
std::optional<KeyEdit> DrawInfoLine(const KeyTrack& track, KeyEditorState& state, bool busy)
{
    InfoLine line = BeginInfoLine();
    const auto first = std::find_if(track.notes.begin(), track.notes.end(),
                                    [&state](const KeyNote& note) { return state.selected.contains(note.command); });
    const auto control = std::find_if(track.controls.begin(), track.controls.end(), [&state](const KeyControl& point)
                                      { return state.selected_controls.contains(point.offset); });
    if (first == track.notes.end() && control != track.controls.end())
    {
        std::optional<KeyEdit> edit =
            DrawControlInfo(track, state, static_cast<std::size_t>(control - track.controls.begin()), busy, line);
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
    std::optional<KeyEdit> edit;
    for (const auto& [value, label, text] : fields)
    {
        if (busy)
        {
            InfoField(line, label, text);
            continue;
        }

        const std::optional<std::string> typed = InfoEntry(line, label, text, state.typing);
        if (typed && *typed != text)
        {
            edit = TypedEdit(track, state, value, *typed);
        }
    }
    EndInfoLine(line);

    return edit;
}

// Zooms the time by `factor`. The time at `anchor_x` stays where it is on the screen.
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

// Zooms the keys by `factor`. The key at `anchor_y` stays where it is on the screen.
void ZoomKeys(Canvas& canvas, KeyEditorState& state, float factor, float anchor_y)
{
    const float rows = (anchor_y - canvas.notes.Min.y + canvas.scroll_y) / canvas.key_height;
    state.key_height = std::clamp(state.key_height * factor, kMinKeyHeight, kMaxKeyHeight);
    canvas.key_height = Px(state.key_height);
    canvas.scroll_y = std::max(rows * canvas.key_height - (anchor_y - canvas.notes.Min.y), 0.0f);
    ImGui::SetScrollY(canvas.scroll_y);
}

// The rows of the keys, with a line under each B. Black keys' rows are darker.
void DrawRows(ImDrawList* draw, const Canvas& canvas)
{
    const int highest = KeyAt(canvas, canvas.notes.Min.y);
    const int lowest = KeyAt(canvas, canvas.notes.Max.y);
    for (int key = lowest; key <= highest; key++)
    {
        const float top = RowTop(canvas, key);
        draw->AddRectFilled(ImVec2(canvas.notes.Min.x, top), ImVec2(canvas.notes.Max.x, top + canvas.key_height),
                            IsBlack(key) ? theme::kPanel : theme::kPanelAlt);
        if (key % 12 == 0)
        {
            draw->AddRectFilled(ImVec2(canvas.notes.Min.x, top + canvas.key_height - Px(1.0f)),
                                ImVec2(canvas.notes.Max.x, top + canvas.key_height), theme::kBorderStrong);
        }
    }
}

// A line across `area` at each beat, and fainter lines at the grid's steps when they're far enough apart.
void DrawGrid(ImDrawList* draw, const Canvas& canvas, const KeyEditorState& state, const ImRect& area)
{
    const double step = GridTicks(state, canvas.ticks_per_quarter);
    const bool steps = step / canvas.ticks_per_quarter * canvas.beat_width >= Px(8.0f);
    const double first = std::max(std::floor(TickAt(canvas, area.Min.x) / step), 0.0);
    const double last = std::ceil(TickAt(canvas, area.Max.x) / step);
    for (double line = first; line <= last; line++)
    {
        const double tick = line * step;
        const bool beat = std::fmod(tick, canvas.ticks_per_quarter) < 1e-6;
        if (!beat && !steps)
        {
            continue;
        }

        const float x = std::floor(XOf(canvas, tick));
        draw->AddRectFilled(ImVec2(x, area.Min.y), ImVec2(x + Px(1.0f), area.Max.y),
                            beat ? theme::kGridBeatOnly : theme::kGridBeat);
    }
}

// The ruler matches the arrangement's: the range between the locators in its upper half, and the beats numbered from
// 1. A strip in the track's colour runs along its bottom.
void DrawRuler(ImDrawList* draw, const Canvas& canvas, const Transport& transport, ImU32 colour)
{
    const ImRect& ruler = canvas.ruler;
    const double quarter = canvas.ticks_per_quarter;
    draw->AddRectFilled(ruler.Min, ruler.Max, theme::kRaised);
    const ImU32 range =
        transport.Cycle() ? theme::WithAlpha(theme::kCycle, 0.9f) : theme::WithAlpha(theme::kTextDim, 0.4f);
    draw->AddRectFilled(ImVec2(XOf(canvas, transport.CycleStart() * quarter), ruler.Min.y + Px(3.0f)),
                        ImVec2(XOf(canvas, transport.CycleEnd() * quarter), ruler.Min.y + Px(11.0f)), range, Px(3.0f));
    draw->AddRectFilled(ImVec2(ruler.Min.x, ruler.Max.y - Px(3.0f)), ruler.Max, theme::WithAlpha(colour, 0.85f));

    // A number and a long mark at each labelled beat, and short marks at the others.
    const int step = BeatStep(canvas.beat_width);
    const int first = std::max(static_cast<int>(std::floor(TickAt(canvas, ruler.Min.x) / quarter)) - 1, 0);
    const int last = static_cast<int>(std::ceil(TickAt(canvas, ruler.Max.x) / quarter));
    ImFont* figures = theme::GetFonts().figures;
    for (int beat = first; beat <= last; beat++)
    {
        const float x = std::floor(XOf(canvas, beat * quarter));
        if (LabelledBeat(beat, step))
        {
            std::array<char, 16> label{};
            std::snprintf(label.data(), label.size(), "%d", beat + 1);
            draw->AddRectFilled(ImVec2(x, ruler.Max.y - Px(16.0f)), ImVec2(x + Px(1.0f), ruler.Max.y - Px(3.0f)),
                                theme::kTextDim);
            DrawText(draw, figures, theme::kSmallSize, ImVec2(x + Px(4.0f), ruler.Max.y - Px(19.0f)), theme::kTextMuted,
                     label.data());
        }
        else if (canvas.beat_width >= Px(4.0f))
        {
            draw->AddRectFilled(ImVec2(x, ruler.Max.y - Px(8.0f)), ImVec2(x + Px(1.0f), ruler.Max.y - Px(3.0f)),
                                theme::kBorderStrong);
        }
    }
}

// The keyboard down the left side, drawn as a piano's: a white key reaches into the rows of the black keys beside it.
// The keys of the selected notes and the key that sounds light up in the track's colour. Each C has its name.
void DrawKeyboard(ImDrawList* draw, const Canvas& canvas, const KeyTrack& track, const KeyEditorState& state)
{
    std::array<bool, kKeys> lit{};
    for (const KeyNote& note : track.notes)
    {
        if (state.selected.contains(note.command))
        {
            lit[static_cast<std::size_t>(note.key)] = true;
        }
    }
    if (const std::optional<KeyNote> sounding = SoundingNote(track, state))
    {
        lit[static_cast<std::size_t>(sounding->key)] = true;
    }

    // The white keys, each with a line along its bottom edge, then the black keys over them.
    const ImU32 kWhite = theme::Mix(theme::kText, theme::kPanel, 0.2f);
    const ImU32 kBlack = theme::kWindow;
    const ImU32 kEdge = theme::Mix(kWhite, kBlack, 0.45f);
    const ImRect& keys = canvas.keyboard;
    const float black_right = keys.Min.x + std::floor(keys.GetWidth() * 0.6f);
    const int highest = std::min(KeyAt(canvas, keys.Min.y) + 1, kKeys - 1);
    const int lowest = std::max(KeyAt(canvas, keys.Max.y) - 1, 0);

    const auto middle = [&canvas](int key)
    {
        return std::floor(RowTop(canvas, key) + canvas.key_height * 0.5f);
    };

    ImFont* text = theme::GetFonts().text;
    for (int key = lowest; key <= highest; key++)
    {
        if (IsBlack(key))
        {
            continue;
        }

        const float top = key + 1 < kKeys && IsBlack(key + 1) ? middle(key + 1) : RowTop(canvas, key);
        const float bottom = key > 0 && IsBlack(key - 1) ? middle(key - 1) : RowTop(canvas, key) + canvas.key_height;
        const ImU32 colour = lit[static_cast<std::size_t>(key)] ? track.colour : kWhite;
        draw->AddRectFilled(ImVec2(keys.Min.x, top), ImVec2(keys.Max.x, bottom), colour);
        draw->AddRectFilled(ImVec2(keys.Min.x, bottom - Px(1.0f)), ImVec2(keys.Max.x, bottom), kEdge);
        if (key % 12 == 0 && canvas.key_height >= Px(9.0f))
        {
            const std::string name = KeyName(key);
            const ImVec2 size = TextSize(text, theme::kSmallSize, name);
            DrawText(draw, text, theme::kSmallSize,
                     ImVec2(keys.Max.x - size.x - Px(7.0f), std::floor((top + bottom - size.y) * 0.5f)),
                     theme::TextOn(colour), name);
        }
    }
    for (int key = lowest; key <= highest; key++)
    {
        if (IsBlack(key))
        {
            const float top = RowTop(canvas, key);
            const ImU32 colour = lit[static_cast<std::size_t>(key)] ? track.colour : kBlack;
            draw->AddRectFilled(ImVec2(keys.Min.x, top), ImVec2(black_right, top + canvas.key_height), colour, Px(2.0f),
                                ImDrawFlags_RoundCornersRight);
        }
    }
    draw->AddRectFilled(ImVec2(keys.Max.x - Px(1.0f), keys.Min.y), keys.Max, theme::kBorder);
}

// A note is a bar in the track's colour. A selected note turns dark, with an outline in the colour.
void DrawNote(ImDrawList* draw, const ImRect& rect, ImU32 colour, bool selected, bool ghost)
{
    if (ghost)
    {
        draw->AddRect(rect.Min, rect.Max, theme::WithAlpha(colour, 0.5f), Px(2.0f), Px(1.0f));
        return;
    }

    const float rounding = Px(2.0f);
    if (selected)
    {
        draw->AddRectFilled(rect.Min, rect.Max, theme::Mix(colour, theme::kWindow, 0.6f), rounding);
        draw->AddRect(rect.Min, rect.Max, colour, rounding, Px(1.5f));
    }
    else
    {
        draw->AddRectFilled(rect.Min, rect.Max, colour, rounding);
        draw->AddRect(rect.Min, rect.Max, theme::Mix(colour, theme::kWindow, 0.35f), rounding, Px(1.0f));
    }
}

// The colour of a note of `velocity` (1 to 127) while the notes take their velocities' colours: from blue for the
// softest through green and yellow to red for the loudest.
ImU32 VelocityColour(int velocity)
{
    // The stops, at velocities 1, 32, 64, 96, 112 and 127, in the tracks' colours.
    struct Stop
    {
        int velocity;
        ImU32 colour;
    };
    static constexpr std::array<Stop, 6> kStops = {{
        {1, theme::kTrackColours[0]},
        {32, theme::kTrackColours[1]},
        {64, theme::kTrackColours[2]},
        {96, theme::kTrackColours[4]},
        {112, theme::kTrackColours[5]},
        {127, theme::kTrackColours[6]},
    }};
    const int clamped = std::clamp(velocity, kStops.front().velocity, kStops.back().velocity);
    for (std::size_t i = 1; i < kStops.size(); i++)
    {
        if (clamped <= kStops[i].velocity)
        {
            const Stop& low = kStops[i - 1];
            const Stop& high = kStops[i];
            const float amount =
                static_cast<float>(clamped - low.velocity) / static_cast<float>(high.velocity - low.velocity);
            return theme::Mix(low.colour, high.colour, amount);
        }
    }

    return kStops.back().colour;
}

// The colour of a note of `velocity`: the track's, or the velocity's while the notes take their velocities' colours.
ImU32 NoteColour(const KeyTrack& track, const KeyEditorState& state, int velocity)
{
    return state.colours == NoteColours::kVelocity ? VelocityColour(velocity) : track.colour;
}

// The ticks that the miniature spans: the song, and any note that lasts past the song's end.
double OverviewTicks(const KeyTrack& track)
{
    double end = track.song_length;
    for (const KeyNote& note : track.notes)
    {
        end = std::max(end, static_cast<double>(note.start) + note.length);
    }

    return end;
}

// The miniature of the track under the ruler: every note over the whole song, and a frame round the part of the song in
// view. Selected notes are light. The notes fill the strip's height, from the track's lowest key to its highest.
void DrawOverview(ImDrawList* draw, const Canvas& canvas, const KeyTrack& track, const KeyEditorState& state)
{
    const ImRect& strip = canvas.overview;
    draw->AddRectFilled(strip.Min, strip.Max, theme::kDisplay);
    draw->AddRectFilled(ImVec2(strip.Min.x, strip.Max.y - Px(1.0f)), strip.Max, theme::kBorder);
    const double total = OverviewTicks(track);
    if (total <= 0.0)
    {
        return;
    }

    const auto x_of = [&strip, total](double tick)
    {
        return strip.Min.x + static_cast<float>(tick / total) * strip.GetWidth();
    };

    const auto by_key = [](const KeyNote& a, const KeyNote& b)
    {
        return a.key < b.key;
    };
    const auto [lowest, highest] = std::minmax_element(track.notes.begin(), track.notes.end(), by_key);
    if (lowest != track.notes.end())
    {
        const int range = highest->key - lowest->key;
        const float top = strip.Min.y + Px(3.0f);
        const float span = strip.Max.y - Px(4.0f) - top;
        const float bar = std::clamp(span / static_cast<float>(range + 1), Px(1.0f), Px(3.0f));
        for (const KeyNote& note : track.notes)
        {
            const float place = range == 0 ? 0.5f : static_cast<float>(highest->key - note.key) / range;
            const float y = top + place * (span - bar);
            const float left = x_of(note.start);
            const float right = std::max(x_of(static_cast<double>(note.start) + note.length), left + Px(1.0f));
            const ImU32 colour =
                state.selected.contains(note.command) ? theme::kText : NoteColour(track, state, note.velocity);
            draw->AddRectFilled(ImVec2(left, y), ImVec2(right, y + bar), colour);
        }
    }

    // The frame round the part in view.
    const float from = std::clamp(x_of(TickAt(canvas, canvas.notes.Min.x)), strip.Min.x, strip.Max.x);
    const float to = std::clamp(x_of(TickAt(canvas, canvas.notes.Max.x)), from, strip.Max.x);
    const ImVec2 min(from, strip.Min.y + Px(1.0f));
    const ImVec2 max(std::max(to, from + Px(2.0f)), strip.Max.y - Px(2.0f));
    draw->AddRectFilled(min, max, theme::WithAlpha(theme::kText, 0.08f), Px(2.0f));
    draw->AddRect(min, max, theme::WithAlpha(theme::kText, 0.55f), Px(2.0f), Px(1.0f));
}

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

// The notes, with the gesture's changes to the selected ones. The notes of a move leave outlines where they were. A
// size shows each selected command's new length.
void DrawNotes(ImDrawList* draw, const Canvas& canvas, const KeyTrack& track, const KeyEditorState& state)
{
    const KeyGesture& gesture = state.gesture;
    const bool moving = gesture.kind == KeyGesture::Kind::kMove;
    const bool sizing = gesture.kind == KeyGesture::Kind::kSize && gesture.resize != 0;
    const std::optional<std::map<uint32_t, uint32_t>> lengths =
        sizing ? BaseLengths(track, state) : std::optional<std::map<uint32_t, uint32_t>>();
    for (const KeyNote& note : track.notes)
    {
        const bool selected = state.selected.contains(note.command);
        const int32_t shift = selected && moving ? gesture.shift : 0;
        const int transpose = selected && moving ? gesture.transpose : 0;
        int32_t resize = 0;
        if (selected && lengths)
        {
            const auto base = lengths->find(note.command);
            const int64_t length = base != lengths->end() ? int64_t{base->second} : int64_t{note.length};
            resize = static_cast<int32_t>(std::max<int64_t>(length + gesture.resize, 1) - note.length);
        }
        const ImRect rect = NoteRect(canvas, note, shift, transpose, resize);
        if (rect.Max.x < canvas.notes.Min.x || rect.Min.x > canvas.notes.Max.x || rect.Max.y < canvas.notes.Min.y ||
            rect.Min.y > canvas.notes.Max.y)
        {
            continue;
        }

        const ImU32 colour = NoteColour(track, state, note.velocity);
        if (selected && moving && (shift != 0 || transpose != 0))
        {
            DrawNote(draw, NoteRect(canvas, note, 0, 0, 0), colour, false, true);
        }
        DrawNote(draw, rect, colour, selected, false);
    }

    if (gesture.kind == KeyGesture::Kind::kDraw)
    {
        DrawNote(draw, NoteRect(canvas, gesture.drawn, 0, 0, 0), NoteColour(track, state, gesture.drawn.velocity), true,
                 false);
    }
}

// The velocity lane, on the grid. Each note has a line at its start, and the line's height gives the velocity. A drag
// changes the velocities.
void DrawLane(ImDrawList* draw, const Canvas& canvas, const KeyTrack& track, const KeyEditorState& state)
{
    draw->AddRectFilled(canvas.lane.Min, canvas.lane.Max, theme::kPanel);
    DrawGrid(draw, canvas, state, canvas.lane);
    draw->AddRectFilled(canvas.lane.Min, ImVec2(canvas.lane.Max.x, canvas.lane.Min.y + Px(1.0f)), theme::kBorderStrong);

    // The gesture's change of velocity: the pressed note's new value, and the same change to the other selected notes.
    const KeyGesture& gesture = state.gesture;
    const bool changing = gesture.kind == KeyGesture::Kind::kVelocity;
    const int pressed = changing ? track.notes[gesture.note].velocity : 0;
    const bool together = changing && state.selected.contains(track.notes[gesture.note].command);
    const float top = canvas.lane.Min.y + Px(10.0f);
    const float bottom = canvas.lane.Max.y - Px(4.0f);
    for (const KeyNote& note : track.notes)
    {
        const float x = std::floor(XOf(canvas, note.start));
        if (x < canvas.lane.Min.x - Px(4.0f) || x > canvas.lane.Max.x)
        {
            continue;
        }

        const bool selected = state.selected.contains(note.command);
        int velocity = note.velocity;
        if (changing && (note.command == track.notes[gesture.note].command || (together && selected)))
        {
            velocity = std::clamp(note.velocity + gesture.velocity - pressed, 1, kTopVelocity);
        }
        const float y = bottom - (bottom - top) * static_cast<float>(velocity) / kTopVelocity;
        const ImU32 base = NoteColour(track, state, velocity);
        const ImU32 colour = selected ? theme::Mix(base, theme::kText, 0.45f) : base;
        draw->AddRectFilled(ImVec2(x, y), ImVec2(x + Px(2.0f), bottom), colour);
        draw->AddRectFilled(ImVec2(x - Px(2.0f), y - Px(1.0f)), ImVec2(x + Px(4.0f), y + Px(1.0f)), colour);
    }
}

// The lane's label, beside the lane under the keyboard.
void DrawLaneLabel(ImDrawList* draw, const Canvas& canvas)
{
    const ImRect& label = canvas.lane_label;
    draw->AddRectFilled(label.Min, label.Max, theme::kRaised);
    draw->AddRectFilled(label.Min, ImVec2(label.Max.x, label.Min.y + Px(1.0f)), theme::kBorderStrong);
    DrawCaps(draw, ImVec2(label.Min.x + Px(10.0f), label.Min.y + Px(10.0f)), theme::kTextMuted, "Velocity");
}

// What the tooltip of a point of a controller lane says: the setting and its value, the command's start, the time over
// which the value moves, and whether the command runs only if the last comparison held.
std::string PointText(const KeyTrack& track, const KeyControl& control)
{
    const ntdlib::commands::Command& command = control.command;
    const double quarter = std::max(track.ticks_per_quarter, 1);
    std::string text = LaneName(SettingOf(command).value_or(LaneSetting{command.kind})) + " " +
                       ArgumentText(command.kind, command.value) + "\nStart " +
                       WithoutFigureSpaces(PositionText(control.tick / quarter, kBeatsPerBar));
    if (command.time && command.time->source == Source::kValue)
    {
        text += "\nMoves over " +
                LengthText(static_cast<double>(command.time->value) / ntdlib::commands::kOne / quarter, kBeatsPerBar);
    }
    if (command.conditional)
    {
        text += "\nRuns only if the last comparison held";
    }

    return text;
}

// The commands of the lanes' settings as the gesture under way leaves them, in the order of their ticks. A drag of a
// point shows the ticks and the values that its edit gives the points (LanePointEdit).
std::vector<KeyControl> ShownControls(const KeyTrack& track, const KeyEditorState& state)
{
    std::vector<KeyControl> controls = track.controls;
    const KeyGesture& gesture = state.gesture;
    if (gesture.kind != KeyGesture::Kind::kControl || gesture.control >= track.controls.size())
    {
        return controls;
    }

    const std::optional<KeyEdit> edit = LanePointEdit(track, state, gesture.control, gesture.shift, gesture.value);
    if (!edit)
    {
        return controls;
    }

    for (KeyControl& control : controls)
    {
        const auto changed = edit->changed_controls.find(control.offset);
        if (changed != edit->changed_controls.end())
        {
            control.command = changed->second;
        }
        const auto moved = edit->moved_controls.find(control.offset);
        if (moved != edit->moved_controls.end())
        {
            control.tick = static_cast<uint32_t>(int64_t{control.tick} + moved->second);
        }
    }
    std::ranges::stable_sort(controls, {}, &KeyControl::tick);

    return controls;
}

// Controller lane `lane`, on the grid. The line of the setting's values runs through the points, filled below it. The
// value that the console starts a track with is fainter. A value from a random number or a variable shades the lane up
// to the next point, and names its source where the name fits.
void DrawControlLane(ImDrawList* draw, const Canvas& canvas, const KeyTrack& track, const KeyEditorState& state,
                     std::size_t lane)
{
    const ImRect& rect = canvas.lanes[lane];
    const LaneSetting setting = state.lanes[lane];
    draw->AddRectFilled(rect.Min, rect.Max, theme::kPanel);
    DrawGrid(draw, canvas, state, rect);
    draw->AddRectFilled(rect.Min, ImVec2(rect.Max.x, rect.Min.y + Px(1.0f)), theme::kBorderStrong);

    // A lane kept from a sound of another console can show a setting that this console's player skips.
    if (!Holds(track.console, setting))
    {
        DrawText(draw, theme::GetFonts().text, theme::kSmallSize,
                 ImVec2(rect.Min.x + Px(12.0f), LaneTop(rect) + Px(4.0f)), theme::kTextMuted,
                 "This console's player skips such a setting. The lane's menu offers the others.");
        return;
    }

    // The line keeps the lane's range during a drag.
    const edit::ValueRange range = LaneRange(track, setting);
    KeyTrack shown;
    shown.console = track.console;
    shown.song_length = track.song_length;
    shown.controls = ShownControls(track, state);
    const Lane shape = LaneOf(shown, setting);
    const std::vector<LanePiece>& line = shape.line;
    std::optional<uint32_t> first;
    if (!shape.points.empty())
    {
        first = shown.controls[shape.points.front().control].tick;
    }
    const ImU32 colour = track.colour;
    const float bottom = LaneBottom(rect);
    for (std::size_t i = 0; i < line.size(); i++)
    {
        const LanePiece& piece = line[i];
        const float left = std::floor(XOf(canvas, piece.start));
        const float right = std::floor(XOf(canvas, piece.end));
        if (right < rect.Min.x || left > rect.Max.x)
        {
            continue;
        }

        if (!piece.from || !piece.to)
        {
            draw->AddRectFilled(ImVec2(left, LaneTop(rect)), ImVec2(right, bottom), theme::WithAlpha(colour, 0.1f));
            continue;
        }

        const bool lead_in = !first || piece.end <= *first;
        const float from = ValueY(rect, range, *piece.from);
        const float to = ValueY(rect, range, *piece.to);
        draw->AddQuadFilled(ImVec2(left, from), ImVec2(right, to), ImVec2(right, bottom), ImVec2(left, bottom),
                            theme::WithAlpha(colour, lead_in ? 0.06f : 0.16f));
        const ImU32 stroke = theme::WithAlpha(colour, lead_in ? 0.45f : 1.0f);
        draw->AddLine(ImVec2(left, from), ImVec2(right, to), stroke, Px(1.5f));
        if (i > 0 && line[i - 1].to && line[i - 1].end == piece.start)
        {
            draw->AddLine(ImVec2(left, ValueY(rect, range, *line[i - 1].to)), ImVec2(left, from), stroke, Px(1.5f));
        }
    }

    // The points, with the source of each value that has no one height where it fits (FittingLabels).
    ImFont* font = theme::GetFonts().text;
    std::vector<float> xs;
    std::vector<float> widths;
    std::vector<std::string> labels;
    for (const LanePoint& point : shape.points)
    {
        const KeyControl& control = shown.controls[point.control];
        xs.push_back(std::floor(XOf(canvas, point.tick)));
        const bool shown_point = xs.back() >= rect.Min.x - Px(8.0f) && xs.back() <= rect.Max.x + Px(8.0f);
        labels.push_back(shown_point && control.command.value.source != Source::kValue
                             ? ArgumentText(control.command.kind, control.command.value)
                             : std::string());
        widths.push_back(labels.back().empty() ? 0.0f : TextSize(font, theme::kSmallSize, labels.back()).x);
    }
    const std::vector<bool> fits = FittingLabels(xs, widths, Px(6.0f));
    for (std::size_t i = 0; i < shape.points.size(); i++)
    {
        const KeyControl& control = shown.controls[shape.points[i].control];
        const float x = xs[i];
        if (x < rect.Min.x - Px(8.0f) || x > rect.Max.x + Px(8.0f))
        {
            continue;
        }

        const auto [top, low] = PointHeights(rect, range, control);
        DrawPoint(draw, x, top, low, colour, state.selected_controls.contains(control.offset),
                  control.command.conditional);
        if (!labels[i].empty() && fits[i])
        {
            DrawText(draw, font, theme::kSmallSize, ImVec2(x + Px(6.0f), LaneTop(rect) - Px(2.0f)), theme::kTextMuted,
                     labels[i]);
        }
    }

    // A point being drawn.
    const KeyGesture& gesture = state.gesture;
    if (gesture.kind == KeyGesture::Kind::kLaneDraw && gesture.lane == lane)
    {
        const float x = std::floor(XOf(canvas, gesture.tick));
        const float y = ValueY(rect, range, gesture.value);
        DrawPoint(draw, x, y, y, theme::kAccent, false, false);
    }
}

// The label of controller lane `lane`, beside it under the keyboard: the setting's name and a chevron for its menu, and
// the values at the top and the bottom of the lane.
void DrawControlLabel(ImDrawList* draw, const Canvas& canvas, const KeyTrack& track, const KeyEditorState& state,
                      std::size_t lane, bool hovered)
{
    const ImRect& label = canvas.lane_labels[lane];
    const LaneSetting setting = state.lanes[lane];
    draw->AddRectFilled(label.Min, label.Max, hovered ? theme::kRaisedHover : theme::kRaised);
    draw->AddRectFilled(label.Min, ImVec2(label.Max.x, label.Min.y + Px(1.0f)), theme::kBorderStrong);

    // The name, cut short to fit beside the chevron. The tooltip gives a name that's cut short.
    const float chevron = Px(12.0f);
    const float width = label.GetWidth() - Px(10.0f) - chevron - Px(4.0f);
    const std::string full = LaneName(setting);
    const std::string name = FitCaps(full, width);
    DrawCaps(draw, ImVec2(label.Min.x + Px(10.0f), label.Min.y + Px(10.0f)), theme::kTextMuted, name);
    if (hovered && CapsSize(full).x > width)
    {
        WrappedTooltip(full);
    }
    DrawIcon(draw, Icon::kChevronDown, ImVec2(label.Max.x - Px(6.0f) - chevron * 0.5f, label.Min.y + Px(16.0f)),
             chevron, theme::kTextDim);

    // The range, when the lane has room for it.
    const edit::ValueRange range = LaneRange(track, setting);
    ImFont* figures = theme::GetFonts().figures;
    const std::string top = ValueText(setting.kind, range.highest);
    const std::string low = ValueText(setting.kind, range.lowest);
    const ImVec2 low_size = TextSize(figures, theme::kSmallSize, low);
    if (label.GetHeight() >= Px(60.0f))
    {
        const ImVec2 top_size = TextSize(figures, theme::kSmallSize, top);
        DrawText(draw, figures, theme::kSmallSize,
                 ImVec2(label.Max.x - Px(6.0f) - top_size.x, LaneTop(label) + Px(14.0f)), theme::kTextMuted, top);
        DrawText(draw, figures, theme::kSmallSize,
                 ImVec2(label.Max.x - Px(6.0f) - low_size.x, LaneBottom(label) - low_size.y), theme::kTextMuted, low);
    }
}

// The button under the lanes' labels that adds a controller lane.
void DrawAddLane(ImDrawList* draw, const Canvas& canvas, bool hovered)
{
    if (hovered)
    {
        draw->AddRectFilled(canvas.add.Min, canvas.add.Max, theme::kControlHover, Px(3.0f));
    }
    DrawIcon(draw, Icon::kAdd, canvas.add.GetCenter(), canvas.add.GetWidth() * 0.7f,
             hovered ? theme::kText : theme::kTextMuted);
}

// The bottom of the lowest lane.
float LanesBottom(const Canvas& canvas)
{
    return canvas.lanes.empty() ? canvas.lane.Max.y : canvas.lanes.back().Max.y;
}

void DrawPlayhead(ImDrawList* draw, const Canvas& canvas, const Transport& transport)
{
    const float x = std::floor(XOf(canvas, transport.Position() * canvas.ticks_per_quarter));
    if (x < canvas.notes.Min.x || x > canvas.notes.Max.x)
    {
        return;
    }

    draw->AddRectFilled(ImVec2(x, canvas.ruler.Max.y - Px(2.0f)), ImVec2(x + Px(1.0f), LanesBottom(canvas)),
                        theme::WithAlpha(theme::kText, 0.85f));
    draw->AddTriangleFilled(ImVec2(x - Px(5.5f), canvas.ruler.Max.y - Px(11.0f)),
                            ImVec2(x + Px(6.5f), canvas.ruler.Max.y - Px(11.0f)),
                            ImVec2(x + Px(0.5f), canvas.ruler.Max.y - Px(2.0f)), theme::kText);
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
        if (!hit)
        {
            if (!shift)
            {
                state.selected.clear();
            }
            gesture.kind = KeyGesture::Kind::kBox;
            return std::nullopt;
        }
        {
            const uint32_t command = track.notes[*hit].command;
            if (shift && state.selected.contains(command))
            {
                state.selected.erase(command);
                return std::nullopt;
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
        return std::nullopt;

    case KeyTool::kDraw:
        if (hit)
        {
            return Removal({track.notes[*hit].command});
        }
        {
            const double step =
                state.snap ? GridTicks(state, canvas.ticks_per_quarter) : canvas.ticks_per_quarter / 4.0;
            const int64_t start = Snapped(state, canvas.ticks_per_quarter, TickAt(canvas, mouse.x), true);
            gesture.kind = KeyGesture::Kind::kDraw;
            gesture.drawn.start = static_cast<uint32_t>(std::max<int64_t>(start, 0));
            gesture.drawn.length = static_cast<uint32_t>(std::max<int64_t>(std::llround(step), 1));
            gesture.drawn.key = KeyAt(canvas, mouse.y);
            gesture.drawn.velocity = static_cast<int>(state.insert_velocity);
            gesture.drawn = WithInstrumentAt(track, gesture.drawn.start, gesture.drawn);
        }
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
        {
            const KeyNote& pressed = track.notes[gesture.note];
            const std::optional<edit::ShiftRange> range = SharedRange(state, state.selected);
            const int64_t start = Snapped(state, canvas.ticks_per_quarter, pressed.start + moved, false);
            const int64_t shift = start - static_cast<int64_t>(pressed.start);
            gesture.shift =
                range
                    ? static_cast<int32_t>(std::clamp<int64_t>(shift, -int64_t{range->earlier}, int64_t{range->later}))
                    : 0;
            const auto [down, up] = TransposeRange(track, state);
            gesture.transpose = std::clamp(KeyAt(canvas, mouse.y) - KeyAt(canvas, gesture.from.y), down, up);
            if (!range && std::abs(moved) * canvas.beat_width / canvas.ticks_per_quarter >= Px(kDragDistance * 3.0f))
            {
                WrappedTooltip("This track's notes set its timing. Hokuto can't move them in time yet.");
            }
            break;
        }

    case KeyGesture::Kind::kSize:
        {
            const std::optional<std::map<uint32_t, uint32_t>> lengths = BaseLengths(track, state);
            if (!lengths)
            {
                gesture.resize = 0;
                WrappedTooltip("This track's notes set its timing. Hokuto can't change their lengths yet.");
                break;
            }

            // The pressed note's command takes the length up to the snapped end. The note can sound for less than its
            // command's length (Sizing).
            const KeyNote& pressed = track.notes[gesture.note];
            const uint32_t end = pressed.start + pressed.length;
            const int64_t snapped = Snapped(state, canvas.ticks_per_quarter, end + moved, false);
            const auto base = lengths->find(pressed.command);
            const uint32_t length = base != lengths->end() ? base->second : pressed.length;
            int64_t resize = snapped - (int64_t{pressed.start} + length);
            for (const auto& [command, length] : *lengths)
            {
                resize = std::max<int64_t>(resize, 1 - static_cast<int64_t>(length));
            }
            gesture.resize = static_cast<int32_t>(resize);
            break;
        }

    case KeyGesture::Kind::kVelocity:
        {
            const float top = canvas.lane.Min.y + Px(10.0f);
            const float bottom = canvas.lane.Max.y - Px(4.0f);
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
        WrappedTooltip(LaneName(setting) + " " + ValueText(setting.kind, gesture.value));
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
        refused = std::string("This value comes from ") +
                  (source == Source::kRandom ? "a random number" : "a variable") + ". The List Editor changes it.";
    }
    else if (source == Source::kValue &&
             (std::abs(upward) >= Px(kDragDistance) || gesture.value != pressed.command.value.value))
    {
        gesture.value = ValueAt(lane, track, setting, range, ValueY(lane, range, pressed.command.value.value) + upward);
    }

    // The tooltip names the value and a moved point's start, or why the point stays.
    std::string text = LaneName(setting) + " " +
                       (source == Source::kValue ? ValueText(setting.kind, gesture.value)
                                                 : ArgumentText(pressed.command.kind, pressed.command.value));
    if (gesture.shift != 0)
    {
        const double start = static_cast<double>(int64_t{pressed.tick} + gesture.shift);
        text += "\nStart " + WithoutFigureSpaces(PositionText(start / canvas.ticks_per_quarter, kBeatsPerBar));
    }
    WrappedTooltip(refused.empty() ? text : refused);
}

// Ends a gesture when the mouse is released. Returns the edit that it makes.
std::optional<KeyEdit> Release(const Canvas& canvas, const KeyTrack& track, KeyEditorState& state, ImVec2 mouse)
{
    const KeyGesture gesture = std::exchange(state.gesture, KeyGesture());
    switch (gesture.kind)
    {
    case KeyGesture::Kind::kMove:
        {
            if (gesture.shift == 0 && gesture.transpose == 0)
            {
                return std::nullopt;
            }

            KeyEdit edit;
            edit.name = "Move";
            for (const uint32_t command : state.selected)
            {
                edit.changes.push_back({.command = command, .shift = gesture.shift, .transpose = gesture.transpose});
            }
            return edit;
        }

    case KeyGesture::Kind::kSize:
        {
            const std::optional<std::map<uint32_t, uint32_t>> lengths = BaseLengths(track, state);
            if (gesture.resize == 0 || !lengths)
            {
                return std::nullopt;
            }

            KeyEdit edit;
            edit.name = "Size";
            for (const auto& [command, length] : *lengths)
            {
                const auto changed = static_cast<int64_t>(length) + gesture.resize;
                edit.changes.push_back(
                    {.command = command, .length = static_cast<uint32_t>(std::max<int64_t>(changed, 1))});
            }
            return edit;
        }

    case KeyGesture::Kind::kVelocity:
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
                edit.changes.push_back(
                    {.command = command, .velocity = std::clamp(velocity + change, 1, kTopVelocity)});
            }
            return edit;
        }

    case KeyGesture::Kind::kBox:
        {
            const ImRect box(ImMin(gesture.from, mouse), ImMax(gesture.from, mouse));
            for (const KeyNote& note : track.notes)
            {
                if (box.Overlaps(NoteRect(canvas, note, 0, 0, 0)))
                {
                    state.selected.insert(note.command);
                }
            }
            return std::nullopt;
        }

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
            return LaneDrawEdit(state.lanes[gesture.lane], gesture.tick, gesture.value);
        }
        return std::nullopt;

    case KeyGesture::Kind::kLaneBox:
        {
            // The box selects each point of the lane whose heights it reaches.
            if (gesture.lane >= canvas.lanes.size())
            {
                return std::nullopt;
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
            return std::nullopt;
        }

    default:
        return std::nullopt;
    }
}

// The mouse in the canvas. The wheel scrolls through the keys, and with Shift through time. Command or Ctrl with the
// wheel zooms the time around the mouse, and Option or Alt the keys. A click on the ruler moves the position.
std::optional<KeyEdit> HandleMouse(Canvas& canvas, const KeyTrack& track, KeyEditorState& state, Transport& transport,
                                   bool busy)
{
    const ImGuiIO& io = ImGui::GetIO();
    const bool hovered = ImGui::IsWindowHovered();
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

    const ImVec2 mouse = io.MousePos;
    if (hovered && canvas.ruler.Contains(mouse) && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
    {
        transport.SetPosition(RulerPosition(state, canvas.ticks_per_quarter, TickAt(canvas, mouse.x)));
        return std::nullopt;
    }

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
        return std::nullopt;
    }
    if (overview && ImGui::IsMouseDown(ImGuiMouseButton_Left))
    {
        ScrollToOverview(canvas, track, mouse.x);
        return std::nullopt;
    }
    if (overview)
    {
        state.gesture = KeyGesture();
        return std::nullopt;
    }

    // A click on a controller lane's label opens its menu, and a click on the button under the labels adds a lane.
    const bool idle = state.gesture.kind == KeyGesture::Kind::kNone;
    for (std::size_t lane = 0; lane < canvas.lane_labels.size(); lane++)
    {
        if (idle && hovered && canvas.lane_labels[lane].Contains(mouse) && !canvas.add.Contains(mouse) &&
            ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            state.lane_menu = lane;
            ImGui::OpenPopup("##lane");
            return std::nullopt;
        }
    }
    if (idle && hovered && canvas.add.Contains(mouse))
    {
        WrappedTooltip("Create Controller Lane");
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            state.lanes.push_back(NextLane(track, state.lanes));
            return std::nullopt;
        }
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

    // The mouse shows where a press sizes a note.
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
        if (edit && first != track.notes.end())
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
// that the track sets has an asterisk, and so does its group. A setting that the console's sequences lack is off.
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
            const std::string group_name = std::string(group.name) + (set ? " *" : "");
            if (!ImGui::BeginMenu(group_name.c_str()))
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
    const auto lanes = static_cast<float>(state.lanes.size() + 1);
    const float room = ImGui::GetContentRegionAvail().y - top - ImGui::GetStyle().ScrollbarSize - Px(kLeastNotesHeight);
    const float lane = std::clamp(room / lanes, Px(kLeastLaneHeight), Px(kLaneHeight));
    ImGui::SetNextWindowContentSize(ImVec2(keyboard + static_cast<float>(quarters + 16.0) * canvas.beat_width,
                                           top + static_cast<float>(kKeys) * canvas.key_height + lane * lanes));
    ImGui::BeginChild("##keys", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None,
                      ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    const ImRect inner = ImGui::GetCurrentWindow()->InnerRect;
    const float lanes_top = inner.Max.y - lane * lanes;
    canvas.scroll_x = ImGui::GetScrollX();
    canvas.scroll_y = ImGui::GetScrollY();
    canvas.ruler = ImRect(ImVec2(inner.Min.x + keyboard, inner.Min.y), ImVec2(inner.Max.x, inner.Min.y + ruler));
    canvas.overview =
        ImRect(ImVec2(inner.Min.x + keyboard, inner.Min.y + ruler), ImVec2(inner.Max.x, inner.Min.y + top));
    canvas.keyboard = ImRect(ImVec2(inner.Min.x, inner.Min.y + top), ImVec2(inner.Min.x + keyboard, lanes_top));
    canvas.notes = ImRect(ImVec2(inner.Min.x + keyboard, inner.Min.y + top), ImVec2(inner.Max.x, lanes_top));
    canvas.lane = ImRect(ImVec2(inner.Min.x + keyboard, lanes_top), ImVec2(inner.Max.x, lanes_top + lane));
    canvas.lane_label = ImRect(ImVec2(inner.Min.x, lanes_top), ImVec2(inner.Min.x + keyboard, lanes_top + lane));
    for (std::size_t i = 1; i <= state.lanes.size(); i++)
    {
        const float lane_top = lanes_top + lane * static_cast<float>(i);
        canvas.lanes.emplace_back(ImVec2(inner.Min.x + keyboard, lane_top), ImVec2(inner.Max.x, lane_top + lane));
        canvas.lane_labels.emplace_back(ImVec2(inner.Min.x, lane_top), ImVec2(inner.Min.x + keyboard, lane_top + lane));
    }
    const float add = Px(kAddSize);
    canvas.add = ImRect(ImVec2(inner.Min.x + Px(6.0f), inner.Max.y - Px(6.0f) - add),
                        ImVec2(inner.Min.x + Px(6.0f) + add, inner.Max.y - Px(6.0f)));

    // A scroll that the program asked for: to the start of the part that the user opened, and to its keys.
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
    state.left_quarter = canvas.scroll_x / canvas.beat_width;
    state.top_key = static_cast<float>(kKeys - 1) - canvas.scroll_y / canvas.key_height;

    // A zoom from the menu keeps the playhead in place. With the playhead out of sight, the middle stays in place. Zoom
    // Full shows the song from its start.
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

    std::optional<KeyEdit> edit = HandleMouse(canvas, track, state, transport, busy);

    // The key and the tick under the mouse, for the toolbar's next frame.
    const ImVec2 mouse = ImGui::GetIO().MousePos;
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

    // The rows and the grid, then the notes and the box of a selection over them. The lane, the keyboard and the ruler
    // go round them, and the playhead over them. The miniature covers the playhead's line.
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

    // Playback that runs off either edge turns the page.
    if (transport.Playing() && state.gesture.kind == KeyGesture::Kind::kNone)
    {
        const float playhead = XOf(canvas, transport.Position() * canvas.ticks_per_quarter);
        if (playhead > canvas.notes.Max.x - Px(16.0f) || playhead < canvas.notes.Min.x)
        {
            ImGui::SetScrollX(std::max(canvas.scroll_x + playhead - canvas.notes.Min.x - Px(32.0f), 0.0f));
        }
    }
    DrawLaneMenu(track, state);
    ImGui::EndChild();

    return edit;
}

} // namespace

std::optional<KeyEdit> DrawKeyEditor(const KeyTrack& track, KeyEditorState& state, Transport& transport, bool busy,
                                     bool* open)
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
    std::optional<KeyEdit> edit = DrawInfoLine(track, state, busy);
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
