// SPDX-License-Identifier: MIT

// The fit of hokuto_check_players: the filter that maps the reference of a sound to the output of a player, and the
// comparison that judges the rest.

#include <numbers>

#include "check_players.h"

namespace hokuto::tools::players
{

namespace
{

// A windowed sinc that resamples the reference at a step: a table of the kernel at 256 phases of a sample. Above a step
// of 1, the console reads faster than the output's rate, and the cutoff drops to the output's Nyquist frequency. A
// `band` below 1 lowers the cutoff by that factor.
class Kernel
{
public:
    // The kernel for a step of `step`, with its cutoff lowered by `band`.
    explicit Kernel(double step, double band = 1.0)
        : scale_(std::min(1.0, 1.0 / step) * band),
          half_(static_cast<int>(std::ceil(8.0 / scale_))),
          width_(static_cast<std::size_t>(2 * half_))
    {
        taps_.resize((kPhases + 1) * width_);
        for (std::size_t phase = 0; phase <= kPhases; phase++)
        {
            const double fraction = static_cast<double>(phase) / kPhases;
            for (std::size_t k = 0; k < width_; k++)
            {
                const double distance = static_cast<double>(static_cast<int>(k) - half_ + 1) - fraction;
                taps_[phase * width_ + k] = static_cast<float>(Value(distance));
            }
        }
    }

    // The value of channel `channel` of `reference` at place `place`, in samples.
    float At(const Reference& reference, std::size_t channel, double place) const
    {
        const double whole = std::floor(place);
        const auto phase = static_cast<std::size_t>(std::lround((place - whole) * kPhases));
        const float* taps = &taps_[phase * width_];
        const long long first = static_cast<long long>(whole) - half_ + 1;
        float sum = 0.0f;
        for (std::size_t k = 0; k < width_; k++)
        {
            sum += taps[k] * reference.Sample(channel, first + static_cast<long long>(k));
        }

        return sum;
    }

private:
    static constexpr std::size_t kPhases = 256;

    // The kernel at `distance` samples: a sinc at the cutoff, in a Blackman window.
    double Value(double distance) const
    {
        const double reach = static_cast<double>(half_);
        if (std::abs(distance) >= reach)
        {
            return 0.0;
        }
        const double x = distance * scale_;
        const double sinc = x == 0.0 ? 1.0 : std::sin(std::numbers::pi * x) / (std::numbers::pi * x);
        const double window = 0.42 + 0.5 * std::cos(std::numbers::pi * distance / reach) +
                              0.08 * std::cos(2.0 * std::numbers::pi * distance / reach);
        return scale_ * sinc * window;
    }

