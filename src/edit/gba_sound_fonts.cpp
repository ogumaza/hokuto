// SPDX-License-Identifier: MIT

#include "edit/gba_sound_fonts.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "ntdlib/agb/gba.h"
#include "ntdlib/mp2k/song.h"
#include "ntdlib/sequence_export/mp2k/soundfont.h"
#include "xsflib/bytes.h"

#include "audio/resampler.h"
#include "edit/sound_font_voices.h"
#include "synth/sound_font.h"

namespace hokuto::edit
{
namespace
{

namespace mp2k = ntdlib::mp2k;

// The velocity whose voices a key uses, and the MIDI bank of the percussion.
constexpr int kVelocity = 100;
constexpr int kPercussionBank = 128;

// A SoundFont's shortest time, in seconds: -12000 timecents. The driver then has no attack, and no fade.
constexpr double kShortest = 0.001;

// The fall of a SoundFont's decay and release in their times, in decibels.
constexpr double kFadeDecibels = 100.0;

// How far a voice of a drum kit can be from the pitch that the SoundFont sets for it before a warning, in cents.
constexpr double kPitchTolerance = 1.0;

double SecondsOf(double timecents)
{
    return std::exp2(timecents / 1200.0);
}

// The attack that reaches the full level in `timecents`. The level starts at the attack, and gains it each frame.
uint8_t AttackOf(double timecents)
{
    const double seconds = SecondsOf(timecents);
    if (seconds <= kShortest)
    {
        return 255;
    }

    return static_cast<uint8_t>(std::clamp(std::lround(255.0 / (seconds * ntdlib::agb::kFrameRate + 1.0)), 1L, 255L));
}

// The frames in which the fade of the driver brings the level from `from` to `to` or below. The level becomes `fade`
// 256ths of itself each frame, rounded down.
int FadeFrames(int from, int to, int fade)
{
    int frames = 0;
    for (int level = from; level > to; frames++)
    {
        level = (level * fade) >> 8;
    }

    return frames;
}

// The decay or release that keeps the pace of a fall of 100 dB in `timecents`, on the driver's way from level `from` to
// level `to`. Rounding down makes the driver's fade faster than its value says, most of all at low levels. Among the
// fades that last the same frames, the one nearest the straight line in decibels is chosen. A fade of 0 ends at once.
uint8_t FadeOf(double timecents, int from, int to)
{
    const double seconds = SecondsOf(timecents);
    if (seconds <= kShortest)
    {
        return 0;
    }

    const double per_frame = kFadeDecibels / (seconds * ntdlib::agb::kFrameRate);
    const auto line = static_cast<int>(std::clamp(std::lround(256.0 * std::pow(10.0, -per_frame / 20.0)), 0L, 255L));
    const int bottom = std::max(to, 1);
    if (from <= bottom)
    {
        return static_cast<uint8_t>(line);
    }

    // The frames grow with the fade.
    const double wanted = 20.0 * std::log10(static_cast<double>(from) / bottom) / per_frame;
    int low = 0;
    int high = 255;
    while (low < high)
    {
        const int middle = (low + high) / 2;
        if (FadeFrames(from, bottom, middle) < wanted)
        {
            low = middle + 1;
        }
        else
        {
            high = middle;
        }
    }

    int fade = low;
    if (fade > 0 && wanted - FadeFrames(from, bottom, fade - 1) < FadeFrames(from, bottom, fade) - wanted)
    {
        fade--;
    }

    return static_cast<uint8_t>(FadeFrames(from, bottom, line) == FadeFrames(from, bottom, fade) ? line : fade);
}

// The release of Camelot's mixer whose fade ntdlib's export converts to the SoundFont release nearest `timecents`
// (ntdlib::sequence_export::mp2k::EnvelopeFor). The mixer subtracts 256 less the release from the level each frame, in
// a straight line from the sustain level of `voice`. Among the releases that last the same frames, the slowest is
// chosen. Its line ends nearest the last frame.
uint8_t StraightReleaseOf(double timecents, const mp2k::DriverInfo& driver, mp2k::Voice voice)
{
    int nearest = 0;
    double distance = std::numeric_limits<double>::infinity();
    for (int release = 0; release <= 255; release++)
    {
        voice.release = static_cast<uint8_t>(release);
        const double off = std::abs(ntdlib::sequence_export::mp2k::EnvelopeFor(driver, voice).release - timecents);
        if (off <= distance)
        {
            nearest = release;
            distance = off;
        }
    }

    return static_cast<uint8_t>(nearest);
}

// The sustain level of a SoundFont's sustain, in centibels below the full level.
uint8_t SustainOf(double centibels)
{
    return static_cast<uint8_t>(std::clamp(std::lround(255.0 * std::pow(10.0, -centibels / 200.0)), 0L, 255L));
}

// A drum kit voice's pan, from a SoundFont's pan of -500 to 500: bit 7 set, and half the driver's pan above 0xC0. The
// centre sets no bit.
uint8_t RhythmPanOf(double pan)
{
    const long half = std::clamp(std::lround(pan * 64.0 / 500.0), -64L, 63L);

    return half == 0 ? uint8_t{0} : static_cast<uint8_t>(0xC0 + half);
}

// How a sample plays: its points, its rate at key 60 in hertz, and the attenuation of its points in whole centibels.
struct SampleKey
{
    auto operator<=>(const SampleKey&) const = default;

