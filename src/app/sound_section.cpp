// SPDX-License-Identifier: MIT

// The Sound section of the inspector and the browser: the rows of a sound's values in the archive's tables.

#include "app/sound_section.h"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/sar/formats.h"

#include "app/sound_section_parts.h"
#include "app/theme.h"
#include "app/widgets.h"
#include "edit/ds_tables.h"
#include "edit/gba_tables.h"

namespace hokuto::app
{
namespace
{

using edit::SoundChange;
using edit::SoundValue;
using sound_section_parts::Control;
using sound_section_parts::DrawWaveSound;
using sound_section_parts::Heading;
using sound_section_parts::kHeadingHeight;
using sound_section_parts::kLabelWidth;
using sound_section_parts::kListHeight;
using sound_section_parts::kListWidth;
using sound_section_parts::kRowGap;
using sound_section_parts::kSlotLabelWidth;
using sound_section_parts::Label;
using sound_section_parts::LabelWidth;
using sound_section_parts::Rows;
using theme::Px;

// A sound has four bank slots and sixteen tracks. The tracks' switches come in two rows.
constexpr int kBankSlots = 4;
constexpr int kTracks = 16;
constexpr int kTracksInRow = 8;

// The middle of the priorities. A Command-click or a Ctrl-click on a slider sets it.
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
    "Sound player that plays this sound in the game. Each player can play only a limited number of sounds at once.";
const char* const kPriorityTip =
    "Priority within the player. When the player is full, a new sound stops the sound with the lowest priority, or "
    "doesn't start.";
const char* const kActorTip =
    "Actor player that plays this sound for a sound actor, 0–3. Each actor player can play only a limited number of "
    "sounds of the actor at once.";
const char* const kVolumeTip = "Volume, 0–255. 127 is full level.";
const char* const kDsVolumeTip = "Volume, 0–127. 127 is full level.";
const char* const kPanModeTip =
    "Dual pans each channel of a stereo wave separately. Balance turns down one channel of a stereo wave.";
const char* const kPanCurveTip =
    "How the pan splits the level between left and right. The plain curves play the centre 3 dB down, or 6 dB down "
    "for Linear. The 0 dB curves play the centre at full level. The clamped curves keep both sides at or below full "
    "level.";
const char* const kChannelPriorityTip =
    "Priority of the notes, added to each track priority. When the console runs out of voices, a new note takes the "
    "voice with the lowest priority.";
const char* const kReleaseTip =
    "On: a released note keeps its priority. Off: the priority of a released note falls to 1, and new notes take its "
    "voice first.";
const char* const kTracksTip =
    "Tracks that the sound allocates when it starts. Tracks that aren't allocated stay silent.";
const char* const kSlotTip =
    "Bank that the notes use when the sequence selects this slot. Slot 0 applies until the sequence selects another.";
const char* const kWiiSlotTip = "Bank that the notes use. Wii sounds use one bank.";
const char* const kDsSlotTip = "Bank that the notes use. DS sounds use one bank.";

// Why an archive of the Wii doesn't hold a value, after the value's tooltip.
const char* const kNoActorTip = " Hokuto doesn't read actor players in Wii archives.";
const char* const kNoPanModeTip = " Wii archives store the pan mode from version 1.2 on.";
const char* const kNoPanCurveTip = " Wii archives store the pan curve from version 1.2 on.";
const char* const kNoReleaseTip = " Wii archives store the release priority fix from version 1.3 on.";
const char* const kDsNoValueTip = " DS sounds don't have this value.";
const char* const kGbaPlayerTip =
    "Music player that plays the song, from the driver table. A song plays only as many tracks as its music player "
    "supports. A new song on a music player ends the previous song.";
const char* const kGbaPriorityTip =
    "Song priority, 0–255. A new song can take a music player only from a song of the same or lower priority. When "
    "notes compete for a channel, the priority is added to each track priority.";
const char* const kGbaReverbTip =
    "Source of the reverb. Driver keeps the reverb that the game or an earlier song set. Song sets the driver reverb "
    "when the song starts. The reverb then stays set for every later song.";
const char* const kGbaLevelTip = "Level of the driver reverb that the song sets, 0–127.";
const char* const kDsNoTracksTip = " DS sounds allocate their tracks in their commands.";

// The labels of the tags that the program offers, in the order of format::kTagKeys, and what each one holds.
constexpr std::array<const char*, format::kTagKeys.size()> kTagLabels = {
    "Title", "Artist", "Game", "Year", "Comment", "Copyright", "Length", "Fade",
};
constexpr std::array<const char*, format::kTagKeys.size()> kTagTips = {
    "Title of the piece or of the soundtrack.",
    "Composer or performer of the music.",
    "Game that the music comes from.",
    "Release year of the game.",
    "Note about the music.",
    "Copyright holder.",
    "Playing time before the fade, such as 2:30. PSF tags store the same value. Hokuto ignores it during playback.",
    "Fade duration, in seconds. PSF tags store the same value. Hokuto ignores it during playback.",
};

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

} // namespace

namespace sound_section_parts
{

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
        std::ranges::for_each(WaveRowLabels(), measure);
        scale = Px(1.0f);
        width = std::max(Px(kLabelWidth), std::ceil(widest + Px(12.0f)));
    }

