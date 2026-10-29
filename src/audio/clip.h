// SPDX-License-Identifier: MIT

// A clip of audio that plays once, such as a stretch of a render around a place that the Export window names. The clip
// usually starts and ends in the middle of the sound. Fades of 10 ms at each end keep it from clicking.

#pragma once

#include <cstddef>
#include <vector>

#include "audio/renderer.h"

namespace hokuto::audio
{

class Clip final : public Renderer
{
public:
    // Plays `samples`, interleaved stereo at `sample_rate`.
    Clip(std::vector<float> samples, int sample_rate);

    int SampleRate() const override;
    std::size_t Render(float* out, std::size_t frames) override;

private:
    const std::vector<float> samples_;
    const int sample_rate_;
    const std::size_t length_; // frames
    const std::size_t fade_;   // frames
    std::size_t position_ = 0;
};

} // namespace hokuto::audio
