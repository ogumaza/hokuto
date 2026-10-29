// SPDX-License-Identifier: MIT

#include "app/views.h"

#include <imgui_internal.h>

#include <algorithm>
#include <array>
#include <cfloat>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "app/markers.h"
#include "app/tempo_row.h"
#include "app/theme.h"
#include "format/kinds.h"

namespace hokuto::app
{
namespace
{

using theme::Px;

// The arrangement's sizes, before the display's scale. The marker row lies under the ruler. A drag of the track list's
// edge takes hold in a band kEdgeGrab wide, centred on the edge.
constexpr float kEdgeGrab = 6.0f;
constexpr float kRulerHeight = 30.0f;
constexpr float kMarkerHeight = 22.0f;
constexpr float kMinBeatWidth = 2.0f;
constexpr float kMaxBeatWidth = 160.0f;
constexpr float kZoomStep = 1.25f;
constexpr float kWheelStep = 60.0f;

// The height of an item in the browser, before the display's scale.
constexpr float kItemHeight = 28.0f;

// The heights that a vertical zoom steps through. A track shorter than kTwoLineHeight shows its name without the bank
// and program. A track shorter than kThreeLineHeight shows them on one line under the name, and a taller one on two.
constexpr std::array<float, 9> kTrackHeights = {30.0f, 38.0f, 46.0f, 54.0f, 66.0f, 80.0f, 98.0f, 120.0f, 148.0f};
constexpr float kTwoLineHeight = 46.0f;
constexpr float kThreeLineHeight = 62.0f;

// The width of a track's colour strip, and of the strip that a click on it takes while the tracks' colours are Custom.
constexpr float kColourStripWidth = 4.0f;
constexpr float kColourStripGrab = 10.0f;

// The palette's swatches, and how many sit in a row. The popup has a name for each window that shows it.
constexpr float kSwatchSize = 20.0f;
constexpr std::size_t kSwatchesPerRow = 9;
constexpr char kColourPopup[] = "##track colour";

// The marker row's menu, and the most commands that the tooltip of a place where the tracks read variables or branch
// lists.
constexpr char kMarkerMenu[] = "##markers";
constexpr std::size_t kMostMarkerLines = 12;

// The tempo row's sizes, before the display's scale: the row's height, the room above and below its line, and the
// distance within which a point takes the mouse. A drag changes a point's tempo by the row's span over the line's room
// for each point of travel, and by a tenth of that with Shift.
constexpr float kTempoHeight = 56.0f;
constexpr float kTempoMargin = 8.0f;
constexpr float kTempoGrab = 7.0f;
constexpr double kFineTempoDrag = 0.1;
constexpr float kTempoMoveDistance = 3.0f;

// The name of the Alt key on this system, and the menu that holds Preferences. The Mac calls the key Option, and puts
// Preferences in the application menu.
#ifdef __APPLE__
constexpr char kAltKey[] = "Option";
constexpr char kPreferencesItem[] = "Hokuto > Preferences…";
#else
constexpr char kAltKey[] = "Alt";
constexpr char kPreferencesItem[] = "Edit > Preferences…";
#endif

// The track list's context menu, and the names of its actions in the order of TrackAction.
constexpr char kTrackMenu[] = "##tracks";
constexpr std::array<const char*, kTrackActions> kTrackActionNames = {
    "Add Track", "Duplicate Tracks", "Remove Selected Tracks", "Copy Track", "Paste Track"};

// The widths of the zones at the sides, and the inspector's sizes, before the display's scale.
constexpr float kInspectorWidth = 232.0f;
constexpr float kRightZoneWidth = 288.0f;
constexpr float kInspectorMargin = 12.0f;
constexpr float kPropertyHeight = 26.0f;

// The sizes of the inspector's menus of a track's bank and program, before the display's scale: the width of the
// program's label, the gap round a menu, and the least width of the menus' lists. The Sound section's rows take the
// same sizes.
constexpr float kInstrumentLabelWidth = 84.0f;
constexpr float kInstrumentGap = 6.0f;
constexpr float kInstrumentListWidth = 240.0f;

// The least height of the inspector's channel strips, before the display's scale. The strips show their controls at
// that height.
constexpr float kLeastStripHeight = 340.0f;

// The browser's sizes, before the display's scale: the margin at the sides of the search field and the Sound section,
// and the gap above the section's heading. While the section is open, the tree takes a share of the room under the
// search field, and the section's rows the rest. The tree and the rows each keep a least height.
constexpr float kBrowserMargin = 10.0f;
constexpr float kSoundGap = 9.0f;
constexpr float kTreeShare = 0.4f;
constexpr float kLeastTreeHeight = 90.0f;
constexpr float kLeastSoundHeight = 90.0f;

// The mixer's sizes, before the display's scale.
constexpr float kStripWidth = 84.0f;
constexpr float kMasterWidth = 96.0f;
constexpr float kFxStripWidth = 104.0f;
constexpr float kInsertHeight = 24.0f;

// The mixing switch in the mixer's header, before the display's scale: its size, and its gap after the title. The
// header is 32 high.
constexpr float kMixingSwitchWidth = 172.0f;
constexpr float kMixingSwitchHeight = 24.0f;
constexpr float kMixingSwitchGap = 18.0f;
constexpr float kEngineSwitchWidth = 236.0f;
constexpr float kHeaderHeight = 32.0f;

// Where a strip's level and peak sit below its top, under the sends, the pan, and M and S. The fader follows them.
constexpr float kLevelsTop = 151.0f;

// The Undo History's sizes, before the display's scale: a row of two lines, and the room above the first row. The line
// lies there when no action is done.
constexpr float kHistoryRowHeight = 42.0f;
constexpr float kHistoryTop = 10.0f;

// The Variables zone's sizes, before the display's scale: a variable's row, the height of its switches, the header of
// the scenes or the changes, a scene's row and a change's row. The field of a change's time is as wide as a time.
constexpr float kVariableRowHeight = 70.0f;
constexpr float kSwitchHeight = 26.0f;
constexpr float kScenesHeaderHeight = 36.0f;
constexpr float kSceneRowHeight = 28.0f;
constexpr float kChangeRowHeight = 76.0f;
constexpr float kTimeFieldWidth = 104.0f;

// nn::atk numbers the global variables from 16. Every sequence that plays shares them.
constexpr int kFirstGlobalVariable = 16;

// The range of the slider of a variable without choices: from -1, the value that a variable starts with, to 127. On the
// Wii, a global variable starts at 0. A typed value can be any value that a variable holds: 16 bits, with a sign.
constexpr SliderRange kAnyValue = {-1.0f, 127.0f, false};
constexpr SliderRange kTypedValue = {-32768.0f, 32767.0f, false};

// The length of the empty song that the arrangement shows with no song open, in bars.
constexpr int kEmptyBars = 64;

// Begins a zone: a docked window with no tab and no padding. End() must follow in every case.
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

// One group of the browser's tree: a row that opens and closes it, then its items. While `reveal` is set, the tree
// scrolls the selected row into view.
void DrawGroup(ImDrawList* draw, const BrowserGroup& group, int index, BrowserState& state, float width, bool reveal)
{
    // The rows of the items that match the search, each followed by its children while it's open. The search also finds
    // an item by the names of its children where they name items, such as a wave sound file's wave sounds. The item
    // then shows those children, open while the search finds them.
    const std::string_view filter(state.filter.data());
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
            const BrowserPlace& place = rows[static_cast<std::size_t>(row)];
            const BrowserItem& item = group.items[static_cast<std::size_t>(place.item)];
            const std::pair<int, int> key(index, place.item);
            const bool child = place.child >= 0;
            Disclosure disclosure = Disclosure::kNone;
            if (!child && !item.children.empty())
            {
                disclosure = state.open.contains(key) || found_open.contains(place.item) ? Disclosure::kOpen
                                                                                         : Disclosure::kClosed;
            }
            const std::string& name = child ? item.children[static_cast<std::size_t>(place.child)] : item.name;
            ImGui::PushID(place.item);
            ImGui::PushID(place.child);
            const bool grey = !item.missing.empty();
            const bool clicked = DrawItem(draw, name, group.colour, state.selected == place, state.playing == place,
                                          child, disclosure, width, grey);
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
            if (!editor.empty() && edits && ImGui::IsItemHovered() &&
                ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
            {
                state.edit_place = place;
                state.clicked = false;
            }

            // A right-click opens the row's menu and selects the row. Another row's menu can be open at the click. It
            // keeps the click from the row until the button comes up and the new menu opens.
            const bool exports = !child || item.children_export;
            if (exports || !editor.empty())
            {
                if (ImGui::BeginPopupContextItem("##menu"))
                {
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
                    }
                    ImGui::PopStyleVar();
                    ImGui::EndPopup();
                }
            }

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
    }
}

// Where the arrangement's parts are on the screen, and its scale. The headers, the ruler and the rows under it stay put
// while the lanes scroll under them. `tempo` is empty while the tempo row is hidden.
struct Canvas
{
    ImRect ruler;
    ImRect markers;
    ImRect tempo;
    ImRect headers;
    ImRect lanes;
    float beat_width = 0.0f;
    float row_height = 0.0f;
    float scroll_x = 0.0f;
    float scroll_y = 0.0f;
    int beats_per_bar = 4;
    int ticks_per_quarter = 48;
};

float XOf(const Canvas& canvas, double quarters)
{
    return canvas.lanes.Min.x + static_cast<float>(quarters) * canvas.beat_width - canvas.scroll_x;
}

double QuartersAt(const Canvas& canvas, float x)
{
    return (x - canvas.lanes.Min.x + canvas.scroll_x) / canvas.beat_width;
}

float RowTop(const Canvas& canvas, std::size_t track)
{
    return canvas.lanes.Min.y + static_cast<float>(track) * canvas.row_height - canvas.scroll_y;
}

// The height in kTrackHeights that lies `steps` steps above `height`, or below it for a negative `steps`.
float StepTrackHeight(float height, int steps)
{
    std::size_t nearest = 0;
    for (std::size_t i = 0; i < kTrackHeights.size(); i++)
    {
        if (std::abs(kTrackHeights[i] - height) < std::abs(kTrackHeights[nearest] - height))
        {
            nearest = i;
        }
    }

    const int index = std::clamp(static_cast<int>(nearest) + steps, 0, static_cast<int>(kTrackHeights.size()) - 1);
    return kTrackHeights[static_cast<std::size_t>(index)];
}

// A track's bank and program in one line of text. The track's header shows that line.
std::string InstrumentText(const TrackView& track)
{
    const std::string program = track.program < 0 ? std::string() : "Program " + std::to_string(track.program);
    if (track.bank.empty() || program.empty())
    {
        return track.bank.empty() && program.empty() ? std::string("No instrument") : track.bank + program;
    }

    return track.bank + " · " + program;
}

// A track's bank and program, each on a line of the track's header.
std::vector<std::string> InstrumentLines(const TrackView& track)
{
    std::vector<std::string> lines;
    if (!track.bank.empty())
    {
        lines.push_back(track.bank);
    }
    if (track.program >= 0)
    {
        lines.push_back("Program " + std::to_string(track.program));
    }
    if (lines.empty())
    {
        lines.emplace_back("No instrument");
    }

    return lines;
}

// The palette of the tracks' colours, in a popup that the caller has begun: a swatch for each colour of
// theme::kTrackPalette, then Automatic. The swatch of `current` is marked. Returns the colour that the user picked for
// `track`, or nothing. Automatic picks the track's automatic colour.
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

// The number of bars from one label on the ruler to the next. The labels then stay apart.
int BarStep(const Canvas& canvas)
{
    const float bar_width = canvas.beat_width * static_cast<float>(canvas.beats_per_bar);
    int step = 1;
    while (bar_width * static_cast<float>(step) < Px(44.0f))
    {
        step *= 2;
    }

    return step;
}

// Zooms in or out by `factor`. The time at `anchor_x` stays where it is on the screen.
void Zoom(Canvas& canvas, ArrangementState& state, float factor, float anchor_x)
{
    const double anchor = QuartersAt(canvas, anchor_x);
    state.beat_width = std::clamp(state.beat_width * factor, kMinBeatWidth, kMaxBeatWidth);
    canvas.beat_width = Px(state.beat_width);
    canvas.scroll_x = std::max(static_cast<float>(anchor) * canvas.beat_width - (anchor_x - canvas.lanes.Min.x), 0.0f);
    ImGui::SetScrollX(canvas.scroll_x);
}

// The mouse in the arrangement. The wheel scrolls. With Shift, the wheel scrolls sideways. Command or Ctrl with the
// wheel zooms around the mouse. The ruler moves the position and sets the cycle.
void HandleMouse(Canvas& canvas, const SongView* song, Transport& transport, ArrangementState& state)
{
    const ImGuiIO& io = ImGui::GetIO();
    const bool hovered = ImGui::IsWindowHovered();
    if (hovered && io.KeyCtrl && io.MouseWheel != 0.0f)
    {
        Zoom(canvas, state, std::pow(kZoomStep, io.MouseWheel), io.MousePos.x);
    }
    else if (hovered && (io.MouseWheel != 0.0f || io.MouseWheelH != 0.0f))
    {
        // macOS turns the wheel with Shift into the horizontal wheel. Other systems leave it vertical.
        const float sideways = io.KeyShift ? io.MouseWheel + io.MouseWheelH : io.MouseWheelH;
        const float down = io.KeyShift ? 0.0f : io.MouseWheel;
        ImGui::SetScrollX(canvas.scroll_x - sideways * Px(kWheelStep));
        ImGui::SetScrollY(canvas.scroll_y - down * Px(kWheelStep));
    }

    // The ruler, with the song's loop for the locators to snap to. The mouse shows where a press grabs a locator.
    RulerGeometry geometry;
    geometry.origin = canvas.lanes.Min.x - canvas.scroll_x;
    geometry.beat_width = canvas.beat_width;
    geometry.middle = canvas.ruler.GetCenter().y;
    geometry.grab = Px(kLocatorGrab);
    geometry.snap = Px(kLocatorSnap);
    geometry.drag = Px(kRangeDragDistance);
    if (song != nullptr && song->loop_start >= 0)
    {
        geometry.loop_start = static_cast<double>(song->loop_start) / song->ticks_per_quarter;
        geometry.loop_end = static_cast<double>(song->loop_end) / song->ticks_per_quarter;
    }
    RulerMouse mouse;
    mouse.x = io.MousePos.x;
    mouse.y = io.MousePos.y;
    mouse.over = hovered && canvas.ruler.Contains(io.MousePos);
    mouse.pressed = mouse.over && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !state.resizing_track_list;
    mouse.down = ImGui::IsMouseDown(ImGuiMouseButton_Left);
    mouse.command = io.KeyCtrl;
    mouse.option = io.KeyAlt;
    if (state.ruler.Update(geometry, mouse, transport))
    {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    }
}

// A line at each bar, and at each beat when the beats are far enough apart. Zoomed far out, bars thin out too.
void DrawBarGrid(ImDrawList* draw, const Canvas& canvas, int first, int last)
{
    const bool beats = canvas.beat_width >= Px(9.0f);
    const int bar_step =
        canvas.beat_width * static_cast<float>(canvas.beats_per_bar) >= Px(10.0f) ? 1 : BarStep(canvas);
    for (int quarter = first; quarter <= last; quarter++)
    {
        const bool bar = quarter % canvas.beats_per_bar == 0;
        if (bar ? quarter / canvas.beats_per_bar % bar_step != 0 : !beats)
        {
            continue;
        }

        const float x = std::floor(XOf(canvas, quarter));
        draw->AddRectFilled(ImVec2(x, canvas.lanes.Min.y), ImVec2(x + Px(1.0f), canvas.lanes.Max.y),
                            bar ? theme::kGridBar : theme::kGridBeat);
    }
}

// A line at each beat, or at each labelled beat when the beats are close together. No line stands out. Nothing says
// where a bar starts.
void DrawBeatGrid(ImDrawList* draw, const Canvas& canvas, int first, int last)
{
    const bool every_beat = canvas.beat_width >= Px(9.0f);
    const int step = BeatStep(canvas.beat_width);
    for (int quarter = first; quarter <= last; quarter++)
    {
        if (!every_beat && !LabelledBeat(quarter, step))
        {
            continue;
        }

        const float x = std::floor(XOf(canvas, quarter));
        draw->AddRectFilled(ImVec2(x, canvas.lanes.Min.y), ImVec2(x + Px(1.0f), canvas.lanes.Max.y),
                            theme::kGridBeatOnly);
    }
}

void DrawGrid(ImDrawList* draw, const Canvas& canvas)
{
    const int first = std::max(static_cast<int>(std::floor(QuartersAt(canvas, canvas.lanes.Min.x))), 0);
    const int last = static_cast<int>(std::ceil(QuartersAt(canvas, canvas.lanes.Max.x)));
    if (kShowBars)
    {
        DrawBarGrid(draw, canvas, first, last);
    }
    else
    {
        DrawBeatGrid(draw, canvas, first, last);
    }
}

// A long mark on the ruler at `x`, with `number` beside it.
void DrawRulerLabel(ImDrawList* draw, const ImRect& ruler, float x, int number)
{
    std::array<char, 16> label{};
    std::snprintf(label.data(), label.size(), "%d", number);
    draw->AddRectFilled(ImVec2(x, ruler.Max.y - Px(13.0f)), ImVec2(x + Px(1.0f), ruler.Max.y), theme::kTextDim);
    DrawText(draw, theme::GetFonts().figures, theme::kSmallSize, ImVec2(x + Px(4.0f), ruler.Max.y - Px(16.0f)),
             theme::kTextMuted, label.data());
}

// A short mark on the ruler at `x`, `height` points tall.
void DrawRulerMark(ImDrawList* draw, const ImRect& ruler, float x, float height)
{
    draw->AddRectFilled(ImVec2(x, ruler.Max.y - Px(height)), ImVec2(x + Px(1.0f), ruler.Max.y), theme::kBorderStrong);
}

// A number and a long mark at each labelled bar, and short marks at the beats.
void DrawBarMarks(ImDrawList* draw, const Canvas& canvas)
{
    const ImRect& ruler = canvas.ruler;
    const int beats = canvas.beats_per_bar;
    const int step = BarStep(canvas);
    const int first = std::max(static_cast<int>(std::floor(QuartersAt(canvas, ruler.Min.x) / beats)) - 1, 0);
    const int last = static_cast<int>(std::ceil(QuartersAt(canvas, ruler.Max.x) / beats));
    for (int bar = first; bar <= last; bar++)
    {
        const float x = std::floor(XOf(canvas, static_cast<double>(bar) * beats));
        if (bar % step == 0)
        {
            DrawRulerLabel(draw, ruler, x, bar + 1);
        }
        else
        {
            DrawRulerMark(draw, ruler, x, 6.0f);
        }
        for (int beat = 1; beat < beats && canvas.beat_width >= Px(9.0f); beat++)
        {
            DrawRulerMark(draw, ruler, std::floor(XOf(canvas, static_cast<double>(bar) * beats + beat)), 4.0f);
        }
    }
}

// A number and a long mark at each labelled beat, and short marks at the others when they're far enough apart.
void DrawBeatMarks(ImDrawList* draw, const Canvas& canvas)
{
    const ImRect& ruler = canvas.ruler;
    const int step = BeatStep(canvas.beat_width);
    const int first = std::max(static_cast<int>(std::floor(QuartersAt(canvas, ruler.Min.x))) - 1, 0);
    const int last = static_cast<int>(std::ceil(QuartersAt(canvas, ruler.Max.x)));
    for (int quarter = first; quarter <= last; quarter++)
    {
        const float x = std::floor(XOf(canvas, quarter));
        if (LabelledBeat(quarter, step))
        {
            DrawRulerLabel(draw, ruler, x, quarter + 1);
        }
        else if (canvas.beat_width >= Px(4.0f))
        {
            DrawRulerMark(draw, ruler, x, 5.0f);
        }
    }
}

void DrawRuler(ImDrawList* draw, const Canvas& canvas, const Transport& transport)
{
    const ImRect& ruler = canvas.ruler;
    draw->AddRectFilled(ruler.Min, ruler.Max, theme::kRaised);

    // The range between the locators, in the ruler's upper half. The range lights up while the cycle is on.
    const ImU32 range =
        transport.Cycle() ? theme::WithAlpha(theme::kCycle, 0.9f) : theme::WithAlpha(theme::kTextDim, 0.4f);
    draw->AddRectFilled(ImVec2(XOf(canvas, transport.CycleStart()), ruler.Min.y + Px(3.0f)),
                        ImVec2(XOf(canvas, transport.CycleEnd()), ruler.Min.y + Px(11.0f)), range, Px(3.0f));

    if (kShowBars)
    {
        DrawBarMarks(draw, canvas);
    }
    else
    {
        DrawBeatMarks(draw, canvas);
    }
    draw->AddRectFilled(ImVec2(ruler.Min.x, ruler.Max.y - Px(1.0f)), ruler.Max, theme::kBorder);
}

// A region of the marker row: the sequence's intro or its loop, from `start` to `end` in quarter notes.
struct CycleMarker
{
    const char* name;
    double start;
    double end;
    ImU32 colour;
    const char* tooltip;
};

// The song's regions: the intro, where the loop starts after the song's start, and the loop. A song without a loop has
// none. During a drag of an edge of the loop, the loop follows the drag.
std::vector<CycleMarker> CycleMarkers(const SongView& song, const std::optional<LoopMove>& drag = std::nullopt)
{
    std::vector<CycleMarker> markers;
    const int32_t loop_start = drag ? drag->start : song.loop_start;
    const int32_t loop_end = drag ? drag->end : song.loop_end;
    if (loop_start < 0 || loop_end <= loop_start)
    {
        return markers;
    }

    const double quarter = std::max(song.ticks_per_quarter, 1);
    if (loop_start > 0)
    {
        markers.push_back({"Intro", 0.0, loop_start / quarter, theme::kTextDim,
                           "The sequence's intro. It plays once, before the loop."});
    }
    markers.push_back({"Loop", loop_start / quarter, loop_end / quarter, theme::kCycle,
                       "The sequence's loop. The game plays it over and over."});

    return markers;
}

// The edge of a timeline's loop at `x` in the marker row: true for the loop's end, false for its start. A drag of
// either edge moves it.
std::optional<bool> LoopEdgeAt(const Canvas& canvas, const SongView& song, float x)
{
    if (!song.loop_moves || song.loop_start < 0 || song.loop_end <= song.loop_start)
    {
        return std::nullopt;
    }

    const double quarter = std::max(song.ticks_per_quarter, 1);
    const float reach = Px(4.0f);
    if (std::abs(x - XOf(canvas, song.loop_end / quarter)) <= reach)
    {
        return true;
    }
    if (std::abs(x - XOf(canvas, song.loop_start / quarter)) <= reach)
    {
        return false;
    }

    return std::nullopt;
}

// Where a region lies in the marker row.
ImRect RegionRect(const Canvas& canvas, const CycleMarker& marker)
{
    const ImRect& row = canvas.markers;
    return ImRect(ImVec2(std::floor(XOf(canvas, marker.start)) + Px(1.0f), row.Min.y + Px(3.0f)),
                  ImVec2(std::floor(XOf(canvas, marker.end)) - Px(1.0f), row.Max.y - Px(4.0f)));
}

// The label of a change of a variable: the variable and its new value.
std::string ChangeLabel(const ChangeView& change)
{
    return std::to_string(change.variable) + " → " + std::to_string(change.value);
}

// The label of a place where the tracks read variables or branch: "if" at a branch, then the variables that the place
// reads. The marker row draws "if" in the List Editor's colour of conditions.
struct LogicLabel
{
    bool branch = false;
    std::string variables;
};

LogicLabel LogicLabelOf(const LogicMarkerView& marker)
{
    return {marker.branch, VariablesLabel(marker.variables)};
}

// The width of a place's label.
float LogicLabelWidth(const LogicLabel& label)
{
    ImFont* font = theme::GetFonts().text;
    const float branch = label.branch ? TextSize(font, theme::kSmallSize, "if").x : 0.0f;
    const float gap = label.branch && !label.variables.empty() ? Px(4.0f) : 0.0f;
    return branch + gap + TextSize(font, theme::kSmallSize, label.variables).x;
}

// Where the marker row's flags lie: a flag at the first place of each of the song's sets of commands, in the order of
// song.logic, and a flag at each change of a variable, in the order of song.changes. Each flag holds its label with the
// margins round it, but stops short of the next flag to its right, of either kind (FlagWidths). Of a set and a change
// at one place, the change's flag holds its label.
struct MarkerFlags
{
    std::vector<ImRect> logic;
    std::vector<ImRect> changes;
};

MarkerFlags MarkerFlagsOf(const Canvas& canvas, const SongView& song)
{
    const auto x_of = [&canvas](double tick)
    {
        return std::floor(XOf(canvas, tick / canvas.ticks_per_quarter));
    };

    std::vector<FlagSpan> spans;
    for (const LogicMarkerView& marker : song.logic)
    {
        spans.push_back({x_of(marker.ticks.front()), LogicLabelWidth(LogicLabelOf(marker)) + Px(10.0f)});
    }
    for (const ChangeView& change : song.changes)
    {
        const float width = TextSize(theme::GetFonts().text, theme::kSmallSize, ChangeLabel(change)).x;
        spans.push_back({x_of(change.tick), width + Px(10.0f)});
    }
    const std::vector<float> widths = FlagWidths(spans, Px(2.0f), Px(2.0f));

    const ImRect& row = canvas.markers;
    MarkerFlags flags;
    for (std::size_t i = 0; i < spans.size(); i++)
    {
        const ImRect flag(ImVec2(spans[i].x, row.Min.y + Px(3.0f)),
                          ImVec2(spans[i].x + widths[i], row.Max.y - Px(4.0f)));
        (i < song.logic.size() ? flags.logic : flags.changes).push_back(flag);
    }

    return flags;
}

// The place of a set of commands under the mouse at `mouse`: the set's flag at its first place, or else the nearest
// place of any set within a few pixels. Nothing elsewhere.
std::optional<MarkerPlace> LogicPlaceAt(const Canvas& canvas, const SongView& song, const MarkerFlags& flags,
                                        ImVec2 mouse)
{
    for (std::size_t i = song.logic.size(); i-- > 0;)
    {
        if (flags.logic[i].Contains(mouse))
        {
            return MarkerPlace{i, song.logic[i].ticks.front()};
        }
    }

    // The nearest place of each set lies on one side of the mouse or the other.
    const auto at = static_cast<int32_t>(std::ceil(QuartersAt(canvas, mouse.x) * canvas.ticks_per_quarter));
    std::optional<MarkerPlace> nearest;
    float distance = Px(3.0f);
    for (std::size_t i = 0; i < song.logic.size(); i++)
    {
        const std::vector<int32_t>& ticks = song.logic[i].ticks;
        const auto after = std::lower_bound(ticks.begin(), ticks.end(), at);
        for (auto tick = after == ticks.begin() ? after : after - 1; tick != ticks.end() && tick <= after; ++tick)
        {
            const float away = std::abs(std::floor(XOf(canvas, static_cast<double>(*tick) / canvas.ticks_per_quarter)) -
                                        std::floor(mouse.x));
            if (away <= distance)
            {
                distance = away;
                nearest = MarkerPlace{i, *tick};
            }
        }
    }

    return nearest;
}

// The tooltip of a set of commands: the commands, up to kMostMarkerLines of them, and the places where the set runs
// when it runs at more than one. At a branch, a line says what "if" means.
std::string LogicTooltip(const LogicMarkerView& marker, int beats_per_bar, int ticks_per_quarter)
{
    std::string text;
    for (std::size_t i = 0; i < marker.commands.size() && i < kMostMarkerLines; i++)
    {
        text += (i == 0 ? "" : "\n") + marker.commands[i].line;
    }
    if (marker.commands.size() > kMostMarkerLines)
    {
        text += "\nand " + std::to_string(marker.commands.size() - kMostMarkerLines) + " more";
    }
    text += "\n\n";
    if (marker.ticks.size() > 1)
    {
        const double quarter = std::max(ticks_per_quarter, 1);
        const auto place = [&](int32_t tick)
        {
            return WithoutFigureSpaces(PositionText(tick / quarter, beats_per_bar));
        };
        text += "These commands run at " + std::to_string(marker.ticks.size()) + " places, from " +
                place(marker.ticks.front()) + " to " + place(marker.ticks.back()) + ". ";
    }
    if (marker.branch)
    {
        text += "A command after \"if\" runs only if the last comparison held. ";
    }

    return text + "Double-click to open the commands in the List Editor.";
}

// Draws a flag of the marker row in `colour`: a line at the flag's place, and the flag's ground.
void DrawFlag(ImDrawList* draw, const Canvas& canvas, const ImRect& flag, ImU32 colour)
{
    const ImRect& row = canvas.markers;
    draw->AddRectFilled(flag.Min, flag.Max, theme::WithAlpha(colour, 0.22f), Px(3.0f));
    draw->AddRectFilled(ImVec2(flag.Min.x, row.Min.y + Px(2.0f)), ImVec2(flag.Min.x + Px(2.0f), row.Max.y - Px(2.0f)),
                        colour);
}

// Where the name of the region at `region` starts, for a name `width` wide: after the flags that cover the region's
// start. The flags would otherwise hide the name.
float RegionNameX(const MarkerFlags& marker_flags, const ImRect& region, float width)
{
    std::vector<ImRect> flags = marker_flags.logic;
    flags.insert(flags.end(), marker_flags.changes.begin(), marker_flags.changes.end());
    const auto left = [](const ImRect& a, const ImRect& b)
    {
        return a.Min.x < b.Min.x;
    };
    std::sort(flags.begin(), flags.end(), left);

    float x = region.Min.x + Px(6.0f);
    for (const ImRect& flag : flags)
    {
        if (flag.Min.x > x + width)
        {
            break;
        }

        if (flag.Max.x > x - Px(4.0f))
        {
            x = flag.Max.x + Px(6.0f);
        }
    }

    return x;
}

// The song's regions, each with its name.
void DrawRegions(ImDrawList* draw, const Canvas& canvas, const SongView& song, const MarkerFlags& flags,
                 const std::optional<LoopMove>& drag)
{
    ImFont* font = theme::GetFonts().text;
    for (const CycleMarker& marker : CycleMarkers(song, drag))
    {
        const ImRect rect = RegionRect(canvas, marker);
        draw->AddRectFilled(rect.Min, rect.Max, theme::WithAlpha(marker.colour, 0.22f), Px(3.0f));
        draw->AddRect(rect.Min, rect.Max, theme::WithAlpha(marker.colour, 0.75f), Px(3.0f), Px(1.0f));
        draw->PushClipRect(rect.Min, rect.Max, true);
        const ImVec2 size = TextSize(font, theme::kSmallSize, marker.name);
        DrawText(draw, font, theme::kSmallSize,
                 ImVec2(RegionNameX(flags, rect, size.x), rect.GetCenter().y - size.y * 0.5f), theme::kText,
                 marker.name);
        draw->PopClipRect();
    }
}

// A thin line at each place where a set of commands runs after its first, one at most in each column of pixels. The
// set's flag marks its first place.
void DrawLaterPlaces(ImDrawList* draw, const Canvas& canvas, const LogicMarkerView& marker)
{
    const ImRect& row = canvas.markers;
    const double first = QuartersAt(canvas, row.Min.x) * canvas.ticks_per_quarter;
    const double last = QuartersAt(canvas, row.Max.x) * canvas.ticks_per_quarter;
    auto tick = std::lower_bound(marker.ticks.begin() + 1, marker.ticks.end(), static_cast<int32_t>(std::floor(first)));
    while (tick != marker.ticks.end() && *tick <= last)
    {
        const float x = std::floor(XOf(canvas, static_cast<double>(*tick) / canvas.ticks_per_quarter));
        draw->AddRectFilled(ImVec2(x, row.Min.y + Px(3.0f)), ImVec2(x + Px(1.0f), row.Max.y - Px(4.0f)),
                            theme::WithAlpha(theme::kAccent, 0.6f));

        // The next place a column of pixels or more to the right.
        const double next = QuartersAt(canvas, x + Px(1.0f)) * canvas.ticks_per_quarter;
        tick = std::lower_bound(tick + 1, marker.ticks.end(), static_cast<int32_t>(std::ceil(next)));
    }
}

// The sets of commands that the song's tracks run where they read variables or branch. A blue flag with the set's label
// marks the first place of each set, and a thinner line each later place.
void DrawLogicFlags(ImDrawList* draw, const Canvas& canvas, const SongView& song, const MarkerFlags& flags)
{
    const ImRect& row = canvas.markers;
    ImFont* font = theme::GetFonts().text;
    for (const LogicMarkerView& marker : song.logic)
    {
        DrawLaterPlaces(draw, canvas, marker);
    }
    for (std::size_t i = 0; i < song.logic.size(); i++)
    {
        const ImRect& flag = flags.logic[i];
        if (flag.Max.x < row.Min.x || flag.Min.x > row.Max.x)
        {
            continue;
        }

        const LogicLabel label = LogicLabelOf(song.logic[i]);
        DrawFlag(draw, canvas, flag, theme::kAccent);
        draw->PushClipRect(flag.Min, flag.Max, true);
        const float top = flag.GetCenter().y - TextSize(font, theme::kSmallSize, "if").y * 0.5f;
        float x = flag.Min.x + Px(6.0f);
        if (label.branch)
        {
            DrawText(draw, font, theme::kSmallSize, ImVec2(x, top), theme::kCycle, "if");
            x += TextSize(font, theme::kSmallSize, "if").x + Px(4.0f);
        }
        DrawText(draw, font, theme::kSmallSize, ImVec2(x, top), theme::kText, label.variables);
        draw->PopClipRect();
    }
}

// The changes of the variables, each with the variable and its new value in a flag in the colour of the play button.
void DrawChangeFlags(ImDrawList* draw, const Canvas& canvas, const SongView& song, const MarkerFlags& flags)
{
    const ImRect& row = canvas.markers;
    ImFont* font = theme::GetFonts().text;
    for (std::size_t i = 0; i < song.changes.size(); i++)
    {
        const ImRect& flag = flags.changes[i];
        if (flag.Max.x < row.Min.x || flag.Min.x > row.Max.x)
        {
            continue;
        }

        const std::string label = ChangeLabel(song.changes[i]);
        const ImVec2 size = TextSize(font, theme::kSmallSize, label);
        DrawFlag(draw, canvas, flag, theme::kPlay);
        draw->PushClipRect(flag.Min, flag.Max, true);
        DrawText(draw, font, theme::kSmallSize, ImVec2(flag.Min.x + Px(6.0f), flag.GetCenter().y - size.y * 0.5f),
                 theme::kText, label);
        draw->PopClipRect();
    }
}

// The marker row under the ruler. A sequence with a loop shows its intro and its loop there as regions. Flags over the
// regions show the places where the tracks read variables or branch, and the changes of the variables. The edges of a
// timeline's loop take the mouse first, then the flags, and then the regions under them. A change's flag comes before
// the other flags. A drag of an edge of a timeline's loop moves it. A double-click on a region sets the locators round
// it, and Alt with a double-click zooms to it. A double-click on a place where the tracks read variables or branch
// opens its commands in the List Editor.
void DrawMarkers(ImDrawList* draw, const Canvas& canvas, const SongView& song, Transport& transport,
                 ArrangementState& state)
{
    // The dragged edge of the loop follows the mouse, on the beats or on the ticks.
    const ImGuiIO& io = ImGui::GetIO();
    const bool dragging = state.loop_drag && ImGui::IsMouseDown(ImGuiMouseButton_Left);
    if (dragging)
    {
        LoopMove& drag = *state.loop_drag;
        const int64_t step = io.KeyShift ? 1 : std::max(canvas.ticks_per_quarter, 1);
        const double tick = QuartersAt(canvas, io.MousePos.x) * canvas.ticks_per_quarter;
        const int64_t snapped = std::llround(tick / static_cast<double>(step)) * step;
        if (drag.end_edge)
        {
            drag.end = static_cast<int32_t>(
                std::clamp<int64_t>(snapped, int64_t{drag.start} + 1, std::numeric_limits<int32_t>::max()));
        }
        else
        {
            drag.start = static_cast<int32_t>(std::clamp<int64_t>(snapped, 0, int64_t{drag.end} - 1));
        }
    }

    const ImRect& row = canvas.markers;
    draw->AddRectFilled(row.Min, row.Max, theme::kPanel);
    draw->AddRectFilled(ImVec2(row.Min.x, row.Max.y - Px(1.0f)), row.Max, theme::kBorder);
    const MarkerFlags flags = MarkerFlagsOf(canvas, song);
    DrawRegions(draw, canvas, song, flags, state.loop_drag);
    DrawLogicFlags(draw, canvas, song, flags);
    DrawChangeFlags(draw, canvas, song, flags);

    // A drag shows the loop while the mouse holds the edge. When the mouse button is released, another tick moves the
    // loop.
    if (state.loop_drag)
    {
        const LoopMove& drag = *state.loop_drag;
        if (dragging)
        {
            const double quarter = std::max(canvas.ticks_per_quarter, 1);
            const std::string start = WithoutFigureSpaces(PositionText(drag.start / quarter, song.beats_per_bar));
            const std::string end = WithoutFigureSpaces(PositionText(drag.end / quarter, song.beats_per_bar));
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
            WrappedTooltip("Loop from " + start + " to " + end);
            return;
        }

        if (drag.start != drag.from_start || drag.end != drag.from_end)
        {
            state.loop_move = drag;
        }
        state.loop_drag.reset();
        return;
    }

    // The edge, the flag or the region under the mouse. A tooltip waits while a button is down. A drag of the track
    // list's edge leaves the row alone.
    if (!ImGui::IsWindowHovered() || !row.Contains(io.MousePos) || state.resizing_track_list)
    {
        return;
    }

    const bool double_click = ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
    const bool still = !ImGui::IsAnyMouseDown();
    if (const std::optional<bool> end_edge = LoopEdgeAt(canvas, song, io.MousePos.x))
    {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !double_click)
        {
            state.loop_drag = LoopMove{song.loop_start, song.loop_end, song.loop_start, song.loop_end, *end_edge};
        }
        else if (still)
        {
            WrappedTooltip(*end_edge ? "The loop's end. Drag it to move the end."
                                     : "The loop's start. Drag it to move the start.");
        }
        return;
    }
    for (std::size_t i = 0; i < song.changes.size(); i++)
    {
        const ChangeView& change = song.changes[i];
        if (flags.changes[i].Contains(io.MousePos))
        {
            if (still)
            {
                WrappedTooltip("Variable " + std::to_string(change.variable) + " changes to " +
                               std::to_string(change.value) + " at " + TimeText(change.time));
            }
            return;
        }
    }
    if (const std::optional<MarkerPlace> place = LogicPlaceAt(canvas, song, flags, io.MousePos))
    {
        if (double_click)
        {
            state.open_marker = place;
        }
        else if (still)
        {
            WrappedTooltip(LogicTooltip(song.logic[place->marker], song.beats_per_bar, canvas.ticks_per_quarter));
        }
        return;
    }
    for (const CycleMarker& marker : CycleMarkers(song))
    {
        if (!RegionRect(canvas, marker).Contains(io.MousePos))
        {
            continue;
        }

        if (double_click && io.KeyAlt)
        {
            state.zoom_range = std::pair(marker.start, marker.end);
        }
        else if (double_click)
        {
            transport.SetLocators(marker.start, marker.end);
        }
        else if (still)
        {
            WrappedTooltip(std::string(marker.tooltip) + " Double-click to set the locators round it. " + kAltKey +
                           "-double-click to zoom to it.");
        }
        return;
    }
}

