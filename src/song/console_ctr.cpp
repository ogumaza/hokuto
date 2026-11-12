// SPDX-License-Identifier: MIT

// The model of the 3DS, on ntdlib's archive mode with the effects of ctr_fx (song/console_model.h).

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <utility>
#include <variant>
#include <vector>

#include "ntdlib/ctr_fx/effects.h"
#include "xsflib/rip/length.h"

#include "song/console_model.h"
#include "song/ctr_model.h"

namespace hokuto::song
{
namespace
{

// The frames that the 3DS's effects process in `mixing`. The enhanced mode keeps the libraries' frames, of 235 samples
// each. Its rate keeps the effects' coefficients those of strict mixing (docs/format.md, "The mixing modes").
ntdlib::ctr_fx::FrameFormat CtrFrameFormat(Mixing mixing)
{
    if (mixing != Mixing::kEnhanced)
    {
        return {};
    }

    const ntdlib::ctr_fx::FrameFormat strict;
    return {CtrModel::kEnhancedFrameSamples,
            strict.rate * static_cast<float>(CtrModel::kEnhancedFrameSamples) / static_cast<float>(strict.samples)};
}

// The frames that each channel's buffers of a reverb of the 3DS hold: the early reflection, the pre-delay and the
// filters.
uint32_t ReverbFramesOf(const ntdlib::ctr_fx::ReverbParam& param)
{
    uint32_t frames =
        ntdlib::ctr_fx::ReverbFrames(param.early_reflection_time) + ntdlib::ctr_fx::ReverbFrames(param.pre_delay_time);
    for (const uint32_t size : param.filter_sizes)
    {
        frames += size / static_cast<uint32_t>(CtrModel::kFrameSamples);
    }

    return frames;
}

// The 3DS: ntdlib's archive mode of the 3DS on ntdlib's model of the DSP's audio firmware (song/ctr_model.h), in strict
// or enhanced mixing. The DSP mixes every frame. A track's meter adds up its notes' mix into the DSP's main bus
// (ntdlib's Engine::MeterTracks). The meters show the frame that the DSP mixed last. The output plays that frame about
// three frames later.
//
// The 3DS's effects on buses A and B run in the buses' aux callbacks, in the order that they process the bus. A bus
// without an effect gets no callback, and nn::snd leaves it off. A callback also gives its bus the volume above 1, and
// measures the bus's front channels for the meter. A snapshot of the model copies the callbacks with the effects'
// state.
//
// A reverb's tail sounds on after the last note, and a delay's echo can come back after a quiet gap. With an effect,
// the model stays busy until the output has stayed within ±8 for a second and the longest time that an effect's input
// takes to come out. The 3DS's reverbs can hold their damping filters at -1 once their sound has gone. The output then
// keeps an offset of a step or two, and never comes out silent. With an effect, a frame within ±8 counts as silent.
class CtrConsole final : public ConsoleModel
{
public:
    // Starts the DSP, and puts the effects of the 3DS on their buses. Throws std::runtime_error if the DSP doesn't
    // start, and std::invalid_argument for an effect's parameters that the library refuses.
    CtrConsole(const ntdlib::sar::SoundArchive& archive, const AuxEffects& effects, Mixing mixing)
        : model_(std::make_unique<CtrModel>(mixing == Mixing::kEnhanced))
    {
        if (!model_->Start(archive))
        {
            throw std::runtime_error("The 3DS DSP didn't start.");
        }

        // The effects go on their buses before the sound starts. A game sets up its effects the same way.
        const ntdlib::ctr_fx::FrameFormat format = CtrFrameFormat(mixing);
        for (std::size_t bus = 0; bus < kBuses; bus++)
        {
            BusProcess process{{}, &buses_[bus]};
            uint32_t frames = 0;
            for (const AuxEffect& effect : effects[bus])
            {
                if (const auto* reverb = std::get_if<CtrReverb>(&effect))
                {
                    process.effects.emplace_back(ntdlib::ctr_fx::Reverb(reverb->library, reverb->param, format));
                    frames += ReverbFramesOf(reverb->param);
                }
                else if (const auto* delay = std::get_if<CtrDelay>(&effect))
                {
                    process.effects.emplace_back(ntdlib::ctr_fx::Delay(delay->param, format));
                    frames += ntdlib::ctr_fx::DelayFrames(delay->param.delay_time);
                }
            }
            if (!process.effects.empty())
            {
                ring_limit_ = std::max(ring_limit_, kSecondFrames + frames);
                model_->Snd().SetAuxCallback(static_cast<int>(bus), std::move(process));
            }
        }
    }

    bool StartSequence(uint32_t sound) override
    {
        player_ = model_->Engine().StartSequence(sound);
        return player_ != nullptr;
    }

    void SetVariable(int number, int16_t value) override
    {
        player_->SetVariable(model_->Engine(), number, value);
    }

    std::array<int16_t, kGameVariables> Variables() const override
    {
        return console_model::GameVariables(player_->LocalVariables(), model_->Engine().GlobalVariables());
    }

    uint32_t TickCount() const override
    {
        return player_->TickCount();
    }

    bool IsFinished() const override
    {
        return player_->IsFinished();
    }

    bool ClockStopped() const override
    {
        return player_->ClockStopped();
    }

    bool IsBusy() const override
    {
        return model_->IsBusy() || quiet_frames_ < ring_limit_;
    }

    // A DSP that stops answering ends the model. Damaged data does the same.
    void RunFrame() override
    {
        if (!model_->RunFrame())
        {
            throw std::runtime_error("The DSP stopped responding.");
        }

        quiet_frames_ = xsflib::rip::Audible(model_->Output()) ? 0 : std::min(quiet_frames_ + 1, ring_limit_);
    }

