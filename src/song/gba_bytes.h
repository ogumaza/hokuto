// SPDX-License-Identifier: MIT

// The bytes of a copy of a GBA ROM that an audition plays: the ROM's bytes, and the bytes that the audition adds after
// them. Each part starts at a whole word.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "ntdlib/agb/rom.h"

namespace hokuto::song
{

// Pads `bytes` with zeros to a whole number of words.
inline void PadToWord(std::vector<uint8_t>& bytes)
{
    bytes.resize((bytes.size() + 3) & ~std::size_t{3});
}

// The bytes of `rom`, padded to a whole number of words.
inline std::vector<uint8_t> PaddedRomCopy(const ntdlib::agb::Rom& rom)
{
    std::vector<uint8_t> bytes(rom.Ptr(ntdlib::agb::kRomBase), rom.Ptr(ntdlib::agb::kRomBase) + rom.Size());
    PadToWord(bytes);
    return bytes;
}

// The GBA address of the end of `bytes`, a copy of a ROM: where the next bytes go.
inline uint32_t EndAddress(const std::vector<uint8_t>& bytes)
{
    return ntdlib::agb::kRomBase + static_cast<uint32_t>(bytes.size());
}

// Appends `appended` to `bytes`, a copy of a ROM padded to a whole number of words, and pads it again. Returns the GBA
// address of the appended bytes.
inline uint32_t AppendWordAligned(std::vector<uint8_t>& bytes, std::span<const uint8_t> appended)
{
    const uint32_t address = EndAddress(bytes);
    bytes.insert(bytes.end(), appended.begin(), appended.end());
    PadToWord(bytes);
    return address;
}

} // namespace hokuto::song
