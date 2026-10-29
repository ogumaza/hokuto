// SPDX-License-Identifier: MIT

#include "song/items.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <exception>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/nwsnd/commands.h"
#include "ntdlib/rsar/bank_file.h"
#include "ntdlib/rsar/convert.h"
#include "ntdlib/rsar/decode.h"
#include "ntdlib/rsar/formats.h"
#include "ntdlib/rsar/wave_file.h"
#include "ntdlib/rsar/wsd_file.h"
#include "ntdlib/rvl/snd/rsar_options.h"
#include "ntdlib/sar/byte_order.h"
#include "ntdlib/sar/decode.h"
#include "ntdlib/sar/info_block.h"
#include "ntdlib/sar/sequence_file.h"
#include "ntdlib/sar/tables.h"
#include "ntdlib/sar/wave_file.h"
#include "seqlib/file_name.h"
#include "seqlib/pcm.h"
#include "seqlib/wav.h"
#include "xsflib/ascii.h"
#include "xsflib/binary.h"

#include "song/archive.h"
#include "song/streams.h"

namespace hokuto::song
{
namespace
{

namespace sar = ntdlib::sar;

// The versions of the sequence files that go to each console: the oldest that the console's games hold. The 3DS's games
// hold 1.0.0.0 or 1.1.0.0. The Wii U's games hold 1.0.0 or 2.0.0, laid out the same way. Switch games hold 2.0.0. A
// game's sound library may refuse a file newer than those of the game. The oldest version is therefore the likeliest to
// load in any game.
constexpr uint32_t kCtrSequenceVersion = 0x01000000;
constexpr uint32_t kWiiUSequenceVersion = 0x00010000;
constexpr uint32_t kSwitchSequenceVersion = 0x00020000;

// The versions of the other files that go to each console (ExportVersions): the oldest that the console's games hold.
constexpr uint32_t kCtrBankVersion = 0x01000100;
constexpr uint32_t kCtrWaveArchiveVersion = 0x01000000;
constexpr uint32_t kCtrWaveSoundFileVersion = 0x01000000;
constexpr uint32_t kCtrWaveVersion = 0x02010000;
constexpr uint32_t kBankVersion = 0x00010000;
constexpr uint32_t kWaveArchiveVersion = 0x00010000;
constexpr uint32_t kWiiUWaveSoundFileVersion = 0x00010000;
constexpr uint32_t kSwitchWaveSoundFileVersion = 0x00010100;
constexpr uint32_t kWiiUWaveVersion = 0x00010100;
constexpr uint32_t kSwitchWaveVersion = 0x00010200;

// The versions of the stream files that go to each console. The 3DS's stream files hold 2.0.0.0 to 2.3.1.0, and the
// oldest goes there. Most of the Wii U's games read stream files only up to 3.0.0. The Switch's games read 4.0.0.
constexpr uint32_t kCtrStreamVersion = 0x02000000;
constexpr uint32_t kWiiUStreamVersion = 0x00030000;
constexpr uint32_t kSwitchStreamVersion = 0x00040000;

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
        throw std::runtime_error(std::string("The archive lacks ") + kind + std::to_string(item.index) + ".");
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
        throw std::runtime_error("Hokuto can't read wave archive " + archive.WaveArchives()[wave_archive].name + ". " +
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
        throw std::runtime_error("Wave archive " + archive.WaveArchives()[wave_archive].name + " lacks wave " +
                                 std::to_string(wave) + ".");
    }

    return waves[wave];
}

// `decoded` as a WAV file of 16-bit PCM at its rate, with its loop. `what` names the wave in the message of the
// std::runtime_error that a wave that a WAV file can't hold throws.
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
        throw std::runtime_error("Hokuto can't write " + what + " as a WAV file. " + error.what());
    }
}

