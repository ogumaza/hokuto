// SPDX-License-Identifier: MIT

// What the Replace window shows: the source of a replacement or an import, the rows of the choices, the results of the
// choices, and the view that the window draws (replace_window.h).

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "ntdlib/dse/formats.h"
#include "ntdlib/rsar/formats.h"
#include "ntdlib/sar/formats.h"

#include "edit/dse_files.h"
#include "edit/files.h"
#include "edit/gba_midi.h"
#include "edit/groups.h"
#include "edit/midi_songs.h"
#include "edit/replace.h"
#include "edit/variables.h"
#include "format/file.h"
#include "song/ds.h"
#include "song/dse.h"
#include "song/played.h"
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
// (edit::EmptyBank). `played` contains the archive of a later console, the Wii or the DS, or the DSE set. It is empty
// on the GBA. The groups of a Wii archive are those with names (edit::WiiGroupsOf). On the Wii, `beside` holds the RWAR
// beside a bank file or a wave sound file that comes in, named `beside_name`, if the file's folder holds one under the
// file's name. Replace… sets only `beside_name`: the new file uses the current wave data. `later` gives the console of
// a bank file or a wave sound file of the 3DS, the Wii U or the Switch that comes in. Wave data of the archive takes
// the place of each wave archive that it names (edit::WiiFileOf). `refused` says why a Wii archive takes no new item
// (edit::WiiAdditionsRefused). The Import window then shows it from the start, and Import stays grey.
struct ReplaceSource
{
    std::shared_ptr<const format::File> file;
    song::Played played;
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

    // A MIDI file that the sequence comes from: its bytes, the notes of MIDI's drum channel and what plays them. On the
    // DS, also the bank's programs that play the file's programs, and the bank whose map last proposed them.
    std::vector<uint8_t> midi;
    edit::MidiDrums midi_drums;
    format::MidiDrumNotes drum_notes;
    edit::ProgramMap midi_programs;
    std::optional<uint32_t> midi_bank;

    // On DSE: the tracks or the bank that come in.
    std::optional<edit::DseIncomingSong> dse_song;
    std::optional<ntdlib::dse::WaveBank> dse_bank;
};

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

// A note of MIDI's drum channel and what plays it: a program of the bank, or -1 for the program that the file sets for
// the channel, and the key that the note maps to.
struct DrumRow
{
    int note = 0;
    int program = -1;
    int key = 0;
    std::string bank;
    std::vector<uint32_t> choices;
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

// An instrument that the notes of a MIDI file play, and the voice of the GBA song's voice group that plays it.
struct GbaInstrumentRow
{
    edit::GbaMidiInstrument instrument;
    int voice = 0;
};

// What the Replace window shows: its title, the heading over the file, the file, the starts with the chosen one, the
// programs, the variables, the warnings and the file that the replacement makes. An error in place of the file says why
// the sequence or the file can't go in. The window then shows the error without the warnings. `left_out` contains the
// tracks that the new commands open and the sound leaves out, a bit each: its old commands open them too. With
// `all_tracks`, the sound allocates them as well. A file in the place of another has no starts, programs or tracks. A
// WAV file's wave has the encoding chosen in the window's menu. A new wave's file is `wave`: the window's Play button
// plays it. With `playable`, Play plays the mapping. For a file in the place of another, it plays the sound of `plays`
// chosen in the menu beside it. A bank that no sound plays gives a note of its first instrument. While the preview is
// being prepared, `preparing` says so.
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

    // An import's choices. A wave archive's name can stay empty. The field then shows `name_hint`. An archive of the DS
    // without names shows no field: `names` is off.
    bool import = false;
    std::array<char, 256> name{};
    std::string name_hint;
    bool names = true;
    std::vector<std::string> models;
    std::size_t model = 0;
    std::vector<std::string> wave_archives; // the archive's wave archives, for the menus of the rows
    std::optional<int> wave_archive;        // a new wave: the wave archive that takes it, by its place in the list
    std::vector<WaveArchiveRow> wave_archive_rows;
    std::string rows_heading = "Wave archives"; // on the Wii, the rows choose wave data, and on the DS a sound's bank
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

    // Whether the menu of the encodings offers those of a DS wave: IMA-ADPCM, 16-bit PCM and 8-bit PCM. The first rate
    // of `rates` is then the WAV file's only when `first_rate_lower` isn't set: a DS wave's rate has a limit.
    bool ds_encodings = false;
    bool first_rate_lower = false;

    // A MIDI file that comes into a GBA file: a menu of the songs whose commands it can take, with the chosen one, and
    // a row for each instrument that the file's notes play. Each row's menu lists the 128 voices of the chosen song's
    // voice group, by `gba_voices`.
    std::vector<std::string> gba_songs;
    std::size_t gba_song = 0;
    std::vector<std::string> gba_voices;
    std::vector<GbaInstrumentRow> gba_rows;

    // A MIDI file on the other consoles: a row for each note of MIDI's drum channel. A change of a row makes new
    // commands (`drums_changed`).
    std::vector<DrumRow> drums;
    bool drums_changed = false;

    bool gba_assembly = false; // the file is a song as assembly, and has no rows

    // A SoundFont that comes into a GBA file: the address of the song's voice group in `result`. Without `gba_voices`,
    // the menu of the songs stands alone.
    uint32_t gba_group = 0;
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

// What the rows of `view` choose for the notes of MIDI's drum channel.
format::MidiDrumNotes ChosenDrumNotes(const ReplaceView& view);

// The wave archives chosen in the rows of `view`.
std::vector<int> ChosenWaveArchives(const ReplaceView& view);

// The instruments chosen in the rows of `view` for the programs that a new bank lacks.
std::vector<edit::ProgramFix> ChosenFixes(const ReplaceView& view);

// The bank slots chosen in `view`, as bank item IDs, or 0xFFFFFFFF for an empty slot. The list ends at the last slot
// with a bank. NintendoWare's tools write a sound's list that way.
std::vector<uint32_t> ChosenBanks(const ReplaceView& view);

// The text in the name field of `view`.
std::string NameOf(const ReplaceView& view);

// The voices chosen in the rows of `view` for a MIDI file's instruments.
edit::GbaVoiceMap ChosenGbaVoices(const ReplaceView& view);

// The programs that the rows of `view` put in the place of others.
edit::ProgramMap ChosenPrograms(const ReplaceView& view);

// The moves and fixed values chosen in the rows of `view`.
edit::VariableMap ChosenVariables(const ReplaceView& view);

// The lower rate chosen in the menu of `view` for a WAV file's wave, or nothing for the WAV file's rate.
std::optional<uint32_t> ChosenRate(const ReplaceView& view);

// The bytes of the wave that Play plays: the new wave, or the wave of a new wave archive chosen in the menu.
std::span<const uint8_t> ChosenWave(const ReplaceView& view);

// The present wave's loop when it's chosen in the menu of `view` for a WAV file without a loop.
std::optional<edit::WaveLoop> ChosenLoop(const ReplaceView& view);

} // namespace hokuto::app
