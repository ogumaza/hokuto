// SPDX-License-Identifier: MIT

// The note edits of a song of Konami's GBA driver (edit/konami.h).

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/commands/units.h"

#include "edit/bytecode.h"
#include "edit/konami.h"
#include "edit/note_blocks.h"
#include "edit/note_edits.h"
#include "edit/notes.h"
#include "song/konami.h"

namespace hokuto::edit
{

namespace
{

using note_detail::Addition;
using note_detail::BlockOf;
using note_detail::BlockStarts;
using note_detail::BlockTicks;
using note_detail::CheckedByte;
using note_detail::Code;
using note_detail::EditNotesIn;
using note_detail::EndOf;
using note_detail::Examine;
using note_detail::KonamiKey;
using note_detail::KonamiKeyEndsInBlock;
using note_detail::KonamiSilentNote;
using note_detail::KonamiStartsAlone;
using note_detail::NoteAt;
using note_detail::Spot;
using note_detail::SpotOf;
using note_detail::StretchEnd;
using ntdlib::commands::Command;
using ntdlib::commands::Kind;
using ntdlib::commands::kOne;

} // namespace

namespace note_detail
{

uint8_t KonamiKey(const Command& note, int key, ntdlib::konami::Revision revision)
{
    if (note.sample && revision == ntdlib::konami::Revision::kDungeonDiceMonsters)
    {
        throw std::invalid_argument(
            "This note plays its sample at the original pitch and has no key. Can't transpose it.");
    }
    if (!note.sample)
    {
        return static_cast<uint8_t>(CheckedByte(key, "key"));
    }
    if (key < INT8_MIN || key > INT8_MAX)
    {
        throw std::invalid_argument(
            "A sample note can be at most 128 semitones below the original pitch and 127 above it.");
    }

    return static_cast<uint8_t>(key);
}

} // namespace note_detail

namespace
{

// Whether `command` of `code`, a sequence of Konami's driver, starts a note on a track whose notes are like `note`: a
// note command, or a command that starts the track's note again (KonamiStartsNoteAgain).
bool StartsKonamiNote(const Code& code, const Command& command, const Command& note)
{
    return command.kind == Kind::kNote || KonamiStartsNoteAgain(command, note, *code.konami);
}

// A volume command that starts a note, and the silent note command that moves with it (KonamiSilentNote).
struct MovedPair
{
    uint32_t silent = 0; // the item of the note command
    uint32_t volume = 0; // the item of the volume command
    song::Stretch stretch;
    int32_t shift = 0;
};

// The edit among `edits`, or else among `silent`, that names item `item` of `code` in stretch `stretch`.
const NoteEdit* EditOf(const Code& code, std::span<const NoteEdit> edits, std::span<const NoteEdit> silent,
                       uint32_t item, const song::Stretch& stretch)
{
    const auto names_item = [&](const NoteEdit& candidate)
    {
        return candidate.command == code.offsets[item] && candidate.stretch == stretch;
    };
    const auto edit = std::ranges::find_if(edits, names_item);
    if (edit != edits.end())
    {
        return &*edit;
    }

    const auto moved = std::ranges::find_if(silent, names_item);
    return moved != silent.end() ? &*moved : nullptr;
}

// Whether a note of block `block` other than the note command `item` starts after `start` and at or before `end` once
// `edits` apply. A command that starts the track's note again counts too. `ticks` contains the ticks of the block
// (BlockTicks). A note that an edit of the same stretch removes doesn't count. A note that such an edit moves counts at
// its new tick. `silent` contains the moves of the silent note commands (TakeSilentNote).
bool NoteBetween(const Code& code, std::size_t block, const std::vector<uint32_t>& ticks,
                 std::span<const NoteEdit> edits, std::span<const NoteEdit> silent, const NoteEdit& edit, uint32_t item,
                 int64_t start, int64_t end)
{
    const std::vector<Command>& items = *code.items;
    for (uint32_t i = code.starts[block]; i < code.starts[block + 1]; ++i)
    {
        if (i == item || !StartsKonamiNote(code, items[i], items[item]))
        {
            continue;
        }

        int64_t tick = ticks[i - code.starts[block]];
        if (const NoteEdit* other = EditOf(code, edits, silent, i, edit.stretch))
        {
            if (other->remove)
            {
                continue;
            }

            tick += other->shift;
        }
        if (tick > start && tick <= end)
        {
            return true;
        }
    }

    return false;
}

// Adds to `changes` the change of the silent note command of `edit`, an edit of the note that the volume command at
// item `item` of `code` starts (KonamiSilentNote). The note command sets the note's key. A move of the note takes the
// note command along and keeps the ticks between the two commands. `pairs` gains each pair that moves. A removal
// leaves the note command: it still ends the note before it. Throws std::invalid_argument with a message for the user
// where the edit could change a note that it doesn't name:
// - any edit where the note command is in another block (kKonamiFarKey);
// - a move or a new key where the note command sets the key of another note too (KonamiStartsAlone);
// - a new key where no note command follows in the block (KonamiKeyEndsInBlock).
void TakeSilentNote(const Code& code, uint32_t item, const NoteEdit& edit, std::vector<NoteEdit>& changes,
                    std::vector<MovedPair>& pairs)
{
    const std::optional<uint32_t> silent = KonamiSilentNote(code, item);
    if (!silent)
    {
        throw std::invalid_argument(std::string(note_detail::kKonamiFarKey));
    }

    const bool moves = !edit.remove && edit.shift != 0;
    const bool transposes = !edit.remove && edit.transpose != 0;
    const bool alone = KonamiStartsAlone(code, *silent);
    if (moves && !alone)
    {
        throw std::invalid_argument(note_detail::KonamiSharedKeyRefusal("move"));
    }
    if (transposes && !alone)
    {
        throw std::invalid_argument(note_detail::KonamiSharedKeyRefusal("transpose"));
    }
    if (transposes && !KonamiKeyEndsInBlock(code, *silent))
    {
        throw std::invalid_argument(std::string(note_detail::kKonamiOpenKey));
    }
    if (!moves && !transposes)
    {
        return;
    }

    changes.push_back({.command = code.offsets[*silent],
                       .stretch = edit.stretch,
                       .shift = moves ? edit.shift : 0,
                       .transpose = transposes ? edit.transpose : 0});
    if (moves)
    {
        pairs.push_back({*silent, item, edit.stretch, edit.shift});
    }
}

// Throws std::invalid_argument with a message for the user where one of `pairs` would move past another command of
// its block once `changes` apply:
// - a command that starts a note and came before the pair's note command would run at its tick or after it;
// - a command that starts a note and came after the pair's volume command would run at its tick or before it;
// - a rest that came before the volume command would run after it.
// The volume command would then start another key, another volume command would start the pair's key, or the rest
// would end the moved note. A command that moves by the same ticks keeps its place.
void CheckMovedPairs(const Code& code, std::span<const NoteEdit> changes, std::span<const MovedPair> pairs)
{
    const std::vector<Command>& items = *code.items;
    for (const MovedPair& pair : pairs)
    {
        const std::size_t block = BlockOf(code, pair.volume);
        const std::vector<uint32_t> ticks = BlockTicks(code, block);
        const int64_t from = int64_t{ticks[pair.silent - code.starts[block]]} + pair.shift;
        const int64_t to = int64_t{ticks[pair.volume - code.starts[block]]} + pair.shift;
        for (uint32_t i = code.starts[block]; i < code.starts[block + 1]; ++i)
        {
            const bool starts = StartsKonamiNote(code, items[i], items[pair.volume]);
            const bool rest = items[i].kind == Kind::kNoteOff && i < pair.volume;
            const NoteEdit* other = EditOf(code, changes, {}, i, pair.stretch);
            if (i == pair.silent || i == pair.volume || (!starts && !rest) ||
                (other && (other->remove || other->shift == pair.shift)))
            {
                continue;
            }

            const int64_t tick = int64_t{ticks[i - code.starts[block]]} + (other ? other->shift : 0);
            const bool passed = rest ? tick > to : i < pair.silent ? tick >= from : tick <= to;
            if (passed)
            {
                throw std::invalid_argument(
                    "Can't move this note past another note or a rest on the track. Move or remove that one first.");
            }
        }
    }
}

// Adds the new notes of `additions` to `added`, in their order, and to `rests` the rest after each note that no other
// note or rest cuts first (EditKonamiNotes). `rest` is the command of a rest.
void PlaceAdditions(const Code& code, const Command& rest, std::span<const NewNote> additions,
                    std::vector<Addition>& added, std::vector<Addition>& rests)
{
    const std::vector<Command>& items = *code.items;

    // The ticks of the new notes, by their stretches. Like a rest, a note cuts the note before it.
    std::map<uint32_t, std::set<uint32_t>> new_ticks;
    for (const NewNote& addition : additions)
    {
        new_ticks[addition.stretch.first].insert(addition.tick);
    }

    for (const NewNote& addition : additions)
    {
        Command note;
        note.kind = Kind::kNote;
        note.sample = addition.sample;
        note.index = addition.index;
        note.key = *code.konami == ntdlib::konami::Revision::kDungeonDiceMonsters && note.sample
                       ? uint8_t{0}
                       : KonamiKey(note, addition.key, *code.konami);
        note.velocity = int64_t{song::KonamiVolumeOf(CheckedByte(addition.velocity, "velocity"), *code.konami,
                                                     note.sample.has_value())} *
                        kOne;
        added.push_back({addition.stretch, addition.tick, note});

        const std::optional<Spot> spot = SpotOf(code, added.back());
        if (!spot)
        {
            continue;
        }

        const std::vector<uint32_t> ticks = BlockTicks(code, spot->block);
        const uint32_t end = spot->tick + addition.length;
        const std::set<uint32_t>& others = new_ticks.at(addition.stretch.first);
        const auto next_new = others.upper_bound(addition.tick);
        bool cut = end > ticks.back() || (next_new != others.end() && *next_new <= addition.tick + addition.length);
        for (uint32_t i = code.starts[spot->block]; i < code.starts[spot->block + 1] && !cut; ++i)
        {
            const uint32_t tick = ticks[i - code.starts[spot->block]];
            cut = (StartsKonamiNote(code, items[i], note) || items[i].kind == Kind::kNoteOff) && tick > spot->tick &&
                  tick <= end;
        }

        // A rest at the tick where the block ends is placed in a later block, behind the commands of that tick
        // (SpotOf). A command there that starts a note cuts the new note first, and the rest would end that command's
        // note.
        const Addition after{addition.stretch, addition.tick + addition.length, rest};
        const std::optional<Spot> rest_spot = cut ? std::nullopt : SpotOf(code, after);
        for (std::size_t block = spot->block + 1; rest_spot && block <= rest_spot->block && !cut; ++block)
        {
            const std::vector<uint32_t> block_ticks = BlockTicks(code, block);
            for (uint32_t i = code.starts[block]; i < code.starts[block + 1] && !cut; ++i)
            {
                cut = (StartsKonamiNote(code, items[i], note) || items[i].kind == Kind::kNoteOff) &&
                      (block < rest_spot->block || block_ticks[i - code.starts[block]] <= rest_spot->tick);
            }
        }
        if (!cut)
        {
            rests.push_back(after);
        }
    }
}

// The first item of block `block` of `code` after item `from` that starts a note like the note command `item`, or with
// `notes_only` false that ends one: a note-off (EditKonamiNote). Nothing where the block has none.
std::optional<uint32_t> NextAfter(const Code& code, std::size_t block, uint32_t item, uint32_t from, bool notes_only)
{
    const std::vector<Command>& items = *code.items;
    for (uint32_t i = from + 1; i < code.starts[block + 1]; ++i)
    {
        if (StartsKonamiNote(code, items[i], items[item]) || (!notes_only && items[i].kind == Kind::kNoteOff))
        {
            return i;
        }
    }

    return std::nullopt;
}

// The item of the volume command that `edit` names as its note in `code`: the command starts the note
// (KonamiSilentNote). Nothing for an edit that names another command.
std::optional<uint32_t> VolumeStartOf(const Code& code, const NoteEdit& edit)
{
    const std::optional<uint32_t> item = ItemAtOffset(code.offsets, edit.command);
    const bool volume = item && (*code.items)[*item].kind == Kind::kVolume && (*code.items)[*item].index == 0;
    return volume ? item : std::nullopt;
}

// Adds to `rests` a rest `rest` at tick `end` of block `block` of `code`, in the stretch of `edit` (EditKonamiNote).
// Throws std::invalid_argument with a message for the user where the stretch doesn't run the block.
void AddRest(const Code& code, const NoteEdit& edit, std::size_t block, int64_t end, const Command& rest,
             std::vector<Addition>& rests)
{
    const std::map<std::size_t, uint32_t> starts = BlockStarts(code, edit.stretch);
    const auto block_start = starts.find(block);
    if (block_start == starts.end())
    {
        throw std::invalid_argument("Can't find the command of this note in the sequence.");
    }

    rests.push_back({edit.stretch, static_cast<uint32_t>(block_start->second + end), rest});
}

// Adds the change of `edit`, one of `edits`, to `changes`: the note's change without its length, then the change of
// its rest. `rests` gains a new rest that the note's new length takes (EditKonamiNotes). `rest` is the command of a
// rest. `silent` contains the moves of the silent note commands (TakeSilentNote). Throws std::invalid_argument with a
// message for the user for a length past the next note or the end of the note's part of the track.
void EditKonamiNote(const Code& code, const Command& rest, std::span<const NoteEdit> edits,
                    std::span<const NoteEdit> silent, const NoteEdit& edit, std::vector<NoteEdit>& changes,
                    std::vector<Addition>& rests)
{
    const std::vector<Command>& items = *code.items;

    // The note command before a volume command that starts the note sets the key (TakeSilentNote).
    const std::optional<uint32_t> volume = VolumeStartOf(code, edit);
    NoteEdit change = edit;
    change.length.reset();
    change.transpose = volume ? 0 : change.transpose;
    changes.push_back(change);
    if (edit.remove || (!edit.length && edit.shift == 0))
    {
        return;
    }

    const uint32_t item = volume ? *volume : NoteAt(code, edit.command);
    const std::size_t block = BlockOf(code, item);
    const std::vector<uint32_t> ticks = BlockTicks(code, block);
    const auto tick_of = [&](uint32_t i)
    {
        return int64_t{ticks[i - code.starts[block]]};
    };

    const int64_t last_tick = ticks.back();
    const int64_t start = tick_of(item) + edit.shift;
    const std::optional<uint32_t> next = NextAfter(code, block, item, item, false);
    if (next && items[*next].kind == Kind::kNoteOff)
    {
        const std::optional<uint32_t> after = NextAfter(code, block, item, *next, true);
        if (edit.length && start + *edit.length > (after ? tick_of(*after) : last_tick))
        {
            throw std::invalid_argument(after ? "A note can't last past the next note on the track. Move or remove "
                                                "the next note first."
                                              : "A note can't last past the end of its section of the track.");
        }

        const int64_t to = edit.length ? start + *edit.length : std::min(tick_of(*next) + edit.shift, last_tick);
        if (!edit.length && NoteBetween(code, block, ticks, edits, silent, edit, item, start, to))
        {
            // Another note cuts the moved note before its rest would. The rest would then cut that note short.
            changes.push_back({.command = code.offsets[*next], .stretch = edit.stretch, .remove = true});
            return;
        }

        if (to != tick_of(*next))
        {
            changes.push_back({.command = code.offsets[*next],
                               .stretch = edit.stretch,
                               .shift = static_cast<int32_t>(to - tick_of(*next))});
        }

        return;
    }

    if (!edit.length)
    {
        return;
    }

    const int64_t end = start + *edit.length;
    const int64_t limit = next ? tick_of(*next) : last_tick;
    if (end > limit)
    {
        throw std::invalid_argument(next ? "A note can't last past the next note on the track. Move or remove the "
                                           "next note first."
                                         : "A note can't last past the end of its section of the track.");
    }

    if (end < limit)
    {
        AddRest(code, edit, block, end, rest, rests);
    }
}

// Why an edit of Konami's driver is refused: a volume command would start the note of another note command again
// (CheckVolumeStarts).
constexpr std::string_view kAddChangesNote = "Can't add a note there. It would change another note on the track.";
constexpr std::string_view kMoveChangesNote = "Can't move this note there. It would change another note on the track.";
constexpr std::string_view kRemoveChangesNote =
    "Can't remove this note. A volume after it starts it again. Remove that volume first.";
constexpr std::string_view kVolumeChangesNote = "Can't move this volume there. It would start another note again.";

// Why an edit of a song is refused: the check can't follow one of its tracks (WalkSong). Each text names what the
// walk met on the track.
constexpr std::string_view kCallInCall =
    "Can't edit this song here. One of its tracks calls inside a call. Edit its commands in the List Editor.";
constexpr std::string_view kCallOutside =
    "Can't edit this song here. One of its tracks calls commands outside the song. Edit its commands in the List "
    "Editor.";
constexpr std::string_view kLoopInCall =
    "Can't edit this song here. One of its tracks starts a loop inside a call. Edit its commands in the List Editor.";
constexpr std::string_view kRunsIntoBytes =
    "Can't edit this song here. One of its tracks runs into bytes that aren't "
    "commands. Edit its commands in the List Editor.";
constexpr std::string_view kNoEnd =
    "Can't edit this song here. One of its tracks has no end. Edit its commands in the List Editor.";

// Why an edit of a song is refused whose loop cuts a track short (WalkSong).
constexpr std::string_view kLoopCutsTrack =
    "Can't edit this song here. The song loops before one of its tracks has "
    "played all its notes. Edit its commands in the List Editor.";

// Why an edit is refused that makes a loop of the song cut a track short, on a song whose loops cut no track before
// the edit (WalkSong). The first two are for a note command at the time of the song's first loop or later. The note
// would never play. The last two are for a note command that only the later loops cut. The note would play once,
// before the first loop, and never after it. Only a note command reaches these texts. No edit of notes or settings
// adds or moves a loop point (WhyFixedInTime in notes.cpp).
constexpr std::string_view kAddAfterLoop = "Can't add a note there. The song loops before it would play.";
constexpr std::string_view kMoveAfterLoop = "Can't move this note there. The song loops before it would play.";
constexpr std::string_view kAddInLaterLoop = "Can't add a note there. It wouldn't play after the song loops.";
constexpr std::string_view kMoveInLaterLoop = "Can't move this note there. It wouldn't play after the song loops.";

// Why an edit is refused that doesn't say where each command or each track's start went. No edit of notes or settings
// does that.
constexpr std::string_view kUnmapped = "Can't make this edit here. Edit the song's commands in the List Editor.";

// The commands that a track runs, in the driver's order: runs of items (song::KonamiRun). The first runs are the
// track's way from its start to its end or to the song's loop. Where the loop starts the track again, the runs of a
// second pass follow.
struct TrackWalk
{
    std::vector<song::KonamiRun> runs;
    std::optional<uint32_t> loop_point; // the item after the last loop point that the track passed
    bool ended = false;
    bool restarts = false;                   // the track's end leaves it ready for a loop of the song
    std::optional<std::string_view> stopped; // why the walk doesn't follow the track, as a message for the user

