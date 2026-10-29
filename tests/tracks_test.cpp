// SPDX-License-Identifier: MIT

// Tests the edits of a sound's tracks (src/edit/tracks.h). A small Switch archive holds seven sequence files. In the
// first, track 0 opens track 1, and track 1 calls a subroutine. Two sounds start there. The second allocates track 2
// too, without a track start for it. A third sound starts at track 1's commands. The second file loops after an intro,
// and the third loops back to its first command. Two sounds start there, one without a start in its entry. The fourth
// file opens all 16 tracks. The fifth allocates tracks 0 to 2 with a command, and opens tracks 1 and 2 after a wait. In
// the sixth, track 0 opens track 1, and track 1 opens track 2. In the seventh, track 0 sets another tempo and opens
// track 1 after a wait, and track 1 selects bank slot 1. Its sound holds a second bank in slot 0 and the first in slot
// 1. Each edit goes through an export of the archive. ntdlib's performer then plays the edited archive, and the notes
// that it plays have to show the edit and nothing else. The pastes that give a sound more bank slots use an archive
// laid out as NintendoWare's tools lay one out. ntdlib writes a longer list of banks only there. A small Wii archive
// holds one sequence sound whose track 0 opens track 1. Track 1 transposes, then jumps to track 0's commands. The
// archive takes each edit too.

#include "edit/tracks.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/commands/units.h"
#include "ntdlib/rsar/formats.h"
#include "ntdlib/rsar/sequence_file.h"
#include "ntdlib/rvl/snd/rsar_options.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sar/sequence_file.h"
#include "ntdlib/testing/rsar_files.h"
#include "ntdlib/testing/sar_files.h"
#include "ntdlib/testing/sar_tools.h"
#include "xsflib/testing/check.h"

#include "edit/instruments.h"
#include "edit/notes.h"
#include "edit/replace.h"
#include "format/archive.h"
#include "format/file.h"
#include "song/archive.h"
#include "song/sequence.h"

using hokuto::edit::AddTrack;
using hokuto::edit::CopiedTrack;
using hokuto::edit::CopyTrack;
using hokuto::edit::DuplicateTrack;
using hokuto::edit::PasteTrack;
using hokuto::edit::Program;
using hokuto::edit::RemoveTrack;
using hokuto::edit::SoundPlace;
using hokuto::edit::TrackEdit;
using hokuto::format::File;
using hokuto::song::Note;
using hokuto::song::Sequence;
using hokuto::song::Track;
using ntdlib::commands::Command;
using ntdlib::commands::Kind;
using ntdlib::commands::kOne;

