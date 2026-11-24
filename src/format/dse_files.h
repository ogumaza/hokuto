// SPDX-License-Identifier: MIT

// The DSE files of a DS cartridge image, as ntdlib's converter finds them: the files of the file system that are DSE
// files as they are, and the DSE files inside the game's packs (ntdlib::dse::Decompressed and FindFiles). A pack is a
// file of the image that holds DSE files behind its headers, compressed or not. The format layer and the song layer
// share the search, and the levels at which the game starts its songs (DseGameStarts).

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "ntdlib/dse/game_starts.h"
#include "ntdlib/dse/sets.h"
#include "ntdlib/ntr/rom.h"

#include "format/dse_places.h"
#include "format/starts.h"

namespace hokuto::format
{

// A DSE file of a cartridge image: its path and bytes, and the file of the image that holds it.
struct DseImageFile
{
    ntdlib::dse::NamedFile named;

    // The number and the name (DseFileName) of the image's file that holds it.
    uint32_t image_file = 0;
    std::string image_path;

    // Whether the image's file is the DSE file, byte for byte. A file inside a pack, or one that the image keeps
    // compressed, isn't.
    bool loose = false;

    // Where the bytes sit in the image's file, or in its decompressed bytes (DsePlace).
    uint32_t offset = 0;
    bool compressed = false;
};

// The DSE files of the cartridge image `rom`, in the order that ntdlib's converter finds them. A file of the image
// takes its path, or "file " and its number where it has no path. A file inside a pack takes the pack's path, a '/' and
// its name in the pack (ntdlib::dse::FindFiles). `storage` keeps the decompressed bytes for as long as the list is in
// use.
std::vector<DseImageFile> DseFilesOf(const ntdlib::ntr::Rom& rom, std::vector<std::vector<uint8_t>>& storage);

// The DSE files of the cartridge image `rom` at `places`, the record that the import made (DsePlaceOf), in the order of
// the record. Only the image's files of compressed places are decompressed, and `storage` keeps their bytes for as
// long as the list is in use. Throws std::invalid_argument where the image has no file that a place names, or a place
// runs past the bytes of its file.
std::vector<DseImageFile> DseFilesAt(const ntdlib::ntr::Rom& rom, std::span<const DsePlace> places,
                                     std::vector<std::vector<uint8_t>>& storage);

// The place of `file` in its image, for the record of a set's files.
DsePlace DsePlaceOf(const DseImageFile& file);

// The name of `file` among the DSE files of an image: its path, or "file " and its number for a file without a path.
std::string DseFileName(const ntdlib::ntr::RomFile& file);

// The starts of the songs among `sounds`, the sounds of the DSE files of the cartridge image `rom` by their numbers
// (ntdlib::sequence_export::dse::ReadSequences). The game's code starts some songs below full level, and some in more
// than one state (ntdlib::dse::GameStarts): the first state is the song's start, and each other state a start named by
// its suffix, such as "_LEVEL256". A start sets only the level (SoundStart::dse_level). A song that the game starts at
// full level alone gets no start, and nor does a sound effect. Where the game's code differs from ntdlib's profile of
// it, the levels come from ntdlib's table of the game alone (ntdlib::dse::SongStatesOf), as ntdlib's converter takes
// them.
std::vector<SoundStart> DseGameStarts(const ntdlib::ntr::Rom& rom, std::span<const ntdlib::dse::SequenceRef> sounds);

// The same for `files`, the DSE files of `rom` (DseFilesOf, or a set of them that a file keeps), with the sounds that
// ntdlib's reader gives them.
std::vector<SoundStart> DseGameStarts(const ntdlib::ntr::Rom& rom, std::span<const ntdlib::dse::NamedFile> files);

// The levels of the songs of a cartridge image, as ntdlib's reader reads them from the game's code
// (ntdlib::dse::GameStarts). `read` is empty where the reader fails, and `problem` then gives the reason in a phrase.
struct DseLevels
{
    std::optional<ntdlib::dse::GameStarts> read;
    std::string problem;
};

// The levels of the songs of `rom`. The read fails where the code of the image differs from the code that ntdlib's
// profile of the game reads.
DseLevels ReadDseLevels(const ntdlib::ntr::Rom& rom);

// The note for the user where ReadDseLevels reads no levels: the songs play at the levels of ntdlib's table of the
// game (ntdlib::dse::SongStatesOf), or at full level. Nothing where it reads them. song::DseOpenNotes and
// App::OpenedFile give it.
std::optional<std::string> DseLevelsNote(const ntdlib::ntr::Rom& rom);

} // namespace hokuto::format
