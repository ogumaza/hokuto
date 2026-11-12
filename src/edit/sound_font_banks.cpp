// SPDX-License-Identifier: MIT

#include "edit/sound_font_banks.h"

#include <algorithm>
#include <array>
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

#include "ntdlib/sar/bank_file.h"
#include "ntdlib/sar/wave_file.h"
#include "ntdlib/sequence_export/consoles.h"
#include "ntdlib/sequence_export/envelope.h"
#include "xsflib/bytes.h"

#include "edit/banks.h"
#include "edit/sound_font_voices.h"
#include "song/items.h"
#include "synth/sound_font.h"

namespace hokuto::edit
{
namespace
{

namespace sar = ntdlib::sar;
namespace export_ = ntdlib::sequence_export;

// The key groups of NintendoWare's regions: 1 to 15. 0 is none.
constexpr int kMostKeyGroups = 15;

// The values of NintendoWare's envelope: 0 to 127.
constexpr int kEnvelopeValues = 128;

// The SoundFont envelope of each value of NintendoWare's envelope on a console, for the searches of nearest values. The
// hold time includes part of the attack time (ConvertEnvelope). The holds for each attack value are converted when a
// region first uses that attack.
class Envelopes
{
public:
    explicit Envelopes(const export_::Settings& settings) : settings_(settings)
    {
        for (int value = 0; value < kEnvelopeValues; ++value)
        {
            export_::EnvelopeValues v;
            v.attack = value;
            v.decay = value;
            v.sustain = value;
            v.release = value;
            v.hold = 0;
            const export_::Sf2Envelope e = export_::ConvertEnvelope(v, settings);
            const auto at = static_cast<std::size_t>(value);
            attacks_[at] = e.attack;
            decays_[at] = e.decay;
            sustains_[at] = e.sustain;
            releases_[at] = e.release;
        }
    }

    // The SoundFont's hold of each hold value after `attack`.
    const std::array<int, kEnvelopeValues>& Holds(uint8_t attack)
    {
        const auto [found, added] = holds_.try_emplace(attack);
        if (added)
        {
            for (int hold = 0; hold < kEnvelopeValues; ++hold)
            {
                export_::EnvelopeValues v;
                v.attack = attack;
                v.hold = hold;
                found->second[static_cast<std::size_t>(hold)] = export_::ConvertEnvelope(v, settings_).hold;
            }
        }

        return found->second;
    }

    // The SoundFont's attack, decay, sustain and release of each value.
    const std::array<int, kEnvelopeValues>& Attacks() const
    {
        return attacks_;
    }

    const std::array<int, kEnvelopeValues>& Decays() const
    {
        return decays_;
    }

    const std::array<int, kEnvelopeValues>& Sustains() const
    {
        return sustains_;
    }

    const std::array<int, kEnvelopeValues>& Releases() const
    {
        return releases_;
    }

private:
    std::array<int, kEnvelopeValues> attacks_{};
    std::array<int, kEnvelopeValues> decays_{};
    std::array<int, kEnvelopeValues> sustains_{};
    std::array<int, kEnvelopeValues> releases_{};
    export_::Settings settings_;
    std::map<uint8_t, std::array<int, kEnvelopeValues>> holds_;
};

// The value whose entry of `table` is nearest `target`. Of two at the same distance, the first.
uint8_t Nearest(std::span<const int> table, double target)
{
    std::size_t best = 0;
    for (std::size_t value = 1; value < table.size(); ++value)
    {
        if (std::abs(table[value] - target) < std::abs(table[best] - target))
        {
            best = value;
        }
    }

    return static_cast<uint8_t>(best);
}

// The settings by which ntdlib's export converts the envelopes of the console of `flavor`.
export_::Settings SettingsOf(sar::Flavor flavor)
{
    switch (flavor)
    {
    case sar::Flavor::kCtr:
        return export_::CtrSettings();
    case sar::Flavor::kCafe:
        return export_::CafeSettings();
    default:
        return export_::SwitchSettings();
    }
}

// Builds a bank's regions and waves from a SoundFont's voices.
class Builder
{
public:
    Builder(const synth::SoundFontData& font, sar::Flavor flavor)
        : font_(font), flavor_(flavor), envelopes_(SettingsOf(flavor))
    {
    }

