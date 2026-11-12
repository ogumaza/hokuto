// SPDX-License-Identifier: MIT

// Tests File > Import > Bank… of a SoundFont 2 file on a GBA file (src/edit/gba_sound_fonts.h). A small SoundFont
// contains a looped saw with a set envelope, a preset of two runs of keys, a quiet preset, one of MIDI bank 128 with a
// pan and one whose scale tuning is 0. The expected values come from the driver's envelope and the SoundFont 2.04
// specification. The driver scales the envelope by 256ths in each frame. In the specification, a decay or a release
// falls 100 dB in its time. The driver's model then has to play each new voice at the pitch that the saw's period
// gives, and the quiet preset's level has to fall by its attenuation.

#include "edit/gba_sound_fonts.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/agb/gba.h"
#include "ntdlib/agb/rom.h"
#include "ntdlib/mp2k/song.h"
#include "ntdlib/sequence_export/mp2k/soundfont.h"
#include "ntdlib/testing/mp2k_cart.h"
#include "seqlib/sf2/soundfont.h"
#include "seqlib/sf2/writer.h"
#include "xsflib/testing/check.h"

#include "cart_bytes.h"
#include "edit/gba_voices.h"
#include "edit/sound_font_voices.h"
#include "format/archive.h"
#include "format/file.h"
#include "song/gba.h"
#include "song/preview.h"

