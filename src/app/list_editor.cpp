// SPDX-License-Identifier: MIT

#include "app/list_editor.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ntdlib/commands/sequence.h"

#include "app/key_edits.h"
#include "app/theme.h"
#include "app/views.h"
#include "app/widgets.h"
#include "song/archive.h"

namespace hokuto::app
{
namespace
{

using ntdlib::commands::Kind;
using theme::Px;

// The editor's sizes, before the display's scale.
constexpr float kToolbarHeight = 40.0f;
constexpr float kHeaderHeight = 26.0f;
constexpr float kRowHeight = 24.0f;
constexpr float kMargin = 10.0f;
constexpr float kIndent = 14.0f;
constexpr float kToolSize = 28.0f;
constexpr float kLeastComment = 120.0f;

// The payload of a label's row that a drag carries: the row's index.
constexpr const char* kLabelPayload = "HOKUTO_LIST_LABEL";

// The widths of the columns before the comment. The comment takes the rest.
constexpr std::array<float, kListColumns - 1> kColumnWidths = {190.0f, 110.0f, 110.0f, 170.0f, 170.0f};
constexpr std::array<const char*, kListColumns> kColumnNames = {"Type",   "Start",  "Length",
                                                                "Data 1", "Data 2", "Comment"};

// The left edge of each column from `left`, then the right edge of the comment at `right`, or further for a narrow
// list.
std::array<float, kListColumns + 1> ColumnEdges(float left, float right)
{
    std::array<float, kListColumns + 1> edges{};
    edges[0] = left;
    for (std::size_t i = 0; i < kColumnWidths.size(); i++)
    {
        edges[i + 1] = edges[i] + Px(kColumnWidths[i]);
    }
    edges[kListColumns] = std::max(right, edges[kListColumns - 1] + Px(kLeastComment));

    return edges;
}

// The column under `x`, or nothing past the edges.
std::optional<ListColumn> ColumnAt(const std::array<float, kListColumns + 1>& edges, float x)
{
    for (std::size_t i = 0; i < kListColumns; i++)
    {
        if (x >= edges[i] && x < edges[i + 1])
        {
            return static_cast<ListColumn>(i);
        }
    }

    return std::nullopt;
}

// The first row whose command is selected, or nothing.
std::optional<std::size_t> FirstSelected(const ListTrack& track, const ListEditorState& state)
{
    for (std::size_t i = 0; i < track.rows.size(); i++)
    {
        if (!IsLabel(track.rows[i]) && state.selected.contains(track.rows[i].row.command))
        {
            return i;
        }
    }

    return std::nullopt;
}

// Whether row `index` is selected: its label, or its command.
bool Selected(const ListTrack& track, const ListEditorState& state, std::size_t index)
{
    const ListRow& row = track.rows[index];
    return IsLabel(row) ? state.selected_label == row.label : state.selected.contains(row.row.command);
}

// Selects the label or the command of row `index` alone, and scrolls to it.
void SelectRow(const ListTrack& track, ListEditorState& state, std::size_t index)
{
    const ListRow& row = track.rows[index];
    state.selected.clear();
    state.selected_label.reset();
    if (IsLabel(row))
    {
        state.selected_label = row.label;
    }
    else
    {
        state.selected = {row.row.command};
    }
    state.anchor = index;
    state.scroll_row = index;
}

// The tooltip of a kind of command that the player of the file's console skips.
std::string SkippedKindText(const ListTrack& track)
{
    return "The " + track.file_engine_name + "'s player skips this kind of command.";
}

// The menus of the kinds of command, a menu for each group. The kinds that the player of the track's console skips show
// grey. Returns the kind that the user chose.
std::optional<Kind> KindMenus(bool enabled, const ListTrack& track)
{
    std::optional<Kind> chosen;
    for (const KindGroup& group : InsertGroups())
    {
        if (!ImGui::BeginMenu(group.name, enabled))
        {
            continue;
        }

        for (const Kind kind : group.kinds)
        {
            const std::string name(KindName(kind));
            const bool runs = song::Runs(track.console, kind, 0);
            if (ImGui::MenuItem(name.c_str(), nullptr, false, runs))
            {
                chosen = kind;
            }
            if (!runs)
            {
                ItemTooltip(SkippedKindText(track));
            }
        }
        ImGui::EndMenu();
    }

    return chosen;
}

// Places the next control of the toolbar at `x`, in the middle of the bar's height.
void PlaceInToolbar(float x, float top, float height)
{
    ImGui::SetCursorScreenPos(ImVec2(x, top + (Px(kToolbarHeight) - height) * 0.5f));
}

// Draws a label in capitals at `x` in the middle of the toolbar, and returns the place after it.
float ToolbarCaps(ImDrawList* draw, float x, float top, const char* label)
{
    const ImVec2 caps = CapsSize(label);
    DrawCaps(draw, ImVec2(x, top + (Px(kToolbarHeight) - caps.y) * 0.5f), theme::kTextMuted, label);

    return x + caps.x + Px(8.0f);
}

// Draws the name of the subroutine that the editor shows by itself, and the menu of its runs. Returns the place after
// them.
float DrawSubroutineTools(const ListTrack& track, ListEditorState& state, ImDrawList* draw, ImVec2 origin)
{
    const ListSubroutine& subroutine = *track.subroutine;
    float x = ToolbarCaps(draw, origin.x + Px(14.0f), origin.y, "SUBROUTINE");
    ImFont* text = theme::GetFonts().text;
    const std::string name = FitText(text, theme::kTextSize,
                                     subroutine.name.empty() ? std::string("Subroutine") : subroutine.name, Px(200.0f));
    const ImVec2 size = TextSize(text, theme::kTextSize, name);
    DrawText(draw, text, theme::kTextSize, ImVec2(x, origin.y + (Px(kToolbarHeight) - size.y) * 0.5f), theme::kText,
             name);

    // Each run names its track and the position of its call.
    const auto run_name = [&track](const ListCall& run)
    {
        const double quarters = static_cast<double>(run.tick) / std::max(track.ticks_per_quarter, 1);
        return "Track " + std::to_string(run.track) + " at " +
               WithoutFigureSpaces(PositionText(quarters, kBeatsPerBar));
    };

    x = ToolbarCaps(draw, x + size.x + Px(20.0f), origin.y, "CALL");
    PlaceInToolbar(x, origin.y, ImGui::GetFrameHeight());
    ImGui::SetNextItemWidth(Px(190.0f));
    const bool any = subroutine.shown < subroutine.runs.size();
    const std::string shown = any ? run_name(subroutine.runs[subroutine.shown]) : std::string("None");
    if (ImGui::BeginCombo("##call", shown.c_str()))
    {
        for (std::size_t i = 0; i < subroutine.runs.size(); i++)
        {
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::Selectable(run_name(subroutine.runs[i]).c_str(), i == subroutine.shown) && i != subroutine.shown)
            {
                state.open_subroutine = ListEditorState::SubroutineChoice{subroutine.first, subroutine.runs[i]};
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }

    return x + Px(190.0f) + Px(20.0f);
}

// The toolbar: the track that the editor shows, and the kind of command that Insert adds, with the Insert button. A
// subroutine that the editor shows by itself takes the track's place, with a menu of its runs, and Show Track at the
// bar's right end. Returns the edit of a click on Insert.
std::optional<ListEdit> DrawToolbar(const ListTrack& track, ListEditorState& state, bool busy)
{
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const float bottom = origin.y + Px(kToolbarHeight);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(origin, ImVec2(origin.x + width, bottom), theme::kRaised);
    draw->AddRectFilled(ImVec2(origin.x, bottom - Px(1.0f)), ImVec2(origin.x + width, bottom), theme::kBorder);

    // The tracks that ran commands, with notes or without.
    float x = 0.0f;
    if (track.subroutine)
    {
        x = DrawSubroutineTools(track, state, draw, origin);
    }
    else
    {
        x = ToolbarCaps(draw, origin.x + Px(14.0f), origin.y, "TRACK");
        PlaceInToolbar(x, origin.y, ImGui::GetFrameHeight());
        ImGui::SetNextItemWidth(Px(110.0f));
        const std::string shown = "Track " + std::to_string(track.number);
        if (ImGui::BeginCombo("##track", shown.c_str()))
        {
            for (const int number : track.tracks)
            {
                const std::string name = "Track " + std::to_string(number);
                if (ImGui::Selectable(name.c_str(), number == track.number) && number != track.number)
                {
                    state.chosen_track = number;
                }
            }
            ImGui::EndCombo();
        }
        x += Px(110.0f) + Px(20.0f);
    }

    // The kind of command that Insert adds. A kind that the track's console skips, chosen for another file, gives way
    // to the volume. Every console runs the volume.
    if (!song::Runs(track.console, state.insert_kind, 0))
    {
        state.insert_kind = Kind::kVolume;
    }
    x = ToolbarCaps(draw, x, origin.y, "INSERT");
    PlaceInToolbar(x, origin.y, ImGui::GetFrameHeight());
    ImGui::SetNextItemWidth(Px(170.0f));
    const std::string kind(KindName(state.insert_kind));
    if (ImGui::BeginCombo("##insert", kind.c_str(), ImGuiComboFlags_HeightLargest))
    {
        for (const KindGroup& group : InsertGroups())
        {
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme::kTextMuted), "%s", group.name);
            ImGui::Indent(Px(10.0f));
            for (const Kind choice : group.kinds)
            {
                const std::string name(KindName(choice));
                const bool runs = song::Runs(track.console, choice, 0);
                if (ImGui::Selectable(name.c_str(), choice == state.insert_kind,
                                      runs ? ImGuiSelectableFlags_None : ImGuiSelectableFlags_Disabled))
                {
                    state.insert_kind = choice;
                }
                if (!runs)
                {
                    ItemTooltip(SkippedKindText(track));
                }
            }
            ImGui::Unindent(Px(10.0f));
        }
        ImGui::EndCombo();
    }

