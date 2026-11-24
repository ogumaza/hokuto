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
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

#include "agblib/agb/gba.h"
#include "ntdlib/cafe/snd/length_model.h"
#include "ntdlib/ctr/snd/engine.h"
#include "ntdlib/ctr/snd/length_model.h"
#include "ntdlib/nwsnd/length_model.h"
#include "ntdlib/nx/atk/length_model.h"
#include "ntdlib/rvl/snd/length_model.h"
#include "xsflib/rip/length.h"

#include "song/start_handle.h"
#include "song/starts.h"

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

// The 3DS's model for the measure, with the start's volume.
struct CtrStartModel : ntdlib::ctr::snd::ArchiveModel
{
    float volume = 1.0f;
};

// The 3DS's struct of the length analysis for CtrStartModel. The 3DS's model of playback scales its output by the
// start's volume (start_handle::AtVolume), and a frame sounds where a sample at that volume is audible. A decaying tail
// at a lower volume falls below the level of silence earlier.
struct CtrStartLengths : CtrLengths
{
    using Model = CtrStartModel;

    static bool Sounded(const Model& model)
    {
        const auto audible = [volume = model.volume](int16_t sample)
        {
            const int16_t heard = start_handle::AtVolume(sample, volume);
            return heard > xsflib::rip::kSilenceLevel || heard < -xsflib::rip::kSilenceLevel;
        };

        return std::ranges::any_of(Output(model), audible);
    }
};

// A change of a variable in the measure: the frame that it comes in, counted from 0 at the sound's start
// (ChangeFrame), the variable and its value.
struct FrameChange
{
    uint64_t frame = 0;
    int variable = 0;
    int16_t value = 0;
};

// A model of the Wii U or the Switch for the measure, with the start's changes of variables. Neither engine schedules
// a change. The model makes each change at the start of its frame, before the frame's ticks, as playback does
// (SequenceModel::ChangeVariables). ntdlib's length analysis reads the engine through `engine`.
template <class Console>
struct ChangingModel
{
    using Inner = typename Console::Model;
    using Engine = std::remove_reference_t<decltype(*std::declval<Inner&>().engine)>;
    using Player = std::remove_pointer_t<decltype(std::declval<Engine&>().StartSequence(0))>;

    std::unique_ptr<Inner> inner;
    Engine* engine = nullptr;
    Player* player = nullptr;
    std::vector<FrameChange> changes;
    uint64_t frame = 0;
};

// The struct of the length analysis for a ChangingModel of the console of `Console`, ntdlib's struct for the console.
template <class Console>
struct ChangingLengths
{
    using Model = ChangingModel<Console>;

    static constexpr double kFrameMs = Console::kFrameMs;

    // The frame's changes, then the frame.
    static bool RunFrame(Model& model)
    {
        for (const FrameChange& change : model.changes)
        {
            if (change.frame == model.frame)
            {
                start_handle::SetNwVariable(*model.engine, *model.player, change.variable, change.value);
            }
        }
        model.engine->RunFrame();
        ++model.frame;

        return true;
    }

    // A change still to come can change the sound while the sequence plays.
    static bool Pending(const Model& model)
    {
        const auto later = [&model](const FrameChange& change)
        {
            return change.frame >= model.frame;
        };

        return !model.player->IsFinished() && std::ranges::any_of(model.changes, later);
    }

    static std::span<const int16_t> Output(const Model& model)
    {
        return Console::Output(*model.inner);
    }

    static bool Ringing(const Model& model)
    {
        return Console::Ringing(*model.inner);
    }

    static std::size_t VoiceCount(const Model& model)
    {
        return Console::VoiceCount(*model.inner);
    }

    static xsflib::rip::VoicePlace Voice(const Model& model, std::size_t index)
    {
        return Console::Voice(*model.inner, index);
    }
};

// The changes of `start` at the frames of a console whose frames last `frame_ms`.
std::vector<FrameChange> FrameChangesOf(const format::SoundStart& start, FrameMs frame_ms)
{
    std::vector<FrameChange> changes;
    for (const VariableChange& change : ChangesOf(start))
    {
        changes.push_back({ChangeFrame(change.time, frame_ms), change.variable, change.value});
    }

    return changes;
}

