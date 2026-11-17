// SPDX-License-Identifier: MIT

// The bytes of a test cart that one of agblib's builders of GBA carts makes.

#pragma once

#include <cstdint>
#include <vector>

#include "agblib/agb/rom.h"

namespace hokuto::testing
{

// The bytes of `rom`, a test cart, from the cart's first address.
inline std::vector<uint8_t> RomBytes(const agblib::agb::Rom& rom)
{
    return std::vector<uint8_t>(rom.Ptr(agblib::agb::kRomBase), rom.Ptr(agblib::agb::kRomBase) + rom.Size());
}

} // namespace hokuto::testing
