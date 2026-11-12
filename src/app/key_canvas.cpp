// SPDX-License-Identifier: MIT

// The layout of the Key Editor's canvas: the places of the ticks, keys, notes, channel bars, lanes and lane points, and
// what is under the mouse.

#include "app/key_canvas.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/commands/units.h"

#include "app/key_editor.h"
#include "app/key_edits.h"
#include "app/lanes.h"
#include "app/theme.h"
#include "edit/commands.h"

namespace hokuto::app
{
namespace
{

using ntdlib::commands::Fixed;
using ntdlib::commands::Source;
using theme::Px;

} // namespace

namespace key_canvas
{

float XOf(const Canvas& canvas, double tick)
{
    return canvas.notes.Min.x + static_cast<float>(tick / canvas.ticks_per_quarter) * canvas.beat_width -
           canvas.scroll_x;
}

double TickAt(const Canvas& canvas, float x)
{
    return static_cast<double>(x - canvas.notes.Min.x + canvas.scroll_x) / canvas.beat_width * canvas.ticks_per_quarter;
}

float RowTop(const Canvas& canvas, int key)
{
    return canvas.notes.Min.y + static_cast<float>(kKeys - 1 - key) * canvas.key_height - canvas.scroll_y;
}

int KeyAt(const Canvas& canvas, float y)
{
    const auto row = static_cast<int>(std::floor((y - canvas.notes.Min.y + canvas.scroll_y) / canvas.key_height));
    return std::clamp(kKeys - 1 - row, 0, kKeys - 1);
}

ImRect NoteRect(const Canvas& canvas, const KeyNote& note, int32_t shift, int transpose, int32_t resize)
{
    const double start = static_cast<double>(note.start) + shift;
    const double length = std::max(static_cast<double>(note.length) + resize, 1.0);
    const float left = std::floor(XOf(canvas, start));
    const float right = std::max(std::floor(XOf(canvas, start + length)), left + Px(3.0f));
    const float top = RowTop(canvas, note.key + transpose);
    return ImRect(ImVec2(left, top + Px(1.0f)), ImVec2(right, top + canvas.key_height - Px(1.0f)));
}

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

std::optional<std::size_t> UnstartedAt(const Canvas& canvas, const KeyTrack& track, ImVec2 pos)
{
    for (std::size_t i = track.unstarted.size(); i-- > 0;)
    {
        if (NoteRect(canvas, track.unstarted[i], 0, 0, 0).Contains(pos))
        {
            return i;
        }
    }

    return std::nullopt;
}

float ChannelRowHeight()
{
    return Px(kChannelRowHeight);
}

ImRect ChannelBarRect(const Canvas& canvas, const ChannelBar& bar)
{
    const float row = ChannelRowHeight();
    const float top = canvas.channels.Min.y + Px(1.0f) + row * static_cast<float>(bar.channel - 1);
    const float left = std::floor(XOf(canvas, bar.start));
    const float right =
        std::max(std::floor(XOf(canvas, static_cast<double>(bar.start) + std::max(bar.length, 1u))), left + Px(3.0f));
    return ImRect(ImVec2(left, top + Px(2.0f)), ImVec2(right, top + row - Px(2.0f)));
}

std::optional<std::size_t> ChannelBarAt(const Canvas& canvas, const KeyTrack& track, ImVec2 pos)
{
    for (std::size_t i = track.channels.size(); i-- > 0;)
    {
        if (ChannelBarRect(canvas, track.channels[i]).Contains(pos))
        {
            return i;
        }
    }

    return std::nullopt;
}

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

float LaneTop(const ImRect& lane)
{
    return lane.Min.y + Px(10.0f);
}

float LaneBottom(const ImRect& lane)
{
    return lane.Max.y - Px(4.0f);
}

float ValueY(const ImRect& lane, const edit::ValueRange& range, Fixed value)
{
    const double amount = static_cast<double>(value - range.lowest) /
                          static_cast<double>(std::max<Fixed>(range.highest - range.lowest, 1));
    return LaneBottom(lane) - (LaneBottom(lane) - LaneTop(lane)) * static_cast<float>(std::clamp(amount, 0.0, 1.0));
}

Fixed ValueAt(const ImRect& lane, const KeyTrack& track, LaneSetting setting, const edit::ValueRange& range, float y)
{
    const double amount = static_cast<double>((LaneBottom(lane) - y) / (LaneBottom(lane) - LaneTop(lane)));
    const double value = static_cast<double>(range.lowest) + amount * static_cast<double>(range.highest - range.lowest);
    return NearestValue(track.console, setting, static_cast<Fixed>(std::llround(value)), range);
}

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

float LanesBottom(const Canvas& canvas)
{
    return canvas.lanes.empty() ? canvas.lane.Max.y : canvas.lanes.back().Max.y;
}

} // namespace key_canvas
} // namespace hokuto::app
