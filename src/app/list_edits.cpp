// SPDX-License-Identifier: MIT

#include "app/list_edits.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/commands/units.h"

#include "app/key_edits.h"
#include "app/transport.h"
#include "edit/bytecode.h"
#include "edit/notes.h"
#include "edit/subroutines.h"
#include "edit/timelines.h"

namespace hokuto::app
{
namespace
{

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
    "Other",
    "Data",
};

// The number of the LFOs, the buses that a track sends to, and the tracks.
constexpr int kLfos = 4;
constexpr int kBuses = 3;
constexpr int kTracks = 16;

// The highest variable that a command can name: a byte.
constexpr int kTopVariable = 255;

// The most digits after a decimal point that a typed number keeps.
constexpr int kFractionDigits = 12;

// What the first data value of a command names.
enum class Addressed
{
    kNothing,
    kBus,
    kLfo,
    kVariable,
};

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

// The column that holds the value of a command of `kind`, or nothing for a kind whose value shows elsewhere or not at
// all.
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

// `text` without spaces at either end.
std::string_view Trimmed(std::string_view text)
{
    while (!text.empty() && text.front() == ' ')
    {
        text.remove_prefix(1);
    }
    while (!text.empty() && text.back() == ' ')
    {
        text.remove_suffix(1);
    }

    return text;
}

// `text` in lower case, for ASCII letters.
std::string Lower(std::string_view text)
{
    std::string lower(text);
    for (char& c : lower)
    {
        if (c >= 'A' && c <= 'Z')
        {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }

    return lower;
}

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

// The suffix of a value of `unit`, such as " cents", or nothing.
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

// A length of `ticks` in the display's form, at `ticks_per_quarter`.
std::string TicksText(ntdlib::commands::Fixed ticks, int ticks_per_quarter)
{
    return LengthText(static_cast<double>(ticks) / kOne / std::max(ticks_per_quarter, 1), kBeatsPerBar);
}

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
    std::array<char, 24> text{};
    if (command.target_outside)
    {
        std::snprintf(text.data(), text.size(), "0x%X", command.target);
        return text.data();
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

    std::snprintf(text.data(), text.size(), "0x%X", track.offsets[command.target]);
    return text.data();
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
                                                      : ArgumentText(command.kind, command.value);

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
        return ArgumentText(command.kind, command.value);
    }

    return {};
}

// A number that the user typed, in fixed point: an optional sign, digits, and a decimal point with more digits.
std::optional<ntdlib::commands::Fixed> ParseFixed(std::string_view text)
{
    text = Trimmed(text);
    const bool negative = !text.empty() && (text.front() == '-' || text.front() == '+');
    const bool minus = negative && text.front() == '-';
    if (negative)
    {
        text.remove_prefix(1);
    }

    const std::size_t point = text.find('.');
    const std::string_view whole = text.substr(0, point);
    const std::string_view fraction = point == std::string_view::npos ? std::string_view() : text.substr(point + 1);

    const auto digits = [](std::string_view part)
    {
        return std::all_of(part.begin(), part.end(), [](char c) { return c >= '0' && c <= '9'; });
    };
    if ((whole.empty() && fraction.empty()) || !digits(whole) || !digits(fraction) || whole.size() > 12)
    {
        return std::nullopt;
    }

    int64_t value = 0;
    if (!whole.empty())
    {
        std::from_chars(whole.data(), whole.data() + whole.size(), value);
    }
    value *= kOne;

    // The fraction rounds to the nearest 65,536th.
    int64_t numerator = 0;
    int64_t denominator = 1;
    for (std::size_t i = 0; i < fraction.size() && i < kFractionDigits; i++)
    {
        numerator = numerator * 10 + (fraction[i] - '0');
        denominator *= 10;
    }
    value += (numerator * kOne + denominator / 2) / denominator;

    return minus ? -value : value;
}

// A number that the user typed for a value of `kind`, with or without the unit's suffix.
std::optional<ntdlib::commands::Fixed> ParseValue(Kind kind, std::string_view text)
{
    const std::string lower = Lower(Trimmed(text));
    if (Switches(kind) && (lower == "on" || lower == "off"))
    {
        return lower == "on" ? kOne : 0;
    }

    std::string_view number = lower;
    const std::string suffix = Lower(Trimmed(UnitSuffix(ntdlib::commands::UnitOf(kind))));
    if (!suffix.empty() && number.ends_with(suffix))
    {
        number.remove_suffix(suffix.size());
    }

    return ParseFixed(number);
}

// A whole number from `text`, or nothing.
std::optional<int64_t> ParseWhole(std::string_view text)
{
    text = Trimmed(text);
    int64_t value = 0;
    const char* end = text.data() + text.size();
    if (text.empty() || std::from_chars(text.data(), end, value).ptr != end)
    {
        return std::nullopt;
    }

    return value;
}

// `text` after `word` and the spaces after it, when it starts with the word. The case of ASCII letters doesn't count.
std::optional<std::string_view> After(std::string_view text, std::string_view word)
{
    text = Trimmed(text);
    if (text.size() < word.size() || Lower(text.substr(0, word.size())) != word)
    {
        return std::nullopt;
    }

    return Trimmed(text.substr(word.size()));
}

// An argument that the user typed: "Variable 3" (or "var 3"), "Random 0 to 127" (or "random(0, 127)"), or a value that
// `parse` reads.
template <typename Parse>
std::optional<Argument> ParseArgument(std::string_view text, Parse parse)
{
    for (const std::string_view word : {"variable", "var"})
    {
        if (const std::optional<std::string_view> rest = After(text, word))
        {
            std::string_view number = *rest;
            if (number.starts_with('(') && number.ends_with(')'))
            {
                number = number.substr(1, number.size() - 2);
            }

            const std::optional<int64_t> variable = ParseWhole(number);
            return variable ? std::optional<Argument>(Argument{Source::kVariable, *variable, 0}) : std::nullopt;
        }
    }

    // A random range's ends lie either side of "to" or a comma, in brackets or not.
    if (const std::optional<std::string_view> rest = After(text, "random"))
    {
        std::string_view range = *rest;
        if (range.starts_with('(') && range.ends_with(')'))
        {
            range = range.substr(1, range.size() - 2);
        }

        const std::string lower = Lower(range);
        std::size_t split = lower.find(" to ");
        std::size_t width = 4;
        if (split == std::string::npos)
        {
            split = lower.find(',');
            width = 1;
        }
        if (split == std::string::npos)
        {
            return std::nullopt;
        }

        const std::optional<int64_t> low = parse(range.substr(0, split));
        const std::optional<int64_t> high = parse(range.substr(split + width));
        if (!low || !high)
        {
            return std::nullopt;
        }

        return Argument{Source::kRandom, std::min(*low, *high), std::max(*low, *high)};
    }

    const std::optional<int64_t> value = parse(text);
    return value ? std::optional<Argument>(Argument{Source::kValue, *value, 0}) : std::nullopt;
}

// A length that the user typed in the display's form, in fixed-point ticks at `ticks_per_quarter`.
std::optional<int64_t> ParseTicks(std::string_view text, int ticks_per_quarter)
{
    const std::optional<double> quarters = ParseLength(text, kBeatsPerBar);
    if (!quarters)
    {
        return std::nullopt;
    }

    return std::llround(*quarters * std::max(ticks_per_quarter, 1)) * kOne;
}

// The name that a refusal gives the argument of a command of `kind`.
std::string ArgumentName(Kind kind, bool time)
{
    if (time)
    {
        return "The time of " + std::string(KindName(kind));
    }
    if (kind == Kind::kNote)
    {
        return "A note's length";
    }

    return std::string(KindName(kind));
}

// Why `command` can't take `argument` in its value, or with `time` in its time, on the console of `track`. Nothing when
// the console's bytes hold it.
std::optional<std::string> Refusal(const ListTrack& track, const Command& command, const Argument& argument, bool time)
{
    const std::string name = ArgumentName(command.kind, time);
    const std::optional<edit::ValueRange> range = edit::ArgumentRange(track.console, command, argument.source, time);
    if (!range)
    {
        if (argument.source == Source::kRandom)
        {
            return name + " can't take a random value.";
        }
        if (argument.source == Source::kVariable)
        {
            return name + " can't take a variable's value.";
        }

        return name + " can't take a value here.";
    }

    const auto inside = [&range](int64_t value)
    {
        return value >= range->lowest && value <= range->highest;
    };
    if (inside(argument.value) && (argument.source != Source::kRandom || inside(argument.high)))
    {
        return std::nullopt;
    }

    // The range in the form that the column shows.
    const bool ticks = time || command.kind == Kind::kNote || command.kind == Kind::kWait;
    const auto shown = [&](int64_t value)
    {
        if (argument.source == Source::kVariable)
        {
            return std::to_string(value);
        }

        return ticks ? WithoutFigureSpaces(TicksText(value, track.ticks_per_quarter)) : ValueText(command.kind, value);
    };
    if (argument.source == Source::kVariable)
    {
        return "A variable's number runs from " + shown(range->lowest) + " to " + shown(range->highest) + ".";
    }

    return name + " runs from " + shown(range->lowest) + " to " + shown(range->highest) + ".";
}

// The edit that gives row `index` the command `command`, or the refusal of its argument.
ListEdit ChangeTo(std::size_t index, std::string name, Command command,
                  std::optional<std::string> refused = std::nullopt)
{
    ListEdit edit;
    edit.action = ListEdit::Action::kChange;
    edit.name = std::move(name);
    edit.rows = {index};
    edit.command = std::move(command);
    edit.refused = refused.value_or("");

    return edit;
}

// The edit of a typed length: a note's length, a wait's length or a setting's time. An empty time takes the time away.
std::optional<ListEdit> TypedLength(const ListTrack& track, std::size_t index, std::string_view text)
{
    const ListRow& row = track.rows[index];
    Command command = row.command;
    const bool time = command.kind != Kind::kNote && command.kind != Kind::kWait;
    if (time && Trimmed(text).empty())
    {
        if (!command.time)
        {
            return std::nullopt;
        }

        command.time.reset();
        return ChangeTo(index, "Change Command", std::move(command));
    }

    const auto ticks = [&track](std::string_view part)
    {
        return ParseTicks(part, track.ticks_per_quarter);
    };
    const std::optional<Argument> argument = ParseArgument(text, ticks);
    if (!argument)
    {
        return std::nullopt;
    }

    // A note's length makes a Size, and a wait's length, its value, a Set Main Value.
    (time ? command.time.emplace() : command.value) = *argument;
    const std::optional<std::string> refused = Refusal(track, command, *argument, time);
    const char* name = command.kind == Kind::kNote ? "Size" : (time ? "Change Command" : "Set Main Value");
    return ChangeTo(index, name, std::move(command), refused);
}

// The edit of what a command addresses, typed as a number with or without its name: a bus as A, B or C, an LFO, a
// variable or a track.
std::optional<ListEdit> TypedIndex(const ListTrack& track, std::size_t index, std::string_view text)
{
    Command command = track.rows[index].command;
    std::string_view number = Trimmed(text);
    int top = kTopVariable;
    std::string what = "A variable's number";
    if (command.kind == Kind::kOpenTrack)
    {
        number = After(number, "track").value_or(number);
        top = kTracks - 1;
        what = "A track";
    }
    else if (AddressedBy(command.kind) == Addressed::kBus)
    {
        const std::string bus = Lower(After(number, "fx").value_or(number));
        if (bus.size() != 1 || bus[0] < 'a' || bus[0] >= 'a' + kBuses)
        {
            return std::nullopt;
        }

        command.index = static_cast<uint8_t>(bus[0] - 'a');
        return command.index == track.rows[index].command.index
                   ? std::nullopt
                   : std::optional<ListEdit>(ChangeTo(index, "Change Command", std::move(command)));
    }
    else if (AddressedBy(command.kind) == Addressed::kLfo)
    {
        number = After(number, "lfo").value_or(number);
        top = kLfos - 1;
        what = "An LFO";
    }
    else
    {
        number = After(number, "variable").value_or(After(number, "var").value_or(number));
    }

    const std::optional<int64_t> value = ParseWhole(number);
    if (!value || *value == command.index)
    {
        return std::nullopt;
    }

    command.index = static_cast<uint8_t>(std::clamp<int64_t>(*value, 0, 255));
    std::optional<std::string> refused;
    if (*value < 0 || *value > top)
    {
        refused = what + " runs from 0 to " + std::to_string(top) + ".";
    }

    return ChangeTo(index, "Change Command", std::move(command), refused);
}

// The edit of a typed list of tracks for Allocate Tracks, such as "0, 1, 2".
std::optional<ListEdit> TypedTracks(const ListTrack& track, std::size_t index, std::string_view text)
{
    int64_t mask = 0;
    std::string_view rest = Trimmed(text);
    if (Lower(rest) != "none")
    {
        while (!rest.empty())
        {
            const std::size_t comma = rest.find(',');
            const std::optional<int64_t> number = ParseWhole(rest.substr(0, comma));
            if (!number || *number < 0 || *number >= kTracks)
            {
                return std::nullopt;
            }

            mask |= int64_t{1} << *number;
            rest = comma == std::string_view::npos ? std::string_view() : rest.substr(comma + 1);
        }
    }

    Command command = track.rows[index].command;
    command.value = Argument{Source::kValue, mask * kOne, 0};
    if (command.value == track.rows[index].command.value)
    {
        return std::nullopt;
    }

    return ChangeTo(index, "Set Main Value", std::move(command));
}

// The edit of a typed value in the column of the command's value.
std::optional<ListEdit> TypedValue(const ListTrack& track, std::size_t index, std::string_view text)
{
    Command command = track.rows[index].command;
    const auto value = [kind = command.kind](std::string_view part)
    {
        return ParseValue(kind, part);
    };
    const std::optional<Argument> argument = ParseArgument(text, value);
    if (!argument)
    {
        return std::nullopt;
    }

    command.value = *argument;
    const std::optional<std::string> refused = Refusal(track, command, *argument, false);
    return ChangeTo(index, "Set Main Value", std::move(command), refused);
}

// The edit of a typed pitch or velocity of a note.
std::optional<ListEdit> TypedNote(const ListTrack& track, std::size_t index, std::string_view text, bool pitch)
{
    Command command = track.rows[index].command;
    if (pitch)
    {
        std::optional<int> key = ParseKeyName(text);
        if (!key)
        {
            const std::optional<int64_t> number = ParseWhole(text);
            key = number && *number >= 0 && *number < kKeys ? std::optional<int>(static_cast<int>(*number))
                                                            : std::nullopt;
        }
        if (!key)
        {
            const std::optional<int64_t> number = ParseWhole(text);
            return number ? std::optional<ListEdit>(ChangeTo(index, "Transpose", std::move(command),
                                                             "A key runs from 0 to 127, C-2 to G8."))
                          : std::nullopt;
        }

        command.key = static_cast<uint8_t>(*key);
        return ChangeTo(index, "Transpose", std::move(command));
    }

    const std::optional<int64_t> velocity = ParseWhole(text);
    if (!velocity)
    {
        return std::nullopt;
    }
    if (*velocity < 0 || *velocity > kTopVelocity)
    {
        return ChangeTo(index, "Change Velocity", std::move(command), "A velocity runs from 0 to 127.");
    }

    command.velocity = *velocity * kOne;
    return ChangeTo(index, "Change Velocity", std::move(command));
}

// What a cell of `column` takes for a command of `kind`, for the refusal of text that it can't read.
std::string TypingHint(Kind kind, ListColumn column)
{
    switch (column)
    {
    case ListColumn::kStart:
        return PositionHint("A start");

    case ListColumn::kLength:
        return LengthHint() + " It can also be \"random 0 to 48\" or \"variable 3\".";

    case ListColumn::kData1:
        if (kind == Kind::kNote)
        {
            return "A key is a number from 0 to 127 or a name, such as C3 for middle C.";
        }
        if (kind == Kind::kAllocateTracks)
        {
            return "The tracks are numbers from 0 to 15 with commas between them, such as 0, 1, 2, or none.";
        }
        if (kind == Kind::kOpenTrack)
        {
            return "A track is a number from 0 to 15.";
        }
        if (ValueColumn(kind) == ListColumn::kData2)
        {
            return AddressedBy(kind) == Addressed::kBus   ? "A bus is A, B or C."
                   : AddressedBy(kind) == Addressed::kLfo ? "An LFO is a number from 0 to 3."
                                                          : "A variable is a number, such as 3 or variable 3.";
        }
        break;

    case ListColumn::kData2:
        if (kind == Kind::kNote)
        {
            return "A velocity is a number from 0 to 127.";
        }
        break;

    default:
        break;
    }

    return ValueHint();
}

// The edit of TypedListEdit, before the check that it changes the command.
std::optional<ListEdit> TypedIn(const ListTrack& track, std::size_t index, ListColumn column, std::string_view text)
{
    const ListRow& row = track.rows[index];
    const Kind kind = row.command.kind;
    switch (column)
    {
    case ListColumn::kStart:
        {
            // A note moves the way that the Key Editor moves it, and a setting the way that a controller lane moves it.
            const std::optional<double> quarters = ParsePosition(text, kBeatsPerBar);
            if (!quarters)
            {
                return std::nullopt;
            }

            const int64_t tick = std::llround(*quarters * std::max(track.ticks_per_quarter, 1));
            ListEdit edit;
            edit.action = ListEdit::Action::kMove;
            edit.name = "Move";
            edit.rows = {index};
            edit.command = row.command;
            edit.shift = static_cast<int32_t>(tick - int64_t{*row.row.tick});
            return edit.shift != 0 ? std::optional<ListEdit>(std::move(edit)) : std::nullopt;
        }

    case ListColumn::kLength:
        return TypedLength(track, index, text);

    case ListColumn::kData1:
        if (kind == Kind::kNote)
        {
            return TypedNote(track, index, text, true);
        }
        if (kind == Kind::kAllocateTracks)
        {
            return TypedTracks(track, index, text);
        }
        if (kind == Kind::kOpenTrack || ValueColumn(kind) == ListColumn::kData2)
        {
            return TypedIndex(track, index, text);
        }
        return TypedValue(track, index, text);

    case ListColumn::kData2:
        return kind == Kind::kNote ? TypedNote(track, index, text, false) : TypedValue(track, index, text);

    default:
        return std::nullopt;
    }
}

// What the List Editor shows of track `number` of `song` before the sequence's commands: its number, its name and its
// stretches, the timebase, every track that ran commands, with notes or without, for the menu, and where the calls of
// every track started runs.
ListTrack TrackOf(const song::Sequence& song, int number)
{
    ListTrack track;
    track.number = number;
    track.name = "Track " + std::to_string(number);
    track.ticks_per_quarter = song.timebase;
    for (const song::Track& played : song.tracks)
    {
        track.tracks.push_back(played.number);
        if (played.number == number)
        {
            track.name = played.name;
            track.stretches = played.stretches;
        }
        for (const song::Stretch& stretch : played.stretches)
        {
            if (stretch.entry == ntdlib::sequence_export::Entry::kCall)
            {
                track.runs.insert(stretch.first);
            }
        }
    }

    return track;
}

// Gives `track` the console of `file`, the offsets of `commands`, the labels at each item and what goes to each.
void AddSequence(ListTrack& track, const format::File& file, const ntdlib::commands::Sequence& commands)
{
    track.console = edit::ConsoleOf(file);
    track.offsets = edit::Offsets(commands, track.console);
    for (const ntdlib::commands::Label& label : commands.labels)
    {
        std::string& names = track.labels[label.item];
        names += (names.empty() ? "" : ", ") + label.name;
        track.targets.emplace_back(label.name, label.item);
    }
    for (const Command& command : commands.items)
    {
        if (!ntdlib::commands::HasTarget(command.kind) || command.target_outside)
        {
            continue;
        }

        Entries& entries = track.entries[command.target];
        (command.kind == Kind::kCall ? entries.calls : command.kind == Kind::kJump ? entries.jumps : entries.starts)++;
    }
}

// Gives `track` its rows of `commands` in the order that `song` ran them, with `track`'s offsets. The track entered the
// sequence at each stretch's first command.
void AddRows(ListTrack& track, const ntdlib::commands::Sequence& commands, const song::Sequence& song)
{
    std::span<const song::Note> notes;
    for (const song::Track& played : song.tracks)
    {
        if (played.number == track.number)
        {
            notes = played.notes;
        }
    }

    // A row takes the depth of its stretch. Within the stretch, a loop's start puts the loop on the stack, and the
    // loop's end takes it off where the track runs on.
    const std::optional<uint32_t> end = song.loop ? std::optional<uint32_t>(song.loop->end) : std::nullopt;
    const std::vector<edit::CommandRow> rows = edit::CommandRows(commands, track.offsets, track.stretches, notes, end);
    int depth = 0;
    for (std::size_t i = 0; i < rows.size(); i++)
    {
        const uint32_t item = *edit::ItemAtOffset(track.offsets, rows[i].command);
        ListRow& row = track.rows.emplace_back();
        row.row = rows[i];
        row.command = commands.items.at(item);
        row.entered = i == 0 || rows[i - 1].stretch != rows[i].stretch;
        depth = row.entered ? track.stretches[rows[i].stretch].depth : depth;
        row.depth = depth;
        if (!row.command.conditional && row.command.kind == Kind::kLoopStart)
        {
            depth++;
        }
        else if (!row.command.conditional && row.command.kind == Kind::kLoopEnd && depth > 0)
        {
            depth--;
        }
    }
}

// The comment of a label's row: what goes to the label's item, such as "2 calls, 1 jump".
std::string EntriesText(const ListTrack& track, uint32_t item)
{
    const auto found = track.entries.find(item);
    if (found == track.entries.end())
    {
        return {};
    }

    std::string text;
    const auto add = [&text](int count, std::string_view one, std::string_view many)
    {
        if (count > 0)
        {
            text += (text.empty() ? "" : ", ") + std::to_string(count) + " " + std::string(count == 1 ? one : many);
        }
    };
    add(found->second.calls, "call", "calls");
    add(found->second.jumps, "jump", "jumps");
    add(found->second.starts, "track start", "track starts");

    return text;
}

// The labels of `track`'s sequence, without its commands, for edit::LabelRefusal and edit::UniqueLabel.
ntdlib::commands::Sequence LabelNames(const ListTrack& track)
{
    ntdlib::commands::Sequence names;
    for (const auto& [name, item] : track.targets)
    {
        names.labels.push_back({name, item});
    }

    return names;
}

// The item of the command of row `index`, or nothing for a command that the sequence's offsets don't hold.
std::optional<uint32_t> ItemOfRow(const ListTrack& track, std::size_t index)
{
    return index < track.rows.size() ? edit::ItemAtOffset(track.offsets, track.rows[index].row.command) : std::nullopt;
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

std::string ValueText(Kind kind, ntdlib::commands::Fixed value)
{
    if (Switches(kind) && (value == 0 || value == kOne))
    {
        return value == 0 ? "Off" : "On";
    }

    return DecimalText(value) + std::string(UnitSuffix(ntdlib::commands::UnitOf(kind)));
}

std::string ArgumentText(Kind kind, const Argument& argument)
{
    switch (argument.source)
    {
    case Source::kVariable:
        return "Variable " + std::to_string(argument.value);
    case Source::kRandom:
        return "Random " + DecimalText(argument.value) + " to " + DecimalText(argument.high) +
               std::string(UnitSuffix(ntdlib::commands::UnitOf(kind)));
    case Source::kValue:
        break;
    }

    return ValueText(kind, argument.value);
}

ListTrack MakeListTrack(const format::File& file, uint32_t sequence, const song::Sequence& song, int number)
{
    ListTrack track = TrackOf(song, number);
    try
    {
        const ntdlib::commands::Sequence& commands = edit::SequenceToEdit(file, sequence);
        AddSequence(track, file, commands);
        AddRows(track, commands, song);
        track.timeline = edit::IsTimeline(file, sequence);
    }
    catch (const std::exception& error)
    {
        track.rows.clear();
        track.unavailable = error.what();
    }

    return track;
}

std::vector<ListTrack> MakeListTracks(const format::File& file, uint32_t sequence, const song::Sequence& song)
{
    std::vector<ListTrack> tracks;
    for (const song::Track& played : song.tracks)
    {
        tracks.push_back(TrackOf(song, played.number));
    }

    // The offsets and the labels, once for every track.
    const ntdlib::commands::Sequence* commands = nullptr;
    ListTrack shared;
    try
    {
        commands = &edit::SequenceToEdit(file, sequence);
        AddSequence(shared, file, *commands);
    }
    catch (const std::exception& error)
    {
        for (ListTrack& track : tracks)
        {
            track.unavailable = error.what();
        }
        return tracks;
    }

    for (ListTrack& track : tracks)
    {
        track.console = shared.console;
        track.offsets = shared.offsets;
        track.labels = shared.labels;
        track.targets = shared.targets;
        track.entries = shared.entries;
        track.timeline = edit::IsTimeline(file, sequence);
        try
        {
            AddRows(track, *commands, song);
        }
        catch (const std::exception& error)
        {
            track.rows.clear();
            track.unavailable = error.what();
        }
    }

    return tracks;
}

ListTrack WithLabelRows(ListTrack track)
{
    std::vector<ListRow> rows;
    for (ListRow& row : track.rows)
    {
        const std::optional<uint32_t> item = edit::ItemAtOffset(track.offsets, row.row.command);
        for (const auto& [name, at] : track.targets)
        {
            if (item == at)
            {
                ListRow& label = rows.emplace_back(row);
                label.label = name;
            }
        }
        rows.push_back(std::move(row));
    }
    track.rows = std::move(rows);
    track.label_rows = true;

    return track;
}

bool Skipped(const ListTrack& track, const ListRow& row)
{
    return !IsLabel(row) && (!song::Runs(track.console, row.command.kind, row.command.index) ||
                             (track.engine && !song::Runs(*track.engine, row.command.kind, row.command.index)));
}

const std::string& SkippingEngine(const ListTrack& track, const ListRow& row)
{
    return track.engine && song::Runs(track.console, row.command.kind, row.command.index) ? track.engine_name
                                                                                          : track.file_engine_name;
}

std::vector<ListCall> SubroutineRuns(const song::Sequence& song, uint32_t first)
{
    // The rows stop at the end of the loop's first pass (MakeListTrack). So do the runs.
    std::vector<ListCall> runs;
    for (const song::Track& played : song.tracks)
    {
        for (uint32_t i = 0; i < played.stretches.size(); i++)
        {
            const song::Stretch& stretch = played.stretches[i];
            if (song.loop && stretch.tick >= song.loop->end)
            {
                break;
            }
            if (stretch.entry == ntdlib::sequence_export::Entry::kCall && stretch.first == first)
            {
                runs.push_back({played.number, i, stretch.tick});
            }
        }
    }

    return runs;
}

std::optional<std::size_t> RowOfPart(const ListTrack& track, const song::Track& played, std::size_t first,
                                     int32_t start)
{
    // The call that started a run ends the stretch before it.
    if (first < played.notes.size())
    {
        const song::Note& note = played.notes[first];
        const std::optional<std::size_t> call = song::OutermostCall(played, note.stretch);
        for (std::size_t i = 0; i < track.rows.size(); i++)
        {
            const ListRow& row = track.rows[i];
            const bool found = call ? row.command.kind == Kind::kCall && row.row.stretch + 1 == *call
                                    : row.row.command == note.command && row.row.stretch == note.stretch;
            if (!IsLabel(row) && found)
            {
                return i;
            }
        }
    }
    for (std::size_t i = 0; i < track.rows.size(); i++)
    {
        const std::optional<uint32_t> tick = track.rows[i].row.tick;
        if (!IsLabel(track.rows[i]) && tick && static_cast<int64_t>(*tick) >= start)
        {
            return i;
        }
    }

    return std::nullopt;
}

int CallsOfRuns(const song::Sequence& song, std::span<const ListCall> runs)
{
    // The call that started a run ends the stretch before it.
    std::set<uint32_t> calls;
    for (const ListCall& run : runs)
    {
        const auto played = std::find_if(song.tracks.begin(), song.tracks.end(),
                                         [&run](const song::Track& track) { return track.number == run.track; });
        if (played != song.tracks.end() && run.stretch > 0 && run.stretch <= played->stretches.size())
        {
            calls.insert(played->stretches[run.stretch - 1].last);
        }
    }

    return static_cast<int>(calls.size());
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
    const std::string until = subroutine.loops ? " up to the end of the loop's first pass" : "";
    const int elsewhere = subroutine.calls - subroutine.sound_calls;
    if (elsewhere <= 0)
    {
        return std::string("The shown sound runs every call of the subroutine that the sequence holds. ") +
               "The CALL menu lists each run" + until + ".";
    }

    return "The sequence holds " + std::to_string(subroutine.calls) + " calls of the subroutine. The shown sound ran " +
           std::to_string(subroutine.sound_calls) + " of them" + until + ". The other " +
           (elsewhere == 1 ? std::string("call lies") : std::to_string(elsewhere) + " lie") +
           " in the commands of other sounds, or in branches that this sound didn't take.";
}

ListTrack WithSubroutine(ListTrack track, ListSubroutine subroutine)
{
    // While the call stays on the stack, the run takes the stretches from the one that the call started.
    const ListCall* run = subroutine.shown < subroutine.runs.size() ? &subroutine.runs[subroutine.shown] : nullptr;
    track.subroutine = std::move(subroutine);
    if (run == nullptr || run->track != track.number || run->stretch >= track.stretches.size())
    {
        track.rows.clear();
        return track;
    }

    const int depth = track.stretches[run->stretch].depth;
    uint32_t end = run->stretch + 1;
    while (end < track.stretches.size() && track.stretches[end].depth >= depth)
    {
        end++;
    }
    std::erase_if(track.rows,
                  [&](const ListRow& row) { return row.row.stretch < run->stretch || row.row.stretch >= end; });
    for (ListRow& row : track.rows)
    {
        row.depth = std::max(row.depth - depth, 0);
    }

    return track;
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
               edit::ArgumentRange(track.console, row.command, Source::kValue, true).has_value();

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

std::optional<ListEdit> TypedListEdit(const ListTrack& track, std::size_t index, ListColumn column,
                                      std::string_view text)
{
    if (index >= track.rows.size() || !Typable(track, track.rows[index], column) ||
        Trimmed(text) == Trimmed(CellText(track, track.rows[index], column)))
    {
        return std::nullopt;
    }

    // A label's row takes a new name.
    if (IsLabel(track.rows[index]))
    {
        ListEdit edit;
        edit.action = ListEdit::Action::kRenameLabel;
        edit.name = "Rename Label";
        edit.rows = {index};
        edit.label = track.rows[index].label;
        edit.new_name = std::string(Trimmed(text));
        edit.refused = edit::LabelRefusal(LabelNames(track), edit.new_name).value_or("");
        return edit;
    }

    // A change that leaves the command as it is makes no edit. Text that the column can't read is refused with what the
    // column takes. The cell keeps its value.
    const std::optional<ListEdit> edit = TypedIn(track, index, column, text);
    if (!edit)
    {
        ListEdit refusal;
        refusal.action = ListEdit::Action::kChange;
        refusal.rows = {index};
        refusal.command = track.rows[index].command;
        refusal.refused = "Hokuto can't read \"" + std::string(Trimmed(text)) + "\" there. " +
                          TypingHint(track.rows[index].command.kind, column);
        return refusal;
    }
    if (edit->action == ListEdit::Action::kChange && edit->refused.empty() &&
        edit->command == track.rows[index].command)
    {
        return std::nullopt;
    }

    return edit;
}

std::optional<TypedArgument> TypedValueOf(const song::Console& console, Command command, std::string_view text)
{
    const auto value = [kind = command.kind](std::string_view part)
    {
        return ParseValue(kind, part);
    };
    const std::optional<Argument> argument = ParseArgument(text, value);
    if (!argument)
    {
        return std::nullopt;
    }

    // The refusal reads only the track's console.
    ListTrack track;
    track.console = console;
    command.value = *argument;
    std::string refused = Refusal(track, command, *argument, false).value_or("");
    return TypedArgument{std::move(command), std::move(refused)};
}

std::string ValueHint()
{
    return "A value is a number, with or without its unit, or \"random 0 to 127\" or \"variable 3\".";
}

std::optional<ListEdit> TargetEdit(const ListTrack& track, std::size_t index, uint32_t target)
{
    if (index >= track.rows.size() || !ntdlib::commands::HasTarget(track.rows[index].command.kind))
    {
        return std::nullopt;
    }

    Command command = track.rows[index].command;
    if (!command.target_outside && command.target == target)
    {
        return std::nullopt;
    }

    command.target = target;
    command.target_outside = false;
    return ChangeTo(index, "Change Command", std::move(command));
}

ListEdit ConditionEdit(const ListTrack& track, std::size_t index)
{
    Command command = track.rows.at(index).command;
    command.conditional = !command.conditional;

    return ChangeTo(index, "Change Command", std::move(command));
}

std::optional<ListEdit> InsertEdit(const ListTrack& track, std::optional<std::size_t> index, Kind kind)
{
    if (track.rows.empty())
    {
        return std::nullopt;
    }

    // A new jump or call leads to the command after it. A new track start leads to the sequence's end. The track that
    // it opens ends there at once.
    const std::size_t before = std::min(index.value_or(track.rows.size() - 1), track.rows.size() - 1);
    const std::optional<uint32_t> item = edit::ItemAtOffset(track.offsets, track.rows[before].row.command);
    const auto end = static_cast<uint32_t>(track.offsets.empty() ? 0 : track.offsets.size() - 1);
    ListEdit edit;
    edit.action = ListEdit::Action::kInsert;
    edit.name = "Insert Command";
    edit.rows = {before};
    edit.command = edit::NewCommand(kind, track.ticks_per_quarter, kind == Kind::kOpenTrack ? end : item.value_or(0));

    return edit;
}

std::optional<ListEdit> RemovalEdit(const ListTrack& track, const std::set<uint32_t>& selected)
{
    // Each command goes once, in the stretch of its first row.
    std::set<uint32_t> taken;
    ListEdit edit;
    edit.action = ListEdit::Action::kRemove;
    edit.name = "Delete";
    for (std::size_t i = 0; i < track.rows.size(); i++)
    {
        const uint32_t command = track.rows[i].row.command;
        if (!IsLabel(track.rows[i]) && selected.contains(command) && taken.insert(command).second)
        {
            edit.rows.push_back(i);
        }
    }

    return edit.rows.empty() ? std::nullopt : std::optional<ListEdit>(std::move(edit));
}

edit::CommandPlace PlaceOf(const ListTrack& track, std::size_t index)
{
    const ListRow& row = track.rows.at(index);
    return edit::CommandPlace{row.row.command, track.stretches.at(row.row.stretch)};
}

std::optional<ListEdit> LabelEdit(const ListTrack& track, std::size_t index)
{
    if (track.timeline || index >= track.rows.size() || IsLabel(track.rows[index]))
    {
        return std::nullopt;
    }

    ListEdit edit;
    edit.action = ListEdit::Action::kAddLabel;
    edit.name = "Insert Label";
    edit.rows = {index};
    edit.label = edit::UniqueLabel(LabelNames(track).labels, "Label");

    return edit;
}

std::optional<ListEdit> LabelMoveEdit(const ListTrack& track, std::size_t label, std::size_t index)
{
    if (label >= track.rows.size() || index >= track.rows.size() || !IsLabel(track.rows[label]) ||
        IsLabel(track.rows[index]) || track.rows[label].row.command == track.rows[index].row.command)
    {
        return std::nullopt;
    }

    ListEdit edit;
    edit.action = ListEdit::Action::kMoveLabel;
    edit.name = "Move Label";
    edit.rows = {index};
    edit.label = track.rows[label].label;

    return edit;
}

std::optional<ListEdit> LabelRemovalEdit(const ListTrack& track, std::size_t index)
{
    if (index >= track.rows.size() || !IsLabel(track.rows[index]))
    {
        return std::nullopt;
    }

    ListEdit edit;
    edit.action = ListEdit::Action::kRemoveLabel;
    edit.name = "Delete Label";
    edit.rows = {index};
    edit.label = track.rows[index].label;

    return edit;
}

std::optional<ListEdit> SubroutineEdit(const ListTrack& track, const std::set<uint32_t>& selected)
{
    // The first row of each selected command, in the order of the commands. The rows of a selected call's run go with
    // the call: they sit deeper than the call until its return.
    std::map<uint32_t, std::size_t> firsts;
    std::optional<int> call_depth;
    for (std::size_t i = 0; i < track.rows.size(); i++)
    {
        const ListRow& row = track.rows[i];
        if (IsLabel(row) || (call_depth && row.depth > *call_depth))
        {
            continue;
        }

        call_depth.reset();
        const std::optional<uint32_t> item = ItemOfRow(track, i);
        if (item && selected.contains(row.row.command))
        {
            firsts.emplace(*item, i);
            if (row.command.kind == Kind::kCall)
            {
                call_depth = row.depth;
            }
        }
    }
    if (firsts.empty())
    {
        return std::nullopt;
    }

    ListEdit edit;
    edit.action = ListEdit::Action::kToSubroutine;
    edit.name = "Convert to Subroutine";
    for (const auto& [item, row] : firsts)
    {
        edit.rows.push_back(row);
    }

    // The commands run from the first row of the first one, as deep as their calls and loops take them. The rows of the
    // commands and of the runs of their calls follow each other from there.
    const uint32_t first = firsts.begin()->first;
    const uint32_t last = firsts.rbegin()->first;
    const std::size_t start = firsts.begin()->second;
    const int base = track.rows[start].depth;
    for (std::size_t i = start; i < track.rows.size(); i++)
    {
        const std::optional<uint32_t> item = ItemOfRow(track, i);
        const bool inside = item && *item >= first && *item <= last;
        if (!inside && track.rows[i].depth <= base)
        {
            break;
        }
        edit.depth = std::max(edit.depth, track.rows[i].depth);
    }

    // The name of the last label at the first command or before it, with _sub after it.
    std::string name = "sub";
    uint32_t named = 0;
    for (const auto& [label, at] : track.targets)
    {
        if (at <= first && (name == "sub" || at >= named))
        {
            name = label + "_sub";
            named = at;
        }
    }
    edit.label = edit::UniqueLabel(LabelNames(track).labels, name);
    if (track.timeline)
    {
        edit.refused =
            "A timeline's subroutines come from the converter. Program in the inspector's Sequence section "
            "lets the sequence take this edit.";
    }

    return edit;
}

std::optional<ListEdit> DissolveEdit(const ListTrack& track, std::size_t index)
{
    if (index >= track.rows.size() || IsLabel(track.rows[index]) || track.rows[index].command.kind != Kind::kCall)
    {
        return std::nullopt;
    }

    ListEdit edit;
    edit.action = ListEdit::Action::kDissolveCall;
    edit.name = "Dissolve Call";
    edit.rows = {index};

    return edit;
}

std::optional<ListEdit> RealCopyEdit(const ListTrack& track, std::size_t index)
{
    std::optional<ListEdit> edit = DissolveEdit(track, index);
    if (edit)
    {
        edit->action = ListEdit::Action::kCopySubroutine;
        edit->name = "Convert to Real Copy";
    }

    return edit;
}

std::optional<uint32_t> RunOfCall(const ListTrack& track, std::size_t index)
{
    if (index >= track.rows.size() || IsLabel(track.rows[index]))
    {
        return std::nullopt;
    }

    const ListRow& row = track.rows[index];
    const uint32_t next = row.row.stretch + 1;
    const bool call =
        row.command.kind == Kind::kCall && !row.command.target_outside && row.command.target < track.offsets.size();
    if (!call || next >= track.stretches.size() ||
        track.stretches[next].entry != ntdlib::sequence_export::Entry::kCall ||
        track.stretches[next].first != track.offsets[row.command.target])
    {
        return std::nullopt;
    }

    return next;
}

} // namespace hokuto::app
