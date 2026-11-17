// SPDX-License-Identifier: MIT

// FFmpeg's program, for the AAC streams of systems without an AAC codec of their own (song/aac.h). Hokuto runs the
// ffmpeg program that the user installed, and links none of FFmpeg's code. On Linux, Hokuto feeds the program through
// pipes. On Windows, it feeds the program through two sockets on 127.0.0.1: Wine runs a Linux program from a Windows
// program, but gives it no pipes and no exit code.

#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "ntdlib/wave/decode.h"

namespace hokuto::song
{

// Whether Hokuto finds an ffmpeg program. On Linux, Hokuto looks in each folder of PATH. On Windows, it looks for
// ffmpeg.exe in each folder of PATH, then for a Linux program in /usr/bin, /usr/local/bin and /snap/bin through Wine's
// drive Z:.
bool FindsFfmpeg();

// The bytes that the ffmpeg program writes when it reads `input`: `input_options` come before the input in the
// program's arguments, and `output_options` before the output. Throws std::runtime_error with a message for the user
// when Hokuto finds no program, or when the program fails. `doing` names the work in the messages, such as "encode".
std::vector<uint8_t> RunFfmpeg(const std::vector<std::string>& input_options, std::span<const uint8_t> input,
                               const std::vector<std::string>& output_options, const char* doing);

// The messages of RunFfmpeg: no program found, and a program that failed, with `detail` after it where it isn't empty.
std::string FfmpegMissing(const char* doing);
std::string FfmpegFailed(const char* doing, const std::string& detail);

// `bytes`, an ADTS stream of AAC, decoded by FFmpeg, as song::DecodeAdts gives it. The caller has checked that the
// bytes start as an ADTS frame.
ntdlib::wave::Pcm DecodeAdtsWithFfmpeg(std::span<const uint8_t> bytes);

// The channels of `pcm` encoded by FFmpeg's AAC encoder, as song::EncodeAdts gives them. The encoder puts its own
// priming before the samples. The caller has checked the samples and the rate of bits (CheckEncodable).
std::vector<uint8_t> EncodeAdtsWithFfmpeg(const ntdlib::wave::Pcm& pcm, uint32_t bit_rate);

} // namespace hokuto::song
