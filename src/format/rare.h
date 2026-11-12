// SPDX-License-Identifier: MIT

// The revision of Rare's driver in a Hokuto file. ntdlib tells the driver's builds apart by the traits of their
// commands (ntdlib::rare::Traits): the format in which a command names its channel, and whether controllers 20 to 23
// set a channel's envelope. A file's revision of the driver stores them as bits: bit 0 for the channel in the command
// byte's high nibble, and bit 1 for the envelope's controllers. The functions here are inline: the song layer uses
// them, and links nothing of the format layer.

#pragma once

#include <cstdint>

#include "ntdlib/rare/commands.h"
#include "ntdlib/rare/song.h"

namespace hokuto::format
{

// The last revision: both bits set.
inline constexpr uint8_t kRareMostRevision = 3;

inline uint8_t RareRevision(const ntdlib::rare::Traits& traits)
{
    return static_cast<uint8_t>((traits.format == ntdlib::rare::Format::kChannelNibble ? 1 : 0) |
                                (traits.envelope_controllers ? 2 : 0));
}

inline ntdlib::rare::Traits RareTraits(uint8_t revision)
{
    return {(revision & 1) != 0 ? ntdlib::rare::Format::kChannelNibble : ntdlib::rare::Format::kChannelByte,
            (revision & 2) != 0};
}

} // namespace hokuto::format
