// SPDX-License-Identifier: MIT

// A GBA ROM whose songs MusicPlayer2000 (MP2K) plays. ntdlib finds the driver in the ROM, with its song table, its
// music players and the settings of its mixer (ntdlib::mp2k::DetectDriver). A song has no name in the ROM: Hokuto names
// it by its number in the song table. ntdlib's performer plays a song on its model of the driver for the arrangement
// (ntdlib::sequence_export::mp2k::PerformSong), and ntdlib's ArchivePlayer plays it on its models of the driver and the
// GBA's sound hardware (song/sequence_model.h).

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "ntdlib/agb/rom.h"
#include "ntdlib/mp2k/driver.h"
#include "ntdlib/mp2k/song.h"

namespace hokuto::song
{

// A GBA ROM, and what ntdlib found of MP2K's driver in it.
struct GbaRom
{
    ntdlib::agb::Rom rom;
    ntdlib::mp2k::DriverInfo info;
};

// The GBA ROM in `bytes`. Throws xsflib::FormatError if the bytes show no sign of MP2K, or if no song of its song table
// plays (GbaSongs).
std::shared_ptr<const GbaRom> LoadGbaRom(std::vector<uint8_t> bytes);

// The songs of the song table that play: those whose header ntdlib reads and whose music player plays at least one of
// their tracks, in the order of their numbers. Two numbers can name the same header. Each one counts.
std::vector<int> GbaSongs(const GbaRom& rom);

// The name that Hokuto gives song `song` of `rom`: "Song" and its number, with as many digits as the largest number of
// the table has, and two at least.
std::string GbaSongName(const GbaRom& rom, int song);

// The most tracks that the music player of each of the first `songs` songs of `rom`'s song table plays, by the song's
// number. A song plays the tracks of its header up to that count (ntdlib::mp2k::SongPlayer).
std::vector<uint32_t> GbaPlayerTracks(const GbaRom& rom, std::size_t songs);

// The rate of the console's output for the songs of `rom`, from the resolution that the driver sets in the sound bias
// register: 32,768 Hz at 9 bits, and 65,536 Hz at the other resolutions. The model of the hardware puts out the higher
// rates' samples at 65,536 Hz (ntdlib::agb::Apu).
int GbaRate(const GbaRom& rom);

// A voice group that songs of a ROM play: its address, the programs that the songs' program changes select in it, and
// the songs that name it in their headers. Every note of a song plays its voice group: a note's program is its voice
// there (ReadSequence).
struct GbaVoiceGroup
{
    friend bool operator==(const GbaVoiceGroup&, const GbaVoiceGroup&) = default;

    uint32_t address = 0;
    std::vector<int> programs; // in order
    std::vector<int> songs;    // in the order of their numbers
};

// The voice groups of the songs that play (GbaSongs), in the order of their addresses. A song's program changes come
// from its decoded commands (ntdlib::mp2k::DecodeSong). Where the decoder keeps a song's bytes as data, or can't decode
// them, the programs that ntdlib's performer selects as it plays the song serve instead.
std::vector<GbaVoiceGroup> GbaVoiceGroups(const GbaRom& rom);

// The songs of `after` whose voice groups play other voices than in `before`, a copy of the ROM before edits of its
// voices: the bytes of a program's voice differ, or of a voice of the table of a key split or a drum kit, or of the
// pattern of a wave voice. A key also plays otherwise when the key table of its split names another voice. The programs
// of each group that the songs select in `before` or in `after` count (GbaVoiceGroups), at every key of their tables. A
// row of a table can read other data as a voice, and no note uses such a row. The word of a song's header that names a
// voice group that moved therefore doesn't count, and neither does a pattern past the end of `before`. The songs come
// in the order of their numbers.
std::vector<int> GbaSongsWithOtherVoices(const GbaRom& before, const GbaRom& after);

// Whether `voice` plays a sample of 4-bit steps. A later version of Camelot's mixer plays a fixed-pitch sample of a
// negative size that way. ntdlib's ReadWave refuses such a sample, and its bytes stay among the ROM's other bytes.
bool GbaVoicePlaysSteps(const GbaRom& rom, const ntdlib::mp2k::Voice& voice);

// Whether `voice`, a voice that no key split or drum kit contains, plays something: a PSG voice of channels 1 to 4, a
// synth that ntdlib's converter recognises, a sample that ntdlib reads with samples, or a sample of 4-bit steps.
bool GbaVoiceSounds(const GbaRom& rom, const ntdlib::mp2k::Voice& voice);

// The name that the Bank Editor shows for the kind of `voice`: "Sample", "Fixed sample", "Square 1", "Square 2",
// "Wave", "Noise", "Key split" or "Drum kit". A PSG voice of another channel is "Unknown".
std::string GbaVoiceKind(const ntdlib::mp2k::Voice& voice);

// Whether the 128 entries of the table at `address` of `rom` contain only voices that play something (GbaVoiceSounds)
// and entries of 12 zero bytes. The songs select the programs in `selected`. Those programs can contain anything. A
// game's table can have fewer entries, with other data after them. A table that Hokuto wrote contains only such
// entries.
bool GbaTableHoldsVoices(const GbaRom& rom, uint32_t address, std::span<const int> selected = {});

// The programs of `group` that the Bank Editor lists and the inspector offers, in order: those that the songs select,
// and, in a table that contains only voices (GbaTableHoldsVoices), every entry that plays something. Nothing shows what
// the bytes after a game's shorter table contain.
std::vector<int> GbaListedPrograms(const GbaRom& rom, const GbaVoiceGroup& group);

// The byte of the memory that memacc commands read (ntdlib::mp2k::Sequencer::Memory) that releases the note of an
// audition ROM (GbaAuditionRom): 1 releases it.
inline constexpr int kAuditionRelease = 0;

// A copy of `rom` whose song table contains one song: a note of `key` at `velocity` on voice `program` of the voice
// group of song `song`. The song's bytes and its table go after the ROM's bytes. Its track sets full volume and the
// centre, and holds the note until byte kAuditionRelease of the memory is 1. The track then releases the note and ends.
// The song plays on song `song`'s music player, with its priority and reverb. The browser and the Key Editor's Acoustic
// Feedback play notes this way (song/preview.h). Throws std::runtime_error if song `song`'s header can't be read.
std::shared_ptr<const GbaRom> GbaAuditionRom(const GbaRom& rom, int song, int program, int key, int velocity);

// The name that Hokuto gives the voice group at `address`: "Voice group" and its address, such as
// "Voice group 0x081A2B3C". A voice group has no name in the ROM.
std::string GbaVoiceGroupName(uint32_t address);

} // namespace hokuto::song
