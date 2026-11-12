// SPDX-License-Identifier: MIT

// What a sequence sound of the 3DS, the Wii U or the Switch becomes on the DS, for edit::ForDs (edit/other_console.h).
// The commands are written as NITRO-Composer reads them, and a bank is written as an SBNK and an SWAR of the DS.
//
// NITRO-Composer plays 48 ticks to a quarter note, and has no timebase. The conversion drops a timebase B at the start
// of the song, and multiplies each tempo by B / 48. On the DS, a sound starts from the first command of the sequence
// file. The tracks that the later consoles' sound table allocates go in as a command at the file's start, and a jump
// there leads to the sound's start where other commands stand before it. The later consoles' allocation command does
// nothing, and stays out. So do the commands that the sound doesn't reach. The commands that the DS's bytes don't hold
// stay out: the sends, the filters and LFOs 1 to 3 among them. On the DS, a setting reaches its value at once. The
// conversion drops the time of the setting. The bytes of the DS store a volume from 0 to 127. A value outside that
// range is clamped to it. An envelope's reset becomes settings of 255, and the DS reads those as the region's values. A
// track's variables, 32 to 47, have no place on the DS. They take the sound's variables that the commands leave free.
// Tracks that share a number then share the variable.
//
// A region of the DS has a wave, a root key, an envelope and a pan. The attack, decay, sustain and release of the later
// consoles keep the same values on the DS: the tables of both follow the table of the DS, and the DS steps them each
// 5.21 ms where the tables of the later consoles count 5 ms. A region's hold, its key group and its interpolation have
// no place. A note of the DS plays one region at a key. The velocity region with the widest run of velocities plays
// each key, and the higher of two such regions. A note-off that the region ignores becomes a release of 255: the driver
// then plays the note to the end of its wave. The pitch of a region moves into the rate of its wave, and its volume
// into the samples of the wave. A wave that two regions play with other pitches or volumes is written once for each. A
// stereo wave is written as the mean of its channels. The samples can't pass 16 bits. Where a volume or the gain of a
// stereo wave would take them past, every wave of the bank gets the same lower level (`scale`), and the regions keep
// their balance.
//
// Waves go in as IMA-ADPCM. A loop runs to the wave's end, and starts at a word: 8 samples (ntdlib::sdat::EncodeWave).

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/ntr/snd/driver.h"
#include "ntdlib/sar/bank_file.h"
#include "ntdlib/sar/wave_file.h"

#include "edit/waves.h"
#include "song/archive.h"

namespace hokuto::edit
{

// The commands of a sequence sound as the DS reads them, and what changed on the way. The sequence plays from item 0,
// and contains no choices of an encoder.
struct DsCommands
{
    ntdlib::commands::Sequence sequence;

    // The commands that stayed out, by what they set (song::SettingName). Each one played otherwise on the source.
    std::map<std::string, std::size_t> left_out;

    std::size_t times = 0;       // the settings that lost their times
    std::size_t held_values = 0; // the values past what the DS's bytes hold, and the velocities above 127

    // The timebase of the song's start, the tempos from variables that it can't scale, and whether a timebase runs
    // later or differs from the first.
    int64_t timebase = 48;
    std::size_t unscaled_tempos = 0;
    bool timebase_changes = false;

    // The tracks' variables that took a sound's variable, and those for which none was free.
    std::map<int, int> moved_variables;
    std::vector<int> stuck_variables;

