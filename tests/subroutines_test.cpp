// SPDX-License-Identifier: MIT

// Tests the edits of a program's labels and subroutines (src/edit/subroutines.h). A small Switch archive holds one
// sequence file. Its sound's track 0 opens track 1, plays key 60, calls a subroutine that plays key 67, plays key 62,
// calls the subroutine again and plays key 64. Track 1 sets its pan and plays no notes. Each edit goes through an
// export of the archive. ntdlib's performer then plays the edited archive, and the notes have to show the edit and
// nothing else.

#include "edit/subroutines.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/testing/sar_files.h"
#include "xsflib/testing/check.h"

#include "edit/commands.h"
#include "format/archive.h"
#include "format/file.h"
#include "song/archive.h"
#include "song/sequence.h"

using hokuto::edit::AddLabel;
using hokuto::edit::CallsTo;
using hokuto::edit::ChangeCommand;
using hokuto::edit::CommandAt;
using hokuto::edit::CommandPlace;
using hokuto::edit::CopySubroutine;
using hokuto::edit::DissolveCall;
using hokuto::edit::Edited;
using hokuto::edit::LabelPlace;
using hokuto::edit::LabelRefusal;
using hokuto::edit::LabelsOf;
using hokuto::edit::MoveLabel;
using hokuto::edit::RemoveLabel;
using hokuto::edit::RenameLabel;
using hokuto::edit::ToSubroutine;
using hokuto::format::File;
using hokuto::song::Note;
using hokuto::song::Sequence;
using ntdlib::commands::Command;
using ntdlib::commands::Kind;

namespace
{

namespace sar = ntdlib::sar;
namespace sar_files = ntdlib::testing::sar_files;

// The offsets of the commands that the tests edit.
constexpr uint32_t kVolume = 15;
constexpr uint32_t kKey60 = 17;
constexpr uint32_t kWait48 = 20;
constexpr uint32_t kFirstCall = 22;
constexpr uint32_t kKey62 = 26;
constexpr uint32_t kSecondCall = 31;
constexpr uint32_t kTrack1 = 41;
constexpr uint32_t kSub = 48;
constexpr uint32_t kSubWait = 51;
constexpr uint32_t kReturn = 53;
constexpr uint32_t kEnd = 54;

// The bytecode. Each command's offset comes first in its comment. The Switch's sequences give arguments in
// little-endian order.
const std::vector<uint8_t> kBytecode = {
    0xFE, 0x03, 0x00,             // 0: allocate tracks 0 and 1
    0x88, 0x01, 41,   0x00, 0x00, // 3: open track 1 at 41
    0xB0, 48,                     // 8: timebase 48
    0xE1, 120,  0x00,             // 10: tempo 120
    0xC7, 0x00,                   // 13: note wait off
    0xC1, 100,                    // 15: volume 100
    60,   100,  24,               // 17: key 60 for 24 ticks, at tick 0
    0x80, 48,                     // 20: wait 48 ticks
    0x8A, 48,   0x00, 0x00,       // 22: call 48, at tick 48
    62,   100,  24,               // 26: key 62 for 24 ticks, at tick 72
    0x80, 24,                     // 29: wait 24 ticks
    0x8A, 48,   0x00, 0x00,       // 31: call 48, at tick 96
    64,   100,  12,               // 35: key 64 for 12 ticks, at tick 120
    0x80, 24,                     // 38: wait 24 ticks
    0xFF,                         // 40: the end
    0xC7, 0x00,                   // 41: track 1: note wait off
    0xC0, 64,                     // 43: pan 64, the centre
    0x80, 96,                     // 45: wait 96 ticks
    0xFF,                         // 47: the end
    67,   100,  6,                // 48: the subroutine: key 67 for 6 ticks
    0x80, 24,                     // 51: wait 24 ticks
    0xFD,                         // 53: return
};

// The labels in the order of their names. NintendoWare's tools write them in that order.
const std::vector<std::pair<std::string, uint32_t>> kLabels = {{"Song", 0}, {"Sub", kSub}, {"Track1", kTrack1}};

// The archive. Its sound starts at the first command, and its entry gives the start. Its bank plays a looped wave for
// every key.
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
    bank.instruments = {keys};

    sar_files::SoundSpec song;
    song.name = "SEQ_SONG";
    song.volume = 127;
    song.banks = {0};
    song.start_offset = 0;
    song.allocate_track_flags = 3;

    sar_files::ArchiveSpec spec;
    spec.sounds = {song};
    spec.banks = {{"BANK_TEST", 1}};
    spec.wave_archives = {{"WARC_TEST", 2}};
    spec.players = {{"PLAYER_TEST", 1}};
    spec.files = {{sar_files::Sequence(false, kBytecode, kLabels), "", false},
                  {sar_files::Bank(false, bank), "", false},
                  {sar_files::WaveArchive(false, {sar_files::Wave(false, wave)}), "", false}};

