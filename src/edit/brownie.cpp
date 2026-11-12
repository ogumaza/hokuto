// SPDX-License-Identifier: MIT

#include "edit/brownie.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "ntdlib/agb/rom.h"
#include "ntdlib/brownie/commands.h"
#include "ntdlib/brownie/driver.h"
#include "ntdlib/commands/sequence.h"

#include "edit/brownie_parts.h"
#include "edit/bytecode.h"
#include "edit/commands.h"
#include "edit/drivers.h"
#include "edit/notes.h"
#include "edit/replace.h"
#include "format/archive.h"
#include "format/brownie_items.h"
#include "format/drivers.h"
#include "song/brownie.h"

namespace hokuto::edit
{

namespace
{

namespace brownie = ntdlib::brownie;
using brownie_parts::Barrier;
using brownie_parts::BlockAt;
using brownie_parts::BlockOf;
using brownie_parts::BlockStart;
using brownie_parts::BrownieRange;
using brownie_parts::BrownieWritable;
using brownie_parts::Code;
using brownie_parts::EditBrownieNotes;
using brownie_parts::EndOf;
using brownie_parts::EndsBlock;
using brownie_parts::Examine;
using brownie_parts::Item;
using brownie_parts::ItemAt;
using brownie_parts::Rewrite;
using brownie_parts::Segment;
using brownie_parts::SegmentOf;
using brownie_parts::SplitAt;
using brownie_parts::StartOf;
using brownie_parts::Tables;
using brownie_parts::TablesOf;
using brownie_parts::Timeline;
using brownie_parts::TimelineOf;
using ntdlib::commands::Command;
using ntdlib::commands::Kind;
using ntdlib::commands::Sequence;
using ntdlib::commands::Source;

// The tick at which a sound's channels read their first commands. The driver starts a sound at the end of the frame in
// which the game asks for it, and its channels run from the next frame on.
constexpr uint32_t kFirstTick = 1;

// The driver's opcode of the command that brings a sound back after a fade. It ends the channel's frame without a
// length.
constexpr uint16_t kResume = 0xE3;

// The setting at item `item` of `timeline`, taken out, and the tick where it stood. Nothing when the item isn't there.
std::optional<std::pair<Item, uint32_t>> TakeSetting(Timeline& timeline, uint32_t item)
{
    for (Segment& segment : timeline.segments)
    {
        const auto found =
            std::ranges::find_if(segment.before, [item](const Item& candidate) { return candidate.old == item; });
        if (found != segment.before.end())
        {
            Item taken = std::move(*found);
            segment.before.erase(found);
            return std::pair(std::move(taken), segment.start);
        }
    }

    const auto found =
        std::ranges::find_if(timeline.after, [item](const Item& candidate) { return candidate.old == item; });
    if (found != timeline.after.end())
    {
        Item taken = std::move(*found);
        timeline.after.erase(found);
        return std::pair(std::move(taken), timeline.length);
    }

    return std::nullopt;
}

// Puts `setting` in `timeline` at `tick`, before the command that waits there, after the settings that stand there
// already.
void PutSetting(Timeline& timeline, Item setting, uint32_t tick)
{
    const std::size_t at = SplitAt(timeline, tick);
    if (at == timeline.segments.size())
    {
        // Before the command that ends the block, if any.
        const auto end = std::ranges::find_if(timeline.after, [](const Item& item) { return EndsBlock(item.command); });
        timeline.after.insert(end, std::move(setting));
        return;
    }

    timeline.segments[at].before.push_back(std::move(setting));
}

// Throws std::invalid_argument with a message for the user when `command` can't go before item `at` of `commands`,
// sequence `sequence` of `file`, on Brownie Brown's driver. A command that waits uses the length in force there
// (InsertCommand).
void PrepareBrownieInsert(const format::File& file, uint32_t sequence, const ntdlib::commands::Sequence& commands,
                          uint32_t at, Command& command)
{
    // A call and a jump read their places from the channel's table, and a list's channels are tracks.
    if (command.kind == Kind::kCall || command.kind == Kind::kJump || command.kind == Kind::kOpenTrack)
    {
        throw std::invalid_argument(
            "A call or a jump of Brownie Brown's driver reads its place from the channel's table. Hokuto doesn't "
            "add one.");
    }
    if (commands.items[at].kind == Kind::kData || BrownieEdits().IsTableEntry(commands.items[at]))
    {
        throw std::invalid_argument("No channel reads these bytes as commands. A new command can't go before them.");
    }

    // A new command that waits uses the length in force there, unless it has frames.
    if (format::BrownieWaits(command) && format::BrownieWaitOf(command) == 0)
    {
        const uint32_t frames = BrownieLengthAt(file, sequence, at);
        format::SetBrownieWait(command, frames != 0 ? frames : 1);
    }
}

// The edits on Brownie Brown's driver. Where the defaults of DriverEdits apply:
// - The driver's bytes keep nothing else around an edit. Writable puts the length bytes in.
// - A sound's list starts its channels, at the sound's start.
// - A jump reads its place in the channel's table. A new jump has no word there.
class BrownieDriverEdits final : public DriverEdits
{
public:
    Edited Writable(const format::File& before, Edited edited, uint32_t sequence) const override
    {
        return BrownieWritable(before, std::move(edited), sequence);
    }