    bool Silent() const override
    {
        const std::span<const int16_t> output = model_->Output();
        const int level = ring_limit_ > 0 ? xsflib::rip::kSilenceLevel : 0;
        return console_model::QuietWithin(output, level);
    }

    void SetMixing(bool) override
    {
    }

    void MuteTrack(int track, bool mute) override
    {
        model_->Engine().MuteTrack(player_->Id(), track, mute);
    }

    void MeterTracks() override
    {
        model_->Engine().MeterTracks(player_->Id());
    }

    std::array<int32_t, 2> TrackPeaks(int track) const override
    {
        return model_->Engine().TrackPeaks(track);
    }

    // A volume up to 1 is the bus's return volume. Above 1, the bus returns at full volume, and the callback gives it
    // the rest of the volume after its effects. The 3DS has no bus C.
    void SetBusVolume(int bus, float volume) override
    {
        if (bus < 0 || bus >= static_cast<int>(kBuses))
        {
            return;
        }

        buses_[static_cast<std::size_t>(bus)].volume = volume;
        model_->Snd().SetAuxReturnVolume(bus, std::min(volume, 1.0f));
    }

    // The callbacks measure their buses in every frame.
    void MeterBuses() override
    {
    }

    std::array<int32_t, 2> BusPeaks(int bus) const override
    {
        if (bus < 0 || bus >= static_cast<int>(kBuses))
        {
            return {0, 0};
        }

        return buses_[static_cast<std::size_t>(bus)].peaks;
    }

    std::span<const int16_t> Output() const override
    {
        return model_->Output();
    }

    void CopyOutput(std::span<float> out) const override
    {
        song::CopyOutput(*model_, out);
    }

    // A snapshot shares the pages of FCRAM and of the DSP's memory that haven't changed since the last snapshot.
    std::unique_ptr<const ConsoleSnapshot> Save() const override
    {
        auto saved = std::make_shared<const CtrModel::Snapshot>(model_->Save(last_.get()));
        last_ = saved;

        return std::make_unique<const Snapshot>(std::move(saved), quiet_frames_);
    }

    void Restore(const ConsoleSnapshot& snapshot) override
    {
        const auto& saved = static_cast<const Snapshot&>(snapshot);
        model_->Restore(*saved.saved);
        quiet_frames_ = saved.quiet_frames;
    }

private:
    // What a bus's callback shares with the console: the bus's volume, and the peaks of the bus's front channels in the
    // last frame that the callback processed, at that volume.
    struct Bus
    {
        float volume = 1.0f;
        std::array<int32_t, 2> peaks{};
    };

    // A bus's callback: the effects in the order that they process the bus, then the volume above 1 and the meter. Each
    // sample of a volume above 1 rounds to the nearest integer, halves away from zero, and stays within 32 bits.
    struct BusProcess
    {
        void operator()(const std::array<int32_t*, 4>& channels, std::size_t samples)
        {
            for (std::variant<ntdlib::ctr_fx::Reverb, ntdlib::ctr_fx::Delay>& effect : effects)
            {
                std::visit([&channels, samples](auto& model) { model.Process(channels, samples); }, effect);
            }

            const auto held = [](double value)
            {
                const double kLowest = std::numeric_limits<int32_t>::min();
                const double kHighest = std::numeric_limits<int32_t>::max();
                return static_cast<int32_t>(std::clamp(std::round(value), kLowest, kHighest));
            };

            const double volume = bus->volume;
            if (volume > 1.0)
            {
                for (int32_t* channel : channels)
                {
                    std::transform(channel, channel + samples, channel,
                                   [&held, volume](int32_t sample) { return held(sample * volume); });
                }
            }

            for (std::size_t c = 0; c < bus->peaks.size(); c++)
            {
                int64_t peak = 0;
                for (std::size_t n = 0; n < samples; n++)
                {
                    peak = std::max(peak, std::abs(int64_t{channels[c][n]}));
                }

                bus->peaks[c] = held(static_cast<double>(peak) * std::min(volume, 1.0));
            }
        }

        std::vector<std::variant<ntdlib::ctr_fx::Reverb, ntdlib::ctr_fx::Delay>> effects;
        Bus* bus;
    };

    struct Snapshot final : ConsoleSnapshot
    {
        Snapshot(std::shared_ptr<const CtrModel::Snapshot> model, uint32_t quiet)
            : saved(std::move(model)), quiet_frames(quiet)
        {
        }

        std::shared_ptr<const CtrModel::Snapshot> saved;
        uint32_t quiet_frames;
    };

    // The 3DS's aux buses: A and B.
    static constexpr std::size_t kBuses = 2;

    // A second of frames, rounded up. The effects count as quiet up to xsflib's level of silence.
    static constexpr auto kSecondFrames =
        static_cast<uint32_t>((CtrModel::kSampleRate + CtrModel::kFrameSamples - 1) / CtrModel::kFrameSamples);

    std::unique_ptr<CtrModel> model_;
    ntdlib::ctr::snd::SequenceSoundPlayer* player_ = nullptr;
    mutable std::shared_ptr<const CtrModel::Snapshot> last_;
    std::array<Bus, kBuses> buses_;
    uint32_t ring_limit_ = 0;   // with an effect, the quiet frames after which the effects have stopped ringing
    uint32_t quiet_frames_ = 0; // the frames since the output was last louder than ±8, up to ring_limit_
};

} // namespace

namespace console_model
{

std::unique_ptr<ConsoleModel> NewCtrModel(const ntdlib::sar::SoundArchive& archive, const AuxEffects& effects,
                                          Mixing mixing)
{
    return std::make_unique<CtrConsole>(archive, effects, mixing);
}

} // namespace console_model

} // namespace hokuto::song