namespace
{

namespace sar = ntdlib::sar;
namespace rsar_files = ntdlib::testing::rsar_files;
namespace sar_files = ntdlib::testing::sar_files;
namespace sar_tools = ntdlib::testing::sar_tools;

// The archive's sounds.
constexpr uint32_t kSong = 0;
constexpr uint32_t kTwin = 1;
constexpr uint32_t kLoop = 2;
constexpr uint32_t kRound = 3;
constexpr uint32_t kRoundTwin = 4;
constexpr uint32_t kFull = 5;
constexpr uint32_t kPart = 6;
constexpr uint32_t kAlloc = 7;
constexpr uint32_t kChain = 8;
constexpr uint32_t kSource = 9;

// The archive's banks, by their item IDs.
const uint32_t kTest = sar::ItemId(sar::kBankItem, 0);
const uint32_t kOther = sar::ItemId(sar::kBankItem, 1);

// The first file's bytecode. Each command's offset comes first in its comment. The Switch's sequences give arguments in
// little-endian order.
const std::vector<uint8_t> kSongBytecode = {
    0x88, 1,    23,   0, 0, // 0: open track 1 at 23
    0xB0, 48,               // 5: timebase 48
    0xE1, 120,  0x00,       // 7: tempo 120
    0xC7, 0x00,             // 10: note wait off
    60,   100,  24,         // 12: key 60 for 24 ticks, at tick 0
    0x80, 48,               // 15: wait 48 ticks
    62,   100,  24,         // 17: key 62 for 24 ticks, at tick 48
    0x80, 48,               // 20: wait 48 ticks
    0xFF,                   // 22: the end, at tick 96
    0xC7, 0x00,             // 23: track 1: note wait off
    0x8A, 35,   0,    0,    // 25: call 35, at tick 0
    64,   100,  24,         // 29: key 64 for 24 ticks, at tick 24
    0x80, 24,               // 32: wait 24 ticks
    0xFF,                   // 34: the end, at tick 48
    67,   100,  12,         // 35: key 67 for 12 ticks
    0x80, 24,               // 38: wait 24 ticks
    0xFD,                   // 40: return
};

// The second file's bytecode: an intro of 96 ticks, then a loop of 48.
const std::vector<uint8_t> kLoopBytecode = {
    0xB0, 48,            // 0: timebase 48
    0xE1, 120,  0x00,    // 2: tempo 120
    0xC7, 0x00,          // 5: note wait off
    60,   100,  24,      // 7: key 60 for 24 ticks, at tick 0
    0x80, 96,            // 10: wait 96 ticks
    62,   100,  24,      // 12: key 62 for 24 ticks, at tick 96: the loop's start
    0x80, 48,            // 15: wait 48 ticks
    0x89, 12,   0,    0, // 17: jump 12, at tick 144
};

// The third file's bytecode: a loop back to the first command.
const std::vector<uint8_t> kRoundBytecode = {
    0xC7, 0x00,        // 0: note wait off
    60,   100,  24,    // 2: key 60 for 24 ticks
    0x80, 48,          // 5: wait 48 ticks
    0x89, 0,    0,  0, // 7: jump 0, at tick 48
};

// The fourth file's bytecode: track starts of tracks 1 to 15 at the end.
std::vector<uint8_t> FullBytecode()
{
    std::vector<uint8_t> bytes;
    for (uint8_t track = 1; track < 16; track++)
    {
        bytes.insert(bytes.end(), {0x88, track, 75, 0, 0});
    }
    bytes.push_back(0xFF);

    return bytes;
}

// The fifth file's bytecode. Track 0 allocates tracks 0 to 2, and opens tracks 1 and 2 after a wait. Both run the same
// commands.
const std::vector<uint8_t> kAllocBytecode = {
    0xFE, 0x07, 0x00,       // 0: allocate tracks 0 to 2
    0xC7, 0x00,             // 3: note wait off
    60,   100,  24,         // 5: key 60 for 24 ticks
    0x80, 24,               // 8: wait 24 ticks
    0x88, 1,    23,   0, 0, // 10: open track 1 at 23, at tick 24
    0x88, 2,    23,   0, 0, // 15: open track 2 at 23
    0x80, 24,               // 20: wait 24 ticks
    0xFF,                   // 22: the end, at tick 48
    0xC7, 0x00,             // 23: tracks 1 and 2: note wait off
    62,   100,  24,         // 25: key 62 for 24 ticks
    0x80, 24,               // 28: wait 24 ticks
    0xFF,                   // 30: the end, at tick 48
};

// The sixth file's bytecode. Track 0 opens track 1, and track 1 opens track 2.
const std::vector<uint8_t> kChainBytecode = {
    0x88, 1,   8,  0, 0, // 0: open track 1 at 8
    0x80, 48,            // 5: wait 48 ticks
    0xFF,                // 7: the end, at tick 48
    0x88, 2,   16, 0, 0, // 8: track 1: open track 2 at 16
    0x80, 48,            // 13: wait 48 ticks
    0xFF,                // 15: the end, at tick 48
    60,   100, 24,       // 16: track 2: key 60 for 24 ticks
    0x80, 24,            // 19: wait 24 ticks
    0xFF,                // 21: the end, at tick 48
};

// The seventh file's bytecode. Track 0 sets the tempo and opens track 1 after a wait. Track 1 selects slot 1.
const std::vector<uint8_t> kSourceBytecode = {
    0xE1, 150,  0x00,       // 0: tempo 150
    0xC7, 0x00,             // 3: note wait off
    0x80, 24,               // 5: wait 24 ticks
    0x88, 1,    18,   0, 0, // 7: open track 1 at 18, at tick 24
    65,   100,  24,         // 12: key 65 for 24 ticks
    0x80, 24,               // 15: wait 24 ticks
    0xFF,                   // 17: the end, at tick 48
    0xC7, 0x00,             // 18: track 1: note wait off
    0xB6, 1,                // 20: bank slot 1
    0x81, 0,                // 22: program 0
    67,   100,  12,         // 24: key 67 for 12 ticks, at tick 24
    0x80, 24,               // 27: wait 24 ticks
    0xFF,                   // 29: the end, at tick 48
};

// The archive's spec. Both banks play a looped wave for every key.
sar_files::ArchiveSpec TestSpec()
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
    song.start_offset = 0;
    song.allocate_track_flags = 0b11;
    sar_files::SoundSpec twin = song;
    twin.name = "SEQ_TWIN";
    twin.allocate_track_flags = 0b111;
    sar_files::SoundSpec loop = song;
    loop.name = "SEQ_LOOP";
    loop.file = 1;
    loop.allocate_track_flags = 1;
    sar_files::SoundSpec round = loop;
    round.name = "SEQ_ROUND";
    round.file = 2;
    sar_files::SoundSpec round_twin = round;
    round_twin.name = "SEQ_ROUND_TWIN";
    round_twin.start_offset = std::nullopt;
    sar_files::SoundSpec full = song;
    full.name = "SEQ_FULL";
    full.file = 3;
    full.allocate_track_flags = 0xFFFF;
    sar_files::SoundSpec part = song;
    part.name = "SEQ_PART";
    part.start_offset = 23;
    sar_files::SoundSpec alloc = song;
    alloc.name = "SEQ_ALLOC";
    alloc.file = 4;
    sar_files::SoundSpec chain = song;
    chain.name = "SEQ_CHAIN";
    chain.file = 5;
    chain.allocate_track_flags = 0b111;
    sar_files::SoundSpec source = song;
    source.name = "SEQ_SOURCE";
    source.file = 8;
    source.banks = {1, 0};

    // The first six sequence files come first, then the first bank, the wave archive, the seventh sequence file and the
    // second bank.
    sar_files::ArchiveSpec spec;
    spec.sounds = {song, twin, loop, round, round_twin, full, part, alloc, chain, source};
    spec.banks = {{"BANK_TEST", 6}, {"BANK_OTHER", 9}};
    spec.wave_archives = {{"WARC_TEST", 7}};
    spec.players = {{"PLAYER_TEST", 1}};
    spec.files = {
        {sar_files::Sequence(false, kSongBytecode, {{"Start", 0}, {"Track1", 23}, {"Sub", 35}}), "", false},
        {sar_files::Sequence(false, kLoopBytecode, {{"Loop", 0}}), "", false},
        {sar_files::Sequence(false, kRoundBytecode, {{"Round", 0}}), "", false},
        {sar_files::Sequence(false, FullBytecode(), {{"Full", 0}}), "", false},
        {sar_files::Sequence(false, kAllocBytecode, {{"Alloc", 0}}), "", false},
        {sar_files::Sequence(false, kChainBytecode, {{"Chain", 0}}), "", false},
        {sar_files::Bank(false, bank), "", false},
        {sar_files::WaveArchive(false, {sar_files::Wave(false, wave)}), "", false},
        {sar_files::Sequence(false, kSourceBytecode, {{"Source", 0}, {"SourceTrack1", 18}}), "", false},
        {sar_files::Bank(false, bank), "", false},
    };

    return spec;
}

// The archive.
std::vector<uint8_t> TestArchive()
{
    return sar_files::Archive(false, TestSpec());
}

// A file that imports the archive laid out as NintendoWare's tools lay one out.
File ToolsFile()
{
    return hokuto::format::ImportSwitchArchive(sar_tools::Archive(sar::Flavor::kNx, TestSpec()));
}

// An archive as the program reads it.
using Archive = std::shared_ptr<const sar::SoundArchive>;

