// SPDX-License-Identifier: MIT

// The parts of hokuto_check_gba_edits that its files share: the plans of the edits, the totals and the comparison of
// each song. check_gba_edits.cpp describes the checks.

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>

#include "edit/notes.h"
#include "song/sequence.h"

namespace hokuto::tools::gba_edits
{

using hokuto::edit::NoteEdit;
using hokuto::song::Note;
using hokuto::song::Sequence;

// What an edit does to a note command.
enum class Change
{
    kMove,
    kVelocity,
    kRemove,
    kLonger,
};

// The edit of a note command, and what it should do to the notes that the command plays.
struct Plan
{
    Change change = Change::kVelocity;
    NoteEdit edit;
    uint32_t ticks = 0; // the command's length
    int velocity = 0;   // the command's velocity
};

// What the check counted, for the summary.
struct Totals
{
    std::size_t roms = 0;
    std::size_t sequences = 0;
    std::size_t unwritable = 0; // sequences that don't come back from Mp2kWritable without an edit
    std::size_t songs = 0;
    std::size_t notes = 0;
    std::size_t commands = 0;
    std::size_t moved = 0;
    std::size_t velocities = 0;
    std::size_t removed = 0;
    std::size_t longer = 0;
    std::size_t refused = 0;
    std::size_t wrong_sequences = 0;
    std::size_t differ = 0;
    std::size_t lengths = 0;   // notes that end sooner than their commands say
    std::size_t together = 0;  // notes that a move puts on the tick of another note of the same key
    std::size_t past_loop = 0; // notes that a move took to the end of the loop's first pass
    std::size_t contested = 0; // notes whose channels other notes may take or keep, and that come, go or end sooner
    std::size_t brief = 0;     // notes that can start and end in one of the driver's frames, and come or go
    std::size_t quiet = 0;     // quiet notes of the PSG channels that come or go
    std::size_t loops = 0;     // songs whose loop moved
    std::size_t outgrown = 0;  // songs whose edits a ROM without free space has no room for
    std::size_t colliding = 0; // songs whose notes collide more often after the edits
    std::size_t added_cut = 0; // notes that the edits cut short
    std::size_t added_unstarted = 0; // notes that the edits keep from starting
    std::size_t failed = 0;
};

// The PSG channel of each note of a song, or 0 for the DirectSound channels.
using ChannelOf = std::function<int(const Note&)>;

// Compares `before`, the song before the edits, with `after`, the song that the exported ROM plays. Prints a line for
// each difference. Returns whether the song differs.
bool Compare(const std::string& name, const Sequence& before, const Sequence& after,
             const std::map<uint32_t, Plan>& plans, const ChannelOf& channel_of, int direct_channels, Totals& totals);

} // namespace hokuto::tools::gba_edits
