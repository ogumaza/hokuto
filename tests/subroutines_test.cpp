// SPDX-License-Identifier: MIT

// Tests the edits of a program's labels and subroutines (src/edit/subroutines.h). A small Switch archive holds one
// sequence file. Its sound's track 0 opens track 1, plays key 60, calls a subroutine that plays key 67, plays key 62,
// calls the subroutine again and plays key 64. Track 1 sets its pan and plays no notes. Each edit goes through an
// export of the archive. ntdlib's performer then plays the edited archive, and the notes have to show the edit and
// nothing else. A small GBA ROM checks the edits by MusicPlayer2000's rules: its patterns and its repeats. A small DS
// archive checks the depth of NITRO-Composer's stack.

#include "edit/subroutines.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/agb/rom.h"
#include "ntdlib/commands/sequence.h"
#include "ntdlib/mp2k/song.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sdat/layout.h"
#include "ntdlib/testing/mp2k_cart.h"
#include "ntdlib/testing/sar_files.h"
#include "ntdlib/testing/sdat_files.h"
#include "xsflib/testing/check.h"

#include "cart_bytes.h"
#include "edit/bytecode.h"
#include "edit/commands.h"
#include "format/archive.h"
#include "format/file.h"
#include "song/archive.h"
#include "song/ds.h"
#include "song/gba.h"
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
using xsflib::testing::ErrorOf;

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
    return !ErrorOf<std::invalid_argument>(edit).empty();
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

// A GBA ROM of one song. Its track plays key 60, calls a pattern, plays key 62, calls the pattern again and plays key
// 64, each 24 ticks after the last. The pattern plays key 67 at the track's last velocity, waits 12 ticks and repeats
// once. The ROM has free space for a song that grows.
std::vector<uint8_t> GbaRom()
{
    namespace mp2k_cart = ntdlib::testing::mp2k_cart;
    using enum ntdlib::mp2k::Command;

    // The pattern's place is known once the track before it is.
    const auto track = [](std::size_t pattern)
    {
        mp2k_cart::Track built;
        built.Cmd(kCmdTempo, 75).Cmd(kCmdVoice, 0).Cmd(kCmdVol, 100).Note(24, 60, 100).Wait(24).Patt(pattern);
        built.Note(24, 62, 100).Wait(24).Patt(pattern).Note(24, 64, 100).Wait(24).Fine();
        const std::size_t here = built.Here();
        built.Note(12, 67).Wait(12).Rept(2, here).Raw({kCmdPend});
        return std::pair(built, here);
    };

    mp2k_cart::Cart cart;
    const uint32_t voices = cart.VoiceGroup({{0, mp2k_cart::Cart::Voice(0, cart.Wave(mp2k_cart::Saw(), 13379, 0))}});
    cart.Song({track(track(0).second).first}, voices);
    for (uint32_t a = 0x38000; a < 0x40000; a++)
    {
        cart.Put8(mp2k_cart::kRomBase + a, 0xFF);
    }

    const ntdlib::agb::Rom rom = cart.ToRom();
    return hokuto::testing::RomBytes(rom);
}

// A GBA ROM of one song. Its track plays key 60, key 67 four times through a repeat, and key 64, each 12 ticks after
// the last. The ROM has free space for a song that grows.
std::vector<uint8_t> GbaRepeatRom()
{
    namespace mp2k_cart = ntdlib::testing::mp2k_cart;
    using enum ntdlib::mp2k::Command;

    mp2k_cart::Track track;
    track.Cmd(kCmdTempo, 75).Cmd(kCmdVoice, 0).Cmd(kCmdVol, 100).Note(12, 60, 100).Wait(12);
    const std::size_t here = track.Here();
    track.Note(12, 67).Wait(12).Rept(4, here).Note(12, 64).Wait(12).Fine();

    mp2k_cart::Cart cart;
    const uint32_t voices = cart.VoiceGroup({{0, mp2k_cart::Cart::Voice(0, cart.Wave(mp2k_cart::Saw(), 13379, 0))}});
    cart.Song({track}, voices);
    for (uint32_t a = 0x38000; a < 0x40000; a++)
    {
        cart.Put8(mp2k_cart::kRomBase + a, 0xFF);
    }

    const ntdlib::agb::Rom rom = cart.ToRom();
    return hokuto::testing::RomBytes(rom);
}

// The start, key and velocity of each note of song 0 as ntdlib's performer plays it from the ROM that `file` builds.
std::vector<std::vector<uint32_t>> GbaNotes(const File& file)
{
    const Sequence song = hokuto::song::ReadSequence(*hokuto::song::LoadGbaRom(hokuto::format::ExportArchive(file)), 0);
    std::vector<std::vector<uint32_t>> notes;
    for (const Note& note : song.tracks.at(0).notes)
    {
        notes.push_back({note.start, note.key, note.velocity});
    }

    return notes;
}

