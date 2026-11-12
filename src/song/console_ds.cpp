// SPDX-License-Identifier: MIT

// The models of the DS, on NITRO-Composer's driver or on DSE's (song/console_model.h).

#include <algorithm>
#include <cmath>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/dse/player.h"
#include "ntdlib/ntr/archive_player.h"
#include "xsflib/rip/length.h"

#include "song/console_model.h"
#include "song/pcm.h"

namespace hokuto::song
{
namespace
{

using console_model::CopyScaled;

// The DS: ntdlib's models of NITRO-Composer and the DS's sound unit (ntdlib::ntr::ArchivePlayer), with the build of the
// archive's driver. A frame of the driver contains 170.5 samples at 32,728.5 Hz in strict mixing: the output's samples
// don't fall evenly in its frames. The model's frames therefore contain two of the driver's frames each, about 10.42
// ms: 341 samples in strict mixing, and 500 at about 47,989 Hz in enhanced mixing. The DS's players report 32,728 Hz. A
// frame of the driver plays its samples, then runs its commands and its ticks. The first frame plays silence and starts
// the sequence. The position is the player's tick counter in the shared work. The SDK reads it with
// SND_GetPlayerTickCounter. The shared work contains the variables too. A game sets them through
// SND_SetPlayerLocalVariable and SND_SetPlayerGlobalVariable, and a change reaches the driver in its next frame. The
// player mutes and meters a track. The DS has no aux buses. Frames without mixing run the sound unit without its
// output. In enhanced mixing, the output that follows them starts the interpolators again (ntdlib::ntr::SoundUnit). A
// game's output effect plays the mixer's output half its buffer later, about 31 ms with the games' values: a lead of
// 0.5 s fills the buffer again.
class Ds final : public ConsoleModel
{
public:
    Ds(const DsArchive& archive, Mixing mixing, bool dry)
        : archive_(archive),
          player_(archive.archive, OptionsOf(archive, mixing)),
          enhanced_(mixing == Mixing::kEnhanced),
          dry_(dry)
    {
    }

    // The sound starts with the profile of its game, and the profile's commands follow at once. A game sends them
    // through the sound's handle the same way. A sound without a sequence throws. So do a sequence, a bank or a file
    // that the archive doesn't contain.
    bool StartSequence(uint32_t sound) override
    {
        if (sound >= archive_.sounds.size() || !archive_.sounds[sound])
        {
            throw std::invalid_argument("The archive has no sequence " + std::to_string(sound) + ".");
        }

        DsProfile profile = DsProfileOf(archive_, sound);
        if (dry_)
        {
            profile = DryDsProfile(std::move(profile));
        }

        player_.Start(*archive_.sounds[sound], profile.settings);
        if (!profile.commands.empty())
        {
            player_.Send(std::move(profile.commands));
        }

        return true;
    }

    void SetVariable(int number, int16_t value) override
    {
        ntdlib::ntr::Variables variables;
        if (number >= 0 && number < kFirstGlobal)
        {
            variables.local[static_cast<std::size_t>(number)] = value;
        }
        else if (number >= kFirstGlobal && number < kGameVariables)
        {
            variables.global[static_cast<std::size_t>(number - kFirstGlobal)] = value;
        }
        else
        {
            return;
        }

        player_.Send(ntdlib::ntr::VariableCommands(variables));
    }

    std::array<int16_t, kGameVariables> Variables() const override
    {
        std::array<int16_t, kGameVariables> values{};
        const std::span<const uint8_t> memory = player_.MainMemory();
        for (int i = 0; i < kGameVariables; i++)
        {
            const uint32_t address = i < kFirstGlobal ? kPlayerWork + 2 * static_cast<uint32_t>(i)
                                                      : kGlobalVariables + 2 * static_cast<uint32_t>(i - kFirstGlobal);
            values[static_cast<std::size_t>(i)] = static_cast<int16_t>(xsflib::LoadLe16(&memory[address]));
        }

        return values;
    }

    uint32_t TickCount() const override
    {
        return xsflib::LoadLe32(&player_.MainMemory()[kPlayerWork + kTickCounter]);
    }

    // The player starts in the model's first frame.
    bool IsFinished() const override
    {
        return frames_ > 0 && !player_.Driver().GetPlayer(0).active;
    }

    bool ClockStopped() const override
    {
        const ntdlib::ntr::snd::Player& player = player_.Driver().GetPlayer(0);
        return player.active && ((static_cast<uint32_t>(player.tempo) * player.tempo_ratio) >> 8) == 0;
    }

