// SPDX-License-Identifier: MIT

// A SoundFont 2 file as Hokuto's renderer reads it: its samples, instruments and presets, and the generators and
// modulators that a note's voices take from them. Sections 7 to 9 of the SoundFont 2.04 specification give the rules.

#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace hokuto::synth
{

// The generator operators, the unused and reserved ones included (section 8.1.2).
inline constexpr int kGeneratorCount = 61;

// The operators that the renderer reads by name. seqlib's sf2::Gen has every one.
inline constexpr int kInstrumentGenerator = 41;
inline constexpr int kKeyRangeGenerator = 43;
inline constexpr int kVelRangeGenerator = 44;
inline constexpr int kSampleIdGenerator = 53;

// A modulator as a file holds it (section 8.2). Two modulators are identical when their sources, destination and
// transform match. Their amounts may differ (section 9.5.1).
struct ModulatorRecord
{
    bool IsIdentical(const ModulatorRecord& other) const;

    uint16_t source = 0;
    uint16_t destination = 0;
    int16_t amount = 0;
    uint16_t amount_source = 0;
    uint16_t transform = 0;
};

// A zone: the generators that it sets, its modulators, and its key and velocity ranges. A local zone of a preset names
// an instrument, and one of an instrument names a sample. A global zone names neither.
struct ZoneRecord
{
    std::array<int32_t, kGeneratorCount> amounts{};
    std::array<bool, kGeneratorCount> set{};
    std::vector<ModulatorRecord> modulators;
    int target = -1;
};

struct InstrumentRecord
{
    ZoneRecord global;
    std::vector<ZoneRecord> zones;
};

struct PresetRecord
{
    int program = 0;
    int bank = 0;
    ZoneRecord global;
    std::vector<ZoneRecord> zones;
};

// A sample's header (section 7.10). The points are indexes into SoundFontData::points.
struct SampleRecord
{
    uint32_t start = 0;
    uint32_t end = 0;
    uint32_t loop_start = 0;
    uint32_t loop_end = 0;
    uint32_t rate = 44100;
    int original_key = 60;
    int correction = 0; // cents
    uint16_t type = 1;
};

struct SoundFontData
{
    std::vector<int16_t> points; // the smpl chunk
    std::vector<SampleRecord> samples;
    std::vector<InstrumentRecord> instruments;
    std::vector<PresetRecord> presets;
};

// Reads the SoundFont `file`. Throws std::runtime_error with a message for the user when the file can't be read.
SoundFontData ReadSoundFont(std::span<const uint8_t> file);

// What a voice starts from (section 9.4): its sample, each generator's value from the instrument's zones plus the
// preset's offsets, and its modulators. initialAttenuation holds the zones' value without the players' scale.
struct VoiceSetup
{
    const SampleRecord* sample = nullptr;
    std::array<double, kGeneratorCount> generators{};
    std::vector<ModulatorRecord> modulators;
};

// The voices that preset `preset` of `font` gives key `key` at velocity `velocity`: one for each pair of a preset zone
// and an instrument zone whose ranges hold the key and the velocity. A zone takes its global zone's range where it sets
// none.
std::vector<VoiceSetup> VoicesFor(const SoundFontData& font, const PresetRecord& preset, int key, int velocity);

// The default modulators of section 8.4, in the form that the renderer applies them. The pitch wheel's modulator
// targets fineTune with 12800 cents. FluidSynth does the same. Section 8.4.10 gives Initial Pitch with 12700 cents.
// Initial Pitch isn't a generator.
std::span<const ModulatorRecord> DefaultModulators();

// The generators' default values (section 8.1.3).
const std::array<double, kGeneratorCount>& DefaultGenerators();

} // namespace hokuto::synth
