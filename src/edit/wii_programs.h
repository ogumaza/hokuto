// SPDX-License-Identifier: MIT

// The bank slots of a sequence of the 3DS, the Wii U or the Switch on the Wii. A sound of the Wii plays one bank, and
// NW4R SND has no bank select. A sound that goes to the Wii takes the banks of the slots that its notes play as one
// bank (ForWii in edit/other_console.h). The slots lie one after another in that bank's programs: the first slot's from
// program 0, and each other slot's after the slot before it. A slot's range holds its bank's programs, and as many more
// as its notes select. The program changes then select the programs of their slots' ranges, and each bank select
// becomes a program change into its slot's range. A track starts with slot 0 and program 0 on every console.
//
// A program change takes the range of the one slot that it can run with. A random program takes the range's ends. A
// program from a variable keeps its number, and so does a program change that can run with more than one slot or with a
// slot from a variable or a random number. A bank select becomes a program change where it can run with one program
// alone, a number. Each other bank select stays, and the Wii's player skips it. A flow analysis of the sound's tracks
// finds the slots and programs (song/track_flow.h).

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <span>

#include "ntdlib/commands/sequence.h"

namespace hokuto::edit
{

// The programs of a bank slot in the Wii's bank: `count` programs from `first`.
struct SlotPrograms
{
    friend bool operator==(const SlotPrograms&, const SlotPrograms&) = default;

    uint32_t first = 0;
    uint32_t count = 0;
};

struct WiiPrograms
{
    // The sequence with its program changes into the slots' ranges.
    ntdlib::commands::Sequence sequence;

    // The range of each slot that the sound's notes play, by slot. Slot 0 always has one. The sequence stays as it is
    // when no other slot has one.
    std::map<int, SlotPrograms> slots;

    // The program changes that took the range of another slot than the first, and the bank selects that became program
    // changes.
    std::size_t programs = 0;
    std::size_t bank_selects = 0;

    // The note commands that may play another instrument on the Wii: a variable or a random number gives their slot or
    // their program there, or tracks of more than one slot reach them.
    std::size_t unsure_notes = 0;

    // Whether a note can play a slot that a variable or a random number selects.
    bool unknown_slots = false;

    // The programs that the notes of each slot play on the console that the sequence comes from, by slot. A random
    // program counts each of its values. A slot whose notes can take a program from a variable holds nothing. Slot -1
    // holds the programs of the notes whose slot a variable or a random number selects.
    std::map<int, std::optional<std::set<uint32_t>>> played;
};

// `sequence`, a sequence of the 3DS, the Wii U or the Switch whose sound starts at item `start`, converted for the Wii
// to play with one bank. Entry s of `programs` gives the number of programs of the bank in slot s, or 0 for a slot
// without a bank. A slot past `programs` holds no bank. The sound's tracks start at `start` and where its commands open
// tracks. Nothing when the analysis would hold more than `most` states of the tracks.
std::optional<WiiPrograms> WiiProgramsOf(const ntdlib::commands::Sequence& sequence, uint32_t start,
                                         std::span<const uint32_t> programs, std::size_t most = 4'000'000);

} // namespace hokuto::edit
