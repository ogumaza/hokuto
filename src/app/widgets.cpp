// SPDX-License-Identifier: MIT

// The controls that Dear ImGui doesn't have, drawn in the program's style (widgets.h).

#include "app/widgets.h"

#include <imgui_internal.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "app/theme.h"

namespace hokuto::app
{
namespace
{

using theme::Px;

// Half the width of the square of a point of a line of values, before the display's scale.
constexpr float kPointSize = 3.5f;

std::string Capitals(std::string_view text)
{
    std::string capitals(text);
    for (char& c : capitals)
    {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }

    return capitals;
}

// `font`, set to draw at the display's density. Dear ImGui sets a font's density only while the font is current, and
// most of the program's text comes from fonts that never are. A font left at the density of one pixel to the point
// comes out blurred on a Retina display.
ImFont* AtDisplayDensity(ImFont* font)
{
    font->CurrentRasterizerDensity = ImGui::GetCurrentContext()->FontRasterizerDensity;

    return font;
}

// The text of a tooltip that the caller has begun, with its lines wrapped at a width that reads well.
void WrappedText(std::string_view text)
{
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
    ImGui::TextUnformatted(text.data(), text.data() + text.size());
    ImGui::PopTextWrapPos();
}

// The lines of `text` in `font` at `size` points, broken between words to fit `width`.
std::vector<std::string_view> WrappedLines(ImFont* font, float size, std::string_view text, float width)
{
    std::vector<std::string_view> lines;
    const char* begin = text.data();
    const char* const end = text.data() + text.size();
    while (begin < end)
    {
        const char* line_end = AtDisplayDensity(font)->CalcWordWrapPosition(theme::FontPx(size), begin, end, width);
        if (line_end == begin)
        {
            line_end = begin + 1;
        }
        std::string_view line(begin, static_cast<std::size_t>(line_end - begin));
        while (!line.empty() && (line.back() == ' ' || line.back() == '\n'))
        {
            line.remove_suffix(1);
        }
        lines.push_back(line);
        begin = line_end;
        while (begin < end && (*begin == ' ' || *begin == '\n'))
        {
            begin++;
        }
    }

    return lines;
}

} // namespace

void DrawText(ImDrawList* draw, ImFont* font, float size, ImVec2 pos, ImU32 colour, std::string_view text)
{
    draw->AddText(AtDisplayDensity(font), theme::FontPx(size), ImFloor(pos), colour, text.data(),
                  text.data() + text.size());
}

ImVec2 TextSize(ImFont* font, float size, std::string_view text)
{
    return AtDisplayDensity(font)->CalcTextSizeA(theme::FontPx(size), FLT_MAX, 0.0f, text.data(),
                                                 text.data() + text.size());
}

float Ascent(ImFont* font, float size)
{
    const ImFontBaked* baked = AtDisplayDensity(font)->GetFontBaked(theme::FontPx(size));
    return baked->Ascent * theme::FontPx(size) / baked->Size;
}

void DrawCaps(ImDrawList* draw, ImVec2 pos, ImU32 colour, std::string_view text)
{
    DrawText(draw, theme::GetFonts().caps, theme::kCapsSize, pos, colour, Capitals(text));
}

ImVec2 CapsSize(std::string_view text)
{
    return TextSize(theme::GetFonts().caps, theme::kCapsSize, Capitals(text));
}

bool IconButton(const char* id, Icon icon, ImVec2 size, bool on, ImU32 on_colour, const char* tooltip)
{
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton(id, size);
    const bool hovered = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();

    // A lit button keeps a tint of its colour. An unlit one shows a frame only under the mouse.
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 end(pos.x + size.x, pos.y + size.y);
    if (on)
    {
        draw->AddRectFilled(pos, end, theme::WithAlpha(on_colour, held ? 0.3f : hovered ? 0.24f : 0.16f), Px(6.0f));
    }
    else if (held || hovered)
    {
        draw->AddRectFilled(pos, end, held ? theme::kControlActive : theme::kControlHover, Px(6.0f));
    }
    const ImU32 colour = on ? on_colour : hovered ? theme::kText : theme::kTextMuted;
    DrawIcon(draw, icon, ImVec2(pos.x + size.x * 0.5f, pos.y + size.y * 0.5f), std::min(size.x, size.y) * 0.5f, colour);

    if (tooltip != nullptr && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
    {
        WrappedTooltip(tooltip);
    }

    return clicked;
}

void DrawLetterButton(ImDrawList* draw, ImVec2 min, ImVec2 max, const char* letter, bool on, ImU32 on_colour,
                      bool hovered)
{
    const ImU32 fill = on ? on_colour : hovered ? theme::kControlHover : theme::kControl;
    draw->AddRectFilled(min, max, fill, Px(4.0f));

    ImFont* font = theme::GetFonts().strong;
    const ImVec2 size = TextSize(font, theme::kSmallSize, letter);
    const ImVec2 pos((min.x + max.x - size.x) * 0.5f, (min.y + max.y - size.y) * 0.5f);
    DrawText(draw, font, theme::kSmallSize, pos, on ? theme::kTextOnColour : theme::kText, letter);
}

bool BeginOpenField(bool open, bool busy)
{
    const bool reenable = open && busy;
    if (reenable)
    {
        ImGui::BeginDisabledOverrideReenable();
    }

    return reenable;
}

void EndOpenField(bool reenabled)
{
    if (reenabled)
    {
        ImGui::EndDisabledOverrideReenable();
    }
}

void PlaceInToolbar(float x, float top, float height)
{
    ImGui::SetCursorScreenPos(ImVec2(x, top + (Px(kToolbarHeight) - height) * 0.5f));
}

Toolbar BeginToolbar(ImDrawList* draw)
{
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const float bottom = origin.y + Px(kToolbarHeight);
    draw->AddRectFilled(origin, ImVec2(origin.x + width, bottom), theme::kRaised);
    draw->AddRectFilled(ImVec2(origin.x, bottom - Px(1.0f)), ImVec2(origin.x + width, bottom), theme::kBorder);

    return {origin, width};
}

void EndToolbar(const Toolbar& bar)
{
    ImGui::SetCursorScreenPos(ImVec2(bar.origin.x, bar.origin.y + Px(kToolbarHeight)));
}

float ToolbarCaps(ImDrawList* draw, float x, float top, const char* label)
{
    const ImVec2 caps = CapsSize(label);
    DrawCaps(draw, ImVec2(x, top + (Px(kToolbarHeight) - caps.y) * 0.5f), theme::kTextMuted, label);

    return x + caps.x + Px(8.0f);
}

bool BeginDialogWindow(const char* title, bool* open, ImGuiWindowFlags flags)
{
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, Px(theme::kDialogPadding));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::ColorConvertU32ToFloat4(theme::kRaised));
    ImGui::PushStyleColor(ImGuiCol_Border, ImGui::ColorConvertU32ToFloat4(theme::kBorderStrong));
    const bool visible = ImGui::Begin(
        title, open,
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings | flags);
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar();

