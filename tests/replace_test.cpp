// SPDX-License-Identifier: MIT

// Tests the replacement of a sound's sequence (src/edit/replace.h). A small Switch archive holds three sequence files.
// The first holds three sounds. Two start where the file starts, without a start in their entries, and the third starts
// at a note. Each of the other files holds one sound, the first with a start of 0 in its entry and the second without
// one. The new sequence comes from a file with two labels. The first plays a note after a program change. The second
// selects bank slot 1, changes the program and calls a subroutine. The checks read the exported archive with ntdlib. A
// file with the commands of the first file gives the starts of its sounds. A file of another console takes the biquad
// filters that come nearest on the archive's console. A small Wii archive holds one sound, and a sequence file of the
// Wii takes the place of its sequence. A sequence file of the Switch goes in there converted. A sweep's pitch of the
// Switch takes the pitch that an older build of the Wii holds.

#include "edit/replace.h"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <span>
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
#include "ntdlib/sound_build.h"
#include "ntdlib/testing/rsar_tools.h"
#include "ntdlib/testing/sar_files.h"
#include "ntdlib/testing/sar_tools.h"
#include "xsflib/testing/check.h"

#include "edit/other_console.h"
#include "format/archive.h"
#include "format/file.h"
#include "format/text.h"
#include "song/archive.h"
#include "song/biquads.h"
#include "song/items.h"

using hokuto::edit::Incoming;
using hokuto::edit::Program;
using hokuto::edit::ProgramMap;
using hokuto::edit::ProgramsSelected;
using hokuto::edit::ReadIncoming;
using hokuto::edit::ReplaceSequence;
using hokuto::edit::Selections;
using hokuto::edit::SharingFile;
using hokuto::edit::SharingStart;
using hokuto::edit::SoundPlace;
using hokuto::edit::SoundStarts;
using hokuto::edit::StartingSound;
using hokuto::format::File;
using ntdlib::commands::Command;
using ntdlib::commands::Kind;
using ntdlib::commands::kOne;
using ntdlib::commands::Source;

