// SPDX-License-Identifier: MIT

#include "edit/effects.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <utility>

#include "format/codes.h"

namespace hokuto::edit
{

using ntdlib::cafe::snd::EffectType;
using ntdlib::ctr_fx::DelayParam;
using ntdlib::ctr_fx::Library;
using ntdlib::ctr_fx::ReverbParam;

namespace
{

// A parameter that a file keeps as a whole number. The format holds it within its limits.
uint32_t Whole(float parameter)
{
    return static_cast<uint32_t>(parameter);
}

// A switch's value in a file: 1 for on, and 0 for off.
float SwitchValue(bool on)
{
    return on ? 1.0f : 0.0f;
}

// The Wii's kinds and their classes.
constexpr std::array<std::pair<format::EffectKind, ntdlib::rvl::snd::EffectType>, 6> kWiiTypes = {{
    {format::EffectKind::kWiiReverbHi, ntdlib::rvl::snd::EffectType::kReverbHi},
    {format::EffectKind::kWiiReverbHi2008, ntdlib::rvl::snd::EffectType::kReverbHi2008},
    {format::EffectKind::kWiiChorus, ntdlib::rvl::snd::EffectType::kChorus},
    {format::EffectKind::kWiiDelay, ntdlib::rvl::snd::EffectType::kDelay},
    {format::EffectKind::kWiiReverbStd, ntdlib::rvl::snd::EffectType::kReverbStd},
    {format::EffectKind::kWiiReverbStd2008, ntdlib::rvl::snd::EffectType::kReverbStd2008},
}};

// An effect of the 3DS of `kind` with `parameters` in a file's order. A time or a filter's size becomes its whole
// number, and a switch other than 0 is on. Throws std::invalid_argument for another kind, or for parameters that don't
// suit the kind.
song::AuxEffect CtrEffectOf(format::EffectKind kind, std::span<const float> parameters)
{
    if (!format::IsCtrKind(kind) || parameters.size() != format::ParameterNamesOf(kind).size())
    {
        throw std::invalid_argument("The parameters aren't those of an effect of the 3DS.");
    }

    if (kind == format::EffectKind::kCtrDelay || kind == format::EffectKind::kCtrNnDelay)
    {
        song::CtrDelay delay;
        delay.param.delay_time = Whole(parameters[0]);
        delay.param.feedback = parameters[1];
        delay.param.damping = parameters[2];
        delay.param.enable_surround = parameters[3] != 0.0f;

        return delay;
    }

    song::CtrReverb reverb;
    reverb.library = kind == format::EffectKind::kCtrReverb     ? Library::kNwSnd
                     : kind == format::EffectKind::kCtrNnReverb ? Library::kNnSnd
                                                                : Library::kNnSnd2011;
    ReverbParam& param = reverb.param;
    param.early_reflection_time = Whole(parameters[0]);
    param.fused_time = Whole(parameters[1]);
    param.pre_delay_time = Whole(parameters[2]);
    param.coloration = parameters[3];
    param.damping = parameters[4];
    param.filter_sizes = {Whole(parameters[5]), Whole(parameters[6]), Whole(parameters[7])};
    param.early_gain = parameters[8];
    param.fused_gain = parameters[9];
    if (kind == format::EffectKind::kCtrReverb)
    {
        param.enable_surround = parameters[10] != 0.0f;
        param.high_pass_damping = parameters[11] != 0.0f;
    }
    else if (kind == format::EffectKind::kCtrNnReverb)
    {
        param.high_pass_damping = parameters[10] != 0.0f;
    }

    return reverb;
}

} // namespace

std::optional<EffectType> WiiUTypeOf(format::EffectKind kind)
{
    switch (kind)
    {
    case format::EffectKind::kReverbHi:
        return EffectType::kReverbHi;
    case format::EffectKind::kReverbStd:
        return EffectType::kReverbStd;
    case format::EffectKind::kChorus:
        return EffectType::kChorus;
    case format::EffectKind::kDelay:
        return EffectType::kDelay;
    case format::EffectKind::kI3dl2Reverb:
    case format::EffectKind::kWiiReverbHi:
    case format::EffectKind::kWiiReverbHi2008:
    case format::EffectKind::kWiiChorus:
    case format::EffectKind::kWiiDelay:
    case format::EffectKind::kWiiReverbStd:
    case format::EffectKind::kWiiReverbStd2008:
    case format::EffectKind::kCtrReverb:
    case format::EffectKind::kCtrDelay:
    case format::EffectKind::kCtrNnReverb:
    case format::EffectKind::kCtrNnDelay:
    case format::EffectKind::kCtrNnReverb2011:
        return std::nullopt;
    }

    return std::nullopt;
}

std::optional<ntdlib::rvl::snd::EffectType> WiiTypeOf(format::EffectKind kind)
{
    for (const auto& [wii_kind, type] : kWiiTypes)
    {
        if (wii_kind == kind)
        {
            return type;
        }
    }

    return std::nullopt;
}

const char* EffectName(format::EffectKind kind)
{
    switch (kind)
    {
    case format::EffectKind::kI3dl2Reverb:
        return "I3DL2 Reverb";
    case format::EffectKind::kReverbHi:
        return "nw::snd Reverb Hi";
    case format::EffectKind::kReverbStd:
        return "nw::snd Reverb Std";
    case format::EffectKind::kChorus:
        return "nw::snd Chorus";
    case format::EffectKind::kDelay:
        return "nw::snd Delay (Wii U)";
    case format::EffectKind::kWiiReverbHi:
        return "AXFX Reverb Hi";
    case format::EffectKind::kWiiReverbHi2008:
        return "AXFX Reverb Hi Exp";
    case format::EffectKind::kWiiChorus:
        return "AXFX Chorus";
    case format::EffectKind::kWiiDelay:
        return "AXFX Delay";
    case format::EffectKind::kWiiReverbStd:
        return "AXFX Reverb Std";
    case format::EffectKind::kWiiReverbStd2008:
        return "AXFX Reverb Std Exp";
    case format::EffectKind::kCtrReverb:
        return "nw::snd Reverb (3DS)";
    case format::EffectKind::kCtrDelay:
        return "nw::snd Delay (3DS)";
    case format::EffectKind::kCtrNnReverb:
        return "nn::snd Reverb";
    case format::EffectKind::kCtrNnDelay:
        return "nn::snd Delay";
    case format::EffectKind::kCtrNnReverb2011:
        return "nn::snd Reverb (2011)";
    }

    return "Effect";
}

bool RunsEffect(format::Engine engine, format::EffectKind kind)
{
    switch (engine)
    {
    case format::Engine::kWiiU:
        return WiiUTypeOf(kind).has_value();
    case format::Engine::k3ds:
        return format::IsCtrKind(kind);
    case format::Engine::kWii:
        return WiiTypeOf(kind).has_value();
    case format::Engine::kSwitch:
        break;
    }

    return kind == format::EffectKind::kI3dl2Reverb;
}

std::vector<float> CtrStartingParameters(format::EffectKind kind)
{
    // The parameters' order is that of format/codes.cpp: the reverb's times, factors, filter sizes and gains, then its
    // switches where the kind has them.
    const ReverbParam reverb;
    const DelayParam delay;
    std::vector<float> parameters;
    switch (kind)
    {
    case format::EffectKind::kCtrReverb:
    case format::EffectKind::kCtrNnReverb:
    case format::EffectKind::kCtrNnReverb2011:
        parameters = {static_cast<float>(reverb.early_reflection_time),
                      static_cast<float>(reverb.fused_time),
                      static_cast<float>(reverb.pre_delay_time),
                      reverb.coloration,
                      reverb.damping,
                      static_cast<float>(reverb.filter_sizes[0]),
                      static_cast<float>(reverb.filter_sizes[1]),
                      static_cast<float>(reverb.filter_sizes[2]),
                      reverb.early_gain,
                      reverb.fused_gain};
        if (kind == format::EffectKind::kCtrReverb)
        {
            parameters.push_back(SwitchValue(reverb.enable_surround));
        }
        if (kind != format::EffectKind::kCtrNnReverb2011)
        {
            parameters.push_back(SwitchValue(reverb.high_pass_damping));
        }
        break;

    case format::EffectKind::kCtrDelay:
    case format::EffectKind::kCtrNnDelay:
        parameters = {static_cast<float>(delay.delay_time), delay.feedback, delay.damping,
                      SwitchValue(delay.enable_surround)};
        break;

    case format::EffectKind::kI3dl2Reverb:
    case format::EffectKind::kReverbHi:
    case format::EffectKind::kReverbStd:
    case format::EffectKind::kChorus:
    case format::EffectKind::kDelay:
    case format::EffectKind::kWiiReverbHi:
    case format::EffectKind::kWiiReverbHi2008:
    case format::EffectKind::kWiiChorus:
    case format::EffectKind::kWiiDelay:
    case format::EffectKind::kWiiReverbStd:
    case format::EffectKind::kWiiReverbStd2008:
        break;
    }

    return parameters;
}

song::AuxEffects ModelEffects(const BusEffects& effects)
{
    song::AuxEffects model;
    for (std::size_t bus = 0; bus < effects.size(); bus++)
    {
        for (const format::Effect& effect : effects[bus])
        {
            if (format::EffectProblem(effect))
            {
                continue;
            }

            if (const std::optional<EffectType> type = WiiUTypeOf(effect.kind))
            {
                model[bus].push_back(song::WiiUEffectOf(*type, effect.parameters));
            }
            else if (const std::optional<ntdlib::rvl::snd::EffectType> wii = WiiTypeOf(effect.kind))
            {
                model[bus].push_back(song::WiiEffectOf(*wii, effect.parameters));
            }
            else if (format::IsCtrKind(effect.kind))
            {
                model[bus].push_back(CtrEffectOf(effect.kind, effect.parameters));
            }
            else
            {
                model[bus].push_back(song::I3dl2Reverb(
                    std::span<const float, song::kI3dl2Parameters>(effect.parameters.data(), song::kI3dl2Parameters)));
            }
        }
    }

    return model;
}

song::Mixing ModelMixing(format::Mixing mixing)
{
    return mixing == format::Mixing::kEnhanced ? song::Mixing::kEnhanced : song::Mixing::kStrict;
}

} // namespace hokuto::edit