// The archive that `file` exports. The program reads an archive the same way.
Archive Read(const File& file)
{
    return hokuto::song::LoadArchive(hokuto::format::ExportArchive(file), "test.bfsar");
}

// Where each sound plays in `file`. The program finds the places the same way.
std::vector<std::optional<SoundPlace>> PlacesOf(const File& file)
{
    const std::vector<uint8_t> built = hokuto::format::ExportArchive(file);
    const Archive archive = hokuto::song::LoadArchive(built, "test.bfsar");
    return hokuto::edit::SoundPlaces(*archive, hokuto::format::SequencesOfFiles(file, built, *archive),
                                     hokuto::format::StartFieldsOfSounds(file, built));
}

// Sound `sound` as ntdlib's performer plays it from the archive that `file` builds.
Sequence Played(const File& file, uint32_t sound)
{
    return hokuto::song::ReadSequence(*Read(file), sound);
}

// The numbers of the tracks of `song`.
std::vector<int> Numbers(const Sequence& song)
{
    std::vector<int> numbers;
    for (const Track& track : song.tracks)
    {
        numbers.push_back(track.number);
    }

    return numbers;
}

// The start, length, key and velocity of each note of track `number` of `song`.
std::vector<std::vector<uint32_t>> Notes(const Sequence& song, int number)
{
    std::vector<std::vector<uint32_t>> notes;
    for (const Track& track : song.tracks)
    {
        for (const Note& note : track.notes)
        {
            if (track.number == number)
            {
                notes.push_back({note.start, note.length, note.key, note.velocity});
            }
        }
    }

    return notes;
}

// The bytecode of sound `sound`'s file in the archive that `file` builds, and its labels by name and offset.
sar::SequenceFile FileOf(const File& file, uint32_t sound)
{
    const Archive archive = Read(file);
    return sar::SequenceFile::Read(archive->FileData(archive->Sounds()[sound].file_id));
}

// The offset of label `name` in `read`, or nothing.
std::optional<uint32_t> OffsetOf(const sar::SequenceFile& read, const std::string& name)
{
    for (const sar::Sequence::Label& label : read.labels)
    {
        if (label.name == name)
        {
            return label.offset;
        }
    }

    return std::nullopt;
}

// The message of the std::invalid_argument that `call` throws: the edit refuses. Nothing when it throws none.
template <typename Call>
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

// Whether `call` throws std::invalid_argument: the edit refuses.
template <typename Call>
bool Refused(const Call& call)
{
    return RefusalOf(call).has_value();
}

// A new track takes the lowest number that neither the sound's commands open nor the twin allocates. Track 0 opens it
// after track 1, and it waits to the end of the notes. The other tracks play as before, and so do the twin and the
// sound that starts at track 1's commands. Only the sound's entry allocates the new track.
void TestAdd()
{
    const File file = hokuto::format::ImportSwitchArchive(TestArchive());
    const Sequence before = Played(file, kSong);
    const Sequence twin = Played(file, kTwin);
    const Sequence part = Played(file, kPart);

    const TrackEdit added = AddTrack(file, PlacesOf(file), kSong, before, std::nullopt);

    const Sequence after = Played(added.file, kSong);
    const std::vector<Command>& items = added.file.sequences[0]->items;
    XSFLIB_CHECK(added.track == 3 && Numbers(after) == (std::vector<int>{0, 1, 3}));
    XSFLIB_CHECK(Notes(after, 0) == Notes(before, 0) && Notes(after, 1) == Notes(before, 1));
    SEQLIB_REQUIRE(after.tracks.size() > 2);
    XSFLIB_CHECK(after.tracks[2].notes.empty() && !after.tracks[2].stretches.empty());
    XSFLIB_CHECK(Numbers(Played(added.file, kTwin)) == Numbers(twin) &&
                 Notes(Played(added.file, kTwin), 1) == Notes(twin, 1));
    XSFLIB_CHECK(!Notes(part, 0).empty() && Notes(Played(added.file, kPart), 0) == Notes(part, 0));
    SEQLIB_REQUIRE(items.size() == 21);
    XSFLIB_CHECK(items[1].kind == Kind::kOpenTrack && items[1].index == 3 && items[1].target == 18);
    XSFLIB_CHECK(items[18].kind == Kind::kNoteWait && items[18].value.value == 0);
    XSFLIB_CHECK(items[19].kind == Kind::kWait && items[19].value.value == 72 * kOne && items[20].kind == Kind::kEnd);
    XSFLIB_CHECK(added.file.native->tables->sounds[kSong].tracks == 0b1011 &&
                 added.file.native->tables->sounds[kTwin].tracks == 0b111);
    const sar::SequenceFile read = FileOf(added.file, kSong);
    XSFLIB_CHECK(OffsetOf(read, "SEQ_SONG_Track_3") == 46u && OffsetOf(read, "Track1") == 28u);
    XSFLIB_CHECK(OffsetOf(read, "Start") == 0u && OffsetOf(read, "Sub") == 40u);
}

// The new track selects the instrument: a bank slot other than 0, then the program. Before a note goes in, the opening
// of the track gives the instrument (OpeningInstrument). A new track without an instrument selects none.
void TestInstrument()
{
    const File file = hokuto::format::ImportSwitchArchive(TestArchive());
    const Sequence song = Played(file, kSong);

    const TrackEdit added = AddTrack(file, PlacesOf(file), kSong, song, Program{1, 7});
    const TrackEdit first = AddTrack(file, PlacesOf(file), kSong, song, Program{0, 5});
    const TrackEdit none = AddTrack(file, PlacesOf(file), kSong, song, std::nullopt);

    const std::vector<Command>& items = added.file.sequences[0]->items;
    const std::vector<Command>& first_items = first.file.sequences[0]->items;
    XSFLIB_CHECK(items.at(19).kind == Kind::kBankSelect && items[19].value.value == kOne);
    XSFLIB_CHECK(items.at(20).kind == Kind::kProgram && items[20].value.value == 7 * kOne);
    XSFLIB_CHECK(first_items.at(19).kind == Kind::kProgram && first_items[19].value.value == 5 * kOne);

    const auto opening = [](const TrackEdit& edit)
    {
        const Sequence played = Played(edit.file, kSong);
        const auto track = std::find_if(played.tracks.begin(), played.tracks.end(),
                                        [&edit](const Track& t) { return t.number == edit.track; });
        XSFLIB_CHECK(track != played.tracks.end() && track->notes.empty());
        if (track == played.tracks.end())
        {
            return std::optional<Program>();
        }

        return hokuto::edit::OpeningInstrument(edit.file, 0, *track);
    };
    XSFLIB_CHECK(opening(added) == (Program{1, 7}));
    XSFLIB_CHECK(opening(first) == (Program{0, 5}));
    XSFLIB_CHECK(!opening(none).has_value());
}

