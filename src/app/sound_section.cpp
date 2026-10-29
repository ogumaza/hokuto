// SPDX-License-Identifier: MIT

#include "app/sound_section.h"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "ntdlib/rsar/formats.h"
#include "ntdlib/sar/formats.h"

#include "app/key_edits.h"
#include "app/theme.h"
#include "app/widgets.h"

namespace hokuto::app
{
namespace
{

using edit::SoundChange;
using edit::SoundValue;
using theme::Px;

// The section's sizes, before the display's scale: the gap around a control in its row, a sub-heading's row, the
// labels' least column, the column of the bank slots' numbers, and the least size of a menu's list.
constexpr float kRowGap = 6.0f;
constexpr float kHeadingHeight = 26.0f;
constexpr float kLabelWidth = 84.0f;
constexpr float kSlotLabelWidth = 24.0f;
constexpr float kListWidth = 240.0f;
constexpr float kListHeight = 320.0f;

// A sound has four bank slots and sixteen tracks. The tracks' switches come in two rows.
constexpr int kBankSlots = 4;
constexpr int kTracks = 16;
constexpr int kTracksInRow = 8;

// The volume at which a sound plays at full level, and the middle of the priorities. A Command-click or a Ctrl-click on
// a slider sets these.
constexpr float kFullVolume = 127.0f;
constexpr float kMiddlePriority = 64.0f;

// nw::snd's pan modes and pan curves, by their numbers. Each curve comes plain, then with the centre at full level,
// then with neither side above full level (ntdlib::nwsnd::PanInfoOf).
constexpr std::array<const char*, 2> kPanModes = {"Dual", "Balance"};
constexpr std::array<const char*, 9> kPanCurves = {
    "Square root", "Square root, 0 dB", "Square root, 0 dB clamped",
    "Sine-cosine", "Sine-cosine, 0 dB", "Sine-cosine, 0 dB clamped",
    "Linear",      "Linear, 0 dB",      "Linear, 0 dB clamped",
};

// What each value does, for the tooltips of the labels.
const char* const kPlayerTip =
    "The sound player that plays this sound in the game. Each player plays a limited number of sounds at once.";
const char* const kPriorityTip =
    "The sound's priority in its player. When the player plays as many sounds as it can, a new sound stops the one of "
    "the lowest priority, or doesn't start.";
const char* const kActorTip =
    "The actor player that plays this sound for a sound actor, from 0 to 3. Each actor player plays a limited number "
    "of the actor's sounds at once.";
const char* const kVolumeTip = "The sound's volume, from 0 to 255. At 127 the sound plays at full level.";
const char* const kPanModeTip =
    "Dual pans each channel of a stereo wave on its own. Balance turns down one channel of a stereo wave.";
const char* const kPanCurveTip =
    "How the pan shares the level between left and right. Plain, the centre plays 3 dB down, or 6 dB down on the "
    "linear curve. With 0 dB, the centre plays at full level. Clamped, neither side plays above full level.";
const char* const kChannelPriorityTip =
    "The priority of the sound's notes, added to each track's priority. When the console runs out of voices, a new "
    "note takes the voice of the lowest priority.";
const char* const kReleaseTip =
    "On, a note keeps its priority in its release. Off, its priority falls to 1 when it's released, and a new note "
    "takes its voice first.";
const char* const kTracksTip =
    "The tracks that the sound allocates when it starts. A track that the sound doesn't allocate stays silent.";
const char* const kSlotTip =
    "The bank that the sound's notes play when the sequence selects this slot. Slot 0 plays until the sequence "
    "selects another.";

// Why an archive of the Wii doesn't hold a value, after the value's tooltip.
const char* const kNoActorTip = " Hokuto reads no actor player in an archive of the Wii.";
const char* const kNoPanModeTip = " An archive of the Wii holds the pan mode from version 1.2 on.";
const char* const kNoPanCurveTip = " An archive of the Wii holds the pan curve from version 1.2 on.";
const char* const kNoReleaseTip = " An archive of the Wii holds the release priority fix from version 1.3 on.";
const char* const kNoHoldTip = " The file's build of NW4R SND reads no hold. Its builds from March 2008 on read it.";

// Why a wave sound's value shows grey, after the value's tooltip.
const char* const kCtrNoFiltersTip = " The 3DS reads no filters from a wave sound file.";
const char* const kWiiNoFiltersTip = " A wave sound file of the Wii holds no filters.";
const char* const kOldFiltersTip =
    " The wave sound file has no place for the filters. The Wii U and the Switch read them from version 1.1.0 of the "
    "file on.";
const char* const kCtrNoAuxCTip = " The 3DS has no aux bus C. Its sounds send to aux buses A and B only.";
const char* const kNoPlaceTip = " The wave sound file has no place for this value.";
const char* const kWiiSlotTip = "The bank that the sound's notes play. A sound of the Wii plays one bank.";

// What a wave sound's values in its wave sound file do, for the tooltips of their labels.
const char* const kWaveTip = "The wave that the sound plays. Its wave sound file names the wave.";
const char* const kRootKeyTip =
    "The root key that the wave sound file gives the sound's note. The Switch and the Wii U don't read it: they play "
    "the wave at the wave's rate times the sound's pitch.";
const char* const kPitchTip =
    "The rate at which the sound plays its wave, as a share of the wave's rate. At 1 the wave plays as it is, and at "
    "2 an octave higher.";
const char* const kWavePanTip = "The sound's pan, from 0 for left to 127 for right. 64 is the centre.";
const char* const kSurroundPanTip =
    "The sound's surround pan, from 0 to 127. It places the sound between the front and the back of a surround mix.";
const char* const kMainSendTip =
    "The level that the sound sends to the main output, from 0 to 127. At 127 the sound plays at full level.";
const char* const kAuxSendTips[] = {
    "The level that the sound sends to aux bus A, from 0 to 127. The mixer's FX channel A holds the bus's effects.",
    "The level that the sound sends to aux bus B, from 0 to 127. The mixer's FX channel B holds the bus's effects.",
    "The level that the sound sends to aux bus C, from 0 to 127. The mixer's FX channel C holds the bus's effects.",
};
const char* const kLowPassTip =
    "The low-pass filter, from 0 to 64. At 64 the filter stays open, and lower values close it further.";
const char* const kBiquadTypeTip =
    "The biquad filter: 0 for none, 1 for a low-pass, 2 for a high-pass, and 3 to 5 for band-passes. On the Switch, 6 "
    "to 10 are the same filters made for 48 kHz.";
const char* const kBiquadValueTip = "How strongly the biquad filter acts, from 0 to 127. At 0 it does nothing.";
const char* const kWaveAttackTip =
    "How fast the sound rises to full level, from 0 to 127. At 127 it starts at full level.";
const char* const kWaveHoldTip = "How long the sound stays at full level after its attack, from 0 to 127.";
const char* const kWaveDecayTip = "How fast the sound falls to its sustain level, from 0 to 127. 127 is the fastest.";
const char* const kWaveSustainTip =
    "The level that the sound holds until the game stops it, from 0 to 127. At 127 it holds at full level.";
const char* const kWaveReleaseTip =
    "How fast the sound fades once the game stops it, from 0 to 127. 127 is the fastest.";

// A value of a wave sound in its wave sound file that a slider sets.
enum class WaveField
{
    kPitch,
    kPan,
    kSurroundPan,
    kMainSend,
    kAuxSendA,
    kAuxSendB,
    kAuxSendC,
    kLowPass,
    kBiquadType,
    kBiquadValue,
    kAttack,
    kHold,
    kDecay,
    kSustain,
    kRelease,
};

// A row of a wave sound's slider: its value, its label, its name in the history, what it does, its range, the value
// that a Command-click or a Ctrl-click sets, and the text that `format` writes.
struct WaveRow
{
    WaveField field;
    const char* label;
    const char* name;
    const char* tooltip;
    SliderRange range;
    float reset;
    const char* format;
};

// The rows, in the order that the section draws them under their sub-headings: the pitch, the pans and the sends, then
// the filters, then the envelope.
constexpr SliderRange kByteRange{0.0f, 127.0f};
const std::array<WaveRow, 15> kWaveRows = {{
    {WaveField::kPitch, "Pitch", "Pitch", kPitchTip, {0.25f, 4.0f, true}, 1.0f, "%.3f"},
    {WaveField::kPan, "Pan", "Pan", kWavePanTip, kByteRange, 64.0f, "%.0f"},
    {WaveField::kSurroundPan, "Surround pan", "Surround Pan", kSurroundPanTip, kByteRange, 0.0f, "%.0f"},
    {WaveField::kMainSend, "Main send", "Main Send", kMainSendTip, kByteRange, 127.0f, "%.0f"},
    {WaveField::kAuxSendA, "Aux A", "Aux Send A", kAuxSendTips[0], kByteRange, 0.0f, "%.0f"},
    {WaveField::kAuxSendB, "Aux B", "Aux Send B", kAuxSendTips[1], kByteRange, 0.0f, "%.0f"},
    {WaveField::kAuxSendC, "Aux C", "Aux Send C", kAuxSendTips[2], kByteRange, 0.0f, "%.0f"},
    {WaveField::kLowPass, "Low-pass", "Low-Pass", kLowPassTip, {0.0f, 64.0f}, 64.0f, "%.0f"},
    {WaveField::kBiquadType, "Biquad", "Biquad Filter", kBiquadTypeTip, {0.0f, 10.0f}, 0.0f, "%.0f"},
    {WaveField::kBiquadValue, "Biquad value", "Biquad Value", kBiquadValueTip, kByteRange, 0.0f, "%.0f"},
    {WaveField::kAttack, "Attack", "Attack", kWaveAttackTip, kByteRange, 127.0f, "%.0f"},
    {WaveField::kHold, "Hold", "Hold", kWaveHoldTip, kByteRange, 0.0f, "%.0f"},
    {WaveField::kDecay, "Decay", "Decay", kWaveDecayTip, kByteRange, 127.0f, "%.0f"},
    {WaveField::kSustain, "Sustain", "Sustain", kWaveSustainTip, kByteRange, 127.0f, "%.0f"},
    {WaveField::kRelease, "Release", "Release", kWaveReleaseTip, kByteRange, 127.0f, "%.0f"},
}};

// The first row of each sub-heading of a wave sound's values after the first.
constexpr std::size_t kFirstFilterRow = 7;
constexpr std::size_t kFirstEnvelopeRow = 10;

// The pitches that the user can type.
constexpr SliderRange kTypedPitch{0.01f, 8.0f, true};

// The labels of the tags that the program offers, in the order of format::kTagKeys, and what each one holds.
constexpr std::array<const char*, format::kTagKeys.size()> kTagLabels = {
    "Title", "Artist", "Game", "Year", "Comment", "Copyright", "Length", "Fade",
};
constexpr std::array<const char*, format::kTagKeys.size()> kTagTips = {
    "The title, such as the name of the piece or of the soundtrack.",
    "Who wrote or performed the music.",
    "The game that the music comes from.",
    "The year of the game's release.",
    "A note about the music.",
    "Who holds the copyright.",
    "How long a player plays before the fade, such as 2:30. PSF files' tags hold the same. Hokuto's playback "
    "ignores it.",
    "How long the fade lasts, in seconds. PSF files' tags hold the same. Hokuto's playback ignores it.",
};

// The rows as the section draws them: the left edge, the width, the top of the next row, and the height of a control.
struct Rows
{
    ImDrawList* draw = nullptr;
    float left = 0.0f;
    float width = 0.0f;
    float y = 0.0f;
    float control = 0.0f;
};

// The place of a row's control: its top left corner, and its width.
struct Control
{
    ImVec2 pos;
    float width = 0.0f;
};

// The labels' column: the widest label of the section and a gap, and kLabelWidth at least. Each system's font gives the
// labels other widths.
float LabelWidth()
{
    constexpr std::array<const char*, 8> kLabels = {
        "Player", "Priority", "Actor player", "Volume", "Pan mode", "Release fix", "Wave", "Root key",
    };
    static float scale = 0.0f;
    static float width = 0.0f;
    if (scale != Px(1.0f))
    {
        ImFont* text = theme::GetFonts().text;
        float widest = 0.0f;
        const auto measure = [&](const char* label)
        {
            widest = std::max(widest, TextSize(text, theme::kTextSize, label).x);
        };
        std::ranges::for_each(kLabels, measure);
        std::ranges::for_each(kTagLabels, measure);
        for (const WaveRow& row : kWaveRows)
        {
            measure(row.label);
        }
        scale = Px(1.0f);
        width = std::max(Px(kLabelWidth), std::ceil(widest + Px(12.0f)));
    }

    return width;
}

// Draws a sub-heading in capitals, and moves to the next row.
void Heading(Rows& rows, const char* text)
{
    const ImVec2 size = CapsSize(text);
    DrawCaps(rows.draw, ImVec2(rows.left, rows.y + Px(kHeadingHeight) - size.y - Px(6.0f)), theme::kTextMuted, text);
    rows.y += Px(kHeadingHeight);
}

// Draws `label` at `pos` in a box of `size`, centred down the box, with `tooltip` while the mouse is over the box. Each
// row's tooltip differs from the others' in its ID scope. A grey label marks a value that the console doesn't read, or
// that the file has no place for.
void DrawLabel(const Rows& rows, ImVec2 pos, ImVec2 size, const char* label, const char* tooltip, bool grey = false)
{
    ImFont* text = theme::GetFonts().text;
    const ImVec2 text_size = TextSize(text, theme::kTextSize, label);
    DrawText(rows.draw, text, theme::kTextSize, ImVec2(pos.x, pos.y + std::floor((size.y - text_size.y) * 0.5f)),
             grey ? theme::kTextDim : theme::kTextMuted, label);
    ImGui::SetCursorScreenPos(pos);
    ImGui::PushID(tooltip);
    ImGui::InvisibleButton("##label", size);
    ImGui::PopID();
    ItemTooltip(tooltip);
}

// Draws a row's label in a column `label_width` wide, and moves to the next row. Returns the place of the row's
// control, at the column's right. A grey control takes no input, and the label's tooltip shows over it too.
Control Label(Rows& rows, const char* label, const char* tooltip, float label_width = LabelWidth(), bool grey = false)
{
    const float height = rows.control + Px(kRowGap);
    DrawLabel(rows, ImVec2(rows.left, rows.y), ImVec2(label_width, height), label, tooltip, grey);
    const Control control{ImVec2(rows.left + label_width, rows.y + std::floor(Px(kRowGap) * 0.5f)),
                          rows.width - label_width};
    if (grey)
    {
        RectTooltip(control.pos, ImVec2(control.pos.x + control.width, control.pos.y + rows.control), tooltip);
    }
    rows.y += height;

    return control;
}

// The height of a line of controls under a label on a row of its own.
float LineHeight(const Rows& rows)
{
    return rows.control + Px(kRowGap) * 0.5f;
}

// Draws a label on a row of its own, and moves past it and `lines` rows of controls under it. Returns the place of the
// first control. The control is as wide as the rows, and the next control lies a line lower (LineHeight). The tooltip
// of a grey label shows over its controls too.
Control LabelAbove(Rows& rows, const char* label, const char* tooltip, int lines, bool grey = false)
{
    const float height = std::floor(TextSize(theme::GetFonts().text, theme::kTextSize, label).y + Px(kRowGap));
    DrawLabel(rows, ImVec2(rows.left, rows.y), ImVec2(LabelWidth(), height), label, tooltip, grey);
    const Control control{ImVec2(rows.left, rows.y + height), rows.width};
    if (grey)
    {
        const float below = LineHeight(rows) * static_cast<float>(lines);
        RectTooltip(control.pos, ImVec2(control.pos.x + control.width, control.pos.y + below), tooltip);
    }
    rows.y += height + LineHeight(rows) * static_cast<float>(lines) + Px(kRowGap) * 0.5f;

    return control;
}

// A slider of number `value` at `control`, from 0 to its highest, with the text that `format` writes. A Command-click
// or a Ctrl-click sets `reset`. The section holds the value that the user drags the slider to. Returns the change once
// the user lets the slider go.
std::optional<SoundChange> Slider(const Rows& rows, Control control, SoundValue value, uint32_t current, float reset,
                                  const char* format, SoundSection& section)
{
    const bool holding = section.held && section.held->value == value;
    float shown = static_cast<float>(holding ? section.held->to : current);
    const SliderRange range{0.0f, static_cast<float>(edit::HighestOf(value))};
    ImGui::SetCursorScreenPos(control.pos);
    ImGui::PushID(static_cast<int>(value));
    if (ValueSlider("##slider", &shown, range, reset, format, ImVec2(control.width, rows.control)))
    {
        const long rounded = std::lround(std::clamp(shown, range.low, range.high));
        section.held = SoundChange{value, 0, static_cast<uint32_t>(rounded)};
    }
    const bool released = ImGui::IsItemDeactivatedAfterEdit();
    ImGui::PopID();
    if (released && section.held && section.held->value == value)
    {
        return std::exchange(section.held, std::nullopt);
    }

    return std::nullopt;
}

// A menu of `names` at `control`, showing the name of `current`. Returns the change to the choice that the user made,
// if it's another.
std::optional<SoundChange> Menu(Control control, SoundValue value, std::span<const char* const> names, uint32_t current)
{
    ImGui::SetCursorScreenPos(control.pos);
    ImGui::SetNextItemWidth(control.width);
    ImGui::SetNextWindowSizeConstraints(ImVec2(Px(kListWidth), 0.0f), ImVec2(FLT_MAX, Px(kListHeight)));
    ImGui::PushID(static_cast<int>(value));
    std::optional<SoundChange> change;
    if (ImGui::BeginCombo("##menu", current < names.size() ? names[current] : ""))
    {
        for (std::size_t i = 0; i < names.size(); i++)
        {
            if (ImGui::Selectable(names[i], i == current) && i != current)
            {
                change = SoundChange{value, 0, static_cast<uint32_t>(i)};
            }
        }
        ImGui::EndCombo();
    }
    ImGui::PopID();

    return change;
}

// Switches at `control` with `labels`, the one of `current` lit. Returns the change to the switch that the user
// clicked, if it's another.
std::optional<SoundChange> Switches(const Rows& rows, Control control, SoundValue value,
                                    std::span<const std::string> labels, uint32_t current)
{
    std::vector<uint8_t> lit(labels.size(), 0);
    if (current < lit.size())
    {
        lit[current] = 1;
    }
    ImGui::SetCursorScreenPos(control.pos);
    ImGui::PushID(static_cast<int>(value));
    const int clicked = SegmentedSwitch("##switches", labels, lit, ImVec2(control.width, rows.control));
    ImGui::PopID();
    if (clicked < 0 || static_cast<uint32_t>(clicked) == current)
    {
        return std::nullopt;
    }

    return SoundChange{value, 0, static_cast<uint32_t>(clicked)};
}

// The level of a sound at `volume`, in dB as the mixer writes it.
std::string VolumeLevel(uint32_t volume)
{
    return LevelText(static_cast<float>(20.0 * std::log10(static_cast<double>(volume) / kFullVolume))) + " dB";
}

// The index of item `id` among the items of `type`, or -1 for an ID of another type, or past `count`.
int IndexOf(uint32_t id, uint32_t type, std::size_t count)
{
    const bool listed = ntdlib::sar::ItemType(id) == type && ntdlib::sar::ItemNumber(id) < count;
    return listed ? static_cast<int>(ntdlib::sar::ItemNumber(id)) : -1;
}

// The player's row and the rows of the values that the player uses. Returns the change that the user made.
std::optional<SoundChange> DrawPlayer(Rows& rows, SoundSection& section)
{
    const SoundView& view = *section.view;
    const format::NativeSound& values = view.values;
    std::optional<SoundChange> change;
    Heading(rows, "Player");

    // The player, from a menu of the archive's players.
    const Control player = Label(rows, "Player", kPlayerTip);
    int chosen = IndexOf(values.player, ntdlib::sar::kPlayerItem, view.players.size());
    ImGui::SetCursorScreenPos(player.pos);
    ImGui::SetNextWindowSizeConstraints(ImVec2(Px(kListWidth), 0.0f), ImVec2(FLT_MAX, FLT_MAX));
    if (FilteredCombo("##player", view.players, chosen, nullptr, section.filter, player.width) && chosen >= 0)
    {
        change = SoundChange{SoundValue::kPlayer, 0,
                             ntdlib::sar::ItemId(ntdlib::sar::kPlayerItem, static_cast<uint32_t>(chosen))};
    }

    // The priority in the player and the actor player, greyed where the archive holds none.
    const bool held = view.held.actor_player;
    const std::string actor_tip = std::string(kActorTip) + (held ? "" : kNoActorTip);
    const Control priority = Label(rows, "Priority", kPriorityTip);
    const Control actor = Label(rows, "Actor player", actor_tip.c_str(), LabelWidth(), !held);
    const std::array<std::string, 4> kActors = {"0", "1", "2", "3"};
    const std::optional<SoundChange> moved =
        Slider(rows, priority, SoundValue::kPlayerPriority, values.player_priority, kMiddlePriority, "%.0f", section);
    ImGui::BeginDisabled(!held);
    const std::optional<SoundChange> switched =
        Switches(rows, actor, SoundValue::kActorPlayer, kActors, values.actor_player);
    ImGui::EndDisabled();

    return change ? change : moved ? moved : switched;
}

// The rows of the volume and the pan. Returns the change that the user made.
std::optional<SoundChange> DrawOutput(Rows& rows, SoundSection& section)
{
    const SoundView& view = *section.view;
    const format::NativeSound& values = view.values;
    Heading(rows, "Output");

    // The volume, with its level in dB.
    const Control volume = Label(rows, "Volume", kVolumeTip);
    const bool holding = section.held && section.held->value == SoundValue::kVolume;
    const std::string format = "%.0f  (" + VolumeLevel(holding ? section.held->to : values.volume) + ")";
    std::optional<SoundChange> change =
        Slider(rows, volume, SoundValue::kVolume, values.volume, kFullVolume, format.c_str(), section);

    // The pan's mode and curve, greyed where the archive holds neither. The curves' names take the rows' width.
    const bool held = view.held.pan;
    const std::string mode_tip = std::string(kPanModeTip) + (held ? "" : kNoPanModeTip);
    const std::string curve_tip = std::string(kPanCurveTip) + (held ? "" : kNoPanCurveTip);
    const Control mode = Label(rows, "Pan mode", mode_tip.c_str(), LabelWidth(), !held);
    const Control curve = LabelAbove(rows, "Pan curve", curve_tip.c_str(), 1, !held);
    const std::array<std::string, 2> kModes = {kPanModes[0], kPanModes[1]};
    ImGui::BeginDisabled(!held);
    const std::optional<SoundChange> switched = Switches(rows, mode, SoundValue::kPanMode, kModes, values.pan_mode);
    const std::optional<SoundChange> chosen = Menu(curve, SoundValue::kPanCurve, kPanCurves, values.pan_curve);
    ImGui::EndDisabled();

    return change ? change : switched ? switched : chosen;
}

// The rows of a sequence sound's notes: their priority, the release priority fix and the tracks. Returns the change
// that the user made.
std::optional<SoundChange> DrawNotes(Rows& rows, SoundSection& section)
{
    const SoundView& view = *section.view;
    const format::NativeSound& values = view.values;
    Heading(rows, "Notes");

    // The priority and the release priority fix, greyed where the archive holds none.
    const bool held = view.held.release_priority_fix;
    const std::string release_tip = std::string(kReleaseTip) + (held ? "" : kNoReleaseTip);
    const Control priority = Label(rows, "Priority", kChannelPriorityTip);
    const Control release = Label(rows, "Release fix", release_tip.c_str(), LabelWidth(), !held);
    const std::array<std::string, 2> kFix = {"Off", "On"};
    const std::optional<SoundChange> moved =
        Slider(rows, priority, SoundValue::kChannelPriority, values.channel_priority, kMiddlePriority, "%.0f", section);
    ImGui::BeginDisabled(!held);
    const std::optional<SoundChange> switched =
        Switches(rows, release, SoundValue::kReleasePriorityFix, kFix, values.release_priority_fix ? 1 : 0);
    ImGui::EndDisabled();
    std::optional<SoundChange> change = moved ? moved : switched;

    // The tracks that the sound allocates, a switch each, in two lines under their label. A click on a switch turns its
    // track on or off.
    const Control tracks = LabelAbove(rows, "Tracks", kTracksTip, kTracks / kTracksInRow);
    for (int line = 0; line < kTracks / kTracksInRow; line++)
    {
        std::array<std::string, kTracksInRow> labels;
        std::array<uint8_t, kTracksInRow> lit{};
        for (int i = 0; i < kTracksInRow; i++)
        {
            const int track = line * kTracksInRow + i;
            labels[static_cast<std::size_t>(i)] = std::to_string(track);
            lit[static_cast<std::size_t>(i)] = (values.tracks >> track & 1) != 0 ? 1 : 0;
        }
        ImGui::SetCursorScreenPos(ImVec2(tracks.pos.x, tracks.pos.y + LineHeight(rows) * static_cast<float>(line)));
        ImGui::PushID(line);
        const int clicked = SegmentedSwitch("##tracks", labels, lit, ImVec2(tracks.width, rows.control));
        ImGui::PopID();
        if (clicked >= 0)
        {
            const uint32_t flag = 1u << (line * kTracksInRow + clicked);
            change = SoundChange{SoundValue::kTracks, 0, values.tracks ^ flag};
        }
    }

    return change;
}

// The rows of a sequence sound's bank slots, each with a menu of the archive's banks. The slots' numbers label the
// rows, and the menus take the rest of the width. A sound of the Wii has one slot. Returns the change that the user
// made.
std::optional<SoundChange> DrawBanks(Rows& rows, SoundSection& section)
{
    const SoundView& view = *section.view;
    std::optional<SoundChange> change;
    const int slots = std::min(view.held.bank_slots, kBankSlots);
    Heading(rows, "Bank slots");
    for (int slot = 0; slot < slots; slot++)
    {
        const std::string label = std::to_string(slot);
        ImGui::PushID(slot);
        const Control control = Label(rows, label.c_str(), slots == 1 ? kWiiSlotTip : kSlotTip, Px(kSlotLabelWidth));
        const uint32_t bank = edit::ValueOf(view.values, SoundValue::kBank, slot);
        int chosen = IndexOf(bank, ntdlib::sar::kBankItem, view.banks.size());
        ImGui::SetCursorScreenPos(control.pos);
        ImGui::SetNextWindowSizeConstraints(ImVec2(Px(kListWidth), 0.0f), ImVec2(FLT_MAX, FLT_MAX));
        if (FilteredCombo("##bank", view.banks, chosen, "None", section.filter, control.width))
        {
            const uint32_t to = chosen < 0 ? ntdlib::sar::kNoItem
                                           : ntdlib::sar::ItemId(ntdlib::sar::kBankItem, static_cast<uint32_t>(chosen));
            change = SoundChange{SoundValue::kBank, slot, to};
        }
        ImGui::PopID();
    }

    return change;
}

// Value `field` of a wave sound's values.
float WaveValueOf(const ntdlib::sar::WaveSoundFile::WaveSound& values, WaveField field)
{
    switch (field)
    {
    case WaveField::kPitch:
        return values.pitch;
    case WaveField::kPan:
        return values.pan;
    case WaveField::kSurroundPan:
        return values.span;
    case WaveField::kMainSend:
        return values.main_send;
    case WaveField::kAuxSendA:
        return values.aux_sends[0];
    case WaveField::kAuxSendB:
        return values.aux_sends[1];
    case WaveField::kAuxSendC:
        return values.aux_sends[2];
    case WaveField::kLowPass:
        return values.low_pass;
    case WaveField::kBiquadType:
        return values.biquad_type;
    case WaveField::kBiquadValue:
        return values.biquad_value;
    case WaveField::kAttack:
        return values.adshr.attack;
    case WaveField::kHold:
        return values.adshr.hold;
    case WaveField::kDecay:
        return values.adshr.decay;
    case WaveField::kSustain:
        return values.adshr.sustain;
    case WaveField::kRelease:
        return values.adshr.release;
    }

    return 0.0f;
}

// The byte of value `field` of a wave sound's values. The pitch has none.
uint8_t* WaveByteOf(ntdlib::sar::WaveSoundFile::WaveSound& values, WaveField field)
{
    switch (field)
    {
    case WaveField::kPan:
        return &values.pan;
    case WaveField::kSurroundPan:
        return &values.span;
    case WaveField::kMainSend:
        return &values.main_send;
    case WaveField::kAuxSendA:
        return &values.aux_sends[0];
    case WaveField::kAuxSendB:
        return &values.aux_sends[1];
    case WaveField::kAuxSendC:
        return &values.aux_sends[2];
    case WaveField::kLowPass:
        return &values.low_pass;
    case WaveField::kBiquadType:
        return &values.biquad_type;
    case WaveField::kBiquadValue:
        return &values.biquad_value;
    case WaveField::kAttack:
        return &values.adshr.attack;
    case WaveField::kHold:
        return &values.adshr.hold;
    case WaveField::kDecay:
        return &values.adshr.decay;
    case WaveField::kSustain:
        return &values.adshr.sustain;
    case WaveField::kRelease:
        return &values.adshr.release;
    case WaveField::kPitch:
        break;
    }

    return nullptr;
}

// Whether the wave sound file has a place for value `field` of the sound that the console reads, by the places of its
// settings.
bool HasPlace(const edit::WaveSoundFields& fields, WaveField field)
{
    const ntdlib::sar::WaveSoundPlaces& places = fields.places;
    switch (field)
    {
    case WaveField::kPitch:
        return places.pitch;
    case WaveField::kPan:
    case WaveField::kSurroundPan:
        return places.pan;
    case WaveField::kMainSend:
        return places.sends;
    case WaveField::kAuxSendA:
        return places.sends && places.aux_sends > 0;
    case WaveField::kAuxSendB:
        return places.sends && places.aux_sends > 1;
    case WaveField::kAuxSendC:
        return places.sends && places.aux_sends > 2;
    case WaveField::kLowPass:
    case WaveField::kBiquadType:
    case WaveField::kBiquadValue:
        return places.filters;
    case WaveField::kAttack:
    case WaveField::kHold:
    case WaveField::kDecay:
    case WaveField::kSustain:
    case WaveField::kRelease:
        return places.envelope;
    }

    return false;
}

// The text of `value` as row `row` writes it.
std::string WaveValueText(const WaveRow& row, float value)
{
    std::array<char, 32> text{};
    std::snprintf(text.data(), text.size(), row.format, static_cast<double>(value));

    return text.data();
}

// The format of a wave sound's pan of `pan`, from 0 to 127: the value, and its side as the mixer and the Bank Editor
// write a pan.
std::string PanFormat(float pan)
{
    constexpr float kCentre = 64.0f;
    const float side = pan < kCentre ? kCentre : 127.0f - kCentre;
    return "%.0f  (" + PanText((std::round(pan) - kCentre) / side) + ")";
}

// A slider of row `row` of a wave sound's values at `control`. The section holds the value that the user drags the
// slider to. Returns the value once the user lets the slider go. The pan shows its side too.
std::optional<float> WaveSlider(const Rows& rows, Control control, const WaveRow& row, float current,
                                SoundSection& section)
{
    const int index = static_cast<int>(row.field);
    const bool holding = section.wave_held && section.wave_held->first == index;
    float shown = holding ? section.wave_held->second : current;
    ImGui::SetCursorScreenPos(control.pos);
    ImGui::PushID(index);
    const SliderRange* typed = row.field == WaveField::kPitch ? &kTypedPitch : nullptr;
    const std::string format = row.field == WaveField::kPan ? PanFormat(shown) : std::string(row.format);
    if (ValueSlider("##wave", &shown, row.range, row.reset, format.c_str(), ImVec2(control.width, rows.control), typed))
    {
        section.wave_held = std::pair(index, shown);
    }
    const bool released = ImGui::IsItemDeactivatedAfterEdit();
    ImGui::PopID();
    if (released && section.wave_held && section.wave_held->first == index)
    {
        return std::exchange(section.wave_held, std::nullopt)->second;
    }

    return std::nullopt;
}

// Why value `field` of `view`'s wave sound shows grey, after the value's tooltip: the console doesn't read it, or the
// wave sound file has no place for it.
const char* GreyTip(const WaveSoundView& view, WaveField field)
{
    if (field == WaveField::kHold && !view.hold)
    {
        return kNoHoldTip;
    }
    if (field == WaveField::kLowPass || field == WaveField::kBiquadType || field == WaveField::kBiquadValue)
    {
        return view.ctr ? kCtrNoFiltersTip : view.wii ? kWiiNoFiltersTip : kOldFiltersTip;
    }
    if (field == WaveField::kAuxSendC && view.ctr)
    {
        return kCtrNoAuxCTip;
    }

    return kNoPlaceTip;
}

// The rows of a wave sound's values in its wave sound file: its wave, its root key, its pitch, its pans and its sends,
// then its filters and its envelope. A value that the file has no place for shows greyed. So does the root key: the
// consoles don't read it. So does the hold where the Wii's build of NW4R SND doesn't read it. What else an edit changes
// and the sizes follow. Returns the change that the user made.
std::optional<WaveSoundChange> DrawWaveSound(Rows& rows, SoundSection& section)
{
    const WaveSoundView& view = *section.wave_sound;
    const std::string where = "Sound: " + section.view->values.name + " - ";
    std::optional<WaveSoundChange> change;
    Heading(rows, "Wave sound");
    ImGui::BeginDisabled(section.wave_busy);

    // The wave, from a menu of the archive's waves, and the root key that the consoles don't read.
    const Control wave = Label(rows, "Wave", kWaveTip);
    const auto found = std::ranges::find(view.waves, view.fields.wave);
    const int present = found != view.waves.end() ? static_cast<int>(found - view.waves.begin()) : -1;
    int chosen = present;
    ImGui::SetCursorScreenPos(wave.pos);
    ImGui::SetNextWindowSizeConstraints(ImVec2(Px(kListWidth), 0.0f), ImVec2(FLT_MAX, FLT_MAX));
    if (FilteredCombo("##wave", view.wave_names, chosen, nullptr, section.filter, wave.width) && chosen >= 0 &&
        chosen != present)
    {
        const std::string from = present >= 0 ? view.wave_names[static_cast<std::size_t>(present)] : "another wave";
        change =
            WaveSoundChange{view.fields.values, view.waves[static_cast<std::size_t>(chosen)], "Change Wave Sound Wave",
                            where + "from " + from + " to " + view.wave_names[static_cast<std::size_t>(chosen)]};
    }
    const Control root = Label(rows, "Root key", kRootKeyTip, LabelWidth(), true);
    const int key = view.fields.values.notes.front().original_key;
    const std::string key_text = std::to_string(key) + "  (" + KeyName(key) + ")";
    ImFont* font = theme::GetFonts().text;
    const ImVec2 key_size = TextSize(font, theme::kTextSize, key_text);
    DrawText(rows.draw, font, theme::kTextSize,
             ImVec2(root.pos.x + Px(8.0f), root.pos.y + std::floor((rows.control - key_size.y) * 0.5f)),
             theme::kTextDim, key_text);

    // A slider for each value, greyed where the file has no place for it.
    for (std::size_t i = 0; i < kWaveRows.size(); ++i)
    {
        if (i == kFirstFilterRow)
        {
            Heading(rows, "Filters");
        }
        else if (i == kFirstEnvelopeRow)
        {
            Heading(rows, "Envelope");
        }

        // Only the Switch has the biquad filters of types 6 to 10.
        WaveRow row = kWaveRows[i];
        if (row.field == WaveField::kBiquadType)
        {
            row.range.high = static_cast<float>(view.biquad_types);
        }
        const bool old = row.field == WaveField::kHold && !view.hold;
        const bool place = HasPlace(view.fields, row.field) && !old;
        const std::string tip = std::string(row.tooltip) + (place ? "" : GreyTip(view, row.field));
        const Control control = Label(rows, row.label, tip.c_str(), LabelWidth(), !place);
        const float current = WaveValueOf(view.fields.values, row.field);
        ImGui::BeginDisabled(!place);
        const std::optional<float> moved = WaveSlider(rows, control, row, current, section);
        ImGui::EndDisabled();
        if (!moved)
        {
            continue;
        }

        ntdlib::sar::WaveSoundFile::WaveSound values = view.fields.values;
        if (uint8_t* byte = WaveByteOf(values, row.field))
        {
            *byte = static_cast<uint8_t>(std::lround(std::clamp(*moved, row.range.low, row.range.high)));
        }
        else
        {
            values.pitch = std::clamp(*moved, kTypedPitch.low, kTypedPitch.high);
        }
        const float to = WaveValueOf(values, row.field);
        if (to != current)
        {
            change = WaveSoundChange{values, view.fields.wave, std::string("Change Wave Sound ") + row.name,
                                     where + "from " + WaveValueText(row, current) + " to " + WaveValueText(row, to)};
        }
    }
    ImGui::EndDisabled();

    // What else an edit changes, and the sizes, in lines that start at the labels and wrap at the section's edge.
    ImGui::SetCursorScreenPos(ImVec2(rows.left, rows.y + Px(kRowGap)));
    ImGui::BeginGroup();
    ImGui::PushTextWrapPos(rows.left + rows.width - ImGui::GetWindowPos().x + ImGui::GetScrollX());
    const ImVec4 kMuted = ImGui::ColorConvertU32ToFloat4(theme::kTextMuted);
    ImGui::TextColored(kMuted, "%s", view.reach.c_str());
    for (const std::string& line : view.sizes)
    {
        ImGui::TextColored(kMuted, "%s", line.c_str());
    }
    ImGui::PopTextWrapPos();
    ImGui::EndGroup();
    rows.y = ImGui::GetItemRectMax().y + Px(kRowGap);

    return change;
}

// The rows of the sound's tags, a box of text for each key that the program offers. Returns the tag that the user
// typed, if it changed.
std::optional<TagChange> DrawTags(Rows& rows, SoundSection& section)
{
    Heading(rows, "Tags");
    std::optional<TagChange> change;
    for (std::size_t i = 0; i < format::kTagKeys.size(); i++)
    {
        const std::string key(format::kTagKeys[i]);
        const Control control = Label(rows, kTagLabels[i], kTagTips[i]);
        const auto found = section.view->tags.find(key);
        const std::string_view value = found != section.view->tags.end() ? std::string_view(found->second) : "";
        std::optional<std::string> typed = TextEntry(key.c_str(), value, control.pos, control.width, section.typing);
        if (typed && *typed != value)
        {
            change = TagChange{key, std::move(*typed)};
        }
    }

    return change;
}

// The view of sound `sound` of `file`. Nothing for a file without the archive's tables, or for a sound past them.
std::optional<SoundView> MakeSoundView(const format::File& file, uint32_t sound)
{
    if (!file.native || !file.native->tables || sound >= file.native->tables->sounds.size())
    {
        return std::nullopt;
    }

    const format::NativeTables& tables = *file.native->tables;
    SoundView view;
    view.sound = sound;
    view.values = tables.sounds[sound];
    for (const format::NativePlayer& player : tables.players)
    {
        view.players.push_back(player.name);
    }
    view.banks = tables.banks;
    view.held = edit::HeldValuesOf(file);
    if (const auto tags = file.sound_tags.find(sound); tags != file.sound_tags.end())
    {
        view.tags = tags->second;
    }

    return view;
}

} // namespace

void ShowSound(SoundSection& section, std::shared_ptr<const format::File> version, std::optional<uint32_t> sound)
{
    if (version == section.version && sound == section.sound)
    {
        return;
    }

    if (sound != section.sound)
    {
        section.typing = TextTyping();
    }
    section.view = version && sound ? MakeSoundView(*version, *sound) : std::nullopt;
    section.version = std::move(version);
    section.sound = sound;
    section.held.reset();
}

float DrawSoundValues(ImDrawList* draw, ImVec2 pos, float width, SoundSection& section)
{
    if (!section.view)
    {
        return pos.y;
    }

    // The controls take less padding than the windows' controls, and the rows stay compact.
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(Px(8.0f), Px(3.0f)));
    Rows rows{draw, pos.x, width, pos.y, ImGui::GetFrameHeight()};
    ImGui::PushID("sound");
    std::optional<SoundChange> change = DrawPlayer(rows, section);
    const std::optional<SoundChange> output = DrawOutput(rows, section);
    change = change ? change : output;
    if (section.view->values.kind == ntdlib::sar::SoundType::kSequence)
    {
        const std::optional<SoundChange> notes = DrawNotes(rows, section);
        const std::optional<SoundChange> banks = DrawBanks(rows, section);
        change = change ? change : notes ? notes : banks;
    }
    std::optional<WaveSoundChange> wave_change;
    if (section.wave_sound)
    {
        wave_change = DrawWaveSound(rows, section);
    }
    std::optional<TagChange> tag = DrawTags(rows, section);
    ImGui::PopID();
    ImGui::PopStyleVar();

