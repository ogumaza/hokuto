// SPDX-License-Identifier: MIT

// Tests the renderer of MIDI files through SoundFont 2 files (src/synth/sf2_synth.h). Each test builds a SoundFont
// around a sine and a MIDI file, renders them, and measures the render. The expected values come from the formulas of
// the SoundFont 2.04 specification and the players' conventions that the renderer's header lists. The tests cover the
// pitch from the key, the root key and the tunings, and the level from the attenuation at 0.4 and the default
// modulators' concave curve. They also cover the envelope's times in timecents, the loops, the sin/cos pan law, the
// exclusive classes, the pitch wheel, the sustain pedal, the drums of channel 10 and the low-pass filter. The last
// tests cover the times that the tempo changes give the events, the progress, the limit of a render's length and the
// files and values that the renderer refuses.

#include "synth/sf2_synth.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

#include "seqlib/midi/builder.h"
#include "seqlib/sf2/soundfont.h"
#include "seqlib/sf2/writer.h"
#include "xsflib/testing/check.h"

namespace
{

namespace sf2 = seqlib::sf2;
namespace midi = seqlib::midi;
using hokuto::synth::RenderSoundFont;
using hokuto::synth::StereoRender;
using G = sf2::Generator;

constexpr double kPi = 3.14159265358979323846;
constexpr int kRate = 48000;

// The sine's period in points, and its frequency at its original key when it plays at 48 kHz.
constexpr int kPeriod = 100;
constexpr double kSineHz = 480.0;

// The level of a side of the render of the sine at full level and at the centre: its amplitude of 0.5, times the
// sin/cos law's 0.7071 at the centre, over the square root of 2.
constexpr double kFullLevel = 0.25;

// The level of a side of the render of the sine at full level, panned to that side.
constexpr double kSideLevel = 0.35355339059327373;

// The samples of a voice's default delay and attack: -12000 timecents, 0.98 ms each.
constexpr std::size_t kDefaultDelay = 47;

// The longest render of the tests, in seconds.
constexpr double kMostSeconds = 60.0;

// The MIDI files' ticks: 960 a second, from 480 a quarter note at 120 beats a minute.
constexpr uint32_t kTicksPerSecond = 960;

// The MIDI files' ticks of `seconds`.
constexpr uint32_t Ticks(double seconds)
{
    return static_cast<uint32_t>(seconds * kTicksPerSecond);
}

// A sine of 48 periods, with its original key at 60. Its loop holds the 46 periods from point 100 to point 4700.
sf2::Sample Sine(uint32_t rate = kRate)
{
    sf2::Sample sample;
    sample.name = "Sine";
    sample.sample_rate = rate;
    sample.original_key = 60;
    sample.loop_start = kPeriod;
    sample.loop_end = 47 * kPeriod;
    for (int i = 0; i < 48 * kPeriod; i++)
    {
        sample.data.push_back(static_cast<int16_t>(std::lround(16384.0 * std::sin(2.0 * kPi * i / kPeriod))));
    }

    return sample;
}

// An instrument zone that plays sample `sample` with `generators`.
sf2::Zone SampleZone(std::vector<G> generators, int sample = 0)
{
    generators.push_back(G::Value(sf2::kSampleId, sample));

    return {std::move(generators), {}};
}

// A preset zone that plays instrument `instrument` with `generators`.
sf2::Zone InstrumentZone(int instrument, std::vector<G> generators = {})
{
    generators.push_back(G::Value(sf2::kInstrument, instrument));

    return {std::move(generators), {}};
}

// A SoundFont whose preset 0 of bank 0 plays an instrument with one zone. The zone plays the sine in sample mode `mode`
// with `generators`.
sf2::SoundFont OneZone(std::vector<G> generators, int mode = 1)
{
    generators.insert(generators.begin(), G::Value(sf2::kSampleModes, mode));
    sf2::SoundFont font;
    font.samples.push_back(Sine());
    font.instruments.push_back({"Sine", {SampleZone(std::move(generators))}});
    font.presets.push_back({"Sine", 0, 0, {InstrumentZone(0)}});

    return font;
}

// `font` with seqlib's global modulators in each instrument's global zone. The converters put them there too. The
// modulators turn the default modulator of velocity to the filter's cutoff off. They give CC10 500 tenths of a percent.
sf2::SoundFont Converted(sf2::SoundFont font)
{
    const std::vector<sf2::Modulator> modulators(sf2::kGlobalZoneModulators.begin(), sf2::kGlobalZoneModulators.end());
    for (sf2::Instrument& instrument : font.instruments)
    {
        instrument.zones.insert(instrument.zones.begin(), sf2::Zone{{}, modulators});
    }

    return font;
}

// A MIDI file of one track at 120 beats a minute. `add` adds the events.
std::vector<uint8_t> Midi(const std::function<void(midi::TrackBuilder&)>& add)
{
    midi::FileBuilder file(480);
    midi::TrackBuilder& track = file.AddTrack();
    track.Tempo(0, 500000);
    add(track);

    return file.Encode();
}

// A note of key `key` on channel 1 with CC7 at 127, from 0 to `seconds`.
std::vector<uint8_t> Note(int key, double seconds, int velocity = 127)
{
    const auto events = [&](midi::TrackBuilder& track)
    {
        track.Control(0, 0, 7, 127);
        track.NoteOn(0, 0, key, velocity);
        track.NoteOff(Ticks(seconds), 0, key);
    };
    return Midi(events);
}

// `midi_file` rendered through `font` at kRate, for kMostSeconds at most.
StereoRender Render(const std::vector<uint8_t>& midi_file, const sf2::SoundFont& font)
{
    return RenderSoundFont(midi_file, sf2::Write(font), kRate, kMostSeconds);
}

// The RMS level of side `side` of `render`, 0 for left and 1 for right, from `from` to `to` seconds. The span shrinks
// to whole periods of the sine at its original key.
double Level(const StereoRender& render, int side, double from, double to)
{
    const auto first = static_cast<std::size_t>(std::lround(from * render.rate));
    const auto last = static_cast<std::size_t>(std::lround(to * render.rate));
    const std::size_t count = std::max<std::size_t>(1, (last - first) / kPeriod * kPeriod);
    double sum = 0.0;
    for (std::size_t i = first; i < first + count; i++)
    {
        const std::size_t index = 2 * i + static_cast<std::size_t>(side);
        const double sample = index < render.samples.size() ? render.samples[index] : 0.0;
        sum += sample * sample;
    }

    return std::sqrt(sum / static_cast<double>(count));
}

// `level` in decibels relative to `reference`.
double Decibels(double level, double reference)
{
    return 20.0 * std::log10(level / reference);
}

// The level of the left side from `from` to `to` seconds, in decibels relative to the sine at full level.
double LeftDecibels(const StereoRender& render, double from, double to)
{
    return Decibels(Level(render, 0, from, to), kFullLevel);
}

// The level of the left side over the two periods around `seconds`, in decibels relative to the sine at full level.
double Around(const StereoRender& render, double seconds)
{
    const double half = static_cast<double>(kPeriod) / render.rate;
    return LeftDecibels(render, seconds - half, seconds + half);
}

// The first sample of the left side that isn't 0.
std::size_t FirstSound(const StereoRender& render)
{
    std::size_t first = 0;
    while (2 * first < render.samples.size() && render.samples[2 * first] == 0.0f)
    {
        first++;
    }

    return first;
}

// One past the last sample of the left side that isn't 0.
std::size_t LastSound(const StereoRender& render)
{
    std::size_t end = render.samples.size() / 2;
    while (end > 0 && render.samples[2 * (end - 1)] == 0.0f)
    {
        end--;
    }

    return end;
}

// The frequency of the left side from `from` to `to` seconds, from the times of its upward zero crossings. Each time
// comes from a straight line between the samples around the crossing.
double Frequency(const StereoRender& render, double from, double to)
{
    const auto first = static_cast<std::size_t>(from * render.rate);
    const auto last = static_cast<std::size_t>(to * render.rate);
    std::optional<double> first_crossing;
    double last_crossing = 0.0;
    int periods = 0;
    for (std::size_t i = first + 1; i < last && 2 * i < render.samples.size(); i++)
    {
        const double before = render.samples[2 * (i - 1)];
        const double after = render.samples[2 * i];
        if (before < 0.0 && after >= 0.0)
        {
            const double crossing = static_cast<double>(i - 1) + before / (before - after);
            if (!first_crossing)
            {
                first_crossing = crossing;
            }
            else
            {
                last_crossing = crossing;
                periods++;
            }
        }
    }
    if (!first_crossing || periods == 0)
    {
        return 0.0;
    }

    return periods * render.rate / (last_crossing - *first_crossing);
}

// The interval from `base` Hz to the left side's frequency, in cents.
double Cents(const StereoRender& render, double from, double to, double base = kSineHz)
{
    return 1200.0 * std::log2(Frequency(render, from, to) / base);
}

// Whether `a` and `b` differ by `tolerance` or less.
bool Near(double a, double b, double tolerance)
{
    return std::abs(a - b) <= tolerance;
}

// The pitch follows the key, the root key, the tunings and the sample's rate (sections 7.10 and 8.1.2).
void TestPitch()
{
    // The key against the root key, and the overriding root key.
    XSFLIB_CHECK(Near(Cents(Render(Note(60, 1.0), OneZone({})), 0.2, 0.8), 0.0, 0.05));
    XSFLIB_CHECK(Near(Cents(Render(Note(67, 1.0), OneZone({})), 0.2, 0.8), 700.0, 0.05));
    XSFLIB_CHECK(
        Near(Cents(Render(Note(60, 1.0), OneZone({G::Value(sf2::kOverridingRootKey, 48)})), 0.2, 0.8), 1200.0, 0.05));

    // The coarse and fine tunings add to the key's interval.
    const sf2::SoundFont tuned = OneZone({G::Value(sf2::kCoarseTune, 2), G::Value(sf2::kFineTune, -37)});
    XSFLIB_CHECK(Near(Cents(Render(Note(64, 1.0), tuned), 0.2, 0.8), 563.0, 0.05));

    // scaleTuning sets the cents of each key: 50 a key gives 600 over 12 keys.
    const sf2::SoundFont scaled = OneZone({G::Value(sf2::kScaleTuning, 50)});
    XSFLIB_CHECK(Near(Cents(Render(Note(72, 1.0), scaled), 0.2, 0.8), 600.0, 0.05));

    // keynum plays every key as key 72.
    XSFLIB_CHECK(Near(Cents(Render(Note(60, 1.0), OneZone({G::Value(sf2::kKeynum, 72)})), 0.2, 0.8), 1200.0, 0.05));

    // A sample at 32 kHz plays its 320 Hz at its original key.
    sf2::SoundFont slow = OneZone({});
    slow.samples[0] = Sine(32000);
    XSFLIB_CHECK(Near(Cents(Render(Note(60, 1.0), slow), 0.2, 0.8, 320.0), 0.0, 0.05));

    // The sample's pitch correction, -13 cents in byte 41 of its header.
    std::vector<uint8_t> corrected = sf2::Write(OneZone({}));
    for (std::size_t i = 0; i + 8 + 41 < corrected.size(); i++)
    {
        if (corrected[i] == 's' && corrected[i + 1] == 'h' && corrected[i + 2] == 'd' && corrected[i + 3] == 'r')
        {
            corrected[i + 8 + 41] = static_cast<uint8_t>(256 - 13);
            break;
        }
    }
    XSFLIB_CHECK(Near(Cents(RenderSoundFont(Note(60, 1.0), corrected, kRate, kMostSeconds), 0.2, 0.8), -13.0, 0.05));
}

// The pitch wheel bends by its share of the bend range. RPN 0 sets the bend range. RPNs 1 and 2 tune the channel.
void TestPitchWheel()
{
    // Half the wheel's range up with a bend range of 12 semitones: 600 cents.
    const auto bent_events = [](midi::TrackBuilder& track)
    {
        track.BendRange(0, 0, 12);
        track.PitchBend(0, 0, 8192 + 4096);
        track.NoteOn(0, 0, 60, 127);
        track.NoteOff(Ticks(1.0), 0, 60);
    };
    const std::vector<uint8_t> bent = Midi(bent_events);
    XSFLIB_CHECK(Near(Cents(Render(bent, OneZone({})), 0.2, 0.8), 600.0, 0.05));

    // While the note sounds, the wheel goes to its bottom. The bend range stays at its default of 2 semitones.
    const auto down_events = [](midi::TrackBuilder& track)
    {
        track.NoteOn(0, 0, 60, 127);
        track.PitchBend(Ticks(0.5), 0, 0);
        track.NoteOff(Ticks(1.0), 0, 60);
    };
    const std::vector<uint8_t> down = Midi(down_events);
    const StereoRender down_render = Render(down, OneZone({}));
    XSFLIB_CHECK(Near(Cents(down_render, 0.1, 0.45), 0.0, 0.05));
    XSFLIB_CHECK(Near(Cents(down_render, 0.55, 0.95), -200.0, 0.05));

    // RPN 2 tunes by 3 semitones. RPN 1 tunes up by a quarter of its range: 50 cents.
    const auto tuned_events = [](midi::TrackBuilder& track)
    {
        track.Control(0, 0, 101, 0);
        track.Control(0, 0, 100, 2);
        track.Control(0, 0, 6, 64 + 3);
        track.Control(0, 0, 100, 1);
        track.Control(0, 0, 6, (8192 + 4096) >> 7);
        track.Control(0, 0, 38, 0);
        track.NoteOn(0, 0, 60, 127);
        track.NoteOff(Ticks(1.0), 0, 60);
    };
    const std::vector<uint8_t> tuned = Midi(tuned_events);
    XSFLIB_CHECK(Near(Cents(Render(tuned, OneZone({})), 0.2, 0.8), 350.0, 0.05));
}

// The level follows the attenuation at 0.4 of the generator. It also follows the default modulators of velocity, CC7
// and CC11: 40 log10(127 / v) dB for a value v.
void TestLevels()
{
    XSFLIB_CHECK(Near(LeftDecibels(Render(Note(60, 1.0), OneZone({})), 0.4, 0.8), 0.0, 0.01));

    // 250 cB in the instrument's zone attenuates by 100 cB. A preset zone's 100 cB adds to it.
    const sf2::SoundFont attenuated = OneZone({G::Value(sf2::kInitialAttenuation, 250)});
    XSFLIB_CHECK(Near(LeftDecibels(Render(Note(60, 1.0), attenuated), 0.4, 0.8), -10.0, 0.01));
    sf2::SoundFont added = attenuated;
    added.presets[0].zones[0] = InstrumentZone(0, {G::Value(sf2::kInitialAttenuation, 100)});
    XSFLIB_CHECK(Near(LeftDecibels(Render(Note(60, 1.0), added), 0.4, 0.8), -14.0, 0.01));

    // The instrument's global zone gives a value that its local zone leaves out. A value that the local zone sets
    // overrides the global one.
    sf2::SoundFont global = OneZone({});
    global.instruments[0].zones.insert(global.instruments[0].zones.begin(),
                                       sf2::Zone{{G::Value(sf2::kInitialAttenuation, 250)}, {}});
    XSFLIB_CHECK(Near(LeftDecibels(Render(Note(60, 1.0), global), 0.4, 0.8), -10.0, 0.01));
    global.instruments[0].zones[1].generators.insert(global.instruments[0].zones[1].generators.begin(),
                                                     G::Value(sf2::kInitialAttenuation, 100));
    XSFLIB_CHECK(Near(LeftDecibels(Render(Note(60, 1.0), global), 0.4, 0.8), -4.0, 0.01));

    // Velocity.
    XSFLIB_CHECK(
        Near(LeftDecibels(Render(Note(60, 1.0, 64), OneZone({})), 0.4, 0.8), -40.0 * std::log10(127.0 / 64.0), 0.01));
    XSFLIB_CHECK(
        Near(LeftDecibels(Render(Note(60, 1.0, 100), OneZone({})), 0.4, 0.8), -40.0 * std::log10(127.0 / 100.0), 0.01));

    // CC7 starts at General MIDI's default of 100. While the note sounds, CC7 goes to 64 at 0.5 s and CC11 at 1 s.
    const auto controlled_events = [](midi::TrackBuilder& track)
    {
        track.NoteOn(0, 0, 60, 127);
        track.Control(Ticks(0.5), 0, 7, 64);
        track.Control(Ticks(1.0), 0, 11, 64);
        track.NoteOff(Ticks(1.5), 0, 60);
    };
    const std::vector<uint8_t> controlled = Midi(controlled_events);
    const StereoRender render = Render(controlled, OneZone({}));
    XSFLIB_CHECK(Near(LeftDecibels(render, 0.1, 0.45), -40.0 * std::log10(127.0 / 100.0), 0.01));
    XSFLIB_CHECK(Near(LeftDecibels(render, 0.6, 0.95), -40.0 * std::log10(127.0 / 64.0), 0.01));
    XSFLIB_CHECK(Near(LeftDecibels(render, 1.1, 1.45), -80.0 * std::log10(127.0 / 64.0), 0.01));
}

// A modulator of the instrument replaces an identical default modulator. An identical modulator of the preset adds to
// it (section 9.5.1).
void TestModulators()
{
    const auto half_volume_events = [](midi::TrackBuilder& track)
    {
        track.Control(0, 0, 7, 64);
        track.NoteOn(0, 0, 60, 127);
        track.NoteOff(Ticks(1.0), 0, 60);
    };
    const std::vector<uint8_t> half_volume = Midi(half_volume_events);
    const double kCc7At64 = -40.0 * std::log10(127.0 / 64.0);
    const sf2::Modulator kCc7 = {sf2::kController | sf2::kNegative | sf2::kConcave | 7, sf2::kInitialAttenuation, 480,
                                 sf2::kNoController, sf2::kLinear};

    sf2::SoundFont replaced = OneZone({});
    replaced.instruments[0].zones.insert(replaced.instruments[0].zones.begin(), sf2::Zone{{}, {kCc7}});
    XSFLIB_CHECK(Near(LeftDecibels(Render(half_volume, replaced), 0.4, 0.8), kCc7At64 / 2.0, 0.01));

    sf2::SoundFont doubled = OneZone({});
    doubled.presets[0].zones.insert(doubled.presets[0].zones.begin(), sf2::Zone{{}, {kCc7}});
    XSFLIB_CHECK(Near(LeftDecibels(Render(half_volume, doubled), 0.4, 0.8), kCc7At64 * 1.5, 0.01));
}

// The pan generator and CC10 place the sound by the sin/cos law. The default CC10 modulator gives CC10 1000 tenths of a
// percent. The global modulator of seqlib gives it 500.
void TestPan()
{
    const auto sides = [](const StereoRender& render)
    {
        return std::pair{Level(render, 0, 0.4, 0.8), Level(render, 1, 0.4, 0.8)};
    };

    const auto [left, right] = sides(Render(Note(60, 1.0), OneZone({G::Value(sf2::kPan, -250)})));
    XSFLIB_CHECK(Near(left, kSideLevel * std::cos(kPi / 8.0), 1e-4));
    XSFLIB_CHECK(Near(right, kSideLevel * std::sin(kPi / 8.0), 1e-4));

    const auto [hard_left, silent_right] = sides(Render(Note(60, 1.0), OneZone({G::Value(sf2::kPan, -500)})));
    XSFLIB_CHECK(Near(hard_left, kSideLevel, 1e-4));
    XSFLIB_CHECK(silent_right == 0.0);

    // CC10 at 32 lies half the way to the left.
    const auto panned_events = [](midi::TrackBuilder& track)
    {
        track.Control(0, 0, 7, 127);
        track.Control(0, 0, 10, 32);
        track.NoteOn(0, 0, 60, 127);
        track.NoteOff(Ticks(1.0), 0, 60);
    };
    const std::vector<uint8_t> panned = Midi(panned_events);
    const auto [default_left, default_right] = sides(Render(panned, OneZone({})));
    XSFLIB_CHECK(Near(default_left, kSideLevel, 1e-4));
    XSFLIB_CHECK(default_right < 1e-6);
    const auto [half_left, half_right] = sides(Render(panned, Converted(OneZone({}))));
    XSFLIB_CHECK(Near(half_left, kSideLevel * std::cos(kPi / 8.0), 1e-4));
    XSFLIB_CHECK(Near(half_right, kSideLevel * std::sin(kPi / 8.0), 1e-4));
}

// The volume envelope's delay, attack, hold, decay, sustain and release (section 8.1.2, generators 33 to 40).
void TestEnvelope()
{
    // A delay of 0.1 s, an attack of 0.2 s, a hold of 0.1 s, a decay of 1 s to a sustain of 30 dB and a release of 0.5
    // s. The attack rises in amplitude. The decay and the release fall 100 dB in their times.
    const sf2::SoundFont font = OneZone({
        G::Value(sf2::kDelayVolEnv, -3986),
        G::Value(sf2::kAttackVolEnv, -2786),
        G::Value(sf2::kHoldVolEnv, -3986),
        G::Value(sf2::kDecayVolEnv, 0),
        G::Value(sf2::kSustainVolEnv, 300),
        G::Value(sf2::kReleaseVolEnv, -1200),
    });
    const StereoRender render = Render(Note(60, 1.0), font);
    XSFLIB_CHECK(FirstSound(render) == static_cast<std::size_t>(std::lround(std::exp2(-3986.0 / 1200.0) * kRate)) + 1);
    XSFLIB_CHECK(Near(Around(render, 0.2), Decibels(0.5, 1.0), 0.02));
    XSFLIB_CHECK(Near(Around(render, 0.35), 0.0, 0.01));
    XSFLIB_CHECK(Near(Around(render, 0.55), -15.0, 0.05));
    XSFLIB_CHECK(Near(Around(render, 0.85), -30.0, 0.01));
    XSFLIB_CHECK(Near(Around(render, 1.05), -40.0, 0.05));
    XSFLIB_CHECK(Near(Around(render, 1.25), -80.0, 0.05));

    // The release ends the voice 100 dB down, 0.35 s after the note-off.
    XSFLIB_CHECK(Near(static_cast<double>(LastSound(render)) / kRate, 1.35, 0.001));

    // keynumToVolEnvHold and keynumToVolEnvDecay scale the hold and the decay by key: 100 timecents a key from key 60
    // halves both at key 72. The hold of 0.25 s ends 0.252 s in, after the default delay and attack. The decay then
    // falls 200 dB a second.
    const sf2::SoundFont scaled = OneZone({
        G::Value(sf2::kHoldVolEnv, -1200),
        G::Value(sf2::kKeynumToVolEnvHold, 100),
        G::Value(sf2::kDecayVolEnv, 0),
        G::Value(sf2::kKeynumToVolEnvDecay, 100),
        G::Value(sf2::kSustainVolEnv, 300),
    });
    const StereoRender keyed = Render(Note(72, 1.0), scaled);
    const double kDecayStart = static_cast<double>(2 * kDefaultDelay + kRate / 4) / kRate;
    XSFLIB_CHECK(Near(Around(keyed, 0.2), 0.0, 0.01));
    XSFLIB_CHECK(Near(Around(keyed, 0.325), -200.0 * (0.325 - kDecayStart), 0.05));
    XSFLIB_CHECK(Near(Around(keyed, 0.6), -30.0, 0.01));
}

// A sample without a loop ends at its last point. A loop repeats while the note sounds. A loop until release plays on
// to the sample's end after the note-off (section 8.1.2, generator 54).
void TestLoops()
{
    const std::vector<uint8_t> note = Note(60, 0.5);
    const std::vector<G> kLongRelease = {G::Value(sf2::kReleaseVolEnv, 1200)};

    // The sample starts after the default delay. Its 4800 points then last 0.1 s.
    const StereoRender once = Render(note, OneZone(kLongRelease, 0));
    XSFLIB_CHECK(Near(LeftDecibels(once, 0.02, 0.09), 0.0, 0.01));
    XSFLIB_CHECK(LastSound(once) == kDefaultDelay + 48 * kPeriod);

    // The release of 2 s falls 50 dB a second from the note-off.
    const StereoRender looped = Render(note, OneZone(kLongRelease, 1));
    XSFLIB_CHECK(Near(LeftDecibels(looped, 0.02, 0.48), 0.0, 0.01));
    XSFLIB_CHECK(Near(Around(looped, 0.7), -10.0, 0.05));

    const StereoRender until_release = Render(note, OneZone(kLongRelease, 3));
    XSFLIB_CHECK(Near(LeftDecibels(until_release, 0.02, 0.48), 0.0, 0.01));
    XSFLIB_CHECK(LastSound(until_release) < static_cast<std::size_t>(0.6 * kRate));
    XSFLIB_CHECK(LastSound(until_release) > static_cast<std::size_t>(0.5 * kRate));

    // The address offsets move the sample's start and end.
    const StereoRender later = Render(note, OneZone({G::Value(sf2::kStartAddrsOffset, 25)}, 0));
    XSFLIB_CHECK(LastSound(later) == kDefaultDelay + 48 * kPeriod - 25);
    const StereoRender sooner = Render(note, OneZone({G::Value(sf2::kEndAddrsOffset, -100)}, 0));
    XSFLIB_CHECK(LastSound(sooner) == kDefaultDelay + 47 * kPeriod);
}

// A note of an exclusive class ends the other notes of the class that its preset plays on its channel. Those notes fall
// silent within 16 ms.
void TestExclusiveClass()
{
    // Key 60 plays on the left and key 62 on the right. Each has a release of 2 s. Presets 0 and 1 play them.
    const auto font = [](int exclusive_class)
    {
        sf2::SoundFont result;
        result.samples.push_back(Sine());
        const std::vector<G> common = {G::Value(sf2::kSampleModes, 1), G::Value(sf2::kReleaseVolEnv, 1200),
                                       G::Value(sf2::kExclusiveClass, exclusive_class)};
        std::vector<G> left = common;
        left.insert(left.begin(), G::Range(sf2::kKeyRange, 60, 60));
        left.push_back(G::Value(sf2::kPan, -500));
        std::vector<G> right = common;
        right.insert(right.begin(), G::Range(sf2::kKeyRange, 62, 62));
        right.push_back(G::Value(sf2::kPan, 500));
        result.instruments.push_back({"Pair", {SampleZone(left), SampleZone(right)}});
        result.presets.push_back({"Pair", 0, 0, {InstrumentZone(0)}});
        result.presets.push_back({"Pair again", 1, 0, {InstrumentZone(0)}});

        return result;
    };

    // Key 60 plays on channel 1 from the start. Key 62 follows at 0.5 s on channel `channel` with program `program`.
    const auto pair = [](int channel, int program)
    {
        const auto events = [&](midi::TrackBuilder& track)
        {
            track.Control(0, 0, 7, 127);
            track.Control(0, channel, 7, 127);
            track.NoteOn(0, 0, 60, 127);
            track.Program(Ticks(0.25), channel, program);
            track.NoteOn(Ticks(0.5), channel, 62, 127);
            track.NoteOff(Ticks(1.0), 0, 60);
            track.NoteOff(Ticks(1.0), channel, 62);
        };
        return Midi(events);
    };

    const double kSide = Decibels(kSideLevel, kFullLevel);

    const StereoRender cut = Render(pair(0, 0), font(1));
    XSFLIB_CHECK(Near(LeftDecibels(cut, 0.3, 0.49), kSide, 0.01));
    XSFLIB_CHECK(LastSound(cut) > static_cast<std::size_t>(0.5 * kRate));
    XSFLIB_CHECK(LastSound(cut) < static_cast<std::size_t>(0.517 * kRate));
    XSFLIB_CHECK(Near(Decibels(Level(cut, 1, 0.6, 0.9), kFullLevel), kSide, 0.01));

    // Without the class, on another channel, and with another preset, the first note sounds on.
    XSFLIB_CHECK(Near(LeftDecibels(Render(pair(0, 0), font(0)), 0.6, 0.9), kSide, 0.01));
    XSFLIB_CHECK(Near(LeftDecibels(Render(pair(1, 0), font(1)), 0.6, 0.9), kSide, 0.01));
    XSFLIB_CHECK(Near(LeftDecibels(Render(pair(0, 1), font(1)), 0.6, 0.9), kSide, 0.01));
}

// The sustain pedal holds a note past its note-off until the pedal goes up.
void TestSustainPedal()
{
    const auto held_events = [](midi::TrackBuilder& track)
    {
        track.Control(0, 0, 7, 127);
        track.Control(0, 0, 64, 127);
        track.NoteOn(0, 0, 60, 127);
        track.NoteOff(Ticks(0.25), 0, 60);
        track.Control(Ticks(0.75), 0, 64, 0);
        track.SetEnd(Ticks(1.0));
    };
    const std::vector<uint8_t> held = Midi(held_events);
    const StereoRender render = Render(held, OneZone({}));
    XSFLIB_CHECK(Near(LeftDecibels(render, 0.3, 0.7), 0.0, 0.01));
    XSFLIB_CHECK(LastSound(render) > static_cast<std::size_t>(0.75 * kRate));
    XSFLIB_CHECK(LastSound(render) < static_cast<std::size_t>(0.76 * kRate));
}

// MIDI channel 10 plays the presets of bank 128, whatever its bank select. Other channels play bank 0's.
void TestDrums()
{
    sf2::SoundFont font;
    font.samples.push_back(Sine());
    font.instruments.push_back({"Left", {SampleZone({G::Value(sf2::kSampleModes, 1), G::Value(sf2::kPan, -500)})}});
    font.instruments.push_back({"Right", {SampleZone({G::Value(sf2::kSampleModes, 1), G::Value(sf2::kPan, 500)})}});
    font.presets.push_back({"Melodic", 0, 0, {InstrumentZone(1)}});
    font.presets.push_back({"Drums", 0, 128, {InstrumentZone(0)}});

    const auto on_channel = [](int channel)
    {
        const auto events = [&](midi::TrackBuilder& track)
        {
            track.Control(0, channel, 7, 127);
            track.Bank(0, channel, 0);
            track.Program(0, channel, 0);
            track.NoteOn(0, channel, 60, 127);
            track.NoteOff(Ticks(1.0), channel, 60);
        };
        return Midi(events);
    };

    const StereoRender drums = Render(on_channel(9), font);
    XSFLIB_CHECK(Near(Level(drums, 0, 0.2, 0.8), kSideLevel, 1e-4));
    XSFLIB_CHECK(Level(drums, 1, 0.2, 0.8) == 0.0);
    const StereoRender melodic = Render(on_channel(0), font);
    XSFLIB_CHECK(Level(melodic, 0, 0.2, 0.8) == 0.0);
    XSFLIB_CHECK(Near(Level(melodic, 1, 0.2, 0.8), kSideLevel, 1e-4));
}

// The low-pass filter peaks at its resonance less 3.01 dB at the cutoff. Its passband falls by half the resonance
// (section 8.1.2, generators 8 and 9). The fonts turn the default modulator of velocity to the cutoff off.
void TestFilter()
{
    const sf2::SoundFont resonant = Converted(OneZone({G::Value(sf2::kInitialFilterQ, 200)}));
    XSFLIB_CHECK(Near(LeftDecibels(Render(Note(60, 1.0), resonant), 0.4, 0.8), -10.0, 0.02));

    // 7051 absolute cents lies within 0.4 cents of 480 Hz, the sine's frequency.
    const sf2::SoundFont at_sine = Converted(OneZone({G::Value(sf2::kInitialFilterFc, 7051)}));
    XSFLIB_CHECK(Near(LeftDecibels(Render(Note(60, 1.0), at_sine), 0.4, 0.8), -3.01, 0.02));
    const sf2::SoundFont peak =
        Converted(OneZone({G::Value(sf2::kInitialFilterFc, 7051), G::Value(sf2::kInitialFilterQ, 200)}));
    XSFLIB_CHECK(Near(LeftDecibels(Render(Note(60, 1.0), peak), 0.4, 0.8), 20.0 - 3.01 - 10.0, 0.02));

    // Two octaves above the cutoff, the filter without resonance cuts 24 dB.
    const sf2::SoundFont below = Converted(OneZone({G::Value(sf2::kInitialFilterFc, 7051 - 2400)}));
    XSFLIB_CHECK(Near(LeftDecibels(Render(Note(60, 1.0), below), 0.4, 0.8), -10.0 * std::log10(1.0 + 256.0), 0.05));
}

// The tempo changes set the times of the events.
void TestTiming()
{
    const auto slower_events = [](midi::TrackBuilder& track)
    {
        track.Tempo(480, 1000000);
        track.NoteOn(960, 0, 60, 127);
        track.NoteOff(1440, 0, 60);
    };
    const std::vector<uint8_t> slower = Midi(slower_events);

    // The tempo halves after 0.5 s. Tick 960 then comes at 1.5 s. The sound starts a sample into the attack.
    const StereoRender render = Render(slower, OneZone({}));
    XSFLIB_CHECK(FirstSound(render) == static_cast<std::size_t>(1.5 * kRate) + kDefaultDelay + 1);
}

// The progress gets shares of the render from 0 to 1. A false return stops the render.
void TestProgress()
{
    const std::vector<uint8_t> midi_file = Note(60, 2.0);
    const std::vector<uint8_t> font = sf2::Write(OneZone({}));

    std::vector<double> shares;
    const auto track = [&](double share)
    {
        shares.push_back(share);

        return true;
    };
    const StereoRender whole = RenderSoundFont(midi_file, font, kRate, kMostSeconds, track);
    SEQLIB_REQUIRE(shares.size() >= 4);

    bool rising = true;
    for (std::size_t i = 1; i < shares.size(); i++)
    {
        rising = rising && shares[i] >= shares[i - 1];
    }
    XSFLIB_CHECK(rising && shares.front() > 0.0 && shares.back() <= 1.0);

    const StereoRender stopped = RenderSoundFont(midi_file, font, kRate, kMostSeconds, [](double) { return false; });
    XSFLIB_CHECK(stopped.samples.size() < whole.samples.size() / 4);
}

// A render stops at its limit of length. The limit can fall in the file or in the voices' tail after it.
void TestLimit()
{
    const std::vector<uint8_t> font = sf2::Write(OneZone({G::Value(sf2::kReleaseVolEnv, 1200)}));
    const std::vector<uint8_t> midi_file = Note(60, 1.0);

    const StereoRender whole = RenderSoundFont(midi_file, font, kRate, kMostSeconds);
    const StereoRender in_file = RenderSoundFont(midi_file, font, kRate, 0.5);
    const StereoRender in_tail = RenderSoundFont(midi_file, font, kRate, 1.5);

    XSFLIB_CHECK(whole.samples.size() > static_cast<std::size_t>(2 * 2.9 * kRate));
    XSFLIB_CHECK(in_file.samples.size() == static_cast<std::size_t>(2 * kRate / 2));
    XSFLIB_CHECK(in_tail.samples.size() == static_cast<std::size_t>(2 * 3 * kRate / 2));
    XSFLIB_CHECK(in_tail.samples.size() <= whole.samples.size() &&
                 std::equal(in_tail.samples.begin(), in_tail.samples.end(), whole.samples.begin()));
}

// Whether `call` throws an `Exception`.
template <typename Exception, typename Call>
bool Throws(Call call)
{
    try
    {
        call();
    }
    catch (const Exception&)
    {
        return true;
    }

    return false;
}

// Files that can't be read, and a rate or a length that isn't positive.
void TestRefusals()
{
    const std::vector<uint8_t> midi_file = Note(60, 1.0);
    const std::vector<uint8_t> font = sf2::Write(OneZone({}));
    const std::vector<uint8_t> kNothing = {1, 2, 3};

    XSFLIB_CHECK(Throws<std::runtime_error>([&] { RenderSoundFont(kNothing, font, kRate, kMostSeconds); }));
    XSFLIB_CHECK(Throws<std::runtime_error>([&] { RenderSoundFont(midi_file, kNothing, kRate, kMostSeconds); }));
    XSFLIB_CHECK(Throws<std::invalid_argument>([&] { RenderSoundFont(midi_file, font, 0, kMostSeconds); }));
    XSFLIB_CHECK(Throws<std::invalid_argument>([&] { RenderSoundFont(midi_file, font, kRate, 0.0); }));
    XSFLIB_CHECK(Throws<std::invalid_argument>([&] { RenderSoundFont(midi_file, font, kRate, std::nan("")); }));
}

} // namespace

int main()
{
    TestPitch();
    TestPitchWheel();
    TestLevels();
    TestModulators();
    TestPan();
    TestEnvelope();
    TestLoops();
    TestExclusiveClass();
    TestSustainPedal();
    TestDrums();
    TestFilter();
    TestTiming();
    TestProgress();
    TestLimit();
    TestRefusals();

    return xsflib::testing::Summary("sf2_synth");
}
