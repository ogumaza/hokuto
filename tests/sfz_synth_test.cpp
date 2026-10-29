// SPDX-License-Identifier: MIT

// Tests the SFZ synth (src/synth/sfz_synth.h). The test's SFZ files play a sine of 480 Hz from a WAV file. The opcodes'
// definitions at sfzformat.com give what each render has to hold: pitch_keycenter and tune set the pitch, volume and
// amp_veltrack or amp_velcurve_N the level, ampeg_attack and ampeg_release the times of the envelope, and pan the
// balance. Each MIDI track plays through the SFZ file of its name, and the tracks' renders add up. A track without
// notes requires no file. An event plays at its sample, by the tempo changes of every track. A stop through the
// progress returns no samples. A track without its file, a region whose sample is missing and a damaged MIDI file
// throw. A note that runs through its loop by more than the loop's length in each sample keeps its level. Renders on
// several threads at once give the samples of one render. A sequence of a small archive renders through its export to
// MIDI and SFZ.

#include "synth/sfz_synth.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "ntdlib/sar/formats.h"
#include "ntdlib/sequence_export/convert.h"
#include "ntdlib/sequence_export/sfz.h"
#include "ntdlib/testing/sar_files.h"
#include "seqlib/midi/builder.h"
#include "seqlib/wav.h"
#include "xsflib/files.h"
#include "xsflib/testing/check.h"

#include "song/archive.h"
#include "song/midi_export.h"
#include "synth/render.h"

using hokuto::synth::MidiSeconds;
using hokuto::synth::RenderSfz;
using hokuto::synth::StereoRender;

