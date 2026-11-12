// SPDX-License-Identifier: MIT

// The parts of hokuto_check_players that its files share: the constants of the fit, a run of a model, the fit, the
// tallies and the checks. check_players.cpp describes the checks.

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "ntdlib/cafe/snd/engine.h"
#include "ntdlib/ctr/snd/engine.h"
#include "ntdlib/ctr/snd/sound_players.h"
#include "ntdlib/nwsnd/stream_sound_player.h"
#include "ntdlib/nx/atk/engine.h"
#include "ntdlib/rsar/formats.h"
#include "ntdlib/rsar/stream_file.h"
#include "ntdlib/rvl/ax/pb.h"
#include "ntdlib/rvl/snd/engine.h"
#include "ntdlib/rvl/snd/rsar_options.h"
#include "ntdlib/rvl/snd/stream_sound.h"
#include "ntdlib/sar/decode.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sar/stream_file.h"
#include "ntdlib/sequence_export/consoles.h"
#include "ntdlib/sound_build.h"
#include "xsflib/console.h"
#include "xsflib/files.h"

#include "checked_archive.h"
#include "song/aac.h"
#include "song/archive.h"
#include "song/ctr_model.h"
#include "song/items.h"
#include "song/streams.h"

namespace hokuto::tools::players
{

namespace atk = ntdlib::nx::atk;
namespace cafe = ntdlib::cafe::snd;
namespace rsar = ntdlib::rsar;
namespace rvl = ntdlib::rvl::snd;
namespace sar = ntdlib::sar;

// How long a sound that loops plays, how long a sound without a loop plays past its wave's end, how far past the end of
// its first pass a stream plays, and how long a sound without a loop or a stream plays at most, in seconds.
constexpr double kLongestSeconds = 30.0;
constexpr double kPastEndSeconds = 1.0;
constexpr double kPastLoopSeconds = 5.0;
constexpr double kLongestStreamSeconds = 360.0;

// A sound fails below this signal-to-noise ratio in the band beyond a fixed filter, in dB, unless it counts apart.
constexpr double kLeastSnr = 20.0;

// The share of a sound's energy in the band below which a sound at a step above 1 takes the better of the ratio in the
// band and the ratio over the whole band.
constexpr double kLeastBandShare = 0.1;

// The residual's mean power where a sound plays, in dB relative to full scale, at or below which the sound passes
// whatever its ratio.
constexpr double kFloorDb = -80.0;

// The output's full scale: the most or the least that a 16-bit sample holds.
constexpr float kFullScale = 32767.0f / 32768.0f;

// The band below which the tool fits the gains that it reports, in Hz. A sound gives gains when at least kLeastLowShare
// of its reference's energy lies there, and a stereo wave when its channels correlate less than kMostAlike.
constexpr double kGainBand = 1000.0;
constexpr double kLeastLowShare = 0.01;
constexpr double kMostAlike = 0.999;

// The taps on each side of an output sample that the fitted filter takes, for up to kMostFilteredChannels channels.
constexpr int kTaps = 6;
constexpr std::size_t kMostFilteredChannels = 4;

// The frames that a sound without a loop may run past the frame where its wave ends.
constexpr long long kLeastEndFrames = -1;
constexpr long long kMostEndFrames = 4;

// The band below which the tool also measures the ratio, in Hz, and the taps on each side of the low-pass filter that
// keeps it. The Wii U's AX mixes at 32 kHz, and the model's output reaches 48 kHz after the final mix. Above 8 kHz, a
// sound of 48 kHz can then hold what the resampling to 32 kHz folds down from above 16 kHz.
constexpr double kBand = 7000.0;
constexpr int kBandTaps = 48;

// A sound that plays at a low rate gets a lower band: the resampler's taps leave images of its sounds above half that
// rate. The band ends at this share of the rate, and its filter takes more taps.
constexpr double kBandShare = 0.45;
constexpr int kMostBandTaps = 1024;

// The output samples of a window that finds the reference's place, and the seconds of each piece of the fit.
constexpr std::size_t kWindow = 4800;
constexpr double kPieceSeconds = 0.5;

// The most of a window's energy that the reference's best fit there may leave for the window to set the step. A window
// that holds no trace of the reference, such as one in a wave's silent tail, leaves more.
constexpr double kMostLeftShare = 0.5;

// The latest output sample where a sound's reference may start, the output samples after the reference first sounds
// that the search for the start spans, and the samples around the first lag found that the second search takes.
constexpr std::size_t kMostStart = 2048;
constexpr std::size_t kOpening = 8192;
constexpr std::size_t kRefine = 16;

// The channels of a wave or a stream that the output has to hold, from their start. A loop repeats to no end.
class Reference
{
public:
    // The channels `channels` of `pcm`. `pcm` has to outlive the reference.
    Reference(const sar::Pcm& pcm, std::vector<std::size_t> channels)
        : pcm_(pcm), channels_(std::move(channels)), loop_(pcm.loop && pcm.loop_start < pcm.loop_end)
    {
    }