    // The region of `voice` at `key`, or nothing for a voice without a sample that the bank can play. With `right`, the
    // wave contains the channels of a stereo sample, and plays at the centre: each channel reaches its side as the two
    // voices do.
    std::optional<sar::VelocityRegion> RegionOf(const synth::VoiceSetup& voice, int key,
                                                const SoundFontWave* right = nullptr)
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

        const synth::SampleRecord& sample = *voice.sample;
        sar::VelocityRegion region;
        region.wave_id_index = WaveIndex(*wave, right, sample.rate);

        // The pitch: the root key, and the tuning in cents. A scale tuning other than 100 cents a key plays this key's
        // pitch from a region of its own.
        const int root = SoundFontRootKey(voice);
        double cents = at(kCoarseTune) * 100.0 + at(kFineTune) + sample.correction;
        int original = root;
        if (at(kScaleTuning) != 100.0)
        {
            cents += (key - root) * at(kScaleTuning);
            original = key;
        }
        if (original < 0 || original > 127)
        {
            const int clamped = std::clamp(original, 0, 127);
            cents += (clamped - original) * 100.0;
            original = clamped;
        }

        region.original_key = static_cast<uint8_t>(original);
        region.pitch = static_cast<float>(std::exp2(cents / 1200.0));

        const double attenuation = SoundFontAttenuation(voice);
        region.volume = static_cast<uint8_t>(std::clamp(std::lround(127.0 * std::pow(10.0, -attenuation / 200.0)), 0L,
                                                        long{std::numeric_limits<uint8_t>::max()}));
        region.pan = right != nullptr
                         ? uint8_t{64}
                         : static_cast<uint8_t>(std::clamp(64L + std::lround(63.0 * at(kPan) / 500.0), 1L, 127L));

        // The key group: a number from 1 for each exclusive class.
        if (const int exclusive = static_cast<int>(at(kExclusiveClass)); exclusive != 0)
        {
            const auto [group, added] = key_groups_.emplace(exclusive, static_cast<int>(key_groups_.size()) + 1);
            if (group->second <= kMostKeyGroups)
            {
                region.key_group = static_cast<uint8_t>(group->second);
            }
            else if (added)
            {
                ++left_out_["exclusive classes after the 15th"];
            }
        }

        // The envelope that comes nearest.
        region.adshr.attack = Nearest(envelopes_.Attacks(), at(kAttackVolEnv));
        region.adshr.hold = Nearest(envelopes_.Holds(region.adshr.attack), at(kHoldVolEnv));
        region.adshr.decay = Nearest(envelopes_.Decays(), at(kDecayVolEnv));
        region.adshr.sustain = Nearest(envelopes_.Sustains(), at(kSustainVolEnv));
        region.adshr.release = Nearest(envelopes_.Releases(), at(kReleaseVolEnv));
        region.ignore_note_off = at(kPlaysPastRelease) != 0.0;

        return region;
    }

    // The bank's waves in the order of the table of waves. Each is a wave file of the console.
    const std::vector<std::vector<uint8_t>>& Waves() const
    {
        return waves_;
    }

