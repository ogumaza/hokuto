// SPDX-License-Identifier: MIT

// The variables of a sequence: which ones its commands set and compare, the choices that their starting values make,
// and the changes that a game makes to them.

#include "song/sequence_parts.h"

namespace hokuto::song
{
namespace
{

using sequence_parts::ReadVariables;
using sequence_parts::VisitNext;

} // namespace

namespace sequence_parts
{

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

} // namespace sequence_parts

namespace
{

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

} // namespace

namespace sequence_parts
{

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
        return "The archive is truncated. The sequence runs past its end.";
    }
    if (reason.find("extends past the archive") != std::string::npos)
    {
        return "The sequence runs past the end of the archive.";
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

// Sets the calls of `sequence` and the variables that it reads from `commands`, the commands of its file. `starts`
// contains the sorted starts of the file's sounds, and the sound starts at `start`, one of them. The first global
// variable is `global_start` (ReadVariables).
void SetCallsAndVariables(Sequence& sequence, const FileCommands& commands, std::span<const uint32_t> starts,
                          uint32_t start, int global_start)
{
    const auto entry = std::lower_bound(starts.begin(), starts.end(), start) - starts.begin();
    SoundVariables variables =
        ReadVariables(commands.sequence, commands.entries[static_cast<std::size_t>(entry)], global_start);
    sequence.calls = CountCalls(commands);
    sequence.variables = std::move(variables.starting);
    sequence.read_variables = std::move(variables.all);
}

void ThrowUnreadable(const std::string& name, const std::exception& error)
{
    throw std::runtime_error(name + " can't be read. " + ReadReason(error.what()));
}

} // namespace sequence_parts

SoundVariables VariablesOf(const ntdlib::commands::Sequence& sequence, uint32_t start, int global_start)
{
    return ReadVariables(sequence, start, global_start);
}

int StartingValue(const Sequence& sequence, int number)
{
    return sequence.wii_build && number >= kFirstGlobal && number < kGameVariables ? 0 : -1;
}

} // namespace hokuto::song
