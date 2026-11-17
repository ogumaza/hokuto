// SPDX-License-Identifier: MIT

// AAC through FFmpeg's program (song/ffmpeg.h): the samples go in and out as 16-bit PCM, little-endian and interleaved.

#include "song/ffmpeg.h"

#include <array>
#include <cstddef>
#include <stdexcept>
#include <string>

namespace hokuto::song
{
namespace
{

// The rates that an ADTS header names, by their index.
constexpr std::array<uint32_t, 13> kRates = {96000, 88200, 64000, 48000, 44100, 32000, 24000,
                                             22050, 16000, 12000, 11025, 8000,  7350};

// The bytes of the whole ADTS frames at the start of `bytes`.
std::span<const uint8_t> WholeFrames(std::span<const uint8_t> bytes)
{
    std::size_t end = 0;
    while (bytes.size() - end >= 7 && bytes[end] == 0xFF && (bytes[end + 1] & 0xF6) == 0xF0)
    {
        const std::size_t length = (std::size_t{bytes[end + 3] & 0x03u} << 11) | (std::size_t{bytes[end + 4]} << 3) |
                                   (std::size_t{bytes[end + 5]} >> 5);
        if (length < 7 || length > bytes.size() - end)
        {
            break;
        }
        end += length;
    }

    return bytes.first(end);
}

// The options of raw 16-bit PCM at `rate` with `channels`.
std::vector<std::string> PcmOptions(uint32_t rate, std::size_t channels)
{
    return {"-f", "s16le", "-ar", std::to_string(rate), "-ac", std::to_string(channels)};
}

} // namespace

std::string FfmpegMissing(const char* doing)
{
    return std::string("Hokuto needs FFmpeg to ") + doing +
           " AAC streams on this system, and can't find it. Install FFmpeg, then try again.";
}

std::string FfmpegFailed(const char* doing, const std::string& detail)
{
    std::string message = std::string("FFmpeg couldn't ") + doing + " the AAC stream.";
    if (!detail.empty())
    {
        message += " " + detail;
    }

    return message;
}

ntdlib::wave::Pcm DecodeAdtsWithFfmpeg(std::span<const uint8_t> bytes)
{
    const std::span<const uint8_t> frames = WholeFrames(bytes);
    const uint32_t rate_index = (bytes[2] >> 2) & 0x0Fu;
    // The channel configuration: 1 to 6 channels as it says, and 8 for a configuration of 7 (7.1).
    const uint32_t configuration = ((bytes[2] & 0x01u) << 2) | (bytes[3] >> 6);
    const uint32_t channels = configuration == 7 ? 8 : configuration;
    if (rate_index >= kRates.size() || channels == 0)
    {
        throw std::runtime_error("The AAC stream header has a sample rate or channel count that Hokuto can't read.");
    }

    const uint32_t rate = kRates[rate_index];
    std::vector<std::string> output = PcmOptions(rate, channels);
    output.insert(output.begin(), {"-c:a", "pcm_s16le"});
    const std::vector<uint8_t> out = RunFfmpeg({"-f", "aac"}, frames, output, "decode");

    ntdlib::wave::Pcm pcm;
    pcm.sample_rate = rate;
    pcm.channels.resize(channels);
    const std::size_t step = 2 * std::size_t{channels};
    for (std::size_t i = 0; i + step <= out.size(); i += step)
    {
        for (uint32_t channel = 0; channel < channels; channel++)
        {
            const std::size_t at = i + 2 * std::size_t{channel};
            pcm.channels[channel].push_back(static_cast<int16_t>(out[at] | (out[at + 1] << 8)));
        }
    }

    pcm.loop_end = static_cast<uint32_t>(pcm.channels.front().size());
    return pcm;
}

std::vector<uint8_t> EncodeAdtsWithFfmpeg(const ntdlib::wave::Pcm& pcm, uint32_t bit_rate)
{
    const std::size_t channels = pcm.channels.size();
    const std::size_t length = pcm.channels.front().size();
    std::vector<uint8_t> in(length * channels * 2);
    for (std::size_t i = 0; i < length; i++)
    {
        for (std::size_t channel = 0; channel < channels; channel++)
        {
            const auto sample = static_cast<uint16_t>(pcm.channels[channel][i]);
            const std::size_t at = (i * channels + channel) * 2;
            in[at] = static_cast<uint8_t>(sample);
            in[at + 1] = static_cast<uint8_t>(sample >> 8);
        }
    }

    return RunFfmpeg(PcmOptions(pcm.sample_rate, channels), in,
                     {"-c:a", "aac", "-b:a", std::to_string(bit_rate), "-f", "adts"}, "encode");
}

} // namespace hokuto::song
