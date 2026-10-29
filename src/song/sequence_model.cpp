// SPDX-License-Identifier: MIT

#include "song/sequence_model.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <exception>
#include <iterator>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <utility>
#include <variant>
#include <vector>

#include "ntdlib/cafe/snd/engine.h"
#include "ntdlib/ctr_fx/effects.h"
#include "ntdlib/nx/atk/effect.h"
#include "ntdlib/nx/atk/engine.h"
#include "ntdlib/nx/audio/i3dl2.h"
#include "ntdlib/rvl/ax/pb.h"
#include "ntdlib/rvl/snd/engine.h"
#include "ntdlib/sequence_export/consoles.h"
#include "xsflib/rip/length.h"

#include "song/ctr_model.h"

namespace hokuto::song
{

// A copy of a console's model between frames.
struct ConsoleSnapshot
{
    virtual ~ConsoleSnapshot() = default;
};

// A console's model of its sound library, and the player of the sequence that it plays. The calls follow ntdlib's
// engines and its SequenceSoundPlayer.
class ConsoleModel
{
public:
    virtual ~ConsoleModel() = default;

    // Starts sequence sound `sound`. Returns false if the sound doesn't start. Damaged data in the archive throws.
    virtual bool StartSequence(uint32_t sound) = 0;

    virtual void SetVariable(int number, int16_t value) = 0;

    // The player's local variables, then the global ones.
    virtual std::array<int16_t, kGameVariables> Variables() const = 0;

    virtual uint32_t TickCount() const = 0;
    virtual bool IsFinished() const = 0;

    // Whether the player's clock gives no ticks: a tempo or a timebase of 0 (ntdlib's ClockStopped). The sequence then
    // plays on without ticks, and none of its commands starts the clock again.
    virtual bool ClockStopped() const = 0;

    virtual bool IsBusy() const = 0;
    virtual void RunFrame() = 0;

    // Whether the last frame came out silent. A model that isn't busy ends after such a frame.
    virtual bool Silent() const = 0;

    // Whether the frames mix (see SequenceModel::SetMixing).
    virtual void SetMixing(bool mixing) = 0;

    // The host's mutes and meters of the player's tracks.
    virtual void MuteTrack(int track, bool mute) = 0;
    virtual void MeterTracks() = 0;
    virtual std::array<int32_t, 2> TrackPeaks(int track) const = 0;

    // The host's volumes and meters of the aux buses. A bus's peaks are at its volume.
    virtual void SetBusVolume(int bus, float volume) = 0;
    virtual void MeterBuses() = 0;
    virtual std::array<int32_t, 2> BusPeaks(int bus) const = 0;

    // The last frame's output in interleaved stereo, and the same in floating point (see CopyOutput).
    virtual std::span<const int16_t> Output() const = 0;
    virtual void CopyOutput(std::span<float> out) const = 0;

    virtual std::unique_ptr<const ConsoleSnapshot> Save() const = 0;
    virtual void Restore(const ConsoleSnapshot& snapshot) = 0;
};

namespace
{

// The Switch: ntdlib's model of nn::atk and the audio renderer, with the effects on its aux buses.
struct Switch
{
    using Model = ntdlib::nx::atk::ArchiveModel;

    static std::unique_ptr<Model> Make(const ntdlib::sar::SoundArchive& archive, const AuxEffects& effects,
                                       Mixing mixing)
    {
        // The effects go on their buses before the sound starts. A game sets up its effects the same way.
        ntdlib::nx::atk::EngineOptions options;
        options.enhanced = mixing == Mixing::kEnhanced;
        auto model = std::make_unique<Model>(archive, options);
        for (std::size_t bus = 0; bus < effects.size(); bus++)
        {
            for (const AuxEffect& effect : effects[bus])
            {
                if (const auto* reverb = std::get_if<ntdlib::nx::audio::I3dl2ReverbParameterSet>(&effect))
                {
                    model->engine->AppendEffect(static_cast<int>(bus),
                                                std::make_unique<ntdlib::nx::atk::I3dl2AuxEffect>(*reverb));
                }
            }
        }

        return model;
    }

    // The I3DL2 reverb's tail sounds on without a quiet gap. The model ends once the engine has stopped and a frame has
    // come out silent.
    static int RingingFrames(const AuxEffects&)
    {
        return 0;
    }

    static std::span<const int16_t> Output(const Model& model)
    {
        return model.renderer.DeviceOutput();
    }

    static void SetMixing(Model& model, bool mixing)
    {
        model.renderer.SetMixing(mixing);
    }

    // Sub-mix 0 holds the main bus, then the aux buses A to C. Each bus's channels reach the final mix's channels at
    // the bus's volume.
    static void SetBusVolume(Model& model, int bus, float volume)
    {
        const int channels = model.sub_mix.channel_count;
        for (int channel = 0; channel < channels; channel++)
        {
            model.renderer.SetSubMixMixVolume(model.sub_mix.mix, volume, (1 + bus) * channels + channel, channel);
        }
    }

    static void MeterBuses(Model& model)
    {
        model.renderer.MeterMix(model.sub_mix.mix, true);
    }

