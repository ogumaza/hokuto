// SPDX-License-Identifier: MIT

// The Bank Editor for a DS bank: the programs with their slots, the structure and rows of a drum set or a key
// split, the fields of the selected region, and the facts.

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ntdlib/sdat/formats.h"

#include "app/bank_editor.h"
#include "app/bank_editor_parts.h"
#include "app/browser.h"
#include "app/item_views.h"
#include "app/key_edits.h"
#include "app/theme.h"
#include "app/widgets.h"
#include "edit/ds_banks.h"

namespace hokuto::app
{
namespace
{

using bank_editor_parts::AcceptedWave;
using bank_editor_parts::AcceptWave;
using bank_editor_parts::BankEditorLayout;
using bank_editor_parts::BankLayoutOf;
using bank_editor_parts::BeginBankEditorWindow;
using bank_editor_parts::BeginFacts;
using bank_editor_parts::BeginFieldTable;
using bank_editor_parts::DrawFactsDivider;
using bank_editor_parts::DrawKeyboard;
using bank_editor_parts::DrawListDivider;
using bank_editor_parts::DrawProgramList;
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
using bank_editor_parts::NeighbourProgram;
using bank_editor_parts::NumberField;
using bank_editor_parts::PanOf;
using bank_editor_parts::ProgramButtonWidths;
using bank_editor_parts::ProgramDetails;
using bank_editor_parts::ProgramNumberField;
using bank_editor_parts::SelectedOrFirst;
using bank_editor_parts::SwitchRow;
using bank_editor_parts::TypedItemOf;
using theme::Px;

// The same on `area` of the window, whatever items are there.
std::optional<BrowserPlace> AcceptWaveIn(const ImRect& area)
{
    return ImGui::BeginDragDropTargetCustom(area, ImGui::GetID("##wave-area")) ? AcceptedWave() : std::nullopt;
}

// The DS's tooltips. The envelope's values run from 0 to 127. The driver turns each into a rate or a level by its
// tables.
const char* const kDsKindTip =
    "Region type: a wave from the wave archives of the bank, a square wave on channels 8 to 13, or noise on channel "
    "14 or 15.";
const char* const kDsKindGreyTip = "Hokuto doesn't support changing this region type.";
const char* const kDsSlotsTip =
    "Wave archives that the regions of this bank use. Each region refers to one of the four slots. A slot without a "
    "wave archive plays nothing.";
const char* const kDsWaveTip = "Wave that the region plays, from a wave archive in one of the four slots of the bank.";
const char* const kDsDutyTip =
    "Duty cycle of the square wave, in eighths. At 7 the wave stays low and the region is silent.";
const char* const kDsRootTip = "Key at which the region plays at its original pitch. Other keys play higher or lower.";
const char* const kDsAttackTip = "Attack rate, 0–127. 0 is the slowest, and 127 starts at full level.";
const char* const kDsDecayTip = "Decay rate, 0–127. Higher values decay faster.";
const char* const kDsSustainTip = "Sustain level while the key is held, 0–127. 0 is silent and 127 is full level.";
const char* const kDsReleaseTip = "Release rate, 0–127. Higher values fade out faster after the key is released.";

// The text that shows for the kind of a DS program where its rows would go.
std::string DsKindText(ntdlib::sdat::InstrumentType type)
{
    using enum ntdlib::sdat::InstrumentType;
    switch (type)
    {
    case kPcm:
        return "Plays a wave from the wave archives of the bank.";
    case kPsg:
        return "Plays a square wave on one of DS channels 8 to 13.";
    case kNoise:
        return "Plays noise on DS channel 14 or 15.";
    case kDirectPcm:
        return "Plays a wave at an address that the game sets at run time. Hokuto can't show or change it.";
    case kDummy:
        return "Plays nothing.";
    default:
        break;
    }

    return "Has no instrument.";
}

// A field of a DS region that contains a number.
using DsField = NumberField<ntdlib::sdat::Region>;

const DsField kDsDuty = {"Duty",
                         "Duty",
                         kDsDutyTip,
                         edit::kHighestDsDuty,
                         3,
                         [](const ntdlib::sdat::Region& region) { return static_cast<int>(region.wave); },
                         [](ntdlib::sdat::Region& region, int value) { region.wave = static_cast<uint16_t>(value); }};
const DsField kDsRoot = {"Root key",
                         "Root Key",
                         kDsRootTip,
                         edit::kHighestDsByte,
                         static_cast<int>(kMiddleC),
                         [](const ntdlib::sdat::Region& region) { return static_cast<int>(region.root_key); },
                         [](ntdlib::sdat::Region& region, int value)
                         { region.root_key = static_cast<uint8_t>(value); }};
const DsField kDsPan = {"Pan",
                        "Pan",
                        kPanTip,
                        edit::kHighestDsByte,
                        static_cast<int>(kCentre),
                        [](const ntdlib::sdat::Region& region) { return static_cast<int>(region.pan); },
                        [](ntdlib::sdat::Region& region, int value) { region.pan = static_cast<uint8_t>(value); }};
const std::array<DsField, 4> kDsEnvelope = {{
    {"Attack", "Attack", kDsAttackTip, edit::kHighestDsByte, edit::kHighestDsByte,
     [](const ntdlib::sdat::Region& region) { return static_cast<int>(region.attack); },
     [](ntdlib::sdat::Region& region, int value) { region.attack = static_cast<uint8_t>(value); }},
    {"Decay", "Decay", kDsDecayTip, edit::kHighestDsByte, edit::kHighestDsByte,
     [](const ntdlib::sdat::Region& region) { return static_cast<int>(region.decay); },
     [](ntdlib::sdat::Region& region, int value) { region.decay = static_cast<uint8_t>(value); }},
    {"Sustain", "Sustain", kDsSustainTip, edit::kHighestDsByte, edit::kHighestDsByte,
     [](const ntdlib::sdat::Region& region) { return static_cast<int>(region.sustain); },
     [](ntdlib::sdat::Region& region, int value) { region.sustain = static_cast<uint8_t>(value); }},
    {"Release", "Release", kDsReleaseTip, edit::kHighestDsByte, edit::kHighestDsByte,
     [](const ntdlib::sdat::Region& region) { return static_cast<int>(region.release); },
     [](ntdlib::sdat::Region& region, int value) { region.release = static_cast<uint8_t>(value); }},
}};

// The row of `field` for `region`, with the value as `format` writes it. GbaRow does the same for a GBA voice.
void DsRow(const DsField& field, const ntdlib::sdat::Region& region, const std::string& format, BankEditorState& state,
           bool* released)
{
    FieldLabel(field.label, field.tooltip);
    const bool holding = state.ds_held && state.held_name == field.name;
    ntdlib::sdat::Region changed = holding ? *state.ds_held : region;
    auto shown = static_cast<float>(field.get(changed));
    const auto high = static_cast<float>(field.high);
    const SliderRange typed{0.0f, high};
    ImGui::PushID(field.label);
    if (ValueSlider("##value", &shown, {0.0f, high}, static_cast<float>(field.reset), format.c_str(),
                    ImVec2(ImGui::GetContentRegionAvail().x, ImGui::GetFrameHeight()), &typed))
    {
        field.set(changed, static_cast<int>(std::lround(std::clamp(shown, 0.0f, high))));
        state.ds_held = changed;
        state.held_name = field.name;
    }
    *released = *released || (SliderReleased() && state.ds_held && state.held_name == field.name);
    ImGui::PopID();
}

// The region that the DS's fields show: the one that the user holds a slider of, or else `region`.
const ntdlib::sdat::Region& DsShown(const ntdlib::sdat::Region& region, const BankEditorState& state)
{
    return state.ds_held ? *state.ds_held : region;
}

bool DsSplit(const DsProgram& program)
{
    return program.type == ntdlib::sdat::InstrumentType::kDrumSet ||
           program.type == ntdlib::sdat::InstrumentType::kKeySplit;
}

// The program of `view` that the editor shows: the state's while the view contains it, or else the first. A new program
// selects its first row. While an edit waits for its version (`waiting`), the program that the edit selects can be
// missing from the view. The editor then shows the first program and keeps the selection.
const DsProgram* SelectedDsProgram(const DsBankView& view, BankEditorState& state, bool waiting)
{
    const auto forget = [&state]
    {
        state.ds_row.reset();
        state.ds_held.reset();
    };
    return SelectedOrFirst(view.programs, state.program, &DsProgram::program, waiting, forget);
}

// The row of a program that the editor shows: the state's while the program has it, or else the first. While an edit
// waits for its version (`waiting`), the row that the edit selects can be missing from the program. The editor then
// shows the first row and keeps the selection.
const DsRegionRow* SelectedDsRow(const DsProgram& program, BankEditorState& state, bool waiting)
{
    return SelectedOrFirst(program.rows, state.ds_row, &DsRegionRow::first_key, waiting,
                           [&state] { state.ds_held.reset(); });
}

// The details of a DS edit in the Undo History: the bank, the program and the keys of a drum set or a key split.
std::string DsDetails(const DsBankView& view, const DsProgram& program, const DsRegionRow& row)
{
    std::string details = ProgramDetails(view.name, program.program);
    if (DsSplit(program))
    {
        details += KeysDetail(row.first_key, row.last_key);
    }

    return details;
}

// Sets `edit` to the bank `model` under the name `name`, as an edit of program `program`.
void SetDsBankEdit(const DsBankView& view, std::string name, ntdlib::sdat::Bank model, uint32_t program, BankEdit& edit)
{
    edit.ds_model = std::move(model);
    edit.name = std::move(name);
    edit.details = ProgramDetails(view.name, program);
}

// Adds an instrument of one region after the selected program, or at program 0 when `chosen` is false.
void DsAddInstrument(const DsBankView& view, bool chosen, BankEditorState& state, BankEdit& edit)
{
    const std::optional<uint32_t> free = edit::FreeDsProgram(view.model, chosen ? *state.program + 1 : 0);
    if (free)
    {
        const std::optional<edit::DsWavePlace> wave =
            view.waves.empty() ? std::nullopt
                               : std::optional(edit::DsWavePlace{view.waves.front().slot, view.waves.front().wave});
        SetDsBankEdit(view, "Add Instrument", edit::WithNewDsInstrument(view.model, *free, edit::NewDsRegion(wave)),
                      *free, edit);
        state.program = free;
        state.ds_row.reset();
    }
    else
    {
        edit.refused = "The bank has no free program after the selected one.";
    }
}

// Copies the selected instrument to the first free program after it.
void DsDuplicateInstrument(const DsBankView& view, BankEditorState& state, BankEdit& edit)
{
    const std::optional<uint32_t> free = edit::FreeDsProgram(view.model, *state.program + 1);
    if (free)
    {
        SetDsBankEdit(view, "Duplicate Instrument", edit::WithDsInstrumentCopy(view.model, *state.program, *free),
                      *free, edit);
        state.program = free;
    }
    else
    {
        edit.refused = "The bank has no free program after the selected one.";
    }
}

// Deletes the selected instrument, `selected` in `view.programs`.
void DsDeleteInstrument(const DsBankView& view, std::vector<DsProgram>::const_iterator selected, BankEditorState& state,
                        BankEdit& edit)
{
    SetDsBankEdit(view, "Delete Instrument", edit::WithoutDsInstrument(view.model, *state.program), *state.program,
                  edit);
    state.program = NeighbourProgram(view.programs, selected, &DsProgram::program);
    state.ds_row.reset();
    state.ds_held.reset();
}

// The popup of the four slots of wave archives of the bank.
void DrawDsSlotsPopup(const DsBankView& view, BankEditorState& state, BankEdit& edit)
{
    if (ImGui::BeginPopup("##ds-slots"))
    {
        std::vector<std::string> names = {"None"};
        names.insert(names.end(), view.wave_archive_names.begin(), view.wave_archive_names.end());
        for (std::size_t slot = 0; slot < view.slots.size(); ++slot)
        {
            const auto held = std::ranges::find(view.wave_archives, uint32_t{view.slots[slot]});
            int picked = held != view.wave_archives.end() ? static_cast<int>(held - view.wave_archives.begin()) + 1 : 0;
            const int before = picked;
            ImGui::AlignTextToFramePadding();
            ImGui::Text("Slot %zu", slot);
            ImGui::SameLine();
            ImGui::PushID(static_cast<int>(slot));
            if (FilteredCombo("##slot", names, picked, nullptr, state.filter, Px(280.0f)) && picked != before)
            {
                std::array<uint16_t, 4> changed = view.slots;
                changed[slot] = picked <= 0
                                    ? ntdlib::sdat::kNoWaveArchive
                                    : static_cast<uint16_t>(view.wave_archives[static_cast<std::size_t>(picked) - 1]);
                edit.ds_slots = changed;
                edit.name = "Change Wave Archive";
                edit.details = view.name + ", slot " + std::to_string(slot);
            }
            ImGui::PopID();
        }
        ImGui::EndPopup();
    }
}

// The label of `program` in the list of programs: the kind of its instrument, with the count of the regions of a split.
std::string DsProgramLabel(const DsProgram& program)
{
    std::string kind = program.kind;
    if (DsSplit(program))
    {
        kind += ", " + std::to_string(program.rows.size()) + (program.rows.size() == 1 ? " region" : " regions");
    }

    return kind;
}

// Moves the selected instrument to program `to`. A model that refuses the move sets the message in `edit.refused`.
void RenumberDsInstrument(const DsBankView& view, uint32_t to, BankEditorState& state, BankEdit& edit)
{
    try
    {
        SetDsBankEdit(view, "Renumber Instrument", edit::WithDsInstrumentMoved(view.model, *state.program, to), to,
                      edit);
        state.program = to;
    }
    catch (const std::invalid_argument& error)
    {
        edit.refused = error.what();
    }
}

// The list of the programs of a DS bank at `pos`, `size` large: the number of each program and the kind of its
// instrument. A click selects a program.
void DrawDsPrograms(const DsBankView& view, BankEditorState& state, const ShownItem& bank, bool busy, ImVec2 pos,
                    ImVec2 size, BankEdit& edit)
{
    ImDrawList* draw = ImGui::GetWindowDrawList();
    DrawCaps(draw, pos, theme::kTextMuted, "PROGRAMS");
    const float caps = CapsSize("PROGRAMS").y + Px(6.0f);
    const float controls = ImGui::GetFrameHeight() * 3.0f + Px(kGap) * 2.0f;
    const ImVec2 list_min(pos.x, pos.y + caps);
    const ImVec2 list_size(size.x, std::max(size.y - caps - controls - Px(kGap), Px(kRowHeight)));
    state.list_min = list_min;
    state.list_max = ImVec2(list_min.x + list_size.x, list_min.y + list_size.y);
    const auto select = [&state](uint32_t program)
    {
        state.program = program;
        state.ds_row.reset();
        state.ds_held.reset();
    };
    DrawProgramList(view.programs, DsProgramLabel, select, state, list_min, list_size, "No instruments");
    if (const std::optional<BrowserPlace> wave = AcceptWave(); wave && !busy)
    {
        edit.wave = wave;
        edit.drop = BankDrop{std::nullopt, 60, 64};
    }

    // Add makes an instrument of one region at the first free program after the selected one: the first wave of the
    // bank's slots, or a square wave. Duplicate copies the selected instrument there. Delete takes it out. The field
    // moves it to the program that the user types. The buttons share the list's width by the widths of their labels.
    const float below = list_min.y + list_size.y + Px(kGap);
    ImGui::SetCursorScreenPos(ImVec2(pos.x, below));
    const std::array<float, 3> width = ProgramButtonWidths(size.x);

    const auto selected =
        state.program ? std::ranges::find(view.programs, *state.program, &DsProgram::program) : view.programs.end();
    const bool chosen = selected != view.programs.end();
    ImGui::BeginDisabled(busy);
    if (ImGui::Button("Add", ImVec2(width[0], 0.0f)))
    {
        DsAddInstrument(view, chosen, state, edit);
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!chosen);
    if (ImGui::Button("Duplicate", ImVec2(width[1], 0.0f)) && chosen)
    {
        DsDuplicateInstrument(view, state, edit);
    }
    ImGui::SameLine();
    if (ImGui::Button("Delete", ImVec2(width[2], 0.0f)) && chosen)
    {
        DsDeleteInstrument(view, selected, state, edit);
    }

    // The program's field shows the selected program. Enter moves the instrument to the program that the user typed.
    if (const std::optional<uint32_t> to =
            ProgramNumberField(state, bank, chosen, busy, pos, size, edit::kHighestDsProgram, edit))
    {
        RenumberDsInstrument(view, *to, state, edit);
    }
    ImGui::EndDisabled();

    // The bank's four slots of wave archives, in a popup.
    ImGui::SetCursorScreenPos(ImVec2(pos.x, ImGui::GetCursorScreenPos().y));
    if (ImGui::Button("Wave Archives…", ImVec2(size.x, 0.0f)))
    {
        ImGui::OpenPopup("##ds-slots");
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("%s", kDsSlotsTip);
    }
    DrawDsSlotsPopup(view, state, edit);
    ImGui::EndDisabled();
}

// The controls under an instrument's rows at `pos`, `width` wide: what makes an instrument of one region a key split or
// a drum set, and what splits a key split's run or sets a drum set's keys. A model that refuses an edit sets the
// message in `edit.refused`. After an edit, the editor selects the row that plays `key` in the new model (DsRowOf):
// the second run of a split, the run that gets the keys of a deleted run, a drum set's selected key, or the nearest
// key that the drum set keeps. A key typed while busy waits for `item`.
void DrawDsStructure(const DsBankView& view, const DsProgram& program, const DsRegionRow* row, BankEditorState& state,
                     const ShownItem& item, bool busy, ImVec2 pos, float width, BankEdit& edit)
{
    using enum ntdlib::sdat::InstrumentType;
    const auto make = [&](std::string name, const auto& model, int key)
    {
        try
        {
            SetDsBankEdit(view, std::move(name), model(), program.program, edit);
            state.ds_row = DsRowOf(*edit.ds_model, program.program, key);
            state.ds_held.reset();
        }
        catch (const std::invalid_argument& error)
        {
            edit.refused = error.what();
        }
    };

    ImGui::SetCursorScreenPos(pos);
    ImGui::BeginDisabled(busy);
    const float field = Px(56.0f);
    if (program.type == kKeySplit)
    {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Split at");
        ImGui::SameLine();
        const int middle = row != nullptr ? (row->first_key + row->last_key + 1) / 2 : 60;
        const std::optional<int> key = KeyField("##split", state.typed_key, state.typing_key, middle, field, busy,
                                                state.typed_waiting, item, edit);
        ImGui::SameLine();
        const bool split = ImGui::Button("Split") || key.has_value();
        if (split && row != nullptr)
        {
            const int at = key.value_or(middle);
            make("Split Region", [&] { return edit::WithDsSplit(view.model, program.program, row->region, at); }, at);
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(row == nullptr || program.rows.size() < 2);
        if (ImGui::Button("Delete Region") && row != nullptr)
        {
            const auto without = [&]
            {
                return edit::WithoutDsRun(view.model, program.program, row->region);
            };
            make("Delete Region", without, row->first_key);
        }
        ImGui::EndDisabled();
    }
    else if (program.type == kDrumSet)
    {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Keys");
        ImGui::SameLine();
        const int first = view.model.instruments[program.program].min_key;
        const int last = view.model.instruments[program.program].max_key;
        const std::optional<int> low =
            KeyField("##low", state.typed_key, state.typing_key, first, field, busy, state.typed_waiting, item, edit);
        ImGui::SameLine();
        ImGui::TextUnformatted("to");
        ImGui::SameLine();
        const std::optional<int> high =
            KeyField("##high", state.typed_last, state.typing_last, last, field, busy, state.typed_waiting, item, edit);
        if ((low && *low != first) || (high && *high != last))
        {
            // The selected key stays selected, or else the nearest key that the drum set keeps.
            const int lowest = low.value_or(first);
            const int highest = high.value_or(last);
            const int kept = std::max(lowest, std::min(row != nullptr ? row->first_key : lowest, highest));
            const auto keys = [&]
            {
                return edit::WithDsDrumKeys(view.model, program.program, lowest, highest);
            };
            make("Change Keys", keys, kept);
        }
    }
    else
    {
        if (ImGui::Button("Make Key Split"))
        {
            make("Make Key Split", [&] { return edit::WithDsKeySplit(view.model, program.program); }, 0);
        }
        ImGui::SameLine();
        if (ImGui::Button("Make Drum Set"))
        {
            make("Make Drum Set", [&] { return edit::WithDsDrumSet(view.model, program.program, 60); }, 60);
        }
    }
    if (DsSplit(program) && row != nullptr)
    {
        const float label = ImGui::CalcTextSize("Keep This Region").x + ImGui::GetStyle().FramePadding.x * 2.0f;
        ImGui::SameLine(pos.x + width - label - ImGui::GetWindowPos().x);
        if (ImGui::Button("Keep This Region"))
        {
            make("Make One Region", [&] { return edit::WithDsOneRegion(view.model, program.program, row->region); }, 0);
        }
    }
    ImGui::EndDisabled();
}

// The rows of a drum set or a key split at `pos`, `size` large: each row's keys and the kind of its region. A click
// selects a row.
void DrawDsRows(const DsProgram& program, BankEditorState& state, ImVec2 pos, ImVec2 size)
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
            const DsRegionRow& row = program.rows[static_cast<std::size_t>(index)];
            ImGui::PushID(index);
            const float left = ImGui::GetCursorScreenPos().x;
            if (ImGui::Selectable("##row", state.ds_row == row.first_key, 0, ImVec2(0.0f, Px(kRowHeight) - Px(6.0f))) &&
                state.ds_row != row.first_key)
            {
                state.ds_row = row.first_key;
                state.ds_held.reset();
            }
            ImGui::PopID();
            const float top = (ImGui::GetItemRectMin().y + ImGui::GetItemRectMax().y) * 0.5f -
                              TextSize(text, theme::kTextSize, row.kind).y * 0.5f;
            DrawText(draw, text, theme::kTextSize, ImVec2(left, top), theme::kText,
                     KeysText(row.first_key, row.last_key));
            DrawText(draw, text, theme::kTextSize, ImVec2(left + column, top), theme::kTextMuted, row.kind);
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

// The text of a square wave's duty as its row shows it.
std::string DsDutyText(int duty)
{
    if (duty >= edit::kHighestDsDuty)
    {
        return "%.0f  (silent)";
    }

    std::array<char, 32> text{};
    std::snprintf(text.data(), text.size(), "%%.0f  (%.1f %%%%)", 12.5 * (duty + 1));
    return text.data();
}

// The row of the kind of `region`, among those that play. Returns the region with the kind that the user chose, or
// nothing. A wave without the waves to choose from sets the refusal in `edit`.
std::optional<ntdlib::sdat::Region> DsKindRow(const DsBankView& view, const ntdlib::sdat::Region& region,
                                              BankEdit& edit)
{
    namespace sdat = ntdlib::sdat;
    // The kind, among those that play. A new kind keeps the bytes that it can: a wave uses the first wave of the
    // bank's slots, and a square wave a duty of a half.
    constexpr std::array<sdat::InstrumentType, 3> kKinds = {sdat::InstrumentType::kPcm, sdat::InstrumentType::kPsg,
                                                            sdat::InstrumentType::kNoise};
    const auto kind = std::ranges::find(kKinds, region.type);
    const bool fixed = kind == kKinds.end();
    const int chosen =
        SwitchRow("Kind", fixed ? kDsKindGreyTip : kDsKindTip, std::array<std::string, 3>{"Wave", "Square", "Noise"},
                  fixed ? -1 : static_cast<int>(kind - kKinds.begin()), fixed);
    if (chosen >= 0)
    {
        sdat::Region after = region;
        after.type = kKinds[static_cast<std::size_t>(chosen)];
        if (after.type == sdat::InstrumentType::kPcm &&
            std::ranges::find(view.waves, DsWaveChoice{region.wave_archive, region.wave}) == view.waves.end())
        {
            if (view.waves.empty())
            {
                edit.refused = "No wave archive in the slots of the bank has any waves.";
            }
            else
            {
                after.wave_archive = view.waves.front().slot;
                after.wave = view.waves.front().wave;
            }
        }
        if (after.type == sdat::InstrumentType::kPsg && region.type != sdat::InstrumentType::kPsg)
        {
            after.wave = 3;
        }
        if (edit.refused.empty())
        {
            return after;
        }
    }

    return std::nullopt;
}

// The row of the wave of `region`. Returns the region with the wave that the user chose, or nothing.
std::optional<ntdlib::sdat::Region> DsWaveRow(const DsBankView& view, const ntdlib::sdat::Region& region,
                                              BankEditorState& state)
{
    namespace sdat = ntdlib::sdat;
    FieldLabel("Wave", kDsWaveTip);
    const auto found = std::ranges::find(view.waves, DsWaveChoice{region.wave_archive, region.wave});
    int picked = found != view.waves.end() ? static_cast<int>(found - view.waves.begin()) : -1;
    const std::string missing = "Slot " + std::to_string(region.wave_archive) + ", wave " + std::to_string(region.wave);
    ImGui::SetNextWindowSizeConstraints(ImVec2(Px(280.0f), 0.0f), ImVec2(FLT_MAX, FLT_MAX));
    if (FilteredCombo("##wave", view.wave_names, picked, picked < 0 ? missing.c_str() : nullptr, state.filter) &&
        picked >= 0 && view.waves[static_cast<std::size_t>(picked)] != DsWaveChoice{region.wave_archive, region.wave})
    {
        sdat::Region after = region;
        after.wave_archive = view.waves[static_cast<std::size_t>(picked)].slot;
        after.wave = view.waves[static_cast<std::size_t>(picked)].wave;
        return after;
    }

    return std::nullopt;
}

// The fields of a DS region, in two tables side by side: its kind, what it plays, its root key and its pan on the left,
// and its envelope on the right.
void DrawDsFields(const DsBankView& view, const DsProgram& program, const DsRegionRow& row, BankEditorState& state,
                  bool busy, ImVec2 pos, ImVec2 size, BankEdit& edit)
{
    namespace sdat = ntdlib::sdat;
    const sdat::Region& region = row.values;
    const sdat::Region& current = DsShown(region, state);
    std::optional<sdat::Region> changed;
    std::string name;
    const auto change = [&](const sdat::Region& after, std::string action)
    {
        changed = after;
        name = std::move(action);
    };
    bool released = false;
    const float half = (size.x - Px(kGap)) * 0.5f;
    ImGui::BeginDisabled(busy);
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(ImGui::GetStyle().CellPadding.x, Px(kFieldPadding)));
    if (BeginFieldTable("##ds-playing", pos, half))
    {
        if (const std::optional<sdat::Region> after = DsKindRow(view, region, edit))
        {
            change(*after, "Change Kind");
        }

        if (region.type == sdat::InstrumentType::kPcm)
        {
            if (const std::optional<sdat::Region> after = DsWaveRow(view, region, state))
            {
                change(*after, "Change Wave");
            }
        }
        else if (region.type == sdat::InstrumentType::kPsg)
        {
            DsRow(kDsDuty, region, DsDutyText(kDsDuty.get(current)), state, &released);
        }
        DsRow(kDsRoot, region, "%.0f  (" + KeyName(current.root_key) + ")", state, &released);
        DsRow(kDsPan, region, "%.0f  (" + PanOf(current.pan) + ")", state, &released);
        ImGui::EndTable();
    }

    // The envelope.
    if (BeginFieldTable("##ds-envelope", ImVec2(pos.x + half + Px(kGap), pos.y), half))
    {
        for (const DsField& field : kDsEnvelope)
        {
            DsRow(field, region, "%.0f", state, &released);
        }
        ImGui::EndTable();
    }
    ImGui::PopStyleVar();
    ImGui::EndDisabled();

    // A slider that the user let go of makes its change, unless it ended on its old value.
    if (released && state.ds_held)
    {
        if (*state.ds_held != region)
        {
            change(*state.ds_held, "Change " + state.held_name);
        }
        state.ds_held.reset();
    }
    if (busy || !changed)
    {
        return;
    }
    edit.ds_region = std::pair(edit::DsRegionPlace{view.bank, program.program, row.region}, *changed);
    edit.name = name;
    edit.details = DsDetails(view, program, row);
}

// The facts of a DS bank at `pos`, `size` large: the sequences that play it, the other banks that play its file, and
// the other places that play the shown region's bytes.
void DrawDsFacts(const DsBankView& view, const DsRegionRow* row, ImVec2 pos, ImVec2 size, bool top)
{
    BeginFacts(pos, size, top);
    if (view.sounds.empty())
    {
        MutedText("No sequence uses this bank.");
    }
    else
    {
        FactLine("Edits affect " + std::to_string(view.sounds.size()) +
                     (view.sounds.size() == 1 ? " sequence: " : " sequences: "),
                 view.sounds);
    }
    if (!view.sharing_banks.empty())
    {
        FactLine("The bank file is also used by ", view.sharing_banks);
    }
    if (row != nullptr && !row->shared.empty())
    {
        FactLine("Editing the region also changes it at ", row->shared);
    }
    ImGui::PopTextWrapPos();
    ImGui::EndChild();
}

// What the right side shows for a program: the selected region's row, the keys that it covers and its root key.
struct DsProgramSide
{
    const DsRegionRow* row = nullptr;
    std::optional<std::pair<int, int>> covered;
    std::optional<int> root;
};

// The right side of the window for `program`: its rows, or a card for an instrument of one region, its structure and
// its fields.
DsProgramSide DrawDsProgramSide(const DsBankView& view, const DsProgram& program, BankEditorState& state,
                                const ShownItem& bank, bool busy, const BankEditorLayout& layout, BankEdit& edit)
{
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float structure_top = layout.keyboard_top - Px(kGap) - ImGui::GetFrameHeight();
    const ImVec2 rows_min(layout.right, layout.rows_top);
    const ImVec2 rows_max(layout.right + layout.right_width, structure_top - Px(kGap));
    const DsRegionRow* row = nullptr;
    std::optional<std::pair<int, int>> covered;
    std::optional<int> root;

    // A wave dropped from the browser, or a file that the system drops, on the rows, the keyboard or the fields
    // lands on the selected region (DropAt, DroppedDsRegion).
    state.map_min = rows_min;
    state.map_max = ImVec2(layout.right + layout.right_width, layout.fields_top + layout.fields_height);
    if (const std::optional<BrowserPlace> wave = AcceptWaveIn(ImRect(state.map_min, state.map_max)); wave && !busy)
    {
        edit.wave = wave;
        edit.drop = BankDrop{program.program, 60, 64};
    }

    DrawCaps(draw, ImVec2(layout.right, layout.origin.y), theme::kTextMuted,
             "Program " + std::to_string(program.program) + ": " + program.kind);
    const ImVec2 fields_min(layout.right, layout.fields_top);
    const ImVec2 fields_max(layout.right + layout.right_width, layout.fields_top + layout.fields_height);
    row = SelectedDsRow(program, state, busy || !edit.name.empty());
    // The split key of a key split belongs to its region. The keys of a drum set belong to the program.
    const bool split_key = program.type == ntdlib::sdat::InstrumentType::kKeySplit && row != nullptr;
    const ShownItem typed = TypedItemOf(
        bank, program.program, split_key ? std::optional(static_cast<std::size_t>(row->first_key)) : std::nullopt);
    if (DsSplit(program))
    {
        DrawDsRows(program, state, rows_min, ImVec2(rows_max.x - rows_min.x, rows_max.y - rows_min.y));
    }
    else
    {
        EmptyState(draw, rows_min, rows_max, program.kind, DsKindText(program.type));
    }
    DrawDsStructure(view, program, row, state, typed, busy, ImVec2(layout.right, structure_top), layout.right_width,
                    edit);

    using enum ntdlib::sdat::InstrumentType;
    if (row == nullptr)
    {
        EmptyState(draw, fields_min, fields_max, "No region", "No key of this program plays anything.");
    }
    else if (row->values.type == kDirectPcm || row->values.type == kDummy)
    {
        covered = std::pair(row->first_key, row->last_key);
        EmptyState(draw, fields_min, fields_max, row->kind, DsKindText(row->values.type));
    }
    else
    {
        covered = std::pair(row->first_key, row->last_key);
        root = DsShown(row->values, state).root_key;
        PushSliderItem(TypedItemOf(bank, program.program, static_cast<std::size_t>(row->first_key)));
        DrawDsFields(view, program, *row, state, busy, fields_min, ImVec2(layout.right_width, layout.fields_height),
                     edit);
        PopSliderItem();
    }

    return {row, covered, root};
}

} // namespace

std::optional<std::pair<uint32_t, std::size_t>> DroppedDsRegion(const DsBankView& view, const BankEditorState& state,
                                                                const BankDrop& drop)
{
    if (!drop.program)
    {
        return std::nullopt;
    }
    const auto program = std::ranges::find(view.programs, *drop.program, &DsProgram::program);
    if (program == view.programs.end() || program->rows.empty())
    {
        return std::nullopt;
    }
    const auto row =
        state.ds_row ? std::ranges::find(program->rows, *state.ds_row, &DsRegionRow::first_key) : program->rows.end();

    return std::pair(*drop.program, (row == program->rows.end() ? program->rows.front() : *row).region);
}

BankEdit DrawDsBankEditor(const DsBankView& view, BankEditorState& state, const ShownItem& bank, bool busy, bool* open)
{
    // The window of a bank, with the same parts as the window of a GBA voice group, and controls under the list and the
    // rows.
    const std::string title = "Bank Editor - " + view.name + "###BankEditor";
    BankEdit edit;
    ForgetDropTargets(state);
    if (!BeginBankEditorWindow(title, state, open))
    {
        return edit;
    }

    // The list of programs on the left. On the right, the rows of a drum set or a key split, or a card for an
    // instrument of one region, then the keyboard and the fields. The facts go across the bottom.
    const BankEditorLayout layout = BankLayoutOf(kFieldRows, "PROGRAM");
    const float structure_top = layout.keyboard_top - Px(kGap) - ImGui::GetFrameHeight();
    const ImVec2 rows_min(layout.right, layout.rows_top);
    const ImVec2 rows_max(layout.right + layout.right_width, structure_top - Px(kGap));
    const bool top = state.reveal;
    DrawDsPrograms(view, state, bank, busy, layout.origin,
                   ImVec2(Px(kListWidth), layout.facts_top - layout.origin.y - Px(kGap)), edit);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    DrawListDivider(draw, layout);

    const DsProgram* program = SelectedDsProgram(view, state, busy || !edit.name.empty());
    DsProgramSide side;
    if (program != nullptr)
    {
        side = DrawDsProgramSide(view, *program, state, bank, busy, layout, edit);
    }
    else
    {
        EmptyState(draw, rows_min, rows_max, "No programs", "The bank has no instruments.");
    }
    DrawKeyboard(side.covered, side.root, state, ImVec2(layout.right, layout.keyboard_top),
                 ImVec2(layout.right + layout.right_width, layout.keyboard_top + Px(kKeyboardHeight)));
    if (program == nullptr)
    {
        state.sounding = -1;
    }

    DrawFactsDivider(draw, layout);
    DrawDsFacts(view, side.row, ImVec2(layout.origin.x, layout.facts_top),
                ImVec2(layout.room.x, layout.origin.y + layout.room.y - layout.facts_top), top);
    ImGui::End();

    return edit;
}

} // namespace hokuto::app
