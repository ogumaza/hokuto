// SPDX-License-Identifier: MIT

#include "song/items.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <exception>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

#include "ntdlib/sar/decode.h"
#include "ntdlib/sar/info_block.h"
#include "ntdlib/sar/tables.h"
#include "ntdlib/sar/wave_file.h"
#include "seqlib/file_name.h"
#include "seqlib/pcm.h"
#include "seqlib/wav.h"
#include "xsflib/ascii.h"
#include "xsflib/binary.h"

#include "song/archive.h"
#include "song/items_parts.h"
#include "song/streams.h"

namespace hokuto::song
{
namespace
{

namespace sar = ntdlib::sar;

using items_parts::AtPitch;
using items_parts::kCutShort;

// The file of item `index` among `items`, or nothing when the archive lacks it.
template <typename Info>
std::span<const uint8_t> FileOf(const sar::SoundArchive& archive, const std::vector<Info>& items, std::size_t index)
{
    if (index >= items.size())
    {
        return {};
    }

    return archive.FileData(items[index].file_id);
}

// The sound of `item`, a sequence sound, a wave sound or a stream sound. Throws std::runtime_error when the archive
// lacks it.
const sar::SoundInfo& SoundOf(const sar::SoundArchive& archive, const Item& item)
{
    const sar::SoundType type = item.kind == ItemKind::kSequence ? sar::SoundType::kSequence
                                : item.kind == ItemKind::kStream ? sar::SoundType::kStream
                                                                 : sar::SoundType::kWaveSound;
    if (item.index >= archive.Sounds().size() || archive.Sounds()[item.index].type != type)
    {
        const char* kind = type == sar::SoundType::kSequence ? "sequence sound "
                           : type == sar::SoundType::kStream ? "stream sound "
                                                             : "wave sound ";
        throw std::runtime_error(std::string("The archive has no ") + kind + std::to_string(item.index) + ".");
    }

    return archive.Sounds()[item.index];
}

// The wave sound file of `item`, a wave sound file given by its first wave sound, among WaveSoundSets. Throws
// std::runtime_error when the archive lacks the wave sound. WaveSoundSets lists the file of every wave sound.
WaveSoundSet SetOf(const sar::SoundArchive& archive, const Item& item)
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

// The waves of wave archive `wave_archive`. Throws std::runtime_error when the archive lacks the wave archive or its
// file, and when the file can't be read.
std::vector<std::span<const uint8_t>> WavesOf(const sar::SoundArchive& archive, uint32_t wave_archive)
{
    const std::span<const uint8_t> file = ItemFile(archive, {ItemKind::kWaveArchive, wave_archive, 0});
    try
    {
        return sar::WaveArchive::Parse(file, ItemOptions(archive)).waves;
    }
    catch (const std::exception& error)
    {
        throw std::runtime_error("Can't read wave archive " + archive.WaveArchives()[wave_archive].name + ". " +
                                 error.what());
    }
}

// The file of wave `wave` of wave archive `wave_archive`. Throws std::runtime_error when WavesOf does, and when the
// wave archive lacks the wave.
std::span<const uint8_t> WaveFileOf(const sar::SoundArchive& archive, uint32_t wave_archive, uint32_t wave)
{
    const std::vector<std::span<const uint8_t>> waves = WavesOf(archive, wave_archive);
    if (wave >= waves.size())
    {
        throw std::runtime_error("Wave archive " + archive.WaveArchives()[wave_archive].name + " has no wave " +
                                 std::to_string(wave) + ".");
    }

    return waves[wave];
}

// Whether `adshr` fades a note in, or down before its release. A sustain at full level leaves the decay and the hold
// without effect.
bool Shapes(const sar::Adshr& adshr)
{
    return adshr.attack < 127 || adshr.sustain < 127;
}

} // namespace

std::vector<uint8_t> PcmAsWav(sar::Pcm decoded, const std::string& what)
{
    seqlib::Pcm pcm;
    pcm.loop = decoded.loop;
    pcm.loop_start = decoded.loop_start;
    pcm.loop_end = decoded.loop_end;
    pcm.channels = std::move(decoded.channels);
    try
    {
        return seqlib::WavFile(pcm, decoded.sample_rate);
    }
    catch (const std::invalid_argument& error)
    {
        throw std::runtime_error("Can't write " + what + " as a WAV file. " + error.what());
    }
}

std::vector<WaveSoundSet> WaveSoundSets(const sar::SoundArchive& archive)
{
    // The wave sounds of each file, in the order of their first wave sounds.
    std::vector<WaveSoundSet> sets;
    std::map<uint32_t, std::size_t> places;
    for (uint32_t i = 0; i < archive.Sounds().size(); i++)
    {
        const sar::SoundInfo& sound = archive.Sounds()[i];
        if (sound.type != sar::SoundType::kWaveSound)
        {
            continue;
        }

        const auto [place, added] = places.emplace(sound.file_id, sets.size());
        if (added)
        {
            sets.push_back({sound.file_id, {}, {}});
        }

        sets[place->second].sounds.push_back(i);
    }

    // Each file takes the name of the sound group that holds its first wave sound and names the file. A file outside
    // every group, such as one that File > Import > Wave Sound File… adds, takes its first wave sound's name.
    const std::vector<sar::SoundGroupInfo>& groups = archive.SoundGroups();
    for (WaveSoundSet& set : sets)
    {
        const uint32_t first = set.sounds.front();
        const auto holds = [&set, first](const sar::SoundGroupInfo& group)
        {
            return group.first_sound <= first && first <= group.last_sound &&
                   std::ranges::find(group.file_ids, set.file_id) != group.file_ids.end();
        };
        const auto group = std::ranges::find_if(groups, holds);
        const std::string& sound = archive.Sounds()[first].name;
        if (group != groups.end())
        {
            set.name = group->name.empty() ? "WSDSET_" + std::to_string(group - groups.begin()) : group->name;
        }
        else
        {
            set.name = sound.empty() ? "WSD_FILE_" + std::to_string(set.file_id) : sound;
        }
    }

    return sets;
}

sar::Options ItemOptions(const sar::SoundArchive& archive)
{
    return {archive.ReadOptions().flavor, {}, false, sar::Rules::kStopAtFirstProblem};
}

std::string ItemFormat(ItemKind kind, sar::Flavor flavor)
{
    const std::string prefix = flavor == sar::Flavor::kCtr ? "BC" : "BF";
    switch (kind)
    {
    case ItemKind::kSequence:
        return prefix + "SEQ";
    case ItemKind::kWaveSound:
    case ItemKind::kWaveSoundFile:
        return prefix + "WSD";
    case ItemKind::kBank:
        return prefix + "BNK";
    case ItemKind::kWaveArchive:
        return prefix + "WAR";
    case ItemKind::kWave:
        return prefix + "WAV";
    case ItemKind::kStream:
        return prefix + "STM";
    }

    return prefix;
}

std::vector<uint32_t> ProgramsOf(const sar::SoundArchive& archive, std::size_t bank)
{
    std::vector<uint32_t> programs;
    const std::span<const uint8_t> file = FileOf(archive, archive.Banks(), bank);
    if (file.empty())
    {
        return programs;
    }

    const sar::Bank parsed = sar::Bank::Parse(file, ItemOptions(archive));
    for (std::size_t program = 0; program < parsed.instruments.size(); program++)
    {
        if (parsed.instruments[program])
        {
            programs.push_back(static_cast<uint32_t>(program));
        }
    }

    return programs;
}

std::size_t WaveCountOf(const sar::SoundArchive& archive, std::size_t wave_archive)
{
    const std::span<const uint8_t> file = FileOf(archive, archive.WaveArchives(), wave_archive);
    if (file.empty())
    {
        return 0;
    }

    return WavesOf(archive, static_cast<uint32_t>(wave_archive)).size();
}

std::span<const uint8_t> ItemFile(const sar::SoundArchive& archive, const Item& item)
{
    // The item's file ID and name. A wave's file sits inside its wave archive's file.
    uint32_t file_id = 0;
    std::string name;
    switch (item.kind)
    {
    case ItemKind::kSequence:
    case ItemKind::kWaveSound:
    case ItemKind::kWaveSoundFile:
        {
            const sar::SoundInfo& sound = SoundOf(archive, item);
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
        if (item.index >= archive.WaveArchives().size())
        {
            throw std::runtime_error("The archive has no wave archive " + std::to_string(item.index) + ".");
        }

        file_id = archive.WaveArchives()[item.index].file_id;
        name = archive.WaveArchives()[item.index].name;
        break;

    case ItemKind::kWave:
        return WaveFileOf(archive, item.index, item.wave);

    case ItemKind::kStream:
        throw std::runtime_error("The stream of " + SoundOf(archive, item).name + " is stored outside the archive.");
    }

    // A file in a group that the game keeps outside the archive is there only once the group file has been added.
    const std::span<const uint8_t> file = archive.FileData(file_id);
    if (file.empty())
    {
        const std::map<uint32_t, std::vector<std::string>> missing = FilesInMissingGroups(archive);
        const auto found = missing.find(file_id);
        throw std::runtime_error(found != missing.end()         ? MissingGroupNote(name, found->second)
                                 : archive.FileMissing(file_id) ? CutShortNote(name)
                                                                : "The archive has no file for " + name + ".");
    }

    return file;
}

namespace
{

// The groups that the game keeps beside the archive, by their indices, with their names. A group's entry holds its
// file's ID at 0. ntdlib names those groups in their order when it reads past damage, and lists every group when it
// stops at the first problem.
std::map<uint32_t, std::string> GroupsBeside(const sar::SoundArchive& archive, const sar::InfoBlock& info,
                                             bool big_endian)
{
    std::map<uint32_t, std::string> beside;
    if (!archive.Groups().empty())
    {
        for (uint32_t g = 0; g < archive.Groups().size(); ++g)
        {
            const std::string& name = archive.Groups()[g].name;
            if (archive.Groups()[g].file_id == sar::kExternalGroupFile)
            {
                beside[g] = name.empty() ? "GROUP_" + std::to_string(g) : name;
            }
        }
    }
    else
    {
        std::size_t k = 0;
        for (uint32_t g = 0; g < info.groups.size() && k < archive.ExternalGroups().size(); ++g)
        {
            const std::vector<uint8_t>& entry = info.groups[g];
            if (entry.size() >= 4 && xsflib::Reader(entry, big_endian).U32(0) == sar::kExternalGroupFile)
            {
                beside[g] = archive.ExternalGroups()[k++];
            }
        }
    }

    return beside;
}

} // namespace

std::map<uint32_t, std::vector<std::string>> FilesInMissingGroups(const sar::SoundArchive& archive)
{
    // A file entry of the INFO block: a reference to the file's location, with its type at 0 and its offset from the
    // entry's start at 4. A file in the archive has type 0x220C. Its location holds the file's offset from the FILE
    // block's body at 4, or 0xFFFFFFFF where only groups hold the file. A reference to the list of the file's groups
    // follows at 12, with its type at 12 and its offset from the location at 16. The list holds a count, then the
    // groups' item IDs. The 3DS's entries hold no list.
    constexpr uint16_t kInternalFile = 0x220C;
    constexpr uint32_t kNoPlace = 0xFFFFFFFF;
    constexpr uint16_t kListType = 0x0100;
    std::map<uint32_t, std::vector<std::string>> missing;
    const sar::Flavor flavor = archive.ReadOptions().flavor;
    std::optional<sar::InfoBlock> info;
    try
    {
        info = sar::InfoBlock::Read(sar::TablesOf(archive.Bytes(), flavor).info, flavor);
    }
    catch (const std::exception&)
    {
        return missing;
    }

    const bool big_endian = flavor == sar::Flavor::kCafe;
    const std::map<uint32_t, std::string> beside = GroupsBeside(archive, *info, big_endian);

    if (beside.empty())
    {
        return missing;
    }

    for (uint32_t id = 0; id < info->files.size(); ++id)
    {
        const std::vector<uint8_t>& entry = info->files[id];
        const xsflib::Reader r(entry, big_endian);
        const std::size_t location = entry.size() >= 8 ? r.U32(4) : entry.size();
        if (!archive.FileData(id).empty() || r.U16(0) != kInternalFile || location + 8 > entry.size() ||
            r.U32(location + 4) != kNoPlace)
        {
            continue;
        }

        // Without a list, the file comes without names. A list that names no group beside the archive leaves the file
        // out: no group file holds it.
        if (location + 20 > entry.size() || r.U16(location + 12) != kListType)
        {
            missing[id];
            continue;
        }

        std::vector<std::string> groups;
        const std::size_t list = location + r.U32(location + 16);
        for (std::size_t k = 0; list + 4 <= entry.size() && k < r.U32(list) && list + 8 + 4 * k <= entry.size(); ++k)
        {
            const auto group = beside.find(sar::ItemNumber(r.U32(list + 4 + 4 * k)));
            if (group != beside.end())
            {
                groups.push_back(group->second);
            }
        }
        if (!groups.empty())
        {
            missing[id] = std::move(groups);
        }
    }

    return missing;
}

std::string MissingGroupNote(const std::string& name, std::span<const std::string> groups)
{
    if (groups.size() < 2)
    {
        return "The game stores " + name + " in " +
               (groups.empty() ? std::string("a group file") : "the group file for " + groups.front()) +
               ". That file wasn't next to the archive when you imported it. Import the archive again with the "
               "group file next to it.";
    }

    std::string list;
    for (std::size_t g = 0; g < groups.size(); ++g)
    {
        list += (g == 0 ? "" : g + 1 == groups.size() ? " and " : ", ") + groups[g];
    }

    return "The game stores " + name + " in the group files for " + list +
           ". None of them was next to the archive when you imported it. Import the archive again with the group "
           "files next to it.";
}

std::string CutShortNote(const std::string& name)
{
    return std::string(kCutShort) + "The file for " + name + " is missing.";
}

std::string MissingBanksNote(const sar::SoundArchive& archive, uint32_t sound)
{
    if (sound >= archive.Sounds().size() || archive.Sounds()[sound].banks.empty())
    {
        return {};
    }

    const std::map<uint32_t, std::vector<std::string>> missing = FilesInMissingGroups(archive);
    std::vector<std::string> banks;
    std::string notes;
    for (const uint32_t bank : archive.Sounds()[sound].banks)
    {
        const uint32_t index = sar::ItemNumber(bank);
        if (bank == sar::kNoItem || index >= archive.Banks().size())
        {
            continue;
        }

        const sar::BankInfo& info = archive.Banks()[index];
        const auto found = missing.find(info.file_id);
        if (found == missing.end() || std::ranges::find(banks, info.name) != banks.end())
        {
            continue;
        }

        banks.push_back(info.name);
        notes += " " + MissingGroupNote(info.name, found->second);
    }
    if (banks.empty())
    {
        return {};
    }

    std::string list;
    for (std::size_t b = 0; b < banks.size(); ++b)
    {
        list += (b == 0 ? "" : b + 1 == banks.size() ? " or " : ", ") + banks[b];
    }

    return "Notes of " + archive.Sounds()[sound].name + " that use " + list + " will be silent." + notes;
}

std::string WriteFailure(const std::string& name)
{
    return "Can't write " + name + ". The disk may be full, or the file or folder may be read-only.";
}

std::string FolderFailure(const std::string& name, const std::error_code& error)
{
    std::string reason = error.message();
    if (!reason.ends_with('.'))
    {
        reason += '.';
    }

    return "Can't create the folder " + name + ". " + reason;
}

std::string PaddedIndex(uint32_t index, std::size_t count)
{
    const std::string last = std::to_string(count > 0 ? count - 1 : 0);
    std::string padded = std::to_string(index);
    if (padded.size() < last.size())
    {
        padded.insert(0, last.size() - padded.size(), '0');
    }

    return padded;
}

std::string ItemFileName(const sar::SoundArchive& archive, const Item& item, std::size_t waves)
{
    std::string name;
    switch (item.kind)
    {
    case ItemKind::kSequence:
    case ItemKind::kWaveSound:
        name = SoundOf(archive, item).name;
        break;

    case ItemKind::kWaveSoundFile:
        name = SetOf(archive, item).name;
        break;

    case ItemKind::kBank:
        name = archive.Banks().at(item.index).name;
        break;

    case ItemKind::kWaveArchive:
        name = archive.WaveArchives().at(item.index).name;
        break;

    // The index takes as many digits as the wave archive's last index.
    case ItemKind::kWave:
        name = archive.WaveArchives().at(item.index).name + "_" + PaddedIndex(item.wave, waves);
        break;

    case ItemKind::kStream:
        SoundOf(archive, item);
        return StreamFileName(archive, item.index);
    }

    return seqlib::SafeFileName(name) + "." + xsflib::AsciiLower(ItemFormat(item.kind, archive.ReadOptions().flavor));
}

std::string WavFileName(const sar::SoundArchive& archive, uint32_t wave_archive, uint32_t wave, std::size_t waves)
{
    std::string name = ItemFileName(archive, {ItemKind::kWave, wave_archive, wave}, waves);
    name.erase(name.rfind('.'));

    return name + ".wav";
}

std::string ItemName(const sar::SoundArchive& archive, const Item& item)
{
    switch (item.kind)
    {
    case ItemKind::kSequence:
    case ItemKind::kWaveSound:
        return SoundOf(archive, item).name;

    case ItemKind::kWaveSoundFile:
        return SetOf(archive, item).name;

    case ItemKind::kBank:
        return archive.Banks().at(item.index).name;

    case ItemKind::kWaveArchive:
        return archive.WaveArchives().at(item.index).name;

    case ItemKind::kWave:
        return archive.WaveArchives().at(item.index).name + ", wave " + std::to_string(item.wave);

    case ItemKind::kStream:
        return SoundOf(archive, item).name;
    }

    return {};
}

sar::Pcm DecodeItemWave(const sar::SoundArchive& archive, uint32_t wave_archive, uint32_t wave)
{
    const std::span<const uint8_t> file = WaveFileOf(archive, wave_archive, wave);
    try
    {
        return sar::DecodeWave(file, ItemOptions(archive));
    }
    catch (const std::exception& error)
    {
        throw std::runtime_error("Can't decode wave " + std::to_string(wave) + " of " +
                                 archive.WaveArchives()[wave_archive].name + ". " + error.what());
    }
}

std::vector<uint8_t> WaveAsWav(const sar::SoundArchive& archive, uint32_t wave_archive, uint32_t wave)
{
    // The decode checks the indices. It runs before the name reads them: C++ doesn't fix the order of a call's
    // arguments, and MSVC and clang-cl run them from right to left.
    sar::Pcm decoded = DecodeItemWave(archive, wave_archive, wave);
    return PcmAsWav(std::move(decoded),
                    "wave " + std::to_string(wave) + " of " + archive.WaveArchives()[wave_archive].name);
}

std::size_t WaveChannels(const sar::SoundArchive& archive, uint32_t wave_archive, uint32_t wave)
{
    const std::span<const uint8_t> file = WaveFileOf(archive, wave_archive, wave);
    try
    {
        return sar::Wave::Parse(file, ItemOptions(archive)).channels.size();
    }
    catch (const std::exception& error)
    {
        throw std::runtime_error("Can't read wave " + std::to_string(wave) + " of " +
                                 archive.WaveArchives()[wave_archive].name + ". " + error.what());
    }
}

WaveSoundNote WaveSoundNoteOf(const sar::SoundArchive& archive, uint32_t sound)
{
    // The sound's place among the wave sounds of its file.
    const std::span<const uint8_t> file = ItemFile(archive, {ItemKind::kWaveSound, sound, 0});
    const sar::SoundInfo& info = archive.Sounds()[sound];
    sar::WaveSoundFile parsed;
    try
    {
        parsed = sar::WaveSoundFile::Parse(file, ItemOptions(archive));
    }
    catch (const std::exception& error)
    {
        throw std::runtime_error("Can't read the wave sound file of " + info.name + ". " + error.what());
    }

    if (info.wave_sound_index >= parsed.sounds.size())
    {
        throw std::runtime_error("The wave sound file of " + info.name + " has no wave sound " +
                                 std::to_string(info.wave_sound_index) + ".");
    }

    // The consoles play note 0 of the sound, and read none of the note events of its tracks (ntdlib's WaveSoundPlayer).
    // The loop below only counts them, for `events`.
    const sar::WaveSoundFile::WaveSound& wave_sound = parsed.sounds[info.wave_sound_index];
    WaveSoundNote note;
    for (const std::vector<sar::WaveSoundFile::NoteEvent>& track : wave_sound.tracks)
    {
        note.events += track.size();
    }

    if (wave_sound.notes.empty())
    {
        throw std::runtime_error(info.name + " has no notes.");
    }

    // The note names a wave of the file's waves, and the file names the wave archive by its item ID. The sound and the
    // note each set a pitch, a pan and an envelope.
    const sar::VelocityRegion& region = wave_sound.notes.front();
    if (region.wave_id_index >= parsed.waves.size())
    {
        throw std::runtime_error("The wave sound file of " + info.name + " is missing the wave that the sound uses.");
    }

    const sar::WaveId& wave = parsed.waves[region.wave_id_index];
    const std::optional<std::size_t> wave_archive =
        sar::ItemIndex(wave.wave_archive_item, sar::kWaveArchiveItem, archive.WaveArchives().size());
    if (!wave_archive)
    {
        throw std::runtime_error("The archive is missing the wave archive that " + info.name + " uses.");
    }

    note.wave_archive = static_cast<uint32_t>(*wave_archive);
    note.wave = wave.index;
    note.pitch = wave_sound.pitch * region.pitch;
    note.panned = wave_sound.pan != 64 || region.pan != 64;
    note.shaped = Shapes(wave_sound.adshr) || Shapes(region.adshr);

    return note;
}

namespace items_parts
{

// `pcm` at the rate at which wave sound `name` plays it at `pitch`. The console reads the wave faster or slower by the
// pitch. At the rate that the pitch gives, the samples play at the same speed. A rate rounded to whole hertz is off by
// half a hertz at most: about a tenth of a cent at 8 kHz.
ntdlib::wave::Pcm AtPitch(ntdlib::wave::Pcm pcm, float pitch, const std::string& name)
{
    const double rate = std::round(static_cast<double>(pcm.sample_rate) * static_cast<double>(pitch));
    if (!(rate >= 1.0 && rate <= static_cast<double>(std::numeric_limits<int32_t>::max())))
    {
        std::array<char, 32> text{};
        std::snprintf(text.data(), text.size(), "%g", static_cast<double>(pitch));
        throw std::runtime_error(name + " plays its wave at a pitch of " + text.data() +
                                 ". No WAV sample rate matches that pitch.");
    }

    pcm.sample_rate = static_cast<uint32_t>(rate);

    return pcm;
}

} // namespace items_parts

sar::Pcm DecodeWaveSound(const sar::SoundArchive& archive, uint32_t sound)
{
    const WaveSoundNote note = WaveSoundNoteOf(archive, sound);
    return AtPitch(DecodeItemWave(archive, note.wave_archive, note.wave), note.pitch, archive.Sounds()[sound].name);
}

std::vector<uint8_t> WaveSoundAsWav(const sar::SoundArchive& archive, uint32_t sound)
{
    // The decode checks the sound's index. It runs before the name reads the index (see WaveAsWav).
    sar::Pcm decoded = DecodeWaveSound(archive, sound);
    return PcmAsWav(std::move(decoded), archive.Sounds()[sound].name);
}

std::string WaveSoundWavFileName(const sar::SoundArchive& archive, uint32_t sound)
{
    std::string name = ItemFileName(archive, {ItemKind::kWaveSound, sound, 0});
    name.erase(name.rfind('.'));

    return name + ".wav";
}

std::vector<uint32_t> SoundsSharingFile(const sar::SoundArchive& archive, uint32_t sound)
{
    // A sound of either kind. SoundOf throws for any other.
    const bool wave_sound =
        sound < archive.Sounds().size() && archive.Sounds()[sound].type == sar::SoundType::kWaveSound;
    const sar::SoundInfo& info = SoundOf(archive, {wave_sound ? ItemKind::kWaveSound : ItemKind::kSequence, sound, 0});
    std::vector<uint32_t> sounds;
    for (std::size_t i = 0; i < archive.Sounds().size(); i++)
    {
        const sar::SoundInfo& other = archive.Sounds()[i];
        if (i != sound && other.type == info.type && other.file_id == info.file_id)
        {
            sounds.push_back(static_cast<uint32_t>(i));
        }
    }

    return sounds;
}

} // namespace hokuto::song
