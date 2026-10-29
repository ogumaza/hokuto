// SPDX-License-Identifier: MIT

// Tests the variables of a sequence that comes in from a file (src/edit/variables.h). The new sequence compares
// variable 4 with 1, adds 1 to it and compares it with 2. Each comparison that holds skips a note. It replaces the
// sequence of a small Switch archive's sound, and ntdlib's performer plays the sound with starting values of the
// variables. A moved variable plays as the old number did, and a fixed one as a scene with its value would. The fixed
// values go in after an allocation of tracks at the start. Loops back to the start pass them, and so does a path that
// runs on into the start.

#include "edit/variables.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
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

using hokuto::edit::MappedVariables;
using hokuto::edit::UsesOf;
using hokuto::edit::VariableReaders;
using hokuto::edit::VariableUses;
using hokuto::edit::WithVariables;
using hokuto::format::File;
using ntdlib::commands::Argument;
using ntdlib::commands::Command;
using ntdlib::commands::Kind;
using ntdlib::commands::kOne;
using ntdlib::commands::Label;
using ntdlib::commands::Sequence;
using ntdlib::commands::Source;

namespace
{

namespace sar = ntdlib::sar;
namespace sar_files = ntdlib::testing::sar_files;

// The archive's sequence. It compares variable 3 with 1, and waits 12 ticks if they're equal, before its note.
const std::vector<uint8_t> kOldBytecode = {
    0xB0, 48,                // 0: timebase 48
    0xF0, 0x90, 3,  1, 0x00, // 2: compare variable 3 with 1
    0xA2, 0x80, 12,          // 7: if the comparison held, wait 12 ticks
    67,   100,  24,          // 10: key 67 for 24 ticks
    0xFF,                    // 13: the end
};

// The new sequence. The variables start at -1. Both comparisons then fail, and the three notes play. With variable 4 at
// 1, the first comparison holds, the variable becomes 2, the second comparison holds, and only key 64 plays.
const std::vector<uint8_t> kNewBytecode = {
    0xB0, 48,                // 0: timebase 48
    0xF0, 0x90, 4,  1, 0x00, // 2: compare variable 4 with 1
    0xA2, 0x89, 15, 0, 0,    // 7: if the comparison held, jump to 15
    60,   100,  24,          // 12: key 60 for 24 ticks
    0xF0, 0x81, 4,  1, 0x00, // 15: add 1 to variable 4
    0xF0, 0x90, 4,  2, 0x00, // 20: compare variable 4 with 2
    0xA2, 0x89, 33, 0, 0,    // 25: if the comparison held, jump to 33
    62,   100,  24,          // 30: key 62 for 24 ticks
    64,   100,  24,          // 33: key 64 for 24 ticks
    0xFF,                    // 36: the end
};

// The new sequence's items: the comparisons, the addition, and the jumps with their targets.
constexpr uint32_t kFirstCompare = 1;
constexpr uint32_t kFirstJump = 2;
constexpr uint32_t kAdd = 4;
constexpr uint32_t kSecondCompare = 5;
constexpr uint32_t kSecondJump = 6;

// The archive, with the old sequence. Its bank plays a looped wave for every key.
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

    sar_files::ArchiveSpec spec;
    spec.sounds = {song};
    spec.banks = {{"BANK_TEST", 1}};
    spec.wave_archives = {{"WARC_TEST", 2}};
    spec.players = {{"PLAYER_TEST", 1}};
    spec.files = {{sar_files::Sequence(false, kOldBytecode, {{"Song", 0}}), "", false},
                  {sar_files::Bank(false, bank), "", false},
                  {sar_files::WaveArchive(false, {sar_files::Wave(false, wave)}), "", false}};

    return sar_files::Archive(false, spec);
}

using Archive = std::shared_ptr<const sar::SoundArchive>;

// The archive of `bytes`, read as the program reads it.
Archive Read(std::vector<uint8_t> bytes)
{
    return hokuto::song::LoadArchive(std::move(bytes), "test.bfsar");
}

// Where each sound plays in `file`. The program finds the places the same way.
std::vector<std::optional<hokuto::edit::SoundPlace>> PlacesOf(const File& file)
{
    const std::vector<uint8_t> built = hokuto::format::ExportArchive(file);
    const Archive archive = Read(built);
    return hokuto::edit::SoundPlaces(*archive, hokuto::format::SequencesOfFiles(file, built, *archive),
                                     hokuto::format::StartFieldsOfSounds(file, built));
}

