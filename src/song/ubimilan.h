// SPDX-License-Identifier: MIT

// A GBA ROM whose music Ubisoft Milan's driver plays. ntdlib finds the driver's sound bank and its tables in the ROM
// (ntdlib::ubimilan::DetectDriver). A sound is a piece of music of the bank: the sequences that it plays one after
// another, and whether it plays them again. A piece has no name in the ROM: Hokuto names it by its number. ntdlib's
// performer plays a piece on its model of the driver for the arrangement
// (ntdlib::sequence_export::ubimilan::PerformSong), and ntdlib's ArchivePlayer plays it on its models of the driver and
// the GBA's sound hardware (song/sequence_model.h).
//
// A sequence has one track of commands for its 16 channels, and each command names its channel. A track of the
// arrangement is a channel. Channels 0 to 2 play the PSG's square 1, square 2 and wave channels. Channel 9 plays the
// kit's samples on the mixer's voices, and the PSG's noise at its lowest keys. The other channels play nothing. A note
// sounds until a note-off of its key, and a note of the kit until any note-off of channel 9. A tick is one of the
// console's frames.

#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "ntdlib/agb/rom.h"
#include "ntdlib/commands/sequence.h"
#include "ntdlib/ubimilan/driver.h"

#include "song/driver.h"

namespace hokuto::song
{

// A GBA ROM, and what ntdlib found of Ubisoft Milan's driver in it.
struct UbiMilanRom final : DriverRom
{
    const DriverRules& Rules() const override;
    std::span<const uint8_t> Bytes() const override;
    std::vector<uint32_t> Sounds() const override;
    uint32_t SoundCount() const override;
    std::string SoundName(uint32_t sound) const override;
    SoundFields FieldsOf(uint32_t sound) const override;
    int Rate() const override;
    std::vector<int> Samples() const override;
    std::string SampleName(int sample) const override;
    double SampleRate(int sample) const override;
    std::vector<uint8_t> SampleWav(int sample) const override;
    SampleShape ShapeOf(int sample) const override;
    std::vector<int> SamplesOfSound(uint32_t sound) const override;
    Sequence ReadSequence(uint32_t sound) const override;
    ntdlib::sequence_export::Conversion ConvertToMidi(uint32_t sound, const std::string& program) const override;
    ntdlib::sequence_export::BankConversion ConvertSamples(std::span<const int> samples,
                                                           const std::string& program) const override;
    std::unique_ptr<DriverPlayer> MakePlayer() const override;
    DriverAudition SampleAudition(int sample, int key, int velocity) const override;
    std::optional<DriverAudition> NoteAudition(const ntdlib::commands::Sequence& commands, const Sequence& sequence,
                                               int track, int program, int key, int velocity) const override;
    std::shared_ptr<const DriverRom> AuditionRom(const DriverAudition& audition) const override;

    // The sample resource that key `key` of the kit at offset `program` plays, or nothing when the key plays the noise
    // or no sample.
    std::optional<int> KitSample(int program, int key) const;

    // Throws std::invalid_argument if `sample` names no sample resource that can be read.
    void CheckSample(int sample) const;

    // The sample resources that the kits name, in order of their numbers.
    std::vector<int> sample_resources;

    ntdlib::agb::Rom rom;
    ntdlib::ubimilan::DriverInfo info;
};

// The rules of Ubisoft Milan's driver (song/driver.h).
const DriverRules& UbiMilanRules();

// `rom` as a ROM with Ubisoft Milan's driver, or nothing for another driver's ROM.
const UbiMilanRom* UbiMilanOf(const DriverRom* rom);

// The GBA ROM in `bytes`. Throws xsflib::FormatError if the bytes show no sign of the driver, or if its sound bank
// contains no piece of music.
std::shared_ptr<const UbiMilanRom> LoadUbiMilanRom(std::vector<uint8_t> bytes);

// The velocity from 1 to 127 that the Key Editor shows for a note command's velocity byte on sequence channel
// `channel`, and back. A note of the kit plays its sample at the byte's volume, 64 to the whole scale. The byte of a
// square or of the noise sets the top 4 bits of the envelope: the starting volume. The wave's byte sets the top 3 bits
// of NR32: bits 5 and 6 set the volume code, and bit 7 plays the wave at 75 % (ntdlib's WaveShare). The velocity is 127
// times the square root of the level, and 1 at least for a level above 0. ntdlib's converter writes the same velocity.
// Back from a velocity, the byte keeps the bits of `old`, the command's byte, that contain no level.
int UbiMilanVelocityOf(int channel, int key, int velocity);
int UbiMilanDriverVelocity(int channel, int key, int velocity, int old);

} // namespace hokuto::song