    // Insert puts the command before the first selected row, or before the last row.
    x += Px(170.0f) + Px(6.0f);
    const ImVec2 button(Px(kToolSize), Px(kToolSize));
    PlaceInToolbar(x, origin.y, button.y);
    std::optional<ListEdit> edit;
    if (IconButton("##add", Icon::kAdd, button, false, 0, "Insert") && !busy)
    {
        edit = InsertEdit(track, FirstSelected(track, state), state.insert_kind);
    }

    // Show Track, at the bar's right end in a subroutine's view. In a narrow zone, it follows Insert instead.
    float right = origin.x + width - Px(14.0f);
    if (track.subroutine)
    {
        const char* label = "Show Track";
        const float button_width = ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2.0f;
        right = std::max(right - button_width, x + button.x + Px(20.0f));
        PlaceInToolbar(right, origin.y, ImGui::GetFrameHeight());
        if (ImGui::Button(label))
        {
            state.close_subroutine = true;
        }
        right -= Px(14.0f);
    }

    // The number of commands, and of the calls that go to a subroutine that the editor shows by itself.
    std::size_t commands = 0;
    for (const ListRow& row : track.rows)
    {
        commands += IsLabel(row) ? 0 : 1;
    }
    std::string count = commands == 1 ? "1 command" : std::to_string(commands) + " commands";
    if (track.subroutine)
    {
        count = CallsText(*track.subroutine) + ", " + count;
    }
    // Where the bar is too narrow, the count is cut short with an ellipsis, and its tooltip gives the whole count. The
    // tooltip of a subroutine's count says where the subroutine's other calls lie.
    ImFont* text = theme::GetFonts().text;
    const float room = right - (x + button.x + Px(20.0f));
    const std::string shown = room > 0.0f ? FitText(text, theme::kSmallSize, count, room) : std::string();
    if (!shown.empty())
    {
        const ImVec2 size = TextSize(text, theme::kSmallSize, shown);
        const ImVec2 at(right - size.x, origin.y + (Px(kToolbarHeight) - size.y) * 0.5f);
        DrawText(draw, text, theme::kSmallSize, at, theme::kTextMuted, shown);

        std::string tooltip = shown == count ? std::string() : count + ".";
        if (track.subroutine)
        {
            tooltip += (tooltip.empty() ? "" : " ") + CallsTooltip(*track.subroutine);
        }
        if (!tooltip.empty())
        {
            ImGui::SetCursorScreenPos(at);
            ImGui::InvisibleButton("##calls", size);
            ItemTooltip(tooltip);
        }
    }

