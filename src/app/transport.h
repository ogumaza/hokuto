// SPDX-License-Identifier: MIT

// The transport: where the song plays from, whether it plays, and the cycle. Positions count quarter notes from the
// song's start. With no song open, the transport runs over an empty song at 120 BPM.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

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

class Transport
{
public:
    double Tempo() const;
    void SetTempo(double beats_per_minute);
    int BeatsPerBar() const;
    void SetBeatsPerBar(int beats);

    // Takes the empty song's tempo and beats to the bar for a time when no song shows. The transport starts with them.
    void TakeEmptySong();

    bool Playing() const;
    double Position() const;

    // Moves the position for the user, such as by a click in the ruler. Moves() counts these moves. Playback that
    // follows the transport starts again from the new position.
    void SetPosition(double quarters);
    uint64_t Moves() const;

    // Sets the position that playback has reached. It isn't a move.
    void Follow(double quarters);

    // The transport's commands. Stop while stopped goes back to where playback last started. IdleStops() counts these
    // presses. Such a press also ends a preview that plays outside the transport, such as the browser's. A step moves
    // to a whole beat, or to a whole bar while bars show. Each step moves once, and a held key repeats it.
    void Start();
    void Stop();
    uint64_t IdleStops() const;
    void StartStop();
    void GoToStart();
    void GoToLeftLocator();
    void GoToRightLocator();
    void Step(int steps);

    // Locate Next Event, or Locate Previous Event for a negative `direction`. The position moves to the nearest of
    // `bounds` that way, such as the starts and ends of a track's parts. A bound at the position doesn't count. The
    // position stays when no bound lies that way.
    void Locate(const std::vector<double>& bounds, int direction);

    // The cycle plays the range between the locators over and over.
    bool Cycle() const;
    void SetCycle(bool on);
    double CycleStart() const;
    double CycleEnd() const;
    void SetLocators(double start, double end);

    // Moves the position on by `seconds` of playback. While the cycle is on, the position goes back round it.
    void Advance(double seconds);

private:
    // The empty song's tempo and beats to the bar.
    static constexpr double kEmptyTempo = 120.0;
    static constexpr int kEmptyBeatsPerBar = 4;

    double tempo_ = kEmptyTempo;
    int beats_per_bar_ = kEmptyBeatsPerBar;
    bool playing_ = false;
    double position_ = 0.0;
    double start_position_ = 0.0;
    uint64_t moves_ = 0;
    uint64_t idle_stops_ = 0;
    bool cycle_ = false;
    double cycle_start_ = 0.0;
    double cycle_end_ = 16.0;
};

} // namespace hokuto::app