    return sar_files::Archive(false, spec);
}

// ntdlib's reading of the archive that `file` builds.
std::shared_ptr<const sar::SoundArchive> Exported(const File& file)
{
    return hokuto::song::LoadArchive(hokuto::format::ExportArchive(file), "test.bfsar");
}

// The sound as ntdlib's performer plays it from the archive that `file` builds.
Sequence Played(const File& file)
{
    return hokuto::song::ReadSequence(*Exported(file), 0);
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

// The notes that the archive plays before an edit.
const std::vector<std::vector<uint32_t>> kNotes = {
    {0, 24, 60}, {48, 6, 67}, {72, 24, 62}, {96, 6, 67}, {120, 12, 64},
};

// The labels of the file that the export of `file` gives back.
std::vector<LabelPlace> LabelsAfterExport(const File& file)
{
    return LabelsOf(hokuto::format::ImportSwitchArchive(hokuto::format::ExportArchive(file)), 0);
}

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

// A label's name is an identifier that no other label has.
void TestLabelNames()
{
    const File file = hokuto::format::ImportSwitchArchive(TestArchive());
    const ntdlib::commands::Sequence& commands = *file.sequences.at(0);

    XSFLIB_CHECK(LabelRefusal(commands, "") && LabelRefusal(commands, "2nd") && LabelRefusal(commands, "a-b"));
    XSFLIB_CHECK(LabelRefusal(commands, "Sub") && LabelRefusal(commands, "Sub ") && LabelRefusal(commands, "Süd"));
    XSFLIB_CHECK(!LabelRefusal(commands, "_x9") && !LabelRefusal(commands, "SubA"));
}

// A new label takes its place among the others by its name, and so does a renamed one. Neither changes what plays. A
// label at the end of the bytecode names the end.
void TestAddRenameRemove()
{
    const File file = hokuto::format::ImportSwitchArchive(TestArchive());
    const Edited added = AddLabel(file, 0, kKey62, "Bridge");
    const Edited at_end = AddLabel(added.file, 0, kEnd, "Tail");
    const Edited renamed = RenameLabel(at_end.file, 0, "Sub", "Refrain");
    const Edited removed = RemoveLabel(renamed.file, 0, "Track1");

    const std::vector<LabelPlace> kAdded = {{"Bridge", kKey62}, {"Song", 0}, {"Sub", kSub}, {"Track1", kTrack1}};
    XSFLIB_CHECK(LabelsOf(added.file, 0) == kAdded && LabelsAfterExport(added.file) == kAdded);
    XSFLIB_CHECK(
        (LabelsOf(at_end.file, 0) ==
         std::vector<LabelPlace>{{"Bridge", kKey62}, {"Song", 0}, {"Sub", kSub}, {"Tail", kEnd}, {"Track1", kTrack1}}));
    const std::vector<LabelPlace> kRenamed = {
        {"Bridge", kKey62}, {"Refrain", kSub}, {"Song", 0}, {"Tail", kEnd}, {"Track1", kTrack1}};
    XSFLIB_CHECK(LabelsOf(renamed.file, 0) == kRenamed && LabelsAfterExport(renamed.file) == kRenamed);
    XSFLIB_CHECK((LabelsOf(removed.file, 0) ==
                  std::vector<LabelPlace>{{"Bridge", kKey62}, {"Refrain", kSub}, {"Song", 0}, {"Tail", kEnd}}));
    XSFLIB_CHECK(Notes(Played(removed.file)) == kNotes && removed.items.size() == file.sequences[0]->items.size());
    XSFLIB_CHECK(*removed.file.sequences[0] != *file.sequences[0]);
    XSFLIB_CHECK(removed.file.sequences[0]->items == file.sequences[0]->items);

    XSFLIB_CHECK(Refuses([&] { AddLabel(file, 0, kKey62 + 1, "Bridge"); }));
    XSFLIB_CHECK(Refuses([&] { AddLabel(file, 0, kKey62, "Song"); }));
    XSFLIB_CHECK(Refuses([&] { RenameLabel(file, 0, "Missing", "Found"); }));
    XSFLIB_CHECK(Refuses([&] { RenameLabel(file, 0, "Sub", "Track1"); }));
    XSFLIB_CHECK(Refuses([&] { RemoveLabel(file, 0, "Missing"); }));
    XSFLIB_CHECK(RenameLabel(file, 0, "Sub", "Sub").file.sequences[0] == file.sequences[0]);
}

// A label that moves takes the calls and the sound's start with it, unless another label stays at its command.
void TestMoveLabel()
{
    const File file = hokuto::format::ImportSwitchArchive(TestArchive());
    const Edited moved = MoveLabel(file, 0, "Sub", kSubWait);
    const Edited alias = AddLabel(file, 0, kSub, "Alias");
    const Edited apart = MoveLabel(alias.file, 0, "Sub", kSubWait);
    const Edited start = MoveLabel(file, 0, "Song", kVolume);

    XSFLIB_CHECK(CommandAt(moved.file, 0, kFirstCall).target == 20 &&
                 CommandAt(moved.file, 0, kSecondCall).target == 20);
    XSFLIB_CHECK(
        (Notes(Played(moved.file)) == std::vector<std::vector<uint32_t>>{{0, 24, 60}, {72, 24, 62}, {120, 12, 64}}));
    XSFLIB_CHECK(CommandAt(apart.file, 0, kFirstCall).target == 19 && Notes(Played(apart.file)) == kNotes);
    XSFLIB_CHECK((LabelsOf(apart.file, 0) ==
                  std::vector<LabelPlace>{{"Alias", kSub}, {"Song", 0}, {"Sub", kSubWait}, {"Track1", kTrack1}}));
    XSFLIB_CHECK(Exported(file)->Sounds()[0].start_offset == 0);
    XSFLIB_CHECK(Exported(start.file)->Sounds()[0].start_offset == kVolume);
    XSFLIB_CHECK(MoveLabel(file, 0, "Sub", kSub).file.sequences[0] == file.sequences[0]);
    XSFLIB_CHECK(Refuses([&] { MoveLabel(file, 0, "Sub", kSubWait + 1); }));
}

// The two calls go to the subroutine.
void TestCalls()
{
    const File file = hokuto::format::ImportSwitchArchive(TestArchive());

    XSFLIB_CHECK((CallsTo(*file.sequences[0], 19) == std::vector<uint32_t>{8, 11}));
    XSFLIB_CHECK(CallsTo(*file.sequences[0], 6).empty());
}

// Key 60 and the wait after it become a subroutine after the others, with a return after them. The sound plays the same
// notes. So does the subroutine's body as a nested subroutine: the first call then calls it in turn. So do the commands
// around the first call. A selection with a gap, one with a label in its middle, one across the end of the stack and
// one with a return are refused. So are a name that a label has and an empty selection.
void TestToSubroutine()
{
    const File file = hokuto::format::ImportSwitchArchive(TestArchive());
    const std::vector<uint32_t> kIntro = {kWait48, kKey60};
    const Edited edited = ToSubroutine(file, 0, kIntro, 0, "Intro");
    const ntdlib::commands::Sequence& result = *edited.file.sequences[0];

    SEQLIB_REQUIRE(result.items.size() == 24 && edited.items.size() > 8);
    XSFLIB_CHECK(Notes(Played(edited.file)) == kNotes);
    XSFLIB_CHECK(result.items[6].kind == Kind::kCall && result.items[6].target == 21);
    XSFLIB_CHECK(result.items[21] == file.sequences[0]->items[6] && result.items[23].kind == Kind::kReturn);
    XSFLIB_CHECK(result.items[1].target == 14 && result.items[7].target == 18 && result.items[10].target == 18);
    XSFLIB_CHECK(edited.added == std::vector<uint32_t>{6} && edited.items[6] == 21u && edited.items[8] == 7u);
    XSFLIB_CHECK((LabelsAfterExport(edited.file) ==
                  std::vector<LabelPlace>{{"Intro", 53}, {"Song", 0}, {"Sub", 47}, {"Track1", 40}}));

    // The subroutine's key and wait, in the stretch of the first call.
    const std::vector<uint32_t> kBody = {kSub, kSubWait};
    const Edited nested = ToSubroutine(file, 0, kBody, 1, "Inner");
    XSFLIB_CHECK(Notes(Played(nested.file)) == kNotes);
    SEQLIB_REQUIRE(nested.file.sequences[0]->items.size() == 24);
    XSFLIB_CHECK(nested.file.sequences[0]->items[19].kind == Kind::kCall);

    // The wait before the first call, the call and the key after it. The track returns from the call inside the new
    // subroutine.
    const std::vector<uint32_t> kAcross = {kWait48, kFirstCall, kKey62};
    const Edited around = ToSubroutine(file, 0, kAcross, 1, "Across");
    SEQLIB_REQUIRE(around.file.sequences[0]->items.size() > 7);
    XSFLIB_CHECK(Notes(Played(around.file)) == kNotes && around.file.sequences[0]->items[7].kind == Kind::kCall);

    const File marked = AddLabel(file, 0, kWait48, "Middle").file;
    const std::vector<uint32_t> kGap = {kKey60, kFirstCall};
    const std::vector<uint32_t> kReturning = {kSubWait, kReturn};
    XSFLIB_CHECK(Refuses([&] { ToSubroutine(file, 0, kGap, 0, "Gap"); }));
    XSFLIB_CHECK(Refuses([&] { ToSubroutine(marked, 0, kIntro, 0, "Marked"); }));
    XSFLIB_CHECK(Refuses([&] { ToSubroutine(file, 0, kIntro, 10, "Deep"); }));
    XSFLIB_CHECK(Refuses([&] { ToSubroutine(file, 0, kReturning, 1, "Back"); }));
    XSFLIB_CHECK(Refuses([&] { ToSubroutine(file, 0, kIntro, 0, "Sub"); }));
    XSFLIB_CHECK(Refuses([&] { ToSubroutine(file, 0, {}, 0, "None"); }));
}

// A copy of the subroutine's key and wait takes the first call's place. The second call keeps the subroutine. A call
// that runs only if the last comparison held, and a subroutine that returns only then, can't give way.
void TestDissolveCall()
{
    const File file = hokuto::format::ImportSwitchArchive(TestArchive());
    const Edited edited = DissolveCall(file, 0, kFirstCall);
    const ntdlib::commands::Sequence& result = *edited.file.sequences[0];
    const ntdlib::commands::Sequence& before = *file.sequences[0];

    SEQLIB_REQUIRE(result.items.size() == 23 && edited.items.size() > 11);
    XSFLIB_CHECK(Notes(Played(edited.file)) == kNotes);
    XSFLIB_CHECK(result.items[8] == before.items[19] && result.items[9] == before.items[20]);
    XSFLIB_CHECK(result.items[12].kind == Kind::kCall && result.items[12].target == 20);
    XSFLIB_CHECK((edited.added == std::vector<uint32_t>{8, 9}) && !edited.items[8] && edited.items[11] == 12u);

    // A call that runs only if the last comparison held, and a subroutine that returns only then.
    const Sequence song = Played(file);
    Command conditional = CommandAt(file, 0, kFirstCall);
    conditional.conditional = true;
    const File if_call = ChangeCommand(file, 0, PlaceOf(song, kFirstCall, 0), conditional).file;
    Command back = CommandAt(file, 0, kReturn);
    back.conditional = true;
    const File if_return = ChangeCommand(file, 0, PlaceOf(song, kReturn, 1), back).file;
    XSFLIB_CHECK(Refuses([&] { DissolveCall(if_call, 0, kFirstCall); }));
    XSFLIB_CHECK(Refuses([&] { DissolveCall(if_return, 0, kFirstCall); }));
    XSFLIB_CHECK(Refuses([&] { DissolveCall(file, 0, kKey62); }));
}

// The second call goes to a copy of the subroutine after the sequence's commands, under a label with a number after the
// subroutine's name. The first call keeps the subroutine. Both play the same notes. The copy's call then gives way to
// its commands.
void TestCopySubroutine()
{
    const File file = hokuto::format::ImportSwitchArchive(TestArchive());
    const Edited edited = CopySubroutine(file, 0, kSecondCall);
    const ntdlib::commands::Sequence& result = *edited.file.sequences[0];
    const ntdlib::commands::Sequence& before = *file.sequences[0];

    SEQLIB_REQUIRE(result.items.size() == 25);
    XSFLIB_CHECK(Notes(Played(edited.file)) == kNotes);
    XSFLIB_CHECK(result.items[11].target == 22 && result.items[8].target == 19);
    XSFLIB_CHECK(std::vector<Command>(result.items.begin() + 22, result.items.end()) ==
                 std::vector<Command>(before.items.begin() + 19, before.items.end()));
    XSFLIB_CHECK((edited.added == std::vector<uint32_t>{22, 23, 24}) && edited.items.size() == 22 &&
                 edited.items[21] == 21u);
    XSFLIB_CHECK((LabelsAfterExport(edited.file) ==
                  std::vector<LabelPlace>{{"Song", 0}, {"Sub", kSub}, {"Sub_2", kEnd}, {"Track1", kTrack1}}));
    XSFLIB_CHECK(Notes(Played(DissolveCall(edited.file, 0, kSecondCall).file)) == kNotes);
    XSFLIB_CHECK(Refuses([&] { CopySubroutine(file, 0, kKey62); }));
}

} // namespace

int main()
{
    TestLabelNames();
    TestAddRenameRemove();
    TestMoveLabel();
    TestCalls();
    TestToSubroutine();
    TestDissolveCall();
    TestCopySubroutine();

    return xsflib::testing::Summary("subroutines");
}
