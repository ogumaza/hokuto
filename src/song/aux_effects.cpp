// SPDX-License-Identifier: MIT

// The effects on the aux buses, their parameters, and how long they ring (song/sequence_model.h).

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <span>
#include <variant>
#include <vector>

#include "ntdlib/cafe/snd/engine.h"
#include "ntdlib/ctr_fx/effects.h"
#include "ntdlib/nx/audio/i3dl2.h"
#include "ntdlib/rvl/ax/pb.h"
#include "ntdlib/rvl/snd/engine.h"

#include "song/console_model.h"
#include "song/sequence_model.h"

namespace hokuto::song
{
namespace
{

static_assert(sizeof(ntdlib::nx::audio::I3dl2ReverbParameterSet) == kI3dl2Parameters * sizeof(float));

// Whether word `word` of the parameter structure of `type` is a mode: in a reverb, the early reflections' mode or the
// fused reverb's.
bool IsMode(ntdlib::cafe::snd::EffectType type, std::size_t word)
{
    switch (type)
    {
    case ntdlib::cafe::snd::EffectType::kReverbHi:
        return word == 6 || word == 8;
    case ntdlib::cafe::snd::EffectType::kReverbStd:
        return word == 5 || word == 7;
    case ntdlib::cafe::snd::EffectType::kChorus:
    case ntdlib::cafe::snd::EffectType::kDelay:
        return false;
    // Hokuto offers neither of Nintendo Land's classes (edit/effects.cpp).
    case ntdlib::cafe::snd::EffectType::kNintendoLandReverb:
    case ntdlib::cafe::snd::EffectType::kNintendoLandDelay:
        return false;
    }

    return false;
}

// The same for the Wii's effects. Only the reverbs of 2008 have modes.
bool IsMode(ntdlib::rvl::snd::EffectType type, std::size_t word)
{
    switch (type)
    {
    case ntdlib::rvl::snd::EffectType::kReverbHi2008:
        return word == static_cast<std::size_t>(ntdlib::rvl::snd::kReverbHi2008Words.early_mode) ||
               word == static_cast<std::size_t>(ntdlib::rvl::snd::kReverbHi2008Words.fused_mode);
    case ntdlib::rvl::snd::EffectType::kReverbStd2008:
        return word == static_cast<std::size_t>(ntdlib::rvl::snd::kReverbStd2008Words.early_mode) ||
               word == static_cast<std::size_t>(ntdlib::rvl::snd::kReverbStd2008Words.fused_mode);
    case ntdlib::rvl::snd::EffectType::kReverbHi:
    case ntdlib::rvl::snd::EffectType::kChorus:
    case ntdlib::rvl::snd::EffectType::kDelay:
    case ntdlib::rvl::snd::EffectType::kReverbStd:
        return false;
    // Hokuto offers neither the mood effect nor the chorus and delay of 2009 (edit/effects.cpp).
    case ntdlib::rvl::snd::EffectType::kPhotoMood:
    case ntdlib::rvl::snd::EffectType::kChorus2009:
    case ntdlib::rvl::snd::EffectType::kDelay2009:
        return false;
    }

    return false;
}

// The seconds in which echoes `delay` seconds apart fall by 60 dB, each `feedback` times the one before. Echoes that
// never fall take forever.
double EchoSeconds(double delay, double feedback)
{
    const double gain = std::fabs(feedback);
    if (!(gain < 1.0))
    {
        return std::numeric_limits<double>::infinity();
    }

    return gain > 0.0 ? delay * 3.0 / -std::log10(gain) : delay;
}

// The seconds in which the tail of `effect` falls by 60 dB: a reverb's decay time after its delays, or the time in
// which a delay line's echoes fall. A chorus gives 0. The Wii U's combs and delay lines store more than one pass of
// what comes in. Their tails therefore start above the input's level. Their decays count a quarter longer: 15 dB more.
// Without that margin, seeks in the standard reverb's tail lay only 57 dB below the signal (2026-10-24). The 3DS's
// combs and delay lines take the same margin.
double TailSeconds(const AuxEffect& effect)
{
    constexpr double kMargin = 1.25;
    if (const auto* reverb = std::get_if<ntdlib::nx::audio::I3dl2ReverbParameterSet>(&effect))
    {
        return static_cast<double>(reverb->reflections_delay) + reverb->late_reverb_delay +
               reverb->late_reverb_decay_time;
    }

    if (const auto* reverb = std::get_if<CtrReverb>(&effect))
    {
        const ntdlib::ctr_fx::ReverbParam& param = reverb->param;
        const double fused = param.pre_delay_time + param.fused_time * kMargin;
        return std::max(static_cast<double>(param.early_reflection_time), fused) / 1000.0;
    }

    if (const auto* delay = std::get_if<CtrDelay>(&effect))
    {
        return EchoSeconds(delay->param.delay_time / 1000.0, delay->param.feedback) * kMargin;
    }

    // The words of the parameter structures (ntdlib::cafe::snd::EffectType and ntdlib::rvl::snd::EffectType): a
    // reverb's pre-delay and fused time in seconds, and a delay's delay in milliseconds and feedback. The Wii's AXFX
    // effects descend from the same library as the Wii U's, and take the same margin.
    if (const auto* wii = std::get_if<WiiEffect>(&effect))
    {
        const auto word = [wii](std::size_t i)
        {
            return i < wii->words.size() ? static_cast<double>(std::bit_cast<float>(wii->words[i])) : 0.0;
        };
        switch (wii->type)
        {
        case ntdlib::rvl::snd::EffectType::kReverbHi:
        case ntdlib::rvl::snd::EffectType::kReverbHi2008:
        case ntdlib::rvl::snd::EffectType::kReverbStd:
        case ntdlib::rvl::snd::EffectType::kReverbStd2008:
            return word(0) + word(1) * kMargin;
        case ntdlib::rvl::snd::EffectType::kChorus:
            return 0.0;
        case ntdlib::rvl::snd::EffectType::kDelay:
            return EchoSeconds(word(0) / 1000.0, word(1)) * kMargin;
        // Hokuto offers neither the mood effect nor the chorus and delay of 2009 (edit/effects.cpp).
        case ntdlib::rvl::snd::EffectType::kPhotoMood:
        case ntdlib::rvl::snd::EffectType::kChorus2009:
        case ntdlib::rvl::snd::EffectType::kDelay2009:
            return 0.0;
        }

        return 0.0;
    }

    const auto& wii_u = std::get<WiiUEffect>(effect);
    const auto word = [&wii_u](std::size_t i)
    {
        return i < wii_u.words.size() ? static_cast<double>(std::bit_cast<float>(wii_u.words[i])) : 0.0;
    };
    switch (wii_u.type)
    {
    case ntdlib::cafe::snd::EffectType::kReverbHi:
    case ntdlib::cafe::snd::EffectType::kReverbStd:
        return word(0) + word(1) * kMargin;
    case ntdlib::cafe::snd::EffectType::kChorus:
        return 0.0;
    case ntdlib::cafe::snd::EffectType::kDelay:
        return EchoSeconds(word(0) / 1000.0, word(1)) * kMargin;
    // Hokuto offers neither of Nintendo Land's classes (edit/effects.cpp).
    case ntdlib::cafe::snd::EffectType::kNintendoLandReverb:
    case ntdlib::cafe::snd::EffectType::kNintendoLandDelay:
        return 0.0;
    }

    return 0.0;
}

} // namespace

namespace console_model
{

uint64_t LeadFramesOf(const AuxEffects& effects, int sample_rate, std::size_t frame_samples)
{
    double seconds = 0.5;
    for (const std::vector<AuxEffect>& bus : effects)
    {
        for (const AuxEffect& effect : bus)
        {
            const auto* wii_u = std::get_if<WiiUEffect>(&effect);
            const auto* wii = std::get_if<WiiEffect>(&effect);
            if ((wii_u != nullptr && wii_u->type == ntdlib::cafe::snd::EffectType::kChorus) ||
                (wii != nullptr && wii->type == ntdlib::rvl::snd::EffectType::kChorus))
            {
                return std::numeric_limits<uint64_t>::max();
            }

            const double tail = TailSeconds(effect);
            seconds = std::isnan(tail) ? 20.0 : std::max(seconds, tail);
        }
    }

    seconds = std::min(seconds, 20.0);

    return static_cast<uint64_t>(std::ceil(seconds * sample_rate / static_cast<double>(frame_samples)));
}

} // namespace console_model

ntdlib::nx::audio::I3dl2ReverbParameterSet I3dl2Reverb(std::span<const float, kI3dl2Parameters> parameters)
{
    return {parameters[0], parameters[1], parameters[2], parameters[3], parameters[4],  parameters[5],
            parameters[6], parameters[7], parameters[8], parameters[9], parameters[10], parameters[11]};
}

std::array<float, kI3dl2Parameters> I3dl2Parameters(const ntdlib::nx::audio::I3dl2ReverbParameterSet& reverb)
{
    return {reverb.room_gain,
            reverb.room_hf_gain,
            reverb.late_reverb_decay_time,
            reverb.late_reverb_hf_decay_ratio,
            reverb.reflections_gain,
            reverb.reflections_delay,
            reverb.reverb_gain,
            reverb.late_reverb_delay,
            reverb.late_reverb_diffusion,
            reverb.late_reverb_density,
            reverb.hf_reference,
            reverb.dry_gain};
}

std::vector<float> WiiUStartingParameters(ntdlib::cafe::snd::EffectType type)
{
    switch (type)
    {
    case ntdlib::cafe::snd::EffectType::kReverbHi:
        return {0.02f, 3.0f, 0.6f, 0.4f, 0.1f, 1.0f, 0.0f, 0.02f, 0.0f, 0.0f, 1.0f};
    case ntdlib::cafe::snd::EffectType::kReverbStd:
        return {0.02f, 3.0f, 0.6f, 0.4f, 1.0f, 0.0f, 0.02f, 0.0f, 0.0f, 1.0f};
    case ntdlib::cafe::snd::EffectType::kChorus:
        return {10.0f, 0.5f, 1.0f, 0.0f, 1.0f};
    case ntdlib::cafe::snd::EffectType::kDelay:
        return {160.0f, 0.4f, 1.0f, 160.0f, 1.0f};
    // Hokuto offers neither of Nintendo Land's classes (edit/effects.cpp).
    case ntdlib::cafe::snd::EffectType::kNintendoLandReverb:
    case ntdlib::cafe::snd::EffectType::kNintendoLandDelay:
        return {};
    }

    return {};
}

WiiUEffect WiiUEffectOf(ntdlib::cafe::snd::EffectType type, std::span<const float> parameters)
{
    WiiUEffect effect;
    effect.type = type;
    const std::size_t words = std::min<std::size_t>(parameters.size(), ntdlib::cafe::snd::EffectParamWords(type));
    for (std::size_t i = 0; i < words; i++)
    {
        const float parameter = parameters[i];
        effect.words.push_back(IsMode(type, i) ? static_cast<uint32_t>(std::max(parameter, 0.0f))
                                               : std::bit_cast<uint32_t>(parameter));
    }

    return effect;
}

std::vector<float> WiiStartingParameters(ntdlib::rvl::snd::EffectType type)
{
    switch (type)
    {
    case ntdlib::rvl::snd::EffectType::kReverbHi:
        return {0.02f, 3.0f, 0.6f, 0.4f, 0.1f, 1.0f};
    case ntdlib::rvl::snd::EffectType::kReverbHi2008:
        return {0.02f, 3.0f, 0.6f, 0.4f, 0.1f, 1.0f, 0.0f, 0.02f, 0.0f, 0.0f, 1.0f};
    case ntdlib::rvl::snd::EffectType::kChorus:
        return {10.0f, 0.5f, 1.0f};
    case ntdlib::rvl::snd::EffectType::kDelay:
        return {160.0f, 0.4f, 1.0f};
    case ntdlib::rvl::snd::EffectType::kReverbStd:
        return {0.02f, 3.0f, 0.6f, 0.4f, 1.0f};
    case ntdlib::rvl::snd::EffectType::kReverbStd2008:
        return {0.02f, 3.0f, 0.6f, 0.4f, 1.0f, 0.0f, 0.02f, 0.0f, 0.0f, 1.0f};
    // Hokuto offers neither the mood effect nor the chorus and delay of 2009 (edit/effects.cpp).
    case ntdlib::rvl::snd::EffectType::kPhotoMood:
    case ntdlib::rvl::snd::EffectType::kChorus2009:
    case ntdlib::rvl::snd::EffectType::kDelay2009:
        return {};
    }

    return {};
}

WiiEffect WiiEffectOf(ntdlib::rvl::snd::EffectType type, std::span<const float> parameters)
{
    WiiEffect effect;
    effect.type = type;
    const std::size_t words = std::min<std::size_t>(parameters.size(), ntdlib::rvl::snd::EffectParamWords(type));
    for (std::size_t i = 0; i < words; i++)
    {
        const float parameter = parameters[i];
        effect.words.push_back(IsMode(type, i) ? static_cast<uint32_t>(std::max(parameter, 0.0f))
                                               : std::bit_cast<uint32_t>(parameter));
    }

    return effect;
}

std::vector<ntdlib::rvl::snd::EffectSetup> WiiEffectSetups(const AuxEffects& effects)
{
    std::vector<ntdlib::rvl::snd::EffectSetup> setups;
    for (std::size_t bus = 0; bus < effects.size(); bus++)
    {
        for (const AuxEffect& effect : effects[bus])
        {
            if (const auto* wii = std::get_if<WiiEffect>(&effect))
            {
                ntdlib::rvl::snd::EffectSetup& setup = setups.emplace_back();
                setup.bus = static_cast<int>(bus);
                setup.type = wii->type;
                std::copy_n(wii->words.begin(), std::min(wii->words.size(), setup.params.size()), setup.params.begin());
            }
        }
    }

    return setups;
}

uint16_t WiiReturnVolume(float volume)
{
    const double kMost = std::numeric_limits<uint16_t>::max();
    const double value = std::clamp(std::round(static_cast<double>(volume) * ntdlib::rvl::ax::kMaxVolume), 0.0, kMost);
    return static_cast<uint16_t>(value);
}

namespace
{

// A second of the frames of 3 ms that AX runs on the Wii U and on the Wii, and the longest time that the effects'
// delays count for: a minute.
constexpr int kRingingSecond = 334;
constexpr double kLongestDelayMs = 60'000.0;

// The first word of an effect's parameters as a float, or 0 for a value that isn't a time.
double FirstTime(const std::vector<uint32_t>& words)
{
    const double value = words.empty() ? 0.0 : static_cast<double>(std::bit_cast<float>(words.front()));
    return std::isfinite(value) && value > 0.0 ? value : 0.0;
}

// The time in ms that an effect takes to give its input back. The first word of each parameter structure holds it: a
// chorus's or a delay's delay in ms, and a reverb's pre-delay in seconds.
double InputDelayMs(const WiiUEffect& effect)
{
    const double first = FirstTime(effect.words);
    switch (effect.type)
    {
    case ntdlib::cafe::snd::EffectType::kChorus:
    case ntdlib::cafe::snd::EffectType::kDelay:
        return first;
    case ntdlib::cafe::snd::EffectType::kReverbHi:
    case ntdlib::cafe::snd::EffectType::kReverbStd:
        return first * 1000.0;
    // Hokuto offers neither of Nintendo Land's classes (edit/effects.cpp).
    case ntdlib::cafe::snd::EffectType::kNintendoLandReverb:
    case ntdlib::cafe::snd::EffectType::kNintendoLandDelay:
        return 0.0;
    }

    return 0.0;
}

double InputDelayMs(const WiiEffect& effect)
{
    const double first = FirstTime(effect.words);
    switch (effect.type)
    {
    case ntdlib::rvl::snd::EffectType::kChorus:
    case ntdlib::rvl::snd::EffectType::kDelay:
        return first;
    case ntdlib::rvl::snd::EffectType::kReverbHi:
    case ntdlib::rvl::snd::EffectType::kReverbHi2008:
    case ntdlib::rvl::snd::EffectType::kReverbStd:
    case ntdlib::rvl::snd::EffectType::kReverbStd2008:
        return first * 1000.0;
    // Hokuto offers neither the mood effect nor the chorus and delay of 2009 (edit/effects.cpp).
    case ntdlib::rvl::snd::EffectType::kPhotoMood:
    case ntdlib::rvl::snd::EffectType::kChorus2009:
    case ntdlib::rvl::snd::EffectType::kDelay2009:
        return 0.0;
    }

    return 0.0;
}

// The frames that the effects of `Effect`'s console among `effects` may ring for (WiiURingingFrames). A bus's effects
// process its samples one after another, and their delays add up.
template <class Effect>
int RingingFramesOf(const AuxEffects& effects)
{
    bool any = false;
    double longest = 0.0;
    for (const std::vector<AuxEffect>& bus : effects)
    {
        double delay = 0.0;
        for (const AuxEffect& effect : bus)
        {
            if (const auto* of_console = std::get_if<Effect>(&effect))
            {
                any = true;
                delay += InputDelayMs(*of_console);
            }
        }

        longest = std::max(longest, delay);
    }
    if (!any)
    {
        return 0;
    }

    constexpr double kFrameMs = 3.0;
    return kRingingSecond + static_cast<int>(std::ceil(std::min(longest, kLongestDelayMs) / kFrameMs));
}

} // namespace

int WiiURingingFrames(const AuxEffects& effects)
{
    return RingingFramesOf<WiiUEffect>(effects);
}

int WiiRingingFrames(const AuxEffects& effects)
{
    return RingingFramesOf<WiiEffect>(effects);
}

} // namespace hokuto::song
