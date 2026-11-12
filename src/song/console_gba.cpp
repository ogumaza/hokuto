// SPDX-License-Identifier: MIT

// The model of the GBA, on MP2K's driver or on another driver's (song/console_model.h).

#include <algorithm>
#include <memory>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

#include "ntdlib/agb/gba.h"
#include "ntdlib/mp2k/archive_player.h"
#include "xsflib/rip/length.h"

#include "song/console_model.h"
#include "song/pcm.h"

namespace hokuto::song
{
namespace
{

using console_model::CopyScaled;

// The GBA: ntdlib's models of the MP2K driver and the GBA sound hardware (ntdlib::mp2k::ArchivePlayer). A frame of the
// console contains 548.625 samples at 32,768 Hz, or 1,097.25 at 65,536 Hz: the output's samples don't fall evenly in
// the display's frames. In strict mixing, the model's frames therefore last 15.625 ms each, a 64th of the output's rate
// in samples, and each runs the console's frames that its samples require. The samples of a console's frame that the
// model's frame doesn't take go to the next. The position is the driver's tick count after the console's frames that
// ran. In enhanced mixing, a frame of the model is a frame of the console: 804 samples at about 48,022 Hz. Every frame
// mixes. The player mutes and meters a track. MP2K has no aux buses and no variables that a game sets through a handle.
// The memory that the memacc commands of a song read belongs to the song. The GBA's other drivers (song/driver.h) play
// the same way, on their ArchivePlayers.
template <typename Driver>
class Agb final : public ConsoleModel
{
public:
    using Player = typename Driver::Player;

    Agb(const typename Driver::Rom& rom, Mixing mixing, std::size_t frame_samples)
        : rom_(rom),
          player_(Driver::MakePlayer(rom)),
          enhanced_(mixing == Mixing::kEnhanced),
          rate_(enhanced_ ? ntdlib::agb::kEnhancedRate : Driver::Rate(rom)),
          frame_samples_(frame_samples)
    {
    }

    // Leaves the driver's echo out from the next start on (DriverPlayer::SetEcho). Only the GBA's other drivers have
    // it. MP2K's mixer makes a reverb instead, and a song's header sets it.
    void LeaveOutEcho()
    {
        Driver::LeaveOutEcho(player_);
    }

    // A song whose header can't be read, or whose music player plays none of its tracks, throws. So does a driver whose
    // output runs at another rate than the ROM's settings give.
    bool StartSequence(uint32_t sound) override
    {
        Driver::Start(player_, rom_, sound, enhanced_);
        return player_.Rate() == rate_;
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
        return Driver::Tick(player_);
    }

    void SetArrangement(const Sequence& sequence) override
    {
        Driver::SetArrangement(player_, sequence);
    }

    bool IsFinished() const override
    {
        return Driver::Finished(player_);
    }

    bool ClockStopped() const override
    {
        return false;
    }

    // The samples that a model's frame left over can hold the end of the sound. They play in the next frames.
    bool IsBusy() const override
    {
        return player_.Busy() || flush_frames_ > 0;
    }

    void RunFrame() override
    {
        peaks_ = {};
        const std::size_t wanted = 2 * frame_samples_;
        if (enhanced_)
        {
            while (float_pending_.size() < wanted)
            {
                const std::size_t before = float_pending_.size();
                player_.RunFrame(float_pending_);
                TakePeaks();
                if (float_pending_.size() == before)
                {
                    throw std::runtime_error("The GBA sound hardware model produced no samples.");
                }
            }

            float_output_.assign(float_pending_.begin(), float_pending_.begin() + static_cast<std::ptrdiff_t>(wanted));
            float_pending_.erase(float_pending_.begin(), float_pending_.begin() + static_cast<std::ptrdiff_t>(wanted));
            output_.resize(wanted);
            for (std::size_t i = 0; i < wanted; i++)
            {
                output_[i] = ToPcm16(float_output_[i]);
            }
        }
        else
        {
            while (pending_.size() < wanted)
            {
                const std::size_t before = pending_.size();
                player_.RunFrame(pending_);
                TakePeaks();
                if (pending_.size() == before)
                {
                    throw std::runtime_error("The GBA sound hardware model produced no samples.");
                }
            }

            output_.assign(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(wanted));
            pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(wanted));
        }

