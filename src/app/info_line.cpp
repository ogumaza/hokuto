// SPDX-License-Identifier: MIT

// The info line at the top of a zone, with its fields and the entries that the user types into, and the text entries
// of the other windows.

#include <imgui_internal.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <utility>

#include "app/theme.h"
#include "app/widgets.h"

namespace hokuto::app
{

namespace
{

using theme::Px;

// The info line's height, before the display's scale.
constexpr float kInfoLineHeight = 28.0f;

// Lets Dear ImGui's text field grow the std::string that `data` types into.
int GrowText(ImGuiInputTextCallbackData* data)
{
    if (data->EventFlag == ImGuiInputTextFlags_CallbackResize)
    {
        auto* text = static_cast<std::string*>(data->UserData);
        text->resize(static_cast<std::size_t>(data->BufTextLen));
        data->Buf = text->data();
    }

    return 0;
}

// Moves the typing of a field that Tab or Shift+Tab ended to `box`.
void PassTyping(TextTyping& typing, ImGuiID box, std::string_view text)
{
    // A field that Tab or Shift+Tab ended passes the typing on, in the frame where it ended or the next. Tab passes it
    // to the next box drawn, and Shift+Tab to the box drawn before the field.
    const bool recent = ImGui::GetFrameCount() - typing.passed <= 1;
    if (typing.box == 0 && recent && (typing.onward || typing.back == box))
    {
        typing.box = box;
        typing.text = std::string(text);
        typing.focus = true;
    }
    if (typing.box != 0 || !recent)
    {
        typing.onward = false;
        typing.back = 0;
    }
}

// Draws the closed box at `pos`, `size` large, with `text` on one line. A click opens the field.
void DrawTextBox(std::string_view text, ImVec2 pos, ImVec2 size, float width, TextTyping& typing, ImGuiID box)
{
    const float rounding = ImGui::GetStyle().FrameRounding;
    const ImVec2 pad = ImGui::GetStyle().FramePadding;
    ImGui::SetCursorScreenPos(pos);
    ImGui::InvisibleButton("##box", size);
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), hovered ? theme::kControlHover : theme::kControl,
                        rounding);
    std::string line(text);
    std::replace(line.begin(), line.end(), '\n', ' ');
    ImFont* font = theme::GetFonts().text;
    const std::string shown = FitText(font, theme::kTextSize, line, width - pad.x * 2.0f);
    const ImVec2 shown_size = TextSize(font, theme::kTextSize, shown);
    DrawText(draw, font, theme::kTextSize, ImVec2(pos.x + pad.x, pos.y + std::floor((size.y - shown_size.y) * 0.5f)),
             theme::kText, shown);
    if (hovered)
    {
        ImGui::SetMouseCursor(ImGuiMouseCursor_TextInput);
    }
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
    {
        typing.box = box;
        typing.text = std::string(text);
        typing.focus = true;
    }
}

// Draws the open field at `pos`, `width` wide. Returns the typed text when the field ends.
std::optional<std::string> TypeInField(std::string_view text, ImVec2 pos, float width, TextTyping& typing,
                                       ImGuiID previous)
{
    // The text field takes the box's place. Its ID differs from the box's: a tap on a trackpad releases the button in
    // the next frame, and the release would end a field that took the box's ID. A click elsewhere ends the typing
    // before anything else takes the click. A field that has lost the focus without ending ends too.
    std::optional<std::string> typed;
    ImGui::SetCursorScreenPos(pos);
    ImGui::SetNextItemWidth(width);
    const bool focusing = std::exchange(typing.focus, false);
    if (focusing)
    {
        ImGui::SetKeyboardFocusHere();
    }
    ImGui::InputText("##field", typing.text.data(), typing.text.capacity() + 1,
                     ImGuiInputTextFlags_AutoSelectAll | ImGuiInputTextFlags_CallbackResize, GrowText, &typing.text);
    // Dear ImGui's Tab moves only among text fields, and the other boxes aren't fields until a click. Tab ends the
    // field here, and passes the typing on.
    const bool active = ImGui::IsItemActive();
    const bool away = active && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsItemHovered();
    const bool tabbed = active && ImGui::IsKeyPressed(ImGuiKey_Tab, false);
    if (ImGui::IsItemDeactivated() || away || tabbed)
    {
        typed = !tabbed && ImGui::IsKeyPressed(ImGuiKey_Escape) ? std::string(text) : typing.text;
        typing.box = 0;
        if (tabbed)
        {
            typing.onward = !ImGui::GetIO().KeyShift;
            typing.back = ImGui::GetIO().KeyShift ? previous : 0;
            typing.passed = ImGui::GetFrameCount();
        }
    }
    else if (!active && !focusing)
    {
        typing.box = 0;
    }

    return typed;
}

} // namespace

