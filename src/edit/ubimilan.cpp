// SPDX-License-Identifier: MIT

#include "edit/ubimilan.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/commands/units.h"
#include "ntdlib/ubimilan/commands.h"
#include "ntdlib/ubimilan/driver.h"

#include "edit/bytecode.h"
#include "edit/drivers.h"
#include "edit/event_timelines.h"
#include "edit/notes.h"
#include "edit/replace.h"
#include "edit/tracks.h"
#include "format/ubimilan.h"
#include "song/driver.h"
#include "song/ubimilan.h"

namespace hokuto::edit
{

namespace
{

using event_timelines::Event;
using event_timelines::kInPlace;
using event_timelines::kMovedNote;
using event_timelines::kMovedOff;
using event_timelines::MoveNote;
using event_timelines::SortedEvents;
using event_timelines::TicksOf;
using event_timelines::WaitOf;
using ntdlib::commands::Command;
using ntdlib::commands::Kind;
using ntdlib::commands::kOne;
using ntdlib::commands::Sequence;
using ntdlib::commands::Source;
using ntdlib::ubimilan::kKitChannel;
using ntdlib::ubimilan::kPsgChannels;
using ntdlib::ubimilan::kWaveChannel;
using ntdlib::ubimilan::PlaysKit;

// The channels of a sequence, and the highest key of a note.
constexpr int kChannels = 16;
constexpr int kHighestKey = 127;

// The commands of a sequence at their ticks from the start, and the tick of the end.
struct Timeline
{
    std::vector<Event> events; // in the order of the items before the edit, then the new ones
    int64_t end = 0;
    uint32_t end_item = 0;
    uint64_t next_order = 0; // the order of the next new command
};

Command ChannelCommand(Kind kind, int channel)
{
    Command command;
    command.kind = kind;
    command.index = static_cast<uint8_t>(channel);
    return command;
}

// Whether `command` names a channel: a note, a note-off, a program change or a command without an effect.
bool NamesChannel(const Command& command)
{
    return command.kind == Kind::kNote || command.kind == Kind::kNoteOff || command.kind == Kind::kProgram ||
           command.kind == Kind::kOther;
}

// The commands of `sequence` at their ticks. Throws std::invalid_argument with a message for the user for a sequence
// that doesn't start the way the driver's sequences do, or that has no end.
Timeline TimelineOf(const Sequence& sequence)
{
    if (sequence.items.size() < 3 || sequence.items[0].kind != Kind::kData || sequence.items[1].kind != Kind::kWait)
    {
        throw std::invalid_argument("Can't edit this sequence. It doesn't start the way the driver's sequences do.");
    }

    Timeline timeline;
    int64_t tick = TicksOf(sequence.items[1]);
    for (uint32_t item = 2; item < sequence.items.size(); item++)
    {
        const Command& command = sequence.items[item];
        if (command.kind == Kind::kWait)
        {
            tick += TicksOf(command);
            continue;
        }

        if (command.kind == Kind::kEnd)
        {
            timeline.end = tick;
            timeline.end_item = item;
            timeline.next_order = 2 * uint64_t{sequence.items.size()};
            return timeline;
        }

        timeline.events.push_back(Event{tick, kInPlace, 2 * uint64_t{item}, command, item});
    }

    throw std::invalid_argument("Can't edit this sequence. It has no end command.");
}

// The event of the note-off that ends the note of event `on`. Nothing where a later note ends the note, or uses up the
// note-off. A note on a PSG channel ends at the next note on the channel. A note of the kit ends at any note-off of the
// kit keys, and another note of the kit uses up that note-off. Any other note ends at a note-off of the same key.
std::optional<std::size_t> NoteOffOf(const std::vector<Event>& events, std::size_t on)
{
    const Command& note = events[on].command;
    const int channel = note.index;
    const bool kit = PlaysKit(channel, note.key);
    const auto same = [channel, kit, &note](const Command& other)
    {
        return kit ? PlaysKit(channel, other.key) : other.key == note.key;
    };
    for (std::size_t i = on + 1; i < events.size(); i++)
    {
        const Command& other = events[i].command;
        if (events[i].removed || other.index != channel)
        {
            continue;
        }
        if (other.kind == Kind::kNote && (channel < kPsgChannels || same(other)))
        {
            return std::nullopt;
        }
        if (other.kind == Kind::kNoteOff && same(other))
        {
            return i;
        }
    }

    return std::nullopt;
}

// `edited` as the driver's bytes store it, with a wait before each command and no two waits in a row. `edited` is an
// edit of sequence `sequence`. Throws std::invalid_argument with a message for the user for an edit that the bytes
// can't store.
Edited UbiMilanWritable(const format::File&, Edited edited, uint32_t sequence)
{
    const Sequence& changed = *edited.file.sequences[sequence];
    if (changed.items.size() < 2 || changed.items[0].kind != Kind::kData)
    {
        return edited;
    }

    Sequence after = changed;
    after.items.clear();
    std::vector<uint32_t> moved(changed.items.size());
    bool ended = false;
    for (std::size_t item = 0; item < changed.items.size(); item++)
    {
        const Command& command = changed.items[item];
        const bool first = item == 0;
        if (!ended && command.kind == Kind::kWait && !after.items.empty() && after.items.back().kind == Kind::kWait)
        {
            after.items.back().value.value += command.value.value;
            moved[item] = static_cast<uint32_t>(after.items.size() - 1);
            continue;
        }

        if (!ended && !first && command.kind != Kind::kWait && after.items.back().kind != Kind::kWait)
        {
            after.items.push_back(WaitOf(0));
        }

        moved[item] = static_cast<uint32_t>(after.items.size());
        after.items.push_back(command);
        ended = ended || command.kind == Kind::kEnd;
    }

    for (ntdlib::commands::Label& label : after.labels)
    {
        label.item = label.item < moved.size() ? moved[label.item] : static_cast<uint32_t>(after.items.size());
    }

    const auto encode = [&]
    {
        ntdlib::ubimilan::EncodeSequence(format::UbiMilanCountdowns(after));
    };
    CheckEncodes("Can't make this edit: ", encode);

    for (std::optional<uint32_t>& item : edited.items)
    {
        if (item && *item < moved.size())
        {
            item = moved[*item];
        }
    }

    for (uint32_t& item : edited.added)
    {
        item = item < moved.size() ? moved[item] : item;
    }

    edited.file.sequences[sequence] = std::move(after);

    return edited;
}

// `file` with the commands of `timeline` at their ticks in sequence `sequence`, `before`. Throws std::invalid_argument
// with a message for the user for a command past the end of the sequence, and for commands one tick apart.
Edited Laid(const format::File& file, uint32_t sequence, const Sequence& before, const Timeline& timeline)
{
    const std::vector<const Event*> events = SortedEvents(timeline.events, timeline.end, "sequence");

    Edited edited;
    edited.items.assign(before.items.size(), std::nullopt);
    Sequence after;
    after.items.push_back(before.items[0]);
    edited.items[0] = 0;
    after.items.push_back(WaitOf(events.empty() ? timeline.end : events.front()->tick));
    edited.items[1] = 1;
    for (std::size_t i = 0; i < events.size(); i++)
    {
        const Event& event = *events[i];
        const auto index = static_cast<uint32_t>(after.items.size());
        after.items.push_back(event.command);
        if (event.old)
        {
            edited.items[*event.old] = index;
        }

        if (event.added && event.command.kind == Kind::kNote)
        {
            edited.added.push_back(index);
        }

        const int64_t next = i + 1 < events.size() ? events[i + 1]->tick : timeline.end;
        if (next - event.tick == 1)
        {
            throw std::invalid_argument("Commands at ticks " + std::to_string(event.tick) + " and " +
                                        std::to_string(next) +
                                        " would be one tick apart. The driver can't play that. Move one of them.");
        }

        if (event.old && *event.old + 1 < before.items.size() && before.items[*event.old + 1].kind == Kind::kWait)
        {
            edited.items[*event.old + 1] = static_cast<uint32_t>(after.items.size());
        }

        after.items.push_back(WaitOf(next - event.tick));
    }

    for (uint32_t item = timeline.end_item; item < before.items.size(); item++)
    {
        edited.items[item] = static_cast<uint32_t>(after.items.size());
        after.items.push_back(before.items[item]);
    }

    for (const ntdlib::commands::Label& label : before.labels)
    {
        if (label.item < edited.items.size() && edited.items[label.item])
        {
            after.labels.push_back(label);
            after.labels.back().item = *edited.items[label.item];
        }
    }

    edited.file = file;
    edited.file.sequences[sequence] = std::move(after);
    return UbiMilanWritable(file, std::move(edited), sequence);
}

// The velocity byte of a note command on `channel` at `key` for `velocity` in the Key Editor. The byte keeps the bits
// of `old` that don't set the level.
int64_t VelocityByte(int channel, int key, int velocity, const Command& old)
{
    const auto byte = static_cast<int>(old.velocity / kOne);
    return int64_t{song::UbiMilanDriverVelocity(channel, key, velocity, byte)} * kOne;
}

// The event of the note-on and of the note-off, where it has one, of the note of each of `edits` in `timeline`, the
// timeline of `before` (UbiMilanEditNotes). `offsets` contains the offsets of the items. Throws std::invalid_argument
// with a message for the user for an edit that names no note.
std::vector<std::pair<std::size_t, std::optional<std::size_t>>> NoteOffs(const Sequence& before,
                                                                         const std::vector<uint32_t>& offsets,
                                                                         const Timeline& timeline,
                                                                         std::span<const NoteEdit> edits)
{
    std::map<uint32_t, std::size_t> events_of_items;
    for (std::size_t i = 0; i < timeline.events.size(); i++)
    {
        events_of_items.emplace(*timeline.events[i].old, i);
    }

    std::vector<std::pair<std::size_t, std::optional<std::size_t>>> notes;
    for (const NoteEdit& edit : edits)
    {
        const std::optional<uint32_t> item = ItemAtOffset(offsets, edit.command);
        if (!item || !events_of_items.contains(*item) || before.items[*item].kind != Kind::kNote)
        {
            throw std::invalid_argument("Can't find the command of this note.");
        }

        const std::size_t on = events_of_items.at(*item);
        notes.emplace_back(on, NoteOffOf(timeline.events, on));
    }

    return notes;
}

// Applies `edit` to its note in `timeline`: the note-on at event `on` and the note-off at event `off`
// (UbiMilanEditNotes). Throws std::invalid_argument with a message for the user for an edit that the driver can't play.
void EditUbiNote(Timeline& timeline, const NoteEdit& edit, std::size_t on, std::optional<std::size_t> off)
{
    Event& note = timeline.events[on];
    if (edit.remove)
    {
        note.removed = true;
        if (off)
        {
            timeline.events[*off].removed = true;
        }

        return;
    }

    const int channel = note.command.index;
    if (edit.transpose != 0)
    {
        const int key = note.command.key + edit.transpose;
        if (key < 0 || key > kHighestKey)
        {
            throw std::invalid_argument("Keys run from 0 to " + std::to_string(kHighestKey) + ".");
        }
        if (PlaysKit(channel, note.command.key) != PlaysKit(channel, key))
        {
            throw std::invalid_argument(
                "On channel 9, keys up to 11 play the noise and higher keys the kit. "
                "A transpose can't move a note between the two.");
        }

        if (off && !PlaysKit(channel, key))
        {
            timeline.events[*off].command.key = static_cast<uint8_t>(key);
        }

        note.command.key = static_cast<uint8_t>(key);
    }

    if (edit.velocity)
    {
        note.command.velocity = VelocityByte(channel, note.command.key, *edit.velocity, note.command);
    }

    MoveNote(edit, note, off ? &timeline.events[*off] : nullptr,
             "This note has no note-off. You can't move it or change its length.");
}

// Adds `addition` to `timeline` (UbiMilanEditNotes). Throws std::invalid_argument with a message for the user for a
// note that the driver can't play.
void AddUbiNote(Timeline& timeline, const NewNote& addition)
{
    const int channel = addition.index;
    if (channel >= kChannels)
    {
        throw std::invalid_argument("The driver has 16 channels.");
    }
    if (addition.key < 0 || addition.key > kHighestKey)
    {
        throw std::invalid_argument("Keys run from 0 to " + std::to_string(kHighestKey) + ".");
    }
    if (addition.length < 1)
    {
        throw std::invalid_argument("A note lasts at least one tick.");
    }

    Command note = ChannelCommand(Kind::kNote, channel);
    note.key = static_cast<uint8_t>(addition.key);
    note.velocity = VelocityByte(channel, addition.key, addition.velocity, Command{});
    Command note_off = ChannelCommand(Kind::kNoteOff, channel);
    note_off.key = note.key;
    const int64_t tick = int64_t{addition.tick} - int64_t{addition.stretch.tick};
    Event& on = timeline.events.emplace_back(Event{tick, kMovedNote, timeline.next_order++, note, std::nullopt});
    on.added = true;
    timeline.events.push_back(
        Event{tick + int64_t{addition.length}, kMovedOff, timeline.next_order++, note_off, std::nullopt});
}

Edited UbiMilanEditNotes(const format::File& file, uint32_t sequence, std::span<const NoteEdit> edits,
                         std::span<const NewNote> additions)
{
    const Sequence& before = SequenceToEdit(file, sequence);
    const std::vector<uint32_t> offsets = Offsets(before, ConsoleOf(file));
    Timeline timeline = TimelineOf(before);

    // The note-off of each edited note, before the edits.
    const std::vector<std::pair<std::size_t, std::optional<std::size_t>>> notes =
        NoteOffs(before, offsets, timeline, edits);
    for (std::size_t n = 0; n < edits.size(); n++)
    {
        const auto [on, off] = notes[n];
        EditUbiNote(timeline, edits[n], on, off);
    }

    for (const NewNote& addition : additions)
    {
        AddUbiNote(timeline, addition);
    }

    return Laid(file, sequence, before, timeline);
}

// The channels that the commands of `sequence` name.
std::set<int> UsedChannels(const Sequence& sequence)
{
    std::set<int> used;
    for (const Command& command : sequence.items)
    {
        if (NamesChannel(command))
        {
            used.insert(command.index);
        }
    }

    return used;
}

// The free channel for a copy of the commands of channel `channel`: the same channel where it is free, or else the
// other square for a square. Throws std::invalid_argument with a message for the user when no channel is free.
int CopyChannel(const std::set<int>& used, int channel)
{
    if (channel == 0 || channel == 1)
    {
        const int other = 1 - channel;
        for (const int candidate : {channel, other})
        {
            if (!used.contains(candidate))
            {
                return candidate;
            }
        }

        throw std::invalid_argument("Both square channels are already in use.");
    }

    if (channel != kWaveChannel && channel != kKitChannel)
    {
        throw std::invalid_argument("This channel makes no sound. Only channels 0 to 2 and 9 do.");
    }
    if (used.contains(channel))
    {
        throw std::invalid_argument(channel == kKitChannel ? "The kit channel is already in use."
                                                           : "The wave channel is already in use.");
    }

    return channel;
}

TrackEdit WithChannelCopy(const format::File& file, uint32_t sequence, int from, int to)
{
    const Sequence& before = SequenceToEdit(file, sequence);
    Timeline timeline = TimelineOf(before);
    const std::size_t count = timeline.events.size();
    for (std::size_t i = 0; i < count; i++)
    {
        const Event event = timeline.events[i];
        if (NamesChannel(event.command) && event.command.index == from)
        {
            Event copy{event.tick, kInPlace, event.order + 1, event.command, std::nullopt};
            copy.command.index = static_cast<uint8_t>(to);
            timeline.events.push_back(copy);
        }
    }

    return {Laid(file, sequence, before, timeline).file, to};
}

// The edits on Ubi Milan's driver. Where the defaults of DriverEdits apply:
// - The bytes of the driver keep nothing else around an edit.
// - The field of the first sequence of a piece points to the one track of commands.
// - No command opens a channel.
// - The driver has no jumps.
// - The driver has no loops.
class UbiMilanDriverEdits final : public DriverEdits
{
public:
    Edited Writable(const format::File& before, Edited edited, uint32_t sequence) const override
    {
        return UbiMilanWritable(before, std::move(edited), sequence);
    }