    // The renderer measures a bus's front left and right after its effects, before its volume.
    static std::array<int32_t, 2> BusPeaks(const Model& model, int bus, float volume)
    {
        const std::span<const int32_t> peaks = model.renderer.MixPeaks(model.sub_mix.mix);
        const auto left = static_cast<std::size_t>((1 + bus) * model.sub_mix.channel_count);
        if (left + 1 >= peaks.size())
        {
            return {0, 0};
        }

        const auto scaled = [volume](int32_t peak)
        {
            const double kMost = std::numeric_limits<int32_t>::max();
            return static_cast<int32_t>(std::min(std::round(static_cast<double>(peak) * volume), kMost));
        };
        return {scaled(peaks[left]), scaled(peaks[left + 1])};
    }
};

// The Wii U: ntdlib's model of nw::snd and AX, with the TV's stereo output. The effects go on the TV's aux buses. The
// I3DL2 reverb comes from the Switch's audio library. The model plays without it.
struct WiiU
{
    using Model = ntdlib::cafe::snd::ArchiveModel;

    static std::unique_ptr<Model> Make(const ntdlib::sar::SoundArchive& archive, const AuxEffects& effects,
                                       Mixing mixing)
    {
        // The engine appends the effects to their buses before the sound starts, in the order that they process each
        // bus. A game appends its effects the same way.
        ntdlib::cafe::snd::EngineOptions options;
        options.enhanced = mixing == Mixing::kEnhanced;
        for (std::size_t bus = 0; bus < effects.size(); bus++)
        {
            for (const AuxEffect& effect : effects[bus])
            {
                if (const auto* wii_u = std::get_if<WiiUEffect>(&effect))
                {
                    options.effects.push_back(
                        {ntdlib::cafe::snd::kEffectDeviceTv, static_cast<uint8_t>(bus), wii_u->type, wii_u->words, {}});
                }
            }
        }

        return std::make_unique<Model>(archive, std::move(options));
    }

    // A delay's echo can come back after a quiet gap. The model stays busy while an effect may still ring.
    static int RingingFrames(const AuxEffects& effects)
    {
        return WiiURingingFrames(effects);
    }

    static std::span<const int16_t> Output(const Model& model)
    {
        return model.system.Output();
    }

    static void SetMixing(Model& model, bool mixing)
    {
        model.system.SetMixing(mixing);
    }

    // AX's return volume is in 1.15. Its largest value is just under 2.
    static void SetBusVolume(Model& model, int bus, float volume)
    {
        const double kMost = std::numeric_limits<uint16_t>::max();
        const double value =
            std::clamp(std::round(static_cast<double>(volume) * ntdlib::cafe::ax::kUnityVolume), 0.0, kMost);
        model.system.SetAuxReturnVolume(ntdlib::cafe::ax::System::Device::kTv, 0, bus, static_cast<uint16_t>(value));
    }

    // AX measures every bus's return.
    static void MeterBuses(Model&)
    {
    }

    // AX's peaks are at the bus's return volume.
    static std::array<int32_t, 2> BusPeaks(const Model& model, int bus, float)
    {
        return model.system.AuxReturnPeaks(ntdlib::cafe::ax::System::Device::kTv, 0, bus);
    }
};

// A console's model with ntdlib's engine. With an effect that may ring after a quiet gap, the model stays busy until
// the output has had no sound (louder than ±8) for the console's RingingFrames.
template <class Console>
class ModelOf final : public ConsoleModel
{
public:
    ModelOf(const ntdlib::sar::SoundArchive& archive, const AuxEffects& effects, Mixing mixing)
        : model_(Console::Make(archive, effects, mixing)),
          ringing_frames_(Console::RingingFrames(effects)),
          quiet_frames_(ringing_frames_)
    {
    }

    bool StartSequence(uint32_t sound) override
    {
        player_ = model_->engine->StartSequence(sound);
        return player_ != nullptr;
    }

    void SetVariable(int number, int16_t value) override
    {
        player_->SetVariable(*model_->engine, number, value);
    }

    std::array<int16_t, kGameVariables> Variables() const override
    {
        std::array<int16_t, kGameVariables> values{};
        values.fill(-1);
        const std::array<int16_t, 16>& local = player_->LocalVariables();
        const std::array<int16_t, 16>& global = model_->engine->GlobalVariables();
        std::copy(local.begin(), local.end(), values.begin());
        std::copy(global.begin(), global.end(), values.begin() + 16);

        return values;
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
        return model_->engine->IsBusy() || quiet_frames_ < ringing_frames_;
    }

    void RunFrame() override
    {
        model_->engine->RunFrame();
        if (ringing_frames_ > 0)
        {
            quiet_frames_ =
                xsflib::rip::Audible(Console::Output(*model_)) ? 0 : std::min(quiet_frames_ + 1, ringing_frames_);
        }
    }

    // A delay that feeds back can leave samples of ±1 for ever. With an effect that may ring, the output therefore ends
    // within ±8. The 3DS's output ends the same way.
    bool Silent() const override
    {
        const std::span<const int16_t> output = Console::Output(*model_);
        const int level = ringing_frames_ > 0 ? xsflib::rip::kSilenceLevel : 0;
        return std::all_of(output.begin(), output.end(), [level](int16_t sample) { return std::abs(sample) <= level; });
    }