    // The number of channels.
    std::size_t Channels() const
    {
        return channels_.size();
    }

    // The samples up to the end of the first pass.
    std::size_t Length() const
    {
        return pcm_.Length();
    }

    // Whether the reference repeats its loop.
    bool Loops() const
    {
        return loop_;
    }

    // The first sample of the first pass where a channel reaches 1/256 of the first pass's peak. Nothing when every
    // channel stays silent.
    std::optional<std::size_t> FirstSound() const
    {
        int peak = 0;
        for (const std::size_t channel : channels_)
        {
            const std::vector<int16_t>& samples = pcm_.channels[channel];
            for (std::size_t i = 0; i < std::min<std::size_t>(samples.size(), pcm_.loop_end); i++)
            {
                peak = std::max(peak, std::abs(int{samples[i]}));
            }
        }
        if (peak == 0)
        {
            return std::nullopt;
        }
        const int threshold = std::max(1, peak / 256);
        std::optional<std::size_t> first;
        for (const std::size_t channel : channels_)
        {
            const std::vector<int16_t>& samples = pcm_.channels[channel];
            for (std::size_t i = 0; i < std::min<std::size_t>(samples.size(), pcm_.loop_end); i++)
            {
                if (std::abs(int{samples[i]}) >= threshold)
                {
                    first = std::min(first.value_or(i), i);
                    break;
                }
            }
        }

        return first;
    }

    // Sample `index` of channel `channel`, as a fraction of full scale. 0 before the start, and after the end of a
    // reference without a loop.
    float Sample(std::size_t channel, long long index) const
    {
        if (index < 0)
        {
            return 0.0f;
        }

        auto place = static_cast<std::size_t>(index);
        if (place >= pcm_.loop_end)
        {
            if (!loop_)
            {
                return 0.0f;
            }
            place = pcm_.loop_start + (place - pcm_.loop_end) % (pcm_.loop_end - pcm_.loop_start);
        }
        const std::vector<int16_t>& samples = pcm_.channels[channels_[channel]];
        return place < samples.size() ? static_cast<float>(samples[place]) / 32768.0f : 0.0f;
    }

private:
    const sar::Pcm& pcm_;
    std::vector<std::size_t> channels_;
    bool loop_;
};

// The model's output, interleaved stereo at `rate`, and its frames.
struct Run
{
    bool started = false;
    std::vector<float> output;
    double rate = 48000.0;
    std::size_t frames = 0;
    std::size_t frame_samples = 0;
    bool ended = false;
};

// What the comparison of a sound's output with its reference found.
struct Comparison
{
    double filtered_snr = 0.0;                // beyond a fixed filter, in dB, or 999
    double band_snr = 0.0;                    // beyond a fixed filter and below `band`, in dB, or 999
    double band = 0.0;                        // in Hz
    double snr = 0.0;                         // in dB, or 999 for an output that equals the fitted reference
    std::size_t start = 0;                    // the output sample where the reference starts
    double place = 0.0;                       // the reference's place there
    double step = 0.0;                        // the reference's samples for each output sample
    std::vector<std::array<double, 2>> gains; // a gain from each channel of the reference to each side, below kGainBand
    double low_share = 0.0;                   // the share of the reference's energy below kGainBand
    double energy = 0.0;                      // the output's energy
    std::optional<long long> end_frames;      // the frames that the sound ran past the frame where its wave ends
    bool band_measured = true;                // false for a sound too short for the band's filter
    double band_share = 1.0;                  // the share of the output's energy in the band
    double residual_level = 0.0;              // the residual's mean power where the sound plays, in dB, or -999
};

// Whether the band of a comparison holds images of the resampler and little else: at a step above 1, with less than
// kLeastBandShare of the output's energy there.
bool Imaged(const Comparison& comparison);

// The ratio that judges a comparison: in the band, or beyond a fixed filter for a sound too short for the band. The
// better of the two judges a comparison whose band holds images and little else.
double Judged(const Comparison& comparison);

// Whether a comparison passes: by the ratio that judges it, or by the floor.
bool Passes(const Comparison& comparison);

// The first sample of interleaved stereo `output` where a side reaches 1/256 of the output's peak. Nothing for a silent
// output.
std::optional<std::size_t> Onset(std::span<const float> output);

// Compares the run's output with the reference. The console reads the reference at `step` samples for each output
// sample.
Comparison Compare(const Run& run, const Reference& reference, double step, bool detail,
                   std::optional<std::size_t> start = std::nullopt);

// The comparison `comparison` of the run's output with the reference, fitted again at its start, place and step. The
// residual counts as zero at each output sample at full scale.
Comparison WithoutFullScale(const Run& run, const Reference& reference, double step, const Comparison& comparison);

// The counts and the extremes of the comparisons of a kind of sound.
struct Tally
{
    // A ratio's bin: 40 dB or more, from 30, from 20, and below 20.
    static std::size_t Bin(double snr)
    {
        return snr >= 40.0 ? 0 : snr >= 30.0 ? 1 : snr >= 20.0 ? 2 : 3;
    }

