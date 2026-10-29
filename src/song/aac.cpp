// SPDX-License-Identifier: MIT

#include "song/aac.h"

#include <algorithm>
#include <cstddef>
#include <stdexcept>

#include "song/adts.h"

namespace hokuto::song
{
namespace
{

// The rates that an ADTS header names, by their index.
constexpr std::array<uint32_t, 13> kRates = {96000, 88200, 64000, 48000, 44100, 32000, 24000,
                                             22050, 16000, 12000, 11025, 8000,  7350};

// The burst of noise that measures the encoder's delay: its start and its length in samples, and the length of the
// whole source.
constexpr std::size_t kBurstStart = 4096;
constexpr std::size_t kBurstLength = 4096;
constexpr std::size_t kSourceLength = 16384;
constexpr std::size_t kLongestDelay = 8192;

// The length of the ADTS frame at the start of `bytes`, its header included, or 0 where no whole frame starts.
std::size_t FrameLength(std::span<const uint8_t> bytes)
{
    if (!IsAdts(bytes) || bytes.size() < 7)
    {
        return 0;
    }

    const std::size_t length = (std::size_t{bytes[3] & 0x03u} << 11) | (std::size_t{bytes[4]} << 3) | (bytes[5] >> 5);
    return length >= 7 && length <= bytes.size() ? length : 0;
}

} // namespace

bool IsAdts(std::span<const uint8_t> bytes)
{
    return bytes.size() >= 2 && bytes[0] == 0xFF && (bytes[1] & 0xF6) == 0xF0;
}

uint32_t AdtsFrames(std::span<const uint8_t> bytes)
{
    uint32_t frames = 0;
    while (const std::size_t length = FrameLength(bytes))
    {
        bytes = bytes.subspan(length);
        frames++;
    }

    return frames;
}

void CheckEncodable(const ntdlib::wave::Pcm& pcm, uint32_t bit_rate)
{
    if (pcm.channels.empty() || pcm.channels.size() > 2)
    {
        throw std::runtime_error("An AAC stream holds 1 or 2 channels.");
    }
    if (pcm.sample_rate != 44100 && pcm.sample_rate != 48000)
    {
        throw std::runtime_error("An AAC stream plays at 44,100 or 48,000 Hz.");
    }
    const std::size_t length = pcm.channels.front().size();
    if (length == 0 ||
        std::ranges::any_of(pcm.channels, [length](const auto& channel) { return channel.size() != length; }))
    {
        throw std::runtime_error("An AAC stream requires samples, the same number in each channel.");
    }
    if (std::ranges::find(kAacBitRates, bit_rate) == kAacBitRates.end())
    {
        throw std::runtime_error("An AAC stream takes 96, 128, 160 or 192 kbit/s.");
    }
}

void AppendAdtsFrame(std::vector<uint8_t>& bytes, std::span<const uint8_t> packet, uint32_t rate, uint32_t channels)
{
    const auto index = static_cast<uint32_t>(std::ranges::find(kRates, rate) - kRates.begin());
    const std::size_t length = packet.size() + 7;
    if (index >= kRates.size() || channels == 0 || channels > 7 || length >= (std::size_t{1} << 13))
    {
        throw std::runtime_error("The AAC frame doesn't fit an ADTS header.");
    }

    // The sync word, MPEG-4, layer 0 and no CRC. AAC-LC, the rate and the channels. The frame's length, the buffer's
    // fullness as variable (0x7FF) and one raw block.
    const uint8_t header[7] = {0xFF,
                               0xF1,
                               static_cast<uint8_t>((1u << 6) | (index << 2) | (channels >> 2)),
                               static_cast<uint8_t>(((channels & 0x03u) << 6) | (length >> 11)),
                               static_cast<uint8_t>(length >> 3),
                               static_cast<uint8_t>(((length & 0x07u) << 5) | 0x1Fu),
                               0xFC};
    bytes.insert(bytes.end(), std::begin(header), std::end(header));
    bytes.insert(bytes.end(), packet.begin(), packet.end());
}

uint32_t EncoderDelay(uint32_t rate, std::size_t channels)
{
    // Noise from a linear congruential generator, in every channel, between silence.
    ntdlib::wave::Pcm source;
    source.sample_rate = rate;
    std::vector<int16_t> samples(kSourceLength, 0);
    uint64_t state = 12345;
    for (std::size_t i = 0; i < kBurstLength; i++)
    {
        state = (state * 1103515245 + 12345) % (uint64_t{1} << 31);
        samples[kBurstStart + i] = static_cast<int16_t>(static_cast<int>((state >> 15) % 16001) - 8000);
    }
    source.channels.assign(channels, samples);
    source.loop_end = kSourceLength;
    const ntdlib::wave::Pcm decoded = DecodeAdts(EncodeAdts(source, kAacBitRates[1]));

    // The lag at which the decoded noise lines up best with the source's.
    const std::vector<int16_t>& out = decoded.channels.front();
    std::size_t best = 0;
    double best_sum = 0.0;
    for (std::size_t lag = 0; lag <= kLongestDelay && kBurstStart + lag + kBurstLength <= out.size(); lag++)
    {
        double sum = 0.0;
        for (std::size_t i = 0; i < kBurstLength; i++)
        {
            sum += static_cast<double>(samples[kBurstStart + i]) * out[kBurstStart + lag + i];
        }
        if (sum > best_sum)
        {
            best_sum = sum;
            best = lag;
        }
    }
    if (best_sum <= 0.0)
    {
        throw std::runtime_error("The system's AAC encoder lost a burst of noise.");
    }

    return static_cast<uint32_t>(best);
}

} // namespace hokuto::song
