// SPDX-License-Identifier: MIT

// Sound archives into Hokuto's files and back. An import cuts a sound archive of the 3DS, the Wii U or the Switch with
// ntdlib (ntdlib/sar/layout.h), with the group files that the game keeps beside it. The archive's sequences become the
// file's sequences, and its pieces and fields the file's native layout. An export joins the pieces again and writes the
// fields. An archive that nothing changed comes back byte for byte, and so do its group files. After an edit, each
// sequence file takes the size that its commands give, and the files after it move. A copy of a sequence file in a
// group file follows its sequence too. The Wii's archives (RSAR) come in and go out the same way, through ntdlib's rsar
// module (ntdlib/rsar/layout.h). So do the songs of a GBA ROM, through ntdlib's mp2k module (ntdlib/mp2k/layout.h). The
// whole ROM is the archive there.

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "ntdlib/dse/sets.h"
#include "ntdlib/ntr/rom_writer.h"
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

// A file made from `rom`, a GBA ROM whose songs MusicPlayer2000 plays, with Engine::kGba. Each song that ntdlib's cut
// finds becomes a sequence (ntdlib::mp2k::Cut), and the ROM's other bytes stay as they are. The file keeps the ROM's
// size as the game's size and has no tables. Throws xsflib::FormatError if Hokuto finds no MusicPlayer2000 in the ROM,
// or no song in its song table.
File ImportGbaRom(std::span<const uint8_t> rom);

// Whether `bytes` start as a GBA ROM: their header contains the fixed value 0x96 at 0xB2, and they don't start with the
// signature of a sound archive or of a Hokuto file.
bool IsGbaRom(std::span<const uint8_t> bytes);

// Whether `rom`, a GBA ROM, contains Konami's driver (ntdlib::konami::DetectDriver). Such a ROM has no sign of
// MusicPlayer2000's driver, and imports with ImportKonamiRom.
bool HoldsKonamiDriver(std::span<const uint8_t> rom);

// A file made from `rom`, a GBA ROM with Konami's driver, with Engine::kKonami. Each song that ntdlib's cut finds
// becomes a sequence (ntdlib::konami::Cut), and the ROM's other bytes stay as they are. The file keeps the ROM's size
// as the game's size and has no tables. Throws xsflib::FormatError if Hokuto finds no such driver in the ROM.
File ImportKonamiRom(std::span<const uint8_t> rom);

// A file made from `sdat`, a sound archive of the DS (SDAT), with Engine::kDs. Each sequence file that ntdlib's cut
// finds becomes a sequence (ntdlib::sdat::Cut), and the archive's other bytes stay as they are. The file keeps the
// archive's size as the game's size and has no tables. Throws xsflib::FormatError for bytes that aren't such an
// archive.
File ImportDsArchive(std::span<const uint8_t> sdat);

// Whether `bytes` start as a sound archive of the DS: with the magic word SDAT.
bool IsDsArchive(std::span<const uint8_t> bytes);

// A sound archive of the DS in a cartridge image: the file of the image's file system that contains it, by its ID and
// its path, and the archive's offset in the file and its size.
struct DsArchivePlace
{
    friend bool operator==(const DsArchivePlace&, const DsArchivePlace&) = default;

    uint32_t file = 0;
    std::string path;
    uint32_t offset = 0;
    uint32_t size = 0;
};

// The sound archives of the DS in the files of `image`, a DS cartridge image, in the order of the files and of the
// archives in each file (ntdlib::sdat::FindArchives). Throws xsflib::FormatError for an image that ntdlib can't read
// (ntdlib::ntr::Rom).
std::vector<DsArchivePlace> DsArchivesIn(std::span<const uint8_t> image);

// A file made from the sound archive at `place` in `image`, a DS cartridge image. ImportDsArchive makes the file the
// same way. Without `place`, the image's largest sound archive (DsArchivesIn). The file keeps the image as its
// cartridge, with the place. Throws xsflib::FormatError for an image that ntdlib can't read, an image without a sound
// archive, and an archive that runs past the end of its file.
File ImportDsImage(std::span<const uint8_t> image, const std::optional<DsArchivePlace>& place = std::nullopt);

// Whether `image`, a DS cartridge image, contains DSE's files: songs, wave banks or banks of sound effects (smdl, swdl
// or sedl) in its file system. Throws xsflib::FormatError for an image that ntdlib can't read (ntdlib::ntr::Rom).
bool HoldsDseFiles(std::span<const uint8_t> image);

