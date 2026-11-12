// SPDX-License-Identifier: MIT

// The values of a GBA song that the Sound sections edit: its music player, its priority and its reverb. The song table
// stores the number of the music player of the song, and the header of the song stores the other two. An edit changes
// those bytes in place, in the runs of the file's ROM (ntdlib/mp2k/layout.h). The ROM keeps its size. A song has no
// other value of the tables: its commands set its volume and its pans, and it plays one voice group.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "edit/native_runs.h"
#include "edit/tables.h"
#include "format/file.h"
#include "song/gba.h"

namespace hokuto::edit
{

// The bit of a song's reverb that sets the driver's reverb when the song starts. The low 7 bits give the level. Without
// the bit, the song keeps the reverb that the driver sets.
inline constexpr uint32_t kGbaReverbOn = 0x80;

// A GBA song's header: the count of tracks, the count of blocks, the priority, the reverb and the voice group's
// address. The field of the song's first track follows it.
inline constexpr uint32_t kGbaHeaderPriority = 2;
inline constexpr uint32_t kGbaHeaderReverb = 3;
inline constexpr uint32_t kGbaHeaderVoices = 4;
inline constexpr uint32_t kGbaHeaderSize = 8;

// The run of `native` that contains a GBA song's header, and the header's offset in it. `first_field` is the field of
// the song's first track (format::GbaTrackFields). Nothing where no run contains the header before that field.
std::optional<RunPlace> GbaHeaderPlace(const format::NativeArchive& native, uint32_t first_field);

// The values that a GBA song contains, for the Sound sections.
HeldValues GbaHeldValues();

// The values of song `song` of `rom` in the form that the Sound sections show. The player gets an item ID of that kind,
// with its index in the driver's table as its number. Throws std::invalid_argument with a message for the user for a
// song that is missing from the ROM's song table or can't be read.
format::NativeSound GbaNativeSoundOf(const song::GbaRom& rom, uint32_t song);

// The names of the music players of `rom`, by their indices: the number of each one and the tracks that it plays.
std::vector<std::string> GbaPlayerNames(const song::GbaRom& rom);

// `file`, a GBA file, with `change` in song `song`'s values. `rom` is the ROM that the file builds. Two numbers of the
// song table can name one header, and both then take a new priority or reverb. Each number has a music player of its
// own. Throws std::invalid_argument with a message for the user for a value that a song doesn't have, a player that is
// missing from the driver or plays no tracks, a number past 255, and bytes that no run of the ROM contains.
format::File ChangeGbaSong(const format::File& file, const song::GbaRom& rom, uint32_t song, const SoundChange& change);

} // namespace hokuto::edit
