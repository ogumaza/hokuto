// SPDX-License-Identifier: MIT

#include "edit/streams.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/rsar/stream_file.h"
#include "ntdlib/sar/archive_model.h"
#include "ntdlib/sar/byte_order.h"
#include "ntdlib/sar/encodings.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sar/stream_file.h"
#include "ntdlib/wave/adpcm.h"
#include "ntdlib/wave/decode.h"
#include "ntdlib/wave/encode.h"
#include "seqlib/wav.h"
#include "xsflib/binary.h"
#include "xsflib/bytes.h"

#include "edit/files_internal.h"
#include "edit/streams_parts.h"
#include "format/kinds.h"
#include "format/model.h"
#include "song/items.h"
#include "song/streams.h"

namespace hokuto::edit
{
namespace
{

namespace rsar = ntdlib::rsar;
namespace sar = ntdlib::sar;
namespace wave = ntdlib::wave;
using streams_parts::CheckValues;
using streams_parts::FinishWavStream;
using streams_parts::LaterStream;
using streams_parts::ReadLater;
using streams_parts::ReadLaterStream;
using streams_parts::ReadWavStream;
using streams_parts::ReadWii;
using streams_parts::WavStream;
using streams_parts::WiiStreamWithValues;

// The versions of the streams that Hokuto writes: 3.0.0 and 4.0.0. Most of the Wii U's games read 3.0.0.
constexpr uint32_t kOldStreamVersion = 0x00030000;
constexpr uint32_t kNewStreamVersion = 0x00040000;

// The most channels that a stream file's settings can count.
constexpr std::size_t kMostChannels = 255;

// Fills the fields of `incoming` that describe `stream`.
void Describe(const sar::StreamFile& stream, IncomingStream& incoming)
{
    incoming.version = stream.version;
    incoming.encoding = stream.encoding;
    incoming.channels = stream.channels.size();
    incoming.rate = stream.sample_rate;
    incoming.frames = stream.sample_count;
    incoming.loop = stream.loop;
    incoming.loop_start = stream.loop ? stream.loop_start : 0;
}

} // namespace

namespace streams_parts
{

sar::StreamFile ReadLater(std::span<const uint8_t> bytes, const char* start)
{
    try
    {
        return sar::StreamFile::Read(bytes);
    }
    catch (const std::exception& error)
    {
        throw std::invalid_argument(start + format::AsSentence(error.what()));
    }
}

LaterStream ReadLaterStream(std::span<const uint8_t> bytes)
{
    // The magic gives the console's letter. The byte order mark tells the Wii U from the Switch.
    const bool big_endian = bytes.size() >= kHeaderSize && MarksBigEndian(bytes);
    const bool little_endian = bytes.size() >= kHeaderSize && MarksLittleEndian(bytes);
    if ((!big_endian && !little_endian) || !IsStreamFile(bytes))
    {
        throw std::invalid_argument(format::NotKindText(bytes, "a stream file"));
    }

    LaterStream read;
    read.console = bytes[0] == 'C' ? sar::Flavor::kCtr : big_endian ? sar::Flavor::kCafe : sar::Flavor::kNx;
    const xsflib::Reader r(bytes, big_endian);
    read.version = r.U32(kVersionField);
    const uint32_t size = r.U32(kSizeField);
    if (size < kHeaderSize || size > bytes.size())
    {
        throw std::invalid_argument("Its header gives a size of " + std::to_string(size) + " bytes, but the file has " +
                                    std::to_string(bytes.size()) + " bytes.");
    }

    read.bytes.assign(bytes.begin(), bytes.begin() + size);
    read.stream = ReadLater(read.bytes);

    return read;
}

WavStream ReadWavStream(std::span<const uint8_t> bytes, sar::WaveEncoding encoding)
{
    if (encoding != sar::WaveEncoding::kDspAdpcm && encoding != sar::WaveEncoding::kPcm16)
    {
        throw std::invalid_argument("Hokuto can encode streams only as DSP-ADPCM or 16-bit PCM.");
    }

    // The WAV file's samples as 16 bits.
    seqlib::WavContents wav = ReadWavFile(bytes);

    const std::size_t channels = wav.pcm.channels.size();
    if (channels > kMostChannels)
    {
        throw std::invalid_argument("It has " + std::to_string(channels) + " channels. A stream can have up to " +
                                    std::to_string(kMostChannels) + ".");
    }

    const std::size_t total = wav.pcm.channels[0].size();
    if (total == 0)
    {
        throw std::invalid_argument("It has no samples.");
    }

    // A stream plays up to the end of its loop.
    WavStream stream;
    IncomingStream& incoming = stream.incoming;
    incoming.loop = wav.pcm.loop;
    incoming.frames = wav.pcm.loop ? wav.pcm.loop_end : total;
    incoming.after_loop = total - incoming.frames;
    TakeWavDetails(wav, incoming);
    for (std::vector<int16_t>& channel : wav.pcm.channels)
    {
        channel.resize(incoming.frames);
    }

    stream.rate = wav.rate;
    stream.channels = std::move(wav.pcm.channels);
    stream.loop_start = wav.pcm.loop ? std::optional<uint32_t>(wav.pcm.loop_start) : std::nullopt;

    return stream;
}

void FinishWavStream(WavStream& wav, uint32_t loop_start, const std::vector<std::vector<int16_t>>* decoded)
{
    IncomingStream& incoming = wav.incoming;
    incoming.frames = wav.channels[0].size();
    incoming.loop_start = wav.loop_start.value_or(0);
    incoming.loop_moved = wav.loop_start ? loop_start - *wav.loop_start : 0;
    incoming.wav_rate = wav.rate;
    incoming.wav_frames = incoming.frames;
    incoming.wav_loop_start = incoming.loop_start;

    // DSP-ADPCM loses some detail. The decoder gives the samples that the console plays.
    if (decoded == nullptr)
    {
        return;
    }

    double signal = 0.0;
    double noise = 0.0;
    for (std::size_t c = 0; c < wav.channels.size(); c++)
    {
        for (std::size_t i = 0; i < incoming.frames; i++)
        {
            const double sample = wav.channels[c][i];
            const double error = sample - (*decoded)[c][i];
            signal += sample * sample;
            noise += error * error;
        }
    }
    if (noise > 0.0)
    {
        incoming.signal_to_noise = 10.0 * std::log10(signal / noise);
    }
}

void CheckValues(const StreamValues& values, uint32_t samples)
{
    if (values.rate == 0)
    {
        throw std::invalid_argument("The sample rate must be above 0.");
    }
    if (values.length == 0 || values.length > samples)
    {
        throw std::invalid_argument("The length must be from 1 to " + std::to_string(samples) + " samples.");
    }
    if (values.loop && values.loop_start >= values.length)
    {
        throw std::invalid_argument("The loop must start before the end of the stream.");
    }
}

} // namespace streams_parts

bool IsStreamFile(std::span<const uint8_t> bytes)
{
    const std::string kMagic = "STM";
    return bytes.size() >= 4 && (bytes[0] == 'C' || bytes[0] == 'F') &&
           std::equal(kMagic.begin(), kMagic.end(), bytes.begin() + 1);
}

bool TracksInFiles(const sar::SoundArchive& archive)
{
    return archive.ReadOptions().flavor == sar::Flavor::kCtr && archive.Version() < sar::kCtrTracksVersion;
}

uint32_t IncomingStreamVersion(const format::File& file, sar::Flavor flavor)
{
    if (flavor == sar::Flavor::kNx)
    {
        return kNewStreamVersion;
    }

    // A game reads the streams that its tools wrote. A file of the Wii U holds its header's fields big-endian.
    uint32_t latest = 0;
    if (file.native)
    {
        for (const auto& [id, stream] : file.native->streams)
        {
            if (stream.from_game && stream.bytes->size() >= kHeaderSize)
            {
                const xsflib::Reader r(*stream.bytes, flavor == sar::Flavor::kCafe);
                latest = std::max(latest, r.U32(kVersionField));
            }
        }
    }

    if (flavor == sar::Flavor::kCtr)
    {
        return latest != 0 ? latest : song::ExportVersions(song::ItemKind::kStream, flavor).file;
    }

    return latest >= kNewStreamVersion ? kNewStreamVersion : kOldStreamVersion;
}

IncomingStream ReadIncomingStreamFile(std::span<const uint8_t> bytes, sar::Flavor flavor, uint32_t version)
{
    // A stream file of the archive's console goes in as it is, unless it holds a later version than a stream of the
    // Wii U or the 3DS takes. One of another console goes in converted.
    LaterStream read = ReadLaterStream(bytes);
    IncomingStream incoming;
    incoming.bytes = std::move(read.bytes);
    sar::StreamFile stream = std::move(read.stream);
    const bool later = (flavor == sar::Flavor::kCafe || flavor == sar::Flavor::kCtr) && stream.version > version;
    if (read.console != flavor || later)
    {
        incoming.converted = ConvertedFile{read.console, read.version, incoming.bytes.size(), std::nullopt};
        try
        {
            stream = sar::ConvertStream(stream, flavor, version);
            incoming.bytes = stream.Write();
        }
        catch (const std::exception& error)
        {
            throw std::invalid_argument("Can't convert it for the " + song::ConsoleName(flavor) + ". " +
                                        format::AsSentence(error.what()));
        }
    }

    Describe(stream, incoming);

    return incoming;
}

IncomingStream ReadIncomingStreamWav(std::span<const uint8_t> bytes, sar::Flavor flavor, uint32_t version,
                                     sar::WaveEncoding encoding)
{
    // The stream, laid out as the tools lay it out. Its loop starts at a block.
    WavStream wav = ReadWavStream(bytes, encoding);
    sar::StreamFile stream;
    try
    {
        stream = sar::MakeStream(flavor, version, encoding, wav.rate, wav.channels, wav.loop_start);
        wav.incoming.bytes = stream.Write();
    }
    catch (const std::invalid_argument& error)
    {
        throw std::invalid_argument(std::string("Can't make a stream from it. ") + format::AsSentence(error.what()));
    }

    Describe(stream, wav.incoming);
    if (encoding == sar::WaveEncoding::kDspAdpcm)
    {
        const sar::Pcm decoded = sar::DecodeStream(stream);
        FinishWavStream(wav, stream.loop_start, &decoded.channels);
    }
    else
    {
        FinishWavStream(wav, stream.loop_start, nullptr);
    }

    return std::move(wav.incoming);
}

StreamChanges ChangesOfStream(const format::File& file, const sar::SoundArchive& archive, uint32_t sound)
{
    if (sound >= archive.Sounds().size() || archive.Sounds()[sound].type != sar::SoundType::kStream)
    {
        throw std::invalid_argument("It isn't a stream sound.");
    }

    const std::optional<uint32_t> id = song::StreamFileOf(archive, sound);
    if (!id)
    {
        throw std::invalid_argument("Only streams in separate files beside the archive can be replaced.");
    }

    // The other sounds that play the file, and the tracks of the sound.
    StreamChanges changes;
    changes.file = *id;
    for (uint32_t other = 0; other < archive.Sounds().size(); other++)
    {
        if (other != sound && song::StreamFileOf(archive, other) == id)
        {
            changes.sounds.push_back(other);
        }
    }

    const sar::StreamSoundInfo& info = archive.Sounds()[sound].stream;
    changes.table_channels = info.channel_count;
    for (const sar::StreamTrackInfo& track : info.tracks)
    {
        changes.tracks.push_back(track.channels);
    }

    changes.tracks_in_file = TracksInFiles(archive);

    // The stream that the file holds.
    if (!file.native)
    {
        return changes;
    }

    const auto found = file.native->streams.find(*id);
    if (found == file.native->streams.end())
    {
        return changes;
    }

    const sar::StreamFile stream = ReadLater(*found->second.bytes, "Can't read the current stream. ");

    changes.present = true;
    changes.encoding = stream.encoding;
    changes.channels = stream.channels.size();
    changes.rate = stream.sample_rate;
    changes.loop = stream.loop;
    changes.loop_start = stream.loop ? stream.loop_start : 0;
    changes.length = stream.sample_count;
    changes.size = found->second.bytes->size();
    changes.from_game = found->second.from_game;
    if (changes.tracks_in_file && stream.tracks)
    {
        changes.file_tracks = *stream.tracks;
        for (const sar::StreamFile::Track& track : *stream.tracks)
        {
            changes.tracks.push_back(track.channels);
        }
    }

    return changes;
}

std::optional<std::vector<sar::StreamFile::Track>> SoundTracksFor(const IncomingStream& incoming,
                                                                  const StreamChanges& changes)
{
    if (!changes.tracks_in_file)
    {
        return std::nullopt;
    }

    sar::StreamFile stream = ReadLater(incoming.bytes);

    if (stream.tracks && !stream.tracks->empty())
    {
        return std::nullopt;
    }

    // The present stream's tracks, when the new stream holds each channel that they play.
    const auto held = [&stream](uint8_t channel)
    {
        return channel < stream.channels.size();
    };
    const auto holds = [&held](const sar::StreamFile::Track& track)
    {
        return std::ranges::all_of(track.channels, held);
    };
    const bool fit = !changes.file_tracks.empty() && std::ranges::all_of(changes.file_tracks, holds);
    if (fit)
    {
        return changes.file_tracks;
    }

    std::vector<sar::StreamFile::Track> tracks;
    for (std::size_t c = 0; c < stream.channels.size(); c += 2)
    {
        sar::StreamFile::Track& track = tracks.emplace_back();
        track.channels.push_back(static_cast<uint8_t>(c));
        if (c + 1 < stream.channels.size())
        {
            track.channels.push_back(static_cast<uint8_t>(c + 1));
        }
    }

    return tracks;
}

IncomingStream WithSoundTracks(IncomingStream incoming, const StreamChanges& changes)
{
    const std::optional<std::vector<sar::StreamFile::Track>> tracks = SoundTracksFor(incoming, changes);
    if (!tracks)
    {
        return incoming;
    }

    try
    {
        sar::StreamFile stream = sar::StreamFile::Read(incoming.bytes);
        stream.tracks = *tracks;
        incoming.bytes = stream.Write();
    }
    catch (const std::exception& error)
    {
        throw std::invalid_argument(std::string("Can't set the tracks of the stream. ") +
                                    format::AsSentence(error.what()));
    }

    return incoming;
}

format::File ReplaceStream(const format::File& file, uint32_t file_id, std::vector<uint8_t> bytes)
{
    if (!file.native)
    {
        throw std::invalid_argument("The file contains no game archive.");
    }

    format::File result = file;
    const auto take_stream = [file_id, &bytes](format::NativeArchive& native)
    {
        const auto found = native.streams.find(file_id);
        const bool same = found != native.streams.end() && *found->second.bytes == bytes;
        const bool from_game = same && found->second.from_game;
        native.streams[file_id] = format::NativeStream{std::move(bytes), from_game};
    };
    result.native.Edit(take_stream);

    return result;
}

StreamValues StreamValuesOf(std::span<const uint8_t> bytes)
{
    if (IsWiiStreamFile(bytes))
    {
        const rsar::StreamFile stream = ReadWii(bytes);
        return {sar::EncodingOf(stream.encoding), stream.sample_rate, stream.loop, stream.loop ? stream.loop_start : 0,
                stream.sample_count};
    }

    sar::StreamFile stream = ReadLater(bytes);

    return {stream.encoding, stream.sample_rate, stream.loop, stream.loop ? stream.loop_start : 0, stream.sample_count};
}

std::vector<uint8_t> StreamWithValues(std::span<const uint8_t> bytes, const StreamValues& values)
{
    if (IsWiiStreamFile(bytes))
    {
        return WiiStreamWithValues(bytes, values);
    }

    sar::StreamFile stream = ReadLater(bytes);

    CheckValues(values, stream.sample_count);

    // A rate, and a loop from a block to the stream's end, keep the encoded samples. The 3DS's tools never start a loop
    // at the first block, and a loop there is made again. A 3DS stream that keeps its samples keeps the samples past
    // its end too. A looped one leaves none of its last block's samples out of the block's count.
    const bool ctr = stream.flavor == sar::Flavor::kCtr;
    const bool adpcm = stream.encoding == sar::WaveEncoding::kDspAdpcm;
    const uint32_t loop_start = values.loop ? values.loop_start : 0;
    const bool same_loop = values.loop == stream.loop && loop_start == (stream.loop ? stream.loop_start : 0);
    const bool at_block = !values.loop || (loop_start % stream.block_samples == 0 && (!ctr || loop_start > 0));
    const bool keeps = values.encoding == stream.encoding && values.length == stream.sample_count && at_block;
    try
    {
        if (keeps && same_loop)
        {
            stream.sample_rate = values.rate;

            return stream.Write();
        }

        if (keeps)
        {
            stream.sample_rate = values.rate;
            stream.loop = values.loop;
            stream.loop_start = loop_start;
            stream.original_loop_start = stream.loop_start;
            stream.original_loop_end = stream.sample_count;
            if (ctr && stream.loop)
            {
                stream.uncounted_samples = 0;
            }

            for (sar::StreamFile::Channel& channel : stream.channels)
            {
                if (adpcm && values.loop && channel.adpcm)
                {
                    const wave::DspAdpcmState state =
                        wave::DspAdpcmStateAt(channel.samples, stream.loop_start, channel.adpcm->coefs,
                                              channel.adpcm->yn1, channel.adpcm->yn2, wave::DspAdpcmSum::kWrap);
                    channel.adpcm->loop_pred_scale = state.pred_scale;
                    channel.adpcm->loop_yn1 = state.yn1;
                    channel.adpcm->loop_yn2 = state.yn2;
                }
            }

            return stream.Write();
        }

        // The stream again, from its samples up to the new length.
        wave::Pcm decoded = sar::DecodeStream(stream);
        for (std::vector<int16_t>& channel : decoded.channels)
        {
            channel.resize(values.length);
        }

        const std::optional<std::vector<sar::StreamFile::Track>> tracks = stream.tracks;
        stream = sar::MakeStream(stream.flavor, stream.version, values.encoding, values.rate, decoded.channels,
                                 values.loop ? std::optional<uint32_t>(values.loop_start) : std::nullopt);
        stream.tracks = tracks;

        return stream.Write();
    }
    catch (const std::invalid_argument& error)
    {
        throw std::invalid_argument(std::string("Can't apply these values to the stream. ") +
                                    format::AsSentence(error.what()));
    }
    catch (const std::exception& error)
    {
        throw std::invalid_argument(std::string("Can't decode the stream. ") + format::AsSentence(error.what()));
    }
}

} // namespace hokuto::edit