// The offset of the `nth` item of `kind` of song 0 of `file`.
uint32_t GbaOffset(const File& file, Kind kind, std::size_t nth = 0)
{
    const ntdlib::commands::Sequence& commands = *file.sequences.at(0);
    const std::vector<uint32_t> offsets = hokuto::edit::Offsets(commands, hokuto::edit::ConsoleOf(file));
    for (std::size_t item = 0; item < commands.items.size(); item++)
    {
        if (commands.items[item].kind == kind && nth-- == 0)
        {
            return offsets[item];
        }
    }

    throw std::logic_error("no such item");
}

// A GBA song accepts labels. The ROM keeps none. Key 60 and the wait after it become a pattern, with a call in their
// place, and the call then gives way to them again. The pattern's commands replace the first call of the cartridge, and
// the second calls a copy of the pattern. The repeats in the copies jump back to the copies. Every edit plays the same
// notes, the notes of the pattern at the velocity of the track. A fourth pattern deep is refused, and so is a pattern
// with a repeat that jumps back to a command before it.
void TestGba()
{
    const File file = hokuto::format::ImportGbaRom(GbaRom());
    const std::vector<std::vector<uint32_t>> notes = {
        {0, 60, 100}, {24, 67, 100}, {36, 67, 100}, {48, 62, 100}, {72, 67, 100}, {84, 67, 100}, {96, 64, 100},
    };
    XSFLIB_CHECK(GbaNotes(file) == notes);

    const Edited labeled = AddLabel(file, 0, GbaOffset(file, Kind::kCall), "Call");
    XSFLIB_CHECK((labeled.file.sequences[0]->labels == std::vector<ntdlib::commands::Label>{{"Call", 5}}));
    XSFLIB_CHECK(GbaNotes(labeled.file) == notes);
    XSFLIB_CHECK(RenameLabel(labeled.file, 0, "Call", "First").file.sequences[0]->labels.at(0).name == "First");
    XSFLIB_CHECK(RemoveLabel(labeled.file, 0, "Call").file.sequences[0]->labels.empty());

    const std::vector<uint32_t> intro = {GbaOffset(file, Kind::kNote), GbaOffset(file, Kind::kWait)};
    const Edited pattern = ToSubroutine(file, 0, intro, 0, "Intro");
    SEQLIB_REQUIRE(pattern.file.sequences[0]->items.size() > 3);
    XSFLIB_CHECK(pattern.file.sequences[0]->items[3].kind == Kind::kCall);
    XSFLIB_CHECK(pattern.file.sequences[0]->items.back().kind == Kind::kReturn);
    XSFLIB_CHECK(GbaNotes(pattern.file) == notes);

    // Nothing reaches the new pattern once its commands replace its call, and the ROM keeps its bytes as data.
    const ntdlib::commands::Sequence& called = *pattern.file.sequences[0];
    const uint32_t call = hokuto::edit::Offsets(called, hokuto::edit::ConsoleOf(pattern.file)).at(pattern.added.at(0));
    XSFLIB_CHECK(GbaNotes(DissolveCall(pattern.file, 0, call).file) == notes);

    // Once the pattern's commands replace the first call, the second is the only call. The second call points to the
    // pattern's note.
    const Edited dissolved = DissolveCall(file, 0, GbaOffset(file, Kind::kCall));
    const ntdlib::commands::Sequence& left = *dissolved.file.sequences[0];
    const auto second = std::ranges::find(left.items, Kind::kCall, &Command::kind);
    SEQLIB_REQUIRE(second != left.items.end() && std::ranges::count(left.items, Kind::kCall, &Command::kind) == 1);
    const auto second_item = static_cast<uint32_t>(second - left.items.begin());
    SEQLIB_REQUIRE(second->target < left.items.size());
    XSFLIB_CHECK(left.items[second->target].kind == Kind::kNote && left.items[second->target].key == 67);
    XSFLIB_CHECK(CallsTo(left, second->target) == std::vector<uint32_t>{second_item});
    XSFLIB_CHECK(GbaNotes(dissolved.file) == notes);
    const Edited copied = CopySubroutine(file, 0, GbaOffset(file, Kind::kCall, 1));
    XSFLIB_CHECK(GbaNotes(copied.file) == notes);
    // Each repeat jumps back to the note at the start of its pattern, with no return between.
    const ntdlib::commands::Sequence& copy = *copied.file.sequences[0];
    std::vector<uint32_t> targets;
    for (std::size_t item = 0; item < copy.items.size(); item++)
    {
        if (copy.items[item].kind == Kind::kRepeat)
        {
            const uint32_t target = copy.items[item].target;
            SEQLIB_REQUIRE(target < item);
            XSFLIB_CHECK(copy.items[target].kind == Kind::kNote && copy.items[target].key == 67);
            for (std::size_t between = target; between < item; between++)
            {
                XSFLIB_CHECK(copy.items[between].kind != Kind::kReturn);
            }
            targets.push_back(target);
        }
    }
    XSFLIB_CHECK(targets.size() == 2 && targets[0] != targets[1]);

    XSFLIB_CHECK(Refuses([&] { ToSubroutine(file, 0, intro, 3, "Deep"); }));
    const std::vector<uint32_t> repeat = {GbaOffset(file, Kind::kWait, 3), GbaOffset(file, Kind::kRepeat)};
    XSFLIB_CHECK(Refuses([&] { ToSubroutine(file, 0, repeat, 1, "Back"); }));
}

