// SPDX-License-Identifier: MIT

// The browser: the search field, the tree of the file's items with their context menus, and the Sound section.

#include "app/browser.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "app/sound_section.h"
#include "app/theme.h"
#include "app/views.h"
#include "app/widgets.h"
#include "app/zone_parts.h"

namespace hokuto::app
{
namespace
{

using theme::Px;
using zone_parts::BeginZone;
using zone_parts::DrawSectionHeading;
using zone_parts::SectionHeading;
using zone_parts::SectionHeadingHeight;
using zone_parts::SectionOpen;

// The height of an item in the browser, before the display's scale.
constexpr float kItemHeight = 28.0f;

// The browser's sizes, before the display's scale: the margin at the sides of the search field and the Sound section,
// and the gap above the section's heading. While the section is open, the tree takes a share of the room under the
// search field, and the section's rows the rest. The tree and the rows each keep a least height.
constexpr float kBrowserMargin = 10.0f;
constexpr float kSoundGap = 9.0f;
constexpr float kTreeShare = 0.4f;
constexpr float kLeastTreeHeight = 90.0f;
constexpr float kLeastSoundHeight = 90.0f;

// The browser's search field. The icon sits inside the field. Text that scrolls left while the field is typed in passes
// under the field's colour around the icon.
void DrawSearchField(BrowserState& state)
{
    const float margin = Px(kBrowserMargin);
    const float text_left = Px(30.0f);
    ImGui::SetCursorPos(ImVec2(margin, ImGui::GetCursorPosY() + margin));
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - margin);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(text_left, Px(7.0f)));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImGui::ColorConvertU32ToFloat4(theme::kPanel));
    ImGui::PushStyleColor(ImGuiCol_Border, ImGui::ColorConvertU32ToFloat4(theme::kBorderStrong));
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::InputTextWithHint("##search", "Search", state.filter.data(), state.filter.size());
    const float height = ImGui::GetItemRectSize().y;
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float border = ImGui::GetStyle().FrameBorderSize;
    draw->AddRectFilled(ImVec2(pos.x + border, pos.y + border),
                        ImVec2(pos.x + text_left - Px(2.0f), pos.y + height - border), theme::kPanel,
                        std::max(ImGui::GetStyle().FrameRounding - border, 0.0f), ImDrawFlags_RoundCornersLeft);
    DrawIcon(draw, Icon::kSearch, ImVec2(pos.x + Px(15.0f), pos.y + height * 0.5f), Px(14.0f), theme::kTextDim);
}

// What a row of the browser shows before its name: nothing, or a chevron that shows whether the item is open.
enum class Disclosure
{
    kNone,
    kClosed,
    kOpen,
};

// A row of the browser's tree: an item, or a child of one, set in a step further. A row that's playing shows the play
// icon at its end. A `grey` row's name shows dimmer. Returns whether the user clicked the row.
bool DrawItem(ImDrawList* draw, const std::string& name, ImU32 colour, bool selected, bool playing, bool child,
              Disclosure disclosure, float width, bool grey)
{
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const ImVec2 size(width, Px(kItemHeight));
    const bool clicked = ImGui::InvisibleButton("##item", size);
    const bool hovered = ImGui::IsItemHovered();
    const float margin = Px(8.0f);
    if (selected || hovered)
    {
        draw->AddRectFilled(ImVec2(pos.x + margin, pos.y), ImVec2(pos.x + size.x - margin, pos.y + size.y),
                            selected ? theme::kControl : theme::kRaisedHover, Px(6.0f));
    }
    if (selected)
    {
        draw->AddRectFilled(ImVec2(pos.x + margin, pos.y + Px(7.0f)),
                            ImVec2(pos.x + margin + Px(3.0f), pos.y + size.y - Px(7.0f)), colour, Px(1.5f));
    }

    // The chevron of an item with children, the name, and the play icon of a row that's playing.
    const float middle = pos.y + size.y * 0.5f;
    if (disclosure != Disclosure::kNone)
    {
        DrawIcon(draw, disclosure == Disclosure::kOpen ? Icon::kChevronDown : Icon::kChevronRight,
                 ImVec2(pos.x + Px(40.0f), middle), Px(11.0f), theme::kTextDim);
    }
    // A long name ends in an ellipsis before the row's end, or before the play icon.
    ImFont* font = theme::GetFonts().text;
    const float name_left = pos.x + Px(child ? 72.0f : 56.0f);
    const float name_right = pos.x + size.x - margin - Px(playing ? 28.0f : 8.0f);
    const std::string shown = FitText(font, theme::kTextSize, name, name_right - name_left);
    const ImVec2 text_size = TextSize(font, theme::kTextSize, shown);
    const ImU32 bright = grey ? theme::kTextMuted : theme::kText;
    const ImU32 text = selected ? bright : grey ? theme::kTextDim : theme::kTextMuted;
    DrawText(draw, font, theme::kTextSize, ImVec2(name_left, middle - text_size.y * 0.5f), text, shown);
    if (playing)
    {
        DrawIcon(draw, Icon::kPlay, ImVec2(pos.x + size.x - margin - Px(16.0f), middle), Px(11.0f), colour);
    }

    return clicked;
}

