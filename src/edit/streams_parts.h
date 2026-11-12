// SPDX-License-Identifier: MIT

// What streams.cpp and wii_streams.cpp share: the stream files of the later consoles, a WAV file read for a stream, the
// checks of a stream's values, and the stream files of the Wii. Only those files include it.

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "ntdlib/rsar/stream_file.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sar/stream_file.h"

#include "edit/streams.h"

namespace hokuto::edit::streams_parts
{

namespace rsar = ntdlib::rsar;
namespace sar = ntdlib::sar;

// A stream file of the 3DS, the Wii U or the Switch: its console, the version in its header, its bytes up to the size
// in its header, and the stream.
struct LaterStream
{
    sar::Flavor console = sar::Flavor::kCafe;
    uint32_t version = 0;
    std::vector<uint8_t> bytes;
    sar::StreamFile stream;
};

// Reads `bytes` as a stream file of the 3DS, the Wii U or the Switch. Throws std::invalid_argument with a message for
// the user for bytes that aren't such a file, and a stream that ntdlib can't read.
LaterStream ReadLaterStream(std::span<const uint8_t> bytes);

// A WAV file's samples for a stream, up to the end of its loop, with the fields of the stream that come from the WAV
// file.
struct WavStream
{
    IncomingStream incoming;
    uint32_t rate = 0;
    std::vector<std::vector<int16_t>> channels;
    std::optional<uint32_t> loop_start;
};

// Reads `bytes` as a WAV file for a stream in `encoding`. Throws std::invalid_argument with a message for the user for
// another encoding than DSP-ADPCM and 16-bit PCM, bytes that aren't a WAV file that Hokuto reads, a file of no samples,
// and one of more channels than a stream holds.
WavStream ReadWavStream(std::span<const uint8_t> bytes, sar::WaveEncoding encoding);

// Gives `wav.incoming` the WAV file's samples, rate and loop again after Describe, with the samples by which the
// stream, whose loop starts at `loop_start`, moved the loop. With `decoded`, the stream's decoded channels, it also
// gives the ratio of signal to noise of DSP-ADPCM.
void FinishWavStream(WavStream& wav, uint32_t loop_start, const std::vector<std::vector<int16_t>>* decoded);

// Checks `values` for a stream of `samples` samples in each channel (StreamWithValues).
void CheckValues(const StreamValues& values, uint32_t samples);

// Reads `bytes` as a stream file of the later consoles. Throws std::invalid_argument with a message for the user that
// starts with `start` for a stream that ntdlib can't read.
sar::StreamFile ReadLater(std::span<const uint8_t> bytes, const char* start = "Can't read the stream. ");

// Reads `bytes` as a stream file of the Wii. Throws std::invalid_argument with a message for the user that starts with
// `start` for a stream that ntdlib can't read.
rsar::StreamFile ReadWii(std::span<const uint8_t> bytes, const char* start = "Can't read the stream. ");

// `bytes`, a stream file of the Wii, with `values` (StreamWithValues).
std::vector<uint8_t> WiiStreamWithValues(std::span<const uint8_t> bytes, const StreamValues& values);

} // namespace hokuto::edit::streams_parts
