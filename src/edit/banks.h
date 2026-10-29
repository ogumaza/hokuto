// SPDX-License-Identifier: MIT

// Edits of a bank of the 3DS, the Wii U or the Switch through its model, ntdlib's sar::BankFile: its instruments by
// program, each instrument's regions, and every value of a region. The Bank Editor makes them. A bank that the user
// hasn't edited stays as the game's bytes. An edited bank keeps its model in the file (format::File's banks). Every
// copy of its file in the archive holds the bytes that the model lays out (ReplaceFile). The model lays a bank out as
// NintendoWare's tools do.
//
// A region covers a run of keys and a run of velocities. The file holds an instrument's regions by their keys: each run
// of keys holds the regions of its velocities. Regions over the same keys share a run. Regions over other keys can't
// overlap. A region plays an entry of the bank's table of waves: a wave archive's item ID and a wave's index. A game
// plays the wave only once a group has loaded its wave archive.
//
// The Bank Editor shows a bank of the Wii in the same model. ntdlib's rsar::BankFile reads the bank, and lays it out
// again as NintendoWare for Revolution's tools lay out a bank whose instruments share no node. The bank's waves lie in
// its wave data, and Hokuto takes that wave data for a wave archive, by the bank's file ID (song::WaveArchivesOf).
// Entry i of the model's table of waves names wave i of the wave data, and a region's wave index names the entry. A
// region's tune is its pitch, and a note-off type of 1 ignores the note-off. The Wii reads no interpolation. NW4R SND
// reads the hold, the note-off type and the key group only in its builds from March 2008 on. A region that names no
// wave plays nothing, and the model leaves it out. The bytes give the bank back, and the file keeps no model of it. The
// waves keep their places in the wave data, and their models (edit/waves.h). A region that plays a wave of another
// file's wave data takes a copy of the wave after the bank's waves.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "ntdlib/rsar/bank_file.h"
#include "ntdlib/rsar/formats.h"
#include "ntdlib/sar/bank_file.h"
#include "ntdlib/sar/formats.h"

#include "edit/files.h"
#include "format/file.h"

namespace hokuto::edit
{

// The highest program that the engines select.
inline constexpr uint32_t kHighestProgram = 0xFFFF;

// A region of an instrument: its keys, its velocities and the values that it plays with. Its wave is an entry of the
// bank's table of waves.
struct Region
{
    friend bool operator==(const Region&, const Region&) = default;

