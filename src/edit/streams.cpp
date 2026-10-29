// SPDX-License-Identifier: MIT

#include "edit/streams.h"

#include <algorithm>
#include <bit>
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

#include "ntdlib/rsar/convert.h"
#include "ntdlib/rsar/formats.h"
#include "ntdlib/rsar/stream_file.h"
#include "ntdlib/sar/byte_order.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sar/stream_file.h"
#include "ntdlib/wave/adpcm.h"
#include "ntdlib/wave/decode.h"
#include "ntdlib/wave/encode.h"
#include "seqlib/read_error.h"
#include "seqlib/wav.h"
#include "xsflib/binary.h"
#include "xsflib/bytes.h"

#include "audio/resampler.h"
#include "format/kinds.h"
#include "format/model.h"
#include "song/aac.h"
#include "song/items.h"
#include "song/streams.h"

namespace hokuto::edit
{
namespace
{

namespace sar = ntdlib::sar;
namespace wave = ntdlib::wave;

// A NintendoWare file's header: the magic, the byte order mark, the header's size, the version at 0x08, the file's size
// at 0x0C and the number of blocks at 0x10, then the blocks' table.
constexpr std::size_t kVersionField = 0x08;
constexpr std::size_t kSizeField = 0x0C;
constexpr std::size_t kHeaderSize = 0x14;

// The versions of the streams that Hokuto writes: 3.0.0 and 4.0.0. Most of the Wii U's games read 3.0.0.
constexpr uint32_t kOldStreamVersion = 0x00030000;
constexpr uint32_t kNewStreamVersion = 0x00040000;

// The version of a 3DS archive from which its table of sounds gives a stream sound's tracks.
constexpr uint32_t kCtrTracksVersion = 0x02030000;

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

// A stream file of the 3DS, the Wii U or the Switch: its console, the version in its header, its bytes up to the size
// in its header, and the stream.
struct LaterStream
{
    sar::Flavor console = sar::Flavor::kCafe;
    uint32_t version = 0;
    std::vector<uint8_t> bytes;
    sar::StreamFile stream;
};

// Reads `bytes` as a stream file of the 3DS, the Wii U or the Switch. Throws std::invalid_argument with a message for
// the user for bytes that aren't such a file, and a stream that ntdlib can't read.
LaterStream ReadLaterStream(std::span<const uint8_t> bytes)
{
    // The magic gives the console's letter. The byte order mark tells the Wii U from the Switch.
    const bool big_endian = bytes.size() >= kHeaderSize && bytes[4] == 0xFE && bytes[5] == 0xFF;
    const bool little_endian = bytes.size() >= kHeaderSize && bytes[4] == 0xFF && bytes[5] == 0xFE;
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
        throw std::invalid_argument("Its header gives a size of " + std::to_string(size) + " bytes. The file holds " +
                                    std::to_string(bytes.size()) + " bytes.");
    }
    read.bytes.assign(bytes.begin(), bytes.begin() + size);
    try
    {
        read.stream = sar::StreamFile::Read(read.bytes);
    }
    catch (const std::exception& error)
    {
        throw std::invalid_argument(std::string("Hokuto can't read the stream. ") + format::AsSentence(error.what()));
    }

