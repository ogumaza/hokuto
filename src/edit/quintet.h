// SPDX-License-Identifier: MIT

// The edits of a song of Quintet's GBA driver, by the rules of the driver's bytes (ntdlib/quintet/commands.h). A song
// has 6 channels, and each channel is a track: a run of commands from the channel's start to its end (FF). A note and a
// rest each give a note value, and the channel waits for it. A dot after a note or a rest adds half its length again: a
// file folds it into the item of the note or the rest (format/quintet.h). A tie (D4) joins each note of the same pitch
// after it to the note, and each rest after a rest to the rest, and the arrangement shows one note. A note sets only a
// pitch from 0 to 11: the channel's commands set its octave, its transposes and its instrument. The driver's notes have
// no velocity.
//
// The edits change notes and rests in place, and every other command keeps its tick. An edit changes the notes or
// rests that ties join along with the note or the rest:
// - A removal makes the note a rest of the same note value.
// - A new note replaces a rest that starts at its tick, with the rest's note value. Its key sets its pitch, and has to
//   fall within the octave that the commands set there.
// - A transpose changes the note's pitch within its octave.
// A note can't move or change its length or its velocity: each of those would move the channel's later commands.
//
// The song's header names its 6 channels, and keeps them. A song can't gain a track. A removal makes the track's notes
// rests, and its other commands stay.

#pragma once

#include <string_view>

#include "ntdlib/commands/sequence.h"

#include "edit/drivers.h"
#include "song/archive.h"

namespace hokuto::edit
{

// The edits of Quintet's driver (edit/drivers.h).
const DriverEdits& QuintetEdits();

// Whether `console` follows the rules of Quintet's driver.
bool IsQuintet(const song::Console& console);

// Whether `command` of Quintet's driver adds to the volume of its channel: a volume step (F1), a volume up (5F) or a
// volume down (6E).
bool StepsQuintetVolume(const ntdlib::commands::Command& command);

// Why a note can't take a pitch outside its octave. The note edits refuse such a transpose, and so does the List Editor
// before an edit where a typed key is below the octave.
inline constexpr std::string_view kQuintetKeepsOctave =
    "A note of Quintet's driver keeps its octave. Transpose it by less, or change the channel's octave in the List "
    "Editor.";

} // namespace hokuto::edit