    // What the bank leaves out, and at how many keys and runs of velocities.
    const std::map<std::string, std::size_t>& LeftOutCounts() const
    {
        return left_out_;
    }

private:
    // The index in the table of waves of `wave`, with `right` as its second channel. A new wave is added after the
    // others.
    uint32_t WaveIndex(const SoundFontWave& wave, const SoundFontWave* right, uint32_t rate)
    {
        const auto [found, added] =
            indexes_.emplace(std::pair(wave, right != nullptr ? std::optional<SoundFontWave>(*right) : std::nullopt),
                             static_cast<uint32_t>(waves_.size()));
        if (!added)
        {
            return found->second;
        }

        const bool big_endian = flavor_ == sar::Flavor::kCafe;
        const auto count = static_cast<std::size_t>(wave.end - wave.start);
        sar::WaveFile file;
        file.flavor = flavor_;
        file.version = song::ExportVersions(song::ItemKind::kWaveArchive, flavor_).waves;
        file.encoding = sar::WaveEncoding::kPcm16;
        file.sample_rate = rate;
        file.loop = wave.loop;
        file.loop_start = wave.loop ? static_cast<uint32_t>(wave.loop_start - wave.start) : 0;
        file.original_loop_start = file.loop_start;
        file.loop_end = static_cast<uint32_t>(count);
        const auto add_channel = [&](const SoundFontWave& points)
        {
            sar::WaveFile::Channel channel;
            channel.samples.resize(count * 2);
            for (std::size_t i = 0; i < count; ++i)
            {
                const auto point = static_cast<uint16_t>(font_.points[static_cast<std::size_t>(points.start) + i]);
                big_endian ? xsflib::StoreBe16(&channel.samples[i * 2], point)
                           : xsflib::StoreLe16(&channel.samples[i * 2], point);
            }

            file.channels.push_back(std::move(channel));
        };
        add_channel(wave);
        if (right != nullptr)
        {
            add_channel(*right);
        }

        waves_.push_back(file.Write());

        return found->second;
    }

    const synth::SoundFontData& font_;
    sar::Flavor flavor_;
    Envelopes envelopes_;
    std::map<std::pair<SoundFontWave, std::optional<SoundFontWave>>, uint32_t> indexes_;
    std::vector<std::vector<uint8_t>> waves_;
    std::map<int, int> key_groups_;
    std::map<std::string, std::size_t> left_out_;
};

// The regions of key `key` of `preset`, a preset of `font`, over the velocity ranges that start at `velocities`, with
// the next range joined where it plays the same (AddPreset). `builder` makes the regions and their waves. `layered`
// gains one for each range whose voices layer.
std::vector<Region> RegionsAtKey(const synth::SoundFontData& font, const synth::PresetRecord& preset, int key,
                                 const std::vector<int>& velocities, Builder& builder, std::size_t& layered)
{
    std::vector<Region> at_key;
    for (std::size_t v = 0; v < velocities.size(); ++v)
    {
        const int last = v + 1 < velocities.size() ? velocities[v + 1] - 1 : 127;
        const std::vector<synth::VoiceSetup> voices = synth::VoicesFor(font, preset, key, std::max(velocities[v], 1));
        if (voices.empty())
        {
            continue;
        }

        const std::optional<StereoVoices> stereo = StereoVoicesOf(font, voices);
        layered += voices.size() > 1 && !stereo;
        const std::optional<sar::VelocityRegion> values =
            stereo ? builder.RegionOf(*stereo->left, key, &stereo->right_wave) : builder.RegionOf(voices.front(), key);
        if (!values)
        {
            continue;
        }

        if (!at_key.empty() && at_key.back().values == *values && at_key.back().last_velocity + 1 == velocities[v])
        {
            at_key.back().last_velocity = static_cast<uint8_t>(last);
            continue;
        }

        at_key.push_back({static_cast<uint8_t>(key), static_cast<uint8_t>(key), static_cast<uint8_t>(velocities[v]),
                          static_cast<uint8_t>(last), *values});
    }

    return at_key;
}

// Whether `at_key`, the regions of a key, continue `run`, the regions of the keys before it: the same values over the
// same velocities, from the next key on (AddPreset).
bool Continues(const std::vector<Region>& run, const std::vector<Region>& at_key)
{
    if (run.size() != at_key.size())
    {
        return false;
    }

    for (std::size_t i = 0; i < run.size(); ++i)
    {
        if (run[i].values != at_key[i].values || run[i].first_velocity != at_key[i].first_velocity ||
            run[i].last_velocity != at_key[i].last_velocity || run[i].last_key + 1 != at_key[i].first_key)
        {
            return false;
        }
    }

    return true;
}

// Adds the instrument of `preset`, a preset of `font`, to `bank` when the preset is in MIDI bank `midi_bank`, or in
// any of 0 to 127 for kEveryMidiBank (BankOfSoundFont). `builder` makes the regions and their waves. `layered` gains
// one for each key and velocity range whose voices layer, and `any` turns true for a preset in the MIDI bank.
void AddPreset(const synth::SoundFontData& font, const synth::PresetRecord& preset, int midi_bank, Builder& builder,
               sar::BankFile& bank, std::size_t& layered, bool& any)
{
    const bool every = midi_bank == kEveryMidiBank;
    const int64_t number = every ? int64_t{preset.bank} * 128 + preset.program : preset.program;
    if ((every ? preset.bank < 0 || preset.bank >= 128 || preset.program >= 128 : preset.bank != midi_bank) ||
        preset.program < 0 || number > kHighestProgram)
    {
        return;
    }

    any = true;

    // Each key's regions over the velocities, joined with the next key's where they play the same.
    const std::vector<int> velocities = SoundFontVelocityStarts(font, preset);
    std::vector<Region> regions;
    std::vector<Region> run;
    const auto close_run = [&regions, &run]
    {
        regions.insert(regions.end(), run.begin(), run.end());
        run.clear();
    };
    for (int key = 0; key < 128; ++key)
    {
        std::vector<Region> at_key = RegionsAtKey(font, preset, key, velocities, builder, layered);
        if (!at_key.empty() && Continues(run, at_key))
        {
            for (Region& region : run)
            {
                region.last_key = static_cast<uint8_t>(key);
            }

            continue;
        }

        close_run();
        run = std::move(at_key);
    }

    close_run();
    if (regions.empty())
    {
        return;
    }

    const auto program = static_cast<std::size_t>(number);
    if (bank.instruments.size() <= program)
    {
        bank.instruments.resize(program + 1);
    }

    bank.instruments[program] = InstrumentOf(std::move(regions));
}

} // namespace

