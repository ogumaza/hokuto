// SPDX-License-Identifier: MIT

// The rows of a wave sound's values in its wave sound file, in the Sound section.

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "ntdlib/rsar/formats.h"
#include "ntdlib/sar/formats.h"

#include "app/key_edits.h"
#include "app/sound_section.h"
#include "app/sound_section_parts.h"
#include "app/theme.h"
#include "app/widgets.h"

namespace hokuto::app
{
namespace
{

using sound_section_parts::Control;
using sound_section_parts::Heading;
using sound_section_parts::kListWidth;
using sound_section_parts::kRowGap;
using sound_section_parts::Label;
using sound_section_parts::LabelWidth;
using sound_section_parts::Rows;
using theme::Px;

// Why a wave sound's value shows grey, after the value's tooltip.
const char* const kNoHoldTip = " The NW4R SND build of this file ignores hold. Builds from March 2008 on support it.";
const char* const kCtrNoFiltersTip = " The 3DS ignores the filters in wave sound files.";
const char* const kWiiNoFiltersTip = " Wii wave sound files don't contain filters.";
const char* const kOldFiltersTip =
    " This version of the wave sound file doesn't store filters. The Wii U and the Switch read filters from version "
    "1.1.0 on.";
const char* const kCtrNoAuxCTip = " The 3DS has no aux bus C. 3DS sounds send only to aux buses A and B.";
const char* const kNoPlaceTip = " This wave sound file has no field for this value.";

// What a wave sound's values in its wave sound file do, for the tooltips of their labels.
const char* const kWaveTip = "Wave that the sound plays.";
const char* const kRootKeyTip =
    "Root key of the note in the wave sound file. The Switch and the Wii U ignore it. They play the wave at its sample "
    "rate multiplied by Pitch.";
const char* const kPitchTip =
    "Playback rate as a multiple of the wave sample rate. 1 plays the wave unchanged, and 2 plays it an octave higher.";
const char* const kSurroundPanTip =
    "Surround pan, 0–127. Places the sound between the front and the back of a surround mix.";
const char* const kMainSendTip = "Send level to the main output, 0–127. 127 is full level.";
const char* const kAuxSendTips[] = {
    "Send level to aux bus A, 0–127. FX channel A in the mixer contains the effects of the bus.",
    "Send level to aux bus B, 0–127. FX channel B in the mixer contains the effects of the bus.",
    "Send level to aux bus C, 0–127. FX channel C in the mixer contains the effects of the bus.",
};
const char* const kLowPassTip = "Low-pass filter, 0–64. 64 leaves the filter open. Lower values close it further.";
const char* const kBiquadTypeTip =
    "Biquad filter type: 0 is off, 1 is low-pass, 2 is high-pass, and 3 to 5 are band-pass. On the Switch, 6 to 10 "
    "are the same filters designed for 48 kHz.";
const char* const kBiquadValueTip = "Strength of the biquad filter, 0–127. 0 has no effect.";
const char* const kWaveAttackTip =
    "Attack rate, 0–127. Higher values rise faster. At 127 the sound starts at full level.";
const char* const kWaveHoldTip = "Hold time at full level after the attack, 0–127.";
const char* const kWaveDecayTip = "Decay rate, 0–127. Higher values decay faster.";
const char* const kWaveSustainTip = "Sustain level until the game stops the sound, 0–127. 127 is full level.";
const char* const kWaveReleaseTip =
    "Release rate, 0–127. Higher values fade out faster after the game stops the sound.";

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
    {WaveField::kPan, "Pan", "Pan", kPanTip, kByteRange, 64.0f, "%.0f"},
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
    const bool released = SliderReleased();
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
        return view.engine == format::Engine::k3ds   ? kCtrNoFiltersTip
               : view.engine == format::Engine::kWii ? kWiiNoFiltersTip
                                                     : kOldFiltersTip;
    }
    if (field == WaveField::kAuxSendC && view.engine == format::Engine::k3ds)
    {
        return kCtrNoAuxCTip;
    }

