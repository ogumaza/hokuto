// SPDX-License-Identifier: MIT

#include "edit/ds_new_items.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/commands/units.h"
#include "ntdlib/ntr/snd/commands.h"
#include "ntdlib/sdat/cut.h"
#include "ntdlib/sdat/files.h"
#include "ntdlib/sdat/formats.h"
#include "ntdlib/sdat/layout.h"
#include "xsflib/binary.h"

#include "edit/ds_files.h"
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

// The lists of the SYMB block, by the kinds of item.
constexpr std::size_t kSequenceNames = 0;
constexpr std::size_t kBankNames = 2;
constexpr std::size_t kWaveArchiveNames = 3;
constexpr std::size_t kStreamNames = 7;

// The longest name of an item, in bytes.
constexpr std::size_t kMostNameBytes = 255;

// A wave archive of more than half of the DS's main memory, and the flag that loads its waves one at a time.
constexpr std::size_t kMostWholeWaveArchive = 0x200000;
constexpr uint32_t kWavesOneByOne = 0x1000000;

// A song without notes: track 0 waits for 16 bars of 4 beats and ends. A beat has 48 ticks, and the length of the wait
// uses two bytes of 7 bits: 3,072 = 0x18 * 128.
constexpr std::array<uint8_t, 4> kEmptySong = {0x80, 0x98, 0x00, 0xFF};

sdat::ArchiveModel ModelOf(const format::File& file)
{
    if (!file.native || file.native->engine != format::Engine::kDs)
    {
        throw std::invalid_argument("The file contains no DS sound archive.");
    }

    try
    {
        return sdat::ReadArchiveModel(format::ExportArchive(file));
    }
    catch (const xsflib::FormatError&)
    {
        throw std::invalid_argument("The archive doesn't use the standard SDK layout. Hokuto can't add items to it.");
    }
}

// Checks `name` against the archive's names. An archive without names gets none.
void CheckName(const sdat::ArchiveModel& model, const std::string& name)
{
    if (!model.symbols)
    {
        return;
    }
    if (name.empty())
    {
        throw std::invalid_argument("Enter a name for the new item.");
    }
    if (name.size() > kMostNameBytes)
    {
        throw std::invalid_argument("The name is longer than 255 bytes.");
    }

    for (const std::vector<std::optional<std::string>>& names : model.symbols->names)
    {
        if (std::ranges::find(names, std::optional(name)) != names.end())
        {
            throw std::invalid_argument("The archive already has an item named " + name + ".");
        }
    }

    for (const std::optional<std::vector<std::optional<std::string>>>& names : model.symbols->sequence_names)
    {
        if (names && std::ranges::find(*names, std::optional(name)) != names->end())
        {
            throw std::invalid_argument("The archive already has an item named " + name + ".");
        }
    }
}

void AddName(sdat::ArchiveModel& model, std::size_t kind, const std::string& name, std::size_t count)
{
    if (model.symbols)
    {
        // A list can be shorter than its table.
        std::vector<std::optional<std::string>>& names = model.symbols->names[kind];
        names.resize(count);
        names.push_back(name);
    }
}

// `file` with the archive that `model` lays out. Its sequences keep their numbers, and `added` new ones follow. Each of
// them stays as the file contains it, its labels included. The game's sizes and the cartridge stay.
format::File Rebuilt(const format::File& file, const sdat::ArchiveModel& model, std::size_t added)
{
    const std::vector<uint8_t> archive = sdat::BuildArchive(model);
    format::File imported = format::ImportDsArchive(archive);
    if (imported.sequences.size() != file.sequences.size() + added)
    {
        throw std::invalid_argument("Can't add the item. Rebuilding the archive would change its sequences.");
    }

    for (std::size_t i = 0; i < file.sequences.size(); i++)
    {
        if (ntdlib::ntr::snd::EncodeSequence(*imported.sequences[i]).bytes !=
            ntdlib::ntr::snd::EncodeSequence(*file.sequences[i]).bytes)
        {
            throw std::invalid_argument("Can't add the item. Rebuilding the archive would change its sequences.");
        }

        imported.sequences[i] = file.sequences[i];
    }

    format::File result = file;
    result.sequences = std::move(imported.sequences);
    result.native = std::move(imported.native);
    const auto keep_sizes_and_image = [&file](format::NativeArchive& native)
    {
        native.game_sizes = file.native->game_sizes;
        native.cartridge = file.native->cartridge;
    };
    result.native.Edit(keep_sizes_and_image);

    return result;
}

// The regions of `instrument` that can play a wave of a wave archive.
std::vector<sdat::Region> WaveRegionsOf(const sdat::Instrument& instrument)
{
    std::vector<sdat::Region> regions = instrument.regions;
    regions.push_back(instrument.region);
    std::erase_if(regions, [](const sdat::Region& region) { return region.type != sdat::InstrumentType::kPcm; });

    return regions;
}

