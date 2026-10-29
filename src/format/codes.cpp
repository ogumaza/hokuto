// SPDX-License-Identifier: MIT

#include "format/codes.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/sar/formats.h"

#include "format/file.h"

namespace hokuto::format
{
namespace
{

using ntdlib::commands::Kind;
using ntdlib::sar::Flavor;
using ntdlib::sar::SoundType;

// Each kind's number. 0 is no kind.
constexpr std::array<std::pair<Kind, uint16_t>, ntdlib::commands::kKindCount> kKindCodes = {{
    {Kind::kNote, 1},
    {Kind::kWait, 2},
    {Kind::kProgram, 3},
    {Kind::kOpenTrack, 4},
    {Kind::kJump, 5},
    {Kind::kCall, 6},
    {Kind::kReturn, 7},
    {Kind::kLoopStart, 8},
    {Kind::kLoopEnd, 9},
    {Kind::kEnd, 10},
    {Kind::kAllocateTracks, 11},
    {Kind::kTimebase, 12},
    {Kind::kEnvelopeHold, 13},
    {Kind::kMonophonic, 14},
    {Kind::kVelocityRange, 15},
    {Kind::kBiquadType, 16},
    {Kind::kBiquadValue, 17},
    {Kind::kBankSelect, 18},
    {Kind::kFrontBypass, 19},
    {Kind::kPan, 20},
    {Kind::kVolume, 21},
    {Kind::kMainVolume, 22},
    {Kind::kTranspose, 23},
    {Kind::kPitchBend, 24},
    {Kind::kBendRange, 25},
    {Kind::kPriority, 26},
    {Kind::kNoteWait, 27},
    {Kind::kTie, 28},
    {Kind::kPortamentoFrom, 29},
    {Kind::kPortamento, 30},
    {Kind::kPortamentoTime, 31},
    {Kind::kAttack, 32},
    {Kind::kDecay, 33},
    {Kind::kSustain, 34},
    {Kind::kRelease, 35},
    {Kind::kEnvelopeReset, 36},
    {Kind::kVolume2, 37},
    {Kind::kPrintVariable, 38},
    {Kind::kSurroundPan, 39},
    {Kind::kLowPass, 40},
    {Kind::kSend, 41},
    {Kind::kMainSend, 42},
    {Kind::kInitialPan, 43},
    {Kind::kMute, 44},
    {Kind::kDamper, 45},
    {Kind::kTempo, 46},
    {Kind::kSweepPitch, 47},
    {Kind::kLfoCurve, 48},
    {Kind::kLfoPhase, 49},
    {Kind::kLfoDepth, 50},
    {Kind::kLfoSpeed, 51},
    {Kind::kLfoType, 52},
    {Kind::kLfoRange, 53},
    {Kind::kLfoDelay, 54},
    {Kind::kLfoPeriod, 55},
    {Kind::kSetVariable, 56},
    {Kind::kAddVariable, 57},
    {Kind::kSubtractVariable, 58},
    {Kind::kMultiplyVariable, 59},
    {Kind::kDivideVariable, 60},
    {Kind::kShiftVariable, 61},
    {Kind::kRandomVariable, 62},
    {Kind::kAndVariable, 63},
    {Kind::kOrVariable, 64},
    {Kind::kXorVariable, 65},
    {Kind::kNotVariable, 66},
    {Kind::kModuloVariable, 67},
    {Kind::kCompareEqual, 68},
    {Kind::kCompareGreaterOrEqual, 69},
    {Kind::kCompareGreater, 70},
    {Kind::kCompareLessOrEqual, 71},
    {Kind::kCompareLess, 72},
    {Kind::kCompareNotEqual, 73},
    {Kind::kUserProcedure, 74},
    {Kind::kOther, 75},
    {Kind::kData, 76},
}};

// Each engine's number and name. 0 is no engine.
constexpr std::array<std::pair<uint8_t, std::string_view>, 4> kEngines = {{
    {1, "switch"},
    {2, "wii_u"},
    {3, "3ds"},
    {4, "wii"},
}};

constexpr std::array<std::pair<uint8_t, std::string_view>, 2> kMixings = {{
    {0, "strict"},
    {1, "enhanced"},
}};

// Each kind of effect's number and name, by the kind. 0 is no effect. An I3DL2 reverb is HSF's type 1 of aux effect.
// The Wii U's kinds follow in the order of nw::snd's classes FxReverbHi, FxReverbStd, FxChorus and FxDelay. The Wii's
// kinds follow in the order of ntdlib's types of its effects, each 5 more than the type. The 3DS's kinds follow:
// nw::snd's FxReverb and FxDelay, nn::snd's FxReverb and FxDelay, and nn::snd's FxReverb of 2011.
constexpr std::array<std::pair<uint8_t, std::string_view>, 16> kEffectKinds = {{
    {1, "i3dl2_reverb"},
    {2, "reverb_hi"},
    {3, "reverb_std"},
    {4, "chorus"},
    {5, "delay"},
    {6, "wii_reverb_hi"},
    {7, "wii_reverb_hi_2008"},
    {8, "wii_chorus"},
    {9, "wii_delay"},
    {10, "wii_reverb_std"},
    {11, "wii_reverb_std_2008"},
    {12, "3ds_reverb"},
    {13, "3ds_delay"},
    {14, "3ds_nn_reverb"},
    {15, "3ds_nn_delay"},
    {16, "3ds_nn_reverb_2011"},
}};

// The parameters of an I3DL2 reverb, in the order of nn::audio's I3dl2ReverbParameterSet.
constexpr std::array<std::string_view, 12> kI3dl2ReverbParameters = {
    "room_gain",         "room_hf_gain", "late_reverb_decay_time", "late_reverb_hf_decay_ratio", "reflections_gain",
    "reflections_delay", "reverb_gain",  "late_reverb_delay",      "late_reverb_diffusion",      "late_reverb_density",
    "hf_reference",      "dry_gain",
};

// The parameters of the Wii U's effects, in the order of the fields of their nw::snd classes' parameter structures.
// They have the names of the system library's parameters. The reverbs' times are in seconds, and those of the chorus
// and the delay in milliseconds.
constexpr std::array<std::string_view, 11> kReverbHiParameters = {
    "pre_delay_time", "fused_time",         "coloration", "damping",    "crosstalk",  "out_gain",
    "early_mode",     "pre_delay_time_max", "fused_mode", "early_gain", "fused_gain",
};
constexpr std::array<std::string_view, 10> kReverbStdParameters = {
    "pre_delay_time", "fused_time",         "coloration", "damping",    "out_gain",
    "early_mode",     "pre_delay_time_max", "fused_mode", "early_gain", "fused_gain",
};
constexpr std::array<std::string_view, 5> kChorusParameters = {"delay_time", "depth", "rate", "feedback", "out_gain"};
constexpr std::array<std::string_view, 5> kDelayParameters = {"delay", "feedback", "out_gain", "max_delay", "lpf"};

// The parameters of the Wii's effects, in the order of their parameter structures, with the names of the Wii U's that
// match them. The reverbs of 2008 have the Wii U's parameters. The standard reverb before 2008 has those of the high
// reverb before 2008, without the crosstalk.
constexpr std::array<std::string_view, 6> kWiiReverbHiParameters = {"pre_delay_time", "fused_time", "coloration",
                                                                    "damping",        "crosstalk",  "out_gain"};
constexpr std::array<std::string_view, 5> kWiiReverbStdParameters = {"pre_delay_time", "fused_time", "coloration",
                                                                     "damping", "out_gain"};
constexpr std::array<std::string_view, 3> kWiiChorusParameters = {"delay_time", "depth", "rate"};
constexpr std::array<std::string_view, 3> kWiiDelayParameters = {"delay", "feedback", "out_gain"};

// The parameters of the 3DS's effects, in the order of the fields of their classes' Param. A reverb's three filter
// sizes take the place of the pointer to them: the two comb filters and the all-pass filter. The times are in
// milliseconds and the sizes in samples. nn::snd's FxReverb has no surround, and its version of 2011 has no high-pass
// damping either. nw::snd's FxDelay and nn::snd's FxDelay take the same parameters.
constexpr std::array<std::string_view, 12> kCtrReverbParameters = {
    "early_reflection_time", "fused_time",    "pre_delay_time", "coloration", "damping",  "comb_size_0",
    "comb_size_1",           "all_pass_size", "early_gain",     "fused_gain", "surround", "high_pass_damping",
};
constexpr std::array<std::string_view, 11> kCtrNnReverbParameters = {
    "early_reflection_time", "fused_time",    "pre_delay_time", "coloration", "damping",           "comb_size_0",
    "comb_size_1",           "all_pass_size", "early_gain",     "fused_gain", "high_pass_damping",
};
constexpr std::array<std::string_view, 10> kCtrNnReverb2011Parameters = {
    "early_reflection_time", "fused_time",  "pre_delay_time", "coloration", "damping",
    "comb_size_0",           "comb_size_1", "all_pass_size",  "early_gain", "fused_gain",
};
constexpr std::array<std::string_view, 4> kCtrDelayParameters = {"delay_time", "feedback", "damping", "surround"};

// The choices of the reverbs' modes, and of the 3DS's switches.
constexpr uint32_t kEarlyModes = 8;
constexpr uint32_t kFusedModes = 6;
constexpr uint32_t kSwitchChoices = 2;

// The limits of the 3DS's parameters, by their names. The times are whole milliseconds. A filter holds whole frames of
// 160 samples. ntdlib's model refuses other sizes: the libraries' last frame of such a filter runs into the next
// buffer. The libraries take any time and size that fits the game's memory. These limits keep each effect's buffers
// within a few megabytes.
constexpr ParameterLimits kCtrShortTime = {0.0f, 1000.0f, 1.0f};
constexpr ParameterLimits kCtrFilterSize = {160.0f, 32000.0f, 160.0f};
constexpr ParameterLimits kCtrGain = {0.0f, 1.0f, 0.0f};
constexpr std::array<std::pair<std::string_view, ParameterLimits>, 12> kCtrLimits = {{
    {"early_reflection_time", kCtrShortTime},
    {"fused_time", {0.0f, 20000.0f, 1.0f}},
    {"pre_delay_time", kCtrShortTime},
    {"coloration", kCtrGain},
    {"damping", kCtrGain},
    {"comb_size_0", kCtrFilterSize},
    {"comb_size_1", kCtrFilterSize},
    {"all_pass_size", kCtrFilterSize},
    {"early_gain", kCtrGain},
    {"fused_gain", kCtrGain},
    {"delay_time", {1.0f, 5000.0f, 1.0f}},
    {"feedback", kCtrGain},
}};

// Each kind of native archive's number and name. 0 is no archive. The Wii's archives take 4 (kWiiArchiveCode).
constexpr std::array<std::pair<Flavor, std::pair<uint8_t, std::string_view>>, 3> kFlavors = {{
    {Flavor::kNx, {1, "switch"}},
    {Flavor::kCafe, {2, "wii_u"}},
    {Flavor::kCtr, {3, "3ds"}},
}};

// Each kind of sound's number and name. 0 is a sound of another kind: ntdlib's SoundType::kUnknown.
constexpr std::array<std::pair<SoundType, std::pair<uint8_t, std::string_view>>, 4> kSoundTypes = {{
    {SoundType::kUnknown, {0, "unknown"}},
    {SoundType::kSequence, {1, "sequence"}},
    {SoundType::kWaveSound, {2, "wave_sound"}},
    {SoundType::kStream, {3, "stream"}},
}};

} // namespace

uint16_t CodeOf(Kind kind)
{
    for (const auto& [k, code] : kKindCodes)
    {
        if (k == kind)
        {
            return code;
        }
    }

    return 0;
}

std::optional<Kind> KindOfCode(uint16_t code)
{
    for (const auto& [kind, c] : kKindCodes)
    {
        if (c == code)
        {
            return kind;
        }
    }

    return std::nullopt;
}

uint8_t CodeOf(Engine engine)
{
    return kEngines[static_cast<std::size_t>(engine)].first;
}

std::optional<Engine> EngineOfCode(uint8_t code)
{
    for (std::size_t i = 0; i < kEngines.size(); ++i)
    {
        if (kEngines[i].first == code)
        {
            return static_cast<Engine>(i);
        }
    }

    return std::nullopt;
}

std::string_view NameOf(Engine engine)
{
    return kEngines[static_cast<std::size_t>(engine)].second;
}

std::optional<Engine> EngineNamed(std::string_view name)
{
    for (std::size_t i = 0; i < kEngines.size(); ++i)
    {
        if (kEngines[i].second == name)
        {
            return static_cast<Engine>(i);
        }
    }

    return std::nullopt;
}

uint8_t CodeOf(Mixing mixing)
{
    return kMixings[static_cast<std::size_t>(mixing)].first;
}

std::optional<Mixing> MixingOfCode(uint8_t code)
{
    for (std::size_t i = 0; i < kMixings.size(); ++i)
    {
        if (kMixings[i].first == code)
        {
            return static_cast<Mixing>(i);
        }
    }

    return std::nullopt;
}

std::string_view NameOf(Mixing mixing)
{
    return kMixings[static_cast<std::size_t>(mixing)].second;
}

std::optional<Mixing> MixingNamed(std::string_view name)
{
    for (std::size_t i = 0; i < kMixings.size(); ++i)
    {
        if (kMixings[i].second == name)
        {
            return static_cast<Mixing>(i);
        }
    }

    return std::nullopt;
}

uint8_t CodeOf(Flavor flavor)
{
    for (const auto& [f, entry] : kFlavors)
    {
        if (f == flavor)
        {
            return entry.first;
        }
    }

    return 0;
}

std::optional<Flavor> FlavorOfCode(uint8_t code)
{
    for (const auto& [flavor, entry] : kFlavors)
    {
        if (entry.first == code)
        {
            return flavor;
        }
    }

    return std::nullopt;
}

std::string_view NameOf(Flavor flavor)
{
    for (const auto& [f, entry] : kFlavors)
    {
        if (f == flavor)
        {
            return entry.second;
        }
    }

    return {};
}

std::optional<Flavor> FlavorNamed(std::string_view name)
{
    for (const auto& [flavor, entry] : kFlavors)
    {
        if (entry.second == name)
        {
            return flavor;
        }
    }

    return std::nullopt;
}

uint8_t CodeOf(EffectKind kind)
{
    return kEffectKinds[static_cast<std::size_t>(kind)].first;
}

std::optional<EffectKind> EffectKindOfCode(uint8_t code)
{
    for (std::size_t i = 0; i < kEffectKinds.size(); ++i)
    {
        if (kEffectKinds[i].first == code)
        {
            return static_cast<EffectKind>(i);
        }
    }

    return std::nullopt;
}

std::string_view NameOf(EffectKind kind)
{
    return kEffectKinds[static_cast<std::size_t>(kind)].second;
}

std::optional<EffectKind> EffectKindNamed(std::string_view name)
{
    for (std::size_t i = 0; i < kEffectKinds.size(); ++i)
    {
        if (kEffectKinds[i].second == name)
        {
            return static_cast<EffectKind>(i);
        }
    }

    return std::nullopt;
}

std::span<const std::string_view> ParameterNamesOf(EffectKind kind)
{
    switch (kind)
    {
    case EffectKind::kI3dl2Reverb:
        return kI3dl2ReverbParameters;
    case EffectKind::kReverbHi:
        return kReverbHiParameters;
    case EffectKind::kReverbStd:
        return kReverbStdParameters;
    case EffectKind::kChorus:
        return kChorusParameters;
    case EffectKind::kDelay:
        return kDelayParameters;
    case EffectKind::kWiiReverbHi:
        return kWiiReverbHiParameters;
    case EffectKind::kWiiReverbHi2008:
        return kReverbHiParameters;
    case EffectKind::kWiiChorus:
        return kWiiChorusParameters;
    case EffectKind::kWiiDelay:
        return kWiiDelayParameters;
    case EffectKind::kWiiReverbStd:
        return kWiiReverbStdParameters;
    case EffectKind::kWiiReverbStd2008:
        return kReverbStdParameters;
    case EffectKind::kCtrReverb:
        return kCtrReverbParameters;
    case EffectKind::kCtrNnReverb:
        return kCtrNnReverbParameters;
    case EffectKind::kCtrNnReverb2011:
        return kCtrNnReverb2011Parameters;
    case EffectKind::kCtrDelay:
    case EffectKind::kCtrNnDelay:
        return kCtrDelayParameters;
    }

    return {};
}

uint32_t ChoicesOf(EffectKind kind, std::size_t parameter)
{
    const std::span<const std::string_view> names = ParameterNamesOf(kind);
    if (parameter >= names.size())
    {
        return 0;
    }

    if (names[parameter] == "early_mode")
    {
        return kEarlyModes;
    }
    if (names[parameter] == "fused_mode")
    {
        return kFusedModes;
    }
    if (IsCtrKind(kind) && (names[parameter] == "surround" || names[parameter] == "high_pass_damping"))
    {
        return kSwitchChoices;
    }

    return 0;
}

bool IsCtrKind(EffectKind kind)
{
    switch (kind)
    {
    case EffectKind::kCtrReverb:
    case EffectKind::kCtrDelay:
    case EffectKind::kCtrNnReverb:
    case EffectKind::kCtrNnDelay:
    case EffectKind::kCtrNnReverb2011:
        return true;
    case EffectKind::kI3dl2Reverb:
    case EffectKind::kReverbHi:
    case EffectKind::kReverbStd:
    case EffectKind::kChorus:
    case EffectKind::kDelay:
    case EffectKind::kWiiReverbHi:
    case EffectKind::kWiiReverbHi2008:
    case EffectKind::kWiiChorus:
    case EffectKind::kWiiDelay:
    case EffectKind::kWiiReverbStd:
    case EffectKind::kWiiReverbStd2008:
        break;
    }

    return false;
}

std::optional<ParameterLimits> LimitsOf(EffectKind kind, std::size_t parameter)
{
    const std::span<const std::string_view> names = ParameterNamesOf(kind);
    if (!IsCtrKind(kind) || parameter >= names.size())
    {
        return std::nullopt;
    }

    for (const auto& [name, limits] : kCtrLimits)
    {
        if (name == names[parameter])
        {
            return limits;
        }
    }

    return std::nullopt;
}

uint8_t CodeOf(SoundType type)
{
    for (const auto& [t, entry] : kSoundTypes)
    {
        if (t == type)
        {
            return entry.first;
        }
    }

    return 0;
}

std::optional<SoundType> SoundTypeOfCode(uint8_t code)
{
    for (const auto& [type, entry] : kSoundTypes)
    {
        if (entry.first == code)
        {
            return type;
        }
    }

    return std::nullopt;
}

std::string_view NameOf(SoundType type)
{
    for (const auto& [t, entry] : kSoundTypes)
    {
        if (t == type)
        {
            return entry.second;
        }
    }

    return {};
}

std::optional<SoundType> SoundTypeNamed(std::string_view name)
{
    for (const auto& [type, entry] : kSoundTypes)
    {
        if (entry.second == name)
        {
            return type;
        }
    }

    return std::nullopt;
}

} // namespace hokuto::format
