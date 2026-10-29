// SPDX-License-Identifier: MIT

// Tests what the arrangement's tempo row shows (src/app/tempo_row.h). A small Switch archive gives the commands of a
// sound's two tracks that set the tempo, how far they can move, and the commands that drags of them give. Songs made by
// hand give the row's span and line. Points made by hand give the labels that fit beside them.

#include "app/tempo_row.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/commands/units.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/testing/sar_files.h"
#include "xsflib/testing/check.h"

#include "app/key_edits.h"
#include "app/song_view.h"
#include "format/archive.h"
#include "format/file.h"
#include "song/archive.h"
#include "song/sequence.h"

using hokuto::app::FittingLabels;
using hokuto::app::kTimedByNotes;
using hokuto::app::SongView;
using hokuto::app::TempoChangeView;
using hokuto::app::TempoCommand;
using hokuto::app::TempoLine;
using hokuto::app::TempoMove;
using hokuto::app::TempoPoints;
using hokuto::app::TempoPointView;
using hokuto::app::TempoSpan;
using hokuto::app::TempoSpanOf;
using ntdlib::commands::Command;
using ntdlib::commands::Kind;
using ntdlib::commands::kOne;
using ntdlib::commands::Source;

namespace
{

namespace sar = ntdlib::sar;
namespace sar_files = ntdlib::testing::sar_files;

// The sound's bytecode. Each command's offset comes first in its comment. The Switch's sequences give arguments in
// little-endian order, and a tempo in 16 bits. Each note waits for its length. Track 0 sets the tempo at ticks 0 and
// 48, and takes it from variable 5 (the variable prefix, 0xA1) at tick 96. Track 1 sets it at tick 24, and at tick 48
// if the last comparison held (the prefix 0xA2). No comparison came before, and the tempo stays. Track 1 then calls a
// subroutine that sets the tempo at tick 60. After a random wait (the prefix 0xA0) from 1 to 8 ticks, it sets the tempo
// once more and ends.
const std::vector<uint8_t> kBytecode = {
    0xB0, 48,                           // 0: timebase 48
    0xE1, 120,  0x00,                   // 2: tempo 120
    0x88, 1,    23,   0x00, 0x00,       // 5: open track 1 at 23
    60,   100,  48,                     // 10: key 60 for 48 ticks
    0xE1, 140,  0x00,                   // 13: tempo 140
    60,   100,  48,                     // 16: key 60 for 48 ticks
    0xA1, 0xE1, 5,                      // 19: tempo from variable 5
    0xFF,                               // 22: the end
    0x80, 24,                           // 23: track 1: wait 24 ticks
    0xE1, 90,   0x00,                   // 25: tempo 90
    64,   100,  24,                     // 28: key 64 for 24 ticks
    0xA2, 0xE1, 100,  0x00,             // 31: if the last comparison held, tempo 100
    0x80, 12,                           // 35: wait 12 ticks
    0x8A, 51,   0x00, 0x00,             // 37: call 51
    0xA0, 0x80, 1,    0x00, 8,    0x00, // 41: wait from 1 to 8 ticks at random
    0xE1, 80,   0x00,                   // 47: tempo 80
    0xFF,                               // 50: the end
    0xE1, 110,  0x00,                   // 51: the subroutine: tempo 110
    0xFD,                               // 54: return
};

// A sound whose note wait comes from variable 3 (the prefix 0xA1 before 0xC7, notewait_on). Whether its note waits for
// its length isn't known, and its tempo can't move.
const std::vector<uint8_t> kWaitFromVariable = {
    0xB0, 48,         // 0: timebase 48
    0xA1, 0xC7, 3,    // 2: the note wait from variable 3
    0xE1, 100,  0x00, // 5: tempo 100
    60,   100,  48,   // 8: key 60 for 48 ticks
    0x80, 48,         // 11: wait 48 ticks
    0xFF,             // 13: the end
};

// The archive, with the sound's bytecode `bytecode`. The bank plays a looped wave for every key.
std::vector<uint8_t> TestArchive(const std::vector<uint8_t>& bytecode = kBytecode)
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
    spec.files = {{sar_files::Sequence(false, bytecode, {{"Song", 0}}), "", false},
                  {sar_files::Bank(false, bank), "", false},
                  {sar_files::WaveArchive(false, {sar_files::Wave(false, wave)}), "", false}};

    return sar_files::Archive(false, spec);
}