    return read;
}

// A WAV file's samples for a stream, up to the end of its loop, with the fields of the stream that come from the WAV
// file.
struct WavStream
{
    IncomingStream incoming;
    uint32_t rate = 0;
    std::vector<std::vector<int16_t>> channels;
    std::optional<uint32_t> loop_start;
};

// Reads `bytes` as a WAV file for a stream in `encoding`. Throws std::invalid_argument with a message for the user for
// another encoding than DSP-ADPCM and 16-bit PCM, bytes that aren't a WAV file that Hokuto reads, a file of no samples,
// and one of more channels than a stream holds.
WavStream ReadWavStream(std::span<const uint8_t> bytes, sar::WaveEncoding encoding)
{
    if (encoding != sar::WaveEncoding::kDspAdpcm && encoding != sar::WaveEncoding::kPcm16)
    {
        throw std::invalid_argument("Hokuto makes a stream in DSP-ADPCM or in 16-bit PCM only.");
    }

    // The WAV file's samples as 16 bits.
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
    if (channels > kMostChannels)
    {
        throw std::invalid_argument("It holds " + std::to_string(channels) + " channels. A stream holds up to " +
                                    std::to_string(kMostChannels) + ".");
    }
    const std::size_t total = wav.pcm.channels[0].size();
    if (total == 0)
    {
        throw std::invalid_argument("It holds no samples.");
    }

    // A stream plays up to the end of its loop.
    WavStream stream;
    IncomingStream& incoming = stream.incoming;
    incoming.loop = wav.pcm.loop;
    incoming.frames = wav.pcm.loop ? wav.pcm.loop_end : total;
    incoming.after_loop = total - incoming.frames;
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
    stream.rate = wav.rate;
    stream.channels = std::move(wav.pcm.channels);
    stream.loop_start = wav.pcm.loop ? std::optional<uint32_t>(wav.pcm.loop_start) : std::nullopt;

    return stream;
}

// Gives `wav.incoming` the WAV file's samples, rate and loop again after Describe, with the samples by which the
// stream, whose loop starts at `loop_start`, moved the loop. With `decoded`, the stream's decoded channels, it also
// gives the ratio of signal to noise of DSP-ADPCM.
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

namespace rsar = ntdlib::rsar;

// The header of a stream file of the Wii: the version at 0x06 and the file's size at 0x08, in 64 bytes.
constexpr std::size_t kWiiVersionField = 0x06;
constexpr std::size_t kWiiSizeField = 0x08;
constexpr std::size_t kWiiHeaderSize = 0x40;

// The version of the stream files that Hokuto writes for the Wii: 1.0, the version of every stream file of the Wii.
constexpr uint32_t kWiiStreamVersion = 0x0100;

// The version of a Wii archive from which a stream sound's entry counts its channels. Before, it holds a bit for each.
constexpr uint16_t kWiiChannelCountVersion = 0x0104;

// Reads `bytes` as a stream file of the Wii. Throws std::invalid_argument with a message for the user that starts with
// `start` for a stream that ntdlib can't read.
rsar::StreamFile ReadWii(std::span<const uint8_t> bytes, const char* start = "Hokuto can't read the stream. ")
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

// Fills the fields of `incoming` that describe `stream`, a stream of the Wii of `version`.
void DescribeWii(const rsar::StreamFile& stream, uint32_t version, IncomingStream& incoming)
{
    incoming.version = version;
    incoming.encoding = static_cast<sar::WaveEncoding>(stream.encoding);
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

// Checks `values` for a stream of `samples` samples in each channel (StreamWithValues).
void CheckValues(const StreamValues& values, uint32_t samples)
{
    if (values.rate == 0)
    {
        throw std::invalid_argument("A stream plays at a rate above 0.");
    }
    if (values.length == 0 || values.length > samples)
    {
        throw std::invalid_argument("The stream holds " + std::to_string(samples) + " samples.");
    }
    if (values.loop && values.loop_start >= values.length)
    {
        throw std::invalid_argument("The loop starts at the stream's end or after it.");
    }
}

// `bytes`, a stream file of the Wii, with `values` (StreamWithValues).
std::vector<uint8_t> WiiStreamWithValues(std::span<const uint8_t> bytes, const StreamValues& values)
{
    rsar::StreamFile stream = ReadWii(bytes);
    CheckValues(values, stream.sample_count);

    // A rate, and a loop from a block to the stream's end, keep the encoded samples. Without a loop, a channel of
    // DSP-ADPCM holds the first frame's predictor and scale for the loop, and no history. That is how the tools write
    // such a channel.
    const auto encoding = static_cast<sar::WaveEncoding>(stream.encoding);
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
                stream.loop = values.loop;
                stream.loop_start = loop_start;
                for (rsar::StreamFile::Channel& channel : stream.channels)
                {
                    if (!channel.adpcm)
                    {
                        continue;
                    }

                    rsar::DspAdpcmInfo& info = *channel.adpcm;
                    const wave::DspAdpcmState state =
                        values.loop ? wave::DspAdpcmStateAt(channel.samples, loop_start, info.coefs, info.yn1, info.yn2,
                                                            wave::DspAdpcmSum::kWide)
                                    : wave::DspAdpcmState{static_cast<uint8_t>(info.pred_scale), 0, 0};
                    info.loop_pred_scale = state.pred_scale;
                    info.loop_yn1 = state.yn1;
                    info.loop_yn2 = state.yn2;
                }
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
        throw std::invalid_argument(std::string("The stream can't take the values. ") +
                                    format::AsSentence(error.what()));
    }
    catch (const std::exception& error)
    {
        throw std::invalid_argument(std::string("Hokuto can't decode the stream. ") + format::AsSentence(error.what()));
    }
}

} // namespace