    uint8_t first_key = 0;
    uint8_t last_key = 127;
    uint8_t first_velocity = 0;
    uint8_t last_velocity = 127;
    ntdlib::sar::VelocityRegion values;
};

// The regions of `instrument`, by their keys and then by their velocities.
std::vector<Region> RegionsOf(const ntdlib::sar::BankFile::Instrument& instrument);

// The instrument with `regions`. Throws std::invalid_argument with a message for the user for a region that ends before
// it starts or past 127, regions whose keys overlap without being the same keys, and regions of the same keys whose
// velocities overlap.
ntdlib::sar::BankFile::Instrument InstrumentOf(std::vector<Region> regions);

// The region of `instrument` that plays `key` at `velocity`, by its place in RegionsOf, or nothing.
std::optional<std::size_t> RegionAt(const ntdlib::sar::BankFile::Instrument& instrument, int key, int velocity);

// `bank` with a new instrument at `program`: one region over every key and velocity that plays `wave`, or none without
// a wave. Throws std::invalid_argument with a message for the user for a program past kHighestProgram, and for one that
// holds an instrument.
ntdlib::sar::BankFile WithNewInstrument(ntdlib::sar::BankFile bank, uint32_t program,
                                        std::optional<ntdlib::sar::WaveId> wave);

// `bank` with a copy of the instrument at `from` at program `to`. Throws std::invalid_argument with a message for the
// user for a program without an instrument at `from`, and as WithNewInstrument does for `to`.
ntdlib::sar::BankFile WithInstrumentCopy(ntdlib::sar::BankFile bank, uint32_t from, uint32_t to);

// `bank` with the instrument at `from` moved to program `to`. Throws as WithInstrumentCopy does.
ntdlib::sar::BankFile WithInstrumentMoved(ntdlib::sar::BankFile bank, uint32_t from, uint32_t to);

// `bank` without the instrument at `program`. The table of instruments ends at the last program with one. Throws
// std::invalid_argument with a message for the user for a program without an instrument.
ntdlib::sar::BankFile WithoutInstrument(ntdlib::sar::BankFile bank, uint32_t program);

// `bank` with `regions` as the instrument at `program`. The table of waves then lists each wave that a region plays,
// once, with the new ones at its end. Throws as InstrumentOf does, and std::invalid_argument with a message for the
// user for a program without an instrument and a region whose wave's entry lies past the table.
ntdlib::sar::BankFile WithRegions(ntdlib::sar::BankFile bank, uint32_t program, std::vector<Region> regions);

// The entry of `wave` in the table of waves of `bank`: the first that names it, or a new one at the table's end.
uint32_t WaveEntry(ntdlib::sar::BankFile& bank, ntdlib::sar::WaveId wave);

// The model of bank `bank` of `archive`, the archive that `file` builds: the file's model where the user edited the
// bank, or the model that ntdlib reads from the bank's file. Throws std::invalid_argument with a message for the user
// for a bank past the archive's list and a bank that NintendoWare's tools didn't lay out.
ntdlib::sar::BankFile BankModelOf(const format::File& file, const ntdlib::sar::SoundArchive& archive, uint32_t bank);

// `file` with bank `bank` of `archive` laid out from `model`. Every copy of the bank's file takes the bytes, and the
// file keeps the model. Where they can, the groups that load the bank take the wave archives that its waves name and
// that they lack (JoinGroups). A game then finds the bank's waves. Throws std::invalid_argument with a message for the
// user for a model of another console than the archive's, a model that can't be laid out, and where ReplaceFile
// refuses.
format::File WithBankModel(const format::File& file, const ntdlib::sar::SoundArchive& archive, uint32_t bank,
                           ntdlib::sar::BankFile model);

// The groups of `archive`, the archive that `file` builds, that load bank `bank` and lack a wave archive that its waves
// name, by their places in the group table. A game that loads such a group plays the waves of that wave archive only
// when another group has loaded it.
std::vector<uint32_t> GroupsLackingWaves(const format::File& file, const ntdlib::sar::SoundArchive& archive,
                                         uint32_t bank);

// The wave archive of the first entry of the table of waves of `bank` that names one of `archive`'s wave archives, by
// its place in the archive's list. Nothing when no entry does. New waves of the bank go there (WithBankWaves).
std::optional<uint32_t> WaveArchiveOfBank(const ntdlib::sar::BankFile& bank, const ntdlib::sar::SoundArchive& archive);

// A file after new waves came in for a bank, and the waves, in their order.
struct BankWaves
{
    format::File file;
    std::vector<ntdlib::sar::WaveId> waves;
};

// `file` with `incoming`, waves from WAV files or wave files, for bank `bank` of `archive` with the model `model`. The
// waves go after the waves of the model's wave archive (WaveArchiveOfBank), each with its model (IncomingWave::model).
// Without such a wave archive, a new wave archive holds them. Its name is the bank's, with WARC_ in the place of BANK_
// or before it, and a number after it where the archive holds the name already. An archive without names gives it none.
// The bank's model doesn't change: the caller gives its regions the new waves. Throws std::invalid_argument with a
// message for the user where WithIncomingWave or AddWaveArchive refuse.
BankWaves WithBankWaves(const format::File& file, const ntdlib::sar::SoundArchive& archive, uint32_t bank,
                        const ntdlib::sar::BankFile& model, const std::vector<IncomingWave>& incoming);

// The bytes of an empty bank of `flavor`, a console's bank without instruments and waves, for Project > Add Bank….
std::vector<uint8_t> EmptyBank(ntdlib::sar::Flavor flavor);

// The name of the wave that entry `entry` of the table of `bank` names in `archive`, such as "wave 3 of WARC_SE", or
// the entry's IDs where the archive lacks the wave archive.
std::string WaveEntryName(const ntdlib::sar::SoundArchive& archive, const ntdlib::sar::BankFile& bank, uint32_t entry);

// The model of bank `bank` of the Wii's `archive`. The comment at the top describes it. Throws std::invalid_argument
// with a message for the user for a bank past the archive's list, a bank whose file or wave data the archive lacks, one
// that ntdlib's model can't read, and a region that the model can't hold: a surround pan, a wave data location other
// than 0 or a note-off type above 1.
ntdlib::sar::BankFile BankModelOf(const ntdlib::rsar::SoundArchive& archive, uint32_t bank);

// `file` with bank `bank` of the Wii's `archive` laid out from `model`, with `waves` after the bank's waves. NewWaveIds
// names those waves. The comment at the top describes the layout. Every group that holds the bank's file takes the new
// file and wave data. Throws std::invalid_argument with a message for the user as BankModelOf does, for an entry of the
// model's table of waves that names no wave of the archive's wave archives or of `waves`, for a model that can't be
// laid out, and where ReplaceFile refuses.
format::File WithBankModel(const format::File& file, const ntdlib::rsar::SoundArchive& archive, uint32_t bank,
                           const ntdlib::sar::BankFile& model, std::vector<ntdlib::rsar::WaveFile> waves = {});

// The IDs in the model's terms of `count` new waves after the waves of bank `bank` of the Wii's `archive`.
// WithBankModel puts the new waves there. Throws as BankModelOf does.
std::vector<ntdlib::sar::WaveId> NewWaveIds(const ntdlib::rsar::SoundArchive& archive, uint32_t bank,
                                            std::size_t count);

// The name of the wave that entry `entry` of the table of `bank`, the model of a bank of the Wii's `archive`, names,
// such as "BANK_SE, wave 7".
std::string WaveEntryName(const ntdlib::rsar::SoundArchive& archive, const ntdlib::sar::BankFile& bank, uint32_t entry);

// The files of an empty bank of the Wii for `archive`, for Project > Add Bank…: a bank without instruments and waves,
// and its wave data. The bank takes the version of the archive's first bank whose file ntdlib reads, or else version
// 1.2 from RSAR 1.4 on and 1.1 before (NewWiiFileVersion). A bank of version 1.2 keeps its waves in an RWAR, and one of
// 1.1 in raw samples. The builds of NW4R SND that link the older archives may not read version 1.2. With `waves`, the
// bank's wave data holds them: File > Import > Wave Archive… on the Wii. Every wave there belongs to the wave data of a
// bank or a wave sound file. Throws std::invalid_argument for a wave that can't be written.
ntdlib::rsar::BankFile::Files EmptyBank(const ntdlib::rsar::SoundArchive& archive,
                                        std::vector<ntdlib::rsar::WaveFile> waves = {});

} // namespace hokuto::edit