// The rows of a group that match the search, the items that the search opened, and the number of items that match.
struct GroupRows
{
    std::vector<BrowserPlace> rows;
    std::set<int> found_open;
    std::size_t matches = 0;
};

// The rows of `group`, number `index`, that match the search in `filter`.
GroupRows MatchingRows(const BrowserGroup& group, int index, std::string_view filter, const BrowserState& state)
{
    // The rows of the items that match the search, each followed by its children while it's open. The search also finds
    // an item by the names of its children where they name items, such as a wave sound file's wave sounds. The item
    // then shows those children, open while the search finds them.
    std::vector<BrowserPlace> rows;
    std::set<int> found_open;
    std::size_t matches = 0;
    for (std::size_t i = 0; i < group.items.size(); i++)
    {
        const BrowserItem& item = group.items[i];
        const auto item_index = static_cast<int>(i);
        std::vector<int> found;
        if (!filter.empty() && item.children_found)
        {
            for (std::size_t child = 0; child < item.children.size(); child++)
            {
                if (Matches(item.children[child], filter))
                {
                    found.push_back(static_cast<int>(child));
                }
            }
        }
        if (!Matches(item.name, filter) && found.empty())
        {
            continue;
        }

        matches++;
        rows.push_back({index, item_index, -1});
        if (state.open.contains({index, item_index}))
        {
            for (std::size_t child = 0; child < item.children.size(); child++)
            {
                rows.push_back({index, item_index, static_cast<int>(child)});
            }
        }
        else if (!found.empty())
        {
            found_open.insert(item_index);
            for (const int child : found)
            {
                rows.push_back({index, item_index, child});
            }
        }
    }

    return {std::move(rows), std::move(found_open), matches};
}

// Draws the row of `group`, `width` wide, with `matches` items. A click on the row opens and closes the group. Returns
// whether the group is open.
bool DrawGroupHeader(ImDrawList* draw, const BrowserGroup& group, std::size_t matches, float width)
{
    bool& expanded = *ImGui::GetStateStorage()->GetBoolRef(ImGui::GetID("expanded"), true);

    // The group's row: a chevron, the icon, the name, and the number of items on the right.
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const ImVec2 size(width, Px(30.0f));
    if (ImGui::InvisibleButton("##group", size))
    {
        expanded = !expanded;
    }
    const float margin = Px(8.0f);
    if (ImGui::IsItemHovered())
    {
        draw->AddRectFilled(ImVec2(pos.x + margin, pos.y), ImVec2(pos.x + size.x - margin, pos.y + size.y),
                            theme::kRaisedHover, Px(6.0f));
    }
    const float middle = pos.y + size.y * 0.5f;
    DrawIcon(draw, expanded ? Icon::kChevronDown : Icon::kChevronRight, ImVec2(pos.x + Px(21.0f), middle), Px(12.0f),
             theme::kTextDim);
    DrawIcon(draw, group.icon, ImVec2(pos.x + Px(40.0f), middle), Px(15.0f), group.colour);
    ImFont* font = theme::GetFonts().text;
    const ImVec2 name_size = TextSize(font, theme::kTextSize, group.name);
    DrawText(draw, font, theme::kTextSize, ImVec2(pos.x + Px(56.0f), middle - name_size.y * 0.5f), theme::kText,
             group.name);

    std::array<char, 16> count{};
    std::snprintf(count.data(), count.size(), "%zu", matches);
    const ImVec2 count_size = TextSize(font, theme::kSmallSize, count.data());
    const float pill_right = pos.x + size.x - margin - Px(8.0f);
    const float pill_left = pill_right - count_size.x - Px(14.0f);
    draw->AddRectFilled(ImVec2(pill_left, middle - Px(9.0f)), ImVec2(pill_right, middle + Px(9.0f)), theme::kControl,
                        Px(9.0f));
    DrawText(draw, font, theme::kSmallSize, ImVec2(pill_left + Px(7.0f), middle - count_size.y * 0.5f),
             theme::kTextMuted, count.data());

    return expanded;
}

