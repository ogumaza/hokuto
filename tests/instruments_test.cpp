// SPDX-License-Identifier: MIT

// Tests the edit of a track's instrument (src/edit/instruments.h). A small Switch archive holds two banks with programs
// 0 to 2 each, and three sequence files. In the first, track 0 selects program 1 and plays two notes. Track 1 plays a
// note with program 0, the program that a track starts with, then selects program 2 for a second note. Track 2 selects
// bank slot 1 and program 0 for its note. Track 3 selects program 1 for a note, then bank slot 1 for a second note. In
// the second file, a note plays program 0 before a program change to a random program. In the third, a note plays
// program 1 before a program change that runs only if the last comparison held. Each edit goes through an export of the
// archive. ntdlib's performer then plays the edited archive, and the instruments of its notes have to show the edit.

#include "edit/instruments.h"

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
#include "ntdlib/testing/sar_files.h"
#include "xsflib/testing/check.h"

#include "edit/replace.h"
#include "format/archive.h"
#include "format/file.h"
#include "song/archive.h"
#include "song/sequence.h"

using hokuto::edit::Edited;
using hokuto::edit::EditInstrument;
using hokuto::edit::Program;
using hokuto::format::File;
using hokuto::song::Sequence;
using hokuto::song::Track;
using ntdlib::commands::Kind;
using ntdlib::commands::kOne;

namespace
{

namespace sar = ntdlib::sar;
namespace sar_files = ntdlib::testing::sar_files;

// The archive's sounds.
constexpr uint32_t kSong = 0;
constexpr uint32_t kRandom = 1;
constexpr uint32_t kChoice = 2;

// The first file's bytecode. Each command's offset comes first in its comment. The Switch's sequences give arguments in
// little-endian order. Each note waits for its length.
const std::vector<uint8_t> kSongBytecode = {
    0x88, 1,   29,   0, 0, // 0: open track 1 at 29
    0x88, 2,   38,   0, 0, // 5: open track 2 at 38
    0x88, 3,   46,   0, 0, // 10: open track 3 at 46
    0xB0, 48,              // 15: timebase 48
    0xE1, 120, 0x00,       // 17: tempo 120
    0x81, 1,               // 20: program 1
    60,   100, 24,         // 22: key 60 for 24 ticks
    62,   100, 24,         // 25: key 62 for 24 ticks
    0xFF,                  // 28: the end
    64,   100, 24,         // 29: track 1: key 64 for 24 ticks, with program 0
    0x81, 2,               // 32: program 2
    65,   100, 24,         // 34: key 65 for 24 ticks
    0xFF,                  // 37: the end
    0xB6, 1,               // 38: track 2: bank slot 1
    0x81, 0,               // 40: program 0
    67,   100, 24,         // 42: key 67 for 24 ticks
    0xFF,                  // 45: the end
    0x81, 1,               // 46: track 3: program 1
    69,   100, 24,         // 48: key 69 for 24 ticks
    0xB6, 1,               // 51: bank slot 1
    71,   100, 24,         // 53: key 71 for 24 ticks
    0xFF,                  // 56: the end
};

// The items of the first file's commands that the tests look at.
constexpr uint32_t kTrack0Program = 5;
constexpr uint32_t kTrack1First = 9;
constexpr uint32_t kTrack2Slot = 13;
constexpr uint32_t kTrack2Program = 14;

// The second file's bytecode.
const std::vector<uint8_t> kRandomBytecode = {
    0xB0, 48,                  // 0: timebase 48
    0xE1, 120,  0x00,          // 2: tempo 120
    60,   100,  24,            // 5: key 60 for 24 ticks, with program 0
    0xA0, 0x81, 0,    0, 2, 0, // 8: program from 0 to 2 at random
    62,   100,  24,            // 14: key 62 for 24 ticks
    0xFF,                      // 17: the end
};

// The third file's bytecode.
const std::vector<uint8_t> kChoiceBytecode = {
    0xB0, 48,         // 0: timebase 48
    0xE1, 120,  0x00, // 2: tempo 120
    0x81, 1,          // 5: program 1
    60,   100,  24,   // 7: key 60 for 24 ticks
    0xA2, 0x81, 1,    // 10: program 1, if the last comparison held
    62,   100,  24,   // 13: key 62 for 24 ticks
    0xFF,             // 16: the end
};

// The archive. Each bank plays a looped wave for every key of each of its three programs.
std::vector<uint8_t> TestArchive()
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
    bank.instruments = {keys, keys, keys};

