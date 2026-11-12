// SPDX-License-Identifier: MIT

// The Variables zone: the scenes, the changes of variables while the song plays, and the variables with their
// switches.

#include "app/variables_zone.h"

#include <imgui.h>

#include <algorithm>
#include <array>
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

using theme::Px;
using zone_parts::BeginZone;
using zone_parts::kChangeRowHeight;
using zone_parts::kSceneRowHeight;
using zone_parts::kScenesHeaderHeight;
using zone_parts::kSwitchHeight;
using zone_parts::kTimeFieldWidth;
using zone_parts::kVariableRowHeight;

// nn::atk numbers the global variables from 16. Every sequence that plays shares them.
constexpr int kFirstGlobalVariable = 16;

// The range of the slider of a variable without choices: from -1, the value that a variable starts with, to 127. On the
// Wii, a global variable starts at 0. A typed value can be any value that a variable holds: 16 bits, with a sign.
constexpr SliderRange kAnyValue = {-1.0f, 127.0f, false};
constexpr SliderRange kTypedValue = {-32768.0f, 32767.0f, false};

// Chooses `value` as the value that `variable` starts with, unless it starts with that value already.
void Choose(VariableView& variable, int value)
{
    if (value != variable.start)
    {
        variable.start = value;
        variable.chosen = value;
    }
}

// The switches of a variable's choices, side by side in a strip `width` wide at `pos`. The switch at the starting value
// is lit. A click on another switch sets its value. Returns false, and draws nothing, when a value is too wide for its
// switch.
bool DrawSwitches(VariableView& variable, ImVec2 pos, float width)
{
    ImFont* figures = theme::GetFonts().figures;
    const float segment = (width - Px(4.0f)) / static_cast<float>(variable.choices.size());
    std::vector<std::string> labels;
    std::vector<uint8_t> lit;
    for (const int choice : variable.choices)
    {
        labels.push_back(std::to_string(choice));
        lit.push_back(choice == variable.start ? 1 : 0);
        if (TextSize(figures, theme::kTextSize, labels.back()).x + Px(12.0f) > segment)
        {
            return false;
        }
    }

    ImGui::SetCursorScreenPos(pos);
    const int clicked = SegmentedSwitch("##switches", labels, lit, ImVec2(width, Px(kSwitchHeight)));
    if (clicked >= 0)
    {
        Choose(variable, variable.choices[static_cast<std::size_t>(clicked)]);
    }

    return true;
}

// A pop-up menu of a variable's choices, for more choices than switches can show.
void DrawChoiceMenu(VariableView& variable, ImVec2 pos, float width)
{
    ImGui::SetCursorScreenPos(pos);
    ImGui::SetNextItemWidth(width);
    PushFigures();
    if (ImGui::BeginCombo("##choices", std::to_string(variable.start).c_str()))
    {
        for (const int choice : variable.choices)
        {
            if (ImGui::Selectable(std::to_string(choice).c_str(), choice == variable.start))
            {
                Choose(variable, choice);
            }
        }
        ImGui::EndCombo();
    }
    ImGui::PopFont();
}

// The slider of a variable without choices. A drag shows each value that it passes, and sets the value where it ends. A
// double-click types any value that a variable holds.
void DrawValueSlider(VariableView& variable, ImVec2 pos, float width)
{
    ImGui::SetCursorScreenPos(pos);
    float value = static_cast<float>(variable.start);
    if (ValueSlider("##value", &value, kAnyValue, static_cast<float>(variable.initial), "%.0f",
                    ImVec2(width, Px(kSwitchHeight)), &kTypedValue))
    {
        variable.start = static_cast<int>(std::lround(value));
    }
    if (SliderReleased())
    {
        variable.chosen = variable.start;
    }
}