// Measures sequence sound `sound` on a model of the Wii U or the Switch that `make_inner()` makes, with the struct of
// the length analysis `Console`, on the console of `flavor`. The sound starts as playback starts it
// (ConsoleModel::ApplyStart), and the start's changes come at their frames.
template <class Console, class MakeInner>
std::optional<xsflib::rip::Timing> MeasureNw(const MakeInner& make_inner, uint32_t sound,
                                             const format::SoundStart& start, sar::Flavor flavor)
{
    using Lengths = ChangingLengths<Console>;
    const std::vector<FrameChange> changes = FrameChangesOf(start, FrameMsOf(flavor));
    const auto make_model = [&make_inner, &changes]
    {
        auto model = std::make_unique<typename Lengths::Model>();
        model->inner = make_inner();
        model->engine = &*model->inner->engine;
        model->changes = changes;
        return model;
    };
    const auto start_sound = [sound, &start](typename Lengths::Model& model)
    {
        auto* player = model.engine->StartSequence(sound);
        if (player == nullptr)
        {
            return player;
        }

        model.player = player;
        for (const auto& [number, value] : StartValuesOf(start))
        {
            start_handle::SetNwVariable(*model.engine, *player, number, value);
        }
        start_handle::ApplyNw(*model.engine, *player, start);
        return player;
    };

    return ntdlib::nwsnd::MeasureSequence<Lengths>(make_model, start_sound,
                                                   ntdlib::nwsnd::LengthRulesFor(Lengths::kFrameMs));
}

} // namespace

std::optional<Length> MeasureLength(const sar::SoundArchive& archive, uint32_t sound, const AuxEffects& effects,
                                    const AuxVolumes& volumes, const format::SoundStart& start)
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
        // The 3DS plays without effects. It gives the sound its start as playback does (ConsoleModel::ApplyStart), and
        // schedules the start's changes. The measure hears the output at the start's volume.
        if (ctr)
        {
            const auto make_model = [&]
            {
                auto model = std::make_unique<CtrStartModel>();
                model->Start(archive);
                model->volume = start.volume.value_or(1.0f);
                return model;
            };
            const auto start_sound = [sound, &start](CtrStartModel& model)
            {
                auto* player = model.engine->StartSequence(sound, start.banks);
                if (player == nullptr)
                {
                    return player;
                }

                for (const auto& [number, value] : StartValuesOf(start))
                {
                    player->SetVariable(*model.engine, number, value);
                }
                for (const VariableChange& change : ChangesOf(start))
                {
                    model.engine->ScheduleVariable(*player, change.variable, change.value,
                                                   ChangeFrame(change.time, FrameMsOf(sar::Flavor::kCtr)));
                }
                start_handle::ApplyCtr(*model.engine, *player, start);
                return player;
            };
            return ntdlib::nwsnd::MeasureSequence<CtrStartLengths>(
                make_model, start_sound, ntdlib::nwsnd::LengthRulesFor(CtrStartLengths::kFrameMs));
        }

        if (wii_u)
        {
            const auto make_model = [&]
            {
                return WiiUModel(archive, effects, volumes);
            };
            return MeasureNw<WiiULengths>(make_model, sound, start, flavor);
        }

        const auto make_model = [&]
        {
            return SwitchModel(archive, effects, volumes);
        };
        return MeasureNw<SwitchLengths>(make_model, sound, start, flavor);
    };
    const std::optional<xsflib::rip::Timing> timing = xsflib::rip::TryMeasure(measure);
    if (!timing)
    {
        return std::nullopt;
    }

    const auto read_sequence = [&]
    {
        return ReadSequence(archive, sound, StartValuesOf(start), ChangesOf(start), start);
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
        return std::min<uint64_t>(longest, static_cast<uint64_t>(std::llround(ms * agblib::agb::kFrameRate / 1000.0)));
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
    // xsflib's limit and fade. A sequence that ends ends at its last sound.
    constexpr xsflib::rip::LengthRules kRules;
    constexpr auto kFadeMs = static_cast<uint32_t>(kRules.fade_ms);
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
        return static_cast<uint32_t>(std::llround(static_cast<double>(frames) * 1000.0 / agblib::agb::kFrameRate));
    };
    const auto longest = static_cast<uint64_t>(kRules.max_ms / 1000.0 * agblib::agb::kFrameRate);
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
        length.length = ms_of(last_sound);
    }

    return length;
}

} // namespace hokuto::song
