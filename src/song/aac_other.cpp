// SPDX-License-Identifier: MIT

// AAC on other systems, such as Linux: they have no AAC codec of their own, and FFmpeg's program decodes and encodes
// (song/ffmpeg.h).

#include "song/aac.h"

#include <stdexcept>

#include "song/adts.h"
#include "song/ffmpeg.h"

namespace hokuto::song
{

bool HasAacCodec()
{
    return FindsFfmpeg();
}

ntdlib::wave::Pcm DecodeAdts(std::span<const uint8_t> bytes)
{
    if (AdtsFrames(bytes) == 0)
    {
        throw std::runtime_error("The file isn't an AAC stream in ADTS.");
    }

    return DecodeAdtsWithFfmpeg(bytes);
}

bool EncodesConstantRate()
{
    return false;
}

std::vector<uint8_t> EncodeAdts(const ntdlib::wave::Pcm& pcm, uint32_t bit_rate)
{
    CheckEncodable(pcm, bit_rate);
    return EncodeAdtsWithFfmpeg(pcm, bit_rate);
}

} // namespace hokuto::song
