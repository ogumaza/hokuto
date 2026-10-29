// SPDX-License-Identifier: MIT

// A file of the archive in the place of another: Replace… of a bank, a wave archive or a wave sound in the browser. A
// wave sound's file is the wave sound file that holds it, with the wave sounds of other sounds. The new file goes into
// every copy of the old one, in the archive, in its groups and in the group files beside it (ntdlib's
// sar::ReplaceFile). A group can hold a copy with other bytes. A group's copy of a bank or a wave sound file can name
// the present file's waves at other places, such as in a wave archive of the group's, at other indices. That copy takes
// the new file with its places: each wave of the new file that the present file names takes its place in the copy, and
// each other wave keeps the archive's place. Any other copy with other bytes takes the new file as it is. The archive's
// tables keep naming it by the same file ID. A file of another console among the 3DS, the Wii U and the Switch goes in
// converted (song::ConvertItemFile). The converted file takes the versions of the file that it replaces. A bank and a
// wave sound file name their waves by a wave archive's place in the archive and a wave's index there. ChangesOf finds
// what the new file changes besides its item, for the Replace window's warnings.
//
// A WAV file can also take the place of a wave of a wave archive. The wave goes in as DSP-ADPCM or as 16-bit PCM, with
// the WAV file's rate, its channels and the first loop of its smpl chunk. It keeps the present wave's version. A wave
// plays up to the end of its loop. The samples after the loop's end stay out. ntdlib's encoder makes the DSP-ADPCM. A
// loop in DSP-ADPCM starts at a frame of 14 samples, and the decoder starts each later pass from the state that the
// wave keeps for the loop's start. A loop's start between frames moves forward to the next frame. The samples between
// repeat after the loop's end, and the loop plays the same samples. The wave keeps the WAV file's loop start as the
// original one. The wave archive's other waves keep their bytes. The new wave archive then goes in as a file in the
// place of the old one. A wave file of the 3DS, the Wii U or the Switch can take a wave's place too. A wave file of the
// archive's console goes in as it is. A wave file of another console goes in converted, with the present wave's
// version.
//
// A WAV file or a wave file can also go in as a new wave after a wave archive's waves: File > Import > Audio File… and
// Wave…. A converted wave and a WAV file's wave then take the version of the wave archive's first wave, or of the first
// wave of the archive's first wave archive with waves (IncomingVersions).

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "ntdlib/rsar/formats.h"
#include "ntdlib/rsar/wave_file.h"
#include "ntdlib/sar/formats.h"

#include "edit/replace.h"
#include "format/file.h"
#include "song/items.h"

