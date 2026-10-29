// SPDX-License-Identifier: MIT

// Tests the AAC streams (src/song/aac.h, and their parts of src/song/streams.h) on tests/aac/burst.adts. The Mac's
// afconvert encoded the file from a second of 48 kHz stereo: silence, then from sample 4,800 a burst of 9,600 samples,
// noise on the left and a 1 kHz sine on the right, then silence (tools/make_aac_burst.py). Apple's encoder puts 2,112
// samples of priming before the source (Apple's Technical Note TN2258). The system's decoder has to give each frame's
// 1,024 samples, the burst 2,112 samples late, and silence around it. An AAC stream then takes a loop, goes out as a
// WAV file and as a stream file of the Wii U, and plays as a stream file of the 3DS. The system's encoder then encodes
// the same source. Its delay has to be the one that EncoderDelay measures, 2,112 samples on the Mac, and the sine has
// to come back within 25 dB.

#include "song/aac.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <stdexcept>
#include <vector>

#include "ntdlib/sar/stream_file.h"
#include "xsflib/files.h"
#include "xsflib/testing/check.h"

#include "song/streams.h"

namespace
{

namespace sar = ntdlib::sar;

constexpr std::size_t kFrames = 49;
constexpr std::size_t kStart = 4800;
constexpr std::size_t kBurst = 9600;
constexpr std::size_t kPriming = 2112;
constexpr double kPeak = 8000.0;
constexpr double kPi = 3.14159265358979323846;

// The bytes of tests/aac/burst.adts. Throws std::runtime_error when the test can't read the file.
std::vector<uint8_t> Burst()
{
    std::vector<uint8_t> bytes;
    if (!xsflib::ReadFile(xsflib::PathFromUtf8(HOKUTO_AAC_DIR) / "burst.adts", bytes))
    {
        throw std::runtime_error("The test can't read tests/aac/burst.adts.");
    }

    return bytes;
}

// The left channel's noise as tools/make_aac_burst.py makes it.
std::vector<double> Noise()
{
    std::vector<double> noise;
    uint64_t state = 12345;
    for (std::size_t i = 0; i < kBurst; i++)
    {
        state = (state * 1103515245 + 12345) % (uint64_t{1} << 31);
        noise.push_back(static_cast<double>((state >> 15) % (2 * 8000 + 1)) - kPeak);
    }

    return noise;
}

// Whether `call` throws std::runtime_error: the system's codec refuses.
template <class Call>
bool Refused(const Call& call)
{
    try
    {
        call();
    }
    catch (const std::runtime_error&)
    {
        return true;
    }

    return false;
}

// The largest sample of `channel` from `from` up to `to`.
int Largest(const std::vector<int16_t>& channel, std::size_t from, std::size_t to)
{
    int largest = 0;
    for (std::size_t i = from; i < to && i < channel.size(); i++)
    {
        largest = std::max(largest, std::abs(int{channel[i]}));
    }

    return largest;
}

// The burst's file holds 49 frames in ADTS, and a stream file of the 3DS holds none. A file without its last byte holds
// one frame fewer. The system's decoder gives 1,024 stereo samples at 48 kHz for each frame. The noise lines up best
// 2,112 samples late, the sine comes back within 25 dB, and silence lies a frame before the burst and a frame after it.
void TestDecoding()
{
    const std::vector<uint8_t> bytes = Burst();
    XSFLIB_CHECK(hokuto::song::IsAdts(bytes));
    const std::vector<uint8_t> kStreamFile = {'C', 'S', 'T', 'M', 0xFF, 0xFE};
    XSFLIB_CHECK(!hokuto::song::IsAdts(kStreamFile));
    SEQLIB_CHECK_EQ(hokuto::song::AdtsFrames(bytes), uint32_t{kFrames});
    SEQLIB_CHECK_EQ(hokuto::song::AdtsFrames(std::span(bytes).first(bytes.size() - 1)), uint32_t{kFrames - 1});

    const ntdlib::wave::Pcm pcm = hokuto::song::DecodeAdts(bytes);
    SEQLIB_CHECK_EQ(pcm.sample_rate, uint32_t{48000});
    SEQLIB_CHECK_EQ(pcm.channels.size(), std::size_t{2});
    XSFLIB_CHECK(!pcm.loop);
    SEQLIB_CHECK_EQ(pcm.loop_end, uint32_t{kFrames * 1024});
    SEQLIB_CHECK_EQ(pcm.channels[0].size(), std::size_t{kFrames * 1024});
    SEQLIB_CHECK_EQ(pcm.channels[1].size(), std::size_t{kFrames * 1024});
    if (pcm.channels.size() != 2 || pcm.channels[1].size() != kFrames * 1024)
    {
        return;
    }

    // The noise lines up best with the priming's delay.
    const std::vector<double> noise = Noise();
    std::size_t best = 0;
    double best_sum = -1e300;
    for (std::size_t lag = 1900; lag <= 2300; lag++)
    {
        double sum = 0.0;
        for (std::size_t i = 0; i < kBurst; i++)
        {
            sum += noise[i] * pcm.channels[0][kStart + lag + i];
        }
        if (sum > best_sum)
        {
            best_sum = sum;
            best = lag;
        }
    }
    SEQLIB_CHECK_EQ(best, kPriming);

    // The sine comes through within 25 dB, away from the burst's ends.
    double error = 0.0;
    double signal = 0.0;
    for (std::size_t i = 1024; i < kBurst - 1024; i++)
    {
        const double expected = kPeak * std::sin(2.0 * kPi * 1000.0 * static_cast<double>(i) / 48000.0);
        const double difference = pcm.channels[1][kStart + kPriming + i] - expected;
        error += difference * difference;
        signal += expected * expected;
    }
    XSFLIB_CHECK(10.0 * std::log10(error / signal) < -25.0);

    // Silence a frame before the burst and a frame after it.
    for (const std::vector<int16_t>& channel : pcm.channels)
    {
        XSFLIB_CHECK(Largest(channel, 0, kStart + kPriming - 1024) <= 4);
        XSFLIB_CHECK(Largest(channel, kStart + kPriming + kBurst + 1024, channel.size()) <= 4);
    }
}

// An AAC stream takes the loop of its sound's entry and ends at the loop's end. Without a loop, the stream ends with
// its samples. A loop's end past the samples moves back to their end. The stream's WAV file holds the loop's start and
// last sample in the smpl chunk. A stream file of the Wii U takes the stream in DSP-ADPCM with the loop. The 3DS plays
// the stream as a stream file in 16-bit PCM, within two steps of the decoder's samples, with the loop at the start of
// the next block. A stream file plays as it is.
void TestStreams()
{
    const std::vector<uint8_t> bytes = Burst();
    const hokuto::song::AacStream kLooped{hokuto::song::AacLoop{6000, 30000}};

    // The loop of the entry, and the stream ends at its end. Without a loop, the stream ends with its samples.
    const sar::Pcm pcm = hokuto::song::DecodeStreamFile(bytes, kLooped);
    XSFLIB_CHECK(pcm.loop);
    SEQLIB_CHECK_EQ(pcm.loop_start, uint32_t{6000});
    SEQLIB_CHECK_EQ(pcm.loop_end, uint32_t{30000});
    SEQLIB_REQUIRE_EQ(pcm.channels.size(), std::size_t{2});
    SEQLIB_CHECK_EQ(pcm.channels[0].size(), std::size_t{30000});
    SEQLIB_REQUIRE_EQ(pcm.channels[1].size(), std::size_t{30000});
    const sar::Pcm whole = hokuto::song::DecodeStreamFile(bytes);
    XSFLIB_CHECK(!whole.loop);
    SEQLIB_REQUIRE_EQ(whole.channels.size(), std::size_t{2});
    SEQLIB_CHECK_EQ(whole.channels[0].size(), std::size_t{kFrames * 1024});
    const sar::Pcm past = hokuto::song::DecodeStreamFile(bytes, {hokuto::song::AacLoop{6000, 90000}});
    SEQLIB_CHECK_EQ(past.loop_end, uint32_t{kFrames * 1024});

    // The WAV file holds the samples and the loop in its smpl chunk: the start and the last sample.
    const std::vector<uint8_t> wav = hokuto::song::StreamAsWav(bytes, kLooped);
    std::size_t smpl = 0;
    for (std::size_t at = 12; at + 8 <= wav.size();)
    {
        const uint32_t size =
            wav[at + 4] | (uint32_t{wav[at + 5]} << 8) | (uint32_t{wav[at + 6]} << 16) | (uint32_t{wav[at + 7]} << 24);
        if (std::memcmp(wav.data() + at, "smpl", 4) == 0)
        {
            smpl = at;
        }
        at += 8 + size + (size & 1);
    }

    const auto u32 = [&](std::size_t at)
    {
        return wav[at] | (uint32_t{wav[at + 1]} << 8) | (uint32_t{wav[at + 2]} << 16) | (uint32_t{wav[at + 3]} << 24);
    };

    SEQLIB_REQUIRE(smpl != 0 && smpl + 8 + 60 <= wav.size());
    SEQLIB_REQUIRE(u32(smpl + 4) >= 60);
    SEQLIB_CHECK_EQ(u32(smpl + 8 + 28), uint32_t{1});
    SEQLIB_CHECK_EQ(u32(smpl + 8 + 44), uint32_t{6000});
    SEQLIB_CHECK_EQ(u32(smpl + 8 + 48), uint32_t{29999});

    // A stream file of the Wii U in DSP-ADPCM, with the loop.
    const sar::StreamFile wii_u =
        sar::StreamFile::Read(hokuto::song::ConvertStreamFile(bytes, sar::Flavor::kCafe, 0x00030000, kLooped));
    XSFLIB_CHECK(wii_u.flavor == sar::Flavor::kCafe);
    XSFLIB_CHECK(wii_u.loop);
    XSFLIB_CHECK(wii_u.encoding == sar::WaveEncoding::kDspAdpcm);
    SEQLIB_CHECK_EQ(wii_u.sample_rate, uint32_t{48000});

    // A stream file of the 3DS in 16-bit PCM, for ntdlib's players. Its samples are the decoder's. The decoder can move
    // a sample by up to two steps from one decoding to the next: Audio Toolbox did, depending on where the process kept
    // its buffers. The loop moves to the start of the block after the one that holds it, and the samples between follow
    // the loop's end (ntdlib's MakeStream).
    const sar::StreamFile ctr = sar::StreamFile::Read(hokuto::song::PlayableStream(bytes, kLooped));
    XSFLIB_CHECK(ctr.flavor == sar::Flavor::kCtr);
    XSFLIB_CHECK(ctr.loop);
    XSFLIB_CHECK(ctr.encoding == sar::WaveEncoding::kPcm16);
    const sar::Pcm played = sar::DecodeStream(ctr);
    const uint32_t block = sar::StreamBlockSamples(sar::WaveEncoding::kPcm16);
    const uint32_t loop_start = (6000 / block + 1) * block;
    SEQLIB_CHECK_EQ(played.loop_start, loop_start);
    SEQLIB_REQUIRE_EQ(played.channels.size(), std::size_t{2});
    SEQLIB_REQUIRE_EQ(played.channels[1].size(), std::size_t{30000 + loop_start - 6000});
    XSFLIB_CHECK(std::equal(pcm.channels[1].begin(), pcm.channels[1].end(), played.channels[1].begin(),
                            [](int16_t a, int16_t b) { return std::abs(int{a} - int{b}) <= 2; }));

    // A stream file stays as it is.
    const std::vector<uint8_t> converted = hokuto::song::ConvertStreamFile(bytes, sar::Flavor::kCtr, 0x02000000);
    XSFLIB_CHECK(hokuto::song::PlayableStream(converted) == converted);
}

// The source of tools/make_aac_burst.py: silence, the burst, then silence, for a second.
ntdlib::wave::Pcm Source()
{
    ntdlib::wave::Pcm pcm;
    pcm.sample_rate = 48000;
    pcm.channels.assign(2, std::vector<int16_t>(48000, 0));
    const std::vector<double> noise = Noise();
    for (std::size_t i = 0; i < kBurst; i++)
    {
        pcm.channels[0][kStart + i] = static_cast<int16_t>(noise[i]);
        pcm.channels[1][kStart + i] =
            static_cast<int16_t>(std::lround(kPeak * std::sin(2.0 * kPi * 1000.0 * static_cast<double>(i) / 48000.0)));
    }
    pcm.loop_end = 48000;

    return pcm;
}

// The system's encoder encodes the burst's source. The encoder's delay is the one that EncoderDelay measures: 2,112
// samples on the Mac, and from 1,024 to 4,096 elsewhere. The noise lines up best after the delay, and the sine comes
// back within 25 dB. The encoder takes a mono source at 96 kbit/s, and refuses 3 channels, 32 kHz and 100 kbit/s.
void TestEncoding()
{
    const ntdlib::wave::Pcm source = Source();
    const std::vector<uint8_t> bytes = hokuto::song::EncodeAdts(source, 128000);
    XSFLIB_CHECK(hokuto::song::IsAdts(bytes));
    const uint32_t delay = hokuto::song::EncoderDelay(48000, 2);
#ifdef __APPLE__
    SEQLIB_CHECK_EQ(delay, uint32_t{kPriming});
#endif
    XSFLIB_CHECK(delay >= 1024 && delay <= 4096);
    if (delay < 1024 || delay > 4096)
    {
        std::printf("The system's encoder has a delay of %u samples.\n", delay);
    }

    // Every sample comes back after the delay. The noise lines up best there, and the sine comes through.
    const ntdlib::wave::Pcm pcm = hokuto::song::DecodeAdts(bytes);
    SEQLIB_CHECK_EQ(pcm.sample_rate, uint32_t{48000});
    SEQLIB_REQUIRE_EQ(pcm.channels.size(), std::size_t{2});
    SEQLIB_REQUIRE(pcm.channels[0].size() >= delay + std::size_t{48000} &&
                   pcm.channels[1].size() >= delay + std::size_t{48000});

    const std::vector<double> noise = Noise();
    std::size_t best = 0;
    double best_sum = -1e300;
    for (std::size_t lag = 0; lag <= 4096; lag++)
    {
        double sum = 0.0;
        for (std::size_t i = 0; i < kBurst; i++)
        {
            sum += noise[i] * pcm.channels[0][kStart + lag + i];
        }
        if (sum > best_sum)
        {
            best_sum = sum;
            best = lag;
        }
    }
    SEQLIB_CHECK_EQ(best, std::size_t{delay});

    double error = 0.0;
    double signal = 0.0;
    for (std::size_t i = 1024; i < kBurst - 1024; i++)
    {
        const double expected = source.channels[1][kStart + i];
        const double difference = pcm.channels[1][kStart + delay + i] - expected;
        error += difference * difference;
        signal += expected * expected;
    }
    XSFLIB_CHECK(10.0 * std::log10(error / signal) < -25.0);

    // The encoders take 1 or 2 channels at 44,100 or 48,000 Hz, at one of the rates of bits.
    ntdlib::wave::Pcm mono = source;
    mono.channels.resize(1);
    XSFLIB_CHECK(hokuto::song::IsAdts(hokuto::song::EncodeAdts(mono, 96000)));

    ntdlib::wave::Pcm wide = source;
    wide.channels.push_back(source.channels[0]);
    ntdlib::wave::Pcm slow = source;
    slow.sample_rate = 32000;

    XSFLIB_CHECK(Refused([&] { hokuto::song::EncodeAdts(wide, 128000); }));
    XSFLIB_CHECK(Refused([&] { hokuto::song::EncodeAdts(slow, 128000); }));
    XSFLIB_CHECK(Refused([&] { hokuto::song::EncodeAdts(source, 100000); }));
}

// The system's decoder refuses eight bytes that aren't AAC.
void TestRefusals()
{
    const std::vector<uint8_t> kNotAac = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};

    XSFLIB_CHECK(Refused([&] { hokuto::song::DecodeAdts(kNotAac); }));
}

} // namespace

int main()
{
    TestDecoding();
    TestStreams();
    TestEncoding();
    TestRefusals();

    return xsflib::testing::Summary("aac");
}