    // Counts the comparison `comparison` of sound `name`. The sound plays at the step `nominal_step`.
    void Add(const std::string& name, const Comparison& comparison, double nominal_step)
    {
        compared++;
        if (comparison.snr < least_snr)
        {
            least_snr = comparison.snr;
            least_name = name;
        }
        bins[Bin(comparison.snr)]++;
        if (comparison.band_measured)
        {
            if (comparison.band_snr < least_band_snr)
            {
                least_band_snr = comparison.band_snr;
                least_band_name = name;
            }
            band_bins[Bin(comparison.band_snr)]++;
        }
        else
        {
            short_sounds++;
            if (comparison.filtered_snr < least_short_snr)
            {
                least_short_snr = comparison.filtered_snr;
                least_short_name = name;
            }
        }
        if (comparison.band_measured && Imaged(comparison) && comparison.band_snr < kLeastSnr)
        {
            imaged++;
            least_imaged_snr = std::min(least_imaged_snr, comparison.filtered_snr);
        }
        if (Judged(comparison) < kLeastSnr && comparison.residual_level <= kFloorDb)
        {
            floored++;
            highest_floored = std::max(highest_floored, comparison.residual_level);
        }
        largest_step = std::max(largest_step, std::abs(comparison.step / nominal_step - 1.0));
        if (comparison.end_frames)
        {
            ends++;
            least_end = std::min(least_end, *comparison.end_frames);
            most_end = std::max(most_end, *comparison.end_frames);
        }
    }

    // Prints the counts of the comparisons of `what` in archive `archive` on a line.
    void Print(const std::string& archive, const char* what) const
    {
        if (compared == 0)
        {
            std::printf("%s: %s: none\n", archive.c_str(), what);
            return;
        }

        std::printf(
            "%s: %s: %zu compared; as heard, %zu at 40 dB or more, %zu from 30, %zu from 20, %zu below 20, the "
            "least %.1f dB (%s); in the band beyond a fixed filter, %zu, %zu, %zu and %zu, the least %.1f dB "
            "(%s); the step %.2g off the pitch's at most",
            archive.c_str(), what, compared, bins[0], bins[1], bins[2], bins[3], least_snr, least_name.c_str(),
            band_bins[0], band_bins[1], band_bins[2], band_bins[3], least_band_snr, least_band_name.c_str(),
            largest_step);
        if (short_sounds > 0)
        {
            std::printf("; %zu too short for the band, the least %.1f dB beyond a fixed filter (%s)", short_sounds,
                        least_short_snr, least_short_name.c_str());
        }
        if (imaged > 0)
        {
            std::printf(
                "; %zu below 20 dB in the band at a step above 1, with less than a tenth of their energy "
                "there, the least %.1f dB beyond a fixed filter",
                imaged, least_imaged_snr);
        }
        if (floored > 0)
        {
            std::printf("; %zu below 20 dB with the residual %.1f dB below full scale or lower", floored,
                        -highest_floored);
        }
        if (ends > 0)
        {
            std::printf("; %zu ended %lld to %lld frames after the frame where their wave ends", ends, least_end,
                        most_end);
        }
        std::printf("\n");
    }