// The marker row's menu. Locate moves the position to a region's start or to a flag. Cycle sets the locators round a
// region, and Zoom fits a region into the lanes.
void DrawMarkerMenu(const SongView& song, Transport& transport, ArrangementState& state)
{
    if (!ImGui::BeginPopup(kMarkerMenu))
    {
        return;
    }

    // Locate's markers, in the order of their places: the regions, the flags of the sets of commands that the tracks
    // run where they read variables or branch, and the changes' flags.
    struct Located
    {
        double quarters;
        std::string name;
    };
    const double quarter = std::max(song.ticks_per_quarter, 1);
    const std::vector<CycleMarker> cycles = CycleMarkers(song);
    std::vector<Located> located;
    for (const CycleMarker& marker : cycles)
    {
        located.push_back({marker.start, marker.name});
    }
    for (const LogicMarkerView& marker : song.logic)
    {
        const LogicLabel label = LogicLabelOf(marker);
        const std::string name = std::string(label.branch ? "if " : "") + label.variables;
        located.push_back({marker.ticks.front() / quarter, label.variables.empty() ? std::string("if") : name});
    }
    for (const ChangeView& change : song.changes)
    {
        located.push_back({change.tick / quarter,
                           "Variable " + std::to_string(change.variable) + " → " + std::to_string(change.value)});
    }
    const auto earlier = [](const Located& a, const Located& b)
    {
        return a.quarters < b.quarters;
    };
    std::stable_sort(located.begin(), located.end(), earlier);

    // A grey menu gives its reason in a tooltip.
    const bool locate = ImGui::BeginMenu("Locate", !located.empty());
    if (located.empty())
    {
        ItemTooltip(
            "The sound has no marker: no loop, no place where its tracks read variables or branch, and no "
            "change of a variable.");
    }
    if (locate)
    {
        for (std::size_t i = 0; i < located.size(); i++)
        {
            ImGui::PushID(static_cast<int>(i));
            // The figures keep the positions' digits in line, and their figure spaces as wide as a digit.
            const std::string position = PositionText(located[i].quarters, song.beats_per_bar);
            PushFigures();
            const bool chosen = ImGui::MenuItem(located[i].name.c_str(), position.c_str());
            ImGui::PopFont();
            if (chosen)
            {
                transport.SetPosition(located[i].quarters);
            }
            ImGui::PopID();
        }
        ImGui::EndMenu();
    }
    const bool cycle = ImGui::BeginMenu("Cycle", !cycles.empty());
    if (cycles.empty())
    {
        ItemTooltip("The sound has no loop. Cycle sets the locators round the intro or the loop.");
    }
    if (cycle)
    {
        for (const CycleMarker& marker : cycles)
        {
            if (ImGui::MenuItem(marker.name))
            {
                transport.SetLocators(marker.start, marker.end);
            }
        }
        ImGui::EndMenu();
    }
    const bool zoom = ImGui::BeginMenu("Zoom", !cycles.empty());
    if (cycles.empty())
    {
        ItemTooltip("The sound has no loop. Zoom fits the intro or the loop into the lanes.");
    }
    if (zoom)
    {
        for (const CycleMarker& marker : cycles)
        {
            if (ImGui::MenuItem(marker.name))
            {
                state.zoom_range = std::pair(marker.start, marker.end);
            }
        }
        ImGui::EndMenu();
    }
    ImGui::EndPopup();
}