// Whether `adshr` fades a note in, or down before its release. A sustain at full level leaves the decay and the hold
// without effect.
bool Shapes(const sar::Adshr& adshr)
{
    return adshr.attack < 127 || adshr.sustain < 127;
}

} // namespace

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
            throw std::runtime_error("The archive lacks bank " + std::to_string(item.index) + ".");
        }
        file_id = archive.Banks()[item.index].file_id;
        name = archive.Banks()[item.index].name;
        break;

    case ItemKind::kWaveArchive:
        if (item.index >= archive.WaveArchives().size())
        {
            throw std::runtime_error("The archive lacks wave archive " + std::to_string(item.index) + ".");
        }
        file_id = archive.WaveArchives()[item.index].file_id;
        name = archive.WaveArchives()[item.index].name;
        break;

    case ItemKind::kWave:
        return WaveFileOf(archive, item.index, item.wave);

    case ItemKind::kStream:
        throw std::runtime_error("The stream of " + SoundOf(archive, item).name + " lies outside the archive.");
    }

    // A file in a group that the game keeps outside the archive is there only once the group file has been added.
    const std::span<const uint8_t> file = archive.FileData(file_id);
    if (file.empty())
    {
        const std::map<uint32_t, std::vector<std::string>> missing = FilesInMissingGroups(archive);
        const auto found = missing.find(file_id);
        throw std::runtime_error(found != missing.end() ? MissingGroupNote(name, found->second)
                                                        : "The archive lacks the file of " + name + ".");
    }

    return file;
}

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

    // The groups that the game keeps beside the archive, by their indices, with their names. A group's entry holds its
    // file's ID at 0. ntdlib names those groups in their order when it reads past damage, and lists every group when it
    // stops at the first problem.
    const bool big_endian = flavor == sar::Flavor::kCafe;
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
        for (uint32_t g = 0; g < info->groups.size() && k < archive.ExternalGroups().size(); ++g)
        {
            const std::vector<uint8_t>& entry = info->groups[g];
            if (entry.size() >= 4 && xsflib::Reader(entry, big_endian).U32(0) == sar::kExternalGroupFile)
            {
                beside[g] = archive.ExternalGroups()[k++];
            }
        }
    }

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
            const auto group = beside.find(r.U32(list + 4 + 4 * k) & 0xFFFFFF);
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
        return "The game keeps the file of " + name + " in " +
               (groups.empty() ? std::string("a group file") : "the group file of " + groups.front()) +
               ". That file wasn't beside the archive when Hokuto imported it. Import the archive again with the "
               "group file beside it.";
    }

    std::string list;
    for (std::size_t g = 0; g < groups.size(); ++g)
    {
        list += (g == 0 ? "" : g + 1 == groups.size() ? " and " : ", ") + groups[g];
    }

    return "The game keeps the file of " + name + " in the group files of " + list +
           ". None of them was beside the archive when Hokuto imported it. Import the archive again with the group "
           "files beside it.";
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

    return "The notes of " + archive.Sounds()[sound].name + " that play " + list + " stay silent." + notes;
}

std::string WriteFailure(const std::string& name)
{
    return "Hokuto can't write " + name + ". The disk may be full, or the file or its folder may be read-only.";
}

std::string FolderFailure(const std::string& name, const std::error_code& error)
{
    std::string reason = error.message();
    if (!reason.ends_with('.'))
    {
        reason += '.';
    }

    return "Hokuto can't make the folder " + name + ". " + reason;
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
        {
            const std::string last = std::to_string(waves > 0 ? waves - 1 : 0);
            std::string index = std::to_string(item.wave);
            if (index.size() < last.size())
            {
                index.insert(0, last.size() - index.size(), '0');
            }
            name = archive.WaveArchives().at(item.index).name + "_" + index;
            break;
        }

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
        throw std::runtime_error("Hokuto can't decode wave " + std::to_string(wave) + " of " +
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
        throw std::runtime_error("Hokuto can't read wave " + std::to_string(wave) + " of " +
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
        throw std::runtime_error("Hokuto can't read the wave sound file of " + info.name + ". " + error.what());
    }
    if (info.wave_sound_index >= parsed.sounds.size())
    {
        throw std::runtime_error("The wave sound file of " + info.name + " lacks wave sound " +
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
        throw std::runtime_error(info.name + " plays no note.");
    }

    // The note names a wave of the file's waves, and the file names the wave archive by its item ID. The sound and the
    // note each set a pitch, a pan and an envelope.
    const sar::VelocityRegion& region = wave_sound.notes.front();
    if (region.wave_id_index >= parsed.waves.size())
    {
        throw std::runtime_error("The wave sound file of " + info.name + " lacks the wave that the sound plays.");
    }
    const sar::WaveId& wave = parsed.waves[region.wave_id_index];
    const std::optional<std::size_t> wave_archive =
        sar::ItemIndex(wave.wave_archive_item, sar::kWaveArchiveItem, archive.WaveArchives().size());
    if (!wave_archive)
    {
        throw std::runtime_error("The archive lacks the wave archive of the wave that " + info.name + " plays.");
    }
    note.wave_archive = static_cast<uint32_t>(*wave_archive);
    note.wave = wave.index;
    note.pitch = wave_sound.pitch * region.pitch;
    note.panned = wave_sound.pan != 64 || region.pan != 64;
    note.shaped = Shapes(wave_sound.adshr) || Shapes(region.adshr);

    return note;
}

namespace
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
                                 ". No sample rate holds that.");
    }

    pcm.sample_rate = static_cast<uint32_t>(rate);

    return pcm;
}

} // namespace

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

