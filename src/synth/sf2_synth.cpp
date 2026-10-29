// SPDX-License-Identifier: MIT

#include "synth/sf2_synth.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <numbers>
#include <stdexcept>
#include <string>
#include <vector>

#include "seqlib/midi/reader.h"

#include "synth/sound_font.h"

namespace hokuto::synth
{
namespace
{

// The samples for which a voice holds its LFOs, its modulation envelope, its pitch and its filter.
constexpr int kBlock = 64;

// The share of initialAttenuation that the players apply.
constexpr double kAttenuationScale = 0.4;

// The largest attenuation, in centibels (section 8.1.3).
constexpr double kMostAttenuation = 1440.0;

// A decay or a release falls by this many centibels in its time: 100 dB (section 8.1.2, generators 36 and 38). A voice
// ends when its volume envelope has fallen that far, to the gain kSilent.
constexpr double kFullFall = 1000.0;
constexpr double kSilent = 1e-5;

// The release of a voice that a note of its exclusive class ends, in timecents: 100 dB in about 16 ms.
constexpr double kExclusiveRelease = -7200.0;

// The range of the filter's cutoff in absolute cents, and its highest frequency as a share of the rate.
constexpr double kLowestCutoff = 1500.0;
constexpr double kHighestCutoff = 13500.0;
constexpr double kHighestCutoffShare = 0.45;

// The frequency of 0 absolute cents (section 8.1.3).
constexpr double kCentsBaseHz = 8.176;

// How long the render goes on after the file's last event while voices still sound, in seconds.
constexpr double kLongestTail = 10.0;

// MIDI channel 10, the General MIDI channel of the drums, and the bank that it plays.
constexpr int kDrumChannel = 9;
constexpr int kDrumBank = 128;

// The controllers that the renderer reads by number.
constexpr int kBankSelect = 0;
constexpr int kVolume = 7;
constexpr int kPan = 10;
constexpr int kExpression = 11;
constexpr int kBankSelectLsb = 32;
constexpr int kVolumeLsb = 39;
constexpr int kPanLsb = 42;
constexpr int kSustain = 64;

// A data entry of the null RPN changes nothing.
constexpr int kNullRpn = 0x3FFF;

// The generators that the voices read by number (section 8.1.2).
enum Generator : int
{
    kStartAddrsOffset = 0,
    kEndAddrsOffset = 1,
    kStartloopAddrsOffset = 2,
    kEndloopAddrsOffset = 3,
    kStartAddrsCoarseOffset = 4,
    kModLfoToPitch = 5,
    kVibLfoToPitch = 6,
    kModEnvToPitch = 7,
    kInitialFilterFc = 8,
    kInitialFilterQ = 9,
    kModLfoToFilterFc = 10,
    kModEnvToFilterFc = 11,
    kEndAddrsCoarseOffset = 12,
    kModLfoToVolume = 13,
    kPanGenerator = 17,
    kDelayModLfo = 21,
    kFreqModLfo = 22,
    kDelayVibLfo = 23,
    kFreqVibLfo = 24,
    kDelayModEnv = 25,
    kAttackModEnv = 26,
    kHoldModEnv = 27,
    kDecayModEnv = 28,
    kSustainModEnv = 29,
    kReleaseModEnv = 30,
    kKeynumToModEnvHold = 31,
    kKeynumToModEnvDecay = 32,
    kDelayVolEnv = 33,
    kAttackVolEnv = 34,
    kHoldVolEnv = 35,
    kDecayVolEnv = 36,
    kSustainVolEnv = 37,
    kReleaseVolEnv = 38,
    kKeynumToVolEnvHold = 39,
    kKeynumToVolEnvDecay = 40,
    kStartloopAddrsCoarseOffset = 45,
    kKeynum = 46,
    kVelocity = 47,
    kInitialAttenuation = 48,
    kEndloopAddrsCoarseOffset = 50,
    kCoarseTune = 51,
    kFineTune = 52,
    kSampleModes = 54,
    kScaleTuning = 56,
    kExclusiveClass = 57,
    kOverridingRootKey = 58,
};

double TimecentsToSeconds(double timecents)
{
    return std::exp2(timecents / 1200.0);
}

// The gain of an attenuation of `centibels`.
double Gain(double centibels)
{
    return std::pow(10.0, -centibels / 200.0);
}

// The samples in `seconds` at `rate`, rounded to the nearest.
int64_t Samples(double seconds, int rate)
{
    return std::max<int64_t>(0, std::llround(seconds * rate));
}

// A MIDI channel's state.
struct Channel
{
    // The tuning that RPNs 1 and 2 add, in cents.
    double Tuning() const
    {
        return (coarse_tuning - 64) * 100.0 + (fine_tuning - 8192) * 100.0 / 8192.0;
    }

    // Sets the state that a channel starts with. General MIDI gives the values. The sound controllers, 70 to 79, start
    // at their centre.
    void Start()
    {
        controllers.fill(0);
        controllers[kVolume] = 100;
        controllers[kPan] = 64;
        for (std::size_t sound = 70; sound <= 79; sound++)
        {
            controllers[sound] = 64;
        }
        ResetControllers();
    }

