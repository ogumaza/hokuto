// SPDX-License-Identifier: MIT

// The Replace window of the browser's Replace…: a sequence file in the place of a sound's sequence (edit/replace.h), a
// bank, a wave archive or a wave sound file in the place of another, a WAV file in the place of a wave (edit/files.h),
// or a stream file or a WAV file in the place of a stream sound's stream (edit/streams.h). For a sequence, the window
// shows the file and offers its starts and the banks of the sound's slots. It pairs each program that the new commands
// select with a program of the bank in the same slot. Each variable that the new commands read before they set it keeps
// its number, moves to another or takes a fixed value (edit/variables.h). For a bank or a wave sound file, it pairs
// each wave archive that the file's waves name with a wave archive of the archive. It lists the warnings before
// anything changes. The same window serves File > Import > Sequence…, Bank…, Wave Archive…, Wave Sound File… and
// Stream… (edit/new_items.h). It then names the new item. For a sequence, a wave sound file or a stream, it offers the
// sounds whose settings the new sounds can copy.

#pragma once

#include <imgui.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "ntdlib/rsar/formats.h"
#include "ntdlib/sar/formats.h"

#include "edit/files.h"
#include "edit/groups.h"
#include "edit/new_items.h"
#include "edit/replace.h"
#include "edit/streams.h"
#include "edit/variables.h"
#include "format/file.h"
#include "song/sequence.h"

