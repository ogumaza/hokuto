// SPDX-License-Identifier: MIT

#include "song/streams.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "ntdlib/rsar/convert.h"
#include "ntdlib/rsar/stream_file.h"
#include "ntdlib/sar/archive_model.h"
#include "ntdlib/sar/byte_order.h"
#include "ntdlib/sar/stream_file.h"
#include "seqlib/file_name.h"
#include "seqlib/pcm.h"
#include "seqlib/wav.h"
#include "xsflib/binary.h"
#include "xsflib/files.h"

#include "song/aac.h"
#include "song/items.h"

namespace rsar = ntdlib::rsar;
namespace sar = ntdlib::sar;

namespace hokuto::song
{
namespace
{

// The stream file in `stream`. Throws std::runtime_error with a message for the user when it can't be read.
sar::StreamFile ReadStream(std::span<const uint8_t> stream)
{
    try
    {
        return sar::StreamFile::Read(stream);
    }
    catch (const xsflib::FormatError& error)
    {
        throw std::runtime_error(std::string("Hokuto can't read the stream. ") + error.what());
    }
}

// The stream file of the Wii in `stream`, read the same way.
rsar::StreamFile ReadWiiStream(std::span<const uint8_t> stream)
{
    try
    {
        return rsar::StreamFile::Read(stream);
    }
    catch (const xsflib::FormatError& error)
    {
        throw std::runtime_error(std::string("Hokuto can't read the stream. ") + error.what());
    }
}

// Whether `bytes` start as a stream file of the Wii: RSTM, with the big-endian byte order mark.
bool IsWiiStream(std::span<const uint8_t> bytes)
{
    return bytes.size() >= 6 && std::memcmp(bytes.data(), "RSTM", 4) == 0 && bytes[4] == 0xFE && bytes[5] == 0xFF;
}

// `text`, a path that a file table gives, as a path from the archive's folder (StreamPath).
std::optional<std::filesystem::path> PathInFolder(std::string text)
{
    if (text.empty())
    {
        return std::nullopt;
    }

    // NintendoWare's paths part their folders with slashes. A backslash counts as one too.
    std::ranges::replace(text, '\\', '/');
    const std::filesystem::path path = xsflib::PathFromUtf8(text);
    if (path.has_root_name() || path.has_root_directory() || path.empty())
    {
        return std::nullopt;
    }
    for (const std::filesystem::path& part : path)
    {
        if (part == "..")
        {
            return std::nullopt;
        }
    }

    return path.lexically_normal();
}

// The paths of `files` in the archive's folder (StreamPaths).
template <class Archive>
std::vector<std::filesystem::path> PathsOf(const Archive& archive, std::span<const uint32_t> files)
{
    std::vector<std::filesystem::path> paths;
    for (const uint32_t file : files)
    {
        std::optional<std::filesystem::path> path =
            file < archive.FileCount() ? StreamPath(archive, file) : std::nullopt;
        if (!path)
        {
            throw std::runtime_error("The archive's file table gives stream file " + std::to_string(file) +
                                     " no path in the archive's folder.");
        }
        paths.push_back(std::move(*path));
    }

    return paths;
}

// The files beside the archive at `path` that the file table of `archive` names in the archive's folder, and for which
// `is_stream` holds (ReadStreams).
template <class Archive, class IsStream>
std::map<uint32_t, std::vector<uint8_t>> StreamsBeside(const Archive& archive, const std::filesystem::path& path,
                                                       const IsStream& is_stream)
{
    std::map<uint32_t, std::vector<uint8_t>> streams;
    for (uint32_t file = 0; file < archive.FileCount(); ++file)
    {
        const std::optional<std::filesystem::path> relative = StreamPath(archive, file);
        std::vector<uint8_t> bytes;
        if (relative && xsflib::ReadFile(path.parent_path() / *relative, bytes) && is_stream(bytes))
        {
            streams[file] = std::move(bytes);
        }
    }

    return streams;
}

// The ID of the file that stream sound `sound` plays beside the archive (StreamFileOf).
template <class Archive, class SoundType>
std::optional<uint32_t> StreamFileOfSound(const Archive& archive, uint32_t sound, SoundType stream)
{
    if (sound >= archive.Sounds().size() || archive.Sounds()[sound].type != stream ||
        !StreamPath(archive, archive.Sounds()[sound].file_id))
    {
        return std::nullopt;
    }

    return archive.Sounds()[sound].file_id;
}

// The name of the stream file of stream sound `sound`, with `extension` for a sound without a path (StreamFileName).
template <class Archive>
std::string StreamFileNameOf(const Archive& archive, uint32_t sound, const char* extension)
{
    const std::optional<uint32_t> file = StreamFileOf(archive, sound);
    const std::optional<std::filesystem::path> path = file ? StreamPath(archive, *file) : std::nullopt;
    if (path && path->has_filename())
    {
        return seqlib::SafeFileName(xsflib::Utf8(path->filename()));
    }

    return seqlib::SafeFileName(archive.Sounds().at(sound).name + extension);
}

} // namespace

std::optional<std::filesystem::path> StreamPath(const sar::SoundArchive& archive, uint32_t file)
{
    return PathInFolder(archive.ExternalPath(file));
}

std::optional<std::filesystem::path> StreamPath(const rsar::SoundArchive& archive, uint32_t file)
{
    std::optional<std::string> text = archive.ExternalPath(file);
    return text ? PathInFolder(std::move(*text)) : std::nullopt;
}

std::vector<std::filesystem::path> StreamPaths(const sar::SoundArchive& archive, std::span<const uint32_t> files)
{
    return PathsOf(archive, files);
}

std::vector<std::filesystem::path> StreamPaths(const rsar::SoundArchive& archive, std::span<const uint32_t> files)
{
    return PathsOf(archive, files);
}

std::optional<std::string> WriteStream(const std::filesystem::path& file, std::span<const uint8_t> bytes)
{
    std::error_code error;
    if (file.has_parent_path())
    {
        std::filesystem::create_directories(file.parent_path(), error);
        if (error)
        {
            return FolderFailure(xsflib::Utf8(file.parent_path().filename()), error);
        }
    }
    if (xsflib::WriteFile(file, bytes))
    {
        return WriteFailure(xsflib::Utf8(file.filename()));
    }

    return std::nullopt;
}

std::map<uint32_t, std::vector<uint8_t>> ReadStreams(const sar::SoundArchive& archive,
                                                     const std::filesystem::path& path)
{
    // A stream of the archive's console: CSTM on the 3DS and FSTM on the others, with the console's byte order mark. On
    // the 3DS, an AAC file in ADTS.
    const sar::Flavor flavor = archive.ReadOptions().flavor;
    const bool big_endian = flavor == sar::Flavor::kCafe;
    const bool ctr = flavor == sar::Flavor::kCtr;
    const char* const magic = ctr ? "CSTM" : "FSTM";
    const auto is_stream = [big_endian, magic, ctr](std::span<const uint8_t> bytes)
    {
        return (bytes.size() >= 6 && std::memcmp(bytes.data(), magic, 4) == 0 &&
                (bytes[4] == 0xFE && bytes[5] == 0xFF) == big_endian) ||
               (ctr && AdtsFrames(bytes) > 0);
    };
    return StreamsBeside(archive, path, is_stream);
}

std::map<uint32_t, std::vector<uint8_t>> ReadStreams(const rsar::SoundArchive& archive,
                                                     const std::filesystem::path& path)
{
    return StreamsBeside(archive, path, IsWiiStream);
}

std::optional<uint32_t> StreamFileOf(const sar::SoundArchive& archive, uint32_t sound)
{
    return StreamFileOfSound(archive, sound, sar::SoundType::kStream);
}

std::optional<uint32_t> StreamFileOf(const rsar::SoundArchive& archive, uint32_t sound)
{
    return StreamFileOfSound(archive, sound, rsar::SoundType::kStream);
}

std::string StreamFileName(const sar::SoundArchive& archive, uint32_t sound)
{
    const bool ctr = archive.ReadOptions().flavor == sar::Flavor::kCtr;
    return StreamFileNameOf(archive, sound, ctr ? ".bcstm" : ".bfstm");
}

std::string StreamFileName(const rsar::SoundArchive& archive, uint32_t sound)
{
    return StreamFileNameOf(archive, sound, ".brstm");
}

std::optional<AacStream> AacStreamOf(const sar::SoundArchive& archive, uint32_t sound)
{
    constexpr uint32_t kAac = 2;
    constexpr uint32_t kLooped = 0x100;
    if (archive.ReadOptions().flavor != sar::Flavor::kCtr || sound >= archive.Sounds().size() ||
        archive.Sounds()[sound].type != sar::SoundType::kStream)
    {
        return std::nullopt;
    }

    // ntdlib's reader of the 3DS's entries leaves the extension out. Its model reads it.
    sar::ArchiveModel model;
    try
    {
        model = sar::ReadArchiveModel(archive.Bytes(), sar::Flavor::kCtr);
    }
    catch (const std::exception&)
    {
        return std::nullopt;
    }
    if (sound >= model.sounds.size() || !model.sounds[sound].stream_extension ||
        (model.sounds[sound].stream_extension->stream_type & 0xFF) != kAac)
    {
        return std::nullopt;
    }

    const sar::StreamExtension& extension = *model.sounds[sound].stream_extension;
    AacStream aac;
    if ((extension.stream_type & kLooped) != 0 && extension.loop_start < extension.loop_end)
    {
        aac.loop = AacLoop{extension.loop_start, extension.loop_end};
    }

    return aac;
}

sar::Pcm DecodeStreamFile(std::span<const uint8_t> stream, const AacStream& aac)
{
    // An AAC stream ends at its loop's end. A loop past the samples ends with them.
    if (IsAdts(stream))
    {
        sar::Pcm pcm = DecodeAdts(stream);
        const uint32_t length = pcm.loop_end;
        if (aac.loop && aac.loop->start < length)
        {
            pcm.loop = true;
            pcm.loop_start = aac.loop->start;
            pcm.loop_end = std::min(aac.loop->end, length);
            for (std::vector<int16_t>& channel : pcm.channels)
            {
                channel.resize(pcm.loop_end);
            }
        }

        return pcm;
    }

    try
    {
        return IsWiiStream(stream) ? rsar::DecodeStream(ReadWiiStream(stream)) : sar::DecodeStream(ReadStream(stream));
    }
    catch (const xsflib::FormatError& error)
    {
        throw std::runtime_error(std::string("Hokuto can't decode the stream. ") + error.what());
    }
}

std::vector<uint8_t> StreamAsWav(std::span<const uint8_t> stream, const AacStream& aac)
{
    sar::Pcm decoded = DecodeStreamFile(stream, aac);
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
        throw std::runtime_error(std::string("Hokuto can't write the stream as a WAV file. ") + error.what());
    }
}