    // The deepest calls and counted loops inside one another that a track reaches, up to kDsNestLimit + 1.
    int deepest_nest = 0;
};

// The calls and counted loops inside one another that the driver supports. It skips a deeper call or loop.
inline constexpr int kDsNestLimit = ntdlib::ntr::snd::kCallDepth;

// The channels of the DS.
inline constexpr std::size_t kDsChannels = ntdlib::ntr::snd::kChannelCount;

// `sequence` as the DS reads it. `sequence` is a sequence of the console `from`. The sound of `sequence` starts at item
// `start` and allocates `tracks`, a bit each. Throws std::invalid_argument with a message for the user for commands
// that the DS's bytes can't hold once they have gone through these steps.
DsCommands AsDs(const ntdlib::commands::Sequence& sequence, uint32_t start, uint32_t tracks, const song::Console& from);

// Adds to `warnings` what `commands` changed on the way to the DS from the console `from` (AsDs).
void AddDsCommandWarnings(const DsCommands& commands, const song::Console& from, std::vector<std::string>& warnings);

// The rate at which the DS mixes. The sound unit reads a channel's sample once in each of its output samples. A wave at
// a higher rate uses more memory, and its samples between stay unread.
inline constexpr uint32_t kDsMixRate = 32728;

// The bytes that the waves of a bank take on the DS, before it encodes them: half a byte for each sample, and the
// parameters and the state of each wave. A wave counts at `most_rate` where it plays at a higher rate. Each wave of
// `waves` that an instrument of `bank` plays counts once for each rate and volume at which its regions play it.
std::size_t DsWavesSize(const ntdlib::sar::BankFile& bank, std::span<const std::optional<ntdlib::sar::WaveFile>> waves,
                        uint32_t most_rate = kDsMixRate);

// A bank as a bank file and a wave archive file of the DS, and what changed on the way.
struct DsBankFiles
{
    std::vector<uint8_t> bank;
    std::vector<uint8_t> waves; // empty for a bank that plays no wave
    std::size_t wave_count = 0; // the waves that `waves` contains

    // The level that every wave took to keep its samples within 16 bits, from above 0 to 1.
    double scale = 1.0;

    std::size_t stereo_waves = 0;    // the waves that went in as the means of their channels
    std::size_t velocity_splits = 0; // the keys' runs whose other velocity regions stayed out
    std::size_t holds = 0;           // the regions whose hold lengthens their envelope
    std::size_t key_groups = 0;      // the regions in a key group
    std::size_t lost_loops = 0;      // the waves whose loops start past the 65,535 words of the wave's field
    std::size_t rates_held = 0;      // the regions whose rate a wave doesn't support, clamped to the nearest
    std::size_t rates_lowered = 0;   // the waves above `most_rate`
    std::size_t silent_regions = 0;  // the regions whose waves are missing from `waves` or hold no samples
};

// The files of the DS for `bank`. The table of waves of `bank` has a place for each entry of `waves`. `stereo_gain`
// returns the gain of the mean of a stereo wave at a level. The bank's programs keep their numbers. An instrument is
// written as one region where one region plays every key, or else as a key split of up to 8 runs of keys, with a region
// that plays nothing in a gap. More runs take a drum set from its first key to its last.
//
// A wave above `most_rate` without a loop is resampled to `most_rate`. A loop on the DS fills whole words of 8 samples,
// and ntdlib::sdat::EncodeWave repeats a loop until it does: up to 8 times its size. A looped wave above `most_rate`
// therefore gets the whole words nearest the length of the loop at `most_rate`, and the rate that keeps its pitch
// (audio::Resample). At `most_rate` or below, a loop of 64 samples or more that doesn't fill whole words gets the
// nearest length that does, and the rate that keeps its pitch too. A shorter loop repeats. A looped wave keeps its rate
// where that nearest length is the loop's length, or where the new rate is outside the DS's range of 256 to 192,000 Hz.
DsBankFiles DsBankFilesOf(const ntdlib::sar::BankFile& bank,
                          std::span<const std::optional<ntdlib::sar::WaveFile>> waves,
                          const std::function<double(const MonoLevel&)>& stereo_gain, uint32_t most_rate = kDsMixRate);

// Adds to `warnings` what changed on the way to `files`, in words for the user. With `at_mix_rate`, the waves that went
// in at the DS's rate from a higher one count too.
void AddDsBankWarnings(const DsBankFiles& files, bool at_mix_rate, std::vector<std::string>& warnings);

} // namespace hokuto::edit
