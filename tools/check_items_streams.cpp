// SPDX-License-Identifier: MIT

// The checks of hokuto_check_items on the streams beside an archive.

#include "check_items.h"

namespace hokuto::tools::items
{

std::optional<std::vector<uint8_t>> StreamBeside(const sar::SoundArchive& archive, uint32_t file,
                                                 const std::filesystem::path& path)
{
    std::string text = archive.ExternalPath(file);
    std::replace(text.begin(), text.end(), '\\', '/');
    if (text.empty() || text.starts_with('/') || text.find("..") != std::string::npos)
    {
        return std::nullopt;
    }
    std::vector<uint8_t> bytes;
    if (!xsflib::ReadFile(path.parent_path() / xsflib::PathFromUtf8(text), bytes))
    {
        return std::nullopt;
    }

    return bytes;
}

namespace
{

// How far the system's decoder of AAC can move a sample between two decodings: Audio Toolbox's samples moved by a step.
constexpr int kAacSteps = 1;

} // namespace

std::string AacStreamProblem(const std::vector<uint8_t>& ours, const sar::ArchiveModel& model, uint32_t sound,
                             Totals& totals)
{
    // The entry's loop. The stream ends at the loop's end.
    const std::optional<sar::StreamExtension>& extension = model.sounds.at(sound).stream_extension;
    if (!extension || (extension->stream_type & 0xFF) != 2)
    {
        return "'s entry doesn't mark an AAC stream";
    }
    sar::Pcm decoded = song::DecodeAdts(ours);
    song::AacStream aac;
    if ((extension->stream_type & 0x100) != 0 && extension->loop_start < extension->loop_end &&
        extension->loop_start < decoded.loop_end)
    {
        aac.loop = song::AacLoop{extension->loop_start, extension->loop_end};
        decoded.loop = true;
        decoded.loop_start = extension->loop_start;
        decoded.loop_end = std::min(extension->loop_end, decoded.loop_end);
        for (std::vector<int16_t>& channel : decoded.channels)
        {
            channel.resize(decoded.loop_end);
        }
    }

    const std::optional<WavRead> wav = ReadWav(song::StreamAsWav(ours, aac));
    if (!wav || !Holds(*wav, decoded, kAacSteps))
    {
        return "'s WAV file differs";
    }
    for (const sar::Flavor other_console : song::OtherConsoles(sar::Flavor::kCtr))
    {
        const std::string console = song::ConsoleName(other_console);
        const uint32_t version = song::ExportVersions(song::ItemKind::kStream, other_console).file;
        const sar::StreamFile converted =
            sar::StreamFile::Read(song::ConvertStreamFile(ours, other_console, version, aac));
        const sar::Pcm samples = sar::DecodeStream(converted);

        // The loop moves forward to the first block at or after its start, and the samples up to there follow its end
        // (ntdlib's MakeStream).
        const uint32_t moved = samples.loop_start - std::min(samples.loop_start, decoded.loop_start);
        const bool same_loop = samples.loop == decoded.loop && samples.loop_start >= decoded.loop_start &&
                               moved < sar::StreamBlockSamples(sar::WaveEncoding::kDspAdpcm) &&
                               samples.loop_end == decoded.loop_end + moved;

        double error = 0.0;
        double signal = 0.0;
        for (std::size_t channel = 0; channel < decoded.channels.size() && channel < samples.channels.size(); channel++)
        {
            for (uint32_t i = 0; i < decoded.loop_end && i < samples.channels[channel].size(); i++)
            {
                const double expected = decoded.channels[channel][i];
                const double difference = samples.channels[channel][i] - expected;
                error += difference * difference;
                signal += expected * expected;
            }
        }
        if (converted.flavor != other_console || converted.version != version ||
            converted.encoding != sar::WaveEncoding::kDspAdpcm || converted.sample_rate != decoded.sample_rate ||
            !same_loop || samples.channels.size() != decoded.channels.size() ||
            (signal > 0.0 && 10.0 * std::log10(error / signal) > -15.0))
        {
            return "'s file for the " + console + " differs";
        }
        totals.stream_conversions++;
    }

    totals.stream_channels += decoded.channels.size();
    totals.aac_streams++;

    return {};
}

std::string StreamProblem(const std::vector<uint8_t>& ours, const std::vector<uint8_t>& theirs, Totals& totals)
{
    if (ours != theirs)
    {
        return "'s file differs";
    }

    // The file of each other console, and back. Between the Wii U and the Switch, the stream comes back with its bytes.
    // A stream that leaves the 3DS drops the samples past its end, and an older version holds no original loop: a
    // stream that goes to the 3DS or leaves it comes back with its samples.
    const sar::StreamFile read = sar::StreamFile::Read(theirs);
    const sar::Pcm decoded = sar::DecodeStream(read);
    for (const sar::Flavor other_console : song::OtherConsoles(read.flavor))
    {
        // The samples, the rate and the loop that plays. Where the 3DS takes part, the loop's original start
        // (StreamFile::original_loop_start) can differ too. The decoded samples leave it out: it plays no part.
        const bool ctr = read.flavor == sar::Flavor::kCtr || other_console == sar::Flavor::kCtr;
        const auto plays_the_same = [&decoded](const sar::Pcm& samples)
        {
            return samples.channels == decoded.channels && samples.sample_rate == decoded.sample_rate &&
                   samples.loop == decoded.loop && samples.loop_start == decoded.loop_start &&
                   samples.loop_end == decoded.loop_end;
        };
        const std::string console = song::ConsoleName(other_console);
        const uint32_t other_version = song::ExportVersions(song::ItemKind::kStream, other_console).file;
        const std::vector<uint8_t> other = song::ConvertStreamFile(ours, other_console, other_version);
        const sar::StreamFile converted = sar::StreamFile::Read(other);
        if (converted.flavor != other_console || converted.version != other_version ||
            !plays_the_same(sar::DecodeStream(converted)))
        {
            return "'s file for the " + console + " differs";
        }
        const std::vector<uint8_t> again = song::ConvertStreamFile(other, read.flavor, read.version);
        if (ctr ? !plays_the_same(sar::DecodeStream(sar::StreamFile::Read(again))) : again != theirs)
        {
            return " comes back from the " + console + (ctr ? " with other samples" : " with other bytes");
        }
        const uint32_t version = song::ExportVersions(song::ItemKind::kStream, read.flavor).file;
        const sar::StreamFile back = sar::StreamFile::Read(song::ConvertStreamFile(other, read.flavor, version));
        if (back.version != version || sar::DecodeStream(back).channels != decoded.channels)
        {
            return " comes back from the " + console + " with other samples in the version that goes to its console";
        }
        totals.stream_conversions++;
    }

    // The WAV file.
    const std::optional<WavRead> wav = ReadWav(song::StreamAsWav(ours));
    if (!wav || !Holds(*wav, decoded))
    {
        return "'s WAV file differs";
    }
    totals.stream_channels += decoded.channels.size();

    return {};
}

} // namespace hokuto::tools::items