std::string ConsoleName(sar::Flavor flavor)
{
    switch (flavor)
    {
    case sar::Flavor::kCtr:
        return "3DS";
    case sar::Flavor::kCafe:
        return "Wii U";
    case sar::Flavor::kNx:
        break;
    }

    return "Switch";
}

std::string VersionText(sar::Flavor flavor, uint32_t version)
{
    std::string text = flavor == sar::Flavor::kCtr ? std::to_string(version >> 24) + "." : "";
    return text + std::to_string(version >> 16 & 0xFF) + "." + std::to_string(version >> 8 & 0xFF) + "." +
           std::to_string(version & 0xFF);
}

bool VersionsCompare(sar::Flavor a, sar::Flavor b)
{
    return (a == sar::Flavor::kCtr) == (b == sar::Flavor::kCtr);
}

bool HoldsOriginalStart(sar::Flavor flavor, uint32_t version)
{
    return version >= (flavor == sar::Flavor::kCtr ? 0x02010100u : 0x00010200u);
}

std::optional<uint32_t> HeaderVersion(std::span<const uint8_t> file)
{
    // The header holds the magic, the byte order mark at 0x04 and the version at 0x08.
    constexpr std::size_t kMark = 0x04;
    constexpr std::size_t kVersion = 0x08;
    if (file.size() < kVersion + 4)
    {
        return std::nullopt;
    }
    const bool big = file[kMark] == 0xFE && file[kMark + 1] == 0xFF;
    const bool little = file[kMark] == 0xFF && file[kMark + 1] == 0xFE;
    if (!big && !little)
    {
        return std::nullopt;
    }

    uint32_t version = 0;
    for (std::size_t i = 0; i < 4; i++)
    {
        const uint32_t byte = file[kVersion + (big ? i : 3 - i)];
        version = version << 8 | byte;
    }

    return version;
}

sar::Flavor OtherConsole(sar::Flavor flavor)
{
    return flavor == sar::Flavor::kCafe ? sar::Flavor::kNx : sar::Flavor::kCafe;
}

std::vector<sar::Flavor> OtherConsoles(sar::Flavor flavor)
{
    std::vector<sar::Flavor> consoles = {OtherConsole(flavor)};
    for (const sar::Flavor console : {sar::Flavor::kCtr, sar::Flavor::kCafe, sar::Flavor::kNx})
    {
        if (console != flavor && console != consoles.front())
        {
            consoles.push_back(console);
        }
    }

    return consoles;
}

