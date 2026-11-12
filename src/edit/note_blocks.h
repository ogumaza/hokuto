// SPDX-License-Identifier: MIT

// The blocks of the commands of a sequence, in the form that the note edits read: the items and blocks of the code, the
// notes and settings in a block, the runs of the note wait, and where a new command is inserted. Only the files of the
// note edits include it.

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/dse/formats.h"
#include "ntdlib/konami/seqformat.h"

#include "format/file.h"
#include "song/archive.h"
#include "song/sequence.h"

namespace hokuto::edit::note_detail
{

using ntdlib::commands::Command;

// A sequence's commands as an edit finds them: each item's offset in the bytecode, then the end, and the first item of
// each block, then the number of items. `fixed` is set for a sequence whose bytes must keep their places: one with a
// target that's a byte offset, or with bytes that no command was decoded from.
struct Code
{
    const std::vector<Command>* items = nullptr;
    std::vector<uint32_t> offsets;
    std::vector<uint32_t> starts;
    bool fixed = false;
    bool dse = false;                               // the commands are DSE's
    std::optional<ntdlib::konami::Revision> konami; // the commands are those of this revision of Konami's driver
    const song::DriverRules* driver = nullptr;      // the commands are those of one of the GBA's other drivers
    bool writes_note_wait = true; // the bytes hold the command that turns the note wait off and on (WriteWaits)
};

// The codes of DSE's commands that a block can't hold. ntdlib::dse's common form keeps them as Kind::kOther.
inline constexpr uint16_t kDseWaitForVoices = ntdlib::dse::kWaitForVoices;
inline constexpr uint16_t kDseLoopPoint = ntdlib::dse::kLoopPoint;
inline constexpr uint16_t kDseRepeatExit = ntdlib::dse::kRepeatExit;
inline constexpr uint16_t kDseLegato = ntdlib::dse::kLegato;

// The ticks of a wait whose length the bytecode gives, or nothing for another command.
std::optional<uint32_t> WaitTicks(const Command& command);

// Whether a track that runs the commands of `code` from item `from` on ends in the same tick: an end that runs always
// comes before any command that takes time or moves the track elsewhere. A note that starts there doesn't sound.
// SequenceTrack::Close (0x022B6158) releases the track's channels in the tick where the track ends.
bool EndsAt(const Code& code, uint32_t from);

// Whether `command` determines the track's timing in a way that a block can't hold: a wait whose length comes from a
// random number, a variable or a comparison, a change of the note wait, and bytes that no command was decoded from.
// Such a command is a block of its own.
bool Barrier(const Command& command);

// The same in `code`. On DSE, a command that waits for the voices of its channel (0x95) also takes a time that the
// bytes don't record. The common form has no kind for it.
bool Barrier(const Code& code, const Command& command);

// Whether `command` of `code` moves the track elsewhere, or may (EndsBlock). On DSE, a track's later passes come back
// after its loop point (0x99), and a repeat's exit (0x9E) can leave the repeat. The common form has no kinds for them.
bool EndsBlockIn(const Code& code, const Command& command);

// The commands of sequence `sequence` of `file`, with its blocks.
Code Examine(const format::File& file, uint32_t sequence);

// The item at `offset` in the bytecode. Throws for an offset where no item starts.
uint32_t ItemAt(const Code& code, uint32_t offset);

// The note command at `offset`.
uint32_t NoteAt(const Code& code, uint32_t offset);

// Whether `command` is a setting: a command that neither plays a note, waits, moves the track elsewhere nor determines
// the track's timing (Barrier). A new setting goes in at a tick (AddSettings).
bool IsSetting(const Command& command);

// Whether `command` reads or changes a variable, compares one, or runs the game's code.
bool UsesVariables(const Command& command);

// The item of the command at `offset` that an edit names: a note's command, or with `settings`, a setting's
// (MoveSettings). Throws std::invalid_argument with a message for the user for a setting that can't move.
uint32_t EditedAt(const Code& code, uint32_t offset, bool settings);

// The block that holds `item`.
std::size_t BlockOf(const Code& code, uint32_t item);

// On Konami's driver, a PSG note command at volume 0 starts no note. A volume command (C0-CF) after it starts one at
// the note command's key, and the arrangement's note names the volume command (song::ReadSequence). KonamiSilentNote
// returns the item of that note command for the volume command at item `item` of `code`: the last note command before
// the item in its block, where that command is a PSG note at volume 0. Nothing for any other item.
std::optional<uint32_t> KonamiSilentNote(const Code& code, uint32_t item);

// Whether the note command at `silent` of `code` sets the key of one note alone (KonamiSilentNote): one volume command
// starts a note between it and the next note command of its block.
bool KonamiStartsAlone(const Code& code, uint32_t silent);

// Whether the key of the note command at `silent` of `code` ends in its block: a note command follows it there.
// Otherwise a volume command of another block can start a note at that key.
bool KonamiKeyEndsInBlock(const Code& code, uint32_t silent);

// Why a note of Konami's driver that a volume command starts can't take any edit: no note command at volume 0 comes
// before the volume command in its block (KonamiSilentNote). Another pass of the track can then reach the volume
// command after another note command, and the volume command would start a note of another key there.
inline constexpr std::string_view kKonamiFarKey =
    "This note's key comes from a command in another section of the track. Edit its commands in the List Editor.";

// Why a note that a volume command starts alone can't take a new key: no note command follows its note command in the
// block (KonamiKeyEndsInBlock). A volume command of another block could start a note at that key.
inline constexpr std::string_view kKonamiOpenKey =
    "Another section of the track can take its key from this note's command. Can't transpose it.";

// Why such a note can't move or change its key: its note command sets the key of another note too
// (KonamiStartsAlone, KonamiKeyEndsInBlock). `action` names the edit, such as "move".
std::string KonamiSharedKeyRefusal(const std::string& action);

// Whether the track's notes wait for their length when `item` runs in `stretch`. Nothing when the item lies outside the
// stretch, or a command before it in the stretch changes the note wait by a random number, a variable or a comparison.
std::optional<bool> NoteWaitAt(const Code& code, const song::Stretch& stretch, uint32_t item);

// The item where the commands at the end of block `block` go in: before its last command when that command moves the
// track elsewhere, and else at the next block's start (Rewrite).
uint32_t TailOf(const Code& code, std::size_t block);

// The tick of each command of block `block` from the block's start, then the block's length, with the note wait off.
std::vector<uint32_t> BlockTicks(const Code& code, std::size_t block);

// Whether `command` is MusicPlayer2000's tie: a note of length 0. A tie sounds until an end of the tie with its key.
bool IsTie(const Command& command);

// The ticks of a note that waits for its length, or nothing for a length that the bytecode doesn't give.
std::optional<uint32_t> NoteTicks(const Command& note);

// The ticks that a note waits for on a track whose notes wait for their length, when a wait of its own can take its
// place: the note runs every time, and the bytecode gives its length.
std::optional<uint32_t> WrittenWait(const Command& note);

// Why a note that waits for its length can't take an edit. `action` names the edit, such as "remove".
std::string WaitRefusal(const Command& note, const std::string& action);

// Why a tie on the GBA can't change but in its velocity (IsTie).
inline constexpr std::string_view kTieRefusal =
    "This note is tied and lasts until a later command ends it. Hokuto can't move, resize, transpose or remove tied "
    "notes yet. You can change the velocity.";

// The states of the note wait that a track can have. Each is a bit.
inline constexpr int kWaitOff = 1;
inline constexpr int kWaitOn = 2;

// The refusals of a move, a new length and a new command on a track whose notes wait for their length, where the bytes
// have no command that turns the note wait off (Code::writes_note_wait).
inline constexpr std::string_view kTimedNote =
    "The notes of this track always wait for their lengths. Can't move this note or change its length.";
inline constexpr std::string_view kTimedCommand =
    "The notes of this track set its timing here. Can't move this command.";
inline constexpr std::string_view kTimedAddition =
    "The notes of this track set its timing here. Can't add a command at that tick.";

// The states of the note wait with which a track can enter each block of `code`, sequence `sequence` of `file`. A track
// opens with the note wait on: at a sound's start, where a command opens it, and with `labels` at a label and at the
// first item too. A DS sound starts at the first item of its sequence file, and no field names it. The tracks of
// MusicPlayer2000 and DSE open with the note wait off: their notes never wait. A DSE track opens after its start. The
// GBA's other drivers give the state at a track's opening (DriverEdits::NoteWaitOpens). A subroutine starts with the
// state at its call. A call comes back with the state at the call when its subroutine leaves the note wait alone, and
// else with any state at the subroutine's returns. A loop's end goes back with its state to each loop's start from
// which a track can reach it. A block that no track reaches has no states.
std::vector<int> EntryStates(const format::File& file, uint32_t sequence, const Code& code, bool labels = true);

// A run of a block's commands on a track whose notes wait for their length: items [`begin`, `end`) between the notes
// that a wait can't take the place of (WrittenWait). `start` is the run's tick from the block's start, or nothing when
// a note before it waits for an unknown time. `ticks` holds the tick of each item from the run's start, then the run's
// length.
struct Run
{
    uint32_t begin = 0;
    uint32_t end = 0;
    std::optional<uint32_t> start;
    std::vector<uint32_t> ticks;
};

// The runs of block `block`, in order. The block's last command stays out of them when it moves the track elsewhere.
std::vector<Run> RunsOf(const Code& code, std::size_t block);

// The run of block `block` that holds `item`, or nothing.
std::optional<Run> RunOf(const Code& code, std::size_t block, uint32_t item);

// The tick at which each block that `stretch` runs starts. The blocks stop where the time becomes unknown: at a wait
// that the bytecode doesn't give, at a change of the note wait that it doesn't give, and at a note that waits for a
// length that it doesn't give.
std::map<std::size_t, uint32_t> BlockStarts(const Code& code, const song::Stretch& stretch);

// A wait of `ticks` in place of `wait`, with the encoder's usual bytes for its length.
Command WaitOf(const Command& wait, uint32_t ticks);

// Checks a key or a velocity of a note command.
int CheckedByte(int value, const char* what);

// A command that an edit puts in at `tick` of `stretch`: a new note, or a new setting (NewSetting).
struct Addition
{
    song::Stretch stretch;
    uint32_t tick = 0;
    Command command;
};

// Where a new command goes: `tick` ticks into block `block`, and whether the track's notes wait for their length there.
struct Spot
{
    std::size_t block = 0;
    uint32_t tick = 0;
    std::optional<bool> note_wait;
};

// Where `addition` goes. Of the blocks that its stretch runs at its tick, the last one whose notes don't wait for their
// length takes it, or else the last one. A new setting goes into the first of them that runs a note at its tick
// instead. It goes in before the note, and the note plays with it. A barrier takes no command.
std::optional<Spot> SpotOf(const Code& code, const Addition& addition);

} // namespace hokuto::edit::note_detail
