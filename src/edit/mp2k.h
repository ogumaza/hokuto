// SPDX-License-Identifier: MIT

// The edits of a GBA song, by the rules of MusicPlayer2000's bytes (ntdlib/mp2k/commands.h). A wait has one of the
// lengths of the codes: 0 to 24 ticks, and 28 to 96 ticks in steps of 2 to 4. A note lasts the length of a code plus up
// to 127 ticks, 223 ticks at most. A note of length 0 is a tie: it sounds until an end of the tie with its key. A
// command can leave out what the state of the track supplies: the running status, and the last key and velocity.
//
// After an edit, each wait whose length no code has splits into waits of the lengths of the codes, the longest first.
// Each note gets the code of the longest length up to its length. Each command that leaves something out, and no longer
// follows commands that leave the track in the state that it relies on, then gets its bytes in full. The rest keep
// their bytes.
//
// Metroid Fusion's sequencer reads a note's key and velocity, and no third byte: a byte below 0x80 after them starts
// the next command (format::NativeArchive::gba_note_arguments). A note there lasts only the length of its code: 0 to 24
// ticks, or 28 to 96 ticks in steps of 2 to 4.

#pragma once

#include <cstdint>

#include "edit/bytecode.h"

namespace hokuto::edit
{

// The longest note that MusicPlayer2000's bytes hold without a tie, in ticks.
inline constexpr uint32_t kLongestMp2kNote = 223;

// The longest length of a note code, in ticks: the longest note without a tie where a note reads two bytes.
inline constexpr uint32_t kLongestMp2kCode = 96;

// The most patterns that the stack of a track can contain. A pattern can call another, three deep, and a call past that
// ends the track (ntdlib's MP2K sequencer). A repeat doesn't use the stack.
inline constexpr int kMp2kPatternDepth = 3;

// `edited` as MusicPlayer2000's bytes can hold it. `edited` is an edit of sequence `sequence` of a GBA file.
// `Edited::items` and `Edited::added` follow the new items, and so do the targets, the labels and the fields of the
// tracks' starts. Throws std::invalid_argument with a message for the user for a note longer than kLongestMp2kNote, and
// for a note whose length no code has where a note reads two bytes.
Edited Mp2kWritable(Edited edited, uint32_t sequence);

// `edited` as the console's bytes can hold it: on the GBA, by MusicPlayer2000's rules (Mp2kWritable), on the GBA's
// other drivers by their rules (DriverEdits::Writable), and otherwise as it is. `edited` is an edit of sequence
// `sequence` of `before`.
Edited Writable(const format::File& before, Edited edited, uint32_t sequence);

} // namespace hokuto::edit
