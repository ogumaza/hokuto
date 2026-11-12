// SPDX-License-Identifier: MIT

// The values of a DS sound that the Sound sections edit: its bank, its volume, its two priorities and its player. A
// sound of the table of sequences stores them in its record of the INFO block of the archive, and a sound of a sequence
// archive in its record of the file of the sequence archive. An edit changes those bytes in place, in the runs of the
// file's archive (edit/native_runs.h). The archive keeps its size. A DS sound has no other value of the tables: the
// commands of the sound allocate its tracks, and the commands and the regions of its bank set its pans.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "edit/ds_new_items.h"
#include "edit/tables.h"
#include "format/file.h"
#include "song/ds.h"

namespace hokuto::edit
{

// The highest volume of a DS sound. The driver treats a higher volume as this one.
inline constexpr uint32_t kHighestDsVolume = 127;

// The values that a DS sound contains, for the Sound sections.
HeldValues DsHeldValues();

// The values of sound `sound` of `archive` in the form that the Sound sections show. The player and the bank take item
// IDs of those kinds, with their indices in the archive's tables as their numbers. Throws std::invalid_argument with a
// message for the user for a sound that is missing from the archive, and xsflib::FormatError for a sequence archive
// that can't be read.
format::NativeSound DsNativeSoundOf(const song::DsArchive& archive, uint32_t sound);

// The names of `archive`'s players, by their indices. A player without a name, or without a record, is named by its
// number.
std::vector<std::string> DsPlayerNames(const song::DsArchive& archive);

// `file` with `values` in sound `sound`'s record. `archive` is the archive that `file` builds. Throws
// std::invalid_argument with a message for the user for a sound that is missing from the archive, a volume or a
// priority past its highest, a new bank or player that the archive has no record of, and bytes that no run of the
// archive contains.
format::File WithDsSoundValues(const format::File& file, const song::DsArchive& archive, uint32_t sound,
                               const DsSoundValues& values);

// `file` with `change` in sound `sound`'s record. The values follow DsNativeSoundOf. Throws std::invalid_argument with
// a message for the user for a value that a DS sound doesn't hold, and as WithDsSoundValues does.
format::File ChangeDsSound(const format::File& file, const song::DsArchive& archive, uint32_t sound,
                           const SoundChange& change);

} // namespace hokuto::edit
