// SPDX-License-Identifier: MIT

// Tests the edits of a sequence's notes (src/edit/notes.h), and how a sound's file leads to its sequence. A small
// Switch archive holds two sequence files. The first plays with the note wait off: key 60 and key 62, then two calls of
// a subroutine that plays key 67, then key 64. A second sound starts at key 64's command. Its track starts with the
// note wait on, and the first sound's track runs the same commands with it off. The second file's track keeps the note
// wait on: each note waits for its length. Each edit goes through an export of the archive. ntdlib's performer then
// plays the edited archive, and the notes that it plays have to show the edit and nothing else. New settings go in at
// their ticks the same way, and the performer has to play them there. A third file loops from the tick of its first
// note. A fourth file sets a volume and a pan between its notes, and settings move to other ticks there. A fifth file
// sets a pan from a random number on a track whose notes wait for their length, then makes the note wait unknown.

#include "edit/notes.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/commands/units.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sequence_export/performance.h"
#include "ntdlib/testing/sar_files.h"
#include "xsflib/testing/check.h"

#include "format/archive.h"
#include "format/file.h"
#include "song/archive.h"
#include "song/sequence.h"

using hokuto::edit::AddSettings;
using hokuto::edit::CommandKey;
using hokuto::edit::CommandsAfter;
using hokuto::edit::Edited;
using hokuto::edit::EditNotes;
using hokuto::edit::LimitsOf;
using hokuto::edit::MoveSettings;
using hokuto::edit::NewNote;
using hokuto::edit::NewSetting;
using hokuto::edit::NoteEdit;
using hokuto::edit::NoteLimits;
using hokuto::edit::NotePlace;
using hokuto::edit::SettingMove;
using hokuto::edit::ShiftRange;
using hokuto::edit::ShiftRangeOf;
using hokuto::edit::WhyFixedInTime;
using hokuto::format::File;
using hokuto::song::Note;
using hokuto::song::Sequence;
using hokuto::song::Setting;
using hokuto::song::Stretch;
using ntdlib::commands::Kind;
using ntdlib::sequence_export::Entry;

