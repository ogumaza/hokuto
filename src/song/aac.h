// SPDX-License-Identifier: MIT

// AAC streams. A stream sound of the 3DS can play an AAC file beside the archive in place of a stream file: AAC-LC
// frames in ADTS, such as BGM_TITLE.aac. The sound's entry marks the stream as AAC, and gives its loop in samples
// (song::AacStreamOf in streams.h). The system's decoder decodes the file, and the system's encoder makes a new one:
// Audio Toolbox on the Mac and Media Foundation on Windows. Each frame gives 1,024 samples, from the file's first frame
// on. The decoders' samples can differ between the two systems in their last bits. They can also differ by a step
// between two decodings on one system. Audio Toolbox's did. The step followed where the process kept its buffers.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "ntdlib/wave/decode.h"

namespace hokuto::song
{

// Whether `bytes` start as an ADTS frame: the sync word, and layer 0.
bool IsAdts(std::span<const uint8_t> bytes);

// The number of ADTS frames in `bytes`, from the start up to the first byte that doesn't start a whole frame.
uint32_t AdtsFrames(std::span<const uint8_t> bytes);

// `bytes`, an ADTS stream of AAC, decoded by the system's decoder: every channel at the stream's rate, 1,024 samples a
// frame, without a loop. Throws std::runtime_error with a message for the user when the bytes aren't such a stream, or
// when the system can't decode them.
ntdlib::wave::Pcm DecodeAdts(std::span<const uint8_t> bytes);

// The rates of bits a second that Hokuto's AAC streams take. Media Foundation's encoder takes these alone.
inline constexpr std::array<uint32_t, 4> kAacBitRates = {96000, 128000, 160000, 192000};

// The channels of `pcm` encoded by the system's encoder as AAC-LC in ADTS, at `bit_rate` bits a second, one of
// kAacBitRates. The loop stays out. The encoder's priming comes before the samples (EncoderDelay), and the last frame
// ends with silence. Each frame's header holds no CRC. The encoders take 1 or 2 channels at 44,100 or 48,000 Hz: Media
// Foundation's takes no others. Throws std::runtime_error with a message for the user for other samples, and when the
// system can't encode them.
std::vector<uint8_t> EncodeAdts(const ntdlib::wave::Pcm& pcm, uint32_t bit_rate);

// Whether EncodeAdts keeps the rate of bits constant. Audio Toolbox's encoder keeps a constant rate in the mode that
// EncodeAdts sets. Each ADTS header adds 7 bytes to the encoder's packets: 2.6 kbit/s at 48,000 Hz. Media Foundation's
// encoder offers no choice of the rate's control. Its rate is a target.
bool EncodesConstantRate();

// The samples that the system's decoder gives before the first sample that the system's encoder encoded, at `rate` with
// `channels`: the encoder's priming. Apple documents 2,112 samples for Audio Toolbox. Media Foundation's encoder puts
// none, and Hokuto puts 2,112 samples of silence there on Windows. Hokuto measures the priming on each system: it
// encodes a burst of noise, decodes it, and finds where the burst comes back. Throws as EncodeAdts.
uint32_t EncoderDelay(uint32_t rate, std::size_t channels);

} // namespace hokuto::song