// A point of the tempo row at `tick` with the tempo `bpm`, from the command at offset `command` in the first stretch of
// track `track`. The track has no name of its own in the sequence, and takes its number's.
TempoPointView Point(int32_t tick, double bpm, int track, uint32_t command)
{
    return {
        .tick = tick, .bpm = bpm, .track = track, .command = command, .track_name = "Track " + std::to_string(track)};
}

// `point`, with room for its command to move `earlier` ticks earlier and `later` ticks later.
TempoPointView WithRoom(TempoPointView point, uint32_t earlier, uint32_t later)
{
    point.earlier = earlier;
    point.later = later;

    return point;
}

// The points come in the order of their ticks: track 0's tempo at tick 0, track 1's at tick 24, track 0's at tick 48,
// track 1's conditional tempo at tick 48, track 1's tempo in the subroutine at tick 60, and track 0's at tick 96. At
// tick 48, the tracks come in the order of their numbers. The last point has no value. It names its variable instead.
// The subroutine's command ran in track 1's second stretch, and each other command in its track's first. The tempo
// after the random wait has no tick that the commands give, and no point. A file without the sequence gives no points.
//
// Like a note, a command moves inside its block. Each note waits for its length here, and every length is known. Track
// 0's block runs from tick 0 to its end at tick 96. Track 1's runs from tick 0 up to the call at tick 60. The
// subroutine holds no wait, and its tempo has no room. The conditional tempo and the tempo from a variable can't move,
// and say why.
void TestPoints()
{
    const std::vector<uint8_t> bytes = TestArchive();
    const hokuto::format::File file = hokuto::format::ImportSwitchArchive(bytes);
    const std::shared_ptr<const sar::SoundArchive> archive = hokuto::song::LoadArchive(bytes, "test.bfsar");
    const hokuto::song::Sequence song = hokuto::song::ReadSequence(*archive, 0);

    const std::vector<TempoPointView> points = TempoPoints(file, 0, song);
    const std::vector<TempoPointView> missing = TempoPoints(hokuto::format::File{}, 0, song);

    const std::vector<TempoPointView> expected = {
        WithRoom(Point(0, 120.0, 0, 2), 0, 96),
        WithRoom(Point(24, 90.0, 1, 25), 24, 36),
        WithRoom(Point(48, 140.0, 0, 13), 48, 48),
        {.tick = 48,
         .bpm = 100.0,
         .track = 1,
         .command = 31,
         .conditional = true,
         .track_name = "Track 1",
         .fixed = "Hokuto can't move a command that runs only if the last comparison held."},
        {.tick = 60, .bpm = 110.0, .track = 1, .command = 51, .stretch = 1, .track_name = "Track 1"},
        {.tick = 96,
         .command = 19,
         .source = "Variable 5",
         .track_name = "Track 0",
         .fixed = "Hokuto can't move a command that reads or changes a variable."}};
    XSFLIB_CHECK(points == expected);
    XSFLIB_CHECK(missing.empty());
}

// A tempo after a note wait from a variable stays at its tick, and says that the track's notes set its timing.
void TestTimedByNotes()
{
    const std::vector<uint8_t> bytes = TestArchive(kWaitFromVariable);
    const hokuto::format::File file = hokuto::format::ImportSwitchArchive(bytes);
    const std::shared_ptr<const sar::SoundArchive> archive = hokuto::song::LoadArchive(bytes, "test.bfsar");
    const hokuto::song::Sequence song = hokuto::song::ReadSequence(*archive, 0);

    const std::vector<TempoPointView> points = TempoPoints(file, 0, song);

    SEQLIB_REQUIRE(points.size() == 1);
    XSFLIB_CHECK(points[0].tick == 0 && points[0].command == 5 && points[0].fixed == kTimedByNotes);
}

// The span runs from 20 below the lowest tempo to 20 above the highest, out to multiples of 10. The song's tempo, the
// changes and the points' values count. A span never goes below 0. A song without changes or points spans its tempo. A
// song whose first change comes after its start spans its tempo at the start too.
void TestSpan()
{
    SongView song;
    song.tempo_changes = {{0, 120.0}, {24, 90.0}, {48, 140.0}};
    song.tempo_points = {Point(96, 75.0, 0, 19), {.tick = 100, .command = 22, .source = "Variable 5"}};
    SongView slow;
    slow.tempo = 15.0;
    slow.tempo_changes = {{0, 15.0}};
    SongView plain;
    plain.tempo = 132.0;
    SongView late;
    late.tempo_changes = {{24, 90.0}};

    const TempoSpan span = TempoSpanOf(song);
    const TempoSpan slow_span = TempoSpanOf(slow);
    const TempoSpan plain_span = TempoSpanOf(plain);
    const TempoSpan late_span = TempoSpanOf(late);

    XSFLIB_CHECK((span == TempoSpan{50.0, 160.0}));
    XSFLIB_CHECK((slow_span == TempoSpan{0.0, 40.0}));
    XSFLIB_CHECK((plain_span == TempoSpan{110.0, 160.0}));
    XSFLIB_CHECK((late_span == TempoSpan{70.0, 140.0}));
}