SoundFontBank BankOfSoundFont(std::span<const uint8_t> sf2, sar::Flavor flavor, int midi_bank)
{
    const synth::SoundFontData font = ReadSoundFontToImport(sf2);

    sar::BankFile bank;
    bank.flavor = flavor;
    bank.version = song::ExportVersions(song::ItemKind::kBank, flavor).file;
    Builder builder(font, flavor);
    std::size_t layered = 0;
    bool any = false;
    for (const synth::PresetRecord& preset : font.presets)
    {
        AddPreset(font, preset, midi_bank, builder, bank, layered, any);
    }

    if (!any)
    {
        throw std::invalid_argument(midi_bank == kEveryMidiBank
                                        ? std::string("The SoundFont has no presets in MIDI banks 0–127.")
                                        : "The SoundFont has no presets in MIDI bank " + std::to_string(midi_bank) +
                                              ".");
    }

    SoundFontBank result;
    for (uint32_t index = 0; index < builder.Waves().size(); ++index)
    {
        bank.waves.push_back({kSoundFontWaves, index});
    }

    result.bank = bank.Write();
    sar::WaveArchiveFile archive;
    archive.flavor = flavor;
    archive.version = song::ExportVersions(song::ItemKind::kWaveArchive, flavor).file;
    archive.waves = builder.Waves();
    result.wave_archive = archive.Write();

    result.layered = layered;
    result.left_out = builder.LeftOutCounts();
    result.warnings = SoundFontWarnings(result.layered, result.left_out);

    return result;
}

std::vector<std::string> SoundFontWarnings(std::size_t layered, const std::map<std::string, std::size_t>& left_out)
{
    std::vector<std::string> warnings;
    if (layered > 0)
    {
        warnings.push_back("The SoundFont layers several voices at " + std::to_string(layered) +
                           (layered == 1 ? " key" : " keys") + ". Only the first voice is used.");
    }

    for (const auto& [what, count] : left_out)
    {
        warnings.push_back("Ignored at " + std::to_string(count) + (count == 1 ? " key: " : " keys: ") + what + ".");
    }

    return warnings;
}

} // namespace hokuto::edit