// A row of the Variables zone from `top_left`: the variable's name, its value while the song plays, and the switches,
// the menu or the slider that choose the value that the song starts with.
void DrawVariable(ImDrawList* draw, VariableView& variable, ImVec2 top_left, float width)
{
    ImFont* text = theme::GetFonts().text;
    ImFont* figures = theme::GetFonts().figures;
    const float margin = Px(14.0f);
    const float left = top_left.x + margin;
    const float right = top_left.x + width - margin;
    const float name_top = top_left.y + Px(10.0f);

    // The name, and the kind of a global variable on the name's baseline.
    const std::string name = "Variable " + std::to_string(variable.number);
    DrawText(draw, text, theme::kTextSize, ImVec2(left, name_top), theme::kText, name);
    if (variable.number >= kFirstGlobalVariable)
    {
        const float baseline = name_top + Ascent(text, theme::kTextSize);
        const ImVec2 at(left + TextSize(text, theme::kTextSize, name).x + Px(8.0f),
                        baseline - Ascent(text, theme::kSmallSize));
        const ImVec2 size = TextSize(text, theme::kSmallSize, "Global");
        DrawText(draw, text, theme::kSmallSize, at, theme::kTextMuted, "Global");
        ImGui::SetCursorScreenPos(at);
        ImGui::InvisibleButton("##global", size);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
        {
            WrappedTooltip(
                "In the game, all playing sequences share global variables. Here, the value you choose applies "
                "only to this sound.");
        }
    }

    // While the song plays, the value at the position of a variable that can differ from its starting value.
    if (variable.value && (variable.written || *variable.value != variable.start))
    {
        const std::string value = std::to_string(*variable.value);
        const ImVec2 size = TextSize(figures, theme::kTextSize, value);
        const float icon = Px(10.0f);
        const float value_left = right - size.x - Px(5.0f) - icon;
        DrawText(draw, figures, theme::kTextSize, ImVec2(right - size.x, name_top), theme::kPlay, value);
        DrawIcon(draw, Icon::kPlay, ImVec2(value_left + icon * 0.5f, name_top + size.y * 0.5f), icon, theme::kPlay);
        ImGui::SetCursorScreenPos(ImVec2(value_left, name_top));
        ImGui::InvisibleButton("##now", ImVec2(right - value_left, size.y));
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
        {
            WrappedTooltip("Value at the current position");
        }
    }

    // The control of the starting value.
    const ImVec2 control(left, name_top + Px(26.0f));
    if (variable.choices.empty())
    {
        DrawValueSlider(variable, control, right - left);
    }
    else if (!DrawSwitches(variable, control, right - left))
    {
        DrawChoiceMenu(variable, control, right - left);
    }
}

// Starts to edit the name of scene `index` in place, from `name`. A name too long for the field loses its end, at the
// start of a character.
void StartRenaming(VariablesState& state, std::size_t index, const std::string& name)
{
    std::size_t size = std::min(name.size(), state.name.size() - 1);
    while (size > 0 && size < name.size() && IsContinuationByte(name[size]))
    {
        size--;
    }

    state.name.fill('\0');
    name.copy(state.name.data(), size);
    state.renaming = index;
    state.focus = true;
}

// The field that replaces the row of scene `index` while the user edits its name.
void DrawSceneNameField(SongView& song, VariablesState& state, std::size_t index, ImVec2 box_min, ImVec2 box_max,
                        float name_x)
{
    const SceneView& scene = song.scenes[index];
    ImGui::SetCursorScreenPos(box_min);
    ImGui::SetNextItemWidth(box_max.x - box_min.x);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                        ImVec2(name_x - box_min.x, (box_max.y - box_min.y - ImGui::GetFontSize()) * 0.5f));
    if (std::exchange(state.focus, false))
    {
        ImGui::SetKeyboardFocusHere();
    }
    ImGui::InputText("##name", state.name.data(), state.name.size(), ImGuiInputTextFlags_AutoSelectAll);
    ImGui::PopStyleVar();
    if (ImGui::IsItemDeactivated())
    {
        const std::string name = state.name.data();
        if (!name.empty() && name != scene.name)
        {
            song.scene_request = SceneRequest{SceneAction::kRename, index, name};
        }
        state.renaming.reset();
    }
}

