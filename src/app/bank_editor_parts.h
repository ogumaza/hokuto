// SPDX-License-Identifier: MIT

// The parts of the Bank Editor that the files of the consoles share (bank_editor.cpp). The names are internal to
// the Bank Editor.

#pragma once

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "app/bank_editor.h"
#include "app/browser.h"
#include "app/theme.h"
#include "app/widgets.h"

namespace hokuto::app::bank_editor_parts
{

// The keys and the velocities that a region covers run from 0 to 127.
inline constexpr int kKeys = 128;
inline constexpr int kHighest = 127;

// The window's sizes, before the display's scale: its first size and its least, the list of programs, a row of the
// list, the keyboard, the facts at the bottom, the labels of the fields, the room above and below a field's control,
// the gap between the parts, and the band inside an edge of a region that a drag takes. The band is as wide as the
// arrangement's and the Key Editor's grabs of an edge. The fields under the map take up to kFieldRows rows in each of
// their two tables.
inline constexpr float kWindowWidth = 1040.0f;
inline constexpr float kWindowHeight = 760.0f;
inline constexpr float kLeastWidth = 760.0f;
inline constexpr float kLeastHeight = 600.0f;
inline constexpr float kListWidth = 240.0f;
inline constexpr float kRowHeight = 26.0f;
inline constexpr float kKeyboardHeight = 54.0f;
inline constexpr float kFactsHeight = 84.0f;
inline constexpr float kLabelWidth = 96.0f;
inline constexpr float kFieldPadding = 2.0f;
inline constexpr int kFieldRows = 8;
inline constexpr int kGbaFieldRows = 10; // a voice's kind and keys take two more
inline constexpr float kGap = 10.0f;
inline constexpr float kEdgeBand = 6.0f;

// The values that a Command-click or a Ctrl-click on a field's slider sets: NintendoWare's defaults.
inline constexpr float kCentre = 64.0f;
inline constexpr float kMiddleC = 60.0f;
inline constexpr float kFastest = 127.0f;

// Where the window's parts are: the content's origin and room, the top of the facts, the left edge and the width of
// the right side, the height and top of the fields, the top of the keyboard, and the top of the rows under the
// right side's caps.
struct BankEditorLayout
{
    ImVec2 origin;
    ImVec2 room;
    float facts_top = 0.0f;
    float right = 0.0f;
    float right_width = 0.0f;
    float fields_height = 0.0f;
    float fields_top = 0.0f;
    float keyboard_top = 0.0f;
    float rows_top = 0.0f;
};

// Opens the Bank Editor's window over the middle of the main window, with `title`, and records where it is in
// `state`. Returns false, with the window ended, when the window is hidden.
bool BeginBankEditorWindow(const std::string& title, BankEditorState& state, bool* open);

// Forgets where the list of programs and the map lay, before a frame draws them again.
void ForgetDropTargets(BankEditorState& state);

// The layout of the open window, with `field_rows` rows of fields above the facts and `caps` over the right side. A
// window with no fields has the keyboard straight above the facts.
BankEditorLayout BankLayoutOf(int field_rows, const char* caps);

// The line between the list of programs and the right side.
void DrawListDivider(ImDrawList* draw, const BankEditorLayout& layout);

// The line above the facts.
void DrawFactsDivider(ImDrawList* draw, const BankEditorLayout& layout);

// Where the map lies, and the width of a key's column and the height of a velocity's row. Velocity 127 lies at the top.
struct MapGeometry
{
    ImVec2 min;
    ImVec2 max;
    float key = 0.0f;
    float velocity = 0.0f;
};

// The geometry of the map from `min` to `max`, with 128 keys across and 128 velocities up.
MapGeometry GeometryOf(ImVec2 min, ImVec2 max);

// The left edge of `key`'s column, and the bottom edge of `velocity`'s row.
float KeyX(const MapGeometry& map, int key);

// The key of the column at `x`, from 0 to 127.
int KeyAt(const MapGeometry& map, float x);

// The velocity of the row at `y`, from 0 to 127.
int VelocityAt(const MapGeometry& map, float y);

// A wave dropped from the browser on the open drag target: its place, or nothing.
std::optional<BrowserPlace> AcceptedWave();

// A wave dropped from the browser on the last item: its place, or nothing.
std::optional<BrowserPlace> AcceptWave();

// The keyboard under the map, one key in each of the map's columns. A press sounds the key, and a drag across the keys
// sounds each in turn. A press near a key's bottom sounds it louder. The `covered` keys, such as the keys of the
// selected region, are tinted, and the `root` key marked.
void DrawKeyboard(const std::optional<std::pair<int, int>>& covered, std::optional<int> root, BankEditorState& state,
                  ImVec2 min, ImVec2 max);

// A row of the fields' table: its label with a tooltip, and the cursor in the value's cell. A grey label marks a field
// that the console doesn't read. Its value takes no input, and the tooltip shows over the value too.
void FieldLabel(const char* label, const char* tooltip, bool grey = false);

// The text of a pan from 0 to 127 as the mixer writes a pan.
std::string PanOf(int pan);

// A field of a DS region or a GBA voice that contains a number: its label, its name in the Undo History, its tooltip,
// its highest value, the value that a Command-click or a Ctrl-click sets, and how `Value` contains it.
template <typename Value>
struct NumberField
{
    const char* label;
    const char* name;
    const char* tooltip;
    int high;
    int reset;
    int (*get)(const Value&);
    void (*set)(Value&, int);
};

// A row of switches for a GBA voice or a DS region: the switch of `chosen` is lit. Returns the switch that the user
// clicked when it isn't the chosen one, or -1.
template <std::size_t N>
int SwitchRow(const char* label, const char* tooltip, const std::array<std::string, N>& labels, int chosen,
              bool grey = false)
{
    FieldLabel(label, tooltip, grey);
    std::array<uint8_t, N> lit{};
    if (chosen >= 0 && static_cast<std::size_t>(chosen) < N)
    {
        lit[static_cast<std::size_t>(chosen)] = 1;
    }
    ImGui::PushID(label);
    ImGui::BeginDisabled(grey);
    const int clicked =
        SegmentedSwitch("##switch", labels, lit, ImVec2(ImGui::GetContentRegionAvail().x, ImGui::GetFrameHeight()));
    ImGui::EndDisabled();
    ImGui::PopID();

    return clicked != chosen && !grey ? clicked : -1;
}

// The item of `items` that the editor shows: the one whose `key` is `selected` while `items` has it, or else the first.
// The function then sets `selected` to its key. `forget` then clears what the old selection set. While an edit waits
// for its version (`waiting`), the item that the edit selects can be missing from `items`. The editor then shows the
// first item and keeps the selection.
template <typename Item, typename Key, typename Forget>
const Item* SelectedOrFirst(const std::vector<Item>& items, std::optional<Key>& selected, Key Item::* key, bool waiting,
                            Forget forget)
{
    const auto found = selected ? std::ranges::find(items, *selected, key) : items.end();
    if (found != items.end())
    {
        return &*found;
    }
    if (waiting && selected)
    {
        return items.empty() ? nullptr : &items.front();
    }
    if (items.empty())
    {
        selected.reset();
        return nullptr;
    }

    selected = items.front().*key;
    forget();

    return &items.front();
}

// The program that the editor selects once Delete has taken out `deleted`: the next program in the list, or else the
// one before it. A selection that jumped to the first program would let a second Delete take out another voice.
template <typename Program>
std::optional<uint32_t> NeighbourProgram(const std::vector<Program>& programs,
                                         typename std::vector<Program>::const_iterator deleted,
                                         uint32_t Program::* number)
{
    if (std::next(deleted) != programs.end())
    {
        return (*std::next(deleted)).*number;
    }
    if (deleted != programs.begin())
    {
        return (*std::prev(deleted)).*number;
    }
    return std::nullopt;
}

// The list of `programs` at `min`, `size` large, in a child that clips its rows. Each row shows its program in figures
// and `label_of` the program after them, and `empty` shows in a list with no programs. A list that reveals the selected
// program starts at its top, and the next frame centres the program's row. A click on a program that isn't the
// selected one calls `select` with the program at once, before the rows below it draw.
template <typename Program, typename LabelOf, typename Select>
void DrawProgramList(const std::vector<Program>& programs, LabelOf label_of, Select select, BankEditorState& state,
                     ImVec2 min, ImVec2 size, const char* empty)
{
    ImGui::SetCursorScreenPos(min);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::ColorConvertU32ToFloat4(theme::kPanel));
    const bool reveal = std::exchange(state.reveal, false);
    if (reveal)
    {
        ImGui::SetNextWindowScroll(ImVec2(0.0f, 0.0f));
    }
    ImGui::BeginChild("##programs", size, ImGuiChildFlags_Borders);
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    ImFont* text = theme::GetFonts().text;
    ImFont* figures = theme::GetFonts().figures;
    const float column = TextSize(figures, theme::kTextSize, "000").x + theme::Px(16.0f);
    const auto revealed =
        reveal && state.program ? std::ranges::find(programs, *state.program, &Program::program) : programs.end();
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(programs.size()));
    if (revealed != programs.end())
    {
        clipper.IncludeItemByIndex(static_cast<int>(revealed - programs.begin()));
    }
    while (clipper.Step())
    {
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row)
        {
            const Program& program = programs[static_cast<std::size_t>(row)];
            std::array<char, 16> id{};
            std::snprintf(id.data(), id.size(), "##%u", program.program);
            const float left = ImGui::GetCursorScreenPos().x;
            if (ImGui::Selectable(id.data(), state.program == program.program, 0,
                                  ImVec2(0.0f, theme::Px(kRowHeight) - theme::Px(6.0f))) &&
                state.program != program.program)
            {
                select(program.program);
            }
            if (revealed != programs.end() && program.program == revealed->program)
            {
                ImGui::SetScrollHereY(0.5f);
            }
            std::array<char, 8> number{};
            std::snprintf(number.data(), number.size(), "%03u", program.program);
            const std::string kind = label_of(program);
            const float top = (ImGui::GetItemRectMin().y + ImGui::GetItemRectMax().y) * 0.5f -
                              TextSize(text, theme::kTextSize, kind).y * 0.5f;
            const float baseline = top + Ascent(text, theme::kTextSize);
            DrawText(draw_list, figures, theme::kTextSize, ImVec2(left, baseline - Ascent(figures, theme::kTextSize)),
                     theme::kText, number.data());
            DrawText(draw_list, text, theme::kTextSize, ImVec2(left + column, top), theme::kText, kind);
        }
    }
    if (programs.empty() && empty != nullptr)
    {
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme::kTextMuted), "%s", empty);
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

