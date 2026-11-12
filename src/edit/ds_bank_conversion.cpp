// SPDX-License-Identifier: MIT

// The bank side of a sound sent to the DS (edit/ds_conversion.h): the regions, the key runs and the instruments
// of a bank, and the bank and wave archive files.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "ntdlib/sar/bank_file.h"
#include "ntdlib/sar/wave_file.h"
#include "ntdlib/sdat/files.h"
#include "ntdlib/sdat/formats.h"

#include "audio/resampler.h"
#include "edit/ds_conversion.h"
#include "edit/ds_waves.h"

namespace hokuto::edit
{
namespace
{

namespace sar = ntdlib::sar;
namespace sdat = ntdlib::sdat;

// A release of 255 in a region plays a note to the end of its wave. The driver then uses a release of 0.
constexpr uint8_t kPlaysToEnd = 0xFF;

// The keys that a region can play, and the runs that a key split can have.
constexpr int kKeys = 128;
constexpr std::size_t kMostSplits = 8;

// The shortest loop that resamples to a whole number of words. A shorter loop repeats at little cost.
constexpr uint32_t kLeastStretchedLoop = 64;

// The words of a wave: a word contains 8 samples of IMA-ADPCM, and the wave starts with a word of the decoder state.
constexpr uint32_t kSamplesInWord = 8;
constexpr std::size_t kWaveHeadBytes = 12 + 4;

// The samples of a wave that it plays: up to the end of its loop, or its end.
uint32_t PlayedSamples(const sar::WaveFile& wave)
{
    const uint32_t held = HeldSamples(wave);
    return wave.loop_end > 0 ? std::min(held, wave.loop_end) : held;
}

// The region that plays a run of keys, before its wave has an index of the DS.
struct KeyRun
{
    int first = 0;
    int last = 0;
    sdat::Region region;
};

// The wave of a region on the DS: an entry of the bank's table of waves, at a rate and a gain.
struct WaveUse
{
    auto operator<=>(const WaveUse&) const = default;

