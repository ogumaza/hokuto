// SPDX-License-Identifier: MIT

// The commands of a sequence on the way between the NintendoWare consoles (edit/other_console.h): the commands that a
// console skips or plays otherwise, the low-pass filters, the global variables, the times of the settings, and the
// encoding.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/nwsnd/commands.h"
#include "ntdlib/nwsnd/console.h"
#include "ntdlib/nwsnd/units.h"
#include "ntdlib/nwsnd/util.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sound_build.h"

#include "edit/bytecode.h"
#include "edit/other_console.h"
#include "edit/other_console_parts.h"
#include "edit/variables.h"
#include "format/kinds.h"
#include "format/model.h"
#include "format/shared.h"
#include "song/archive.h"
#include "song/biquads.h"
#include "song/driver.h"
#include "song/items.h"
#include "song/sequence.h"

namespace hokuto::edit
{

namespace
{

namespace sar = ntdlib::sar;
using other_console_parts::AddTimeWarnings;
using other_console_parts::AsWii;
using other_console_parts::CountSkipped;
using other_console_parts::GlobalsWarning;
using other_console_parts::ListOf;
using other_console_parts::PlaysOtherwise;
using other_console_parts::SkippedWarning;
using other_console_parts::WiiCommands;
using other_console_parts::WithoutItems;
using other_console_parts::WithoutSkipped;
using other_console_parts::WithStartingGlobals;

} // namespace

namespace other_console_parts
{

// Whether `command` plays otherwise on console `to` than on console `from`: a command that the player of either side
// skips (song::Runs), or one that does nothing on `from` and that `to` reads as part of another command. The 3DS and
// the Wii read 0xF0 with 0xA0 to 0xBF without a byte, and the Wii U and the Switch read the settings of LFOs 1 to 3
// there. The Wii's builds before March 2008 read 0xA3 to 0xA5 as commands that do nothing. The later consoles read time
// prefixes there.
bool PlaysOtherwise(const ntdlib::commands::Command& command, const song::Console& from, const song::Console& to)
{
    if (!song::Runs(from, command.kind, command.index) || !song::Runs(to, command.kind, command.index))
    {
        return true;
    }
    if (command.kind != ntdlib::commands::Kind::kOther)
    {
        return false;
    }

    const ntdlib::nwsnd::Grammar read = song::GrammarOf(from);
    const ntdlib::nwsnd::Grammar written = song::GrammarOf(to);
    const uint16_t opcode = command.encoding.opcode;
    return (!read.extra_lfo_bytes && written.extra_lfo_bytes && opcode >= 0xF0A0 && opcode <= 0xF0BF) ||
           (!read.time_prefixes && written.time_prefixes && opcode >= 0xA3 && opcode <= 0xA5);
}

// The warning about `counts`, the commands of each kind that play otherwise on console `to` than on console `from`
// (PlaysOtherwise), or nothing. With `left_out`, the new file leaves them out. Otherwise they stay in their places.
std::optional<std::string> SkippedWarning(const std::map<std::string, std::size_t>& counts, const song::Console& from,
                                          const song::Console& to, bool left_out)
{
    std::size_t total = 0;
    std::vector<std::string> kinds;
    for (const auto& [name, count] : counts)
    {
        total += count;
        kinds.push_back(name + " (" + std::to_string(count) + ")");
    }
    if (total == 0)
    {
        return std::nullopt;
    }

    const bool one = total == 1;
    const std::string commands = std::to_string(total) + (one ? " command" : " commands");
    std::string warning = left_out ? "The new file leaves out " + commands + ": "
                                   : commands + (one ? " stays in the file: " : " stay in the file: ");
    warning += ListOf(kinds) + ".";
    if (to.engine == format::Engine::kDs)
    {
        return warning + (one ? " The DS skips it." : " The DS skips them.");
    }
    if (from.wii)
    {
        return warning + (one ? " The Wii skipped it." : " The Wii skipped them.");
    }
    if (to.wii)
    {
        return warning + (one ? " The Wii skips it." : " The Wii skips them.");
    }
    if (FlavorOf(to) == sar::Flavor::kCtr)
    {
        return warning + (one ? " The 3DS skips it." : " The 3DS skips them.");
    }

    return warning + (one ? " The 3DS skipped it. The " : " The 3DS skipped them. The ") +
           song::ConsoleName(FlavorOf(to)) + (left_out ? " would run " : " runs ") + (one ? "it." : "them.");
}

// The commands of `sequence` that play otherwise on console `to` than on console `from` (PlaysOtherwise), by what they
// set.
std::map<std::string, std::size_t> CountSkipped(const ntdlib::commands::Sequence& sequence, const song::Console& from,
                                                const song::Console& to)
{
    std::map<std::string, std::size_t> counts;
    for (const ntdlib::commands::Command& command : sequence.items)
    {
        if (PlaysOtherwise(command, from, to))
        {
            counts[song::SettingName(command)]++;
        }
    }

    return counts;
}

// `sequence` without the items that `leaves` marks, with item `start` where the sound starts. The targets, the labels
// and the start move to the items that stay. Code that entered at an item that leaves enters at the next item that
// stays, or at the end.
std::pair<ntdlib::commands::Sequence, uint32_t> WithoutItems(const ntdlib::commands::Sequence& sequence, uint32_t start,
                                                             const std::vector<bool>& leaves)
{
    std::vector<uint32_t> moved(sequence.items.size() + 1);
    uint32_t kept = 0;
    for (std::size_t item = 0; item <= sequence.items.size(); ++item)
    {
        moved[item] = kept;
        kept += item < sequence.items.size() && leaves[item] ? 0 : 1;
    }

    ntdlib::commands::Sequence result;
    for (std::size_t item = 0; item < sequence.items.size(); ++item)
    {
        if (leaves[item])
        {
            continue;
        }

        ntdlib::commands::Command& command = result.items.emplace_back(sequence.items[item]);
        if (ntdlib::commands::HasTarget(command.kind) && !command.target_outside)
        {
            command.target = moved.at(command.target);
        }
    }

    for (ntdlib::commands::Label label : sequence.labels)
    {
        label.item = moved.at(label.item);
        result.labels.push_back(std::move(label));
    }

    return {std::move(result), moved.at(start)};
}

// `sequence` without the commands that play otherwise on console `to` than on console `from` (PlaysOtherwise), with
// item `start` where the sound starts (WithoutItems).
std::pair<ntdlib::commands::Sequence, uint32_t> WithoutSkipped(const ntdlib::commands::Sequence& sequence,
                                                               uint32_t start, const song::Console& from,
                                                               const song::Console& to)
{
    std::vector<bool> leaves(sequence.items.size());
    for (std::size_t item = 0; item < sequence.items.size(); ++item)
    {
        leaves[item] = PlaysOtherwise(sequence.items[item], from, to);
    }

    return WithoutItems(sequence, start, leaves);
}

// The 3DS's player sets volume2 and the main volume at once, and their time only takes its bytes
// (ntdlib::nwsnd::CtrPolicy). The players of the Wii U and the Switch move such a value over the time. Takes the times
// of those settings out of `sequence`, a sequence of the 3DS, and gives how many went.
std::size_t WithoutCtrTimes(ntdlib::commands::Sequence& sequence)
{
    using ntdlib::commands::Kind;
    std::size_t times = 0;
    for (ntdlib::commands::Command& command : sequence.items)
    {
        if (command.time && (command.kind == Kind::kVolume2 || command.kind == Kind::kMainVolume))
        {
            command.time.reset();
            ++times;
        }
    }

    return times;
}

// The warning about `times` settings of volume2 and the main volume of a sequence of the 3DS that go to the console of
// `to`: settings that lost their times (WithoutCtrTimes), or that keep them in a sequence whose commands keep their
// places.
std::string CtrTimesWarning(std::size_t times, sar::Flavor to, bool lost)
{
    const bool one = times == 1;
    const std::string count = std::to_string(times);
    const std::string console = song::ConsoleName(to);
    if (lost)
    {
        return "The 3DS applies volume2 and main volume changes at once, without a ramp. " +
               (one ? "The ramp time of 1 such change is dropped. The " + console + " applies it at once too."
                    : "The ramp times of " + count + " such changes are dropped. The " + console +
                          " applies them at once too.");
    }

    return "The 3DS applies volume2 and main volume changes at once, without a ramp. " +
           (one ? "1 such change keeps its ramp time because the commands in the file must stay at their "
                  "offsets. The " +
                      console + " ramps it over that time."
                : count +
                      " such changes keep their ramp times because the commands in the file must stay at their "
                      "offsets. The " +
                      console + " ramps them over those times.");
}

// Whether the console of `to` reads each command of `sequence` with the length that the console of `from` reads. The
// encoder refuses a sequence whose bytes keep their places when a command's length changes.
bool SameLengths(const ntdlib::commands::Sequence& sequence, sar::Flavor from, sar::Flavor to)
{
    try
    {
        return Offsets(sequence, song::Console{.engine = format::EngineOf(from)}) ==
               Offsets(sequence, song::Console{.engine = format::EngineOf(to)});
    }
    catch (const std::invalid_argument&)
    {
        return false;
    }
}

} // namespace other_console_parts

namespace
{

// The value that NintendoWare works out a cutoff from, for `fixed`, the value of a low-pass command (units.h): 1 + the
// value.
float LowPassValue(int64_t fixed)
{
    return 1.0f + static_cast<float>(static_cast<double>(fixed) / static_cast<double>(ntdlib::commands::kOne));
}

// Of the values that a low-pass command can have, the one at the least `distance`. Of those at the same distance, the
// one nearest `lpf` goes in. The command holds a byte: a value of (byte - 64) / 64.
template <typename Distance>
int64_t NearestLowPass(int64_t lpf, Distance distance)
{
    using ntdlib::commands::kOne;
    int64_t best = lpf;
    double nearest = std::numeric_limits<double>::infinity();
    for (int64_t byte = 0; byte < 128; ++byte)
    {
        const int64_t candidate = (byte - 64) * kOne / 64;
        const double octaves = distance(candidate);
        if (octaves < nearest || (octaves == nearest && std::llabs(candidate - lpf) < std::llabs(best - lpf)))
        {
            best = candidate;
            nearest = octaves;
        }
    }

    return best;
}

// For the value `lpf` of a low-pass command (units.h), the value whose cutoff on the later consoles lies nearest the
// cutoff that NW4R SND's `build` gives `lpf`. Builds before November 2008 work the cutoff out: 32000 Hz times 2^(10
// (value - 1)), for the value 1 + lpf held to 0 and 1. The later consoles take it from a table of 24 cutoffs from 80 Hz
// to 16 kHz, a third of an octave apart, at the table's step below the value. Of the command's values whose cutoffs lie
// nearest in octaves, the one nearest `lpf` goes in. From 16 kHz, neither filters. Later builds read the table as the
// later consoles do, and `lpf` stays.
int64_t LaterLowPass(int64_t lpf, const ntdlib::SndBuild& build)
{
    const uint16_t wii = ntdlib::nwsnd::CalcLpfFreq<ntdlib::nwsnd::RevolutionPolicy>(LowPassValue(lpf), build);
    if (build.SinceNovember2008() || wii >= 16000)
    {
        return lpf;
    }

    const auto distance = [wii](int64_t candidate)
    {
        const uint16_t later = ntdlib::nwsnd::CalcLpfFreq<ntdlib::nwsnd::CafePolicy>(LowPassValue(candidate));
        return std::abs(std::log2(static_cast<double>(later) / static_cast<double>(wii)));
    };

    return NearestLowPass(lpf, distance);
}

} // namespace

namespace other_console_parts
{

// The commands of a sequence of the Wii in the forms in which the later consoles read the same commands. `build` is the
// build of NW4R SND that plays them. The Wii's end after a comparison ends the track whatever the comparison gave, and
// the end loses its comparison. A random prefix before a variable prefix gives nothing on the Wii: the variable gives
// the value. The later consoles would read a command 0xA1 after the random prefix, and the random prefix goes. NW4R SND
// sets volume2, the main volume and, before September 2009, the pitch bend at once. Their times go. A low-pass from a
// value or a random number takes the values whose cutoffs on the later consoles lie nearest (LaterLowPass).
ntdlib::commands::Sequence AsLaterConsoles(ntdlib::commands::Sequence sequence, const ntdlib::SndBuild& build)
{
    using ntdlib::commands::Kind;
    using ntdlib::commands::Source;
    for (ntdlib::commands::Command& command : sequence.items)
    {
        if (command.kind == Kind::kEnd)
        {
            command.conditional = false;
        }

        if (command.value.source == Source::kVariable || command.encoding.unused_variable)
        {
            command.encoding.unused_random = false;
        }

        if (command.kind == Kind::kVolume2 || command.kind == Kind::kMainVolume ||
            (command.kind == Kind::kPitchBend && !ntdlib::nwsnd::RevolutionPolicy::BendRamps(build)))
        {
            command.time.reset();
        }

        if (command.kind == Kind::kLowPass && command.value.source != Source::kVariable)
        {
            command.value.value = LaterLowPass(command.value.value, build);
            if (command.value.source == Source::kRandom)
            {
                command.value.high = LaterLowPass(command.value.high, build);
            }
        }
    }

    return sequence;
}

// Which items of `sequence`, a sequence of the Wii, hold bytes that no command reaches: data at the start, or after a
// jump without a comparison, an end or such data. The Wii's ends end their tracks whatever a comparison gave. A track
// that reaches a return outside every call runs on past it, and data after a return can play.
std::vector<bool> DeadData(const ntdlib::commands::Sequence& sequence)
{
    using ntdlib::commands::Kind;
    std::vector<bool> dead(sequence.items.size());
    for (std::size_t item = 0; item < sequence.items.size(); ++item)
    {
        if (sequence.items[item].kind != Kind::kData)
        {
            continue;
        }

        const ntdlib::commands::Command* before = item > 0 ? &sequence.items[item - 1] : nullptr;
        dead[item] = before == nullptr || dead[item - 1] || before->kind == Kind::kEnd ||
                     (before->kind == Kind::kJump && !before->conditional);
    }

    return dead;
}

// Gives the sound that starts at item `start` of `sequence` commands at its start that set each global variable that
// the sound reads before it sets it, and whose starting value can change what the sound plays (song::VariablesOf), to
// `wanted` while it holds `held`. NW4R SND starts the global variables at 0, and the later consoles start them at -1.
// Each such variable takes a comparison with `held` and a setting after it. A comparison of the last variable with
// itself then gives the comparison's flag its starting value. Returns the item where the sound starts, and the
// variables' numbers.
std::pair<uint32_t, std::vector<int>> WithStartingGlobals(ntdlib::commands::Sequence& sequence, uint32_t start,
                                                          int64_t held, int64_t wanted)
{
    using ntdlib::commands::Argument;
    using ntdlib::commands::Command;
    using ntdlib::commands::Kind;
    using ntdlib::commands::Source;
    std::vector<int> globals;
    for (const song::Variable& variable : song::VariablesOf(sequence, start).starting)
    {
        if (variable.number >= kSoundVariables && variable.number < song::kGameVariables)
        {
            globals.push_back(variable.number);
        }
    }
    if (globals.empty())
    {
        return {start, {}};
    }

    std::vector<Command> commands;
    for (const int global : globals)
    {
        const auto index = static_cast<uint8_t>(global);
        commands.push_back(song::ValueCommand(Kind::kCompareEqual, held, index));
        Command set = song::ValueCommand(Kind::kSetVariable, wanted, index);
        set.conditional = true;
        commands.push_back(std::move(set));
    }

    commands.push_back(Command{.kind = Kind::kCompareEqual,
                               .index = static_cast<uint8_t>(globals.back()),
                               .value = Argument{Source::kVariable, globals.back(), 0}});

    return {InsertAtStart(sequence, start, std::move(commands)), globals};
}

// The warning about the global variables `globals`. Commands at the sound's start set their starting values to the
// value that the console of the sound's file gives them (WithStartingGlobals). `to_wii` says whether the sound goes to
// the Wii.
std::string GlobalsWarning(std::span<const int> globals, bool to_wii)
{
    std::vector<std::string> numbers;
    for (const int global : globals)
    {
        numbers.push_back(std::to_string(global));
    }

    const bool one = globals.size() == 1;
    return "The sound reads global " + std::string(one ? "variable " : "variables ") + ListOf(numbers) +
           " before setting " + (one ? "it" : "them") +
           ". NW4R SND starts global variables at 0. Later consoles start them at -1. New commands at the start of "
           "the sound set " +
           (one ? "it" : "each one") + (to_wii ? " to -1 if it is 0." : " to 0 if it is -1.");
}

} // namespace other_console_parts

namespace
{

// The cutoff in Hz that the console of `flavor` gives a low-pass value from 0 to 1 (Util::CalcLpfFreq).
uint16_t LaterCutoff(float value, sar::Flavor flavor)
{
    switch (flavor)
    {
    case sar::Flavor::kCtr:
        return ntdlib::nwsnd::CalcLpfFreq<ntdlib::nwsnd::CtrPolicy>(value);
    case sar::Flavor::kCafe:
        return ntdlib::nwsnd::CalcLpfFreq<ntdlib::nwsnd::CafePolicy>(value);
    case sar::Flavor::kNx:
        break;
    }

    return ntdlib::nwsnd::CalcLpfFreq<ntdlib::nwsnd::NxPolicy>(value);
}

// For the value `lpf` of a low-pass command of the console of `from` (units.h), the value whose cutoff in NW4R SND's
// `build` lies nearest in octaves the cutoff that the console of `from` gives `lpf`: LaterLowPass the other way. Of the
// command's values at the same distance, the one nearest `lpf` goes in. From 16 kHz, neither filters: a value that
// filters nothing on the console of `from` takes the value nearest `lpf` that filters nothing on the Wii. Builds from
// November 2008 read the later consoles' table, and `lpf` stays.
int64_t WiiLowPass(int64_t lpf, sar::Flavor from, const ntdlib::SndBuild& build)
{
    if (build.SinceNovember2008())
    {
        return lpf;
    }

    const auto wii = [&build](int64_t fixed)
    {
        return ntdlib::nwsnd::CalcLpfFreq<ntdlib::nwsnd::RevolutionPolicy>(LowPassValue(fixed), build);
    };

    constexpr uint16_t kOpen = 16000;
    const uint16_t later = LaterCutoff(LowPassValue(lpf), from);
    if (later >= kOpen && wii(lpf) >= kOpen)
    {
        return lpf;
    }

    constexpr double kFar = std::numeric_limits<double>::infinity();
    const auto distance = [&wii, later](int64_t candidate)
    {
        const uint16_t cutoff = wii(candidate);
        return later >= kOpen ? (cutoff >= kOpen ? 0.0 : kFar)
                              : std::abs(std::log2(static_cast<double>(cutoff) / static_cast<double>(later)));
    };

    return NearestLowPass(lpf, distance);
}

} // namespace

namespace other_console_parts
{

// The commands of a sequence of the console of `from` in the forms in which NW4R SND's `build` reads the same commands:
// AsLaterConsoles the other way. The Wii's end after a comparison ends the track whatever the comparison gave. Such an
// end becomes a jump after the comparison to an end after the sequence's items, and plays as before. NW4R SND sets
// volume2, the main volume and, before September 2009, the pitch bend at once. Their times go. The builds before March
// 2008 lack the time prefixes (RevolutionPolicy::HasCommand). Every setting's time goes there, and the setting takes
// its value at once. A low-pass from a value or a random number takes the values whose cutoffs on the Wii lie nearest
// (WiiLowPass). The later consoles keep a sweep's pitch in 64ths of a semitone. The builds before March 2008 keep it in
// 32nds. A sweep's pitch takes the build's nearest pitch: the pitch that the encoder writes. The text form then holds
// the pitch too.
WiiCommands AsWii(ntdlib::commands::Sequence sequence, sar::Flavor from, const ntdlib::SndBuild& build)
{
    using ntdlib::commands::Command;
    using ntdlib::commands::Kind;
    using ntdlib::commands::Source;
    WiiCommands result;
    const auto end = static_cast<uint32_t>(sequence.items.size());
    const bool prefixes = ntdlib::nwsnd::RevolutionPolicy::HasCommand(build, 0xA3);
    const ntdlib::nwsnd::Scale sweep =
        ntdlib::nwsnd::ScaleOf(Kind::kSweepPitch, ntdlib::nwsnd::GrammarOf<ntdlib::nwsnd::RevolutionPolicy>(build));
    const auto held_pitch = [&sweep](int64_t pitch)
    {
        return ntdlib::nwsnd::ToUnit(ntdlib::nwsnd::FromUnit(pitch, sweep), sweep);
    };

    bool ends = false;
    for (Command& command : sequence.items)
    {
        if (command.kind == Kind::kEnd && command.conditional)
        {
            command = Command{.kind = Kind::kJump, .conditional = true, .target = end};
            ends = true;
        }
        if (command.time && (command.kind == Kind::kVolume2 || command.kind == Kind::kMainVolume ||
                             (command.kind == Kind::kPitchBend && !ntdlib::nwsnd::RevolutionPolicy::BendRamps(build))))
        {
            command.time.reset();
            ++result.times;
        }
        else if (command.time && !prefixes)
        {
            command.time.reset();
            ++result.untimed;
        }

        if (command.kind == Kind::kLowPass && command.value.source != Source::kVariable)
        {
            command.value.value = WiiLowPass(command.value.value, from, build);
            if (command.value.source == Source::kRandom)
            {
                command.value.high = WiiLowPass(command.value.high, from, build);
            }
        }
        if (command.kind == Kind::kSweepPitch && command.value.source != Source::kVariable)
        {
            command.value.value = held_pitch(command.value.value);
            if (command.value.source == Source::kRandom)
            {
                command.value.high = held_pitch(command.value.high);
            }
        }
    }
    if (ends)
    {
        sequence.items.push_back(Command{.kind = Kind::kEnd});
    }

    result.sequence = std::move(sequence);

    return result;
}

// The warnings about the settings of `commands` whose times went (AsWii), after `warnings`.
void AddTimeWarnings(const WiiCommands& commands, std::vector<std::string>& warnings)
{
    if (commands.times > 0)
    {
        const bool one = commands.times == 1;
        warnings.push_back(
            "NW4R SND applies volume2, main volume and pitch bend changes at once, without a ramp. " +
            (one ? std::string("The ramp time of 1 such change is dropped.")
                 : "The ramp times of " + std::to_string(commands.times) + " such changes are dropped."));
    }

    if (commands.untimed > 0)
    {
        const bool one = commands.untimed == 1;
        warnings.push_back("NW4R SND builds before March 2008 apply every setting at once. " +
                           std::string(one ? "The ramp time of " : "The ramp times of ") +
                           std::to_string(commands.untimed) + std::string(commands.times > 0 ? " other" : "") +
                           (one ? " change is dropped." : " changes are dropped."));
    }
}

} // namespace other_console_parts

CommandsForWii WiiCommandsOf(const ntdlib::commands::Sequence& sequence, sar::Flavor from,
                             const ntdlib::SndBuild& build)
{
    if (Fixed(sequence))
    {
        throw std::invalid_argument(
            "The commands in the sequence file must stay at their offsets. Hokuto can't put such a sequence into a "
            "Wii archive yet.");
    }

    // The commands that the build skips stay out. The others go in as NW4R SND reads them (AsWii), without the
    // encoder's choices of the other console. The biquad filters of the Switch take the Wii's nearest (MapBiquads).
    const song::Console reads{.engine = format::EngineOf(from)};
    const song::Console writes{.engine = format::Engine::kWii, .wii = build};
    WiiCommands commands = AsWii(WithoutSkipped(sequence, 0, reads, writes).first, from, build);
    const song::BiquadChanges biquads = song::MapBiquads(commands.sequence, {}, reads, writes);
    CommandsForWii result{
        .sequence =
            *format::WithoutChoices({format::Shared<ntdlib::commands::Sequence>(std::move(commands.sequence))}).front(),
        .warnings = {}};
    if (const std::optional<std::string> warning =
            SkippedWarning(CountSkipped(sequence, reads, writes), reads, writes, true))
    {
        result.warnings.push_back(*warning);
    }

    AddTimeWarnings(commands, result.warnings);
    if (const std::optional<std::string> warning = song::BiquadWarning(biquads, reads, writes))
    {
        result.warnings.push_back(*warning);
    }

    return result;
}

std::pair<uint32_t, std::vector<int>> WithLaterGlobals(ntdlib::commands::Sequence& sequence, uint32_t start)
{
    return WithStartingGlobals(sequence, start, 0, -1);
}

std::string LaterGlobalsWarning(std::span<const int> globals)
{
    return GlobalsWarning(globals, true);
}

namespace other_console_parts
{

// `sequence` encoded with `grammar`, and the offset of each of its labels. Throws std::invalid_argument with a message
// for the user when the encoder can't write the commands.
std::pair<std::vector<uint8_t>, std::vector<ntdlib::nwsnd::LabelOffset>> EncodedFile(
    const ntdlib::commands::Sequence& sequence, const ntdlib::nwsnd::Grammar& grammar, const std::string& console)
{
    ntdlib::nwsnd::Encoded encoded;
    try
    {
        encoded = ntdlib::nwsnd::EncodeSequence(sequence, grammar);
    }
    catch (const std::invalid_argument& error)
    {
        throw std::invalid_argument("The commands in the sequence file can't be encoded for the " + console + ". " +
                                    format::AsSentence(error.what()));
    }

    std::vector<ntdlib::nwsnd::LabelOffset> labels;
    for (const ntdlib::commands::Label& label : sequence.labels)
    {
        labels.push_back({label.name, encoded.offsets[label.item]});
    }

    return {std::move(encoded.bytes), std::move(labels)};
}

} // namespace other_console_parts

} // namespace hokuto::edit
