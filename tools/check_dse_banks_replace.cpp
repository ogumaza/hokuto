// SPDX-License-Identifier: MIT

// The rounds of hokuto_check_dse_banks that replace banks and waves.

#include "check_dse_banks.h"

namespace hokuto::tools::dse_banks
{

void BankRounds(const Game& game, Totals& totals)
{
    const DseSet& set = *game.set;
    const std::vector<uint32_t> banks = DistinctBanks(set);
    for (std::size_t place = 0; place < banks.size(); ++place)
    {
        const uint32_t bank = banks[place];
        const dse::WaveBank& held = *set.banks[bank].bank;
        const std::string name = hokuto::song::DseBankName(held);
        try
        {
            const File replaced = edit::ReplaceDseBank(game.file, set, bank, edit::ReadIncomingDseBank(held.Write()));
            if (hokuto::format::ExportArchive(replaced) == game.bytes)
            {
                totals.same_banks++;
            }
            else
            {
                Fail(game, totals, name + ": its file gives another set");
            }
        }
        catch (const std::invalid_argument& error)
        {
            Fail(game, totals, name + " refused its file: " + error.what());
            continue;
        }

        // The programs and waves of the next bank are written to every file that contains the bank, with the bank's id
        // and name.
        std::optional<uint32_t> next;
        for (std::size_t step = 1; step < banks.size() && !next; ++step)
        {
            const uint32_t other = banks[(place + step) % banks.size()];
            if (set.banks[other].bank->samples.has_value() == held.samples.has_value() &&
                set.banks[other].bank->instruments.has_value())
            {
                next = other;
            }
        }
        if (!next || !held.instruments)
        {
            totals.next_banks_apart += next ? 0 : 1;
            continue;
        }
        const dse::WaveBank& incoming = *set.banks[*next].bank;
        try
        {
            const File replaced = edit::ReplaceDseBank(game.file, set, bank, incoming);
            const std::vector<uint8_t> exported = hokuto::format::ExportArchive(replaced);
            const std::shared_ptr<const DseSet> read = hokuto::song::LoadDseSet(exported, game.build);
            std::vector<uint64_t> offsets;
            for (const hokuto::song::DseBank& each : set.banks)
            {
                if (each.bank == set.banks[bank].bank)
                {
                    offsets.push_back(each.file_offset);
                }
            }
            const std::vector<std::string> paths = PathsAt(game.bytes, offsets);
            bool right = OthersKept(game.bytes, exported, paths) && read->banks.size() == set.banks.size();
            for (std::size_t i = 0; i < set.banks.size() && right; ++i)
            {
                if (set.banks[i].bank != set.banks[bank].bank)
                {
                    continue;
                }
                const dse::WaveBank& now = *read->banks[i].bank;
                right = hokuto::song::DseBankId(now) == hokuto::song::DseBankId(held) &&
                        hokuto::song::DseBankName(now) == name && now.instruments == incoming.instruments &&
                        now.waves == incoming.waves && now.samples == incoming.samples;
            }
            if (right)
            {
                totals.next_banks++;
            }
            else
            {
                Fail(game, totals,
                     name + ": the bank of " + hokuto::song::DseBankName(incoming) + " doesn't read back in its place");
            }
        }
        catch (const std::invalid_argument& error)
        {
            std::printf("%s: %s refused %s: %s\n", game.name.c_str(), name.c_str(),
                        hokuto::song::DseBankName(incoming).c_str(), error.what());
            totals.next_banks_refused++;
        }
    }
    std::fflush(stdout);
}

namespace
{

// The ratio of the energy of `wanted` to that of its difference from `got`, in decibels, over `wanted`'s samples.
double RatioOf(std::span<const int16_t> wanted, std::span<const int16_t> got)
{
    double signal = 0.0;
    double noise = 0.0;
    for (std::size_t i = 0; i < wanted.size(); ++i)
    {
        const double sample = wanted[i];
        const double other = i < got.size() ? got[i] : 0.0;
        signal += sample * sample;
        noise += (sample - other) * (sample - other);
    }

    return noise == 0.0 ? 1000.0 : 10.0 * std::log10(std::max(signal, 1.0) / noise);
}

// The step between the waves of `set` that play: the count of waves in a form that Hokuto replaces, divided by
// kSampledWaves and rounded down, and at least 1.
std::size_t SampleStep(const DseSet& set)
{
    std::size_t count = 0;
    for (const uint32_t bank : DistinctBanks(set))
    {
        const dse::WaveBank& held = *set.banks[bank].bank;
        for (const uint32_t wave : held.samples ? hokuto::song::DseBankWaves(held) : std::vector<uint32_t>())
        {
            count += held.waves[wave]->bytes[kWaveFormat] <= kImaAdpcm ? 1 : 0;
        }
    }

    return std::max<std::size_t>(count / kSampledWaves, 1);
}

// Replaces wave `wave` of `bank`, `item` named `name`, with its WAV file. Every `step`th wave that is imported, counted
// in `place`, plays. Returns whether the wave went in and played.
bool ReplaceWave(const Game& game, uint32_t bank, uint32_t wave, const hokuto::song::Item& item,
                 const std::string& name, std::size_t step, std::size_t& place, Totals& totals)
{
    const DseSet& set = *game.set;
    try
    {
        const std::vector<uint8_t> wav = hokuto::song::DseItemBytes(set, item);
        const edit::WaveChanges changes = edit::ChangesOfDseWave(set, bank, wave);
        const edit::IncomingWave incoming = edit::ReadIncomingDsWave(wav, changes.encoding);
        const File replaced =
            edit::ReplaceDseWave(game.file, set, bank, wave, {.wave = incoming.bytes, .rate = incoming.rate});
        const std::vector<uint8_t> exported = hokuto::format::ExportArchive(replaced);
        totals.replaced_waves++;
        totals.same_waves += exported == game.bytes ? 1 : 0;
        if (place++ % step != 0)
        {
            return false;
        }
        totals.sampled_waves++;
        const std::shared_ptr<const DseSet> read = hokuto::song::LoadDseSet(exported, game.build);
        const ntdlib::wave::Pcm before = hokuto::song::DseWavePcm(set, bank, wave);
        const ntdlib::wave::Pcm after = hokuto::song::DseWavePcm(*read, bank, wave);
        const std::span<const int16_t> wanted =
            std::span(before.channels.at(0)).first(std::min<std::size_t>(before.loop_end, before.channels[0].size()));
        if (after.channels.empty() || after.sample_rate != before.sample_rate || after.loop != before.loop ||
            (before.loop && after.loop_start != before.loop_start))
        {
            Fail(game, totals, name + ": the wave comes back with another rate or loop");
        }
        else if (changes.encoding == ntdlib::sar::WaveEncoding::kImaAdpcm)
        {
            const double ratio = RatioOf(wanted, after.channels[0]);
            totals.ima_waves++;
            totals.ima_below += ratio < kLeastImaRatio ? 1 : 0;
            totals.lowest_ima = std::min(totals.lowest_ima, ratio);
        }
        else if (std::ranges::equal(
                     wanted, std::span(after.channels[0]).first(std::min(wanted.size(), after.channels[0].size()))) &&
                 after.channels[0].size() >= wanted.size())
        {
            totals.exact_waves++;
        }
        else
        {
            Fail(game, totals, name + ": the wave decodes to other samples");
        }
    }
    catch (const std::invalid_argument& error)
    {
        Fail(game, totals, name + " refused its WAV file: " + error.what());
        return false;
    }

    return true;
}

// Whether each bank of `read`, `set` with wave `wave` of `bank` at `rate`, plays the wave at that rate, with the tuning
// of each split that plays it moved by the rate of its table.
bool TuningsMove(const DseSet& set, const DseSet& read, uint32_t bank, uint32_t wave, uint32_t rate, Totals& totals)
{
    bool right = true;
    for (std::size_t i = 0; i < set.banks.size() && right; ++i)
    {
        const hokuto::song::DseBank& old_bank = set.banks[i];
        if ((old_bank.bank != set.banks[bank].bank && old_bank.main_bank != set.banks[bank].bank) ||
            wave >= old_bank.bank->waves.size() || !old_bank.bank->waves[wave])
        {
            continue;
        }
        const dse::WaveBank& now = *read.banks[i].bank;
        const uint32_t was = old_bank.bank->waves[wave]->SampleRate();
        right = now.waves.at(wave)->SampleRate() == rate;
        if (!old_bank.bank->instruments || was == 0 || was == rate)
        {
            continue;
        }
        const double moved = kTuningOfDoubledRate * std::log2(static_cast<double>(rate) / was);
        for (std::size_t program = 0; program < old_bank.bank->instruments->programs.size() && right; ++program)
        {
            const auto& old_program = old_bank.bank->instruments->programs[program];
            if (!old_program)
            {
                continue;
            }
            const auto& new_program = now.instruments->programs.at(program);
            for (std::size_t split = 0; split < old_program->splits.size() && right; ++split)
            {
                const auto& old_split = old_program->splits[split];
                if (Get16(old_split, edit::kDseWave) != wave)
                {
                    continue;
                }
                const int tuning = static_cast<int16_t>(Get16(old_split, edit::kDseTuning));
                const int now_tuning = static_cast<int16_t>(Get16(new_program->splits.at(split), edit::kDseTuning));
                right = now_tuning == std::llround(tuning + moved);
                totals.half_tunings++;
            }
        }
    }

    return right;
}

// Replaces wave `wave` of `bank`, `item` named `name`, with its WAV file at `rate`, half the wave's rate.
void HalveRate(const Game& game, uint32_t bank, uint32_t wave, const hokuto::song::Item& item, const std::string& name,
               uint32_t rate, Totals& totals)
{
    const DseSet& set = *game.set;
    try
    {
        const std::vector<uint8_t> wav = hokuto::song::DseItemBytes(set, item);
        const edit::IncomingWave incoming =
            edit::ReadIncomingDsWave(wav, ntdlib::sar::WaveEncoding::kPcm16, std::nullopt, rate);
        const File replaced =
            edit::ReplaceDseWave(game.file, set, bank, wave, {.wave = incoming.bytes, .rate = incoming.rate});
        const std::shared_ptr<const DseSet> read =
            hokuto::song::LoadDseSet(hokuto::format::ExportArchive(replaced), game.build);
        totals.half_rates++;
        const bool right =
            read->banks.size() == set.banks.size() && TuningsMove(set, *read, bank, wave, incoming.rate, totals);
        if (!right)
        {
            Fail(game, totals, name + ": half the rate gives another rate or tuning");
        }
    }
    catch (const std::invalid_argument& error)
    {
        std::printf("%s: %s refused half its rate: %s\n", game.name.c_str(), name.c_str(), error.what());
        totals.half_refused++;
    }
}

} // namespace

void WaveReplaceRound(const Game& game, Totals& totals)
{
    // Every wave is imported. A sample of them plays: every nth, with n their count divided by kSampledWaves and
    // rounded down.
    const DseSet& set = *game.set;
    const std::size_t step = SampleStep(set);
    std::size_t place = 0;
    for (const uint32_t bank : DistinctBanks(set))
    {
        const dse::WaveBank& held = *set.banks[bank].bank;
        if (!held.samples)
        {
            continue;
        }

        for (const uint32_t wave : hokuto::song::DseBankWaves(held))
        {
            if (held.waves[wave]->bytes[kWaveFormat] > kImaAdpcm)
            {
                continue;
            }

            const hokuto::song::Item item{hokuto::song::ItemKind::kWave, bank, wave};
            const std::string name = hokuto::song::DseItemName(set, item);
            if (!ReplaceWave(game, bank, wave, item, name, step, place, totals))
            {
                continue;
            }

            // At half the rate, each split that plays the wave moves its tuning by an octave, by the rate of its
            // table.
            const uint32_t rate = held.waves[wave]->SampleRate() / 2;
            if (rate < edit::kLeastDseRate)
            {
                continue;
            }
            HalveRate(game, bank, wave, item, name, rate, totals);
        }
    }
    std::fflush(stdout);
}

} // namespace hokuto::tools::dse_banks