    // Resets what reset all controllers resets (General MIDI's Recommended Practice RP-015). The bank select, the
    // volume, the pan, the sound controllers, the effects' depths and the RPNs' values stay.
    void ResetControllers()
    {
        for (int controller = 0; controller < 128; controller++)
        {
            const bool kept = controller == kBankSelect || controller == kBankSelectLsb || controller == kVolume ||
                              controller == kVolumeLsb || controller == kPan || controller == kPanLsb ||
                              (controller >= 70 && controller <= 79) || (controller >= 91 && controller <= 95);
            if (!kept)
            {
                controllers[static_cast<std::size_t>(controller)] = 0;
            }
        }
        controllers[kExpression] = 127;
        key_pressure.fill(0);
        pitch_wheel = 8192;
        channel_pressure = 0;
        rpn = kNullRpn;
    }

    std::array<int, 128> controllers{};
    std::array<int, 128> key_pressure{};
    int bank = 0;
    int pitch_wheel = 8192;
    int channel_pressure = 0;
    int rpn = kNullRpn;
    double bend_range = 2.0; // semitones (RPN 0)
    int fine_tuning = 8192;  // RPN 1: 8192 at 0, a semitone either way at the ends
    int coarse_tuning = 64;  // RPN 2: semitones above 64
    const PresetRecord* preset = nullptr;
};

// The curves of the modulators' sources (section 8.2.1). `index` runs from 0 to 127. A fraction counts as the whole
// value below it. At a whole value v, the concave curve is -20/96 log10(((127 - v) / 127)^2). That equals
// -5/12 log10((127 - v) / 127). The convex curve is 1 - concave(127 - v). The specification's figures show both curves.
// Each curve runs from 0 at 0 to 1 at 127.
double Concave(double index)
{
    if (index < 0.0)
    {
        return 0.0;
    }
    if (index >= 127.0)
    {
        return 1.0;
    }

    const int point = static_cast<int>(index);
    return point == 0 ? 0.0 : -(5.0 / 12.0) * std::log10((127.0 - point) / 127.0);
}

double Convex(double index)
{
    if (index < 0.0)
    {
        return 0.0;
    }
    if (index >= 127.0)
    {
        return 1.0;
    }

    const int point = static_cast<int>(index);
    return point == 0 ? 0.0 : 1.0 + (5.0 / 12.0) * std::log10(point / 127.0);
}

// The value of modulator source `source` (section 8.2.1) for a voice of key `key` and velocity `velocity` on `channel`.
// A 7-bit value v counts as x = v / 128. The pitch wheel's value counts as v / 16384. A negative source turns x into
// 1 - x. The concave and convex curves of a unipolar source then take 127 x.
double SourceValue(uint16_t source, const Channel& channel, int key, int velocity)
{
    const int index = source & 0x7F;
    double value = 0.0;
    double range = 128.0;
    if ((source & 0x80) != 0)
    {
        // The controllers that section 8.2.1 doesn't allow as sources give nothing.
        if (index == 0 || index == 6 || index == 32 || index == 38 || (index >= 98 && index <= 101) || index >= 120)
        {
            return 0.0;
        }
        value = channel.controllers[static_cast<std::size_t>(index)];
    }
    else
    {
        switch (index)
        {
        case 0:
            return 1.0;
        case 2:
            value = velocity;
            break;
        case 3:
            value = key;
            break;
        case 10:
            value = channel.key_pressure[static_cast<std::size_t>(std::clamp(key, 0, 127))];
            break;
        case 13:
            value = channel.channel_pressure;
            break;
        case 14:
            value = channel.pitch_wheel;
            range = 16384.0;
            break;
        case 16:
            value = channel.bend_range;
            break;
        default:
            return 0.0;
        }
    }

    // The direction, the polarity and the curve.
    const double x = value / range;
    const bool negative = (source & 0x100) != 0;
    const bool bipolar = (source & 0x200) != 0;
    switch (source >> 10 & 3)
    {
    case 0:
        if (bipolar)
        {
            return negative ? 1.0 - 2.0 * x : 2.0 * x - 1.0;
        }
        return negative ? 1.0 - x : x;

    case 1:
        if (bipolar)
        {
            const double side = x > 0.5 ? Concave(127.0 * 2.0 * (x - 0.5)) : -Concave(127.0 * 2.0 * (0.5 - x));
            return negative ? -side : side;
        }
        return Concave(127.0 * (negative ? 1.0 - x : x));

    case 2:
        if (bipolar)
        {
            const double side = x > 0.5 ? Convex(127.0 * 2.0 * (x - 0.5)) : -Convex(127.0 * 2.0 * (0.5 - x));
            return negative ? -side : side;
        }
        return Convex(127.0 * (negative ? 1.0 - x : x));

    default:
        {
            const bool high = x >= 0.5;
            if (bipolar)
            {
                return high != negative ? 1.0 : -1.0;
            }
            return high != negative ? 1.0 : 0.0;
        }
    }
}

// A triangle LFO (section 8.1.2, generators 21 to 24): silent for its delay, then from 0 up to 1, down to -1 and back.
class Lfo
{
public:
    void Start(double delay_timecents, double frequency_cents, int rate)
    {
        delay_ = Samples(TimecentsToSeconds(delay_timecents), rate);
        step_ = kCentsBaseHz * std::exp2(std::clamp(frequency_cents, -16000.0, 4500.0) / 1200.0) / rate;
        phase_ = 0.0;
    }