namespace
{

namespace export_ = ntdlib::sequence_export;
namespace sar_files = ntdlib::testing::sar_files;

constexpr double kPi = 3.14159265358979323846;
constexpr int kRate = 48000;

// The sine: 480 Hz, a period of 100 samples at 48 kHz, for 2 s at 16,000 of 32,767.
constexpr double kSineHz = 480.0;
constexpr int kPeriod = 100;

// The MIDI files' ticks in a second: 480 to a quarter note at 120 BPM.
constexpr double kTicksPerSecond = 960.0;

// The sine as a WAV file, at kRate.
std::vector<uint8_t> SineWav()
{
    std::vector<int16_t> samples(2 * kRate);
    for (std::size_t i = 0; i < samples.size(); i++)
    {
        samples[i] = static_cast<int16_t>(std::lround(16000.0 * std::sin(2.0 * kPi * kSineHz * i / kRate)));
    }

    return seqlib::WavFile(samples, kRate);
}

// A constant 16,000 for 1 s. Its first sample shows where a note starts.
std::vector<uint8_t> ConstantWav()
{
    const std::vector<int16_t> kSamples(kRate, 16000);
    return seqlib::WavFile(kSamples, kRate);
}

// An SFZ file with one region over the sample `sample` for every key, with `opcodes`. Its <control> header finds the
// sample as the converter's files find theirs.
export_::SfzFile SfzFile(const std::string& name, const std::string& sample, const std::string& opcodes)
{
    return {name, "<control>\ndefault_path=../" + export_::kSfzSamplesFolder +
                      "/\nhint_ram_based=1\n\n<region>\nsample=" + sample + "\n" + opcodes + "\n"};
}

// `files` with the samples sine.wav and constant.wav.
export_::SfzInstruments Instruments(std::vector<export_::SfzFile> files)
{
    return {std::move(files), {{"sine.wav", SineWav()}, {"constant.wav", ConstantWav()}}};
}

// A note of a MIDI track: its channel, key and velocity, and the ticks of its note-on and note-off.
struct Note
{
    int channel = 0;
    int key = 62;
    int velocity = 127;
    uint32_t on = 0;
    uint32_t off = 480;
};

// A MIDI track: its name and its notes.
struct Track
{
    std::string name;
    std::vector<Note> notes;
};

// A MIDI file at 120 BPM with 480 ticks to a quarter note, with `tracks`.
std::vector<uint8_t> Midi(const std::vector<Track>& tracks)
{
    seqlib::midi::FileBuilder file(480);
    for (const Track& track : tracks)
    {
        seqlib::midi::TrackBuilder& builder = file.AddTrack();
        builder.Name(track.name);
        builder.Tempo(0, 500000);
        for (const Note& note : track.notes)
        {
            builder.NoteOn(note.on, note.channel, note.key, note.velocity);
            builder.NoteOff(note.off, note.channel, note.key);
        }
    }

    return file.Encode();
}

// `key` at `velocity` for `held` seconds, through a region with `opcodes` over the sine, rendered for `seconds`.
StereoRender RenderNote(const std::string& opcodes, int key, int velocity, double held, double seconds)
{
    const Note note{0, key, velocity, 0, static_cast<uint32_t>(std::lround(held * kTicksPerSecond))};
    const std::vector<uint8_t> midi = Midi({{"Track 0", {note}}});
    return RenderSfz(midi, Instruments({SfzFile("Track 0.sfz", "sine.wav", opcodes)}), kRate, seconds);
}

// Channel `channel` of `render`: 0 for the left and 1 for the right.
std::vector<double> Channel(const StereoRender& render, int channel)
{
    std::vector<double> samples;
    for (std::size_t i = channel; i < render.samples.size(); i += 2)
    {
        samples.push_back(render.samples[i]);
    }

    return samples;
}

// The RMS level of `samples` from sample `first` up to sample `end`.
double Rms(const std::vector<double>& samples, std::size_t first, std::size_t end)
{
    double sum = 0.0;
    for (std::size_t i = first; i < end; i++)
    {
        sum += samples[i] * samples[i];
    }

    return std::sqrt(sum / static_cast<double>(end - first));
}

// The RMS level of `samples` from `from` to `to` seconds.
double Rms(const std::vector<double>& samples, double from, double to)
{
    return Rms(samples, static_cast<std::size_t>(from * kRate), static_cast<std::size_t>(to * kRate));
}

// The RMS level of `samples` over the sine's period centred at `seconds`.
double PeriodRms(const std::vector<double>& samples, double seconds)
{
    const auto centre = static_cast<std::size_t>(std::lround(seconds * kRate));
    return Rms(samples, centre - kPeriod / 2, centre + kPeriod / 2);
}

// `ratio` in decibels.
double Db(double ratio)
{
    return 20.0 * std::log10(ratio);
}

// The frequency of `samples` from `from` to `to` seconds, from the times of their rising zero crossings.
double Frequency(const std::vector<double>& samples, double from, double to)
{
    std::vector<double> crossings;
    for (auto i = static_cast<std::size_t>(from * kRate) + 1; i < static_cast<std::size_t>(to * kRate); i++)
    {
        if (samples[i - 1] < 0.0 && samples[i] >= 0.0)
        {
            crossings.push_back(static_cast<double>(i - 1) + samples[i - 1] / (samples[i - 1] - samples[i]));
        }
    }
    if (crossings.size() < 2)
    {
        return 0.0;
    }

    return static_cast<double>(crossings.size() - 1) * kRate / (crossings.back() - crossings.front());
}

// The largest difference between the samples of `a` and `b`, or infinity when their lengths differ.
double LargestDifference(const std::vector<float>& a, const std::vector<float>& b)
{
    if (a.size() != b.size())
    {
        return std::numeric_limits<double>::infinity();
    }

    double largest = 0.0;
    for (std::size_t i = 0; i < a.size(); i++)
    {
        largest = std::max(largest, static_cast<double>(std::abs(a[i] - b[i])));
    }

    return largest;
}

// pitch_keycenter is the key that plays the sample at its pitch. Each key above it raises the pitch by a semitone, by
// pitch_keytrack's default of 100 cents. tune raises it by its cents. Each pitch holds within half a cent.
void TestPitch()
{
    const auto cents = [](const std::string& opcodes, int key)
    {
        const StereoRender render = RenderNote(opcodes, key, 127, 0.5, 0.5);
        return 1200.0 * std::log2(Frequency(Channel(render, 0), 0.1, 0.4) / kSineHz);
    };

    XSFLIB_CHECK(std::abs(cents("pitch_keycenter=62", 62)) < 0.5);
    XSFLIB_CHECK(std::abs(cents("pitch_keycenter=62", 74) - 1200.0) < 0.5);
    XSFLIB_CHECK(std::abs(cents("pitch_keycenter=62", 55) + 700.0) < 0.5);
    XSFLIB_CHECK(std::abs(cents("pitch_keycenter=62 tune=37", 62) - 37.0) < 0.5);
    XSFLIB_CHECK(std::abs(cents("pitch_keycenter=62 tune=-50", 67) - 450.0) < 0.5);
}

// volume sets the level in decibels. With amp_veltrack at its default of 100, velocity v plays at 20 log10((v / 127)^2)
// dB. amp_veltrack=0 leaves the level alone. amp_velcurve_N gives the amplitude at velocity N, with straight lines
// between the points that the file gives and amplitude 1 at velocity 127. Each level holds within 0.01 dB.
void TestLevel()
{
    const auto level = [](const std::string& opcodes, int velocity)
    {
        const StereoRender render = RenderNote("pitch_keycenter=62 " + opcodes, 62, velocity, 0.5, 0.5);
        return Db(Rms(Channel(render, 0), 0.1, 0.4));
    };
    const double full = level("", 127);

    XSFLIB_CHECK(std::abs(level("volume=-6", 127) - full + 6.0) < 0.01);
    XSFLIB_CHECK(std::abs(level("volume=-14.5", 127) - full + 14.5) < 0.01);
    XSFLIB_CHECK(std::abs(level("", 64) - full - Db(std::pow(64.0 / 127.0, 2.0))) < 0.01);
    XSFLIB_CHECK(std::abs(level("amp_veltrack=0", 64) - full) < 0.01);
    XSFLIB_CHECK(std::abs(level("amp_velcurve_64=0.5", 64) - full - Db(0.5)) < 0.01);
    XSFLIB_CHECK(std::abs(level("amp_velcurve_64=0.5", 96) - full - Db(0.5 + 0.5 * 32.0 / 63.0)) < 0.01);
}

// ampeg_attack is the time in which the level rises to full, and ampeg_release the time in which it falls after the
// note-off. sfzformat.com describes ARIA's envelopes: a straight rise, and a fall that multiplies the level by exp(-8 /
// (t s)) at each sample, with t the time and s the rate. A quarter of the attack then plays at a quarter of the level,
// and half the release 35 dB down. sfizz falls to exp(-9) instead of exp(-8) in the release's time: 39 dB down at half
// of it. The test takes 30 to 45 dB there. At the end of the release, the level lies 60 dB down or lower.
void TestEnvelope()
{
    const StereoRender render = RenderNote("pitch_keycenter=62 ampeg_attack=0.1 ampeg_release=0.2", 62, 127, 0.5, 1.0);
    const std::vector<double> left = Channel(render, 0);
    const double sustain = Rms(left, 0.2, 0.45);
    const auto at = [&](double seconds)
    {
        return PeriodRms(left, seconds) / sustain;
    };

    XSFLIB_CHECK(std::abs(at(0.025) - 0.25) < 0.01);
    XSFLIB_CHECK(std::abs(at(0.05) - 0.5) < 0.01);
    XSFLIB_CHECK(std::abs(at(0.075) - 0.75) < 0.01);
    XSFLIB_CHECK(at(0.09) < 0.95);
    XSFLIB_CHECK(std::abs(at(0.11) - 1.0) < 0.001);
    XSFLIB_CHECK(std::abs(at(0.49) - 1.0) < 0.001);
    XSFLIB_CHECK(Db(at(0.55)) < -5.0);
    XSFLIB_CHECK(Db(at(0.6)) > -45.0 && Db(at(0.6)) < -30.0);
    XSFLIB_CHECK(Db(at(0.7)) < -60.0);
}

// pan places a mono sample: 0 in the centre, negative values to the left and positive ones to the right, from -100 to
// 100. At -100 the right side is silent, and at 100 the left side. Opposite values mirror each other.
void TestPan()
{
    const auto sides = [](const std::string& pan)
    {
        const StereoRender render = RenderNote("pitch_keycenter=62 pan=" + pan, 62, 127, 0.5, 0.5);
        return std::pair(Rms(Channel(render, 0), 0.1, 0.4), Rms(Channel(render, 1), 0.1, 0.4));
    };
    const auto [centre_left, centre_right] = sides("0");
    const auto [left_left, left_right] = sides("-100");
    const auto [right_left, right_right] = sides("100");
    const auto [half_left, half_right] = sides("-50");
    const auto [mirror_left, mirror_right] = sides("50");

    XSFLIB_CHECK(std::abs(Db(centre_left / centre_right)) < 0.01);
    XSFLIB_CHECK(Db(left_right / left_left) < -60.0);
    XSFLIB_CHECK(Db(right_left / right_right) < -60.0);
    XSFLIB_CHECK(half_left > half_right && half_left < left_left && half_right > left_right);
    XSFLIB_CHECK(std::abs(Db(half_left / mirror_right)) < 0.01);
    XSFLIB_CHECK(std::abs(Db(half_right / mirror_left)) < 0.01);
}

// Each MIDI track plays through the file of its name, whatever its channel: "Track 1 (layer 2)" plays through Track
// 1.sfz too. Track 0's file pans its notes to the left, and Track 1's to the right. The render of the three tracks is
// the sum of their renders alone.
void TestTracks()
{
    const export_::SfzInstruments instruments =
        Instruments({SfzFile("Track 0.sfz", "sine.wav", "pitch_keycenter=62 pan=-100"),
                     SfzFile("Track 1.sfz", "sine.wav", "pitch_keycenter=62 pan=100")});
    const Track kZero{"Track 0", {{0, 62, 127, 0, 480}}};
    const Track kOne{"Track 1", {{5, 62, 100, 240, 720}}};
    const Track kLayer{"Track 1 (layer 2)", {{6, 74, 90, 480, 960}}};

    const auto render = [&](const std::vector<Track>& tracks)
    {
        return RenderSfz(Midi(tracks), instruments, kRate, 1.5).samples;
    };
    const std::vector<float> all = render({kZero, kOne, kLayer});
    std::vector<float> sum(all.size(), 0.0f);
    for (const Track& track : {kZero, kOne, kLayer})
    {
        const std::vector<float> alone = render({track});
        SEQLIB_REQUIRE_EQ(alone.size(), all.size());
        for (std::size_t i = 0; i < sum.size(); i++)
        {
            sum[i] += alone[i];
        }
    }

    const StereoRender layer_alone{kRate, render({kLayer})};

    XSFLIB_CHECK(LargestDifference(all, sum) < 1e-6);
    XSFLIB_CHECK(Rms(Channel(layer_alone, 0), 0.6, 0.9) < 1e-6);
    XSFLIB_CHECK(Rms(Channel(layer_alone, 1), 0.6, 0.9) > 0.1);
    XSFLIB_CHECK(std::abs(Frequency(Channel(layer_alone, 1), 0.6, 0.9) - 2.0 * kSineHz) < 0.1);
}

// A MIDI track without a note before the render's end plays nothing, and requires no SFZ file. The converter's first
// track takes the song's name, and marks the loop's start with CC111 on the first channel. A track whose note starts
// past the render's end sets its volume before it. Neither track changes the render.
void TestTracksWithoutNotes()
{
    seqlib::midi::FileBuilder file(480);
    seqlib::midi::TrackBuilder& song = file.AddTrack();
    song.Name("Song");
    song.Tempo(0, 500000);
    song.Control(240, 0, 111, 0);
    seqlib::midi::TrackBuilder& zero = file.AddTrack();
    zero.Name("Track 0");
    zero.NoteOn(0, 0, 62, 127);
    zero.NoteOff(480, 0, 62);
    seqlib::midi::TrackBuilder& late = file.AddTrack();
    late.Name("Track 1");
    late.Control(0, 1, 7, 100);
    late.NoteOn(960, 1, 62, 127);
    late.NoteOff(1200, 1, 62);
    const export_::SfzInstruments instruments = Instruments({SfzFile("Track 0.sfz", "sine.wav", "pitch_keycenter=62")});

    const StereoRender expected = RenderSfz(Midi({{"Track 0", {{}}}}), instruments, kRate, 0.75);
    const StereoRender render = RenderSfz(file.Encode(), instruments, kRate, 0.75);

    XSFLIB_CHECK(Rms(Channel(expected, 0), 0.1, 0.4) > 0.1);
    XSFLIB_CHECK(LargestDifference(render.samples, expected.samples) == 0.0);
}

// An event plays at its sample, by the tempo changes of every track. At 120 BPM and 480 ticks to a quarter note, tick
// 480 plays at 0.5 s. A tempo of 240 BPM there, in a track without notes, puts tick 960 at 0.75 s: sample 36,000. The
// note's end at tick 1,200 lies at 0.875 s.
void TestTiming()
{
    seqlib::midi::FileBuilder file(480);
    seqlib::midi::TrackBuilder& tempo = file.AddTrack();
    tempo.Name("Tempo");
    tempo.Tempo(0, 500000);
    tempo.Tempo(480, 250000);
    seqlib::midi::TrackBuilder& notes = file.AddTrack();
    notes.Name("Track 0");
    notes.NoteOn(960, 0, 60, 127);
    notes.NoteOff(1200, 0, 60);
    const std::vector<uint8_t> midi = file.Encode();

    const StereoRender render =
        RenderSfz(midi, Instruments({SfzFile("Track 0.sfz", "constant.wav", "pitch_keycenter=60")}), kRate, 1.0);
    const std::vector<double> left = Channel(render, 0);
    SEQLIB_REQUIRE_EQ(left.size(), std::size_t{kRate});

    XSFLIB_CHECK(std::abs(MidiSeconds(midi) - 0.875) < 1e-12);
    XSFLIB_CHECK(std::all_of(left.begin(), left.begin() + 36000, [](double sample) { return sample == 0.0; }));
    XSFLIB_CHECK(left[36000] > 0.1);
}

// The progress gets fractions that rise to 1. When it returns false, the render stops and gives no samples.
void TestProgress()
{
    const std::vector<uint8_t> midi = Midi({{"Track 0", {{}}}, {"Track 1", {{}}}});
    const export_::SfzInstruments instruments = Instruments({SfzFile("Track 0.sfz", "sine.wav", "pitch_keycenter=62"),
                                                             SfzFile("Track 1.sfz", "sine.wav", "pitch_keycenter=62")});
    std::vector<double> fractions;
    const auto keep = [&](double fraction)
    {
        fractions.push_back(fraction);

        return true;
    };
    const StereoRender whole = RenderSfz(midi, instruments, kRate, 1.0, keep);

    int calls = 0;
    const auto stop = [&](double)
    {
        calls++;

        return calls < 10;
    };
    const StereoRender stopped = RenderSfz(midi, instruments, kRate, 1.0, stop);

    SEQLIB_CHECK_EQ(whole.samples.size(), std::size_t{2 * kRate});
    SEQLIB_REQUIRE(fractions.size() > 2);
    XSFLIB_CHECK(fractions.front() > 0.0 && std::abs(fractions.back() - 1.0) < 1e-12);
    XSFLIB_CHECK(std::is_sorted(fractions.begin(), fractions.end()));
    XSFLIB_CHECK(stopped.samples.empty());
    SEQLIB_CHECK_EQ(calls, 10);
}

// Whether `render` throws std::runtime_error.
template <typename Render>
bool Throws(const Render& render)
{
    try
    {
        render();
    }
    catch (const std::runtime_error&)
    {
        return true;
    }

    return false;
}

// A track that plays without an SFZ file of its name, a region whose sample is missing and a damaged MIDI file throw. A
// file without regions plays nothing.
void TestErrors()
{
    const std::vector<uint8_t> midi = Midi({{"Track 0", {{}}}, {"Track 2", {{}}}});
    const export_::SfzFile zero = SfzFile("Track 0.sfz", "sine.wav", "pitch_keycenter=62");
    const export_::SfzFile two = SfzFile("Track 2.sfz", "sine.wav", "pitch_keycenter=62");
    const export_::SfzFile missing = SfzFile("Track 2.sfz", "missing.wav", "pitch_keycenter=62");
    const export_::SfzFile kEmpty{"Track 2.sfz", "<control>\ndefault_path=../samples/\n"};
    const std::vector<uint8_t> damaged(midi.begin(), midi.begin() + 20);

    XSFLIB_CHECK(!Throws([&] { RenderSfz(midi, Instruments({zero, two}), kRate, 0.5); }));
    XSFLIB_CHECK(Throws([&] { RenderSfz(midi, Instruments({zero}), kRate, 0.5); }));
    XSFLIB_CHECK(Throws([&] { RenderSfz(midi, Instruments({zero, missing}), kRate, 0.5); }));
    XSFLIB_CHECK(Throws([&] { RenderSfz(damaged, Instruments({zero, two}), kRate, 0.5); }));
    XSFLIB_CHECK(Throws([&] { MidiSeconds(damaged); }));

    const StereoRender alone = RenderSfz(Midi({{"Track 0", {{}}}}), Instruments({zero}), kRate, 0.5);
    const StereoRender with_empty = RenderSfz(midi, Instruments({zero, kEmpty}), kRate, 0.5);
    XSFLIB_CHECK(LargestDifference(alone.samples, with_empty.samples) == 0.0);
}

// With loop_mode=loop_continuous, a note plays its sample from loop_start to loop_end over and over while it lasts. Key
// 127 over pitch_keycenter=0 plays the sample 2^(127/12) times as fast: about 1,534 samples in each output sample,
// through a loop of 100. The note's position then moves back through the loop many times in each sample. Over the
// constant, the note renders, and holds the level of the sample at its pitch within 0.01 dB. sfizz 1.2.3 moved the
// position back once at most in each sample, and gave samples that aren't finite there (cmake/sfizz_loop_fix.cmake).
void TestFastLoop()
{
    const auto render = [](int key)
    {
        const Note note{0, key, 127, 0, 480};
        const std::string kOpcodes = "pitch_keycenter=0 loop_mode=loop_continuous loop_start=1000 loop_end=1099";
        const std::vector<uint8_t> midi = Midi({{"Track 0", {note}}});
        return RenderSfz(midi, Instruments({SfzFile("Track 0.sfz", "constant.wav", kOpcodes)}), kRate, 0.5);
    };
    const StereoRender slow = render(0);
    StereoRender fast;
    const auto render_fast = [&]
    {
        fast = render(127);
    };

    SEQLIB_REQUIRE(!Throws(render_fast));
    XSFLIB_CHECK(std::abs(Db(Rms(Channel(fast, 0), 0.1, 0.4)) - Db(Rms(Channel(slow, 0), 0.1, 0.4))) < 0.01);
}

// Four renders on four threads at once give the samples of a render on one thread.
void TestThreads()
{
    const std::vector<uint8_t> midi =
        Midi({{"Track 0", {{0, 62, 127, 0, 480}, {0, 69, 100, 240, 720}}}, {"Track 1", {{1, 50, 90, 120, 600}}}});
    const export_::SfzInstruments instruments =
        Instruments({SfzFile("Track 0.sfz", "sine.wav", "pitch_keycenter=62 ampeg_release=0.1 pan=-30"),
                     SfzFile("Track 1.sfz", "sine.wav", "pitch_keycenter=62 ampeg_attack=0.05 tune=20 pan=40")});
    const std::vector<float> expected = RenderSfz(midi, instruments, kRate, 1.0).samples;

    std::vector<std::vector<float>> renders(4);
    std::vector<std::thread> threads;
    for (std::vector<float>& render : renders)
    {
        threads.emplace_back([&] { render = RenderSfz(midi, instruments, kRate, 1.0).samples; });
    }
    for (std::thread& thread : threads)
    {
        thread.join();
    }

    for (const std::vector<float>& render : renders)
    {
        XSFLIB_CHECK(LargestDifference(render, expected) == 0.0);
    }
}

// A file in the system's temporary folder with `bytes` in it, deleted again with the object.
class TemporaryFile
{
public:
    explicit TemporaryFile(std::span<const uint8_t> bytes)
        : path_(std::filesystem::temp_directory_path() /
                ("hokuto_sfz_synth_test_" + std::to_string(std::random_device{}()) + ".bfsar"))
    {
        xsflib::WriteFile(path_, bytes);
    }

