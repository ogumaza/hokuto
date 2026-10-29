// SPDX-License-Identifier: MIT

#include "audio/clip.h"

#include <algorithm>
#include <utility>

namespace hokuto::audio
{

Clip::Clip(std::vector<float> samples, int sample_rate)
    : samples_(std::move(samples)),
      sample_rate_(sample_rate),
      length_(samples_.size() / 2),
      fade_(std::min(static_cast<std::size_t>(std::max(sample_rate, 0)) / 100, length_ / 2))
{
}

int Clip::SampleRate() const
{
    return sample_rate_;
}

std::size_t Clip::Render(float* out, std::size_t frames)
{
    // The fades ramp the level linearly over the first and last 10 ms, or over half of a shorter clip.
    const std::size_t n = std::min(frames, length_ - position_);
    for (std::size_t i = 0; i < n; i++)
    {
        const std::size_t frame = position_ + i;
        const std::size_t edge = std::min(frame, length_ - 1 - frame);
        const float gain = edge < fade_ ? static_cast<float>(edge) / static_cast<float>(fade_) : 1.0f;
        out[i * 2] = samples_[frame * 2] * gain;
        out[i * 2 + 1] = samples_[frame * 2 + 1] * gain;
    }
    position_ += n;

    return n;
}

} // namespace hokuto::audio