    sar_files::SoundSpec song;
    song.name = "SEQ_SONG";
    song.volume = 127;
    song.banks = {0, 1};
    song.allocate_track_flags = 0xF;
    sar_files::SoundSpec random = song;
    random.name = "SEQ_RANDOM";
    random.file = 1;
    random.allocate_track_flags = 1;
    sar_files::SoundSpec choice = random;
    choice.name = "SEQ_CHOICE";
    choice.file = 2;

    sar_files::ArchiveSpec spec;
    spec.sounds = {song, random, choice};
    spec.banks = {{"BANK_A", 3}, {"BANK_B", 4}};
    spec.wave_archives = {{"WARC_TEST", 5}};
    spec.players = {{"PLAYER_TEST", 1}};
    spec.files = {{sar_files::Sequence(false, kSongBytecode, {{"Song", 0}}), "", false},
                  {sar_files::Sequence(false, kRandomBytecode, {{"Random", 0}}), "", false},
                  {sar_files::Sequence(false, kChoiceBytecode, {{"Choice", 0}}), "", false},
                  {sar_files::Bank(false, bank), "", false},
                  {sar_files::Bank(false, bank), "", false},
                  {sar_files::WaveArchive(false, {sar_files::Wave(false, wave)}), "", false}};

    return sar_files::Archive(false, spec);
}

// ntdlib's performer plays sound `sound` of the archive that `file` builds.
Sequence Played(const File& file, uint32_t sound)
{
    const std::shared_ptr<const sar::SoundArchive> archive =
        hokuto::song::LoadArchive(hokuto::format::ExportArchive(file), "test.bfsar");
    return hokuto::song::ReadSequence(*archive, sound);
}

// The instrument of each note of `track`.
std::vector<Program> Instruments(const Track& track)
{
    std::vector<Program> instruments;
    for (const hokuto::song::Note& note : track.notes)
    {
        instruments.push_back({note.bank_slot, static_cast<int>(note.program)});
    }

    return instruments;
}

// The start and key of each note of `track`. The edits leave them as they are.
std::vector<std::vector<uint32_t>> Notes(const Track& track)
{
    std::vector<std::vector<uint32_t>> notes;
    for (const hokuto::song::Note& note : track.notes)
    {
        notes.push_back({note.start, note.key});
    }

    return notes;
}

// The edit of track `index` of sound `sound` of `file` from `from` to `to`.
Edited Edit(const File& file, uint32_t sound, std::size_t index, Program from, Program to)
{
    const Sequence played = Played(file, sound);
    const Track& track = played.tracks.at(index);
    return EditInstrument(file, sound, track.number, track.stretches, from, to);
}

// The message for the user with which the edit refuses, or nothing.
std::optional<std::string> Refusal(const File& file, uint32_t sound, std::size_t index, Program from, Program to)
{
    try
    {
        Edit(file, sound, index, from, to);
    }
    catch (const std::invalid_argument& error)
    {
        return error.what();
    }

    return std::nullopt;
}

// Whether the edit refuses with a message for the user.
bool Refuses(const File& file, uint32_t sound, std::size_t index, Program from, Program to)
{
    return Refusal(file, sound, index, from, to).has_value();
}

// The tracks of `after` other than track `index` play the instruments and notes of `before`.
bool OthersAsBefore(const Sequence& before, const Sequence& after, std::size_t index)
{
    bool same = before.tracks.size() == after.tracks.size();
    for (std::size_t i = 0; same && i < before.tracks.size(); i++)
    {
        same = i == index || (Instruments(before.tracks[i]) == Instruments(after.tracks[i]) &&
                              Notes(before.tracks[i]) == Notes(after.tracks[i]));
    }

    return same;
}

