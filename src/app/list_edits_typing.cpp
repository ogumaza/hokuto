// SPDX-License-Identifier: MIT

// Typed text in the List Editor: the parsers of values, lengths, indices, tracks and notes, the refusals, and the edits
// and hints of a typed cell.

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/commands/units.h"
#include "xsflib/ascii.h"

#include "app/key_edits.h"
#include "app/list_edits.h"
#include "app/list_edits_parts.h"
#include "app/time_text.h"
#include "app/transport.h"
#include "edit/commands.h"
#include "edit/quintet.h"
#include "edit/subroutines.h"
#include "song/archive.h"
#include "song/driver.h"
#include "song/konami.h"

namespace hokuto::app
{
namespace
{

using list_edits_parts::Addressed;
using list_edits_parts::AddressedBy;
using list_edits_parts::ChangeTo;
using list_edits_parts::kBuses;
using list_edits_parts::kLfos;
using list_edits_parts::kTracks;
using list_edits_parts::LabelNames;
using list_edits_parts::TicksText;
using list_edits_parts::UnitSuffix;
using list_edits_parts::ValueColumn;
using ntdlib::commands::Argument;
using ntdlib::commands::Command;
using ntdlib::commands::Kind;
using ntdlib::commands::kOne;
using ntdlib::commands::Source;

// The highest variable that a command can name: a byte.
constexpr int kTopVariable = 255;

// The most digits after a decimal point that a typed number keeps.
constexpr int kFractionDigits = 12;

// `text` in lower case, for ASCII letters.
std::string Lower(std::string_view text)
{
    return xsflib::AsciiLower(std::string(text));
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
        return std::string(KindName(kind)) + " time";
    }
    if (kind == Kind::kNote)
    {
        return "Note length";
    }

    return std::string(KindName(kind));
}

// Why `command` can't take `argument` in its value, or with `time` in its time, on the console of `track`. Nothing when
// the console's bytes hold it.
std::optional<std::string> Refusal(const ListTrack& track, const Command& command, const Argument& argument, bool time)
{
    const std::string name = ArgumentName(command.kind, time);
    const std::optional<edit::ValueRange> range =
        edit::ArgumentRange(track.console, track.number, command, argument.source, time);
    if (!range)
    {
        if (argument.source == Source::kRandom)
        {
            return name + " can't be random.";
        }
        if (argument.source == Source::kVariable)
        {
            return name + " can't come from a variable.";
        }

        return name + " can't be set here.";
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

        return ticks ? WithoutFigureSpaces(TicksText(value, track.ticks_per_quarter))
                     : ValueText(track.console, command.kind, value);
    };
    if (argument.source == Source::kVariable)
    {
        return "The variable number must be between " + shown(range->lowest) + " and " + shown(range->highest) + ".";
    }

    return name + " must be between " + shown(range->lowest) + " and " + shown(range->highest) + ".";
}

} // namespace

namespace list_edits_parts
{

ListEdit ChangeTo(std::size_t index, std::string name, Command command, std::optional<std::string> refused)
{
    ListEdit edit;
    edit.action = ListEdit::Action::kChange;
    edit.name = std::move(name);
    edit.rows = {index};
    edit.command = std::move(command);
    edit.refused = refused.value_or("");

    return edit;
}

} // namespace list_edits_parts

