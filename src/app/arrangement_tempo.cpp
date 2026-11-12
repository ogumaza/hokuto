// SPDX-License-Identifier: MIT

// The arrangement's tempo row: the line of the tempo, its points and their drags, and the row's header.

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "app/arrangement.h"
#include "app/arrangement_canvas.h"
#include "app/song_view.h"
#include "app/tempo_row.h"
#include "app/theme.h"
#include "app/time_text.h"
#include "app/transport.h"
#include "app/widgets.h"

namespace hokuto::app
{
namespace
{

using arrangement_canvas::Canvas;
using arrangement_canvas::kTempoGrab;
using arrangement_canvas::kTempoMargin;
using arrangement_canvas::TempoRowEnd;
using arrangement_canvas::XOf;
using theme::Px;

// A tempo as the tempo row shows it: a whole number of beats a minute, or two decimals for a tempo between them.
std::string TempoText(double bpm)
{
    std::array<char, 32> text{};
    const bool whole = std::abs(bpm - std::round(bpm)) < 0.005;
    std::snprintf(text.data(), text.size(), whole ? "%.0f" : "%.2f", bpm);

    return text.data();
}

// Where the tempo `bpm` lies in the tempo row, between the row's margins. A tempo outside the row's span lies at the
// nearer margin.
float TempoY(const ImRect& row, const TempoSpan& span, double bpm)
{
    const float margin = Px(kTempoMargin);
    const double share = span.high > span.low ? std::clamp((bpm - span.low) / (span.high - span.low), 0.0, 1.0) : 0.5;
    return std::floor(row.Max.y - margin - static_cast<float>(share) * (row.GetHeight() - margin * 2.0f));
}

// Where a point of the tempo row lies: at its tick, and at its tempo. A tempo without one value lies in the middle of
// the row. During `drag`, each run of the dragged command lies at the drag's tempo, and its tick moves by the drag's
// shift.
ImVec2 TempoPointPos(const Canvas& canvas, const TempoSpan& span, const TempoPointView& point,
                     const std::optional<TempoMove>& drag)
{
    const bool dragged = drag && drag->point.command == point.command;
    const int32_t tick = point.tick + (dragged ? drag->shift : 0);
    const float x = std::floor(XOf(canvas, static_cast<double>(tick) / canvas.ticks_per_quarter));
    if (!point.bpm)
    {
        return ImVec2(x, std::floor(canvas.tempo.GetCenter().y));
    }

    return ImVec2(x, TempoY(canvas.tempo, span, dragged ? drag->to : *point.bpm));
}

// The point of the tempo row under the mouse at `mouse`: the nearest within kTempoGrab. Of two points in one place, the
// later takes the mouse. It's drawn over the earlier one.
std::optional<std::size_t> TempoPointAt(const Canvas& canvas, const SongView& song, const TempoSpan& span, ImVec2 mouse)
{
    std::optional<std::size_t> nearest;
    float distance = Px(kTempoGrab);
    for (std::size_t i = 0; i < song.tempo_points.size(); i++)
    {
        const ImVec2 at = TempoPointPos(canvas, span, song.tempo_points[i], std::nullopt);
        const float away = std::hypot(at.x - mouse.x, at.y - mouse.y);
        if (away <= distance)
        {
            distance = away;
            nearest = i;
        }
    }

    return nearest;
}

// The tooltip of point `index` of the tempo row: the track, the place and the tempo, then what the mouse does there.
std::string TempoTooltip(const SongView& song, std::size_t index)
{
    const TempoPointView& point = song.tempo_points[index];
    const double quarter = std::max(song.ticks_per_quarter, 1);
    std::string text = point.track_name + " sets the tempo at " +
                       WithoutFigureSpaces(PositionText(point.tick / quarter, song.beats_per_bar)) + ": " +
                       (point.bpm ? TempoText(*point.bpm) + " BPM" : point.source) + "\n\n";
    const auto same = [&point](const TempoPointView& other)
    {
        return other.command == point.command;
    };
    const auto runs = std::count_if(song.tempo_points.begin(), song.tempo_points.end(), same);
    if (runs > 1)
    {
        text += "The command runs at " + std::to_string(runs) + " places. Editing it changes all of them. ";
    }
    if (!point.reach.empty())
    {
        text += "An edit also changes " + point.reach + ". ";
    }
    if (point.conditional)
    {
        text += "The command runs only if the last comparison was true. ";
    }
    if (Overridden(song, index))
    {
        text += "A later command sets the tempo again in the same tick. ";
    }
    if (point.bpm && point.fixed.empty())
    {
        text +=
            "Drag up or down to change the tempo, or sideways to move it to another beat. Hold Shift for finer "
            "control. ";
    }
    else if (point.bpm)
    {
        text += "Drag up or down to change the tempo. Hold Shift for finer control. ";
    }
    else if (point.fixed.empty())
    {
        text += "Drag sideways to move it to another beat, or with Shift to another tick. ";
    }

    return text + "Double-click to open the command in the List Editor.";
}

// The mouse's cursor over a point of the tempo row: arrows in the directions that a drag of the point takes.
ImGuiMouseCursor TempoCursor(const TempoPointView& point)
{
    if (point.bpm && point.fixed.empty())
    {
        return ImGuiMouseCursor_ResizeAll;
    }
    if (point.bpm)
    {
        return ImGuiMouseCursor_ResizeNS;
    }

    return point.fixed.empty() ? ImGuiMouseCursor_ResizeEW : ImGuiMouseCursor_Arrow;
}

} // namespace

namespace arrangement_canvas
{

int32_t TempoRowEnd(const SongView& song)
{
    int32_t end = song.length;
    for (const TempoPointView& point : song.tempo_points)
    {
        end = std::max(end, point.tick);
    }

    return end;
}

} // namespace arrangement_canvas

namespace
{

// The tempo row's line: a step at each change of the tempo, filled below, up to TempoRowEnd. The tempo's number stands
// over the start of each step that has room for it, or under the step where the row's top leaves no room.
void DrawTempoLine(ImDrawList* draw, const Canvas& canvas, const SongView& song,
                   const std::vector<TempoChangeView>& line, const TempoSpan& span)
{
    const ImRect& row = canvas.tempo;
    const double quarter = canvas.ticks_per_quarter;
    const float end = std::floor(XOf(canvas, static_cast<double>(TempoRowEnd(song)) / quarter));
    const float bottom = row.Max.y - Px(1.0f);
    ImFont* font = theme::GetFonts().figures;
    for (std::size_t i = 0; i < line.size(); i++)
    {
        const float left = std::floor(XOf(canvas, line[i].tick / quarter));
        const float right = i + 1 < line.size() ? std::floor(XOf(canvas, line[i + 1].tick / quarter)) : end;
        if (left > row.Max.x)
        {
            break;
        }

        const float y = TempoY(row, span, line[i].bpm);
        if (i > 0)
        {
            draw->AddLine(ImVec2(left, TempoY(row, span, line[i - 1].bpm)), ImVec2(left, y), theme::kAccent, Px(1.5f));
        }
        if (right <= left || right < row.Min.x)
        {
            continue;
        }

        draw->AddRectFilled(ImVec2(left, y), ImVec2(right, bottom), theme::WithAlpha(theme::kAccent, 0.16f));
        draw->AddLine(ImVec2(left, y), ImVec2(right, y), theme::kAccent, Px(1.5f));
        const std::string number = TempoText(line[i].bpm);
        const ImVec2 size = TextSize(font, theme::kSmallSize, number);
        if (right - left >= size.x + Px(12.0f))
        {
            const float above = y - Px(3.0f) - size.y;
            const float top = above >= row.Min.y + Px(1.0f) ? above : y + Px(4.0f);
            DrawText(draw, font, theme::kSmallSize, ImVec2(left + Px(6.0f), top), theme::kTextMuted, number);
        }
    }
}

// The tempo row's points. A tempo without one value has a bar over the row, with its source beside it where it fits
// (FittingLabels). The Key Editor's controller lanes draw such values alike. The point under the mouse is light, and so
// is each run of a dragged command.
void DrawTempoPoints(ImDrawList* draw, const Canvas& canvas, const SongView& song, const TempoSpan& span,
                     const std::optional<TempoMove>& drag, std::optional<std::size_t> hovered)
{
    const ImRect& row = canvas.tempo;
    ImFont* font = theme::GetFonts().text;
    std::vector<ImVec2> places;
    std::vector<float> xs;
    std::vector<float> widths;
    for (const TempoPointView& point : song.tempo_points)
    {
        places.push_back(TempoPointPos(canvas, span, point, drag));
        const bool shown = places.back().x >= row.Min.x - Px(8.0f) && places.back().x <= row.Max.x + Px(8.0f);
        xs.push_back(places.back().x);
        widths.push_back(shown && !point.bpm ? TextSize(font, theme::kSmallSize, point.source).x : 0.0f);
    }
    const std::vector<bool> fits = FittingLabels(xs, widths, Px(6.0f));
    for (std::size_t i = 0; i < song.tempo_points.size(); i++)
    {
        const TempoPointView& point = song.tempo_points[i];
        const ImVec2 at = places[i];
        if (at.x < row.Min.x - Px(8.0f) || at.x > row.Max.x + Px(8.0f))
        {
            continue;
        }

        const bool light = hovered == i || (drag && drag->point.command == point.command);
        if (point.bpm)
        {
            DrawPoint(draw, at.x, at.y, at.y, theme::kAccent, light, point.conditional);
            continue;
        }

        DrawPoint(draw, at.x, row.Min.y + Px(kTempoMargin), row.Max.y - Px(kTempoMargin), theme::kAccent, light,
                  point.conditional);
        if (fits[i])
        {
            DrawText(draw, font, theme::kSmallSize, ImVec2(at.x + Px(6.0f), row.Min.y + Px(2.0f)), theme::kTextMuted,
                     point.source);
        }
    }
}

} // namespace

namespace arrangement_canvas
{

namespace
{

// Follows a drag of a tempo point by the mouse, and returns the move that the drag shows. `refused` receives why a
// point that can't move stays at its tick. It also receives where a move stops at the point's room (ShiftOfTempoPoint).
TempoMove FollowTempoDrag(const Canvas& canvas, const ImRect& row, const TempoSpan& span, TempoMove& drag,
                          std::string& refused)
{
    const ImGuiIO& io = ImGui::GetIO();
    if (drag.point.bpm)
    {
        const double room = std::max(row.GetHeight() - Px(kTempoMargin) * 2.0f, 1.0f);
        const double step = (span.high - span.low) / room * (io.KeyShift ? kFineTempoDrag : 1.0);
        drag.to = std::max(drag.to - static_cast<double>(io.MouseDelta.y) * step, 1.0);
    }

    // The move in time starts once the mouse has gone far enough to the side.
    const float across = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0.0f).x;
    if (drag.point.fixed.empty() && (std::abs(across) >= Px(kTempoMoveDistance) || drag.shift != 0))
    {
        const int64_t step = io.KeyShift ? 1 : canvas.ticks_per_quarter;
        const double tick =
            drag.point.tick + static_cast<double>(across / canvas.beat_width) * canvas.ticks_per_quarter;
        const int64_t moved = std::llround(tick / static_cast<double>(step)) * step - drag.point.tick;
        TempoShift shift = ShiftOfTempoPoint(drag.point, moved, canvas.ticks_per_quarter);
        drag.shift = shift.ticks;
        refused = std::move(shift.refused);
    }
    else if (!drag.point.fixed.empty() && std::abs(across) >= Px(kTempoMoveDistance * 3.0f))
    {
        refused = drag.point.fixed;
    }