    // The times of the first pass, in the unit of the waits: the time that the pass took, the time of its last note
    // command or loop point, and the time at which it reached a loop of the song.
    bool first_pass = true;
    int64_t time = 0;
    std::optional<int64_t> last_state;
    uint32_t last_state_item = 0; // the item of that note command or loop point
    int64_t loop_time = 0;        // the time of `loop_point`, or 0 at the track's start
    std::optional<int64_t> loops_at;
};

// Adds to `walk` the runs of the items of `items` from `first` up to `end`, in the order in which a track of Konami's
// driver runs them. A call plays the commands of its range and returns (`ranges`). An end or a loop of the song ends
// the walk. A loop point moves the place where the track starts again. `called` is set inside a call. Returns false,
// with the reason in `walk.stopped`, for a way that the walk doesn't follow: a call inside a call, a call without a
// range, a loop point inside a call, and bytes that aren't commands. The driver keeps one return address, and a call
// inside a call replaces it.
bool WalkRange(const std::vector<Command>& items, std::span<const KonamiCallRange> ranges, uint32_t first, uint32_t end,
               bool called, TrackWalk& walk)
{
    uint32_t start = first;
    for (uint32_t item = first; item < end && item < items.size(); ++item)
    {
        const Command& command = items[item];
        if (command.kind == Kind::kData || (called && command.kind == Kind::kLoopStart))
        {
            walk.stopped = command.kind == Kind::kData ? kRunsIntoBytes : kLoopInCall;
            return false;
        }

        if (walk.first_pass)
        {
            walk.time += command.kind == Kind::kWait ? command.value.value : 0;
            if (command.kind == Kind::kNote || command.kind == Kind::kLoopStart)
            {
                walk.last_state = walk.time;
                walk.last_state_item = item;
            }
        }

        if (command.kind == Kind::kLoopStart)
        {
            walk.loop_point = item + 1;
            walk.loop_time = walk.first_pass ? walk.time : walk.loop_time;
        }
        else if (command.kind == Kind::kCall)
        {
            const auto range =
                std::ranges::find_if(ranges, [item](const KonamiCallRange& each) { return each.call == item; });
            if (called || range == ranges.end())
            {
                walk.stopped = called ? kCallInCall : kCallOutside;
                return false;
            }

            walk.runs.emplace_back(start, item + 1);
            if (!WalkRange(items, ranges, range->first, range->end, true, walk))
            {
                return false;
            }
            if (walk.ended)
            {
                return true;
            }

            start = item + 1;
        }
        else if (command.kind == Kind::kEnd || command.kind == Kind::kLoopEnd)
        {
            // An end with index 0 leaves the track ready for the song's loop. The others end it for good.
            walk.runs.emplace_back(start, item + 1);
            walk.ended = true;
            walk.restarts = command.kind == Kind::kLoopEnd || command.index == 0;
            walk.loops_at =
                walk.first_pass && command.kind == Kind::kLoopEnd ? std::optional(walk.time) : walk.loops_at;
            return true;
        }
    }

    walk.runs.emplace_back(start, end);
    return true;
}

// One pass of the track over `items` from item `from`, added to `walk` (WalkRange). `walk.stopped` is set for a way
// that the walk doesn't follow, and for a track that runs past the last command.
void WalkPass(const std::vector<Command>& items, std::span<const KonamiCallRange> ranges, uint32_t from,
              TrackWalk& walk)
{
    walk.ended = false;
    if (WalkRange(items, ranges, from, static_cast<uint32_t>(items.size()), false, walk) && !walk.ended)
    {
        walk.stopped = kNoEnd;
    }
}

// The walks of the tracks of a song, by the item where each track starts, and why the check doesn't follow the song,
// as a message for the user. `apart` is set where a track that the loop starts again ends at another time than the
// song loops.
struct SongWalk
{
    std::map<uint32_t, TrackWalk> tracks;
    std::optional<std::string_view> stopped;
    uint32_t cut_item = 0;  // for kLoopCutsTrack: the last note command or loop point of the track that the loop cuts
    bool later_cut = false; // for kLoopCutsTrack: the first loop cuts no track, and a later one does
    bool apart = false;
};

// The walk of the tracks that start at `entries` of `items`: the first pass of each and, where the song loops, a
// second pass.
// The first track that reaches a loop of the song starts every track again that hasn't ended for good. Each starts at
// the last loop point that it passed, or at its start, and keeps its key (ntdlib src/konami/sequencer.cpp, Step,
// ResetTracks and Op::kLoopPoint). A volume command at the top of the loop then starts the key of the last note command
// of the first pass. A third pass starts as the second one does.
// A loop cuts a longer track short. The walk follows each pass of a track to the track's end. That is right while the
// loop cuts the track after its last note command and loop point: its key and its loop point are then the ones that
// the driver has at the cut. The first loop comes at the earliest time at which a track reaches a loop of the song.
// Each later loop comes one loop length after the loop before it. The loop length is the shortest time from a track's
// loop point to its loop of the song. In a song with an intro, a track without a loop point starts every later pass
// at its start, and the later loops cut it earlier than the first one did. A song with a track that runs a note
// command or a loop point at the time of a cut or later is not followed (kLoopCutsTrack): the driver starts the next
// pass with another key or at another place than the walk would. The tracks of every song that the sequence contains
// count together here. The reader shows a song until the state of its tracks repeats (song::ReadSequence). That is the
// first pass, and the second pass too where a track starts it with another key. The walk checks both passes, whatever
// the reader shows.
SongWalk WalkSong(const std::vector<Command>& items, std::span<const KonamiCallRange> ranges,
                  std::span<const std::optional<uint32_t>> entries)
{
    SongWalk song;
    std::optional<int64_t> loop;
    for (const std::optional<uint32_t>& entry : entries)
    {
        if (!entry || song.tracks.contains(*entry))
        {
            continue;
        }

        TrackWalk& walk = song.tracks[*entry];
        WalkPass(items, ranges, *entry, walk);
        if (walk.stopped)
        {
            song.stopped = walk.stopped;
            return song;
        }

        walk.first_pass = false;
        loop = walk.loops_at && (!loop || *walk.loops_at < *loop) ? walk.loops_at : loop;
    }

    if (!loop)
    {
        return song;
    }

    // A track that ended for good before the loop stays ended.
    const auto restarted = [&loop](const TrackWalk& walk)
    {
        return walk.restarts || walk.time >= *loop;
    };
    int64_t length = *loop;
    for (const auto& [entry, walk] : song.tracks)
    {
        length = walk.loops_at ? std::min(length, *walk.loops_at - walk.loop_time) : length;
    }

    // The first loop cuts a track at the time `loop`, and each later one a loop length after the track's loop point.
    for (const bool later : {false, true})
    {
        for (const auto& [entry, walk] : song.tracks)
        {
            const int64_t cut = later ? walk.loop_time + length : *loop;
            if (restarted(walk) && walk.last_state && *walk.last_state >= cut && (!later || length > 0))
            {
                song.stopped = kLoopCutsTrack;
                song.cut_item = walk.last_state_item;
                song.later_cut = later;
                return song;
            }
        }
    }

    for (auto& [entry, walk] : song.tracks)
    {
        if (!restarted(walk))
        {
            continue;
        }

        song.apart = song.apart || walk.time != *loop;
        WalkPass(items, ranges, walk.loop_point.value_or(entry), walk);
        if (walk.stopped)
        {
            song.stopped = walk.stopped;
            return song;
        }
    }

    return song;
}

// The items where the tracks of sequence `sequence` of `file` start, by the fields of the song table. A field that
// names no item of the sequence has nothing.
std::vector<std::optional<uint32_t>> TrackEntries(const format::File& file, uint32_t sequence)
{
    std::vector<std::optional<uint32_t>> entries;
    for (const ntdlib::sar::Field& field : file.native->fields)
    {
        entries.push_back(NamesItemOf(*file.native, field.to, sequence) ? std::optional(field.to.offset)
                                                                        : std::nullopt);
    }

    return entries;
}

// Whether the edit `edited` of sequence `sequence` of `before` moved the volume command at item `item`. The command
// is item `now` after the edit. A moved command runs at another tick of its block, or in another block.
bool VolumeMoved(const format::File& before, const Edited& edited, uint32_t sequence, uint32_t item, uint32_t now)
{
    const Code was = Examine(before, sequence);
    const Code is = Examine(edited.file, sequence);
    const std::size_t block = BlockOf(was, item);
    return is.starts.size() != was.starts.size() || BlockOf(is, now) != block ||
           BlockTicks(was, block)[item - was.starts[block]] != BlockTicks(is, block)[now - is.starts[block]];
}

// The note commands of each time that a track runs each volume command (C0-CF) of `items` along `walk`, by the volume
// command's item and in the order of the walk (song::KonamiVolumeStartsAlong).
using VolumeTimes = std::map<uint32_t, std::vector<std::array<std::optional<uint32_t>, 2>>>;

VolumeTimes VolumeTimesOf(const std::vector<Command>& items, const TrackWalk& walk, ntdlib::konami::Revision revision)
{
    VolumeTimes times;
    for (const song::KonamiRunStart& start : song::KonamiVolumeStartsAlong(items, walk.runs, revision))
    {
        times[start.volume].push_back(start.notes);
    }

    return times;
}

// Whether `command` decides which note command a track runs before a volume command: a note command, a volume command
// that starts notes, a call, a loop point, a loop of the song or an end.
bool OrdersVolumeStarts(const Command& command)
{
    return command.kind == Kind::kNote || (command.kind == Kind::kVolume && command.index == 0) ||
           command.kind == Kind::kCall || command.kind == Kind::kLoopStart || command.kind == Kind::kLoopEnd ||
           command.kind == Kind::kEnd;
}

// Whether the edit `edited` keeps every command in its order, and adds and removes none of those that order the volume
// starts (OrdersVolumeStarts). `was` are the items before the edit and `now` the items after it. `from` maps each item
// after the edit to its item before the edit. Such an edit changes values and lengths, and every volume command follows
// the note command that it followed before.
bool KeepsCommandOrder(const std::vector<Command>& was, const std::vector<Command>& now, const Edited& edited,
                       const std::vector<std::optional<uint32_t>>& from)
{
    std::optional<uint32_t> last;
    for (uint32_t i = 0; i < was.size(); ++i)
    {
        const std::optional<uint32_t> place = edited.items[i];
        if (place ? (last && *place <= *last) || *place >= now.size() || now[*place].kind != was[i].kind ||
                        now[*place].index != was[i].index
                  : OrdersVolumeStarts(was[i]))
        {
            return false;
        }

        last = place ? place : last;
    }

    for (uint32_t i = 0; i < now.size(); ++i)
    {
        if (!from[i] && OrdersVolumeStarts(now[i]))
        {
            return false;
        }
    }

    return true;
}

// The waits of `items` between the commands that order the volume starts (OrdersVolumeStarts), in the order of the
// items: the sum before each such command, and the sum after the last one. Two lists of items with those commands in
// the same order and with the same sums run each of them at the same time.
std::vector<int64_t> WaitsBetween(const std::vector<Command>& items)
{
    std::vector<int64_t> waits = {0};
    for (const Command& command : items)
    {
        if (OrdersVolumeStarts(command))
        {
            waits.push_back(0);
        }

        waits.back() += command.kind == Kind::kWait ? command.value.value : 0;
    }

    return waits;
}

// Checks `edited`, an edit of sequence `sequence` of `before`: every volume command that starts a note again still
// starts it at the key that it had before the edit. The volume command starts the note at the key of the last note
// command that its track ran before it (song::KonamiNoteCommandBefore, the rule of the reader). That command has to be
// the same command as before. Another command of the same key and sample doesn't pass: the note of the volume command
// would name that command and show its velocity, and the edit didn't name that note. A command whose key the edit
// changed is still the same command: the notes that it starts change with it. Throws std::invalid_argument with a
// message for the user otherwise, by the edit that put another command there: a new note, a removal, a move of a note
// or a move of the volume command.
// A volume command of volume 0 starts its channel at no level, and the reader shows no note for it. The check leaves
// it out: a new note before it is cut there, and no other note changes.
// A song without a volume command after the edit has nothing to check. Neither has an edit that keeps the commands in
// their order, adds or removes no note command and no volume command (KeepsCommandOrder) and keeps the time of each of
// them (WaitsBetween): a new velocity, a new length, a new key or a setting's new value. A move of a note that keeps
// the order is still walked. The song's loop can cut the track before the note's new time (WalkSong).
// For any other edit, the check walks each track's commands in the driver's order, before and after the edit
// (WalkSong), and takes the rule's note command for each time that the track runs a volume command
// (song::KonamiVolumeStartsAlong). Commands that two tracks run count for each of them. Such an edit of a song with a
// track that the walk doesn't follow is refused. So is an edit that doesn't say where each command went. An edit after
// which a track runs a volume command more or fewer times is refused as a move.
void CheckVolumeStarts(const format::File& before, const Edited& edited, uint32_t sequence)
{
    const ntdlib::commands::Sequence& old = *before.sequences[sequence];
    const ntdlib::commands::Sequence& changed = *edited.file.sequences[sequence];
    const std::vector<Command>& was = old.items;
    const std::vector<Command>& now = changed.items;
    const auto starts_notes = [](const Command& command)
    {
        return command.kind == Kind::kVolume && command.index == 0;
    };
    if (std::ranges::none_of(now, starts_notes))
    {
        return;
    }

    if (edited.items.size() != was.size())
    {
        throw std::invalid_argument(std::string(kUnmapped));
    }

    // The item before the edit of each item after it. A new command has none.
    const ntdlib::konami::Revision revision = *KonamiOf(ConsoleOf(before));
    std::vector<std::optional<uint32_t>> from(now.size());
    for (uint32_t i = 0; i < edited.items.size(); ++i)
    {
        if (edited.items[i] && *edited.items[i] < from.size())
        {
            from[*edited.items[i]] = i;
        }
    }

    if (KeepsCommandOrder(was, now, edited, from) && WaitsBetween(was) == WaitsBetween(now))
    {
        return;
    }

    const std::vector<std::optional<uint32_t>> entries = TrackEntries(before, sequence);
    const std::vector<std::optional<uint32_t>> new_entries = TrackEntries(edited.file, sequence);
    if (new_entries.size() != entries.size())
    {
        throw std::invalid_argument(std::string(kUnmapped));
    }

    // Refuses the edit that took the volume command at item `volume` from the note command `old_command` to
    // `new_command`. A new note in front of a call's commands shortens their first wait. The volume command then runs
    // at another tick of its block, and the new note is still the cause.
    const auto refuse = [&](uint32_t volume, std::optional<uint32_t> old_command, std::optional<uint32_t> new_command)
    {
        if (new_command && !from[*new_command])
        {
            throw std::invalid_argument(std::string(kAddChangesNote));
        }
        if (old_command && !edited.items[*old_command])
        {
            throw std::invalid_argument(std::string(kRemoveChangesNote));
        }
        if (VolumeMoved(before, edited, sequence, *from[volume], volume))
        {
            throw std::invalid_argument(std::string(kVolumeChangesNote));
        }

        throw std::invalid_argument(std::string(kMoveChangesNote));
    };
    const auto same = [&](std::optional<uint32_t> old_command, std::optional<uint32_t> new_command)
    {
        return old_command && new_command ? from[*new_command] == old_command : !old_command && !new_command;
    };

    const std::vector<KonamiCallRange> ranges = KonamiCallRanges(old);
    const std::vector<KonamiCallRange> new_ranges = KonamiCallRangesAfter(old, changed, edited);
    const SongWalk old_song = WalkSong(was, ranges, entries);
    const SongWalk new_song = WalkSong(now, new_ranges, new_entries);
    if (old_song.stopped)
    {
        throw std::invalid_argument(std::string(*old_song.stopped));
    }
    if (new_song.stopped)
    {
        // The song is followed as it is, and the edit would make its loop cut a track: the text is about the edit.
        const bool cuts = *new_song.stopped == kLoopCutsTrack;
        const bool added = cuts && now[new_song.cut_item].kind == Kind::kNote && !from[new_song.cut_item];
        const std::string_view add = new_song.later_cut ? kAddInLaterLoop : kAddAfterLoop;
        const std::string_view move = new_song.later_cut ? kMoveInLaterLoop : kMoveAfterLoop;
        throw std::invalid_argument(std::string(!cuts ? *new_song.stopped : added ? add : move));
    }

    std::set<std::pair<uint32_t, uint32_t>> walked;
    for (std::size_t field = 0; field < entries.size(); ++field)
    {
        if (!entries[field] && !new_entries[field])
        {
            continue;
        }
        if (!entries[field] || !new_entries[field])
        {
            throw std::invalid_argument(std::string(kUnmapped));
        }
        if (!walked.emplace(*entries[field], *new_entries[field]).second)
        {
            continue;
        }

        const TrackWalk& old_walk = old_song.tracks.at(*entries[field]);
        const TrackWalk& new_walk = new_song.tracks.at(*new_entries[field]);

        // The track can run a volume command more than once: in a call's commands, and on the second pass. Each time
        // after the edit is compared with the same time before it. An edit that moves a volume command into a call's
        // commands or out of them changes how often the command runs, and how often its note starts.
        const VolumeTimes old_times = VolumeTimesOf(was, old_walk, revision);
        for (const auto& [volume, new_notes] : VolumeTimesOf(now, new_walk, revision))
        {
            // A volume command of volume 0 starts no note that sounds, before and after the edit.
            if (!from[volume] || (now[volume].value.value == 0 && was[*from[volume]].value.value == 0))
            {
                continue;
            }

            const auto old_notes = old_times.find(*from[volume]);
            if (old_notes == old_times.end() || old_notes->second.size() != new_notes.size())
            {
                throw std::invalid_argument(std::string(VolumeMoved(before, edited, sequence, *from[volume], volume)
                                                            ? kVolumeChangesNote
                                                            : kMoveChangesNote));
            }

            for (std::size_t time = 0; time < new_notes.size(); ++time)
            {
                // A note command of the track, or of the track before it, can set the key (song::KonamiNoteStartsOn).
                for (const std::size_t number : {0U, 1U})
                {
                    const std::optional<uint32_t> old_command = old_notes->second[time][number];
                    const std::optional<uint32_t> new_command = new_notes[time][number];
                    if ((old_command || new_command) && !same(old_command, new_command) &&
                        KonamiStartsNoteAgain(now[volume], new_command ? now[*new_command] : was[*old_command],
                                              revision))
                    {
                        refuse(volume, old_command, new_command);
                    }
                }
            }
        }
    }
}

} // namespace

void CheckKonamiVolumeStarts(const format::File& before, const Edited& edited, uint32_t sequence)
{
    CheckVolumeStarts(before, edited, sequence);
}

KonamiWalked KonamiWalkOf(const format::File& file, uint32_t sequence)
{
    const Code code = Examine(file, sequence);
    const std::vector<Command>& items = *code.items;
    const std::vector<KonamiCallRange> ranges = KonamiCallRanges(*file.sequences[sequence]);
    const SongWalk song = WalkSong(items, ranges, TrackEntries(file, sequence));
    KonamiWalked walked;
    walked.ends_apart = song.apart;
    if (song.stopped)
    {
        walked.stopped = std::string(*song.stopped);
        walked.loop_cuts = *song.stopped == kLoopCutsTrack;
        walked.later_loop_cuts = song.later_cut;
        return walked;
    }

    std::set<song::KonamiVolumeStart> starts;
    for (const auto& [entry, walk] : song.tracks)
    {
        for (const song::KonamiRunStart& start : song::KonamiVolumeStartsAlong(items, walk.runs, *code.konami))
        {
            for (const std::size_t number : {0U, 1U})
            {
                const std::optional<uint32_t> note = start.notes[number];
                starts.insert({code.offsets[start.volume], note ? std::optional(code.offsets[*note]) : std::nullopt,
                               number == 1});
            }
        }
    }

    walked.starts.assign(starts.begin(), starts.end());
    return walked;
}

// `edits` and `additions` as Konami's driver stores them, then EditNotesIn. The command of a note has no length: the
// note sounds until the track's next note or rest. A command that starts the track's note again, such as a volume on a
// PSG track, counts as a note here. A new length moves the rest after the note, or puts a new one in before the next
// note. It can't take the note past the next note, or past its part of the track's commands. A moved note's rest moves
// with it. The rest is placed where another note would start before it. A new note takes a rest after its length,
// unless a note or a rest comes sooner. A PSG note command at volume 0 starts no note. A volume command after it starts
// one, and the note names the volume command (ntdlib src/konami/sequencer.cpp and sequencer_wct2004.cpp, Op::kPsgNote
// and Op::kVolume). A new velocity of such a note changes the volume command, and a new key the note command. A move
// moves both commands. A removal takes the volume command out. A new length moves or adds the rest after the volume
// command. An edit that could change a note that it doesn't name is refused (TakeSilentNote, CheckMovedPairs).
Edited EditKonamiNotes(const format::File& file, uint32_t sequence, std::span<const NoteEdit> edits,
                       std::span<const NewNote> additions)
{
    const Code code = Examine(file, sequence);
    Command rest;
    rest.kind = Kind::kNoteOff;

    // The new notes come first: Edited::added then follows `additions`.
    std::vector<Addition> added;
    std::vector<Addition> rests;
    PlaceAdditions(code, rest, additions, added, rests);

    // The silent note commands first: the rests of the other notes follow where those commands move.
    std::vector<NoteEdit> changes;
    std::vector<MovedPair> pairs;
    for (const NoteEdit& edit : edits)
    {
        if (const std::optional<uint32_t> volume = VolumeStartOf(code, edit))
        {
            TakeSilentNote(code, *volume, edit, changes, pairs);
        }
    }

    const std::vector<NoteEdit> silent = changes;
    for (const NoteEdit& edit : edits)
    {
        EditKonamiNote(code, rest, edits, silent, edit, changes, rests);
    }

    CheckMovedPairs(code, changes, pairs);
    added.insert(added.end(), rests.begin(), rests.end());

    return EditNotesIn(file, sequence, changes, added, false, false);
}

NewNote NewKonamiNote(const format::File& file, uint32_t sequence, const song::Track& track,
                      const song::Stretch& stretch, uint32_t tick, int key, int velocity, uint32_t length,
                      std::optional<KonamiSampleNote> copied)
{
    // A track without notes whose commands end where they start has no place for a note. Other empty tracks of the song
    // often run the same commands: a wait of 0 and an end.
    if (track.notes.empty())
    {
        const Code empty = Examine(file, sequence);
        const std::map<std::size_t, uint32_t> starts = BlockStarts(empty, stretch);
        const std::optional<StretchEnd> end = starts.empty() ? std::nullopt : std::optional(EndOf(empty, starts));
        if (end && end->ends && end->tick <= stretch.tick && tick >= end->tick)
        {
            throw std::invalid_argument(std::string(kKonamiNoRoom));
        }
    }

    if (copied && !ntdlib::konami::IsPsgTrack(track.number))
    {
        return {stretch, tick, copied->semitones, velocity, length, copied->sample, 0};
    }

    const Code code = Examine(file, sequence);

    // The note before it on the track's commands, or else the next one.
    const song::Note* like = nullptr;
    for (const song::Note& note : track.notes)
    {
        if (note.command == song::kNoCommand || note.command_track >= 0)
        {
            continue;
        }
        if (note.start > tick && like != nullptr)
        {
            break;
        }

        like = &note;
    }

    if (like != nullptr)
    {
        // A note that a volume command starts takes its key from the note command before it (KonamiSilentNote).
        const std::optional<uint32_t> named = ItemAtOffset(code.offsets, like->command);
        const std::optional<uint32_t> silent = named ? KonamiSilentNote(code, *named) : std::nullopt;
        const std::optional<uint32_t> item = silent ? silent : named;
        if (item && (*code.items)[*item].kind == Kind::kNote)
        {
            const Command& command = (*code.items)[*item];
            const int base = command.sample ? static_cast<int8_t>(command.key) : command.key;
            return {stretch, tick, base + key - like->key, velocity, length, command.sample, command.index};
        }
    }

    // A PSG track without notes plays note 0 at C2, MIDI's key 36, and a sample track its first sample at its pitch.
    if (ntdlib::konami::IsPsgTrack(track.number))
    {
        return {stretch, tick, std::max(key - kKonamiLowestPsgKey, 0), velocity, length, std::nullopt, 0};
    }

    return {stretch, tick, key - kKonamiSampleKey, velocity, length, uint16_t{0}, 0};
}

} // namespace hokuto::edit
