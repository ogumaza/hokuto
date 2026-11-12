// SPDX-License-Identifier: MIT

#include "edit/ds_conversion.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/commands/units.h"
#include "ntdlib/ntr/snd/commands.h"

#include "edit/bytecode.h"
#include "edit/other_console_parts.h"
#include "song/archive.h"
#include "song/track_flow.h"

namespace hokuto::edit
{
namespace
{

using ntdlib::commands::Argument;
using ntdlib::commands::Command;
using ntdlib::commands::FromUnit;
using ntdlib::commands::Kind;
using ntdlib::commands::Scale;
using ntdlib::commands::Sequence;
using ntdlib::commands::Source;
using ntdlib::commands::ToUnit;
using other_console_parts::ListOf;
using other_console_parts::SkippedWarning;

// The ticks of a quarter note on the DS, and the later consoles' tempo before a command sets one.
constexpr int64_t kDsTimebase = 48;
constexpr int64_t kStartingTempo = 120;

// The first of a track's variables, past the 16 of the sound and the 16 global ones.
constexpr int kFirstTrackVariable = 32;
constexpr int kTrackVariables = 16;
constexpr int kSoundVariables = 16;

// An envelope's value on the DS that leaves the region's value in place.
constexpr int64_t kRegionsValue = 255;

// The velocities that the DS's tables hold.
constexpr int64_t kHighestVelocity = 127;

// The lowest and the highest of the console's values that the DS's bytes hold for a command of `kind`.
struct HeldValues
{
    int64_t low = 0;
    int64_t high = 0;
};

// The values that a conversion to the DS limits a command of `kind` to: the values that the DS plays, such as volumes
// up to 127. NtrRange in edit/command_ranges.cpp returns the values that the bytes can encode for the List Editor
// instead.
HeldValues HeldValuesOf(Kind kind)
{
    constexpr int64_t kByte = 255;
    constexpr int64_t kSeven = 127;
    constexpr int64_t kHighest16 = std::numeric_limits<int16_t>::max();
    constexpr int64_t kLowest16 = std::numeric_limits<int16_t>::min();
    switch (kind)
    {
    case Kind::kNote:
    case Kind::kWait:
    case Kind::kProgram:
        return {0, 0xFFFFFFF};
    case Kind::kVolume:
    case Kind::kMainVolume:
    case Kind::kVolume2:
    case Kind::kPan:
    case Kind::kPortamentoFrom:
        return {0, kSeven};
    case Kind::kTranspose:
    case Kind::kPitchBend:
        return {-128, kSeven};
    case Kind::kTempo:
    case Kind::kLfoDelay:
        return {0, kHighest16};
    case Kind::kSweepPitch:
        return {kLowest16, kHighest16};
    default:
        break;
    }

    if (kind >= Kind::kSetVariable && kind <= Kind::kCompareNotEqual)
    {
        return {kLowest16, kHighest16};
    }

    return {0, kByte};
}

// Clamps `value`, in the units of commands/units.h, to the values `held` of the console. Returns whether the value
// changed.
bool HoldValue(int64_t& value, const Scale& scale, const HeldValues& held)
{
    const int64_t console = FromUnit(value, scale);
    const int64_t kept = std::clamp(console, held.low, held.high);
    if (kept == console)
    {
        return false;
    }

    value = ToUnit(kept, scale);

    return true;
}

// Clamps the values of `command` to what the bytes of the DS can store. Returns how many changed. A random value's ends
// are 16 bits on the DS, whatever the command's width.
std::size_t HoldValues(Command& command)
{
    std::size_t changed = 0;
    if (command.kind == Kind::kNote && FromUnit(command.velocity, Scale{}) > kHighestVelocity)
    {
        command.velocity = ToUnit(kHighestVelocity, Scale{});
        ++changed;
    }

    Argument& value = command.value;
    if (value.source == Source::kVariable)
    {
        return changed;
    }

    const Scale scale = ntdlib::ntr::snd::ScaleOf(command.kind);
    HeldValues held = HeldValuesOf(command.kind);
    if (value.source == Source::kRandom)
    {
        held.low = std::max<int64_t>(held.low, std::numeric_limits<int16_t>::min());
        held.high = std::min<int64_t>(held.high, std::numeric_limits<int16_t>::max());
        changed += HoldValue(value.high, scale, held) ? 1 : 0;
    }

    changed += HoldValue(value.value, scale, held) ? 1 : 0;

    return changed;
}

// Whether a command of `kind` names a variable in its index.
bool NamesVariable(Kind kind)
{
    return (kind >= Kind::kSetVariable && kind <= Kind::kCompareNotEqual) || kind == Kind::kPrintVariable;
}

// What the commands from `start` give of the song's start: its timebase, and whether a tempo runs there. The walk
// follows the track until a command that can take time, or until a command that runs only if a comparison held.
struct SongStart
{
    std::optional<int64_t> timebase;
    bool tempo = false;
};

SongStart SongStartOf(const Sequence& sequence, uint32_t start)
{
    SongStart found;
    std::set<uint32_t> seen;
    uint32_t item = start;
    while (item < sequence.items.size() && seen.insert(item).second)
    {
        const Command& command = sequence.items[item];
        if (command.kind == Kind::kNote || command.kind == Kind::kWait || command.kind == Kind::kCall ||
            command.kind == Kind::kEnd || command.kind == Kind::kReturn || command.kind == Kind::kData ||
            command.conditional)
        {
            break;
        }

        if (command.kind == Kind::kTimebase && !found.timebase && command.value.source == Source::kValue)
        {
            found.timebase = FromUnit(command.value.value, Scale{});
        }

        if (command.kind == Kind::kTempo)
        {
            found.tempo = true;
        }

        item = command.kind == Kind::kJump ? command.target : item + 1;
    }

    return found;
}

// A tempo in the units of the DS's 48 ticks to a quarter note, at a song's timebase.
int64_t ScaledTempo(int64_t tempo, int64_t timebase)
{
    return std::llround(static_cast<double>(tempo) * static_cast<double>(timebase) / kDsTimebase);
}

// The deepest calls and counted loops inside one another that a track of `sequence` reaches from item 0, up to
// kDsNestLimit + 1. A deeper nest counts as that.
int DeepestNest(const Sequence& sequence)
{
    constexpr std::size_t kMostStates = 4'000'000;
    const auto after = [](const Command& command, uint32_t, const std::vector<int>& before)
    {
        std::vector<int> states;
        for (const int depth : before)
        {
            if (command.kind == Kind::kCall || command.kind == Kind::kLoopStart)
            {
                states.push_back(std::min(depth + 1, kDsNestLimit + 1));
            }
            else if (command.kind == Kind::kReturn || command.kind == Kind::kLoopEnd)
            {
                states.push_back(std::max(depth - 1, 0));
            }
            else
            {
                states.push_back(depth);
            }
        }

        std::ranges::sort(states);
        states.erase(std::ranges::unique(states).begin(), states.end());

        return states;
    };

    constexpr uint32_t kFirst = 0;
    const auto states = song::TrackStates(sequence, std::span<const uint32_t>(&kFirst, 1), 0, after, kMostStates);
    int deepest = 0;
    if (states)
    {
        for (const std::vector<int>& held : *states)
        {
            if (!held.empty())
            {
                deepest = std::max(deepest, held.back());
            }
        }
    }

    return deepest;
}

// Moves each track's variable that the reached commands of `sequence` use to a sound's variable that they leave free.
void MoveTrackVariables(Sequence& sequence, DsCommands& result)
{
    std::set<int> used;
    const auto add = [&used](int64_t number)
    {
        used.insert(static_cast<int>(number));
    };
    for (const Command& command : sequence.items)
    {
        if (command.value.source == Source::kVariable)
        {
            add(command.value.value);
        }

        if (NamesVariable(command.kind))
        {
            add(command.index);
        }
    }

    int free = kSoundVariables - 1;
    for (int number = kFirstTrackVariable; number < kFirstTrackVariable + kTrackVariables; ++number)
    {
        if (!used.contains(number))
        {
            continue;
        }

        while (free >= 0 && used.contains(free))
        {
            --free;
        }
        if (free < 0)
        {
            result.stuck_variables.push_back(number);
            continue;
        }

        result.moved_variables[number] = free--;
    }
    if (result.moved_variables.empty())
    {
        return;
    }

    const auto moved = [&result](int64_t number)
    {
        const auto found = result.moved_variables.find(static_cast<int>(number));
        return found != result.moved_variables.end() ? int64_t{found->second} : number;
    };
    for (Command& command : sequence.items)
    {
        if (command.value.source == Source::kVariable)
        {
            command.value.value = moved(command.value.value);
        }

        if (NamesVariable(command.kind))
        {
            command.index = static_cast<uint8_t>(moved(command.index));
        }
    }
}

// Adds `command`, a command that runs on the DS, to `out` as the DS reads it: without its encoding and its time, and
// with its tempo for the DS's timebase (DsCommandsOfItems). Counts in `result` the times, the tempos that stay unscaled
// and the values that change.
void AddKept(const Command& command, DsCommands& result, std::vector<Command>& out)
{
    Command& kept = out.emplace_back(command);
    kept.encoding = {};
    if (kept.time)
    {
        kept.time.reset();
        ++result.times;
    }
    if (kept.kind == Kind::kTempo && result.timebase != kDsTimebase)
    {
        if (kept.value.source == Source::kVariable)
        {
            ++result.unscaled_tempos;
        }
        else
        {
            kept.value.value = ScaledTempo(kept.value.value, result.timebase);
            kept.value.high = ScaledTempo(kept.value.high, result.timebase);
        }
    }

    result.held_values += HoldValues(kept);
}

// The commands that each item of `sequence` that `reached` marks becomes on the DS, from console `from` (AsDs). Counts
// in `result` the commands that stay out, the times, the tempos and the values that change.
std::vector<std::vector<Command>> DsCommandsOfItems(const Sequence& sequence, const std::vector<bool>& reached,
                                                    const song::Console& from, DsCommands& result)
{
    const song::Console to{.engine = format::Engine::kDs};

    // Whether each kind and index runs on each console. The answers require the encoders.
    std::map<std::pair<Kind, uint8_t>, std::pair<bool, bool>> runs;
    const auto runs_on = [&](const Command& command) -> const std::pair<bool, bool>&
    {
        const auto [place, added] = runs.try_emplace({command.kind, command.index});
        if (added)
        {
            place->second = {song::Runs(from, command.kind, command.index),
                             song::Runs(to, command.kind, command.index)};
        }

        return place->second;
    };

    // The commands that each reached item becomes.
    std::vector<std::vector<Command>> becomes(sequence.items.size());
    for (std::size_t item = 0; item < sequence.items.size(); ++item)
    {
        if (!reached[item])
        {
            continue;
        }

        const Command& command = sequence.items[item];
        const auto& [runs_there, runs_here] = runs_on(command);
        if (!runs_there)
        {
            ++result.left_out[song::SettingName(command)];
            continue;
        }

        switch (command.kind)
        {
        case Kind::kAllocateTracks:
        case Kind::kTimebase:
            break;

        case Kind::kEnvelopeReset:
            for (const Kind setting : {Kind::kAttack, Kind::kDecay, Kind::kSustain, Kind::kRelease})
            {
                becomes[item].push_back(
                    Command{.kind = setting,
                            .conditional = command.conditional,
                            .value = Argument{.value = ToUnit(kRegionsValue, ntdlib::ntr::snd::ScaleOf(setting))}});
            }
            break;

        default:
            if (!runs_here)
            {
                ++result.left_out[song::SettingName(command)];
                break;
            }

            AddKept(command, result, becomes[item]);
            break;
        }
    }

    return becomes;
}

// The commands at the file's start: the tracks, the tempo that the timebase requires and a jump to the sound's
// start. Track 0 always plays. A loop to the sound's start comes back past them.
std::vector<Command> OpeningCommands(uint32_t tracks, const SongStart& song_start, int64_t timebase)
{
    std::vector<Command> opening;
    if ((tracks & ~uint32_t{1}) != 0)
    {
        opening.push_back(Command{.kind = Kind::kAllocateTracks, .value = Argument{.value = ToUnit(tracks, Scale{})}});
    }

    if (timebase != kDsTimebase && !song_start.tempo)
    {
        const int64_t tempo = ScaledTempo(ToUnit(kStartingTempo, ntdlib::ntr::snd::ScaleOf(Kind::kTempo)), timebase);
        opening.push_back(Command{.kind = Kind::kTempo, .value = Argument{.value = tempo}});
    }

    return opening;
}

// The new index of each item of a sequence whose items become `becomes`, and of its end, after `opening` (AsDs). Where
// commands come before the sound's start, item `start`, `opening` gains a jump to it.
std::vector<uint32_t> PlaceItems(const std::vector<std::vector<Command>>& becomes, uint32_t start,
                                 std::vector<Command>& opening)
{
    std::vector<uint32_t> first(becomes.size() + 1);
    bool before_start = false;
    uint32_t at = static_cast<uint32_t>(opening.size());
    for (std::size_t item = 0; item < becomes.size(); ++item)
    {
        first[item] = at;
        at += static_cast<uint32_t>(becomes[item].size());
        before_start = before_start || (item < start && !becomes[item].empty());
    }

    first[becomes.size()] = at;
    if (before_start)
    {
        // The jump itself moves every item on by one.
        for (uint32_t& place : first)
        {
            ++place;
        }

        opening.push_back(Command{.kind = Kind::kJump, .target = first.at(start)});
    }

    return first;
}

// Writes `opening` and the commands of `becomes` into `out`, with their targets moved to the items that `first`
// contains (AsDs).
void WriteDsCommands(std::vector<Command> opening, std::vector<std::vector<Command>>& becomes,
                     const std::vector<uint32_t>& first, Sequence& out)
{
    // The commands, with their targets moved. An end follows a path that would run off the last command.
    out.items = std::move(opening);
    bool past_end = false;
    for (std::vector<Command>& commands : becomes)
    {
        for (Command& command : commands)
        {
            if (ntdlib::commands::HasTarget(command.kind) && !command.target_outside)
            {
                command.target = first.at(command.target);
                past_end = past_end || command.target == first.back();
            }

            out.items.push_back(std::move(command));
        }
    }
    if (past_end || out.items.empty() || RunsOn(out.items.back()))
    {
        out.items.push_back(Command{.kind = Kind::kEnd});
    }
}

} // namespace

DsCommands AsDs(const Sequence& sequence, uint32_t start, uint32_t tracks, const song::Console& from)
{
    DsCommands result;
    const std::vector<bool> reached = Reached(sequence, start, true);

    // The song's timebase, and whether another runs. A timebase below 1 stops a later console's player, and stays out
    // with the others.
    const SongStart song_start = SongStartOf(sequence, start);
    result.timebase = song_start.timebase && *song_start.timebase > 0 ? *song_start.timebase : kDsTimebase;
    for (std::size_t item = 0; item < sequence.items.size(); ++item)
    {
        const Command& command = sequence.items[item];
        if (reached[item] && command.kind == Kind::kTimebase &&
            (command.value.source != Source::kValue || FromUnit(command.value.value, Scale{}) != result.timebase))
        {
            result.timebase_changes = true;
        }
    }

    std::vector<std::vector<Command>> becomes = DsCommandsOfItems(sequence, reached, from, result);
    std::vector<Command> opening = OpeningCommands(tracks, song_start, result.timebase);
    const std::vector<uint32_t> first = PlaceItems(becomes, start, opening);
    Sequence& out = result.sequence;
    WriteDsCommands(std::move(opening), becomes, first, out);

    MoveTrackVariables(out, result);
    result.deepest_nest = DeepestNest(out);

    return result;
}

void AddDsCommandWarnings(const DsCommands& commands, const song::Console& from, std::vector<std::string>& warnings)
{
    if (const std::optional<std::string> warning =
            SkippedWarning(commands.left_out, from, song::Console{.engine = format::Engine::kDs}, true))
    {
        warnings.push_back(*warning);
    }

    if (commands.times > 0)
    {
        warnings.push_back(commands.times == 1
                               ? "The DS applies settings at once, without a ramp. The ramp time of 1 setting is "
                                 "dropped."
                               : "The DS applies settings at once, without a ramp. The ramp times of " +
                                     std::to_string(commands.times) + " settings are dropped.");
    }

    if (commands.held_values > 0)
    {
        warnings.push_back(
            commands.held_values == 1
                ? "1 value of the sequence is out of range for the DS. It is set to the nearest value in range."
                : std::to_string(commands.held_values) +
                      " values of the sequence are out of range for the DS. They are set to the nearest values in "
                      "range.");
    }

    if (commands.timebase_changes)
    {
        warnings.push_back(
            "The sequence changes its ticks per quarter note during playback. The DS always uses 48. The tempos "
            "follow the ticks per quarter note at the start. After a change, the song plays differently.");
    }

    if (commands.unscaled_tempos > 0)
    {
        warnings.push_back(std::to_string(commands.unscaled_tempos) +
                           (commands.unscaled_tempos == 1 ? " tempo comes" : " tempos come") +
                           " from a variable. The DS uses 48 ticks per quarter note, but the song uses " +
                           std::to_string(commands.timebase) +
                           (commands.unscaled_tempos == 1 ? ". That tempo plays at a different speed."
                                                          : ". Those tempos play at a different speed."));
    }

    if (!commands.moved_variables.empty())
    {
        std::vector<std::string> moved;
        for (const auto& [from_number, to_number] : commands.moved_variables)
        {
            moved.push_back(std::to_string(from_number) + " to " + std::to_string(to_number));
        }

        warnings.push_back("The DS has no track variables. Sound variables replace them: " + ListOf(moved) +
                           ". Tracks that used the same track variable now share one sound variable.");
    }

    if (commands.deepest_nest > kDsNestLimit)
    {
        warnings.push_back("The calls and counted loops of the sequence nest more than " +
                           std::to_string(kDsNestLimit) +
                           " deep. NITRO-Composer skips a deeper call or loop. The track plays differently there.");
    }
}

} // namespace hokuto::edit
