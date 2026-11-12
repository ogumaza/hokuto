// SPDX-License-Identifier: MIT

#include "edit/konami.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/commands/units.h"
#include "ntdlib/konami/commands.h"
#include "ntdlib/konami/layout.h"
#include "ntdlib/sar/layout.h"

#include "edit/drivers.h"
#include "edit/mp2k.h"

namespace hokuto::edit
{

namespace
{

using ntdlib::commands::Command;
using ntdlib::commands::Kind;
using ntdlib::commands::kOne;
using ntdlib::commands::Sequence;

constexpr int kMostCalledCommands = 255;

// Whether the driver reads no delay after `command`: the track has ended, or the song has gone back to its loop.
bool EndsTrack(const Command& command)
{
    return command.kind == Kind::kEnd || command.kind == Kind::kLoopEnd;
}

// The new index just past the last command of `range` that stays, or `target` when none does. A call returns after its
// last command. `old` is the sequence before the edit.
uint32_t NewEnd(const Sequence& old, const Edited& edited, const KonamiCallRange& range, uint32_t target)
{
    uint32_t end = target;
    for (uint32_t item = range.first; item < range.end; item++)
    {
        if (edited.items[item] && old.items[item].kind != Kind::kWait)
        {
            end = std::max(end, *edited.items[item] + 1);
        }
    }

    return end;
}

// The time that the items of `commands` from `first` up to `end` take, up to the last command among them. A call
// returns after its last command, and the driver doesn't read the delay after that command.
int64_t TimeToLastCommand(const Sequence& commands, uint32_t first, uint32_t end)
{
    int64_t time = 0;
    int64_t waited = 0;
    for (uint32_t item = first; item < end && item < commands.items.size(); item++)
    {
        if (commands.items[item].kind == Kind::kWait)
        {
            waited += commands.items[item].value.value;
        }
        else
        {
            time += waited;
            waited = 0;
        }
    }

    return time;
}

// `edited`'s sequence with each call's count of the commands that its range contains now. `old` is the sequence before
// the edit.
void FollowCalls(const Sequence& old, const Edited& edited, Sequence& after)
{
    if (edited.items.size() != old.items.size())
    {
        return;
    }

    for (const KonamiCallRange& range : KonamiCallRanges(old))
    {
        const std::optional<uint32_t> call = edited.items[range.call];
        if (!call)
        {
            continue;
        }

        Command& command = after.items[*call];
        const uint32_t end = NewEnd(old, edited, range, command.target);
        int count = 0;
        for (uint32_t item = command.target; item < end && item < after.items.size(); item++)
        {
            count += after.items[item].kind != Kind::kWait ? 1 : 0;
        }
        if (count == 0 || count > kMostCalledCommands)
        {
            throw std::invalid_argument("A call of Konami's driver must contain 1–" +
                                        std::to_string(kMostCalledCommands) +
                                        " commands. This edit would leave a call with " + std::to_string(count) + ".");
        }

        command.value.value = int64_t{count} * kOne;
    }
}

// The items of `commands`, sequence `sequence` of a file with `native`, where code enters: the targets and the fields
// of the tracks' starts.
std::set<uint32_t> EnteredItems(const Sequence& commands, const format::NativeArchive& native, uint32_t sequence)
{
    std::set<uint32_t> entered;
    for (const Command& command : commands.items)
    {
        if (ntdlib::commands::HasTarget(command.kind) && !command.target_outside)
        {
            entered.insert(command.target);
        }
    }

    ForEachItemPlace(native, sequence, [&entered](const ntdlib::sar::Place& place) { entered.insert(place.offset); });

    return entered;
}

// Puts a delay of no ticks into `changed`, the new sequence `sequence` of `edited`, at item `at`, before the command
// there. The later items of `edited`, the targets, the labels and the fields follow. Code that entered at the command
// enters at the delay.
void InsertWait(Edited& edited, Sequence& changed, uint32_t sequence, uint32_t at)
{
    Command wait;
    wait.kind = Kind::kWait;
    changed.items.insert(changed.items.begin() + at, std::move(wait));

    const auto later = [at](uint32_t item)
    {
        return item > at ? item + 1 : item;
    };
    for (Command& command : changed.items)
    {
        if (ntdlib::commands::HasTarget(command.kind) && !command.target_outside)
        {
            command.target = later(command.target);
        }
    }

    for (ntdlib::commands::Label& label : changed.labels)
    {
        label.item = later(label.item);
    }

    MoveItemPlaces(edited.file, sequence, later);
    for (std::optional<uint32_t>& item : edited.items)
    {
        if (item && *item >= at)
        {
            ++*item;
        }
    }

    for (uint32_t& item : edited.added)
    {
        if (item >= at)
        {
            ++item;
        }
    }
}

// What a track meets on its way through commands, without a delay.
enum class Way
{
    kOnward,   // the commands run out
    kSongLoop, // a song loop
    kLeaves,   // a delay, an end, or commands that no walk can follow
};

// What a track meets from item `first` up to item `end`. Item `end` isn't among those items. A call's commands count
// where the call stands, `depth` calls deep.
Way WayFrom(const Sequence& commands, std::span<const KonamiCallRange> ranges, uint32_t first, uint32_t end, int depth)
{
    constexpr int kMostCalls = 8;
    for (uint32_t item = first; item < end; ++item)
    {
        const Command& command = commands.items[item];
        if (command.kind == Kind::kLoopEnd)
        {
            return Way::kSongLoop;
        }

        if (command.kind == Kind::kEnd || command.kind == Kind::kData ||
            (command.kind == Kind::kWait && command.value.value != 0))
        {
            return Way::kLeaves;
        }

        if (command.kind == Kind::kCall)
        {
            const auto range =
                std::ranges::find_if(ranges, [item](const KonamiCallRange& each) { return each.call == item; });
            if (range == ranges.end() || depth == kMostCalls)
            {
                return Way::kLeaves;
            }

            const Way way = WayFrom(commands, ranges, range->first, range->end, depth + 1);
            if (way != Way::kOnward)
            {
                return way;
            }
        }
    }

    return Way::kOnward;
}

} // namespace

std::vector<KonamiCallRange> KonamiCallRanges(const Sequence& commands)
{
    std::vector<KonamiCallRange> ranges;
    for (uint32_t call = 0; call < commands.items.size(); call++)
    {
        const Command& command = commands.items[call];
        if (command.kind != Kind::kCall || command.target_outside || command.target >= commands.items.size())
        {
            continue;
        }

        const int64_t count = command.value.value / kOne;
        int64_t seen = 0;
        uint32_t item = command.target;
        while (item < commands.items.size() && seen < count && commands.items[item].kind != Kind::kData)
        {
            seen += commands.items[item].kind != Kind::kWait ? 1 : 0;
            item++;
        }

        ranges.push_back({call, command.target, item});
    }

    return ranges;
}

std::vector<KonamiCallRange> KonamiCallRangesAfter(const Sequence& old, const Sequence& after, const Edited& edited)
{
    std::vector<KonamiCallRange> ranges;
    for (const KonamiCallRange& range : KonamiCallRanges(old))
    {
        if (const std::optional<uint32_t> call = edited.items[range.call])
        {
            const uint32_t target = after.items[*call].target;
            ranges.push_back({*call, target, NewEnd(old, edited, range, target)});
        }
    }

    return ranges;
}

std::size_t KonamiLoopsWithoutWaits(const Sequence& commands, std::span<const uint32_t> entries)
{
    const uint32_t size = static_cast<uint32_t>(commands.items.size());
    std::set<uint32_t> places;
    for (const uint32_t entry : entries)
    {
        places.insert(entry);
    }

    for (uint32_t item = 0; item < size; ++item)
    {
        if (commands.items[item].kind == Kind::kLoopStart)
        {
            places.insert(item + 1);
        }
    }

    const std::vector<KonamiCallRange> ranges = KonamiCallRanges(commands);
    return static_cast<std::size_t>(
        std::ranges::count_if(places, [&commands, &ranges, size](uint32_t place)
                              { return WayFrom(commands, ranges, place, size, 0) == Way::kSongLoop; }));
}

bool KonamiStartsNoteAgain(const Command& command, const Command& note, ntdlib::konami::Revision revision)
{
    // Index 0 is the volume command (C0-CF). A PSG track's channel starts again at the new volume. The Dungeon Dice
    // Monsters revision plays the track's sound again at it on any track.
    return command.kind == Kind::kVolume && command.index == 0 &&
           (!note.sample || revision == ntdlib::konami::Revision::kDungeonDiceMonsters);
}

uint32_t KonamiLongestWait(ntdlib::konami::Revision revision)
{
    if (ntdlib::konami::EternalDuelistLayout(revision) || revision == ntdlib::konami::Revision::kDungeonDiceMonsters)
    {
        return 0xFFF;
    }

    return revision == ntdlib::konami::Revision::kUltimateMasters ? 0xFFFF : 0x1FFF;
}

bool KonamiPlaysOnNextTrack(const Command& command)
{
    return command.kind == Kind::kNote && command.sample && command.index == 1;
}

Edited KonamiKeepCallLengths(const format::File& before, Edited edited, uint32_t sequence)
{
    const Sequence& old = *before.sequences[sequence];
    if (edited.items.size() != old.items.size())
    {
        return edited;
    }

    Sequence changed = *edited.file.sequences[sequence];
    bool any = false;
    for (const KonamiCallRange& range : KonamiCallRanges(old))
    {
        const std::optional<uint32_t> call = edited.items[range.call];
        if (!call)
        {
            continue;
        }

        const uint32_t end = NewEnd(old, edited, range, changed.items[*call].target);
        const int64_t lost = TimeToLastCommand(old, range.first, range.end) -
                             TimeToLastCommand(changed, changed.items[*call].target, end);
        if (lost == 0)
        {
            continue;
        }

        // The call returns to the item right after it. That item is the delay after the call, or a command that the
        // edit put at the tick where the call returns. A new delay is inserted before such a command.
        const uint32_t delay = *call + 1;
        const bool entered = EnteredItems(changed, *edited.file.native, sequence).contains(delay);
        if (!entered && delay < changed.items.size() && changed.items[delay].kind != Kind::kWait)
        {
            InsertWait(edited, changed, sequence, delay);
        }

        if (entered || delay >= changed.items.size() || changed.items[delay].value.value + lost < 0)
        {
            throw std::invalid_argument(
                "This edit changes how long a call plays, and the delay after the call can't make up for it. Keep the "
                "last command of the call where it is.");
        }

        Command& wait = changed.items[delay];
        wait.value.value += lost;
        wait.encoding = {};
        wait.bytes.clear();
        any = true;
    }
    if (any)
    {
        edited.file.sequences[sequence] = std::move(changed);
    }

    return edited;
}

namespace
{

// Joins `command`, a delay right after the delay `joined`, to it (JoinDelays). `enters` says whether code enters at
// `command`. Throws std::invalid_argument with a message for the user where code enters at a delay after a delay of
// some ticks.
void JoinWait(Command& joined, Command& command, bool enters)
{
    if (enters && joined.value.value != 0)
    {
        throw std::invalid_argument(
            "A track or call starts at this delay, and another delay comes right before it. Can't write "
            "two delays in a row here. Keep a command between them.");
    }

    // A delay of no ticks changes nothing, and the delay beside it keeps its bytes.
    if (enters || joined.value.value == 0)
    {
        joined = std::move(command);
    }
    else if (command.value.value != 0)
    {
        joined.value.value += command.value.value;
        joined.encoding = {};
        joined.bytes.clear();
    }
}

// The items of `changed` with each delay joined to the delay before it, where code doesn't enter at it, and a delay of
// 0 before each command without a delay (KonamiWritable).
// `firsts` lists where code that enters at each old item enters: at a new delay before a command. `places` contains the
// new index of each old item.
struct JoinedDelays
{
    Sequence after;
    std::vector<uint32_t> firsts;
    std::vector<uint32_t> places;
};

// JoinDelays joins them. `entered` lists the items where code enters. Throws std::invalid_argument with a message for
// the user for delays that the driver can't take.
JoinedDelays JoinDelays(Sequence& changed, const std::set<uint32_t>& entered)
{
    Sequence after;
    after.labels = changed.labels;
    std::vector<uint32_t> firsts;
    std::vector<uint32_t> places;
    const auto zero_wait = []
    {
        Command wait;
        wait.kind = Kind::kWait;
        return wait;
    };
    enum class Last
    {
        kNothing, // the start, or what the driver reads no delay after
        kWait,
        kCommand,
    };
    Last last = Last::kNothing;
    for (uint32_t item = 0; item < changed.items.size(); item++)
    {
        Command& command = changed.items[item];
        firsts.push_back(static_cast<uint32_t>(after.items.size()));
        if (command.kind == Kind::kData)
        {
            if (last == Last::kCommand)
            {
                throw std::invalid_argument("Track commands can't run into bytes that no track reads.");
            }

            places.push_back(static_cast<uint32_t>(after.items.size()));
            after.items.push_back(std::move(command));
            last = Last::kNothing;
            continue;
        }

        if (command.kind == Kind::kWait)
        {
            if (last == Last::kWait)
            {
                JoinWait(after.items.back(), command, entered.contains(item));
                firsts.back() = static_cast<uint32_t>(after.items.size() - 1);
                places.push_back(static_cast<uint32_t>(after.items.size() - 1));
                continue;
            }

            places.push_back(static_cast<uint32_t>(after.items.size()));
            after.items.push_back(std::move(command));
            last = Last::kWait;
            continue;
        }

        // A command takes the delay before it. Where it has none, a delay of 0 is inserted, and code enters there. Code
        // that enters reads a delay first.
        if (last == Last::kWait && entered.contains(item))
        {
            throw std::invalid_argument(
                "A track or call starts at this tick, and the command before it was removed. Can't write the two "
                "delays in a row that this leaves. Keep a command there.");
        }

        if (last != Last::kWait)
        {
            after.items.push_back(zero_wait());
        }

        places.push_back(static_cast<uint32_t>(after.items.size()));
        last = EndsTrack(command) ? Last::kNothing : Last::kCommand;
        after.items.push_back(std::move(command));
    }

    firsts.push_back(static_cast<uint32_t>(after.items.size()));
    places.push_back(static_cast<uint32_t>(after.items.size()));
    if (last == Last::kCommand)
    {
        throw std::invalid_argument("Track commands must end with a track end or a song loop.");
    }

    return {std::move(after), std::move(firsts), std::move(places)};
}

// Throws std::invalid_argument with a message for the user where revision `revision` of Konami's driver can't encode
// `after`, sequence `sequence` of `native`, for each piece that contains it (KonamiWritable).
void CheckPieces(const format::NativeArchive& native, const Sequence& after, uint32_t sequence,
                 ntdlib::konami::Revision revision)
{
    // The bytes have to hold each command where its track reads it, or an export would refuse the edit much later.
    const std::vector<std::vector<uint32_t>> entries =
        ntdlib::konami::TrackEntries(*native.pieces, native.fields, revision);
    for (std::size_t piece = 0; piece < native.pieces->size(); ++piece)
    {
        const auto* held = std::get_if<ntdlib::sar::SequencePiece>(&(*native.pieces)[piece]);
        if (held == nullptr || held->sequence != sequence)
        {
            continue;
        }

        try
        {
            ntdlib::konami::EncodeSequence(after, entries[piece], revision);
        }
        catch (const std::invalid_argument&)
        {
            throw std::invalid_argument(
                "This revision of Konami's driver can't encode this command with these values.");
        }
    }
}

// `edited` as the driver's bytes can hold it. `edited` is an edit of sequence `sequence` of `before`, a file of
// Konami's driver. `Edited::items` and `Edited::added` follow the new items, and so do the targets, the labels and the
// fields of the tracks' starts. An `Edited::items` without an entry for each item of the sequence leaves the calls'
// counts as they are. Throws std::invalid_argument with a message for the user for a delay that the bytes can't hold, a
// call that would play no command or more than 255, and a command that the revision has no bytes for.
Edited KonamiWritable(const format::File& before, Edited edited, uint32_t sequence)
{
    const ntdlib::konami::Revision revision = *KonamiOf(ConsoleOf(edited.file));
    Sequence changed = *edited.file.sequences[sequence];
    FollowCalls(*before.sequences[sequence], edited, changed);

    // The items where code enters the sequence. A delay there can't join the delay before it.
    const std::set<uint32_t> entered = EnteredItems(changed, *edited.file.native, sequence);

    auto [after, firsts, places] = JoinDelays(changed, entered);

    const uint32_t longest = KonamiLongestWait(revision);
    for (const Command& command : after.items)
    {
        if (command.kind == Kind::kWait && command.value.value / kOne > longest)
        {
            throw std::invalid_argument("A delay of Konami's driver can be at most " + std::to_string(longest) +
                                        " ticks. Keep a command in the middle of this one.");
        }
    }

    RenumberItems(after, edited, sequence, firsts, places);

    CheckPieces(*edited.file.native, after, sequence, revision);
    edited.file.sequences[sequence] = std::move(after);

    return edited;
}

// Throws std::invalid_argument with a message for the user when `command` can't go before item `at` of `commands` on
// Konami's driver (InsertCommand).
void CheckKonamiInsert(const ntdlib::commands::Sequence& commands, uint32_t at, const Command& command)
{
    // A new note takes its sample in the Key Editor. A call and a loop point hold more than the List Editor's
    // columns show.
    switch (command.kind)
    {
    case Kind::kNote:
        throw std::invalid_argument(
            "Use the Key Editor to add notes to a song of Konami's driver. The Key Editor also sets the sample.");
    case Kind::kCall:
    case Kind::kLoopStart:
    case Kind::kLoopEnd:
        throw std::invalid_argument("Can't insert calls or loops in a song of Konami's driver.");
    default:
        break;
    }

    if (commands.items[at].kind == Kind::kData)
    {
        throw std::invalid_argument("No track reads these bytes. Can't insert a command before them.");
    }
}

// The track that plays the PSG's wave channel.
constexpr int kKonamiWaveTrack = 2;

// The edits on Konami's driver. Where the defaults of DriverEdits apply:
// - A note of Konami's driver has no length, and never waits.
// - The song table starts each track of a song.
// - The song table contains the starts of a song's tracks.
// - A loop's end jumps back to the song's loop point for ever.
class KonamiDriverEdits final : public DriverEdits
{
public:
    Edited Writable(const format::File& before, Edited edited, uint32_t sequence) const override
    {
        return KonamiWritable(before, std::move(edited), sequence);
    }