// The widths of the Add, Duplicate and Delete buttons under the list of programs. They share `width` by the widths of
// their labels.
std::array<float, 3> ProgramButtonWidths(float width);

// The field of the program under the list of programs, at the left of `pos` and the line of the cursor. The field runs
// to the right edge of `size`. It shows the selected program until the user types. Enter returns the program that the
// user typed when it differs from the selected one, and refuses text that isn't a program up to `highest` in `edit`.
// While `busy`, the program that the user typed waits (BankEditorState::typed_waiting) for the program of `bank`.
std::optional<uint32_t> ProgramNumberField(BankEditorState& state, const ShownItem& bank, bool chosen, bool busy,
                                           ImVec2 pos, ImVec2 size, uint32_t highest, BankEdit& edit);

// Begins a table of fields at `pos`, `width` wide: a column of labels kLabelWidth wide, and a column of values. Returns
// ImGui::BeginTable's result.
bool BeginFieldTable(const char* id, ImVec2 pos, float width);

// Begins the child window of the facts at `pos`, `size` large, with text that wraps at its edge. With `top`, it scrolls
// back to its top. ImGui::PopTextWrapPos and ImGui::EndChild end it.
void BeginFacts(ImVec2 pos, ImVec2 size, bool top);

// The details of an edit of `program` of the bank or the voice group `name` in the Undo History.
std::string ProgramDetails(const std::string& name, uint32_t program);