    ImGui::SetCursorScreenPos(ImVec2(origin.x, bottom));

    return edit;
}

// The columns' names over the rows.
void DrawColumnNames(const std::array<float, kListColumns + 1>& edges)
{
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const float bottom = origin.y + Px(kHeaderHeight);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(origin, ImVec2(origin.x + width, bottom), theme::kPanelAlt);
    draw->AddRectFilled(ImVec2(origin.x, bottom - Px(1.0f)), ImVec2(origin.x + width, bottom), theme::kBorder);
    for (std::size_t i = 0; i < kListColumns; i++)
    {
        const ImVec2 caps = CapsSize(kColumnNames[i]);
        DrawCaps(draw, ImVec2(edges[i] + Px(kMargin), origin.y + (Px(kHeaderHeight) - caps.y) * 0.5f),
                 theme::kTextMuted, kColumnNames[i]);
    }

    ImGui::SetCursorScreenPos(ImVec2(origin.x, bottom));
}

// Selects row `index` for a click. Shift selects the rows from the last click to this one, and Ctrl adds the row's
// command or takes it away. Lists on the Mac and on Windows select the same way.
void ClickRow(const ListTrack& track, ListEditorState& state, std::size_t index)
{
    const ImGuiIO& io = ImGui::GetIO();
    const uint32_t command = track.rows[index].row.command;
    if (IsLabel(track.rows[index]))
    {
        SelectRow(track, state, index);
        state.scroll_row.reset();
        return;
    }

    state.selected_label.reset();
    if (io.KeyShift && state.anchor && *state.anchor < track.rows.size())
    {
        const auto [low, high] = std::minmax(*state.anchor, index);
        state.selected.clear();
        for (std::size_t i = low; i <= high; i++)
        {
            if (!IsLabel(track.rows[i]))
            {
                state.selected.insert(track.rows[i].row.command);
            }
        }
        return;
    }

    if (io.KeyCtrl)
    {
        if (!state.selected.erase(command))
        {
            state.selected.insert(command);
        }
    }
    else
    {
        state.selected = {command};
    }
    state.anchor = index;
}

// Starts typing in `column` of row `index`, with the cell's text in the field.
void StartTyping(const ListTrack& track, ListEditorState& state, std::size_t index, ListColumn column)
{
    const std::string text = WithoutFigureSpaces(CellText(track, track.rows[index], column));
    state.typing = std::pair(index, column);
    state.text = {};
    text.copy(state.text.data(), state.text.size() - 1);
    state.focus = true;
}

// Draws the cells of row `index` from `y`. The type sits further in for each call or loop on the track's stack, with a
// line for each. A command that runs only if the last comparison held has an "if" before its type. A start that isn't
// known shows as a dash. A command that the engine's player skips shows grey.
void DrawCells(ImDrawList* draw, const ListTrack& track, std::size_t index, float y,
               const std::array<float, kListColumns + 1>& edges)
{
    const ListRow& row = track.rows[index];
    const bool skipped = Skipped(track, row);
    ImFont* text = theme::GetFonts().text;
    ImFont* figures = theme::GetFonts().figures;
    const float middle = y + Px(kRowHeight) * 0.5f;
    const float margin = Px(kMargin);
    const auto draw_text = [&](ImFont* font, float x, float right, ImU32 colour, std::string_view value)
    {
        const ImVec2 size = TextSize(font, theme::kTextSize, value);
        DrawText(draw, font, theme::kTextSize, ImVec2(x, std::floor(middle - size.y * 0.5f)), colour,
                 FitText(font, theme::kTextSize, value, right - x));
        return x + std::min(size.x, right - x);
    };

    float type_x = edges[0] + margin;
    for (int level = 0; level < row.depth; level++)
    {
        const float line_x = type_x + Px(3.0f);
        draw->AddRectFilled(ImVec2(line_x, y), ImVec2(line_x + Px(1.0f), y + Px(kRowHeight)), theme::kBorderStrong);
        type_x += Px(kIndent);
    }
    if (row.command.conditional && !IsLabel(row))
    {
        type_x = draw_text(text, type_x, edges[1] - margin, theme::kCycle, "if") + Px(6.0f);
    }
    draw_text(text, type_x, edges[1] - margin,
              IsLabel(row) ? theme::kAccent
              : skipped    ? theme::kTextMuted
                           : theme::kText,
              CellText(track, row, ListColumn::kType));

    for (std::size_t i = 1; i < kListColumns; i++)
    {
        const auto column = static_cast<ListColumn>(i);
        std::string value = CellText(track, row, column);
        ImU32 colour = column == ListColumn::kComment || skipped ? theme::kTextMuted : theme::kText;
        if (column == ListColumn::kStart && value.empty())
        {
            value = "—";
            colour = theme::kTextDim;
        }
        draw_text(column == ListColumn::kComment ? text : figures, edges[i] + margin, edges[i + 1] - margin, colour,
                  value);
    }
}

// The text field over the cell that the user types in. Enter, Tab or a click elsewhere returns the text, and Escape
// puts the value back. The Key Editor's info line works the same way.
std::optional<std::string> DrawTyping(ListEditorState& state, float top,
                                      const std::array<float, kListColumns + 1>& edges)
{
    const auto [index, column] = *state.typing;
    const auto i = static_cast<std::size_t>(column);
    const float y = top + static_cast<float>(index) * Px(kRowHeight);
    const ImVec2 pad(Px(4.0f), Px(2.0f));
    const float height = ImGui::GetFontSize() + pad.y * 2.0f;
    ImGui::SetCursorScreenPos(ImVec2(edges[i] + Px(kMargin) - pad.x, y + (Px(kRowHeight) - height) * 0.5f));
    ImGui::SetNextItemWidth(edges[i + 1] - edges[i] - Px(kMargin));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, pad);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImGui::ColorConvertU32ToFloat4(theme::kDisplay));
    const bool focusing = std::exchange(state.focus, false);
    if (focusing)
    {
        ImGui::SetKeyboardFocusHere();
    }
    // The field keeps the cell's font.
    if (column != ListColumn::kComment)
    {
        PushFigures();
    }
    ImGui::InputText("##typing", state.text.data(), state.text.size(), ImGuiInputTextFlags_AutoSelectAll);
    if (column != ListColumn::kComment)
    {
        ImGui::PopFont();
    }
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();

    // A click elsewhere ends the typing before anything else takes the click. A field that lost the focus without
    // ending ends too.
    std::optional<std::string> typed;
    const bool active = ImGui::IsItemActive();
    const bool away = active && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsItemHovered();
    if (ImGui::IsItemDeactivated() || away)
    {
        typed = std::string(state.text.data());
        state.typing.reset();
    }
    else if (!active && !focusing)
    {
        state.typing.reset();
    }

    return typed;
}