    TemporaryFile(const TemporaryFile&) = delete;
    TemporaryFile& operator=(const TemporaryFile&) = delete;

    ~TemporaryFile()
    {
        std::error_code error;
        std::filesystem::remove(path_, error);
    }

    const std::filesystem::path& Path() const
    {
        return path_;
    }

private:
    std::filesystem::path path_;
};

// A small Switch archive: a sequence that plays key 60 for 24 ticks at timebase 48 and 120 BPM, a quarter of a second,
// on a bank whose one instrument plays a saw at original key 60. The saw loops over 480 samples at 48 kHz: 100 Hz.
std::vector<uint8_t> SawArchive()
{
    std::vector<uint8_t> saw;
    for (int i = 0; i < 480; i++)
    {
        const auto sample = static_cast<uint16_t>(static_cast<int16_t>(i * 40 - 9600));
        saw.push_back(static_cast<uint8_t>(sample));
        saw.push_back(static_cast<uint8_t>(sample >> 8));
    }
    sar_files::WaveSpec wave;
    wave.encoding = 1;
    wave.loop = true;
    wave.sample_rate = 48000;
    wave.loop_end = 480;
    wave.channels = {saw};

    sar_files::Region region;
    region.original_key = 60;
    sar_files::VelocityTable velocities;
    velocities.entries = {{0, region}};
    sar_files::KeyTable keys;
    keys.entries = {{0, velocities}};
    sar_files::BankSpec bank;
    bank.waves = {{0x05000000, 0}};
    bank.instruments = {keys};

    sar_files::SoundSpec sound;
    sound.name = "SEQ_NOTE";
    sound.volume = 127;
    sound.banks = {0};
    sar_files::ArchiveSpec spec;
    spec.sounds = {sound};
    spec.banks = {{"BANK_TEST", 1}};
    spec.wave_archives = {{"WARC_TEST", 2}};
    spec.players = {{"PLAYER_TEST", 1}};
    spec.files = {
        {sar_files::Sequence(false, {0xB0, 48, 0xE1, 120, 0x00, 60, 100, 24, 0xFF}, {{"Note", 0}}), "", false},
        {sar_files::Bank(false, bank), "", false},
        {sar_files::WaveArchive(false, {sar_files::Wave(false, wave)}), "", false}};

    return sar_files::Archive(false, spec);
}

// The archive's sequence goes through Hokuto's export to MIDI and SFZ, and renders: the saw's 100 Hz for about a
// quarter of a second.
void TestArchive()
{
    const TemporaryFile file(SawArchive());
    const std::shared_ptr<const ntdlib::sar::SoundArchive> archive = hokuto::song::OpenArchive(file.Path());
    const export_::Conversion conversion = hokuto::song::ConvertToMidi(*archive, 0, {}, {}, "Hokuto");
    SEQLIB_REQUIRE(!conversion.sfz.files.empty() && !conversion.sfz.samples.empty());

    const double seconds = MidiSeconds(conversion.midi);
    const StereoRender render = RenderSfz(conversion.midi, conversion.sfz, kRate, seconds + 0.5);
    const std::vector<double> left = Channel(render, 0);
    const double peak = *std::max_element(render.samples.begin(), render.samples.end(),
                                          [](float a, float b) { return std::abs(a) < std::abs(b); });

    XSFLIB_CHECK(seconds > 0.2 && seconds < 1.0);
    XSFLIB_CHECK(std::abs(peak) > 0.01 && std::abs(peak) < 1.0);
    XSFLIB_CHECK(std::abs(1200.0 * std::log2(Frequency(left, 0.02, 0.2) / 100.0)) < 1.0);
    XSFLIB_CHECK(Rms(left, seconds + 0.3, seconds + 0.5) < 1e-4);
}

} // namespace

int main()
{
    TestPitch();
    TestLevel();
    TestEnvelope();
    TestPan();
    TestTracks();
    TestTracksWithoutNotes();
    TestTiming();
    TestProgress();
    TestErrors();
    TestFastLoop();
    TestThreads();
    TestArchive();

    return xsflib::testing::Summary("sfz_synth");
}
