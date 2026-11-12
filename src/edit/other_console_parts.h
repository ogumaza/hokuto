// SPDX-License-Identifier: MIT

// The parts of the conversions of edit/other_console.h that their files share: the song that a conversion builds, the
// commands, the memory of the 3DS, the banks of one bank a sound, and the steps that the paths call. Only those files
// include it.

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
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

#include "ntdlib/commands/sequence.h"
#include "ntdlib/nwsnd/commands.h"
#include "ntdlib/nwsnd/util.h"
#include "ntdlib/rsar/bank_file.h"
#include "ntdlib/rsar/wave_file.h"
#include "ntdlib/sar/archive_model.h"
#include "ntdlib/sar/bank_file.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sar/wave_file.h"
#include "ntdlib/sound_build.h"

#include "edit/files.h"
#include "edit/other_console.h"
#include "edit/waves.h"
#include "edit/wii_programs.h"
#include "format/drivers.h"
#include "format/file.h"
#include "format/kinds.h"
#include "song/archive.h"

namespace hokuto::edit::other_console_parts
{

namespace sar = ntdlib::sar;

// `names` in a sentence, such as "A, B and C".
std::string ListOf(std::span<const std::string> names);

// Whether a conversion to the DS or the Wii leaves out a setting of `settings`: single play, option 17, the 3D
// settings, the value after them or a user parameter. With `remote_filter`, the remote filter counts too. The Wii keeps
// it, and the DS has no place for it.
bool SettingsLeftOut(const sar::SoundModel& settings, bool remote_filter);

// Whether `command` plays otherwise on console `to` than on console `from`: a command that the player of either side
// skips (song::Runs), or one that does nothing on `from` and that `to` reads as part of another command. The 3DS and
// the Wii read 0xF0 with 0xA0 to 0xBF without a byte, and the Wii U and the Switch read the settings of LFOs 1 to 3
// there. The Wii's builds before March 2008 read 0xA3 to 0xA5 as commands that do nothing. The later consoles read time
// prefixes there.
bool PlaysOtherwise(const ntdlib::commands::Command& command, const song::Console& from, const song::Console& to);

// The warning about `counts`, the commands of each kind that play otherwise on console `to` than on console `from`
// (PlaysOtherwise), or nothing. With `left_out`, the new file leaves them out. Otherwise they stay in their places.
std::optional<std::string> SkippedWarning(const std::map<std::string, std::size_t>& counts, const song::Console& from,
                                          const song::Console& to, bool left_out);

// The commands of `sequence` that play otherwise on console `to` than on console `from` (PlaysOtherwise), by what they
// set.
std::map<std::string, std::size_t> CountSkipped(const ntdlib::commands::Sequence& sequence, const song::Console& from,
                                                const song::Console& to);

// `sequence` without the items that `leaves` marks, with item `start` where the sound starts. The targets, the labels
// and the start move to the items that stay. Code that entered at an item that leaves enters at the next item that
// stays, or at the end.
std::pair<ntdlib::commands::Sequence, uint32_t> WithoutItems(const ntdlib::commands::Sequence& sequence, uint32_t start,
                                                             const std::vector<bool>& leaves);

// `sequence` without the commands that play otherwise on console `to` than on console `from` (PlaysOtherwise), with
// item `start` where the sound starts (WithoutItems).
std::pair<ntdlib::commands::Sequence, uint32_t> WithoutSkipped(const ntdlib::commands::Sequence& sequence,
                                                               uint32_t start, const song::Console& from,
                                                               const song::Console& to);

// The 3DS's player sets volume2 and the main volume at once, and their time only takes its bytes
// (ntdlib::nwsnd::CtrPolicy). The players of the Wii U and the Switch move such a value over the time. Takes the times
// of those settings out of `sequence`, a sequence of the 3DS, and gives how many went.
std::size_t WithoutCtrTimes(ntdlib::commands::Sequence& sequence);

// The warning about `times` settings of volume2 and the main volume of a sequence of the 3DS that go to the console of
// `to`: settings that lost their times (WithoutCtrTimes), or that keep them in a sequence whose commands keep their
// places.
std::string CtrTimesWarning(std::size_t times, sar::Flavor to, bool lost);

// Whether the console of `to` reads each command of `sequence` with the length that the console of `from` reads. The
// encoder refuses a sequence whose bytes keep their places when a command's length changes.
bool SameLengths(const ntdlib::commands::Sequence& sequence, sar::Flavor from, sar::Flavor to);

// A file that goes in: its name and its bytes for the other console. A wave archive also gives the latest version among
// the waves that it held, and its waves in 8-bit PCM by their indices.
struct Converted
{
    std::string name;
    IncomingFile incoming;
    std::optional<uint32_t> held_waves;
    std::vector<uint32_t> pcm8;
    std::optional<std::size_t> mono_bytes; // the file's bytes with its stereo waves as mono
};

// A note of a stereo wave: its key, its velocity and its track's pan at its start, from -1 to 1.
struct NotePan
{
    uint8_t key = 60;
    uint8_t velocity = 127;
    float pan = 0.0f;
};

// The level of a stereo wave as mono on the 3DS (MonoLevelOf), and the gain that its samples take. The regions that
// play it take the rest of their gains (GainAt).
struct MonoGains
{
    MonoLevel level;
    double samples = 1.0;
};

// The stereo waves of the wave archives that go in as mono (MonoWaveOf), and what they changed.
struct Mono
{
    bool on = false;
    uint32_t pan_mode = 0; // the sound's pan mode and pan curve
    ntdlib::nwsnd::PanInfo pan_info;

