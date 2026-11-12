// SPDX-License-Identifier: MIT

// The Bank Editor for a bank of the 3DS, the Wii U, the Switch or the Wii: the programs, the key map of the
// regions, the fields of the selected region and the facts.

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ntdlib/sar/bank_file.h"
#include "ntdlib/sar/formats.h"

#include "app/bank_editor.h"
#include "app/bank_editor_parts.h"
#include "app/browser.h"
#include "app/item_views.h"
#include "app/key_edits.h"
#include "app/theme.h"
#include "app/widgets.h"
#include "edit/banks.h"

namespace hokuto::app
{
namespace
{

namespace sar = ntdlib::sar;
using bank_editor_parts::AcceptWave;
using bank_editor_parts::BankEditorLayout;
using bank_editor_parts::BankLayoutOf;
using bank_editor_parts::BeginBankEditorWindow;
using bank_editor_parts::BeginFacts;
using bank_editor_parts::BeginFieldTable;
using bank_editor_parts::DrawFactsDivider;
using bank_editor_parts::DrawKeyboard;
using bank_editor_parts::DrawListDivider;
using bank_editor_parts::FieldLabel;
using bank_editor_parts::GeometryOf;
using bank_editor_parts::kCentre;
using bank_editor_parts::kEdgeBand;
using bank_editor_parts::KeyAt;
using bank_editor_parts::KeyX;
using bank_editor_parts::kFastest;
using bank_editor_parts::kFieldPadding;
using bank_editor_parts::kFieldRows;
using bank_editor_parts::kGap;
using bank_editor_parts::kHighest;
using bank_editor_parts::kKeyboardHeight;
using bank_editor_parts::kKeys;
using bank_editor_parts::kListWidth;
using bank_editor_parts::kMiddleC;
using bank_editor_parts::kRowHeight;
using bank_editor_parts::MapGeometry;
using bank_editor_parts::PanOf;
using bank_editor_parts::ProgramButtonWidths;
using bank_editor_parts::ProgramDetails;
using bank_editor_parts::ProgramNumberField;
using bank_editor_parts::TypedItemOf;
using bank_editor_parts::VelocityAt;
using edit::Region;
using theme::Px;

// The tuning's slider runs two octaves each way, in cents. A typed value can go further.
constexpr float kTuningRange = 2400.0f;
constexpr float kTypedTuning = 9600.0f;

// The highest key group. Key group 0 cuts no other note.
constexpr float kHighestKeyGroup = 15.0f;

// The interpolation by its numbers: 0 AX's 4-tap filters on the Wii U, or the DSP's polyphase filters on the 3DS, 1
// linear, other values none.
constexpr std::array<const char*, 3> kInterpolations = {"4-tap", "Linear", "None"};
constexpr const char* kCtrFirstInterpolation = "Polyphase";

// What each field does, for the tooltips of the labels.
const char* const kKeysTip = "Key range of the region. Drag its left or right edge on the key map to change it.";
const char* const kVelocitiesTip =
    "Velocity range of the region. Drag its top or bottom edge on the key map to change it.";
const char* const kWaveTip =
    "Wave that the region plays, from a wave archive in this archive. In the game, it plays only after a group has "
    "loaded the wave archive.";
const char* const kRootTip =
    "Key at which the region plays the wave at its original sample rate. Each key above or below shifts the pitch by "
    "a semitone.";
const char* const kTuningTip = "Fine tuning, in cents (hundredths of a semitone).";
const char* const kVolumeTip =
    "Volume, 0–255. 127 is full level. Higher values are louder, up to a limit set by the console.";
const char* const kKeyGroupTip =
    "Key group, 0–15. A note in groups 1 to 15 cuts off other notes in the same group, like an open and a closed "
    "hi-hat. Group 0 cuts nothing.";
const char* const kAttackTip = "Attack rate, 0–127. Higher values rise faster. At 127 the note starts at full level.";
const char* const kHoldTip =
    "Hold time at full level after the attack, 0–127. The time grows with the square of the value, up to about 4 s.";
const char* const kDecayTip = "Decay rate, 0–127. Higher values decay faster.";
const char* const kSustainTip = "Sustain level while the key is held, 0–127. 127 is full level.";
const char* const kReleaseTip = "Release rate, 0–127. Higher values fade out faster after the key is released.";
const char* const kInterpolationTip = "How the console mixer interpolates between the samples of the wave.";
const char* const kSwitchInterpolationTip =
    "The Switch audio renderer ignores interpolation. Every region sounds the same, whatever the value.";
const char* const kWiiInterpolationTip =
    "Wii banks don't store interpolation. The Wii mixer interpolates every wave the same way.";

// Why the file's build of NW4R SND reads none of a region's hold, key group and note-off. Its builds from March 2008 on
// read them.
const char* const kOldHoldTip = "This NW4R SND build ignores hold. Builds from March 2008 on support it.";
const char* const kOldKeyGroupTip = "This NW4R SND build ignores key groups. Builds from March 2008 on support them.";
const char* const kOldNoteOffTip = "This NW4R SND build ignores note-off. Builds from March 2008 on support it.";
const char* const kNoteOffTip =
    "Whether a note-off releases the note. When ignored, the note plays the wave to its end. Drums often work this "
    "way.";

float VelocityY(const MapGeometry& map, int velocity)
{
    return map.max.y - static_cast<float>(velocity) * map.velocity;
}

ImRect RectOf(const MapGeometry& map, const Region& region)
{
    return ImRect(KeyX(map, region.first_key), VelocityY(map, region.last_velocity + 1), KeyX(map, region.last_key + 1),
                  VelocityY(map, region.first_velocity));
}

// A region's place among `regions` by its first key and first velocity. No two regions of an instrument share both.
std::optional<std::size_t> PlaceOf(const std::vector<Region>& regions, const std::optional<std::pair<int, int>>& region)
{
    if (!region)
    {
        return std::nullopt;
    }

    for (std::size_t r = 0; r < regions.size(); ++r)
    {
        if (regions[r].first_key == region->first && regions[r].first_velocity == region->second)
        {
            return r;
        }
    }

    return std::nullopt;
}

std::pair<int, int> IdentityOf(const Region& region)
{
    return {region.first_key, region.first_velocity};
}

// The part of the region at `rect` that a drag from `point` moves: an edge within the band, or else the whole region. A
// region too narrow for both edges and a middle gives its edges less room.
RegionPart PartAt(const ImRect& rect, ImVec2 point)
{
    const float band_x = std::min(Px(kEdgeBand), rect.GetWidth() / 3.0f);
    const float band_y = std::min(Px(kEdgeBand), rect.GetHeight() / 3.0f);
    if (point.x < rect.Min.x + band_x)
    {
        return RegionPart::kLeft;
    }
    if (point.x > rect.Max.x - band_x)
    {
        return RegionPart::kRight;
    }
    if (point.y < rect.Min.y + band_y)
    {
        return RegionPart::kTop;
    }
    if (point.y > rect.Max.y - band_y)
    {
        return RegionPart::kBottom;
    }

    return RegionPart::kBody;
}

// `before` moved by `keys` and `velocities` as a drag of `part` moves it. The region stays inside the map, and an edge
// stops at the other edge. A move of the whole region moves its root key with it.
Region Dragged(Region before, RegionPart part, int keys, int velocities)
{
    const auto clamp = [](int value, int low, int high)
    {
        return static_cast<uint8_t>(std::clamp(value, low, high));
    };
    switch (part)
    {
    case RegionPart::kBody:
        {
            const int key_shift = std::clamp(keys, -before.first_key, kHighest - before.last_key);
            const int velocity_shift = std::clamp(velocities, -before.first_velocity, kHighest - before.last_velocity);
            before.first_key = clamp(before.first_key + key_shift, 0, kHighest);
            before.last_key = clamp(before.last_key + key_shift, 0, kHighest);
            before.first_velocity = clamp(before.first_velocity + velocity_shift, 0, kHighest);
            before.last_velocity = clamp(before.last_velocity + velocity_shift, 0, kHighest);
            before.values.original_key = clamp(before.values.original_key + key_shift, 0, kHighest);
            break;
        }

    case RegionPart::kLeft:
        before.first_key = clamp(before.first_key + keys, 0, before.last_key);
        break;

    case RegionPart::kRight:
        before.last_key = clamp(before.last_key + keys, before.first_key, kHighest);
        break;

    case RegionPart::kTop:
        before.last_velocity = clamp(before.last_velocity + velocities, before.first_velocity, kHighest);
        break;

    case RegionPart::kBottom:
        before.first_velocity = clamp(before.first_velocity + velocities, 0, before.last_velocity);
        break;
    }

    return before;
}

// Gives `edit` the bank with `regions` as the instrument at `program`, or the reason that the bank can't take them.
void EditRegions(const BankView& view, uint32_t program, std::vector<Region> regions, std::string name, BankEdit& edit)
{
    try
    {
        edit.model = edit::WithRegions(view.model, program, std::move(regions));
        edit.name = std::move(name);
        edit.details = ProgramDetails(view.name, program);
    }
    catch (const std::invalid_argument& error)
    {
        edit.refused = error.what();
    }
}

// The programs that hold instruments, in order.
std::vector<uint32_t> ProgramsOf(const sar::BankFile& bank)
{
    std::vector<uint32_t> programs;
    for (std::size_t program = 0; program < bank.instruments.size(); ++program)
    {
        if (bank.instruments[program])
        {
            programs.push_back(static_cast<uint32_t>(program));
        }
    }

    return programs;
}

// The selected instrument's program: the state's while the bank holds an instrument there, or else the first. An edit
// of this frame can select a program that only its new model holds: a new instrument, a copy or a new number. The state
// keeps that program, and the frame shows no instrument. The view shows the new model from the next frame on.
std::optional<uint32_t> SelectedProgram(const BankView& view, const BankEdit& edit, BankEditorState& state)
{
    const auto holds = [&state](const sar::BankFile& bank)
    {
        return state.program && *state.program < bank.instruments.size() && bank.instruments[*state.program];
    };
    if (holds(view.model))
    {
        return state.program;
    }
    if (edit.model && holds(*edit.model))
    {
        return std::nullopt;
    }

    const std::vector<uint32_t> programs = ProgramsOf(view.model);
    state.program = programs.empty() ? std::nullopt : std::optional<uint32_t>(programs.front());
    state.region.reset();

    return state.program;
}

// The rows of `programs` in a child window at `list_min`, `list_size` large.
void DrawSarProgramList(const BankView& view, const std::vector<uint32_t>& programs, BankEditorState& state,
                        ImVec2 list_min, ImVec2 list_size)
{
    // Each instrument's row: its program and its regions. A click selects it. The program's figures share one width,
    // and the regions start in one column. A list that reveals its selected program starts at its top, and the next
    // frame centres the program's row.
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
    if (programs.empty())
    {
        MutedText("No instruments");
        ImGui::TextWrapped("Drag WAV files or waves from the browser here.");
    }
    const auto revealed = reveal && state.program ? std::ranges::find(programs, *state.program) : programs.end();
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
            const uint32_t program = programs[static_cast<std::size_t>(row)];
            const std::size_t regions = edit::RegionsOf(*view.model.instruments[program]).size();
            std::array<char, 16> id{};
            std::snprintf(id.data(), id.size(), "##%u", program);
            const float left = ImGui::GetCursorScreenPos().x;
            if (ImGui::Selectable(id.data(), state.program == program, 0, ImVec2(0.0f, Px(kRowHeight) - Px(6.0f))))
            {
                state.program = program;
                state.region.reset();
            }
            if (revealed != programs.end() && program == *revealed)
            {
                ImGui::SetScrollHereY(0.5f);
            }
            std::array<char, 8> number{};
            std::snprintf(number.data(), number.size(), "%03u", program);
            const std::string count = std::to_string(regions) + (regions == 1 ? " region" : " regions");
            const float top = (ImGui::GetItemRectMin().y + ImGui::GetItemRectMax().y) * 0.5f -
                              TextSize(text, theme::kTextSize, count).y * 0.5f;
            const float baseline = top + Ascent(text, theme::kTextSize);
            DrawText(draw_list, figures, theme::kTextSize, ImVec2(left, baseline - Ascent(figures, theme::kTextSize)),
                     theme::kText, number.data());
            DrawText(draw_list, text, theme::kTextSize, ImVec2(left + column, top), theme::kText, count);
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

// Adds an instrument after the selected program, or at program 0 when `chosen` is false.
void SarAddInstrument(const BankView& view, bool chosen, BankEditorState& state, BankEdit& edit)
{
    try
    {
        AddedInstrument added =
            WithAddedInstrument(view.model, chosen ? *state.program + 1 : 0, view.engine == format::Engine::kWii);
        edit.model = std::move(added.bank);
        edit.name = "Add Instrument";
        edit.details = ProgramDetails(view.name, added.program);
        state.program = added.program;
    }
    catch (const std::invalid_argument& error)
    {
        edit.refused = error.what();
    }
}

// Copies the selected instrument to the first free program after it.
void SarDuplicateInstrument(const BankView& view, BankEditorState& state, BankEdit& edit)
{
    const std::optional<uint32_t> free = FreeProgram(view.model, *state.program + 1);
    if (free)
    {
        edit.model = edit::WithInstrumentCopy(view.model, *state.program, *free);
        edit.name = "Duplicate Instrument";
        edit.details = ProgramDetails(view.name, *free);
        state.program = free;
    }
    else
    {
        edit.refused = "The bank has no free program after the selected one.";
    }
}

// Deletes the selected instrument, one of `programs`.
void SarDeleteInstrument(const BankView& view, const std::vector<uint32_t>& programs, BankEditorState& state,
                         BankEdit& edit)
{
    edit.model = edit::WithoutInstrument(view.model, *state.program);
    edit.name = "Delete Instrument";
    edit.details = ProgramDetails(view.name, *state.program);

    // The selection moves to the next program, or else to the one before it. The GBA and the DS do the same.
    const auto deleted = std::ranges::find(programs, *state.program);
    state.program.reset();
    if (deleted != programs.end() && std::next(deleted) != programs.end())
    {
        state.program = *std::next(deleted);
    }
    else if (deleted != programs.end() && deleted != programs.begin())
    {
        state.program = *std::prev(deleted);
    }
    state.region.reset();
}

// The list of programs at `pos`, `size` large, with the buttons that add, duplicate and delete an instrument and the
// field that renumbers it under the list.
void DrawPrograms(const BankView& view, BankEditorState& state, const ShownItem& bank, bool busy, ImVec2 pos,
                  ImVec2 size, BankEdit& edit)
{
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const std::vector<uint32_t> programs = ProgramsOf(view.model);
    DrawCaps(draw, pos, theme::kTextMuted, "PROGRAMS");
    const float caps = CapsSize("PROGRAMS").y + Px(6.0f);
    const float controls = ImGui::GetFrameHeight() * 2.0f + Px(kGap);
    const ImVec2 list_min(pos.x, pos.y + caps);
    const ImVec2 list_size(size.x, std::max(size.y - caps - controls - Px(kGap), Px(kRowHeight)));
    state.list_min = list_min;
    state.list_max = ImVec2(list_min.x + list_size.x, list_min.y + list_size.y);

    DrawSarProgramList(view, programs, state, list_min, list_size);
    if (const std::optional<BrowserPlace> wave = AcceptWave(); wave && !busy)
    {
        edit.wave = wave;
        edit.drop = BankDrop{std::nullopt, 60, 64};
    }

    // Add makes an instrument at the first free program after the selected one (WithAddedInstrument). Duplicate copies
    // the selected instrument there. Delete takes it out. The field moves it to the program that the user types. The
    // buttons share the list's width by the widths of their labels.
    ImGui::SetCursorScreenPos(ImVec2(pos.x, state.list_max.y + Px(kGap)));
    const std::array<float, 3> width = ProgramButtonWidths(size.x);

    const bool chosen = state.program.has_value();
    ImGui::BeginDisabled(busy);
    if (ImGui::Button("Add", ImVec2(width[0], 0.0f)))
    {
        SarAddInstrument(view, chosen, state, edit);
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!chosen);
    if (ImGui::Button("Duplicate", ImVec2(width[1], 0.0f)) && chosen)
    {
        SarDuplicateInstrument(view, state, edit);
    }
    ImGui::SameLine();
    if (ImGui::Button("Delete", ImVec2(width[2], 0.0f)) && chosen)
    {
        SarDeleteInstrument(view, programs, state, edit);
    }

    // The program's field shows the selected program. Enter moves the instrument to the program that the user typed.
    if (const std::optional<uint32_t> to =
            ProgramNumberField(state, bank, chosen, busy, pos, size, edit::kHighestProgram, edit))
    {
        try
        {
            edit.model = edit::WithInstrumentMoved(view.model, *state.program, *to);
            edit.name = "Renumber Instrument";
            edit.details = ProgramDetails(view.name, *to);
            state.program = *to;
        }
        catch (const std::invalid_argument& error)
        {
            edit.refused = error.what();
        }
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();
}

// Draws the ground of the map.
void DrawMapGround(ImDrawList* draw, const MapGeometry& map)
{
    // The map's ground, with a line at each C and at each quarter of the velocities. Black keys' columns are darker.
    draw->AddRectFilled(map.min, map.max, theme::kDisplay, Px(4.0f));
    for (int key = 0; key < kKeys; ++key)
    {
        if (IsBlack(key))
        {
            draw->AddRectFilled(ImVec2(KeyX(map, key), map.min.y), ImVec2(KeyX(map, key + 1), map.max.y),
                                theme::WithAlpha(theme::kPanel, 0.6f));
        }
        if (key % 12 == 0)
        {
            draw->AddLine(ImVec2(KeyX(map, key), map.min.y), ImVec2(KeyX(map, key), map.max.y), theme::kGridBar,
                          Px(1.0f));
        }
    }
    for (int velocity = 32; velocity < kKeys; velocity += 32)
    {
        draw->AddLine(ImVec2(map.min.x, VelocityY(map, velocity)), ImVec2(map.max.x, VelocityY(map, velocity)),
                      theme::kGridBeat, Px(1.0f));
    }
}

// The region of `shown` under `mouse`, the selected one first.
std::optional<std::size_t> RegionUnder(const MapGeometry& map, const std::vector<Region>& shown,
                                       std::optional<std::size_t> selected, ImVec2 mouse)
{
    if (selected && RectOf(map, shown[*selected]).Contains(mouse))
    {
        return selected;
    }

    for (std::size_t r = 0; r < shown.size(); ++r)
    {
        if (RectOf(map, shown[r]).Contains(mouse))
        {
            return r;
        }
    }

    return std::nullopt;
}

// Selects the region under the mouse at a press, and starts a drag of it.
void StartRegionDrag(const std::vector<Region>& regions, std::optional<std::size_t> under, BankEditorState& state,
                     bool busy, const MapGeometry& map, ImVec2 mouse)
{
    state.region = under ? std::optional<std::pair<int, int>>(IdentityOf(regions[*under])) : std::nullopt;
    if (under && !busy)
    {
        state.dragging = PartAt(RectOf(map, regions[*under]), mouse);
        state.dragged = *under;
        state.drag_from = mouse;
        state.before = regions;
        state.moved = regions;
    }
}

// Moves the dragged region, or one of its edges, with the mouse.
void FollowRegionDrag(BankEditorState& state, const MapGeometry& map, ImVec2 mouse)
{
    const auto keys = static_cast<int>(std::lround((mouse.x - state.drag_from.x) / map.key));
    const auto velocities = static_cast<int>(std::lround((state.drag_from.y - mouse.y) / map.velocity));
    state.moved = state.before;
    state.moved[state.dragged] = Dragged(state.before[state.dragged], *state.dragging, keys, velocities);
}

// Ends a drag, and sets `edit` to the moved regions.
void FinishRegionDrag(const BankView& view, uint32_t program, BankEditorState& state, BankEdit& edit)
{
    if (state.dragged < state.moved.size() && state.moved != state.before)
    {
        const char* name = *state.dragging == RegionPart::kBody ? "Move Region" : "Resize Region";
        state.region = IdentityOf(state.moved[state.dragged]);
        EditRegions(view, program, state.moved, name, edit);
    }
    state.dragging.reset();
}

// Draws `shown`, the regions, each with the name of its wave where it fits. Region `lit` has an outline.
void DrawMapRegions(ImDrawList* draw, const BankView& view, const MapGeometry& map, const std::vector<Region>& shown,
                    std::optional<std::size_t> lit)
{
    ImFont* font = theme::GetFonts().text;
    draw->PushClipRect(map.min, map.max, true);
    for (std::size_t r = 0; r < shown.size(); ++r)
    {
        const ImRect rect = RectOf(map, shown[r]);
        const bool on = lit == r;
        draw->AddRectFilled(rect.Min, rect.Max, theme::WithAlpha(theme::kAccent, on ? 0.55f : 0.28f), Px(2.0f));
        draw->AddRect(rect.Min, rect.Max, on ? theme::kText : theme::WithAlpha(theme::kAccent, 0.9f), Px(2.0f),
                      Px(on ? 1.5f : 1.0f));
        const uint32_t entry = shown[r].values.wave_id_index;
        const std::string name = entry < view.entries.size() ? view.entries[entry] : std::string();
        const std::string fitted = FitText(font, theme::kSmallSize, name, rect.GetWidth() - Px(8.0f));
        if (!fitted.empty() && rect.GetHeight() > Px(18.0f))
        {
            DrawText(draw, font, theme::kSmallSize, ImVec2(rect.Min.x + Px(4.0f), rect.Min.y + Px(3.0f)), theme::kText,
                     fitted);
        }
    }
    draw->PopClipRect();
}

// The map of the instrument's regions at `map`: keys across and velocities up. A click selects a region, and a drag
// moves it or one of its edges. Delete or Backspace deletes the selected region. A wave from the browser dropped on the
// map makes a region, or gives one the wave.
void DrawMap(const BankView& view, uint32_t program, BankEditorState& state, bool busy, const MapGeometry& map,
             BankEdit& edit)
{
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const std::vector<Region> regions = edit::RegionsOf(*view.model.instruments[program]);
    state.map_min = map.min;
    state.map_max = map.max;

    DrawMapGround(draw, map);

    // The mouse's gestures. A press finds the region under the mouse, the selected one first, and the part of it that a
    // drag moves.
    ImGui::SetCursorScreenPos(map.min);
    ImGui::InvisibleButton("##map", ImVec2(map.max.x - map.min.x, map.max.y - map.min.y));
    const ImVec2 mouse = ImGui::GetMousePos();
    const std::vector<Region>& shown = state.dragging ? state.moved : regions;
    const std::optional<std::size_t> selected = PlaceOf(shown, state.region);
    const std::optional<std::size_t> under =
        ImGui::IsItemHovered() || ImGui::IsItemActive() ? RegionUnder(map, shown, selected, mouse) : std::nullopt;
    if (under && !state.dragging && !busy)
    {
        const RegionPart part = PartAt(RectOf(map, shown[*under]), mouse);
        if (part == RegionPart::kLeft || part == RegionPart::kRight)
        {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        }
        else if (part == RegionPart::kTop || part == RegionPart::kBottom)
        {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
        }
    }
    if (ImGui::IsItemActivated())
    {
        StartRegionDrag(regions, under, state, busy, map, mouse);
    }
    if (state.dragging && ImGui::IsItemActive() && state.dragged < state.before.size())
    {
        FollowRegionDrag(state, map, mouse);
    }
    if (state.dragging && ImGui::IsItemDeactivated())
    {
        FinishRegionDrag(view, program, state, edit);
    }

    // Delete or Backspace takes the selected region out while the window has the keys.
    const bool keys = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::GetIO().WantTextInput;
    const bool remove = ImGui::IsKeyPressed(ImGuiKey_Delete, false) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false);
    if (keys && remove && selected && !busy && !state.dragging)
    {
        std::vector<Region> left = regions;
        left.erase(left.begin() + static_cast<std::ptrdiff_t>(*selected));
        EditRegions(view, program, std::move(left), "Delete Region", edit);
        state.region.reset();
    }

    // A wave dropped from the browser lands at the key and the velocity under the mouse.
    if (const std::optional<BrowserPlace> wave = AcceptWave(); wave && !busy)
    {
        edit.wave = wave;
        edit.drop = BankDrop{program, KeyAt(map, mouse.x), VelocityAt(map, mouse.y)};
    }

    // The regions, each with the name of its wave where it fits. The selected region is lit, with an outline.
    const std::optional<std::size_t> lit = PlaceOf(shown, state.region);
    DrawMapRegions(draw, view, map, shown, lit);

    // A tooltip names the key and the velocity under the mouse.
    if (ImGui::IsItemHovered() && !state.dragging)
    {
        const int key = KeyAt(map, mouse.x);
        WrappedTooltip(KeyName(key) + " (key " + std::to_string(key) + "), velocity " +
                       std::to_string(VelocityAt(map, mouse.y)));
    }
}

// A field of a region that holds a byte.
enum class Field
{
    kRootKey,
    kVolume,
    kPan,
    kKeyGroup,
    kAttack,
    kHold,
    kDecay,
    kSustain,
    kRelease,
};

uint8_t& ByteOf(sar::VelocityRegion& values, Field field)
{
    switch (field)
    {
    case Field::kRootKey:
        return values.original_key;
    case Field::kVolume:
        return values.volume;
    case Field::kPan:
        return values.pan;
    case Field::kKeyGroup:
        return values.key_group;
    case Field::kAttack:
        return values.adshr.attack;
    case Field::kHold:
        return values.adshr.hold;
    case Field::kDecay:
        return values.adshr.decay;
    case Field::kSustain:
        return values.adshr.sustain;
    case Field::kRelease:
        break;
    }

    return values.adshr.release;
}

// A field's row: its label, its name in the Undo History, its tooltip, its slider's highest value and the highest that
// the user can type, and the value that a Command-click or a Ctrl-click sets.
struct ByteField
{
    Field field;
    const char* label;
    const char* name;
    const char* tooltip;
    float high;
    float typed_high;
    float reset;
};

const ByteField kRootKeyField = {Field::kRootKey, "Root key", "Root Key", kRootTip, 127.0f, 127.0f, kMiddleC};
const ByteField kVolumeField = {Field::kVolume, "Volume", "Volume", kVolumeTip, 127.0f, 255.0f, kFullVolume};
const ByteField kPanField = {Field::kPan, "Pan", "Pan", kPanTip, 127.0f, 127.0f, kCentre};
const ByteField kKeyGroupField = {Field::kKeyGroup, "Key group", "Key Group", kKeyGroupTip,
                                  kHighestKeyGroup, 255.0f,      0.0f};
const std::array<ByteField, 5> kEnvelopeFields = {{
    {Field::kAttack, "Attack", "Attack", kAttackTip, 127.0f, 127.0f, kFastest},
    {Field::kHold, "Hold", "Hold", kHoldTip, 127.0f, 127.0f, 0.0f},
    {Field::kDecay, "Decay", "Decay", kDecayTip, 127.0f, 127.0f, kFastest},
    {Field::kSustain, "Sustain", "Sustain", kSustainTip, 127.0f, 127.0f, kFastest},
    {Field::kRelease, "Release", "Release", kReleaseTip, 127.0f, 127.0f, kFastest},
}};

// The region that the fields show: the one that the user holds a slider of, or else the selected one.
const Region& Shown(const Region& region, const BankEditorState& state)
{
    return state.held ? *state.held : region;
}

// The row of `field` for `region`, with the value as `format` writes it. The state holds the region as the user drags
// the slider. Sets `*released` once the user lets go of it. With `grey_tip`, the row shows grey and takes no edits, and
// the tip says why.
void ByteRow(const ByteField& field, const Region& region, const std::string& format, BankEditorState& state,
             bool* released, const char* grey_tip = nullptr)
{
    FieldLabel(field.label, grey_tip != nullptr ? grey_tip : field.tooltip, grey_tip != nullptr);
    ImGui::BeginDisabled(grey_tip != nullptr);
    const bool holding = state.held && state.held_name == field.name;
    Region changed = holding ? *state.held : region;
    auto shown = static_cast<float>(ByteOf(changed.values, field.field));
    const SliderRange typed{0.0f, field.typed_high};
    ImGui::PushID(field.label);
    if (ValueSlider("##value", &shown, {0.0f, field.high}, field.reset, format.c_str(),
                    ImVec2(ImGui::GetContentRegionAvail().x, ImGui::GetFrameHeight()), &typed))
    {
        ByteOf(changed.values, field.field) =
            static_cast<uint8_t>(std::lround(std::clamp(shown, 0.0f, field.typed_high)));
        state.held = changed;
        state.held_name = field.name;
    }
    *released = *released || (SliderReleased() && state.held && state.held_name == field.name);
    ImGui::PopID();
    ImGui::EndDisabled();
}

// The row of the tuning, in cents from the region's pitch, the same way.
void TuningRow(const Region& region, BankEditorState& state, bool* released)
{
    FieldLabel("Tuning", kTuningTip);
    const bool holding = state.held && state.held_name == "Tuning";
    Region changed = holding ? *state.held : region;
    const double pitch = std::max(static_cast<double>(changed.values.pitch), 1e-6);
    auto cents = static_cast<float>(1200.0 * std::log2(pitch));
    const SliderRange typed{-kTypedTuning, kTypedTuning};
    if (ValueSlider("##tuning", &cents, {-kTuningRange, kTuningRange}, 0.0f, "%+.0f cents",
                    ImVec2(ImGui::GetContentRegionAvail().x, ImGui::GetFrameHeight()), &typed))
    {
        changed.values.pitch = static_cast<float>(std::exp2(std::round(cents) / 1200.0));
        state.held = changed;
        state.held_name = "Tuning";
    }
    *released = *released || (SliderReleased() && state.held && state.held_name == "Tuning");
}

// The row of the wave of region `place` of `regions`, from a menu of the archive's waves.
void SarWaveRow(const BankView& view, uint32_t program, const std::vector<Region>& regions, std::size_t place,
                BankEditorState& state, BankEdit& edit)
{
    const Region& region = regions[place];
    // The wave, from a menu of the archive's waves. A wave that the table names but the archive lacks shows as the
    // entry's name.
    FieldLabel("Wave", kWaveTip);
    const uint32_t entry = region.values.wave_id_index;
    const auto found =
        entry < view.model.waves.size() ? std::ranges::find(view.waves, view.model.waves[entry]) : view.waves.end();
    int chosen = found != view.waves.end() ? static_cast<int>(found - view.waves.begin()) : -1;
    const std::string missing = entry < view.entries.size() ? view.entries[entry] : "No wave";
    ImGui::SetNextWindowSizeConstraints(ImVec2(Px(280.0f), 0.0f), ImVec2(FLT_MAX, FLT_MAX));
    if (FilteredCombo("##wave", view.wave_names, chosen, chosen < 0 ? missing.c_str() : nullptr, state.filter) &&
        chosen >= 0)
    {
        sar::BankFile bank = view.model;
        std::vector<Region> changed = regions;
        changed[place].values.wave_id_index = edit::WaveEntry(bank, view.waves[static_cast<std::size_t>(chosen)]);
        try
        {
            edit.model = edit::WithRegions(std::move(bank), program, std::move(changed));
            edit.name = "Change Wave";
            edit.details = ProgramDetails(view.name, program);
        }
        catch (const std::invalid_argument& error)
        {
            edit.refused = error.what();
        }
    }
}

// The row of the interpolation of region `place` of `regions`.
void SarInterpolationRow(const BankView& view, uint32_t program, const std::vector<Region>& regions, std::size_t place,
                         BankEdit& edit)
{
    const Region& region = regions[place];
    FieldLabel("Interpolation",
               view.interpolation                    ? kInterpolationTip
               : view.engine == format::Engine::kWii ? kWiiInterpolationTip
                                                     : kSwitchInterpolationTip,
               !view.interpolation);
    const int interpolation = std::min<int>(region.values.interpolation, 2);
    const std::array<std::string, 3> labels = {
        view.engine == format::Engine::k3ds ? kCtrFirstInterpolation : kInterpolations[0], kInterpolations[1],
        kInterpolations[2]};
    std::array<uint8_t, 3> lit{};
    lit[static_cast<std::size_t>(interpolation)] = 1;
    ImGui::BeginDisabled(!view.interpolation);
    const int clicked = SegmentedSwitch("##interpolation", labels, lit,
                                        ImVec2(ImGui::GetContentRegionAvail().x, ImGui::GetFrameHeight()));
    ImGui::EndDisabled();
    if (clicked >= 0 && clicked != interpolation && view.interpolation)
    {
        std::vector<Region> changed = regions;
        changed[place].values.interpolation = static_cast<uint8_t>(clicked);
        EditRegions(view, program, std::move(changed), "Change Interpolation", edit);
    }
}

// The row of the note-off of region `place` of `regions`.
void SarNoteOffRow(const BankView& view, uint32_t program, const std::vector<Region>& regions, std::size_t place,
                   BankEdit& edit)
{
    const Region& region = regions[place];
    FieldLabel("Note-off", view.hold_and_groups ? kNoteOffTip : kOldNoteOffTip, !view.hold_and_groups);
    const std::array<std::string, 2> kChoices = {"Plays", "Ignored"};
    std::array<uint8_t, 2> on{};
    on[region.values.ignore_note_off ? 1 : 0] = 1;
    ImGui::BeginDisabled(!view.hold_and_groups);
    const int note_off =
        SegmentedSwitch("##note-off", kChoices, on, ImVec2(ImGui::GetContentRegionAvail().x, ImGui::GetFrameHeight()));
    ImGui::EndDisabled();
    if (note_off >= 0 && (note_off == 1) != region.values.ignore_note_off && view.hold_and_groups)
    {
        std::vector<Region> changed = regions;
        changed[place].values.ignore_note_off = note_off == 1;
        EditRegions(view, program, std::move(changed), "Change Note-Off", edit);
    }
}

// The fields of the selected region, in two tables side by side: what it plays on the left, and its envelope, its
// interpolation and its note-off on the right. A change of a slider ends once the user lets go of it.
void DrawFields(const BankView& view, uint32_t program, const std::vector<Region>& regions, std::size_t place,
                BankEditorState& state, bool busy, ImVec2 pos, ImVec2 size, BankEdit& edit)
{
    const Region& region = regions[place];
    const Region& current = Shown(region, state);
    bool released = false;
    const float half = (size.x - Px(kGap)) * 0.5f;
    ImGui::BeginDisabled(busy);
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(ImGui::GetStyle().CellPadding.x, Px(kFieldPadding)));
    if (BeginFieldTable("##playing", pos, half))
    {
        // The keys and the velocities. The map's drags change them.
        FieldLabel("Keys", kKeysTip);
        ImGui::Text("%s to %s (%d to %d)", KeyName(current.first_key).c_str(), KeyName(current.last_key).c_str(),
                    current.first_key, current.last_key);
        FieldLabel("Velocities", kVelocitiesTip);
        ImGui::Text("%d to %d", current.first_velocity, current.last_velocity);

        SarWaveRow(view, program, regions, place, state, edit);

        // The root key and the tuning, the volume and the pan, and the key group.
        ByteRow(kRootKeyField, region, "%.0f  (" + KeyName(current.values.original_key) + ")", state, &released);
        TuningRow(region, state, &released);
        ByteRow(kVolumeField, region, "%.0f  (" + VolumeLevel(current.values.volume) + ")", state, &released);
        ByteRow(kPanField, region, "%.0f  (" + PanOf(current.values.pan) + ")", state, &released);
        ByteRow(kKeyGroupField, region, current.values.key_group == 0 ? "%.0f  (none)" : "%.0f", state, &released,
                view.hold_and_groups ? nullptr : kOldKeyGroupTip);
        ImGui::EndTable();
    }

    // The envelope, the interpolation and the note-off. The Switch and the Wii read no interpolation, and its field
    // shows grey. Before March 2008, NW4R SND reads no hold and no note-off, and their fields show grey on the Wii.
    if (BeginFieldTable("##envelope", ImVec2(pos.x + half + Px(kGap), pos.y), half))
    {
        for (const ByteField& field : kEnvelopeFields)
        {
            const bool old = field.field == Field::kHold && !view.hold_and_groups;
            ByteRow(field, region, "%.0f", state, &released, old ? kOldHoldTip : nullptr);
        }

        SarInterpolationRow(view, program, regions, place, edit);
        SarNoteOffRow(view, program, regions, place, edit);
        ImGui::EndTable();
    }
    ImGui::PopStyleVar();
    ImGui::EndDisabled();

    // A slider that the user let go of makes its change, unless it ended on its old value. An edit of the regions lays
    // the bank out from its model. An edit that changes nothing could still change the bank's bytes. While busy, the
    // slider's change is dropped, and the fields show the selected region again.
    if (released && state.held)
    {
        if (!busy && *state.held != regions[place])
        {
            std::vector<Region> changed = regions;
            changed[place] = *state.held;
            EditRegions(view, program, std::move(changed), "Change " + state.held_name, edit);
        }
        state.held.reset();
    }
}

// The facts at `pos`, `size` large: what else plays the bank, the groups that lack its waves, and the sizes. The lines
// wrap at the window's edge, and scroll where they don't fit. They start at their `top`.
void DrawFacts(const BankView& view, ImVec2 pos, ImVec2 size, bool top)
{
    BeginFacts(pos, size, top);
    std::string sounds;
    for (std::size_t s = 0; s < view.sounds.size() && s < 4; ++s)
    {
        sounds += (s == 0 ? "" : ", ") + view.sounds[s];
    }
    if (view.sounds.size() > 4)
    {
        sounds += " and " + std::to_string(view.sounds.size() - 4) + " more";
    }
    const ImVec4 kMuted = ImGui::ColorConvertU32ToFloat4(theme::kTextMuted);
    if (view.sounds.empty())
    {
        ImGui::TextColored(kMuted, "No sequence sound uses this bank.");
    }
    else
    {
        ImGui::TextColored(kMuted, "Edits affect %zu sequence sound%s: %s.", view.sounds.size(),
                           view.sounds.size() == 1 ? "" : "s", sounds.c_str());
        if (ImGui::IsItemHovered() && view.sounds.size() > 4)
        {
            std::string all;
            for (const std::string& sound : view.sounds)
            {
                all += (all.empty() ? "" : "\n") + sound;
            }
            WrappedTooltip(all);
        }
    }
    for (const std::string& group : view.lacking)
    {
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme::kCycle),
                           "%s loads the bank but is missing one of its wave archives. In the game, those waves play "
                           "only after another group loads them.",
                           group.c_str());
    }
    for (const std::string& line : view.sizes)
    {
        ImGui::TextColored(kMuted, "%s", line.c_str());
    }
    ImGui::PopTextWrapPos();
    ImGui::EndChild();
}

} // namespace

