// SPDX-License-Identifier: MIT

#include "edit/rare.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
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
#include "ntdlib/rare/commands.h"
#include "ntdlib/rare/sequencer.h"

#include "edit/bytecode.h"
#include "edit/drivers.h"
#include "edit/event_timelines.h"
#include "edit/notes.h"
#include "edit/replace.h"
#include "edit/tracks.h"
#include "format/rare.h"

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

// The channels of a tune, and the highest key and velocity of a note.
constexpr int kChannels = ntdlib::rare::kChannels;
constexpr int kHighestKey = 127;
constexpr int kMostVelocity = 127;

// The longest delay: 3 bytes.
constexpr int64_t kLongestDelay = 0xFFFFFF;

// Whether `command` names a channel.
bool NamesChannel(const Command& command)
{
    switch (command.kind)
    {
    case Kind::kNote:
    case Kind::kNoteOff:
    case Kind::kProgram:
    case Kind::kPitchBend:
    case Kind::kLfoDepth:
    case Kind::kVolume:
    case Kind::kAttack:
    case Kind::kDecay:
    case Kind::kSustain:
    case Kind::kRelease:
    case Kind::kLoopStart:
    case Kind::kLoopEnd:
    case Kind::kMonophonic:
        return true;
    case Kind::kOther:
        return command.encoding.opcode != ntdlib::rare::kCmdNop;
    default:
        return false;
    }
}

// The run of items of a track: from its start to its end, or to the end of its loop after the start of its loop.
struct Run
{
    uint32_t first = 0;
    uint32_t last = 0; // the end, or the loop's end
};

// The run of the track that starts at item `entry`, or nothing where the track runs into data or past the items.
std::optional<Run> RunFrom(const Sequence& sequence, uint32_t entry)
{
    bool looping = false;
    for (uint32_t item = entry; item < sequence.items.size(); item++)
    {
        const Command& command = sequence.items[item];
        if (command.kind == Kind::kData)
        {
            return std::nullopt;
        }

        looping = looping || command.kind == Kind::kLoopStart;
        if (command.kind == Kind::kEnd || (looping && command.kind == Kind::kLoopEnd))
        {
            return Run{entry, item};
        }
    }

    return std::nullopt;
}

// The items where the tracks of sequence `sequence` of `file` start, in their order: the fields of the tunes' track
// lists name them.
std::vector<uint32_t> EntriesOf(const format::File& file, uint32_t sequence)
{
    std::set<uint32_t> entries;
    if (file.native)
    {
        ForEachItemPlace(*file.native, sequence,
                         [&entries](const ntdlib::sar::Place& place) { entries.insert(place.offset); });
    }

    return {entries.begin(), entries.end()};
}

// The run that contains item `item`. Throws std::invalid_argument with a message for the user where no track runs it.
Run RunOfItem(const Sequence& sequence, std::span<const uint32_t> entries, uint32_t item)
{
    for (const uint32_t entry : entries)
    {
        const std::optional<Run> run = RunFrom(sequence, entry);
        if (run && run->first <= item && item <= run->last)
        {
            return *run;
        }
    }

    throw std::invalid_argument("Can't find the track of this note's command.");
}

// The commands of a run at their ticks from the track's start, and the tick of its end.
struct Timeline
{
    Run run;
    std::vector<Event> events; // in the order of the items before the edit, then the new ones
    int64_t end = 0;
    uint64_t next_order = 0;
};

Timeline TimelineOf(const Sequence& sequence, const Run& run)
{
    Timeline timeline;
    timeline.run = run;
    int64_t tick = 0;
    for (uint32_t item = run.first; item < run.last; item++)
    {
        const Command& command = sequence.items[item];
        if (command.kind == Kind::kWait)
        {
            tick += TicksOf(command);
            continue;
        }

        timeline.events.push_back(Event{tick, kInPlace, 2 * uint64_t{item}, command, item});
    }

    timeline.end = tick;
    timeline.next_order = 2 * uint64_t{sequence.items.size()};

    return timeline;
}