bool IsStreamFile(std::span<const uint8_t> bytes)
{
    const std::string kMagic = "STM";
    return bytes.size() >= 4 && (bytes[0] == 'C' || bytes[0] == 'F') &&
           std::equal(kMagic.begin(), kMagic.end(), bytes.begin() + 1);
}

bool TracksInFiles(const sar::SoundArchive& archive)
{
    return archive.ReadOptions().flavor == sar::Flavor::kCtr && archive.Version() < kCtrTracksVersion;
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
            throw std::invalid_argument("Hokuto can't convert it for the " + song::ConsoleName(flavor) + ". " +
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
        throw std::invalid_argument(std::string("Hokuto can't make a stream of it. ") +
                                    format::AsSentence(error.what()));
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
        throw std::invalid_argument("Hokuto replaces only streams beside the archive, in its folder.");
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
    sar::StreamFile stream;
    try
    {
        stream = sar::StreamFile::Read(*found->second.bytes);
    }
    catch (const std::exception& error)
    {
        throw std::invalid_argument(std::string("Hokuto can't read the present stream. ") +
                                    format::AsSentence(error.what()));
    }
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

    sar::StreamFile stream;
    try
    {
        stream = sar::StreamFile::Read(incoming.bytes);
    }
    catch (const std::exception& error)
    {
        throw std::invalid_argument(std::string("Hokuto can't read the stream. ") + format::AsSentence(error.what()));
    }
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
        throw std::invalid_argument(std::string("Hokuto can't give the stream its tracks. ") +
                                    format::AsSentence(error.what()));
    }

    return incoming;
}

