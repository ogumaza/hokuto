// SPDX-License-Identifier: MIT

// Checks ntdlib's players of wave sounds and of stream sounds on a sound archive of the Switch, the Wii U, the 3DS or
// the Wii. The browser's previews play through those players (src/song/preview.h). The tool opens the archive with the
// group files and the streams beside it. Each wave sound, and each stream sound whose stream lies beside the archive,
// plays by itself on the console's model without effects. The tool starts each one on ntdlib's engines itself, with
// helpers that the preview uses too, such as song::WaveSoundNoteOf. The check never runs song::SoundPreview. A wave
// sound without a loop plays until it ends, and one with a loop for 30 s. A stream with a loop plays to 5 s past the
// end of its first pass. One without a loop plays to 1 s past its end. Neither plays for more than 6 minutes. A stream
// without a loop that the limit cuts short doesn't have to end.
//
// The reference doesn't come from the model's voices. ntdlib's readers decode the wave of the wave sound's first note,
// or each channel of the stream's tracks. The exports decode them that way too. A wave or a stream that loops repeats
// its loop. The tool resamples the reference with a windowed sinc. It finds the output sample where the reference
// starts, and then the place in the reference and the step that the console's resampler takes, from the sound's pitch.
// It fits a gain from each channel of the reference to each side of the output, and measures the output's
// signal-to-noise ratio against the fitted reference: the ratio as heard. The consoles' resamplers interpolate with 4
// taps. The 4 taps dull the highest frequencies. The tool therefore also fits a filter of 13 taps from each channel to
// each side, and measures the ratio against that: the ratio beyond a fixed filter. It then measures that ratio in the
// band below 7 kHz, or below 0.45 times the rate at which the sound plays when that's lower: the band holds no images
// of the resampling. A sound whose wave ends has to end with it. The gains that the tool reports come from a fit of the
// reference and the output below 1 kHz. A stereo wave whose channels hold nearly the same samples, and a sound with
// little energy there, give no gains.
//
// A sound fails below 20 dB in the band. The fits leave out the voice's first frame. The band's filter spans samples on
// each side of each sample that it measures. A sound that plays for less than twice the filter's length after its first
// frame is judged by the ratio beyond a fixed filter instead. A wave that ends within its first frame counts apart. At
// a step above 1, the console's resampler puts images of a sound's higher frequencies into the band too. When the band
// holds less than a tenth of such a sound's energy, the better of the two ratios judges it. A sound also passes when
// the residual where it plays lies 80 dB or more below full scale. The output holds 16-bit samples. Their rounding
// alone can keep a quiet sound's ratio below 20 dB. A tone that swells can fit best whole periods early. When a sound
// fits below 20 dB from the start that the search finds, a start at the output's onset takes its place if it fits
// better. On every console, a stream sound below 20 dB whose output reaches full scale counts apart, and its end isn't
// checked: the mix of a stream's tracks can clip, and the fit can't follow it.
//
// A wave sound counts apart when its settings change it beyond a gain: a filter, or an envelope that fades it in or
// holds it below full level. The tool also counts the wave sounds that the WAV export plays otherwise than the game.
// The export plays note 0 at the sound's pitch times the note's. The game plays note 0 at the sound's pitch.
//
// On the 3DS, each sound plays on ntdlib's archive mode of the 3DS, on its model of the DSP's audio firmware, in strict
// mixing (song/ctr_model.h). The DSP's output runs at about 32,728.5 Hz. nn::snd turns a pitch into the DSP's step at
// 32,728 Hz, and the tool takes that rate. The output follows the engine by four frames: three of silence and one of
// the DSP. The tool takes a sound's end four frames after the frame where the engine ends it. The 3DS's players read no
// filters, no sends of a stream's tracks and no pitch of a stream sound. A stream sound plays up to 8 channels: no more
// than its entry gives. A stream's voices keep the pan curve 0. An AAC stream plays as the preview plays it: the
// system's decoder decodes it into a stream file of 16-bit PCM, with the loop that the sound's entry gives
// (song::PlayableStream). The reference is that file's samples. The AAC test checks the decoding.
//
// On the Wii, the tool takes the game's build of NW4R SND from the main.dol of the extracted disc that holds the
// archive, or else the build that the archive's version gives. An import does the same. Each wave sound plays on
// ntdlib's model of NW4R SND and AX, with ntdlib's model of AX's microcode in place of the DSP. The model's output runs
// at AX's 32 kHz. NW4R SND plays note 0 of a wave sound, and reads only its wave and its envelope. The reference is
// that wave at the sound's pitch. A wave sound of the Wii has no filter. A stream sound plays on ntdlib's model of NW4R
// SND's stream player, at the stream's rate: the Wii's stream sounds have no pitch. The build's player selects the
// tracks (rvl::snd::StrmPlayer::TracksOf), and each track's voice plays the track's first two channels. The builds
// before March 2008 play the stream's first two channels at full volume and in the centre. A stream sound that the
// build's player can't play counts apart. The model plays every track at the stream file's volume, and the mix of a
// stream's layers can clip (see above). A game's code sets the tracks' volumes as it plays the stream.
//
// The tool prints a line for each stream sound, and for each wave sound that fails by its ratio or ends at another
// time. It then prints a summary. It returns 1 when such a sound doesn't count apart, and 2 when the archive can't be
// read. With a sound's name, it checks that sound alone, and prints the ratios in each half second.
//
// usage: hokuto_check_players <archive> [sound]

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
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

#include "song/aac.h"
#include "song/archive.h"
#include "song/ctr_model.h"
#include "song/items.h"
#include "song/streams.h"

