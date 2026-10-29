// SPDX-License-Identifier: MIT

// Tests the edits of any command of a sequence (src/edit/commands.h). A small Switch archive holds one sequence file.
// Its sound's track 0 opens track 1, plays key 60, calls a subroutine that plays key 67, and plays key 62 and key 64.
// Its volume comes from a variable before key 62, and a random wait comes before key 64. Track 1 sets its pan and plays
// no notes. Each edit goes through an export of the archive. ntdlib's performer then plays the edited archive, and the
// notes that it plays have to show the edit and nothing else. A small 3DS archive checks the commands that the 3DS's
// player skips.

#include "edit/commands.h"

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
#include "ntdlib/sound_build.h"
#include "ntdlib/testing/sar_files.h"
#include "xsflib/testing/check.h"

#include "edit/notes.h"
#include "format/archive.h"
#include "format/file.h"
#include "song/archive.h"
#include "song/sequence.h"

using hokuto::edit::ArgumentRange;
using hokuto::edit::ChangeCommand;
using hokuto::edit::ChangeCommands;
using hokuto::edit::CommandAt;
using hokuto::edit::CommandPlace;
using hokuto::edit::CommandRow;
using hokuto::edit::CommandRows;
using hokuto::edit::DrawsRandom;
using hokuto::edit::Edited;
using hokuto::edit::InsertCommand;
using hokuto::edit::ItemOf;
using hokuto::edit::LabelPlace;
using hokuto::edit::LabelsOf;
using hokuto::edit::MoveAndChangeCommands;
using hokuto::edit::Nearest;
using hokuto::edit::NewCommand;
using hokuto::edit::RemoveCommands;
using hokuto::edit::SettingMove;
using hokuto::edit::ValueRange;
using hokuto::format::File;
using hokuto::song::Console;
using hokuto::song::Note;
using hokuto::song::Sequence;
using hokuto::song::Setting;
using hokuto::song::Track;
using ntdlib::commands::Argument;
using ntdlib::commands::Command;
using ntdlib::commands::Kind;
using ntdlib::commands::kOne;
using ntdlib::commands::Source;

namespace
{

namespace sar = ntdlib::sar;
namespace sar_files = ntdlib::testing::sar_files;

// The offsets of the commands that the tests edit.
constexpr uint32_t kVolume = 15;
constexpr uint32_t kKey60 = 17;
constexpr uint32_t kWait48 = 20;
constexpr uint32_t kCall = 22;
constexpr uint32_t kVariableVolume = 26;
constexpr uint32_t kTrack1 = 44;
constexpr uint32_t kSub = 51;
constexpr uint32_t kEnd = 57;

// The bytecode. Each command's offset comes first in its comment. The Switch's sequences give arguments in
// little-endian order.
const std::vector<uint8_t> kBytecode = {
    0xFE, 0x03, 0x00,                   // 0: allocate tracks 0 and 1
    0x88, 0x01, 44,   0x00, 0x00,       // 3: open track 1 at 44
    0xB0, 48,                           // 8: timebase 48
    0xE1, 120,  0x00,                   // 10: tempo 120
    0xC7, 0x00,                         // 13: note wait off
    0xC1, 100,                          // 15: volume 100
    60,   100,  24,                     // 17: key 60 for 24 ticks, at tick 0
    0x80, 48,                           // 20: wait 48 ticks
    0x8A, 51,   0x00, 0x00,             // 22: call 51, at tick 48
    0xA1, 0xC1, 0x03,                   // 26: volume from variable 3, at tick 72
    62,   100,  24,                     // 29: key 62 for 24 ticks, at tick 72
    0xA0, 0x80, 12,   0x00, 24,   0x00, // 32: wait from 12 to 24 ticks at random
    64,   100,  12,                     // 38: key 64 for 12 ticks
    0x80, 24,                           // 41: wait 24 ticks
    0xFF,                               // 43: the end
    0xC7, 0x00,                         // 44: track 1: note wait off
    0xC0, 64,                           // 46: pan 64, the centre
    0x80, 96,                           // 48: wait 96 ticks
    0xFF,                               // 50: the end
    67,   100,  6,                      // 51: the subroutine: key 67 for 6 ticks
    0x80, 24,                           // 54: wait 24 ticks
    0xFD,                               // 56: return
};

// A loop of three passes. Each pass compares variable 3 with 1 and waits 12 ticks if they're equal. The variables start
// at -1. The comparison therefore fails. Each pass then sets the volume and waits 12 ticks. A note follows the loop.
const std::vector<uint8_t> kPollBytecode = {
    0xB0, 48,                  // 0: timebase 48
    0xE1, 120,  0x00,          // 2: tempo 120
    0xD4, 3,                   // 5: a loop of three passes
    0xF0, 0x90, 3,    1, 0x00, // 7: compare variable 3 with 1
    0xA2, 0x80, 12,            // 12: if the comparison held, wait 12 ticks
    0xC1, 100,                 // 15: volume 100
    0x80, 12,                  // 17: wait 12 ticks
    0xFC,                      // 19: the loop's end
    60,   100,  24,            // 20: key 60 for 24 ticks
    0xFF,                      // 23: the end
};

// A loop of two passes. Each pass compares variable 3 with -1, its starting value, and waits 12 ticks. It then sets the
// volume, waits 12 ticks more and plays a note.
const std::vector<uint8_t> kHeldBytecode = {
    0xB0, 48,                     // 0: timebase 48
    0xE1, 120,  0x00,             // 2: tempo 120
    0xD4, 2,                      // 5: a loop of two passes
    0xF0, 0x90, 3,    0xFF, 0xFF, // 7: compare variable 3 with -1
    0xA2, 0x80, 12,               // 12: if the comparison held, wait 12 ticks
    0xC1, 100,                    // 15: volume 100
    0x80, 12,                     // 17: wait 12 ticks
    60,   100,  12,               // 19: key 60 for 12 ticks
    0xFC,                         // 22: the loop's end
    0xFF,                         // 23: the end
};

// The archive, with `bytecode` and its labels. Its bank plays a looped wave for every key.
std::vector<uint8_t> TestArchive(const std::vector<uint8_t>& bytecode = kBytecode,
                                 const std::vector<std::pair<std::string, uint32_t>>& labels = {
                                     {"Song", 0}, {"Track1", kTrack1}, {"Sub", kSub}})
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