    return TempoMove{drag.point, drag.from, std::round(drag.to), drag.shift};
}

// A drag shows its tempo and a moved point's start while the mouse holds the point, or why the point stays at its
// tick. When the mouse button is released, another tempo or another tick becomes a move.
void FinishTempoDrag(const Canvas& canvas, const SongView& song, const TempoMove& shown, const std::string& refused,
                     ArrangementState& state)
{
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
    {
        std::string text = shown.point.bpm ? TempoText(shown.to) + " BPM" : shown.point.source;
        if (shown.shift != 0)
        {
            const double start = static_cast<double>(shown.point.tick + shown.shift) / canvas.ticks_per_quarter;
            text += "\nStart " + WithoutFigureSpaces(PositionText(start, song.beats_per_bar));
        }
        ImGui::SetMouseCursor(TempoCursor(shown.point));
        WrappedTooltip(refused.empty() ? text : refused);
        return;
    }

    if (shown.to != shown.from || shown.shift != 0)
    {
        state.tempo_move = shown;
    }
    state.tempo_drag.reset();
}

} // namespace

void DrawTempo(ImDrawList* draw, const Canvas& canvas, const SongView& song, ArrangementState& state)
{
    const ImRect& row = canvas.tempo;
    draw->AddRectFilled(row.Min, row.Max, theme::kPanel);
    draw->AddRectFilled(ImVec2(row.Min.x, row.Max.y - Px(1.0f)), row.Max, theme::kBorder);

    // The row keeps its span during a drag. A drag past the span's ends goes on, and its point stays at the row's edge.
    const ImGuiIO& io = ImGui::GetIO();
    const TempoSpan span = TempoSpanOf(song);
    std::optional<TempoMove> shown;
    std::string refused;
    if (state.tempo_drag)
    {
        shown = FollowTempoDrag(canvas, row, span, *state.tempo_drag, refused);
    }
    const bool over = ImGui::IsWindowHovered() && row.Contains(io.MousePos) && !state.resizing_track_list;
    const std::optional<std::size_t> hovered =
        over && !state.tempo_drag ? TempoPointAt(canvas, song, span, io.MousePos) : std::nullopt;
    DrawTempoLine(draw, canvas, song, TempoLine(song, shown), span);
    DrawTempoPoints(draw, canvas, song, span, shown, hovered);

    if (state.tempo_drag)
    {
        FinishTempoDrag(canvas, song, *shown, refused, state);
        return;
    }

    // The point under the mouse. A tooltip waits while a button is down.
    if (!hovered)
    {
        return;
    }

    const TempoPointView& point = song.tempo_points[*hovered];
    ImGui::SetMouseCursor(TempoCursor(point));
    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
    {
        state.open_tempo = point;
    }
    else if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && (point.bpm || point.fixed.empty()))
    {
        const double bpm = point.bpm.value_or(0.0);
        state.tempo_drag = TempoMove{point, bpm, bpm};
    }
    else if (!ImGui::IsAnyMouseDown())
    {
        WrappedTooltip(TempoTooltip(song, *hovered));
    }
}