    double scale_;
    int half_;
    std::size_t width_;
    std::vector<float> taps_;
};

// Solves `matrix` times x = `vector` for a small system, with a little damping for a channel that stays silent.
std::vector<double> Solve(std::vector<std::vector<double>> matrix, std::vector<double> vector)
{
    const std::size_t size = vector.size();
    double largest = 0.0;
    for (std::size_t i = 0; i < size; i++)
    {
        largest = std::max(largest, matrix[i][i]);
    }
    for (std::size_t i = 0; i < size; i++)
    {
        matrix[i][i] += largest * 1e-9 + 1e-12;
    }

    for (std::size_t i = 0; i < size; i++)
    {
        std::size_t pivot = i;
        for (std::size_t r = i + 1; r < size; r++)
        {
            if (std::abs(matrix[r][i]) > std::abs(matrix[pivot][i]))
            {
                pivot = r;
            }
        }
        std::swap(matrix[i], matrix[pivot]);
        std::swap(vector[i], vector[pivot]);
        for (std::size_t r = i + 1; r < size; r++)
        {
            const double factor = matrix[r][i] / matrix[i][i];
            for (std::size_t c = i; c < size; c++)
            {
                matrix[r][c] -= factor * matrix[i][c];
            }
            vector[r] -= factor * vector[i];
        }
    }

    std::vector<double> x(size);
    for (std::size_t i = size; i-- > 0;)
    {
        double sum = vector[i];
        for (std::size_t c = i + 1; c < size; c++)
        {
            sum -= matrix[i][c] * x[c];
        }
        x[i] = sum / matrix[i][i];
    }

    return x;
}

// The reference's channels at the output samples from `first` - `reach` up to `first` + `count` + `reach`, with the
// reference at place `place` at output sample `first`.
std::vector<std::vector<float>> Resampled(const Reference& reference, const Kernel& kernel, double place, double step,
                                          std::size_t count, int reach)
{
    std::vector<std::vector<float>> resampled(reference.Channels(), std::vector<float>(count + 2 * reach));
    for (std::size_t c = 0; c < reference.Channels(); c++)
    {
        for (std::size_t i = 0; i < resampled[c].size(); i++)
        {
            const double offset = static_cast<double>(static_cast<long long>(i) - reach);
            resampled[c][i] = kernel.At(reference, c, place + step * offset);
        }
    }

    return resampled;
}

// A low-pass filter at kGainBand for an output at `rate`: a Butterworth filter of the fourth order, as two biquads.
class LowPass
{
public:
    // The filter for an output at `rate` Hz, at rest.
    explicit LowPass(double rate)
    {
        const double k = std::tan(std::numbers::pi * kGainBand / rate);
        for (std::size_t i = 0; i < 2; i++)
        {
            // The poles of the fourth order's two pairs.
            const double q = 1.0 / (2.0 * std::cos(std::numbers::pi * (2.0 * static_cast<double>(i) + 1.0) / 8.0));
            const double norm = 1.0 / (1.0 + k / q + k * k);
            Section& section = sections_[i];
            section.b0 = k * k * norm;
            section.b1 = 2.0 * section.b0;
            section.b2 = section.b0;
            section.a1 = 2.0 * (k * k - 1.0) * norm;
            section.a2 = (1.0 - k / q + k * k) * norm;
        }
    }

    // The filter's output for the next sample `input`.
    float Next(float input)
    {
        double x = input;
        for (Section& section : sections_)
        {
            const double y = section.b0 * x + section.s1;
            section.s1 = section.b1 * x - section.a1 * y + section.s2;
            section.s2 = section.b2 * x - section.a2 * y;
            x = y;
        }

        return static_cast<float>(x);
    }

private:
    struct Section
    {
        double b0 = 0.0;
        double b1 = 0.0;
        double b2 = 0.0;
        double a1 = 0.0;
        double a2 = 0.0;
        double s1 = 0.0;
        double s2 = 0.0;
    };

    std::array<Section, 2> sections_;
};

// A least-squares fit of each side of the output to the reference's channels, each through a filter of 2 × `taps` + 1
// taps. A fit without taps fits a gain.
class Fit
{
public:
    // A fit of `channels` channels without samples, each through 2 × `taps` + 1 taps.
    Fit(std::size_t channels, int taps)
        : channels_(channels),
          taps_(taps),
          size_(channels * static_cast<std::size_t>(2 * taps + 1)),
          gram_(size_, std::vector<double>(size_)),
          cross_(2, std::vector<double>(size_))
    {
    }

    // Adds `count` output samples from `first` on. `resampled` holds the reference's channels from `reach` output
    // samples before `first` (Resampled), with `reach` at least the taps.
    void Add(const std::vector<std::vector<float>>& resampled, int reach, std::span<const float> output,
             std::size_t first, std::size_t count)
    {
        const auto width = static_cast<std::size_t>(2 * taps_ + 1);
        const auto skip = static_cast<std::size_t>(reach - taps_);
        std::vector<double> x(size_);
        for (std::size_t n = 0; n < count; n++)
        {
            for (std::size_t c = 0; c < channels_; c++)
            {
                for (std::size_t k = 0; k < width; k++)
                {
                    x[c * width + k] = resampled[c][n + skip + k];
                }
                const double middle = resampled[c][n + static_cast<std::size_t>(reach)];
                reference_energy_ += middle * middle;
            }
            for (std::size_t a = 0; a < size_; a++)
            {
                const double xa = x[a];
                std::vector<double>& row = gram_[a];
                for (std::size_t b = a; b < size_; b++)
                {
                    row[b] += xa * x[b];
                }
            }
            for (std::size_t side = 0; side < 2; side++)
            {
                const double y = output[(first + n) * 2 + side];
                energy_ += y * y;
                for (std::size_t a = 0; a < size_; a++)
                {
                    cross_[side][a] += x[a] * y;
                }
            }
        }
    }