    sar_files::SoundSpec song;
    song.name = "SEQ_SONG";
    song.volume = 127;
    song.banks = {0};
    song.allocate_track_flags = 3;

    sar_files::ArchiveSpec spec;
    spec.sounds = {song};
    spec.banks = {{"BANK_TEST", 1}};
    spec.wave_archives = {{"WARC_TEST", 2}};
    spec.players = {{"PLAYER_TEST", 1}};
    spec.files = {{sar_files::Sequence(false, bytecode, labels), "", false},
                  {sar_files::Bank(false, bank), "", false},
                  {sar_files::WaveArchive(false, {sar_files::Wave(false, wave)}), "", false}};

    return sar_files::Archive(false, spec);
}

// The sound as ntdlib's performer plays it from the archive that `file` builds.
Sequence Played(const File& file)
{
    const std::shared_ptr<const sar::SoundArchive> archive =
        hokuto::song::LoadArchive(hokuto::format::ExportArchive(file), "test.bfsar");
    return hokuto::song::ReadSequence(*archive, 0);
}

// The start, length and key of each note of track 0 of `sequence`, up to `count` notes.
std::vector<std::vector<uint32_t>> Notes(const Sequence& sequence, std::size_t count = 3)
{
    std::vector<std::vector<uint32_t>> notes;
    for (const Note& note : sequence.tracks.at(0).notes)
    {
        if (notes.size() < count)
        {
            notes.push_back({note.start, note.length, note.key});
        }
    }

    return notes;
}

// The first three notes that the archive plays before an edit.
const std::vector<std::vector<uint32_t>> kNotes = {{0, 24, 60}, {48, 6, 67}, {72, 24, 62}};

// The place of the command at `command` in stretch `stretch` of track 0.
CommandPlace PlaceOf(const Sequence& sequence, uint32_t command, std::size_t stretch)
{
    return CommandPlace{command, sequence.tracks.at(0).stretches.at(stretch)};
}

// Whether `edit` refuses with a message for the user.
template <typename Edit>
bool Refuses(Edit edit)
{
    try
    {
        edit();
    }
    catch (const std::invalid_argument&)
    {
        return true;
    }

    return false;
}

