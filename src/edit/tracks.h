// SPDX-License-Identifier: MIT

// Edits of a sequence sound's tracks: a new track, a copy of a track, a track pasted from another sound, and a track's
// removal. The player gives a sequence sound the tracks that its entry in the archive's tables allocates. Track 0
// starts where the sound starts, and track starts (open_track) open the other tracks. A track start of a track that the
// sound doesn't allocate does nothing.
//
// A new track takes the lowest number from 1 that the sound's commands don't open. Another sound that runs the new
// track start mustn't allocate the number either. The new track's commands go after the sequence's commands, under a
// new label: the note wait off, the instrument, then waits that last as long as the sound. With a loop, the new track
// loops with the sound. Its track start goes in after the track starts and the allocation at the start of the sound's
// commands, or else before the sound's first command. The sounds that start at that command then start at the track
// start. Code that jumps there doesn't run it. The sound's entry allocates the new track.
//
// A timeline takes a new track, a copy and a pasted track as NintendoWare's MIDI converter lays out a song of more
// tracks. The track's code goes after the code of the tracks of lower numbers, under the converter's name of the track,
// such as SMF_SONG_Track_2. Its track start goes into the song's header. Where the sound starts at the first track's
// first command, the track start takes the sound's start and the labels there, but the first track's label. The
// timeline then lays the song out again (SettleTimelines), and the track has to hold only what a converted track holds.
//
// A copy of a track takes a number in the same way. Its commands are a copy of those that the track runs, its
// subroutines included, under new labels. The copy leaves out track starts and allocations. Neither takes any time. Its
// track start goes in after each track start that opens the track. A copy of track 0 starts where a new track starts.
// The copy runs the same commands at the same ticks as the track. A command that draws a random number or changes a
// variable then draws or changes it once more.
//
// A pasted track copies a track of another sound, or of the same one. Like a copy, it runs the commands that the track
// runs, under new labels. They leave out the commands that set the tempo, the timebase and the volume of the whole
// sound. The sound that takes the track keeps its tempo. The new track takes a number as a new track does, and its
// track start goes in where a new track's goes. A wait to the tick at which the other sound first started the track
// comes first. A track that its sound starts more than once plays once. The track's notes play the banks that they
// played in the other sound. A slot of the sound that holds a bank of the track takes the track's selects of that bank.
// Otherwise an empty slot that the sound's commands never select takes the bank. A bank select from a variable or a
// random number keeps its slot. A sound of the Wii plays one bank and has no bank select. A track from a sound of
// another bank can't go in there. A track from a sound that counts other ticks to a quarter note takes the sound's
// ticks: its waits, the lengths of its notes and the times over which its settings move scale with them. A time that
// comes from a variable, or falls between the sound's ticks, can't scale.
//
// A track's removal leaves its commands and its track starts in the sequence. The sound's entry stops allocating the
// track, and so does an allocation at the sound's start. Track 0 starts the other tracks, and stays. So does a track
// that opens other tracks of the sound. They would stop with it.

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "ntdlib/commands/sequence.h"

#include "edit/replace.h"
#include "format/file.h"
#include "song/sequence.h"

namespace hokuto::edit
{

// A file after an edit that adds a track, and the new track's number.
struct TrackEdit
{
    format::File file;
    int track = 0;
};

// The file after sound `sound` of `file` gains a new track. `places` gives where each sound plays (SoundPlaces), and
// `song` the sound as ntdlib's performer played it, for its length and its loop. The new track selects `instrument`
// first if there's one. Throws std::invalid_argument with a message for the user for a sound that plays no sequence of
// the file, a file without the archive's tables, commands that can't move, and a sound without a free track.
TrackEdit AddTrack(const format::File& file, std::span<const std::optional<SoundPlace>> places, uint32_t sound,
                   const song::Sequence& song, std::optional<Program> instrument);

// The file after sound `sound` of `file` gains a copy of its track `track`. Throws std::invalid_argument with a message
// for the user for the reasons that AddTrack gives, and for a track that the sound doesn't play.
TrackEdit DuplicateTrack(const format::File& file, std::span<const std::optional<SoundPlace>> places, uint32_t sound,
                         int track);

// A track of a sound, copied for a paste into another sound: the commands that the track runs, without track starts and
// allocations, and without the commands that set the whole sound's tempo, timebase or volume. The commands keep their
// order in the sequence file. A target counts from the first command, and so do `entry` and the labels that the
// sequence file gave the commands. The track starts at command `entry`. It can start after commands that it reaches,
// such as a track that jumps back to commands that other sounds share. `tick` is the tick at which the sound first
// started the track, and `timebase` the sound's ticks to a quarter note. `banks` holds the sound's bank slots: bank
// item IDs, or 0xFFFFFFFF for an empty slot. `sound` and `track` name the sound and the track.
struct CopiedTrack
{
    std::vector<ntdlib::commands::Command> commands;
    std::vector<ntdlib::commands::Label> labels;
    uint32_t entry = 0;
    uint32_t tick = 0;
    int timebase = 48;
    std::vector<uint32_t> banks;
    uint32_t sound = 0;
    int track = 0;
};

// Track `track` of sound `sound` of `file`, for PasteTrack. `song` gives the sound as ntdlib's performer played it, for
// the tick at which the track starts. A track that the sound starts more than once is copied from its first start.
// Throws std::invalid_argument with a message for the user for a sound that plays no sequence of the file, a track that
// the sound doesn't play, and a track whose commands jump to byte offsets.
CopiedTrack CopyTrack(const format::File& file, std::span<const std::optional<SoundPlace>> places, uint32_t sound,
                      const song::Sequence& song, int track);

// The file after sound `sound` of `file` gains `copied` as a new track. `song` gives the sound as ntdlib's performer
// played it, for its ticks to a quarter note. Throws std::invalid_argument with a message for the user for the reasons
// that AddTrack gives, for times that can't scale to the sound's ticks, and for a bank that the sound's slots can't
// take.
TrackEdit PasteTrack(const format::File& file, std::span<const std::optional<SoundPlace>> places, uint32_t sound,
                     const song::Sequence& song, const CopiedTrack& copied);

// The file after sound `sound` of `file` stops allocating its track `track`. Throws std::invalid_argument with a
// message for the user for track 0, a track that the sound doesn't allocate, a track that opens other tracks of the
// sound, a sound that would allocate no track, and a file without the archive's tables.
format::File RemoveTrack(const format::File& file, std::span<const std::optional<SoundPlace>> places, uint32_t sound,
                         int track);

} // namespace hokuto::edit