bool OverTempoPoint(const Canvas& canvas, const SongView& song, ImVec2 mouse)
{
    return canvas.tempo.Contains(mouse) && TempoPointAt(canvas, song, TempoSpanOf(song), mouse).has_value();
}

void DrawTempoHeader(ImDrawList* draw, const ImRect& header, const SongView& song)
{
    draw->AddRectFilled(header.Min, header.Max, theme::kRaised);
    draw->AddRectFilled(ImVec2(header.Min.x, header.Max.y - Px(1.0f)), header.Max, theme::kBorder);
    draw->AddRectFilled(ImVec2(header.Max.x - Px(1.0f), header.Min.y), header.Max, theme::kBorder);
    const ImVec2 caps = CapsSize("TEMPO");
    DrawCaps(draw, ImVec2(header.Min.x + Px(14.0f), header.GetCenter().y - caps.y * 0.5f), theme::kTextMuted, "TEMPO");

    // The scale lines up with the line's highest and lowest places.
    const TempoSpan span = TempoSpanOf(song);
    ImFont* font = theme::GetFonts().figures;
    const std::string high = TempoText(span.high);
    const std::string low = TempoText(span.low);
    const ImVec2 high_size = TextSize(font, theme::kSmallSize, high);
    const ImVec2 low_size = TextSize(font, theme::kSmallSize, low);
    const float right = header.Max.x - Px(kEdgeGrab) - Px(6.0f);
    DrawText(draw, font, theme::kSmallSize,
             ImVec2(right - high_size.x, header.Min.y + Px(kTempoMargin) - high_size.y * 0.5f), theme::kTextMuted,
             high);
    DrawText(draw, font, theme::kSmallSize,
             ImVec2(right - low_size.x, header.Max.y - Px(kTempoMargin) - low_size.y * 0.5f), theme::kTextMuted, low);
    if (ImGui::IsWindowHovered() && header.Contains(ImGui::GetIO().MousePos) && !ImGui::IsAnyMouseDown())
    {
        WrappedTooltip("Tempo of the sequence, in beats per minute");
    }
}

} // namespace arrangement_canvas
} // namespace hokuto::app
