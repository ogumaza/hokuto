// SPDX-License-Identifier: MIT

// What the files that read and write the files of an archive share: the fields of the headers of the consoles' files,
// the strict rules of a read, the reading of a wave archive's file and its waves for writing, and a file's kind in a
// sentence.

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/rsar/sequence_file.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sar/wave_file.h"
#include "seqlib/wav.h"

#include "edit/files.h"
#include "format/file.h"

namespace hokuto::edit
{

// A NintendoWare file's header: the magic, the byte order mark, the header's size, the version at 0x08, the file's size
// at 0x0C and the number of blocks at 0x10, then the blocks' table.
inline constexpr std::size_t kVersionField = 0x08;
inline constexpr std::size_t kSizeField = 0x0C;
inline constexpr std::size_t kHeaderSize = 0x14;

// Whether the byte order mark at 0x04 of `bytes`, a NintendoWare file, marks a big-endian file: FE FF.
bool MarksBigEndian(std::span<const uint8_t> bytes);

// Whether the byte order mark at 0x04 of `bytes`, a NintendoWare file, marks a little-endian file: FF FE.
bool MarksLittleEndian(std::span<const uint8_t> bytes);

// Options that read a file of `flavor` by the strict rules.
ntdlib::sar::Options StrictOptions(ntdlib::sar::Flavor flavor);

// The file of wave archive `wave_archive` of `archive`, read for writing. Throws std::invalid_argument with a message
// for the user for an archive without the wave archive, and for a file that Hokuto can't read.
ntdlib::sar::WaveArchiveFile WaveArchiveFileOf(const ntdlib::sar::SoundArchive& archive, uint32_t wave_archive);

// Throws std::invalid_argument with a message for the user when `file` holds no wave `wave`.
void CheckWave(const ntdlib::sar::WaveArchiveFile& file, uint32_t wave);

// Wave `wave` of `file`, read for writing. Throws std::invalid_argument with a message for the user when `file` holds
// no such wave, and for a wave that Hokuto can't read.
ntdlib::sar::WaveFile WaveFileOf(const ntdlib::sar::WaveArchiveFile& file, uint32_t wave);

// The version of wave `wave` of wave archive `wave_archive` of `archive`. Throws std::invalid_argument with a message
// for the user as WaveArchiveFileOf and WaveFileOf do.
uint32_t PresentWaveVersion(const ntdlib::sar::SoundArchive& archive, uint32_t wave_archive, uint32_t wave);

// A sequence file of the Wii, and the offset of each item of its sequence in its bytecode.
struct WiiSequenceFile
{
    ntdlib::rsar::SequenceFile file;
    std::vector<uint32_t> offsets;
};

// `sequence` in a sequence file of the Wii of `version`, with its labels, by the rules of the build of NW4R SND that
// `native` selects (format::BuildOfWiiArchive). Throws std::invalid_argument with a message for the user for commands
// that the build can't write.
WiiSequenceFile WiiSequenceFileOf(const ntdlib::commands::Sequence& sequence, const format::NativeArchive& native,
                                  uint16_t version);

// The sequence of each sequence file in `sequences` (format::SequencesOfFiles), by the file's ID.
std::map<uint32_t, uint32_t> SequencesByFileId(const std::vector<std::optional<uint32_t>>& sequences);

// The numbers of the sequences of an archive cut again after an edit.
struct SequenceNumbering
{
    // The number in the edited file of each sequence of the cut, by its number in the cut.
    std::map<uint32_t, uint32_t> numbers;

    // The number after the last one that `numbers` assigns to a new sequence.
    uint32_t next = 0;
};

// The numbers of the sequences of a cut. `before` and `after` give the sequence of each sequence file by the file's ID,
// in the edited file and in the cut. Each sequence of the edited file keeps its number, by the ID of its file. The
// `kept` numbers that the edited file uses come first. The cut's other sequences follow them, in the order of the cut,
// up to `count`. Throws std::invalid_argument with a message for the user when the cut is missing a sequence of the
// edited file.
SequenceNumbering SequenceNumbers(const std::map<uint32_t, uint32_t>& before, const std::map<uint32_t, uint32_t>& after,
                                  uint32_t kept, uint32_t count);

// The offset in `entry`, a bank entry of the INFO block of an archive of `flavor`, of its list of wave archives: a
// count, then the wave archives' item IDs. Nothing for an entry without a list, or with a list whose count is past the
// entry's end.
std::optional<std::size_t> WaveArchiveListOf(std::span<const uint8_t> entry, ntdlib::sar::Flavor flavor);

// A file of `kind` in a sentence: "bank file", "wave archive file" or "wave sound file".
std::string FileText(FileKind kind);

// The fields of the header of a file of the Wii: the version, 16 bits, and the file's size, 32 bits. The header takes
// 16 bytes at least.
inline constexpr std::size_t kWiiVersionField = 0x06;
inline constexpr std::size_t kWiiSizeField = 0x08;
inline constexpr std::size_t kWiiHeaderSize = 0x10;

// The version of a Wii archive from which a stream sound's entry counts its channels. Before, it holds a bit for each.
inline constexpr uint16_t kWiiChannelCountVersion = 0x0104;

// `bytes` read as a WAV file, for a new wave or stream. Throws std::invalid_argument with a message for the user for a
// file that seqlib can't read.
seqlib::WavContents ReadWavFile(std::span<const uint8_t> bytes);

// Sets the details of `incoming` that the WAV file `wav` contains: its bits, whether its samples are floating point,
// its loops and their kinds, and what the read dropped.
void TakeWavDetails(const seqlib::WavContents& wav, IncomingWave& incoming);

// The sums of the squares of samples and of their errors, for a signal-to-noise ratio.
struct NoiseSums
{
    double signal = 0.0;
    double noise = 0.0;
};

// Adds `original` against `decoded` to `sums`, over the samples that both have.
void AddNoise(std::span<const int16_t> original, std::span<const int16_t> decoded, NoiseSums& sums);

// The signal-to-noise ratio of `sums` in dB, or nothing where either sum is 0.
std::optional<double> SignalToNoise(const NoiseSums& sums);

} // namespace hokuto::edit