    void SetMixing(bool mixing) override
    {
        Console::SetMixing(*model_, mixing);
    }

    void MuteTrack(int track, bool mute) override
    {
        model_->engine->MuteTrack(player_->Id(), track, mute);
    }

    void MeterTracks() override
    {
        model_->engine->MeterTracks(player_->Id());
    }

    std::array<int32_t, 2> TrackPeaks(int track) const override
    {
        return model_->engine->TrackPeaks(track);
    }

    void SetBusVolume(int bus, float volume) override
    {
        Console::SetBusVolume(*model_, bus, volume);
        bus_volumes_[static_cast<std::size_t>(bus)] = volume;
    }

    void MeterBuses() override
    {
        Console::MeterBuses(*model_);
    }

    std::array<int32_t, 2> BusPeaks(int bus) const override
    {
        return Console::BusPeaks(*model_, bus, bus_volumes_[static_cast<std::size_t>(bus)]);
    }

    std::span<const int16_t> Output() const override
    {
        return Console::Output(*model_);
    }

    void CopyOutput(std::span<float> out) const override
    {
        song::CopyOutput(*model_, out);
    }

    std::unique_ptr<const ConsoleSnapshot> Save() const override
    {
        return std::make_unique<const Snapshot>(model_->Save(), quiet_frames_);
    }

    void Restore(const ConsoleSnapshot& snapshot) override
    {
        const auto& saved = static_cast<const Snapshot&>(snapshot);
        model_->Restore(saved.saved);
        quiet_frames_ = saved.quiet_frames;
    }

private:
    using Model = typename Console::Model;

    struct Snapshot final : ConsoleSnapshot
    {
        Snapshot(typename Model::Snapshot model, int quiet) : saved(std::move(model)), quiet_frames(quiet)
        {
        }

        typename Model::Snapshot saved;
        int quiet_frames;
    };

    std::unique_ptr<Model> model_;
    decltype(model_->engine->StartSequence(0)) player_ = nullptr;
    AuxVolumes bus_volumes_ = kFullVolumes;
    int ringing_frames_; // the quiet frames after which the effects have stopped ringing, or 0
    int quiet_frames_;   // the frames since the output last had sound, up to ringing_frames_
};

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
            throw std::runtime_error("The 3DS's DSP didn't start.");
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
                ring_frames_ = std::max(ring_frames_, kSecondFrames + frames);
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
        std::array<int16_t, kGameVariables> values{};
        values.fill(-1);
        const std::array<int16_t, 16>& local = player_->LocalVariables();
        const std::array<int16_t, 16>& global = model_->Engine().GlobalVariables();
        std::copy(local.begin(), local.end(), values.begin());
        std::copy(global.begin(), global.end(), values.begin() + 16);

        return values;
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
        return model_->IsBusy() || quiet_frames_ < ring_frames_;
    }

    // A DSP that stops answering ends the model. Damaged data does the same.
    void RunFrame() override
    {
        if (!model_->RunFrame())
        {
            throw std::runtime_error("The DSP stopped answering.");
        }

        const std::span<const int16_t> output = model_->Output();
        const bool loud =
            std::any_of(output.begin(), output.end(), [](int16_t sample) { return std::abs(sample) > kQuietLevel; });
        quiet_frames_ = loud ? 0 : std::min(quiet_frames_ + 1, ring_frames_);
    }

    bool Silent() const override
    {
        const std::span<const int16_t> output = model_->Output();
        const int level = ring_frames_ > 0 ? kQuietLevel : 0;
        return std::all_of(output.begin(), output.end(), [level](int16_t sample) { return std::abs(sample) <= level; });
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

    // The output's level up to which the effects count as quiet, and a second of frames, rounded up.
    static constexpr int kQuietLevel = 8;
    static constexpr auto kSecondFrames =
        static_cast<uint32_t>((CtrModel::kSampleRate + CtrModel::kFrameSamples - 1) / CtrModel::kFrameSamples);

    std::unique_ptr<CtrModel> model_;
    ntdlib::ctr::snd::SequenceSoundPlayer* player_ = nullptr;
    mutable std::shared_ptr<const CtrModel::Snapshot> last_;
    std::array<Bus, kBuses> buses_;
    uint32_t ring_frames_ = 0;  // with an effect, the quiet frames after which the effects have stopped ringing
    uint32_t quiet_frames_ = 0; // the frames since the output was last louder than ±8, up to ring_frames_
};

void CopyScaled(std::span<const float> floats, std::span<const int16_t> samples, std::span<float> out);

// The Wii: ntdlib's model of NW4R SND and AX, with ntdlib's model of AX's microcode of the game's build in place of the
// DSP. The model follows the sequence's build of NW4R SND. So does the performer. The effects of the Wii go on the aux
// buses, and run on that build's AXFX library. Every frame mixes. A track's meter adds up its voices' mix into the main
// bus (ntdlib's ArchiveModel::MeterTracks). A mute sets the track's volume to 0 through the sound's handle
// (SeqPlayer::SetTrackVolume). WSF's arrangements leave a track out the same way. The track plays on. Unlike the other
// consoles' mutes, this one reaches the engine: a muted note can end sooner than its release would sound.
class Wii final : public ConsoleModel
{
public:
    using Model = ntdlib::rvl::snd::ArchiveModel;