InfoLine BeginInfoLine()
{
    InfoLine line;
    line.draw = ImGui::GetWindowDrawList();
    line.origin = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    line.bottom = line.origin.y + Px(kInfoLineHeight);
    line.middle = (line.origin.y + line.bottom) * 0.5f;
    line.x = line.origin.x + Px(14.0f);
    line.right = line.origin.x + width - Px(14.0f);
    line.draw->AddRectFilled(line.origin, ImVec2(line.origin.x + width, line.bottom), theme::kPanelAlt);
    line.draw->AddRectFilled(ImVec2(line.origin.x, line.bottom - Px(1.0f)), ImVec2(line.origin.x + width, line.bottom),
                             theme::kBorder);

    return line;
}

void InfoField(InfoLine& line, const char* label, std::string_view value)
{
    const ImVec2 caps = CapsSize(label);
    ImFont* figures = theme::GetFonts().figures;
    const ImVec2 size = TextSize(figures, theme::kTextSize, value);
    if (line.x + caps.x + Px(8.0f) + size.x > line.right)
    {
        line.x = line.right;
        return;
    }
    DrawCaps(line.draw, ImVec2(line.x, line.middle - caps.y * 0.5f), theme::kTextMuted, label);
    DrawText(line.draw, figures, theme::kTextSize, ImVec2(line.x + caps.x + Px(8.0f), line.middle - size.y * 0.5f),
             theme::kText, value);
    line.x += caps.x + Px(8.0f) + size.x + Px(22.0f);
}