    Edited Placed(const format::File& before, Edited edited, uint32_t sequence) const override
    {
        // Every edit of notes or settings is checked as it leaves the commands, whatever it was.
        CheckKonamiVolumeStarts(before, edited, sequence);

        return KonamiKeepCallLengths(before, std::move(edited), sequence);
    }

    Edited EditNotes(const format::File& file, uint32_t sequence, std::span<const NoteEdit> edits,
                     std::span<const NewNote> additions) const override
    {
        return edit::Writable(file, Placed(file, EditKonamiNotes(file, sequence, edits, additions), sequence),
                              sequence);
    }

    std::optional<ValueRange> Range(const song::Console& console, int track, const Command& command,
                                    ntdlib::commands::Source source, bool time) const override
    {
        return KonamiRange(command, source, time, *song::KonamiRevisionOf(console.driver), track);
    }

    std::size_t LoopsWithoutWaits(const Sequence& sequence, std::span<const uint32_t> entries) const override
    {
        return KonamiLoopsWithoutWaits(sequence, entries);
    }

    // No command of the driver starts a track.
    bool OpensTrack(const Sequence&, const Command& item, int track) const override
    {
        return item.kind == Kind::kOpenTrack && item.index == track;
    }

    TrackEdit AddTrack(const format::File& file, std::span<const std::optional<SoundPlace>> places, uint32_t sound,
                       const song::Sequence&, std::optional<Program>) const override
    {
        return AddKonamiTrack(file, places, sound);
    }

