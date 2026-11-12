// SPDX-License-Identifier: MIT

#include "edit/krawall.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/commands/units.h"
#include "ntdlib/krawall/commands.h"
#include "ntdlib/krawall/instruments.h"

#include "edit/bytecode.h"
#include "edit/commands.h"
#include "edit/krawall_parts.h"
#include "edit/notes.h"
#include "edit/replace.h"
#include "edit/tracks.h"
#include "format/file.h"

namespace hokuto::edit
{

namespace
{

using krawall_parts::EffectOf;
using krawall_parts::HeaderOf;
using krawall_parts::ModuleHeader;
using krawall_parts::NextNoteLacksInstrument;
using krawall_parts::OperandOf;
using ntdlib::commands::Command;
using ntdlib::commands::Kind;
using ntdlib::commands::kOne;
using ntdlib::commands::Sequence;
using ntdlib::commands::Source;

const char* const kKeepsTracks = "A module of Krawall keeps the channels of its header. You can't add a track to it.";

// Why a note can't move and why its length can't change. The edits refuse both (EditKrawallNote), and the Key Editor
// shows the texts before an edit (FitLimits).
const char* const kNoteStays = "A note of Krawall can't move. Its row sets its tick.";
const char* const kNoteKeepsLength =
    "A note of Krawall sounds until the next note or note off of its channel. Change "
    "those rows in the List Editor.";

// Makes `command`, a note, a note off. With `keep_instrument`, the note off keeps the note's instrument.
void MakeNoteOff(Command& command, bool keep_instrument)
{
    command.kind = Kind::kNoteOff;
    command.key = 0;
    if (!keep_instrument)
    {
        command.sample = 0;
    }
}

// The effects of Krawall that the edits look for, by the numbers of ntdlib's player (ntdlib/src/krawall/player.cpp).
constexpr uint8_t kPortaNote = 19;
constexpr uint8_t kVolumeSlidePorta = 24;
constexpr uint8_t kOffset = 27;
constexpr uint8_t kRetrig = 29;
constexpr uint8_t kNoteDelay = 45;

// Whether an effect starts a note of its channel without a note. A note delay starts the channel's note at its tick,
// even after a note off (Player::PlayEntry and the delay in Player::Effect). In a module with instruments, a note off
// lets the note fade with its envelope. A retrigger or an offset then starts the fading note's sample again
// (Player::SetPos). In a module without instruments, a note off stops the channel, and neither of them starts it.
bool StartsNote(const Command& command, bool instruments)
{
    const std::optional<uint8_t> effect = EffectOf(command);
    if (!effect)
    {
        return false;
    }

    return (*effect == kNoteDelay && OperandOf(command) != 0) ||
           (instruments && (*effect == kRetrig || *effect == kOffset));
}

// Whether the note at item `note` of `commands` only slides the channel's note towards its key. Its entry contains a
// slide to a note, and the note then sets the slide's goal without starting (Player::PlayEntry).
bool SlidesOnly(const Sequence& commands, uint32_t note)
{
    const uint8_t channel = commands.items[note].index;
    for (uint32_t i = note + 1; i < commands.items.size() && commands.items[i].index == channel; i++)
    {
        const std::optional<uint8_t> effect = EffectOf(commands.items[i]);
        if (effect)
        {
            return *effect == kPortaNote || *effect == kVolumeSlidePorta;
        }
        if (commands.items[i].kind != Kind::kVolume)
        {
            break;
        }
    }

    return false;
}

// The items after the note at item `from` of `commands` that start the channel's note again without a note, up to the
// channel's next note in the pattern that starts: the effects for which StartsNote returns true, in the note's entry
// and in the channel's later entries without a note or with a note that only slides (SlidesOnly). In the module, they
// play the note again: its sample at its pitch, with a delay or a retrigger. After a removal, they would play the
// channel's note before it.
std::vector<uint32_t> EchoesOf(const Sequence& commands, uint32_t from, bool instruments)
{
    std::vector<uint32_t> echoes;
    const uint8_t channel = commands.items[from].index;
    for (uint32_t i = from + 1; i < commands.items.size(); i++)
    {
        const Command& command = commands.items[i];
        if ((command.kind == Kind::kOther && command.encoding.opcode == ntdlib::krawall::kPatternOpcode) ||
            (command.kind == Kind::kNote && command.index == channel && !SlidesOnly(commands, i)))
        {
            break;
        }

        if (command.index == channel && command.kind != Kind::kWait && StartsNote(command, instruments))
        {
            echoes.push_back(i);
        }
    }

    return echoes;
}

// `after` without the items of `removed`. A row that loses its last item joins the wait before it. The decoder reads a
// run of empty rows the same way. The places that name the items move with them.
Edited WithoutItems(const format::File& file, uint32_t sequence, Sequence after, std::set<uint32_t> removed)
{
    if (removed.empty())
    {
        return InPlace(file, sequence, std::move(after));
    }

    std::optional<uint32_t> wait;
    bool kept = false;
    for (uint32_t item = 0; item < after.items.size(); item++)
    {
        Command& command = after.items[item];
        if (removed.contains(item))
        {
            continue;
        }

        if (command.kind == Kind::kWait && wait && !kept)
        {
            after.items[*wait].value.value += command.value.value;
            removed.insert(item);
            continue;
        }

        if (command.kind == Kind::kWait)
        {
            wait = item;
            kept = false;
        }
        else if (command.kind == Kind::kOther && command.encoding.opcode == ntdlib::krawall::kPatternOpcode)
        {
            wait.reset();
            kept = false;
        }
        else
        {
            kept = true;
        }
    }

    std::vector<uint32_t> moved(after.items.size() + 1);
    uint32_t count = 0;
    for (uint32_t item = 0; item <= after.items.size(); item++)
    {
        moved[item] = count;
        count += removed.contains(item) ? 0 : 1;
    }

    Sequence result;
    for (uint32_t item = 0; item < after.items.size(); item++)
    {
        if (!removed.contains(item))
        {
            result.items.push_back(after.items[item]);
        }
    }

    result.labels = after.labels;
    for (ntdlib::commands::Label& label : result.labels)
    {
        label.item = moved.at(label.item);
    }

    Edited edited{file, {}, {}};
    edited.file.sequences[sequence] = std::move(result);
    MoveItemPlaces(edited.file, sequence, [&moved](uint32_t item) { return moved.at(item); });
    for (uint32_t item = 0; item < after.items.size(); item++)
    {
        edited.items.push_back(removed.contains(item) ? std::nullopt : std::optional<uint32_t>(moved[item]));
    }

    return edited;
}

// The field of a row's entry that `command` fills: 0 for the note or the instrument, 1 for the volume and 2 for the
// effect. Nothing for an item that no entry contains.
std::optional<int> FieldOf(const Command& command)
{
    switch (command.kind)
    {
    case Kind::kNote:
    case Kind::kProgram:
    case Kind::kNoteOff:
        return 0;
    case Kind::kVolume:
        return 1;
    default:
        break;
    }

    return EffectOf(command) ? std::optional<int>(2) : std::nullopt;
}

// Writes into `order` the items of the row [begin, end) of `commands` in the order of `channels`, each channel's items
// in the order note, volume and effect (JoinedEntries). Throws std::invalid_argument with a message for the user where
// the row contains two items of one field of a channel.
void OrderRow(const Sequence& commands, uint32_t begin, uint32_t end, const std::set<uint8_t>& channels,
              std::vector<uint32_t>& order)
{
    uint32_t next = begin;
    for (const uint8_t channel : channels)
    {
        std::array<std::optional<uint32_t>, 3> fields;
        for (uint32_t item = begin; item < end; item++)
        {
            const Command& command = commands.items[item];
            if (command.index != channel)
            {
                continue;
            }

            std::optional<uint32_t>& field = fields[static_cast<std::size_t>(*FieldOf(command))];
            if (field)
            {
                const int number = *FieldOf(command);
                throw std::invalid_argument(std::string("This channel's entry in that row already has ") +
                                            (number == 0   ? "a note or an instrument"
                                             : number == 1 ? "a volume"
                                                           : "an effect") +
                                            ". Move the setting to another row.");
            }

            field = item;
        }

        for (const std::optional<uint32_t>& field : fields)
        {
            if (field)
            {
                order[next++] = *field;
            }
        }
    }
}

// Writes into `order` the items of each row of `commands` that JoinedEntries rearranges, in their new order. Returns
// whether it rearranged a row.
bool JoinEntryRuns(const Sequence& commands, std::vector<uint32_t>& order)
{
    bool joined = false;
    uint32_t begin = 0;
    for (uint32_t end = 0; end <= commands.items.size(); end++)
    {
        if (end < commands.items.size() && FieldOf(commands.items[end]))
        {
            continue;
        }

        // The row [begin, end): its channels, and whether one of them has two entries or follows a higher channel.
        std::set<uint8_t> channels;
        bool rearrange = false;
        for (uint32_t item = begin; item < end; item++)
        {
            const uint8_t channel = commands.items[item].index;
            const bool seen = !channels.insert(channel).second;
            if (item > begin)
            {
                const Command& last = commands.items[item - 1];
                rearrange = rearrange || (last.index != channel && (seen || last.index > channel)) ||
                            (last.index == channel && *FieldOf(last) >= *FieldOf(commands.items[item]));
            }
        }
        if (rearrange)
        {
            OrderRow(commands, begin, end, channels, order);
            joined = true;
        }

        begin = end + 1;
    }

    return joined;
}

// `edited` with each channel's items of a row in one entry, in the order note, volume and effect. A setting that moves
// or comes in at a row is inserted before the row's first note, or at the row's end (EditNotesIn). It would then make a
// second entry of its channel. The player plays each entry in turn, and each entry sets the channel's effect anew
// (Player::PlayEntry). A volume before a note's instrument would also give way to the instrument's volume. The entries
// of such a row then go in the order of their channels, the order of a module's rows. So do those of a row where an
// entry follows one of a higher channel. Throws std::invalid_argument with a message for the user where a row contains
// two items of one field of a channel.
Edited JoinedEntries(Edited edited, uint32_t sequence)
{
    const Sequence& commands = *edited.file.sequences[sequence];
    std::vector<uint32_t> order(commands.items.size());
    for (uint32_t item = 0; item < order.size(); item++)
    {
        order[item] = item;
    }

    const bool joined = JoinEntryRuns(commands, order);
    if (!joined)
    {
        return edited;
    }

    // The items in their new order. `placed` contains the new place of each item, and of the end.
    std::vector<uint32_t> placed(commands.items.size() + 1);
    Sequence result;
    for (uint32_t at = 0; at < order.size(); at++)
    {
        placed[order[at]] = at;
        result.items.push_back(commands.items[order[at]]);
    }

    placed.back() = static_cast<uint32_t>(commands.items.size());
    result.labels = commands.labels;
    for (ntdlib::commands::Label& label : result.labels)
    {
        label.item = placed.at(label.item);
    }

    edited.file.sequences[sequence] = std::move(result);
    MoveItemPlaces(edited.file, sequence, [&placed](uint32_t item) { return placed.at(item); });
    for (std::optional<uint32_t>& item : edited.items)
    {
        item = item ? std::optional<uint32_t>(placed.at(*item)) : std::nullopt;
    }

    for (uint32_t& item : edited.added)
    {
        item = placed.at(item);
    }

    return edited;
}

// Applies `edit` to its note in `after`, a copy of `before` that the edits change (KrawallEditNotes). `offsets`
// contains the offsets of the items in `before`, and `module` the module header of the note. `instruments` is true for
// a module with instruments. `edited` gains the note's item, and `removals` the item of a removed note. A second edit
// of a note does nothing. Throws std::invalid_argument with a message for the user for an edit that the driver can't
// play.
void EditKrawallNote(const Sequence& before, const std::vector<uint32_t>& offsets,
                     const std::optional<ModuleHeader>& module, bool instruments, Sequence& after,
                     std::set<uint32_t>& edited, std::vector<uint32_t>& removals, const NoteEdit& edit)
{
    const std::optional<uint32_t> item = ItemAtOffset(offsets, edit.command);
    if (!item)
    {
        throw std::invalid_argument("The note's command isn't where the Key Editor shows it.");
    }
    if (before.items[*item].kind != Kind::kNote)
    {
        throw std::invalid_argument("This note comes from a row effect. Edit the effect in the List Editor.");
    }
    if (!edited.insert(*item).second)
    {
        return;
    }

    Command& note = after.items[*item];
    if (edit.remove)
    {
        if (!module)
        {
            throw std::invalid_argument("Can't find the header of this note's module.");
        }

        // In a module without instruments, the note off keeps the note's instrument. The channel's later notes
        // without one then play its sample and volume as before. In a module with instruments, a note off's
        // instrument reads no sample (Player::PlayEntry). The note off has none, and a later note of the channel
        // without one would play the instrument before.
        MakeNoteOff(note, !instruments);
        removals.push_back(*item);
        return;
    }

    if (edit.velocity && *edit.velocity != 127)
    {
        throw std::invalid_argument("Krawall's notes have no velocity. Change the row's volume in the List Editor.");
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
        const int key = note.key + edit.transpose;
        if (key < 1 || key > ntdlib::krawall::kInstrumentNotes)
        {
            throw std::invalid_argument("Krawall's notes run from C-0 to B-7. Transpose this note by less.");
        }

        note.key = static_cast<uint8_t>(key);
    }
}

Edited KrawallEditNotes(const format::File& file, uint32_t sequence, std::span<const NoteEdit> edits,
                        std::span<const NewNote> additions)
{
    if (!additions.empty())
    {
        throw std::invalid_argument(
            "Can't draw a new note in a module of Krawall. Add it to a row in the List Editor: "
            "Edit > Open List Editor.");
    }

    const Sequence& before = SequenceToEdit(file, sequence);
    const std::vector<uint32_t> offsets = Offsets(before, ConsoleOf(file));
    const std::optional<ModuleHeader> module = HeaderOf(file, sequence);
    const bool instruments = !module || module->instruments;
    Sequence after = before;
    std::set<uint32_t> edited;
    std::vector<uint32_t> removals;
    std::set<uint32_t> removed;
    for (const NoteEdit& edit : edits)
    {
        EditKrawallNote(before, offsets, module, instruments, after, edited, removals, edit);
    }

    // The removed notes' instruments and their echoes, once every edit is in.
    for (const uint32_t item : removals)
    {
        if (instruments && before.items[item].sample.value_or(0) != 0 &&
            NextNoteLacksInstrument(after, *module, item, before.items[item].index))
        {
            throw std::invalid_argument(
                "Can't remove this note. A later note of its channel has no instrument and plays this note's "
                "instrument. Give that note an instrument in the List Editor first.");
        }

        for (const uint32_t echo : EchoesOf(after, item, instruments))
        {
            removed.insert(echo);
        }
    }

    return WritableByOffsets(file, WithoutItems(file, sequence, std::move(after), removed), sequence);
}

// The edits on Krawall. Where the defaults of DriverEdits apply:
// - A row's notes don't wait: a wait ends the row.
// - The driver has no loops.
class KrawallDriverEdits final : public FixedTracksEdits
{
public:
    KrawallDriverEdits()
        : FixedTracksEdits(kKeepsTracks,
                           "A module of Krawall keeps the channels of its header. You can't paste a track into one.")
    {
    }

