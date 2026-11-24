// SPDX-License-Identifier: MIT

// The songs and the sound effects of a DS cartridge image that Procyon Studio's sound driver, DSE, plays: songs (smdl),
// wave banks (swdl) and banks of sound effects (sedl) in the image's file system or inside the game's packs. ntdlib's
// performer plays a song or a sound effect on its model of the driver for the arrangement
// (ntdlib::sequence_export::dse::Perform), and ntdlib's dse::Player plays it on its models of the driver and the DS's
// sound unit (song/sequence_model.h). The driver's build comes from the image's ARM9 binary and its overlays
// (DseBuildOf).
//
// The image's sounds are its songs and its sound effects, in the order of ntdlib::sequence_export::dse::ReadSequences:
// the songs, each with its wave bank and the main bank, then the slots of each bank of sound effects. A song without
// its bank is left out, with a warning.
//
// The length of the driver's tick comes from the game's settings, by the path of each file (ntdlib::dse::TickOf). A
// game that ntdlib doesn't list, and a path that the game's rows don't cover, take the driver's default tick of 10 ms.
//
// Some games start a song below full level (ntdlib::dse::GameStarts). Playback, the reads and the exports start each
// song in its state (DseSoundStart): the level of the sound's start in the file (format::SoundStart::dse_level), or for
// an image, the level that the game's code gives.
//
// Some games keep their files in version 0x402, the oldest that DSE's driver reads. ntdlib reads them as files of
// version 0x415, and its cut makes a song's tracks a sequence. An edit writes a bank of version 0x402 with
// ntdlib::dse::OldestBank, from the bank file's bytes (DseBank::oldest). An export of such a song or bank gives the
// file's bytes in version 0x402 (DseSet::oldest_songs and DseBank::oldest).

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ntdlib/dse/driver.h"
#include "ntdlib/dse/formats.h"
#include "ntdlib/dse/game_starts.h"
#include "ntdlib/dse/sets.h"
#include "ntdlib/sequence_export/convert.h"
#include "ntdlib/sequence_export/dse/sequence_input.h"
#include "ntdlib/wave/decode.h"

#include "format/starts.h"
#include "song/items.h"

namespace hokuto::song
{

// A wave bank of a set, the main bank that contains the waves of a bank without samples, or null, where the bank's file
// starts in the bytes that LoadDseSet read, and the length of the driver's tick for the bank's file. The tick sets
// the steps of the envelopes and the LFOs. `oldest` holds the bytes of a bank file of version 0x402, and is null for
// one of version 0x415.
struct DseBank
{
    std::shared_ptr<const ntdlib::dse::WaveBank> bank;
    std::shared_ptr<const ntdlib::dse::WaveBank> main_bank;
    uint64_t file_offset = 0;
    int tick_microseconds = ntdlib::dse::Driver::kTickMicroseconds;
    std::shared_ptr<const std::vector<uint8_t>> oldest;
};

// The DSE sounds of a cartridge image, and the build of the driver that plays them.
struct DseSet
{
    DseSet() = default;
    DseSet(const DseSet&) = delete;
    DseSet& operator=(const DseSet&) = delete;

    ntdlib::dse::Build build = ntdlib::dse::Build::kLater;
    std::vector<ntdlib::sequence_export::dse::SequenceInput> sounds; // by the sounds' numbers

    // Where each sound's tracks start in its file, by the sounds' numbers, in the layout of version 0x415 that the
    // driver's offsets count in (ntdlib::dse::TracksOffset).
    std::vector<uint32_t> tracks_offsets;

    // The bytes of each song file of version 0x402, by the sounds' numbers. Null for a song of version 0x415 and for a
    // sound effect.
    std::vector<std::shared_ptr<const std::vector<uint8_t>>> oldest_songs;

    // The length of the driver's tick in microseconds, by the sounds' numbers.
    std::vector<int> ticks;

