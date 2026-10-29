// SPDX-License-Identifier: MIT

// Controls that Dear ImGui lacks: icons, icon buttons, the M and S buttons of a track, faders, pan knobs, meters,
// switches, menus that filter their items, and the header of each zone. Each one draws in the program's style.

#pragma once

#include <imgui.h>

#include <array>
#include <cfloat>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace hokuto::app
{

enum class Icon
{
    kGoToStart,
    kRewind,
    kForward,
    kStop,
    kPlay,
    kCycle,
    kInspector,
    kBrowser,
    kVariables,
    kMixer,
    kHistory,
    kSetup,
    kSearch,
    kChevronRight,
    kChevronDown,
    kSound,
    kSequence,
    kBank,
    kWave,
    kPointer,
    kPencil,
    kEraser,
    kSnap,
    kAdd,
};

// The level of silence, in dB.
inline constexpr float kSilence = -std::numeric_limits<float>::infinity();

// Draws `icon` in a square of `size` around `centre`.
void DrawIcon(ImDrawList* draw, Icon icon, ImVec2 centre, float size, ImU32 colour);

// Draws text in `font` at `size` points.
void DrawText(ImDrawList* draw, ImFont* font, float size, ImVec2 pos, ImU32 colour, std::string_view text);
ImVec2 TextSize(ImFont* font, float size, std::string_view text);

// The distance from the top of a line of `font` at `size` points to its baseline. Text of two sizes lines up when their
// baselines meet.
float Ascent(ImFont* font, float size);

// `text`, cut short with an ellipsis to fit in `width` at `size` in `font`. The cut falls between UTF-8 characters.
std::string FitText(ImFont* font, float size, std::string_view text, float width);

// Draws a label in small capitals, such as a zone's title.
void DrawCaps(ImDrawList* draw, ImVec2 pos, ImU32 colour, std::string_view text);
ImVec2 CapsSize(std::string_view text);

// `text` in capitals, cut short with an ellipsis to fit in `width` as DrawCaps draws it. A last word of one character,
// such as the 2 of "Volume 2", stays after the ellipsis: "VOLU… 2".
std::string FitCaps(std::string_view text, float width);

// A button with an icon and no frame until the mouse is over it. While `on`, it's lit in `on_colour`. Returns true when
// clicked.
bool IconButton(const char* id, Icon icon, ImVec2 size, bool on, ImU32 on_colour, const char* tooltip);

// Draws a small button with a letter, such as a track's M or S. While `on`, the button is lit in `on_colour`.
void DrawLetterButton(ImDrawList* draw, ImVec2 min, ImVec2 max, const char* letter, bool on, ImU32 on_colour,
                      bool hovered);

// A vertical fader for a level in dB. Command-click on the Mac, or Ctrl-click elsewhere, sets 0 dB. Shift moves the
// fader finely. Returns true when the level changed.
bool Fader(const char* id, float* level, ImVec2 size);

// A pan knob, from -1 for left to 1 for right. Command-click or Ctrl-click centres it.
bool PanKnob(const char* id, float* pan, float radius);

// A horizontal slider for the level in dB of a send to `bus`, in a slot that names the bus on the left and shows the
// level on the right. The level sits on the fader's curve, and dragging right raises it. Command-click on the Mac, or
// Ctrl-click elsewhere, sets 0 dB. Shift moves the slider finely. Returns true when the level changed.
bool SendSlider(const char* id, const char* bus, float* level, ImVec2 size);

// A pan from -1 to 1 as the mixer writes it: C for the centre, or L or R and the distance from the centre in
// hundredths.
std::string PanText(float pan);

// A level in dB as the mixer writes it: to a tenth of a dB, with a sign above 0. A level below -60 dB is -∞.
std::string LevelText(float level);

// The range of a value slider: its lowest and highest values, and whether equal moves of the slider multiply the value
// by equal amounts. A time or a frequency suits such a slider.
struct SliderRange
{
    float low = 0.0f;
    float high = 1.0f;
    bool logarithmic = false;
};

// A horizontal slider, filled from the left up to the value, with the value in the middle as `format` writes it. The
// slider moves with the mouse from where it is and doesn't jump to the click. Shift moves it finely. Command-click on
// the Mac, or Ctrl-click elsewhere, sets `reset`. A double-click types a value from `typed`, or from `range` without
// it. A typed value can lie past the slider's ends. Returns true when the value changed. During a drag, the slider
// follows the mouse from where the drag started, whatever the caller does to the value. Dear ImGui's
// IsItemDeactivatedAfterEdit returns true when an edit ends. `id` starts with ##. Dear ImGui's text input shows any
// other `id` as a label. Inside a disabled block, the style's alpha fades the slider. Dear ImGui's controls fade by the
// same alpha.
bool ValueSlider(const char* id, float* value, const SliderRange& range, float reset, const char* format, ImVec2 size,
                 const SliderRange* typed = nullptr);

// Draws a meter for a level in dB. The meter is green to -18 dB, yellow to -6 dB and red above. A vertical meter fills
// from the bottom, and a horizontal one from the left.
void DrawMeter(ImDrawList* draw, ImVec2 min, ImVec2 max, float level);
void DrawHorizontalMeter(ImDrawList* draw, ImVec2 min, ImVec2 max, float level);

// Draws a point of a line of values at `x`, over the heights from `top` to `bottom`: a square at a value's height, or a
// bar over a random value's range or a variable's whole lane with a square in its middle. A selected point is light. A
// point whose command runs only if the last comparison held is hollow. The Key Editor's controller lanes and the
// arrangement's tempo row draw their points this way.
void DrawPoint(ImDrawList* draw, float x, float top, float bottom, ImU32 colour, bool selected, bool conditional);

// The height on a fader or a meter, from 0 to 1, for a level in dB.
float FaderPosition(float level);

// A shortcut as each system writes it in menus: "Ctrl+Shift+Z" on Windows and "⇧⌘Z" on the Mac. The modifiers follow
// the Mac's order there.
std::string ShortcutText(ImGuiKeyChord keys);

// An info line: a strip across the window that shows the selected object's values, each after its label in capitals.
// BeginInfoLine draws the strip at the cursor. InfoField and NoObjectSelected fill it from the left, and EndInfoLine
// moves the cursor below it.
struct InfoLine
{
    ImDrawList* draw = nullptr;
    float x = 0.0f;      // where the next field starts
    float right = 0.0f;  // where the fields end. A field past it doesn't show, nor do the fields after it.
    float middle = 0.0f; // the strip's vertical middle
    ImVec2 origin;
    float bottom = 0.0f;
};
InfoLine BeginInfoLine();
void InfoField(InfoLine& line, const char* label, std::string_view value);
void NoObjectSelected(InfoLine& line);
void EndInfoLine(const InfoLine& line);

// The field of an info line that the user types in: its label, the text so far, and whether the field takes the
// keyboard's focus in the next frame. The field is empty while nobody types.
struct InfoTyping
{
    std::string field;
    std::array<char, 48> text{};
    bool focus = false;
};

// A field of an info line that can be typed in. A click on the value turns it into a text field, with the value
// selected. Enter, Tab or a click elsewhere returns the text, and Escape puts the value back. `typing` keeps the text
// between frames.
std::optional<std::string> InfoEntry(InfoLine& line, const char* label, std::string_view value, InfoTyping& typing);

// A box of text that the user types in: the box that takes the typing, the text so far, and whether the box takes the
// keyboard's focus in the next frame. The box is 0 while nobody types. `last` is the last box drawn. Tab and Shift+Tab
// end the field in frame `passed`. In that frame and the next, the next box drawn then takes the typing where `onward`
// holds, or else box `back`.
struct TextTyping
{
    ImGuiID box = 0;
    std::string text;
    bool focus = false;
    ImGuiID last = 0;
    bool onward = false;
    ImGuiID back = 0;
    int passed = -1;
};

// A box `width` wide at `pos` that shows `text` on one line. A click turns the box into a text field, with the text
// selected. Enter, Tab or a click elsewhere returns the field's text, and Escape returns `text` as it was. Tab then
// turns the next box that shares `typing` into a text field, and Shift+Tab the box before. The first box follows the
// last, and the last the first. `id` tells the box apart from the others in the ID scope. `typing` keeps the text
// between frames, and boxes can share it.
std::optional<std::string> TextEntry(const char* id, std::string_view text, ImVec2 pos, float width,
                                     TextTyping& typing);

// Draws the header of a zone and moves the cursor below it. The title sits on the left, and a detail on the right.
void ZoneHeader(std::string_view title, std::string_view detail);

// Draws a title and a line of explanation on a card in the middle of the rectangle from `min` to `max`.
void EmptyState(ImDrawList* draw, ImVec2 min, ImVec2 max, std::string_view title, std::string_view text);

// A label in the left column of a settings table. The label lines up with the control beside it.
void SettingLabel(const char* label);

// The centre nearest `centre` at which a window of `size` lies inside the main window, at least `margin` from its
// edges. A window that doesn't fit between the margins on an axis lies over the main window's middle on that axis.
ImVec2 CentreInside(ImVec2 centre, ImVec2 size, float margin);

// Shows `text` wrapped to the width left in the window, with the lines broken only at spaces. Dear ImGui's TextWrapped
// also breaks after a full stop, such as inside "main.dol". A word wider than the window runs past its edge.
void TextWrappedAtSpaces(std::string_view text);

// A button in the accent colour. Such a button marks the main action of a window. Inside a disabled block, it takes the
// look of the other buttons, faded. Returns true when it's clicked.
bool PrimaryButton(const char* label);

// A check box with `label` after it. A click on either turns `on` over, and returns true.
bool CheckBox(const char* label, bool* on);

// Shows `text` in a tooltip while the user hovers the last item, with its lines wrapped at a width that reads well.
// Empty text shows no tooltip. WrappedTooltip shows it whatever the mouse is over. The caller checks that.
void ItemTooltip(std::string_view text);
void WrappedTooltip(std::string_view text);

// While the mouse rests over the rectangle from `min` to `max` of the window, shows `text` in a tooltip. ItemTooltip
// does the same for an item. A control that takes no input, such as a grey value, gives no tooltip of its own.
void RectTooltip(ImVec2 min, ImVec2 max, std::string_view text);

// `text`, cut short with an ellipsis where it's wider than `width` in the present font. The cut falls between whole
// characters.
std::string FittedText(const std::string& text, float width);

// Makes the figures the font of the controls that follow, at the present size. A field that types a number then keeps
// the figures that its value shows in. ImGui::PopFont takes it off.
void PushFigures();

// Puts the theme's spacing back for the items of a menu or a popup. A popup takes the style that the code around it
// pushed, and the browser's tree and the mixer push tight spacing for their rows. ImGui::PopStyleVar takes it off
// before the popup ends.
void PushMenuSpacing();

// Whether `item` contains `filter`. The case of ASCII letters doesn't count. The search fields match names this way.
bool Matches(std::string_view item, std::string_view filter);

// A menu of `items`, `width` wide, with a field at its top that filters them by name (Matches). Unless `none` is null,
// it names a first item for no choice, at -1. The list grows with the items up to a height. Any choice closes the menu.
// Returns whether the user chose another item. `id` starts with ##.
bool FilteredCombo(const char* id, const std::vector<std::string>& items, int& chosen, const char* none,
                   std::array<char, 128>& filter, float width = -FLT_MIN);

// A row of switches side by side at the cursor, in a strip of `size`, each with a label. A switch whose entry in `lit`
// isn't 0 is lit in the accent colour. A switch whose entry in `disabled` isn't 0 takes no clicks. Its label is grey
// unless it's lit. Returns the switch that the user clicked, or -1. The Variables zone's switches and the Sound
// section's look the same. Inside a disabled block, the style's alpha fades the strip. `id` starts with ##.
int SegmentedSwitch(const char* id, std::span<const std::string> labels, std::span<const uint8_t> lit, ImVec2 size,
                    std::span<const uint8_t> disabled = {});

// Whether the window's menu bar, or one of its menus, has the focus. Only Windows draws the menu bar in the window. An
// editor that had the focus keeps it then. The menus' commands reach that editor.
bool MenuBarFocused();

// Whether Escape closes the window that's being drawn: the key comes while the window has the focus, and no text field
// is being typed in. The first Escape in a field ends the field alone. A window's open menu takes Escape first.
bool EscapeCloses();

} // namespace hokuto::app
