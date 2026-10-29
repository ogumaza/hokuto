// SPDX-License-Identifier: MIT

#include "song/lengths.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>

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

} // namespace hokuto::song
