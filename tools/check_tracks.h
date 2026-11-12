// SPDX-License-Identifier: MIT

// The parts of hokuto_check_tracks that its files share: the totals, an archive as the rounds read it, the comparisons
// of notes and the rounds. check_tracks.cpp describes the rounds.

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/dse/commands.h"
#include "ntdlib/dse/length_model.h"
#include "ntdlib/mp2k/song.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sequence_export/performance.h"
#include "xsflib/console.h"
#include "xsflib/files.h"

#include "check_support.h"
#include "checked_archive.h"
#include "checked_saves.h"
#include "edit/bytecode.h"
#include "edit/instruments.h"
#include "edit/mp2k.h"
#include "edit/notes.h"
#include "edit/rd2.h"
#include "edit/replace.h"
#include "edit/tracks.h"
#include "format/engine.h"
#include "format/file.h"
#include "notes_apart.h"
#include "song/ds.h"
#include "song/rd2.h"
#include "song/sequence.h"
#include "song/ubimilan.h"

namespace hokuto::tools::tracks
{

namespace edit = hokuto::edit;
namespace sar = ntdlib::sar;
namespace song = hokuto::song;
using hokuto::format::File;
using hokuto::tools::CheckedArchive;
using hokuto::tools::LoopText;
using hokuto::tools::SpreadIndices;
using ntdlib::commands::Command;
using ntdlib::commands::Kind;
using ntdlib::commands::Source;

// The sounds of a sequence file that take part in the rounds.
constexpr std::size_t kMostSounds = 4;

// What the check counted, for the summary.
struct Totals
{
    std::size_t sequences = 0;
    std::size_t sounds = 0;
    std::size_t unreadable = 0;
    std::size_t taking_part = 0;
    std::size_t added = 0;
    std::size_t added_holding = 0;
    std::size_t added_differing = 0;
    std::size_t notes = 0;
    std::size_t copied = 0;
    std::size_t copied_apart = 0;
    std::size_t copied_differing = 0;
    std::size_t removed = 0;
    std::size_t removed_apart = 0;
    std::size_t removed_differing = 0;
    std::size_t removed_ending = 0;
    std::size_t pasted = 0;
    std::size_t pasted_apart = 0;
    std::size_t pasted_differing = 0;
    std::size_t pasted_late = 0;
    std::size_t pasted_slots = 0;
    std::size_t pasted_ending = 0;
    std::size_t others = 0;
    std::size_t moved_loops = 0;
    std::size_t tick_notes = 0;
    std::size_t voice_notes = 0;     // edits that play otherwise only where the voices of Nintendo R&D2's driver decide
    std::size_t held_channels = 0;   // new notes of that driver whose voice notes of other tracks hold
    std::size_t loop_end_notes = 0;  // new notes at the end of a loop that the performer found a pass sooner
    std::size_t unstarted_notes = 0; // new notes that the driver never started
    std::size_t taken_notes = 0;     // new GBA notes whose PSG channel a later note took before they sounded
    std::size_t sooner_notes = 0;    // new notes that play first a whole number of loop passes sooner
    std::size_t added_to_empty = 0;  // new tracks in sounds that end in their first frame: their reads showed no track
    std::size_t shared = 0; // reads of songs that play an edited song, under another number or a GBA song's header
    std::size_t module_songs = 0;         // songs of Krawall that take a removal of another song of their module
    std::size_t silent_ends = 0;          // removals on Krawall after which the notes keep their silent ends
    std::size_t outgrown = 0;             // edits that a GBA ROM without free space has no room for
    std::size_t copied_without_loop = 0;  // copies after which the performer finds no loop, on Konami's driver
    std::size_t copied_without_sweep = 0; // copies that lose square 1's sweep, on Brownie Brown's driver
    std::size_t beat_notes = 0;        // new notes after which only notes of a tick differ, on Brownie Brown's driver
    std::size_t pasted_first_pass = 0; // pastes that compare up to a song's first pass, on Konami's driver
    std::size_t revealed = 0;          // GBA removals that bring a track past the music player's count into play
    std::size_t quiet_pastes = 0;      // GBA pastes that play otherwise only in quiet notes of the PSG channels
    std::size_t problems = 0;
    std::map<std::string, std::size_t> refusals;
};

// What the tool reads of the archive: its sounds, where each plays in the file, and each sequence sound as ntdlib's
// performer read it.
struct Archive
{
    std::string name;
    CheckedArchive archive;
    File file;
    std::vector<std::optional<edit::SoundPlace>> places;
    std::map<uint32_t, song::Sequence> songs;
};

// A note of a track: its start, key, length and velocity.
using TrackNote = std::tuple<uint32_t, uint8_t, uint32_t, uint8_t>;

// The fewest ticks that a note lasts in the comparisons that let a copy or a paste differ in notes of a tick.
constexpr uint32_t kTickNotes = 2;

// Whether the archive is a DS sound archive. As on the GBA, the driver shares its channels among the tracks.
inline bool g_ds = false;

// Whether the archive contains DSE files. Its driver shares its voices among the tracks.
inline bool g_dse = false;

// Whether the archive is a ROM of Nintendo R&D2's driver. Its tracks share the driver's voices by priority. A new note
// takes the voice of a note of the same or a lower priority when no voice is free, and a note can find no voice.
// ntdlib's performer marks no note of the driver as cut or never started.
inline bool g_rd2 = false;

// Whether the archive is a ROM of Brownie Brown's driver. A new track there uses the kind of channel of the track of
// the instrument that it plays, with the track's settings at its first note. The bytes of a note contain no velocity:
// each note plays at 127.
inline bool g_brownie = false;

// While true, no note's length counts in the comparisons, and every note can stand alone (VoicesDecide).
inline bool g_any_length = false;

// The notes that stood alone in the comparisons of VoicesDecide that held.
inline std::size_t g_voice_alone = 0;

// The most ticks that a frame of the DS's driver runs in `before` or `after`. NITRO-Composer's runs the fastest tempo
// over 240, rounded up. The DSE driver runs the ticks that pass at the tempo in the time of its frame, by the timebase
// of the sound. On the other consoles, 0.
uint32_t FrameTicks(const song::Sequence& before, const song::Sequence& after);

// The program of a GBA note that the driver never started. ntdlib's performer assigns such a note no instrument.
constexpr uint32_t kNoProgram = std::numeric_limits<uint32_t>::max();

// The notes that the commands of `track` of `sound` play, in their order. On the GBA and the DS, they include the notes
// that the driver never started (song::Track::unstarted), by their starts, keys and velocities, with kNoProgram. The
// driver shares its channels among the tracks. A copy of a track can then lose its notes to the original, and an edit
// of one track can change which notes of the others start.
std::vector<song::Note> CommandNotes(const song::Sequence& sound, const song::Track& track);

// How the notes of two lists pair up, in their order: the pairs of their indices, the notes that stand alone, and the
// first notes that pair with none when the lists differ.
struct Pairing
{
    std::vector<std::pair<std::size_t, std::size_t>> pairs;
    std::size_t alone = 0;
    std::optional<std::pair<std::size_t, std::size_t>> failed;
};

// The notes that stood alone in the pairings of lists that match (Pairing), for the summary.
inline std::size_t g_alone = 0;

// Whether `same` is true once no note length counts and every note can stand alone, on Nintendo R&D2's driver. A new
// note, a copy, a paste or a removal changes how many voices the song's notes ask for, and the driver then assigns
// other notes other voices, or none. Their lengths change, and a note can stay silent or sound. The tool counts such an
// edit apart, and counts the notes that stand alone in it.
template <class Same>
bool VoicesDecide(Same same)
{
    if (!g_rd2)
    {
        return false;
    }

    const std::size_t alone = g_alone;
    g_any_length = true;
    const bool held = same();
    g_any_length = false;
    g_voice_alone += held ? g_alone - alone : 0;
    g_alone = alone;

    return held;
}

// The notes of the track numbered `number` of `sound` that start before tick `end` and last `shortest` ticks or more,
// in their order (CommandNotes).
std::vector<TrackNote> NotesOf(const song::Sequence& sound, int number,
                               uint32_t end = std::numeric_limits<uint32_t>::max(), uint32_t shortest = 0);

// How `after` differs from `before`, two lists of notes: their counts, and the first notes that pair with none (Pair).
std::string NoteDifference(const std::vector<TrackNote>& before, const std::vector<TrackNote>& after);

// The numbers of the tracks of `sound`.
std::set<int> NumbersOf(const song::Sequence& sound);

// The numbers of the tracks of `after` that compare with `numbers`, those of `before` that an edit keeps. ntdlib's
// performer records no stretch for a track in a sound that lasts no time, and such a sound shows only its tracks with
// notes. An edit that makes the sound last then shows its other tracks, without notes, and they don't count.
std::set<int> ComparedNumbersOf(const song::Sequence& before, const song::Sequence& after,
                                const std::set<int>& numbers);

// `sound` without its quiet notes on the PSG channels of the GBA: those at half the full level or less, by their
// velocity times the volume of their track. A PSG channel's envelope moves in 16 steps, and such a note can end in its
// first frames. The state that the note before it leaves, and the frame where it starts, then decide whether it sounds.
// A track that plays beside other tracks or after other notes can then gain or lose such notes
// (tools/check_gba_edits.cpp).
song::Sequence WithoutQuietPsgNotes(song::Sequence sound);

// How a sound plays after an edit, against how it played before: what differs, or nothing, and whether the loop moved a
// whole number of passes.
struct Comparison
{
    std::optional<std::string> difference;
    bool moved = false;
};

// The numbers of `numbers`, such as "0, 1 and 3".
std::string NumbersText(const std::set<int>& numbers);

// Whether two lists of notes that start before tick `end` match: their starts, keys and velocities, and with `lengths`
// the lengths of the notes that end before `end`. A note that lasts to the end of the performance lasts as long as the
// sound. On the GBA and the DS, the length of a note that a later note cut, or that never started, doesn't count, and a
// note that never started can stand alone. Lengths can differ by `frame` ticks. The pairing contains the notes that
// pair up (Pair).
Pairing PairNotes(const std::vector<TrackNote>& before, const std::vector<TrackNote>& after, uint32_t end, bool lengths,
                  uint32_t frame = 0);

// Whether two lists of notes match by the rules of PairNotes.
bool SameNotes(const std::vector<TrackNote>& before, const std::vector<TrackNote>& after, uint32_t end, bool lengths,
               uint32_t frame = 0);

// How `after` compares with `before`. `after` has to play the tracks `numbers`, and each track of `before` among them
// with its notes. Track `copy` has to play the notes of track `original` of `before`, and a new track `empty` no notes.
// ntdlib's performer marks the loop on the first pass that the next pass repeats. An edit can change the state that it
// compares, and the loop can then start a whole number of passes earlier or later. The notes then have to match up to
// the earlier loop's end (SameTrackNotes). The comparison gives the first difference that it finds. Notes shorter than
// `shortest` ticks don't count. With `ends`, a note that lasts to the end of a sound without a loop in both matches
// whatever its lengths.
Comparison Compare(const song::Sequence& before, const song::Sequence& after, const std::set<int>& numbers,
                   std::optional<int> copy = std::nullopt, int original = 0, std::optional<int> empty = std::nullopt,
                   uint32_t shortest = 0, bool ends = false);

// The items that track `track` of the sound at `place` in `file` runs. A GBA song's header starts each track.
std::vector<bool> ItemsOfTrack(const File& file, const edit::SoundPlace& place, int track);

// Whether a DSE command makes the channel draw random numbers: the keys (0xD5) or pitches (0xD6) of its notes, or the
// waveforms 6 and 7 of an LFO that it sets (0xDC, 0xE4, 0xEC and 0xF0 in its fifth byte, and parameter 4 of 0xF2).
bool DrawsDseNumbers(const Command& command);

// Whether a command draws a random number, uses a variable, runs the game's code or runs only if a comparison held. A
// copy of a track with such a command can play otherwise than the track. On the GBA, a memory access that jumps
// compares a byte of the memory.
bool Chancy(const Command& command);

// The tick at which the arrangement of `sound` ends: the loop's end, or the sound's length.
uint32_t EndOf(const song::Sequence& sound);

// Whether a command among `items` of `sequence` meets `test`.
template <typename Test>
bool Any(const ntdlib::commands::Sequence& sequence, const std::vector<bool>& items, Test test)
{
    for (std::size_t item = 0; item < items.size(); item++)
    {
        if (items[item] && test(sequence.items[item]))
        {
            return true;
        }
    }

    return false;
}

// The track of `sound` with the most notes, the lowest first. Nothing for a sound without notes.
std::optional<int> BusiestTrack(const song::Sequence& sound);

// `sound`, a read of a sound of `file`, in the form that a check compares on Konami's driver. In the Dungeon Dice
// Monsters revision, a command can play a note on the next track (edit::KonamiPlaysOnNextTrack). In the read, such a
// note belongs to the track of the command, not to the track that plays it. On any revision, key 255 of a PSG note
// points past the driver's table of pitches, into bytes that another song can hold. Such a key doesn't count.
song::Sequence ComparableRead(const Archive& archive, const File& file, uint32_t number, const song::Sequence& sound);

// Every sequence sound of `version` read by ntdlib's performer, by its index. A sound whose read fails is missing, and
// counts as a problem.
std::map<uint32_t, song::Sequence> ReadAll(const Archive& archive, const File& version, const std::string& round,
                                           Totals& totals);

// The sounds that receive the edits of `edits` besides the sounds that it names, each with the sound whose edit it
// receives. On the DS, every sound of a sequence file starts at the file's first command (edit::SharingStart). An edit
// of one sound's tracks then reaches the file's other sounds. Each of them has to play the edit as the edited sound
// plays it. On Krawall, the songs of a module play the patterns of its sequence. An edit of one song's tracks reaches
// the module's other songs in the same way.
template <class Value>
std::map<uint32_t, uint32_t> Sharers(const Archive& archive, const std::map<uint32_t, Value>& edits)
{
    std::map<uint32_t, uint32_t> sharers;
    for (const auto& [sound, value] : edits)
    {
        if (archive.archive.Krawall())
        {
            const uint32_t sequence = archive.places[sound]->sequence;
            for (const auto& [other, song] : archive.songs)
            {
                if (archive.places[other] && archive.places[other]->sequence == sequence && !edits.contains(other))
                {
                    sharers.emplace(other, sound);
                }
            }
            continue;
        }

        for (const uint32_t other : edit::SharingStart(archive.places, sound))
        {
            if (archive.archive.ds && archive.songs.contains(other) && !edits.contains(other))
            {
                sharers.emplace(other, sound);
            }
        }
    }

    return sharers;
}

// Checks that every sound of `after` that the round didn't edit plays as before. A GBA song that shares an edited
// song's header has to play as the edited song plays. So does another number of an edited song of Konami's driver. A
// live stage's version of such a song plays the same commands on its terms, and compares only with another live stage's
// version.
void CheckOthers(const Archive& archive, const std::map<uint32_t, song::Sequence>& after,
                 const std::set<uint32_t>& edited, const std::string& round, Totals& totals);

// The first round: a new track for each sound of `chosen`, then a note in the new track of the first such sound of each
// sequence.
void AddTracks(const Archive& archive, const std::vector<uint32_t>& chosen, Totals& totals);

// The second round: a copy of the track with the most notes of each sound of `chosen`.
void CopyTracks(const Archive& archive, const std::vector<uint32_t>& chosen, Totals& totals);

// Whether track `number` of a song of Konami's driver is a PSG track that can play on the next track's channel, or lose
// its channel to the track before it. A removal then decides which notes play, in a revision whose records hold a pan.
bool KonamiPsgMate(const Archive& archive, int number);

// The third round: the removal of the last track with notes, other than track 0, of each sound of `chosen`.
void RemoveTracks(const Archive& archive, const std::vector<uint32_t>& chosen, Totals& totals);

// The fourth round: each sound of `chosen` takes a paste of the track with the most notes of another sound.
void PasteTracks(const Archive& archive, const std::vector<uint32_t>& chosen, Totals& totals);

} // namespace hokuto::tools::tracks
