// SPDX-License-Identifier: MIT

#include "song/mixdown.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

#include "song/player.h"

namespace hokuto::song
{
namespace
{

// The sequence's loop, when a pass of it lasts at least a tick.
std::optional<Loop> PlayedLoop(const Sequence& sequence)
{
    if (sequence.loop && sequence.loop->end > sequence.loop->start)
    {
        return sequence.loop;
    }

    return std::nullopt;
}

// The share of the way from `before` to `after` at which the position reaches `at`, from 0 to 1. A frame in which the
// position doesn't move reaches it at its start.
double ShareReaching(double before, double after, double at)
{
    if (after <= before)
    {
        return 0.0;
    }

    return std::clamp((at - before) / (after - before), 0.0, 1.0);
}

// The frames that a seamless loop's end compares after each candidate end: 4 of the model's frames. The candidates
// lie up to 2 of the model's frames either side of the end that the tick count gives.
constexpr std::size_t kSeamFrames = 4;
constexpr std::size_t kSeamReach = 2;

// The end of a seamless loop in `samples` that starts at stereo frame `start`. The tick count places the end at `end`,
// to within about a model's frame of `frame` stereo frames: the model starts the next pass's notes at the start of
// one of its frames, and a frame of some consoles lasts a fraction of a stereo frame more than `frame`. The render goes
// on into the next pass. Of the ends up to kSeamReach frames either side of `end`, the one after which the render
// repeats the loop's first frames most closely, by the sum of the squared differences, replaces `end` when it differs
// less than half as much. A loop without such a repeat, such as one whose voices change from pass to pass, keeps `end`.
std::size_t SeamlessEnd(const std::vector<float>& samples, std::size_t start, std::size_t end, std::size_t frame)
{
    const std::size_t compared = kSeamFrames * frame * 2;
    const auto difference = [&](std::size_t candidate)
    {
        double sum = 0.0;
        for (std::size_t i = 0; i < compared; i++)
        {
            const double d = static_cast<double>(samples[candidate * 2 + i]) - samples[start * 2 + i];
            sum += d * d;
        }
        return sum;
    };

    const double at_end = difference(end);
    std::size_t best = end;
    double least = at_end;
    const std::size_t reach = kSeamReach * frame;
    const std::size_t first = end > start + reach + frame ? end - reach : start + frame;
    for (std::size_t candidate = first; candidate <= end + reach && candidate * 2 + compared <= samples.size();
         candidate++)
    {
        const double sum = candidate == end ? at_end : difference(candidate);
        if (sum < least)
        {
            least = sum;
            best = candidate;
        }
    }

    return least < at_end / 4.0 ? best : end;
}

} // namespace

MixdownRange SongRange(const Sequence& sequence, int loops, double fade_seconds, double tail_seconds)
{
    const std::optional<Loop> loop = PlayedLoop(sequence);
    if (!loop)
    {
        return {.fade_seconds = std::max(fade_seconds, 0.0), .tail_seconds = std::max(tail_seconds, 0.0)};
    }

    // Pass n ends where the tick count reaches the loop's end n - 1 more times.
    const auto passes = static_cast<uint64_t>(std::max(loops, 1));
    const uint64_t pass = loop->end - loop->start;
    return {.start = 0, .fade_at = loop->end + (passes - 1) * pass, .fade_seconds = std::max(fade_seconds, 0.0)};
}

MixdownRange SeamlessRange(const Sequence& sequence, double tail_seconds)
{
    const std::optional<Loop> loop = PlayedLoop(sequence);
    if (!loop)
    {
        return SongRange(sequence, 1, kMixdownFadeSeconds, tail_seconds);
    }

    MixdownRange range = SongRange(sequence, 2, 0.0);
    range.seamless = true;

    return range;
}

MixdownRange LocatorRange(uint32_t left, uint32_t right, double fade_seconds)
{
    const auto [start, end] = std::minmax(left, right);
    return {.start = start, .fade_at = end, .fade_seconds = std::max(fade_seconds, 0.0)};
}

double MixdownSeconds(const Sequence& sequence, const MixdownRange& range)
{
    const std::optional<Loop> loop = PlayedLoop(sequence);
    double ms = 0.0;
    if (range.fade_at && loop && *range.fade_at > loop->end)
    {
        // The passes after the first each last as long as the loop.
        const double pass = MsOfTick(sequence, loop->end) - MsOfTick(sequence, loop->start);
        const double passes =
            static_cast<double>(*range.fade_at - loop->end) / static_cast<double>(loop->end - loop->start);
        ms = MsOfTick(sequence, loop->end) + passes * pass - MsOfTick(sequence, range.start);
    }
    else if (range.fade_at)
    {
        ms = MsOfTick(sequence, static_cast<double>(*range.fade_at)) - MsOfTick(sequence, range.start);
    }
    else
    {
        // The length counts the last release. The tails stop at their limit after the last note's end.
        const double end = MsOfTick(sequence, sequence.length);
        const double cut =
            MsOfTick(sequence, sequence.notes_end) + range.tail_seconds.value_or(kMixdownLimitSeconds) * 1000.0;
        ms = std::min(end, cut) - MsOfTick(sequence, range.start);
    }

    const double seconds = std::max(ms, 0.0) / 1000.0;
    const double fade = range.fade_at ? range.fade_seconds : 0.0;
    return seconds < kMixdownLimitSeconds ? seconds + fade : kMixdownLimitSeconds + range.fade_seconds;
}

MixdownMix StemMix(int track, const MixdownMix& mix, StemEffects effects)
{
    const uint32_t alone = track >= 0 && track < 32 ? 1u << track : 0u;
    return {.mutes = ~alone | mix.mutes,
            .bus_volumes = effects == StemEffects::kDry ? AuxVolumes{} : mix.bus_volumes,
            .gain = effects == StemEffects::kStereoOut ? mix.gain : 1.0f};
}

Mixdown RenderMixdown(const std::shared_ptr<SequenceModel>& model, const MixdownRange& range, const MixdownMix& mix,
                      const MixdownProgress& progress)
{
    // A fade past the loop's first pass requires the loop to play again: the cycle on the loop's locators. The
    // program's Cycle plays the loop the same way. The player ends a looped sequence at the end of a pass otherwise.
    const std::optional<Loop> loop = PlayedLoop(model->GetSequence());
    const auto clock = std::make_shared<PlaybackClock>(model->FrameSamples());
    if (loop && range.fade_at)
    {
        clock->SetCycle({.on = true, .start = loop->start, .end = loop->end});
    }
    clock->SetMutes(mix.mutes);
    clock->SetBusVolumes(mix.bus_volumes);

    Mixdown mixdown;
    mixdown.sample_rate = model->SampleRate();
    const double rate = mixdown.sample_rate;
    const std::size_t frame = model->FrameSamples();
    const auto limit = static_cast<std::size_t>(kMixdownLimitSeconds * rate);
    const auto tail = static_cast<std::size_t>(std::llround(range.tail_seconds.value_or(0.0) * rate));
    const auto tail_fade = std::min(tail, static_cast<std::size_t>(std::llround(kMixdownTailFadeSeconds * rate)));
    std::size_t fade = static_cast<std::size_t>(std::llround(range.fade_seconds * rate));
    // A seamless render plays on past its end, to choose the end (SeamlessEnd).
    const std::size_t seam = range.seamless && loop ? (kSeamFrames + kSeamReach) * frame : 0;

    // The ticks whose samples the render marks: the end of its range, and for a seamless loop, the end of the loop's
    // first pass. The render passes each at the share of the frame where the model's tick count reaches it. A seamless
    // loop's end uses the share of its start: the loop lasts a whole number of frames, until SeamlessEnd places the end
    // where the next pass repeats the loop's start.
    const std::array<std::optional<uint64_t>, 2> marked = {
        range.fade_at, range.seamless && loop ? std::optional<uint64_t>(loop->end) : std::nullopt};
    std::array<std::optional<std::size_t>, 2> marked_frames{};
    std::optional<double> pass_share;

    // The player writes a frame of the model at a time. The fade starts at the end of the range, or at the end of the
    // tails' time, or at the limit. The tails' time starts after the model's frame in which the sequence ends.
    SequencePlayer player(model, range.start, clock);
    std::optional<std::size_t> fade_start;
    double before = range.start;
    std::vector<float> block(frame * 2);
    while (true)
    {
        const std::size_t frames = mixdown.samples.size() / 2;
        if (fade_start && frames >= *fade_start + fade + (marked_frames[1] ? seam : 0))
        {
            break;
        }
        if (!fade_start && frames >= limit)
        {
            mixdown.limited = true;
            fade_start = frames;
            continue;
        }

        const std::size_t written = player.Render(block.data(), frame);
        mixdown.samples.insert(mixdown.samples.end(), block.begin(),
                               block.begin() + static_cast<std::ptrdiff_t>(written * 2));
        const double after = clock->ModelTicks();
        for (std::size_t m = 0; m < marked.size() && written > 0; m++)
        {
            if (marked[m] && !marked_frames[m] && after >= static_cast<double>(*marked[m]))
            {
                double share = ShareReaching(before, after, static_cast<double>(*marked[m]));
                if (m == 1)
                {
                    pass_share = share;
                }
                else if (marked[1] && pass_share)
                {
                    share = *pass_share;
                }
                marked_frames[m] =
                    frames + static_cast<std::size_t>(std::llround(share * static_cast<double>(written)));
            }
        }
        before = after;
        if (!fade_start && marked_frames[0])
        {
            fade_start = marked_frames[0];
        }
        if (!fade_start && !range.fade_at && range.tail_seconds && model->SequenceEnded())
        {
            fade_start = frames + written + tail - tail_fade;
            fade = tail_fade;
        }

        if (progress && !progress(mixdown.samples.size() / 2))
        {
            mixdown.cancelled = true;
            break;
        }
        if (written < frame)
        {
            break;
        }
    }

    // A seamless loop ends where the render repeats its start. The render then ends there.
    if (range.seamless && marked_frames[0] && marked_frames[1] && !mixdown.limited && !mixdown.cancelled &&
        mixdown.samples.size() >= (*marked_frames[0] + seam) * 2 && *marked_frames[0] > *marked_frames[1])
    {
        fade_start = range.seamless_end && *range.seamless_end > *marked_frames[1]
                         ? *range.seamless_end
                         : SeamlessEnd(mixdown.samples, *marked_frames[1], *marked_frames[0], frame);
    }

    // The fade falls in a straight line from full volume. xsflib's players fade the same way. The render ends where it
    // reaches silence, or where the output ended.
    if (fade_start)
    {
        const std::size_t end = std::min(mixdown.samples.size() / 2, *fade_start + fade);
        for (std::size_t i = *fade_start; i < end; i++)
        {
            const auto gain =
                static_cast<float>(1.0 - static_cast<double>(i - *fade_start) / static_cast<double>(fade));
            mixdown.samples[i * 2] *= gain;
            mixdown.samples[i * 2 + 1] *= gain;
        }
        mixdown.samples.resize(end * 2);
    }
    if (mix.gain != 1.0f)
    {
        for (float& sample : mixdown.samples)
        {
            sample *= mix.gain;
        }
    }

    // A seamless render keeps both passes as playback plays them. It loops the second. The jump back from its end
    // lands where the second pass starts, with the tails of the first pass sounding: the tails that the second pass
    // leaves at its end. The way into the loop from the first pass stays as it plays.
    if (range.seamless && marked_frames[1] && !mixdown.limited && !mixdown.cancelled &&
        mixdown.samples.size() / 2 > *marked_frames[1])
    {
        mixdown.loop = MixdownLoop{.start = *marked_frames[1], .end = mixdown.samples.size() / 2};
    }
    mixdown.tails_cut = fade_start && !range.fade_at && !mixdown.limited && fade > 0 &&
                        mixdown.samples.size() / 2 == *fade_start + fade;

    return mixdown;
}

} // namespace hokuto::song
