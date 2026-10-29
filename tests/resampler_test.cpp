// SPDX-License-Identifier: MIT

// Tests the resampler (src/audio/resampler.h). A sine below the new Nyquist frequency comes out as the same sine at the
// new rate. One above it comes out silent. The new samples last as long as the old ones. A loop keeps its place, and
// the samples after its end follow on from its start, at a lower rate and at a higher one. A render's frames go to a
// higher rate and to a lower one, each channel apart.

#include "audio/resampler.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

#include "xsflib/testing/check.h"

using hokuto::audio::Resample;
using hokuto::audio::ResampledPlace;
using hokuto::audio::ResampleFrames;

namespace
{

constexpr double kPi = 3.14159265358979323846;
constexpr double kAmplitude = 16000.0;

// `count` samples of a sine of `frequency` Hz at `rate` samples a second.
std::vector<int16_t> Sine(double frequency, uint32_t rate, std::size_t count)
{
    std::vector<int16_t> samples(count);
    for (std::size_t i = 0; i < count; i++)
    {
        samples[i] = static_cast<int16_t>(
            std::lround(kAmplitude * std::sin(2.0 * kPi * frequency * static_cast<double>(i) / rate)));
    }

    return samples;
}

// The largest difference between `samples` and `expected` from sample `from` to sample `to`.
double LargestError(const std::vector<int16_t>& samples, const std::vector<int16_t>& expected, std::size_t from,
                    std::size_t to)
{
    double largest = 0.0;
    for (std::size_t i = from; i < to && i < samples.size() && i < expected.size(); i++)
    {
        largest = std::max(largest, std::abs(static_cast<double>(samples[i]) - expected[i]));
    }

    return largest;
}

// The largest sample of `samples` from sample `from` to sample `to`.
double Peak(const std::vector<int16_t>& samples, std::size_t from, std::size_t to)
{
    double peak = 0.0;
    for (std::size_t i = from; i < to && i < samples.size(); i++)
    {
        peak = std::max(peak, std::abs(static_cast<double>(samples[i])));
    }

    return peak;
}

// `count` frames of two channels at `rate` frames a second: a sine of `left` Hz on the first channel, and one of
// `right` Hz on the second, each at half of full scale.
std::vector<float> Frames(double left, double right, uint32_t rate, std::size_t count)
{
    std::vector<float> frames(count * 2);
    for (std::size_t i = 0; i < count; i++)
    {
        const double time = static_cast<double>(i) / rate;
        frames[i * 2] = static_cast<float>(0.5 * std::sin(2.0 * kPi * left * time));
        frames[i * 2 + 1] = static_cast<float>(0.5 * std::sin(2.0 * kPi * right * time));
    }

    return frames;
}

// The largest difference between channel `channel` of `frames` and of `expected` from frame `from` to frame `to`.
double LargestFrameError(const std::vector<float>& frames, const std::vector<float>& expected, std::size_t channel,
                         std::size_t from, std::size_t to)
{
    double largest = 0.0;
    for (std::size_t i = from; i < to && i * 2 < frames.size() && i * 2 < expected.size(); i++)
    {
        largest = std::max(largest, std::abs(static_cast<double>(frames[i * 2 + channel]) - expected[i * 2 + channel]));
    }

    return largest;
}

// Whether `call` throws std::invalid_argument.
template <typename Call>
bool Refused(Call call)
{
    try
    {
        call();
    }
    catch (const std::invalid_argument&)
    {
        return true;
    }

    return false;
}

// A sine of 1 kHz goes from 48 kHz to 32 kHz and to 22,050 Hz within a few steps of 16 bits, 80 dB below the sine's
// level. The edges fade in from the silence around the samples. A sine of 17 kHz lies above the Nyquist frequency of 32
// kHz and comes out more than 80 dB down. One of 14 kHz lies in the passband and comes out whole.
void TestSines()
{
    const std::vector<int16_t> one = Resample(Sine(1000.0, 48000, 48000), 48000, 32000);
    const std::vector<int16_t> lower = Resample(Sine(1000.0, 48000, 48000), 48000, 22050);
    const std::vector<int16_t> high = Resample(Sine(17000.0, 48000, 48000), 48000, 32000);
    const std::vector<int16_t> pass = Resample(Sine(14000.0, 48000, 48000), 48000, 32000);

    SEQLIB_CHECK_EQ(one.size(), std::size_t{32000});
    SEQLIB_CHECK_EQ(lower.size(), std::size_t{22050});
    XSFLIB_CHECK(LargestError(one, Sine(1000.0, 32000, 32000), 1000, 31000) <= kAmplitude * 1e-4);
    XSFLIB_CHECK(LargestError(lower, Sine(1000.0, 22050, 22050), 1000, 21050) <= kAmplitude * 1e-4);
    XSFLIB_CHECK(Peak(high, 1000, 31000) <= kAmplitude * 1e-4);
    XSFLIB_CHECK(LargestError(pass, Sine(14000.0, 32000, 32000), 1000, 31000) <= kAmplitude * 2e-3);
}

// The new samples last as long as the old ones, to the nearest sample. A loop starts at its place at the new rate, and
// the loop lasts as long. A sine with a whole number of cycles in its loop goes on into the loop's start: the new
// loop's samples are the sine's, up to its end. The filter reads the samples after the loop as the console plays them.
// Those samples keep the end clean. An intro of 1,441 samples at 48 kHz takes 961 at 32 kHz, and a loop of 4,799 takes
// 3,199. Each part stretches to fit: its new samples hold the sine at their times in the old samples.
void TestLoop()
{
    // 1 kHz: a cycle of 48 samples at 48 kHz and 32 at 32 kHz. The loop holds 100 cycles after an intro of 30.
    const std::vector<int16_t> sine = Sine(1000.0, 48000, 130 * 48);
    const std::vector<int16_t> looped = Resample(sine, 48000, 32000, 30 * 48);
    const std::vector<int16_t> expected = Sine(1000.0, 32000, 130 * 32);
    const std::vector<int16_t> stretched = Resample(sine, 48000, 32000, 1441);
    std::vector<int16_t> at_times(961 + 3199);
    for (std::size_t k = 0; k < at_times.size(); k++)
    {
        const double time = k < 961 ? k * 1441.0 / 961.0 : 1441.0 + static_cast<double>(k - 961) * 4799.0 / 3199.0;
        at_times[k] = static_cast<int16_t>(std::lround(kAmplitude * std::sin(2.0 * kPi * 1000.0 * time / 48000.0)));
    }

    SEQLIB_CHECK_EQ(ResampledPlace(30 * 48, 48000, 32000), std::size_t{30 * 32});
    SEQLIB_CHECK_EQ(ResampledPlace(3, 48000, 32000), std::size_t{2});
    SEQLIB_CHECK_EQ(ResampledPlace(5, 48000, 32000), std::size_t{3});
    SEQLIB_CHECK_EQ(ResampledPlace(1, 48000, 32000), std::size_t{1});
    SEQLIB_CHECK_EQ(ResampledPlace(1441, 48000, 32000), std::size_t{961});
    SEQLIB_CHECK_EQ(stretched.size(), at_times.size());
    XSFLIB_CHECK(LargestError(stretched, at_times, 100, at_times.size() - 100) <= kAmplitude * 1e-4);
    SEQLIB_CHECK_EQ(Resample(Sine(1000.0, 48000, 3), 48000, 32000).size(), std::size_t{2});
    SEQLIB_CHECK_EQ(Resample(Sine(1000.0, 48000, 1), 48000, 8000).size(), std::size_t{1});
    SEQLIB_CHECK_EQ(looped.size(), std::size_t{130 * 32});
    XSFLIB_CHECK(LargestError(looped, expected, 30 * 32, 130 * 32) <= kAmplitude * 1e-4);
    SEQLIB_CHECK_EQ(Resample(sine, 48000, 32000, 130 * 48 - 1).size(), ResampledPlace(130 * 48 - 1, 48000, 32000) + 1);
}

// A sine of 1 kHz goes from 32 kHz up to 48 kHz within a few steps, with a loop of 100 cycles after an intro of 30. The
// new loop's samples are the sine's, up to its end, as in TestLoop.
void TestHigherRate()
{
    const std::vector<int16_t> sine = Sine(1000.0, 32000, 130 * 32);
    const std::vector<int16_t> looped = Resample(sine, 32000, 48000, 30 * 32);
    const std::vector<int16_t> whole = Resample(Sine(1000.0, 32000, 32000), 32000, 48000);

    SEQLIB_CHECK_EQ(whole.size(), std::size_t{48000});
    XSFLIB_CHECK(LargestError(whole, Sine(1000.0, 48000, 48000), 1000, 47000) <= kAmplitude * 1e-4);
    SEQLIB_CHECK_EQ(ResampledPlace(30 * 32, 32000, 48000), std::size_t{30 * 48});
    SEQLIB_CHECK_EQ(looped.size(), std::size_t{130 * 48});
    XSFLIB_CHECK(LargestError(looped, Sine(1000.0, 48000, 130 * 48), 30 * 48, 130 * 48) <= kAmplitude * 1e-4);
}

// A rate of 0, an equal rate, no samples and a loop start past the samples are refused.
void TestRefusals()
{
    const std::vector<int16_t> sine = Sine(1000.0, 48000, 100);

    XSFLIB_CHECK(Refused([&] { Resample(sine, 48000, 0); }));
    XSFLIB_CHECK(Refused([&] { Resample(sine, 0, 32000); }));
    XSFLIB_CHECK(Refused([&] { Resample(sine, 48000, 48000); }));
    XSFLIB_CHECK(Refused([&] { Resample({}, 48000, 32000); }));
    XSFLIB_CHECK(Refused([&] { Resample(sine, 48000, 32000, 100); }));
    XSFLIB_CHECK(!Refused([&] { Resample(sine, 48000, 32000, 99); }));
}

// A second of two channels goes from the 3DS's 32,728 Hz to 48 kHz and from 48 kHz to 32,728 Hz. Sines of 1 kHz and 14
// kHz lie in the passband of both and come out as the same sines, within 80 dB and 50 dB. Each channel keeps its sine.
// A sine of 17 kHz lies above the Nyquist frequency of 32,728 Hz and comes out more than 80 dB down. The new frames
// last as long. Frames at the same rate come back as they are. A rate of 0, no channels and samples that don't fill
// their frames are refused.
void TestFrames()
{
    const std::vector<float> up = ResampleFrames(Frames(1000.0, 14000.0, 32728, 32728), 2, 32728, 48000);
    const std::vector<float> down = ResampleFrames(Frames(14000.0, 1000.0, 48000, 48000), 2, 48000, 32728);
    const std::vector<float> high = ResampleFrames(Frames(17000.0, 17000.0, 48000, 48000), 2, 48000, 32728);
    const std::vector<float> up_expected = Frames(1000.0, 14000.0, 48000, 48000);
    const std::vector<float> down_expected = Frames(14000.0, 1000.0, 32728, 32728);

    SEQLIB_CHECK_EQ(up.size(), std::size_t{48000 * 2});
    SEQLIB_CHECK_EQ(down.size(), std::size_t{32728 * 2});
    XSFLIB_CHECK(LargestFrameError(up, up_expected, 0, 1000, 47000) <= 0.5 * 1e-4);
    XSFLIB_CHECK(LargestFrameError(up, up_expected, 1, 1000, 47000) <= 0.5 * 3e-3);
    XSFLIB_CHECK(LargestFrameError(down, down_expected, 0, 1000, 31728) <= 0.5 * 3e-3);
    XSFLIB_CHECK(LargestFrameError(down, down_expected, 1, 1000, 31728) <= 0.5 * 1e-4);
    XSFLIB_CHECK(LargestFrameError(high, std::vector<float>(high.size(), 0.0f), 0, 1000, 31728) <= 0.5 * 1e-4);
    XSFLIB_CHECK(LargestFrameError(high, std::vector<float>(high.size(), 0.0f), 1, 1000, 31728) <= 0.5 * 1e-4);

    const std::vector<float> same = Frames(1000.0, 2000.0, 48000, 100);

    XSFLIB_CHECK(ResampleFrames(same, 2, 48000, 48000) == same);
    XSFLIB_CHECK(ResampleFrames({}, 2, 48000, 32728).empty());
    XSFLIB_CHECK(Refused([&] { ResampleFrames(same, 2, 0, 48000); }));
    XSFLIB_CHECK(Refused([&] { ResampleFrames(same, 2, 48000, 0); }));
    XSFLIB_CHECK(Refused([&] { ResampleFrames(same, 0, 48000, 32728); }));
    XSFLIB_CHECK(Refused([&] { ResampleFrames(same, 3, 48000, 32728); }));
}

} // namespace

int main()
{
    TestSines();
    TestLoop();
    TestHigherRate();
    TestRefusals();
    TestFrames();

    return xsflib::testing::Summary("resampler");
}