format::File ReplaceStream(const format::File& file, uint32_t file_id, std::vector<uint8_t> bytes)
{
    if (!file.native)
    {
        throw std::invalid_argument("The file holds no archive of a game.");
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

std::optional<AacChanges> ChangesOfAacStream(const format::File& file, const sar::SoundArchive& archive, uint32_t sound)
{
    const std::optional<song::AacStream> aac = song::AacStreamOf(archive, sound);
    const std::optional<uint32_t> id = song::StreamFileOf(archive, sound);
    if (!aac || !id || !file.native)
    {
        return std::nullopt;
    }
    const auto found = file.native->streams.find(*id);
    if (found != file.native->streams.end() && !song::IsAdts(*found->second.bytes))
    {
        return std::nullopt;
    }

    AacChanges changes;
    changes.file = *id;
    changes.loop = aac->loop;
    for (uint32_t other = 0; other < archive.Sounds().size(); other++)
    {
        if (other != sound && song::StreamFileOf(archive, other) == id)
        {
            changes.sounds.push_back(other);
        }
    }

    // A stream that the file lacks takes the channels that the sound's entry gives, and the rate of bits of Pokémon X's
    // AAC files.
    if (found == file.native->streams.end())
    {
        changes.present = false;
        changes.channels = archive.Sounds().at(sound).stream.channel_count;
        changes.bit_rate = 128000;
        return changes;
    }
    const std::vector<uint8_t>& bytes = *found->second.bytes;
    const sar::Pcm pcm = song::DecodeStreamFile(bytes, *aac);
    changes.channels = pcm.channels.size();
    changes.rate = pcm.sample_rate;
    changes.length = pcm.loop_end;
    changes.size = bytes.size();

    // The present rate of bits, from the file's size and its frames' samples.
    const double seconds = static_cast<double>(song::AdtsFrames(bytes)) * 1024.0 / pcm.sample_rate;
    const double bits = seconds > 0.0 ? static_cast<double>(bytes.size()) * 8.0 / seconds : 0.0;
    changes.bit_rate = *std::ranges::min_element(
        song::kAacBitRates, {}, [bits](uint32_t rate) { return std::abs(static_cast<double>(rate) - bits); });

    return changes;
}

IncomingAac ReadIncomingAac(std::span<const uint8_t> bytes, const AacChanges& changes)
{
    // The new stream takes the present stream's channels. In place of a stream that the file lacks, it takes those of
    // the sound's entry, 1 or 2. An entry of other channels leaves the file's.
    const bool entry = !changes.present && (changes.channels == 1 || changes.channels == 2);
    const bool given = changes.present || entry;

    // An AAC file of the present rate and channels goes in as it is.
    IncomingAac incoming;
    sar::Pcm pcm;
    try
    {
        if (song::IsAdts(bytes))
        {
            pcm = song::DecodeAdts(bytes);
            incoming.channels = pcm.channels.size();
            incoming.rate = pcm.sample_rate;
            const bool channels = given ? incoming.channels == changes.channels : incoming.channels <= 2;
            const bool rate =
                changes.present ? incoming.rate == changes.rate : incoming.rate == 44100 || incoming.rate == 48000;
            const bool fits = channels && rate;
            if (fits && song::AdtsFrames(bytes) > 0)
            {
                incoming.stream_channels = incoming.channels;
                incoming.stream_rate = incoming.rate;
                incoming.bytes.assign(bytes.begin(), bytes.end());
                incoming.length = pcm.loop_end;
                if (changes.loop && changes.loop->end <= pcm.loop_end)
                {
                    incoming.loop = changes.loop;
                    incoming.length = changes.loop->end;
                    incoming.present_loop = true;
                }
                return incoming;
            }
        }
        else if (IsStreamFile(bytes) || IsWiiStreamFile(bytes))
        {
            pcm = song::DecodeStreamFile(bytes);
            incoming.channels = pcm.channels.size();
            incoming.rate = pcm.sample_rate;
        }
        else
        {
            seqlib::WavContents wav;
            try
            {
                wav = seqlib::ReadWav(bytes);
            }
            catch (const seqlib::ReadError& error)
            {
                throw std::invalid_argument(
                    "Hokuto reads an AAC file, a WAV file or a stream file there. It can't "
                    "read this one as a WAV file: " +
                    error.Reason() + ".");
            }
            pcm.channels = std::move(wav.pcm.channels);
            pcm.loop = wav.pcm.loop;
            pcm.loop_start = wav.pcm.loop_start;
            pcm.loop_end = wav.pcm.loop_end;
            pcm.sample_rate = wav.rate;
            incoming.channels = pcm.channels.size();
            incoming.rate = wav.rate;
            if (!pcm.loop)
            {
                pcm.loop_end = static_cast<uint32_t>(pcm.channels.at(0).size());
            }
        }
    }
    catch (const std::runtime_error& error)
    {
        throw std::invalid_argument(error.what());
    }

    // The samples up to the loop's end, with the present stream's channels or the entry's. Without either, the new
    // stream keeps 1 or 2 channels. Without a present stream, it takes a rate that the encoder takes.
    if (pcm.channels.empty() || pcm.channels[0].empty())
    {
        throw std::invalid_argument("It holds no samples.");
    }
    if (!given && pcm.channels.size() > 2)
    {
        throw std::invalid_argument("It holds " + std::to_string(pcm.channels.size()) +
                                    " channels. An AAC stream holds 1 or 2.");
    }
    const std::size_t channels = given ? changes.channels : pcm.channels.size();
    const uint32_t rate = changes.present                                        ? changes.rate
                          : pcm.sample_rate == 44100 || pcm.sample_rate == 48000 ? pcm.sample_rate
                                                                                 : 48000;
    incoming.stream_channels = channels;
    incoming.stream_rate = rate;
    const std::size_t total = pcm.channels[0].size();
    const std::size_t end = pcm.loop ? std::min<std::size_t>(pcm.loop_end, total) : total;
    incoming.after_loop = total - end;
    for (std::vector<int16_t>& channel : pcm.channels)
    {
        channel.resize(end);
    }
    if (pcm.channels.size() == 1 && channels == 2)
    {
        pcm.channels.push_back(pcm.channels[0]);
    }
    else if (pcm.channels.size() == 2 && channels == 1)
    {
        for (std::size_t i = 0; i < end; i++)
        {
            pcm.channels[0][i] = static_cast<int16_t>((pcm.channels[0][i] + pcm.channels[1][i]) / 2);
        }
        pcm.channels.resize(1);
    }
    else if (pcm.channels.size() != channels)
    {
        throw std::invalid_argument("It holds " + std::to_string(pcm.channels.size()) + " channels. " +
                                    (changes.present ? "The present stream holds " : "The sound's entry gives ") +
                                    std::to_string(channels) + ".");
    }

    // The present stream's rate. A loop keeps its place in time.
    std::optional<std::size_t> loop_start;
    if (pcm.loop && pcm.loop_start < end)
    {
        loop_start = pcm.loop_start;
    }
    if (pcm.sample_rate != rate)
    {
        for (std::vector<int16_t>& channel : pcm.channels)
        {
            channel = audio::Resample(channel, pcm.sample_rate, rate, loop_start);
        }
        if (loop_start)
        {
            loop_start = audio::ResampledPlace(*loop_start, pcm.sample_rate, rate);
        }
        pcm.sample_rate = rate;
    }

    // The system's encoder, at the present rate of bits. The loop follows the encoder's delay.
    pcm.loop = false;
    pcm.loop_end = static_cast<uint32_t>(pcm.channels[0].size());
    try
    {
        incoming.bytes = song::EncodeAdts(pcm, changes.bit_rate);
        incoming.delay = song::EncoderDelay(pcm.sample_rate, pcm.channels.size());
    }
    catch (const std::runtime_error& error)
    {
        throw std::invalid_argument(error.what());
    }
    incoming.encoded = true;
    incoming.length = incoming.delay + pcm.loop_end;
    if (loop_start)
    {
        incoming.loop = song::AacLoop{static_cast<uint32_t>(incoming.delay + *loop_start), incoming.length};
    }

    return incoming;
}

format::File ReplaceAacStream(const format::File& file, const AacChanges& changes, const IncomingAac& incoming)
{
    // The stream type of an AAC file, and the bit that marks a loop.
    constexpr uint32_t kAac = 2;
    constexpr uint32_t kLooped = 0x100;

    format::File result;
    format::Model model;
    try
    {
        result = file.form == format::ArchiveForm::kModel ? file : format::InModelForm(file);
        model = format::ModelOf(result);
    }
    catch (const std::exception& error)
    {
        throw std::invalid_argument(std::string("The new loop requires the model form, and the file can't take it. ") +
                                    format::AsSentence(error.what()));
    }

    // Each sound that plays the stream as AAC takes the new loop.
    for (sar::SoundModel& sound : model.archive.sounds)
    {
        if (sound.type != sar::SoundType::kStream || sound.file_id != changes.file || !sound.stream_extension ||
            (sound.stream_extension->stream_type & 0xFF) != kAac)
        {
            continue;
        }

        sar::StreamExtension& extension = *sound.stream_extension;
        extension.stream_type = (extension.stream_type & ~kLooped) | (incoming.loop ? kLooped : 0);
        extension.loop_start = incoming.loop ? incoming.loop->start : 0;
        extension.loop_end = incoming.loop ? incoming.loop->end : 0;

        // In CSAR 2.3.0.0, the details hold the values as options. The tools leave out the loop of a stream without
        // one.
        if (sound.stream_options != 0)
        {
            sound.stream_options = incoming.loop ? 0x07 : 0x01;
        }
    }
    model.streams[changes.file] = incoming.bytes;

    try
    {
        return format::WithModel(std::move(result), model);
    }
    catch (const std::exception& error)
    {
        throw std::invalid_argument(std::string("Hokuto can't lay out the archive with the new stream. ") +
                                    format::AsSentence(error.what()));
    }
}

StreamValues StreamValuesOf(std::span<const uint8_t> bytes)
{
    if (IsWiiStreamFile(bytes))
    {
        const rsar::StreamFile stream = ReadWii(bytes);
        return {static_cast<sar::WaveEncoding>(stream.encoding), stream.sample_rate, stream.loop,
                stream.loop ? stream.loop_start : 0, stream.sample_count};
    }

    sar::StreamFile stream;
    try
    {
        stream = sar::StreamFile::Read(bytes);
    }
    catch (const std::exception& error)
    {
        throw std::invalid_argument(std::string("Hokuto can't read the stream. ") + format::AsSentence(error.what()));
    }

    return {stream.encoding, stream.sample_rate, stream.loop, stream.loop ? stream.loop_start : 0, stream.sample_count};
}

std::vector<uint8_t> StreamWithValues(std::span<const uint8_t> bytes, const StreamValues& values)
{
    if (IsWiiStreamFile(bytes))
    {
        return WiiStreamWithValues(bytes, values);
    }

    sar::StreamFile stream;
    try
    {
        stream = sar::StreamFile::Read(bytes);
    }
    catch (const std::exception& error)
    {
        throw std::invalid_argument(std::string("Hokuto can't read the stream. ") + format::AsSentence(error.what()));
    }
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
        throw std::invalid_argument(std::string("The stream can't take the values. ") +
                                    format::AsSentence(error.what()));
    }
    catch (const std::exception& error)
    {
        throw std::invalid_argument(std::string("Hokuto can't decode the stream. ") + format::AsSentence(error.what()));
    }
}

bool IsWiiStreamFile(std::span<const uint8_t> bytes)
{
    const std::string kMagic = "RSTM";
    return bytes.size() >= 6 && std::equal(kMagic.begin(), kMagic.end(), bytes.begin()) && bytes[4] == 0xFE &&
           bytes[5] == 0xFF;
}

IncomingStream ReadIncomingWiiStreamFile(std::span<const uint8_t> bytes,
                                         std::span<const rsar::StreamFile::Track> tracks)
{
    // A stream file of the Wii goes in as it is, with its tracks.
    IncomingStream incoming;
    if (IsWiiStreamFile(bytes))
    {
        const uint32_t size = bytes.size() >= kWiiHeaderSize ? xsflib::LoadBe32(&bytes[kWiiSizeField]) : 0;
        if (size < kWiiHeaderSize || size > bytes.size())
        {
            throw std::invalid_argument("Its header gives a size of " + std::to_string(size) +
                                        " bytes. The file holds " + std::to_string(bytes.size()) + " bytes.");
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
        throw std::invalid_argument(std::string("Hokuto can't convert it for the Wii. ") +
                                    format::AsSentence(error.what()));
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
        throw std::invalid_argument(std::string("Hokuto can't make a stream of it. ") +
                                    format::AsSentence(error.what()));
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
        throw std::invalid_argument("Hokuto replaces only streams beside the archive, in its folder.");
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
    const rsar::StreamFile stream = ReadWii(*found->second.bytes, "Hokuto can't read the present stream. ");
    changes.present = true;
    changes.encoding = static_cast<sar::WaveEncoding>(stream.encoding);
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
