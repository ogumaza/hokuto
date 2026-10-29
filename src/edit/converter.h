// SPDX-License-Identifier: MIT

// The songs that NintendoWare's MIDI converter wrote, laid out again by its rules. Composers wrote the music of the
// NintendoWare consoles in a DAW, and the converter turned each MIDI file into a sequence. It cut each track at the
// MIDI file's bar lines. A bar that repeats on its track went into a subroutine, once, and each copy calls it. The
// rules describe the converter's output.
//
// A converted song's labels start with "SMF_" and the MIDI file's name. Its commands run in this order: the Begin label
// and the allocation of the tracks, the Start label, the timebase and the track starts, then each track's code. Each
// track starts with the note wait off. Its commands follow at their ticks, with the loop's start as a label and its end
// as a jump back to the label. Music after the loop's end follows the jump, and the track ends. The track's subroutines
// follow its end, in the order of their first calls. The End label and an end close the song. Bytes that no command
// reaches can follow.
//
// The layout here writes a song's tracks again from their commands at their ticks: with the converter's subroutines, or
// with every bar in its track. A song's commands don't show everything that the converter read from the MIDI file.
// ConverterHints holds the rest: the bar lines, the blocks of commands before the song's start, and the bars whose
// treatment no rule explains.

#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "ntdlib/commands/sequence.h"

#include "format/file.h"

namespace hokuto::edit
{

// The hints that a file keeps for a timeline (format/file.h).
using format::ConverterHints;
using format::TrackHints;

// The hints of a sequence (see ReadConverted), or why it has none.
struct ConvertedSong
{
    std::optional<ConverterHints> hints;
    std::string problem; // empty with hints
};

// The hints with which LayOutConverted gives `sequence` back unchanged, for a sequence that holds one converted song,
// and nothing else but items after the song that nothing in the song targets. No hints for any other sequence, with the
// reason. The hints name as few bars as they can.
ConvertedSong ReadConverted(const ntdlib::commands::Sequence& sequence);

// `sequence`, a sequence that holds one converted song, laid out as the converter lays out the song's commands at their
// ticks: with subroutines, or with every bar in its track. The labels come in the order of their names. The converter
// sorts them that way. Throws std::invalid_argument for a sequence that doesn't hold one converted song.
ntdlib::commands::Sequence LayOutConverted(const ntdlib::commands::Sequence& sequence, const ConverterHints& hints,
                                           bool subroutines);

// The layout without subroutines of `edited`, an edit of `before`. Both hold one converted song laid out without
// subroutines. A rest that `edited` splits in two with nothing between joins again, unless `before` splits the same
// track's rest at the same tick: the MIDI file held an event there. A track whose blocks before the song's start no
// longer fit its commands at tick 0 loses them from `hints`. Throws std::invalid_argument with the reason for an
// `edited` that doesn't hold one converted song, or that the hints can't lay out with subroutines.
ntdlib::commands::Sequence SettledLayout(const ntdlib::commands::Sequence& before,
                                         const ntdlib::commands::Sequence& edited, ConverterHints& hints);

// The item of `joined` that each item of `split` became. `joined` is `split` with runs of waits joined into one wait
// each (SettledLayout). Throws std::invalid_argument for sequences that differ otherwise.
std::vector<std::optional<uint32_t>> ItemsAfterJoins(const ntdlib::commands::Sequence& split,
                                                     const ntdlib::commands::Sequence& joined);

// The item of `to` that holds the command of each item of `from`, then the end, two layouts of one converted song: an
// item of the header before the first track, the item of a label that both layouts hold, or the end. Nothing for
// another item. Throws std::invalid_argument for a sequence that doesn't hold one converted song.
std::vector<std::optional<uint32_t>> MatchingItems(const ntdlib::commands::Sequence& from,
                                                   const ntdlib::commands::Sequence& to);

// In `timeline`, a converted song laid out without subroutines, the item where new settings at the start of track
// `track` go: after the note wait that the track starts with, and after the blocks before the song's start that `hints`
// give. The new settings run once, before the labels there. Nothing for a track that the song lacks.
std::optional<uint32_t> TrackOpening(const ntdlib::commands::Sequence& timeline, const ConverterHints& hints,
                                     uint8_t track);

// In `timeline`, a converted song, the number of the track whose code the player runs from item `first`: the track
// whose label is at `first`, or the track that the commands from `first` run into. Such a path runs on, and takes the
// jumps that run always. The song numbers its tracks by their labels. The commands that open a track can give it
// another number. Nothing where the path ends or runs into no track's label.
std::optional<uint8_t> TrackRunFrom(const ntdlib::commands::Sequence& timeline, uint32_t first);

// Where the header of a converted song starts the song's tracks: the item of the first command that starts one, and the
// tick there.
struct ConvertedStart
{
    friend bool operator==(const ConvertedStart&, const ConvertedStart&) = default;