        // A frame of the console lasts less than two frames of the model.
        flush_frames_ = player_.Busy() ? 2 : std::max(flush_frames_ - 1, 0);
        TakeQuiet();
    }

    // The player says when the console's sound has ended (IsBusy). A PSG channel that stops can leave its level in the
    // output. In enhanced mixing, the reverb's tail rings on below the console's steps, and the reverb feeds back the
    // output of a pass of the mixer's buffer before: 16 of the console's frames at most. Camelot's echo feeds back the
    // output of 4 frames before. The sound therefore counts as silent once each side has stayed within ±8 of a level
    // for a frame in strict mixing, and for 17 frames in enhanced mixing.
    bool Silent() const override
    {
        return quiet_frames_ >= (enhanced_ ? kEnhancedQuietFrames : 1);
    }

    void SetMixing(bool) override
    {
    }

    void MuteTrack(int track, bool mute) override
    {
        if (track >= 0 && track < Driver::kTracks)
        {
            player_.MuteTrack(track, mute);
        }
    }

    void MeterTracks() override
    {
        player_.MeterTracks(true);
    }

    // The peaks of the console's frames that the model's frame ran.
    std::array<int32_t, 2> TrackPeaks(int track) const override
    {
        return track >= 0 && track < Driver::kTracks ? peaks_[static_cast<std::size_t>(track)]
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
        auto saved = std::make_shared<const typename Player::Snapshot>(player_.Save(last_ ? &*last_ : nullptr));
        last_ = saved;
        auto snapshot = std::make_unique<Snapshot>();
        snapshot->saved = std::move(saved);
        snapshot->pending = pending_;
        snapshot->float_pending = float_pending_;
        snapshot->output = output_;
        snapshot->float_output = float_output_;
        snapshot->flush_frames = flush_frames_;
        snapshot->quiet_frames = quiet_frames_;
        snapshot->quiet_low = quiet_low_;
        snapshot->quiet_high = quiet_high_;

        return snapshot;
    }

    // The mutes and the meters hold through a restore.
    void Restore(const ConsoleSnapshot& snapshot) override
    {
        const auto& saved = static_cast<const Snapshot&>(snapshot);
        player_.Restore(*saved.saved);
        pending_ = saved.pending;
        float_pending_ = saved.float_pending;
        output_ = saved.output;
        float_output_ = saved.float_output;
        flush_frames_ = saved.flush_frames;
        quiet_frames_ = saved.quiet_frames;
        quiet_low_ = saved.quiet_low;
        quiet_high_ = saved.quiet_high;
    }

private:
    // The model's frames, each a console's frame, that the enhanced mode's output stays quiet for before the sound
    // counts as silent: a pass of the mixer's buffer, and one more.
    static constexpr int kEnhancedQuietFrames = 17;

    struct Snapshot final : ConsoleSnapshot
    {
        std::shared_ptr<const typename Player::Snapshot> saved;
        std::vector<int16_t> pending;
        std::vector<float> float_pending;
        std::vector<int16_t> output;
        std::vector<float> float_output;
        int flush_frames = 0;
        int quiet_frames = 0;
        std::array<int16_t, 2> quiet_low{};
        std::array<int16_t, 2> quiet_high{};
    };