    // AX mixes 96 stereo samples at 32 kHz in each frame of 3 ms.
    static constexpr int kSampleRate = 32'000;
    static constexpr auto kFrameSamples = static_cast<std::size_t>(ntdlib::rvl::ax::kSamplesPerFrame);

    Wii(const ntdlib::rsar::SoundArchive& archive, const ntdlib::SndBuild& build, const AuxEffects& effects)
        : model_(std::make_unique<Model>()),
          setups_(WiiEffectSetups(effects)),
          ringing_limit_(WiiRingingFrames(effects))
    {
        model_->build = build;
        model_->StartWithRenderer(archive);
    }

    // The effects go on the buses in the first AX frame after the sound starts, in the order that they process each
    // bus. A game appends its effects as the sound starts the same way.
    bool StartSequence(uint32_t sound) override
    {
        model_->engine->SetEffects(setups_, {}, sound);
        player_ = model_->engine->StartSequence(sound);
        return player_ != nullptr;
    }

    void SetVariable(int number, int16_t value) override
    {
        player_->SetVariable(*model_->engine, number, value);
    }

    std::array<int16_t, kGameVariables> Variables() const override
    {
        std::array<int16_t, kGameVariables> values{};
        values.fill(-1);
        const std::array<int16_t, 16>& local = player_->LocalVariables();
        const std::array<int16_t, 16>& global = model_->engine->GlobalVariables();
        std::copy(local.begin(), local.end(), values.begin());
        std::copy(global.begin(), global.end(), values.begin() + 16);

        return values;
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

    // A delay's echo can come back after a quiet gap. With an effect on a bus, the model stays busy after the output
    // last had sound (louder than ±8), for a second and the longest time that a bus's effects take to give their input
    // back (WiiRingingFrames).
    bool IsBusy() const override
    {
        return model_->engine->IsBusy() || (model_->engine->HasEffects() && ringing_frames_ > 0);
    }

    // The model of the microcode refuses a command list that it lacks a command for. AX's stereo mode never sends one.
    // NW4R SND returns a bus at full volume as it appends the bus's first effect (AxManager::AppendEffect), in the
    // first AX frame after the start. The buses' volumes therefore go to AX before each frame. A game sets them after
    // it appends its effects.
    void RunFrame() override
    {
        for (std::size_t bus = 0; bus < returns_.size(); bus++)
        {
            model_->ax.SetAuxReturnVolume(static_cast<int>(bus), returns_[bus]);
        }

        output_.clear();
        if (!model_->RunFrame(output_))
        {
            throw std::runtime_error("The model of AX's microcode refused a frame.");
        }
        ringing_frames_ = xsflib::rip::Audible(output_) ? ringing_limit_ : std::max(ringing_frames_ - 1, 0);
    }

    // A delay that feeds back can leave samples of ±1 for ever. With an effect on a bus, the output therefore ends
    // within ±8. So does the 3DS's output.
    bool Silent() const override
    {
        const int level = model_->engine->HasEffects() ? xsflib::rip::kSilenceLevel : 0;
        return std::all_of(output_.begin(), output_.end(),
                           [level](int16_t sample) { return std::abs(sample) <= level; });
    }

    void SetMixing(bool) override
    {
    }

    void MuteTrack(int track, bool mute) override
    {
        const uint32_t bit = 1u << track;
        mutes_ = mute ? mutes_ | bit : mutes_ & ~bit;
        player_->SetTrackVolume(bit, mute ? 0.0f : 1.0f);
    }

    void MeterTracks() override
    {
        model_->MeterTracks(player_->Id());
    }

    // A muted track's meter stays at 0. So does one on the other consoles.
    std::array<int32_t, 2> TrackPeaks(int track) const override
    {
        if (track >= 0 && track < 32 && (mutes_ >> track & 1u) != 0)
        {
            return {0, 0};
        }

        return model_->TrackPeaks(track);
    }

    void SetBusVolume(int bus, float volume) override
    {
        returns_[static_cast<std::size_t>(bus)] = WiiReturnVolume(volume);
    }

    // The renderer measures every bus's return.
    void MeterBuses() override
    {
    }

    // The renderer's peaks are at the bus's return volume.
    std::array<int32_t, 2> BusPeaks(int bus) const override
    {
        return model_->AuxReturnPeaks(bus);
    }

    std::span<const int16_t> Output() const override
    {
        return output_;
    }

    void CopyOutput(std::span<float> out) const override
    {
        CopyScaled({}, output_, out);
    }

    // Each snapshot shares the memory pages that haven't changed with the one before.
    std::unique_ptr<const ConsoleSnapshot> Save() const override
    {
        auto saved = std::make_shared<const Model::Snapshot>(model_->Save(last_ ? &*last_ : nullptr));
        last_ = saved;

        return std::make_unique<const Snapshot>(std::move(saved), output_, ringing_frames_);
    }

    // A snapshot holds the tracks' volumes from the time it was taken. The mutes hold through a restore.
    void Restore(const ConsoleSnapshot& snapshot) override
    {
        const auto& saved = static_cast<const Snapshot&>(snapshot);
        model_->Restore(*saved.saved);
        output_ = saved.output;
        ringing_frames_ = saved.ringing_frames;
        player_->SetTrackVolume(mutes_, 0.0f);
        player_->SetTrackVolume(~mutes_, 1.0f);
    }

private:
    struct Snapshot final : ConsoleSnapshot
    {
        Snapshot(std::shared_ptr<const Model::Snapshot> model, std::vector<int16_t> last, int ringing)
            : saved(std::move(model)), output(std::move(last)), ringing_frames(ringing)
        {
        }

        std::shared_ptr<const Model::Snapshot> saved;
        std::vector<int16_t> output;
        int ringing_frames;
    };

    std::unique_ptr<Model> model_;
    std::vector<ntdlib::rvl::snd::EffectSetup> setups_; // the effects on the buses
    int ringing_limit_;                                 // the quiet frames after which the effects have stopped ringing
    ntdlib::rvl::snd::SeqPlayer* player_ = nullptr;
    std::vector<int16_t> output_;                         // the last frame's output
    mutable std::shared_ptr<const Model::Snapshot> last_; // the latest snapshot
    uint32_t mutes_ = 0;                                  // a bit for each muted track
    int ringing_frames_ = 0;                              // the frames left until a silent output ends the sound
    std::array<uint16_t, kAuxBuses> returns_ = {ntdlib::rvl::ax::kMaxVolume, ntdlib::rvl::ax::kMaxVolume,
                                                ntdlib::rvl::ax::kMaxVolume}; // the buses' return volumes
};

static_assert(sizeof(ntdlib::nx::audio::I3dl2ReverbParameterSet) == kI3dl2Parameters * sizeof(float));

// A change's frame (ChangeFrame) counts the model's frames.
static_assert(Switch::Model::kFrameSamples * 1000 ==
              FrameMsOf(ntdlib::sar::Flavor::kNx).numerator * static_cast<uint32_t>(Switch::Model::kSampleRate));
static_assert(WiiU::Model::kFrameSamples * 1000 ==
              FrameMsOf(ntdlib::sar::Flavor::kCafe).numerator * static_cast<uint32_t>(WiiU::Model::kSampleRate));
static_assert(CtrModel::kFrameSamples * 8192 * 1000 * FrameMsOf(ntdlib::sar::Flavor::kCtr).denominator ==
              FrameMsOf(ntdlib::sar::Flavor::kCtr).numerator * uint64_t{268111856});

// The 3DS's frames last as long in both modes. The enhanced rate is the DSP's rate times 235 / 160, rounded.
static_assert((uint64_t{268111856} * CtrModel::kEnhancedFrameSamples + 8192 * CtrModel::kFrameSamples / 2) /
                  (8192 * CtrModel::kFrameSamples) ==
              CtrModel::kEnhancedSampleRate);

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
    }

