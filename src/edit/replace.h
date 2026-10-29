// SPDX-License-Identifier: MIT

// A sequence in place of a sound's sequence: Replace… in the browser. The sequence comes from a sequence file of the
// 3DS, the Wii U or the Switch. Those consoles' commands take the same common form in ntdlib, and a file of any of them
// goes into an archive of any of them. On the Wii, the sequence comes from a sequence file of the Wii (RSEQ). Its
// commands are read by the rules of the archive's build of NW4R SND. The sound plays the commands that the console's
// player reaches from a start in the file: one of its labels, or its first command. The sound keeps its bank slots, its
// volume and its other values in the archive's tables. The player gives a sequence only the tracks that the sound's
// entry allocates. A replacement therefore adds the tracks that the new commands open and the sound's old commands
// don't (TracksToAdd). A sound can mute a track on purpose: it leaves out a track that its commands open. Such a track
// stays out unless the caller adds it.
//
// When no other sound plays the sound's sequence file, the new file takes the place of its commands and labels. The
// sound then starts at the start in the new file. Otherwise the commands that the start reaches go after the file's
// commands, under a new label, and the sound starts there. The other sounds keep their commands. A sound whose entry in
// the tables leaves its start out starts at the file's first command. A jump to the new commands then goes in before
// that command, and every other sound of the file that starts there takes the new sequence too.
//
// A program map changes the programs that the new sequence's program changes select. A program change selects a program
// of the bank in the slot that its track has selected. Each track starts with slot 0.

#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/rsar/formats.h"
#include "ntdlib/rsar/sequence_file.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sar/sequence_file.h"
#include "ntdlib/sound_build.h"

#include "format/file.h"
#include "song/archive.h"

namespace hokuto::edit
{

// A sequence file to bring in: the console of its bytes, its commands and labels in ntdlib's common form, and its size
// in bytes. `wii` marks commands for an archive of the Wii, by the rules of its build of NW4R SND. They come from a
// sequence file of the Wii (RSEQ), and `flavor` then means nothing. With `converted`, they come from a sequence file of
// `flavor`, converted as NW4R SND reads its commands (WiiCommandsOf). `warnings` says what a conversion changed: for
// the Wii, or in the biquad filters of a file of another console.
struct Incoming
{
    ntdlib::sar::Flavor flavor = ntdlib::sar::Flavor::kNx;
    ntdlib::commands::Sequence sequence;
    std::size_t size = 0;
    bool wii = false;
    bool converted = false;
    std::vector<std::string> warnings;
};

// Reads `bytes` as a sequence file of the 3DS, the Wii U or the Switch. With `into`, it reads them for an archive of
// that console (ConsoleOf). A file of another console among the three then takes the biquad filters that come nearest
// on that console (song::MapBiquads). On the Wii, the file is read for the archive by the rules of the build of NW4R
// SND that `into` gives instead. A sequence file of the Wii (RSEQ) goes in as it is. A sequence file of another console
// goes in converted (WiiCommandsOf). The commands are read from the file's labels, or from its first command when it
// has none. The zeros that pad the file's bytecode stay out of its commands when the file's writer would add them back.
// Throws std::invalid_argument with a message for the user for bytes that aren't such a file, and for commands that
// overlap or jump into one another. The message names the kind of a file of another kind (format::NotKindText). It also
// throws for a sequence file of the Wii without the Wii's `into`: a file of the Wii goes only into an archive of the
// Wii. On the Wii, it throws for a sequence whose commands have to keep their places.
Incoming ReadIncoming(std::span<const uint8_t> bytes, const std::optional<song::Console>& into = std::nullopt);

// The commands of `read`, a sequence file of the 3DS, the Wii U or the Switch read from `bytes`, from the file's labels
// and from `starts` too. ReadIncoming reads the same commands. Throws std::invalid_argument with a message for the user
// for commands that can't be read, or that overlap or jump into one another.
ntdlib::commands::Sequence SequenceFileCommands(const ntdlib::sar::SequenceFile& read, std::span<const uint8_t> bytes,
                                                std::span<const uint32_t> starts);

// The same for a sequence file of the Wii (RSEQ), by the rules of NW4R SND's `build`.
ntdlib::commands::Sequence SequenceFileCommands(const ntdlib::rsar::SequenceFile& read, std::span<const uint8_t> bytes,
                                                const ntdlib::SndBuild& build, std::span<const uint32_t> starts);

// A program that a program change selects: the bank slot that its track has selected, and the program's number.
struct Program
{
    friend auto operator<=>(const Program&, const Program&) = default;