// Throws std::invalid_argument with a message for the user where a slot of `wave_archives` names a wave archive that
// `model` doesn't contain. kNoWaveArchive marks an empty slot.
void CheckWaveArchiveSlots(const sdat::ArchiveModel& model, const std::array<uint16_t, 4>& wave_archives)
{
    for (const uint16_t wave_archive : wave_archives)
    {
        if (wave_archive != kNoWaveArchive)
        {
            CheckDsRecord(model.wave_archives, wave_archive, "wave archive");
        }
    }
}

} // namespace

DsSoundValues DsSoundValuesOf(const song::DsArchive& archive, uint32_t sound)
{
    CheckDsRecord(archive.sounds, sound, "sound");

    const ntdlib::ntr::SequenceRef& ref = *archive.sounds[sound];
    if (!ref.archived)
    {
        const sdat::SequenceInfo& info = *archive.archive.sequences.at(ref.index);
        return {info.bank, info.volume, info.channel_priority, info.player_priority, info.player};
    }

    const sdat::SequenceArchiveInfo& info = *archive.archive.sequence_archives.at(ref.index);
    const sdat::ArchivedSequence record =
        sdat::SequenceArchive::Parse(archive.archive.File(info.file_id)).sequences.at(*ref.archived);

    return {record.bank, record.volume, record.channel_priority, record.player_priority, record.player};
}

std::optional<std::string> DsAdditionsRefused(const format::File& file)
{
    try
    {
        ModelOf(file);
    }
    catch (const std::invalid_argument& error)
    {
        return error.what();
    }

    return std::nullopt;
}

std::vector<std::string> DsTakenNames(const song::DsArchive& archive)
{
    std::vector<std::string> taken;
    if (!archive.archive.has_symbols)
    {
        return taken;
    }

    for (const std::vector<std::optional<std::string>>& names : archive.archive.symbols.names)
    {
        for (const std::optional<std::string>& name : names)
        {
            if (name)
            {
                taken.push_back(*name);
            }
        }
    }

    for (const std::optional<std::vector<std::optional<std::string>>>& names : archive.archive.symbols.sequence_names)
    {
        for (const std::optional<std::string>& name : names.value_or(std::vector<std::optional<std::string>>()))
        {
            if (name)
            {
                taken.push_back(*name);
            }
        }
    }

    std::ranges::sort(taken);

    return taken;
}

format::File AddDsSequenceSound(const format::File& file, const std::string& name, std::span<const uint8_t> bytes,
                                const DsSoundValues& values)
{
    ReadDsSequenceFile(bytes);

    sdat::ArchiveModel model = ModelOf(file);
    CheckName(model, name);
    if (model.sequences.size() >= song::kFirstDsArchivedSound)
    {
        throw std::invalid_argument("The sequence table of the archive is full.");
    }

    CheckDsRecord(model.banks, values.bank, "bank");
    CheckDsRecord(model.players, values.player, "player");

    AddName(model, kSequenceNames, name, model.sequences.size());
    model.sequences.push_back(sdat::SequenceInfo{static_cast<uint32_t>(model.files.size()), values.bank, values.volume,
                                                 values.channel_priority, values.player_priority, values.player, 0,
                                                 ""});
    model.files.emplace_back(bytes.begin(), bytes.end());

    return Rebuilt(file, model, 1);
}

format::File AddDsBank(const format::File& file, const std::string& name, std::span<const uint8_t> bytes,
                       const std::array<uint16_t, 4>& wave_archives)
{
    ReadDsBank(bytes);
    sdat::ArchiveModel model = ModelOf(file);
    CheckName(model, name);
    CheckWaveArchiveSlots(model, wave_archives);

    AddName(model, kBankNames, name, model.banks.size());
    model.banks.push_back(sdat::BankInfo{static_cast<uint32_t>(model.files.size()), wave_archives, ""});
    model.files.emplace_back(bytes.begin(), bytes.end());

    return Rebuilt(file, model, 0);
}

std::size_t DsWaveArchiveCount(const format::File& file)
{
    return ModelOf(file).wave_archives.size();
}

format::File AddDsWaveArchive(const format::File& file, const std::string& name, std::span<const uint8_t> bytes)
{
    ReadDsWaveArchive(bytes);

    sdat::ArchiveModel model = ModelOf(file);
    CheckName(model, name);
    AddName(model, kWaveArchiveNames, name, model.wave_archives.size());
    const uint32_t flags = bytes.size() > kMostWholeWaveArchive ? kWavesOneByOne : 0;
    model.wave_archives.push_back(sdat::WaveArchiveInfo{static_cast<uint32_t>(model.files.size()) | flags, ""});
    model.files.emplace_back(bytes.begin(), bytes.end());

    return Rebuilt(file, model, 0);
}

DsStreamSettings DsStreamSettingsOf(const song::DsArchive& archive, uint32_t stream)
{
    const auto& streams = archive.archive.streams;
    CheckDsRecord(streams, stream, "stream");

    const sdat::StreamInfo& info = *streams[stream];
    return {info.volume, info.player_priority, info.player, info.flags};
}