namespace
{

// Why console `to` can't hold the commands of `sequence` after its encoder refused them with `error`. Each command that
// the console can't write by itself counts by what it sets (SettingName). The 3DS lacks the Wii U's and the Switch's
// LFOs 1 to 3, for one. Another failure keeps the encoder's words.
std::string WhyNotHeld(const ntdlib::commands::Sequence& sequence, sar::Flavor to, const std::string& error)
{
    using ntdlib::commands::Kind;
    std::map<std::string, std::size_t> lacked;
    std::size_t total = 0;
    for (const ntdlib::commands::Command& command : sequence.items)
    {
        if (command.kind == Kind::kData || ntdlib::commands::HasTarget(command.kind))
        {
            continue;
        }

        ntdlib::commands::Sequence alone;
        alone.items = {command};
        try
        {
            ntdlib::nwsnd::EncodeSequence(alone, GrammarOf(to));
        }
        catch (const std::invalid_argument&)
        {
            lacked[SettingName(command)]++;
            total++;
        }
    }
    if (total == 0)
    {
        return "The " + ConsoleName(to) + " can't hold its commands. " + error;
    }

    // Such as "7 of LFOs 1 to 3", or "2 of LFOs 1 to 3 and 1 of time prefixes".
    std::string list;
    std::size_t listed = 0;
    for (const auto& [name, count] : lacked)
    {
        list += listed == 0 ? "" : listed + 1 == lacked.size() ? " and " : ", ";
        list += std::to_string(count) + " of " + name;
        listed++;
    }

    return "The " + ConsoleName(to) + "'s player lacks " + std::to_string(total) + " of the file's commands: " + list +
           ".";
}

} // namespace

ConvertedSequence ConvertSequenceFile(std::span<const uint8_t> file, sar::Flavor from, sar::Flavor to,
                                      std::span<const uint32_t> starts)
{
    // The commands as the console reads them, from the labels and the starts.
    sar::SequenceFile read;
    std::vector<ntdlib::nwsnd::LabelOffset> labels;
    ntdlib::nwsnd::Decoded decoded;
    try
    {
        read = sar::SequenceFile::Read(file);
        for (const sar::Sequence::Label& label : read.labels)
        {
            labels.push_back({label.name, label.offset});
        }
        decoded = ntdlib::nwsnd::DecodeSequence(read.data, labels, GrammarOf(from), starts);
    }
    catch (const std::exception& error)
    {
        throw std::runtime_error(std::string("The file can't be read. ") + error.what());
    }
    if (decoded.opaque)
    {
        throw std::runtime_error("The file's commands overlap or jump into one another.");
    }

    // The same commands as the target's console writes them. The labels take the offsets of their items.
    const BiquadChanges biquads = MapBiquads(decoded.sequence, decoded.entries, {.flavor = from}, {.flavor = to});
    ntdlib::nwsnd::Encoded encoded;
    try
    {
        encoded = ntdlib::nwsnd::EncodeSequence(decoded.sequence, GrammarOf(to));
    }
    catch (const std::invalid_argument& error)
    {
        throw std::runtime_error(WhyNotHeld(decoded.sequence, to, error.what()));
    }
    sar::SequenceFile converted;
    converted.flavor = to;
    converted.version = ExportVersions(ItemKind::kSequence, to).file;
    converted.data = std::move(encoded.bytes);
    for (const ntdlib::commands::Label& label : decoded.sequence.labels)
    {
        converted.labels.push_back({label.name, encoded.offsets[label.item]});
    }

    return {converted.Write(), biquads};
}

ConvertedSequence OtherConsoleSequenceFile(const sar::SoundArchive& archive, uint32_t sound, sar::Flavor to)
{
    // The sounds that play the file start where their start offsets say, with a label or without one.
    const std::span<const uint8_t> file = ItemFile(archive, {ItemKind::kSequence, sound, 0});
    std::vector<uint32_t> starts = {archive.Sounds()[sound].start_offset};
    for (const uint32_t other : SoundsSharingFile(archive, sound))
    {
        starts.push_back(archive.Sounds()[other].start_offset);
    }

    try
    {
        return ConvertSequenceFile(file, archive.ReadOptions().flavor, to, starts);
    }
    catch (const std::exception& error)
    {
        throw std::runtime_error("Hokuto can't convert the sequence file of " + archive.Sounds()[sound].name + ". " +
                                 error.what());
    }
}