    return false;
}

// The stereo frames in each of the console's frames in `mixing`. Throws std::invalid_argument for a console that Hokuto
// doesn't play.
std::size_t FrameSamplesOf(ntdlib::sar::Flavor flavor, Mixing mixing)
{
    switch (flavor)
    {
    case ntdlib::sar::Flavor::kNx:
        return Switch::Model::kFrameSamples;
    case ntdlib::sar::Flavor::kCafe:
        return WiiU::Model::kFrameSamples;
    case ntdlib::sar::Flavor::kCtr:
        return mixing == Mixing::kEnhanced ? CtrModel::kEnhancedFrameSamples : CtrModel::kFrameSamples;
    default:
        throw std::invalid_argument("Hokuto plays only the sound archives of the Switch, the Wii U and the 3DS.");
    }
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
    }

    return 0.0;
}

// The frames of the lead (SequenceModel::LeadFrames): 0.5 s, or the longest tail of the effects, up to 20 s. A chorus
// makes every frame mix.
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

// Copies `floats` to `out`, or else `samples` with the scale of a 16-bit sample, and zeros after them.
void CopyScaled(std::span<const float> floats, std::span<const int16_t> samples, std::span<float> out)
{
    constexpr float kSampleScale = 1.0f / 32768.0f;
    if (!floats.empty())
    {
        const std::size_t count = std::min(floats.size(), out.size());
        std::copy_n(floats.begin(), count, out.begin());
        std::fill(out.begin() + static_cast<std::ptrdiff_t>(count), out.end(), 0.0f);
        return;
    }

    for (std::size_t i = 0; i < out.size(); i++)
    {
        out[i] = i < samples.size() ? static_cast<float>(samples[i]) * kSampleScale : 0.0f;
    }
}

} // namespace

void CopyOutput(const ntdlib::nx::atk::ArchiveModel& model, std::span<float> out)
{
    CopyScaled(model.renderer.DeviceOutputFloat(), model.renderer.DeviceOutput(), out);
}

void CopyOutput(const ntdlib::cafe::snd::ArchiveModel& model, std::span<float> out)
{
    CopyScaled(model.system.OutputFloat(), model.system.Output(), out);
}

void CopyOutput(const CtrModel& model, std::span<float> out)
{
    CopyScaled(model.FloatOutput(), model.Output(), out);
}