    // The gain of each channel to each side at 0 Hz, and the energy of the output that the fit leaves.
    std::pair<std::vector<std::array<double, 2>>, double> Solved() const
    {
        std::vector<std::vector<double>> full = gram_;
        for (std::size_t a = 0; a < size_; a++)
        {
            for (std::size_t b = 0; b < a; b++)
            {
                full[a][b] = full[b][a];
            }
        }
        const auto width = static_cast<std::size_t>(2 * taps_ + 1);
        std::vector<std::array<double, 2>> gains(channels_);
        double left = energy_;
        for (std::size_t side = 0; side < 2; side++)
        {
            const std::vector<double> g = Solve(full, cross_[side]);
            for (std::size_t a = 0; a < size_; a++)
            {
                gains[a / width][side] += g[a];
                left -= g[a] * cross_[side][a];
            }
        }

        return {gains, std::max(left, 0.0)};
    }

    // Writes what the solved fit leaves of `count` output samples from `first` on into `residual`, interleaved as the
    // output, with `resampled` as for Add.
    void Residual(const std::vector<std::vector<float>>& resampled, int reach, std::span<const float> output,
                  std::size_t first, std::size_t count, std::vector<float>& residual) const
    {
        std::vector<std::vector<double>> full = gram_;
        for (std::size_t a = 0; a < size_; a++)
        {
            for (std::size_t b = 0; b < a; b++)
            {
                full[a][b] = full[b][a];
            }
        }
        const std::array<std::vector<double>, 2> g = {Solve(full, cross_[0]), Solve(full, cross_[1])};
        const auto width = static_cast<std::size_t>(2 * taps_ + 1);
        const auto skip = static_cast<std::size_t>(reach - taps_);
        for (std::size_t n = 0; n < count; n++)
        {
            for (std::size_t side = 0; side < 2; side++)
            {
                double y = output[(first + n) * 2 + side];
                for (std::size_t c = 0; c < channels_; c++)
                {
                    for (std::size_t k = 0; k < width; k++)
                    {
                        y -= g[side][c * width + k] * resampled[c][n + skip + k];
                    }
                }
                residual[(first + n) * 2 + side] = static_cast<float>(y);
            }
        }
    }

    // The energy of both sides of the output samples that Add took.
    double Energy() const
    {
        return energy_;
    }