// The tooltip of the menu items that a timeline's labels and subroutines don't take.
constexpr const char* kTimelineOnly =
    "A timeline's labels and subroutines come from the converter. Program in the inspector's Sequence section lets "
    "the sequence take this edit.";

// Asks the program to show the subroutine at offset `first` by itself, from the run `run` or else from its first run.
void OpenSubroutine(ListEditorState& state, uint32_t first, std::optional<ListCall> run)
{
    state.open_subroutine = ListEditorState::SubroutineChoice{first, run};
}

// The context menu of a label's row: Rename Label, Open Subroutine and Delete Label. Returns the edit that the user
// chose.
std::optional<ListEdit> DrawLabelMenu(const ListTrack& track, ListEditorState& state, bool busy, std::size_t index)
{
    std::optional<ListEdit> edit;
    const ListRow& row = track.rows[index];
    if (ImGui::MenuItem("Rename Label", nullptr, false, !busy && !track.timeline))
    {
        StartTyping(track, state, index, ListColumn::kData1);
    }
    if (track.timeline)
    {
        ItemTooltip(kTimelineOnly);
    }
    const bool runs = track.runs.contains(row.row.command);
    if (ImGui::MenuItem("Open Subroutine", nullptr, false, runs))
    {
        OpenSubroutine(state, row.row.command, std::nullopt);
    }
    ItemTooltip(runs ? "Shows the subroutine at the label by itself, with its calls."
                     : "No call of the shown sound goes to the label.");

    ImGui::Separator();
    if (ImGui::MenuItem("Delete Label", nullptr, false, !busy && !track.timeline))
    {
        edit = LabelRemovalEdit(track, index);
    }
    if (track.timeline)
    {
        ItemTooltip(kTimelineOnly);
    }

    return edit;
}