// The archive plays the instruments that its bytecode selects.
void TestArchivePlays()
{
    const File file = hokuto::format::ImportSwitchArchive(TestArchive());

    const Sequence song = Played(file, kSong);
    const Sequence random = Played(file, kRandom);
    const Sequence choice = Played(file, kChoice);

    SEQLIB_REQUIRE(song.tracks.size() == 4);
    XSFLIB_CHECK(Instruments(song.tracks[0]) == (std::vector<Program>{{0, 1}, {0, 1}}));
    XSFLIB_CHECK(Instruments(song.tracks[1]) == (std::vector<Program>{{0, 0}, {0, 2}}));
    XSFLIB_CHECK(Instruments(song.tracks[2]) == (std::vector<Program>{{1, 0}}));
    XSFLIB_CHECK(Instruments(song.tracks[3]) == (std::vector<Program>{{0, 1}, {1, 1}}));
    XSFLIB_CHECK(random.tracks.size() == 1 && random.tracks[0].notes.size() == 2);
    XSFLIB_CHECK(choice.tracks.size() == 1 && choice.tracks[0].notes.size() == 2);
}

// A program change that selects the program of the instrument's notes alone takes the new program in its item.
void TestProgram()
{
    const File file = hokuto::format::ImportSwitchArchive(TestArchive());
    const Sequence before = Played(file, kSong);

    const Edited edited = Edit(file, kSong, 0, {0, 1}, {0, 2});

    const Sequence after = Played(edited.file, kSong);
    XSFLIB_CHECK(Instruments(after.tracks.at(0)) == (std::vector<Program>{{0, 2}, {0, 2}}));
    XSFLIB_CHECK(Notes(after.tracks[0]) == Notes(before.tracks[0]) && OthersAsBefore(before, after, 0));
    XSFLIB_CHECK(edited.file.sequences[0]->items.size() == file.sequences[0]->items.size());
    XSFLIB_CHECK(edited.file.sequences[0]->items.at(kTrack0Program).value.value == 2 * kOne);
}

// Track 1's first note plays the program that the track starts with. A new program change at the track's start selects
// the new program. The note of program 2 keeps it. The items from the track's first command on move by one.
void TestStart()
{
    const File file = hokuto::format::ImportSwitchArchive(TestArchive());
    const Sequence before = Played(file, kSong);

    const Edited edited = Edit(file, kSong, 1, {0, 0}, {0, 1});

    const Sequence after = Played(edited.file, kSong);
    const auto& items = edited.file.sequences[0]->items;
    XSFLIB_CHECK(Instruments(after.tracks.at(1)) == (std::vector<Program>{{0, 1}, {0, 2}}));
    XSFLIB_CHECK(Notes(after.tracks[1]) == Notes(before.tracks[1]) && OthersAsBefore(before, after, 1));
    XSFLIB_CHECK(items.size() == file.sequences[0]->items.size() + 1);
    XSFLIB_CHECK(items.at(kTrack1First).kind == Kind::kProgram && items[kTrack1First].value.value == kOne);
    XSFLIB_CHECK(edited.items.at(kTrack1First) == kTrack1First + 1 &&
                 edited.items.at(kTrack0Program) == kTrack0Program);
}

// Track 2's bank select and program change take the new slot and program.
void TestSlot()
{
    const File file = hokuto::format::ImportSwitchArchive(TestArchive());
    const Sequence before = Played(file, kSong);

    const Edited edited = Edit(file, kSong, 2, {1, 0}, {0, 2});

    const Sequence after = Played(edited.file, kSong);
    const auto& items = edited.file.sequences[0]->items;
    XSFLIB_CHECK(Instruments(after.tracks.at(2)) == (std::vector<Program>{{0, 2}}));
    XSFLIB_CHECK(OthersAsBefore(before, after, 2));
    XSFLIB_CHECK(items.size() == file.sequences[0]->items.size());
    XSFLIB_CHECK(items.at(kTrack2Slot).value.value == 0 && items.at(kTrack2Program).value.value == 2 * kOne);
}

// Track 3's first note plays from the slot that the track starts with, and its bank select comes before the second. A
// new bank select at the track's start selects the new slot.
void TestSlotAtStart()
{
    const File file = hokuto::format::ImportSwitchArchive(TestArchive());
    const Sequence before = Played(file, kSong);

    const Edited edited = Edit(file, kSong, 3, {0, 1}, {1, 1});

    const Sequence after = Played(edited.file, kSong);
    XSFLIB_CHECK(Instruments(after.tracks.at(3)) == (std::vector<Program>{{1, 1}, {1, 1}}));
    XSFLIB_CHECK(Notes(after.tracks[3]) == Notes(before.tracks[3]) && OthersAsBefore(before, after, 3));
    XSFLIB_CHECK(edited.file.sequences[0]->items.size() == file.sequences[0]->items.size() + 1);
}

