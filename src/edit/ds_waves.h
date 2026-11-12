// SPDX-License-Identifier: MIT

// The edits of a DS wave, for its properties and for Replace…: its rate, its loop and its encoding, and new bytes in
// its place. A wave of a wave archive (SWAR) starts with 12 bytes of parameters (SNDWaveParam): the encoding, whether
// the wave loops, the rate, the channel's timer, the loop's start and the loop's length. The channel reads a wave in
// 32-bit words, and the loop's start and length count words. The loop runs to the wave's end, and the end stays. A new
// rate or loop changes those bytes in place, in the run of the archive that contains the file of the wave archive
// (edit/native_runs.h). The archive keeps its size. A new encoding makes the wave again from its samples, and the file
// of the wave archive gets the new size (edit/ds_files.h).

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "ntdlib/sdat/formats.h"

#include "edit/files.h"
#include "format/file.h"
#include "song/ds.h"

namespace hokuto::edit
{

// The rates that a DS wave can take. The timer of a lower rate doesn't fit its 16 bits. A higher rate than the highest
// would have no use on a console that mixes at about 32,728 Hz.
inline constexpr uint32_t kLeastDsRate = 256;
inline constexpr uint32_t kHighestDsRate = 192000;

// Throws std::invalid_argument with a message for the user for a rate below kLeastDsRate or above kHighestDsRate, for
// a wave of `console` ("DS" or "DSE").
void CheckDsRate(uint32_t rate, std::string_view console);

// Whether waves at these rates take the same timer, and so play at the same pitch. The WAV file of a wave stores the
// rate that the timer sets, and the wave can store another rate with the same timer.
bool SameDsTimer(uint32_t first, uint32_t second);

// A wave's bytes (ntdlib::sdat::EncodeWave) of `samples` at `rate`, up to kHighestDsRate. The wave stores a rate in 16
// bits. Above 65,535, it stores the low 16 bits as the tools of the SDK write them. The timer then sets the rate.
// Throws std::invalid_argument as ntdlib::sdat::EncodeWave does.
std::vector<uint8_t> EncodeDsWave(std::span<const int16_t> samples, uint32_t rate, ntdlib::sdat::WaveEncoding encoding,
                                  std::optional<uint32_t> loop_start);

// The values of a DS wave that its properties edit. The rate is the one that song::DsWaveRate returns. The loop's start
// counts samples. Without a loop, it is 0. Without an encoding, the wave keeps the one that it has.
struct DsWaveValues
{
    friend bool operator==(const DsWaveValues&, const DsWaveValues&) = default;

    uint32_t rate = 0;
    bool loop = false;
    uint32_t loop_start = 0;
    std::optional<ntdlib::sdat::WaveEncoding> encoding = std::nullopt;
};

// The values of wave `wave` of wave archive `wave_archive` of `archive`. Throws std::runtime_error with a message for
// the user for a wave that is missing from the archive or can't be read.
DsWaveValues DsWaveValuesOf(const song::DsArchive& archive, uint32_t wave_archive, uint32_t wave);

// `file`, a file of a DS archive, with `values` for wave `wave` of wave archive `wave_archive`. `archive` is the
// archive that `file` builds. A new rate gets the timer that the tools of the SDK compute for it: 16,756,991 / the
// rate, rounded down. The same rate keeps the wave's rate and timer. A loop starts at a word before the wave's end.
// Without a loop, the start of the loop is the first word of samples of the wave. The tools write it the same way. A
// new encoding encodes the wave's samples again (ntdlib::sdat::EncodeWave). A loop's start inside a word of it then
// moves forward to the next word's start. The edit reaches every wave archive whose file contains the wave. Throws
// std::invalid_argument with a message for the user for a rate past kLeastDsRate or kHighestDsRate, a loop that starts
// off a word or at the end, a wave that is missing from the archive, and bytes that no run of the archive contains.
format::File WithDsWave(const format::File& file, const song::DsArchive& archive, uint32_t wave_archive, uint32_t wave,
                        const DsWaveValues& values);

// The same, with `starts`, the starts of the pieces of `file`'s native layout (format::NativePieceStarts).
format::File WithDsWave(const format::File& file, const song::DsArchive& archive, uint32_t wave_archive, uint32_t wave,
                        const DsWaveValues& values, std::span<const uint64_t> starts);

// `file` with `bytes`, a wave's parameters and samples (ntdlib::sdat::WaveBytes), in the place of wave `wave` of wave
// archive `wave_archive`, or after its waves when `wave` is their number. The other waves keep their bytes, and their
// indexes. Throws std::invalid_argument with a message for the user for a wave past the waves, for a file whose waves
// aren't laid out as the tools lay them out, and as edit::WithDsFile does.
format::File WithDsWaveBytes(const format::File& file, const song::DsArchive& archive, uint32_t wave_archive,
                             uint32_t wave, std::span<const uint8_t> bytes);

// `file` with `waves`, each a wave's parameters and samples, after the waves of wave archive `wave_archive`. Throws
// std::invalid_argument as WithDsWaveBytes does.
format::File WithDsNewWaves(const format::File& file, const song::DsArchive& archive, uint32_t wave_archive,
                            std::span<const std::vector<uint8_t>> waves);

// What wave `wave` of wave archive `wave_archive` contains, for the Replace window: its encoding, its rate, its loop,
// its size in the file and the banks that play it. Throws std::runtime_error with a message for the user for a wave
// that is missing from the archive or can't be read.
WaveChanges ChangesOfDsWave(const song::DsArchive& archive, uint32_t wave_archive, uint32_t wave);

// Whether `bytes` start as a wave file of the DS does: with SWAV.
bool IsDsWaveFile(std::span<const uint8_t> bytes);

} // namespace hokuto::edit