// The menu of the row at `place`, of `item` or one of its children: the `editor` command, enabled when `edits` is true,
// and Export… and Replace…. A `grey` row has none of them.
void DrawItemMenu(const BrowserGroup& group, const BrowserItem& item, const BrowserPlace& place, BrowserState& state,
                  const std::string& editor, bool edits, bool grey)
{
    const bool child = place.child >= 0;
    const bool exports = !child || item.children_export;
    if (!exports && editor.empty())
    {
        return;
    }
    if (!ImGui::BeginPopupContextItem("##menu"))
    {
        return;
    }

    PushMenuSpacing();
    if (ImGui::IsWindowAppearing())
    {
        state.selected = place;
    }
    if (!editor.empty() && ImGui::MenuItem(editor.c_str(), nullptr, false, edits))
    {
        state.edit_place = place;
    }
    if (!editor.empty() && !edits)
    {
        ItemTooltip(grey ? item.missing : item.edit_note);
    }
    if (!editor.empty() && exports)
    {
        ImGui::Separator();
    }

    // A greyed Export… or Replace… says why when the item gives a reason.
    if (exports)
    {
        if (ImGui::MenuItem("Export…", nullptr, false, (child || item.exports) && !grey))
        {
            state.export_place = place;
        }
        if (grey || (!child && !item.exports))
        {
            ItemTooltip(grey ? item.missing : item.export_note);
        }
        const bool replaces = (child ? item.children_replace : item.replaces) && !grey;
        if (ImGui::MenuItem("Replace…", nullptr, false, replaces))
        {
            state.replace_place = place;
        }
        if (grey || (!child && !item.replaces && !item.replace_note.empty()))
        {
            ItemTooltip(grey ? item.missing : item.replace_note);
        }
        else if (child && !replaces)
        {
            const std::string& note =
                !item.replaces && !item.replace_note.empty() ? item.replace_note : group.children_replace_note;
            if (!note.empty())
            {
                ItemTooltip(note);
            }
        }
    }
    ImGui::PopStyleVar();
    ImGui::EndPopup();
}

