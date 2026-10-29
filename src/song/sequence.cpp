// SPDX-License-Identifier: MIT

#include "song/sequence.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iterator>
#include <limits>
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
#include "ntdlib/nwsnd/commands.h"
#include "ntdlib/nwsnd/console.h"
#include "ntdlib/rvl/snd/rsar_options.h"
#include "ntdlib/sar/sequence_file.h"
#include "ntdlib/sequence_export/instruments.h"
#include "ntdlib/sequence_export/performer.h"
#include "ntdlib/sequence_export/rsar_instruments.h"
#include "xsflib/binary.h"

#include "song/archive.h"

namespace hokuto::song
{
namespace
{

namespace nwsnd = ntdlib::nwsnd;
namespace sar = ntdlib::sar;
namespace export_ = ntdlib::sequence_export;

using export_::EventKind;
using export_::TrackEvent;

// The settings of `track` for `setting`. `T` is Track or const Track.
template <typename T>
auto& SettingsOf(T& track, Mix setting)
{
    switch (setting)
    {
    case Mix::kVolume:
        return track.volume;
    case Mix::kPan:
        return track.pan;
    case Mix::kSendA:
        return track.sends[0];
    case Mix::kSendB:
        return track.sends[1];
    case Mix::kSendC:
        break;
    }

    return track.sends[2];
}

// Adds a setting from `tick` on. A later setting at the same tick replaces the earlier one. A value that doesn't change
// the setting is left out.
void AddSetting(std::vector<Setting>& settings, uint32_t tick, int value, int fallback)
{
    if (!settings.empty() && settings.back().tick == tick)
    {
        settings.pop_back();
    }

    const int current = settings.empty() ? fallback : settings.back().value;
    if (value != current)
    {
        settings.push_back({tick, value});
    }
}

// A track's notes, settings and instrument, from its events in the performance.
Track ReadTrack(int number, std::vector<TrackEvent> events, const export_::Performance& performance,
                const std::function<std::string(uint32_t slot)>& bank_name)
{
    Track track;
    track.number = number;
    const std::string& name = performance.track_names[static_cast<std::size_t>(number)];
    track.name = name.empty() ? "Track " + std::to_string(number) : name;

    // A note's off finds its on by the note's ID. A note that never ends lasts to the end of the sequence. In a
    // sequence with a loop, the performer stops at its end tick and ends there every note that still sounds. Such a
    // note lasts to the loop's end. The performer repeats a loop shorter than 10 s until the performance lasts 10 s.
    // Each note plays the instrument of the last preset before it. The track's instrument is the one that the first
    // note plays.
    std::stable_sort(events.begin(), events.end(),
                     [](const TrackEvent& a, const TrackEvent& b) { return a.tick < b.tick; });
    std::map<uint32_t, std::size_t> sounding;
    const export_::PresetKey* preset = nullptr;
    for (const TrackEvent& event : events)
    {
        switch (event.kind)
        {
        case EventKind::kNoteOn:
            {
                if (track.notes.empty() && preset != nullptr)
                {
                    track.bank = bank_name(preset->bank_slot);
                    track.program = static_cast<int>(preset->program);
                }

                // The note's ID leads to its command.
                sounding[event.note] = track.notes.size();
                Note& note = track.notes.emplace_back(Note{event.tick, 0, event.key, event.value});
                if (event.note >= 1 && event.note <= performance.note_commands.size())
                {
                    note.command = performance.note_commands[event.note - 1].offset;
                    note.stretch = performance.note_commands[event.note - 1].stretch;
                }
                if (preset != nullptr)
                {
                    note.bank_slot = preset->bank_slot;
                    note.program = preset->program;
                }
                break;
            }

        case EventKind::kNoteOff:
            {
                const auto found = sounding.find(event.note);
                if (found != sounding.end())
                {
                    Note& note = track.notes[found->second];
                    const bool held = performance.loop && event.tick >= performance.end_tick;
                    const uint32_t end = held ? std::min(performance.loop->end, event.tick) : event.tick;
                    note.length = end - std::min(note.start, end);
                    sounding.erase(found);
                }
                break;
            }

        case EventKind::kPreset:
            preset = event.preset < performance.presets.size() ? &performance.presets[event.preset] : nullptr;
            break;

        default:
            break;
        }
    }

    for (const auto& [id, index] : sounding)
    {
        Note& note = track.notes[index];
        note.length = std::max(performance.end_tick, note.start) - note.start;
    }

    // The track's volume, pan and sends at each change that its commands and ramps make, and the stretches of its
    // commands.
    for (const export_::MixChange& change : performance.mix[static_cast<std::size_t>(number)])
    {
        AddSetting(SettingsOf(track, change.setting), change.tick, change.value, MixDefault(change.setting));
    }
    track.stretches = performance.stretches[static_cast<std::size_t>(number)];

    return track;
}

// The calls that a track made from outside any call. For each of the track's stretches, the stretch that entered the
// call that it ran in, or nothing outside calls. `returns` takes the tick at which each call returned.
std::vector<std::optional<std::size_t>> OutermostCalls(const Track& track, std::map<std::size_t, uint32_t>& returns)
{
    std::vector<std::optional<std::size_t>> calls;
    std::optional<std::size_t> current;
    std::size_t depth = 0;
    for (std::size_t i = 0; i < track.stretches.size(); i++)
    {
        const Stretch& stretch = track.stretches[i];
        if (stretch.entry == export_::Entry::kCall)
        {
            current = depth == 0 ? std::optional<std::size_t>(i) : current;
            depth++;
        }
        else if (stretch.entry == export_::Entry::kReturn && depth > 0)
        {
            depth--;
            if (depth == 0)
            {
                returns.emplace(*current, stretch.tick);
                current.reset();
            }
        }
        calls.push_back(current);
    }

    return calls;
}

// Calls `visit` with each item of `sequence` that can run after item `item`: the next item, unless the command always
// jumps, returns or ends, and the target of a jump, a call or a track start. A call's next item stands for the call's
// return. An item can be past the end of the sequence.
template <typename Visit>
void VisitNext(const ntdlib::commands::Sequence& sequence, uint32_t item, Visit&& visit)
{
    using ntdlib::commands::Kind;

    const ntdlib::commands::Command& command = sequence.items[item];
    const bool stops = command.kind == Kind::kJump || command.kind == Kind::kReturn || command.kind == Kind::kEnd;
    if (ntdlib::commands::HasTarget(command.kind) && !command.target_outside)
    {
        visit(command.target);
    }
    if (!stops || command.conditional)
    {
        visit(item + 1);
    }
}

// The items of `sequence` that the player can reach from the items `starts`.
std::vector<uint32_t> Reached(const ntdlib::commands::Sequence& sequence, const std::vector<uint32_t>& starts)
{
    std::vector<bool> seen(sequence.items.size());
    std::vector<uint32_t> pending = starts;
    std::vector<uint32_t> reached;
    const auto add = [&pending](uint32_t next)
    {
        pending.push_back(next);
    };
    while (!pending.empty())
    {
        const uint32_t item = pending.back();
        pending.pop_back();
        if (item >= sequence.items.size() || seen[item])
        {
            continue;
        }

        seen[item] = true;
        reached.push_back(item);
        VisitNext(sequence, item, add);
    }

    return reached;
}

// The commands of a sequence file, the offset in the bytecode of each item and of the end, and the item at each of the
// sounds' starts.
struct FileCommands
{
    ntdlib::commands::Sequence sequence;
    std::vector<uint32_t> offsets;
    std::vector<uint32_t> entries;
};

// The commands of the bytecode `data`, decoded from its `labels` and from `starts`, the starts of the sounds that play
// it. The decoder names each target by its item, and the encoder gives each item's offset. Returns nothing for bytecode
// that the codec can't give back.
std::optional<FileCommands> DecodeCommands(std::span<const uint8_t> data, const std::vector<nwsnd::LabelOffset>& labels,
                                           const std::vector<uint32_t>& starts, const nwsnd::Grammar& grammar)
{
    try
    {
        nwsnd::Decoded decoded = nwsnd::DecodeSequence(data, labels, grammar, starts);
        nwsnd::Encoded encoded = nwsnd::EncodeSequence(decoded.sequence, grammar);
        if (std::ranges::equal(encoded.bytes, data))
        {
            return FileCommands{std::move(decoded.sequence), std::move(encoded.offsets), std::move(decoded.entries)};
        }
    }
    catch (const std::invalid_argument&)
    {
    }
    catch (const std::out_of_range&)
    {
    }

    return std::nullopt;
}

// The commands of `file`, decoded from its labels and from `starts` (DecodeCommands).
std::optional<FileCommands> DecodeFile(const sar::SequenceFile& file, const std::vector<uint32_t>& starts,
                                       const nwsnd::Grammar& grammar)
{
    std::vector<nwsnd::LabelOffset> labels;
    for (const sar::Sequence::Label& label : file.labels)
    {
        labels.push_back({label.name, label.offset});
    }

    return DecodeCommands(file.data, labels, starts, grammar);
}

// The number of call commands in `file` that go to each offset in its bytecode, among those that the sounds' starts
// reach. Calls that no sound reaches, such as calls after a jump that always jumps, don't count.
std::map<uint32_t, int> CountCalls(const FileCommands& file)
{
    std::map<uint32_t, int> calls;
    for (const uint32_t item : Reached(file.sequence, file.entries))
    {
        const ntdlib::commands::Command& command = file.sequence.items[item];
        if (command.kind == ntdlib::commands::Kind::kCall && !command.target_outside &&
            command.target < file.offsets.size())
        {
            calls[file.offsets[command.target]]++;
        }
    }

    return calls;
}

// A comparison of a variable with a fixed value: a command of `kind` with `value`.
struct Comparison
{
    friend auto operator<=>(const Comparison&, const Comparison&) = default;