    return visible;
}

bool IsBlack(int key)
{
    const int note = key % 12;
    return note == 1 || note == 3 || note == 6 || note == 8 || note == 10;
}

std::string FxName(std::size_t channel)
{
    return std::string("FX ") + static_cast<char>('A' + channel);
}

bool IsContinuationByte(char byte)
{
    return (static_cast<unsigned char>(byte) & 0xC0) == 0x80;
}

void DrawPoint(ImDrawList* draw, float x, float top, float bottom, ImU32 colour, bool selected, bool conditional)
{
    const ImU32 fill = selected ? theme::kText : colour;
    if (bottom > top)
    {
        draw->AddRectFilled(ImVec2(x - Px(1.0f), top), ImVec2(x + Px(1.0f), bottom), theme::WithAlpha(fill, 0.7f));
    }

    const float y = std::floor((top + bottom) * 0.5f);
    const float size = Px(kPointSize);
    const ImVec2 min(x - size, y - size);
    const ImVec2 max(x + size, y + size);
    draw->AddRectFilled(min, max, conditional ? theme::kPanel : fill);
    draw->AddRect(min, max, selected ? colour : fill, 0.0f, Px(1.0f));
}

std::string ShortcutText(ImGuiKeyChord keys)
{
    std::string text;
#ifdef __APPLE__
    if ((keys & ImGuiMod_Super) != 0)
    {
        text += "⌃"; // Control
    }
    if ((keys & ImGuiMod_Alt) != 0)
    {
        text += "⌥"; // Option
    }
    if ((keys & ImGuiMod_Shift) != 0)
    {
        text += "⇧"; // Shift
    }
    if ((keys & ImGuiMod_Ctrl) != 0)
    {
        text += "⌘"; // Command
    }
#else
    if ((keys & ImGuiMod_Ctrl) != 0)
    {
        text += "Ctrl+";
    }
    if ((keys & ImGuiMod_Shift) != 0)
    {
        text += "Shift+";
    }
    if ((keys & ImGuiMod_Alt) != 0)
    {
        text += "Alt+";
    }
#endif

    // The names of the keypad's keys. Dear ImGui calls them Keypad0, KeypadEnter and so on.
    const auto key = static_cast<ImGuiKey>(keys & ~ImGuiMod_Mask_);
    if (key >= ImGuiKey_Keypad0 && key <= ImGuiKey_Keypad9)
    {
        return text + "Num " + static_cast<char>('0' + (key - ImGuiKey_Keypad0));
    }
    switch (key)
    {
    case ImGuiKey_KeypadDecimal:
        return text + "Num .";
    case ImGuiKey_KeypadDivide:
        return text + "Num /";
    case ImGuiKey_KeypadMultiply:
        return text + "Num *";
    case ImGuiKey_KeypadSubtract:
        return text + "Num -";
    case ImGuiKey_KeypadAdd:
        return text + "Num +";
    case ImGuiKey_KeypadEnter:
        return text + "Num Enter";
    default:
        return text + ImGui::GetKeyName(key);
    }
}