    // Returns the value now. The LFO then moves on by `samples`.
    double Next(int samples)
    {
        double value = 0.0;
        if (delay_ <= 0)
        {
            value = phase_ < 0.25 ? 4.0 * phase_ : phase_ < 0.75 ? 2.0 - 4.0 * phase_ : 4.0 * phase_ - 4.0;
            phase_ += step_ * samples;
            phase_ -= std::floor(phase_);
        }
        else
        {
            delay_ -= samples;
        }

        return value;
    }

private:
    int64_t delay_ = 0;
    double step_ = 0.0;
    double phase_ = 0.0;
};

// The modulation envelope (section 8.1.2, generators 25 to 32), from 0 to 1. Its attack, decay and release are linear.
class ModulationEnvelope
{
public:
    void Start(const std::array<double, kGeneratorCount>& g, int key, int rate)
    {
        delay_ = Samples(TimecentsToSeconds(std::clamp(g[kDelayModEnv], -12000.0, 5000.0)), rate);
        attack_ = Samples(TimecentsToSeconds(std::clamp(g[kAttackModEnv], -12000.0, 8000.0)), rate);
        hold_ = Samples(
            TimecentsToSeconds(std::clamp(g[kHoldModEnv] + g[kKeynumToModEnvHold] * (60 - key), -12000.0, 5000.0)),
            rate);
        decay_step_ =
            1.0 /
            std::max<int64_t>(1, Samples(TimecentsToSeconds(std::clamp(
                                             g[kDecayModEnv] + g[kKeynumToModEnvDecay] * (60 - key), -12000.0, 8000.0)),
                                         rate));
        sustain_ = 1.0 - std::clamp(g[kSustainModEnv], 0.0, 1000.0) / 1000.0;
        release_step_ =
            1.0 /
            std::max<int64_t>(1, Samples(TimecentsToSeconds(std::clamp(g[kReleaseModEnv], -12000.0, 8000.0)), rate));
    }

    void Release()
    {
        released_ = true;
    }

    // Returns the value now. The envelope then moves on by `samples`.
    double Next(int samples)
    {
        const double value = value_;
        for (int i = 0; i < samples; i++)
        {
            Step();
        }

        return value;
    }

private:
    void Step()
    {
        if (released_)
        {
            value_ = std::max(0.0, value_ - release_step_);
        }
        else if (delay_ > 0)
        {
            delay_--;
        }
        else if (attack_ > 0)
        {
            value_ = std::min(1.0, value_ + (1.0 - value_) / static_cast<double>(attack_));
            attack_--;
        }
        else if (hold_ > 0)
        {
            value_ = 1.0;
            hold_--;
        }
        else
        {
            value_ = std::max(sustain_, value_ - decay_step_);
        }
    }

    int64_t delay_ = 0;
    int64_t attack_ = 0;
    int64_t hold_ = 0;
    double decay_step_ = 1.0;
    double sustain_ = 1.0;
    double release_step_ = 1.0;
    double value_ = 0.0;
    bool released_ = false;
};

// One voice: a sample that plays from a zone of a preset for a note (section 9.1).
class Voice
{
public:
    Voice(const SoundFontData& font, const VoiceSetup& setup, const PresetRecord* preset, int channel, int key,
          int velocity, uint64_t note, int rate)
        : points_(&font.points),
          generators_(setup.generators),
          modulators_(setup.modulators),
          preset_(preset),
          channel_(channel),
          key_(key),
          velocity_(velocity),
          note_(note),
          rate_(rate)
    {
        const SampleRecord& sample = *setup.sample;

        const auto offset = [&](int fine, int coarse)
        {
            return static_cast<int64_t>(generators_[fine]) + 32768 * static_cast<int64_t>(generators_[coarse]);
        };

        const int64_t size = static_cast<int64_t>(points_->size());
        start_ = std::clamp<int64_t>(sample.start + offset(kStartAddrsOffset, kStartAddrsCoarseOffset), 0, size);
        end_ = std::clamp<int64_t>(sample.end + offset(kEndAddrsOffset, kEndAddrsCoarseOffset), start_, size);
        loop_start_ = std::clamp<int64_t>(
            sample.loop_start + offset(kStartloopAddrsOffset, kStartloopAddrsCoarseOffset), start_, end_);
        loop_end_ = std::clamp<int64_t>(sample.loop_end + offset(kEndloopAddrsOffset, kEndloopAddrsCoarseOffset),
                                        loop_start_, end_);
        const int mode = static_cast<int>(generators_[kSampleModes]) & 3;
        loop_mode_ = loop_end_ - loop_start_ >= 2 ? mode : 0;
        looping_ = loop_mode_ == 1 || loop_mode_ == 3;
        position_ = static_cast<double>(start_);
        exclusive_class_ = static_cast<int>(generators_[kExclusiveClass]);

        // The key and the velocity that the voice's generators and modulators read.
        pitch_key_ = generators_[kKeynum] >= 0.0 ? static_cast<int>(generators_[kKeynum]) : key;
        if (generators_[kVelocity] >= 0.0)
        {
            velocity_ = static_cast<int>(generators_[kVelocity]);
        }
        const double root = generators_[kOverridingRootKey] >= 0.0 ? generators_[kOverridingRootKey]
                            : sample.original_key <= 127           ? sample.original_key
                                                                   : 60.0;
        key_cents_ = pitch_key_ - root;
        correction_ = sample.correction;
        rate_ratio_ = static_cast<double>(sample.rate) / rate;

        // The volume envelope (section 8.1.2, generators 33 to 40).
        delay_ = Samples(TimecentsToSeconds(std::clamp(generators_[kDelayVolEnv], -12000.0, 5000.0)), rate);
        attack_ = Samples(TimecentsToSeconds(std::clamp(generators_[kAttackVolEnv], -12000.0, 8000.0)), rate);
        hold_ = Samples(
            TimecentsToSeconds(std::clamp(
                generators_[kHoldVolEnv] + generators_[kKeynumToVolEnvHold] * (60 - pitch_key_), -12000.0, 5000.0)),
            rate);
        decay_factor_ = FallFactor(generators_[kDecayVolEnv] + generators_[kKeynumToVolEnvDecay] * (60 - pitch_key_));
        sustain_ = Gain(std::clamp(generators_[kSustainVolEnv], 0.0, kFullFall));
        release_factor_ = FallFactor(generators_[kReleaseVolEnv]);
        stage_ = delay_ > 0 ? Stage::kDelay : Stage::kAttack;

        modulation_envelope_.Start(generators_, pitch_key_, rate);
        modulation_lfo_.Start(generators_[kDelayModLfo], generators_[kFreqModLfo], rate);
        vibrato_lfo_.Start(generators_[kDelayVibLfo], generators_[kFreqVibLfo], rate);
    }