    return width;
}

void Heading(Rows& rows, const char* text)
{
    const ImVec2 size = CapsSize(text);
    DrawCaps(rows.draw, ImVec2(rows.left, rows.y + Px(kHeadingHeight) - size.y - Px(6.0f)), theme::kTextMuted, text);
    rows.y += Px(kHeadingHeight);
}

Control Label(Rows& rows, const char* label, const char* tooltip, float label_width, bool grey)
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

} // namespace sound_section_parts

namespace
{

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
    const uint32_t highest = value == SoundValue::kVolume           ? section.view->held.highest_volume
                             : value == SoundValue::kPlayerPriority ? section.view->held.highest_priority
                                                                    : edit::HighestOf(value);
    const SliderRange range{0.0f, static_cast<float>(highest)};
    ImGui::SetCursorScreenPos(control.pos);
    ImGui::PushID(static_cast<int>(value));
    if (ValueSlider("##slider", &shown, range, reset, format, ImVec2(control.width, rows.control)))
    {
        const long rounded = std::lround(std::clamp(shown, range.low, range.high));
        section.held = SoundChange{value, 0, static_cast<uint32_t>(rounded)};
    }
    const bool released = SliderReleased();
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
    const std::string actor_tip = std::string(kActorTip) + (held ? "" : view.ds ? kDsNoValueTip : kNoActorTip);
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

// The rows of a GBA song: its music player, its priority and its reverb. Returns the change that the user made.
std::optional<SoundChange> DrawGbaSong(Rows& rows, SoundSection& section)
{
    const SoundView& view = *section.view;
    const format::NativeSound& values = view.values;
    std::optional<SoundChange> change;
    Heading(rows, "Music player");

    const Control player = Label(rows, "Player", kGbaPlayerTip);
    int chosen = IndexOf(values.player, ntdlib::sar::kPlayerItem, view.players.size());
    ImGui::SetCursorScreenPos(player.pos);
    ImGui::SetNextWindowSizeConstraints(ImVec2(Px(kListWidth), 0.0f), ImVec2(FLT_MAX, FLT_MAX));
    if (FilteredCombo("##player", view.players, chosen, nullptr, section.filter, player.width) && chosen >= 0)
    {
        change = SoundChange{SoundValue::kPlayer, 0,
                             ntdlib::sar::ItemId(ntdlib::sar::kPlayerItem, static_cast<uint32_t>(chosen))};
    }
    const Control priority = Label(rows, "Priority", kGbaPriorityTip);
    const std::optional<SoundChange> moved =
        Slider(rows, priority, SoundValue::kPlayerPriority, values.player_priority, 0.0f, "%.0f", section);

    // The level of the reverb means nothing while the song keeps the reverb of the driver.
    Heading(rows, "Reverb");
    const bool on = (values.reverb & edit::kGbaReverbOn) != 0;
    const uint32_t level = values.reverb & ~edit::kGbaReverbOn;
    const std::array<std::string, 2> kSources = {"Driver", "Song"};
    const Control source = Label(rows, "Reverb", kGbaReverbTip);
    const Control amount = Label(rows, "Level", kGbaLevelTip, LabelWidth(), !on);
    std::optional<SoundChange> switched = Switches(rows, source, SoundValue::kReverb, kSources, on ? 1 : 0);
    if (switched)
    {
        switched->to = level | (switched->to != 0 ? edit::kGbaReverbOn : 0);
    }
    ImGui::BeginDisabled(!on);
    std::optional<SoundChange> leveled = Slider(rows, amount, SoundValue::kReverb, level, 0.0f, "%.0f", section);
    ImGui::EndDisabled();
    if (leveled)
    {
        leveled->to |= edit::kGbaReverbOn;
    }

    return change ? change : moved ? moved : switched ? switched : leveled;
}

// The rows of the volume and the pan. Returns the change that the user made.
std::optional<SoundChange> DrawOutput(Rows& rows, SoundSection& section)
{
    const SoundView& view = *section.view;
    const format::NativeSound& values = view.values;
    Heading(rows, "Output");

    // The volume, with its level in dB.
    const Control volume = Label(rows, "Volume", view.ds ? kDsVolumeTip : kVolumeTip);
    const bool holding = section.held && section.held->value == SoundValue::kVolume;
    const std::string format = "%.0f  (" + VolumeLevel(holding ? section.held->to : values.volume) + ")";
    std::optional<SoundChange> change =
        Slider(rows, volume, SoundValue::kVolume, values.volume, kFullVolume, format.c_str(), section);

    // The pan's mode and curve, greyed where the archive holds neither. The curves' names take the rows' width.
    const bool held = view.held.pan;
    const std::string mode_tip = std::string(kPanModeTip) + (held ? "" : view.ds ? kDsNoValueTip : kNoPanModeTip);
    const std::string curve_tip = std::string(kPanCurveTip) + (held ? "" : view.ds ? kDsNoValueTip : kNoPanCurveTip);
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
    const std::string release_tip = std::string(kReleaseTip) + (held ? "" : view.ds ? kDsNoValueTip : kNoReleaseTip);
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
    const std::string tracks_tip = std::string(kTracksTip) + (view.held.tracks ? "" : kDsNoTracksTip);
    const Control tracks = LabelAbove(rows, "Tracks", tracks_tip.c_str(), kTracks / kTracksInRow, !view.held.tracks);
    ImGui::BeginDisabled(!view.held.tracks);
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
    ImGui::EndDisabled();

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
        const char* const tip = view.ds ? kDsSlotTip : slots == 1 ? kWiiSlotTip : kSlotTip;
        const Control control = Label(rows, label.c_str(), tip, Px(kSlotLabelWidth));
        const uint32_t bank = edit::ValueOf(view.values, SoundValue::kBank, slot);
        int chosen = IndexOf(bank, ntdlib::sar::kBankItem, view.banks.size());
        ImGui::SetCursorScreenPos(control.pos);
        ImGui::SetNextWindowSizeConstraints(ImVec2(Px(kListWidth), 0.0f), ImVec2(FLT_MAX, FLT_MAX));
        // A DS sound always plays a bank.
        if (FilteredCombo("##bank", view.banks, chosen, view.ds ? nullptr : "None", section.filter, control.width))
        {
            const uint32_t to = chosen < 0 ? ntdlib::sar::kNoItem
                                           : ntdlib::sar::ItemId(ntdlib::sar::kBankItem, static_cast<uint32_t>(chosen));
            change = SoundChange{SoundValue::kBank, slot, to};
        }
        ImGui::PopID();
    }

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
    // An archive without names names its players and banks after their numbers. The browser does the same.
    for (std::size_t i = 0; i < tables.players.size(); i++)
    {
        const std::string& name = tables.players[i].name;
        view.players.push_back(!name.empty() ? name : "PLAYER_" + std::to_string(i));
    }
    for (std::size_t i = 0; i < tables.banks.size(); i++)
    {
        view.banks.push_back(!tables.banks[i].empty() ? tables.banks[i] : "BANK_" + std::to_string(i));
    }
    view.held = edit::HeldValuesOf(file);
    if (const auto tags = file.sound_tags.find(sound); tags != file.sound_tags.end())
    {
        view.tags = tags->second;
    }

