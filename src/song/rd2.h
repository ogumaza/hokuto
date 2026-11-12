// SPDX-License-Identifier: MIT

// A GBA ROM whose music Nintendo R&D2's driver plays. ntdlib finds the driver in the ROM, with its revision, its tables
// and the settings that the game's code sets for each sequence (ntdlib::rd2::DetectDriver). A song has no name in the
// ROM: Hokuto names it by its number. ntdlib's performer plays a song on its model of the driver for the arrangement
// (ntdlib::sequence_export::rd2::PerformSong), and ntdlib's ArchivePlayer plays it on its models of the driver and the
// GBA's sound hardware (song/sequence_model.h).
//
// The driver's songs are its sequences, then its sound effects: each a track of one of the sound effects' sequences
// (ntdlib::rd2::SongCount). The game's code can play a sequence in more than one way, such as with a track muted. Each
// way is a version of the song (ntdlib::sequence_export::rd2::SongVersions), and each version is a sound. Sound `song`
// is the song's first version. The sounds after the songs are the other versions, in the order of their songs.

#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "ntdlib/agb/rom.h"
#include "ntdlib/commands/sequence.h"
#include "ntdlib/rd2/driver.h"

#include "song/driver.h"

namespace hokuto::song
{

// The samples that the notes of a ROM's sounds play, read once for each ROM (rd2.cpp).
struct Rd2Uses;
std::shared_ptr<Rd2Uses> MakeRd2Uses();

// A GBA ROM, and what ntdlib found of Nintendo R&D2's driver in it.
struct Rd2Rom final : DriverRom
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
    DriverAudition SampleAudition(int sample, int key, int velocity) const override;
    std::optional<DriverAudition> NoteAudition(const ntdlib::commands::Sequence& commands, const Sequence& sequence,
                                               int track, int program, int key, int velocity) const override;
    std::shared_ptr<const DriverRom> AuditionRom(const DriverAudition& audition) const override;

    // The song that sound `sound` plays, and the version (ntdlib::rd2::VersionSettings).
    int SongOf(uint32_t sound) const;
    int VersionOf(uint32_t sound) const;

    // Throws std::invalid_argument if `sample` names no sample that can be read.
    void CheckSample(int sample) const;

    // The driver's bank number for each slot in the list of banks of sound `sound`, up to the last slot that the
    // sequence of the sound selects (command C7). A note's preset is 128 times its bank plus its instrument.
    std::vector<uint16_t> BankListOf(uint32_t sound) const;

    // The address of each sample that the driver's sample sets name, once each, in the order of the sets. A sample's
    // number is its place here.
    std::vector<uint32_t> sample_addresses;

    // Each sound's song and version, by the sound's number.
    std::vector<std::pair<int, int>> sounds;

    ntdlib::agb::Rom rom;
    ntdlib::rd2::DriverInfo info;

    // The samples that each sound's notes play, filled on first use. Reading them takes up to a second. The audio
    // thread and the UI thread both read the ROM: a std::once_flag in the table guards the fill. A copy of the ROM
    // with other bytes requires a new table.
    std::shared_ptr<Rd2Uses> uses = MakeRd2Uses();
};

// The volume at which a track of the driver plays until a command sets it (command E0).
inline constexpr int kRd2StartVolume = 0x80;

// The rules of revision `revision` of Nintendo R&D2's driver (song/driver.h).
const DriverRules& Rd2Rules(ntdlib::rd2::Revision revision);

// `rom` as a ROM with Nintendo R&D2's driver, or nothing for another driver's ROM.
const Rd2Rom* Rd2Of(const DriverRom* rom);

// The GBA ROM in `bytes`. Throws xsflib::FormatError if the bytes show no sign of the driver, or if its tables contain
// no song that plays.
std::shared_ptr<const Rd2Rom> LoadRd2Rom(std::vector<uint8_t> bytes);

// The name that Hokuto gives bank `bank` of the driver's banks, such as "Bank 003".
std::string Rd2BankName(uint16_t bank);

// The velocity from 1 to 127 that the Key Editor shows for a note of the driver's velocity `velocity`, up to 255, and
// back: the nearest, and 1 at least for a velocity of 1 or more.
int Rd2VelocityOf(int velocity);
int Rd2DriverVelocity(int velocity);

} // namespace hokuto::song