// The tick in `timeline` of item `item`, or of the item at which the commands after it go on.
int64_t TickOfItem(const Sequence& sequence, const Timeline& timeline, uint32_t item)
{
    int64_t tick = 0;
    for (uint32_t i = timeline.run.first; i < std::min(item, timeline.run.last); i++)
    {
        if (sequence.items[i].kind == Kind::kWait)
        {
            tick += TicksOf(sequence.items[i]);
        }
    }

    return tick;
}

// The event of the note-off that ends the note of event `on`: the first note-off of its key on its channel that no
// earlier note of the key uses. Nothing where the track has none.
std::optional<std::size_t> NoteOffOf(const std::vector<Event>& events, std::size_t on)
{
    const Command& note = events[on].command;
    int waiting = 0;
    for (std::size_t i = on + 1; i < events.size(); i++)
    {
        const Command& other = events[i].command;
        if (events[i].removed || other.index != note.index || other.key != note.key)
        {
            continue;
        }

        if (other.kind == Kind::kNote)
        {
            waiting++;
        }
        else if (other.kind == Kind::kNoteOff)
        {
            if (waiting == 0)
            {
                return i;
            }

            waiting--;
        }
    }

    return std::nullopt;
}

// Returns `edited`, an edit of sequence `sequence`, once the driver's bytes can hold it. Throws std::invalid_argument
// with a message for the user for an edit that the bytes can't hold, or that the driver would read otherwise.
Edited RareWritable(const format::File&, Edited edited, uint32_t sequence)
{
    const ntdlib::rare::Traits traits = format::RareTraits(edited.file.native->driver->revision);
    const auto encode = [&]
    {
        ntdlib::rare::EncodeSequence(*edited.file.sequences[sequence], EntriesOf(edited.file, sequence), traits);
    };
    CheckEncodes("Rare's driver can't hold this edit: ", encode);

    return edited;
}

// Lays the commands of `timeline` at their ticks after `after`'s items, then the item that ends the timeline's run
// (Laid). `before` is the sequence before the edit. `edited.items` gains the new place of each item of the run, and
// `edited.added` the new notes. `starts` gains the place where the run starts now. Throws std::invalid_argument with a
// message for the user for a command before the start or past the end of the track.
void LayTimeline(const Sequence& before, const Timeline& timeline, Edited& edited, std::map<uint32_t, uint32_t>& starts,
                 Sequence& after)
{
    const std::vector<const Event*> events = SortedEvents(timeline.events, timeline.end, "track");

    // A delay before each command that comes later than the one before it. A delay that keeps its ticks keeps its
    // bytes.
    const auto first_wait = static_cast<uint32_t>(after.items.size());
    int64_t tick = 0;
    const auto wait_until = [&](int64_t next, std::optional<uint32_t> old_before)
    {
        for (int64_t left = next - tick; left > 0;)
        {
            const int64_t ticks = std::min(left, kLongestDelay);
            Command wait = WaitOf(ticks);
            if (old_before && *old_before > timeline.run.first && before.items[*old_before - 1].kind == Kind::kWait &&
                TicksOf(before.items[*old_before - 1]) == ticks)
            {
                wait = before.items[*old_before - 1];
                edited.items[*old_before - 1] = static_cast<uint32_t>(after.items.size());
            }

            after.items.push_back(wait);
            left -= ticks;
        }

        tick = next;
    };
    for (const Event* event : events)
    {
        wait_until(event->tick, event->old);
        const auto index = static_cast<uint32_t>(after.items.size());
        after.items.push_back(event->command);
        if (event->old)
        {
            edited.items[*event->old] = index;
        }

        if (event->added && event->command.kind == Kind::kNote)
        {
            edited.added.push_back(index);
        }
    }

    wait_until(timeline.end, timeline.run.last);

    // The track's start names the first item of the run, a delay or a command.
    starts[timeline.run.first] = first_wait;
    for (uint32_t i = timeline.run.first; i < timeline.run.last; i++)
    {
        if (!edited.items[i] && before.items[i].kind == Kind::kWait)
        {
            edited.items[i] = static_cast<uint32_t>(after.items.size());
        }
    }

    edited.items[timeline.run.last] = static_cast<uint32_t>(after.items.size());
    after.items.push_back(before.items[timeline.run.last]);
}

