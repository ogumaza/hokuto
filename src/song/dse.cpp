// SPDX-License-Identifier: MIT

#include "song/dse.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ntdlib/dse/layout.h"
#include "ntdlib/dse/sets.h"
#include "ntdlib/ntr/rom.h"
#include "ntdlib/sequence_export/dse/convert.h"
#include "ntdlib/sequence_export/dse/instruments.h"
#include "seqlib/file_name.h"
#include "xsflib/binary.h"

#include "song/midi_export.h"

namespace hokuto::song
{

namespace
{

// The values of byte 0x4B of a bank's header (ntdlib/dse/sets.h), and the offset at which a bank whose waves are in a
// main bank stores the id of the main bank.
constexpr uint8_t kMainBank = 1;
constexpr uint8_t kWavesInMainBank = 2;
constexpr std::size_t kMainBankId = 0x40;

std::string_view FolderOf(std::string_view path)
{
    const std::size_t slash = path.rfind('/');
    return slash == std::string_view::npos ? std::string_view() : path.substr(0, slash + 1);
}

// Sounds whose banks hold the same bytes share one bank. An edit of the bank then reaches each of them.
std::map<uint16_t, std::vector<std::shared_ptr<const ntdlib::dse::WaveBank>>> ShareSameBanks(DseSet& set)
{
    std::map<uint16_t, std::vector<std::shared_ptr<const ntdlib::dse::WaveBank>>> played;
    const auto share_played = [&played](std::shared_ptr<const ntdlib::dse::WaveBank>& bank)
    {
        if (!bank)
        {
            return;
        }

        std::vector<std::shared_ptr<const ntdlib::dse::WaveBank>>& same = played[DseBankId(*bank)];
        const auto equal = [&bank](const std::shared_ptr<const ntdlib::dse::WaveBank>& other)
        {
            return other == bank || *other == *bank;
        };
        const auto found = std::ranges::find_if(same, equal);
        if (found == same.end())
        {
            same.push_back(bank);
        }
        else
        {
            bank = *found;
        }
    };
    for (ntdlib::sequence_export::dse::SequenceInput& sound : set.sounds)
    {
        std::ranges::for_each(sound.banks, share_played);
        share_played(sound.main_bank);
    }

    return played;
}

// Adds to `set` each wave bank of `files`, with the bank that a sound uses in place of an equal one. Returns the folder
// of each bank.
std::vector<std::string_view> AddWaveBanks(
    DseSet& set, std::span<const ntdlib::dse::NamedFile> files, std::span<const uint8_t> bytes,
    std::map<uint16_t, std::vector<std::shared_ptr<const ntdlib::dse::WaveBank>>>& played)
{
    std::vector<std::string_view> folders;
    for (const ntdlib::dse::NamedFile& file : files)
    {
        if (file.bytes.size() < 4 || std::memcmp(file.bytes.data(), "swdl", 4) != 0)
        {
            continue;
        }

        std::shared_ptr<const ntdlib::dse::WaveBank> bank;
        try
        {
            bank = std::make_shared<const ntdlib::dse::WaveBank>(ntdlib::dse::WaveBank::Parse(file.bytes));
        }
        catch (const xsflib::FormatError& error)
        {
            set.warnings.push_back(file.path + ": can't read the wave bank (" + error.what() + ")");
            continue;
        }

        for (const std::shared_ptr<const ntdlib::dse::WaveBank>& same : played[DseBankId(*bank)])
        {
            if (*same == *bank)
            {
                bank = same;
                break;
            }
        }

        set.banks.push_back(
            {.bank = std::move(bank), .file_offset = static_cast<uint64_t>(file.bytes.data() - bytes.data())});
        folders.push_back(FolderOf(file.path));
    }

    return folders;
}

// `set` with every wave bank of `files`. A bank that a sound uses keeps its copy. A bank whose waves are in a main bank
// gets the main bank of its id: the one in its folder, or else the first in `files`. ntdlib's search for the banks of a
// song does the same.
void ReadBanks(DseSet& set, std::span<const ntdlib::dse::NamedFile> files, std::span<const uint8_t> bytes)
{
    std::map<uint16_t, std::vector<std::shared_ptr<const ntdlib::dse::WaveBank>>> played = ShareSameBanks(set);
    const std::vector<std::string_view> folders = AddWaveBanks(set, files, bytes, played);

    for (std::size_t i = 0; i < set.banks.size(); i++)
    {
        const ntdlib::dse::WaveBank& bank = *set.banks[i].bank;
        if (bank.header[0x4B] != kWavesInMainBank)
        {
            continue;
        }

        const auto id = static_cast<uint16_t>(bank.header[kMainBankId] | bank.header[kMainBankId + 1] << 8);
        for (std::size_t other = 0; other < set.banks.size(); other++)
        {
            const ntdlib::dse::WaveBank& main_bank = *set.banks[other].bank;
            if (main_bank.header[0x4B] != kMainBank || DseBankId(main_bank) != id)
            {
                continue;
            }

            if (!set.banks[i].main_bank || folders[other] == folders[i])
            {
                set.banks[i].main_bank = set.banks[other].bank;
            }

            if (folders[other] == folders[i])
            {
                break;
            }
        }
    }
}

// `set` with the sounds of `files`, and where their tracks start. `bytes` contains the bytes of the files.
void ReadSounds(DseSet& set, std::span<const ntdlib::dse::NamedFile> files, std::span<const uint8_t> bytes)
{
    set.sounds = ntdlib::sequence_export::dse::ReadSequences(files, set.warnings);

    std::map<std::string_view, std::span<const uint8_t>> by_path;
    for (const ntdlib::dse::NamedFile& file : files)
    {
        by_path.emplace(file.path, file.bytes);
    }

    for (const ntdlib::sequence_export::dse::SequenceInput& sound : set.sounds)
    {
        const auto file = by_path.find(sound.ref.path);
        const std::optional<uint32_t> offset =
            file == by_path.end() ? std::nullopt : ntdlib::dse::TracksOffset(file->second, sound.ref.effect);
        set.tracks_offsets.push_back(offset.value_or(0));
    }

    ReadBanks(set, files, bytes);
}

} // namespace

std::shared_ptr<const DseSet> LoadDseSet(std::span<const uint8_t> image)
{
    // The image's files under the names that ntdlib's converter gives them: their paths, or their numbers for files
    // without a path.
    const ntdlib::ntr::Rom rom = ntdlib::ntr::Rom::Parse(image);
    std::vector<ntdlib::dse::NamedFile> files;
    for (const ntdlib::ntr::RomFile& file : rom.Files())
    {
        files.push_back({file.path.empty() ? "file " + std::to_string(file.id) : file.path, rom.FileData(file.id)});
    }

    auto set = std::make_shared<DseSet>();
    set->build = ntdlib::dse::BuildOf(rom.Arm9());
    ReadSounds(*set, files, image);
    return set;
}

std::shared_ptr<const DseSet> LoadDseSet(std::span<const uint8_t> set, ntdlib::dse::Build build)
{
    auto loaded = std::make_shared<DseSet>();
    loaded->build = build;
    ReadSounds(*loaded, ntdlib::dse::ReadSet(set), set);
    return loaded;
}

ntdlib::dse::Build DseBuildOf(std::span<const uint8_t> image)
{
    try
    {
        return ntdlib::dse::BuildOf(ntdlib::ntr::Rom::Parse(image).Arm9());
    }
    catch (const xsflib::FormatError&)
    {
        return ntdlib::dse::Build::kLater;
    }
}

std::vector<uint32_t> DseSounds(const DseSet& set)
{
    std::vector<uint32_t> sounds;
    for (std::size_t i = 0; i < set.sounds.size(); i++)
    {
        sounds.push_back(static_cast<uint32_t>(i));
    }

    return sounds;
}

std::string DseSoundName(const DseSet& set, uint32_t sound)
{
    return sound < set.sounds.size() ? set.sounds[sound].name : "Sound " + std::to_string(sound);
}

std::string DseSoundFolder(const DseSet& set, uint32_t sound)
{
    return sound < set.sounds.size() ? set.sounds[sound].folder : std::string();
}

std::vector<ntdlib::dse::SequenceRef> DseSoundRefs(const DseSet& set)
{
    std::vector<ntdlib::dse::SequenceRef> refs;
    for (const ntdlib::sequence_export::dse::SequenceInput& sound : set.sounds)
    {
        refs.push_back(sound.ref);
    }

    return refs;
}

uint16_t DseBankId(const ntdlib::dse::WaveBank& bank)
{
    return static_cast<uint16_t>(bank.header[0x0E] | bank.header[0x0F] << 8);
}

std::string DseBankName(const ntdlib::dse::WaveBank& bank)
{
    std::string name;
    for (std::size_t i = 0x20; i < 0x30 && bank.header[i] != 0; i++)
    {
        name += static_cast<char>(bank.header[i]);
    }

    // The name is the name of the file, with what the 16 bytes of the field keep of its extension.
    if (const std::size_t dot = name.rfind('.'); dot != std::string::npos && dot > 0)
    {
        name.resize(dot);
    }

    return !name.empty() ? name : "BANK_" + std::to_string(bank.header[0x0E] | bank.header[0x0F] << 8);
}

std::vector<uint32_t> DseBankPrograms(const ntdlib::dse::WaveBank& bank)
{
    std::vector<uint32_t> programs;
    if (bank.instruments)
    {
        for (std::size_t program = 0; program < bank.instruments->programs.size(); program++)
        {
            if (bank.instruments->programs[program])
            {
                programs.push_back(static_cast<uint32_t>(program));
            }
        }
    }

    return programs;
}

std::vector<uint32_t> DseProgramBanks(const DseSet& set)
{
    std::vector<uint32_t> banks;
    for (std::size_t i = 0; i < set.banks.size(); i++)
    {
        if (set.banks[i].bank->instruments)
        {
            banks.push_back(static_cast<uint32_t>(i));
        }
    }

    return banks;
}

std::vector<uint32_t> DseSampleBanks(const DseSet& set)
{
    std::vector<uint32_t> banks;
    for (std::size_t i = 0; i < set.banks.size(); i++)
    {
        if (set.banks[i].bank->samples)
        {
            banks.push_back(static_cast<uint32_t>(i));
        }
    }

    return banks;
}

std::vector<uint32_t> DseBankWaves(const ntdlib::dse::WaveBank& bank)
{
    std::vector<uint32_t> waves;
    for (std::size_t i = 0; i < bank.waves.size(); i++)
    {
        if (bank.waves[i])
        {
            waves.push_back(static_cast<uint32_t>(i));
        }
    }

    return waves;
}

ntdlib::wave::Pcm DseWavePcm(const DseSet& set, uint32_t bank, uint32_t wave)
{
    const std::string name = DseItemName(set, {.kind = ItemKind::kWave, .index = bank, .wave = wave});
    if (bank >= set.banks.size())
    {
        throw std::runtime_error("The image has no " + name + ".");
    }

    const DseBank& found = set.banks[bank];
    ntdlib::sequence_export::dse::BankSet banks({.banks = {found.bank.get()}, .main_bank = found.main_bank.get()});
    const ntdlib::sequence_export::dse::Pcm* decoded =
        wave <= 0xFFFF ? banks.Wave(DseBankId(*found.bank), static_cast<uint16_t>(wave)) : nullptr;
    if (decoded == nullptr)
    {
        throw std::runtime_error("Can't decode " + name + ".");
    }

    ntdlib::wave::Pcm pcm;
    pcm.sample_rate = decoded->sample_rate;
    pcm.loop = decoded->loop;
    pcm.loop_start = decoded->loop_start;
    pcm.loop_end = decoded->loop_end;
    pcm.channels = decoded->channels;

    return pcm;
}

std::string DseItemName(const DseSet& set, const Item& item)
{
    switch (item.kind)
    {
    case ItemKind::kBank:
    case ItemKind::kWaveArchive:
    case ItemKind::kWave:
        {
            const std::string bank = item.index < set.banks.size() ? DseBankName(*set.banks[item.index].bank)
                                                                   : "bank " + std::to_string(item.index);
            return item.kind == ItemKind::kWave ? bank + ", wave " + std::to_string(item.wave) : bank;
        }

    default:
        return DseSoundName(set, item.index);
    }
}

std::string DseItemFileName(const DseSet& set, const Item& item, std::size_t waves)
{
    if (item.kind == ItemKind::kBank || item.kind == ItemKind::kWaveArchive)
    {
        return seqlib::SafeFileName(DseItemName(set, item)) + ".swd";
    }
    if (item.kind != ItemKind::kWave)
    {
        return seqlib::SafeFileName(DseSoundName(set, item.index)) + ".smd";
    }

    // The slot has as many digits as the last slot of the bank.
    const Item bank{.kind = ItemKind::kWaveArchive, .index = item.index};
    return seqlib::SafeFileName(DseItemName(set, bank) + "_" + PaddedIndex(item.wave, waves)) + ".wav";
}

std::vector<uint8_t> DseItemBytes(const DseSet& set, const Item& item)
{
    const std::string name = DseItemName(set, item);
    if (item.kind == ItemKind::kSequence)
    {
        if (item.index >= set.sounds.size())
        {
            throw std::runtime_error("The image has no " + name + ".");
        }
        if (!set.sounds[item.index].song)
        {
            throw std::runtime_error(name + " is a sound effect. Its file holds the other sound effects of its bank.");
        }

        return set.sounds[item.index].song->Write();
    }

    if (item.index >= set.banks.size())
    {
        throw std::runtime_error("The image has no " + name + ".");
    }
    if (item.kind == ItemKind::kWave)
    {
        return PcmAsWav(DseWavePcm(set, item.index, item.wave), name);
    }
    if (item.kind != ItemKind::kBank && item.kind != ItemKind::kWaveArchive)
    {
        throw std::runtime_error(name + " has no file to export.");
    }

    return set.banks[item.index].bank->Write();
}

ntdlib::sequence_export::BankConversion ConvertDseBank(const DseSet& set, uint32_t bank,
                                                       std::span<const uint32_t> programs)
{
    if (bank >= set.banks.size())
    {
        throw std::runtime_error("The image has no bank " + std::to_string(bank) + ".");
    }

    const DseBank& found = set.banks[bank];
    const std::string name = DseBankName(*found.bank);
    if (programs.empty())
    {
        throw BankWithoutInstruments(name + " has no instruments.");
    }

    return ntdlib::sequence_export::dse::ConvertBank(*found.bank, found.main_bank.get(), programs, name, "Hokuto");
}

} // namespace hokuto::song
