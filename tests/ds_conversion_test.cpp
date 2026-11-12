// SPDX-License-Identifier: MIT

// What a sound of a later console becomes on the DS. The commands' expected bytes come from the DS's SSEQ format: 0xFE
// and a mask allocate tracks, 0x93 opens a track, 0x94 jumps, 0xE1 sets the tempo in 16 bits, 0xC1 the volume, 0xC3 the
// transposition, 0xD0 to 0xD3 the envelope, a key below 0x80 starts a note and 0xFF ends a track. The DS plays 48 ticks
// to a quarter note. A song of 96 ticks therefore doubles its tempo. The banks' expected values come from the format of
// an SBNK and an SWAR, and from the source's values: the pitch of a region times the rate of its wave gives the rate on
// the DS, the region volume over 127 scales the samples, and a stereo wave becomes the mean of its channels.

#include "edit/ds_conversion.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/commands/units.h"
#include "ntdlib/ntr/snd/commands.h"
#include "ntdlib/sar/bank_file.h"
#include "ntdlib/sar/wave_file.h"
#include "ntdlib/sdat/decode.h"
#include "ntdlib/sdat/formats.h"
#include "xsflib/testing/check.h"

#include "edit/ds_waves.h"
#include "edit/waves.h"
#include "song/archive.h"

