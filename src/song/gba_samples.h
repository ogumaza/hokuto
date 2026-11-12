// SPDX-License-Identifier: MIT

// A sample of MusicPlayer2000 as a file, in the two forms in which GBA projects keep their samples. The `.bin` form
// contains the ROM's bytes: the 16-byte header, the samples of signed 8-bit PCM, and the one more that the mixer reads
// past the last. An AIFF file contains the same samples: one channel of 8 bits, with the rate in its COMM chunk, and
// the loop in its markers. Its frames count that last sample too, and the header's size counts one less. Compressed
// samples take neither form. A sample also exports as a WAV file.

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "ntdlib/mp2k/song.h"

#include "song/gba.h"

namespace hokuto::song
{

// The `.bin` form of the sample at `address` of `rom`. Throws std::invalid_argument with a message for the user for a
// sample that can't be read, or a compressed one.
std::vector<uint8_t> GbaSampleBin(const GbaRom& rom, uint32_t address);

// The AIFF form of the sample at `address` of `rom`. Throws as GbaSampleBin does.
std::vector<uint8_t> GbaSampleAiff(const GbaRom& rom, uint32_t address);

// A WAV file of the sample at `address` of `rom`: 16 bits, at the rate that key 60 plays, with the loop that the
// converter sets on a sample of a SoundFont. The last sample that the mixer reads stays out. Throws as GbaSampleBin
// does.
std::vector<uint8_t> GbaSampleWav(const GbaRom& rom, uint32_t address);

// The address of the sample that `voice` plays, where the two forms can store it: not a PSG voice, a key split or a
// drum kit, not a compressed sample, and not a header without samples.
std::optional<uint32_t> GbaVoiceSample(const GbaRom& rom, const ntdlib::mp2k::Voice& voice);

// The samples that the listed programs of the voice groups of `rom` play, through key splits and drum kits too, where
// the two forms can hold them (GbaVoiceSample), in the order of their addresses.
std::vector<uint32_t> GbaVoiceGroupSamples(const GbaRom& rom);

// Whether `bytes` read as a sample's file: an AIFF file, or a header that a `.bin` file's samples follow.
bool IsGbaSampleFile(std::span<const uint8_t> bytes);

// The sample of the file in `bytes`, in the `.bin` form. A loop in an AIFF file runs from the marker that its INST
// chunk names as the sustain loop's start, and its end marker ends the samples. Throws std::invalid_argument with a
// message for the user for bytes that are neither form, a compressed sample, and an AIFF file that doesn't contain one
// channel of 8 bits or has a rate that the header can't store.
std::vector<uint8_t> GbaSampleOfFile(std::span<const uint8_t> bytes);

} // namespace hokuto::song
