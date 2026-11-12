// SPDX-License-Identifier: MIT

// The List Editor's structural edits: a new target or condition, an inserted or removed command, the edits of
// labels, and the edits of subroutines and their calls.

#include "app/list_edits.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/commands/units.h"
#include "ntdlib/sequence_export/performance.h"

#include "app/list_edits_parts.h"
#include "edit/bytecode.h"
#include "edit/commands.h"
#include "song/driver.h"

namespace hokuto::app
{
namespace
{

using list_edits_parts::ChangeTo;
using list_edits_parts::ItemOfRow;
using list_edits_parts::LabelNames;
using ntdlib::commands::Command;
using ntdlib::commands::Kind;
using ntdlib::commands::kOne;

} // namespace

std::optional<ListEdit> TargetEdit(const ListTrack& track, std::size_t index, uint32_t target)
{
    if (index >= track.rows.size() || !ntdlib::commands::HasTarget(track.rows[index].command.kind))
    {
        return std::nullopt;
    }

    Command command = track.rows[index].command;
    if (!command.target_outside && command.target == target)
    {
        return std::nullopt;
    }

    command.target = target;
    command.target_outside = false;
    return ChangeTo(index, "Change Command", std::move(command));
}

ListEdit ConditionEdit(const ListTrack& track, std::size_t index)
{
    Command command = track.rows.at(index).command;
    command.conditional = !command.conditional;

    return ChangeTo(index, "Change Command", std::move(command));
}

std::optional<ListEdit> InsertEdit(const ListTrack& track, std::optional<std::size_t> index, Kind kind)
{
    if (track.rows.empty())
    {
        return std::nullopt;
    }

    // A new jump or call leads to the command after it. A new track start leads to the sequence's end. The track that
    // it opens ends there at once.
    const std::size_t before = std::min(index.value_or(track.rows.size() - 1), track.rows.size() - 1);
    const std::optional<uint32_t> item = edit::ItemAtOffset(track.offsets, track.rows[before].row.command);
    const auto end = static_cast<uint32_t>(track.offsets.empty() ? 0 : track.offsets.size() - 1);
    ListEdit edit;
    edit.action = ListEdit::Action::kInsert;
    edit.name = "Insert Command";
    edit.rows = {before};
    edit.command = edit::NewCommand(kind, track.ticks_per_quarter, kind == Kind::kOpenTrack ? end : item.value_or(0));
    if (track.console.driver && kind == Kind::kVolume)
    {
        edit.command.value.value = int64_t{track.console.driver->MostVolume(track.number)} * kOne;
    }

    return edit;
}

std::optional<ListEdit> RemovalEdit(const ListTrack& track, const std::set<uint32_t>& selected)
{
    // Each command goes once, in the stretch of its first row.
    std::set<uint32_t> taken;
    ListEdit edit;
    edit.action = ListEdit::Action::kRemove;
    edit.name = "Delete";
    for (std::size_t i = 0; i < track.rows.size(); i++)
    {
        const uint32_t command = track.rows[i].row.command;
        if (!IsLabel(track.rows[i]) && selected.contains(command) && taken.insert(command).second)
        {
            edit.rows.push_back(i);
        }
    }

    return edit.rows.empty() ? std::nullopt : std::optional<ListEdit>(std::move(edit));
}

edit::CommandPlace PlaceOf(const ListTrack& track, std::size_t index)
{
    const ListRow& row = track.rows.at(index);
    return edit::CommandPlace{row.row.command, track.stretches.at(row.row.stretch)};
}

std::optional<ListEdit> LabelEdit(const ListTrack& track, std::size_t index)
{
    if (track.timeline || index >= track.rows.size() || IsLabel(track.rows[index]))
    {
        return std::nullopt;
    }

    ListEdit edit;
    edit.action = ListEdit::Action::kAddLabel;
    edit.name = "Insert Label";
    edit.rows = {index};
    edit.label = edit::UniqueLabel(LabelNames(track).labels, "Label");

    return edit;
}

std::optional<ListEdit> LabelMoveEdit(const ListTrack& track, std::size_t label, std::size_t index)
{
    if (label >= track.rows.size() || index >= track.rows.size() || !IsLabel(track.rows[label]) ||
        IsLabel(track.rows[index]) || track.rows[label].row.command == track.rows[index].row.command)
    {
        return std::nullopt;
    }

    ListEdit edit;
    edit.action = ListEdit::Action::kMoveLabel;
    edit.name = "Move Label";
    edit.rows = {index};
    edit.label = track.rows[label].label;

    return edit;
}

std::optional<ListEdit> LabelRemovalEdit(const ListTrack& track, std::size_t index)
{
    if (index >= track.rows.size() || !IsLabel(track.rows[index]))
    {
        return std::nullopt;
    }

    ListEdit edit;
    edit.action = ListEdit::Action::kRemoveLabel;
    edit.name = "Delete Label";
    edit.rows = {index};
    edit.label = track.rows[index].label;

    return edit;
}

std::optional<ListEdit> SubroutineEdit(const ListTrack& track, const std::set<uint32_t>& selected)
{
    // The first row of each selected command, in the order of the commands. The rows of a selected call's run go with
    // the call: they sit deeper than the call until its return.
    std::map<uint32_t, std::size_t> firsts;
    std::optional<int> call_depth;
    for (std::size_t i = 0; i < track.rows.size(); i++)
    {
        const ListRow& row = track.rows[i];
        if (IsLabel(row) || (call_depth && row.depth > *call_depth))
        {
            continue;
        }

        call_depth.reset();
        const std::optional<uint32_t> item = ItemOfRow(track, i);
        if (item && selected.contains(row.row.command))
        {
            firsts.emplace(*item, i);
            if (row.command.kind == Kind::kCall)
            {
                call_depth = row.depth;
            }
        }
    }
    if (firsts.empty())
    {
        return std::nullopt;
    }

    ListEdit edit;
    edit.action = ListEdit::Action::kToSubroutine;
    edit.name = "Convert to Subroutine";
    for (const auto& [item, row] : firsts)
    {
        edit.rows.push_back(row);
    }

    // The commands run from the first row of the first one, as deep as their calls and loops take them. The rows of the
    // commands and of the runs of their calls follow each other from there.
    const uint32_t first = firsts.begin()->first;
    const uint32_t last = firsts.rbegin()->first;
    const std::size_t start = firsts.begin()->second;
    const int base = track.rows[start].depth;
    for (std::size_t i = start; i < track.rows.size(); i++)
    {
        const std::optional<uint32_t> item = ItemOfRow(track, i);
        const bool inside = item && *item >= first && *item <= last;
        if (!inside && track.rows[i].depth <= base)
        {
            break;
        }
        edit.depth = std::max(edit.depth, track.rows[i].depth);
    }

    // The name of the last label at the first command or before it, with _sub after it.
    std::string name = "sub";
    uint32_t named = 0;
    for (const auto& [label, at] : track.targets)
    {
        if (at <= first && (name == "sub" || at >= named))
        {
            name = label + "_sub";
            named = at;
        }
    }
    edit.label = edit::UniqueLabel(LabelNames(track).labels, name);
    if (track.timeline)
    {
        edit.refused =
            "In a timeline, the converter creates the subroutines. To make this edit, switch the sequence to Program "
            "in the Sequence section of the inspector.";
    }

    return edit;
}

std::optional<ListEdit> DissolveEdit(const ListTrack& track, std::size_t index)
{
    if (index >= track.rows.size() || IsLabel(track.rows[index]) || track.rows[index].command.kind != Kind::kCall)
    {
        return std::nullopt;
    }

    ListEdit edit;
    edit.action = ListEdit::Action::kDissolveCall;
    edit.name = "Dissolve Call";
    edit.rows = {index};

    return edit;
}

std::optional<ListEdit> RealCopyEdit(const ListTrack& track, std::size_t index)
{
    std::optional<ListEdit> edit = DissolveEdit(track, index);
    if (edit)
    {
        edit->action = ListEdit::Action::kCopySubroutine;
        edit->name = "Convert to Real Copy";
    }

    return edit;
}

std::optional<uint32_t> RunOfCall(const ListTrack& track, std::size_t index)
{
    if (index >= track.rows.size() || IsLabel(track.rows[index]))
    {
        return std::nullopt;
    }

    const ListRow& row = track.rows[index];
    const uint32_t next = row.row.stretch + 1;
    const bool call =
        row.command.kind == Kind::kCall && !row.command.target_outside && row.command.target < track.offsets.size();
    if (!call || next >= track.stretches.size() ||
        track.stretches[next].entry != ntdlib::sequence_export::Entry::kCall ||
        track.stretches[next].first != track.offsets[row.command.target])
    {
        return std::nullopt;
    }

    return next;
}

} // namespace hokuto::app
