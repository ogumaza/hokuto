// SPDX-License-Identifier: MIT

#include "edit/sound_font_voices.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace hokuto::edit
{
namespace
{

// The SoundFont players apply 0.4 of a zone's initialAttenuation (the global rulings on SoundFonts).
constexpr double kAttenuationScale = 0.4;

// The steps of a coarse offset of a sample's points.
constexpr int64_t kCoarseStep = 32768;

// The highest byOriginalPitch that names a key, and the key that a higher one plays as.
constexpr int kHighestRootKey = 127;
constexpr int kUnpitchedRootKey = 60;

// The generators that the imports leave out where a voice sets them, with what they do, for the warnings.
struct LeftOut
{
    int generator;
    const char* what;
};
constexpr LeftOut kLeftOut[] = {
    {kModLfoToPitch, "modulation LFO to pitch"},
    {kVibLfoToPitch, "vibrato LFO"},
    {kModEnvToPitch, "modulation envelope to pitch"},
    {kFilterCutoff, "low-pass filter"},
    {kModLfoToCutoff, "modulation LFO to filter cutoff"},
    {kModEnvToCutoff, "modulation envelope to filter cutoff"},
    {kModLfoToVolume, "modulation LFO to volume"},
    {kDelayVolEnv, "volume envelope delay"},
    {kKeyToHold, "key number to hold"},
    {kKeyToDecay, "key number to decay"},
    {kFixedKey, "fixed key"},
    {kFixedVelocity, "fixed velocity"},
};

// The low and high values of a range generator in `zone`, or nothing where the zone sets none.
std::optional<std::pair<int, int>> RangeOf(const synth::ZoneRecord& zone, int generator)
{
    if (!zone.set[static_cast<std::size_t>(generator)])
    {
        return std::nullopt;
    }

    const auto amount = static_cast<uint32_t>(zone.amounts[static_cast<std::size_t>(generator)]);

    return std::pair(static_cast<int>(amount & 0xFF), static_cast<int>((amount >> 8) & 0xFF));
}

} // namespace

synth::SoundFontData ReadSoundFontToImport(std::span<const uint8_t> sf2)
{
    try
    {
        return synth::ReadSoundFont(sf2);
    }
    catch (const std::exception& error)
    {
        throw std::invalid_argument(error.what());
    }
}

std::optional<SoundFontWave> SoundFontWaveOf(const synth::SoundFontData& font, const synth::VoiceSetup& voice,
                                             std::map<std::string, std::size_t>& left_out)
{
    const auto at = [&voice](int generator)
    {
        return GeneratorOf(voice, generator);
    };
    if (voice.sample == nullptr)
    {
        return std::nullopt;
    }

    for (const LeftOut& left : kLeftOut)
    {
        if (at(left.generator) != synth::DefaultGenerators()[static_cast<std::size_t>(left.generator)])
        {
            ++left_out[left.what];
        }
    }

    const synth::SampleRecord& sample = *voice.sample;
    SoundFontWave wave;
    wave.sample = static_cast<std::size_t>(voice.sample - font.samples.data());
    wave.start = int64_t{sample.start} + static_cast<int64_t>(at(kStartOffset)) +
                 kCoarseStep * static_cast<int64_t>(at(kStartCoarseOffset));
    wave.end = int64_t{sample.end} + static_cast<int64_t>(at(kEndOffset)) +
               kCoarseStep * static_cast<int64_t>(at(kEndCoarseOffset));
    const int modes = static_cast<int>(at(kSampleModes));
    wave.loop = modes == 1 || modes == 3;
    if (modes == 3)
    {
        ++left_out["sample mode 3 (play to the end after release)"];
    }

    if (wave.loop)
    {
        wave.loop_start = int64_t{sample.loop_start} + static_cast<int64_t>(at(kLoopStartOffset)) +
                          kCoarseStep * static_cast<int64_t>(at(kLoopStartCoarseOffset));
        wave.loop_end = int64_t{sample.loop_end} + static_cast<int64_t>(at(kLoopEndOffset)) +
                        kCoarseStep * static_cast<int64_t>(at(kLoopEndCoarseOffset));
        if (wave.loop_start < wave.start || wave.loop_end <= wave.loop_start || wave.loop_end > wave.end)
        {
            ++left_out["loops outside the sample"];
            wave.loop = false;
            wave.loop_start = 0;
            wave.loop_end = 0;
        }
        else
        {
            wave.end = wave.loop_end;
        }
    }

    const auto points = static_cast<int64_t>(font.points.size());
    wave.start = std::clamp<int64_t>(wave.start, 0, points);
    wave.end = std::clamp<int64_t>(wave.end, wave.start, points);
    if (wave.end <= wave.start || sample.rate == 0)
    {
        ++left_out["empty samples"];
        return std::nullopt;
    }

    return wave;
}

double SoundFontAttenuation(const synth::VoiceSetup& voice)
{
    double attenuation = kAttenuationScale * GeneratorOf(voice, kAttenuation);
    for (const synth::ModulatorRecord& modulator : voice.modulators)
    {
        if (modulator.source == 0 && modulator.amount_source == 0 && modulator.destination == kAttenuation)
        {
            attenuation += modulator.amount;
        }
    }

    return attenuation;
}

int SoundFontRootKey(const synth::VoiceSetup& voice)
{
    const double overriding = GeneratorOf(voice, kRootKey);
    if (overriding >= 0.0)
    {
        return static_cast<int>(overriding);
    }

    return voice.sample->original_key <= kHighestRootKey ? voice.sample->original_key : kUnpitchedRootKey;
}

std::optional<StereoVoices> StereoVoicesOf(const synth::SoundFontData& font, std::span<const synth::VoiceSetup> voices)
{
    if (voices.size() != 2 || voices[0].sample == nullptr || voices[1].sample == nullptr)
    {
        return std::nullopt;
    }

    // A sample's type in section 7.10 of the SoundFont 2.04 specification: 1 mono, 2 right and 4 left.
    constexpr uint16_t kMono = 1;
    constexpr uint16_t kRight = 2;
    constexpr uint16_t kLeft = 4;
    constexpr double kSide = 500.0;
    const uint16_t first = voices[0].sample->type;
    const uint16_t second = voices[1].sample->type;
    std::size_t left = 0;
    if ((first == kRight && second == kLeft) ||
        (first == kMono && second == kMono && GeneratorOf(voices[0], kPan) == kSide &&
         GeneratorOf(voices[1], kPan) == -kSide))
    {
        left = 1;
    }
    else if (!(first == kLeft && second == kRight) &&
             !(first == kMono && second == kMono && GeneratorOf(voices[0], kPan) == -kSide &&
               GeneratorOf(voices[1], kPan) == kSide))
    {
        return std::nullopt;
    }

    const synth::VoiceSetup& left_voice = voices[left];
    const synth::VoiceSetup& right_voice = voices[1 - left];

    if (left_voice.sample->rate != right_voice.sample->rate ||
        left_voice.sample->correction != right_voice.sample->correction ||
        left_voice.sample->original_key != right_voice.sample->original_key)
    {
        return std::nullopt;
    }

    for (const int generator : {kRootKey, kCoarseTune, kFineTune, kScaleTuning, kExclusiveClass})
    {
        if (GeneratorOf(left_voice, generator) != GeneratorOf(right_voice, generator))
        {
            return std::nullopt;
        }
    }

    if (SoundFontAttenuation(left_voice) != SoundFontAttenuation(right_voice))
    {
        return std::nullopt;
    }

    std::map<std::string, std::size_t> left_out;
    const std::optional<SoundFontWave> left_wave = SoundFontWaveOf(font, left_voice, left_out);
    const std::optional<SoundFontWave> right_wave = SoundFontWaveOf(font, right_voice, left_out);
    if (!left_wave || !right_wave || left_wave->end - left_wave->start != right_wave->end - right_wave->start ||
        left_wave->loop != right_wave->loop ||
        (left_wave->loop && (left_wave->loop_start - left_wave->start != right_wave->loop_start - right_wave->start ||
                             left_wave->loop_end - left_wave->start != right_wave->loop_end - right_wave->start)))
    {
        return std::nullopt;
    }

    return StereoVoices{&left_voice, &right_voice, *left_wave, *right_wave};
}

std::vector<int> SoundFontVelocityStarts(const synth::SoundFontData& font, const synth::PresetRecord& preset)
{
    std::vector<int> starts = {0};
    const auto add = [&starts](const synth::ZoneRecord& zone)
    {
        if (const auto range = RangeOf(zone, kVelocityRange))
        {
            starts.push_back(range->first);
            starts.push_back(range->second + 1);
        }
    };
    add(preset.global);
    for (const synth::ZoneRecord& zone : preset.zones)
    {
        add(zone);
        if (zone.target >= 0 && static_cast<std::size_t>(zone.target) < font.instruments.size())
        {
            const synth::InstrumentRecord& instrument = font.instruments[static_cast<std::size_t>(zone.target)];
            add(instrument.global);
            for (const synth::ZoneRecord& inner : instrument.zones)
            {
                add(inner);
            }
        }
    }

    std::erase_if(starts, [](int velocity) { return velocity > 127; });
    std::ranges::sort(starts);
    starts.erase(std::unique(starts.begin(), starts.end()), starts.end());

    return starts;
}

} // namespace hokuto::edit
