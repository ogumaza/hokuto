// SPDX-License-Identifier: MIT

// The edits of a DS stream (STRM), for its properties, Replace… and File > Import > Stream…. The HEAD block of a stream
// file contains the encoding, whether the stream loops, its channels, its rate, a timer, the loop's start and the
// samples in each channel, then how the samples are laid out in blocks. The DATA block contains the blocks, the part of
// each channel in turn. A part of IMA-ADPCM starts with 4 bytes of the decoder's state: the channel can start at any
// block, and a loop starts at a block. The timer also determines the rate: 16,756,991 / the rate / 32. The file can
// take a new size (edit/ds_files.h).

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "ntdlib/sdat/formats.h"

#include "edit/files.h"
#include "format/file.h"
#include "song/ds.h"

namespace hokuto::edit
{

// The highest rate of a stream: the 16 bits of its field. The lowest is that of a wave (kLeastDsRate).
inline constexpr uint32_t kHighestDsStreamRate = 65535;

// A stream file of `channels`, one or two of the same length, at `rate`. A loop's start moves forward to a block's
// start, and the samples from the start of the loop to the start of the block repeat after the end: the loop plays as
// it did. Each block of IMA-ADPCM starts with the decoder state of the encoding of the whole channel, and each pass of
// the loop plays as the first. The timer is 16,756,991 / `rate` / 32, rounded down. Throws std::invalid_argument with a
// message for the user for other channels, a rate past kLeastDsRate or kHighestDsStreamRate, and a loop start at or
// past the end. ntdlib::sdat::EncodeStream encodes the stream after these checks.
std::vector<uint8_t> EncodeDsStream(std::span<const std::vector<int16_t>> channels, uint32_t rate,
                                    ntdlib::sdat::WaveEncoding encoding, std::optional<uint32_t> loop_start);

// The values of a DS stream that its properties edit. The loop's start counts samples. Without a loop, it is 0. Without
// an encoding, the stream keeps the one that it has.
struct DsStreamValues
{
    friend bool operator==(const DsStreamValues&, const DsStreamValues&) = default;

    uint32_t rate = 0;
    bool loop = false;
    uint32_t loop_start = 0;
    std::optional<ntdlib::sdat::WaveEncoding> encoding = std::nullopt;
};

// The file's ID of stream `stream` of `archive`. Throws std::invalid_argument with a message for the user for a stream
// that is missing from the archive.
uint32_t DsStreamFileId(const song::DsArchive& archive, uint32_t stream);

// The values of stream `stream` of `archive`. Throws std::invalid_argument with a message for the user for a stream
// that is missing from the archive or can't be read.
DsStreamValues DsStreamValuesOf(const song::DsArchive& archive, uint32_t stream);

// `file`, a file of a DS archive, with `values` for stream `stream`. `archive` is the archive that `file` builds. A new
// rate or loop changes only those fields of the stream's HEAD block, and the blocks stay. A new rate uses the timer of
// EncodeDsStream. The same rate keeps the rate and the timer that the stream has. A loop starts at a block before the
// end. Like EncodeDsStream, a new encoding makes the stream again from its samples. Throws std::invalid_argument with a
// message for the user for a rate past its limits, a loop that starts off a block or at the end, a stream that is
// missing from the archive, and as edit::WithDsFile does.
format::File WithDsStream(const format::File& file, const song::DsArchive& archive, uint32_t stream,
                          const DsStreamValues& values);

// `file` with `bytes`, a stream file, in the place of stream `stream`'s file. Throws std::invalid_argument with a
// message for the user for bytes that aren't a stream file, a stream that is missing from the archive, and as
// edit::WithDsFile does.
format::File WithDsStreamBytes(const format::File& file, const song::DsArchive& archive, uint32_t stream,
                               std::span<const uint8_t> bytes);

// What stream `stream` contains, for the Replace window. Throws std::invalid_argument with a message for the user as
// DsStreamValuesOf does.
WaveChanges ChangesOfDsStream(const song::DsArchive& archive, uint32_t stream);

// Whether `bytes` start as a stream file of the DS does: with STRM.
bool IsDsStreamFile(std::span<const uint8_t> bytes);

} // namespace hokuto::edit