    bool IsBusy() const override
    {
        return player_.Busy();
    }

    void RunFrame() override
    {
        peaks_ = {};
        const std::size_t samples = enhanced_ ? kEnhancedSamples : kStrictSamples;
        output_.clear();
        float_output_.clear();
        if (!mixing_)
        {
            for (int frame = 0; frame < kDriverFrames; frame++)
            {
                player_.RunFrame();
            }

            output_.assign(2 * samples, 0);
            float_output_.assign(enhanced_ ? 2 * samples : 0, 0.0f);
        }
        else if (enhanced_)
        {
            for (int frame = 0; frame < kDriverFrames; frame++)
            {
                player_.RunFrame(float_output_);
                TakePeaks();
            }

            output_.resize(float_output_.size());
            for (std::size_t i = 0; i < float_output_.size(); i++)
            {
                output_[i] = ToPcm16(float_output_[i]);
            }
        }
        else
        {
            std::vector<ntdlib::ntr::SoundOutput> out;
            for (int frame = 0; frame < kDriverFrames; frame++)
            {
                player_.RunFrame(out);
                TakePeaks();
            }

            ntdlib::ntr::AppendPcm16(out, output_);
        }

        if (output_.size() != 2 * samples)
        {
            throw std::runtime_error("The DS sound unit model produced " + std::to_string(output_.size() / 2) +
                                     " samples in a frame of " + std::to_string(samples) + ".");
        }

        frames_++;
    }

    // In enhanced mixing, a channel that stops rings out through the interpolator after the driver has let it go.
    bool Silent() const override
    {
        return !xsflib::rip::Audible(output_);
    }

    void SetMixing(bool mixing) override
    {
        mixing_ = mixing;
    }

    void MuteTrack(int track, bool mute) override
    {
        if (track >= 0 && track < ntdlib::ntr::snd::kTracksPerPlayer)
        {
            player_.MuteTrack(0, track, mute);
        }
    }

    void MeterTracks() override
    {
        player_.MeterTracks(0);
    }

    // The peaks of the driver's frames that the model's frame ran.
    std::array<int32_t, 2> TrackPeaks(int track) const override
    {
        return track >= 0 && track < ntdlib::ntr::snd::kTracksPerPlayer ? peaks_[static_cast<std::size_t>(track)]
                                                                        : std::array<int32_t, 2>{};
    }

    void SetBusVolume(int, float) override
    {
    }

    void MeterBuses() override
    {
    }

    std::array<int32_t, 2> BusPeaks(int) const override
    {
        return {0, 0};
    }

    std::span<const int16_t> Output() const override
    {
        return output_;
    }

    void CopyOutput(std::span<float> out) const override
    {
        CopyScaled(enhanced_ ? std::span<const float>(float_output_) : std::span<const float>(), output_, out);
    }

    std::unique_ptr<const ConsoleSnapshot> Save() const override
    {
        auto saved = std::make_shared<const Player::Snapshot>(player_.Save(last_ ? &*last_ : nullptr));
        last_ = saved;
        auto snapshot = std::make_unique<Snapshot>();
        snapshot->saved = std::move(saved);
        snapshot->output = output_;
        snapshot->float_output = float_output_;
        snapshot->frames = frames_;

        return snapshot;
    }

    // The mutes and the meters hold through a restore.
    void Restore(const ConsoleSnapshot& snapshot) override
    {
        const auto& saved = static_cast<const Snapshot&>(snapshot);
        player_.Restore(*saved.saved);
        output_ = saved.output;
        float_output_ = saved.float_output;
        frames_ = saved.frames;
    }

private:
    using Player = ntdlib::ntr::ArchivePlayer;

    // The driver's frames in each of the model's frames, and the samples that they hold.
    static constexpr int kDriverFrames = 2;
    static constexpr std::size_t kStrictSamples = 341;
    static constexpr std::size_t kEnhancedSamples = kDriverFrames * ntdlib::ntr::kEnhancedFrameSamples;
    static_assert(kDriverFrames * ntdlib::ntr::snd::kFrameClocks == kStrictSamples * 1024);

    // The places in main memory of player 0's variables and tick counter, and of the global variables, in the shared
    // work (SNDSharedWork).
    static constexpr uint32_t kSharedWork = Player::kSharedWork - Player::kMainMemory;
    static constexpr uint32_t kPlayerWork = kSharedWork + 0x20;
    static constexpr uint32_t kTickCounter = 0x20;
    static constexpr uint32_t kGlobalVariables = kSharedWork + 0x260;