std::vector<uint8_t> ConvertStreamFile(std::span<const uint8_t> stream, sar::Flavor to, uint32_t version,
                                       const AacStream& aac)
{
    if (IsAdts(stream))
    {
        const sar::Pcm pcm = DecodeStreamFile(stream, aac);
        try
        {
            return sar::MakeStream(to, version, sar::WaveEncoding::kDspAdpcm, pcm.sample_rate, pcm.channels,
                                   pcm.loop ? std::optional<uint32_t>(pcm.loop_start) : std::nullopt)
                .Write();
        }
        catch (const std::invalid_argument& error)
        {
            throw std::runtime_error(std::string("Hokuto can't convert the stream. ") + error.what());
        }
    }
    if (IsWiiStream(stream))
    {
        const rsar::StreamFile file = ReadWiiStream(stream);
        try
        {
            return rsar::StreamFileOf(file, to, version).Write();
        }
        catch (const std::invalid_argument& error)
        {
            throw std::runtime_error(std::string("Hokuto can't convert the stream. ") + error.what());
        }
    }

    const sar::StreamFile file = ReadStream(stream);
    try
    {
        return sar::ConvertStream(file, to, version).Write();
    }
    catch (const std::invalid_argument& error)
    {
        throw std::runtime_error(std::string("Hokuto can't convert the stream. ") + error.what());
    }
}

