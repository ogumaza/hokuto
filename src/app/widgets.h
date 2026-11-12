// SPDX-License-Identifier: MIT

// Controls that Dear ImGui lacks: icons, icon buttons, the M and S buttons of a track, faders, pan knobs, meters,
// switches, menus that filter their items, and the header of each zone. Each one draws in the program's style.

#pragma once

#include <imgui.h>

#include <array>
#include <cfloat>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "app/icons.h"

namespace hokuto::format
{
struct File;
} // namespace hokuto::format

namespace hokuto::app
{

// The level of silence, in dB.
inline constexpr float kSilence = -std::numeric_limits<float>::infinity();

// The volume of a sound or of a region at full level, of 0 to 127. VolumeLevel shows it as 0 dB.
inline constexpr float kFullVolume = 127.0f;

// The height of the toolbar of the Key Editor and the List Editor, and the size of its buttons, before the display's
// scale.
inline constexpr float kToolbarHeight = 40.0f;
inline constexpr float kToolSize = 28.0f;

// Places the next control of an editor's toolbar at `x`, in the middle of the bar's height. The bar starts at `top`.
void PlaceInToolbar(float x, float top, float height);

// The top left corner and the width of an editor's toolbar.
struct Toolbar
{
    ImVec2 origin;
    float width = 0.0f;
};

// Draws an editor's toolbar at the cursor, across the room that the window has left: its fill, and a line at its
// bottom. EndToolbar ends it.
Toolbar BeginToolbar(ImDrawList* draw);

// Puts the cursor at the left edge under `bar`.
void EndToolbar(const Toolbar& bar);

// Draws a label in capitals at `x` in the middle of the toolbar, and returns the place after it.
float ToolbarCaps(ImDrawList* draw, float x, float top, const char* label);

// Draws text in `font` at `size` points.
void DrawText(ImDrawList* draw, ImFont* font, float size, ImVec2 pos, ImU32 colour, std::string_view text);
ImVec2 TextSize(ImFont* font, float size, std::string_view text);

// The distance from the top of a line of `font` at `size` points to its baseline. Text of two sizes lines up when their
// baselines meet.
float Ascent(ImFont* font, float size);

// `text`, cut short with an ellipsis to fit in `width` at `size` in `font`. The cut falls between UTF-8 characters.
std::string FitText(ImFont* font, float size, std::string_view text, float width);

// A line of text that FitText fits into `width` at `size` in `font`.
struct FittedLine
{
    ImFont* font = nullptr;
    float size = 0.0f;
    std::string_view text;
    float width = 0.0f;
};

// The tooltip of lines that FitText fits: every line that isn't empty, in full and one to a line, where any of them
// doesn't fit. Empty where they all fit.
std::string CutLinesTooltip(std::span<const FittedLine> lines);

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

// The level in dB at the top of a fader's travel.
inline constexpr float kFaderTop = 6.0f;

// A vertical fader for a level in dB, up to `highest`. Command-click on the Mac, or Ctrl-click elsewhere, sets 0 dB.
// Shift moves the fader finely. Returns true when the level changed.
bool Fader(const char* id, float* level, ImVec2 size, float highest = kFaderTop);

// A pan knob, from -1 for left to 1 for right. Command-click or Ctrl-click centres it.
bool PanKnob(const char* id, float* pan, float radius);

// The tooltip of a pan field from 0 to 127, in the Bank Editor and the Sound section.
inline constexpr char kPanTip[] = "Pan, 0–127: 0 is left, 64 is centre, 127 is right.";

// A horizontal slider for the level in dB of a send to `bus`, in a slot that names the bus on the left and shows the
// level on the right. The level sits on the fader's curve, and dragging right raises it. Command-click on the Mac, or
// Ctrl-click elsewhere, sets 0 dB. Shift moves the slider finely. Returns true when the level changed.
bool SendSlider(const char* id, const char* bus, float* level, ImVec2 size);

// A pan from -1 to 1 as the mixer writes it: C for the centre, or L or R and the distance from the centre in
// hundredths.
std::string PanText(float pan);

// A level in dB as the mixer writes it: to a tenth of a dB, with a sign above 0. A level below -60 dB is -∞.
std::string LevelText(float level);

// The level of a volume of a sound or of a region, from 0 to 127 at full volume, in dB as the mixer writes it.
std::string VolumeLevel(double volume);

// A gain in the decibels that a fader shows, and back. A gain of 0 is silence (kSilence).
float DecibelsOfGain(float gain);
float GainOfDecibels(float level);

// The item whose values the fields of an editor show: the version of the file, what the editor shows in it, such as a
// sound, a bank or a wave, and the item's place there, such as a track and a row. A value that the user typed while the
// program was busy waits for that item only (WaitingValues). The item refers to its version without keeping it. Two
// items compare the version's control block, not its address. A later version can't match a version that is gone,
// even at the same address. A version that is gone matches no version. An item without a version matches another item
// without one.
struct ShownItem
{
    friend bool operator==(const ShownItem& a, const ShownItem& b)
    {
        const bool same_version = !a.version.owner_before(b.version) && !b.version.owner_before(a.version);
        return same_version && !a.Expired() && a.shown == b.shown && a.place == b.place;
    }

