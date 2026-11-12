// SPDX-License-Identifier: MIT

// The stream files of the Wii (edit/streams.h): their values, the incoming stream files and WAV files, and the
// changes of a new stream.

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/rsar/convert.h"
#include "ntdlib/rsar/formats.h"
#include "ntdlib/rsar/stream_file.h"
#include "ntdlib/sar/encodings.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/wave/adpcm.h"
#include "ntdlib/wave/decode.h"
#include "ntdlib/wave/encode.h"
#include "xsflib/bytes.h"

#include "edit/files_internal.h"
#include "edit/streams.h"
#include "edit/streams_parts.h"
#include "format/kinds.h"
#include "format/model.h"
#include "song/items.h"

namespace hokuto::edit
{
namespace
{

namespace sar = ntdlib::sar;
namespace wave = ntdlib::wave;
using streams_parts::CheckValues;
using streams_parts::FinishWavStream;
using streams_parts::LaterStream;
using streams_parts::ReadLaterStream;
using streams_parts::ReadWavStream;
using streams_parts::ReadWii;
using streams_parts::WavStream;

namespace rsar = ntdlib::rsar;

// The header of a stream file of the Wii is 64 bytes long. It has the fields of every file of the Wii
// (kWiiVersionField).
constexpr std::size_t kWiiStreamHeaderSize = 0x40;

// The version of the stream files that Hokuto writes for the Wii: 1.0, the version of every stream file of the Wii.
constexpr uint32_t kWiiStreamVersion = 0x0100;

// Fills the fields of `incoming` that describe `stream`, a stream of the Wii of `version`.
void DescribeWii(const rsar::StreamFile& stream, uint32_t version, IncomingStream& incoming)
{
    incoming.version = version;
    incoming.encoding = sar::EncodingOf(stream.encoding);
    incoming.channels = stream.channels.size();
    incoming.rate = stream.sample_rate;
    incoming.frames = stream.sample_count;
    incoming.loop = stream.loop;
    incoming.loop_start = stream.loop ? stream.loop_start : 0;
}

// Gives `stream`, a stream of the Wii, `tracks` where it holds each channel that they play.
void TakeTracks(rsar::StreamFile& stream, std::span<const rsar::StreamFile::Track> tracks)
{
    const auto held = [&stream](uint8_t channel)
    {
        return channel < stream.channels.size();
    };
    const auto holds = [&held](const rsar::StreamFile::Track& track)
    {
        return !track.channels.empty() && std::ranges::all_of(track.channels, held);
    };
    if (!tracks.empty() && std::ranges::all_of(tracks, holds))
    {
        stream.tracks.assign(tracks.begin(), tracks.end());
    }
}

// Sets the loop of `stream`, a stream file of the Wii, to `loop` and `loop_start`, with the loop's predictor, scale and
// history in each channel of DSP-ADPCM. Without a loop, a channel stores the predictor and the scale of the first
// frame.
void SetLoopStates(rsar::StreamFile& stream, bool loop, uint32_t loop_start)
{
    stream.loop = loop;
    stream.loop_start = loop_start;
    for (rsar::StreamFile::Channel& channel : stream.channels)
    {
        if (!channel.adpcm)
        {
            continue;
        }

        rsar::DspAdpcmInfo& info = *channel.adpcm;
        const wave::DspAdpcmState state = loop ? wave::DspAdpcmStateAt(channel.samples, loop_start, info.coefs,
                                                                       info.yn1, info.yn2, wave::DspAdpcmSum::kWide)
                                               : wave::DspAdpcmState{static_cast<uint8_t>(info.pred_scale), 0, 0};
        info.loop_pred_scale = state.pred_scale;
        info.loop_yn1 = state.yn1;
        info.loop_yn2 = state.yn2;
    }
}

} // namespace

namespace streams_parts
{

rsar::StreamFile ReadWii(std::span<const uint8_t> bytes, const char* start)
{
    try
    {
        return rsar::StreamFile::Read(bytes);
    }
    catch (const std::exception& error)
    {
        throw std::invalid_argument(start + format::AsSentence(error.what()));
    }
}

std::vector<uint8_t> WiiStreamWithValues(std::span<const uint8_t> bytes, const StreamValues& values)
{
    rsar::StreamFile stream = ReadWii(bytes);
    CheckValues(values, stream.sample_count);

    // A rate, and a loop from a block to the stream's end, keep the encoded samples. Without a loop, a channel of
    // DSP-ADPCM holds the first frame's predictor and scale for the loop, and no history. That is how the tools write
    // such a channel.
    const sar::WaveEncoding encoding = sar::EncodingOf(stream.encoding);
    const uint32_t loop_start = values.loop ? values.loop_start : 0;
    const bool same_loop = values.loop == stream.loop && loop_start == (stream.loop ? stream.loop_start : 0);
    const bool at_block = !values.loop || loop_start % stream.block_samples == 0;
    const bool keeps = values.encoding == encoding && values.length == stream.sample_count && at_block;
    try
    {
        if (keeps)
        {
            stream.sample_rate = values.rate;
            if (!same_loop)
            {
                SetLoopStates(stream, values.loop, loop_start);
            }

            return stream.Write();
        }

        // The stream again, from its samples up to the new length, with its tracks.
        wave::Pcm decoded = rsar::DecodeStream(stream);
        for (std::vector<int16_t>& channel : decoded.channels)
        {
            channel.resize(values.length);
        }

        rsar::StreamFile made =
            rsar::MakeStream(static_cast<rsar::WaveEncoding>(values.encoding), values.rate, decoded.channels,
                             values.loop ? std::optional<uint32_t>(values.loop_start) : std::nullopt);
        made.extended_tracks = stream.extended_tracks;
        made.tracks = std::move(stream.tracks);

        return made.Write();
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

} // namespace streams_parts

bool IsWiiStreamFile(std::span<const uint8_t> bytes)
{
    const std::string kMagic = "RSTM";
    return bytes.size() >= 6 && std::equal(kMagic.begin(), kMagic.end(), bytes.begin()) && MarksBigEndian(bytes);
}

IncomingStream ReadIncomingWiiStreamFile(std::span<const uint8_t> bytes,
                                         std::span<const rsar::StreamFile::Track> tracks)
{
    // A stream file of the Wii goes in as it is, with its tracks.
    IncomingStream incoming;
    if (IsWiiStreamFile(bytes))
    {
        const uint32_t size = bytes.size() >= kWiiStreamHeaderSize ? xsflib::LoadBe32(&bytes[kWiiSizeField]) : 0;
        if (size < kWiiStreamHeaderSize || size > bytes.size())
        {
            throw std::invalid_argument("Its header gives a size of " + std::to_string(size) +
                                        " bytes, but the file has " + std::to_string(bytes.size()) + " bytes.");
        }

        incoming.bytes.assign(bytes.begin(), bytes.begin() + size);
        DescribeWii(ReadWii(incoming.bytes), xsflib::LoadBe16(&bytes[kWiiVersionField]), incoming);

        return incoming;
    }

    // One of another console goes in converted. Without tracks, it takes those given where they fit.
    LaterStream read = ReadLaterStream(bytes);
    incoming.converted = ConvertedFile{read.console, read.version, read.bytes.size(), std::nullopt};
    rsar::StreamFile stream;
    try
    {
        stream = rsar::WiiStreamOf(read.stream);
        if (!read.stream.tracks || read.stream.tracks->empty())
        {
            TakeTracks(stream, tracks);
        }

        incoming.bytes = stream.Write();
    }
    catch (const std::exception& error)
    {
        throw std::invalid_argument(std::string("Can't convert it for the Wii. ") + format::AsSentence(error.what()));
    }

    DescribeWii(stream, kWiiStreamVersion, incoming);

    return incoming;
}

IncomingStream ReadIncomingWiiStreamWav(std::span<const uint8_t> bytes, sar::WaveEncoding encoding,
                                        std::span<const rsar::StreamFile::Track> tracks)
{
    // The stream, laid out as the tools lay it out. Its loop starts at a block. It takes the tracks given where they
    // fit.
    WavStream wav = ReadWavStream(bytes, encoding);
    rsar::StreamFile stream;
    try
    {
        stream = rsar::MakeStream(static_cast<rsar::WaveEncoding>(encoding), wav.rate, wav.channels, wav.loop_start);
        TakeTracks(stream, tracks);
        wav.incoming.bytes = stream.Write();
    }
    catch (const std::invalid_argument& error)
    {
        throw std::invalid_argument(std::string("Can't make a stream from it. ") + format::AsSentence(error.what()));
    }

    DescribeWii(stream, kWiiStreamVersion, wav.incoming);
    if (encoding == sar::WaveEncoding::kDspAdpcm)
    {
        const wave::Pcm decoded = rsar::DecodeStream(stream);
        FinishWavStream(wav, stream.loop_start, &decoded.channels);
    }
    else
    {
        FinishWavStream(wav, stream.loop_start, nullptr);
    }

    return std::move(wav.incoming);
}

StreamChanges ChangesOfWiiStream(const format::File& file, const rsar::SoundArchive& archive, uint32_t sound)
{
    if (sound >= archive.Sounds().size() || archive.Sounds()[sound].type != rsar::SoundType::kStream)
    {
        throw std::invalid_argument("It isn't a stream sound.");
    }

    const std::optional<uint32_t> id = song::StreamFileOf(archive, sound);
    if (!id)
    {
        throw std::invalid_argument("Only streams in separate files beside the archive can be replaced.");
    }

    // The other sounds that play the file, and the channels that the sound's entry names. The stream file gives the
    // tracks.
    StreamChanges changes;
    changes.file = *id;
    for (uint32_t other = 0; other < archive.Sounds().size(); other++)
    {
        if (other != sound && song::StreamFileOf(archive, other) == id)
        {
            changes.sounds.push_back(other);
        }
    }

    const uint16_t channels = archive.Sounds()[sound].allocate_channels;
    changes.table_channels = archive.Version() >= kWiiChannelCountVersion
                                 ? channels
                                 : static_cast<std::size_t>(std::popcount(static_cast<unsigned>(channels)));
    changes.tracks_in_file = true;

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

    const rsar::StreamFile stream = ReadWii(*found->second.bytes, "Can't read the current stream. ");
    changes.present = true;
    changes.encoding = sar::EncodingOf(stream.encoding);
    changes.channels = stream.channels.size();
    changes.rate = stream.sample_rate;
    changes.loop = stream.loop;
    changes.loop_start = stream.loop ? stream.loop_start : 0;
    changes.length = stream.sample_count;
    changes.size = found->second.bytes->size();
    changes.from_game = found->second.from_game;
    changes.wii_tracks = stream.tracks;
    for (const rsar::StreamFile::Track& track : stream.tracks)
    {
        changes.tracks.push_back(track.channels);
    }

    return changes;
}

} // namespace hokuto::edit