    TrackEdit DuplicateTrack(const format::File& file, std::span<const std::optional<SoundPlace>> places,
                             uint32_t sound, int track) const override
    {
        return DuplicateKonamiTrack(file, places, sound, track);
    }

    CopiedTrack CopyTrack(const format::File& file, std::span<const std::optional<SoundPlace>> places, uint32_t sound,
                          const song::Sequence& song, int track) const override
    {
        return CopyKonamiTrack(file, places, sound, song, track);
    }

    TrackEdit PasteTrack(const format::File& file, std::span<const std::optional<SoundPlace>> places, uint32_t sound,
                         const song::Sequence&, const CopiedTrack& copied) const override
    {
        return PasteKonamiTrack(file, places, sound, copied);
    }

    format::File RemoveTrack(const format::File& file, std::span<const std::optional<SoundPlace>> places,
                             uint32_t sound, int track) const override
    {
        return RemoveKonamiTrack(file, places, sound, track);
    }

    bool NotesSetVolume() const override
    {
        return true;
    }

    void CheckMixEdit(const ntdlib::commands::Sequence& /*commands*/, const std::vector<uint32_t>& /*offsets*/,
                      std::span<const song::Stretch> /*stretches*/, song::Mix setting, bool run_empty) const override
    {
        // A new volume command would sound on a PSG channel, and a sample track's next note sets its volume anyway.
        if (setting == song::Mix::kVolume && run_empty)
        {
            throw std::invalid_argument(
                "This track starts no notes and has no volume to change. Another track can play notes on this "
                "channel through its pan setting. Change the volume on that track.");
        }
    }