namespace
{

namespace atk = ntdlib::nx::atk;
namespace cafe = ntdlib::cafe::snd;
namespace rsar = ntdlib::rsar;
namespace rvl = ntdlib::rvl::snd;
namespace sar = ntdlib::sar;

constexpr double kPi = 3.14159265358979323846;

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
        const double sinc = x == 0.0 ? 1.0 : std::sin(kPi * x) / (kPi * x);
        const double window =
            0.42 + 0.5 * std::cos(kPi * distance / reach) + 0.08 * std::cos(2.0 * kPi * distance / reach);
        return scale_ * sinc * window;
    }

    double scale_;
    int half_;
    std::size_t width_;
    std::vector<float> taps_;
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

// Runs the console's model with the sound that `start` starts, until nothing sounds or for `limit` frames.
template <class ArchiveModel, class Start>
Run RunModel(const sar::SoundArchive& archive, std::size_t limit, const Start& start)
{
    Run run;
    ArchiveModel model(archive);
    run.rate = ArchiveModel::kSampleRate;
    run.frame_samples = ArchiveModel::kFrameSamples;
    if (!start(*model.engine))
    {
        return run;
    }
    run.started = true;
    while (run.frames < limit && model.engine->IsBusy())
    {
        model.engine->RunFrame();
        std::span<const int16_t> output;
        if constexpr (std::is_same_v<ArchiveModel, cafe::ArchiveModel>)
        {
            output = model.system.Output();
        }
        else
        {
            output = model.renderer.DeviceOutput();
        }
        for (const int16_t sample : output)
        {
            run.output.push_back(static_cast<float>(sample) / 32768.0f);
        }
        run.frames++;
    }
    run.ended = !model.engine->IsBusy();

    return run;
}

// Runs ntdlib's archive mode of the 3DS in strict mixing with the sound that `start` starts, until nothing sounds or
// for `limit` frames. The model runs kTailFrames frames past the engine's end, more than the output's lag. The output
// of each frame counts. The run's frames end with the output's lag after the engine's end.
template <class Start>
Run RunCtrModel(const sar::SoundArchive& archive, std::size_t limit, const Start& start)
{
    using hokuto::song::CtrModel;
    constexpr std::size_t kLagFrames = CtrModel::kSilence / CtrModel::kFrameSamples + 1;

    Run run;
    CtrModel model;
    run.rate = CtrModel::kSampleRate;
    run.frame_samples = CtrModel::kFrameSamples;
    if (!model.Start(archive) || !start(model.Engine()))
    {
        return run;
    }
    run.started = true;
    std::size_t frames = 0;
    std::optional<std::size_t> engine_end;
    while (frames < limit && model.IsBusy())
    {
        if (!model.RunFrame())
        {
            break;
        }

        for (const int16_t sample : model.Output())
        {
            run.output.push_back(static_cast<float>(sample) / 32768.0f);
        }
        frames++;
        if (!engine_end && !model.Engine().IsBusy())
        {
            engine_end = frames;
        }
    }
    run.ended = !model.IsBusy();
    run.frames = engine_end ? std::min(frames, *engine_end + kLagFrames) : frames;

    return run;
}

// Runs ntdlib's model of NW4R SND and AX for the Wii's `archive`, on `build` of NW4R SND, with the sound that `start`
// starts, until nothing sounds or for `limit` frames.
template <class Start>
Run RunWiiModel(const rsar::SoundArchive& archive, const ntdlib::SndBuild& build, std::size_t limit, const Start& start)
{
    Run run;
    rvl::ArchiveModel model;
    model.build = build;
    model.StartWithRenderer(archive);
    run.rate = ntdlib::rvl::ax::kSampleRate;
    run.frame_samples = static_cast<std::size_t>(ntdlib::rvl::ax::kSamplesPerFrame);
    if (!start(*model.engine))
    {
        return run;
    }
    run.started = true;
    std::vector<int16_t> output;
    while (run.frames < limit && model.engine->IsBusy())
    {
        output.clear();
        if (!model.RunFrame(output))
        {
            break;
        }
        for (const int16_t sample : output)
        {
            run.output.push_back(static_cast<float>(sample) / 32768.0f);
        }
        run.frames++;
    }
    run.ended = !model.engine->IsBusy();

    return run;
}

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
        const double k = std::tan(kPi * kGainBand / rate);
        for (std::size_t i = 0; i < 2; i++)
        {
            // The poles of the fourth order's two pairs.
            const double q = 1.0 / (2.0 * std::cos(kPi * (2.0 * static_cast<double>(i) + 1.0) / 8.0));
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
bool Imaged(const Comparison& comparison)
{
    return comparison.step > 1.0 && comparison.band_share < kLeastBandShare;
}

// The ratio that judges a comparison: in the band, or beyond a fixed filter for a sound too short for the band. The
// better of the two judges a comparison whose band holds images and little else.
double Judged(const Comparison& comparison)
{
    if (!comparison.band_measured)
    {
        return comparison.filtered_snr;
    }

    return Imaged(comparison) ? std::max(comparison.band_snr, comparison.filtered_snr) : comparison.band_snr;
}

// Whether a comparison passes: by the ratio that judges it, or by the floor.
bool Passes(const Comparison& comparison)
{
    return Judged(comparison) >= kLeastSnr || comparison.residual_level <= kFloorDb;
}

// The first sample of interleaved stereo `output` where a side reaches 1/256 of the output's peak. Nothing for a silent
// output.
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

// Compares the run's output with the reference. The console reads the reference at `step` samples for each output
// sample.
Comparison Compare(const Run& run, const Reference& reference, double step, bool detail,
                   std::optional<std::size_t> start = std::nullopt)
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
        Comparison found = result;
        found.step = fitted_step;
        found.place = anchor_place - fitted_step * static_cast<double>(skip);

        // One gain for each channel and side over the whole output, from the voice's second frame on, a piece at a
        // time, and one filter. The gains that the tool reports come from a third fit, of the reference and the output
        // below kGainBand. The consoles' resamplers pass every frequency alike there. A filter's gain at 0 Hz can take
        // any value when the sound holds nothing low, such as a tone.
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
            const std::vector<std::vector<float>> resampled =
                Resampled(reference, kernel, place, found.step, count, kTaps);
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

        // What the filter leaves, and the ratio below the band: the output and the residual each through a low-pass
        // filter.
        const std::size_t begin = found.start + std::min(run.frame_samples, total - found.start);
        std::vector<float> residual(run.output.size());
        for (std::size_t first = begin; first < total; first += piece)
        {
            const std::size_t count = std::min(piece, total - first);
            const double place = found.place + found.step * static_cast<double>(first - found.start);
            filtered.Residual(Resampled(reference, kernel, place, found.step, count, kTaps), kTaps, output, first,
                              count, residual);
        }
        found.band = std::min(kBand, kBandShare * step * run.rate);
        const int half = std::min(kMostBandTaps, static_cast<int>(std::ceil(kBandTaps * kBand / found.band)));
        std::vector<double> low(static_cast<std::size_t>(2 * half + 1));
        const double cutoff = found.band / run.rate;
        for (int k = -half; k <= half; k++)
        {
            const double x = 2.0 * cutoff * k;
            const double sinc = k == 0 ? 1.0 : std::sin(kPi * x) / (kPi * x);
            const double window =
                0.42 + 0.5 * std::cos(kPi * k / (half + 1)) + 0.08 * std::cos(2.0 * kPi * k / (half + 1));
            low[static_cast<std::size_t>(k + half)] = 2.0 * cutoff * sinc * window;
        }
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

        const auto ratio = [](double kept, double left)
        {
            return left <= 0.0 ? 999.0 : 10.0 * std::log10(kept / left);
        };

        const auto [kept, band_left] = band_energies(begin, total);
        found.band_snr = ratio(kept, band_left);
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
                    static_cast<double>(first) / run.rate, ratio(energy, left), ratio(piece_kept, piece_left),
                    found.band, energy > 0.0 ? 10.0 * std::log10(energy / static_cast<double>(2 * count)) : -999.0,
                    peak > 0.0f ? 20.0 * std::log10(static_cast<double>(peak)) : -999.0);
            }
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