    Edited Writable(const format::File& before, Edited edited, uint32_t sequence) const override
    {
        return WritableByOffsets(before, std::move(edited), sequence);
    }

    // A row contains one entry of each channel.
    Edited Placed(const format::File&, Edited edited, uint32_t sequence) const override
    {
        return JoinedEntries(std::move(edited), sequence);
    }

    Edited EditNotes(const format::File& file, uint32_t sequence, std::span<const NoteEdit> edits,
                     std::span<const NewNote> additions) const override
    {
        return KrawallEditNotes(file, sequence, edits, additions);
    }

    // The bytes of an entry's fields: the volume column's byte, and an instrument of up to 16 bits.
    std::optional<ValueRange> Range(const song::Console& console, int /*track*/, const Command& command, Source source,
                                    bool time) const override
    {
        if (time || source != Source::kValue)
        {
            return std::nullopt;
        }

        switch (command.kind)
        {
        case Kind::kVolume:
            return ValueRange{0, 0xFF * kOne};
        case Kind::kProgram:
            return ValueRange{1 * kOne, (console.driver->Revision() != 0 ? 0xFFFF : 0x1FF) * kOne};
        default:
            break;
        }

        return std::nullopt;
    }

    // The player plays each row for the ticks of the module's speed, at least 1: no run of rows plays in no time.
    std::size_t LoopsWithoutWaits(const Sequence&, std::span<const uint32_t>) const override
    {
        return 0;
    }