std::string FitText(ImFont* font, float size, std::string_view text, float width)
{
    if (TextSize(font, size, text).x <= width)
    {
        return std::string(text);
    }

    // Drop the last character, with its continuation bytes, until the rest and the ellipsis fit. No space comes before
    // the ellipsis.
    std::string cut(text);
    while (!cut.empty())
    {
        while (!cut.empty() && IsContinuationByte(cut.back()))
        {
            cut.pop_back();
        }
        if (!cut.empty())
        {
            cut.pop_back();
        }
        while (!cut.empty() && cut.back() == ' ')
        {
            cut.pop_back();
        }

        const std::string fitted = cut + "…";
        if (TextSize(font, size, fitted).x <= width)
        {
            return fitted;
        }
    }

    return {};
}

std::string CutLinesTooltip(std::span<const FittedLine> lines)
{
    const auto cut = [](const FittedLine& line)
    {
        return TextSize(line.font, line.size, line.text).x > line.width;
    };
    if (std::ranges::none_of(lines, cut))
    {
        return {};
    }

    std::string tooltip;
    for (const FittedLine& line : lines)
    {
        if (!line.text.empty())
        {
            tooltip += (tooltip.empty() ? "" : "\n") + std::string(line.text);
        }
    }

    return tooltip;
}

std::string FitCaps(std::string_view text, float width)
{
    ImFont* caps = theme::GetFonts().caps;
    const std::string capitals = Capitals(text);
    const std::size_t space = capitals.rfind(' ');
    if (CapsSize(capitals).x <= width || space == std::string::npos || space == 0 || capitals.size() - space != 2)
    {
        return FitText(caps, theme::kCapsSize, capitals, width);
    }

    // The last word tells the label from its neighbours, such as Volume 2 from Volume.
    const std::string last = capitals.substr(space);
    const float room = width - TextSize(caps, theme::kCapsSize, last).x;
    return FitText(caps, theme::kCapsSize, capitals.substr(0, space), room) + last;
}

