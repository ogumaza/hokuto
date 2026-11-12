// SPDX-License-Identifier: MIT

// What a voice of a SoundFont 2 file plays, for the imports that make a bank of it (edit/sound_font_banks.h) or the
// voices of a GBA voice group (edit/gba_sound_fonts.h): the generators by their numbers, the points of the samples that
// the voice plays, its level, and the velocities at which a preset can change.

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "synth/sound_font.h"

namespace hokuto::edit
{

// The generators that the imports read, by their numbers in section 8.1.2 of the SoundFont 2.04 specification.
enum SoundFontGenerator : int
{
    kStartOffset = 0,
    kEndOffset = 1,
    kLoopStartOffset = 2,
    kLoopEndOffset = 3,
    kStartCoarseOffset = 4,
    kModLfoToPitch = 5,
    kVibLfoToPitch = 6,
    kModEnvToPitch = 7,
    kFilterCutoff = 8,
    kModLfoToCutoff = 10,
    kModEnvToCutoff = 11,
    kEndCoarseOffset = 12,
    kModLfoToVolume = 13,
    kPan = 17,
    kDelayVolEnv = 33,
    kAttackVolEnv = 34,
    kHoldVolEnv = 35,
    kDecayVolEnv = 36,
    kSustainVolEnv = 37,
    kReleaseVolEnv = 38,
    kKeyToHold = 39,
    kKeyToDecay = 40,
    kKeyRange = 43,
    kVelocityRange = 44,
    kLoopStartCoarseOffset = 45,
    kFixedKey = 46,
    kFixedVelocity = 47,
    kAttenuation = 48,
    kLoopEndCoarseOffset = 50,
    kCoarseTune = 51,
    kFineTune = 52,
    kSampleModes = 54,
    kScaleTuning = 56,
    kExclusiveClass = 57,
    kRootKey = 58,

    // The specification leaves generator 59 unused, and players ignore it. SoundFontOfSfz marks a zone of
    // `loop_mode=one_shot` with it, and its note plays on past its release.
    kPlaysPastRelease = 59,
};

// How a voice plays its sample: the points from its start to its end, or to the end of its loop with the loop. The
// points count from the start of the SoundFont's samples.
struct SoundFontWave
{
    auto operator<=>(const SoundFontWave&) const = default;

    std::size_t sample = 0;
    int64_t start = 0;
    int64_t end = 0;
    bool loop = false;
    int64_t loop_start = 0;
    int64_t loop_end = 0;
};

// The SoundFont 2 file `sf2`, read for an import. Throws std::invalid_argument with the message of the read for a file
// that synth::ReadSoundFont can't read.
synth::SoundFontData ReadSoundFontToImport(std::span<const uint8_t> sf2);

// The value of generator `generator` of `voice`.
inline double GeneratorOf(const synth::VoiceSetup& voice, int generator)
{
    return voice.generators[static_cast<std::size_t>(generator)];
}

// What `voice` plays, or nothing for a voice without a sample that an import can play. `left_out` counts what the
// imports leave out wherever the voice sets it, in words for the user: the filters, the LFOs, the modulation envelope,
// the volume envelope's delay, and a loop or a sample that doesn't hold.
std::optional<SoundFontWave> SoundFontWaveOf(const synth::SoundFontData& font, const synth::VoiceSetup& voice,
                                             std::map<std::string, std::size_t>& left_out);

// The two voices of a stereo sample, with the waves that they play.
struct StereoVoices
{
    const synth::VoiceSetup* left = nullptr;
    const synth::VoiceSetup* right = nullptr;
    SoundFontWave left_wave;
    SoundFontWave right_wave;
};

// The voices of `voices` as a stereo sample, or nothing for voices that aren't one. A SoundFont plays a stereo sample
// as two voices: a left sample and a right sample, or two mono samples panned to the two sides. The two have to play
// the same way: at one rate, with one root key and tuning, at one level, over as many points and with one loop.
std::optional<StereoVoices> StereoVoicesOf(const synth::SoundFontData& font, std::span<const synth::VoiceSetup> voices);

// The attenuation of `voice` in centibels: 0.4 of the zones' initialAttenuation, and the constant modulators' amounts.
// The SoundFont players apply 0.4 of initialAttenuation too (the global rulings on SoundFonts).
double SoundFontAttenuation(const synth::VoiceSetup& voice);

// The root key of `voice`, a voice with a sample: the zones' overridingRootKey, or else the sample's byOriginalPitch.
// Section 7.10 of the SoundFont 2.04 specification treats a byOriginalPitch from 128 to 255 as key 60. 255 marks an
// unpitched sample. Hokuto's renderer follows that.
int SoundFontRootKey(const synth::VoiceSetup& voice);

// The velocities at which the regions of `preset` can change: 0, and the velocity after the end of each velocity range
// and at the start of each range, of the zones of the preset and the zones of their instruments.
std::vector<int> SoundFontVelocityStarts(const synth::SoundFontData& font, const synth::PresetRecord& preset);

} // namespace hokuto::edit
