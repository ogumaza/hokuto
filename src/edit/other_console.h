// SPDX-License-Identifier: MIT

// A sequence sound of a file as a new file of another console among the 3DS, the Wii U and the Switch. The consoles run
// the same commands, but the 3DS's player skips some (song::Runs). Their banks, wave archives and waves hold the same
// structures in other byte orders. The new file keeps its archive in the model form (format::NewFile). It holds the
// sound, a sequence file with the commands of the sound's sequence file, the banks in the sound's slots and the wave
// archives that their waves name. Each file goes in as a file of the other console, with the versions that Export…
// writes for that console (song::ExportVersions). File > Import > Sequence…, Bank… and Wave Archive… add them
// (edit/new_items.h). A command that the 3DS's player skips stays out of the sequence. So does a command of the 3DS
// that does nothing there and that the other console reads as a setting. A sequence whose commands have to keep their
// places, such as one with bytes that no command reaches, goes in as the other console's decoder reads the converted
// file (song::ConvertSequenceFile). The 3DS's player sets volume2 and the main volume at once, and the Wii U and the
// Switch move them over a time. From the 3DS, those settings lose their times, unless the commands keep their places.
// The Switch's renderer plays no wave in 8-bit PCM. Such a wave goes to the Switch in 16-bit PCM, with the same
// samples, through the properties' edit of a wave's encoding (edit/waves.h).
//
// Hokuto's model of the 3DS keeps each wave archive that a sound plays whole in its memory. A game also loads a whole
// wave archive into its sound heap. When the wave archives that the banks name don't fit there together, the banks go
// to the 3DS with only the instruments of program 0 and of the programs that the sound's program changes select. Each
// wave archive then holds only the waves of those instruments.
//
// The 3DS plays 23 notes and 24 channels of waves at once, and drops the notes of the lowest priority for the others. A
// stereo wave takes two channels. The stereo waves of the wave archives can go to the 3DS as mono, each the mean of its
// two channels, in its encoding. The mean takes a gain that keeps the power of the two channels at the pans of the
// wave's notes, by the sound's pan mode and pan curve. The samples take as much of the gain as their peak allows. The
// regions that play the wave take the rest, up to a volume of 255.
//
// The sound then takes the source sound's values in the archive's tables through the Sound sections' edits
// (edit/tables.h): its volume, its pan mode and pan curve, its priorities, its actor player, the tracks that it
// allocates and its release priority fix. It plays on the new file's player. Its other settings in the archive's tables
// follow through the model: its 3D settings, its single play, its user parameters and its remote filter. The new file
// takes the source file's tags, the sound's tags, scenes and tracks' colours, the aux buses' volumes and the sound's
// mode of mixing. A console that refuses the mode takes strict mixing (format::MixingRefused). A timeline stays a
// timeline. The effects that the other console runs stay on their buses, and the others stay behind. The lengths stay
// behind too: the other console's model can play a sound for another length. The new file comes back from both forms as
// it is.
//
// A sound of the Wii goes to the Wii U first, and on from there to the 3DS or the Switch. Its bank goes in as a bank of
// the Wii U, after a wave archive of its waves (ntdlib::rsar::BankFileOf and WaveFileOf). The commands that the Wii's
// build of NW4R SND skips stay out, and so do the bytes that no command reaches. The other commands go in as the Wii U
// reads them. An end after a comparison ends the track whatever the comparison gave. NW4R SND sets volume2, the main
// volume and, before September 2009, the pitch bend at once, and their times go. A low-pass takes the value whose
// cutoff on the later consoles lies nearest. NW4R SND starts the global variables at 0, and the later consoles start
// them at -1. Commands at the sound's start give the global variables that the sound reads before it sets them the
// Wii's starting value. The sound's volume goes in at 127 at most. NW4R SND plays a higher volume at 127. A sound of
// the build of October 2006 takes the dual pan mode and the square-root pan curve. The remote filter, the user
// parameters and the Wii's effects stay behind.

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/rsar/bank_file.h"
#include "ntdlib/rsar/wave_file.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sound_build.h"

#include "format/file.h"

