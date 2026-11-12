// SPDX-License-Identifier: MIT

// The model of the Wii, on NW4R SND and AX (song/console_model.h).

#include <algorithm>
#include <memory>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

#include "ntdlib/rvl/ax/pb.h"
#include "ntdlib/rvl/snd/engine.h"
#include "xsflib/rip/length.h"

#include "song/console_model.h"

namespace hokuto::song
{
namespace
{

using console_model::CopyScaled;

// The Wii: ntdlib's model of NW4R SND and AX, with ntdlib's model of AX's microcode of the game's build in place of the
// DSP. The model follows the sequence's build of NW4R SND. So does the performer. The effects of the Wii go on the aux
// buses, and run on that build's AXFX library. Every frame mixes. A track's meter adds up its voices' mix into the main
// bus (ntdlib's ArchiveModel::MeterTracks). A mute sets the track's volume to 0 through the sound's handle
// (SeqPlayer::SetTrackVolume). The track plays on. Unlike the other consoles' mutes, this one reaches the engine: a
// muted note can end sooner than its release would sound.
class Wii final : public ConsoleModel
{
public:
    using Model = ntdlib::rvl::snd::ArchiveModel;

    // AX mixes 96 stereo samples at 32 kHz in each frame of 3 ms.
    static constexpr int kSampleRate = 32'000;
    static constexpr auto kFrameSamples = static_cast<std::size_t>(ntdlib::rvl::ax::kSamplesPerFrame);

    Wii(const ntdlib::rsar::SoundArchive& archive, const ntdlib::SndBuild& build, const AuxEffects& effects)
        : model_(std::make_unique<Model>()), setups_(WiiEffectSetups(effects)), ring_limit_(WiiRingingFrames(effects))
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

    // A delay's echo can come back after a quiet gap. With an effect on a bus, the model stays busy after the output
    // last had sound (louder than ±8), for a second and the longest time that a bus's effects take to give their input
    // back (WiiRingingFrames).
    bool IsBusy() const override
    {
        return model_->engine->IsBusy() || (model_->engine->HasEffects() && ring_left_ > 0);
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
            throw std::runtime_error("The AX microcode model rejected a frame.");
        }

        ring_left_ = xsflib::rip::Audible(output_) ? ring_limit_ : std::max(ring_left_ - 1, 0);
    }

    // A delay that feeds back can leave samples of ±1 for ever. With an effect on a bus, the output therefore ends
    // within ±8. So does the 3DS's output.
    bool Silent() const override
    {
        const int level = model_->engine->HasEffects() ? xsflib::rip::kSilenceLevel : 0;
        return console_model::QuietWithin(output_, level);
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

        return std::make_unique<const Snapshot>(std::move(saved), output_, ring_left_);
    }

    // A snapshot holds the tracks' volumes from the time it was taken. The mutes hold through a restore.
    void Restore(const ConsoleSnapshot& snapshot) override
    {
        const auto& saved = static_cast<const Snapshot&>(snapshot);
        model_->Restore(*saved.saved);
        output_ = saved.output;
        ring_left_ = saved.ring_left;
        player_->SetTrackVolume(mutes_, 0.0f);
        player_->SetTrackVolume(~mutes_, 1.0f);
    }

private:
    struct Snapshot final : ConsoleSnapshot
    {
        Snapshot(std::shared_ptr<const Model::Snapshot> model, std::vector<int16_t> last, int ringing)
            : saved(std::move(model)), output(std::move(last)), ring_left(ringing)
        {
        }

        std::shared_ptr<const Model::Snapshot> saved;
        std::vector<int16_t> output;
        int ring_left;
    };

    std::unique_ptr<Model> model_;
    std::vector<ntdlib::rvl::snd::EffectSetup> setups_; // the effects on the buses
    int ring_limit_;                                    // the quiet frames after which the effects have stopped ringing
    ntdlib::rvl::snd::SeqPlayer* player_ = nullptr;
    std::vector<int16_t> output_;                         // the last frame's output
    mutable std::shared_ptr<const Model::Snapshot> last_; // the latest snapshot
    uint32_t mutes_ = 0;                                  // a bit for each muted track
    int ring_left_ = 0;                                   // the frames left until a silent output ends the sound
    std::array<uint16_t, kAuxBuses> returns_ = {ntdlib::rvl::ax::kMaxVolume, ntdlib::rvl::ax::kMaxVolume,
                                                ntdlib::rvl::ax::kMaxVolume}; // the buses' return volumes
};

} // namespace

namespace console_model
{

std::unique_ptr<ConsoleModel> NewWiiModel(const ntdlib::rsar::SoundArchive& archive, const ntdlib::SndBuild& build,
                                          const AuxEffects& effects)
{
    return std::make_unique<Wii>(archive, build, effects);
}

int WiiSampleRate()
{
    return Wii::kSampleRate;
}

std::size_t WiiFrameSamples()
{
    return Wii::kFrameSamples;
}

} // namespace console_model

} // namespace hokuto::song
