// SPDX-License-Identifier: MIT

// The steps of the note edits that their files share: the rewrite of a block, the end that a note can't last past, the
// runs that turn the note wait off, and the edit itself. Only the files of the note edits include it.

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/konami/seqformat.h"
#include "ntdlib/sequence_export/performance.h"

#include "edit/bytecode.h"
#include "edit/note_blocks.h"
#include "edit/notes.h"
#include "edit/timelines.h"
#include "format/file.h"
#include "song/sequence.h"

namespace hokuto::edit::note_detail
{

using ntdlib::commands::Command;

// What an edit does to a block: the notes that it takes out, by item, and the commands that it puts in, at their ticks
// from the block's start. A moved note's item comes with it, and a new command's place among the edit's new commands.
struct BlockChange
{
    struct Placed
    {
        uint32_t tick = 0;
        Command command;
        std::optional<uint32_t> item;
        std::optional<std::size_t> addition;
    };

    std::set<uint32_t> removed;
    std::vector<Placed> placed;
    bool extend = false; // a note goes past the end of a timeline's track, and the track lasts to the note's end

    // The tick from the block's start that the block lasts to at least, before its last command: the end of a note
    // that runs past the end of a timeline's track.
    uint32_t until = 0;
};

// Rewrites block [`begin`, `end`) of `items` with `change` onto the end of `out`. `edited` takes the new index of each
// item that stays, of each moved note and of each new command. A note goes in before the wait at its tick, after the
// other commands at that tick. A new setting goes in before the first note at its tick. Inside a wait, a new command
// splits the wait in two. At the block's end, it goes before a last command that moves the track elsewhere.
void Rewrite(const Code& code, const std::vector<Command>& items, uint32_t begin, uint32_t end, BlockChange change,
             std::vector<Command>& out, Edited& edited);

// The byte in `note`, a note command of Konami's driver, that plays `key`: the semitones from the pitch of its sample,
// or the note of the driver. Throws std::invalid_argument with a message for the user where the byte can't hold it.
uint8_t KonamiKey(const Command& note, int key, ntdlib::konami::Revision revision);

// How far a note of a timeline can move past the end of a track that ends there.
inline constexpr uint32_t kFarLater = 1u << 24;

// The end of a stretch of a timeline's track, from the ticks of its blocks (BlockStarts): its last block, the tick
// where that block starts and the tick where it ends. A track that loops jumps back to the loop's start there, and one
// that ends there ends.
struct StretchEnd
{
    std::size_t block = 0;
    uint32_t start = 0;
    uint32_t tick = 0;
    bool loops = false;
    bool ends = false;
};

StretchEnd EndOf(const Code& code, const std::map<std::size_t, uint32_t>& starts);

// Where a command of a timeline is added, and the end of the track that a note then moves.
struct TimelineSpot
{
    Spot spot;

    // For a note that ends past the end of a track that ends there: the stretch's last block, and the tick from the
    // block's start where the track then ends, at the note's end.
    std::optional<std::pair<std::size_t, uint32_t>> lasts;
};

// The end of the track that ends at `end`, for a note that starts at tick `at` of the track and runs past the end, or
// nothing. The track then lasts to the note's end. Where the header doesn't give the song's end (`song_end`), the note
// stays inside the track. A note whose length the bytecode doesn't give stays too.
std::optional<std::pair<std::size_t, uint32_t>> LastsTo(const StretchEnd& end, uint32_t at, const Command& note,
                                                        const SongEnd& song_end);

// The ticks of a program's stretch, and the end that it runs into at its last command. `ticks` contains the tick of
// each item from `first` on, up to the first command whose time the bytecode doesn't specify. The rest is set where
// `ends` is: the stretch runs into an end that no comparison decides, and every command has its time. `tick` is the
// tick of the end, `block` the block that contains it, and `start` the tick of that block. `held` contains the notes
// that may sound up to the end: a note whose command lasts past it, a note of a length that the bytecode doesn't
// specify, and a tie that no end of the tie ends first. `fixed` is set where a move of the end could change notes that
// the stretch doesn't show: the stretch starts after its track opens, a note of the end's block waits for its length,
// or the track may tie its notes or play one note at a time. Such a track holds a note past the length of its command.
struct ProgramEnd
{
    bool ends = false;
    uint32_t tick = 0;
    std::size_t block = 0;
    uint32_t start = 0;
    uint32_t first = 0;
    std::vector<uint32_t> ticks;
    std::set<uint32_t> held;
    bool fixed = false;