// A tempo as the tempo row shows it: a whole number of beats a minute, or two decimals for a tempo between them.
std::string TempoText(double bpm)
{
    std::array<char, 32> text{};
    const bool whole = std::abs(bpm - std::round(bpm)) < 0.005;
    std::snprintf(text.data(), text.size(), whole ? "%.0f" : "%.2f", bpm);

    return text.data();
}

// Where the tempo `bpm` lies in the tempo row, between the row's margins. A tempo outside the row's span lies at the
// nearer margin.
float TempoY(const ImRect& row, const TempoSpan& span, double bpm)
{
    const float margin = Px(kTempoMargin);
    const double share = span.high > span.low ? std::clamp((bpm - span.low) / (span.high - span.low), 0.0, 1.0) : 0.5;
    return std::floor(row.Max.y - margin - static_cast<float>(share) * (row.GetHeight() - margin * 2.0f));
}

// Where a point of the tempo row lies: at its tick, and at its tempo. A tempo without one value lies in the middle of
// the row. During `drag`, each run of the dragged command lies at the drag's tempo, and its tick moves by the drag's
// shift.
ImVec2 TempoPointPos(const Canvas& canvas, const TempoSpan& span, const TempoPointView& point,
                     const std::optional<TempoMove>& drag)
{
    const bool dragged = drag && drag->point.command == point.command;
    const int32_t tick = point.tick + (dragged ? drag->shift : 0);
    const float x = std::floor(XOf(canvas, static_cast<double>(tick) / canvas.ticks_per_quarter));
    if (!point.bpm)
    {
        return ImVec2(x, std::floor(canvas.tempo.GetCenter().y));
    }

    return ImVec2(x, TempoY(canvas.tempo, span, dragged ? drag->to : *point.bpm));
}

// The point of the tempo row under the mouse at `mouse`: the nearest within kTempoGrab. Of two points in one place, the
// later takes the mouse. It's drawn over the earlier one.
std::optional<std::size_t> TempoPointAt(const Canvas& canvas, const SongView& song, const TempoSpan& span, ImVec2 mouse)
{
    std::optional<std::size_t> nearest;
    float distance = Px(kTempoGrab);
    for (std::size_t i = 0; i < song.tempo_points.size(); i++)
    {
        const ImVec2 at = TempoPointPos(canvas, span, song.tempo_points[i], std::nullopt);
        const float away = std::hypot(at.x - mouse.x, at.y - mouse.y);
        if (away <= distance)
        {
            distance = away;
            nearest = i;
        }
    }

    return nearest;
}

// The tooltip of point `index` of the tempo row: the track, the place and the tempo, then what the mouse does there.
std::string TempoTooltip(const SongView& song, std::size_t index)
{
    const TempoPointView& point = song.tempo_points[index];
    const double quarter = std::max(song.ticks_per_quarter, 1);
    std::string text = point.track_name + " sets the tempo at " +
                       WithoutFigureSpaces(PositionText(point.tick / quarter, song.beats_per_bar)) + ": " +
                       (point.bpm ? TempoText(*point.bpm) + " BPM" : point.source) + "\n\n";
    const auto same = [&point](const TempoPointView& other)
    {
        return other.command == point.command;
    };
    const auto runs = std::count_if(song.tempo_points.begin(), song.tempo_points.end(), same);
    if (runs > 1)
    {
        text += "The command runs at " + std::to_string(runs) + " places. A change changes them all. ";
    }
    if (!point.reach.empty())
    {
        text += "An edit also changes " + point.reach + ". ";
    }
    if (point.conditional)
    {
        text += "The command runs only if the last comparison held. ";
    }
    if (Overridden(song, index))
    {
        text += "A later command sets the tempo again in the same tick. ";
    }
    if (point.bpm && point.fixed.empty())
    {
        text +=
            "Drag up or down to change the tempo, and to the side to move it to another beat. Shift makes the drag "
            "finer. ";
    }
    else if (point.bpm)
    {
        text += "Drag up or down to change the tempo, finer with Shift. ";
    }
    else if (point.fixed.empty())
    {
        text += "Drag to the side to move it to another beat, or to another tick with Shift. ";
    }

    return text + "Double-click to open the command in the List Editor.";
}

// The mouse's cursor over a point of the tempo row: arrows in the directions that a drag of the point takes.
ImGuiMouseCursor TempoCursor(const TempoPointView& point)
{
    if (point.bpm && point.fixed.empty())
    {
        return ImGuiMouseCursor_ResizeAll;
    }
    if (point.bpm)
    {
        return ImGuiMouseCursor_ResizeNS;
    }

    return point.fixed.empty() ? ImGuiMouseCursor_ResizeEW : ImGuiMouseCursor_Arrow;
}

// The tick where the tempo row's line ends: the song's end, or the last point where a track sets the tempo after it.
int32_t TempoRowEnd(const SongView& song)
{
    int32_t end = song.length;
    for (const TempoPointView& point : song.tempo_points)
    {
        end = std::max(end, point.tick);
    }

    return end;
}

// The tempo row's line: a step at each change of the tempo, filled below, up to TempoRowEnd. The tempo's number stands
// over the start of each step that has room for it, or under the step where the row's top leaves no room.
void DrawTempoLine(ImDrawList* draw, const Canvas& canvas, const SongView& song,
                   const std::vector<TempoChangeView>& line, const TempoSpan& span)
{
    const ImRect& row = canvas.tempo;
    const double quarter = canvas.ticks_per_quarter;
    const float end = std::floor(XOf(canvas, static_cast<double>(TempoRowEnd(song)) / quarter));
    const float bottom = row.Max.y - Px(1.0f);
    ImFont* font = theme::GetFonts().figures;
    for (std::size_t i = 0; i < line.size(); i++)
    {
        const float left = std::floor(XOf(canvas, line[i].tick / quarter));
        const float right = i + 1 < line.size() ? std::floor(XOf(canvas, line[i + 1].tick / quarter)) : end;
        if (left > row.Max.x)
        {
            break;
        }

        const float y = TempoY(row, span, line[i].bpm);
        if (i > 0)
        {
            draw->AddLine(ImVec2(left, TempoY(row, span, line[i - 1].bpm)), ImVec2(left, y), theme::kAccent, Px(1.5f));
        }
        if (right <= left || right < row.Min.x)
        {
            continue;
        }

        draw->AddRectFilled(ImVec2(left, y), ImVec2(right, bottom), theme::WithAlpha(theme::kAccent, 0.16f));
        draw->AddLine(ImVec2(left, y), ImVec2(right, y), theme::kAccent, Px(1.5f));
        const std::string number = TempoText(line[i].bpm);
        const ImVec2 size = TextSize(font, theme::kSmallSize, number);
        if (right - left >= size.x + Px(12.0f))
        {
            const float above = y - Px(3.0f) - size.y;
            const float top = above >= row.Min.y + Px(1.0f) ? above : y + Px(4.0f);
            DrawText(draw, font, theme::kSmallSize, ImVec2(left + Px(6.0f), top), theme::kTextMuted, number);
        }
    }
}

// The tempo row's points. A tempo without one value has a bar over the row, with its source beside it where it fits
// (FittingLabels). The Key Editor's controller lanes draw such values alike. The point under the mouse is light, and so
// is each run of a dragged command.
void DrawTempoPoints(ImDrawList* draw, const Canvas& canvas, const SongView& song, const TempoSpan& span,
                     const std::optional<TempoMove>& drag, std::optional<std::size_t> hovered)
{
    const ImRect& row = canvas.tempo;
    ImFont* font = theme::GetFonts().text;
    std::vector<ImVec2> places;
    std::vector<float> xs;
    std::vector<float> widths;
    for (const TempoPointView& point : song.tempo_points)
    {
        places.push_back(TempoPointPos(canvas, span, point, drag));
        const bool shown = places.back().x >= row.Min.x - Px(8.0f) && places.back().x <= row.Max.x + Px(8.0f);
        xs.push_back(places.back().x);
        widths.push_back(shown && !point.bpm ? TextSize(font, theme::kSmallSize, point.source).x : 0.0f);
    }
    const std::vector<bool> fits = FittingLabels(xs, widths, Px(6.0f));
    for (std::size_t i = 0; i < song.tempo_points.size(); i++)
    {
        const TempoPointView& point = song.tempo_points[i];
        const ImVec2 at = places[i];
        if (at.x < row.Min.x - Px(8.0f) || at.x > row.Max.x + Px(8.0f))
        {
            continue;
        }

        const bool light = hovered == i || (drag && drag->point.command == point.command);
        if (point.bpm)
        {
            DrawPoint(draw, at.x, at.y, at.y, theme::kAccent, light, point.conditional);
            continue;
        }

        DrawPoint(draw, at.x, row.Min.y + Px(kTempoMargin), row.Max.y - Px(kTempoMargin), theme::kAccent, light,
                  point.conditional);
        if (fits[i])
        {
            DrawText(draw, font, theme::kSmallSize, ImVec2(at.x + Px(6.0f), row.Min.y + Px(2.0f)), theme::kTextMuted,
                     point.source);
        }
    }
}

// The tempo row under the marker row: the tempo as ntdlib's performer played it, as a line with a step at each change,
// and a point at each command that sets the tempo. A drag of a point up or down changes its tempo in whole beats a
// minute, from 1 up, finer with Shift. A drag to the side moves the command to the nearest beat, or to the nearest tick
// with Shift, as far as the command can move. A double-click on a point opens its command in the List Editor.
void DrawTempo(ImDrawList* draw, const Canvas& canvas, const SongView& song, ArrangementState& state)
{
    const ImRect& row = canvas.tempo;
    draw->AddRectFilled(row.Min, row.Max, theme::kPanel);
    draw->AddRectFilled(ImVec2(row.Min.x, row.Max.y - Px(1.0f)), row.Max, theme::kBorder);

    // The row keeps its span during a drag. A drag past the span's ends goes on, and its point stays at the row's edge.
    const ImGuiIO& io = ImGui::GetIO();
    const TempoSpan span = TempoSpanOf(song);
    std::optional<TempoMove> shown;
    std::string refused;
    if (state.tempo_drag)
    {
        TempoMove& drag = *state.tempo_drag;
        if (drag.point.bpm)
        {
            const double room = std::max(row.GetHeight() - Px(kTempoMargin) * 2.0f, 1.0f);
            const double step = (span.high - span.low) / room * (io.KeyShift ? kFineTempoDrag : 1.0);
            drag.to = std::max(drag.to - static_cast<double>(io.MouseDelta.y) * step, 1.0);
        }

        // The move in time starts once the mouse has gone far enough to the side.
        const float across = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0.0f).x;
        if (drag.point.fixed.empty() && (std::abs(across) >= Px(kTempoMoveDistance) || drag.shift != 0))
        {
            const int64_t step = io.KeyShift ? 1 : canvas.ticks_per_quarter;
            const double tick =
                drag.point.tick + static_cast<double>(across / canvas.beat_width) * canvas.ticks_per_quarter;
            const int64_t moved = std::llround(tick / static_cast<double>(step)) * step - drag.point.tick;
            drag.shift = static_cast<int32_t>(
                std::clamp<int64_t>(moved, -int64_t{drag.point.earlier}, int64_t{drag.point.later}));
        }
        else if (!drag.point.fixed.empty() && std::abs(across) >= Px(kTempoMoveDistance * 3.0f))
        {
            refused = drag.point.fixed;
        }
        shown = TempoMove{drag.point, drag.from, std::round(drag.to), drag.shift};
    }
    const bool over = ImGui::IsWindowHovered() && row.Contains(io.MousePos) && !state.resizing_track_list;
    const std::optional<std::size_t> hovered =
        over && !state.tempo_drag ? TempoPointAt(canvas, song, span, io.MousePos) : std::nullopt;
    DrawTempoLine(draw, canvas, song, TempoLine(song, shown), span);
    DrawTempoPoints(draw, canvas, song, span, shown, hovered);

    // A drag shows its tempo and a moved point's start while the mouse holds the point, or why the point stays at its
    // tick. When the mouse button is released, another tempo or another tick becomes a move.
    if (state.tempo_drag)
    {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            std::string text = shown->point.bpm ? TempoText(shown->to) + " BPM" : shown->point.source;
            if (shown->shift != 0)
            {
                const double start = static_cast<double>(shown->point.tick + shown->shift) / canvas.ticks_per_quarter;
                text += "\nStart " + WithoutFigureSpaces(PositionText(start, song.beats_per_bar));
            }
            ImGui::SetMouseCursor(TempoCursor(shown->point));
            WrappedTooltip(refused.empty() ? text : refused);
            return;
        }

        if (shown->to != shown->from || shown->shift != 0)
        {
            state.tempo_move = shown;
        }
        state.tempo_drag.reset();
        return;
    }

    // The point under the mouse. A tooltip waits while a button is down.
    if (!hovered)
    {
        return;
    }

    const TempoPointView& point = song.tempo_points[*hovered];
    ImGui::SetMouseCursor(TempoCursor(point));
    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
    {
        state.open_tempo = point;
    }
    else if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && (point.bpm || point.fixed.empty()))
    {
        const double bpm = point.bpm.value_or(0.0);
        state.tempo_drag = TempoMove{point, bpm, bpm};
    }
    else if (!ImGui::IsAnyMouseDown())
    {
        WrappedTooltip(TempoTooltip(song, *hovered));
    }
}

// The tempo row's header: its name, and the tempos at the top and the bottom of the row's line.
void DrawTempoHeader(ImDrawList* draw, const ImRect& header, const SongView& song)
{
    draw->AddRectFilled(header.Min, header.Max, theme::kRaised);
    draw->AddRectFilled(ImVec2(header.Min.x, header.Max.y - Px(1.0f)), header.Max, theme::kBorder);
    draw->AddRectFilled(ImVec2(header.Max.x - Px(1.0f), header.Min.y), header.Max, theme::kBorder);
    const ImVec2 caps = CapsSize("TEMPO");
    DrawCaps(draw, ImVec2(header.Min.x + Px(14.0f), header.GetCenter().y - caps.y * 0.5f), theme::kTextMuted, "TEMPO");

    // The scale lines up with the line's highest and lowest places.
    const TempoSpan span = TempoSpanOf(song);
    ImFont* font = theme::GetFonts().figures;
    const std::string high = TempoText(span.high);
    const std::string low = TempoText(span.low);
    const ImVec2 high_size = TextSize(font, theme::kSmallSize, high);
    const ImVec2 low_size = TextSize(font, theme::kSmallSize, low);
    const float right = header.Max.x - Px(kEdgeGrab) - Px(6.0f);
    DrawText(draw, font, theme::kSmallSize,
             ImVec2(right - high_size.x, header.Min.y + Px(kTempoMargin) - high_size.y * 0.5f), theme::kTextMuted,
             high);
    DrawText(draw, font, theme::kSmallSize,
             ImVec2(right - low_size.x, header.Max.y - Px(kTempoMargin) - low_size.y * 0.5f), theme::kTextMuted, low);
    if (ImGui::IsWindowHovered() && header.Contains(ImGui::GetIO().MousePos) && !ImGui::IsAnyMouseDown())
    {
        WrappedTooltip("The tempo as the sequence plays it, in beats a minute");
    }
}

