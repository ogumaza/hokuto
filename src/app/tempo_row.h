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
#include <vector>

#include "ntdlib/commands/sequence.h"

#include "app/song_view.h"
#include "format/file.h"
#include "song/sequence.h"

namespace hokuto::app
{

// The commands of the tracks of `song` that set the tempo, in the order of their ticks. At a tick, the tracks come in
// the order of their numbers, and each track's commands in the order that it ran them. `song` is sequence `sequence` of
// `file` as ntdlib's performer played it. The commands are those that the List Editor lists for each track
// (list_edits.h), up to the end of the loop's first pass. A command whose tick isn't known stays out
// (edit::CommandRow). So does each track whose commands the List Editor can't show. Each point gives how far its
// command can move (edit::LimitsOf), or why it can't.
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
// starts. A moved tempo without one value keeps the tempo that the line showed at its old tick.
std::vector<TempoChangeView> TempoLine(const SongView& song, const std::optional<TempoMove>& drag);

// Whether a later command than point `index` of `song` sets the tempo again in the same tick. The later command then
// sets the tempo there.
bool Overridden(const SongView& song, std::size_t index);

// The command at `point` of sequence `sequence` of `file` with the tempo `bpm`, at the nearest tempo that the console's
// player plays (edit::ArgumentRange), and at 1 at least. Nothing for a tempo that isn't a number, a command that isn't
// a tempo from a value, and a command that isn't there.
std::optional<ntdlib::commands::Command> TempoCommand(const format::File& file, uint32_t sequence,
                                                      const TempoPointView& point, double bpm);

// Which labels beside the points at `xs` fit. The label of point i is `widths[i]` wide and starts `gap` after the
// point. It fits where no other point lies from the point to the label's end. The tempo row and the Key Editor's
// controller lanes draw a value's source beside its bar only where it fits. The labels of points a pixel or two apart
// then don't print over one another, and the tooltip gives the source.
std::vector<bool> FittingLabels(std::span<const float> xs, std::span<const float> widths, float gap);

} // namespace hokuto::app
