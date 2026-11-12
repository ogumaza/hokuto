// SPDX-License-Identifier: MIT

// The parts that the views and the drawing of the Replace window share (replace_common.cpp). The names are
// internal to the Replace window.

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ntdlib/rsar/formats.h"
#include "ntdlib/sar/formats.h"

#include "app/replace_view.h"
#include "edit/files.h"
#include "format/file.h"

namespace hokuto::app::replace_common
{

// A sound's bank slots.
inline constexpr std::size_t kSlots = 4;

// The tracks of a mask, by their numbers, in a list: "track 3", "tracks 1 and 3".
std::string TrackList(uint32_t tracks);

// The bank slots of sound `sound` of `source`'s archive, as bank item IDs, or ntdlib::sar::kNoItem for an empty slot. A
// sound of the Wii has one slot.
std::vector<uint32_t> SoundBanks(const ReplaceSource& source, uint32_t sound);

// The names of the banks of `source`'s archive, in order. A bank without a name gets the name BANK_n. The browser names
// it the same way.
std::vector<std::string> BankNames(const ReplaceSource& source);

// The programs of bank `bank` of `source`'s archive. Throws as song::ProgramsOf does.
std::vector<uint32_t> ProgramsOfBank(const ReplaceSource& source, uint32_t bank);

// What a window of a MIDI file says of the file.
std::string MidiSource(const ReplaceSource& source);

// The rows of the drum notes of `source`, played by `bank`. `choices` lists the programs of `bank`.
void FillDrumRows(ReplaceView& view, const ReplaceSource& source, const std::string& bank,
                  const std::vector<uint32_t>& choices);

// The same with the bank in the slot of the drum channel, among the banks in the sound's slots or the slots chosen for
// an import.
void FillDrumRows(ReplaceView& view, const ReplaceSource& source);

// Whether the bank of a row's slot has the program chosen in the row.
bool Holds(const ProgramRow& row);

// The warnings about the programs: those from a variable or a random range, the slots without a bank, and the programs
// that are missing from a bank. A DS sequence plays one bank, and `slots` is false there.
void AddProgramWarnings(const std::vector<ProgramRow>& rows, std::size_t undecided, std::vector<std::string>& warnings,
                        bool slots = true);

// A list of numbers: "5", "5 and 6".
std::string NumberList(const std::vector<uint32_t>& numbers);

// The samples of a WAV file in words: "16-bit PCM" or "32-bit floating point".
std::string SamplesText(const edit::IncomingWave& incoming);

// A wave's encoding in words, such as "DSP-ADPCM" or "16-bit PCM".
const char* EncodingText(ntdlib::sar::WaveEncoding encoding);

// The line of the sizes of a wave of `present_size` bytes in `present`, and the new one of `added_size` bytes in
// `added`. A larger new wave adds `memory`, a sentence about the memory that the game sets aside, where there is one.
std::string WaveSizeLine(ntdlib::sar::WaveEncoding present, std::size_t present_size, ntdlib::sar::WaveEncoding added,
                         std::size_t added_size, const char* memory);

// The sentence of a wave's size line about the memory that a game sets aside for the waves of a sound, on the DS.
inline constexpr char kWaveMemory[] =
    "Games load the waves a sound requires into a fixed memory area. Larger waves may not fit.";

// A wave's channels in words: "mono", "stereo" or "3 channels".
std::string ChannelsText(std::size_t channels);

// A count of samples and its length at `rate`: "52,000 samples (1.08 s)".
std::string LengthText(std::size_t frames, uint32_t rate);

// Whether the archive of `source` holds names (edit::HoldsNames), and true without an archive. Where the archive holds
// none, the window refuses a new item that requires a name from the start, and offers no new group. The edit refuses
// such an item too.
bool HoldsNames(const ReplaceSource& source);

// The place of a new item with number `number` among the archive's `items`, such as "sounds", for a warning: after the
// others, or first where the archive holds none.
std::string Placed(std::size_t number, const std::string& items);

// The names of the items of the archive whose tables are `tables`, sorted, each once: its sounds, banks, wave archives,
// players and groups. With `archive`, the archive's sound groups count too, and so do the names that the browser shows
// for the archive's items. The browser names a bank or a wave archive without a name after its number, such as WARC_3.
std::vector<std::string> TakenNames(const format::NativeTables& tables, const ntdlib::sar::SoundArchive* archive);

// Whether `taken`, sorted names (TakenNames), holds `name`.
bool NameTaken(std::span<const std::string> taken, const std::string& name);

// Why an import can't make its new `items`, such as "bank" or "sounds", without a name.
std::string NoNameRefusal(std::string_view items);

// Why an import can't name a new item `name`: an item of the archive has the name.
std::string NameInUse(const std::string& name);

// The new items of an import of a file: "bank" for a bank, else "sound" or "sounds" for `sounds` new sounds.
std::string_view NewItemsWord(bool bank, std::size_t sounds);

// Why an import can't name its new `item` `name`: an empty name, or one that `taken` says is in use. Nothing for a name
// that the item can take. `taken` runs only for a name that isn't empty.
template <typename Taken>
std::optional<std::string> NameRefusal(const std::string& name, std::string_view item, const Taken& taken)
{
    if (name.empty())
    {
        return NoNameRefusal(item);
    }
    if (taken(name))
    {
        return NameInUse(name);
    }

    return std::nullopt;
}

// The pairings that `map` contains for the drum notes `notes`.
format::MidiDrumNotes PairedDrumNotes(const format::BankMidiMap& map, std::span<const int> notes);

// The program that `map` pairs with MIDI program `number`, when the pairing changes the program.
std::optional<int> PairedProgram(const format::BankMidiMap& map, int number);

// The warning that `players`, the names of what plays a wave, will play the new wave.
std::string WavePlayersText(const std::vector<std::string>& players);

// Adds the warning that a wave gains or loses its loop: the current wave loops when `present_loop`, and the new one
// when `new_loop`.
void AddWaveLoopWarning(bool present_loop, bool new_loop, std::vector<std::string>& warnings);

// The warning that the sample rate changes from `from` to `to`.
std::string RateChangeText(uint32_t from, uint32_t to);

// A count of waves in words: "1 wave", "12 waves".
std::string WavesText(std::size_t count);

// Gives `view` how the item changes the size of each group that it can join, and the size of the files that a new group
// takes. A group's size is the size of the files that it loads. A group's size before the edit is the Export window's
// measure (format::MeasureGrowth), from the archive that an export writes (FindGroups). `sizes` gives the size of each
// file that the edit adds or changes, by its ID. That archive gives the size of each other file. A file that a group
// holds and the edit changes changes the group's size too. The line of a group names the files that it takes besides
// the item's. A new file past the archive's files is part of the item, and the line doesn't name it.
void SizeGroups(ReplaceView& view, const ReplaceSource& source, const std::map<uint32_t, std::size_t>& sizes = {});

// Gives `view` the groups of `source`'s archive for an item that requires the files `after` in the place of an item
// that required `before`, or for a new item that copies a sound that requires `before` (edit::GroupOptions). A new
// item's file comes first in `after`, under an ID past the archive's files. A group that would take a file that Hokuto
// can't read is unavailable. The item joins the groups that the user chose, or else those that the window proposes, and
// the new groups that the user made. `note` says which groups the window proposes, if it proposes any, and `none` what
// follows when no group loads the item. `sizes` gives the sizes of the files that the edit adds or changes
// (SizeGroups). Without the archive's tables, the window offers no groups.
void FillGroups(ReplaceView& view, const ReplaceSource& source, std::span<const uint32_t> before,
                std::span<const uint32_t> after, std::string note, std::string none,
                const std::map<uint32_t, std::size_t>& sizes = {});

// The waves of each wave archive of `archive`, or nothing for a wave archive that Hokuto can't read. Puts the wave
// archives' names in `names` for a menu, each with its waves where they're known. A wave archive without a name gets
// the name WARC_n. The browser names it the same way.
std::vector<std::optional<std::size_t>> WaveArchiveMenu(const ntdlib::sar::SoundArchive& archive,
                                                        std::vector<std::string>& names);

// Adds to `warnings` what a WAV file holds and its wave leaves out or changes: its loops, its samples' format, the
// detail that DSP-ADPCM loses and the bytes that hold no whole frame.
void WavWarnings(const edit::IncomingWave& incoming, std::vector<std::string>& warnings, bool stream = false,
                 bool ds = false);

// The end of a source line for a wave or a stream that loops from sample `start` to sample `end`.
std::string LoopText(uint64_t start, uint64_t end);

// The line that names `file_name`, a WAV file, and says what its wave `incoming` contains.
std::string WavSourceLine(const std::string& file_name, const edit::IncomingWave& incoming);

// The line that names `file_name`, a wave file, and says what its wave `incoming` contains, for an archive of `flavor`.
// A wave file of the other console gives the console and the version of the file that was read.
std::string WaveSourceLine(const std::string& file_name, const edit::IncomingWave& incoming,
                           ntdlib::sar::Flavor flavor);

// The rates that a WAV file's wave can take: the WAV file's `wav_rate` first, then the present wave's and the usual
// rates below it, from the highest.
std::vector<uint32_t> RatesOf(uint32_t wav_rate, std::optional<uint32_t> present = std::nullopt);

// The version of a file of the Wii in words, such as "1.2".
std::string WiiVersionText(uint32_t version);

// The files that sound `sound` of the Wii's `archive` requires in a group, by their IDs: its file, and a sequence
// sound's bank's file (edit::AddWiiSequenceSound). None for a sound past the sound table.
std::vector<uint32_t> WiiFilesOfSound(const ntdlib::rsar::SoundArchive& archive, uint32_t sound);

// The names of the sounds, the banks and the players of `source.played.Wii()`, and of its groups with names, sorted,
// each once. No two of them can share a name (edit::AddWiiSequenceSound).
std::vector<std::string> WiiTakenNames(const ReplaceSource& source);

// Gives `view` the groups with names of `source.played.Wii()` (edit::WiiGroupsOf) for a new item that requires the
// files `after` and copies a sound that requires the files `before`. FillGroups does the same for the other consoles.
// The new item's file comes first in `after`, under an ID past the archive's files. A group that loads the sound takes
// only the files that the sound doesn't require. `sizes` gives the sizes of the new files (SizeGroups). The Wii's
// groups keep their numbers: the window makes no new group there.
void FillWiiGroups(ReplaceView& view, const ReplaceSource& source, std::span<const uint32_t> before,
                   std::span<const uint32_t> after, std::string note, std::string none,
                   const std::map<uint32_t, std::size_t>& sizes = {});

// What the window says when no group with a name loads a new item of the Wii: its file goes into the archive's last
// group (edit::AddWiiSequenceSound).
inline constexpr char kWiiLastGroup[] =
    "The new file goes into the unnamed last group. That group contains the files that aren't in any other group.";

// The same for several new files.
inline constexpr char kWiiLastGroupFiles[] =
    "The new files go into the unnamed last group. That group contains the files that aren't in any other group.";

// Adds to `view` where a new item's file goes when the archive holds no group with a name.
void AddWiiGroupWarning(ReplaceView& view);

// The names of the wave data of `source.played.Wii()` for the menus of the rows, each with its waves where Hokuto reads
// them: the RWAR beside the incoming file first if `beside` says so and `source.beside` holds one, then the wave data
// of the archive's files in the order of song::WaveArchivesOf. Gives the waves of each, or nothing for wave data that
// Hokuto can't read.
std::vector<std::optional<std::size_t>> WiiWaveDataMenu(const ReplaceSource& source, bool beside,
                                                        std::vector<std::string>& names);

// `view` with the reason why the Wii archive of `source` takes no new item (ReplaceSource::refused), and Import grey.
ReplaceView Refused(const ReplaceSource& source, ReplaceView view);

// A WAV file without a loop can take the present wave's loop if the loop lies inside the WAV file's samples. Otherwise
// `view` says why the new wave can't take it. `wav` tells whether `incoming` comes from a WAV file.
void OfferPresentLoop(ReplaceView& view, const edit::IncomingWave& incoming, bool wav,
                      const edit::WaveChanges& changes);

// The line of the sizes of the current file of the DS and the new one.
std::string DsSizeLine(std::size_t present, std::size_t added, const char* item);

// The line that names `file_name`, a WAV file or a wave file of the DS, and says what it contains.
std::string DsWaveSourceLine(const std::string& file_name, const edit::IncomingWave& incoming, bool wav);

// The rate that edit::ReadIncomingDsWave and edit::ReadIncomingDsStream give a WAV file whose rate is above the limit,
// when no rate is chosen.
inline constexpr uint32_t kDsFallbackRate = 48000;

// The rates that the wave or the stream of the DS from a WAV file can have: those of RatesOf, up to `highest`, the
// highest rate of a DS wave or of a stream. Above that limit, the readers' fallback rate comes first.
std::vector<uint32_t> DsRatesOf(const edit::IncomingWave& incoming, uint32_t present, uint32_t highest);

// Offers in `view` the encodings of the DS and the rates of DsRatesOf, with the rate of `incoming` chosen. For a WAV
// file whose rate passes `highest`, the fallback rate of the readers comes first.
void OfferDsRates(ReplaceView& view, const edit::IncomingWave& incoming, uint32_t present, uint32_t highest);

// Fills the new sound's name and the sounds of `archive` that the import may copy settings from.
template <typename Archive>
void FillImportModels(ReplaceView& view, const ReplaceSource& source, const Archive& archive, const std::string& name)
{
    name.copy(view.name.data(), view.name.size() - 1);
    for (const uint32_t model : source.models)
    {
        view.models.push_back(archive.Sounds()[model].name);
    }
    view.model = static_cast<std::size_t>(std::ranges::find(source.models, source.sound) - source.models.begin());
}

} // namespace hokuto::app::replace_common
