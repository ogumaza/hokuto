// SPDX-License-Identifier: MIT

#include "edit/ds_banks.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/sdat/files.h"
#include "ntdlib/sdat/formats.h"
#include "xsflib/binary.h"

#include "edit/ds_files.h"
#include "edit/native_runs.h"
#include "format/archive.h"
#include "format/file.h"
#include "song/ds.h"

namespace hokuto::edit
{
namespace
{

namespace sdat = ntdlib::sdat;

// The bank file's table of instruments: a record of 4 bytes for each program, after the file's header of 16 bytes and
// the DATA block's 0x2C bytes. A record stores the kind of the instrument in its low byte.
constexpr uint32_t kTable = 0x3C;
constexpr std::size_t kMostDsWaves = 65536;
constexpr uint32_t kRecordBytes = 4;

// A region of a drum set or a key split: its kind, a byte of padding, then its values. A drum set's regions follow its
// first and last keys. A key split's follow its 8 last keys.
constexpr uint32_t kTypedRegionBytes = 12;
constexpr uint32_t kDrumSetHead = 2;
constexpr uint32_t kKeySplitHead = 8;
constexpr uint32_t kKindBytes = 2;
constexpr std::size_t kValueBytes = 10;

using ntdlib::sdat::kNoWaveArchive;

bool Playable(sdat::InstrumentType type)
{
    return type == sdat::InstrumentType::kPcm || type == sdat::InstrumentType::kPsg ||
           type == sdat::InstrumentType::kNoise;
}

// Refuses `value` for the field `name` of a region when it is above `highest` and differs from `before`.
void CheckValue(const char* name, int value, int before, int highest)
{
    if (value != before && value > highest)
    {
        throw std::invalid_argument(std::string(name) + " must be from 0 to " + std::to_string(highest) + ".");
    }
}

// The region's 10 bytes of values.
std::array<uint8_t, kValueBytes> ValueBytes(const sdat::Region& region)
{
    return {static_cast<uint8_t>(region.wave & 0xFF),
            static_cast<uint8_t>(region.wave >> 8),
            static_cast<uint8_t>(region.wave_archive & 0xFF),
            static_cast<uint8_t>(region.wave_archive >> 8),
            region.root_key,
            region.attack,
            region.decay,
            region.sustain,
            region.release,
            region.pan};
}

// The release of a new region. Nothing fixes it: the Bank Editor edits it.
constexpr uint8_t kNewRegionRelease = 125;

// A key split has up to 8 runs, and a key of 127 is the highest.
constexpr std::size_t kMostRuns = 8;
constexpr int kHighestKey = 127;

// The first key of run `region` of a key split.
int FirstKeyOf(const sdat::Instrument& instrument, std::size_t region)
{
    return region == 0 ? 0 : instrument.split_keys[region - 1] + 1;
}

// The instrument at `program`. `program` has to hold an instrument.
sdat::Instrument& InstrumentAt(sdat::Bank& bank, uint32_t program)
{
    if (program >= bank.instruments.size() || bank.instruments[program].type == sdat::InstrumentType::kNone)
    {
        throw std::invalid_argument("The bank has no instrument at program " + std::to_string(program) + ".");
    }

    return bank.instruments[program];
}

// Makes room for a new instrument at `program`: a free program that the table has, or one past its end that the table
// grows to. Throws std::invalid_argument with a message for the user for a program past kHighestDsProgram or one that
// has an instrument.
void MakeRoom(sdat::Bank& bank, uint32_t program)
{
    if (program > kHighestDsProgram)
    {
        throw std::invalid_argument("Program numbers must be from 0 to " + std::to_string(kHighestDsProgram) + ".");
    }
    if (program < bank.instruments.size() && bank.instruments[program].type != sdat::InstrumentType::kNone)
    {
        throw std::invalid_argument("Program " + std::to_string(program) + " already has an instrument.");
    }

    if (program >= bank.instruments.size())
    {
        bank.instruments.resize(std::size_t{program} + 1);
    }
}

bool OneRegion(const sdat::Instrument& instrument)
{
    return instrument.type != sdat::InstrumentType::kNone && instrument.type != sdat::InstrumentType::kDrumSet &&
           instrument.type != sdat::InstrumentType::kKeySplit;
}

} // namespace

DsRegionBytes DsRegionBytesOf(const sdat::Bank& bank, uint32_t program, std::size_t region)
{
    if (program >= bank.instruments.size())
    {
        throw std::invalid_argument("The bank has no program " + std::to_string(program) + ".");
    }

    const sdat::Instrument& instrument = bank.instruments[program];
    const auto at = [&instrument, region](uint32_t head)
    {
        const uint32_t start = instrument.offset + head + static_cast<uint32_t>(region) * kTypedRegionBytes;
        return DsRegionBytes{start + kKindBytes, start};
    };
    switch (instrument.type)
    {
    case sdat::InstrumentType::kNone:
        break;

    case sdat::InstrumentType::kDrumSet:
        if (region < instrument.regions.size())
        {
            return at(kDrumSetHead);
        }
        break;

    case sdat::InstrumentType::kKeySplit:
        if (region < instrument.regions.size())
        {
            return at(kKeySplitHead);
        }
        break;

    default:
        if (region == 0)
        {
            return {instrument.offset, kTable + program * kRecordBytes};
        }
        break;
    }

    throw std::invalid_argument("Program " + std::to_string(program) + " has no region " + std::to_string(region) +
                                ".");
}

format::File WithDsRegion(const format::File& file, const song::DsArchive& archive, const DsRegionPlace& place,
                          const sdat::Region& region)
{
    if (!file.native || file.native->engine != format::Engine::kDs)
    {
        throw std::invalid_argument("The file contains no DS sound archive.");
    }

    return WithDsRegion(file, archive, place, region, format::NativePieceStarts(file));
}

format::File WithDsRegion(const format::File& file, const song::DsArchive& archive, const DsRegionPlace& place,
                          const sdat::Region& region, std::span<const uint64_t> starts)
{
    if (!file.native || file.native->engine != format::Engine::kDs)
    {
        throw std::invalid_argument("The file contains no DS sound archive.");
    }

    const sdat::Archive& sdat_archive = archive.archive;
    CheckDsRecord(sdat_archive.banks, place.bank, "bank");
    const sdat::BankInfo& info = *sdat_archive.banks[place.bank];
    if (info.file_id >= sdat_archive.files.size())
    {
        throw std::invalid_argument("The bank file is missing from the archive.");
    }

    const sdat::Bank bank = sdat::Bank::Parse(sdat_archive.File(info.file_id));
    const DsRegionBytes bytes = DsRegionBytesOf(bank, place.program, place.region);
    const sdat::Instrument& instrument = bank.instruments[place.program];
    const bool typed =
        instrument.type == sdat::InstrumentType::kDrumSet || instrument.type == sdat::InstrumentType::kKeySplit;
    const sdat::Region& before = typed ? instrument.regions[place.region] : instrument.region;

    // A kind changes only among those that play: a wave, a square wave and noise. An instrument of one region at
    // another program can play the same bytes, with its kind in its record of the table.
    if (region.type != before.type)
    {
        if (!Playable(region.type) || !Playable(before.type))
        {
            throw std::invalid_argument("A region can switch only between the wave, square wave and noise types.");
        }

        for (uint32_t other = 0; other < bank.instruments.size(); ++other)
        {
            const sdat::Instrument& sharing = bank.instruments[other];
            if (other != place.program && Playable(sharing.type) && sharing.offset == bytes.values)
            {
                throw std::invalid_argument("Program " + std::to_string(other) +
                                            " shares this region but stores its type in another table entry. "
                                            "Can't change the region type here.");
            }
        }
    }

    CheckValue("Root key", region.root_key, before.root_key, kHighestDsByte);
    CheckValue("Attack", region.attack, before.attack, kHighestDsByte);
    CheckValue("Decay", region.decay, before.decay, kHighestDsByte);
    CheckValue("Sustain", region.sustain, before.sustain, kHighestDsByte);
    CheckValue("Release", region.release, before.release, kHighestDsByte);
    CheckValue("Pan", region.pan, before.pan, kHighestDsByte);
    const bool new_kind = region.type != before.type;
    if (region.type == sdat::InstrumentType::kPsg && (new_kind || region.wave != before.wave) &&
        region.wave > kHighestDsDuty)
    {
        throw std::invalid_argument("Square wave duty must be from 0 to " + std::to_string(kHighestDsDuty) + ".");
    }

    if (region.type == sdat::InstrumentType::kPcm &&
        (new_kind || region.wave != before.wave || region.wave_archive != before.wave_archive))
    {
        if (region.wave_archive > kHighestDsSlot || info.wave_archives[region.wave_archive] == kNoWaveArchive)
        {
            throw std::invalid_argument("Wave archive slot " + std::to_string(region.wave_archive) +
                                        " of the bank is empty.");
        }

        const uint32_t wave_archive = info.wave_archives[region.wave_archive];
        const std::size_t waves = song::DsWaveCount(archive, wave_archive);
        if (region.wave >= waves)
        {
            throw std::invalid_argument(song::DsWaveArchiveName(archive, wave_archive) + " has no wave " +
                                        std::to_string(region.wave) + ". It has " + std::to_string(waves) +
                                        (waves == 1 ? " wave." : " waves."));
        }
    }

    // The values, then the kind where it changed. The bank's file starts at its offset in the archive.
    const uint64_t start = sdat_archive.files[info.file_id].offset;
    format::File result = WithArchiveBytes(file, starts, start + bytes.values, ValueBytes(region), "The bank");
    if (new_kind)
    {
        const std::array<uint8_t, 1> kind = {static_cast<uint8_t>(region.type)};
        result = WithArchiveBytes(result, starts, start + bytes.kind, kind, "The bank");
    }

    return result;
}

sdat::Bank DsBankModelOf(const song::DsArchive& archive, uint32_t bank)
{
    const sdat::Archive& tables = archive.archive;
    CheckDsRecord(tables.banks, bank, "bank");
    if (tables.banks[bank]->file_id >= tables.files.size())
    {
        throw std::invalid_argument("The archive has no bank " + std::to_string(bank) + ".");
    }

    try
    {
        return sdat::Bank::Parse(tables.File(tables.banks[bank]->file_id));
    }
    catch (const xsflib::FormatError& error)
    {
        throw std::invalid_argument("Can't read " + song::DsBankName(archive, bank) + ": " + error.what() + ".");
    }
}

format::File WithDsBankModel(const format::File& file, const song::DsArchive& archive, uint32_t bank,
                             const sdat::Bank& model)
{
    const sdat::Archive& tables = archive.archive;
    CheckDsRecord(tables.banks, bank, "bank");

    return WithDsFile(file, tables.banks[bank]->file_id, sdat::BankFile(model));
}

sdat::Region NewDsRegion(std::optional<DsWavePlace> wave)
{
    sdat::Region region;
    region.type = wave ? sdat::InstrumentType::kPcm : sdat::InstrumentType::kPsg;
    region.wave = wave ? wave->wave : 3;
    region.wave_archive = wave ? wave->slot : 0;
    region.release = kNewRegionRelease;

    return region;
}

std::optional<uint32_t> FreeDsProgram(const sdat::Bank& bank, uint32_t start)
{
    for (uint32_t program = start; program <= kHighestDsProgram; ++program)
    {
        if (program >= bank.instruments.size() || bank.instruments[program].type == sdat::InstrumentType::kNone)
        {
            return program;
        }
    }

    return std::nullopt;
}

sdat::Bank WithNewDsInstrument(sdat::Bank bank, uint32_t program, const sdat::Region& region)
{
    MakeRoom(bank, program);
    sdat::Instrument& instrument = bank.instruments[program];
    instrument = {};
    instrument.type = region.type;
    instrument.region = region;

    return bank;
}

sdat::Bank WithDsInstrumentCopy(sdat::Bank bank, uint32_t from, uint32_t to)
{
    sdat::Instrument copy = InstrumentAt(bank, from);
    MakeRoom(bank, to);
    copy.offset = 0;
    bank.instruments[to] = std::move(copy);

    return bank;
}

sdat::Bank WithDsInstrumentMoved(sdat::Bank bank, uint32_t from, uint32_t to)
{
    sdat::Instrument moved = InstrumentAt(bank, from);
    MakeRoom(bank, to);
    bank.instruments[to] = std::move(moved);
    bank.instruments[from] = {};

    return bank;
}

sdat::Bank WithoutDsInstrument(sdat::Bank bank, uint32_t program)
{
    InstrumentAt(bank, program) = {};

    return bank;
}

sdat::Bank WithDsKeySplit(sdat::Bank bank, uint32_t program)
{
    sdat::Instrument& instrument = InstrumentAt(bank, program);
    if (!OneRegion(instrument))
    {
        throw std::invalid_argument("Program " + std::to_string(program) + " has more than one region.");
    }

    instrument.regions = {instrument.region};
    instrument.split_keys = {};
    instrument.split_keys[0] = kHighestKey;
    instrument.region = {};
    instrument.type = sdat::InstrumentType::kKeySplit;
    instrument.offset = 0;

    return bank;
}

sdat::Bank WithDsDrumSet(sdat::Bank bank, uint32_t program, int key)
{
    sdat::Instrument& instrument = InstrumentAt(bank, program);
    if (!OneRegion(instrument))
    {
        throw std::invalid_argument("Program " + std::to_string(program) + " has more than one region.");
    }
    if (key < 0 || key > kHighestKey)
    {
        throw std::invalid_argument("Keys must be from 0 to 127.");
    }

    instrument.regions = {instrument.region};
    instrument.min_key = static_cast<uint8_t>(key);
    instrument.max_key = static_cast<uint8_t>(key);
    instrument.region = {};
    instrument.type = sdat::InstrumentType::kDrumSet;
    instrument.offset = 0;

    return bank;
}

sdat::Bank WithDsOneRegion(sdat::Bank bank, uint32_t program, std::size_t region)
{
    sdat::Instrument& instrument = InstrumentAt(bank, program);
    if (OneRegion(instrument) || region >= instrument.regions.size() ||
        instrument.regions[region].type == sdat::InstrumentType::kNone)
    {
        throw std::invalid_argument("Program " + std::to_string(program) + " has no region " + std::to_string(region) +
                                    " to keep.");
    }

    const sdat::Region kept = instrument.regions[region];
    instrument = {};
    instrument.type = kept.type;
    instrument.region = kept;

    return bank;
}

sdat::Bank WithDsRegionWave(sdat::Bank bank, uint32_t program, std::size_t region, DsWavePlace wave)
{
    sdat::Instrument& instrument = InstrumentAt(bank, program);
    const bool one_region = OneRegion(instrument);
    if (one_region ? region != 0 : region >= instrument.regions.size())
    {
        throw std::invalid_argument("Program " + std::to_string(program) + " has no region " + std::to_string(region) +
                                    ".");
    }

    sdat::Region& values = one_region ? instrument.region : instrument.regions[region];
    values.type = sdat::InstrumentType::kPcm;
    values.wave = wave.wave;
    values.wave_archive = wave.slot;
    if (one_region)
    {
        instrument.type = sdat::InstrumentType::kPcm;
    }

    return bank;
}

DsNewWaves DsNewWavePlaces(const song::DsArchive& archive, uint32_t bank, std::size_t count)
{
    const sdat::Archive& tables = archive.archive;
    CheckDsRecord(tables.banks, bank, "bank");

    const std::array<uint16_t, 4>& slots = tables.banks[bank]->wave_archives;
    const auto named = [&tables](uint16_t wave_archive)
    {
        return wave_archive < tables.wave_archives.size() && tables.wave_archives[wave_archive];
    };
    const auto slot = std::ranges::find_if(slots, named);
    if (slot == slots.end())
    {
        throw std::invalid_argument("The bank has no wave archive. Choose one under Wave Archives… first.");
    }

    DsNewWaves added;
    added.wave_archive = *slot;
    const std::size_t first = song::DsWaveCount(archive, added.wave_archive);
    if (first + count > kMostDsWaves)
    {
        throw std::invalid_argument("Too many waves. " + song::DsWaveArchiveName(archive, added.wave_archive) +
                                    " would have more than 65,536, the most that a region can refer to.");
    }

    for (std::size_t k = 0; k < count; k++)
    {
        added.places.push_back({static_cast<uint16_t>(slot - slots.begin()), static_cast<uint16_t>(first + k)});
    }

    return added;
}

std::optional<uint16_t> DsSlotOfWaveArchive(const song::DsArchive& archive, uint32_t bank, uint32_t wave_archive)
{
    const sdat::Archive& tables = archive.archive;
    if (bank >= tables.banks.size() || !tables.banks[bank])
    {
        return std::nullopt;
    }

    const std::array<uint16_t, 4>& slots = tables.banks[bank]->wave_archives;
    const auto slot = std::ranges::find(slots, wave_archive);

    return slot == slots.end() ? std::nullopt : std::optional(static_cast<uint16_t>(slot - slots.begin()));
}

DroppedDsWaves WithDroppedDsWaves(sdat::Bank bank, std::optional<std::pair<uint32_t, std::size_t>> region,
                                  std::span<const DsWavePlace> waves)
{
    if (region)
    {
        if (waves.size() != 1)
        {
            throw std::invalid_argument(
                "A region plays one wave. Drag one wave onto the region, or several onto the program list.");
        }

        return {WithDsRegionWave(std::move(bank), region->first, region->second, waves.front()),
                "Change Wave",
                {region->first}};
    }

    std::vector<uint32_t> programs;
    uint32_t next = 0;
    for (const DsWavePlace& wave : waves)
    {
        const std::optional<uint32_t> program = FreeDsProgram(bank, next);
        if (!program)
        {
            throw std::invalid_argument("The bank has no free program for another instrument.");
        }

        bank = WithNewDsInstrument(std::move(bank), *program, NewDsRegion(wave));
        programs.push_back(*program);
        next = *program + 1;
    }

    return {std::move(bank), waves.size() == 1 ? "Add Instrument" : "Add Instruments", std::move(programs)};
}

sdat::Bank WithDsSplit(sdat::Bank bank, uint32_t program, std::size_t region, int key)
{
    sdat::Instrument& instrument = InstrumentAt(bank, program);
    if (instrument.type != sdat::InstrumentType::kKeySplit || region >= instrument.regions.size())
    {
        throw std::invalid_argument("Program " + std::to_string(program) + " has no region " + std::to_string(region) +
                                    " to split.");
    }
    if (instrument.regions.size() >= kMostRuns)
    {
        throw std::invalid_argument("A key split can have up to 8 regions.");
    }

    const int first = FirstKeyOf(instrument, region);
    const int last = instrument.split_keys[region];
    if (key <= first || key > last || key < 2)
    {
        throw std::invalid_argument("The region covers keys " + std::to_string(first) + "–" + std::to_string(last) +
                                    ". Choose a split key above " + std::to_string(first) + ", up to " +
                                    std::to_string(last) + " and no lower than 2.");
    }

    const auto at = static_cast<std::ptrdiff_t>(region);
    instrument.regions.insert(instrument.regions.begin() + at, instrument.regions[region]);
    std::copy_backward(instrument.split_keys.begin() + at, instrument.split_keys.end() - 1,
                       instrument.split_keys.end());
    instrument.split_keys[region] = static_cast<uint8_t>(key - 1);
    instrument.offset = 0;

    return bank;
}

sdat::Bank WithoutDsRun(sdat::Bank bank, uint32_t program, std::size_t region)
{
    sdat::Instrument& instrument = InstrumentAt(bank, program);
    if (instrument.type != sdat::InstrumentType::kKeySplit || region >= instrument.regions.size())
    {
        throw std::invalid_argument("Program " + std::to_string(program) + " has no region " + std::to_string(region) +
                                    ".");
    }
    if (instrument.regions.size() == 1)
    {
        throw std::invalid_argument("A key split must have at least one region.");
    }

    const bool last = region + 1 == instrument.regions.size();
    const uint8_t last_key = instrument.split_keys[region];
    const auto at = static_cast<std::ptrdiff_t>(region);
    instrument.regions.erase(instrument.regions.begin() + at);
    std::copy(instrument.split_keys.begin() + at + 1, instrument.split_keys.end(), instrument.split_keys.begin() + at);
    instrument.split_keys.back() = 0;
    if (last)
    {
        instrument.split_keys[region - 1] = last_key;
    }

    instrument.offset = 0;

    return bank;
}

sdat::Bank WithDsDrumKeys(sdat::Bank bank, uint32_t program, int first, int last)
{
    sdat::Instrument& instrument = InstrumentAt(bank, program);
    if (instrument.type != sdat::InstrumentType::kDrumSet || instrument.regions.empty())
    {
        throw std::invalid_argument("Program " + std::to_string(program) + " isn't a drum set.");
    }
    if (first < 0 || last > kHighestKey || first > last)
    {
        throw std::invalid_argument("Drum set keys must be from 0 to 127, with the low key first.");
    }

    // Each key gets its region, or the region of the nearest key.
    const int old_first = instrument.min_key;
    const int old_last = old_first + static_cast<int>(instrument.regions.size()) - 1;
    std::vector<sdat::Region> regions;
    for (int key = first; key <= last; ++key)
    {
        regions.push_back(
            instrument.regions[static_cast<std::size_t>(std::clamp(key, old_first, old_last) - old_first)]);
    }

    instrument.regions = std::move(regions);
    instrument.min_key = static_cast<uint8_t>(first);
    instrument.max_key = static_cast<uint8_t>(last);
    instrument.offset = 0;

    return bank;
}

} // namespace hokuto::edit