// The commands of kNewBytecode as Replace… reads them.
Sequence NewSequence()
{
    return hokuto::edit::ReadIncoming(sar_files::Sequence(false, kNewBytecode, {{"New", 0}})).sequence;
}

// The keys that the sound plays after it plays `mapped`, with the variables' starting values `values`.
std::vector<uint8_t> Keys(const MappedVariables& mapped, const hokuto::song::StartValues& values = {})
{
    const File file = hokuto::format::ImportSwitchArchive(TestArchive());
    const File replaced =
        hokuto::edit::ReplaceSequence(file, PlacesOf(file), 0, mapped.sequence, mapped.start, {}, "New", 0);
    const Archive archive = Read(hokuto::format::ExportArchive(replaced));
    const hokuto::song::Sequence played = hokuto::song::ReadSequence(*archive, 0, values);
    std::vector<uint8_t> keys;
    for (const hokuto::song::Note& note : played.tracks.at(0).notes)
    {
        keys.push_back(note.key);
    }

    return keys;
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

// The bit of variable `number` in a mask of variables.
uint64_t Bit(int number)
{
    return uint64_t{1} << number;
}

// A comparison of `variable` with `value`.
Command Compare(uint8_t variable, int value)
{
    return Command{.kind = Kind::kCompareEqual, .index = variable, .value = {Source::kValue, value * kOne, 0}};
}

// A jump to item `target`.
Command Jump(uint32_t target)
{
    return Command{.kind = Kind::kJump, .target = target};
}

// A wait of 12 ticks.
Command Wait()
{
    return Command{.kind = Kind::kWait, .value = {Source::kValue, 12 * kOne, 0}};
}

// The new commands read variable 4 and set it: the addition reads it as well. A command that sets a variable without
// reading it, and an argument that takes a variable's value, count too. Commands that the start doesn't reach don't.
void TestUses()
{
    const Sequence sequence = NewSequence();

    const VariableUses uses = UsesOf(sequence, 0);

    XSFLIB_CHECK(sequence.items.size() == 10 && sequence.items[kAdd].kind == Kind::kAddVariable);
    XSFLIB_CHECK(uses.reads == Bit(4) && uses.sets == Bit(4));

    Sequence other;
    other.items = {Command{.kind = Kind::kSetVariable, .index = 20, .value = {Source::kValue, kOne, 0}},
                   Command{.kind = Kind::kVolume, .value = {Source::kVariable, 7, 0}},
                   Command{.kind = Kind::kPan, .time = Argument{Source::kVariable, 8, 0}}, Command{.kind = Kind::kEnd},
                   Compare(9, 1)};

    const VariableUses others = UsesOf(other, 0);

    XSFLIB_CHECK(others.reads == (Bit(7) | Bit(8)) && others.sets == Bit(20));
    XSFLIB_CHECK(UsesOf(other, 4).reads == Bit(9) && UsesOf(other, 9).reads == 0);

    // A move reaches only the commands that the start reaches.
    const MappedVariables moved = WithVariables(other, 0, {{{8, 5}, {9, 5}}, {}});

    SEQLIB_REQUIRE(moved.sequence.items.size() > 4);
    XSFLIB_CHECK(moved.sequence.items[2].time == (Argument{Source::kVariable, 5, 0}) &&
                 moved.sequence.items[4].index == 9);
}

// A move gives the variable its new number in each command, and two variables can trade numbers. A move onto a number
// that another variable keeps is refused. A move of a variable that the commands don't use changes nothing.
void TestMoves()
{
    const Sequence sequence = NewSequence();

    const MappedVariables moved = WithVariables(sequence, 0, {{{4, 9}}, {}});

    XSFLIB_CHECK(moved.start == 0 && moved.holders.empty() && moved.sequence.items.size() == sequence.items.size());
    XSFLIB_CHECK(moved.sequence.items[kFirstCompare].index == 9 && moved.sequence.items[kAdd].index == 9 &&
                 moved.sequence.items[kSecondCompare].index == 9);
    XSFLIB_CHECK(WithVariables(sequence, 0, {{{5, 9}}, {}}).sequence == sequence);

    // The volume takes variable 9's value. Variables 4 and 9 trade numbers.
    Sequence both = sequence;
    both.items.insert(both.items.begin() + 1, Command{.kind = Kind::kVolume, .value = {Source::kVariable, 9, 0}});
    for (Command& command : both.items)
    {
        command.target += ntdlib::commands::HasTarget(command.kind) ? 1 : 0;
    }

    const MappedVariables traded = WithVariables(both, 0, {{{4, 9}, {9, 4}}, {}});

    XSFLIB_CHECK(traded.sequence.items[1].value == (Argument{Source::kVariable, 4, 0}));
    XSFLIB_CHECK(traded.sequence.items[kFirstCompare + 1].index == 9 && traded.sequence.items[kAdd + 1].index == 9);
    XSFLIB_CHECK(Refused([&both] { WithVariables(both, 0, {{{4, 9}}, {}}); }));
    XSFLIB_CHECK(Refused([&sequence] { WithVariables(sequence, 0, {{{4, 32}}, {}}); }));
}

// A fixed variable takes a command at the start that sets it. The jumps move past it, and the label at the start names
// it. The variable keeps its number, unless the game's code sets that number or it's global. It then takes the lowest
// of the sound's variables that's free. A fixed variable that the commands don't read takes no command.
void TestFixed()
{
    const Sequence sequence = NewSequence();

    const MappedVariables fixed = WithVariables(sequence, 0, {{}, {{4, 1}}});
    const std::vector<Command>& items = fixed.sequence.items;

    XSFLIB_CHECK(fixed.start == 0 && fixed.holders == (std::map<int, int>{{4, 4}}));
    SEQLIB_REQUIRE(items.size() == sequence.items.size() + 1);
    XSFLIB_CHECK(items[0].kind == Kind::kSetVariable && items[0].index == 4 &&
                 items[0].value == (Argument{Source::kValue, kOne, 0}));
    XSFLIB_CHECK(items[kFirstJump + 1].target == kAdd + 1 && items[kSecondJump + 1].target == 9);
    XSFLIB_CHECK(fixed.sequence.labels == (std::vector<Label>{{"New", 0}}));

    // The game's code sets variable 4, and variable 0 holds the value.
    const MappedVariables taken = WithVariables(sequence, 0, {{}, {{4, 1}}}, 1u << 4);

    SEQLIB_REQUIRE(taken.sequence.items.size() > kAdd + 1);
    XSFLIB_CHECK(taken.holders == (std::map<int, int>{{4, 0}}) && taken.sequence.items[0].index == 0);
    XSFLIB_CHECK(taken.sequence.items[kFirstCompare + 1].index == 0 && taken.sequence.items[kAdd + 1].index == 0);

    // A global variable takes the lowest variable that neither the commands nor the game's code use.
    Sequence global = sequence;
    for (const uint32_t item : {kFirstCompare, kAdd, kSecondCompare})
    {
        global.items[item].index = 20;
    }
    global.items.insert(global.items.begin() + 1, Compare(0, 3));
    for (Command& command : global.items)
    {
        command.target += ntdlib::commands::HasTarget(command.kind) ? 1 : 0;
    }

    const MappedVariables held = WithVariables(global, 0, {{}, {{20, -5}}}, 0b10);

    XSFLIB_CHECK(held.holders == (std::map<int, int>{{20, 2}}));
    SEQLIB_REQUIRE(held.sequence.items.size() > kFirstCompare + 2);
    XSFLIB_CHECK(held.sequence.items[0].index == 2 && held.sequence.items[0].value.value == -5 * kOne);
    XSFLIB_CHECK(held.sequence.items[2].index == 0 && held.sequence.items[kFirstCompare + 2].index == 2);
    XSFLIB_CHECK(Refused([&global] { WithVariables(global, 0, {{}, {{20, 1}}}, 0xFFFE); }));

    // Variable 6 isn't read. Values past a variable's range, a variable that moves too and a variable past 31 are
    // refused.
    XSFLIB_CHECK(WithVariables(sequence, 0, {{}, {{6, 1}}}).sequence == sequence);
    XSFLIB_CHECK(Refused([&sequence] { WithVariables(sequence, 0, {{}, {{4, 32768}}}); }));
    XSFLIB_CHECK(Refused([&sequence] { WithVariables(sequence, 0, {{{4, 5}}, {{4, 1}}}); }));
    XSFLIB_CHECK(Refused([&sequence] { WithVariables(sequence, 0, {{}, {{40, 1}}}); }));
}

// The fixed value goes in after an allocation of tracks at the start. A loop back to the start passes it. A path that
// runs on into a start in the middle of the commands jumps past it.
void TestOpening()
{
    Sequence allocated;
    allocated.items = {Command{.kind = Kind::kAllocateTracks, .value = {Source::kValue, 3 * kOne, 0}}, Compare(4, 1),
                       Wait(), Jump(0)};
    allocated.labels = {{"Start", 0}};

    const MappedVariables opening = WithVariables(allocated, 0, {{}, {{4, 1}}});

    XSFLIB_CHECK(opening.start == 0);
    SEQLIB_REQUIRE(opening.sequence.items.size() == 5);
    XSFLIB_CHECK(opening.sequence.items[0].kind == Kind::kAllocateTracks &&
                 opening.sequence.items[1].kind == Kind::kSetVariable && opening.sequence.items[4].target == 2);
    XSFLIB_CHECK(opening.sequence.labels == (std::vector<Label>{{"Start", 0}}));

    Sequence middle;
    middle.items = {Command{.kind = Kind::kNote, .key = 60, .value = {Source::kValue, 24 * kOne, 0}}, Wait(),
                    Compare(4, 1), Jump(2), Command{.kind = Kind::kEnd}};
    middle.labels = {{"Intro", 0}, {"Main", 2}};

    const MappedVariables run_on = WithVariables(middle, 2, {{}, {{4, 1}}});
    const std::vector<Command>& items = run_on.sequence.items;

    XSFLIB_CHECK(run_on.start == 3);
    SEQLIB_REQUIRE(items.size() == 7);
    XSFLIB_CHECK(items[2].kind == Kind::kJump && items[2].target == 4 && items[3].kind == Kind::kSetVariable);
    XSFLIB_CHECK(items[4].kind == Kind::kCompareEqual && items[5].target == 4);
    XSFLIB_CHECK(run_on.sequence.labels == (std::vector<Label>{{"Intro", 0}, {"Main", 3}}));
}

// The replaced sound plays a moved variable as the old number, and a fixed one as a scene with its value would.
void TestPlayed()
{
    const Sequence sequence = NewSequence();

    const MappedVariables kept = WithVariables(sequence, 0, {});
    const MappedVariables moved = WithVariables(sequence, 0, {{{4, 9}}, {}});
    const MappedVariables fixed = WithVariables(sequence, 0, {{}, {{4, 1}}});
    const MappedVariables held = WithVariables(sequence, 0, {{}, {{4, 1}}}, 1u << 4);

    XSFLIB_CHECK(Keys(kept) == (std::vector<uint8_t>{60, 62, 64}));
    XSFLIB_CHECK(Keys(kept, {{4, 1}}) == (std::vector<uint8_t>{64}));
    XSFLIB_CHECK(Keys(moved, {{9, 1}}) == (std::vector<uint8_t>{64}));
    XSFLIB_CHECK(Keys(moved, {{4, 1}}) == (std::vector<uint8_t>{60, 62, 64}));
    XSFLIB_CHECK(Keys(fixed) == (std::vector<uint8_t>{64}));
    XSFLIB_CHECK(Keys(fixed, {{4, 0}}) == (std::vector<uint8_t>{64}));
    XSFLIB_CHECK(Keys(WithVariables(sequence, 0, {{}, {{4, 0}}}), {{4, 1}}) == (std::vector<uint8_t>{60, 62, 64}));
    XSFLIB_CHECK(Keys(held, {{0, 7}}) == (std::vector<uint8_t>{64}));
}

// The archive's sound reads variable 3 from its start.
void TestReaders()
{
    const File file = hokuto::format::ImportSwitchArchive(TestArchive());
    const auto readers = VariableReaders(file, PlacesOf(file));
    for (std::size_t number = 0; number < readers.size(); number++)
    {
        XSFLIB_CHECK(readers[number] == (number == 3 ? std::vector<uint32_t>{0} : std::vector<uint32_t>{}));
    }
}

} // namespace

int main()
{
    TestUses();
    TestMoves();
    TestFixed();
    TestOpening();
    TestPlayed();
    TestReaders();

    return xsflib::testing::Summary("variables");
}