    // The same version and the same thing that the editor shows, with `at` as the place.
    ShownItem At(std::array<uint64_t, 3> at) const
    {
        return ShownItem{version, shown, at};
    }

    // Whether the item had a version, and nothing keeps that version any more.
    bool Expired() const
    {
        const std::weak_ptr<const format::File> none;
        const bool had_version = version.owner_before(none) || none.owner_before(version);
        return had_version && version.expired();
    }

    std::weak_ptr<const format::File> version;
    uint64_t shown = 0;
    std::array<uint64_t, 3> place{};
};

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
// Inside a disabled block, a field that the user opened before still receives the keys (BeginOpenField). The value that
// it ends with waits until the slider is drawn enabled (WaitingValues), and the slider shows it. The slider then
// returns true with that value. SliderReleased tells the caller that the edit has ended.
bool ValueSlider(const char* id, float* value, const SliderRange& range, float reset, const char* format, ImVec2 size,
                 const SliderRange* typed = nullptr);

// Whether an edit of the ValueSlider drawn last has ended in this frame: the user let the slider go or ended its field
// (Dear ImGui's IsItemDeactivatedAfterEdit), or a value typed while the slider was disabled came back.
bool SliderReleased();

// Names the item whose values the ValueSliders show until PopSliderItem, such as a region of a bank or a sound. A value
// typed into such a slider while it was disabled waits for that item only (WaitingValues). Once the slider shows
// another item, the value drops, also where the slider shows the same value. Sliders outside such a block show an empty
// item.
void PushSliderItem(const ShownItem& item);
void PopSliderItem();

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

// Begins a window over the middle of the main window, in the style of the dialogs: Studio Setup, the Bank Editor and
// the like. The window doesn't collapse, dock or keep its settings, and `flags` add to that. The caller sets the size
// first. Returns ImGui::Begin's result: the caller ends the window either way.
bool BeginDialogWindow(const char* title, bool* open, ImGuiWindowFlags flags);

// Whether MIDI key `key` is a black key of a piano keyboard.
bool IsBlack(int key);

// The name of FX channel `channel` in the mixer: "FX A" for the first.
std::string FxName(std::size_t channel);

// Whether `byte` continues a character of UTF-8, rather than starting one.
bool IsContinuationByte(char byte);

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

// Whether the box of TextEntry with `id`, in the current ID scope, is a text field now.
bool TextEntryOpen(const char* id, const TextTyping& typing);

// Re-enables, inside a disabled block, a field that the user has open (`open`) while the program is `busy`. An editor
// draws its fields inside BeginDisabled while the program is busy. After each edit of a large archive, the program is
// busy for a second or more. Dear ImGui ignores the keys of a disabled field, and ends a disabled field under the
// pointer. Returns whether it re-enabled the field. EndOpenField then ends that, after the field.
bool BeginOpenField(bool open, bool busy);
void EndOpenField(bool reenabled);

// The values that the user typed into fields while the program was busy, by the field's ID. A field that the user
// opened while the program was free stays open (BeginOpenField). The value that it ends with waits here with the item
// that the field showed (ShownItem), and the field shows it. Once the program is free, the field returns the value. The
// result is the same as if the user had ended the field then. A field that shows another value by then drops it. So
// does a field that shows another item, also where it shows the same text. A field that isn't drawn for a frame drops
// its value too. That happens when its editor closes, when the user collapses its window, and when Dear ImGui clips it.
// The value then drops without a message, even if the field shows again before the program is free. A value whose
// version is gone drops as well. Only the item that a value was typed for can take it.
template <typename Value>
class WaitingValues
{
public:
    // The value that field `id` shows for `item`, where it shows `value` without a value that waits.
    const Value& Shown(const ShownItem& item, ImGuiID id, const Value& value) const
    {
        const auto found = waiting_.find(id);
        return found != waiting_.end() && Waits(found->second, item, value) ? found->second.typed : value;
    }