    if (change)
    {
        section.changed = change;
    }
    if (tag)
    {
        section.tag_changed = std::move(tag);
    }
    if (wave_change)
    {
        section.wave_changed = std::move(wave_change);
    }

    return rows.y;
}

namespace
{

// ShowWaveSound for an archive of either kind.
template <class Archive>
bool ShowWaveSoundOf(SoundSection& section, std::shared_ptr<const format::File> version, const Archive* archive,
                     bool busy)
{
    section.wave_busy = busy;
    const bool wave_sound = section.view && section.view->values.kind == ntdlib::sar::SoundType::kWaveSound;
    if (!wave_sound || archive == nullptr || !version)
    {
        section.wave_sound.reset();
        section.wave_version.reset();
        section.wave_sound_of.reset();
        section.wave_held.reset();
        return false;
    }
    if (version == section.wave_version && section.sound == section.wave_sound_of)
    {
        return false;
    }

    if constexpr (std::is_same_v<Archive, ntdlib::rsar::SoundArchive>)
    {
        section.wave_sound = MakeWaveSoundView(*version, *archive, *section.sound);
    }
    else
    {
        section.wave_sound = MakeWaveSoundView(*archive, *section.sound);
    }
    section.wave_version = std::move(version);
    section.wave_sound_of = section.sound;
    section.wave_held.reset();

    return true;
}

} // namespace

