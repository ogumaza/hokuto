// SPDX-License-Identifier: MIT

// The text of the List Editor's cells: the names of the commands, their values and arguments in each unit, the calls of
// a subroutine, and which cells accept typed text.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/commands/units.h"

#include "app/key_edits.h"
#include "app/list_edits.h"
#include "app/list_edits_parts.h"
#include "app/time_text.h"
#include "app/transport.h"
#include "edit/bytecode.h"
#include "edit/commands.h"
#include "edit/notes.h"
#include "song/archive.h"
#include "song/konami.h"

namespace hokuto::app
{
namespace
{

using list_edits_parts::Addressed;
using list_edits_parts::AddressedBy;
using list_edits_parts::EntriesText;
using list_edits_parts::kBuses;
using list_edits_parts::kTracks;
using list_edits_parts::TicksText;
using list_edits_parts::UnitSuffix;
using list_edits_parts::ValueColumn;
using ntdlib::commands::Argument;
using ntdlib::commands::Command;
using ntdlib::commands::Kind;
using ntdlib::commands::kOne;
using ntdlib::commands::Source;
using ntdlib::commands::Unit;

// The names in the Type column, in the order of Kind. A variable's operations and comparisons name the operation. The
// first data value names the variable.
constexpr std::array<std::string_view, ntdlib::commands::kKindCount> kKindNames = {
    "Note",
    "Wait",
    "Program",
    "Open Track",
    "Jump",
    "Call",
    "Return",
    "Loop Start",
    "Loop End",
    "End",
    "Allocate Tracks",
    "Timebase",
    "Hold",
    "Monophonic",
    "Velocity Range",
    "Biquad Type",
    "Biquad Value",
    "Bank Select",
    "Front Bypass",
    "Pan",
    "Volume",
    "Main Volume",
    "Transpose",
    "Pitch Bend",
    "Bend Range",
    "Priority",
    "Note Wait",
    "Tie",
    "Portamento From",
    "Portamento",
    "Portamento Time",
    "Attack",
    "Decay",
    "Sustain",
    "Release",
    "Envelope Reset",
    "Volume 2",
    "Print Variable",
    "Surround Pan",
    "Low-Pass",
    "Send",
    "Main Send",
    "Initial Pan",
    "Mute",
    "Damper",
    "Tempo",
    "Sweep Pitch",
    "LFO Curve",
    "LFO Phase",
    "LFO Depth",
    "LFO Speed",
    "LFO Type",
    "LFO Range",
    "LFO Delay",
    "LFO Period",
    "Set",
    "Add",
    "Subtract",
    "Multiply",
    "Divide",
    "Shift",
    "Random",
    "And",
    "Or",
    "Xor",
    "Not",
    "Modulo",
    "Compare =",
    "Compare ≥",
    "Compare >",
    "Compare ≤",
    "Compare <",
    "Compare ≠",
    "User Procedure",
    "Note Off",
    "Repeat",
    "Jump If =",
    "Jump If ≠",
    "Jump If >",
    "Jump If ≥",
    "Jump If ≤",
    "Jump If <",
    "Tune",
    "Register",
    "Sample Wave",
    "Voice Type",
    "Echo Volume",
    "Echo Length",
    "PSG Length",
    "Sweep",
    "Sample Offset",
    "Other",
    "Data",
};
static_assert(kKindNames.back() == "Data", "each kind of command takes a name");

} // namespace

namespace list_edits_parts
{

Addressed AddressedBy(Kind kind)
{
    if (kind == Kind::kSend)
    {
        return Addressed::kBus;
    }
    if (kind >= Kind::kLfoCurve && kind <= Kind::kLfoPeriod)
    {
        return Addressed::kLfo;
    }
    if (kind >= Kind::kSetVariable && kind <= Kind::kCompareNotEqual)
    {
        return Addressed::kVariable;
    }

    return Addressed::kNothing;
}

std::optional<ListColumn> ValueColumn(Kind kind)
{
    switch (kind)
    {
    case Kind::kNote:
    case Kind::kWait:
    case Kind::kOpenTrack:
    case Kind::kJump:
    case Kind::kCall:
    case Kind::kReturn:
    case Kind::kLoopEnd:
    case Kind::kEnd:
    case Kind::kEnvelopeReset:
    case Kind::kOther:
    case Kind::kData:
        return std::nullopt;
    default:
        return AddressedBy(kind) == Addressed::kNothing ? ListColumn::kData1 : ListColumn::kData2;
    }
}

} // namespace list_edits_parts

namespace
{

// A fixed-point value as an exact decimal: 65,536 is 1, and 98,304 is 1.5.
std::string DecimalText(ntdlib::commands::Fixed value)
{
    const bool negative = value < 0;
    const uint64_t magnitude = negative ? 0 - static_cast<uint64_t>(value) : static_cast<uint64_t>(value);
    std::string text = (negative ? "-" : "") + std::to_string(magnitude / kOne);
    uint64_t fraction = magnitude % kOne;
    if (fraction != 0)
    {
        text += '.';
    }
    while (fraction != 0)
    {
        fraction *= 10;
        text += static_cast<char>('0' + fraction / kOne);
        fraction %= kOne;
    }

    return text;
}

} // namespace

namespace list_edits_parts
{

std::string_view UnitSuffix(std::optional<Unit> unit)
{
    switch (unit.value_or(Unit::kNumber))
    {
    case Unit::kCents:
        return " cents";
    case Unit::kBpm:
        return " BPM";
    case Unit::kMs:
        return " ms";
    case Unit::kHz:
        return " Hz";
    default:
        return "";
    }
}

std::string TicksText(ntdlib::commands::Fixed ticks, int ticks_per_quarter)
{
    return LengthText(static_cast<double>(ticks) / kOne / std::max(ticks_per_quarter, 1), kBeatsPerBar);
}

std::string_view UnitSuffix(const song::Console& console, Kind kind)
{
    if (console.engine == format::Engine::kGba && (kind == Kind::kLfoSpeed || kind == Kind::kLfoDelay))
    {
        return {};
    }

    return UnitSuffix(ntdlib::commands::UnitOf(kind));
}

} // namespace list_edits_parts

namespace
{

// A length argument: its length, a random range of lengths, or a variable.
std::string LengthArgumentText(const Argument& argument, int ticks_per_quarter)
{
    switch (argument.source)
    {
    case Source::kVariable:
        return "Variable " + std::to_string(argument.value);
    case Source::kRandom:
        return "Random " + WithoutFigureSpaces(TicksText(argument.value, ticks_per_quarter)) + " to " +
               WithoutFigureSpaces(TicksText(argument.high, ticks_per_quarter));
    case Source::kValue:
        break;
    }

    return TicksText(argument.value, ticks_per_quarter);
}

// The tracks of a mask of tracks, such as "0, 1, 2", or None.
std::string TracksText(int64_t mask)
{
    std::string text;
    for (int track = 0; track < kTracks; track++)
    {
        if ((mask & (int64_t{1} << track)) != 0)
        {
            text += (text.empty() ? "" : ", ") + std::to_string(track);
        }
    }

    return text.empty() ? "None" : text;
}

// The target of a jump, a call or a track start: the label at its item, the end, or its offset.
std::string TargetText(const ListTrack& track, const Command& command)
{
    if (command.target_outside)
    {
        return OffsetText(command.target);
    }

    const auto label = std::find_if(track.targets.begin(), track.targets.end(),
                                    [&command](const auto& target) { return target.second == command.target; });
    if (label != track.targets.end())
    {
        return label->first;
    }
    if (command.target + 1 >= track.offsets.size())
    {
        return "End";
    }

    return OffsetText(track.offsets[command.target]);
}

// The bytes of a data item in hexadecimal, up to eight of them.
std::string BytesText(const std::vector<uint8_t>& bytes)
{
    constexpr std::size_t kShown = 8;
    std::string text;
    for (std::size_t i = 0; i < std::min(bytes.size(), kShown); i++)
    {
        std::array<char, 4> byte{};
        std::snprintf(byte.data(), byte.size(), "%02X", bytes[i]);
        text += (i == 0 ? "" : " ") + std::string(byte.data());
    }

    return bytes.size() > kShown ? text + " …" : text;
}

// The first or second data value of `row`.
std::string DataText(const ListTrack& track, const ListRow& row, bool first)
{
    const Command& command = row.command;
    switch (command.kind)
    {
    case Kind::kNote:
        if (first && command.sample)
        {
            // On Konami's driver, a note names its sample and its semitones from the sample's pitch.
            const int semitones = static_cast<int8_t>(command.key);
            return "Sample " + std::to_string(*command.sample) +
                   (semitones == 0 ? "" : (semitones > 0 ? " +" : " ") + std::to_string(semitones));
        }
        if (first && song::KonamiRevisionOf(track.console.driver))
        {
            return "Note " + std::to_string(command.key);
        }
        if (first && row.played_key)
        {
            // On Quintet's driver, the key that plays: the command has only the pitch in the channel's octave.
            return KeyName(*row.played_key);
        }
        return first ? KeyName(command.key) : DecimalText(command.velocity);

    case Kind::kOpenTrack:
        return first ? "Track " + std::to_string(command.index) : TargetText(track, command);

    case Kind::kJump:
    case Kind::kCall:
        return first ? TargetText(track, command) : std::string();

    case Kind::kAllocateTracks:
        if (!first)
        {
            return {};
        }
        return command.value.source == Source::kValue ? TracksText(command.value.value / kOne)
                                                      : ArgumentText(track.console, command.kind, command.value);

    case Kind::kOther:
        {
            std::array<char, 16> text{};
            std::snprintf(text.data(), text.size(), "Opcode 0x%X", command.encoding.opcode);
            return first ? std::string(text.data()) : std::string();
        }

    case Kind::kData:
        return first ? BytesText(command.bytes) : std::string();

    default:
        break;
    }

    const std::optional<ListColumn> column = ValueColumn(command.kind);
    if (first && column == ListColumn::kData2)
    {
        switch (AddressedBy(command.kind))
        {
        case Addressed::kBus:
            return command.index < kBuses ? std::string(1, static_cast<char>('A' + command.index))
                                          : std::to_string(command.index);
        case Addressed::kLfo:
            return "LFO " + std::to_string(command.index);
        default:
            return "Variable " + std::to_string(command.index);
        }
    }
    if (column == (first ? ListColumn::kData1 : ListColumn::kData2))
    {
        return ArgumentText(track.console, command.kind, command.value);
    }

    return {};
}

} // namespace

bool Switches(Kind kind)
{
    switch (kind)
    {
    case Kind::kNoteWait:
    case Kind::kTie:
    case Kind::kMonophonic:
    case Kind::kDamper:
    case Kind::kPortamento:
    case Kind::kFrontBypass:
        return true;
    default:
        return false;
    }
}

std::string ValueText(const song::Console& console, Kind kind, ntdlib::commands::Fixed value)
{
    if (Switches(kind) && (value == 0 || value == kOne))
    {
        return value == 0 ? "Off" : "On";
    }

    return DecimalText(value) + std::string(UnitSuffix(console, kind));
}

std::string ArgumentText(const song::Console& console, Kind kind, const Argument& argument)
{
    switch (argument.source)
    {
    case Source::kVariable:
        return "Variable " + std::to_string(argument.value);
    case Source::kRandom:
        return "Random " + DecimalText(argument.value) + " to " + DecimalText(argument.high) +
               std::string(UnitSuffix(console, kind));
    case Source::kValue:
        break;
    }

    return ValueText(console, kind, argument.value);
}

std::string IgnoredOn(std::string_view engine)
{
    // The rest of the program names a console with its article: "on the GBA". Krawall and DSE are drivers.
    const bool driver = engine.ends_with("driver") || engine == "DSE" || engine == "Krawall";
    return (driver ? " by " : " on the ") + std::string(engine);
}

std::string CallsText(const ListSubroutine& subroutine)
{
    const auto count = [](int calls)
    {
        return calls == 1 ? std::string("1 call") : std::to_string(calls) + " calls";
    };

    const int elsewhere = subroutine.calls - subroutine.sound_calls;
    if (elsewhere <= 0)
    {
        return count(subroutine.sound_calls) + " in this sound";
    }

    return count(subroutine.sound_calls) + " in this sound, " + std::to_string(elsewhere) +
           " elsewhere in the sequence";
}

std::string CallsTooltip(const ListSubroutine& subroutine)
{
    const std::string until = subroutine.loops ? " up to the end of the first loop pass" : "";
    const int elsewhere = subroutine.calls - subroutine.sound_calls;
    if (elsewhere <= 0)
    {
        return "All calls to this subroutine in the sequence come from this sound. The CALL menu lists each call" +
               until + ".";
    }

    return "The sequence contains " +
           (subroutine.calls == 1 ? std::string("1 call") : std::to_string(subroutine.calls) + " calls") +
           " to this subroutine. This sound made " + std::to_string(subroutine.sound_calls) + " of them" + until +
           ". " +
           (elsewhere == 1 ? std::string("The other call is in another sound, or in a branch that this sound didn't "
                                         "take.")
                           : "The other " + std::to_string(elsewhere) +
                                 " are in other sounds, or in branches that this sound didn't take.");
}

std::string OffsetText(uint32_t offset)
{
    std::array<char, 16> text{};
    std::snprintf(text.data(), text.size(), "0x%X", offset);
    return text.data();
}

std::string_view KindName(Kind kind)
{
    const auto index = static_cast<std::size_t>(kind);
    return index < kKindNames.size() ? kKindNames[index] : "Other";
}

std::span<const KindGroup> InsertGroups()
{
    static const std::vector<KindGroup> kGroups = {
        {"Notes and Time", {Kind::kNote, Kind::kWait, Kind::kNoteWait, Kind::kTie, Kind::kTempo, Kind::kTimebase}},
        {"Mix",
         {Kind::kVolume, Kind::kVolume2, Kind::kMainVolume, Kind::kPan, Kind::kInitialPan, Kind::kSurroundPan,
          Kind::kSend, Kind::kMainSend, Kind::kFrontBypass, Kind::kMute}},
        {"Pitch",
         {Kind::kTranspose, Kind::kPitchBend, Kind::kBendRange, Kind::kSweepPitch, Kind::kPortamento,
          Kind::kPortamentoFrom, Kind::kPortamentoTime}},
        {"Instrument",
         {Kind::kProgram, Kind::kBankSelect, Kind::kPriority, Kind::kVelocityRange, Kind::kMonophonic, Kind::kDamper}},
        {"Envelope",
         {Kind::kAttack, Kind::kDecay, Kind::kSustain, Kind::kRelease, Kind::kEnvelopeHold, Kind::kEnvelopeReset}},
        {"Filter", {Kind::kLowPass, Kind::kBiquadType, Kind::kBiquadValue}},
        {"LFO",
         {Kind::kLfoDepth, Kind::kLfoSpeed, Kind::kLfoRange, Kind::kLfoType, Kind::kLfoDelay, Kind::kLfoCurve,
          Kind::kLfoPhase, Kind::kLfoPeriod}},
        {"Flow",
         {Kind::kJump, Kind::kCall, Kind::kReturn, Kind::kLoopStart, Kind::kLoopEnd, Kind::kEnd, Kind::kOpenTrack,
          Kind::kAllocateTracks}},
        {"Variables",
         {Kind::kSetVariable, Kind::kAddVariable, Kind::kSubtractVariable, Kind::kMultiplyVariable,
          Kind::kDivideVariable, Kind::kModuloVariable, Kind::kShiftVariable, Kind::kAndVariable, Kind::kOrVariable,
          Kind::kXorVariable, Kind::kNotVariable, Kind::kRandomVariable}},
        {"Comparisons",
         {Kind::kCompareEqual, Kind::kCompareNotEqual, Kind::kCompareGreater, Kind::kCompareGreaterOrEqual,
          Kind::kCompareLess, Kind::kCompareLessOrEqual}},
        {"Game", {Kind::kUserProcedure, Kind::kPrintVariable}},
    };
    return kGroups;
}

Kind InsertKindOn(const song::Console& console, Kind kind)
{
    if (song::Runs(console, kind, 0))
    {
        return kind;
    }
    if (song::Runs(console, Kind::kVolume, 0))
    {
        return Kind::kVolume;
    }

    // A driver without a volume, such as the Ubisoft Milan driver, uses the first kind of the menu that it runs.
    const auto runs = [&console](Kind choice)
    {
        return song::Runs(console, choice, 0);
    };
    for (const KindGroup& group : InsertGroups())
    {
        const auto found = std::ranges::find_if(group.kinds, runs);
        if (found != group.kinds.end())
        {
            return *found;
        }
    }

    return kind;
}

std::string CellText(const ListTrack& track, const ListRow& row, ListColumn column)
{
    // A label's row names the label, and counts what goes to it. Its start is its command's.
    if (IsLabel(row) && column != ListColumn::kStart)
    {
        const std::optional<uint32_t> item = edit::ItemAtOffset(track.offsets, row.row.command);
        switch (column)
        {
        case ListColumn::kType:
            return "Label";
        case ListColumn::kData1:
            return row.label;
        case ListColumn::kComment:
            return item ? EntriesText(track, *item) : std::string();
        default:
            return {};
        }
    }

    const Command& command = row.command;
    switch (column)
    {
    case ListColumn::kType:
        return std::string(KindName(command.kind));

    case ListColumn::kStart:
        return row.row.tick ? PositionText(static_cast<double>(*row.row.tick) / std::max(track.ticks_per_quarter, 1),
                                           kBeatsPerBar)
                            : std::string();

    case ListColumn::kLength:
        if (command.kind == Kind::kNote || command.kind == Kind::kWait)
        {
            return LengthArgumentText(command.value, track.ticks_per_quarter);
        }
        return command.time ? LengthArgumentText(*command.time, track.ticks_per_quarter) : std::string();

    case ListColumn::kData1:
    case ListColumn::kData2:
        return DataText(track, row, column == ListColumn::kData1);

    case ListColumn::kComment:
        break;
    }

    // The comment of a command without a label counts what goes to it. A label's row counts the same. Otherwise the
    // comment names the command's labels until they have rows.
    const std::optional<uint32_t> item = edit::ItemAtOffset(track.offsets, row.row.command);
    if (item && !track.labels.contains(*item))
    {
        return EntriesText(track, *item);
    }
    const auto labels = item && !track.label_rows ? track.labels.find(*item) : track.labels.end();
    return labels != track.labels.end() ? labels->second : std::string();
}

bool Typable(const ListTrack& track, const ListRow& row, ListColumn column)
{
    if (IsLabel(row))
    {
        return column == ListColumn::kData1 && !track.timeline;
    }

    const Kind kind = row.command.kind;
    switch (column)
    {
    case ListColumn::kStart:
        return (kind == Kind::kNote || !edit::WhyFixedInTime(row.command)) && row.row.tick;

    case ListColumn::kLength:
        return kind == Kind::kNote || kind == Kind::kWait ||
               edit::ArgumentRange(track.console, track.number, row.command, Source::kValue, true).has_value();

    case ListColumn::kData1:
        return kind == Kind::kNote || kind == Kind::kOpenTrack || kind == Kind::kAllocateTracks ||
               ValueColumn(kind).has_value();

    case ListColumn::kData2:
        return kind == Kind::kNote || ValueColumn(kind) == ListColumn::kData2;

    default:
        return false;
    }
}

bool NamesTarget(const ListRow& row, ListColumn column)
{
    if (IsLabel(row))
    {
        return false;
    }

    const Kind kind = row.command.kind;
    return ((kind == Kind::kJump || kind == Kind::kCall) && column == ListColumn::kData1) ||
           (kind == Kind::kOpenTrack && column == ListColumn::kData2);
}

} // namespace hokuto::app
