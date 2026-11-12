// SPDX-License-Identifier: MIT

// The items of the Wii's archives, and the Wii's files of the later consoles' waves (song/items.h).

#include <cmath>
#include <cstring>
#include <exception>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>

#include "ntdlib/rsar/bank_file.h"
#include "ntdlib/rsar/convert.h"
#include "ntdlib/rsar/decode.h"
#include "ntdlib/rsar/formats.h"
#include "ntdlib/rsar/wave_file.h"
#include "ntdlib/rsar/wsd_file.h"
#include "ntdlib/rvl/snd/rsar_options.h"
#include "ntdlib/sar/wave_file.h"
#include "seqlib/file_name.h"
#include "xsflib/ascii.h"

#include "song/items.h"
#include "song/items_parts.h"
#include "song/streams.h"

namespace hokuto::song
{
namespace
{

namespace sar = ntdlib::sar;

using items_parts::AtPitch;
using items_parts::kCutShort;

} // namespace

std::vector<uint8_t> WiiWaveFileOf(const sar::SoundArchive& archive, const Item& item)
{
    if (item.kind != ItemKind::kWave && item.kind != ItemKind::kWaveArchive)
    {
        throw std::invalid_argument("WiiWaveFileOf converts waves and wave archives.");
    }

    const std::span<const uint8_t> file = ItemFile(archive, item);
    try
    {
        return item.kind == ItemKind::kWave ? ntdlib::rsar::WiiWaveOf(sar::WaveFile::Read(file)).Write()
                                            : ntdlib::rsar::WiiWaveArchiveOf(sar::WaveArchiveFile::Read(file)).Write();
    }
    catch (const std::exception& error)
    {
        throw std::runtime_error("Can't convert the file of " + ItemName(archive, item) + " for the Wii. " +
                                 error.what());
    }
}

namespace
{

namespace rsar = ntdlib::rsar;

// The sound of `item` of the Wii's `archive`: a sequence sound, a wave sound or a stream sound. Throws
// std::runtime_error when the archive lacks it.
const rsar::SoundInfo& SoundOf(const rsar::SoundArchive& archive, const Item& item)
{
    const rsar::SoundType type = item.kind == ItemKind::kSequence ? rsar::SoundType::kSequence
                                 : item.kind == ItemKind::kStream ? rsar::SoundType::kStream
                                                                  : rsar::SoundType::kWaveSound;
    if (item.index >= archive.Sounds().size() || archive.Sounds()[item.index].type != type)
    {
        const char* kind = type == rsar::SoundType::kSequence ? "sequence sound "
                           : type == rsar::SoundType::kStream ? "stream sound "
                                                              : "wave sound ";
        throw std::runtime_error(std::string("The archive has no ") + kind + std::to_string(item.index) + ".");
    }

    return archive.Sounds()[item.index];
}

// The wave sound file of `item`, a wave sound file given by its first wave sound, among WaveSoundSets. Throws
// std::runtime_error when the archive lacks the wave sound. WaveSoundSets lists the file of every wave sound.
WaveSoundSet SetOf(const rsar::SoundArchive& archive, const Item& item)
{
    const uint32_t file_id = SoundOf(archive, item).file_id;
    for (WaveSoundSet& set : WaveSoundSets(archive))
    {
        if (set.file_id == file_id)
        {
            return set;
        }
    }

    throw std::logic_error("WaveSoundSets lists the file of every wave sound");
}

// The wave archive of file `file_id` among WaveArchivesOf. Throws std::runtime_error when the archive has none.
WiiWaveArchive WaveArchiveOf(const rsar::SoundArchive& archive, uint32_t file_id)
{
    for (WiiWaveArchive& wave_archive : WaveArchivesOf(archive))
    {
        if (wave_archive.file_id == file_id)
        {
            return wave_archive;
        }
    }

    throw std::runtime_error("The archive has no waves for file " + std::to_string(file_id) + ".");
}

// The RWAV files of the RWAR `wave_archive`. Throws std::runtime_error when the RWAR can't be read.
std::vector<std::span<const uint8_t>> RwavsOf(const rsar::SoundArchive& archive, const WiiWaveArchive& wave_archive)
{
    try
    {
        return rsar::WaveArchive::Parse(archive.WaveData(wave_archive.file_id)).waves;
    }
    catch (const std::exception& error)
    {
        throw std::runtime_error("Can't read the waves of " + wave_archive.name + ". " + error.what());
    }
}

// The waves that the WAVE block of the bank or the wave sound file of `wave_archive` describes in its wave data, read
// with `options`. Throws std::runtime_error when the file or its waves can't be read.
std::vector<rsar::Wave> BlockWavesOf(const rsar::SoundArchive& archive, const WiiWaveArchive& wave_archive,
                                     const rsar::ReadOptions& options = {})
{
    const std::span<const uint8_t> file = archive.FileData(wave_archive.file_id);
    const std::span<const uint8_t> wave_data = archive.WaveData(wave_archive.file_id);
    try
    {
        if (file.size() >= 4 && std::memcmp(file.data(), "RWSD", 4) == 0)
        {
            return rsar::WaveSoundFile::Parse(file, wave_data, options).Waves();
        }

        return rsar::Bank::Parse(file, wave_data, options).Waves();
    }
    catch (const std::exception& error)
    {
        throw std::runtime_error("Can't read the waves of " + wave_archive.name + ". " + error.what());
    }
}

// Wave `wave` of `wave_archive` as ntdlib reads it. Throws std::runtime_error when the wave archive lacks the wave, and
// when the wave can't be read.
rsar::Wave WaveOf(const rsar::SoundArchive& archive, const WiiWaveArchive& wave_archive, uint32_t wave)
{
    // The wave data is read once, for the count and for the wave.
    std::vector<std::span<const uint8_t>> rwavs;
    std::vector<rsar::Wave> block_waves;
    if (wave_archive.rwar)
    {
        rwavs = RwavsOf(archive, wave_archive);
    }
    else
    {
        block_waves = BlockWavesOf(archive, wave_archive);
    }
    if (wave >= (wave_archive.rwar ? rwavs.size() : block_waves.size()))
    {
        throw std::runtime_error(wave_archive.name + " has no wave " + std::to_string(wave) + ".");
    }
    if (!wave_archive.rwar)
    {
        return std::move(block_waves[wave]);
    }

    try
    {
        return rsar::Wave::ParseFile(rwavs[wave]);
    }
    catch (const std::exception& error)
    {
        throw std::runtime_error("Can't read wave " + std::to_string(wave) + " of " + wave_archive.name + ". " +
                                 error.what());
    }
}

} // namespace

std::vector<uint32_t> ProgramsOf(const ntdlib::rsar::SoundArchive& archive, std::size_t bank)
{
    std::vector<uint32_t> programs;
    const std::span<const uint8_t> file =
        bank < archive.Banks().size() ? archive.FileData(archive.Banks()[bank].file_id) : std::span<const uint8_t>();
    if (file.empty())
    {
        return programs;
    }

    const ntdlib::rsar::Bank parsed = ntdlib::rsar::Bank::Parse(file);
    for (uint32_t program = 0; program < parsed.InstrumentCount(); program++)
    {
        if (parsed.Plays(program))
        {
            programs.push_back(program);
        }
    }

    return programs;
}

std::vector<WiiWaveArchive> WaveArchivesOf(const rsar::SoundArchive& archive)
{
    // A bank's or a wave sound file's wave data is an RWAR, or the samples that the file's WAVE block describes.
    std::vector<WiiWaveArchive> wave_archives;
    std::set<uint32_t> files;
    const auto add = [&](uint32_t file_id, const std::string& name)
    {
        const std::span<const uint8_t> wave_data = archive.WaveData(file_id);
        const bool rwar = wave_data.size() >= 4 && std::memcmp(wave_data.data(), "RWAR", 4) == 0;
        if (archive.FileData(file_id).empty() || wave_data.empty() || files.contains(file_id))
        {
            return;
        }

        files.insert(file_id);
        wave_archives.push_back({file_id, name, rwar});
    };
    for (const rsar::BankInfo& bank : archive.Banks())
    {
        add(bank.file_id, bank.name);
    }

    for (const WaveSoundSet& set : WaveSoundSets(archive))
    {
        add(set.file_id, set.name);
    }

    return wave_archives;
}

std::vector<WaveSoundSet> WaveSoundSets(const rsar::SoundArchive& archive)
{
    std::vector<WaveSoundSet> sets;
    std::map<uint32_t, std::size_t> places;
    for (uint32_t i = 0; i < archive.Sounds().size(); i++)
    {
        const rsar::SoundInfo& sound = archive.Sounds()[i];
        if (sound.type != rsar::SoundType::kWaveSound)
        {
            continue;
        }

        const auto [place, added] = places.emplace(sound.file_id, sets.size());
        if (added)
        {
            sets.push_back({sound.file_id, "WSD_FILE_" + std::to_string(sound.file_id), {}});
        }

        sets[place->second].sounds.push_back(i);
    }

    return sets;
}

std::size_t WaveCountOf(const rsar::SoundArchive& archive, uint32_t file_id)
{
    const WiiWaveArchive wave_archive = WaveArchiveOf(archive, file_id);
    return wave_archive.rwar ? RwavsOf(archive, wave_archive).size() : BlockWavesOf(archive, wave_archive).size();
}

std::span<const uint8_t> ItemFile(const rsar::SoundArchive& archive, const Item& item)
{
    // The item's file ID and name. A wave's file sits inside its wave archive's RWAR.
    uint32_t file_id = 0;
    std::string name;
    switch (item.kind)
    {
    case ItemKind::kSequence:
    case ItemKind::kWaveSound:
    case ItemKind::kWaveSoundFile:
        {
            const rsar::SoundInfo& sound = SoundOf(archive, item);
            file_id = sound.file_id;
            name = sound.name;
            break;
        }

    case ItemKind::kBank:
        if (item.index >= archive.Banks().size())
        {
            throw std::runtime_error("The archive has no bank " + std::to_string(item.index) + ".");
        }

        file_id = archive.Banks()[item.index].file_id;
        name = archive.Banks()[item.index].name;
        break;

    case ItemKind::kWaveArchive:
    case ItemKind::kWave:
        {
            const WiiWaveArchive wave_archive = WaveArchiveOf(archive, item.index);
            if (!wave_archive.rwar)
            {
                throw std::runtime_error("The waves of " + wave_archive.name +
                                         " aren't a separate file. A WAVE block in the file that contains them "
                                         "describes them.");
            }
            if (item.kind == ItemKind::kWaveArchive)
            {
                return archive.WaveData(wave_archive.file_id);
            }

            const std::vector<std::span<const uint8_t>> waves = RwavsOf(archive, wave_archive);
            if (item.wave >= waves.size())
            {
                throw std::runtime_error(wave_archive.name + " has no wave " + std::to_string(item.wave) + ".");
            }

            return waves[item.wave];
        }

    case ItemKind::kStream:
        throw std::runtime_error("The stream of " + SoundOf(archive, item).name + " is stored outside the archive.");
    }

    const std::span<const uint8_t> file = archive.FileData(file_id);
    if (file.empty())
    {
        throw std::runtime_error(archive.FileMissing(file_id) ? CutShortNote(name)
                                                              : "The archive has no file for " + name + ".");
    }

    return file;
}

std::string WiiMissingNote(const rsar::SoundArchive& archive, uint32_t file_id, const std::string& missing)
{
    const bool cut_short = archive.FileMissing(file_id) || archive.WaveDataMissing(file_id);

    return cut_short ? std::string(kCutShort) + missing : missing;
}

std::vector<uint8_t> ItemBytes(const rsar::SoundArchive& archive, const Item& item)
{
    if (item.kind != ItemKind::kWaveArchive && item.kind != ItemKind::kWave)
    {
        const std::span<const uint8_t> file = ItemFile(archive, item);
        return {file.begin(), file.end()};
    }

    const WiiWaveArchive wave_archive = WaveArchiveOf(archive, item.index);
    if (wave_archive.rwar)
    {
        const std::span<const uint8_t> file = ItemFile(archive, item);
        return {file.begin(), file.end()};
    }

    // The waves of the model of the bank or the wave sound file, as RWAV files.
    std::vector<rsar::WaveFile> waves;
    try
    {
        const std::optional<rsar::FileData> data = archive.File(wave_archive.file_id);
        if (!data)
        {
            throw std::runtime_error(
                WiiMissingNote(archive, wave_archive.file_id, "The file is missing from the archive."));
        }

        waves = data->file.size() >= 4 && std::memcmp(data->file.data(), "RWSD", 4) == 0
                    ? rsar::WsdFile::Read(data->file, data->wave_data).waves
                    : rsar::BankFile::Read(data->file, data->wave_data).waves;
    }
    catch (const std::exception& error)
    {
        throw std::runtime_error("Can't read the waves of " + wave_archive.name + ". " + error.what());
    }

    if (item.kind == ItemKind::kWave && item.wave >= waves.size())
    {
        throw std::runtime_error(wave_archive.name + " has no wave " + std::to_string(item.wave) + ".");
    }

    try
    {
        if (item.kind == ItemKind::kWave)
        {
            return waves[item.wave].Write();
        }

        rsar::WaveArchiveFile rwar;
        for (const rsar::WaveFile& wave : waves)
        {
            rwar.waves.push_back(wave.Write());
        }

        return rwar.Write();
    }
    catch (const std::invalid_argument& error)
    {
        throw std::runtime_error("Can't write the waves of " + wave_archive.name + ". " + error.what());
    }
}

std::string WiiItemFormat(ItemKind kind)
{
    switch (kind)
    {
    case ItemKind::kSequence:
        return "BRSEQ";
    case ItemKind::kWaveSound:
    case ItemKind::kWaveSoundFile:
        return "BRWSD";
    case ItemKind::kBank:
        return "BRBNK";
    case ItemKind::kWaveArchive:
        return "BRWAR";
    case ItemKind::kWave:
        return "BRWAV";
    case ItemKind::kStream:
        return "BRSTM";
    }

    return "BR";
}

std::string ItemFileName(const rsar::SoundArchive& archive, const Item& item, std::size_t waves)
{
    std::string name;
    switch (item.kind)
    {
    case ItemKind::kSequence:
    case ItemKind::kWaveSound:
        name = SoundOf(archive, item).name;
        break;

    // A stream takes the name of its file beside the archive.
    case ItemKind::kStream:
        SoundOf(archive, item);
        return StreamFileName(archive, item.index);

    case ItemKind::kWaveSoundFile:
        name = SetOf(archive, item).name;
        break;

    case ItemKind::kBank:
        name = archive.Banks().at(item.index).name;
        break;

    case ItemKind::kWaveArchive:
        name = WaveArchiveOf(archive, item.index).name;
        break;

    // The index takes as many digits as the wave archive's last index.
    case ItemKind::kWave:
        name = WaveArchiveOf(archive, item.index).name + "_" + PaddedIndex(item.wave, waves);
        break;
    }

    return seqlib::SafeFileName(name) + "." + xsflib::AsciiLower(WiiItemFormat(item.kind));
}

std::string WavFileName(const rsar::SoundArchive& archive, uint32_t file_id, uint32_t wave, std::size_t waves)
{
    std::string name = ItemFileName(archive, {ItemKind::kWave, file_id, wave}, waves);
    name.erase(name.rfind('.'));

    return name + ".wav";
}

std::string ItemName(const rsar::SoundArchive& archive, const Item& item)
{
    switch (item.kind)
    {
    case ItemKind::kSequence:
    case ItemKind::kWaveSound:
    case ItemKind::kStream:
        return SoundOf(archive, item).name;

    case ItemKind::kWaveSoundFile:
        return SetOf(archive, item).name;

    case ItemKind::kBank:
        return archive.Banks().at(item.index).name;

    case ItemKind::kWaveArchive:
        return WaveArchiveOf(archive, item.index).name;

    case ItemKind::kWave:
        return WaveArchiveOf(archive, item.index).name + ", wave " + std::to_string(item.wave);
    }

    return {};
}

std::vector<uint8_t> LaterWaveFileOf(const rsar::SoundArchive& archive, const Item& item, sar::Flavor to)
{
    if (item.kind != ItemKind::kWave && item.kind != ItemKind::kWaveArchive)
    {
        throw std::invalid_argument("LaterWaveFileOf converts waves and wave archives.");
    }

    // The waves come with the state at their loops' starts. The later consoles' files hold that state.
    const WiiWaveArchive wave_archive = WaveArchiveOf(archive, item.index);
    std::vector<rsar::Wave> waves;
    if (!wave_archive.rwar)
    {
        waves = BlockWavesOf(archive, wave_archive, ntdlib::rvl::snd::kRsarOptions);
    }
    else
    {
        const std::vector<std::span<const uint8_t>> rwavs = RwavsOf(archive, wave_archive);
        for (std::size_t wave = 0; wave < rwavs.size(); ++wave)
        {
            try
            {
                waves.push_back(rsar::Wave::ParseFile(rwavs[wave], ntdlib::rvl::snd::kRsarOptions));
            }
            catch (const std::exception& error)
            {
                throw std::runtime_error("Can't read wave " + std::to_string(wave) + " of " + wave_archive.name + ". " +
                                         error.what());
            }
        }
    }
    if (item.kind == ItemKind::kWave && item.wave >= waves.size())
    {
        throw std::runtime_error(wave_archive.name + " has no wave " + std::to_string(item.wave) + ".");
    }

    const FileVersions versions = ExportVersions(ItemKind::kWaveArchive, to);
    const auto later = [&](std::size_t wave)
    {
        try
        {
            return rsar::WaveFileOf(waves[wave], to, versions.waves).Write();
        }
        catch (const std::exception& error)
        {
            throw std::runtime_error("Can't convert wave " + std::to_string(wave) + " of " + wave_archive.name + ". " +
                                     error.what());
        }
    };
    if (item.kind == ItemKind::kWave)
    {
        return later(item.wave);
    }

    sar::WaveArchiveFile file{.flavor = to, .version = versions.file, .waves = {}};
    for (std::size_t wave = 0; wave < waves.size(); ++wave)
    {
        file.waves.push_back(later(wave));
    }

    return file.Write();
}

rsar::Pcm DecodeItemWave(const rsar::SoundArchive& archive, uint32_t file_id, uint32_t wave)
{
    const WiiWaveArchive wave_archive = WaveArchiveOf(archive, file_id);
    const rsar::Wave parsed = WaveOf(archive, wave_archive, wave);
    try
    {
        return rsar::DecodeWave(parsed);
    }
    catch (const std::exception& error)
    {
        throw std::runtime_error("Can't decode wave " + std::to_string(wave) + " of " + wave_archive.name + ". " +
                                 error.what());
    }
}

std::vector<uint8_t> WaveAsWav(const rsar::SoundArchive& archive, uint32_t file_id, uint32_t wave)
{
    // The decode checks the indices before the name reads them.
    rsar::Pcm decoded = DecodeItemWave(archive, file_id, wave);
    return PcmAsWav(std::move(decoded), "wave " + std::to_string(wave) + " of " + WaveArchiveOf(archive, file_id).name);
}

std::size_t WaveChannels(const rsar::SoundArchive& archive, uint32_t file_id, uint32_t wave)
{
    return WaveOf(archive, WaveArchiveOf(archive, file_id), wave).channels.size();
}

WaveSoundNote WaveSoundNoteOf(const rsar::SoundArchive& archive, uint32_t sound)
{
    // The sound's place among the wave sounds of its file. The file's wave data holds the waves.
    const std::span<const uint8_t> file = ItemFile(archive, {ItemKind::kWaveSound, sound, 0});
    const rsar::SoundInfo& info = archive.Sounds()[sound];
    rsar::WaveSoundFile parsed;
    try
    {
        parsed = rsar::WaveSoundFile::Parse(file, ntdlib::rvl::snd::kRsarOptions);
    }
    catch (const std::exception& error)
    {
        throw std::runtime_error("Can't read the wave sound file of " + info.name + ". " + error.what());
    }

    if (info.sub_index < 0 || static_cast<std::size_t>(info.sub_index) >= parsed.Sounds().size())
    {
        throw std::runtime_error("The wave sound file of " + info.name + " has no wave sound " +
                                 std::to_string(info.sub_index) + ".");
    }

    // NW4R SND plays note 0, at the sound's pitch.
    const rsar::WaveSoundFile::WaveSound& wave_sound = parsed.Sounds()[static_cast<std::size_t>(info.sub_index)];
    if (wave_sound.notes.empty())
    {
        throw std::runtime_error(info.name + " has no notes.");
    }

    const rsar::WaveSoundFile::Note& first = wave_sound.notes.front();
    if (first.wave_index < 0 || static_cast<std::size_t>(first.wave_index) >= WaveCountOf(archive, info.file_id))
    {
        throw std::runtime_error("The wave sound file of " + info.name + " is missing the wave that the sound uses.");
    }

    WaveSoundNote note;
    note.wave_archive = info.file_id;
    note.wave = static_cast<uint32_t>(first.wave_index);
    note.pitch = wave_sound.pitch;
    note.panned = wave_sound.pan != 64 || wave_sound.surround_pan != 0;
    note.shaped = first.attack < 127 || first.sustain < 127;

    return note;
}

rsar::Pcm DecodeWaveSound(const rsar::SoundArchive& archive, uint32_t sound)
{
    const WaveSoundNote note = WaveSoundNoteOf(archive, sound);
    return AtPitch(DecodeItemWave(archive, note.wave_archive, note.wave), note.pitch, archive.Sounds()[sound].name);
}

std::vector<uint8_t> WaveSoundAsWav(const rsar::SoundArchive& archive, uint32_t sound)
{
    // The decode checks the sound's index. It runs before the name reads the index.
    rsar::Pcm decoded = DecodeWaveSound(archive, sound);
    return PcmAsWav(std::move(decoded), archive.Sounds()[sound].name);
}

std::string WaveSoundWavFileName(const rsar::SoundArchive& archive, uint32_t sound)
{
    std::string name = ItemFileName(archive, {ItemKind::kWaveSound, sound, 0});
    name.erase(name.rfind('.'));

    return name + ".wav";
}

std::vector<uint32_t> SoundsSharingFile(const rsar::SoundArchive& archive, uint32_t sound)
{
    // A sound of either kind. SoundOf throws for any other.
    const bool wave_sound =
        sound < archive.Sounds().size() && archive.Sounds()[sound].type == rsar::SoundType::kWaveSound;
    const rsar::SoundInfo& info = SoundOf(archive, {wave_sound ? ItemKind::kWaveSound : ItemKind::kSequence, sound, 0});
    std::vector<uint32_t> sounds;
    for (std::size_t i = 0; i < archive.Sounds().size(); i++)
    {
        const rsar::SoundInfo& other = archive.Sounds()[i];
        if (i != sound && other.type == info.type && other.file_id == info.file_id)
        {
            sounds.push_back(static_cast<uint32_t>(i));
        }
    }

    return sounds;
}

} // namespace hokuto::song
