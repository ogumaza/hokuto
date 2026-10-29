// SPDX-License-Identifier: MIT

// Smooths the joins in the output. At a join, the frames don't follow on from the frames before them. A stop or a seek
// discards the ring's frames, the ring runs out, or frames start again after silence. The output then moves from its
// last sample to the new frames over about a millisecond at 48 kHz, and a stop or a seek makes no click. Elsewhere the
// frames pass unchanged.

#pragma once

#include <array>
#include <cstddef>

namespace hokuto::audio
{

class Declicker
{
public:
    Declicker();

    // Smooths `count` stereo frames in place. The first `audio` of them came from the ring, and the rest are silence.
    // `skipped` says that the ring skipped discarded frames before them.
    void Process(float* frames, std::size_t count, std::size_t audio, bool skipped);

private:
    // The fraction of the gap that's left after each frame.
    const float decay_;

    // The last frame of output, and the gap between the output and the frames, for each channel. The output starts
    // after silence.
    std::array<float, 2> previous_{};
    std::array<float, 2> gap_{};
    bool silent_ = true;
};

} // namespace hokuto::audio
