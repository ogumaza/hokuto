// SPDX-License-Identifier: MIT

// The parts of hokuto_check_items that its files share: a WAV file as the tool reads it, the totals, the helpers and
// the checks. check_items.cpp describes the checks.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/nwsnd/commands.h"
#include "ntdlib/rsar/decode.h"
#include "ntdlib/rsar/formats.h"
#include "ntdlib/rvl/snd/engine.h"
#include "ntdlib/rvl/snd/rsar_options.h"
#include "ntdlib/sar/archive_model.h"
#include "ntdlib/sar/decode.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sar/sequence_file.h"
#include "ntdlib/sar/stream_file.h"
#include "ntdlib/sar/wave_file.h"
#include "ntdlib/sequence_export/consoles.h"
#include "ntdlib/sequence_export/convert.h"
#include "ntdlib/sequence_export/sfz.h"
#include "ntdlib/testing/sequence_export_check.h"
#include "seqlib/sf2/reader.h"
#include "xsflib/ascii.h"
#include "xsflib/binary.h"
#include "xsflib/bytes.h"
#include "xsflib/console.h"
#include "xsflib/files.h"

#include "check_support.h"
#include "format/archive.h"
#include "format/file.h"
#include "song/aac.h"
#include "song/archive.h"
#include "song/biquads.h"
#include "song/items.h"
#include "song/midi_export.h"
#include "song/preview.h"
#include "song/streams.h"

