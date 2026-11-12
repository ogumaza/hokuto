// SPDX-License-Identifier: MIT

// The note edits of edit/notes.h: the edits of notes and settings, and the limits of a drag. The blocks are in
// note_blocks.cpp, their rewrite in note_rewrite.cpp, the runs that turn the note wait off in note_wait_edits.cpp, and
// the rules of Konami's driver in konami_notes.cpp.

#include "edit/notes.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/commands/units.h"
#include "ntdlib/sar/layout.h"

#include "edit/bytecode.h"
#include "edit/drivers.h"
#include "edit/engine_edits.h"
#include "edit/mp2k.h"
#include "edit/note_blocks.h"
#include "edit/note_edits.h"
#include "edit/timelines.h"
#include "song/archive.h"

namespace hokuto::edit
{

namespace
{

using note_detail::Addition;
using note_detail::BlockChange;
using note_detail::BlockOf;
using note_detail::BlockStarts;
using note_detail::BlockTicks;
using note_detail::CheckedByte;
using note_detail::Code;
using note_detail::EditedAt;
using note_detail::EditNotesIn;
using note_detail::EditWritten;
using note_detail::EndOf;
using note_detail::EndsAt;
using note_detail::EndsBlockIn;
using note_detail::EntryStates;
using note_detail::Examine;
using note_detail::IsSetting;
using note_detail::IsTie;
using note_detail::ItemAt;
using note_detail::kDseLegato;
using note_detail::kEndRefusal;
using note_detail::kFarLater;
using note_detail::KonamiKey;
using note_detail::KonamiSilentNote;
using note_detail::KonamiStartsAlone;
using note_detail::kTieRefusal;
using note_detail::kWaitOn;
using note_detail::Last;
using note_detail::LastsTo;
using note_detail::NoteAt;
using note_detail::NoteTicks;
using note_detail::NoteWaitAt;
using note_detail::ProgramEnd;
using note_detail::ProgramEnds;
using note_detail::Rewrite;
using note_detail::Run;
using note_detail::RunOf;
using note_detail::RunsToWrite;
using note_detail::Spot;
using note_detail::SpotOf;
using note_detail::StretchEnd;
using note_detail::TailOf;
using note_detail::TimelineSpot;
using note_detail::TimelineSpotOf;
using note_detail::TrackEndIn;
using note_detail::UsesVariables;
using note_detail::WaitOf;
using note_detail::WaitRefusal;
using note_detail::WaitTicks;
using note_detail::WithItems;
using ntdlib::commands::Argument;
using ntdlib::commands::Command;
using ntdlib::commands::Kind;
using ntdlib::commands::kOne;
using ntdlib::commands::Source;

} // namespace

namespace note_detail
{

namespace
{

// Writes the new length of `edit` into `note`, item `item` of `code` in block `block` (EditNotesIn). A note of a
// timeline that runs past the end of its track, and a note of a program that runs past the end of the program, move
// that end in `blocks`. `new_lengths` lists the notes whose lengths change alone, and `ends` the ends of the programs.
void ResizeNote(const Code& code, uint32_t item, const NoteEdit* edit, Command& note, std::size_t block, bool timeline,
                const SongEnd& song_end, const std::set<uint32_t>& new_lengths, ProgramEnds& ends,
                std::map<std::size_t, BlockChange>& blocks)
{
    note.value = Argument{Source::kValue, static_cast<int64_t>(*edit->length) * kOne, 0};
    note.encoding.value_bytes = 0;

    // A timeline's note that now runs past the end of its track moves the end.
    const std::map<std::size_t, uint32_t> starts = timeline && edit->shift == 0 && !edit->remove
                                                       ? BlockStarts(code, edit->stretch)
                                                       : std::map<std::size_t, uint32_t>();
    if (const auto start = starts.find(block); start != starts.end())
    {
        const uint32_t at = start->second + BlockTicks(code, block)[item - code.starts[block]];
        Last(blocks, LastsTo(EndOf(code, starts), at, note, song_end));
    }

    // So does a program's note, where the end cuts no other note (ProgramEndOf). The end of a later stretch
    // (NoteEdit::track_end) stays: the notes of the stretches between could sound up to it.
    const ProgramEnd* end = !timeline && !code.konami && !code.dse && new_lengths.contains(item) && *edit->length > 0
                                ? &ends.Of(edit->stretch)
                                : nullptr;
    const std::optional<uint32_t> at = end ? end->TickOf(item) : std::nullopt;
    if (at && end->ends && *at + *edit->length > end->tick)
    {
        const bool others = std::ranges::any_of(end->held, [&](uint32_t held) { return !new_lengths.contains(held); });
        if (end->fixed || others)
        {
            throw std::invalid_argument(std::string(kEndRefusal));
        }

        Last(blocks, std::pair(end->block, *at + *edit->length - end->start));
    }
    else if (at && !end->ends && edit->track_end && *at + *edit->length > *edit->track_end)
    {
        throw std::invalid_argument(std::string(kEndRefusal));
    }
}

// Moves `note`, item `item` of `code` in block `block`, by the shift of `edit` in `blocks` (EditNotesIn). With
// `settings`, the note is a setting.
void ShiftNote(const Code& code, uint32_t item, const NoteEdit* edit, const Command& note, std::size_t block,
               bool timeline, const SongEnd& song_end, bool settings, std::map<std::size_t, BlockChange>& blocks)
{
    const std::vector<uint32_t> ticks = BlockTicks(code, block);
    const int64_t tick = static_cast<int64_t>(ticks[item - code.starts[block]]) + edit->shift;
    const std::map<std::size_t, uint32_t> starts =
        timeline ? BlockStarts(code, edit->stretch) : std::map<std::size_t, uint32_t>();
    if (const auto start = starts.find(block); start != starts.end())
    {
        // A timeline's note moves to any tick of its track before the loop's end (TimelineSpotOf).
        const int64_t at = static_cast<int64_t>(start->second) + tick;
        if (at < 0)
        {
            throw std::invalid_argument(settings ? "Can't place a command before the start of the track."
                                                 : "Can't place a note before the start of the track.");
        }

        const TimelineSpot placed =
            TimelineSpotOf(code, Addition{edit->stretch, static_cast<uint32_t>(at), note}, song_end);
        blocks[block].removed.insert(item);
        blocks[placed.spot.block].placed.push_back({placed.spot.tick, note, item, std::nullopt});
        Last(blocks, placed.lasts);
        return;
    }

    if (tick < 0 || tick > static_cast<int64_t>(ticks.back()))
    {
        throw std::invalid_argument(settings ? "Can't move a command outside its section of the track."
                                             : "Can't move a note outside its section of the track.");
    }

    if (!settings && note.kind == Kind::kNote && tick == static_cast<int64_t>(ticks.back()) &&
        EndsAt(code, TailOf(code, block)))
    {
        throw std::invalid_argument("A note can't start on the tick where the track ends. It wouldn't sound.");
    }

    blocks[block].removed.insert(item);
    blocks[block].placed.push_back({static_cast<uint32_t>(tick), note, item, std::nullopt});

    // A legato's command (0xC0) acts on the next note of the track. It goes with the note before it, and the
    // next note still takes it.
    for (uint32_t next = item + 1; code.dse && note.kind == Kind::kNote && next < code.starts[block + 1]; ++next)
    {
        const Command& after = (*code.items)[next];
        if (after.kind == Kind::kOther && after.encoding.opcode == kDseLegato)
        {
            blocks[block].removed.insert(next);
            blocks[block].placed.push_back({static_cast<uint32_t>(tick), after, next, std::nullopt});
            break;
        }

        if (after.kind == Kind::kNote || after.kind == Kind::kWait)
        {
            break;
        }
    }
}

// Changes item `item` of `items`, a note of `code`, as `edit` says, and records in `blocks` the moves, the removals
// and the ends that move (EditNotesIn). Returns whether the note's block changes.
bool ChangeNote(const Code& code, std::vector<Command>& items, uint32_t item, const NoteEdit* edit, bool timeline,
                const SongEnd& song_end, bool settings, const std::set<uint32_t>& new_lengths, ProgramEnds& ends,
                std::map<std::size_t, BlockChange>& blocks)
{
    Command& note = items[item];
    if (edit->transpose != 0)
    {
        note.key = code.konami ? KonamiKey(note,
                                           note.sample ? static_cast<int8_t>(note.key) + edit->transpose
                                                       : note.key + edit->transpose,
                                           *code.konami)
                               : static_cast<uint8_t>(CheckedByte(note.key + edit->transpose, "key"));
    }

    if (edit->velocity)
    {
        const int velocity = CheckedByte(*edit->velocity, "velocity");
        const int64_t level =
            int64_t{code.driver ? code.driver->VolumeOf(velocity, note.sample.has_value()) : velocity} * kOne;

        // On Konami's driver, a volume command sets the volume of the note that it starts (KonamiSilentNote).
        if (code.konami && note.kind == Kind::kVolume)
        {
            note.value.value = level;
        }
        else
        {
            note.velocity = level;
        }
    }

    if (!edit->length && edit->shift == 0 && !edit->remove)
    {
        return false;
    }

    // A note that waits for its length leaves a wait of that length when it goes. The wait keeps the note's
    // condition: a note whose comparison failed doesn't wait.
    const std::optional<bool> note_wait = NoteWaitAt(code, edit->stretch, item);
    if (note_wait.value_or(true) && settings)
    {
        throw std::invalid_argument(note_wait ? "Another part of the sequence may run this command with note "
                                                "wait on. Can't move it."
                                              : "An earlier command changes note wait during playback. Can't "
                                                "move this command.");
    }

    if (note_wait.value_or(true))
    {
        if (edit->remove && !edit->length && edit->shift == 0 && note_wait && NoteTicks(note))
        {
            Command wait;
            wait.kind = Kind::kWait;
            wait.conditional = note.conditional;
            note = WaitOf(wait, *NoteTicks(note));
            return true;
        }

        const bool removal = edit->remove && !edit->length && edit->shift == 0;
        throw std::invalid_argument(note_wait ? WaitRefusal(note, removal ? "remove" : "move or resize")
                                              : "An earlier command changes note wait during playback. Can't "
                                                "move or resize this note.");
    }

    const std::size_t block = BlockOf(code, item);
    if (edit->length)
    {
        ResizeNote(code, item, edit, note, block, timeline, song_end, new_lengths, ends, blocks);
    }

    if (edit->remove)
    {
        blocks[block].removed.insert(item);
        return true;
    }

    if (edit->shift != 0)
    {
        ShiftNote(code, item, edit, note, block, timeline, song_end, settings, blocks);
    }

    return true;
}

// Places each of `additions` in `blocks`, in the block of `code` that runs at its tick (EditNotesIn). Returns whether
// a block changes.
bool PlaceAdditions(const Code& code, std::span<const Addition> additions, bool timeline, const SongEnd& song_end,
                    std::map<std::size_t, BlockChange>& blocks)
{
    bool resized = false;
    // Each new command goes into the block that runs at its tick (SpotOf).
    for (std::size_t index = 0; index < additions.size(); ++index)
    {
        const Addition& addition = additions[index];
        if (timeline)
        {
            const TimelineSpot placed = TimelineSpotOf(code, addition, song_end);
            blocks[placed.spot.block].placed.push_back({placed.spot.tick, addition.command, std::nullopt, index});
            Last(blocks, placed.lasts);
            resized = true;
            continue;
        }

        const std::optional<Spot> spot = SpotOf(code, addition);
        if (!spot || spot->note_wait != false)
        {
            throw std::invalid_argument(addition.command.kind == Kind::kNote
                                            ? "Can't add a note at that tick on this track."
                                            : "Can't add a command at that tick on this track.");
        }

        blocks[spot->block].placed.push_back({spot->tick, addition.command, std::nullopt, index});
        resized = true;
    }

    return resized;
}

// `file` with the items of sequence `sequence` after the changes of `blocks` to `items`, the items of `code`
// (EditNotesIn). `added` counts the new commands.
Edited RewriteBlocks(const format::File& file, uint32_t sequence, const Code& code, const std::vector<Command>& items,
                     std::size_t added, std::map<std::size_t, BlockChange>& blocks)
{
    // The blocks in order, each one rewritten or as it was.
    Edited edited;
    edited.items.resize(items.size());
    edited.added.resize(added);
    std::vector<Command> rewritten;
    std::vector<uint32_t> new_starts;
    for (std::size_t block = 0; block + 1 < code.starts.size(); ++block)
    {
        new_starts.push_back(static_cast<uint32_t>(rewritten.size()));
        const auto change = blocks.find(block);
        if (change != blocks.end())
        {
            Rewrite(code, items, code.starts[block], code.starts[block + 1], std::move(change->second), rewritten,
                    edited);
            continue;
        }

        for (uint32_t i = code.starts[block]; i < code.starts[block + 1]; ++i)
        {
            edited.items[i] = static_cast<uint32_t>(rewritten.size());
            rewritten.push_back(items[i]);
        }
    }

    new_starts.push_back(static_cast<uint32_t>(rewritten.size()));

    edited.file = WithItems(file, sequence, code, std::move(rewritten), new_starts);

    return edited;
}

} // namespace

Edited EditNotesIn(const format::File& file, uint32_t sequence, std::span<const NoteEdit> edits,
                   std::span<const Addition> additions, bool written, bool settings)
{
    const Code code = Examine(file, sequence);
    std::vector<Command> items = *code.items;
    const bool timeline = IsTimeline(file, sequence);
    const SongEnd song_end = TimelineSongEnd(file, sequence);

    // Each command once, with the first edit that names it.
    std::map<uint32_t, const NoteEdit*> named;
    for (const NoteEdit& edit : edits)
    {
        named.emplace(EditedAt(code, edit.command, settings), &edit);
    }

    // MusicPlayer2000 holds a tie until an end of the tie with its key.
    if (ConsoleOf(file).engine == format::Engine::kGba)
    {
        for (const auto& [item, edit] : named)
        {
            if (IsTie(items[item]) && (edit->transpose != 0 || edit->length || edit->shift != 0 || edit->remove))
            {
                throw std::invalid_argument(std::string(kTieRefusal));
            }
        }
    }

    // On a track whose notes wait for their length, a move, a new length and a new command first turn the note wait off
    // in the runs that they change.
    if (!written)
    {
        const std::map<uint32_t, Run> runs = RunsToWrite(file, sequence, code, named, additions);
        if (!runs.empty())
        {
            if (code.fixed)
            {
                throw std::invalid_argument(
                    "The commands in this sequence can't move. You can change only keys and velocities.");
            }

            return EditWritten(file, sequence, code, runs, edits, additions, settings);
        }
    }

    // The changes to each command. A move takes the note out of its block and puts it back at its new tick.
    std::map<std::size_t, BlockChange> blocks;
    bool resized = false;
    std::set<uint32_t> new_lengths;
    for (const auto& [item, edit] : named)
    {
        if (edit->length && edit->shift == 0 && !edit->remove)
        {
            new_lengths.insert(item);
        }
    }

    ProgramEnds ends(code);
    for (const auto& [item, edit] : named)
    {
        resized =
            ChangeNote(code, items, item, edit, timeline, song_end, settings, new_lengths, ends, blocks) || resized;
    }

    resized = PlaceAdditions(code, additions, timeline, song_end, blocks) || resized;
    if (resized && code.fixed)
    {
        throw std::invalid_argument(
            "The commands in this sequence can't move. You can change only keys and velocities.");
    }

    return RewriteBlocks(file, sequence, code, items, additions.size(), blocks);
}

} // namespace note_detail

namespace
{

// Whether the commands right before block `block` of `code` run a note in the tick where the block starts.
bool NoteRunsBefore(const Code& code, std::size_t block)
{
    for (uint32_t item = code.starts[block]; item-- > 0;)
    {
        const Command& before = (*code.items)[item];
        if (before.kind == Kind::kNote)
        {
            return true;
        }
        if (EndsBlockIn(code, before) || WaitTicks(before).value_or(0) > 0)
        {
            return false;
        }
    }

    return false;
}

// Whether a track or a call of sequence `sequence` of `file` enters at the delay right after `item`.
bool EntersAfter(const format::File& file, uint32_t sequence, const Code& code, uint32_t item)
{
    const uint32_t next = item + 1;
    if (next >= code.items->size() || (*code.items)[next].kind != Kind::kWait)
    {
        return false;
    }

    for (const Command& command : *code.items)
    {
        if (ntdlib::commands::HasTarget(command.kind) && !command.target_outside && command.target == next)
        {
            return true;
        }
    }

    bool entered = false;
    ForEachItemPlace(*file.native, sequence,
                     [&](const ntdlib::sar::Place& place) { entered = entered || place.offset == next; });

    return entered;
}

} // namespace

Edited EditNotes(const format::File& file, uint32_t sequence, std::span<const NoteEdit> edits,
                 std::span<const NewNote> additions)
{
    return EditsOf(ConsoleOf(file)).EditNotes(file, sequence, edits, additions);
}

Edited EditNotesWithLengths(const format::File& file, uint32_t sequence, std::span<const NoteEdit> edits,
                            std::span<const NewNote> additions)
{
    const song::DriverRules* driver = ConsoleOf(file).driver;
    std::vector<Addition> notes;
    for (const NewNote& addition : additions)
    {
        Command note;
        note.kind = Kind::kNote;
        note.key = static_cast<uint8_t>(CheckedByte(addition.key, "key"));
        const int velocity = CheckedByte(addition.velocity, "velocity");
        note.velocity = static_cast<int64_t>(driver ? driver->VolumeOf(velocity, false) : velocity) * kOne;
        note.value = Argument{Source::kValue, static_cast<int64_t>(addition.length) * kOne, 0};
        notes.push_back({addition.stretch, addition.tick, std::move(note)});
    }

    return Writable(file, EditNotesIn(file, sequence, edits, notes, false, false), sequence);
}

Edited AddSettings(const format::File& file, uint32_t sequence, std::span<const NewSetting> settings)
{
    std::vector<Addition> additions;
    for (const NewSetting& setting : settings)
    {
        if (!IsSetting(setting.command))
        {
            throw std::invalid_argument("Only a setting, such as a volume, can be added at a tick.");
        }
        if (file.native && !song::Runs(ConsoleOf(file), setting.command.kind, setting.command.index))
        {
            throw std::invalid_argument(SkippedMessage(ConsoleOf(file), "setting"));
        }

        additions.push_back({setting.stretch, setting.tick, setting.command});
    }

    const song::DriverRules* driver = ConsoleOf(file).driver;
    if (driver)
    {
        if (std::optional<Edited> added = EditsOf(*driver).AddSettings(file, sequence, settings))
        {
            return *std::move(added);
        }
    }

    Edited placed = EditNotesIn(file, sequence, {}, additions, false, false);
    Edited edited = Writable(file, EditsOf(ConsoleOf(file)).Placed(file, std::move(placed), sequence), sequence);

    // The console's bytes have to hold the new commands.
    try
    {
        Offsets(*edited.file.sequences[sequence], ConsoleOf(edited.file));
    }
    catch (const std::invalid_argument&)
    {
        throw std::invalid_argument("This console's sequence format can't store these values.");
    }

    return edited;
}

Edited MoveSettings(const format::File& file, uint32_t sequence, std::span<const SettingMove> moves,
                    std::span<const song::Stretch> rows)
{
    const song::DriverRules* driver = ConsoleOf(file).driver;
    const DriverEdits* driver_edits = driver ? &EditsOf(*driver) : nullptr;
    if (driver_edits != nullptr)
    {
        if (std::optional<Edited> moved = driver_edits->MoveSettings(file, sequence, moves))
        {
            return *std::move(moved);
        }
    }

    std::vector<NoteEdit> edits;
    for (const SettingMove& move : moves)
    {
        const int32_t shift = driver_edits != nullptr ? driver_edits->ShiftOfSetting(rows, move) : move.shift;
        edits.push_back({.command = move.command, .stretch = move.stretch, .shift = shift});
    }

    Edited moved = EditNotesIn(file, sequence, edits, {}, false, true);

    return Writable(file, EditsOf(ConsoleOf(file)).Placed(file, std::move(moved), sequence), sequence);
}

std::optional<song::Stretch> StretchAt(std::span<const song::Stretch> stretches, uint32_t tick)
{
    std::optional<song::Stretch> found;
    for (const song::Stretch& stretch : stretches)
    {
        if (stretch.tick <= tick)
        {
            found = stretch;
        }
    }

    return found;
}

std::optional<ShiftRange> ShiftRangeOf(const format::File& file, uint32_t sequence, uint32_t command,
                                       const song::Stretch& stretch)
{
    const NotePlace place{.command = command, .stretch = stretch};
    return LimitsOf(file, sequence, std::span(&place, 1)).front().shift;
}

std::optional<uint32_t> TrackEndOf(const format::File& file, uint32_t sequence,
                                   std::span<const song::Stretch> stretches)
{
    if (stretches.empty())
    {
        return std::nullopt;
    }

    // A stretch that the sequence's bytecode doesn't hold gives no end.
    try
    {
        const Code code = Examine(file, sequence);
        if (code.konami || code.dse)
        {
            return std::nullopt;
        }

        return TrackEndIn(code, stretches);
    }
    catch (const std::invalid_argument&)
    {
        return std::nullopt;
    }
}

namespace
{

// A timeline's note moves to any tick of its track before the loop's end, and past the end of a track that
// ends, up to the song's end (TimelineSongEnd and TimelineSpotOf). Where the header doesn't give that tick, the
// note stays before the tick where its track ends. Nothing moves to the end of the loop that the arrangement
// shows, or past it.
// TimelineLimits sets that room in `limit` for `note`, whose command `command` sits at tick `tick` of block `block`
// of `code`, where `in_timeline` says that the note belongs to a timeline and doesn't wait for its length. `loop_end`
// is the end of the loop in the arrangement.
void TimelineLimits(const Code& code, const NotePlace& note, const Command& command, std::size_t block, uint32_t tick,
                    bool in_timeline, const SongEnd& song_end, std::optional<uint32_t> loop_end, NoteLimits& limit)
{
    const std::map<std::size_t, uint32_t> starts =
        in_timeline ? BlockStarts(code, note.stretch) : std::map<std::size_t, uint32_t>();
    if (const auto start = starts.find(block); start != starts.end())
    {
        const uint32_t at = start->second + tick;
        const StretchEnd end = EndOf(code, starts);
        const bool past_end = command.kind == Kind::kNote && end.ends && !(song_end.loops && !song_end.tick);
        uint32_t later = end.tick > at ? end.tick - at : 0;
        if (end.loops || (command.kind == Kind::kNote && end.ends && !past_end))
        {
            later = later > 0 ? later - 1 : 0;
        }
        else if (past_end)
        {
            later = kFarLater;
        }

        if (!end.loops && song_end.tick)
        {
            later = std::min(later, *song_end.tick > at ? *song_end.tick - at - 1 : 0);
        }

        if (loop_end)
        {
            later = std::min(later, *loop_end > at ? *loop_end - at - 1 : 0);
        }

        limit.shift = ShiftRange{at - note.stretch.tick, later};

        // The end of a track that ends cuts a note whose command lasts past it. An edit of the note's length moves
        // the end (LastsTo).
        limit.cut_by_end = command.kind == Kind::kNote && LastsTo(end, at, command, song_end).has_value();
    }
}

// The length of note `command`, item `item` of block `block` of `code`, that an edit can change, where it has one
// (LimitsOf). `ticks` contains the ticks of the items of the block.
std::optional<uint32_t> LengthLimit(const Code& code, const Command& command, uint32_t item, std::size_t block,
                                    const std::vector<uint32_t>& ticks)
{
    const Argument& length = command.value;
    const bool volume_start = KonamiSilentNote(code, item).has_value();
    if (code.konami && (command.kind == Kind::kNote || volume_start))
    {
        // The command has no length. The note sounds up to the next note or rest of its block. A note that a volume
        // command starts sounds up to the next volume command too.
        uint32_t until = ticks.back();
        for (uint32_t i = item + 1; i < code.starts[block + 1]; ++i)
        {
            const Kind kind = (*code.items)[i].kind;
            if (kind == Kind::kNote || kind == Kind::kNoteOff ||
                (volume_start && kind == Kind::kVolume && (*code.items)[i].index == 0))
            {
                until = ticks[i - code.starts[block]];
                break;
            }
        }

        return until - ticks[item - code.starts[block]];
    }
    else if (command.kind == Kind::kNote)
    {
        return WholeTicks(length);
    }

    return std::nullopt;
}

// The ticks that a note or a setting can move among: the ticks of its block or run, its tick there, and the item where
// the block or run ends (LimitsOf).
struct MoveTicks
{
    std::vector<uint32_t> ticks;
    uint32_t tick = 0;
    uint32_t tail = 0;
};

// The limits of the notes and settings of sequence `sequence` of `file`, with `code` its blocks (LimitsOf). The entry
// states of the blocks come once, when a note first requires them.
class NoteLimiter
{
public:
    NoteLimiter(const format::File& file, uint32_t sequence, const Code& code, std::optional<uint32_t> loop_end)
        : file_(file),
          sequence_(sequence),
          code_(code),
          loop_end_(loop_end),
          timeline_(IsTimeline(file, sequence)),
          song_end_(TimelineSongEnd(file, sequence)),
          gba_(ConsoleOf(file).engine == format::Engine::kGba),
          program_ends_(code)
    {
    }

