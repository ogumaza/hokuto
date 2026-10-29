// SPDX-License-Identifier: MIT

#include "app/bank_editor.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/sar/bank_file.h"
#include "ntdlib/sar/formats.h"

#include "app/key_edits.h"
#include "app/theme.h"
#include "app/widgets.h"
#include "edit/banks.h"

namespace hokuto::app
{
namespace
{

namespace sar = ntdlib::sar;
using edit::Region;
using theme::Px;

// The keys and the velocities that a region covers run from 0 to 127.
constexpr int kKeys = 128;
constexpr int kHighest = 127;

// The window's sizes, before the display's scale: its first size and its least, the list of programs, a row of the
// list, the keyboard, the facts at the bottom, the labels of the fields, the room above and below a field's control,
// the gap between the parts, and the band inside an edge of a region that a drag takes. The band is as wide as the
// arrangement's and the Key Editor's grabs of an edge. The fields under the map take up to kFieldRows rows in each of
// their two tables.
constexpr float kWindowWidth = 1040.0f;
constexpr float kWindowHeight = 760.0f;
constexpr float kLeastWidth = 760.0f;
constexpr float kLeastHeight = 600.0f;
constexpr float kListWidth = 240.0f;
constexpr float kRowHeight = 26.0f;
constexpr float kKeyboardHeight = 54.0f;
constexpr float kFactsHeight = 84.0f;
constexpr float kLabelWidth = 96.0f;
constexpr float kFieldPadding = 2.0f;
constexpr int kFieldRows = 8;
constexpr float kGap = 10.0f;
constexpr float kEdgeBand = 6.0f;

// The values that a Command-click or a Ctrl-click on a field's slider sets: NintendoWare's defaults.
constexpr float kFullVolume = 127.0f;
constexpr float kCentre = 64.0f;
constexpr float kMiddleC = 60.0f;
constexpr float kFastest = 127.0f;

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
const char* const kKeysTip =
    "The keys that the region covers. A drag of its left or right edge on the map changes them.";
const char* const kVelocitiesTip =
    "The velocities that the region covers. A drag of its top or bottom edge on the map changes them.";
const char* const kWaveTip =
    "The wave that the region plays: a wave of one of the archive's wave archives. A game plays it only once a group "
    "has loaded the wave archive.";
const char* const kRootTip =
    "The key at which the region plays its wave at the wave's sample rate. Each key above or below plays a semitone "
    "higher or lower.";
const char* const kTuningTip = "The region's tuning, in cents: hundredths of a semitone.";
const char* const kVolumeTip =
    "The region's volume, from 0 to 255. At 127 the region plays at full level. A higher value plays louder, up to a "
    "limit that the console sets.";
const char* const kPanTip = "The region's pan, from 0 for left to 127 for right. 64 is the centre.";
const char* const kKeyGroupTip =
    "A note of a key group from 1 to 15 cuts the other notes of its group. An open hi-hat and a closed one cut each "
    "other this way. Group 0 cuts nothing.";
const char* const kAttackTip = "How fast the note rises to full level, from 0 to 127. At 127 it starts at full level.";
const char* const kHoldTip =
    "How long the note stays at full level after its attack, from 0 to 127. The time grows with the square of the "
    "value, up to about 4 s.";
const char* const kDecayTip = "How fast the note falls to its sustain level, from 0 to 127. 127 is the fastest.";
const char* const kSustainTip =
    "The level that the note holds while its key is down, from 0 to 127. At 127 the note holds at full level.";
const char* const kReleaseTip = "How fast the note fades once its key is let go, from 0 to 127. 127 is the fastest.";
const char* const kInterpolationTip = "How the console's mixer reads the wave between its samples.";
const char* const kSwitchInterpolationTip =
    "The Switch's audio renderer reads no interpolation. Every region plays the same, whatever its value.";
const char* const kWiiInterpolationTip =
    "A bank of the Wii holds no interpolation. The Wii's mixer reads every wave the same way.";

// Why the file's build of NW4R SND reads none of a region's hold, key group and note-off. Its builds from March 2008 on
// read them.
const char* const kOldHoldTip = "The file's build of NW4R SND reads no hold. Its builds from March 2008 on read it.";
const char* const kOldKeyGroupTip =
    "The file's build of NW4R SND reads no key group. Its builds from March 2008 on read it.";
const char* const kOldNoteOffTip =
    "The file's build of NW4R SND reads no note-off. Its builds from March 2008 on read it.";
const char* const kNoteOffTip =
    "Whether a note-off releases the note. Ignored, the note plays its wave to the end. Drums often do.";

bool IsBlack(int key)
{
    const int note = key % 12;
    return note == 1 || note == 3 || note == 6 || note == 8 || note == 10;
}

// Where the map lies, and the width of a key's column and the height of a velocity's row. Velocity 127 lies at the top.
struct MapGeometry
{
    ImVec2 min;
    ImVec2 max;
    float key = 0.0f;
    float velocity = 0.0f;
};

MapGeometry GeometryOf(ImVec2 min, ImVec2 max)
{
    return {min, max, (max.x - min.x) / static_cast<float>(kKeys), (max.y - min.y) / static_cast<float>(kKeys)};
}

// The left edge of `key`'s column, and the bottom edge of `velocity`'s row.
float KeyX(const MapGeometry& map, int key)
{
    return map.min.x + static_cast<float>(key) * map.key;
}

float VelocityY(const MapGeometry& map, int velocity)
{
    return map.max.y - static_cast<float>(velocity) * map.velocity;
}

int KeyAt(const MapGeometry& map, float x)
{
    return std::clamp(static_cast<int>(std::floor((x - map.min.x) / map.key)), 0, kHighest);
}

int VelocityAt(const MapGeometry& map, float y)
{
    return std::clamp(static_cast<int>(std::floor((map.max.y - y) / map.velocity)), 0, kHighest);
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

// The details of an action on the instrument at `program` of `view`'s bank.
std::string ProgramDetails(const BankView& view, uint32_t program)
{
    return view.name + ", program " + std::to_string(program);
}

// Gives `edit` the bank with `regions` as the instrument at `program`, or the reason that the bank can't take them.
void EditRegions(const BankView& view, uint32_t program, std::vector<Region> regions, std::string name, BankEdit& edit)
{
    try
    {
        edit.model = edit::WithRegions(view.model, program, std::move(regions));
        edit.name = std::move(name);
        edit.details = ProgramDetails(view, program);
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

// A wave dropped from the browser on the last item: its place, or nothing.
std::optional<BrowserPlace> AcceptWave()
{
    std::optional<BrowserPlace> place;
    if (ImGui::BeginDragDropTarget())
    {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kWavePayload))
        {
            if (payload->DataSize == static_cast<int>(sizeof(BrowserPlace)))
            {
                BrowserPlace dropped;
                std::copy_n(static_cast<const char*>(payload->Data), sizeof(BrowserPlace),
                            reinterpret_cast<char*>(&dropped));
                place = dropped;
            }
        }
        ImGui::EndDragDropTarget();
    }

    return place;
}

// The list of programs at `pos`, `size` large, with the buttons that add, duplicate and delete an instrument and the
// field that renumbers it under the list.
void DrawPrograms(const BankView& view, BankEditorState& state, bool busy, ImVec2 pos, ImVec2 size, BankEdit& edit)
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
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme::kTextMuted), "No instruments");
        ImGui::TextWrapped("Drop WAV files or waves from the browser here.");
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
    if (const std::optional<BrowserPlace> wave = AcceptWave(); wave && !busy)
    {
        edit.wave = wave;
        edit.drop = BankDrop{std::nullopt, 60, 64};
    }