// Draws the tracks' headers and follows their clicks. A right-click selects the track under the mouse and opens the
// list's context menu. While the user drags the track list's edge, or the mouse is over it, `takes_clicks` is false.
void DrawTrackHeaders(ImDrawList* draw, const Canvas& canvas, SongView& song, ArrangementState& state,
                      bool takes_clicks)
{
    const ImGuiIO& io = ImGui::GetIO();
    const bool hovered = takes_clicks && ImGui::IsWindowHovered() && canvas.headers.Contains(io.MousePos);
    const bool clicked = hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    const bool menu = hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right);
    ImFont* text = theme::GetFonts().text;
    ImFont* figures = theme::GetFonts().figures;
    for (std::size_t i = 0; i < song.tracks.size(); i++)
    {
        TrackView& track = song.tracks[i];
        const float top = RowTop(canvas, i);
        if (top + canvas.row_height <= canvas.headers.Min.y || top >= canvas.headers.Max.y)
        {
            continue;
        }

        // The row, lit in the accent's colour when it's selected. The track's colour runs down the left edge. While the
        // tracks' colours are Custom, a click on the strip offers the palette, and the strip widens under the mouse.
        const ImRect row(ImVec2(canvas.headers.Min.x, top), ImVec2(canvas.headers.Max.x, top + canvas.row_height));
        const bool selected = state.selected_track == static_cast<int>(i);
        const ImRect strip(row.Min, ImVec2(row.Min.x + Px(kColourStripGrab), row.Max.y));
        const bool over_strip = state.custom_colours && hovered && strip.Contains(io.MousePos);

        // The splitter between the arrangement and the zone on its left reaches a few points into the strip, and takes
        // a press there before the arrangement does. A press on the strip opens the palette all the same. ClearActiveID
        // first takes the press from the splitter.
        const bool strip_pressed = state.custom_colours && takes_clicks &&
                                   ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) &&
                                   canvas.headers.Contains(io.MousePos) && strip.Contains(io.MousePos) &&
                                   ImGui::IsMouseClicked(ImGuiMouseButton_Left);
        if (over_strip)
        {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            WrappedTooltip("Track colour");
        }
        draw->AddRectFilled(row.Min, row.Max,
                            selected ? theme::Mix(theme::kRaised, theme::kAccent, 0.2f) : theme::kRaised);
        const float strip_width = over_strip ? Px(kColourStripWidth + 2.0f) : Px(kColourStripWidth);
        draw->AddRectFilled(row.Min, ImVec2(row.Min.x + strip_width, row.Max.y), track.colour);
        draw->AddRectFilled(ImVec2(row.Min.x, row.Max.y - Px(1.0f)), row.Max, theme::kBorder);

        // The M and S buttons. A row taller than the default keeps its content at the top.
        const float middle = row.Min.y + std::min(canvas.row_height, Px(kDefaultTrackHeight)) * 0.5f;
        const ImVec2 button(Px(22.0f), Px(20.0f));
        const float button_top = std::floor(middle - button.y * 0.5f);
        const ImRect mute(ImVec2(row.Max.x - Px(62.0f), button_top),
                          ImVec2(row.Max.x - Px(62.0f) + button.x, button_top + button.y));
        const ImRect solo(ImVec2(mute.Max.x + Px(4.0f), button_top),
                          ImVec2(mute.Max.x + Px(4.0f) + button.x, button_top + button.y));
        const bool over_mute = hovered && mute.Contains(io.MousePos);
        const bool over_solo = hovered && solo.Contains(io.MousePos);
        DrawLetterButton(draw, mute.Min, mute.Max, "M", track.mute, theme::kMute, over_mute);
        DrawLetterButton(draw, solo.Min, solo.Max, "S", track.solo, theme::kSolo, over_solo);

        // The number, the name, and the bank and program under it. A tall row shows the bank and the program on lines
        // of their own, and a short row has room for the name alone. Text that doesn't fit ends in an ellipsis.
        std::array<char, 16> number{};
        std::snprintf(number.data(), number.size(), "%zu", i + 1);
        const ImVec2 number_size = TextSize(figures, theme::kSmallSize, number.data());
        DrawText(draw, figures, theme::kSmallSize,
                 ImVec2(row.Min.x + Px(22.0f) - number_size.x * 0.5f, middle - number_size.y * 0.5f), theme::kTextMuted,
                 number.data());
        const float left = row.Min.x + Px(40.0f);
        const float width = mute.Min.x - Px(8.0f) - left;
        draw->PushClipRect(ImVec2(row.Min.x, row.Min.y), ImVec2(mute.Min.x - Px(8.0f), row.Max.y), true);
        if (canvas.row_height >= Px(kThreeLineHeight))
        {
            const std::vector<std::string> lines = InstrumentLines(track);
            const float name_top = std::floor(middle - Px(lines.size() > 1 ? 23.0f : 18.0f));
            DrawText(draw, text, theme::kTextSize, ImVec2(left, name_top), theme::kText,
                     FitText(text, theme::kTextSize, track.name, width));
            for (std::size_t line = 0; line < lines.size(); line++)
            {
                DrawText(draw, text, theme::kSmallSize,
                         ImVec2(left, name_top + Px(19.0f) + Px(15.0f) * static_cast<float>(line)), theme::kTextMuted,
                         FitText(text, theme::kSmallSize, lines[line], width));
            }
        }
        else if (canvas.row_height >= Px(kTwoLineHeight))
        {
            const float name_top = std::floor(middle - Px(18.0f));
            DrawText(draw, text, theme::kTextSize, ImVec2(left, name_top), theme::kText,
                     FitText(text, theme::kTextSize, track.name, width));
            DrawText(draw, text, theme::kSmallSize, ImVec2(left, name_top + Px(19.0f)), theme::kTextMuted,
                     FitText(text, theme::kSmallSize, InstrumentText(track), width));
        }
        else
        {
            const ImVec2 name_size = TextSize(text, theme::kTextSize, track.name);
            DrawText(draw, text, theme::kTextSize, ImVec2(left, middle - name_size.y * 0.5f), theme::kText,
                     FitText(text, theme::kTextSize, track.name, width));
        }
        draw->PopClipRect();

        if (strip_pressed)
        {
            if (ImGui::GetActiveID() != 0)
            {
                ImGui::ClearActiveID();
            }
            state.colour_popup_track = static_cast<int>(i);
            ImGui::OpenPopup(kColourPopup);
        }
        else if (clicked && row.Contains(io.MousePos))
        {
            if (over_mute || over_solo)
            {
                bool& flag = over_mute ? track.mute : track.solo;
                flag = !flag;
            }
            else
            {
                state.selected_track = static_cast<int>(i);
            }
        }
        if (menu && row.Contains(io.MousePos))
        {
            state.selected_track = static_cast<int>(i);
        }
    }
    draw->AddRectFilled(ImVec2(canvas.headers.Max.x - Px(1.0f), canvas.headers.Min.y), canvas.headers.Max,
                        theme::kBorder);
    if (menu)
    {
        ImGui::OpenPopup(kTrackMenu);
    }
}

// Draws a part: a body in the track's colour, a strip with the name across the top, and the notes in miniature. The
// notes fill the part's range of keys. A part too short for them shows the strip alone. A silent track's parts are
// dimmed.
void DrawPart(ImDrawList* draw, const Canvas& canvas, const ImRect& rect, const PartView& part, const TrackView& track,
              bool silent, bool selected)
{
    const ImU32 colour = silent ? theme::Mix(track.colour, theme::kPanel, 0.7f) : track.colour;
    const float rounding = Px(4.0f);
    const float strip = Px(15.0f);
    draw->AddRectFilled(rect.Min, rect.Max, theme::Mix(colour, theme::kWindow, 0.62f), rounding);
    const ImU32 strip_colour = theme::Mix(colour, theme::kWindow, 0.08f);
    draw->AddRectFilled(rect.Min, ImVec2(rect.Max.x, rect.Min.y + strip), strip_colour, rounding,
                        ImDrawFlags_RoundCornersTop);

    // The name: the subroutine's for a call's part, and the track's for another. A linked part shows an equals sign at
    // the end of its strip. A silent track's dimmed strip, or a dark colour, takes light text.
    draw->PushClipRect(rect.Min, rect.Max, true);
    ImFont* font = theme::GetFonts().text;
    const std::string& name = part.name.empty() ? track.name : part.name;
    const ImVec2 name_size = TextSize(font, theme::kSmallSize, name);
    const ImVec2 mark_size = TextSize(font, theme::kSmallSize, "=");
    const float mark_left = rect.Max.x - Px(6.0f) - mark_size.x;
    const bool linked = part.linked && rect.GetWidth() > mark_size.x * 4.0f;
    draw->PushClipRect(rect.Min, ImVec2(linked ? mark_left - Px(4.0f) : rect.Max.x, rect.Min.y + strip), true);
    const ImU32 text_colour = theme::TextOn(strip_colour);
    DrawText(draw, font, theme::kSmallSize, ImVec2(rect.Min.x + Px(6.0f), rect.Min.y + (strip - name_size.y) * 0.5f),
             text_colour, name);
    draw->PopClipRect();
    if (linked)
    {
        DrawText(draw, font, theme::kSmallSize, ImVec2(mark_left, rect.Min.y + (strip - mark_size.y) * 0.5f),
                 text_colour, "=");
    }

    const float top = rect.Min.y + strip + Px(4.0f);
    const float bottom = rect.Max.y - Px(4.0f);
    if (!part.notes.empty() && bottom - top >= Px(6.0f))
    {
        const auto [lowest, highest] = std::minmax_element(
            part.notes.begin(), part.notes.end(), [](const NoteView& a, const NoteView& b) { return a.key < b.key; });
        const int low = lowest->key;
        const int span = std::max(highest->key - low + 1, 12);
        const float step = (bottom - top) / static_cast<float>(span);
        const float height = std::clamp(step, Px(1.5f), Px(3.0f));
        const ImU32 note_colour = theme::Mix(colour, theme::kText, silent ? 0.25f : 0.6f);
        const float origin = XOf(canvas, static_cast<double>(part.start) / canvas.ticks_per_quarter);
        const float tick_width = canvas.beat_width / static_cast<float>(canvas.ticks_per_quarter);
        for (const NoteView& note : part.notes)
        {
            const float x = origin + static_cast<float>(note.start) * tick_width;
            const float width = std::max(static_cast<float>(note.length) * tick_width, Px(1.5f));
            const float y = bottom - static_cast<float>(note.key - low + 1) * step + (step - height) * 0.5f;
            draw->AddRectFilled(ImVec2(x, y), ImVec2(x + width, y + height), note_colour, Px(0.75f));
        }
    }
    draw->PopClipRect();

    draw->AddRect(rect.Min, rect.Max, selected ? theme::kText : theme::Mix(colour, theme::kWindow, 0.25f), rounding,
                  selected ? Px(1.5f) : Px(1.0f));
}