// Adds to `after` each label of `before` whose item stays, at the item's new place in `items` (Laid).
void MoveLabels(const Sequence& before, const std::vector<std::optional<uint32_t>>& items, Sequence& after)
{
    for (const ntdlib::commands::Label& label : before.labels)
    {
        if (label.item < items.size() && items[label.item])
        {
            after.labels.push_back(label);
            after.labels.back().item = *items[label.item];
        }
    }
}

// `edited` with the commands of each timeline of `timelines` at their ticks in its sequence `sequence`, `before`. Each
// edit's item map starts from `edited.items`. Throws std::invalid_argument with a message for the user for a command
// past the end of its track.
Edited Laid(const format::File& file, uint32_t sequence, const Sequence& before, std::span<const Timeline> timelines)
{
    std::map<uint32_t, const Timeline*> by_first;
    for (const Timeline& timeline : timelines)
    {
        by_first.emplace(timeline.run.first, &timeline);
    }

    Edited edited;
    edited.items.assign(before.items.size(), std::nullopt);
    std::map<uint32_t, uint32_t> starts; // where each track's run starts now, by its first item before the edit
    Sequence after;
    for (uint32_t item = 0; item < before.items.size();)
    {
        const auto found = by_first.find(item);
        if (found == by_first.end())
        {
            edited.items[item] = static_cast<uint32_t>(after.items.size());
            after.items.push_back(before.items[item]);
            item++;
            continue;
        }

        const Timeline& timeline = *found->second;
        LayTimeline(before, timeline, edited, starts, after);
        item = timeline.run.last + 1;
    }

    MoveLabels(before, edited.items, after);

    edited.file = file;
    edited.file.sequences[sequence] = std::move(after);
    const auto moved = [&](uint32_t item)
    {
        if (const auto start = starts.find(item); start != starts.end())
        {
            return start->second;
        }

        for (uint32_t i = item; i < edited.items.size(); i++)
        {
            if (edited.items[i])
            {
                return *edited.items[i];
            }
        }

        return static_cast<uint32_t>(edited.file.sequences[sequence]->items.size());
    };
    MoveItemPlaces(edited.file, sequence, moved);
    return RareWritable(file, std::move(edited), sequence);
}

// The channel of the note nearest `tick` in `timeline`, before it where there's one: a new note there plays on that
// channel. Else the channel of any command of the track that names one, or channel 0.
uint8_t NewNoteChannel(const Timeline& timeline, int64_t tick)
{
    std::optional<uint8_t> before;
    std::optional<uint8_t> after;
    std::optional<uint8_t> any;
    for (const Event& event : timeline.events)
    {
        if (event.added || !NamesChannel(event.command))
        {
            continue;
        }

        any = any.value_or(event.command.index);
        if (event.command.kind != Kind::kNote)
        {
            continue;
        }

        if (event.tick <= tick)
        {
            before = event.command.index;
        }
        else if (!after)
        {
            after = event.command.index;
        }
    }

    return before.value_or(after.value_or(any.value_or(0)));
}

// The timeline in `timelines` of the track of `before` that runs item `item`, by the run's first item (RareEditNotes).
// `entries` contains the items where code enters. `timelines` gains it the first time.
Timeline& TimelineOfItem(const Sequence& before, const std::vector<uint32_t>& entries,
                         std::map<uint32_t, Timeline>& timelines, uint32_t item)
{
    const Run run = RunOfItem(before, entries, item);
    auto found = timelines.find(run.first);
    if (found == timelines.end())
    {
        found = timelines.emplace(run.first, TimelineOf(before, run)).first;
    }

    return found->second;
}

// The event of `timeline` that contains item `item`. Throws std::invalid_argument with a message for the user where
// none does.
std::size_t EventOf(const Timeline& timeline, uint32_t item)
{
    for (std::size_t i = 0; i < timeline.events.size(); i++)
    {
        if (timeline.events[i].old == item)
        {
            return i;
        }
    }

    throw std::invalid_argument("Can't find the command of this note.");
}

// An edited note: its timeline, its event, and its note-off's event (RareEditNotes).
struct Found
{
    Timeline* timeline = nullptr;
    std::size_t on = 0;
    std::optional<std::size_t> off;
};

