// SPDX-License-Identifier: MIT

// Tests what the List Editor works out without a window (src/app/list_edits.h): the text of each column, the edits of
// typed values, and the commands that it inserts and deletes. A track made by hand holds a command of each shape. Small
// Switch archives then give the rows of a sound's tracks, those without notes included, and the rows of a loop.

#include "app/list_edits.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/commands/units.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/testing/sar_files.h"
#include "xsflib/testing/check.h"

#include "app/transport.h"
#include "format/archive.h"
#include "format/file.h"
#include "song/archive.h"
#include "song/sequence.h"

using hokuto::app::CallsOfRuns;
using hokuto::app::CallsText;
using hokuto::app::CallsTooltip;
using hokuto::app::CellText;
using hokuto::app::ConditionEdit;
using hokuto::app::DissolveEdit;
using hokuto::app::InsertEdit;
using hokuto::app::IsLabel;
using hokuto::app::KindName;
using hokuto::app::LabelEdit;
using hokuto::app::LabelMoveEdit;
using hokuto::app::LabelRemovalEdit;
using hokuto::app::ListCall;
using hokuto::app::ListColumn;
using hokuto::app::ListEdit;
using hokuto::app::ListRow;
using hokuto::app::ListSubroutine;
using hokuto::app::ListTrack;
using hokuto::app::MakeListTrack;
using hokuto::app::NamesTarget;
using hokuto::app::RealCopyEdit;
using hokuto::app::RemovalEdit;
using hokuto::app::RowOfPart;
using hokuto::app::RunOfCall;
using hokuto::app::Skipped;
using hokuto::app::SkippingEngine;
using hokuto::app::SubroutineEdit;
using hokuto::app::SubroutineRuns;
using hokuto::app::TargetEdit;
using hokuto::app::Typable;
using hokuto::app::TypedListEdit;
using hokuto::app::WithLabelRows;
using hokuto::app::WithSubroutine;
using ntdlib::commands::Argument;
using ntdlib::commands::Command;
using ntdlib::commands::Kind;
using ntdlib::commands::kOne;
using ntdlib::commands::Source;