// The context menu of scene `index`.
void DrawSceneMenu(SongView& song, VariablesState& state, std::size_t index)
{
    const SceneView& scene = song.scenes[index];
    if (ImGui::BeginPopupContextItem("##menu"))
    {
        if (ImGui::MenuItem("Update Scene"))
        {
            song.scene_request = SceneRequest{SceneAction::kUpdate, index, {}};
        }
        if (ImGui::MenuItem("Rename Scene"))
        {
            StartRenaming(state, index, scene.name);
        }
        if (ImGui::MenuItem("Delete Scene"))
        {
            song.scene_request = SceneRequest{SceneAction::kDelete, index, {}};
        }
        ImGui::EndPopup();
    }
}

// The box and the name of `scene`.
void DrawSceneBox(ImDrawList* draw, const SceneView& scene, ImVec2 box_min, ImVec2 box_max, float name_x, bool hovered,
                  bool held)
{
    ImFont* text = theme::GetFonts().text;
    // A lit scene keeps a tint of the accent, like a lit switch. Another shows a frame under the mouse.
    if (scene.lit)
    {
        const float alpha = held ? 0.42f : hovered ? 0.36f : 0.3f;
        draw->AddRectFilled(box_min, box_max, theme::WithAlpha(theme::kAccent, alpha), Px(6.0f));
    }
    else if (hovered || held)
    {
        draw->AddRectFilled(box_min, box_max, held ? theme::kControlActive : theme::kControlHover, Px(6.0f));
    }
    const bool named = !scene.name.empty();
    const std::string name =
        FitText(text, theme::kTextSize, named ? scene.name : "Untitled", box_max.x - Px(10.0f) - name_x);
    const float name_y = std::floor((box_min.y + box_max.y - TextSize(text, theme::kTextSize, name).y) * 0.5f);
    DrawText(draw, text, theme::kTextSize, ImVec2(name_x, name_y), named ? theme::kText : theme::kTextMuted, name);
}

// The row of scene `index`, from `min` to `max`. A click recalls the scene, and a double-click edits its name in place.
// The row's context menu updates, renames or deletes the scene.
void DrawScene(ImDrawList* draw, SongView& song, VariablesState& state, std::size_t index, ImVec2 min, ImVec2 max)
{
    const SceneView& scene = song.scenes[index];
    const ImVec2 box_min(min.x, min.y + Px(1.0f));
    const ImVec2 box_max(max.x, max.y - Px(1.0f));
    const float name_x = box_min.x + Px(10.0f);
    ImGui::PushID(static_cast<int>(index));

    // While the user edits the name, a field takes the row's place. Enter or a click elsewhere keeps the new name.
    // Escape gives the field the old name back.
    if (state.renaming == index)
    {
        DrawSceneNameField(song, state, index, box_min, box_max, name_x);
        ImGui::PopID();

        return;
    }

    ImGui::SetCursorScreenPos(min);
    if (ImGui::InvisibleButton("##scene", ImVec2(max.x - min.x, max.y - min.y)))
    {
        song.scene_request = SceneRequest{SceneAction::kRecall, index, {}};
    }
    const bool hovered = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();
    if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
    {
        StartRenaming(state, index, scene.name);
    }
    DrawSceneMenu(song, state, index);
    ImGui::PopID();

    DrawSceneBox(draw, scene, box_min, box_max, name_x, hovered, held);
}

