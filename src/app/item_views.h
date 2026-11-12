// SPDX-License-Identifier: MIT

// What the Bank Editor and the properties of a wave or a stream show of their items, worked out without a window, and
// the edits of their gestures that need no window: the waves that a drop places on a bank, and the places of a loop's
// markers.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ntdlib/mp2k/song.h"
#include "ntdlib/rsar/formats.h"
#include "ntdlib/sar/bank_file.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sdat/formats.h"

#include "edit/dse_banks.h"
#include "edit/gba_voices.h"
#include "edit/waves.h"
#include "format/file.h"
#include "song/driver.h"
#include "song/ds.h"
#include "song/dse.h"
#include "song/gba.h"

namespace hokuto::app
{

// What the Bank Editor shows of a bank. The program fills in the sizes once it has measured them.
struct BankView
{
    uint32_t bank = 0; // the bank's place in the archive's list
    std::string name;
    ntdlib::sar::BankFile model;
    std::vector<std::string> entries;                // the name of the wave of each entry of the bank's table of waves
    std::vector<ntdlib::sar::WaveId> waves;          // the archive's waves
    std::vector<std::string> wave_names;             // the names of `waves`, for the menu of a region's wave
    bool interpolation = true;                       // whether the console reads a region's interpolation
    format::Engine engine = format::Engine::kSwitch; // the engine of the bank's archive
    bool hold_and_groups = true;      // whether the console reads a region's hold, key group and note-off
    std::vector<std::string> sounds;  // the sequence sounds with the bank in a slot
    std::vector<std::string> lacking; // the groups that load the bank and are missing a wave archive that it names
    std::vector<std::string> sizes;   // the bank's size, and each group's that loads it
};

// The view of bank `bank` of `archive`, the archive that `file` builds. Throws std::invalid_argument with a message for
// the user where edit::BankModelOf refuses.
BankView MakeBankView(const format::File& file, const ntdlib::sar::SoundArchive& archive, uint32_t bank);

// The kind of a GBA voice: a sample, a fixed sample, one of the PSG channels, a key split or a drum kit. A PSG voice of
// another channel is kUnknown. song::GbaVoiceKind returns the name of each kind.
enum class GbaVoiceType
{
    kSample,
    kFixedSample,
    kSquare1,
    kSquare2,
    kWave,
    kNoise,
    kKeySplit,
    kDrumKit,
    kUnknown,
};

// The kind of `voice`, by the same rules as song::GbaVoiceKind.
GbaVoiceType GbaVoiceTypeOf(const ntdlib::mp2k::Voice& voice);

// A voice of a GBA voice group as the Bank Editor shows it: the keys that play it, its values, and its kind
// (song::GbaVoiceKind). `shared` names the other places of the ROM's voice groups that play the voice's bytes: a
// program, or a key of a key split or a drum kit.
struct GbaVoiceRow
{
    int first_key = 0;
    int last_key = 127;
    ntdlib::mp2k::Voice voice;
    std::string kind;
    std::vector<std::string> shared;
};

// A program of a GBA voice group: its voice, and the voices of a key split's runs of keys or of a drum kit's keys. Only
// the keys whose voices play something have rows (song::GbaVoiceSounds). A drum kit's table can run on past its voices
// into other bytes, such as a key split's key table or a wave's pattern. A key whose voice overlaps them has no row.
struct GbaProgram
{
    uint32_t program = 0;
    GbaVoiceRow voice;
    std::vector<GbaVoiceRow> rows;
};

// The rows of `program` for the edits of its tables.
std::vector<edit::GbaKeyRow> KeyRowsOf(const GbaProgram& program);

// A sample of a GBA ROM: its address, its rate at key 60 in Hz, whether it loops, and its samples.
struct GbaSample
{
    uint32_t address = 0;
    double rate = 0.0;
    bool loops = false;
    uint32_t size = 0;
};

// What the Bank Editor shows of a GBA voice group: the programs that the songs select (song::GbaVoiceGroups), the ROM's
// samples for the menu of a voice's sample, the patterns that the group's wave voices play, each with the places that
// play it, and the songs that play the group. `song` is the first of them: the keyboard plays the voices with its music
// player, priority and reverb. `free_programs` lists the programs that a new voice can use (edit::FreeGbaPrograms).
struct GbaBankView
{
    uint32_t address = 0;
    std::string name;
    std::vector<GbaProgram> programs;
    std::vector<GbaSample> samples;
    std::vector<std::string> sample_names;
    std::map<uint32_t, edit::GbaPattern> patterns;
    std::map<uint32_t, std::vector<std::string>> pattern_places;
    std::vector<std::string> songs;
    int song = -1;
    std::vector<uint32_t> free_programs;
};

// The name that the Bank Editor gives the sample at `address`, such as "Sample 0x0868C3D4".
std::string GbaSampleName(uint32_t address);

// The view of the voice group at `address` of `rom`. Throws std::invalid_argument with a message for the user when no
// song of the ROM plays the voice group.
GbaBankView MakeGbaBankView(const song::GbaRom& rom, uint32_t address);

// The moves of GBA voice groups' tables: each address with the addresses that a move took its table to or brought it
// from, the latest last. The Bank Editor follows a group through Undo and Redo with them.
using GbaGroupMoves = std::map<uint32_t, std::vector<uint32_t>>;

// Records the move of a voice group's table from `from` to `to` in `moves`.
void RecordGbaGroupMove(GbaGroupMoves& moves, uint32_t from, uint32_t to);

// The address of the voice group that the Bank Editor shows at `address`, in a ROM whose songs play `groups`: `address`
// itself while a song plays a group there, or else the latest address in `moves` that a move took the table to or
// brought it from and that a song plays. Without one, `address`.
uint32_t FollowedGbaGroup(const GbaGroupMoves& moves, uint32_t address, const std::vector<song::GbaVoiceGroup>& groups);

// A region of a DS instrument as the Bank Editor shows it: the keys that play it, its place among the instrument's
// regions, its values, the name of its kind (DsKindName), and the other places of the bank that play its bytes: a
// program, a drum set's key or a key split's run of keys.
struct DsRegionRow
{
    int first_key = 0;
    int last_key = 127;
    std::size_t region = 0;
    ntdlib::sdat::Region values;
    std::string kind;
    std::vector<std::string> shared;
};

// A program of a DS bank: the kind of its instrument, and its regions. An instrument of one region has one row over
// every key. A drum set has a row for each key, and a key split for each run of keys. A region of kind kNone has no
// row.
struct DsProgram
{
    uint32_t program = 0;
    ntdlib::sdat::InstrumentType type = ntdlib::sdat::InstrumentType::kNone;
    std::string kind;
    std::vector<DsRegionRow> rows;
};

// A wave that a region of a DS bank can play: the slot of the bank's wave archive that contains it, and its index
// there.
struct DsWaveChoice
{
    friend bool operator==(const DsWaveChoice&, const DsWaveChoice&) = default;