    return view;
}

// The view of sound `sound` of `archive`, the archive that `file` builds. Nothing for a sound that is missing from the
// archive or can't be read.
std::optional<SoundView> MakeGbaSoundView(const format::File& file, const song::GbaRom& rom, uint32_t sound)
{
    SoundView view;
    try
    {
        view.values = edit::GbaNativeSoundOf(rom, sound);
    }
    catch (const std::exception&)
    {
        return std::nullopt;
    }

    view.sound = sound;
    view.players = edit::GbaPlayerNames(rom);
    view.held = edit::GbaHeldValues();
    view.gba = true;
    if (const auto tags = file.sound_tags.find(sound); tags != file.sound_tags.end())
    {
        view.tags = tags->second;
    }

    return view;
}

std::optional<SoundView> MakeDsSoundView(const format::File& file, const song::DsArchive& archive, uint32_t sound)
{
    SoundView view;
    try
    {
        view.values = edit::DsNativeSoundOf(archive, sound);
    }
    catch (const std::exception&)
    {
        return std::nullopt;
    }

    view.sound = sound;
    view.players = edit::DsPlayerNames(archive);
    for (uint32_t bank = 0; bank < archive.archive.banks.size(); bank++)
    {
        view.banks.push_back(song::DsBankName(archive, bank));
    }
    view.held = edit::DsHeldValues();
    view.ds = true;
    if (const auto tags = file.sound_tags.find(sound); tags != file.sound_tags.end())
    {
        view.tags = tags->second;
    }

    return view;
}

} // namespace

