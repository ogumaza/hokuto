// SPDX-License-Identifier: MIT

// Audio that the render worker renders ahead of the audio device: a song, an auditioned note or the test tone.

#pragma once

#include <cstddef>

namespace hokuto::audio
{

class Renderer
{
public:
    virtual ~Renderer() = default;

    // The rate of the frames that Render writes.
    virtual int SampleRate() const = 0;

    // Writes up to `frames` stereo frames to `out`: two floats each, the left sample, then the right. Returns how many
    // it wrote. That's fewer than `frames` only at the end of the audio, and 0 after it. The output doesn't depend on
    // how the calls divide the audio into blocks.
    virtual std::size_t Render(float* out, std::size_t frames) = 0;
};

} // namespace hokuto::audio
