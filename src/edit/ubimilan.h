// SPDX-License-Identifier: MIT

// The edits of a piece of Ubisoft Milan's GBA driver, by the rules of the driver's bytes (ntdlib/ubimilan/commands.h).
// A sequence has one track of commands for 16 channels, and each command names a channel. A file stores each wait as a
// length in ticks (format/ubimilan.h). Every command has a wait after it, and no command runs one tick after another.
//
// A note sounds until a note-off of the same key on the same channel. A note of the kit sounds until any note-off of
// channel 9. A later note on a PSG channel ends the note before it. So does a later note of the kit in a piece that
// keeps one voice for the kit. The edits place the commands of the sequence at their ticks:
// - A move shifts the note and its note-off to the new ticks. A new length moves the note-off. A note without a
//   note-off of its own supports neither edit.
// - A removal deletes the note and its note-off.
// - A new note is added with a note-off at the end of its length, on the channel of the track (NewNote::index).
// - A transpose changes the key of the note, and of its note-off on a PSG channel and the noise. On channel 9, keys up
//   to 11 play the noise and higher keys the kit. A transpose can't move a note between the two.
// Within a tick, a moved note-off comes first and a moved note last. The end of the sequence keeps its tick, and no
// command can move past it. An edit that would put commands one tick apart is refused.
//
// A track is a channel. Channels 0 to 2 play the square 1, square 2 and wave channels of the PSG, and channel 9 plays
// the kit and the noise. A new track uses the first free channel of the four, with a program change at the first
// command of the sequence. A copy and a pasted track use the channel of the track where that channel is free. A square
// uses the other square channel otherwise. The commands are added at the same ticks. A removal deletes every command of
// the channel.

#pragma once

#include "edit/drivers.h"
#include "song/archive.h"

namespace hokuto::edit
{

// The edits of Ubisoft Milan's driver (edit/drivers.h).
const DriverEdits& UbiMilanEdits();

// Whether `console` follows the rules of Ubisoft Milan's driver.
bool IsUbiMilan(const song::Console& console);

} // namespace hokuto::edit