void DrawLanes(ImDrawList* draw, const Canvas& canvas, SongView& song, const Transport& transport,
               ArrangementState& state)
{
    // The lanes, then the grid and the cycle's range over them.
    for (std::size_t i = 0; i < song.tracks.size(); i++)
    {
        const float top = RowTop(canvas, i);
        draw->AddRectFilled(ImVec2(canvas.lanes.Min.x, top), ImVec2(canvas.lanes.Max.x, top + canvas.row_height),
                            i % 2 == 0 ? theme::kPanel : theme::kPanelAlt);
        draw->AddRectFilled(ImVec2(canvas.lanes.Min.x, top + canvas.row_height - Px(1.0f)),
                            ImVec2(canvas.lanes.Max.x, top + canvas.row_height), theme::kBorder);
    }
    DrawGrid(draw, canvas);
    if (transport.Cycle())
    {
        draw->AddRectFilled(ImVec2(XOf(canvas, transport.CycleStart()), canvas.lanes.Min.y),
                            ImVec2(XOf(canvas, transport.CycleEnd()), canvas.lanes.Max.y),
                            theme::WithAlpha(theme::kCycle, 0.05f));
    }

    // A line down the lanes at the loop's start and at its end.
    if (song.loop_start >= 0)
    {
        for (const int32_t tick : {song.loop_start, song.loop_end})
        {
            const float x = std::floor(XOf(canvas, static_cast<double>(tick) / canvas.ticks_per_quarter));
            draw->AddRectFilled(ImVec2(x, canvas.lanes.Min.y), ImVec2(x + Px(1.0f), canvas.lanes.Max.y),
                                theme::WithAlpha(theme::kCycle, 0.45f));
        }
    }

    // The parts. A click on one selects it, and a double-click opens it in the Key Editor. A click between them selects
    // the track, and a double-click there opens the track in the Key Editor at the beat under the mouse.
    const ImGuiIO& io = ImGui::GetIO();
    const bool clicked = ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
                         canvas.lanes.Contains(io.MousePos) && !state.resizing_track_list;
    bool hit = false;
    for (std::size_t i = 0; i < song.tracks.size(); i++)
    {
        const TrackView& track = song.tracks[i];
        const float top = RowTop(canvas, i);
        if (top + canvas.row_height <= canvas.lanes.Min.y || top >= canvas.lanes.Max.y)
        {
            continue;
        }

        const bool silent = !IsHeard(song, i);
        for (std::size_t p = 0; p < track.parts.size(); p++)
        {
            const PartView& part = track.parts[p];
            const float left = XOf(canvas, static_cast<double>(part.start) / canvas.ticks_per_quarter);
            const float right = XOf(canvas, static_cast<double>(part.start + part.length) / canvas.ticks_per_quarter);
            if (right < canvas.lanes.Min.x || left > canvas.lanes.Max.x)
            {
                continue;
            }

            const ImRect rect(ImVec2(std::floor(left) + Px(1.0f), top + Px(3.0f)),
                              ImVec2(std::floor(right) - Px(1.0f), top + canvas.row_height - Px(4.0f)));
            const bool selected =
                state.selected_part_track == static_cast<int>(i) && state.selected_part == static_cast<int>(p);
            DrawPart(draw, canvas, rect, part, track, silent, selected);
            if (!part.name.empty() && rect.Contains(io.MousePos) && ImGui::IsWindowHovered() &&
                !ImGui::IsAnyMouseDown())
            {
                WrappedTooltip(part.linked ? "Linked part: subroutine " + part.name +
                                                 ". Other calls play it too, in this sound or in others. An edit "
                                                 "of its notes changes what every call plays."
                                           : "Subroutine " + part.name + ". No other call plays it.");
            }
            if (clicked && rect.Contains(io.MousePos))
            {
                state.selected_part_track = static_cast<int>(i);
                state.selected_part = static_cast<int>(p);
                state.selected_track = static_cast<int>(i);
                state.open_editor = state.open_editor || ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
                hit = true;
            }
        }
    }

    if (clicked && !hit)
    {
        state.selected_part_track = -1;
        state.selected_part = -1;
        const auto row =
            static_cast<std::size_t>((io.MousePos.y - canvas.lanes.Min.y + canvas.scroll_y) / canvas.row_height);
        if (row < song.tracks.size())
        {
            state.selected_track = static_cast<int>(row);
        }
        if (row < song.tracks.size() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
        {
            state.open_editor = true;
            state.open_beat = std::max(QuartersAt(canvas, io.MousePos.x), 0.0);
        }
    }
}

void DrawPlayhead(ImDrawList* draw, const Canvas& canvas, const Transport& transport)
{
    const float x = std::floor(XOf(canvas, transport.Position()));
    draw->PushClipRect(ImVec2(canvas.lanes.Min.x - Px(7.0f), canvas.ruler.Min.y), canvas.lanes.Max, true);
    draw->AddRectFilled(ImVec2(x, canvas.ruler.Max.y - Px(2.0f)), ImVec2(x + Px(1.0f), canvas.lanes.Max.y),
                        theme::WithAlpha(theme::kText, 0.85f));
    draw->AddTriangleFilled(ImVec2(x - Px(5.5f), canvas.ruler.Max.y - Px(11.0f)),
                            ImVec2(x + Px(6.5f), canvas.ruler.Max.y - Px(11.0f)),
                            ImVec2(x + Px(0.5f), canvas.ruler.Max.y - Px(2.0f)), theme::kText);
    draw->PopClipRect();
}

void DrawCanvas(SongView* song, Transport& transport, ArrangementState& state)
{
    // A width or a height from a file takes the nearest that the arrangement offers.
    state.beat_width = std::clamp(state.beat_width, kMinBeatWidth, kMaxBeatWidth);
    state.track_height = StepTrackHeight(state.track_height, 0);

    // A vertical zoom from the keyboard changes the height of every track.
    const float old_height = state.track_height;
    if (state.vertical_zoom_steps != 0)
    {
        state.track_height = StepTrackHeight(state.track_height, state.vertical_zoom_steps);
        state.vertical_zoom_steps = 0;
    }

    // The song's scale, or the scale of an empty song.
    Canvas canvas;
    canvas.beats_per_bar = song != nullptr ? song->beats_per_bar : transport.BeatsPerBar();
    canvas.ticks_per_quarter = song != nullptr ? song->ticks_per_quarter : 48;
    canvas.beat_width = Px(state.beat_width);
    canvas.row_height = Px(state.track_height);
    // With the tempo row, the song runs on to the row's last point.
    const bool tempo_row = song != nullptr && state.tempo_row;
    const double length =
        song != nullptr ? static_cast<double>(tempo_row ? TempoRowEnd(*song) : song->length) / song->ticks_per_quarter
                        : static_cast<double>(kEmptyBars) * canvas.beats_per_bar;
    const std::size_t tracks = song != nullptr ? song->tracks.size() : 0;

    // The content: the headers and the song across, and the ruler, the rows under it and the tracks down. Some room
    // follows the song's end. A hidden tempo row ends its drag.
    const float header_width = Px(state.track_list_width);
    const float ruler_height = Px(kRulerHeight);
    const float marker_height = song != nullptr ? Px(kMarkerHeight) : 0.0f;
    const float tempo_height = tempo_row ? Px(kTempoHeight) : 0.0f;
    const float top = ruler_height + marker_height + tempo_height;
    if (!tempo_row)
    {
        state.tempo_drag.reset();
    }
    ImGui::SetNextWindowContentSize(
        ImVec2(header_width + static_cast<float>(length + canvas.beats_per_bar * 8.0) * canvas.beat_width,
               top + static_cast<float>(tracks) * canvas.row_height + Px(80.0f)));
    ImGui::BeginChild("##canvas", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None,
                      ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    const ImRect inner = ImGui::GetCurrentWindow()->InnerRect;
    canvas.scroll_x = ImGui::GetScrollX();
    canvas.scroll_y = ImGui::GetScrollY();
    canvas.ruler =
        ImRect(ImVec2(inner.Min.x + header_width, inner.Min.y), ImVec2(inner.Max.x, inner.Min.y + ruler_height));
    canvas.markers = ImRect(ImVec2(inner.Min.x + header_width, inner.Min.y + ruler_height),
                            ImVec2(inner.Max.x, inner.Min.y + ruler_height + marker_height));
    canvas.tempo =
        ImRect(ImVec2(inner.Min.x + header_width, canvas.markers.Max.y), ImVec2(inner.Max.x, inner.Min.y + top));
    canvas.headers = ImRect(ImVec2(inner.Min.x, inner.Min.y + top), ImVec2(inner.Min.x + header_width, inner.Max.y));
    canvas.lanes = ImRect(ImVec2(inner.Min.x + header_width, inner.Min.y + top), inner.Max);

    // A drag of the track list's edge sets the list's width. The band lies across the line at the list's right end, the
    // list's last pixel. A press in the band goes to the edge, not to the ruler, the rows or the lanes. The edge keeps
    // the press's distance from the line. A press alone then doesn't move it.
    const ImGuiIO& io = ImGui::GetIO();
    const float line = inner.Min.x + header_width - Px(0.5f);
    const ImRect edge(ImVec2(line - Px(kEdgeGrab) * 0.5f, inner.Min.y),
                      ImVec2(line + Px(kEdgeGrab) * 0.5f, inner.Max.y));
    const bool over_edge = ImGui::IsWindowHovered() && edge.Contains(io.MousePos);
    if (over_edge && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
    {
        state.resizing_track_list = true;
        state.track_list_grab = io.MousePos.x - (inner.Min.x + header_width);
    }
    if (state.resizing_track_list && !ImGui::IsMouseDown(ImGuiMouseButton_Left))
    {
        state.resizing_track_list = false;
    }
    if (state.resizing_track_list)
    {
        state.track_list_width = std::clamp((io.MousePos.x - state.track_list_grab - inner.Min.x) / Px(1.0f),
                                            kMinTrackListWidth, kMaxTrackListWidth);
    }
    if (over_edge || state.resizing_track_list)
    {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    }

    // The track at the top of the lanes stays there through a vertical zoom.
    if (state.track_height != old_height)
    {
        canvas.scroll_y *= state.track_height / old_height;
        ImGui::SetScrollY(canvas.scroll_y);
    }

    // A zoom from the keyboard keeps the playhead in place. With the playhead out of sight, the middle stays in place.
    HandleMouse(canvas, song, transport, state);
    if (state.zoom_steps != 0)
    {
        const float playhead = XOf(canvas, transport.Position());
        const bool visible = playhead >= canvas.lanes.Min.x && playhead <= canvas.lanes.Max.x;
        Zoom(canvas, state, std::pow(kZoomStep, static_cast<float>(state.zoom_steps)),
             visible ? playhead : (canvas.lanes.Min.x + canvas.lanes.Max.x) * 0.5f);
        state.zoom_steps = 0;
    }

    // Zoom Full shows the song from its start to its end.
    if (state.zoom_full)
    {
        const float width = canvas.lanes.GetWidth() / Px(1.0f);
        if (length > 0.0 && width > 0.0f)
        {
            state.beat_width = std::clamp(width / static_cast<float>(length), kMinBeatWidth, kMaxBeatWidth);
            canvas.beat_width = Px(state.beat_width);
        }
        canvas.scroll_x = 0.0f;
        ImGui::SetScrollX(0.0f);
        state.zoom_full = false;
    }

    // A zoom to a range fits the range into the lanes.
    if (state.zoom_range)
    {
        const auto [start, end] = *state.zoom_range;
        const float width = canvas.lanes.GetWidth() / Px(1.0f);
        if (end > start && width > 0.0f)
        {
            state.beat_width = std::clamp(width / static_cast<float>(end - start), kMinBeatWidth, kMaxBeatWidth);
            canvas.beat_width = Px(state.beat_width);
        }
        canvas.scroll_x = std::max(static_cast<float>(start) * canvas.beat_width, 0.0f);
        ImGui::SetScrollX(canvas.scroll_x);
        state.zoom_range.reset();
    }

    // A scroll that the program sets, such as the one that a file keeps.
    if (const std::optional<std::pair<double, float>> scroll = std::exchange(state.scroll_to, std::nullopt))
    {
        canvas.scroll_x = std::max(static_cast<float>(scroll->first) * canvas.beat_width, 0.0f);
        canvas.scroll_y = std::max(scroll->second * canvas.row_height, 0.0f);
        ImGui::SetScrollX(canvas.scroll_x);
        ImGui::SetScrollY(canvas.scroll_y);
    }
    state.left_quarter = canvas.scroll_x / canvas.beat_width;
    state.top_track = canvas.scroll_y / canvas.row_height;

    // The lanes, then the headers and the ruler over them, then the playhead over everything.
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->PushClipRect(canvas.lanes.Min, canvas.lanes.Max, true);
    if (song != nullptr)
    {
        DrawLanes(draw, canvas, *song, transport, state);
    }
    else
    {
        DrawGrid(draw, canvas);
    }
    draw->PopClipRect();
    draw->PushClipRect(canvas.headers.Min, canvas.headers.Max, true);
    draw->AddRectFilled(canvas.headers.Min, canvas.headers.Max, theme::kPanel);
    if (song != nullptr)
    {
        DrawTrackHeaders(draw, canvas, *song, state, !over_edge && !state.resizing_track_list);
    }
    draw->PopClipRect();

    // The palette that a click on a track's colour strip offers.
    if (ImGui::BeginPopup(kColourPopup))
    {
        const auto track = static_cast<std::size_t>(state.colour_popup_track);
        if (song != nullptr && state.colour_popup_track >= 0 && track < song->tracks.size())
        {
            if (std::optional<TrackColourPick> pick = DrawPalette(state.colour_popup_track, song->tracks[track].colour))
            {
                state.colour_pick = pick;
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndPopup();
    }

    // The track list's context menu. A line divides Copy Track and Paste Track from the other actions.
    if (ImGui::BeginPopup(kTrackMenu))
    {
        for (std::size_t i = 0; i < kTrackActions; i++)
        {
            if (i == static_cast<std::size_t>(TrackAction::kCopy))
            {
                ImGui::Separator();
            }

            if (ImGui::MenuItem(kTrackActionNames[i], nullptr, false, state.track_actions[i]))
            {
                state.track_action = static_cast<TrackAction>(i);
            }
        }
        ImGui::EndPopup();
    }
    draw->PushClipRect(canvas.ruler.Min, canvas.ruler.Max, true);
    DrawRuler(draw, canvas, transport);
    draw->PopClipRect();
    if (song != nullptr)
    {
        draw->PushClipRect(canvas.markers.Min, canvas.markers.Max, true);
        DrawMarkers(draw, canvas, *song, transport, state);
        draw->PopClipRect();
    }
    if (tempo_row)
    {
        draw->PushClipRect(canvas.tempo.Min, canvas.tempo.Max, true);
        DrawTempo(draw, canvas, *song, state);
        draw->PopClipRect();
    }
    DrawPlayhead(draw, canvas, transport);

    // The corner above the headers.
    const ImRect corner(inner.Min, ImVec2(inner.Min.x + header_width, inner.Min.y + ruler_height));
    draw->AddRectFilled(corner.Min, corner.Max, theme::kRaised);
    draw->AddRectFilled(ImVec2(corner.Min.x, corner.Max.y - Px(1.0f)), corner.Max, theme::kBorder);
    draw->AddRectFilled(ImVec2(corner.Max.x - Px(1.0f), corner.Min.y), corner.Max, theme::kBorder);
    std::array<char, 32> count{};
    if (tracks == 0)
    {
        std::snprintf(count.data(), count.size(), "No tracks");
    }
    else
    {
        std::snprintf(count.data(), count.size(), tracks == 1 ? "%zu track" : "%zu tracks", tracks);
    }
    // The count sits on the baseline of the ruler's numbers.
    DrawText(draw, theme::GetFonts().text, theme::kSmallSize,
             ImVec2(corner.Min.x + Px(14.0f), corner.Max.y - Px(16.0f)), theme::kTextMuted, count.data());

    // The marker row's header, under the corner.
    if (song != nullptr)
    {
        const ImRect header(ImVec2(inner.Min.x, canvas.markers.Min.y),
                            ImVec2(inner.Min.x + header_width, canvas.markers.Max.y));
        draw->AddRectFilled(header.Min, header.Max, theme::kRaised);
        draw->AddRectFilled(ImVec2(header.Min.x, header.Max.y - Px(1.0f)), header.Max, theme::kBorder);
        draw->AddRectFilled(ImVec2(header.Max.x - Px(1.0f), header.Min.y), header.Max, theme::kBorder);
        const ImVec2 caps = CapsSize("MARKERS");
        DrawCaps(draw, ImVec2(header.Min.x + Px(14.0f), header.GetCenter().y - caps.y * 0.5f), theme::kTextMuted,
                 "MARKERS");

        // A click on the header opens the marker row's menu. A chevron at the header's end shows that it opens. The
        // track list's edge stays free for a drag.
        ImGui::SetCursorScreenPos(header.Min);
        ImGui::InvisibleButton("##marker header", ImVec2(header.GetWidth() - Px(kEdgeGrab) * 0.5f, header.GetHeight()));
        const bool hovered = ImGui::IsItemHovered();
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
        {
            ImGui::OpenPopup(kMarkerMenu);
        }
        const float chevron = Px(10.0f);
        DrawIcon(draw, Icon::kChevronDown, ImVec2(header.Max.x - Px(12.0f) - chevron * 0.5f, header.GetCenter().y),
                 chevron, hovered ? theme::kText : theme::kTextMuted);
        if (hovered && !ImGui::IsPopupOpen(kMarkerMenu))
        {
            WrappedTooltip("Locate a marker, set the locators round a region, or zoom to it");
        }
        DrawMarkerMenu(*song, transport, state);
    }

    // The tempo row's header, under the marker row's.
    if (tempo_row)
    {
        DrawTempoHeader(
            draw,
            ImRect(ImVec2(inner.Min.x, canvas.tempo.Min.y), ImVec2(inner.Min.x + header_width, canvas.tempo.Max.y)),
            *song);
    }

    if (song == nullptr)
    {
        EmptyState(draw, canvas.lanes.Min, canvas.lanes.Max, "No sequence is open", state.empty_hint);
    }

    // Playback that runs off the right edge turns the page.
    if (transport.Playing())
    {
        const float playhead = XOf(canvas, transport.Position());
        if (playhead > canvas.lanes.Max.x - Px(16.0f) || playhead < canvas.lanes.Min.x)
        {
            ImGui::SetScrollX(std::max(canvas.scroll_x + playhead - canvas.lanes.Min.x - Px(32.0f), 0.0f));
        }
    }
    ImGui::EndChild();
}

// What a channel strip shows. The output's strip has no sends, pan, mute or solo. A track's strip records the user's
// moves of its controls in the track's view (TrackView::held and moved). While `busy`, the controls take no moves.
struct Strip
{
    const char* name = "";
    ImU32 colour = 0;
    float* volume = nullptr;
    float* pan = nullptr;
    std::array<float, kSendCount>* sends = nullptr;
    std::size_t buses = kSendCount; // the sends that the console's tracks have
    bool* mute = nullptr;
    bool* solo = nullptr;
    std::array<float, 2> meter = {kSilence, kSilence};
    float* peak = nullptr;
    TrackView* track = nullptr;
    bool selected = false;
    bool busy = false;
};

// Follows the user's hold of the control that the last item drew, in `track`'s view. The control showed `before` and
// shows `after`. A hold that ends with a new value becomes a move.
void FollowHold(TrackView* track, MixControl control, float before, float after)
{
    if (track == nullptr)
    {
        return;
    }

    if (ImGui::IsItemActivated())
    {
        track->held = MixMove{control, before, before};
    }
    if (ImGui::IsItemDeactivated() && track->held && track->held->control == control)
    {
        if (after != track->held->from)
        {
            track->moved = MixMove{control, track->held->from, after};
        }
        else
        {
            track->held.reset();
        }
    }
}

// Starts and ends the controls of a strip that are disabled while the program is busy. The controls keep their look.
void BeginHeldOff(bool busy)
{
    ImGui::PushStyleVar(ImGuiStyleVar_DisabledAlpha, 1.0f);
    ImGui::BeginDisabled(busy);
}

void EndHeldOff()
{
    ImGui::EndDisabled();
    ImGui::PopStyleVar();
}

// Draws the decibels of the faders and the meters, right-aligned at `right`. `top` is the height of 6 dB, and `bottom`
// the height of silence. A mark that would crowd the one above it, or the mark for silence at the bottom, is left out.
void DrawDecibelScale(ImDrawList* draw, float right, float top, float bottom)
{
    constexpr std::array<float, 9> kMarks = {6.0f, 0.0f, -6.0f, -12.0f, -18.0f, -24.0f, -36.0f, -48.0f, kSilence};
    ImFont* figures = theme::GetFonts().figures;
    const float silence_top = std::floor(bottom - TextSize(figures, theme::kCapsSize, "∞").y * 0.5f);
    float free_from = top - Px(20.0f);
    for (const float mark : kMarks)
    {
        const std::string text = mark == kSilence ? std::string("∞") : std::to_string(static_cast<int>(mark));
        const ImVec2 size = TextSize(figures, theme::kCapsSize, text);
        const float text_top = std::floor(top + (bottom - top) * (1.0f - FaderPosition(mark)) - size.y * 0.5f);
        if (text_top < free_from || (mark != kSilence && text_top + size.y > silence_top))
        {
            continue;
        }

        DrawText(draw, figures, theme::kCapsSize, ImVec2(right - size.x, text_top), theme::kTextMuted, text);
        free_from = text_top + size.y;
    }
}

// Draws the M and S buttons side by side, from `pos`. A click on either one switches its flag.
void DrawMuteSolo(ImDrawList* draw, ImVec2 pos, ImVec2 button, float gap, bool* mute, bool* solo)
{
    const ImVec2 solo_pos(pos.x + button.x + gap, pos.y);
    ImGui::SetCursorScreenPos(pos);
    if (ImGui::InvisibleButton("##mute", button))
    {
        *mute = !*mute;
    }
    DrawLetterButton(draw, pos, ImVec2(pos.x + button.x, pos.y + button.y), "M", *mute, theme::kMute,
                     ImGui::IsItemHovered());
    ImGui::SetCursorScreenPos(solo_pos);
    if (ImGui::InvisibleButton("##solo", button))
    {
        *solo = !*solo;
    }
    DrawLetterButton(draw, solo_pos, ImVec2(solo_pos.x + button.x, solo_pos.y + button.y), "S", *solo, theme::kSolo,
                     ImGui::IsItemHovered());
}

// Draws a value in a dark box.
void DrawValueBox(ImDrawList* draw, ImVec2 min, ImVec2 max, const std::string& value, ImU32 colour)
{
    draw->AddRectFilled(min, max, theme::kDisplay, Px(4.0f));
    ImFont* figures = theme::GetFonts().figures;
    const ImVec2 size = TextSize(figures, theme::kSmallSize, value);
    draw->PushClipRect(min, max, true);
    DrawText(draw, figures, theme::kSmallSize, ImVec2((min.x + max.x - size.x) * 0.5f, (min.y + max.y - size.y) * 0.5f),
             colour, value);
    draw->PopClipRect();
}

// The tooltip of control `control` of `track`'s strip: the control's name, and what else an edit of it changes. Without
// a track, or where an edit changes the track alone, the name stands alone.
std::string ControlTooltip(const TrackView* track, MixControl control)
{
    constexpr std::array<const char*, kMixControlCount> kNames = {"Volume", "Pan", "Send to FX A", "Send to FX B",
                                                                  "Send to FX C"};
    const auto index = static_cast<std::size_t>(control);
    const std::string name = kNames[index];
    if (track == nullptr || track->mix_reach[index].empty())
    {
        return name;
    }

    return name + ". An edit also changes " + track->mix_reach[index] + ".";
}

// The tooltip of the equals sign on the name of `track`'s strip: what else an edit of each of its controls changes.
// Empty where every control's edit changes the track alone.
std::string LinkedMixTooltip(const TrackView& track)
{
    std::string lines;
    for (std::size_t control = 0; control < kMixControlCount; control++)
    {
        if (!track.mix_reach[control].empty())
        {
            const std::string name = ControlTooltip(nullptr, static_cast<MixControl>(control));
            lines += "\n" + name + ": " + track.mix_reach[control] + ".";
        }
    }
    if (lines.empty())
    {
        return lines;
    }

    const std::string heading =
        "Linked mix. Other tracks or sounds run some of this track's commands. An edit of a control changes them too:";
    return heading + lines;
}

// Draws a strip's level and its loudest peak since a click on the peak cleared it, in boxes either side of `centre` at
// `y`. The peak turns red above 0 dB. A strip without a peak shows silence there.
void DrawLevels(ImDrawList* draw, float centre, float y, float level, float* peak)
{
    const float shown = peak != nullptr ? *peak : kSilence;
    const ImVec2 box(Px(36.0f), Px(19.0f));
    const ImVec2 level_min(centre - box.x - Px(1.0f), y);
    const ImVec2 peak_min(centre + Px(1.0f), y);
    DrawValueBox(draw, level_min, ImVec2(level_min.x + box.x, y + box.y), LevelText(level), theme::kText);
    DrawValueBox(draw, peak_min, ImVec2(peak_min.x + box.x, y + box.y), LevelText(shown),
                 shown > 0.0f ? theme::kMeterHigh : theme::kTextMuted);
    if (peak != nullptr)
    {
        ImGui::SetCursorScreenPos(peak_min);
        if (ImGui::InvisibleButton("##peak", box))
        {
            *peak = kSilence;
        }
        ItemTooltip("Peak. Click to clear it.");
    }
}

// Draws a strip's meters, left and right, and the scale that they share with the fader. The fader stands at `y`,
// `height` high, left of `centre`.
void DrawFaderMeters(ImDrawList* draw, float centre, float y, float height, const std::array<float, 2>& meter)
{
    const float scale_top = y + Px(12.0f);
    const float scale_bottom = y + height - Px(12.0f);
    const float meter_x = centre + Px(23.0f);
    DrawMeter(draw, ImVec2(meter_x, scale_top), ImVec2(meter_x + Px(4.0f), scale_bottom), meter[0]);
    DrawMeter(draw, ImVec2(meter_x + Px(6.0f), scale_top), ImVec2(meter_x + Px(10.0f), scale_bottom), meter[1]);
    DrawDecibelScale(draw, meter_x - Px(4.0f), scale_top, scale_bottom);
}

// Draws a channel strip at the cursor. The strip holds the sends, the pan, M and S, the level and the peak, the fader
// with its scale and meters, and the name at the bottom in the track's colour. A track whose mix other tracks or sounds
// share shows an equals sign after its name. Returns true when the strip is clicked.
bool DrawStrip(const Strip& strip, ImVec2 size)
{
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const ImVec2 end(pos.x + size.x, pos.y + size.y);
    const ImGuiIO& io = ImGui::GetIO();
    const bool clicked = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) &&
                         ImGui::IsMouseClicked(ImGuiMouseButton_Left) && ImRect(pos, end).Contains(io.MousePos);
    draw->AddRectFilled(pos, end, strip.selected ? theme::Mix(theme::kRaised, theme::kAccent, 0.12f) : theme::kRaised,
                        Px(8.0f));
    draw->AddRect(pos, end, strip.selected ? theme::WithAlpha(theme::kAccent, 0.6f) : theme::kBorder, Px(8.0f),
                  Px(1.0f));
    const float centre = std::floor(pos.x + size.x * 0.5f);
    ImFont* text = theme::GetFonts().text;

    // A slot for each send. The output's strip leaves the space empty, and the faders line up.
    float y = pos.y + Px(8.0f);
    if (strip.sends != nullptr)
    {
        BeginHeldOff(strip.busy);
        for (std::size_t bus = 0; bus < kSendCount; bus++)
        {
            const std::string name = std::string("FX ") + static_cast<char>('A' + bus);
            const auto control = static_cast<MixControl>(static_cast<int>(MixControl::kSendA) + static_cast<int>(bus));
            float& level = (*strip.sends)[bus];
            const float before = level;
            const ImVec2 slot(pos.x + Px(6.0f), y + static_cast<float>(bus) * Px(21.0f));
            const ImVec2 slot_size(size.x - Px(12.0f), Px(18.0f));
            ImGui::PushID(static_cast<int>(bus));
            ImGui::SetCursorScreenPos(slot);
            if (bus < strip.buses)
            {
                SendSlider("##send", name.c_str(), &level, slot_size);
                FollowHold(strip.track, control, before, level);
                ItemTooltip(ControlTooltip(strip.track, control));
            }
            else
            {
                // A send that the console's tracks lack: an empty slot with the bus's name.
                ImGui::InvisibleButton("##send", slot_size);
                draw->AddRect(slot, ImVec2(slot.x + slot_size.x, slot.y + slot_size.y), theme::kBorder, Px(4.0f),
                              Px(1.0f));
                const ImVec2 name_size = TextSize(text, theme::kSmallSize, name);
                DrawText(draw, text, theme::kSmallSize,
                         ImVec2(slot.x + Px(6.0f), std::floor(slot.y + (slot_size.y - name_size.y) * 0.5f)),
                         theme::kTextMuted, name);
                ItemTooltip("The 3DS's tracks send to FX A and FX B only.");
            }
            ImGui::PopID();
        }
        EndHeldOff();
    }
    y += Px(70.0f);

    // The pan knob and its value.
    const float radius = Px(13.0f);
    if (strip.pan != nullptr)
    {
        const float before = *strip.pan;
        ImGui::SetCursorScreenPos(ImVec2(centre - radius, y));
        BeginHeldOff(strip.busy);
        PanKnob("##pan", strip.pan, radius);
        FollowHold(strip.track, MixControl::kPan, before, *strip.pan);
        if (strip.track != nullptr && !strip.track->mix_reach[static_cast<std::size_t>(MixControl::kPan)].empty())
        {
            ItemTooltip(ControlTooltip(strip.track, MixControl::kPan));
        }
        EndHeldOff();
        const std::string pan = PanText(*strip.pan);
        ImFont* figures = theme::GetFonts().figures;
        const ImVec2 pan_size = TextSize(figures, theme::kSmallSize, pan);
        DrawText(draw, figures, theme::kSmallSize, ImVec2(centre - pan_size.x * 0.5f, y + radius * 2.0f + Px(3.0f)),
                 theme::kTextMuted, pan);
    }
    else
    {
        const ImVec2 label_size = CapsSize("Output");
        DrawCaps(draw, ImVec2(centre - label_size.x * 0.5f, y + Px(10.0f)), theme::kTextMuted, "Output");
    }
    y += Px(46.0f);

    // M and S.
    if (strip.mute != nullptr && strip.solo != nullptr)
    {
        const ImVec2 button(Px(26.0f), Px(20.0f));
        DrawMuteSolo(draw, ImVec2(centre - button.x - Px(2.0f), y), button, Px(4.0f), strip.mute, strip.solo);
    }

    // The level and the peak. The FX channels' strips put theirs in the same place.
    y = pos.y + Px(kLevelsTop);
    DrawLevels(draw, centre, y, *strip.volume, strip.peak);
    y += Px(27.0f);

    // The fader, the meters, and the scale that they share between them.
    const float name_height = Px(28.0f);
    const float fader_height = std::max(end.y - name_height - Px(8.0f) - y, Px(48.0f));
    ImGui::SetCursorScreenPos(ImVec2(centre - Px(34.0f), y));
    const float before = *strip.volume;
    BeginHeldOff(strip.busy);
    Fader("##fader", strip.volume, ImVec2(Px(30.0f), fader_height));
    FollowHold(strip.track, MixControl::kVolume, before, *strip.volume);
    if (strip.track != nullptr && !strip.track->mix_reach[static_cast<std::size_t>(MixControl::kVolume)].empty())
    {
        ItemTooltip(ControlTooltip(strip.track, MixControl::kVolume));
    }
    EndHeldOff();
    DrawFaderMeters(draw, centre, y, fader_height, strip.meter);

    // The name, in a box of the track's colour. A linked mix shows an equals sign at the end of the box. The
    // arrangement marks a linked part the same way.
    const ImVec2 name_min(pos.x + Px(5.0f), end.y - name_height);
    const ImVec2 name_max(end.x - Px(5.0f), end.y - Px(5.0f));
    draw->AddRectFilled(name_min, name_max, strip.colour, Px(4.0f));
    const std::string linked = strip.track != nullptr ? LinkedMixTooltip(*strip.track) : std::string();
    const ImVec2 mark_size = TextSize(text, theme::kSmallSize, "=");
    const float mark_left = name_max.x - Px(6.0f) - mark_size.x;
    const float name_right = linked.empty() ? name_max.x - Px(4.0f) : mark_left - Px(3.0f);
    const std::string shown = FitText(text, theme::kSmallSize, strip.name, name_right - name_min.x - Px(4.0f));
    const ImVec2 name_size = TextSize(text, theme::kSmallSize, shown);
    DrawText(draw, text, theme::kSmallSize,
             ImVec2(std::max(centre - name_size.x * 0.5f, name_min.x + Px(4.0f)),
                    name_min.y + (name_max.y - name_min.y - name_size.y) * 0.5f),
             theme::TextOn(strip.colour), shown);
    if (!linked.empty())
    {
        DrawText(draw, text, theme::kSmallSize,
                 ImVec2(mark_left, name_min.y + (name_max.y - name_min.y - mark_size.y) * 0.5f),
                 theme::TextOn(strip.colour), "=");
        ImGui::SetCursorScreenPos(name_min);
        ImGui::InvisibleButton("##linked", ImVec2(name_max.x - name_min.x, name_max.y - name_min.y));
        ItemTooltip(linked);
    }

    // The strip takes its place in the layout.
    ImGui::SetCursorScreenPos(pos);
    ImGui::Dummy(size);

    return clicked;
}

Strip TrackStrip(TrackView& track, bool selected, bool busy)
{
    Strip strip;
    strip.name = track.name.c_str();
    strip.colour = track.colour;
    strip.volume = &track.volume;
    strip.pan = &track.pan;
    strip.sends = &track.sends;
    strip.mute = &track.mute;
    strip.solo = &track.solo;
    strip.meter = track.meter;
    strip.peak = &track.peak;
    strip.track = &track;
    strip.selected = selected;
    strip.busy = busy;

    return strip;
}

Strip OutputStrip(MasterBus& master)
{
    Strip strip;
    strip.name = "Stereo Out";
    strip.colour = theme::kControlActive;
    strip.volume = &master.volume;
    strip.meter = master.meter;
    strip.peak = &master.peak;

    return strip;
}

// Draws an insert of an FX channel from `min` to `max`, with the name of its effect. The insert of the open effect is
// lit. The name of an idle effect shows grey.
void DrawInsert(ImDrawList* draw, ImVec2 min, ImVec2 max, const std::string& name, bool open, bool hovered, bool idle)
{
    const ImU32 fill = open      ? theme::Mix(theme::kControl, theme::kAccent, 0.3f)
                       : hovered ? theme::kControlHover
                                 : theme::kControl;
    draw->AddRectFilled(min, max, fill, Px(4.0f));
    if (open)
    {
        draw->AddRect(min, max, theme::kAccent, Px(4.0f), Px(1.0f));
    }

    ImFont* text = theme::GetFonts().text;
    const std::string shown = FitText(text, theme::kSmallSize, name, max.x - min.x - Px(14.0f));
    const ImVec2 size = TextSize(text, theme::kSmallSize, shown);
    DrawText(draw, text, theme::kSmallSize, ImVec2(min.x + Px(8.0f), (min.y + max.y - size.y) * 0.5f),
             idle ? theme::kTextMuted : theme::kText, shown);
}

// Draws an empty insert from `min` to `max`, with a plus in the middle.
void DrawEmptyInsert(ImDrawList* draw, ImVec2 min, ImVec2 max, bool hovered)
{
    draw->AddRect(min, max, hovered ? theme::kBorderStrong : theme::kBorder, Px(4.0f), Px(1.0f));
    const ImVec2 centre(std::floor((min.x + max.x) * 0.5f), std::floor((min.y + max.y) * 0.5f));
    const float arm = Px(4.5f);
    const ImU32 colour = hovered ? theme::kText : theme::kTextMuted;
    draw->AddLine(ImVec2(centre.x - arm, centre.y), ImVec2(centre.x + arm, centre.y), colour, Px(1.5f));
    draw->AddLine(ImVec2(centre.x, centre.y - arm), ImVec2(centre.x, centre.y + arm), colour, Px(1.5f));
}

// Draws the strip of FX channel `channel` at the cursor: its inserts at the top, the level and the peak, the fader with
// its scale and meters, and its name at the bottom. A click on an effect, or a choice in an empty insert's menu, sets
// `fx.clicked`. A hold of the fader that ends with a new level sets `fx.moved`.
void DrawFxStrip(std::size_t channel, FxChannels& fx, ImVec2 size)
{
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const ImVec2 end(pos.x + size.x, pos.y + size.y);
    const float centre = std::floor(pos.x + size.x * 0.5f);
    draw->AddRectFilled(pos, end, theme::kRaised, Px(8.0f));
    draw->AddRect(pos, end, theme::kBorder, Px(8.0f), Px(1.0f));

    // An insert for each effect, then an empty one.
    DrawCaps(draw, ImVec2(pos.x + Px(8.0f), pos.y + Px(12.0f)), theme::kTextMuted, "Inserts");
    float y = pos.y + Px(12.0f) + CapsSize("Inserts").y + Px(8.0f);
    const std::vector<std::string>& effects = fx.effects[channel];
    for (std::size_t i = 0; i <= effects.size(); i++)
    {
        const EffectPlace place{channel, i};
        const ImVec2 min(pos.x + Px(6.0f), y);
        const ImVec2 max(end.x - Px(6.0f), y + Px(kInsertHeight));
        ImGui::PushID(static_cast<int>(i));
        ImGui::SetCursorScreenPos(min);
        const bool clicked = ImGui::InvisibleButton("##insert", ImVec2(max.x - min.x, max.y - min.y));
        const bool hovered = ImGui::IsItemHovered();
        if (i < effects.size())
        {
            const bool idle = i < fx.idle[channel].size() && fx.idle[channel][i];
            DrawInsert(draw, min, max, effects[i], fx.open == place, hovered, idle);
            ItemTooltip(idle ? effects[i] + "\n" + fx.idle_note : effects[i]);
            if (clicked)
            {
                fx.clicked = place;
            }
        }
        else
        {
            // A click opens the menu of the kinds of effect.
            DrawEmptyInsert(draw, min, max, hovered);
            ItemTooltip("Add an effect");
            if (clicked)
            {
                ImGui::OpenPopup("##kinds");
            }
            if (ImGui::BeginPopup("##kinds"))
            {
                PushMenuSpacing();
                for (std::size_t kind = 0; kind < fx.kinds.size(); kind++)
                {
                    if (ImGui::MenuItem(fx.kinds[kind].c_str()))
                    {
                        fx.clicked = place;
                        fx.kind = kind;
                    }
                }
                ImGui::PopStyleVar();
                ImGui::EndPopup();
            }
        }
        ImGui::PopID();
        y += Px(kInsertHeight) + Px(4.0f);
    }

    // The level and the peak, then the fader, in line with the tracks' strips. More inserts than fit push them down.
    y = std::max(pos.y + Px(kLevelsTop), y + Px(2.0f));
    DrawLevels(draw, centre, y, fx.volumes[channel], &fx.peaks[channel]);
    y += Px(27.0f);
    const float fader_height = std::max(end.y - Px(28.0f) - Px(8.0f) - y, Px(48.0f));
    float& volume = fx.volumes[channel];
    const float before = volume;
    ImGui::SetCursorScreenPos(ImVec2(centre - Px(34.0f), y));
    Fader("##fader", &volume, ImVec2(Px(30.0f), fader_height));
    if (ImGui::IsItemActivated())
    {
        fx.held = FxMove{channel, before, before};
    }
    if (ImGui::IsItemDeactivated() && fx.held && fx.held->channel == channel)
    {
        if (volume != fx.held->from)
        {
            fx.moved = FxMove{channel, fx.held->from, volume};
        }
        fx.held.reset();
    }
    DrawFaderMeters(draw, centre, y, fader_height, fx.meters[channel]);

    // The name, in a box at the bottom.
    const std::string name = std::string("FX ") + static_cast<char>('A' + channel);
    const ImU32 kColour = theme::Mix(theme::kAccent, theme::kRaised, 0.45f);
    const ImVec2 name_min(pos.x + Px(5.0f), end.y - Px(28.0f));
    const ImVec2 name_max(end.x - Px(5.0f), end.y - Px(5.0f));
    draw->AddRectFilled(name_min, name_max, kColour, Px(4.0f));
    ImFont* text = theme::GetFonts().text;
    const ImVec2 name_size = TextSize(text, theme::kSmallSize, name);
    DrawText(draw, text, theme::kSmallSize,
             ImVec2(std::floor((name_min.x + name_max.x - name_size.x) * 0.5f),
                    name_min.y + (name_max.y - name_min.y - name_size.y) * 0.5f),
             theme::TextOn(kColour), name);

    // The strip takes its place in the layout.
    ImGui::SetCursorScreenPos(pos);
    ImGui::Dummy(size);
}

// The selected track, or nullptr when there's none.
TrackView* SelectedTrack(SongView* song, const ArrangementState& state)
{
    if (song == nullptr || state.selected_track < 0 || state.selected_track >= static_cast<int>(song->tracks.size()))
    {
        return nullptr;
    }

    return &song->tracks[static_cast<std::size_t>(state.selected_track)];
}

// The selected part when it's on the selected track, or nullptr.
const PartView* SelectedPart(const TrackView& track, const ArrangementState& state)
{
    if (state.selected_part_track != state.selected_track || state.selected_part < 0 ||
        state.selected_part >= static_cast<int>(track.parts.size()))
    {
        return nullptr;
    }

    return &track.parts[static_cast<std::size_t>(state.selected_part)];
}

// The top of the inspector: the track's name in a box of its colour, with its M and S buttons. Returns the bottom of
// the box.
float DrawTrackCard(ImDrawList* draw, ImVec2 pos, float width, TrackView& track)
{
    const float height = Px(36.0f);
    const ImVec2 end(pos.x + width, pos.y + height);
    draw->AddRectFilled(pos, end, track.colour, Px(6.0f));

    const ImVec2 button(Px(24.0f), Px(20.0f));
    const float gap = Px(4.0f);
    const ImVec2 mute(end.x - Px(8.0f) - button.x * 2.0f - gap, std::floor(pos.y + (height - button.y) * 0.5f));
    DrawMuteSolo(draw, mute, button, gap, &track.mute, &track.solo);

    ImFont* strong = theme::GetFonts().strong;
    const std::string shown = FitText(strong, theme::kTextSize, track.name, mute.x - Px(6.0f) - pos.x - Px(10.0f));
    const ImVec2 name_size = TextSize(strong, theme::kTextSize, shown);
    DrawText(draw, strong, theme::kTextSize, ImVec2(pos.x + Px(10.0f), pos.y + (height - name_size.y) * 0.5f),
             theme::TextOn(track.colour), shown);

    return end.y;
}

// A property in the inspector, with its value as text. An empty value means there's none.
struct Property
{
    const char* label;
    std::string value;
};

// The heading of a section of the inspector as drawn: the bottom of its line, and whether the section is open.
struct SectionHeading
{
    float bottom = 0.0f;
    bool open = true;
};

// Whether the section titled `title` of the current zone is open. Dear ImGui keeps whether each section is open. The
// section starts open while `open_at_first` is set.
bool SectionOpen(const char* title, bool open_at_first)
{
    ImGui::PushID(title);
    const bool open = ImGui::GetStateStorage()->GetBool(ImGui::GetID("open"), open_at_first);
    ImGui::PopID();

    return open;
}

// The height of the heading of a section titled `title`: from its top to the bottom that DrawSectionHeading returns.
float SectionHeadingHeight(const char* title)
{
    return CapsSize(title).y + Px(9.0f);
}

// Draws the heading of a section of the inspector or the browser: a chevron and a title in capitals over a line, with
// `detail` at the right. A click on the heading opens or closes the section (SectionOpen).
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

// Draws a row for each property of a section of the inspector. Each row has the label on the left and the value on the
// right. Returns the bottom of the last row.
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

// The names that the inspector's menu of banks gives the bank slots of `slots` that hold banks, and the slot of each. A
// bank in two slots shows its slot after its name.
struct BankMenu
{
    std::vector<std::string> names;
    std::vector<int> slots;
};

BankMenu BankMenuOf(std::span<const SlotView> slots)
{
    BankMenu menu;
    for (std::size_t slot = 0; slot < slots.size(); slot++)
    {
        const std::string& bank = slots[slot].bank;
        if (bank.empty())
        {
            continue;
        }

        const auto same = [&bank](const SlotView& other)
        {
            return other.bank == bank;
        };
        const bool twice = std::count_if(slots.begin(), slots.end(), same) > 1;
        menu.names.push_back(twice ? bank + " (slot " + std::to_string(slot) + ")" : bank);
        menu.slots.push_back(static_cast<int>(slot));
    }

    return menu;
}

// Draws the inspector's rows of a track's instrument: its bank and its program. While the song lists the sound's bank
// slots, each row holds a menu: the banks in the slots, and the programs of the track's bank. The bank's menu lies
// under its label, at the rows' width: banks have long names. The Sound section lays out its pan curve the same way. A
// choice waits in the track's view (TrackView::instrument_choice). A bank keeps the track's program where it has one,
// and takes its first program otherwise. While `busy`, and for a track without notes, the rows show text. Returns the
// bottom of the rows.
float DrawInstrumentRows(ImDrawList* draw, ImVec2 pos, float width, const SongView& song, TrackView& track,
                         ArrangementState& state, bool busy)
{
    const std::size_t slot = static_cast<std::size_t>(std::max(track.slot, 0));
    if (busy || track.program < 0 || track.slot < 0 || slot >= song.slots.size() || song.slots[slot].programs.empty())
    {
        const std::array<Property, 2> instrument = {
            Property{"Bank", track.bank},
            Property{"Program", track.program < 0 ? std::string() : std::to_string(track.program)},
        };
        return DrawPropertyRows(draw, pos, width, instrument);
    }

    // The bank's label, with its menu under it. Another bank keeps the program where it has it.
    ImFont* text = theme::GetFonts().text;
    const float frame = ImGui::GetFrameHeight();
    const float gap = Px(kInstrumentGap);
    const ImVec2 bank_size = TextSize(text, theme::kTextSize, "Bank");
    const float label_height = std::floor(bank_size.y + gap);
    DrawText(draw, text, theme::kTextSize, ImVec2(pos.x, pos.y + std::floor((label_height - bank_size.y) * 0.5f)),
             theme::kTextMuted, "Bank");

    // A linked instrument shows an equals sign at the end of the label's row. Linked parts and mixes show the same
    // sign. The menus say what else an edit changes.
    const std::string also =
        track.instrument_reach.empty() ? std::string() : "An edit also changes " + track.instrument_reach + ".";
    if (!also.empty())
    {
        const ImVec2 mark_size = TextSize(text, theme::kTextSize, "=");
        const ImVec2 mark_min(pos.x + width - mark_size.x, pos.y + std::floor((label_height - mark_size.y) * 0.5f));
        DrawText(draw, text, theme::kTextSize, mark_min, theme::kText, "=");
        ImGui::SetCursorScreenPos(mark_min);
        ImGui::InvisibleButton("##linked", mark_size);
        ItemTooltip("Linked instrument. Other tracks or sounds run some of this track's commands. " + also);
    }

    const BankMenu banks = BankMenuOf(song.slots);
    const auto found = std::find(banks.slots.begin(), banks.slots.end(), track.slot);
    int chosen_bank = found != banks.slots.end() ? static_cast<int>(found - banks.slots.begin()) : -1;
    ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + label_height));
    ImGui::SetNextWindowSizeConstraints(ImVec2(Px(kInstrumentListWidth), 0.0f), ImVec2(FLT_MAX, FLT_MAX));
    const bool bank_chosen = FilteredCombo("##bank", banks.names, chosen_bank, nullptr, state.instrument_filter, width);
    ItemTooltip(also);
    if (bank_chosen && chosen_bank >= 0)
    {
        const int to = banks.slots[static_cast<std::size_t>(chosen_bank)];
        const std::vector<uint32_t>& programs = song.slots[static_cast<std::size_t>(to)].programs;
        const bool kept = std::find(programs.begin(), programs.end(), track.program) != programs.end();
        if (!programs.empty())
        {
            track.instrument_choice = InstrumentChoice{to, kept ? track.program : static_cast<int>(programs.front())};
        }
    }

    // The program's label, with its menu of the programs of the track's bank beside it.
    const float row = pos.y + label_height + frame + std::floor(gap * 0.5f);
    const ImVec2 program_size = TextSize(text, theme::kTextSize, "Program");
    DrawText(draw, text, theme::kTextSize, ImVec2(pos.x, row + std::floor((frame + gap - program_size.y) * 0.5f)),
             theme::kTextMuted, "Program");
    const std::vector<uint32_t>& programs = song.slots[slot].programs;
    std::vector<std::string> names;
    int chosen_program = -1;
    for (std::size_t i = 0; i < programs.size(); i++)
    {
        names.push_back("Program " + std::to_string(programs[i]));
        chosen_program = static_cast<int>(programs[i]) == track.program ? static_cast<int>(i) : chosen_program;
    }
    const float menu_left = pos.x + Px(kInstrumentLabelWidth);
    ImGui::SetCursorScreenPos(ImVec2(menu_left, row + std::floor(gap * 0.5f)));
    ImGui::SetNextWindowSizeConstraints(ImVec2(Px(kInstrumentListWidth), 0.0f), ImVec2(FLT_MAX, FLT_MAX));
    const bool program_chosen =
        FilteredCombo("##program", names, chosen_program, nullptr, state.instrument_filter, pos.x + width - menu_left);
    ItemTooltip(also);
    if (program_chosen && chosen_program >= 0)
    {
        track.instrument_choice =
            InstrumentChoice{track.slot, static_cast<int>(programs[static_cast<std::size_t>(chosen_program)])};
    }

    return row + frame + gap;
}

// Draws the inspector's Sequence section under `pos`: a switch between the two ways to edit the shown sequence. A
// timeline holds each note by itself. NintendoWare's MIDI converter laid the sequence out that way from a MIDI file. A
// program holds the commands as the console's bytes hold them. A click on the other way sets the song's `mode_request`.
// A sequence that the converter didn't write can't be a timeline, and the tooltip says why. Returns the bottom of the
// switch.
float DrawModeSwitch(ImVec2 pos, float width, SongView& song, bool busy)
{
    const std::array<std::string, 2> kLabels = {"Timeline", "Program"};
    const std::array<uint8_t, 2> lit = {static_cast<uint8_t>(song.timeline ? 1 : 0),
                                        static_cast<uint8_t>(song.timeline ? 0 : 1)};
    const ImVec2 size(width, Px(kSwitchHeight));
    const float top = pos.y + Px(4.0f);
    ImGui::SetCursorScreenPos(ImVec2(pos.x, top));
    const int clicked = SegmentedSwitch("##mode", kLabels, lit, size);

    // The half under the mouse explains its way, or why the sequence can't take it.
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const bool over = ImGui::IsWindowHovered() && mouse.y >= top && mouse.y < top + size.y && mouse.x >= pos.x &&
                      mouse.x < pos.x + width;
    const bool timeline_half = mouse.x < pos.x + width * 0.5f;
    if (over && !ImGui::IsAnyMouseDown())
    {
        WrappedTooltip(timeline_half ? (song.timeline_refused.empty()
                                            ? "Edit the sequence as NintendoWare's MIDI converter wrote it from a MIDI "
                                              "file: each note stands alone, and the loop's edges move. A save writes "
                                              "the converter's subroutines again."
                                            : song.timeline_refused)
                                     : "Edit the commands as the console's bytes hold them: subroutines, calls, "
                                       "jumps and labels.");
    }
    if (!busy && clicked == 0 && !song.timeline && song.timeline_refused.empty())
    {
        song.mode_request = true;
    }
    else if (!busy && clicked == 1 && song.timeline)
    {
        song.mode_request = false;
    }

    return top + size.y + Px(4.0f);
}

// Draws the inspector's row of a track's colour, under the track's properties: the label on the left and a swatch of
// the colour on the right. While the tracks' colours are Custom, a click on the swatch offers the palette. Returns the
// bottom of the row.
float DrawColourProperty(ImDrawList* draw, ImVec2 pos, float width, const TrackView& track, ArrangementState& state)
{
    ImFont* text = theme::GetFonts().text;
    const float height = Px(kPropertyHeight);
    const ImVec2 label_size = TextSize(text, theme::kTextSize, "Colour");
    DrawText(draw, text, theme::kTextSize, ImVec2(pos.x, pos.y + (height - label_size.y) * 0.5f), theme::kTextMuted,
             "Colour");

    // The swatch.
    const ImVec2 size(Px(28.0f), Px(14.0f));
    const ImVec2 min(pos.x + width - size.x, std::floor(pos.y + (height - size.y) * 0.5f));
    const ImVec2 max(min.x + size.x, min.y + size.y);
    ImGui::SetCursorScreenPos(min);
    ImGui::InvisibleButton("##colour", size);
    const bool hovered = state.custom_colours && ImGui::IsItemHovered();
    draw->AddRectFilled(min, max, track.colour, Px(3.0f));
    if (hovered)
    {
        const float out = Px(2.0f);
        draw->AddRect(ImVec2(min.x - out, min.y - out), ImVec2(max.x + out, max.y + out), theme::kText, Px(4.0f),
                      Px(1.5f));
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }
    if (!state.custom_colours)
    {
        ItemTooltip(std::string("Choose Custom track colours in ") + kPreferencesItem + " to set this colour");
    }
    else if (ImGui::IsItemClicked())
    {
        ImGui::OpenPopup(kColourPopup);
    }

    // The palette.
    if (ImGui::BeginPopup(kColourPopup))
    {
        if (std::optional<TrackColourPick> pick = DrawPalette(state.selected_track, track.colour))
        {
            state.colour_pick = pick;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    return pos.y + height;
}

// The Undo History's rows and its line, in a child window that scrolls. Each row shows the action's number, name and
// time, with the details below. The line lies under the last action that's done. Returns the number of actions that the
// user chose to have done.
std::optional<std::size_t> DrawHistoryRows(std::span<const HistoryRow> rows, std::size_t done)
{
    ImGui::BeginChild("##rows", ImVec2(0.0f, 0.0f));
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const float row_height = Px(kHistoryRowHeight);
    const float top = origin.y + Px(kHistoryTop);
    const float margin = Px(8.0f);
    const float view_top = ImGui::GetWindowPos().y;
    const float view_bottom = view_top + ImGui::GetWindowHeight();
    const auto line_y = [top, row_height](std::size_t place)
    {
        return top + static_cast<float>(place) * row_height;
    };

    // A new action, an undo or a redo scrolls the line into view.
    ImGuiStorage* storage = ImGui::GetStateStorage();
    int* shown_done = storage->GetIntRef(ImGui::GetID("##done"), -1);
    int* shown_rows = storage->GetIntRef(ImGui::GetID("##count"), -1);
    if (*shown_done != static_cast<int>(done) || *shown_rows != static_cast<int>(rows.size()))
    {
        *shown_done = static_cast<int>(done);
        *shown_rows = static_cast<int>(rows.size());
        const float y = line_y(done);
        if (y < view_top + row_height || y > view_bottom - row_height)
        {
            ImGui::SetScrollFromPosY(y - view_top, 0.5f);
        }
    }

    // The rows in view. The actions that undo stepped back over have dimmer names. The line's handle comes after the
    // rows and lies over them. Each row allows the overlap. The handle then gets the clicks over it.
    ImFont* text = theme::GetFonts().text;
    ImFont* figures = theme::GetFonts().figures;
    std::optional<std::size_t> chosen;
    for (std::size_t i = 0; i < rows.size(); i++)
    {
        const float y = line_y(i);
        if (y + row_height < view_top || y > view_bottom)
        {
            continue;
        }

        ImGui::SetCursorScreenPos(ImVec2(origin.x, y));
        ImGui::PushID(static_cast<int>(i));
        ImGui::SetNextItemAllowOverlap();
        if (ImGui::InvisibleButton("##row", ImVec2(width, row_height)))
        {
            chosen = i + 1;
        }
        if (ImGui::IsItemHovered())
        {
            draw->AddRectFilled(ImVec2(origin.x + margin, y + Px(2.0f)),
                                ImVec2(origin.x + width - margin, y + row_height - Px(2.0f)), theme::kRaisedHover,
                                Px(6.0f));
        }
        ImGui::PopID();

        const HistoryRow& row = rows[i];
        const std::string number = std::to_string(i + 1);
        const float first = y + Px(5.0f);
        const float second = y + Px(23.0f);
        const float name_x = origin.x + Px(42.0f);
        const float right = origin.x + width - margin - Px(8.0f);
        const ImVec2 number_size = TextSize(figures, theme::kSmallSize, number);
        const ImVec2 time_size = TextSize(figures, theme::kSmallSize, row.time);
        DrawText(draw, figures, theme::kSmallSize, ImVec2(name_x - Px(10.0f) - number_size.x, first + Px(1.5f)),
                 theme::kTextMuted, number);
        DrawText(draw, text, theme::kTextSize, ImVec2(name_x, first), i < done ? theme::kText : theme::kTextMuted,
                 FitText(text, theme::kTextSize, row.name, right - time_size.x - Px(10.0f) - name_x));
        DrawText(draw, figures, theme::kSmallSize, ImVec2(right - time_size.x, first + Px(1.5f)), theme::kTextMuted,
                 row.time);
        DrawText(draw, text, theme::kSmallSize, ImVec2(name_x, second), theme::kTextMuted,
                 FitText(text, theme::kSmallSize, row.details, right - name_x));
    }

    // The line, with a handle on its left end. A drag of the handle moves the line to the nearest place between rows,
    // or above the first row to undo every action.
    const float y = line_y(done);
    const float left = origin.x + margin;
    ImGui::SetCursorScreenPos(ImVec2(left - Px(4.0f), y - Px(9.0f)));
    ImGui::InvisibleButton("##line", ImVec2(Px(20.0f), Px(18.0f)));
    if (ImGui::IsItemActive())
    {
        const float place = std::round((ImGui::GetIO().MousePos.y - top) / row_height);
        const auto dragged = static_cast<std::size_t>(std::clamp(place, 0.0f, static_cast<float>(rows.size())));
        if (dragged != done)
        {
            chosen = dragged;
        }
    }
    if (ImGui::IsItemHovered() || ImGui::IsItemActive())
    {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
    }
    draw->AddRectFilled(ImVec2(left, y - Px(1.0f)), ImVec2(origin.x + width - margin, y + Px(1.0f)), theme::kAccent);
    draw->AddTriangleFilled(ImVec2(left, y - Px(6.0f)), ImVec2(left + Px(8.0f), y), ImVec2(left, y + Px(6.0f)),
                            theme::kAccent);

    // The rows set the height that scrolls.
    ImGui::SetCursorScreenPos(ImVec2(origin.x, line_y(rows.size())));
    ImGui::Dummy(ImVec2(width, Px(kHistoryTop)));
    ImGui::EndChild();

    return chosen;
}

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
    if (ImGui::IsItemDeactivatedAfterEdit())
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
                "In the game, every sequence that plays shares a global variable. Here, the value that you "
                "choose stays with this sound.");
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
            WrappedTooltip("The value at the position");
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
    while (size > 0 && size < name.size() && (static_cast<unsigned char>(name[size]) & 0xC0) == 0x80)
    {
        size--;
    }

    state.name.fill('\0');
    name.copy(state.name.data(), size);
    state.renaming = index;
    state.focus = true;
}

// The row of scene `index`, from `min` to `max`. A click recalls the scene, and a double-click edits its name in place.
// The row's context menu updates, renames or deletes the scene.
void DrawScene(ImDrawList* draw, SongView& song, VariablesState& state, std::size_t index, ImVec2 min, ImVec2 max)
{
    const SceneView& scene = song.scenes[index];
    ImFont* text = theme::GetFonts().text;
    const ImVec2 box_min(min.x, min.y + Px(1.0f));
    const ImVec2 box_max(max.x, max.y - Px(1.0f));
    const float name_x = box_min.x + Px(10.0f);
    ImGui::PushID(static_cast<int>(index));

    // While the user edits the name, a field takes the row's place. Enter or a click elsewhere keeps the new name.
    // Escape gives the field the old name back.
    if (state.renaming == index)
    {
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
    ImGui::PopID();

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

    // The menu of the variables.
    ImGui::SetCursorScreenPos(ImVec2(left, line_top));
    ImGui::SetNextItemWidth(right - left - time_width - gap);
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

    // The field of the time shows the change's time until the user types in it.
    const bool typing = state.timing == index;
    std::array<char, 32> shown{};
    TimeText(change.time).copy(shown.data(), shown.size() - 1);
    ImGui::SetCursorScreenPos(ImVec2(right - time_width, line_top));
    ImGui::SetNextItemWidth(time_width);
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
                "A change's time takes seconds, minutes and seconds, or hours, minutes and seconds, such as 90, 1:30 "
                "or 0:01:30.000. After a colon, the minutes and the seconds stay below 60."};
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
        WrappedTooltip("The time from the start, when the game sets the variable");
    }
    ImGui::OpenPopupOnItemClick("##change", ImGuiPopupFlags_MouseButtonRight);
    ImGui::PopStyleVar();

    // The value's switches, menu or slider: those of the variable among the variables that the song reads. A drag of
    // the slider shows each value that it passes, and the value where it ends becomes the change's value.
    const VariableView* read = ReadVariable(song, change.variable);
    VariableView value{.number = change.variable, .start = change.value};
    if (read != nullptr)
    {
        value.choices = read->choices;
    }
    const ImVec2 control(left, line_top + height + Px(8.0f));
    if (value.choices.empty())
    {
        DrawValueSlider(value, control, right - left);
    }
    else if (!DrawSwitches(value, control, right - left))
    {
        DrawChoiceMenu(value, control, right - left);
    }
    change.value = value.start;
    if (value.chosen)
    {
        ChangeView edited = change;
        edited.value = *value.chosen;
        song.change_request = ChangeRequest{ChangeAction::kEdit, index, edited, {}};
    }

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

int BeatStep(float beat_width)
{
    int power = 1;
    int step = 1;
    while (beat_width * static_cast<float>(step) < Px(44.0f))
    {
        if (step == power)
        {
            step = power * 2;
        }
        else if (step == power * 2)
        {
            step = power * 5;
        }
        else
        {
            power *= 10;
            step = power;
        }
    }

    return step;
}

bool LabelledBeat(int quarter, int step)
{
    return quarter == 0 || (quarter + 1) % step == 0;
}

void BuildLayout(ImGuiID dockspace)
{
    // Start from an empty node the size of the main window's work area. The splits take their sizes from it.
    const ImVec2 size = ImGui::GetMainViewport()->WorkSize;
    ImGui::DockBuilderRemoveNode(dockspace);
    ImGui::DockBuilderAddNode(dockspace, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspace, size);

    // The zones at the sides take their widths in points, and the arrangement takes the rest. The arrangement also
    // takes the room that the window gains or loses. The mixer takes part of the middle's height.
    ImGuiID middle = dockspace;
    const float left_width = std::min(Px(kInspectorWidth), size.x * 0.3f);
    const float right_width = std::min(Px(kRightZoneWidth), size.x * 0.3f);
    const ImGuiID left = ImGui::DockBuilderSplitNode(middle, ImGuiDir_Left, left_width / size.x, nullptr, &middle);
    ImGuiID right =
        ImGui::DockBuilderSplitNode(middle, ImGuiDir_Right, right_width / (size.x - left_width), nullptr, &middle);
    const ImGuiID right_bottom = ImGui::DockBuilderSplitNode(right, ImGuiDir_Down, 0.3f, nullptr, &right);
    const ImGuiID right_middle = ImGui::DockBuilderSplitNode(right, ImGuiDir_Down, 0.36f, nullptr, &right);
    const ImGuiID bottom = ImGui::DockBuilderSplitNode(middle, ImGuiDir_Down, 0.4f, nullptr, &middle);

    ImGui::DockBuilderDockWindow(kInspector, left);
    ImGui::DockBuilderDockWindow(kBrowser, right);
    ImGui::DockBuilderDockWindow(kVariables, right_middle);
    ImGui::DockBuilderDockWindow(kUndoHistory, right_bottom);
    ImGui::DockBuilderDockWindow(kArrangement, middle);
    ImGui::DockBuilderDockWindow(kMixer, bottom);
    ImGui::DockBuilderDockWindow(kKeyEditor, bottom);
    ImGui::DockBuilderDockWindow(kListEditor, bottom);
    ImGui::DockBuilderFinish(dockspace);
}

void DockWithMixer(const char* window)
{
    // The mixer's node: the window's, or else its place in the settings.
    const ImGuiID id = ImHashStr(window);
    if (ImGui::FindWindowByID(id) != nullptr || ImGui::FindWindowSettingsByID(id) != nullptr)
    {
        return;
    }
    ImGuiID node = 0;
    if (const ImGuiWindow* mixer = ImGui::FindWindowByName(kMixer))
    {
        node = mixer->DockId;
    }
    else if (const ImGuiWindowSettings* settings = ImGui::FindWindowSettingsByID(ImHashStr(kMixer)))
    {
        node = settings->DockId;
    }
    if (node != 0)
    {
        ImGui::SetNextWindowDockID(node, ImGuiCond_FirstUseEver);
    }
}

void DrawInspector(SongView* song, MasterBus& master, ArrangementState& state, SoundSection& sound, bool busy,
                   bool* open)
{
    if (!*open)
    {
        return;
    }

    if (BeginZone(kInspector, open))
    {
        // The header names the selected track as the mixer and the Key Editor name it.
        TrackView* track = SelectedTrack(song, state);
        ZoneHeader("Inspector", track != nullptr ? track->name : std::string());

        // The sections scroll when they're taller than the zone.
        ImGui::BeginChild("##sections", ImVec2(0.0f, 0.0f));
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        const float margin = Px(kInspectorMargin);
        const float left = origin.x + margin;
        const float width = avail.x - margin * 2.0f;
        const float gap = Px(18.0f);

        // The track's name, its instrument, and the selected part on it.
        float y = origin.y + margin;
        if (track != nullptr)
        {
            y = DrawTrackCard(draw, ImVec2(left, y), width, *track) + gap;
            const SectionHeading track_heading = DrawSectionHeading(draw, ImVec2(left, y), width, "Track", "", true);
            y = track_heading.bottom;
            if (track_heading.open)
            {
                y = DrawInstrumentRows(draw, ImVec2(left, y), width, *song, *track, state, busy);
                y = DrawColourProperty(draw, ImVec2(left, y), width, *track, state);
            }
            y += gap;

            const SectionHeading part_heading = DrawSectionHeading(draw, ImVec2(left, y), width, "Part", "", true);
            y = part_heading.bottom;
            const PartView* part = SelectedPart(*track, state);
            std::array<Property, 3> details = {Property{"Start", ""}, Property{"Length", ""}, Property{"Notes", ""}};
            if (part != nullptr)
            {
                const double quarters = song->ticks_per_quarter;
                details[0].value = PositionText(part->start / quarters, song->beats_per_bar);
                details[1].value = LengthText(part->length / quarters, song->beats_per_bar);
                details[2].value = std::to_string(part->notes.size());
            }
            if (part_heading.open)
            {
                y = DrawPropertyRows(draw, ImVec2(left, y), width, details);
            }
            y += gap;
        }
        else
        {
            ImFont* font = theme::GetFonts().text;
            const ImVec2 size = TextSize(font, theme::kTextSize, "No track is selected");
            DrawText(draw, font, theme::kTextSize, ImVec2(left + (width - size.x) * 0.5f, y + Px(8.0f)),
                     theme::kTextMuted, "No track is selected");
            y += Px(8.0f) + size.y + gap;
        }

        // How the program edits the shown sequence.
        if (song != nullptr)
        {
            const SectionHeading sequence_heading = DrawSectionHeading(draw, ImVec2(left, y), width, "Sequence",
                                                                       song->timeline ? "Timeline" : "Program", true);
            y = sequence_heading.bottom;
            if (sequence_heading.open)
            {
                y = DrawModeSwitch(ImVec2(left, y), width, *song, busy);
            }
            y += gap;
        }

        // The values of the shown sound in the archive's tables. The section starts closed, and its heading names the
        // sound.
        if (sound.view)
        {
            const SectionHeading sound_heading =
                DrawSectionHeading(draw, ImVec2(left, y), width, "Sound", sound.view->values.name, false);
            y = sound_heading.bottom;
            if (sound_heading.open)
            {
                y = DrawSoundValues(draw, ImVec2(left, y), width, sound);
            }
            y += gap;
        }

        // The channel strips of the track and the output fill the rest of the zone, and keep a height that shows their
        // controls.
        const float between = Px(6.0f);
        const float rest = std::floor(origin.y + avail.y - margin - y);
        const ImVec2 strip(std::floor((width - between) * 0.5f), std::max(rest, Px(kLeastStripHeight)));
        if (track != nullptr)
        {
            ImGui::SetCursorScreenPos(ImVec2(left, y));
            ImGui::PushID("track");
            Strip track_strip = TrackStrip(*track, false, busy);
            track_strip.buses = song->sends;
            DrawStrip(track_strip, strip);
            ImGui::PopID();
        }
        ImGui::SetCursorScreenPos(ImVec2(left + width - strip.x, y));
        ImGui::PushID("output");
        DrawStrip(OutputStrip(master), strip);
        ImGui::PopID();

        // The strips and the margin below them set the height that scrolls.
        ImGui::SetCursorScreenPos(ImVec2(origin.x, y + strip.y));
        ImGui::Dummy(ImVec2(width, margin));
        ImGui::EndChild();
    }
    ImGui::End();
}

namespace
{

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
        // A file from before the archive's tables came into the format holds no values for a sound.
        ImFont* font = theme::GetFonts().text;
        const char* text = sound.sound ? "The file holds no values" : "No sound is selected";
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

namespace
{

// A whole number of semitones or of velocity typed in the info line, with a sign or without one, from -127 to 127.
std::optional<int32_t> ParseAmount(std::string_view text)
{
    while (!text.empty() && text.front() == ' ')
    {
        text.remove_prefix(1);
    }
    while (!text.empty() && text.back() == ' ')
    {
        text.remove_suffix(1);
    }
    if (!text.empty() && text.front() == '+')
    {
        text.remove_prefix(1);
    }

    int32_t amount = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), amount);
    if (text.empty() || error != std::errc() || end != text.data() + text.size() || amount < -127 || amount > 127)
    {
        return std::nullopt;
    }

    return amount;
}

// The arrangement's info line shows the selected part: its name, start, end, length and notes. Without a selection, the
// line says so. A start typed there moves the part's notes. A transposition or a velocity typed there changes them by
// that much, and the field shows 0 again. Hokuto has nowhere to keep a part's transposition and velocity apart from its
// notes. Text that the line can't read is refused with a message.
void DrawPartInfoLine(const SongView* song, ArrangementState& state, bool busy)
{
    InfoLine line = BeginInfoLine();
    const bool selected =
        song != nullptr && state.selected_part_track >= 0 &&
        state.selected_part_track < static_cast<int>(song->tracks.size()) && state.selected_part >= 0 &&
        state.selected_part <
            static_cast<int>(song->tracks[static_cast<std::size_t>(state.selected_part_track)].parts.size());
    if (!selected)
    {
        NoObjectSelected(line);
        EndInfoLine(line);
        return;
    }

    const TrackView& track = song->tracks[static_cast<std::size_t>(state.selected_part_track)];
    const PartView& part = track.parts[static_cast<std::size_t>(state.selected_part)];
    const double quarter = std::max(song->ticks_per_quarter, 1);

    const auto typed = [&](const char* label, const std::string& value) -> std::optional<std::string>
    {
        if (busy)
        {
            InfoField(line, label, value);
            return std::nullopt;
        }

        std::optional<std::string> text = InfoEntry(line, label, value, state.part_typing);
        return text && *text != value ? text : std::nullopt;
    };

    const auto ask = [&](PartValue value, int64_t amount)
    {
        if (amount != 0 && amount >= std::numeric_limits<int32_t>::min() &&
            amount <= std::numeric_limits<int32_t>::max())
        {
            state.part_edit =
                PartEdit{state.selected_part_track, state.selected_part, value, static_cast<int32_t>(amount), {}};
        }
    };

    const auto refuse = [&](std::string refused)
    {
        state.part_edit = PartEdit{.refused = std::move(refused)};
    };

    InfoField(line, "NAME", part.name.empty() ? track.name : part.name);
    if (const std::optional<std::string> start =
            typed("START", PositionText(part.start / quarter, song->beats_per_bar)))
    {
        if (const std::optional<double> quarters = ParsePosition(*start, song->beats_per_bar))
        {
            ask(PartValue::kStart, std::llround(*quarters * quarter) - int64_t{part.start});
        }
        else
        {
            refuse("Hokuto can't read \"" + *start + "\" as a start. " + PositionHint("A start"));
        }
    }
    InfoField(line, "END", PositionText((part.start + part.length) / quarter, song->beats_per_bar));
    InfoField(line, "LENGTH", LengthText(part.length / quarter, song->beats_per_bar));
    InfoField(line, "NOTES", std::to_string(part.notes.size()));
    if (const std::optional<std::string> semitones = typed("TRANSPOSE", "0"))
    {
        if (const std::optional<int32_t> amount = ParseAmount(*semitones))
        {
            ask(PartValue::kTranspose, *amount);
        }
        else
        {
            refuse("Hokuto can't read \"" + *semitones +
                   "\" as a transposition. A transposition is a number of semitones from -127 to 127.");
        }
    }
    if (const std::optional<std::string> velocity = typed("VELOCITY", "0"))
    {
        if (const std::optional<int32_t> amount = ParseAmount(*velocity))
        {
            ask(PartValue::kVelocity, *amount);
        }
        else
        {
            refuse("Hokuto can't read \"" + *velocity +
                   "\" as a change of velocity. A change of velocity is a number from -127 to 127.");
        }
    }
    if (part.linked)
    {
        InfoField(line, "LINKED", "Yes");
    }
    EndInfoLine(line);
}

} // namespace

void DrawArrangement(SongView* song, Transport& transport, ArrangementState& state, bool busy)
{
    if (BeginZone(kArrangement, nullptr))
    {
        std::string detail;
        if (song != nullptr)
        {
            detail = song->name;
        }
        ZoneHeader("Arrangement", detail);
        DrawPartInfoLine(song, state, busy);
        DrawCanvas(song, transport, state);
    }
    ImGui::End();
}

namespace
{

// Draws the switch between the shown sound's two mixing modes in the mixer's header at `header`, after the title. A
// click on the other mode sets the song's `mixing_request`. Each half's tooltip explains its mode. A sound that can't
// take enhanced mixing has that half disabled, and its tooltip says why. The cursor stays where it was.
void DrawMixingSwitch(ImVec2 header, SongView& song, bool busy)
{
    const std::array<std::string, 2> kLabels = {"Strict", "Enhanced"};
    const std::array<uint8_t, 2> lit = {static_cast<uint8_t>(song.enhanced ? 0 : 1),
                                        static_cast<uint8_t>(song.enhanced ? 1 : 0)};
    const std::array<uint8_t, 2> disabled = {0, static_cast<uint8_t>(song.enhanced_refused.empty() ? 0 : 1)};
    const float title = TextSize(theme::GetFonts().strong, theme::kTextSize, "Mixer").x;
    const ImVec2 size(Px(kMixingSwitchWidth), Px(kMixingSwitchHeight));
    const ImVec2 pos(std::floor(header.x + Px(14.0f) + title + Px(kMixingSwitchGap)),
                     std::floor(header.y + (Px(kHeaderHeight) - size.y) * 0.5f));
    const ImVec2 cursor = ImGui::GetCursorScreenPos();
    ImGui::SetCursorScreenPos(pos);
    const int clicked = SegmentedSwitch("##mixing", kLabels, lit, size, disabled);
    ImGui::SetCursorScreenPos(cursor);

    // The half under the mouse explains its mode, or why the sound can't take it.
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const bool over = ImGui::IsWindowHovered() && mouse.y >= pos.y && mouse.y < pos.y + size.y && mouse.x >= pos.x &&
                      mouse.x < pos.x + size.x;
    if (over && !ImGui::IsAnyMouseDown())
    {
        WrappedTooltip(mouse.x < pos.x + size.x * 0.5f
                           ? "Mix the sound as the console does: its voice limit, its resampling and its rounding. A "
                             "game plays the sound this way."
                       : song.enhanced_refused.empty()
                           ? "Mix the sound without the console's limits: up to 1,024 voices, mixed at 48 kHz in "
                             "floating point and resampled with a long windowed-sinc filter. The notes, the "
                             "instruments and the effects play as on the console."
                           : song.enhanced_refused);
    }
    if (!busy && clicked == 0 && song.enhanced)
    {
        song.mixing_request = false;
    }
    else if (!busy && clicked == 1 && !song.enhanced && song.enhanced_refused.empty())
    {
        song.mixing_request = true;
    }
}

// The names of the engines of the mixer's switch, and the libraries whose rules they follow, in the order of
// format::Engine.
constexpr std::array<const char*, 4> kEngineLabels = {"Switch", "Wii U", "3DS", "Wii"};
constexpr std::array<const char*, 4> kEngineLibraries = {"nn::atk on the audio renderer", "nw::snd on AX",
                                                         "nn::snd on the DSP", "NW4R SND on AX"};

// What the tooltip of engine `engine` of the mixer's switch says.
std::string EngineTip(const SongView& song, int engine)
{
    const auto index = static_cast<std::size_t>(engine);
    const std::string console = kEngineLabels[index];
    const std::string file = kEngineLabels[static_cast<std::size_t>(song.file_engine)];
    if (!song.engine_refused[index].empty())
    {
        return song.engine_refused[index];
    }
    if (engine == song.file_engine)
    {
        return "Play the sound on the " + console + "'s engine, the file's: " + kEngineLibraries[index] +
               ". The sound plays as the game plays it.";
    }

    std::string tip = "Play the sound on the " + console + "'s engine: " + kEngineLibraries[index] +
                      ". Hokuto plays the sound's conversion for the " + console +
                      ", as File > Export > Another Console… writes it. The file keeps the sound for the " + file + ".";
    if (engine == song.failed_engine && !song.engine_error.empty())
    {
        tip += "\n\nHokuto can't convert the sound. " + format::AsSentence(song.engine_error) + " It plays on the " +
               file + "'s engine.";
    }
    else if (engine == song.engine && !song.engine_notes.empty())
    {
        tip += "\n";
        for (const std::string& note : song.engine_notes)
        {
            tip += "\n" + note;
        }
    }

    return tip;
}

// Draws the switch between the engines that can play the shown sound in the mixer's header at `header`, after the
// switch of the mixing modes. A click on another engine sets the song's `engine_request`. Each engine's tooltip says
// how it plays the sound, or why it can't. The cursor stays where it was.
void DrawEngineSwitch(ImVec2 header, SongView& song, bool busy)
{
    std::array<std::string, 4> labels;
    std::array<uint8_t, 4> lit{};
    std::array<uint8_t, 4> disabled{};
    for (std::size_t i = 0; i < labels.size(); i++)
    {
        labels[i] = kEngineLabels[i];
        lit[i] = static_cast<uint8_t>(static_cast<int>(i) == song.engine ? 1 : 0);
        disabled[i] = static_cast<uint8_t>(song.engine_refused[i].empty() ? 0 : 1);
    }
    const float title = TextSize(theme::GetFonts().strong, theme::kTextSize, "Mixer").x;
    const ImVec2 size(Px(kEngineSwitchWidth), Px(kMixingSwitchHeight));
    const ImVec2 pos(
        std::floor(header.x + Px(14.0f) + title + Px(kMixingSwitchGap) + Px(kMixingSwitchWidth) + Px(kMixingSwitchGap)),
        std::floor(header.y + (Px(kHeaderHeight) - size.y) * 0.5f));
    const ImVec2 cursor = ImGui::GetCursorScreenPos();
    ImGui::SetCursorScreenPos(pos);
    const int clicked = SegmentedSwitch("##engine", labels, lit, size, disabled);
    ImGui::SetCursorScreenPos(cursor);

    // The engine under the mouse says how it plays the sound, or why it can't.
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const bool over = ImGui::IsWindowHovered() && mouse.y >= pos.y && mouse.y < pos.y + size.y && mouse.x >= pos.x &&
                      mouse.x < pos.x + size.x;
    if (over && !ImGui::IsAnyMouseDown())
    {
        const auto engine = static_cast<int>((mouse.x - pos.x) / size.x * static_cast<float>(labels.size()));
        WrappedTooltip(EngineTip(song, std::clamp(engine, 0, static_cast<int>(labels.size()) - 1)));
    }
    if (!busy && clicked >= 0 && clicked != song.engine &&
        song.engine_refused[static_cast<std::size_t>(clicked)].empty())
    {
        song.engine_request = clicked;
    }
}

} // namespace

void DrawMixer(SongView* song, MasterBus& master, ArrangementState& state, FxChannels* fx, bool busy, bool* open)
{
    if (!*open)
    {
        return;
    }

    if (BeginZone(kMixer, open))
    {
        const std::size_t channels = song != nullptr ? song->tracks.size() : 0;
        const ImVec2 header = ImGui::GetCursorScreenPos();
        ZoneHeader("Mixer", channels == 0   ? std::string()
                            : channels == 1 ? std::string("1 channel")
                                            : std::to_string(channels) + " channels");
        if (song != nullptr)
        {
            DrawMixingSwitch(header, *song, busy);
            if (song->engines)
            {
                DrawEngineSwitch(header, *song, busy);
            }
        }
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        const float output_width = Px(kMasterWidth) + Px(20.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Px(10.0f), Px(10.0f)));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(Px(6.0f), 0.0f));

        // The tracks' channels scroll sideways.
        ImGui::BeginChild("##channels", ImVec2(avail.x - output_width, avail.y), ImGuiChildFlags_AlwaysUseWindowPadding,
                          ImGuiWindowFlags_HorizontalScrollbar);
        const float height = ImGui::GetContentRegionAvail().y;
        for (std::size_t i = 0; i < channels; i++)
        {
            ImGui::PushID(static_cast<int>(i));
            Strip strip = TrackStrip(song->tracks[i], state.selected_track == static_cast<int>(i), busy);
            strip.buses = song->sends;
            if (DrawStrip(strip, ImVec2(Px(kStripWidth), height)))
            {
                state.selected_track = static_cast<int>(i);
            }
            ImGui::SameLine();
            ImGui::PopID();
        }

        // Without tracks, a note fills the width that the FX channels leave.
        if (channels == 0)
        {
            const ImVec2 pos = ImGui::GetCursorScreenPos();
            const std::size_t fx_count = fx != nullptr ? std::min(fx->count, kFxChannels) : 0;
            const float fx_width = Px(14.0f) + static_cast<float>(fx_count) * (Px(kFxStripWidth) + Px(6.0f));
            const float avail_width = ImGui::GetContentRegionAvail().x;
            const float width = fx != nullptr ? std::max(avail_width - fx_width, Px(320.0f)) : avail_width;
            EmptyState(ImGui::GetWindowDrawList(), pos, ImVec2(pos.x + width, pos.y + height), "No channels",
                       "Each track of the sequence gets a channel here.");
            ImGui::Dummy(ImVec2(width, height));
            ImGui::SameLine();
        }

        // The FX channels follow the tracks' channels, past a line.
        if (fx != nullptr)
        {
            const ImVec2 at = ImGui::GetCursorScreenPos();
            ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(at.x + Px(3.0f), at.y + Px(10.0f)),
                                                      ImVec2(at.x + Px(4.0f), at.y + height - Px(10.0f)),
                                                      theme::kBorder);
            ImGui::SetCursorScreenPos(ImVec2(at.x + Px(14.0f), at.y));
            ImGui::PushID("##fx");
            for (std::size_t channel = 0; channel < std::min(fx->count, kFxChannels); channel++)
            {
                ImGui::PushID(static_cast<int>(channel));
                DrawFxStrip(channel, *fx, ImVec2(Px(kFxStripWidth), height));
                ImGui::SameLine();
                ImGui::PopID();
            }
            ImGui::PopID();
        }
        ImGui::EndChild();

        // The output's channel stays at the right.
        ImGui::SameLine(0.0f, 0.0f);
        ImGui::BeginChild("##output", ImVec2(output_width, avail.y), ImGuiChildFlags_AlwaysUseWindowPadding,
                          ImGuiWindowFlags_NoScrollbar);
        const ImVec2 pos = ImGui::GetWindowPos();
        ImGui::GetWindowDrawList()->AddRectFilled(
            ImVec2(pos.x, pos.y + Px(10.0f)), ImVec2(pos.x + Px(1.0f), pos.y + avail.y - Px(10.0f)), theme::kBorder);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + Px(4.0f));
        DrawStrip(OutputStrip(master), ImVec2(Px(kMasterWidth), ImGui::GetContentRegionAvail().y));
        ImGui::EndChild();
        ImGui::PopStyleVar(2);
    }
    ImGui::End();
}

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
                       "Variables that a game sets show here.");
        }
        else
        {
            DrawVariableRows(*song, state);
        }
    }
    ImGui::End();
}

std::optional<std::size_t> DrawUndoHistory(std::span<const HistoryRow> rows, std::size_t done, bool* open)
{
    if (!*open)
    {
        return std::nullopt;
    }

    // The header counts the actions that are done.
    std::optional<std::size_t> chosen;
    if (BeginZone(kUndoHistory, open))
    {
        ZoneHeader("Undo History",
                   rows.empty() ? std::string() : std::to_string(done) + " of " + std::to_string(rows.size()));
        if (rows.empty())
        {
            const ImVec2 pos = ImGui::GetCursorScreenPos();
            const ImVec2 avail = ImGui::GetContentRegionAvail();
            EmptyState(ImGui::GetWindowDrawList(), pos, ImVec2(pos.x + avail.x, pos.y + avail.y), "No edits yet",
                       "Each edit appears here.");
        }
        else
        {
            chosen = DrawHistoryRows(rows, done);
        }
    }
    ImGui::End();

    return chosen;
}

} // namespace hokuto::app
