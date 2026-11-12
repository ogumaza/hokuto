// SPDX-License-Identifier: MIT

#include "edit/bytecode.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "ntdlib/agb/rom.h"
#include "ntdlib/commands/sequence.h"
#include "ntdlib/dse/commands.h"
#include "ntdlib/dse/formats.h"
#include "ntdlib/mp2k/commands.h"
#include "ntdlib/ntr/snd/commands.h"
#include "ntdlib/nwsnd/commands.h"
#include "ntdlib/sar/layout.h"

#include "edit/converter.h"
#include "edit/drivers.h"
#include "edit/engine_edits.h"
#include "edit/engine_loops.h"
#include "format/archive.h"
#include "song/archive.h"
#include "song/track_flow.h"

namespace hokuto::edit
{

song::Console ConsoleOf(const format::File& file)
{
    if (!file.native)
    {
        throw std::invalid_argument("The file contains no sound archive.");
    }

    switch (format::FamilyOf(*file.native))
    {
    case format::Family::kNw4r:
        return song::Console{.engine = format::Engine::kWii, .wii = format::BuildOfWiiArchive(*file.native)};
    case format::Family::kMp2k:
        return song::Console{.engine = format::Engine::kGba, .note_arguments = file.native->gba_note_arguments};
    case format::Family::kNitroComposer:
        return song::Console{.engine = format::Engine::kDs};
    case format::Family::kDse:
        return song::Console{.engine = format::Engine::kDse};
    case format::Family::kGbaDriver:
        return song::Console{.engine = file.native->engine,
                             .driver = &song::RulesOf(file.native->engine, file.native->driver->revision)};
    case format::Family::kNintendoWare:
        break;
    }

    return song::Console{.engine = file.native->engine};
}

std::string SkippedMessage(const song::Console& console, const std::string& what)
{
    if (console.wii)
    {
        return "This build of NW4R SND doesn't support this " + what +
               ". Wii sequences have no bank select and no LFO phase, curve or period. Older builds are missing some "
               "commands.";
    }
    if (console.engine == format::Engine::kGba)
    {
        return "MusicPlayer2000 doesn't support this " + what + ".";
    }
    if (console.engine == format::Engine::kDs)
    {
        return "NITRO-Composer doesn't support this " + what +
               ". DS tracks have a single LFO with no curve, phase or period, and no sends, bank select or filters.";
    }
    if (console.engine == format::Engine::kDse)
    {
        return "DSE doesn't support this " + what + ". DSE tracks have no jumps, calls, sends or variables.";
    }
    if (console.driver)
    {
        return "This revision of " + std::string(console.driver->Title()) + " doesn't support this " + what + ".";
    }

    return "The 3DS sequence player ignores this " + what +
           ". 3DS tracks have a single LFO with no curve, phase or period, and send only to FX A and FX B.";
}

void RefuseGba(const format::File& file, const char* message)
{
    if (file.native && file.native->engine == format::Engine::kGba)
    {
        throw std::invalid_argument(message);
    }
}

const ntdlib::commands::Sequence& SequenceToEdit(const format::File& file, uint32_t sequence)
{
    if (!file.native || sequence >= file.sequences.size())
    {
        throw std::invalid_argument("Only sequences in a sound archive can be edited.");
    }

    return *file.sequences[sequence];
}

std::optional<uint32_t> WholeTicks(const ntdlib::commands::Argument& value)
{
    using ntdlib::commands::kOne;
    if (value.source != ntdlib::commands::Source::kValue || value.value < 0 || value.value % kOne != 0)
    {
        return std::nullopt;
    }

    return static_cast<uint32_t>(value.value / kOne);
}

std::vector<uint32_t> Offsets(const ntdlib::commands::Sequence& sequence, const song::Console& console)
{
    if (console.engine == format::Engine::kDs)
    {
        return ntdlib::ntr::snd::EncodeSequence(sequence).offsets;
    }
    if (console.engine == format::Engine::kDse)
    {
        return ntdlib::dse::EncodeTracks(sequence).offsets;
    }

    if (console.driver)
    {
        return console.driver->ItemOffsets(sequence);
    }

    return console.engine == format::Engine::kGba
               ? ntdlib::mp2k::EncodeSequence(sequence, ntdlib::agb::kRomBase, console.note_arguments).offsets
               : ntdlib::nwsnd::EncodeSequence(sequence, song::GrammarOf(console)).offsets;
}

std::optional<uint32_t> ItemAtOffset(const std::vector<uint32_t>& offsets, uint32_t offset)
{
    if (offsets.empty())
    {
        return std::nullopt;
    }

    const auto found = std::lower_bound(offsets.begin(), offsets.end() - 1, offset);
    if (found == offsets.end() - 1 || *found != offset)
    {
        return std::nullopt;
    }

    return static_cast<uint32_t>(found - offsets.begin());
}

bool Fixed(const ntdlib::commands::Sequence& sequence)
{
    const auto fixed = [](const ntdlib::commands::Command& item)
    {
        return item.target_outside || item.kind == ntdlib::commands::Kind::kData;
    };
    return std::any_of(sequence.items.begin(), sequence.items.end(), fixed);
}

bool KeepsPlaces(const format::File& file, const ntdlib::commands::Sequence& sequence)
{
    const song::Console console = ConsoleOf(file);
    return console.engine != format::Engine::kGba && console.engine != format::Engine::kDse && !console.driver &&
           Fixed(sequence);
}

bool RunsOn(const ntdlib::commands::Command& command)
{
    using ntdlib::commands::Kind;
    const bool stops = command.kind == Kind::kJump || command.kind == Kind::kReturn || command.kind == Kind::kEnd;
    return !stops || command.conditional;
}

namespace
{

// The items of a sequence that the player reaches outside every call, and those that it reaches inside a call.
struct Flow
{
    std::vector<bool> outside;
    std::vector<bool> inside;
};

// The items of `sequence` that the player reaches from the items of `starts`, outside every call and inside a call.
// Reached gives the same items. A track starts outside every call. A return outside every call has no call to return
// to, and the player runs on past it.
Flow FlowFrom(const ntdlib::commands::Sequence& sequence, std::span<const uint32_t> starts, bool track_starts)
{
    using ntdlib::commands::Kind;
    const std::vector<ntdlib::commands::Command>& items = sequence.items;
    Flow flow{std::vector<bool>(items.size(), false), std::vector<bool>(items.size(), false)};
    std::vector<std::pair<uint32_t, bool>> next;
    for (const uint32_t start : starts)
    {
        next.emplace_back(start, false);
    }
    while (!next.empty())
    {
        const auto [item, in_call] = next.back();
        next.pop_back();
        std::vector<bool>& reached = in_call ? flow.inside : flow.outside;
        if (item >= items.size() || reached[item])
        {
            continue;
        }

        // A jump goes on only when it runs only if the last comparison held. A return inside a call and an end do the
        // same.
        reached[item] = true;
        const ntdlib::commands::Command& command = items[item];
        const bool target = ntdlib::commands::HasTarget(command.kind) && !command.target_outside;
        if (target && command.kind == Kind::kOpenTrack)
        {
            if (track_starts)
            {
                next.emplace_back(command.target, false);
            }
        }
        else if (target)
        {
            next.emplace_back(command.target, in_call || command.kind == Kind::kCall);
        }

        const bool returns = command.kind == Kind::kReturn && !command.conditional && in_call;
        if (command.kind != Kind::kData && (RunsOn(command) || (command.kind == Kind::kReturn && !returns)))
        {
            next.emplace_back(item + 1, in_call);
        }
    }

    return flow;
}

} // namespace

bool RunsInto(const format::File& file, uint32_t sequence, uint32_t item)
{
    using ntdlib::commands::Kind;
    const ntdlib::commands::Sequence& commands = *file.sequences[sequence];
    if (item == 0 || item > commands.items.size())
    {
        return false;
    }

    const ntdlib::commands::Command& before = commands.items[item - 1];
    const song::Console console = ConsoleOf(file);
    if ((console.engine == format::Engine::kGba || console.engine == format::Engine::kDse || console.driver) &&
        before.kind == Kind::kData)
    {
        // MusicPlayer2000's decoder keeps as data only the bytes that no track reaches, such as the padding after a
        // track's end. The decoder of DSE keeps the start of a track and the bytes after its end, and a track starts
        // after its start.
        return false;
    }

    if (EditsOf(console).IsTableEntry(before))
    {
        // No track runs a table of tracks' entries, such as the header of a song of Nintendo R&D2's driver.
        return false;
    }

    if (RunsOn(before) || before.kind != Kind::kReturn)
    {
        return RunsOn(before);
    }

    // A return runs on where a sound reaches it outside every call. A sound starts at the bytecode's first item, or
    // where a field of the archive's tables names an item.
    std::vector<uint32_t> starts = {0};
    for (const ntdlib::sar::Field& field : file.native->fields)
    {
        if (NamesItemOf(*file.native, field.to, sequence))
        {
            starts.push_back(field.to.offset);
        }
    }

    return FlowFrom(commands, starts, true).outside[item - 1];
}

// DSE's commands that select a bank.
constexpr uint16_t kDseFirstBankSelect = ntdlib::dse::kBank;
constexpr uint16_t kDseLastBankSelect = ntdlib::dse::kBankLow;

namespace
{

// The item of `edited`, sequence `sequence` of `file`, a timeline, after the commands that open track `track`, whose
// run starts at item `first` (InsertAtOpening). Throws std::invalid_argument with a message for the user for a track
// that isn't part of the timeline.
uint32_t TimelineOpening(const format::File& file, uint32_t sequence, const ntdlib::commands::Sequence& edited,
                         uint32_t first, int track)
{
    const std::optional<uint8_t> number = TrackRunFrom(edited, first);
    const std::optional<uint32_t> opening =
        number ? TrackOpening(edited, file.timelines->at(sequence), *number) : std::nullopt;
    if (!opening)
    {
        throw std::invalid_argument("Track " + std::to_string(track) + " isn't part of the timeline.");
    }

    return *opening;
}

// The item of `edited` after the bank selects of DSE among the commands from item `first` that run before a note, a
// wait or an end (InsertAtOpening).
uint32_t AfterDseBankSelects(const ntdlib::commands::Sequence& edited, uint32_t first)
{
    using ntdlib::commands::Command;
    using ntdlib::commands::Kind;

    for (uint32_t item = first; item < edited.items.size(); item++)
    {
        const Command& command = edited.items[item];
        if (command.kind == Kind::kNote || command.kind == Kind::kWait || command.kind == Kind::kEnd)
        {
            break;
        }

        const uint16_t opcode = command.encoding.opcode;
        if (command.kind == Kind::kOther && opcode >= kDseFirstBankSelect && opcode <= kDseLastBankSelect)
        {
            first = item + 1;
        }
    }

    return first;
}

// The new place of an item after InsertAtOpening puts `count` commands and `jumps` jumps past them at item `first`.
struct OpeningMove
{
    uint32_t first = 0;
    uint32_t jumps = 0;
    uint32_t count = 0;