// One row of `group`, number `index`, at `place`: an item or one of its children, `width` wide. The search opened the
// items in `found_open`.
void DrawGroupRow(ImDrawList* draw, const BrowserGroup& group, int index, const BrowserPlace& place,
                  const std::set<int>& found_open, BrowserState& state, float width)
{
    const BrowserItem& item = group.items[static_cast<std::size_t>(place.item)];
    const std::pair<int, int> key(index, place.item);
    const bool child = place.child >= 0;
    Disclosure disclosure = Disclosure::kNone;
    if (!child && !item.children.empty())
    {
        disclosure =
            state.open.contains(key) || found_open.contains(place.item) ? Disclosure::kOpen : Disclosure::kClosed;
    }
    const std::string& name = child ? item.children[static_cast<std::size_t>(place.child)] : item.name;
    ImGui::PushID(place.item);
    ImGui::PushID(place.child);
    const bool grey = !item.missing.empty();
    const bool clicked = DrawItem(draw, name, group.colour, state.selected == place, state.playing == place, child,
                                  disclosure, width, grey);
    if (grey)
    {
        ItemTooltip(item.missing);
    }
    if (clicked)
    {
        state.selected = place;
        state.clicked = (child || item.plays) && !grey;
        if (disclosure == Disclosure::kOpen && !found_open.contains(place.item))
        {
            state.open.erase(key);
        }
        else if (disclosure == Disclosure::kClosed)
        {
            state.open.insert(key);
        }
    }

    // A double-click opens the row in its editor. Its second click doesn't play the row again.
    const std::string& editor = child ? item.children_edit : item.edit;
    const bool edits = (child || item.edits) && !grey;
    if (!editor.empty() && edits && ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
    {
        state.edit_place = place;
        state.clicked = false;
    }

    // A right-click opens the row's menu and selects the row. Another row's menu can be open at the click. It
    // keeps the click from the row until the button comes up and the new menu opens.
    DrawItemMenu(group, item, place, state, editor, edits, grey);

    // A wave drags to the Bank Editor, with its name beside the pointer.
    if (child && item.children_drag && !grey && ImGui::BeginDragDropSource())
    {
        ImGui::SetDragDropPayload(kWavePayload, &place, sizeof(place));
        ImGui::TextUnformatted(name.c_str());
        ImGui::EndDragDropSource();
    }
    ImGui::PopID();
    ImGui::PopID();
}

// One group of the browser's tree: a row that opens and closes it, then its items. While `reveal` is set, the tree
// scrolls the selected row into view.
void DrawGroup(ImDrawList* draw, const BrowserGroup& group, int index, BrowserState& state, float width, bool reveal)
{
    const std::string_view filter(state.filter.data());
    const auto [rows, found_open, matches] = MatchingRows(group, index, filter, state);

    // The group's row, then its rows while it's open.
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const bool expanded = DrawGroupHeader(draw, group, matches, width);
    if (!expanded)
    {
        return;
    }

    // The rows, or a line that says there are none. Items can share a name. Their indexes distinguish them. An archive
    // can hold thousands of items, and a wave archive thousands of waves. Only the rows in view are drawn. A click on
    // an item with children opens or closes it. A click on a child plays it, or stops it, and so does a click on an
    // item that plays.
    if (matches == 0)
    {
        const ImVec2 line = ImGui::GetCursorScreenPos();
        ImFont* font = theme::GetFonts().text;
        DrawText(draw, font, theme::kSmallSize, ImVec2(line.x + Px(56.0f), line.y + Px(4.0f)), theme::kTextMuted,
                 filter.empty() ? "Empty" : "No matches");
        ImGui::Dummy(ImVec2(width, Px(24.0f)));
    }
    const float step = Px(kItemHeight) + ImGui::GetStyle().ItemSpacing.y;
    // std::find of a BrowserPlace fails to compile under clang-cl with Microsoft's STL 14.51 (microsoft/STL#6294).
    const auto selected =
        std::find_if(rows.begin(), rows.end(), [&state](const BrowserPlace& row) { return row == state.selected; });
    if (reveal && selected != rows.end())
    {
        const float top = ImGui::GetCursorScreenPos().y + static_cast<float>(selected - rows.begin()) * step;
        ImGui::ScrollToRect(ImGui::GetCurrentWindow(), ImRect(pos.x, top, pos.x + width, top + Px(kItemHeight)),
                            ImGuiScrollFlags_KeepVisibleEdgeY);
    }
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(rows.size()), step);
    while (clipper.Step())
    {
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++)
        {
            DrawGroupRow(draw, group, index, rows[static_cast<std::size_t>(row)], found_open, state, width);
        }
    }
}

// The height of the browser's tree, from the room for the tree and the Sound section's rows. While the section is open,
// the tree takes its share of the room, and the tree and the rows keep their least heights where the room allows. A
// drag of the line between them sets the share (DrawTreeDivider). Otherwise the tree takes the whole room.
float TreeHeight(float room, bool open)
{
    if (!open)
    {
        return room;
    }

    const float share = ImGui::GetStateStorage()->GetFloat(ImGui::GetID("tree share"), kTreeShare);
    const float least = std::min(Px(kLeastTreeHeight), room);
    const float most = std::max(room - Px(kLeastSoundHeight), least);
    return std::clamp(std::floor(room * share), least, most);
}

// Draws the line under the browser's tree, `width` wide at `pos`. While `drags` is set, a drag of the line moves it
// with the mouse, and sets the tree's share of the room `room` tall from `top`.
void DrawTreeDivider(ImDrawList* draw, ImVec2 pos, float width, float top, float room, bool drags)
{
    if (drags)
    {
        ImGui::SetCursorScreenPos(pos);
        ImGui::InvisibleButton("##divider", ImVec2(width, Px(kSoundGap)));
        ImGuiStorage* storage = ImGui::GetStateStorage();
        const ImGuiID from = ImGui::GetID("drag from");
        if (ImGui::IsItemActivated())
        {
            storage->SetFloat(from, pos.y - top);
        }
        if (ImGui::IsItemActive() && room > 0.0f)
        {
            const float tree = storage->GetFloat(from) + ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0.0f).y;
            storage->SetFloat(ImGui::GetID("tree share"), std::clamp(tree / room, 0.0f, 1.0f));
        }
        if (ImGui::IsItemHovered() || ImGui::IsItemActive())
        {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
        }
    }
    draw->AddRectFilled(ImVec2(pos.x, pos.y + Px(2.0f)), ImVec2(pos.x + width, pos.y + Px(3.0f)), theme::kBorder);
}

