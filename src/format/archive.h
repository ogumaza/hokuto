// SPDX-License-Identifier: MIT

// Sound archives into Hokuto's files and back. An import cuts a sound archive of the 3DS, the Wii U or the Switch with
// ntdlib (ntdlib/sar/layout.h), with the group files that the game keeps beside it. The archive's sequences become the
// file's sequences, and its pieces and fields the file's native layout. An export joins the pieces again and writes the
// fields. An archive that nothing changed comes back byte for byte, and so do its group files. After an edit, each
// sequence file takes the size that its commands give, and the files after it move. A copy of a sequence file in a
// group file follows its sequence too. The Wii's archives (RSAR) come in and go out the same way, through ntdlib's rsar
// module (ntdlib/rsar/layout.h).

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/rsar/formats.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sar/layout.h"
#include "ntdlib/sound_build.h"

#include "format/file.h"

namespace hokuto::format
{

// A file made from sound archive `archive` of flavor `flavor`, with `group_files`, the group files that the game keeps
// beside it, and `streams`, the game's stream files beside it by their file IDs. A Switch archive (kNx) gets
// Engine::kSwitch, a Wii U archive (kCafe) Engine::kWiiU, and a 3DS archive (kCtr) Engine::k3ds. Each group file keeps
// its group and the game's size and SHA-1. The group is the one whose copies the file holds (ntdlib::sar::GroupOfFile),
// or else the one that `groups` names for it: the group's label among the groups kept outside the archive
// (ntdlib::sar::SoundArchive::GroupFileGroups). The 3DS's file table holds no lists of groups, and only the label gives
// a group file's group there. The file keeps the archive's sizes as the game's sizes, unless ntdlib can't list a
// group's files (see SizesOf). The sequence files of `known` take the commands that it gives them where those give the
// files' bytes back (ntdlib::sar::Cut): a file in the model form lays its archive out from its sequences, and gives
// them. Throws xsflib::FormatError if the archive's tables can't be read.
File ImportArchive(std::span<const uint8_t> archive, ntdlib::sar::Flavor flavor,
                   std::vector<std::vector<uint8_t>> group_files = {},
                   std::map<uint32_t, std::vector<uint8_t>> streams = {}, std::span<const std::string> groups = {},
                   const ntdlib::sar::KnownSequences& known = {});

// A file made from the Switch's sound archive `archive` (see ImportArchive).
inline File ImportSwitchArchive(std::span<const uint8_t> archive, std::vector<std::vector<uint8_t>> group_files = {})
{
    return ImportArchive(archive, ntdlib::sar::Flavor::kNx, std::move(group_files));
}

// A file made from `archive`, a sound archive of the Wii (RSAR), with Engine::kWii. `build` is the game's build of NW4R
// SND, from the game's main.dol (song::ReadWiiBuild), and the file keeps it. Without it, the archive's version gives
// the build. The sequences decode by the rules of the build that BuildOfWiiArchive gives. The file keeps the archive's
// size as the game's size. It keeps the values of the archive's tables that ntdlib reads for playback
// (ntdlib::rvl::snd::kRsarOptions), unless ntdlib can't read them. The players and banks take the item IDs of the later
// consoles' archives, and a sequence sound has one bank slot. An export writes the values in place
// (ntdlib::rsar::WriteTableValues), and gives the rest of the tables back as they are, with the offsets and sizes that
// the edits change. As for the later consoles, `streams` holds the game's stream files beside the archive by their file
// IDs. `known` gives the commands of sequence files as for ImportArchive. Throws xsflib::FormatError if the archive's
// tables can't be read.
File ImportWiiArchive(std::span<const uint8_t> archive, const std::optional<ntdlib::SndBuild>& build = {},
                      std::map<uint32_t, std::vector<uint8_t>> streams = {},
                      const ntdlib::sar::KnownSequences& known = {});

// The build of NW4R SND whose rules the sequences of `native`, an archive of the Wii, follow: the game's build that the
// file keeps, or else the build that the archive's version in its header gives: a build of June 2008 for version 1.4 or
// later, and otherwise a build of 2007. ntdlib plays an archive without the game's main.dol by the same rule
// (ntdlib::sequence_export::BuildOfRsarVersion). Throws std::invalid_argument when the first piece doesn't hold the
// header.
ntdlib::SndBuild BuildOfWiiArchive(const NativeArchive& native);

// The build of NW4R SND that plays the Wii's archives of version `version` without the game's main.dol: a build of June
// 2008 for version 1.4 or later, and otherwise a build of 2007 (BuildOfWiiArchive).
ntdlib::SndBuild BuildOfWiiVersion(uint16_t version);

// The version of `native`, an archive of the Wii, from the header in its first piece, such as 0x0104 for version 1.4.
// Throws std::invalid_argument when the first piece doesn't hold the header.
uint16_t WiiVersionOf(const NativeArchive& native);

// The archive that `file`'s native layout builds, with the values of the file's tables. A sound's entry takes the place
// of each optional value that requires one (NeededPlaces), and a sequence sound's list of banks takes the length of its
// banks. The INFO block then grows or shrinks, and the files after it move. Throws std::invalid_argument for a file
// without a native layout, and for a native layout that ntdlib can't join, such as one with a value past the console's
// range. It also throws for tables that the archive can't take, such as a new place or a list of another length in an
// archive that NintendoWare's tools didn't lay out. An archive of the Wii takes the values in place, and nothing moves.
// It throws there for a value that the archive's version doesn't hold, such as a pan mode before version 1.2.
std::vector<uint8_t> ExportArchive(const File& file);

// The group files that `file`'s native layout builds, in its order. Throws the exceptions of ExportArchive.
std::vector<std::vector<uint8_t>> ExportGroupFiles(const File& file);

// The group files of `native` as ntdlib's layouts, for ntdlib's functions that take them in one array
// (ntdlib::sar::ReplaceFile and FileCopies).
std::vector<ntdlib::sar::GroupFileLayout> GroupFileLayouts(const NativeArchive& native);

// The group files that `file`'s native layout builds, by their groups' indices. A group file whose group Hokuto
// couldn't find is left out. Throws the exceptions of ExportArchive.
std::map<uint32_t, std::vector<uint8_t>> GroupFilesBeside(const File& file);

// A group file that an export builds with other bytes than the game's: its place among the file's group files, its
// group's index and name, and its bytes. The name is empty when Hokuto couldn't find the group, or when the file has no
// tables.
struct ChangedGroupFile
{
    std::size_t index = 0;
    std::optional<uint32_t> group;
    std::string name;
    std::vector<uint8_t> bytes;
};

// The group files of `file` whose bytes differ from the game's, by their sizes and SHA-1s. An export writes them beside
// the archive (GroupFileOutputs). Throws the exceptions of ExportArchive.
std::vector<ChangedGroupFile> ChangedGroupFiles(const File& file);

// A file that an export writes beside the archive: its name in the archive's folder, and its bytes.
using FileBeside = std::pair<std::string, std::vector<uint8_t>>;

// The files that an export writes beside `archive`, an exported archive of `flavor`, for the group files in `changed`.
// A group file goes into the first SARC archive of `sarcs` that holds a file of its group, in that file's place
// (ntdlib::sar::GroupOfFile and WithSarcFile). The SARC archive then goes out under its name, with each group file that
// it takes. Another group file goes out under its group's name and .bcgrp on the 3DS, or .bfgrp on the Wii U and the
// Switch. A group file whose group Hokuto couldn't find stays out. Throws xsflib::FormatError if the archive's tables
// can't be read.
std::vector<FileBeside> GroupFileOutputs(std::vector<ChangedGroupFile> changed, std::span<const uint8_t> archive,
                                         ntdlib::sar::Flavor flavor, std::span<const FileBeside> sarcs);

// A group of an archive, with the size in bytes of the files that it loads: the size that its growth counts from, and
// its size after an export. The game loads a group's files into its sound heap. The heap's size isn't in the game's
// files.
struct GroupGrowth
{
    friend bool operator==(const GroupGrowth&, const GroupGrowth&) = default;