// The line starts at the song's tempo and steps at each later change. A drag of a point shows its tempo at each run of
// its command, from the run's tick up to the next change: at the change in the run's tick, or at a new step where the
// tick has none. Track 1 runs the command at offset 25 twice. A drag of a point that a later command overrides in the
// same tick leaves the line as it is. So does a drag of a point that isn't among the song's points.
void TestLine()
{
    SongView song;
    song.tempo = 120.0;
    song.tempo_changes = {{0, 120.0}, {24, 90.0}, {48, 150.0}, {60, 90.0}};
    song.tempo_points = {Point(0, 120.0, 0, 2),
                         Point(24, 90.0, 1, 25),
                         Point(36, 90.0, 1, 28),
                         Point(48, 140.0, 0, 13),
                         Point(48, 150.0, 1, 31),
                         Point(60, 90.0, 1, 25),
                         {.tick = 72, .command = 19, .source = "Variable 5"}};
    const auto drag = [&song](std::size_t point, double bpm)
    {
        return TempoLine(song, TempoMove{song.tempo_points[point], *song.tempo_points[point].bpm, bpm});
    };

    const std::vector<TempoChangeView> plain = TempoLine(song, std::nullopt);
    const std::vector<TempoChangeView> first = drag(0, 110.0);
    const std::vector<TempoChangeView> twice = drag(1, 100.0);
    const std::vector<TempoChangeView> repeated = drag(2, 95.0);
    const std::vector<TempoChangeView> overridden = drag(3, 200.0);
    const std::vector<TempoChangeView> last = drag(4, 160.0);
    const std::vector<TempoChangeView> missing = TempoLine(song, TempoMove{Point(12, 90.0, 0, 7), 90.0, 100.0});

    XSFLIB_CHECK((plain == std::vector<TempoChangeView>{{0, 120.0}, {24, 90.0}, {48, 150.0}, {60, 90.0}}));
    XSFLIB_CHECK((first == std::vector<TempoChangeView>{{0, 110.0}, {24, 90.0}, {48, 150.0}, {60, 90.0}}));
    XSFLIB_CHECK((twice == std::vector<TempoChangeView>{{0, 120.0}, {24, 100.0}, {48, 150.0}, {60, 100.0}}));
    XSFLIB_CHECK(
        (repeated == std::vector<TempoChangeView>{{0, 120.0}, {24, 90.0}, {36, 95.0}, {48, 150.0}, {60, 90.0}}));
    XSFLIB_CHECK(overridden == plain && missing == plain);
    XSFLIB_CHECK((last == std::vector<TempoChangeView>{{0, 120.0}, {24, 90.0}, {48, 160.0}, {60, 90.0}}));
}

