// SPDX-License-Identifier: MIT

// Positions, lengths and times as the display shows them, the parsers of what the user types in their forms, and the
// helpers of text that the parsers and the windows share. A position counts quarter notes from the song's start.

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace hokuto::app
{

// Hokuto counts beats only (the maintainer, 2026-10-14). No sequence format stores a time signature, and a guess can't
// be trusted. The code for bars stays behind this switch. The feature may come back.
inline constexpr bool kShowBars = false;

// The ticks to the quarter note that the display counts: 120 to the sixteenth.
inline constexpr int kDisplayTicksPerQuarter = 480;

// A position as the display shows it. With bars, it shows the bar, the beat, the sixteenth and the tick, with 120 ticks
// to the sixteenth. Without them, it's the beat, the sixteenth and the tick. Figure spaces pad the first field and the
// tick to three digits. A figure space is as wide as a digit. The fields then keep their places as the numbers change.
std::string PositionText(double quarters, int beats_per_bar);

// A length in the same form. Each field counts from 0.
std::string LengthText(double quarters, int beats_per_bar);

// `text` without the figure spaces that PositionText and LengthText pad their fields with, for a position or a length
// in a sentence.
std::string WithoutFigureSpaces(std::string text);

// The quarter notes of a position that the user typed in PositionText's form, or of a length in LengthText's form. The
// fields after the first can be left out: they then count as the start of the field before. "5" is the start of the
// fifth beat, and "5. 3" the start of its third sixteenth. The figure spaces and spaces don't count. A beat or a
// sixteenth past the bar's or the beat's last one, a tick past 119, and anything else can't be read.
std::optional<double> ParsePosition(std::string_view text, int beats_per_bar);
std::optional<double> ParseLength(std::string_view text, int beats_per_bar);

// What a field of a position or a length takes, for the message that refuses text that ParsePosition or ParseLength
// can't read. Each gives an example in the display's form. PositionHint("A start") gives "A start is a position, such
// as 2. 1. 0.", and LengthHint gives "A length is a number of beats, sixteenths and ticks, such as 0. 1. 0." With bars,
// the bar comes first.
std::string PositionHint(std::string_view subject);
std::string LengthHint();

// A time as the display shows it: hours, minutes, seconds and milliseconds, such as 0:01:30.000.
std::string TimeText(uint64_t ms);

// The whole milliseconds that the display shows for a time of `ms`: the time rounded down, and 0 for a time before 0. A
// time a millionth of a millisecond short of a whole one shows that one. A new change of a variable takes the same
// time.
uint64_t WholeMs(double ms);

// The milliseconds of a time that the user typed: seconds, minutes and seconds, or hours, minutes and seconds, with a
// colon between each two, such as 90, 1:30 or 0:01:30.000. The seconds take three decimals at most. After a colon, the
// minutes and the seconds stay below 60. Spaces at either end don't count. Returns nothing for any other text, or for a
// time past `limit`.
std::optional<uint64_t> ParseTime(std::string_view text, uint64_t limit);

// `text` without spaces at either end.
std::string_view Trimmed(std::string_view text);

// The number that `text` writes in decimal digits alone, or nothing for other text or a number past 64 bits.
std::optional<uint64_t> WholeNumber(std::string_view text);

// `names` joined for a sentence: "A", "A and B" or "A, B and C".
std::string JoinedNames(std::span<const std::string> names);

} // namespace hokuto::app
