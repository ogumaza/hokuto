// SPDX-License-Identifier: MIT

#include "app/effect_editor.h"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <optional>
#include <span>
#include <stdexcept>
#include <tuple>

#include "ntdlib/cafe/snd/effect_setup.h"
#include "ntdlib/nx/audio/i3dl2.h"
#include "ntdlib/rvl/snd/effect_setup.h"

#include "app/theme.h"
#include "app/widgets.h"
#include "format/archive.h"
#include "format/codes.h"
#include "song/sequence_model.h"

namespace hokuto::app
{
namespace
{

using edit::CtrStartingParameters;
using edit::WiiTypeOf;
using edit::WiiUTypeOf;
using ntdlib::cafe::snd::EffectType;
using ntdlib::nx::audio::kI3dl2ReverbPresets;
using theme::Px;

static_assert(kFxChannels == format::kAuxBuses && std::tuple_size_v<song::AuxEffects> == format::kAuxBuses);

// The kinds that each console's inserts offer. The Switch has nn::audio's I3DL2 reverb, and the Wii U the effects of
// nw::snd's four classes. The Wii has the effects of the SDK's AXFX library. NW4R SND's four classes set them up.
// Before 2008, the reverbs take the older parameters. From 2008, they are AXFX's Exp reverbs, with the parameters of
// the Wii U's reverbs. The 3DS has the reverbs and delays of nw::snd and nn::snd. Games of 2011 link nn::snd's older
// reverb. edit::EffectName gives each kind's name.
using Kind = format::EffectKind;
constexpr std::array<Kind, 1> kSwitchKinds = {Kind::kI3dl2Reverb};
constexpr std::array<Kind, 4> kWiiUKinds = {Kind::kReverbHi, Kind::kReverbStd, Kind::kChorus, Kind::kDelay};
constexpr std::array<Kind, 4> kWiiKinds = {Kind::kWiiReverbHi, Kind::kWiiReverbStd, Kind::kWiiChorus, Kind::kWiiDelay};
constexpr std::array<Kind, 4> kWii2008Kinds = {Kind::kWiiReverbHi2008, Kind::kWiiReverbStd2008, Kind::kWiiChorus,
                                               Kind::kWiiDelay};
constexpr std::array<Kind, 5> kCtrKinds = {Kind::kCtrReverb, Kind::kCtrDelay, Kind::kCtrNnReverb, Kind::kCtrNnDelay,
                                           Kind::kCtrNnReverb2011};

// The kinds that the inserts of `file` offer for a sound on `engine`. The Wii's kinds follow the build of NW4R SND that
// a file of the Wii keeps. A sound of another console plays on the Wii's engine as edit::ForWii converts it, for the
// build of June 2008.
std::span<const Kind> KindsOf(const format::File& file, format::Engine engine)
{
    switch (engine)
    {
    case format::Engine::kWiiU:
        return kWiiUKinds;
    case format::Engine::k3ds:
        return kCtrKinds;
    case format::Engine::kWii:
        return file.native && (!file.native->wii || format::BuildOfWiiArchive(*file.native).SinceMarch2008())
                   ? std::span<const Kind>(kWii2008Kinds)
                   : std::span<const Kind>(kWiiKinds);
    case format::Engine::kSwitch:
        break;
    }

    return kSwitchKinds;
}

// A parameter's row in the window: its label, its range and the format of its value in the units shown, the factor from
// the file's units to those, and the steps in each of the file's units that an edit rounds to. A mode's row has the
// names of its choices instead, in a menu. With `decibels`, the file holds a gain as a factor, and the row shows it in
// dB. Its steps are then steps in each dB.
struct Row
{
    const char* label;
    SliderRange range;
    const char* format;
    double scale;
    double steps;
    std::span<const char* const> choices = {};
    bool decibels = false;
};

// The reverb's rows, in the order that a file keeps the parameters in. The ranges are those of the I3DL2 guidelines.
// The dry gain isn't I3DL2's. nn::audio adds it: a factor from 0 to 1. The row shows it in dB, from -100 dB to 0 dB.
// The other gains' rows use dB too. The gains move in steps of a millibel, and the delays in steps of 0.1 ms.
constexpr std::array<Row, song::kI3dl2Parameters> kI3dl2Rows = {{
    {"Room", {-100.0f, 0.0f}, "%.2f dB", 0.01, 1.0},
    {"Room HF", {-100.0f, 0.0f}, "%.2f dB", 0.01, 1.0},
    {"Decay time", {0.1f, 20.0f, true}, "%.2f s", 1.0, 100.0},
    {"Decay HF ratio", {0.1f, 2.0f}, "%.2f", 1.0, 100.0},
    {"Reflections", {-100.0f, 10.0f}, "%.2f dB", 0.01, 1.0},
    {"Reflections delay", {0.0f, 300.0f}, "%.1f ms", 1000.0, 10000.0},
    {"Reverb", {-100.0f, 20.0f}, "%.2f dB", 0.01, 1.0},
    {"Reverb delay", {0.0f, 100.0f}, "%.1f ms", 1000.0, 10000.0},
    {"Diffusion", {0.0f, 100.0f}, "%.0f %%", 1.0, 1.0},
    {"Density", {0.0f, 100.0f}, "%.0f %%", 1.0, 1.0},
    {"HF reference", {20.0f, 20000.0f, true}, "%.0f Hz", 1.0, 1.0},
    {"Dry", {-100.0f, 0.0f}, "%.2f dB", 1.0, 100.0, {}, true},
}};

// The modes of the Wii U's reverbs, by the system library's numbers: the delay of the first early reflection, and the
// room that the fused reverb's lines are sized for.
constexpr std::array<const char*, 8> kEarlyModes = {"5 ms",  "10 ms", "15 ms", "20 ms",
                                                    "25 ms", "30 ms", "35 ms", "40 ms"};
constexpr std::array<const char*, 6> kFusedModes = {"Old AXFX",   "Metal Tank", "Small Room",
                                                    "Large Room", "Hall",       "Cavernous"};

// The rows of the Wii U's effects, in the order of their parameter structures, with the parameters' names in the system
// library. The library holds each gain and factor between 0 and 1, the chorus's delay between 0.1 ms and 50 ms and its
// rate between 0.1 Hz and 2 Hz, and each feedback below 1. The reverbs' times move in steps of 0.1 ms, the chorus's and
// the delay's in steps of 0.01 ms, and the gains and factors in steps of 0.01.
constexpr std::array<Row, 11> kReverbHiRows = {{
    {"Pre-delay", {0.0f, 500.0f}, "%.1f ms", 1000.0, 10000.0},
    {"Fused time", {0.0f, 20.0f}, "%.2f s", 1.0, 10000.0},
    {"Coloration", {0.0f, 1.0f}, "%.2f", 1.0, 100.0},
    {"Damping", {0.0f, 1.0f}, "%.2f", 1.0, 100.0},
    {"Crosstalk", {0.0f, 1.0f}, "%.2f", 1.0, 100.0},
    {"Out gain", {0.0f, 1.0f}, "%.2f", 1.0, 100.0},
    {"Early mode", {}, "", 1.0, 1.0, kEarlyModes},
    {"Pre-delay max", {0.0f, 500.0f}, "%.1f ms", 1000.0, 10000.0},
    {"Fused mode", {}, "", 1.0, 1.0, kFusedModes},
    {"Early gain", {0.0f, 1.0f}, "%.2f", 1.0, 100.0},
    {"Fused gain", {0.0f, 1.0f}, "%.2f", 1.0, 100.0},
}};
constexpr std::array<Row, 10> kReverbStdRows = {{
    {"Pre-delay", {0.0f, 500.0f}, "%.1f ms", 1000.0, 10000.0},
    {"Fused time", {0.0f, 20.0f}, "%.2f s", 1.0, 10000.0},
    {"Coloration", {0.0f, 1.0f}, "%.2f", 1.0, 100.0},
    {"Damping", {0.0f, 1.0f}, "%.2f", 1.0, 100.0},
    {"Out gain", {0.0f, 1.0f}, "%.2f", 1.0, 100.0},
    {"Early mode", {}, "", 1.0, 1.0, kEarlyModes},
    {"Pre-delay max", {0.0f, 500.0f}, "%.1f ms", 1000.0, 10000.0},
    {"Fused mode", {}, "", 1.0, 1.0, kFusedModes},
    {"Early gain", {0.0f, 1.0f}, "%.2f", 1.0, 100.0},
    {"Fused gain", {0.0f, 1.0f}, "%.2f", 1.0, 100.0},
}};
constexpr std::array<Row, 5> kChorusRows = {{
    {"Delay time", {0.1f, 50.0f}, "%.2f ms", 1.0, 100.0},
    {"Depth", {0.0f, 1.0f}, "%.2f", 1.0, 100.0},
    {"Rate", {0.1f, 2.0f}, "%.2f Hz", 1.0, 100.0},
    {"Feedback", {0.0f, 0.99f}, "%.2f", 1.0, 100.0},
    {"Out gain", {0.0f, 1.0f}, "%.2f", 1.0, 100.0},
}};
constexpr std::array<Row, 5> kDelayRows = {{
    {"Delay", {0.04f, 5000.0f, true}, "%.2f ms", 1.0, 100.0},
    {"Feedback", {0.0f, 0.99f}, "%.2f", 1.0, 100.0},
    {"Out gain", {0.0f, 1.0f}, "%.2f", 1.0, 100.0},
    {"Max delay", {0.04f, 5000.0f, true}, "%.2f ms", 1.0, 100.0},
    {"LPF", {0.0f, 1.0f}, "%.2f", 1.0, 100.0},
}};

// The rows of the Wii's effects, in the order of their parameter structures, with the names of NW4R SND's parameters.
// The reverbs of 2008 have the Wii U's rows. FxReverbHi of the builds before 2008, and AXFX's standard reverb, hold the
// pre-delay between 0 and 100 ms and the fused time between 0 and 10 s. FxChorus and FxDelay take their times in whole
// milliseconds: the chorus's delay from 1 ms to 50 ms, its depth times the delay up to 50 ms, and its rate from 0.1 Hz
// to 2 Hz. The delay runs from 1 ms to 5,000 ms, and its feedback and gain in hundredths, the feedback up to 0.99.
constexpr std::array<Row, 6> kWiiReverbHiRows = {{
    {"Pre-delay", {0.0f, 100.0f}, "%.1f ms", 1000.0, 10000.0},
    {"Fused time", {0.0f, 10.0f}, "%.2f s", 1.0, 10000.0},
    {"Coloration", {0.0f, 1.0f}, "%.2f", 1.0, 100.0},
    {"Damping", {0.0f, 1.0f}, "%.2f", 1.0, 100.0},
    {"Crosstalk", {0.0f, 1.0f}, "%.2f", 1.0, 100.0},
    {"Out gain", {0.0f, 1.0f}, "%.2f", 1.0, 100.0},
}};
constexpr std::array<Row, 5> kWiiReverbStdRows = {{
    {"Pre-delay", {0.0f, 100.0f}, "%.1f ms", 1000.0, 10000.0},
    {"Fused time", {0.0f, 10.0f}, "%.2f s", 1.0, 10000.0},
    {"Coloration", {0.0f, 1.0f}, "%.2f", 1.0, 100.0},
    {"Damping", {0.0f, 1.0f}, "%.2f", 1.0, 100.0},
    {"Out gain", {0.0f, 1.0f}, "%.2f", 1.0, 100.0},
}};
constexpr std::array<Row, 3> kWiiChorusRows = {{
    {"Delay time", {1.0f, 50.0f}, "%.0f ms", 1.0, 1.0},
    {"Depth", {0.0f, 1.0f}, "%.2f", 1.0, 100.0},
    {"Rate", {0.1f, 2.0f}, "%.2f Hz", 1.0, 100.0},
}};
constexpr std::array<Row, 3> kWiiDelayRows = {{
    {"Delay", {1.0f, 5000.0f, true}, "%.0f ms", 1.0, 1.0},
    {"Feedback", {0.0f, 0.99f}, "%.2f", 1.0, 100.0},
    {"Out gain", {0.0f, 1.0f}, "%.2f", 1.0, 100.0},
}};

// The rows of the 3DS's effects, in the order of their classes' Param, with a reverb's filter sizes in place of the
// pointer to them. The ranges are the format's limits (format::LimitsOf). The times move in steps of a millisecond, and
// the gains and factors in steps of 0.01. A filter's size moves in the libraries' frames of 160 samples, and shows as
// its time at the libraries' 32,728 Hz. A frame lasts as long in enhanced mixing.
constexpr double kMsPerSample = 1000.0 / 32728.0;
constexpr SliderRange kFilterRange = {static_cast<float>(160 * kMsPerSample), static_cast<float>(32000 * kMsPerSample)};
constexpr std::array<const char*, 2> kSwitchChoices = {"Off", "On"};
constexpr Row kEarlyReflectionRow = {"Early reflection", {0.0f, 1000.0f}, "%.0f ms", 1.0, 1.0};
constexpr Row kFusedTimeRow = {"Fused time", {0.0f, 20000.0f}, "%.0f ms", 1.0, 1.0};
constexpr Row kPreDelayRow = {"Pre-delay", {0.0f, 1000.0f}, "%.0f ms", 1.0, 1.0};
constexpr Row kColorationRow = {"Coloration", {0.0f, 1.0f}, "%.2f", 1.0, 100.0};
constexpr Row kDampingRow = {"Damping", {0.0f, 1.0f}, "%.2f", 1.0, 100.0};
constexpr Row kFirstCombRow = {"Comb filter 1", kFilterRange, "%.1f ms", kMsPerSample, 1.0 / 160.0};
constexpr Row kSecondCombRow = {"Comb filter 2", kFilterRange, "%.1f ms", kMsPerSample, 1.0 / 160.0};
constexpr Row kAllPassRow = {"All-pass filter", kFilterRange, "%.1f ms", kMsPerSample, 1.0 / 160.0};
constexpr Row kEarlyGainRow = {"Early gain", {0.0f, 1.0f}, "%.2f", 1.0, 100.0};
constexpr Row kFusedGainRow = {"Fused gain", {0.0f, 1.0f}, "%.2f", 1.0, 100.0};
constexpr Row kSurroundRow = {"Surround", {}, "", 1.0, 1.0, kSwitchChoices};
constexpr Row kHighPassRow = {"High-pass damping", {}, "", 1.0, 1.0, kSwitchChoices};
constexpr std::array<Row, 12> kCtrReverbRows = {{
    kEarlyReflectionRow,
    kFusedTimeRow,
    kPreDelayRow,
    kColorationRow,
    kDampingRow,
    kFirstCombRow,
    kSecondCombRow,
    kAllPassRow,
    kEarlyGainRow,
    kFusedGainRow,
    kSurroundRow,
    kHighPassRow,
}};
constexpr std::array<Row, 11> kCtrNnReverbRows = {{
    kEarlyReflectionRow,
    kFusedTimeRow,
    kPreDelayRow,
    kColorationRow,
    kDampingRow,
    kFirstCombRow,
    kSecondCombRow,
    kAllPassRow,
    kEarlyGainRow,
    kFusedGainRow,
    kHighPassRow,
}};
constexpr std::array<Row, 10> kCtrNnReverb2011Rows = {{
    kEarlyReflectionRow,
    kFusedTimeRow,
    kPreDelayRow,
    kColorationRow,
    kDampingRow,
    kFirstCombRow,
    kSecondCombRow,
    kAllPassRow,
    kEarlyGainRow,
    kFusedGainRow,
}};
constexpr std::array<Row, 4> kCtrDelayRows = {{
    {"Delay", {1.0f, 5000.0f, true}, "%.0f ms", 1.0, 1.0},
    {"Feedback", {0.0f, 1.0f}, "%.2f", 1.0, 100.0},
    kDampingRow,
    kSurroundRow,
}};

// A delay of one of the effects, and the longest delay that its line holds, by their places in the kind's parameters.
// The system library holds a delay at its maximum.
struct DelayLimit
{
    format::EffectKind kind;
    std::size_t delay;
    std::size_t maximum;
};

constexpr std::array<DelayLimit, 5> kDelayLimits = {{
    {format::EffectKind::kReverbHi, 0, 7},
    {format::EffectKind::kReverbStd, 0, 6},
    {format::EffectKind::kDelay, 0, 3},
    {format::EffectKind::kWiiReverbHi2008, 0, 7},
    {format::EffectKind::kWiiReverbStd2008, 0, 6},
}};

// Keeps each delay of `effect` within its maximum after an edit of parameter `edited`. A longer delay lengthens its
// maximum, and a shorter maximum shortens its delay. The window then shows the delay that plays.
void KeepDelaysWithinMaxima(format::Effect& effect, std::size_t edited)
{
    for (const DelayLimit& limit : kDelayLimits)
    {
        if (limit.kind != effect.kind || std::max(limit.delay, limit.maximum) >= effect.parameters.size())
        {
            continue;
        }

        float& delay = effect.parameters[limit.delay];
        float& maximum = effect.parameters[limit.maximum];
        if (edited == limit.delay && delay > maximum)
        {
            maximum = delay;
        }
        else if (edited == limit.maximum && maximum < delay)
        {
            delay = maximum;
        }
    }
}

// The rows of a kind's parameters, in a file's order.
std::span<const Row> RowsOf(format::EffectKind kind)
{
    switch (kind)
    {
    case format::EffectKind::kI3dl2Reverb:
        return kI3dl2Rows;
    case format::EffectKind::kReverbHi:
        return kReverbHiRows;
    case format::EffectKind::kReverbStd:
        return kReverbStdRows;
    case format::EffectKind::kChorus:
        return kChorusRows;
    case format::EffectKind::kDelay:
        return kDelayRows;
    case format::EffectKind::kWiiReverbHi:
        return kWiiReverbHiRows;
    case format::EffectKind::kWiiReverbHi2008:
        return kReverbHiRows;
    case format::EffectKind::kWiiChorus:
        return kWiiChorusRows;
    case format::EffectKind::kWiiDelay:
        return kWiiDelayRows;
    case format::EffectKind::kWiiReverbStd:
        return kWiiReverbStdRows;
    case format::EffectKind::kWiiReverbStd2008:
        return kReverbStdRows;
    case format::EffectKind::kCtrReverb:
        return kCtrReverbRows;
    case format::EffectKind::kCtrNnReverb:
        return kCtrNnReverbRows;
    case format::EffectKind::kCtrNnReverb2011:
        return kCtrNnReverb2011Rows;
    case format::EffectKind::kCtrDelay:
    case format::EffectKind::kCtrNnDelay:
        return kCtrDelayRows;
    }

    return {};
}

// The parameters of nn::audio's preset `preset`, in a file's order.
std::vector<float> PresetParameters(std::size_t preset)
{
    const std::array<float, song::kI3dl2Parameters> parameters = song::I3dl2Parameters(kI3dl2ReverbPresets[preset]);
    return {parameters.begin(), parameters.end()};
}

// A preset's name: nn::audio's number for it, and its decay time.
std::string PresetName(std::size_t preset)
{
    std::array<char, 16> decay{};
    std::snprintf(decay.data(), decay.size(), "%.1f",
                  static_cast<double>(kI3dl2ReverbPresets[preset].late_reverb_decay_time));

    return "Preset " + std::to_string(preset) + " (" + decay.data() + " s decay)";
}

// The presets' menu. It shows the preset that the parameters match, or Custom. A choice sets every parameter. Returns
// true when the user chose a preset.
bool DrawPresets(format::Effect& effect)
{
    std::size_t matched = kI3dl2ReverbPresets.size();
    for (std::size_t i = 0; i < kI3dl2ReverbPresets.size(); i++)
    {
        if (effect.parameters == PresetParameters(i))
        {
            matched = i;
        }
    }
    const std::string current = matched < kI3dl2ReverbPresets.size() ? PresetName(matched) : "Custom";
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (!ImGui::BeginCombo("##preset", current.c_str()))
    {
        return false;
    }

    bool chosen = false;
    for (std::size_t i = 0; i < kI3dl2ReverbPresets.size(); i++)
    {
        if (ImGui::Selectable(PresetName(i).c_str(), i == matched))
        {
            effect.parameters = PresetParameters(i);
            chosen = true;
        }
    }
    ImGui::EndCombo();

    return chosen;
}

// The parameters that a new effect of `kind` starts from, and that a reset of a parameter returns to: nn::audio's first
// preset for the I3DL2 reverb, Hokuto's starting values for the effects of the Wii U and the Wii, and the libraries'
// defaults for the 3DS's effects.
std::vector<float> StartingParameters(format::EffectKind kind)
{
    if (const std::optional<EffectType> type = WiiUTypeOf(kind))
    {
        return song::WiiUStartingParameters(*type);
    }
    if (const std::optional<ntdlib::rvl::snd::EffectType> type = WiiTypeOf(kind))
    {
        return song::WiiStartingParameters(*type);
    }
    if (format::IsCtrKind(kind))
    {
        return CtrStartingParameters(kind);
    }

    return PresetParameters(0);
}

// A mode's menu. A choice sets the parameter to the choice's number. Returns true when the user chose another one.
bool DrawChoices(const Row& row, float& parameter)
{
    const float last = static_cast<float>(row.choices.size() - 1);
    const auto current = static_cast<std::size_t>(std::clamp(parameter, 0.0f, last));
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (!ImGui::BeginCombo("##choice", row.choices[current]))
    {
        return false;
    }

    bool chosen = false;
    for (std::size_t i = 0; i < row.choices.size(); i++)
    {
        if (ImGui::Selectable(row.choices[i], i == current) && i != current)
        {
            parameter = static_cast<float>(i);
            chosen = true;
        }
    }
    ImGui::EndCombo();

    return chosen;
}

// Draws the sliders and menus of the parameters. An edit of a slider rounds the value to its row's steps, in the file's
// units, and keeps it within the format's limits and each delay within its maximum. Returns true when an edit ended.
// `*changing` says whether a drag or a typed value is under way.
bool DrawParameters(format::Effect& effect, bool* changing)
{
    const std::span<const Row> rows = RowsOf(effect.kind);
    const std::vector<float> defaults = StartingParameters(effect.kind);
    const std::size_t count = std::min({rows.size(), effect.parameters.size(), defaults.size()});
    bool edited = false;
    *changing = false;
    for (std::size_t i = 0; i < count; i++)
    {
        const Row& row = rows[i];
        SettingLabel(row.label);
        ImGui::PushID(static_cast<int>(i));
        if (!row.choices.empty())
        {
            edited = DrawChoices(row, effect.parameters[i]) || edited;
            ImGui::PopID();
            continue;
        }

        // A factor of 0 shows as -inf dB.
        const auto in_row = [&row](float parameter)
        {
            return static_cast<float>(row.decibels ? 20.0 * std::log10(static_cast<double>(parameter))
                                                   : parameter * row.scale);
        };
        float shown = in_row(effect.parameters[i]);
        const float reset = in_row(defaults[i]);
        const ImVec2 size(ImGui::GetContentRegionAvail().x, ImGui::GetFrameHeight());
        if (ValueSlider("##value", &shown, row.range, reset, row.format, size))
        {
            // Adding 0 turns -0 into 0.
            float& parameter = effect.parameters[i];
            const double rounded = std::round(shown / row.scale * row.steps) / row.steps;
            parameter = static_cast<float>(row.decibels ? std::pow(10.0, rounded / 20.0) : rounded + 0.0);
            if (const std::optional<format::ParameterLimits> limits = format::LimitsOf(effect.kind, i))
            {
                parameter = std::clamp(parameter, limits->low, limits->high);
            }
            KeepDelaysWithinMaxima(effect, i);
        }
        edited = edited || ImGui::IsItemDeactivatedAfterEdit();
        *changing = *changing || ImGui::IsItemActive();
        ImGui::PopID();
    }

    return edited;
}

} // namespace

std::size_t FxChannelsOf(format::Engine engine)
{
    return engine == format::Engine::k3ds ? 2 : kFxChannels;
}

std::vector<std::string> EffectKindNames(const format::File& file, format::Engine engine)
{
    std::vector<std::string> names;
    for (const Kind kind : KindsOf(file, engine))
    {
        names.emplace_back(edit::EffectName(kind));
    }

    return names;
}

format::Effect NewEffect(const format::File& file, format::Engine engine, std::size_t kind)
{
    const std::span<const Kind> kinds = KindsOf(file, engine);
    if (kinds.empty())
    {
        throw std::invalid_argument("Hokuto has no effects of the engine's console.");
    }

    format::Effect effect;
    effect.kind = kinds[std::min(kind, kinds.size() - 1)];
    effect.parameters = StartingParameters(effect.kind);

    return effect;
}

const char* EffectName(const format::Effect& effect)
{
    return edit::EffectName(effect.kind);
}

std::string EffectTitle(const EffectPlace& place, const format::Effect& effect)
{
    return std::string("FX ") + static_cast<char>('A' + place.channel) + ": Ins. " + std::to_string(place.index + 1) +
           " - " + EffectName(effect);
}

ActionNames EffectChangeNames(const EffectPlace& place, EffectEdit edit, const format::Effect& before,
                              const format::Effect& after)
{
    const std::string title = EffectTitle(place, after);
    if (edit == EffectEdit::kPreset && after.kind == format::EffectKind::kI3dl2Reverb)
    {
        for (std::size_t i = 0; i < kI3dl2ReverbPresets.size(); i++)
        {
            if (after.parameters == PresetParameters(i))
            {
                return {"Load Preset", title + " - " + PresetName(i)};
            }
        }
    }

    // The parameters that differ. One that changed alone gets its name.
    const std::span<const Row> rows = RowsOf(after.kind);
    std::size_t changed = 0;
    std::size_t last = 0;
    for (std::size_t i = 0; i < std::min({before.parameters.size(), after.parameters.size(), rows.size()}); i++)
    {
        if (before.parameters[i] != after.parameters[i])
        {
            changed++;
            last = i;
        }
    }
    if (changed == 1)
    {
        return {"Change Parameter", title + " - " + rows[last].label};
    }

    return {"Change Parameters", title};
}

EffectEdit DrawEffectWindow(const EffectPlace& place, format::Effect& effect, bool* open)
{
    // A window over the middle of the main window, in the style of Studio Setup. The title names the channel, the
    // insert's number and the effect. The window keeps its place when another effect opens in it.
    const std::string title = EffectTitle(place, effect) + "###Effect";
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(Px(480.0f), 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, Px(theme::kDialogPadding));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::ColorConvertU32ToFloat4(theme::kRaised));
    ImGui::PushStyleColor(ImGuiCol_Border, ImGui::ColorConvertU32ToFloat4(theme::kBorderStrong));
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking |
                                        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings;
    const bool visible = ImGui::Begin(title.c_str(), open, kFlags);
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar();
    if (!visible)
    {
        ImGui::End();
        return EffectEdit::kNone;
    }

    // Escape closes the window while it has the focus, unless a text field is being typed in. Studio Setup does the
    // same.
    if (EscapeCloses())
    {
        *open = false;
    }

    // The preset, if the kind has presets, then the parameters.
    EffectEdit edit = EffectEdit::kNone;
    if (ImGui::BeginTable("##parameters", 2))
    {
        ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthFixed, Px(150.0f));
        ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch);
        // nn::audio has presets of the I3DL2 reverb. Hokuto offers none for the other consoles' effects.
        bool chosen = false;
        if (effect.kind == format::EffectKind::kI3dl2Reverb)
        {
            SettingLabel("Preset");
            chosen = DrawPresets(effect);
        }
        bool changing = false;
        const bool edited = DrawParameters(effect, &changing);
        if (chosen)
        {
            edit = EffectEdit::kPreset;
        }
        else if (edited)
        {
            edit = EffectEdit::kChanged;
        }
        else if (changing)
        {
            edit = EffectEdit::kChanging;
        }
        ImGui::EndTable();
    }

    // Remove takes the effect out of its insert.
    ImGui::Spacing();
    if (ImGui::Button("Remove"))
    {
        edit = EffectEdit::kRemoved;
    }
    ImGui::End();

    return edit;
}

} // namespace hokuto::app
