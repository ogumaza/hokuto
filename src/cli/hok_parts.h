// SPDX-License-Identifier: MIT

// The parts of hok that its files share: the input that hok reads, the count and the output of a written file, and the
// writers of an archive's items, waves and instruments. Only hok.cpp and hok_items.cpp include it.

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ntdlib/sound_build.h"

#include "song/played.h"

namespace hokuto::cli::hok_parts
{

// A sound archive, a ROM or a set of sounds, with its streams by their file IDs. An archive of the Wii comes with the
// game's build of NW4R SND when the game's main.dol gives it.
struct ArchiveInput
{
    hokuto::song::Played played;
    std::map<uint32_t, std::vector<uint8_t>> streams;
    std::optional<ntdlib::SndBuild> build;
};

// The count and the output of a written file (hok.cpp).
std::string Counted(std::size_t count, std::string_view one, std::string_view many);
void WriteOutput(const std::string& path, std::span<const uint8_t> bytes);

// The writers of an archive's items, waves and instruments, for each console and driver (hok_items.cpp).
std::size_t WriteItems(const ArchiveInput& input, const std::filesystem::path& folder);
std::size_t WriteWavs(const ArchiveInput& input, const std::filesystem::path& folder);
std::size_t WriteWiiItems(const ArchiveInput& input, const std::filesystem::path& folder);
std::size_t WriteWiiWavs(const ArchiveInput& input, const std::filesystem::path& folder);
std::size_t WriteBanks(const ArchiveInput& input, const std::filesystem::path& folder, bool sfz);
std::size_t WriteDseItems(const ArchiveInput& input, const std::filesystem::path& folder);
std::size_t WriteDseWavs(const ArchiveInput& input, const std::filesystem::path& folder);
std::size_t WriteDsItems(const ArchiveInput& input, const std::filesystem::path& folder);
std::size_t WriteDsWavs(const ArchiveInput& input, const std::filesystem::path& folder);
std::size_t WriteGbaItems(const ArchiveInput& input, const std::filesystem::path& folder);
std::size_t WriteGbaWavs(const ArchiveInput& input, const std::filesystem::path& folder);
std::size_t WriteVoiceGroups(const ArchiveInput& input, const std::filesystem::path& folder);
std::size_t WriteDriverWavs(const ArchiveInput& input, const std::filesystem::path& folder);
std::size_t WriteDriverSoundFonts(const ArchiveInput& input, const std::filesystem::path& folder, bool sfz);
std::size_t WriteDriverItems(const ArchiveInput& input, const std::filesystem::path& folder);

} // namespace hokuto::cli::hok_parts