namespace hokuto::app
{

// What a replacement starts from: the version of the file, an archive that the version or an earlier one builds, where
// each sound plays (edit::SoundPlaces), the sound, and the sequence file to put in, with the file's name. An import of
// a sequence or a wave sound file lists the sounds whose settings the new sounds can copy in `models`. `sound` is then
// the one that they copy. `sound_starts` gives the sounds whose sequence holds the sequence file's commands, with their
// starts (FindSoundStarts). `readers` gives the sounds whose commands read each variable that a game can set
// (FindReaders). `banks` holds the bank slots once the user has chosen them. Until then, a replaced sound keeps its
// slots, and a new sound takes the model's. `groups` holds the groups of the archive (FindGroups). `file_sizes` holds
// the size of each of the archive's files as an export writes it, by its ID. `group_sizes` holds the size of the files
// that each group loads, measured as the Export window measures it (FindGroups). `joined` holds the groups that the new
// or replaced item joins once the user has chosen them, by their indices in the archive's group table. Until then, the
// item joins the groups that the window proposes. `new_groups` holds the names of the new groups that the user makes
// for the item. An import of a sequence without `models` gives the new sound Hokuto's settings, in a file in the model
// form. `new_song` marks Project > Add Sequence Sound…: the incoming sequence is an empty song (edit::EmptySong) that
// takes the new sound's name. `new_bank` marks Project > Add Bank…: the incoming file is an empty bank
// (edit::EmptyBank). A file of the Wii holds `wii`, its archive, in place of `archive`. Its groups are those with names
// (edit::WiiGroupsOf). On the Wii, `beside` holds the RWAR beside a bank file or a wave sound file that comes in, named
// `beside_name`, if the file's folder holds one under the file's name. `later` gives the console of a bank file or a
// wave sound file of the 3DS, the Wii U or the Switch that comes in. Wave data of the archive takes the place of each
// wave archive that it names (edit::WiiFileOf). `refused` says why a Wii archive takes no new item
// (edit::WiiAdditionsRefused). The Import window then shows it from the start, and Import stays grey.
struct ReplaceSource
{
    std::shared_ptr<const format::File> file;
    std::shared_ptr<const ntdlib::sar::SoundArchive> archive;
    std::shared_ptr<const ntdlib::rsar::SoundArchive> wii;
    std::vector<std::optional<edit::SoundPlace>> places;
    uint32_t sound = 0;
    edit::Incoming incoming;
    std::string file_name;
    std::vector<uint32_t> models;
    std::optional<std::vector<uint32_t>> banks;
    std::vector<edit::StartingSound> sound_starts;
    std::array<std::vector<uint32_t>, song::kGameVariables> readers;
    std::vector<edit::Group> groups;
    std::vector<std::size_t> file_sizes;
    std::vector<uint64_t> group_sizes;
    std::optional<std::vector<uint32_t>> joined;
    std::vector<std::string> new_groups;
    bool new_song = false;
    bool new_bank = false;
    std::vector<uint8_t> beside;
    std::string beside_name;
    std::optional<ntdlib::sar::Flavor> later;
    std::optional<std::string> refused;
};

// Fills `source.groups` (edit::GroupsOf, or edit::WiiGroupsOf on the Wii) once the source holds its file and its
// archive. The window reads the groups once, and its views offer them. It also measures the archive that an export
// writes: `source.file_sizes` and `source.group_sizes` (format::SizesOf). The export folds each timeline into the
// converter's subroutines (edit::FoldTimelines). The archive that the program plays holds the timelines with every bar
// in their tracks, and their files take more bytes there. The window's sizes then follow the Export window's.
void FindGroups(ReplaceSource& source);

// Fills `source.sound_starts` (edit::SoundStarts) once the source holds its file, its places and the incoming sequence
// file. A file without the places gives no starts. A timeline's commands have to be laid out with its subroutines for
// the comparison. The window therefore keeps the starts.
void FindSoundStarts(ReplaceSource& source);

// Fills `source.readers` (edit::VariableReaders) once the source holds its file and its places. A file without the
// places gives no readers.
void FindReaders(ReplaceSource& source);

// The sequences whose commands a replacement of the sequence of `source.sound` replaces with the file's commands, for
// edit::SettleTimelines: the sound's sequence when no other sound plays its file. Otherwise the new commands go after
// the file's commands, and the other sounds keep theirs.
std::vector<uint32_t> ReplacedSequences(const ReplaceSource& source);

// A start that the window offers: a label of the file, the file's first command, or a place where sounds of the archive
// start. When the file holds the commands of a sequence of the archive, `sounds` names the sounds that start at the
// start's item (edit::SoundStarts). A place where they start and no label names becomes a start of its own.
struct ReplaceStart
{
    std::string name;
    uint32_t item = 0;
    bool label = true;
    std::vector<std::string> sounds;
};

// A program that the new commands select, the bank in its slot of the sound, the bank's programs, and the program that
// takes its place. A slot without a bank has no name and no programs.
struct ProgramRow
{
    edit::Program program;
    std::string bank;
    std::vector<uint32_t> choices;
    int target = 0;
};

// What becomes of a variable of the new commands: it keeps its number, moves to another, or takes a fixed value.
enum class VariableChoice
{
    kKeep,
    kMove,
    kFix,
};

// A variable that the new commands read before they set it, or a global one that they set, and what becomes of it.
// `choices` gives values that tell its comparisons apart (song::Variable). Only a variable that the commands read
// before they set it takes a fixed value. `to` is the number that a move gives it, and `value` the fixed value.
struct VariableRow
{
    int number = 0;
    std::vector<int> choices;
    bool read = true;
    VariableChoice choice = VariableChoice::kKeep;
    int to = 0;
    int value = 0;
};

// A wave archive that a new bank's waves name, and the wave archive of the archive that takes its place: its place in
// the archive's list, or -1 for none.
struct WaveArchiveRow
{
    std::string label;
    int target = -1;
};

// A program that the sounds of a bank select and the new bank lacks, and the instruments that can play its notes: the
// present bank's, or one of the new bank's. `items` names each one, and `fixes` gives its fix. At -1, the notes stay
// silent.
struct ProgramFixRow
{
    uint32_t program = 0;
    std::vector<std::string> items;
    std::vector<edit::ProgramFix> fixes;
    int choice = -1;
};

// What the Replace window shows: its title, the heading over the file, the file, the starts with the chosen one, the
// programs, the variables, the warnings and the file that the replacement makes. An error in place of the file says why
// the sequence or the file can't go in. `left_out` holds the tracks that the new commands open and the sound leaves
// out, a bit each: its old commands open them too. With `all_tracks`, the sound allocates them as well. A file in the
// place of another has no starts, programs or tracks. A WAV file's wave has the encoding chosen in the window's menu. A
// new wave's file is `wave`: the window's Play button plays it. With `playable`, Play plays the mapping. For a file in
// the place of another, it plays the sound of `plays` chosen in the menu beside it. A bank that no sound plays gives a
// note of its first instrument. While the preview is being prepared, `preparing` says so.
//
// For a sequence, a menu for each bank slot of the sound holds the archive's banks. A field at the top of each menu
// filters its items by `filter`. A variable's row holds a menu of the choices for it. A move's menu holds the numbers
// of `numbers`, each with what the new commands and the archive's sounds do with it. An import shows the new item's
// name in a field. For a sequence or a wave sound file, a menu holds the names of the sounds whose settings the new
// sounds can copy, with the chosen one. For a bank or a wave sound file, new or in the place of another, a menu for
// each row holds the archive's wave archives. For a bank in the place of another, a menu for each program that the
// sounds select and the new bank lacks holds the instruments that can play its notes. The import makes the new version
// of the file only once the user confirms: the edit builds the whole archive again. `ready` says that the edit can be
// made. `action` names the button that makes the edit. While the edit's work runs, `working` says so, and the window
// takes only Cancel.
struct ReplaceView
{
    std::string title;
    std::string action = "Replace";
    std::string heading = "Sequence file";
    std::string source;
    bool sequence = true;
    std::vector<ReplaceStart> starts;
    std::size_t chosen = 0;
    std::array<char, 128> filter{}; // the text that filters the open menu
    std::vector<ProgramRow> programs;
    std::vector<VariableRow> variables;
    std::vector<std::string> numbers; // the variables 0 to 31, for the menus of the moves
    uint32_t left_out = 0;
    bool all_tracks = false;
    std::optional<ntdlib::sar::WaveEncoding> encoding;
    std::vector<uint8_t> wave;

