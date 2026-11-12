// SPDX-License-Identifier: MIT

// The parts of hokuto_check_banks that its files share: the constants, the totals, the helpers and the checks.
// check_banks.cpp describes the checks.

#pragma once

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "ntdlib/rsar/archive_model.h"
#include "ntdlib/rsar/bank_file.h"
#include "ntdlib/rsar/formats.h"
#include "ntdlib/rsar/stream_file.h"
#include "ntdlib/rsar/wave_file.h"
#include "ntdlib/rvl/snd/rsar_options.h"
#include "ntdlib/sar/bank_file.h"
#include "ntdlib/sar/decode.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sar/stream_file.h"
#include "ntdlib/sar/wave_file.h"
#include "ntdlib/sound_build.h"
#include "ntdlib/wave/adpcm.h"
#include "xsflib/binary.h"
#include "xsflib/console.h"
#include "xsflib/files.h"

#include "check_part.h"
#include "checked_archive.h"
#include "checked_saves.h"
#include "edit/banks.h"
#include "edit/streams.h"
#include "edit/waves.h"
#include "format/archive.h"
#include "format/file.h"
#include "later_passes.h"
#include "song/aac.h"
#include "song/archive.h"
#include "song/items.h"
#include "song/preview.h"
#include "song/streams.h"

namespace hokuto::tools::banks
{

namespace sar = ntdlib::sar;
namespace rsar = ntdlib::rsar;
namespace edit = hokuto::edit;
namespace format = hokuto::format;
namespace song = hokuto::song;

// How long a note plays, in seconds: its hold and the start of its release.
constexpr double kNoteSeconds = 1.5;

// How far the level of a note with its volume halved may lie from the ratio of the volumes, in dB, and the level below
// which a note counts apart. On the Wii U, AX ramps a voice's volume across each frame in whole steps of its 1.15
// volume. Each step rounds towards zero, and a ramp stops short of its target by up to 95 of the volume's units. A
// halved volume falls short by twice as much of its level. Where a note's envelope moves, its level can then lie 0.1 dB
// off the ratio.
constexpr double kLevelTolerance = 0.2;
constexpr double kQuietLevel = -60.0;

// A region's full volume. A region above it plays through the console's limits on gains, and its level doesn't follow
// the ratio of the volumes.
constexpr int kFullVolume = 127;

// The peak of a note on the 3DS from which its level doesn't follow the ratio of the volumes. nn::snd turns the DSP's
// limiter on. The limiter takes about 0.9 * a * a dB off the level of a note that peaks at a of full scale without it.
// With its volume halved, a note that peaks at half of full scale then falls by about 0.17 dB less than the ratio, and
// louder notes by more. With the limiter off, the levels follow the ratio.
constexpr float kLimitedPeak = 0.5F;

// The share of a wave's or a stream's rate, length and middle that the edits take.
constexpr double kShare = 0.75;

// The ratio of signal to noise in dB below which a wave in DSP-ADPCM counts apart.
constexpr double kLowSignalToNoise = 20.0;

// A channel's DSP-ADPCM frames and its info, for the check of a loop's later passes.
using AdpcmChannel = std::pair<std::span<const uint8_t>, std::optional<sar::DspAdpcmInfo>>;

// The Wii's DSP loads the loop's context at the loop's end, and its accelerator reads each frame's header at the
// frame's start (ntdlib's rvl::ax::Renderer). Its sums don't wrap.
constexpr LoopDecoding kWiiLoopDecoding = {ntdlib::wave::DspAdpcmSum::kWide, false};

// What the checks found, for the summary.
struct Totals
{
    std::size_t problems = 0;

    std::size_t banks = 0;
    std::size_t unread_banks = 0;
    std::size_t refused_banks = 0; // banks of the Wii that ntdlib reads and the Bank Editor refuses
    std::size_t shared_banks = 0;  // banks whose file an earlier bank plays
    std::size_t programs = 0;
    std::size_t quiet_programs = 0;
    std::size_t loud_programs = 0;
    std::size_t limited_programs = 0; // notes on the 3DS that the DSP's limiter compresses
    std::size_t level_programs = 0;   // the programs whose halved volume the levels check
    double level_error = 0.0;

    std::size_t waves = 0;
    std::size_t unread_waves = 0;
    std::size_t adpcm_loops = 0;
    std::size_t adpcm_waves = 0;
    std::size_t low_snr_waves = 0;
    double least_snr = std::numeric_limits<double>::infinity();