std::unique_ptr<ntdlib::nx::atk::ArchiveModel> SwitchModel(const ntdlib::sar::SoundArchive& archive,
                                                           const AuxEffects& effects, const AuxVolumes& volumes)
{
    std::unique_ptr<Switch::Model> model = Switch::Make(archive, effects, Mixing::kStrict);
    for (std::size_t bus = 0; bus < kAuxBuses; bus++)
    {
        Switch::SetBusVolume(*model, static_cast<int>(bus), volumes[bus]);
    }

    return model;
}

std::unique_ptr<ntdlib::cafe::snd::ArchiveModel> WiiUModel(const ntdlib::sar::SoundArchive& archive,
                                                           const AuxEffects& effects, const AuxVolumes& volumes)
{
    std::unique_ptr<WiiU::Model> model = WiiU::Make(archive, effects, Mixing::kStrict);
    for (std::size_t bus = 0; bus < kAuxBuses; bus++)
    {
        WiiU::SetBusVolume(*model, static_cast<int>(bus), volumes[bus]);
    }

    return model;
}

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

SequenceModel::SequenceModel(std::shared_ptr<const ntdlib::sar::SoundArchive> archive,
                             std::shared_ptr<const Sequence> sequence, AuxEffects effects, Mixing mixing,
                             uint64_t snapshot_interval)
    : archive_(std::move(archive)),
      sequence_(std::move(sequence)),
      effects_(std::move(effects)),
      mixing_(mixing),
      flavor_(archive_->ReadOptions().flavor),
      frame_ms_(FrameMsOf(flavor_)),
      frame_samples_(FrameSamplesOf(flavor_, mixing_)),
      lead_(LeadFramesOf(effects_, SampleRate(), frame_samples_)),
      interval_(snapshot_interval > 0 ? snapshot_interval : 2 * static_cast<uint64_t>(SampleRate()) / frame_samples_)
{
}

SequenceModel::SequenceModel(std::shared_ptr<const ntdlib::rsar::SoundArchive> archive,
                             std::shared_ptr<const Sequence> sequence, AuxEffects effects, uint64_t snapshot_interval)
    : wii_archive_(std::move(archive)),
      sequence_(std::move(sequence)),
      effects_(std::move(effects)),
      mixing_(Mixing::kStrict),
      flavor_(ntdlib::sar::Flavor::kCafe), // unused on the Wii
      frame_ms_(kWiiFrameMs),
      frame_samples_(Wii::kFrameSamples),
      lead_(LeadFramesOf(effects_, SampleRate(), frame_samples_)),
      interval_(snapshot_interval > 0 ? snapshot_interval : 2 * static_cast<uint64_t>(SampleRate()) / frame_samples_)
{
}

SequenceModel::~SequenceModel() = default;

const Sequence& SequenceModel::GetSequence() const
{
    return *sequence_;
}

int SequenceModel::SampleRate() const
{
    if (wii_archive_)
    {
        return Wii::kSampleRate;
    }

    switch (flavor_)
    {
    case ntdlib::sar::Flavor::kCafe:
        return WiiU::Model::kSampleRate;
    case ntdlib::sar::Flavor::kCtr:
        return mixing_ == Mixing::kEnhanced ? CtrModel::kEnhancedSampleRate : CtrModel::kSampleRate;
    default:
        return Switch::Model::kSampleRate;
    }
}

std::size_t SequenceModel::FrameSamples() const
{
    return frame_samples_;
}

double SequenceModel::TicksPerFrame(double ticks) const
{
    // The ticks in a minute over the frames in a minute. The 3DS's frames don't last a whole number of milliseconds.
    const double frames_per_minute =
        60000.0 * static_cast<double>(frame_ms_.denominator) / static_cast<double>(frame_ms_.numerator);
    const auto tick = static_cast<uint32_t>(std::clamp(ticks, 0.0, static_cast<double>(sequence_->length)));
    return TempoAt(*sequence_, tick) * sequence_->timebase / frames_per_minute;
}

bool SequenceModel::Seek(uint64_t ticks)
{
    if (!started_ && !Start())
    {
        return false;
    }
    if (!playing_)
    {
        return false;
    }

    // The latest snapshot at or before the target that lies before the lead's start, or has mixed since then, starts
    // the run. The first snapshot, at tick 0, always does. The model runs on from where it is when that's between the
    // snapshot and the target, and it lies before the lead's start or has mixed since then.
    const auto target = static_cast<double>(ticks);
    const double lead_start = LeadStartOfTick(target);
    const auto settles = [lead_start](double at, double mixed_from)
    {
        return at <= lead_start || mixed_from <= lead_start;
    };

    const Saved* from = &saved_.front();
    for (const Saved& saved : saved_)
    {
        if (static_cast<double>(saved.ticks) > target)
        {
            break;
        }

        if (settles(static_cast<double>(saved.ticks), saved.mixed_from_ticks))
        {
            from = &saved;
        }
    }
    if (failed_ || frame_ < from->frame || after_ > target || !settles(after_, mixed_from_ticks_))
    {
        Restore(*from);
    }

    // Close to the target, the run mixes throughout. Further away, it mixes from the lead on.
    const double reach = static_cast<double>(std::max(lead_, interval_)) * TicksPerFrame(target);
    const bool close = target - static_cast<double>(model_->TickCount()) <= reach;
    bool reached = false;
    for (;;)
    {
        mixes_ = close || InLeadOfTick(target);
        if (!Step())
        {
            break;
        }
        if (after_ > target)
        {
            reached = true;
            break;
        }
        if (model_->ClockStopped())
        {
            break;
        }
    }
    mixes_ = true;

    return reached;
}

