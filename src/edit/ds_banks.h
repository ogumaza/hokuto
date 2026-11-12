// SPDX-License-Identifier: MIT

// The edits of a DS bank's regions (SBNK), for the Bank Editor. A region has 10 bytes of values: the index of a wave
// and the bank slot of the wave archive that contains the wave, or the duty of a square wave, then the root key, the
// envelope and the pan. An instrument of one region keeps the region's kind in the bank's table. A drum set or a key
// split keeps each region's kind in the byte before its values. An edit changes those bytes in place, in the run of the
// archive that contains the bank file (edit/native_runs.h). The archive keeps its size.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/sdat/formats.h"

#include "format/file.h"
#include "song/ds.h"

namespace hokuto::edit
{

// The highest values of a DS region: a square wave's duty, a slot of the bank's wave archives, and each byte of the
// root key, the envelope and the pan. The driver keeps a duty of 7 low: the square wave is silent.
inline constexpr int kHighestDsDuty = 7;
inline constexpr int kHighestDsSlot = 3;
inline constexpr int kHighestDsByte = 127;

// A region of a DS bank: the bank's index in the archive's table of banks, the program, and the region's index among
// the instrument's regions. An instrument of one region has region 0.
struct DsRegionPlace
{
    friend bool operator==(const DsRegionPlace&, const DsRegionPlace&) = default;

    uint32_t bank = 0;
    uint32_t program = 0;
    std::size_t region = 0;
};

// Where a region is in the bank file: the offset of its 10 bytes of values, and the offset of the byte that stores its
// kind.
struct DsRegionBytes
{
    friend bool operator==(const DsRegionBytes&, const DsRegionBytes&) = default;

