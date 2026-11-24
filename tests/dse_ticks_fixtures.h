// SPDX-License-Identifier: MIT

// What the tests of DSE's ticks share: the driver's two ticks, the set of a cartridge image's DSE files as ntdlib's
// converter finds them, and the MIDI file of ntdlib's converter at a tick.

#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "ntdlib/dse/driver.h"
#include "ntdlib/dse/files.h"
#include "ntdlib/dse/layout.h"
#include "ntdlib/ntr/rom.h"
#include "ntdlib/sequence_export/dse/convert.h"
#include "ntdlib/sequence_export/dse/performance.h"

#include "song/dse.h"

namespace hokuto::testing::dse_ticks
{

inline constexpr int kTick = ntdlib::dse::Driver::kTickMicroseconds;
inline constexpr int kSlowTick = ntdlib::dse::Driver::kSlowTickMicroseconds;
static_assert(kTick == 10000 && kSlowTick == 16715);

// A set of the DSE files of the cartridge image `image` (ntdlib::dse::Join), as ntdlib's converter finds them: the
// files of the file system under their paths, and the DSE files in the game's packs (ntdlib::dse::Decompressed and
// FindFiles).
inline std::vector<uint8_t> SetOf(std::span<const uint8_t> image)
{
    const ntdlib::ntr::Rom rom = ntdlib::ntr::Rom::Parse(image);
    std::vector<ntdlib::dse::NamedFile> named;
    for (const ntdlib::ntr::RomFile& file : rom.Files())
    {
        named.push_back({file.path.empty() ? "file " + std::to_string(file.id) : file.path, rom.FileData(file.id)});
    }

    std::vector<std::vector<uint8_t>> storage;
    const std::vector<ntdlib::dse::NamedFile> found = ntdlib::dse::FindFiles(ntdlib::dse::Decompressed(named, storage));
    const ntdlib::dse::Layout layout = ntdlib::dse::Cut(found);

    return ntdlib::dse::Join(layout.sequences, layout.pieces, layout.fields);
}

// The MIDI file of ntdlib's converter for sound `sound` with a tick of `tick`.
inline std::vector<uint8_t> DirectMidi(const song::DseSet& set, uint32_t sound, int tick)
{
    const ntdlib::sequence_export::dse::PerformOptions options{.build = set.build, .tick_microseconds = tick};
    return ntdlib::sequence_export::dse::ConvertSequence(set.sounds[sound].Files(), song::DseSoundName(set, sound),
                                                         options, "Hokuto")
        .midi;
}

} // namespace hokuto::testing::dse_ticks
