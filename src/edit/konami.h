// SPDX-License-Identifier: MIT

// The edits of a song of Konami's GBA driver, by the rules of its bytes (ntdlib/konami/commands.h). A track's bytes are
// a delay, then commands that each take a delay after them. A call's target is at a delay. The call plays a count of
// commands from the target, and comes back to the delay after the call.
//
// After an edit, a delay of 0 is inserted between two commands that are next to each other, and before a command where
// a track, or a call, starts. Two delays next to each other become one. The count of a call follows the commands that
// its part of the bytes contains after the edit.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/konami/seqformat.h"

#include "edit/bytecode.h"
#include "edit/commands.h"
#include "edit/drivers.h"
#include "edit/notes.h"
#include "edit/tracks.h"
#include "song/archive.h"
#include "song/konami.h"

namespace hokuto::edit
{

// The edits of Konami's driver (edit/drivers.h).
const DriverEdits& KonamiEdits();

// The revision of Konami's driver whose bytes `console` follows, or nothing for another driver or console.
inline std::optional<ntdlib::konami::Revision> KonamiOf(const song::Console& console)
{
    return song::KonamiRevisionOf(console.driver);
}

// The commands that a call of `commands` plays: the items from `first`, its target, up to `end`. Item `end` isn't one
// of the commands.
struct KonamiCallRange
{
    uint32_t call = 0;
    uint32_t first = 0;
    uint32_t end = 0;
};

// The range of each call of `commands` whose target is among the items.
std::vector<KonamiCallRange> KonamiCallRanges(const ntdlib::commands::Sequence& commands);

// The ranges of the calls of `old` among `after`, the commands of the same sequence after `edited`, an edit of its
// notes or its settings. A call's count follows its commands only when the edit is written (DriverEdits::Writable), and
// KonamiCallRanges can't give the ranges before that. A call that the edit removed has no range.
std::vector<KonamiCallRange> KonamiCallRangesAfter(const ntdlib::commands::Sequence& old,
                                                   const ntdlib::commands::Sequence& after, const Edited& edited);

// Whether `command`, a command of Konami's driver, plays a note on the next track: the revision without a mixer has
// such a note.
bool KonamiPlaysOnNextTrack(const ntdlib::commands::Command& command);

// Whether `command`, a command of `revision` of Konami's driver on a track whose notes are like `note`, starts the
// track's note again. A volume does on a PSG track, and on every track of the Dungeon Dice Monsters revision
// (ntdlib/konami/seqformat.h). The arrangement then shows a note that names the note command before it. After a PSG
// note command at volume 0, the note names the volume command (note_detail::KonamiSilentNote).
bool KonamiStartsNoteAgain(const ntdlib::commands::Command& command, const ntdlib::commands::Command& note,
                           ntdlib::konami::Revision revision);

// The longest delay that `revision`'s bytes hold, in ticks.
uint32_t KonamiLongestWait(ntdlib::konami::Revision revision);

// How many of the places that a song loop takes a track back to reach another song loop without a delay. Those places
// are `entries`, the items where tracks start, and the item after each loop point. After such a song loop, the driver
// never ends its frame, and the console hangs. A call's commands count where the call stands.
std::size_t KonamiLoopsWithoutWaits(const ntdlib::commands::Sequence& commands, std::span<const uint32_t> entries);

// Checks `edited`, an edit of the notes or the settings of sequence `sequence` of `before`, a file of Konami's driver:
// every volume command (C0-CF) that starts a note again still starts it at the key that it had before the edit. A
// volume command starts a PSG channel at the key of the last note command that its track ran. A new, moved or removed
// note command before it, or a move of the volume command, would change the key of a note that the edit doesn't name.
// Throws std::invalid_argument with a message for the user for such an edit. EditNotes, MoveSettings and AddSettings
// pass the check (DriverEdits::Placed). The List Editor's edits of single commands (ChangeCommand, InsertCommand,
// RemoveCommands) stay outside it on purpose: they change the bytes as the user wrote them.
// The check walks each track's commands in the order in which the driver runs them. For a song that the walk doesn't
// follow, such as one with a call inside a call, it refuses every edit that adds, removes or moves a command.
void CheckKonamiVolumeStarts(const format::File& before, const Edited& edited, uint32_t sequence);

// How the walk of CheckKonamiVolumeStarts takes a sequence. `starts` are the volume commands that a track runs, each
// with the note command at whose key it starts a note: on the track's first pass and, where the song's loop starts the
// track again, on its second pass, for the track of the commands and for the track after it. Sorted. A check compares
// them with the reader's (song::KonamiVolumeStarts): the walk and the driver's model have to agree.
// `stopped` is the message of a song that the walk doesn't follow. Such a song has no starts. `loop_cuts` is set
// where the reason is the song's loop: it comes before a note command or a loop point of a track. `later_loop_cuts` is
// set too where the first loop cuts no track that way, and a later one does. `ends_apart` is set where a track that
// the loop starts again ends at another time than the song loops.
struct KonamiWalked
{
    std::vector<song::KonamiVolumeStart> starts;
    std::optional<std::string> stopped;
    bool loop_cuts = false;
    bool later_loop_cuts = false;
    bool ends_apart = false;
};

KonamiWalked KonamiWalkOf(const format::File& file, uint32_t sequence);

// `edited`, an edit of the notes or the settings of sequence `sequence` of `before`, a file of Konami's driver, with
// each call as long as it was. A call returns after its last command, and the driver reads no delay after that command.
// When an edit moves or removes that command, the delay after the call makes up the difference. Where the edit puts a
// command at the tick where the call returns, a new delay is inserted before the command. Leaves an `Edited::items`
// without an entry for each item alone. Throws std::invalid_argument with a message for the user when that delay can't.
Edited KonamiKeepCallLengths(const format::File& before, Edited edited, uint32_t sequence);

// `edits` and `additions` as Konami's driver stores them (konami_notes.cpp), before KonamiKeepCallLengths and Writable.
Edited EditKonamiNotes(const format::File& file, uint32_t sequence, std::span<const NoteEdit> edits,
                       std::span<const NewNote> additions);

// The note to add at `tick` of `stretch`, with the arrangement's `key`, on `track`, a track of a song of Konami's
// driver in sequence `sequence` of `file`. It plays what the note before it on the track's commands plays, or else the
// next one, and is as many semitones from that note. A track without notes plays its PSG channel, or the first sample.
// A copy of a sample's note on a sample track plays `copied` instead. Throws std::invalid_argument with kKonamiNoRoom
// for a track without notes whose commands in `stretch` end at the stretch's start, at `tick` or before it.
NewNote NewKonamiNote(const format::File& file, uint32_t sequence, const song::Track& track,
                      const song::Stretch& stretch, uint32_t tick, int key, int velocity, uint32_t length,
                      std::optional<KonamiSampleNote> copied = {});

// ArgumentRange on track `track` of revision `revision` of Konami's driver (command_ranges.cpp).
std::optional<ValueRange> KonamiRange(const ntdlib::commands::Command& command, ntdlib::commands::Source source,
                                      bool time, ntdlib::konami::Revision revision, int track);

// The Project menu's edits of tracks on Konami's driver (konami_tracks.cpp).
TrackEdit AddKonamiTrack(const format::File& file, std::span<const std::optional<SoundPlace>> places, uint32_t sound);
TrackEdit DuplicateKonamiTrack(const format::File& file, std::span<const std::optional<SoundPlace>> places,
                               uint32_t sound, int track);
CopiedTrack CopyKonamiTrack(const format::File& file, std::span<const std::optional<SoundPlace>> places, uint32_t sound,
                            const song::Sequence& song, int track);
TrackEdit PasteKonamiTrack(const format::File& file, std::span<const std::optional<SoundPlace>> places, uint32_t sound,
                           const CopiedTrack& copied);
format::File RemoveKonamiTrack(const format::File& file, std::span<const std::optional<SoundPlace>> places,
                               uint32_t sound, int track);

} // namespace hokuto::edit