// The rows follow the track through the call and back. A random wait leaves the ticks unknown until a note's start.
void TestRows()
{
    const File file = hokuto::format::ImportSwitchArchive(TestArchive());
    const Sequence song = Played(file);
    const Track& track = song.tracks.at(0);

    const std::vector<CommandRow> rows = CommandRows(file, 0, track.stretches, track.notes, std::nullopt);
    const std::vector<CommandRow> first = CommandRows(file, 0, track.stretches, track.notes, 48u);
    const std::vector<CommandRow> silent = CommandRows(file, 0, song.tracks.at(1).stretches, {}, std::nullopt);

    const uint32_t key64 = track.notes.at(3).start;
    XSFLIB_CHECK(track.notes.at(3).key == 64 && key64 >= 84 && key64 <= 96);
    const std::vector<CommandRow> kRows = {
        {0, 0, 0},   {3, 0, 0},   {8, 0, 0},   {10, 0, 0},     {13, 0, 0},     {15, 0, 0},
        {17, 0, 0},  {20, 0, 0},  {22, 0, 48}, {51, 1, 48},    {54, 1, 48},    {56, 1, 72},
        {26, 2, 72}, {29, 2, 72}, {32, 2, 72}, {38, 2, key64}, {41, 2, key64}, {43, 2, key64 + 24},
    };
    XSFLIB_CHECK(rows == kRows);
    XSFLIB_CHECK(first == std::vector<CommandRow>(kRows.begin(), kRows.begin() + 9));
    XSFLIB_CHECK((silent == std::vector<CommandRow>{{44, 0, 0}, {46, 0, 0}, {48, 0, 0}, {50, 0, 96}}));
    XSFLIB_CHECK(song.tracks.size() == 2 && song.tracks[1].number == 1 && song.tracks[1].notes.empty());
}

// After a wait that runs only if the last comparison held, the ticks count back from the next tick that's known: the
// start of the next pass of the loop, or the note's start after the last pass. With an end at tick 30, the rows stop at
// the last pass's end, at tick 36.
void TestRowsAfterUnknownWait()
{
    const File file = hokuto::format::ImportSwitchArchive(TestArchive(kPollBytecode, {{"Song", 0}}));
    const Sequence song = Played(file);
    const Track& track = song.tracks.at(0);

    const std::vector<CommandRow> rows = CommandRows(file, 0, track.stretches, track.notes, std::nullopt);
    const std::vector<CommandRow> first = CommandRows(file, 0, track.stretches, track.notes, 30u);

    const std::vector<CommandRow> kRows = {
        {0, 0, 0},   {2, 0, 0},   {5, 0, 0},   {7, 0, 0},   {12, 0, 0},  {15, 0, 0},  {17, 0, 0},
        {19, 0, 12}, {7, 1, 12},  {12, 1, 12}, {15, 1, 12}, {17, 1, 12}, {19, 1, 24}, {7, 2, 24},
        {12, 2, 24}, {15, 2, 24}, {17, 2, 24}, {19, 2, 36}, {20, 2, 36}, {23, 2, 60},
    };
    XSFLIB_CHECK(track.stretches.size() == 3);
    XSFLIB_CHECK(rows == kRows);
    XSFLIB_CHECK(first == std::vector<CommandRow>(kRows.begin(), kRows.begin() + 17));
}

// Where the wait runs, the ticks after it count back from the note's start. With an end at tick 18, the note starts
// after the end. The ticks before it still count back from its start.
void TestRowsAfterHeldWait()
{
    const File file = hokuto::format::ImportSwitchArchive(TestArchive(kHeldBytecode, {{"Song", 0}}));
    const Sequence song = Played(file);
    const Track& track = song.tracks.at(0);

    const std::vector<CommandRow> rows = CommandRows(file, 0, track.stretches, track.notes, std::nullopt);
    const std::vector<CommandRow> first = CommandRows(file, 0, track.stretches, track.notes, 18u);

    const std::vector<CommandRow> kRows = {
        {0, 0, 0},   {2, 0, 0},  {5, 0, 0},   {7, 0, 0},   {12, 0, 0},  {15, 0, 12}, {17, 0, 12}, {19, 0, 24},
        {22, 0, 36}, {7, 1, 36}, {12, 1, 36}, {15, 1, 48}, {17, 1, 48}, {19, 1, 60}, {22, 1, 72}, {23, 1, 72},
    };
    XSFLIB_CHECK(rows == kRows);
    XSFLIB_CHECK(first == std::vector<CommandRow>(kRows.begin(), kRows.begin() + 7));
}

// The song draws a random number for its wait before key 64. The loop that compares a variable draws none.
void TestDrawsRandom()
{
    const File file = hokuto::format::ImportSwitchArchive(TestArchive());
    const File poll = hokuto::format::ImportSwitchArchive(TestArchive(kPollBytecode, {{"Song", 0}}));

    XSFLIB_CHECK(DrawsRandom(file, 0, Played(file)));
    XSFLIB_CHECK(!DrawsRandom(poll, 0, Played(poll)));
}

