// SPDX-License-Identifier: MIT

// The files of a mixdown: a render as a WAV file at a chosen rate and sample format, and the files of one sound of a
// batch (song/batch.h), the whole mix and its stems. The Export Audio Mixdown window and hok mixdown fill in
// MixdownSettings, and render each sound through RenderSoundFiles.

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "song/mixdown.h"
#include "song/sequence.h"
#include "song/sequence_model.h"

namespace hokuto::song
{

// The samples of a WAV file: 16-bit or 24-bit PCM, or 32-bit floating point.
enum class SampleFormat
{
    kPcm16,
    kPcm24,
    kFloat32,
};

// How a render becomes a WAV file: its rate (0 keeps the render's), its sample format, whether its peak goes to full
// scale, and whether 16-bit samples get a dither.
struct WavForm
{
    friend bool operator==(const WavForm&, const WavForm&) = default;

    int rate = 0;
    SampleFormat format = SampleFormat::kPcm16;
    bool normalise = false;
    bool dither = false;
};

// A WAV file of `samples`, interleaved stereo at `rate` frames a second with full scale at 1.0, in `form`.
// - Another rate goes through the resampler (audio::ResampleFrames). A loop then moves to the nearest frames of the new
//   rate. The frames after its end repeat its start, and the filter then reads across the jump back.
// - Normalising scales the samples so that the largest reaches full scale, after the resampler.
// - PCM rounds each sample to the nearest step and clamps it to the format's range. The console's 16-bit samples
//   come out as they are. A dither adds noise of a triangular spread of 1 step either way before 16-bit rounding: plain
//   TPDF, the same noise for each file. Floating point keeps every sample, past full scale too.
// - A loop goes in a smpl chunk: a forward loop from its start to the frame before its end.
// Throws std::invalid_argument for samples that don't fit in a RIFF file.
std::vector<uint8_t> MixdownWav(std::span<const float> samples, int rate, const WavForm& form,
                                const std::optional<MixdownLoop>& loop = std::nullopt);

// The tracks of `sequence` that play notes, by their numbers: the tracks that get a stem.
std::vector<int> StemTracks(const Sequence& sequence);

// How a mixdown renders and writes each sound: the range, the mix, what stems keep, and the files' form.
struct MixdownSettings
{
    friend bool operator==(const MixdownSettings&, const MixdownSettings&) = default;

    int loops = kMixdownLoops;
    double fade_seconds = kMixdownFadeSeconds;
    double tail_seconds = kMixdownTailSeconds;
    bool seamless = false;             // a seamless loop in place of the fade (SeamlessRange)
    std::optional<MixdownRange> range; // none renders the whole song (SongRange or SeamlessRange)
    MixdownMix mix;                    // the whole mix: its muted tracks, the aux buses' volumes, Stereo Out
    StemEffects stem_effects = StemEffects::kEffects;
    WavForm form;
};

// The range of `sequence` that `settings` render.
MixdownRange RangeOf(const Sequence& sequence, const MixdownSettings& settings);

// A file of a sound: the whole mix, or with `track`, the stem of that track.
struct MixdownFile
{
    friend bool operator==(const MixdownFile&, const MixdownFile&) = default;

    std::string name;
    std::optional<int> track;
};

// Gives a new model of the sequence for each render, dry for a dry stem.
using MakeMixdownModel = std::function<std::shared_ptr<SequenceModel>(bool dry)>;

// Takes a rendered file's name and bytes.
using MixdownWrite = std::function<void(const std::string& name, std::span<const uint8_t> bytes)>;

// What RenderSoundFiles wrote: the count of files, whether a render stopped at kMixdownLimitSeconds or cut tails at
// their limit, and whether the caller stopped it.
struct SoundFiles
{
    std::size_t files = 0;
    bool limited = false;
    bool tails_cut = false;
    bool cancelled = false;
};

// Renders `sequence` with `settings` into each of `files`, in order. Each file goes to `write` as soon as it's
// rendered. `progress` follows each render, from 0 for each one.
SoundFiles RenderSoundFiles(const MakeMixdownModel& make_model, const Sequence& sequence,
                            std::span<const MixdownFile> files, const MixdownSettings& settings,
                            const MixdownWrite& write, const MixdownProgress& progress = {});

} // namespace hokuto::song
