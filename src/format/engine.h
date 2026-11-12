// SPDX-License-Identifier: MIT

// The engines that play a file's sounds. This header contains the list alone, without code: the song layer names a
// driver's engine through it, and links nothing of the format layer.

#pragma once

#include <cstddef>
#include <cstdint>

namespace hokuto::format
{

// The rules that play a file's sounds: one console's, the Switch's, the Wii U's, the 3DS's, the Wii's, the GBA's or the
// DS's. A sound can play on another engine than its file's (File::sound_engines). The GBA's engine is MusicPlayer2000
// (MP2K). The DS has two: NITRO-Composer of its SDK, and Procyon Studio's Digital Sound Elements (DSE). The GBA's other
// drivers each take an engine of their own too (format/drivers.h).
enum class Engine : uint8_t
{
    kSwitch,
    kWiiU,
    k3ds,
    kWii,
    kGba,
    kDs,
    kDse,
    kKonami,
    kRd2,
    kBrownie,
    kUbiMilan,
    kRare,
    kQuintet,
    kKrawall,
};

// The number of engines.
inline constexpr std::size_t kEngineCount = 14;

} // namespace hokuto::format
