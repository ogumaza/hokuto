// SPDX-License-Identifier: MIT

// The parts of the Bank Editor that every console shares: the sizes, the geometry of the key map, the keyboard, the
// labels and typed numbers of the fields, the drops of waves, and the drop targets of the window.

#include "app/bank_editor.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "app/bank_editor_parts.h"
#include "app/browser.h"
#include "app/item_views.h"
#include "app/key_edits.h"
#include "app/theme.h"
#include "app/time_text.h"
#include "app/widgets.h"

namespace hokuto::app
{
namespace
{

using bank_editor_parts::GeometryOf;
using bank_editor_parts::KeyAt;
using bank_editor_parts::MapGeometry;
using bank_editor_parts::VelocityAt;
using theme::Px;

} // namespace

namespace bank_editor_parts
{

bool BeginBankEditorWindow(const std::string& title, BankEditorState& state, bool* open)
{
    ImGui::SetNextWindowSize(ImVec2(Px(kWindowWidth), Px(kWindowHeight)), ImGuiCond_Appearing);
    ImGui::SetNextWindowSizeConstraints(ImVec2(Px(kLeastWidth), Px(kLeastHeight)), ImVec2(FLT_MAX, FLT_MAX));
    const bool visible =
        BeginDialogWindow(title.c_str(), open, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    if (!visible)
    {
        state.sounding = -1;
        state.window_min = ImVec2();
        state.window_max = ImVec2();
        ImGui::End();
        return false;
    }

    state.window_min = ImGui::GetWindowPos();
    state.window_max =
        ImVec2(state.window_min.x + ImGui::GetWindowWidth(), state.window_min.y + ImGui::GetWindowHeight());

    return true;
}

void ForgetDropTargets(BankEditorState& state)
{
    state.map_min = ImVec2();
    state.map_max = ImVec2();
    state.list_min = ImVec2();
    state.list_max = ImVec2();
}

BankEditorLayout BankLayoutOf(int field_rows, const char* caps)
{
    BankEditorLayout layout;
    layout.origin = ImGui::GetCursorScreenPos();
    layout.room = ImGui::GetContentRegionAvail();
    layout.facts_top = layout.origin.y + layout.room.y - Px(kFactsHeight);
    layout.right = layout.origin.x + Px(kListWidth) + Px(kGap) * 2.0f;
    layout.right_width = layout.origin.x + layout.room.x - layout.right;
    layout.fields_height = static_cast<float>(field_rows) * (ImGui::GetFrameHeight() + Px(kFieldPadding) * 2.0f);
    layout.fields_top = field_rows > 0 ? layout.facts_top - Px(kGap) - layout.fields_height : layout.facts_top;
    layout.keyboard_top = layout.fields_top - Px(kGap) - Px(kKeyboardHeight);
    layout.rows_top = layout.origin.y + CapsSize(caps).y + Px(6.0f);

    return layout;
}

void DrawListDivider(ImDrawList* draw, const BankEditorLayout& layout)
{
    draw->AddLine(ImVec2(layout.right - Px(kGap), layout.origin.y),
                  ImVec2(layout.right - Px(kGap), layout.facts_top - Px(kGap)), theme::kBorder, Px(1.0f));
}

void DrawFactsDivider(ImDrawList* draw, const BankEditorLayout& layout)
{
    const float y = layout.facts_top - Px(kGap) * 0.5f;
    draw->AddLine(ImVec2(layout.origin.x, y), ImVec2(layout.origin.x + layout.room.x, y), theme::kBorder, Px(1.0f));
}

std::array<float, 3> ProgramButtonWidths(float width)
{
    const std::array<float, 3> labels = {ImGui::CalcTextSize("Add").x, ImGui::CalcTextSize("Duplicate").x,
                                         ImGui::CalcTextSize("Delete").x};
    const float padding = ImGui::GetStyle().FramePadding.x * 2.0f;
    const float room = width - ImGui::GetStyle().ItemSpacing.x * 2.0f - padding * 3.0f;
    const float scale = room / (labels[0] + labels[1] + labels[2]);

    std::array<float, 3> widths{};
    for (std::size_t button = 0; button < widths.size(); ++button)
    {
        widths[button] = std::max(labels[button] * scale + padding, 0.0f);
    }

    return widths;
}

std::optional<uint32_t> ProgramNumberField(BankEditorState& state, const ShownItem& bank, bool chosen, bool busy,
                                           ImVec2 pos, ImVec2 size, uint32_t highest, BankEdit& edit)
{
    ImGui::SetCursorScreenPos(ImVec2(pos.x, ImGui::GetCursorScreenPos().y));
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Program");
    ImGui::SameLine();
    const std::string shown = chosen ? std::to_string(*state.program) : std::string();
    ImGui::SetNextItemWidth(pos.x + size.x - ImGui::GetCursorScreenPos().x);
    const ShownItem item = TypedItemOf(bank, chosen ? *state.program : std::numeric_limits<uint32_t>::max());
    const std::optional<std::string> ended =
        TypedField("##program", state.typed_program, state.typing_program, shown, busy, state.typed_waiting, item);
    if (!ended || !chosen)
    {
        return std::nullopt;
    }

    // The field takes any text, and refuses what isn't a program with a message.
    state.typing_program = false;
    const char* typed = ended->c_str();
    const unsigned long to = std::strtoul(typed, nullptr, 10);
    if (const std::optional<std::string> refusal = ProgramRefusal(typed, highest))
    {
        edit.refused = *refusal;
        return std::nullopt;
    }

    return to != *state.program ? std::optional(static_cast<uint32_t>(to)) : std::nullopt;
}

MapGeometry GeometryOf(ImVec2 min, ImVec2 max)
{
    return {min, max, (max.x - min.x) / static_cast<float>(kKeys), (max.y - min.y) / static_cast<float>(kKeys)};
}

float KeyX(const MapGeometry& map, int key)
{
    return map.min.x + static_cast<float>(key) * map.key;
}

int KeyAt(const MapGeometry& map, float x)
{
    return std::clamp(static_cast<int>(std::floor((x - map.min.x) / map.key)), 0, kHighest);
}

int VelocityAt(const MapGeometry& map, float y)
{
    return std::clamp(static_cast<int>(std::floor((map.max.y - y) / map.velocity)), 0, kHighest);
}

std::optional<BrowserPlace> AcceptedWave()
{
    std::optional<BrowserPlace> place;
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

    return place;
}

std::optional<BrowserPlace> AcceptWave()
{
    return ImGui::BeginDragDropTarget() ? AcceptedWave() : std::nullopt;
}

void DrawKeyboard(const std::optional<std::pair<int, int>>& covered, std::optional<int> root, BankEditorState& state,
                  ImVec2 min, ImVec2 max)
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
        if (covered && key >= covered->first && key <= covered->second)
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
    if (root)
    {
        const float centre = KeyX(keys, *root) + keys.key * 0.5f;
        draw->AddCircleFilled(ImVec2(centre, min.y + Px(6.0f)), std::max(Px(2.5f), keys.key * 0.3f), theme::kCycle);
    }
}

void FieldLabel(const char* label, const char* tooltip, bool grey)
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

std::string PanOf(int pan)
{
    const float side = pan < kCentre ? kCentre : static_cast<float>(kHighest) - kCentre;
    return PanText((static_cast<float>(pan) - kCentre) / side);
}

bool BeginFieldTable(const char* id, ImVec2 pos, float width)
{
    ImGui::SetCursorScreenPos(pos);
    if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_None, ImVec2(width, 0.0f)))
    {
        return false;
    }

    ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthFixed, Px(kLabelWidth));
    ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch);

    return true;
}