    SoundFontWave wave;
    std::optional<SoundFontWave> right;
    uint32_t key_rate = 0;
    int attenuation = 0;
};

// The points of a sample, once they include the attenuation of the voice, with its rate at key 60 in hertz and the
// start of its loop.
struct TakenSample
{
    std::vector<int16_t> points;
    double key_rate = 0.0;
    std::optional<uint32_t> loop_start;
};

// What a key of a preset plays.
struct KeyVoice
{
    std::size_t sample = 0;
    bool whole_keys = true; // the voice's scale tuning is 100 cents a key
    mp2k::Voice voice;
};

// Builds the samples and the voices of a SoundFont's presets.
class Builder
{
public:
    Builder(const synth::SoundFontData& font, const mp2k::DriverInfo& driver, std::optional<uint32_t> rate)
        : font_(font), driver_(driver), rate_(rate)
    {
    }

    // The preset's voices, with no rows for a preset that plays no sample.
    GbaSoundFontPreset PresetOf(const synth::PresetRecord& preset)
    {
        GbaSoundFontPreset made;
        made.midi_bank = preset.bank;
        made.number = preset.program;
        made.name = preset.name;

        const std::vector<int> velocities = SoundFontVelocityStarts(font_, preset);
        std::vector<std::optional<KeyVoice>> keys(128);
        bool whole_keys = true;
        for (int key = 0; key < 128; ++key)
        {
            keys[static_cast<std::size_t>(key)] = KeyOf(preset, key, velocities);
            whole_keys =
                whole_keys && (!keys[static_cast<std::size_t>(key)] || keys[static_cast<std::size_t>(key)]->whole_keys);
        }

        made.drums = preset.bank == kPercussionBank || !whole_keys;

        // A drum kit gets a row for each key. Other presets join each key with the next where they play the same.
        for (int key = 0; key < 128; ++key)
        {
            const std::optional<KeyVoice>& at = keys[static_cast<std::size_t>(key)];
            if (!at)
            {
                continue;
            }

            if (!made.drums && !made.rows.empty() && made.rows.back().last_key + 1 == key &&
                SameVoice(made.rows.back(), *at))
            {
                made.rows.back().last_key = key;
                continue;
            }

            made.rows.push_back({key, key, at->sample, at->voice});
        }

        if (!made.drums)
        {
            for (GbaSoundFontRow& row : made.rows)
            {
                row.voice.pan_sweep = 0;
            }
        }

        return made;
    }