namespace hokuto::edit
{

// The kinds of file that Replace… puts in an item's place.
enum class FileKind
{
    kBank,
    kWaveArchive,
    kWaveSoundFile,
};

// The kind's name in a sentence, such as "wave archive".
std::string NameOf(FileKind kind);

// A file that goes in converted, from the other console or from a later version than the archive's files: the console,
// the version in the header and the size of the file that was read, and the version that the waves of a wave archive
// take. `wii` marks a file of the Wii that goes in written again in an earlier version. `flavor` then means nothing.
struct ConvertedFile
{
    ntdlib::sar::Flavor flavor = ntdlib::sar::Flavor::kCafe;
    uint32_t version = 0;
    std::size_t size = 0;
    std::optional<uint32_t> wave_version;
    bool wii = false;
};

// A file to put in: its kind, the version in its header and its bytes, and the file that was read when it went in
// converted. On the Wii, a bank file or a wave sound file can come in with wave data, its waves in the file's form. A
// file without wave data plays the wave data of the file whose place it takes.
struct IncomingFile
{
    FileKind kind = FileKind::kBank;
    uint32_t version = 0;
    std::vector<uint8_t> bytes;
    std::optional<ConvertedFile> converted;
    std::vector<uint8_t> wave_data;
};

// Reads `bytes` as a file of `kind` for an archive of `flavor`. The file's header gives its size. Bytes past it stay
// out. A file of another console among the 3DS, the Wii U and the Switch goes in converted with `versions`
// (song::ConvertItemFile). Without them, it takes the versions of the files that Export… writes for the archive's
// console (song::ExportVersions). With `versions`, a file of the archive's console goes in with them too when it holds
// a later version than they give, or when a wave of a wave archive does. A game may refuse a later version than its
// files hold. Without them, such a file goes in as it is. ntdlib has to read the file by the strict rules before the
// conversion and after it. Throws std::invalid_argument with a message for the user for bytes that aren't such a file,
// and a file that ntdlib can't read or convert.
IncomingFile ReadIncomingFile(std::span<const uint8_t> bytes, FileKind kind, ntdlib::sar::Flavor flavor,
                              const std::optional<song::FileVersions>& versions = std::nullopt);

// The versions that a file takes in `archive` when it goes in converted (ReadIncomingFile): a file of the other
// console, or one of the archive's console with a later version. A file in the place of item `item` takes those of the
// item's file. A new item takes those of the archive's first file of `kind`. A wave archive's waves take the version of
// its first wave, or of the first wave of the archive's first wave archive with waves. Where the archive holds no such
// file, the versions of the files that Export… writes for the archive's console serve (song::ExportVersions).
song::FileVersions IncomingVersions(const ntdlib::sar::SoundArchive& archive, FileKind kind,
                                    std::optional<uint32_t> item = std::nullopt);

// The file ID of item `item` of `archive`: bank `item`, wave archive `item`, or the wave sound file of sound `item`.
// Throws std::invalid_argument with a message for the user for an item past the archive's list, and for a sound that
// isn't a wave sound.
uint32_t FileIdOf(const ntdlib::sar::SoundArchive& archive, FileKind kind, uint32_t item);

// The waves that a bank or a wave sound file names and the archive lacks, for one wave archive: the wave archive
// itself, by its place in the archive's list, or the waves past its end.
struct LackedWaves
{
    uint32_t wave_archive = 0;
    bool whole = false;          // the archive lacks the wave archive, or can't read it
    std::size_t count = 0;       // the waves that the wave archive holds
    std::vector<uint32_t> waves; // the waves past its end, in order, each once
};

// What a new file changes besides its item. Each list of items holds places in the archive's lists, in order.
struct FileChanges
{
    std::vector<uint32_t> sharing;     // the other items whose file it is: banks, wave archives or sounds
    std::size_t other_copies = 0;      // the copies of the present file in groups that hold other bytes, and take
                                       // the new file as it is
    std::size_t placed_copies = 0;     // the copies in groups that name the present file's waves at other places
    std::size_t unplaced_waves = 0;    // the new file's waves that the present file doesn't name: those copies name
                                       // them at the archive's places
    std::optional<uint32_t> version;   // the version in the present file's header
    std::size_t size = 0;              // the present file's size
    std::vector<LackedWaves> lacked;   // a bank or a wave sound file: the waves that the archive lacks
    std::vector<uint32_t> programs;    // a bank: the present bank's programs that the new one lacks
    std::vector<uint32_t> players;     // a bank: the sequence sounds that have it in a slot
    std::size_t count = 0;             // the new wave archive's waves, or the new wave sound file's wave sounds
    std::vector<uint32_t> past_banks;  // a wave archive: the banks that name waves past its end
    std::vector<uint32_t> past_sounds; // a wave archive: the wave sounds whose notes play waves past its end, and for a
                                       // wave sound file, the sounds that play wave sounds past its end
};

// What `incoming` changes in the place of item `item` of `archive`, the archive that `file` builds. Banks, wave
// archives and wave sound files that ntdlib can't read count as naming nothing.
FileChanges ChangesOf(const format::File& file, const ntdlib::sar::SoundArchive& archive, uint32_t item,
                      const IncomingFile& incoming);

// The sounds that play item `item` of `archive`, by their places in the sound table, in order: for a bank, the sequence
// sounds with the bank, or another bank of its file, in a slot; for a wave archive, the sequence sounds with a bank
// that names its waves in a slot, and the wave sounds whose notes play its waves; for a wave sound, the wave sounds of
// its file. Banks and wave sound files that ntdlib can't read count as naming nothing. The Replace window plays one of
// them.
std::vector<uint32_t> SoundsPlaying(const ntdlib::sar::SoundArchive& archive, FileKind kind, uint32_t item);

// The programs of a bank that the sounds of an archive select, in order, each once, and a count of those sounds'
// program changes whose program or slot comes from a random number or a variable.
struct BankSelections
{
    std::vector<uint32_t> programs;
    std::size_t undecided = 0;
};

// The programs of bank `bank` of `archive` that its sounds select. A sequence sound with the bank in a slot, or a bank
// of the same file, selects the programs of its program changes in that slot. Only the program changes that the player
// reaches from the sound's start count (ProgramsSelected). `places` gives where each sound plays in `file`
// (SoundPlaces). A sound without a place selects none. Throws std::invalid_argument with a message for the user for a
// bank past the archive's list.
BankSelections ProgramsOfBank(const format::File& file, const ntdlib::sar::SoundArchive& archive,
                              std::span<const std::optional<SoundPlace>> places, uint32_t bank);

// The Wii's files. A bank file (RBNK) and a wave sound file (RWSD) of the Wii keep their waves beside them, in their
// wave data: samples that the file's WAVE block describes, or an RWAR. A bank of version 1.1 and a wave sound file up
// to version 1.2 have a WAVE block. Hokuto takes a file's wave data for a wave archive, by the file's ID
// (song::WiiWaveArchive). Replace… of a bank or a wave sound puts a file of the Wii in the place of the item's file,
// and the wave data stays. The new file has to keep its waves as the present one does. Replace… of a wave archive puts
// an RWAR's waves in the place of the waves of a bank's or a wave sound file's wave data. Every group that holds the
// file takes the new bytes (ntdlib::rsar::ReplaceFile). A file of the 3DS, the Wii U or the Switch goes in converted. A
// wave archive's waves go in as ntdlib::rsar::WiiWaveArchiveOf converts them. A bank file or a wave sound file names
// the waves of its console's wave archives. WiiFileOf gives it the waves of the Wii archive's wave data instead, in new
// wave data.

// Reads `bytes` as a file of the Wii of `kind`: an RBNK, an RWAR or an RWSD. The file's header gives its size. Bytes
// past it stay out. ntdlib has to read the file. A wave archive file of the 3DS, the Wii U or the Switch goes in as an
// RWAR of the same waves (ntdlib::rsar::WiiWaveArchiveOf). Throws std::invalid_argument with a message for the user for
// bytes that aren't such a file, for a bank file or a wave sound file of the 3DS, the Wii U or the Switch (WiiFileOf
// converts them), and for a file that ntdlib can't read or convert.
IncomingFile ReadWiiIncomingFile(std::span<const uint8_t> bytes, FileKind kind);

// The console of `bytes` when they start as a file of `kind` of the 3DS, the Wii U or the Switch does, by the magic and
// the byte order mark, or nothing.
std::optional<ntdlib::sar::Flavor> LaterConsoleOf(std::span<const uint8_t> bytes, FileKind kind);

// The version that a new file of `kind` takes in the Wii's `archive`: the version of the archive's first file of the
// kind that ntdlib reads. Without one, a bank takes version 1.2 from RSAR 1.4 on and 1.1 before. A wave sound file then
// keeps its waves as the archive's banks do: version 1.3 beside banks of 1.2, and 1.2 otherwise. A wave archive takes
// 1.0.
uint16_t NewWiiFileVersion(const ntdlib::rsar::SoundArchive& archive, FileKind kind);

// The version of the file of item `item` of the Wii's `archive`, a bank or a wave sound, by `kind`: the version in the
// file's header. A file that the archive lacks gives NewWiiFileVersion's. A file of another console that takes the
// item's place takes this version (WiiFileOf).
uint16_t PresentWiiVersion(const ntdlib::rsar::SoundArchive& archive, FileKind kind, uint32_t item);

// `incoming`, a bank file or a wave sound file of console `flavor` among the 3DS, the Wii U and the Switch
// (ReadIncomingFile), as a file of the Wii of `version` with its wave data, for the Wii's `archive`
// (ntdlib::rsar::WiiBankOf and WiiWsdOf). `map` gives a file of `archive` for each wave archive that the file's waves
// name, by its item ID (WaveArchivesNamed): wave i of the wave archive is wave i of the file's wave data. The new wave
// data holds the waves that the file's table names, in its order. A bank of version 1.1 and a wave sound file of 1.2
// describe them in a WAVE block, and later versions keep them in an RWAR. A wave that the chosen wave data lacks plays
// nothing. `converted` of the result describes the file that was read. Throws std::invalid_argument with a message for
// the user for a wave archive that `map` leaves out, wave data that Hokuto can't read, and a file that ntdlib can't
// read or convert.
IncomingFile WiiFileOf(const IncomingFile& incoming, ntdlib::sar::Flavor flavor,
                       const ntdlib::rsar::SoundArchive& archive, const std::map<uint32_t, uint32_t>& map,
                       uint16_t version);

// Wave data for a bank file or a wave sound file of the Wii that comes in: the bytes of an RWAR, or of the wave data of
// a file of the archive, and their waves.
struct WiiWaveData
{
    std::vector<uint8_t> bytes;
    std::vector<ntdlib::rsar::WaveFile> waves;
    bool rwar = true; // the bytes are an RWAR, not samples that a WAVE block describes
};

// Reads `bytes` as an RWAR beside a bank file or a wave sound file of the Wii. The file's header gives its size. Bytes
// past it stay out. Throws std::invalid_argument with a message for the user for bytes that aren't an RWAR, and for a
// wave that ntdlib can't read.
WiiWaveData ReadWiiWaveData(std::span<const uint8_t> bytes);

// The wave data of file `file_id` of the Wii's `archive`, a bank's or a wave sound file's, with its waves (WavesOf).
// Throws as WavesOf does.
WiiWaveData WiiWaveDataOf(const ntdlib::rsar::SoundArchive& archive, uint32_t file_id);

// `incoming`, a bank file or a wave sound file of the Wii (ReadWiiIncomingFile), with the waves of `data` as its wave
// data, in `version` where it holds a later version. The wave data takes the file's form: the bytes of `data` where
// they have it, and otherwise the waves laid out in it. A WAVE block describes particular places in its samples. A file
// with a WAVE block therefore takes only waves laid out as the tools lay them out, such as the waves of an RWAR that
// Export… writes for the file's wave data. A later version goes in written again through ntdlib's model of the file.
// Throws std::invalid_argument with a message for the user for a WAVE block that describes other samples, a file that
// names a wave past the waves, and a file that ntdlib can't write in `version`.
IncomingFile WiiFileWithWaves(const IncomingFile& incoming, const WiiWaveData& data, uint16_t version);

// What keeps `wave_data` from serving as the wave data of `bytes`, a bank file or a wave sound file of the Wii, or
// nothing: ntdlib can't read the file's waves from it, or the file names a wave past its waves.
std::string WiiWaveDataProblem(std::span<const uint8_t> bytes, FileKind kind, std::span<const uint8_t> wave_data);

// The file ID of item `item` of the Wii's `archive`: bank `item`'s file, the file whose wave data is wave archive
// `item` (`item` is the file's ID), or the wave sound file of sound `item`. Throws std::invalid_argument with a message
// for the user for an item past the archive's lists, a file without wave data inside the archive, and a sound that
// isn't a wave sound.
uint32_t FileIdOf(const ntdlib::rsar::SoundArchive& archive, FileKind kind, uint32_t item);

// What `incoming` changes in the place of item `item` of the Wii's `archive`. The other items of the file are the
// file's other banks, or the other wave sounds of a wave sound file. A bank or a wave sound file names its waves by
// their indices in its wave data. `lacked` then holds the waves past the end of the present wave data, with the file's
// ID as the wave archive's place. A new RWAR's banks and wave sounds are those of its file. Banks and wave sound files
// that ntdlib can't read count as naming nothing. Throws the exceptions of FileIdOf.
FileChanges ChangesOf(const format::File& file, const ntdlib::rsar::SoundArchive& archive, uint32_t item,
                      const IncomingFile& incoming);

// `file` after `incoming` takes the place of the file of item `item` of the Wii's `archive`, the archive that `file`
// builds: a bank's file, a wave sound's file, or the wave data of a wave archive's file. A file with wave data takes
// the place of the present wave data too. A wave archive beside a file with a WAVE block gives the file its waves: the
// file describes them in its WAVE block (WithWaves). Throws std::invalid_argument with a message for the user for a
// file without wave data that keeps its waves otherwise than the present file, and the exceptions of FileIdOf,
// WithWaves and ReplaceFile.
format::File ReplaceWiiFile(const format::File& file, const ntdlib::rsar::SoundArchive& archive, uint32_t item,
                            const IncomingFile& incoming);

// The sounds that play item `item` of the Wii's `archive`, by their places in the sound table, in order: for a bank,
// the sequence sounds with the bank or another bank of its file; for a wave archive, the sounds that play the banks or
// the wave sounds of its file; for a wave sound, the wave sounds of its file.
std::vector<uint32_t> SoundsPlaying(const ntdlib::rsar::SoundArchive& archive, FileKind kind, uint32_t item);

// An instrument for a program that a new bank lacks: the present bank's instrument at the program, or the new bank's
// instrument at program `from`. The Replace window of a bank offers them for the programs that the sounds select
// (ProgramsOfBank).
struct ProgramFix
{
    friend bool operator==(const ProgramFix&, const ProgramFix&) = default;

