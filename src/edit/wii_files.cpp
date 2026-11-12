// SPDX-License-Identifier: MIT

// Replace… of the banks, wave archives, wave sound files and waves of the Wii (edit/files.h), with their wave data.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ntdlib/rsar/bank_file.h"
#include "ntdlib/rsar/convert.h"
#include "ntdlib/rsar/formats.h"
#include "ntdlib/rsar/wave_file.h"
#include "ntdlib/rsar/wsd_file.h"
#include "ntdlib/rvl/snd/rsar_options.h"
#include "ntdlib/sar/encodings.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sar/wave_file.h"
#include "xsflib/binary.h"
#include "xsflib/bytes.h"

#include "edit/files.h"
#include "edit/files_internal.h"
#include "edit/other_console.h"
#include "edit/waves.h"
#include "format/kinds.h"

namespace hokuto::edit
{

namespace
{

namespace sar = ntdlib::sar;
namespace rsar = ntdlib::rsar;

// The magic of a file of the Wii of `kind`, and of the same file on the 3DS, the Wii U or the Switch without its first
// letter.
std::string WiiMagicOf(FileKind kind)
{
    switch (kind)
    {
    case FileKind::kBank:
        return "RBNK";
    case FileKind::kWaveArchive:
        return "RWAR";
    case FileKind::kWaveSoundFile:
        break;
    }

    return "RWSD";
}

// Whether `bytes`, a bank file or a wave sound file of the Wii, describe their waves in a WAVE block. Nothing when
// ntdlib can't read them.
std::optional<bool> HasWaveBlock(std::span<const uint8_t> bytes, FileKind kind)
{
    try
    {
        return kind == FileKind::kBank
                   ? rsar::Bank::Parse(bytes, ntdlib::rvl::snd::kRsarOptions).HasWaveBlock()
                   : rsar::WaveSoundFile::Parse(bytes, ntdlib::rvl::snd::kRsarOptions).HasWaveBlock();
    }
    catch (const std::exception&)
    {
        return std::nullopt;
    }
}

// The keys and the velocities of MIDI: 0 to 127.
constexpr int kMidiValues = 128;

// The waves that the regions of `bank` play, by their indices in its wave data, in order, each once.
std::vector<uint32_t> WavesOfBank(const rsar::Bank& bank)
{
    std::set<uint32_t> waves;
    for (uint32_t program = 0; program < bank.InstrumentCount(); program++)
    {
        if (!bank.Plays(program))
        {
            continue;
        }

        for (int key = 0; key < kMidiValues; key++)
        {
            for (int velocity = 0; velocity < kMidiValues; velocity++)
            {
                const rsar::Region* region = bank.Find(program, key, velocity);
                if (region != nullptr && region->wave_index >= 0)
                {
                    waves.insert(static_cast<uint32_t>(region->wave_index));
                }
            }
        }
    }

    return {waves.begin(), waves.end()};
}

// The waves that the notes of the wave sounds of `file` play, by their indices in its wave data, in order, each once.
std::vector<uint32_t> WavesOfWaveSounds(const rsar::WaveSoundFile& file)
{
    std::set<uint32_t> waves;
    for (const rsar::WaveSoundFile::WaveSound& sound : file.Sounds())
    {
        for (const rsar::WaveSoundFile::Note& note : sound.notes)
        {
            if (note.wave_index >= 0)
            {
                waves.insert(static_cast<uint32_t>(note.wave_index));
            }
        }
    }

    return {waves.begin(), waves.end()};
}

// The waves of `bytes`, a bank file or a wave sound file of the Wii, and how many of them `wave_data` holds: nothing
// when ntdlib can't read them from it, or without `wave_data`.
std::pair<std::vector<uint32_t>, std::optional<std::size_t>> WavesIn(
    std::span<const uint8_t> bytes, FileKind kind, std::optional<std::span<const uint8_t>> wave_data = std::nullopt)
{
    std::vector<uint32_t> waves;
    std::optional<std::size_t> count;
    try
    {
        if (kind == FileKind::kBank)
        {
            waves = WavesOfBank(rsar::Bank::Parse(bytes, ntdlib::rvl::snd::kRsarOptions));
            if (wave_data)
            {
                count = rsar::Bank::Parse(bytes, *wave_data, ntdlib::rvl::snd::kRsarOptions).Waves().size();
            }
        }
        else
        {
            waves = WavesOfWaveSounds(rsar::WaveSoundFile::Parse(bytes, ntdlib::rvl::snd::kRsarOptions));
            if (wave_data)
            {
                count = rsar::WaveSoundFile::Parse(bytes, *wave_data, ntdlib::rvl::snd::kRsarOptions).Waves().size();
            }
        }
    }
    catch (const std::exception&)
    {
        count.reset();
    }

    return {waves, count};
}

// Throws std::invalid_argument with a message for the user when ntdlib can't read `bytes`, an RWAR, and each of its
// waves.
void ReadWiiWaveArchive(std::span<const uint8_t> bytes)
{
    try
    {
        for (const std::span<const uint8_t> wave :
             rsar::WaveArchive::Parse(bytes, ntdlib::rvl::snd::kRsarOptions).waves)
        {
            rsar::Wave::ParseFile(wave, ntdlib::rvl::snd::kRsarOptions);
        }
    }
    catch (const std::exception& error)
    {
        throw std::invalid_argument(std::string("Can't read the wave archive file. ") +
                                    format::AsSentence(error.what()));
    }
}

// `bytes`, a wave archive file of `flavor`, as an RWAR of the same waves (ntdlib::rsar::WiiWaveArchiveOf).
IncomingFile WiiWaveArchiveFrom(std::span<const uint8_t> bytes, sar::Flavor flavor)
{
    const IncomingFile read = ReadIncomingFile(bytes, FileKind::kWaveArchive, flavor);
    IncomingFile incoming{FileKind::kWaveArchive, 0, {}, {}, {}};
    try
    {
        const rsar::WaveArchiveFile converted = rsar::WiiWaveArchiveOf(sar::WaveArchiveFile::Read(read.bytes));
        incoming.version = converted.version;
        incoming.bytes = converted.Write();
    }
    catch (const std::exception& error)
    {
        throw std::invalid_argument(std::string("Can't convert it for the Wii. ") + format::AsSentence(error.what()));
    }

    incoming.converted = ConvertedFile{
        .flavor = flavor, .version = read.version, .size = read.bytes.size(), .wave_version = rsar::WaveFile().version};
    ReadWiiWaveArchive(incoming.bytes);

    return incoming;
}

// Whether sound `sound` of `archive` is a sequence sound with a bank of file `file_id`, or a wave sound of the file.
bool PlaysFile(const rsar::SoundArchive& archive, uint32_t sound, uint32_t file_id)
{
    const rsar::SoundInfo& info = archive.Sounds()[sound];
    if (info.type == rsar::SoundType::kSequence)
    {
        return info.bank < archive.Banks().size() && archive.Banks()[info.bank].file_id == file_id;
    }

    return info.type == rsar::SoundType::kWaveSound && info.file_id == file_id;
}

} // namespace

IncomingFile ReadWiiIncomingFile(std::span<const uint8_t> bytes, FileKind kind)
{
    const auto starts = [&bytes](const std::string& with)
    {
        return bytes.size() >= with.size() && std::equal(with.begin(), with.end(), bytes.begin());
    };

    // The magic gives the kind. The other consoles' files of the kind take another first letter. Their wave archives go
    // in converted.
    const std::string magic = WiiMagicOf(kind);
    if (!starts(magic))
    {
        const std::optional<sar::Flavor> later = LaterConsoleOf(bytes, kind);
        if (later && kind == FileKind::kWaveArchive)
        {
            return WiiWaveArchiveFrom(bytes, *later);
        }

        throw std::invalid_argument(later ? "It's a " + FileText(kind) +
                                                " of the 3DS, the Wii U or the Switch. To convert it, choose wave data "
                                                "of the archive for each of its wave archives."
                                          : format::NotKindText(bytes, "a " + FileText(kind) + " of the Wii"));
    }

    if (bytes.size() < kWiiHeaderSize || !MarksBigEndian(bytes))
    {
        throw std::invalid_argument(format::NotKindText(bytes, "a " + FileText(kind) + " of the Wii"));
    }

    // The header's size.
    const xsflib::Reader r(bytes, true);
    const uint32_t size = r.U32(kWiiSizeField);
    if (size < kWiiHeaderSize || size > bytes.size())
    {
        throw std::invalid_argument("Its header gives a size of " + std::to_string(size) + " bytes, but the file has " +
                                    std::to_string(bytes.size()) + " bytes.");
    }

    IncomingFile incoming{
        kind, r.U16(kWiiVersionField), std::vector<uint8_t>(bytes.begin(), bytes.begin() + size), {}, {}};

    // ReadWiiWaveArchive throws with the whole message for a wave archive.
    if (kind == FileKind::kWaveArchive)
    {
        ReadWiiWaveArchive(incoming.bytes);

        return incoming;
    }

    try
    {
        switch (kind)
        {
        case FileKind::kBank:
            rsar::Bank::Parse(incoming.bytes, ntdlib::rvl::snd::kRsarOptions);
            break;
        case FileKind::kWaveArchive:
            break;
        case FileKind::kWaveSoundFile:
            rsar::WaveSoundFile::Parse(incoming.bytes, ntdlib::rvl::snd::kRsarOptions);
            break;
        }
    }
    catch (const std::exception& error)
    {
        throw std::invalid_argument("Can't read the " + FileText(kind) + ". " + format::AsSentence(error.what()));
    }

    return incoming;
}

uint16_t NewWiiFileVersion(const rsar::SoundArchive& archive, FileKind kind)
{
    // The version of the file of the archive's first bank, or of its first wave sound, that ntdlib reads.
    const auto first = [&archive](FileKind of) -> std::optional<uint16_t>
    {
        std::vector<uint32_t> files;
        for (const rsar::BankInfo& bank : archive.Banks())
        {
            if (of == FileKind::kBank)
            {
                files.push_back(bank.file_id);
            }
        }

        for (const rsar::SoundInfo& sound : archive.Sounds())
        {
            if (of == FileKind::kWaveSoundFile && sound.type == rsar::SoundType::kWaveSound)
            {
                files.push_back(sound.file_id);
            }
        }

        for (const uint32_t id : files)
        {
            const std::span<const uint8_t> bytes = archive.FileData(id);
            try
            {
                if (!bytes.empty())
                {
                    return of == FileKind::kBank
                               ? rsar::Bank::Parse(bytes, ntdlib::rvl::snd::kRsarOptions).Version()
                               : rsar::WaveSoundFile::Parse(bytes, ntdlib::rvl::snd::kRsarOptions).Version();
                }
            }
            catch (const std::exception&)
            {
                // The next file gives the version.
            }
        }

        return std::nullopt;
    };
    switch (kind)
    {
    case FileKind::kBank:
        return first(kind).value_or(archive.Version() >= 0x0104 ? 0x0102 : 0x0101);
    case FileKind::kWaveArchive:
        return rsar::WaveArchiveFile().version;
    case FileKind::kWaveSoundFile:
        break;
    }

    // A wave sound file of version 1.3 keeps its waves in an RWAR. So does a bank of version 1.2.
    return first(kind).value_or(NewWiiFileVersion(archive, FileKind::kBank) >= 0x0102 ? 0x0103 : 0x0102);
}

uint16_t PresentWiiVersion(const rsar::SoundArchive& archive, FileKind kind, uint32_t item)
{
    const std::span<const uint8_t> present = archive.FileData(FileIdOf(archive, kind, item));
    return present.size() >= kWiiHeaderSize ? xsflib::LoadBe16(&present[kWiiVersionField])
                                            : NewWiiFileVersion(archive, kind);
}

IncomingFile WiiFileOf(const IncomingFile& incoming, sar::Flavor flavor, const rsar::SoundArchive& archive,
                       const std::map<uint32_t, uint32_t>& map, uint16_t version)
{
    // The table of waves of the bank or the wave sound file.
    const bool bank = incoming.kind == FileKind::kBank;
    sar::Options options;
    options.flavor = flavor;
    sar::WaveSoundFile wave_sounds;
    std::vector<sar::WaveId> table;
    try
    {
        if (bank)
        {
            table = sar::Bank::Parse(incoming.bytes, options).waves;
        }
        else
        {
            wave_sounds = sar::WaveSoundFile::Parse(incoming.bytes, options);
            table = wave_sounds.waves;
        }
    }
    catch (const std::exception& error)
    {
        throw std::invalid_argument("Can't read the " + NameOf(incoming.kind) + ". " +
                                    format::AsSentence(error.what()));
    }

    // Each entry takes the wave at its index in the wave data that takes its wave archive's place. Each file's waves
    // are read once.
    std::map<uint32_t, std::vector<rsar::WaveFile>> held;
    std::vector<std::optional<rsar::WaveFile>> waves;
    for (const sar::WaveId& wave : table)
    {
        const auto target = map.find(wave.wave_archive_item);
        if (target == map.end())
        {
            throw std::invalid_argument("Choose wave data of the archive for wave archive " +
                                        std::to_string(sar::ItemNumber(wave.wave_archive_item)) + ".");
        }

        auto [place, added] = held.try_emplace(target->second);
        if (added)
        {
            place->second = WavesOf(archive, target->second);
        }

        waves.push_back(wave.index < place->second.size() ? std::optional(place->second[wave.index]) : std::nullopt);
    }

    // The file of the Wii and its wave data.
    IncomingFile result{
        incoming.kind, version, {}, ConvertedFile{flavor, incoming.version, incoming.bytes.size(), {}}, {}};
    try
    {
        if (bank)
        {
            rsar::BankFile::Files files = WiiBankOfFile(incoming.bytes, flavor, version, waves).Write();
            result.bytes = std::move(files.file);
            result.wave_data = std::move(files.wave_data);
        }
        else
        {
            rsar::WsdFile::Files files = rsar::WiiWsdOf(wave_sounds, version, waves).Write();
            result.bytes = std::move(files.file);
            result.wave_data = std::move(files.wave_data);
        }
    }
    catch (const std::exception& error)
    {
        throw std::invalid_argument(std::string("Can't convert it for the Wii. ") + format::AsSentence(error.what()));
    }

    return result;
}

WiiWaveData ReadWiiWaveData(std::span<const uint8_t> bytes)
{
    const IncomingFile read = ReadWiiIncomingFile(bytes, FileKind::kWaveArchive);
    if (read.converted)
    {
        throw std::invalid_argument("It isn't a wave archive file of the Wii.");
    }

    WiiWaveData data{read.bytes, {}, true};
    try
    {
        for (const std::vector<uint8_t>& wave : rsar::WaveArchiveFile::Read(read.bytes).waves)
        {
            data.waves.push_back(rsar::WaveFile::Read(wave));
        }
    }
    catch (const std::exception& error)
    {
        throw std::invalid_argument(std::string("Can't read the waves in the wave archive file. ") +
                                    format::AsSentence(error.what()));
    }

    return data;
}

WiiWaveData WiiWaveDataOf(const rsar::SoundArchive& archive, uint32_t file_id)
{
    const std::optional<rsar::FileData> data = archive.File(file_id);
    if (!data || data->wave_data.empty())
    {
        throw std::invalid_argument("The archive has no wave data for file " + std::to_string(file_id) + ".");
    }

    return {{data->wave_data.begin(), data->wave_data.end()},
            WavesOf(archive, file_id),
            data->wave_data.size() >= 4 && std::ranges::equal(data->wave_data.first(4), std::string_view("RWAR"))};
}

IncomingFile WiiFileWithWaves(const IncomingFile& incoming, const WiiWaveData& data, uint16_t version)
{
    // The wave data in the file's form: the bytes as they are, or the waves laid out as the tools lay them out.
    const std::string kind = NameOf(incoming.kind);
    const std::optional<bool> block = HasWaveBlock(incoming.bytes, incoming.kind);
    if (!block)
    {
        throw std::invalid_argument("Can't read the " + kind + ".");
    }

    IncomingFile result = incoming;
    if (*block != data.rwar)
    {
        result.wave_data = data.bytes;
    }
    else if (*block)
    {
        result.wave_data = rsar::RawWaveData(data.waves);
    }
    else
    {
        rsar::WaveArchiveFile rwar;
        for (const rsar::WaveFile& wave : data.waves)
        {
            rwar.waves.push_back(wave.Write());
        }

        result.wave_data = rwar.Write();
    }
    if (const std::string problem = WiiWaveDataProblem(result.bytes, result.kind, result.wave_data); !problem.empty())
    {
        throw std::invalid_argument(problem);
    }

    // ntdlib's model of the file reads it with the wave data. A later version than `version` goes in written again.
    rsar::BankFile bank;
    rsar::WsdFile wave_sounds;
    try
    {
        if (incoming.kind == FileKind::kBank)
        {
            bank = rsar::BankFile::Read(result.bytes, result.wave_data);
        }
        else
        {
            wave_sounds = rsar::WsdFile::Read(result.bytes, result.wave_data);
        }
    }
    catch (const std::exception& error)
    {
        throw std::invalid_argument(
            *block ? "The WAVE block of the " + kind + " doesn't match these waves. " + format::AsSentence(error.what())
                   : "Can't read the " + kind + " with these waves. " + format::AsSentence(error.what()));
    }

    if (version >= incoming.version)
    {
        return result;
    }

    try
    {
        bank.version = version;
        wave_sounds.version = version;
        if (incoming.kind == FileKind::kBank)
        {
            rsar::BankFile::Files files = bank.Write();
            result.bytes = std::move(files.file);
            result.wave_data = std::move(files.wave_data);
        }
        else
        {
            rsar::WsdFile::Files files = wave_sounds.Write();
            result.bytes = std::move(files.file);
            result.wave_data = std::move(files.wave_data);
        }
    }
    catch (const std::invalid_argument& error)
    {
        throw std::invalid_argument("Can't write the " + kind + " in version " + std::to_string(version >> 8) + "." +
                                    std::to_string(version & 0xFF) + ". " + format::AsSentence(error.what()));
    }

    result.version = version;
    result.converted = ConvertedFile{.version = incoming.version, .size = incoming.bytes.size(), .wii = true};

    return result;
}

std::string WiiWaveDataProblem(std::span<const uint8_t> bytes, FileKind kind, std::span<const uint8_t> wave_data)
{
    const auto [waves, held] = WavesIn(bytes, kind, wave_data);
    const std::string file = kind == FileKind::kBank ? "bank" : "wave sound file";
    if (!held)
    {
        return "Can't read the waves of the " + file + " from its wave data.";
    }
    if (!waves.empty() && waves.back() >= *held)
    {
        return "The " + file + " refers to wave " + std::to_string(waves.back()) + ", but its wave data has only " +
               std::to_string(*held) + (*held == 1 ? " wave." : " waves.");
    }

    return {};
}

uint32_t FileIdOf(const rsar::SoundArchive& archive, FileKind kind, uint32_t item)
{
    switch (kind)
    {
    case FileKind::kBank:
        if (item >= archive.Banks().size())
        {
            throw std::invalid_argument("The archive has no such bank.");
        }

        return archive.Banks()[item].file_id;

    case FileKind::kWaveArchive:
        if (archive.WaveData(item).empty())
        {
            throw std::invalid_argument("The archive has no such wave archive.");
        }

        return item;

    case FileKind::kWaveSoundFile:
        break;
    }

    if (item >= archive.Sounds().size() || archive.Sounds()[item].type != rsar::SoundType::kWaveSound)
    {
        throw std::invalid_argument("The sound isn't a wave sound.");
    }

    return archive.Sounds()[item].file_id;
}

namespace
{

// Adds to `changes` the banks other than bank `item` whose file is `id`, the sounds that play the file, and the
// programs of `present`, the bank now, that are missing from `incoming` (ChangesOf).
void WiiBankChanges(const rsar::SoundArchive& archive, uint32_t item, uint32_t id, std::span<const uint8_t> present,
                    const IncomingFile& incoming, FileChanges& changes)
{
    // The file's other banks, and the sounds that play them.
    for (uint32_t i = 0; i < archive.Banks().size(); i++)
    {
        if (i != item && archive.Banks()[i].file_id == id)
        {
            changes.sharing.push_back(i);
        }
    }

    for (uint32_t sound = 0; sound < archive.Sounds().size(); sound++)
    {
        if (archive.Sounds()[sound].type == rsar::SoundType::kSequence && PlaysFile(archive, sound, id))
        {
            changes.players.push_back(sound);
        }
    }

    // The present bank's programs that the new one lacks.
    try
    {
        const rsar::Bank before = rsar::Bank::Parse(present, ntdlib::rvl::snd::kRsarOptions);
        const rsar::Bank after = rsar::Bank::Parse(incoming.bytes, ntdlib::rvl::snd::kRsarOptions);
        for (uint32_t program = 0; program < before.InstrumentCount(); program++)
        {
            if (before.Plays(program) && (program >= after.InstrumentCount() || !after.Plays(program)))
            {
                changes.programs.push_back(program);
            }
        }
    }
    catch (const std::exception&)
    {
        // A bank that ntdlib can't read plays nothing.
    }
}

// Adds to `changes` the wave sounds of wave sound file `id` other than sound `item`, and those past the end of
// `incoming` (ChangesOf).
void WiiWaveSoundChanges(const rsar::SoundArchive& archive, uint32_t item, uint32_t id, const IncomingFile& incoming,
                         FileChanges& changes)
{
    // The file's other wave sounds, and those past the new file's end.
    std::size_t count = 0;
    try
    {
        count = rsar::WaveSoundFile::Parse(incoming.bytes, ntdlib::rvl::snd::kRsarOptions).Sounds().size();
    }
    catch (const std::exception&)
    {
        count = 0;
    }

    changes.count = count;
    for (uint32_t sound = 0; sound < archive.Sounds().size(); sound++)
    {
        const rsar::SoundInfo& info = archive.Sounds()[sound];
        if (info.type != rsar::SoundType::kWaveSound || info.file_id != id)
        {
            continue;
        }

        if (sound != item)
        {
            changes.sharing.push_back(sound);
        }

        if (info.sub_index < 0 || static_cast<std::size_t>(info.sub_index) >= count)
        {
            changes.past_sounds.push_back(sound);
        }
    }
}

// Adds to `changes` the banks and the wave sounds of wave archive `id` that name waves past the end of `incoming`
// (ChangesOf).
void WiiWaveArchiveChanges(const rsar::SoundArchive& archive, uint32_t id, const IncomingFile& incoming,
                           FileChanges& changes)
{
    // The banks and the wave sounds of the file that name waves past the new RWAR's end.
    try
    {
        changes.count = rsar::WaveArchive::Parse(incoming.bytes, ntdlib::rvl::snd::kRsarOptions).waves.size();
    }
    catch (const std::exception&)
    {
        changes.count = 0;
    }

    const std::span<const uint8_t> owner = archive.FileData(id);
    const bool bank =
        std::ranges::any_of(archive.Banks(), [id](const rsar::BankInfo& info) { return info.file_id == id; });
    const std::vector<uint32_t> waves = WavesIn(owner, bank ? FileKind::kBank : FileKind::kWaveSoundFile).first;
    if (!waves.empty() && waves.back() >= changes.count)
    {
        for (uint32_t i = 0; i < archive.Banks().size() && bank; i++)
        {
            if (archive.Banks()[i].file_id == id)
            {
                changes.past_banks.push_back(i);
            }
        }

        for (uint32_t sound = 0; sound < archive.Sounds().size() && !bank; sound++)
        {
            if (archive.Sounds()[sound].type == rsar::SoundType::kWaveSound && PlaysFile(archive, sound, id))
            {
                changes.past_sounds.push_back(sound);
            }
        }
    }
}

// Adds to `changes` the waves of wave archive `id` that `incoming` names and its wave data doesn't hold (ChangesOf).
// `wave_data` contains the wave data now.
void AddLackedWaves(uint32_t id, std::span<const uint8_t> wave_data, const IncomingFile& incoming, FileChanges& changes)
{
    // The waves that the new file names and its wave data lacks: the present wave data, or the wave data that comes
    // with it.
    const auto [waves, count] =
        WavesIn(incoming.bytes, incoming.kind, incoming.wave_data.empty() ? wave_data : incoming.wave_data);
    LackedWaves lacked{.wave_archive = id, .whole = !count, .count = count.value_or(0), .waves = {}};
    for (const uint32_t wave : waves)
    {
        if (!count || wave >= *count)
        {
            lacked.waves.push_back(wave);
        }
    }
    if (lacked.whole || !lacked.waves.empty())
    {
        changes.lacked.push_back(std::move(lacked));
    }
}

} // namespace

FileChanges ChangesOf(const format::File& /*file*/, const rsar::SoundArchive& archive, uint32_t item,
                      const IncomingFile& incoming)
{
    // The present file, and its version and size from its header.
    const uint32_t id = FileIdOf(archive, incoming.kind, item);
    const std::span<const uint8_t> wave_data = archive.WaveData(id);
    const std::span<const uint8_t> present = incoming.kind == FileKind::kWaveArchive ? wave_data : archive.FileData(id);
    const bool header = incoming.kind != FileKind::kWaveArchive ||
                        (present.size() >= 4 && std::ranges::equal(present.first(4), std::string_view("RWAR")));
    FileChanges changes;
    changes.size = present.size();
    if (present.size() >= kWiiHeaderSize && header)
    {
        const xsflib::Reader r(present, true);
        changes.version = r.U16(kWiiVersionField);
        changes.size = std::min<std::size_t>(r.U32(kWiiSizeField), present.size());
    }

    if (incoming.kind != FileKind::kWaveArchive)
    {
        changes.wave_block = HasWaveBlock(present, incoming.kind);
    }

    switch (incoming.kind)
    {
    case FileKind::kBank:
        WiiBankChanges(archive, item, id, present, incoming, changes);
        break;

    case FileKind::kWaveSoundFile:
        WiiWaveSoundChanges(archive, item, id, incoming, changes);
        break;

    case FileKind::kWaveArchive:
        WiiWaveArchiveChanges(archive, id, incoming, changes);
        return changes;
    }

    AddLackedWaves(id, wave_data, incoming, changes);

    return changes;
}

format::File ReplaceWiiFile(const format::File& file, const rsar::SoundArchive& archive, uint32_t item,
                            const IncomingFile& incoming)
{
    const uint32_t id = FileIdOf(archive, incoming.kind, item);
    const std::span<const uint8_t> present = archive.FileData(id);
    const bool bank =
        std::ranges::any_of(archive.Banks(), [id](const rsar::BankInfo& info) { return info.file_id == id; });
    const FileKind owner =
        incoming.kind == FileKind::kWaveArchive ? (bank ? FileKind::kBank : FileKind::kWaveSoundFile) : incoming.kind;
    const std::optional<bool> block = HasWaveBlock(present, owner);
    if (incoming.kind == FileKind::kWaveArchive && block.value_or(false))
    {
        // A file that describes its waves in a WAVE block describes the RWAR's waves there.
        std::vector<rsar::WaveFile> waves;
        try
        {
            for (const std::vector<uint8_t>& wave : rsar::WaveArchiveFile::Read(incoming.bytes).waves)
            {
                waves.push_back(rsar::WaveFile::Read(wave));
            }
        }
        catch (const std::exception& error)
        {
            throw std::invalid_argument(std::string("Can't read the waves in the wave archive. ") +
                                        format::AsSentence(error.what()));
        }

        return WithWaves(file, archive, id, std::move(waves));
    }

    if (incoming.kind == FileKind::kWaveArchive)
    {
        return ReplaceFile(file, id, present, incoming.bytes);
    }
    if (!incoming.wave_data.empty())
    {
        return ReplaceFile(file, id, incoming.bytes, incoming.wave_data);
    }

    // The new file keeps its waves as the present one does.
    const std::optional<bool> incoming_block = HasWaveBlock(incoming.bytes, incoming.kind);
    if (block && incoming_block && *block != *incoming_block)
    {
        const std::string kind = NameOf(incoming.kind);
        const std::string current = *block ? "describes its waves in a WAVE block, over samples beside it"
                                           : "keeps its waves in an RWAR beside it";
        const std::string next = *block ? "keeps its waves in an RWAR" : "describes its waves in a WAVE block";
        throw std::invalid_argument("The current " + kind + " " + current + ". The new " + kind + " " + next +
                                    ". Can't replace one with the other.");
    }

    return ReplaceFile(file, id, incoming.bytes);
}

std::vector<uint32_t> SoundsPlaying(const rsar::SoundArchive& archive, FileKind kind, uint32_t item)
{
    std::vector<uint32_t> sounds;
    uint32_t id = 0;
    try
    {
        id = FileIdOf(archive, kind, item);
    }
    catch (const std::invalid_argument&)
    {
        return sounds;
    }

    for (uint32_t sound = 0; sound < archive.Sounds().size(); sound++)
    {
        const bool plays = PlaysFile(archive, sound, id);
        const bool kind_plays =
            kind == FileKind::kWaveArchive ||
            (kind == FileKind::kBank) == (archive.Sounds()[sound].type == rsar::SoundType::kSequence);
        if (plays && kind_plays)
        {
            sounds.push_back(sound);
        }
    }

    return sounds;
}

WaveChanges ChangesOfWiiWave(const rsar::SoundArchive& archive, uint32_t file_id, uint32_t wave)
{
    const rsar::WaveFile present = PresentWave(archive, file_id, wave);
    WaveChanges changes;
    changes.encoding = sar::EncodingOf(present.encoding);
    changes.channels = present.channels.size();
    changes.rate = present.sample_rate;
    changes.loop = present.loop;
    changes.loop_start = present.loop_start;
    changes.loop_end = present.loop_end;
    changes.size = present.Write().size();

    // The banks of the file whose regions name the wave, and the wave sounds of the file whose notes play it. A file
    // that ntdlib can't read names nothing.
    const std::span<const uint8_t> bytes = archive.FileData(file_id);
    const std::vector<uint32_t> named = WavesIn(bytes, FileKind::kBank).first;
    for (uint32_t bank = 0; bank < archive.Banks().size(); bank++)
    {
        if (archive.Banks()[bank].file_id == file_id && std::ranges::find(named, wave) != named.end())
        {
            changes.banks.push_back(bank);
        }
    }

    for (uint32_t sound = 0; sound < archive.Sounds().size(); sound++)
    {
        const rsar::SoundInfo& info = archive.Sounds()[sound];
        if (info.type != rsar::SoundType::kWaveSound || info.file_id != file_id || info.sub_index < 0)
        {
            continue;
        }

        try
        {
            const rsar::WaveSoundFile read = rsar::WaveSoundFile::Parse(bytes, ntdlib::rvl::snd::kRsarOptions);
            const auto index = static_cast<std::size_t>(info.sub_index);
            const auto plays = [wave](const rsar::WaveSoundFile::Note& note)
            {
                return note.wave_index >= 0 && static_cast<uint32_t>(note.wave_index) == wave;
            };
            if (index < read.Sounds().size() && std::ranges::any_of(read.Sounds()[index].notes, plays))
            {
                changes.sounds.push_back(sound);
            }
        }
        catch (const std::exception&)
        {
            // A wave sound file that ntdlib can't read names no wave.
        }
    }

    return changes;
}

format::File WithIncomingWiiWave(const format::File& file, const rsar::SoundArchive& archive, uint32_t file_id,
                                 std::optional<uint32_t> wave, const IncomingWave& incoming)
{
    std::vector<rsar::WaveFile> waves = WavesOf(archive, file_id);
    if (wave && *wave >= waves.size())
    {
        throw std::invalid_argument("The wave data has no wave " + std::to_string(*wave) + ".");
    }

    rsar::WaveFile read;
    try
    {
        read = rsar::WaveFile::Read(incoming.bytes);
    }
    catch (const std::exception& error)
    {
        throw std::invalid_argument(std::string("Can't read the new wave. ") + format::AsSentence(error.what()));
    }

    // The new wave takes the present wave's version, or that of the first wave.
    if (wave || !waves.empty())
    {
        read.version = waves[wave.value_or(0)].version;
    }

    if (wave)
    {
        waves[*wave] = std::move(read);
    }
    else
    {
        waves.push_back(std::move(read));
    }

    return WithWaves(file, archive, file_id, std::move(waves));
}

} // namespace hokuto::edit