    ntdlib::commands::Kind kind = ntdlib::commands::Kind::kCompareEqual;
    int value = 0;
};

// The outcome of a comparison of `kind` of a variable whose value is `current` with `value`, or nothing for a command
// of another kind. The variable stands on the left. nn::atk's tracks compare the same way.
std::optional<bool> Compare(ntdlib::commands::Kind kind, int current, int value)
{
    using ntdlib::commands::Kind;

    switch (kind)
    {
    case Kind::kCompareEqual:
        return current == value;
    case Kind::kCompareGreaterOrEqual:
        return current >= value;
    case Kind::kCompareGreater:
        return current > value;
    case Kind::kCompareLessOrEqual:
        return current <= value;
    case Kind::kCompareLess:
        return current < value;
    case Kind::kCompareNotEqual:
        return current != value;
    default:
        return std::nullopt;
    }
}

// Whether a command of `kind` sets a variable.
bool SetsVariable(ntdlib::commands::Kind kind)
{
    using ntdlib::commands::Kind;

    switch (kind)
    {
    case Kind::kSetVariable:
    case Kind::kAddVariable:
    case Kind::kSubtractVariable:
    case Kind::kMultiplyVariable:
    case Kind::kDivideVariable:
    case Kind::kShiftVariable:
    case Kind::kRandomVariable:
    case Kind::kAndVariable:
    case Kind::kOrVariable:
    case Kind::kXorVariable:
    case Kind::kNotVariable:
    case Kind::kModuloVariable:
        return true;
    default:
        return false;
    }
}

// The choices of a variable with `comparisons` (see Variable) that starts at `start`. A value next to each compared
// value, or the value itself, gives every outcome of the comparisons that any value gives.
std::vector<int> ChoicesOf(const std::set<Comparison>& comparisons, int start)
{
    if (comparisons.empty())
    {
        return {};
    }

    // The values to try, the starting value first, then from the nearest to 0.
    constexpr int kLowest = std::numeric_limits<int16_t>::min();
    constexpr int kHighest = std::numeric_limits<int16_t>::max();
    std::set<int> candidates = {start};
    for (const Comparison& comparison : comparisons)
    {
        for (const int offset : {-1, 0, 1})
        {
            candidates.insert(std::clamp(comparison.value + offset, kLowest, kHighest));
        }
    }

    std::vector<int> tried(candidates.begin(), candidates.end());
    const auto preference = [start](int value)
    {
        return std::tuple(value != start, std::abs(value), value < 0);
    };
    std::sort(tried.begin(), tried.end(), [&preference](int a, int b) { return preference(a) < preference(b); });

    // The first value with each set of outcomes stays.
    std::set<std::vector<bool>> outcomes_seen;
    std::vector<int> choices;
    for (const int value : tried)
    {
        std::vector<bool> outcomes;
        for (const Comparison& comparison : comparisons)
        {
            outcomes.push_back(Compare(comparison.kind, value, comparison.value).value_or(false));
        }
        if (outcomes_seen.insert(std::move(outcomes)).second)
        {
            choices.push_back(value);
        }
    }
    std::sort(choices.begin(), choices.end());

    return choices;
}

// The bit of variable `number` in a set of the variables that a game can set, or 0 for another number.
uint32_t VariableBit(int64_t number)
{
    return number >= 0 && number < kGameVariables ? 1u << number : 0u;
}

// Whether `command` goes to an item of `sequence`: a jump, a call or a track start whose target lies inside it.
bool TargetsItem(const ntdlib::commands::Sequence& sequence, const ntdlib::commands::Command& command)
{
    return ntdlib::commands::HasTarget(command.kind) && !command.target_outside &&
           command.target < sequence.items.size();
}

// Whether a comparison of `command` tests its variable against a fixed value.
bool ComparesWithValue(const ntdlib::commands::Command& command)
{
    return Compare(command.kind, 0, 0).has_value() && command.value.source == ntdlib::commands::Source::kValue;
}

// The variables that a command reads and those that it sets for certain, a bit for each number. `reads` holds every
// variable that the command reads. `exact` holds those whose exact value counts. A comparison with a fixed value
// requires only its outcome. Every other read requires the exact value: an argument that takes a variable's value, a
// comparison with a variable or a random value, and a command that works out a variable's new value from its value.
// `sets` holds the variables that the command sets without reading them. nn::atk's set, random and not commands do
// that. A command behind an if prefix sets nothing for certain.
struct VariableUse
{
    uint32_t reads = 0;
    uint32_t exact = 0;
    uint32_t sets = 0;
};

VariableUse UseOf(const ntdlib::commands::Command& command)
{
    using ntdlib::commands::Kind;
    using ntdlib::commands::Source;

    VariableUse use;
    if (command.value.source == Source::kVariable)
    {
        use.exact |= VariableBit(command.value.value);
    }
    if (command.time && command.time->source == Source::kVariable)
    {
        use.exact |= VariableBit(command.time->value);
    }

    // The variable that the command addresses.
    const bool overwrites = command.kind == Kind::kSetVariable || command.kind == Kind::kRandomVariable ||
                            command.kind == Kind::kNotVariable;
    if (ComparesWithValue(command))
    {
        use.reads |= VariableBit(command.index);
    }
    else if (Compare(command.kind, 0, 0).has_value() || (SetsVariable(command.kind) && !overwrites))
    {
        use.exact |= VariableBit(command.index);
    }
    if (overwrites && !command.conditional)
    {
        use.sets |= VariableBit(command.index);
    }
    use.reads |= use.exact;

    return use;
}

// What a track's paths set for certain, a bit for each variable, by item. `on_return` holds, for the first item of a
// subroutine, what a call of it sets before it returns: every bit if it never returns. `on_open` holds, for an
// open-track command, what the track that runs the command sets after it, before the track stops for the tick. nn::atk
// runs the track that the command opens only after that stop.
struct Summaries
{
    std::vector<uint32_t> on_return;
    std::vector<uint32_t> on_open;
};

// For each item of `sequence`, the variables that every path from item `from` sets before the item runs, a bit for each
// number, or nothing for an item that `from` doesn't reach. The path past a call takes what the call sets
// (Summaries::on_return). With `tracks`, the paths also go into the subroutines that calls run and the tracks that
// commands open. A track starts with what the opening command's track set before the command and before its next stop
// (Summaries::on_open). Without `tracks`, the paths keep to the commands that one track runs outside its calls.
std::vector<std::optional<uint32_t>> SetBefore(const ntdlib::commands::Sequence& sequence,
                                               const std::vector<VariableUse>& uses, const Summaries& summaries,
                                               uint32_t from, bool tracks)
{
    using ntdlib::commands::Kind;

    std::vector<std::optional<uint32_t>> set(sequence.items.size());
    std::vector<uint32_t> pending;
    const auto follow = [&set, &pending](uint32_t next, uint32_t sets)
    {
        if (next < set.size() && (!set[next] || (*set[next] & sets) != *set[next]))
        {
            set[next] = set[next].value_or(~0u) & sets;
            pending.push_back(next);
        }
    };
    follow(from, 0);

    // A path that sets fewer variables lowers what the items after it have.
    while (!pending.empty())
    {
        const uint32_t item = pending.back();
        pending.pop_back();
        const ntdlib::commands::Command& command = sequence.items[item];
        const uint32_t after = *set[item] | uses[item].sets;
        if (command.kind == Kind::kCall && TargetsItem(sequence, command))
        {
            if (tracks)
            {
                follow(command.target, after);
            }
            follow(item + 1, command.conditional ? after : after | summaries.on_return[command.target]);
        }
        else if (command.kind == Kind::kOpenTrack && TargetsItem(sequence, command))
        {
            if (tracks)
            {
                follow(command.target, after | summaries.on_open[item]);
            }
            follow(item + 1, after);
        }
        else
        {
            VisitNext(sequence, item, [&follow, after](uint32_t next) { follow(next, after); });
        }
    }

    return set;
}

// Whether a track can stop for the tick at `command`: at a wait, at a note while the track's notes wait for their
// length, or at the end.
bool MayWait(const ntdlib::commands::Command& command)
{
    using ntdlib::commands::Kind;

    switch (command.kind)
    {
    case Kind::kWait:
        return command.value.source != ntdlib::commands::Source::kValue || command.value.value != 0;
    case Kind::kNote:
    case Kind::kEnd:
        return true;
    default:
        return false;
    }
}

// Whether the item at `item` jumps back to an earlier item with no command between them that sets a variable or calls a
// subroutine. Nothing in the track then changes what such a loop tests. The loop waits for another track or the game,
// and nn::atk stops the track for the tick after 10,000 commands, anywhere in the loop.
bool LoopsIdle(const ntdlib::commands::Sequence& sequence, uint32_t item)
{
    using ntdlib::commands::Kind;

    const ntdlib::commands::Command& command = sequence.items[item];
    if (command.kind != Kind::kJump || !TargetsItem(sequence, command) || command.target > item)
    {
        return false;
    }

    for (uint32_t other = command.target; other < item; other++)
    {
        const Kind kind = sequence.items[other].kind;
        if (SetsVariable(kind) || kind == Kind::kCall)
        {
            return false;
        }
    }

    return true;
}

// What a call of the subroutine at item `entry` sets before it returns (see Summaries).
uint32_t SetOnReturn(const ntdlib::commands::Sequence& sequence, const std::vector<VariableUse>& uses,
                     const Summaries& summaries, uint32_t entry)
{
    const std::vector<std::optional<uint32_t>> set = SetBefore(sequence, uses, summaries, entry, false);
    uint32_t sets = ~0u;
    for (std::size_t item = 0; item < set.size(); item++)
    {
        if (set[item] && sequence.items[item].kind == ntdlib::commands::Kind::kReturn)
        {
            sets &= *set[item];
        }
    }

    return sets;
}

// For the first item of each subroutine in `entries`, whether a call of the subroutine can stop for the tick before it
// returns.
std::vector<bool> WaitInCalls(const ntdlib::commands::Sequence& sequence, const std::vector<VariableUse>& uses,
                              const Summaries& summaries, const std::set<uint32_t>& entries)
{
    std::map<uint32_t, std::vector<uint32_t>> runs;
    for (const uint32_t entry : entries)
    {
        const std::vector<std::optional<uint32_t>> set = SetBefore(sequence, uses, summaries, entry, false);
        for (uint32_t item = 0; item < set.size(); item++)
        {
            if (set[item])
            {
                runs[entry].push_back(item);
            }
        }
    }

    // A call of a subroutine that can stop can stop too. The subroutines that can stop grow round by round.
    std::vector<bool> waits(sequence.items.size());
    for (bool changed = true; changed;)
    {
        changed = false;
        for (const auto& [entry, items] : runs)
        {
            const auto stops = [&sequence, &waits](uint32_t item)
            {
                const ntdlib::commands::Command& command = sequence.items[item];
                return MayWait(command) || LoopsIdle(sequence, item) ||
                       (command.kind == ntdlib::commands::Kind::kCall && TargetsItem(sequence, command) &&
                        waits[command.target]);
            };
            if (!waits[entry] && std::any_of(items.begin(), items.end(), stops))
            {
                waits[entry] = true;
                changed = true;
            }
        }
    }

    return waits;
}

// What the track that runs the open-track command at `item` sets after the command, before it stops for the tick (see
// Summaries). The track can stop at a command that can wait, in a call of a subroutine that can stop (`waits`), at a
// return to an unknown caller, or anywhere in an idle loop. A track that never stops sets nothing for certain.
uint32_t SetOnOpen(const ntdlib::commands::Sequence& sequence, const std::vector<VariableUse>& uses,
                   const Summaries& summaries, const std::vector<bool>& waits, uint32_t item)
{
    using ntdlib::commands::Kind;

    const std::vector<std::optional<uint32_t>> set = SetBefore(sequence, uses, summaries, item + 1, false);
    std::optional<uint32_t> sets;
    for (uint32_t other = 0; other < set.size(); other++)
    {
        if (!set[other])
        {
            continue;
        }

        const ntdlib::commands::Command& command = sequence.items[other];
        if (MayWait(command) || command.kind == Kind::kReturn ||
            (command.kind == Kind::kCall && TargetsItem(sequence, command) && waits[command.target]))
        {
            sets = sets.value_or(~0u) & *set[other];
        }
        if (LoopsIdle(sequence, other))
        {
            sets = sets.value_or(~0u) & set[command.target].value_or(0);
        }
    }

    return sets.value_or(0);
}

// The summaries of the subroutines and the open-track commands among the items `reached`.
Summaries Summarize(const ntdlib::commands::Sequence& sequence, const std::vector<VariableUse>& uses,
                    const std::vector<uint32_t>& reached)
{
    using ntdlib::commands::Kind;

    std::set<uint32_t> entries;
    for (const uint32_t item : reached)
    {
        const ntdlib::commands::Command& command = sequence.items[item];
        if (command.kind == Kind::kCall && TargetsItem(sequence, command))
        {
            entries.insert(command.target);
        }
    }

    // What each call sets starts at every bit and shrinks round by round, until a round changes nothing.
    Summaries summaries{std::vector<uint32_t>(sequence.items.size(), ~0u),
                        std::vector<uint32_t>(sequence.items.size(), 0)};
    for (bool changed = true; changed;)
    {
        changed = false;
        for (const uint32_t entry : entries)
        {
            const uint32_t sets = SetOnReturn(sequence, uses, summaries, entry);
            changed = changed || sets != summaries.on_return[entry];
            summaries.on_return[entry] = sets;
        }
    }

    const std::vector<bool> waits = WaitInCalls(sequence, uses, summaries, entries);
    for (const uint32_t item : reached)
    {
        const ntdlib::commands::Command& command = sequence.items[item];
        if (command.kind == Kind::kOpenTrack && TargetsItem(sequence, command))
        {
            summaries.on_open[item] = SetOnOpen(sequence, uses, summaries, waits, item);
        }
    }

    return summaries;
}

// The milliseconds that a tick of `sequence` lasts at `bpm` quarter notes a minute. A tempo of 0 stops the ticks.
double MsPerTick(const Sequence& sequence, double bpm)
{
    return bpm > 0.0 ? 60'000.0 / (bpm * sequence.timebase) : 0.0;
}

// The variables that a set of commands reads: the comparisons with fixed values that reach each one, a bit for each
// variable whose exact value counts, and a bit for each variable that the commands change.
class Reads
{
public:
    // Adds what `command` reads of the variables in `bits`, with `use`, its use of them.
    void Add(const ntdlib::commands::Command& command, const VariableUse& use, uint32_t bits)
    {
        for (int number = 0; number < kGameVariables; number++)
        {
            if (((use.reads & bits) >> number & 1u) != 0)
            {
                comparisons_.try_emplace(number);
            }
        }
        exact_ |= use.exact & bits;
        if (ComparesWithValue(command) && (bits & VariableBit(command.index)) != 0)
        {
            comparisons_[command.index].insert(
                {command.kind, static_cast<int>(command.value.value / ntdlib::commands::kOne)});
        }
        if (SetsVariable(command.kind))
        {
            changed_ |= VariableBit(command.index);
        }
    }