    // A new wave archive's waves, each a wave file's bytes. Play plays the wave chosen in the menu of `plays`.
    std::vector<std::vector<uint8_t>> waves;

    // A WAV file's wave: the rates that it can take, the WAV file's first, with the chosen one. A WAV file without a
    // loop in the place of a looped wave can take the present wave's loop, at the WAV file's rate.
    std::vector<uint32_t> rates;
    std::size_t rate = 0;
    std::optional<edit::WaveLoop> present_loop;
    bool take_loop = false;
    std::vector<std::string> warnings;
    std::optional<format::File> result;
    std::string error;

    // A sequence's bank slots.
    std::vector<std::string> banks; // the archive's banks, for the menus of the bank slots
    std::vector<int> slots;         // the bank in each slot, by its place in the archive's list, or -1
    bool slots_changed = false;     // whether the user chose a slot's bank in this frame

    // An import's choices. A wave archive's name can stay empty. The field then shows `name_hint`.
    bool import = false;
    std::array<char, 256> name{};
    std::string name_hint;
    std::vector<std::string> models;
    std::size_t model = 0;
    std::vector<std::string> wave_archives; // the archive's wave archives, for the menus of the rows
    std::optional<int> wave_archive;        // a new wave: the wave archive that takes it, by its place in the list
    std::vector<WaveArchiveRow> wave_archive_rows;
    std::string rows_heading = "Wave archives"; // on the Wii, the rows choose wave data
    std::vector<ProgramFixRow> program_fixes;
    bool ready = false;
    std::string working;

    // Play, and the sounds that it can play: their names and their places in the sound table. While the preview plays,
    // `previewing` is set, and the button reads Stop.
    bool playable = false;
    std::vector<std::string> plays;
    std::vector<uint32_t> play_sounds;
    std::size_t play = 0;
    bool preparing = false;
    bool previewing = false;

    // The groups of the archive (edit::GroupOptions), and those that the item joins, by their indices in the archive's
    // group table. A menu lists the groups when `offers_groups` says so: when Hokuto can read the archive's tables.
    // `new_groups` holds the names of the new groups that the item joins. A new group's name can't be among
    // `taken_names`, the sorted names of the archive's items. `groups_note` says which groups the window proposes, and
    // `groups_none` what follows when no group loads the item. A choice of a group or a new group sets `groups_changed`
    // and requires no new view. Without `makes_groups`, the menu offers no new group: the Wii's groups keep their
    // numbers. On the Wii, the indices are those among the groups with names.
    bool offers_groups = false;
    bool makes_groups = true;
    std::vector<edit::GroupOption> groups;
    std::vector<uint32_t> joined;
    std::vector<std::string> new_groups;
    std::vector<std::string> taken_names;
    std::string groups_note;
    std::string groups_none;
    bool groups_changed = false;

