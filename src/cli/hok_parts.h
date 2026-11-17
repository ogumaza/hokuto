// SPDX-License-Identifier: MIT

// The parts of hok that its files share: the input that hok reads, the count and the output of a written file, the
// writers of an archive's items, waves and instruments, and hok mixdown. Only hok.cpp, hok_items.cpp and
// hok_mixdown.cpp include it.

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ntdlib/sound_build.h"

#include "song/batch.h"
#include "song/played.h"

namespace hokuto::format
{

struct File;

} // namespace hokuto::format

namespace hokuto::cli::hok_parts
{

// A sound archive, a ROM or a set of sounds, with its streams by their file IDs. An archive of the Wii comes with the
// game's build of NW4R SND when the game's main.dol names it. What a .hok file builds comes with the file.
struct ArchiveInput
{
    hokuto::song::Played played;
    std::map<uint32_t, std::vector<uint8_t>> streams;
    std::optional<ntdlib::SndBuild> build;
    std::shared_ptr<const hokuto::format::File> hok;
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

// The sounds of `all` that `chosen` names, by name or by number, in the order of `all`. With none chosen, it returns
// them all. Throws std::runtime_error for a name that `input` doesn't contain (hok_mixdown.cpp).
std::vector<hokuto::song::BatchSound> ChosenSounds(const std::vector<hokuto::song::BatchSound>& all,
                                                   const std::vector<std::string>& chosen, const std::string& input);

// The folder at `path`, made if it's missing. Throws std::runtime_error when it can't be made (hok_mixdown.cpp).
std::filesystem::path MadeFolder(const std::string& path);

// Runs hok mixdown with `arguments`, the whole command line, and reads its input with `read` (hok_mixdown.cpp). Returns
// 1 when a sound failed, or else 0. Throws std::invalid_argument for options that don't fit, and the errors of `read`.
int RunMixdown(std::span<const std::string> arguments, const std::function<ArchiveInput(const std::string&)>& read);

// Runs hok midi in the same way (hok_midi.cpp).
int RunMidi(std::span<const std::string> arguments, const std::function<ArchiveInput(const std::string&)>& read);

} // namespace hokuto::cli::hok_parts