// With a loop, the new track waits to the loop's start and loops with the sound. A label names the loop's start. A note
// goes into the loop, and the loop stays.
void TestLoop()
{
    const File file = hokuto::format::ImportSwitchArchive(TestArchive());
    const Sequence before = Played(file, kLoop);

    const TrackEdit added = AddTrack(file, PlacesOf(file), kLoop, before, std::nullopt);
    const Sequence after = Played(added.file, kLoop);
    const Track& track = after.tracks.at(1);
    SEQLIB_REQUIRE(hokuto::edit::StretchAt(track.stretches, 120));
    const hokuto::edit::NewNote note{*hokuto::edit::StretchAt(track.stretches, 120), 120, 72, 100, 12};
    const hokuto::edit::Edited edited = hokuto::edit::EditNotes(added.file, 1, {}, {&note, 1});

    const Sequence played = Played(edited.file, kLoop);
    XSFLIB_CHECK(added.track == 1 && Numbers(after) == (std::vector<int>{0, 1}) && track.notes.empty());
    XSFLIB_CHECK(before.loop && after.loop == before.loop && played.loop == before.loop);
    XSFLIB_CHECK(Notes(after, 0) == Notes(before, 0) && Notes(played, 0) == Notes(before, 0));
    XSFLIB_CHECK(Notes(played, 1) == (std::vector<std::vector<uint32_t>>{{120, 12, 72, 100}}));
    const sar::SequenceFile read = FileOf(added.file, kLoop);
    XSFLIB_CHECK(OffsetOf(read, "SEQ_LOOP_Track_1") == 26u && OffsetOf(read, "SEQ_LOOP_Track_1_Loop") == 30u);
}

// Where no track start opens the sound's commands, the new one goes first. The sounds that start there start at it, and
// the loop back to the first command doesn't run it again. The twin without a start in its entry runs it, and doesn't
// allocate the track. A copy of track 0 starts there in the same way.
void TestFirst()
{
    const File file = hokuto::format::ImportSwitchArchive(TestArchive());
    const Sequence before = Played(file, kRound);
    const Sequence twin = Played(file, kRoundTwin);

    const TrackEdit added = AddTrack(file, PlacesOf(file), kRound, before, std::nullopt);
    const TrackEdit copied = DuplicateTrack(file, PlacesOf(file), kRound, 0);

    const sar::SequenceFile read = FileOf(added.file, kRound);
    const Sequence copy = Played(copied.file, kRound);
    const Sequence after = Played(added.file, kRound);
    XSFLIB_CHECK(added.track == 1 && Numbers(after) == (std::vector<int>{0, 1}));
    XSFLIB_CHECK(Notes(after, 0) == Notes(before, 0) && after.loop == before.loop);
    XSFLIB_CHECK(Numbers(Played(added.file, kRoundTwin)) == (std::vector<int>{0}) &&
                 Notes(Played(added.file, kRoundTwin), 0) == Notes(twin, 0));
    XSFLIB_CHECK(read.data.at(0) == 0x88 && read.data.at(1) == 1);
    XSFLIB_CHECK(read.data.at(12) == 0x89 && read.data.at(13) == 5 && OffsetOf(read, "Round") == 0u);
    XSFLIB_CHECK(copied.track == 1 && Numbers(copy) == (std::vector<int>{0, 1}));
    XSFLIB_CHECK(Notes(copy, 0) == Notes(before, 0) && Notes(copy, 1) == Notes(before, 0));
}

// A copy of track 1 plays its notes, with a copy of the subroutine. Its track start goes after track 1's, and runs only
// if the last comparison held where track 1's does. A copy of track 0 leaves out the track start of track 1, and plays
// track 0's notes.
void TestDuplicate()
{
    const File file = hokuto::format::ImportSwitchArchive(TestArchive());
    const Sequence before = Played(file, kSong);

    ntdlib::commands::Sequence branching = *file.sequences[0];
    branching.items[0].conditional = true;
    File branches = file;
    branches.sequences[0] = std::move(branching);

    const TrackEdit one = DuplicateTrack(file, PlacesOf(file), kSong, 1);
    const TrackEdit zero = DuplicateTrack(file, PlacesOf(file), kSong, 0);
    const TrackEdit conditional = DuplicateTrack(branches, PlacesOf(branches), kSong, 1);

    const Sequence after = Played(one.file, kSong);
    const Sequence copied = Played(zero.file, kSong);
    const std::vector<Command>& items = one.file.sequences[0]->items;
    XSFLIB_CHECK(one.track == 3 && Numbers(after) == (std::vector<int>{0, 1, 3}));
    XSFLIB_CHECK(Notes(after, 3) == Notes(before, 1) && Notes(after, 1) == Notes(before, 1) &&
                 Notes(after, 0) == Notes(before, 0));
    XSFLIB_CHECK(items[1].kind == Kind::kOpenTrack && items[1].index == 3 && items[1].target == 18);
    XSFLIB_CHECK(items.at(19).kind == Kind::kCall && items[19].target == 23 && items.at(25).kind == Kind::kReturn);
    const sar::SequenceFile read = FileOf(one.file, kSong);
    XSFLIB_CHECK(OffsetOf(read, "SEQ_SONG_Track_3") == 46u && OffsetOf(read, "Sub_2") == 58u);
    XSFLIB_CHECK(!OffsetOf(read, "Track1_2"));
    XSFLIB_CHECK(zero.track == 3 && Numbers(copied) == (std::vector<int>{0, 1, 3}));
    XSFLIB_CHECK(Notes(copied, 3) == Notes(before, 0) && Notes(copied, 1) == Notes(before, 1));
    XSFLIB_CHECK(zero.file.sequences[0]->items.at(18).kind == Kind::kNoteWait &&
                 zero.file.sequences[0]->items.size() == 24);
    XSFLIB_CHECK(conditional.file.sequences[0]->items.at(1).kind == Kind::kOpenTrack &&
                 conditional.file.sequences[0]->items[1].conditional);
    XSFLIB_CHECK(!items[1].conditional);
}