// What a wave sound plays, and how its WAV export differs from the game.
struct WaveSoundFacts
{
    std::string name;
    uint32_t wave_archive = 0;
    uint32_t wave = 0;
    float pitch = 1.0f;
    bool filtered = false;     // a low-pass or a biquad filter
    bool shaped = false;       // an envelope that fades it in or holds it below full level
    bool panned = false;       // a pan, a surround pan, or a pan curve other than the default
    bool sent = false;         // sends other than the main bus at full level
    bool muted = false;        // a main send of 0
    bool export_wave = false;  // the WAV export plays another wave
    bool export_pitch = false; // the WAV export plays at another pitch
    std::size_t notes = 0;
    std::size_t events = 0;
};

// The facts of wave sound `sound`. Throws std::runtime_error when its file or its wave is missing.
WaveSoundFacts FactsOf(const sar::SoundArchive& archive, uint32_t sound)
{
    const sar::SoundInfo& info = archive.Sounds()[sound];
    const std::span<const uint8_t> file =
        hokuto::song::ItemFile(archive, {hokuto::song::ItemKind::kWaveSound, sound, 0});
    const sar::WaveSoundFile parsed = sar::WaveSoundFile::Parse(file, hokuto::song::ItemOptions(archive));
    if (info.wave_sound_index >= parsed.sounds.size())
    {
        throw std::runtime_error("the file lacks the wave sound");
    }
    const sar::WaveSoundFile::WaveSound& wave_sound = parsed.sounds[info.wave_sound_index];
    if (wave_sound.notes.empty() || wave_sound.notes.front().wave_id_index >= parsed.waves.size())
    {
        throw std::runtime_error("the sound plays no wave");
    }
    const sar::WaveId& id = parsed.waves[wave_sound.notes.front().wave_id_index];
    const std::optional<std::size_t> wave_archive =
        sar::ItemIndex(id.wave_archive_item, sar::kWaveArchiveItem, archive.WaveArchives().size());
    if (!wave_archive)
    {
        throw std::runtime_error("the archive lacks the wave archive");
    }

    WaveSoundFacts facts;
    facts.name = info.name;
    facts.wave_archive = static_cast<uint32_t>(*wave_archive);
    facts.wave = id.index;
    facts.pitch = wave_sound.pitch;
    facts.filtered = wave_sound.low_pass != 64 || wave_sound.biquad_type != 0;
    facts.shaped = wave_sound.adshr.attack < 127 || wave_sound.adshr.sustain < 127;
    facts.panned = wave_sound.pan != 64 || wave_sound.span != 0 || info.pan_curve != 0;
    facts.sent = wave_sound.main_send != 127 || wave_sound.aux_sends != std::array<uint8_t, 3>{};
    facts.muted = wave_sound.main_send == 0;
    facts.notes = wave_sound.notes.size();

    // The WAV export's note.
    try
    {
        const hokuto::song::WaveSoundNote note = hokuto::song::WaveSoundNoteOf(archive, sound);
        facts.export_wave = note.wave_archive != facts.wave_archive || note.wave != facts.wave;
        facts.export_pitch = note.pitch != facts.pitch;
        facts.events = note.events;
    }
    catch (const std::exception&)
    {
        facts.export_wave = true;
    }

    return facts;
}

