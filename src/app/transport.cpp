// SPDX-License-Identifier: MIT

#include "app/transport.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string_view>
#include <system_error>
#include <vector>

namespace hokuto::app
{
namespace
{

// A bound this close to the position, in quarter notes, counts as the position itself.
constexpr double kSameBound = 1e-6;

// `value` with figure spaces before it, up to `width` digits.
std::string Padded(int64_t value, std::size_t width)
{
    const std::string digits = std::to_string(value);
    std::string text;
    for (std::size_t i = digits.size(); i < width; i++)
    {
        text += "\u2007";
    }

    return text + digits;
}

// The bar, the beat and the sixteenth count from `first`. The tick counts from 0.
std::string BarsAndBeats(double quarters, int beats_per_bar, int first)
{
    const auto ticks = static_cast<int64_t>(std::floor(quarters * kDisplayTicksPerQuarter + 1e-6));
    const int64_t ticks_per_bar = int64_t{kDisplayTicksPerQuarter} * beats_per_bar;
    return Padded(ticks / ticks_per_bar + first, 3) + ". " +
           std::to_string(ticks / kDisplayTicksPerQuarter % beats_per_bar + first) + ". " +
           std::to_string(ticks / (kDisplayTicksPerQuarter / 4) % 4 + first) + ". " +
           Padded(ticks % (kDisplayTicksPerQuarter / 4), 3);
}

// The beat and the sixteenth count from `first`. The tick counts from 0.
std::string Beats(double quarters, int first)
{
    const auto ticks = static_cast<int64_t>(std::floor(quarters * kDisplayTicksPerQuarter + 1e-6));
    return Padded(ticks / kDisplayTicksPerQuarter + first, 3) + ". " +
           std::to_string(ticks / (kDisplayTicksPerQuarter / 4) % 4 + first) + ". " +
           Padded(ticks % (kDisplayTicksPerQuarter / 4), 3);
}

// The quarter notes of a position or a length in the display's form. The display's fields count from `first`. Each
// field holds up to nine digits.
std::optional<double> ParseFields(std::string_view text, int beats_per_bar, int first)
{
    // The fields between the points, without the spaces and the figure spaces that PositionText puts in. A space
    // between two digits ends the field.
    constexpr std::string_view kFigureSpace = "\u2007";
    std::vector<std::string> fields(1);
    bool spaced = false;
    while (!text.empty())
    {
        const bool space = text.starts_with(kFigureSpace) || text.front() == ' ';
        const char c = text.front();
        text.remove_prefix(text.starts_with(kFigureSpace) ? kFigureSpace.size() : 1);
        if (space)
        {
            spaced = !fields.back().empty();
        }
        else if (c == '.')
        {
            fields.emplace_back();
            spaced = false;
        }
        else if (c >= '0' && c <= '9' && !spaced && fields.back().size() < 9)
        {
            fields.back() += c;
        }
        else
        {
            return std::nullopt;
        }
    }
    while (fields.size() > 1 && fields.back().empty())
    {
        fields.pop_back();
    }

    // The bar, the beat and the sixteenth, or the beat and the sixteenth, then the tick. A field that's left out counts
    // as `first`, or as 0 for the tick.
    const std::vector<int64_t> counts = kShowBars
                                            ? std::vector<int64_t>{0, beats_per_bar, 4, kDisplayTicksPerQuarter / 4}
                                            : std::vector<int64_t>{0, 4, kDisplayTicksPerQuarter / 4};
    if (fields.size() > counts.size() || fields.front().empty())
    {
        return std::nullopt;
    }
    int64_t ticks = 0;
    int64_t ticks_per_field = kShowBars ? int64_t{kDisplayTicksPerQuarter} * beats_per_bar : kDisplayTicksPerQuarter;
    for (std::size_t i = 0; i < counts.size(); i++)
    {
        const int64_t start = i + 1 == counts.size() ? 0 : first;
        int64_t value = start;
        if (i < fields.size())
        {
            if (fields[i].empty() ||
                std::from_chars(fields[i].data(), fields[i].data() + fields[i].size(), value).ec != std::errc() ||
                value < start || (counts[i] != 0 && value - start >= counts[i]))
            {
                return std::nullopt;
            }
        }
        ticks += (value - start) * ticks_per_field;
        if (i + 1 < counts.size())
        {
            ticks_per_field /= counts[i + 1];
        }
    }

    return static_cast<double>(ticks) / kDisplayTicksPerQuarter;
}

} // namespace

std::string PositionText(double quarters, int beats_per_bar)
{
    return kShowBars ? BarsAndBeats(quarters, beats_per_bar, 1) : Beats(quarters, 1);
}

std::string LengthText(double quarters, int beats_per_bar)
{
    return kShowBars ? BarsAndBeats(quarters, beats_per_bar, 0) : Beats(quarters, 0);
}

std::string WithoutFigureSpaces(std::string text)
{
    constexpr std::string_view kFigureSpace = "\u2007";
    for (std::size_t at = text.find(kFigureSpace); at != std::string::npos; at = text.find(kFigureSpace))
    {
        text.erase(at, kFigureSpace.size());
    }

    return text;
}

std::optional<double> ParsePosition(std::string_view text, int beats_per_bar)
{
    return ParseFields(text, beats_per_bar, 1);
}

std::optional<double> ParseLength(std::string_view text, int beats_per_bar)
{
    return ParseFields(text, beats_per_bar, 0);
}

std::string PositionHint(std::string_view subject)
{
    // The second beat, in a bar of 4 beats when bars show.
    const std::string example = WithoutFigureSpaces(PositionText(1.0, 4));
    return std::string(subject) + " is a position, such as " + example + ".";
}

std::string LengthHint()
{
    // A sixteenth, in a bar of 4 beats when bars show.
    const std::string example = WithoutFigureSpaces(LengthText(0.25, 4));
    return std::string("A length is a number of ") + (kShowBars ? "bars, beats" : "beats") +
           ", sixteenths and ticks, such as " + example + ".";
}

std::string TimeText(uint64_t ms)
{
    std::array<char, 40> text{};
    std::snprintf(text.data(), text.size(), "%llu:%02u:%02u.%03u", static_cast<unsigned long long>(ms / 3'600'000),
                  static_cast<unsigned>(ms / 60'000 % 60), static_cast<unsigned>(ms / 1'000 % 60),
                  static_cast<unsigned>(ms % 1'000));

    return text.data();
}

uint64_t WholeMs(double ms)
{
    return static_cast<uint64_t>(std::floor(std::max(ms, 0.0) + 1e-6));
}

std::optional<uint64_t> ParseTime(std::string_view text, uint64_t limit)
{
    while (!text.empty() && text.front() == ' ')
    {
        text.remove_prefix(1);
    }
    while (!text.empty() && text.back() == ' ')
    {
        text.remove_suffix(1);
    }

    // The fields from the left. Each holds up to ten digits, and the last one up to three decimals after them.
    std::vector<std::string_view> fields;
    for (std::size_t colon = text.find(':'); colon != std::string_view::npos; colon = text.find(':'))
    {
        fields.push_back(text.substr(0, colon));
        text.remove_prefix(colon + 1);
    }
    std::string_view decimals;
    if (const std::size_t point = text.find('.'); point != std::string_view::npos)
    {
        decimals = text.substr(point + 1);
        text = text.substr(0, point);
        if (decimals.empty() || decimals.size() > 3)
        {
            return std::nullopt;
        }
    }
    fields.push_back(text);
    if (fields.size() > 3)
    {
        return std::nullopt;
    }

    // Hours, minutes and seconds, with the first field the largest. The fields after it stay below 60.
    const auto digits = [](std::string_view field)
    {
        return !field.empty() && field.size() <= 10 &&
               std::all_of(field.begin(), field.end(), [](char c) { return c >= '0' && c <= '9'; });
    };

    uint64_t seconds = 0;
    for (std::size_t i = 0; i < fields.size(); i++)
    {
        uint64_t value = 0;
        if (!digits(fields[i]) ||
            std::from_chars(fields[i].data(), fields[i].data() + fields[i].size(), value).ec != std::errc() ||
            (i > 0 && value >= 60))
        {
            return std::nullopt;
        }
        seconds = seconds * 60 + value;
    }

    // The decimals give the milliseconds.
    if (!decimals.empty() && !digits(decimals))
    {
        return std::nullopt;
    }
    uint64_t ms = seconds * 1000;
    for (std::size_t i = 0; i < decimals.size(); i++)
    {
        ms += static_cast<uint64_t>(decimals[i] - '0') * (i == 0 ? 100 : i == 1 ? 10 : 1);
    }

    return ms <= limit ? std::optional<uint64_t>(ms) : std::nullopt;
}

double Transport::Tempo() const
{
    return tempo_;
}

void Transport::SetTempo(double beats_per_minute)
{
    tempo_ = beats_per_minute;
}

int Transport::BeatsPerBar() const
{
    return beats_per_bar_;
}

void Transport::SetBeatsPerBar(int beats)
{
    beats_per_bar_ = beats;
}

void Transport::TakeEmptySong()
{
    tempo_ = kEmptyTempo;
    beats_per_bar_ = kEmptyBeatsPerBar;
}

bool Transport::Playing() const
{
    return playing_;
}

double Transport::Position() const
{
    return position_;
}

void Transport::SetPosition(double quarters)
{
    position_ = std::max(quarters, 0.0);
    if (!playing_)
    {
        start_position_ = position_;
    }
    moves_++;
}

uint64_t Transport::Moves() const
{
    return moves_;
}

void Transport::Follow(double quarters)
{
    position_ = std::max(quarters, 0.0);
}

void Transport::Start()
{
    if (!playing_)
    {
        playing_ = true;
        start_position_ = position_;
    }
}

void Transport::Stop()
{
    if (playing_)
    {
        playing_ = false;
    }
    else
    {
        position_ = start_position_;
        idle_stops_++;
    }
}

uint64_t Transport::IdleStops() const
{
    return idle_stops_;
}

void Transport::StartStop()
{
    if (playing_)
    {
        Stop();
    }
    else
    {
        Start();
    }
}

void Transport::GoToStart()
{
    SetPosition(0.0);
}

void Transport::GoToLeftLocator()
{
    SetPosition(cycle_start_);
}

void Transport::GoToRightLocator()
{
    SetPosition(cycle_end_);
}

void Transport::Step(int steps)
{
    const double step = kShowBars ? beats_per_bar_ : 1.0;
    SetPosition((std::floor(position_ / step + 1e-9) + steps) * step);
}

void Transport::Locate(const std::vector<double>& bounds, int direction)
{
    std::optional<double> nearest;
    for (const double bound : bounds)
    {
        const bool ahead = direction > 0 ? bound > position_ + kSameBound : bound < position_ - kSameBound;
        if (ahead && (!nearest || (direction > 0 ? bound < *nearest : bound > *nearest)))
        {
            nearest = bound;
        }
    }

    if (nearest)
    {
        SetPosition(*nearest);
    }
}

bool Transport::Cycle() const
{
    return cycle_;
}

void Transport::SetCycle(bool on)
{
    cycle_ = on;
}

double Transport::CycleStart() const
{
    return cycle_start_;
}

double Transport::CycleEnd() const
{
    return cycle_end_;
}

void Transport::SetLocators(double start, double end)
{
    cycle_start_ = std::max(std::min(start, end), 0.0);
    cycle_end_ = std::max(start, end);
}

void Transport::Advance(double seconds)
{
    if (!playing_)
    {
        return;
    }

    // Playback that reaches the right locator goes back to the left one. Playback that starts past it carries on.
    const double before = position_;
    position_ += seconds * tempo_ / 60.0;
    const double length = cycle_end_ - cycle_start_;
    if (cycle_ && length > 0.0 && before < cycle_end_ && position_ >= cycle_end_)
    {
        position_ = cycle_start_ + std::fmod(position_ - cycle_end_, length);
    }
}

} // namespace hokuto::app