// A copy of a track leaves out the commands that set the whole sound's tempo, timebase and volume. Track 1 sets another
// tempo in the tick where track 0 sets its tempo. Track 1 runs after track 0, and the sound keeps track 1's tempo with
// a copy of track 0.
void TestDuplicateTempo()
{
    const File file = hokuto::format::ImportSwitchArchive(TestArchive());
    ntdlib::commands::Sequence tempos = *file.sequences[0];
    tempos.items.at(9) = tempos.items.at(2);
    tempos.items[9].value.value = 60 * kOne;
    File slower = file;
    slower.sequences[0] = std::move(tempos);
    const Sequence before = Played(slower, kSong);

    const TrackEdit zero = DuplicateTrack(slower, PlacesOf(slower), kSong, 0);
    const Sequence copied = Played(zero.file, kSong);
    XSFLIB_CHECK(before.tempo.size() == 1 && before.tempo[0].bpm == 60.0 && copied.tempo == before.tempo);
    XSFLIB_CHECK(Notes(copied, 3) == Notes(before, 0) && Notes(copied, 1) == Notes(before, 1));
}

// An allocation at the sound's start stays first. A new track's start goes after it, and before the sound's first note.
// The track starts after the first wait stay. Track 2 runs the same commands as track 1. A copy of track 1 still starts
// after track 1's start only. A removed track leaves the allocation at the start too.
void TestAllocation()
{
    const File file = hokuto::format::ImportSwitchArchive(TestArchive());
    const std::vector<std::optional<SoundPlace>> places = PlacesOf(file);
    const Sequence before = Played(file, kAlloc);

    const TrackEdit added = AddTrack(file, places, kAlloc, before, std::nullopt);
    const TrackEdit copied = DuplicateTrack(file, places, kAlloc, 1);
    const File removed = RemoveTrack(file, places, kAlloc, 2);

    const std::vector<Command>& items = added.file.sequences[4]->items;
    const std::vector<Command>& copy = copied.file.sequences[4]->items;
    XSFLIB_CHECK(Numbers(before) == (std::vector<int>{0, 1, 2}) && Notes(before, 1) == Notes(before, 2));
    XSFLIB_CHECK(added.track == 3 && Numbers(Played(added.file, kAlloc)) == (std::vector<int>{0, 1, 2, 3}));
    XSFLIB_CHECK(items[0].kind == Kind::kAllocateTracks && items[1].kind == Kind::kOpenTrack && items[1].index == 3);
    XSFLIB_CHECK(items[5].kind == Kind::kOpenTrack && items[5].index == 1 && items[6].index == 2);
    XSFLIB_CHECK(copied.track == 3 && copy.size() == 17 && copy[4].kind == Kind::kOpenTrack && copy[4].index == 1);
    XSFLIB_CHECK(copy[5].kind == Kind::kOpenTrack && copy[5].index == 3 && copy[6].index == 2);
    XSFLIB_CHECK(Notes(Played(copied.file, kAlloc), 3) == Notes(before, 1));
    XSFLIB_CHECK(Numbers(Played(removed, kAlloc)) == (std::vector<int>{0, 1}));
    XSFLIB_CHECK(removed.sequences[4]->items[0].value.value == 3 * kOne &&
                 removed.native->tables->sounds[kAlloc].tracks == 0b11);
}

// A removed track leaves the sound's entry. Its commands stay. Track 0, a track that the sound doesn't allocate, the
// last track that a sound allocates and a sound without a free track are refused. So is a track that opens another
// track of the sound, until that track goes. So are copies of a track that the sound doesn't allocate, and of one that
// its commands don't open, and a new track among commands that can't move.
void TestRemove()
{
    const File file = hokuto::format::ImportSwitchArchive(TestArchive());
    const std::vector<std::optional<SoundPlace>> places = PlacesOf(file);
    const Sequence before = Played(file, kSong);
    const Sequence full = Played(file, kFull);

    // Entries that allocate only track 0, and only track 1.
    File odd = file;
    const auto change = [](hokuto::format::NativeArchive& native)
    {
        native.tables->sounds[kSong].tracks = 1;
        native.tables->sounds[kLoop].tracks = 0b10;
    };
    odd.native.Edit(change);

    // A jump to a byte offset. Such a target can't move.
    ntdlib::commands::Sequence outside = *file.sequences[2];
    outside.items.at(3).target_outside = true;
    File fixed = file;
    fixed.sequences[2] = std::move(outside);

    const File removed = RemoveTrack(file, places, kSong, 1);
    const File unchained = RemoveTrack(RemoveTrack(file, places, kChain, 2), places, kChain, 1);

    const Sequence after = Played(removed, kSong);
    XSFLIB_CHECK(Numbers(after) == (std::vector<int>{0}) && Notes(after, 0) == Notes(before, 0));
    XSFLIB_CHECK(removed.native->tables->sounds[kSong].tracks == 1 && removed.sequences[0] == file.sequences[0]);
    XSFLIB_CHECK(Numbers(Played(file, kChain)) == (std::vector<int>{0, 1, 2}));
    XSFLIB_CHECK(Numbers(Played(unchained, kChain)) == (std::vector<int>{0}));
    XSFLIB_CHECK(Refused([&] { RemoveTrack(file, places, kChain, 1); }));
    XSFLIB_CHECK(Refused([&] { RemoveTrack(odd, places, kLoop, 1); }));
    XSFLIB_CHECK(Refused([&] { DuplicateTrack(odd, places, kSong, 1); }));
    XSFLIB_CHECK(Refused([&] { DuplicateTrack(file, places, kPart, 1); }));
    XSFLIB_CHECK(Refused([&] { RemoveTrack(file, places, kSong, 0); }));
    XSFLIB_CHECK(Refused([&] { RemoveTrack(file, places, kSong, 2); }));
    XSFLIB_CHECK(Refused([&] { AddTrack(file, places, kFull, full, std::nullopt); }));
    XSFLIB_CHECK(Refused([&] { DuplicateTrack(file, places, kSong, 2); }));
    XSFLIB_CHECK(Refused([&] { AddTrack(fixed, places, kRound, Played(file, kRound), std::nullopt); }));
}

