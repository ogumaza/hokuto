// SPDX-License-Identifier: MIT

#include "edit/files.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <map>
#include <memory>
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
#include "ntdlib/rsar/layout.h"
#include "ntdlib/rsar/wave_file.h"
#include "ntdlib/rsar/wsd_file.h"
#include "ntdlib/rvl/snd/rsar_options.h"
#include "ntdlib/sar/bank_edit.h"
#include "ntdlib/sar/byte_order.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sar/layout.h"
#include "ntdlib/sar/wave_file.h"
#include "ntdlib/wave/adpcm.h"
#include "ntdlib/wave/encode.h"
#include "seqlib/read_error.h"
#include "seqlib/wav.h"
#include "xsflib/binary.h"
#include "xsflib/bytes.h"

#include "audio/resampler.h"
#include "edit/bytecode.h"
#include "edit/other_console.h"
#include "edit/waves.h"
#include "format/archive.h"
#include "format/kinds.h"
#include "song/archive.h"

namespace hokuto::edit
{
namespace
{

namespace sar = ntdlib::sar;
namespace wave = ntdlib::wave;

// An item ID holds the item's kind in its top byte and its place in its list below.
constexpr uint32_t kIndexBits = 0x00FFFFFF;
constexpr uint32_t kBankKind = 0x03;
constexpr uint32_t kWaveArchiveKind = 0x05;

// A NintendoWare file's header: the magic, the byte order mark, the header's size, the version at 0x08, the file's size
// at 0x0C and the number of blocks at 0x10, then the blocks' table.
constexpr std::size_t kVersionField = 0x08;
constexpr std::size_t kSizeField = 0x0C;
constexpr std::size_t kHeaderSize = 0x14;

// The most bytes of samples that a wave file takes. Its references hold signed 32-bit offsets.
constexpr std::size_t kMostWaveBytes = 0x7FFFFFFF;

// The samples of a frame of DSP-ADPCM.
constexpr std::size_t kFrameSamples = 14;

// The letters after the console's letter in the magic of a file of `kind`.
std::string MagicOf(FileKind kind)
{
    switch (kind)
    {
    case FileKind::kBank:
        return "BNK";
    case FileKind::kWaveArchive:
        return "WAR";
    case FileKind::kWaveSoundFile:
        break;
    }

    return "WSD";
}

// The waves of each of the archive's wave archives, or nothing for one that ntdlib can't read.
std::vector<std::optional<std::size_t>> WaveCounts(const sar::SoundArchive& archive)
{
    std::vector<std::optional<std::size_t>> counts;
    for (const sar::WaveArchiveInfo& wave_archive : archive.WaveArchives())
    {
        std::optional<std::size_t>& count = counts.emplace_back();
        try
        {
            count = sar::WaveArchive::Parse(archive.FileData(wave_archive.file_id), archive.ReadOptions()).waves.size();
        }
        catch (const std::exception&)
        {
            // The archive lacks the file, or ntdlib can't read it.
        }
    }

    return counts;
}

// Whether `wave` names a wave of wave archive `wave_archive`.
bool Names(const sar::WaveId& wave, uint32_t wave_archive)
{
    return wave.wave_archive_item >> 24 == kWaveArchiveKind && (wave.wave_archive_item & kIndexBits) == wave_archive;
}

// The waves of `waves` that the archive lacks, by wave archive in order. `counts` gives the waves of each of the
// archive's wave archives (WaveCounts). A wave that names an item of another kind counts as naming a wave archive that
// the archive lacks.
std::vector<LackedWaves> Lacked(std::span<const sar::WaveId> waves,
                                const std::vector<std::optional<std::size_t>>& counts)
{
    std::map<uint32_t, LackedWaves> lacked;
    for (const sar::WaveId& wave : waves)
    {
        const uint32_t index = wave.wave_archive_item & kIndexBits;
        const bool known = wave.wave_archive_item >> 24 == kWaveArchiveKind && index < counts.size() && counts[index];
        if (known && wave.index < *counts[index])
        {
            continue;
        }

        LackedWaves& entry = lacked[index];
        entry.wave_archive = index;
        entry.whole = entry.whole || !known;
        entry.count = known ? *counts[index] : 0;
        if (known)
        {
            entry.waves.push_back(wave.index);
        }
    }

    std::vector<LackedWaves> list;
    for (auto& [index, entry] : lacked)
    {
        std::ranges::sort(entry.waves);
        entry.waves.erase(std::ranges::unique(entry.waves).begin(), entry.waves.end());
        list.push_back(std::move(entry));
    }

    return list;
}

// The waves that wave sound `sound` of `file` plays: the waves of its notes.
std::vector<sar::WaveId> WavesOf(const sar::WaveSoundFile& file, uint32_t sound)
{
    std::vector<sar::WaveId> waves;
    if (sound >= file.sounds.size())
    {
        return waves;
    }

    for (const sar::VelocityRegion& note : file.sounds[sound].notes)
    {
        if (note.wave_id_index < file.waves.size())
        {
            waves.push_back(file.waves[note.wave_id_index]);
        }
    }

    return waves;
}

// The banks of `archive` that name a wave for which `named` holds.
template <class Named>
std::vector<uint32_t> BanksNaming(const sar::SoundArchive& archive, Named named)
{
    std::vector<uint32_t> banks;
    for (std::size_t i = 0; i < archive.Banks().size(); i++)
    {
        std::vector<sar::WaveId> waves;
        try
        {
            waves = sar::Bank::Parse(archive.FileData(archive.Banks()[i].file_id), archive.ReadOptions()).waves;
        }
        catch (const std::exception&)
        {
            // The bank names nothing that Hokuto can read.
        }

        if (std::ranges::any_of(waves, named))
        {
            banks.push_back(static_cast<uint32_t>(i));
        }
    }

    return banks;
}

// The wave sounds of `archive` whose notes play a wave for which `named` holds.
template <class Named>
std::vector<uint32_t> WaveSoundsPlaying(const sar::SoundArchive& archive, Named named)
{
    // Each wave sound file once.
    std::map<uint32_t, std::optional<sar::WaveSoundFile>> files;
    std::vector<uint32_t> sounds;
    for (std::size_t i = 0; i < archive.Sounds().size(); i++)
    {
        const sar::SoundInfo& sound = archive.Sounds()[i];
        if (sound.type != sar::SoundType::kWaveSound)
        {
            continue;
        }

        const auto [found, added] = files.try_emplace(sound.file_id);
        if (added)
        {
            try
            {
                found->second = sar::WaveSoundFile::Parse(archive.FileData(sound.file_id), archive.ReadOptions());
            }
            catch (const std::exception&)
            {
                // The file names nothing that Hokuto can read.
            }
        }

        if (found->second && std::ranges::any_of(WavesOf(*found->second, sound.wave_sound_index), named))
        {
            sounds.push_back(static_cast<uint32_t>(i));
        }
    }

    return sounds;
}

// The file of wave archive `wave_archive` of `archive`, read for writing. Throws std::invalid_argument with a message
// for the user for an archive without the wave archive, and for a file that Hokuto can't read.
sar::WaveArchiveFile WaveArchiveFileOf(const sar::SoundArchive& archive, uint32_t wave_archive)
{
    const uint32_t file_id = FileIdOf(archive, FileKind::kWaveArchive, wave_archive);
    try
    {
        return sar::WaveArchiveFile::Read(archive.FileData(file_id));
    }
    catch (const std::exception& error)
    {
        throw std::invalid_argument(std::string("Hokuto can't read the wave archive. ") +
                                    format::AsSentence(error.what()));
    }
}

// Throws std::invalid_argument with a message for the user when `file` holds no wave `wave`.
void CheckWave(const sar::WaveArchiveFile& file, uint32_t wave)
{
    if (wave >= file.waves.size())
    {
        throw std::invalid_argument("The wave archive holds " + std::to_string(file.waves.size()) +
                                    (file.waves.size() == 1 ? " wave" : " waves") + ". It has no wave " +
                                    std::to_string(wave) + ".");
    }
}

// Wave `wave` of `file`, read for writing. Throws std::invalid_argument with a message for the user when `file` holds
// no such wave, and for a wave that Hokuto can't read.
sar::WaveFile WaveFileOf(const sar::WaveArchiveFile& file, uint32_t wave)
{
    CheckWave(file, wave);
    try
    {
        return sar::WaveFile::Read(file.waves[wave]);
    }
    catch (const std::exception& error)
    {
        throw std::invalid_argument(std::string("Hokuto can't read the present wave. ") +
                                    format::AsSentence(error.what()));
    }
}

// Puts `channels`, each `incoming.frames` long, into `file` as 16-bit PCM, big-endian with `big_endian`: a wave file of
// the 3DS, the Wii U or the Switch (ntdlib::sar::WaveFile), or of the Wii (ntdlib::rsar::WaveFile). Throws
// std::invalid_argument with a message for the user for samples that a wave file can't hold.
template <class WaveFile>
void AddPcm16(const std::vector<std::vector<int16_t>>& channels, const IncomingWave& incoming, WaveFile& file,
              bool big_endian)
{
    if (incoming.frames * channels.size() * 2 > kMostWaveBytes)
    {
        throw std::invalid_argument("Its samples take more than 2 GiB as 16-bit PCM. A wave file can't hold them.");
    }

    file.loop_start = incoming.loop_start;
    file.loop_end = static_cast<uint32_t>(incoming.frames);
    for (const std::vector<int16_t>& channel : channels)
    {
        std::vector<uint8_t> samples(incoming.frames * 2);
        for (std::size_t i = 0; i < incoming.frames; i++)
        {
            const auto sample = static_cast<uint16_t>(channel[i]);
            big_endian ? xsflib::StoreBe16(&samples[i * 2], sample) : xsflib::StoreLe16(&samples[i * 2], sample);
        }
        typename WaveFile::Channel added;
        added.samples = std::move(samples);
        file.channels.push_back(std::move(added));
    }
}

// Puts `channels`, each `incoming.frames` long, into `file` as DSP-ADPCM: a wave file of the 3DS, the Wii U or the
// Switch, or of the Wii. A loop's start moves forward to a frame, and the channels grow by the samples that repeat
// after the loop's end. The encoder decodes as the Wii U and the Switch do. The Wii's accelerator gives the same
// samples. The move and the ratio of signal to noise go into `incoming`. Throws std::invalid_argument with a message
// for the user for samples that a wave file can't hold.
template <class WaveFile>
void AddDspAdpcm(std::vector<std::vector<int16_t>>& channels, IncomingWave& incoming, WaveFile& file)
{
    if (wave::DspAdpcmBytes(incoming.frames + kFrameSamples - 1) * channels.size() > kMostWaveBytes)
    {
        throw std::invalid_argument("Its samples take more than 2 GiB as DSP-ADPCM. A wave file can't hold them.");
    }

    // The loop starts at a frame.
    uint32_t loop_start = 0;
    if (incoming.loop)
    {
        loop_start = wave::AlignDspAdpcmLoop(channels, incoming.loop_start);
        incoming.loop_moved = loop_start - incoming.loop_start;
    }
    const std::size_t length = channels[0].size();
    file.loop_start = loop_start;
    file.loop_end = static_cast<uint32_t>(length);

    // Each channel's frames and the decoder's states, and the error of the decoded samples.
    double signal = 0.0;
    double noise = 0.0;
    for (const std::vector<int16_t>& channel : channels)
    {
        const wave::DspAdpcmChannel encoded = wave::EncodeDspAdpcm(channel, loop_start, wave::DspAdpcmSum::kWrap);
        const std::vector<int16_t> decoded = wave::DecodeDspAdpcm(encoded.data, static_cast<uint32_t>(length),
                                                                  encoded.coefs, 0, 0, wave::DspAdpcmSum::kWrap);
        for (std::size_t i = 0; i < length; i++)
        {
            const double sample = channel[i];
            const double error = sample - decoded[i];
            signal += sample * sample;
            noise += error * error;
        }

        typename WaveFile::Channel added;
        added.samples = encoded.data;
        auto& info = added.adpcm.emplace();
        info.coefs = encoded.coefs;
        info.pred_scale = encoded.pred_scale;
        info.loop_pred_scale = encoded.loop_pred_scale;
        info.loop_yn1 = encoded.loop_yn1;
        info.loop_yn2 = encoded.loop_yn2;
        file.channels.push_back(std::move(added));
    }
    if (noise > 0.0)
    {
        incoming.signal_to_noise = 10.0 * std::log10(signal / noise);
    }
}

// The kind of item whose file is a file of `kind`.
song::ItemKind ItemKindOf(FileKind kind)
{
    switch (kind)
    {
    case FileKind::kBank:
        return song::ItemKind::kBank;
    case FileKind::kWaveArchive:
        return song::ItemKind::kWaveArchive;
    case FileKind::kWaveSoundFile:
        break;
    }

    return song::ItemKind::kWaveSound;
}

// Reads `bytes`, a file of `kind` on the console of `flavor`, by ntdlib's strict rules. Each wave of a wave archive has
// to read too. Throws std::invalid_argument with a message for the user when one doesn't.
void ReadStrictly(std::span<const uint8_t> bytes, FileKind kind, sar::Flavor flavor)
{
    sar::Options options;
    options.flavor = flavor;
    options.rules = sar::Rules::kStopAtFirstProblem;
    try
    {
        if (kind == FileKind::kBank)
        {
            sar::Bank::Parse(bytes, options);
        }
        else if (kind == FileKind::kWaveArchive)
        {
            for (const std::span<const uint8_t> wave : sar::WaveArchive::Parse(bytes, options).waves)
            {
                sar::Wave::Parse(wave, options);
            }
        }
        else
        {
            sar::WaveSoundFile::Parse(bytes, options);
        }
    }
    catch (const std::exception& error)
    {
        throw std::invalid_argument(std::string("Hokuto can't read it. ") + format::AsSentence(error.what()));
    }
}

// The latest version among the waves of `file`, a wave archive of `flavor`, or nothing for a wave archive without
// waves. ntdlib has to read the file by the strict rules (ReadStrictly).
std::optional<uint32_t> LatestWaveVersion(std::span<const uint8_t> file, sar::Flavor flavor)
{
    sar::Options options;
    options.flavor = flavor;
    std::optional<uint32_t> latest;
    for (const std::span<const uint8_t> wave : sar::WaveArchive::Parse(file, options).waves)
    {
        if (wave.size() >= kHeaderSize)
        {
            const uint32_t version = xsflib::Reader(wave, flavor == sar::Flavor::kCafe).U32(kVersionField);
            latest = std::max(latest.value_or(0), version);
        }
    }

    return latest;
}

// `bytes`, a file of `kind` on the console of `from`, as a file of `to` with `versions`. A file of another console
// converts as Export… converts it (song::ConvertItemFile). A file of the same console keeps its byte order. A wave
// archive is written again with its waves. A bank file and a wave sound file take the version in their header, and
// their structures keep their places.
std::vector<uint8_t> WithVersions(std::span<const uint8_t> bytes, FileKind kind, sar::Flavor from, sar::Flavor to,
                                  const song::FileVersions& versions)
{
    if (from != to)
    {
        return song::ConvertItemFile(bytes, ItemKindOf(kind), from, to, versions);
    }
    if (kind == FileKind::kWaveArchive)
    {
        return sar::ConvertWaveArchive(sar::WaveArchiveFile::Read(bytes), to, versions.file, versions.waves).Write();
    }

    return sar::WithConsole(bytes, to, versions.file);
}

// The version of the first wave of `file`, a wave archive that ntdlib reads with `options`, or nothing for a wave
// archive without waves or one that ntdlib can't read.
std::optional<uint32_t> FirstWaveVersion(std::span<const uint8_t> file, const sar::Options& options)
{
    try
    {
        const std::vector<std::span<const uint8_t>> waves = sar::WaveArchive::Parse(file, options).waves;
        if (!waves.empty() && waves[0].size() >= kHeaderSize)
        {
            return xsflib::Reader(waves[0], options.flavor == sar::Flavor::kCafe).U32(kVersionField);
        }
    }
    catch (const std::exception&)
    {
        // Another wave archive may give the version.
    }

    return std::nullopt;
}

// A wave in a table of waves: its wave archive's item ID and its index.
using WaveKey = std::pair<uint32_t, uint32_t>;

// The places at which `copy`, a group's copy of `present`, names the waves of `present`'s table. `present` is a bank
// file or a wave sound file of `flavor`. A group's copy can name the same waves in a wave archive of the group's, at
// other indices. The map gives each wave of `present` its place in the copy. Nothing when the copy differs from
// `present` in other bytes than those of its table, when a wave of `present` has two places in the copy, or when ntdlib
// can't read either table.
std::optional<std::map<WaveKey, sar::WaveId>> PlacesInCopy(std::span<const uint8_t> present,
                                                           std::span<const uint8_t> copy, sar::Flavor flavor)
{
    try
    {
        const std::vector<sar::WaveId> waves = sar::WaveTable(present, flavor);
        const std::vector<sar::WaveId> places = sar::WaveTable(copy, flavor);
        if (waves.size() != places.size() || !std::ranges::equal(sar::WithWaveTable(present, flavor, places), copy))
        {
            return std::nullopt;
        }

        std::map<WaveKey, sar::WaveId> result;
        for (std::size_t i = 0; i < waves.size(); i++)
        {
            const auto [place, added] = result.try_emplace({waves[i].wave_archive_item, waves[i].index}, places[i]);
            if (!added && place->second != places[i])
            {
                return std::nullopt;
            }
        }

        return result;
    }
    catch (const std::exception&)
    {
        // A wave archive, or a file whose table ntdlib can't read.
        return std::nullopt;
    }
}

// The bytes that a group's copy of `present` takes when `incoming` takes the place of `present`.
struct CopyBytes
{
    std::vector<uint8_t> bytes;