namespace hokuto::edit
{

// The memory in which Hokuto's model of the 3DS keeps wave archives: 64 MB (ntdlib's kArchiveModeFcram).
inline constexpr std::size_t kCtrWaveMemory = 64 * 1024 * 1024;

// The memory in which Hokuto's model of the Wii keeps a bank's waves: the 64 MB of the Wii's MEM2.
inline constexpr std::size_t kWiiWaveMemory = 64 * 1024 * 1024;

// A file of the other console with one sound, and what changed on the way, in words for the user. For the 3DS,
// `stereo_waves` says whether the sound's notes play stereo waves, and `mono_waves` whether they went in as mono. For
// the Wii, `slot_programs` gives the first program of each bank slot in the sound's one bank when the banks of several
// slots went in as one (WiiProgramsOf in edit/wii_programs.h). A note of slot s and program p then plays program
// slot_programs[s] + p there.
struct OtherConsoleSong
{
    format::File file;
    std::vector<std::string> warnings;
    bool stereo_waves = false;
    bool mono_waves = false;
    std::map<int, uint32_t> slot_programs;
};

// How the stereo waves of a sound go to the 3DS. The 3DS plays 24 channels of waves at once, and a stereo wave takes
// two. A stereo wave as mono holds the mean of its two channels (MonoWaveOf in edit/waves.h).
enum class MonoWaves
{
    kAuto, // as mono when the sound holds more channels at once than the 3DS plays, before the notes' releases
    kOn,
    kOff,
};

// Sequence sound `sound` of `file` as a new file of the console of `to`. `file` is a file of the 3DS, the Wii U, the
// Switch or the Wii. It holds its sequences as a saved file holds them. A bank that can't come along, such as one whose
// file the archive lacks, stays behind with a warning, and its slot stays empty. So does a bank whose wave archives
// can't come along. The warnings also name the commands that stay out, the files that go in with an older version than
// they held, the effects that stay behind, the other sounds whose commands the sequence file holds, and settings that
// Hokuto can't read. The biquad filters take the types and values that come nearest on the other console
// (song::MapBiquads). `mono` sets how the stereo waves go to the 3DS. `ctr_wave_memory` gives the memory of the 3DS's
// model for wave archives. Throws std::invalid_argument with a message for the user for a file without the archive's
// tables, a target of the file's console, a sound that isn't a sequence sound, and a sequence that Hokuto can't read or
// write for the other console. It also throws one for a sound for the 3DS whose wave archives don't fit in that memory,
// with its banks whole or with fewer instruments. A bank whose values ntdlib can't read keeps every instrument, and so
// do the banks of a sound whose programs come from a variable or a random number. Throws the exceptions of the imports
// too.
OtherConsoleSong ForOtherConsole(const format::File& file, uint32_t sound, ntdlib::sar::Flavor to,
                                 MonoWaves mono = MonoWaves::kAuto, std::size_t ctr_wave_memory = kCtrWaveMemory);

// Sequence sound `sound` of `file`, a file of the 3DS, the Wii U or the Switch, as a new file of the Wii.
// ForOtherConsole sends a sound to another console the same way. The new file keeps its archive in the model form, of
// RSAR 1.4, and NW4R SND's build of June 2008 plays it (format::NewFile). A sound of the Wii plays one bank, and NW4R
// SND has no bank select. The banks of the slots that the sound's notes play go in as one bank of the Wii with their
// waves, one slot after another (WiiProgramsOf in edit/wii_programs.h, ntdlib::rsar::WiiBankOf and WiiWaveOf). The
// program changes then select their slots' programs there, and the bank selects become program changes. Where the notes
// play only the first slot, its bank goes in alone. The banks of the other slots stay behind with a warning. A wave
// that the Wii doesn't play, such as one in IMA-ADPCM, stays behind, and its regions play nothing. A bank that can't
// come along stays behind, and its programs play nothing. The commands that the Wii's build skips stay out, such as the
// bank selects that stay and LFOs 1 to 3. The other commands go in as NW4R SND reads them. An end after a comparison
// becomes a jump to an end after the comparison: the Wii's end ends the track whatever the comparison gave. NW4R SND
// sets volume2, the main volume and the pitch bend at once, and their times go. A low-pass takes the value whose cutoff
// on the Wii lies nearest, and so do the biquad filters of the Switch (song::MapBiquads). Commands at the sound's start
// give the global variables that the sound reads before it sets them the later consoles' starting value of -1. The
// sound takes the source sound's values through the Sound sections' edits, and its remote filter. Its volume goes in at
// 127 at most. NW4R SND plays a higher volume at 127. Its actor player, 3D settings, single play and user parameters
// stay behind. The other parts follow as ForOtherConsole gives them. Of the effects, only the AXFX effects go along.
// With `wii_build`, the new file holds that build of NW4R SND, and the commands follow its rules: a build before March
// 2008 takes every setting at once. Throws std::invalid_argument with a message for the user for a file without the
// archive's tables, a file of the Wii, a sound that isn't a sequence sound, and a sequence that Hokuto can't read or
// write for the Wii, such as one whose commands have to keep their places. `wii_wave_memory` gives the memory of the
// Wii's model for the bank's waves. When the waves don't fit, the bank takes only the instruments of the programs that
// the sound's notes select. A slot whose notes can take a program from a variable keeps every instrument. ForWii throws
// std::invalid_argument with a message for the user when the waves still don't fit.
OtherConsoleSong ForWii(const format::File& file, uint32_t sound,
                        const std::optional<ntdlib::SndBuild>& wii_build = std::nullopt,
                        std::size_t wii_wave_memory = kWiiWaveMemory);

// Why Hokuto can't play the sequence sounds of `file` on `engine`, or nothing when it can. A sound plays on the engine
// of the file's console as it is. On the engine of another console, it plays as that console plays its conversion
// (ForEngine).
std::optional<std::string> EngineRefused(const format::File& file, format::Engine engine);

// Sequence sound `sound` of `file` converted for the console of `engine`. ForOtherConsole or ForWii converts it from
// the file as a saved file holds it. `file` holds its timelines as the program edits them. Throws std::invalid_argument
// for an engine that EngineRefused refuses or that the file plays on already, and the exceptions of ForOtherConsole and
// ForWii.
OtherConsoleSong ForEngine(const format::File& file, uint32_t sound, format::Engine engine);

// A later console's commands for the Wii (WiiCommandsOf), and what changed on the way in words for the user.
struct CommandsForWii
{
    ntdlib::commands::Sequence sequence;
    std::vector<std::string> warnings;
};

// The commands that NW4R SND's `build` reads for those of `sequence`, and what changed on the way in words for the
// user. `sequence` comes from a sequence file of the console of `from`: a file of a later console that goes into an
// archive of the Wii. The commands that the build skips stay out, and the others take the Wii's forms. ForWii gives
// them the same forms. The global variables keep their starting values: NW4R SND starts them at 0, and the later
// consoles at -1. The game's code can set them. Throws std::invalid_argument with a message for the user for commands
// that have to keep their places: the encoder can't move them.
CommandsForWii WiiCommandsOf(const ntdlib::commands::Sequence& sequence, ntdlib::sar::Flavor from,
                             const ntdlib::SndBuild& build);

// Gives the sound that starts at item `start` of `sequence`, commands of a later console for the Wii (WiiCommandsOf),
// commands at its start that set each global variable that it reads before it sets it to -1 while it holds 0. ForWii
// does the same. NW4R SND starts the global variables at 0, and the later consoles start them at -1. Returns the item
// where the sound then starts, and the variables' numbers.
std::pair<uint32_t, std::vector<int>> WithLaterGlobals(ntdlib::commands::Sequence& sequence, uint32_t start);

// The warning about the global variables `globals` that commands at a sound's start set (WithLaterGlobals).
std::string LaterGlobalsWarning(std::span<const int> globals);

// `bytes`, a bank file of the 3DS, the Wii U or the Switch, as a bank of the Wii of `version` with `waves`
// (ntdlib::rsar::WiiBankOf). ForWii sends a bank the same way: each program plays the region that the later consoles'
// reader finds at each key and velocity. Entry i of `waves` gives the wave of entry i of the bank's table of waves. An
// entry without a wave leaves its regions without a wave: NW4R SND plays nothing there. Throws std::invalid_argument
// with a message for the user for bytes that ntdlib can't read as a bank of `flavor`, and the exceptions of WiiBankOf.
ntdlib::rsar::BankFile WiiBankOfFile(std::span<const uint8_t> bytes, ntdlib::sar::Flavor flavor, uint16_t version,
                                     const std::vector<std::optional<ntdlib::rsar::WaveFile>>& waves);

// The files that Export… writes for an item that goes to another console: the item's file, the file of its waves that
// goes beside it where the item keeps its waves apart, and what changed on the way in words for the user.
struct ConvertedItem
{
    std::vector<uint8_t> file;
    std::vector<uint8_t> waves;
    std::vector<std::string> warnings;
};

// The sequence file of sequence sound `sound` of `archive`, an archive of the 3DS, the Wii U or the Switch, as a
// sequence file of the Wii (RSEQ) for the build of NW4R SND that a new file of the Wii takes (WiiCommandsOf). The
// commands are read from the file's labels and from the starts of the sounds that play the file. The labels keep their
// names. The global variables keep their starting values. Throws std::invalid_argument with a message for the user for
// a file that Hokuto can't read or write for the Wii, such as one whose commands have to keep their places.
ConvertedItem WiiSequenceFileOf(const ntdlib::sar::SoundArchive& archive, uint32_t sound);

// Bank `index` of `archive`, an archive of the 3DS, the Wii U or the Switch, as a bank file of the Wii with an RWAR of
// its waves. This is how ForWii sends a bank: each program plays the region that the later consoles' reader finds at
// each key and velocity. A wave that the archive lacks plays nothing. So does a wave that the Wii doesn't play, such as
// one in IMA-ADPCM, and a warning counts those. Throws std::invalid_argument with a message for the user for a bank
// that ntdlib can't read or convert.
ConvertedItem WiiBankFilesOf(const ntdlib::sar::SoundArchive& archive, uint32_t index);

// The wave sound file of wave sound `sound` of `archive`, an archive of the 3DS, the Wii U or the Switch, as a wave
// sound file of the Wii of version 1.3 with an RWAR of its waves (ntdlib::rsar::WiiWsdOf). The waves come along as for
// WiiBankFilesOf. Throws std::invalid_argument with a message for the user for a file that ntdlib can't read or
// convert.
ConvertedItem WiiWaveSoundFilesOf(const ntdlib::sar::SoundArchive& archive, uint32_t sound);

// The sequence file of sequence sound `sound` of the Wii's `archive` as a sequence file of the console of `to`. NW4R
// SND's `build` reads the file's commands. FromWii makes the same conversion of a sound's commands. The commands that
// the build skips and the bytes that no command reaches stay out. The others go in as the later consoles read them. The
// commands are read from the file's labels and from the starts of the sounds that play the file. The labels keep their
// names. The global variables keep their starting values. Throws std::invalid_argument with a message for the user for
// a file that Hokuto can't read or write there, such as one whose commands have to keep their places.
ConvertedItem LaterSequenceFileOf(const ntdlib::rsar::SoundArchive& archive, uint32_t sound,
                                  const ntdlib::SndBuild& build, ntdlib::sar::Flavor to);

// Bank `index` of the Wii's `archive` as a bank file of the console of `to`, with a wave archive file of its waves
// (ntdlib::rsar::BankFileOf and WaveFileOf). FromWii sends a bank the same way: the regions play with the values that
// NW4R SND's `build` reads. The bank names its waves in the archive's first wave archive. File > Import > Bank… pairs
// that wave archive with one of the archive's. Throws std::invalid_argument for a bank that the archive lacks or whose
// file or waves it lacks, and the exceptions of ntdlib's reader and converters.
ConvertedItem LaterBankFilesOf(const ntdlib::rsar::SoundArchive& archive, uint32_t index, const ntdlib::SndBuild& build,
                               ntdlib::sar::Flavor to);

} // namespace hokuto::edit