// The items of a row's context menu that edit labels and subroutines: Insert Label, Convert to Subroutine, and for a
// call Open Subroutine, Dissolve Call and Convert to Real Copy. Returns the edit that the user chose.
std::optional<ListEdit> DrawSubroutineItems(const ListTrack& track, ListEditorState& state, bool busy,
                                            std::size_t index)
{
    std::optional<ListEdit> edit;
    const ListRow& row = track.rows[index];
    const bool program = !busy && !track.timeline;
    if (ImGui::MenuItem("Insert Label", nullptr, false, program))
    {
        edit = LabelEdit(track, index);
    }
    if (track.timeline)
    {
        ItemTooltip(kTimelineOnly);
    }
    if (ImGui::MenuItem("Convert to Subroutine", nullptr, false, program))
    {
        edit = SubroutineEdit(track, state.selected);
    }
    ItemTooltip(track.timeline ? kTimelineOnly
                               : "The selected commands go after the sequence's commands as a "
                                 "subroutine, with a call in their place.");
    if (row.command.kind != Kind::kCall)
    {
        return edit;
    }

    const std::optional<uint32_t> run = RunOfCall(track, index);
    if (ImGui::MenuItem("Open Subroutine", nullptr, false, run.has_value()))
    {
        const song::Stretch& stretch = track.stretches[*run];
        OpenSubroutine(state, stretch.first, ListCall{track.number, *run, stretch.tick});
    }
    if (ImGui::MenuItem("Dissolve Call", nullptr, false, program))
    {
        edit = DissolveEdit(track, index);
    }
    ItemTooltip(track.timeline ? kTimelineOnly : "A copy of the subroutine's commands takes the call's place.");
    if (ImGui::MenuItem("Convert to Real Copy", nullptr, false, program))
    {
        edit = RealCopyEdit(track, index);
    }
    ItemTooltip(track.timeline ? kTimelineOnly
                               : "The call goes to a copy of its subroutine, which no other call "
                                 "plays.");

    return edit;
}

// The context menu of a row: a command to insert before it, its condition, its target, the edits of labels and
// subroutines, and Delete. A label's row has another menu (DrawLabelMenu). Returns the edit that the user chose.
std::optional<ListEdit> DrawRowMenu(const ListTrack& track, ListEditorState& state, bool busy)
{
    if (!ImGui::BeginPopup("##row"))
    {
        state.menu_row.reset();
        return std::nullopt;
    }

    std::optional<ListEdit> edit;
    const std::size_t index = state.menu_row.value_or(0);
    if (index >= track.rows.size())
    {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return std::nullopt;
    }

    const ListRow& row = track.rows[index];
    if (IsLabel(row))
    {
        edit = DrawLabelMenu(track, state, busy, index);
        ImGui::EndPopup();
        return edit;
    }

    if (ImGui::BeginMenu("Insert Before", !busy))
    {
        if (const std::optional<Kind> kind = KindMenus(!busy, track))
        {
            edit = InsertEdit(track, index, *kind);
        }
        ImGui::EndMenu();
    }
    if (ImGui::MenuItem("Condition", nullptr, row.command.conditional, !busy))
    {
        edit = ConditionEdit(track, index);
    }
    ItemTooltip("The command runs only if the last comparison held.");

    // Go to Target selects the first row that runs the target. The target of a track start runs on the other track, and
    // Go to Target shows that track from its start.
    if (ntdlib::commands::HasTarget(row.command.kind))
    {
        const uint32_t target = row.command.target;
        const auto found = std::find_if(track.rows.begin(), track.rows.end(),
                                        [&](const ListRow& other)
                                        {
                                            return !row.command.target_outside && target < track.offsets.size() &&
                                                   other.row.command == track.offsets[target];
                                        });
        const int opened = static_cast<int>(row.command.index);
        const bool other = row.command.kind == Kind::kOpenTrack && found == track.rows.end() &&
                           std::find(track.tracks.begin(), track.tracks.end(), opened) != track.tracks.end();
        if (ImGui::MenuItem("Go to Target", nullptr, false, found != track.rows.end() || other))
        {
            if (found != track.rows.end())
            {
                SelectRow(track, state, static_cast<std::size_t>(found - track.rows.begin()));
            }
            else
            {
                state.chosen_track = opened;
            }
        }
    }

    ImGui::Separator();
    if (std::optional<ListEdit> chosen = DrawSubroutineItems(track, state, busy, index))
    {
        edit = std::move(chosen);
    }

    ImGui::Separator();
    if (ImGui::MenuItem("Delete", nullptr, false, !busy))
    {
        edit = RemovalEdit(track, state.selected);
    }
    ImGui::EndPopup();

    return edit;
}