format::File AddDsStream(const format::File& file, const std::string& name, std::span<const uint8_t> bytes,
                         const DsStreamSettings& settings)
{
    ReadDsStream(bytes);

    sdat::ArchiveModel model = ModelOf(file);
    CheckName(model, name);
    if (settings.player >= model.stream_players.size() || !model.stream_players[settings.player])
    {
        throw std::invalid_argument("The archive has no stream player " + std::to_string(settings.player) +
                                    ". A stream requires one.");
    }

    AddName(model, kStreamNames, name, model.streams.size());
    sdat::StreamInfo info;
    info.file_id = static_cast<uint32_t>(model.files.size());
    info.volume = settings.volume;
    info.player_priority = settings.player_priority;
    info.player = settings.player;
    info.flags = settings.flags;
    model.streams.push_back(std::move(info));
    model.files.emplace_back(bytes.begin(), bytes.end());

    return Rebuilt(file, model, 0);
}

format::File WithDsBankSlots(const format::File& file, uint32_t bank, const std::array<uint16_t, 4>& wave_archives)
{
    sdat::ArchiveModel model = ModelOf(file);
    CheckDsRecord(model.banks, bank, "bank");
    CheckWaveArchiveSlots(model, wave_archives);
    model.banks[bank]->wave_archives = wave_archives;

    return Rebuilt(file, model, 0);
}

format::File WithDsBankFile(const format::File& file, uint32_t bank, const std::string& name,
                            std::span<const uint8_t> bytes, const std::array<uint16_t, 4>& wave_archives)
{
    ReadDsBank(bytes);
    sdat::ArchiveModel model = ModelOf(file);
    CheckDsRecord(model.banks, bank, "bank");
    CheckWaveArchiveSlots(model, wave_archives);
    if (model.symbols)
    {
        std::vector<std::optional<std::string>>& names = model.symbols->names[kBankNames];
        names.resize(std::max(names.size(), model.banks.size()));
        if (names[bank] != name)
        {
            CheckName(model, name);
            names[bank] = name;
        }
    }

    model.banks[bank]->wave_archives = wave_archives;
    model.files.at(model.banks[bank]->file_id).assign(bytes.begin(), bytes.end());

    return Rebuilt(file, model, 0);
}

std::vector<uint8_t> EmptyDsBankFile()
{
    return sdat::BankFile(sdat::Bank{});
}

std::vector<uint8_t> EmptyDsSequenceFile()
{
    return sdat::SequenceFile(kEmptySong);
}

std::vector<DsBankSlot> DsBankSlots(std::span<const uint8_t> bytes)
{
    std::map<uint16_t, uint32_t> waves;
    for (const sdat::Instrument& instrument : ReadDsBank(bytes).instruments)
    {
        for (const sdat::Region& region : WaveRegionsOf(instrument))
        {
            uint32_t& highest = waves[region.wave_archive];
            highest = std::max<uint32_t>(highest, uint32_t{region.wave} + 1);
        }
    }

    std::vector<DsBankSlot> slots;
    for (const auto& [slot, count] : waves)
    {
        slots.push_back({slot, count});
    }

    return slots;
}

std::vector<std::string> DsSequenceWarnings(const song::DsArchive& archive, uint32_t bank,
                                            std::span<const uint8_t> bytes)
{
    const std::optional<ntdlib::commands::Sequence> sequence = sdat::ReadSequenceFile(bytes);
    if (!sequence)
    {
        return {};
    }

    const std::vector<uint32_t> held = song::DsBankPrograms(archive, bank);
    std::set<int64_t> lacked;
    for (const ntdlib::commands::Command& command : sequence->items)
    {
        if (command.kind == ntdlib::commands::Kind::kProgram &&
            command.value.source == ntdlib::commands::Source::kValue &&
            std::ranges::find(held, command.value.value / ntdlib::commands::kOne) == held.end())
        {
            lacked.insert(command.value.value / ntdlib::commands::kOne);
        }
    }

    std::vector<std::string> warnings;
    if (!lacked.empty())
    {
        std::string programs;
        for (const int64_t program : lacked)
        {
            programs += (programs.empty() ? "" : ", ") + std::to_string(program);
        }

        warnings.push_back("Programs missing from " + song::DsBankName(archive, bank) + ": " + programs +
                           ". Their notes will be silent.");
    }

    return warnings;
}

std::vector<std::string> DsNewBankWarnings(const song::DsArchive& archive, std::span<const uint8_t> bytes,
                                           const std::array<uint16_t, 4>& wave_archives)
{
    std::size_t silent = 0;
    for (const sdat::Instrument& instrument : ReadDsBank(bytes).instruments)
    {
        for (const sdat::Region& region : WaveRegionsOf(instrument))
        {
            const bool slot =
                region.wave_archive < wave_archives.size() && wave_archives[region.wave_archive] != kNoWaveArchive;
            silent += !slot || region.wave >= song::DsWaveCount(archive, wave_archives[region.wave_archive]) ? 1 : 0;
        }
    }

    std::vector<std::string> warnings;
    if (silent > 0)
    {
        warnings.push_back(silent == 1 ? std::string("1 region in the new bank uses a wave that the chosen wave "
                                                     "archives don't have. It will be silent.")
                                       : std::to_string(silent) +
                                             " regions in the new bank use waves that the chosen wave archives don't "
                                             "have. They will be silent.");
    }

    return warnings;
}

} // namespace hokuto::edit