// The facts of the Wii's wave sound `sound`, read with ntdlib alone. NW4R SND plays note 0 of the sound, at the sound's
// pitch, and reads only the note's wave and envelope. The wave lies in the wave data of the sound's file, by the file's
// ID (song::WaveArchivesOf). AX mixes an aux bus back into the output only through its effect, and the preview plays
// none. Only a main send below full level counts as another send. Throws std::runtime_error when the file or its wave
// is missing.
WaveSoundFacts FactsOf(const rsar::SoundArchive& archive, uint32_t sound)
{
    const rsar::SoundInfo& info = archive.Sounds()[sound];
    const rsar::WaveSoundFile parsed =
        rsar::WaveSoundFile::Parse(archive.FileData(info.file_id), ntdlib::rvl::snd::kRsarOptions);
    if (info.sub_index < 0 || static_cast<std::size_t>(info.sub_index) >= parsed.Sounds().size())
    {
        throw std::runtime_error("the file lacks the wave sound");
    }
    const rsar::WaveSoundFile::WaveSound& wave_sound = parsed.Sounds()[static_cast<std::size_t>(info.sub_index)];
    if (wave_sound.notes.empty() || wave_sound.notes.front().wave_index < 0)
    {
        throw std::runtime_error("the sound plays no wave");
    }
    const rsar::WaveSoundFile::Note& note = wave_sound.notes.front();

    WaveSoundFacts facts;
    facts.name = info.name;
    facts.wave_archive = info.file_id;
    facts.wave = static_cast<uint32_t>(note.wave_index);
    facts.pitch = wave_sound.pitch;
    facts.shaped = note.attack < 127 || note.sustain < 127;
    facts.panned = wave_sound.pan != 64 || wave_sound.surround_pan != 0 || info.pan_curve != 0;
    facts.sent = wave_sound.main_send != 127;
    facts.muted = wave_sound.main_send == 0;
    facts.notes = wave_sound.notes.size();

    // The WAV export's note.
    try
    {
        const hokuto::song::WaveSoundNote exported = hokuto::song::WaveSoundNoteOf(archive, sound);
        facts.export_wave = exported.wave_archive != facts.wave_archive || exported.wave != facts.wave;
        facts.export_pitch = exported.pitch != facts.pitch;
        facts.events = exported.events;
    }
    catch (const std::exception&)
    {
        facts.export_wave = true;
    }

    return facts;
}

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
bool Silent(const sar::Pcm& pcm, const std::vector<std::size_t>& channels)
{
    for (const std::size_t channel : channels)
    {
        for (const int16_t sample : pcm.channels[channel])
        {
            if (sample != 0)
            {
                return false;
            }
        }
    }

    return true;
}

// Whether channels `a` and `b` of `pcm` correlate by kMostAlike or more over the first pass. A fit can't then separate
// their gains.
bool Alike(const sar::Pcm& pcm, std::size_t a, std::size_t b)
{
    double ab = 0.0;
    double aa = 0.0;
    double bb = 0.0;
    const std::size_t length = std::min<std::size_t>({pcm.channels[a].size(), pcm.channels[b].size(), pcm.Length()});
    for (std::size_t i = 0; i < length; i++)
    {
        const double x = pcm.channels[a][i];
        const double y = pcm.channels[b][i];
        ab += x * y;
        aa += x * x;
        bb += y * y;
    }

    return aa > 0.0 && bb > 0.0 && ab >= kMostAlike * std::sqrt(aa * bb);
}

// Whether a sound ended outside kLeastEndFrames to kMostEndFrames frames of the frame where its wave ends.
bool WrongLength(const Comparison& comparison)
{
    return comparison.end_frames &&
           (*comparison.end_frames < kLeastEndFrames || *comparison.end_frames > kMostEndFrames);
}

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