// The labels lead to their commands, and an offset to its item. The ranges follow the console's bytes. A tempo and a
// timebase start at 1: 0 stops the player's clock. Except on the Wii before March 2008, a tempo ends at 1023. The
// player holds a tempo to that value.
void TestLabelsAndRanges()
{
    const File file = hokuto::format::ImportSwitchArchive(TestArchive());
    Command volume = CommandAt(file, 0, kVolume);
    const Command& wait = CommandAt(file, 0, kWait48);
    Command pan = NewCommand(Kind::kPan, 48, 0);
    pan.value.value = kOne / 2;
    const Command tempo = NewCommand(Kind::kTempo, 48, 0);

    const auto range = [](const Command& command, Source source, bool time = false)
    {
        return ArgumentRange(Console{.flavor = sar::Flavor::kNx}, command, source, time);
    };

    // The ranges of Wii Sports' build and of a build dated 20080610. No known build has that date. Such a build plays
    // as the builds from March 2008.
    const std::optional<ValueRange> early_wii =
        ArgumentRange(Console{.wii = ntdlib::SndBuild{.date = 20070608}}, tempo, Source::kValue);
    const std::optional<ValueRange> later_wii =
        ArgumentRange(Console{.wii = ntdlib::SndBuild{.date = 20080610}}, tempo, Source::kValue);

    XSFLIB_CHECK((LabelsOf(file, 0) == std::vector<LabelPlace>{{"Song", 0}, {"Track1", kTrack1}, {"Sub", kSub}}));
    XSFLIB_CHECK(ItemOf(file, 0, kSub) == 19 && ItemOf(file, 0, kEnd) == 22);
    XSFLIB_CHECK(Refuses([&] { ItemOf(file, 0, kSub + 1); }) && Refuses([&] { CommandAt(file, 0, kEnd); }));
    XSFLIB_CHECK(volume.kind == Kind::kVolume && volume.value.value == 100 * kOne);
    XSFLIB_CHECK((range(volume, Source::kValue) == ValueRange{0, 255 * kOne}));
    XSFLIB_CHECK((range(volume, Source::kRandom) == ValueRange{-32768 * kOne, 32767 * kOne}));
    XSFLIB_CHECK((range(volume, Source::kVariable) == ValueRange{0, 255}));
    XSFLIB_CHECK((range(volume, Source::kValue, true) == ValueRange{0, 65535 * kOne}));
    XSFLIB_CHECK((range(wait, Source::kValue) == ValueRange{0, ((int64_t{1} << 32) - 1) * kOne}));
    XSFLIB_CHECK(!range(wait, Source::kValue, true));
    XSFLIB_CHECK((range(pan, Source::kValue) == ValueRange{-64 * kOne, 191 * kOne}));
    XSFLIB_CHECK(
        (range(NewCommand(Kind::kTranspose, 48, 0), Source::kValue) == ValueRange{-12800 * kOne, 12700 * kOne}));
    XSFLIB_CHECK((range(NewCommand(Kind::kAllocateTracks, 48, 0), Source::kValue) == ValueRange{0, 65535 * kOne}));
    XSFLIB_CHECK(!range(NewCommand(Kind::kAllocateTracks, 48, 0), Source::kRandom));
    XSFLIB_CHECK(!range(NewCommand(Kind::kJump, 48, 0), Source::kValue));
    XSFLIB_CHECK(Nearest(Console{.flavor = sar::Flavor::kNx}, pan).value.value == kOne);
    XSFLIB_CHECK((range(tempo, Source::kValue) == ValueRange{kOne, 1023 * kOne}) &&
                 (range(tempo, Source::kRandom) == ValueRange{kOne, 1023 * kOne}));
    XSFLIB_CHECK((range(NewCommand(Kind::kTimebase, 48, 0), Source::kValue) == ValueRange{kOne, 255 * kOne}));
    XSFLIB_CHECK(early_wii && early_wii->lowest == kOne && early_wii->highest > 1023 * kOne);
    XSFLIB_CHECK((later_wii == ValueRange{kOne, 1023 * kOne}));
}

