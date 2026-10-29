// SPDX-License-Identifier: MIT

// Tests the places where a song's tracks read variables or branch (src/app/markers.h). Commands made by hand give the
// variables that each kind of command reads. A small Switch archive then gives the places of a sound's two tracks.

#include "app/markers.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/commands/units.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/testing/sar_files.h"
#include "xsflib/testing/check.h"

#include "app/song_view.h"
#include "format/archive.h"
#include "format/file.h"
#include "song/archive.h"
#include "song/sequence.h"

using hokuto::app::FlagSpan;
using hokuto::app::FlagWidths;
using hokuto::app::LogicMarkers;
using hokuto::app::LogicMarkerView;
using hokuto::app::MarkerCommand;
using hokuto::app::VariablesLabel;
using hokuto::app::VariablesRead;
using ntdlib::commands::Argument;
using ntdlib::commands::Command;
using ntdlib::commands::Kind;
using ntdlib::commands::kOne;
using ntdlib::commands::Source;

namespace
{

namespace sar = ntdlib::sar;
namespace sar_files = ntdlib::testing::sar_files;

// A command of `kind` that addresses variable `index`, with the value `value`.
Command Addressing(Kind kind, uint8_t index, Argument value)
{
    Command command;
    command.kind = kind;
    command.index = index;
    command.value = value;

    return command;
}

// A comparison and each operation that works out a new value from the old one read their variable. An argument reads
// the variable that it comes from. A set, a random number and a not read only the variables of their arguments. A print
// reads none.
void TestVariablesRead()
{
    const Argument kValueOne{Source::kValue, kOne, 0};
    const Argument kVariableFour{Source::kVariable, 4, 0};
    Command timed = Addressing(Kind::kVolume, 0, Argument{Source::kVariable, 5, 0});
    timed.time = Argument{Source::kVariable, 7, 0};

    const std::vector<int> compared = VariablesRead(Addressing(Kind::kCompareGreater, 3, kVariableFour));
    std::vector<std::vector<int>> updated;
    for (const Kind kind :
         {Kind::kAddVariable, Kind::kSubtractVariable, Kind::kMultiplyVariable, Kind::kDivideVariable,
          Kind::kShiftVariable, Kind::kAndVariable, Kind::kOrVariable, Kind::kXorVariable, Kind::kModuloVariable})
    {
        updated.push_back(VariablesRead(Addressing(kind, 2, kValueOne)));
    }
    const std::vector<int> set = VariablesRead(Addressing(Kind::kSetVariable, 2, kVariableFour));
    const std::vector<int> random = VariablesRead(Addressing(Kind::kRandomVariable, 2, kValueOne));
    const std::vector<int> negated = VariablesRead(Addressing(Kind::kNotVariable, 2, kValueOne));
    const std::vector<int> printed = VariablesRead(Addressing(Kind::kPrintVariable, 2, kValueOne));
    const std::vector<int> volume = VariablesRead(timed);

    XSFLIB_CHECK((compared == std::vector<int>{3, 4}));
    XSFLIB_CHECK((updated == std::vector<std::vector<int>>(9, std::vector<int>{2})));
    XSFLIB_CHECK((set == std::vector<int>{4}));
    XSFLIB_CHECK(random.empty() && negated.empty() && printed.empty());
    XSFLIB_CHECK((volume == std::vector<int>{5, 7}));
}

// A label names each variable once, in order.
void TestLabel()
{
    XSFLIB_CHECK(VariablesLabel({}).empty());
    XSFLIB_CHECK(VariablesLabel(std::vector<int>{3}) == "Var 3");
    XSFLIB_CHECK(VariablesLabel(std::vector<int>{3, 14}) == "Var 3, 14");
}

// The marker row's flags stop 2 pixels short of the next flag to their right, of either kind, and keep 2 pixels at
// least. A place at pixel 100 whose label takes 120 pixels, with a change at pixel 160, stops at pixel 158. Of the two
// flags at pixel 300, the later keeps its width. The flags come in any order.
void TestFlagWidths()
{
    const std::vector<FlagSpan> kFlags = {{160.0f, 50.0f}, {100.0f, 120.0f}, {300.0f, 30.0f},
                                          {300.0f, 60.0f}, {420.0f, 90.0f},  {0.0f, 40.0f}};
    const std::vector<float> widths = FlagWidths(kFlags, 2.0f, 2.0f);

    XSFLIB_CHECK((widths == std::vector<float>{50.0f, 58.0f, 2.0f, 60.0f, 90.0f, 40.0f}));
    for (std::size_t i = 0; i < kFlags.size(); i++)
    {
        for (std::size_t j = 0; j < kFlags.size(); j++)
        {
            // No flag reaches past the start of a flag that starts to its right.
            XSFLIB_CHECK(kFlags[j].x <= kFlags[i].x || kFlags[i].x + widths[i] <= kFlags[j].x - 2.0f);
        }
    }
    XSFLIB_CHECK(FlagWidths({}, 2.0f, 2.0f).empty());
}

// The sound's bytecode. Each command's offset comes first in its comment. The Switch's sequences give arguments in
// little-endian order. Each note waits for its length. Track 0 compares variable 3 with 1 and, if they're equal (the if
// prefix, 0xA2), jumps to Skip. The variables start at -1. The jump therefore doesn't run. After its note, track 0
// takes its volume from variable 5 (the variable prefix, 0xA1). It then compares variable 3 with 2 in each of a loop's
// two passes. Track 1 adds 1 to variable 4, and waits for as many ticks as variable 6 holds. The comparison after the
// wait runs at tick 24, the tick of the note after it. After the note, track 1 takes its pan from variable 5 if the
// comparison held. Sets, a random number and a not read nothing.
const std::vector<uint8_t> kBytecode = {
    0xB0, 48,                     // 0: timebase 48
    0xE1, 120,  0x00,             // 2: tempo 120
    0x88, 1,    54,   0x00, 0x00, // 5: open track 1 at 54
    0xF0, 0x80, 5,    100,  0x00, // 10: set variable 5 to 100
    0xF0, 0x90, 3,    1,    0x00, // 15: compare variable 3 with 1
    0xA2, 0x89, 53,   0x00, 0x00, // 20: if the comparison held, jump to 53
    60,   100,  48,               // 25: key 60 for 48 ticks
    0xA1, 0xC1, 5,                // 28: volume from variable 5
    0x80, 12,                     // 31: wait 12 ticks
    0xD4, 2,                      // 33: a loop of two passes
    0xF0, 0x90, 3,    2,    0x00, // 35: compare variable 3 with 2
    0x80, 12,                     // 40: wait 12 ticks
    0xFC,                         // 42: the loop's end
    0xF0, 0x86, 7,    3,    0x00, // 43: set variable 7 to a random number
    0xF0, 0x8A, 8,    0,    0x00, // 48: variable 8 takes the not of 0
    0xFF,                         // 53: Skip: the end
    0xF0, 0x81, 4,    1,    0x00, // 54: track 1: add 1 to variable 4
    0xF0, 0x80, 6,    24,   0x00, // 59: set variable 6 to 24
    0xA1, 0x80, 6,                // 64: wait for variable 6's ticks
    0xF0, 0x95, 4,    0,    0x00, // 67: compare variable 4 with 0, unequal
    64,   100,  24,               // 72: key 64 for 24 ticks
    0xA2, 0xA1, 0xC0, 5,          // 75: if the comparison held, pan from variable 5
    0xFF,                         // 79: the end
};

// The archive. The bank plays a looped wave for every key.
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
    song.banks = {0};
    song.allocate_track_flags = 3;