    // The new index of item `item`. With `opening`, an item at `first` moves to the first new command.
    uint32_t operator()(uint32_t item, bool opening) const
    {
        if (item < first)
        {
            return item;
        }

        return item == first && opening ? first + jumps : item + jumps + count;
    }
};

// Moves the targets and the labels of `edited` by `moved` for the new commands at the opening of track `track`. `edits`
// is the class of the engine family of the track. With `opening_labels`, the labels at the opening go on naming the new
// commands (InsertAtOpening).
void MoveTargetsAndLabels(ntdlib::commands::Sequence& edited, const EngineEdits& edits, int track, bool opening_labels,
                          const OpeningMove& moved)
{
    using ntdlib::commands::Command;

    const uint32_t first = moved.first;

    // A command that opens the track leads to the new commands. On the GBA's other drivers, the driver defines the
    // commands that open a track (DriverEdits::OpensTrack). The song header of Nintendo R&D2's driver opens track 0
    // too, and an entry of a sound effects' sequence opens a sound effect's track 0.
    const auto opens = [track, &edits, &edited](const Command& item)
    {
        return edits.OpensTrack(edited, item, track);
    };

    // A label at `first` that a jump or a call reaches goes on naming the command that they reach, past the new
    // commands. The label then keeps its jumps. The sound's start stays at the new commands.
    const auto reaches = [first, &opens](const Command& item)
    {
        return ntdlib::commands::HasTarget(item.kind) && !item.target_outside && item.target == first && !opens(item);
    };
    const bool labels_at_opening = opening_labels && std::none_of(edited.items.begin(), edited.items.end(), reaches);
    for (Command& item : edited.items)
    {
        if (ntdlib::commands::HasTarget(item.kind) && !item.target_outside)
        {
            item.target = moved(item.target, opens(item));
        }
    }

    for (ntdlib::commands::Label& label : edited.labels)
    {
        label.item = moved(label.item, labels_at_opening);
    }
}

// Moves the places of the archive's tables in `file` that name the items of sequence `sequence` by `moved`. With
// `opening_labels`, a place at the opening keeps naming the new commands (InsertAtOpening).
void MoveFieldPlaces(format::File& file, uint32_t sequence, bool opening_labels, const OpeningMove& moved)
{
    // A field of the archive's tables measures a sound's start from the bytecode's first item. That item stays first.
    const auto move_places = [&](format::NativeArchive& native)
    {
        for (ntdlib::sar::Field& field : native.fields)
        {
            if (NamesItemOf(native, field.from, sequence) && field.from.offset != 0)
            {
                field.from.offset = moved(field.from.offset, false);
            }

            if (NamesItemOf(native, field.to, sequence))
            {
                field.to.offset = moved(field.to.offset, opening_labels);
            }
        }
    };
    file.native.Edit(move_places);
}

} // namespace

OpeningInsertion InsertAtOpening(format::File& file, uint32_t sequence, uint32_t first, int track,
                                 std::vector<ntdlib::commands::Command> commands)
{
    using ntdlib::commands::Command;
    using ntdlib::commands::Kind;
    ntdlib::commands::Sequence edited = SequenceToEdit(file, sequence);
    const song::Console console = ConsoleOf(file);

    // A timeline's track takes its first settings after the commands that open it. Every path through the track runs
    // them, and the labels there move past them. On the GBA, a field of the song's header starts each track, and the
    // field stays at the new commands.
    const bool timeline = file.timelines && file.timelines->contains(sequence);
    const bool driver = console.driver != nullptr;
    bool opening_labels = track == 0 || console.engine == format::Engine::kGba || driver;
    if (timeline)
    {
        first = TimelineOpening(file, sequence, edited, first, track);
        opening_labels = false;
    }
    // On the DS, track 0 runs the allocation of tracks at the sequence's first command, and then its first command. The
    // new commands go after the allocation and require no jump. Elsewhere, NITRO-Composer reads 0xFE as no command, and
    // the mask's bytes as commands. A target at the allocation would then run the new commands, and the edit refuses
    // it.
    const bool after_allocation = console.engine == format::Engine::kDs && track == 0 && first == 1 &&
                                  !edited.items.empty() && edited.items[0].kind == Kind::kAllocateTracks;
    const auto at_allocation = [](const Command& item)
    {
        return ntdlib::commands::HasTarget(item.kind) && !item.target_outside && item.target == 0;
    };
    if (after_allocation && std::any_of(edited.items.begin(), edited.items.end(), at_allocation))
    {
        throw std::invalid_argument(
            "A command in the sequence jumps to the track allocation at its start. Can't add a command at the "
            "start of track 0.");
    }
    // A channel of a DSE sound effect starts without a bank, and a program selected then stays at 0 (the driver's
    // SelectProgram). The new commands go after the bank's selects at the track's opening. DSE has no jumps.
    const bool dse = console.engine == format::Engine::kDse;
    if (dse)
    {
        first = AfterDseBankSelects(edited, first);
    }
    // A call of Konami's driver counts its commands, and its tracks take no jumps (DriverEdits::JumpsPastOpenings).
    const bool driver_jumps = EditsOf(console).JumpsPastOpenings();
    const uint32_t jumps =
        !timeline && !after_allocation && !dse && driver_jumps && RunsInto(file, sequence, first) ? 1 : 0;
    const auto count = static_cast<uint32_t>(commands.size());
    const OpeningMove moved{first, jumps, count};
    MoveTargetsAndLabels(edited, EditsOf(console), track, opening_labels, moved);

    // The new commands, after the jump past them.
    std::vector<Command> added;
    if (jumps != 0)
    {
        added.push_back(Command{.kind = Kind::kJump, .target = first + 1 + count});
    }

    added.insert(added.end(), std::make_move_iterator(commands.begin()), std::make_move_iterator(commands.end()));
    edited.items.insert(edited.items.begin() + first, added.begin(), added.end());
    file.sequences[sequence] = std::move(edited);
    MoveFieldPlaces(file, sequence, opening_labels, moved);

    return {first, jumps + count};
}

std::vector<bool> Reached(const ntdlib::commands::Sequence& sequence, uint32_t start, bool track_starts)
{
    const Flow flow = FlowFrom(sequence, {&start, 1}, track_starts);
    std::vector<bool> reached(sequence.items.size(), false);
    for (std::size_t item = 0; item < reached.size(); item++)
    {
        reached[item] = flow.outside[item] || flow.inside[item];
    }

    return reached;
}

namespace loop_detail
{

// CommandsWithoutWayOut on DSE. A track runs straight on, but for its repeats. The repeats all end. Only its end jumps
// back. The commands from the track's last wait to its end have no way out when a loop point is among them.
std::size_t DseCommandsWithoutWayOut(const ntdlib::commands::Sequence& sequence)
{
    using ntdlib::commands::Command;
    using ntdlib::commands::Kind;
    using ntdlib::commands::Source;

    constexpr uint16_t kWaitForVoices = ntdlib::dse::kWaitForVoices;
    constexpr uint16_t kEndTrack = ntdlib::dse::kEndTrack;
    constexpr uint16_t kLoopPoint = ntdlib::dse::kLoopPoint;

    std::size_t count = 0;
    std::size_t way_out = 0;
    bool loops = false;
    for (std::size_t item = 0; item < sequence.items.size(); ++item)
    {
        const Command& command = sequence.items[item];
        const uint16_t opcode = command.encoding.opcode;
        if (command.kind == Kind::kOther && opcode == kLoopPoint)
        {
            loops = true;
        }
        else if (command.kind == Kind::kData ||
                 (command.kind == Kind::kWait &&
                  (command.value.source != Source::kValue || command.value.value != 0)) ||
                 (command.kind == Kind::kOther && opcode == kWaitForVoices))
        {
            way_out = item;
            loops = false;
        }
        else if (command.kind == Kind::kEnd)
        {
            if (loops && (opcode == 0 || opcode == kEndTrack))
            {
                count += item - way_out;
            }

            way_out = item;
            loops = false;
        }
    }

    return count;
}

} // namespace loop_detail

namespace
{

// Whether each note of `sequence`, a DS sequence whose tracks start at `starts`, waits for its length
// (CommandsWithoutWayOut).
std::vector<bool> NotesWait(const ntdlib::commands::Sequence& sequence, std::span<const uint32_t> starts)
{
    using ntdlib::commands::Command;
    using ntdlib::commands::Kind;
    using ntdlib::commands::Source;

    const std::size_t size = sequence.items.size();
    std::vector<bool> notes_wait(size, false);
    const auto after = [](const Command& command, uint32_t, const std::vector<int>& before)
    {
        if (command.kind != Kind::kNoteWait)
        {
            return before;
        }

        return command.value.source != Source::kValue ? std::vector<int>{0, 1}
                                                      : std::vector<int>{command.value.value != 0 ? 1 : 0};
    };
    constexpr std::size_t kMostStates = 8 * 1024 * 1024;
    const auto states = song::TrackStates(sequence, std::span<const uint32_t>(starts), 1, after, kMostStates);
    for (uint32_t item = 0; item < size; ++item)
    {
        notes_wait[item] = !states || (*states)[item].empty() || (*states)[item].back() == 1;
    }

    return notes_wait;
}

// Whether each item of `sequence` has a way out: whether it can reach a wait or an end from `starts`
// (CommandsWithoutWayOut). `notes_wait` says whether each note waits for its length.
std::vector<bool> WaysOut(const ntdlib::commands::Sequence& sequence, std::span<const uint32_t> starts,
                          const std::vector<bool>& notes_wait)
{
    using ntdlib::commands::Command;
    using ntdlib::commands::Kind;
    using ntdlib::commands::Source;

    const std::vector<Command>& items = sequence.items;
    const std::size_t size = items.size();
    const song::TrackPaths paths = song::TrackPathsOf(sequence, starts);
    std::vector<std::vector<uint32_t>> comes_from(size);
    std::vector<bool> way_out(size, false);
    std::vector<uint32_t> pending;
    const auto mark = [&way_out, &pending](uint32_t item)
    {
        if (!way_out[item])
        {
            way_out[item] = true;
            pending.push_back(item);
        }
    };
    for (uint32_t item = 0; item < size; ++item)
    {
        const Command& command = items[item];
        const bool waits =
            (command.kind == Kind::kWait && (command.value.source != Source::kValue || command.value.value != 0)) ||
            (command.kind == Kind::kNote && notes_wait[item]);
        std::vector<uint32_t> next = paths.next[item];

        // MP2K's repeats and comparisons jump to their targets too.
        if (ntdlib::commands::HasTarget(command.kind) && command.kind != Kind::kOpenTrack && !command.target_outside)
        {
            next.push_back(command.target);
        }

        const bool leaves = waits || command.conditional || command.kind == Kind::kEnd || command.kind == Kind::kData ||
                            next.empty() || std::ranges::any_of(next, [size](uint32_t to) { return to >= size; });
        if (leaves)
        {
            mark(item);
            continue;
        }

        for (const uint32_t to : next)
        {
            comes_from[to].push_back(item);
        }
    }
    while (!pending.empty())
    {
        const uint32_t item = pending.back();
        pending.pop_back();
        for (const uint32_t from : comes_from[item])
        {
            mark(from);
        }
    }

    return way_out;
}

} // namespace

namespace loop_detail
{

std::size_t CommandsWithoutWayOutByJumps(const ntdlib::commands::Sequence& sequence,
                                         std::span<const uint32_t> sound_starts, bool ds)
{
    using ntdlib::commands::Command;
    using ntdlib::commands::Kind;
    using ntdlib::commands::Source;

    const std::vector<Command>& items = sequence.items;
    const std::size_t size = items.size();
    std::vector<uint32_t> starts = {0};
    starts.insert(starts.end(), sound_starts.begin(), sound_starts.end());
    for (const ntdlib::commands::Label& label : sequence.labels)
    {
        starts.push_back(label.item);
    }

    std::erase_if(starts, [size](uint32_t start) { return start >= size; });

    // On the DS, each track starts with its note wait on. A note waits where the analysis finds the note wait on, or
    // where no start reaches it.
    const std::vector<bool> notes_wait = ds ? NotesWait(sequence, starts) : std::vector<bool>(size, false);

    // The ways out spread back along the paths: a command that can reach a wait or an end has one.
    const std::vector<bool> way_out = WaysOut(sequence, starts, notes_wait);

    return static_cast<std::size_t>(std::ranges::count(way_out, false));
}

} // namespace loop_detail

std::size_t CommandsWithoutWayOut(const ntdlib::commands::Sequence& sequence, const song::Console& console,
                                  std::span<const uint32_t> sound_starts)
{
    return EditsOf(console).LoopsWithoutWaits(sequence, sound_starts);
}

void RefuseLoopsWithoutWaits(const format::File& before, const format::File& after)
{
    if (!after.native || (after.native->engine != format::Engine::kGba && after.native->engine != format::Engine::kDs &&
                          after.native->engine != format::Engine::kDse && !after.native->driver))
    {
        return;
    }

    const song::Console console = ConsoleOf(after);
    const auto count = [&console](const format::File& file, uint32_t sequence)
    {
        std::vector<uint32_t> starts;
        ForEachItemPlace(*file.native, sequence,
                         [&starts](const ntdlib::sar::Place& place) { starts.push_back(place.offset); });
        return CommandsWithoutWayOut(*file.sequences[sequence], console, starts);
    };
    for (uint32_t sequence = 0; sequence < after.sequences.size(); sequence++)
    {
        const bool held = sequence < before.sequences.size();
        if (held && after.sequences[sequence].SharesWith(before.sequences[sequence]))
        {
            continue;
        }

        if (count(after, sequence) > (held ? count(before, sequence) : 0))
        {
            throw std::invalid_argument(
                "This edit would leave a track in a loop with no wait. The console would hang.");
        }
    }
}

std::string UniqueLabel(const std::vector<ntdlib::commands::Label>& labels, const std::string& name)
{
    const auto taken = [&labels](const std::string& candidate)
    {
        const auto same = [&candidate](const ntdlib::commands::Label& label)
        {
            return label.name == candidate;
        };
        return std::any_of(labels.begin(), labels.end(), same);
    };

    std::string unique = name;
    for (int number = 2; taken(unique); number++)
    {
        unique = name + "_" + std::to_string(number);
    }

    return unique;
}

bool NamesItemOf(const format::NativeArchive& native, const ntdlib::sar::Place& place, uint32_t sequence)
{
    if (place.item && place.sequence)
    {
        return place.piece == sequence;
    }

    const auto* piece = place.item && place.piece < native.pieces->size()
                            ? std::get_if<ntdlib::sar::SequencePiece>(&(*native.pieces)[place.piece])
                            : nullptr;
    return piece != nullptr && piece->sequence == sequence;
}

void RenumberItems(ntdlib::commands::Sequence& after, Edited& edited, uint32_t sequence,
                   const std::vector<uint32_t>& firsts, const std::vector<uint32_t>& places)
{
    for (ntdlib::commands::Command& command : after.items)
    {
        if (ntdlib::commands::HasTarget(command.kind) && !command.target_outside && command.target < firsts.size())
        {
            command.target = firsts[command.target];
        }
    }

    for (ntdlib::commands::Label& label : after.labels)
    {
        label.item = firsts.at(label.item);
    }

    for (std::optional<uint32_t>& item : edited.items)
    {
        if (item)
        {
            item = places.at(*item);
        }
    }

    for (uint32_t& item : edited.added)
    {
        item = places.at(item);
    }

    MoveItemPlaces(edited.file, sequence, [&firsts](uint32_t item) { return firsts.at(item); });
}

} // namespace hokuto::edit