void ZoneHeader(std::string_view title, std::string_view detail)
{
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const float height = Px(32.0f);
    draw->AddRectFilled(pos, ImVec2(pos.x + width, pos.y + height), theme::kRaised);
    draw->AddRectFilled(ImVec2(pos.x, pos.y + height - Px(1.0f)), ImVec2(pos.x + width, pos.y + height),
                        theme::kBorder);

    // The title in semibold. The detail sits on the title's baseline.
    ImFont* strong = theme::GetFonts().strong;
    const ImVec2 title_size = TextSize(strong, theme::kTextSize, title);
    const float title_top = std::floor(pos.y + (height - title_size.y) * 0.5f);
    DrawText(draw, strong, theme::kTextSize, ImVec2(pos.x + Px(14.0f), title_top), theme::kText, title);
    if (!detail.empty())
    {
        ImFont* font = theme::GetFonts().text;
        const float room = width - Px(14.0f) - title_size.x - Px(12.0f) - Px(14.0f);
        const std::string shown = FitText(font, theme::kSmallSize, detail, room);
        const ImVec2 size = TextSize(font, theme::kSmallSize, shown);
        const float baseline = title_top + Ascent(strong, theme::kTextSize);
        DrawText(draw, font, theme::kSmallSize,
                 ImVec2(pos.x + width - Px(14.0f) - size.x, baseline - Ascent(font, theme::kSmallSize)),
                 theme::kTextMuted, shown);
    }

    // A dummy item takes the header's place. The zone's content then starts below it.
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
    ImGui::Dummy(ImVec2(width, height));
    ImGui::PopStyleVar();
}

void EmptyState(ImDrawList* draw, ImVec2 min, ImVec2 max, std::string_view title, std::string_view text)
{
    // In a narrow zone, the card's sides lose padding first. The text then wraps, and the title ends in an ellipsis.
    ImFont* font = theme::GetFonts().text;
    const ImVec2 padding(Px(28.0f), Px(20.0f));
    const float gap = Px(6.0f);
    const float width = max.x - min.x;
    const float room = std::max(width - Px(16.0f) - Px(24.0f), Px(40.0f));
    const std::string shown_title = FitText(font, theme::kTitleSize, title, room);
    const ImVec2 title_size = TextSize(font, theme::kTitleSize, shown_title);
    const std::vector<std::string_view> lines = WrappedLines(font, theme::kTextSize, text, room);
    const float line_height = TextSize(font, theme::kTextSize, "A").y;
    float text_width = 0.0f;
    for (const std::string_view line : lines)
    {
        text_width = std::max(text_width, TextSize(font, theme::kTextSize, line).x);
    }
    const float text_height = line_height * static_cast<float>(lines.size());
    const float top = std::floor(min.y + (max.y - min.y - title_size.y - gap - text_height) * 0.45f);

    // A card behind the text. Web pages draw their empty states that way.
    const float card_width = std::min(std::max(title_size.x, text_width) + padding.x * 2.0f, width - Px(16.0f));
    const float card_left = std::floor(min.x + (width - card_width) * 0.5f);
    const ImVec2 card_min(card_left, top - padding.y);
    const ImVec2 card_max(card_left + card_width, top + title_size.y + gap + text_height + padding.y);
    draw->PushClipRect(min, max, true);
    draw->AddRectFilled(card_min, card_max, theme::kRaised, Px(10.0f));
    draw->AddRect(card_min, card_max, theme::kBorder, Px(10.0f), Px(1.0f));
    DrawText(draw, font, theme::kTitleSize, ImVec2(min.x + (width - title_size.x) * 0.5f, top), theme::kTextMuted,
             shown_title);
    float y = top + title_size.y + gap;
    for (const std::string_view line : lines)
    {
        const float line_width = TextSize(font, theme::kTextSize, line).x;
        DrawText(draw, font, theme::kTextSize, ImVec2(min.x + (width - line_width) * 0.5f, y), theme::kTextMuted, line);
        y += line_height;
    }
    draw->PopClipRect();
}

