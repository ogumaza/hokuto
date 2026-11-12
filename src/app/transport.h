// SPDX-License-Identifier: MIT

// The transport: where the song plays from, whether it plays, and the cycle. Positions count quarter notes from the
// song's start. With no song open, the transport runs over an empty song at 120 BPM.

#pragma once

#include <cstdint>
#include <optional>
#include <vector>

namespace hokuto::app
{

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

// Finds the jumps of a transport's position: its moves for the user (Transport::Moves) and its Stops while stopped
// (Transport::IdleStops). Go to Project Start and another song from its start are such jumps. A view that keeps the
// playhead in sight follows them (PageTurn).
class JumpWatch
{
public:
    // Whether the position has jumped since the last call, or since the watch was made.
    bool Jumped(const Transport& transport);

private:
    uint64_t moves_ = 0;
    uint64_t idle_stops_ = 0;
};

// A view of the song across, in its pixels from the song's start: it starts at `scroll` and is `width` wide. Playback
// turns the page once the playhead comes within `margin` of the right edge. A new page shows the playhead `lead` from
// its left edge.
struct PageView
{
    float scroll = 0.0f;
    float width = 0.0f;
    float margin = 0.0f;
    float lead = 0.0f;
};

// The scroll of `view` that turns the page to the playhead at `playhead`, in the same pixels, or nothing where the view
// stays. While `playing`, the page turns once the playhead comes within the margin of the right edge, or is to the left
// of the view. After a jump (`jumped`), the page turns once the playhead is outside the view. The new page starts no
// earlier than the song's start.
std::optional<float> PageTurn(const PageView& view, float playhead, bool playing, bool jumped);

} // namespace hokuto::app
