// SPDX-License-Identifier: MIT

#include "song/audibility.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <stdexcept>
#include <utility>

namespace hokuto::song
{
namespace
{

constexpr double kHighPassHz = 20.0;
constexpr double kCountRangeDb = 50.0;
constexpr double kCountFloorDbfs = -70.0;
constexpr std::size_t kSpanWindows = 20;
constexpr std::size_t kSpanHop = 10;
constexpr std::size_t kSpanCounted = 10;
constexpr std::size_t kRunWindows = 4;
constexpr double kSilentDb = -200.0;

// The search for the lag: windows of 1 ms over the first 20 s, within 100 ms either way. Levels more than 60 dB under
// the loudest window count as that floor. A curve whose levels spread by less than 1 dB gives no lag: a steady sound
// lines up at any lag. Peaks of the correlation within 0.002 of the best tie, and the one nearest 0 wins.
constexpr double kLagWindowSeconds = 0.001;
constexpr double kLagSeconds = 20.0;
constexpr double kMostLagSeconds = 0.1;
constexpr double kLagRangeDb = 60.0;
constexpr double kFlatDb = 1.0;
constexpr double kTie = 0.002;

double ToDb(double power)
{
    return power > 0.0 ? std::max(10.0 * std::log10(power), kSilentDb) : kSilentDb;
}

// The mean power of each whole window of `size` frames of `samples`, interleaved stereo, after the high-pass. The
// windows start `shift` frames into the samples. A negative shift puts silence before them. `most` limits the windows.
std::vector<double> WindowPowers(std::span<const float> samples, int rate, std::size_t size, int64_t shift,
                                 std::size_t most)
{
    // The bilinear transform of the analog Butterworth high-pass, with the cutoff prewarped.
    const double k = std::tan(std::numbers::pi * kHighPassHz / rate);
    const double norm = 1.0 / (1.0 + std::numbers::sqrt2 * k + k * k);
    const double b0 = norm;
    const double b1 = -2.0 * norm;
    const double b2 = norm;
    const double a1 = 2.0 * (k * k - 1.0) * norm;
    const double a2 = (1.0 - std::numbers::sqrt2 * k + k * k) * norm;

    // Each channel runs through its filter. The power of each frame adds to the window that its place falls in.
    const auto frames = static_cast<int64_t>(samples.size() / 2);
    const int64_t placed = frames - shift;
    const std::size_t count = placed > 0 ? std::min(static_cast<std::size_t>(placed) / size, most) : 0;
    std::vector<double> powers(count, 0.0);
    std::array<double, 2> x1{}, x2{}, y1{}, y2{};
    for (int64_t frame = 0; frame < frames; frame++)
    {
        const int64_t place = frame - shift;
        const bool counts = place >= 0 && static_cast<std::size_t>(place) / size < count;
        if (!counts && place >= 0)
        {
            break;
        }

        for (std::size_t channel = 0; channel < 2; channel++)
        {
            const double x = samples[static_cast<std::size_t>(frame) * 2 + channel];
            const double y = b0 * x + b1 * x1[channel] + b2 * x2[channel] - a1 * y1[channel] - a2 * y2[channel];
            x2[channel] = x1[channel];
            x1[channel] = x;
            y2[channel] = y1[channel];
            y1[channel] = y;
            if (counts)
            {
                powers[static_cast<std::size_t>(place) / size] += y * y;
            }
        }
    }
    for (double& power : powers)
    {
        power /= static_cast<double>(size * 2);
    }

    return powers;
}

std::vector<double> Levels(const std::vector<double>& powers)
{
    std::vector<double> levels(powers.size());
    std::transform(powers.begin(), powers.end(), levels.begin(), ToDb);

    return levels;
}

// The median of `values`. An even count takes the mean of the two middle values. numpy's median does the same.
double Median(std::vector<double> values)
{
    const std::size_t half = values.size() / 2;
    std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(half), values.end());
    const double upper = values[half];
    if (values.size() % 2 != 0)
    {
        return upper;
    }

    return (upper + *std::max_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(half))) / 2.0;
}

// Whether `levels` spread by less than 1 dB around their mean, or hold fewer than two windows.
bool Flat(const std::vector<double>& levels)
{
    if (levels.size() < 2)
    {
        return true;
    }

    double sum = 0.0, sum_squares = 0.0;
    for (const double level : levels)
    {
        sum += level;
        sum_squares += level * level;
    }

    const double count = static_cast<double>(levels.size());
    return sum_squares - sum * sum / count < kFlatDb * kFlatDb * count;
}

