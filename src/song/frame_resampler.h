// SPDX-License-Identifier: MIT

// Brings frames of interleaved stereo to frames of another length that last as long. A player keeps its rate when a
// model at another rate takes over playback (song/player.h). On the 3DS, the two mixing modes differ in rate. A frame
// holds 160 samples at about 32,728 Hz in strict mixing, and 235 at about 48,070 Hz in enhanced mixing.
//
// The interpolator is ntdlib's windowed sinc (ntdlib/wave/sinc.h), at the step from one length to the other. Output j
// of a frame lies j * from / to samples into the frame's input, in 65536ths of a sample and rounded down, less a delay.
// The delay is the kernel's half width and one sample. Each output then takes only the inputs of its frame and of the
// frames before. Silence comes before the first frame.
//
// A ResampledRenderer plays a renderer's frames at another rate the same way. During playback, an audition in the other
// mode of the 3DS reaches the playback's rate through one (AtRate).

#pragma once

#include <array>
#include <cstddef>
#include <memory>
#include <span>
#include <vector>

#include "ntdlib/wave/sinc.h"

#include "audio/renderer.h"

namespace hokuto::song
{

class FrameResampler
{
public:
    // From frames of `from` stereo samples to frames of `to`. Throws std::invalid_argument for a length of 0.
    FrameResampler(std::size_t from, std::size_t to);

    std::size_t From() const
    {
        return from_;
    }

    // The delay of the output, in samples of the input.
    std::size_t Delay() const;

    // Takes the next frame, `from` stereo samples interleaved, and writes the frame of `to` stereo samples to `out`.
    // Samples past the end of `in` count as silence. `out` holds 2 * `to` floats.
    void Process(std::span<const float> in, std::span<float> out);

private:
    // The inputs kept from the frames before: the kernel's whole width.
    std::size_t History() const;

    std::size_t from_;
    std::size_t to_;
    const ntdlib::wave::SincKernel* kernel_;
    std::array<std::vector<float>, 2> input_; // each channel's inputs from the frames before, then the frame's
};

class ResampledRenderer final : public audio::Renderer
{
public:
    // Plays `source`, whose frames hold `from` stereo samples, in frames of `to` stereo samples at `rate`. The frames
    // of both last as long. After the source's end, silence brings the resampler's delay out, and the output ends at
    // the end of a frame. Throws std::invalid_argument for a length of 0.
    ResampledRenderer(std::unique_ptr<audio::Renderer> source, std::size_t from, std::size_t to, int rate);

    int SampleRate() const override
    {
        return rate_;
    }

    std::size_t Render(float* out, std::size_t frames) override;

private:
    // Makes the next frame of the output. Returns false once the output has ended.
    bool NextFrame();

    std::unique_ptr<audio::Renderer> source_;
    FrameResampler resampler_;
    int rate_;
    std::vector<float> input_;  // a frame of the source
    std::vector<float> output_; // a frame of the output
    std::size_t output_read_;   // the floats of output_ that Render has taken
    std::size_t flush_;         // the frames of silence that remain after the source's end
    bool ended_ = false;        // whether the source has ended
};

// `renderer` at `rate`. A renderer at `rate` comes back as it is. So does a renderer that no resampler here brings to
// `rate`. Between the 3DS's two mixing modes, a ResampledRenderer brings the renderer's frames to the other mode's. The
// frames of both modes last as long.
std::unique_ptr<audio::Renderer> AtRate(std::unique_ptr<audio::Renderer> renderer, int rate);

} // namespace hokuto::song