    uint32_t item = 0;
    uint32_t tick = 0;
};

// Where the header of `timeline`, a converted song, starts the song's tracks when the player runs it from item `first`.
// The path runs as in TrackRunFrom, up to the first command that opens one of the song's tracks, the jump to a track's
// label, or the label where the header runs into it. The tick adds up the header's waits on the way: a header can wait
// before the song starts, such as one written by hand. Nothing where `first` lies after the first track's label, where
// the path ends before it starts a track, and where a wait on the way takes its time from a variable, a random number
// or a comparison.
std::optional<ConvertedStart> SongStartFrom(const ntdlib::commands::Sequence& timeline, uint32_t first);

// The tick at which the header of `timeline`, a converted song, starts each of the song's tracks when the player runs
// it from item `first`, by the tracks' labels. The path runs as in SongStartFrom, and on past the commands that open
// the song's tracks, up to the jump to a track's label, the label where the header runs into it, or the header's end.
// The header can open tracks at other ticks: Paste Track opens a pasted track where the sound starts, before the
// header's waits. A track that the path doesn't start, such as one that only a sound's start reaches, takes the tick
// where the header starts the song's first track (SongStartFrom). Nothing where the path starts no track, and where a
// wait on the way takes its time from a variable, a random number or a comparison.
std::optional<std::map<std::string, uint32_t>> TrackStartsFrom(const ntdlib::commands::Sequence& timeline,
                                                               uint32_t first);

// Where a new track's code goes in a converted song. This follows the converter's layout of a song of more tracks.
struct ConvertedTrackPlace
{
    // The track's label: the song's labels' start, Track_ and the track's number.
    std::string label;

    // The item in front of which the code goes: the label of the song's first track of a higher number, or else the End
    // label.
    uint32_t before = 0;

    // The labels of the song's tracks.
    std::vector<std::string> tracks;
};

// Where track `number`'s code goes in `timeline`, a converted song laid out without subroutines. Throws
// std::invalid_argument for a sequence that doesn't hold one converted song, and with a message for the user for a song
// that holds a track of that number already.
ConvertedTrackPlace NewTrackPlace(const ntdlib::commands::Sequence& timeline, uint8_t number);

// The loop of a converted song: the tick of the label that its tracks' jumps go back to, and the tick of the jumps.
struct ConvertedLoop
{
    friend bool operator==(const ConvertedLoop&, const ConvertedLoop&) = default;

    uint32_t start = 0;
    uint32_t end = 0;
};

// The loop of `timeline`, a converted song laid out without subroutines. A track loops with a jump that runs always,
// back to a label of the track. Nothing when no track loops, or when two tracks loop at other ticks.
std::optional<ConvertedLoop> LoopOfConverted(const ntdlib::commands::Sequence& timeline);

// The loop of each track of `timeline`, a converted song laid out without subroutines, that loops as in
// LoopOfConverted, in the order of the song's tracks. The ticks count from the song's first command: each track's ticks
// start where the header starts the track when the player runs it from item 0 (TrackStartsFrom). Tracks can loop at
// other ticks, such as a track pasted from a song with another loop. Nothing where the header doesn't give the tick at
// which a looping track starts.
std::optional<std::vector<ConvertedLoop>> SongLoopsOf(const ntdlib::commands::Sequence& timeline);

// `timeline`, a converted song laid out without subroutines, with its loop at `loop`: each looping track's label at the
// loop's start and its jump at the loop's end. The label goes before the commands at its tick, and so does the jump.
// Commands after the jump end with an end, at the tick where the track ended. A track counts the loop's ticks from its
// tick in `starts`, by its label (TrackStartsFrom), and a track without one from the song's first command. Throws
// std::invalid_argument with a message for the user for a song without a loop, for a loop that doesn't end after its
// start, for a loop that starts before a looping track does, and for a loop without a note. A loop without a note
// sounds nothing. ntdlib's performer then finds no loop.
ntdlib::commands::Sequence WithLoop(const ntdlib::commands::Sequence& timeline, const ConverterHints& hints,
                                    ConvertedLoop loop, const std::map<std::string, uint32_t>& starts = {});

// A song without notes. NintendoWare's MIDI converter writes it for a MIDI file named `name` that holds a tempo of 120
// beats a minute and nothing else: the Begin label with the allocation of track 0, then the Start label and track 0.
// The track turns the note wait off, sets the tempo and ends. The End label and an end follow. A sound starts at the
// Start label, item kEmptySongStart. The program edits the song as a timeline.
ntdlib::commands::Sequence EmptySong(const std::string& name);

// The item of an empty song where a sound starts: the Start label's.
inline constexpr uint32_t kEmptySongStart = 1;

} // namespace hokuto::edit