// A file made from the DSE files of `image`, a DS cartridge image, with Engine::kDse. ntdlib's cut (ntdlib::dse::Cut)
// lays them out as a set: each song and each sound effect becomes a sequence, and the other bytes stay as they are. The
// file keeps the set's size as the game's size, and the image as its cartridge. Throws xsflib::FormatError for an
// image that ntdlib can't read, and for an image without such files.
File ImportDseImage(std::span<const uint8_t> image);

// A file made from `set`, the set that a file of DSE's files builds (ExportArchive), without a cartridge. Throws
// xsflib::FormatError for a directory that doesn't fit the set.
File ImportDseSet(std::span<const uint8_t> set);

// Whether `bytes` start as a DS cartridge image: their header contains the CRC-16 of the Nintendo logo, 0xCF56, at
// 0x15C.
bool IsDsImage(std::span<const uint8_t> bytes);

// Whether `image`, a DS cartridge image, runs on the DSi too: the unit code at 0x12 of its header has bit 1 set. The
// header of such an image contains the hashes of its digest, with HMACs that only the DSi key can compute, and an RSA
// signature over them that only Nintendo can make.
bool IsDsiImage(std::span<const uint8_t> image);

// The DSi's key of the HMACs of a cartridge image's digest, from `bytes`, a key file of its 64 bytes. Throws
// std::invalid_argument for a file of another size.
ntdlib::ntr::DsiKeys DsiKeysOf(std::span<const uint8_t> bytes);

// The cartridge image that `cartridge` contains: its bytes, with 0xFF bytes up to its size. Throws
// std::invalid_argument for an image of more bytes than its size.
std::vector<uint8_t> CartridgeImage(const Cartridge& cartridge);

// The cartridge image that a DS file exports, and the hashes of the image that the change leaves stale
// (ntdlib::ntr::WithFiles).
struct DsImageExport
{
    std::vector<uint8_t> image;
    std::vector<ntdlib::ntr::StaleHash> stale;
};

// The cartridge image of `file`, a file of a DS cartridge image. A set of DSE files writes each of its files that
// changed under its path in the image. Otherwise the archive that ExportArchive builds replaces the archive of the
// game. When the archive or the set keeps the bytes of the game, the image comes back byte for byte. Otherwise ntdlib
// writes the image again with the new bytes of the archive's file (ntdlib::ntr::WithFiles). The items after the file
// move by multiples of 0x200 bytes, and the FAT, the header and its CRC follow. With `keys`, the digest of a DSi image
// gets new hashes. Without them, its hashes of the bytes that changed are stale. An archive that shares its file with
// other bytes keeps its size: nothing tells Hokuto what points into those bytes. Throws std::invalid_argument for a
// file without a cartridge and for such an archive of another size, and the exceptions of ExportArchive and
// ntdlib::ntr::WithFiles.
DsImageExport ExportDsImage(const File& file, const std::optional<ntdlib::ntr::DsiKeys>& keys = std::nullopt);

// Whether the file of the cartridge image of `file`, a file of a DS archive, contains other data beside the game's
// archive. Such an archive keeps its size (ExportDsImage). An archive without a cartridge shares nothing.
bool DsArchiveSharesFile(const File& file);

// Where each piece of the native layout of `file`, a file of a GBA ROM, a DS sound archive or a set of DSE files,
// starts in the ROM, the archive or the set that the file builds, then the layout's end. A run occupies its bytes, and
// a song of a GBA ROM the size of its place (ntdlib::mp2k::Cut). On the GBA's other drivers, a song occupies the bytes
// that the driver encodes for it (Driver::PieceBytes), and the piece after a list's room starts at the room's end. A
// sequence of a DS archive or a set occupies the bytes of its commands, and padding brings the next piece to its
// alignment (ntdlib::sdat::Join and ntdlib::dse::Join). Returns nothing for a file of another console. Throws the
// exceptions of ntdlib::ntr::snd::EncodeSequence, ntdlib::dse::EncodeTracks and Driver::PieceBytes.
std::vector<uint64_t> NativePieceStarts(const File& file);

// The sequence of each sequence piece of `pieces`, by where the piece starts: `starts` lists where each piece starts
// (NativePieceStarts, or ntdlib's Starts), and `shift` moves every start by the same amount.
template <typename Start>
std::map<Start, uint32_t> SequenceStarts(std::span<const ntdlib::sar::Piece> pieces, std::span<const Start> starts,
                                         Start shift = 0)
{
    std::map<Start, uint32_t> sequence_at;
    for (std::size_t p = 0; p < pieces.size(); ++p)
    {
        if (const auto* sequence = std::get_if<ntdlib::sar::SequencePiece>(&pieces[p]))
        {
            sequence_at.emplace(starts[p] + shift, sequence->sequence);
        }
    }

    return sequence_at;
}

