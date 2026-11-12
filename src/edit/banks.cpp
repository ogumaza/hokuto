// SPDX-License-Identifier: MIT

#include "edit/banks.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iterator>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "ntdlib/rsar/bank_file.h"
#include "ntdlib/rsar/convert.h"
#include "ntdlib/rsar/formats.h"
#include "ntdlib/rsar/wave_file.h"
#include "ntdlib/sar/bank_file.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sar/tables.h"
#include "ntdlib/sar/wave_file.h"

#include "edit/files.h"
#include "edit/groups.h"
#include "edit/new_items.h"
#include "edit/waves.h"
#include "format/file.h"
#include "format/kinds.h"
#include "song/items.h"

namespace hokuto::edit
{
namespace
{

namespace sar = ntdlib::sar;

constexpr int kHighestValue = 127;

// The instrument at `program`, or a message for the user.
const sar::BankFile::Instrument& InstrumentAt(const sar::BankFile& bank, uint32_t program)
{
    if (program >= bank.instruments.size() || !bank.instruments[program])
    {
        throw std::invalid_argument("Program " + std::to_string(program) + " has no instrument.");
    }

    return *bank.instruments[program];
}

// Makes room for an instrument at `program`. The program holds none yet.
void MakeRoom(sar::BankFile& bank, uint32_t program)
{
    if (program > kHighestProgram)
    {
        throw std::invalid_argument("The highest program number is " + std::to_string(kHighestProgram) + ".");
    }
    if (program < bank.instruments.size() && bank.instruments[program])
    {
        throw std::invalid_argument("Program " + std::to_string(program) + " already has an instrument.");
    }

    if (program >= bank.instruments.size())
    {
        bank.instruments.resize(program + 1);
    }
}

// A layout that lists the programs lists a new instrument's program last.
void AddToOrder(sar::BankFile& bank, uint32_t program)
{
    if (!bank.order.empty())
    {
        bank.order.push_back(program);
    }
}

// The table of instruments ends at the last program with one.
void TrimInstruments(sar::BankFile& bank)
{
    while (!bank.instruments.empty() && !bank.instruments.back())
    {
        bank.instruments.pop_back();
    }
}

// Calls `visit` with each velocity region of `bank`.
template <class Visit>
void ForEachRegion(sar::BankFile& bank, Visit visit)
{
    for (std::optional<sar::BankFile::Instrument>& instrument : bank.instruments)
    {
        if (!instrument)
        {
            continue;
        }

        for (sar::BankFile::KeyRegion& keys : instrument->key_regions)
        {
            for (sar::BankFile::Region& region : keys.regions)
            {
                visit(region);
            }
        }
    }
}

// The table of waves of `bank` with each wave that a region plays, once, in the table's order. The regions name their
// waves' new entries. A region whose entry is past the table keeps its entry.
void KeepPlayedWaves(sar::BankFile& bank)
{
    std::vector<bool> played(bank.waves.size(), false);
    const auto mark = [&played](const sar::BankFile::Region& region)
    {
        if (region.values.wave_id_index < played.size())
        {
            played[region.values.wave_id_index] = true;
        }
    };
    ForEachRegion(bank, mark);

    // Each entry's new place: the first kept entry that names the same wave.
    std::vector<sar::WaveId> waves;
    std::vector<uint32_t> places(bank.waves.size(), 0);
    for (std::size_t entry = 0; entry < bank.waves.size(); ++entry)
    {
        if (!played[entry])
        {
            continue;
        }

        const auto same = std::ranges::find(waves, bank.waves[entry]);
        places[entry] = static_cast<uint32_t>(same - waves.begin());
        if (same == waves.end())
        {
            waves.push_back(bank.waves[entry]);
        }
    }

    const auto move = [&places](sar::BankFile::Region& region)
    {
        if (region.values.wave_id_index < places.size())
        {
            region.values.wave_id_index = places[region.values.wave_id_index];
        }
    };
    ForEachRegion(bank, move);
    bank.waves = std::move(waves);
}

std::string KeysText(int first, int last)
{
    return std::to_string(first) + "–" + std::to_string(last);
}

// Whether `group` holds file `file_id`.
bool Holds(const Group& group, uint32_t file_id)
{
    return std::ranges::find(group.files, file_id) != group.files.end();
}

// The groups of `archive`, the archive that `file` builds, that load file `file_id` and lack one of the files `needed`,
// by their places in the group table. With `reachable_only`, only the groups that an edit can reach (WithBankModel and
// GroupsLackingWaves).
std::vector<uint32_t> GroupsLacking(const format::File& file, const sar::SoundArchive& archive, uint32_t file_id,
                                    std::span<const uint32_t> needed, bool reachable_only)
{
    std::vector<uint32_t> lacking;
    for (const Group& group : GroupsOf(file, archive))
    {
        const auto held = [&group](uint32_t id)
        {
            return Holds(group, id);
        };
        if ((!reachable_only || group.unavailable.empty()) && Holds(group, file_id) &&
            !std::ranges::all_of(needed, held))
        {
            lacking.push_back(group.group);
        }
    }

    return lacking;
}

// The groups of `archive`, the archive that `file` builds, that load file `file_id` and that an edit can reach, by
// their places in the group table.
std::vector<uint32_t> GroupsLoading(const format::File& file, const sar::SoundArchive& archive, uint32_t file_id)
{
    std::vector<uint32_t> loading;
    for (const Group& group : GroupsOf(file, archive))
    {
        if (group.unavailable.empty() && Holds(group, file_id))
        {
            loading.push_back(group.group);
        }
    }

    return loading;
}

// A name for a new wave archive of bank `bank` of `tables`: the bank's with WARC_ in the place of BANK_ or before it,
// and a number after it where the tables or `archive` hold the name already.
std::string NewWaveArchiveName(const format::NativeTables& tables, const sar::SoundArchive& archive, uint32_t bank)
{
    std::vector<std::string> taken = tables.banks;
    taken.insert(taken.end(), tables.wave_archives.begin(), tables.wave_archives.end());
    taken.insert(taken.end(), tables.groups.begin(), tables.groups.end());
    for (const format::NativeSound& sound : tables.sounds)
    {
        taken.push_back(sound.name);
    }

    for (const format::NativePlayer& player : tables.players)
    {
        taken.push_back(player.name);
    }

    for (const sar::SoundGroupInfo& group : archive.SoundGroups())
    {
        taken.push_back(group.name);
    }

    const std::string& name = tables.banks.at(bank);
    const std::string stem = "WARC_" + (name.starts_with("BANK_") ? name.substr(5) : name);
    std::string candidate = stem;
    for (int number = 2; std::ranges::find(taken, candidate) != taken.end(); number++)
    {
        candidate = stem + "_" + std::to_string(number);
    }

    return candidate;
}

} // namespace

std::vector<Region> RegionsOf(const sar::BankFile::Instrument& instrument)
{
    std::vector<Region> regions;
    for (const sar::BankFile::KeyRegion& keys : instrument.key_regions)
    {
        for (const sar::BankFile::Region& region : keys.regions)
        {
            regions.push_back(
                {keys.first_key, keys.last_key, region.first_velocity, region.last_velocity, region.values});
        }
    }

    return regions;
}

sar::BankFile::Instrument InstrumentOf(std::vector<Region> regions)
{
    for (const Region& region : regions)
    {
        if (region.first_key > region.last_key || region.last_key > kHighestValue)
        {
            throw std::invalid_argument("Invalid key range: " + KeysText(region.first_key, region.last_key) +
                                        ". Key ranges must be within 0–127, with the low key first.");
        }
        if (region.first_velocity > region.last_velocity || region.last_velocity > kHighestValue)
        {
            throw std::invalid_argument("Invalid velocity range: " + std::to_string(region.first_velocity) + "–" +
                                        std::to_string(region.last_velocity) +
                                        ". Velocity ranges must be within 0–127, with the low velocity first.");
        }
    }

    // The regions by their keys, then by their velocities. Each run of keys holds the regions over it.
    const auto earlier = [](const Region& a, const Region& b)
    {
        return std::tie(a.first_key, a.last_key, a.first_velocity) <
               std::tie(b.first_key, b.last_key, b.first_velocity);
    };
    std::ranges::stable_sort(regions, earlier);
    sar::BankFile::Instrument instrument;
    for (const Region& region : regions)
    {
        sar::BankFile::KeyRegion* keys = instrument.key_regions.empty() ? nullptr : &instrument.key_regions.back();
        if (keys == nullptr || keys->first_key != region.first_key || keys->last_key != region.last_key)
        {
            if (keys != nullptr && keys->last_key >= region.first_key)
            {
                throw std::invalid_argument("Key ranges " + KeysText(keys->first_key, keys->last_key) + " and " +
                                            KeysText(region.first_key, region.last_key) +
                                            " overlap. Regions with different key ranges can't overlap.");
            }

            keys = &instrument.key_regions.emplace_back();
            keys->first_key = region.first_key;
            keys->last_key = region.last_key;
        }
        if (!keys->regions.empty() && keys->regions.back().last_velocity >= region.first_velocity)
        {
            throw std::invalid_argument("Two regions with keys " + KeysText(region.first_key, region.last_key) +
                                        " overlap at velocity " + std::to_string(region.first_velocity) + ".");
        }

        keys->regions.push_back({region.first_velocity, region.last_velocity, region.values});
    }

    return instrument;
}

std::optional<std::size_t> RegionAt(const sar::BankFile::Instrument& instrument, int key, int velocity)
{
    const std::vector<Region> regions = RegionsOf(instrument);
    for (std::size_t r = 0; r < regions.size(); ++r)
    {
        const Region& region = regions[r];
        if (key >= region.first_key && key <= region.last_key && velocity >= region.first_velocity &&
            velocity <= region.last_velocity)
        {
            return r;
        }
    }

    return std::nullopt;
}

sar::BankFile WithNewInstrument(sar::BankFile bank, uint32_t program, std::optional<sar::WaveId> wave)
{
    MakeRoom(bank, program);
    sar::BankFile::Instrument& instrument = bank.instruments[program].emplace();
    if (wave)
    {
        sar::BankFile::Region region;
        region.values.wave_id_index = WaveEntry(bank, *wave);
        instrument.key_regions.push_back({0, kHighestValue, {region}});
    }

    AddToOrder(bank, program);

    return bank;
}

sar::BankFile WithInstrumentCopy(sar::BankFile bank, uint32_t from, uint32_t to)
{
    sar::BankFile::Instrument copy = InstrumentAt(bank, from);
    MakeRoom(bank, to);
    bank.instruments[to] = std::move(copy);
    AddToOrder(bank, to);

    return bank;
}

sar::BankFile WithInstrumentMoved(sar::BankFile bank, uint32_t from, uint32_t to)
{
    sar::BankFile::Instrument moved = InstrumentAt(bank, from);
    if (from == to)
    {
        return bank;
    }

    MakeRoom(bank, to);
    bank.instruments[to] = std::move(moved);
    bank.instruments[from].reset();
    std::ranges::replace(bank.order, from, to);
    TrimInstruments(bank);

    return bank;
}

sar::BankFile WithoutInstrument(sar::BankFile bank, uint32_t program)
{
    InstrumentAt(bank, program);
    bank.instruments[program].reset();
    std::erase(bank.order, program);
    TrimInstruments(bank);

    return bank;
}

sar::BankFile WithRegions(sar::BankFile bank, uint32_t program, std::vector<Region> regions)
{
    InstrumentAt(bank, program);
    for (const Region& region : regions)
    {
        if (region.values.wave_id_index >= bank.waves.size())
        {
            throw std::invalid_argument("A region uses wave entry " + std::to_string(region.values.wave_id_index) +
                                        ", but the wave table has only " + std::to_string(bank.waves.size()) +
                                        " entries.");
        }
    }

    bank.instruments[program] = InstrumentOf(std::move(regions));
    KeepPlayedWaves(bank);

    return bank;
}

uint32_t WaveEntry(sar::BankFile& bank, sar::WaveId wave)
{
    const auto found = std::ranges::find(bank.waves, wave);
    if (found != bank.waves.end())
    {
        return static_cast<uint32_t>(found - bank.waves.begin());
    }

    bank.waves.push_back(wave);

    return static_cast<uint32_t>(bank.waves.size() - 1);
}

sar::BankFile BankModelOf(const format::File& file, const sar::SoundArchive& archive, uint32_t bank)
{
    const uint32_t file_id = FileIdOf(archive, FileKind::kBank, bank);
    if (const auto edited = file.banks.find(file_id); edited != file.banks.end())
    {
        return *edited->second;
    }

    try
    {
        return sar::BankFile::Read(song::ItemFile(archive, {song::ItemKind::kBank, bank, 0}));
    }
    catch (const std::exception& error)
    {
        throw std::invalid_argument(std::string("Can't edit this bank. ") + format::AsSentence(error.what()));
    }
}

format::File WithBankModel(const format::File& file, const sar::SoundArchive& archive, uint32_t bank,
                           sar::BankFile model)
{
    if (model.flavor != archive.ReadOptions().flavor)
    {
        throw std::invalid_argument("The bank and the archive are for different consoles.");
    }

    const uint32_t file_id = FileIdOf(archive, FileKind::kBank, bank);
    std::vector<uint8_t> bytes;
    try
    {
        bytes = model.Write();
    }
    catch (const std::invalid_argument& error)
    {
        throw std::invalid_argument(std::string("Can't write the bank file. ") + format::AsSentence(error.what()));
    }

    format::File result = ReplaceFile(file, file_id, bytes);
    result.banks[file_id] = std::move(model);

    // Where they can, the groups that load the bank and lack a wave archive that its waves now name take it. A group
    // that can't take it still lacks it, and the Bank Editor lists it (GroupsLackingWaves).
    const std::vector<uint32_t> needed = FilesOfItem(archive, sar::ItemId(sar::kBankItem, bank), {.file = bytes});
    const std::vector<uint32_t> lacking = GroupsLacking(file, archive, file_id, needed, true);
    if (!lacking.empty())
    {
        try
        {
            result = JoinGroups(result, lacking, std::vector<uint32_t>{sar::ItemId(sar::kBankItem, bank)}, {});
        }
        catch (const std::invalid_argument&)
        {
            // The groups stay as they were.
        }
    }

    return result;
}

std::vector<uint32_t> GroupsLackingWaves(const format::File& file, const sar::SoundArchive& archive, uint32_t bank)
{
    const uint32_t file_id = FileIdOf(archive, FileKind::kBank, bank);
    const std::vector<uint32_t> needed = FilesOfItem(archive, sar::ItemId(sar::kBankItem, bank));

    return GroupsLacking(file, archive, file_id, needed, false);
}

std::optional<uint32_t> WaveArchiveOfBank(const sar::BankFile& bank, const sar::SoundArchive& archive)
{
    for (const sar::WaveId& wave : bank.waves)
    {
        const uint32_t item = wave.wave_archive_item;
        if (sar::ItemType(item) == sar::kWaveArchiveItem && sar::ItemNumber(item) < archive.WaveArchives().size())
        {
            return sar::ItemNumber(item);
        }
    }

    return std::nullopt;
}

BankWaves WithBankWaves(const format::File& file, const sar::SoundArchive& archive, uint32_t bank,
                        const sar::BankFile& model, const std::vector<IncomingWave>& incoming)
{
    BankWaves result{file, {}};
    if (incoming.empty())
    {
        return result;
    }

    // The waves go after those of the bank's wave archive.
    if (const std::optional<uint32_t> wave_archive = WaveArchiveOfBank(model, archive))
    {
        const auto first = static_cast<uint32_t>(song::WaveCountOf(archive, *wave_archive));
        result.file = WithNewWaves(file, archive, *wave_archive, incoming);
        for (std::size_t k = 0; k < incoming.size(); ++k)
        {
            result.waves.push_back(
                {sar::ItemId(sar::kWaveArchiveItem, *wave_archive), first + static_cast<uint32_t>(k)});
        }

        return result;
    }

    // Without one, a new wave archive holds them. The new file goes after the archive's other files.
    if (!file.native || !file.native->tables)
    {
        throw std::invalid_argument(
            "Can't add a wave archive for the new waves. Hokuto can't read the tables of this archive.");
    }

    const sar::Flavor flavor = archive.ReadOptions().flavor;
    sar::WaveArchiveFile waves;
    waves.flavor = flavor;
    waves.version = IncomingVersions(archive, FileKind::kWaveArchive).file;
    for (const IncomingWave& wave : incoming)
    {
        waves.waves.push_back(wave.bytes);
    }

    const bool named = sar::TablesOf(archive.Bytes(), flavor).strings.has_value();
    const std::string name = named ? NewWaveArchiveName(*file.native->tables, archive, bank) : std::string();
    const auto index = static_cast<uint32_t>(file.native->tables->wave_archives.size());
    const auto file_id = static_cast<uint32_t>(archive.FileCount());
    result.file = AddWaveArchive(file, name, waves.Write());
    for (std::size_t k = 0; k < incoming.size(); ++k)
    {
        result.waves.push_back({sar::ItemId(sar::kWaveArchiveItem, index), static_cast<uint32_t>(k)});
        if (incoming[k].model)
        {
            result.file.waves[{file_id, static_cast<uint32_t>(k)}] = *incoming[k].model;
        }
    }

    // Where they can, the groups that load the bank load the new wave archive too.
    const std::vector<uint32_t> loading = GroupsLoading(file, archive, FileIdOf(archive, FileKind::kBank, bank));
    if (!loading.empty())
    {
        try
        {
            result.file =
                JoinGroups(result.file, loading, std::vector<uint32_t>{sar::ItemId(sar::kWaveArchiveItem, index)}, {});
        }
        catch (const std::invalid_argument&)
        {
            // The groups stay as they were. The Bank Editor lists them (GroupsLackingWaves).
        }
    }

    return result;
}

std::vector<uint8_t> EmptyBank(sar::Flavor flavor)
{
    sar::BankFile bank;
    bank.flavor = flavor;

    return bank.Write();
}

std::string WaveEntryName(const sar::SoundArchive& archive, const sar::BankFile& bank, uint32_t entry)
{
    if (entry >= bank.waves.size())
    {
        return "entry " + std::to_string(entry);
    }

    const sar::WaveId& wave = bank.waves[entry];
    const uint32_t wave_archive = sar::ItemNumber(wave.wave_archive_item);
    if (wave_archive >= archive.WaveArchives().size())
    {
        return "wave archive " + std::to_string(wave_archive) + ", wave " + std::to_string(wave.index);
    }

    return song::ItemName(archive, {song::ItemKind::kWave, wave_archive, wave.index});
}

namespace
{

namespace rsar = ntdlib::rsar;

// A bank of the Wii in ntdlib's model, and its file's ID.
struct WiiBank
{
    uint32_t file_id = 0;
    rsar::BankFile read;
};

WiiBank ReadWiiBank(const rsar::SoundArchive& archive, uint32_t bank)
{
    if (bank >= archive.Banks().size())
    {
        throw std::invalid_argument("The archive has only " + std::to_string(archive.Banks().size()) + " banks.");
    }

    WiiBank wii;
    wii.file_id = archive.Banks()[bank].file_id;
    const std::optional<rsar::FileData> data = archive.File(wii.file_id);
    if (!data)
    {
        throw std::invalid_argument(
            song::WiiMissingNote(archive, wii.file_id, "The bank file or its wave data is missing from the archive."));
    }

    try
    {
        wii.read = rsar::BankFile::Read(data->file, data->wave_data);
    }
    catch (const std::exception& error)
    {
        throw std::invalid_argument(std::string("Can't edit this bank. ") + format::AsSentence(error.what()));
    }

    return wii;
}

} // namespace

sar::BankFile BankModelOf(const rsar::SoundArchive& archive, uint32_t bank)
{
    const WiiBank wii = ReadWiiBank(archive, bank);
    sar::BankFile model;
    model.version = wii.read.version;
    for (uint32_t wave = 0; wave < wii.read.waves.size(); ++wave)
    {
        model.waves.push_back({sar::ItemId(sar::kWaveArchiveItem, wii.file_id), wave});
    }

    // Each instrument as NW4R SND plays it. A region that names no wave of the bank plays nothing, and stays out.
    const std::vector<std::optional<rsar::BankFile::Instrument>> instruments = wii.read.Instruments();
    model.instruments.resize(instruments.size());
    for (std::size_t program = 0; program < instruments.size(); ++program)
    {
        if (!instruments[program])
        {
            continue;
        }

        sar::BankFile::Instrument& instrument = model.instruments[program].emplace();
        for (const rsar::BankFile::KeyRegion& keys : instruments[program]->key_regions)
        {
            sar::BankFile::KeyRegion made{keys.first_key, keys.last_key, {}};
            for (const rsar::BankFile::VelocityRegion& region : keys.regions)
            {
                const rsar::Region& values = region.values;
                if (values.surround_pan != 0 || values.wave_data_location != 0 || values.note_off_type > 1)
                {
                    throw std::invalid_argument("Can't edit this bank. Program " + std::to_string(program) +
                                                " has a region with a surround pan, a wave address or a note-off "
                                                "type that the Bank Editor doesn't support.");
                }
                if (values.wave_index < 0 || static_cast<std::size_t>(values.wave_index) >= wii.read.waves.size())
                {
                    continue;
                }

                made.regions.push_back({region.first_velocity, region.last_velocity, rsar::ValuesOf(values)});
            }
            if (!made.regions.empty())
            {
                instrument.key_regions.push_back(std::move(made));
            }
        }
    }

    TrimInstruments(model);

    return model;
}

namespace
{

// The place in `made_waves` of the wave of each entry of the wave table of `model`, a bank whose wave archive is file
// `file_id` of `archive` (WithBankModel). `made_waves` holds the bank's waves and the new waves, and gains a copy of
// each wave of another file's wave data that an entry names. Throws std::invalid_argument with a message for the user
// for an entry that names no wave.
std::vector<int32_t> WavePlaces(const rsar::SoundArchive& archive, const sar::BankFile& model, uint32_t file_id,
                                std::vector<rsar::WaveFile>& made_waves)
{
    const std::size_t own = made_waves.size();
    std::vector<int32_t> places;
    std::vector<std::pair<sar::WaveId, int32_t>> copies;
    for (const sar::WaveId& entry : model.waves)
    {
        const uint32_t of = sar::ItemNumber(entry.wave_archive_item);
        if (sar::ItemType(entry.wave_archive_item) != sar::kWaveArchiveItem)
        {
            throw std::invalid_argument("An entry in the wave table doesn't refer to a wave archive.");
        }

        if (of == file_id)
        {
            if (entry.index >= own)
            {
                throw std::invalid_argument("A region uses wave " + std::to_string(entry.index) +
                                            ", but the bank has only " + std::to_string(own) + " waves.");
            }

            places.push_back(static_cast<int32_t>(entry.index));
            continue;
        }

        const auto same = [&entry](const std::pair<sar::WaveId, int32_t>& copy)
        {
            return copy.first == entry;
        };
        if (const auto copied = std::ranges::find_if(copies, same); copied != copies.end())
        {
            places.push_back(copied->second);
            continue;
        }

        std::vector<rsar::WaveFile> others = WavesOf(archive, of);
        if (entry.index >= others.size())
        {
            const std::string name = song::ItemName(archive, {song::ItemKind::kWaveArchive, of, 0});
            throw std::invalid_argument("A region uses wave " + std::to_string(entry.index) + " of " + name + ", but " +
                                        name + " has only " + std::to_string(others.size()) + " waves.");
        }

        made_waves.push_back(std::move(others[entry.index]));
        places.push_back(static_cast<int32_t>(made_waves.size() - 1));
        copies.emplace_back(entry, places.back());
    }

    return places;
}

// The instruments of `model` on the Wii, each region playing the wave at `places` of its entry (WithBankModel). Throws
// std::invalid_argument with a message for the user for a region whose entry isn't in the wave table.
std::vector<std::optional<rsar::BankFile::Instrument>> WiiInstruments(const sar::BankFile& model,
                                                                      const std::vector<int32_t>& places)
{
    std::vector<std::optional<rsar::BankFile::Instrument>> instruments(model.instruments.size());
    for (std::size_t program = 0; program < model.instruments.size(); ++program)
    {
        if (!model.instruments[program])
        {
            continue;
        }

        rsar::BankFile::Instrument& instrument = instruments[program].emplace();
        for (const sar::BankFile::KeyRegion& keys : model.instruments[program]->key_regions)
        {
            rsar::BankFile::KeyRegion& made = instrument.key_regions.emplace_back();
            made.first_key = keys.first_key;
            made.last_key = keys.last_key;
            for (const sar::BankFile::Region& region : keys.regions)
            {
                if (region.values.wave_id_index >= places.size())
                {
                    throw std::invalid_argument(
                        "A region uses wave entry " + std::to_string(region.values.wave_id_index) +
                        ", but the wave table has only " + std::to_string(places.size()) + " entries.");
                }

                made.regions.push_back({region.first_velocity, region.last_velocity,
                                        rsar::RegionOf(region.values, places[region.values.wave_id_index])});
            }
        }
    }

    return instruments;
}

} // namespace

format::File WithBankModel(const format::File& file, const rsar::SoundArchive& archive, uint32_t bank,
                           const sar::BankFile& model, std::vector<rsar::WaveFile> waves)
{
    const WiiBank wii = ReadWiiBank(archive, bank);

    // The bank's waves, the new waves, then a copy of each wave of another file's wave data that an entry names.
    std::vector<rsar::WaveFile> made_waves = wii.read.waves;
    const std::size_t present = made_waves.size();
    std::ranges::move(waves, std::back_inserter(made_waves));
    const std::vector<int32_t> places = WavePlaces(archive, model, wii.file_id, made_waves);

    // The instruments, each region playing its entry's wave.
    const std::vector<std::optional<rsar::BankFile::Instrument>> instruments = WiiInstruments(model, places);

    // The bank in every group that holds it. Its waves keep their places and their models.
    rsar::BankFile::Files files;
    try
    {
        files = rsar::BankFile::FromInstruments(wii.read.version, instruments, std::move(made_waves)).Write();
    }
    catch (const std::invalid_argument& error)
    {
        throw std::invalid_argument(std::string("Can't write the bank file. ") + format::AsSentence(error.what()));
    }

    format::File result = ReplaceFile(file, wii.file_id, files.file, std::span<const uint8_t>(files.wave_data));
    for (const auto& [place, wave_model] : file.waves)
    {
        if (place.file == wii.file_id && place.index < present)
        {
            result.waves.emplace(place, wave_model);
        }
    }

    return result;
}

std::vector<sar::WaveId> NewWaveIds(const rsar::SoundArchive& archive, uint32_t bank, std::size_t count)
{
    const WiiBank wii = ReadWiiBank(archive, bank);
    std::vector<sar::WaveId> ids;
    for (std::size_t k = 0; k < count; ++k)
    {
        ids.push_back(
            {sar::ItemId(sar::kWaveArchiveItem, wii.file_id), static_cast<uint32_t>(wii.read.waves.size() + k)});
    }

    return ids;
}

std::string WaveEntryName(const rsar::SoundArchive& archive, const sar::BankFile& bank, uint32_t entry)
{
    if (entry >= bank.waves.size())
    {
        return "entry " + std::to_string(entry);
    }

    const sar::WaveId& wave = bank.waves[entry];
    return song::ItemName(archive, {song::ItemKind::kWave, sar::ItemNumber(wave.wave_archive_item), wave.index});
}

rsar::BankFile::Files EmptyBank(const rsar::SoundArchive& archive, std::vector<rsar::WaveFile> waves)
{
    return rsar::BankFile::FromInstruments(NewWiiFileVersion(archive, FileKind::kBank), {}, std::move(waves)).Write();
}

} // namespace hokuto::edit