namespace
{

namespace mp2k = ntdlib::mp2k;
namespace mp2k_cart = ntdlib::testing::mp2k_cart;
namespace sf2 = seqlib::sf2;
using hokuto::edit::GbaSoundFontVoices;
using hokuto::edit::GbaVoicesOfSoundFont;
using hokuto::format::File;

// The saw's period in points, and its rate.
constexpr std::size_t kPeriod = 64;
constexpr uint32_t kRate = 16000;

// A SoundFont of a looped saw with its root at key 60: an envelope at preset 3, two runs of keys at preset 4, a quiet
// preset 5, a drum kit of keys 36 and 38 in MIDI bank 128, and preset 6 at a scale tuning of 0.
std::vector<uint8_t> TestFont()
{
    using G = sf2::Generator;
    sf2::SoundFont font;
    sf2::Sample sample;
    sample.name = "saw";
    sample.sample_rate = kRate;
    sample.original_key = 60;
    sample.loop_start = 16 * kPeriod;
    sample.loop_end = 32 * kPeriod;
    for (std::size_t i = 0; i < 32 * kPeriod; ++i)
    {
        sample.data.push_back(static_cast<int16_t>(static_cast<int>(i % kPeriod) * 800 - 25000));
    }
    font.samples.push_back(sample);

    const auto zone = [](std::vector<G> generators)
    {
        generators.push_back(G::Value(sf2::kSampleModes, 1));
        generators.push_back(G::Value(sf2::kSampleId, 0));
        return sf2::Zone{std::move(generators), {}};
    };
    const auto keys = [](int low, int high)
    {
        return G::Range(sf2::kKeyRange, static_cast<uint8_t>(low), static_cast<uint8_t>(high));
    };
    // 1200 timecents are 2 seconds, and 0 timecents 1 second.
    font.instruments.push_back({"envelope",
                                {zone({G::Value(sf2::kAttackVolEnv, 0), G::Value(sf2::kDecayVolEnv, 0),
                                       G::Value(sf2::kSustainVolEnv, 200), G::Value(sf2::kReleaseVolEnv, 1200)})}});
    font.instruments.push_back(
        {"runs", {zone({keys(0, 59)}), zone({keys(60, 127), G::Value(sf2::kReleaseVolEnv, 0)})}});
    // In the players, 500 of initialAttenuation plays 200 cB, 20 dB.
    font.instruments.push_back({"quiet", {zone({G::Value(sf2::kInitialAttenuation, 500)})}});
    font.instruments.push_back({"kit", {zone({keys(36, 36), G::Value(sf2::kPan, -250)}), zone({keys(38, 38)})}});
    font.instruments.push_back({"fixed", {zone({G::Value(sf2::kScaleTuning, 0)})}});
    font.presets.push_back({"envelope", 3, 0, {{{G::Value(sf2::kInstrument, 0)}, {}}}});
    font.presets.push_back({"runs", 4, 0, {{{G::Value(sf2::kInstrument, 1)}, {}}}});
    font.presets.push_back({"quiet", 5, 0, {{{G::Value(sf2::kInstrument, 2)}, {}}}});
    font.presets.push_back({"fixed", 6, 0, {{{G::Value(sf2::kInstrument, 4)}, {}}}});
    font.presets.push_back({"kit", 0, 128, {{{G::Value(sf2::kInstrument, 3)}, {}}}});

    return sf2::Write(font);
}

// The frames in which the fade of the driver brings the level from `from` to `to` or below: the level becomes `fade`
// 256ths of itself each frame, rounded down.
int FramesOf(int fade, int from, int to)
{
    int frames = 0;
    for (int level = from; level > to; ++frames)
    {
        level = (level * fade) >> 8;
    }

    return frames;
}

// The seconds in which a SoundFont's envelope falls 100 dB at the pace of `frames` from `from` to `to`.
double SecondsOfFall(int frames, int from, int to)
{
    return 100.0 / (20.0 * std::log10(static_cast<double>(from) / to)) * frames / ntdlib::agb::kFrameRate;
}

// Whether `fade` brings the driver from `from` to `to` within a frame of a fall of 100 dB in `seconds`.
bool KeepsPace(int fade, double seconds, int from, int to)
{
    const double frames = seconds / SecondsOfFall(1, from, to);

    return std::abs(FramesOf(fade, from, to) - frames) <= 1.0;
}

const hokuto::edit::GbaSoundFontPreset& PresetNamed(const GbaSoundFontVoices& voices, const std::string& name)
{
    const auto found = std::ranges::find(voices.presets, name, &hokuto::edit::GbaSoundFontPreset::name);

    return voices.presets.at(static_cast<std::size_t>(found - voices.presets.begin()));
}

// A zone that plays on past its release, such as one that the one_shot of an SFZ file makes, gets the slowest release
// of the driver. A fade of 255 keeps 255/256 of the level each frame.
void TestPlaysPastRelease()
{
    sf2::SoundFont font;
    sf2::Sample sample;
    sample.name = "wave";
    sample.sample_rate = kRate;
    sample.original_key = 60;
    sample.data.assign(4096, 8000);
    font.samples.push_back(sample);
    using G = sf2::Generator;
    font.instruments.push_back(
        {"shot", {{{G::Value(hokuto::edit::kPlaysPastRelease, 1), G::Value(sf2::kSampleId, 0)}, {}}}});
    font.presets.push_back({"shot", 0, 0, {{{G::Value(sf2::kInstrument, 0)}, {}}}});

    const GbaSoundFontVoices voices = GbaVoicesOfSoundFont(sf2::Write(font), {});
    SEQLIB_REQUIRE(voices.presets.size() == 1 && voices.presets[0].rows.size() == 1);
    XSFLIB_CHECK(voices.presets[0].rows[0].voice.release == 255);
}

// A SoundFont's presets become rows of GBA voices: a sample with one envelope, the envelope's attack and sustain, a key
// split for another envelope, a drum kit for MIDI bank 128 and a fixed pitch for a scale tuning of 0.
void TestValues()
{
    const GbaSoundFontVoices voices = GbaVoicesOfSoundFont(TestFont(), {});
    XSFLIB_CHECK(voices.highest_rate == kRate);
    XSFLIB_CHECK(voices.presets.size() == 5);
    XSFLIB_CHECK(voices.presets.front().name == "envelope" && voices.presets.back().name == "kit");

    // One sample with one envelope over every key is one row.
    const hokuto::edit::GbaSoundFontPreset& envelope = PresetNamed(voices, "envelope");
    XSFLIB_CHECK(!envelope.drums);
    SEQLIB_REQUIRE(envelope.rows.size() == 1);
    XSFLIB_CHECK(envelope.rows[0].first_key == 0 && envelope.rows[0].last_key == 127);
    // An attack of a second gains 255/(frames + 1) a frame.
    XSFLIB_CHECK(envelope.rows[0].voice.attack == std::lround(255.0 / (ntdlib::agb::kFrameRate + 1.0)));
    XSFLIB_CHECK(KeepsPace(envelope.rows[0].voice.decay, 1.0, 255, envelope.rows[0].voice.sustain));
    XSFLIB_CHECK(KeepsPace(envelope.rows[0].voice.release, 2.0, envelope.rows[0].voice.sustain,
                           envelope.rows[0].voice.sustain / 8));
    // 200 cB of sustain is a tenth of the full level.
    XSFLIB_CHECK(envelope.rows[0].voice.sustain == std::lround(25.5));

    // Another envelope over some keys is another row of a key split, and both play the same sample.
    const hokuto::edit::GbaSoundFontPreset& runs = PresetNamed(voices, "runs");
    SEQLIB_REQUIRE(runs.rows.size() == 2 && !runs.drums);
    XSFLIB_CHECK(runs.rows[0].first_key == 0 && runs.rows[0].last_key == 59);
    XSFLIB_CHECK(runs.rows[1].first_key == 60 && runs.rows[1].last_key == 127);
    XSFLIB_CHECK(runs.rows[0].sample == runs.rows[1].sample);
    XSFLIB_CHECK(runs.rows[0].voice.release == 0 && KeepsPace(runs.rows[1].voice.release, 1.0, 255, 31));

    // MIDI bank 128 is a drum kit: a row for each key, each with its key, and a pan.
    const hokuto::edit::GbaSoundFontPreset& kit = PresetNamed(voices, "kit");
    XSFLIB_CHECK(kit.drums);
    SEQLIB_REQUIRE(kit.rows.size() == 2);
    XSFLIB_CHECK(kit.rows[0].first_key == 36 && kit.rows[0].last_key == 36 && kit.rows[0].voice.key == 36);
    XSFLIB_CHECK(kit.rows[1].voice.key == 38);
    XSFLIB_CHECK(kit.rows[0].voice.RhythmPan() == -64 && kit.rows[1].voice.pan_sweep == 0);

    // A scale tuning of 0 plays the root's pitch at every key. The preset becomes a drum kit, and each of its voices
    // names that pitch.
    const hokuto::edit::GbaSoundFontPreset& fixed = PresetNamed(voices, "fixed");
    XSFLIB_CHECK(fixed.drums && fixed.rows.size() == 128);
    XSFLIB_CHECK(fixed.rows.front().voice.key == 60 && fixed.rows.back().voice.key == 60);

    // The rate of a sample at key 60 is the rate of the sample: its root key is 60. Its points go in at 8 bits, with
    // the quiet preset's at a tenth of the level.
    const std::vector<uint8_t>& plain = voices.samples.at(envelope.rows[0].sample);
    const std::vector<uint8_t>& quiet = voices.samples.at(PresetNamed(voices, "quiet").rows[0].sample);
    const auto point = [](const std::vector<uint8_t>& sample, std::size_t at)
    {
        return static_cast<int8_t>(sample.at(16 + at));
    };
    const auto rate = [](const std::vector<uint8_t>& sample)
    {
        return (uint32_t{sample[4]} | uint32_t{sample[5]} << 8 | uint32_t{sample[6]} << 16 |
                uint32_t{sample[7]} << 24) /
               1024;
    };
    XSFLIB_CHECK(rate(plain) == kRate && rate(quiet) == kRate);
    XSFLIB_CHECK(point(plain, 0) == ((-25000 + 128) >> 8));
    XSFLIB_CHECK(point(quiet, 0) == ((-2500 + 128) >> 8));

    // What the voices leave out shows in the warnings.
    XSFLIB_CHECK(!voices.warnings.empty() && voices.warnings.front() == "Samples are converted to 8-bit PCM.");
}

// A sample imported at a lower rate has fewer points, and its key 60 plays at that rate.
void TestLowerRate()
{
    // A looped sample at half its rate has half the points, and key 60 plays it at half the rate.
    const GbaSoundFontVoices voices = GbaVoicesOfSoundFont(TestFont(), {}, kRate / 2);
    XSFLIB_CHECK(voices.highest_rate == kRate);
    const std::vector<uint8_t>& sample = voices.samples.front();
    const auto word = [&sample](std::size_t at)
    {
        return uint32_t{sample[at]} | uint32_t{sample[at + 1]} << 8 | uint32_t{sample[at + 2]} << 16 |
               uint32_t{sample[at + 3]} << 24;
    };
    XSFLIB_CHECK(word(4) / 1024 == kRate / 2);
    XSFLIB_CHECK(word(8) == 8 * kPeriod);
    XSFLIB_CHECK(word(12) == 16 * kPeriod);
}

// A SoundFont of one sample of `rate` with its byOriginalPitch at `root`, played by preset 0 at every key.
std::vector<uint8_t> OneSampleFont(uint32_t rate, uint8_t root)
{
    sf2::SoundFont font;
    sf2::Sample sample;
    sample.name = "one";
    sample.sample_rate = rate;
    sample.original_key = root;
    sample.data.assign(4096, 8000);
    font.samples.push_back(sample);
    using G = sf2::Generator;
    font.instruments.push_back({"one", {{{G::Value(sf2::kSampleId, 0)}, {}}}});
    font.presets.push_back({"one", 0, 0, {{{G::Value(sf2::kInstrument, 0)}, {}}}});

    return sf2::Write(font);
}

// The rate of `sample`, a sample of the voices, in 1024ths of a hertz, as MP2K's header stores it.
uint32_t HeaderRate(const std::vector<uint8_t>& sample)
{
    return uint32_t{sample.at(4)} | uint32_t{sample.at(5)} << 8 | uint32_t{sample.at(6)} << 16 |
           uint32_t{sample.at(7)} << 24;
}

// A sample of 96 kHz with its root at key 6 plays at 2^4.5 times its rate at key 60, more than 2^31 1024ths of a hertz.
// MP2K's header stores that rate in 1024ths of a hertz, rounded to the nearest, on every platform.
void TestHighRate()
{
    const GbaSoundFontVoices voices = GbaVoicesOfSoundFont(OneSampleFont(96000, 6), {});

    SEQLIB_REQUIRE(voices.samples.size() == 1);
    XSFLIB_CHECK(HeaderRate(voices.samples[0]) ==
                 static_cast<uint32_t>(std::llround(96000.0 * std::exp2(4.5) * 1024.0)));
}

// A sample whose byOriginalPitch is 255, unpitched, in a zone without an overriding root key plays as key 60, as
// section 7.10 of the SoundFont 2.04 specification says. Key 60 plays it at its rate.
void TestUnpitched()
{
    const GbaSoundFontVoices voices = GbaVoicesOfSoundFont(OneSampleFont(kRate, 255), {});

    SEQLIB_REQUIRE(voices.samples.size() == 1);
    XSFLIB_CHECK(HeaderRate(voices.samples[0]) == kRate * 1024);
}

// On Camelot's mixer, the release of a voice subtracts 256 less its value from the level each frame, in a straight line
// (ntdlib's sequencer). Preset 4's upper keys release in a second from the full level. Among the releases, the voice
// gets the one whose fade in ntdlib's export is nearest a second, and that export is within 100 timecents of it.
void TestCamelotRelease()
{
    mp2k::DriverInfo camelot;
    camelot.camelot_mixer = true;

    const GbaSoundFontVoices voices = GbaVoicesOfSoundFont(TestFont(), camelot);

    const hokuto::edit::GbaSoundFontPreset& runs = PresetNamed(voices, "runs");
    SEQLIB_REQUIRE(runs.rows.size() == 2 && runs.rows[1].voice.sustain == 255);
    mp2k::Voice voice = runs.rows[1].voice;
    const int taken = std::abs(ntdlib::sequence_export::mp2k::EnvelopeFor(camelot, voice).release);
    int nearest = taken;
    for (int release = 0; release <= 255; ++release)
    {
        voice.release = static_cast<uint8_t>(release);
        nearest = std::min(nearest, std::abs(ntdlib::sequence_export::mp2k::EnvelopeFor(camelot, voice).release));
    }
    XSFLIB_CHECK(taken == nearest && taken <= 100);
}

// A decay that a voice of the driver exports to a SoundFont imports again as a fade of the same frames. The test writes
// the decay as ntdlib's converter does. The straight line in decibels would take 253 for 248: rounding down makes the
// fade faster than its value.
void TestFades()
{
    for (const int fade : {120, 165, 210, 240, 253, 254})
    {
        using G = sf2::Generator;
        sf2::SoundFont font;
        sf2::Sample sample;
        sample.name = "sample";
        sample.sample_rate = kRate;
        sample.original_key = 60;
        sample.data.assign(kPeriod, 1000);
        font.samples.push_back(sample);
        const double seconds = SecondsOfFall(FramesOf(fade, 255, 2), 255, 2);
        const auto timecents = static_cast<int16_t>(std::lround(1200.0 * std::log2(seconds)));
        font.instruments.push_back({"decay",
                                    {{{G::Value(sf2::kDecayVolEnv, timecents), G::Value(sf2::kSustainVolEnv, 1440),
                                       G::Value(sf2::kSampleId, 0)},
                                      {}}}});
        font.presets.push_back({"decay", 1, 0, {{{G::Value(sf2::kInstrument, 0)}, {}}}});
        const GbaSoundFontVoices voices = GbaVoicesOfSoundFont(sf2::Write(font), {});
        const hokuto::edit::GbaSoundFontRow& row = voices.presets.at(0).rows.at(0);
        XSFLIB_CHECK(row.voice.sustain == 0);
        XSFLIB_CHECK(FramesOf(row.voice.decay, 255, 2) == FramesOf(fade, 255, 2));
    }
}

// Only a scale tuning of 0 at the mixer's rate becomes a voice of fixed pitch. A sample at a lower rate, or 2 semitones
// off the mixer's rate, stays an ordinary voice.
void TestFixedPitch()
{
    // A scale tuning of 0 at the mixer's rate is the driver's voice of fixed pitch, over every key.
    ntdlib::mp2k::DriverInfo driver;
    driver.mix_rate = kRate;
    const GbaSoundFontVoices voices = GbaVoicesOfSoundFont(TestFont(), driver);
    const hokuto::edit::GbaSoundFontPreset& fixed = PresetNamed(voices, "fixed");
    XSFLIB_CHECK(!fixed.drums);
    SEQLIB_REQUIRE(fixed.rows.size() == 1);
    XSFLIB_CHECK(fixed.rows[0].first_key == 0 && fixed.rows[0].last_key == 127);
    XSFLIB_CHECK(fixed.rows[0].voice.type == ntdlib::mp2k::kVoiceFixed);
    // The other presets stay ordinary voices.
    XSFLIB_CHECK(PresetNamed(voices, "envelope").rows.at(0).voice.type == 0);

    // A sample imported at a lower rate would play too fast at the mixer rate.
    const GbaSoundFontVoices lower = GbaVoicesOfSoundFont(TestFont(), driver, kRate / 2);
    XSFLIB_CHECK(PresetNamed(lower, "fixed").drums);

    // 2 semitones off the mixer's rate is no fixed pitch.
    driver.mix_rate = static_cast<int>(std::lround(kRate * std::exp2(2.0 / 12.0)));
    XSFLIB_CHECK(PresetNamed(GbaVoicesOfSoundFont(TestFont(), driver), "fixed").drums);
}

// The samples take back the driver's master volume as far as their peak allows, and Camelot's mixer leaves the master
// volume out.
void TestMasterVolume()
{
    // The saw starts at -25000, and its highest point gives the peak. The first point of each sample shows the gain:
    // the driver scales the samples by (master volume + 1) / 16, and the points take that back as far as the peak
    // allows.
    const double peak = std::max(25000.0, static_cast<double>(kPeriod - 1) * 800.0 - 25000.0);
    const auto first = [](const ntdlib::mp2k::DriverInfo& driver)
    {
        const GbaSoundFontVoices voices = GbaVoicesOfSoundFont(TestFont(), driver);
        return static_cast<int8_t>(voices.samples.front().at(16));
    };
    const auto near = [](int point, double expected)
    {
        return std::abs(point - expected / 256.0) <= 1.0;
    };
    ntdlib::mp2k::DriverInfo driver;
    XSFLIB_CHECK(near(first(driver), -25000.0));
    driver.master_volume = 13;
    XSFLIB_CHECK(near(first(driver), -25000.0 * 16.0 / 14.0));
    // Twice the level wouldn't fit.
    driver.master_volume = 7;
    XSFLIB_CHECK(near(first(driver), -25000.0 * 32768.0 / peak));
    // Camelot's mixer leaves the master volume out.
    driver.camelot_mixer = true;
    XSFLIB_CHECK(near(first(driver), -25000.0));
}

// The two voices of a stereo sample make one voice at the centre. The sample of that voice contains the mean of the
// channels. Each point of this right channel has the value of the left channel, negated. The mean is silence.
void TestStereo()
{
    using G = sf2::Generator;
    sf2::SoundFont font;
    sf2::Sample left;
    left.name = "left";
    left.sample_rate = kRate;
    left.original_key = 60;
    left.type = sf2::kLeft;
    left.link = 1;
    for (std::size_t i = 0; i < 32 * kPeriod; ++i)
    {
        left.data.push_back(static_cast<int16_t>(static_cast<int>(i % kPeriod) * 800 - 25000));
    }
    sf2::Sample right = left;
    right.name = "right";
    right.type = sf2::kRight;
    right.link = 0;
    std::ranges::transform(right.data, right.data.begin(), [](int16_t point) { return static_cast<int16_t>(-point); });
    font.samples.push_back(left);
    font.samples.push_back(right);
    const auto zone = [](int pan, int sample)
    {
        return sf2::Zone{{G::Value(sf2::kPan, pan), G::Value(sf2::kSampleId, sample)}, {}};
    };
    font.instruments.push_back({"stereo", {zone(-500, 0), zone(500, 1)}});
    font.presets.push_back({"stereo", 0, 128, {{{G::Value(sf2::kInstrument, 0)}, {}}}});

    const GbaSoundFontVoices voices = GbaVoicesOfSoundFont(sf2::Write(font), {});
    XSFLIB_CHECK(std::ranges::none_of(
        voices.warnings, [](const std::string& line) { return line.find("layers") != std::string::npos; }));
    SEQLIB_REQUIRE(voices.samples.size() == 1 && voices.presets.size() == 1);
    XSFLIB_CHECK(std::ranges::all_of(
        voices.presets[0].rows, [](const hokuto::edit::GbaSoundFontRow& row) { return row.voice.pan_sweep == 0; }));
    const std::span<const uint8_t> points = std::span<const uint8_t>(voices.samples[0]).subspan(16);
    XSFLIB_CHECK(!points.empty());
    XSFLIB_CHECK(std::ranges::all_of(points, [](uint8_t point) { return point == 0; }));
}

// Each preset gets its program number where that's free, and the lowest free number where it isn't. A preset past the
// free programs gets none.
void TestPrograms()
{
    const GbaSoundFontVoices voices = GbaVoicesOfSoundFont(TestFont(), {});

    using Programs = std::vector<std::optional<uint32_t>>;
    using hokuto::edit::GbaSoundFontPrograms;

    // Each gets its number where that's free. The kit's number, 0, isn't free here, and 2 is the lowest that's left.
    const std::vector<uint32_t> free = {2, 3, 4, 5, 6, 9};
    XSFLIB_CHECK(GbaSoundFontPrograms(voices, free, {}) == (Programs{3, 4, 5, 6, 2}));

    // A preset past the free programs gets none, and an earlier preset doesn't get the number of a later one.
    const std::vector<uint32_t> few = {5, 6};
    XSFLIB_CHECK(GbaSoundFontPrograms(voices, few, {}) == (Programs{std::nullopt, std::nullopt, 5, 6, std::nullopt}));
    XSFLIB_CHECK(GbaSoundFontPrograms(voices, {}, {}) == Programs(5));

    // A program that has a voice that no song selects gets a preset of its number. A preset without one gets an empty
    // program before such a program.
    const std::vector<uint32_t> empty = {50};
    const std::vector<uint32_t> unplayed = {3, 7, 8};
    XSFLIB_CHECK(GbaSoundFontPrograms(voices, empty, unplayed) == (Programs{3, 50, 7, 8, std::nullopt}));
}

// `count` samples of the left channel of a note of `program` at `key`, on the song's voice group.
std::vector<float> NoteOf(const std::shared_ptr<const hokuto::song::GbaRom>& rom, int program, int key,
                          std::size_t count)
{
    hokuto::song::GbaNotePreview preview(rom, hokuto::song::GbaSongs(*rom).front(), program, key, 127,
                                         std::make_shared<const std::atomic<bool>>(false));
    std::vector<float> left;
    std::vector<float> block(2 * 1024);
    while (left.size() < count)
    {
        const std::size_t got = preview.Render(block.data(), 1024);
        if (got == 0)
        {
            break;
        }
        for (std::size_t i = 0; i < got; ++i)
        {
            left.push_back(block[2 * i]);
        }
    }

    return left;
}

// The frequency of a saw: the upward crossings of zero between the first and the last, in Hz at `rate`.
double FrequencyOf(std::span<const float> samples, uint32_t rate)
{
    std::optional<std::size_t> first;
    std::size_t last = 0;
    std::size_t crossings = 0;
    for (std::size_t i = 1; i < samples.size(); ++i)
    {
        if (samples[i - 1] < 0.0F && samples[i] >= 0.0F)
        {
            first = first.value_or(i);
            last = i;
            ++crossings;
        }
    }

    return crossings < 2 ? 0.0 : static_cast<double>(crossings - 1) * rate / static_cast<double>(last - *first);
}

double LevelOf(std::span<const float> samples)
{
    double sum = 0.0;
    for (const float sample : samples)
    {
        sum += double(sample) * double(sample);
    }

    return std::sqrt(sum / static_cast<double>(samples.size()));
}

// A cartridge of one song on a voice group of one sample, with room after it.
struct SmallCart
{
    std::vector<uint8_t> bytes;
    uint32_t voices = 0;
};

SmallCart MakeCart()
{
    using enum mp2k::Command;

    mp2k_cart::Cart cart;
    SmallCart made;
    std::vector<int8_t> pcm(64);
    for (std::size_t i = 0; i < pcm.size(); ++i)
    {
        pcm[i] = static_cast<int8_t>(static_cast<int>(i) - 32);
    }
    const uint32_t sample = cart.Wave(pcm, 13379, 0);
    cart.Place({0, 0, 0, 0});
    made.voices = cart.VoiceGroup({{0, mp2k_cart::Cart::Voice(0, sample, 60, {255, 0, 255, 165})}});
    mp2k_cart::Track track;
    track.Cmd(kCmdTempo, 75).Cmd(kCmdVol, 100);
    track.Cmd(kCmdVoice, 0).Note(24, 60, 100).Wait(24);
    track.Fine();
    cart.Song({track}, made.voices);

    const ntdlib::agb::Rom rom = cart.ToRom();
    made.bytes = hokuto::testing::RomBytes(rom);
    made.bytes.resize(made.bytes.size() + 0x40000, 0xFF);

    return made;
}

// The presets are imported into a voice group, and the driver's model plays them.
void TestPlays()
{
    const SmallCart cart = MakeCart();
    const File file = hokuto::format::ImportGbaRom(cart.bytes);
    const std::shared_ptr<const hokuto::song::GbaRom> rom = hokuto::song::LoadGbaRom(cart.bytes);
    const GbaSoundFontVoices voices = GbaVoicesOfSoundFont(TestFont(), {});
    const hokuto::edit::GbaGroupVersion version = hokuto::edit::WithGbaSoundFont(file, *rom, cart.voices, voices);
    const std::vector<uint8_t> exported = hokuto::format::ExportArchive(version.file);
    const std::shared_ptr<const hokuto::song::GbaRom> built = hokuto::song::LoadGbaRom(exported);

    // The song plays as before, from the table that its header names now.
    XSFLIB_CHECK(hokuto::song::GbaSongsWithOtherVoices(*rom, *built).empty());
    XSFLIB_CHECK(hokuto::edit::UnplayedGbaPrograms(*built, version.group).size() == 5);

    // A voice of one sample, a key split, a sample again for the quiet preset and a drum kit for the two others. Preset
    // 0 of MIDI bank 128 finds program 0 taken, and gets the lowest that's free.
    const auto type_of = [&built, &version](int program)
    {
        mp2k::Voice voice;
        XSFLIB_CHECK(mp2k::ReadVoice(built->rom, version.group + static_cast<uint32_t>(program) * 12, voice));
        return voice.type;
    };
    XSFLIB_CHECK(type_of(3) == 0 && type_of(5) == 0);
    XSFLIB_CHECK(type_of(4) == mp2k::kVoiceKeySplit);
    XSFLIB_CHECK(type_of(6) == mp2k::kVoiceDrumKit);
    XSFLIB_CHECK(type_of(1) == mp2k::kVoiceDrumKit);

    // A preset at the mixer's rate imports as a fixed-pitch voice of the driver.
    ntdlib::mp2k::DriverInfo at_mixer_rate = rom->info;
    at_mixer_rate.mix_rate = kRate;
    const hokuto::edit::GbaGroupVersion fixed_version =
        hokuto::edit::WithGbaSoundFont(file, *rom, cart.voices, GbaVoicesOfSoundFont(TestFont(), at_mixer_rate));
    const std::shared_ptr<const hokuto::song::GbaRom> fixed_rom =
        hokuto::song::LoadGbaRom(hokuto::format::ExportArchive(fixed_version.file));
    mp2k::Voice fixed_voice;
    XSFLIB_CHECK(mp2k::ReadVoice(fixed_rom->rom, fixed_version.group + 6 * 12, fixed_voice));
    XSFLIB_CHECK(fixed_voice.type == mp2k::kVoiceFixed);

    const auto rate =
        static_cast<uint32_t>(hokuto::song::GbaNotePreview(built, hokuto::song::GbaSongs(*built).front(), 4, 60, 127,
                                                           std::make_shared<const std::atomic<bool>>(false))
                                  .SampleRate());
    constexpr double kBase = static_cast<double>(kRate) / kPeriod;
    const auto within = [](double frequency, double expected)
    {
        return std::abs(frequency - expected) < expected * 0.01;
    };
    const auto frequency = [&built, rate](int program, int key)
    {
        return FrequencyOf(NoteOf(built, program, key, rate), rate);
    };

    // Key 60 plays the saw at its root, and an octave up at twice the frequency, on both rows of the key split.
    XSFLIB_CHECK(within(frequency(4, 60), kBase));
    XSFLIB_CHECK(within(frequency(4, 72), 2 * kBase));
    XSFLIB_CHECK(within(frequency(4, 48), kBase / 2));

    // The drum kit's voices play at their keys. A key that no row covers plays nothing.
    XSFLIB_CHECK(within(frequency(1, 36), kBase / 4));
    XSFLIB_CHECK(within(frequency(1, 38), kBase / 4 * std::exp2(2.0 / 12.0)));
    const std::vector<float> no_row = NoteOf(built, 1, 40, rate / 4);
    XSFLIB_CHECK(no_row.empty() || LevelOf(no_row) == 0.0);

    // A scale tuning of 0 plays the root's pitch at every key.
    XSFLIB_CHECK(within(frequency(6, 40), kBase));
    XSFLIB_CHECK(within(frequency(6, 90), kBase));

    // The quiet preset plays 20 dB below the key split's second row. That row has the same envelope but for the
    // release.
    const double loud = LevelOf(NoteOf(built, 4, 60, rate / 2));
    const double quiet = LevelOf(NoteOf(built, 5, 60, rate / 2));
    XSFLIB_CHECK(loud > 0.0);
    XSFLIB_CHECK(std::abs(20.0 * std::log10(quiet / loud) + 20.0) < 1.0);
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("gba_sound_fonts",
                                    []
                                    {
                                        TestValues();
                                        TestPlaysPastRelease();
                                        TestLowerRate();
                                        TestHighRate();
                                        TestUnpitched();
                                        TestFades();
                                        TestCamelotRelease();
                                        TestFixedPitch();
                                        TestMasterVolume();
                                        TestStereo();
                                        TestPrograms();
                                        TestPlays();
                                    });
}