void DrawDialogHeading(ImDrawList* draw, const char* text)
{
    DrawCaps(draw, ImGui::GetCursorScreenPos(), theme::kTextMuted, text);
    ImGui::Dummy(CapsSize(text));
}

void SettingLabel(const char* label)
{
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    MutedText(label);
    ImGui::TableNextColumn();
}

ImVec2 CentreInside(ImVec2 centre, ImVec2 size, float margin)
{
    const ImGuiViewport* viewport = ImGui::GetMainViewport();

    const auto inside = [margin](float middle, float length, float start, float room)
    {
        const float low = start + margin + length * 0.5f;
        const float high = start + room - margin - length * 0.5f;
        return low > high ? start + room * 0.5f : std::clamp(middle, low, high);
    };
    return ImVec2(inside(centre.x, size.x, viewport->WorkPos.x, viewport->WorkSize.x),
                  inside(centre.y, size.y, viewport->WorkPos.y, viewport->WorkSize.y));
}

DialogPlace PlaceDialog(const std::string& title, const std::optional<ImVec2>& place, const std::optional<float>& top,
                        float margin)
{
    const ImGuiWindow* window = ImGui::FindWindowByName(title.c_str());
    DialogPlace placed;
    placed.dragged = window != nullptr && ImGui::GetCurrentContext()->MovingWindow == window;
    const ImVec2 size = window != nullptr ? window->Size : ImVec2(0.0f, 0.0f);
    placed.centre = CentreInside(place.value_or(ImGui::GetMainViewport()->GetCenter()), size, margin);
    if (top)
    {
        placed.centre = CentreInside(ImVec2(placed.centre.x, *top + size.y * 0.5f), size, margin);
    }
    if (!placed.dragged)
    {
        ImGui::SetNextWindowPos(placed.centre, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    }

    return placed;
}

void FollowDialogDrag(const DialogPlace& placed, std::optional<ImVec2>& place, std::optional<float>& top)
{
    if (placed.dragged)
    {
        const ImVec2 corner = ImGui::GetWindowPos();
        const ImVec2 size = ImGui::GetWindowSize();
        const ImVec2 moved(corner.x + size.x * 0.5f, corner.y + size.y * 0.5f);
        if (std::abs(moved.x - placed.centre.x) > 1.0f || std::abs(moved.y - placed.centre.y) > 1.0f)
        {
            place = moved;
        }
    }
    if (placed.dragged || (!top && !ImGui::IsWindowAppearing()))
    {
        top = ImGui::GetWindowPos().y;
    }
}

bool PrimaryButton(const char* label)
{
    // A disabled button takes the plain look, faded. The accent colour faded still reads as a button that can be
    // clicked.
    if ((ImGui::GetCurrentContext()->CurrentItemFlags & ImGuiItemFlags_Disabled) != 0)
    {
        return ImGui::Button(label);
    }

    ImGui::PushStyleColor(ImGuiCol_Button, ImGui::ColorConvertU32ToFloat4(theme::kAccent));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                          ImGui::ColorConvertU32ToFloat4(theme::Mix(theme::kAccent, theme::kText, 0.15f)));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                          ImGui::ColorConvertU32ToFloat4(theme::Mix(theme::kAccent, theme::kWindow, 0.15f)));
    ImGui::PushStyleColor(ImGuiCol_Border, ImGui::ColorConvertU32ToFloat4(theme::kAccent));
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(theme::kTextOnAccent));
    const bool clicked = ImGui::Button(label);
    ImGui::PopStyleColor(5);

    return clicked;
}