// A track of another sound plays its notes in the sound, from the tick at which the other sound started it. Its bank
// select selects the sound's slot 0. That slot holds the same bank. An empty slot takes the bank of its first notes,
// and a bank select first selects that slot. The sound keeps its notes, and labels name the new commands.
void TestPaste()
{
    const File file = ToolsFile();
    const std::vector<std::optional<SoundPlace>> places = PlacesOf(file);
    const Sequence source = Played(file, kSource);
    const Sequence before = Played(file, kSong);

    const CopiedTrack copied = CopyTrack(file, places, kSource, source, 1);
    const TrackEdit pasted = PasteTrack(file, places, kSong, before, copied);

    const Sequence after = Played(pasted.file, kSong);
    const std::vector<Command>& items = pasted.file.sequences[0]->items;
    XSFLIB_CHECK(copied.tick == 24 && copied.commands.size() == 6 && copied.labels.size() == 1);
    XSFLIB_CHECK(pasted.track == 3 && Numbers(after) == (std::vector<int>{0, 1, 3}));
    XSFLIB_CHECK(!Notes(source, 1).empty() && Notes(after, 3) == Notes(source, 1));
    XSFLIB_CHECK(Notes(after, 0) == Notes(before, 0) && Notes(after, 1) == Notes(before, 1));
    XSFLIB_CHECK(after.tracks.at(2).notes.at(0).bank_slot == 0 && source.tracks.at(1).notes.at(0).bank_slot == 1);
    XSFLIB_CHECK(pasted.file.native->tables->sounds[kSong].banks == (std::vector<uint32_t>{kTest, kOther}));
    XSFLIB_CHECK(items.at(18).kind == Kind::kBankSelect && items[18].value.value == kOne);
    XSFLIB_CHECK(items.at(19).kind == Kind::kWait && items[19].value.value == 24 * kOne);
    XSFLIB_CHECK(items.at(21).kind == Kind::kBankSelect && items[21].value.value == 0);
    const sar::SequenceFile read = FileOf(pasted.file, kSong);
    XSFLIB_CHECK(OffsetOf(read, "SEQ_SONG_Track_3") == 46u && OffsetOf(read, "SourceTrack1") == 50u);
}

// A paste of another sound's track 0 leaves out its tempo and its track start. The sound keeps its tempo and its loop.
// Track 0 of a sound without a track start opens the pasted track first.
void TestPasteFirst()
{
    const File file = ToolsFile();
    const std::vector<std::optional<SoundPlace>> places = PlacesOf(file);
    const Sequence source = Played(file, kSource);
    const Sequence before = Played(file, kLoop);

    const TrackEdit pasted = PasteTrack(file, places, kLoop, before, CopyTrack(file, places, kSource, source, 0));

    const Sequence after = Played(pasted.file, kLoop);
    XSFLIB_CHECK(pasted.track == 1 && Numbers(after) == (std::vector<int>{0, 1}));
    XSFLIB_CHECK(!Notes(source, 0).empty() && Notes(after, 1) == Notes(source, 0));
    XSFLIB_CHECK(Notes(after, 0) == Notes(before, 0) && after.tempo == before.tempo && after.loop == before.loop);
    XSFLIB_CHECK(before.tempo != source.tempo && pasted.file.sequences[1]->items.at(0).kind == Kind::kOpenTrack);
    XSFLIB_CHECK(pasted.file.native->tables->sounds[kLoop].banks == (std::vector<uint32_t>{kTest, kOther}));
}

// A pasted track's jumps go to its copied commands. The bank select that comes first stays out of the loop, and the
// sound loops as the other sound did.
void TestPasteLoop()
{
    const File file = ToolsFile();
    const std::vector<std::optional<SoundPlace>> places = PlacesOf(file);
    const Sequence loop = Played(file, kLoop);

    const TrackEdit pasted =
        PasteTrack(file, places, kSource, Played(file, kSource), CopyTrack(file, places, kLoop, loop, 0));

    const Sequence after = Played(pasted.file, kSource);
    XSFLIB_CHECK(pasted.track == 2 && !Notes(loop, 0).empty() && Notes(after, 2) == Notes(loop, 0));
    XSFLIB_CHECK(loop.loop && after.loop == loop.loop);
}

// A track pasted into the sound that it came from plays its notes again, with a copy of its subroutine. The bank slots
// stay. The track's name labels its first command, in place of the label there.
void TestPasteSame()
{
    const File file = hokuto::format::ImportSwitchArchive(TestArchive());
    const std::vector<std::optional<SoundPlace>> places = PlacesOf(file);
    const Sequence before = Played(file, kSong);

    const TrackEdit pasted = PasteTrack(file, places, kSong, before, CopyTrack(file, places, kSong, before, 1));

    const Sequence after = Played(pasted.file, kSong);
    XSFLIB_CHECK(pasted.track == 3 && Notes(after, 3) == Notes(before, 1) && Notes(after, 1) == Notes(before, 1));
    XSFLIB_CHECK(pasted.file.native->tables->sounds[kSong].banks == file.native->tables->sounds[kSong].banks);
    const sar::SequenceFile read = FileOf(pasted.file, kSong);
    XSFLIB_CHECK(OffsetOf(read, "SEQ_SONG_Track_3") == 46u && OffsetOf(read, "Sub_2") && !OffsetOf(read, "Track1_2"));
}

