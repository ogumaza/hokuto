// SPDX-License-Identifier: MIT

// Tests the level measure of the audibility method (src/song/audibility.h) on sines whose levels follow from their
// amplitudes. A render that only differs in gain gives no finding. A step of 2.5 dB over 2 s gives a span of 2.5 dB
// there. A missing loud note gives a run of 1 s at the note's level. A silent render counts against every window of the
// reference, and a quiet render that only plays quieter gives no finding. An offset that starts partway passes the
// high-pass and gives no finding. A render that comes 30 ms late, or 20 ms early, lines up again. So do two short
// renders whose notes lie apart by other times.

#include "song/audibility.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <stdexcept>
#include <vector>

#include "xsflib/testing/check.h"

using hokuto::song::CompareLevels;
using hokuto::song::LevelComparison;
using hokuto::song::LevelPlace;
using hokuto::song::WorstPlaces;

namespace
{

constexpr double kPi = 3.14159265358979323846;
constexpr int kRate = 48000;

// `seconds` of interleaved stereo silence.
std::vector<float> Silence(double seconds)
{
    return std::vector<float>(static_cast<std::size_t>(std::lround(seconds * kRate)) * 2, 0.0f);
}

// Adds a sine of `frequency` Hz and `amplitude` to both channels of `samples`, from `from` to `to` seconds.
void AddSine(std::vector<float>& samples, double frequency, double amplitude, double from, double to)
{
    const auto first = static_cast<std::size_t>(std::lround(from * kRate));
    const auto last = std::min(static_cast<std::size_t>(std::lround(to * kRate)), samples.size() / 2);
    for (std::size_t i = first; i < last; i++)
    {
        const auto sample =
            static_cast<float>(amplitude * std::sin(2.0 * kPi * frequency * static_cast<double>(i) / kRate));
        samples[i * 2] += sample;
        samples[i * 2 + 1] += sample;
    }
}

// Adds a sine of `frequency` Hz to both channels of `samples` from `from` to `to` seconds, at `amplitude` at first and
// falling by `db_per_second` dB each second.
void AddDecay(std::vector<float>& samples, double frequency, double amplitude, double from, double to,
              double db_per_second)
{
    const auto first = static_cast<std::size_t>(std::lround(from * kRate));
    const auto last = std::min(static_cast<std::size_t>(std::lround(to * kRate)), samples.size() / 2);
    for (std::size_t i = first; i < last; i++)
    {
        const double seconds = static_cast<double>(i - first) / kRate;
        const double level = amplitude * std::pow(10.0, -db_per_second * seconds / 20.0);
        const auto sample =
            static_cast<float>(level * std::sin(2.0 * kPi * frequency * static_cast<double>(i) / kRate));
        samples[i * 2] += sample;
        samples[i * 2 + 1] += sample;
    }
}

// `samples` times `factor` from `from` to `to` seconds.
std::vector<float> Scaled(std::vector<float> samples, double factor, double from, double to)
{
    const auto first = static_cast<std::size_t>(std::lround(from * kRate)) * 2;
    const auto last = std::min(static_cast<std::size_t>(std::lround(to * kRate)) * 2, samples.size());
    for (std::size_t i = first; i < last; i++)
    {
        samples[i] = static_cast<float>(samples[i] * factor);
    }

    return samples;
}

// `samples` moved later by `frames`, or earlier for a negative count, with silence in the gap.
std::vector<float> Moved(const std::vector<float>& samples, long frames)
{
    std::vector<float> moved(samples.size(), 0.0f);
    const auto count = static_cast<long>(samples.size() / 2);
    for (long i = 0; i < count; i++)
    {
        const long j = i + frames;
        if (j >= 0 && j < count)
        {
            moved[static_cast<std::size_t>(j) * 2] = samples[static_cast<std::size_t>(i) * 2];
            moved[static_cast<std::size_t>(j) * 2 + 1] = samples[static_cast<std::size_t>(i) * 2 + 1];
        }
    }

    return moved;
}

// 10 s of notes: a sine of 440 Hz that sounds for 0.2 s of every 0.5 s, at an amplitude of 0.25.
std::vector<float> Notes()
{
    std::vector<float> samples = Silence(10.0);
    for (double at = 0.0; at < 10.0; at += 0.5)
    {
        AddSine(samples, 440.0, 0.25, at, at + 0.2);
    }

    return samples;
}

// 10 s of a tremolo: a sine of 440 Hz at an amplitude of 0.25 for 25 ms and 0.05 for the next 25 ms, every 50 ms.
std::vector<float> Tremolo()
{
    std::vector<float> samples = Silence(10.0);
    for (double at = 0.0; at < 10.0; at += 0.05)
    {
        AddSine(samples, 440.0, 0.25, at, at + 0.025);
        AddSine(samples, 440.0, 0.05, at + 0.025, at + 0.05);
    }

    return samples;
}

// 10 s of a sine of 1 kHz at an amplitude of 0.25: -15 dBFS in power.
std::vector<float> Steady()
{
    std::vector<float> samples = Silence(10.0);
    AddSine(samples, 1000.0, 0.25, 0.0, 10.0);

    return samples;
}

// Whether `value` lies within `tolerance` of `expected`.
bool Near(double value, double expected, double tolerance)
{
    return std::abs(value - expected) <= tolerance;
}

// The same render, and the render at half its amplitude: no finding. Half the amplitude gives a gain of -6.02 dB. Every
// window counts and lies within 1 dB once the gain is taken off. Two silent renders give no window.
void TestGain()
{
    const std::vector<float> steady = Steady();
    const LevelComparison same = CompareLevels(steady, steady, kRate);
    const LevelComparison half = CompareLevels(steady, Scaled(steady, 0.5, 0.0, 10.0), kRate);
    const LevelComparison silent = CompareLevels(Silence(2.0), Silence(2.0), kRate);

    SEQLIB_CHECK_EQ(same.windows, std::size_t{200});
    SEQLIB_CHECK_EQ(same.within_1db, std::size_t{200});
    XSFLIB_CHECK(same.places.empty());
    XSFLIB_CHECK(same.gain_db == 0.0);
    XSFLIB_CHECK(same.lag == 0.0);
    XSFLIB_CHECK(Near(half.gain_db, 20.0 * std::log10(0.5), 0.01));
    SEQLIB_CHECK_EQ(half.within_1db, std::size_t{200});
    XSFLIB_CHECK(half.places.empty());
    SEQLIB_CHECK_EQ(silent.windows, std::size_t{0});
    XSFLIB_CHECK(silent.places.empty());
}

// A render that stays silent throughout takes no gain. Against the steady tone at -15.05 dBFS, each of the 200 windows
// counts, 50 dB off: the silence counts at 50 dB below the loudest window. The spans make one place of 10 s, and the
// windows make a run of 10 s. A quiet render isn't a silent render. A sine at -66 dBFS against the same sine at half
// its amplitude, -72.04 dBFS, takes a gain of 20 log10(0.5) = -6.02 dB. Every window then lies within 1 dB.
void TestSilentRender()
{
    const LevelComparison silent = CompareLevels(Steady(), Silence(10.0), kRate);
    std::vector<float> quiet = Silence(10.0);
    AddSine(quiet, 1000.0, std::sqrt(2.0) * std::pow(10.0, -66.0 / 20.0), 0.0, 10.0);
    const LevelComparison quieter = CompareLevels(quiet, Scaled(quiet, 0.5, 0.0, 10.0), kRate);

    XSFLIB_CHECK(silent.gain_db == 0.0);
    SEQLIB_CHECK_EQ(silent.windows, std::size_t{200});
    SEQLIB_CHECK_EQ(silent.beyond_3db, std::size_t{200});
    XSFLIB_CHECK(Near(silent.worst_db, -50.0, 0.01));
    SEQLIB_CHECK_EQ(silent.places.size(), std::size_t{2});
    const auto run = [](const LevelPlace& place)
    {
        return place.run;
    };
    SEQLIB_CHECK_EQ(std::count_if(silent.places.begin(), silent.places.end(), run), std::ptrdiff_t{1});
    for (const LevelPlace& place : silent.places)
    {
        XSFLIB_CHECK(Near(place.start, 0.0, 1e-9));
        XSFLIB_CHECK(Near(place.seconds, 10.0, 1e-9));
        XSFLIB_CHECK(Near(place.db, -50.0, 0.01));
    }
    XSFLIB_CHECK(Near(quieter.gain_db, 20.0 * std::log10(0.5), 0.01));
    SEQLIB_CHECK_EQ(quieter.windows, std::size_t{200});
    SEQLIB_CHECK_EQ(quieter.within_1db, std::size_t{200});
    XSFLIB_CHECK(quieter.places.empty());
}

// The render lies 2.5 dB higher from 4 s to 6 s. The spans of 1 s from 4 s, 4.5 s and 5 s lie wholly in it, with a
// median of 2.5 dB. Those from 3.5 s and 5.5 s lie half in it: their median is the mean of 0 and 2.5 dB, 1.25 dB. The
// five spans make one place from 3.5 s to 6.5 s. It lies as far off as the step. No window lies 6 dB off. The 40
// windows in the step lie from 1 to 3 dB off, and the worst lies in the step.
void TestSpan()
{
    const std::vector<float> steady = Steady();
    const LevelComparison step = CompareLevels(steady, Scaled(steady, std::pow(10.0, 2.5 / 20.0), 4.0, 6.0), kRate);

    XSFLIB_CHECK(step.lag == 0.0);
    XSFLIB_CHECK(Near(step.gain_db, 0.0, 0.01));
    SEQLIB_CHECK_EQ(step.places.size(), std::size_t{1});
    if (step.places.size() == 1)
    {
        XSFLIB_CHECK(!step.places[0].run);
        XSFLIB_CHECK(Near(step.places[0].start, 3.5, 1e-9));
        XSFLIB_CHECK(Near(step.places[0].seconds, 3.0, 1e-9));
        XSFLIB_CHECK(Near(step.places[0].db, 2.5, 0.02));
    }
    SEQLIB_CHECK_EQ(step.windows, std::size_t{200});
    SEQLIB_CHECK_EQ(step.from_1_to_3db, std::size_t{40});
    SEQLIB_CHECK_EQ(step.beyond_3db, std::size_t{0});
    XSFLIB_CHECK(Near(step.worst_db, 2.5, 0.05));
    XSFLIB_CHECK(step.worst_at >= 4.0 && step.worst_at < 6.0);
}

// The reference holds a note of 1 kHz from 3 s to 4 s, at 4 times the amplitude of a sine of 440 Hz under it. The
// render lacks the note. Over the note, the reference lies 10 log10(1 + 16) = 12.3 dB higher: a run of 20 windows. The
// spans from 2.5 s and 3.5 s lie half in the note, with a median of -6.15 dB. With the span from 3 s, they make one
// place from 2.5 s to 4.5 s. It lies as far off as the run.
void TestRun()
{
    std::vector<float> reference = Silence(8.0);
    AddSine(reference, 440.0, 0.1, 0.0, 8.0);
    std::vector<float> render = reference;
    AddSine(reference, 1000.0, 0.4, 3.0, 4.0);
    const LevelComparison missing = CompareLevels(reference, render, kRate);
    const double note = -10.0 * std::log10(17.0);

    XSFLIB_CHECK(Near(missing.gain_db, 0.0, 0.01));
    SEQLIB_CHECK_EQ(missing.places.size(), std::size_t{2});
    if (missing.places.size() == 2)
    {
        const LevelPlace& span = missing.places[0];
        const LevelPlace& run = missing.places[1];
        XSFLIB_CHECK(!span.run);
        XSFLIB_CHECK(Near(span.start, 2.5, 1e-9));
        XSFLIB_CHECK(Near(span.seconds, 2.0, 1e-9));
        XSFLIB_CHECK(Near(span.db, note, 0.05));
        XSFLIB_CHECK(run.run);
        XSFLIB_CHECK(Near(run.start, 3.0, 1e-9));
        XSFLIB_CHECK(Near(run.seconds, 1.0, 1e-9));
        XSFLIB_CHECK(Near(run.db, note, 0.05));
    }
    SEQLIB_CHECK_EQ(missing.beyond_3db, std::size_t{20});
    XSFLIB_CHECK(Near(missing.worst_db, note, 0.1));
    XSFLIB_CHECK(missing.worst_at >= 3.0 && missing.worst_at < 4.0);
}

// From 5 s on, the render holds an offset of 0.1 as well, such as a console whose output moves its offset with a note.
// The offset would add 10 log10((0.03125 + 0.01) / 0.03125) = 1.2 dB to each window from 5 s. The high-pass takes it
// out within a window of its start. No finding remains.
void TestOffset()
{
    const std::vector<float> steady = Steady();
    std::vector<float> offset = steady;
    for (std::size_t i = 5 * kRate * 2; i < offset.size(); i++)
    {
        offset[i] += 0.1f;
    }
    const LevelComparison comparison = CompareLevels(steady, offset, kRate);

    XSFLIB_CHECK(comparison.places.empty());
    XSFLIB_CHECK(Near(comparison.gain_db, 0.0, 0.01));
    SEQLIB_CHECK_EQ(comparison.beyond_3db, std::size_t{0});
}

// A render 30 ms late lines up again, and so does one 20 ms early. Lined up, they match the reference. The late
// render's first 30 ms are silent, and its last 30 ms lie past the reference's end. A render 150 ms late lines up by
// 100 ms at most. The rest of the lag puts 50 ms of each note in another window, more than 3 dB off. Those windows lie
// 50 dB off: the silence counts at 50 dB below the loudest window. A tremolo that repeats every 50 ms lines up at each
// of its periods as well. A render 10 ms late lines up at the nearest, 10 ms, not at 60 ms or at -40 ms.
void TestLag()
{
    const std::vector<float> notes = Notes();
    const LevelComparison late = CompareLevels(notes, Moved(notes, kRate * 30 / 1000), kRate);
    const LevelComparison early = CompareLevels(notes, Moved(notes, -kRate * 20 / 1000), kRate);
    const LevelComparison far = CompareLevels(notes, Moved(notes, kRate * 150 / 1000), kRate);
    const std::vector<float> tremolo = Tremolo();
    const LevelComparison periodic = CompareLevels(tremolo, Moved(tremolo, kRate * 10 / 1000), kRate);

    XSFLIB_CHECK(Near(late.lag, 0.030, 1e-9));
    XSFLIB_CHECK(late.places.empty());
    XSFLIB_CHECK(Near(late.gain_db, 0.0, 0.01));
    XSFLIB_CHECK(Near(early.lag, -0.020, 1e-9));
    XSFLIB_CHECK(early.places.empty());
    XSFLIB_CHECK(Near(far.lag, 0.1, 1e-9));
    SEQLIB_CHECK_EQ(far.beyond_3db, std::size_t{60});
    XSFLIB_CHECK(Near(std::abs(far.worst_db), 50.0, 0.1));
    XSFLIB_CHECK(Near(periodic.lag, 0.010, 1e-9));
}

// Two notes that decay, the second 68 ms after the first in the reference and 72 ms after it in the render, such as on
// two consoles whose frames fall at other times. The render starts 10 ms earlier and ends 90 ms in, partway through its
// second note. In dB, a decay falls in a straight line, and two such lines correlate fully at any lag. Over the windows
// where the curves overlap alone, a lag of 62 ms would line the reference's first note up with the render's second,
// with nothing else in the overlap. Each render counts as silent past its ends, and the first notes line up at
// -10 ms. Both windows of 50 ms then count.
void TestShortLag()
{
    std::vector<float> reference = Silence(0.125);
    AddDecay(reference, 1000.0, 0.316, 0.020, 0.125, 1500.0);
    AddDecay(reference, 1000.0, 0.0794, 0.088, 0.125, 1500.0);
    std::vector<float> render = Silence(0.090);
    AddDecay(render, 1000.0, 0.447, 0.010, 0.090, 1500.0);
    AddDecay(render, 1000.0, 0.0708, 0.082, 0.090, 1500.0);
    const LevelComparison comparison = CompareLevels(reference, render, kRate);

    XSFLIB_CHECK(Near(comparison.lag, -0.010, 1e-9));
    SEQLIB_CHECK_EQ(comparison.windows, std::size_t{2});
}

// WorstPlaces takes the places beyond a threshold, the furthest off first. A run counts up to 6 dB, whatever its mean.
void TestWorstPlaces()
{
    LevelComparison comparison;
    comparison.places = {
        {1.0, 1.0, 2.0, false}, {5.0, 1.5, -4.0, false}, {8.0, 0.2, 6.5, true}, {9.0, 0.3, -0.5, true}};

    const std::vector<LevelPlace> beyond_3 = WorstPlaces(comparison, 3.0, 2);
    SEQLIB_CHECK_EQ(beyond_3.size(), std::size_t{2});
    if (beyond_3.size() == 2)
    {
        XSFLIB_CHECK(beyond_3[0] == comparison.places[2]);
        XSFLIB_CHECK(beyond_3[1] == comparison.places[1]);
    }
    const std::vector<LevelPlace> beyond_1 = WorstPlaces(comparison, 1.0, 10);
    SEQLIB_CHECK_EQ(beyond_1.size(), std::size_t{4});
    if (beyond_1.size() == 4)
    {
        XSFLIB_CHECK(beyond_1[2] == comparison.places[0]);
        XSFLIB_CHECK(beyond_1[3] == comparison.places[3]);
    }
    XSFLIB_CHECK(WorstPlaces(comparison, 7.0, 10).empty());
}

// A rate below 1,000 Hz is refused.
void TestRefusal()
{
    bool refused = false;
    try
    {
        CompareLevels(Silence(1.0), Silence(1.0), 999);
    }
    catch (const std::invalid_argument&)
    {
        refused = true;
    }

    XSFLIB_CHECK(refused);
}

} // namespace

int main()
{
    TestGain();
    TestSilentRender();
    TestSpan();
    TestRun();
    TestOffset();
    TestLag();
    TestShortLag();
    TestWorstPlaces();
    TestRefusal();

    return xsflib::testing::Summary("audibility");
}