    uint32_t program = 0;
    std::optional<uint32_t> from;
};

// `bytes`, a bank file of the archive's console for the place of bank `bank` of `archive`, with the instrument of each
// fix at its program (ntdlib's sar::WithInstruments). A copy holds the whole instrument and plays the same waves. A
// copy of the present bank's instrument plays the present bank's waves. The groups that load the bank for a sound hold
// their wave archives already. Throws std::invalid_argument with a message for the user for a program that the new bank
// holds or that two fixes take, an instrument that the bank to copy from lacks, and a bank file that ntdlib can't read.
std::vector<uint8_t> WithProgramFixes(const ntdlib::sar::SoundArchive& archive, uint32_t bank,
                                      std::span<const uint8_t> bytes, std::span<const ProgramFix> fixes);

// `file` after its native archive's file `file_id` holds `bytes` in every copy. A group's copy that names the present
// file's waves at other places takes `bytes` with those places (see above). On the Wii, the file's wave data holds
// `wave_data` too where the call gives it (ntdlib::rsar::ReplaceFile). The file's models of the bank or the waves that
// the file held go (format::File's banks and waves). Throws std::invalid_argument with a message for the user for a
// file without a native archive, a file whose bytes another file ID names too, and a file that the cut keeps inside the
// archive's bytes, such as one that an earlier version of Hokuto cut. It also throws for wave data outside the Wii, and
// on the Wii for wave data for a file without wave data.
format::File ReplaceFile(const format::File& file, uint32_t file_id, std::span<const uint8_t> bytes,
                         std::optional<std::span<const uint8_t>> wave_data = std::nullopt);

// A wave for a wave archive, from a WAV file or from a wave file: the wave file that goes in, and what the file held.
// The rate, the frames and the loop's start are the wave's: the file's, or after a resample to a lower rate. A loop in
// DSP-ADPCM starts `loop_moved` samples later, and the wave holds as many samples more. For a wave file, the fields
// from `loop_moved` on keep their defaults. They describe a WAV file.
struct IncomingWave
{
    std::vector<uint8_t> bytes;             // the wave file
    uint32_t version = 0;                   // the version in the wave file's header
    std::optional<ConvertedFile> converted; // a wave file that went in converted: the file that was read
    ntdlib::sar::WaveEncoding encoding = ntdlib::sar::WaveEncoding::kDspAdpcm;
    std::size_t channels = 0;
    uint32_t rate = 0;      // samples per second
    std::size_t frames = 0; // the WAV file's samples in each channel, up to its loop's end
    bool loop = false;
    uint32_t loop_start = 0;
    uint32_t loop_moved = 0; // the samples from the WAV file's loop start to the wave's