    // How the item changes the size of each group of `groups` that it joins: a line for the warnings, or nothing for a
    // group that holds the item or that the edit can't reach. `new_group_size` gives the size of the files that a new
    // group takes. `item_files` lists the files that the item requires, its file first (edit::GroupOptions). The window
    // shows the lines of the chosen groups (GroupGrowthLines).
    std::vector<std::string> group_growth;
    std::size_t new_group_size = 0;
    std::vector<uint32_t> item_files;
};

// What the user did in the Replace window.
enum class ReplaceChoice
{
    kNone,
    kCancel,  // Cancel, Escape or the window's close button
    kReplace, // Replace or Import, once the edit can be made
    kChange,  // another start, another program for a row, the tracks, another encoding, another name or another model
    kPlay,    // Play: the new wave, the new stream or the mapping
    kStop,    // Stop, while the preview plays
};

// The view of `source`'s replacement from start `chosen`, with `programs` in the place of the programs that the new
// commands select, and the moves and fixed values of `variables`. The sound allocates the tracks that the new commands
// open and its old commands don't. With `all_tracks`, it allocates every track that the new commands open. The sound
// takes the bank slots of `source.banks` if the user chose them. Without a choice, the window starts where the sound
// named after the file starts, or else at the label named after the file, or else at the first label, or else at the
// file's first command. The window proposes the groups that load the sound and lack a file of its new bank slots.
ReplaceView MakeReplaceView(const ReplaceSource& source, std::optional<std::size_t> chosen,
                            const edit::ProgramMap& programs, const edit::VariableMap& variables, bool all_tracks);

// The view of `source`'s sequence as a new sound named `name`, a copy of sound `source.sound`, from start `chosen`,
// with `programs` in the place of the programs that the new commands select, and the moves and fixed values of
// `variables` (File > Import > Sequence…). The view holds no result. Without a choice, the window starts where the
// sound named after the file starts, or else at the label named after the file, or else at the first label, or else at
// the file's first command. The window proposes the groups that load the sound that the new one copies.
ReplaceView MakeImportView(const ReplaceSource& source, std::optional<std::size_t> chosen,
                           const edit::ProgramMap& programs, const edit::VariableMap& variables,
                           const std::string& name);

// The name that the Import window offers first for the file `file_name`: the file's name without its extension, with a
// number after it while an item of the archive holds the name or the browser shows it. `file` holds the archive, and
// `archive` is the archive that `file` builds. Hokuto's Export… names a file after its item. For the `count` sounds of
// a wave sound file, none of the names that edit::WaveSoundNames makes from it may be taken. An archive without names
// gets an empty name: the edits refuse a name there.
std::string FirstName(const format::File& file, const ntdlib::sar::SoundArchive& archive, const std::string& file_name,
                      std::size_t count = 1);

// The view of `incoming`, a bank file, a wave archive file or a wave sound file of the archive's console, as a new item
// named `name` (File > Import > Bank…, Wave Archive… or Wave Sound File…). The rows of a bank or a wave sound file pair
// each wave archive that `named` lists with the wave archive of `targets`, by its place in the archive's list, or -1
// for none. A wave sound file's new sounds copy the settings of sound `source.sound`, one of `source.models`. The view
// holds no result. For a wave sound file, the window proposes the groups that load that sound. No sound plays a new
// bank or wave archive, and the window proposes no group for it. With `source.new_bank`, the view is Project > Add
// Bank…'s, and `incoming` an empty bank.
ReplaceView MakeItemImportView(const ReplaceSource& source, const edit::IncomingFile& incoming,
                               const std::vector<edit::NamedWaveArchive>& named, const std::vector<int>& targets,
                               const std::string& name);

// The wave archives that the Import window offers first for the waves of `file`, a bank file or a wave sound file, one
// for each that `named` lists. Where a bank or a wave sound of the archive has the same file, they're the wave archives
// that the file names. So are they for a file in the place of another when `present`, the wave archives that the
// present file names, lists each of them. Otherwise they're the archive's last wave archives, in order: the wave
// archives imported last.
std::vector<int> FirstWaveArchives(const ntdlib::sar::SoundArchive& archive, std::span<const uint8_t> file,
                                   const std::vector<edit::NamedWaveArchive>& named,
                                   const std::vector<edit::NamedWaveArchive>& present = {});

// The wave archives chosen in the rows of `view`.
std::vector<int> ChosenWaveArchives(const ReplaceView& view);

// The instruments chosen in the rows of `view` for the programs that a new bank lacks.
std::vector<edit::ProgramFix> ChosenFixes(const ReplaceView& view);

// The bank slots chosen in `view`, as bank item IDs, or 0xFFFFFFFF for an empty slot. The list ends at the last slot
// with a bank. NintendoWare's tools write a sound's list that way.
std::vector<uint32_t> ChosenBanks(const ReplaceView& view);

// The text in the name field of `view`.
std::string NameOf(const ReplaceView& view);

// The view of a file in the place of item `source.sound` of `source.archive`, a bank, a wave archive or a wave sound by
// the kind of `incoming`. The rows of a bank or a wave sound file pair each wave archive that `named` lists with the
// wave archive of `targets`, by its place in the archive's list, or -1 for none. `incoming` names the chosen wave
// archives (edit::WithWaveArchiveMap), and holds the instruments of `fixes` (edit::WithProgramFixes). `changes` gives
// what the new file changes besides the item, without the fixes, and `result` the file that the replacement makes.
// Without a result, `error` says why the file can't go in. A bank's rows give each program that the sounds select and
// the new bank lacks one of `fixes`, or none. `source.places` gives where the sounds play. Without the places, a
// warning lists the present bank's programs that the new one lacks. The window proposes the groups that load the item
// and lack a file that the new file requires, such as a wave archive that its waves name.
ReplaceView MakeFileReplaceView(const ReplaceSource& source, const edit::IncomingFile& incoming,
                                const edit::FileChanges& changes, std::optional<format::File> result, std::string error,
                                const std::vector<edit::NamedWaveArchive>& named = {},
                                const std::vector<int>& targets = {}, std::span<const edit::ProgramFix> fixes = {});

// The same for a file of the Wii in the place of item `source.sound` of `source.wii`: a bank, the wave data of a bank
// or a wave sound file (edit::FileIdOf), or a wave sound's file, by the kind of `incoming`. The window offers no
// groups: the file keeps its places in them. For a bank file or a wave sound file of another console (`source.later`),
// the rows pair each wave archive that `named` lists with the wave data of `targets`, by its place among
// song::WaveArchivesOf, or -1 for none. `incoming` is then the converted file (edit::WiiFileOf).
ReplaceView MakeWiiFileReplaceView(const ReplaceSource& source, const edit::IncomingFile& incoming,
                                   const edit::FileChanges& changes, std::optional<format::File> result,
                                   std::string error, const std::vector<edit::NamedWaveArchive>& named = {},
                                   const std::vector<int>& targets = {});

// The name that the Import window offers first for the file `file_name` on the Wii: the file's name without its
// extension, with a number after it while an item of `source.wii` or one of its groups holds the name. FirstName
// follows the same rule.
std::string FirstWiiName(const ReplaceSource& source, const std::string& file_name, std::size_t count = 1);

// The view of `source`'s sequence as a new sound of `source.wii`. MakeImportView makes the same view for the other
// consoles. The sound has one bank slot. The window proposes the groups with names that load the sound that the new one
// copies. Without a model, the new sound takes Hokuto's settings (edit::AddWiiSequenceSound). Play waits for a preview
// of the Wii's new items.
ReplaceView MakeWiiImportView(const ReplaceSource& source, std::optional<std::size_t> chosen,
                              const edit::ProgramMap& programs, const edit::VariableMap& variables,
                              const std::string& name);

// The file that a new item of `incoming` brings into `source.wii`, with its wave data: a bank or a wave sound file of
// the Wii with the wave data chosen in the row of `targets` (edit::WiiFileWithWaves), a bank file or a wave sound file
// of another console converted with the rows' wave data (edit::WiiFileOf), or a wave archive's waves as the wave data
// of a new bank without instruments (edit::EmptyBank). The menu of the row of a file of the Wii lists the RWAR beside
// it first if `source.beside` holds one, then the archive's wave data in the order of song::WaveArchivesOf. A file that
// holds its wave data already, such as an empty bank, goes in as it is. Throws std::invalid_argument with a message for
// the user as those calls do.
edit::IncomingFile WiiImportedFile(const ReplaceSource& source, const edit::IncomingFile& incoming,
                                   const std::vector<edit::NamedWaveArchive>& named, const std::vector<int>& targets);

// The wave data that the Import window offers first for `incoming` on the Wii, one for each row: for a file of the Wii,
// the RWAR beside it, or else the wave data of a file of the archive with the same bytes; for a file of another
// console, the wave data of file `present` where a replacement gives one, or else the archive's last wave data in
// order. -1 leaves a row without a choice.
std::vector<int> FirstWiiWaveData(const ReplaceSource& source, const edit::IncomingFile& incoming,
                                  const std::vector<edit::NamedWaveArchive>& named,
                                  std::optional<uint32_t> present = std::nullopt);

// The view of `incoming` as a new item of `source.wii` named `name` (File > Import > Bank…, Wave Archive… or Wave Sound
// File…, and Project > Add Bank…). MakeItemImportView makes the same view for the other consoles. The rows choose the
// wave data of a bank or a wave sound file (WiiImportedFile). A wave sound file's new sounds copy the settings of sound
// `source.sound`. A wave archive goes in as the wave data of a new bank without instruments: the Wii keeps every wave
// in the wave data of a bank or a wave sound file. The view holds no result.
ReplaceView MakeWiiItemImportView(const ReplaceSource& source, const edit::IncomingFile& incoming,
                                  const std::vector<edit::NamedWaveArchive>& named, const std::vector<int>& targets,
                                  const std::string& name);

// The view of `incoming` as a new wave after the waves of the wave data of file `file_id` of `source.wii`, its wave
// `wave` (File > Import > Audio File… or Wave…). MakeNewWaveView makes the same view for the other consoles. Without a
// result, `error` says why the wave can't go in.
ReplaceView MakeWiiNewWaveView(const ReplaceSource& source, uint32_t file_id, uint32_t wave,
                               const edit::IncomingWave& incoming, bool wav, std::optional<format::File> result,
                               std::string error);

// The name of item `item` of the Wii's `archive` whose file is of `kind`: a bank's, a wave archive's by its file's ID,
// or a wave sound's.
std::string ItemName(const ntdlib::rsar::SoundArchive& archive, edit::FileKind kind, uint32_t item);

// The view of the wave of a WAV file or a wave file (`wav`) in the place of wave `wave` of wave archive `source.sound`.
// `incoming` gives the new wave in its encoding, `changes` the present wave and what plays it, and `archive_changes`
// what the new wave archive changes besides the wave archive. The new wave archive takes `archive_size` bytes. Without
// a result, `error` says why the wave can't go in.
ReplaceView MakeWaveReplaceView(const ReplaceSource& source, uint32_t wave, const edit::IncomingWave& incoming,
                                bool wav, const edit::WaveChanges& changes, const edit::FileChanges& archive_changes,
                                std::size_t archive_size, std::optional<format::File> result, std::string error);

// The same for a wave of the Wii in the place of wave `wave` of the wave data of file `source.sound` of `source.wii`.
// The wave data belongs to the file: the new waves stay beside it.
ReplaceView MakeWiiWaveReplaceView(const ReplaceSource& source, uint32_t wave, const edit::IncomingWave& incoming,
                                   bool wav, const edit::WaveChanges& changes, std::optional<format::File> result,
                                   std::string error);

// The view of `incoming` as new wave `wave` of wave archive `wave_archive` (File > Import > Audio File… or Wave…).
// `incoming` comes from a WAV file or a wave file (`wav`). A WAV file's wave has the encoding chosen in the window's
// menu. `archive_changes` gives what the new wave archive changes besides the wave archive. The new wave archive takes
// `archive_size` bytes. Without a result, `error` says why the wave can't go in.
ReplaceView MakeNewWaveView(const ReplaceSource& source, uint32_t wave_archive, uint32_t wave,
                            const edit::IncomingWave& incoming, bool wav, const edit::FileChanges& archive_changes,
                            std::size_t archive_size, std::optional<format::File> result, std::string error);

// The view of the stream of a stream file or a WAV file (`wav`) in the place of the stream of stream sound
// `source.sound`. `incoming` gives the new stream, in a WAV file's encoding chosen in the window's menu. `changes`
// gives the present stream and what plays it. `given` holds the tracks that the new stream took for the sound
// (edit::SoundTracksFor), if any. The window's Play button plays the new stream. Without a result, `error` says why the
// stream can't go in.
ReplaceView MakeStreamReplaceView(const ReplaceSource& source, const edit::IncomingStream& incoming, bool wav,
                                  const edit::StreamChanges& changes,
                                  const std::optional<std::vector<ntdlib::sar::StreamFile::Track>>& given,
                                  std::optional<format::File> result, std::string error);

// The view of `incoming`, an AAC stream, in the place of the AAC stream of 3DS stream sound `source.sound`. `changes`
// gives the present stream and what plays it. The window's Play button plays the new stream. Without a result, `error`
// says why the stream can't go in.
ReplaceView MakeAacStreamReplaceView(const ReplaceSource& source, const edit::IncomingAac& incoming,
                                     const edit::AacChanges& changes, std::optional<format::File> result,
                                     std::string error);

// The view of the stream of a stream file or a WAV file (`wav`) as new stream sound `name`, a copy of sound
// `source.sound`, one of `source.models` (File > Import > Stream…). `incoming` gives the stream, in a WAV file's
// encoding chosen in the window's menu. `made` gives the new sound's stream, its path and its details
// (edit::MakeNewStream). Without it, `error` says why the stream can't go in. The window's Play button plays the stream
// as the new sound mixes its tracks. The view holds no result.
ReplaceView MakeStreamImportView(const ReplaceSource& source, const edit::IncomingStream& incoming, bool wav,
                                 const std::string& name, const std::optional<edit::NewStream>& made,
                                 const std::string& error);

// Whether a sound, a bank or a player of the Wii's `archive` holds `name`. ntdlib's reader of the archive doesn't give
// the groups' names.
bool WiiNameTaken(const ntdlib::rsar::SoundArchive& archive, const std::string& name);

// The same views for a stream sound of the Wii, `source.sound` of `source.wii`. The sound plays the tracks of its
// stream file that its entry names, on the file's build of NW4R SND. A WAV file, or a stream file of a later console
// without tracks, takes the present stream's tracks where its channels allow (edit::ReadIncomingWiiStreamFile and
// edit::ReadIncomingWiiStreamWav). The views say which tracks the game plays. An export writes the stream beside the
// archive, and the archive's file table takes its size.
ReplaceView MakeWiiStreamReplaceView(const ReplaceSource& source, const edit::IncomingStream& incoming, bool wav,
                                     const edit::StreamChanges& changes, std::optional<format::File> result,
                                     std::string error);
ReplaceView MakeWiiStreamImportView(const ReplaceSource& source, const edit::IncomingStream& incoming, bool wav,
                                    const std::string& name, const std::optional<edit::NewStream>& made,
                                    const std::string& error);

// The name of item `item` of `archive` whose file is of `kind`: a bank's, a wave archive's or a wave sound's.
std::string ItemName(const ntdlib::sar::SoundArchive& archive, edit::FileKind kind, uint32_t item);

// The name that the browser lists for the item whose file Replace… replaces: ItemName's, but a wave sound file takes
// the name of its set of wave sounds (song::WaveSoundSets), such as WSDSET_SE. On the Wii, it takes WSD_FILE_ and its
// file ID.
std::string ReplacedName(const ntdlib::sar::SoundArchive& archive, edit::FileKind kind, uint32_t item);
std::string ReplacedName(const ntdlib::rsar::SoundArchive& archive, edit::FileKind kind, uint32_t item);

// The programs that the rows of `view` put in the place of others.
edit::ProgramMap ChosenPrograms(const ReplaceView& view);

// The item ID of the item whose file a replacement of `kind` replaces: bank, wave archive or wave sound `item`, by its
// place in the archive's list.
uint32_t ReplacedItem(edit::FileKind kind, uint32_t item);

// The moves and fixed values chosen in the rows of `view`.
edit::VariableMap ChosenVariables(const ReplaceView& view);

// The lower rate chosen in the menu of `view` for a WAV file's wave, or nothing for the WAV file's rate.
std::optional<uint32_t> ChosenRate(const ReplaceView& view);

// The bytes of the wave that Play plays: the new wave, or the wave of a new wave archive chosen in the menu.
std::span<const uint8_t> ChosenWave(const ReplaceView& view);

// The present wave's loop when it's chosen in the menu of `view` for a WAV file without a loop.
std::optional<edit::WaveLoop> ChosenLoop(const ReplaceView& view);

// Draws the Replace window while a replacement waits. `place` keeps the window's centre where the user dragged it.
// Returns what the user did in this frame.
ReplaceChoice DrawReplaceWindow(ReplaceView& view, std::optional<ImVec2>& place);

// Closes the Replace window if it's open.
void CloseReplaceWindow();

} // namespace hokuto::app