bool SequenceModel::SeekFrame(uint64_t frame)
{
    if (!started_ && !Start())
    {
        return false;
    }
    if (!playing_)
    {
        return false;
    }

    // The latest snapshot at or before the frame that lies before the lead's start, or has mixed since then, starts the
    // run. The first snapshot always does. The model runs on from where it is when that's between the snapshot and the
    // frame, and it lies before the lead's start or has mixed since then. It stays where it is when its last frame is
    // the one.
    const uint64_t lead_start = LeadStart(frame);
    const auto settles = [lead_start](uint64_t at, uint64_t mixed_from)
    {
        return at <= lead_start || mixed_from <= lead_start;
    };

    const Saved* from = &saved_.front();
    for (const Saved& saved : saved_)
    {
        if (saved.frame > frame)
        {
            break;
        }

        if (settles(saved.frame, saved.mixed_from))
        {
            from = &saved;
        }
    }
    if (failed_ || frame_ < from->frame || frame_ > frame + 1 || !settles(frame_, mixed_from_))
    {
        Restore(*from);
    }

    // Close to the frame, the run mixes throughout. Further away, it mixes from the lead on.
    const bool close = frame + 1 - frame_ <= std::max(lead_, interval_);
    bool reached = true;
    while (frame_ <= frame)
    {
        mixes_ = close || InLead(frame);
        if (!Step())
        {
            reached = false;
            break;
        }
    }
    mixes_ = true;

    return reached;
}

bool SequenceModel::RunFrame()
{
    return playing_ && Step();
}

void SequenceModel::SetMixing(bool mixing)
{
    mixes_ = mixing;
}

uint64_t SequenceModel::LeadFrames() const
{
    return lead_;
}

bool SequenceModel::InLead(uint64_t frame) const
{
    return frame_ >= LeadStart(frame);
}

bool SequenceModel::InLeadOfTick(double ticks) const
{
    // A frame that starts less than two frames' ticks before the lead's start mixes. The first frame that mixes then
    // starts at or before the lead's start, unless the tempo more than doubles within a frame.
    const double at = model_ != nullptr ? static_cast<double>(model_->TickCount()) : 0.0;
    return at + 2.0 * TicksPerFrame(at) > LeadStartOfTick(ticks);
}

void SequenceModel::SetMutes(uint32_t tracks)
{
    if (tracks == mutes_)
    {
        return;
    }

    mutes_ = tracks;
    if (playing_)
    {
        ApplyMutes();
    }
}

void SequenceModel::SetBusVolumes(const AuxVolumes& volumes)
{
    if (volumes == bus_volumes_)
    {
        return;
    }

    bus_volumes_ = volumes;
    if (playing_)
    {
        ApplyBusVolumes();
    }
}

std::span<const int16_t> SequenceModel::Output() const
{
    if (!model_)
    {
        return {};
    }

    return model_->Output();
}

std::span<const float> SequenceModel::FloatOutput()
{
    if (!model_)
    {
        return {};
    }

    float_output_.resize(frame_samples_ * 2);
    model_->CopyOutput(float_output_);

    return float_output_;
}

double SequenceModel::Before() const
{
    return before_;
}

double SequenceModel::After() const
{
    return after_;
}

bool SequenceModel::ClockStopped() const
{
    return playing_ && model_->ClockStopped();
}

uint64_t SequenceModel::Frames() const
{
    return frame_;
}

std::array<int32_t, 2> SequenceModel::TrackPeaks(int track) const
{
    if (!model_)
    {
        return {0, 0};
    }

    return model_->TrackPeaks(track);
}

std::array<int32_t, 2> SequenceModel::BusPeaks(int bus) const
{
    if (!model_ || bus < 0 || bus >= static_cast<int>(kAuxBuses))
    {
        return {0, 0};
    }

    return model_->BusPeaks(bus);
}

std::size_t SequenceModel::SnapshotCount() const
{
    return saved_.size();
}

std::array<int16_t, kGameVariables> SequenceModel::Variables() const
{
    if (playing_)
    {
        return model_->Variables();
    }

    std::array<int16_t, kGameVariables> values{};
    values.fill(-1);
    for (const auto& [number, value] : sequence_->start_values)
    {
        if (number >= 0 && number < kGameVariables)
        {
            values[static_cast<std::size_t>(number)] = value;
        }
    }

    return values;
}

