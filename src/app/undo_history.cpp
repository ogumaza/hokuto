// SPDX-License-Identifier: MIT

// The Undo History: the rows of the actions, and the line under the last action that's done.

#include "app/undo_history.h"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "app/theme.h"
#include "app/views.h"
#include "app/widgets.h"
#include "app/zone_parts.h"

namespace hokuto::app
{
namespace
{

using theme::Px;
using zone_parts::BeginZone;

// The Undo History's sizes, before the display's scale: a row of two lines, and the room above the first row. The line
// lies there when no action is done.
constexpr float kHistoryRowHeight = 42.0f;
constexpr float kHistoryTop = 10.0f;

// The places of the rows and the line in the child window: the window's origin, its width, the height of a row, the
// top of the first row and the margin at each side.
struct HistoryLayout
{
    ImVec2 origin;
    float width = 0.0f;
    float row_height = 0.0f;
    float top = 0.0f;
    float margin = 0.0f;

    // The height of the line at a place between rows. Place 0 is above the first row.
    float LineY(std::size_t place) const
    {
        return top + static_cast<float>(place) * row_height;
    }
};

// Keeps the place of the line and the count of the actions, and scrolls the line into view when either changes.
void ScrollToLine(const HistoryLayout& layout, std::size_t done, std::size_t count, float view_top, float view_bottom)
{
    // A new action, an undo or a redo scrolls the line into view. The values are read and set by their IDs: a pointer
    // into Dear ImGui's storage doesn't survive the insertion of another key.
    ImGuiStorage* storage = ImGui::GetStateStorage();
    const ImGuiID done_id = ImGui::GetID("##done");
    const ImGuiID count_id = ImGui::GetID("##count");
    if (storage->GetInt(done_id, -1) != static_cast<int>(done) ||
        storage->GetInt(count_id, -1) != static_cast<int>(count))
    {
        storage->SetInt(done_id, static_cast<int>(done));
        storage->SetInt(count_id, static_cast<int>(count));
        const float y = layout.LineY(done);
        if (y < view_top + layout.row_height || y > view_bottom - layout.row_height)
        {
            ImGui::SetScrollFromPosY(y - view_top, 0.5f);
        }
    }
}

// Draws the row of one action at height y. Returns whether the user clicked it.
bool DrawHistoryRow(ImDrawList* draw, const HistoryLayout& layout, const HistoryRow& row, std::size_t i,
                    std::size_t done, float y)
{
    const auto& [origin, width, row_height, top, margin] = layout;
    ImFont* text = theme::GetFonts().text;
    ImFont* figures = theme::GetFonts().figures;

    // A name or details that don't fit show in full in the row's tooltip.
    const std::string number = std::to_string(i + 1);
    const float first = y + Px(5.0f);
    const float second = y + Px(23.0f);
    const float name_x = origin.x + Px(42.0f);
    const float right = origin.x + width - margin - Px(8.0f);
    const ImVec2 number_size = TextSize(figures, theme::kSmallSize, number);
    const ImVec2 time_size = TextSize(figures, theme::kSmallSize, row.time);
    const std::array<FittedLine, 2> lines = {
        FittedLine{text, theme::kTextSize, row.name, right - time_size.x - Px(10.0f) - name_x},
        FittedLine{text, theme::kSmallSize, row.details, right - name_x}};

    bool clicked = false;
    ImGui::SetCursorScreenPos(ImVec2(origin.x, y));
    ImGui::PushID(static_cast<int>(i));
    ImGui::SetNextItemAllowOverlap();
    if (ImGui::InvisibleButton("##row", ImVec2(width, row_height)))
    {
        clicked = true;
    }
    if (ImGui::IsItemHovered())
    {
        draw->AddRectFilled(ImVec2(origin.x + margin, y + Px(2.0f)),
                            ImVec2(origin.x + width - margin, y + row_height - Px(2.0f)), theme::kRaisedHover,
                            Px(6.0f));
        ItemTooltip(CutLinesTooltip(lines));
    }
    ImGui::PopID();

    DrawText(draw, figures, theme::kSmallSize, ImVec2(name_x - Px(10.0f) - number_size.x, first + Px(1.5f)),
             theme::kTextMuted, number);
    DrawText(draw, text, theme::kTextSize, ImVec2(name_x, first), i < done ? theme::kText : theme::kTextMuted,
             FitText(lines[0].font, lines[0].size, lines[0].text, lines[0].width));
    DrawText(draw, figures, theme::kSmallSize, ImVec2(right - time_size.x, first + Px(1.5f)), theme::kTextMuted,
             row.time);
    DrawText(draw, text, theme::kSmallSize, ImVec2(name_x, second), theme::kTextMuted,
             FitText(lines[1].font, lines[1].size, lines[1].text, lines[1].width));

    return clicked;
}

// Draws the line under the last action that's done. Returns the place that a drag of the line's handle moves it to.
std::optional<std::size_t> DrawHistoryLine(ImDrawList* draw, const HistoryLayout& layout, std::size_t done,
                                           std::size_t count)
{
    const auto& [origin, width, row_height, top, margin] = layout;
    std::optional<std::size_t> chosen;

    // The line, with a handle on its left end. A drag of the handle moves the line to the nearest place between rows,
    // or above the first row to undo every action.
    const float y = layout.LineY(done);
    const float left = origin.x + margin;
    ImGui::SetCursorScreenPos(ImVec2(left - Px(4.0f), y - Px(9.0f)));
    ImGui::InvisibleButton("##line", ImVec2(Px(20.0f), Px(18.0f)));
    if (ImGui::IsItemActive())
    {
        const float place = std::round((ImGui::GetIO().MousePos.y - top) / row_height);
        const auto dragged = static_cast<std::size_t>(std::clamp(place, 0.0f, static_cast<float>(count)));
        if (dragged != done)
        {
            chosen = dragged;
        }
    }
    if (ImGui::IsItemHovered() || ImGui::IsItemActive())
    {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
    }
    draw->AddRectFilled(ImVec2(left, y - Px(1.0f)), ImVec2(origin.x + width - margin, y + Px(1.0f)), theme::kAccent);
    draw->AddTriangleFilled(ImVec2(left, y - Px(6.0f)), ImVec2(left + Px(8.0f), y), ImVec2(left, y + Px(6.0f)),
                            theme::kAccent);

    return chosen;
}

// The Undo History's rows and its line, in a child window that scrolls. Each row shows the action's number, name and
// time, with the details below. The line lies under the last action that's done. Returns the number of actions that the
// user chose to have done.
std::optional<std::size_t> DrawHistoryRows(std::span<const HistoryRow> rows, std::size_t done)
{
    ImGui::BeginChild("##rows", ImVec2(0.0f, 0.0f));
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const float row_height = Px(kHistoryRowHeight);
    const float top = origin.y + Px(kHistoryTop);
    const float margin = Px(8.0f);
    const float view_top = ImGui::GetWindowPos().y;
    const float view_bottom = view_top + ImGui::GetWindowHeight();
    const HistoryLayout layout = {
        .origin = origin, .width = width, .row_height = row_height, .top = top, .margin = margin};
    ScrollToLine(layout, done, rows.size(), view_top, view_bottom);

    // The rows in view. The actions that undo stepped back over have dimmer names. The line's handle comes after the
    // rows and lies over them. Each row allows the overlap. The handle then gets the clicks over it.
    std::optional<std::size_t> chosen;
    for (std::size_t i = 0; i < rows.size(); i++)
    {
        const float y = layout.LineY(i);
        if (y + row_height < view_top || y > view_bottom)
        {
            continue;
        }

        if (DrawHistoryRow(draw, layout, rows[i], i, done, y))
        {
            chosen = i + 1;
        }
    }

    // The line under the last action that's done.
    if (const std::optional<std::size_t> dragged = DrawHistoryLine(draw, layout, done, rows.size()))
    {
        chosen = dragged;
    }

    // The rows set the height that scrolls.
    ImGui::SetCursorScreenPos(ImVec2(origin.x, layout.LineY(rows.size())));
    ImGui::Dummy(ImVec2(width, Px(kHistoryTop)));
    ImGui::EndChild();

    return chosen;
}

} // namespace

std::optional<std::size_t> DrawUndoHistory(std::span<const HistoryRow> rows, std::size_t done, bool* open)
{
    if (!*open)
    {
        return std::nullopt;
    }

    // The header counts the actions that are done.
    std::optional<std::size_t> chosen;
    if (BeginZone(kUndoHistory, open))
    {
        ZoneHeader("Undo History",
                   rows.empty() ? std::string() : std::to_string(done) + " of " + std::to_string(rows.size()));
        if (rows.empty())
        {
            const ImVec2 pos = ImGui::GetCursorScreenPos();
            const ImVec2 avail = ImGui::GetContentRegionAvail();
            EmptyState(ImGui::GetWindowDrawList(), pos, ImVec2(pos.x + avail.x, pos.y + avail.y), "No edits yet",
                       "Each edit appears here.");
        }
        else
        {
            chosen = DrawHistoryRows(rows, done);
        }
    }
    ImGui::End();

    return chosen;
}

} // namespace hokuto::app
