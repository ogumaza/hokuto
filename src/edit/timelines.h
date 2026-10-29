// SPDX-License-Identifier: MIT

// The sequences that the program edits as timelines (format::File::timelines). NintendoWare's MIDI converter wrote each
// such sequence from a MIDI file. While the program edits a timeline, its sequence holds every bar in its track. The
// MIDI file held them the same way. Each note stands alone, and an edit of a bar changes that bar only. A file on disk
// holds the sequence as the console's bytes hold it, with the converter's subroutines. The program lays each timeline
// out without subroutines when it opens a file, and with them when it saves or exports one (edit/converter.h). The
// program edits the other sequences as programs: their commands as the console's bytes hold them.
//
// The two layouts of a timeline hold the same header and the same labels. A sound has to start in the header or at a
// label.

#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <vector>

#include "ntdlib/commands/sequence.h"

#include "edit/bytecode.h"
#include "edit/converter.h"
#include "format/file.h"
#include "song/sequence.h"

namespace hokuto::edit
{

// Whether `file` edits sequence `sequence` as a timeline.
bool IsTimeline(const format::File& file, uint32_t sequence);

// `file` as the program edits it: each timeline laid out with every bar in its track. When the file doesn't say which
// sequences are timelines, each sequence that the converter lays out again byte for byte becomes one. A timeline that
// its hints don't lay out byte for byte, or whose sounds start elsewhere than in its header or at a label, becomes a
// program.
format::File OpenTimelines(format::File file);

// `file` as a file on disk holds it: each timeline laid out with its subroutines. NintendoWare's converter writes a
// timeline that way. A save and an export start from it. Throws std::invalid_argument for a timeline that its hints
// can't lay out.
format::File FoldTimelines(const format::File& file);

// The commands of sequence `sequence` of `file` as a saved file holds them: a timeline laid out with its subroutines
// (FoldTimelines), and a program as it is. Throws std::invalid_argument for a timeline that its hints can't lay out.
ntdlib::commands::Sequence SavedSequence(const format::File& file, uint32_t sequence);

// A file after an edit, with each timeline that the edit changed in the converter's layout, and the new item of each
// item of those timelines, then of their ends, by their sequences. `replaced` lists the sequences that the edit
// replaced with the commands of a file, and the new sequences. The file edits them as programs, and WithNewModes finds
// their modes.
struct Settled
{
    format::File file;
    std::map<uint32_t, std::vector<std::optional<uint32_t>>> items;
    std::vector<uint32_t> replaced;
};

// `edited`, an edit of `before`, with each timeline that the edit changed laid out as the converter lays it out
// (SettledLayout). A timeline that `before` edits with other hints, or as a program, stays as the edit left it. The
// edit replaced the commands of the sequences of `replaced` with those of a file. Replace… makes such an edit. Each of
// them whose commands changed is replaced, and so is each new sequence. Throws std::invalid_argument with a message for
// the user for an edit that a timeline can't hold.
Settled SettleTimelines(const format::File& before, format::File edited, std::span<const uint32_t> replaced = {});

// `file` with sequence `sequence` edited as a program: laid out with its subroutines. A program stays as it is.
format::File AsProgram(const format::File& file, uint32_t sequence);

// `file` with sequence `sequence` edited as a timeline. A timeline stays as it is. Throws std::invalid_argument with a
// message for the user for a sequence that NintendoWare's converter didn't write, as far as Hokuto can tell, and for
// one whose sounds start elsewhere than in its header or at a label.
format::File AsTimeline(const format::File& file, uint32_t sequence);

// `file` with each sequence of `sequences` edited as OpenTimelines first finds it: as a timeline when the converter
// lays it out again byte for byte. For a sequence that an edit replaced, or a new one.
format::File WithNewModes(format::File file, std::span<const uint32_t> sequences);

// The loop of timeline `sequence` of `file` in ticks from its first command: the loop of every track that loops
// (SongLoopsOf). Each track counts its ticks from where the header starts it. Nothing when no track loops, when two
// tracks loop at other ticks, and where the header doesn't give the ticks.
std::optional<ConvertedLoop> TimelineLoop(const format::File& file, uint32_t sequence);

// The loop of timeline `sequence` of `file` as the song plays it, in ticks from its first command: TimelineLoop when
// every looping track loops at the same ticks. Looping tracks can loop from other ticks over passes of one length, such
// as a track that an earlier version of Hokuto added a pass late. The song then repeats from the tick at which the last
// of them enters its loop, for a pass of that length. Nothing when no track loops, when the tracks' passes differ in
// length, and where the header doesn't give the ticks.
std::optional<ConvertedLoop> TimelineSongLoop(const format::File& file, uint32_t sequence);

// Where the song of a timeline ends for the notes of its tracks that end: the tick, from the song's first command, at
// which the last of its looping tracks first jumps back to its loop's start (SongLoopsOf). Every looping track has
// played its first pass by then. That's the end of TimelineLoop's loop when the tracks loop together. A track pasted
// from a song with another loop can loop elsewhere, and so can a track that an earlier version of Hokuto added. `loops`
// says whether a track loops. `tick` holds nothing for a song without a loop, and where the header doesn't give the
// ticks at which its looping tracks start.
struct SongEnd
{
    bool loops = false;
    std::optional<uint32_t> tick;
};
SongEnd TimelineSongEnd(const format::File& file, uint32_t sequence);

// The arrangement's form of `read`, a read of a sound of timeline `sequence` of `file` by ntdlib's performer. The
// performer finds a loop where every track's state repeats. It can find the song's loop (TimelineSongLoop) a whole
// number of passes late, such as when the intro set a volume that the loop sets again, or when a track that ends plays
// a note in the loop's first pass. Each pass runs the same commands. The read then ends with the first pass of the
// song's loop (song::FirstPass). A read with changes of the variables, or of a program, stays as it is.
song::Sequence TimelineRead(const format::File& file, uint32_t sequence, song::Sequence read);

// The file with the loop of timeline `sequence` of `file` at `loop` (WithLoop). The edit's items give the new item of
// each note. Throws std::invalid_argument with a message for the user for a sequence that isn't a timeline or has no
// loop, and for a loop that doesn't end after its start, starts before the song's tracks or holds no note. `loop`
// counts its ticks as TimelineLoop does.
Edited MoveLoop(const format::File& file, uint32_t sequence, ConvertedLoop loop);

} // namespace hokuto::edit
