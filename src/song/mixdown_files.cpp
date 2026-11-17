// SPDX-License-Identifier: MIT

#include "song/mixdown_files.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "seqlib/pcm.h"
#include "seqlib/wav.h"

#include "audio/resampler.h"

namespace hokuto::song
{
namespace
{

// The WAVE format tags of PCM and of floating point.
constexpr uint16_t kFormatPcm = 1;
constexpr uint16_t kFormatFloat = 3;

void PutFourcc(std::vector<uint8_t>& out, std::string_view fourcc)
{
    out.insert(out.end(), fourcc.begin(), fourcc.end());
}

void PutLittle(std::vector<uint8_t>& out, uint32_t value, std::size_t bytes)
{
    for (std::size_t i = 0; i < bytes; i++)
    {
        out.push_back(static_cast<uint8_t>(value >> (8 * i)));
    }
}

// `sample` scaled to a PCM sample of `bits`, rounded to the nearest step, half away from zero, and clamped.
int32_t PcmSample(float sample, int bits)
{
    const double full = std::ldexp(1.0, bits - 1);
    const double scaled = std::round(static_cast<double>(sample) * full);
    if (std::isnan(scaled))
    {
        return 0;
    }

    return static_cast<int32_t>(std::clamp(scaled, -full, full - 1.0));
}

// The size of a smpl chunk with one loop.
constexpr uint32_t kSamplerSize = 36 + 24;

// Appends a smpl chunk in the form that seqlib's WAV files use: the sample period in nanoseconds, middle C as the unity
// note, and one forward loop from frame `start` to frame `end` - 1 that repeats without end.
void PutSampler(std::vector<uint8_t>& out, uint32_t rate, const MixdownLoop& loop)
{
    PutFourcc(out, "smpl");
    PutLittle(out, kSamplerSize, 4);
    PutLittle(out, 0, 4); // manufacturer
    PutLittle(out, 0, 4); // product
    PutLittle(out, static_cast<uint32_t>((1'000'000'000ull + rate / 2) / rate), 4);
    PutLittle(out, 60, 4); // unity note
    PutLittle(out, 0, 4);  // pitch fraction
    PutLittle(out, 0, 4);  // SMPTE format
    PutLittle(out, 0, 4);  // SMPTE offset
    PutLittle(out, 1, 4);  // loops
    PutLittle(out, 0, 4);  // sampler data
    PutLittle(out, 0, 4);  // the loop's identifier
    PutLittle(out, 0, 4);  // forward
    PutLittle(out, static_cast<uint32_t>(loop.start), 4);
    PutLittle(out, static_cast<uint32_t>(loop.end - 1), 4);
    PutLittle(out, 0, 4); // fraction
    PutLittle(out, 0, 4); // plays without end
}

// A stereo WAV file of 24-bit PCM or 32-bit floating point, with `loop` in a smpl chunk. A floating-point file contains
// a fact chunk with its count of frames. The format requires one in every format but PCM.
std::vector<uint8_t> DeepWav(std::span<const float> samples, uint32_t rate, SampleFormat format,
                             const std::optional<MixdownLoop>& loop)
{
    constexpr uint32_t kChannels = 2;
    const bool floating = format == SampleFormat::kFloat32;
    const uint32_t width = floating ? 4 : 3;
    const uint64_t data = static_cast<uint64_t>(samples.size()) * width;
    const uint32_t fact = floating ? 12 : 0;
    const uint32_t sampler = loop ? 8 + kSamplerSize : 0;
    const uint64_t riff = 4 + (8 + 18) + fact + 8 + data + (data % 2) + sampler;
    if (riff > std::numeric_limits<uint32_t>::max())
    {
        throw std::invalid_argument("the samples don't fit in a RIFF file");
    }

    std::vector<uint8_t> out;
    out.reserve(static_cast<std::size_t>(riff) + 8);
    PutFourcc(out, "RIFF");
    PutLittle(out, static_cast<uint32_t>(riff), 4);
    PutFourcc(out, "WAVE");

    // The fmt chunk of WAVEFORMATEX, with no extra bytes.
    PutFourcc(out, "fmt ");
    PutLittle(out, 18, 4);
    PutLittle(out, floating ? kFormatFloat : kFormatPcm, 2);
    PutLittle(out, kChannels, 2);
    PutLittle(out, rate, 4);
    PutLittle(out, rate * kChannels * width, 4);
    PutLittle(out, kChannels * width, 2);
    PutLittle(out, width * 8, 2);
    PutLittle(out, 0, 2);
    if (floating)
    {
        PutFourcc(out, "fact");
        PutLittle(out, 4, 4);
        PutLittle(out, static_cast<uint32_t>(samples.size() / kChannels), 4);
    }

    PutFourcc(out, "data");
    PutLittle(out, static_cast<uint32_t>(data), 4);
    for (const float sample : samples)
    {
        PutLittle(out, floating ? std::bit_cast<uint32_t>(sample) : static_cast<uint32_t>(PcmSample(sample, 24)),
                  width);
    }
    if (data % 2 != 0)
    {
        out.push_back(0);
    }
    if (loop)
    {
        PutSampler(out, rate, *loop);
    }

    return out;
}

} // namespace

std::vector<uint8_t> MixdownWav(std::span<const float> samples, int rate, const WavForm& form,
                                const std::optional<MixdownLoop>& loop)
{
    std::optional<MixdownLoop> kept =
        loop && loop->start < loop->end && loop->end <= samples.size() / 2 ? loop : std::nullopt;
    const int to = form.rate > 0 ? form.rate : rate;
    std::vector<float> changed;
    if (to != rate)
    {
        // A loop's frames follow its end once more. The filter then reads across the jump back. The new loop starts
        // and ends at the nearest frames of the new rate, and the file ends with it.
        changed.assign(samples.begin(), samples.end());
        if (kept)
        {
            changed.insert(changed.end(), samples.begin() + static_cast<std::ptrdiff_t>(kept->start * 2),
                           samples.begin() + static_cast<std::ptrdiff_t>(kept->end * 2));
        }
        changed = audio::ResampleFrames(changed, 2, static_cast<uint32_t>(rate), static_cast<uint32_t>(to));
        if (kept)
        {
            const auto from = static_cast<uint32_t>(rate);
            const auto at = static_cast<uint32_t>(to);
            kept = MixdownLoop{.start = audio::ResampledPlace(kept->start, from, at),
                               .end = std::max(audio::ResampledPlace(kept->end, from, at),
                                               audio::ResampledPlace(kept->start, from, at) + 1)};
            changed.resize(std::min(changed.size(), kept->end * 2));
        }
    }
    if (form.normalise)
    {
        if (changed.empty())
        {
            changed.assign(samples.begin(), samples.end());
        }
        float peak = 0.0f;
        for (const float sample : changed)
        {
            peak = std::isfinite(sample) ? std::max(peak, std::abs(sample)) : peak;
        }
        if (peak > 0.0f)
        {
            for (float& sample : changed)
            {
                sample /= peak;
            }
        }
    }
    if (to != rate || form.normalise)
    {
        samples = changed;
    }

    if (form.format != SampleFormat::kPcm16)
    {
        return DeepWav(samples, static_cast<uint32_t>(to), form.format, kept);
    }

    // The dither's noise comes from a fixed seed. A file then comes out the same each time.
    uint32_t state = 0x9E3779B9u;
    const auto noise = [&state]
    {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return static_cast<double>(state) / 4294967296.0;
    };
    const auto pcm16 = [&](float sample)
    {
        if (!form.dither)
        {
            return static_cast<int16_t>(PcmSample(sample, 16));
        }
        const double first = noise();
        const double second = noise();
        const double dithered = static_cast<double>(sample) * 32768.0 + first - second;
        return static_cast<int16_t>(std::isnan(dithered) ? 0.0 : std::clamp(std::round(dithered), -32768.0, 32767.0));
    };

    seqlib::Pcm pcm;
    pcm.channels.assign(2, std::vector<int16_t>(samples.size() / 2));
    for (std::size_t i = 0; i < samples.size() / 2; i++)
    {
        pcm.channels[0][i] = pcm16(samples[i * 2]);
        pcm.channels[1][i] = pcm16(samples[i * 2 + 1]);
    }
    if (kept)
    {
        pcm.loop = true;
        pcm.loop_start = static_cast<uint32_t>(kept->start);
        pcm.loop_end = static_cast<uint32_t>(kept->end);
    }

    return seqlib::WavFile(pcm, static_cast<uint32_t>(to));
}

std::vector<int> StemTracks(const Sequence& sequence)
{
    std::vector<int> tracks;
    for (const Track& track : sequence.tracks)
    {
        if (!track.notes.empty())
        {
            tracks.push_back(track.number);
        }
    }

    std::ranges::sort(tracks);
    tracks.erase(std::ranges::unique(tracks).begin(), tracks.end());

    return tracks;
}

MixdownRange RangeOf(const Sequence& sequence, const MixdownSettings& settings)
{
    if (settings.range)
    {
        return *settings.range;
    }

    return settings.seamless ? SeamlessRange(sequence, settings.tail_seconds)
                             : SongRange(sequence, settings.loops, settings.fade_seconds, settings.tail_seconds);
}

SoundFiles RenderSoundFiles(const MakeMixdownModel& make_model, const Sequence& sequence,
                            std::span<const MixdownFile> files, const MixdownSettings& settings,
                            const MixdownWrite& write, const MixdownProgress& progress)
{
    // Every file of a seamless loop ends where the whole mix ends (MixdownRange::seamless_end). Stems alone render the
    // whole mix first, to find that end.
    MixdownRange range = RangeOf(sequence, settings);
    SoundFiles written;
    if (range.seamless && !files.empty() && files.front().track)
    {
        const Mixdown whole = RenderMixdown(make_model(false), range, settings.mix, progress);
        if (whole.cancelled)
        {
            written.cancelled = true;
            return written;
        }
        if (whole.loop)
        {
            range.seamless_end = whole.loop->end;
        }
    }
    for (const MixdownFile& file : files)
    {
        const MixdownMix mix = file.track ? StemMix(*file.track, settings.mix, settings.stem_effects) : settings.mix;
        const bool dry = file.track && settings.stem_effects == StemEffects::kDry;
        const Mixdown mixdown = RenderMixdown(make_model(dry), range, mix, progress);
        if (mixdown.cancelled)
        {
            written.cancelled = true;
            break;
        }

        if (range.seamless && !range.seamless_end && mixdown.loop)
        {
            range.seamless_end = mixdown.loop->end;
        }
        write(file.name, MixdownWav(mixdown.samples, mixdown.sample_rate, settings.form, mixdown.loop));
        written.files++;
        written.limited = written.limited || mixdown.limited;
        written.tails_cut = written.tails_cut || mixdown.tails_cut;
    }

    return written;
}

} // namespace hokuto::song