namespace
{

namespace sar = ntdlib::sar;
namespace sar_files = ntdlib::testing::sar_files;

// The rows of the track made by hand.
enum Row : std::size_t
{
    kNote,
    kWait,
    kVolume,
    kRandomVolume,
    kVariableVolume,
    kSend,
    kLfo,
    kSet,
    kJump,
    kJumpToEnd,
    kAllocate,
    kTranspose,
    kTempo,
    kNoteWait,
    kOpen,
    kEnd,
    kRows,
};

// A command of `kind` with the value `value`.
Command Valued(Kind kind, Argument value, uint8_t index = 0)
{
    Command command;
    command.kind = kind;
    command.index = index;
    command.value = value;

    return command;
}

// A plain value of `units` in fixed point.
Argument Plain(int64_t units)
{
    return Argument{Source::kValue, units * kOne, 0};
}

// A track of the Switch with one command of each shape, 4 bytes apart, at 48 ticks to the quarter note. Labels name
// rows 0 and 2.
ListTrack HandMade()
{
    std::vector<Command> commands(kRows);
    commands[kNote] = Valued(Kind::kNote, Plain(24));
    commands[kNote].key = 60;
    commands[kNote].velocity = 100 * kOne;
    commands[kWait] = Valued(Kind::kWait, Plain(48));
    commands[kVolume] = Valued(Kind::kVolume, Plain(100));
    commands[kRandomVolume] = Valued(Kind::kVolume, Argument{Source::kRandom, 0, 127 * kOne});
    commands[kVariableVolume] = Valued(Kind::kVolume, Argument{Source::kVariable, 3, 0});
    commands[kSend] = Valued(Kind::kSend, Plain(64));
    commands[kLfo] = Valued(Kind::kLfoDepth, Argument{Source::kValue, kOne / 2, 0}, 1);
    commands[kSet] = Valued(Kind::kSetVariable, Plain(2), 5);
    commands[kJump] = Valued(Kind::kJump, Argument{});
    commands[kJumpToEnd] = Valued(Kind::kJump, Argument{});
    commands[kJumpToEnd].target = kRows;
    commands[kAllocate] = Valued(Kind::kAllocateTracks, Plain(7));
    commands[kTranspose] = Valued(Kind::kTranspose, Plain(-100));
    commands[kTempo] = Valued(Kind::kTempo, Plain(120));
    commands[kNoteWait] = Valued(Kind::kNoteWait, Plain(0));
    commands[kOpen] = Valued(Kind::kOpenTrack, Argument{}, 1);
    commands[kOpen].target = 2;
    commands[kEnd] = Valued(Kind::kEnd, Argument{});

    ListTrack track;
    track.name = "Track 0";
    track.stretches = {hokuto::song::Stretch{}};
    for (uint32_t i = 0; i <= kRows; i++)
    {
        track.offsets.push_back(i * 4);
    }

    for (uint32_t i = 0; i < kRows; i++)
    {
        ListRow& row = track.rows.emplace_back();
        row.row = {i * 4, 0, i == 0 ? 0u : 48u};
        row.command = commands[i];
        row.entered = i == 0;
    }

    track.labels = {{0, "Start"}, {2, "Loop"}};
    track.targets = {{"Start", 0}, {"Loop", 2}};
    track.tracks = {0};

    return track;
}

// The text of `column` of row `row` of `track`.
std::string Cell(const ListTrack& track, std::size_t row, ListColumn column)
{
    return CellText(track, track.rows.at(row), column);
}

// The command of the edit of `text` typed into `column` of row `row`, when the edit changes the command.
std::optional<Command> Typed(const ListTrack& track, std::size_t row, ListColumn column, const std::string& text)
{
    const std::optional<ListEdit> edit = TypedListEdit(track, row, column, text);
    if (!edit || edit->action != ListEdit::Action::kChange || !edit->refused.empty())
    {
        return std::nullopt;
    }

    return edit->command;
}

// Why the edit of `text` typed into `column` of row `row` is refused, or empty.
std::string Refused(const ListTrack& track, std::size_t row, ListColumn column, const std::string& text)
{
    const std::optional<ListEdit> edit = TypedListEdit(track, row, column, text);
    return edit ? edit->refused : std::string();
}

// The name of the edit of `text` typed into `column` of row `row`, or empty without an edit.
std::string TypedName(const ListTrack& track, std::size_t row, ListColumn column, const std::string& text)
{
    const std::optional<ListEdit> edit = TypedListEdit(track, row, column, text);
    return edit ? edit->name : std::string();
}

// Each kind has a name, and the columns show each command's values. A command without a label counts in its comment
// what goes to it.
void TestText()
{
    const ListTrack track = HandMade();

    for (int kind = 0; kind < ntdlib::commands::kKindCount; kind++)
    {
        XSFLIB_CHECK(!KindName(static_cast<Kind>(kind)).empty());
    }
    XSFLIB_CHECK(KindName(Kind::kData) == "Data" && KindName(Kind::kOpenTrack) == "Open Track");
    XSFLIB_CHECK(KindName(Kind::kCompareGreaterOrEqual) == "Compare ≥");
    XSFLIB_CHECK(Cell(track, kNote, ListColumn::kType) == "Note");
    XSFLIB_CHECK(Cell(track, kNote, ListColumn::kStart) == hokuto::app::PositionText(0.0, 4));
    XSFLIB_CHECK(Cell(track, kWait, ListColumn::kStart) == hokuto::app::PositionText(1.0, 4));
    XSFLIB_CHECK(Cell(track, kNote, ListColumn::kLength) == hokuto::app::LengthText(0.5, 4));
    XSFLIB_CHECK(Cell(track, kNote, ListColumn::kData1) == "C3" && Cell(track, kNote, ListColumn::kData2) == "100");
    XSFLIB_CHECK(Cell(track, kNote, ListColumn::kComment) == "Start");
    XSFLIB_CHECK(Cell(track, kVolume, ListColumn::kData1) == "100" &&
                 Cell(track, kVolume, ListColumn::kComment) == "Loop");
    XSFLIB_CHECK(Cell(track, kRandomVolume, ListColumn::kData1) == "Random 0 to 127");
    XSFLIB_CHECK(Cell(track, kVariableVolume, ListColumn::kData1) == "Variable 3");
    XSFLIB_CHECK(Cell(track, kSend, ListColumn::kData1) == "A" && Cell(track, kSend, ListColumn::kData2) == "64");
    XSFLIB_CHECK(Cell(track, kLfo, ListColumn::kData1) == "LFO 1" && Cell(track, kLfo, ListColumn::kData2) == "0.5");
    XSFLIB_CHECK(Cell(track, kSet, ListColumn::kType) == "Set" &&
                 Cell(track, kSet, ListColumn::kData1) == "Variable 5");
    XSFLIB_CHECK(Cell(track, kJump, ListColumn::kData1) == "Start" &&
                 Cell(track, kJumpToEnd, ListColumn::kData1) == "End");
    XSFLIB_CHECK(Cell(track, kAllocate, ListColumn::kData1) == "0, 1, 2");
    XSFLIB_CHECK(Cell(track, kTranspose, ListColumn::kData1) == "-100 cents");
    XSFLIB_CHECK(Cell(track, kTempo, ListColumn::kData1) == "120 BPM");
    XSFLIB_CHECK(Cell(track, kNoteWait, ListColumn::kData1) == "Off");
    XSFLIB_CHECK(Cell(track, kOpen, ListColumn::kData1) == "Track 1" &&
                 Cell(track, kOpen, ListColumn::kData2) == "Loop");
    XSFLIB_CHECK(Cell(track, kEnd, ListColumn::kData1).empty() && Cell(track, kWait, ListColumn::kData1).empty());

    // A command without a label counts the jumps, calls and track starts that go to it. A label's name stands for them.
    ListTrack targeted = track;
    targeted.entries[kRandomVolume] = {0, 2, 0};
    targeted.entries[kVolume] = {1, 0, 0};
    XSFLIB_CHECK(Cell(targeted, kRandomVolume, ListColumn::kComment) == "2 jumps");
    XSFLIB_CHECK(Cell(targeted, kVolume, ListColumn::kComment) == "Loop");
    XSFLIB_CHECK(Cell(targeted, kSend, ListColumn::kComment).empty());
}

// A typed value changes the command. A value from a random number or a variable can be typed too. A value outside the
// console's bytes is refused with its range.
void TestTypedValues()
{
    const ListTrack track = HandMade();

    const std::optional<Command> quieter = Typed(track, kVolume, ListColumn::kData1, "90");
    const std::optional<Command> random = Typed(track, kVolume, ListColumn::kData1, "random 10 to 20");
    const std::optional<Command> bracketed = Typed(track, kVolume, ListColumn::kData1, "Random(20, 10)");
    const std::optional<Command> variable = Typed(track, kVolume, ListColumn::kData1, "var(4)");
    const std::optional<Command> lower = Typed(track, kTranspose, ListColumn::kData1, "-200 cents");
    const std::optional<Command> slower = Typed(track, kTempo, ListColumn::kData1, "90");
    const std::optional<Command> on = Typed(track, kNoteWait, ListColumn::kData1, "on");

    XSFLIB_CHECK((quieter && quieter->value == Argument{Source::kValue, 90 * kOne, 0}));
    XSFLIB_CHECK(TypedName(track, kVolume, ListColumn::kData1, "90") == "Set Main Value");
    SEQLIB_REQUIRE((random && random->value == Argument{Source::kRandom, 10 * kOne, 20 * kOne}));
    XSFLIB_CHECK(bracketed && bracketed->value == random->value);
    XSFLIB_CHECK((variable && variable->value == Argument{Source::kVariable, 4, 0}));
    XSFLIB_CHECK(lower && lower->value.value == -200 * kOne && slower && slower->value.value == 90 * kOne);
    XSFLIB_CHECK(on && on->value.value == kOne);
    XSFLIB_CHECK(Refused(track, kVolume, ListColumn::kData1, "300") == "Volume runs from 0 to 255.");
    XSFLIB_CHECK(Refused(track, kVolume, ListColumn::kData1, "variable 300") ==
                 "A variable's number runs from 0 to 255.");
    XSFLIB_CHECK(Refused(track, kVolume, ListColumn::kData1, "loud") ==
                 "Hokuto can't read \"loud\" there. A value is a number, with or without its unit, or \"random 0 to "
                 "127\" or \"variable 3\".");
    XSFLIB_CHECK(!TypedListEdit(track, kVolume, ListColumn::kData1, " 100 "));
    XSFLIB_CHECK(!TypedListEdit(track, kVolume, ListColumn::kType, "Pan"));
}

// A note's pitch, velocity, start and length, a wait's length, and a setting's start and time. A setting that reads or
// changes a variable keeps its start, and so does a command that decides the timing. Text that a start or a length
// can't read is refused with an example that it reads.
void TestTypedNotesAndLengths()
{
    const ListTrack track = HandMade();

    const std::optional<Command> higher = Typed(track, kNote, ListColumn::kData1, "D3");
    const std::optional<Command> softer = Typed(track, kNote, ListColumn::kData2, "90");
    const std::optional<ListEdit> moved = TypedListEdit(track, kNote, ListColumn::kStart, "2");
    const std::optional<ListEdit> volume = TypedListEdit(track, kVolume, ListColumn::kStart, "3");
    const std::optional<Command> shorter = Typed(track, kNote, ListColumn::kLength, "0. 1. 0");
    const std::optional<Command> waits = Typed(track, kWait, ListColumn::kLength, "random 0. 1. 0 to 0. 2. 0");
    const std::optional<Command> ramp = Typed(track, kVolume, ListColumn::kLength, "0. 1. 0");
    ListTrack timed = track;
    timed.rows[kVolume].command.time = Argument{Source::kValue, 12 * kOne, 0};
    const std::optional<Command> sudden = Typed(timed, kVolume, ListColumn::kLength, "");

    XSFLIB_CHECK(higher && higher->key == 62 && TypedName(track, kNote, ListColumn::kData1, "D3") == "Transpose");
    XSFLIB_CHECK(Typed(track, kNote, ListColumn::kData1, "62") == higher);
    XSFLIB_CHECK(softer && softer->velocity == 90 * kOne &&
                 Refused(track, kNote, ListColumn::kData2, "128") == "A velocity runs from 0 to 127.");
    XSFLIB_CHECK(Refused(track, kNote, ListColumn::kData1, "200") == "A key runs from 0 to 127, C-2 to G8.");
    XSFLIB_CHECK(Refused(track, kNote, ListColumn::kData1, "H3") ==
                 "Hokuto can't read \"H3\" there. A key is a number from 0 to 127 or a name, such as C3 for middle C.");
    XSFLIB_CHECK(Refused(track, kNote, ListColumn::kStart, "2.1.1.0") ==
                 "Hokuto can't read \"2.1.1.0\" there. A start is a position, such as 2. 1. 0.");
    XSFLIB_CHECK(Refused(track, kNote, ListColumn::kLength, "0.1.0.0") ==
                 "Hokuto can't read \"0.1.0.0\" there. A length is a number of beats, sixteenths and ticks, such as "
                 "0. 1. 0. It can also be \"random 0 to 48\" or \"variable 3\".");
    XSFLIB_CHECK(moved && moved->action == ListEdit::Action::kMove && moved->shift == 48 && moved->name == "Move");
    XSFLIB_CHECK(volume && volume->action == ListEdit::Action::kMove && volume->shift == 48);
    XSFLIB_CHECK(!TypedListEdit(track, kVariableVolume, ListColumn::kStart, "3"));
    XSFLIB_CHECK(!TypedListEdit(track, kSet, ListColumn::kStart, "3"));
    XSFLIB_CHECK(Typable(track, track.rows[kRandomVolume], ListColumn::kStart));
    XSFLIB_CHECK(Typable(track, track.rows[kTempo], ListColumn::kStart));
    XSFLIB_CHECK(!Typable(track, track.rows[kNoteWait], ListColumn::kStart));
    XSFLIB_CHECK(!Typable(track, track.rows[kWait], ListColumn::kStart));
    XSFLIB_CHECK(shorter && shorter->value.value == 12 * kOne);
    XSFLIB_CHECK(TypedName(track, kNote, ListColumn::kLength, "0. 1. 0") == "Size");
    XSFLIB_CHECK((waits && waits->value == Argument{Source::kRandom, 12 * kOne, 24 * kOne}));
    XSFLIB_CHECK(TypedName(track, kWait, ListColumn::kLength, "0. 2. 0") == "Set Main Value");
    XSFLIB_CHECK(!TypedListEdit(track, kWait, ListColumn::kLength, "1"));
    XSFLIB_CHECK((ramp && ramp->time == Argument{Source::kValue, 12 * kOne, 0}));
    XSFLIB_CHECK(TypedName(track, kVolume, ListColumn::kLength, "0. 1. 0") == "Change Command");
    XSFLIB_CHECK(sudden && !sudden->time);
    XSFLIB_CHECK(Typable(track, track.rows[kVolume], ListColumn::kLength));
    XSFLIB_CHECK(!Typable(track, track.rows[kAllocate], ListColumn::kLength));
}

// What a command addresses: a bus, an LFO, a variable or a track, and the tracks that Allocate Tracks names.
void TestTypedIndices()
{
    const ListTrack track = HandMade();

    const std::optional<Command> bus = Typed(track, kSend, ListColumn::kData1, "B");
    const std::optional<Command> fx = Typed(track, kSend, ListColumn::kData1, "FX C");
    const std::optional<Command> variable = Typed(track, kSet, ListColumn::kData1, "Variable 7");
    const std::optional<Command> tracks = Typed(track, kAllocate, ListColumn::kData1, "0, 3");
    const std::optional<Command> opened = Typed(track, kOpen, ListColumn::kData1, "Track 2");

    XSFLIB_CHECK(bus && bus->index == 1 && fx && fx->index == 2 &&
                 Refused(track, kSend, ListColumn::kData1, "D") ==
                     "Hokuto can't read \"D\" there. A bus is A, B or C.");
    XSFLIB_CHECK(Refused(track, kLfo, ListColumn::kData1, "LFO 5") == "An LFO runs from 0 to 3.");
    XSFLIB_CHECK(variable && variable->index == 7);
    XSFLIB_CHECK(tracks && tracks->value.value == 9 * kOne);
    XSFLIB_CHECK(opened && opened->index == 2);
    XSFLIB_CHECK(Refused(track, kOpen, ListColumn::kData1, "16") == "A track runs from 0 to 15.");
}

// A target comes from the labels, and the condition switches on and off. A new command goes before a row, and a
// deletion takes each selected command once.
void TestTargetsInsertsAndRemovals()
{
    const ListTrack track = HandMade();

    const std::optional<ListEdit> target = TargetEdit(track, kJump, 2);
    const ListEdit condition = ConditionEdit(track, kVolume);
    const std::optional<ListEdit> volume = InsertEdit(track, kVolume, Kind::kVolume);
    const std::optional<ListEdit> jump = InsertEdit(track, std::nullopt, Kind::kJump);
    const std::optional<ListEdit> open = InsertEdit(track, kNote, Kind::kOpenTrack);
    const std::optional<ListEdit> removal = RemovalEdit(track, {4, 8, 999});

    XSFLIB_CHECK(NamesTarget(track.rows[kJump], ListColumn::kData1) &&
                 !Typable(track, track.rows[kJump], ListColumn::kData1));
    XSFLIB_CHECK(NamesTarget(track.rows[kOpen], ListColumn::kData2) &&
                 !NamesTarget(track.rows[kOpen], ListColumn::kData1));
    XSFLIB_CHECK(target && target->command.target == 2 && !TargetEdit(track, kJump, 0));
    XSFLIB_CHECK(condition.command.conditional && condition.rows == std::vector<std::size_t>{kVolume});
    SEQLIB_REQUIRE(volume && volume->action == ListEdit::Action::kInsert && volume->name == "Insert Command");
    XSFLIB_CHECK(volume->rows == std::vector<std::size_t>{kVolume} && volume->command.value.value == 127 * kOne);
    XSFLIB_CHECK(jump && jump->rows == std::vector<std::size_t>{kEnd} && jump->command.target == kEnd);
    XSFLIB_CHECK(open && open->command.target == kRows && open->command.index == 1);
    XSFLIB_CHECK(removal && removal->name == "Delete" && removal->rows == (std::vector<std::size_t>{kWait, kVolume}));
    XSFLIB_CHECK(!RemovalEdit(track, {}) && !InsertEdit(ListTrack{}, std::nullopt, Kind::kVolume));
}

// The bytecode of a sound whose track 0 opens track 1, plays a note and calls a subroutine. Track 1 starts at track 0's
// end, and plays no notes.
const std::vector<uint8_t> kBytecode = {
    0x88, 0x01, 14,   0x00, 0x00, // 0: open track 1 at 14
    0xC7, 0x00,                   // 5: note wait off
    60,   100,  24,               // 7: key 60 for 24 ticks
    0x8A, 15,   0x00, 0x00,       // 10: call 15
    0xFF,                         // 14: the end
    0x80, 12,                     // 15: the subroutine: wait 12 ticks
    0xFD,                         // 17: return
};

// The labels of that bytecode.
const std::vector<std::pair<std::string, uint32_t>> kLabels = {{"Song", 0}, {"Sub", 15}};

// The bytecode of a sound whose track 0 plays a note in a loop of two passes.
const std::vector<uint8_t> kLoopBytecode = {
    0xD4, 2,       // 0: a loop of two passes
    60,   100, 24, // 2: key 60 for 24 ticks
    0xFC,          // 5: the loop's end
    0xFF,          // 6: the end
};

// The bytecode of a sound whose track 0 waits 12 ticks, then calls a subroutine over and over. The subroutine plays a
// note of 24 ticks.
const std::vector<uint8_t> kLoopedCallBytecode = {
    0x80, 12,              // 0: wait 12 ticks
    0x8A, 10,  0x00, 0x00, // 2: call 10
    0x89, 2,   0x00, 0x00, // 6: jump 2
    60,   100, 24,         // 10: the subroutine: key 60 for 24 ticks
    0xFD,                  // 13: return
};

// The archive of `bytecode` with `labels`. Its bank plays a looped wave for every key.
std::vector<uint8_t> TestArchive(const std::vector<uint8_t>& bytecode = kBytecode,
                                 const std::vector<std::pair<std::string, uint32_t>>& labels = kLabels)
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

// The rows of a sound's track follow the calls. A track without notes has rows too. Each row that starts a stretch is
// where the track entered the sequence.
void TestTrackOfArchive()
{
    const std::vector<uint8_t> bytes = TestArchive();
    const hokuto::format::File file = hokuto::format::ImportSwitchArchive(bytes);
    const std::shared_ptr<const sar::SoundArchive> archive = hokuto::song::LoadArchive(bytes, "test.bfsar");
    const hokuto::song::Sequence song = hokuto::song::ReadSequence(*archive, 0);

    const ListTrack first = MakeListTrack(file, 0, song, 0);
    const ListTrack second = MakeListTrack(file, 0, song, 1);
    const ListTrack missing = MakeListTrack(hokuto::format::File{}, 0, song, 0);

    std::vector<uint32_t> commands;
    std::vector<bool> entered;
    for (const ListRow& row : first.rows)
    {
        commands.push_back(row.row.command);
        entered.push_back(row.entered);
    }
    XSFLIB_CHECK((commands == std::vector<uint32_t>{0, 5, 7, 10, 15, 17, 14}));
    XSFLIB_CHECK((entered == std::vector<bool>{true, false, false, false, true, false, true}));
    SEQLIB_REQUIRE(first.rows.size() > 4);
    XSFLIB_CHECK(first.rows[4].depth == 1 && first.tracks == (std::vector<int>{0, 1}));
    XSFLIB_CHECK(Cell(first, 0, ListColumn::kComment) == "Song" && Cell(first, 3, ListColumn::kData1) == "Sub");
    XSFLIB_CHECK(second.rows.size() == 1 && second.rows[0].row.command == 14 && second.name == "Track 1");
    XSFLIB_CHECK(missing.rows.empty() && !missing.unavailable.empty());
}

// The commands inside a loop sit one level further in, from the loop's start to its end. The loop's second pass starts
// a stretch of its own.
void TestDepthOfLoops()
{
    const std::vector<uint8_t> bytes = TestArchive(kLoopBytecode, {{"Song", 0}});
    const hokuto::format::File file = hokuto::format::ImportSwitchArchive(bytes);
    const std::shared_ptr<const sar::SoundArchive> archive = hokuto::song::LoadArchive(bytes, "test.bfsar");
    const hokuto::song::Sequence song = hokuto::song::ReadSequence(*archive, 0);

    const ListTrack track = MakeListTrack(file, 0, song, 0);

    std::vector<uint32_t> commands;
    std::vector<int> depths;
    for (const ListRow& row : track.rows)
    {
        commands.push_back(row.row.command);
        depths.push_back(row.depth);
    }
    XSFLIB_CHECK((commands == std::vector<uint32_t>{0, 2, 5, 2, 5, 6}));
    XSFLIB_CHECK((depths == std::vector<int>{0, 1, 1, 1, 1, 0}));
}

// A call in a loop runs in each pass that ntdlib's performer plays. The List Editor's rows stop at the end of the
// loop's first pass. The runs of the subroutine stop there too: a later run would show no rows. The subroutine's part
// starts at the call's row.
void TestRunsOfALoopedCall()
{
    const std::vector<uint8_t> bytes = TestArchive(kLoopedCallBytecode, {{"Song", 0}, {"Sub", 10}});
    const hokuto::format::File file = hokuto::format::ImportSwitchArchive(bytes);
    const std::shared_ptr<const sar::SoundArchive> archive = hokuto::song::LoadArchive(bytes, "test.bfsar");
    const hokuto::song::Sequence song = hokuto::song::ReadSequence(*archive, 0);
    XSFLIB_CHECK(song.loop && song.loop->start == 12 && song.loop->end == 36);

    const std::vector<ListCall> runs = SubroutineRuns(song, 10);
    XSFLIB_CHECK(runs.size() == 1 && runs[0].track == 0 && runs[0].tick == 12);
    const ListTrack alone = WithSubroutine(MakeListTrack(file, 0, song, 0), ListSubroutine{"Sub", 10, runs, 0, 1});
    XSFLIB_CHECK(alone.rows.size() == 2 && alone.rows[0].row.command == 10 && alone.rows[1].row.command == 13);

    // The subroutine's part starts at the call's row, the command at 2.
    SEQLIB_REQUIRE(!song.tracks.empty());
    const ListTrack track = MakeListTrack(file, 0, song, 0);
    const std::optional<std::size_t> call = RowOfPart(track, song.tracks[0], 0, 12);
    XSFLIB_CHECK(!song.tracks[0].notes.empty() && call && track.rows[*call].row.command == 2);
    XSFLIB_CHECK(CallsOfRuns(song, runs) == 1);
}

// Each label has a row before its command, and its comment counts the calls that go there. The commands' comments then
// leave the labels out. A label's row takes a new name, and the menus' edits name the label. The selected commands
// become a subroutine named after the label before them, as deep as the call's run goes. A selected call takes the rows
// of its run with it. The call's run shows by itself, with the sound's calls counted apart. A part of the arrangement
// starts at its first note's row.
void TestLabelsAndSubroutines()
{
    const std::vector<uint8_t> bytes = TestArchive();
    const hokuto::format::File file = hokuto::format::ImportSwitchArchive(bytes);
    const std::shared_ptr<const sar::SoundArchive> archive = hokuto::song::LoadArchive(bytes, "test.bfsar");
    const hokuto::song::Sequence song = hokuto::song::ReadSequence(*archive, 0);
    const ListTrack plain = MakeListTrack(file, 0, song, 0);
    const ListTrack track = WithLabelRows(plain);

    // The rows: the label Song, the commands at 0, 5, 7 and 10, the label Sub, the commands at 15 and 17, and the end.
    std::vector<std::string> labels;
    for (const ListRow& row : track.rows)
    {
        labels.push_back(row.label);
    }
    XSFLIB_CHECK((labels == std::vector<std::string>{"Song", "", "", "", "", "Sub", "", "", ""}));
    XSFLIB_CHECK(Cell(track, 0, ListColumn::kType) == "Label" && Cell(track, 0, ListColumn::kData1) == "Song");
    XSFLIB_CHECK(Cell(track, 5, ListColumn::kComment) == "1 call" && Cell(track, 0, ListColumn::kComment).empty());
    XSFLIB_CHECK(Cell(track, 1, ListColumn::kComment).empty() && Cell(plain, 0, ListColumn::kComment) == "Song");
    XSFLIB_CHECK(Typable(track, track.rows[0], ListColumn::kData1) && !NamesTarget(track.rows[0], ListColumn::kData1));
    XSFLIB_CHECK(!Typable(track, track.rows[0], ListColumn::kStart) && IsLabel(track.rows[5]));

    const std::optional<ListEdit> renamed = TypedListEdit(track, 0, ListColumn::kData1, " Intro ");
    SEQLIB_REQUIRE(renamed && renamed->action == ListEdit::Action::kRenameLabel && renamed->name == "Rename Label");
    XSFLIB_CHECK(renamed->label == "Song" && renamed->new_name == "Intro" && renamed->refused.empty());
    XSFLIB_CHECK(!Refused(track, 0, ListColumn::kData1, "Sub").empty());
    XSFLIB_CHECK(!Refused(track, 0, ListColumn::kData1, "1st").empty());

    const std::optional<ListEdit> added = LabelEdit(track, 2);
    const std::optional<ListEdit> moved = LabelMoveEdit(track, 0, 3);
    const std::optional<ListEdit> removed = LabelRemovalEdit(track, 5);
    SEQLIB_REQUIRE(added && added->action == ListEdit::Action::kAddLabel && added->label == "Label");
    XSFLIB_CHECK(added->rows == std::vector<std::size_t>{2} && !LabelEdit(track, 0));
    SEQLIB_REQUIRE(moved && moved->action == ListEdit::Action::kMoveLabel && moved->label == "Song");
    XSFLIB_CHECK(moved->rows == std::vector<std::size_t>{3} && !LabelMoveEdit(track, 0, 1) &&
                 !LabelMoveEdit(track, 2, 3));
    XSFLIB_CHECK(removed && removed->label == "Sub" && !LabelRemovalEdit(track, 1));

    // The note wait and the key, then the key and the call. The call's run goes one call deeper.
    const std::optional<ListEdit> two = SubroutineEdit(track, {5, 7});
    const std::optional<ListEdit> call = SubroutineEdit(track, {7, 10});
    SEQLIB_REQUIRE(two && two->action == ListEdit::Action::kToSubroutine && two->name == "Convert to Subroutine");
    XSFLIB_CHECK(two->rows == (std::vector<std::size_t>{2, 3}) && two->label == "Song_sub" && two->depth == 0);
    XSFLIB_CHECK(call && call->rows == (std::vector<std::size_t>{3, 4}) && call->depth == 1);
    XSFLIB_CHECK(!SubroutineEdit(track, {}));

    // A selected call takes the rows of its run with it. The run's rows by themselves stay a selection.
    const std::optional<ListEdit> with_run = SubroutineEdit(track, {7, 10, 15, 17});
    const std::optional<ListEdit> run = SubroutineEdit(track, {15, 17});
    XSFLIB_CHECK(with_run && with_run->rows == (std::vector<std::size_t>{3, 4}) && with_run->depth == 1);
    XSFLIB_CHECK(run && run->rows == (std::vector<std::size_t>{6, 7}) && run->depth == 1);

    const std::optional<ListEdit> dissolved = DissolveEdit(track, 4);
    const std::optional<ListEdit> copied = RealCopyEdit(track, 4);
    XSFLIB_CHECK(dissolved && dissolved->action == ListEdit::Action::kDissolveCall &&
                 dissolved->name == "Dissolve Call");
    XSFLIB_CHECK(copied && copied->action == ListEdit::Action::kCopySubroutine &&
                 copied->name == "Convert to Real Copy");
    XSFLIB_CHECK(!DissolveEdit(track, 3) && !RealCopyEdit(track, 5));

    // The call's run, alone.
    const std::vector<ListCall> runs = SubroutineRuns(song, 15);
    XSFLIB_CHECK(RunOfCall(track, 4) == 1u && !RunOfCall(track, 3));
    XSFLIB_CHECK((runs == std::vector<ListCall>{{0, 1, 0}}) && SubroutineRuns(song, 7).empty());
    const ListTrack alone = WithSubroutine(plain, ListSubroutine{"Sub", 15, runs, 0, 1});
    SEQLIB_REQUIRE(alone.rows.size() == 2);
    XSFLIB_CHECK(alone.rows[0].row.command == 15 && alone.rows[1].row.command == 17);
    XSFLIB_CHECK(alone.rows[0].depth == 0 && alone.subroutine && alone.subroutine->name == "Sub");
    XSFLIB_CHECK(WithSubroutine(plain, ListSubroutine{"Sub", 15, runs, 1, 1}).rows.empty());

    // The toolbar counts the sound's calls apart from the others in the sequence.
    XSFLIB_CHECK(CallsOfRuns(song, runs) == 1);
    const ListSubroutine one{"Sub", 15, runs, 0, 1, 1};
    const ListSubroutine more{"Sub", 15, runs, 0, 63, 1};
    XSFLIB_CHECK(CallsText(one) == "1 call in this sound" &&
                 CallsTooltip(one).starts_with("The shown sound runs every"));
    XSFLIB_CHECK(CallsText(more) == "1 call in this sound, 62 elsewhere in the sequence");
    XSFLIB_CHECK(CallsTooltip(more).find("The other 62 lie in the commands of other sounds") != std::string::npos);

    // A part outside calls starts at its first note's row. A part without its note starts at the first row at its start
    // or later: the return at tick 12.
    SEQLIB_REQUIRE(!song.tracks.empty());
    const std::optional<std::size_t> note = RowOfPart(track, song.tracks[0], 0, 0);
    const std::optional<std::size_t> later = RowOfPart(track, song.tracks[0], 99, 12);
    SEQLIB_REQUIRE(note && *note < track.rows.size() && later && *later < track.rows.size());
    XSFLIB_CHECK(track.rows[*note].row.command == 7 && track.rows[*later].row.command == 17);
    XSFLIB_CHECK(!RowOfPart(track, song.tracks[0], 99, 1000));
}

// A command that the player of the engine that plays the sound skips: the 3DS's player has one LFO, and the Switch's
// four. The engine is the console's unless the track names another. On another console's engine, a command that the
// file's console skips shows grey too: the sound's conversion leaves it out. The row names the console that skips it. A
// label's row runs nothing.
void TestSkipped()
{
    ListTrack track = HandMade();
    track.engine_name = "3DS";
    track.file_engine_name = "Switch";
    XSFLIB_CHECK(!Skipped(track, track.rows[kLfo]) && !Skipped(track, track.rows[kVolume]));
    track.engine = hokuto::song::Console{.flavor = ntdlib::sar::Flavor::kCtr};
    XSFLIB_CHECK(Skipped(track, track.rows[kLfo]) && SkippingEngine(track, track.rows[kLfo]) == "3DS");
    XSFLIB_CHECK(!Skipped(track, track.rows[kVolume]) && !Skipped(track, track.rows[kSend]));
    ListRow label = track.rows[kLfo];
    label.label = "Loop";
    XSFLIB_CHECK(!Skipped(track, label));

    ListTrack ctr = HandMade();
    ctr.console = hokuto::song::Console{.flavor = ntdlib::sar::Flavor::kCtr};
    ctr.engine_name = "Switch";
    ctr.file_engine_name = "3DS";
    XSFLIB_CHECK(Skipped(ctr, ctr.rows[kLfo]) && SkippingEngine(ctr, ctr.rows[kLfo]) == "3DS");
    ctr.engine = hokuto::song::Console{.flavor = ntdlib::sar::Flavor::kNx};
    XSFLIB_CHECK(Skipped(ctr, ctr.rows[kLfo]) && SkippingEngine(ctr, ctr.rows[kLfo]) == "3DS");
    XSFLIB_CHECK(!Skipped(ctr, ctr.rows[kVolume]));
}

} // namespace

int main()
{
    TestText();
    TestTypedValues();
    TestTypedNotesAndLengths();
    TestTypedIndices();
    TestTargetsInsertsAndRemovals();
    TestTrackOfArchive();
    TestDepthOfLoops();
    TestLabelsAndSubroutines();
    TestRunsOfALoopedCall();
    TestSkipped();

    return xsflib::testing::Summary("list_edits");
}
