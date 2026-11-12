// SPDX-License-Identifier: MIT

// The views of the DS's banks, waves and streams, and of DSE's banks and waves.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/dse/formats.h"
#include "ntdlib/sar/encodings.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sdat/decode.h"
#include "ntdlib/sdat/formats.h"
#include "ntdlib/wave/decode.h"

#include "app/item_views.h"
#include "app/item_views_parts.h"
#include "app/key_edits.h"
#include "app/time_text.h"
#include "edit/ds_banks.h"
#include "edit/ds_streams.h"
#include "edit/ds_waves.h"
#include "edit/dse_banks.h"
#include "song/ds.h"
#include "song/dse.h"
#include "song/items.h"

namespace hokuto::app
{
namespace
{

namespace sar = ntdlib::sar;
using item_views_parts::kHighest;

// The name of a row's place in a DS bank: the program, with a drum set's key or a key split's run of keys.
std::string DsPlaceName(const DsProgram& program, const DsRegionRow& row)
{
    std::string name = "program " + std::to_string(program.program);
    if (program.type == ntdlib::sdat::InstrumentType::kDrumSet)
    {
        name += ", key " + KeyName(row.first_key);
    }
    else if (program.type == ntdlib::sdat::InstrumentType::kKeySplit)
    {
        name += KeysDetail(row.first_key, row.last_key);
    }

    return name;
}

// The program that the Bank Editor shows for instrument `program` of `bank`, without the places that share its bytes.
DsProgram DsProgramOf(const ntdlib::sdat::Bank& bank, uint32_t program)
{
    namespace sdat = ntdlib::sdat;
    const sdat::Instrument& instrument = bank.instruments[program];
    DsProgram shown{program, instrument.type, DsKindName(instrument.type), {}};
    const auto add = [&shown](int first, int last, std::size_t region, const sdat::Region& values)
    {
        if (values.type != sdat::InstrumentType::kNone)
        {
            shown.rows.push_back({first, last, region, values, DsKindName(values.type), {}});
        }
    };
    if (instrument.type == sdat::InstrumentType::kDrumSet)
    {
        for (std::size_t r = 0; r < instrument.regions.size(); ++r)
        {
            const int key = instrument.min_key + static_cast<int>(r);
            add(key, key, r, instrument.regions[r]);
        }
    }
    else if (instrument.type == sdat::InstrumentType::kKeySplit)
    {
        int first = 0;
        for (std::size_t r = 0; r < instrument.regions.size(); ++r)
        {
            const int last = instrument.split_keys[r];
            add(first, last, r, instrument.regions[r]);
            first = last + 1;
        }
    }
    else
    {
        add(0, kHighest, 0, instrument.region);
    }

    return shown;
}

// The line of a DS wave's window that names the banks whose slots name wave archive `wave_archive`, and the programs
// whose regions play `wave` from that slot.
std::string DsWaveReach(const song::DsArchive& archive, uint32_t wave_archive, uint32_t wave)
{
    namespace sdat = ntdlib::sdat;
    const sdat::Archive& sdat_archive = archive.archive;
    std::vector<std::string> playing;
    for (uint32_t bank = 0; bank < sdat_archive.banks.size(); ++bank)
    {
        const std::optional<sdat::BankInfo>& info = sdat_archive.banks[bank];
        if (!info)
        {
            continue;
        }
        std::set<uint32_t> programs;
        try
        {
            const sdat::Bank file = sdat::Bank::Parse(sdat_archive.File(info->file_id));
            for (uint32_t program = 0; program < file.instruments.size(); ++program)
            {
                const sdat::Instrument& instrument = file.instruments[program];
                std::vector<sdat::Region> regions = instrument.regions;
                regions.push_back(instrument.region);
                for (const sdat::Region& region : regions)
                {
                    if (region.type == sdat::InstrumentType::kPcm && region.wave == wave &&
                        region.wave_archive < sdat::kWaveArchiveSlots &&
                        info->wave_archives[region.wave_archive] == wave_archive)
                    {
                        programs.insert(program);
                    }
                }
            }
        }
        catch (const std::exception&)
        {
            continue;
        }
        if (!programs.empty())
        {
            std::string line = song::DsBankName(archive, bank) + (programs.size() == 1 ? " (program " : " (programs ");
            for (const uint32_t program : programs)
            {
                line += (program == *programs.begin() ? "" : ", ") + std::to_string(program);
            }
            playing.push_back(line + ")");
        }
    }

    return playing.empty() ? "No bank uses this wave." : "Edits affect " + JoinedNames(playing) + ".";
}

} // namespace

WaveView MakeDsStreamView(const song::DsArchive& archive, uint32_t stream)
{
    namespace sdat = ntdlib::sdat;
    const edit::DsStreamValues values = edit::DsStreamValuesOf(archive, stream);
    const sdat::Stream parsed = sdat::Stream::Parse(archive.archive.File(edit::DsStreamFileId(archive, stream)));

    WaveView view;
    view.ds = true;
    view.stream = true;
    view.name = song::DsStreamName(archive, stream);
    for (uint32_t channel = 0; channel < parsed.channels; channel++)
    {
        view.samples.push_back(sdat::DecodeStream(parsed, channel));
    }
    view.peaks = PeaksOf(view.samples);
    view.held = view.samples.empty() ? 0 : static_cast<uint32_t>(view.samples[0].size());
    view.ds_block_samples = parsed.block_samples;
    view.values.encoding = sar::EncodingOf(parsed.encoding);
    view.values.rate = values.rate;
    view.values.loop = values.loop;
    view.values.loop_start = values.loop_start;
    view.values.loop_end = view.held;
    view.encodings = {sar::WaveEncoding::kImaAdpcm, sar::WaveEncoding::kPcm16, sar::WaveEncoding::kPcm8};
    view.regions = "None";
    view.reach = "The game code starts this stream by number.";

    return view;
}

WaveView MakeDsWaveView(const song::DsArchive& archive, uint32_t wave_archive, uint32_t wave)
{
    namespace sdat = ntdlib::sdat;
    const edit::DsWaveValues values = edit::DsWaveValuesOf(archive, wave_archive, wave);
    const sdat::Archive& sdat_archive = archive.archive;
    const sdat::WaveArchive parsed = sdat::WaveArchive::Parse(
        sdat_archive.File(sdat_archive.wave_archives[wave_archive]->file_id & sdat::kFileIdBits));
    const sdat::Wave& present = parsed.waves[wave];

    // The window uses the names of the encodings on the later consoles. Another encoding makes the wave again.
    const sar::WaveEncoding encoding = sar::EncodingOf(present.encoding);
    WaveView view;
    view.ds = true;
    view.name = song::DsWaveArchiveName(archive, wave_archive) + ", wave " + std::to_string(wave);
    view.samples.push_back(sdat::DecodeWave(present));
    view.peaks = PeaksOf(view.samples);
    view.held = static_cast<uint32_t>(view.samples[0].size());
    view.values.encoding = encoding;
    view.values.rate = values.rate;
    view.values.loop = values.loop && values.loop_start < view.held;
    view.values.loop_start = view.values.loop ? values.loop_start : 0;
    view.values.loop_end = view.held;
    view.encodings = {sar::WaveEncoding::kImaAdpcm, sar::WaveEncoding::kPcm16, sar::WaveEncoding::kPcm8};

    // The banks whose slots name the wave archive, and the programs whose regions play the wave from that slot.
    view.reach = DsWaveReach(archive, wave_archive, wave);
    view.sizes.push_back(
        song::DsWaveArchiveName(archive, wave_archive) + ": " +
        Grouped(sdat_archive.files[sdat_archive.wave_archives[wave_archive]->file_id & sdat::kFileIdBits].size) +
        " bytes");
    view.sizes.emplace_back("Only a new encoding changes the size.");

    return view;
}

WaveView MakeDseWaveView(const song::DseSet& set, uint32_t bank, uint32_t wave)
{
    const edit::DseWaveValues values = edit::DseWaveValuesOf(set, bank, wave);
    const ntdlib::wave::Pcm pcm = song::DseWavePcm(set, bank, wave);
    const uint8_t format = set.banks[bank].bank->waves[wave]->bytes[0x13];

    WaveView view;
    view.ds = true;
    view.dse = true;
    view.name = song::DseItemName(set, {.kind = song::ItemKind::kWave, .index = bank, .wave = wave});
    view.samples = pcm.channels;
    view.peaks = PeaksOf(view.samples);
    view.held = view.samples.empty() ? 0 : static_cast<uint32_t>(view.samples[0].size());
    view.values.encoding = format == 0   ? sar::WaveEncoding::kPcm8
                           : format == 2 ? sar::WaveEncoding::kImaAdpcm
                                         : sar::WaveEncoding::kPcm16;
    view.values.rate = values.rate;
    view.values.loop = values.loop && values.loop_start < view.held;
    view.values.loop_start = view.values.loop ? values.loop_start : 0;
    view.values.loop_end = view.held;
    view.encodings = {view.values.encoding};

    // The banks that contain the wave in their tables, and the programs of each that play it.
    std::vector<std::string> playing;
    std::vector<const ntdlib::dse::WaveBank*> listed;
    for (const song::DseBank& other : set.banks)
    {
        const bool holds = other.bank == set.banks[bank].bank || other.main_bank == set.banks[bank].bank;
        if (!holds || std::ranges::find(listed, other.bank.get()) != listed.end() || !other.bank->instruments)
        {
            continue;
        }

        listed.push_back(other.bank.get());
        std::vector<uint32_t> programs;
        for (const uint32_t program : song::DseBankPrograms(*other.bank))
        {
            const auto& splits = other.bank->instruments->programs[program]->splits;
            if (std::ranges::any_of(splits, [wave](const edit::DseSplit& split)
                                    { return edit::DseSplitWave(split) == static_cast<int>(wave); }))
            {
                programs.push_back(program);
            }
        }
        if (programs.empty())
        {
            continue;
        }

        std::string line = song::DseBankName(*other.bank) + (programs.size() == 1 ? " (program " : " (programs ");
        for (std::size_t i = 0; i < programs.size(); ++i)
        {
            line += (i == 0 ? "" : ", ") + std::to_string(programs[i]);
        }
        playing.push_back(line + ")");
    }
    constexpr std::size_t kMostListed = 8;
    const std::size_t shown = std::min(playing.size(), kMostListed);
    std::vector<std::string> named(playing.begin(), playing.begin() + static_cast<std::ptrdiff_t>(shown));
    if (playing.size() > shown)
    {
        const std::size_t rest = playing.size() - shown;
        named.push_back(std::to_string(rest) + (rest == 1 ? " more bank" : " more banks"));
    }
    view.reach = playing.empty() ? "No bank uses this wave." : "Edits affect " + JoinedNames(named) + ".";
    view.sizes.emplace_back("A new rate or loop doesn't change the size of the bank files.");

    return view;
}

DseBankView MakeDseBankView(const song::DseSet& set, uint32_t bank)
{
    if (bank >= set.banks.size())
    {
        throw std::invalid_argument("The image has no bank " + std::to_string(bank) + ".");
    }

    const song::DseBank& found = set.banks[bank];
    DseBankView view;
    view.bank = bank;
    view.name = song::DseBankName(*found.bank);
    const auto wave_name = [](int wave)
    {
        return "Wave " + std::to_string(wave);
    };
    for (const uint32_t program : song::DseBankPrograms(*found.bank))
    {
        DseProgramView shown;
        shown.program = program;
        const auto& splits = found.bank->instruments->programs[program]->splits;
        for (std::size_t split = 0; split < splits.size(); ++split)
        {
            shown.rows.push_back({split, splits[split], wave_name(edit::DseSplitWave(splits[split]))});
        }
        view.programs.push_back(std::move(shown));
    }
    for (const uint32_t wave : song::DseBankWaves(*found.bank))
    {
        view.waves.push_back(static_cast<uint16_t>(wave));
        view.wave_names.push_back(wave_name(static_cast<int>(wave)));
    }
    for (const uint32_t sound : song::DseSounds(set))
    {
        if (std::ranges::find(set.sounds[sound].banks, found.bank) != set.sounds[sound].banks.end())
        {
            view.sounds.push_back(song::DseSoundName(set, sound));
        }
    }

    return view;
}

std::string DsKindName(ntdlib::sdat::InstrumentType type)
{
    using enum ntdlib::sdat::InstrumentType;
    switch (type)
    {
    case kPcm:
        return "Wave";
    case kPsg:
        return "Square";
    case kNoise:
        return "Noise";
    case kDirectPcm:
        return "Wave by address";
    case kDummy:
        return "Silent";
    case kDrumSet:
        return "Drum set";
    case kKeySplit:
        return "Key split";
    case kNone:
        break;
    }

    return "None";
}

namespace
{

// The programs of bank file `file` in `view`, with the other places that share each region's values.
void FillDsPrograms(DsBankView& view, const ntdlib::sdat::Bank& file)
{
    namespace sdat = ntdlib::sdat;
    std::map<uint32_t, std::vector<std::string>> places;
    for (uint32_t program = 0; program < file.instruments.size(); ++program)
    {
        if (file.instruments[program].type == sdat::InstrumentType::kNone)
        {
            continue;
        }
        DsProgram shown = DsProgramOf(file, program);
        for (const DsRegionRow& row : shown.rows)
        {
            places[edit::DsRegionBytesOf(file, program, row.region).values].push_back(DsPlaceName(shown, row));
        }
        view.programs.push_back(std::move(shown));
    }
    for (DsProgram& program : view.programs)
    {
        for (DsRegionRow& row : program.rows)
        {
            const std::string place = DsPlaceName(program, row);
            for (const std::string& other : places[edit::DsRegionBytesOf(file, program.program, row.region).values])
            {
                if (other != place)
                {
                    row.shared.push_back(other);
                }
            }
        }
    }
}

// The waves of the wave archives in the slots of `info` in `view`.
void FillDsSlotWaves(DsBankView& view, const song::DsArchive& archive, const ntdlib::sdat::BankInfo& info)
{
    for (std::size_t slot = 0; slot < info.wave_archives.size(); ++slot)
    {
        const uint16_t wave_archive = info.wave_archives[slot];
        if (wave_archive == ntdlib::sdat::kNoWaveArchive)
        {
            continue;
        }
        const std::string name = song::DsWaveArchiveName(archive, wave_archive);
        const std::size_t waves = song::DsWaveCount(archive, wave_archive);
        for (std::size_t wave = 0; wave < waves; ++wave)
        {
            view.waves.push_back({static_cast<uint16_t>(slot), static_cast<uint16_t>(wave)});
            view.wave_names.push_back(name + ", wave " + std::to_string(wave));
        }
    }
}

// The banks that play the same file as `bank` in `view`, and the sequences that play any of them.
void FillDsBankSounds(DsBankView& view, const song::DsArchive& archive, uint32_t bank,
                      const ntdlib::sdat::BankInfo& info)
{
    const ntdlib::sdat::Archive& sdat_archive = archive.archive;
    std::set<uint32_t> banks = {bank};
    for (uint32_t other = 0; other < sdat_archive.banks.size(); ++other)
    {
        if (other != bank && sdat_archive.banks[other] && sdat_archive.banks[other]->file_id == info.file_id)
        {
            banks.insert(other);
            view.sharing_banks.push_back(song::DsBankName(archive, other));
        }
    }
    for (const uint32_t sound : song::DsSounds(archive))
    {
        try
        {
            const std::optional<uint32_t> played = song::DsBankOf(archive, sound);
            if (played && banks.contains(*played))
            {
                view.sounds.push_back(song::DsSoundName(archive, sound));
            }
        }
        catch (const std::exception&)
        {
        }
    }
}

} // namespace

DsBankView MakeDsBankView(const song::DsArchive& archive, uint32_t bank)
{
    namespace sdat = ntdlib::sdat;
    const sdat::Archive& sdat_archive = archive.archive;
    if (bank >= sdat_archive.banks.size() || !sdat_archive.banks[bank])
    {
        throw std::invalid_argument("The archive has no bank " + std::to_string(bank) + ".");
    }
    const sdat::BankInfo& info = *sdat_archive.banks[bank];
    const sdat::Bank file = sdat::Bank::Parse(sdat_archive.File(info.file_id));

    DsBankView view;
    view.bank = bank;
    view.name = song::DsBankName(archive, bank);
    view.model = file;
    view.slots = info.wave_archives;
    view.wave_archives = song::DsWaveArchives(archive);
    for (const uint32_t wave_archive : view.wave_archives)
    {
        view.wave_archive_names.push_back(song::DsWaveArchiveName(archive, wave_archive));
    }
    FillDsPrograms(view, file);

    // The waves of the wave archives in the bank's slots.
    FillDsSlotWaves(view, archive, info);

    // The banks that play the same file, and the sequences that play any of them.
    FillDsBankSounds(view, archive, bank, info);

    return view;
}

std::optional<int> DsRowOf(const ntdlib::sdat::Bank& bank, uint32_t program, int key)
{
    if (program >= bank.instruments.size())
    {
        return std::nullopt;
    }

    const DsProgram shown = DsProgramOf(bank, program);
    const auto plays = [key](const DsRegionRow& row)
    {
        return row.first_key <= key && key <= row.last_key;
    };
    const auto row = std::ranges::find_if(shown.rows, plays);
    return row != shown.rows.end() ? std::optional<int>(row->first_key) : std::nullopt;
}

} // namespace hokuto::app
