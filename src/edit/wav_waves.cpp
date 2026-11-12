// SPDX-License-Identifier: MIT

// The waves that come in for Replace… and File > Import (edit/files.h): WAV files, encoded for the 3DS, the Wii U, the
// Switch, the Wii, the DS and the GBA, the DS's streams from WAV files, and the consoles' wave files.

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

#include "ntdlib/rsar/convert.h"
#include "ntdlib/rsar/formats.h"
#include "ntdlib/rsar/wave_file.h"
#include "ntdlib/sar/byte_order.h"
#include "ntdlib/sar/encodings.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sar/wave_file.h"
#include "ntdlib/sdat/decode.h"
#include "ntdlib/sdat/files.h"
#include "ntdlib/sdat/formats.h"
#include "ntdlib/wave/adpcm.h"
#include "ntdlib/wave/encode.h"
#include "ntdlib/wave/pcm.h"
#include "seqlib/wav.h"
#include "xsflib/binary.h"
#include "xsflib/bytes.h"

#include "audio/resampler.h"
#include "edit/ds_streams.h"
#include "edit/ds_waves.h"
#include "edit/files.h"
#include "edit/files_internal.h"
#include "edit/gba_voices.h"
#include "format/kinds.h"
#include "song/gba_samples.h"

namespace hokuto::edit
{

namespace
{

namespace sar = ntdlib::sar;
namespace wave = ntdlib::wave;
namespace rsar = ntdlib::rsar;

// The most bytes of samples that a wave file takes. Its references hold signed 32-bit offsets.
constexpr std::size_t kMostWaveBytes = 0x7FFFFFFF;

// The samples of a frame of DSP-ADPCM.
constexpr std::size_t kFrameSamples = 14;

// Puts `channels`, each `incoming.frames` long, into `file` as 16-bit PCM, big-endian with `big_endian`: a wave file of
// the 3DS, the Wii U or the Switch (ntdlib::sar::WaveFile), or of the Wii (ntdlib::rsar::WaveFile). Throws
// std::invalid_argument with a message for the user for samples that a wave file can't hold.
template <class WaveFile>
void AddPcm16(const std::vector<std::vector<int16_t>>& channels, const IncomingWave& incoming, WaveFile& file,
              bool big_endian)
{
    if (incoming.frames * channels.size() * 2 > kMostWaveBytes)
    {
        throw std::invalid_argument(
            "As 16-bit PCM, the samples would take more than 2 GiB. That's too large for a wave file.");
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
        throw std::invalid_argument(
            "As DSP-ADPCM, the samples would take more than 2 GiB. That's too large for a wave file.");
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
        throw std::invalid_argument("Hokuto can encode waves only as DSP-ADPCM or 16-bit PCM.");
    }

    // The WAV file's samples as 16 bits. A wave holds one or two channels.
    seqlib::WavContents wav = ReadWavFile(bytes);

    const std::size_t channels = wav.pcm.channels.size();
    if (channels > 2)
    {
        throw std::invalid_argument("It has " + std::to_string(channels) +
                                    " channels. A wave can have only one or two.");
    }

    const std::size_t total = wav.pcm.channels[0].size();
    if (total == 0)
    {
        throw std::invalid_argument("It has no samples.");
    }

    // A wave plays up to the end of its loop. A WAV file without a loop takes the loop that the caller gives.
    if (!wav.pcm.loop && loop)
    {
        if (loop->start >= loop->end || loop->end > total)
        {
            throw std::invalid_argument("The loop from sample " + std::to_string(loop->start) + " to " +
                                        std::to_string(loop->end) + " isn't valid for the " + std::to_string(total) +
                                        " samples of the WAV file.");
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
    TakeWavDetails(wav, incoming);
    for (std::vector<int16_t>& channel : wav.pcm.channels)
    {
        channel.resize(incoming.frames);
    }

    // A lower rate resamples the samples. The loop keeps its place.
    if (rate)
    {
        if (*rate == 0 || *rate >= wav.rate)
        {
            throw std::invalid_argument("The new sample rate must be lower than the WAV file's " +
                                        std::to_string(wav.rate) + " Hz.");
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

// The samples of `bytes`, a WAV file, for a DS wave or stream (`kind`), as WavSamples reads them in PCM16. A file above
// `highest` Hz is read again at 48000 Hz. Throws std::invalid_argument with a message for the user for a rate below the
// DS's least.
std::vector<std::vector<int16_t>> DsWavSamples(std::span<const uint8_t> bytes, std::optional<WaveLoop> loop,
                                               std::optional<uint32_t> rate, uint32_t highest, const std::string& kind,
                                               IncomingWave& incoming)
{
    std::vector<std::vector<int16_t>> channels = WavSamples(bytes, sar::WaveEncoding::kPcm16, loop, rate, incoming);
    if (incoming.rate > highest)
    {
        channels = WavSamples(bytes, sar::WaveEncoding::kPcm16, loop, 48000, incoming);
    }

    if (incoming.rate < kLeastDsRate)
    {
        throw std::invalid_argument("A DS " + kind + " must have a sample rate from " + std::to_string(kLeastDsRate) +
                                    " to " + std::to_string(highest) + " Hz. This one is " +
                                    std::to_string(incoming.rate) + " Hz.");
    }

    return channels;
}

} // namespace

IncomingWave ReadIncomingWave(std::span<const uint8_t> bytes, const sar::SoundArchive& archive, uint32_t wave_archive,
                              std::optional<uint32_t> wave, sar::WaveEncoding encoding, std::optional<WaveLoop> loop,
                              std::optional<uint32_t> rate)
{
    if (encoding != sar::WaveEncoding::kDspAdpcm && encoding != sar::WaveEncoding::kPcm16)
    {
        throw std::invalid_argument("Hokuto can encode waves only as DSP-ADPCM or 16-bit PCM.");
    }

    // The present wave gives the version. A new wave takes the version of the archive's waves.
    const uint32_t version = wave ? PresentWaveVersion(archive, wave_archive, *wave)
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

IncomingWave ReadIncomingDsWave(std::span<const uint8_t> bytes, sar::WaveEncoding encoding,
                                std::optional<WaveLoop> loop, std::optional<uint32_t> rate)
{
    namespace sdat = ntdlib::sdat;
    const std::optional<sdat::WaveEncoding> held = sdat::EncodingOf(encoding);
    if (!held)
    {
        throw std::invalid_argument("A DS wave must be 8-bit PCM, 16-bit PCM or IMA-ADPCM.");
    }

    // The WAV file's samples, mixed to the one channel that a DS wave has.
    IncomingWave incoming;
    const std::vector<std::vector<int16_t>> channels =
        DsWavSamples(bytes, loop, rate, kHighestDsRate, "wave", incoming);
    incoming.encoding = encoding;
    std::vector<int16_t> samples(channels[0].size());
    for (std::size_t i = 0; i < samples.size(); i++)
    {
        const int sum = channels.size() == 2 ? channels[0][i] + channels[1][i] : 2 * channels[0][i];
        samples[i] = static_cast<int16_t>(std::lround(sum / 2.0));
    }

    try
    {
        incoming.bytes = EncodeDsWave(samples, incoming.rate, *held,
                                      incoming.loop ? std::optional<uint32_t>(incoming.loop_start) : std::nullopt);
    }
    catch (const std::invalid_argument& error)
    {
        throw std::invalid_argument(std::string("Can't encode it as a DS wave. ") + format::AsSentence(error.what()));
    }

    // The new wave: where its loop starts, and how far PCM8 or IMA-ADPCM is from the samples.
    const sdat::Wave made = sdat::Wave::Parse(xsflib::Reader(incoming.bytes), 0, incoming.bytes.size());
    if (incoming.loop)
    {
        incoming.loop_moved = made.LoopStartSample() - incoming.loop_start;
    }

    if (*held != sdat::WaveEncoding::kPcm16)
    {
        NoiseSums sums;
        AddNoise(samples, sdat::DecodeWave(made), sums);
        incoming.signal_to_noise = SignalToNoise(sums);
    }

    return incoming;
}

IncomingWave ReadIncomingDsWaveFile(std::span<const uint8_t> bytes)
{
    namespace sdat = ntdlib::sdat;
    sdat::Wave wave;
    try
    {
        wave = sdat::Wave::ParseFile(bytes);
    }
    catch (const xsflib::FormatError& error)
    {
        throw std::invalid_argument(std::string("Can't read it as a DS wave file: ") + error.what() + ".");
    }

    IncomingWave incoming;
    incoming.bytes = sdat::WaveBytes(wave);
    incoming.encoding = sar::EncodingOf(wave.encoding);
    incoming.channels = 1;
    incoming.rate = song::DsWaveRate(wave);
    incoming.frames = wave.Samples();
    incoming.loop = wave.loop != 0;
    incoming.loop_start = incoming.loop ? wave.LoopStartSample() : 0;

    return incoming;
}

IncomingWave ReadIncomingGbaSample(std::span<const uint8_t> bytes, std::optional<WaveLoop> loop,
                                   std::optional<uint32_t> rate)
{
    IncomingWave incoming;
    if (song::IsGbaSampleFile(bytes))
    {
        incoming.bytes = song::GbaSampleOfFile(bytes);
        const std::span<const uint8_t> header(incoming.bytes);
        const auto word = [header](std::size_t at)
        {
            return xsflib::LoadLe32(&header[at]);
        };
        incoming.encoding = sar::WaveEncoding::kPcm8;
        incoming.channels = 1;
        incoming.rate = word(4) / 1024;
        incoming.frames = word(12);
        incoming.loop = header[3] != 0;
        incoming.loop_start = incoming.loop ? word(8) : 0;
        return incoming;
    }

    const std::vector<std::vector<int16_t>> channels =
        WavSamples(bytes, sar::WaveEncoding::kPcm16, loop, rate, incoming);
    incoming.encoding = sar::WaveEncoding::kPcm8;
    incoming.channels = 1;

    std::vector<int16_t> samples(channels[0].size());
    NoiseSums sums;
    for (std::size_t i = 0; i < samples.size(); i++)
    {
        const int sum = channels.size() == 2 ? channels[0][i] + channels[1][i] : 2 * channels[0][i];
        samples[i] = static_cast<int16_t>(std::lround(sum / 2.0));
        const int narrow = static_cast<int8_t>(wave::EncodePcm8(samples[i]));
        const double error = static_cast<double>(samples[i]) - static_cast<double>(narrow * 256);
        sums.signal += static_cast<double>(samples[i]) * static_cast<double>(samples[i]);
        sums.noise += error * error;
    }
    incoming.signal_to_noise = SignalToNoise(sums);

    try
    {
        incoming.bytes = EncodeGbaSample(samples, incoming.rate,
                                         incoming.loop ? std::optional<uint32_t>(incoming.loop_start) : std::nullopt);
    }
    catch (const std::invalid_argument& error)
    {
        throw std::invalid_argument(std::string("Can't encode it as a GBA sample. ") +
                                    format::AsSentence(error.what()));
    }

    return incoming;
}

IncomingWave ReadIncomingDsStream(std::span<const uint8_t> bytes, sar::WaveEncoding encoding,
                                  std::optional<WaveLoop> loop, std::optional<uint32_t> rate)
{
    namespace sdat = ntdlib::sdat;
    const std::optional<sdat::WaveEncoding> held = sdat::EncodingOf(encoding);
    if (!held)
    {
        throw std::invalid_argument("A DS stream must be 8-bit PCM, 16-bit PCM or IMA-ADPCM.");
    }

    IncomingWave incoming;
    const std::vector<std::vector<int16_t>> channels =
        DsWavSamples(bytes, loop, rate, kHighestDsStreamRate, "stream", incoming);
    incoming.encoding = encoding;

    try
    {
        incoming.bytes = EncodeDsStream(channels, incoming.rate, *held,
                                        incoming.loop ? std::optional<uint32_t>(incoming.loop_start) : std::nullopt);
    }
    catch (const std::invalid_argument& error)
    {
        throw std::invalid_argument(std::string("Can't encode it as a DS stream. ") + format::AsSentence(error.what()));
    }

    // The new stream: where its loop starts, and how far PCM8 or IMA-ADPCM is from the samples.
    const sdat::Stream made = sdat::Stream::Parse(incoming.bytes);
    if (incoming.loop)
    {
        incoming.loop_moved = made.loop_start - incoming.loop_start;
    }

    if (*held != sdat::WaveEncoding::kPcm16)
    {
        NoiseSums sums;
        for (std::size_t channel = 0; channel < channels.size(); channel++)
        {
            AddNoise(channels[channel], sdat::DecodeStream(made, static_cast<uint32_t>(channel)), sums);
        }

        incoming.signal_to_noise = SignalToNoise(sums);
    }

    return incoming;
}

IncomingWave ReadIncomingDsStreamFile(std::span<const uint8_t> bytes)
{
    namespace sdat = ntdlib::sdat;
    sdat::Stream stream;
    try
    {
        stream = sdat::Stream::Parse(bytes);
    }
    catch (const xsflib::FormatError& error)
    {
        throw std::invalid_argument(std::string("Can't read it as a DS stream file: ") + error.what() + ".");
    }

    IncomingWave incoming;
    incoming.bytes.assign(bytes.begin(), bytes.end());
    incoming.encoding = sar::EncodingOf(stream.encoding);
    incoming.channels = stream.channels;
    incoming.rate = stream.sample_rate;
    incoming.frames = stream.samples;
    incoming.loop = stream.loop != 0;
    incoming.loop_start = incoming.loop ? stream.loop_start : 0;

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
        throw std::invalid_argument(std::string("Can't encode it as a Wii wave. ") + format::AsSentence(error.what()));
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
        const bool big_endian = bytes.size() >= kHeaderSize && MarksBigEndian(bytes);
        const bool little_endian = bytes.size() >= kHeaderSize && MarksLittleEndian(bytes);
        if (!big_endian && !little_endian)
        {
            throw std::invalid_argument(format::NotKindText(bytes, "a wave file"));
        }

        const xsflib::Reader r(bytes, big_endian);
        const uint32_t size = r.U32(kSizeField);
        if (size < kHeaderSize || size > bytes.size())
        {
            throw std::invalid_argument("Its header gives a size of " + std::to_string(size) +
                                        " bytes, but the file has " + std::to_string(bytes.size()) + " bytes.");
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
            throw std::invalid_argument(std::string("Can't convert the wave for the Wii. ") +
                                        format::AsSentence(error.what()));
        }

        incoming.version = read.version;
        incoming.converted = ConvertedFile{flavor, r.U32(kVersionField), size, std::nullopt};
    }
    else
    {
        if (!IsWiiWaveFile(bytes) || bytes.size() < kWiiHeaderSize || !MarksBigEndian(bytes))
        {
            throw std::invalid_argument(format::NotKindText(bytes, "a wave file"));
        }

        const xsflib::Reader r(bytes, true);
        const uint32_t size = r.U32(kWiiSizeField);
        if (size < kWiiHeaderSize || size > bytes.size())
        {
            throw std::invalid_argument("Its header gives a size of " + std::to_string(size) +
                                        " bytes, but the file has " + std::to_string(bytes.size()) + " bytes.");
        }

        incoming.bytes.assign(bytes.begin(), bytes.begin() + size);
        incoming.version = r.U16(kWiiVersionField);
        try
        {
            read = rsar::WaveFile::Read(incoming.bytes);
        }
        catch (const std::exception& error)
        {
            throw std::invalid_argument(std::string("Can't read the wave. ") + format::AsSentence(error.what()));
        }
    }

    incoming.encoding = sar::EncodingOf(read.encoding);
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
        wave ? std::optional<uint32_t>(PresentWaveVersion(archive, wave_archive, *wave)) : std::nullopt;

    // The magic gives the console's letter. The byte order mark tells the Wii U from the Switch.
    const sar::Flavor flavor = archive.ReadOptions().flavor;
    const bool big_endian = bytes.size() >= kHeaderSize && MarksBigEndian(bytes);
    const bool little_endian = bytes.size() >= kHeaderSize && MarksLittleEndian(bytes);
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
        throw std::invalid_argument("Its header gives a size of " + std::to_string(size) + " bytes, but the file has " +
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
            throw std::invalid_argument("Can't convert it for the " + song::ConsoleName(flavor) + ". " +
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
        throw std::invalid_argument(std::string("Can't read the wave. ") + format::AsSentence(error.what()));
    }

    incoming.encoding = read.encoding;
    incoming.channels = read.channels.size();
    incoming.rate = read.sample_rate;
    incoming.frames = read.loop_end;
    incoming.loop = read.loop;
    incoming.loop_start = read.loop ? read.loop_start : 0;

    return incoming;
}

} // namespace hokuto::edit