    // Works out the values that the modulators move, from the channel's controllers (section 9.5).
    void Modulate(const Channel& channel)
    {
        std::array<double, kGeneratorCount> added{};
        for (const ModulatorRecord& modulator : modulators_)
        {
            if (modulator.destination >= kGeneratorCount)
            {
                continue;
            }

            const double primary = SourceValue(modulator.source, channel, pitch_key_, velocity_);
            const double secondary = modulator.amount_source == 0
                                         ? 1.0
                                         : SourceValue(modulator.amount_source, channel, pitch_key_, velocity_);
            double value = modulator.amount * primary * secondary;
            if (modulator.transform == 2)
            {
                value = std::abs(value);
            }
            added[modulator.destination] += value;
        }

        const auto total = [&](int oper)
        {
            return generators_[oper] + added[oper];
        };

        attenuation_ = std::clamp(kAttenuationScale * generators_[kInitialAttenuation] + added[kInitialAttenuation],
                                  0.0, kMostAttenuation);
        // The sin/cos law, with each side as a sine. A side then falls to exactly 0 at the other end.
        const double pan = std::clamp(total(kPanGenerator), -500.0, 500.0) / 500.0;
        left_ = static_cast<float>(std::sin((1.0 - pan) * std::numbers::pi / 4.0));
        right_ = static_cast<float>(std::sin((1.0 + pan) * std::numbers::pi / 4.0));
        pitch_cents_ = key_cents_ * total(kScaleTuning) + 100.0 * total(kCoarseTune) + total(kFineTune) + correction_ +
                       channel.Tuning();
        cutoff_cents_ = total(kInitialFilterFc);
        resonance_ = std::clamp(total(kInitialFilterQ), 0.0, 960.0);
        mod_lfo_to_pitch_ = total(kModLfoToPitch);
        vib_lfo_to_pitch_ = total(kVibLfoToPitch);
        mod_env_to_pitch_ = total(kModEnvToPitch);
        mod_lfo_to_cutoff_ = total(kModLfoToFilterFc);
        mod_env_to_cutoff_ = total(kModEnvToFilterFc);
        mod_lfo_to_volume_ = total(kModLfoToVolume);
    }

    // Starts the release. A sample that loops until release then plays on to its end.
    void Release()
    {
        if (stage_ == Stage::kDone)
        {
            return;
        }

        stage_ = Stage::kRelease;
        modulation_envelope_.Release();
        if (loop_mode_ == 3)
        {
            looping_ = false;
        }
    }

    // Ends the voice quickly, for a note of its exclusive class.
    void ReleaseExclusive()
    {
        release_factor_ = FallFactor(kExclusiveRelease);
        Release();
    }

    // Mixes the next `frames` samples into `out`, left and right in turn.
    void Render(float* out, int frames)
    {
        for (int i = 0; i < frames && stage_ != Stage::kDone; i++)
        {
            if (block_left_ == 0)
            {
                StartBlock();
            }
            block_left_--;

            // The sample starts to play when the volume envelope's delay ends. FluidSynth's voices start the same way.
            if (stage_ == Stage::kDelay)
            {
                Envelope();
                gain_ += gain_step_;
                continue;
            }

            const float sample = Interpolated();
            const float filtered = b0_ * sample + b1_ * x1_ + b2_ * x2_ - a1_ * y1_ - a2_ * y2_;
            x2_ = x1_;
            x1_ = sample;
            y2_ = y1_;
            y1_ = filtered;
            const float level = static_cast<float>(Envelope()) * gain_;
            gain_ += gain_step_;
            out[2 * i] += filtered * level * left_;
            out[2 * i + 1] += filtered * level * right_;

            position_ += step_;
            if (looping_ && position_ >= static_cast<double>(loop_end_))
            {
                const auto start = static_cast<double>(loop_start_);
                position_ = start + std::fmod(position_ - start, static_cast<double>(loop_end_ - loop_start_));
            }
            else if (!looping_ && position_ >= static_cast<double>(end_))
            {
                stage_ = Stage::kDone;
            }
        }
    }

    bool Finished() const
    {
        return stage_ == Stage::kDone;
    }

    bool Released() const
    {
        return stage_ == Stage::kRelease;
    }

    const PresetRecord* Preset() const
    {
        return preset_;
    }

