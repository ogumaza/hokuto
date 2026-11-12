// SPDX-License-Identifier: MIT

// A GBA ROM whose music Quintet's driver plays. ntdlib finds the driver in the ROM, with its revision and its tables
// (ntdlib::quintet::DetectDriver). A game can keep its music in several music files (ntdlib::quintet::ReadMusicFiles).
// Each file numbers its songs and sound effects after the file before, and Hokuto numbers the sounds the same way. A
// song has no name in the ROM: Hokuto names it by its number. ntdlib's performer plays a song on its model of the
// driver for the arrangement (ntdlib::sequence_export::quintet::PerformSong). ntdlib's ArchivePlayer plays it on its
// models of the driver and the GBA's sound hardware (song/sequence_model.h).
//
// A song has 6 channels, and each is a track: square 1, square 2, the wave channel, the noise channel, FIFO A and FIFO
// B. A channel's commands select its instrument: a duty, a wave, a noise macro or a sample (ntdlib/quintet/
// instruments.h). A sample is one of the game's PCM samples. Hokuto numbers the samples of each music file's two banks
// once each, in the order of the files, the banks and their numbers.

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
#include "ntdlib/quintet/driver.h"

#include "song/driver.h"

namespace hokuto::song
{

// A GBA ROM, and what ntdlib found of Quintet's driver in it. The functions below give what the overrides give.
struct QuintetRom final : DriverRom
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
    std::vector<uint32_t> TrackPrograms(int track) const override;
    Sequence ReadSequence(uint32_t sound) const override;
    ntdlib::sequence_export::Conversion ConvertToMidi(uint32_t sound, const std::string& program) const override;
    ntdlib::sequence_export::BankConversion ConvertSamples(std::span<const int> samples,
                                                           const std::string& program) const override;
    std::unique_ptr<DriverPlayer> MakePlayer() const override;

    // An audition plays a note on a channel of a song, with the instrument that the program of the Key Editor selects
    // (ntdlib's ArchivePlayer::StartNote). `DriverAudition::track` names the channel, and its setup selects the
    // instrument. The key of the note sets its index in the pitch table: 36 less.
    DriverAudition SampleAudition(int sample, int key, int velocity) const override;
    std::optional<DriverAudition> NoteAudition(const ntdlib::commands::Sequence& commands, const Sequence& sequence,
                                               int track, int program, int key, int velocity) const override;
    std::shared_ptr<const DriverRom> AuditionRom(const DriverAudition& audition) const override;

    // The music file of song `song` (numbered across the whole game), and the number of the song in the file. Nothing
    // for a number past the songs.
    std::optional<std::pair<const ntdlib::quintet::DriverInfo*, int>> FileOf(int song) const;

    // A sample of a bank of a music file.
    struct SampleKey
    {
        std::size_t file = 0;
        int bank = 0;
        int number = 0;
    };

    // A note of an audition.
    struct AuditionNote
    {
        int song = 0;
        int channel = 0;
        uint8_t instrument = 0;
        int index = 24;
        uint8_t volume = 15;
    };

    ntdlib::agb::Rom rom;
    std::vector<ntdlib::quintet::DriverInfo> files; // as ntdlib::quintet::ReadMusicFiles returns them
    std::vector<SampleKey> samples;
    std::vector<uint32_t> waves;          // the waves that the songs' wave channels select, in order
    std::optional<AuditionNote> audition; // where the ROM's one sound plays an audition
};

// The rules of revision `revision` of Quintet's driver in a Hokuto file (format/quintet.h).
const DriverRules& QuintetRules(uint8_t revision);

// `rom` as a ROM with Quintet's driver, or nothing for another driver's ROM.
const QuintetRom* QuintetOf(const DriverRom* rom);

// The GBA ROM in `bytes`. Throws xsflib::FormatError if the bytes show no sign of Quintet's driver, or if its music
// files contain no song that plays.
std::shared_ptr<const QuintetRom> LoadQuintetRom(std::vector<uint8_t> bytes);

// The GBA ROM in `bytes` with only its driver and music files read: enough for ReadSequence, without the browser's
// samples and waves. Throws xsflib::FormatError as LoadQuintetRom does for a ROM without the driver.
std::shared_ptr<const QuintetRom> LoadQuintetSongs(std::vector<uint8_t> bytes);

// The most volume (5E) that channel `track` plays in revision `start` of the driver: 15 on a square and the noise
// channel, 1 on a FIFO, and on the wave channel the last of the revision's wave levels (ntdlib's
// RevisionTraits::wave_levels): 3, or 4 in the J revision and where the revision isn't known. Above it, ntdlib's model
// writes another level to the hardware: the level of a square or of the noise channel wraps in its 4 bits, the level of
// a FIFO sets the bit of the other FIFO, and the wave channel keeps the code before.
int QuintetMostVolume(int track, std::optional<ntdlib::quintet::Revision> start);

} // namespace hokuto::song