// A new command starts with the values that the console gives a track.
void TestNewCommand()
{
    const Command note = NewCommand(Kind::kNote, 96, 0);
    const Command jump = NewCommand(Kind::kJump, 48, 7);

    XSFLIB_CHECK(note.key == 60 && note.velocity == 100 * kOne && note.value.value == 96 * kOne);
    XSFLIB_CHECK(jump.target == 7 && NewCommand(Kind::kOpenTrack, 48, 9).index == 1);
    XSFLIB_CHECK(NewCommand(Kind::kVolume, 48, 0).value.value == 127 * kOne);
    XSFLIB_CHECK(NewCommand(Kind::kTempo, 48, 0).value.value == 120 * kOne);
    XSFLIB_CHECK(NewCommand(Kind::kWait, 24, 0).value.value == 24 * kOne);
}

// A setting takes a new value, and a value from a variable becomes a value. A note's key, velocity and length change as
// the Key Editor changes them. A call goes elsewhere.
void TestChange()
{
    const File file = hokuto::format::ImportSwitchArchive(TestArchive());
    const Sequence song = Played(file);
    Command quieter = CommandAt(file, 0, kVolume);
    quieter.value.value = 90 * kOne;
    Command fixed = CommandAt(file, 0, kVariableVolume);
    fixed.value = Argument{Source::kValue, 110 * kOne, 0};
    Command note = CommandAt(file, 0, kKey60);
    note.key = 61;
    note.velocity = 90 * kOne;
    Command longer = CommandAt(file, 0, kKey60);
    longer.value.value = 36 * kOne;
    Command call = CommandAt(file, 0, kCall);
    call.target = ItemOf(file, 0, kTrack1);

    const Edited quiet = ChangeCommand(file, 0, PlaceOf(song, kVolume, 0), quieter);
    const Edited value = ChangeCommand(file, 0, PlaceOf(song, kVariableVolume, 2), fixed);
    const Edited key = ChangeCommand(file, 0, PlaceOf(song, kKey60, 0), note);
    const Edited length = ChangeCommand(file, 0, PlaceOf(song, kKey60, 0), longer);
    const Edited elsewhere = ChangeCommand(file, 0, PlaceOf(song, kCall, 0), call);

    const Sequence quiet_song = Played(quiet.file);
    XSFLIB_CHECK(Notes(quiet_song) == kNotes && quiet_song.tracks[0].volume.at(0) == (Setting{0, 90}));
    const Sequence value_song = Played(value.file);
    XSFLIB_CHECK(Notes(value_song) == kNotes && value_song.tracks[0].volume.at(1) == (Setting{72, 110}));
    XSFLIB_CHECK(CommandAt(value.file, 0, kVariableVolume + 2).key == 62);
    XSFLIB_CHECK(value.file.sequences[0]->labels == file.sequences[0]->labels);
    const Sequence key_song = Played(key.file);
    XSFLIB_CHECK((Notes(key_song) == std::vector<std::vector<uint32_t>>{{0, 24, 61}, {48, 6, 67}, {72, 24, 62}}));
    SEQLIB_REQUIRE(!key_song.tracks.empty() && !key_song.tracks[0].notes.empty());
    XSFLIB_CHECK(key_song.tracks[0].notes[0].velocity == 90);
    XSFLIB_CHECK(
        (Notes(Played(length.file)) == std::vector<std::vector<uint32_t>>{{0, 36, 60}, {48, 6, 67}, {72, 24, 62}}));
    XSFLIB_CHECK((Notes(Played(elsewhere.file)) == std::vector<std::vector<uint32_t>>{{0, 24, 60}}));
}

// Several settings change in one edit, and each keeps its item. The value from a variable becomes a value, and the
// commands after it move. A note, a change of a command's kind and a value past the console's bytes are refused.
void TestChangeSeveral()
{
    const File file = hokuto::format::ImportSwitchArchive(TestArchive());
    const Sequence song = Played(file);
    Command quieter = CommandAt(file, 0, kVolume);
    quieter.value.value = 90 * kOne;
    Command fixed = CommandAt(file, 0, kVariableVolume);
    fixed.value = Argument{Source::kValue, 110 * kOne, 0};
    Command note = CommandAt(file, 0, kKey60);
    note.key = 61;
    Command pan = quieter;
    pan.kind = Kind::kPan;
    Command loud = quieter;
    loud.value.value = 300 * kOne;
    const std::vector<CommandPlace> places = {PlaceOf(song, kVolume, 0), PlaceOf(song, kVariableVolume, 2)};

    const Edited changed = ChangeCommands(file, 0, places, std::vector<Command>{quieter, fixed});

    const Sequence played = Played(changed.file);
    XSFLIB_CHECK(Notes(played) == kNotes);
    XSFLIB_CHECK(played.tracks[0].volume.at(0) == (Setting{0, 90}) &&
                 played.tracks[0].volume.at(1) == (Setting{72, 110}));
    XSFLIB_CHECK(changed.items == ChangeCommand(file, 0, places[0], quieter).items);
    XSFLIB_CHECK(CommandAt(changed.file, 0, kVariableVolume + 2).key == 62);

    const std::vector<CommandPlace> volume = {places[0]};
    const std::vector<CommandPlace> key = {PlaceOf(song, kKey60, 0)};
    XSFLIB_CHECK(Refuses([&] { ChangeCommands(file, 0, key, std::vector<Command>{note}); }));
    XSFLIB_CHECK(Refuses([&] { ChangeCommands(file, 0, volume, std::vector<Command>{pan}); }));
    XSFLIB_CHECK(Refuses([&] { ChangeCommands(file, 0, volume, std::vector<Command>{loud}); }));
}