    // The limits of `note`.
    NoteLimits LimitOf(const NotePlace& note);

private:
    // The limits of `note` by the timing of its track, before KonamiVolumeLimits.
    NoteLimits TimingLimitOf(const NotePlace& note);

    // Changes `limit`, the limits of `note`, where it is a volume command of Konami's driver that starts a note.
    void KonamiVolumeLimits(const NotePlace& note, NoteLimits& limit) const;

    // Whether a track enters block `block` with the note wait on.
    bool EnteredWithWaitOn(std::size_t block);

    // The ticks that the command at item `item` of block `block` can move among, on a track whose notes wait for their
    // length with `note_wait`, or nothing where the command stays at its tick.
    std::optional<MoveTicks> TicksAround(std::size_t block, uint32_t item, bool note_wait);

    const format::File& file_;
    const uint32_t sequence_;
    const Code& code_;
    const std::optional<uint32_t> loop_end_;
    const bool timeline_;
    const SongEnd song_end_;
    const bool gba_;
    std::optional<std::vector<int>> states_;
    std::optional<std::vector<int>> started_;
    ProgramEnds program_ends_;
};

NoteLimits NoteLimiter::LimitOf(const NotePlace& note)
{
    NoteLimits limit = TimingLimitOf(note);
    KonamiVolumeLimits(note, limit);
    return limit;
}

NoteLimits NoteLimiter::TimingLimitOf(const NotePlace& note)
{
    // A note moves inside its block. On a track whose notes wait for their length, it moves inside its run, once
    // waits of their own take the place of the notes' waits (WriteWaits). Every track has to enter the block with
    // the note wait on. Otherwise no track may run the note with the note wait on: its wait would follow its
    // length. A setting moves the same way, whatever the note wait of other tracks. A tie on the GBA stays where
    // it is.
    NoteLimits limit;
    const uint32_t item = ItemAt(code_, note.command);
    const Command& command = (*code_.items)[item];
    const std::optional<bool> note_wait = NoteWaitAt(code_, note.stretch, item);
    if (gba_ && IsTie(command))
    {
        limit.fixed = kTieRefusal;
        return limit;
    }
    if (code_.fixed || !note_wait || (command.kind != Kind::kNote && WhyFixedInTime(command)))
    {
        return limit;
    }

    const std::size_t block = BlockOf(code_, item);
    if (!*note_wait && command.kind == Kind::kNote && EnteredWithWaitOn(block))
    {
        return limit;
    }

    const std::optional<MoveTicks> around = TicksAround(block, item, *note_wait);
    if (!around)
    {
        return limit;
    }

    const std::vector<uint32_t>& ticks = around->ticks;
    const uint32_t tick = around->tick;
    const uint32_t tail = around->tail;

    // A note can't start in the tick where its track ends (EndsAt).
    const bool ends = command.kind == Kind::kNote && ticks.back() > tick && EndsAt(code_, tail);
    limit.shift = ShiftRange{tick, ticks.back() - tick - (ends ? 1 : 0)};

    // On Konami's driver, a delay that a track or a call enters can't follow another delay. The last command before
    // such a delay can't move earlier.
    if (code_.konami && limit.shift->later == 0 && EntersAfter(file_, sequence_, code_, item))
    {
        limit.shift->earlier = 0;
    }

    // A setting at the start of a block runs after a note that the commands before the block run in that tick. A
    // volume there can take the place of the note's start. The setting stops a tick later.
    if (code_.konami && command.kind != Kind::kNote && limit.shift->earlier == tick && tick > 0 &&
        NoteRunsBefore(code_, block))
    {
        limit.shift->earlier = tick - 1;
    }

    // A volume command that starts a note takes the silent note command before it along (EditKonamiNotes). The two
    // stop where that command reaches the start of the block.
    const std::optional<uint32_t> silent = *note_wait ? std::nullopt : KonamiSilentNote(code_, item);
    if (silent && KonamiStartsAlone(code_, *silent))
    {
        limit.shift->earlier = std::min(limit.shift->earlier, ticks[*silent - code_.starts[block]]);
    }

    TimelineLimits(code_, note, command, block, tick, timeline_ && !*note_wait, song_end_, loop_end_, limit);

    // So does the end of a program's track, in the note's stretch (ProgramEndOf) or after it
    // (NotePlace::track_end).
    if (!timeline_ && !*note_wait && command.kind == Kind::kNote && NoteTicks(command) && !code_.konami && !code_.dse)
    {
        const ProgramEnd& end = program_ends_.Of(note.stretch);
        const std::optional<uint32_t> at = end.TickOf(item);
        limit.cut_by_end =
            end.ends ? end.held.contains(item) : at && note.track_end && *at + *NoteTicks(command) > *note.track_end;
    }

    if (const std::optional<uint32_t> length = LengthLimit(code_, command, item, block, ticks))
    {
        limit.length = length;
    }

    return limit;
}

void NoteLimiter::KonamiVolumeLimits(const NotePlace& note, NoteLimits& limit) const
{
    // On Konami's driver, the edits of a note that a volume command starts refuse what could change another note
    // (TakeSilentNote in konami_notes.cpp), and the limits give the same reasons. A volume command without a silent
    // note command in its block accepts no edit as a note (kKonamiFarKey). One whose note command sets the key of
    // another note too can't move or take a new key. One whose note command is the last of its block can't take a new
    // key (kKonamiOpenKey).
    if (!code_.konami)
    {
        return;
    }
    const uint32_t item = ItemAt(code_, note.command);
    const Command& command = (*code_.items)[item];
    if (command.kind != Kind::kVolume || command.index != 0)
    {
        return;
    }

    const std::optional<uint32_t> silent = KonamiSilentNote(code_, item);
    if (!silent)
    {
        limit.fixed_length = note_detail::kKonamiFarKey;
        if (note.note)
        {
            limit = {.fixed = std::string(note_detail::kKonamiFarKey),
                     .fixed_length = std::string(note_detail::kKonamiFarKey),
                     .fixed_key = std::string(note_detail::kKonamiFarKey)};
        }
        return;
    }
    if (!KonamiStartsAlone(code_, *silent))
    {
        limit.shift.reset();
        limit.fixed = note_detail::KonamiSharedKeyRefusal("move");
        limit.fixed_key = note_detail::KonamiSharedKeyRefusal("transpose");
    }
    else if (!note_detail::KonamiKeyEndsInBlock(code_, *silent))
    {
        limit.fixed_key = note_detail::kKonamiOpenKey;
    }
}

bool NoteLimiter::EnteredWithWaitOn(std::size_t block)
{
    if (!started_)
    {
        started_ = EntryStates(file_, sequence_, code_, false);
    }

    return ((*started_)[block] & kWaitOn) != 0;
}

std::optional<MoveTicks> NoteLimiter::TicksAround(std::size_t block, uint32_t item, bool note_wait)
{
    MoveTicks around;
    around.tail = TailOf(code_, block);
    if (note_wait)
    {
        // Without the command that turns the note wait off, no wait can take the place of a note's wait
        // (WriteWaits). The command stays at its tick.
        if (!code_.writes_note_wait)
        {
            return std::nullopt;
        }

        if (!states_)
        {
            states_ = EntryStates(file_, sequence_, code_);
        }

        std::optional<Run> run = (*states_)[block] == kWaitOn ? RunOf(code_, block, item) : std::nullopt;
        if (!run)
        {
            return std::nullopt;
        }

        around.tick = run->ticks[item - run->begin];
        around.tail = run->end;
        around.ticks = std::move(run->ticks);
    }
    else
    {
        around.ticks = BlockTicks(code_, block);
        around.tick = around.ticks[item - code_.starts[block]];
    }

    return around;
}

} // namespace

std::vector<NoteLimits> LimitsOf(const format::File& file, uint32_t sequence, std::span<const NotePlace> notes,
                                 std::optional<uint32_t> loop_end, std::span<const song::Stretch> rows)
{
    const song::DriverRules* driver = ConsoleOf(file).driver;
    const DriverEdits* driver_edits = driver ? &EditsOf(*driver) : nullptr;
    if (driver_edits != nullptr)
    {
        if (std::optional<std::vector<NoteLimits>> limits = driver_edits->LimitsOf(file, sequence, notes))
        {
            return *std::move(limits);
        }
    }

    const Code code = Examine(file, sequence);
    NoteLimiter limiter(file, sequence, code, loop_end);
    std::vector<NoteLimits> limits;
    for (const NotePlace& note : notes)
    {
        limits.push_back(limiter.LimitOf(note));
    }

    if (driver_edits != nullptr)
    {
        std::vector<Kind> kinds;
        for (const NotePlace& note : notes)
        {
            kinds.push_back((*code.items)[ItemAt(code, note.command)].kind);
        }

        driver_edits->FitLimits(notes, kinds, rows, limits);
    }

    return limits;
}

std::optional<std::string> WhyFixedInTime(const Command& command)
{
    if (!IsSetting(command) || command.kind == Kind::kTimebase || command.kind == Kind::kAllocateTracks)
    {
        return "Only a setting, such as a volume, can move to another tick.";
    }
    if (command.conditional)
    {
        return "Can't move a command that runs only if the last comparison was true.";
    }
    if (UsesVariables(command))
    {
        return "Can't move a command that reads or changes a variable.";
    }

    return std::nullopt;
}

int CommandKey(const format::File& file, uint32_t sequence, uint32_t command)
{
    const Code code = Examine(file, sequence);

    // On Konami's driver, a note that a volume command starts plays at the key of the note command before it.
    const std::optional<uint32_t> silent = KonamiSilentNote(code, ItemAt(code, command));
    return (*code.items)[silent ? *silent : NoteAt(code, command)].key;
}

std::vector<uint32_t> CommandsAfter(const format::File& file, uint32_t sequence, const Edited& edited,
                                    std::span<const uint32_t> commands)
{
    const Code code = Examine(file, sequence);
    const std::vector<Command>& items = edited.file.sequences.at(sequence)->items;
    const std::vector<uint32_t> offsets = Offsets(*edited.file.sequences[sequence], ConsoleOf(edited.file));

    // A removed note on a track whose notes wait for their length leaves a wait in its place.
    std::vector<uint32_t> after;
    // On Konami's driver, a volume command can start the note that names it (KonamiSilentNote).
    const auto add = [&](std::optional<uint32_t> item, bool volume_start)
    {
        if (item && (items[*item].kind == Kind::kNote || (volume_start && items[*item].kind == Kind::kVolume)))
        {
            after.push_back(offsets[*item]);
        }
    };
    for (const uint32_t command : commands)
    {
        const uint32_t item = ItemAt(code, command);
        add(edited.items.at(item), code.konami && (*code.items)[item].kind == Kind::kVolume);
    }

    for (const uint32_t item : edited.added)
    {
        add(item, false);
    }

    return after;
}

} // namespace hokuto::edit
