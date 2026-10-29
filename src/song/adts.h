// SPDX-License-Identifier: MIT

// What the systems' AAC encoders share (song/aac.h): the checks of what they take, and the ADTS header of each frame.

#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "ntdlib/wave/decode.h"

namespace hokuto::song
{

// Throws std::runtime_error with a message for the user unless `pcm` holds 1 or 2 channels of the same length, with
// samples, at 44,100 or 48,000 Hz, and `bit_rate` is one of kAacBitRates.
void CheckEncodable(const ntdlib::wave::Pcm& pcm, uint32_t bit_rate);

// Appends `packet`, a raw AAC-LC frame of `channels` at `rate`, to `bytes` as an ADTS frame: a header of 7 bytes
// without a CRC, then the packet. The header gives MPEG-4, the buffer's fullness as variable, and one raw block.
void AppendAdtsFrame(std::vector<uint8_t>& bytes, std::span<const uint8_t> packet, uint32_t rate, uint32_t channels);

} // namespace hokuto::song
