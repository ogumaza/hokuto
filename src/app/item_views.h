// SPDX-License-Identifier: MIT

// What the Bank Editor and the properties of a wave or a stream show of their items, worked out without a window, and
// the edits of their gestures that need no window: the waves that a drop places on a bank, and the places of a loop's
// markers.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/rsar/formats.h"
#include "ntdlib/sar/bank_file.h"
#include "ntdlib/sar/formats.h"

#include "edit/waves.h"
#include "format/file.h"

namespace hokuto::app
{

// What the Bank Editor shows of a bank. The program fills in the sizes once it has measured them.
struct BankView
{
    uint32_t bank = 0; // the bank's place in the archive's list
    std::string name;
    ntdlib::sar::BankFile model;
    std::vector<std::string> entries;       // the name of the wave of each entry of the bank's table of waves
    std::vector<ntdlib::sar::WaveId> waves; // the archive's waves
    std::vector<std::string> wave_names;    // the names of `waves`, for the menu of a region's wave
    bool interpolation = true;              // whether the console reads a region's interpolation
    bool wii = false;                       // a bank of the Wii
    bool ctr = false;                       // a bank of the 3DS
    bool hold_and_groups = true;            // whether the console reads a region's hold, key group and note-off
    std::vector<std::string> sounds;        // the sequence sounds with the bank in a slot
    std::vector<std::string> lacking;       // the groups that load the bank and lack a wave archive that it names
    std::vector<std::string> sizes;         // the bank's size, and each group's that loads it
};

// The view of bank `bank` of `archive`, the archive that `file` builds. Throws std::invalid_argument with a message for
// the user where edit::BankModelOf refuses.
BankView MakeBankView(const format::File& file, const ntdlib::sar::SoundArchive& archive, uint32_t bank);

// The view of bank `bank` of the Wii's `archive`, the archive that `file` builds, in the model of edit/banks.h. The
// menu of a region's wave lists the waves of the archive's wave archives (song::WaveArchivesOf). The sequence sounds
// that play the bank's file, through this bank or another, change with an edit. The Wii reads no interpolation, and the
// file's build of NW4R SND gives whether the console reads the hold, the key group and the note-off. Throws as
// edit::BankModelOf does.
BankView MakeBankView(const format::File& file, const ntdlib::rsar::SoundArchive& archive, uint32_t bank);

// Where something dropped on the Bank Editor lands: the map of the instrument at `program`, at `key` and `velocity`, or
// the list of programs without a program.
struct BankDrop
{
    std::optional<uint32_t> program;
    int key = 60;
    int velocity = 64;
};

// A bank after waves dropped on the Bank Editor, and the action's name in the Undo History. `programs` lists the
// programs that the drop changes, in order. `region` is the first region that the drop makes or changes in the first of
// them, by its first key and its first velocity.
struct DroppedWaves
{
    ntdlib::sar::BankFile bank;
    std::string name;
    std::vector<uint32_t> programs;
    std::pair<int, int> region;
};

// `programs` as the Undo History names them: "program 9", "programs 1 and 3", "programs 14 to 16" for a run of three or
// more, or else a list such as "programs 1, 3 and 5".
std::string ProgramsText(const std::vector<uint32_t>& programs);

// The first program of `bank` from `from` up that holds no instrument, or nothing past the highest that the engines
// select.
std::optional<uint32_t> FreeProgram(const ntdlib::sar::BankFile& bank, uint32_t from);

// `bank` with `waves` placed at `drop`. On the list of programs, or on the map of a program without an instrument, each
// wave makes an instrument at the next free program, from the drop's program up, with one region over every key and
// velocity that plays the wave at key 60. One wave dropped on a region gives the region the wave. Other drops on a map
// make a region of one key at each wave, from the key under the drop up, over every velocity. Each such region plays
// its wave at its key. A drum kit is such a map. Throws std::invalid_argument with a message for the user for a key
// that a region covers already, keys past 127, and programs past the highest.
DroppedWaves WithDroppedWaves(ntdlib::sar::BankFile bank, const BankDrop& drop,
                              const std::vector<ntdlib::sar::WaveId>& waves);

// The values of a wave or a stream that its properties edit. A wave plays up to its loop's end, with a loop or without
// one. A stream plays up to its end, and its loop runs to its end too: `loop_end` is then the stream's length. Without
// a loop, the loop starts at 0.
struct WaveValues
{
    friend bool operator==(const WaveValues&, const WaveValues&) = default;

