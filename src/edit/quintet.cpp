// SPDX-License-Identifier: MIT

#include "edit/quintet.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/commands/units.h"
#include "ntdlib/sar/layout.h"

#include "edit/bytecode.h"
#include "edit/commands.h"
#include "edit/notes.h"
#include "edit/replace.h"
#include "edit/tracks.h"
#include "format/archive.h"
#include "format/drivers.h"
#include "format/quintet.h"
#include "song/quintet.h"
#include "song/sequence.h"

namespace hokuto::edit
{

namespace
{

using ntdlib::commands::Command;
using ntdlib::commands::Kind;
using ntdlib::commands::kOne;
using ntdlib::commands::Sequence;
using ntdlib::commands::Source;

// The command that sets a channel's loop point, in the A revision's numbering.
constexpr uint16_t kLoopPoint = 0xBF;

// The commands that add 1 to a channel's volume and take 1 from it, in the A revision's numbering.
constexpr uint16_t kVolumeUp = 0x5F;
constexpr uint16_t kVolumeDown = 0x6E;

// The highest pitch of a note.
constexpr int kHighestPitch = 11;

const char* const kKeepsTracks = "A song of Quintet's driver has its 6 channels. You can't add a track to it.";

// Why a note can't move and why its length can't change. The edits refuse both (EditQuintetNote), and the Key Editor
// shows the texts before an edit (FitLimits).
const char* const kNoteStays =
    "A note of Quintet's driver can't move. The notes and rests before it on its channel set its tick.";
const char* const kNoteKeepsLength =
    "A note of Quintet's driver keeps its length. A new length would move the channel's later notes.";

// The item of the rest that starts at `tick` in `stretch` of `sequence`, or nothing where none does. The stretch's
// commands run one after another from its first, and each note, rest and wait moves the channel on by its length.
std::optional<uint32_t> RestAt(const Sequence& sequence, const std::vector<uint32_t>& offsets,
                               const song::Stretch& stretch, uint32_t tick)
{
    const std::optional<uint32_t> first = ItemAtOffset(offsets, stretch.first);
    const std::optional<uint32_t> last = ItemAtOffset(offsets, stretch.last);
    if (!first || !last)
    {
        return std::nullopt;
    }

    int64_t at = stretch.tick;
    for (uint32_t item = *first; item <= *last && item < sequence.items.size(); item++)
    {
        const Command& command = sequence.items[item];
        if (at == tick && command.kind == Kind::kNoteOff)
        {
            return item;
        }

        if (format::QuintetTimed(command.kind))
        {
            at += command.value.value / kOne;
        }
        if (at > tick)
        {
            break;
        }
    }

    return std::nullopt;
}

// The items that ties (D4) join to item `item` of `sequence`, a note or a rest: each tie followed by a note of the same
// pitch, or by a rest after a rest, adds that item's length to the note (ntdlib's Sequencer::NoteTicks). The
// arrangement shows one note.
std::vector<uint32_t> TiedTo(const Sequence& sequence, uint32_t item)
{
    std::vector<uint32_t> tied;
    const Command& first = sequence.items[item];
    for (uint32_t at = item + 1; at + 1 < sequence.items.size() && sequence.items[at].kind == Kind::kTie; at += 2)
    {
        const Command& next = sequence.items[at + 1];
        const bool same = first.kind == Kind::kNote
                              ? next.kind == Kind::kNote && next.key == first.key && next.index == first.index
                              : next.kind == Kind::kNoteOff;
        if (!same || first.kind == Kind::kWait)
        {
            break;
        }

        tied.push_back(at + 1);
    }

    return tied;
}

// Applies `edit` to its note in `after`, a copy of `before` that the edit changes (QuintetEditNotes). `offsets`
// contains the offsets of the items in `before`. Throws std::invalid_argument with a message for the user for a note
// that the Key Editor doesn't show there, and for an edit that the driver can't play.
void EditQuintetNote(const Sequence& before, const std::vector<uint32_t>& offsets, Sequence& after,
                     const NoteEdit& edit)
{
    const std::optional<uint32_t> item = ItemAtOffset(offsets, edit.command);
    if (!item || after.items[*item].kind != Kind::kNote)
    {
        throw std::invalid_argument("The note's command isn't where the Key Editor shows it.");
    }

    // A tie joins notes of one pitch into the note, and rests into a rest: an edit changes them together.
    const std::vector<uint32_t> tied = TiedTo(before, *item);
    Command& note = after.items[*item];
    if (edit.remove)
    {
        for (const uint32_t removed : tied)
        {
            after.items[removed].kind = Kind::kNoteOff;
            after.items[removed].key = 0;
            after.items[removed].index = 0;
        }

        note.kind = Kind::kNoteOff;
        note.key = 0;
        note.index = 0;
        return;
    }

    if (edit.velocity && *edit.velocity != 127)
    {
        throw std::invalid_argument("Quintet's notes have no velocity.");
    }
    if (edit.shift != 0)
    {
        throw std::invalid_argument(kNoteStays);
    }
    if (edit.length)
    {
        throw std::invalid_argument(kNoteKeepsLength);
    }

    if (edit.transpose != 0)
    {
        if (note.index == 1)
        {
            throw std::invalid_argument("This note plays the last note again. It has no pitch of its own.");
        }

        const int pitch = note.key + edit.transpose;
        if (pitch < 0 || pitch > kHighestPitch)
        {
            throw std::invalid_argument(std::string(kQuintetKeepsOctave));
        }

        note.key = static_cast<uint8_t>(pitch);
        for (const uint32_t joined : tied)
        {
            after.items[joined].key = note.key;
        }
    }
}

// Puts `addition` in the place of the rest at its tick in `after`, a copy of `before` (QuintetEditNotes), and returns
// the rest's item. `offsets` contains the offsets of the items in `before`. Throws std::invalid_argument with a message
// for the user where no rest starts at the tick. The key has to be in the octave (QuintetEditNotes).
uint32_t AddQuintetNote(const Sequence& before, const std::vector<uint32_t>& offsets, Sequence& after,
                        const NewNote& addition)
{
    const std::optional<uint32_t> rest = RestAt(after, offsets, addition.stretch, addition.tick);
    if (!rest)
    {
        throw std::invalid_argument(
            "A new note of Quintet's driver takes the place of a rest. No rest starts at that tick.");
    }
    for (const uint32_t joined : TiedTo(before, *rest))
    {
        after.items[joined].kind = Kind::kNote;
        after.items[joined].key = static_cast<uint8_t>(addition.key);
        after.items[joined].index = 0;
    }

    Command& note = after.items[*rest];
    note.kind = Kind::kNote;
    note.key = static_cast<uint8_t>(addition.key);
    note.index = 0;

    return *rest;
}

// A new note: the offset of its command, and the tick of the rest that it replaces.
struct AddedNote
{
    uint32_t offset = 0;
    uint32_t tick = 0;
};

// What the converter makes of the new notes: each one a note of its own, one joined to the note before it, or one that
// makes no note.
enum class NewNotes
{
    kSound,
    kJoined,
    kSilent,
};

// What the converter makes of `added`, new notes in sequence `sequence` of `file` (NewNotes). ntdlib's converter keeps
// only a note that sounds in some frame of its model. A channel of the J revision starts at volume 0 with its hardware
// envelope going down, and a note plays at the channel's volume: before a volume command it plays nothing. One of the A
// revision starts with the envelope going up, and its note sounds (ntdlib's RevisionTraits::start_envelope_up). The
// converter follows the channel's run, its loops and each revision. A tie before the rest, after a note of the same
// pitch, joins the new note to that note: the read then shows the note before it going on over the new note's tick,
// however the bytes write the tie and the dots.
NewNotes CheckNotesSound(const format::File& file, uint32_t sequence, std::span<const AddedNote> added)
{
    const std::shared_ptr<const song::QuintetRom> rom = song::LoadQuintetSongs(format::ExportArchive(file));
    const std::vector<uint64_t> starts = format::NativePieceStarts(file);
    for (const uint32_t sound : rom->Sounds())
    {
        const std::optional<uint64_t> field = rom->FieldsOf(sound).sequence;
        if (!field || format::SequenceNamedAt(file, *field, starts) != sequence)
        {
            continue;
        }

        const song::Sequence read = rom->ReadSequence(sound);
        for (const AddedNote& note : added)
        {
            const auto runs = [&](const song::Stretch& stretch)
            {
                return stretch.first <= note.offset && note.offset <= stretch.last;
            };
            const auto plays = [&](const song::Note& played)
            {
                return played.command == note.offset;
            };
            const auto joins = [&](const song::Note& played)
            {
                return played.start < note.tick && note.tick < played.start + played.length;
            };
            for (const song::Track& track : read.tracks)
            {
                if (std::ranges::none_of(track.stretches, runs) || std::ranges::any_of(track.notes, plays))
                {
                    continue;
                }
                return std::ranges::any_of(track.notes, joins) ? NewNotes::kJoined : NewNotes::kSilent;
            }
        }
        return NewNotes::kSound;
    }

    return NewNotes::kSound;
}

Edited QuintetEditNotes(const format::File& file, uint32_t sequence, std::span<const NoteEdit> edits,
                        std::span<const NewNote> additions)
{
    const Sequence& before = SequenceToEdit(file, sequence);
    const std::vector<uint32_t> offsets = Offsets(before, ConsoleOf(file));
    Sequence after = before;
    for (const NoteEdit& edit : edits)
    {
        EditQuintetNote(before, offsets, after, edit);
    }

    // A key out of the octave can't be written. The note is inserted at the lowest key of the octave for the converter
    // check. A note that wouldn't sound in any octave gets that reason first.
    std::vector<uint32_t> added;
    std::vector<AddedNote> added_notes;
    bool outside = false;
    for (NewNote addition : additions)
    {
        if (addition.key < 0 || addition.key > kHighestPitch)
        {
            outside = true;
            addition.key = 0;
        }
        added.push_back(AddQuintetNote(before, offsets, after, addition));
        added_notes.push_back({offsets[added.back()], addition.tick});
    }

    Edited edited = InPlace(file, sequence, std::move(after));
    edited.added = std::move(added);
    Edited writable = WritableByOffsets(file, std::move(edited), sequence);
    const NewNotes outcome =
        added_notes.empty() ? NewNotes::kSound : CheckNotesSound(writable.file, sequence, added_notes);
    if (outcome == NewNotes::kSilent)
    {
        throw std::invalid_argument(
            "A note there wouldn't sound. The channel has no volume yet. Draw it after the channel's volume command.");
    }
    if (outside)
    {
        throw std::invalid_argument(
            "A new note of Quintet's driver has to be in the octave that the channel plays there. Change the channel's "
            "octave in the List Editor first.");
    }
    if (outcome == NewNotes::kJoined)
    {
        throw std::invalid_argument(
            "Can't add a note of this key here. The tie before the rest would join it to the note before it.");
    }

    return writable;
}

// The item where track `track` of sound `sound` starts: the field of its channel in the song's header names it.
uint32_t ChannelStart(const format::File& file, std::span<const std::optional<SoundPlace>> places, uint32_t sound,
                      int track)
{
    const SoundPlace& place = PlaceToEdit(places, sound);
    if (track < 0 || static_cast<std::size_t>(track) >= place.track_fields.size() || !file.native ||
        place.track_fields[static_cast<std::size_t>(track)] >= file.native->fields.size())
    {
        throw std::invalid_argument("The song has no such channel.");
    }

    return file.native->fields[place.track_fields[static_cast<std::size_t>(track)]].to.offset;
}

// Whether a track of Quintet's driver that ran `stretches` runs a command that adds to its volume
// (StepsQuintetVolume).
bool StepsVolume(const ntdlib::commands::Sequence& sequence, const std::vector<uint32_t>& offsets,
                 std::span<const song::Stretch> stretches)
{
    for (const song::Stretch& stretch : stretches)
    {
        const std::optional<uint32_t> first = ItemAtOffset(offsets, stretch.first);
        const std::optional<uint32_t> last = ItemAtOffset(offsets, stretch.last);
        for (uint32_t i = first.value_or(0); first && last && i <= *last; ++i)
        {
            if (StepsQuintetVolume(sequence.items[i]))
            {
                return true;
            }
        }
    }

    return false;
}

class QuintetDriverEdits final : public FixedTracksEdits
{
public:
    QuintetDriverEdits()
        : FixedTracksEdits(kKeepsTracks, "A song of Quintet's driver has its 6 channels. You can't paste a track.")
    {
    }

