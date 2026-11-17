// SPDX-License-Identifier: MIT

// Renders a sequence to audio offline, faster than real time, through the SequencePlayer that plays it in the program.
// The render then contains the samples that playback writes with the same model, the same start, the same cycle, mutes
// and aux buses' volumes, sample for sample. The render ends where its range says, and a fade can follow.
//
// The range of a whole song follows the rippers' rule for a song's length. A song that loops plays its loop a number of
// times and then fades while the loop plays on. xsflib's rips play two passes and a fade of 10 s. A song without a loop
// plays to its end and on through its last notes' releases and the effects' tails. Playback with Cycle off plays the
// same. A range between the locators starts at the left locator and fades or stops at the right one.

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include "song/sequence.h"
#include "song/sequence_model.h"

namespace hokuto::song
{

// The passes of the loop and the fade of a whole song by default: xsflib's rule for a rip's length.
inline constexpr int kMixdownLoops = 2;
inline constexpr double kMixdownFadeSeconds = 10.0;

// The longest tail of a song that ends, by default: 10 s after the sequence's end. A tail that still sounds there fades
// over the last second of the 10 s.
inline constexpr double kMixdownTailSeconds = 10.0;
inline constexpr double kMixdownTailFadeSeconds = 1.0;

// The limit of a render: 600 s, the limit of xsflib's rips. A song whose clock stops, or one without a loop that never
// ends, fades there. A render stays in memory: a minute uses about 23 MB at 48 kHz.
inline constexpr double kMixdownLimitSeconds = 600.0;

// Where a render starts and ends. It starts at tick `start` of the arrangement.
// - With `fade_at`, it fades from the sample that plays that tick, over `fade_seconds`, and ends there. The tick counts
//   on past the loop's end as the loop plays again (SequenceModel::After). A fade of 0 s ends the render at that
//   sample.
// - Without `fade_at`, the render plays the sequence and its tails to their end. Playback with Cycle off does too.
//   With `tail_seconds`, the tails last at most that long after the model's frame in which the sequence ends, and fade
//   over their last second (kMixdownTailFadeSeconds) when they still sound there.
// - With `seamless`, the render plays to `fade_at`, the end of the loop's second pass, without a fade. Mixdown::loop
//   marks the second pass, from the point where the first pass ends. Its end falls within two of the model's frames of
//   where the tick count passes the second pass's end. The render places it where the next pass repeats the loop's
//   first frames most closely, when a place repeats them clearly more closely than the place that the tick count gives.
//   The place that the tick count gives lasts a whole number of the model's frames after the loop's start.
//   `seamless_end` sets the end, in stereo frames from the render's start, in place of that search: the stems of a
//   song use the end of its whole mix, so that every file of the song loops in step.
// A render that reaches the limit (kMixdownLimitSeconds) before its end fades from there.
struct MixdownRange
{
    friend bool operator==(const MixdownRange&, const MixdownRange&) = default;

    uint32_t start = 0;
    std::optional<uint64_t> fade_at;
    double fade_seconds = 0.0;
    std::optional<double> tail_seconds;
    bool seamless = false;
    std::optional<std::size_t> seamless_end;
};

// The range of the whole of `sequence`. A sequence that loops plays `loops` passes of its loop, from its start, and
// fades over `fade_seconds`. Fewer than 1 pass counts as 1. A sequence without a loop plays to its end, with tails of
// at most `tail_seconds`.
MixdownRange SongRange(const Sequence& sequence, int loops = kMixdownLoops, double fade_seconds = kMixdownFadeSeconds,
                       double tail_seconds = kMixdownTailSeconds);

// The range of `sequence` as a seamless loop: its intro and two passes of its loop, with the second pass looped. A
// sequence without a loop uses SongRange's range.
MixdownRange SeamlessRange(const Sequence& sequence, double tail_seconds = kMixdownTailSeconds);

// The range between locators `left` and `right`, ticks of the arrangement, with a fade of `fade_seconds` from the right
// one. Locators in the wrong order swap.
MixdownRange LocatorRange(uint32_t left, uint32_t right, double fade_seconds = 0.0);

// About how long a render of `sequence` over `range` lasts, in seconds, from the arrangement's tempos: the Export Audio
// Mixdown window shows it for each song. A song without a loop counts to its length, release included, and its tails
// up to their limit. A render can differ by a frame or two, and by the drift of a driver's tempo from the
// arrangement's. A render stops at kMixdownLimitSeconds and its fade.
double MixdownSeconds(const Sequence& sequence, const MixdownRange& range);

// What the program's playback sets on the player: the muted tracks, a bit for each track's number, and the aux
// buses' volumes (PlaybackClock::SetMutes and SetBusVolumes). Playback then sets the Stereo Out fader's gain on the
// output. A render applies it last, after the fade.
struct MixdownMix
{
    friend bool operator==(const MixdownMix&, const MixdownMix&) = default;

    uint32_t mutes = 0;
    AuxVolumes bus_volumes = kFullVolumes;
    float gain = 1.0f;
};

// What a stem keeps of the mix:
// - kDry: the track's voices alone, without the aux buses, and without the DS's capture units or a GBA driver's echo
//   (ModelOptions::dry). The model for the stem has to be dry too.
// - kEffects: with the track's sends to the aux buses, at the FX channels' volumes.
// - kStereoOut: with the sends and the Stereo Out fader's gain. The track then sounds as it does in the whole mix.
enum class StemEffects
{
    kDry,
    kEffects,
    kStereoOut,
};

// The mix of the stem of track `track`, from the whole mix `mix`: the track plays alone, every other track muted. A
// muted track goes on playing and keeps its voices. Each track therefore sounds in its stem as it does in the whole
// mix. A track that `mix` mutes stays muted.
MixdownMix StemMix(int track, const MixdownMix& mix = {}, StemEffects effects = StemEffects::kEffects);

// A loop in a render, in stereo frames: from `start` to the frame before `end`.
struct MixdownLoop
{
    friend bool operator==(const MixdownLoop&, const MixdownLoop&) = default;

    std::size_t start = 0;
    std::size_t end = 0;
};

// A render: interleaved stereo at the model's rate, full scale at 1.0 (SequenceModel::FloatOutput). A seamless render
// contains its loop. `limited` says that it faded at kMixdownLimitSeconds, `tails_cut` that its tails sounded on past
// their limit and faded there, and `cancelled` that the caller stopped it.
struct Mixdown
{
    std::vector<float> samples;
    int sample_rate = 0;
    std::optional<MixdownLoop> loop;
    bool limited = false;
    bool tails_cut = false;
    bool cancelled = false;
};

// Called after each frame of a render with the stereo frames rendered so far. Returning false stops the render.
using MixdownProgress = std::function<bool(std::size_t frames)>;

// Renders `range` of the sequence that `model` plays, with `mix`, through a SequencePlayer. A model that plays a loop
// with a fade goes round the loop as the game does (the cycle on the loop's locators). Every other render plays with
// Cycle off. The model is used up: give each render a new model. Throws what the model throws, such as
// std::bad_alloc.
Mixdown RenderMixdown(const std::shared_ptr<SequenceModel>& model, const MixdownRange& range,
                      const MixdownMix& mix = {}, const MixdownProgress& progress = {});

} // namespace hokuto::song