    // Whether the copy names `present`'s waves at other places, and takes `incoming` with those places.
    bool placed = false;

    // In such a copy, the waves of `incoming`'s table that `present` doesn't name. They keep the archive's places.
    std::size_t unplaced = 0;
};

// The bytes that `copy`, a group's copy of `present`, takes when `incoming` takes the place of `present`. Each is a
// file of `flavor`. A copy that holds `present`'s bytes takes `incoming`. A copy that names `present`'s waves at other
// places takes `incoming` with those places: each wave of `incoming`'s table that `present` names takes its place in
// the copy (PlacesInCopy). Any other copy takes `incoming` as it is.
CopyBytes BytesOfCopy(std::span<const uint8_t> copy, std::span<const uint8_t> present,
                      std::span<const uint8_t> incoming, sar::Flavor flavor)
{
    CopyBytes result{.bytes = std::vector<uint8_t>(incoming.begin(), incoming.end())};
    if (std::ranges::equal(copy, present))
    {
        return result;
    }
    const std::optional<std::map<WaveKey, sar::WaveId>> places = PlacesInCopy(present, copy, flavor);
    std::vector<sar::WaveId> waves;
    try
    {
        waves = places ? sar::WaveTable(incoming, flavor) : std::vector<sar::WaveId>();
    }
    catch (const std::exception&)
    {
        // A new file without a table that ntdlib reads goes in as it is.
        return result;
    }
    if (!places)
    {
        return result;
    }

    for (sar::WaveId& wave : waves)
    {
        const auto place = places->find({wave.wave_archive_item, wave.index});
        if (place != places->end())
        {
            wave = place->second;
        }
        else
        {
            result.unplaced++;
        }
    }
    result.bytes = sar::WithWaveTable(incoming, flavor, waves);
    result.placed = true;

    return result;
}

} // namespace

std::string NameOf(FileKind kind)
{
    switch (kind)
    {
    case FileKind::kBank:
        return "bank";
    case FileKind::kWaveArchive:
        return "wave archive";
    case FileKind::kWaveSoundFile:
        break;
    }

    return "wave sound file";
}

namespace
{

// A file of `kind` in a sentence: "bank file", "wave archive file" or "wave sound file".
std::string FileText(FileKind kind)
{
    return kind == FileKind::kWaveSoundFile ? NameOf(kind) : NameOf(kind) + " file";
}

} // namespace

IncomingFile ReadIncomingFile(std::span<const uint8_t> bytes, FileKind kind, sar::Flavor flavor,
                              const std::optional<song::FileVersions>& versions)
{
    // The magic gives the kind and the console's letter. The byte order mark tells the Wii U from the Switch.
    const std::string magic = MagicOf(kind);
    const bool big_endian = bytes.size() >= kHeaderSize && bytes[4] == 0xFE && bytes[5] == 0xFF;
    const bool little_endian = bytes.size() >= kHeaderSize && bytes[4] == 0xFF && bytes[5] == 0xFE;
    if ((!big_endian && !little_endian) || (bytes[0] != 'C' && bytes[0] != 'F') ||
        !std::equal(magic.begin(), magic.end(), bytes.begin() + 1))
    {
        throw std::invalid_argument(format::NotKindText(bytes, "a " + FileText(kind)));
    }

    // The 3DS's files are little-endian. So are the Switch's.
    const sar::Flavor console = bytes[0] == 'C' ? sar::Flavor::kCtr
                                : big_endian    ? sar::Flavor::kCafe
                                                : sar::Flavor::kNx;
    if (console == sar::Flavor::kCtr && big_endian)
    {
        throw std::invalid_argument(format::NotKindText(bytes, "a " + FileText(kind)));
    }

    // The header's size.
    const xsflib::Reader r(bytes, big_endian);
    const uint32_t size = r.U32(kSizeField);
    if (size < kHeaderSize || size > bytes.size())
    {
        throw std::invalid_argument("Its header gives a size of " + std::to_string(size) + " bytes. The file holds " +
                                    std::to_string(bytes.size()) + " bytes.");
    }

    // A file of another console goes in converted, with the versions of the archive's files. With `versions`, so does a
    // file of the archive's console that holds a later version, or a wave archive with a wave of a later version. A
    // game may refuse a later version than its files hold.
    IncomingFile incoming{
        kind, r.U32(kVersionField), std::vector<uint8_t>(bytes.begin(), bytes.begin() + size), {}, {}};
    ReadStrictly(incoming.bytes, kind, console);
    const song::FileVersions chosen = versions.value_or(song::ExportVersions(ItemKindOf(kind), flavor));
    const bool later =
        versions && (incoming.version > chosen.file ||
                     (kind == FileKind::kWaveArchive && LatestWaveVersion(incoming.bytes, console) > chosen.waves));
    if (console != flavor || later)
    {
        ConvertedFile converted{console, incoming.version, incoming.bytes.size(), std::nullopt};
        if (kind == FileKind::kWaveArchive)
        {
            converted.wave_version = chosen.waves;
        }
        try
        {
            incoming.bytes = WithVersions(incoming.bytes, kind, console, flavor, chosen);
        }
        catch (const std::exception& error)
        {
            throw std::invalid_argument("Hokuto can't convert it for the " + song::ConsoleName(flavor) + ". " +
                                        format::AsSentence(error.what()));
        }
        incoming.version = chosen.file;
        incoming.converted = converted;
        ReadStrictly(incoming.bytes, kind, flavor);
    }

    return incoming;
}

song::FileVersions IncomingVersions(const sar::SoundArchive& archive, FileKind kind, std::optional<uint32_t> item)
{
    // The item's file, then the archive's files of the kind in the order of their items.
    std::vector<uint32_t> files;
    if (item)
    {
        try
        {
            files.push_back(FileIdOf(archive, kind, *item));
        }
        catch (const std::invalid_argument&)
        {
            // A new item takes the versions of the archive's files.
        }
    }
    if (kind == FileKind::kBank)
    {
        for (const sar::BankInfo& bank : archive.Banks())
        {
            files.push_back(bank.file_id);
        }
    }
    else if (kind == FileKind::kWaveArchive)
    {
        for (const sar::WaveArchiveInfo& wave_archive : archive.WaveArchives())
        {
            files.push_back(wave_archive.file_id);
        }
    }
    else
    {
        for (const sar::SoundInfo& sound : archive.Sounds())
        {
            if (sound.type == sar::SoundType::kWaveSound)
            {
                files.push_back(sound.file_id);
            }
        }
    }

    // The first file of the kind gives the file's version, and the first wave the waves' version.
    const sar::Flavor flavor = archive.ReadOptions().flavor;
    const std::string magic = MagicOf(kind);
    std::optional<uint32_t> file_version;
    std::optional<uint32_t> wave_version;
    for (const uint32_t file_id : files)
    {
        const std::span<const uint8_t> file = archive.FileData(file_id);
        if (file.size() < kHeaderSize || !std::equal(magic.begin(), magic.end(), file.begin() + 1))
        {
            continue;
        }

        if (!file_version)
        {
            file_version = xsflib::Reader(file, flavor == sar::Flavor::kCafe).U32(kVersionField);
        }
        if (kind == FileKind::kWaveArchive && !wave_version)
        {
            wave_version = FirstWaveVersion(file, archive.ReadOptions());
        }
    }

    // The versions of Export…'s files serve for the rest.
    song::FileVersions versions = song::ExportVersions(ItemKindOf(kind), flavor);
    versions.file = file_version.value_or(versions.file);
    versions.waves = wave_version.value_or(versions.waves);

    return versions;
}

uint32_t FileIdOf(const sar::SoundArchive& archive, FileKind kind, uint32_t item)
{
    switch (kind)
    {
    case FileKind::kBank:
        if (item < archive.Banks().size())
        {
            return archive.Banks()[item].file_id;
        }
        break;

    case FileKind::kWaveArchive:
        if (item < archive.WaveArchives().size())
        {
            return archive.WaveArchives()[item].file_id;
        }
        break;

    case FileKind::kWaveSoundFile:
        if (item < archive.Sounds().size() && archive.Sounds()[item].type == sar::SoundType::kWaveSound)
        {
            return archive.Sounds()[item].file_id;
        }
        break;
    }

    throw std::invalid_argument("The archive has no " + NameOf(kind) + " there.");
}

FileChanges ChangesOf(const format::File& file, const sar::SoundArchive& archive, uint32_t item,
                      const IncomingFile& incoming)
{
    // The present file, and the other items whose file it is.
    FileChanges changes;
    const uint32_t file_id = FileIdOf(archive, incoming.kind, item);
    const std::span<const uint8_t> present = archive.FileData(file_id);
    const sar::Options& options = archive.ReadOptions();
    changes.size = present.size();
    if (present.size() >= kHeaderSize)
    {
        changes.version = xsflib::Reader(present, options.flavor == sar::Flavor::kCafe).U32(kVersionField);
    }

    const auto share = [&](std::size_t count, const auto& file_of)
    {
        for (std::size_t i = 0; i < count; i++)
        {
            if (i != item && file_of(i))
            {
                changes.sharing.push_back(static_cast<uint32_t>(i));
            }
        }
    };
    if (incoming.kind == FileKind::kBank)
    {
        share(archive.Banks().size(), [&](std::size_t i) { return archive.Banks()[i].file_id == file_id; });
    }
    else if (incoming.kind == FileKind::kWaveArchive)
    {
        share(archive.WaveArchives().size(),
              [&](std::size_t i) { return archive.WaveArchives()[i].file_id == file_id; });
    }
    else
    {
        const auto same_file = [&](std::size_t i)
        {
            const sar::SoundInfo& sound = archive.Sounds()[i];
            return sound.type == sar::SoundType::kWaveSound && sound.file_id == file_id;
        };
        share(archive.Sounds().size(), same_file);
    }

    // The copies in groups that hold other bytes. Only a group's copy can differ from the present file. A copy that
    // names the present file's waves at other places takes the new file with those places.
    if (file.native)
    {
        const format::NativeArchive& native = *file.native;
        std::vector<sar::FileCopy> copies;
        try
        {
            copies = sar::FileCopies(native.flavor, format::CopySequences(file), *native.pieces, native.fields,
                                     format::GroupFileLayouts(native), file_id);
        }
        catch (const std::exception&)
        {
            // The replacement can't read the archive's tables either, and says so.
        }
        for (const sar::FileCopy& copy : copies)
        {
            if (std::ranges::equal(copy.bytes, present))
            {
                continue;
            }

            const CopyBytes bytes = BytesOfCopy(copy.bytes, present, incoming.bytes, native.flavor);
            changes.placed_copies += bytes.placed ? 1 : 0;
            changes.unplaced_waves = std::max(changes.unplaced_waves, bytes.unplaced);
            changes.other_copies += bytes.placed ? 0 : 1;
        }
    }

    // What the new file names, and what names the new wave archive's waves.
    const std::vector<std::optional<std::size_t>> counts = WaveCounts(archive);
    switch (incoming.kind)
    {
    case FileKind::kBank:
        {
            const sar::Bank bank = sar::Bank::Parse(incoming.bytes, options);
            changes.lacked = Lacked(bank.waves, counts);
            std::optional<sar::Bank> old;
            try
            {
                old = sar::Bank::Parse(present, options);
            }
            catch (const std::exception&)
            {
                // The present bank holds no programs that Hokuto can read.
            }
            for (std::size_t program = 0; old && program < old->instruments.size(); program++)
            {
                const bool held = program < bank.instruments.size() && bank.instruments[program].has_value();
                if (old->instruments[program] && !held)
                {
                    changes.programs.push_back(static_cast<uint32_t>(program));
                }
            }
            const uint32_t id = kBankKind << 24 | item;
            for (std::size_t i = 0; i < archive.Sounds().size(); i++)
            {
                if (std::ranges::find(archive.Sounds()[i].banks, id) != archive.Sounds()[i].banks.end())
                {
                    changes.players.push_back(static_cast<uint32_t>(i));
                }
            }
            break;
        }

    case FileKind::kWaveArchive:
        {
            changes.count = sar::WaveArchive::Parse(incoming.bytes, options).waves.size();
            const auto past = [&](const sar::WaveId& wave)
            {
                return Names(wave, item) && wave.index >= changes.count;
            };
            changes.past_banks = BanksNaming(archive, past);
            changes.past_sounds = WaveSoundsPlaying(archive, past);
            break;
        }

    case FileKind::kWaveSoundFile:
        {
            const sar::WaveSoundFile sounds = sar::WaveSoundFile::Parse(incoming.bytes, options);
            changes.lacked = Lacked(sounds.waves, counts);
            changes.count = sounds.sounds.size();
            for (std::size_t i = 0; i < archive.Sounds().size(); i++)
            {
                const sar::SoundInfo& sound = archive.Sounds()[i];
                if (sound.type == sar::SoundType::kWaveSound && sound.file_id == file_id &&
                    sound.wave_sound_index >= sounds.sounds.size())
                {
                    changes.past_sounds.push_back(static_cast<uint32_t>(i));
                }
            }
            break;
        }
    }

    return changes;
}

std::vector<uint32_t> SoundsPlaying(const sar::SoundArchive& archive, FileKind kind, uint32_t item)
{
    // The banks that a sequence sound plays the item through, and the wave sounds that play it.
    std::vector<uint32_t> banks;
    std::vector<uint32_t> wave_sounds;
    if (kind == FileKind::kBank)
    {
        // Banks can share a file. The sounds of each of them play it.
        const uint32_t file_id = FileIdOf(archive, kind, item);
        for (std::size_t b = 0; b < archive.Banks().size(); b++)
        {
            if (archive.Banks()[b].file_id == file_id)
            {
                banks.push_back(static_cast<uint32_t>(b));
            }
        }
    }
    else if (kind == FileKind::kWaveArchive)
    {
        const auto named = [item](const sar::WaveId& wave)
        {
            return Names(wave, item);
        };
        banks = BanksNaming(archive, named);
        wave_sounds = WaveSoundsPlaying(archive, named);
    }
    else
    {
        const uint32_t file_id = FileIdOf(archive, kind, item);
        for (std::size_t i = 0; i < archive.Sounds().size(); i++)
        {
            const sar::SoundInfo& sound = archive.Sounds()[i];
            if (sound.type == sar::SoundType::kWaveSound && sound.file_id == file_id)
            {
                wave_sounds.push_back(static_cast<uint32_t>(i));
            }
        }
    }

    std::vector<uint32_t> sounds;
    for (std::size_t i = 0; i < archive.Sounds().size(); i++)
    {
        const sar::SoundInfo& sound = archive.Sounds()[i];
        const auto in_slot = [&banks](uint32_t slot)
        {
            return slot >> 24 == kBankKind && std::ranges::find(banks, slot & kIndexBits) != banks.end();
        };
        const bool sequence = sound.type == sar::SoundType::kSequence && std::ranges::any_of(sound.banks, in_slot);
        if (sequence || std::ranges::find(wave_sounds, static_cast<uint32_t>(i)) != wave_sounds.end())
        {
            sounds.push_back(static_cast<uint32_t>(i));
        }
    }

    return sounds;
}

BankSelections ProgramsOfBank(const format::File& file, const sar::SoundArchive& archive,
                              std::span<const std::optional<SoundPlace>> places, uint32_t bank)
{
    // The slots that hold the bank or a bank of the same file.
    const uint32_t file_id = FileIdOf(archive, FileKind::kBank, bank);
    const auto same_file = [&archive, file_id](uint32_t slot)
    {
        const uint32_t index = slot & kIndexBits;
        return slot >> 24 == kBankKind && index < archive.Banks().size() && archive.Banks()[index].file_id == file_id;
    };

    // The programs that each sound selects in those slots. Sounds that start at the same item of a sequence select the
    // same programs.
    std::map<std::pair<uint32_t, uint32_t>, Selections> selected;
    std::set<uint32_t> programs;
    BankSelections selections;
    for (std::size_t i = 0; i < archive.Sounds().size() && i < places.size(); i++)
    {
        const std::vector<uint32_t>& slots = archive.Sounds()[i].banks;
        if (!places[i] || std::ranges::none_of(slots, same_file))
        {
            continue;
        }

        const SoundPlace& place = *places[i];
        const uint32_t start = place.start_field ? file.native->fields.at(*place.start_field).to.offset : 0;
        const auto [found, added] = selected.try_emplace({place.sequence, start});
        if (added)
        {
            found->second = ProgramsSelected(SequenceToEdit(file, place.sequence), start);
        }
        for (const Program& program : found->second.programs)
        {
            const auto slot = static_cast<std::size_t>(program.slot);
            if (slot < slots.size() && same_file(slots[slot]))
            {
                programs.insert(static_cast<uint32_t>(program.number));
            }
        }
        selections.undecided += found->second.undecided;
    }
    selections.programs.assign(programs.begin(), programs.end());

    return selections;
}

namespace
{

namespace rsar = ntdlib::rsar;

// The fields of the header of a file of the Wii: the version, 16 bits, and the file's size, 32 bits. The header takes
// 16 bytes at least.
constexpr std::size_t kWiiVersionField = 0x06;
constexpr std::size_t kWiiSizeField = 0x08;
constexpr std::size_t kWiiHeaderSize = 0x10;

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