// The note-off of each of `edits` of `before`, before the edits (RareEditNotes). `offsets` contains the offsets of the
// items, and `entries` the items where code enters. `timelines` gains the notes' timelines.
std::vector<Found> NoteOffs(const Sequence& before, const std::vector<uint32_t>& offsets,
                            const std::vector<uint32_t>& entries, std::map<uint32_t, Timeline>& timelines,
                            std::span<const NoteEdit> edits)
{
    std::vector<Found> notes;
    for (const NoteEdit& edit : edits)
    {
        const std::optional<uint32_t> item = ItemAtOffset(offsets, edit.command);
        if (!item || before.items[*item].kind != Kind::kNote)
        {
            throw std::invalid_argument("Can't find the command of this note.");
        }

        Timeline& timeline = TimelineOfItem(before, entries, timelines, *item);
        const std::size_t on = EventOf(timeline, *item);
        notes.push_back({&timeline, on, NoteOffOf(timeline.events, on)});
    }

    return notes;
}

// Applies `edit` to its note, `found` (RareEditNotes).
void EditRareNote(const NoteEdit& edit, const Found& found)
{
    Timeline& timeline = *found.timeline;
    const std::size_t on = found.on;
    const std::optional<std::size_t> off = found.off;
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

    if (edit.transpose != 0)
    {
        const int key = note.command.key + edit.transpose;
        if (key < 0 || key > kHighestKey)
        {
            throw std::invalid_argument("Keys run from 0 to " + std::to_string(kHighestKey) + ".");
        }

        if (off)
        {
            timeline.events[*off].command.key = static_cast<uint8_t>(key);
        }

        note.command.key = static_cast<uint8_t>(key);
    }

    if (edit.velocity)
    {
        note.command.velocity = std::clamp(*edit.velocity, 1, kMostVelocity) * kOne;
    }

    MoveNote(edit, note, off ? &timeline.events[*off] : nullptr,
             "This note's note-off isn't on its track. You can't move the note or change its length.");
}

// Adds `addition` to the timeline of its track in `timelines` (RareEditNotes). `before`, `offsets` and `entries` give
// the sequence before the edit.
void AddRareNote(const Sequence& before, const std::vector<uint32_t>& offsets, const std::vector<uint32_t>& entries,
                 std::map<uint32_t, Timeline>& timelines, const NewNote& addition)
{
    if (addition.key < 0 || addition.key > kHighestKey)
    {
        throw std::invalid_argument("Keys run from 0 to " + std::to_string(kHighestKey) + ".");
    }
    if (addition.length < 1)
    {
        throw std::invalid_argument("A note lasts at least one tick.");
    }

    // The first command of the stretch identifies the track, and the tick in the track at which the stretch starts.
    const std::optional<uint32_t> first = ItemAtOffset(offsets, addition.stretch.first);
    if (!first)
    {
        throw std::invalid_argument("Can't find where this note's track starts.");
    }

    Timeline& timeline = TimelineOfItem(before, entries, timelines, *first);
    const int64_t tick = TickOfItem(before, timeline, *first) + int64_t{addition.tick} - int64_t{addition.stretch.tick};
    const uint8_t channel = NewNoteChannel(timeline, tick);
    if (channel >= kChannels)
    {
        throw std::invalid_argument("The driver has 16 channels.");
    }

    Command note;
    note.kind = Kind::kNote;
    note.index = channel;
    note.key = static_cast<uint8_t>(addition.key);
    note.velocity = std::clamp(addition.velocity, 1, kMostVelocity) * kOne;
    Command note_off;
    note_off.kind = Kind::kNoteOff;
    note_off.index = channel;
    note_off.key = note.key;
    Event& added = timeline.events.emplace_back(Event{tick, kMovedNote, timeline.next_order++, note});
    added.added = true;
    timeline.events.push_back(Event{tick + int64_t{addition.length}, kMovedOff, timeline.next_order++, note_off});
}

