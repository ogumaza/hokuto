// SPDX-License-Identifier: MIT

// The parts of hokuto_check_layout that its files share: the totals, the helpers and the checks. check_layout.cpp
// describes the checks.

#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/commands/units.h"
#include "ntdlib/nwsnd/commands.h"
#include "ntdlib/nwsnd/console.h"
#include "ntdlib/rsar/formats.h"
#include "ntdlib/rsar/layout.h"
#include "ntdlib/rsar/sequence_file.h"
#include "ntdlib/sar/bank_edit.h"
#include "ntdlib/sar/byte_order.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sar/groups.h"
#include "ntdlib/sar/info_block.h"
#include "ntdlib/sar/layout.h"
#include "ntdlib/sar/sarc.h"
#include "ntdlib/sar/sequence_file.h"
#include "ntdlib/sar/string_block.h"
#include "ntdlib/sar/tables.h"
#include "ntdlib/sar/wave_file.h"
#include "ntdlib/testing/sar_tools.h"
#include "seqlib/file_name.h"
#include "xsflib/binary.h"
#include "xsflib/console.h"
#include "xsflib/files.h"

#include "format/archive.h"
#include "song/archive.h"

namespace hokuto::tools::layout
{

namespace commands = ntdlib::commands;
namespace nwsnd = ntdlib::nwsnd;
namespace sar = ntdlib::sar;

// What the check counted, for the summary.
struct Totals
{
    int archives = 0;
    int problems = 0;
    int sequences = 0;
    int pieces = 0;
    int fields = 0;
    int files = 0;
    int groups = 0;
    int starts = 0;
    int strict = 0;
    int same = 0;
    int unreached = 0;
    int left = 0;
    int values = 0;
    int runs = 0;
    int run_files = 0;
    int run_groups = 0;
    int replaced = 0;
    int name_blocks = 0;
    int names = 0;
    int nameless = 0;
    int info_blocks = 0;
    int entries = 0;
    int moved = 0;
    int new_lead = 0;
    int cut_short = 0;
    int new_sounds = 0;
    int new_pieces = 0;
    int no_model = 0;
    int new_banks = 0;
    int new_wave_archives = 0;
    int listed = 0;
    int no_bank = 0;
    int bank_lists = 0;
    int longer_lists = 0;
    int shorter_lists = 0;
    int new_wave_sounds = 0;
    int wave_sound_archives = 0;
    int from_sequences = 0;
    int no_wave_sound = 0;
    int external_files = 0;
    int new_streams = 0;
    int stream_archives = 0;
    int stream_extras = 0;  // copies of stream sounds whose details name an extension or a prefetch file
    int stream_refused = 0; // copies in 3DS archives of versions whose stream sounds ntdlib doesn't lay out
    std::map<std::string, std::pair<int, int>> stream_layouts; // the copies of other kinds with the stream sound's
                                                               // details and all of them, by the archive's version
    int place_archives = 0;
    int place_sounds = 0;
    int pan_places = 0;
    int priority_places = 0;
    int channel_places = 0;
    int option_archives = 0;
    int pan_options = 0;
    int priority_options = 0;
    int channel_options = 0;
    std::map<std::string, std::pair<int, int>> kinds; // the files in runs and all files, by the magic at their starts
    int groups_inside = 0;                            // groups' files in the archives that GroupWith gave back
    int group_archives = 0;                           // archives with group files beside them
    int group_files = 0;                              // group files beside the archives
    int group_replaced = 0;                           // files in the group files that took 40 more bytes
    int group_sequences = 0;                          // copies of sequence files in the group files
    int group_only_sequences = 0;                     // sequences of the sequence files that only the group files hold
    int group_only_starts = 0;                        // the fields of their sounds' starts
    int group_copies = 0;   // the copies that took the bytes, in the archives and the group files
    int sarcs = 0;          // SARC archives beside the archives
    int sarc_writes = 0;    // SARC archives that took new group files after ReplaceFile
    int managed_writes = 0; // new group files that went into a userManagementFiles folder after ReplaceFile
    int instrument_banks = 0;
    int unread_banks = 0;
    int empty_banks = 0;
    int loose_banks = 0;
    int copies = 0;
    int gap_copies = 0;
    int new_waves = 0;
    int same_runs = 0;
    int first_wave_runs = 0;
    int swapped_banks = 0;
    int wii_archives = 0;
    int wii_sequences = 0;
    int wii_pieces = 0;
    int wii_fields = 0;
    int wii_files = 0; // sequence files read back after the growth
    int wii_left = 0;  // sequence files that stay in the bytes
    int64_t wii_bytes = 0;
};

// The grammar of the sequences of an archive of `flavor`.
nwsnd::Grammar GrammarFor(sar::Flavor flavor);

// The file of `sequence` as the writer writes it.
std::vector<uint8_t> FileOf(const commands::Sequence& sequence, uint32_t version, sar::Flavor flavor);

// The sequence of each sequence piece, in order.
std::vector<uint32_t> SequencesOfPieces(const sar::Layout& layout);

// Whether the fields of `a` and `b` lie at the same places, with items compared by their pieces only.
bool SamePlaces(const sar::Layout& a, const sar::Layout& b);

// `layout` with waits after the first item of each sequence, and the labels, targets and places moved. `added` gets the
// number of waits for each sequence.
sar::Layout Grow(const sar::Layout& layout, std::vector<uint32_t>& added);

// Where each of the original's pieces starts, for the sequence piece at a file's start.
std::map<std::size_t, std::size_t> PieceStarts(const sar::Layout& layout);

// Prints problem `what` of the archive at `path`, and counts it.
void Problem(const std::filesystem::path& path, const std::string& what, Totals& totals);

// The STRG block of `archive`, from the header's table of blocks. Nothing for an archive without one.
std::optional<std::span<const uint8_t>> StringBlockOf(std::span<const uint8_t> archive, sar::Flavor flavor);

// The name that the reader gives item `item_id` of `archive`, for the types whose lists it reads. Nothing for another
// type.
std::optional<std::string> ReaderNameOf(const sar::SoundArchive& archive, uint32_t item_id);

// Checks that ntdlib's StringBlock writes the STRG block of `original` again from its names, byte for byte, that
// FindItem finds each name's item, and that the reader gives the items the same names.
void CheckNames(const std::filesystem::path& path, const std::vector<uint8_t>& original, const sar::Options& options,
                Totals& totals);

// Checks the files and the sounds' starts of `joined`, the bytes of `grown`, with the reader.
void CheckTables(const std::filesystem::path& path, const std::vector<uint8_t>& original,
                 const std::vector<uint8_t>& joined, const sar::Layout& layout, const sar::Layout& grown,
                 const std::vector<uint32_t>& added, const sar::Options& options, Totals& totals);

// Whether `a` and `b` hold the same values, but for the volume and the tracks' flags.
bool SameOtherwise(const sar::SoundInfo& a, const sar::SoundInfo& b);

// The bytes that a file of `size` bytes gains: 8 to 80.
std::vector<uint8_t> Gain(std::size_t size);

// The magic at the start of `file`, or "other" when it isn't four letters.
std::string KindOf(std::span<const uint8_t> file);

// Checks the groups of `original`, an archive at `path`, and the group files beside it. `layout` is the cut of the
// archive alone. The 3DS's file table holds no lists of groups. There, the name under which the tool finds a group file
// beside the archive gives its group (ntdlib::sar::SoundArchive::GroupFileGroups).
void CheckGroupFiles(const std::filesystem::path& path, const std::vector<uint8_t>& original, const sar::Layout& layout,
                     const sar::Options& options, Totals& totals);

// Checks the runs of files that `layout`, the cut of `original`, holds, after each one grows.
void CheckRuns(const std::filesystem::path& path, const std::vector<uint8_t>& original, const sar::Layout& layout,
               const sar::Options& options, Totals& totals);

// Checks the writing of the tables' values, where the strict rules read the archive.
void CheckValues(const std::filesystem::path& path, const std::vector<uint8_t>& original, sar::Options options,
                 Totals& totals);

// Checks the places of the sounds' optional values, where the strict rules read the archive.
void CheckPlaces(const std::filesystem::path& path, const std::vector<uint8_t>& original, const sar::Layout& layout,
                 sar::Options options, Totals& totals);

// Whether `a` and `b` hold the same values.
bool SameSound(const sar::SoundInfo& a, const sar::SoundInfo& b);

// How many items of each kind an archive gains.
struct Growth
{
    std::size_t sounds = 0;
    std::size_t banks = 0;
    std::size_t wave_archives = 0;
    std::size_t files = 0;
};

// Whether `b` reads the items of `a` other than its sounds the same, with the same names, and the same files. `b` holds
// the banks, wave archives and files of `growth` after them.
bool SameOtherItems(const sar::SoundArchive& a, const sar::SoundArchive& b, const Growth& growth);

// Whether `b` reads the items of `a` the same, with the same names, and the same files. `b` holds the items of `growth`
// after them.
bool Extends(const sar::SoundArchive& a, const sar::SoundArchive& b, const Growth& growth);

// Whether the reader reads the same items from `a` and `b`, with the same names, and the same files.
bool SameReading(const sar::SoundArchive& a, const sar::SoundArchive& b);

// The offset of the FILE block of `archive`, from the header's table of blocks.
std::size_t FileBlockOf(std::span<const uint8_t> archive, sar::Flavor flavor);

// The distance from `block`, the start of a FILE block in an archive of `flavor`, to the block's first file.
std::size_t LeadOf(std::size_t block, sar::Flavor flavor);

// Checks ntdlib's writing of the INFO block of `original` and its replacement. `layout` is the cut of `original`.
void CheckInfo(const std::filesystem::path& path, const std::vector<uint8_t>& original, const sar::Layout& layout,
               const sar::Options& options, Totals& totals);

// Whether each entry of `archive`'s INFO block that holds a string index names its item there, and whether the string
// table holds the names in the order of the tables. The options of an entry start 20 bytes in for a sound, 24 for a
// sound group, 12 for a bank, 8 for a wave archive, and 4 for a group and a player. Bit 0 gives the string index.
bool NamesInOrder(std::span<const uint8_t> archive, sar::Flavor flavor);

// A name that `archive`'s STRG block lacks, from `stem`, or none for an archive without one.
std::string FreeName(std::span<const uint8_t> archive, sar::Flavor flavor, const std::string& stem);

// Checks ntdlib's new file and new sequence sound in `original`, with its cut `layout`. The model is the first sequence
// sound whose file the reader reads.
void CheckNewItems(const std::filesystem::path& path, const std::vector<uint8_t>& original, const sar::Layout& layout,
                   const sar::Options& options, Totals& totals);

// Checks ntdlib's new wave sounds in `original`, with its cut `layout`. A copy of the first wave sound file that the
// reader reads takes a copy of each wave sound that the file holds, at its index. A wave sound copied from the first
// sequence sound follows, at index 0.
void CheckNewWaveSounds(const std::filesystem::path& path, const std::vector<uint8_t>& original,
                        const sar::Layout& layout, const sar::Options& options, Totals& totals);

// Checks ntdlib's new files outside the archive and new stream sounds in `original`.
void CheckNewStreams(const std::filesystem::path& path, const std::vector<uint8_t>& original,
                     const sar::Options& options, Totals& totals);

// Checks ntdlib's new wave archives and new bank in `original`, with its cut `layout`. The model is the first bank
// whose file the reader reads, with waves in wave archives whose files the reader reads. Each of those wave archives
// gets a copy, with a name where the archive has names. The model's file then names the copies (WithWaveArchives) and
// goes in as a new bank. Its entry lists the copies where the model's entry lists wave archives.
void CheckNewBank(const std::filesystem::path& path, const std::vector<uint8_t>& original, const sar::Layout& layout,
                  const sar::Options& options, Totals& totals);

// Checks ntdlib's WithValuePlaces on `original`, with its cut `layout`, where the strict rules read the archive and it
// isn't cut short. Every option of a pan, of the priorities and of a channel priority leaves the sounds' entries, and
// WithValuePlaces gives them back.
void CheckValuePlaces(const std::filesystem::path& path, const std::vector<uint8_t>& original,
                      const sar::Layout& layout, sar::Options options, Totals& totals);

// Checks ntdlib's lists of banks in `original`, with its cut `layout`. Each sequence sound with fewer than four banks
// takes a list one slot longer, with an empty slot at its end. Then each sequence sound with banks takes a list one
// bank shorter.
void CheckBankLists(const std::filesystem::path& path, const std::vector<uint8_t>& original, const sar::Layout& layout,
                    const sar::Options& options, Totals& totals);

// Checks ntdlib's WithInstruments on each bank of `original` with instruments whose file the reader reads.
void CheckInstruments(const std::filesystem::path& path, const std::vector<uint8_t>& original,
                      const sar::Options& options, Totals& totals);

// Checks an archive of the Wii (RSAR) through Hokuto's import and ntdlib's rsar module: the round trip, then every
// sequence grown as Grow grows it. ntdlib's reader finds each file where it moved. A file that holds no sequence keeps
// its bytes, and so does each file's wave data. A sequence's file holds the grown commands as the writer writes them.
// Each group holds its files as NW4R's tools lay them out. The cut of the grown archive gives the grown commands and
// the fields at the same places. As in the program, the import takes the game's build of NW4R SND from the main.dol of
// the extracted disc that holds the archive. Without it, the archive's version gives the build.
void CheckWii(const std::filesystem::path& path, const std::vector<uint8_t>& original, Totals& totals);

} // namespace hokuto::tools::layout