FileVersions ExportVersions(ItemKind kind, sar::Flavor flavor)
{
    const bool ctr = flavor == sar::Flavor::kCtr;
    const bool wii_u = flavor == sar::Flavor::kCafe;
    const uint32_t wave = ctr ? kCtrWaveVersion : wii_u ? kWiiUWaveVersion : kSwitchWaveVersion;
    switch (kind)
    {
    case ItemKind::kSequence:
        return {ctr ? kCtrSequenceVersion : wii_u ? kWiiUSequenceVersion : kSwitchSequenceVersion, 0};
    case ItemKind::kWaveSound:
    case ItemKind::kWaveSoundFile:
        return {ctr ? kCtrWaveSoundFileVersion : wii_u ? kWiiUWaveSoundFileVersion : kSwitchWaveSoundFileVersion, 0};
    case ItemKind::kBank:
        return {ctr ? kCtrBankVersion : kBankVersion, 0};
    case ItemKind::kWaveArchive:
        return {ctr ? kCtrWaveArchiveVersion : kWaveArchiveVersion, wave};
    case ItemKind::kWave:
        break;
    case ItemKind::kStream:
        return {ctr ? kCtrStreamVersion : wii_u ? kWiiUStreamVersion : kSwitchStreamVersion, 0};
    }

    return {wave, 0};
}

std::vector<uint8_t> ConvertItemFile(std::span<const uint8_t> file, ItemKind kind, sar::Flavor from, sar::Flavor to,
                                     const FileVersions& versions)
{
    if (kind == ItemKind::kSequence)
    {
        throw std::invalid_argument("ConvertSequenceFile converts sequence files.");
    }

    // The magic gives the file's kind and its console's letter, and the byte order mark its byte order.
    const std::string format = ItemFormat(kind, from);
    const bool wii_u = from == sar::Flavor::kCafe;
    if (file.size() < 8 || !std::equal(format.begin() + 1, format.end(), file.begin()) ||
        file[4] != (wii_u ? 0xFE : 0xFF) || file[5] != (wii_u ? 0xFF : 0xFE))
    {
        throw std::runtime_error("It isn't a " + format + " file of the " + ConsoleName(from) + ".");
    }
    if (kind == ItemKind::kStream)
    {
        return ConvertStreamFile(file, to, versions.file);
    }

    // A bank file and a wave sound file keep their layout. A wave and a wave archive are written again.
    try
    {
        if (kind == ItemKind::kWaveArchive)
        {
            return sar::ConvertWaveArchive(sar::WaveArchiveFile::Read(file), to, versions.file, versions.waves).Write();
        }
        if (kind == ItemKind::kWave)
        {
            return sar::ConvertWave(sar::WaveFile::Read(file), to, versions.file).Write();
        }

        return sar::WithConsole(file, to, versions.file);
    }
    catch (const std::exception& error)
    {
        throw std::runtime_error(error.what());
    }
}

std::vector<uint8_t> OtherConsoleFile(const sar::SoundArchive& archive, const Item& item, sar::Flavor to)
{
    if (item.kind == ItemKind::kSequence)
    {
        return OtherConsoleSequenceFile(archive, item.index, to).file;
    }

    const std::span<const uint8_t> file = ItemFile(archive, item);
    const sar::Flavor flavor = archive.ReadOptions().flavor;
    try
    {
        return ConvertItemFile(file, item.kind, flavor, to, ExportVersions(item.kind, to));
    }
    catch (const std::exception& error)
    {
        throw std::runtime_error("Hokuto can't convert the file of " + ItemName(archive, item) + ". " + error.what());
    }
}

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
        throw std::runtime_error("Hokuto can't convert the file of " + ItemName(archive, item) + " for the Wii. " +
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
        throw std::runtime_error(std::string("The archive lacks ") + kind + std::to_string(item.index) + ".");
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

    throw std::runtime_error("The archive lacks the waves of file " + std::to_string(file_id) + ".");
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
        throw std::runtime_error("Hokuto can't read the waves of " + wave_archive.name + ". " + error.what());
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
        throw std::runtime_error("Hokuto can't read the waves of " + wave_archive.name + ". " + error.what());
    }
}