bool ShowWaveSound(SoundSection& section, std::shared_ptr<const format::File> version,
                   const ntdlib::sar::SoundArchive* archive, bool busy)
{
    return ShowWaveSoundOf(section, std::move(version), archive, busy);
}

bool ShowWaveSound(SoundSection& section, std::shared_ptr<const format::File> version,
                   const ntdlib::rsar::SoundArchive* archive, bool busy)
{
    return ShowWaveSoundOf(section, std::move(version), archive, busy);
}

std::string TagLabel(std::string_view key)
{
    const auto found = std::find(format::kTagKeys.begin(), format::kTagKeys.end(), key);
    return found != format::kTagKeys.end() ? kTagLabels[static_cast<std::size_t>(found - format::kTagKeys.begin())]
                                           : std::string(key);
}

std::string TagTip(std::string_view key)
{
    const auto found = std::find(format::kTagKeys.begin(), format::kTagKeys.end(), key);
    return found != format::kTagKeys.end() ? kTagTips[static_cast<std::size_t>(found - format::kTagKeys.begin())]
                                           : std::string();
}

std::string SoundValueText(const SoundView& view, edit::SoundValue value, uint32_t value_of)
{
    switch (value)
    {
    case SoundValue::kPlayer:
        {
            const int player = IndexOf(value_of, ntdlib::sar::kPlayerItem, view.players.size());
            return player < 0 ? "no player" : view.players[static_cast<std::size_t>(player)];
        }

    case SoundValue::kVolume:
        return std::to_string(value_of) + " (" + VolumeLevel(value_of) + ")";

    case SoundValue::kPanMode:
        return value_of < kPanModes.size() ? kPanModes[value_of] : std::to_string(value_of);

    case SoundValue::kPanCurve:
        return value_of < kPanCurves.size() ? kPanCurves[value_of] : std::to_string(value_of);

    case SoundValue::kBank:
        {
            const int bank = IndexOf(value_of, ntdlib::sar::kBankItem, view.banks.size());
            return bank < 0 ? "None" : view.banks[static_cast<std::size_t>(bank)];
        }

    case SoundValue::kTracks:
        {
            std::string text;
            for (int track = 0; track < kTracks; track++)
            {
                if ((value_of >> track & 1) != 0)
                {
                    text += (text.empty() ? "" : ", ") + std::to_string(track);
                }
            }
            return text.empty() ? "none" : text;
        }

    case SoundValue::kReleasePriorityFix:
        return value_of != 0 ? "On" : "Off";

    case SoundValue::kPlayerPriority:
    case SoundValue::kActorPlayer:
    case SoundValue::kChannelPriority:
        break;
    }

    return std::to_string(value_of);
}

} // namespace hokuto::app
