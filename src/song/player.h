// SPDX-License-Identifier: MIT

// Plays a sequence of a sound archive on its SequenceModel. The model renders frames of the console's length: 5 ms on
// the Switch, 240 samples at 48 kHz, 3 ms on the Wii U, 144 samples at 48 kHz, about 4.9 ms on the 3DS, 160 samples at
// the DSP's 32,728 Hz, and 3 ms on the Wii, 96 samples at 32 kHz. A player seeks the model to the position that
// playback starts from, and writes the frames from there on. A PlaybackClock keeps the position at the start of each
// frame written. The program shows the position of the sample that the audio device plays.
//
// The cycle plays the range between the locators over and over. Locators on the game's loop let the model go round the
// loop as the game does. Notes that sound across the loop's end then go on sounding. Other locators send playback back
// to the left locator once it reaches the right one. Each pass then starts from the state the model has there the first
// time through, and the frames on either side of the jump fade out and in. Past the end of the sequence, the position
// goes on at the tempo at the end. The last notes' releases and then silence fill the cycle up to the right locator.
// Playback that starts after the right locator plays on. With the cycle off, the sequence plays once: to the end of its
// loop's pass, or until its last note has faded.
//
// After an edit, a new model plays the new version from the position on, without a stop. The program runs the new model
// up to the player's frame (see ModelCatchUp), and offers it through the clock. The player takes it at its next frame.
// That frame fades the old model's output out and the new model's output in. When the edit changed the time of the
// ticks, such as with a new tempo, the new model runs up to the player's position in ticks instead, and takes over at
// the frame that plays it.
//
// On the 3DS, the two mixing modes differ in rate. A frame holds 160 samples at about 32,728 Hz in strict mixing, and
// 235 at about 48,070 Hz in enhanced mixing. Both last as long. A player keeps the rate of the model that it started
// with. After a switch of the mode during playback, the new model's frames reach the player's rate through a
// FrameResampler, until playback stops. The resampler delays them by about 1 ms. The next playback plays the model at
// its rate.

#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

#include "audio/renderer.h"
#include "song/frame_resampler.h"
#include "song/sequence.h"
#include "song/sequence_model.h"

namespace hokuto::song
{

// Where a SequencePlayer's output is in the sequence, the levels of its tracks and aux buses, and the values of its
// variables. The player writes them on the render worker's thread. The program reads them, and sets the cycle, the
// mutes and the buses' volumes.
class PlaybackClock
{
public:
    // The cycle in the sequence's ticks: whether it's on, and the locators.
    struct CycleRange
    {
        bool on = false;
        uint32_t start = 0;
        uint32_t end = 0;
    };

    // The peaks of the meters in a frame, left and right, as amplitudes: each track's by the track's number, and each
    // aux bus's. 1 is the output's full scale.
    struct Peaks
    {
        friend bool operator==(const Peaks&, const Peaks&) = default;

        std::array<std::array<float, 2>, SequenceModel::kTracks> tracks{};
        std::array<std::array<float, 2>, kAuxBuses> buses{};
    };

    // A model that a player takes over playback with, and whether it takes over by ticks (Offer).
    struct Offered
    {
        std::shared_ptr<SequenceModel> model;
        bool by_ticks = false;
    };

    // The values of the variables that a game can set, after a frame, by their numbers (see SequenceModel::Variables).
    // kUnset holds the value that each starts with.
    using Variables = std::array<int16_t, kGameVariables>;
    static constexpr Variables kUnset = []
    {
        Variables values{};
        values.fill(-1);
        return values;
    }();

    // A clock for a model whose frames hold `frame_samples` stereo frames (SequenceModel::FrameSamples).
    explicit PlaybackClock(uint64_t frame_samples);

