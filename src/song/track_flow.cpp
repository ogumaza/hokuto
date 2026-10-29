// SPDX-License-Identifier: MIT

#include "song/track_flow.h"

#include <map>
#include <set>

namespace hokuto::song
{
namespace
{

using ntdlib::commands::Command;
using ntdlib::commands::Kind;
using ntdlib::commands::Sequence;

// A track's deepest loops inside one another that the analysis follows. NintendoWare's players hold 10 loops and calls
// at most.
constexpr int kDeepestLoop = 16;

// For each loop's end, the items after the loops' starts that the end can go back to. A loop's start reaches its end
// within its call level, past as many other loops' starts as ends.
using LoopBacks = std::map<uint32_t, std::vector<uint32_t>>;

LoopBacks LoopBacksOf(const Sequence& sequence)
{
    LoopBacks backs;
    const std::size_t size = sequence.items.size();
    for (uint32_t start = 0; start < size; ++start)
    {
        if (sequence.items[start].kind != Kind::kLoopStart)
        {
            continue;
        }

        // The walk from the loop's first item, with the depth of the other loops that it has entered.
        std::set<std::pair<uint32_t, int>> seen;
        std::vector<std::pair<uint32_t, int>> pending = {{start + 1, 0}};
        const auto add = [&pending](uint32_t item, int depth)
        {
            pending.emplace_back(item, std::min(depth, kDeepestLoop));
        };
        while (!pending.empty())
        {
            const auto [item, depth] = pending.back();
            pending.pop_back();
            if (item >= size || !seen.insert({item, depth}).second)
            {
                continue;
            }

            const Command& command = sequence.items[item];
            switch (command.kind)
            {
            case Kind::kLoopStart:
                add(item + 1, depth + 1);
                break;
            case Kind::kLoopEnd:
                if (depth == 0)
                {
                    backs[item].push_back(start + 1);
                }
                else
                {
                    add(item + 1, depth - 1);
                }
                break;
            case Kind::kJump:
                if (!command.target_outside)
                {
                    add(command.target, depth);
                }
                break;
            case Kind::kReturn:
            case Kind::kEnd:
            case Kind::kData:
                break;
            default:
                add(item + 1, depth);
                break;
            }

            if (command.conditional && command.kind != Kind::kData)
            {
                add(item + 1, depth);
            }
        }
    }

    return backs;
}

// Calls `visit` with each item that can run after `item` within the call level that runs it: the next item, unless the
// command always jumps, returns or ends, and a jump's target. A call's next item stands for its return. A loop's end
// can go back to the items of `backs` too. A jump or a call past the bytecode's end ends the track.
template <typename Visit>
void VisitLevel(const Sequence& sequence, uint32_t item, const LoopBacks& backs, Visit&& visit)
{
    const Command& command = sequence.items[item];
    if (command.kind == Kind::kData)
    {
        return;
    }

    if (command.kind == Kind::kJump && !command.target_outside)
    {
        visit(command.target);
    }
    if (const auto found = backs.find(item); found != backs.end())
    {
        for (const uint32_t back : found->second)
        {
            visit(back);
        }
    }
    const bool stops = command.kind == Kind::kJump || command.kind == Kind::kReturn || command.kind == Kind::kEnd ||
                       (command.kind == Kind::kCall && command.target_outside);
    if (!stops || command.conditional)
    {
        visit(item + 1);
    }
}

// The items that a walk within the call level reaches from `pending`. With `run_on`, a return also goes on to the next
// item. TrackPathsOf sets it for the walk from the tracks' starts, outside every call: a return there has no call to
// return to, and the player runs on past it.
std::set<uint32_t> ReachedInLevel(const Sequence& sequence, std::vector<uint32_t> pending, const LoopBacks& backs,
                                  bool run_on)
{
    std::set<uint32_t> reached;

    const auto add = [&pending](uint32_t next)
    {
        pending.push_back(next);
    };
    while (!pending.empty())
    {
        const uint32_t item = pending.back();
        pending.pop_back();
        if (item >= sequence.items.size() || !reached.insert(item).second)
        {
            continue;
        }

        VisitLevel(sequence, item, backs, add);
        if (run_on && sequence.items[item].kind == Kind::kReturn)
        {
            add(item + 1);
        }
    }

    return reached;
}

} // namespace

TrackPaths TrackPathsOf(const Sequence& sequence, std::span<const uint32_t> starts)
{
    const std::size_t size = sequence.items.size();
    TrackPaths paths{.next = std::vector<std::vector<uint32_t>>(size),
                     .opens = std::vector<std::optional<uint32_t>>(size)};

    // The tracks' starts: `starts`, and the targets of the commands that open tracks.
    std::vector<uint32_t> tracks(starts.begin(), starts.end());
    for (uint32_t item = 0; item < size; ++item)
    {
        const Command& command = sequence.items[item];
        if (command.kind == Kind::kOpenTrack && !command.target_outside)
        {
            tracks.push_back(command.target);
            paths.opens[item] = command.target;
        }
    }

    // The returns outside every call go on to their next items. The others go back to the items after the calls that
    // reach them.
    const LoopBacks backs = LoopBacksOf(sequence);
    const std::set<uint32_t> outside = ReachedInLevel(sequence, tracks, backs, true);
    std::map<uint32_t, std::vector<uint32_t>> back_to;
    std::map<uint32_t, std::vector<uint32_t>> returns_of;
    for (uint32_t item = 0; item < size; ++item)
    {
        const Command& command = sequence.items[item];
        if (command.kind != Kind::kCall || command.target_outside)
        {
            continue;
        }

        auto [found, added] = returns_of.try_emplace(command.target);
        if (added)
        {
            for (const uint32_t reached : ReachedInLevel(sequence, {command.target}, backs, false))
            {
                if (sequence.items[reached].kind == Kind::kReturn)
                {
                    found->second.push_back(reached);
                }
            }
        }
        for (const uint32_t ret : found->second)
        {
            back_to[ret].push_back(item + 1);
        }
    }

    // A call goes into its subroutine, and comes back from the subroutine's returns. A return outside every call goes
    // on to its next item.
    for (uint32_t item = 0; item < size; ++item)
    {
        const Command& command = sequence.items[item];
        std::vector<uint32_t>& next = paths.next[item];
        const auto to = [&next](uint32_t after)
        {
            next.push_back(after);
        };
        if (command.kind == Kind::kCall)
        {
            if (!command.target_outside)
            {
                to(command.target);
            }
            if (command.conditional)
            {
                to(item + 1);
            }
            continue;
        }
        VisitLevel(sequence, item, backs, to);
        if (command.kind == Kind::kReturn)
        {
            if (const auto found = back_to.find(item); found != back_to.end())
            {
                next.insert(next.end(), found->second.begin(), found->second.end());
            }
            if (outside.contains(item))
            {
                to(item + 1);
            }
        }
    }

    return paths;
}

} // namespace hokuto::song
