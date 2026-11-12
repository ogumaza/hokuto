// SPDX-License-Identifier: MIT

// The definitions of the helpers that files_internal.h declares.

#include "edit/files_internal.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/nwsnd/commands.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sar/wave_file.h"
#include "seqlib/read_error.h"
#include "seqlib/wav.h"
#include "xsflib/binary.h"

#include "format/archive.h"
#include "format/kinds.h"
#include "song/archive.h"

namespace hokuto::edit
{
namespace
{

// The byte order mark of a NintendoWare file's header.
constexpr std::size_t kByteOrderField = 0x04;
constexpr uint8_t kMarkHigh = 0xFE;
constexpr uint8_t kMarkLow = 0xFF;

// A bank entry of the INFO block: its file at 0, then a reference to its list of wave archives. The reference's type
// sits at 4 and its offset from the entry's start at 8. The list holds a count, then the wave archives' item IDs.
constexpr std::size_t kListReference = 4;
constexpr uint16_t kListType = 0x0100;

} // namespace

bool MarksBigEndian(std::span<const uint8_t> bytes)
{
    return bytes.size() > kByteOrderField + 1 && bytes[kByteOrderField] == kMarkHigh &&
           bytes[kByteOrderField + 1] == kMarkLow;
}

bool MarksLittleEndian(std::span<const uint8_t> bytes)
{
    return bytes.size() > kByteOrderField + 1 && bytes[kByteOrderField] == kMarkLow &&
           bytes[kByteOrderField + 1] == kMarkHigh;
}

namespace
{

namespace sar = ntdlib::sar;

} // namespace

sar::Options StrictOptions(sar::Flavor flavor)
{
    sar::Options options;
    options.flavor = flavor;
    options.rules = sar::Rules::kStopAtFirstProblem;

    return options;
}

WiiSequenceFile WiiSequenceFileOf(const ntdlib::commands::Sequence& sequence, const format::NativeArchive& native,
                                  uint16_t version)
{
    ntdlib::nwsnd::Encoded encoded;
    try
    {
        const ntdlib::SndBuild build = format::BuildOfWiiArchive(native);
        encoded = ntdlib::nwsnd::EncodeSequence(
            sequence, song::GrammarOf(song::Console{.engine = format::Engine::kWii, .wii = build}));
    }
    catch (const std::exception& error)
    {
        throw std::invalid_argument(std::string("Can't write the new commands for the Wii. ") +
                                    format::AsSentence(error.what()));
    }

    WiiSequenceFile made;
    made.file.version = version;
    made.file.data = std::move(encoded.bytes);
    for (const ntdlib::commands::Label& label : sequence.labels)
    {
        made.file.labels.push_back({label.name, encoded.offsets.at(label.item)});
    }

    made.offsets = std::move(encoded.offsets);

    return made;
}

sar::WaveArchiveFile WaveArchiveFileOf(const sar::SoundArchive& archive, uint32_t wave_archive)
{
    const uint32_t file_id = FileIdOf(archive, FileKind::kWaveArchive, wave_archive);
    try
    {
        return sar::WaveArchiveFile::Read(archive.FileData(file_id));
    }
    catch (const std::exception& error)
    {
        throw std::invalid_argument(std::string("Can't read the wave archive. ") + format::AsSentence(error.what()));
    }
}

void CheckWave(const sar::WaveArchiveFile& file, uint32_t wave)
{
    if (wave >= file.waves.size())
    {
        throw std::invalid_argument("The wave archive has " + std::to_string(file.waves.size()) +
                                    (file.waves.size() == 1 ? " wave" : " waves") + ". There's no wave " +
                                    std::to_string(wave) + ".");
    }
}

sar::WaveFile WaveFileOf(const sar::WaveArchiveFile& file, uint32_t wave)
{
    CheckWave(file, wave);
    try
    {
        return sar::WaveFile::Read(file.waves[wave]);
    }
    catch (const std::exception& error)
    {
        throw std::invalid_argument(std::string("Can't read the current wave. ") + format::AsSentence(error.what()));
    }
}

std::optional<std::size_t> WaveArchiveListOf(std::span<const uint8_t> entry, sar::Flavor flavor)
{
    const xsflib::Reader r(entry, flavor == sar::Flavor::kCafe);
    if (entry.size() < kListReference + 8 || r.U16(kListReference) != kListType)
    {
        return std::nullopt;
    }

    const std::size_t list = r.U32(kListReference + 4);
    if (list + 4 > entry.size())
    {
        return std::nullopt;
    }

    return list;
}

uint32_t PresentWaveVersion(const sar::SoundArchive& archive, uint32_t wave_archive, uint32_t wave)
{
    return WaveFileOf(WaveArchiveFileOf(archive, wave_archive), wave).version;
}

std::map<uint32_t, uint32_t> SequencesByFileId(const std::vector<std::optional<uint32_t>>& sequences)
{
    std::map<uint32_t, uint32_t> by_file;
    for (uint32_t id = 0; id < sequences.size(); ++id)
    {
        if (sequences[id])
        {
            by_file[id] = *sequences[id];
        }
    }

    return by_file;
}

SequenceNumbering SequenceNumbers(const std::map<uint32_t, uint32_t>& before, const std::map<uint32_t, uint32_t>& after,
                                  uint32_t kept, uint32_t count)
{
    std::map<uint32_t, uint32_t> numbers;
    for (const auto& [id, sequence] : before)
    {
        const auto found = after.find(id);
        if (found == after.end() || numbers.emplace(found->second, sequence).first->second != sequence)
        {
            throw std::invalid_argument("After adding the items, can't find the existing sequences in the archive.");
        }
    }

    uint32_t next = kept;
    for (uint32_t c = 0; c < count; ++c)
    {
        if (numbers.try_emplace(c, next).second)
        {
            ++next;
        }
    }

    return {std::move(numbers), next};
}

std::string FileText(FileKind kind)
{
    return kind == FileKind::kWaveSoundFile ? NameOf(kind) : NameOf(kind) + " file";
}

seqlib::WavContents ReadWavFile(std::span<const uint8_t> bytes)
{
    try
    {
        return seqlib::ReadWav(bytes);
    }
    catch (const seqlib::ReadError& error)
    {
        throw std::invalid_argument("Can't read it as a WAV file: " + error.Reason() + ".");
    }
}

void TakeWavDetails(const seqlib::WavContents& wav, IncomingWave& incoming)
{
    incoming.bits = wav.bits;
    incoming.floating = wav.floating;
    incoming.loops = wav.loops;
    incoming.loop_forward = wav.loop_forward;
    incoming.loop_endless = wav.loop_endless;
    incoming.dropped = wav.dropped;
}

void AddNoise(std::span<const int16_t> original, std::span<const int16_t> decoded, NoiseSums& sums)
{
    for (std::size_t i = 0; i < std::min(original.size(), decoded.size()); i++)
    {
        const double difference = static_cast<double>(original[i]) - static_cast<double>(decoded[i]);
        sums.signal += static_cast<double>(original[i]) * static_cast<double>(original[i]);
        sums.noise += difference * difference;
    }
}

std::optional<double> SignalToNoise(const NoiseSums& sums)
{
    if (sums.noise > 0.0 && sums.signal > 0.0)
    {
        return 10.0 * std::log10(sums.signal / sums.noise);
    }

    return std::nullopt;
}

} // namespace hokuto::edit
