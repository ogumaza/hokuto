// SPDX-License-Identifier: MIT

// What tracks.cpp and track_copies.cpp share: the tracks that a sound allocates, the label and the place of a new
// track, the checks before one is added, and the file with a new track. Only those files include it.

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "ntdlib/commands/sequence.h"

#include "edit/track_common.h"
#include "edit/tracks.h"
#include "format/file.h"

namespace hokuto::edit::tracks_parts
{

using ntdlib::commands::Command;
using ntdlib::commands::Label;
using ntdlib::commands::Sequence;
using track_detail::SoundStart;

// A command to put in front of item `before` of a sequence. With `takes_start`, the labels and the sounds' starts at
// the item name the new command. The targets of jumps, calls and track starts keep naming the item.
struct Insertion
{
    uint32_t before = 0;
    Command command;
    bool takes_start = false;
};

// The tracks that sound `sound` of `file` allocates, a bit each, where it starts at item `start` of `sequence`: those
// of its entry in the archive's tables and of an allocation there. A DS sound has no such entry. NITRO-Composer always
// allocates track 0, and reads an allocation only as a sequence's first command (ntdlib's ntr::snd::Driver).
uint32_t AllocationOf(const format::File& file, const Sequence& sequence, uint32_t sound, uint32_t start);

// The label of new track `number` of sound `sound`: the name of the sound and the name of the track. The edits read no
// names of a DS archive's sounds. The label of a DS track contains only the name of the track.
std::string TrackLabel(const format::File& file, uint32_t sound, int number);

// The items of the track starts of track `track` among the commands of a sound that starts at item `start` of
// `sequence`.
std::vector<uint32_t> StartsOfTrack(const Sequence& sequence, uint32_t start, int track);

// Throws std::invalid_argument with a message for the user when a track can't go into `sequence`, sound `sound`'s
// sequence in `file`: the file has no archive tables, or the sequence's commands can't move. A DS sound allocates its
// tracks without the tables.
void CheckRoom(const format::File& file, const Sequence& sequence, uint32_t sound);

// With a track start before each item of `before`, the lowest track from 1 that the commands of sound `sound` don't
// open. Another sound that reaches such an item may run the track start. It mustn't allocate the track either. Throws
// std::invalid_argument when every track is taken.
int FreeTrack(const format::File& file, std::span<const std::optional<SoundPlace>> places, uint32_t sound,
              std::span<const uint32_t> before);

// The item before which a new track start goes in the commands of a sound that starts at item `start` of `sequence`:
// after the last track start or allocation among the commands that run there before any time passes. Those stop at a
// note, a wait, a branch and an end. Without a track start or an allocation there, the sound's first command.
uint32_t TrackStartPlace(const Sequence& sequence, uint32_t start);

// The file after `insertions` go into sequence `start.sequence` of `file` once `track` and its `labels` go after the
// sequence's items, and sound `sound` allocates track `number`. An insertion's target counts the new items after the
// sequence's. A timeline takes the track as NintendoWare's MIDI converter lays out a song of more tracks
// (WithTimelineTrack). On the DS, the allocation at the sound's start allocates the track. A sound without one gets one
// before its first command. Throws std::invalid_argument for commands that the console's bytecode can't hold.
TrackEdit WithNewTrack(const format::File& file, SoundStart start, uint32_t sound, int number,
                       std::span<const Command> track, std::span<const Label> labels,
                       std::vector<Insertion> insertions);

} // namespace hokuto::edit::tracks_parts