// A setting moves to another tick and takes a new value in one edit. It keeps its item, and the notes play as before. A
// move of a note is refused.
void TestMoveAndChange()
{
    const File file = hokuto::format::ImportSwitchArchive(TestArchive());
    const Sequence song = Played(file);
    Command quieter = CommandAt(file, 0, kVolume);
    quieter.value.value = 90 * kOne;
    const std::vector<SettingMove> later = {{kVolume, song.tracks[0].stretches[0], 24}};
    const std::vector<SettingMove> note = {{kKey60, song.tracks[0].stretches[0], 24}};
    const std::vector<CommandPlace> volume = {PlaceOf(song, kVolume, 0)};

    const Edited moved = MoveAndChangeCommands(file, 0, later, volume, std::vector<Command>{quieter});

    const Sequence played = Played(moved.file);
    const std::optional<uint32_t> item = moved.items.at(ItemOf(file, 0, kVolume));
    SEQLIB_REQUIRE(item && !played.tracks[0].volume.empty());
    XSFLIB_CHECK(Notes(played) == kNotes && played.tracks[0].volume.at(0) == (Setting{24, 90}));
    XSFLIB_CHECK(moved.file.sequences[0]->items.at(*item) == Nearest(Console{.flavor = sar::Flavor::kNx}, quieter));
    XSFLIB_CHECK(Refuses([&] { MoveAndChangeCommands(file, 0, note, volume, std::vector<Command>{quieter}); }));
}

// The condition, and the time over which a setting moves, change the command's bytes. The commands after it move.
void TestConditionAndTime()
{
    const File file = hokuto::format::ImportSwitchArchive(TestArchive());
    const Sequence song = Played(file);
    Command conditional = CommandAt(file, 0, kVolume);
    conditional.conditional = true;
    Command ramp = CommandAt(file, 0, kVolume);
    ramp.time = Argument{Source::kValue, 24 * kOne, 0};

    const Edited condition = ChangeCommand(file, 0, PlaceOf(song, kVolume, 0), conditional);
    const Edited timed = ChangeCommand(file, 0, PlaceOf(song, kVolume, 0), ramp);

    XSFLIB_CHECK(CommandAt(condition.file, 0, kVolume).conditional &&
                 CommandAt(condition.file, 0, kKey60 + 1).key == 60);
    XSFLIB_CHECK(Notes(Played(condition.file)) == kNotes);
    XSFLIB_CHECK(CommandAt(timed.file, 0, kVolume).time == ramp.time && CommandAt(timed.file, 0, kKey60 + 3).key == 60);
    XSFLIB_CHECK(Notes(Played(timed.file)) == kNotes);
}

