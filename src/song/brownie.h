// SPDX-License-Identifier: MIT

// A GBA ROM whose music and sound effects Brownie Brown's driver plays. ntdlib finds the driver in the ROM, with its
// revision and its tables (ntdlib::brownie::DetectDriver). A sound has no name in the ROM: Hokuto names it by its
// number. ntdlib's performer plays a sound on its model of the driver for the arrangement
// (ntdlib::sequence_export::brownie::PerformSong), and ntdlib's ArchivePlayer plays it on its models of the driver and
// the GBA's sound hardware (song/sequence_model.h).
//
// The driver's song table names each sound's list of channels. Each channel is a track of the same number: tracks 0 to
// 7 play the PSG's channels, and tracks from 8 on play samples. The sounds share their commands. The commands of each
// group of sounds that share bytes are one sequence (ntdlib::brownie::FindRegions), and a sound's list starts at an
// item of its sequence.
//
// A note, a rest, a hold and a release each wait for the channel's length. A length byte chooses that length from the
// driver's table. The bytes of a note hold no length: the note sounds until the channel's next note, rest or release.

#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "ntdlib/agb/rom.h"
#include "ntdlib/brownie/commands.h"
#include "ntdlib/brownie/driver.h"
#include "ntdlib/commands/sequence.h"

#include "song/driver.h"
#include "song/sequence.h"

namespace hokuto::song
{

// A sample of the driver: its 8-bit points from `start` up to `end`, going back to `loop` at the end, unless `loop` is
// `end`.
struct BrownieSample
{
    friend bool operator==(const BrownieSample&, const BrownieSample&) = default;

    uint32_t start = 0;
    uint32_t end = 0;
    uint32_t loop = 0;
};

// A GBA ROM, and what ntdlib found of Brownie Brown's driver in it.
struct BrownieRom final : DriverRom
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

    // The driver's number of sound `sound`, or -1 for a number that names none.
    int SongOf(uint32_t sound) const;

    // Throws std::invalid_argument if `sample` names no sample.
    void CheckSample(int sample) const;

    // The decoded sequence that contains the list of sound `sound`, and the item where the list starts. Nothing for a
    // sound that is in no region of the ROM.
    struct ListPlace
    {
        uint32_t region = 0;
        uint32_t list = 0;
    };
    std::optional<ListPlace> ListOf(uint32_t sound) const;

    // The sample sets of revision 0, or the samples of revision 1, that the sounds' commands select (command E1), by
    // their numbers in the driver's table.
    std::vector<uint32_t> SampleSelections() const;

    // The driver's samples: those of each sample set that the commands select, once each, in the order of the sets, or
    // in revision 1 each sample that they select, by its number in the driver's table. A sample's number in `samples`
    // is its number for Hokuto.
    std::map<int, BrownieSample> samples;

    // The regions of the ROM's sounds as ntdlib decodes them, and where each sound's list starts.
    std::vector<ntdlib::brownie::Decoded> regions;
    std::map<int, ListPlace> lists;

    // The driver's number of each sound, by the sound's number. The ROM's sounds keep their numbers.
    std::vector<int> sounds;

    // The settings with which a sample channel first plays each sample set, or in revision 1 each sample, by its number
    // in the driver's table: the commands that the channel ran before its first note of the set or the sample. An
    // audition of a sample starts with them.
    std::map<uint32_t, std::vector<ntdlib::commands::Command>> audition_setups;

    ntdlib::agb::Rom rom;
    ntdlib::brownie::DriverInfo info;
};

// The rules of revision `revision` of Brownie Brown's driver (song/driver.h).
const DriverRules& BrownieRules(ntdlib::brownie::Revision revision);

// `rom` as a ROM with Brownie Brown's driver, or nothing for another driver's ROM.
const BrownieRom* BrownieOf(const DriverRom* rom);

// The GBA ROM in `bytes`. Throws xsflib::FormatError if the bytes show no sign of the driver, or if its tables contain
// no sound that plays.
std::shared_ptr<const BrownieRom> LoadBrownieRom(std::vector<uint8_t> bytes);

// The kind of program that a note of track `track` of `revision` plays (command E1, F1 or FC), as the index of its
// Kind::kProgram (ntdlib/brownie/commands.h). Nothing for the noise channel. The noise channel selects no program.
std::optional<uint8_t> BrownieProgramIndex(ntdlib::brownie::Revision revision, int track);

// The commands that set the settings of track `track` where its note `note` starts: its programs, envelopes, volume,
// pans, transpose, tune and sweep, one of each kind and index. `commands` contains the commands of the sequence. The
// stretches of the track fall within them. Nothing where the commands can't be laid out at the revision's addresses.
std::optional<std::vector<ntdlib::commands::Command>> BrownieSettingsAt(ntdlib::brownie::Revision revision,
                                                                        const ntdlib::commands::Sequence& commands,
                                                                        const Track& track, const Note& note);

// The key that the arrangement shows for a note whose command's byte is `byte`, on track `track` of a ROM whose PSG's
// lowest key stands for MIDI key `base`, with the channel's transpose `transpose` in semitones. A noise note shows at
// its byte. A sample note of revision 0 from key 64 on plays key 47 (ntdlib::brownie::SampleKey).
int BrownieShownKey(ntdlib::brownie::Revision revision, int track, int base, int transpose, uint8_t byte);

} // namespace hokuto::song