// A slot that the sound's commands select stays out even when it's empty. A bank takes its slot when that slot is free.
// A bank select from a variable keeps its slots: each has to be free or hold the bank already. Four full slots, a bank
// that the archive lacks, a track that the sound doesn't play and a jump to a byte offset are refused.
void TestSlots()
{
    const File file = ToolsFile();
    const std::vector<std::optional<SoundPlace>> places = PlacesOf(file);
    const Sequence song = Played(file, kSong);
    const Sequence source = Played(file, kSource);

    // The source with the second bank alone. Its track 1 selects the empty slot 1.
    File single = file;
    const auto one = [](hokuto::format::NativeArchive& native)
    {
        native.tables->sounds[kSource].banks = {kOther};
    };
    single.native.Edit(one);

    // The source's track 1 with slot 2 for the second bank, and with a slot from a variable.
    CopiedTrack own = CopyTrack(file, places, kSource, source, 1);
    own.commands.at(1).value.value = 2 * kOne;
    own.banks = {sar::kNoItem, sar::kNoItem, kOther};
    CopiedTrack chancy = own;
    chancy.commands[1].value = {ntdlib::commands::Source::kVariable, 0, 0};
    chancy.banks = {kTest, kOther};
    CopiedTrack clash = chancy;
    clash.banks = {kOther};
    CopiedTrack foreign = own;
    foreign.banks = {sar::kNoItem, sar::kNoItem, sar::ItemId(sar::kBankItem, 5)};

    // A sound with four full slots, and a jump to a byte offset.
    File full = file;
    const auto fill = [](hokuto::format::NativeArchive& native)
    {
        native.tables->sounds[kSong].banks = {kTest, kTest, kTest, kTest};
    };
    full.native.Edit(fill);
    ntdlib::commands::Sequence outside = *file.sequences[2];
    outside.items.at(3).target_outside = true;
    File fixed = file;
    fixed.sequences[2] = std::move(outside);

    const TrackEdit taken =
        PasteTrack(single, places, kSource, Played(single, kSource), CopyTrack(file, places, kSong, song, 0));
    const TrackEdit kept = PasteTrack(file, places, kSong, song, own);
    const TrackEdit varied = PasteTrack(file, places, kSong, song, chancy);

    const Sequence after = Played(taken.file, kSource);
    XSFLIB_CHECK(taken.file.native->tables->sounds[kSource].banks ==
                 (std::vector<uint32_t>{kOther, sar::kNoItem, kTest}));
    XSFLIB_CHECK(taken.track == 2 && Notes(after, 2) == Notes(song, 0) && after.tempo == source.tempo);
    XSFLIB_CHECK(kept.file.native->tables->sounds[kSong].banks == (std::vector<uint32_t>{kTest, sar::kNoItem, kOther}));
    XSFLIB_CHECK(kept.file.sequences[0]->items.at(20).kind == Kind::kBankSelect &&
                 kept.file.sequences[0]->items[20].value.value == 2 * kOne);
    XSFLIB_CHECK(varied.file.native->tables->sounds[kSong].banks == (std::vector<uint32_t>{kTest, kOther}));
    XSFLIB_CHECK(Refused([&] { PasteTrack(file, places, kSong, song, clash); }));
    XSFLIB_CHECK(Refused([&] { PasteTrack(file, places, kSong, song, foreign); }));
    XSFLIB_CHECK(Refused([&] { PasteTrack(full, places, kSong, song, CopyTrack(file, places, kSource, source, 0)); }));
    XSFLIB_CHECK(Refused([&] { CopyTrack(file, places, kSource, source, 5); }));
    XSFLIB_CHECK(Refused([&] { CopyTrack(fixed, places, kRound, Played(file, kRound), 0); }));
}

// A track that counts other ticks to a quarter note takes the sound's ticks: its waits, its notes' lengths, the times
// over which its settings move and the tick at which it starts. A time that falls between the sound's ticks, or comes
// from a variable, is refused, and so is a start between them.
void TestTimebase()
{
    const File file = ToolsFile();
    const std::vector<std::optional<SoundPlace>> places = PlacesOf(file);
    const Sequence song = Played(file, kSong);

    // The source's track 1 as if its sound counted 96 ticks to a quarter note, then 24. A volume moves over 24 ticks
    // before the note.
    CopiedTrack slow = CopyTrack(file, places, kSource, Played(file, kSource), 1);
    slow.timebase = 96;
    Command volume;
    volume.kind = Kind::kVolume;
    volume.value.value = 100 * kOne;
    volume.time = ntdlib::commands::Argument{ntdlib::commands::Source::kValue, 24 * kOne, 0};
    slow.commands.insert(slow.commands.begin() + 1, volume);
    CopiedTrack quick = slow;
    quick.timebase = 24;
    CopiedTrack odd = slow;
    odd.commands.at(4).value.value = 13 * kOne;
    CopiedTrack waiting = slow;
    waiting.commands.at(5).value = {ntdlib::commands::Source::kVariable, 0, 0};
    CopiedTrack late = slow;
    late.tick = 25;

    const TrackEdit halved = PasteTrack(file, places, kSong, song, slow);
    const TrackEdit doubled = PasteTrack(file, places, kSong, song, quick);

    XSFLIB_CHECK(Notes(Played(halved.file, kSong), 3) == (std::vector<std::vector<uint32_t>>{{12, 6, 67, 100}}));
    XSFLIB_CHECK(Notes(Played(doubled.file, kSong), 3) == (std::vector<std::vector<uint32_t>>{{48, 24, 67, 100}}));
    SEQLIB_REQUIRE(halved.file.sequences[0]->items.at(21).time && doubled.file.sequences[0]->items.at(21).time);
    XSFLIB_CHECK(halved.file.sequences[0]->items.at(21).time->value == 12 * kOne);
    XSFLIB_CHECK(doubled.file.sequences[0]->items.at(21).time->value == 48 * kOne);
    XSFLIB_CHECK(Refused([&] { PasteTrack(file, places, kSong, song, odd); }));
    XSFLIB_CHECK(Refused([&] { PasteTrack(file, places, kSong, song, waiting); }));
    XSFLIB_CHECK(Refused([&] { PasteTrack(file, places, kSong, song, late); }));
}