    // The channel's notes become note offs, in every pattern of the module.
    format::File RemoveTrack(const format::File& file, std::span<const std::optional<SoundPlace>> places,
                             uint32_t sound, int track) const override
    {
        const uint32_t sequence = PlaceToEdit(places, sound).sequence;
        Sequence after = SequenceToEdit(file, sequence);
        bool found = false;
        for (Command& command : after.items)
        {
            if (command.kind == Kind::kNote && command.index == track)
            {
                MakeNoteOff(command, false);
                found = true;
            }
        }
        if (!found)
        {
            throw std::invalid_argument("This track plays no notes.");
        }

        return WritableByOffsets(file, InPlace(file, sequence, std::move(after)), sequence).file;
    }

    void CheckMixable(const song::Console& /*console*/, int /*track*/, song::Mix /*setting*/) const override
    {
        // A channel's volume comes from each note's sample and the volume column of its rows.
        throw std::invalid_argument(
            "Krawall sets a channel's volume and pan with each note and each row. Change the "
            "rows' volumes in the List Editor: Edit > Open List Editor.");
    }

    // A setting moves by rows: to the row that plays at its new tick.
    int32_t ShiftOfSetting(std::span<const song::Stretch> rows, const SettingMove& move) const override
    {
        if (move.shift == 0)
        {
            return move.shift;
        }

        const std::optional<int32_t> shift = KrawallRowsOfMove(rows, move.stretch, move.shift);
        if (!shift)
        {
            throw std::invalid_argument("A setting of Krawall stays in the rows of its pattern. Move it less far.");
        }

        return *shift;
    }

