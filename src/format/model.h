// SPDX-License-Identifier: MIT

// A file's archive in Hokuto's model form: the items of the archive's tables and its files, without the layout of the
// archive's bytes. ntdlib reads the items of an archive of the 3DS, the Wii U or the Switch that NintendoWare's tools
// laid out (ntdlib::sar::ReadArchiveModel). It lays an archive out from them as the tools do
// (ntdlib::sar::BuildArchive). ntdlib does the same for an archive of the Wii (ntdlib::rsar::ReadArchiveModel and
// ntdlib::rsar::BuildArchive). The file's sequences take the place of the bytes of their sequence files, and a sequence
// sound starts at an item of its sequence. The model keeps the group files beside the archive and the streams whole. A
// file in the model form keeps the model in its SNDS and ITMS chunks, in place of the native layout. In memory, the
// file holds the native archive that the model builds, and the edits work on that archive as on any other.
//
// The model keeps none of the encoder's choices (ntdlib::commands::Encoding), and no byte that the tools' layout
// doesn't give. An export lays every file out again. The archive plays the same as the archive that the model came
// from. Its bytes can differ.

#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "ntdlib/commands/units.h"
#include "ntdlib/nwsnd/commands.h"
#include "ntdlib/rsar/archive_model.h"
#include "ntdlib/sar/archive_model.h"
#include "ntdlib/sound_build.h"

#include "format/file.h"

namespace hokuto::format
{

// A file of the model's file table that holds one of the file's sequences: the sequence, and the version of the
// sequence file.
struct ModelSequence
{
    friend bool operator==(const ModelSequence&, const ModelSequence&) = default;

    uint32_t sequence = 0;
    uint32_t version = 0;
};

// The model of a file's archive. A file of `sequences` that the FILE block holds has no bytes in `archive`: its
// sequence gives them. A copy of a file of `sequences` in a group file keeps its bytes, and WithModel gives the copy
// the bytes of the file's sequence. A sequence file that only group files hold has its copies there. A sequence sound
// of `starts` starts at the item that `starts` gives, in its file's sequence, and its start offset stays 0. Any other
// sequence sound starts at its offset. The group files beside the archive come in the order of the native archive's
// group files, each with the index of its group in the archive's groups where Hokuto found it (NativeGroupFile::group).
// `streams` holds the files beside the archive whose bytes the file holds, such as streams. A model of the Wii's
// archive holds `wii` in place of `archive`, and no group files. It keeps the streams beside the archive in `streams`,
// and the game's build of NW4R SND too. Without one, the archive's version gives the build (BuildOfWiiArchive). A file
// of the Wii's model takes its bytes from the sequence in every group that holds it.
struct Model
{
    friend bool operator==(const Model&, const Model&) = default;

    ntdlib::sar::ArchiveModel archive;
    std::optional<ntdlib::rsar::ArchiveModel> wii;
    std::optional<ntdlib::SndBuild> wii_build;
    std::map<uint32_t, ModelSequence> sequences; // by the file's ID
    std::map<uint32_t, uint32_t> starts;         // by the sound's index in the sound table
    std::vector<std::vector<uint8_t>> group_files;
    std::vector<std::optional<uint32_t>> group_file_groups; // by the group files' order
    std::map<uint32_t, std::vector<uint8_t>> streams;       // by the file's ID
};

// The model of `file`'s archive: the model of the archive that `file`'s native archive builds, with the sequences
// without the encoder's choices. Throws std::invalid_argument for a file without a native archive, for a sound that
// starts inside a command, and the exceptions of ExportArchive. Throws xsflib::FormatError for an archive that ntdlib's
// model doesn't hold, such as one that NintendoWare's tools didn't lay out.
Model ModelOf(const File& file);

// `file` with the archive of `model`: the native archive that the model builds with `file`'s sequences, and the form
// kModel. The group files' copies of the sequence files take the bytes of `file`'s sequences. The group files and the
// streams count as Hokuto's, and an export writes each one. The file keeps no game's sizes. Throws xsflib::FormatError
// with a message for the user for a model that names a sequence that `file` lacks or an item past its list, for a model
// that ntdlib can't lay out, for a group file whose copies can't take the bytes, and for an archive whose sequence
// files don't cut to `file`'s sequences.
File WithModel(File file, const Model& model);

// The rules by which the console of `model` reads its sequences. On the Wii, the model's build of NW4R SND gives them,
// or else its version (BuildOfWiiVersion).
ntdlib::nwsnd::Grammar GrammarOf(const Model& model);

// `file` in the model form: its sequences without the encoder's choices, with the archive that their model builds.
// Throws the exceptions of ModelOf and WithModel.
File InModelForm(File file);

// The name of a new file's player.
inline constexpr std::string_view kDefaultPlayer = "PLAYER_DEFAULT";

// A new file for `engine`, in the model form, with strict mixing. Its archive holds one player, kDefaultPlayer, and
// nothing else. The player plays one sound at a time. The archive takes FSAR 2.2.0 on the Switch and 2.0.0 on the Wii
// U, and CSAR 2.0.0.0 on the 3DS: the oldest versions of the archives of each console that Hokuto lays out. On the Wii,
// it takes RSAR 1.4, and the archive's version gives the build of NW4R SND: a build of June 2008. That is the latest
// version and build that Hokuto models, with the most of the later consoles' commands and banks' values.
File NewFile(Engine engine);

// `sequences` without the encoder's choices. A sequence without choices stays shared.
std::vector<Shared<ntdlib::commands::Sequence>> WithoutChoices(
    const std::vector<Shared<ntdlib::commands::Sequence>>& sequences);

// The chunks that keep a file's archive in the model form, in both forms of the file.
inline constexpr std::string_view kSoundsChunk = "SNDS";
inline constexpr std::string_view kItemsChunk = "ITMS";

// The bytes of `model`'s SNDS chunk: the sounds, with their values in Hokuto's units (docs/format.md).
std::vector<uint8_t> SoundsChunk(const Model& model);

// The bytes of `model`'s ITMS chunk: the archive's kind and version, its other tables, its files, and the group files
// beside it. On the Wii, it also holds the layout's choices and the game's build of NW4R SND.
std::vector<uint8_t> ItemsChunk(const Model& model);

// The model that the SNDS chunk `sounds` and the ITMS chunk `items` hold. A value in Hokuto's units takes the nearest
// value of the console. Throws xsflib::FormatError with a message for the user for a damaged chunk, and for a chunk
// that a later version of Hokuto wrote.
Model ReadModel(std::span<const uint8_t> sounds, std::span<const uint8_t> items);

// A sound's volume or a stream track's volume as a level in Hokuto's units, and the volume nearest a level. The
// console's volume v is a gain of v / 127, and a level l a gain of (l / 127) squared. Each volume from 0 to 255 comes
// back from its level. A level past the volumes takes the nearest end.
ntdlib::commands::Fixed LevelOfVolume(uint8_t volume);
uint8_t VolumeOfLevel(ntdlib::commands::Fixed level);

} // namespace hokuto::format