std::vector<uint8_t> WiiStreamFile(std::span<const uint8_t> stream, const AacStream& aac)
{
    if (IsWiiStream(stream))
    {
        throw std::runtime_error("The stream is a stream of the Wii already.");
    }

    try
    {
        if (IsAdts(stream))
        {
            const sar::Pcm pcm = DecodeStreamFile(stream, aac);
            return rsar::MakeStream(rsar::WaveEncoding::kDspAdpcm, pcm.sample_rate, pcm.channels,
                                    pcm.loop ? std::optional<uint32_t>(pcm.loop_start) : std::nullopt)
                .Write();
        }

        return rsar::WiiStreamOf(ReadStream(stream)).Write();
    }
    catch (const std::invalid_argument& error)
    {
        throw std::runtime_error(std::string("Hokuto can't convert the stream for the Wii. ") + error.what());
    }
}

std::vector<uint8_t> PlayableStream(std::span<const uint8_t> stream, const AacStream& aac)
{
    if (!IsAdts(stream))
    {
        return std::vector<uint8_t>(stream.begin(), stream.end());
    }

    // The 3DS's tools write stream files of 2.0.0.0 and later.
    constexpr uint32_t kCtrStreamVersion = 0x02000000;
    const sar::Pcm pcm = DecodeStreamFile(stream, aac);
    try
    {
        return sar::MakeStream(sar::Flavor::kCtr, kCtrStreamVersion, sar::WaveEncoding::kPcm16, pcm.sample_rate,
                               pcm.channels, pcm.loop ? std::optional<uint32_t>(pcm.loop_start) : std::nullopt)
            .Write();
    }
    catch (const std::invalid_argument& error)
    {
        throw std::runtime_error(std::string("Hokuto can't play the stream. ") + error.what());
    }
}

} // namespace hokuto::song