// A GBA track's note, wait and repeat become a pattern. MusicPlayer2000's repeat jumps to an address. The repeat of
// the pattern jumps back to its note, and the track plays the same notes. A repeat that went back to the call
// would start the pattern again each time, and the fourth pattern deep would end the track.
void TestGbaRepeatInPattern()
{
    const File file = hokuto::format::ImportGbaRom(GbaRepeatRom());
    const std::vector<std::vector<uint32_t>> notes = {
        {0, 60, 100}, {12, 67, 100}, {24, 67, 100}, {36, 67, 100}, {48, 67, 100}, {60, 64, 100},
    };
    XSFLIB_CHECK(GbaNotes(file) == notes);

    const std::vector<uint32_t> repeated = {GbaOffset(file, Kind::kNote, 1), GbaOffset(file, Kind::kWait, 1),
                                            GbaOffset(file, Kind::kRepeat)};
    const Edited pattern = ToSubroutine(file, 0, repeated, 0, "Repeated");
    const ntdlib::commands::Sequence& commands = *pattern.file.sequences[0];
    const auto rept = std::ranges::find(commands.items, Kind::kRepeat, &Command::kind);
    SEQLIB_REQUIRE(rept != commands.items.end() && rept->target < commands.items.size());
    XSFLIB_CHECK(commands.items[rept->target].kind == Kind::kNote && commands.items[rept->target].key == 67);
    XSFLIB_CHECK(GbaNotes(pattern.file) == notes);
}

// A DS archive of one sequence. Its track 0 sets its volume, plays key 60 for 48 ticks and waits 48 ticks.
std::vector<uint8_t> DsArchive()
{
    namespace sdat_files = ntdlib::testing::sdat_files;
    const sdat_files::Bytes track = {0xFE, 0x01, 0x00, 0xC1, 0x64, 0x3C, 0x7F, 0x30, 0x80, 0x30, 0xFF};
    const sdat_files::Wave square{
        .loop = 1,
        .loop_length = 4,
        .data = {0x40, 0x40, 0xC0, 0xC0, 0x40, 0x40, 0xC0, 0xC0, 0x40, 0x40, 0xC0, 0xC0, 0x40, 0x40, 0xC0, 0xC0}};
    constexpr uint16_t kNone = 0xFFFF;

    ntdlib::sdat::ArchiveModel model;
    model.sequences = {ntdlib::sdat::SequenceInfo{0, 0, 127, 64, 64, 0, 0, ""}};
    model.banks = {ntdlib::sdat::BankInfo{1, {0, kNone, kNone, kNone}, ""}};
    model.wave_archives = {ntdlib::sdat::WaveArchiveInfo{2, ""}};
    model.players = {ntdlib::sdat::PlayerInfo{1, 0, 0, 0, ""}};
    model.files = {sdat_files::Sequence(track), sdat_files::Bank({{.type = 1, .region = {.wave = 0}}}),
                   sdat_files::WaveArchive({square})};

    return ntdlib::sdat::BuildArchive(model);
}

// The start, length and key of each note of track 0 of sequence 0 as ntdlib's model of NITRO-Composer plays it from
// the archive that `file` builds.
std::vector<std::vector<uint32_t>> DsNotes(const File& file)
{
    return Notes(hokuto::song::ReadSequence(*hokuto::song::LoadDsArchive(hokuto::format::ExportArchive(file)), 0));
}

// NITRO-Composer keeps 3 calls and loops on a track's stack, and skips a call past them (ntdlib::ntr::snd::kCallDepth).
// The note and the wait become a subroutine where they run 2 deep, and the track plays the same note. Where they run 3
// deep, the call is refused: the track would never run it.
void TestDs()
{
    const File file = hokuto::format::ImportDsArchive(DsArchive());
    const std::vector<uint32_t> offsets = hokuto::edit::Offsets(*file.sequences[0], hokuto::edit::ConsoleOf(file));
    SEQLIB_REQUIRE(file.sequences[0]->items.size() == 5 && file.sequences[0]->items[2].kind == Kind::kNote);
    const std::vector<uint32_t> body = {offsets.at(2), offsets.at(3)};

    const Edited edited = ToSubroutine(file, 0, body, 2, "Body");

    XSFLIB_CHECK((DsNotes(file) == std::vector<std::vector<uint32_t>>{{0, 48, 60}}));
    XSFLIB_CHECK(DsNotes(edited.file) == DsNotes(file));
    XSFLIB_CHECK(Refuses([&] { ToSubroutine(file, 0, body, 3, "Deep"); }));
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("subroutines",
                                    []
                                    {
                                        TestLabelNames();
                                        TestAddRenameRemove();
                                        TestMoveLabel();
                                        TestCalls();
                                        TestToSubroutine();
                                        TestDissolveCall();
                                        TestCopySubroutine();
                                        TestGba();
                                        TestGbaRepeatInPattern();
                                        TestDs();
                                    });
}