    struct Snapshot final : ConsoleSnapshot
    {
        std::shared_ptr<const Player::Snapshot> saved;
        std::vector<int16_t> output;
        std::vector<float> float_output;
        uint64_t frames = 0;
    };

    static ntdlib::ntr::ArchivePlayerOptions OptionsOf(const DsArchive& archive, Mixing mixing)
    {
        ntdlib::ntr::ArchivePlayerOptions options;
        options.build = archive.build;
        options.enhanced = mixing == Mixing::kEnhanced;
        return options;
    }

    void TakePeaks()
    {
        for (std::size_t t = 0; t < peaks_.size(); t++)
        {
            const std::array<int32_t, 2> peaks = player_.TrackPeaks(static_cast<int>(t));
            peaks_[t] = {std::max(peaks_[t][0], peaks[0]), std::max(peaks_[t][1], peaks[1])};
        }
    }

    const DsArchive& archive_;
    Player player_;
    bool enhanced_;
    bool dry_;
    bool mixing_ = true;
    std::vector<int16_t> output_;     // the last frame's output
    std::vector<float> float_output_; // the same in enhanced mixing, in floating point
    uint64_t frames_ = 0;             // the model's frames since the start
    std::array<std::array<int32_t, 2>, ntdlib::ntr::snd::kTracksPerPlayer> peaks_{};
    mutable std::shared_ptr<const Player::Snapshot> last_; // the latest snapshot
};

// The length of the model's frames on the DS (see Ds).
static_assert(2 * ntdlib::ntr::snd::kFrameClocks * 1000 * kDsFrameMs.denominator ==
              kDsFrameMs.numerator * uint64_t{ntdlib::ntr::kSystemClockHz});

// DSE on the DS: ntdlib's models of the DSE driver, the sound driver of the DS and the sound unit
// (ntdlib::dse::Player), with the build of the image's driver. DSE's ticks come every 5,236 ticks of the OS timer:
// 335,104 system clocks, or 9.999 ms. The sound driver's frames come every 174,592 system clocks. Each frame of the
// model contains two frames of the sound driver. So does each frame of Ds. The player runs the frames and the ticks
// that come in that time in their order. The player starts the song or the sound effect at once, and DSE plays it from
// its first tick. The position is the sequence's tick count (dse::Driver::SequenceTick). DSE has no variables, and its
// clock never stops: a tempo of 0 counts as 1 beat a minute. The player mutes and meters the tracks of the song or the
// sound effect. The DS has no aux buses.
class Dse final : public ConsoleModel
{
public:
    Dse(const DseSet& set, Mixing mixing)
        : set_(set), player_(OptionsOf(set, mixing)), enhanced_(mixing == Mixing::kEnhanced)
    {
    }

    // A number past the image's sounds throws. So do a song or a bank that DSE's driver can't play.
    bool StartSequence(uint32_t sound) override
    {
        if (sound >= set_.sounds.size())
        {
            throw std::invalid_argument("The image has no sound " + std::to_string(sound) + ".");
        }

        files_ = set_.sounds[sound].Files();
        player_.LoadBanks(files_);
        return player_.Start(files_);
    }

    void SetVariable(int, int16_t) override
    {
    }

    std::array<int16_t, kGameVariables> Variables() const override
    {
        std::array<int16_t, kGameVariables> values{};
        values.fill(-1);

        return values;
    }

    uint32_t TickCount() const override
    {
        return player_.Driver().SequenceTick();
    }

    bool IsFinished() const override
    {
        return frames_ > 0 && !player_.Driver().SequencePlays();
    }

    bool ClockStopped() const override
    {
        return false;
    }

    bool IsBusy() const override
    {
        return player_.Busy();
    }

    void RunFrame() override
    {
        const std::size_t samples = enhanced_ ? kEnhancedSamples : kStrictSamples;
        output_.clear();
        float_output_.clear();
        if (!mixing_)
        {
            player_.Run(kFrameClocks);
            output_.assign(2 * samples, 0);
            float_output_.assign(enhanced_ ? 2 * samples : 0, 0.0f);
        }
        else if (enhanced_)
        {
            player_.Run(kFrameClocks, float_output_);
            output_.resize(float_output_.size());
            for (std::size_t i = 0; i < float_output_.size(); i++)
            {
                output_[i] = ToPcm16(float_output_[i]);
            }
        }
        else
        {
            std::vector<ntdlib::ntr::SoundOutput> out;
            player_.Run(kFrameClocks, out);
            ntdlib::ntr::AppendPcm16(out, output_);
        }

        if (output_.size() != 2 * samples)
        {
            throw std::runtime_error("The DS sound unit model produced " + std::to_string(output_.size() / 2) +
                                     " samples in a frame of " + std::to_string(samples) + ".");
        }

        frames_++;
    }