// Draws the browser's Sound section from `pos` under the tree down to the bottom of the zone, `width` wide: a heading
// that names the selected sound, and the sound's values under it while the section is open. The values scroll when
// they're taller than the room. `open` says whether the section was open when the tree took its height. Returns whether
// the section is open after a click on its heading.
bool DrawBrowserSound(ImVec2 pos, float width, bool open, SoundSection& sound)
{
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float margin = Px(kBrowserMargin);
    const std::string_view name = sound.view ? std::string_view(sound.view->values.name) : std::string_view();
    const SectionHeading heading =
        DrawSectionHeading(draw, ImVec2(pos.x + margin, pos.y), width - margin * 2.0f, "Sound", name, false);
    if (!open || !heading.open)
    {
        return heading.open;
    }

    // The values scroll under the heading.
    ImGui::SetCursorScreenPos(ImVec2(pos.x, heading.bottom));
    ImGui::BeginChild("##sound", ImVec2(0.0f, 0.0f));
    ImDrawList* rows = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float rows_width = ImGui::GetContentRegionAvail().x - margin * 2.0f;
    float bottom = origin.y;
    if (sound.view)
    {
        bottom = DrawSoundValues(rows, ImVec2(origin.x + margin, origin.y), rows_width, sound);
    }
    else
    {
        // Some files contain no values for a sound: one from before the archive's tables came into the format, and one
        // of DSE or Konami's driver. A sound of the GBA or the DS whose values Hokuto can't read shows none either.
        ImFont* font = theme::GetFonts().text;
        const char* text = sound.sound ? "The file has no values for this sound" : "No sound is selected";
        const ImVec2 size = TextSize(font, theme::kTextSize, text);
        DrawText(rows, font, theme::kTextSize,
                 ImVec2(origin.x + margin + (rows_width - size.x) * 0.5f, origin.y + Px(8.0f)), theme::kTextMuted,
                 text);
        bottom = origin.y + Px(8.0f) + size.y;
    }

    // The rows and the margin below them set the height that scrolls.
    ImGui::SetCursorScreenPos(ImVec2(origin.x, bottom));
    ImGui::Dummy(ImVec2(rows_width, margin));
    ImGui::EndChild();

    return true;
}

} // namespace

void DrawBrowser(const std::vector<BrowserGroup>& groups, std::string_view file, BrowserState& state,
                 SoundSection& sound, bool* open)
{
    if (!*open)
    {
        return;
    }

    if (BeginZone(kBrowser, open))
    {
        ZoneHeader("Browser", file);
        DrawSearchField(state);

        // The tree scrolls under the search field. The Sound section sits under the tree. While the section is closed,
        // its heading sits at the bottom of the zone.
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + Px(2.0f));
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        const bool sound_open = SectionOpen("Sound", false);
        const float room = avail.y - Px(kSoundGap) - SectionHeadingHeight("Sound") - Px(kBrowserMargin) * 0.5f;
        const float tree = TreeHeight(room, sound_open);

        // A tree that the section's opening shortened scrolls the selected row into view, and so does a tree whose
        // selection the program set.
        ImGuiStorage* storage = ImGui::GetStateStorage();
        const ImGuiID reveal = ImGui::GetID("reveal");
        const bool chosen = std::exchange(state.reveal, false);
        const bool revealing = storage->GetBool(reveal, false) || chosen;
        storage->SetBool(reveal, false);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, Px(1.0f)));
        if (std::exchange(state.top, false))
        {
            ImGui::SetNextWindowScroll(ImVec2(0.0f, 0.0f));
        }
        ImGui::BeginChild("##tree", ImVec2(0.0f, std::max(tree, 1.0f)));
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const float width = ImGui::GetContentRegionAvail().x;
        for (std::size_t i = 0; i < groups.size(); i++)
        {
            if (groups[i].hidden)
            {
                continue;
            }

            ImGui::PushID(static_cast<int>(i));
            DrawGroup(draw, groups[i], static_cast<int>(i), state, width, revealing);
            ImGui::PopID();
        }
        ImGui::EndChild();
        ImGui::PopStyleVar();

        // The section sits under the line below the tree. Its opening shortens the tree in the next frame.
        const ImVec2 divider(origin.x, origin.y + tree);
        DrawTreeDivider(ImGui::GetWindowDrawList(), divider, avail.x, origin.y, room, sound_open);
        if (DrawBrowserSound(ImVec2(origin.x, divider.y + Px(kSoundGap)), avail.x, sound_open, sound) && !sound_open)
        {
            storage->SetBool(reveal, true);
        }
    }
    ImGui::End();
}

} // namespace hokuto::app