// Plays a wave sound of `facts` that plays `pcm` at `volume`, and compares the output with the wave. `run_for` runs the
// console's model with the sound for the seconds that it takes. The model's output runs at `rate`. `archive` names the
// archive in the lines that the check prints. With `detail`, the comparison prints the ratios in each half second.
template <class RunFor>
void CheckWaveSound(const std::string& archive, const WaveSoundFacts& facts, const sar::Pcm& pcm, uint8_t volume,
                    double rate, const RunFor& run_for, bool detail, WaveSoundTotals& totals)
{
    totals.export_wave += facts.export_wave ? 1 : 0;
    totals.export_pitch += !facts.export_wave && facts.export_pitch ? 1 : 0;
    totals.several_notes += facts.notes > 1 ? 1 : 0;
    totals.several_events += facts.events > 1 ? 1 : 0;
    totals.filtered += facts.filtered ? 1 : 0;
    totals.shaped += facts.shaped ? 1 : 0;
    totals.panned += facts.panned ? 1 : 0;
    totals.sent += facts.sent ? 1 : 0;

    // The console reads the wave at its rate times the sound's pitch.
    const double step = static_cast<double>(pcm.sample_rate) * static_cast<double>(facts.pitch) / rate;
    const bool loops = pcm.loop && pcm.loop_start < pcm.loop_end;
    const double seconds =
        loops ? kLongestSeconds
              : std::min(kLongestStreamSeconds, static_cast<double>(pcm.Length()) / (step * rate) + kPastEndSeconds);
    const Run run = run_for(seconds);
    if (!run.started)
    {
        std::printf("%s: %s doesn't start\n", archive.c_str(), facts.name.c_str());
        totals.failed++;
        return;
    }
    if (!loops && !run.ended)
    {
        std::printf("%s: %s doesn't end with its wave\n", archive.c_str(), facts.name.c_str());
        totals.endless++;
        totals.failed++;
        return;
    }
    std::vector<std::size_t> channels;
    for (std::size_t c = 0; c < std::min<std::size_t>(pcm.channels.size(), 2); c++)
    {
        channels.push_back(c);
    }
    const Reference reference(pcm, channels);
    if (detail)
    {
        std::printf(
            "  wave %u of wave archive %u: %zu channels, %zu samples at %u Hz%s; the pitch %.6f, the step %.6f\n",
            facts.wave, facts.wave_archive, pcm.channels.size(), static_cast<std::size_t>(pcm.Length()),
            pcm.sample_rate, loops ? ", looped" : "", static_cast<double>(facts.pitch), step);
    }
    const Comparison comparison = Compare(run, reference, step, detail);
    if (detail)
    {
        std::printf("  the start at %zu, the place %.2f, the step %.6f\n", comparison.start, comparison.place,
                    comparison.step);
    }
    if (comparison.energy == 0.0)
    {
        if (Silent(pcm, channels) || volume == 0 || facts.muted)
        {
            totals.silent++;
        }
        else if (static_cast<double>(pcm.Length()) / step <= static_cast<double>(run.frame_samples) &&
                 Onset(run.output))
        {
            totals.first_frame++;
        }
        else
        {
            std::printf("%s: %s plays nothing\n", archive.c_str(), facts.name.c_str());
            totals.failed++;
        }
        return;
    }

    const bool counted_apart = facts.filtered || facts.shaped;
    (counted_apart ? totals.apart : totals.comparable).Add(facts.name, comparison, step);
    if (!Passes(comparison) || WrongLength(comparison))
    {
        char band[64];
        if (comparison.band_measured)
        {
            std::snprintf(band, sizeof(band), "%.1f dB below %.0f Hz", comparison.band_snr, comparison.band);
        }
        else
        {
            std::snprintf(band, sizeof(band), "too short for the band");
        }
        std::printf(
            "%s: %s: %.1f dB as heard, %.1f dB beyond a fixed filter, %s, the residual at %.1f dB, ends %s frames "
            "after its wave%s\n",
            archive.c_str(), facts.name.c_str(), comparison.snr, comparison.filtered_snr, band,
            comparison.residual_level, comparison.end_frames ? std::to_string(*comparison.end_frames).c_str() : "-",
            counted_apart ? " (counted apart)" : "");
        totals.failed += counted_apart ? 0 : 1;
    }

    // The gains over the sound's volume, with the default pan curve: of a mono wave at the centre, and of each channel
    // of a stereo wave. Either pan mode keeps a stereo wave's channels on their sides there.
    if (!facts.panned && !facts.sent && !counted_apart && volume > 0)
    {
        const double share = volume / 127.0;
        if (comparison.low_share < kLeastLowShare ||
            (comparison.gains.size() == 2 && Alike(pcm, channels[0], channels[1])))
        {
            totals.without_gains++;
        }
        else if (comparison.gains.size() == 1)
        {
            totals.mono.Add(comparison.gains[0][0] / share);
            totals.mono.Add(comparison.gains[0][1] / share);
        }
        else
        {
            totals.stereo_direct.Add(comparison.gains[0][0] / share);
            totals.stereo_direct.Add(comparison.gains[1][1] / share);
            totals.stereo_cross.Add(comparison.gains[0][1] / share);
            totals.stereo_cross.Add(comparison.gains[1][0] / share);
        }
    }
}

// Prints the summary of the wave sounds of the archive `archive`.
void PrintWaveSounds(const std::string& archive, const WaveSoundTotals& totals)
{
    std::printf(
        "%s: %zu wave sounds, %zu without their files or waves, %zu silent as their waves or settings, %zu that don't "
        "end with their waves; %zu filtered, %zu shaped, %zu panned, %zu with other sends\n",
        archive.c_str(), totals.wave_sounds, totals.missing, totals.silent, totals.endless, totals.filtered,
        totals.shaped, totals.panned, totals.sent);
    std::printf(
        "%s: the WAV export plays another wave in %zu, and the same wave at another pitch in %zu; %zu hold more "
        "than one note, and %zu more than one note event\n",
        archive.c_str(), totals.export_wave, totals.export_pitch, totals.several_notes, totals.several_events);
    if (totals.first_frame > 0)
    {
        std::printf("%s: %zu wave sounds end within their first frame, before the fits begin\n", archive.c_str(),
                    totals.first_frame);
    }
    totals.comparable.Print(archive, "wave sounds without a filter or a shaping envelope");
    totals.apart.Print(archive, "wave sounds counted apart");
    std::printf("%s: %zu wave sounds give no gains: alike channels, or little energy below 1 kHz\n", archive.c_str(),
                totals.without_gains);
    totals.mono.Print(archive, "the gains of a mono wave at the centre, over the sound's volume");
    totals.stereo_direct.Print(archive, "the gains of a stereo wave's channels to their sides");
    totals.stereo_cross.Print(archive, "the gains of a stereo wave's channels to the other side");
}

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