    Edited EditNotes(const format::File& file, uint32_t sequence, std::span<const NoteEdit> edits,
                     std::span<const NewNote> additions) const override
    {
        return BrownieWritable(file, EditBrownieNotes(file, sequence, edits, additions), sequence);
    }

    std::optional<ValueRange> Range(const song::Console& console, int /*track*/, const Command& command, Source source,
                                    bool time) const override
    {
        return BrownieRange(command, source, time, static_cast<brownie::Revision>(console.driver->Revision()));
    }

    // A channel that reads too many commands in a frame ends (ntdlib's sequencer). No loop hangs the console.
    std::size_t LoopsWithoutWaits(const Sequence&, std::span<const uint32_t>) const override
    {
        return 0;
    }

    // Each note waits for the channel's length.
    bool NoteWaitOpens(const Sequence&) const override
    {
        return true;
    }

    bool OpensTrack(const Sequence&, const Command& item, int track) const override
    {
        return item.kind == Kind::kOpenTrack && item.index == track;
    }

    bool IsTableEntry(const Command& item) const override
    {
        return format::BrownieIsWord(item);
    }

    // F4 starts a repeat of a number of passes, and F5 ends it.
    bool CountsRepeats() const override
    {
        return true;
    }

    TrackEdit AddTrack(const format::File& file, std::span<const std::optional<SoundPlace>> places, uint32_t sound,
                       const song::Sequence& song, std::optional<Program>) const override;
    TrackEdit DuplicateTrack(const format::File& file, std::span<const std::optional<SoundPlace>> places,
                             uint32_t sound, int track) const override;
    CopiedTrack CopyTrack(const format::File& file, std::span<const std::optional<SoundPlace>> places, uint32_t sound,
                          const song::Sequence& song, int track) const override;
    TrackEdit PasteTrack(const format::File& file, std::span<const std::optional<SoundPlace>> places, uint32_t sound,
                         const song::Sequence&, const CopiedTrack& copied) const override;
    format::File RemoveTrack(const format::File& file, std::span<const std::optional<SoundPlace>> places,
                             uint32_t sound, int track) const override;
    void CheckMixable(const song::Console& console, int track, song::Mix setting) const override;
    int FirstNoteKey(const format::File& file, uint32_t sequence, int track, const DrawnKeyNote& drawn, int transpose,
                     int key) const override;
    std::optional<Edited> AddSettings(const format::File& file, uint32_t sequence,
                                      std::span<const NewSetting> settings) const override;
    std::optional<Edited> MoveSettings(const format::File& file, uint32_t sequence,
                                       std::span<const SettingMove> moves) const override;
    std::optional<std::vector<NoteLimits>> LimitsOf(const format::File& file, uint32_t sequence,
                                                    std::span<const NotePlace> notes) const override;
    void PrepareInsert(const format::File& file, uint32_t sequence, const ntdlib::commands::Sequence& commands,
                       uint32_t at, Command& command) const override;
    bool NotesHoldLengths() const override;
    std::vector<std::pair<int, uint32_t>> ListedStarts(const ntdlib::commands::Sequence& sequence,
                                                       uint32_t list) const override;
    void CheckTrackInstrument(const song::Console& console, int track) const override;
    bool SelectsProgram(const song::Console& console, int track, const Command& command) const override;
    bool PassesTime(const Command& command) const override;
    uint8_t ProgramIndex(const song::Console& console, int track) const override;
    std::optional<uint32_t> RowTicks(const Command& command, std::optional<uint32_t> ticks) const override;
};

// A sound's list: its sequence, and the items of its channels, each a channel's word and the word of its table after
// it.
struct ListPlace
{
    uint32_t sequence = 0;
    uint32_t list = 0;
    std::vector<brownie::ListTrack> tracks;
};

// The list of sound `sound` of `file`. Throws std::invalid_argument with a message for the user for a sound that plays
// no list of the file.
ListPlace ListOf(const format::File& file, std::span<const std::optional<SoundPlace>> places, uint32_t sound)
{
    if (sound >= places.size() || !places[sound] || !places[sound]->start_field)
    {
        throw std::invalid_argument("The sound plays no list of channels that Hokuto can edit.");
    }

    ListPlace place;
    place.sequence = places[sound]->sequence;
    place.list = file.native->fields.at(*places[sound]->start_field).to.offset;
    try
    {
        place.tracks = brownie::TracksOfList(SequenceToEdit(file, place.sequence), place.list);
    }
    catch (const std::invalid_argument&)
    {
        throw std::invalid_argument("The sound plays no list of channels that Hokuto can edit.");
    }

    return place;
}

// The end of the list: the item after the table word of its last channel.
uint32_t ListEnd(const ListPlace& place)
{
    return place.tracks.empty() ? place.list : place.tracks.back().table + 1;
}

// Throws std::invalid_argument with a message for the user when another sound's list starts at or inside the list
// of `place`: a change of the list would change that sound too.
void RefuseSharedList(const format::File& file, std::span<const std::optional<SoundPlace>> places, uint32_t sound,
                      const ListPlace& place)
{
    for (uint32_t other = 0; other < places.size(); other++)
    {
        if (other == sound || !places[other] || places[other]->sequence != place.sequence ||
            !places[other]->start_field)
        {
            continue;
        }

        const uint32_t start = file.native->fields.at(*places[other]->start_field).to.offset;
        if (start >= place.list && start <= ListEnd(place))
        {
            throw std::invalid_argument(
                "Another sound plays this sound's list of channels, or a part of it. A change of the list would "
                "change that sound too.");
        }
    }
}

// The channels of revision `revision` that play the same kind of sound as channel `channel`: a square, the wave, the
// noise or samples.
std::vector<int> ChannelsLike(brownie::Revision revision, int channel)
{
    const int count = brownie::LayoutOf(revision).channels;
    const brownie::ChannelKind wanted = brownie::KindOf(revision, channel);
    std::vector<int> like;
    for (int other = 0; other < count; other++)
    {
        if (brownie::KindOf(revision, other) == wanted)
        {
            like.push_back(other);
        }
    }

    return like;
}

// Whether channels `a` and `b` of revision `revision` play on the same part of the hardware. The PSG channels of the
// music and of the sound effects share the channels of the PSG. In the Magical Vacation revision, the sample channels
// of the sound effects use the FIFOs of the music channels.
bool ShareHardware(brownie::Revision revision, int a, int b)
{
    if (a < brownie::kFirstSampleChannel || b < brownie::kFirstSampleChannel)
    {
        return a < brownie::kFirstSampleChannel && b < brownie::kFirstSampleChannel &&
               a % brownie::kFirstEffectChannel == b % brownie::kFirstEffectChannel;
    }

    return a == b || (revision == brownie::Revision::kMagicalVacation &&
                      (a - brownie::kFirstSampleChannel) % brownie::kFifoCount ==
                          (b - brownie::kFirstSampleChannel) % brownie::kFifoCount);
}

// The first channel of `candidates` that the list of `place` leaves free, and whose part of the hardware no channel of
// the list plays in revision `revision`. Throws std::invalid_argument with a message for the user when there's none.
int FreeChannel(brownie::Revision revision, const ListPlace& place, std::span<const int> candidates)
{
    for (const int channel : candidates)
    {
        const bool taken = std::ranges::any_of(place.tracks, [revision, channel](const brownie::ListTrack& track)
                                               { return ShareHardware(revision, track.channel, channel); });
        if (!taken)
        {
            return channel;
        }
    }

    throw std::invalid_argument("The sound's list has no free channel of this kind.");
}

// The word of a channel's list entry that names the channel's table at item `table`.
Command TableWord(uint32_t table)
{
    Command word;
    word.kind = Kind::kJump;
    word.encoding.opcode = brownie::kAddressWord;
    word.target = table;

    return word;
}

// `file` with a channel of number `channel` in the list of `place`, after its other channels. The channel reads the
// commands from item `entry` on, and its calls and jumps read the table that `word` names: an item of the sequence, or
// an address outside it. `added` contains new commands to add at the end of the sequence. The targets `entry` and
// `word` count the items of the sequence with those commands at its end, before the two new items of the list move
// them. Throws std::invalid_argument with a message for the user for an edit that the driver's bytes can't hold.
format::File WithChannel(const format::File& file, const ListPlace& place, int channel, uint32_t entry, Command word,
                         std::vector<Command> added)
{
    Sequence sequence = SequenceToEdit(file, place.sequence);
    const uint32_t end = ListEnd(place);

    Command open;
    open.kind = Kind::kOpenTrack;
    open.index = static_cast<uint8_t>(channel);
    open.target = entry;

    // The new commands go at the end, and the list's two new items before its end: every item from the list's end on
    // moves by two.
    for (Command& command : added)
    {
        sequence.items.push_back(std::move(command));
    }

    const auto moved = [end](uint32_t item)
    {
        return item >= end ? item + 2 : item;
    };
    MoveTargets(sequence, moved);
    open.target = moved(open.target);
    if (!word.target_outside)
    {
        word.target = moved(word.target);
    }

    sequence.items.insert(sequence.items.begin() + end, {open, word});

    format::File edited = file;
    edited.sequences[place.sequence] = std::move(sequence);
    // A list that starts at its end, without channels, keeps its start.
    MoveItemPlaces(edited, place.sequence,
                   [&](uint32_t item) { return item == end && end == place.list ? item : moved(item); });

    return edited;
}

// The edit that `file` with a new channel makes, after Writable. `sound` names the sound and `channel` the channel.
TrackEdit Written(const format::File& before, format::File after, uint32_t sequence, int channel)
{
    Edited edited;
    edited.file = std::move(after);
    edited.file = BrownieWritable(before, std::move(edited), sequence).file;

    return {std::move(edited.file), channel};
}

TrackEdit BrownieDriverEdits::AddTrack(const format::File& file, std::span<const std::optional<SoundPlace>> places,
                                       uint32_t sound, const song::Sequence& song,
                                       std::optional<Program> instrument) const
{
    const brownie::Revision revision = *BrownieOf(ConsoleOf(file));
    const ListPlace place = ListOf(file, places, sound);
    RefuseSharedList(file, places, sound, place);

    // A program means another setting on each kind of channel. The new channel copies the kind of the first track whose
    // first note plays `instrument`, and the settings that the track has at that note.
    std::vector<Command> added;
    std::vector<int> candidates;
    const auto source =
        std::ranges::find_if(song.tracks,
                             [&instrument](const song::Track& track)
                             {
                                 return instrument && !track.notes.empty() &&
                                        track.notes.front().program == static_cast<uint32_t>(instrument->number) &&
                                        track.notes.front().bank_slot == instrument->slot;
                             });
    if (source != song.tracks.end())
    {
        const std::optional<std::vector<Command>> settings =
            song::BrownieSettingsAt(revision, SequenceToEdit(file, place.sequence), *source, source->notes.front());
        if (settings)
        {
            added = *settings;
            candidates = ChannelsLike(revision, source->number);
        }
    }

    // Otherwise, the first free channel: a sample channel first, then a channel of the PSG.
    if (candidates.empty())
    {
        const int count = brownie::LayoutOf(revision).channels;
        for (int channel = brownie::kFirstSampleChannel; channel < count; channel++)
        {
            candidates.push_back(channel);
        }

        for (int channel = 0; channel < brownie::kFirstSampleChannel; channel++)
        {
            candidates.push_back(channel);
        }
    }

    const int channel = FreeChannel(revision, place, candidates);

    // After the settings, the new channel rests for the sound's length, and loops with the sound. Like the sound's
    // other channels, it reads its first command at tick 1 (kFirstTick). A jump reads its target from word 0 of the
    // channel's table. The channel's commands start with that table.
    const auto size = static_cast<uint32_t>(SequenceToEdit(file, place.sequence).items.size());
    const uint32_t rests = size + static_cast<uint32_t>(added.size());
    Command rest;
    rest.kind = Kind::kNoteOff;
    rest.index = channel < brownie::kFirstSampleChannel ? 1 : 0;
    uint32_t loop = rests;
    const uint32_t end = std::max<uint32_t>(song.loop ? song.loop->end : song.length, kFirstTick + 1);
    if (song.loop && song.loop->start > kFirstTick && song.loop->start < song.loop->end)
    {
        format::SetBrownieWait(rest, song.loop->start - kFirstTick);
        added.push_back(rest);
        loop++;
    }

    format::SetBrownieWait(rest, end - (loop > rests ? song.loop->start : kFirstTick));
    added.push_back(rest);
    uint32_t table = size;
    if (song.loop)
    {
        Command jump;
        jump.kind = Kind::kJump;
        jump.index = 0;
        jump.target = loop;
        added.push_back(jump);

        table = size + static_cast<uint32_t>(added.size());
        added.push_back(TableWord(loop));
    }
    else
    {
        Command end;
        end.kind = Kind::kEnd;
        added.push_back(end);
    }

    return Written(file, WithChannel(file, place, channel, size, TableWord(table), std::move(added)), place.sequence,
                   channel);
}

TrackEdit BrownieDriverEdits::DuplicateTrack(const format::File& file,
                                             std::span<const std::optional<SoundPlace>> places, uint32_t sound,
                                             int track) const
{
    const brownie::Revision revision = *BrownieOf(ConsoleOf(file));
    const ListPlace place = ListOf(file, places, sound);
    RefuseSharedList(file, places, sound, place);
    const auto found = std::ranges::find_if(
        place.tracks, [track](const brownie::ListTrack& listed) { return listed.channel == track; });
    if (found == place.tracks.end())
    {
        throw std::invalid_argument("The sound plays no such track.");
    }

    // The copy reads the track's table, in the sound's commands or outside them.
    const Command& word = SequenceToEdit(file, place.sequence).items.at(found->table);
    const int channel = FreeChannel(revision, place, ChannelsLike(revision, track));

    return Written(file, WithChannel(file, place, channel, found->entry, word, {}), place.sequence, channel);
}

CopiedTrack BrownieDriverEdits::CopyTrack(const format::File& file, std::span<const std::optional<SoundPlace>> places,
                                          uint32_t sound, const song::Sequence&, int track) const
{
    const ListPlace place = ListOf(file, places, sound);
    const auto found = std::ranges::find_if(
        place.tracks, [track](const brownie::ListTrack& listed) { return listed.channel == track; });
    if (found == place.tracks.end())
    {
        throw std::invalid_argument("The sound plays no such track.");
    }

    // The copy names the track's commands and the word of its list entry that names its table, in the sequence: a
    // paste reads them there. The sequence's commands go along, to tell whether the sequence is the same at the paste.
    CopiedTrack copied;
    copied.commands = SequenceToEdit(file, place.sequence).items;
    copied.entry = found->entry;
    copied.banks = {static_cast<uint32_t>(*BrownieOf(ConsoleOf(file))), place.sequence, found->table};
    copied.sound = sound;
    copied.track = track;
    copied.driver = format::Engine::kBrownie;

    return copied;
}

TrackEdit BrownieDriverEdits::PasteTrack(const format::File& file, std::span<const std::optional<SoundPlace>> places,
                                         uint32_t sound, const song::Sequence&, const CopiedTrack& copied) const
{
    const brownie::Revision revision = *BrownieOf(ConsoleOf(file));
    if (copied.driver != format::Engine::kBrownie || copied.banks.size() != 3 ||
        copied.banks[0] != static_cast<uint32_t>(revision))
    {
        throw std::invalid_argument("A sound of Brownie Brown's driver takes only a track of a sound of this ROM.");
    }

    const ListPlace place = ListOf(file, places, sound);
    RefuseSharedList(file, places, sound, place);
    if (copied.banks[1] != place.sequence || SequenceToEdit(file, place.sequence).items != copied.commands)
    {
        throw std::invalid_argument(
            "Brownie Brown's driver plays a pasted track from the copied track's commands in place. Those commands are "
            "stored with another group of sounds, or have changed since you copied the track.");
    }

    const int channel = FreeChannel(revision, place, ChannelsLike(revision, copied.track));

    const Command& word = copied.commands.at(copied.banks[2]);

    return Written(file, WithChannel(file, place, channel, copied.entry, word, {}), place.sequence, channel);
}

format::File BrownieDriverEdits::RemoveTrack(const format::File& file,
                                             std::span<const std::optional<SoundPlace>> places, uint32_t sound,
                                             int track) const
{
    const ListPlace place = ListOf(file, places, sound);
    RefuseSharedList(file, places, sound, place);
    const auto found = std::ranges::find_if(
        place.tracks, [track](const brownie::ListTrack& listed) { return listed.channel == track; });
    if (found == place.tracks.end())
    {
        throw std::invalid_argument("The sound plays no such track.");
    }

    // The channel's two words leave the list. A target or a place that named them names the item after them.
    Sequence sequence = SequenceToEdit(file, place.sequence);
    const uint32_t first = found->table - 1;
    const auto moved = [first](uint32_t item)
    {
        return item >= first + 2 ? item - 2 : std::min(item, first);
    };
    sequence.items.erase(sequence.items.begin() + first, sequence.items.begin() + first + 2);
    MoveTargets(sequence, moved);

    format::File edited = file;
    edited.sequences[place.sequence] = std::move(sequence);
    MoveItemPlaces(edited, place.sequence, moved);

    return Written(file, std::move(edited), place.sequence, track).file;
}

// Brownie Brown's driver sets the pan of a sample channel as a level for each side, and the pan of a PSG channel as the
// sides that it plays on. The volume of a PSG channel lowers the level of the envelope: a higher value plays quieter.
// The controls of the mixer support neither.
void BrownieDriverEdits::CheckMixable(const song::Console& console, int track, song::Mix setting) const
{
    if (setting == song::Mix::kPan)
    {
        throw std::invalid_argument(
            "Brownie Brown's driver sets a pan as a level for each side, or as the sides that a "
            "PSG channel plays on. The List Editor changes its commands: Edit > Open List "
            "Editor.");
    }
    if (setting == song::Mix::kVolume && console.driver->PsgTrack(track))
    {
        throw std::invalid_argument(
            "On a PSG channel of Brownie Brown's driver, the volume lowers the envelope's level, "
            "and a higher value plays quieter. The List Editor changes its commands: Edit > Open "
            "List Editor.");
    }
}

// A note's key leaves out the key of the PSG's lowest note (BrownieCommandKey).
int BrownieDriverEdits::FirstNoteKey(const format::File& file, uint32_t /*sequence*/, int track,
                                     const DrawnKeyNote& drawn, int transpose, int /*key*/) const
{
    const std::optional<int> byte = BrownieCommandKey(file, track, transpose, drawn.key);
    if (!byte)
    {
        throw std::invalid_argument("No note of this track plays that key.");
    }

    return *byte;
}

std::optional<Edited> BrownieDriverEdits::AddSettings(const format::File& file, uint32_t sequence,
                                                      std::span<const NewSetting> settings) const
{
    return AddBrownieSettings(file, sequence, settings);
}

std::optional<Edited> BrownieDriverEdits::MoveSettings(const format::File& file, uint32_t sequence,
                                                       std::span<const SettingMove> moves) const
{
    return MoveBrownieSettings(file, sequence, moves);
}

std::optional<std::vector<NoteLimits>> BrownieDriverEdits::LimitsOf(const format::File& file, uint32_t sequence,
                                                                    std::span<const NotePlace> notes) const
{
    return BrownieLimitsOf(file, sequence, notes);
}

void BrownieDriverEdits::PrepareInsert(const format::File& file, uint32_t sequence,
                                       const ntdlib::commands::Sequence& commands, uint32_t at, Command& command) const
{
    PrepareBrownieInsert(file, sequence, commands, at, command);
}

// A note has no length: its value is its wait. Like a wait's value, that wait moves the commands after the note.
bool BrownieDriverEdits::NotesHoldLengths() const
{
    return false;
}

// A sound starts at a list of its channels, each a track of the same number.
std::vector<std::pair<int, uint32_t>> BrownieDriverEdits::ListedStarts(const ntdlib::commands::Sequence& sequence,
                                                                       uint32_t list) const
{
    return BrownieListStarts(sequence, list);
}

// A noise channel's notes give the noise's settings, and no command selects an instrument.
void BrownieDriverEdits::CheckTrackInstrument(const song::Console& console, int track) const
{
    if (!song::BrownieProgramIndex(*BrownieOf(console), track))
    {
        throw std::invalid_argument(
            "A noise channel of Brownie Brown's driver plays no instrument. Each of its notes "
            "gives the noise's settings.");
    }
}

// Only the command that the track's notes take their program from selects it (song::BrownieProgramIndex): E1 on a
// sample channel, F1's duty on a square channel and FC's wave on the wave channel.
bool BrownieDriverEdits::SelectsProgram(const song::Console& console, int track, const Command& command) const
{
    if (track < 0)
    {
        return true;
    }

    const std::optional<uint8_t> index = song::BrownieProgramIndex(*BrownieOf(console), track);
    return index && command.index == *index;
}

// A rest and a release wait for the channel's length too (format::BrownieWaits).
bool BrownieDriverEdits::PassesTime(const Command& command) const
{
    return format::BrownieWaits(command);
}

uint8_t BrownieDriverEdits::ProgramIndex(const song::Console& console, int track) const
{
    return song::BrownieProgramIndex(*BrownieOf(console), track).value_or(0);
}

// Each command that waits stores its frames (format/brownie_items.h).
std::optional<uint32_t> BrownieDriverEdits::RowTicks(const Command& command, std::optional<uint32_t> ticks) const
{
    if (!format::BrownieWaits(command))
    {
        return ticks;
    }

    const uint32_t frames = format::BrownieWaitOf(command);
    return frames != 0 ? std::optional(frames) : std::nullopt;
}

} // namespace

namespace brownie_parts
{

std::shared_ptr<const Tables> TablesOf(const format::File& file)
{
    static std::mutex mutex;
    static std::vector<std::pair<format::Shared<std::vector<ntdlib::sar::Piece>>, std::shared_ptr<const Tables>>> kept;
    {
        const std::lock_guard lock(mutex);
        for (const auto& [pieces, tables] : kept)
        {
            if (pieces.SharesWith(file.native->pieces))
            {
                return tables;
            }
        }
    }

    ntdlib::agb::Rom rom;
    rom.Assign(format::ExportArchive(file));
    brownie::DriverInfo info;
    std::string error;
    if (!brownie::DetectDriver(rom, {}, info, error))
    {
        throw std::invalid_argument("Hokuto can't read the driver's tables of the ROM that the file builds.");
    }

    auto tables = std::make_shared<Tables>();
    tables->revision = info.revision;
    tables->base = brownie::PsgTuning(rom, info).base;
    const int sets = info.revision == brownie::Revision::kMagicalVacation ? 1 : 256;
    for (int set = 0; set < sets; set++)
    {
        for (int byte = 0x80; byte < 0xE0; byte++)
        {
            const uint32_t place =
                info.length_table + static_cast<uint32_t>(byte & 0x7F) + 16 * static_cast<uint32_t>(set);
            const uint8_t frames = rom.Contains(place, 1) ? rom.U8(place) : 0;
            tables->frames.push_back(frames != 0 ? frames : 256);
        }
    }

    const std::lock_guard lock(mutex);
    kept.emplace_back(file.native->pieces, tables);
    if (kept.size() > 4)
    {
        kept.erase(kept.begin());
    }

    return tables;
}

bool EndsBlock(const Command& item)
{
    switch (item.kind)
    {
    case Kind::kJump:
    case Kind::kCall:
    case Kind::kReturn:
    case Kind::kEnd:
    case Kind::kLoopStart:
    case Kind::kLoopEnd:
    case Kind::kOpenTrack:
        return true;
    default:
        return false;
    }
}

bool Barrier(const Command& item)
{
    return item.kind == Kind::kData || (item.kind == Kind::kOther && item.encoding.opcode == kResume) ||
           (format::BrownieWaits(item) && format::BrownieWaitOf(item) == 0);
}

Command Hold()
{
    Command hold;
    hold.kind = Kind::kWait;
    return hold;
}

} // namespace brownie_parts

const DriverEdits& BrownieEdits()
{
    static const BrownieDriverEdits edits;
    return edits;
}

std::optional<brownie::Revision> BrownieOf(const song::Console& console)
{
    if (!RunsDriver(console, format::Engine::kBrownie))
    {
        return std::nullopt;
    }

    return static_cast<brownie::Revision>(console.driver->Revision());
}

Edited AddBrownieSettings(const format::File& file, uint32_t sequence, std::span<const NewSetting> settings)
{
    const Code code = Examine(file, sequence);
    std::map<std::size_t, Timeline> timelines;
    for (std::size_t index = 0; index < settings.size(); index++)
    {
        const NewSetting& setting = settings[index];
        if (WhyFixedInTime(setting.command) || format::BrownieWaits(setting.command))
        {
            throw std::invalid_argument("Only a setting, such as a volume, goes in at a tick.");
        }

        const auto [block, start] = BlockAt(code, setting.stretch, setting.tick);
        auto found = timelines.find(block);
        if (found == timelines.end())
        {
            found = timelines.emplace(block, TimelineOf(code, block)).first;
        }

        PutSetting(found->second, {setting.command, std::nullopt, index}, setting.tick - start);
    }

    return BrownieWritable(file, Rewrite(file, sequence, code, std::move(timelines), settings.size()), sequence);
}

Edited MoveBrownieSettings(const format::File& file, uint32_t sequence, std::span<const SettingMove> moves)
{
    const Code code = Examine(file, sequence);
    std::map<std::size_t, Timeline> timelines;
    std::set<uint32_t> named;
    std::vector<std::tuple<std::size_t, Item, int64_t>> taken;
    for (const SettingMove& move : moves)
    {
        const uint32_t item = ItemAt(code, move.command);
        if (const std::optional<std::string> why = WhyFixedInTime(code.commands->items[item]))
        {
            throw std::invalid_argument(*why);
        }
        if (move.shift == 0 || !named.insert(item).second)
        {
            continue;
        }

        const std::size_t block = BlockOf(code, item);
        BlockStart(code, move.stretch, block);
        auto found = timelines.find(block);
        if (found == timelines.end())
        {
            found = timelines.emplace(block, TimelineOf(code, block)).first;
        }

        std::optional<std::pair<Item, uint32_t>> setting = TakeSetting(found->second, item);
        if (!setting)
        {
            throw std::invalid_argument("The setting's command isn't where the arrangement shows it.");
        }

        taken.emplace_back(block, std::move(setting->first), int64_t{setting->second} + move.shift);
    }

    for (auto& [block, setting, tick] : taken)
    {
        Timeline& timeline = timelines.at(block);
        if (tick < 0 || tick > timeline.length)
        {
            throw std::invalid_argument("A setting can't move out of its part of the track's commands.");
        }

        PutSetting(timeline, std::move(setting), static_cast<uint32_t>(tick));
    }

    return BrownieWritable(file, Rewrite(file, sequence, code, std::move(timelines), 0), sequence);
}

std::vector<NoteLimits> BrownieLimitsOf(const format::File& file, uint32_t sequence, std::span<const NotePlace> notes)
{
    const Code code = Examine(file, sequence);
    std::map<std::size_t, Timeline> timelines;
    std::vector<NoteLimits> limits;
    for (const NotePlace& note : notes)
    {
        NoteLimits& limit = limits.emplace_back();
        const uint32_t item = ItemAt(code, note.command);
        const Command& command = code.commands->items[item];
        const std::size_t block = BlockOf(code, item);
        auto found = timelines.find(block);
        if (found == timelines.end())
        {
            found = timelines.emplace(block, TimelineOf(code, block)).first;
        }

        const Timeline& timeline = found->second;
        if (command.kind == Kind::kNote)
        {
            const std::optional<std::size_t> index = SegmentOf(timeline, item);
            if (!index || Barrier(command))
            {
                continue;
            }

            const uint32_t tick = timeline.segments[*index].start;
            limit.shift = ShiftRange{tick, timeline.length - 1 - tick};
            limit.length = StartOf(timeline, EndOf(timeline, *index)) - tick;
            continue;
        }

        if (WhyFixedInTime(command) || format::BrownieWaits(command))
        {
            continue;
        }

        Timeline copy = timeline;
        const std::optional<std::pair<Item, uint32_t>> setting = TakeSetting(copy, item);
        if (setting)
        {
            limit.shift = ShiftRange{setting->second, timeline.length - setting->second};
        }
    }

    return limits;
}

std::vector<std::pair<int, uint32_t>> BrownieListStarts(const Sequence& sequence, uint32_t list)
{
    std::vector<std::pair<int, uint32_t>> starts;
    try
    {
        for (const brownie::ListTrack& track : brownie::TracksOfList(sequence, list))
        {
            starts.emplace_back(track.channel, track.entry);
        }
    }
    catch (const std::invalid_argument&)
    {
        return {};
    }

    return starts;
}

std::optional<int> BrownieCommandKey(const format::File& file, int track, int transpose, int shown)
{
    const std::shared_ptr<const Tables> tables = TablesOf(file);
    const bool noise =
        track < brownie::kFirstSampleChannel && track % brownie::kFirstEffectChannel == brownie::kNoiseChannel;
    const int byte = noise ? shown : shown - tables->base - transpose;
    if (byte < 0 || byte > 0x7F ||
        song::BrownieShownKey(tables->revision, track, tables->base, transpose, static_cast<uint8_t>(byte)) != shown)
    {
        return std::nullopt;
    }

    return byte;
}

uint32_t BrownieLengthAt(const format::File& file, uint32_t sequence, uint32_t item)
{
    const std::shared_ptr<const Tables> tables = TablesOf(file);
    const format::BrownieReadings readings =
        format::ReadBrownieLengths(SequenceToEdit(file, sequence), tables->revision, tables->Length());
    if (item >= readings.lengths.size() || readings.lengths[item].size() != 1)
    {
        return 0;
    }

    return *readings.lengths[item].begin();
}

} // namespace hokuto::edit