namespace
{

namespace sar = ntdlib::sar;
namespace sar_files = ntdlib::testing::sar_files;
namespace sar_tools = ntdlib::testing::sar_tools;

// The archive's sounds.
constexpr uint32_t kShared = 0;
constexpr uint32_t kEntry = 1;
constexpr uint32_t kAlone = 2;
constexpr uint32_t kPlain = 3;
constexpr uint32_t kTwin = 4;

// The first file's bytecode. One sound starts at offset 0 and the other at offset 7. The file has one command more than
// the new sequence has before Main. When Main's commands go after the file's, the call's target therefore changes.
const std::vector<uint8_t> kSharedBytecode = {
    0xB0, 48,        // 0: timebase 48
    0xE1, 120, 0x00, // 2: tempo 120
    0xC1, 100,       // 5: volume 100
    60,   100, 24,   // 7: key 60 for 24 ticks
    0xFF,            // 10: the end
};

// The first file's bytecode as the Wii U holds it: the tempo is big-endian.
const std::vector<uint8_t> kWiiUSharedBytecode = {0xB0, 48, 0xE1, 0x00, 120, 0xC1, 100, 60, 100, 24, 0xFF};

// The second and the third file's bytecode.
const std::vector<uint8_t> kAloneBytecode = {0xB0, 48, 64, 100, 24, 0xFF};
const std::vector<uint8_t> kPlainBytecode = {0xB0, 48, 65, 100, 24, 0xFF};

// The new sequence's bytecode in the Switch's byte order. Intro starts at 0 and Main at 8.
const std::vector<uint8_t> kNewBytecode = {
    0xB0, 48,         // 0: timebase 48
    0x81, 5,          // 2: program 5
    60,   100, 24,    // 4: key 60 for 24 ticks
    0xFF,             // 7: the end
    0xB6, 1,          // 8: bank slot 1
    0x81, 7,          // 10: program 7
    0x8A, 17,  0,  0, // 12: call 17
    0xFF,             // 16: the end
    62,   100, 24,    // 17: key 62 for 24 ticks
    0xFD,             // 20: return
};

// The same as the Wii U holds it: the call's offset is big-endian.
const std::vector<uint8_t> kWiiUNewBytecode = {0xB0, 48,   0x81, 5, 60, 100,  24, 0xFF, 0xB6, 1,   0x81,
                                               7,    0x8A, 0,    0, 17, 0xFF, 62, 100,  24,   0xFD};

// The items of the new sequence where its labels start.
constexpr uint32_t kIntro = 0;
constexpr uint32_t kMain = 4;

// The test archive's sounds and files. SEQ_SHARED, SEQ_ENTRY and SEQ_TWIN play the first file, SEQ_ENTRY from offset 7.
// SEQ_ALONE plays the second file with a start of 0 in its entry, and SEQ_PLAIN the third without one.
sar_files::ArchiveSpec TestSpec()
{
    sar_files::SoundSpec shared;
    shared.name = "SEQ_SHARED";
    sar_files::SoundSpec entry = shared;
    entry.name = "SEQ_ENTRY";
    entry.start_offset = 7;
    sar_files::SoundSpec alone = shared;
    alone.name = "SEQ_ALONE";
    alone.file = 1;
    alone.start_offset = 0;
    sar_files::SoundSpec plain = shared;
    plain.name = "SEQ_PLAIN";
    plain.file = 2;
    sar_files::SoundSpec twin = shared;
    twin.name = "SEQ_TWIN";

    sar_files::ArchiveSpec spec;
    spec.sounds = {shared, entry, alone, plain, twin};
    spec.players = {{"PLAYER_TEST", 1}};
    spec.files = {{sar_files::Sequence(false, kSharedBytecode, {{"Shared", 0}, {"Entry", 7}}), "", false},
                  {sar_files::Sequence(false, kAloneBytecode, {{"Alone", 0}}), "", false},
                  {sar_files::Sequence(false, kPlainBytecode, {{"Plain", 0}}), "", false}};

    return spec;
}

// The archive of TestSpec, for the Switch.
std::vector<uint8_t> TestArchive()
{
    return sar_files::Archive(false, TestSpec());
}

// An archive as the program reads it.
using Archive = std::shared_ptr<const sar::SoundArchive>;

// The archive `bytes`. The program reads an archive the same way.
Archive Read(std::vector<uint8_t> bytes)
{
    return hokuto::song::LoadArchive(std::move(bytes), "test.bfsar");
}

// Where each sound plays in `file`. The program finds the places the same way.
std::vector<std::optional<SoundPlace>> PlacesOf(const File& file)
{
    const std::vector<uint8_t> built = hokuto::format::ExportArchive(file);
    const Archive archive = Read(built);
    return hokuto::edit::SoundPlaces(*archive, hokuto::format::SequencesOfFiles(file, built, *archive),
                                     hokuto::format::StartFieldsOfSounds(file, built));
}

// The bytecode of sound `sound`'s file in `archive`, without the zeros that pad it.
std::vector<uint8_t> BytecodeOf(const Archive& archive, uint32_t sound)
{
    std::vector<uint8_t> data = sar::SequenceFile::Read(archive->FileData(archive->Sounds()[sound].file_id)).data;
    while (!data.empty() && data.back() == 0)
    {
        data.pop_back();
    }

    return data;
}

// The labels of sound `sound`'s file in `archive`, by name and offset.
std::vector<std::pair<std::string, uint32_t>> LabelsOf(const Archive& archive, uint32_t sound)
{
    std::vector<std::pair<std::string, uint32_t>> labels;
    for (const sar::Sequence::Label& label :
         sar::SequenceFile::Read(archive->FileData(archive->Sounds()[sound].file_id)).labels)
    {
        labels.emplace_back(label.name, label.offset);
    }

    return labels;
}

// `bytes` after `before`.
std::vector<uint8_t> Joined(std::vector<uint8_t> before, const std::vector<uint8_t>& bytes)
{
    before.insert(before.end(), bytes.begin(), bytes.end());

    return before;
}

// The new sequence's file, with its labels Intro and Main. Replace… reads a file the same way.
Incoming NewFile()
{
    return ReadIncoming(sar_files::Sequence(false, kNewBytecode, {{"Intro", 0}, {"Main", 8}}));
}

// The archive after sound `sound` of `file` plays the new sequence from item `start`.
Archive Replaced(const File& file, uint32_t sound, uint32_t start, const ProgramMap& programs = {})
{
    const Incoming incoming = NewFile();
    const std::string label = start == kMain ? "Main" : "Intro";
    const File replaced = ReplaceSequence(file, PlacesOf(file), sound, incoming.sequence, start, programs, label, 0);
    return Read(hokuto::format::ExportArchive(replaced));
}

// Whether `call` throws std::invalid_argument: the edit refuses.
template <typename Call>
bool Refused(Call call)
{
    try
    {
        call();
    }
    catch (const std::invalid_argument&)
    {
        return true;
    }

    return false;
}

// The message of the refusal that `call` throws, or nothing.
template <typename Call>
std::string RefusalOf(Call call)
{
    try
    {
        call();
    }
    catch (const std::invalid_argument& error)
    {
        return error.what();
    }

    return {};
}

// A file of each console reads into the same commands. Its padding stays out, and its labels name their items. Like the
// Wii U, the 3DS writes its commands' arguments big-endian. Bytes that aren't a sequence file and commands that overlap
// are refused. The refusal names the kind of a file of another kind, such as a wave file. A file without labels is read
// from its first command.
void TestRead()
{
    const std::vector<uint8_t> bytes = sar_files::Sequence(false, kNewBytecode, {{"Intro", 0}, {"Main", 8}});
    const std::vector<uint8_t> wii_u = sar_files::Sequence(true, kWiiUNewBytecode, {{"Intro", 0}, {"Main", 8}});
    const std::vector<uint8_t> ctr =
        sar_files::Sequence(sar::Flavor::kCtr, kWiiUNewBytecode, {{"Intro", 0}, {"Main", 8}});

    const Incoming incoming = ReadIncoming(bytes);
    const Incoming other = ReadIncoming(wii_u);

    const std::vector<Command>& items = incoming.sequence.items;
    XSFLIB_CHECK(incoming.flavor == sar::Flavor::kNx && other.flavor == sar::Flavor::kCafe);
    XSFLIB_CHECK(incoming.size == bytes.size() && other.sequence == incoming.sequence);
    XSFLIB_CHECK(items.size() == 10 && items[kMain].kind == Kind::kBankSelect && items.back().kind == Kind::kReturn);
    XSFLIB_CHECK(incoming.sequence.labels.size() == 2 && incoming.sequence.labels[0].item == kIntro &&
                 incoming.sequence.labels[1].item == kMain);
    XSFLIB_CHECK(Refused([] { ReadIncoming(std::vector<uint8_t>(64, 0x55)); }));
    XSFLIB_CHECK(Refused([] { ReadIncoming(sar_files::Sequence(false, {0x89, 1, 0, 0, 0xFF}, {{"Into", 0}})); }));
    const std::vector<uint8_t> kWave = {'F', 'W', 'A', 'V', 0xFF, 0xFE, 0x40, 0};
    XSFLIB_CHECK(RefusalOf([&kWave] { ReadIncoming(kWave); }) ==
                 "It's a wave file of the Switch, not a sequence file.");
    const Incoming from_ctr = ReadIncoming(ctr);
    XSFLIB_CHECK(from_ctr.flavor == sar::Flavor::kCtr && from_ctr.size == ctr.size());
    XSFLIB_CHECK(from_ctr.sequence == incoming.sequence);

    // A file without labels is read from its first command.
    const Incoming unlabelled = ReadIncoming(sar_files::Sequence(false, kAloneBytecode, {}));
    const std::vector<Command>& alone = unlabelled.sequence.items;
    XSFLIB_CHECK(alone.size() == 3 && alone[0].kind == Kind::kTimebase && alone[1].kind == Kind::kNote &&
                 alone[2].kind == Kind::kEnd && unlabelled.sequence.labels.empty());
}

// Each start selects the programs of its path, with the slot that its track selects. A program from a variable is
// undecided.
void TestPrograms()
{
    Incoming incoming = NewFile();
    const Selections intro = ProgramsSelected(incoming.sequence, kIntro);
    const Selections main = ProgramsSelected(incoming.sequence, kMain);
    SEQLIB_REQUIRE(incoming.sequence.items.size() > kMain + 1);
    incoming.sequence.items[kMain + 1].value = {Source::kVariable, 3, 0};
    const Selections varied = ProgramsSelected(incoming.sequence, kMain);

    XSFLIB_CHECK(intro.programs == (std::vector<Program>{{0, 5}}) && intro.undecided == 0);
    XSFLIB_CHECK(main.programs == (std::vector<Program>{{1, 7}}) && main.undecided == 0);
    XSFLIB_CHECK(varied.programs.empty() && varied.undecided == 1);
}

// A sound alone in its file, with a start in its entry: the new file takes the old one's place, and the sound starts at
// Main. The map changes the program that Main's path selects, and leaves Intro's. The other files stay.
void TestAlone()
{
    const File file = hokuto::format::ImportSwitchArchive(TestArchive());
    const Archive before = Read(hokuto::format::ExportArchive(file));
    std::vector<uint8_t> expected = kNewBytecode;
    expected[11] = 9;

    const Archive after = Replaced(file, kAlone, kMain, {{{1, 7}, 9}, {{0, 5}, 6}, {{1, 8}, 2}});
    const Archive wii_u = Read(hokuto::format::ExportArchive(
        ReplaceSequence(file, PlacesOf(file), kAlone,
                        ReadIncoming(sar_files::Sequence(true, kWiiUNewBytecode, {{"Intro", 0}, {"Main", 8}})).sequence,
                        kMain, {{{1, 7}, 9}}, "Main", 0)));

    XSFLIB_CHECK(after->Sounds()[kAlone].start_offset == 8 && BytecodeOf(after, kAlone) == expected);
    XSFLIB_CHECK(BytecodeOf(wii_u, kAlone) == expected);
    XSFLIB_CHECK((LabelsOf(after, kAlone) == std::vector<std::pair<std::string, uint32_t>>{{"Intro", 0}, {"Main", 8}}));
    XSFLIB_CHECK(BytecodeOf(after, kShared) == BytecodeOf(before, kShared) &&
                 after->Sounds()[kEntry].start_offset == 7);
    XSFLIB_CHECK(BytecodeOf(after, kPlain) == BytecodeOf(before, kPlain));
}

// A program that takes more bytes than the one that it replaces: the commands after it move, and so do the call's
// target and the label. A program change whose value the sequence pads keeps the padding while the value fits in it.
void TestWiderPrograms()
{
    const File file = hokuto::format::ImportSwitchArchive(TestArchive());
    const std::vector<std::optional<SoundPlace>> places = PlacesOf(file);
    const std::vector<uint8_t> kPadded = {0xB0, 48, 0x81, 0x80, 5, 60, 100,  24, 0xFF, 0xB6, 1,
                                          0x81, 7,  0x8A, 18,   0, 0,  0xFF, 62, 100,  24,   0xFD};
    const std::vector<uint8_t> kWide = {0xB0, 48, 0x81, 0x81, 0x48, 60, 100,  24, 0xFF, 0xB6, 1,
                                        0x81, 7,  0x8A, 18,   0,    0,  0xFF, 62, 100,  24,   0xFD};
    const std::vector<uint8_t> kWider = {0xB0, 48, 0x81, 0x81, 0x9C, 0x20, 60,   100, 24,  0xFF, 0xB6, 1,
                                         0x81, 7,  0x8A, 19,   0,    0,    0xFF, 62,  100, 24,   0xFD};
    std::vector<uint8_t> held = kPadded;
    held[4] = 7;
    const Incoming incoming = ReadIncoming(sar_files::Sequence(false, kPadded, {{"Intro", 0}, {"Main", 9}}));
    const auto replaced = [&](int program)
    {
        return Read(hokuto::format::ExportArchive(
            ReplaceSequence(file, places, kAlone, incoming.sequence, kIntro, {{{0, 5}, program}}, "Intro", 0)));
    };

    const Archive after = Replaced(file, kAlone, kIntro, {{{0, 5}, 200}});
    const Archive kept = replaced(7);
    const Archive grown = replaced(20000);

    XSFLIB_CHECK(after->Sounds()[kAlone].start_offset == 0 && BytecodeOf(after, kAlone) == kWide);
    XSFLIB_CHECK((LabelsOf(after, kAlone) == std::vector<std::pair<std::string, uint32_t>>{{"Intro", 0}, {"Main", 9}}));
    XSFLIB_CHECK(BytecodeOf(kept, kAlone) == held && BytecodeOf(grown, kAlone) == kWider);
    XSFLIB_CHECK(
        (LabelsOf(grown, kAlone) == std::vector<std::pair<std::string, uint32_t>>{{"Intro", 0}, {"Main", 10}}));
}

// A sound alone in its file, without a start in its entry: a jump to Main goes in before the new file's commands, and
// the call's target moves past it.
void TestPlain()
{
    const File file = hokuto::format::ImportSwitchArchive(TestArchive());
    std::vector<uint8_t> expected = Joined({0x89, 12, 0, 0}, kNewBytecode);
    expected[4 + 13] = 21;

    const Archive after = Replaced(file, kPlain, kMain);

    XSFLIB_CHECK(after->Sounds()[kPlain].start_offset == 0);
    XSFLIB_CHECK(BytecodeOf(after, kPlain) == expected);
}

// A sound that shares its file, with a start in its entry: Main's path goes after the file's commands, with the call's
// target moved, under the label Main. The other sound keeps its commands.
void TestEntry()
{
    const File file = hokuto::format::ImportSwitchArchive(TestArchive());
    const std::vector<std::optional<SoundPlace>> places = PlacesOf(file);

    const Archive after = Replaced(file, kEntry, kMain);

    XSFLIB_CHECK(SharingFile(places, kEntry) == (std::vector<uint32_t>{kShared, kTwin}));
    XSFLIB_CHECK(SharingStart(places, kEntry).empty());
    XSFLIB_CHECK(after->Sounds()[kEntry].start_offset == 11 && after->Sounds()[kShared].start_offset == 0);
    XSFLIB_CHECK(BytecodeOf(after, kEntry) ==
                 Joined(kSharedBytecode, {0xB6, 1, 0x81, 7, 0x8A, 20, 0, 0, 0xFF, 62, 100, 24, 0xFD}));
    XSFLIB_CHECK((LabelsOf(after, kEntry) ==
                  std::vector<std::pair<std::string, uint32_t>>{{"Shared", 0}, {"Entry", 7}, {"Main", 11}}));
}

// A sound that shares its file and starts where the file starts: a jump to Intro's path goes in first, and the start in
// the other sound's entry moves past it. The sound that starts with it takes the jump too. A label that the file holds
// takes a number.
void TestSharedStart()
{
    const File file = hokuto::format::ImportSwitchArchive(TestArchive());
    const std::vector<std::optional<SoundPlace>> places = PlacesOf(file);
    const Incoming incoming = NewFile();

    const File replaced = ReplaceSequence(file, places, kShared, incoming.sequence, kIntro, {}, "Entry", 0);
    const Archive after = Read(hokuto::format::ExportArchive(replaced));

    XSFLIB_CHECK(SharingStart(places, kShared) == std::vector<uint32_t>{kTwin});
    XSFLIB_CHECK(after->Sounds()[kShared].start_offset == 0 && after->Sounds()[kTwin].start_offset == 0);
    XSFLIB_CHECK(after->Sounds()[kEntry].start_offset == 11);
    XSFLIB_CHECK(BytecodeOf(after, kShared) ==
                 Joined(Joined({0x89, 15, 0, 0}, kSharedBytecode), {0xB0, 48, 0x81, 5, 60, 100, 24, 0xFF}));
    XSFLIB_CHECK((LabelsOf(after, kShared) ==
                  std::vector<std::pair<std::string, uint32_t>>{{"Shared", 4}, {"Entry", 11}, {"Entry_2", 15}}));
}

// New commands that reach a return outside every call: the player runs on past the return, and the commands after it go
// in too.
void TestReturnRunsOn()
{
    const File file = hokuto::format::ImportSwitchArchive(TestArchive());
    const std::vector<uint8_t> kBytecode = {
        0xB0, 48,      // 0: timebase 48
        0xC1, 80,      // 2: volume 80
        0xFD,          // 4: a return outside every call
        0x81, 5,       // 5: program 5
        60,   100, 24, // 7: key 60 for 24 ticks
        0xFF,          // 10: the end
    };
    const Incoming incoming = ReadIncoming(sar_files::Sequence(false, kBytecode, {{"RunOn", 0}}));
    const File replaced = ReplaceSequence(file, PlacesOf(file), kEntry, incoming.sequence, 0, {}, "RunOn", 0);
    const Archive after = Read(hokuto::format::ExportArchive(replaced));

    XSFLIB_CHECK(after->Sounds()[kEntry].start_offset == 11);
    XSFLIB_CHECK(BytecodeOf(after, kEntry) == Joined(kSharedBytecode, kBytecode));
}

// Each sound's file goes back in its place from its start, and the archive comes back byte for byte.
void TestSameFile()
{
    const File file = hokuto::format::ImportSwitchArchive(TestArchive());
    const std::vector<uint8_t> original = hokuto::format::ExportArchive(file);
    const Archive archive = Read(original);
    const std::vector<std::optional<SoundPlace>> places = PlacesOf(file);
    const std::vector<uint32_t> kStarts = {0, 3, 0, 0, 0};

    for (uint32_t sound = 0; sound < kStarts.size(); sound++)
    {
        const std::span<const uint8_t> bytes = archive->FileData(archive->Sounds()[sound].file_id);
        const Incoming incoming = ReadIncoming(bytes);

        const File replaced = ReplaceSequence(file, places, sound, incoming.sequence, kStarts[sound], {}, "Same", 0);

        XSFLIB_CHECK(hokuto::format::ExportArchive(replaced) == original);
    }
}

// A file with the commands of a sequence of the archive gives the starts of the sounds that play the sequence: from the
// archive, with other labels, and from the Wii U. A file with other commands gives none.
void TestSoundStarts()
{
    const File file = hokuto::format::ImportSwitchArchive(TestArchive());
    const Archive archive = Read(hokuto::format::ExportArchive(file));
    const std::vector<std::optional<SoundPlace>> places = PlacesOf(file);
    const auto starts = [&](const std::vector<uint8_t>& bytes)
    {
        return SoundStarts(file, places, ReadIncoming(bytes).sequence);
    };

    const std::span<const uint8_t> shared = archive->FileData(archive->Sounds()[kShared].file_id);
    const std::span<const uint8_t> alone = archive->FileData(archive->Sounds()[kAlone].file_id);
    const std::vector<StartingSound> kSharing = {{kShared, 0}, {kEntry, 3}, {kTwin, 0}};

    const std::vector<StartingSound> from_archive = starts({shared.begin(), shared.end()});
    const std::vector<StartingSound> relabelled = starts(sar_files::Sequence(false, kSharedBytecode, {{"Other", 0}}));
    const std::vector<StartingSound> from_wii_u =
        starts(sar_files::Sequence(true, kWiiUSharedBytecode, {{"Shared", 0}}));
    const std::vector<StartingSound> from_alone = starts({alone.begin(), alone.end()});
    const std::vector<StartingSound> from_new = SoundStarts(file, places, NewFile().sequence);

    XSFLIB_CHECK(from_archive == kSharing);
    XSFLIB_CHECK(relabelled == kSharing);
    XSFLIB_CHECK(from_wii_u == kSharing);
    XSFLIB_CHECK((from_alone == std::vector<StartingSound>{{kAlone, 0}}));
    XSFLIB_CHECK(from_new.empty());
}

// The sound allocates the tracks that the new commands open and its old commands don't, and so does a sound that starts
// at the jump with it. The other sounds keep their tracks. A track that the sound's old commands open and the sound
// leaves out stays out. Without the archive's tables, no track can be added. A sound past the archive's sounds is
// refused.
void TestTracks()
{
    const File file = hokuto::format::ImportSwitchArchive(TestArchive());
    const std::vector<std::optional<SoundPlace>> places = PlacesOf(file);
    const std::vector<uint8_t> kBytecode = {
        0xFE, 0x05, 0x00,       // 0: allocate tracks 0 and 2
        0x88, 3,    9,    0, 0, // 3: open track 3 at 9
        0xFF,                   // 8: the end
        60,   100,  24,         // 9: key 60 for 24 ticks
        0xFF,                   // 12: the end
    };
    const std::vector<uint8_t> kMore = {
        0x88, 4,   10, 0, 0, // 0: open track 4 at 10
        0x88, 3,   10, 0, 0, // 5: open track 3 at 10
        60,   100, 24,       // 10: key 60 for 24 ticks
        0xFF,                // 13: the end
    };
    const Incoming incoming = ReadIncoming(sar_files::Sequence(false, kBytecode, {{"Tracks", 0}}));
    const Incoming more_tracks = ReadIncoming(sar_files::Sequence(false, kMore, {{"More", 0}}));
    const uint32_t added = hokuto::edit::TracksToAdd(file, places, kAlone, incoming.sequence, 0);
    File bare = file;
    bare.native.Edit([](hokuto::format::NativeArchive& native) { native.tables.reset(); });

    // A sound that leaves out tracks 2 and 3 of the commands that it plays. The commands of a sound with a start in its
    // entry count from that start.
    File muted = ReplaceSequence(file, places, kAlone, incoming.sequence, 0, {}, "Tracks", 0);
    const uint32_t more_added = hokuto::edit::TracksToAdd(muted, places, kAlone, more_tracks.sequence, 0);
    const File entry = ReplaceSequence(file, places, kEntry, incoming.sequence, 0, {}, "Tracks", 0);
    const uint32_t entry_added = hokuto::edit::TracksToAdd(file, places, kEntry, incoming.sequence, 0);
    const uint32_t entry_again = hokuto::edit::TracksToAdd(entry, places, kEntry, incoming.sequence, 0);

    const Archive alone = Read(hokuto::format::ExportArchive(
        ReplaceSequence(file, places, kAlone, incoming.sequence, 0, {}, "Tracks", added)));
    const Archive shared = Read(hokuto::format::ExportArchive(
        ReplaceSequence(file, places, kShared, incoming.sequence, 0, {}, "Tracks",
                        hokuto::edit::TracksToAdd(file, places, kShared, incoming.sequence, 0))));
    const Archive layered = Read(hokuto::format::ExportArchive(
        ReplaceSequence(muted, places, kAlone, more_tracks.sequence, 0, {}, "More", more_added)));

    XSFLIB_CHECK(hokuto::edit::TracksOpened(incoming.sequence, 0) == 0b1101);
    XSFLIB_CHECK(hokuto::edit::TracksOpened(NewFile().sequence, kMain) == 1);
    XSFLIB_CHECK(added == 0b1100 && more_added == 0b10000);
    XSFLIB_CHECK(entry_added == 0b1100 && entry_again == 0);
    XSFLIB_CHECK(alone->Sounds()[kAlone].allocate_track_flags == 0b1101);
    XSFLIB_CHECK(alone->Sounds()[kPlain].allocate_track_flags == 1);
    XSFLIB_CHECK(shared->Sounds()[kShared].allocate_track_flags == 0b1101);
    XSFLIB_CHECK(shared->Sounds()[kTwin].allocate_track_flags == 0b1101);
    XSFLIB_CHECK(shared->Sounds()[kEntry].allocate_track_flags == 1);
    XSFLIB_CHECK(layered->Sounds()[kAlone].allocate_track_flags == 0b10001);
    XSFLIB_CHECK(Refused([&] { ReplaceSequence(bare, places, kAlone, incoming.sequence, 0, {}, "Tracks", added); }));
    XSFLIB_CHECK(!Refused([&] { ReplaceSequence(bare, places, kAlone, NewFile().sequence, kMain, {}, "Main", 0); }));
    XSFLIB_CHECK(Refused([&] { hokuto::edit::TracksToAdd(file, places, 7, incoming.sequence, 0); }));
}

// The sound and each sound that starts at the jump with it take the bank slots. The other sounds keep theirs. A list of
// a new length requires an archive laid out as NintendoWare's tools lay one out. The entries of such an archive always
// hold a start. No sound there therefore starts at the jump with another. More than four slots, a slot that holds no
// bank of the archive and a file without the archive's tables are refused.
void TestBankSlots()
{
    sar_files::ArchiveSpec spec = TestSpec();
    spec.banks = {{"BANK_A", 3}, {"BANK_B", 3}};
    spec.files.push_back({std::vector<uint8_t>(16, 0xBA), "", false});
    sar_files::ArchiveSpec listed = spec;
    for (sar_files::SoundSpec& sound : listed.sounds)
    {
        sound.banks = {0};
    }
    const File tools = hokuto::format::ImportSwitchArchive(sar_tools::Archive(sar::Flavor::kNx, spec));
    const File file = hokuto::format::ImportSwitchArchive(sar_files::Archive(false, listed));
    const std::vector<std::optional<SoundPlace>> tools_places = PlacesOf(tools);
    const std::vector<std::optional<SoundPlace>> places = PlacesOf(file);
    const Incoming incoming = NewFile();
    const uint32_t kFirstBank = sar::ItemId(sar::kBankItem, 0);
    const uint32_t kSecondBank = sar::ItemId(sar::kBankItem, 1);
    const std::vector<uint32_t> kSlots = {sar::kNoItem, kSecondBank};

    const auto refused = [&](const File& in, const std::vector<uint32_t>& banks)
    {
        return Refused([&] { ReplaceSequence(in, places, kShared, incoming.sequence, kIntro, {}, "Intro", 0, banks); });
    };

    File bare = file;
    bare.native.Edit([](hokuto::format::NativeArchive& native) { native.tables.reset(); });

    const Archive longer = Read(hokuto::format::ExportArchive(
        ReplaceSequence(tools, tools_places, kShared, incoming.sequence, kIntro, {}, "Intro", 0, kSlots)));
    const Archive shared = Read(hokuto::format::ExportArchive(
        ReplaceSequence(file, places, kShared, incoming.sequence, kIntro, {}, "Intro", 0, {{kSecondBank}})));

    XSFLIB_CHECK(longer->Sounds()[kShared].banks == kSlots && longer->Sounds()[kTwin].banks.empty());
    XSFLIB_CHECK(BytecodeOf(longer, kShared) == Joined(kSharedBytecode, {0xB0, 48, 0x81, 5, 60, 100, 24, 0xFF}));
    XSFLIB_CHECK(shared->Sounds()[kShared].banks == std::vector<uint32_t>{kSecondBank});
    XSFLIB_CHECK(shared->Sounds()[kTwin].banks == std::vector<uint32_t>{kSecondBank});
    XSFLIB_CHECK(shared->Sounds()[kEntry].banks == std::vector<uint32_t>{kFirstBank});
    XSFLIB_CHECK(!refused(file, {}));
    XSFLIB_CHECK(refused(file, std::vector<uint32_t>(5, sar::kNoItem)));
    XSFLIB_CHECK(refused(file, {sar::ItemId(sar::kBankItem, 2)}));
    XSFLIB_CHECK(refused(file, {sar::ItemId(sar::kPlayerItem, 0)}));
    XSFLIB_CHECK(refused(bare, kSlots));
}

// A field of the tables that names the end of the sound's sequence names the new end. A field can't name another
// command of a sequence whose commands are all new, or a distance from one.
void TestPlaces()
{
    const File file = hokuto::format::ImportSwitchArchive(TestArchive());
    const std::vector<std::optional<SoundPlace>> places = PlacesOf(file);
    const Incoming incoming = NewFile();
    SEQLIB_REQUIRE(places.size() > kAlone && places[kAlone] && places[kAlone]->start_field &&
                   *places[kAlone]->start_field < file.native->fields.size());
    const uint32_t sequence = places[kAlone]->sequence;
    ntdlib::sar::Field end = file.native->fields[*places[kAlone]->start_field];
    end.to.offset = static_cast<uint32_t>(file.sequences[sequence]->items.size());
    ntdlib::sar::Field inside = end;
    inside.to.offset = 1;
    ntdlib::sar::Field from_inside = end;
    from_inside.from.offset = 1;

    const auto with = [&file](const ntdlib::sar::Field& field)
    {
        File changed = file;
        changed.native.Edit([&field](hokuto::format::NativeArchive& native) { native.fields.push_back(field); });

        return changed;
    };

    const File replaced = ReplaceSequence(with(end), places, kAlone, incoming.sequence, kMain, {}, "Main", 0);

    XSFLIB_CHECK(replaced.native->fields.back().to.offset == replaced.sequences[sequence]->items.size());
    XSFLIB_CHECK(
        Refused([&] { ReplaceSequence(with(inside), places, kAlone, incoming.sequence, kMain, {}, "In", 0); }));
    XSFLIB_CHECK(
        Refused([&] { ReplaceSequence(with(from_inside), places, kAlone, incoming.sequence, kMain, {}, "In", 0); }));
}

// A start past the end, a sound past the archive's sounds, and new commands that run into bytes in a shared file or
// behind a jump. Then a shared file whose targets are byte offsets, and a program that the console's bytecode can't
// hold.
void TestRefusals()
{
    const File file = hokuto::format::ImportSwitchArchive(TestArchive());
    const std::vector<std::optional<SoundPlace>> places = PlacesOf(file);
    const Incoming incoming = NewFile();
    SEQLIB_REQUIRE(incoming.sequence.items.size() > kMain + 3);
    ntdlib::commands::Sequence data = incoming.sequence;
    data.items[kMain + 3] = Command{.kind = Kind::kData, .bytes = {0}};
    File fixed = file;
    ntdlib::commands::Sequence outside = *file.sequences[0];
    outside.items.push_back(Command{.kind = Kind::kJump, .target = 64, .target_outside = true});
    fixed.sequences[0] = outside;
    ntdlib::commands::Sequence wide = incoming.sequence;
    wide.items[1].value.value = (int64_t{1} << 35) * kOne;

    XSFLIB_CHECK(Refused([&] { ReplaceSequence(file, places, kAlone, incoming.sequence, 10, {}, "Past", 0); }));
    XSFLIB_CHECK(Refused([&] { ReplaceSequence(file, places, 7, incoming.sequence, kMain, {}, "None", 0); }));
    XSFLIB_CHECK(Refused([&] { ReplaceSequence(file, places, kEntry, data, kMain, {}, "Data", 0); }));
    XSFLIB_CHECK(Refused([&] { ReplaceSequence(file, places, kPlain, data, kMain, {}, "Data", 0); }));
    XSFLIB_CHECK(!Refused([&] { ReplaceSequence(file, places, kAlone, data, kMain, {}, "Data", 0); }));
    XSFLIB_CHECK(Refused([&] { ReplaceSequence(fixed, places, kEntry, incoming.sequence, kMain, {}, "Fixed", 0); }));
    XSFLIB_CHECK(Refused([&] { ReplaceSequence(file, places, kAlone, wide, kIntro, {}, "Wide", 0); }));
}

// The new sequence's bytecode as the Wii holds it. NW4R SND has no bank select. Intro starts at 0 and Main at 8.
const std::vector<uint8_t> kWiiNewBytecode = {
    0xB0, 48,          // 0: timebase 48
    0x81, 5,           // 2: program 5
    60,   100, 24,     // 4: key 60 for 24 ticks
    0xFF,              // 7: the end
    0x81, 7,           // 8: program 7
    0x8A, 0,   0,  15, // 10: call 15
    0xFF,              // 14: the end
    62,   100, 24,     // 15: key 62 for 24 ticks
    0xFD,              // 18: return
};

// A sequence file of the Wii with `bytecode` and `labels`.
std::vector<uint8_t> WiiSequence(const std::vector<uint8_t>& bytecode,
                                 std::vector<ntdlib::rsar::Sequence::Label> labels)
{
    ntdlib::rsar::SequenceFile file;
    file.data = bytecode;
    file.labels = std::move(labels);

    return file.Write();
}

// Where each sound of `file`, a file of the Wii, plays (edit::SoundPlaces), from the archive that the file builds.
std::vector<std::optional<SoundPlace>> WiiPlacesOf(const File& file)
{
    const std::vector<uint8_t> built = hokuto::format::ExportArchive(file);
    const std::shared_ptr<const ntdlib::rsar::SoundArchive> read =
        hokuto::song::LoadWiiArchive(built, "test.brsar", ntdlib::rvl::snd::kRsarOptions);
    return hokuto::edit::SoundPlaces(*read, hokuto::format::SequencesOfFiles(file, built, *read),
                                     hokuto::format::StartFieldsOfSounds(file, built));
}

// A sequence of the Switch that reads global variable 20 before it sets it. It picks program 1 if the variable holds
// -1, and plays key 60.
const std::vector<uint8_t> kGlobalBytecode = {
    0xB0, 48,                   // 0: timebase 48
    0xF0, 0x90, 20, 0xFF, 0xFF, // 2: whether variable 20 holds -1
    0xA2, 0x81, 1,              // 7: if so, program 1
    60,   100,  24,             // 10: key 60 for 24 ticks
    0xFF,                       // 13: the end
};

// A sequence of the Switch whose volume and volume2 move over 48 ticks, then a note.
const std::vector<uint8_t> kTimedBytecode = {
    0xB0, 48,               // 0: timebase 48
    0xA3, 0xC1, 100, 48, 0, // 2: volume 100 over 48 ticks
    0xA3, 0xD5, 90,  48, 0, // 7: volume2 90 over 48 ticks
    60,   100,  24,         // 12: key 60 for 24 ticks
    0xFF,                   // 15: the end
};

// A sequence file of the Wii takes the place of the sequence of the one sound of a Wii archive. The commands come in by
// the rules of the archive's build of NW4R SND. No other sound plays the file. The new file therefore takes the old
// one's place, and the sound starts at Main. A sequence file of the Wii is refused without a build. One of the Switch
// goes in converted. Its bank select stays out, and the new file holds the Wii's bytecode. One with bytes that no
// command reaches is refused: its commands have to keep their places. A converted sound that reads a global variable
// before it sets it takes commands at its start. They set the variable to the later consoles' starting value of -1
// while it holds the Wii's 0. NW4R SND sets volume2 at once, and its builds before March 2008 lack the time prefixes: a
// volume that moves over time takes its value at once there. A sound of the Wii has one bank slot.
void TestWii()
{
    const std::vector<uint8_t> archive = ntdlib::testing::rsar_tools::MakeArchive(
        0x0104, {{WiiSequence(kAloneBytecode, {{"SEQ_SONG", 0}})}}, {{"SEQ_SONG", 0, 0}}, {{0}});
    const File file = hokuto::format::ImportWiiArchive(archive);
    const hokuto::song::Console build{.wii = hokuto::format::BuildOfWiiArchive(*file.native)};
    const std::vector<uint8_t> bytes = WiiSequence(kWiiNewBytecode, {{"Intro", 0}, {"Main", 8}});
    const Incoming incoming = ReadIncoming(bytes, build);

    SEQLIB_REQUIRE(incoming.wii && incoming.size == bytes.size() && incoming.sequence.items.size() == 9);
    XSFLIB_CHECK(incoming.sequence.labels.size() == 2 && incoming.sequence.labels[1].item == kMain);
    XSFLIB_CHECK(incoming.sequence.items[kMain + 1].kind == Kind::kCall);
    XSFLIB_CHECK(Refused([&bytes] { ReadIncoming(bytes); }));
    const std::string refusal =
        RefusalOf([&build] { ReadIncoming(sar_files::Sequence(false, kNewBytecode, {{"Intro", 0}}), build); });
    XSFLIB_CHECK(refusal ==
                 "The sequence file's commands have to keep their places. Hokuto can't put such a sequence "
                 "into an archive of the Wii yet.");

    // The Switch's sequence, converted.
    const Incoming later = ReadIncoming(sar_files::Sequence(false, kNewBytecode, {{"Intro", 0}, {"Main", 8}}), build);
    XSFLIB_CHECK(later.wii && later.converted && later.flavor == sar::Flavor::kNx && later.warnings.size() == 1);
    XSFLIB_CHECK(later.sequence.items.size() == 9 && later.sequence.labels.size() == 2 &&
                 later.sequence.labels[1].item == kMain && later.sequence.items[kMain + 1].kind == Kind::kCall);
    XSFLIB_CHECK(!incoming.converted && incoming.warnings.empty());

    // A global variable of the converted sequence.
    Incoming global = ReadIncoming(sar_files::Sequence(false, kGlobalBytecode, {{"Song", 0}}), build);
    XSFLIB_CHECK(global.converted && global.warnings.empty() && global.sequence.items.size() == 5);
    const auto [start, globals] = hokuto::edit::WithLaterGlobals(global.sequence, 0);
    SEQLIB_REQUIRE(start == 0 && (globals == std::vector<int>{20}) && global.sequence.items.size() == 8);
    const Command& compare = global.sequence.items[0];
    const Command& set = global.sequence.items[1];
    XSFLIB_CHECK(compare.kind == Kind::kCompareEqual && compare.index == 20 && compare.value.value == 0);
    XSFLIB_CHECK(set.kind == Kind::kSetVariable && set.conditional && set.index == 20 && set.value.value == -kOne);
    XSFLIB_CHECK(global.sequence.items[3].kind == Kind::kTimebase);
    const std::vector<uint8_t> timed = sar_files::Sequence(false, kTimedBytecode, {{"Song", 0}});
    const Incoming june = ReadIncoming(timed, build);
    const Incoming older = ReadIncoming(timed, hokuto::song::Console{.wii = ntdlib::SndBuild{.date = 20070608}});
    XSFLIB_CHECK(june.sequence.items.size() == 5 && june.sequence.items[1].kind == Kind::kVolume &&
                 june.sequence.items[1].time && june.sequence.items[2].kind == Kind::kVolume2 &&
                 !june.sequence.items[2].time && june.warnings.size() == 1);
    XSFLIB_CHECK(older.sequence.items.size() == 5 && !older.sequence.items[1].time && !older.sequence.items[2].time);
    XSFLIB_CHECK(older.warnings.size() == 2 &&
                 older.warnings[1] ==
                     "NW4R SND's builds before March 2008 have no times for settings. 1 other "
                     "setting of the sequence loses its time, and changes at once.");

    // Without volume2's warning before it, the warning of the older builds counts no "other" setting.
    const std::vector<uint8_t> volume_only(kTimedBytecode.begin(), kTimedBytecode.begin() + 7);
    std::vector<uint8_t> volume_bytecode = volume_only;
    volume_bytecode.insert(volume_bytecode.end(), kTimedBytecode.begin() + 12, kTimedBytecode.end());
    const Incoming volume = ReadIncoming(sar_files::Sequence(false, volume_bytecode, {{"Song", 0}}),
                                         hokuto::song::Console{.wii = ntdlib::SndBuild{.date = 20070608}});
    XSFLIB_CHECK(volume.warnings.size() == 1 &&
                 volume.warnings[0] ==
                     "NW4R SND's builds before March 2008 have no times for settings. 1 setting of the sequence loses "
                     "its time, and changes at once.");
    XSFLIB_CHECK(hokuto::edit::LaterGlobalsWarning(globals) ==
                 "The sound reads global variable 20 before it sets it. NW4R SND starts the global variables at 0, and "
                 "the later consoles start them at -1. Commands at the sound's start set it to -1 while it holds 0.");

    const File replaced = ReplaceSequence(file, WiiPlacesOf(file), 0, incoming.sequence, kMain, {}, "Main", 0);
    const std::shared_ptr<const ntdlib::rsar::SoundArchive> after = hokuto::song::LoadWiiArchive(
        hokuto::format::ExportArchive(replaced), "test.brsar", ntdlib::rvl::snd::kRsarOptions);
    const ntdlib::rsar::SequenceFile written = ntdlib::rsar::SequenceFile::Read(after->FileData(0));
    std::vector<uint8_t> data = written.data;
    while (!data.empty() && data.back() == 0)
    {
        data.pop_back();
    }
    XSFLIB_CHECK(data == kWiiNewBytecode && written.labels.size() == 2 && written.labels[1].name == "Main" &&
                 written.labels[1].offset == 8);
    XSFLIB_CHECK(after->Sounds()[0].start_offset == 8);

    // The converted sequence gives the same file.
    const File converted = ReplaceSequence(file, WiiPlacesOf(file), 0, later.sequence, kMain, {}, "Main", 0);
    XSFLIB_CHECK(std::ranges::equal(hokuto::song::LoadWiiArchive(hokuto::format::ExportArchive(converted), "test.brsar",
                                                                 ntdlib::rvl::snd::kRsarOptions)
                                        ->FileData(0),
                                    after->FileData(0)));
    XSFLIB_CHECK(Refused(
        [&]
        {
            ReplaceSequence(file, WiiPlacesOf(file), 0, incoming.sequence, kIntro, {}, "Intro", 0,
                            std::vector<uint32_t>{sar::kNoItem, sar::kNoItem});
        }));
}

// A sequence of the Switch that sweeps from a pitch of 1.5625 cents: a 64th of a semitone.
const std::vector<uint8_t> kSweepBytecode = {
    0xB0, 48,      // 0: timebase 48
    0xE3, 1,   0,  // 2: a sweep from 1/64 of a semitone
    60,   100, 24, // 5: key 60 for 24 ticks
    0xFF,          // 8: the end
};

// A sweep of the Switch goes into a file of the Wii whose build keeps a sweep's pitch in 32nds of a semitone: an
// archive of version 1.3 takes a build of 2007. The pitch of a 64th takes the build's nearest pitch, a 32nd. The
// encoder writes the same byte for both pitches. The replaced file then opens again from its text form, and its
// archive holds the same bytes as with the pitch of a 64th.
void TestWiiSweep()
{
    const std::vector<uint8_t> archive = ntdlib::testing::rsar_tools::MakeArchive(
        0x0103, {{WiiSequence(kAloneBytecode, {{"SEQ_SONG", 0}})}}, {{"SEQ_SONG", 0, 0}}, {{0}});
    const File file = hokuto::format::ImportWiiArchive(archive);
    const hokuto::song::Console build{.wii = hokuto::format::BuildOfWiiArchive(*file.native)};

    const Incoming incoming = ReadIncoming(sar_files::Sequence(false, kSweepBytecode, {{"Song", 0}}), build);
    const File replaced = ReplaceSequence(file, WiiPlacesOf(file), 0, incoming.sequence, 0, {}, "Song", 0);
    SEQLIB_REQUIRE(incoming.sequence.items.size() > 1);
    ntdlib::commands::Sequence finer = incoming.sequence;
    finer.items[1].value.value = 100 * kOne / 64;
    const File unrounded = ReplaceSequence(file, WiiPlacesOf(file), 0, finer, 0, {}, "Song", 0);

    const auto reopens = [](const File& saved)
    {
        try
        {
            return hokuto::format::ReadText(hokuto::format::WriteText(saved)).sequences == saved.sequences;
        }
        catch (const std::exception&)
        {
            return false;
        }
    };

    const std::vector<uint8_t> exported = hokuto::format::ExportArchive(replaced);
    const std::vector<uint8_t> data =
        ntdlib::rsar::SequenceFile::Read(hokuto::song::LoadWiiArchive(exported, "test.brsar")->FileData(0)).data;

    XSFLIB_CHECK(!build.wii->SinceMarch2008());
    XSFLIB_CHECK(incoming.sequence.items.size() == 4 && incoming.sequence.items[1].kind == Kind::kSweepPitch);
    XSFLIB_CHECK(incoming.sequence.items[1].value.value == 100 * kOne / 32);
    XSFLIB_CHECK(reopens(replaced));
    XSFLIB_CHECK(exported == hokuto::format::ExportArchive(unrounded));
    XSFLIB_CHECK(data.size() >= 5 && data[2] == 0xE3 && data[3] == 0 && data[4] == 1);
}

// A file of another console takes the biquad filters that come nearest on the archive's console (song::MapBiquads),
// with a warning. A high-pass of the Switch goes into an archive of the Wii U at the Wii U's nearest value. The file
// that Export… writes for the Wii U goes into an archive of the Switch with type 7, the same filter made for 48 kHz. A
// file of the archive's console keeps its filters. The file for the Wii U offers the starts of the sounds that play the
// Switch's file, the sound that starts where no label names it included. A file with another command offers none.
void TestBiquads()
{
    const std::vector<uint8_t> kBytecode = {
        0xB0, 48,      // 0: timebase 48
        0xB4, 2,       // 2: a high-pass
        0xB5, 70,      // 4: of value 70
        60,   100, 24, // 6: key 60 for 24 ticks
        0xFF,          // 9: the end
    };
    sar_files::SoundSpec song;
    song.name = "SEQ_SONG";
    sar_files::SoundSpec note = song;
    note.name = "SEQ_NOTE";
    note.start_offset = 6;
    sar_files::ArchiveSpec spec;
    spec.sounds = {song, note};
    spec.players = {{"PLAYER_TEST", 1}};
    const std::vector<uint8_t> nx = sar_files::Sequence(false, kBytecode, {{"Song", 0}});
    spec.files = {{.bytes = nx}};
    const File file = hokuto::format::ImportSwitchArchive(sar_files::Archive(false, spec));
    const Archive archive = Read(hokuto::format::ExportArchive(file));
    const std::vector<uint8_t> for_wii_u = hokuto::song::OtherConsoleSequenceFile(*archive, 0, sar::Flavor::kCafe).file;
    const hokuto::song::Console kOnSwitch{.flavor = sar::Flavor::kNx};
    const hokuto::song::Console kOnWiiU{.flavor = sar::Flavor::kCafe};
    const int64_t nearest = hokuto::song::NearestBiquadValue(2, 70, kOnWiiU) * kOne;

    const Incoming there = ReadIncoming(nx, kOnWiiU);
    const Incoming back = ReadIncoming(for_wii_u, kOnSwitch);
    const Incoming same = ReadIncoming(nx, kOnSwitch);

    const std::vector<Command>& items = there.sequence.items;
    SEQLIB_REQUIRE(items.size() == 5 && back.sequence.items.size() > 2 && same.sequence.items.size() > 2);
    XSFLIB_CHECK(items[1].kind == Kind::kBiquadType && items[1].value.value == 2 * kOne);
    XSFLIB_CHECK(items[2].kind == Kind::kBiquadValue && items[2].value.value == nearest && nearest != 70 * kOne);
    XSFLIB_CHECK(there.warnings.size() == 1 &&
                 there.warnings[0].ends_with(" 1 setting of the value of a filter of types 1 to 5 took the "
                                             "value whose passband on the Wii U lies nearest."));
    XSFLIB_CHECK(back.sequence.items[1].value.value == 7 * kOne && back.sequence.items[2].value.value == nearest);
    XSFLIB_CHECK(back.warnings.size() == 1 &&
                 back.warnings[0].ends_with(" 1 setting of types 1 to 5 took types 6 to 10."));
    XSFLIB_CHECK(same.warnings.empty() && same.sequence == ReadIncoming(nx).sequence);
    XSFLIB_CHECK(same.sequence.items[2].value.value == 70 * kOne);

    const std::vector<std::optional<SoundPlace>> places = PlacesOf(file);
    XSFLIB_CHECK((SoundStarts(file, places, back.sequence) == std::vector<StartingSound>{{0, 0}, {1, 3}}));
    ntdlib::commands::Sequence other = back.sequence;
    other.items[0].value.value = 96 * kOne;
    XSFLIB_CHECK(SoundStarts(file, places, other).empty());
}

} // namespace

int main()
{
    TestRead();
    TestPrograms();
    TestAlone();
    TestWiderPrograms();
    TestPlain();
    TestEntry();
    TestSharedStart();
    TestReturnRunsOn();
    TestSameFile();
    TestSoundStarts();
    TestTracks();
    TestBankSlots();
    TestPlaces();
    TestRefusals();
    TestBiquads();
    TestWii();
    TestWiiSweep();

    return xsflib::testing::Summary("replace");
}
