// SPDX-License-Identifier: MIT

// Tests the lower rates of single waves of a bank sent to the DS (src/edit/ds_bank_conversion.cpp). Where a bank
// doesn't fit on the DS, edit::LowerLargestDsWave gives one wave the next of edit::kDsLowerRates below the highest rate
// at which it plays. The wave whose next rate is the highest goes first, and of two such waves, the larger. A large
// wave at 48,000 Hz and a small wave at 32,000 Hz have to take each rate in turn, the large wave first, and the bytes
// of edit::DsWavesSize have to fall with each step. A wave at 8,182 Hz or below takes no lower rate. The DS files have
// to hold each wave at its rate, and count the waves resampled to each lower rate apart from those resampled to the
// DS's mix rate. A looped wave whose loop can't take the whole words of the lower rate keeps its rate, and doesn't
// count.

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <vector>

#include "ntdlib/sar/bank_file.h"
#include "ntdlib/sar/wave_file.h"
#include "ntdlib/sdat/formats.h"
#include "xsflib/testing/check.h"

#include "edit/ds_conversion.h"
#include "edit/waves.h"

namespace
{

namespace edit = hokuto::edit;
namespace sar = ntdlib::sar;
namespace sdat = ntdlib::sdat;

// A wave of `count` samples of 16-bit PCM in one channel at `rate`, with a loop from `loop_start` to its end or
// without one.
sar::WaveFile WaveOf(std::size_t count, uint32_t rate, std::optional<uint32_t> loop_start = std::nullopt)
{
    sar::WaveFile wave;
    wave.flavor = sar::Flavor::kNx;
    wave.encoding = sar::WaveEncoding::kPcm16;
    wave.sample_rate = rate;
    wave.loop_end = static_cast<uint32_t>(count);
    wave.loop = loop_start.has_value();
    wave.loop_start = loop_start.value_or(0);
    sar::WaveFile::Channel& channel = wave.channels.emplace_back();
    for (std::size_t i = 0; i < count; ++i)
    {
        const auto sample = static_cast<int16_t>(i % 64 < 32 ? 1000 : -1000);
        channel.samples.push_back(static_cast<uint8_t>(sample & 0xFF));
        channel.samples.push_back(static_cast<uint8_t>((sample >> 8) & 0xFF));
    }

    return wave;
}

// An instrument whose one region plays entry `wave` of the bank's table over every key and velocity.
sar::BankFile::Instrument InstrumentOf(uint32_t wave)
{
    sar::BankFile::Region region;
    region.first_velocity = 0;
    region.last_velocity = 127;
    region.values.wave_id_index = wave;
    region.values.adshr = {127, 127, 127, 0, 127};
    sar::BankFile::Instrument instrument;
    instrument.key_regions.push_back({0, 127, {region}});

    return instrument;
}

double NoStereoGain(const edit::MonoLevel&)
{
    return 1.0;
}

// A bank of a large wave at 48,000 Hz in program 0 and a small wave at 32,000 Hz in program 1.
struct TwoWaves
{
    sar::BankFile bank;
    std::vector<std::optional<sar::WaveFile>> waves;
};

TwoWaves TwoWavesBank()
{
    TwoWaves made;
    made.bank.instruments.emplace_back(InstrumentOf(0));
    made.bank.instruments.emplace_back(InstrumentOf(1));
    made.waves = {WaveOf(48000, 48000), WaveOf(8000, 32000)};

    return made;
}

// Both waves take each lower rate before either takes the next, the large wave first, and the bytes fall with each
// step.
void TestLargestFirst()
{
    const TwoWaves two = TwoWavesBank();
    edit::DsWaveRates rates;
    std::vector<edit::DsWaveRates> steps;
    std::vector<std::size_t> sizes = {edit::DsWavesSize(two.bank, two.waves)};

    while (edit::LowerLargestDsWave(two.bank, two.waves, rates))
    {
        steps.push_back(rates);
        sizes.push_back(edit::DsWavesSize(two.bank, two.waves, edit::kDsMixRate, rates));
    }

    const std::vector<edit::DsWaveRates> expected = {
        {{0, 24000}},
        {{0, 24000}, {1, 24000}},
        {{0, 16364}, {1, 24000}},
        {{0, 16364}, {1, 16364}},
        {{0, 12000}, {1, 16364}},
        {{0, 12000}, {1, 12000}},
        {{0, 8182}, {1, 12000}},
        {{0, 8182}, {1, 8182}},
    };
    XSFLIB_CHECK(steps == expected);
    for (std::size_t step = 1; step < sizes.size(); ++step)
    {
        XSFLIB_CHECK(sizes[step] < sizes[step - 1]);
    }
}

// A wave at 8,182 Hz or below takes no lower rate.
void TestLowestRate()
{
    sar::BankFile bank;
    bank.instruments.emplace_back(InstrumentOf(0));
    const std::vector<std::optional<sar::WaveFile>> waves = {WaveOf(4000, 8000)};
    edit::DsWaveRates rates;

    const std::optional<uint32_t> lowered = edit::LowerLargestDsWave(bank, waves, rates);

    XSFLIB_CHECK(!lowered);
    XSFLIB_CHECK(rates.empty());
}

// The DS files hold each wave at its rate. A wave resampled to a rate of `wave_rates` counts apart from one resampled
// to the mix rate.
void TestFiles()
{
    const TwoWaves two = TwoWavesBank();

    const edit::DsBankFiles large_lowered =
        edit::DsBankFilesOf(two.bank, two.waves, NoStereoGain, edit::kDsMixRate, {{0, 16364}});
    const edit::DsBankFiles small_lowered =
        edit::DsBankFilesOf(two.bank, two.waves, NoStereoGain, edit::kDsMixRate, {{1, 24000}});

    const sdat::WaveArchive large = sdat::WaveArchive::Parse(large_lowered.waves);
    SEQLIB_REQUIRE(large.waves.size() == 2);
    XSFLIB_CHECK(large.waves[0].sample_rate == 16364 && large.waves[1].sample_rate == 32000);
    XSFLIB_CHECK(large_lowered.rates_lowered == 0);
    XSFLIB_CHECK(large_lowered.lowered_rates == (std::map<uint32_t, std::size_t>{{16364, 1}}));
    const sdat::WaveArchive small = sdat::WaveArchive::Parse(small_lowered.waves);
    SEQLIB_REQUIRE(small.waves.size() == 2);
    XSFLIB_CHECK(small.waves[0].sample_rate == edit::kDsMixRate && small.waves[1].sample_rate == 24000);
    XSFLIB_CHECK(small_lowered.rates_lowered == 1);
    XSFLIB_CHECK(small_lowered.lowered_rates == (std::map<uint32_t, std::size_t>{{24000, 1}}));
}

// Looped waves at 48,000 Hz with a highest rate of 24,000 Hz. A loop of 4,096 samples takes the whole words nearest
// 2,048 samples and the rate that keeps its pitch, and counts as lowered. A loop of 8 samples, one word, keeps its
// rate: half of it fills no word. It doesn't count.
void TestLoopedWaves()
{
    sar::BankFile bank;
    bank.instruments.emplace_back(InstrumentOf(0));
    bank.instruments.emplace_back(InstrumentOf(1));
    const std::vector<std::optional<sar::WaveFile>> waves = {WaveOf(8192, 48000, 4096), WaveOf(1000, 48000, 992)};

    const edit::DsBankFiles files =
        edit::DsBankFilesOf(bank, waves, NoStereoGain, edit::kDsMixRate, {{0, 24000}, {1, 24000}});

    const sdat::WaveArchive archive = sdat::WaveArchive::Parse(files.waves);
    SEQLIB_REQUIRE(archive.waves.size() == 2);
    XSFLIB_CHECK(archive.waves[0].sample_rate == 24000 && archive.waves[1].sample_rate == 48000);
    XSFLIB_CHECK(files.lowered_rates == (std::map<uint32_t, std::size_t>{{24000, 1}}));
    XSFLIB_CHECK(files.rates_lowered == 0);
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("ds_wave_rates",
                                    []
                                    {
                                        TestLargestFirst();
                                        TestLowestRate();
                                        TestFiles();
                                        TestLoopedWaves();
                                    });
}
