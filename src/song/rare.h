// SPDX-License-Identifier: MIT

// A GBA ROM whose music Rare's driver plays. ntdlib finds the driver in the ROM, with its build and its tables
// (ntdlib::rare::DetectDriver). A tune has no name in the ROM: Hokuto names it by its number. ntdlib's performer plays
// a tune on its model of the driver for the arrangement (ntdlib::sequence_export::rare::PerformSong). ntdlib's
// ArchivePlayer plays it on its models of the driver and the GBA's sound hardware (song/sequence_model.h).
//
// The driver's songs are the tunes of its tune table. One tune of a game can play two arrangements, one above water and
// one under water (ntdlib::rare::DriverInfo::water). Each arrangement is a version of the song, and each version is a
// sound. Sound `song` is the song's first version. The sounds after the songs are the other versions, in the order of
// their songs.
//
// A tune header names a program map and an instrument table. They give the tune's instruments. A sample is an
// instrument that plays a sample of 8-bit PCM: Hokuto numbers each such instrument of the tunes once, in the order of
// the tunes, their programs and the keys of their drum kits and key splits.

#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/agb/rom.h"
#include "ntdlib/commands/sequence.h"
#include "ntdlib/rare/driver.h"

#include "song/driver.h"

namespace hokuto::song
{

// A GBA ROM, and what ntdlib found of Rare's driver in it.
struct RareRom final : DriverRom
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
    std::vector<DriverBankSlot> BankSlots(uint32_t sound) const override;
    Sequence ReadSequence(uint32_t sound) const override;
    ntdlib::sequence_export::Conversion ConvertToMidi(uint32_t sound, const std::string& program) const override;
    ntdlib::sequence_export::BankConversion ConvertSamples(std::span<const int> samples,
                                                           const std::string& program) const override;
    std::unique_ptr<DriverPlayer> MakePlayer() const override;

    // An audition plays a lone note of a program of a tune on the driver's model (ntdlib's ArchivePlayer::StartNote).
    // `DriverAudition::track` names the tune whose program map contains the program, and the setup selects the program.
    DriverAudition SampleAudition(int sample, int key, int velocity) const override;
    std::optional<DriverAudition> NoteAudition(const ntdlib::commands::Sequence& commands, const Sequence& sequence,
                                               int track, int program, int key, int velocity) const override;
    std::shared_ptr<const DriverRom> AuditionRom(const DriverAudition& audition) const override;

    // The tune that sound `sound` plays, and the version: 1 for the arrangement under water.
    int SongOf(uint32_t sound) const;
    int VersionOf(uint32_t sound) const;

    // The address of each sample instrument, once each (see the top of this file). A sample's number is its place here.
    std::vector<uint32_t> sample_instruments;

    // Each sound's tune and version, by the sound's number.
    std::vector<std::pair<int, int>> sounds;

    // The note that sound 0 plays in a copy of AuditionRom.
    struct AuditionNote
    {
        int tune = 0;
        uint8_t program = 0;
        uint8_t key = 60;
        uint8_t velocity = 127;
    };
    std::optional<AuditionNote> audition;

    ntdlib::agb::Rom rom;
    ntdlib::rare::DriverInfo info;
};

// The rules of a build of Rare's driver, by the bits of the file's revision (format/rare.h).
const DriverRules& RareRules(uint8_t revision);

// `rom` as a ROM with Rare's driver, or nothing for another driver's ROM.
const RareRom* RareOf(const DriverRom* rom);

// The GBA ROM in `bytes`. Throws xsflib::FormatError if the bytes show no sign of the driver, or if its tune table
// contains no tune that plays.
std::shared_ptr<const RareRom> LoadRareRom(std::vector<uint8_t> bytes);

} // namespace hokuto::song
