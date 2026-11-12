// SPDX-License-Identifier: MIT

// A sound archive of the DS (SDAT). NITRO-Composer, the sound driver of the SDK of the DS, plays the sequences of such
// an archive. ntdlib's performer plays a sequence on its model of the driver for the arrangement
// (ntdlib::sequence_export::ntr::Perform), and ntdlib's ArchivePlayer plays it on its models of the driver and the DS's
// sound unit (song/sequence_model.h). The driver's build comes from the ARM7 binary of the cartridge image that
// contains the archive (ntdlib::ntr::snd::BuildOf).
//
// The archive's sounds are its sequences. The number of a sound of the INFO block's table of sequences is its index
// there. From kFirstDsArchivedSound on, the numbers count the sequences of each sequence archive (SSAR) of the table of
// sequence archives in turn, each in the order of its table. A sequence archive whose file can't be read counts none.
// An entry that contains no sequence keeps its number, and plays nothing.

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "ntdlib/ntr/archive_player.h"
#include "ntdlib/ntr/snd/driver.h"
#include "ntdlib/sdat/formats.h"
#include "ntdlib/sequence_export/convert.h"
#include "ntdlib/sequence_export/ntr/performance.h"
#include "ntdlib/wave/decode.h"

#include "song/items.h"

namespace hokuto::song
{

// The offset of a sequence that an SSAR's table lists without a sequence.
inline constexpr uint32_t kNoSsarSequence = 0xFFFFFFFF;

// The number of the first sound of a sequence archive. The number of a sound of the archive's table of sequences is its
// index there. A new sequence is added at the end of that table, and the sequence archives' sounds keep their numbers.
inline constexpr uint32_t kFirstDsArchivedSound = 0x10000;

// The place by which a game's profile names a sound archive of the DS (ntdlib::ntr::GameProfileOf): the game code in
// the header of the cartridge image, the path of the file in the image that contains the archive, and the index of the
// archive among the archives of that file (ntdlib::sdat::FindArchives).
struct DsGame
{
    friend bool operator==(const DsGame&, const DsGame&) = default;

    std::string code;
    std::string path;
    uint32_t index = 0;
};

// A sound archive of the DS, the build of the driver that plays it, its game and its sounds. The archive points into
// `bytes`. An archive that came without its cartridge image has no game.
struct DsArchive
{
    DsArchive() = default;
    DsArchive(const DsArchive&) = delete;
    DsArchive& operator=(const DsArchive&) = delete;