BankEdit DrawBankEditor(const BankView& view, BankEditorState& state, const ShownItem& bank, bool busy, bool* open)
{
    // A window over the middle of the main window, in the style of the effects' windows. The user can make it larger.
    const std::string title = "Bank Editor - " + view.name + "###BankEditor";
    BankEdit edit;
    if (!BeginBankEditorWindow(title, state, open))
    {
        return edit;
    }

    // The parts: the list of programs on the left, then the map, the keyboard and the fields, and the facts across the
    // bottom.
    const BankEditorLayout layout = BankLayoutOf(kFieldRows, "MAP");
    const bool top = state.reveal;
    DrawPrograms(view, state, bank, busy, layout.origin,
                 ImVec2(Px(kListWidth), layout.facts_top - layout.origin.y - Px(kGap)), edit);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    DrawListDivider(draw, layout);

    // The map of the selected instrument, or a card that says how to make one.
    const std::optional<uint32_t> program = SelectedProgram(view, edit, state);
    const MapGeometry map = GeometryOf(ImVec2(layout.right, layout.rows_top),
                                       ImVec2(layout.right + layout.right_width, layout.keyboard_top - Px(2.0f)));
    std::optional<Region> selected;
    if (program)
    {
        const std::string caps = "MAP OF PROGRAM " + std::to_string(*program);
        DrawCaps(draw, ImVec2(layout.right, layout.origin.y), theme::kTextMuted, caps);
        DrawMap(view, *program, state, busy, map, edit);
        const std::vector<Region> regions = edit::RegionsOf(*view.model.instruments[*program]);
        if (const std::optional<std::size_t> place = PlaceOf(regions, state.region))
        {
            selected = regions[*place];
            PushSliderItem(TypedItemOf(bank, *program, *place));
            DrawFields(view, *program, regions, *place, state, busy, ImVec2(layout.right, layout.fields_top),
                       ImVec2(layout.right_width, layout.fields_height), edit);
            PopSliderItem();
        }
        else
        {
            state.held.reset();
            EmptyState(draw, ImVec2(layout.right, layout.fields_top),
                       ImVec2(layout.right + layout.right_width, layout.fields_top + layout.fields_height),
                       "No region selected",
                       regions.empty() ? "Drag WAV files or waves from the browser onto the key map to create regions."
                                       : "Click a region on the key map to edit it.");
        }
    }
    else
    {
        state.map_min = ImVec2();
        state.map_max = ImVec2();
        EmptyState(draw, map.min, map.max, "No instruments",
                   "Drag WAV files or waves from the browser onto the program list, or add an instrument.");
    }
    DrawKeyboard(selected ? std::optional(std::pair(selected->first_key, selected->last_key)) : std::nullopt,
                 selected ? std::optional<int>(selected->values.original_key) : std::nullopt, state,
                 ImVec2(layout.right, layout.keyboard_top),
                 ImVec2(layout.right + layout.right_width, layout.keyboard_top + Px(kKeyboardHeight)));
    if (!program)
    {
        state.sounding = -1;
    }

    // The facts across the bottom.
    DrawFactsDivider(draw, layout);
    DrawFacts(view, ImVec2(layout.origin.x, layout.facts_top),
              ImVec2(layout.room.x, layout.origin.y + layout.room.y - layout.facts_top), top);
    ImGui::End();

    return edit;
}

} // namespace hokuto::app