    // What field `id` returns for `item` in this frame: `ended`, the value that the user ended the field with in this
    // frame, if any, or a value that waited for the item. `value` is what the field shows without a value that waits.
    // While `busy`, `ended` waits, and the field returns nothing.
    std::optional<Value> Pass(const ShownItem& item, ImGuiID id, const Value& value, std::optional<Value> ended,
                              bool busy)
    {
        const int frame = ImGui::GetFrameCount();
        DropStale();
        const auto found = waiting_.find(id);
        if (busy)
        {
            if (ended)
            {
                waiting_[id] = Waiting{item, std::move(*ended), value, frame};
            }
            else if (found != waiting_.end() && found->second.item == item)
            {
                found->second.frame = frame;
            }
            else if (found != waiting_.end())
            {
                waiting_.erase(found);
            }
            return std::nullopt;
        }

        if (found == waiting_.end())
        {
            return ended;
        }

        if (!ended && Waits(found->second, item, value))
        {
            ended = std::move(found->second.typed);
        }
        waiting_.erase(found);

        return ended;
    }

    // Drops the value that waits for field `id`.
    void Drop(ImGuiID id)
    {
        waiting_.erase(id);
    }

    // Drops the values that no field can take any more: those of a field that wasn't drawn in the last frame, and
    // those whose version is gone.
    void DropStale()
    {
        const int frame = ImGui::GetFrameCount();
        std::erase_if(waiting_, [frame](const auto& entry)
                      { return frame > entry.second.frame + 1 || entry.second.item.Expired(); });
    }

    // Whether no value waits.
    bool Empty() const
    {
        return waiting_.empty();
    }

private:
    struct Waiting
    {
        ShownItem item;
        Value typed;
        Value before;
        int frame = 0;
    };

    // Whether `waiting` belongs to a field that shows `value` for `item` in this frame.
    static bool Waits(const Waiting& waiting, const ShownItem& item, const Value& value)
    {
        return waiting.item == item && waiting.before == value && ImGui::GetFrameCount() <= waiting.frame + 1;
    }

    std::map<ImGuiID, Waiting> waiting_;
};

// InfoEntry on a line that accepts no edits while the program is `busy`. While busy, a field that the user opened
// before stays a text field, and the others show their values. The text that a field ends with waits in `waiting` for
// `item` until the program is free, and the line shows it.
std::optional<std::string> BusyInfoEntry(InfoLine& line, const char* label, const std::string& value,
                                         InfoTyping& typing, WaitingValues<std::string>& waiting, const ShownItem& item,
                                         bool busy);

// Draws the header of a zone and moves the cursor below it. The title sits on the left, and a detail on the right.
void ZoneHeader(std::string_view title, std::string_view detail);

// Draws a title and a line of explanation on a card in the middle of the rectangle from `min` to `max`.
void EmptyState(ImDrawList* draw, ImVec2 min, ImVec2 max, std::string_view title, std::string_view text);

// Draws a heading in capitals at the cursor with `draw`, and moves the cursor past it: a section of the Export window
// or the Replace window, in the style of Studio Setup.
void DrawDialogHeading(ImDrawList* draw, const char* text);

// A label in the left column of a settings table. The label lines up with the control beside it.
void SettingLabel(const char* label);

// The centre nearest `centre` at which a window of `size` lies inside the main window, at least `margin` from its
// edges. A window that doesn't fit between the margins on an axis lies over the main window's middle on that axis.
ImVec2 CentreInside(ImVec2 centre, ImVec2 size, float margin);

// Where PlaceDialog put a modal window: whether the user drags it in this frame, and the centre it was given.
struct DialogPlace
{
    bool dragged = false;
    ImVec2 centre;
};

// Places the modal window `title` before it begins. The window opens centred on `place`, or else over the middle of the
// main window, at least `margin` from its edges. Once `top` is known, the window keeps that top and grows down. It
// moves up only to stay inside the main window. While the user drags it, the drag places it.
DialogPlace PlaceDialog(const std::string& title, const std::optional<ImVec2>& place, const std::optional<float>& top,
                        float margin);

// Keeps, after the window `placed` has begun, the centre that a drag sets in `place`. A click on the title bar that
// moves nothing keeps the old one. The first frame after the window appears sets `top`, and so does each frame of a
// drag.
void FollowDialogDrag(const DialogPlace& placed, std::optional<ImVec2>& place, std::optional<float>& top);

// Shows `text` in the muted colour of notes and empty lists.
void MutedText(const char* text);

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
// Return in the field chooses the one item left.
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