// The menu of the sequence's labels for the target of a jump, a call or a track start, with a field that filters them.
// Returns the edit of a choice.
std::optional<ListEdit> DrawTargetMenu(const ListTrack& track, ListEditorState& state, bool busy)
{
    if (!ImGui::BeginPopup("##targets"))
    {
        state.target_row.reset();
        return std::nullopt;
    }

    // The labels in the order of the file, then the end of the sequence. The menu is as wide as the longest name, up to
    // a limit.
    const uint32_t end = track.offsets.empty() ? 0 : static_cast<uint32_t>(track.offsets.size() - 1);
    std::vector<std::pair<std::string, uint32_t>> targets = track.targets;
    targets.emplace_back("End", end);
    float width = Px(240.0f);
    for (const auto& target : targets)
    {
        width = std::max(width, ImGui::CalcTextSize(target.first.c_str()).x + ImGui::GetStyle().ScrollbarSize +
                                    ImGui::GetStyle().ItemSpacing.x * 2.0f);
    }
    width = std::min(width, Px(480.0f));

    // The menu opens with an empty search, and the current target in view.
    const bool appearing = ImGui::IsWindowAppearing();
    if (appearing)
    {
        state.filter.fill('\0');
        ImGui::SetKeyboardFocusHere();
    }

    // A click on a target's cell opens the menu. The second click of a double-click there lands outside the menu, and
    // Dear ImGui closes the menu at the frame's end. The rows then open it again in the next frame.
    if (!appearing && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) &&
        !ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem))
    {
        state.reopen_targets = true;
    }
    ImGui::SetNextItemWidth(width);
    ImGui::InputTextWithHint("##filter", "Search", state.filter.data(), state.filter.size());

    std::optional<ListEdit> edit;
    const std::size_t index = state.target_row.value_or(0);
    ImGui::BeginChild("##labels", ImVec2(width, Px(260.0f)));
    for (std::size_t i = 0; i < targets.size(); i++)
    {
        const auto& [name, item] = targets[i];
        if (!Matches(name, state.filter.data()))
        {
            continue;
        }

        ImGui::PushID(static_cast<int>(i));
        const bool current = index < track.rows.size() && track.rows[index].command.target == item;
        if (ImGui::Selectable(name.c_str(), current) && !busy)
        {
            edit = TargetEdit(track, index, item);
            ImGui::CloseCurrentPopup();
        }
        if (current && appearing)
        {
            ImGui::SetScrollHereY(0.5f);
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::EndPopup();

    return edit;
}

// The keys while the editor has the focus and nobody types: Delete or Backspace deletes the selected commands or the
// selected label, the arrows select the row above or below, and Escape selects nothing. The Edit menu's Select All
// selects every command (DrawListEditor). Returns the edit of a deletion.
std::optional<ListEdit> HandleKeys(const ListTrack& track, ListEditorState& state, bool busy)
{
    if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false))
    {
        if (busy)
        {
            return std::nullopt;
        }
        if (state.selected_label)
        {
            const auto label = std::find_if(track.rows.begin(), track.rows.end(),
                                            [&](const ListRow& row) { return row.label == *state.selected_label; });
            return label == track.rows.end()
                       ? std::nullopt
                       : LabelRemovalEdit(track, static_cast<std::size_t>(label - track.rows.begin()));
        }
        return RemovalEdit(track, state.selected);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
    {
        state.selected.clear();
        state.selected_label.reset();
    }

    const bool up = ImGui::IsKeyPressed(ImGuiKey_UpArrow);
    const bool down = ImGui::IsKeyPressed(ImGuiKey_DownArrow);
    if ((up || down) && !track.rows.empty())
    {
        // The anchor can lie past the rows of a track that changed since the click.
        const std::size_t from =
            std::min(state.anchor.value_or(FirstSelected(track, state).value_or(0)), track.rows.size() - 1);
        const std::size_t to = up ? (from == 0 ? 0 : from - 1) : std::min(from + 1, track.rows.size() - 1);
        SelectRow(track, state, to);
    }

    return std::nullopt;
}