Edited RareEditNotes(const format::File& file, uint32_t sequence, std::span<const NoteEdit> edits,
                     std::span<const NewNote> additions)
{
    const Sequence& before = SequenceToEdit(file, sequence);
    const std::vector<uint32_t> offsets = Offsets(before, ConsoleOf(file));
    const std::vector<uint32_t> entries = EntriesOf(file, sequence);

    // The timeline of each track that an edit touches, by the run's first item.
    std::map<uint32_t, Timeline> timelines;
    const std::vector<Found> notes = NoteOffs(before, offsets, entries, timelines, edits);
    for (std::size_t n = 0; n < edits.size(); n++)
    {
        EditRareNote(edits[n], notes[n]);
    }

    for (const NewNote& addition : additions)
    {
        AddRareNote(before, offsets, entries, timelines, addition);
    }

    std::vector<Timeline> laid;
    for (auto& [first, timeline] : timelines)
    {
        laid.push_back(std::move(timeline));
    }

    return Laid(file, sequence, before, laid);
}

// The run of track `track` of sound `sound`: the field of the track's place in the tune's track list names its start.
Run TrackRun(const format::File& file, std::span<const std::optional<SoundPlace>> places, uint32_t sound, int track)
{
    const uint32_t sequence = PlaceToEdit(places, sound).sequence;
    const SoundPlace& place = *places[sound];
    if (track < 0 || static_cast<std::size_t>(track) >= place.track_fields.size() || !file.native ||
        place.track_fields[static_cast<std::size_t>(track)] >= file.native->fields.size())
    {
        throw std::invalid_argument("The tune has no such track.");
    }

    const ntdlib::sar::Place& start = file.native->fields[place.track_fields[static_cast<std::size_t>(track)]].to;
    const std::optional<Run> run =
        start.item ? RunFrom(SequenceToEdit(file, sequence), start.offset) : std::optional<Run>();
    if (!run)
    {
        throw std::invalid_argument("Can't find this track's commands.");
    }

    return *run;
}

const char* const kKeepsTracks = "A tune of Rare's driver keeps its count of tracks. You can't add a track to it.";

// The edits on Rare's driver. Where the defaults of DriverEdits apply:
// - The bytes of the driver keep nothing else around an edit.
// - The fields of the tune's track list start the tracks.
// - A loop's end sends the track back to the loop's start for ever.
class RareDriverEdits final : public FixedTracksEdits
{
public:
    RareDriverEdits()
        : FixedTracksEdits(kKeepsTracks, "A tune of Rare's driver keeps its count of tracks. You can't paste a track.")
    {
    }

    Edited Writable(const format::File& before, Edited edited, uint32_t sequence) const override
    {
        return RareWritable(before, std::move(edited), sequence);
    }

    Edited EditNotes(const format::File& file, uint32_t sequence, std::span<const NoteEdit> edits,
                     std::span<const NewNote> additions) const override
    {
        return RareEditNotes(file, sequence, edits, additions);
    }