    sar_files::ArchiveSpec spec;
    spec.sounds = {song};
    spec.banks = {{"BANK_TEST", 1}};
    spec.wave_archives = {{"WARC_TEST", 2}};
    spec.players = {{"PLAYER_TEST", 1}};
    spec.files = {{sar_files::Sequence(false, kBytecode, {{"Song", 0}, {"Skip", 53}}), "", false},
                  {sar_files::Bank(false, bank), "", false},
                  {sar_files::WaveArchive(false, {sar_files::Wave(false, wave)}), "", false}};

    return sar_files::Archive(false, spec);
}

// The places gather the commands of both tracks at their ticks: the comparison, the jump, the operation and the wait at
// tick 0, the comparison after the wait at tick 24, and the volume and the pan at tick 48. The loop's comparison runs
// at two places, and the places share its set. A file without the sequence gives no sets.
void TestPlaces()
{
    const std::vector<uint8_t> bytes = TestArchive();
    const hokuto::format::File file = hokuto::format::ImportSwitchArchive(bytes);
    const std::shared_ptr<const sar::SoundArchive> archive = hokuto::song::LoadArchive(bytes, "test.bfsar");
    const hokuto::song::Sequence song = hokuto::song::ReadSequence(*archive, 0);

    const std::vector<LogicMarkerView> markers = LogicMarkers(file, 0, song);
    const std::vector<LogicMarkerView> missing = LogicMarkers(hokuto::format::File{}, 0, song);

    const std::vector<MarkerCommand> kStart = {{0, 15, "Track 0: Compare = Variable 3, 1"},
                                               {0, 20, "Track 0: if Jump Skip"},
                                               {1, 54, "Track 1: Add Variable 4, 1"},
                                               {1, 64, "Track 1: Wait Variable 6"}};
    const std::vector<MarkerCommand> kCounted = {{1, 67, "Track 1: Compare ≠ Variable 4, 0"}};
    const std::vector<MarkerCommand> kLater = {{0, 28, "Track 0: Volume Variable 5"},
                                               {1, 75, "Track 1: if Pan Variable 5"}};
    const std::vector<MarkerCommand> kLooped = {{0, 35, "Track 0: Compare = Variable 3, 2"}};
    XSFLIB_CHECK(markers.size() == 4);
    XSFLIB_CHECK((markers.at(0) == LogicMarkerView{{0}, true, {3, 4, 6}, kStart}));
    XSFLIB_CHECK((markers.at(1) == LogicMarkerView{{24}, false, {4}, kCounted}));
    XSFLIB_CHECK((markers.at(2) == LogicMarkerView{{48}, true, {5}, kLater}));
    XSFLIB_CHECK((markers.at(3) == LogicMarkerView{{60, 72}, false, {3}, kLooped}));
    XSFLIB_CHECK(missing.empty());
}

} // namespace

int main()
{
    TestVariablesRead();
    TestLabel();
    TestFlagWidths();
    TestPlaces();

    return xsflib::testing::Summary("markers");
}
