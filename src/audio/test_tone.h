// SPDX-License-Identifier: MIT

// The test tone of the Studio Setup window: one second of a 440 Hz sine at about -18 dBFS in both channels. Short fades
// at each end keep it from clicking.

#pragma once

#include <cstddef>

#include "audio/renderer.h"

namespace hokuto::audio
{

class TestTone final : public Renderer
{
public:
    explicit TestTone(int sample_rate);

    int SampleRate() const override;
    std::size_t Render(float* out, std::size_t frames) override;

private:
    const int sample_rate_;
    const std::size_t length_; // frames
    const std::size_t fade_;   // frames
    std::size_t position_ = 0;
};

} // namespace hokuto::audio
