// SPDX-License-Identifier: MIT

// The lengths that a file keeps for players (format::SoundLength). The length analysis measures them (song/lengths.h).

#pragma once

#include <cstddef>
#include <functional>
#include <optional>

#include "ntdlib/rsar/formats.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sound_build.h"

#include "format/file.h"

namespace hokuto::edit
{

// Called after each sound that WithLengths measures, with the sounds measured and the sounds to measure in all. It
// returns false to stop the measure. The measuring threads call it, one at a time.
using LengthProgress = std::function<bool(std::size_t measured, std::size_t sounds)>;

// The most sounds that WithLengths measures at once: one on each of the processor's threads. Each holds a model of the
// console.
std::size_t MostLengthThreads();

// `file` with the lengths of its sequence sounds, in place of the lengths that it held. `archive` is the archive that
// the file builds. Each sound plays with the file's effects, at its buses' volumes. A sound of the 3DS plays without
// them (song::MeasureLength). `threads` sounds are measured at once, and MostLengthThreads at most. A sound that the
// model can't play gets no lengths. Returns nothing once `progress` stops the measure.
std::optional<format::File> WithLengths(const format::File& file, const ntdlib::sar::SoundArchive& archive,
                                        std::size_t threads, const LengthProgress& progress = {});

// The same for a file of the Wii. `archive` is the archive that the file builds, read by
// ntdlib::rvl::snd::kRsarOptions, and each sound plays on the model of `build` of NW4R SND (format::BuildOfWiiArchive).
std::optional<format::File> WithLengths(const format::File& file, const ntdlib::rsar::SoundArchive& archive,
                                        const ntdlib::SndBuild& build, std::size_t threads,
                                        const LengthProgress& progress = {});

} // namespace hokuto::edit
