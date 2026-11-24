// SPDX-License-Identifier: MIT

#include "song/dse.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <functional>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ntdlib/dse/build.h"
#include "ntdlib/dse/driver.h"
#include "ntdlib/dse/game_starts.h"
#include "ntdlib/dse/layout.h"
#include "ntdlib/dse/oldest.h"
#include "ntdlib/dse/sets.h"
#include "ntdlib/dse/ticks.h"
#include "ntdlib/ntr/rom.h"
#include "ntdlib/sequence_export/dse/convert.h"
#include "ntdlib/sequence_export/dse/instruments.h"
#include "seqlib/file_name.h"
#include "xsflib/binary.h"
#include "xsflib/bytes.h"

#include "format/dse_files.h"
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

// The length of the driver's tick for the file at `path` in the game whose header gives `game_code`
// (ntdlib::dse::TickOf), or the driver's default tick where ntdlib doesn't give one.
int PathTick(std::string_view game_code, std::string_view path)
{
    return ntdlib::dse::TickOf(game_code, path).tick_microseconds.value_or(ntdlib::dse::Driver::kTickMicroseconds);
}

// Where `part` starts in `bytes`, or 0 for a part that lies elsewhere, such as a file that the search decompressed.
uint64_t OffsetIn(std::span<const uint8_t> bytes, std::span<const uint8_t> part)
{
    const std::less_equal<const uint8_t*> at_or_before;
    const bool inside =
        at_or_before(bytes.data(), part.data()) && at_or_before(part.data() + part.size(), bytes.data() + bytes.size());

    return inside ? static_cast<uint64_t>(part.data() - bytes.data()) : 0;
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

// Adds to `set` each wave bank of `files`, with the bank that a sound uses in place of an equal one, and the tick of
// its folder in the game of `game_code`. Returns the folder of each bank.
std::vector<std::string_view> AddWaveBanks(
    DseSet& set, std::span<const ntdlib::dse::NamedFile> files, std::span<const uint8_t> bytes,
    std::map<uint16_t, std::vector<std::shared_ptr<const ntdlib::dse::WaveBank>>>& played, std::string_view game_code)
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

        const std::string_view folder = FolderOf(file.path);
        set.banks.push_back(
            {.bank = std::move(bank),
             .file_offset = OffsetIn(bytes, file.bytes),
             .tick_microseconds = PathTick(game_code, file.path),
             .oldest = ntdlib::dse::IsOldestVersion(file.bytes)
                           ? std::make_shared<const std::vector<uint8_t>>(file.bytes.begin(), file.bytes.end())
                           : nullptr});
        folders.push_back(folder);
    }

    return folders;
}

// `set` with every wave bank of `files`. A bank that a sound uses keeps its copy. A bank whose waves are in a main bank
// gets the main bank of its id: the one in its folder, or else the first in `files`. ntdlib's search for the banks of a
// song does the same.
void ReadBanks(DseSet& set, std::span<const ntdlib::dse::NamedFile> files, std::span<const uint8_t> bytes,
               std::string_view game_code)
{
    std::map<uint16_t, std::vector<std::shared_ptr<const ntdlib::dse::WaveBank>>> played = ShareSameBanks(set);
    const std::vector<std::string_view> folders = AddWaveBanks(set, files, bytes, played, game_code);

    for (std::size_t i = 0; i < set.banks.size(); i++)
    {
        const ntdlib::dse::WaveBank& bank = *set.banks[i].bank;
        if (bank.header[0x4B] != kWavesInMainBank)
        {
            continue;
        }

        const auto id = xsflib::LoadLe16(&bank.header[kMainBankId]);
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

// `set` with the sounds of `files`, where their tracks start, their ticks in the game of `game_code`, and the states
// in which `starts` start them. `bytes` contains the bytes of the files.
void ReadSounds(DseSet& set, std::span<const ntdlib::dse::NamedFile> files, std::span<const uint8_t> bytes,
                std::string_view game_code, std::span<const format::SoundStart> starts)
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
        const bool oldest = file != by_path.end() && !sound.ref.effect && ntdlib::dse::IsOldestVersion(file->second);
        set.oldest_songs.push_back(
            oldest ? std::make_shared<const std::vector<uint8_t>>(file->second.begin(), file->second.end()) : nullptr);
        set.ticks.push_back(PathTick(game_code, sound.ref.path));
    }

    // A sound effect starts at full level.
    set.states.resize(set.sounds.size());
    for (const format::SoundStart& start : starts)
    {
        if (start.name.empty() && start.dse_level && start.sound < set.sounds.size() &&
            !set.sounds[start.sound].ref.effect)
        {
            set.states[start.sound].level = *start.dse_level;
        }
    }

    ReadBanks(set, files, bytes, game_code);
}

} // namespace

