// SPDX-License-Identifier: MIT

// The parts of hokuto_check_dse_banks that its files share: the format of a bank file, the totals, a game, the helpers
// and the rounds. check_dse_banks.cpp describes the rounds.

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "ntdlib/dse/formats.h"
#include "ntdlib/dse/layout.h"
#include "ntdlib/sequence_export/dse/convert.h"
#include "ntdlib/wave/decode.h"
#include "seqlib/midi/reader.h"
#include "xsflib/console.h"
#include "xsflib/files.h"

#include "app/item_views.h"
#include "edit/dse_banks.h"
#include "edit/dse_files.h"
#include "edit/files.h"
#include "format/archive.h"
#include "format/file.h"
#include "format/text.h"
#include "song/dse.h"
#include "song/preview.h"
#include "song/sequence.h"

namespace hokuto::tools::dse_banks
{

namespace dse = ntdlib::dse;
namespace edit = hokuto::edit;
using edit::DseSplit;
using hokuto::format::File;
using hokuto::song::DseSet;

constexpr int kHighest = 127;
constexpr std::size_t kMostPrograms = 128;
constexpr int kNoteVelocity = 127;

// The format of a bank file: its header, the counts in it, a chunk's header, and a program's parts.
constexpr std::size_t kBankHeader = 0x50;
constexpr std::size_t kWaveCount = 0x46;
constexpr std::size_t kProgramCount = 0x48;
constexpr std::size_t kChunkHeader = 16;
constexpr std::size_t kChunkSize = 12;
constexpr std::size_t kProgramHeader = 0x10;
constexpr std::size_t kProgramSplits = 0x02;
constexpr std::size_t kProgramLfos = 0x0B;
constexpr std::size_t kLfoSize = 0x10;
constexpr std::size_t kSplitSize = 0x30;

// The format of a wave's parameters.
constexpr std::size_t kWaveFormat = 0x13;
constexpr std::size_t kWaveLoops = 0x15;
constexpr std::size_t kWaveRate = 0x20;
constexpr std::size_t kWaveLoopStart = 0x28;
constexpr std::size_t kWaveLoopLength = 0x2C;
constexpr uint8_t kPcm8 = 0;
constexpr uint8_t kPcm16 = 1;
constexpr uint8_t kImaAdpcm = 2;
constexpr uint8_t kNoise = 4;

// An LFO's settings: whether it runs, and the driver's output that it moves. Output 2 moves a voice's level.
constexpr std::size_t kLfoType = 1;
constexpr std::size_t kLfoOutput = 2;
constexpr uint8_t kLevelOutput = 2;

// The semitones of a doubled rate, in the tuning's units.
constexpr double kTuningOfDoubledRate = 12.0 * 256.0;

// The replacements of --replace: how far a note that comes back from a MIDI file may be from its place, in ticks of 48
// to the quarter note, and the least ratio of signal to noise that counts an IMA-ADPCM wave as close.
constexpr int64_t kMidiTicks = 2;
constexpr uint32_t kMidiTimebase = 48;
constexpr double kLeastImaRatio = 20.0;

// The sounds and the waves that play after a replacement: up to 255 of each image, and 128 to 192 of a larger one.
constexpr std::size_t kSampledSounds = 128;
constexpr std::size_t kSampledWaves = 128;

// The renders: the note's length, and where the measures start.
constexpr double kRenderSeconds = 1.0;
constexpr double kMeasureFrom = 0.02;
constexpr double kShortestMeasure = 0.05;
constexpr double kQuietestDb = -36.0;
constexpr double kLevelTolerance = 1.0;
constexpr std::size_t kFewestCrossings = 40;
constexpr double kLeastOctaveRatio = 0.7;
constexpr double kMostOctaveRatio = 1.4;

// What the check counted, for the summary.
struct Totals
{
    std::size_t images = 0;
    std::size_t banks = 0;
    std::size_t splits = 0;
    std::size_t split_edits = 0;
    std::size_t waves = 0;
    std::size_t wave_edits = 0;
    std::size_t waves_refused = 0; // square waves and noise: they have to refuse the edit
    std::size_t waves_apart = 0;   // waves of one word of samples
    std::size_t edits_refused = 0; // edits of a wave that a table can't take
    std::size_t levels = 0;
    std::size_t levels_apart = 0;
    std::size_t root_pitches = 0;
    std::size_t rate_pitches = 0;
    std::size_t pitches_apart = 0;
    std::size_t pitch_checks_left_out = 0; // root keys outside 0 to 115, and the rates of square waves
    double lowest_ratio = 1.0;
    double highest_ratio = 0.0;
    std::size_t failed = 0;

