// SPDX-License-Identifier: MIT

// Projects: the archives of one Hokuto file. An archive is the part of an image that one driver plays, such as the
// songs of MusicPlayer2000 in a GBA ROM that holds Nintendo R&D2's driver too. A file contains one archive or more, and
// each archive is a File: its engine, its sequences, its native archive and what the file keeps for its sounds. A
// project of more than one archive comes from one GBA ROM. No other console has that form yet. docs/format.md gives
// the form of a file with more than one archive.
//
// Each archive cuts the whole ROM. The runs of an archive therefore contain the songs of the other archives
// as bytes. Where two archives both have a run, the two runs contain the same bytes. The import gives that state, and
// WithArchive and ExportProject keep it. The ROM has one free space, at its end. It starts past the bytes that any of
// the drivers reads: NativeDriver::free_start of a driver other than MusicPlayer2000, and
// NativeArchive::gba_free_floor of MusicPlayer2000's archive.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "format/file.h"

namespace hokuto::format
{

// The archives of a file, in the order of their engines' numbers, and the archive in front: the one that the program
// shows. The mixing mode, the effects and the volumes of the buses, the tags of the file and the choice of the filler
// runs belong to the project. The File of each archive contains the same values.
struct Project
{
    friend bool operator==(const Project&, const Project&) = default;

    std::vector<File> archives;
    std::size_t front = 0;

    // With more than one archive, the chunks outside them that Hokuto doesn't read.
    std::vector<OtherChunk> other_chunks;
};

// A project of `file` alone.
Project ProjectOf(File file);

// The archives that a GBA ROM contains, and a message for each archive that the import leaves out.
struct ImportedArchives
{
    Project project;
    std::vector<std::string> notes;
};

// The archives of `rom`, a GBA ROM. MusicPlayer2000's detection and that of each other driver run. Each driver whose
// cut gives a song or more becomes an archive. A ROM where no cut gives a song imports as it did before projects: with
// the first driver that the ROM contains, or else with MusicPlayer2000. An archive whose sequence pieces or fields
// overlap those of an earlier archive is left out, with a note that names its driver. The free space of each archive
// starts at the latest of the starts that the archives give. Throws xsflib::FormatError as ImportGbaRom does when the
// ROM contains no driver.
ImportedArchives ImportGbaArchives(std::span<const uint8_t> rom);

// `project` with `edited` in the place of archive `index`. An edit of archive `index` made `edited`. The bytes that the
// edit changed in the runs of the archive go into the runs of the other archives. The ROM's growth goes at the end of
// the last run of each. Bytes that arrive at or past the free space of another archive move its free space past them,
// to the next multiple of kFreeSpaceStep. The values of the project go to the other archives. Throws
// std::invalid_argument when the edit changes a byte that another archive keeps in a sequence piece: two drivers would
// then use the same bytes.
Project WithArchive(Project project, std::size_t index, File edited);

// `archive` without the values that a file with more than one archive keeps once for every archive: the effects and
// the volumes of the buses, and the file's tags.
File WithoutProjectValues(File archive);

// The step to which a free space that bytes of another archive reach moves.
inline constexpr uint64_t kFreeSpaceStep = 256;

// The image that `project` exports: each archive's join in turn. After the join of an archive, its changes go into the
// runs of the archives after it, as WithArchive takes an edit. The join of the last archive gives the image. A project
// of one archive exports that archive (ExportArchive). Throws std::invalid_argument as ExportArchive does, and when a
// join changes a byte that a later archive keeps in a sequence piece.
std::vector<uint8_t> ExportProject(const Project& project);

// What's wrong with the archives of `project`, or nothing. A project has an archive at least, and its archive in front
// is one of them. With more than one archive, each is a native archive of an engine of the GBA. Their engines come in
// the order of their numbers. Their mixing mode and their choice of the filler runs are the project's, and no archive
// takes the filler runs. The archives give one size of the game's ROM, and the bytes of two archives agree wherever
// both have a run. The sequence pieces and the fields of two archives share no byte.
std::optional<std::string> ProjectProblem(const Project& project);

// `project` with the free space of each archive of MusicPlayer2000 at the latest start that the other archives store
// (NativeArchive::gba_free_floor). The file doesn't store the floor. Each reader sets it.
Project WithFreeFloors(Project project);

// The binary form of `project`. A project of one archive takes the form of a file (Write). Throws std::invalid_argument
// as Write does, and for a project that ProjectProblem refuses.
std::vector<uint8_t> WriteProject(const Project& project);

// The text form of `project`, as WriteProject gives the binary form (WriteText).
std::string WriteProjectText(const Project& project);

// Reads a project in either form: a file of one archive or more. Throws xsflib::FormatError with a message for the user
// as Read and ReadText do, and for a file whose archives ProjectProblem refuses.
Project ReadProject(std::span<const uint8_t> bytes);

// The project of a file in the binary form, and of one in the text form, as ReadProject reads them.
Project ReadBinaryProject(std::span<const uint8_t> bytes);
Project ReadTextProject(std::string_view text);

} // namespace hokuto::format