    Edited Writable(const format::File& before, Edited edited, uint32_t sequence) const override
    {
        return WritableByOffsets(before, std::move(edited), sequence);
    }

    Edited EditNotes(const format::File& file, uint32_t sequence, std::span<const NoteEdit> edits,
                     std::span<const NewNote> additions) const override
    {
        return QuintetEditNotes(file, sequence, edits, additions);
    }

    // A note stays at its tick and keeps its length.
    void FitLimits(std::span<const NotePlace> /*notes*/, std::span<const Kind> kinds,
                   std::span<const song::Stretch> /*rows*/, std::vector<NoteLimits>& limits) const override
    {
        for (std::size_t i = 0; i < limits.size(); i++)
        {
            if (kinds[i] == Kind::kNote)
            {
                limits[i] = {.fixed = kNoteStays, .fixed_length = kNoteKeepsLength};
            }
        }
    }

    // The bytes of each command's arguments.
    std::optional<ValueRange> Range(const song::Console& console, int track, const Command& command, Source source,
                                    bool time) const override
    {
        if (time || source != Source::kValue)
        {
            return std::nullopt;
        }

        switch (command.kind)
        {
        case Kind::kTempo:
        case Kind::kProgram:
            return ValueRange{0, 0xFF * kOne};
        case Kind::kVolume:
            return command.index == 0 ? ValueRange{0, console.driver->MostVolume(track) * kOne}
                                      : ValueRange{-0x80 * kOne, 0x7F * kOne};
        case Kind::kTranspose:
            return ValueRange{-0x80 * kOne, 0x7F * kOne};
        case Kind::kTune:
            return command.index == 0 ? ValueRange{-0x80 * kOne, 0x7F * kOne}
                                      : ValueRange{-0x8000 * kOne, 0x7FFF * kOne};
        case Kind::kLoopEnd:
            return ValueRange{1 * kOne, 256 * kOne};
        default:
            break;
        }

        return std::nullopt;
    }

