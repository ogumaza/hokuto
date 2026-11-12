// SPDX-License-Identifier: MIT

// The helpers that the track edits of every engine use: the counts of tracks and bank slots, where a sound starts, new
// commands, the commands of a copy of a track, and a change of timebase. Only the files of the track edits include it.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <vector>

#include "ntdlib/commands/sequence.h"

#include "edit/bytecode.h"
#include "edit/replace.h"
#include "format/file.h"

namespace hokuto::edit::track_detail
{

using ntdlib::commands::Command;
using ntdlib::commands::Kind;
using ntdlib::commands::Sequence;

// A sequence's tracks are 0 to 15, and a sound's bank slots 0 to 3. A sound of the Wii plays one bank, in slot 0.
inline constexpr int kTracks = 16;
inline constexpr int kWiiBankSlots = 1;

// Where a sound plays: its sequence, and the item where it starts.
struct SoundStart
{
    uint32_t sequence = 0;
    uint32_t item = 0;
};

// Where sound `sound` of `file` plays. A sound whose entry leaves its start out starts at the first item. Throws
// std::invalid_argument for a sound that plays no sequence of the file.
SoundStart StartOf(const format::File& file, std::span<const std::optional<SoundPlace>> places, uint32_t sound);

// The first item of the data at the end of `sequence`: every item from it on is Kind::kData. The size of `sequence`
// where its last item isn't data.
uint32_t TrailingDataStart(const Sequence& sequence);

// `before` with `commands` at item `at`, before the data at its end. Every item of `before` moves by `moved`, and the
// targets and the labels that name them move with them (MoveTargets). A target in `commands` counts from the item
// `first_added`.
template <typename Moved>
Sequence InsertedBeforeData(const Sequence& before, std::span<const Command> commands, uint32_t at,
                            uint32_t first_added, Moved moved)
{
    Sequence after = before;
    MoveTargets(after, moved);
    std::vector<Command> added(commands.begin(), commands.end());
    for (Command& command : added)
    {
        if (ntdlib::commands::HasTarget(command.kind) && !command.target_outside)
        {
            command.target += first_added;
        }
    }

    after.items.insert(after.items.begin() + at, added.begin(), added.end());

    return after;
}

// A command of `kind` with the whole number `value`.
Command CommandOf(Kind kind, int64_t value);

// The kinds of command that a copy of a track leaves out: those that act on other tracks, and those that set the whole
// sound's tempo, timebase and volume. In the same sound, the track still sets those. A copy that set them again would
// run after other tracks in a tick, and the last setting wins there.
inline constexpr std::array kWholeSound = {Kind::kOpenTrack, Kind::kAllocateTracks, Kind::kTempo, Kind::kTimebase,
                                           Kind::kMainVolume};

// The commands that a copy of a track runs: those that `sequence` runs from the items of `starts`, without commands of
// the kinds of `left_out`. `moved` takes the index in the copy of each item that the copy runs or leaves out, and of
// the end. An item that the copy leaves out leads to the item after it.
std::vector<Command> CopyOf(const Sequence& sequence, std::span<const uint32_t> starts,
                            std::map<uint32_t, uint32_t>& moved, std::span<const Kind> left_out);

// `commands` with their times counted in ticks of a quarter note of `to` ticks in place of `from`: the lengths of the
// notes, the waits, and the times over which settings move. A time that changes drops the padding of its bytes. Nothing
// when a time comes from a variable or falls between the new ticks.
std::optional<std::vector<Command>> InTimebase(std::vector<Command> commands, int from, int to);

} // namespace hokuto::edit::track_detail