void ShowSound(SoundSection& section, std::shared_ptr<const format::File> version, std::optional<uint32_t> sound,
               const song::DsArchive* ds, const song::GbaRom* gba)
{
    if (version == section.version && sound == section.sound)
    {
        return;
    }

    if (sound != section.sound)
    {
        section.typing = TextTyping();
    }
    if (version && sound)
    {
        section.view = gba != nullptr  ? MakeGbaSoundView(*version, *gba, *sound)
                       : ds != nullptr ? MakeDsSoundView(*version, *ds, *sound)
                                       : MakeSoundView(*version, *sound);
    }
    else
    {
        section.view.reset();
    }
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
    PushSliderItem(ShownItem{.version = section.version, .shown = section.view->sound});
    std::optional<SoundChange> change = section.view->gba ? DrawGbaSong(rows, section) : DrawPlayer(rows, section);
    const std::optional<SoundChange> output = section.view->gba ? std::nullopt : DrawOutput(rows, section);
    change = change ? change : output;
    if (!section.view->gba && section.view->values.kind == ntdlib::sar::SoundType::kSequence)
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
    PopSliderItem();
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

    case SoundValue::kReverb:
        return (value_of & edit::kGbaReverbOn) != 0 ? "level " + std::to_string(value_of & ~edit::kGbaReverbOn)
                                                    : "driver reverb";

    case SoundValue::kPlayerPriority:
    case SoundValue::kActorPlayer:
    case SoundValue::kChannelPriority:
        break;
    }

    return std::to_string(value_of);
}

} // namespace hokuto::app
