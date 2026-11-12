// SPDX-License-Identifier: MIT

// The edits of a piece of Krawall, by the rules of a module's patterns (ntdlib/krawall/commands.h). A sequence contains
// the patterns of a module, and each of the module's channels is a track. A row contains an entry for each channel that
// it has data for: a note with its instrument, a volume and an effect. A wait ends a run of rows. A note sounds until
// the next note or note off of its channel. A note has no velocity: its sample and the volume column set its volume.
//
// The edits change a note's entry in place, and every row keeps its place:
// - A removal makes the note a note off. The note before it on the channel still ends there. In a module without
//   instruments (S3M), the note off keeps the note's instrument: the channel's later notes without one play its sample
//   and volume as before. In a module with instruments (XM), a note off's instrument reads no sample. A removal is
//   refused there when a later note of the channel without an instrument plays the note's instrument. The effects that
//   would start the channel's note again go with the note: a note delay, and in a module with instruments, a
//   retrigger and an offset. They go from the note's entry, and from the channel's later entries without a note, up to
//   the channel's next note in the pattern. A note with a slide to it doesn't start, and the effects after it go too.
// - A transpose changes the note's number, from C-0 to B-7.
// A note can't move, change its length or take a velocity, and the Key Editor can't draw a new note. Each of those
// would require the row at a tick, and a row's ticks follow the module's speed as the player reaches it. The module
// header sets the channels, and a piece can't gain a track. A removal of a track makes its notes note offs.
//
// A setting moves by rows. The editors state the move in ticks of the arrangement, and the setting moves to the row
// that plays at its new tick in the stretch that the editor shows. Every other pass of its row moves it by the same
// number of rows. The arrangement's stretches give the rows (song::Track::stretches): each row that played is a
// stretch. A setting that comes to a row joins the entry of its channel there. The player sets the channel's effect
// anew at each entry. An entry has one note, one volume and one effect. A move to a row whose entry already has the
// setting's field is refused.

#pragma once

#include <cstdint>
#include <optional>
#include <span>

#include "edit/drivers.h"
#include "edit/notes.h"
#include "song/archive.h"

namespace hokuto::edit
{

// The edits of Krawall (edit/drivers.h).
const DriverEdits& KrawallEdits();

// Whether `console` follows the rules of Krawall.
bool IsKrawall(const song::Console& console);

// The rows that a move of `shift` of the arrangement's ticks moves a setting by, from its row `stretch` among `rows`
// (song::Track::stretches): to the row that plays at the new tick. Only the rows that follow on from `stretch` in its
// pattern count: a row that a jump, a break, a loop or the next order starts begins another run. Nothing where the new
// tick falls outside those rows, and where `stretch` is missing from `rows`.
std::optional<int32_t> KrawallRowsOfMove(std::span<const song::Stretch> rows, const song::Stretch& stretch,
                                         int32_t shift);

// `range`, a move of rows from row `stretch` among `rows`, in the arrangement's ticks: up to the ticks of the farthest
// rows of the run of `stretch` within the range (KrawallRowsOfMove). Nothing where `stretch` is missing from `rows`.
std::optional<ShiftRange> KrawallTicksOfRows(std::span<const song::Stretch> rows, const song::Stretch& stretch,
                                             const ShiftRange& range);

} // namespace hokuto::edit
