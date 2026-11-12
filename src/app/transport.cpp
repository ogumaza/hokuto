// SPDX-License-Identifier: MIT

#include "app/transport.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <vector>

#include "app/time_text.h"

namespace hokuto::app
{
namespace
{

// A bound this close to the position, in quarter notes, counts as the position itself.
constexpr double kSameBound = 1e-6;

} // namespace

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
    // From between two whole steps, the first step moves to the next one ahead or behind.
    const double step = kShowBars ? beats_per_bar_ : 1.0;
    const double place = position_ / step;
    const double whole = steps < 0 ? std::ceil(place - 1e-9) : std::floor(place + 1e-9);
    SetPosition((whole + steps) * step);
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

bool JumpWatch::Jumped(const Transport& transport)
{
    const bool jumped = transport.Moves() != moves_ || transport.IdleStops() != idle_stops_;
    moves_ = transport.Moves();
    idle_stops_ = transport.IdleStops();

    return jumped;
}

std::optional<float> PageTurn(const PageView& view, float playhead, bool playing, bool jumped)
{
    const bool past_left = playhead < view.scroll;
    const bool off_page = playing && (past_left || playhead > view.scroll + view.width - view.margin);
    const bool jumped_out = jumped && (past_left || playhead > view.scroll + view.width);
    if (!off_page && !jumped_out)
    {
        return std::nullopt;
    }

    return std::max(playhead - view.lead, 0.0f);
}

} // namespace hokuto::app