    return kNoPlaceTip;
}

// The row of the wave, from a menu of the archive's waves. Returns the change that the user made.
std::optional<WaveSoundChange> DrawWaveMenu(Rows& rows, SoundSection& section, const WaveSoundView& view,
                                            const std::string& where)
{
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
        return WaveSoundChange{view.fields.values, view.waves[static_cast<std::size_t>(chosen)],
                               "Change Wave Sound Wave",
                               where + "from " + from + " to " + view.wave_names[static_cast<std::size_t>(chosen)]};
    }

    return std::nullopt;
}

// The row of the root key. The consoles don't read it.
void DrawRootKey(Rows& rows, const WaveSoundView& view)
{
    const Control root = Label(rows, "Root key", kRootKeyTip, LabelWidth(), true);
    const int key = view.fields.values.notes.front().original_key;
    const std::string key_text = std::to_string(key) + "  (" + KeyName(key) + ")";
    ImFont* font = theme::GetFonts().text;
    const ImVec2 key_size = TextSize(font, theme::kTextSize, key_text);
    DrawText(rows.draw, font, theme::kTextSize,
             ImVec2(root.pos.x + Px(8.0f), root.pos.y + std::floor((rows.control - key_size.y) * 0.5f)),
             theme::kTextDim, key_text);
}

// The change of the value of `row` from `current` to `moved`, or nothing where the value stays.
std::optional<WaveSoundChange> WaveRowChange(const WaveSoundView& view, const WaveRow& row, float current, float moved,
                                             const std::string& where)
{
    ntdlib::sar::WaveSoundFile::WaveSound values = view.fields.values;
    if (uint8_t* byte = WaveByteOf(values, row.field))
    {
        *byte = static_cast<uint8_t>(std::lround(std::clamp(moved, row.range.low, row.range.high)));
    }
    else
    {
        values.pitch = std::clamp(moved, kTypedPitch.low, kTypedPitch.high);
    }
    const float to = WaveValueOf(values, row.field);
    if (to != current)
    {
        return WaveSoundChange{values, view.fields.wave, std::string("Change Wave Sound ") + row.name,
                               where + "from " + WaveValueText(row, current) + " to " + WaveValueText(row, to)};
    }

    return std::nullopt;
}

} // namespace

namespace sound_section_parts
{

std::vector<const char*> WaveRowLabels()
{
    std::vector<const char*> labels;
    for (const WaveRow& row : kWaveRows)
    {
        labels.push_back(row.label);
    }

    return labels;
}

std::optional<WaveSoundChange> DrawWaveSound(Rows& rows, SoundSection& section)
{
    const WaveSoundView& view = *section.wave_sound;
    const std::string where = "Sound: " + section.view->values.name + " - ";
    std::optional<WaveSoundChange> change;
    Heading(rows, "Wave sound");
    ImGui::BeginDisabled(section.wave_busy);

    // The wave, from a menu of the archive's waves, and the root key that the consoles don't read.
    change = DrawWaveMenu(rows, section, view, where);
    DrawRootKey(rows, view);

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

        if (std::optional<WaveSoundChange> moved_to = WaveRowChange(view, row, current, *moved, where))
        {
            change = std::move(moved_to);
        }
    }
    ImGui::EndDisabled();

    // What else an edit changes, and the sizes, in lines that start at the labels and wrap at the section's edge.
    ImGui::SetCursorScreenPos(ImVec2(rows.left, rows.y + Px(kRowGap)));
    ImGui::BeginGroup();
    ImGui::PushTextWrapPos(rows.left + rows.width - ImGui::GetWindowPos().x + ImGui::GetScrollX());
    MutedText(view.reach.c_str());
    for (const std::string& line : view.sizes)
    {
        MutedText(line.c_str());
    }
    ImGui::PopTextWrapPos();
    ImGui::EndGroup();
    rows.y = ImGui::GetItemRectMax().y + Px(kRowGap);

    return change;
}

} // namespace sound_section_parts

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

} // namespace hokuto::app