    // Counts the last frame in the run of frames whose output stays within ±8 of a level on each side, or starts a new
    // run.
    void TakeQuiet()
    {
        std::array<int16_t, 2> low{};
        std::array<int16_t, 2> high{};
        for (std::size_t side = 0; side < 2 && side < output_.size(); side++)
        {
            low[side] = output_[side];
            high[side] = output_[side];
            for (std::size_t i = side; i < output_.size(); i += 2)
            {
                low[side] = std::min(low[side], output_[i]);
                high[side] = std::max(high[side], output_[i]);
            }
        }

        const auto quiet = [](const std::array<int16_t, 2>& lows, const std::array<int16_t, 2>& highs)
        {
            return highs[0] - lows[0] <= 2 * xsflib::rip::kSilenceLevel &&
                   highs[1] - lows[1] <= 2 * xsflib::rip::kSilenceLevel;
        };
        const std::array<int16_t, 2> run_low = {std::min(low[0], quiet_low_[0]), std::min(low[1], quiet_low_[1])};
        const std::array<int16_t, 2> run_high = {std::max(high[0], quiet_high_[0]), std::max(high[1], quiet_high_[1])};
        if (quiet_frames_ > 0 && quiet(run_low, run_high))
        {
            quiet_frames_++;
            quiet_low_ = run_low;
            quiet_high_ = run_high;
        }
        else if (quiet(low, high))
        {
            quiet_frames_ = 1;
            quiet_low_ = low;
            quiet_high_ = high;
        }
        else
        {
            quiet_frames_ = 0;
        }
    }

    void TakePeaks()
    {
        for (std::size_t t = 0; t < peaks_.size(); t++)
        {
            const std::array<int32_t, 2> peaks = player_.TrackPeaks(static_cast<int>(t));
            peaks_[t] = {std::max(peaks_[t][0], peaks[0]), std::max(peaks_[t][1], peaks[1])};
        }
    }

    const typename Driver::Rom& rom_;
    Player player_;
    bool enhanced_;
    int rate_; // the output's rate
    std::size_t frame_samples_;
    std::vector<int16_t> pending_;        // the samples that the model's frames haven't taken yet
    std::vector<float> float_pending_;    // the same in enhanced mixing
    std::vector<int16_t> output_;         // the last frame's output
    std::vector<float> float_output_;     // the same in enhanced mixing, in floating point
    int flush_frames_ = 0;                // the frames that may still hold the sound's end
    int quiet_frames_ = 0;                // the last frames whose output stays within ±8 of a level on each side
    std::array<int16_t, 2> quiet_low_{};  // the lowest sample of those frames on each side
    std::array<int16_t, 2> quiet_high_{}; // the highest
    std::array<std::array<int32_t, 2>, Driver::kTracks> peaks_{};
    mutable std::shared_ptr<const typename Player::Snapshot> last_; // the latest snapshot
};

// MusicPlayer2000 on the model of the GBA.
struct Mp2kDriver
{
    using Rom = GbaRom;
    using Player = ntdlib::mp2k::ArchivePlayer;
    static constexpr int kTracks = ntdlib::mp2k::kMaxTracks;

    static int Rate(const Rom& rom)
    {
        return GbaRate(rom);
    }

    static Player MakePlayer(const Rom& rom)
    {
        return Player(rom.rom, rom.info);
    }

    static void Start(Player& player, const Rom&, uint32_t sound, bool enhanced)
    {
        ntdlib::mp2k::ArchivePlayerOptions options;
        options.enhanced = enhanced;
        player.Start(static_cast<int>(sound), options);
    }

    static uint32_t Tick(const Player& player)
    {
        return static_cast<uint32_t>(player.Driver().Tick());
    }

    // MP2K's clock counts the arrangement's ticks.
    static void SetArrangement(Player&, const Sequence&)
    {
    }

    static bool Finished(const Player& player)
    {
        return player.Driver().TracksEnded();
    }
};

using Gba = Agb<Mp2kDriver>;

// A driver's player (song/driver.h) in the shape of ntdlib's ArchivePlayer, for Agb.
class BoxedPlayer
{
public:
    struct Snapshot
    {
        std::shared_ptr<const DriverPlayer::Snapshot> saved;
    };

    explicit BoxedPlayer(std::unique_ptr<DriverPlayer> player) : player_(std::move(player))
    {
    }

    DriverPlayer& Player()
    {
        return *player_;
    }