        for (int key = 0; key < 128; key++)
        {
            for (int velocity = 0; velocity < 128; velocity++)
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
// when ntdlib can't read them from it.
std::pair<std::vector<uint32_t>, std::optional<std::size_t>> WavesIn(std::span<const uint8_t> bytes, FileKind kind,
                                                                     std::span<const uint8_t> wave_data)
{
    std::vector<uint32_t> waves;
    std::optional<std::size_t> count;
    try
    {
        if (kind == FileKind::kBank)
        {
            waves = WavesOfBank(rsar::Bank::Parse(bytes, ntdlib::rvl::snd::kRsarOptions));
            count = rsar::Bank::Parse(bytes, wave_data, ntdlib::rvl::snd::kRsarOptions).Waves().size();
        }
        else
        {
            waves = WavesOfWaveSounds(rsar::WaveSoundFile::Parse(bytes, ntdlib::rvl::snd::kRsarOptions));
            count = rsar::WaveSoundFile::Parse(bytes, wave_data, ntdlib::rvl::snd::kRsarOptions).Waves().size();
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
        throw std::invalid_argument(std::string("Hokuto can't read the wave archive file. ") +
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
        throw std::invalid_argument(std::string("Hokuto can't convert it for the Wii. ") +
                                    format::AsSentence(error.what()));
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
                                                " of the 3DS, the Wii U or the Switch. It goes in converted once "
                                                "wave data of the archive takes the place of each of its wave archives."
                                          : format::NotKindText(bytes, "a " + FileText(kind) + " of the Wii"));
    }
    if (bytes.size() < kWiiHeaderSize || bytes[4] != 0xFE || bytes[5] != 0xFF)
    {
        throw std::invalid_argument(format::NotKindText(bytes, "a " + FileText(kind) + " of the Wii"));
    }

    // The header's size.
    const xsflib::Reader r(bytes, true);
    const uint32_t size = r.U32(kWiiSizeField);
    if (size < kWiiHeaderSize || size > bytes.size())
    {
        throw std::invalid_argument("Its header gives a size of " + std::to_string(size) + " bytes. The file holds " +
                                    std::to_string(bytes.size()) + " bytes.");
    }
    IncomingFile incoming{
        kind, r.U16(kWiiVersionField), std::vector<uint8_t>(bytes.begin(), bytes.begin() + size), {}, {}};
    try
    {
        switch (kind)
        {
        case FileKind::kBank:
            rsar::Bank::Parse(incoming.bytes, ntdlib::rvl::snd::kRsarOptions);
            break;
        case FileKind::kWaveArchive:
            ReadWiiWaveArchive(incoming.bytes);
            break;
        case FileKind::kWaveSoundFile:
            rsar::WaveSoundFile::Parse(incoming.bytes, ntdlib::rvl::snd::kRsarOptions);
            break;
        }
    }
    catch (const std::exception& error)
    {
        throw std::invalid_argument("Hokuto can't read the " + FileText(kind) + ". " +
                                    format::AsSentence(error.what()));
    }

    return incoming;
}

std::optional<sar::Flavor> LaterConsoleOf(std::span<const uint8_t> bytes, FileKind kind)
{
    // The magic gives the kind and the console's letter. The byte order mark tells the Wii U from the Switch. The 3DS's
    // files are little-endian.
    const std::string magic = MagicOf(kind);
    const bool big_endian = bytes.size() >= kHeaderSize && bytes[4] == 0xFE && bytes[5] == 0xFF;
    const bool little_endian = bytes.size() >= kHeaderSize && bytes[4] == 0xFF && bytes[5] == 0xFE;
    if ((!big_endian && !little_endian) || (bytes[0] != 'C' && bytes[0] != 'F') ||
        !std::equal(magic.begin(), magic.end(), bytes.begin() + 1))
    {
        return std::nullopt;
    }
    if (bytes[0] == 'C')
    {
        return little_endian ? std::optional(sar::Flavor::kCtr) : std::nullopt;
    }

    return big_endian ? sar::Flavor::kCafe : sar::Flavor::kNx;
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
        throw std::invalid_argument("Hokuto can't read the " + NameOf(incoming.kind) + ". " +
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
                                        std::to_string(wave.wave_archive_item & kIndexBits) + ".");
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
        throw std::invalid_argument(std::string("Hokuto can't convert it for the Wii. ") +
                                    format::AsSentence(error.what()));
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
        throw std::invalid_argument(std::string("Hokuto can't read the wave archive file's waves. ") +
                                    format::AsSentence(error.what()));
    }

    return data;
}

WiiWaveData WiiWaveDataOf(const rsar::SoundArchive& archive, uint32_t file_id)
{
    const std::optional<rsar::FileData> data = archive.File(file_id);
    if (!data || data->wave_data.empty())
    {
        throw std::invalid_argument("The archive lacks the wave data of file " + std::to_string(file_id) + ".");
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
        throw std::invalid_argument("Hokuto can't read the " + kind + ".");
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
        throw std::invalid_argument(*block ? "The " + kind + "'s WAVE block describes other samples than the waves. " +
                                                 format::AsSentence(error.what())
                                           : "Hokuto can't read the " + kind + " with the waves. " +
                                                 format::AsSentence(error.what()));
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
        throw std::invalid_argument("Hokuto can't write the " + kind + " in version " + std::to_string(version >> 8) +
                                    "." + std::to_string(version & 0xFF) + ". " + format::AsSentence(error.what()));
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
        return "Hokuto can't read the " + file + "'s waves from its wave data.";
    }
    if (!waves.empty() && waves.back() >= *held)
    {
        return "The " + file + " names wave " + std::to_string(waves.back()) + ", and its wave data holds " +
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
            throw std::invalid_argument("The archive has no bank there.");
        }
        return archive.Banks()[item].file_id;

    case FileKind::kWaveArchive:
        if (archive.WaveData(item).empty())
        {
            throw std::invalid_argument("The archive has no wave archive there.");
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

    switch (incoming.kind)
    {
    case FileKind::kBank:
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
            break;
        }

    case FileKind::kWaveSoundFile:
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
            break;
        }

    case FileKind::kWaveArchive:
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
            const std::vector<uint32_t> waves =
                WavesIn(owner, bank ? FileKind::kBank : FileKind::kWaveSoundFile, wave_data).first;
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

            return changes;
        }
    }

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
            throw std::invalid_argument(std::string("Hokuto can't read the wave archive's waves. ") +
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
        const std::string present = *block ? "describes its waves in a WAVE block, over samples beside it"
                                           : "keeps its waves in an RWAR beside it";
        const std::string next = *block ? "keeps its waves in an RWAR" : "describes its waves in a WAVE block";
        throw std::invalid_argument("The present " + kind + " " + present + ". The new " + kind + " " + next +
                                    ". Hokuto can't put the one in the place of the other.");
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

std::vector<uint8_t> WithProgramFixes(const sar::SoundArchive& archive, uint32_t bank, std::span<const uint8_t> bytes,
                                      std::span<const ProgramFix> fixes)
{
    // The present bank's instrument comes from the archive's file, and another from the new bank.
    const std::span<const uint8_t> present = archive.FileData(FileIdOf(archive, FileKind::kBank, bank));
    std::vector<sar::InstrumentCopy> copies;
    for (const ProgramFix& fix : fixes)
    {
        copies.push_back({fix.from ? bytes : present, fix.from.value_or(fix.program), fix.program});
    }

    try
    {
        return sar::WithInstruments(bytes, archive.ReadOptions().flavor, copies);
    }
    catch (const std::exception& error)
    {
        throw std::invalid_argument(std::string("Hokuto can't copy the instruments into the new bank. ") +
                                    format::AsSentence(error.what()));
    }
}

namespace
{

// The pieces in which each copy of file `file_id` of `file`, a file of the 3DS, the Wii U or the Switch, holds `bytes`
// in the place of the present file. A group's copy that names the present file's waves at other places takes `bytes`
// with those places (BytesOfCopy). The present file is the one that the archive that `file` builds gives, with the
// group files. Throws the exceptions of ntdlib's ReplaceFile.
sar::ReplacedPieces ReplacedCopies(const format::File& file, uint32_t file_id, std::span<const uint8_t> bytes)
{
    const format::NativeArchive& native = *file.native;
    const std::vector<ntdlib::commands::Sequence> sequences = format::CopySequences(file);
    const std::vector<sar::GroupFileLayout> group_files = format::GroupFileLayouts(native);
    const std::vector<sar::FileCopy> copies =
        sar::FileCopies(native.flavor, sequences, *native.pieces, native.fields, group_files, file_id);
    const auto same = [&copies](const sar::FileCopy& copy)
    {
        return copy.bytes == copies.front().bytes;
    };
    if (copies.empty() || std::ranges::all_of(copies, same))
    {
        return sar::ReplaceFile(native.flavor, sequences, *native.pieces, native.fields, group_files, file_id, bytes);
    }

    // The copies differ. The archive that the file builds gives the present file.
    const std::shared_ptr<const sar::SoundArchive> archive =
        song::LoadArchive(format::ExportArchive(file), "the archive", format::ExportGroupFiles(file));
    const std::span<const uint8_t> present = archive->FileData(file_id);

    const auto copy_bytes = [&](const sar::FileCopy& copy)
    {
        return BytesOfCopy(copy.bytes, present, bytes, native.flavor).bytes;
    };
    return sar::ReplaceFile(native.flavor, sequences, *native.pieces, native.fields, group_files, file_id, copy_bytes);
}

} // namespace

format::File ReplaceFile(const format::File& file, uint32_t file_id, std::span<const uint8_t> bytes,
                         std::optional<std::span<const uint8_t>> wave_data)
{
    if (!file.native)
    {
        throw std::invalid_argument("The file holds no archive of a game.");
    }
    if (wave_data && !file.native->wii)
    {
        throw std::invalid_argument("Only a file of the Wii has wave data beside it.");
    }

    // ntdlib's reasons, in words for the user. The copies in the group files beside the archive take the bytes too. On
    // the Wii, every group that holds the file holds a copy.
    const format::NativeArchive& native = *file.native;
    sar::ReplacedPieces pieces;
    try
    {
        if (native.wii)
        {
            pieces.pieces = ntdlib::rsar::ReplaceFile(format::BuildOfWiiArchive(native), format::CopySequences(file),
                                                      *native.pieces, native.fields, file_id, bytes, wave_data);
        }
        else
        {
            pieces = ReplacedCopies(file, file_id, bytes);
        }
    }
    catch (const std::invalid_argument& error)
    {
        const std::string reason = error.what();
        if (reason.find("shares its bytes") != std::string::npos)
        {
            throw std::invalid_argument(
                "Another file of the archive holds the same bytes. That file would change too.");
        }
        if (reason.find("isn't a piece of its own") != std::string::npos && native.wii)
        {
            throw std::invalid_argument(
                "The old file lies among the archive's other bytes. Hokuto can't replace it there. An archive that is "
                "cut short can leave a file there.");
        }
        if (reason.find("isn't a piece of its own") != std::string::npos)
        {
            throw std::invalid_argument(
                "The old file lies among the archive's other bytes. Hokuto can't replace it there. A .hok file saved "
                "before 2026-10-17 keeps every bank, wave archive and wave sound file that way. A new import of the "
                "game's archive cuts them out.");
        }
        if (reason.find("holds no file") != std::string::npos)
        {
            throw std::invalid_argument("The old file lies outside the archive. Hokuto can't replace it there.");
        }
        if (reason.find("no group of the archive holds the wave data") != std::string::npos)
        {
            throw std::invalid_argument("The old file has no wave data beside it.");
        }
        if (reason.find("no group of the archive holds") != std::string::npos)
        {
            throw std::invalid_argument("No group of the archive holds the old file. Hokuto can't replace it there.");
        }

        throw std::invalid_argument("The archive's tables can't follow the new file. " + format::AsSentence(reason));
    }
    catch (const xsflib::FormatError& error)
    {
        throw std::invalid_argument(std::string("Hokuto can't read the archive's tables. ") +
                                    format::AsSentence(error.what()));
    }

    const auto take_pieces = [&pieces](format::NativeArchive& edited)
    {
        if (*edited.pieces != pieces.pieces)
        {
            edited.pieces = std::move(pieces.pieces);
        }
        for (std::size_t g = 0; g < edited.group_files.size() && g < pieces.group_files.size(); ++g)
        {
            if (*edited.group_files[g].pieces != pieces.group_files[g])
            {
                edited.group_files[g].pieces = std::move(pieces.group_files[g]);
            }
        }
    };
    format::File result = file;
    result.native.Edit(take_pieces);

    // The models of the file's bank or waves no longer give its bytes.
    result.banks.erase(file_id);
    const auto in_file = [file_id](const auto& wave)
    {
        return wave.first.file == file_id;
    };
    std::erase_if(result.waves, in_file);

    return result;
}

namespace
{

// The samples of `bytes`, a WAV file, for a wave in `encoding`: 16 bits in one or two channels, up to the end of the
// loop. ReadIncomingWave reads the same samples. A WAV file without a loop takes `loop`. With `rate`, lower than the
// WAV file's, the samples are resampled to it, and the loop keeps its place. `incoming` takes the WAV file's details
// and the wave's rate, frames and loop. Throws std::invalid_argument with a message for the user as ReadIncomingWave
// does.
std::vector<std::vector<int16_t>> WavSamples(std::span<const uint8_t> bytes, sar::WaveEncoding encoding,
                                             std::optional<WaveLoop> loop, std::optional<uint32_t> rate,
                                             IncomingWave& incoming)
{
    if (encoding != sar::WaveEncoding::kDspAdpcm && encoding != sar::WaveEncoding::kPcm16)
    {
        throw std::invalid_argument("Hokuto makes a wave in DSP-ADPCM or in 16-bit PCM only.");
    }

    // The WAV file's samples as 16 bits. A wave holds one or two channels.
    seqlib::WavContents wav;
    try
    {
        wav = seqlib::ReadWav(bytes);
    }
    catch (const seqlib::ReadError& error)
    {
        throw std::invalid_argument("Hokuto can't read it as a WAV file: " + error.Reason() + ".");
    }
    const std::size_t channels = wav.pcm.channels.size();
    if (channels > 2)
    {
        throw std::invalid_argument("It holds " + std::to_string(channels) + " channels. A wave holds one or two.");
    }
    const std::size_t total = wav.pcm.channels[0].size();
    if (total == 0)
    {
        throw std::invalid_argument("It holds no samples.");
    }

    // A wave plays up to the end of its loop. A WAV file without a loop takes the loop that the caller gives.
    if (!wav.pcm.loop && loop)
    {
        if (loop->start >= loop->end || loop->end > total)
        {
            throw std::invalid_argument("The loop from sample " + std::to_string(loop->start) + " to " +
                                        std::to_string(loop->end) + " doesn't lie inside the WAV file's " +
                                        std::to_string(total) + " samples.");
        }
        wav.pcm.loop = true;
        wav.pcm.loop_start = loop->start;
        wav.pcm.loop_end = loop->end;
    }
    incoming.encoding = encoding;
    incoming.channels = channels;
    incoming.rate = wav.rate;
    incoming.loop = wav.pcm.loop;
    incoming.loop_start = wav.pcm.loop ? wav.pcm.loop_start : 0;
    incoming.frames = wav.pcm.loop ? wav.pcm.loop_end : total;
    incoming.after_loop = total - incoming.frames;
    incoming.loop_from_present = incoming.loop && wav.loops == 0;
    incoming.wav_rate = wav.rate;
    incoming.wav_frames = incoming.frames;
    incoming.wav_loop_start = incoming.loop_start;
    incoming.bits = wav.bits;
    incoming.floating = wav.floating;
    incoming.loops = wav.loops;
    incoming.loop_forward = wav.loop_forward;
    incoming.loop_endless = wav.loop_endless;
    incoming.dropped = wav.dropped;
    for (std::vector<int16_t>& channel : wav.pcm.channels)
    {
        channel.resize(incoming.frames);
    }

    // A lower rate resamples the samples. The loop keeps its place.
    if (rate)
    {
        if (*rate == 0 || *rate >= wav.rate)
        {
            throw std::invalid_argument("A wave takes a rate lower than the WAV file's " + std::to_string(wav.rate) +
                                        " Hz only.");
        }
        const std::optional<std::size_t> start =
            incoming.loop ? std::optional<std::size_t>(incoming.loop_start) : std::nullopt;
        for (std::vector<int16_t>& channel : wav.pcm.channels)
        {
            channel = audio::Resample(channel, wav.rate, *rate, start);
        }
        incoming.rate = *rate;
        incoming.loop_start = incoming.loop ? static_cast<uint32_t>(audio::ResampledPlace(*start, wav.rate, *rate)) : 0;
        incoming.frames = wav.pcm.channels[0].size();
    }

    return std::move(wav.pcm.channels);
}

// The model of a wave made of `channels`: its samples as 16-bit PCM before the encoding, up to `loop_end`, with the
// encoding, the rate and the loop. The samples keep the repeat after a loop that moved.
format::WaveModel ModelOf(std::vector<std::vector<int16_t>> channels, sar::WaveEncoding encoding, uint32_t rate,
                          bool loop, uint32_t loop_start, uint32_t loop_end)
{
    format::WaveModel model;
    model.encoding = encoding;
    model.rate = rate;
    model.loop = loop;
    model.loop_start = loop ? loop_start : 0;
    model.loop_end = loop_end;
    model.length = loop_end;
    for (std::vector<int16_t>& channel : channels)
    {
        channel.resize(loop_end);
        model.samples.push_back(std::move(channel));
    }

    return model;
}

} // namespace

IncomingWave ReadIncomingWave(std::span<const uint8_t> bytes, const sar::SoundArchive& archive, uint32_t wave_archive,
                              std::optional<uint32_t> wave, sar::WaveEncoding encoding, std::optional<WaveLoop> loop,
                              std::optional<uint32_t> rate)
{
    if (encoding != sar::WaveEncoding::kDspAdpcm && encoding != sar::WaveEncoding::kPcm16)
    {
        throw std::invalid_argument("Hokuto makes a wave in DSP-ADPCM or in 16-bit PCM only.");
    }

    // The present wave gives the version. A new wave takes the version of the archive's waves.
    const uint32_t version = wave ? WaveFileOf(WaveArchiveFileOf(archive, wave_archive), *wave).version
                                  : IncomingVersions(archive, FileKind::kWaveArchive, wave_archive).waves;

    // The wave file, with the samples in the encoding. The original loop start is the WAV file's, at the wave's rate.
    IncomingWave incoming;
    std::vector<std::vector<int16_t>> channels = WavSamples(bytes, encoding, loop, rate, incoming);
    sar::WaveFile file;
    file.flavor = archive.ReadOptions().flavor;
    file.version = version;
    file.encoding = encoding;
    file.loop = incoming.loop;
    file.sample_rate = incoming.rate;
    file.original_loop_start = incoming.loop_start;
    if (encoding == sar::WaveEncoding::kDspAdpcm)
    {
        AddDspAdpcm(channels, incoming, file);
    }
    else
    {
        AddPcm16(channels, incoming, file, file.flavor == sar::Flavor::kCafe);
    }
    incoming.bytes = file.Write();
    incoming.version = version;
    incoming.model =
        ModelOf(std::move(channels), encoding, file.sample_rate, file.loop, file.loop_start, file.loop_end);

    return incoming;
}

IncomingWave ReadIncomingWiiWave(std::span<const uint8_t> bytes, uint16_t version, sar::WaveEncoding encoding,
                                 std::optional<WaveLoop> loop, std::optional<uint32_t> rate)
{
    // The wave, with the samples in the encoding. The Wii's waves keep no original loop start.
    IncomingWave incoming;
    std::vector<std::vector<int16_t>> channels = WavSamples(bytes, encoding, loop, rate, incoming);
    ntdlib::rsar::WaveFile file;
    file.version = version;
    file.encoding = encoding == sar::WaveEncoding::kDspAdpcm ? ntdlib::rsar::WaveEncoding::kDspAdpcm
                                                             : ntdlib::rsar::WaveEncoding::kPcm16;
    file.loop = incoming.loop;
    file.sample_rate = incoming.rate;
    if (encoding == sar::WaveEncoding::kDspAdpcm)
    {
        AddDspAdpcm(channels, incoming, file);
    }
    else
    {
        AddPcm16(channels, incoming, file, true);
    }
    try
    {
        incoming.bytes = file.Write();
    }
    catch (const std::invalid_argument& error)
    {
        throw std::invalid_argument(std::string("A wave of the Wii can't hold it. ") +
                                    format::AsSentence(error.what()));
    }
    incoming.version = version;
    incoming.model =
        ModelOf(std::move(channels), encoding, file.sample_rate, file.loop, file.loop_start, file.loop_end);

    return incoming;
}

bool IsWaveFile(std::span<const uint8_t> bytes)
{
    const std::string kMagic = "WAV";
    return bytes.size() >= 4 && (bytes[0] == 'C' || bytes[0] == 'F') &&
           std::equal(kMagic.begin(), kMagic.end(), bytes.begin() + 1);
}

bool IsWiiWaveFile(std::span<const uint8_t> bytes)
{
    const std::string kMagic = "RWAV";
    return bytes.size() >= 4 && std::equal(kMagic.begin(), kMagic.end(), bytes.begin());
}

IncomingWave ReadIncomingWiiWaveFile(std::span<const uint8_t> bytes)
{
    // A wave file of the Wii starts with RWAV and a big-endian byte order mark. The other consoles' wave files start
    // with CWAV or FWAV, and go in converted. The header gives the file's size.
    IncomingWave incoming;
    rsar::WaveFile read;
    if (IsWaveFile(bytes))
    {
        const bool big_endian = bytes.size() >= kHeaderSize && bytes[4] == 0xFE && bytes[5] == 0xFF;
        const bool little_endian = bytes.size() >= kHeaderSize && bytes[4] == 0xFF && bytes[5] == 0xFE;
        if (!big_endian && !little_endian)
        {
            throw std::invalid_argument(format::NotKindText(bytes, "a wave file"));
        }
        const xsflib::Reader r(bytes, big_endian);
        const uint32_t size = r.U32(kSizeField);
        if (size < kHeaderSize || size > bytes.size())
        {
            throw std::invalid_argument("Its header gives a size of " + std::to_string(size) +
                                        " bytes. The file holds " + std::to_string(bytes.size()) + " bytes.");
        }
        const sar::Flavor flavor = bytes[0] == 'C' ? sar::Flavor::kCtr
                                   : big_endian    ? sar::Flavor::kCafe
                                                   : sar::Flavor::kNx;
        try
        {
            read = rsar::WiiWaveOf(sar::WaveFile::Read(bytes.first(size)));
            incoming.bytes = read.Write();
        }
        catch (const std::exception& error)
        {
            throw std::invalid_argument(std::string("Hokuto can't convert the wave for the Wii. ") +
                                        format::AsSentence(error.what()));
        }
        incoming.version = read.version;
        incoming.converted = ConvertedFile{flavor, r.U32(kVersionField), size, std::nullopt};
    }
    else
    {
        if (!IsWiiWaveFile(bytes) || bytes.size() < kWiiHeaderSize || bytes[4] != 0xFE || bytes[5] != 0xFF)
        {
            throw std::invalid_argument(format::NotKindText(bytes, "a wave file"));
        }
        const xsflib::Reader r(bytes, true);
        const uint32_t size = r.U32(kWiiSizeField);
        if (size < kWiiHeaderSize || size > bytes.size())
        {
            throw std::invalid_argument("Its header gives a size of " + std::to_string(size) +
                                        " bytes. The file holds " + std::to_string(bytes.size()) + " bytes.");
        }
        incoming.bytes.assign(bytes.begin(), bytes.begin() + size);
        incoming.version = r.U16(kWiiVersionField);
        try
        {
            read = rsar::WaveFile::Read(incoming.bytes);
        }
        catch (const std::exception& error)
        {
            throw std::invalid_argument(std::string("Hokuto can't read the wave. ") + format::AsSentence(error.what()));
        }
    }
    incoming.encoding = static_cast<sar::WaveEncoding>(read.encoding);
    incoming.channels = read.channels.size();
    incoming.rate = read.sample_rate;
    incoming.frames = read.loop_end;
    incoming.loop = read.loop;
    incoming.loop_start = read.loop ? read.loop_start : 0;

    return incoming;
}

IncomingWave ReadIncomingWaveFile(std::span<const uint8_t> bytes, const sar::SoundArchive& archive,
                                  uint32_t wave_archive, std::optional<uint32_t> wave)
{
    // A wave in the place of another has to find the present wave.
    const std::optional<uint32_t> present =
        wave ? std::optional<uint32_t>(WaveFileOf(WaveArchiveFileOf(archive, wave_archive), *wave).version)
             : std::nullopt;

    // The magic gives the console's letter. The byte order mark tells the Wii U from the Switch.
    const sar::Flavor flavor = archive.ReadOptions().flavor;
    const bool big_endian = bytes.size() >= kHeaderSize && bytes[4] == 0xFE && bytes[5] == 0xFF;
    const bool little_endian = bytes.size() >= kHeaderSize && bytes[4] == 0xFF && bytes[5] == 0xFE;
    if ((!big_endian && !little_endian) || !IsWaveFile(bytes))
    {
        throw std::invalid_argument(format::NotKindText(bytes, "a wave file"));
    }
    const sar::Flavor console = bytes[0] == 'C' ? sar::Flavor::kCtr
                                : big_endian    ? sar::Flavor::kCafe
                                                : sar::Flavor::kNx;
    if (console == sar::Flavor::kCtr && big_endian)
    {
        throw std::invalid_argument(format::NotKindText(bytes, "a wave file"));
    }
    const xsflib::Reader r(bytes, big_endian);
    const uint32_t size = r.U32(kSizeField);
    if (size < kHeaderSize || size > bytes.size())
    {
        throw std::invalid_argument("Its header gives a size of " + std::to_string(size) + " bytes. The file holds " +
                                    std::to_string(bytes.size()) + " bytes.");
    }

    // A wave file of another console goes in converted, with the present wave's version or that of the archive's waves.
    // So does a wave file of the archive's console with a later version. A game may refuse a later version than its
    // waves hold.
    IncomingWave incoming;
    incoming.bytes.assign(bytes.begin(), bytes.begin() + size);
    incoming.version = r.U32(kVersionField);
    const uint32_t version = present ? *present : IncomingVersions(archive, FileKind::kWaveArchive, wave_archive).waves;
    if (console != flavor || incoming.version > version)
    {
        incoming.converted = ConvertedFile{console, incoming.version, incoming.bytes.size(), std::nullopt};
        try
        {
            incoming.bytes =
                console != flavor
                    ? song::ConvertItemFile(incoming.bytes, song::ItemKind::kWave, console, flavor, {version, version})
                    : sar::ConvertWave(sar::WaveFile::Read(incoming.bytes), flavor, version).Write();
        }
        catch (const std::exception& error)
        {
            throw std::invalid_argument("Hokuto can't convert it for the " + song::ConsoleName(flavor) + ". " +
                                        format::AsSentence(error.what()));
        }
        incoming.version = version;
    }

    // What the wave holds.
    sar::WaveFile read;
    try
    {
        read = sar::WaveFile::Read(incoming.bytes);
    }
    catch (const std::exception& error)
    {
        throw std::invalid_argument(std::string("Hokuto can't read the wave. ") + format::AsSentence(error.what()));
    }
    incoming.encoding = read.encoding;
    incoming.channels = read.channels.size();
    incoming.rate = read.sample_rate;
    incoming.frames = read.loop_end;
    incoming.loop = read.loop;
    incoming.loop_start = read.loop ? read.loop_start : 0;

    return incoming;
}

WaveChanges ChangesOfWave(const sar::SoundArchive& archive, uint32_t wave_archive, uint32_t wave)
{
    const sar::WaveArchiveFile file = WaveArchiveFileOf(archive, wave_archive);
    const sar::WaveFile present = WaveFileOf(file, wave);

    WaveChanges changes;
    changes.encoding = present.encoding;
    changes.channels = present.channels.size();
    changes.rate = present.sample_rate;
    changes.loop = present.loop;
    changes.loop_start = present.loop_start;
    changes.loop_end = present.loop_end;
    changes.size = file.waves[wave].size();
    const auto named = [&](const sar::WaveId& id)
    {
        return Names(id, wave_archive) && id.index == wave;
    };
    changes.banks = BanksNaming(archive, named);
    changes.sounds = WaveSoundsPlaying(archive, named);

    return changes;
}

std::vector<uint8_t> WaveArchiveWith(const sar::SoundArchive& archive, uint32_t wave_archive,
                                     std::optional<uint32_t> wave, std::span<const uint8_t> wave_file)
{
    sar::WaveArchiveFile file = WaveArchiveFileOf(archive, wave_archive);
    if (wave)
    {
        CheckWave(file, *wave);
        file.waves[*wave].assign(wave_file.begin(), wave_file.end());
    }
    else
    {
        file.waves.emplace_back(wave_file.begin(), wave_file.end());
    }

    return file.Write();
}

std::vector<uint8_t> WaveArchiveWithNew(const sar::SoundArchive& archive, uint32_t wave_archive,
                                        std::span<const std::vector<uint8_t>> wave_files)
{
    sar::WaveArchiveFile file = WaveArchiveFileOf(archive, wave_archive);
    file.waves.insert(file.waves.end(), wave_files.begin(), wave_files.end());

    return file.Write();
}

WaveChanges ChangesOfWiiWave(const rsar::SoundArchive& archive, uint32_t file_id, uint32_t wave)
{
    const rsar::WaveFile present = PresentWave(archive, file_id, wave);
    WaveChanges changes;
    changes.encoding = static_cast<sar::WaveEncoding>(present.encoding);
    changes.channels = present.channels.size();
    changes.rate = present.sample_rate;
    changes.loop = present.loop;
    changes.loop_start = present.loop_start;
    changes.loop_end = present.loop_end;
    changes.size = present.Write().size();

    // The banks of the file whose regions name the wave, and the wave sounds of the file whose notes play it. A file
    // that ntdlib can't read names nothing.
    const std::span<const uint8_t> bytes = archive.FileData(file_id);
    const std::vector<uint32_t> named = WavesIn(bytes, FileKind::kBank, {}).first;
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
        throw std::invalid_argument("The wave data holds no wave " + std::to_string(*wave) + ".");
    }
    rsar::WaveFile read;
    try
    {
        read = rsar::WaveFile::Read(incoming.bytes);
    }
    catch (const std::exception& error)
    {
        throw std::invalid_argument(std::string("Hokuto can't read the new wave. ") + format::AsSentence(error.what()));
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