    int ChannelNumber() const
    {
        return channel_;
    }

    int Key() const
    {
        return key_;
    }

    uint64_t Note() const
    {
        return note_;
    }

    int ExclusiveClass() const
    {
        return exclusive_class_;
    }

    // Whether a note-off came while the sustain pedal held the note down.
    bool Sustained() const
    {
        return sustained_;
    }

    void SetSustained(bool sustained)
    {
        sustained_ = sustained;
    }

private:
    enum class Stage
    {
        kDelay,
        kAttack,
        kHold,
        kDecay,
        kSustain,
        kRelease,
        kDone,
    };

    // The factor by which the volume envelope falls each sample. The envelope falls 100 dB in `timecents`.
    double FallFactor(double timecents) const
    {
        const double samples =
            std::max(1.0, TimecentsToSeconds(std::clamp(timecents, -12000.0, 8000.0)) * static_cast<double>(rate_));
        return Gain(kFullFall / samples);
    }

    // Holds the LFOs, the modulation envelope, the pitch, the filter and the gain for the next block.
    void StartBlock()
    {
        block_left_ = kBlock;
        const double mod_env = modulation_envelope_.Next(kBlock);
        const double mod_lfo = modulation_lfo_.Next(kBlock);
        const double vib_lfo = vibrato_lfo_.Next(kBlock);

        const double cents =
            pitch_cents_ + mod_lfo * mod_lfo_to_pitch_ + vib_lfo * vib_lfo_to_pitch_ + mod_env * mod_env_to_pitch_;
        step_ = rate_ratio_ * std::exp2(cents / 1200.0);

        const double cutoff = std::clamp(cutoff_cents_ + mod_lfo * mod_lfo_to_cutoff_ + mod_env * mod_env_to_cutoff_,
                                         kLowestCutoff, kHighestCutoff);
        if (cutoff != filter_cutoff_ || resonance_ != filter_resonance_)
        {
            SetFilter(cutoff, resonance_);
        }

        // The gain of the attenuation, the modulation LFO's swing of the volume and the filter. The gain ramps over the
        // block.
        const double attenuation = attenuation_ - mod_lfo * mod_lfo_to_volume_;
        const float target = static_cast<float>(Gain(attenuation) * filter_gain_);
        if (first_block_)
        {
            gain_ = target;
            first_block_ = false;
        }
        gain_step_ = (target - gain_) / kBlock;
    }

    // The cookbook low-pass at `cutoff` absolute cents. A resonance of `resonance` centibels peaks at resonance / 10 -
    // 3.01 dB above the passband, at the cutoff. The passband falls by half the resonance (section 8.1.2, generator 9).
    void SetFilter(double cutoff, double resonance)
    {
        filter_cutoff_ = cutoff;
        filter_resonance_ = resonance;
        const double hz = std::min(kCentsBaseHz * std::exp2(cutoff / 1200.0), kHighestCutoffShare * rate_);
        const double q = std::pow(10.0, (resonance / 10.0 - 3.01) / 20.0);
        filter_gain_ = Gain(resonance / 2.0);
        const double w = 2.0 * std::numbers::pi * hz / rate_;
        const double alpha = std::sin(w) / (2.0 * q);
        const double a0 = 1.0 + alpha;
        b1_ = static_cast<float>((1.0 - std::cos(w)) / a0);
        b0_ = b1_ / 2.0f;
        b2_ = b0_;
        a1_ = static_cast<float>(-2.0 * std::cos(w) / a0);
        a2_ = static_cast<float>((1.0 - alpha) / a0);
    }

    // Returns the volume envelope's gain for this sample. The envelope then moves on a sample.
    double Envelope()
    {
        const double value = envelope_;
        switch (stage_)
        {
        case Stage::kDelay:
            if (--delay_ <= 0)
            {
                stage_ = Stage::kAttack;
            }
            break;

        case Stage::kAttack:
            if (attack_ <= 0)
            {
                envelope_ = 1.0;
                stage_ = Stage::kHold;
                return 1.0;
            }
            envelope_ = std::min(1.0, envelope_ + (1.0 - envelope_) / static_cast<double>(attack_));
            attack_--;
            if (attack_ <= 0)
            {
                stage_ = Stage::kHold;
            }
            break;

        case Stage::kHold:
            if (--hold_ <= 0)
            {
                stage_ = Stage::kDecay;
            }
            break;

        case Stage::kDecay:
            envelope_ *= decay_factor_;
            if (envelope_ <= sustain_)
            {
                envelope_ = sustain_;
                stage_ = Stage::kSustain;
            }
            break;

        case Stage::kSustain:
            if (sustain_ <= kSilent)
            {
                stage_ = Stage::kDone;
            }
            break;

        case Stage::kRelease:
            envelope_ *= release_factor_;
            if (envelope_ <= kSilent)
            {
                stage_ = Stage::kDone;
            }
            break;

        case Stage::kDone:
            break;
        }

        return value;
    }

    // The sample's point `index`. While the voice loops, an index past the loop's end counts round the loop.
    float Point(int64_t index) const
    {
        if (looping_ && index >= loop_end_)
        {
            index = loop_start_ + (index - loop_start_) % (loop_end_ - loop_start_);
        }
        if (index < 0 || index >= static_cast<int64_t>(points_->size()) || (!looping_ && index >= end_))
        {
            return 0.0f;
        }

        return (*points_)[static_cast<std::size_t>(index)] / 32768.0f;
    }