    // Add makes an empty instrument at the first free program after the selected one. Duplicate copies the selected
    // instrument there. Delete takes it out. The field moves it to the program that the user types. The buttons share
    // the list's width by the widths of their labels.
    ImGui::SetCursorScreenPos(ImVec2(pos.x, state.list_max.y + Px(kGap)));
    const std::array<float, 3> labels = {ImGui::CalcTextSize("Add").x, ImGui::CalcTextSize("Duplicate").x,
                                         ImGui::CalcTextSize("Delete").x};
    const float padding = ImGui::GetStyle().FramePadding.x * 2.0f;
    const float room = size.x - ImGui::GetStyle().ItemSpacing.x * 2.0f - padding * 3.0f;
    const float scale = room / (labels[0] + labels[1] + labels[2]);
    const auto width = [&](std::size_t button)
    {
        return std::max(labels[button] * scale + padding, 0.0f);
    };

    const bool chosen = state.program.has_value();
    ImGui::BeginDisabled(busy);
    if (ImGui::Button("Add", ImVec2(width(0), 0.0f)))
    {
        const std::optional<uint32_t> free = FreeProgram(view.model, chosen ? *state.program + 1 : 0);
        if (free)
        {
            edit.model = edit::WithNewInstrument(view.model, *free, std::nullopt);
            edit.name = "Add Instrument";
            edit.details = ProgramDetails(view, *free);
            state.program = free;
        }
        else
        {
            edit.refused = "The bank has no free program after the selected one.";
        }
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!chosen);
    if (ImGui::Button("Duplicate", ImVec2(width(1), 0.0f)) && chosen)
    {
        const std::optional<uint32_t> free = FreeProgram(view.model, *state.program + 1);
        if (free)
        {
            edit.model = edit::WithInstrumentCopy(view.model, *state.program, *free);
            edit.name = "Duplicate Instrument";
            edit.details = ProgramDetails(view, *free);
            state.program = free;
        }
        else
        {
            edit.refused = "The bank has no free program after the selected one.";
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Delete", ImVec2(width(2), 0.0f)) && chosen)
    {
        edit.model = edit::WithoutInstrument(view.model, *state.program);
        edit.name = "Delete Instrument";
        edit.details = ProgramDetails(view, *state.program);
        state.program.reset();
    }

    // The program's field shows the selected program. Enter moves the instrument to the program that the user typed.
    ImGui::SetCursorScreenPos(ImVec2(pos.x, ImGui::GetCursorScreenPos().y));
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Program");
    ImGui::SameLine();
    if (!state.typing_program)
    {
        state.typed_program.fill('\0');
        if (chosen)
        {
            std::snprintf(state.typed_program.data(), state.typed_program.size(), "%u", *state.program);
        }
    }
    ImGui::SetNextItemWidth(pos.x + size.x - ImGui::GetCursorScreenPos().x);
    const bool entered = ImGui::InputText("##program", state.typed_program.data(), state.typed_program.size(),
                                          ImGuiInputTextFlags_EnterReturnsTrue);
    state.typing_program = ImGui::IsItemActive();
    if (entered && chosen)
    {
        // The field takes any text, and refuses what isn't a program with a message.
        state.typing_program = false;
        const char* typed = state.typed_program.data();
        char* end = nullptr;
        const unsigned long to = std::strtoul(typed, &end, 10);
        const bool digits = typed[0] >= '0' && typed[0] <= '9';
        if (!digits || end == typed || *end != '\0' || to > edit::kHighestProgram)
        {
            edit.refused = "A program goes from 0 to " + std::to_string(edit::kHighestProgram) + ".";
        }
        else if (to != *state.program)
        {
            try
            {
                edit.model = edit::WithInstrumentMoved(view.model, *state.program, static_cast<uint32_t>(to));
                edit.name = "Renumber Instrument";
                edit.details = ProgramDetails(view, static_cast<uint32_t>(to));
                state.program = static_cast<uint32_t>(to);
            }
            catch (const std::invalid_argument& error)
            {
                edit.refused = error.what();
            }
        }
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();
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

    // The mouse's gestures. A press finds the region under the mouse, the selected one first, and the part of it that a
    // drag moves.
    ImGui::SetCursorScreenPos(map.min);
    ImGui::InvisibleButton("##map", ImVec2(map.max.x - map.min.x, map.max.y - map.min.y));
    const ImVec2 mouse = ImGui::GetMousePos();
    const std::vector<Region>& shown = state.dragging ? state.moved : regions;
    const std::optional<std::size_t> selected = PlaceOf(shown, state.region);
    const auto hit = [&]() -> std::optional<std::size_t>
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
    };
    const std::optional<std::size_t> under = ImGui::IsItemHovered() || ImGui::IsItemActive() ? hit() : std::nullopt;
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
    if (state.dragging && ImGui::IsItemActive() && state.dragged < state.before.size())
    {
        const auto keys = static_cast<int>(std::lround((mouse.x - state.drag_from.x) / map.key));
        const auto velocities = static_cast<int>(std::lround((state.drag_from.y - mouse.y) / map.velocity));
        state.moved = state.before;
        state.moved[state.dragged] = Dragged(state.before[state.dragged], *state.dragging, keys, velocities);
    }
    if (state.dragging && ImGui::IsItemDeactivated())
    {
        if (state.dragged < state.moved.size() && state.moved != state.before)
        {
            const char* name = *state.dragging == RegionPart::kBody ? "Move Region" : "Resize Region";
            state.region = IdentityOf(state.moved[state.dragged]);
            EditRegions(view, program, state.moved, name, edit);
        }
        state.dragging.reset();
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
    ImFont* font = theme::GetFonts().text;
    draw->PushClipRect(map.min, map.max, true);
    const std::optional<std::size_t> lit = PlaceOf(shown, state.region);
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

    // A tooltip names the key and the velocity under the mouse.
    if (ImGui::IsItemHovered() && !state.dragging)
    {
        const int key = KeyAt(map, mouse.x);
        WrappedTooltip(KeyName(key) + " (key " + std::to_string(key) + "), velocity " +
                       std::to_string(VelocityAt(map, mouse.y)));
    }
}

// The keyboard under the map, one key in each of the map's columns. A press sounds the key, and a drag across the keys
// sounds each in turn. A press near a key's bottom sounds it louder. The selected region's keys are tinted, and its
// root key marked.
void DrawKeyboard(const std::optional<Region>& region, BankEditorState& state, ImVec2 min, ImVec2 max)
{
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const MapGeometry keys = GeometryOf(min, max);
    ImGui::SetCursorScreenPos(min);
    ImGui::InvisibleButton("##keyboard", ImVec2(max.x - min.x, max.y - min.y));
    if (ImGui::IsItemActive())
    {
        const ImVec2 mouse = ImGui::GetMousePos();
        const float depth = std::clamp((mouse.y - min.y) / (max.y - min.y), 0.0f, 1.0f);
        state.sounding = KeyAt(keys, mouse.x);
        state.velocity = std::clamp(static_cast<int>(std::lround(30.0f + depth * 97.0f)), 1, kHighest);
    }
    else
    {
        state.sounding = -1;
    }

    // Each key's colour: tinted where the selected region covers it, and lit while it sounds.
    const ImU32 kWhite = theme::Mix(theme::kText, theme::kPanel, 0.2f);
    const ImU32 kBlack = theme::kWindow;
    const ImU32 kLine = theme::WithAlpha(kBlack, 0.6f);
    const auto colour_of = [&](int key)
    {
        ImU32 colour = IsBlack(key) ? kBlack : kWhite;
        if (region && key >= region->first_key && key <= region->last_key)
        {
            colour = theme::Mix(colour, theme::kAccent, 0.45f);
        }

        return key == state.sounding ? theme::kPlay : colour;
    };

    // The keys line up with the map's columns. A white key takes its column, and below the black keys the halves of
    // their columns beside it. A piano's keys lie the same way. Lines part the white keys, and the black keys lie over
    // them.
    const float black_bottom = min.y + (max.y - min.y) * 0.62f;
    for (int key = 0; key < kKeys; ++key)
    {
        const float left = KeyX(keys, key);
        const float right = left + keys.key;
        if (!IsBlack(key))
        {
            draw->AddRectFilled(ImVec2(left, min.y), ImVec2(right, max.y), colour_of(key));
            if (key % 12 == 0 || key % 12 == 5)
            {
                draw->AddLine(ImVec2(left, min.y), ImVec2(left, max.y), kLine, Px(1.0f));
            }
            continue;
        }

        const float middle = left + keys.key * 0.5f;
        draw->AddRectFilled(ImVec2(left, black_bottom), ImVec2(middle, max.y), colour_of(key - 1));
        draw->AddRectFilled(ImVec2(middle, black_bottom), ImVec2(right, max.y), colour_of(key + 1));
        draw->AddLine(ImVec2(middle, black_bottom), ImVec2(middle, max.y), kLine, Px(1.0f));
        draw->AddRectFilled(ImVec2(left, min.y), ImVec2(right, black_bottom), colour_of(key));
    }

    // Each C has its name, over the keys after it.
    ImFont* font = theme::GetFonts().text;
    for (int key = 0; key < kKeys; key += 12)
    {
        const std::string name = KeyName(key);
        const ImVec2 size = TextSize(font, theme::kSmallSize, name);
        DrawText(draw, font, theme::kSmallSize, ImVec2(KeyX(keys, key) + Px(2.0f), max.y - size.y - Px(2.0f)),
                 theme::TextOn(colour_of(key)), name);
    }
    if (region)
    {
        const float centre = KeyX(keys, region->values.original_key) + keys.key * 0.5f;
        draw->AddCircleFilled(ImVec2(centre, min.y + Px(6.0f)), std::max(Px(2.5f), keys.key * 0.3f), theme::kCycle);
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

// A row of the fields' table: its label with a tooltip, and the cursor in the value's cell. A grey label marks a field
// that the console doesn't read. Its value takes no input, and the tooltip shows over the value too.
void FieldLabel(const char* label, const char* tooltip, bool grey = false)
{
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(grey ? theme::kTextDim : theme::kTextMuted), "%s", label);
    ItemTooltip(tooltip);
    ImGui::TableNextColumn();
    if (grey)
    {
        const ImVec2 cell = ImGui::GetCursorScreenPos();
        RectTooltip(cell, ImVec2(cell.x + ImGui::GetContentRegionAvail().x, cell.y + ImGui::GetFrameHeight()), tooltip);
    }
    ImGui::SetNextItemWidth(-FLT_MIN);
}

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
    *released = *released || (ImGui::IsItemDeactivatedAfterEdit() && state.held && state.held_name == field.name);
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
    *released = *released || (ImGui::IsItemDeactivatedAfterEdit() && state.held && state.held_name == "Tuning");
}

// The text of a pan from 0 to 127 as the mixer writes a pan.
std::string PanOf(int pan)
{
    const float side = pan < kCentre ? kCentre : static_cast<float>(kHighest) - kCentre;
    return PanText((static_cast<float>(pan) - kCentre) / side);
}

// The level of a region's volume, in dB as the mixer writes it.
std::string VolumeLevelOf(int volume)
{
    return LevelText(static_cast<float>(20.0 * std::log10(static_cast<double>(volume) / kFullVolume))) + " dB";
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
    ImGui::SetCursorScreenPos(pos);
    if (ImGui::BeginTable("##playing", 2, ImGuiTableFlags_None, ImVec2(half, 0.0f)))
    {
        ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthFixed, Px(kLabelWidth));
        ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch);

        // The keys and the velocities. The map's drags change them.
        FieldLabel("Keys", kKeysTip);
        ImGui::Text("%s to %s (%d to %d)", KeyName(current.first_key).c_str(), KeyName(current.last_key).c_str(),
                    current.first_key, current.last_key);
        FieldLabel("Velocities", kVelocitiesTip);
        ImGui::Text("%d to %d", current.first_velocity, current.last_velocity);

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
                edit.details = ProgramDetails(view, program);
            }
            catch (const std::invalid_argument& error)
            {
                edit.refused = error.what();
            }
        }