// The song's scenes from `top_left`, above its variables: a header with the button that stores a new scene, then a row
// for each scene. Returns the height that they take.
float DrawScenes(ImDrawList* draw, SongView& song, VariablesState& state, ImVec2 top_left, float width)
{
    if (state.renaming && *state.renaming >= song.scenes.size())
    {
        state.renaming.reset();
    }

    // The header: the section's name on the left, and the button on the right.
    const float margin = Px(14.0f);
    const float header = Px(kScenesHeaderHeight);
    const float button = Px(24.0f);
    const float middle = top_left.y + header * 0.5f;
    const ImVec2 caps = CapsSize("Scenes");
    DrawCaps(draw, ImVec2(top_left.x + margin, std::floor(middle - caps.y * 0.5f)), theme::kTextMuted, "Scenes");
    ImGui::SetCursorScreenPos(
        ImVec2(top_left.x + width - margin - button + Px(5.0f), std::floor(middle - button * 0.5f)));
    if (IconButton("##store", Icon::kAdd, ImVec2(button, button), false, 0, "Store Scene"))
    {
        song.scene_request = SceneRequest{SceneAction::kStore, 0, {}};
    }

    // A row for each scene, or a note that there are none.
    const float row_height = Px(kSceneRowHeight);
    const float rows_top = top_left.y + header - Px(4.0f);
    if (song.scenes.empty())
    {
        ImFont* text = theme::GetFonts().text;
        const float y = std::floor(rows_top + (row_height - TextSize(text, theme::kTextSize, "No scenes").y) * 0.5f);
        DrawText(draw, text, theme::kTextSize, ImVec2(top_left.x + margin, y), theme::kTextMuted, "No scenes");

        return header + row_height + Px(4.0f);
    }
    for (std::size_t i = 0; i < song.scenes.size(); i++)
    {
        const float y = rows_top + row_height * static_cast<float>(i);
        DrawScene(draw, song, state, i, ImVec2(top_left.x + margin - Px(4.0f), y),
                  ImVec2(top_left.x + width - margin + Px(4.0f), y + row_height));
    }

    return header + row_height * static_cast<float>(song.scenes.size()) + Px(4.0f);
}

// The variable numbered `number` among the variables that the song reads, or nullptr.
const VariableView* ReadVariable(const SongView& song, int number)
{
    for (const VariableView& variable : song.read_variables)
    {
        if (variable.number == number)
        {
            return &variable;
        }
    }

    return nullptr;
}

// The menu of the variables of change `index`, at `pos` and `width` wide.
void DrawChangeVariable(SongView& song, const ChangeView& change, std::size_t index, ImVec2 pos, float width)
{
    ImGui::SetCursorScreenPos(pos);
    ImGui::SetNextItemWidth(width);
    if (ImGui::BeginCombo("##variable", ("Variable " + std::to_string(change.variable)).c_str()))
    {
        for (const VariableView& variable : song.read_variables)
        {
            const bool current = variable.number == change.variable;
            if (ImGui::Selectable(("Variable " + std::to_string(variable.number)).c_str(), current) && !current)
            {
                ChangeView edited = change;
                edited.variable = variable.number;
                song.change_request = ChangeRequest{ChangeAction::kEdit, index, edited, {}};
            }
        }
        ImGui::EndCombo();
    }
    ImGui::OpenPopupOnItemClick("##change", ImGuiPopupFlags_MouseButtonRight);
}

// The field of the time of change `index`, at `pos` and `width` wide.
void DrawChangeTime(SongView& song, VariablesState& state, const ChangeView& change, std::size_t index, ImVec2 pos,
                    float width)
{
    // The field of the time shows the change's time until the user types in it.
    const bool typing = state.timing == index;
    std::array<char, 32> shown{};
    TimeText(change.time).copy(shown.data(), shown.size() - 1);
    ImGui::SetCursorScreenPos(pos);
    ImGui::SetNextItemWidth(width);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImGui::ColorConvertU32ToFloat4(theme::kDisplay));
    PushFigures();
    ImGui::InputText("##time", typing ? state.time.data() : shown.data(), shown.size(),
                     ImGuiInputTextFlags_AutoSelectAll);
    ImGui::PopFont();
    ImGui::PopStyleColor();
    if (ImGui::IsItemActivated())
    {
        state.timing = index;
        state.time = shown;
    }
    if (ImGui::IsItemDeactivated() && typing)
    {
        const std::optional<uint64_t> time = ParseTime(state.time.data(), std::numeric_limits<uint32_t>::max());
        if (!time)
        {
            song.change_request = ChangeRequest{
                ChangeAction::kEdit, index, change,
                "Enter the time in seconds, minutes:seconds or hours:minutes:seconds, such as 90, 1:30 or "
                "0:01:30.000. Minutes and seconds after a colon must be below 60."};
        }
        else if (*time != change.time)
        {
            ChangeView edited = change;
            edited.time = static_cast<uint32_t>(*time);
            song.change_request = ChangeRequest{ChangeAction::kEdit, index, edited, {}};
        }
        state.timing.reset();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal) && !ImGui::IsItemActive())
    {
        WrappedTooltip("Time from the start at which the game sets the variable");
    }
    ImGui::OpenPopupOnItemClick("##change", ImGuiPopupFlags_MouseButtonRight);
}