// Wave `wave` of `wave_archive` as ntdlib reads it. Throws std::runtime_error when the wave archive lacks the wave, and
// when the wave can't be read.
rsar::Wave WaveOf(const rsar::SoundArchive& archive, const WiiWaveArchive& wave_archive, uint32_t wave)
{
    const std::size_t count =
        wave_archive.rwar ? RwavsOf(archive, wave_archive).size() : BlockWavesOf(archive, wave_archive).size();
    if (wave >= count)
    {
        throw std::runtime_error(wave_archive.name + " lacks wave " + std::to_string(wave) + ".");
    }
    if (!wave_archive.rwar)
    {
        return BlockWavesOf(archive, wave_archive)[wave];
    }

    try
    {
        return rsar::Wave::ParseFile(RwavsOf(archive, wave_archive)[wave]);
    }
    catch (const std::exception& error)
    {
        throw std::runtime_error("Hokuto can't read wave " + std::to_string(wave) + " of " + wave_archive.name + ". " +
                                 error.what());
    }
}

} // namespace

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
            throw std::runtime_error("The archive lacks bank " + std::to_string(item.index) + ".");
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
                                         " are no file by themselves. The WAVE block of their file describes them.");
            }
            if (item.kind == ItemKind::kWaveArchive)
            {
                return archive.WaveData(wave_archive.file_id);
            }

            const std::vector<std::span<const uint8_t>> waves = RwavsOf(archive, wave_archive);
            if (item.wave >= waves.size())
            {
                throw std::runtime_error(wave_archive.name + " lacks wave " + std::to_string(item.wave) + ".");
            }

            return waves[item.wave];
        }

    case ItemKind::kStream:
        throw std::runtime_error("The stream of " + SoundOf(archive, item).name + " lies outside the archive.");
    }

    const std::span<const uint8_t> file = archive.FileData(file_id);
    if (file.empty())
    {
        throw std::runtime_error("The archive lacks the file of " + name + ".");
    }

    return file;
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
            throw std::runtime_error("The archive lacks the file.");
        }
        waves = data->file.size() >= 4 && std::memcmp(data->file.data(), "RWSD", 4) == 0
                    ? rsar::WsdFile::Read(data->file, data->wave_data).waves
                    : rsar::BankFile::Read(data->file, data->wave_data).waves;
    }
    catch (const std::exception& error)
    {
        throw std::runtime_error("Hokuto can't read the waves of " + wave_archive.name + ". " + error.what());
    }
    if (item.kind == ItemKind::kWave && item.wave >= waves.size())
    {
        throw std::runtime_error(wave_archive.name + " lacks wave " + std::to_string(item.wave) + ".");
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
        throw std::runtime_error("Hokuto can't write the waves of " + wave_archive.name + ". " + error.what());
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
        {
            const std::string last = std::to_string(waves > 0 ? waves - 1 : 0);
            std::string index = std::to_string(item.wave);
            if (index.size() < last.size())
            {
                index.insert(0, last.size() - index.size(), '0');
            }
            name = WaveArchiveOf(archive, item.index).name + "_" + index;
            break;
        }
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
                throw std::runtime_error("Hokuto can't read wave " + std::to_string(wave) + " of " + wave_archive.name +
                                         ". " + error.what());
            }
        }
    }
    if (item.kind == ItemKind::kWave && item.wave >= waves.size())
    {
        throw std::runtime_error(wave_archive.name + " lacks wave " + std::to_string(item.wave) + ".");
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
            throw std::runtime_error("Hokuto can't convert wave " + std::to_string(wave) + " of " + wave_archive.name +
                                     ". " + error.what());
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
        throw std::runtime_error("Hokuto can't decode wave " + std::to_string(wave) + " of " + wave_archive.name +
                                 ". " + error.what());
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
        throw std::runtime_error("Hokuto can't read the wave sound file of " + info.name + ". " + error.what());
    }
    if (info.sub_index < 0 || static_cast<std::size_t>(info.sub_index) >= parsed.Sounds().size())
    {
        throw std::runtime_error("The wave sound file of " + info.name + " lacks wave sound " +
                                 std::to_string(info.sub_index) + ".");
    }

    // NW4R SND plays note 0, at the sound's pitch.
    const rsar::WaveSoundFile::WaveSound& wave_sound = parsed.Sounds()[static_cast<std::size_t>(info.sub_index)];
    if (wave_sound.notes.empty())
    {
        throw std::runtime_error(info.name + " plays no note.");
    }
    const rsar::WaveSoundFile::Note& first = wave_sound.notes.front();
    if (first.wave_index < 0 || static_cast<std::size_t>(first.wave_index) >= WaveCountOf(archive, info.file_id))
    {
        throw std::runtime_error("The wave sound file of " + info.name + " lacks the wave that the sound plays.");
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