    // The rounds of --replace.
    std::size_t sounds = 0;
    std::size_t sampled_sounds = 0;
    std::size_t same_sounds = 0;
    std::size_t sounds_apart = 0; // bytes after a track's end, or tracks that select none of the sound's banks
    std::size_t sounds_refused = 0;
    std::size_t next_sounds = 0;
    std::size_t next_refused = 0;
    std::size_t next_lengths = 0; // notes that the sound's bank ends elsewhere
    std::size_t next_counts = 0;  // tracks whose arrangement contains more or fewer notes
    std::size_t midi_sounds = 0;
    std::size_t midi_refused = 0;
    std::size_t midi_notes = 0;
    std::size_t midi_matched = 0;
    std::size_t midi_lengths = 0;  // matched notes whose lengths differ by more than kMidiTicks
    std::size_t midi_whole = 0;    // sounds whose notes all came back
    std::size_t midi_noteless = 0; // sounds whose MIDI files hold no note before the end
    std::size_t midi_arranged = 0; // the notes of the game's sounds in the arrangement
    std::size_t same_banks = 0;
    std::size_t next_banks = 0;
    std::size_t next_banks_refused = 0;
    std::size_t next_banks_apart = 0; // banks that no next bank fits
    std::size_t replaced_waves = 0;
    std::size_t sampled_waves = 0;
    std::size_t same_waves = 0;  // the export gave the image back
    std::size_t exact_waves = 0; // 8-bit and 16-bit PCM that decoded to the same samples
    std::size_t ima_waves = 0;
    std::size_t ima_below = 0; // IMA-ADPCM waves below kLeastImaRatio
    double lowest_ima = 1000.0;
    std::size_t half_rates = 0;
    std::size_t half_tunings = 0;
    std::size_t half_refused = 0;
};

// An image and what the checks read from it.
struct Game
{
    std::string name;
    File file;
    std::vector<uint8_t> bytes;
    std::vector<uint64_t> starts;
    ntdlib::dse::Build build = ntdlib::dse::Build::kLater;
    std::shared_ptr<const DseSet> set;
};

// Where a split is in a bank file.
struct SplitAt
{
    uint32_t program = 0;
    std::size_t split = 0;
    std::size_t offset = 0;
};

// Where a bank file stores the parameters of its waves, by their slots, and its splits. The walk reads the file's bytes
// alone, not ntdlib's reader.
struct Walk
{
    std::map<uint32_t, std::size_t> waves;
    std::vector<SplitAt> splits;
};

uint16_t Get16(std::span<const uint8_t> bytes, std::size_t at);

uint32_t Get32(std::span<const uint8_t> bytes, std::size_t at);

void Put32(std::span<uint8_t> bytes, std::size_t at, uint32_t value);

Walk WalkBank(std::span<const uint8_t> file);

// The index in the set's banks of each bank that the set contains once, the first file of its bytes.
std::vector<uint32_t> DistinctBanks(const DseSet& set);

void Fail(const Game& game, Totals& totals, const std::string& message);

// `file` comes back from both its forms and produces `exported`.
void CheckForms(const Game& game, const File& file, const std::vector<uint8_t>& exported, const char* round,
                Totals& totals);

void CheckExport(const Game& game, const std::vector<uint8_t>& exported, const std::vector<uint8_t>& expected,
                 const char* round, Totals& totals);

// The new bytes of a split of a bank whose table contains the slots `waves`.
using Change = std::function<DseSplit(const DseSplit&, const std::vector<uint32_t>& waves)>;

DseSplit Envelopes(const DseSplit& old, const std::vector<uint32_t>&);

DseSplit Values(const DseSplit& old, const std::vector<uint32_t>&);

DseSplit Keys(const DseSplit& old, const std::vector<uint32_t>&);

DseSplit Waves(const DseSplit& old, const std::vector<uint32_t>& waves);

void Round(const Game& game, const char* name, const Change& change, Totals& totals);

uint32_t SamplesInWord(uint8_t format);

uint32_t FirstWord(uint8_t format);

void WaveRound(const Game& game, Totals& totals);

// The place of a note that one split alone plays at velocity 127, in the split's range nearest to key 60.
struct Note
{
    uint32_t bank = 0;
    uint32_t program = 0;
    std::size_t split = 0;
    int key = 60;
};

void Renders(const Game& game, Totals& totals);

// The paths of the files whose bytes start at `offsets` in the set's bytes.
std::vector<std::string> PathsAt(std::span<const uint8_t> set, const std::vector<uint64_t>& offsets);

// Whether every file of `after` but those at `changed` has its bytes in `before`, and the two sets contain the same
// paths.
bool OthersKept(std::span<const uint8_t> before, std::span<const uint8_t> after,
                const std::vector<std::string>& changed);

// Each sound is replaced with the song file of its tracks, then the song file of the next sound of its kind, then its
// MIDI file.
void SoundRounds(const Game& game, Totals& totals);

// Each bank is replaced with its file, then with the file of the next bank that contains samples where it does.
void BankRounds(const Game& game, Totals& totals);

// Each wave of each bank with samples is exported as its WAV file and imported again in its place, in its encoding. It
// then comes back at half its rate as 16-bit PCM.
void WaveReplaceRound(const Game& game, Totals& totals);

} // namespace hokuto::tools::dse_banks
