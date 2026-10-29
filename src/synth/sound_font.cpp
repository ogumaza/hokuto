// SPDX-License-Identifier: MIT

#include "synth/sound_font.h"

#include <algorithm>
#include <cstddef>
#include <stdexcept>
#include <string>

#include "seqlib/sf2/reader.h"

namespace hokuto::synth
{
namespace
{

// The sizes of the records of the pdta list (section 7).
constexpr std::size_t kPresetHeaderSize = 38;
constexpr std::size_t kBagSize = 4;
constexpr std::size_t kModulatorSize = 10;
constexpr std::size_t kGeneratorSize = 4;
constexpr std::size_t kInstrumentHeaderSize = 22;
constexpr std::size_t kSampleHeaderSize = 46;

// A key or velocity range that holds every value.
constexpr int32_t kFullRange = 0x7F00;

// The sample types of ROM samples (section 7.10). The file holds none of their points.
constexpr uint16_t kRomSample = 0x8000;

uint16_t ReadU16(const uint8_t* bytes)
{
    return static_cast<uint16_t>(bytes[0] | bytes[1] << 8);
}

uint32_t ReadU32(const uint8_t* bytes)
{
    return static_cast<uint32_t>(bytes[0]) | static_cast<uint32_t>(bytes[1]) << 8 |
           static_cast<uint32_t>(bytes[2]) << 16 | static_cast<uint32_t>(bytes[3]) << 24;
}

// Whether generator `oper` takes an unsigned amount: a range or an index.
bool IsUnsigned(int oper)
{
    return oper == kInstrumentGenerator || oper == kKeyRangeGenerator || oper == kVelRangeGenerator ||
           oper == kSampleIdGenerator;
}

// Whether a preset's zones may set generator `oper` (section 8.1.2). The address offsets, keynum, velocity,
// sampleModes, exclusiveClass and overridingRootKey belong to instruments only. The ranges and the instrument act on
// the zone itself.
bool AddsAtPresetLevel(int oper)
{
    switch (oper)
    {
    case 0:
    case 1:
    case 2:
    case 3:
    case 4:
    case 12:
    case 45:
    case 50:
    case 46:
    case 47:
    case 54:
    case 57:
    case 58:
    case kInstrumentGenerator:
    case kKeyRangeGenerator:
    case kVelRangeGenerator:
    case kSampleIdGenerator:
        return false;
    default:
        return oper < kGeneratorCount;
    }
}

// The zones of bags `first` to `last` of the list whose chunks are `bags`, `generators` and `modulators`. A zone's
// generator `terminal` names its target. The first zone without one is the global zone. Other zones without one are
// left out (section 7.3).
void ReadZones(const seqlib::sf2::Records& records, const char* bags, const char* generators, const char* modulators,
               std::size_t first, std::size_t last, int terminal, ZoneRecord& global, std::vector<ZoneRecord>& zones)
{
    for (std::size_t bag = first; bag < last; bag++)
    {
        const uint8_t* record = records.Record(bags, kBagSize, bag);
        const uint8_t* next = records.Record(bags, kBagSize, bag + 1);

        // The generators up to the terminal one. Those after it don't count.
        ZoneRecord zone;
        for (std::size_t g = ReadU16(record); g < ReadU16(next); g++)
        {
            const uint8_t* gen = records.Record(generators, kGeneratorSize, g);
            const int oper = ReadU16(gen);
            const uint16_t raw = ReadU16(gen + 2);
            if (oper == terminal)
            {
                zone.target = raw;
                break;
            }
            if (oper < kGeneratorCount)
            {
                zone.amounts[oper] = IsUnsigned(oper) ? raw : static_cast<int16_t>(raw);
                zone.set[oper] = true;
            }
        }

        for (std::size_t m = ReadU16(record + 2); m < ReadU16(next + 2); m++)
        {
            const uint8_t* mod = records.Record(modulators, kModulatorSize, m);
            zone.modulators.push_back({ReadU16(mod), ReadU16(mod + 2), static_cast<int16_t>(ReadU16(mod + 4)),
                                       ReadU16(mod + 6), ReadU16(mod + 8)});
        }

        if (zone.target >= 0)
        {
            zones.push_back(std::move(zone));
        }
        else if (bag == first)
        {
            global = std::move(zone);
        }
    }
}

// Whether `zone`, with its global zone `global`, holds key `key` and velocity `velocity`.
bool InRanges(const ZoneRecord& zone, const ZoneRecord& global, int key, int velocity)
{
    const auto holds = [&](int oper, int value)
    {
        const int32_t range = zone.set[oper]     ? zone.amounts[oper]
                              : global.set[oper] ? global.amounts[oper]
                                                 : kFullRange;
        return value >= (range & 0xFF) && value <= (range >> 8 & 0xFF);
    };
    return holds(kKeyRangeGenerator, key) && holds(kVelRangeGenerator, velocity);
}

// Puts each of `zone`'s modulators in `list`, in the place of an identical one (section 9.5.1).
void Supersede(std::vector<ModulatorRecord>& list, const std::vector<ModulatorRecord>& zone)
{
    for (const ModulatorRecord& modulator : zone)
    {
        const auto same = [&](const ModulatorRecord& other)
        {
            return other.IsIdentical(modulator);
        };
        const auto found = std::find_if(list.begin(), list.end(), same);
        if (found != list.end())
        {
            *found = modulator;
        }
        else
        {
            list.push_back(modulator);
        }
    }
}

SoundFontData ReadData(std::span<const uint8_t> file)
{
    const seqlib::sf2::Records records = seqlib::sf2::Read(file);
    SoundFontData font;

    // The points. A file without them can still hold ROM samples. The renderer leaves those out.
    const auto smpl = records.chunks.find("smpl");
    if (smpl != records.chunks.end())
    {
        const std::vector<uint8_t>& bytes = smpl->second;
        font.points.resize(bytes.size() / 2);
        for (std::size_t i = 0; i < font.points.size(); i++)
        {
            font.points[i] = static_cast<int16_t>(ReadU16(&bytes[2 * i]));
        }
    }

    // The samples, without the terminal record.
    const std::size_t samples = records.Count("shdr", kSampleHeaderSize);
    for (std::size_t s = 0; s + 1 < samples; s++)
    {
        const uint8_t* header = records.Record("shdr", kSampleHeaderSize, s);
        SampleRecord sample;
        sample.start = ReadU32(header + 20);
        sample.end = ReadU32(header + 24);
        sample.loop_start = ReadU32(header + 28);
        sample.loop_end = ReadU32(header + 32);
        sample.rate = ReadU32(header + 36);
        sample.original_key = header[40];
        sample.correction = static_cast<int8_t>(header[41]);
        sample.type = ReadU16(header + 44);
        font.samples.push_back(std::move(sample));
    }

    const std::size_t instruments = records.Count("inst", kInstrumentHeaderSize);
    for (std::size_t i = 0; i + 1 < instruments; i++)
    {
        const uint8_t* header = records.Record("inst", kInstrumentHeaderSize, i);
        const uint8_t* next = records.Record("inst", kInstrumentHeaderSize, i + 1);
        InstrumentRecord instrument;
        ReadZones(records, "ibag", "igen", "imod", ReadU16(header + 20), ReadU16(next + 20), kSampleIdGenerator,
                  instrument.global, instrument.zones);
        font.instruments.push_back(std::move(instrument));
    }

    const std::size_t presets = records.Count("phdr", kPresetHeaderSize);
    for (std::size_t p = 0; p + 1 < presets; p++)
    {
        const uint8_t* header = records.Record("phdr", kPresetHeaderSize, p);
        const uint8_t* next = records.Record("phdr", kPresetHeaderSize, p + 1);
        PresetRecord preset;
        preset.program = ReadU16(header + 20);
        preset.bank = ReadU16(header + 22);
        ReadZones(records, "pbag", "pgen", "pmod", ReadU16(header + 24), ReadU16(next + 24), kInstrumentGenerator,
                  preset.global, preset.zones);
        font.presets.push_back(std::move(preset));
    }

    return font;
}

} // namespace

bool ModulatorRecord::IsIdentical(const ModulatorRecord& other) const
{
    return source == other.source && destination == other.destination && amount_source == other.amount_source &&
           transform == other.transform;
}

SoundFontData ReadSoundFont(std::span<const uint8_t> file)
{
    try
    {
        return ReadData(file);
    }
    catch (const std::exception& e)
    {
        throw std::runtime_error(std::string("The SoundFont can't be read: ") + e.what());
    }
}

std::vector<VoiceSetup> VoicesFor(const SoundFontData& font, const PresetRecord& preset, int key, int velocity)
{
    std::vector<VoiceSetup> voices;
    for (const ZoneRecord& preset_zone : preset.zones)
    {
        if (!InRanges(preset_zone, preset.global, key, velocity) ||
            static_cast<std::size_t>(preset_zone.target) >= font.instruments.size())
        {
            continue;
        }

        const InstrumentRecord& instrument = font.instruments[static_cast<std::size_t>(preset_zone.target)];
        for (const ZoneRecord& zone : instrument.zones)
        {
            if (!InRanges(zone, instrument.global, key, velocity) ||
                static_cast<std::size_t>(zone.target) >= font.samples.size())
            {
                continue;
            }
            const SampleRecord& sample = font.samples[static_cast<std::size_t>(zone.target)];
            if ((sample.type & kRomSample) != 0)
            {
                continue;
            }

            // The instrument's local zone overrides its global zone. The global zone overrides the defaults. The
            // preset's local zone adds to the instrument's values. Where the local zone sets nothing, the preset's
            // global zone adds instead (section 9.4).
            VoiceSetup voice;
            voice.sample = &sample;
            voice.generators = DefaultGenerators();
            for (int oper = 0; oper < kGeneratorCount; oper++)
            {
                if (zone.set[oper])
                {
                    voice.generators[oper] = zone.amounts[oper];
                }
                else if (instrument.global.set[oper])
                {
                    voice.generators[oper] = instrument.global.amounts[oper];
                }
                if (AddsAtPresetLevel(oper) && preset_zone.set[oper])
                {
                    voice.generators[oper] += preset_zone.amounts[oper];
                }
                else if (AddsAtPresetLevel(oper) && preset.global.set[oper])
                {
                    voice.generators[oper] += preset.global.amounts[oper];
                }
            }

            // At each level, the local zone's modulators supersede identical ones of the global zone. The instrument's
            // modulators supersede identical defaults. A modulator of the preset adds its amount to an identical one.
            // Without one, the modulator joins the list (section 9.5.1).
            const std::span<const ModulatorRecord> defaults = DefaultModulators();
            voice.modulators.assign(defaults.begin(), defaults.end());
            Supersede(voice.modulators, instrument.global.modulators);
            Supersede(voice.modulators, zone.modulators);
            std::vector<ModulatorRecord> preset_modulators;
            Supersede(preset_modulators, preset.global.modulators);
            Supersede(preset_modulators, preset_zone.modulators);
            for (const ModulatorRecord& modulator : preset_modulators)
            {
                const auto same = [&](const ModulatorRecord& other)
                {
                    return other.IsIdentical(modulator);
                };
                const auto found = std::find_if(voice.modulators.begin(), voice.modulators.end(), same);
                if (found != voice.modulators.end())
                {
                    found->amount = static_cast<int16_t>(std::clamp(found->amount + modulator.amount, -32768, 32767));
                }
                else
                {
                    voice.modulators.push_back(modulator);
                }
            }

            voices.push_back(std::move(voice));
        }
    }

    return voices;
}

std::span<const ModulatorRecord> DefaultModulators()
{
    // Velocity to attenuation, velocity to cutoff, channel pressure and CC1 to the vibrato's depth, CC7 to attenuation,
    // CC10 to pan, CC11 to attenuation, CC91 and CC93 to the effects' sends, and the pitch wheel to fineTune.
    static constexpr ModulatorRecord kDefaults[] = {
        {0x0502, 48, 960, 0, 0}, {0x0102, 8, -2400, 0, 0},       {0x000D, 6, 50, 0, 0},   {0x0081, 6, 50, 0, 0},
        {0x0587, 48, 960, 0, 0}, {0x028A, 17, 1000, 0, 0},       {0x058B, 48, 960, 0, 0}, {0x00DB, 16, 200, 0, 0},
        {0x00DD, 15, 200, 0, 0}, {0x020E, 52, 12800, 0x0010, 0},
    };
    return kDefaults;
}

const std::array<double, kGeneratorCount>& DefaultGenerators()
{
    static const std::array<double, kGeneratorCount> kDefaults = []
    {
        std::array<double, kGeneratorCount> defaults{};
        defaults[8] = 13500.0;
        for (const int time : {21, 23, 25, 26, 27, 28, 30, 33, 34, 35, 36, 38})
        {
            defaults[static_cast<std::size_t>(time)] = -12000.0;
        }
        defaults[46] = -1.0;
        defaults[47] = -1.0;
        defaults[56] = 100.0;
        defaults[58] = -1.0;

        return defaults;
    }();
    return kDefaults;
}

} // namespace hokuto::synth