    uint32_t values = 0;
    uint32_t kind = 0;
};

// Where region `region` of program `program` of `bank` is in the bank's file. Throws std::invalid_argument for a
// program past the bank's table, and for a region that the instrument doesn't hold.
DsRegionBytes DsRegionBytesOf(const ntdlib::sdat::Bank& bank, uint32_t program, std::size_t region);

// `file`, a file of a DS archive, with `region`'s kind and values at `place`. `archive` is the archive that `file`
// builds. A kind can change among a wave, a square wave and noise. A region whose bytes an instrument of one region at
// another program also uses keeps its kind: the kind of that instrument is elsewhere in the table. A wave has to be in
// a wave archive of the bank's slots, and a duty can't pass kHighestDsDuty. A value that the edit leaves unchanged
// isn't checked. Throws std::invalid_argument with a message for the user for a value that the region can't store, and
// for bytes that no run of the archive contains.
format::File WithDsRegion(const format::File& file, const song::DsArchive& archive, const DsRegionPlace& place,
                          const ntdlib::sdat::Region& region);

// The same, with `starts`, the starts of the pieces of `file`'s native layout (format::NativePieceStarts). An edit of a
// region keeps every piece's start, and a run of edits can find them once.
format::File WithDsRegion(const format::File& file, const song::DsArchive& archive, const DsRegionPlace& place,
                          const ntdlib::sdat::Region& region, std::span<const uint64_t> starts);

// The highest program of the table of a bank where the Bank Editor can add an instrument.
inline constexpr uint32_t kHighestDsProgram = 32767;

// The instruments of bank `bank` of `archive`. Throws std::invalid_argument with a message for the user for a bank that
// is missing from the archive or can't be read.
ntdlib::sdat::Bank DsBankModelOf(const song::DsArchive& archive, uint32_t bank);

// `file` once its bank `bank` uses a file laid out from `model` (ntdlib::sdat::BankFile). `archive` is the archive that
// `file` builds. The file can take a new size (edit/ds_files.h). The edit reaches every bank that uses the file. Throws
// std::invalid_argument with a message for the user for a model that the bank's file can't hold, and for a bank that is
// missing from the archive.
format::File WithDsBankModel(const format::File& file, const song::DsArchive& archive, uint32_t bank,
                             const ntdlib::sdat::Bank& model);

// The values of a new region: the first wave of the bank's slots when `wave` is given, or else a square wave of 50 %
// duty. It plays at full level, with a release of 125. No anchor shows the values of the SDK tools for a new region.
struct DsWavePlace
{
    uint16_t slot = 0;
    uint16_t wave = 0;
};
ntdlib::sdat::Region NewDsRegion(std::optional<DsWavePlace> wave);

// The first program from `start` on that has no instrument, up to kHighestDsProgram. The table's end counts.
std::optional<uint32_t> FreeDsProgram(const ntdlib::sdat::Bank& bank, uint32_t start);

// The model's edits of instruments. Each one writes separate bytes for a new or changed instrument: an edit of it
// reaches no other program. Each throws std::invalid_argument with a message for the user for a program that the edit
// can't take.
//
// An instrument of one region of `region`'s kind, at `program`. `program` has no instrument.
ntdlib::sdat::Bank WithNewDsInstrument(ntdlib::sdat::Bank bank, uint32_t program, const ntdlib::sdat::Region& region);
// A copy of the instrument at `from`, at `to`. `to` has no instrument.
ntdlib::sdat::Bank WithDsInstrumentCopy(ntdlib::sdat::Bank bank, uint32_t from, uint32_t to);
// The instrument at `from`, at `to`. `to` has no instrument.
ntdlib::sdat::Bank WithDsInstrumentMoved(ntdlib::sdat::Bank bank, uint32_t from, uint32_t to);
// `bank` without the instrument at `program`. The table keeps its length.
ntdlib::sdat::Bank WithoutDsInstrument(ntdlib::sdat::Bank bank, uint32_t program);

// An instrument of one region becomes a key split of one run over every key, or a drum set of the one key `key`.
ntdlib::sdat::Bank WithDsKeySplit(ntdlib::sdat::Bank bank, uint32_t program);
ntdlib::sdat::Bank WithDsDrumSet(ntdlib::sdat::Bank bank, uint32_t program, int key);
// A key split or a drum set becomes an instrument of its region `region`.
ntdlib::sdat::Bank WithDsOneRegion(ntdlib::sdat::Bank bank, uint32_t program, std::size_t region);
// Run `region` of a key split splits in two with the same values, and `key` starts the second. A key split has up to 8
// runs, and the first run ends at key 1 or later: a last key of 0 ends the instrument's list.
ntdlib::sdat::Bank WithDsSplit(ntdlib::sdat::Bank bank, uint32_t program, std::size_t region, int key);
// Run `region` of a key split is removed. The next run gets its keys, or for the last run the run before it.
ntdlib::sdat::Bank WithoutDsRun(ntdlib::sdat::Bank bank, uint32_t program, std::size_t region);
// A drum set plays the keys from `first` to `last`. A new key copies the values of the nearest key that the drum set
// had.
ntdlib::sdat::Bank WithDsDrumKeys(ntdlib::sdat::Bank bank, uint32_t program, int first, int last);
// Region `region` of the instrument at `program` plays `wave`. A region of any kind becomes a wave. Its other values
// stay.
ntdlib::sdat::Bank WithDsRegionWave(ntdlib::sdat::Bank bank, uint32_t program, std::size_t region, DsWavePlace wave);

// The places of `count` new waves after the waves of the wave archive in the first of bank `bank`'s slots that names
// one, and that wave archive. Throws std::invalid_argument with a message for the user for a bank whose slots name no
// wave archive, and for a wave past the 16 bits that a region names it with.
struct DsNewWaves
{
    uint32_t wave_archive = 0;
    std::vector<DsWavePlace> places;
};
DsNewWaves DsNewWavePlaces(const song::DsArchive& archive, uint32_t bank, std::size_t count);

// The slot of bank `bank` that names wave archive `wave_archive`, or nothing.
std::optional<uint16_t> DsSlotOfWaveArchive(const song::DsArchive& archive, uint32_t bank, uint32_t wave_archive);

// What waves dropped on a DS bank make: the new model, the name of the edit in the history, and the programs that it
// changes. Without `region`, a drop on the list of programs, each wave makes an instrument of one region at the next
// free program. One wave on `region`, a program and the index of the region, sets the wave of the region. Throws
// std::invalid_argument with a message for the user for several waves on a region, and for a bank without a free
// program.
struct DroppedDsWaves
{
    ntdlib::sdat::Bank bank;
    std::string name;
    std::vector<uint32_t> programs;
};
DroppedDsWaves WithDroppedDsWaves(ntdlib::sdat::Bank bank, std::optional<std::pair<uint32_t, std::size_t>> region,
                                  std::span<const DsWavePlace> waves);

} // namespace hokuto::edit