// A line of the facts that lists `names` after `lead`, four at most, with the rest in its tooltip.
void FactLine(const std::string& lead, const std::vector<std::string>& names);

// A text field `id` in `text` that shows `shown` until the user types. `typing` keeps whether the field is open.
// Returns the text that the user ends the field with, by Enter. While `busy`, the text waits in `waiting` for `item`,
// and the field returns it once the program is free.
std::optional<std::string> TypedField(const char* id, std::span<char> text, bool& typing, const std::string& shown,
                                      bool busy, WaitingValues<std::string>& waiting, const ShownItem& item);

// A field for a key that shows `shown` until the user types. Enter returns the key that the user typed, and refuses
// other text with `refusal`. While `busy`, the key that the user typed waits in `waiting` for `item`.
std::optional<int> KeyField(const char* id, std::array<char, 8>& text, bool& typing, int shown, float width, bool busy,
                            WaitingValues<std::string>& waiting, const ShownItem& item, BankEdit& edit,
                            const char* refusal = "Keys range from 0 to 127.");

// The item of the program, or the sample, and the region of `bank` whose fields the editor draws, for the typed fields
// and PushSliderItem. A value typed while the program is busy then drops when the user selects another program or
// region. It never applies to another region whose field shows the same text.
ShownItem TypedItemOf(const ShownItem& bank, uint32_t program, std::optional<std::size_t> region = std::nullopt);

} // namespace hokuto::app::bank_editor_parts