    // In enhanced mixing, a channel that stops rings out through the interpolator after the driver has let it go.
    bool Silent() const override
    {
        return !xsflib::rip::Audible(output_);
    }

    void SetMixing(bool mixing) override
    {
        mixing_ = mixing;
    }

    void MuteTrack(int track, bool mute) override
    {
        if (track >= 0 && track < Player::kHostTracks)
        {
            player_.MuteTrack(track, mute);
        }
    }

    void MeterTracks() override
    {
        player_.MeterTracks(true);
    }

    // The peaks of the model's last frame.
    std::array<int32_t, 2> TrackPeaks(int track) const override
    {
        return player_.TrackPeaks(track);
    }

    void SetBusVolume(int, float) override
    {
    }

    void MeterBuses() override
    {
    }

    std::array<int32_t, 2> BusPeaks(int) const override
    {
        return {0, 0};
    }

    std::span<const int16_t> Output() const override
    {
        return output_;
    }

    void CopyOutput(std::span<float> out) const override
    {
        CopyScaled(enhanced_ ? std::span<const float>(float_output_) : std::span<const float>(), output_, out);
    }

    std::unique_ptr<const ConsoleSnapshot> Save() const override
    {
        auto snapshot = std::make_unique<Snapshot>();
        snapshot->saved = std::make_shared<const Player::Snapshot>(player_.Save());
        snapshot->output = output_;
        snapshot->float_output = float_output_;
        snapshot->frames = frames_;

        return snapshot;
    }

    // The mutes and the meters hold through a restore.
    void Restore(const ConsoleSnapshot& snapshot) override
    {
        const auto& saved = static_cast<const Snapshot&>(snapshot);
        player_.Restore(*saved.saved);
        output_ = saved.output;
        float_output_ = saved.float_output;
        frames_ = saved.frames;
    }

private:
    using Player = ntdlib::dse::Player;

    static_assert(SequenceModel::kTracks == Player::kHostTracks);

    // The system clocks of the model's frames: two of the sound driver's frames, and the samples that they hold.
    static constexpr uint64_t kFrameClocks = 2 * ntdlib::ntr::snd::kFrameClocks;
    static constexpr std::size_t kStrictSamples = 341;
    static constexpr std::size_t kEnhancedSamples = 2 * ntdlib::ntr::kEnhancedFrameSamples;
    static_assert(kFrameClocks == kStrictSamples * 1024);

    struct Snapshot final : ConsoleSnapshot
    {
        std::shared_ptr<const Player::Snapshot> saved;
        std::vector<int16_t> output;
        std::vector<float> float_output;
        uint64_t frames = 0;
    };

    static ntdlib::dse::PlayerOptions OptionsOf(const DseSet& set, Mixing mixing)
    {
        ntdlib::dse::PlayerOptions options;
        options.build = set.build;
        options.enhanced = mixing == Mixing::kEnhanced;
        return options;
    }

    const DseSet& set_;
    Player player_;
    bool enhanced_;
    bool mixing_ = true;
    ntdlib::dse::SequenceFiles files_; // the files of the sound that plays
    std::vector<int16_t> output_;      // the last frame's output
    std::vector<float> float_output_;  // the same in enhanced mixing, in floating point
    uint64_t frames_ = 0;              // the model's frames since the start
};

} // namespace

namespace console_model
{

std::size_t DsFrameSamples(Mixing mixing)
{
    return mixing == Mixing::kEnhanced ? 2 * ntdlib::ntr::kEnhancedFrameSamples : 341;
}

int DsSampleRate(Mixing mixing)
{
    return mixing == Mixing::kEnhanced ? static_cast<int>(std::lround(ntdlib::ntr::kEnhancedRate))
                                       : static_cast<int>(ntdlib::ntr::kOutputRate);
}

std::unique_ptr<ConsoleModel> NewDsModel(const DsArchive& archive, Mixing mixing, bool dry)
{
    return std::make_unique<Ds>(archive, mixing, dry);
}

std::unique_ptr<ConsoleModel> NewDseModel(const DseSet& set, Mixing mixing)
{
    return std::make_unique<Dse>(set, mixing);
}

} // namespace console_model

} // namespace hokuto::song