namespace hokuto::tools::items
{

namespace export_ = ntdlib::sequence_export;
namespace rsar = ntdlib::rsar;
namespace sar = ntdlib::sar;
namespace song = hokuto::song;

// A WAV file as the tool reads it: the format, the samples by channel, and the smpl chunk's first loop.
struct WavRead
{
    uint16_t channels = 0;
    uint32_t rate = 0;
    std::vector<std::vector<int16_t>> samples;
    bool loop = false;
    uint32_t loop_start = 0;
    uint32_t loop_last = 0; // the last sample that the loop plays
};

// The little-endian 16-bit number at `at` of `bytes`.
uint16_t Le16(std::span<const uint8_t> bytes, std::size_t at);

// The little-endian 32-bit number at `at` of `bytes`.
uint32_t Le32(std::span<const uint8_t> bytes, std::size_t at);

// Reads a 16-bit PCM WAV file by the RIFF specification: a fmt chunk of PCM or of WAVE_FORMAT_EXTENSIBLE, a data chunk
// and an optional smpl chunk. Returns nothing for a file that breaks a rule.
std::optional<WavRead> ReadWav(std::span<const uint8_t> file);

// Whether `wav` holds `pcm` at its rate, with its loop, each sample within `tolerance` steps.
bool Holds(const WavRead& wav, const sar::Pcm& pcm, int tolerance = 0);

// What the check counted.
struct Totals
{
    std::size_t items = 0;
    std::size_t waves = 0;
    std::size_t undecoded = 0;
    std::size_t wide = 0;
    std::size_t clashes = 0;
    std::size_t missing = 0;
    std::size_t differ = 0;
    std::size_t banks = 0;
    std::size_t instruments = 0;
    std::size_t empty_banks = 0;
    std::size_t unread_banks = 0;
    std::size_t split_banks = 0;
    std::size_t louder_banks = 0;
    std::size_t bank_warnings = 0;
    std::size_t converted = 0;
    std::size_t refused = 0;      // sequence files that reach a command that the 3DS lacks
    std::size_t overlapping = 0;  // sequence files whose commands overlap: no other console takes them
    std::size_t biquads = 0;      // sequence files whose biquad filters take other types or values there
    std::size_t biquads_back = 0; // sequence files whose biquad filters come back otherwise
    std::size_t versions = 0;
    std::size_t unreached = 0;
    std::size_t converted_files = 0; // banks, wave sound files and wave archives for the other consoles
    std::size_t converted_waves = 0;
    std::size_t file_versions = 0; // files that take another version for another console
    std::size_t wave_versions = 0;
    std::size_t loop_starts = 0; // waves that come back with another original loop start
    std::size_t trimmed = 0;     // waves that come back without the bytes after their last sample
    std::size_t wave_sounds = 0;
    std::size_t pitched = 0;
    std::size_t silent = 0;
    std::size_t streams = 0;
    std::size_t stream_channels = 0;
    std::size_t stream_conversions = 0; // a stream's conversions to another console and back
    std::size_t aac_streams = 0;
    std::size_t lacked_streams = 0;
    std::size_t wave_sound_files = 0;
    std::size_t named_files = 0; // wave sound files under the names of their sound groups
    std::size_t notes = 0;       // the Wii's notes as the browser plays them
    std::size_t silent_notes = 0;
    std::size_t endless_notes = 0; // notes that reach the preview's limit
};

// The stream file that the file table of `archive` names for file `file`, read by the tool from the folder of the
// archive at `path`: the table's path with slashes, inside the folder. Nothing when the table names no such path, and
// when the folder lacks the file.
std::optional<std::vector<uint8_t>> StreamBeside(const sar::SoundArchive& archive, uint32_t file,
                                                 const std::filesystem::path& path);

// What is wrong with the exports of the AAC stream `ours` of stream sound `sound`, or nothing. `model` is ntdlib's
// model of the archive.
std::string AacStreamProblem(const std::vector<uint8_t>& ours, const sar::ArchiveModel& model, uint32_t sound,
                             Totals& totals);

// What is wrong with the exports of the stream `ours` that the Hokuto file holds, or nothing. `theirs` holds the file
// beside the archive.
std::string StreamProblem(const std::vector<uint8_t>& ours, const std::vector<uint8_t>& theirs, Totals& totals);

// The wave that the Wii's wave sound `sound` of `archive` plays. The tool reads it with ntdlib alone: the wave of the
// sound's first note, among the waves of its file's wave data, decoded, at the wave's rate times the sound's pitch,
// rounded to whole hertz. Nothing when the archive lacks the sound's file, its note or its wave, and when one of them
// can't be read.
std::optional<rsar::Pcm> WiiWaveSoundPcm(const rsar::SoundArchive& archive, const rsar::SoundInfo& sound);

// What is wrong with the export of a bank's instruments, or nothing: Hokuto's files against those that ntdlib's
// converter makes of the opened archive, the SoundFont by seqlib's check and its presets, and the SFZ files by ntdlib's
// check of a conversion's SFZ files.
std::string BankProblem(const export_::BankConversion& ours, const export_::BankConversion& theirs,
                        const std::vector<uint32_t>& programs);

// What is wrong with the file of sequence sound `sound` for the console of `other`, or nothing. The file comes from
// `built`. `opened` holds the file as the archive gives it. ntdlib's reader of that console has to read the new file,
// and its player has to reach the commands that the archive's file holds (ReachedCommands), but for the biquad filters.
// Converted back, the file has to hold the labels and reach the commands of the archive's file again, but for the
// biquad filters. `totals` counts the files whose filters change there, and those whose filters come back otherwise. It
// also counts the files that come back with another version, and those that come back with other bytes. A file that
// reaches a command that the 3DS lacks has to fail for the 3DS, and `totals` counts it apart. So does a file whose
// commands overlap: it has to fail for every console.
std::string OtherConsoleProblem(const sar::SoundArchive& built, const sar::SoundArchive& opened, uint32_t sound,
                                sar::Flavor other, Totals& totals);

// What is wrong with the file of `item` for the console of `other`, or nothing: a bank's, a wave sound's or a wave
// archive's. The file comes from `built`. `opened` holds the file as the archive gives it. ntdlib's reader of that
// console has to read the same values from the new file: a bank's waves and regions, a wave sound file's waves and wave
// sounds, and each wave's samples, rate, loop and encoding. The new file and its waves have to hold the versions of
// Export…'s files for that console. A wave that goes out alone has to give the bytes that the new wave archive holds
// for it. Converted back with the versions of the archive's file, a bank file and a wave sound file have to give the
// archive's bytes. A wave can come back with another original loop start, or without bytes after its last samples.
// `totals` counts those apart, with the files and waves that take another version.
std::string OtherConsoleFileProblem(const sar::SoundArchive& built, const sar::SoundArchive& opened,
                                    const song::Item& item, sar::Flavor other, Totals& totals);

// Whether `call` throws std::runtime_error: the program refuses what it does.
template <typename Call>
bool Refuses(const Call& call)
{
    try
    {
        call();
    }
    catch (const std::runtime_error&)
    {
        return true;
    }

    return false;
}

// The check of the Wii's archive at `path` with the bytes `bytes` (see the top of check_items.cpp).
int CheckWii(const std::filesystem::path& path, const std::vector<uint8_t>& bytes);

} // namespace hokuto::tools::items