void BeginFacts(ImVec2 pos, ImVec2 size, bool top)
{
    ImGui::SetCursorScreenPos(pos);
    if (top)
    {
        ImGui::SetNextWindowScroll(ImVec2(0.0f, 0.0f));
    }
    ImGui::BeginChild("##facts", size);
    ImGui::PushTextWrapPos(0.0f);
}

std::string ProgramDetails(const std::string& name, uint32_t program)
{
    return name + ", program " + std::to_string(program);
}

void FactLine(const std::string& lead, const std::vector<std::string>& names)
{
    constexpr std::size_t kMostListed = 4;
    const std::size_t shown = std::min(names.size(), kMostListed);
    std::vector<std::string> listed(names.begin(), names.begin() + static_cast<std::ptrdiff_t>(shown));
    if (names.size() > shown)
    {
        listed.push_back(std::to_string(names.size() - shown) + " more");
    }
    MutedText((lead + JoinedNames(listed) + ".").c_str());
    if (ImGui::IsItemHovered() && names.size() > shown)
    {
        std::string all;
        for (const std::string& name : names)
        {
            all += (all.empty() ? "" : "\n") + name;
        }
        WrappedTooltip(all);
    }
}

std::optional<std::string> TypedField(const char* id, std::span<char> text, bool& typing, const std::string& shown,
                                      bool busy, WaitingValues<std::string>& waiting, const ShownItem& item)
{
    const ImGuiID field = ImGui::GetID(id);
    if (!typing)
    {
        std::ranges::fill(text, '\0');
        waiting.Shown(item, field, shown).copy(text.data(), text.size() - 1);
    }
    const bool reenabled = BeginOpenField(typing, busy);
    const bool entered = ImGui::InputText(id, text.data(), text.size(), ImGuiInputTextFlags_EnterReturnsTrue);
    EndOpenField(reenabled);
    typing = ImGui::IsItemActive();

    return waiting.Pass(item, field, shown, entered ? std::optional<std::string>(text.data()) : std::nullopt, busy);
}

std::optional<int> KeyField(const char* id, std::array<char, 8>& text, bool& typing, int shown, float width, bool busy,
                            WaitingValues<std::string>& waiting, const ShownItem& item, BankEdit& edit,
                            const char* refusal)
{
    ImGui::SetNextItemWidth(width);
    const std::optional<std::string> ended = TypedField(id, text, typing, std::to_string(shown), busy, waiting, item);
    if (!ended)
    {
        return std::nullopt;
    }

    typing = false;
    const std::optional<uint64_t> key = WholeNumber(*ended);
    if (!key || *key > static_cast<uint64_t>(kHighest))
    {
        edit.refused = refusal;
        return std::nullopt;
    }

    return static_cast<int>(*key);
}

ShownItem TypedItemOf(const ShownItem& bank, uint32_t program, std::optional<std::size_t> region)
{
    // The place contains the region plus one, or 0 for none.
    return bank.At({program, region ? uint64_t{*region} + 1 : 0, 0});
}

} // namespace bank_editor_parts

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

} // namespace hokuto::app