// A value past the console's bytes, a time on a wait, a command of another kind, and a target past the end are refused.
// In a sequence with bytes that no command reads, a change keeps the command's size.
void TestChangeRefusals()
{
    const File file = hokuto::format::ImportSwitchArchive(TestArchive());
    const Sequence song = Played(file);
    Command loud = CommandAt(file, 0, kVolume);
    loud.value.value = 300 * kOne;
    Command timed = CommandAt(file, 0, kWait48);
    timed.time = Argument{Source::kValue, 24 * kOne, 0};
    Command other = CommandAt(file, 0, kVolume);
    other.kind = Kind::kPan;
    Command far = CommandAt(file, 0, kCall);
    far.target = 23;
    File fixed = file;
    ntdlib::commands::Sequence with_data = *file.sequences[0];
    with_data.items.push_back(Command{.kind = Kind::kData, .bytes = {0x00}});
    fixed.sequences[0] = std::move(with_data);
    Command quieter = CommandAt(file, 0, kVolume);
    quieter.value.value = 90 * kOne;
    Command longer = CommandAt(file, 0, kWait48);
    longer.value.value = 200 * kOne;

    XSFLIB_CHECK(Refuses([&] { ChangeCommand(file, 0, PlaceOf(song, kVolume, 0), loud); }));
    XSFLIB_CHECK(Refuses([&] { ChangeCommand(file, 0, PlaceOf(song, kWait48, 0), timed); }));
    XSFLIB_CHECK(Refuses([&] { ChangeCommand(file, 0, PlaceOf(song, kVolume, 0), other); }));
    XSFLIB_CHECK(Refuses([&] { ChangeCommand(file, 0, PlaceOf(song, kCall, 0), far); }));
    XSFLIB_CHECK(CommandAt(ChangeCommand(fixed, 0, PlaceOf(song, kVolume, 0), quieter).file, 0, kVolume) == quieter);
    XSFLIB_CHECK(Refuses([&] { ChangeCommand(fixed, 0, PlaceOf(song, kWait48, 0), longer); }));
    XSFLIB_CHECK(Refuses([&] { InsertCommand(fixed, 0, PlaceOf(song, kVolume, 0), false, quieter); }));
    XSFLIB_CHECK(Refuses([&] { RemoveCommands(fixed, 0, std::vector<CommandPlace>{PlaceOf(song, kVolume, 0)}); }));
}

// A new command goes before another. Where the track entered at the other command, code that enters there runs the new
// command too. Elsewhere, only the command before runs on into it.
void TestInsert()
{
    const File file = hokuto::format::ImportSwitchArchive(TestArchive());
    const Sequence song = Played(file);
    Command volume = NewCommand(Kind::kVolume, 48, 0);
    volume.value.value = 50 * kOne;
    const Command wait = NewCommand(Kind::kWait, 24, 0);
    const Command lead = NewCommand(Kind::kWait, 12, 0);

    const Edited louder = InsertCommand(file, 0, PlaceOf(song, kKey60, 0), false, volume);
    const Edited entered = InsertCommand(file, 0, PlaceOf(song, kSub, 1), true, wait);
    const Edited passed = InsertCommand(file, 0, PlaceOf(song, kSub, 1), false, wait);
    const Edited started = InsertCommand(file, 0, PlaceOf(song, 0, 0), true, lead);

    const Sequence louder_song = Played(louder.file);
    XSFLIB_CHECK(Notes(louder_song) == kNotes && louder_song.tracks[0].volume.at(0) == (Setting{0, 50}));
    XSFLIB_CHECK(louder.added == std::vector<uint32_t>{6} && louder.items[5] == 5u && louder.items[6] == 7u);
    const std::vector<ntdlib::commands::Label> kLabels = {{"Song", 0}, {"Track1", 16}, {"Sub", 20}};
    XSFLIB_CHECK(louder.file.sequences[0]->labels == kLabels);
    XSFLIB_CHECK(
        (Notes(Played(entered.file)) == std::vector<std::vector<uint32_t>>{{0, 24, 60}, {72, 6, 67}, {96, 24, 62}}));
    XSFLIB_CHECK(entered.file.sequences[0]->labels.at(2).item == 19);
    XSFLIB_CHECK(Notes(Played(passed.file)) == kNotes && passed.file.sequences[0]->labels.at(2).item == 20);
    XSFLIB_CHECK(
        (Notes(Played(started.file)) == std::vector<std::vector<uint32_t>>{{12, 24, 60}, {60, 6, 67}, {84, 24, 62}}));
}

