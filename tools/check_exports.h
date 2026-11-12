// SPDX-License-Identifier: MIT

// The parts of hokuto_check_exports that its files share: the counts, the sides of a comparison and the checks of
// each console family. check_exports.cpp describes the checks.

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/ntr/game_profiles.h"
#include "ntdlib/ntr/rom.h"
#include "ntdlib/rsar/formats.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sequence_export/agb/settings.h"
#include "ntdlib/sequence_export/archive_converter.h"
#include "ntdlib/sequence_export/brownie/convert.h"
#include "ntdlib/sequence_export/consoles.h"
#include "ntdlib/sequence_export/convert.h"
#include "ntdlib/sequence_export/dse/convert.h"
#include "ntdlib/sequence_export/konami/convert.h"
#include "ntdlib/sequence_export/krawall/convert.h"
#include "ntdlib/sequence_export/mp2k/convert.h"
#include "ntdlib/sequence_export/mp2k/performance.h"
#include "ntdlib/sequence_export/ntr/convert.h"
#include "ntdlib/sequence_export/quintet/convert.h"
#include "ntdlib/sequence_export/rare/convert.h"
#include "ntdlib/sequence_export/rd2/convert.h"
#include "ntdlib/sequence_export/ubimilan/convert.h"
#include "seqlib/midi/performance.h"
#include "seqlib/sf2/reader.h"
#include "xsflib/bytes.h"
#include "xsflib/console.h"
#include "xsflib/files.h"

#include "edit/ds_files.h"
#include "format/archive.h"
#include "format/drivers.h"
#include "format/file.h"
#include "song/archive.h"
#include "song/brownie.h"
#include "song/ds.h"
#include "song/dse.h"
#include "song/gba.h"
#include "song/konami.h"
#include "song/krawall.h"
#include "song/midi_export.h"
#include "song/quintet.h"
#include "song/rare.h"
#include "song/rd2.h"
#include "song/ubimilan.h"

namespace hokuto::tools::exports
{

namespace export_ = ntdlib::sequence_export;
namespace rsar = ntdlib::rsar;
namespace sar = ntdlib::sar;

// One side's conversion of a sound, or its error.
struct Side
{
    std::optional<export_::Conversion> conversion;
    std::string error;
};

// What a check of an archive's sounds found.
struct Counts
{
    std::size_t sounds = 0;
    std::size_t differ = 0;
    std::size_t failed = 0;
};

// Whether `a` and `b` hold the same SFZ files, in their order.
bool SameSfzFiles(const export_::SfzInstruments& a, const export_::SfzInstruments& b);

// Whether `a` and `b` hold the same SFZ samples, in their order.
bool SameSfzSamples(const export_::SfzInstruments& a, const export_::SfzInstruments& b);

// The parts joined into a list for a line: "a, b and c".
std::string Listed(const std::vector<std::string>& parts);

// Counts sound `sound_name` of `name`, and prints what differs between the two sides' conversions. Hokuto's message
// names the sound before ntdlib's.
void CompareSides(const Side& theirs, const Side& ours, const std::string& name, const std::string& sound_name,
                  Counts& counts);

// Checks the conversions of the sounds of the GBA ROM with another of the GBA's drivers in `bytes`, named `name`.
// `only` names a sound, such as Song 05, to check alone. `reference_folder` names the folder of the reference files, or
// nothing, and `stem` the start of their names. Both sides name `program` as the program that made the files. Returns
// the tool's exit status.
int CheckDriverRom(std::vector<uint8_t> bytes, const std::string& name, const std::string& only,
                   const std::filesystem::path& reference_folder, const std::string& stem, const std::string& program);

// Checks the conversions of the songs of the GBA ROM in `bytes`, named `name`. `only` names a song, such as Song 05, to
// check alone. Both sides name `program` as the program that made the files. Returns the tool's exit status.
int CheckGbaRom(std::vector<uint8_t> bytes, const std::string& name, const std::string& only,
                const std::string& program);

// Checks the conversions of the sounds of each sound archive of the DS cartridge image in `bytes`, or of the sound
// archive of the DS in `bytes`, named `name`. Both sides name `program` as the program that made the files. Returns the
// tool's exit status.
int CheckDs(std::vector<uint8_t> bytes, const std::string& name, const std::string& only, const std::string& program);

// Checks the conversions of the DSE sounds of the DS cartridge image in `bytes`, named `name`, against those that the
// set of the game's image produces. ntdlib's frame of a DSE converter makes both conversions. `only` names a sound to
// check alone. Both sides name `program` as the program that made the files. Returns the tool's exit status.
int CheckDse(const std::vector<uint8_t>& bytes, const std::string& name, const std::string& only,
             const std::string& program);

} // namespace hokuto::tools::exports
