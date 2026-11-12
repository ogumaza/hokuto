// SPDX-License-Identifier: MIT

// The parts that several zones draw: the zone's window, the palette of track colours, and the headings and rows of
// the inspector's sections.

#include "app/zone_parts.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "app/arrangement.h"
#include "app/song_view.h"
#include "app/theme.h"
#include "app/widgets.h"

namespace hokuto::app
{
namespace
{

using theme::Px;

} // namespace

namespace zone_parts
{

bool BeginZone(const char* name, bool* open)
{
    ImGuiWindowClass zone;
    zone.DockNodeFlagsOverrideSet = ImGuiDockNodeFlags_NoTabBar;
    ImGui::SetNextWindowClass(&zone);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                                        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse;
    const bool visible = ImGui::Begin(name, open, kFlags);
    ImGui::PopStyleVar();

    return visible;
}

std::optional<TrackColourPick> DrawPalette(int track, ImU32 current)
{
    std::optional<TrackColourPick> pick;
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 size(Px(kSwatchSize), Px(kSwatchSize));
    for (std::size_t i = 0; i < std::size(theme::kTrackPalette); i++)
    {
        if (i % kSwatchesPerRow != 0)
        {
            ImGui::SameLine(0.0f, Px(4.0f));
        }
        ImGui::PushID(static_cast<int>(i));
        const ImU32 colour = theme::kTrackPalette[i];
        if (ImGui::InvisibleButton("##swatch", size))
        {
            pick = TrackColourPick{track, colour};
        }
        const ImVec2 min = ImGui::GetItemRectMin();
        const ImVec2 max = ImGui::GetItemRectMax();
        draw->AddRectFilled(min, max, colour, Px(3.0f));
        if (colour == current || ImGui::IsItemHovered())
        {
            const float out = Px(2.0f);
            draw->AddRect(ImVec2(min.x - out, min.y - out), ImVec2(max.x + out, max.y + out), theme::kText, Px(4.0f),
                          Px(1.5f));
        }
        ImGui::PopID();
    }

    ImGui::Spacing();
    if (ImGui::MenuItem("Automatic"))
    {
        pick = TrackColourPick{track, std::nullopt};
    }

    return pick;
}

TrackView* SelectedTrack(SongView* song, const ArrangementState& state)
{
    if (song == nullptr || state.selected_track < 0 || state.selected_track >= static_cast<int>(song->tracks.size()))
    {
        return nullptr;
    }

    return &song->tracks[static_cast<std::size_t>(state.selected_track)];
}

bool SectionOpen(const char* title, bool open_at_first)
{
    ImGui::PushID(title);
    const bool open = ImGui::GetStateStorage()->GetBool(ImGui::GetID("open"), open_at_first);
    ImGui::PopID();

    return open;
}

float SectionHeadingHeight(const char* title)
{
    return CapsSize(title).y + Px(9.0f);
}

SectionHeading DrawSectionHeading(ImDrawList* draw, ImVec2 pos, float width, const char* title, std::string_view detail,
                                  bool open_at_first)
{
    const ImVec2 title_size = CapsSize(title);
    const float height = title_size.y + Px(6.0f);
    bool open = SectionOpen(title, open_at_first);
    ImGui::PushID(title);
    ImGui::SetCursorScreenPos(pos);
    if (ImGui::InvisibleButton("##heading", ImVec2(width, height)))
    {
        open = !open;
        ImGui::GetStateStorage()->SetBool(ImGui::GetID("open"), open);
    }
    const bool hovered = ImGui::IsItemHovered();
    ImGui::PopID();

    // The chevron and the title brighten under the mouse. The detail takes the room at the right.
    const float middle = pos.y + title_size.y * 0.5f;
    const float title_left = pos.x + Px(14.0f);
    DrawIcon(draw, open ? Icon::kChevronDown : Icon::kChevronRight, ImVec2(pos.x + Px(4.0f), middle), Px(10.0f),
             hovered ? theme::kText : theme::kTextDim);
    DrawCaps(draw, ImVec2(title_left, pos.y), hovered ? theme::kText : theme::kTextMuted, title);
    if (!detail.empty())
    {
        ImFont* font = theme::GetFonts().text;
        const float room = pos.x + width - (title_left + title_size.x + Px(12.0f));
        const std::string shown = FitText(font, theme::kSmallSize, detail, room);
        const ImVec2 size = TextSize(font, theme::kSmallSize, shown);
        DrawText(draw, font, theme::kSmallSize, ImVec2(pos.x + width - size.x, std::floor(middle - size.y * 0.5f)),
                 theme::kTextMuted, shown);
    }
    const float line = pos.y + height;
    draw->AddRectFilled(ImVec2(pos.x, line), ImVec2(pos.x + width, line + Px(1.0f)), theme::kBorder);

    return {line + Px(3.0f), open};
}

float DrawPropertyRows(ImDrawList* draw, ImVec2 pos, float width, std::span<const Property> properties)
{
    float y = pos.y;
    ImFont* text = theme::GetFonts().text;
    ImFont* figures = theme::GetFonts().figures;
    const float height = Px(kPropertyHeight);
    for (const Property& property : properties)
    {
        const bool none = property.value.empty();
        const std::string_view value = none ? std::string_view("–") : std::string_view(property.value);
        const ImVec2 label_size = TextSize(text, theme::kTextSize, property.label);
        const ImVec2 value_size = TextSize(figures, theme::kTextSize, value);
        DrawText(draw, text, theme::kTextSize, ImVec2(pos.x, y + (height - label_size.y) * 0.5f), theme::kTextMuted,
                 property.label);
        DrawText(draw, figures, theme::kTextSize,
                 ImVec2(pos.x + width - value_size.x, y + (height - value_size.y) * 0.5f),
                 none ? theme::kTextMuted : theme::kText, value);
        y += height;
    }

    return y;
}

} // namespace zone_parts
} // namespace hokuto::app