    // The sample at the voice's position: a Catmull-Rom spline through the four points around it.
    float Interpolated() const
    {
        const auto index = static_cast<int64_t>(position_);
        const auto t = static_cast<float>(position_ - static_cast<double>(index));
        float p0 = 0.0f;
        float p1 = 0.0f;
        float p2 = 0.0f;
        float p3 = 0.0f;
        const int64_t limit = looping_ ? loop_end_ : end_;
        if (index >= 1 && index + 2 < limit)
        {
            const int16_t* points = &(*points_)[static_cast<std::size_t>(index - 1)];
            p0 = points[0] / 32768.0f;
            p1 = points[1] / 32768.0f;
            p2 = points[2] / 32768.0f;
            p3 = points[3] / 32768.0f;
        }
        else
        {
            p0 = Point(index - 1);
            p1 = Point(index);
            p2 = Point(index + 1);
            p3 = Point(index + 2);
        }

        return p1 +
               0.5f * t * (p2 - p0 + t * (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3 + t * (3.0f * (p1 - p2) + p3 - p0)));
    }

    const std::vector<int16_t>* points_;
    std::array<double, kGeneratorCount> generators_;
    std::vector<ModulatorRecord> modulators_;
    const PresetRecord* preset_ = nullptr;
    int channel_ = 0;
    int key_ = 0;
    int velocity_ = 0;
    uint64_t note_ = 0;
    int rate_ = 48000;
    int pitch_key_ = 0;
    int exclusive_class_ = 0;
    bool sustained_ = false;

    // The sample.
    int64_t start_ = 0;
    int64_t end_ = 0;
    int64_t loop_start_ = 0;
    int64_t loop_end_ = 0;
    int loop_mode_ = 0;
    bool looping_ = false;
    double position_ = 0.0;
    double step_ = 0.0;
    double rate_ratio_ = 1.0;
    double key_cents_ = 0.0;
    double correction_ = 0.0;

    // What the modulators move.
    double attenuation_ = 0.0;
    double pitch_cents_ = 0.0;
    double cutoff_cents_ = kHighestCutoff;
    double resonance_ = 0.0;
    double mod_lfo_to_pitch_ = 0.0;
    double vib_lfo_to_pitch_ = 0.0;
    double mod_env_to_pitch_ = 0.0;
    double mod_lfo_to_cutoff_ = 0.0;
    double mod_env_to_cutoff_ = 0.0;
    double mod_lfo_to_volume_ = 0.0;
    float left_ = 0.0f;
    float right_ = 0.0f;

    // The volume envelope.
    Stage stage_ = Stage::kDelay;
    int64_t delay_ = 0;
    int64_t attack_ = 0;
    int64_t hold_ = 0;
    double decay_factor_ = 1.0;
    double sustain_ = 1.0;
    double release_factor_ = 1.0;
    double envelope_ = 0.0;

    ModulationEnvelope modulation_envelope_;
    Lfo modulation_lfo_;
    Lfo vibrato_lfo_;

    // The block, the filter and the gain.
    int block_left_ = 0;
    bool first_block_ = true;
    double filter_cutoff_ = -1.0;
    double filter_resonance_ = -1.0;
    double filter_gain_ = 1.0;
    float b0_ = 1.0f;
    float b1_ = 0.0f;
    float b2_ = 0.0f;
    float a1_ = 0.0f;
    float a2_ = 0.0f;
    float x1_ = 0.0f;
    float x2_ = 0.0f;
    float y1_ = 0.0f;
    float y2_ = 0.0f;
    float gain_ = 0.0f;
    float gain_step_ = 0.0f;
};

// The 16 channels and their voices.
class Synth
{
public:
    Synth(const SoundFontData& font, int rate) : font_(font), rate_(rate)
    {
        for (std::size_t p = 0; p < font_.presets.size(); p++)
        {
            presets_.emplace(PresetKey(font_.presets[p].bank, font_.presets[p].program), &font_.presets[p]);
        }
        for (int ch = 0; ch < 16; ch++)
        {
            Channel& channel = channels_[static_cast<std::size_t>(ch)];
            channel.Start();
            channel.preset = FindPreset(ch, 0, 0);
        }
    }

    // Plays the channel message `bytes`.
    void Message(const std::vector<uint8_t>& bytes)
    {
        if (bytes.empty() || bytes[0] < 0x80 || bytes[0] >= 0xF0)
        {
            return;
        }

        const int ch = bytes[0] & 0x0F;
        const int data1 = bytes.size() > 1 ? bytes[1] & 0x7F : 0;
        const int data2 = bytes.size() > 2 ? bytes[2] & 0x7F : 0;
        Channel& channel = channels_[static_cast<std::size_t>(ch)];
        switch (bytes[0] & 0xF0)
        {
        case 0x80:
            NoteOff(ch, data1);
            break;

        case 0x90:
            if (data2 == 0)
            {
                NoteOff(ch, data1);
            }
            else
            {
                NoteOn(ch, data1, data2);
            }
            break;

        case 0xA0:
            channel.key_pressure[static_cast<std::size_t>(data1)] = data2;
            ModulateChannel(ch);
            break;

        case 0xB0:
            Control(ch, data1, data2);
            break;

        case 0xC0:
            channel.preset = FindPreset(ch, channel.bank, data1);
            break;

        case 0xD0:
            channel.channel_pressure = data1;
            ModulateChannel(ch);
            break;

        default:
            channel.pitch_wheel = data1 | data2 << 7;
            ModulateChannel(ch);
            break;
        }
    }