namespace
{

namespace commands = ntdlib::commands;
namespace edit = hokuto::edit;
namespace sar = ntdlib::sar;
namespace sdat = ntdlib::sdat;
namespace song = hokuto::song;

using commands::Argument;
using commands::Command;
using commands::Kind;
using commands::Sequence;
using commands::Source;

using Bytes = std::vector<uint8_t>;

const song::Console kSwitch{.engine = hokuto::format::Engine::kSwitch};

// A command that sets `kind` to `value`, in the unit's steps of 1.
Command Set(Kind kind, int64_t value, uint8_t index = 0)
{
    return Command{.kind = kind, .index = index, .value = Argument{.value = value * commands::kOne}};
}

// A note command.
Command Note(uint8_t key, int64_t velocity, int64_t length)
{
    return Command{.kind = Kind::kNote,
                   .key = key,
                   .velocity = velocity * commands::kOne,
                   .value = Argument{.value = length * commands::kOne}};
}

// A command whose target is item `target`.
Command Goes(Kind kind, uint32_t target, uint8_t index = 0)
{
    return Command{.kind = kind, .index = index, .target = target};
}

const Command kEnd{.kind = Kind::kEnd};

// The bytes that the DS's encoder writes for `sequence`.
Bytes BytesOf(const Sequence& sequence)
{
    return ntdlib::ntr::snd::EncodeSequence(sequence).bytes;
}

// A timebase, a tempo from the song's start, the tracks and the commands that the DS's bytes hold and those they don't.
void TestCommands()
{
    Command timed = Set(Kind::kVolume, 100);
    timed.time = Argument{.value = 24 * commands::kOne};
    const Command send = Set(Kind::kSend, 20, 2);
    const Command lfo = Set(Kind::kLfoDepth, 20, 1);
    Sequence source;
    source.items = {
        Set(Kind::kTimebase, 96),
        Set(Kind::kTempo, 150),
        Set(Kind::kProgram, 5),
        timed,
        send,
        lfo,
        Set(Kind::kTranspose, 200),
        Set(Kind::kVolume, 200),
        Note(60, 130, 96),
        Command{.kind = Kind::kEnvelopeReset},
        kEnd,
        Note(70, 100, 48),
    };

    const edit::DsCommands result = edit::AsDs(source, 0, 0b11, kSwitch);

    const Bytes expected = {
        0xFE, 0x03, 0x00,       // tracks 0 and 1
        0xE1, 0x2C, 0x01,       // a tempo of 300: 150 at 96 ticks to a quarter note
        0x81, 0x05,             // program 5
        0xC1, 0x64,             // volume 100, without its time
        0xC3, 0x02,             // transposition by 2 semitones
        0xC1, 0x7F,             // volume 200 is written as 127
        0x3C, 0x7F, 0x60,       // key 60, velocity 127, 96 ticks
        0xD0, 0xFF, 0xD1, 0xFF, // the envelope's reset: the region's values
        0xD2, 0xFF, 0xD3, 0xFF,
        0xFF, // the end
    };
    XSFLIB_CHECK(BytesOf(result.sequence) == expected);
    XSFLIB_CHECK(result.sequence.labels.empty());
    XSFLIB_CHECK((result.left_out ==
                  std::map<std::string, std::size_t>{{song::SettingName(send), 1}, {song::SettingName(lfo), 1}}));
    XSFLIB_CHECK(result.times == 1);
    XSFLIB_CHECK(result.held_values == 2);
    XSFLIB_CHECK(result.timebase == 96 && !result.timebase_changes && result.unscaled_tempos == 0);
    XSFLIB_CHECK(result.deepest_nest == 0);
}

// A timebase without a tempo keeps the song's speed: the later consoles start at 120 beats a minute. Another timebase
// later in the song is reported, and a tempo from a variable can't take the timebase.
void TestTimebase()
{
    Sequence source;
    source.items = {Set(Kind::kTimebase, 96), Note(60, 100, 24), kEnd};
    XSFLIB_CHECK(BytesOf(edit::AsDs(source, 0, 1, kSwitch).sequence) ==
                 (Bytes{0xE1, 0xF0, 0x00, 0x3C, 0x64, 0x18, 0xFF}));

    source.items = {Note(60, 100, 24), Set(Kind::kTimebase, 96), kEnd};
    const edit::DsCommands later = edit::AsDs(source, 0, 1, kSwitch);
    XSFLIB_CHECK(later.timebase == 48 && later.timebase_changes);
    XSFLIB_CHECK(BytesOf(later.sequence) == (Bytes{0x3C, 0x64, 0x18, 0xFF}));

    Command variable_tempo{.kind = Kind::kTempo, .value = Argument{.source = Source::kVariable, .value = 3}};
    source.items = {Set(Kind::kTimebase, 96), variable_tempo, Note(60, 100, 24), kEnd};
    const edit::DsCommands unscaled = edit::AsDs(source, 0, 1, kSwitch);
    XSFLIB_CHECK(unscaled.unscaled_tempos == 1);
    XSFLIB_CHECK(unscaled.sequence.items.at(0).value == variable_tempo.value);
}

// A sound plays from the file's first command on the DS. A track that the sound opens before its start stays, and a
// jump leads past it. No jump is required for the commands that the sound doesn't reach: they go.
void TestStart()
{
    Sequence source;
    source.items = {
        Note(60, 100, 48),            // 0: track 1
        kEnd,                         // 1
        Goes(Kind::kOpenTrack, 0, 1), // 2: the sound's start
        Note(62, 100, 48),            // 3
        kEnd,                         // 4
    };
    const Bytes expected = {
        0xFE, 0x03, 0x00,             // 0: tracks 0 and 1
        0x94, 0x0B, 0x00, 0x00,       // 3: a jump to the sound's start
        0x3C, 0x64, 0x30,             // 7: track 1
        0xFF,                         // 10
        0x93, 0x01, 0x07, 0x00, 0x00, // 11: opens track 1
        0x3E, 0x64, 0x30,             // 16
        0xFF,                         // 19
    };
    XSFLIB_CHECK(BytesOf(edit::AsDs(source, 2, 0b11, kSwitch).sequence) == expected);

    // Another sound's commands go. A loop back to the sound's start lands on the new start.
    source.items = {Note(70, 100, 48), kEnd, Note(60, 100, 48), Goes(Kind::kJump, 2)};
    XSFLIB_CHECK(BytesOf(edit::AsDs(source, 2, 1, kSwitch).sequence) ==
                 (Bytes{0x3C, 0x64, 0x30, 0x94, 0x00, 0x00, 0x00}));

    // A last command that runs on gets an end after it.
    source.items = {Note(60, 100, 48)};
    XSFLIB_CHECK(BytesOf(edit::AsDs(source, 0, 1, kSwitch).sequence) == (Bytes{0x3C, 0x64, 0x30, 0xFF}));
}

// The DS has no variables of a track. They take the sound's variables that the commands leave free, from the highest.
void TestVariables()
{
    Sequence source;
    source.items = {
        Set(Kind::kSetVariable, 1, 32),
        Set(Kind::kSetVariable, 2, 15),
        Command{.kind = Kind::kVolume, .value = Argument{.source = Source::kVariable, .value = 33}},
        Note(60, 100, 48),
        kEnd,
    };

    const edit::DsCommands result = edit::AsDs(source, 0, 1, kSwitch);

    XSFLIB_CHECK((result.moved_variables == std::map<int, int>{{32, 14}, {33, 13}}));
    XSFLIB_CHECK(result.stuck_variables.empty());
    XSFLIB_CHECK(result.sequence.items.at(0).index == 14 && result.sequence.items.at(1).index == 15);
    XSFLIB_CHECK(result.sequence.items.at(2).value.value == 13);

    // Without a free variable, the track's variable stays, and the caller refuses the sound.
    Sequence full;
    for (uint8_t number = 0; number < 16; ++number)
    {
        full.items.push_back(Set(Kind::kSetVariable, 1, number));
    }
    full.items.push_back(Set(Kind::kSetVariable, 1, 40));
    full.items.push_back(kEnd);
    XSFLIB_CHECK((edit::AsDs(full, 0, 1, kSwitch).stuck_variables == std::vector<int>{40}));
}

// The driver supports 3 calls and counted loops inside one another.
void TestNesting()
{
    const auto nested = [](int depth)
    {
        Sequence sequence;
        sequence.items = {Goes(Kind::kCall, 2), kEnd};
        for (int level = 1; level < depth; ++level)
        {
            sequence.items.push_back(Goes(Kind::kCall, static_cast<uint32_t>(2 + 2 * level)));
            sequence.items.push_back(Command{.kind = Kind::kReturn});
        }
        sequence.items.push_back(Note(60, 100, 48));
        sequence.items.push_back(Command{.kind = Kind::kReturn});

        return sequence;
    };

    XSFLIB_CHECK(edit::AsDs(nested(3), 0, 1, kSwitch).deepest_nest == 3);
    XSFLIB_CHECK(edit::AsDs(nested(4), 0, 1, kSwitch).deepest_nest == 4);
}

// A wave of 16-bit PCM in one channel for each of `channels`.
sar::WaveFile WaveOf(const std::vector<std::vector<int16_t>>& channels, uint32_t rate,
                     std::optional<uint32_t> loop_start = std::nullopt)
{
    sar::WaveFile wave;
    wave.flavor = sar::Flavor::kNx;
    wave.encoding = sar::WaveEncoding::kPcm16;
    wave.sample_rate = rate;
    wave.loop = loop_start.has_value();
    wave.loop_start = loop_start.value_or(0);
    wave.loop_end = static_cast<uint32_t>(channels.front().size());
    for (const std::vector<int16_t>& samples : channels)
    {
        sar::WaveFile::Channel& channel = wave.channels.emplace_back();
        for (const int16_t sample : samples)
        {
            channel.samples.push_back(static_cast<uint8_t>(sample & 0xFF));
            channel.samples.push_back(static_cast<uint8_t>((sample >> 8) & 0xFF));
        }
    }

    return wave;
}

// `count` samples of a sine of `amplitude`, with a period of 64 samples.
std::vector<int16_t> Sine(std::size_t count, double amplitude)
{
    std::vector<int16_t> samples(count);
    for (std::size_t i = 0; i < count; ++i)
    {
        samples[i] = static_cast<int16_t>(std::lround(amplitude * std::sin(2.0 * 3.14159265358979 * i / 64.0)));
    }

    return samples;
}

// The RMS level of `samples`.
double RmsOf(const std::vector<int16_t>& samples)
{
    double sum = 0.0;
    for (const int16_t sample : samples)
    {
        sum += static_cast<double>(sample) * sample;
    }

    return samples.empty() ? 0.0 : std::sqrt(sum / static_cast<double>(samples.size()));
}

// A region of a velocity run that plays entry `wave` of the bank's table.
sar::BankFile::Region RegionOf(uint32_t wave, uint8_t first_velocity = 0, uint8_t last_velocity = 127)
{
    sar::BankFile::Region region;
    region.first_velocity = first_velocity;
    region.last_velocity = last_velocity;
    region.values.wave_id_index = wave;
    region.values.adshr = {127, 127, 127, 0, 127};

    return region;
}

// An instrument whose key runs each play their regions.
sar::BankFile::Instrument InstrumentOf(const std::vector<sar::BankFile::KeyRegion>& keys)
{
    sar::BankFile::Instrument instrument;
    instrument.key_regions = keys;

    return instrument;
}

double NoStereoGain(const edit::MonoLevel&)
{
    return 1.0;
}

// The instrument types, from one region up to a drum set, and a region's values.
void TestInstruments()
{
    sar::BankFile::Region values = RegionOf(0);
    values.values.original_key = 67;
    values.values.pan = 30;
    values.values.adshr = {100, 90, 80, 0, 70};

    const auto play = RegionOf(0);
    sar::BankFile bank;
    bank.instruments.emplace_back(InstrumentOf({{0, 127, {values}}}));
    bank.instruments.emplace_back();
    bank.instruments.emplace_back(InstrumentOf({{0, 59, {play}}, {60, 127, {RegionOf(1)}}}));
    bank.instruments.emplace_back(InstrumentOf({{10, 20, {play}}, {30, 40, {RegionOf(1)}}}));
    std::vector<sar::BankFile::KeyRegion> many;
    for (uint8_t key = 10; key < 100; key = static_cast<uint8_t>(key + 10))
    {
        many.push_back({key, key, {play}});
    }
    bank.instruments.emplace_back(InstrumentOf(many));
    bank.instruments.emplace_back(InstrumentOf({{0, 0, {play}}, {5, 127, {RegionOf(1)}}}));
    const std::vector<std::optional<sar::WaveFile>> waves = {WaveOf({Sine(64, 1000.0)}, 32000),
                                                             WaveOf({Sine(64, 2000.0)}, 32000)};

    const edit::DsBankFiles files = edit::DsBankFilesOf(bank, waves, NoStereoGain);

    const sdat::Bank made = sdat::Bank::Parse(files.bank);
    SEQLIB_REQUIRE(made.instruments.size() == 6);
    const sdat::Instrument& one = made.instruments[0];
    XSFLIB_CHECK(one.type == sdat::InstrumentType::kPcm);
    XSFLIB_CHECK(one.region.wave == 0 && one.region.wave_archive == 0 && one.region.root_key == 67 &&
                 one.region.pan == 30 && one.region.attack == 100 && one.region.decay == 90 &&
                 one.region.sustain == 80 && one.region.release == 70);
    XSFLIB_CHECK(made.instruments[1].type == sdat::InstrumentType::kNone);

    const sdat::Instrument& split = made.instruments[2];
    XSFLIB_CHECK(split.type == sdat::InstrumentType::kKeySplit);
    XSFLIB_CHECK(split.split_keys[0] == 59 && split.split_keys[1] == 127 && split.split_keys[2] == 0);
    XSFLIB_CHECK(split.regions.size() == 2 && split.regions[0].wave == 0 && split.regions[1].wave == 1);

    const sdat::Instrument& gaps = made.instruments[3];
    XSFLIB_CHECK(gaps.type == sdat::InstrumentType::kKeySplit);
    XSFLIB_CHECK(gaps.split_keys[0] == 9 && gaps.split_keys[1] == 20 && gaps.split_keys[2] == 29 &&
                 gaps.split_keys[3] == 40);
    XSFLIB_CHECK(gaps.regions.size() == 4 && gaps.regions[0].type == sdat::InstrumentType::kNone &&
                 gaps.regions[1].type == sdat::InstrumentType::kPcm &&
                 gaps.regions[2].type == sdat::InstrumentType::kNone && gaps.regions[3].wave == 1);

    // Nine keys with gaps between them take more than 8 runs.
    const sdat::Instrument& drums = made.instruments[4];
    XSFLIB_CHECK(drums.type == sdat::InstrumentType::kDrumSet && drums.min_key == 10 && drums.max_key == 90);
    XSFLIB_CHECK(drums.regions.size() == 81 && drums.regions[0].type == sdat::InstrumentType::kPcm &&
                 drums.regions[1].type == sdat::InstrumentType::kNone &&
                 drums.regions[80].type == sdat::InstrumentType::kPcm);

    // A first key of 0 can't end a split's first run: a key of 0 ends the list.
    const sdat::Instrument& from_zero = made.instruments[5];
    XSFLIB_CHECK(from_zero.type == sdat::InstrumentType::kDrumSet && from_zero.min_key == 0 &&
                 from_zero.max_key == 127);
    XSFLIB_CHECK(from_zero.regions.size() == 128 && from_zero.regions[1].type == sdat::InstrumentType::kNone &&
                 from_zero.regions[5].wave == 1);

    XSFLIB_CHECK(sdat::WaveArchive::Parse(files.waves).waves.size() == 2);
    XSFLIB_CHECK(files.scale == 1.0 && files.stereo_waves == 0 && files.velocity_splits == 0);
}

// A note of the DS plays one region at a key: the one with the widest run of velocities, or the higher of two such. The
// other values with no place count.
void TestRegionValues()
{
    sar::BankFile::Region held = RegionOf(2);
    held.values.adshr = {127, 100, 90, 50, 127};
    held.values.key_group = 3;
    held.values.ignore_note_off = true;

    sar::BankFile bank;
    bank.instruments.emplace_back(InstrumentOf({{0, 127, {RegionOf(0, 0, 100), RegionOf(1, 101, 127)}}}));
    bank.instruments.emplace_back(InstrumentOf({{0, 127, {RegionOf(0, 0, 63), RegionOf(1, 64, 127)}}}));
    bank.instruments.emplace_back(InstrumentOf({{0, 127, {held}}}));
    bank.instruments.emplace_back(InstrumentOf({{0, 127, {RegionOf(3)}}}));
    const std::vector<std::optional<sar::WaveFile>> waves = {WaveOf({Sine(64, 1000.0)}, 32000),
                                                             WaveOf({Sine(64, 2000.0)}, 32000),
                                                             WaveOf({Sine(64, 3000.0)}, 32000), std::nullopt};

    const edit::DsBankFiles files = edit::DsBankFilesOf(bank, waves, NoStereoGain);

    const sdat::Bank made = sdat::Bank::Parse(files.bank);
    SEQLIB_REQUIRE(made.instruments.size() == 4);
    XSFLIB_CHECK(made.instruments[0].region.wave == 0);
    XSFLIB_CHECK(made.instruments[1].region.wave == 1);
    XSFLIB_CHECK(made.instruments[2].region.wave == 2 && made.instruments[2].region.release == 0xFF);
    XSFLIB_CHECK(made.instruments[3].type == sdat::InstrumentType::kNone);
    XSFLIB_CHECK(files.velocity_splits == 2 && files.holds == 1 && files.key_groups == 1 && files.silent_regions == 1);
}

// The pitch of a region is converted into the rate of the wave, and its volume into the samples. A wave is added once
// for each pair of rate and level.
void TestRatesAndLevels()
{
    constexpr double kAmplitude = 16000.0;
    sar::BankFile::Region up = RegionOf(0);
    up.values.pitch = 2.0f;
    sar::BankFile::Region quiet = RegionOf(0);
    quiet.values.volume = 64;
    sar::BankFile::Region octaves = RegionOf(1);
    octaves.values.pitch = 8.0f;
    octaves.values.original_key = 60;
    sar::BankFile::Region low = RegionOf(2);
    low.values.pitch = 0.25f;
    low.values.original_key = 60;

    sar::BankFile bank;
    bank.instruments.emplace_back(InstrumentOf({{0, 127, {RegionOf(0)}}}));
    bank.instruments.emplace_back(InstrumentOf({{0, 127, {RegionOf(0)}}}));
    bank.instruments.emplace_back(InstrumentOf({{0, 127, {up}}}));
    bank.instruments.emplace_back(InstrumentOf({{0, 127, {quiet}}}));
    bank.instruments.emplace_back(InstrumentOf({{0, 127, {octaves}}}));
    bank.instruments.emplace_back(InstrumentOf({{0, 127, {low}}}));
    const std::vector<std::optional<sar::WaveFile>> waves = {
        WaveOf({Sine(640, kAmplitude)}, 32000), WaveOf({Sine(64, 1000.0)}, 48000), WaveOf({Sine(64, 1000.0)}, 1000)};

    const edit::DsBankFiles files = edit::DsBankFilesOf(bank, waves, NoStereoGain, edit::kHighestDsRate);

    const sdat::Bank made = sdat::Bank::Parse(files.bank);
    const sdat::WaveArchive archive = sdat::WaveArchive::Parse(files.waves);
    SEQLIB_REQUIRE(archive.waves.size() == 5);
    XSFLIB_CHECK(made.instruments[0].region.wave == 0 && made.instruments[1].region.wave == 0);
    XSFLIB_CHECK(archive.waves[0].encoding == sdat::WaveEncoding::kImaAdpcm && archive.waves[0].sample_rate == 32000);

    XSFLIB_CHECK(made.instruments[2].region.wave == 1 && archive.waves[1].sample_rate == 64000);
    const double whole = RmsOf(sdat::DecodeWave(archive.waves[0]));
    XSFLIB_CHECK(std::abs(whole / (kAmplitude / std::sqrt(2.0)) - 1.0) < 0.03);

    XSFLIB_CHECK(made.instruments[3].region.wave == 2 && archive.waves[2].sample_rate == 32000);
    const double scaled = RmsOf(sdat::DecodeWave(archive.waves[2]));
    XSFLIB_CHECK(std::abs(scaled / whole - 64.0 / 127.0) < 0.02);

    // 48,000 Hz at 8 times is 384,000 Hz. A DS wave supports 192,000 Hz at most. The root key moves an octave instead.
    // The timer of 192,000 Hz is 16,756,991 / 192,000.
    XSFLIB_CHECK(made.instruments[4].region.root_key == 48 && archive.waves[3].timer == 87);
    XSFLIB_CHECK(files.rates_held == 0);

    // 1,000 Hz at a quarter is 250 Hz. A DS wave supports 256 Hz at least. The root key moves an octave instead.
    XSFLIB_CHECK(made.instruments[5].region.root_key == 72 && archive.waves[4].sample_rate == 500);
}

// A stereo wave is converted to the mean of its channels. A gain past 16 bits lowers the level of every wave together.
void TestStereoAndScale()
{
    constexpr double kAmplitude = 20000.0;
    const std::vector<int16_t> sine = Sine(640, kAmplitude);
    std::vector<int16_t> negated = sine;
    for (int16_t& sample : negated)
    {
        sample = static_cast<int16_t>(-sample);
    }

    sar::BankFile bank;
    bank.instruments.emplace_back(InstrumentOf({{0, 127, {RegionOf(0)}}}));
    bank.instruments.emplace_back(InstrumentOf({{0, 127, {RegionOf(1)}}}));
    const std::vector<std::optional<sar::WaveFile>> same = {WaveOf({sine, sine}, 32000),
                                                            WaveOf({sine, negated}, 32000)};

    const edit::DsBankFiles plain = edit::DsBankFilesOf(bank, same, NoStereoGain);

    XSFLIB_CHECK(plain.stereo_waves == 2 && plain.scale == 1.0);
    const sdat::WaveArchive means = sdat::WaveArchive::Parse(plain.waves);
    SEQLIB_REQUIRE(means.waves.size() == 2);
    XSFLIB_CHECK(std::abs(RmsOf(sdat::DecodeWave(means.waves[0])) / (kAmplitude / std::sqrt(2.0)) - 1.0) < 0.03);
    XSFLIB_CHECK(RmsOf(sdat::DecodeWave(means.waves[1])) < 16.0);

    // A gain of 2 would take the first wave to 40,000. Every wave is scaled by 32,767 / 40,000.
    const auto twice = [](const edit::MonoLevel&)
    {
        return 2.0;
    };
    const edit::DsBankFiles loud = edit::DsBankFilesOf(bank, same, twice);
    XSFLIB_CHECK(std::abs(loud.scale - 32767.0 / 40000.0) < 1e-6);
    const sdat::WaveArchive scaled = sdat::WaveArchive::Parse(loud.waves);
    const double expected = kAmplitude * 2.0 * loud.scale / std::sqrt(2.0);
    XSFLIB_CHECK(std::abs(RmsOf(sdat::DecodeWave(scaled.waves[0])) / expected - 1.0) < 0.03);
}

// A wave above the highest rate is converted at that rate, and keeps its level and its pitch. The sine's frequency
// follows from its zero crossings in the time that the new samples last.
void TestRates()
{
    constexpr std::size_t kSamples = 48000;
    constexpr double kAmplitude = 16000.0;
    sar::BankFile bank;
    bank.instruments.emplace_back(InstrumentOf({{0, 127, {RegionOf(0)}}}));
    bank.instruments.emplace_back(InstrumentOf({{0, 127, {RegionOf(1)}}}));
    const std::vector<int16_t> sine = Sine(kSamples, kAmplitude);
    const std::vector<std::optional<sar::WaveFile>> waves = {WaveOf({sine}, 48000), WaveOf({sine}, 32000)};
    const auto crossings = [](const std::vector<int16_t>& samples)
    {
        std::size_t count = 0;
        for (std::size_t i = 1; i < samples.size(); ++i)
        {
            count += (samples[i - 1] < 0) != (samples[i] < 0) ? 1 : 0;
        }
        return count;
    };
    const std::size_t crossings_in = crossings(sine);

    const edit::DsBankFiles mix_rate = edit::DsBankFilesOf(bank, waves, NoStereoGain);

    XSFLIB_CHECK(mix_rate.rates_lowered == 1);
    const sdat::WaveArchive at_mix = sdat::WaveArchive::Parse(mix_rate.waves);
    SEQLIB_REQUIRE(at_mix.waves.size() == 2);
    XSFLIB_CHECK(at_mix.waves[0].sample_rate == edit::kDsMixRate && at_mix.waves[1].sample_rate == 32000);
    const std::vector<int16_t> lowered = sdat::DecodeWave(at_mix.waves[0]);
    XSFLIB_CHECK(std::abs(static_cast<double>(lowered.size()) - edit::kDsMixRate) < 16.0);
    XSFLIB_CHECK(std::abs(RmsOf(lowered) / (kAmplitude / std::sqrt(2.0)) - 1.0) < 0.03);
    XSFLIB_CHECK(crossings(lowered) >= crossings_in - 2 && crossings(lowered) <= crossings_in + 2);
    std::vector<std::string> warnings;
    edit::AddDsBankWarnings(mix_rate, true, warnings);
    XSFLIB_CHECK(warnings ==
                 std::vector<std::string>{"The DS mixes at 32,728 Hz. 1 wave above that rate is resampled to it."});

    // At a lower limit, both waves go in at it.
    const edit::DsBankFiles limited = edit::DsBankFilesOf(bank, waves, NoStereoGain, 16364);

    XSFLIB_CHECK(limited.rates_lowered == 2);
    const sdat::WaveArchive at_limit = sdat::WaveArchive::Parse(limited.waves);
    SEQLIB_REQUIRE(at_limit.waves.size() == 2);
    XSFLIB_CHECK(at_limit.waves[0].sample_rate == 16364 && at_limit.waves[1].sample_rate == 16364);
    XSFLIB_CHECK(std::abs(static_cast<double>(sdat::DecodeWave(at_limit.waves[1]).size()) - 24546.0) < 16.0);
    XSFLIB_CHECK(crossings(sdat::DecodeWave(at_limit.waves[1])) >= crossings_in - 2);
}

// A loop starts at a word of 8 samples, and the wave ends with the loop. A loop start inside a word moves on to the
// next word. A loop that fills no whole words is rounded to the nearest length that does. The wave gets the rate that
// keeps its pitch. A short loop repeats until it fills them. A wave that loops plays no samples past its loop's end.
void TestLoops()
{
    sar::WaveFile on_word = WaveOf({Sine(2500, 1000.0)}, 32000, 1000);
    on_word.loop_end = 2000;
    sar::WaveFile off_word = WaveOf({Sine(2000, 1000.0)}, 32000, 1001);
    sar::WaveFile high = WaveOf({Sine(4000, 1000.0)}, 48000, 1000);
    sar::WaveFile tiny = WaveOf({Sine(100, 1000.0)}, 32000, 88);

    sar::BankFile bank;
    bank.instruments.emplace_back(InstrumentOf({{0, 127, {RegionOf(0)}}}));
    bank.instruments.emplace_back(InstrumentOf({{0, 127, {RegionOf(1)}}}));
    bank.instruments.emplace_back(InstrumentOf({{0, 127, {RegionOf(2)}}}));
    bank.instruments.emplace_back(InstrumentOf({{0, 127, {RegionOf(3)}}}));
    const std::vector<std::optional<sar::WaveFile>> waves = {on_word, off_word, high, tiny};

    const edit::DsBankFiles files = edit::DsBankFilesOf(bank, waves, NoStereoGain);

    const sdat::WaveArchive archive = sdat::WaveArchive::Parse(files.waves);
    SEQLIB_REQUIRE(archive.waves.size() == 4);
    XSFLIB_CHECK(archive.waves[0].loop == 1 && archive.waves[0].LoopStartSample() == 1000);
    XSFLIB_CHECK(archive.waves[0].Samples() == 2000 && archive.waves[0].sample_rate == 32000);

    // 999 samples take 1,000, at 32,000 * 1,000 / 999 Hz. The loop's start moves with them to 1,002, and on to 1,008.
    XSFLIB_CHECK(archive.waves[1].loop == 1 && archive.waves[1].LoopStartSample() == 1008);
    XSFLIB_CHECK(archive.waves[1].Samples() == 2008 && archive.waves[1].sample_rate == 32032);

    // 3,000 samples at 48,000 Hz come to 2,045.5 at 32,728 Hz. The nearest whole words hold 2,048, at
    // 48,000 * 2,048 / 3,000 Hz. The loop's start comes to 683, and moves on to 688.
    XSFLIB_CHECK(archive.waves[2].loop == 1 && archive.waves[2].LoopStartSample() == 688);
    XSFLIB_CHECK(archive.waves[2].Samples() == 688 + 2048 && archive.waves[2].sample_rate == 32768);

    // 12 samples repeat to 24, the first whole words.
    XSFLIB_CHECK(archive.waves[3].loop == 1 && archive.waves[3].LoopStartSample() == 88);
    XSFLIB_CHECK(archive.waves[3].Samples() == 88 + 24 && archive.waves[3].sample_rate == 32000);
    XSFLIB_CHECK(files.lost_loops == 0);
}

// The size before the waves are added counts each wave that a region plays, and no other.
void TestSize()
{
    sar::BankFile bank;
    bank.instruments.emplace_back(InstrumentOf({{0, 127, {RegionOf(0)}}}));
    bank.instruments.emplace_back(InstrumentOf({{0, 127, {RegionOf(0)}}}));
    const std::vector<std::optional<sar::WaveFile>> waves = {WaveOf({Sine(1000, 1000.0)}, 32000),
                                                             WaveOf({Sine(4000, 1000.0)}, 32000)};

    // 16 bytes of parameters and state, and half a byte for each sample. At half the rate, the samples are half as
    // many.
    XSFLIB_CHECK(edit::DsWavesSize(bank, waves) == 16 + 500);
    XSFLIB_CHECK(edit::DsWavesSize(bank, waves, 16000) == 16 + 250);

    // A region that plays the wave at another volume gets a wave of its level.
    sar::BankFile::Region quiet = RegionOf(0);
    quiet.values.volume = 64;
    bank.instruments.emplace_back(InstrumentOf({{0, 127, {quiet}}}));
    XSFLIB_CHECK(edit::DsWavesSize(bank, waves) == 2 * (16 + 500));
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("ds_conversion",
                                    []
                                    {
                                        TestCommands();
                                        TestTimebase();
                                        TestStart();
                                        TestVariables();
                                        TestNesting();
                                        TestInstruments();
                                        TestRegionValues();
                                        TestRatesAndLevels();
                                        TestStereoAndScale();
                                        TestRates();
                                        TestLoops();
                                        TestSize();
                                    });
}