    // A note sits on its row, and a setting's rows give its room in the arrangement's ticks.
    void FitLimits(std::span<const NotePlace> notes, std::span<const Kind> kinds, std::span<const song::Stretch> rows,
                   std::vector<NoteLimits>& limits) const override
    {
        for (std::size_t i = 0; i < notes.size(); i++)
        {
            if (kinds[i] == Kind::kNote)
            {
                limits[i] = {.fixed = kNoteStays, .fixed_length = kNoteKeepsLength};
            }
            else if (limits[i].shift)
            {
                limits[i].shift = KrawallTicksOfRows(rows, notes[i].stretch, *limits[i].shift);
            }
        }
    }

    // Each note names its instrument. The entry of the note in its row contains the instrument.
    void CheckInstruments() const override
    {
        throw std::invalid_argument(
            "Each note of Krawall names its instrument. Change the notes' instruments in the List Editor: Edit > Open "
            "List Editor.");
    }
};

} // namespace

const DriverEdits& KrawallEdits()
{
    static const KrawallDriverEdits edits;
    return edits;
}

bool IsKrawall(const song::Console& console)
{
    return RunsDriver(console, format::Engine::kKrawall);
}

namespace
{

// The place of `stretch` among `rows`.
std::optional<std::size_t> RowOf(std::span<const song::Stretch> rows, const song::Stretch& stretch)
{
    const auto found = std::ranges::find(rows, stretch);
    if (found == rows.end())
    {
        return std::nullopt;
    }

    return static_cast<std::size_t>(found - rows.begin());
}

// Whether row `row` of `rows` follows on from the row before it in its pattern.
bool FollowsOn(std::span<const song::Stretch> rows, std::size_t row)
{
    return row > 0 && row < rows.size() && rows[row].entry == ntdlib::sequence_export::Entry::kOpen;
}

} // namespace

std::optional<int32_t> KrawallRowsOfMove(std::span<const song::Stretch> rows, const song::Stretch& stretch,
                                         int32_t shift)
{
    const std::optional<std::size_t> from = RowOf(rows, stretch);
    if (!from)
    {
        return std::nullopt;
    }

    const int64_t tick = int64_t{stretch.tick} + shift;
    std::size_t to = *from;
    if (shift >= 0)
    {
        while (FollowsOn(rows, to + 1) && rows[to + 1].tick <= tick)
        {
            to++;
        }
        if (to + 1 < rows.size() && rows[to + 1].tick <= tick)
        {
            return std::nullopt;
        }
    }
    else
    {
        while (FollowsOn(rows, to) && rows[to].tick > tick)
        {
            to--;
        }
        if (rows[to].tick > tick)
        {
            return std::nullopt;
        }
    }

    return static_cast<int32_t>(static_cast<int64_t>(to) - static_cast<int64_t>(*from));
}

std::optional<ShiftRange> KrawallTicksOfRows(std::span<const song::Stretch> rows, const song::Stretch& stretch,
                                             const ShiftRange& range)
{
    const std::optional<std::size_t> from = RowOf(rows, stretch);
    if (!from)
    {
        return std::nullopt;
    }

    std::size_t earliest = *from;
    for (uint32_t row = 0; row < range.earlier && FollowsOn(rows, earliest); row++)
    {
        earliest--;
    }

    std::size_t latest = *from;
    for (uint32_t row = 0; row < range.later && FollowsOn(rows, latest + 1); row++)
    {
        latest++;
    }

    return ShiftRange{rows[*from].tick - rows[earliest].tick, rows[latest].tick - rows[*from].tick};
}

} // namespace hokuto::edit
