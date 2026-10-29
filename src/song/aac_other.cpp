// SPDX-License-Identifier: MIT

// AAC on other systems: Hokuto has no decoder or encoder there.

#include "song/aac.h"

#include <stdexcept>

namespace hokuto::song
{

ntdlib::wave::Pcm DecodeAdts(std::span<const uint8_t> /*bytes*/)
{
    throw std::runtime_error("Hokuto decodes AAC streams on the Mac and on Windows only.");
}

bool EncodesConstantRate()
{
    return false;
}

std::vector<uint8_t> EncodeAdts(const ntdlib::wave::Pcm& /*pcm*/, uint32_t /*bit_rate*/)
{
    throw std::runtime_error("Hokuto encodes AAC streams on the Mac and on Windows only.");
}

} // namespace hokuto::song
