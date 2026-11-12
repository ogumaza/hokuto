// SPDX-License-Identifier: MIT

// The scale between a 16-bit sample and a float sample. The models, the players and the previews share it.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace hokuto::song
{

// A 16-bit sample as a float, from -1 to just under 1.
constexpr float kSampleScale = 1.0f / 32768.0f;

// A float sample as a 16-bit sample: rounded, and clipped at the limits of 16 bits.
inline int16_t ToPcm16(float sample)
{
    return static_cast<int16_t>(std::clamp(std::lround(sample * 32768.0f), -32768L, 32767L));
}

} // namespace hokuto::song