    // A delay and a tempo take up to 3 bytes. A program and a controller's value take a byte. The bytes of a note
    // store no length, and no command uses a time, a random value or a variable.
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
        case Kind::kTempo:
            return ValueRange{0, kLongestDelay * kOne};
        case Kind::kProgram:
        case Kind::kVolume:
        case Kind::kLfoDepth:
        case Kind::kAttack:
        case Kind::kDecay:
        case Kind::kSustain:
        case Kind::kRelease:
            return ValueRange{0, 0xFF * kOne};
        default:
            break;
        }

        return std::nullopt;
    }

    // A loop whose commands hold no delay runs for ever within one of the driver's frames.
    std::size_t LoopsWithoutWaits(const Sequence& sequence, std::span<const uint32_t> entries) const override
    {
        std::size_t count = 0;
        for (const uint32_t entry : entries)
        {
            const std::optional<Run> run = RunFrom(sequence, entry);
            if (!run || sequence.items[run->last].kind != Kind::kLoopEnd)
            {
                continue;
            }

            int64_t ticks = 0;
            bool looping = false;
            for (uint32_t item = run->first; item < run->last; item++)
            {
                looping = looping || sequence.items[item].kind == Kind::kLoopStart;
                if (looping && sequence.items[item].kind == Kind::kWait)
                {
                    ticks += TicksOf(sequence.items[item]);
                }
            }

            count += ticks == 0 ? 1 : 0;
        }

        return count;
    }

    // The track keeps its place in the tune's track list. Its notes and note-offs go, and its other commands stay.
    format::File RemoveTrack(const format::File& file, std::span<const std::optional<SoundPlace>> places,
                             uint32_t sound, int track) const override
    {
        const uint32_t sequence = PlaceToEdit(places, sound).sequence;
        const Sequence& before = SequenceToEdit(file, sequence);
        Timeline timeline = TimelineOf(before, TrackRun(file, places, sound, track));
        bool found = false;
        for (Event& event : timeline.events)
        {
            if (event.command.kind == Kind::kNote || event.command.kind == Kind::kNoteOff)
            {
                event.removed = true;
                found = true;
            }
        }
        if (!found)
        {
            throw std::invalid_argument("This track plays no notes.");
        }

        return Laid(file, sequence, before, std::span(&timeline, 1)).file;
    }

    std::string RefusedPan(const song::DriverRules& /*rules*/) const override
    {
        return "Tracks of Rare's driver have no pan.";
    }

    void IndexOpening(const ntdlib::commands::Sequence& commands, uint32_t first,
                      ntdlib::commands::Command& command) const override
    {
        // A volume of Rare's driver sets the volume of a channel: the channel of the track's commands.
        const std::optional<uint8_t> channel = RareTrackChannel(commands, first);
        if (!channel)
        {
            throw std::invalid_argument("This track has no channel yet. Add a note to it first.");
        }

        command.index = *channel;
    }

    // The driver keeps one program for each channel, and every track that plays on the channel plays it.
    bool ChannelsHoldPrograms() const override
    {
        return true;
    }

    std::optional<uint8_t> ProgramChannel(const ntdlib::commands::Sequence& commands,
                                          const std::vector<uint32_t>& offsets,
                                          std::span<const song::Stretch> stretches) const override
    {
        return RareInstrumentChannel(commands, offsets, stretches);
    }
};

} // namespace

const DriverEdits& RareEdits()
{
    static const RareDriverEdits edits;
    return edits;
}

bool IsRare(const song::Console& console)
{
    return RunsDriver(console, format::Engine::kRare);
}

std::optional<uint8_t> RareTrackChannel(const Sequence& sequence, uint32_t first)
{
    const std::optional<Run> run = RunFrom(sequence, first);
    for (uint32_t item = first; run && item <= run->last; item++)
    {
        if (NamesChannel(sequence.items[item]))
        {
            return sequence.items[item].index;
        }
    }

    return std::nullopt;
}

std::optional<uint8_t> RareInstrumentChannel(const Sequence& sequence, const std::vector<uint32_t>& offsets,
                                             std::span<const song::Stretch> stretches)
{
    const auto plays = [](const Command& command)
    {
        return command.kind == Kind::kNote || command.kind == Kind::kProgram;
    };

    // The items that the track ran, and the channels that its commands name.
    std::vector<bool> ran(sequence.items.size());
    std::set<uint8_t> played;
    std::optional<uint8_t> named;
    for (const song::Stretch& stretch : stretches)
    {
        const std::optional<uint32_t> first = ItemAtOffset(offsets, stretch.first);
        const std::optional<uint32_t> last = ItemAtOffset(offsets, stretch.last);
        if (!first || !last || *last < *first)
        {
            throw std::invalid_argument("The track's commands aren't where the inspector shows them.");
        }

        for (uint32_t item = *first; item <= *last && item < sequence.items.size(); item++)
        {
            ran[item] = true;
            const Command& command = sequence.items[item];
            if (plays(command))
            {
                played.insert(command.index);
            }

            if (!named && NamesChannel(command))
            {
                named = command.index;
            }
        }
    }
    if (played.size() > 1)
    {
        throw std::invalid_argument(
            "This track plays on more than one MIDI channel. Change its program changes in the "
            "List Editor: Edit > Open List Editor.");
    }

    const std::optional<uint8_t> channel = played.empty() ? named : std::optional<uint8_t>(*played.begin());
    for (uint32_t item = 0; channel && item < sequence.items.size(); item++)
    {
        const Command& command = sequence.items[item];
        if (!ran[item] && plays(command) && command.index == *channel)
        {
            throw std::invalid_argument(
                "Another track plays on this track's MIDI channel. Rare's driver keeps one "
                "program for each channel. Change the program changes in the List Editor: "
                "Edit > Open List Editor.");
        }
    }

    return channel;
}

} // namespace hokuto::edit
