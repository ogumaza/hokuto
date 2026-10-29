// SPDX-License-Identifier: MIT

#include "audio/test_tone.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace hokuto::audio
{
namespace
{

constexpr double kFrequency = 440.0;

// An amplitude of 0.125 is about -18 dBFS.
constexpr double kLevel = 0.125;

} // namespace

TestTone::TestTone(int sample_rate)
    : sample_rate_(sample_rate), length_(static_cast<std::size_t>(std::max(sample_rate, 0))), fade_(length_ / 100)
{
}

int TestTone::SampleRate() const
{
    return sample_rate_;
}

std::size_t TestTone::Render(float* out, std::size_t frames)
{
    // The fades ramp the level linearly over the first and last 10 ms. Library math is fine here: no test compares the
    // tone's samples between platforms.
    const std::size_t n = std::min(frames, length_ - position_);
    for (std::size_t i = 0; i < n; i++)
    {
        const std::size_t frame = position_ + i;
        const std::size_t edge = std::min(frame, length_ - 1 - frame);
        const double gain = edge < fade_ ? static_cast<double>(edge) / static_cast<double>(fade_) : 1.0;
        const double phase =
            2.0 * std::numbers::pi * kFrequency * static_cast<double>(frame) / static_cast<double>(sample_rate_);
        const auto sample = static_cast<float>(kLevel * gain * std::sin(phase));
        out[i * 2] = sample;
        out[i * 2 + 1] = sample;
    }
    position_ += n;

    return n;
}

} // namespace hokuto::audio
