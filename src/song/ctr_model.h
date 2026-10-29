// SPDX-License-Identifier: MIT

// ntdlib's archive mode of the 3DS (ntdlib/ctr/snd/engine.h), with its output in frames. The DSP is ntdlib's model of
// the DSP's audio firmware (ntdlib/ctr/dsp/firmware_model.h).
//
// In strict mixing, the model gives the samples of Teakra running the firmware of a 3DS game, bit for bit. The DSP's
// output follows the DSP's clock, not the sound frames: it makes a few samples while the firmware starts, fewer than
// 160 in each of the first frames, and now and then 159 or 161 in a frame. Each frame here gives the next 160 samples
// of that output. The output starts with 480 samples of silence: three frames. A frame then never waits for samples
// that the DSP hasn't made. Past the silence, the samples are the DSP's output, sample for sample. The silence delays
// the output by 14.7 ms. The DSP plays the engine's changes about a frame later. The output therefore follows the
// engine by about four frames, or 20 ms.
//
// In enhanced mixing, the model runs in its enhanced mode. Each of the DSP's frames then mixes 235 stereo samples in
// floating point, at about 48,070 Hz. Each frame here gives the next frame that the DSP mixed. The output starts with
// four frames of silence. A note then starts within 0.4 ms of its start in strict mixing. The frames of the two modes
// last as long, and a frame of one mode plays at the same time as the frame of the same number in the other.

#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <span>
#include <vector>

#include "ntdlib/ctr/dsp/dsp.h"
#include "ntdlib/ctr/nnsnd/sound_system.h"
#include "ntdlib/ctr/snd/engine.h"
#include "ntdlib/sar/formats.h"

namespace hokuto::song
{

class CtrModel
{
public:
    // A copy of the model between frames: the archive model's, and the samples that no frame has taken yet.
    struct Snapshot
    {
        ntdlib::ctr::snd::ArchiveModel::Snapshot model;
        std::deque<int16_t> pending;
        std::deque<float> pending_floats;
        std::vector<int16_t> output;
        std::vector<float> float_output;
        int idle_frames = 0;
    };

    // The stereo samples in a frame of strict mixing, and the rate that Hokuto gives the audio device. The 3DS's
    // players report the same rate. The DSP's exact rate is 268111856 / 8192 Hz, about 32,728.498 Hz.
    static constexpr std::size_t kFrameSamples = 160;
    static constexpr int kSampleRate = 32728;

    // The same in enhanced mixing. The exact rate is 235 / 160 of the DSP's, about 48,069.98 Hz.
    static constexpr std::size_t kEnhancedFrameSamples = ntdlib::ctr::dsp::kEnhancedFrameSamples;
    static constexpr int kEnhancedSampleRate = 48070;

    // The stereo samples of silence before the DSP's output, in strict and in enhanced mixing.
    static constexpr std::size_t kSilence = 3 * kFrameSamples;
    static constexpr std::size_t kEnhancedSilence = 4 * kEnhancedFrameSamples;

    // The frames after the engine's last sound in which the output can still sound. The output follows the engine by a
    // little over four frames. The rest are to spare.
    static constexpr int kTailFrames = 8;

    // A model in strict mixing, or in enhanced mixing with `enhanced`.
    explicit CtrModel(bool enhanced = false);
    CtrModel(const CtrModel&) = delete;
    CtrModel& operator=(const CtrModel&) = delete;

    // Starts the DSP and the engine on `archive`. The archive has to outlive the model. Returns false if the DSP
    // doesn't start.
    bool Start(const ntdlib::sar::SoundArchive& archive);

    // The stereo samples of the model's frame, in its mixing mode.
    std::size_t FrameSamples() const
    {
        return enhanced_ ? kEnhancedFrameSamples : kFrameSamples;
    }

    // The engine, once the model has started.
    ntdlib::ctr::snd::Engine& Engine()
    {
        return *model_->engine;
    }

    // The model of nn::snd, once the model has started: the processing of the aux buses and their return volumes. A
    // snapshot copies the buses' callbacks with what they hold.
    ntdlib::ctr::nnsnd::SoundSystem& Snd()
    {
        return model_->snd;
    }

    // Runs the engine's next frame and takes the next frame of output. Returns false if the DSP stopped answering. The
    // frame's output then stays silent.
    bool RunFrame();

    // Whether anything can still sound: the engine's sounds, or their last samples. The DSP gives those up to
    // kTailFrames frames after the engine has run them.
    bool IsBusy() const
    {
        return model_->engine->IsBusy() || idle_frames_ < kTailFrames;
    }

    // The last frame's output in interleaved stereo. In enhanced mixing, each sample is the floating-point sample's,
    // rounded to the nearest whole number and held within 16 bits.
    std::span<const int16_t> Output() const
    {
        return output_;
    }

    // The last frame's output in floating point in enhanced mixing, interleaved, with full scale at 1. Strict mixing
    // gives nothing here.
    std::span<const float> FloatOutput() const
    {
        return float_output_;
    }

    // Takes a snapshot. The snapshot and `previous` share the memory pages that haven't changed since `previous`.
    Snapshot Save(const Snapshot* previous);
    void Restore(const Snapshot& snapshot);

private:
    bool enhanced_;
    std::unique_ptr<ntdlib::ctr::snd::ArchiveModel> model_;
    std::deque<int16_t> pending_;      // the DSP's samples, interleaved, that no frame has taken
    std::deque<float> pending_floats_; // the same in enhanced mixing
    std::vector<int16_t> output_;
    std::vector<float> float_output_;
    int idle_frames_ = 0; // the frames since the engine last sounded, up to kTailFrames
};

} // namespace hokuto::song