// The 3DS's player skips the first LFO's curve and the send to bus C. Their arguments have no range there, and the List
// Editor refuses them: a new command, and a send that changes to bus C. A command that the player runs goes in.
void Test3ds()
{
    const std::vector<uint8_t> kCtrBytecode = {
        0xB0, 48,        // 0: timebase 48
        0xE1, 0x00, 120, // 2: tempo 120, in the 3DS's big-endian order
        0xD9, 40,        // 5: send A 40
        60,   100,  24,  // 7: key 60 for 24 ticks
        0xFF,            // 10: the end
    };
    sar_files::ArchiveSpec spec;
    spec.sounds = {{.name = "SEQ_CTR", .file = 0, .banks = {0}}};
    spec.banks = {{"BANK_CTR", 1}};
    spec.wave_archives = {{"WARC_CTR", 2}};
    spec.players = {{"PLAYER_CTR", 1}};
    spec.files = {{sar_files::Sequence(sar::Flavor::kCtr, kCtrBytecode, {{"Ctr", 0}})},
                  {sar_files::Bank(sar::Flavor::kCtr, sar_files::OneRegionBank())},
                  {sar_files::WaveArchive(sar::Flavor::kCtr,
                                          {sar_files::Wave(sar::Flavor::kCtr, sar_files::ConstantWave(false))})}};
    const File file = hokuto::format::ImportArchive(sar_files::Archive(sar::Flavor::kCtr, spec), sar::Flavor::kCtr);
    const Sequence song = Played(file);
    Command send_c = NewCommand(Kind::kSend, 48, 0);
    send_c.index = 2;
    Command volume = NewCommand(Kind::kVolume, 48, 0);
    volume.value.value = 50 * kOne;

    XSFLIB_CHECK(
        !ArgumentRange(Console{.flavor = sar::Flavor::kCtr}, NewCommand(Kind::kLfoCurve, 48, 0), Source::kValue));
    XSFLIB_CHECK(!ArgumentRange(Console{.flavor = sar::Flavor::kCtr}, send_c, Source::kValue));
    XSFLIB_CHECK(ArgumentRange(Console{.flavor = sar::Flavor::kNx}, send_c, Source::kValue).has_value());
    XSFLIB_CHECK(
        Refuses([&] { InsertCommand(file, 0, PlaceOf(song, 7, 0), false, NewCommand(Kind::kLfoCurve, 48, 0)); }));
    XSFLIB_CHECK(Refuses([&] { InsertCommand(file, 0, PlaceOf(song, 7, 0), false, send_c); }));

    Command moved = CommandAt(file, 0, 5);
    moved.index = 2;
    XSFLIB_CHECK(Refuses([&] { ChangeCommand(file, 0, PlaceOf(song, 5, 0), moved); }));

    const Edited quieter = InsertCommand(file, 0, PlaceOf(song, 7, 0), false, volume);

    XSFLIB_CHECK(Played(quieter.file).tracks.at(0).volume.at(0) == (Setting{0, 50}));
}

// A removed command takes the code that entered at it to the command after it. A note goes as the Key Editor removes
// it.
void TestRemove()
{
    const File file = hokuto::format::ImportSwitchArchive(TestArchive());
    const Sequence song = Played(file);
    const std::vector<CommandPlace> wait = {PlaceOf(song, kWait48, 0)};
    const std::vector<CommandPlace> opening = {CommandPlace{kTrack1, song.tracks.at(1).stretches.at(0)}};
    const std::vector<CommandPlace> both = {PlaceOf(song, kSub, 1), PlaceOf(song, kVolume, 0)};

    const Edited earlier = RemoveCommands(file, 0, wait);
    const Edited moved = RemoveCommands(file, 0, opening);
    const Edited fewer = RemoveCommands(file, 0, both);

    XSFLIB_CHECK(
        (Notes(Played(earlier.file)) == std::vector<std::vector<uint32_t>>{{0, 24, 60}, {0, 6, 67}, {24, 24, 62}}));
    XSFLIB_CHECK(!earlier.items[7] && earlier.items[8] == 7u && earlier.items[21] == 20u);
    XSFLIB_CHECK(CommandAt(moved.file, 0, kTrack1).kind == Kind::kPan);
    XSFLIB_CHECK(moved.file.sequences[0]->labels.at(1).item == 15 && moved.file.sequences[0]->items.at(1).target == 15);
    const Sequence fewer_song = Played(fewer.file);
    XSFLIB_CHECK((Notes(fewer_song, 2) == std::vector<std::vector<uint32_t>>{{0, 24, 60}, {72, 24, 62}}));
    XSFLIB_CHECK(fewer.file.sequences[0]->items.size() == 20 && fewer_song.tracks[0].volume.at(0).tick == 72);
    XSFLIB_CHECK(fewer.file.sequences[0]->labels.at(2).item == 18 && !fewer.items[5] && !fewer.items[19]);
}

} // namespace

int main()
{
    TestRows();
    TestRowsAfterUnknownWait();
    TestRowsAfterHeldWait();
    TestDrawsRandom();
    TestLabelsAndRanges();
    TestNewCommand();
    TestChange();
    TestChangeSeveral();
    TestMoveAndChange();
    TestConditionAndTime();
    TestChangeRefusals();
    TestInsert();
    Test3ds();
    TestRemove();

    return xsflib::testing::Summary("commands");
}