// Checks stream sound `sound` of `archive` against `pcm`, its stream of `stream_rate` decoded, played at `pitch` (see
// the top of the file). The channels of `tracks` play, each once, up to `most_channels`. `run_for` runs the console's
// model with the sound for a number of seconds at most. The tracks at the centre give gains where `gains` holds: on the
// Switch and the Wii U, with the default pan curve only.
template <class RunFor>
void CheckStreamSound(const std::string& archive, const std::string& sound, const sar::Pcm& pcm, uint32_t stream_rate,
                      uint8_t volume, double pitch, const std::vector<PlayedTrack>& tracks, std::size_t most_channels,
                      bool gains, double rate, const RunFor& run_for, bool detail, StreamTotals& totals)
{
    std::vector<std::size_t> channels;
    for (const PlayedTrack& track : tracks)
    {
        for (const uint8_t channel : track.channels)
        {
            if (channel < pcm.channels.size() && channel < most_channels &&
                std::find(channels.begin(), channels.end(), channel) == channels.end())
            {
                channels.push_back(channel);
            }
        }
    }
    if (channels.empty())
    {
        totals.silent++;
        return;
    }

    const double step = static_cast<double>(stream_rate) * pitch / rate;
    const double first_pass = static_cast<double>(pcm.Length()) / (step * rate);
    const double seconds =
        std::min(kLongestStreamSeconds, first_pass + (pcm.loop ? kPastLoopSeconds : kPastEndSeconds));
    const bool cut_short = !pcm.loop && first_pass + kPastEndSeconds > kLongestStreamSeconds;
    const Run run = run_for(seconds);
    if (!run.started)
    {
        std::printf("%s: %s doesn't start\n", archive.c_str(), sound.c_str());
        totals.failed++;
        return;
    }
    const Reference reference(pcm, channels);
    const Comparison comparison = Compare(run, reference, step, detail);
    if (comparison.energy == 0.0)
    {
        if (Silent(pcm, channels) || volume == 0)
        {
            totals.silent++;
        }
        else
        {
            std::printf("%s: %s plays nothing\n", archive.c_str(), sound.c_str());
            totals.failed++;
        }
        return;
    }
    totals.tally.Add(sound, comparison, step);
    std::printf(
        "%s: %s: %zu channels at %u Hz, %s, %.1f s; %.1f dB as heard, %.1f dB beyond a fixed filter, %.1f "
        "dB below %.0f Hz, the step %.2g off the pitch's, the start at %zu, the gains",
        archive.c_str(), sound.c_str(), channels.size(), stream_rate, pcm.loop ? "looped" : "once", first_pass,
        comparison.snr, comparison.filtered_snr, comparison.band_snr, comparison.band, comparison.step / step - 1.0,
        comparison.start);
    for (const std::array<double, 2>& gain : comparison.gains)
    {
        std::printf(" %.4f/%.4f", gain[0], gain[1]);
    }
    if (comparison.end_frames)
    {
        std::printf(", ends %lld frames after its stream", *comparison.end_frames);
    }
    if (Judged(comparison) < kLeastSnr)
    {
        std::printf(", the residual at %.1f dB", comparison.residual_level);
    }

    // The output clips where the mix of the tracks reaches full scale. The fit then can't follow it.
    const auto full_scale = static_cast<std::size_t>(std::count_if(
        run.output.begin(), run.output.end(), [](float sample) { return std::abs(sample) >= kFullScale; }));
    if (full_scale > 0)
    {
        std::printf(", %zu samples at full scale", full_scale);
    }
    std::printf("\n");
    if (full_scale > 0 && !Passes(comparison))
    {
        totals.clipped++;
    }
    else if (!Passes(comparison) || WrongLength(comparison) || (!pcm.loop && !cut_short && !run.ended))
    {
        totals.failed++;
    }

    // The gains over the volumes: of each track at the centre that plays one channel or a pair. Either pan mode keeps a
    // pair's channels on their sides there.
    const auto index = [&](uint8_t channel)
    {
        return static_cast<std::size_t>(std::find(channels.begin(), channels.end(), channel) - channels.begin());
    };
    for (const PlayedTrack& track : tracks)
    {
        if (track.pan != 64 || track.span != 0 || !gains || track.volume == 0 || volume == 0 ||
            track.channels.empty() || track.channels[0] >= pcm.channels.size())
        {
            continue;
        }

        const double gain_of_volumes = volume / 127.0 * track.volume / 127.0;
        if (comparison.low_share < kLeastLowShare ||
            (track.channels.size() > 1 && track.channels[1] < pcm.channels.size() &&
             Alike(pcm, track.channels[0], track.channels[1])))
        {
            totals.without_gains++;
        }
        else if (track.channels.size() == 1)
        {
            const std::array<double, 2>& gain = comparison.gains[index(track.channels[0])];
            totals.mono.Add(gain[0] / gain_of_volumes);
            totals.mono.Add(gain[1] / gain_of_volumes);
        }
        else if (track.channels[1] < pcm.channels.size())
        {
            const std::array<double, 2>& left = comparison.gains[index(track.channels[0])];
            const std::array<double, 2>& right = comparison.gains[index(track.channels[1])];
            totals.direct.Add(left[0] / gain_of_volumes);
            totals.direct.Add(right[1] / gain_of_volumes);
            totals.cross.Add(left[1] / gain_of_volumes);
            totals.cross.Add(right[0] / gain_of_volumes);
        }
    }
}

// Prints the summary of an archive's stream sounds.
void PrintStreamSounds(const std::string& archive, const StreamTotals& totals)
{
    std::printf("%s: %zu stream sounds, %zu without their streams, %zu silent, %zu of AAC\n", archive.c_str(),
                totals.sounds, totals.absent, totals.silent, totals.aac);
    if (totals.refused > 0)
    {
        std::printf("%s: the game's build of NW4R SND plays %zu stream sounds no track of their streams\n",
                    archive.c_str(), totals.refused);
    }
    if (totals.clipped > 0)
    {
        std::printf("%s: %zu stream sounds below 20 dB clip at full scale, and count apart\n", archive.c_str(),
                    totals.clipped);
    }
    totals.tally.Print(archive, "stream sounds");
    std::printf("%s: %zu tracks give no gains: alike channels, or little energy below 1 kHz\n", archive.c_str(),
                totals.without_gains);
    totals.mono.Print(archive, "the gains of a mono track at the centre, over the volumes");
    totals.direct.Print(archive, "the gains of a pair's channels to their sides");
    totals.cross.Print(archive, "the gains of a pair's channels to the other side");
}

// Whether the file at `path` starts as a sound archive of the Wii (RSAR).
bool IsWiiArchive(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    std::array<char, 4> magic{};
    return file.read(magic.data(), magic.size()) && std::string_view(magic.data(), magic.size()) == "RSAR";
}