    std::vector<uint8_t> bytes;
    ntdlib::sdat::Archive archive;
    ntdlib::ntr::snd::Build build;
    std::optional<DsGame> game;
    std::vector<std::optional<ntdlib::ntr::SequenceRef>> sounds; // by the sounds' numbers
};

// The archive in `bytes`, from its start to the size in its header, played by `build` of the driver, of `game`. Throws
// xsflib::FormatError for bytes that aren't a sound archive of the DS.
std::shared_ptr<const DsArchive> LoadDsArchive(std::vector<uint8_t> bytes,
                                               const ntdlib::ntr::snd::Build& build = ntdlib::ntr::snd::kDefaultBuild,
                                               std::optional<DsGame> game = std::nullopt);

// The build of the driver in the ARM7 binary of the cartridge image `image`. An image whose ARM7 binary doesn't contain
// the driver's tables once returns the default build (ntdlib::ntr::snd::kDefaultBuild). So do bytes that end before the
// binary. The 0xFF bytes after the end of the image may be missing.
ntdlib::ntr::snd::Build DsBuildOf(std::span<const uint8_t> image);

// The game of the sound archive at `offset` in file `file` of the cartridge image `image`. The size of the image is
// `size`. The 0xFF bytes after the end of the image may be missing. The reader of ntdlib requires the bytes of every
// file. A copy of such an image then gets the 0xFF bytes back. Nothing for an image that ntdlib can't read, and for a
// file that is missing from the image.
std::optional<DsGame> DsGameOf(std::span<const uint8_t> image, uint32_t size, uint32_t file, uint32_t offset);

// What a game's code sets as it starts a sequence of its archive, and the driver's commands that it sends right after
// the start (ntdlib::ntr::ArchivePlayer::Send).
struct DsProfile
{
    ntdlib::ntr::GameSettings settings;
    ntdlib::ntr::snd::CommandList commands;
};

// The profile of sound `sound` of `archive`, by the archive's game and the sequence's label
// (ntdlib::ntr::GameProfileOf). A sound without a sequence gets the default profile. So does a sound of an archive
// without a game, or of a game without a profile. The profile's arrangements, the other states of the game, stay out:
// the sound plays as the game starts it.
DsProfile DsProfileOf(const DsArchive& archive, uint32_t sound);

// What `archive`'s game sets for every sequence of the archive, such as the output effect that it starts at boot or a
// reverb that plays the whole time. The default settings for an archive without a game. The Bank Editor's notes play
// with these settings (DsNotePreview).
ntdlib::ntr::GameSettings DsGameSettingsOf(const DsArchive& archive);

// The same profile without the capture units' effects: the reverb and the output effect. A MIDI file has neither. Its
// CC91 contains only the volume of the reverb, for a synth's reverb. A dry render uses this profile
// (song/dry_render.h).
DsProfile DryDsProfile(DsProfile profile);

// The options of ntdlib's performer for sound `sound` of `archive`: the archive's build of the driver, and what the
// sound's profile sets as the sequence starts (DsProfileOf). The variables of `values`, 0 to 31, replace those of the
// profile. The arrangement and the export to MIDI play the sound with these options.
ntdlib::sequence_export::ntr::PerformOptions DsPerformOptions(const DsArchive& archive, uint32_t sound,
                                                              const std::map<int, int16_t>& values);

// The numbers of the sounds that hold sequences, in order.
std::vector<uint32_t> DsSounds(const DsArchive& archive);

// The sounds of both archives that can take other channels in `after` than in `before`: those whose sequence, bank,
// channel priority or player changed, or whose bank uses a wave archive that changed. Each file counts by its bytes.
std::vector<uint32_t> DsSoundsThatChange(const DsArchive& before, const DsArchive& after);

// The name of sound `sound`: the label of its sequence (ntdlib::ntr::Label). A number past the sounds names the number.
std::string DsSoundName(const DsArchive& archive, uint32_t sound);

// The bank that sound `sound` uses: its index in the table of banks of the archive. The entry of the sound names the
// bank by that index. Nothing for a sound without a sequence, and for a number past the sounds.
std::optional<uint32_t> DsBankOf(const DsArchive& archive, uint32_t sound);

// The name of bank `bank`: its name in the archive, or BANK_ and its index.
std::string DsBankName(const DsArchive& archive, uint32_t bank);

// The programs of bank `bank` that hold an instrument, in order. None for a bank that is missing from the archive or
// whose file can't be read.
std::vector<uint32_t> DsBankPrograms(const DsArchive& archive, uint32_t bank);

// The local variable of player 0 whose value of 1 lets an audition's note go (DsAuditionArchive).
constexpr int kDsAuditionRelease = 0;

// An archive that plays a note of program `program` of bank `bank` of `archive` by itself: the bank, the wave archives
// that its slots name, a player and one sequence. The sequence selects the program, sets the volume to 127 and the pan
// to the centre, and plays `key` at `velocity` with ties on. Each tick, it holds the note until local variable
// kDsAuditionRelease of its player is 1. Its track then ends, and the driver releases the note. The sequence plays on
// player 0. The tracks of player 0 may take any channel. The archive gets the driver build and the game of `archive`.
// Throws std::runtime_error with a message for the user for a bank that is missing from the archive, and
// xsflib::FormatError for a file that is missing from the archive.
std::shared_ptr<const DsArchive> DsAuditionArchive(const DsArchive& archive, uint32_t bank, int program, int key,
                                                   int velocity);

// The indexes of the archive's banks, wave archives and streams: those of the entries of their tables, in order.
std::vector<uint32_t> DsBanks(const DsArchive& archive);
std::vector<uint32_t> DsWaveArchives(const DsArchive& archive);
std::vector<uint32_t> DsStreams(const DsArchive& archive);

// The name of wave archive `wave_archive`: its name in the archive, or WAVEARC_ and its index. A stream's name is its
// name in the archive, or STRM_ and its index.
std::string DsWaveArchiveName(const DsArchive& archive, uint32_t wave_archive);
std::string DsStreamName(const DsArchive& archive, uint32_t stream);

// The number of waves of wave archive `wave_archive`. 0 for a wave archive that is missing from the archive or whose
// file can't be read.
std::size_t DsWaveCount(const DsArchive& archive, uint32_t wave_archive);

// The samples of wave `wave` of wave archive `wave_archive`, decoded to 16-bit PCM with its loop. The rate is that of
// the channel's timer at the wave's root key: 16,756,991 Hz over the timer. ntdlib's converter writes the same rate.
// Throws std::runtime_error with a message for the user for a wave that is missing from the archive or can't be read.
ntdlib::wave::Pcm DsWavePcm(const DsArchive& archive, uint32_t wave_archive, uint32_t wave);

// The rate that the windows show for `wave`: the wave's rate where its timer follows that rate, within a percent, and
// otherwise the rate of the timer. The SDK's tools keep only the low 16 bits of a rate above 65,535.
uint32_t DsWaveRate(const ntdlib::sdat::Wave& wave);

// The samples of `bytes`, a wave's parameters and samples (ntdlib::sdat::WaveBytes), decoded the same way. Throws
// xsflib::FormatError for bytes that aren't a wave.
ntdlib::wave::Pcm DsWavePcmOf(std::span<const uint8_t> bytes);

// The samples of `bytes`, a stream file, with the rate in its field and its loop. Throws xsflib::FormatError for other
// bytes.
ntdlib::wave::Pcm DsStreamPcmOf(std::span<const uint8_t> bytes);

// The samples of every channel of stream `stream`, decoded the same way, with the stream's loop. Throws
// std::runtime_error with a message for the user for a stream that is missing from the archive or can't be read.
ntdlib::wave::Pcm DsStreamPcm(const DsArchive& archive, uint32_t stream);

// The name of `item` in messages: the name of a bank, a wave archive or a stream, or the wave archive and the index of
// a wave.
std::string DsItemName(const DsArchive& archive, const Item& item);

// The name of `item`'s file: the item's name and the extension of its format, .sseq, .sbnk, .swar, .swav or .strm. A
// wave takes its wave archive's name and its index, padded to the digits of the last of `waves`, the wave archive's
// waves.
std::string DsItemFileName(const DsArchive& archive, const Item& item, std::size_t waves = 0);

// The bytes of `item`'s file as the archive stores it: a sequence's SSEQ file, a bank's SBNK file, a wave archive's
// SWAR file, a wave's SWAV file (sdat::WaveFile) or a stream's STRM file. Throws std::runtime_error with a message for
// the user for an item that is missing from the archive or can't be read, and for a sound that a sequence archive
// contains.
std::vector<uint8_t> DsItemBytes(const DsArchive& archive, const Item& item);

// The instruments at `programs` of bank `bank` as SoundFont 2 and SFZ files, by the archive's build of the driver
// (ntdlib::sequence_export::ntr::ConvertBank). Throws std::runtime_error with a message for the user for a bank that
// is missing from the archive, and BankWithoutInstruments (song/midi_export.h) for no programs.
ntdlib::sequence_export::BankConversion ConvertDsBank(const DsArchive& archive, uint32_t bank,
                                                      std::span<const uint32_t> programs);

// The region of `instrument` that NITRO-Composer plays at `key`, or nothing (the driver's ReadInstrument). A drum set
// contains a region for each key of its range. A key split plays the first region whose last key is `key` or above.
const ntdlib::sdat::Region* DsRegionAt(const ntdlib::sdat::Instrument& instrument, int key);

} // namespace hokuto::song