namespace
{

// The edit of a typed length: a note's length, a wait's length or a setting's time. An empty time takes the time away.
std::optional<ListEdit> TypedLength(const ListTrack& track, std::size_t index, std::string_view text)
{
    const ListRow& row = track.rows[index];
    Command command = row.command;
    const bool time = command.kind != Kind::kNote && command.kind != Kind::kWait;
    if (time && Trimmed(text).empty())
    {
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
    std::string what = "The variable number";
    if (command.kind == Kind::kOpenTrack)
    {
        number = After(number, "track").value_or(number);
        top = kTracks - 1;
        what = "The track number";
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
        what = "The LFO number";
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
        refused = what + " must be between 0 and " + std::to_string(top) + ".";
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

// The edit of a typed pitch of a note of Konami's driver: a transposition of a sample, or a note.
std::optional<ListEdit> TypedKonamiPitch(std::size_t index, std::string_view text, Command command)
{
    // A cell's text names the note first: "Sample 12 +3", "Sample 12" or "Note 40".
    const std::string name = command.sample ? "Sample " + std::to_string(*command.sample) : "Note";
    const bool named = text.starts_with(name);
    std::string_view number = named ? text.substr(name.size()) : text;
    number.remove_prefix(std::min(number.find_first_not_of(' '), number.size()));
    number.remove_prefix(number.starts_with('+') ? 1 : 0);
    const std::optional<int64_t> value =
        named && number.empty() && command.sample ? std::optional<int64_t>(0) : ParseWhole(number);
    const int64_t lowest = command.sample ? INT8_MIN : 0;
    const int64_t highest = command.sample ? INT8_MAX : UINT8_MAX;
    if (!value || *value < lowest || *value > highest)
    {
        std::string refused = command.sample ? "Enter a transposition from -128 to +127 semitones, such as +3 or -5."
                                             : "The note must be between 0 and 255.";
        return ChangeTo(index, "Transpose", std::move(command), std::move(refused));
    }

    command.key = static_cast<uint8_t>(*value);
    return ChangeTo(index, "Transpose", std::move(command));
}

// The edit of a typed key of a note, by name or number. On Quintet's driver, the row shows the key that the note plays
// (`played`), and the command contains only the pitch in the channel's octave: the typed key moves the pitch by its
// distance from the shown key. The note edits refuse a pitch above the octave.
std::optional<ListEdit> TypedKey(std::size_t index, std::string_view text, Command command,
                                 std::optional<int> played = std::nullopt)
{
    std::optional<int> key = ParseKeyName(text);
    if (!key)
    {
        const std::optional<int64_t> number = ParseWhole(text);
        key = number && *number >= 0 && *number < kKeys ? std::optional<int>(static_cast<int>(*number)) : std::nullopt;
    }
    if (!key)
    {
        const std::optional<int64_t> number = ParseWhole(text);
        return number ? std::optional<ListEdit>(ChangeTo(index, "Transpose", std::move(command),
                                                         "The key must be between 0 and 127 (C-2 to G8)."))
                      : std::nullopt;
    }

    if (played)
    {
        const int pitch = command.key + *key - *played;
        if (pitch < 0)
        {
            return ChangeTo(index, "Transpose", std::move(command), std::string(edit::kQuintetKeepsOctave));
        }
        key = std::min(pitch, static_cast<int>(UINT8_MAX));
    }

    command.key = static_cast<uint8_t>(*key);
    return ChangeTo(index, "Transpose", std::move(command));
}

// The edit of a typed velocity of a note, in the range of the track's driver.
std::optional<ListEdit> TypedVelocity(const ListTrack& track, std::size_t index, std::string_view text, Command command)
{
    const std::optional<int64_t> velocity = ParseWhole(text);
    if (!velocity)
    {
        return std::nullopt;
    }
    const std::optional<song::NoteVelocities> velocities =
        track.console.driver != nullptr ? track.console.driver->Velocities() : std::nullopt;
    if (velocities && (!velocities->most || *velocity < 0 || *velocity > *velocities->most))
    {
        return ChangeTo(index, "Change Velocity", std::move(command), velocities->refusal);
    }
    if (!velocities && (*velocity < 0 || *velocity > kTopVelocity))
    {
        return ChangeTo(index, "Change Velocity", std::move(command), "Velocity must be between 0 and 127.");
    }

    command.velocity = *velocity * kOne;
    return ChangeTo(index, "Change Velocity", std::move(command));
}

// The edit of a typed pitch or velocity of a note.
std::optional<ListEdit> TypedNote(const ListTrack& track, std::size_t index, std::string_view text, bool pitch)
{
    Command command = track.rows[index].command;
    if (pitch && song::KonamiRevisionOf(track.console.driver))
    {
        return TypedKonamiPitch(index, text, std::move(command));
    }
    if (pitch)
    {
        return TypedKey(index, text, std::move(command), track.rows[index].played_key);
    }

    return TypedVelocity(track, index, text, std::move(command));
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
            return "Enter track numbers from 0 to 15, separated by commas (such as 0, 1, 2), or none.";
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
            return edit.shift != 0 ? std::move(edit) : ChangeTo(index, "Change Command", row.command);
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

} // namespace

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
        refusal.refused =
            "Can't read \"" + std::string(Trimmed(text)) + "\". " + TypingHint(track.rows[index].command.kind, column);
        return refusal;
    }
    if (edit->action == ListEdit::Action::kChange && edit->refused.empty() &&
        edit->command == track.rows[index].command)
    {
        return std::nullopt;
    }

    return edit;
}

std::optional<TypedArgument> TypedValueOf(const song::Console& console, int track, Command command,
                                          std::string_view text)
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

    // The refusal reads only the track's console and number.
    ListTrack list;
    list.console = console;
    list.number = track;
    command.value = *argument;
    std::string refused = Refusal(list, command, *argument, false).value_or("");
    return TypedArgument{std::move(command), std::move(refused)};
}

std::string ValueHint()
{
    return "A value is a number (with or without its unit), \"random 0 to 127\" or \"variable 3\".";
}

} // namespace hokuto::app