    uint16_t slot = 0;
    uint16_t wave = 0;
};

// What the Bank Editor shows of a DS bank: the bank's instruments, the programs that contain one, the waves of the wave
// archives in the bank's slots for the menu of a region's wave, with their names, the other banks of the archive that
// play the bank's file, and the sequences that play the bank or one of those banks. `slots` contains the wave archive
// of each of the bank's four slots, or 0xFFFF for none. `wave_archives` and `wave_archive_names` list the archive's
// wave archives for the menus of the slots.
struct DsBankView
{
    uint32_t bank = 0;
    std::string name;
    ntdlib::sdat::Bank model;
    std::array<uint16_t, 4> slots = {};
    std::vector<uint32_t> wave_archives;
    std::vector<std::string> wave_archive_names;
    std::vector<DsProgram> programs;
    std::vector<DsWaveChoice> waves;
    std::vector<std::string> wave_names;
    std::vector<std::string> sharing_banks;
    std::vector<std::string> sounds;
};

// A split of a DSE program in the Bank Editor: its place among the program's splits, its bytes, and the name of its
// wave.
struct DseSplitRow
{
    std::size_t split = 0;
    edit::DseSplit values{};
    std::string wave;
};

// A program of a DSE bank: its number and its splits.
struct DseProgramView
{
    uint32_t program = 0;
    std::vector<DseSplitRow> rows;
};

// What the Bank Editor shows of a DSE bank: its programs, the waves of its table for the menu of a split's wave, with
// their names, and the sounds that play the bank.
struct DseBankView
{
    uint32_t bank = 0;
    std::string name;
    std::vector<DseProgramView> programs;
    std::vector<uint16_t> waves;
    std::vector<std::string> wave_names;
    std::vector<std::string> sounds;
};

// The view of bank `bank`, an index in `set`'s banks. Throws std::invalid_argument with a message for the user for a
// bank that is missing from the set.
DseBankView MakeDseBankView(const song::DseSet& set, uint32_t bank);

// Whether the songs of a ROM's samples are read: a thread still reads them, they're read, or they can't be read.
enum class DriverSongsState
{
    kReading,
    kRead,
    kUnreadable,
};

// What the Bank Editor shows of a sample of one of the GBA's other drivers, such as Konami's: its number, its rate at
// semitone 0 in Hz, its length and its loop's start in samples, and the songs that play it. On a driver whose samples
// the editor edits (edit/konami_samples.h), `editable` is set, the rate runs from `least_rate` to `most_rate`, and
// `every_pitch` says whether every pitch of a note plays at it. `loops` says whether the sample can loop, and
// `no_loop_reason` why it can't. `sharing` lists the other samples that an edit changes. `rate_songs` lists the other
// songs whose notes of other samples take the sample's rate (song::KonamiBorrowedRates): an edit of the rate changes
// them too. `songs_state` says whether the two lists are read yet.
struct DriverSampleView
{
    uint32_t sample = 0;
    int rate = 0;
    uint32_t length = 0;
    std::optional<uint32_t> loop_start;
    std::vector<std::string> songs;
    std::vector<std::string> rate_songs;
    bool editable = false;
    uint32_t least_rate = 0;
    uint32_t most_rate = 0;
    bool every_pitch = true;
    bool loops = false;
    std::string no_loop_reason;
    std::vector<std::string> sharing;
    DriverSongsState songs_state = DriverSongsState::kReading;
};

// What the Bank Editor shows of a ROM with such a driver: the samples that its songs play.
struct DriverBankView
{
    std::vector<DriverSampleView> samples;
};

// The songs that play each sample of a ROM, by the sample's number. `rate_songs` contains the songs whose notes of
// other samples take the sample's rate. Reading them reads each song's notes: up to a second on a large ROM.
struct DriverSampleSongs
{
    DriverSongsState state = DriverSongsState::kReading;
    std::map<int, std::vector<std::string>> songs;
    std::map<int, std::vector<std::string>> rate_songs;
};

// The songs of the samples of `rom`.
DriverSampleSongs SongsOfDriverSamples(const song::DriverRom& rom);

// The view of the samples of `rom`, with the songs of `songs`.
DriverBankView MakeDriverBankView(const song::DriverRom& rom, const DriverSampleSongs& songs);

// Sets the songs of each sample of `view` from `songs`, and the state of the songs. A sample that `songs` doesn't hold
// shows no song.
void ShowDriverSampleSongs(DriverBankView& view, const DriverSampleSongs& songs);

// The name of the kind of a DS instrument or of a region: Wave, Square, Noise, Wave by address, Silent, Drum set or Key
// split.
std::string DsKindName(ntdlib::sdat::InstrumentType type);

// The view of bank `bank` of `archive`. Throws std::invalid_argument with a message for the user for a bank that is
// missing from the archive, and xsflib::FormatError for a bank file that can't be read.
DsBankView MakeDsBankView(const song::DsArchive& archive, uint32_t bank);

// The first key of the row of program `program` of `bank` that plays `key` (DsProgram::rows), or nothing where no row
// plays it. After an edit of a program's structure, the Bank Editor selects the row that now covers the keys of the
// edited row.
std::optional<int> DsRowOf(const ntdlib::sdat::Bank& bank, uint32_t program, int key);

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

// A bank after the Bank Editor's Add, and the program of the new instrument.
struct AddedInstrument
{
    ntdlib::sar::BankFile bank;
    uint32_t program = 0;
};

// `bank` with a new instrument at the first free program from `from` up (FreeProgram). The instrument has no regions,
// except on the Wii. NW4R SND finds no instrument at a program without a region, and a bank file of the Wii keeps none
// there. A bank of the Wii (`wii`) therefore gets an instrument with one region over every key and velocity. The region
// plays the first wave of the bank's table at key 60. A wave dropped on the list makes such an instrument too. Throws
// std::invalid_argument with a message for the user for a bank without a free program from `from` up, and a bank of the
// Wii without waves.
AddedInstrument WithAddedInstrument(ntdlib::sar::BankFile bank, uint32_t from, bool wii);

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
    uint32_t ds_block_samples = 0; // a DS stream: the samples in a block of the encoding that it contains
    bool ds = false;  // a DS wave: the loop starts at a word and runs to the end, and the end stays (edit/ds_waves.h)
    bool dse = false; // a wave of a DSE bank: a new rate also moves the tunings of its splits (edit/dse_banks.h)
};

