// SPDX-License-Identifier: MIT

#include "song/lengths.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <vector>

#include "ntdlib/agb/gba.h"
#include "ntdlib/cafe/snd/length_model.h"
#include "ntdlib/ctr/snd/engine.h"
#include "ntdlib/ctr/snd/length_model.h"
#include "ntdlib/nwsnd/length_model.h"
#include "ntdlib/nx/atk/length_model.h"
#include "ntdlib/rvl/snd/length_model.h"
#include "xsflib/rip/length.h"

namespace hokuto::song
{
namespace
{

namespace sar = ntdlib::sar;
using SwitchLengths = ntdlib::nx::atk::LengthConsole;
using WiiULengths = ntdlib::cafe::snd::LengthConsole;
using CtrLengths = ntdlib::ctr::snd::LengthConsole;
using WiiLengths = ntdlib::rvl::snd::LengthConsole;

static_assert(kLengthLoops == xsflib::rip::kLengthLoops);

// The lengths of `timing`, measured on a console whose frames last `frame_ms`. A fade before the limit follows the
// loops, and the arrangement shows the loop that the performer finds (`read_sequence()`).
template <class ReadSequence>
Length LengthOf(const xsflib::rip::Timing& timing, double frame_ms, const ReadSequence& read_sequence)
{
    const xsflib::rip::LengthRules rules = ntdlib::nwsnd::LengthRulesFor(frame_ms);
    Length length;
    length.length = static_cast<uint32_t>(timing.length_ms);
    length.fade = static_cast<uint32_t>(timing.fade_ms);
    length.silent = timing.silent;
    if (length.fade > 0 && static_cast<double>(timing.length_ms) < rules.max_ms)
    {
        length.loops = kLengthLoops;
        try
        {
            length.loop = read_sequence().loop;
        }
        catch (const std::exception&)
        {
            // The performer can't read the sound. The lengths give no loop.
        }
    }

    return length;
}

} // namespace

std::optional<Length> MeasureLength(const sar::SoundArchive& archive, uint32_t sound, const AuxEffects& effects,
                                    const AuxVolumes& volumes)
{
    if (sound >= archive.Sounds().size() || archive.Sounds()[sound].type != sar::SoundType::kSequence)
    {
        return std::nullopt;
    }

    const sar::Flavor flavor = archive.ReadOptions().flavor;
    const bool wii_u = flavor == sar::Flavor::kCafe;
    const bool ctr = flavor == sar::Flavor::kCtr;
    const auto measure = [&]
    {
        // The 3DS plays without effects.
        if (ctr)
        {
            const auto make_model = [&]
            {
                auto model = std::make_unique<ntdlib::ctr::snd::ArchiveModel>();
                model->Start(archive);
                return model;
            };
            return ntdlib::nwsnd::MeasureSequence<CtrLengths>(make_model, sound);
        }

        if (wii_u)
        {
            const auto make_model = [&]
            {
                return WiiUModel(archive, effects, volumes);
            };
            return ntdlib::nwsnd::MeasureSequence<WiiULengths>(make_model, sound);
        }

        const auto make_model = [&]
        {
            return SwitchModel(archive, effects, volumes);
        };
        return ntdlib::nwsnd::MeasureSequence<SwitchLengths>(make_model, sound);
    };
    const std::optional<xsflib::rip::Timing> timing = xsflib::rip::TryMeasure(measure);
    if (!timing)
    {
        return std::nullopt;
    }

    const auto read_sequence = [&]
    {
        return ReadSequence(archive, sound);
    };
    return LengthOf(*timing, ctr ? CtrLengths::kFrameMs : (wii_u ? WiiULengths::kFrameMs : SwitchLengths::kFrameMs),
                    read_sequence);
}

std::optional<Length> MeasureLength(const ntdlib::rsar::SoundArchive& archive, const ntdlib::SndBuild& build,
                                    uint32_t sound, const AuxEffects& effects, const AuxVolumes& volumes)
{
    if (sound >= archive.Sounds().size() || archive.Sounds()[sound].type != ntdlib::rsar::SoundType::kSequence)
    {
        return std::nullopt;
    }

    // The effects go on the buses before the sound starts, and the buses' volumes go to AX before each frame. The
    // program plays the sound the same way (SequenceModel).
    const auto measure = [&]
    {
        const auto make_model = [&]
        {
            auto model = std::make_unique<WiiLengths::Model>();
            model->build = build;
            model->StartWithRenderer(archive);
            model->engine->SetEffects(WiiEffectSetups(effects), {}, sound);
            std::array<uint16_t, kAuxBuses> returns{};
            for (std::size_t bus = 0; bus < kAuxBuses; bus++)
            {
                returns[bus] = WiiReturnVolume(volumes[bus]);
            }

            model->aux_returns = returns;
            return model;
        };
        return ntdlib::nwsnd::MeasureSequence<WiiLengths>(make_model, sound);
    };
    const std::optional<xsflib::rip::Timing> timing = xsflib::rip::TryMeasure(measure);
    if (!timing)
    {
        return std::nullopt;
    }

    const auto read_sequence = [&]
    {
        return ReadSequence(archive, sound, {}, {}, build);
    };
    return LengthOf(*timing, WiiLengths::kFrameMs, read_sequence);
}

namespace
{

// The loop's ticks take the song's tempos. A tick of Konami's driver is a frame, and one of Nintendo R&D2's driver
// lasts a frame at a tempo of 150. Each pass of the loop lasts as long as the first.
uint64_t LoopTarget(const Sequence& sequence, uint64_t longest)
{
    if (sequence.loop)
    {
        const double start = MsOfTick(sequence, sequence.loop->start);
        const double pass = MsOfTick(sequence, sequence.loop->end) - start;
        const double ms = start + kLengthLoops * pass;
        return std::min<uint64_t>(longest, static_cast<uint64_t>(std::llround(ms * ntdlib::agb::kFrameRate / 1000.0)));
    }

    return longest;
}

// The frames that the driver plays of a sound, up to `target`.
struct PlayedFrames
{
    uint64_t last_sound = 0; // the frames played to the end of the last one with sound
    bool sounded = false;
};

// A sound that the driver can't play to its end, such as a damaged entry of the ROM's table, has no length.
std::optional<PlayedFrames> PlayToEnd(DriverPlayer& player, uint64_t target)
{
    PlayedFrames played;
    std::vector<int16_t> samples;
    try
    {
        while (player.Frames() < target && player.Busy())
        {
            samples.clear();
            player.RunFrame(samples);
            if (std::ranges::any_of(samples, [](int16_t sample) { return sample != 0; }))
            {
                played.last_sound = player.Frames();
                played.sounded = true;
            }
        }
    }
    catch (const std::exception&)
    {
        return std::nullopt;
    }

    return played;
}

} // namespace

std::optional<Length> MeasureLength(const DriverRom& rom, uint32_t sound)
{
    // xsflib's limit, fade and silence after the last sound.
    constexpr xsflib::rip::LengthRules kRules;
    constexpr auto kFadeMs = static_cast<uint32_t>(kRules.fade_ms);
    constexpr auto kAfterLastMs = static_cast<uint32_t>(kRules.tail_ms);
    std::optional<Sequence> sequence;
    try
    {
        sequence = rom.ReadSequence(sound);
    }
    catch (const std::exception&)
    {
        return std::nullopt;
    }

    const auto ms_of = [](uint64_t frames)
    {
        return static_cast<uint32_t>(std::llround(static_cast<double>(frames) * 1000.0 / ntdlib::agb::kFrameRate));
    };
    const auto longest = static_cast<uint64_t>(kRules.max_ms / 1000.0 * ntdlib::agb::kFrameRate);
    const uint64_t target = LoopTarget(*sequence, longest);

    const std::unique_ptr<DriverPlayer> player = rom.MakePlayer();
    try
    {
        player->Start(sound, false);
    }
    catch (const std::exception&)
    {
        return std::nullopt;
    }

    const std::optional<PlayedFrames> played = PlayToEnd(*player, target);
    if (!played)
    {
        return std::nullopt;
    }

    const uint64_t last_sound = played->last_sound;
    const bool sounded = played->sounded;

    Length length;
    length.silent = !sounded;
    if (!sounded)
    {
        return length;
    }

    if (sequence->loop && target < longest && player->Frames() >= target)
    {
        length.length = ms_of(target);
        length.fade = kFadeMs;
        length.loops = kLengthLoops;
        length.loop = sequence->loop;
    }
    else if (player->Frames() >= longest)
    {
        length.length = ms_of(longest);
        length.fade = kFadeMs;
    }
    else
    {
        length.length = ms_of(last_sound) + kAfterLastMs;
    }

    return length;
}

} // namespace hokuto::song