// Checks the wave sounds and the stream sounds of the Wii's archive at `path`, or only the sound `only` (see the top of
// the file).
int CheckWii(const std::filesystem::path& path, const std::string& only)
{
    const std::string name = xsflib::Utf8(path.filename());
    std::shared_ptr<const rsar::SoundArchive> archive;
    std::map<uint32_t, std::vector<uint8_t>> streams;
    std::optional<ntdlib::SndBuild> read;
    ntdlib::SndBuild build;
    try
    {
        std::vector<uint8_t> bytes;
        if (!xsflib::ReadFile(path, bytes))
        {
            throw std::runtime_error("the archive can't be read");
        }
        archive = hokuto::song::LoadWiiArchive(std::move(bytes), name, ntdlib::rvl::snd::kRsarOptions);
        streams = hokuto::song::ReadStreams(*archive, path);
        std::vector<uint8_t> dol;
        const std::optional<std::filesystem::path> found = hokuto::song::FindMainDol(path);
        read = found && xsflib::ReadFile(*found, dol) ? hokuto::song::ReadWiiBuild(dol) : std::nullopt;
        build = read.value_or(ntdlib::sequence_export::BuildOfRsarVersion(archive->Version()));
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "%s: %s\n", name.c_str(), error.what());
        return 2;
    }

    const double kRate = ntdlib::rvl::ax::kSampleRate;
    const double kAxFrameSamples = ntdlib::rvl::ax::kSamplesPerFrame;
    WaveSoundTotals totals;
    StreamTotals stream_totals;
    for (uint32_t sound = 0; sound < archive->Sounds().size(); sound++)
    {
        const rsar::SoundInfo& info = archive->Sounds()[sound];
        if (!only.empty() && info.name != only)
        {
            continue;
        }
        if (info.type == rsar::SoundType::kStream)
        {
            stream_totals.sounds++;
            const std::optional<uint32_t> file = hokuto::song::StreamFileOf(*archive, sound);
            const auto found = file ? streams.find(*file) : streams.end();
            if (found == streams.end())
            {
                stream_totals.absent++;
                continue;
            }

            rsar::StreamFile stream;
            sar::Pcm pcm;
            try
            {
                stream = rsar::StreamFile::Read(found->second);
                pcm = rsar::DecodeStream(stream);
            }
            catch (const std::exception& error)
            {
                std::printf("%s: %s: %s\n", name.c_str(), info.name.c_str(), error.what());
                stream_totals.failed++;
                continue;
            }

            // The tracks that the build's player plays. A track's voice plays the track's first two channels. The Wii's
            // stream sounds have no pitch.
            const std::optional<std::vector<rvl::StrmPlayer::Track>> tracks =
                rvl::StrmPlayer::TracksOf(build, info, archive->Version(), stream);
            if (!tracks)
            {
                std::printf("%s: %s: the game's build of NW4R SND plays no track of the stream\n", name.c_str(),
                            info.name.c_str());
                stream_totals.refused++;
                continue;
            }
            std::vector<PlayedTrack> played;
            for (const rvl::StrmPlayer::Track& track : *tracks)
            {
                if (track.active)
                {
                    const std::size_t count = std::min<std::size_t>(track.channels.size(), 2);
                    played.push_back(
                        {{track.channels.begin(), track.channels.begin() + static_cast<std::ptrdiff_t>(count)},
                         track.volume,
                         track.pan,
                         0});
                }
            }
            const std::vector<uint8_t>& bytes = found->second;
            const auto run_for = [&](double seconds)
            {
                const auto limit = static_cast<std::size_t>(seconds * kRate / kAxFrameSamples) + 8;
                const auto start_stream = [sound, &bytes](rvl::Engine& engine)
                {
                    return engine.StartStreamSound(sound, bytes) != nullptr;
                };
                return RunWiiModel(*archive, build, limit, start_stream);
            };
            CheckStreamSound(name, info.name, pcm, stream.sample_rate, info.volume, 1.0, played,
                             rvl::StrmPlayer::kMaxChannels, true, kRate, run_for, !only.empty(), stream_totals);
            continue;
        }
        if (info.type != rsar::SoundType::kWaveSound)
        {
            continue;
        }

        totals.wave_sounds++;
        WaveSoundFacts facts;
        sar::Pcm pcm;
        try
        {
            facts = FactsOf(*archive, sound);
            pcm = hokuto::song::DecodeItemWave(*archive, facts.wave_archive, facts.wave);
        }
        catch (const std::exception&)
        {
            totals.missing++;
            continue;
        }
        const auto run_for = [&](double seconds)
        {
            const auto limit = static_cast<std::size_t>(seconds * kRate / kAxFrameSamples) + 8;
            return RunWiiModel(*archive, build, limit,
                               [sound](rvl::Engine& engine) { return engine.StartWaveSound(sound) != nullptr; });
        };
        CheckWaveSound(name, facts, pcm, info.volume, kRate, run_for, !only.empty(), totals);
    }

    const std::string build_text = !read                 ? "the build that the archive's version gives"
                                   : build.date > 0      ? std::to_string(build.date)
                                   : build.October2006() ? "2006"
                                                         : "a build without a banner";
    std::printf("%s: NW4R SND of %s, mixed at %.0f Hz\n", name.c_str(), build_text.c_str(), kRate);
    PrintWaveSounds(name, totals);
    PrintStreamSounds(name, stream_totals);

    return totals.failed + stream_totals.failed > 0 ? 1 : 0;
}

} // namespace