// A drag that moves a point moves each run of its command. Its tempo holds from the run's new tick up to the next
// change. At the old tick, the command before the run in that tick decides the tempo, or else the tempo before the tick
// holds on. At tick 0, the tempo with which the sequence starts holds: 120. A run that a later command overrides in its
// tick takes its tempo to its new tick. A tempo without one value takes the tempo that the line showed at its tick.
void TestMovedLine()
{
    SongView song;
    song.tempo = 120.0;
    song.tempo_changes = {{0, 120.0}, {24, 90.0}, {48, 150.0}, {60, 90.0}};
    song.tempo_points = {Point(0, 120.0, 0, 2), Point(24, 90.0, 1, 25), Point(48, 140.0, 0, 13),
                         Point(48, 150.0, 1, 31), Point(60, 90.0, 1, 25)};
    SongView start;
    start.tempo = 100.0;
    start.tempo_changes = {{0, 100.0}, {48, 150.0}};
    start.tempo_points = {Point(0, 100.0, 0, 2), Point(48, 150.0, 0, 13)};
    SongView random;
    random.tempo_changes = {{24, 85.0}};
    random.tempo_points = {{.tick = 24, .command = 41, .source = "Random 80 to 100"}};

    const auto move = [](const SongView& moved, std::size_t point, double bpm, int32_t shift)
    {
        const TempoPointView& dragged = moved.tempo_points[point];
        return TempoLine(moved, TempoMove{dragged, dragged.bpm.value_or(0.0), bpm, shift});
    };

    const std::vector<TempoChangeView> twice = move(song, 1, 100.0, 6);
    const std::vector<TempoChangeView> overriding = move(song, 3, 150.0, -6);
    const std::vector<TempoChangeView> overridden = move(song, 2, 140.0, -6);
    const std::vector<TempoChangeView> first = move(start, 0, 100.0, 24);
    const std::vector<TempoChangeView> drawn = move(random, 0, 0.0, 24);

    XSFLIB_CHECK((twice == std::vector<TempoChangeView>{{0, 120.0}, {30, 100.0}, {48, 150.0}, {66, 100.0}}));
    XSFLIB_CHECK(
        (overriding == std::vector<TempoChangeView>{{0, 120.0}, {24, 90.0}, {42, 150.0}, {48, 140.0}, {60, 90.0}}));
    XSFLIB_CHECK(
        (overridden == std::vector<TempoChangeView>{{0, 120.0}, {24, 90.0}, {42, 140.0}, {48, 150.0}, {60, 90.0}}));
    XSFLIB_CHECK((first == std::vector<TempoChangeView>{{0, 120.0}, {24, 100.0}, {48, 150.0}}));
    XSFLIB_CHECK((drawn == std::vector<TempoChangeView>{{0, 120.0}, {48, 85.0}}));
}

// A drag gives the tempo command its new tempo, at the nearest whole tempo that the Switch's player plays: 1 at least
// and 1023 at most. The player holds a tempo to 1023. A tempo from a variable, a command of another kind, an offset
// where no command starts, a tempo that isn't a number and a file without an archive give nothing.
void TestCommand()
{
    const hokuto::format::File file = hokuto::format::ImportSwitchArchive(TestArchive());
    const TempoPointView point = Point(48, 140.0, 0, 13);

    const std::optional<Command> faster = TempoCommand(file, 0, point, 150.6);
    const std::optional<Command> slower = TempoCommand(file, 0, point, 149.4);
    const std::optional<Command> highest = TempoCommand(file, 0, point, 1.0e7);
    const std::optional<Command> lowest = TempoCommand(file, 0, point, -5.0);
    const std::optional<Command> variable = TempoCommand(file, 0, {.tick = 96, .command = 19}, 100.0);
    const std::optional<Command> note = TempoCommand(file, 0, Point(48, 140.0, 0, 10), 100.0);
    const std::optional<Command> nowhere = TempoCommand(file, 0, Point(48, 140.0, 0, 14), 100.0);
    const std::optional<Command> unknown = TempoCommand(file, 0, point, std::numeric_limits<double>::quiet_NaN());
    const std::optional<Command> empty = TempoCommand(hokuto::format::File{}, 0, point, 100.0);

    SEQLIB_REQUIRE(faster && slower && highest && lowest);
    XSFLIB_CHECK(faster->kind == Kind::kTempo && faster->value.source == Source::kValue);
    XSFLIB_CHECK(faster->value.value == 151 * kOne && slower->value.value == 149 * kOne);
    XSFLIB_CHECK(highest->value.value == 1023 * kOne && lowest->value.value == kOne);
    XSFLIB_CHECK(!variable && !note && !nowhere && !unknown && !empty);
}

// A label fits where no other point lies from its point to its end, a gap of 6 pixels and its width on. Of points a few
// pixels apart, only the last draws its label. Two points at one place draw none. The points can come in any order. A
// label that ends at the next point doesn't fit.
void TestFittingLabels()
{
    const std::vector<float> kXs = {100.0f, 0.0f, 50.0f, 4.0f, 2.0f, 50.0f, 140.0f, 200.0f, 240.0f};
    const std::vector<float> kWidths = {20.0f, 20.0f, 30.0f, 20.0f, 20.0f, 30.0f, 0.0f, 34.0f, 0.0f};

    const std::vector<bool> fits = FittingLabels(kXs, kWidths, 6.0f);

    XSFLIB_CHECK((fits == std::vector<bool>{true, false, false, true, false, false, true, false, true}));
    XSFLIB_CHECK(FittingLabels({}, {}, 6.0f).empty());
}

} // namespace

int main()
{
    TestPoints();
    TestTimedByNotes();
    TestSpan();
    TestLine();
    TestMovedLine();
    TestCommand();
    TestFittingLabels();

    return xsflib::testing::Summary("tempo_row");
}
