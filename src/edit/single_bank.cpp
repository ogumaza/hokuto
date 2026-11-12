// SPDX-License-Identifier: MIT

// The banks of a sound for a console that plays one bank a sound (edit/other_console.h): a later console's bank as the
// reader plays it, its waves as waves of another console, and the slots of a sound merged into one bank.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/rsar/bank_file.h"
#include "ntdlib/rsar/convert.h"
#include "ntdlib/rsar/wave_file.h"
#include "ntdlib/sar/bank_file.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sar/wave_file.h"

#include "edit/other_console.h"
#include "edit/other_console_parts.h"
#include "edit/wii_programs.h"
#include "format/kinds.h"
#include "song/items.h"

namespace hokuto::edit
{

namespace
{

namespace sar = ntdlib::sar;
using other_console_parts::BankFileOfRegions;

} // namespace

namespace other_console_parts
{

// The instruments of `bank` as the later consoles' reader plays them (sar::Instrument::Find): a key region for each run
// of keys whose velocities find the same regions, and a velocity region for each run of velocities that finds one
// region. A program that plays no region at any key and velocity has no instrument.
sar::BankFile BankFileOfRegions(const sar::Bank& bank)
{
    sar::BankFile file;
    file.waves = bank.waves;
    for (const std::optional<sar::Instrument>& instrument : bank.instruments)
    {
        std::optional<sar::BankFile::Instrument>& out = file.instruments.emplace_back();
        if (!instrument)
        {
            continue;
        }

        // The region that each key plays at each velocity.
        using Column = std::array<const sar::VelocityRegion*, 128>;
        std::array<Column, 128> grid{};
        for (int key = 0; key < 128; ++key)
        {
            for (int velocity = 0; velocity < 128; ++velocity)
            {
                grid[key][velocity] = instrument->Find(key, velocity);
            }
        }

        sar::BankFile::Instrument regions;
        for (int first = 0; first < 128;)
        {
            int last = first;
            while (last + 1 < 128 && grid[last + 1] == grid[first])
            {
                ++last;
            }

            const Column& column = grid[first];
            sar::BankFile::KeyRegion key_region{
                .first_key = static_cast<uint8_t>(first), .last_key = static_cast<uint8_t>(last), .regions = {}};
            for (int low = 0; low < 128;)
            {
                int high = low;
                while (high + 1 < 128 && column[high + 1] == column[low])
                {
                    ++high;
                }

                if (column[low] != nullptr)
                {
                    key_region.regions.push_back({.first_velocity = static_cast<uint8_t>(low),
                                                  .last_velocity = static_cast<uint8_t>(high),
                                                  .values = *column[low]});
                }

                low = high + 1;
            }
            if (!key_region.regions.empty())
            {
                regions.key_regions.push_back(std::move(key_region));
            }

            first = last + 1;
        }
        if (!regions.key_regions.empty())
        {
            out = std::move(regions);
        }
    }

    return file;
}

const std::vector<std::vector<uint8_t>>& WavesOf(const sar::SoundArchive& archive, uint32_t item,
                                                 WaveArchiveWaves& read)
{
    auto [place, added] = read.try_emplace(item);
    const std::optional<std::size_t> wave_archive =
        sar::ItemIndex(item, sar::kWaveArchiveItem, archive.WaveArchives().size());
    if (added && wave_archive)
    {
        const std::span<const uint8_t> bytes = song::ItemFile(
            archive, {.kind = song::ItemKind::kWaveArchive, .index = static_cast<uint32_t>(*wave_archive)});
        try
        {
            place->second =
                bytes.empty() ? std::vector<std::vector<uint8_t>>{} : sar::WaveArchiveFile::Read(bytes).waves;
        }
        catch (const std::exception&)
        {
            place->second.clear();
        }
    }

    return place->second;
}

// The name of bank `id` of `archive`, or BANK_ and its number for a bank without a name.
std::string BankNameOf(const sar::SoundArchive& archive, uint32_t id)
{
    const std::optional<std::size_t> index = sar::ItemIndex(id, sar::kBankItem, archive.Banks().size());
    return index && !archive.Banks()[*index].name.empty() ? archive.Banks()[*index].name
                                                          : "BANK_" + std::to_string(sar::ItemNumber(id));
}

// The bytes of bank `id`'s file in `archive`. Throws std::invalid_argument for a bank that the archive lacks.
std::span<const uint8_t> BankBytesOf(const sar::SoundArchive& archive, uint32_t id)
{
    const std::optional<std::size_t> index = sar::ItemIndex(id, sar::kBankItem, archive.Banks().size());
    if (!index)
    {
        throw std::invalid_argument("The archive has no such bank.");
    }

    const std::span<const uint8_t> file =
        song::ItemFile(archive, {.kind = song::ItemKind::kBank, .index = static_cast<uint32_t>(*index)});
    if (file.empty())
    {
        throw std::invalid_argument("The archive has no file for the bank.");
    }

    return file;
}

// The programs of bank `id` of `archive`: the entries of its table of instruments. 0 for a bank that can't come along.
uint32_t BankProgramsOf(const sar::SoundArchive& archive, uint32_t id)
{
    try
    {
        return static_cast<uint32_t>(
            sar::Bank::Parse(BankBytesOf(archive, id), archive.ReadOptions()).instruments.size());
    }
    catch (const std::exception&)
    {
        return 0;
    }
}

std::optional<WiiPrograms> SoundProgramsOf(const sar::SoundArchive& archive, const format::NativeSound& source,
                                           const ntdlib::commands::Sequence& sequence, uint32_t start)
{
    std::vector<uint32_t> slot_programs;
    for (const uint32_t id : source.banks)
    {
        slot_programs.push_back(id == sar::kNoItem ? 0 : BankProgramsOf(archive, id));
    }

    return WiiProgramsOf(sequence, start, slot_programs);
}

void SetSlotPrograms(Song& song, const WiiPrograms& programs)
{
    for (const auto& [slot, range] : programs.slots)
    {
        song.result.slot_programs[slot] = range.first;
    }
}

// `bank` with only the instruments of `programs`. Its table of waves holds only the waves that those instruments play,
// in their order. A region that plays a wave past the table still does.
sar::BankFile WithOnlyInstruments(sar::BankFile bank, const std::set<uint32_t>& programs)
{
    KeepOnlyInstruments(bank, programs, [](const sar::WaveId& id) { return id; });

    return bank;
}

// The warning about `left` waves of bank `name` that the Wii doesn't play.
std::string LeftWavesWarning(std::size_t left, const std::string& name)
{
    return std::to_string(left) + (left == 1 ? " wave of bank " : " waves of bank ") + name +
           (left == 1 ? " is left out because it uses an encoding that the Wii can't play, such as IMA-ADPCM. Its "
                        "regions are silent."
                      : " are left out because they use an encoding that the Wii can't play, such as IMA-ADPCM. Their "
                        "regions are silent.");
}

// Bank `id` of `archive` as a bank of the Wii with an RWAR of its waves (ntdlib::rsar::WiiBankOf and WiiWaveOf). The
// bank plays the region that the later consoles' reader finds at each key and velocity (BankFileOfRegions). A wave that
// the archive lacks plays nothing on either console. A wave that the Wii can't play, such as one in IMA-ADPCM, stays
// behind with a warning, and its regions play nothing. A bank that can't come along stays behind with a warning, and
// the sound plays an empty bank in its place. With `kept`, the bank holds only the instruments of its programs.
WiiBank WiiBankFrom(Song& song, const sar::SoundArchive& archive, uint32_t id, WaveArchiveWaves& read,
                    const std::optional<std::set<uint32_t>>& kept)
{
    WiiBank bank{.name = BankNameOf(archive, id), .files = {}};
    try
    {
        const WavedBank<ntdlib::rsar::WaveFile> later =
            WavedBankOf<ntdlib::rsar::WaveFile>(archive, id, read, kept, ntdlib::rsar::WiiWaveOf);
        bank.files = ntdlib::rsar::WiiBankOf(later.file, kWiiBankVersion, later.waves).Write();
        if (later.left > 0)
        {
            song.result.warnings.push_back(LeftWavesWarning(later.left, bank.name));
        }
    }
    catch (const std::exception& error)
    {
        song.result.warnings.push_back("Bank " + bank.name + " is left out. " + format::AsSentence(error.what()) +
                                       " The sound uses an empty bank instead.");
        bank.files = ntdlib::rsar::BankFile::FromInstruments(kWiiBankVersion, {}, {}).Write();
    }

    return bank;
}

// The instruments that slot `slot` keeps where a sound's bank takes only the instruments that the sound selects: the
// programs that the slot's notes play, and those of the notes whose slot a variable or a random number selects
// (WiiPrograms::played). Nothing where a variable can give the program of such a note: the slot keeps every instrument.
std::optional<std::set<uint32_t>> KeptOf(const WiiPrograms& programs, int slot)
{
    const auto played = [&programs](int of) -> std::optional<std::set<uint32_t>>
    {
        const auto found = programs.played.find(of);
        return found != programs.played.end() ? found->second : std::set<uint32_t>{};
    };
    std::optional<std::set<uint32_t>> kept = played(slot);
    const std::optional<std::set<uint32_t>> unknown = played(-1);
    if (!kept || !unknown)
    {
        return std::nullopt;
    }

    kept->insert(unknown->begin(), unknown->end());

    return kept;
}

// The banks of the slots that `programs` gives ranges as one bank of the Wii. Each bank goes in as WiiBankFrom sends
// it.
WiiBank MergedWiiBank(Song& song, const sar::SoundArchive& archive, std::span<const uint32_t> banks,
                      const WiiPrograms& programs, WaveArchiveWaves& read, bool trim)
{
    MergedBank<ntdlib::rsar::WaveFile> merged = MergedBankOf<ntdlib::rsar::WaveFile>(
        song, archive, banks, programs, read, trim, ntdlib::rsar::WiiWaveOf,
        "A Wii sound can use only one bank because NW4R SND has no bank select. ", LeftWavesWarning);

    return WiiBank{.name = std::move(merged.name),
                   .files = ntdlib::rsar::WiiBankOf(merged.file, kWiiBankVersion, merged.waves).Write()};
}

// The warnings of a console whose sounds play one bank: the banks of a sound's other slots stay behind there, and notes
// may play another instrument. `console` names that console.
void AddOneBankWarnings(Song& song, const sar::SoundArchive& archive, std::span<const uint32_t> banks,
                        const std::optional<WiiPrograms>& programs, bool merged, sar::Flavor from,
                        const std::string& console)
{
    std::vector<std::string> others;
    for (std::size_t slot = 0; slot < banks.size(); ++slot)
    {
        const uint32_t id = banks[slot];
        if (id != sar::kNoItem && (merged ? !programs->slots.contains(static_cast<int>(slot)) : slot > 0))
        {
            others.push_back(BankNameOf(archive, id));
        }
    }
    if (!others.empty())
    {
        const bool one = others.size() == 1;
        std::string stays = (one ? "The bank in the sound's other slot is left out: "
                                 : "The banks in the sound's other slots are left out: ") +
                            ListOf(others) + ".";
        if (!programs)
        {
            stays += " A " + console +
                     " sound can use only one bank. Notes that select another slot use the bank in the first slot. "
                     "Hokuto can't follow the bank selects of the sequence.";
        }
        else if (programs->unknown_slots)
        {
            stays += one ? " Only a variable or a random number selects its slot."
                         : " Only a variable or a random number selects their slots.";
        }
        else
        {
            stays += one ? " None of the notes use it." : " None of the notes use them.";
        }

        song.result.warnings.push_back(stays);
    }

    if (programs && programs->unsure_notes > 0)
    {
        const bool one = programs->unsure_notes == 1;
        song.result.warnings.push_back(
            std::to_string(programs->unsure_notes) +
            (one ? " note command may play a different instrument on the "
                 : " note commands may play different instruments on the ") +
            console + " than on the " + song::ConsoleName(from) +
            (one ? ". Its bank slot or program comes from a variable or a random number, or differs between the "
                   "paths that reach it."
                 : ". Their bank slots or programs come from a variable or a random number, or differ between the "
                   "paths that reach them."));
    }
}

} // namespace other_console_parts

ntdlib::rsar::BankFile WiiBankOfFile(std::span<const uint8_t> bytes, sar::Flavor flavor, uint16_t version,
                                     const std::vector<std::optional<ntdlib::rsar::WaveFile>>& waves)
{
    sar::Bank bank;
    try
    {
        bank = sar::Bank::Parse(bytes, sar::Options{.flavor = flavor});
    }
    catch (const std::exception& error)
    {
        throw std::invalid_argument(std::string("Can't read the bank. ") + format::AsSentence(error.what()));
    }

    return ntdlib::rsar::WiiBankOf(BankFileOfRegions(bank), version, waves);
}

} // namespace hokuto::edit