bool CheckBox(const char* label, bool* on)
{
    ImFont* font = theme::GetFonts().text;
    const ImVec2 text_size = TextSize(font, theme::kTextSize, label);
    const float box = Px(18.0f);
    const float gap = Px(8.0f);
    const float height = ImGui::GetFrameHeight();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton(label, ImVec2(box + gap + text_size.x, height));
    if (clicked)
    {
        *on = !*on;
    }

    // A box in the accent colour with a check mark when it's on, and an empty box when it's off. Inside a disabled
    // block, the style's alpha fades it.
    const bool hovered = ImGui::IsItemHovered();
    const float alpha = ImGui::GetStyle().Alpha;
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 min(pos.x, std::floor(pos.y + (height - box) * 0.5f));
    const ImVec2 max(min.x + box, min.y + box);
    if (*on)
    {
        const ImU32 fill = hovered ? theme::Mix(theme::kAccent, theme::kText, 0.15f) : theme::kAccent;
        draw->AddRectFilled(min, max, theme::WithAlpha(fill, alpha), Px(4.0f));
        const std::array<ImVec2, 3> check = {ImVec2(min.x + box * 0.26f, min.y + box * 0.53f),
                                             ImVec2(min.x + box * 0.43f, min.y + box * 0.7f),
                                             ImVec2(min.x + box * 0.75f, min.y + box * 0.33f)};
        draw->AddPolyline(check.data(), static_cast<int>(check.size()), theme::WithAlpha(theme::kTextOnAccent, alpha),
                          Px(2.0f));
    }
    else
    {
        draw->AddRectFilled(min, max, theme::WithAlpha(hovered ? theme::kControlHover : theme::kControl, alpha),
                            Px(4.0f));
        draw->AddRect(min, max, theme::WithAlpha(theme::kBorderStrong, alpha), Px(4.0f), Px(1.0f));
    }
    DrawText(draw, font, theme::kTextSize, ImVec2(max.x + gap, std::floor(pos.y + (height - text_size.y) * 0.5f)),
             theme::WithAlpha(theme::kText, alpha), label);

    return clicked;
}

void ItemTooltip(std::string_view text)
{
    if (text.empty())
    {
        return;
    }

    // Dear ImGui reads the padding as the tooltip begins. The code around it may have pushed other padding.
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, Px(theme::kWindowPadding));
    const bool shown = ImGui::BeginItemTooltip();
    ImGui::PopStyleVar();
    if (!shown)
    {
        return;
    }

    WrappedText(text);
    ImGui::EndTooltip();
}

void PushFigures()
{
    ImGui::PushFont(theme::GetFonts().figures, 0.0f);
}

void PushMenuSpacing()
{
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(Px(theme::kItemSpacing), Px(theme::kItemSpacing)));
}

void MutedText(const char* text)
{
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme::kTextMuted), "%s", text);
}

void TextWrappedAtSpaces(std::string_view text)
{
    // The lines go out as one item. Their spacing is that of Dear ImGui's TextWrapped. An item for each line would put
    // the spacing between items between the lines too.
    const float width = ImGui::GetContentRegionAvail().x;
    std::string lines;
    std::size_t begin = 0;
    while (begin < text.size())
    {
        // The words from `begin` that fit on the line, and one word at least.
        std::size_t end = text.find(' ', begin);
        end = end == std::string_view::npos ? text.size() : end;
        while (end < text.size())
        {
            std::size_t next = text.find(' ', end + 1);
            next = next == std::string_view::npos ? text.size() : next;
            if (ImGui::CalcTextSize(text.data() + begin, text.data() + next).x > width)
            {
                break;
            }
            end = next;
        }

        // A word wider than the line, such as a long name, breaks where the line ends. A break never falls inside a
        // character.
        if (width > 0.0f && ImGui::CalcTextSize(text.data() + begin, text.data() + end).x > width)
        {
            std::size_t fits = begin;
            for (std::size_t at = begin + 1; at <= end; at++)
            {
                if (at < end && IsContinuationByte(text[at]))
                {
                    continue;
                }
                if (fits > begin && ImGui::CalcTextSize(text.data() + begin, text.data() + at).x > width)
                {
                    break;
                }
                fits = at;
            }
            lines.append(lines.empty() ? "" : "\n").append(text.substr(begin, fits - begin));
            begin = fits;
            continue;
        }

        lines.append(lines.empty() ? "" : "\n").append(text.substr(begin, end - begin));
        begin = text.find_first_not_of(' ', end);
        begin = begin == std::string_view::npos ? text.size() : begin;
    }
    if (!lines.empty())
    {
        // A wrap position that the window sets would break the lines again, after a full stop too.
        ImGui::PushTextWrapPos(-1.0f);
        ImGui::TextUnformatted(lines.data(), lines.data() + lines.size());
        ImGui::PopTextWrapPos();
    }
}