// The sequence of `file`, a file of a DS sound archive, that each sequence of the INFO block's table plays, by its
// index there: the sequence that its file became (ntdlib::sdat::Cut). The sounds of the archive's sequence archives
// come after those of the table (song::DsSounds), and have none: the cut keeps a sequence archive among the archive's
// bytes. Nothing either for an entry that has no sequence, or whose file stays among the bytes. `sdat` is the archive
// that `file` builds (ExportArchive). Returns nothing for a file of another console. Throws the exceptions of
// ntdlib::ntr::snd::EncodeSequence.
std::vector<std::optional<uint32_t>> DsSoundSequences(const File& file, std::span<const uint8_t> sdat);

// The sequence of `file`, a file of a set of DSE's files, that each of `sounds` plays, by its place there: the sequence
// of the song or of the sound effect (ntdlib::dse::Cut). Nothing for a sound whose file stays among the bytes. `set` is
// the set that `file` builds (ExportArchive). Returns nothing for a file of another console. Throws the exceptions of
// ntdlib::dse::EncodeTracks and of ntdlib::dse::ReadSet.
std::vector<std::optional<uint32_t>> DseSoundSequences(const File& file, std::span<const uint8_t> set,
                                                       std::span<const ntdlib::dse::SequenceRef> sounds);

// The sequence of `file`, a file of a GBA ROM, that contains song `song` of the song table at `song_table`, a ROM
// address: the song's piece. Nothing for a song that the cut leaves among the ROM's bytes, and for a file of another
// console.
std::optional<uint32_t> GbaSongSequence(const File& file, uint32_t song_table, int song);

// The fields of `file`'s native layout that start the tracks of each of the first `songs` songs of the song table at
// `song_table`, by the song's number: the fields of the song's header, in the header's order. Two numbers that name one
// header give the same fields. Nothing for a song that the cut leaves among the ROM's bytes, and for a file of another
// console.
std::vector<std::vector<uint32_t>> GbaTrackFields(const File& file, uint32_t song_table, std::size_t songs);

// The sequence of `file`, a file of a ROM with Konami's driver, that contains song `song` of the song table at
// `song_table`, a ROM address: the sequence of the piece that the song's entry names. Nothing for a song that stays
// among the ROM's bytes.
std::optional<uint32_t> KonamiSongSequence(const File& file, uint32_t song_table, int song);

// The fields of `file`, a file of a ROM with Konami's driver, that name the starts of the tracks of song `song` of the
// song table at `song_table`, in the tracks' order. Empty for a song that stays among the ROM's bytes.
std::vector<uint32_t> KonamiTrackFields(const File& file, uint32_t song_table, int song);

// The sequences of `file`, a file of a GBA ROM, with songs that outgrow their places in the ROM or headers that gain
// tracks. An export moves them into the ROM's free space (ntdlib::mp2k::Join). Returns nothing for a file of another
// console, and for a driver whose songs are in lists with rooms (DriverFacts::rooms). Throws std::invalid_argument for
// a song that MP2K's bytes can't hold.
std::vector<uint32_t> MovedSongs(const File& file);

// Whether `file`, a file of a GBA ROM, contains songs that outgrow their places while the ROM has no room for them
// after its data: they would make the ROM larger than the 32 MiB that the GBA maps. An export then puts them into the
// ROM's filler runs where the file allows it (NativeArchive::gba_filler), and refuses the file otherwise. Returns false
// for a file of another console, and for a driver that can't put songs into filler runs (DriverFacts::filler_runs).
// Throws the exceptions of ExportArchive other than the refusal.
bool LacksRoom(const File& file);

// `version`, a file of a GBA ROM, with the songs of `sequences` as `original`, an earlier version of the file, contains
// them: their sequences, and the headers at the starts of the runs after them, with the fields of the headers' tracks.
// The edits of the songs then stay out of an export. Throws std::invalid_argument when either file isn't a file of a
// GBA ROM, and when the pieces of the files differ in number or in the sequence of a piece of `sequences`.
File WithSongsOf(const File& version, const File& original, std::span<const uint32_t> sequences);

// The bytes of the archive or the ROM that the program plays for `file`. The songs of a GBA ROM that don't fit after
// its data move into its filler runs, whatever the file's choice: the program plays the edits that an export without
// filler runs leaves out. Otherwise, the bytes of ExportArchive.
std::vector<uint8_t> PlayedBytes(const File& file);

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
// It throws there for a value that the archive's version doesn't hold, such as a pan mode before version 1.2. A GBA ROM
// keeps its songs in their places where they fit, and the others go into the ROM's free space (ntdlib::mp2k::Join).
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
