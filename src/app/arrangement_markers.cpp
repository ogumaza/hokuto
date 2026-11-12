// SPDX-License-Identifier: MIT

// The arrangement's marker row: the cycle, the loop, the regions, and the places where the tracks read variables
// or branch, with the row's menu.

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "app/arrangement.h"
#include "app/arrangement_canvas.h"
#include "app/markers.h"
#include "app/song_view.h"
#include "app/theme.h"
#include "app/time_text.h"
#include "app/transport.h"
#include "app/widgets.h"
#include "app/zone_parts.h"

namespace hokuto::app
{
namespace
{

using arrangement_canvas::Canvas;
using arrangement_canvas::kMostMarkerLines;
using arrangement_canvas::QuartersAt;
using arrangement_canvas::XOf;
using theme::Px;
using zone_parts::kAltKey;

// A region of the marker row: the sequence's intro or its loop, from `start` to `end` in quarter notes.
struct CycleMarker
{
    const char* name;
    double start;
    double end;
    ImU32 colour;
    const char* tooltip;
};

// The song's regions: the intro, where the loop starts after the song's start, and the loop. A song without a loop has
// none. During a drag of an edge of the loop, the loop follows the drag.
std::vector<CycleMarker> CycleMarkers(const SongView& song, const std::optional<LoopMove>& drag = std::nullopt)
{
    std::vector<CycleMarker> markers;
    const int32_t loop_start = drag ? drag->start : song.loop_start;
    const int32_t loop_end = drag ? drag->end : song.loop_end;
    if (loop_start < 0 || loop_end <= loop_start)
    {
        return markers;
    }

    const double quarter = std::max(song.ticks_per_quarter, 1);
    if (loop_start > 0)
    {
        markers.push_back({"Intro", 0.0, loop_start / quarter, theme::kTextDim, "Intro. Plays once, before the loop."});
    }
    markers.push_back({"Loop", loop_start / quarter, loop_end / quarter, theme::kCycle,
                       "Loop. Repeats until the game stops the sound."});

    return markers;
}

// The edge of a timeline's loop at `x` in the marker row: true for the loop's end, false for its start. A drag of
// either edge moves it.
std::optional<bool> LoopEdgeAt(const Canvas& canvas, const SongView& song, float x)
{
    if (!song.loop_moves || song.loop_start < 0 || song.loop_end <= song.loop_start)
    {
        return std::nullopt;
    }

    const double quarter = std::max(song.ticks_per_quarter, 1);
    const float reach = Px(4.0f);
    if (std::abs(x - XOf(canvas, song.loop_end / quarter)) <= reach)
    {
        return true;
    }
    if (std::abs(x - XOf(canvas, song.loop_start / quarter)) <= reach)
    {
        return false;
    }

    return std::nullopt;
}

// Where a region lies in the marker row.
ImRect RegionRect(const Canvas& canvas, const CycleMarker& marker)
{
    const ImRect& row = canvas.markers;
    return ImRect(ImVec2(std::floor(XOf(canvas, marker.start)) + Px(1.0f), row.Min.y + Px(3.0f)),
                  ImVec2(std::floor(XOf(canvas, marker.end)) - Px(1.0f), row.Max.y - Px(4.0f)));
}

// The label of a change of a variable: the variable and its new value.
std::string ChangeLabel(const ChangeView& change)
{
    return std::to_string(change.variable) + " → " + std::to_string(change.value);
}

// The label of a place where the tracks read variables or branch: "if" at a branch, then the variables that the place
// reads. The marker row draws "if" in the List Editor's colour of conditions.
struct LogicLabel
{
    bool branch = false;
    std::string variables;
};

LogicLabel LogicLabelOf(const LogicMarkerView& marker)
{
    return {marker.branch, VariablesLabel(marker.variables)};
}

// The width of a place's label.
float LogicLabelWidth(const LogicLabel& label)
{
    ImFont* font = theme::GetFonts().text;
    const float branch = label.branch ? TextSize(font, theme::kSmallSize, "if").x : 0.0f;
    const float gap = label.branch && !label.variables.empty() ? Px(4.0f) : 0.0f;
    return branch + gap + TextSize(font, theme::kSmallSize, label.variables).x;
}

// Where the marker row's flags lie: a flag at the first place of each of the song's sets of commands, in the order of
// song.logic, and a flag at each change of a variable, in the order of song.changes. Each flag holds its label with the
// margins round it, but stops short of the next flag to its right, of either kind (FlagWidths). Of a set and a change
// at one place, the change's flag holds its label.
struct MarkerFlags
{
    std::vector<ImRect> logic;
    std::vector<ImRect> changes;
};

MarkerFlags MarkerFlagsOf(const Canvas& canvas, const SongView& song)
{
    const auto x_of = [&canvas](double tick)
    {
        return std::floor(XOf(canvas, tick / canvas.ticks_per_quarter));
    };

    std::vector<FlagSpan> spans;
    for (const LogicMarkerView& marker : song.logic)
    {
        spans.push_back({x_of(marker.ticks.front()), LogicLabelWidth(LogicLabelOf(marker)) + Px(10.0f)});
    }
    for (const ChangeView& change : song.changes)
    {
        const float width = TextSize(theme::GetFonts().text, theme::kSmallSize, ChangeLabel(change)).x;
        spans.push_back({x_of(change.tick), width + Px(10.0f)});
    }
    const std::vector<float> widths = FlagWidths(spans, Px(2.0f), Px(2.0f));

    const ImRect& row = canvas.markers;
    MarkerFlags flags;
    for (std::size_t i = 0; i < spans.size(); i++)
    {
        const ImRect flag(ImVec2(spans[i].x, row.Min.y + Px(3.0f)),
                          ImVec2(spans[i].x + widths[i], row.Max.y - Px(4.0f)));
        (i < song.logic.size() ? flags.logic : flags.changes).push_back(flag);
    }

    return flags;
}

// The place of a set of commands under the mouse at `mouse`: the set's flag at its first place, or else the nearest
// place of any set within a few pixels. Nothing elsewhere.
std::optional<MarkerPlace> LogicPlaceAt(const Canvas& canvas, const SongView& song, const MarkerFlags& flags,
                                        ImVec2 mouse)
{
    for (std::size_t i = song.logic.size(); i-- > 0;)
    {
        if (flags.logic[i].Contains(mouse))
        {
            return MarkerPlace{i, song.logic[i].ticks.front()};
        }
    }

    // The nearest place of each set lies on one side of the mouse or the other.
    const auto at = static_cast<int32_t>(std::ceil(QuartersAt(canvas, mouse.x) * canvas.ticks_per_quarter));
    std::optional<MarkerPlace> nearest;
    float distance = Px(3.0f);
    for (std::size_t i = 0; i < song.logic.size(); i++)
    {
        const std::vector<int32_t>& ticks = song.logic[i].ticks;
        const auto after = std::lower_bound(ticks.begin(), ticks.end(), at);
        for (auto tick = after == ticks.begin() ? after : after - 1; tick != ticks.end() && tick <= after; ++tick)
        {
            const float away = std::abs(std::floor(XOf(canvas, static_cast<double>(*tick) / canvas.ticks_per_quarter)) -
                                        std::floor(mouse.x));
            if (away <= distance)
            {
                distance = away;
                nearest = MarkerPlace{i, *tick};
            }
        }
    }

    return nearest;
}

// The tooltip of a set of commands: the commands, up to kMostMarkerLines of them, and the places where the set runs
// when it runs at more than one. At a branch, a line says what "if" means.
std::string LogicTooltip(const LogicMarkerView& marker, int beats_per_bar, int ticks_per_quarter)
{
    std::string text;
    for (std::size_t i = 0; i < marker.commands.size() && i < kMostMarkerLines; i++)
    {
        text += (i == 0 ? "" : "\n") + marker.commands[i].line;
    }
    if (marker.commands.size() > kMostMarkerLines)
    {
        text += "\nand " + std::to_string(marker.commands.size() - kMostMarkerLines) + " more";
    }
    text += "\n\n";
    if (marker.ticks.size() > 1)
    {
        const double quarter = std::max(ticks_per_quarter, 1);
        const auto place = [&](int32_t tick)
        {
            return WithoutFigureSpaces(PositionText(tick / quarter, beats_per_bar));
        };
        text += "These commands run at " + std::to_string(marker.ticks.size()) + " places, from " +
                place(marker.ticks.front()) + " to " + place(marker.ticks.back()) + ". ";
    }
    if (marker.branch)
    {
        text += "A command after \"if\" runs only if the last comparison was true. ";
    }

    return text + "Double-click to open the commands in the List Editor.";
}

// Draws a flag of the marker row in `colour`: a line at the flag's place, and the flag's ground.
void DrawFlag(ImDrawList* draw, const Canvas& canvas, const ImRect& flag, ImU32 colour)
{
    const ImRect& row = canvas.markers;
    draw->AddRectFilled(flag.Min, flag.Max, theme::WithAlpha(colour, 0.22f), Px(3.0f));
    draw->AddRectFilled(ImVec2(flag.Min.x, row.Min.y + Px(2.0f)), ImVec2(flag.Min.x + Px(2.0f), row.Max.y - Px(2.0f)),
                        colour);
}

// Where the name of the region at `region` starts, for a name `width` wide: after the flags that cover the region's
// start. The flags would otherwise hide the name.
float RegionNameX(const MarkerFlags& marker_flags, const ImRect& region, float width)
{
    std::vector<ImRect> flags = marker_flags.logic;
    flags.insert(flags.end(), marker_flags.changes.begin(), marker_flags.changes.end());
    const auto left = [](const ImRect& a, const ImRect& b)
    {
        return a.Min.x < b.Min.x;
    };
    std::sort(flags.begin(), flags.end(), left);

    float x = region.Min.x + Px(6.0f);
    for (const ImRect& flag : flags)
    {
        if (flag.Min.x > x + width)
        {
            break;
        }

        if (flag.Max.x > x - Px(4.0f))
        {
            x = flag.Max.x + Px(6.0f);
        }
    }

    return x;
}

// The song's regions, each with its name.
void DrawRegions(ImDrawList* draw, const Canvas& canvas, const SongView& song, const MarkerFlags& flags,
                 const std::optional<LoopMove>& drag)
{
    ImFont* font = theme::GetFonts().text;
    for (const CycleMarker& marker : CycleMarkers(song, drag))
    {
        const ImRect rect = RegionRect(canvas, marker);
        draw->AddRectFilled(rect.Min, rect.Max, theme::WithAlpha(marker.colour, 0.22f), Px(3.0f));
        draw->AddRect(rect.Min, rect.Max, theme::WithAlpha(marker.colour, 0.75f), Px(3.0f), Px(1.0f));
        draw->PushClipRect(rect.Min, rect.Max, true);
        const ImVec2 size = TextSize(font, theme::kSmallSize, marker.name);
        DrawText(draw, font, theme::kSmallSize,
                 ImVec2(RegionNameX(flags, rect, size.x), rect.GetCenter().y - size.y * 0.5f), theme::kText,
                 marker.name);
        draw->PopClipRect();
    }
}

// A thin line at each place where a set of commands runs after its first, one at most in each column of pixels. The
// set's flag marks its first place.
void DrawLaterPlaces(ImDrawList* draw, const Canvas& canvas, const LogicMarkerView& marker)
{
    const ImRect& row = canvas.markers;
    const double first = QuartersAt(canvas, row.Min.x) * canvas.ticks_per_quarter;
    const double last = QuartersAt(canvas, row.Max.x) * canvas.ticks_per_quarter;
    auto tick = std::lower_bound(marker.ticks.begin() + 1, marker.ticks.end(), static_cast<int32_t>(std::floor(first)));
    while (tick != marker.ticks.end() && *tick <= last)
    {
        const float x = std::floor(XOf(canvas, static_cast<double>(*tick) / canvas.ticks_per_quarter));
        draw->AddRectFilled(ImVec2(x, row.Min.y + Px(3.0f)), ImVec2(x + Px(1.0f), row.Max.y - Px(4.0f)),
                            theme::WithAlpha(theme::kAccent, 0.6f));

        // The next place a column of pixels or more to the right.
        const double next = QuartersAt(canvas, x + Px(1.0f)) * canvas.ticks_per_quarter;
        tick = std::lower_bound(tick + 1, marker.ticks.end(), static_cast<int32_t>(std::ceil(next)));
    }
}

// The sets of commands that the song's tracks run where they read variables or branch. A blue flag with the set's label
// marks the first place of each set, and a thinner line each later place.
void DrawLogicFlags(ImDrawList* draw, const Canvas& canvas, const SongView& song, const MarkerFlags& flags)
{
    const ImRect& row = canvas.markers;
    ImFont* font = theme::GetFonts().text;
    for (const LogicMarkerView& marker : song.logic)
    {
        DrawLaterPlaces(draw, canvas, marker);
    }
    for (std::size_t i = 0; i < song.logic.size(); i++)
    {
        const ImRect& flag = flags.logic[i];
        if (flag.Max.x < row.Min.x || flag.Min.x > row.Max.x)
        {
            continue;
        }

        const LogicLabel label = LogicLabelOf(song.logic[i]);
        DrawFlag(draw, canvas, flag, theme::kAccent);
        draw->PushClipRect(flag.Min, flag.Max, true);
        const float top = flag.GetCenter().y - TextSize(font, theme::kSmallSize, "if").y * 0.5f;
        float x = flag.Min.x + Px(6.0f);
        if (label.branch)
        {
            DrawText(draw, font, theme::kSmallSize, ImVec2(x, top), theme::kCycle, "if");
            x += TextSize(font, theme::kSmallSize, "if").x + Px(4.0f);
        }
        DrawText(draw, font, theme::kSmallSize, ImVec2(x, top), theme::kText, label.variables);
        draw->PopClipRect();
    }
}

// The changes of the variables, each with the variable and its new value in a flag in the colour of the play button.
void DrawChangeFlags(ImDrawList* draw, const Canvas& canvas, const SongView& song, const MarkerFlags& flags)
{
    const ImRect& row = canvas.markers;
    ImFont* font = theme::GetFonts().text;
    for (std::size_t i = 0; i < song.changes.size(); i++)
    {
        const ImRect& flag = flags.changes[i];
        if (flag.Max.x < row.Min.x || flag.Min.x > row.Max.x)
        {
            continue;
        }

        const std::string label = ChangeLabel(song.changes[i]);
        const ImVec2 size = TextSize(font, theme::kSmallSize, label);
        DrawFlag(draw, canvas, flag, theme::kPlay);
        draw->PushClipRect(flag.Min, flag.Max, true);
        DrawText(draw, font, theme::kSmallSize, ImVec2(flag.Min.x + Px(6.0f), flag.GetCenter().y - size.y * 0.5f),
                 theme::kText, label);
        draw->PopClipRect();
    }
}

} // namespace

namespace arrangement_canvas
{

namespace
{

// Moves the dragged edge of the loop to the mouse.
void DragLoopEdge(const Canvas& canvas, LoopMove& drag)
{
    const ImGuiIO& io = ImGui::GetIO();
    const int64_t step = io.KeyShift ? 1 : std::max(canvas.ticks_per_quarter, 1);
    const double tick = QuartersAt(canvas, io.MousePos.x) * canvas.ticks_per_quarter;
    const int64_t snapped = std::llround(tick / static_cast<double>(step)) * step;
    if (drag.end_edge)
    {
        drag.end = static_cast<int32_t>(
            std::clamp<int64_t>(snapped, int64_t{drag.start} + 1, std::numeric_limits<int32_t>::max()));
    }
    else
    {
        drag.start = static_cast<int32_t>(std::clamp<int64_t>(snapped, 0, int64_t{drag.end} - 1));
    }
}

// A drag shows the loop while the mouse holds the edge. When the mouse button is released, another tick moves the
// loop.
void FinishLoopDrag(const Canvas& canvas, const SongView& song, bool dragging, ArrangementState& state)
{
    const LoopMove& drag = *state.loop_drag;
    if (dragging)
    {
        const double quarter = std::max(canvas.ticks_per_quarter, 1);
        const std::string start = WithoutFigureSpaces(PositionText(drag.start / quarter, song.beats_per_bar));
        const std::string end = WithoutFigureSpaces(PositionText(drag.end / quarter, song.beats_per_bar));
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        WrappedTooltip("Loop from " + start + " to " + end);
        return;
    }

    if (drag.start != drag.from_start || drag.end != drag.from_end)
    {
        state.loop_move = drag;
    }
    state.loop_drag.reset();
}

// Handles the mouse over the row of markers: on an edge of the loop, a flag or a region.
void HoverMarkerRow(const Canvas& canvas, const SongView& song, const MarkerFlags& flags, Transport& transport,
                    ArrangementState& state)
{
    const ImGuiIO& io = ImGui::GetIO();
    const bool double_click = ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
    const bool still = !ImGui::IsAnyMouseDown();
    if (const std::optional<bool> end_edge = LoopEdgeAt(canvas, song, io.MousePos.x))
    {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !double_click)
        {
            state.loop_drag = LoopMove{song.loop_start, song.loop_end, song.loop_start, song.loop_end, *end_edge};
        }
        else if (still)
        {
            WrappedTooltip(*end_edge ? "Loop end. Drag to move it." : "Loop start. Drag to move it.");
        }
        return;
    }
    for (std::size_t i = 0; i < song.changes.size(); i++)
    {
        const ChangeView& change = song.changes[i];
        if (flags.changes[i].Contains(io.MousePos))
        {
            if (still)
            {
                WrappedTooltip("Variable " + std::to_string(change.variable) + " changes to " +
                               std::to_string(change.value) + " at " + TimeText(change.time));
            }
            return;
        }
    }
    if (const std::optional<MarkerPlace> place = LogicPlaceAt(canvas, song, flags, io.MousePos))
    {
        if (double_click)
        {
            state.open_marker = place;
        }
        else if (still)
        {
            WrappedTooltip(LogicTooltip(song.logic[place->marker], song.beats_per_bar, canvas.ticks_per_quarter));
        }
        return;
    }
    for (const CycleMarker& marker : CycleMarkers(song))
    {
        if (!RegionRect(canvas, marker).Contains(io.MousePos))
        {
            continue;
        }

        if (double_click && io.KeyAlt)
        {
            state.zoom_range = std::pair(marker.start, marker.end);
        }
        else if (double_click)
        {
            transport.SetLocators(marker.start, marker.end);
        }
        else if (still)
        {
            WrappedTooltip(std::string(marker.tooltip) + " Double-click to set the locators around it. " + kAltKey +
                           "-double-click to zoom to it.");
        }
        return;
    }
}

} // namespace

void DrawMarkers(ImDrawList* draw, const Canvas& canvas, const SongView& song, Transport& transport,
                 ArrangementState& state)
{
    // The dragged edge of the loop follows the mouse, on the beats or on the ticks.
    const ImGuiIO& io = ImGui::GetIO();
    const bool dragging = state.loop_drag && ImGui::IsMouseDown(ImGuiMouseButton_Left);
    if (dragging)
    {
        DragLoopEdge(canvas, *state.loop_drag);
    }

    const ImRect& row = canvas.markers;
    draw->AddRectFilled(row.Min, row.Max, theme::kPanel);
    draw->AddRectFilled(ImVec2(row.Min.x, row.Max.y - Px(1.0f)), row.Max, theme::kBorder);
    const MarkerFlags flags = MarkerFlagsOf(canvas, song);
    DrawRegions(draw, canvas, song, flags, state.loop_drag);
    DrawLogicFlags(draw, canvas, song, flags);
    DrawChangeFlags(draw, canvas, song, flags);

    if (state.loop_drag)
    {
        FinishLoopDrag(canvas, song, dragging, state);
        return;
    }

    // The edge, the flag or the region under the mouse. A tooltip waits while a button is down. A drag of the track
    // list's edge leaves the row alone.
    if (!ImGui::IsWindowHovered() || !row.Contains(io.MousePos) || state.resizing_track_list)
    {
        return;
    }

    HoverMarkerRow(canvas, song, flags, transport, state);
}

namespace
{

// Locate's markers, in the order of their places: the regions, the flags of the sets of commands that the tracks
// run where they read variables or branch, and the changes' flags.
struct Located
{
    double quarters;
    std::string name;
};

// The markers of Locate in `song`, with the regions of `cycles`.
std::vector<Located> LocatedMarkers(const SongView& song, const std::vector<CycleMarker>& cycles)
{
    const double quarter = std::max(song.ticks_per_quarter, 1);
    std::vector<Located> located;
    for (const CycleMarker& marker : cycles)
    {
        located.push_back({marker.start, marker.name});
    }
    for (const LogicMarkerView& marker : song.logic)
    {
        const LogicLabel label = LogicLabelOf(marker);
        const std::string name = std::string(label.branch ? "if " : "") + label.variables;
        located.push_back({marker.ticks.front() / quarter, label.variables.empty() ? std::string("if") : name});
    }
    for (const ChangeView& change : song.changes)
    {
        located.push_back({change.tick / quarter,
                           "Variable " + std::to_string(change.variable) + " → " + std::to_string(change.value)});
    }
    const auto earlier = [](const Located& a, const Located& b)
    {
        return a.quarters < b.quarters;
    };
    std::stable_sort(located.begin(), located.end(), earlier);

    return located;
}

// The Locate menu of `located`. A choice moves the position to the marker.
void DrawLocateMenu(const std::vector<Located>& located, const SongView& song, Transport& transport)
{
    // A grey menu gives its reason in a tooltip.
    const bool locate = ImGui::BeginMenu("Locate", !located.empty());
    if (located.empty())
    {
        ItemTooltip("The sound has no markers: no loop, no variable reads or branches, and no variable changes.");
    }
    if (locate)
    {
        for (std::size_t i = 0; i < located.size(); i++)
        {
            ImGui::PushID(static_cast<int>(i));
            // The figures keep the positions' digits in line, and their figure spaces as wide as a digit.
            const std::string position = PositionText(located[i].quarters, song.beats_per_bar);
            PushFigures();
            const bool chosen = ImGui::MenuItem(located[i].name.c_str(), position.c_str());
            ImGui::PopFont();
            if (chosen)
            {
                transport.SetPosition(located[i].quarters);
            }
            ImGui::PopID();
        }
        ImGui::EndMenu();
    }
}

// A menu of the regions of `cycles` under `label`. `on_choose` takes the chosen region. Without a loop, the menu is
// grey and `tooltip` contains the reason.
template <typename OnChoose>
void DrawCycleMenu(const char* label, const char* tooltip, const std::vector<CycleMarker>& cycles, OnChoose on_choose)
{
    const bool open = ImGui::BeginMenu(label, !cycles.empty());
    if (cycles.empty())
    {
        ItemTooltip(tooltip);
    }
    if (open)
    {
        for (const CycleMarker& marker : cycles)
        {
            if (ImGui::MenuItem(marker.name))
            {
                on_choose(marker);
            }
        }
        ImGui::EndMenu();
    }
}

} // namespace

void DrawMarkerMenu(const SongView& song, Transport& transport, ArrangementState& state)
{
    if (!ImGui::BeginPopup(kMarkerMenu))
    {
        return;
    }

    const std::vector<CycleMarker> cycles = CycleMarkers(song);
    const std::vector<Located> located = LocatedMarkers(song, cycles);
    DrawLocateMenu(located, song, transport);
    DrawCycleMenu("Cycle", "The sound has no loop. Cycle sets the locators around the intro or the loop.", cycles,
                  [&](const CycleMarker& marker) { transport.SetLocators(marker.start, marker.end); });
    DrawCycleMenu("Zoom", "The sound has no loop. Zoom fits the intro or the loop into the lanes.", cycles,
                  [&](const CycleMarker& marker) { state.zoom_range = std::pair(marker.start, marker.end); });
    ImGui::EndPopup();
}

} // namespace arrangement_canvas
} // namespace hokuto::app
