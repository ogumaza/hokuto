// SPDX-License-Identifier: MIT

#include "app/markers.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <map>
#include <numeric>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/commands/sequence.h"

#include "app/list_edits.h"
#include "app/time_text.h"
#include "app/transport.h"
#include "edit/bytecode.h"

namespace hokuto::app
{
namespace
{

using ntdlib::commands::Command;
using ntdlib::commands::Kind;
using ntdlib::commands::Source;

// Whether a command of `kind` works out its variable's new value from the variable's value.
bool UpdatesVariable(Kind kind)
{
    switch (kind)
    {
    case Kind::kAddVariable:
    case Kind::kSubtractVariable:
    case Kind::kMultiplyVariable:
    case Kind::kDivideVariable:
    case Kind::kShiftVariable:
    case Kind::kAndVariable:
    case Kind::kOrVariable:
    case Kind::kXorVariable:
    case Kind::kModuloVariable:
        return true;
    default:
        return false;
    }
}

// Whether a command of `kind` compares its variable with its value.
bool Compares(Kind kind)
{
    return kind >= Kind::kCompareEqual && kind <= Kind::kCompareNotEqual;
}

// The line that describes `row` of `track`: the track's name, then the command as the List Editor's columns show it. A
// note's or a wait's length comes last. So does a time that comes from a variable.
std::string LineOf(const ListTrack& track, const ListRow& row)
{
    const Command& command = row.command;
    std::vector<std::string> values;
    for (const ListColumn column : {ListColumn::kData1, ListColumn::kData2})
    {
        std::string value = CellText(track, row, column);
        if (!value.empty())
        {
            values.push_back(std::move(value));
        }
    }
    const bool timed = command.time && command.time->source == Source::kVariable;
    if (command.kind == Kind::kNote || command.kind == Kind::kWait || timed)
    {
        values.push_back(WithoutFigureSpaces(CellText(track, row, ListColumn::kLength)));
    }

    std::string line = track.name + ": " + (command.conditional ? "if " : "") + CellText(track, row, ListColumn::kType);
    for (std::size_t i = 0; i < values.size(); i++)
    {
        line += (i == 0 ? " " : ", ") + values[i];
    }

    return line;
}

// Whether `command` branches or reads a variable.
bool IsLogic(const Command& command)
{
    return command.conditional || !VariablesRead(command).empty();
}

// Whether sequence `sequence` of `file` has a command that branches or reads a variable. A sequence that the List
// Editor can't list has none.
bool HasLogicCommands(const format::File& file, uint32_t sequence)
{
    try
    {
        const std::vector<Command>& items = edit::SequenceToEdit(file, sequence).items;
        if (std::none_of(items.begin(), items.end(), IsLogic))
        {
            return false;
        }
    }
    catch (const std::exception&)
    {
        return false;
    }

    return true;
}

// A row of `track` that branches or reads a variable, at `tick`.
struct Run
{
    uint32_t tick;
    const ListTrack* track;
    const ListRow* row;
};

// The rows of `tracks` that branch or read variables, in the order of their ticks. At a tick, the tracks come in the
// order of their numbers, and each track's rows in the order that it ran them.
std::vector<Run> LogicRuns(const std::vector<ListTrack>& tracks)
{
    std::vector<Run> runs;
    for (const ListTrack& track : tracks)
    {
        for (const ListRow& row : track.rows)
        {
            if (row.row.tick && IsLogic(row.command))
            {
                runs.push_back({*row.row.tick, &track, &row});
            }
        }
    }
    const auto earlier = [](const Run& a, const Run& b)
    {
        return a.tick < b.tick;
    };
    std::stable_sort(runs.begin(), runs.end(), earlier);

    return runs;
}

// Fills `marker` with the commands of `runs` from `first` to `last`, their lines and the variables that they read.
// `lines` keeps the line of each command once it's worked out.
void FillLogicMarker(LogicMarkerView& marker, const std::vector<Run>& runs, std::size_t first, std::size_t last,
                     std::map<std::pair<int, uint32_t>, std::string>& lines)
{
    for (std::size_t i = first; i < last; i++)
    {
        const Run& run = runs[i];
        const std::pair<int, uint32_t> place(run.track->number, run.row->row.command);
        const auto [line, new_line] = lines.try_emplace(place);
        if (new_line)
        {
            line->second = LineOf(*run.track, *run.row);
        }
        const std::vector<int> read = VariablesRead(run.row->command);
        marker.branch = marker.branch || run.row->command.conditional;
        marker.variables.insert(marker.variables.end(), read.begin(), read.end());
        marker.commands.push_back({place.first, place.second, line->second});
    }

    // Each set names each variable once, in order.
    std::sort(marker.variables.begin(), marker.variables.end());
    marker.variables.erase(std::unique(marker.variables.begin(), marker.variables.end()), marker.variables.end());
}

} // namespace

std::vector<int> VariablesRead(const Command& command)
{
    std::set<int> read;
    if (command.value.source == Source::kVariable)
    {
        read.insert(static_cast<int>(command.value.value));
    }
    if (command.time && command.time->source == Source::kVariable)
    {
        read.insert(static_cast<int>(command.time->value));
    }
    if (Compares(command.kind) || UpdatesVariable(command.kind))
    {
        read.insert(command.index);
    }

    return {read.begin(), read.end()};
}

std::vector<LogicMarkerView> LogicMarkers(const format::File& file, uint32_t sequence, const song::Sequence& song)
{
    // A sequence without such commands has no places. The List Editor's rows then aren't worked out.
    if (!HasLogicCommands(file, sequence))
    {
        return {};
    }

    // The rows that read variables or branch, in the order of their ticks. At a tick, the tracks come in the order of
    // their numbers, and each track's rows in the order that it ran them.
    const std::vector<ListTrack> tracks = MakeListTracks(file, sequence, song);
    const std::vector<Run> runs = LogicRuns(tracks);

    // The places that run the same commands share a set. Each command's line is worked out once.
    using Signature = std::vector<std::pair<int, uint32_t>>;
    std::map<Signature, std::size_t> sets;
    std::map<std::pair<int, uint32_t>, std::string> lines;
    std::vector<LogicMarkerView> markers;
    Signature signature;
    for (std::size_t first = 0, last = 0; first < runs.size(); first = last)
    {
        signature.clear();
        for (last = first; last < runs.size() && runs[last].tick == runs[first].tick; last++)
        {
            signature.emplace_back(runs[last].track->number, runs[last].row->row.command);
        }

        const auto [set, added] = sets.try_emplace(signature, markers.size());
        if (added)
        {
            LogicMarkerView& marker = markers.emplace_back();
            FillLogicMarker(marker, runs, first, last, lines);
        }
        markers[set->second].ticks.push_back(static_cast<int32_t>(runs[first].tick));
    }

    return markers;
}

std::string VariablesLabel(std::span<const int> variables)
{
    std::string label;
    for (std::size_t i = 0; i < variables.size(); i++)
    {
        label += (i == 0 ? "Var " : ", ") + std::to_string(variables[i]);
    }

    return label;
}

std::vector<float> FlagWidths(std::span<const FlagSpan> flags, float gap, float least)
{
    // The flags from left to right. Of the flags at one place, the later in `flags` comes later.
    std::vector<std::size_t> order(flags.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::ranges::stable_sort(order, {}, [flags](std::size_t i) { return flags[i].x; });

    std::vector<float> widths(flags.size());
    for (std::size_t k = 0; k < order.size(); k++)
    {
        const FlagSpan& flag = flags[order[k]];
        const float room =
            k + 1 < order.size() ? flags[order[k + 1]].x - flag.x - gap : std::numeric_limits<float>::infinity();
        widths[order[k]] = std::clamp(flag.wanted, least, std::max(room, least));
    }

    return widths;
}

} // namespace hokuto::app