    uint32_t entry = 0;
    uint32_t rate = 0;
    double gain = 1.0;
};

// The velocity region of `key_region` that plays its keys: the one with the widest run of velocities, and the higher of
// two such. Nothing for a key region without one.
const sar::BankFile::Region* ChosenRegion(const sar::BankFile::KeyRegion& key_region)
{
    const sar::BankFile::Region* chosen = nullptr;
    int widest = -1;
    for (const sar::BankFile::Region& region : key_region.regions)
    {
        const int width = region.last_velocity - region.first_velocity;
        if (width >= widest)
        {
            widest = width;
            chosen = &region;
        }
    }

    return chosen;
}

// The rate and the root key of a region whose wave plays at `rate` Hz, clamped to the rates that a wave supports. An
// octave of the rate moves into the root key where the rate falls outside the rates that a wave supports.
struct RateAndRoot
{
    uint32_t rate = 0;
    int root = 60;
    bool held = false;
};

RateAndRoot RateAndRootOf(double rate, int root)
{
    constexpr int kOctave = 12;
    RateAndRoot found{.rate = 0, .root = root};
    while (rate > kHighestDsRate && found.root >= kOctave)
    {
        rate /= 2.0;
        found.root -= kOctave;
    }
    while (rate < kLeastDsRate && found.root + kOctave < kKeys)
    {
        rate *= 2.0;
        found.root += kOctave;
    }

    const double kept = std::clamp(rate, static_cast<double>(kLeastDsRate), static_cast<double>(kHighestDsRate));
    found.rate = static_cast<uint32_t>(std::lround(kept));
    found.held = kept != rate;

    return found;
}

// The rate and the root key of a region that plays `wave` with `values`.
RateAndRoot RateAndRootOf(const sar::WaveFile& wave, const sar::VelocityRegion& values)
{
    return RateAndRootOf(static_cast<double>(wave.sample_rate) * values.pitch,
                         std::min<int>(values.original_key, kKeys - 1));
}

// The samples of a wave as the conversion lays them out: at their rate, with the start of their loop.
struct LaidOutSamples
{
    std::vector<int16_t> samples;
    uint32_t rate = 0;
    std::optional<uint32_t> loop_start;
};

// `samples` at `rate`, held to `most_rate`, with a loop in whole words (see DsBankFilesOf). The loop of `samples` runs
// from `loop_start` to the end of the samples.
LaidOutSamples LaidOutOf(std::vector<int16_t> samples, uint32_t rate, std::optional<uint32_t> loop_start,
                         uint32_t most_rate)
{
    const uint32_t target = std::min(rate, most_rate);
    if (!loop_start)
    {
        if (target == rate || samples.empty())
        {
            return {std::move(samples), rate, std::nullopt};
        }

        return {audio::Resample(samples, rate, target), target, std::nullopt};
    }

    const auto length = static_cast<uint32_t>(samples.size() - *loop_start);
    const bool lower = target < rate;
    if (length < kLeastStretchedLoop && !lower)
    {
        return {std::move(samples), rate, loop_start};
    }

    // The loop length nearest the one that follows from the rate, in whole words.
    const double wanted = static_cast<double>(length) * target / rate;
    const auto words = std::max(std::lround(wanted / kSamplesInWord), 1L);
    const auto stretched = static_cast<uint32_t>(words) * kSamplesInWord;
    const double new_rate = static_cast<double>(rate) * stretched / length;
    if (stretched == length || new_rate < kLeastDsRate || new_rate > kHighestDsRate)
    {
        return {std::move(samples), rate, loop_start};
    }

    return {audio::Resample(samples, length, stretched, *loop_start), static_cast<uint32_t>(std::lround(new_rate)),
            static_cast<uint32_t>(audio::ResampledPlace(*loop_start, length, stretched))};
}

// The instrument of `runs`, in the order of their keys, none of them overlapping (see the header).
sdat::Instrument InstrumentOf(const std::vector<KeyRun>& runs)
{
    sdat::Instrument instrument;
    if (runs.size() == 1 && runs.front().first == 0 && runs.front().last == kKeys - 1)
    {
        instrument.type = sdat::InstrumentType::kPcm;
        instrument.region = runs.front().region;
        return instrument;
    }

    // The runs of a key split, with a region that plays nothing in a gap. The keys past the last run play nothing, and
    // a last key of 0 would end the list.
    std::vector<KeyRun> splits;
    int next = 0;
    for (const KeyRun& run : runs)
    {
        if (run.first > next)
        {
            splits.push_back({next, run.first - 1, sdat::Region{}});
        }

        splits.push_back(run);
        next = run.last + 1;
    }
    if (splits.size() <= kMostSplits && splits.front().last > 0)
    {
        instrument.type = sdat::InstrumentType::kKeySplit;
        for (std::size_t i = 0; i < splits.size(); ++i)
        {
            instrument.split_keys[i] = static_cast<uint8_t>(splits[i].last);
            instrument.regions.push_back(splits[i].region);
        }

        return instrument;
    }

    // A drum set has a region for each key from its first to its last.
    instrument.type = sdat::InstrumentType::kDrumSet;
    instrument.min_key = static_cast<uint8_t>(runs.front().first);
    instrument.max_key = static_cast<uint8_t>(runs.back().last);
    for (int key = runs.front().first; key <= runs.back().last; ++key)
    {
        const auto holds_key = [key](const KeyRun& run)
        {
            return run.first <= key && key <= run.last;
        };
        const auto run = std::ranges::find_if(runs, holds_key);
        instrument.regions.push_back(run != runs.end() ? run->region : sdat::Region{});
    }

    return instrument;
}

// The samples of `wave` that it plays as one channel: the samples of the first channel, or the mean of two, rounded
// half away from zero.
std::vector<int16_t> OneChannelOf(const sar::WaveFile& wave)
{
    const uint32_t count = PlayedSamples(wave);
    std::vector<std::vector<int16_t>> channels = DecodedSamples(wave, count);
    if (channels.size() < 2)
    {
        return channels.empty() ? std::vector<int16_t>{} : std::move(channels.front());
    }

    std::vector<int16_t> mean(count);
    for (uint32_t i = 0; i < count; ++i)
    {
        const int sum = channels[0][i] + channels[1][i];
        mean[i] = static_cast<int16_t>(sum >= 0 ? (sum + 1) / 2 : (sum - 1) / 2);
    }

    return mean;
}

// The largest magnitude among `samples`.
int PeakOf(std::span<const int16_t> samples)
{
    int peak = 0;
    for (const int16_t sample : samples)
    {
        peak = std::max(peak, std::abs(static_cast<int>(sample)));
    }

    return peak;
}

} // namespace

std::size_t DsWavesSize(const sar::BankFile& bank, std::span<const std::optional<sar::WaveFile>> waves,
                        uint32_t most_rate)
{
    std::set<std::tuple<uint32_t, uint32_t, uint8_t>> uses;
    for (const std::optional<sar::BankFile::Instrument>& instrument : bank.instruments)
    {
        if (!instrument)
        {
            continue;
        }

        for (const sar::BankFile::KeyRegion& key_region : instrument->key_regions)
        {
            const sar::BankFile::Region* region = ChosenRegion(key_region);
            if (region == nullptr)
            {
                continue;
            }

            const uint32_t entry = region->values.wave_id_index;
            if (entry < waves.size() && waves[entry])
            {
                uses.insert({entry, RateAndRootOf(*waves[entry], region->values).rate, region->values.volume});
            }
        }
    }

    std::size_t size = 0;
    for (const auto& [entry, rate, volume] : uses)
    {
        const uint64_t samples = PlayedSamples(*waves[entry]);
        size += kWaveHeadBytes + (rate > most_rate ? samples * most_rate / rate : samples) / 2;
    }

    return size;
}

void AddDsBankWarnings(const DsBankFiles& files, bool at_mix_rate, std::vector<std::string>& warnings)
{
    if (files.stereo_waves > 0)
    {
        warnings.push_back(files.stereo_waves == 1
                               ? "DS waves are mono. 1 stereo wave is converted to mono by averaging its channels."
                               : "DS waves are mono. " + std::to_string(files.stereo_waves) +
                                     " stereo waves are converted to mono by averaging their channels.");
    }

    if (files.velocity_splits > 0)
    {
        warnings.push_back(
            "DS regions have no velocity ranges. " + std::to_string(files.velocity_splits) +
            (files.velocity_splits == 1 ? " key range of the bank uses" : " key ranges of the bank use") +
            " different regions at different velocities. Only the region with the widest velocity range is kept.");
    }

    if (files.holds > 0)
    {
        warnings.push_back(std::to_string(files.holds) +
                           (files.holds == 1 ? " region holds its level" : " regions hold their level") +
                           " before the decay. DS regions have no hold. The decay starts at once.");
    }

    if (files.key_groups > 0)
    {
        warnings.push_back(std::to_string(files.key_groups) + (files.key_groups == 1 ? " region is" : " regions are") +
                           " in a key group. DS regions have no key groups. Their notes don't cut off other notes.");
    }

    if (files.lost_loops > 0)
    {
        warnings.push_back(
            files.lost_loops == 1
                ? std::string("1 wave loses its loop and plays once. The loop starts beyond the 65,535-word limit of "
                              "a DS wave.")
                : std::to_string(files.lost_loops) +
                      " waves lose their loops and play once. The loops start beyond the 65,535-word limit of a DS "
                      "wave.");
    }

    if (at_mix_rate && files.rates_lowered > 0)
    {
        static_assert(kDsMixRate == 32728, "The warning's text gives the DS's mix rate.");
        warnings.push_back("The DS mixes at 32,728 Hz. " + std::to_string(files.rates_lowered) +
                           (files.rates_lowered == 1 ? " wave above that rate is" : " waves above that rate are") +
                           " resampled to it.");
    }

    if (files.rates_held > 0)
    {
        warnings.push_back(std::to_string(files.rates_held) +
                           (files.rates_held == 1 ? " region plays a wave at a rate above the DS maximum and uses "
                                                    "the nearest rate. Its notes play at a different pitch."
                                                  : " regions play waves at rates above the DS maximum and use the "
                                                    "nearest rate. Their notes play at a different pitch."));
    }
}

namespace
{

// Lowers `result.scale` so that each wave of `waves` keeps its samples within 16 bits at its loudest gain among `uses`
// (DsBankFilesOf).
void LevelGains(std::span<const WaveUse> uses, std::span<const std::optional<sar::WaveFile>> waves, DsBankFiles& result)
{
    std::map<uint32_t, double> loudest;
    for (const WaveUse& use : uses)
    {
        double& gain = loudest[use.entry];
        gain = std::max(gain, use.gain);
    }

    for (const auto& [entry, gain] : loudest)
    {
        if (gain > 1.0)
        {
            const int peak = PeakOf(OneChannelOf(*waves[entry]));
            if (peak > 0)
            {
                result.scale = std::min(result.scale, 32767.0 / (gain * peak));
            }
        }
    }
}

// The wave of each of `uses`, a wave of `waves`, in IMA-ADPCM at its gain and `result.scale`, at a rate up to
// `most_rate` (DsBankFilesOf). `result` counts the lowered rates and the lost loops.
std::vector<std::vector<uint8_t>> EncodeWaves(std::span<const WaveUse> uses,
                                              std::span<const std::optional<sar::WaveFile>> waves, uint32_t most_rate,
                                              DsBankFiles& result)
{
    std::vector<std::vector<uint8_t>> encoded;
    for (const WaveUse& use : uses)
    {
        const sar::WaveFile& wave = *waves[use.entry];
        std::vector<int16_t> samples = OneChannelOf(wave);
        const double gain = use.gain * result.scale;
        if (gain != 1.0)
        {
            for (int16_t& sample : samples)
            {
                sample = static_cast<int16_t>(std::clamp<long>(std::lround(sample * gain), -32768, 32767));
            }
        }

        const bool loops = wave.loop && wave.loop_start < samples.size();
        LaidOutSamples laid_out =
            LaidOutOf(std::move(samples), use.rate, loops ? std::optional(wave.loop_start) : std::nullopt, most_rate);
        result.rates_lowered += use.rate > most_rate ? 1 : 0;

        // The field of a loop's start counts words, the decoder's state first.
        const uint32_t words =
            laid_out.loop_start ? (*laid_out.loop_start + kSamplesInWord - 1) / kSamplesInWord + 1 : 0;
        if (words > std::numeric_limits<uint16_t>::max())
        {
            ++result.lost_loops;
            laid_out.loop_start = std::nullopt;
        }

        encoded.push_back(
            EncodeDsWave(laid_out.samples, laid_out.rate, sdat::WaveEncoding::kImaAdpcm, laid_out.loop_start));
    }

    return encoded;
}

// Makes the regions of a DS bank from the waves `waves` (DsBankFilesOf). Each wave is placed when a region first plays
// it. A stereo wave plays at the gain that `stereo_gain` returns for its mono level. The counts go into `result`.
class DsRegionMaker
{
public:
    DsRegionMaker(std::span<const std::optional<sar::WaveFile>> waves,
                  std::function<double(const MonoLevel&)> stereo_gain, DsBankFiles& result)
        : waves_(waves), stereo_gain_(std::move(stereo_gain)), result_(result)
    {
    }