// The switches, the menu or the slider of the value of change `index`, at `control` and `width` wide.
void DrawChangeValue(SongView& song, ChangeView& change, std::size_t index, ImVec2 control, float width)
{
    // The value's switches, menu or slider: those of the variable among the variables that the song reads. A drag of
    // the slider shows each value that it passes, and the value where it ends becomes the change's value.
    const VariableView* read = ReadVariable(song, change.variable);
    VariableView value{.number = change.variable, .start = change.value};
    if (read != nullptr)
    {
        value.choices = read->choices;
    }
    if (value.choices.empty())
    {
        DrawValueSlider(value, control, width);
    }
    else if (!DrawSwitches(value, control, width))
    {
        DrawChoiceMenu(value, control, width);
    }
    change.value = value.start;
    if (value.chosen)
    {
        ChangeView edited = change;
        edited.value = *value.chosen;
        song.change_request = ChangeRequest{ChangeAction::kEdit, index, edited, {}};
    }
}

// The row of change `index` from `top_left`: a menu of the variables that the change can set, the field of its time,
// and the switches, the menu or the slider of its value. Enter or a click elsewhere keeps a typed time, and Escape
// gives the field the old time back. A time that can't be read changes nothing. The row's context menu removes the
// change.
void DrawChange(SongView& song, VariablesState& state, std::size_t index, ImVec2 top_left, float width)
{
    ChangeView& change = song.changes[index];
    const float margin = Px(14.0f);
    const float left = top_left.x + margin;
    const float right = top_left.x + width - margin;
    const float line_top = top_left.y + Px(10.0f);
    const float gap = Px(8.0f);
    const float height = Px(kSwitchHeight);
    const float time_width = std::min(Px(kTimeFieldWidth), (right - left) * 0.5f);
    ImGui::PushID(static_cast<int>(index));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                        ImVec2(Px(8.0f), std::max((height - ImGui::GetFontSize()) * 0.5f, 0.0f)));

    DrawChangeVariable(song, change, index, ImVec2(left, line_top), right - left - time_width - gap);
    DrawChangeTime(song, state, change, index, ImVec2(right - time_width, line_top), time_width);
    ImGui::PopStyleVar();

    const ImVec2 control(left, line_top + height + Px(8.0f));
    DrawChangeValue(song, change, index, control, right - left);

    if (ImGui::BeginPopup("##change"))
    {
        if (ImGui::MenuItem("Remove Change"))
        {
            song.change_request = ChangeRequest{ChangeAction::kRemove, index, change, {}};
        }
        ImGui::EndPopup();
    }
    ImGui::PopID();
}