    // The player's side. Mark gives the position, in ticks, at the start of output frame `frame`, counted from the
    // first frame written, the peaks of the meters in the frame, and the variables' values after it. Mark the frames in
    // order, before their samples are written. SetWritten gives the stereo frames written so far. SetEnded says that
    // the output has ended.
    void Mark(uint64_t frame, double ticks, const Peaks& peaks, const Variables& variables = kUnset);
    void SetWritten(uint64_t samples);
    void SetEnded();

    // The program's side: the stereo frames written so far, and whether the output has ended.
    uint64_t Written() const;
    bool Ended() const;

    // The position, in ticks, at stereo frame `sample` of the output. Between the starts of two frames, the position
    // moves on in a straight line. Returns nothing for a sample that hasn't been written.
    std::optional<double> PositionAt(uint64_t sample) const;

    // The highest peaks of each meter in the frames that hold stereo frames `from` to `to` of the output, `to` not
    // included. Frames that haven't been marked, or that the clock no longer keeps, add nothing.
    Peaks PeaksBetween(uint64_t from, uint64_t to) const;

    // The variables' values after the frame that holds stereo frame `sample` of the output. Returns nothing for a frame
    // that hasn't been marked, or that the clock no longer keeps.
    std::optional<Variables> VariablesAt(uint64_t sample) const;

    // The cycle that the player follows. It starts off.
    void SetCycle(const CycleRange& cycle);
    CycleRange Cycle() const;

    // The tracks that the player mutes, a bit for each track's number (see SequenceModel::SetMutes). None at first.
    void SetMutes(uint32_t tracks);
    uint32_t Mutes() const;

    // The volumes of the aux buses that the player plays (see SequenceModel::SetBusVolumes). Full volume at first.
    void SetBusVolumes(const AuxVolumes& volumes);
    AuxVolumes BusVolumes() const;

    // The model's frame at the start of the player's next frame (see SequenceModel::Frames), and its position in ticks
    // there (see SequenceModel::After). The player sets both after each frame of the model. A model that catches up
    // with playback runs up to the frame, or up to the position when the time of its ticks differs.
    void SetModelFrames(uint64_t frames);
    uint64_t ModelFrames() const;
    void SetModelTicks(double ticks);
    double ModelTicks() const;

    // A model that takes over playback from the player's next frame. With `by_ticks`, it takes over at the frame that
    // plays the old model's position in ticks. Without it, it takes over at the old model's frame. The program offers
    // it, and the player takes it. A model offered before the player took the last one takes its place. TakeOffer
    // returns no model without an offer.
    void Offer(std::shared_ptr<SequenceModel> model, bool by_ticks = false);
    Offered TakeOffer();

private:
    // A frame's mark stays for kMarks frames: 1.28 s on the Switch, 0.77 s on the Wii U and the Wii, and 1.25 s on the
    // 3DS. The render worker writes about 100 ms ahead of the device.
    static constexpr std::size_t kMarks = 256;

    // The meters: the tracks', then the aux buses'.
    static constexpr std::size_t kMeters = SequenceModel::kTracks + kAuxBuses;

    uint64_t frame_samples_;
    std::array<std::atomic<double>, kMarks> marks_{};
    std::array<std::array<std::atomic<float>, kMeters * 2>, kMarks> peaks_{}; // the tracks' then the buses', flat
    std::array<std::array<std::atomic<int16_t>, kGameVariables>, kMarks> variables_{};
    std::atomic<uint64_t> marked_{0};
    std::atomic<uint64_t> written_{0};
    std::atomic<bool> ended_{false};
    std::atomic<bool> cycle_on_{false};
    std::atomic<uint64_t> locators_{0}; // the left locator in the high half, the right one in the low half
    std::atomic<uint32_t> mutes_{0};
    std::array<std::atomic<float>, kAuxBuses> bus_volumes_{};
    std::atomic<uint64_t> model_frames_{0};
    std::atomic<double> model_ticks_{0.0};