    std::size_t compared = 0;
    std::array<std::size_t, 4> bins{};
    std::array<std::size_t, 4> band_bins{};
    double least_snr = 1e9;
    std::string least_name;
    double least_band_snr = 1e9;
    std::string least_band_name;
    std::size_t short_sounds = 0;
    double least_short_snr = 1e9;
    std::string least_short_name;
    std::size_t imaged = 0;
    double least_imaged_snr = 1e9;
    std::size_t floored = 0;
    double highest_floored = -1e9;
    double largest_step = 0.0;
    std::size_t ends = 0;
    long long least_end = 1000000;
    long long most_end = -1000000;
};

// The extremes of a set of gains.
struct GainRange
{
    // Counts the gain `value`.
    void Add(double value)
    {
        count++;
        least = std::min(least, value);
        most = std::max(most, value);
    }

    // Prints the extremes of the gains of `what` in archive `archive` on a line, when it counted any.
    void Print(const std::string& archive, const char* what) const
    {
        if (count > 0)
        {
            std::printf("%s: %s: %zu, from %.4f to %.4f\n", archive.c_str(), what, count, least, most);
        }
    }

    std::size_t count = 0;
    double least = 1e9;
    double most = -1e9;
};

// Whether each of `channels` of `pcm` holds nothing but zeros.
bool Silent(const sar::Pcm& pcm, const std::vector<std::size_t>& channels);

// Whether channels `a` and `b` of `pcm` correlate by kMostAlike or more over the first pass. A fit can't then separate
// their gains.
bool Alike(const sar::Pcm& pcm, std::size_t a, std::size_t b);

// Whether a sound ended outside kLeastEndFrames to kMostEndFrames frames of the frame where its wave ends.
bool WrongLength(const Comparison& comparison);

// The counts of an archive's wave sounds, and the tallies and gains of those compared.
struct WaveSoundTotals
{
    Tally comparable;
    Tally apart;
    GainRange mono;
    GainRange stereo_direct;
    GainRange stereo_cross;
    std::size_t without_gains = 0;
    std::size_t wave_sounds = 0;
    std::size_t missing = 0;
    std::size_t silent = 0;
    std::size_t first_frame = 0;
    std::size_t failed = 0;
    std::size_t endless = 0;
    std::size_t export_wave = 0;
    std::size_t export_pitch = 0;
    std::size_t several_notes = 0;
    std::size_t several_events = 0;
    std::size_t filtered = 0;
    std::size_t shaped = 0;
    std::size_t panned = 0;
    std::size_t sent = 0;
};

// Prints the summary of the wave sounds of the archive `archive`.
void PrintWaveSounds(const std::string& archive, const WaveSoundTotals& totals);

// What the check reads of a track of a stream sound that plays: its channels, its volume, its pan and its span.
struct PlayedTrack
{
    std::vector<uint8_t> channels;
    uint8_t volume = 127;
    uint8_t pan = 64;
    uint8_t span = 0;
};

// What the check of an archive's stream sounds counts.
struct StreamTotals
{
    Tally tally;
    GainRange mono;
    GainRange direct;
    GainRange cross;
    std::size_t sounds = 0;
    std::size_t absent = 0;
    std::size_t silent = 0;
    std::size_t refused = 0; // the Wii's sounds that the build's player can't play
    std::size_t clipped = 0; // the sounds whose output reaches full scale
    std::size_t without_gains = 0;
    std::size_t aac = 0;
    std::size_t failed = 0;
};

// Prints the summary of an archive's stream sounds.
void PrintStreamSounds(const std::string& archive, const StreamTotals& totals);

} // namespace hokuto::tools::players