    ntdlib::sar::WaveEncoding encoding = ntdlib::sar::WaveEncoding::kDspAdpcm;
    uint32_t rate = 0;
    bool loop = false;
    uint32_t loop_start = 0;
    uint32_t loop_end = 0;
};

// The least and the most sample of each run of kPeakRun samples of each channel, for a waveform drawn at a scale where
// a point covers many samples.
using Peaks = std::vector<std::vector<std::pair<int16_t, int16_t>>>;

// The samples of a run of a waveform's peaks.
inline constexpr std::size_t kPeakRun = 256;

// The peaks of `samples`, each channel's samples.
Peaks PeaksOf(const std::vector<std::vector<int16_t>>& samples);

// What the properties of a wave or a stream show. The samples are those that the item holds, decoded as the console
// decodes them, up to the most that its end can move to (`held`). `encodings` lists the encodings that the item can
// take. A stream's properties add its tracks and its regions. `reach` says what else an edit changes, and `sizes` gives
// the item's size, and each group's that loads it. The program adds the groups' sizes once it has measured them.
struct WaveView
{
    std::string name;
    bool stream = false;
    WaveValues values;
    uint32_t held = 0;
    std::vector<std::vector<int16_t>> samples;
    Peaks peaks;
    std::vector<ntdlib::sar::WaveEncoding> encodings;
    std::vector<std::string> tracks;
    std::string regions;
    std::string reach;
    std::vector<std::string> sizes;
};

// The samples that a loop's start steps by in `encoding` where the samples stay: a frame of DSP-ADPCM in a wave, and a
// block in a stream (ntdlib's StreamBlockSamples).
uint32_t LoopStep(ntdlib::sar::WaveEncoding encoding, bool stream);

// The sizes of the groups that load file `file_id` of `archive`, the archive that `file` builds with its group files,
// as lines: the bytes of the files that each group loads, and how many more or fewer than the game's group loads
// (format::GameSizes). No lines for a file that no group loads, or where ntdlib can't read the groups. The sizes are
// those of the archive that an export writes: its timelines take the converter's subroutines again
// (edit::FoldTimelines). The Export window measures the same sizes. `archive` gives the groups. The editors show the
// lines beside an item's size. The program measures them on a thread of their own.
std::vector<std::string> GroupSizeLines(const format::File& file, const ntdlib::sar::SoundArchive& archive,
                                        uint32_t file_id);

// The same for file `file_id` of the Wii's archive that `file` exports: each group that holds it, and the bytes of the
// files and the wave data that the group holds. The file keeps no sizes of the game's groups on the Wii. Throws the
// exceptions of format::ExportArchive and song::LoadWiiArchive.
std::vector<std::string> WiiGroupSizeLines(const format::File& file, uint32_t file_id);

// The properties of wave `wave` of wave archive `wave_archive` of `archive`, the archive that `file` builds. Throws
// std::invalid_argument with a message for the user for a wave that Hokuto can't read.
WaveView MakeWaveView(const format::File& file, const ntdlib::sar::SoundArchive& archive, uint32_t wave_archive,
                      uint32_t wave);

// The properties of wave `wave` of the wave data of file `file_id` of the Wii's `archive`, the archive that `file`
// builds: a bank's or a wave sound file's (song::WaveArchivesOf). Throws std::invalid_argument with a message for the
// user for a wave that Hokuto can't read.
WaveView MakeWaveView(const format::File& file, const ntdlib::rsar::SoundArchive& archive, uint32_t file_id,
                      uint32_t wave);

// A number with a comma between each group of three digits, such as 12,345.
std::string Grouped(uint64_t number);

// A pan from 0 to 127 as a stream track's line gives it: "the centre", or the side and the steps from the centre, such
// as "12 left".
std::string PanName(int pan);

// A sentence's list of the channels of a stream's track, counted from 1: "channel 1", "channels 1 and 2".
std::string ChannelsOf(const std::vector<uint8_t>& channels);

// The message for the user when the file holds no stream for stream sound `sound` and its stream file `file`: the
// stream wasn't beside the archive at its import.
std::string MissingStreamText(const std::string& sound, const std::string& file);

// The properties of the stream of stream sound `sound` of `archive`, the archive that `file` builds. Throws
// std::invalid_argument with a message for the user for a sound without a stream that the file keeps
// (MissingStreamText), and a stream that ntdlib can't read.
WaveView MakeStreamView(const format::File& file, const ntdlib::sar::SoundArchive& archive, uint32_t sound);

// The properties of the stream of stream sound `sound` of the Wii's `archive`, the archive that `file` builds. The
// stream file gives the tracks, and the file's build of NW4R SND gives the tracks that the sound plays
// (ntdlib::rvl::snd::StrmPlayer::TracksOf). Throws std::invalid_argument with a message for the user for a sound
// without a stream that the file keeps, and a stream that ntdlib can't read.
WaveView MakeStreamView(const format::File& file, const ntdlib::rsar::SoundArchive& archive, uint32_t sound);

// What the Sound section shows of a wave sound's values in its wave sound file (edit/waves.h), with the archive's waves
// for the menu of the sound's wave. `reach` says what else an edit changes, and `sizes` gives the file's size, and each
// group's that loads it. The program adds the groups' sizes once it has measured them.
struct WaveSoundView
{
    edit::WaveSoundFields fields;
    std::vector<ntdlib::sar::WaveId> waves;
    std::vector<std::string> wave_names;
    bool hold = true;     // whether the console reads the envelope's hold
    int biquad_types = 5; // the highest type of biquad filter that the console has: 10 on the Switch
    bool ctr = false;     // whether the archive is the 3DS's
    bool wii = false;     // whether the archive is the Wii's
    std::string reach;
    std::vector<std::string> sizes;
};

// The view of wave sound `sound` of `archive`. Nothing for a sound that isn't a wave sound, and for one whose values
// Hokuto can't read (edit::WaveSoundFieldsOf).
std::optional<WaveSoundView> MakeWaveSoundView(const ntdlib::sar::SoundArchive& archive, uint32_t sound);

// The view of the Wii's wave sound `sound` of `archive`, the archive that `file` builds. The menu of the sound's wave
// lists the waves of its file's wave data. The file's build of NW4R SND gives whether the console reads the hold.
// Nothing as for the other consoles.
std::optional<WaveSoundView> MakeWaveSoundView(const format::File& file, const ntdlib::rsar::SoundArchive& archive,
                                               uint32_t sound);

// The place of a loop's marker nearest `target`: a whole number of `step` samples, from `least` to `most`. With `snap`,
// the place within `reach` samples of `target` where the channels of `samples` lie nearest 0, the nearest to `target`
// of equals. A place past the last sample counts as the last sample. Without such a place, or without `snap`, the place
// nearest `target`. Nothing when no place lies from `least` to `most`.
std::optional<uint32_t> LoopMarkerPlace(const std::vector<std::vector<int16_t>>& samples, double target, uint32_t step,
                                        uint32_t least, uint32_t most, bool snap, double reach);

// The values of `view`'s item after the loop's switch turns the loop on or off. Without a loop, the loop starts at 0. A
// loop that comes on starts at `before`, the start of the loop that went off, at the place nearest it that the encoding
// allows before the end (LoopMarkerPlace). Without `before`, or without such a place, it starts at 0.
WaveValues WithLoopSwitched(const WaveView& view, bool on, std::optional<uint32_t> before);

} // namespace hokuto::app