    // The runs of keys of `instrument`, with its regions on the DS.
    std::vector<KeyRun> KeyRunsOf(const sar::BankFile::Instrument& instrument);

    // The waves that the regions play, at their rates and gains, in the order of their places.
    const std::vector<WaveUse>& Uses() const
    {
        return uses_;
    }

private:
    // The gain of wave `entry`: 1 for a mono wave, the stereo gain once for a stereo one.
    double GainOf(uint32_t entry);

    std::span<const std::optional<sar::WaveFile>> waves_;
    std::function<double(const MonoLevel&)> stereo_gain_;
    DsBankFiles& result_;
    std::map<uint32_t, double> mono_gains_;
    std::map<WaveUse, uint16_t> places_;
    std::vector<WaveUse> uses_;
};

double DsRegionMaker::GainOf(uint32_t entry)
{
    const sar::WaveFile& wave = *waves_[entry];
    if (wave.channels.size() != 2)
    {
        return 1.0;
    }

    const auto [place, added] = mono_gains_.try_emplace(entry, 1.0);
    if (added)
    {
        place->second = stereo_gain_(MonoLevelOf(wave));
        ++result_.stereo_waves;
    }

    return place->second;
}

std::vector<KeyRun> DsRegionMaker::KeyRunsOf(const sar::BankFile::Instrument& instrument)
{
    std::vector<KeyRun> runs;
    for (const sar::BankFile::KeyRegion& key_region : instrument.key_regions)
    {
        const sar::BankFile::Region* chosen = ChosenRegion(key_region);
        if (chosen == nullptr)
        {
            continue;
        }

        result_.velocity_splits += key_region.regions.size() > 1 ? 1 : 0;
        const sar::VelocityRegion& values = chosen->values;
        result_.key_groups += values.key_group != 0 ? 1 : 0;
        result_.holds += values.adshr.hold != 0 && values.adshr.sustain != 127 ? 1 : 0;
        const uint32_t entry = values.wave_id_index;
        if (entry >= waves_.size() || !waves_[entry] || PlayedSamples(*waves_[entry]) == 0)
        {
            ++result_.silent_regions;
            continue;
        }

        const sar::WaveFile& wave = *waves_[entry];
        const RateAndRoot rate = RateAndRootOf(wave, values);
        result_.rates_held += rate.held ? 1 : 0;
        const WaveUse use{entry, rate.rate, static_cast<double>(values.volume) / 127.0 * GainOf(entry)};
        const auto [place, added] = places_.try_emplace(use, static_cast<uint16_t>(uses_.size()));
        if (added)
        {
            uses_.push_back(use);
        }

        sdat::Region region;
        region.type = sdat::InstrumentType::kPcm;
        region.wave = place->second;
        region.wave_archive = 0;
        region.root_key = static_cast<uint8_t>(rate.root);
        region.attack = values.adshr.attack;
        region.decay = values.adshr.decay;
        region.sustain = values.adshr.sustain;
        region.release = values.ignore_note_off ? kPlaysToEnd : values.adshr.release;
        region.pan = std::min<uint8_t>(values.pan, 127);
        if (!runs.empty() && runs.back().last + 1 == key_region.first_key && runs.back().region == region)
        {
            runs.back().last = key_region.last_key;
        }
        else
        {
            runs.push_back({key_region.first_key, key_region.last_key, region});
        }
    }

    return runs;
}

} // namespace

DsBankFiles DsBankFilesOf(const sar::BankFile& bank, std::span<const std::optional<sar::WaveFile>> waves,
                          const std::function<double(const MonoLevel&)>& stereo_gain, uint32_t most_rate)
{
    DsBankFiles result;

    // Each instrument's runs of keys, with its regions on the DS. Each wave is placed when a region first plays it.
    DsRegionMaker regions(waves, stereo_gain, result);
    sdat::Bank model;
    for (const std::optional<sar::BankFile::Instrument>& instrument : bank.instruments)
    {
        sdat::Instrument& made = model.instruments.emplace_back();
        if (!instrument)
        {
            continue;
        }

        const std::vector<KeyRun> runs = regions.KeyRunsOf(*instrument);
        if (!runs.empty())
        {
            made = InstrumentOf(runs);
        }
    }

    // A gain above 1 can take the samples past 16 bits. Every wave is then scaled to the level that keeps the loudest
    // wave within them.
    LevelGains(regions.Uses(), waves, result);

    // The waves, in IMA-ADPCM.
    const std::vector<std::vector<uint8_t>> encoded = EncodeWaves(regions.Uses(), waves, most_rate, result);

    result.wave_count = encoded.size();
    result.bank = sdat::BankFile(model);
    if (!encoded.empty())
    {
        result.waves = sdat::WaveArchiveFile(encoded);
    }

    return result;
}

} // namespace hokuto::edit
