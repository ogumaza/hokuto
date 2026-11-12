// SPDX-License-Identifier: MIT

// What the arrangement's tempo row shows of a song, worked out without a window: the commands that set the tempo, the
// tempos that the row spans, and the command that a drag of a point gives. A point is a command that sets the tempo, at
// the tick at which a track ran it. The line follows the tempo as ntdlib's performer played it. The tests run without a
// window.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/commands/units.h"

#include "app/song_view.h"
#include "edit/commands.h"
#include "format/file.h"
#include "song/archive.h"
#include "song/sequence.h"

namespace hokuto::app
{

// The commands of the tracks of `song` that set the tempo, in the order of their ticks. At a tick, the tracks come in
// the order of their numbers, and each track's commands in the order that it ran them. `song` is sequence `sequence` of
// `file` as ntdlib's performer played it. The commands are those that the List Editor lists for each track
// (list_edits.h), up to the end of the loop's first pass. A command whose tick isn't known stays out
// (edit::CommandRow). So does each track whose commands the List Editor can't show, and each track whose tempo commands
// set only its tempo (song::DriverRules::SetsSongTempo). Each point gives how far its command can move
// (edit::LimitsOf), or why it can't.
std::vector<TempoPointView> TempoPoints(const format::File& file, uint32_t sequence, const song::Sequence& song);

// The tempos that the tempo row spans, in beats a minute.
struct TempoSpan
{
    friend bool operator==(const TempoSpan&, const TempoSpan&) = default;

    double low = 0.0;
    double high = 0.0;
};

// The span of the tempo row of `song`: from 20 below its lowest tempo to 20 above its highest, out to multiples of 10,
// and from 0 at least. The tempo at the song's start counts, and so do the changes of the tempo and the values of the
// points.
TempoSpan TempoSpanOf(const SongView& song);

// The tempo row's line of `song`: the tempo at the song's start, then each later change of the tempo, in the order of
// their ticks. During `drag`, the line shows the drag's tempo at each run of the dragged point's command, from the
// run's tick up to the next change. A run that a later command overrides in the same tick changes nothing. A drag that
// moves the point moves each run by its shift, overridden or not. The command before the run in its old tick then sets
// the tempo there. Without one, the tempo before the tick holds on, and at tick 0 the tempo with which the sequence
// starts (`starting_tempo`). A moved tempo without one value keeps the tempo that the line showed at its old tick.
std::vector<TempoChangeView> TempoLine(const SongView& song, const std::optional<TempoMove>& drag);

// Whether a later command than point `index` of `song` sets the tempo again in the same tick. The later command then
// sets the tempo there.
bool Overridden(const SongView& song, std::size_t index);

// A move of a tempo point in time: the ticks that it moves by, later for more than 0, and why it stops short of the
// tick that a drag asks for, for the user. `refused` is empty where it doesn't.
struct TempoShift
{
    friend bool operator==(const TempoShift&, const TempoShift&) = default;

    int32_t ticks = 0;
    std::string refused;
};

// The move of `point` that a drag of `wanted` ticks asks for. The move stops at the point's room
// (TempoPointView::earlier and later), and `refused` then says where (PointLimitText). `ticks_per_quarter` is the
// ticks of a quarter note.
TempoShift ShiftOfTempoPoint(const TempoPointView& point, int64_t wanted, int ticks_per_quarter);

// The beats a minute that a tempo command of `value` sets on `console`, and the value that sets `bpm`. A tempo of a GBA
// driver counts with 60 frames a second (song::DriverRules::TempoBpm). On Quintet's driver, the beats a minute are
// those that play at the GBA's frame rate. The line shows the same values.
double TempoBpmOf(const song::Console& console, ntdlib::commands::Fixed value);
ntdlib::commands::Fixed TempoValueOf(const song::Console& console, double bpm);

// The command at `point` of sequence `sequence` of `file` with the tempo `bpm`, at the nearest tempo that the console's
// player plays (edit::ArgumentRange), and at 1 at least. Nothing for a tempo that isn't a number, a command that isn't
// a tempo from a value, and a command that isn't there.
std::optional<ntdlib::commands::Command> TempoCommand(const format::File& file, uint32_t sequence,
                                                      const TempoPointView& point, double bpm);

// The commands that a new tempo at `point` changes, and their new values. `command` is the point's command with the new
// tempo (TempoCommand). The point's command comes first. On a driver whose tempo commands of some tracks set only the
// tempo of their track (song::DriverRules::SetsSongTempo), the tempo commands of those tracks that run at the tick of
// the point with the old value of the point get the new tempo too. On Quintet's driver, the tempo of the first channel
// sets the tempo of every channel, and the other channels often set the same tempo again for themselves. They would
// keep the old tempo, and the song would fall out of step. `song` is sequence `sequence` of `file` as ntdlib's
// performer played it. Throws std::invalid_argument for a point whose track or stretch is missing from `song`.
struct TempoChange
{
    std::vector<edit::CommandPlace> places;
    std::vector<ntdlib::commands::Command> commands;
};
TempoChange TempoChangeOf(const format::File& file, uint32_t sequence, const song::Sequence& song,
                          const TempoPointView& point, const ntdlib::commands::Command& command);

// Which labels beside the points at `xs` fit. The label of point i is `widths[i]` wide and starts `gap` after the
// point. It fits where no other point lies from the point to the label's end. The tempo row and the Key Editor's
// controller lanes draw a value's source beside its bar only where it fits. The labels of points a pixel or two apart
// then don't print over one another, and the tooltip gives the source.
std::vector<bool> FittingLabels(std::span<const float> xs, std::span<const float> widths, float gap);

} // namespace hokuto::app