// The rows, in a part of the window that scrolls. Only the rows in view are drawn. A line between two rows marks the
// transport's position: before the first row that runs after it. During playback, the rows scroll to keep the line in
// view. Returns the edit of a typed value, a menu or a key.
std::optional<ListEdit> DrawRows(const ListTrack& track, ListEditorState& state, const Transport& transport, bool busy)
{
    ImGui::BeginChild("##rows", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None, ImGuiWindowFlags_None);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const float row_height = Px(kRowHeight);
    const float view_top = ImGui::GetWindowPos().y;
    const float view_height = ImGui::GetWindowHeight();
    const std::array<float, kListColumns + 1> edges = ColumnEdges(origin.x, origin.x + width);

    // The line of the position.
    const double position = transport.Position() * std::max(track.ticks_per_quarter, 1);
    std::size_t line = track.rows.size();
    for (std::size_t i = 0; i < track.rows.size(); i++)
    {
        const std::optional<uint32_t> tick = track.rows[i].row.tick;
        if (tick && static_cast<double>(*tick) > position + 1e-6)
        {
            line = i;
            break;
        }
    }
    const float line_y = origin.y + static_cast<float>(line) * row_height;

    // A row to scroll to goes a third of the way down the view. During playback, the line does the same once it leaves
    // the view. A zone that comes to the front lays its rows out in a hidden frame first. The scroll goes again once
    // the rows show.
    if (state.scroll_row && *state.scroll_row < track.rows.size())
    {
        const float y = static_cast<float>(*state.scroll_row) * row_height;
        ImGui::SetScrollY(std::max(y - view_height / 3.0f, 0.0f));
        if (!ImGui::GetCurrentWindow()->Hidden)
        {
            state.scroll_row.reset();
        }
    }
    else if (transport.Playing() && (line_y < view_top || line_y > view_top + view_height - row_height))
    {
        state.scroll_row.reset();
        ImGui::SetScrollY(std::max(line_y - origin.y - view_height / 3.0f, 0.0f));
    }
    else
    {
        state.scroll_row.reset();
    }

    std::optional<ListEdit> edit;
    const char* menu = nullptr;
    const bool typing = state.typing.has_value();
    for (std::size_t i = 0; i < track.rows.size(); i++)
    {
        const float y = origin.y + static_cast<float>(i) * row_height;
        if (y + row_height < view_top || y > view_top + view_height)
        {
            continue;
        }

        // A row that the track entered by its start, a jump, a call, a return or a loop's end has a line above it.
        const ListRow& row = track.rows[i];
        ImGui::SetCursorScreenPos(ImVec2(origin.x, y));
        ImGui::PushID(static_cast<int>(i));
        ImGui::SetNextItemAllowOverlap();
        ImGui::InvisibleButton("##row", ImVec2(std::max(width, edges.back() - origin.x), row_height),
                               ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
        const bool hovered = ImGui::IsItemHovered();
        const bool selected = Selected(track, state, i);
        if (IsLabel(row) && !selected && !hovered)
        {
            draw->AddRectFilled(ImVec2(origin.x, y), ImVec2(origin.x + width, y + row_height), theme::kPanelAlt);
        }
        if (selected || hovered)
        {
            draw->AddRectFilled(
                ImVec2(origin.x, y), ImVec2(origin.x + width, y + row_height),
                selected ? theme::WithAlpha(theme::kAccent, hovered ? 0.3f : 0.22f) : theme::kRaisedHover);
        }

        // A label's row drags onto the row of the command that the label moves to.
        if (IsLabel(row) && !busy && !track.timeline &&
            ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoPreviewTooltip))
        {
            ImGui::SetDragDropPayload(kLabelPayload, &i, sizeof(i));
            WrappedTooltip("Move " + row.label);
            ImGui::EndDragDropSource();
        }
        if (!IsLabel(row) && !busy && ImGui::BeginDragDropTarget())
        {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kLabelPayload))
            {
                std::size_t label = 0;
                std::memcpy(&label, payload->Data, sizeof(label));
                edit = LabelMoveEdit(track, label, i);
            }
            ImGui::EndDragDropTarget();
        }
        if (row.entered && i > 0)
        {
            draw->AddRectFilled(ImVec2(origin.x, y), ImVec2(origin.x + width, y + Px(1.0f)), theme::kBorder);
        }

        // A typable cell under the mouse has a box behind it.
        const std::optional<ListColumn> column = hovered ? ColumnAt(edges, ImGui::GetIO().MousePos.x) : std::nullopt;
        const bool typable = column && (NamesTarget(row, *column) || Typable(track, row, *column));
        if (typable && !busy)
        {
            const auto c = static_cast<std::size_t>(*column);
            draw->AddRectFilled(ImVec2(edges[c] + Px(kMargin) - Px(4.0f), y + Px(2.0f)),
                                ImVec2(edges[c + 1] - Px(4.0f), y + row_height - Px(2.0f)), theme::kControl, Px(3.0f));
            ImGui::SetMouseCursor(NamesTarget(row, *column) ? ImGuiMouseCursor_Hand : ImGuiMouseCursor_TextInput);
        }
        DrawCells(draw, track, i, y, edges);
        if (hovered && Skipped(track, row))
        {
            ItemTooltip("The " + SkippingEngine(track, row) + "'s player skips this command.");
        }

        // A click selects. A click on a command's cell that takes a value also types the value, or opens the menu of
        // targets: the box under the mouse is the field. A label's row drags, and a double-click types its name. A
        // click with Shift or Ctrl only selects. A click while a field is open ends the typing first. A right-click
        // opens the row's context menu.
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
        {
            const ImGuiIO& io = ImGui::GetIO();
            const bool types = typable && !busy && !typing && !io.KeyShift && !io.KeyCtrl &&
                               (!IsLabel(row) || ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left));
            if (!IsLabel(row) || !types)
            {
                ClickRow(track, state, i);
            }
            if (types && NamesTarget(row, *column))
            {
                state.target_row = i;
                menu = "##targets";
            }
            else if (types)
            {
                StartTyping(track, state, i, *column);
            }
        }
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
        {
            if (!selected)
            {
                SelectRow(track, state, i);
                state.scroll_row.reset();
            }
            state.menu_row = i;
            menu = "##row";
        }
        ImGui::PopID();
    }

    // The field of a typed value scrolls with the rows.
    if (state.typing && state.typing->first < track.rows.size() && !busy)
    {
        const auto [index, column] = *state.typing;
        const std::optional<std::string> typed = DrawTyping(state, origin.y, edges);
        if (typed)
        {
            edit = TypedListEdit(track, index, column, *typed);
        }
    }

    // The line of the position, with a mark at its left end.
    draw->AddRectFilled(ImVec2(origin.x, line_y - Px(1.0f)), ImVec2(origin.x + width, line_y + Px(1.0f)),
                        theme::WithAlpha(theme::kText, 0.85f));
    draw->AddTriangleFilled(ImVec2(origin.x, line_y - Px(5.0f)), ImVec2(origin.x + Px(7.0f), line_y),
                            ImVec2(origin.x, line_y + Px(5.0f)), theme::kText);

    // The rows set the height that scrolls.
    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + static_cast<float>(track.rows.size()) * row_height));
    ImGui::Dummy(ImVec2(width, Px(kRowHeight)));
    ImGui::EndChild();

    // A row's menus open in the editor's window. DrawRowMenu and DrawTargetMenu draw them there. The menu of targets
    // opens again after a double-click's second click closed it.
    if (std::exchange(state.reopen_targets, false) && state.target_row && menu == nullptr)
    {
        menu = "##targets";
    }
    if (menu != nullptr)
    {
        ImGui::OpenPopup(menu);
    }

    return edit;
}

} // namespace

