// SPDX-License-Identifier: MIT

// Converts samples to another sample rate: for a WAV file that goes in as a wave at a lower rate (edit/files.h), and
// for a WAV file that goes in as an AAC stream at the stream's rate (edit/streams.h). A low-pass filter takes out what
// the lower rate can't hold: a sinc in Kaiser's window. It passes up to 90 % of the lower rate's Nyquist frequency, and
// holds what lies from the Nyquist frequency up about 90 dB down. Each new sample comes from the filter at its time in
// the old samples. The same filter brings a render's frames to another rate for the Export window's measure
// (measure/measure.h).
//
// A looped wave keeps its loop. The intro and the loop each take a whole number of new samples, and each stretches by
// less than a sample to fit them. The loop's end then meets its start, and the filter reads the loop's start again
// after the loop's end. This is how the console plays the loop. Before the first sample, and after the last of a wave
// without a loop, the filter reads silence.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace hokuto::audio
{

// The place at `to` samples a second of sample `sample` at `from` samples a second, rounded to the nearest sample.
std::size_t ResampledPlace(std::size_t sample, uint32_t from, uint32_t to);

// `samples`, a channel at `from` samples a second, at `to` samples a second, another rate. Without a loop, the new
// samples last as long, to the nearest sample, and one sample at least. A loop from `loop_start` to the end of
// `samples` starts at ResampledPlace(`loop_start`) and lasts as long, to the nearest sample, and one sample at least.
// Throws std::invalid_argument for a rate of 0, the same rate twice, no samples, and a loop start at or past the end.
std::vector<int16_t> Resample(std::span<const int16_t> samples, uint32_t from, uint32_t to,
                              std::optional<std::size_t> loop_start = std::nullopt);

// `samples`, frames of `channels` interleaved channels at `from` frames a second, at `to` frames a second. The new
// frames last as long, to the nearest frame. Before the first frame and after the last, the filter reads silence.
// Frames at the same rate come back as they are. Throws std::invalid_argument for a rate of 0, no channels, and samples
// that don't fill their frames.
std::vector<float> ResampleFrames(std::span<const float> samples, std::size_t channels, uint32_t from, uint32_t to);

} // namespace hokuto::audio