    std::string name;
    std::size_t before = 0;
    std::size_t after = 0;
};

// The size that an archive's growth counts from and its size after an export, and the groups in it that grew, in the
// order of its group table.
struct ArchiveGrowth
{
    std::size_t before = 0;
    std::size_t after = 0;
    std::vector<GroupGrowth> groups;
};

// How `after`, an archive of `flavor` that an export built, grew from the sizes `before`: the game's sizes, or the
// sizes of the archive that the export started from. A group's files are the copies that its file holds and the
// archive's files that it links to. A group that the game keeps beside the archive counts the files of its group file
// in `beside`, by the group's index, and no files without one. When ntdlib can't read the groups of `after`, the result
// gives only the archives' sizes.
ArchiveGrowth MeasureGrowth(const GameSizes& before, std::span<const uint8_t> after, ntdlib::sar::Flavor flavor,
                            const std::map<uint32_t, std::vector<uint8_t>>& beside = {});

// The sizes of `archive`, an archive of `flavor`, measured as an import measures the game's sizes: the archive's size,
// and the size of the files that each of its groups loads. As for MeasureGrowth, `beside` gives the group files beside
// the archive by their groups' indices. The groups' sizes are left out when ntdlib can't read the groups.
GameSizes SizesOf(std::span<const uint8_t> archive, ntdlib::sar::Flavor flavor,
                  const std::map<uint32_t, std::vector<uint8_t>>& beside = {});

// The sequence of `file` that each file of `archive` holds, by the file's ID: the sequence whose piece starts where the
// file starts. A file that only the group files beside the archive hold takes the sequence whose piece starts where its
// first copy starts there. Other files have none. `bytes` are the archive's bytes. The native layout of `file` builds
// them. An edit of the sequences or the tables leaves the answer as it is. Throws std::invalid_argument for a file
// without a native layout, for an archive of the Wii, and for bytes that the layout's pieces don't fit.
std::vector<std::optional<uint32_t>> SequencesOfFiles(const File& file, std::span<const uint8_t> bytes,
                                                      const ntdlib::sar::SoundArchive& archive);

// The same for the Wii's `archive`, read from `bytes`. A file that several groups hold has a copy in each. The copy
// that ntdlib's reader takes gives the sequence (ntdlib::rsar::SoundArchive::FileData). Throws std::invalid_argument
// for a file without a native layout of the Wii, and for bytes that the layout's pieces don't fit.
std::vector<std::optional<uint32_t>> SequencesOfFiles(const File& file, std::span<const uint8_t> bytes,
                                                      const ntdlib::rsar::SoundArchive& archive);

// The field of `file`'s native layout that holds each sound's start in its sequence file, by the sound's place in the
// archive's sound table (ntdlib::sar::StartFields). `bytes` are the archive's bytes. The native layout of `file` builds
// them. The tables in the layout's first piece give the places, and `bytes` give them only when that piece doesn't hold
// the tables. A sound without such a field starts where its file's bytecode starts, or its file stays in the bytes.
// Edits leave the answer as it is: they move the places of the fields, not the fields. Throws std::invalid_argument for
// a file without a native layout and for bytes that the layout's pieces don't fit, and xsflib::FormatError for tables
// that can't be read.
std::vector<std::optional<uint32_t>> StartFieldsOfSounds(const File& file, std::span<const uint8_t> bytes);

// Which of each sound's optional values the tables of `native` have a place for, by the sound's place in the archive's
// sound table (ntdlib::sar::TableValuePlaces). An export gives an entry the places that its values require
// (NeededPlaces). The archive's first piece holds its header and its tables. Nothing when that piece doesn't hold the
// whole INFO block, when ntdlib can't read the block, or for an archive of the Wii.
std::optional<std::vector<ntdlib::sar::ValuePlaces>> TableValuePlacesOf(const NativeArchive& native);

// The optional values of `sound` that require a place in the sound's entry: those that differ from the values that
// ntdlib's reader gives a sound whose entry leaves them out. The reader gives a pan mode and a pan curve of 0, and a
// player priority of 64 with actor player 0. It gives a sequence sound a channel priority of 64 without the release
// priority fix.
ntdlib::sar::ValuePlaces NeededPlaces(const NativeSound& sound);

} // namespace hokuto::format