namespace
{

namespace sar = ntdlib::sar;
namespace sar_files = ntdlib::testing::sar_files;

// The archive's sounds.
constexpr uint32_t kSong = 0;
constexpr uint32_t kTail = 1;
constexpr uint32_t kWaiting = 2;

// The offsets of the first file's note commands.
constexpr uint32_t kKey60 = 7;
constexpr uint32_t kKey62 = 12;
constexpr uint32_t kKey64 = 25;
constexpr uint32_t kKey67 = 31;

// The first file's bytecode. Each command's offset comes first in its comment. The Switch's sequences give arguments in
// little-endian order.
const std::vector<uint8_t> kSongBytecode = {
    0xB0, 48,            // 0: timebase 48
    0xE1, 120,  0x00,    // 2: tempo 120
    0xC7, 0x00,          // 5: note wait off
    60,   100,  24,      // 7: key 60 for 24 ticks, at tick 0
    0x80, 48,            // 10: wait 48 ticks
    62,   100,  24,      // 12: key 62 for 24 ticks, at tick 48
    0x80, 48,            // 15: wait 48 ticks
    0x8A, 31,   0,    0, // 17: call 31, at tick 96
    0x8A, 31,   0,    0, // 21: call 31, at tick 120
    64,   100,  12,      // 25: key 64 for 12 ticks, at tick 144
    0x80, 24,            // 28: wait 24 ticks
    0xFF,                // 30: the end, at tick 168
    67,   100,  6,       // 31: key 67 for 6 ticks
    0x80, 24,            // 34: wait 24 ticks
    0xFD,                // 36: return
};

// The offset of the second file's note of length 0.
constexpr uint32_t kFinish = 15;

// The second file's bytecode: three notes that each wait for their length, at ticks 0, 24 and 48. No sound plays the
// note of length 0 at the label "Finish". With the note wait on, it waits until the track's notes have ended.
const std::vector<uint8_t> kWaitingBytecode = {
    0xB0, 48,        // 0: timebase 48
    0xE1, 120, 0x00, // 2: tempo 120
    60,   100, 24,   // 5: key 60 for 24 ticks
    62,   100, 24,   // 8: key 62 for 24 ticks
    64,   100, 24,   // 11: key 64 for 24 ticks
    0xFF,            // 14: the end
    65,   100, 0,    // 15: key 65 until the track's notes have ended
    0xFF,            // 18: the end
};

// The third file's bytecode: key 60 at tick 0, then a loop of a wait from tick 0. The loop's start begins a block of
// its own at the note's tick.
const std::vector<uint8_t> kLoopBytecode = {
    0xB0, 48,            // 0: timebase 48
    0xE1, 120,  0x00,    // 2: tempo 120
    0xC7, 0x00,          // 5: note wait off
    60,   100,  24,      // 7: key 60 for 24 ticks, at tick 0
    0x80, 48,            // 10: wait 48 ticks, at tick 0: the loop's start
    0x89, 10,   0,    0, // 12: jump 10, at tick 48
};

// The offsets of the fourth file's volume, pan, first note and conditional volume.
constexpr uint32_t kVolume100 = 4;
constexpr uint32_t kPan10 = 11;
constexpr uint32_t kKey60First = 6;
constexpr uint32_t kIfVolume = 18;

// The fourth file's bytecode: a volume at tick 0 and a pan at tick 48, between notes, with the note wait off. A volume
// at tick 96 runs only if the last comparison held.
const std::vector<uint8_t> kSettingsBytecode = {
    0xB0, 48,       // 0: timebase 48
    0xC7, 0x00,     // 2: note wait off
    0xC1, 100,      // 4: volume 100, at tick 0
    60,   100,  24, // 6: key 60 for 24 ticks, at tick 0
    0x80, 48,       // 9: wait 48 ticks
    0xC0, 74,       // 11: pan 10 to the right, at tick 48
    62,   100,  24, // 13: key 62 for 24 ticks, at tick 48
    0x80, 48,       // 16: wait 48 ticks
    0xA2, 0xC1, 90, // 18: if the last comparison held, volume 90, at tick 96
    0xFF,           // 21: the end, at tick 96
};

// The offsets of the fifth file's random pan and its pan after the note wait from a variable.
constexpr uint32_t kRandomPan = 2;
constexpr uint32_t kPanAfter = 17;

// The fifth file's bytecode: a pan from a random number, then two notes that wait for their length. A note wait from a
// variable follows. The note wait is unknown after it.
const std::vector<uint8_t> kRandomBytecode = {
    0xB0, 48,                       // 0: timebase 48
    0xA0, 0xC0, 32, 0x00, 96, 0x00, // 2: pan from 32 to 96 at random, at tick 0
    60,   100,  24,                 // 8: key 60 for 24 ticks
    62,   100,  24,                 // 11: key 62 for 24 ticks, at tick 24
    0xA1, 0xC7, 3,                  // 14: note wait from variable 3, at tick 48
    0xC0, 64,                       // 17: pan at the centre
    64,   100,  24,                 // 19: key 64 for 24 ticks
    0xFF,                           // 22: the end
};

// An archive whose sounds `sounds` play the sequence files `sequences`. Its bank plays a looped wave for every key.
std::vector<uint8_t> ArchiveOf(const std::vector<sar_files::SoundSpec>& sounds,
                               const std::vector<std::vector<uint8_t>>& sequences)
{
    sar_files::WaveSpec wave;
    wave.encoding = 1;
    wave.loop = true;
    wave.sample_rate = 48000;
    wave.loop_end = 64;
    wave.channels = {std::vector<uint8_t>(128, 0x10)};

    sar_files::Region region;
    region.original_key = 60;
    sar_files::VelocityTable velocities;
    velocities.entries = {{0, region}};
    sar_files::KeyTable keys;
    keys.entries = {{0, velocities}};
    sar_files::BankSpec bank;
    bank.waves = {{0x05000000, 0}};
    bank.instruments = {keys};

    // The sequence files come first, then the bank and the wave archive.
    sar_files::ArchiveSpec spec;
    spec.sounds = sounds;
    const auto files = static_cast<uint32_t>(sequences.size());
    spec.banks = {{"BANK_TEST", files}};
    spec.wave_archives = {{"WARC_TEST", files + 1}};
    spec.players = {{"PLAYER_TEST", 1}};
    for (const std::vector<uint8_t>& sequence : sequences)
    {
        spec.files.push_back({sequence, "", false});
    }
    spec.files.push_back({sar_files::Bank(false, bank), "", false});
    spec.files.push_back({sar_files::WaveArchive(false, {sar_files::Wave(false, wave)}), "", false});

    return sar_files::Archive(false, spec);
}

// The archive of the first two files.
std::vector<uint8_t> TestArchive()
{
    sar_files::SoundSpec song;
    song.name = "SEQ_SONG";
    song.volume = 127;
    song.banks = {0};
    sar_files::SoundSpec tail = song;
    tail.name = "SEQ_TAIL";
    tail.start_offset = kKey64;
    sar_files::SoundSpec waiting = song;
    waiting.name = "SEQ_WAITING";
    waiting.file = 1;

    return ArchiveOf({song, tail, waiting},
                     {sar_files::Sequence(false, kSongBytecode, {{"Song", 0}, {"Sub", kKey67}}),
                      sar_files::Sequence(false, kWaitingBytecode, {{"Waiting", 0}, {"Finish", kFinish}})});
}

// The archive of the third file.
std::vector<uint8_t> LoopArchive()
{
    sar_files::SoundSpec loop;
    loop.name = "SEQ_LOOP";
    loop.volume = 127;
    loop.banks = {0};

    return ArchiveOf({loop}, {sar_files::Sequence(false, kLoopBytecode, {{"Loop", 0}})});
}

// The archive of the fourth file.
std::vector<uint8_t> SettingsArchive()
{
    sar_files::SoundSpec settings;
    settings.name = "SEQ_SETTINGS";
    settings.volume = 127;
    settings.banks = {0};

    return ArchiveOf({settings}, {sar_files::Sequence(false, kSettingsBytecode, {{"Settings", 0}})});
}

// An archive whose sound plays the fifth file.
std::vector<uint8_t> RandomArchive()
{
    sar_files::SoundSpec random;
    random.name = "SEQ_RANDOM";
    random.volume = 127;
    random.banks = {0};

    return ArchiveOf({random}, {sar_files::Sequence(false, kRandomBytecode, {{"Random", 0}})});
}

// The notes of track 0 that sound `sound` of the archive that `file` builds plays.
Sequence Played(const File& file, uint32_t sound)
{
    const std::shared_ptr<const sar::SoundArchive> archive =
        hokuto::song::LoadArchive(hokuto::format::ExportArchive(file), "test.bfsar");
    return hokuto::song::ReadSequence(*archive, sound);
}

// The start, length and key of each note of track 0 of `sequence`.
std::vector<std::vector<uint32_t>> Notes(const Sequence& sequence)
{
    std::vector<std::vector<uint32_t>> notes;
    for (const Note& note : sequence.tracks.at(0).notes)
    {
        notes.push_back({note.start, note.length, note.key});
    }

    return notes;
}

// An edit of the note at `index` of track 0 of `sequence`, in the stretch that ran it.
NoteEdit EditOf(const Sequence& sequence, std::size_t index)
{
    const Note& note = sequence.tracks.at(0).notes.at(index);
    return NoteEdit{.command = note.command, .stretch = sequence.tracks[0].stretches.at(note.stretch)};
}

// A command that sets the volume to `volume`.
ntdlib::commands::Command Volume(int volume)
{
    ntdlib::commands::Command command;
    command.kind = Kind::kVolume;
    command.value = ntdlib::commands::Argument{ntdlib::commands::Source::kValue, volume * ntdlib::commands::kOne, 0};

    return command;
}

// The message of the std::invalid_argument that `call` throws: a refusal for the user. Nothing when it throws none.
template <class Call>
std::optional<std::string> RefusalOf(const Call& call)
{
    try
    {
        call();
    }
    catch (const std::invalid_argument& error)
    {
        return error.what();
    }

    return std::nullopt;
}

// Whether the new settings `settings` are refused with a message for the user.
bool RefusesSettings(const File& file, uint32_t sequence, const std::vector<NewSetting>& settings)
{
    return RefusalOf([&] { AddSettings(file, sequence, settings); }).has_value();
}

// Why the moves `moves` are refused, for the user, or nothing when they aren't.
std::optional<std::string> MoveRefusal(const File& file, uint32_t sequence, const std::vector<SettingMove>& moves)
{
    return RefusalOf([&] { MoveSettings(file, sequence, moves); });
}

// Whether the moves `moves` are refused with a message for the user.
bool RefusesMoves(const File& file, uint32_t sequence, const std::vector<SettingMove>& moves)
{
    return MoveRefusal(file, sequence, moves).has_value();
}

// Whether `edit` refuses with a message for the user.
bool Refuses(const File& file, uint32_t sequence, const NoteEdit& edit)
{
    return RefusalOf([&] { EditNotes(file, sequence, std::vector<NoteEdit>{edit}); }).has_value();
}

// The sound's file leads to the file's sequence. The notes lead to their commands and the stretches that ran them.
void TestCommands()
{
    const std::vector<uint8_t> bytes = TestArchive();
    const File file = hokuto::format::ImportSwitchArchive(bytes);
    const std::shared_ptr<const sar::SoundArchive> archive = hokuto::song::LoadArchive(bytes, "test.bfsar");

    const std::vector<std::optional<uint32_t>> sequences = hokuto::format::SequencesOfFiles(file, bytes, *archive);
    const Sequence song = hokuto::song::ReadSequence(*archive, kSong);

    XSFLIB_CHECK((sequences == std::vector<std::optional<uint32_t>>{0u, 1u, std::nullopt, std::nullopt}));
    const std::vector<std::vector<uint32_t>> kNotes = {
        {0, 24, 60}, {48, 24, 62}, {96, 6, 67}, {120, 6, 67}, {144, 12, 64}};
    XSFLIB_CHECK(Notes(song) == kNotes);
    std::vector<uint32_t> commands;
    std::vector<uint32_t> stretches;
    for (const Note& note : song.tracks[0].notes)
    {
        commands.push_back(note.command);
        stretches.push_back(note.stretch);
    }
    XSFLIB_CHECK((commands == std::vector<uint32_t>{kKey60, kKey62, kKey67, kKey67, kKey64}));
    XSFLIB_CHECK((stretches == std::vector<uint32_t>{0, 0, 1, 3, 4}));
    const std::vector<Stretch> kStretches = {
        {.tick = 0, .first = 0, .last = 17, .entry = Entry::kOpen, .depth = 0, .note_wait = true},
        {.tick = 96, .first = 31, .last = 36, .entry = Entry::kCall, .depth = 1, .note_wait = false},
        {.tick = 120, .first = 21, .last = 21, .entry = Entry::kReturn, .depth = 0, .note_wait = false},
        {.tick = 120, .first = 31, .last = 36, .entry = Entry::kCall, .depth = 1, .note_wait = false},
        {.tick = 144, .first = 25, .last = 30, .entry = Entry::kReturn, .depth = 0, .note_wait = false},
    };
    XSFLIB_CHECK(song.tracks[0].stretches == kStretches);
}

// A note's key, velocity and length change in its command. Each call of the subroutine plays its change.
void TestKeyVelocityLength()
{
    const File file = hokuto::format::ImportSwitchArchive(TestArchive());
    const Sequence song = Played(file, kSong);
    NoteEdit louder = EditOf(song, 1);
    louder.velocity = 90;
    NoteEdit higher = EditOf(song, 3);
    higher.transpose = 2;
    NoteEdit longer = EditOf(song, 0);
    longer.length = 36;

    const Edited edited = EditNotes(file, 0, std::vector<NoteEdit>{louder, higher, longer});

    const Sequence played = Played(edited.file, kSong);
    const std::vector<std::vector<uint32_t>> kNotes = {
        {0, 36, 60}, {48, 24, 62}, {96, 6, 69}, {120, 6, 69}, {144, 12, 64}};
    XSFLIB_CHECK(Notes(played) == kNotes);
    SEQLIB_REQUIRE(!played.tracks.empty() && played.tracks[0].notes.size() > 1);
    XSFLIB_CHECK(played.tracks[0].notes[1].velocity == 90 && played.tracks[0].notes[0].velocity == 100);
    XSFLIB_CHECK(edited.file.sequences[0]->items.size() == file.sequences[0]->items.size());
    XSFLIB_CHECK(edited.file.sequences[1] == file.sequences[1]);
}

// A move splits the wait that the note's new tick falls in. The other notes keep their ticks. A note in the subroutine
// moves in both calls.
void TestMove()
{
    const File file = hokuto::format::ImportSwitchArchive(TestArchive());
    const Sequence song = Played(file, kSong);
    NoteEdit first = EditOf(song, 0);
    first.shift = 1;
    NoteEdit later = EditOf(song, 1);
    later.shift = 12;
    NoteEdit sub = EditOf(song, 2);
    sub.shift = 6;

    const Edited edited = EditNotes(file, 0, std::vector<NoteEdit>{first, later, sub});

    const std::vector<std::vector<uint32_t>> kNotes = {
        {1, 24, 60}, {60, 24, 62}, {102, 6, 67}, {126, 6, 67}, {144, 12, 64}};
    XSFLIB_CHECK(Notes(Played(edited.file, kSong)) == kNotes);

    // Key 60 goes one tick into the wait after it, and key 62 between the two parts of the wait after it. Key 67 does
    // the same in the subroutine. The second sound still starts at key 64. The subroutine's label stays at its start,
    // now a wait of 6 ticks before key 67.
    XSFLIB_CHECK(edited.file.sequences[0]->items.size() == file.sequences[0]->items.size() + 3);
    SEQLIB_REQUIRE(edited.items.size() > 14);
    XSFLIB_CHECK((edited.items[3] == 4u && edited.items[4] == 5u && edited.items[5] == 7u && edited.items[6] == 8u));
    XSFLIB_CHECK((edited.items[12] == 15u && edited.items[13] == 16u && edited.items[14] == 17u));
    XSFLIB_CHECK(Notes(Played(edited.file, kTail)) == (std::vector<std::vector<uint32_t>>{{0, 12, 64}}));
    const File again = hokuto::format::ImportSwitchArchive(hokuto::format::ExportArchive(edited.file));
    const ntdlib::commands::Sequence& commands = *again.sequences[0];
    XSFLIB_CHECK(commands.labels.size() == 2 && commands.labels.at(1).name == "Sub");
    const std::size_t sub_start = commands.labels.at(1).item;
    XSFLIB_CHECK(commands.items.at(sub_start).kind == ntdlib::commands::Kind::kWait);
    XSFLIB_CHECK(commands.items.at(sub_start).value.value == 6 * ntdlib::commands::kOne);
    XSFLIB_CHECK(commands.items.at(sub_start + 1).key == 67);
}

// A note moves only inside its block. A note that one track runs with the note wait on and another with it off doesn't
// move. In the stretch that runs it with the note wait off, it doesn't change its length or go either, and no note goes
// in beside it: the other track's notes would wait for them. Its velocity still changes.
void TestMoveLimits()
{
    const File file = hokuto::format::ImportSwitchArchive(TestArchive());
    const Sequence song = Played(file, kSong);

    const std::optional<ShiftRange> range = ShiftRangeOf(file, 0, kKey62, song.tracks[0].stretches[0]);
    const std::optional<ShiftRange> sub = ShiftRangeOf(file, 0, kKey67, song.tracks[0].stretches[1]);

    XSFLIB_CHECK(range == (ShiftRange{48, 48}) && sub == (ShiftRange{0, 24}));
    NoteEdit too_late = EditOf(song, 1);
    too_late.shift = 49;
    NoteEdit too_early = EditOf(song, 0);
    too_early.shift = -1;
    XSFLIB_CHECK(Refuses(file, 0, too_late) && Refuses(file, 0, too_early));

    // The second sound runs key 64 with the note wait on, and the first sound with it off.
    const Sequence tail = Played(file, kTail);
    NoteEdit waiting = EditOf(tail, 0);
    waiting.shift = 6;
    XSFLIB_CHECK(Refuses(file, 0, waiting));
    XSFLIB_CHECK(!ShiftRangeOf(file, 0, kKey64, tail.tracks[0].stretches[0]));

    NoteEdit moved = EditOf(song, 4);
    moved.shift = 6;
    NoteEdit longer = EditOf(song, 4);
    longer.length = 13;
    NoteEdit removed = EditOf(song, 4);
    removed.remove = true;
    NoteEdit softer = EditOf(song, 4);
    softer.velocity = 90;
    XSFLIB_CHECK(Refuses(file, 0, moved) && Refuses(file, 0, longer) && Refuses(file, 0, removed));
    XSFLIB_CHECK(!Refuses(file, 0, softer));
    XSFLIB_CHECK(!ShiftRangeOf(file, 0, kKey64, song.tracks[0].stretches[4]));
    const std::vector<NoteLimits> limits =
        LimitsOf(file, 0, std::vector<NotePlace>{{kKey64, song.tracks[0].stretches[4]}});
    XSFLIB_CHECK(limits.size() == 1 && !limits[0].shift && !limits[0].length);
    const std::vector<NewNote> added = {
        {.stretch = song.tracks[0].stretches[4], .tick = 150, .key = 65, .velocity = 80, .length = 6}};
    XSFLIB_CHECK(RefusalOf([&] { EditNotes(file, 0, {}, added); }).has_value());
}

// The limits of several notes come from one look at the sequence: each note's range and its command's length. A note
// that can't move has neither, and neither has a wait. An offset where no command starts is refused.
void TestLimits()
{
    const File file = hokuto::format::ImportSwitchArchive(TestArchive());
    const Sequence song = Played(file, kSong);
    const Sequence tail = Played(file, kTail);
    const std::vector<NotePlace> places = {{kKey60, song.tracks[0].stretches[0]},
                                           {kKey67, song.tracks[0].stretches[1]},
                                           {kKey64, tail.tracks[0].stretches[0]}};

    const std::vector<NoteLimits> limits = LimitsOf(file, 0, places);

    SEQLIB_REQUIRE(limits.size() == 3);
    XSFLIB_CHECK(limits[0].shift == (ShiftRange{0, 96}) && limits[0].length == 24u);
    XSFLIB_CHECK(limits[1].shift == (ShiftRange{0, 24}) && limits[1].length == 6u);
    XSFLIB_CHECK(!limits[2].shift && !limits[2].length);
    const std::vector<NoteLimits> wait =
        LimitsOf(file, 0, std::vector<NotePlace>{{.command = 10, .stretch = song.tracks[0].stretches[0]}});
    XSFLIB_CHECK(wait.size() == 1 && !wait[0].shift && !wait[0].length);
    XSFLIB_CHECK(RefusalOf([&] { LimitsOf(file, 0, std::vector<NotePlace>{{.command = 11}}); }).has_value());
}

// A note goes, and new notes go in at their ticks. A new note at the end of a block goes before the call that ends it.
// A new note in the subroutine plays in both calls. The new notes in the first block move the second sound's start. The
// sound still starts at key 64.
void TestRemoveAndAdd()
{
    const File file = hokuto::format::ImportSwitchArchive(TestArchive());
    const Sequence song = Played(file, kSong);
    NoteEdit removed = EditOf(song, 0);
    removed.remove = true;
    const std::vector<NewNote> added = {
        {.stretch = song.tracks[0].stretches[0], .tick = 72, .key = 65, .velocity = 80, .length = 12},
        {.stretch = song.tracks[0].stretches[0], .tick = 96, .key = 59, .velocity = 80, .length = 6},
        {.stretch = song.tracks[0].stretches[1], .tick = 100, .key = 72, .velocity = 80, .length = 3},
    };

    const Edited edited = EditNotes(file, 0, std::vector<NoteEdit>{removed}, added);

    const std::vector<std::vector<uint32_t>> kNotes = {{48, 24, 62}, {72, 12, 65}, {96, 6, 59},  {96, 6, 67},
                                                       {100, 3, 72}, {120, 6, 67}, {124, 3, 72}, {144, 12, 64}};
    XSFLIB_CHECK(Notes(Played(edited.file, kSong)) == kNotes);
    XSFLIB_CHECK(!edited.items[3]);
    XSFLIB_CHECK(Notes(Played(edited.file, kTail)) == (std::vector<std::vector<uint32_t>>{{0, 12, 64}}));
}

// After an edit, the commands of the notes that stay and of the new notes lead to the notes that the edited sequence
// plays. A note that goes leaves no command. Each command gives its note's key.
void TestCommandsAfter()
{
    const File file = hokuto::format::ImportSwitchArchive(TestArchive());
    const Sequence song = Played(file, kSong);
    NoteEdit removed = EditOf(song, 0);
    removed.remove = true;
    NoteEdit moved = EditOf(song, 1);
    moved.shift = 12;
    const std::vector<NewNote> added = {
        {.stretch = song.tracks[0].stretches[0], .tick = 72, .key = 65, .velocity = 80, .length = 12}};
    const Edited edited = EditNotes(file, 0, std::vector<NoteEdit>{removed, moved}, added);

    const std::vector<uint32_t> after =
        CommandsAfter(file, 0, edited, std::vector<uint32_t>{kKey60, kKey62, kKey67, kKey64});

    SEQLIB_REQUIRE(after.size() == 4);
    std::vector<int> keys;
    for (const uint32_t command : after)
    {
        keys.push_back(CommandKey(edited.file, 0, command));
    }
    XSFLIB_CHECK((keys == std::vector<int>{62, 67, 64, 65}));
    const Sequence played = Played(edited.file, kSong);
    std::vector<uint32_t> commands;
    for (const Note& note : played.tracks.at(0).notes)
    {
        commands.push_back(note.command);
    }
    XSFLIB_CHECK((commands == std::vector<uint32_t>{after[0], after[3], after[1], after[1], after[2]}));
    XSFLIB_CHECK(RefusalOf([&] { CommandKey(file, 0, 10); }).has_value());
}

// On a track whose notes wait for their length, a key or velocity changes, and a note that goes leaves a wait. The
// notes after it keep their ticks. A note of length 0 can't go or move.
void TestNoteWait()
{
    const File file = hokuto::format::ImportSwitchArchive(TestArchive());
    const Sequence waiting = Played(file, kWaiting);
    NoteEdit softer = EditOf(waiting, 0);
    softer.velocity = 50;
    NoteEdit removed = EditOf(waiting, 1);
    removed.remove = true;
    NoteEdit finish;
    finish.command = kFinish;
    finish.stretch = {.first = kFinish, .last = kFinish + 3, .note_wait = true};
    finish.remove = true;
    NoteEdit finish_moved = finish;
    finish_moved.remove = false;
    finish_moved.shift = 6;

    const Edited edited = EditNotes(file, 1, std::vector<NoteEdit>{softer, removed});

    const Sequence played = Played(edited.file, kWaiting);
    XSFLIB_CHECK(Notes(played) == (std::vector<std::vector<uint32_t>>{{0, 24, 60}, {48, 24, 64}}));
    SEQLIB_REQUIRE(!played.tracks.empty() && !played.tracks[0].notes.empty());
    XSFLIB_CHECK(played.tracks[0].notes[0].velocity == 50);
    XSFLIB_CHECK(edited.file.sequences[1]->items.size() == file.sequences[1]->items.size());
    XSFLIB_CHECK(Refuses(file, 1, finish) && Refuses(file, 1, finish_moved));
    const std::vector<NoteLimits> limits =
        LimitsOf(file, 1, std::vector<NotePlace>{{kFinish, finish.stretch}, {8, waiting.tracks[0].stretches[0]}});
    XSFLIB_CHECK(!limits.at(0).shift && limits.at(1).shift == (ShiftRange{24, 47}) && limits.at(1).length == 24u);
}

// A note can't start in the tick where its track ends. The console releases the track's notes in that tick. The second
// file's track ends at tick 72. Key 62 moves 47 ticks later at most, and a new note can't go at tick 72.
void TestTrackEnd()
{
    const File file = hokuto::format::ImportSwitchArchive(TestArchive());
    const Sequence waiting = Played(file, kWaiting);
    NoteEdit to_end = EditOf(waiting, 1);
    to_end.shift = 48;
    NoteEdit before_end = to_end;
    before_end.shift = 47;
    const std::vector<NewNote> at_end = {
        {.stretch = waiting.tracks[0].stretches[0], .tick = 72, .key = 67, .velocity = 80, .length = 6}};

    XSFLIB_CHECK(Refuses(file, 1, to_end));
    XSFLIB_CHECK(RefusalOf([&] { EditNotes(file, 1, {}, at_end); }).has_value());

    const Sequence moved = Played(EditNotes(file, 1, std::vector<NoteEdit>{before_end}).file, kWaiting);
    std::vector<std::pair<uint32_t, int>> starts;
    for (const Note& note : moved.tracks.at(0).notes)
    {
        starts.emplace_back(note.start, note.key);
    }
    XSFLIB_CHECK(starts == (std::vector<std::pair<uint32_t, int>>{{0, 60}, {48, 64}, {71, 62}}));
}

// On a track whose notes wait for their length, a move, a new length and a new note first turn the note wait off around
// the notes of their run. Each of those notes then waits out its length with a wait of its own, and the note wait comes
// back on after the run. Every other note keeps its tick.
void TestNoteWaitWritten()
{
    const File file = hokuto::format::ImportSwitchArchive(TestArchive());
    const Sequence waiting = Played(file, kWaiting);
    NoteEdit longer = EditOf(waiting, 0);
    longer.length = 36;
    NoteEdit later = EditOf(waiting, 1);
    later.shift = 6;
    const std::vector<NewNote> added = {
        {.stretch = waiting.tracks[0].stretches[0], .tick = 12, .key = 67, .velocity = 80, .length = 6}};

    const Edited edited = EditNotes(file, 1, std::vector<NoteEdit>{longer, later}, added);

    const std::vector<std::vector<uint32_t>> kNotes = {{0, 36, 60}, {12, 6, 67}, {30, 24, 62}, {48, 24, 64}};
    XSFLIB_CHECK(Notes(Played(edited.file, kWaiting)) == kNotes);

    // The note wait goes off before the timebase and comes back on before the end. The new note splits the wait after
    // key 60, and key 62 splits the wait after its old place.
    const std::vector<ntdlib::commands::Command>& items = edited.file.sequences[1]->items;
    std::vector<Kind> kinds;
    for (const ntdlib::commands::Command& item : items)
    {
        kinds.push_back(item.kind);
    }
    const std::vector<Kind> kKinds = {Kind::kNoteWait, Kind::kTimebase, Kind::kTempo, Kind::kNote,
                                      Kind::kWait,     Kind::kNote,     Kind::kWait,  Kind::kWait,
                                      Kind::kNote,     Kind::kWait,     Kind::kNote,  Kind::kWait,
                                      Kind::kNoteWait, Kind::kEnd,      Kind::kNote,  Kind::kEnd};
    XSFLIB_CHECK(kinds == kKinds);
    XSFLIB_CHECK(items.at(0).value.value == 0 && items.at(12).value.value == ntdlib::commands::kOne);
    SEQLIB_REQUIRE(edited.items.size() > 5);
    XSFLIB_CHECK((edited.items[2] == 3u && edited.items[3] == 8u && edited.items[4] == 10u && edited.items[5] == 13u));
    XSFLIB_CHECK(edited.added == std::vector<uint32_t>{5});
    const std::vector<ntdlib::commands::Label> kLabels = {{"Waiting", 0}, {"Finish", 14}};
    XSFLIB_CHECK(edited.file.sequences[1]->labels == kLabels);
}

// New settings go in at their ticks. A setting inside a wait splits it, and a setting at a note's tick goes in before
// the note. A setting at the end of a block goes before the call that ends it, and one between the calls goes before
// the second call. A setting in the subroutine plays in both calls. The notes keep their ticks. A note, a wait and a
// jump are refused, and so is a tick past the stretch's end.
void TestAddSettings()
{
    const File file = hokuto::format::ImportSwitchArchive(TestArchive());
    const Sequence song = Played(file, kSong);
    const std::vector<Stretch>& stretches = song.tracks[0].stretches;
    const std::vector<NewSetting> settings = {
        {.stretch = stretches[0], .tick = 72, .command = Volume(90)},
        {.stretch = stretches[0], .tick = 48, .command = Volume(80)},
        {.stretch = stretches[0], .tick = 96, .command = Volume(70)},
        {.stretch = stretches[1], .tick = 100, .command = Volume(60)},
        {.stretch = stretches[2], .tick = 120, .command = Volume(75)},
    };
    ntdlib::commands::Command note = Volume(100);
    note.kind = Kind::kNote;
    ntdlib::commands::Command wait = Volume(12);
    wait.kind = Kind::kWait;
    ntdlib::commands::Command jump;
    jump.kind = Kind::kJump;

    const Edited edited = AddSettings(file, 0, settings);

    const Sequence played = Played(edited.file, kSong);
    XSFLIB_CHECK(Notes(played) == Notes(song));
    std::vector<Setting> volumes = played.tracks[0].volume;
    std::erase_if(volumes, [](const Setting& volume) { return volume.tick == 0; });
    const std::vector<Setting> kVolumes = {{48, 80}, {72, 90}, {96, 70}, {100, 60}, {120, 75}, {124, 60}};
    XSFLIB_CHECK(volumes == kVolumes);
    const std::vector<ntdlib::commands::Command>& items = edited.file.sequences[0]->items;
    SEQLIB_REQUIRE(edited.added.size() == 5 && edited.items[5] && edited.items[7] && edited.items[8]);
    XSFLIB_CHECK(edited.added[1] + 1 == *edited.items[5] && items.at(*edited.items[5]).key == 62);
    XSFLIB_CHECK(edited.added[2] + 1 == *edited.items[7] && items.at(*edited.items[7]).kind == Kind::kCall);
    XSFLIB_CHECK(edited.added[4] + 1 == *edited.items[8] && items.at(*edited.items[8]).kind == Kind::kCall);
    XSFLIB_CHECK(Notes(Played(edited.file, kTail)) == (std::vector<std::vector<uint32_t>>{{0, 12, 64}}));
    for (const ntdlib::commands::Command& refused : {note, wait, jump})
    {
        XSFLIB_CHECK(RefusesSettings(file, 0, {{.stretch = stretches[0], .tick = 24, .command = refused}}));
    }
    XSFLIB_CHECK(RefusesSettings(file, 0, {{.stretch = stretches[0], .tick = 200, .command = Volume(90)}}));
}

// A loop starts at the tick of a note, after the note. A new setting at that tick goes in before the note all the same.
// The note plays with the setting, and the loop doesn't run it again.
void TestAddSettingBeforeLoop()
{
    const File file = hokuto::format::ImportSwitchArchive(LoopArchive());
    const Sequence loop = Played(file, 0);
    const std::vector<NewSetting> settings = {
        {.stretch = loop.tracks[0].stretches[0], .tick = 0, .command = Volume(90)}};

    const Edited edited = AddSettings(file, 0, settings);

    const std::vector<ntdlib::commands::Command>& items = edited.file.sequences[0]->items;
    SEQLIB_REQUIRE(edited.added.size() == 1 && edited.items[3] && edited.items[4] && edited.items[5]);
    XSFLIB_CHECK(edited.added[0] == 3 && *edited.items[3] == 4 && items.at(4).kind == Kind::kNote);
    XSFLIB_CHECK(items.at(*edited.items[5]).kind == Kind::kJump && items[*edited.items[5]].target == *edited.items[4]);
    XSFLIB_CHECK(Notes(Played(edited.file, 0)) == Notes(loop));
}

// On a track whose notes wait for their length, a new setting first turns the note wait off around the notes of its
// run. The notes keep their ticks.
void TestAddSettingWaiting()
{
    const File file = hokuto::format::ImportSwitchArchive(TestArchive());
    const Sequence waiting = Played(file, kWaiting);
    const std::vector<NewSetting> settings = {
        {.stretch = waiting.tracks[0].stretches[0], .tick = 12, .command = Volume(90)}};

    const Edited edited = AddSettings(file, 1, settings);

    const Sequence played = Played(edited.file, kWaiting);
    XSFLIB_CHECK(Notes(played) == Notes(waiting));
    std::vector<Setting> volumes = played.tracks[0].volume;
    std::erase_if(volumes, [](const Setting& volume) { return volume.tick == 0; });
    XSFLIB_CHECK(volumes == (std::vector<Setting>{{12, 90}}));
    SEQLIB_REQUIRE(edited.added.size() == 1);
    XSFLIB_CHECK(edited.file.sequences[1]->items.at(edited.added[0]).kind == Kind::kVolume);
    XSFLIB_CHECK(edited.file.sequences[1]->items.at(0).kind == Kind::kNoteWait);
}

// Settings move to other ticks. Two moves into one wait split it in three. The notes keep their ticks, and the
// performer plays the volume and the pan at their new ticks. A setting moves as far as the start and the end of its
// block. A note, the timebase and a command that runs only if the last comparison held don't move, and neither does a
// setting past its block.
void TestMoveSettings()
{
    const File file = hokuto::format::ImportSwitchArchive(SettingsArchive());
    const Sequence song = Played(file, 0);
    const Stretch& stretch = song.tracks[0].stretches.at(0);
    const std::vector<SettingMove> moves = {{.command = kVolume100, .stretch = stretch, .shift = 24},
                                            {.command = kPan10, .stretch = stretch, .shift = -36}};

    const Edited edited = MoveSettings(file, 0, moves);
    const std::optional<ShiftRange> volume_range = ShiftRangeOf(file, 0, kVolume100, stretch);
    const std::optional<ShiftRange> pan_range = ShiftRangeOf(file, 0, kPan10, stretch);

    const Sequence played = Played(edited.file, 0);
    XSFLIB_CHECK(Notes(played) == Notes(song));
    std::vector<Setting> volumes = played.tracks[0].volume;
    std::erase_if(volumes, [](const Setting& volume) { return volume.tick == 0 || volume.tick >= 96; });
    XSFLIB_CHECK(volumes == (std::vector<Setting>{{24, 100}}));
    std::vector<Setting> pans = played.tracks[0].pan;
    std::erase_if(pans, [](const Setting& pan) { return pan.tick == 0; });
    XSFLIB_CHECK(pans == (std::vector<Setting>{{12, 10}}));
    const std::vector<ntdlib::commands::Command>& items = edited.file.sequences[0]->items;
    SEQLIB_REQUIRE(items.size() == 12 && edited.items[2] && edited.items[5]);
    XSFLIB_CHECK(*edited.items[5] == 4 && *edited.items[2] == 6 && items[6].kind == Kind::kVolume);
    XSFLIB_CHECK(items[3].kind == Kind::kWait && items[3].value.value == 12 * ntdlib::commands::kOne);
    XSFLIB_CHECK(items[7].kind == Kind::kWait && items[7].value.value == 24 * ntdlib::commands::kOne);
    XSFLIB_CHECK((volume_range == ShiftRange{0, 96} && pan_range == ShiftRange{48, 48}));
    for (const uint32_t refused : {kKey60First, kIfVolume, 0u})
    {
        XSFLIB_CHECK(RefusesMoves(file, 0, {{.command = refused, .stretch = stretch, .shift = 12}}));
    }
    XSFLIB_CHECK(RefusesMoves(file, 0, {{.command = kIfVolume, .stretch = stretch, .shift = -12}}));
    XSFLIB_CHECK(RefusesMoves(file, 0, {{.command = kPan10, .stretch = stretch, .shift = 49}}));
    XSFLIB_CHECK(RefusesMoves(file, 0, {{.command = kVolume100, .stretch = stretch, .shift = -1}}));
    XSFLIB_CHECK(!RefusesMoves(file, 0, {{.command = kPan10, .stretch = stretch, .shift = 48}}));
    XSFLIB_CHECK(!RefusesMoves(file, 0, {{.command = kPan10, .stretch = stretch, .shift = -48}}));
}

// On a track whose notes wait for their length, a setting moves once the note wait turns off around the notes of its
// run. The tempo goes into the wait that takes the place of the first note's. The notes keep their ticks. A move past
// the run's end is refused, and a move of 0 ticks changes nothing.
void TestMoveSettingWaiting()
{
    const File file = hokuto::format::ImportSwitchArchive(TestArchive());
    const Sequence waiting = Played(file, kWaiting);
    const Stretch& stretch = waiting.tracks[0].stretches[0];
    const std::vector<SettingMove> moves = {{.command = 2, .stretch = stretch, .shift = 12}};
    const std::vector<SettingMove> still = {{.command = 2, .stretch = stretch}};

    const Edited edited = MoveSettings(file, 1, moves);
    const Edited unchanged = MoveSettings(file, 1, still);

    const Sequence played = Played(edited.file, kWaiting);
    XSFLIB_CHECK(Notes(played) == Notes(waiting));
    const std::vector<ntdlib::commands::Command>& items = edited.file.sequences[1]->items;
    SEQLIB_REQUIRE(edited.items[1] && *edited.items[1] >= 1);
    const uint32_t tempo = *edited.items[1];
    XSFLIB_CHECK(items.at(0).kind == Kind::kNoteWait && items.at(tempo).kind == Kind::kTempo);
    XSFLIB_CHECK(items.at(tempo - 1).kind == Kind::kWait &&
                 items[tempo - 1].value.value == 12 * ntdlib::commands::kOne);
    XSFLIB_CHECK(items.at(tempo - 2).kind == Kind::kNote && items[tempo - 2].key == 60);
    XSFLIB_CHECK(RefusesMoves(file, 1, {{.command = 2, .stretch = stretch, .shift = 73}}));
    XSFLIB_CHECK(unchanged.file == file);
}

// Like any setting, a pan from a random number moves on a track whose notes wait for their length. After a note wait
// from a variable, a setting stays: the note wait there is unknown.
void TestMoveRandomSetting()
{
    const File file = hokuto::format::ImportSwitchArchive(RandomArchive());
    const Sequence song = Played(file, 0);
    const Stretch& stretch = song.tracks[0].stretches.at(0);
    const std::vector<SettingMove> later = {{.command = kRandomPan, .stretch = stretch, .shift = 12}};
    const std::vector<SettingMove> after = {{.command = kPanAfter, .stretch = stretch, .shift = 12}};
    const std::vector<NotePlace> places = {{kRandomPan, stretch}, {kPanAfter, stretch}};

    const Edited moved = MoveSettings(file, 0, later);
    const std::vector<NoteLimits> limits = LimitsOf(file, 0, places);

    XSFLIB_CHECK(Notes(Played(moved.file, 0)) == Notes(song));
    const std::vector<ntdlib::commands::Command>& items = moved.file.sequences[0]->items;
    SEQLIB_REQUIRE(moved.items[1]);
    XSFLIB_CHECK(items.at(*moved.items[1]).kind == Kind::kPan &&
                 items[*moved.items[1]].value.source == ntdlib::commands::Source::kRandom);
    XSFLIB_CHECK(items.at(*moved.items[1] - 1).kind == Kind::kWait &&
                 items[*moved.items[1] - 1].value.value == 12 * ntdlib::commands::kOne);
    SEQLIB_REQUIRE(limits.size() == 2);
    XSFLIB_CHECK(limits[0].shift == (ShiftRange{0, 48}) && !limits[1].shift);
    XSFLIB_CHECK(MoveRefusal(file, 0, after) ==
                 "A command before this one changes the note wait as the track plays. Hokuto can't move it.");
}

// Only a setting moves in time: not a note wait, the timebase or a track's allocation. A setting that runs only if the
// last comparison held stays, and so does one that reads a variable.
void TestWhyFixedInTime()
{
    ntdlib::commands::Command conditional = Volume(90);
    conditional.conditional = true;
    ntdlib::commands::Command variable = Volume(0);
    variable.value = ntdlib::commands::Argument{ntdlib::commands::Source::kVariable, 3, 0};
    ntdlib::commands::Command note_wait;
    note_wait.kind = Kind::kNoteWait;
    ntdlib::commands::Command timebase;
    timebase.kind = Kind::kTimebase;
    ntdlib::commands::Command tracks;
    tracks.kind = Kind::kAllocateTracks;

    const std::string kOnlySettings = "Only a setting, such as a volume, moves to another tick.";
    XSFLIB_CHECK(!WhyFixedInTime(Volume(90)));
    XSFLIB_CHECK(WhyFixedInTime(conditional) ==
                 "Hokuto can't move a command that runs only if the last comparison held.");
    XSFLIB_CHECK(WhyFixedInTime(variable) == "Hokuto can't move a command that reads or changes a variable.");
    XSFLIB_CHECK(WhyFixedInTime(note_wait) == kOnlySettings && WhyFixedInTime(timebase) == kOnlySettings);
    XSFLIB_CHECK(WhyFixedInTime(tracks) == kOnlySettings);
}

// An edit of a command that isn't a note, a key past 127 and a velocity past 127 are refused.
void TestRefusals()
{
    const File file = hokuto::format::ImportSwitchArchive(TestArchive());
    const Sequence song = Played(file, kSong);
    NoteEdit wait = EditOf(song, 0);
    wait.command = 10;
    NoteEdit high = EditOf(song, 0);
    high.transpose = 68;
    NoteEdit loud = EditOf(song, 0);
    loud.velocity = 128;

    XSFLIB_CHECK(Refuses(file, 0, wait) && Refuses(file, 0, high) && Refuses(file, 0, loud));
}

} // namespace

int main()
{
    TestCommands();
    TestKeyVelocityLength();
    TestMove();
    TestMoveLimits();
    TestLimits();
    TestRemoveAndAdd();
    TestCommandsAfter();
    TestNoteWait();
    TestTrackEnd();
    TestNoteWaitWritten();
    TestAddSettings();
    TestAddSettingBeforeLoop();
    TestAddSettingWaiting();
    TestMoveSettings();
    TestMoveSettingWaiting();
    TestMoveRandomSetting();
    TestWhyFixedInTime();
    TestRefusals();

    return xsflib::testing::Summary("notes");
}
