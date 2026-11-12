// SPDX-License-Identifier: MIT

// What the note edits of Ubi Milan's driver (edit/ubimilan.h) and Rare's (edit/rare.h) share. Each puts the commands of
// a track or a sequence at their ticks, moves the ticks, and lays the commands out again in the order of their ticks.

#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/commands/units.h"

#include "edit/notes.h"
#include "song/driver.h"

namespace hokuto::edit::event_timelines
{

// A command at its tick. `group` sorts the commands of a tick: a moved note-off first, the commands that stay next, and
// a moved note last. `order` keeps the order of the items within a group.
struct Event
{
    int64_t tick = 0;
    int group = 1;
    uint64_t order = 0;
    ntdlib::commands::Command command;
    std::optional<uint32_t> old = {}; // the item before the edit
    bool added = false;               // a new note
    bool removed = false;
};

// The groups of Event.
inline constexpr int kMovedOff = 0;
inline constexpr int kInPlace = 1;
inline constexpr int kMovedNote = 2;

// The ticks of `wait`, a wait command.
inline int64_t TicksOf(const ntdlib::commands::Command& wait)
{
    return wait.value.value / ntdlib::commands::kOne;
}

// A wait of `ticks`.
inline ntdlib::commands::Command WaitOf(int64_t ticks)
{
    return song::ValueCommand(ntdlib::commands::Kind::kWait, ticks);
}

// The events of `events` that stay, in the order of their ticks, then their groups and their orders. Throws
// std::invalid_argument with a message for the user for an event before the start of the `noun` ("sequence" or
// "track") or past its end at tick `end`.
inline std::vector<const Event*> SortedEvents(const std::vector<Event>& events, int64_t end, const std::string& noun)
{
    std::vector<const Event*> sorted;
    for (const Event& event : events)
    {
        if (!event.removed)
        {
            sorted.push_back(&event);
        }
    }

    std::ranges::stable_sort(sorted, [](const Event* a, const Event* b)
                             { return std::tie(a->tick, a->group, a->order) < std::tie(b->tick, b->group, b->order); });
    for (const Event* event : sorted)
    {
        if (event->tick < 0)
        {
            throw std::invalid_argument("Commands can't move before the start of the " + noun + ".");
        }
        if (event->tick > end)
        {
            throw std::invalid_argument("Commands can't move past the end of the " + noun + " (tick " +
                                        std::to_string(end) + ").");
        }
    }

    return sorted;
}

// Moves `note` and its note-off `off` by the shift and the length of `edit`. A moved note sorts last at its tick, and
// its note-off first. Throws std::invalid_argument with a message for the user: `no_off` where the note has no
// note-off, and another for a length under a tick.
inline void MoveNote(const NoteEdit& edit, Event& note, Event* off, const char* no_off)
{
    if (edit.shift == 0 && !edit.length)
    {
        return;
    }
    if (off == nullptr)
    {
        throw std::invalid_argument(no_off);
    }

    const int64_t length = edit.length ? int64_t{*edit.length} : off->tick - note.tick;
    if (length < 1)
    {
        throw std::invalid_argument("A note lasts at least one tick.");
    }

    if (edit.shift != 0)
    {
        note.tick += edit.shift;
        note.group = kMovedNote;
    }

    off->tick = note.tick + length;
    off->group = kMovedOff;
}

} // namespace hokuto::edit::event_timelines
