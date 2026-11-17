// SPDX-License-Identifier: MIT

// The levels of renders that several check tools measure, in dB relative to full scale.

#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>

namespace hokuto::tools
{

// The mean power of `samples` in dB relative to full scale, or `floor_db` when every sample is 0 or there's none. Each
// tool keeps its floor. A floor that a tool prints, or subtracts from a level that it prints, can't change without a
// change to the tool's lines.
inline double MeanSquareDb(std::span<const float> samples, double floor_db)
{
    double sum = 0.0;
    for (const float sample : samples)
    {
        sum += static_cast<double>(sample) * sample;
    }

    return sum > 0.0 ? 10.0 * std::log10(sum / static_cast<double>(samples.size())) : floor_db;
}

// The level of `frames` frames of `samples`, interleaved stereo, from frame `from`, in dB relative to full scale. The
// frames before the start and after the end count as silence, and `from` may be negative. Silence gives -200 dB.
inline double WindowDb(std::span<const float> samples, int64_t from, std::size_t frames)
{
    double sum = 0.0;
    for (std::size_t i = 0; i < frames * 2; i++)
    {
        const int64_t at = from * 2 + static_cast<int64_t>(i);
        if (at >= 0 && at < static_cast<int64_t>(samples.size()))
        {
            sum += static_cast<double>(samples[static_cast<std::size_t>(at)]) * samples[static_cast<std::size_t>(at)];
        }
    }

    return sum > 0.0 ? 10.0 * std::log10(sum / static_cast<double>(frames * 2)) : -200.0;
}

} // namespace hokuto::tools
