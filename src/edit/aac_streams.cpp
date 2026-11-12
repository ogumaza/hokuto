// SPDX-License-Identifier: MIT

// The AAC streams of the 3DS (edit/streams.h): the changes of a new AAC stream, the incoming file, and the stream
// that replaces the current one.

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

#include "ntdlib/sar/formats.h"
#include "ntdlib/sar/stream_file.h"
#include "seqlib/read_error.h"
#include "seqlib/wav.h"

#include "audio/resampler.h"
#include "edit/streams.h"
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

} // namespace

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

namespace
{

// The samples of `bytes`, a WAV file, with its loop (ReadIncomingAac). Without a loop, the loop's end is the end of the
// samples. Throws std::invalid_argument with a message for the user for a file that isn't a WAV file.
sar::Pcm WavPcm(std::span<const uint8_t> bytes)
{
    sar::Pcm pcm;
    seqlib::WavContents wav;
    try
    {
        wav = seqlib::ReadWav(bytes);
    }
    catch (const seqlib::ReadError& error)
    {
        throw std::invalid_argument("Can't read this file as a WAV file: " + error.Reason() +
                                    ". An AAC stream can be replaced with an AAC, WAV or stream file.");
    }

    pcm.channels = std::move(wav.pcm.channels);
    pcm.loop = wav.pcm.loop;
    pcm.loop_start = wav.pcm.loop_start;
    pcm.loop_end = wav.pcm.loop_end;
    pcm.sample_rate = wav.rate;
    if (!pcm.loop)
    {
        pcm.loop_end = static_cast<uint32_t>(pcm.channels.at(0).size());
    }

    return pcm;
}

// `pcm`, up to sample `end`, with `channels` channels: a mono file's channel twice, or the mean of a stereo file's
// (ReadIncomingAac). Throws std::invalid_argument with a message for the user for other channels. `present` says
// whether the channels are those of the current stream or of the sound's entry.
void MatchChannels(sar::Pcm& pcm, std::size_t channels, std::size_t end, bool present)
{
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
        throw std::invalid_argument("It has " + std::to_string(pcm.channels.size()) + " channels. " +
                                    (present ? "The current stream has " : "The sound entry specifies ") +
                                    std::to_string(channels) + ".");
    }
}

// MatchRate resamples `pcm`, whose samples run up to `end`, to `rate`, and returns the loop's start.
std::optional<std::size_t> MatchRate(sar::Pcm& pcm, uint32_t rate, std::size_t end)
{
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

    return loop_start;
}

// EncodeAac writes the samples of `pcm` into `incoming` at `bit_rate`, with the loop from `loop_start`.
void EncodeAac(sar::Pcm& pcm, uint32_t bit_rate, std::optional<std::size_t> loop_start, IncomingAac& incoming)
{
    // The system's encoder, at the present rate of bits. The loop follows the encoder's delay.
    pcm.loop = false;
    pcm.loop_end = static_cast<uint32_t>(pcm.channels[0].size());
    try
    {
        incoming.bytes = song::EncodeAdts(pcm, bit_rate);
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
}

// Whether `bytes`, an ADTS file that decodes to `pcm` at the channels and rate of `incoming`, can stay as it is: at the
// channels that `given` asks for, or else at 1 or 2, and at the rate of the current stream, or else at 44100 or 48000
// Hz. Then `incoming` gets the bytes of the file, its length, and the loop of `changes` where the loop ends in the
// file.
bool TakeAsIs(std::span<const uint8_t> bytes, const AacChanges& changes, bool given, const sar::Pcm& pcm,
              IncomingAac& incoming)
{
    const bool channels = given ? incoming.channels == changes.channels : incoming.channels <= 2;
    const bool rate =
        changes.present ? incoming.rate == changes.rate : incoming.rate == 44100 || incoming.rate == 48000;
    const bool fits = channels && rate;
    if (!fits || song::AdtsFrames(bytes) == 0)
    {
        return false;
    }

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

    return true;
}

} // namespace

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
            if (TakeAsIs(bytes, changes, given, pcm, incoming))
            {
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
            pcm = WavPcm(bytes);
            incoming.channels = pcm.channels.size();
            incoming.rate = pcm.sample_rate;
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
        throw std::invalid_argument("It has no samples.");
    }
    if (!given && pcm.channels.size() > 2)
    {
        throw std::invalid_argument("It has " + std::to_string(pcm.channels.size()) +
                                    " channels. An AAC stream can have only one or two.");
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

    MatchChannels(pcm, channels, end, changes.present);
    const std::optional<std::size_t> loop_start = MatchRate(pcm, rate, end);
    EncodeAac(pcm, changes.bit_rate, loop_start, incoming);

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
        throw std::invalid_argument(
            std::string("The new loop requires the model form, but the file can't be converted to it. ") +
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
        throw std::invalid_argument(std::string("Can't lay out the archive with the new stream. ") +
                                    format::AsSentence(error.what()));
    }
}

} // namespace hokuto::edit