    void FollowPan(const song::Console& console, Command& command) const override
    {
        // A pan of Konami's driver lowers one side from the command's level. The level has to hold the position.
        const std::optional<ntdlib::konami::Revision> konami = KonamiOf(console);
        if (konami && !ntdlib::konami::UltimateMasters(*konami) && command.velocity != 0)
        {
            const int64_t position =
                ntdlib::commands::FromUnit(command.value.value, ntdlib::konami::ScaleOf(Kind::kPan, *konami));
            command.velocity = std::max(command.velocity, std::abs(position) * kOne);
        }
    }

    // A track without notes that runs no commands, or whose commands end where they start, takes no note
    // (kKonamiNoRoom).
    std::optional<NewNote> PlaceDrawnNote(const format::File& file, uint32_t sequence, const song::Track& track,
                                          const std::optional<song::Stretch>& stretch,
                                          const DrawnKeyNote& drawn) const override
    {
        if (!stretch && track.stretches.empty())
        {
            throw std::invalid_argument(std::string(kKonamiNoRoom));
        }
        if (!stretch)
        {
            return std::nullopt;
        }

        std::optional<KonamiSampleNote> copied;
        if (drawn.sample_semitones)
        {
            copied = KonamiSampleNote{static_cast<uint16_t>(drawn.program), *drawn.sample_semitones};
        }

        return NewKonamiNote(file, sequence, track, *stretch, drawn.start, drawn.key, drawn.velocity, drawn.length,
                             copied);
    }