std::optional<std::string> InfoEntry(InfoLine& line, const char* label, std::string_view value, InfoTyping& typing)
{
    // A field that doesn't fit doesn't show, and typing in it ends.
    const ImVec2 caps = CapsSize(label);
    ImFont* figures = theme::GetFonts().figures;
    const ImVec2 size = TextSize(figures, theme::kTextSize, value);
    if (line.x + caps.x + Px(8.0f) + size.x > line.right)
    {
        line.x = line.right;
        if (typing.field == label)
        {
            typing.field.clear();
        }
        return std::nullopt;
    }
    DrawCaps(line.draw, ImVec2(line.x, line.middle - caps.y * 0.5f), theme::kTextMuted, label);
    const ImVec2 at(line.x + caps.x + Px(8.0f), line.middle - size.y * 0.5f);
    line.x = at.x + size.x + Px(22.0f);
    ImGui::PushID(label);

    // Until a click, the value shows as text. A box behind it shows that it can be typed in.
    std::optional<std::string> typed;
    const ImVec2 pad(Px(4.0f), Px(3.0f));
    if (typing.field != label)
    {
        ImGui::SetCursorScreenPos(ImVec2(at.x - pad.x, at.y - pad.y));
        ImGui::InvisibleButton("##value", ImVec2(size.x + pad.x * 2.0f, size.y + pad.y * 2.0f));
        if (ImGui::IsItemHovered())
        {
            line.draw->AddRectFilled(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), theme::kRaised, Px(3.0f));
            ImGui::SetMouseCursor(ImGuiMouseCursor_TextInput);
        }
        DrawText(line.draw, figures, theme::kTextSize, at, theme::kText, value);
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
        {
            typing.field = label;
            typing.text = {};
            value.copy(typing.text.data(), typing.text.size() - 1);
            typing.focus = true;
        }
        ImGui::PopID();
        return std::nullopt;
    }

    // The text field takes the value's place. Its ID differs from the value's: a tap on a trackpad releases the button
    // in the next frame, and the release would end a field that took the value's ID. A click elsewhere ends the typing
    // before anything else takes the click. A field that has lost the focus without ending ends too.
    const float height = ImGui::GetFontSize() + pad.y * 2.0f;
    ImGui::SetCursorScreenPos(ImVec2(at.x - pad.x, line.middle - height * 0.5f));
    ImGui::SetNextItemWidth(std::max(size.x + Px(28.0f), Px(72.0f)));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, pad);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImGui::ColorConvertU32ToFloat4(theme::kDisplay));
    const bool focusing = std::exchange(typing.focus, false);
    if (focusing)
    {
        ImGui::SetKeyboardFocusHere();
    }
    PushFigures();
    ImGui::InputText("##typing", typing.text.data(), typing.text.size(), ImGuiInputTextFlags_AutoSelectAll);
    ImGui::PopFont();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    const bool active = ImGui::IsItemActive();
    const bool away = active && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsItemHovered();
    if (ImGui::IsItemDeactivated() || away)
    {
        typed = std::string(typing.text.data());
        typing.field.clear();
    }
    else if (!active && !focusing)
    {
        typing.field.clear();
    }
    ImGui::PopID();

    return typed;
}

std::optional<std::string> TextEntry(const char* id, std::string_view text, ImVec2 pos, float width, TextTyping& typing)
{
    ImGui::PushID(id);
    const ImGuiID box = ImGui::GetID("##box");
    const ImVec2 size(width, ImGui::GetFrameHeight());
    const ImGuiID previous = std::exchange(typing.last, box);

    PassTyping(typing, box, text);

    // Until a click, the text shows in a box like a text field's. A line break shows as a space.
    if (typing.box != box)
    {
        DrawTextBox(text, pos, size, width, typing, box);
        ImGui::PopID();
        return std::nullopt;
    }

    const std::optional<std::string> typed = TypeInField(text, pos, width, typing, previous);
    ImGui::PopID();

    return typed;
}

bool TextEntryOpen(const char* id, const TextTyping& typing)
{
    ImGui::PushID(id);
    const ImGuiID box = ImGui::GetID("##box");
    ImGui::PopID();

    return typing.box == box;
}

std::optional<std::string> BusyInfoEntry(InfoLine& line, const char* label, const std::string& value,
                                         InfoTyping& typing, WaitingValues<std::string>& waiting, const ShownItem& item,
                                         bool busy)
{
    const ImGuiID field = ImGui::GetID(label);
    const std::string& shown = waiting.Shown(item, field, value);
    std::optional<std::string> typed;
    if (busy && typing.field != label)
    {
        InfoField(line, label, shown);
    }
    else
    {
        typed = InfoEntry(line, label, shown, typing);
    }

    return waiting.Pass(item, field, value, std::move(typed), busy);
}

void NoObjectSelected(InfoLine& line)
{
    ImFont* text = theme::GetFonts().text;
    const ImVec2 size = TextSize(text, theme::kSmallSize, "No Object Selected");
    DrawText(line.draw, text, theme::kSmallSize, ImVec2(line.x, line.middle - size.y * 0.5f), theme::kTextMuted,
             "No Object Selected");
}

void EndInfoLine(const InfoLine& line)
{
    ImGui::SetCursorScreenPos(ImVec2(line.origin.x, line.bottom));
}

} // namespace hokuto::app
