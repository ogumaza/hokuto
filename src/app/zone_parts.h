// SPDX-License-Identifier: MIT

// The parts that several zones draw: the zone's window, the palette of track colours, the headings and rows of the
// inspector's sections, and the sizes that the zones share. The names are internal to the files of the zones.

#pragma once

#include <imgui.h>

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "app/arrangement.h"
#include "app/song_view.h"

namespace hokuto::app::zone_parts
{

// The palette's swatches, and how many sit in a row. The popup has a name for each window that shows it.
inline constexpr float kSwatchSize = 20.0f;
inline constexpr std::size_t kSwatchesPerRow = 9;
inline constexpr char kColourPopup[] = "##track colour";

// The name of the Alt key on this system, and the menu that holds Preferences. The Mac calls the key Option, and puts
// Preferences in the application menu.
#ifdef __APPLE__
inline constexpr char kAltKey[] = "Option";
inline constexpr char kPreferencesItem[] = "Hokuto > Preferences…";
#else
inline constexpr char kAltKey[] = "Alt";
inline constexpr char kPreferencesItem[] = "Edit > Preferences…";
#endif

// The widths of the zones at the sides, and the inspector's sizes, before the display's scale.
inline constexpr float kInspectorWidth = 232.0f;
inline constexpr float kRightZoneWidth = 288.0f;
inline constexpr float kInspectorMargin = 12.0f;
inline constexpr float kPropertyHeight = 26.0f;

// The Variables zone's sizes, before the display's scale: a variable's row, the height of its switches, the header of
// the scenes or the changes, a scene's row and a change's row. The field of a change's time is as wide as a time.
inline constexpr float kVariableRowHeight = 70.0f;
inline constexpr float kSwitchHeight = 26.0f;
inline constexpr float kScenesHeaderHeight = 36.0f;
inline constexpr float kSceneRowHeight = 28.0f;
inline constexpr float kChangeRowHeight = 76.0f;
inline constexpr float kTimeFieldWidth = 104.0f;

// Begins a zone: a docked window with no tab and no padding. End() must follow in every case.
bool BeginZone(const char* name, bool* open);

// The palette of the tracks' colours, in a popup that the caller has begun: a swatch for each colour of
// theme::kTrackPalette, then Automatic. The swatch of `current` is marked. Returns the colour that the user picked for
// `track`, or nothing. Automatic picks the track's automatic colour.
std::optional<TrackColourPick> DrawPalette(int track, ImU32 current);

// The selected track, or nullptr when there's none.
TrackView* SelectedTrack(SongView* song, const ArrangementState& state);

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
bool SectionOpen(const char* title, bool open_at_first);

// The height of the heading of a section titled `title`: from its top to the bottom that DrawSectionHeading returns.
float SectionHeadingHeight(const char* title);

// Draws the heading of a section of the inspector or the browser: a chevron and a title in capitals over a line, with
// `detail` at the right. A click on the heading opens or closes the section (SectionOpen).
SectionHeading DrawSectionHeading(ImDrawList* draw, ImVec2 pos, float width, const char* title, std::string_view detail,
                                  bool open_at_first);

// Draws a row for each property of a section of the inspector. Each row has the label on the left and the value on the
// right. Returns the bottom of the last row.
float DrawPropertyRows(ImDrawList* draw, ImVec2 pos, float width, std::span<const Property> properties);

} // namespace hokuto::app::zone_parts