    int slot = 0;
    int number = 0;
};

// The programs that a sound's commands select (ProgramsSelected), and a count of the program changes with a value from
// a random number or a variable.
struct Selections
{
    std::vector<Program> programs; // in order, each once
    std::size_t undecided = 0;
};

// The programs that the commands that the player reaches from item `start` of a sequence select, and the program
// changes whose program or bank slot comes from a random number or a variable.
Selections ProgramsSelected(const ntdlib::commands::Sequence& sequence, uint32_t start);

// The programs that go in the place of others: a program of a slot becomes another program of the same slot. A program
// that the map leaves out stays as it is.
using ProgramMap = std::map<Program, int>;

// `sequence` with the programs of `programs` in the place of those that its program changes select. Only the program
// changes that the player reaches from item `start` take them. A program change that can run with several slots takes a
// new program only when the map gives each slot the same one. A program from a random number or a variable stays.
ntdlib::commands::Sequence WithPrograms(const ntdlib::commands::Sequence& sequence, uint32_t start,
                                        const ProgramMap& programs);

// The tracks that the commands that the player reaches from item `start` of a sequence open, a bit each: track 0, the
// tracks of an allocation at the start, and every track that a track start opens.
uint32_t TracksOpened(const ntdlib::commands::Sequence& sequence, uint32_t start);

// Where a sequence sound plays in a file: its sequence, and the field of the native layout that holds its start, or
// nothing for a sound whose entry leaves the start out.
struct SoundPlace
{
    uint32_t sequence = 0;
    std::optional<uint32_t> start_field;
};

// Where each sound of `archive` plays in a file, by its place in the sound table. `sequences` gives the file's sequence
// of each of the archive's files (format::SequencesOfFiles), and `start_fields` the field of each sound's start
// (format::StartFieldsOfSounds). A sound that isn't a sequence sound has no place, and neither has one whose file stays
// in the archive's bytes.
std::vector<std::optional<SoundPlace>> SoundPlaces(const ntdlib::sar::SoundArchive& archive,
                                                   std::span<const std::optional<uint32_t>> sequences,
                                                   std::span<const std::optional<uint32_t>> start_fields);

// The same for the Wii's `archive`.
std::vector<std::optional<SoundPlace>> SoundPlaces(const ntdlib::rsar::SoundArchive& archive,
                                                   std::span<const std::optional<uint32_t>> sequences,
                                                   std::span<const std::optional<uint32_t>> start_fields);

// The sounds that a replacement of sound `sound`'s sequence changes besides it, by their places in the sound table: the
// other sounds that start where it starts in a file of other sounds, without a start in their entries. Nothing in the
// tables tells their starts apart. `places` gives where each sound plays (SoundPlaces).
std::vector<uint32_t> SharingStart(std::span<const std::optional<SoundPlace>> places, uint32_t sound);

// The other sounds that play sound `sound`'s sequence file, by their places in the sound table.
std::vector<uint32_t> SharingFile(std::span<const std::optional<SoundPlace>> places, uint32_t sound);

// A sound and the item where it starts in its sequence.
struct StartingSound
{
    friend bool operator==(const StartingSound&, const StartingSound&) = default;

    uint32_t sound = 0;
    uint32_t item = 0;
};

// The sounds of `file` whose sequence holds the commands of `incoming`, in the order of the sound table, with the item
// where each one starts: the item that its entry names, or the first. The labels can differ, and so can the types and
// values of the biquad filters (song::SameButBiquads). A file that Export… writes for a sound, for its console or
// another, holds the same commands. Replace… then offers the sounds' starts where no label names them. `places` gives
// where each sound plays (SoundPlaces). Throws std::invalid_argument with a message for the user for a file that isn't
// from the Switch or the Wii U.
std::vector<StartingSound> SoundStarts(const format::File& file, std::span<const std::optional<SoundPlace>> places,
                                       const ntdlib::commands::Sequence& incoming);

// The tracks that sound `sound` of `file` has to allocate besides its tracks to play `incoming` from item `start`: the
// tracks that the new commands open and the commands that the sound plays now don't (TracksOpened). Throws
// std::invalid_argument with a message for the user for a sound that plays no sequence of the file.
uint32_t TracksToAdd(const format::File& file, std::span<const std::optional<SoundPlace>> places, uint32_t sound,
                     const ntdlib::commands::Sequence& incoming, uint32_t start);

// Throws std::invalid_argument with a message for the user for more than four bank slots, and for a slot that holds
// neither a bank of `tables` nor 0xFFFFFFFF.
void CheckBankSlots(const format::NativeTables& tables, std::span<const uint32_t> slots);

// The file after sound `sound` of `file` plays `incoming` from item `start`, with `programs` in the place of the
// programs that its program changes select. `places` gives where each sound plays (SoundPlaces). A new label at the new
// commands takes `label`, or `label` with a number after it when the file's labels hold the name already. The sound
// allocates `tracks` besides its tracks, a bit each, and so does each sound that SharingStart gives. `banks` gives the
// bank slots of the same sounds: bank item IDs, or 0xFFFFFFFF for an empty slot. Without it, they keep theirs. Throws
// std::invalid_argument with a message for the user for a sound that plays no sequence of the file, a start past the
// end of `incoming`, commands that can't move, and commands that the console's bytecode can't hold. Commands can't move
// when their targets are byte offsets or when they hold bytes that no command reaches. It also throws for tracks to add
// or bank slots in a file without the archive's tables, and for bank slots that CheckBankSlots refuses. A sound of the
// Wii has one bank slot.
format::File ReplaceSequence(const format::File& file, std::span<const std::optional<SoundPlace>> places,
                             uint32_t sound, const ntdlib::commands::Sequence& incoming, uint32_t start,
                             const ProgramMap& programs, const std::string& label, uint32_t tracks,
                             const std::optional<std::vector<uint32_t>>& banks = std::nullopt);

} // namespace hokuto::edit
