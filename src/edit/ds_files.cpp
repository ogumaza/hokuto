// SPDX-License-Identifier: MIT

#include "edit/ds_files.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/sar/layout.h"
#include "ntdlib/sdat/cut.h"
#include "ntdlib/sdat/formats.h"
#include "xsflib/binary.h"

#include "format/archive.h"
#include "format/file.h"
#include "format/shared.h"
#include "song/ds.h"

namespace hokuto::edit
{
namespace
{

namespace sdat = ntdlib::sdat;

using sdat::kNoWaveArchive;

void CheckDs(const format::File& file)
{
    if (!file.native || file.native->engine != format::Engine::kDs)
    {
        throw std::invalid_argument("The file contains no DS sound archive.");
    }
}

const sdat::BankInfo& BankInfoOf(const song::DsArchive& archive, uint32_t bank)
{
    const sdat::Archive& sdat_archive = archive.archive;
    CheckDsRecord(sdat_archive.banks, bank, "bank");

    return *sdat_archive.banks[bank];
}

const sdat::WaveArchiveInfo& WaveArchiveInfoOf(const song::DsArchive& archive, uint32_t wave_archive)
{
    const sdat::Archive& sdat_archive = archive.archive;
    CheckDsRecord(sdat_archive.wave_archives, wave_archive, "wave archive");

    return *sdat_archive.wave_archives[wave_archive];
}

// The regions of `instrument` that can play a wave of a wave archive.
std::vector<sdat::Region> RegionsOf(const sdat::Instrument& instrument)
{
    std::vector<sdat::Region> regions = instrument.regions;
    regions.push_back(instrument.region);
    std::erase_if(regions, [](const sdat::Region& region) { return region.type != sdat::InstrumentType::kPcm; });

    return regions;
}

// "12 regions" or "1 region", with `noun`.
std::string Counted(std::size_t count, const char* noun)
{
    return std::to_string(count) + " " + noun + (count == 1 ? "" : "s");
}

} // namespace

sdat::Bank ReadDsBank(std::span<const uint8_t> bytes)
{
    try
    {
        return sdat::Bank::Parse(bytes);
    }
    catch (const xsflib::FormatError& error)
    {
        throw std::invalid_argument(std::string("Can't read the file as an SBNK bank: ") + error.what() + ".");
    }
}

sdat::WaveArchive ReadDsWaveArchive(std::span<const uint8_t> bytes)
{
    try
    {
        return sdat::WaveArchive::Parse(bytes);
    }
    catch (const xsflib::FormatError& error)
    {
        throw std::invalid_argument(std::string("Can't read the file as an SWAR wave archive: ") + error.what() + ".");
    }
}

ntdlib::commands::Sequence ReadDsSequenceFile(std::span<const uint8_t> bytes)
{
    std::optional<ntdlib::commands::Sequence> commands = sdat::ReadSequenceFile(bytes);
    if (!commands)
    {
        throw std::invalid_argument("Can't read the file as an SSEQ sequence file.");
    }

    return std::move(*commands);
}

sdat::Stream ReadDsStream(std::span<const uint8_t> bytes)
{
    try
    {
        return sdat::Stream::Parse(bytes);
    }
    catch (const xsflib::FormatError& error)
    {
        throw std::invalid_argument(std::string("Can't read the file as an STRM stream: ") + error.what() + ".");
    }
}

format::File WithDsFile(const format::File& file, uint32_t file_id, std::span<const uint8_t> bytes)
{
    CheckDs(file);
    std::vector<ntdlib::sar::Piece> pieces;
    try
    {
        pieces =
            sdat::ReplaceFile(format::CopySequences(file), *file.native->pieces, file.native->fields, file_id, bytes);
    }
    catch (const std::invalid_argument& error)
    {
        throw std::invalid_argument(std::string("Can't replace the file in the archive: ") + error.what() + ".");
    }

    format::File result = file;
    result.native.Edit([&](format::NativeArchive& native) { native.pieces = std::move(pieces); });
    if (format::DsArchiveSharesFile(file) && format::ExportArchive(result).size() != format::ExportArchive(file).size())
    {
        throw std::invalid_argument(
            "Can't change the size of the sound archive. It shares its file in the image with other data, and "
            "Hokuto can't move that data.");
    }

    return result;
}

format::File ReplaceDsBank(const format::File& file, const song::DsArchive& archive, uint32_t bank,
                           std::span<const uint8_t> bytes)
{
    const sdat::BankInfo& info = BankInfoOf(archive, bank);
    ReadDsBank(bytes);

    return WithDsFile(file, info.file_id, bytes);
}

std::vector<std::string> DsBankWarnings(const song::DsArchive& archive, uint32_t bank, std::span<const uint8_t> bytes)
{
    const sdat::BankInfo& info = BankInfoOf(archive, bank);
    const sdat::Bank incoming = ReadDsBank(bytes);
    std::vector<std::string> warnings;

    // The programs that the current bank has and the new one doesn't.
    std::string lacked;
    for (const uint32_t program : song::DsBankPrograms(archive, bank))
    {
        if (program >= incoming.instruments.size() || incoming.instruments[program].type == sdat::InstrumentType::kNone)
        {
            lacked += (lacked.empty() ? "" : ", ") + std::to_string(program);
        }
    }
    if (!lacked.empty())
    {
        warnings.push_back("Programs missing from the new bank: " + lacked + ". Their notes will be silent.");
    }

    // The regions whose slots name no wave archive, or a wave archive without the wave.
    std::size_t silent = 0;
    for (const sdat::Instrument& instrument : incoming.instruments)
    {
        for (const sdat::Region& region : RegionsOf(instrument))
        {
            const bool slot = region.wave_archive < info.wave_archives.size() &&
                              info.wave_archives[region.wave_archive] != kNoWaveArchive;
            silent +=
                !slot || region.wave >= song::DsWaveCount(archive, info.wave_archives[region.wave_archive]) ? 1 : 0;
        }
    }
    if (silent > 0)
    {
        warnings.push_back(silent == 1
                               ? std::string("1 region in the new bank uses a wave that is missing from its wave "
                                             "archives. It will be silent.")
                               : std::to_string(silent) +
                                     " regions in the new bank use waves that are missing from its wave "
                                     "archives. They will be silent.");
    }

    return warnings;
}

std::size_t DsBankInstruments(std::span<const uint8_t> bytes)
{
    std::size_t count = 0;
    for (const sdat::Instrument& instrument : ReadDsBank(bytes).instruments)
    {
        count += instrument.type != sdat::InstrumentType::kNone ? 1 : 0;
    }

    return count;
}

format::File ReplaceDsWaveArchive(const format::File& file, const song::DsArchive& archive, uint32_t wave_archive,
                                  std::span<const uint8_t> bytes)
{
    const sdat::WaveArchiveInfo& info = WaveArchiveInfoOf(archive, wave_archive);
    ReadDsWaveArchive(bytes);

    return WithDsFile(file, info.file_id & sdat::kFileIdBits, bytes);
}

std::vector<std::string> DsWaveArchiveWarnings(const song::DsArchive& archive, uint32_t wave_archive,
                                               std::span<const uint8_t> bytes)
{
    WaveArchiveInfoOf(archive, wave_archive);
    const std::size_t waves = ReadDsWaveArchive(bytes).waves.size();

    // The regions of the banks whose slots name the wave archive, and whose waves fall past the end of the new waves.
    std::size_t silent = 0;
    const sdat::Archive& sdat_archive = archive.archive;
    for (const std::optional<sdat::BankInfo>& info : sdat_archive.banks)
    {
        if (!info || info->file_id >= sdat_archive.files.size())
        {
            continue;
        }

        sdat::Bank bank;
        try
        {
            bank = sdat::Bank::Parse(sdat_archive.File(info->file_id));
        }
        catch (const xsflib::FormatError&)
        {
            continue;
        }

        for (const sdat::Instrument& instrument : bank.instruments)
        {
            for (const sdat::Region& region : RegionsOf(instrument))
            {
                silent += region.wave_archive < info->wave_archives.size() &&
                                  info->wave_archives[region.wave_archive] == wave_archive && region.wave >= waves
                              ? 1
                              : 0;
            }
        }
    }

    std::vector<std::string> warnings;
    if (silent > 0)
    {
        warnings.push_back("The new file has " + Counted(waves, "wave") + ". " + Counted(silent, "region") +
                           (silent == 1 ? " in the banks uses a higher wave number and will be silent."
                                        : " in the banks use higher wave numbers and will be silent."));
    }

    return warnings;
}

std::size_t DsWaveArchiveWaves(std::span<const uint8_t> bytes)
{
    return ReadDsWaveArchive(bytes).waves.size();
}

format::File ReplaceDsSequence(const format::File& file, const song::DsArchive& archive, uint32_t sound,
                               std::span<const uint8_t> bytes)
{
    CheckDs(file);
    const std::vector<std::optional<uint32_t>> sequences = format::DsSoundSequences(file, archive.bytes);
    if (sound >= sequences.size() || !sequences[sound])
    {
        throw std::invalid_argument(song::DsSoundName(archive, sound) +
                                    " has no sequence that Hokuto can replace. Its commands are in a sequence "
                                    "archive or among other data of the archive.");
    }

    format::File result = file;
    result.sequences[*sequences[sound]] = ReadDsSequenceFile(bytes);

    return result;
}

std::vector<uint32_t> DsSoundsOfSequence(const format::File& file, const song::DsArchive& archive, uint32_t sound)
{
    const std::vector<std::optional<uint32_t>> sequences = format::DsSoundSequences(file, archive.bytes);
    std::vector<uint32_t> sounds;
    if (sound >= sequences.size() || !sequences[sound])
    {
        return sounds;
    }

    sounds.push_back(sound);
    for (uint32_t other = 0; other < sequences.size(); other++)
    {
        if (other != sound && sequences[other] == sequences[sound])
        {
            sounds.push_back(other);
        }
    }

    return sounds;
}

std::vector<uint32_t> DsSoundsOfBank(const song::DsArchive& archive, uint32_t bank)
{
    std::vector<uint32_t> sounds;
    for (const uint32_t sound : song::DsSounds(archive))
    {
        if (song::DsBankOf(archive, sound) == bank)
        {
            sounds.push_back(sound);
        }
    }

    return sounds;
}

std::vector<uint32_t> DsSoundsOfWaveArchive(const song::DsArchive& archive, uint32_t wave_archive)
{
    std::vector<uint32_t> sounds;
    for (const uint32_t sound : song::DsSounds(archive))
    {
        const std::optional<uint32_t> bank = song::DsBankOf(archive, sound);
        if (bank && *bank < archive.archive.banks.size() && archive.archive.banks[*bank] &&
            std::ranges::find(archive.archive.banks[*bank]->wave_archives, wave_archive) !=
                archive.archive.banks[*bank]->wave_archives.end())
        {
            sounds.push_back(sound);
        }
    }

    return sounds;
}

std::optional<song::DsGame> DsGameOfFile(const format::File& file)
{
    if (!file.native || !file.native->cartridge)
    {
        return std::nullopt;
    }

    const format::Cartridge& cartridge = *file.native->cartridge;
    return song::DsGameOf(*cartridge.image, cartridge.size, cartridge.file, cartridge.offset);
}

} // namespace hokuto::edit