    void PrepareInsert(const format::File& /*file*/, uint32_t /*sequence*/, const ntdlib::commands::Sequence& commands,
                       uint32_t at, Command& command) const override
    {
        CheckKonamiInsert(commands, at, command);
    }

    // The driver stores the semitones of a sample as a signed byte.
    std::optional<int> SampleTranspose(const Command& before, const Command& command) const override
    {
        if (!before.sample)
        {
            return std::nullopt;
        }

        return static_cast<int8_t>(command.key) - static_cast<int8_t>(before.key);
    }

    bool NotesPlayOnNextTrack() const override
    {
        return true;
    }

    // Before a track has notes, a track whose notes select their samples has no instrument.
    void CheckOpeningInstrument(const song::Console& console, int track) const override
    {
        if (!ntdlib::konami::IsPsgTrack(track) && *KonamiOf(console) != ntdlib::konami::Revision::kDungeonDiceMonsters)
        {
            throw std::invalid_argument(
                "On this track of Konami's driver, each note selects its sample. The track has no instrument until it "
                "has notes.");
        }
    }

    std::optional<std::string> RefusedBankSelect() const override
    {
        return "A song of Konami's driver uses a single sample table. Its tracks can't select another.";
    }

    // F6, a program change of index 2, selects an entry of a table that the game passes to the driver.
    bool SelectsProgram(const song::Console& /*console*/, int /*track*/, const Command& command) const override
    {
        return command.index < 2;
    }

    bool NotesSelectSamples() const override
    {
        return true;
    }

    // The wave channel selects a wave, and the other PSG channels a duty. The first revision reads a wave and a duty
    // from the same bytes.
    uint8_t ProgramIndex(const song::Console& console, int track) const override
    {
        return track == kKonamiWaveTrack && *KonamiOf(console) != ntdlib::konami::Revision::kUltimateMasters ? 1 : 0;
    }
};

} // namespace

const DriverEdits& KonamiEdits()
{
    static const KonamiDriverEdits edits;
    return edits;
}

} // namespace hokuto::edit
