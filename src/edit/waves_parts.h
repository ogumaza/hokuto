// SPDX-License-Identifier: MIT

// What waves.cpp and wii_waves.cpp share: the sizes of samples, the models that a wave edit keeps, and the check of a
// model. Only those files include it.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

#include "ntdlib/sar/formats.h"

#include "format/file.h"

namespace hokuto::edit::waves_parts
{

namespace sar = ntdlib::sar;

// The bytes of each frame of DSP-ADPCM.
inline constexpr std::size_t kFrameBytes = 8;

// The bytes of each sample of a PCM encoding.
std::size_t SampleBytes(sar::WaveEncoding encoding);

// The models of the waves of wave archive file `file_id` in `from`, but for wave `wave`.
void KeepOtherModels(const format::File& from, uint32_t file_id, std::optional<uint32_t> wave, format::File& to);

// Throws std::invalid_argument with a message for the user for a model that format::WaveModelProblem refuses. A game's
// wave can loop from its end to its end. A model can't hold that loop, and the message says how to change it.
void RequireWaveModel(const format::WaveModel& model);

} // namespace hokuto::edit::waves_parts