    // Mixes the next `frames` samples of every voice into `out` and removes the finished voices.
    void Render(float* out, int frames)
    {
        for (Voice& voice : voices_)
        {
            voice.Render(out, frames);
        }
        std::erase_if(voices_, [](const Voice& voice) { return voice.Finished(); });
    }

    bool Silent() const
    {
        return voices_.empty();
    }

private:
    static int PresetKey(int bank, int program)
    {
        return bank << 8 | program;
    }

    // The preset that channel `ch` plays for `bank` and `program`. A missing preset gives way to program `program` of
    // bank 0, or else to program 0 of bank 0. FluidSynth falls back the same way. Channel 10 plays program `program` of
    // bank 128, or else program 0 of bank 128.
    const PresetRecord* FindPreset(int ch, int bank, int program) const
    {
        const auto find = [&](int b, int p) -> const PresetRecord*
        {
            const auto found = presets_.find(PresetKey(b, p));
            return found == presets_.end() ? nullptr : found->second;
        };

        if (ch == kDrumChannel)
        {
            const PresetRecord* drums = find(kDrumBank, program);
            return drums != nullptr ? drums : find(kDrumBank, 0);
        }

        const PresetRecord* preset = find(bank, program);
        if (preset == nullptr)
        {
            preset = find(0, program);
        }
        if (preset == nullptr)
        {
            preset = find(0, 0);
        }

        return preset;
    }

    void NoteOn(int ch, int key, int velocity)
    {
        // A note-on of a key that sounds on the channel acts as the key's note-off first. FluidSynth does the same.
        NoteOff(ch, key);

        const Channel& channel = channels_[static_cast<std::size_t>(ch)];
        if (channel.preset == nullptr)
        {
            return;
        }

        const uint64_t note = next_note_++;
        const std::vector<VoiceSetup> setups = VoicesFor(font_, *channel.preset, key, velocity);
        for (const VoiceSetup& setup : setups)
        {
            Voice voice(font_, setup, channel.preset, ch, key, velocity, note, rate_);
            voice.Modulate(channel);

            // A note of an exclusive class ends the class's other notes of the preset on the channel (section 8.1.2,
            // generator 57).
            if (voice.ExclusiveClass() != 0)
            {
                for (Voice& other : voices_)
                {
                    if (other.ChannelNumber() == ch && other.Preset() == channel.preset &&
                        other.ExclusiveClass() == voice.ExclusiveClass() && other.Note() != note)
                    {
                        other.ReleaseExclusive();
                    }
                }
            }
            voices_.push_back(std::move(voice));
        }
    }

    void NoteOff(int ch, int key)
    {
        const bool sustain = channels_[static_cast<std::size_t>(ch)].controllers[kSustain] >= 64;
        for (Voice& voice : voices_)
        {
            if (voice.ChannelNumber() == ch && voice.Key() == key && !voice.Released() && !voice.Sustained())
            {
                if (sustain)
                {
                    voice.SetSustained(true);
                }
                else
                {
                    voice.Release();
                }
            }
        }
    }

    void Control(int ch, int controller, int value)
    {
        Channel& channel = channels_[static_cast<std::size_t>(ch)];
        switch (controller)
        {
        case 0:
            channel.bank = value;
            channel.controllers[kBankSelect] = value;
            return;

        case 6:
        case 38:
            DataEntry(channel, controller == 6, value);
            ModulateChannel(ch);
            return;

        case 98:
        case 99:
            channel.rpn = kNullRpn;
            return;

        case 100:
            channel.rpn = (channel.rpn & 0x3F80) | value;
            return;

        case 101:
            channel.rpn = (channel.rpn & 0x7F) | value << 7;
            return;

        case 120:
            std::erase_if(voices_, [ch](const Voice& voice) { return voice.ChannelNumber() == ch; });
            return;

        case 121:
            channel.ResetControllers();
            ReleaseSustained(ch);
            ModulateChannel(ch);
            return;

        case 123:
            for (int key = 0; key < 128; key++)
            {
                NoteOff(ch, key);
            }
            return;

        default:
            break;
        }

        channel.controllers[static_cast<std::size_t>(controller)] = value;
        if (controller == kSustain && value < 64)
        {
            ReleaseSustained(ch);
        }
        ModulateChannel(ch);
    }

    // Data entry for the channel's RPN: the bend range (RPN 0), the fine tuning (RPN 1) and the coarse tuning (RPN 2).
    static void DataEntry(Channel& channel, bool msb, int value)
    {
        switch (channel.rpn)
        {
        case 0:
            {
                const double cents = channel.bend_range - std::floor(channel.bend_range);
                channel.bend_range = msb ? value + cents : std::floor(channel.bend_range) + value / 100.0;
                break;
            }

        case 1:
            channel.fine_tuning =
                msb ? (value << 7 | (channel.fine_tuning & 0x7F)) : ((channel.fine_tuning & 0x3F80) | value);
            break;

        case 2:
            if (msb)
            {
                channel.coarse_tuning = value;
            }
            break;

        default:
            break;
        }
    }

    void ReleaseSustained(int ch)
    {
        for (Voice& voice : voices_)
        {
            if (voice.ChannelNumber() == ch && voice.Sustained())
            {
                voice.SetSustained(false);
                voice.Release();
            }
        }
    }

