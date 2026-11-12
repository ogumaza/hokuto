// SPDX-License-Identifier: MIT

// hokuto_check_instruments: the commands that an edit changes, and the tracks that take them.

#include "check_instruments.h"

namespace hokuto::tools::instruments
{

std::set<int> TakingTracks(const ntdlib::commands::Sequence& sequence, const song::Console& console,
                           const song::Sequence& read, const std::set<uint32_t>& changed, std::optional<int> edited)
{
    if (console.driver == nullptr || !edit::EditsOf(*console.driver).StartedTracksTakeSettings())
    {
        return {};
    }

    std::map<int, std::set<int>> started;
    std::set<int> giving;
    for (const song::Track& track : read.tracks)
    {
        for (const uint32_t item : ItemsRun(sequence, console, track.stretches))
        {
            const ntdlib::commands::Command& command = sequence.items[item];
            if (command.kind == ntdlib::commands::Kind::kOpenTrack && !command.target_outside)
            {
                started[track.number].insert(command.index);
            }
            if (changed.contains(item))
            {
                giving.insert(track.number);
            }
        }
    }
    if (edited)
    {
        giving.insert(*edited);
    }

    std::set<int> taking;
    std::vector<int> pending(giving.begin(), giving.end());
    while (!pending.empty())
    {
        const int number = pending.back();
        pending.pop_back();
        for (const int other : started[number])
        {
            if (!giving.contains(other) && taking.insert(other).second)
            {
                pending.push_back(other);
            }
        }
    }

    return taking;
}

std::set<uint32_t> ItemsRun(const ntdlib::commands::Sequence& sequence, const song::Console& console,
                            const std::vector<song::Stretch>& stretches)
{
    std::set<uint32_t> items;
    const std::vector<uint32_t> offsets = edit::Offsets(sequence, console);
    for (const song::Stretch& stretch : stretches)
    {
        const std::optional<uint32_t> first = edit::ItemAtOffset(offsets, stretch.first);
        const std::optional<uint32_t> last = edit::ItemAtOffset(offsets, stretch.last);
        if (!first || !last)
        {
            continue;
        }

        for (uint32_t item = *first; item <= *last; item++)
        {
            items.insert(item);
        }
    }

    return items;
}

bool FollowsInstrument(const ntdlib::commands::Sequence& sequence, const song::Console& console,
                       const song::Sequence& song)
{
    using ntdlib::commands::Kind;
    using ntdlib::commands::Source;
    const std::vector<uint32_t> offsets = edit::Offsets(sequence, console);
    for (const song::Track& track : song.tracks)
    {
        for (const song::Stretch& stretch : track.stretches)
        {
            const std::optional<uint32_t> first = edit::ItemAtOffset(offsets, stretch.first);
            const std::optional<uint32_t> last = edit::ItemAtOffset(offsets, stretch.last);
            if (!first || !last)
            {
                continue;
            }

            bool note_wait = stretch.note_wait;
            for (uint32_t item = *first; item <= *last; item++)
            {
                const ntdlib::commands::Command& command = sequence.items[item];
                const bool on = command.value.source != Source::kValue || command.value.value != 0;
                const bool decided = command.value.source != Source::kValue || command.value.value <= 0;
                if ((command.kind == Kind::kTie || command.kind == Kind::kMonophonic) && on)
                {
                    return true;
                }
                if (command.kind == Kind::kNoteWait)
                {
                    note_wait = on;
                }
                else if (command.kind == Kind::kNote && note_wait && decided)
                {
                    return true;
                }
            }
        }
    }

    return false;
}

std::set<uint32_t> ChangedItems(const ntdlib::commands::Sequence& before, const ntdlib::commands::Sequence& after,
                                const edit::Edited& edited)
{
    std::set<uint32_t> changed;
    std::set<uint32_t> kept;
    for (uint32_t item = 0; item < before.items.size(); item++)
    {
        const std::optional<uint32_t> moved = item < edited.items.size() ? edited.items[item] : std::nullopt;
        if (!moved || *moved >= after.items.size())
        {
            changed.insert(item);
            continue;
        }

        kept.insert(*moved);
        ntdlib::commands::Command command = before.items[item];
        const bool target = ntdlib::commands::HasTarget(command.kind) && !command.target_outside;
        if (target && command.target < edited.items.size() && edited.items[command.target])
        {
            command.target = *edited.items[command.target];
        }
        if (command != after.items[*moved])
        {
            changed.insert(item);
        }
    }

    // A new command leads to the item after it.
    for (uint32_t item = 0; item < after.items.size(); item++)
    {
        if (kept.contains(item))
        {
            continue;
        }

        for (uint32_t old = 0; old < before.items.size(); old++)
        {
            if (edited.items[old] && *edited.items[old] > item)
            {
                changed.insert(old);
                break;
            }
        }
    }

    return changed;
}

std::string OtherChange(const ntdlib::commands::Sequence& before, const ntdlib::commands::Sequence& after,
                        const edit::Edited& edited, const edit::Program& to)
{
    using ntdlib::commands::Kind;
    std::vector<bool> kept(after.items.size(), false);
    for (uint32_t item = 0; item < before.items.size(); item++)
    {
        const std::optional<uint32_t> moved = item < edited.items.size() ? edited.items[item] : std::nullopt;
        if (!moved || *moved >= after.items.size())
        {
            return "item " + std::to_string(item) + " is gone";
        }
        kept[*moved] = true;
    }

    // The new value of a command that sets a part of the instrument.
    const auto value_of = [&to](Kind kind)
    {
        return int64_t{kind == Kind::kProgram ? to.number : to.slot} * ntdlib::commands::kOne;
    };

    for (uint32_t item = 0; item < before.items.size(); item++)
    {
        ntdlib::commands::Command command = before.items[item];
        const ntdlib::commands::Command& now = after.items[*edited.items[item]];
        const bool target = ntdlib::commands::HasTarget(command.kind) && !command.target_outside;
        if (target && command.target < edited.items.size() && edited.items[command.target])
        {
            const uint32_t moved = *edited.items[command.target];
            const bool new_between =
                now.target <= moved &&
                std::none_of(kept.begin() + now.target, kept.begin() + moved, [](bool old) { return old; });
            command.target = new_between ? now.target : moved;
        }
        if (command.kind == Kind::kProgram || command.kind == Kind::kBankSelect)
        {
            ntdlib::commands::Command changed = command;
            changed.value.value = value_of(command.kind);
            command = now == changed ? changed : command;
        }
        if (command != now)
        {
            return "item " + std::to_string(item) + " changed otherwise";
        }
    }

    for (uint32_t item = 0; item < after.items.size(); item++)
    {
        const ntdlib::commands::Command& command = after.items[item];
        const bool setter = (command.kind == Kind::kProgram || command.kind == Kind::kBankSelect) &&
                            command.value.value == value_of(command.kind);
        if (!kept[item] && !setter && command.kind != Kind::kJump)
        {
            return "item " + std::to_string(item) + " is new and neither sets the instrument nor jumps";
        }
    }

    return {};
}

} // namespace hokuto::tools::instruments
