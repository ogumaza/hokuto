// SPDX-License-Identifier: MIT

// The parts of hokuto_check_gba_midi that its files share: the totals, the notes that the comparison leaves out, and
// the MIDI file that the tool writes of each song. check_gba_midi.cpp describes the check.

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <utility>
#include <vector>

#include "format/file.h"
#include "song/sequence.h"

namespace hokuto::tools::gba_midi
{

// What the check counted, for the summary.
struct Totals
{
    std::size_t roms = 0;
    std::size_t songs = 0;
    std::size_t tracks = 0;
    std::size_t notes = 0;
    std::size_t ties = 0;   // notes that the MIDI files hold as ties of more than 223 ticks
    std::size_t silent = 0; // notes of velocity 0
    std::size_t unstarted_ties = 0;
    std::size_t crossing = 0; // notes that a later note of their key cuts short in the MIDI file
    std::size_t past_loop = 0;
    std::size_t sooner = 0;            // notes that end sooner than their commands say
    std::size_t brief = 0;             // notes that the loop's end cuts to less than a frame's ticks
    std::size_t commands_left_out = 0; // commands that the MIDI files don't hold
    std::size_t transposed = 0;        // notes whose keys include the transpositions of their tracks
    std::size_t unreported = 0;        // note commands that the performer reports as no note
    std::size_t outgrown = 0;          // songs that a ROM without free space has no room for
    std::size_t without_notes = 0;
    std::size_t passes_away = 0; // songs whose loop the performer found a whole number of passes away
    std::size_t warned = 0;      // songs whose import gave a warning
    std::size_t differ = 0;
    std::size_t failed = 0;
};

// The notes that the comparison leaves out: by their tracks, the starts and keys of the notes counted apart.
using LeftOut = std::map<std::size_t, std::set<std::pair<uint32_t, uint8_t>>>;

// The semitones by which the track of each note transposes it: by the tracks, the starts of the notes and the keys of
// their commands.
using Shifts = std::map<std::size_t, std::map<std::pair<uint32_t, uint8_t>, int>>;

// The MIDI file of `song`, sequence `sequence` of `file`, and the notes that the comparison leaves out.
struct Written
{
    std::vector<uint8_t> midi;
    LeftOut left_out;
    LeftOut loose;
    Shifts shifts;
    bool cut_short = false; // a note that a later note of its key cuts short
};

// Writes the MIDI file of `song`, sequence `sequence` of `file`, and counts its notes and commands in `totals`.
Written MidiOf(const hokuto::format::File& file, uint32_t sequence, const hokuto::song::Sequence& song, Totals& totals);

} // namespace hokuto::tools::gba_midi