int main(int argc, char** argv)
{
    const std::vector<std::string> arguments = xsflib::console::Arguments(argc, argv);
    if (arguments.size() < 2 || arguments.size() > 3)
    {
        std::fprintf(stderr, "usage: hokuto_check_players <archive> [sound]\n");
        return 2;
    }
    const std::string only = arguments.size() > 2 ? arguments[2] : std::string();

    const std::filesystem::path path = xsflib::PathFromUtf8(arguments[1]);
    if (IsWiiArchive(path))
    {
        return CheckWii(path, only);
    }
    const std::string name = xsflib::Utf8(path.filename());
    std::shared_ptr<const sar::SoundArchive> archive;
    std::map<uint32_t, std::vector<uint8_t>> streams;
    try
    {
        archive = hokuto::song::OpenArchive(path);
        streams = hokuto::song::ReadStreams(*archive, path);
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "%s: %s\n", name.c_str(), error.what());
        return 2;
    }
    const bool wii_u = archive->ReadOptions().flavor == sar::Flavor::kCafe;
    const bool ctr = archive->ReadOptions().flavor == sar::Flavor::kCtr;
    const double rate = ctr     ? static_cast<double>(hokuto::song::CtrModel::kSampleRate)
                        : wii_u ? cafe::ArchiveModel::kSampleRate
                                : atk::ArchiveModel::kSampleRate;
    const double frame_samples = ctr     ? static_cast<double>(hokuto::song::CtrModel::kFrameSamples)
                                 : wii_u ? cafe::ArchiveModel::kFrameSamples
                                         : atk::ArchiveModel::kFrameSamples;

    // Runs the console's model for `seconds` at most with the sound that `start` starts.
    const auto run_model = [&](double seconds, const auto& start)
    {
        const auto limit = static_cast<std::size_t>(seconds * rate / frame_samples) + 8;
        if (ctr)
        {
            return RunCtrModel(*archive, limit, start);
        }

        return wii_u ? RunModel<cafe::ArchiveModel>(*archive, limit, start)
                     : RunModel<atk::ArchiveModel>(*archive, limit, start);
    };

    WaveSoundTotals waves;
    StreamTotals stream_totals;

    for (uint32_t sound = 0; sound < archive->Sounds().size(); sound++)
    {
        const sar::SoundInfo& info = archive->Sounds()[sound];
        if (!only.empty() && info.name != only)
        {
            continue;
        }

        if (info.type == sar::SoundType::kWaveSound)
        {
            waves.wave_sounds++;
            WaveSoundFacts facts;
            sar::Pcm pcm;
            try
            {
                facts = FactsOf(*archive, sound);
                pcm = hokuto::song::DecodeItemWave(*archive, facts.wave_archive, facts.wave);
            }
            catch (const std::exception&)
            {
                waves.missing++;
                continue;
            }
            const auto run_for = [&](double seconds)
            {
                return run_model(seconds, [sound](auto& engine) { return engine.StartWaveSound(sound) != nullptr; });
            };
            CheckWaveSound(name, facts, pcm, info.volume, rate, run_for, !only.empty(), waves);
        }
        else if (info.type == sar::SoundType::kStream)
        {
            stream_totals.sounds++;
            const std::optional<uint32_t> file = hokuto::song::StreamFileOf(*archive, sound);
            const auto found = file ? streams.find(*file) : streams.end();
            if (found == streams.end())
            {
                stream_totals.absent++;
                continue;
            }

            std::vector<uint8_t> bytes;
            sar::StreamFile stream;
            sar::Pcm pcm;
            try
            {
                const bool aac = hokuto::song::IsAdts(found->second);
                bytes = aac ? hokuto::song::PlayableStream(
                                  found->second,
                                  hokuto::song::AacStreamOf(*archive, sound).value_or(hokuto::song::AacStream()))
                            : found->second;
                stream = sar::StreamFile::Read(bytes);
                pcm = sar::DecodeStream(stream);
                stream_totals.aac += aac ? 1 : 0;
            }
            catch (const std::exception& error)
            {
                std::printf("%s: %s: %s\n", name.c_str(), info.name.c_str(), error.what());
                stream_totals.failed++;
                continue;
            }

            // The tracks that play. The 3DS plays 8 channels at most, and the other consoles 16.
            ntdlib::nwsnd::StreamTracks tracks;
            if (ctr)
            {
                const ntdlib::ctr::snd::StreamTracks played = ntdlib::ctr::snd::StreamTracksOf(info.stream, stream);
                tracks.valid = played.valid;
                tracks.tracks = played.tracks;
            }
            else
            {
                tracks = ntdlib::nwsnd::StreamTracksOf(info.stream, stream);
            }
            std::vector<PlayedTrack> played;
            for (std::size_t t = 0; t < tracks.tracks.size(); t++)
            {
                if ((tracks.valid >> t & 1) != 0)
                {
                    const sar::StreamTrackInfo& track = tracks.tracks[t];
                    played.push_back({track.channels, track.volume, track.pan, track.span});
                }
            }
            const std::size_t most_channels =
                ctr ? std::min<std::size_t>(info.stream.channel_count,
                                            ntdlib::ctr::snd::StreamSoundPlayer::kMaxChannels)
                    : 16;
            const auto run_for = [&](double seconds)
            {
                const auto start_stream = [sound, &bytes](auto& engine)
                {
                    return engine.StartStreamSound(sound, bytes) != nullptr;
                };
                return run_model(seconds, start_stream);
            };
            CheckStreamSound(name, info.name, pcm, stream.sample_rate, info.volume,
                             ctr ? 1.0 : static_cast<double>(info.stream.pitch), played, most_channels,
                             ctr || info.pan_curve == 0, rate, run_for, !only.empty(), stream_totals);
        }
    }

    PrintWaveSounds(name, waves);
    PrintStreamSounds(name, stream_totals);

    return stream_totals.failed + waves.failed > 0 ? 1 : 0;
}