// The lag in frames by which `render` comes after `reference`: the shift of the render's levels in windows of 1 ms that
// correlates best with the reference's, or 0 when either curve is flat.
int64_t LagFrames(std::span<const float> reference, std::span<const float> render, int rate)
{
    const auto size = static_cast<std::size_t>(std::lround(kLagWindowSeconds * rate));
    const auto windows = static_cast<std::size_t>(std::lround(kLagSeconds / kLagWindowSeconds));
    const auto most = static_cast<int64_t>(std::lround(kMostLagSeconds / kLagWindowSeconds));
    std::vector<double> a = Levels(WindowPowers(reference, rate, size, 0, windows));
    std::vector<double> b = Levels(WindowPowers(render, rate, size, 0, windows));
    const double top = std::max(a.empty() ? kSilentDb : *std::max_element(a.begin(), a.end()),
                                b.empty() ? kSilentDb : *std::max_element(b.begin(), b.end()));
    if (top <= kSilentDb)
    {
        return 0;
    }
    for (std::vector<double>* levels : {&a, &b})
    {
        for (double& level : *levels)
        {
            level = std::max(level, top - kLagRangeDb);
        }
    }
    if (Flat(a) || Flat(b))
    {
        return 0;
    }

    // The correlation of the two curves at each lag from -100 ms to 100 ms, over the windows of either. A curve holds
    // the floor before its start, and after its end when its sound ends within the 20 s. A lag that moves a sound past
    // the other's start or end therefore pairs the sound with silence. Past the 20 s, a pair counts for nothing.
    const double floor = top - kLagRangeDb;
    const auto size_a = static_cast<int64_t>(a.size());
    const auto size_b = static_cast<int64_t>(b.size());
    const bool a_ends = a.size() < windows;
    const bool b_ends = b.size() < windows;
    std::vector<std::pair<int64_t, double>> correlations;
    for (int64_t lag = -most; lag <= most; lag++)
    {
        double sum_a = 0.0, sum_b = 0.0, sum_aa = 0.0, sum_bb = 0.0, sum_ab = 0.0;
        std::size_t n = 0;
        for (int64_t i = std::min<int64_t>(0, -lag); i < std::max(size_a, size_b - lag); i++)
        {
            const int64_t j = i + lag;
            if ((i >= size_a && !a_ends) || (j >= size_b && !b_ends))
            {
                continue;
            }

            const double x = i >= 0 && i < size_a ? a[static_cast<std::size_t>(i)] : floor;
            const double y = j >= 0 && j < size_b ? b[static_cast<std::size_t>(j)] : floor;
            sum_a += x;
            sum_b += y;
            sum_aa += x * x;
            sum_bb += y * y;
            sum_ab += x * y;
            n++;
        }
        if (n < 2)
        {
            continue;
        }

        const double count = static_cast<double>(n);
        const double spread_a = std::max(sum_aa - sum_a * sum_a / count, 0.0);
        const double spread_b = std::max(sum_bb - sum_b * sum_b / count, 0.0);
        if (spread_a <= 0.0 || spread_b <= 0.0)
        {
            continue;
        }

        correlations.emplace_back(lag, (sum_ab - sum_a * sum_b / count) / std::sqrt(spread_a * spread_b));
    }

    // The peaks, the ends included. Among those within the tie of the best, the lag nearest 0 wins.
    std::vector<std::pair<int64_t, double>> peaks;
    for (std::size_t i = 0; i < correlations.size(); i++)
    {
        const double correlation = correlations[i].second;
        if ((i == 0 || correlation >= correlations[i - 1].second) &&
            (i + 1 == correlations.size() || correlation >= correlations[i + 1].second))
        {
            peaks.push_back(correlations[i]);
        }
    }
    double best_correlation = -2.0;
    for (const auto& [lag, correlation] : peaks)
    {
        best_correlation = std::max(best_correlation, correlation);
    }
    int64_t best = 0;
    bool found = false;
    for (const auto& [lag, correlation] : peaks)
    {
        if (correlation >= best_correlation - kTie && (!found || std::abs(lag) < std::abs(best)))
        {
            best = lag;
            found = true;
        }
    }

    return best * static_cast<int64_t>(size);
}

// The start of each span of windows: every kSpanHop windows, and one that ends at the last window.
std::vector<std::size_t> SpanStarts(std::size_t count)
{
    if (count <= kSpanWindows)
    {
        return {0};
    }

    std::vector<std::size_t> starts;
    for (std::size_t start = 0; start + kSpanWindows <= count; start += kSpanHop)
    {
        starts.push_back(start);
    }
    if (starts.back() + kSpanWindows < count)
    {
        starts.push_back(count - kSpanWindows);
    }

    return starts;
}

} // namespace