// Track 0 plays from the slot that it starts with, and selects its program. The bank select goes in at the sound's
// start, before the track starts, and the program change takes the new program.
void TestBoth()
{
    const File file = hokuto::format::ImportSwitchArchive(TestArchive());
    const Sequence before = Played(file, kSong);

    const Edited edited = Edit(file, kSong, 0, {0, 1}, {1, 2});

    const Sequence after = Played(edited.file, kSong);
    const auto& items = edited.file.sequences[0]->items;
    XSFLIB_CHECK(Instruments(after.tracks.at(0)) == (std::vector<Program>{{1, 2}, {1, 2}}));
    XSFLIB_CHECK(Notes(after.tracks[0]) == Notes(before.tracks[0]) && OthersAsBefore(before, after, 0));
    XSFLIB_CHECK(items.size() == file.sequences[0]->items.size() + 1 && items.at(0).kind == Kind::kBankSelect);
    XSFLIB_CHECK(items.at(kTrack0Program + 1).value.value == 2 * kOne);
}

// The program change to a random program takes over after the first note. The first note's program comes from the
// track's start, and a new program change there selects the new program.
void TestBeforeRandom()
{
    const File file = hokuto::format::ImportSwitchArchive(TestArchive());

    const Edited edited = Edit(file, kRandom, 0, {0, 0}, {0, 2});

    const Sequence after = Played(edited.file, kRandom);
    XSFLIB_CHECK(after.tracks.at(0).notes.size() == 2 && Instruments(after.tracks[0]).at(0) == (Program{0, 2}));
    XSFLIB_CHECK(edited.file.sequences[1]->items.size() == file.sequences[1]->items.size() + 1);
}

// The edit refuses a program change that selects the program of notes from two slots, a slot that notes of two programs
// play from, a slot that notes of a random program play from, and an instrument that only a note of a random program
// may play. It also refuses a program change that may select the program of a note after a program change that runs
// only if the last comparison held, an instrument that no note plays, and a slot outside 0 to 3. In a sequence whose
// commands can't move, a program change takes a new program, and a new command is refused.
void TestRefusals()
{
    const File file = hokuto::format::ImportSwitchArchive(TestArchive());
    const Sequence song = Played(file, kSong);
    File fixed = file;
    ntdlib::commands::Sequence outside = *file.sequences[0];
    outside.items.at(1).target_outside = true;
    fixed.sequences[0] = std::move(outside);
    const auto refuses_fixed = [&](std::size_t index, Program from, Program to)
    {
        try
        {
            EditInstrument(fixed, kSong, song.tracks.at(index).number, song.tracks[index].stretches, from, to);
        }
        catch (const std::invalid_argument&)
        {
            return true;
        }

        return false;
    };

    XSFLIB_CHECK(Refuses(file, kSong, 3, {0, 1}, {0, 2}));
    XSFLIB_CHECK(Refuses(file, kSong, 1, {0, 0}, {1, 0}));
    XSFLIB_CHECK(Refusal(file, kRandom, 0, {0, 0}, {1, 0}).value_or("").find("random number") != std::string::npos);
    XSFLIB_CHECK(Refusal(file, kRandom, 0, {0, 1}, {0, 2}).value_or("").find("random number") != std::string::npos);
    XSFLIB_CHECK(Refuses(file, kChoice, 0, {0, 1}, {0, 0}));
    XSFLIB_CHECK(Refusal(file, kSong, 0, {1, 1}, {0, 0}).value_or("").find("No note") != std::string::npos);
    XSFLIB_CHECK(Refuses(file, kSong, 0, {0, 1}, {4, 1}));
    XSFLIB_CHECK(!Refuses(file, kSong, 0, {0, 1}, {0, 1}));
    XSFLIB_CHECK(refuses_fixed(1, {0, 0}, {0, 1}) && !refuses_fixed(0, {0, 1}, {0, 2}));
}

} // namespace

int main()
{
    TestArchivePlays();
    TestProgram();
    TestStart();
    TestSlot();
    TestSlotAtStart();
    TestBoth();
    TestBeforeRandom();
    TestRefusals();

    return xsflib::testing::Summary("instruments");
}
