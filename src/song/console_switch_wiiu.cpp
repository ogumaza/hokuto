// SPDX-License-Identifier: MIT

// The models of the Switch and the Wii U, on nn::atk and on nw::snd (song/console_model.h).

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <span>
#include <utility>
#include <variant>
#include <vector>

#include "ntdlib/cafe/snd/engine.h"
#include "ntdlib/nx/atk/effect.h"
#include "ntdlib/nx/atk/engine.h"
#include "ntdlib/nx/audio/i3dl2.h"
#include "xsflib/rip/length.h"

#include "song/console_model.h"

namespace hokuto::song
{
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
          ring_limit_(Console::RingingFrames(effects)),
          quiet_frames_(ring_limit_)
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
        return console_model::GameVariables(player_->LocalVariables(), model_->engine->GlobalVariables());
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
        return model_->engine->IsBusy() || quiet_frames_ < ring_limit_;
    }

    void RunFrame() override
    {
        model_->engine->RunFrame();
        if (ring_limit_ > 0)
        {
            quiet_frames_ =
                xsflib::rip::Audible(Console::Output(*model_)) ? 0 : std::min(quiet_frames_ + 1, ring_limit_);
        }
    }

    // A delay that feeds back can leave samples of ±1 for ever. With an effect that may ring, the output therefore ends
    // within ±8. The 3DS's output ends the same way.
    bool Silent() const override
    {
        const std::span<const int16_t> output = Console::Output(*model_);
        const int level = ring_limit_ > 0 ? xsflib::rip::kSilenceLevel : 0;
        return console_model::QuietWithin(output, level);
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
    int ring_limit_;   // the quiet frames after which the effects have stopped ringing, or 0
    int quiet_frames_; // the frames since the output last had sound, up to ring_limit_
};

// A change's frame (ChangeFrame) counts the model's frames.
static_assert(Switch::Model::kFrameSamples * 1000 ==
              FrameMsOf(ntdlib::sar::Flavor::kNx).numerator * static_cast<uint32_t>(Switch::Model::kSampleRate));
static_assert(WiiU::Model::kFrameSamples * 1000 ==
              FrameMsOf(ntdlib::sar::Flavor::kCafe).numerator * static_cast<uint32_t>(WiiU::Model::kSampleRate));

} // namespace

namespace console_model
{

std::unique_ptr<ConsoleModel> NewSwitchModel(const ntdlib::sar::SoundArchive& archive, const AuxEffects& effects,
                                             Mixing mixing)
{
    return std::make_unique<ModelOf<Switch>>(archive, effects, mixing);
}

std::unique_ptr<ConsoleModel> NewWiiUModel(const ntdlib::sar::SoundArchive& archive, const AuxEffects& effects,
                                           Mixing mixing)
{
    return std::make_unique<ModelOf<WiiU>>(archive, effects, mixing);
}

} // namespace console_model

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

} // namespace hokuto::song
