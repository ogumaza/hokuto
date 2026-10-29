// SPDX-License-Identifier: MIT

// A flow analysis of a sequence's tracks: the states that a track can hold before each item. A state holds the settings
// that an analysis follows, such as a biquad filter's type and the setting of its value, or a bank slot and a program.
// A track follows its jumps, calls and loops. A call can come back from each return that its subroutine reaches, and a
// loop's end can go back to each loop's start that reaches it. A return outside every call runs on. A jump or a call
// past the bytecode's end ends the track. A command behind an if prefix may not run.

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "ntdlib/commands/sequence.h"

namespace hokuto::song
{

// The items that a track can run after each item of a sequence, and the item where each command that opens a track
// starts the track.
struct TrackPaths
{
    std::vector<std::vector<uint32_t>> next;
    std::vector<std::optional<uint32_t>> opens;
};

// The paths of the tracks of `sequence`. A return that a track reaches outside every call, from an item of `starts` or
// from the start of a track that a command opens, runs on to its next item.
TrackPaths TrackPathsOf(const ntdlib::commands::Sequence& sequence, std::span<const uint32_t> starts);

// For each item of `sequence`, the states that a track can hold before the item runs, sorted and without repeats.
// Tracks start at the items of `starts` with `initial`, and so do the tracks that the commands open.
// `after(command, item, before)` gives the states after the command at `item` from the states before it, sorted and
// without repeats. A command behind an if prefix also keeps the states before it. Nothing when the analysis would hold
// more than `most` states in all.
template <typename State, typename After>
std::optional<std::vector<std::vector<State>>> TrackStates(const ntdlib::commands::Sequence& sequence,
                                                           std::span<const uint32_t> starts, const State& initial,
                                                           After&& after, std::size_t most)
{
    const std::size_t size = sequence.items.size();
    std::vector<std::vector<State>> states(size);
    if (size == 0)
    {
        return states;
    }

    // Each item's states grow until no item's states change.
    const TrackPaths paths = TrackPathsOf(sequence, starts);
    std::vector<bool> queued(size);
    std::vector<uint32_t> pending;
    std::size_t held = 0;
    bool too_many = false;
    const auto add = [&](uint32_t item, const std::vector<State>& in)
    {
        if (item >= size || too_many)
        {
            return;
        }

        std::vector<State>& state = states[item];
        std::vector<State> merged;
        std::set_union(state.begin(), state.end(), in.begin(), in.end(), std::back_inserter(merged));
        if (merged.size() == state.size())
        {
            return;
        }

        held += merged.size() - state.size();
        too_many = held > most;
        state = std::move(merged);
        if (!queued[item])
        {
            queued[item] = true;
            pending.push_back(item);
        }
    };

    const std::vector<State> opening = {initial};
    for (const uint32_t start : starts)
    {
        add(start, opening);
    }
    while (!pending.empty() && !too_many)
    {
        const uint32_t item = pending.back();
        pending.pop_back();
        queued[item] = false;
        const ntdlib::commands::Command& command = sequence.items[item];
        std::vector<State> out = after(command, item, states[item]);
        if (command.conditional)
        {
            std::vector<State> both;
            std::set_union(out.begin(), out.end(), states[item].begin(), states[item].end(), std::back_inserter(both));
            out = std::move(both);
        }
        for (const uint32_t next : paths.next[item])
        {
            add(next, out);
        }
        if (paths.opens[item])
        {
            add(*paths.opens[item], opening);
        }
    }
    if (too_many)
    {
        return std::nullopt;
    }

    return states;
}

} // namespace hokuto::song