    // The notes of stereo waves, by the item ID of their bank in the source archive and their programs.
    std::map<std::pair<uint32_t, uint32_t>, std::vector<NotePan>> notes;

    // The stereo waves, by the wave archive's item ID in the source archive and their indices, with each one's gains
    // once a region or the wave archive uses them (GainsOf). The wave archives that hold them, as files of the 3DS with
    // their waves in stereo, by the same IDs.
    std::map<uint32_t, std::map<uint32_t, std::optional<MonoGains>>> waves;
    std::map<uint32_t, sar::WaveArchiveFile> sources;
    std::size_t count = 0;   // the waves that went in as mono
    std::size_t regions = 0; // the regions that play them
    double least_db = 0.0;   // the least and the most of the regions' gains, in dB
    double most_db = 0.0;
    std::optional<double> lowest_ratio; // the lowest ratio of signal to noise of those in DSP-ADPCM, in dB
    std::size_t held = 0;               // the regions whose volumes stay at 255, the highest
};

// The gain that gives the mean of a stereo wave of `level` the power of its two channels, over notes at `pans`: the
// square root of the ratio of the two powers, each summed over the notes (PowersAt). It stops at 8. Silence gives 1.
double GainAt(const Mono& mono, const MonoLevel& level, std::span<const float> pans);

// The song as it takes shape: the new file and the warnings, and the wave archives and banks that went in, by the
// source archive's item IDs. A wave archive maps to its place in the new archive's list, and a bank to its new item ID,
// or to kNoItem when it stays behind.
struct Song
{
    OtherConsoleSong result;
    std::map<uint32_t, uint32_t> wave_archives;
    std::map<uint32_t, uint32_t> banks;
    std::vector<std::string> older_waves;    // the wave archives whose waves lost the original starts of their loops
    std::size_t wave_bytes = 0;              // what the new wave archives take in the memory of the 3DS's model
    std::map<uint32_t, Converted> converted; // wave archives converted before they go in, by their item IDs
    Mono mono;
};

// Wave archive `id` of `archive` as a file of `to`. With `song.mono.on`, its stereo waves go in as mono, and `song`
// keeps them. Throws std::runtime_error and std::invalid_argument with a message for the user for a wave archive that
// the archive lacks, and for a file that can't be converted.
Converted ConvertWaveArchive(Song& song, const sar::SoundArchive& archive, uint32_t id, sar::Flavor to);

// `bytes`, wave archive `id` of the source archive as a file of the 3DS, with each stereo wave as mono (MonoWaveOf).
// The samples take their gain (GainsOf). `kept` gives the source's index of each wave of a wave archive that holds only
// some of them, or null for one that holds them all. `mono` counts the waves, and keeps the lowest ratio of signal to
// noise.
std::vector<uint8_t> MonoWaveArchive(std::span<const uint8_t> bytes, Mono& mono, uint32_t id,
                                     const std::vector<uint32_t>* kept);

// The bytes that a wave archive of `size` bytes takes in the memory of the 3DS's model. The model starts each wave
// archive at a multiple of 32 bytes.
std::size_t StoredSize(std::size_t size);

// `bytes` in words, such as "12.3 MB".
std::string SizeText(std::size_t bytes);

// The instruments and waves that go to the 3DS when the wave archives that a sound's banks name don't fit in the memory
// of the 3DS's model together: the instruments of `programs` in each bank, and the waves of those instruments in each
// wave archive, by their indices in order. `whole` gives the bytes that the wave archives would take whole.
struct Trim
{
    std::set<uint32_t> programs;
    std::map<uint32_t, std::vector<uint32_t>> waves; // by the wave archive's item ID in the source archive
    std::size_t whole = 0;
};

// The places in the table of waves of `bank` that the regions of its instruments of `programs` name.
std::set<uint32_t> TableEntriesOf(const sar::BankFile& bank, const std::set<uint32_t>& programs);

// Leaves in `bank` only the instruments of `programs`. Its table of waves keeps only the entries that those instruments
// play, in their order, each made from the old entry by `entry`. A region that plays an entry past the table plays the
// entry past the new table.
template <typename Entry>
void KeepOnlyInstruments(sar::BankFile& bank, const std::set<uint32_t>& programs, Entry entry)
{
    for (uint32_t program = 0; program < bank.instruments.size(); ++program)
    {
        if (!programs.contains(program))
        {
            bank.instruments[program].reset();
        }
    }
    while (!bank.instruments.empty() && !bank.instruments.back())
    {
        bank.instruments.pop_back();
    }

    // The table of waves, and the places of the old entries in it.
    std::map<uint32_t, uint32_t> places;
    std::vector<sar::WaveId> waves;
    for (const uint32_t kept : TableEntriesOf(bank, programs))
    {
        places[kept] = static_cast<uint32_t>(waves.size());
        waves.push_back(entry(bank.waves[kept]));
    }

    const auto past = static_cast<uint32_t>(waves.size());
    bank.waves = std::move(waves);
    for (std::optional<sar::BankFile::Instrument>& instrument : bank.instruments)
    {
        if (!instrument)
        {
            continue;
        }

        for (sar::BankFile::KeyRegion& key_region : instrument->key_regions)
        {
            for (sar::BankFile::Region& region : key_region.regions)
            {
                const auto place = places.find(region.values.wave_id_index);
                region.values.wave_id_index = place != places.end() ? place->second : past;
            }
        }
    }
}

// The trim of banks `banks` of `archive` for a sound that plays `sequence` from item `start` on the 3DS, or nothing
// when their wave archives fit in `memory` together as files of the 3DS. The instruments of program 0 and of the
// programs that the sound's program changes select stay, in every bank. The wave archives go to the 3DS first, and
// `song` keeps them. A bank or a wave archive that can't come along takes no memory: it stays behind later. Throws
// std::invalid_argument with a message for the user for wave archives that don't fit, when a variable or a random
// number gives a program, or ntdlib can't read the values of a bank.
std::optional<Trim> TrimFor(Song& song, const sar::SoundArchive& archive, std::span<const uint32_t> banks,
                            const ntdlib::commands::Sequence& sequence, uint32_t start, std::size_t memory);

// `bytes`, a wave archive file, with only its waves at `kept`, in their order.
std::vector<uint8_t> TrimmedWaveArchive(std::span<const uint8_t> bytes, std::span<const uint32_t> kept);

// `bytes`, a bank file, with only the instruments of the programs of `trim`. Its table of waves holds only the waves
// that those instruments play, in their order, at their places in the wave archives of `trim`. A wave that `trim`
// leaves out of its wave archive plays nothing. It played nothing before either.
std::vector<uint8_t> TrimmedBank(std::span<const uint8_t> bytes, const Trim& trim);

// `bytes`, bank `id` of the source archive as a file of the 3DS. Each region that plays a wave of `mono` takes the gain
// of the wave at the pans of the region's notes (GainAt): the pan of each note's track at the note's start, plus the
// region's pan. A region without notes takes the gain at its pan. The region's volume takes the part of the gain that
// the wave's samples don't take (GainsOf), up to 255. `mono` counts the regions, and keeps the range of their gains and
// the regions held at 255. With `trim`, only the instruments of its programs change: the others stay out.
std::vector<uint8_t> WithMonoVolumes(std::span<const uint8_t> bytes, Mono& mono, uint32_t id, const Trim* trim);

// The sequence that sound `sound` of `archive` plays and the item where it starts: the sequence of `file` that the
// sound's file holds, or the commands that Hokuto reads from the file's bytes. `sequences` gives the file's sequence of
// each of the archive's files (format::SequencesOfFiles). Throws std::invalid_argument with a message for the user for
// a sequence that can't be read, and for a start where no command starts.
std::pair<ntdlib::commands::Sequence, uint32_t> SequenceOf(const format::File& file, const sar::SoundArchive& archive,
                                                           std::span<const std::optional<uint32_t>> sequences,
                                                           uint32_t sound);

// A sequence sound that a conversion sends to another console: the sound's model in the tables, the archive that the
// file exports as bytes and with its group files, the file's sequence of each of the archive's files
// (format::SequencesOfFiles), the sound's sequence and the item where it starts (SequenceOf), and the sound's file.
struct SourceSequence
{
    const format::NativeSound& source;
    std::vector<uint8_t> bytes;
    std::shared_ptr<const sar::SoundArchive> archive;
    std::vector<std::optional<uint32_t>> sequences;
    ntdlib::commands::Sequence sequence;
    uint32_t start = 0;
    uint32_t file_id = 0;
};

// Sound `sound` of `file` as a SourceSequence. `file` has tables. Throws std::invalid_argument with a message for the
// user for a sound that isn't a sequence sound, and as SequenceOf does.
SourceSequence SourceSequenceSound(const format::File& file, uint32_t sound);

// The ends of the refusals of a bank too big for the other console: where Hokuto can't follow the programs of the
// sequence, and where a variable selects some of them (VariableSelectsPrograms).
inline constexpr char kProgramsNotFollowed[] =
    " Unused instruments can't be dropped because Hokuto can't follow the programs of the sequence.";
inline constexpr char kVariablePrograms[] =
    " A variable selects some programs of the sound. Their banks keep every instrument.";

// Whether a variable can give the program of a note of `programs`: a slot of WiiPrograms::played without a set.
bool VariableSelectsPrograms(const WiiPrograms& programs);

// The name of sequence sound `sound` of the source, `source`, for the new file: its name, or SEQ_ and its number.
std::string SequenceSoundName(const format::NativeSound& source, uint32_t sound);

// `db` with one decimal place, for a warning.
std::string DbText(double db);

// The warning about the settings of the new sound that only the model holds, when they stay behind.
inline constexpr std::string_view kSettingsLeft =
    "The sound's 3D settings, single play, user parameters and remote filter are left out. ";

// `song`'s file as a save and an open give it: the archive that its model lays out. The new sound, sound `added`, takes
// the settings of `source`, the model of the source sound, that the archive's tables hold beyond the Sound sections'
// values: its remote filter, its single play, its 3D settings, its options 17 and 28 to 31, and the value after its 3D
// settings. They stay behind with a warning when the new archive can't take them.
void Settle(Song& song, uint32_t added, const std::optional<sar::SoundModel>& source);

// The parts of `file` that go with sound `sound` into `song`'s file as sound `added`: the file's tags, the sound's
// tags, scenes and tracks' colours, the aux buses' volumes and the effects that the other console runs, the sound's
// mode of mixing, and the mode of its sequence, `sequence`, when it has one. The new file holds that sequence as
// `new_sequence`, when it holds it among its sequences.
void CopyParts(Song& song, const format::File& file, uint32_t sound, uint32_t added, std::optional<uint32_t> sequence,
               std::optional<uint32_t> new_sequence);

// The most notes of a sound that sound at once before their releases, and the most channels of their waves at once:
// with the waves as they are, and with the stereo waves as mono. `stereo` says whether a note plays a stereo wave.
// `stereo_notes` holds the notes of stereo waves, by the item IDs of their banks and their programs.
struct Held
{
    std::size_t notes = 0;
    std::size_t channels = 0;
    std::size_t mono_channels = 0;
    bool stereo = false;
    std::map<std::pair<uint32_t, uint32_t>, std::vector<NotePan>> stereo_notes;
};

// What ntdlib's performer reads of sound `sound` of `archive`. A note sounds from its start to its end. Each channel of
// its wave takes a voice of the console. A note whose wave the archive lacks counts for nothing. The 3DS plays each
// step of a track's pan from -64 to 63 as a 63rd of the way to a side (UpdateChannelParam).
Held HeldOf(const sar::SoundArchive& archive, uint32_t sound);

// Gives sound `added` of `song`'s file `values`, the values of a sound in the archive's tables that kCopiedValues
// names, through the Sound sections' edits. A value that the new archive can't take stays behind with a warning.
void CopyValues(Song& song, uint32_t added, const format::NativeSound& values);

// The commands of a sequence of the Wii in the forms in which the later consoles read the same commands. `build` is the
// build of NW4R SND that plays them. The Wii's end after a comparison ends the track whatever the comparison gave, and
// the end loses its comparison. A random prefix before a variable prefix gives nothing on the Wii: the variable gives
// the value. The later consoles would read a command 0xA1 after the random prefix, and the random prefix goes. NW4R SND
// sets volume2, the main volume and, before September 2009, the pitch bend at once. Their times go. A low-pass from a
// value or a random number takes the values whose cutoffs on the later consoles lie nearest (LaterLowPass).
ntdlib::commands::Sequence AsLaterConsoles(ntdlib::commands::Sequence sequence, const ntdlib::SndBuild& build);

// Which items of `sequence`, a sequence of the Wii, hold bytes that no command reaches: data at the start, or after a
// jump without a comparison, an end or such data. The Wii's ends end their tracks whatever a comparison gave. A track
// that reaches a return outside every call runs on past it, and data after a return can play.
std::vector<bool> DeadData(const ntdlib::commands::Sequence& sequence);

// Gives the sound that starts at item `start` of `sequence` commands at its start that set each global variable that
// the sound reads before it sets it, and whose starting value can change what the sound plays (song::VariablesOf), to
// `wanted` while it holds `held`. NW4R SND starts the global variables at 0, and the later consoles start them at -1.
// Each such variable takes a comparison with `held` and a setting after it. A comparison of the last variable with
// itself then gives the comparison's flag its starting value. Returns the item where the sound starts, and the
// variables' numbers.
std::pair<uint32_t, std::vector<int>> WithStartingGlobals(ntdlib::commands::Sequence& sequence, uint32_t start,
                                                          int64_t held, int64_t wanted);

// The warning about the global variables `globals`. Commands at the sound's start set their starting values to the
// value that the console of the sound's file gives them (WithStartingGlobals). `to_wii` says whether the sound goes to
// the Wii.
std::string GlobalsWarning(std::span<const int> globals, bool to_wii);

// The item ID that the table of waves of a bank of the Wii names its waves' wave archive by, before the bank goes in.
// AddBank then names the new wave archive instead.
inline constexpr uint32_t kWiiWaves = sar::ItemId(sar::kWaveArchiveItem, 0);

// The name of the wave archive that holds the waves of the Wii's bank `bank`: WARC_ in place of BANK_ at the start of
// the bank's name, or before the name.
std::string WaveArchiveNameOf(const std::string& bank);

// Sequence sound `sound` of `file`, a file of the Wii, as a new file of the Wii U. The sound's bank goes in as a bank
// of the Wii U, after a wave archive of its waves. The commands that the Wii's build of NW4R SND skips stay out, and so
// do the bytes that no command reaches (DeadData). The other commands go in as the Wii U reads them (AsLaterConsoles),
// with commands at the sound's start that give the global variables the Wii's starting value (WithStartingGlobals).
// NW4R SND plays a sound's volume above 127 at 127, and the sound takes 127. The build of October 2006 pans by the dual
// mode and the square-root curve alone, and the sound takes them. The remote filter and the user parameters stay
// behind. So do the Wii's effects. The warnings name no console but the Wii: the file can go on to another console.
// Throws std::invalid_argument with a message for the user for a sound that isn't a sequence sound, and for a sequence
// that Hokuto can't send yet.
OtherConsoleSong FromWii(const format::File& file, uint32_t sound);

// Sound `sound` of `file` as a new file of the Wii U. `file` is a file of the GBA, another of the GBA's drivers, the DS
// or DSE. With `to_ds`, the file continues to the DS (FromConversion). A sequence of the DS never does.
OtherConsoleSong FromConverter(const format::File& file, uint32_t sound, bool to_ds);

// A sequence of a later console as NW4R SND reads it (AsWii), the settings of volume2, the main volume and the pitch
// bend whose times went, and the other settings whose times went.
struct WiiCommands
{
    ntdlib::commands::Sequence sequence;
    std::size_t times = 0;
    std::size_t untimed = 0;
};

// The commands of a sequence of the console of `from` in the forms in which NW4R SND's `build` reads the same commands:
// AsLaterConsoles the other way. The Wii's end after a comparison ends the track whatever the comparison gave. Such an
// end becomes a jump after the comparison to an end after the sequence's items, and plays as before. NW4R SND sets
// volume2, the main volume and, before September 2009, the pitch bend at once. Their times go. The builds before March
// 2008 lack the time prefixes (RevolutionPolicy::HasCommand). Every setting's time goes there, and the setting takes
// its value at once. A low-pass from a value or a random number takes the values whose cutoffs on the Wii lie nearest
// (WiiLowPass). The later consoles keep a sweep's pitch in 64ths of a semitone. The builds before March 2008 keep it in
// 32nds. A sweep's pitch takes the build's nearest pitch: the pitch that the encoder writes. The text form then holds
// the pitch too.
WiiCommands AsWii(ntdlib::commands::Sequence sequence, sar::Flavor from, const ntdlib::SndBuild& build);

// The warnings about the settings of `commands` whose times went (AsWii), after `warnings`.
void AddTimeWarnings(const WiiCommands& commands, std::vector<std::string>& warnings);

// The version of the Wii's bank files that hold their waves in an RWAR, and of the Wii's sequence files.
inline constexpr uint16_t kWiiBankVersion = 0x0102;
inline constexpr uint16_t kWiiSequenceVersion = 0x0100;

// The version of the Wii's wave sound files that hold their waves in an RWAR.
inline constexpr uint16_t kWiiWaveSoundVersion = 0x0103;

// The instruments of `bank` as the later consoles' reader plays them (sar::Instrument::Find): a key region for each run
// of keys whose velocities find the same regions, and a velocity region for each run of velocities that finds one
// region. A program that plays no region at any key and velocity has no instrument.
sar::BankFile BankFileOfRegions(const sar::Bank& bank);

// A bank of the Wii and its name.
struct WiiBank
{
    std::string name;
    ntdlib::rsar::BankFile::Files files;
};

// The waves of each wave archive of an archive of the 3DS, the Wii U or the Switch, by the wave archive's item, read
// once. A wave archive that the archive lacks holds none.
using WaveArchiveWaves = std::map<uint32_t, std::vector<std::vector<uint8_t>>>;

const std::vector<std::vector<uint8_t>>& WavesOf(const sar::SoundArchive& archive, uint32_t item,
                                                 WaveArchiveWaves& read);

// The name of bank `id` of `archive`, or BANK_ and its number for a bank without a name.
std::string BankNameOf(const sar::SoundArchive& archive, uint32_t id);

// The bytes of bank `id`'s file in `archive`. Throws std::invalid_argument for a bank that the archive lacks.
std::span<const uint8_t> BankBytesOf(const sar::SoundArchive& archive, uint32_t id);

// The programs of bank `id` of `archive`: the entries of its table of instruments. 0 for a bank that can't come along.
uint32_t BankProgramsOf(const sar::SoundArchive& archive, uint32_t id);

// The programs of the slots that sound `source` of `archive` plays from item `start` of `sequence`, with the programs
// of the banks in its slots (WiiProgramsOf).
std::optional<WiiPrograms> SoundProgramsOf(const sar::SoundArchive& archive, const format::NativeSound& source,
                                           const ntdlib::commands::Sequence& sequence, uint32_t start);

// Sets OtherConsoleSong::slot_programs of `song` to the first program of each slot of `programs`.
void SetSlotPrograms(Song& song, const WiiPrograms& programs);

// `bank` with only the instruments of `programs`. Its table of waves holds only the waves that those instruments play,
// in their order. A region that plays a wave past the table still does.
sar::BankFile WithOnlyInstruments(sar::BankFile bank, const std::set<uint32_t>& programs);

// A later console's bank with its waves as waves of another console (WavedBankOf). `left` counts the waves that can't
// go there.
template <typename Wave>
struct WavedBank
{
    sar::BankFile file;
    std::vector<std::optional<Wave>> waves;
    std::size_t left = 0;
};

// Bank `id` of `archive` as the later consoles' reader plays it (BankFileOfRegions), with each entry of its table of
// waves as `convert` returns it. An entry without a wave stands for a wave that is missing from the archive, or a wave
// that can't be read or converted. With `kept`, the bank holds only the instruments of its programs, and the waves that
// they play. Throws for a bank that can't come along.
template <typename Wave, typename Convert>
WavedBank<Wave> WavedBankOf(const sar::SoundArchive& archive, uint32_t id, WaveArchiveWaves& read,
                            const std::optional<std::set<uint32_t>>& kept, const Convert& convert)
{
    sar::BankFile file = BankFileOfRegions(sar::Bank::Parse(BankBytesOf(archive, id), archive.ReadOptions()));
    if (kept)
    {
        file = WithOnlyInstruments(std::move(file), *kept);
    }

    WavedBank<Wave> bank{.file = std::move(file), .waves = {}};
    for (const sar::WaveId& wave : bank.file.waves)
    {
        const std::vector<std::vector<uint8_t>>& held = WavesOf(archive, wave.wave_archive_item, read);
        if (wave.index >= held.size())
        {
            bank.waves.emplace_back();
            continue;
        }

        try
        {
            bank.waves.push_back(convert(sar::WaveFile::Read(held[wave.index])));
        }
        catch (const std::exception&)
        {
            bank.waves.emplace_back();
            ++bank.left;
        }
    }

    return bank;
}

// The warning about `left` waves of bank `name` that the Wii doesn't play.
std::string LeftWavesWarning(std::size_t left, const std::string& name);

// Bank `id` of `archive` as a bank of the Wii with an RWAR of its waves (ntdlib::rsar::WiiBankOf and WiiWaveOf). The
// bank plays the region that the later consoles' reader finds at each key and velocity (BankFileOfRegions). A wave that
// the archive lacks plays nothing on either console. A wave that the Wii can't play, such as one in IMA-ADPCM, stays
// behind with a warning, and its regions play nothing. A bank that can't come along stays behind with a warning, and
// the sound plays an empty bank in its place. With `kept`, the bank holds only the instruments of its programs.
WiiBank WiiBankFrom(Song& song, const sar::SoundArchive& archive, uint32_t id, WaveArchiveWaves& read,
                    const std::optional<std::set<uint32_t>>& kept);

// The instruments that slot `slot` keeps where a sound's bank takes only the instruments that the sound selects: the
// programs that the slot's notes play, and those of the notes whose slot a variable or a random number selects
// (WiiPrograms::played). Nothing where a variable can give the program of such a note: the slot keeps every instrument.
std::optional<std::set<uint32_t>> KeptOf(const WiiPrograms& programs, int slot);

// The banks of a sound's slots as one bank, with its name.
template <typename Wave>
struct MergedBank
{
    std::string name;
    sar::BankFile file;
    std::vector<std::optional<Wave>> waves;
};

// The warning of MergedBankOf about what became of the slots: `one_bank`, then the banks that `ranges` place in the
// bank `bank_name` at their programs, and the bank selects and program changes that `programs` changed.
std::string MergedBankWarning(const std::string& one_bank, const std::string& bank_name,
                              std::span<const std::pair<std::string, std::string>> ranges, const WiiPrograms& programs);

// Adds `count` instruments of `bank` to `merged`, the instruments of MergedBankOf, with the indexes of their waves in
// `merged`'s table. A wave that `indexes` contains is added once. `waves` gains each new wave. Without `bank`, the
// instruments stay empty.
template <typename Wave>
void AddSlotInstruments(const std::optional<WavedBank<Wave>>& bank, uint32_t count, sar::BankFile& merged,
                        std::vector<std::optional<Wave>>& waves,
                        std::map<std::pair<uint32_t, uint32_t>, uint32_t>& indexes)
{
    for (uint32_t program = 0; program < count; ++program)
    {
        std::optional<sar::BankFile::Instrument>& instrument = merged.instruments.emplace_back();
        if (!bank || program >= bank->file.instruments.size() || !bank->file.instruments[program])
        {
            continue;
        }

        instrument = *bank->file.instruments[program];
        for (sar::BankFile::KeyRegion& key : instrument->key_regions)
        {
            for (sar::BankFile::Region& region : key.regions)
            {
                const uint32_t index = region.values.wave_id_index;
                const sar::WaveId& wave = bank->file.waves[index];
                const auto [found, added] =
                    indexes.try_emplace({wave.wave_archive_item, wave.index}, merged.waves.size());
                if (added)
                {
                    merged.waves.push_back(wave);
                    waves.push_back(bank->waves[index]);
                }

                region.values.wave_id_index = found->second;
            }
        }
    }
}

// The banks of the slots that `programs` gives ranges as one bank (WiiProgramsOf): each slot's instruments from the
// first program of its range. `convert` returns each wave (WavedBankOf). `banks` gives the bank in each slot of the
// sound. A slot without a bank gives its programs no instruments, and so does a bank that can't come along, with a
// warning. A wave that two banks share goes in once. The bank takes the names of the slots' banks. With `trim`, each
// slot keeps only the instruments that the sound selects (KeptOf). `one_bank` starts the warning that says what became
// of the slots, and `left_warning` is the warning about a bank's waves that can't go in.
template <typename Wave, typename Convert>
MergedBank<Wave> MergedBankOf(Song& song, const sar::SoundArchive& archive, std::span<const uint32_t> banks,
                              const WiiPrograms& programs, WaveArchiveWaves& read, bool trim, const Convert& convert,
                              const std::string& one_bank, std::string (*left_warning)(std::size_t, const std::string&))
{
    const std::map<int, SlotPrograms>& slots = programs.slots;
    sar::BankFile merged;
    std::vector<std::optional<Wave>> waves;
    std::map<std::pair<uint32_t, uint32_t>, uint32_t> indexes;
    std::vector<std::string> names;
    std::vector<std::pair<std::string, std::string>> ranges;
    for (const auto& [slot, range] : slots)
    {
        const uint32_t id =
            static_cast<std::size_t>(slot) < banks.size() ? banks[static_cast<std::size_t>(slot)] : sar::kNoItem;
        std::optional<WavedBank<Wave>> bank;
        if (id != sar::kNoItem)
        {
            const std::string name = BankNameOf(archive, id);
            try
            {
                bank = WavedBankOf<Wave>(archive, id, read, trim ? KeptOf(programs, slot) : std::nullopt, convert);
                const auto past = [&bank](const sar::BankFile::Region& region)
                {
                    return region.values.wave_id_index >= bank->waves.size();
                };
                const auto key_past = [&past](const sar::BankFile::KeyRegion& key)
                {
                    return std::ranges::any_of(key.regions, past);
                };
                const auto past_waves = [&key_past](const auto& instrument)
                {
                    return instrument && std::ranges::any_of(instrument->key_regions, key_past);
                };
                if (std::ranges::any_of(bank->file.instruments, past_waves))
                {
                    throw std::invalid_argument("Its regions refer to waves past the end of its wave table.");
                }

                names.push_back(name);
                if (range.count == 1)
                {
                    ranges.emplace_back(name, "at program " + std::to_string(range.first));
                }
                else if (range.count > 1)
                {
                    ranges.emplace_back(name, "at programs " + std::to_string(range.first) + " to " +
                                                  std::to_string(range.first + range.count - 1));
                }

                if (bank->left > 0)
                {
                    song.result.warnings.push_back(left_warning(bank->left, name));
                }
            }
            catch (const std::exception& error)
            {
                song.result.warnings.push_back("Bank " + name + " is left out. " + format::AsSentence(error.what()) +
                                               " Its programs are silent.");
                bank.reset();
            }
        }

        // The slot's instruments, with the indexes of their waves in the new bank's table.
        AddSlotInstruments(bank, range.count, merged, waves, indexes);
    }

    MergedBank<Wave> bank{.name = names.empty() ? "BANK_0" : names.front(), .file = {}, .waves = {}};
    for (std::size_t n = 1; n < names.size(); ++n)
    {
        bank.name += "_" + names[n];
    }

    song.result.warnings.push_back(MergedBankWarning(one_bank, bank.name, ranges, programs));
    bank.file = std::move(merged);
    bank.waves = std::move(waves);

    return bank;
}

// The banks of the slots that `programs` gives ranges as one bank of the Wii. Each bank goes in as WiiBankFrom sends
// it.
WiiBank MergedWiiBank(Song& song, const sar::SoundArchive& archive, std::span<const uint32_t> banks,
                      const WiiPrograms& programs, WaveArchiveWaves& read, bool trim);

// The warnings of a console whose sounds play one bank: the banks of a sound's other slots stay behind there, and notes
// may play another instrument. `console` names that console.
void AddOneBankWarnings(Song& song, const sar::SoundArchive& archive, std::span<const uint32_t> banks,
                        const std::optional<WiiPrograms>& programs, bool merged, sar::Flavor from,
                        const std::string& console);

// Adds to `song` the warning about the other sounds, `sharing` of them, whose commands the sequence file contains. They
// are copied but unused.
void AddSharingWarning(Song& song, std::size_t sharing);

// Whether `file` contains a song of the GBA's drivers, a sequence of the DS or a sound of DSE. Such a sound converts to
// the Wii U first, through a converter (FromConverter), and on from there.
bool GoesThroughConverter(const format::File& file);

// Throws std::invalid_argument with a message for the user for a file without the tables of its archive.
void RequireTables(const format::File& file);

// `next`, the step of a send after `first`, with the warnings of `first` before the warnings of `next`.
OtherConsoleSong AfterStep(const OtherConsoleSong& first, OtherConsoleSong next);

// `sequence` encoded with `grammar`, and the offset of each of its labels. Throws std::invalid_argument with a message
// for the user when the encoder can't write the commands.
std::pair<std::vector<uint8_t>, std::vector<ntdlib::nwsnd::LabelOffset>> EncodedFile(
    const ntdlib::commands::Sequence& sequence, const ntdlib::nwsnd::Grammar& grammar, const std::string& console);

} // namespace hokuto::edit::other_console_parts