    // FF jumps back to the channel's loop point (BF). A loop point right before FF keeps the channel there. Between
    // them, a note, a rest or a wait has to move the channel on, or the driver reads them again and again in one frame.
    std::size_t LoopsWithoutWaits(const Sequence& sequence, std::span<const uint32_t> entries) const override
    {
        std::size_t count = 0;
        for (const uint32_t entry : entries)
        {
            std::optional<uint32_t> loop;
            bool waits = false;
            for (uint32_t item = entry; item < sequence.items.size(); item++)
            {
                const Command& command = sequence.items[item];
                if (command.kind == Kind::kData)
                {
                    break;
                }

                if (command.kind == Kind::kEnd)
                {
                    count += loop && *loop + 1 < item && !waits ? 1 : 0;
                    break;
                }

                if (command.kind == Kind::kOther && command.encoding.opcode == kLoopPoint)
                {
                    loop = item;
                    waits = false;
                }

                waits = waits || format::QuintetTimed(command.kind);
            }
        }

        return count;
    }

    // A note waits for its note value.
    bool NoteWaitOpens(const Sequence&) const override
    {
        return true;
    }

    // D6 starts a repeat, and D7 ends it after a number of passes.
    bool CountsRepeats() const override
    {
        return true;
    }

    // The channel's notes become rests of the same note values.
    format::File RemoveTrack(const format::File& file, std::span<const std::optional<SoundPlace>> places,
                             uint32_t sound, int track) const override
    {
        const uint32_t start = ChannelStart(file, places, sound, track);
        const uint32_t sequence = PlaceToEdit(places, sound).sequence;
        Sequence after = SequenceToEdit(file, sequence);
        bool found = false;
        for (uint32_t item = start; item < after.items.size(); item++)
        {
            Command& command = after.items[item];
            if (command.kind == Kind::kEnd || command.kind == Kind::kData)
            {
                break;
            }

            if (command.kind == Kind::kNote)
            {
                command.kind = Kind::kNoteOff;
                command.key = 0;
                command.index = 0;
                found = true;
            }
        }
        if (!found)
        {
            throw std::invalid_argument("This track plays no notes.");
        }

        return WritableByOffsets(file, InPlace(file, sequence, std::move(after)), sequence).file;
    }