    Edited EditNotes(const format::File& file, uint32_t sequence, std::span<const NoteEdit> edits,
                     std::span<const NewNote> additions) const override
    {
        return UbiMilanEditNotes(file, sequence, edits, additions);
    }

    // A wait lasts 0 ticks or 2 to 65,536, and the first wait up to 65,535 (format/ubimilan.h). A program is a byte.
    // The bytes of a note and a note-off store no length, and no command uses a time, a random value or a variable.
    std::optional<ValueRange> Range(const song::Console&, int /*track*/, const Command& command, Source source,
                                    bool time) const override
    {
        if (time || source != Source::kValue)
        {
            return std::nullopt;
        }

        switch (command.kind)
        {
        case Kind::kWait:
            return ValueRange{0, (format::kUbiMilanLongestCountdown + 1) * kOne};
        case Kind::kProgram:
            return ValueRange{0, 0xFF * kOne};
        default:
            break;
        }

        return std::nullopt;
    }

    // A piece plays its list of sequences again, and no command jumps.
    std::size_t LoopsWithoutWaits(const Sequence&, std::span<const uint32_t>) const override
    {
        return 0;
    }

    // A new track uses the first free channel of 0, 1, 2 and 9. A program change is added at the tick of the first
    // command of the sequence.
    TrackEdit AddTrack(const format::File& file, std::span<const std::optional<SoundPlace>> places, uint32_t sound,
                       const song::Sequence&, std::optional<Program> instrument) const override
    {
        const uint32_t sequence = PlaceToEdit(places, sound).sequence;
        const Sequence& before = SequenceToEdit(file, sequence);
        const std::set<int> used = UsedChannels(before);
        std::optional<int> channel;
        for (const int candidate : {0, 1, kWaveChannel, kKitChannel})
        {
            if (!used.contains(candidate))
            {
                channel = candidate;
                break;
            }
        }
        if (!channel)
        {
            throw std::invalid_argument("Every channel that makes sound is already in use.");
        }

        Timeline timeline = TimelineOf(before);
        const Command program = song::ValueCommand(
            Kind::kProgram, instrument ? std::clamp(instrument->number, 0, 0xFF) : 0, static_cast<uint8_t>(*channel));
        const int64_t tick = timeline.events.empty() ? timeline.end : timeline.events.front().tick;
        timeline.events.push_back(Event{tick, kMovedOff, timeline.next_order++, program, std::nullopt});

        return {Laid(file, sequence, before, timeline).file, *channel};
    }

