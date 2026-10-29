// SPDX-License-Identifier: MIT

#include "audio/declicker.h"

#include <cmath>

namespace hokuto::audio
{
namespace
{

// The gap closes by a factor of e every 48 frames: a millisecond at 48 kHz. After 5 ms, under 1% of it is left.
constexpr float kJoinFrames = 48.0f;

// A gap below 16-bit audio's smallest step. It ends there, and the frames pass unchanged from then on.
constexpr float kNegligible = 1e-7f;

} // namespace

Declicker::Declicker() : decay_(std::exp(-1.0f / kJoinFrames))
{
}

void Declicker::Process(float* frames, std::size_t count, std::size_t audio, bool skipped)
{
    for (std::size_t i = 0; i < count; i++)
    {
        // A join: frames after a skip, the silence after the ring's frames, or the ring's frames after silence. The
        // output continues from its last frame, and the gap to the frames closes from there.
        const bool silent = i >= audio;
        const bool join = (i == 0 && skipped) || silent != silent_;
        silent_ = silent;
        for (std::size_t channel = 0; channel < 2; channel++)
        {
            float& sample = frames[i * 2 + channel];
            if (join)
            {
                gap_[channel] = previous_[channel] - sample;
            }
            sample += gap_[channel];
            gap_[channel] = std::abs(gap_[channel]) < kNegligible ? 0.0f : gap_[channel] * decay_;
            previous_[channel] = sample;
        }
    }
}

} // namespace hokuto::audio