    const DriverPlayer& Player() const
    {
        return *player_;
    }

    int Rate() const
    {
        return player_->Rate();
    }

    bool Busy() const
    {
        return player_->Busy();
    }

    void RunFrame(std::vector<int16_t>& out)
    {
        player_->RunFrame(out);
    }

    void RunFrame(std::vector<float>& out)
    {
        player_->RunFrame(out);
    }

    void MuteTrack(int track, bool mute)
    {
        if (track < player_->Tracks())
        {
            player_->MuteTrack(track, mute);
        }
    }

    void MeterTracks(bool meter)
    {
        player_->MeterTracks(meter);
    }

    std::array<int32_t, 2> TrackPeaks(int track) const
    {
        return track < player_->Tracks() ? player_->TrackPeaks(track) : std::array<int32_t, 2>{};
    }

    Snapshot Save(const Snapshot* previous) const
    {
        return Snapshot{player_->Save(previous != nullptr ? previous->saved.get() : nullptr)};
    }

    void Restore(const Snapshot& snapshot)
    {
        player_->Restore(*snapshot.saved);
    }

private:
    std::unique_ptr<DriverPlayer> player_;
};

// One of the GBA's other drivers on the model of the GBA (song/driver.h). A driver plays 16 tracks at most.
struct OtherDriver
{
    using Rom = DriverRom;
    using Player = BoxedPlayer;
    static constexpr int kTracks = 16;

    static int Rate(const Rom& rom)
    {
        return rom.Rate();
    }

    static Player MakePlayer(const Rom& rom)
    {
        return Player(rom.MakePlayer());
    }

    static void Start(Player& player, const Rom&, uint32_t sound, bool enhanced)
    {
        player.Player().Start(sound, enhanced);
    }

    static void LeaveOutEcho(Player& player)
    {
        player.Player().SetEcho(false);
    }

    static uint32_t Tick(const Player& player)
    {
        return player.Player().Tick();
    }

    static void SetArrangement(Player& player, const Sequence& sequence)
    {
        player.Player().SetArrangement(sequence);
    }

    static bool Finished(const Player& player)
    {
        return player.Player().Finished();
    }
};

using DriverConsole = Agb<OtherDriver>;

} // namespace

namespace console_model
{

// The frames a second of the model in strict mixing: each lasts 125 / 8 ms.
constexpr int kStrictFramesASecond = 64;

FrameMs GbaFrameMs(Mixing mixing)
{
    // 280,896 cycles at 16,777,216 Hz, or 548,625 / 32,768 ms. A 64th of a second is 125 / 8 ms.
    static_assert(ntdlib::agb::kFrameCycles * 1000 * 32768 == 548625 * ntdlib::agb::kSecondCycles);
    return mixing == Mixing::kEnhanced ? kGbaFrameMs : FrameMs{125, 8};
}

std::size_t GbaFrameSamples(const GbaRom& rom, Mixing mixing)
{
    return mixing == Mixing::kEnhanced ? ntdlib::agb::kEnhancedFrameSamples
                                       : static_cast<std::size_t>(GbaRate(rom) / kStrictFramesASecond);
}

std::size_t DriverFrameSamples(const DriverRom& rom, Mixing mixing)
{
    return mixing == Mixing::kEnhanced ? ntdlib::agb::kEnhancedFrameSamples
                                       : static_cast<std::size_t>(rom.Rate() / kStrictFramesASecond);
}

std::unique_ptr<ConsoleModel> NewGbaModel(const GbaRom& rom, Mixing mixing, std::size_t frame_samples)
{
    return std::make_unique<Gba>(rom, mixing, frame_samples);
}

std::unique_ptr<ConsoleModel> NewDriverModel(const DriverRom& rom, Mixing mixing, std::size_t frame_samples, bool dry)
{
    auto console = std::make_unique<DriverConsole>(rom, mixing, frame_samples);
    if (dry)
    {
        console->LeaveOutEcho();
    }

    return console;
}

} // namespace console_model

} // namespace hokuto::song