// The Wii archive's bytecode. Track 0 opens track 1, and each plays a note. The Wii's sequences give arguments in
// big-endian order.
const std::vector<uint8_t> kWiiBytecode = {
    0x88, 1,    0,  0, 13, // 0: open track 1 at 13
    0xC7, 0x00,            // 5: note wait off
    60,   100,  24,        // 7: key 60 for 24 ticks
    0x80, 48,              // 10: wait 48 ticks
    0xFF,                  // 12: the end, at tick 48
    0xC3, 4,               // 13: track 1: transpose 4
    0x89, 0,    0,  5,     // 15: jump 5: track 1 plays key 64
};

// The Wii archive. Its one sound allocates tracks 0 and 1, and plays a bank whose program 0 plays a wave.
File WiiFile()
{
    ntdlib::rsar::SequenceFile sequence;
    sequence.data = kWiiBytecode;
    sequence.labels = {{"SEQ_WII", 0}, {"WiiTrack1", 13}};

    return hokuto::format::ImportWiiArchive(rsar_files::MakeArchive(sequence.Write(), 0, 0b11));
}

// The program's reading of the Wii archive that `file` builds.
std::shared_ptr<const ntdlib::rsar::SoundArchive> ReadWii(const File& file)
{
    return hokuto::song::LoadWiiArchive(hokuto::format::ExportArchive(file), "test.brsar",
                                        ntdlib::rvl::snd::kRsarOptions);
}

// Where each sound plays in `file`, a file of the Wii.
std::vector<std::optional<SoundPlace>> WiiPlacesOf(const File& file)
{
    const std::vector<uint8_t> built = hokuto::format::ExportArchive(file);
    const auto archive = hokuto::song::LoadWiiArchive(built, "test.brsar", ntdlib::rvl::snd::kRsarOptions);
    return hokuto::edit::SoundPlaces(*archive, hokuto::format::SequencesOfFiles(file, built, *archive),
                                     hokuto::format::StartFieldsOfSounds(file, built));
}

// The Wii's sound as ntdlib's performer plays it from the archive that `file` builds.
Sequence WiiPlayed(const File& file)
{
    return hokuto::song::ReadSequence(*ReadWii(file), 0);
}

// The Wii's sequences take each edit. A new track, a copy of track 1 and a paste of track 1 play as on the other
// consoles, and a removal stops track 1. The paste starts where track 1 starts, after the commands that it jumps back
// to, and transposes their key too. A sound of the Wii plays one bank and has no bank select: a track of another bank
// can't go in.
void TestWii()
{
    const File file = WiiFile();
    const Sequence before = WiiPlayed(file);
    XSFLIB_CHECK(Numbers(before) == (std::vector<int>{0, 1}));
    XSFLIB_CHECK(Notes(before, 0) == (std::vector<std::vector<uint32_t>>{{0, 24, 60, 100}}) &&
                 Notes(before, 1) == (std::vector<std::vector<uint32_t>>{{0, 24, 64, 100}}));

    const TrackEdit added = AddTrack(file, WiiPlacesOf(file), 0, before, Program{0, 0});
    const Sequence after = WiiPlayed(added.file);
    XSFLIB_CHECK(added.track == 2 && Numbers(after) == (std::vector<int>{0, 1, 2}));
    XSFLIB_CHECK(Notes(after, 0) == Notes(before, 0) && Notes(after, 1) == Notes(before, 1));
    XSFLIB_CHECK(after.tracks.at(2).notes.empty() && added.file.native->tables->sounds[0].tracks == 0b111);
    const std::vector<Command>& items = added.file.sequences[0]->items;
    XSFLIB_CHECK(items.at(1).kind == Kind::kOpenTrack && items[1].index == 2);
    XSFLIB_CHECK(std::ranges::none_of(items, [](const Command& command) { return command.kind == Kind::kBankSelect; }));

    const TrackEdit copied = DuplicateTrack(file, WiiPlacesOf(file), 0, 1);
    XSFLIB_CHECK(copied.track == 2 && Notes(WiiPlayed(copied.file), 2) == Notes(before, 1) &&
                 Notes(WiiPlayed(copied.file), 1) == Notes(before, 1));

    const File removed = RemoveTrack(file, WiiPlacesOf(file), 0, 1);
    XSFLIB_CHECK(Numbers(WiiPlayed(removed)) == (std::vector<int>{0}) && removed.native->tables->sounds[0].tracks == 1);

    const CopiedTrack track = CopyTrack(file, WiiPlacesOf(file), 0, before, 1);
    const TrackEdit pasted = PasteTrack(file, WiiPlacesOf(file), 0, before, track);
    XSFLIB_CHECK(track.commands.size() == 6 && track.entry == 4 && track.commands[4].kind == Kind::kTranspose);
    XSFLIB_CHECK(pasted.track == 2 && Notes(WiiPlayed(pasted.file), 2) == Notes(before, 1));
    XSFLIB_CHECK(pasted.file.native->tables->sounds[0].banks == file.native->tables->sounds[0].banks);

    CopiedTrack other = track;
    other.banks = {sar::ItemId(sar::kBankItem, 1)};
    XSFLIB_CHECK(RefusalOf([&] { PasteTrack(file, WiiPlacesOf(file), 0, before, other); }) ==
                 "The track plays a bank, and the sound plays BANK_TEST. A sound of the Wii plays one bank.");
}

} // namespace

int main()
{
    TestAdd();
    TestInstrument();
    TestLoop();
    TestFirst();
    TestDuplicate();
    TestDuplicateTempo();
    TestAllocation();
    TestRemove();
    TestPaste();
    TestPasteFirst();
    TestPasteLoop();
    TestPasteSame();
    TestSlots();
    TestTimebase();
    TestWii();

    return xsflib::testing::Summary("tracks");
}
