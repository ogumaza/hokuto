// SPDX-License-Identifier: MIT

// The edit of the instrument that a track's notes play, for the inspector's Bank and Program. A note plays the program
// that the track's last program change selected, of the bank in the sound's slot that the track's last bank select
// selected. A track starts with program 0 and slot 0. The edit gives the notes of a track that play one instrument
// another instrument. It changes the program changes and the bank selects that set their instrument. Where the track's
// start sets it, a new command at the track's start sets the new one. Only the track's opening runs the new command.
// The mixer's new commands go in the same way (mix.h).
//
// The edit refuses where a command that it would change sets the instrument of other notes too, such as a bank select
// before notes of two programs. It also refuses where the program or the slot of such notes comes from a variable, a
// random number or a comparison. Other tracks and sounds that run the same commands change with the track, such as the
// callers of a subroutine. The inspector lists them before an edit (reach.h).
//
// A sound of the Wii plays one bank, in slot 0. A track of the Wii takes another program only.

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "ntdlib/commands/sequence.h"

#include "edit/bytecode.h"
#include "edit/replace.h"
#include "format/file.h"
#include "song/sequence.h"

namespace hokuto::edit
{

// The program changes and bank selects of `commands` that set the instrument of the notes that play `instrument`, on a
// track that ran `stretches` (song::Track::stretches). `offsets` gives the offset of each item (Offsets). An edit of
// those notes' instrument changes them. The track's start sets the instrument where no command before the notes sets
// its program or its slot. Throws std::invalid_argument with a message for the user where the track's commands aren't
// where `stretches` shows them.
Setters InstrumentSetters(const ntdlib::commands::Sequence& commands, const std::vector<uint32_t>& offsets,
                          std::span<const song::Stretch> stretches, Program instrument);

// The file after the notes of track `track` of sequence `sequence` of `file` that play `from` play `to` in its place,
// and the new index of each of the sequence's items. The track's commands ran in `stretches` (song::Track::stretches).
// Throws std::invalid_argument with a message for the user for an instrument that no note of the track plays, a slot
// outside 0 to 3, a command that sets the instrument of other notes too, a program or a slot from a variable, a random
// number or a comparison, and a new command in a sequence whose commands can't move.
Edited EditInstrument(const format::File& file, uint32_t sequence, int track, std::span<const song::Stretch> stretches,
                      Program from, Program to);

// The instrument that the opening of `track`, a track of sequence `sequence` of `file`, selects: the program changes
// and the bank selects that the track runs in its first stretch before time passes. A track that selects a program and
// no slot plays slot 0, and a track that selects a slot and no program plays program 0. Nothing when the opening
// selects neither, or a value from a variable, a random number or a comparison. A new track selects its instrument this
// way (AddTrack). Throws std::invalid_argument where the track's commands aren't where its stretches show them.
std::optional<Program> OpeningInstrument(const format::File& file, uint32_t sequence, const song::Track& track);

} // namespace hokuto::edit