    // The variables in the order of their numbers. The outcomes of the comparisons tell values apart only where no
    // command takes the exact value. The global variables start at `global_start`.
    std::vector<Variable> Variables(int global_start) const
    {
        std::vector<Variable> variables;
        for (const auto& [number, compared] : comparisons_)
        {
            const bool counts = (exact_ >> number & 1u) != 0;
            const int start = number >= kFirstGlobal ? global_start : -1;
            variables.push_back(
                {number, counts ? std::vector<int>() : ChoicesOf(compared, start), (changed_ >> number & 1u) != 0});
        }

        return variables;
    }

private:
    std::map<int, std::set<Comparison>> comparisons_;
    uint32_t exact_ = 0;
    uint32_t changed_ = 0;
};

// The variables that a game can set and that the commands reached from item `start` of `sequence` read (VariablesOf).
// `starting` holds those that the commands read before every path to them has set them. Only such a variable's starting
// value can change what the sound plays. A path follows the tracks that the sound's commands open, and passes the calls
// that it meets with what they set. `all` holds every variable that the commands read. A change while the sound plays
// can reach each of them. The global variables start at `global_start`.
SoundVariables ReadVariables(const ntdlib::commands::Sequence& sequence, uint32_t start, int global_start)
{
    std::vector<VariableUse> uses;
    uses.reserve(sequence.items.size());
    for (const ntdlib::commands::Command& command : sequence.items)
    {
        uses.push_back(UseOf(command));
    }
    const std::vector<uint32_t> reached = Reached(sequence, {start});
    const std::vector<std::optional<uint32_t>> set_before =
        SetBefore(sequence, uses, Summarize(sequence, uses, reached), start, true);

    // What a starting value reaches, and what the commands read at any time.
    Reads starting;
    Reads all;
    for (const uint32_t item : reached)
    {
        const ntdlib::commands::Command& command = sequence.items[item];
        starting.Add(command, uses[item], ~set_before[item].value_or(~0u));
        all.Add(command, uses[item], ~0u);
    }

    return {starting.Variables(global_start), all.Variables(global_start)};
}

// The changes of `changes` that a game can make, in the order of their times. Changes at the same time keep their
// order.
Changes GameChanges(const Changes& changes)
{
    Changes kept;
    for (const VariableChange& change : changes)
    {
        if (change.variable >= 0 && change.variable < kGameVariables)
        {
            kept.push_back(change);
        }
    }
    std::ranges::stable_sort(kept, {}, &VariableChange::time);

    return kept;
}

// ntdlib's reason why a sound can't be read, in the program's words. An archive that is cut short is the usual cause.
// Another reason takes a capital letter and a full stop.
std::string ReadReason(std::string reason)
{
    if (reason.find("truncated archive") != std::string::npos)
    {
        return "The archive is cut short. The sound's sequence runs past its end.";
    }
    if (reason.find("extends past the archive") != std::string::npos)
    {
        return "The sound's sequence runs past the archive's end.";
    }

    if (!reason.empty() && reason.front() >= 'a' && reason.front() <= 'z')
    {
        reason.front() = static_cast<char>(reason.front() - 'a' + 'A');
    }
    if (!reason.empty() && !reason.ends_with('.'))
    {
        reason += '.';
    }

    return reason;
}

} // namespace

SoundVariables VariablesOf(const ntdlib::commands::Sequence& sequence, uint32_t start, int global_start)
{
    return ReadVariables(sequence, start, global_start);
}

int StartingValue(const Sequence& sequence, int number)
{
    return sequence.wii_build && number >= kFirstGlobal && number < kGameVariables ? 0 : -1;
}

std::optional<std::size_t> OutermostCall(const Track& track, std::size_t stretch)
{
    std::map<std::size_t, uint32_t> returns;
    const std::vector<std::optional<std::size_t>> calls = OutermostCalls(track, returns);
    return stretch < calls.size() ? calls[stretch] : std::nullopt;
}

std::vector<Part> SplitParts(const Sequence& sequence, const Track& track, int grid, int gap)
{
    // The loop's start and end divide the sequence into sections. A part stays inside its section.
    std::vector<uint32_t> bounds;
    if (sequence.loop)
    {
        for (const uint32_t bound : {sequence.loop->start, sequence.loop->end})
        {
            if (bound > 0 && (bounds.empty() || bound > bounds.back()))
            {
                bounds.push_back(bound);
            }
        }
    }

    const auto section_of = [&bounds](uint32_t tick)
    {
        return static_cast<std::size_t>(std::upper_bound(bounds.begin(), bounds.end(), tick) - bounds.begin());
    };

    // The call that each note ran in, and where each call returned.
    std::map<std::size_t, uint32_t> returns;
    const std::vector<std::optional<std::size_t>> calls = OutermostCalls(track, returns);
    const auto call_of = [&calls](const Note& note)
    {
        return note.stretch < calls.size() ? calls[note.stretch] : std::nullopt;
    };

    // Each note joins the part before it, unless it's in another section or another call, or the gap lies between them.
    // The notes of one call join its part whatever lies between them.
    const auto ticks = [&sequence](int quarters)
    {
        return static_cast<uint32_t>(std::max(sequence.timebase * std::max(quarters, 1), 1));
    };
    const uint32_t line = ticks(grid);
    const uint32_t gap_ticks = ticks(gap);
    std::vector<Part> parts;
    std::optional<std::size_t> part_call;
    std::size_t section = 0;
    uint32_t notes_end = 0;

    // Where the last part ends without a part after it: at its call's return, or at the line after its notes, inside
    // its section.
    const auto end_of_part = [&]
    {
        const auto returned = part_call ? returns.find(*part_call) : returns.end();
        const uint32_t end = returned != returns.end() ? returned->second : (notes_end + line - 1) / line * line;
        const uint32_t section_end = section < bounds.size() ? bounds[section] : std::numeric_limits<uint32_t>::max();
        return std::max(std::min(end, section_end), parts.back().start + 1);
    };
    for (std::size_t i = 0; i < track.notes.size(); i++)
    {
        const Note& note = track.notes[i];
        const std::optional<std::size_t> call = call_of(note);
        const std::size_t note_section = section_of(note.start);
        if (!parts.empty() && note_section == section && call == part_call &&
            (call || note.start < notes_end + gap_ticks))
        {
            parts.back().count++;
            notes_end = std::max(notes_end, note.start + note.length);
            continue;
        }

        // A call's part starts at the call, and another part at the line before its first note. A part after a call's
        // part starts once the call has returned, and a part before a call's part ends where the call starts.
        const uint32_t section_start = note_section > 0 ? bounds[note_section - 1] : 0;
        uint32_t start = std::max(call ? track.stretches[*call].tick : note.start / line * line, section_start);
        if (!parts.empty())
        {
            const uint32_t end = end_of_part();
            start = part_call ? std::max(start, std::min(end, note.start)) : start;
            parts.back().end = std::max(std::min(end, start), parts.back().start + 1);
        }
        section = note_section;
        part_call = call;
        const std::optional<uint32_t> subroutine =
            call ? std::optional<uint32_t>(track.stretches[*call].first) : std::nullopt;
        parts.push_back({start, 0, i, 1, subroutine});
        notes_end = note.start + note.length;
    }

    if (!parts.empty())
    {
        parts.back().end = end_of_part();
    }

    return parts;
}

int SettingAt(const std::vector<Setting>& settings, uint32_t tick, int fallback)
{
    const auto after = std::upper_bound(settings.begin(), settings.end(), tick,
                                        [](uint32_t value, const Setting& setting) { return value < setting.tick; });
    return after == settings.begin() ? fallback : std::prev(after)->value;
}

const std::vector<Setting>& MixSettings(const Track& track, Mix setting)
{
    return SettingsOf(track, setting);
}

int MixDefault(Mix setting)
{
    return setting == Mix::kVolume ? 127 : 0;
}

int MixAt(const Track& track, Mix setting, uint32_t tick)
{
    return SettingAt(MixSettings(track, setting), tick, MixDefault(setting));
}

Sequence FirstPass(Sequence sequence, Loop loop)
{
    const uint32_t end = loop.end;
    sequence.loop = loop;
    sequence.length = end;
    sequence.notes_end = 0;
    std::erase_if(sequence.tempo, [end](const Tempo& tempo) { return tempo.tick >= end && tempo.tick > 0; });
    const auto later = [end](const Setting& setting)
    {
        return setting.tick >= end;
    };
    for (Track& track : sequence.tracks)
    {
        std::erase_if(track.notes, [end](const Note& note) { return note.start >= end; });
        for (Note& note : track.notes)
        {
            note.length = std::min(note.length, end - note.start);
            sequence.notes_end = std::max(sequence.notes_end, note.start + note.length);
        }
        std::erase_if(track.volume, later);
        std::erase_if(track.pan, later);
        for (std::vector<Setting>& sends : track.sends)
        {
            std::erase_if(sends, later);
        }
    }

    return sequence;
}

bool SameTiming(const Sequence& a, const Sequence& b)
{
    return a.timebase == b.timebase && a.tempo == b.tempo && a.loop == b.loop;
}

double TempoAt(const Sequence& sequence, uint32_t tick)
{
    double bpm = 120.0;
    for (const Tempo& tempo : sequence.tempo)
    {
        if (tempo.tick > tick)
        {
            break;
        }

        bpm = tempo.bpm;
    }

    return bpm;
}

double MsOfTick(const Sequence& sequence, double tick)
{
    // Each stretch of ticks at one tempo adds its time.
    double ms = 0.0;
    double from = 0.0;
    double bpm = 120.0;
    for (const Tempo& tempo : sequence.tempo)
    {
        if (tempo.tick >= tick)
        {
            break;
        }

        ms += (tempo.tick - from) * MsPerTick(sequence, bpm);
        from = tempo.tick;
        bpm = tempo.bpm;
    }

    return ms + (tick - from) * MsPerTick(sequence, bpm);
}

double TickOfMs(const Sequence& sequence, double ms)
{
    // The stretch of ticks at one tempo that holds `ms`.
    double at = 0.0;
    double from = 0.0;
    double bpm = 120.0;
    for (const Tempo& tempo : sequence.tempo)
    {
        const double next = at + (tempo.tick - from) * MsPerTick(sequence, bpm);
        if (next > ms)
        {
            break;
        }

        at = next;
        from = tempo.tick;
        bpm = tempo.bpm;
    }

    const double per_tick = MsPerTick(sequence, bpm);
    return per_tick > 0.0 ? from + (ms - at) / per_tick : from;
}

uint32_t PositionOfTicks(const Sequence& sequence, uint64_t ticks)
{
    const std::optional<Loop>& loop = sequence.loop;
    if (!loop || ticks < loop->end || loop->end <= loop->start)
    {
        return static_cast<uint32_t>(std::min<uint64_t>(ticks, std::numeric_limits<uint32_t>::max()));
    }

    return loop->start + static_cast<uint32_t>((ticks - loop->end) % (loop->end - loop->start));
}

uint64_t ChangeFrame(uint32_t time, FrameMs frame_ms)
{
    // The time in frames, rounded up. A time and a denominator below 2^32 and 2^25 can't overflow.
    return (static_cast<uint64_t>(time) * frame_ms.denominator + frame_ms.numerator - 1) / frame_ms.numerator;
}

uint64_t FramesIn(uint64_t ms, FrameMs frame_ms)
{
    return ms * frame_ms.denominator / frame_ms.numerator;
}

export_::PerformOptions PerformOptionsOf(const StartValues& values, const Changes& changes, sar::Flavor flavor)
{
    // ntdlib's models of nn::atk and nw::snd start the sound in their frame 0 and play the first tick in frame 1. The
    // 3DS's archive mode plays the first tick in frame 0.
    return PerformOptionsOf(values, changes, FrameMsOf(flavor), flavor == sar::Flavor::kCtr ? 0 : 1);
}

export_::PerformOptions PerformOptionsOf(const StartValues& values, const Changes& changes, FrameMs frame_ms,
                                         uint64_t first_tick)
{
    export_::PerformOptions options;
    for (const auto& [number, value] : values)
    {
        if (number >= 0 && number < kGameVariables)
        {
            options.variables[static_cast<uint8_t>(number)] = value;
        }
    }

    // The performer counts its frames from the first tick's.
    for (const VariableChange& change : GameChanges(changes))
    {
        const uint64_t frame = ChangeFrame(change.time, frame_ms);
        options.changes.push_back(
            {frame > first_tick ? frame - first_tick : 0, static_cast<uint8_t>(change.variable), change.value});
    }

    return options;
}

Sequence ReadSequence(const sar::SoundArchive& archive, uint32_t sound, const StartValues& values,
                      const Changes& changes)
{
    if (sound >= archive.Sounds().size() || archive.Sounds()[sound].type != sar::SoundType::kSequence)
    {
        throw std::runtime_error("The archive has no sequence sound " + std::to_string(sound) + ".");
    }

    // The performer sets the variables before the sequence starts, and makes the changes at the starts of their frames.
    // A game does both through the sound's handle.
    const sar::SoundInfo& info = archive.Sounds()[sound];
    const sar::Flavor flavor = archive.ReadOptions().flavor;
    const export_::Settings settings = SettingsOf(archive);
    const export_::PerformOptions options = PerformOptionsOf(values, changes, flavor);
    StartValues kept(options.variables.begin(), options.variables.end());
    Changes kept_changes = GameChanges(changes);
    try
    {
        export_::BankSet banks(archive, info, settings);
        const export_::Performance performance = export_::Perform(archive, sound, banks, options, settings);

        Sequence sequence;
        sequence.name = info.name;
        sequence.sound = sound;
        sequence.start_values = std::move(kept);
        sequence.changes = std::move(kept_changes);
        sequence.timebase = static_cast<int>(std::max<uint32_t>(performance.timebase, 1));
        for (const auto& tempo : performance.tempo)
        {
            sequence.tempo.push_back({tempo.tick, tempo.bpm});
        }
        if (performance.loop)
        {
            sequence.loop = Loop{performance.loop->start, performance.loop->end};
        }
        sequence.holds = performance.holds;

        // The tracks that ran commands, with notes or without. A sequence with a loop ends with the loop. The performer
        // repeats a loop shorter than 10 s until the performance lasts 10 s, but playback goes back round the loop at
        // its end. Without a loop, the performer's end includes the last notes' release. The sequence lasts at least to
        // the end of its last note.
        sequence.length = sequence.loop ? sequence.loop->end : performance.end_tick;
        for (std::size_t i = 0; i < performance.tracks.size(); i++)
        {
            Track track = ReadTrack(static_cast<int>(i), performance.tracks[i], performance,
                                    [&banks](uint32_t slot) { return banks.BankName(slot); });
            if (track.notes.empty() && track.stretches.empty())
            {
                continue;
            }

            if (sequence.loop)
            {
                const uint32_t end = sequence.loop->end;
                std::erase_if(track.notes, [end](const Note& note) { return note.start >= end; });
            }
            for (const Note& note : track.notes)
            {
                sequence.notes_end = std::max(sequence.notes_end, note.start + note.length);
            }
            sequence.length = std::max(sequence.length, sequence.notes_end);
            sequence.tracks.push_back(std::move(track));
        }

        // The labels name the subroutines that the arrangement shows. The calls show which of them other calls play
        // too, from this sound or from the other sounds of the file. The sound's commands show which variables it
        // reads. A file that can't be read plays all the same.
        std::set<uint32_t> starts;
        for (const sar::SoundInfo& other : archive.Sounds())
        {
            if (other.type == sar::SoundType::kSequence && other.file_id == info.file_id)
            {
                starts.insert(other.start_offset);
            }
        }
        const std::vector<uint32_t> entries(starts.begin(), starts.end());
        try
        {
            const sar::SequenceFile file = sar::SequenceFile::Read(archive.FileData(info.file_id));
            for (const sar::Sequence::Label& label : file.labels)
            {
                sequence.labels.emplace(label.offset, label.name);
            }
            if (const std::optional<FileCommands> commands = DecodeFile(file, entries, GrammarOf(flavor)))
            {
                const auto entry =
                    std::lower_bound(entries.begin(), entries.end(), info.start_offset) - entries.begin();
                SoundVariables variables =
                    ReadVariables(commands->sequence, commands->entries[static_cast<std::size_t>(entry)], -1);
                sequence.calls = CountCalls(*commands);
                sequence.variables = std::move(variables.starting);
                sequence.read_variables = std::move(variables.all);
            }
        }
        catch (const xsflib::FormatError&)
        {
            sequence.labels.clear();
        }

        return sequence;
    }
    catch (const xsflib::FormatError& error)
    {
        throw std::runtime_error(info.name + " can't be read. " + ReadReason(error.what()));
    }
}

Sequence ReadSequence(const ntdlib::rsar::SoundArchive& archive, uint32_t sound, const StartValues& values,
                      const Changes& changes, const std::optional<ntdlib::SndBuild>& build)
{
    if (sound >= archive.Sounds().size() || archive.Sounds()[sound].type != ntdlib::rsar::SoundType::kSequence)
    {
        throw std::runtime_error("The archive has no sequence sound " + std::to_string(sound) + ".");
    }

    // The performer sets the variables before the sequence starts, and makes the changes at the starts of their frames.
    // ntdlib's model of the Wii starts the sequence when the sound starts, and plays the first tick in its frame 0. A
    // Wii sequence plays one bank.
    const ntdlib::rsar::SoundInfo& info = archive.Sounds()[sound];
    const export_::Settings settings = SettingsOf(archive, build);
    const export_::PerformOptions options = PerformOptionsOf(values, changes, kWiiFrameMs, 0);
    StartValues kept(options.variables.begin(), options.variables.end());
    Changes kept_changes = GameChanges(changes);
    try
    {
        export_::RsarBankSet banks(archive, info, settings);
        const export_::Performance performance = export_::Perform(archive, sound, banks, options, settings);

        Sequence sequence;
        sequence.name = info.name;
        sequence.sound = sound;
        sequence.start_values = std::move(kept);
        sequence.changes = std::move(kept_changes);
        sequence.wii_build = settings.wii_build;
        sequence.timebase = static_cast<int>(std::max<uint32_t>(performance.timebase, 1));
        for (const auto& tempo : performance.tempo)
        {
            sequence.tempo.push_back({tempo.tick, tempo.bpm});
        }
        if (performance.loop)
        {
            sequence.loop = Loop{performance.loop->start, performance.loop->end};
        }
        sequence.holds = performance.holds;

        // The tracks and the length follow the same rules as the other consoles' (see above).
        sequence.length = sequence.loop ? sequence.loop->end : performance.end_tick;
        for (std::size_t i = 0; i < performance.tracks.size(); i++)
        {
            Track track = ReadTrack(static_cast<int>(i), performance.tracks[i], performance,
                                    [&banks](uint32_t) { return banks.BankName(); });
            if (track.notes.empty() && track.stretches.empty())
            {
                continue;
            }

            if (sequence.loop)
            {
                const uint32_t end = sequence.loop->end;
                std::erase_if(track.notes, [end](const Note& note) { return note.start >= end; });
            }
            for (const Note& note : track.notes)
            {
                sequence.notes_end = std::max(sequence.notes_end, note.start + note.length);
            }
            sequence.length = std::max(sequence.length, sequence.notes_end);
            sequence.tracks.push_back(std::move(track));
        }

        // As on the other consoles, the labels, the calls and the variables come from the sound's commands. The
        // commands decode by the rules of the build that the performer follows.
        std::set<uint32_t> starts;
        for (const ntdlib::rsar::SoundInfo& other : archive.Sounds())
        {
            if (other.type == ntdlib::rsar::SoundType::kSequence && other.file_id == info.file_id)
            {
                starts.insert(other.start_offset);
            }
        }
        const std::vector<uint32_t> entries(starts.begin(), starts.end());
        try
        {
            const std::optional<ntdlib::rsar::FileData> file = archive.File(info.file_id);
            const ntdlib::rsar::Sequence parsed =
                file ? ntdlib::rsar::Sequence::Parse(file->file, ntdlib::rvl::snd::kRsarOptions)
                     : ntdlib::rsar::Sequence{};
            std::vector<nwsnd::LabelOffset> labels;
            for (const ntdlib::rsar::Sequence::Label& label : parsed.labels)
            {
                sequence.labels.emplace(label.offset, label.name);
                labels.push_back({label.name, label.offset});
            }
            const nwsnd::Grammar grammar = nwsnd::GrammarOf<nwsnd::RevolutionPolicy>(settings.wii_build);
            if (const std::optional<FileCommands> commands = DecodeCommands(parsed.data, labels, entries, grammar))
            {
                const auto entry =
                    std::lower_bound(entries.begin(), entries.end(), info.start_offset) - entries.begin();
                SoundVariables variables =
                    ReadVariables(commands->sequence, commands->entries[static_cast<std::size_t>(entry)], 0);
                sequence.calls = CountCalls(*commands);
                sequence.variables = std::move(variables.starting);
                sequence.read_variables = std::move(variables.all);
            }
        }
        catch (const xsflib::FormatError&)
        {
            sequence.labels.clear();
        }

        return sequence;
    }
    catch (const xsflib::FormatError& error)
    {
        throw std::runtime_error(info.name + " can't be read. " + ReadReason(error.what()));
    }
}

} // namespace hokuto::song