void WrappedTooltip(std::string_view text)
{
    if (text.empty())
    {
        return;
    }

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, Px(theme::kWindowPadding));
    const bool shown = ImGui::BeginTooltip();
    ImGui::PopStyleVar();
    if (!shown)
    {
        return;
    }

    WrappedText(text);
    ImGui::EndTooltip();
}

void RectTooltip(ImVec2 min, ImVec2 max, std::string_view text)
{
    // Dear ImGui shows an item's tooltip once the mouse has rested over the item for a moment.
    const ImGuiContext& context = *ImGui::GetCurrentContext();
    if (ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(min, max) && !ImGui::IsAnyMouseDown() &&
        context.MouseStationaryTimer >= ImGui::GetStyle().HoverStationaryDelay)
    {
        WrappedTooltip(text);
    }
}

std::string FittedText(const std::string& text, float width)
{
    if (ImGui::CalcTextSize(text.c_str()).x <= width)
    {
        return text;
    }

    // Each step drops a character, with its continuation bytes, and the spaces before the ellipsis.
    std::size_t end = text.size();
    while (end > 0)
    {
        do
        {
            end--;
        } while (end > 0 && IsContinuationByte(text[end]));
        std::size_t kept = end;
        while (kept > 0 && text[kept - 1] == ' ')
        {
            kept--;
        }
        const std::string cut = text.substr(0, kept) + "…";
        if (ImGui::CalcTextSize(cut.c_str()).x <= width)
        {
            return cut;
        }
    }

    return "…";
}

bool Matches(std::string_view item, std::string_view filter)
{
    const auto same = [](char a, char b)
    {
        return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
    };
    return filter.empty() || std::search(item.begin(), item.end(), filter.begin(), filter.end(), same) != item.end();
}

bool FilteredCombo(const char* id, const std::vector<std::string>& items, int& chosen, const char* none,
                   std::array<char, 128>& filter, float width)
{
    const bool listed = chosen >= 0 && static_cast<std::size_t>(chosen) < items.size();
    const char* preview = listed ? items[static_cast<std::size_t>(chosen)].c_str() : none != nullptr ? none : "";
    ImGui::SetNextItemWidth(width);
    if (!ImGui::BeginCombo(id, preview, ImGuiComboFlags_HeightLargest))
    {
        return false;
    }

    // The field takes the keys as the menu opens, empty.
    if (ImGui::IsWindowAppearing())
    {
        filter.fill('\0');
        ImGui::SetKeyboardFocusHere();
    }
    // A compact row of controls may have pushed tighter padding. The field takes the theme's.
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, Px(theme::kFramePadding));
    const bool entered = ImGui::InputTextWithHint("##filter", "Search", filter.data(), filter.size(),
                                                  ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::PopStyleVar();
    std::vector<int> shown;
    if (none != nullptr && Matches(none, filter.data()))
    {
        shown.push_back(-1);
    }
    for (std::size_t i = 0; i < items.size(); i++)
    {
        if (Matches(items[i], filter.data()))
        {
            shown.push_back(static_cast<int>(i));
        }
    }
    if (shown.empty())
    {
        MutedText("No matches");
    }

    // Return in the field chooses the one item left and closes the menu, like a click on the item.
    bool changed = false;
    if (entered && shown.size() == 1)
    {
        changed = shown.front() != chosen;
        chosen = shown.front();
        ImGui::CloseCurrentPopup();
    }

    // The items that hold the filter's text scroll under the field. A choice closes the menu, a choice of the chosen
    // item too. The items sit in a child window, and Dear ImGui closes the menu only on a click in the menu's window.
    const float rows = ImGui::GetTextLineHeightWithSpacing() * static_cast<float>(shown.size());
    ImGui::BeginChild("##items", ImVec2(0.0f, std::min(rows, Px(260.0f))));
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(shown.size()));
    while (clipper.Step())
    {
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++)
        {
            const int i = shown[static_cast<std::size_t>(row)];
            ImGui::PushID(i);
            const char* label = i < 0 ? none : items[static_cast<std::size_t>(i)].c_str();
            if (ImGui::Selectable(label, i == chosen))
            {
                changed = i != chosen;
                chosen = i;
                ImGui::CloseCurrentPopup();
            }
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
    ImGui::EndCombo();

    return changed;
}