    TrackEdit DuplicateTrack(const format::File& file, std::span<const std::optional<SoundPlace>> places,
                             uint32_t sound, int track) const override
    {
        const uint32_t sequence = PlaceToEdit(places, sound).sequence;
        const std::set<int> used = UsedChannels(SequenceToEdit(file, sequence));
        if (!used.contains(track))
        {
            throw std::invalid_argument("This track has no commands.");
        }

        return WithChannelCopy(file, sequence, track, CopyChannel(used, track));
    }

    // The commands of the channel, each after a wait that sets its tick from the start of the sequence.
    CopiedTrack CopyTrack(const format::File& file, std::span<const std::optional<SoundPlace>> places, uint32_t sound,
                          const song::Sequence& song, int track) const override
    {
        const uint32_t sequence = PlaceToEdit(places, sound).sequence;
        const Timeline timeline = TimelineOf(SequenceToEdit(file, sequence));
        CopiedTrack copied;
        int64_t tick = 0;
        for (const Event& event : timeline.events)
        {
            if (NamesChannel(event.command) && event.command.index == track)
            {
                copied.commands.push_back(WaitOf(event.tick - tick));
                copied.commands.push_back(event.command);
                tick = event.tick;
            }
        }
        if (copied.commands.empty())
        {
            throw std::invalid_argument("This track has no commands.");
        }

        copied.timebase = song.timebase;
        copied.sound = sound;
        copied.track = track;
        copied.driver = format::Engine::kUbiMilan;

        return copied;
    }

