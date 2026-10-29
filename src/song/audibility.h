// SPDX-License-Identifier: MIT

// The level measure of the project's audibility method, for the measured warnings of an export. It compares a render of
// a song with a reference render of the same song, such as a synth's render of an exported MIDI file with Hokuto's
// playback. Both pass through a causal second-order Butterworth high-pass at 20 Hz first. An offset below 20 Hz isn't
// heard, but it would add to a window's power. The measure then takes windows of 50 ms without overlap. A window's
// level is 10 log10 of the mean power of both channels. The gain is the median over the reference's counted windows of
// the render's level less the reference's. It's never a finding: a listener sets the volume. A window counts when the
// louder render lies within 50 dB of the loudest window of either render, and above -70 dBFS. Both levels are held at
// 50 dB below that loudest window. A span of 1 s (20 windows, a hop of 10) with at least 10 counted windows is a
// finding when its median difference passes 1 dB either way (L1). About 1 dB is the smallest change of level that
// listeners notice in music. A run of 4 counted windows or more (200 ms), each more than 6 dB off, is a finding too
// (L2). A missing or extra note, or a wrong envelope, gives such a run. 6 dB is plainly audible at once.

#pragma once

#include <cstddef>
#include <span>
#include <vector>

namespace hokuto::song
{

// A place where a render's level differs from the reference's: a span of 1 s (L1), or a run of windows (L2).
struct LevelPlace
{
    bool operator==(const LevelPlace&) const = default;

    double start = 0.0;   // seconds from the reference's start
    double seconds = 0.0; // the place's length
    double db = 0.0;      // the render's level less the reference's, less the gain: a span's median, or a run's mean
    bool run = false;     // a run of windows each more than 6 dB off, or else a span
};

// What the level measure gives.
struct LevelComparison
{
    double lag = 0.0;        // seconds by which the render comes after the reference (see CompareLevels)
    double gain_db = 0.0;    // the render's level less the reference's, over the counted windows
    std::size_t windows = 0; // the counted windows
    std::size_t within_1db = 0;
    std::size_t from_1_to_3db = 0;
    std::size_t beyond_3db = 0;
    double worst_db = 0.0; // the counted window that lies furthest off, and its start in seconds
    double worst_at = 0.0;
    std::vector<LevelPlace> places; // every finding, in the order of their starts
};

// The audibility method's thresholds.
inline constexpr double kWindowSeconds = 0.05;
inline constexpr double kSpanFindingDb = 1.0;
inline constexpr double kRunFindingDb = 6.0;

// Compares `render` with `reference`, both in interleaved stereo at `rate`. A conversion's timing can put the render's
// notes a few milliseconds away from the reference's. The measure therefore lines the render up with the reference
// first, by the lag within 100 ms either way at which their levels in windows of 1 ms agree best. Each render counts as
// silent before its start and after its end. The measure runs to the end of the shorter render after that. A render
// that stays silent throughout takes a gain of 0 dB, and every window where the reference sounds counts against it.
// Such a render lies below -70 dBFS in every window, and more than 50 dB below the reference's loudest window. Throws
// std::invalid_argument for a rate below 1,000 Hz.
LevelComparison CompareLevels(std::span<const float> reference, std::span<const float> render, int rate);

// The places of `comparison` that lie beyond `db` either way, the loudest difference first. A run counts beyond any
// threshold up to 6 dB.
std::vector<LevelPlace> WorstPlaces(const LevelComparison& comparison, double db, std::size_t most);

} // namespace hokuto::song
