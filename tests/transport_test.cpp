// SPDX-License-Identifier: MIT

// Tests the transport's commands that go to the locators and to the bounds of parts, its empty song, and the text of
// times, positions and lengths, with the hints of what a typed position or length takes (src/app/transport.h), without
// a window.

#include "app/transport.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "xsflib/testing/check.h"

using hokuto::app::LengthHint;
using hokuto::app::LengthText;
using hokuto::app::ParseLength;
using hokuto::app::ParsePosition;
using hokuto::app::ParseTime;
using hokuto::app::PositionHint;
using hokuto::app::PositionText;
using hokuto::app::TimeText;
using hokuto::app::Transport;
using hokuto::app::WholeMs;

namespace
{

// Each command moves the position to its locator, while stopped or playing. Each counts as a move. A move while stopped
// sets where Stop goes back to. A move while playing doesn't. Only the Stop while stopped counts as an idle stop: the
// program ends a preview at such a press.
void TestGoToLocators()
{
    Transport transport;
    transport.SetLocators(8.0, 16.0);
    transport.SetPosition(3.0);
    const uint64_t moves = transport.Moves();
    const uint64_t idle_stops = transport.IdleStops();

    transport.GoToLeftLocator();
    const double left = transport.Position();
    transport.GoToRightLocator();
    const double right = transport.Position();
    transport.Start();
    transport.GoToLeftLocator();
    const double playing = transport.Position();
    transport.Stop();
    const uint64_t stopped = transport.IdleStops();
    transport.Stop();

    SEQLIB_CHECK(left == 8.0 && right == 16.0 && playing == 8.0);
    SEQLIB_CHECK(transport.Moves() == moves + 3);
    SEQLIB_CHECK(transport.Position() == 16.0);
    SEQLIB_CHECK(stopped == idle_stops && transport.IdleStops() == idle_stops + 1);
}

// Locate moves to the nearest bound ahead, or behind for a negative direction. The bounds can come in any order, and
// twice. A bound at the position doesn't count. With no bound that way, the position stays.
void TestLocate()
{
    Transport transport;
    const std::vector<double> kBounds = {8.0, 0.0, 12.0, 4.0, 12.0};
    transport.SetPosition(4.0);

    transport.Locate(kBounds, 1);
    const double next = transport.Position();
    transport.Locate(kBounds, -1);
    const double back = transport.Position();
    transport.Locate(kBounds, -1);
    const double previous = transport.Position();
    transport.Locate(kBounds, -1);
    const double before_first = transport.Position();
    transport.SetPosition(5.5);
    transport.Locate(kBounds, 1);
    transport.Locate(kBounds, 1);
    const double last = transport.Position();
    transport.Locate(kBounds, 1);
    const double after_last = transport.Position();

    SEQLIB_CHECK(next == 8.0 && back == 4.0 && previous == 0.0 && before_first == 0.0);
    SEQLIB_CHECK(last == 12.0 && after_last == 12.0);
}

// A time shows hours, minutes, seconds and milliseconds. A typed time takes seconds, minutes and seconds, or hours,
// minutes and seconds, with three decimals at most. After a colon, the minutes and the seconds stay below 60. A time
// past the limit, a sign, an empty field and anything else can't be read. The display shows a time's whole
// milliseconds, rounded down. A time a millionth of a millisecond short of a whole one shows that one.
void TestTimeText()
{
    constexpr uint64_t kLimit = 4'294'967'295;

    SEQLIB_CHECK(TimeText(0) == "0:00:00.000");
    SEQLIB_CHECK(TimeText(90'000) == "0:01:30.000");
    SEQLIB_CHECK(TimeText(3'723'004) == "1:02:03.004");
    SEQLIB_CHECK(TimeText(kLimit) == "1193:02:47.295");
    SEQLIB_CHECK(WholeMs(2'020.7) == 2'020);
    SEQLIB_CHECK(WholeMs(2'020.9999999) == 2'021);
    SEQLIB_CHECK(WholeMs(2'021.0) == 2'021);
    SEQLIB_CHECK(WholeMs(-3.5) == 0);
    SEQLIB_CHECK(ParseTime("90", kLimit) == 90'000u);
    SEQLIB_CHECK(ParseTime(" 1:30 ", kLimit) == 90'000u);
    SEQLIB_CHECK(ParseTime("0:01:30.000", kLimit) == 90'000u);
    SEQLIB_CHECK(ParseTime("1.5", kLimit) == 1'500u);
    SEQLIB_CHECK(ParseTime("1:02:03.04", kLimit) == 3'723'040u);
    SEQLIB_CHECK(ParseTime("4294967.295", kLimit) == kLimit);
    SEQLIB_CHECK(ParseTime("1193:02:47.295", kLimit) == kLimit);
    for (const char* text : {"", " ", "4294967.296", "-1", "+1", "1.", ".5", "1.2345", "1:60", "1:00:60", ":30",
                             "1::30", "1:2:3:4", "1e3", "0x10", "1,5", "1:30.5.5", "12345678901"})
    {
        SEQLIB_CHECK(!ParseTime(text, kLimit).has_value());
    }
}

// A typed position reads back what PositionText shows: the beat and the sixteenth from 1, and the tick from 0, with 480
// ticks to the quarter. The fields after the first can be left out. A length's fields count from 0. A field past its
// range, a space between two digits, an empty field between points, a sign and anything else can't be read.
void TestPositionText()
{
    constexpr int kBeats = 4;

    for (const double quarters : {0.0, 0.25, 4.0, 10.5, 123.0 + 7.0 / 480.0})
    {
        SEQLIB_CHECK(ParsePosition(PositionText(quarters, kBeats), kBeats) == quarters);
        SEQLIB_CHECK(ParseLength(LengthText(quarters, kBeats), kBeats) == quarters);
    }
    SEQLIB_CHECK(ParsePosition("1", kBeats) == 0.0);
    SEQLIB_CHECK(ParsePosition("5", kBeats) == 4.0);
    SEQLIB_CHECK(ParsePosition("5. 3", kBeats) == 4.5);
    SEQLIB_CHECK(ParsePosition(" 5. 3. 60 ", kBeats) == 4.625);
    SEQLIB_CHECK(ParsePosition("5.3.60.", kBeats) == 4.625);
    SEQLIB_CHECK(ParseLength("2", kBeats) == 2.0);
    SEQLIB_CHECK(ParseLength("0. 1", kBeats) == 0.25);
    SEQLIB_CHECK(ParseLength("0. 0. 1", kBeats) == 1.0 / 480.0);
    for (const char* text : {"", " ", ".", "0", "5. 0", "5. 5", "5. 1. 120", "5 3", "5..3", ". 3", "-1", "+1",
                             "5. 1. 0. 0", "1e3", "5,3", "1234567890"})
    {
        SEQLIB_CHECK(!ParsePosition(text, kBeats).has_value());
    }
    for (const char* text : {"", "0. 4", "0. 0. 120", "-1", "1. 2. 3. 4"})
    {
        SEQLIB_CHECK(!ParseLength(text, kBeats).has_value());
    }
}

// A hint of what a field of a position or a length takes gives an example that the field reads: the second beat, and
// a sixteenth.
void TestHints()
{
    constexpr int kBeats = 4;

    SEQLIB_CHECK(PositionHint("An end") == "An end is a position, such as 2. 1. 0.");
    SEQLIB_CHECK(LengthHint() == "A length is a number of beats, sixteenths and ticks, such as 0. 1. 0.");
    SEQLIB_CHECK(ParsePosition("2. 1. 0", kBeats) == 1.0);
    SEQLIB_CHECK(ParseLength("0. 1. 0", kBeats) == 0.25);
}

// A transport starts with the empty song: 120 BPM and 4 beats to the bar. A song's tempo and beats to the bar give way
// to the empty song's again once no song shows.
void TestEmptySong()
{
    Transport transport;
    XSFLIB_CHECK(transport.Tempo() == 120.0 && transport.BeatsPerBar() == 4);
    transport.SetTempo(87.5);
    transport.SetBeatsPerBar(3);

    transport.TakeEmptySong();

    XSFLIB_CHECK(transport.Tempo() == 120.0 && transport.BeatsPerBar() == 4);
}

} // namespace

int main()
{
    TestGoToLocators();
    TestLocate();
    TestEmptySong();
    TestTimeText();
    TestPositionText();
    TestHints();

    return xsflib::testing::Summary("transport");
}
