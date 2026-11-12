// SPDX-License-Identifier: MIT

// What files.cpp and file_replace.cpp share: the banks and wave sounds that name a wave, and the bytes of a group's
// copy of a file that a new file replaces. Only those files include it.

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <map>
#include <optional>
#include <span>
#include <vector>

#include "ntdlib/sar/formats.h"

namespace hokuto::edit::files_parts
{

namespace sar = ntdlib::sar;

// Whether `wave` names a wave of wave archive `wave_archive`.
bool Names(const sar::WaveId& wave, uint32_t wave_archive);

// The waves that wave sound `sound` of `file` plays: the waves of its notes.
std::vector<sar::WaveId> WavesOf(const sar::WaveSoundFile& file, uint32_t sound);

// The banks of `archive` that name a wave for which `named` holds.
template <class Named>
std::vector<uint32_t> BanksNaming(const sar::SoundArchive& archive, Named named)
{
    std::vector<uint32_t> banks;
    for (std::size_t i = 0; i < archive.Banks().size(); i++)
    {
        std::vector<sar::WaveId> waves;
        try
        {
            waves = sar::Bank::Parse(archive.FileData(archive.Banks()[i].file_id), archive.ReadOptions()).waves;
        }
        catch (const std::exception&)
        {
            // The bank names nothing that Hokuto can read.
        }

        if (std::ranges::any_of(waves, named))
        {
            banks.push_back(static_cast<uint32_t>(i));
        }
    }

    return banks;
}

// The wave sounds of `archive` whose notes play a wave for which `named` holds.
template <class Named>
std::vector<uint32_t> WaveSoundsPlaying(const sar::SoundArchive& archive, Named named)
{
    // Each wave sound file once.
    std::map<uint32_t, std::optional<sar::WaveSoundFile>> files;
    std::vector<uint32_t> sounds;
    for (std::size_t i = 0; i < archive.Sounds().size(); i++)
    {
        const sar::SoundInfo& sound = archive.Sounds()[i];
        if (sound.type != sar::SoundType::kWaveSound)
        {
            continue;
        }

        const auto [found, added] = files.try_emplace(sound.file_id);
        if (added)
        {
            try
            {
                found->second = sar::WaveSoundFile::Parse(archive.FileData(sound.file_id), archive.ReadOptions());
            }
            catch (const std::exception&)
            {
                // The file names nothing that Hokuto can read.
            }
        }

        if (found->second && std::ranges::any_of(WavesOf(*found->second, sound.wave_sound_index), named))
        {
            sounds.push_back(static_cast<uint32_t>(i));
        }
    }

    return sounds;
}

// The bytes that a group's copy of `present` takes when `incoming` takes the place of `present`.
struct CopyBytes
{
    std::vector<uint8_t> bytes;

    // Whether the copy names `present`'s waves at other places, and takes `incoming` with those places.
    bool placed = false;

    // In such a copy, the waves of `incoming`'s table that `present` doesn't name. They keep the archive's places.
    std::size_t unplaced = 0;
};

// The bytes that `copy`, a group's copy of `present`, takes when `incoming` takes the place of `present`. Each is a
// file of `flavor`. A copy that holds `present`'s bytes takes `incoming`. A copy that names `present`'s waves at other
// places takes `incoming` with those places: each wave of `incoming`'s table that `present` names takes its place in
// the copy (PlacesInCopy). Any other copy takes `incoming` as it is.
CopyBytes BytesOfCopy(std::span<const uint8_t> copy, std::span<const uint8_t> present,
                      std::span<const uint8_t> incoming, sar::Flavor flavor);

} // namespace hokuto::edit::files_parts