LevelComparison CompareLevels(std::span<const float> reference, std::span<const float> render, int rate)
{
    if (rate < 1000)
    {
        throw std::invalid_argument("CompareLevels requires a rate of 1,000 Hz or more");
    }

    // The levels of the windows, with the render lined up.
    LevelComparison comparison;
    const int64_t lag = LagFrames(reference, render, rate);
    comparison.lag = static_cast<double>(lag) / rate;
    const auto size = static_cast<std::size_t>(std::lround(kWindowSeconds * rate));
    const std::vector<double> b = Levels(WindowPowers(reference, rate, size, 0, SIZE_MAX));
    const std::vector<double> render_levels = Levels(WindowPowers(render, rate, size, lag, SIZE_MAX));
    const std::size_t count = std::min(b.size(), render_levels.size());
    if (count == 0)
    {
        return comparison;
    }

    // The gain: the median difference over the windows that count by the reference alone. A render that stays below the
    // floor throughout, and more than the count's range below the reference, takes no gain. Its silence is then the
    // finding, and not a gain of some 200 dB. A quiet render a few dB under a quiet reference still takes its gain.
    const double reference_top = *std::max_element(b.begin(), b.begin() + static_cast<std::ptrdiff_t>(count));
    const double render_top =
        *std::max_element(render_levels.begin(), render_levels.begin() + static_cast<std::ptrdiff_t>(count));
    std::vector<double> differences;
    for (std::size_t i = 0; i < count; i++)
    {
        if (b[i] >= reference_top - kCountRangeDb && b[i] > kCountFloorDbfs)
        {
            differences.push_back(render_levels[i] - b[i]);
        }
    }
    const bool silent = render_top <= kCountFloorDbfs && render_top < reference_top - kCountRangeDb;
    comparison.gain_db = differences.empty() || silent ? 0.0 : Median(differences);

    // Each window's difference, with both levels held at the floor.
    std::vector<double> a(count);
    for (std::size_t i = 0; i < count; i++)
    {
        a[i] = render_levels[i] - comparison.gain_db;
    }
    const double top = std::max(*std::max_element(a.begin(), a.end()),
                                *std::max_element(b.begin(), b.begin() + static_cast<std::ptrdiff_t>(count)));
    const double floor = top - kCountRangeDb;
    std::vector<bool> counted(count);
    std::vector<double> d(count);
    for (std::size_t i = 0; i < count; i++)
    {
        const double louder = std::max(a[i], b[i]);
        counted[i] = louder >= floor && louder > kCountFloorDbfs;
        d[i] = std::max(a[i], floor) - std::max(b[i], floor);
        if (!counted[i])
        {
            continue;
        }

        // The first window furthest off is the worst.
        const double off = std::abs(d[i]);
        comparison.windows++;
        (off < 1.0 ? comparison.within_1db : off <= 3.0 ? comparison.from_1_to_3db : comparison.beyond_3db)++;
        if (comparison.windows == 1 || off > std::abs(comparison.worst_db))
        {
            comparison.worst_db = d[i];
            comparison.worst_at = static_cast<double>(i) * kWindowSeconds;
        }
    }

    // L1: the spans whose median passes 1 dB. Spans that overlap with the same sign make one place, with the median
    // furthest off.
    std::vector<LevelPlace> places;
    for (const std::size_t start : SpanStarts(count))
    {
        const std::size_t end = std::min(start + kSpanWindows, count);
        std::vector<double> span;
        for (std::size_t i = start; i < end; i++)
        {
            if (counted[i])
            {
                span.push_back(d[i]);
            }
        }
        if (span.size() < kSpanCounted)
        {
            continue;
        }

        const double median = Median(std::move(span));
        if (std::abs(median) <= kSpanFindingDb)
        {
            continue;
        }

        const double begin = static_cast<double>(start) * kWindowSeconds;
        const double seconds = static_cast<double>(end - start) * kWindowSeconds;
        LevelPlace* last = places.empty() ? nullptr : &places.back();
        if (last != nullptr && begin <= last->start + last->seconds && (median > 0.0) == (last->db > 0.0))
        {
            last->seconds = begin + seconds - last->start;
            last->db = std::abs(median) > std::abs(last->db) ? median : last->db;
            continue;
        }

        places.push_back({begin, seconds, median, false});
    }

    // L2: the runs of counted windows that lie more than 6 dB off, by their mean.
    std::size_t run_start = 0;
    bool in_run = false;
    for (std::size_t i = 0; i <= count; i++)
    {
        const bool off = i < count && counted[i] && std::abs(d[i]) > kRunFindingDb;
        if (off && !in_run)
        {
            run_start = i;
            in_run = true;
        }
        else if (!off && in_run)
        {
            in_run = false;
            if (i - run_start < kRunWindows)
            {
                continue;
            }

            double sum = 0.0;
            for (std::size_t j = run_start; j < i; j++)
            {
                sum += d[j];
            }
            places.push_back({static_cast<double>(run_start) * kWindowSeconds,
                              static_cast<double>(i - run_start) * kWindowSeconds,
                              sum / static_cast<double>(i - run_start), true});
        }
    }

    std::stable_sort(places.begin(), places.end(),
                     [](const LevelPlace& x, const LevelPlace& y) { return x.start < y.start; });
    comparison.places = std::move(places);

    return comparison;
}

std::vector<LevelPlace> WorstPlaces(const LevelComparison& comparison, double db, std::size_t most)
{
    std::vector<LevelPlace> worst;
    for (const LevelPlace& place : comparison.places)
    {
        if (std::abs(place.db) > db || (place.run && db <= kRunFindingDb))
        {
            worst.push_back(place);
        }
    }

    std::stable_sort(worst.begin(), worst.end(),
                     [](const LevelPlace& x, const LevelPlace& y) { return std::abs(x.db) > std::abs(y.db); });
    if (worst.size() > most)
    {
        worst.resize(most);
    }

    return worst;
}

} // namespace hokuto::song
