// SPDX-License-Identifier: MIT

// The Bank Editor for a DSE bank: the programs, the splits with their ranges, the fields of the selected split,
// and the facts.

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "app/bank_editor.h"
#include "app/bank_editor_parts.h"
#include "app/item_views.h"
#include "app/key_edits.h"
#include "app/theme.h"
#include "app/widgets.h"
#include "edit/dse_banks.h"

namespace hokuto::app
{
namespace
{

using bank_editor_parts::BankEditorLayout;
using bank_editor_parts::BankLayoutOf;
using bank_editor_parts::BeginBankEditorWindow;
using bank_editor_parts::BeginFacts;
using bank_editor_parts::BeginFieldTable;
using bank_editor_parts::DrawFactsDivider;
using bank_editor_parts::DrawKeyboard;
using bank_editor_parts::DrawListDivider;
using bank_editor_parts::FactLine;
using bank_editor_parts::FieldLabel;
using bank_editor_parts::ForgetDropTargets;
using bank_editor_parts::kCentre;
using bank_editor_parts::KeyField;
using bank_editor_parts::kFieldPadding;
using bank_editor_parts::kFieldRows;
using bank_editor_parts::kGap;
using bank_editor_parts::kKeyboardHeight;
using bank_editor_parts::kListWidth;
using bank_editor_parts::kMiddleC;
using bank_editor_parts::kRowHeight;
using bank_editor_parts::PanOf;
using bank_editor_parts::ProgramDetails;
using bank_editor_parts::SelectedOrFirst;
using bank_editor_parts::TypedItemOf;
using theme::Px;

constexpr char kDseWaveTip[] = "Wave in the bank table that this split plays.";
constexpr char kDseRootTip[] =
    "Key that plays the wave at its original pitch. The driver uses the distance of this key from middle C as the "
    "coarse tuning of the split.";
constexpr char kDseTuningTip[] =
    "Pitch offset of the split, in semitones. DSE ignores the sample rate of the wave. Instead, the tools "
    "store here the semitones between that rate and the 32,728 Hz of the sound unit. A new rate in the wave "
    "properties updates this value.";
constexpr char kDseVolumeTip[] = "Split level. The driver multiplies it by the velocity and the program volume.";
constexpr char kDseKeyGroupTip[] =
    "Key group of the bank that limits the voices of this split. A group past the end of the bank list acts as "
    "group 0.";
constexpr char kDseBendTip[] = "Pitch bend range, in semitones. A track can set a different range.";
constexpr char kDseMultiplierTip[] = "Multiplies the envelope times. At 0, the driver uses a different time table.";
constexpr char kDseAttackLevelTip[] = "Level at which the attack starts.";
constexpr char kDseAttackTip[] = "Attack time to full level. 0 starts at full level.";
constexpr char kDseHoldTip[] = "Time at full level after the attack.";
constexpr char kDseDecayTip[] = "Time to decay to the sustain level.";
constexpr char kDseSustainTip[] = "Level at the end of the decay.";
constexpr char kDseDecay2Tip[] = "Time to then fall from the sustain level. 127 holds the sustain level.";
constexpr char kDseReleaseTip[] = "Time to fade to silence after the note ends.";

// A field of a DSE split that contains a byte: its label, its name in the Undo History, its tooltip, its byte, and the
// value that a Command-click or a Ctrl-click sets.
struct DseField
{
    const char* label;
    const char* name;
    const char* tooltip;
    std::size_t at;
    int reset;
};

constexpr std::array<DseField, 4> kDsePlaying = {{
    {"Volume", "Volume", kDseVolumeTip, edit::kDseVolume, edit::kHighestDseByte},
    {"Pan", "Pan", kPanTip, edit::kDsePan, static_cast<int>(kCentre)},
    {"Key group", "Key Group", kDseKeyGroupTip, edit::kDseKeyGroup, 0},
    {"Bend range", "Bend Range", kDseBendTip, edit::kDseBendRange, 2},
}};
constexpr std::array<DseField, 8> kDseEnvelope = {{
    {"Multiplier", "Multiplier", kDseMultiplierTip, edit::kDseMultiplier, 1},
    {"Attack level", "Attack Level", kDseAttackLevelTip, edit::kDseAttackLevel, 0},
    {"Attack", "Attack", kDseAttackTip, edit::kDseAttack, 0},
    {"Hold", "Hold", kDseHoldTip, edit::kDseHold, 0},
    {"Decay", "Decay", kDseDecayTip, edit::kDseDecay, 0},
    {"Sustain", "Sustain", kDseSustainTip, edit::kDseSustain, edit::kHighestDseByte},
    {"Decay 2", "Decay 2", kDseDecay2Tip, edit::kDseDecay2, edit::kHighestDseByte},
    {"Release", "Release", kDseReleaseTip, edit::kDseRelease, 40},
}};

// The row of a slider for a value of `split` that `get` and `set` read and write. DsRow draws such a row for a DS
// region.
template <typename Get, typename Set>
void DseSliderRow(const char* label, const char* name, const char* tooltip, const edit::DseSplit& split, Get get,
                  Set set, SliderRange range, SliderRange typed, float reset, const std::string& format,
                  BankEditorState& state, bool* released)
{
    FieldLabel(label, tooltip);
    const bool holding = state.dse_held && state.held_name == name;
    edit::DseSplit changed = holding ? *state.dse_held : split;
    float shown = get(changed);
    ImGui::PushID(label);
    if (ValueSlider("##value", &shown, range, reset, format.c_str(),
                    ImVec2(ImGui::GetContentRegionAvail().x, ImGui::GetFrameHeight()), &typed))
    {
        set(changed, std::clamp(shown, typed.low, typed.high));
        state.dse_held = changed;
        state.held_name = name;
    }
    *released = *released || (SliderReleased() && state.dse_held && state.held_name == name);
    ImGui::PopID();
}

void DseRow(const DseField& field, const edit::DseSplit& split, const std::string& format, BankEditorState& state,
            bool* released)
{
    const auto high = static_cast<float>(edit::kHighestDseByte);
    DseSliderRow(
        field.label, field.name, field.tooltip, split,
        [&field](const edit::DseSplit& values) { return static_cast<float>(values[field.at]); },
        [&field](edit::DseSplit& values, float value) { values[field.at] = static_cast<uint8_t>(std::lround(value)); },
        {0.0f, high}, {0.0f, high}, static_cast<float>(field.reset), format, state, released);
}

// The split that the DSE fields show: the one that the user holds a slider of, or else `split`.
const edit::DseSplit& DseShown(const edit::DseSplit& split, const BankEditorState& state)
{
    return state.dse_held ? *state.dse_held : split;
}

// The program of `view` that the editor shows: the state's while the view contains it, or else the first.
const DseProgramView* SelectedDseProgram(const DseBankView& view, BankEditorState& state)
{
    const auto forget = [&state]
    {
        state.dse_row.reset();
        state.dse_held.reset();
    };
    return SelectedOrFirst(view.programs, state.program, &DseProgramView::program, false, forget);
}

// The split of a program that the editor shows: the state's while the program has it, or else the first.
const DseSplitRow* SelectedDseRow(const DseProgramView& program, BankEditorState& state)
{
    if (state.dse_row && *state.dse_row < program.rows.size())
    {
        return &program.rows[*state.dse_row];
    }
    if (program.rows.empty())
    {
        state.dse_row.reset();
        return nullptr;
    }

    state.dse_row = 0;
    state.dse_held.reset();

    return &program.rows.front();
}

// The list of a DSE bank's programs at `pos`, `size` large: each program's number and how many splits it contains. A
// click selects a program.
void DrawDsePrograms(const DseBankView& view, BankEditorState& state, ImVec2 pos, ImVec2 size)
{
    ImDrawList* draw = ImGui::GetWindowDrawList();
    DrawCaps(draw, pos, theme::kTextMuted, "PROGRAMS");
    const float caps = CapsSize("PROGRAMS").y + Px(6.0f);
    const ImVec2 list_min(pos.x, pos.y + caps);
    const ImVec2 list_size(size.x, std::max(size.y - caps, Px(kRowHeight)));
    ImGui::SetCursorScreenPos(list_min);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::ColorConvertU32ToFloat4(theme::kPanel));
    const bool reveal = std::exchange(state.reveal, false);
    if (reveal)
    {
        ImGui::SetNextWindowScroll(ImVec2(0.0f, 0.0f));
    }
    ImGui::BeginChild("##programs", list_size, ImGuiChildFlags_Borders);
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    ImFont* text = theme::GetFonts().text;
    ImFont* figures = theme::GetFonts().figures;
    const float column = TextSize(figures, theme::kTextSize, "000").x + Px(16.0f);
    const auto revealed = reveal && state.program
                              ? std::ranges::find(view.programs, *state.program, &DseProgramView::program)
                              : view.programs.end();
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(view.programs.size()));
    if (revealed != view.programs.end())
    {
        clipper.IncludeItemByIndex(static_cast<int>(revealed - view.programs.begin()));
    }
    while (clipper.Step())
    {
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row)
        {
            const DseProgramView& program = view.programs[static_cast<std::size_t>(row)];
            std::array<char, 16> id{};
            std::snprintf(id.data(), id.size(), "##%u", program.program);
            const float left = ImGui::GetCursorScreenPos().x;
            if (ImGui::Selectable(id.data(), state.program == program.program, 0,
                                  ImVec2(0.0f, Px(kRowHeight) - Px(6.0f))) &&
                state.program != program.program)
            {
                state.program = program.program;
                state.dse_row.reset();
                state.dse_held.reset();
            }
            if (revealed != view.programs.end() && program.program == revealed->program)
            {
                ImGui::SetScrollHereY(0.5f);
            }
            std::array<char, 8> number{};
            std::snprintf(number.data(), number.size(), "%03u", program.program);
            const std::string kind =
                std::to_string(program.rows.size()) + (program.rows.size() == 1 ? " split" : " splits");
            const float top = (ImGui::GetItemRectMin().y + ImGui::GetItemRectMax().y) * 0.5f -
                              TextSize(text, theme::kTextSize, kind).y * 0.5f;
            DrawText(draw_list, figures, theme::kTextSize, ImVec2(left, top), theme::kTextMuted, number.data());
            DrawText(draw_list, text, theme::kTextSize, ImVec2(left + column, top), theme::kText, kind);
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

// The rows of a program's splits at `pos`, `size` large: each split's keys, its velocities and its wave. A click
// selects a row.
void DrawDseRows(const DseProgramView& program, BankEditorState& state, ImVec2 pos, ImVec2 size)
{
    ImGui::SetCursorScreenPos(pos);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::ColorConvertU32ToFloat4(theme::kPanel));
    ImGui::BeginChild("##rows", size, ImGuiChildFlags_Borders);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    ImFont* text = theme::GetFonts().text;
    const float column = TextSize(text, theme::kTextSize, "C#-1 to C#-1").x + Px(16.0f);
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(program.rows.size()));
    while (clipper.Step())
    {
        for (int index = clipper.DisplayStart; index < clipper.DisplayEnd; ++index)
        {
            const DseSplitRow& row = program.rows[static_cast<std::size_t>(index)];
            ImGui::PushID(index);
            const float left = ImGui::GetCursorScreenPos().x;
            if (ImGui::Selectable("##row", state.dse_row == row.split, 0, ImVec2(0.0f, Px(kRowHeight) - Px(6.0f))) &&
                state.dse_row != row.split)
            {
                state.dse_row = row.split;
                state.dse_held.reset();
            }
            ImGui::PopID();
            const std::string keys = KeysText(row.values[edit::kDseLowKey], row.values[edit::kDseHighKey]);
            const std::string rest = row.wave + ", velocities " + std::to_string(row.values[edit::kDseLowVelocity]) +
                                     " to " + std::to_string(row.values[edit::kDseHighVelocity]);
            const float top = (ImGui::GetItemRectMin().y + ImGui::GetItemRectMax().y) * 0.5f -
                              TextSize(text, theme::kTextSize, keys).y * 0.5f;
            DrawText(draw, text, theme::kTextSize, ImVec2(left, top), theme::kText, keys);
            DrawText(draw, text, theme::kTextSize, ImVec2(left + column, top), theme::kTextMuted, rest);
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

// The fields for a split's keys and velocities at `pos`. Enter makes the edit. A value typed while busy waits for
// `item`.
void DrawDseRanges(const edit::DseSplit& split, BankEditorState& state, const ShownItem& item, bool busy, ImVec2 pos,
                   std::optional<edit::DseSplit>& changed, std::string& name, BankEdit& edit)
{
    const auto number = [&](const char* id, std::array<char, 8>& text, bool& typing, std::size_t at, const char* action,
                            const char* refusal)
    {
        const std::optional<int> typed =
            KeyField(id, text, typing, split[at], Px(56.0f), busy, state.typed_waiting, item, edit, refusal);
        if (typed && *typed != split[at])
        {
            changed = split;
            (*changed)[at] = static_cast<uint8_t>(*typed);
            name = action;
        }
    };

    constexpr char kVelocityRefusal[] = "Velocities range from 0 to 127.";
    constexpr char kKeyRefusal[] = "Keys range from 0 to 127.";
    ImGui::SetCursorScreenPos(pos);
    ImGui::BeginDisabled(busy);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Keys");
    ImGui::SameLine();
    number("##low-key", state.typed_key, state.typing_key, edit::kDseLowKey, "Change Keys", kKeyRefusal);
    ImGui::SameLine();
    ImGui::TextUnformatted("to");
    ImGui::SameLine();
    number("##high-key", state.typed_last, state.typing_last, edit::kDseHighKey, "Change Keys", kKeyRefusal);
    ImGui::SameLine(0.0f, Px(kGap));
    ImGui::TextUnformatted("Velocities");
    ImGui::SameLine();
    number("##low-velocity", state.typed_velocity, state.typing_velocity, edit::kDseLowVelocity, "Change Velocities",
           kVelocityRefusal);
    ImGui::SameLine();
    ImGui::TextUnformatted("to");
    ImGui::SameLine();
    number("##high-velocity", state.typed_top_velocity, state.typing_top_velocity, edit::kDseHighVelocity,
           "Change Velocities", kVelocityRefusal);
    ImGui::EndDisabled();
}

// The fields of a split, in two tables side by side: what it plays and how on the left, and its envelope on the right.
// `changed` and `name` take a new split and the name of its edit.
void DrawDseFields(const DseBankView& view, const DseSplitRow& row, BankEditorState& state, bool busy, ImVec2 pos,
                   ImVec2 size, std::optional<edit::DseSplit>& changed, std::string& name)
{
    const edit::DseSplit& split = row.values;
    const edit::DseSplit& current = DseShown(split, state);
    bool released = false;
    const float half = (size.x - Px(kGap)) * 0.5f;
    ImGui::BeginDisabled(busy);
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(ImGui::GetStyle().CellPadding.x, Px(kFieldPadding)));
    if (BeginFieldTable("##dse-playing", pos, half))
    {
        FieldLabel("Wave", kDseWaveTip);
        const auto found = std::ranges::find(view.waves, static_cast<uint16_t>(edit::DseSplitWave(split)));
        int picked = found != view.waves.end() ? static_cast<int>(found - view.waves.begin()) : -1;
        const std::string missing = "Wave " + std::to_string(edit::DseSplitWave(split));
        ImGui::SetNextWindowSizeConstraints(ImVec2(Px(280.0f), 0.0f), ImVec2(FLT_MAX, FLT_MAX));
        if (FilteredCombo("##wave", view.wave_names, picked, picked < 0 ? missing.c_str() : nullptr, state.filter) &&
            picked >= 0 && view.waves[static_cast<std::size_t>(picked)] != edit::DseSplitWave(split))
        {
            changed = split;
            edit::SetDseSplitWave(*changed, view.waves[static_cast<std::size_t>(picked)]);
            name = "Change Wave";
        }

        const auto high = static_cast<float>(edit::kHighestDseByte);
        DseSliderRow(
            "Root key", "Root Key", kDseRootTip, split, [](const edit::DseSplit& values)
            { return static_cast<float>(edit::DseRootKey(values)); }, [](edit::DseSplit& values, float value)
            { edit::SetDseRootKey(values, static_cast<int>(std::lround(value))); }, {0.0f, high}, {0.0f, high},
            static_cast<float>(kMiddleC), "%.0f  (" + KeyName(edit::DseRootKey(current)) + ")", state, &released);
        constexpr float kTuningUnits = 256.0f;
        DseSliderRow(
            "Tuning", "Tuning", kDseTuningTip, split,
            [](const edit::DseSplit& values) { return static_cast<float>(edit::DseTuning(values)) / kTuningUnits; },
            [](edit::DseSplit& values, float value)
            { edit::SetDseTuning(values, static_cast<int>(std::lround(value * kTuningUnits))); }, {-24.0f, 24.0f},
            {-128.0f, 127.99f}, 0.0f, "%.2f", state, &released);
        for (const DseField& field : kDsePlaying)
        {
            const std::string format =
                field.at == edit::kDsePan ? "%.0f  (" + PanOf(current[edit::kDsePan]) + ")" : "%.0f";
            DseRow(field, split, format, state, &released);
        }
        ImGui::EndTable();
    }

    if (BeginFieldTable("##dse-envelope", ImVec2(pos.x + half + Px(kGap), pos.y), half))
    {
        for (const DseField& field : kDseEnvelope)
        {
            DseRow(field, split, "%.0f", state, &released);
        }
        ImGui::EndTable();
    }
    ImGui::PopStyleVar();
    ImGui::EndDisabled();

    // A slider that the user let go of makes its change, if it ended elsewhere.
    if (released && state.dse_held)
    {
        if (*state.dse_held != split)
        {
            changed = *state.dse_held;
            name = "Change " + state.held_name;
        }
        state.dse_held.reset();
    }
}

// The facts under a DSE bank's fields: the sounds that an edit changes.
void DrawDseFacts(const DseBankView& view, ImVec2 pos, ImVec2 size, bool top)
{
    BeginFacts(pos, size, top);
    if (view.sounds.empty())
    {
        MutedText("No sound uses this bank.");
    }
    else
    {
        FactLine(
            "Edits affect " + std::to_string(view.sounds.size()) + (view.sounds.size() == 1 ? " sound: " : " sounds: "),
            view.sounds);
    }
    ImGui::PopTextWrapPos();
    ImGui::EndChild();
}

} // namespace

BankEdit DrawDseBankEditor(const DseBankView& view, BankEditorState& state, const ShownItem& bank, bool busy,
                           bool* open)
{
    // The window of a bank, with the same parts as the window of a DS bank: the list of programs on the left, then a
    // program's splits, the keys and velocities of the selected one, the keyboard and its fields. The facts go across
    // the bottom.
    const std::string title = "Bank Editor - " + view.name + "###BankEditor";
    BankEdit edit;
    ForgetDropTargets(state);
    if (!BeginBankEditorWindow(title, state, open))
    {
        return edit;
    }

    const BankEditorLayout layout = BankLayoutOf(kFieldRows, "PROGRAM");
    const float ranges_top = layout.keyboard_top - Px(kGap) - ImGui::GetFrameHeight();
    const ImVec2 rows_min(layout.right, layout.rows_top);
    const ImVec2 rows_max(layout.right + layout.right_width, ranges_top - Px(kGap));
    const bool top = state.reveal;
    DrawDsePrograms(view, state, layout.origin, ImVec2(Px(kListWidth), layout.facts_top - layout.origin.y - Px(kGap)));
    ImDrawList* draw = ImGui::GetWindowDrawList();
    DrawListDivider(draw, layout);

    const DseProgramView* program = SelectedDseProgram(view, state);
    std::optional<std::pair<int, int>> covered;
    std::optional<int> root;
    if (program != nullptr)
    {
        DrawCaps(draw, ImVec2(layout.right, layout.origin.y), theme::kTextMuted,
                 "Program " + std::to_string(program->program));
        const DseSplitRow* row = SelectedDseRow(*program, state);
        const ShownItem typed =
            TypedItemOf(bank, program->program, row != nullptr ? std::optional(row->split) : std::nullopt);
        DrawDseRows(*program, state, rows_min, ImVec2(rows_max.x - rows_min.x, rows_max.y - rows_min.y));
        if (row == nullptr)
        {
            EmptyState(draw, ImVec2(layout.right, layout.fields_top),
                       ImVec2(layout.right + layout.right_width, layout.fields_top + layout.fields_height), "No split",
                       "The program plays nothing.");
        }
        else
        {
            const edit::DseSplit& shown = DseShown(row->values, state);
            covered = std::pair(int{shown[edit::kDseLowKey]}, int{shown[edit::kDseHighKey]});
            root = edit::DseRootKey(shown);
            std::optional<edit::DseSplit> changed;
            std::string name;
            DrawDseRanges(row->values, state, typed, busy, ImVec2(layout.right, ranges_top), changed, name, edit);
            PushSliderItem(typed);
            DrawDseFields(view, *row, state, busy, ImVec2(layout.right, layout.fields_top),
                          ImVec2(layout.right_width, layout.fields_height), changed, name);
            PopSliderItem();
            if (changed && !busy)
            {
                edit.dse_split = std::pair(edit::DseSplitPlace{view.bank, program->program, row->split}, *changed);
                edit.name = std::move(name);
                edit.details = ProgramDetails(view.name, program->program) + ", split " + std::to_string(row->split);
            }
        }
    }
    else
    {
        EmptyState(draw, rows_min, rows_max, "No programs", "The bank has no instruments.");
    }
    DrawKeyboard(covered, root, state, ImVec2(layout.right, layout.keyboard_top),
                 ImVec2(layout.right + layout.right_width, layout.keyboard_top + Px(kKeyboardHeight)));
    if (program == nullptr)
    {
        state.sounding = -1;
    }

    DrawFactsDivider(draw, layout);
    DrawDseFacts(view, ImVec2(layout.origin.x, layout.facts_top),
                 ImVec2(layout.room.x, layout.origin.y + layout.room.y - layout.facts_top), top);
    ImGui::End();

    return edit;
}

} // namespace hokuto::app