    std::string RefusedPan(const song::DriverRules& /*rules*/) const override
    {
        return "Tracks of Quintet's driver have no pan.";
    }

    void CheckMixEdit(const ntdlib::commands::Sequence& commands, const std::vector<uint32_t>& offsets,
                      std::span<const song::Stretch> stretches, song::Mix setting, bool /*run_empty*/) const override
    {
        // A volume step (F1), a volume up (5F) and a volume down (6E) of Quintet's driver add to the volume. A new
        // volume would change what they add to.
        if (setting == song::Mix::kVolume && StepsVolume(commands, offsets, stretches))
        {
            throw std::invalid_argument(
                "This track's volume changes in steps. Change its volume commands in the List Editor: Edit > Open "
                "List Editor.");
        }
    }
};

} // namespace

const DriverEdits& QuintetEdits()
{
    static const QuintetDriverEdits edits;
    return edits;
}

bool IsQuintet(const song::Console& console)
{
    return RunsDriver(console, format::Engine::kQuintet);
}

bool StepsQuintetVolume(const Command& command)
{
    return (command.kind == Kind::kVolume && command.index == 1) ||
           (command.kind == Kind::kOther &&
            (command.encoding.opcode == kVolumeUp || command.encoding.opcode == kVolumeDown));
}

} // namespace hokuto::edit