// The song's changes from `top_left`, after its scenes: a header with the button that adds a change at the position,
// then a row for each change. Returns the height that they take.
float DrawChanges(ImDrawList* draw, SongView& song, VariablesState& state, ImVec2 top_left, float width)
{
    if (state.timing && *state.timing >= song.changes.size())
    {
        state.timing.reset();
    }

    // The header: the section's name on the left, and the button on the right.
    const float margin = Px(14.0f);
    const float header = Px(kScenesHeaderHeight);
    const float button = Px(24.0f);
    const float middle = top_left.y + header * 0.5f;
    const ImVec2 caps = CapsSize("Changes");
    DrawCaps(draw, ImVec2(top_left.x + margin, std::floor(middle - caps.y * 0.5f)), theme::kTextMuted, "Changes");
    ImGui::SetCursorScreenPos(
        ImVec2(top_left.x + width - margin - button + Px(5.0f), std::floor(middle - button * 0.5f)));
    if (IconButton("##add", Icon::kAdd, ImVec2(button, button), false, 0, "Add Change"))
    {
        song.change_request = ChangeRequest{ChangeAction::kAdd, 0, {}, {}};
    }

    // A row for each change, or a note that there are none.
    const float rows_top = top_left.y + header - Px(4.0f);
    if (song.changes.empty())
    {
        ImFont* text = theme::GetFonts().text;
        const float row_height = Px(kSceneRowHeight);
        const float y = std::floor(rows_top + (row_height - TextSize(text, theme::kTextSize, "No changes").y) * 0.5f);
        DrawText(draw, text, theme::kTextSize, ImVec2(top_left.x + margin, y), theme::kTextMuted, "No changes");

        return header + row_height + Px(4.0f);
    }
    const float row_height = Px(kChangeRowHeight);
    ImGui::PushID("##changes");
    for (std::size_t i = 0; i < song.changes.size(); i++)
    {
        const float top = rows_top + row_height * static_cast<float>(i);
        draw->AddRectFilled(ImVec2(top_left.x + margin, top), ImVec2(top_left.x + width - margin, top + Px(1.0f)),
                            theme::kBorder);
        DrawChange(song, state, i, ImVec2(top_left.x, top), width);
    }
    ImGui::PopID();

    return header + row_height * static_cast<float>(song.changes.size()) + Px(4.0f);
}

// The rows of the Variables zone: the scenes, the changes, then the variables with a line above each one. They scroll
// under the zone's header.
void DrawVariableRows(SongView& song, VariablesState& state)
{
    ImGui::BeginChild("##rows", ImVec2(0.0f, 0.0f));
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const float row_height = Px(kVariableRowHeight);
    float above = DrawScenes(draw, song, state, origin, width);
    above += DrawChanges(draw, song, state, ImVec2(origin.x, origin.y + above), width);
    for (std::size_t i = 0; i < song.variables.size(); i++)
    {
        const float top = origin.y + above + row_height * static_cast<float>(i);
        draw->AddRectFilled(ImVec2(origin.x + Px(14.0f), top), ImVec2(origin.x + width - Px(14.0f), top + Px(1.0f)),
                            theme::kBorder);
        ImGui::PushID(static_cast<int>(i));
        DrawVariable(draw, song.variables[i], ImVec2(origin.x, top), width);
        ImGui::PopID();
    }

    // The rows set the height that scrolls.
    ImGui::SetCursorScreenPos(
        ImVec2(origin.x, origin.y + above + row_height * static_cast<float>(song.variables.size())));
    ImGui::Dummy(ImVec2(width, Px(4.0f)));
    ImGui::EndChild();
}

} // namespace

void DrawVariables(SongView* song, VariablesState& state, bool* open)
{
    if (!*open)
    {
        return;
    }

    if (BeginZone(kVariables, open))
    {
        // A song whose starting values change nothing can still read variables that a change can set.
        const std::size_t count = song != nullptr ? song->variables.size() : 0;
        ZoneHeader("Variables", count == 0   ? std::string()
                                : count == 1 ? std::string("1 variable")
                                             : std::to_string(count) + " variables");
        if (count == 0 && (song == nullptr || (song->read_variables.empty() && song->changes.empty())))
        {
            const ImVec2 pos = ImGui::GetCursorScreenPos();
            const ImVec2 avail = ImGui::GetContentRegionAvail();
            EmptyState(ImGui::GetWindowDrawList(), pos, ImVec2(pos.x + avail.x, pos.y + avail.y), "No variables",
                       "Variables that the game sets appear here.");
        }
        else
        {
            DrawVariableRows(*song, state);
        }
    }
    ImGui::End();
}

} // namespace hokuto::app