        // The root key and the tuning, the volume and the pan, and the key group.
        ByteRow(kRootKeyField, region, "%.0f  (" + KeyName(current.values.original_key) + ")", state, &released);
        TuningRow(region, state, &released);
        ByteRow(kVolumeField, region, "%.0f  (" + VolumeLevelOf(current.values.volume) + ")", state, &released);
        ByteRow(kPanField, region, "%.0f  (" + PanOf(current.values.pan) + ")", state, &released);
        ByteRow(kKeyGroupField, region, current.values.key_group == 0 ? "%.0f  (none)" : "%.0f", state, &released,
                view.hold_and_groups ? nullptr : kOldKeyGroupTip);
        ImGui::EndTable();
    }

    // The envelope, the interpolation and the note-off. The Switch and the Wii read no interpolation, and its field
    // shows grey. Before March 2008, NW4R SND reads no hold and no note-off, and their fields show grey on the Wii.
    ImGui::SetCursorScreenPos(ImVec2(pos.x + half + Px(kGap), pos.y));
    if (ImGui::BeginTable("##envelope", 2, ImGuiTableFlags_None, ImVec2(half, 0.0f)))
    {
        ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthFixed, Px(kLabelWidth));
        ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch);
        for (const ByteField& field : kEnvelopeFields)
        {
            const bool old = field.field == Field::kHold && !view.hold_and_groups;
            ByteRow(field, region, "%.0f", state, &released, old ? kOldHoldTip : nullptr);
        }

        FieldLabel("Interpolation",
                   view.interpolation ? kInterpolationTip
                   : view.wii         ? kWiiInterpolationTip
                                      : kSwitchInterpolationTip,
                   !view.interpolation);
        const int interpolation = std::min<int>(region.values.interpolation, 2);
        const std::array<std::string, 3> labels = {view.ctr ? kCtrFirstInterpolation : kInterpolations[0],
                                                   kInterpolations[1], kInterpolations[2]};
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

        FieldLabel("Note-off", view.hold_and_groups ? kNoteOffTip : kOldNoteOffTip, !view.hold_and_groups);
        const std::array<std::string, 2> kChoices = {"Plays", "Ignored"};
        std::array<uint8_t, 2> on{};
        on[region.values.ignore_note_off ? 1 : 0] = 1;
        ImGui::BeginDisabled(!view.hold_and_groups);
        const int note_off = SegmentedSwitch("##note-off", kChoices, on,
                                             ImVec2(ImGui::GetContentRegionAvail().x, ImGui::GetFrameHeight()));
        ImGui::EndDisabled();
        if (note_off >= 0 && (note_off == 1) != region.values.ignore_note_off && view.hold_and_groups)
        {
            std::vector<Region> changed = regions;
            changed[place].values.ignore_note_off = note_off == 1;
            EditRegions(view, program, std::move(changed), "Change Note-Off", edit);
        }
        ImGui::EndTable();
    }
    ImGui::PopStyleVar();
    ImGui::EndDisabled();

    // A slider that the user let go of makes its change.
    if (released && state.held && !busy)
    {
        std::vector<Region> changed = regions;
        changed[place] = *state.held;
        EditRegions(view, program, std::move(changed), "Change " + state.held_name, edit);
        state.held.reset();
    }
}