    // The samples' bytes. Every sample gets the one gain that restores the master volume of the driver, as far as the
    // loudest point allows.
    std::vector<std::vector<uint8_t>> TakeSamples() const
    {
        int peak = 0;
        for (const TakenSample& sample : taken_)
        {
            for (const int16_t point : sample.points)
            {
                peak = std::max(peak, std::abs(int{point}));
            }
        }

        const double level = driver_.camelot_mixer ? 1.0 : (driver_.master_volume + 1) / 16.0;
        const double gain = peak == 0 ? 1.0 : std::min(1.0 / level, 32768.0 / peak);

        std::vector<std::vector<uint8_t>> samples;
        samples.reserve(taken_.size());
        for (const TakenSample& sample : taken_)
        {
            std::vector<int16_t> points(sample.points);
            for (int16_t& point : points)
            {
                point = static_cast<int16_t>(std::clamp(std::lround(point * gain), -32768L, 32767L));
            }

            std::vector<uint8_t> encoded = EncodeGbaSample(
                points, static_cast<uint32_t>(std::max(std::llround(sample.key_rate), 1LL)), sample.loop_start);

            // The header stores the rate in 1024ths of a hertz. That can pass the 32 bits of a long on Windows.
            const auto fine = static_cast<uint32_t>(std::max(std::llround(sample.key_rate * 1024.0), 1LL));
            xsflib::StoreLe32(&encoded[4], fine);
            samples.push_back(std::move(encoded));
        }

        return samples;
    }

    uint32_t HighestRate() const
    {
        return highest_rate_;
    }

    std::size_t Layered() const
    {
        return layered_;
    }

    std::size_t VelocityLayers() const
    {
        return velocity_layers_;
    }

    const std::map<std::string, std::size_t>& LeftOutCounts() const
    {
        return left_out_;
    }

private:
    static bool SameVoice(const GbaSoundFontRow& row, const KeyVoice& at)
    {
        const auto values = [](const mp2k::Voice& voice)
        {
            return std::tuple(voice.attack, voice.decay, voice.sustain, voice.release);
        };

        return row.sample == at.sample && values(row.voice) == values(at.voice);
    }

    // The first voice at velocity 100, or else at the highest velocity that plays. Another voice at the key counts as a
    // layer, and another sample at another velocity counts too.
    std::optional<KeyVoice> KeyOf(const synth::PresetRecord& preset, int key, std::span<const int> velocities)
    {
        std::vector<synth::VoiceSetup> voices = synth::VoicesFor(font_, preset, key, kVelocity);
        for (auto velocity = velocities.rbegin(); voices.empty() && velocity != velocities.rend(); ++velocity)
        {
            voices = synth::VoicesFor(font_, preset, key, std::max(*velocity, 1));
        }
        if (voices.empty())
        {
            return std::nullopt;
        }

        const std::optional<StereoVoices> stereo = StereoVoicesOf(font_, voices);
        layered_ += voices.size() > 1 && !stereo;

        for (const int velocity : velocities)
        {
            const std::vector<synth::VoiceSetup> others = synth::VoicesFor(font_, preset, key, std::max(velocity, 1));
            if (!others.empty() && others.front().sample != voices.front().sample)
            {
                ++velocity_layers_;
                break;
            }
        }

        return stereo ? VoiceOf(*stereo->left, key, &stereo->right_wave) : VoiceOf(voices.front(), key);
    }