    // The model on offer, and whether there is one. The player looks at the flag in each frame, and takes the lock only
    // when a model is on offer.
    std::mutex offer_mutex_;
    Offered offer_;
    std::atomic<bool> offered_{false};
};

class SequencePlayer final : public audio::Renderer
{
public:
    // Plays the sequence of `model` from tick `start` of its first pass. The sequence's players share its model. A
    // player uses the model only once Render is first called, on the render worker's thread. The clock's frames have
    // the model's length. A model that the clock offers has frames that last as long. Their length can differ (see
    // above).
    SequencePlayer(std::shared_ptr<SequenceModel> model, uint32_t start, std::shared_ptr<PlaybackClock> clock);

    int SampleRate() const override;
    std::size_t Render(float* out, std::size_t frames) override;

private:
    // What the end of a pass does: nothing, the game's loop, or a jump back to the left locator.
    enum class CycleMode
    {
        kOff,
        kGameLoop,
        kLocators,
    };

    // The cycle's mode, and the right locator held to the end of the loop's first pass. A cycle whose left locator
    // isn't before that end is off.
    CycleMode ModeOf(const PlaybackClock::CycleRange& cycle) const;
    uint32_t CycleEnd(const PlaybackClock::CycleRange& cycle) const;

    // Writes the next frame into frame_. Returns false at the end of the output.
    bool NextFrame();

    // Seeks the model to tick `ticks` and takes the frame that plays it. If the sequence ends before that frame,
    // silence follows inside the cycle, and the output ends outside it.
    bool Begin(uint32_t ticks, bool fade_in, const PlaybackClock::CycleRange& cycle, CycleMode mode);

    // After a frame of the model: the fade at the end of a pass, and whether the cycle jumps back or the output ends.
    void Finish(double mark, bool fade_in, const PlaybackClock::CycleRange& cycle, CycleMode mode);

    // A frame of silence inside the cycle, after the end of the sequence. At the right locator, the cycle jumps back.
    bool Silence(const PlaybackClock::CycleRange& cycle, CycleMode mode);

    // Takes `next`, a model that the program offers, in place of the model, and writes the frame that crossfades from
    // one to the other. The old model's next frame fades out, and the new model's frame at the same place fades in: the
    // same frame, or the frame that plays the same tick. If the new model's output has ended before that frame, the old
    // model's frame fades out alone. Silence then follows inside the cycle, and the output ends outside it.
    bool Crossfade(PlaybackClock::Offered next, const PlaybackClock::CycleRange& cycle, CycleMode mode);

    // Takes the model's last frame into frame_, or silence, and marks the frame's position. The first frame after a
    // seek fades in, and the last frame before the end of a pass fades out. The output then starts and stops without a
    // click. TakeFrame also gives the clock the model's frame.
    void TakeFrame(double position, bool fade_in, bool fade_out);
    void TakeSilence(double position);

    // The model's last frame at the player's length. A frame of another length goes through the resampler.
    std::span<const float> ModelFrame();

    std::shared_ptr<SequenceModel> model_;
    std::shared_ptr<PlaybackClock> clock_;
    int sample_rate_;
    uint32_t start_;
    bool started_ = false;
    bool ended_ = false;
    bool wrap_ = false;         // the last frame ended a pass of the cycle
    bool silent_ = false;       // the sequence has ended inside the cycle
    double silent_ticks_ = 0.0; // the position of the next frame of silence

    // The frame that Render copies from, as floats, two for each stereo frame, and the floats copied from it so far.
    // The frames count from the first one written. fading_ holds the old model's frame in a crossfade.
    std::vector<float> frame_;
    std::vector<float> fading_;
    std::size_t frame_read_;
    uint64_t frames_ = 0;
    uint64_t written_ = 0;

    // The resampler of a model whose frames have another length than the player's, and its last frame. A seek and a new
    // model start a new resampler.
    std::optional<FrameResampler> resampler_;
    std::vector<float> resampled_;
};

} // namespace hokuto::song
