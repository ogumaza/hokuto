// SPDX-License-Identifier: MIT

// The parts of hokuto_check_ds_replace that its files share: the totals, an archive of an image, the helpers of the
// rounds and the rounds. check_ds_replace.cpp describes the rounds.

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ntdlib/sar/formats.h"
#include "ntdlib/sdat/decode.h"
#include "ntdlib/sdat/files.h"
#include "ntdlib/sdat/formats.h"
#include "xsflib/binary.h"
#include "xsflib/console.h"
#include "xsflib/files.h"

#include "check_support.h"
#include "edit/ds_files.h"
#include "edit/ds_new_items.h"
#include "edit/ds_streams.h"
#include "edit/ds_tables.h"
#include "edit/ds_waves.h"
#include "edit/files.h"
#include "format/archive.h"
#include "format/file.h"
#include "format/text.h"
#include "song/dry_render.h"
#include "song/ds.h"
#include "song/items.h"
#include "song/sequence.h"
#include "song/sequence_model.h"

namespace hokuto::tools::ds_replace
{

namespace sar = ntdlib::sar;
namespace sdat = ntdlib::sdat;
namespace song = hokuto::song;
using hokuto::format::File;

constexpr double kPlaySeconds = 3.0;
constexpr std::size_t kImaStartSamples = 8;
constexpr uint32_t kPlayEvery = 32;
constexpr std::size_t kNewModels = 4;
constexpr uint16_t kNoWaveArchive = 0xFFFF;

// The ratio of signal to noise that the tool assigns to samples that decode exactly, in dB.
constexpr double kExactRatio = 1000.0;

// The words that the field of the loop start of a wave contains (ntdlib::sdat::EncodeWave).
constexpr uint32_t kMostLoopWords = 0xFFFF;

struct Totals
{
    std::size_t archives = 0;
    std::size_t sequences = 0;
    std::size_t banks = 0;
    std::size_t wave_archives = 0;
    std::size_t back_apart = 0;
    std::size_t next_files = 0;
    std::size_t next_apart = 0;
    std::size_t waves = 0;
    std::size_t wave_files_back = 0;
    std::size_t wave_files_apart = 0; // waves whose bytes in the file differ from their parameters and samples
    std::size_t laid_otherwise = 0;
    std::size_t unread_wave_archives = 0;
    std::size_t wavs = 0;
    std::size_t wavs_apart = 0;
    double lowest_ratio = 1000.0;
    std::size_t exact = 0;
    std::size_t encodings = 0;
    std::size_t encodings_apart = 0;
    std::size_t new_waves = 0;
    std::size_t new_waves_apart = 0;
    std::size_t played = 0;
    std::size_t silent = 0;
    std::size_t too_large = 0;
    std::size_t sequence_apart = 0;
    std::size_t new_sounds = 0;
    std::size_t new_banks = 0;
    std::size_t new_wave_archives = 0;
    std::size_t new_apart = 0;
    std::size_t new_played = 0;
    std::size_t new_silent = 0;
    std::size_t new_too_large = 0;
    std::size_t new_unread = 0; // models that can't render before the edit
    std::size_t value_edits = 0;
    std::size_t levels = 0;
    std::size_t levels_apart = 0;
    double worst_level = 0.0; // the largest difference from the ratio of the volumes, in dB
    std::size_t streams = 0;
    std::size_t streams_apart = 0;
    std::size_t stream_loops = 0;
    std::size_t stream_loops_moved = 0;
    double lowest_stream_ratio = 1000.0;
    std::size_t failed = 0;
};

// An archive of an image, with what the rounds share.
struct Game
{
    std::string name;
    File file;
    std::vector<uint8_t> bytes;
    std::shared_ptr<const song::DsArchive> archive;
    bool shares_file = false; // the archive shares its file in the image with other data (format::DsArchiveSharesFile)
};

// The bytes of each wave of `parsed` as the edit splits them: from each offset up to the next.
std::vector<std::vector<uint8_t>> WavesOf(const sdat::WaveArchive& parsed, std::span<const uint8_t> bytes);

// The sounds that hold a sequence file of their own, one for each file. A sequence archive's sounds share theirs.
std::vector<uint32_t> SequenceSounds(const Game& game);

// The first bank of each bank file, and the first wave archive of each wave archive file, with their file IDs.
std::vector<std::pair<uint32_t, uint32_t>> BanksOf(const Game& game);

std::vector<std::pair<uint32_t, uint32_t>> WaveArchivesOf(const Game& game);

void Fail(const Game& game, const std::string& round, const std::string& what, Totals& totals);

// Whether file `id` of `tables` shares bytes with another file of the FAT. The edits refuse new bytes for such a file
// (ntdlib::sdat::ReplaceFile).
bool SharesBytes(const sdat::Archive& tables, uint32_t id);

// Whether the edits may refuse `bytes`, a file of `Parsed`'s kind, in the place of file `id` of the game's archive, for
// a reason that the rounds' headers give: ntdlib can't read the game's file or `bytes`, the file shares bytes with
// another file, or the archive shares its file in the image with other data and `bytes` take another size.
template <class Parsed>
bool MayRefuse(const Game& game, uint32_t id, std::span<const uint8_t> bytes)
{
    const sdat::Archive& tables = game.archive->archive;
    try
    {
        Parsed::Parse(tables.File(id));
        Parsed::Parse(bytes);
    }
    catch (const std::exception&)
    {
        return true;
    }

    return SharesBytes(tables, id) || (game.shares_file && bytes.size() != tables.files[id].size);
}

// Each file is replaced with the bytes of Export….
void Back(const Game& game, Totals& totals);

// Each file is replaced with the next file of its kind.
void Next(const Game& game, Totals& totals);

// The ratio of the energy of `samples` to that of their difference from `decoded`, in dB. In this way, the tool
// measures the ratio of signal to noise that the Replace window shows. Nothing when `decoded` contains `samples`
// exactly, or when `samples` are silent.
std::optional<double> Ratio(std::span<const int16_t> samples, std::span<const int16_t> decoded);

sar::WaveEncoding ToSar(sdat::WaveEncoding encoding);

// Each wave is imported again from its files.
void Waves(const Game& game, uint32_t every, Totals& totals);

// The waves of every wave archive are converted to 16-bit PCM. The sounds' output stays. Only the wave archives that
// the played sounds' banks name go into the version that they play: each one lays the whole archive out again.
void Plays(const Game& game, Totals& totals);

// Sequence sounds go in again as new sounds, each with a new bank and new wave archives.
void NewItems(const Game& game, Totals& totals);

// The round of sound values.
void SoundValues(const Game& game, Totals& totals);

// The round of streams.
void Streams(const Game& game, Totals& totals);

} // namespace hokuto::tools::ds_replace