    std::size_t streams = 0;
    std::size_t aac_streams = 0;
    double least_stream_snr = std::numeric_limits<double>::infinity();

    std::size_t wave_sounds = 0;
    std::size_t unread_wave_sounds = 0;

    std::size_t other_files = 0;
};

// The file that the edits so far have made, and the archive that it builds.
struct Edited
{
    format::File file;
    std::shared_ptr<const sar::SoundArchive> archive;
};

// Prints a problem and counts it.
void Problem(Totals& totals, const std::string& where, const std::string& what);

// The archive that `file` builds, with its group files.
std::shared_ptr<const sar::SoundArchive> Built(const format::File& file, const std::string& name);

// The samples of `preview` for up to kNoteSeconds. Two floats for each stereo frame.
std::vector<float> Rendered(song::NotePreview& preview);

// The highest magnitude of `samples`, against full scale.
float Peak(std::span<const float> samples);

// The level of `samples` in dB against full scale, or -infinity for silence.
double Level(std::span<const float> samples);

// The ratio of signal to noise in dB of `heard` against `expected`, over the first `length` samples of each channel.
double SignalToNoise(const std::vector<std::vector<int16_t>>& expected, const std::vector<std::vector<int16_t>>& heard,
                     uint32_t length);

// Whether the first `length` samples of each channel of `a` and `b` are the same.
bool SameStart(const std::vector<std::vector<int16_t>>& a, const std::vector<std::vector<int16_t>>& b, uint32_t length);

// `values` of a stream at kShare of the rate, with a loop from the first block past the middle, or from the start
// where no block starts past the middle.
edit::StreamValues LoopedStreamValues(const edit::StreamValues& values);

// `model` of a wave at kShare of the rate, with the loop's end at kShare of the length and its start near the middle of
// the loop, on a frame for DSP-ADPCM.
format::WaveModel LoopedWaveModel(const format::WaveModel& model);

// The other encoding of a wave or a stream that the checks take: PCM16 for DSP-ADPCM, and DSP-ADPCM for the others.
sar::WaveEncoding OtherEncoding(sar::WaveEncoding encoding);

// Whether `after`, a wave or a stream in another encoding, keeps the rate, the channels and the loop of `before`. The
// loop may start up to `slack` - 1 samples later. The samples up to the new start then repeat after the loop's end, and
// the end moves as far as the start.
bool KeepsValues(const ntdlib::wave::Pcm& before, const ntdlib::wave::Pcm& after, uint32_t slack);

// How the console of `flavor` decodes the later passes of a loop in DSP-ADPCM. The Switch and the 3DS read the
// predictor and the scale from the frame's header when the loop starts at a frame's start. The Wii U takes them from
// the loop's context there too. The 3DS's sums don't wrap (ntdlib's sar/decode.h).
LoopDecoding LoopDecodingOf(sar::Flavor flavor);

// The DSP-ADPCM info of a channel of the 3DS, the Wii U or the Switch.
sar::DspAdpcmInfo InfoOf(const sar::DspAdpcmInfo& info);

// The DSP-ADPCM info of a channel of the Wii, in the terms of the later consoles.
sar::DspAdpcmInfo InfoOf(const rsar::DspAdpcmInfo& info);

// The DSP-ADPCM frames and the info of each of `channels`, a wave's or a stream's.
template <typename Channels>
std::vector<AdpcmChannel> AdpcmChannels(const Channels& channels)
{
    std::vector<AdpcmChannel> result;
    for (const auto& channel : channels)
    {
        result.emplace_back(channel.samples, channel.adpcm ? std::optional(InfoOf(*channel.adpcm)) : std::nullopt);
    }

    return result;
}

// Whether every later pass of the loop of each channel decodes to the first pass's samples, `first`, by `decoding`.
bool LaterPassesMatch(const std::vector<AdpcmChannel>& channels, const std::vector<std::vector<int16_t>>& first,
                      uint32_t loop_start, uint32_t loop_end, const LoopDecoding& decoding);

// `bank` with each region's volume halved, rounded up.
sar::BankFile Halved(sar::BankFile bank);

// `bank` with a copy of each instrument at a new program after the bank's programs, and the programs of the copies by
// those of their instruments.
std::pair<sar::BankFile, std::vector<std::pair<uint32_t, uint32_t>>> WithCopies(sar::BankFile bank);

// Checks the notes of bank `bank` before its edits, in `before`, and after them, in `halved` and `copied`. `copies`
// gives the programs of the copies by those of their instruments.
void CheckNotes(const std::shared_ptr<const sar::SoundArchive>& before,
                const std::shared_ptr<const sar::SoundArchive>& halved,
                const std::shared_ptr<const sar::SoundArchive>& copied, uint32_t bank,
                const std::vector<std::pair<uint32_t, uint32_t>>& copies, const std::string& where, Totals& totals);

// Checks bank `bank` of the game's archive `game` with its volumes halved and with copies of its instruments, from
// `edited`. `edited` then holds the halved volumes. Returns the bank's model with them, or nothing when the bank took
// no edit.
std::optional<sar::BankFile> CheckBank(Edited& edited, const sar::SoundArchive& game, uint32_t bank,
                                       const std::string& name, Totals& totals);

// The multiple of `step` nearest `target`.
uint32_t Nearest(double target, uint32_t step);

// Checks the edits of a wave that start from `edited`: at a new rate with a new loop, then with its end back. `model`
// and `present` are the wave's model and its decoded samples. Returns the file and the archive with the end back.
Edited CheckLoop(const Edited& edited, uint32_t wave_archive, uint32_t wave, const format::WaveModel& model,
                 const sar::Pcm& present, const std::string& name, const std::string& where, Totals& totals);

// Checks the edit of a wave that starts from `edited` and gives it another encoding. `model` and `present` are the
// wave's model and its decoded samples.
void CheckEncoding(const Edited& edited, uint32_t wave_archive, uint32_t wave, const format::WaveModel& model,
                   const sar::Pcm& present, const std::string& name, const std::string& where, Totals& totals);

// Checks wave `wave` of wave archive `wave_archive` of the game's archive `game` from `edited`: at a new rate with a
// new loop, with its end back, and in another encoding. `edited` then holds the wave with its end back.
void CheckWave(Edited& edited, const sar::SoundArchive& game, uint32_t wave_archive, uint32_t wave,
               const std::string& name, Totals& totals);

// Checks the stream of file `file_id` that `edited` keeps: at a new rate with a loop at a block, and in the other
// encoding. `edited` then holds the stream with the loop.
void CheckStream(Edited& edited, uint32_t file_id, const std::string& name, Totals& totals);

// Whether two wave sounds' fields hold the same values and wave.
bool SameFields(const edit::WaveSoundFields& a, const edit::WaveSoundFields& b);

// Checks wave sound `sound` from `edited`: a new pan, surround pan, pitch and sends where its file has places for them,
// and the next of `waves`. `edited` then holds the new values.
void CheckWaveSound(Edited& edited, uint32_t sound, const std::vector<sar::WaveId>& waves, const std::string& name,
                    Totals& totals);

// Checks that every file of `edited`'s archive that no edit reaches holds the bytes of the game's archive `game`: every
// file but the banks, the wave archives, the wave sound files and the groups' files. A group's file starts with CGRP on
// the 3DS and FGRP on the Wii U and the Switch.
void CheckOtherFiles(const Edited& edited, const sar::SoundArchive& game, const std::string& name, Totals& totals);

// Whether `part` holds the item at `place` in its list.
bool Holds(const Part& part, std::size_t place);

// The summary of a run whose part `heading` names, after `seconds`.
void PrintSummary(const std::string& heading, const Totals& totals, double seconds);

// `value` moved by `step` within 0 to 127.
uint8_t Moved(uint8_t value, int step);

// Checks `bytes`, the archive of the Wii at `path`, with the part `part` of the items. Run checks the other consoles'
// archives in the same manner. The banks and the waves take edits with `items`, the wave sounds with `wave_sounds`, and
// the streams beside the archive with `streams`. The game's build of NW4R SND comes from the main.dol at `dol_path`, or
// else from the main.dol of the extracted disc that holds the archive (song::FindMainDol). Without either, the
// archive's version gives the build.
int RunWii(const std::filesystem::path& path, const std::vector<uint8_t>& bytes, const std::string& dol_path,
           const Part& part, bool items, bool wave_sounds, bool streams);

// The tool, with the command line's `arguments`. Returns the tool's exit status.
int Run(std::vector<std::string> arguments);

} // namespace hokuto::tools::banks