    // The energy of the reference's channels at the output samples that Add took, from their middle taps alone.
    double ReferenceEnergy() const
    {
        return reference_energy_;
    }

private:
    std::size_t channels_;
    int taps_;
    std::size_t size_;
    std::vector<std::vector<double>> gram_;
    std::vector<std::vector<double>> cross_;
    double energy_ = 0.0;
    double reference_energy_ = 0.0;
};

// The share of a window's output that the best fit of the reference from `place` on leaves, or nothing when either
// stays silent there.
std::optional<double> LeftShare(const Reference& reference, const Kernel& kernel, double place, double step,
                                std::span<const float> output, std::size_t first, std::size_t count)
{
    Fit fit(reference.Channels(), 0);
    fit.Add(Resampled(reference, kernel, place, step, count, 0), 0, output, first, count);
    if (fit.Energy() < 1e-12 || fit.ReferenceEnergy() < 1e-12)
    {
        return std::nullopt;
    }

    return fit.Solved().second / fit.Energy();
}

// The place within 4 samples of `centre` where the reference fits the window best, in steps of 1/2 sample, then 1/16,
// 1/128 and 1/1024. Nothing when the window stays silent.
std::optional<double> BestPlace(const Reference& reference, const Kernel& kernel, double centre, double step,
                                std::span<const float> output, std::size_t first, std::size_t count)
{
    std::optional<double> least = LeftShare(reference, kernel, centre, step, output, first, count);
    if (!least)
    {
        return std::nullopt;
    }
    double best = centre;
    for (const double width : {0.5, 1.0 / 16.0, 1.0 / 128.0, 1.0 / 1024.0})
    {
        const double around = best;
        for (int k = -8; k <= 8; k++)
        {
            if (k == 0)
            {
                continue;
            }

            const double candidate = around + width * k;
            const std::optional<double> left = LeftShare(reference, kernel, candidate, step, output, first, count);
            if (left && *left < *least)
            {
                least = left;
                best = candidate;
            }
        }
    }

    return best;
}

} // namespace

bool Imaged(const Comparison& comparison)
{
    return comparison.step > 1.0 && comparison.band_share < kLeastBandShare;
}

double Judged(const Comparison& comparison)
{
    if (!comparison.band_measured)
    {
        return comparison.filtered_snr;
    }

    return Imaged(comparison) ? std::max(comparison.band_snr, comparison.filtered_snr) : comparison.band_snr;
}

bool Passes(const Comparison& comparison)
{
    return Judged(comparison) >= kLeastSnr || comparison.residual_level <= kFloorDb;
}

std::optional<std::size_t> Onset(std::span<const float> output)
{
    float peak = 0.0f;
    for (const float sample : output)
    {
        peak = std::max(peak, std::abs(sample));
    }
    if (peak == 0.0f)
    {
        return std::nullopt;
    }
    for (std::size_t i = 0; i < output.size(); i++)
    {
        if (std::abs(output[i]) >= peak / 256.0f)
        {
            return i / 2;
        }
    }

    return std::nullopt;
}

namespace
{

// The lag, from `lowest` to `highest` output samples, at which a gain times the sum of the channels of `reference`, in
// `band`, fits `sides`, the sum of the output's sides, best over its first `opening` samples. `step` is the reference
// samples for each output sample. The gain has to be positive. Nothing when no lag fits.
std::optional<std::size_t> BestLag(const Reference& reference, double step, const std::vector<double>& sides,
                                   std::size_t opening, const Kernel& band, std::size_t lowest, std::size_t highest)
{
    std::vector<double> channels(opening);
    std::vector<double> energies(opening + 1); // the energy of `channels` before each sample
    for (std::size_t n = 0; n < opening; n++)
    {
        for (std::size_t c = 0; c < reference.Channels(); c++)
        {
            channels[n] += band.At(reference, c, step * static_cast<double>(n));
        }
        energies[n + 1] = energies[n] + channels[n] * channels[n];
    }
    std::optional<std::size_t> found;
    double best = 0.0;
    for (std::size_t lag = lowest; lag <= highest && lag < opening; lag++)
    {
        double product = 0.0;
        for (std::size_t n = lag; n < opening; n++)
        {
            product += sides[n] * channels[n - lag];
        }
        const double energy = energies[opening - lag];
        const double score = product > 0.0 && energy > 0.0 ? product * product / energy : 0.0;
        if (score > best)
        {
            best = score;
            found = lag;
        }
    }

    return found;
}

// The step from windows of `output` ever further on from `anchor`, whose place in `reference` is `anchor_place`,
// searched in `search`'s band. Without `every_window`, only the windows where the reference fits well count, and
// `passed_over` says whether a window didn't. `step` is the first guess, and `total` the output's samples.
double SearchStep(const Reference& reference, const Kernel& search, std::span<const float> output, double step,
                  std::size_t total, std::size_t anchor, double anchor_place, bool every_window, bool& passed_over)
{
    double found = step;
    for (std::size_t distance = 12000; anchor + distance + kWindow <= total; distance *= 4)
    {
        const double predicted = anchor_place + found * static_cast<double>(distance);
        if (!reference.Loops() && predicted + step * kWindow > static_cast<double>(reference.Length()))
        {
            break;
        }
        const std::optional<double> place =
            BestPlace(reference, search, predicted, found, output, anchor + distance, kWindow);
        if (!place)
        {
            continue;
        }
        const std::optional<double> left =
            every_window ? std::nullopt
                         : LeftShare(reference, search, *place, found, output, anchor + distance, kWindow);
        if (every_window || (left && *left < kMostLeftShare))
        {
            found = (*place - anchor_place) / static_cast<double>(distance);
        }
        else
        {
            passed_over = true;
        }
    }

    return found;
}

// Fits `reference` to `output`, the output of `run`, at the place and the step of `found`. `kernel` resamples the
// reference a piece of `piece` output samples at a time, over the output's `total` samples. Sets the gains, the energy
// and the ratios of `found`, and returns the fit with the filter.
//
// One gain for each channel and side over the whole output, from the voice's second frame on, a piece at a
// time, and one filter. The gains that the tool reports come from a third fit, of the reference and the output
// below kGainBand. The consoles' resamplers pass every frequency alike there. A filter's gain at 0 Hz can take
// any value when the sound holds nothing low, such as a tone.
Fit FitWholeOutput(const Run& run, const Reference& reference, const Kernel& kernel, std::span<const float> output,
                   std::size_t total, std::size_t piece, Comparison& found)
{
    const int taps = reference.Channels() <= kMostFilteredChannels ? kTaps : 0;
    Fit plain(reference.Channels(), 0);
    Fit filtered(reference.Channels(), taps);
    Fit gain_fit(reference.Channels(), 0);
    std::vector<LowPass> reference_low(reference.Channels(), LowPass(run.rate));
    std::array<LowPass, 2> output_low{LowPass(run.rate), LowPass(run.rate)};
    for (std::size_t first = found.start + std::min(run.frame_samples, total - found.start); first < total;
         first += piece)
    {
        const std::size_t count = std::min(piece, total - first);
        const double place = found.place + found.step * static_cast<double>(first - found.start);
        const std::vector<std::vector<float>> resampled = Resampled(reference, kernel, place, found.step, count, kTaps);
        plain.Add(resampled, kTaps, output, first, count);
        filtered.Add(resampled, kTaps, output, first, count);
        std::vector<std::vector<float>> low_reference(reference.Channels(), std::vector<float>(count));
        std::vector<float> low_output(count * 2);
        for (std::size_t n = 0; n < count; n++)
        {
            for (std::size_t c = 0; c < reference.Channels(); c++)
            {
                low_reference[c][n] = reference_low[c].Next(resampled[c][n + kTaps]);
            }
            for (std::size_t side = 0; side < 2; side++)
            {
                low_output[n * 2 + side] = output_low[side].Next(output[(first + n) * 2 + side]);
            }
        }
        gain_fit.Add(low_reference, 0, low_output, 0, count);
    }

    const auto snr = [](const Fit& fit, double left)
    {
        return left <= 0.0 ? 999.0 : 10.0 * std::log10(fit.Energy() / left);
    };

    const double plain_left = plain.Solved().second;
    const double filtered_left = filtered.Solved().second;
    found.gains = gain_fit.Solved().first;
    found.low_share = plain.ReferenceEnergy() > 0.0 ? gain_fit.ReferenceEnergy() / plain.ReferenceEnergy() : 0.0;
    found.energy = plain.Energy();
    found.snr = snr(plain, plain_left);
    found.filtered_snr = snr(filtered, filtered_left);

    return filtered;
}

// The taps of a windowed low-pass filter with its cutoff at `band` Hz, at `rate` samples a second: `half` taps on
// each side of the centre.
std::vector<double> BandLowPass(double band, double rate, int half)
{
    std::vector<double> low(static_cast<std::size_t>(2 * half + 1));
    const double cutoff = band / rate;
    for (int k = -half; k <= half; k++)
    {
        const double x = 2.0 * cutoff * k;
        const double sinc = k == 0 ? 1.0 : std::sin(std::numbers::pi * x) / (std::numbers::pi * x);
        const double window = 0.42 + 0.5 * std::cos(std::numbers::pi * k / (half + 1)) +
                              0.08 * std::cos(2.0 * std::numbers::pi * k / (half + 1));
        low[static_cast<std::size_t>(k + half)] = 2.0 * cutoff * sinc * window;
    }

    return low;
}

// The ratio of the energies `kept` and `left`, in dB.
double Ratio(double kept, double left)
{
    return left <= 0.0 ? 999.0 : 10.0 * std::log10(kept / left);
}

// Prints the ratios in each piece of `piece` samples of `output`, the output of `run`, from `begin` to `total`:
// beyond the filter, from `residual`, and below `band` Hz, through `band_energies`. Also prints the output's
// level and peak in each piece.
template <typename BandEnergies>
void PrintPieces(const Run& run, std::span<const float> output, const std::vector<float>& residual, std::size_t begin,
                 std::size_t total, std::size_t piece, double band, const BandEnergies& band_energies)
{
    for (std::size_t first = begin; first < total; first += piece)
    {
        const std::size_t count = std::min(piece, total - first);
        double energy = 0.0;
        double left = 0.0;
        float peak = 0.0f;
        for (std::size_t i = first * 2; i < (first + count) * 2; i++)
        {
            energy += output[i] * output[i];
            left += residual[i] * residual[i];
            peak = std::max(peak, std::abs(output[i]));
        }
        const auto [piece_kept, piece_left] = band_energies(first, first + count);
        std::printf(
            "  from %.2f s: %.1f dB beyond the filter, %.1f dB below %.0f Hz, the output at %.1f dB, its "
            "peak at %.2f dB\n",
            static_cast<double>(first) / run.rate, Ratio(energy, left), Ratio(piece_kept, piece_left), band,
            energy > 0.0 ? 10.0 * std::log10(energy / static_cast<double>(2 * count)) : -999.0,
            peak > 0.0f ? 20.0 * std::log10(static_cast<double>(peak)) : -999.0);
    }
}

// The fits of `reference` to the `output` of `run` at step `fitted_step`, from the start of `result`. The place at the
// start comes from `anchor_place`, the place `skip` output samples after it. `kernel` resamples the reference a piece
// of `piece` output samples at a time, over the output's `total` samples, and `step` is the pitch's step. With `print`,
// prints the ratios in each piece. With `without_full_scale`, the residual counts as zero at each output sample at full
// scale.
Comparison FitAt(const Run& run, const Reference& reference, double step, const Kernel& kernel,
                 std::span<const float> output, std::size_t total, std::size_t piece, const Comparison& result,
                 double anchor_place, std::size_t skip, double fitted_step, bool print, bool without_full_scale)
{
    Comparison found = result;
    found.step = fitted_step;
    found.place = anchor_place - fitted_step * static_cast<double>(skip);
    const Fit filtered = FitWholeOutput(run, reference, kernel, output, total, piece, found);

    // What the filter leaves, and the ratio below the band: the output and the residual each through a low-pass
    // filter.
    const std::size_t begin = found.start + std::min(run.frame_samples, total - found.start);
    std::vector<float> residual(run.output.size());
    for (std::size_t first = begin; first < total; first += piece)
    {
        const std::size_t count = std::min(piece, total - first);
        const double place = found.place + found.step * static_cast<double>(first - found.start);
        filtered.Residual(Resampled(reference, kernel, place, found.step, count, kTaps), kTaps, output, first, count,
                          residual);
    }
    if (without_full_scale)
    {
        for (std::size_t i = 0; i < residual.size(); i++)
        {
            if (std::abs(output[i]) >= kFullScale)
            {
                residual[i] = 0.0f;
            }
        }
    }
    found.band = std::min(kBand, kBandShare * step * run.rate);
    const int half = std::min(kMostBandTaps, static_cast<int>(std::ceil(kBandTaps * kBand / found.band)));
    const std::vector<double> low = BandLowPass(found.band, run.rate, half);
    const auto reach = static_cast<std::size_t>(half);
    const auto band_energies = [&](std::size_t from, std::size_t to)
    {
        double kept = 0.0;
        double left = 0.0;
        for (std::size_t n = std::max(from, begin + reach); n + reach < std::min(to, total); n++)
        {
            for (std::size_t side = 0; side < 2; side++)
            {
                double y = 0.0;
                double r = 0.0;
                for (std::size_t k = 0; k < low.size(); k++)
                {
                    const std::size_t at = (n + k - reach) * 2 + side;
                    y += low[k] * output[at];
                    r += low[k] * residual[at];
                }
                kept += y * y;
                left += r * r;
            }
        }

        return std::pair(kept, left);
    };

    const auto [kept, band_left] = band_energies(begin, total);
    found.band_snr = Ratio(kept, band_left);
    found.band_share = found.energy > 0.0 ? kept / found.energy : 1.0;

    // Where the sound plays: up to the reference's end, or to the output's end for a reference that loops. The band
    // counts when it measures the filter's length of samples there. The residual's level is its mean power there:
    // in the band, or over the whole band for a sound too short for the band.
    std::size_t last = total;
    if (!reference.Loops())
    {
        const double end =
            static_cast<double>(found.start) + (static_cast<double>(reference.Length()) - found.place) / found.step;
        last = std::min(total, static_cast<std::size_t>(std::max(0.0, std::ceil(end))));
    }
    const std::size_t measured = last > begin + 2 * reach ? last - begin - 2 * reach : 0;
    found.band_measured = last >= begin + 2 * low.size();
    double left_power = 0.0;
    std::size_t left_count = 0;
    if (found.band_measured)
    {
        left_power = last == total ? band_left : band_energies(begin, last).second;
        left_count = measured;
    }
    else
    {
        for (std::size_t i = begin * 2; i < last * 2; i++)
        {
            left_power += double{residual[i]} * double{residual[i]};
        }
        left_count = last > begin ? last - begin : 0;
    }
    found.residual_level = left_count == 0     ? 0.0
                           : left_power <= 0.0 ? -999.0
                                               : 10.0 * std::log10(left_power / (2.0 * left_count));

    // In detail, the ratios in each piece, and the output's level and peak.
    if (print)
    {
        PrintPieces(run, output, residual, begin, total, piece, found.band, band_energies);
    }

    // A reference without a loop ends. The engine detects the end of the voices a frame or so later.
    if (!reference.Loops() && run.ended)
    {
        const double end =
            static_cast<double>(found.start) + (static_cast<double>(reference.Length()) - found.place) / found.step;
        const auto end_frame = static_cast<long long>(std::ceil(end / static_cast<double>(run.frame_samples)));
        found.end_frames = static_cast<long long>(run.frames) - end_frame;
    }

    return found;
}

} // namespace

Comparison Compare(const Run& run, const Reference& reference, double step, bool detail,
                   std::optional<std::size_t> start)
{
    Comparison result;
    const Kernel kernel(step);
    const std::size_t total = run.output.size() / 2;
    const auto piece = static_cast<std::size_t>(kPieceSeconds * run.rate);

    // The search for the start, the place and the step takes the lowest third of the band. The consoles' resamplers
    // delay every frequency there alike. Higher up, their delays vary with the frequency, and a window's place would
    // depend on the sounds in it.
    const Kernel search(step, 1.0 / 3.0);
    const std::span<const float> output(run.output);

    // The search begins where the reference first sounds: a wave can open with silence. `skip` output samples after the
    // start, the reference reaches `skip_place`, 64 samples before it sounds.
    const std::size_t sounds = reference.FirstSound().value_or(0);
    const auto skip = static_cast<std::size_t>(static_cast<double>(sounds > 64 ? sounds - 64 : 0) / step);
    const double skip_place = step * static_cast<double>(skip);

    // The start: the lag, up to kMostStart output samples, at which a gain times the sum of the reference's channels
    // fits the sum of the sides best, in the lower two thirds of the band. The fit spans the output from its first
    // sample to kOpening samples after the reference first sounds at the latest lag. A lag that puts the reference a
    // period early or late then leaves sounds in the output or the reference without a partner. The gain has to be
    // positive: a tone fits half a period off inverted. Within kRefine samples of that lag, the start is then the lag
    // that fits best in the lowest third of the band. Higher up, a burst of high sounds can fit best a few samples off.
    const auto lead = static_cast<std::size_t>(std::ceil(static_cast<double>(sounds) / step));
    const std::size_t opening = std::min(total, kMostStart + lead + kOpening);
    std::vector<double> sides(opening);
    for (std::size_t n = 0; n < opening; n++)
    {
        sides[n] = double{output[n * 2]} + double{output[n * 2 + 1]};
    }

    const auto best_lag = [&](const Kernel& band, std::size_t lowest, std::size_t highest)
    {
        return BestLag(reference, step, sides, opening, band, lowest, highest);
    };

    if (start)
    {
        result.start = *start;
    }
    else
    {
        const std::size_t coarse = best_lag(Kernel(step, 2.0 / 3.0), 0, kMostStart).value_or(0);
        result.start = best_lag(search, coarse > kRefine ? coarse - kRefine : 0, coarse + kRefine).value_or(coarse);
    }

    // The place there, within 4 samples of `skip_place`. Then the step: the place at windows ever further on. The step
    // so far predicts each window's place within a fraction of a sample. The first search takes only the windows where
    // the reference fits well (kMostLeftShare). A window in a wave's silent tail can set a wrong step otherwise. A
    // sound whose energy lies above the search's band fits well nowhere. The second search then runs too, with every
    // window.
    const std::size_t anchor = std::min(result.start + skip, total);
    const std::size_t anchor_count = std::min(kWindow, total - anchor);
    const double anchor_place =
        BestPlace(reference, search, skip_place, step, output, anchor, anchor_count).value_or(skip_place);
    bool passed_over = false;
    const auto search_step = [&](bool every_window)
    {
        return SearchStep(reference, search, output, step, total, anchor, anchor_place, every_window, passed_over);
    };
    std::vector<double> steps{search_step(false)};
    if (passed_over)
    {
        const double every = search_step(true);
        if (every != steps.front())
        {
            steps.push_back(every);
        }
    }

    // The fits at a step. The step gives the place at the start from the place at the anchor.
    const auto fit_at = [&](double fitted_step, bool print)
    {
        return FitAt(run, reference, step, kernel, output, total, piece, result, anchor_place, skip, fitted_step, print,
                     false);
    };

    // The step that fits better counts. A search can still settle on a step a little off where the reference holds
    // little in the lowest third of the band. When the sound fits below kLeastSnr with the searches' steps, the pitch's
    // step takes their place if it fits better.
    Comparison fitted = fit_at(steps.front(), false);
    if (steps.size() > 1)
    {
        Comparison other = fit_at(steps.back(), false);
        if (Judged(other) > Judged(fitted))
        {
            fitted = std::move(other);
        }
    }
    if (Judged(fitted) < kLeastSnr && std::find(steps.begin(), steps.end(), step) == steps.end())
    {
        Comparison nominal = fit_at(step, false);
        if (Judged(nominal) > Judged(fitted))
        {
            fitted = std::move(nominal);
        }
    }

    // A tone that swells can fit best whole periods early. When the sound fits below kLeastSnr from the start that the
    // search found, the start at the output's onset takes its place if it fits better: where the output first reaches
    // 1/256 of its peak, less the output samples before the reference first sounds.
    if (!start && Judged(fitted) < kLeastSnr)
    {
        if (const std::optional<std::size_t> onset = Onset(output))
        {
            const std::size_t guess = *onset > lead ? *onset - lead : 0;
            const std::size_t other_start =
                best_lag(search, guess > kRefine ? guess - kRefine : 0, guess + kRefine).value_or(guess);
            if (other_start != result.start)
            {
                Comparison other = Compare(run, reference, step, false, other_start);
                if (Judged(other) > Judged(fitted))
                {
                    return detail ? Compare(run, reference, step, true, other_start) : other;
                }
            }
        }
    }
    if (detail)
    {
        fitted = fit_at(fitted.step, true);
    }

    return fitted;
}

Comparison WithoutFullScale(const Run& run, const Reference& reference, double step, const Comparison& comparison)
{
    const Kernel kernel(step);
    const std::size_t total = run.output.size() / 2;
    const auto piece = static_cast<std::size_t>(kPieceSeconds * run.rate);

    return FitAt(run, reference, step, kernel, run.output, total, piece, comparison, comparison.place, 0,
                 comparison.step, false, true);
}

} // namespace hokuto::tools::players