    void ModulateChannel(int ch)
    {
        const Channel& channel = channels_[static_cast<std::size_t>(ch)];
        for (Voice& voice : voices_)
        {
            if (voice.ChannelNumber() == ch)
            {
                voice.Modulate(channel);
            }
        }
    }

    const SoundFontData& font_;
    int rate_;
    std::map<int, const PresetRecord*> presets_;
    std::array<Channel, 16> channels_;
    std::vector<Voice> voices_;
    uint64_t next_note_ = 0;
};

// A channel message at its sample.
struct TimedMessage
{
    int64_t sample = 0;
    std::vector<uint8_t> bytes;
};

// The channel messages of every track of `file`, at their samples at `rate`, and the sample of the file's last event.
// Events at the same tick keep the order of their tracks. The tempo changes of every track count.
std::vector<TimedMessage> TimedMessages(const seqlib::midi::File& file, int rate, int64_t& last)
{
    struct Placed
    {
        uint64_t tick;
        std::size_t track;
        std::size_t index;
    };
    std::vector<Placed> order;
    for (std::size_t t = 0; t < file.tracks.size(); t++)
    {
        for (std::size_t i = 0; i < file.tracks[t].events.size(); i++)
        {
            order.push_back({file.tracks[t].events[i].tick, t, i});
        }
    }

    const auto earlier = [](const Placed& a, const Placed& b)
    {
        return a.tick < b.tick || (a.tick == b.tick && a.track < b.track);
    };
    std::stable_sort(order.begin(), order.end(), earlier);

    // A division with its top bit set counts frames of SMPTE time. Its ticks don't follow the tempo.
    const bool smpte = (file.division & 0x8000) != 0;
    const int frames = -static_cast<int8_t>(file.division >> 8);
    const double smpte_tick = smpte ? 1.0 / std::max(1, frames * (file.division & 0xFF)) : 0.0;
    const double division = std::max<uint16_t>(1, file.division);
    double seconds = 0.0;
    uint64_t tick = 0;
    double tempo = 500000.0; // microseconds a quarter note
    std::vector<TimedMessage> messages;
    last = 0;
    for (const Placed& placed : order)
    {
        const seqlib::midi::Event& event = file.tracks[placed.track].events[placed.index];
        const double ticks = static_cast<double>(placed.tick - tick);
        seconds += smpte ? ticks * smpte_tick : ticks * tempo / 1e6 / division;
        tick = placed.tick;
        const int64_t sample = std::llround(seconds * rate);
        last = std::max(last, sample);

        const std::vector<uint8_t>& bytes = event.bytes;
        if (bytes.size() >= 6 && bytes[0] == 0xFF && bytes[1] == 0x51 && bytes[2] == 3)
        {
            tempo = bytes[3] << 16 | bytes[4] << 8 | bytes[5];
        }
        else if (!bytes.empty() && bytes[0] >= 0x80 && bytes[0] < 0xF0)
        {
            messages.push_back({sample, bytes});
        }
    }

    return messages;
}

} // namespace

StereoRender RenderSoundFont(std::span<const uint8_t> midi, std::span<const uint8_t> sf2, int rate, double most_seconds,
                             const std::function<bool(double)>& progress)
{
    if (rate <= 0)
    {
        throw std::invalid_argument("The rate of a render has to be positive.");
    }
    if (!(most_seconds > 0.0))
    {
        throw std::invalid_argument("The length of a render has to be positive.");
    }

    seqlib::midi::File file;
    try
    {
        file = seqlib::midi::Read(midi);
    }
    catch (const std::exception& e)
    {
        throw std::runtime_error(std::string("The MIDI file can't be read: ") + e.what());
    }
    const SoundFontData font = ReadSoundFont(sf2);
    int64_t last = 0;
    const std::vector<TimedMessage> messages = TimedMessages(file, rate, last);

    // The render runs from message to message. After the last message, the render goes on while voices sound, for
    // kLongestTail at most. The render stops at `most_seconds` in any case.
    Synth synth(font, rate);
    StereoRender render;
    render.rate = rate;
    const double most_samples = most_seconds * rate;
    const int64_t most = most_samples < 0x1p62 ? std::max<int64_t>(1, std::llround(most_samples)) : int64_t{1} << 62;
    const int64_t limit = std::min(last + Samples(kLongestTail, rate), most);
    const double expected = static_cast<double>(std::max<int64_t>(1, std::min(last + rate, most)));
    int64_t done = 0;
    int64_t reported = 0;
    bool stopped = false;
    const auto run_to = [&](int64_t target)
    {
        while (done < target && !stopped)
        {
            const int frames = static_cast<int>(std::min<int64_t>(target - done, 4096));
            render.samples.resize(static_cast<std::size_t>(2 * (done + frames)), 0.0f);
            synth.Render(&render.samples[static_cast<std::size_t>(2 * done)], frames);
            done += frames;
            if (progress && done - reported >= rate / 4)
            {
                reported = done;
                stopped = !progress(std::min(1.0, static_cast<double>(done) / expected));
            }
        }
    };

    for (const TimedMessage& message : messages)
    {
        if (message.sample >= most)
        {
            break;
        }

        run_to(message.sample);
        if (stopped)
        {
            return render;
        }
        synth.Message(message.bytes);
    }

    run_to(std::min(last, most));
    while (!synth.Silent() && done < limit && !stopped)
    {
        run_to(std::min(limit, done + 4096));
    }

    return render;
}

} // namespace hokuto::synth