    // What `voice` plays at `key`. With `right`, the sample contains the mean of the channels of a stereo sample at the
    // centre.
    std::optional<KeyVoice> VoiceOf(const synth::VoiceSetup& voice, int key, const SoundFontWave* right = nullptr)
    {
        const auto at = [&voice](int generator)
        {
            return GeneratorOf(voice, generator);
        };
        const std::optional<SoundFontWave> wave = SoundFontWaveOf(font_, voice, left_out_);
        if (!wave)
        {
            return std::nullopt;
        }

        if (at(kExclusiveClass) != 0.0)
        {
            ++left_out_["exclusive class"];
        }
        // The driver keeps its voices at the full level for a frame before the decay (ntdlib's export writes that as a
        // hold).
        if (SecondsOf(at(kHoldVolEnv)) * ntdlib::agb::kFrameRate > 2.0)
        {
            ++left_out_["volume envelope hold"];
        }

        const synth::SampleRecord& sample = *voice.sample;
        const int root = SoundFontRootKey(voice);
        const double cents = at(kCoarseTune) * 100.0 + at(kFineTune) + sample.correction;
        const double scale = at(kScaleTuning);
        const double attenuation = std::max(SoundFontAttenuation(voice), 0.0);

        KeyVoice made;
        made.whole_keys = scale == 100.0;
        made.voice.key = static_cast<uint8_t>(std::clamp(key, 0, 127));
        const double played = sample.rate * std::exp2(cents / 1200.0);
        if (scale == 0.0 && !(rate_ && sample.rate > *rate_) &&
            std::abs(1200.0 * std::log2(played / driver_.mix_rate)) <= kPitchTolerance)
        {
            // The driver's voice of fixed pitch plays at the mixer's rate at every key, without interpolation.
            made.whole_keys = true;
            made.voice.type = mp2k::kVoiceFixed;
        }
        else if (!made.whole_keys)
        {
            const double pitch = root + (key - root) * scale / 100.0;
            const long nearest = std::clamp(std::lround(pitch), 0L, 127L);
            if (std::abs(pitch - static_cast<double>(nearest)) * 100.0 > kPitchTolerance)
            {
                ++left_out_["pitches between semitones from scale tuning"];
            }

            made.voice.key = static_cast<uint8_t>(nearest);
        }

        made.voice.pan_sweep = RhythmPanOf(right != nullptr ? 0.0 : at(kPan));
        made.voice.attack = AttackOf(at(kAttackVolEnv));
        made.voice.sustain = SustainOf(at(kSustainVolEnv));
        // ntdlib's converter measures the fades over these levels too: a decay down to the sustain, and a release down
        // to an eighth of the level that it falls from.
        const int sustain = made.voice.sustain;
        const int released = sustain != 0 ? sustain : 255;
        made.voice.decay = FadeOf(at(kDecayVolEnv), 255, sustain != 0 ? sustain : 2);

        // Camelot's mixer releases in a straight line.
        if (at(kPlaysPastRelease) != 0.0)
        {
            made.voice.release = 255;
        }
        else if (driver_.camelot_mixer)
        {
            made.voice.release = StraightReleaseOf(at(kReleaseVolEnv), driver_, made.voice);
        }
        else
        {
            made.voice.release = FadeOf(at(kReleaseVolEnv), released, std::max(1, released / 8));
        }

        made.sample =
            SampleIndex(*wave, right, sample.rate, std::exp2((60.0 - root + cents / 100.0) / 12.0), attenuation);

        return made;
    }

    // The index of the sample that plays `wave` at `rate`, with key 60 at `ratio` times that rate. A new sample is
    // added after the others.
    std::size_t SampleIndex(const SoundFontWave& wave, const SoundFontWave* right, uint32_t rate, double ratio,
                            double attenuation)
    {
        const SampleKey key{wave, right != nullptr ? std::optional<SoundFontWave>(*right) : std::nullopt,
                            static_cast<uint32_t>(std::max(std::lround(rate * ratio), 1L)),
                            static_cast<int>(std::lround(attenuation))};
        const auto [found, added] = indexes_.emplace(key, taken_.size());
        if (!added)
        {
            return found->second;
        }

        // The points, at the chosen rate. A loop keeps its pitch from its new length: the intro and the loop each
        // stretch by less than a sample to take whole samples.
        highest_rate_ = std::max(highest_rate_, rate);
        std::vector<int16_t> points(font_.points.begin() + wave.start, font_.points.begin() + wave.end);
        if (right != nullptr)
        {
            for (std::size_t i = 0; i < points.size(); ++i)
            {
                const int other = font_.points[static_cast<std::size_t>(right->start) + i];
                points[i] = static_cast<int16_t>((points[i] + other) / 2);
            }
        }

        std::vector<int16_t> taken(points);
        std::optional<uint32_t> loop_start;
        if (wave.loop)
        {
            loop_start = static_cast<uint32_t>(wave.loop_start - wave.start);
        }

        double key_rate = rate * ratio;
        if (rate_ && rate > *rate_)
        {
            const std::size_t old_size = taken.size();
            const std::optional<uint32_t> old_start = loop_start;
            taken = audio::Resample(points, rate, *rate_,
                                    old_start ? std::optional<std::size_t>(*old_start) : std::nullopt);
            if (old_start)
            {
                loop_start = static_cast<uint32_t>(audio::ResampledPlace(*old_start, rate, *rate_));
                key_rate *=
                    static_cast<double>(taken.size() - *loop_start) / static_cast<double>(old_size - *old_start);
            }
            else
            {
                key_rate *= static_cast<double>(*rate_) / rate;
            }
        }

        // The driver has no level for a voice. The sample's points take the voice's attenuation.
        if (key.attenuation > 0)
        {
            const double gain = std::pow(10.0, -attenuation / 200.0);
            for (int16_t& point : taken)
            {
                point = static_cast<int16_t>(std::lround(point * gain));
            }
        }

        taken_.push_back({std::move(taken), key_rate, loop_start});

        return found->second;
    }