    std::vector<ntdlib::dse::SongState> states; // the state in which each sound starts, by the sounds' numbers
    std::vector<DseBank> banks;                 // every wave bank of the files, in their order
    std::vector<std::string> warnings;          // the files that can't be read, and the songs without their banks
};

// The DSE sounds of the cartridge image `image`, from its files and from the files inside the game's packs
// (format::DseFilesOf). An image without the files of DSE has no sounds. A bank inside a compressed pack gives a
// DseBank::file_offset of 0. Each song starts at the level that the game's code gives it (format::DseGameStarts).
// Throws xsflib::FormatError for bytes that aren't a cartridge image of the DS.
std::shared_ptr<const DseSet> LoadDseSet(std::span<const uint8_t> image);

// The DSE sounds of `set`, a set of a game's DSE files (ntdlib::dse::Join), for `build` of the driver, in the game
// whose header gives `game_code`, with `starts`, the starts of a file. An empty code takes the driver's default tick
// for every file. A sound starts in the state that its start without a name gives, or else at full level.
// edit::DseSetOfFile loads the set of a file with the starts of the file. Throws xsflib::FormatError for a set's
// directory that doesn't fit the set.
std::shared_ptr<const DseSet> LoadDseSet(std::span<const uint8_t> set, ntdlib::dse::Build build,
                                         std::string_view game_code, std::span<const format::SoundStart> starts = {});

// The build of the driver in the code of the cartridge image `image`: its ARM9 binary and its overlays
// (ntdlib::dse::BuildOf). Bytes that aren't such an image, and a build that no model covers, give the later build. The
// image can leave out the bytes of 0xFF at its end (format::Cartridge).
ntdlib::dse::Build DseBuildOf(std::span<const uint8_t> image);

// The game code in the header of the cartridge image `image`, or an empty string for bytes that aren't such an image.
std::string DseGameCodeOf(std::span<const uint8_t> image);

// The notes for the user when the cartridge image `image` opens with its sounds `set`, one for each guess that the
// playback makes: a build of the driver that no model covers (it plays on the later build's model), a game that
// ntdlib's table of ticks doesn't list, and the folders of a listed game that hold a song or a bank of sound effects
// and that the game's rows don't cover. Those sounds play with the tick of 10 ms. An image whose code differs from
// ntdlib's profile of the levels of its songs gets a note too (format::DseLevelsNote). A set without sounds gets no
// note. Throws xsflib::FormatError for bytes that aren't a cartridge image of the DS.
std::vector<std::string> DseOpenNotes(std::span<const uint8_t> image, const DseSet& set);

// The length of the driver's tick in microseconds for sound `sound` of `set`. A number past the sounds gives the
// driver's default tick.
int DseSoundTick(const DseSet& set, uint32_t sound);

// The state in which sound `sound` of `set` starts, and DSE's start of it (ntdlib::dse::StartOf). A sound effect, and
// a number past the sounds, start at full level.
ntdlib::dse::SongState DseSoundState(const DseSet& set, uint32_t sound);
ntdlib::dse::StartParameters DseSoundStart(const DseSet& set, uint32_t sound);

// The numbers of the sounds, in order.
std::vector<uint32_t> DseSounds(const DseSet& set);

// The name of sound `sound`: the file name of its song without the extension, or the name of its bank of sound effects
// and its slot (SequenceInput::name). A number past the sounds names the number.
std::string DseSoundName(const DseSet& set, uint32_t sound);

// The folder of sound `sound` in the image's file system, with its last '/', or empty.
std::string DseSoundFolder(const DseSet& set, uint32_t sound);

// The file and the slot of each sound, by the sounds' numbers.
std::vector<ntdlib::dse::SequenceRef> DseSoundRefs(const DseSet& set);

// The id that a track selects `bank` by, from its header.
uint16_t DseBankId(const ntdlib::dse::WaveBank& bank);

// The name of `bank`: the name in its header, or BANK_ and its number.
std::string DseBankName(const ntdlib::dse::WaveBank& bank);

// The programs of `bank` that hold an instrument, in order.
std::vector<uint32_t> DseBankPrograms(const ntdlib::dse::WaveBank& bank);

// The indexes in DseSet::banks of the banks that contain programs, and of those that contain samples. A main bank
// contains samples and usually no programs. The waves of the other banks are in a main bank. An item of the browser
// names a bank by such an index.
std::vector<uint32_t> DseProgramBanks(const DseSet& set);
std::vector<uint32_t> DseSampleBanks(const DseSet& set);

// The slots of `bank`'s table of waves that hold a wave, in order.
std::vector<uint32_t> DseBankWaves(const ntdlib::dse::WaveBank& bank);

// The samples of wave `wave` of bank `bank` of `set`, decoded to 16-bit PCM with its loop and at its rate. Throws
// std::runtime_error with a message for the user for a wave that is missing from the bank or can't be read.
ntdlib::wave::Pcm DseWavePcm(const DseSet& set, uint32_t bank, uint32_t wave);

// The name of `item` in messages: the name of a bank, or the bank and the slot of a wave. A sequence has the name of
// its sound.
std::string DseItemName(const DseSet& set, const Item& item);

// The name of `item`'s file: a bank's name and .swd, or a wave's bank, its slot and .wav. The slot is padded to the
// digits of the last of `waves`, the waves of the bank. The file of a sound ends in .smd.
std::string DseItemFileName(const DseSet& set, const Item& item, std::size_t waves = 0);

// The bytes of `item`'s file: a song's smdl file (kSequence), a bank's swdl file (kBank or kWaveArchive), or a wave as
// a WAV file of 16-bit PCM with its loop. A song or a bank keeps the version of its file. Throws std::runtime_error
// with a message for the user for an item that is missing from the set or can't be read, and for a sound effect. The
// file of a sound effect contains the other sound effects of the same bank.
std::vector<uint8_t> DseItemBytes(const DseSet& set, const Item& item);

// The instruments at `programs` of bank `bank` as SoundFont 2 and SFZ files
// (ntdlib::sequence_export::dse::ConvertBank), by the rules of the set's build of the driver. Throws std::runtime_error
// with a message for the user for a bank that is missing from the set, and BankWithoutInstruments (song/midi_export.h)
// for no programs.
ntdlib::sequence_export::BankConversion ConvertDseBank(const DseSet& set, uint32_t bank,
                                                       std::span<const uint32_t> programs);

} // namespace hokuto::song