    // The tick of item `item`, or nothing for an item outside the stretch or after the times that it contains.
    std::optional<uint32_t> TickOf(uint32_t item) const
    {
        return item >= first && item - first < ticks.size() ? std::optional(ticks[item - first]) : std::nullopt;
    }
};

// The ticks and the end of `stretch`. The ticks follow BlockStarts.
ProgramEnd ProgramEndOf(const Code& code, const song::Stretch& stretch);

// The ticks and the end of each stretch of a program (ProgramEndOf), found once for each stretch.
class ProgramEnds
{
public:
    explicit ProgramEnds(const Code& code) : code_(code)
    {
    }

    const ProgramEnd& Of(const song::Stretch& stretch)
    {
        const auto key = std::tuple(stretch.first, stretch.last, stretch.tick, stretch.note_wait, stretch.entry);
        auto found = ends_.find(key);
        if (found == ends_.end())
        {
            found = ends_.emplace(key, ProgramEndOf(code_, stretch)).first;
        }

        return found->second;
    }

private:
    const Code& code_;
    std::map<std::tuple<uint32_t, uint32_t, uint32_t, bool, ntdlib::sequence_export::Entry>, ProgramEnd> ends_;
};

// The tick where a track of `code` that ran `stretches`, in order, ends (TrackEndOf). On the GBA, a stretch that would
// start in the song's last tick belongs to the next pass, and the stretches leave it out (ntdlib's mp2k::StretchesOf).
// A track whose last stretch returns from a call to commands that end the track in the same tick (EndsAt) then ends at
// the return's tick. The calls and returns at the ends of the stretches before give the command after the call.
std::optional<uint32_t> TrackEndIn(const Code& code, std::span<const song::Stretch> stretches);

// Why a program's note can't last past the end of its track: a move of the end could change other notes.
inline constexpr std::string_view kEndRefusal =
    "The track ends before this note does. Moving the end could change other notes too. To move it anyway, lengthen "
    "the wait before the End in the List Editor.";

// Where a command of a timeline goes at `addition`'s tick: in the block of its stretch that runs at the tick (SpotOf),
// in any block of the track before the loop's end. A note past the end of a track that ends there goes after the
// stretch's last block. A note that runs past that end moves it: the track then lasts to the note's end
// (TimelineSpot::lasts). `song_end` gives the tick where the song's looping tracks have each gone back to their loop's
// start once (TimelineSongEnd). A track that ends takes no command at or after it. Where the header doesn't give that
// tick, a note of a track that ends stays inside it. Throws std::invalid_argument with a message for the user for a
// tick before the stretch's start, at or after the loop's end, past the end of a track that ends where the song's end
// is unknown, or where no block of the stretch takes the command.
TimelineSpot TimelineSpotOf(const Code& code, const Addition& addition, const SongEnd& song_end);

// Records in `blocks` the end that a note moves the block of `lasts` to (TimelineSpot::lasts). The latest end wins.
void Last(std::map<std::size_t, BlockChange>& blocks, const std::optional<std::pair<std::size_t, uint32_t>>& lasts);

// The file with `rewritten` in place of the items of sequence `sequence`. Block `i` of `code` now starts at item
// `new_starts[i]`, and the last of `new_starts` is the end of the items. The targets, the labels and the sounds' starts
// move with the blocks that they start.
format::File WithItems(const format::File& file, uint32_t sequence, const Code& code, std::vector<Command> rewritten,
                       const std::vector<uint32_t>& new_starts);

// The runs that `named` and `additions` require to turn the note wait off, by the item where each begins. They're those
// of a move, a new length and a new command on a track whose notes wait for their length. Throws std::invalid_argument
// for a note whose wait no wait of its own can take the place of, and for a note that other code may run with the note
// wait off.
std::map<uint32_t, Run> RunsToWrite(const format::File& file, uint32_t sequence, const Code& code,
                                    const std::map<uint32_t, const NoteEdit*>& named,
                                    std::span<const Addition> additions);

// The edit of EditNotes, AddSettings and MoveSettings. With `settings`, the edits name settings, and only move them.
// Unless `written`, a move, a new length or a new command on a track whose notes wait for their length first turns the
// note wait off in the run that it changes (WriteWaits).
Edited EditNotesIn(const format::File& file, uint32_t sequence, std::span<const NoteEdit> edits,
                   std::span<const Addition> additions, bool written, bool settings);

// The edit of EditNotesIn after the runs `runs` of `code`, sequence `sequence` of `file`, turn the note wait off
// (WriteWaits). The edits and the new commands follow their commands and stretches into the new version. The new index
// of each item follows both steps.
Edited EditWritten(const format::File& file, uint32_t sequence, const Code& code, const std::map<uint32_t, Run>& runs,
                   std::span<const NoteEdit> edits, std::span<const Addition> additions, bool settings);

} // namespace hokuto::edit::note_detail