    // The ratio of the energy of the WAV file's samples to that of the error of DSP-ADPCM, in decibels. Nothing for
    // 16-bit PCM, or when DSP-ADPCM gives the samples back exactly.
    std::optional<double> signal_to_noise;

    uint16_t bits = 16;         // the WAV file's bits per sample
    bool floating = false;      // whether the WAV file's samples are floating point
    uint32_t loops = 0;         // the loops in the WAV file's smpl chunk
    bool loop_forward = true;   // whether its first loop plays forward
    bool loop_endless = true;   // whether its first loop repeats without end
    std::size_t after_loop = 0; // the WAV file's samples after its loop's end: the wave leaves them out
    std::size_t dropped = 0;    // the bytes at the end of the WAV file's samples that hold no whole frame

    // The WAV file's rate, its samples up to its loop's end and its loop's start. A resample makes the wave's differ.
    uint32_t wav_rate = 0;
    std::size_t wav_frames = 0;
    uint32_t wav_loop_start = 0;
    bool loop_from_present = false; // the loop is the present wave's: the WAV file holds none

    // The model of a WAV file's wave: the wave's samples as 16-bit PCM before the encoding, with the encoding, the rate
    // and the loop. A file keeps it with the wave (format::WaveModel). Nothing for a wave file.
    std::optional<format::WaveModel> model;
};

// A loop for a WAV file without one: its start and its end, in the WAV file's samples. Replace… of a wave offers the
// present wave's loop this way.
struct WaveLoop
{
    uint32_t start = 0;
    uint32_t end = 0;
};

// Reads `bytes` as a WAV file for wave `wave` of wave archive `wave_archive` of `archive`, or for a new wave of the
// wave archive without `wave`. Makes the wave in `encoding`: DSP-ADPCM or 16-bit PCM. A WAV file without a loop takes
// `loop`, and the samples after its end stay out. A WAV file that holds a loop keeps it. With `rate`, lower than the
// WAV file's, the wave holds the samples resampled to it (audio::Resample). Throws std::invalid_argument with a message
// for the user for bytes that aren't a WAV file that Hokuto reads, a file of more than two channels, of no samples or
// of too many, an archive without the wave, a present wave that Hokuto can't read, another encoding, a loop outside the
// samples, and a rate that isn't lower than the WAV file's.
IncomingWave ReadIncomingWave(std::span<const uint8_t> bytes, const ntdlib::sar::SoundArchive& archive,
                              uint32_t wave_archive, std::optional<uint32_t> wave,
                              ntdlib::sar::WaveEncoding encoding = ntdlib::sar::WaveEncoding::kDspAdpcm,
                              std::optional<WaveLoop> loop = std::nullopt, std::optional<uint32_t> rate = std::nullopt);

// The same for a wave of the Wii: `bytes` of the result hold the wave as an RWAV file of `version`
// (ntdlib::rsar::WaveFile), such as 0x0102. A bank or a wave sound file that keeps its waves in a WAVE block holds the
// wave's values and samples (ntdlib::rsar::BankFile and WsdFile), and an RWAR holds the RWAV file. The Wii's waves keep
// no original start of their loops. Throws std::invalid_argument with a message for the user as ReadIncomingWave does,
// and for a wave that an RWAV file can't hold.
IncomingWave ReadIncomingWiiWave(std::span<const uint8_t> bytes, uint16_t version,
                                 ntdlib::sar::WaveEncoding encoding = ntdlib::sar::WaveEncoding::kDspAdpcm,
                                 std::optional<WaveLoop> loop = std::nullopt,
                                 std::optional<uint32_t> rate = std::nullopt);

// Whether `bytes` start as a wave file of the 3DS, the Wii U or the Switch does: with CWAV or FWAV.
bool IsWaveFile(std::span<const uint8_t> bytes);

// Whether `bytes` start as a wave file of the Wii does: with RWAV.
bool IsWiiWaveFile(std::span<const uint8_t> bytes);

// Reads `bytes` as a wave file of the Wii (RWAV). The file's header gives its size. Bytes past it stay out. A wave file
// of the 3DS, the Wii U or the Switch goes in converted (ntdlib::rsar::WiiWaveOf): an RWAV file of version 1.2 with its
// encoding, its rate, its loop and its first two channels. Throws std::invalid_argument with a message for the user for
// bytes that aren't a wave file, and a wave that ntdlib can't read or convert.
IncomingWave ReadIncomingWiiWaveFile(std::span<const uint8_t> bytes);

// Reads `bytes` as a wave file of the 3DS, the Wii U or the Switch for wave `wave` of wave archive `wave_archive` of
// `archive`, or for a new wave of the wave archive without `wave`. The file's header gives its size. Bytes past it stay
// out. A wave file of another console goes in converted, with the present wave's version or the version that a new wave
// takes. So does a wave file of the archive's console with a later version. Throws std::invalid_argument with a message
// for the user for bytes that aren't a wave file, a wave that ntdlib can't read or convert, an archive without the
// wave, and a present wave that Hokuto can't read.
IncomingWave ReadIncomingWaveFile(std::span<const uint8_t> bytes, const ntdlib::sar::SoundArchive& archive,
                                  uint32_t wave_archive, std::optional<uint32_t> wave = std::nullopt);

// The present wave, and what plays it. Each list of items holds places in the archive's lists, in order.
struct WaveChanges
{
    ntdlib::sar::WaveEncoding encoding = ntdlib::sar::WaveEncoding::kDspAdpcm;
    std::size_t channels = 0;
    uint32_t rate = 0;
    bool loop = false;
    uint32_t loop_start = 0;
    uint32_t loop_end = 0;        // the samples' count when the wave doesn't loop
    std::size_t size = 0;         // the wave file's size
    std::vector<uint32_t> banks;  // the banks that name the wave
    std::vector<uint32_t> sounds; // the wave sounds whose notes play it
};

// Wave `wave` of wave archive `wave_archive` of `archive`, and what plays it. Banks and wave sound files that ntdlib
// can't read count as naming nothing. Throws std::invalid_argument with a message for the user for an archive without
// the wave, and for a wave that Hokuto can't read.
WaveChanges ChangesOfWave(const ntdlib::sar::SoundArchive& archive, uint32_t wave_archive, uint32_t wave);

// The file of wave archive `wave_archive` of `archive` with `wave_file` in the place of wave `wave`, or after the waves
// without `wave`. The other waves keep their bytes. Throws std::invalid_argument with a message for the user for an
// archive without the wave archive or the wave, and for a wave archive that Hokuto can't read.
std::vector<uint8_t> WaveArchiveWith(const ntdlib::sar::SoundArchive& archive, uint32_t wave_archive,
                                     std::optional<uint32_t> wave, std::span<const uint8_t> wave_file);

// The file of wave archive `wave_archive` of `archive` with `wave_files` after its waves, in their order. The other
// waves keep their bytes. Throws as WaveArchiveWith does.
std::vector<uint8_t> WaveArchiveWithNew(const ntdlib::sar::SoundArchive& archive, uint32_t wave_archive,
                                        std::span<const std::vector<uint8_t>> wave_files);

// Wave `wave` of the wave data of file `file_id` of the Wii's `archive`, and what plays it: the banks of the file whose
// regions name the wave, or the wave sounds of the file whose notes play it. Throws std::invalid_argument with a
// message for the user as WavesOf does (edit/waves.h), and for a wave past the wave data's waves.
WaveChanges ChangesOfWiiWave(const ntdlib::rsar::SoundArchive& archive, uint32_t file_id, uint32_t wave);

// `file` after `incoming`, a wave of the Wii, takes the place of wave `wave` of the wave data of file `file_id` of the
// Wii's `archive`, the archive that `file` builds, or goes after the wave data's waves without `wave`. The new wave
// takes the version of the present wave, or of the wave data's first wave. The other waves keep their bytes. Throws
// std::invalid_argument with a message for the user as WavesOf and WithWaves do, and for a wave past the wave data's
// waves.
format::File WithIncomingWiiWave(const format::File& file, const ntdlib::rsar::SoundArchive& archive, uint32_t file_id,
                                 std::optional<uint32_t> wave, const IncomingWave& incoming);

} // namespace hokuto::edit
