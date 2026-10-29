// SPDX-License-Identifier: MIT

// A sound archive opened from a file, with the group files that the game keeps beside it. Hokuto reads the BFSAR
// archives of the Switch and the Wii U, and the BCSAR archives of the 3DS. ntdlib reads them as hacsf2, cafesf2 and
// citrusf2 do. The archive's flavor, in ntdlib::sar::SoundArchive::ReadOptions, names its console. The Wii's BRSAR
// archives go through ntdlib's rsar. revosf2 reads them with rsar too.

#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/nwsnd/commands.h"
#include "ntdlib/rsar/formats.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sequence_export/settings.h"
#include "ntdlib/sound_build.h"

namespace hokuto::song
{

// Reads the sound archive of the Switch, the Wii U or the 3DS at `path`, and adds the group files beside it. Throws
// std::runtime_error with a message for the user if the file can't be read or isn't such an archive.
std::shared_ptr<const ntdlib::sar::SoundArchive> OpenArchive(const std::filesystem::path& path);

// The sound archive of the Switch, the Wii U or the 3DS in `bytes`, read from `path`, with the group files beside it.
// Messages call it `name`. Throws std::runtime_error with a message for the user if the bytes aren't such an archive.
// The archive keeps the group files that it found (ntdlib::sar::SoundArchive::GroupFiles).
std::shared_ptr<const ntdlib::sar::SoundArchive> LoadArchive(std::vector<uint8_t> bytes, const std::string& name,
                                                             const std::filesystem::path& path);

// The sound archive of the Switch, the Wii U or the 3DS in `bytes`, with `group_files`, such as those that a .hok file
// keeps. Messages call it `name`. Throws std::runtime_error with a message for the user if the bytes aren't such an
// archive, or if a group file isn't a group file.
std::shared_ptr<const ntdlib::sar::SoundArchive> LoadArchive(std::vector<uint8_t> bytes, const std::string& name,
                                                             std::span<const std::vector<uint8_t>> group_files = {});

// The SARC archives in `folders`: the files whose names end in .sarc, by their names, with their bytes. The archives of
// each folder come in the order of their names' UTF-8 bytes, and the folders in their order. An import reads them in
// that order. A folder that an earlier one names again is left out, and so are a file that can't be read and a name
// that an earlier folder gave.
std::vector<std::pair<std::string, std::vector<uint8_t>>> SarcArchivesIn(
    std::span<const std::filesystem::path> folders);

// The folder beside an archive in which a game can keep the group files that its code loads. An import looks for a
// group's file there after the archive's folder (ntdlib::sequence_export::AddGroupFiles).
inline constexpr char kUserManagementFolder[] = "userManagementFiles";

// `outputs`, the files that an export writes beside an archive for its group files (format::GroupFileOutputs), at the
// places where the game keeps them. A group file goes into the userManagementFiles folder when an import would read the
// game's file from there: the first of `folders` that holds a file of its name holds it in that folder, and not beside
// the archive. Its name then starts with the folder's name and a slash. The other files keep their names.
std::vector<std::pair<std::string, std::vector<uint8_t>>> PlaceGroupFiles(
    std::vector<std::pair<std::string, std::vector<uint8_t>>> outputs, std::span<const std::filesystem::path> folders);

// The flavor of the archive in `bytes`: kCtr, the 3DS, for a BCSAR archive, kCafe, the Wii U, for a big-endian BFSAR
// archive, and otherwise kNx, the Switch.
ntdlib::sar::Flavor FlavorOfArchive(std::span<const uint8_t> bytes);

// The settings of ntdlib's performer and converter for the console whose archive `archive` is: the Switch's, the
// Wii U's or the 3DS's. citrusf2 converts with the 3DS's.
ntdlib::sequence_export::Settings SettingsOf(const ntdlib::sar::SoundArchive& archive);

// The sound archive of the Wii in `bytes`, read by `options`. Messages call it `name`. Throws std::runtime_error with a
// message for the user if the bytes aren't such an archive. The converter reads an archive by rsar's default options.
// So does revosf2. The model of the Wii plays an archive read by ntdlib::rvl::snd::kRsarOptions.
std::shared_ptr<const ntdlib::rsar::SoundArchive> LoadWiiArchive(std::vector<uint8_t> bytes, const std::string& name,
                                                                 const ntdlib::rsar::ReadOptions& options = {});

// The settings of ntdlib's performer and converter for the Wii archive `archive`: those of `build`, the game's build of
// NW4R SND, or else of the build that the archive's version gives. revosf2 takes that build when it lacks the game's
// main.dol.
ntdlib::sequence_export::Settings SettingsOf(const ntdlib::rsar::SoundArchive& archive,
                                             const std::optional<ntdlib::SndBuild>& build = {});

// The build of NW4R SND that a game's main.dol links, from its banner, with the build of AX that its microcode
// identifies (ntdlib::FindSndBuild and ntdlib::IdentifyBuild). revosf2 reads the build the same way. Nothing for bytes
// that aren't a DOL with AX's microcode.
std::optional<ntdlib::SndBuild> ReadWiiBuild(std::span<const uint8_t> dol);

// The main.dol of the extracted disc that holds the Wii archive at `archive`: sys/main.dol beside a folder named files
// that holds the archive, at any depth. revosf2 looks for it there too. Dolphin and wit extract a disc that way.
// Nothing if there's none.
std::optional<std::filesystem::path> FindMainDol(const std::filesystem::path& archive);

// The console whose player reads a sequence's bytecode: the 3DS, the Wii U or the Switch by its flavor, or the Wii with
// the build of NW4R SND that plays the archive. The Wii's builds differ in the commands that they read.
struct Console
{
    ntdlib::sar::Flavor flavor = ntdlib::sar::Flavor::kNx; // means nothing on the Wii
    std::optional<ntdlib::SndBuild> wii;
};

// The rules of NintendoWare's codec by which the console of `flavor` reads its sequences: the Wii U's for kCafe, the
// 3DS's for kCtr, and otherwise the Switch's. ntdlib's join encodes a sequence by the same rules.
ntdlib::nwsnd::Grammar GrammarOf(ntdlib::sar::Flavor flavor);

// The same for `console`. On the Wii, the build of NW4R SND gives the rules.
ntdlib::nwsnd::Grammar GrammarOf(const Console& console);

// Whether the sequence player of the console of `flavor` runs a command of `kind` with `index`. The 3DS's player has
// one LFO, without a curve, a phase or a period, and sends to buses A and B only. It skips the other commands of those
// kinds. The players of the Wii U and the Switch run every kind.
bool Runs(ntdlib::sar::Flavor flavor, ntdlib::commands::Kind kind, uint8_t index);

// The same for `console`. NW4R SND's builds lack the commands that ntdlib::nwsnd::RevolutionPolicy::HasCommand leaves
// out, such as the bank select, the LFO's phase, curve and period, and the commands that later builds added.
bool Runs(const Console& console, ntdlib::commands::Kind kind, uint8_t index);

// What `command` sets, for a message about the commands that a console skips or lacks, such as "send C", "LFOs 1 to 3"
// or "bank select".
std::string SettingName(const ntdlib::commands::Command& command);

} // namespace hokuto::song