    const synth::SoundFontData& font_;
    const mp2k::DriverInfo& driver_;
    std::optional<uint32_t> rate_;
    std::map<SampleKey, std::size_t> indexes_;
    std::vector<TakenSample> taken_;
    uint32_t highest_rate_ = 0;
    std::size_t layered_ = 0;
    std::size_t velocity_layers_ = 0;
    std::map<std::string, std::size_t> left_out_;
};

// The bytes of the tables of a preset's key split or drum kit (edit::WithGbaKeyRows).
std::size_t TableBytes(const GbaSoundFontPreset& preset)
{
    if (preset.drums)
    {
        return 128 * kGbaVoiceBytes;
    }
    if (preset.rows.size() == 1 && preset.rows.front().first_key == 0 && preset.rows.front().last_key == 127)
    {
        return 0;
    }

    return (preset.rows.size() + 1) * kGbaVoiceBytes + 128;
}

// A multiple of 4 at `size` or after it. The driver reads a sample's header as words.
std::size_t WordsUp(std::size_t size)
{
    return (size + 3) / 4 * 4;
}

std::string KeysText(std::size_t count)
{
    return std::to_string(count) + (count == 1 ? " key" : " keys");
}

} // namespace

GbaSoundFontVoices GbaVoicesOfSoundFont(std::span<const uint8_t> sf2, const mp2k::DriverInfo& driver,
                                        std::optional<uint32_t> rate)
{
    const synth::SoundFontData font = ReadSoundFontToImport(sf2);

    std::vector<const synth::PresetRecord*> order;
    for (const synth::PresetRecord& preset : font.presets)
    {
        order.push_back(&preset);
    }

    std::ranges::stable_sort(order, {}, [](const synth::PresetRecord* preset)
                             { return std::pair(int{preset->bank}, int{preset->program}); });

    GbaSoundFontVoices voices;
    Builder builder(font, driver, rate);
    for (const synth::PresetRecord* preset : order)
    {
        GbaSoundFontPreset made = builder.PresetOf(*preset);
        if (!made.rows.empty())
        {
            voices.bytes += TableBytes(made);
            voices.presets.push_back(std::move(made));
        }
    }
    if (voices.presets.empty())
    {
        throw std::invalid_argument("The SoundFont has no presets with samples.");
    }

    voices.samples = builder.TakeSamples();
    voices.highest_rate = builder.HighestRate();
    for (const std::vector<uint8_t>& sample : voices.samples)
    {
        voices.bytes += WordsUp(sample.size());
    }

    voices.warnings.push_back("Samples are converted to 8-bit PCM.");
    if (builder.Layered() > 0)
    {
        voices.warnings.push_back("The SoundFont layers several voices at " + KeysText(builder.Layered()) +
                                  ". Only the first voice is used.");
    }

    if (builder.VelocityLayers() > 0)
    {
        voices.warnings.push_back("The SoundFont uses different samples for different velocities at " +
                                  KeysText(builder.VelocityLayers()) + ". Only the sample for velocity 100 is used.");
    }

    for (const auto& [what, count] : builder.LeftOutCounts())
    {
        voices.warnings.push_back("Ignored at " + KeysText(count) + ": " + what + ".");
    }

    return voices;
}

std::vector<std::optional<uint32_t>> GbaSoundFontPrograms(const GbaSoundFontVoices& voices,
                                                          std::span<const uint32_t> free,
                                                          std::span<const uint32_t> unplayed)
{
    std::vector<std::optional<uint32_t>> programs(voices.presets.size());
    std::vector<uint32_t> left(free.begin(), free.end());
    std::ranges::sort(left);
    std::vector<uint32_t> others(unplayed.begin(), unplayed.end());
    std::ranges::sort(others);

    // First the presets whose numbers the lists hold. An earlier preset then doesn't take the number of a later preset.
    for (std::size_t i = 0; i < programs.size(); i++)
    {
        const auto number = static_cast<uint32_t>(voices.presets[i].number);
        for (std::vector<uint32_t>* list : {&left, &others})
        {
            const auto own = std::ranges::find(*list, number);
            if (own != list->end())
            {
                programs[i] = number;
                list->erase(own);
                break;
            }
        }
    }

    left.insert(left.end(), others.begin(), others.end());
    for (std::size_t i = 0; i < programs.size() && !left.empty(); i++)
    {
        if (!programs[i])
        {
            programs[i] = left.front();
            left.erase(left.begin());
        }
    }

    return programs;
}

GbaGroupVersion WithGbaSoundFont(const format::File& file, const song::GbaRom& rom, uint32_t group,
                                 const GbaSoundFontVoices& voices)
{
    const auto [free, unplayed] = GbaOpenProgramsOf(rom, group);
    if (free.empty() && unplayed.empty())
    {
        throw std::invalid_argument("The songs use every program in this voice group. None is free for the import.");
    }

    const std::vector<std::optional<uint32_t>> programs = GbaSoundFontPrograms(voices, free, unplayed);

    // The samples that the presets with programs play, together in the free space on multiples of 4 bytes.
    std::vector<std::optional<std::size_t>> offsets(voices.samples.size());
    std::vector<uint8_t> bytes;
    for (std::size_t i = 0; i < voices.presets.size(); i++)
    {
        if (!programs[i])
        {
            continue;
        }

        for (const GbaSoundFontRow& row : voices.presets[i].rows)
        {
            std::optional<std::size_t>& offset = offsets.at(row.sample);
            if (!offset)
            {
                offset = bytes.size();
                const std::vector<uint8_t>& sample = voices.samples[row.sample];
                bytes.insert(bytes.end(), sample.begin(), sample.end());
                bytes.resize(WordsUp(bytes.size()));
            }
        }
    }

    const PlacedGbaBytes placed = WithGbaNewBytes(file, bytes);
    const auto voice_of = [&placed, &offsets](const GbaSoundFontRow& row)
    {
        mp2k::Voice voice = NewGbaSampleVoice(placed.address + static_cast<uint32_t>(*offsets.at(row.sample)));
        voice.type |= row.voice.type;
        voice.key = row.voice.key;
        voice.pan_sweep = row.voice.pan_sweep;
        voice.attack = row.voice.attack;
        voice.decay = row.voice.decay;
        voice.sustain = row.voice.sustain;
        voice.release = row.voice.release;

        return voice;
    };

    // The program of each preset contains a voice of its first sample, and the key splits and drum kits then get their
    // rows.
    std::vector<std::pair<uint32_t, mp2k::Voice>> first;
    for (std::size_t i = 0; i < voices.presets.size(); i++)
    {
        if (programs[i])
        {
            first.emplace_back(*programs[i], voice_of(voices.presets[i].rows.front()));
        }
    }

    GbaGroupVersion version = WithGbaVoicesAt(placed.file, rom, group, first);
    for (std::size_t i = 0; i < voices.presets.size(); i++)
    {
        const GbaSoundFontPreset& preset = voices.presets[i];
        if (!programs[i] || TableBytes(preset) == 0)
        {
            continue;
        }

        std::vector<GbaKeyRow> rows;
        for (const GbaSoundFontRow& row : preset.rows)
        {
            rows.push_back({row.first_key, row.last_key, voice_of(row)});
        }

        mp2k::Voice split;
        split.address = version.group + mp2k::kVoiceSize * *programs[i];
        version.file = WithGbaKeyRows(
            version.file, GbaSplitOf(split, preset.drums ? mp2k::kVoiceDrumKit : mp2k::kVoiceKeySplit), rows);
    }

    return version;
}

} // namespace hokuto::edit
