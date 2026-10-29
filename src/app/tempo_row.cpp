// SPDX-License-Identifier: MIT

#include "app/tempo_row.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iterator>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/commands/units.h"

#include "app/key_edits.h"
#include "app/list_edits.h"
#include "edit/bytecode.h"
#include "edit/commands.h"
#include "edit/notes.h"

namespace hokuto::app
{
namespace
{

using ntdlib::commands::Command;
using ntdlib::commands::Fixed;
using ntdlib::commands::Kind;
using ntdlib::commands::kOne;
using ntdlib::commands::Source;

// The tempo row's room above and below the song's tempos, and the step that its ends round to, in beats a minute.
constexpr double kSpanMargin = 20.0;
constexpr double kSpanStep = 10.0;

// Far past any tempo that a console holds, and small enough for a Fixed to hold.
constexpr double kFarthestBpm = 1.0e9;

// The tempo with which the consoles' sequences start, in beats a minute (song::TempoAt).
constexpr double kStartingTempo = 120.0;

// The offsets of the commands of sequence `sequence` of `file` that set the tempo, in order. None for a file without
// the sequence.
std::vector<uint32_t> TempoOffsets(const format::File& file, uint32_t sequence)
{
    std::vector<uint32_t> found;
    try
    {
        const ntdlib::commands::Sequence& commands = edit::SequenceToEdit(file, sequence);
        const std::vector<uint32_t> offsets = edit::Offsets(commands, edit::ConsoleOf(file));
        for (std::size_t i = 0; i < commands.items.size(); i++)
        {
            if (commands.items[i].kind == Kind::kTempo)
            {
                found.push_back(offsets[i]);
            }
        }
    }
    catch (const std::exception&)
    {
        found.clear();
    }

    return found;
}

} // namespace

std::vector<TempoPointView> TempoPoints(const format::File& file, uint32_t sequence, const song::Sequence& song)
{
    // A sequence without tempo commands has no points. Only the tracks whose stretches hold one list their rows.
    const std::vector<uint32_t> tempo_offsets = TempoOffsets(file, sequence);
    std::vector<TempoPointView> points;
    std::vector<edit::NotePlace> places;
    for (const song::Track& track : song.tracks)
    {
        const auto holds_tempo = [&tempo_offsets](const song::Stretch& stretch)
        {
            const auto first = std::lower_bound(tempo_offsets.begin(), tempo_offsets.end(), stretch.first);
            return first != tempo_offsets.end() && *first <= stretch.last;
        };
        if (std::none_of(track.stretches.begin(), track.stretches.end(), holds_tempo))
        {
            continue;
        }

        const ListTrack list = MakeListTrack(file, sequence, song, track.number);
        for (const ListRow& row : list.rows)
        {
            if (!row.row.tick || row.command.kind != Kind::kTempo)
            {
                continue;
            }

            TempoPointView& point = points.emplace_back();
            point.tick = static_cast<int32_t>(std::min<uint32_t>(*row.row.tick, std::numeric_limits<int32_t>::max()));
            if (row.command.value.source == Source::kValue)
            {
                point.bpm = static_cast<double>(row.command.value.value) / static_cast<double>(kOne);
            }
            else
            {
                point.source = ArgumentText(Kind::kTempo, row.command.value);
            }
            point.track = track.number;
            point.command = row.row.command;
            point.stretch = row.row.stretch;
            point.conditional = row.command.conditional;
            point.track_name = list.name;
            point.fixed = edit::WhyFixedInTime(row.command).value_or(std::string());
            places.push_back({row.row.command, track.stretches.at(row.row.stretch)});
        }
    }

    // How far each point's command can move. A command that can't move says why. A sequence that LimitsOf can't examine
    // leaves each command without room.
    std::vector<edit::NoteLimits> limits(points.size());
    try
    {
        limits =
            edit::LimitsOf(file, sequence, places, song.loop ? std::optional<uint32_t>(song.loop->end) : std::nullopt);
    }
    catch (const std::exception&)
    {
        // `limits` gives no command room.
    }
    for (std::size_t i = 0; i < points.size(); i++)
    {
        if (const std::optional<edit::ShiftRange> room = limits[i].shift)
        {
            points[i].earlier = room->earlier;
            points[i].later = room->later;
        }
        else if (points[i].fixed.empty())
        {
            points[i].fixed = kTimedByNotes;
        }
    }

    // The tracks come in the order of their numbers, and each track's rows in the order that it ran them.
    const auto earlier = [](const TempoPointView& a, const TempoPointView& b)
    {
        return a.tick < b.tick;
    };
    std::stable_sort(points.begin(), points.end(), earlier);

    return points;
}

TempoSpan TempoSpanOf(const SongView& song)
{
    double low = song.tempo;
    double high = song.tempo;
    const auto take = [&low, &high](double bpm)
    {
        low = std::min(low, bpm);
        high = std::max(high, bpm);
    };
    for (const TempoChangeView& change : song.tempo_changes)
    {
        take(change.bpm);
    }
    for (const TempoPointView& point : song.tempo_points)
    {
        if (point.bpm)
        {
            take(*point.bpm);
        }
    }

    return {std::max(std::floor((low - kSpanMargin) / kSpanStep) * kSpanStep, 0.0),
            std::ceil((high + kSpanMargin) / kSpanStep) * kSpanStep};
}

std::vector<TempoChangeView> TempoLine(const SongView& song, const std::optional<TempoMove>& drag)
{
    // The song's tempo holds the changes at tick 0.
    std::vector<TempoChangeView> line = {{0, song.tempo}};
    for (const TempoChangeView& change : song.tempo_changes)
    {
        if (change.tick > 0)
        {
            line.push_back(change);
        }
    }

    if (!drag)
    {
        return line;
    }

    // The runs of the dragged command, each with the tempo that it sets. A tempo without one value sets the tempo that
    // the line shows at the run's tick.
    const auto before = [](int32_t tick, const TempoChangeView& change)
    {
        return tick < change.tick;
    };
    const auto change_at = [&line, &before](int32_t tick)
    {
        return std::prev(std::upper_bound(line.begin() + 1, line.end(), tick, before));
    };

    std::vector<std::pair<std::size_t, double>> runs;
    for (std::size_t i = 0; i < song.tempo_points.size(); i++)
    {
        const TempoPointView& point = song.tempo_points[i];
        if (point.command == drag->point.command)
        {
            runs.emplace_back(i, point.bpm ? drag->to : change_at(point.tick)->bpm);
        }
    }

    // The runs leave their ticks first. The command before a run in its tick then sets the tempo there. Without one,
    // the tempo before the tick holds on, and at tick 0 the starting tempo. A run that a later command overrides in the
    // same tick sets nothing there.
    for (const std::pair<std::size_t, double>& run : runs)
    {
        const std::size_t index = run.first;
        const TempoPointView& point = song.tempo_points[index];
        const auto change = change_at(point.tick);
        if (Overridden(song, index) || change->tick != point.tick)
        {
            continue;
        }

        const TempoPointView* earlier = index > 0 ? &song.tempo_points[index - 1] : nullptr;
        if (earlier != nullptr && earlier->tick == point.tick && earlier->bpm)
        {
            change->bpm = *earlier->bpm;
        }
        else if (change == line.begin())
        {
            change->bpm = kStartingTempo;
        }
        else
        {
            line.erase(change);
        }
    }

    // Each run's tempo then holds from its new tick up to the next change. A run that stays where a later command
    // overrides it changes nothing.
    for (const auto& [index, bpm] : runs)
    {
        if (drag->shift == 0 && Overridden(song, index))
        {
            continue;
        }

        const int32_t tick = std::max(song.tempo_points[index].tick + drag->shift, 0);
        const auto change = change_at(tick);
        if (change->tick == tick)
        {
            change->bpm = bpm;
        }
        else
        {
            line.insert(std::next(change), {tick, bpm});
        }
    }

    return line;
}

bool Overridden(const SongView& song, std::size_t index)
{
    const std::vector<TempoPointView>& points = song.tempo_points;
    return index + 1 < points.size() && points[index + 1].tick == points[index].tick;
}

std::optional<Command> TempoCommand(const format::File& file, uint32_t sequence, const TempoPointView& point,
                                    double bpm)
{
    if (!std::isfinite(bpm))
    {
        return std::nullopt;
    }

    try
    {
        Command command = edit::CommandAt(file, sequence, point.command);
        if (command.kind != Kind::kTempo || command.value.source != Source::kValue)
        {
            return std::nullopt;
        }

        // The tempo stays within what the console's player plays (edit::ArgumentRange), and at 1 at least. A tempo of 0
        // stops the sequence.
        const song::Console console = edit::ConsoleOf(file);
        const double limited = std::clamp(bpm, 1.0, kFarthestBpm);
        auto value = static_cast<Fixed>(std::llround(limited * static_cast<double>(kOne)));
        if (const std::optional<edit::ValueRange> range = edit::ArgumentRange(console, command, Source::kValue))
        {
            value = std::clamp(value, range->lowest, range->highest);
        }
        command.value.value = value;

        return edit::Nearest(console, command);
    }
    catch (const std::invalid_argument&)
    {
        return std::nullopt;
    }
}

std::vector<bool> FittingLabels(std::span<const float> xs, std::span<const float> widths, float gap)
{
    std::vector<float> sorted(xs.begin(), xs.end());
    std::ranges::sort(sorted);
    std::vector<bool> fits(xs.size(), false);
    for (std::size_t i = 0; i < xs.size() && i < widths.size(); i++)
    {
        // The points from this point's place up to the label's end: this point, and any other.
        const auto from = std::ranges::lower_bound(sorted, xs[i]);
        const auto to = std::ranges::upper_bound(sorted, xs[i] + gap + widths[i]);
        fits[i] = to - from == 1;
    }

    return fits;
}

} // namespace hokuto::app