std::shared_ptr<const DseSet> LoadDseSet(std::span<const uint8_t> image)
{
    // The image's DSE files, loose and in packs, under the names that ntdlib's converter gives them.
    const ntdlib::ntr::Rom rom = ntdlib::ntr::Rom::Parse(image);
    std::vector<std::vector<uint8_t>> storage;
    std::vector<ntdlib::dse::NamedFile> files;
    for (format::DseImageFile& found : format::DseFilesOf(rom, storage))
    {
        files.push_back(std::move(found.named));
    }

    auto set = std::make_shared<DseSet>();
    set->build = ntdlib::dse::BuildOf(rom).value_or(ntdlib::dse::Build::kLater);
    ReadSounds(*set, files, image, rom.Header().game_code, format::DseGameStarts(rom, files));
    return set;
}

std::shared_ptr<const DseSet> LoadDseSet(std::span<const uint8_t> set, ntdlib::dse::Build build,
                                         std::string_view game_code, std::span<const format::SoundStart> starts)
{
    auto loaded = std::make_shared<DseSet>();
    loaded->build = build;
    ReadSounds(*loaded, ntdlib::dse::ReadSet(set), set, game_code, starts);
    return loaded;
}

ntdlib::dse::Build DseBuildOf(std::span<const uint8_t> image)
{
    try
    {
        return ntdlib::dse::BuildOf(ntdlib::ntr::Rom::Parse(image)).value_or(ntdlib::dse::Build::kLater);
    }
    catch (const xsflib::FormatError&)
    {
        return ntdlib::dse::Build::kLater;
    }
}

std::string DseGameCodeOf(std::span<const uint8_t> image)
{
    try
    {
        return ntdlib::ntr::Rom::Parse(image).Header().game_code;
    }
    catch (const xsflib::FormatError&)
    {
        return {};
    }
}

std::vector<std::string> DseOpenNotes(std::span<const uint8_t> image, const DseSet& set)
{
    if (set.sounds.empty())
    {
        return {};
    }

    const ntdlib::ntr::Rom rom = ntdlib::ntr::Rom::Parse(image);
    std::vector<std::string> notes;
    if (!ntdlib::dse::BuildOf(rom))
    {
        notes.push_back("No model covers this game's build of DSE. Its sounds play on the model of the later build.");
    }
    if (const std::optional<std::string> note = format::DseLevelsNote(rom))
    {
        notes.push_back(*note);
    }

    // The tick: a game that ntdlib's table doesn't list, or the folders of a listed game that none of its rows covers.
    const std::string game_code = rom.Header().game_code;
    bool unlisted = false;
    std::vector<std::string> folders;
    for (const ntdlib::sequence_export::dse::SequenceInput& sound : set.sounds)
    {
        const ntdlib::dse::GameTick tick = ntdlib::dse::TickOf(game_code, sound.ref.path);
        unlisted = unlisted || !tick.listed;
        const std::string folder = sound.folder.empty() ? "/" : sound.folder;
        if (tick.listed && !tick.tick_microseconds && std::ranges::find(folders, folder) == folders.end())
        {
            folders.push_back(folder);
        }
    }

    if (unlisted)
    {
        notes.push_back(
            "This game's DSE tick is unknown. Its sounds play with a tick of 10 ms, and their "
            "timing, envelopes and LFOs may differ from the game.");
    }
    if (!folders.empty())
    {
        std::string list;
        for (const std::string& folder : folders)
        {
            list += (list.empty() ? "" : ", ") + folder;
        }
        notes.push_back("The DSE tick of these folders is unknown: " + list +
                        ". Their sounds play with a tick of 10 ms, and their timing, envelopes and LFOs may differ "
                        "from the game.");
    }

    return notes;
}

int DseSoundTick(const DseSet& set, uint32_t sound)
{
    return sound < set.ticks.size() ? set.ticks[sound] : ntdlib::dse::Driver::kTickMicroseconds;
}

ntdlib::dse::SongState DseSoundState(const DseSet& set, uint32_t sound)
{
    return sound < set.states.size() ? set.states[sound] : ntdlib::dse::SongState{};
}

ntdlib::dse::StartParameters DseSoundStart(const DseSet& set, uint32_t sound)
{
    return ntdlib::dse::StartOf(DseSoundState(set, sound));
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

        const std::shared_ptr<const std::vector<uint8_t>>& oldest = set.oldest_songs[item.index];
        return oldest ? *oldest : set.sounds[item.index].song->Write();
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

    const DseBank& bank = set.banks[item.index];
    return bank.oldest ? *bank.oldest : bank.bank->Write();
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

    return ntdlib::sequence_export::dse::ConvertBank(*found.bank, found.main_bank.get(), programs,
                                                     found.tick_microseconds, name, "Hokuto", set.build);
}

} // namespace hokuto::song