// The facts at `pos`, `size` large: what else plays the bank, the groups that lack its waves, and the sizes. The lines
// wrap at the window's edge, and scroll where they don't fit. They start at their `top`.
void DrawFacts(const BankView& view, ImVec2 pos, ImVec2 size, bool top)
{
    ImGui::SetCursorScreenPos(pos);
    if (top)
    {
        ImGui::SetNextWindowScroll(ImVec2(0.0f, 0.0f));
    }
    ImGui::BeginChild("##facts", size);
    ImGui::PushTextWrapPos(0.0f);
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
        ImGui::TextColored(kMuted, "No sequence sound has the bank in a slot.");
    }
    else
    {
        ImGui::TextColored(kMuted, "An edit changes %zu sequence sound%s: %s.", view.sounds.size(),
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
                           "%s loads the bank but lacks a wave archive that its waves name. A game that loads the "
                           "group plays those waves only once another group has loaded them.",
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

bool OverBankEditor(const BankEditorState& state, ImVec2 point)
{
    return ImRect(state.window_min, state.window_max).Contains(point);
}

std::optional<BankDrop> DropAt(const BankEditorState& state, ImVec2 point)
{
    const ImRect list(state.list_min, state.list_max);
    if (list.Contains(point))
    {
        return BankDrop{std::nullopt, 60, 64};
    }

    const ImRect map(state.map_min, state.map_max);
    if (!map.Contains(point) || !state.program)
    {
        return std::nullopt;
    }

    const MapGeometry geometry = GeometryOf(state.map_min, state.map_max);
    return BankDrop{state.program, KeyAt(geometry, point.x), VelocityAt(geometry, point.y)};
}

BankEdit DrawBankEditor(const BankView& view, BankEditorState& state, bool busy, bool* open)
{
    // A window over the middle of the main window, in the style of the effects' windows. The user can make it larger.
    const std::string title = "Bank Editor - " + view.name + "###BankEditor";
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(Px(kWindowWidth), Px(kWindowHeight)), ImGuiCond_Appearing);
    ImGui::SetNextWindowSizeConstraints(ImVec2(Px(kLeastWidth), Px(kLeastHeight)), ImVec2(FLT_MAX, FLT_MAX));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, Px(theme::kDialogPadding));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::ColorConvertU32ToFloat4(theme::kRaised));
    ImGui::PushStyleColor(ImGuiCol_Border, ImGui::ColorConvertU32ToFloat4(theme::kBorderStrong));
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking |
                                        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar |
                                        ImGuiWindowFlags_NoScrollWithMouse;
    const bool visible = ImGui::Begin(title.c_str(), open, kFlags);
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar();
    BankEdit edit;
    if (!visible)
    {
        state.sounding = -1;
        state.window_min = ImVec2();
        state.window_max = ImVec2();
        ImGui::End();
        return edit;
    }
    state.window_min = ImGui::GetWindowPos();
    state.window_max =
        ImVec2(state.window_min.x + ImGui::GetWindowWidth(), state.window_min.y + ImGui::GetWindowHeight());

    // The parts: the list of programs on the left, then the map, the keyboard and the fields, and the facts across the
    // bottom.
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 room = ImGui::GetContentRegionAvail();
    const float facts_top = origin.y + room.y - Px(kFactsHeight);
    const float right = origin.x + Px(kListWidth) + Px(kGap) * 2.0f;
    const float right_width = origin.x + room.x - right;
    const float fields_height = static_cast<float>(kFieldRows) * (ImGui::GetFrameHeight() + Px(kFieldPadding) * 2.0f);
    const float fields_top = facts_top - Px(kGap) - fields_height;
    const float keyboard_top = fields_top - Px(kGap) - Px(kKeyboardHeight);
    const float map_top = origin.y + CapsSize("MAP").y + Px(6.0f);
    const bool top = state.reveal;
    DrawPrograms(view, state, busy, origin, ImVec2(Px(kListWidth), facts_top - origin.y - Px(kGap)), edit);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddLine(ImVec2(right - Px(kGap), origin.y), ImVec2(right - Px(kGap), facts_top - Px(kGap)), theme::kBorder,
                  Px(1.0f));

    // The map of the selected instrument, or a card that says how to make one.
    const std::optional<uint32_t> program = SelectedProgram(view, edit, state);
    const MapGeometry map = GeometryOf(ImVec2(right, map_top), ImVec2(right + right_width, keyboard_top - Px(2.0f)));
    std::optional<Region> selected;
    if (program)
    {
        const std::string caps = "MAP OF PROGRAM " + std::to_string(*program);
        DrawCaps(draw, ImVec2(right, origin.y), theme::kTextMuted, caps);
        DrawMap(view, *program, state, busy, map, edit);
        const std::vector<Region> regions = edit::RegionsOf(*view.model.instruments[*program]);
        if (const std::optional<std::size_t> place = PlaceOf(regions, state.region))
        {
            selected = regions[*place];
            DrawFields(view, *program, regions, *place, state, busy, ImVec2(right, fields_top),
                       ImVec2(right_width, fields_height), edit);
        }
        else
        {
            state.held.reset();
            EmptyState(draw, ImVec2(right, fields_top), ImVec2(right + right_width, fields_top + fields_height),
                       "No region selected",
                       regions.empty() ? "Drop WAV files or waves from the browser on the map to make regions."
                                       : "Click a region on the map to show its fields.");
        }
    }
    else
    {
        state.map_min = ImVec2();
        state.map_max = ImVec2();
        EmptyState(draw, map.min, map.max, "No instruments",
                   "Drop WAV files or waves from the browser on the list of programs, or add an instrument.");
    }
    DrawKeyboard(selected, state, ImVec2(right, keyboard_top),
                 ImVec2(right + right_width, keyboard_top + Px(kKeyboardHeight)));
    if (!program)
    {
        state.sounding = -1;
    }

    // The facts across the bottom.
    draw->AddLine(ImVec2(origin.x, facts_top - Px(kGap) * 0.5f), ImVec2(origin.x + room.x, facts_top - Px(kGap) * 0.5f),
                  theme::kBorder, Px(1.0f));
    DrawFacts(view, ImVec2(origin.x, facts_top), ImVec2(room.x, origin.y + room.y - facts_top), top);
    ImGui::End();

    return edit;
}

} // namespace hokuto::app