// The samples that a loop's start steps by in `encoding` where the samples stay: a frame of DSP-ADPCM in a wave, and a
// block in a stream (ntdlib's StreamBlockSamples).
uint32_t LoopStep(ntdlib::sar::WaveEncoding encoding, bool stream);

// The samples by which the loop start of the item of `view` steps in `encoding`: the step that LoopStep returns, or a
// word of a DS wave (ntdlib::sdat::SamplesPerWord).
uint32_t LoopStepOf(const WaveView& view, ntdlib::sar::WaveEncoding encoding);

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

// The properties of wave `wave` of wave archive `wave_archive` of the DS's `archive`. The banks whose slots name the
// wave archive, and whose regions play the wave, make up the reach. Throws std::runtime_error with a message for the
// user for a wave that Hokuto can't read.
WaveView MakeDsWaveView(const song::DsArchive& archive, uint32_t wave_archive, uint32_t wave);

// The view of wave `wave` of bank `bank`, an index in `set`'s banks. Throws std::runtime_error with a message for the
// user for a wave that is missing from the bank or can't be read.
WaveView MakeDseWaveView(const song::DseSet& set, uint32_t bank, uint32_t wave);

// The same for stream `stream` of a DS archive: a loop starts at a block, and runs to the end. Throws
// std::runtime_error with a message for the user for a stream that can't be read.
WaveView MakeDsStreamView(const song::DsArchive& archive, uint32_t stream);

// A number with a comma between each group of three digits, such as 12,345.
std::string Grouped(uint64_t number);

// The line that says where a loop's start moves, in the words of the Replace window: the rule of a DS wave (`ds`), of a
// stream or of a wave in DSP-ADPCM, then "The loop start moves from sample `from` to `to`.". With `repeated`, the line
// also says that the samples in between repeat after the loop end, and that the loop sounds the same.
std::string LoopMoveLine(bool stream, bool ds, uint32_t from, uint32_t to, bool repeated);

// The message for the text of a program field, `typed`, that isn't a program from 0 to `highest`. Text that isn't a
// number gets a message that says so. Nothing for a program.
std::optional<std::string> ProgramRefusal(std::string_view typed, uint32_t highest);

// A pan from 0 to 127 as a stream track's line gives it: "centre", or the side and the steps from the centre, such as
// "12 left".
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
    format::Engine engine = format::Engine::kSwitch; // the engine of the archive
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