int SegmentedSwitch(const char* id, std::span<const std::string> labels, std::span<const uint8_t> lit, ImVec2 size,
                    std::span<const uint8_t> disabled)
{
    // Inside a disabled block, the style's alpha fades the strip, its switches and their labels.
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const ImVec2 end(pos.x + size.x, pos.y + size.y);
    const float inset = Px(2.0f);
    const float segment = (size.x - inset * 2.0f) / static_cast<float>(std::max<std::size_t>(labels.size(), 1));
    draw->AddRectFilled(pos, end, ImGui::GetColorU32(theme::kDisplay), Px(6.0f));
    draw->AddRect(pos, end, ImGui::GetColorU32(theme::kBorder), Px(6.0f), Px(1.0f));

    // Each switch, with its label in the middle.
    ImFont* figures = theme::GetFonts().figures;
    int clicked = -1;
    ImGui::PushID(id);
    for (std::size_t i = 0; i < labels.size(); i++)
    {
        const ImVec2 min(std::floor(pos.x + inset + segment * static_cast<float>(i)), pos.y + inset);
        const ImVec2 max(std::floor(pos.x + inset + segment * static_cast<float>(i + 1)), end.y - inset);
        ImGui::SetCursorScreenPos(min);
        ImGui::PushID(static_cast<int>(i));
        const bool off = i < disabled.size() && disabled[i] != 0;
        ImGui::BeginDisabled(off);
        if (ImGui::InvisibleButton("##switch", ImVec2(max.x - min.x, max.y - min.y)))
        {
            clicked = static_cast<int>(i);
        }
        const bool hovered = ImGui::IsItemHovered();
        const bool held = ImGui::IsItemActive();
        ImGui::EndDisabled();
        ImGui::PopID();

        const bool on = i < lit.size() && lit[i] != 0;
        if (on)
        {
            const float alpha = held ? 0.42f : hovered ? 0.36f : 0.3f;
            draw->AddRectFilled(min, max, ImGui::GetColorU32(theme::kAccent, alpha), Px(4.0f));
        }
        else if (hovered || held)
        {
            draw->AddRectFilled(min, max, ImGui::GetColorU32(held ? theme::kControlActive : theme::kControlHover),
                                Px(4.0f));
        }
        const ImVec2 text = TextSize(figures, theme::kTextSize, labels[i]);
        const ImVec2 at(std::floor((min.x + max.x - text.x) * 0.5f), std::floor((min.y + max.y - text.y) * 0.5f));
        const ImU32 colour = on || hovered ? theme::kText : off ? theme::kTextDim : theme::kTextMuted;
        DrawText(draw, figures, theme::kTextSize, at, ImGui::GetColorU32(colour), labels[i]);
    }
    ImGui::PopID();

    return clicked;
}

bool EscapeCloses()
{
    // At the window's start, a field that's being typed in is still the active item. It ends at the same Escape, later
    // in the frame.
    return ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
           ImGui::IsKeyPressed(ImGuiKey_Escape, false) && ImGui::GetInputTextState(ImGui::GetActiveID()) == nullptr;
}

bool MenuBarFocused()
{
    // A menu opens as a popup of the bar, and a submenu as a popup of its menu.
    for (const ImGuiWindow* window = ImGui::GetCurrentContext()->NavWindow; window != nullptr;
         window = window->ParentWindow)
    {
        if (std::string_view(window->Name) == "##MainMenuBar")
        {
            return true;
        }
    }

    return false;
}

} // namespace hokuto::app