std::optional<ListEdit> DrawListEditor(const ListTrack& track, ListEditorState& state, const Transport& transport,
                                       bool busy, bool* open)
{
    // A zone that docks with the mixer in the lower zone, and takes its place while it's open (see kListEditor). Its
    // header names the track. Opening a track brings the editor to the front.
    if (state.scroll_row)
    {
        ImGui::SetNextWindowFocus();
    }
    ImGuiWindowClass zone;
    zone.DockNodeFlagsOverrideSet = ImGuiDockNodeFlags_NoTabBar;
    ImGui::SetNextWindowClass(&zone);
    DockWithMixer(kListEditor);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::ColorConvertU32ToFloat4(theme::kPanel));
    const bool visible =
        ImGui::Begin(kListEditor, open,
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    if (!visible)
    {
        ImGui::End();
        state.focused = false;
        state.typing.reset();
        return std::nullopt;
    }
    state.focused =
        ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) || (state.focused && MenuBarFocused());

    // The selection keeps the commands that the track still shows, and the label once the edits' versions are read.
    std::set<uint32_t> shown;
    for (const ListRow& row : track.rows)
    {
        shown.insert(row.row.command);
    }
    std::erase_if(state.selected, [&shown](uint32_t command) { return !shown.contains(command); });
    const auto shows_label = [&state](const ListRow& row)
    {
        return row.label == state.selected_label;
    };
    if (state.selected_label && !busy && std::none_of(track.rows.begin(), track.rows.end(), shows_label))
    {
        state.selected_label.reset();
    }

    // The Edit menu's Select All selects every command that the track shows, and no label.
    if (std::exchange(state.select_all, false))
    {
        state.selected_label.reset();
        for (const ListRow& row : track.rows)
        {
            if (!IsLabel(row))
            {
                state.selected.insert(row.row.command);
            }
        }
    }

    ZoneHeader("List Editor", track.name);
    std::optional<ListEdit> edit = DrawToolbar(track, state, busy);
    if (track.rows.empty())
    {
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        EmptyState(ImGui::GetWindowDrawList(), pos, ImVec2(pos.x + avail.x, pos.y + avail.y), "No commands",
                   track.unavailable.empty() ? "This track runs no commands." : track.unavailable);

        // The toolbar moved the cursor, and the window grows only up to an item. No row is there to scroll to. A row to
        // scroll to would otherwise give the window the focus in every frame, and close the toolbar's menus.
        ImGui::Dummy(ImVec2(std::max(avail.x, 0.0f), std::max(avail.y, 0.0f)));
        state.scroll_row.reset();
        ImGui::End();
        return edit;
    }

    const ImVec2 origin = ImGui::GetCursorScreenPos();
    DrawColumnNames(ColumnEdges(origin.x, origin.x + ImGui::GetContentRegionAvail().x));
    std::optional<ListEdit> listed = DrawRows(track, state, transport, busy);
    std::optional<ListEdit> chosen = DrawRowMenu(track, state, busy);
    if (!chosen)
    {
        chosen = DrawTargetMenu(track, state, busy);
    }
    if (!chosen && state.focused && !state.typing && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId))
    {
        chosen = HandleKeys(track, state, busy);
    }
    ImGui::End();

    return edit ? edit : listed ? listed : chosen;
}

} // namespace hokuto::app