    TrackEdit PasteTrack(const format::File& file, std::span<const std::optional<SoundPlace>> places, uint32_t sound,
                         const song::Sequence&, const CopiedTrack& copied) const override
    {
        if (copied.driver != format::Engine::kUbiMilan)
        {
            throw std::invalid_argument("You can paste only a track of Ubisoft Milan's driver here.");
        }

        const uint32_t sequence = PlaceToEdit(places, sound).sequence;
        const Sequence& before = SequenceToEdit(file, sequence);
        const int channel = CopyChannel(UsedChannels(before), copied.track);

        Timeline timeline = TimelineOf(before);
        int64_t tick = 0;
        for (const Command& command : copied.commands)
        {
            if (command.kind == Kind::kWait)
            {
                tick += TicksOf(command);
                continue;
            }
            if (tick > timeline.end)
            {
                throw std::invalid_argument("The copied track runs past the end of the sequence (tick " +
                                            std::to_string(timeline.end) + ").");
            }

            Command pasted = command;
            pasted.index = static_cast<uint8_t>(channel);
            timeline.events.push_back(Event{tick, kInPlace, timeline.next_order++, pasted, std::nullopt});
        }

        return {Laid(file, sequence, before, timeline).file, channel};
    }

    format::File RemoveTrack(const format::File& file, std::span<const std::optional<SoundPlace>> places,
                             uint32_t sound, int track) const override
    {
        const uint32_t sequence = PlaceToEdit(places, sound).sequence;
        const Sequence& before = SequenceToEdit(file, sequence);
        Timeline timeline = TimelineOf(before);
        bool found = false;
        for (Event& event : timeline.events)
        {
            if (NamesChannel(event.command) && event.command.index == track)
            {
                event.removed = true;
                found = true;
            }
        }
        if (!found)
        {
            throw std::invalid_argument("This track has no commands.");
        }

        return Laid(file, sequence, before, timeline).file;
    }

    std::string RefusedPan(const song::DriverRules& /*rules*/) const override
    {
        return "Tracks of Ubisoft Milan's driver have no pan.";
    }

    // A note's command names the track's channel.
    std::optional<uint8_t> NewNoteIndex(int track) const override
    {
        return static_cast<uint8_t>(track);
    }
};

} // namespace

const DriverEdits& UbiMilanEdits()
{
    static const UbiMilanDriverEdits edits;
    return edits;
}

bool IsUbiMilan(const song::Console& console)
{
    return RunsDriver(console, format::Engine::kUbiMilan);
}

} // namespace hokuto::edit
