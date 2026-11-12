// SPDX-License-Identifier: MIT

// A GBA ROM whose music Krawall plays: Sebastian Kienzl's player of XM and S3M modules. ntdlib finds the player's build
// and tables and the game's modules in the ROM (ntdlib::krawall::DetectDriver). A sound is a piece that the conversion
// numbers: a module, or one of the songs that its orders separate (DriverInfo::songs). A piece has no name in the ROM:
// Hokuto names it by its number. ntdlib's performer plays a piece on its model of the player for the arrangement
// (ntdlib::sequence_export::krawall::PerformSong), and ntdlib's ArchivePlayer plays it on its models of the player and
// the GBA's sound hardware (song/sequence_model.h).
//
// A sequence contains the patterns of a module, and each of the module's first 16 channels is a track. A note names its
// instrument: in an S3M module, a sample of the game's table, and in an XM module, an instrument that maps each note to
// such a sample. A note's program is the sample that it plays. The modified build loads each module from a packed music
// file. Its modules stay as data in a file: Hokuto plays and exports them, and doesn't edit them.

#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "ntdlib/agb/rom.h"
#include "ntdlib/commands/sequence.h"
#include "ntdlib/krawall/driver.h"

#include "song/driver.h"

namespace hokuto::song
{

// A GBA ROM, and what ntdlib found of Krawall in it.
struct KrawallRom final : DriverRom
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

    // An audition plays a note of an instrument of the module of a piece through ntdlib's ArchivePlayer::StartNote. The
    // setup's first command names the piece, and its second the instrument. The Key Editor's key 60 plays C-4: note 49.
    DriverAudition SampleAudition(int sample, int key, int velocity) const override;
    std::optional<DriverAudition> NoteAudition(const ntdlib::commands::Sequence& commands, const Sequence& sequence,
                                               int track, int program, int key, int velocity) const override;
    std::shared_ptr<const DriverRom> AuditionRom(const DriverAudition& audition) const override;

    // The ROM that contains the modules: the unpacked copy in the modified build, or else the ROM. ntdlib's readers of
    // the modules, their instruments and samples, and its performer and converter take it in place of the ROM. In the
    // modified build, the modules are past the end of the ROM.
    const ntdlib::agb::Rom& ModulesRom() const;

    // The sample of the game's table that `instrument` (from 1) plays at note `note` (from 1 for C-0) in the module of
    // piece `sound`, or nothing.
    std::optional<int> SampleOf(uint32_t sound, uint16_t instrument, uint8_t note) const;

    // A note of an audition.
    struct AuditionNote
    {
        int song = 0;
        uint16_t instrument = 1;
        uint8_t note = 49;
    };

    ntdlib::agb::Rom rom;
    ntdlib::krawall::DriverInfo info;
    std::vector<int> samples;             // the samples of the game's table that the modules play, in order
    std::optional<AuditionNote> audition; // where the ROM's one sound plays an audition
};

// The rules of Krawall in a Hokuto file: revision 1 for the builds from 2004/09/17 on, and 0 for the earlier ones
// (format/drivers.h).
const DriverRules& KrawallRules(uint8_t revision);

// `rom` as a ROM with Krawall, or nothing for another driver's ROM.
const KrawallRom* KrawallOf(const DriverRom* rom);

// The GBA ROM in `bytes`. Throws xsflib::FormatError if the bytes show no sign of Krawall, or if the game has no piece
// of music.
std::shared_ptr<const KrawallRom> LoadKrawallRom(std::vector<uint8_t> bytes);

} // namespace hokuto::song