bool SequenceModel::Start()
{
    // The effects go on their buses before the sound starts. A game sets up its effects the same way. Damaged data in
    // the archive throws.
    started_ = true;
    try
    {
        if (wii_archive_)
        {
            model_ = std::make_unique<Wii>(
                *wii_archive_,
                sequence_->wii_build.value_or(ntdlib::sequence_export::BuildOfRsarVersion(wii_archive_->Version())),
                effects_);
        }
        else if (flavor_ == ntdlib::sar::Flavor::kCafe)
        {
            model_ = std::make_unique<ModelOf<WiiU>>(*archive_, effects_, mixing_);
        }
        else if (flavor_ == ntdlib::sar::Flavor::kCtr)
        {
            model_ = std::make_unique<CtrConsole>(*archive_, effects_, mixing_);
        }
        else
        {
            model_ = std::make_unique<ModelOf<Switch>>(*archive_, effects_, mixing_);
        }
        playing_ = model_->StartSequence(sequence_->sound);
    }
    catch (const std::exception&)
    {
        playing_ = false;
    }
    if (!playing_)
    {
        return false;
    }

    // The variables take their starting values before the sound's first frame. A game sets them through the sound's
    // handle the same way.
    for (const auto& [number, value] : sequence_->start_values)
    {
        model_->SetVariable(number, value);
    }

    ApplyMutes();
    ApplyBusVolumes();
    model_->MeterTracks();
    model_->MeterBuses();
    saved_.push_back({0, 0, 0, 0.0, model_->Save()});

    return true;
}

bool SequenceModel::Step()
{
    // The effects' tails sound on after the last channel stops. The model runs until a frame comes out silent. With an
    // effect that rings, a frame within ±8 counts as silent (ConsoleModel::Silent).
    if (failed_ || (!model_->IsBusy() && model_->Silent()))
    {
        return false;
    }

    // A snapshot comes before the frame's changes of the variables, and the changes before the frame's ticks. Once the
    // sequence has ended, its tick count stops. The position goes on at the tempo at the end, and the frames mix.
    SaveIfDue();
    ChangeVariables();
    const bool ended = model_->IsFinished();
    const bool mixes = mixes_ || ended;
    before_ = ended ? after_ : static_cast<double>(model_->TickCount());
    model_->SetMixing(mixes);
    try
    {
        model_->RunFrame();
    }
    catch (const std::exception&)
    {
        failed_ = true;
        return false;
    }
    after_ = ended ? before_ + TicksPerFrame(before_) : static_cast<double>(model_->TickCount());
    frame_++;
    if (!mixes)
    {
        mixed_from_ = frame_;
        mixed_from_ticks_ = after_;
    }

    return true;
}

void SequenceModel::SaveIfDue()
{
    // While the sequence plays, a snapshot in each stretch of interval_ frames of the first pass, the first time the
    // model runs it. A settled state takes the place of a snapshot that isn't.
    const uint64_t ticks = model_->TickCount();
    if (model_->IsFinished() || ticks >= sequence_->length)
    {
        return;
    }

    const auto later = std::upper_bound(saved_.begin(), saved_.end(), frame_,
                                        [](uint64_t frame, const Saved& saved) { return frame < saved.frame; });
    Saved& last = *std::prev(later);
    if (last.frame / interval_ == frame_ / interval_)
    {
        if (!Settled(last.frame, last.mixed_from) && Settled(frame_, mixed_from_))
        {
            last = {frame_, ticks, mixed_from_, mixed_from_ticks_, model_->Save()};
        }
        return;
    }

    saved_.insert(later, {frame_, ticks, mixed_from_, mixed_from_ticks_, model_->Save()});

    // Past the limit, the interval doubles, and each new stretch keeps its first snapshot.
    while (saved_.size() > kMaxSnapshots)
    {
        interval_ *= 2;
        std::vector<Saved> kept;
        for (Saved& saved : saved_)
        {
            if (kept.empty() || kept.back().frame / interval_ != saved.frame / interval_)
            {
                kept.push_back(std::move(saved));
            }
        }
        saved_ = std::move(kept);
    }
}

void SequenceModel::ChangeVariables()
{
    for (const VariableChange& change : sequence_->changes)
    {
        if (ChangeFrame(change.time, frame_ms_) == frame_)
        {
            model_->SetVariable(change.variable, change.value);
        }
    }
}

void SequenceModel::Restore(const Saved& saved)
{
    // A snapshot holds the volumes of the buses that it was taken with.
    model_->Restore(*saved.snapshot);
    ApplyBusVolumes();
    frame_ = saved.frame;
    mixed_from_ = saved.mixed_from;
    mixed_from_ticks_ = saved.mixed_from_ticks;
    failed_ = false;
}

uint64_t SequenceModel::LeadStart(uint64_t frame) const
{
    return frame + 1 > lead_ ? frame + 1 - lead_ : 0;
}

double SequenceModel::LeadStartOfTick(double ticks) const
{
    return std::max(ticks - static_cast<double>(lead_) * TicksPerFrame(ticks), 0.0);
}

bool SequenceModel::Settled(uint64_t frame, uint64_t mixed_from) const
{
    return mixed_from == 0 || frame - mixed_from >= lead_;
}

void SequenceModel::ApplyMutes()
{
    for (int track = 0; track < kTracks; track++)
    {
        model_->MuteTrack(track, (mutes_ >> track & 1u) != 0);
    }
}

void SequenceModel::ApplyBusVolumes()
{
    for (std::size_t bus = 0; bus < kAuxBuses; bus++)
    {
        model_->SetBusVolume(static_cast<int>(bus), bus_volumes_[bus]);
    }
}

} // namespace hokuto::song
