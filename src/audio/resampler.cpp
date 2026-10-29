// SPDX-License-Identifier: MIT

#include "audio/resampler.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <vector>

namespace hokuto::audio
{
namespace
{

constexpr double kPi = 3.14159265358979323846;

// The filter passes up to 90 % of the lower rate's Nyquist frequency and stops from 100 %. Its cutoff lies between.
// Kaiser's window with this beta holds the stopband about 90 dB down. The taps follow from the transition's width
// (Kaiser's formula for 90 dB).
constexpr double kPass = 0.9;
constexpr double kBeta = 8.96;
constexpr double kAttenuation = 90.0;

// The kernel's table holds this many points for each old sample, between which it interpolates.
constexpr std::size_t kPhases = 1024;

// sin(π·x). Built from additions, multiplications and divisions. Every platform then computes the same value.
double SinPi(double x)
{
    // x in [-1, 1), then in [-1/2, 1/2] by sin(π·(1 − x)) = sin(π·x).
    x -= 2.0 * std::floor((x + 1.0) / 2.0);
    if (x > 0.5)
    {
        x = 1.0 - x;
    }
    else if (x < -0.5)
    {
        x = -1.0 - x;
    }

    // Taylor's series of sin up to the 23rd power. At π/2, the next term is below 10^-19.
    const double y = kPi * x;
    const double y2 = y * y;
    double term = y;
    double result = 0.0;
    for (int n = 1; n <= 23; n += 2)
    {
        result += term;
        term = -term * y2 / static_cast<double>((n + 1) * (n + 2));
    }

    return result;
}

// The modified Bessel function of the first kind and order 0, from its series.
double BesselI0(double x)
{
    const double half = x / 2.0;
    double term = 1.0;
    double sum = 1.0;
    for (int k = 1; k < 64; k++)
    {
        term *= half / static_cast<double>(k);
        const double squared = term * term;
        sum += squared;
        if (squared < sum * 1e-18)
        {
            break;
        }
    }

    return sum;
}

// The kernel: a sinc with its cutoff at `cutoff` cycles an old sample, in Kaiser's window `half` old samples to each
// side. `table[i]` holds it at i / kPhases old samples from its middle, up to `half`, and 0 past it.
std::vector<double> KernelTable(double cutoff, std::size_t half)
{
    std::vector<double> table(half * kPhases + 2, 0.0);
    const double window = BesselI0(kBeta);
    for (std::size_t i = 0; i <= half * kPhases; i++)
    {
        const double t = static_cast<double>(i) / static_cast<double>(kPhases);
        const double u = 2.0 * cutoff * t;
        const double sinc = i == 0 ? 1.0 : SinPi(u) / (kPi * u);
        const double r = t / static_cast<double>(half);
        table[i] = 2.0 * cutoff * sinc * BesselI0(kBeta * std::sqrt(std::max(0.0, 1.0 - r * r))) / window;
    }

    return table;
}

// The filter of a resample from `from` to `to` samples a second: the kernel's table, and the old samples that it reads
// on each side of a new sample's time. The transition runs from 90 % to 100 % of the lower rate's Nyquist frequency.
struct Filter
{
    std::vector<double> table;
    std::size_t half = 0;
};

Filter FilterFor(uint32_t from, uint32_t to)
{
    const double ratio = std::min(static_cast<double>(to) / static_cast<double>(from), 1.0);
    const double cutoff = (kPass + 1.0) / 4.0 * ratio;
    const double width = (1.0 - kPass) / 2.0 * ratio;
    const auto half = static_cast<std::size_t>(std::ceil((kAttenuation - 7.95) / (14.36 * width) / 2.0)) + 1;
    return {KernelTable(cutoff, half), half};
}

} // namespace

std::size_t ResampledPlace(std::size_t sample, uint32_t from, uint32_t to)
{
    return static_cast<std::size_t>((static_cast<uint64_t>(sample) * to * 2 + from) / (uint64_t{from} * 2));
}

std::vector<int16_t> Resample(std::span<const int16_t> samples, uint32_t from, uint32_t to,
                              std::optional<std::size_t> loop_start)
{
    if (from == 0 || to == 0 || to == from)
    {
        throw std::invalid_argument("A resample goes to another rate.");
    }
    if (samples.empty())
    {
        throw std::invalid_argument("A resample requires samples.");
    }
    if (loop_start && *loop_start >= samples.size())
    {
        throw std::invalid_argument("The loop starts past the samples.");
    }

    // The new samples: the intro and the loop, each a whole number of them.
    const std::size_t count = samples.size();
    const std::size_t start = loop_start ? ResampledPlace(*loop_start, from, to) : 0;
    const std::size_t loop = loop_start ? std::max<std::size_t>(ResampledPlace(count - *loop_start, from, to), 1) : 0;
    const std::size_t total = loop_start ? start + loop : std::max<std::size_t>(ResampledPlace(count, from, to), 1);

    // The time of new sample k in old samples. Each part stretches to fit its new samples.
    const auto time_of = [&](std::size_t k)
    {
        if (!loop_start)
        {
            return static_cast<double>(k) * from / to;
        }
        if (k < start)
        {
            return static_cast<double>(k) * static_cast<double>(*loop_start) / static_cast<double>(start);
        }

        return static_cast<double>(*loop_start) +
               static_cast<double>(k - start) * static_cast<double>(count - *loop_start) / static_cast<double>(loop);
    };

    // The filter. The transition runs from 90 % to 100 % of the lower rate's Nyquist frequency.
    const Filter filter = FilterFor(from, to);
    const std::size_t half = filter.half;
    const std::vector<double>& table = filter.table;

    // The old samples with what the filter reads around them: silence before, and the loop again or silence after.
    const std::size_t pad = half + 1;
    std::vector<double> padded(pad + count + pad, 0.0);
    for (std::size_t i = 0; i < count; i++)
    {
        padded[pad + i] = samples[i];
    }
    for (std::size_t i = 0; loop_start && i < pad; i++)
    {
        padded[pad + count + i] = samples[*loop_start + i % (count - *loop_start)];
    }

    // Each new sample adds up the old samples on either side of its time, each by the kernel at its distance. The
    // distances of the taps on one side share the fraction of the time. The table's place of each tap therefore follows
    // from the first.
    std::vector<int16_t> resampled(total);
    for (std::size_t k = 0; k < total; k++)
    {
        const double t = time_of(k);
        const double whole = std::floor(t);
        const double phase = (t - whole) * static_cast<double>(kPhases);
        const auto first = static_cast<std::size_t>(phase);
        const double fraction = phase - static_cast<double>(first);
        const std::size_t at = pad + static_cast<std::size_t>(whole);
        double sum = 0.0;
        for (std::size_t m = 0; m < half; m++)
        {
            // At distance m + the time's fraction, before the time.
            const std::size_t i = m * kPhases + first;
            sum += padded[at - m] * (table[i] + (table[i + 1] - table[i]) * fraction);
        }
        for (std::size_t m = 0; m < half; m++)
        {
            // At distance m + 1 − the time's fraction, after it.
            const double place = static_cast<double>((m + 1) * kPhases) - phase;
            const auto i = static_cast<std::size_t>(place);
            sum += padded[at + 1 + m] * (table[i] + (table[i + 1] - table[i]) * (place - static_cast<double>(i)));
        }
        resampled[k] = static_cast<int16_t>(std::clamp(std::lround(sum), -32768L, 32767L));
    }

    return resampled;
}

std::vector<float> ResampleFrames(std::span<const float> samples, std::size_t channels, uint32_t from, uint32_t to)
{
    if (from == 0 || to == 0 || channels == 0)
    {
        throw std::invalid_argument("A resample requires two rates and a channel.");
    }
    if (samples.size() % channels != 0)
    {
        throw std::invalid_argument("The samples don't fill their frames.");
    }
    const std::size_t frames = samples.size() / channels;
    if (from == to || frames == 0)
    {
        return {samples.begin(), samples.end()};
    }

    const Filter filter = FilterFor(from, to);
    const std::size_t half = filter.half;
    const std::vector<double>& table = filter.table;
    const std::size_t total = ResampledPlace(frames, from, to);
    std::vector<float> resampled(total * channels);
    std::vector<double> weights(2 * half);
    for (std::size_t k = 0; k < total; k++)
    {
        // The kernel at each old frame around the new frame's time: the frames before the time first, then the frames
        // after it. Resample finds the kernel in this order too.
        const double t = static_cast<double>(k) * from / to;
        const double whole = std::floor(t);
        const double phase = (t - whole) * static_cast<double>(kPhases);
        const auto first = static_cast<std::size_t>(phase);
        const double fraction = phase - static_cast<double>(first);
        for (std::size_t m = 0; m < half; m++)
        {
            const std::size_t i = m * kPhases + first;
            weights[m] = table[i] + (table[i + 1] - table[i]) * fraction;
            const double place = static_cast<double>((m + 1) * kPhases) - phase;
            const auto j = static_cast<std::size_t>(place);
            weights[half + m] = table[j] + (table[j + 1] - table[j]) * (place - static_cast<double>(j));
        }

        // Each channel adds up its frames by the kernel. Frames outside the samples are silent.
        const auto at = static_cast<std::size_t>(whole);
        for (std::size_t c = 0; c < channels; c++)
        {
            double sum = 0.0;
            for (std::size_t m = 0; m < half; m++)
            {
                if (m <= at && at - m < frames)
                {
                    sum += samples[(at - m) * channels + c] * weights[m];
                }
                if (at + 1 + m < frames)
                {
                    sum += samples[(at + 1 + m) * channels + c] * weights[half + m];
                }
            }
            resampled[k * channels + c] = static_cast<float>(sum);
        }
    }

    return resampled;
}

} // namespace hokuto::audio
