// SPDX-License-Identifier: MIT

// Tests the bank slots of a sequence of a later console on the Wii (src/edit/wii_programs.h). Made-up sequences select
// banks and programs. The slots that their notes play take ranges of programs one after another. A program change of
// one slot takes its range, and a bank select becomes a program change into its slot's range where it runs with one
// program. A random program takes its range's ends. A program from a variable keeps its number, and so does a program
// change in a subroutine that tracks of two slots call. A bank select from a variable, and a bank select that runs with
// two programs, stay. The notes that may then play another instrument on the Wii count. A note behind a program past
// its bank's programs widens the slot's range. A sequence whose notes play only the first slot stays as it is. The
// analysis also gives the programs that each slot's notes play: each value of a random program, and nothing for a slot
// whose notes can take a program from a variable. It gives nothing when it would hold more states of the tracks than
// its limit allows.

#include "edit/wii_programs.h"

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <utility>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/commands/units.h"
#include "xsflib/testing/check.h"

namespace
{

using hokuto::edit::SlotPrograms;
using hokuto::edit::WiiPrograms;
using hokuto::edit::WiiProgramsOf;
using ntdlib::commands::Command;
using ntdlib::commands::Kind;
using ntdlib::commands::kOne;
using ntdlib::commands::Sequence;
using ntdlib::commands::Source;

// A command of `kind` with `value`.
Command Setting(Kind kind, int value)
{
    Command command;
    command.kind = kind;
    command.value = {.source = Source::kValue, .value = value * kOne, .high = 0};

    return command;
}

// A program change to `program`.
Command Program(int program)
{
    return Setting(Kind::kProgram, program);
}

// A bank select of `slot`.
Command BankSelect(int slot)
{
    return Setting(Kind::kBankSelect, slot);
}

// A command of `kind` that takes its value from `variable`.
Command FromVariable(Kind kind, int variable)
{
    Command command;
    command.kind = kind;
    command.value = {.source = Source::kVariable, .value = variable, .high = 0};

    return command;
}

// A command of `kind` with a random value from `low` to `high`.
Command Random(Kind kind, int low, int high)
{
    Command command;
    command.kind = kind;
    command.value = {.source = Source::kRandom, .value = low * kOne, .high = high * kOne};

    return command;
}

// A note of `key` for 24 ticks, at velocity 100.
Command Note(uint8_t key)
{
    Command command = Setting(Kind::kNote, 24);
    command.key = key;
    command.velocity = 100 * kOne;

    return command;
}

// A command of `kind` that goes to item `target`, with `index`, such as a track's number.
Command WithTarget(Kind kind, uint32_t target, uint8_t index = 0)
{
    Command command;
    command.kind = kind;
    command.target = target;
    command.index = index;

    return command;
}

// A command of `kind` without a value.
Command Plain(Kind kind)
{
    Command command;
    command.kind = kind;

    return command;
}

// `command`, run only if the last comparison held.
Command Conditional(Command command)
{
    command.conditional = true;

    return command;
}

// A sequence of `items`, without labels.
Sequence Of(std::vector<Command> items)
{
    return {.items = std::move(items), .labels = {}};
}

// Two banks of two programs each.
const std::vector<uint32_t> kTwoBanks = {2, 2};

// The programs that the notes of each slot play.
using Played = std::map<int, std::optional<std::set<uint32_t>>>;

// The program of `command`, a program change with a number.
int64_t ProgramOf(const Command& command)
{
    return command.kind == Kind::kProgram && command.value.source == Source::kValue ? command.value.value / kOne : -1;
}

// A track selects slot 1 and slot 0 again. The first slot's programs stay, and slot 1's come after them.
void TestSlots()
{
    const Sequence sequence = Of({Program(1), Note(60), BankSelect(1), Note(62), Program(0), Note(64), BankSelect(0),
                                  Note(65), Plain(Kind::kEnd)});

    const std::optional<WiiPrograms> programs = WiiProgramsOf(sequence, 0, kTwoBanks);

    SEQLIB_REQUIRE(programs.has_value());
    XSFLIB_CHECK((programs->slots == std::map<int, SlotPrograms>{{0, {0, 2}}, {1, {2, 2}}}));
    const std::vector<Command>& items = programs->sequence.items;
    SEQLIB_REQUIRE(items.size() > 6);
    XSFLIB_CHECK(ProgramOf(items[0]) == 1 && ProgramOf(items[2]) == 3 && ProgramOf(items[4]) == 2 &&
                 ProgramOf(items[6]) == 0);
    XSFLIB_CHECK(programs->programs == 1 && programs->bank_selects == 2);
    XSFLIB_CHECK(programs->unsure_notes == 0 && !programs->unknown_slots);
    XSFLIB_CHECK((programs->played == Played{{0, std::set<uint32_t>{0, 1}}, {1, std::set<uint32_t>{0, 1}}}));

    // A bank select behind an if prefix becomes a program change behind it.
    const Sequence behind_if = Of({Conditional(BankSelect(1)), Note(60), Plain(Kind::kEnd)});

    const std::optional<WiiPrograms> conditional = WiiProgramsOf(behind_if, 0, kTwoBanks);

    XSFLIB_CHECK(conditional && conditional->sequence.items[0].conditional &&
                 ProgramOf(conditional->sequence.items[0]) == 2 && conditional->unsure_notes == 0);
}

// Notes that play only the first slot leave the sequence as it is. The sound's tracks start at its start.
void TestFirstSlot()
{
    const Sequence late_select = Of({Program(1), Note(60), BankSelect(1), Plain(Kind::kEnd)});

    const std::optional<WiiPrograms> programs = WiiProgramsOf(late_select, 0, kTwoBanks);

    SEQLIB_REQUIRE(programs.has_value());
    XSFLIB_CHECK(programs->sequence == late_select);
    XSFLIB_CHECK((programs->slots == std::map<int, SlotPrograms>{{0, {0, 2}}}));
    XSFLIB_CHECK(programs->bank_selects == 0 && programs->unsure_notes == 0);

    const Sequence other_sound =
        Of({BankSelect(1), Note(60), Plain(Kind::kEnd), Program(1), Note(62), Plain(Kind::kEnd)});

    const std::optional<WiiPrograms> started = WiiProgramsOf(other_sound, 3, kTwoBanks);

    XSFLIB_CHECK(started && started->sequence == other_sound && started->slots.size() == 1);
}

// A random program of slot 1 takes the range's ends, and its notes play each of its values. A program from a variable
// keeps its number, and its note may play another instrument. The analysis gives no list of the programs that the notes
// of that slot play. A program past its bank's programs widens the range. A slot without a bank takes a range of the
// programs that its notes select.
void TestValues()
{
    const Sequence random = Of({BankSelect(1), Random(Kind::kProgram, 0, 3), Note(60), Plain(Kind::kEnd)});

    const std::optional<WiiPrograms> drawn = WiiProgramsOf(random, 0, kTwoBanks);

    SEQLIB_REQUIRE(drawn.has_value());
    XSFLIB_CHECK((drawn->slots == std::map<int, SlotPrograms>{{0, {0, 2}}, {1, {2, 4}}}));
    SEQLIB_REQUIRE(drawn->sequence.items.size() > 1);
    const Command& range = drawn->sequence.items[1];
    XSFLIB_CHECK(range.value.source == Source::kRandom && range.value.value == 2 * kOne &&
                 range.value.high == 5 * kOne);
    XSFLIB_CHECK(ProgramOf(drawn->sequence.items[0]) == 2 && drawn->unsure_notes == 0);
    XSFLIB_CHECK((drawn->played == Played{{1, std::set<uint32_t>{0, 1, 2, 3}}}));

    const Sequence variable = Of({BankSelect(1), FromVariable(Kind::kProgram, 5), Note(60), Plain(Kind::kEnd)});

    const std::optional<WiiPrograms> read = WiiProgramsOf(variable, 0, kTwoBanks);

    SEQLIB_REQUIRE(read.has_value());
    SEQLIB_REQUIRE(read->sequence.items.size() > 1);
    XSFLIB_CHECK(read->sequence.items[1] == variable.items[1] && read->unsure_notes == 1);
    XSFLIB_CHECK((read->played == Played{{1, std::nullopt}}));

    const Sequence past = Of({BankSelect(1), Program(5), Note(60), Plain(Kind::kEnd)});

    const std::optional<WiiPrograms> wide = WiiProgramsOf(past, 0, kTwoBanks);

    SEQLIB_REQUIRE(wide.has_value());
    XSFLIB_CHECK((wide->slots == std::map<int, SlotPrograms>{{0, {0, 2}}, {1, {2, 6}}}));
    SEQLIB_REQUIRE(wide->sequence.items.size() > 1);
    XSFLIB_CHECK(ProgramOf(wide->sequence.items[1]) == 7 && wide->unsure_notes == 0);

    const Sequence no_bank = Of({BankSelect(2), Note(60), Plain(Kind::kEnd)});

    const std::optional<WiiPrograms> empty = WiiProgramsOf(no_bank, 0, kTwoBanks);

    SEQLIB_REQUIRE(empty.has_value());
    XSFLIB_CHECK((empty->slots == std::map<int, SlotPrograms>{{0, {0, 2}}, {2, {2, 1}}}));
    SEQLIB_REQUIRE(!empty->sequence.items.empty());
    XSFLIB_CHECK(ProgramOf(empty->sequence.items[0]) == 2);
}

// A bank select from a variable stays, and its notes may play another slot. So may the notes of a subroutine whose
// program change two slots reach. A bank select that a loop reaches with another program stays, and the program change
// after it puts the note right. The programs of the notes whose slot a variable selects count apart from the slots'.
void TestFlow()
{
    const Sequence variable = Of({FromVariable(Kind::kBankSelect, 3), Note(60), Plain(Kind::kEnd)});

    const std::optional<WiiPrograms> unknown = WiiProgramsOf(variable, 0, kTwoBanks);

    SEQLIB_REQUIRE(unknown.has_value());
    XSFLIB_CHECK(unknown->unknown_slots && unknown->unsure_notes == 1 && unknown->sequence == variable);
    XSFLIB_CHECK((unknown->played == Played{{-1, std::set<uint32_t>{0}}}));

    // Track 0 selects slot 1 and calls the subroutine at item 6. Track 1 calls it in slot 0.
    const Sequence shared =
        Of({WithTarget(Kind::kOpenTrack, 4, 1), BankSelect(1), WithTarget(Kind::kCall, 6), Plain(Kind::kEnd),
            WithTarget(Kind::kCall, 6), Plain(Kind::kEnd), Program(1), Note(60), Plain(Kind::kReturn)});

    const std::optional<WiiPrograms> called = WiiProgramsOf(shared, 0, kTwoBanks);

    SEQLIB_REQUIRE(called.has_value());
    XSFLIB_CHECK((called->slots == std::map<int, SlotPrograms>{{0, {0, 2}}, {1, {2, 2}}}));
    SEQLIB_REQUIRE(called->sequence.items.size() > 6);
    XSFLIB_CHECK(ProgramOf(called->sequence.items[6]) == 1 && called->unsure_notes == 1);

    // The jump back reaches the bank select with program 1.
    const Sequence looped =
        Of({BankSelect(1), Program(1), Note(60), Setting(Kind::kWait, 24), WithTarget(Kind::kJump, 0)});

    const std::optional<WiiPrograms> again = WiiProgramsOf(looped, 0, kTwoBanks);

    SEQLIB_REQUIRE(again.has_value());
    SEQLIB_REQUIRE(again->sequence.items.size() > 1);
    XSFLIB_CHECK(again->sequence.items[0].kind == Kind::kBankSelect && ProgramOf(again->sequence.items[1]) == 3);
    XSFLIB_CHECK(again->bank_selects == 0 && again->programs == 1 && again->unsure_notes == 0);

    XSFLIB_CHECK(!WiiProgramsOf(shared, 0, kTwoBanks, 1));
}

} // namespace

int main()
{
    TestSlots();
    TestFirstSlot();
    TestValues();
    TestFlow();

    return xsflib::testing::Summary("wii_programs");
}
