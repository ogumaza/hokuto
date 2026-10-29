// SPDX-License-Identifier: MIT

// What the synths give: stereo samples at a rate.

#pragma once

#include <vector>

namespace hokuto::synth
{

// A stereo render: the samples, left and right in turn, at `rate` samples a second. Full scale is 1.0. The samples
// aren't clipped.
struct StereoRender
{
    int rate = 48000;
    std::vector<float> samples;
};

} // namespace hokuto::synth
