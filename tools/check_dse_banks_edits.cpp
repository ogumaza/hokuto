// SPDX-License-Identifier: MIT

// The rounds of hokuto_check_dse_banks that edit the values, the keys and the waves of the programs.

#include "check_dse_banks.h"

namespace hokuto::tools::dse_banks
{

DseSplit Envelopes(const DseSplit& old, const std::vector<uint32_t>&)
{
    DseSplit split = old;
    for (const std::size_t at : {edit::kDseAttackLevel, edit::kDseAttack, edit::kDseDecay, edit::kDseSustain,
                                 edit::kDseHold, edit::kDseDecay2, edit::kDseRelease})
    {
        if (split[at] <= kHighest)
        {
            split[at] = static_cast<uint8_t>((split[at] + 13) % (kHighest + 1));
        }
    }

    return split;
}

DseSplit Values(const DseSplit& old, const std::vector<uint32_t>&)
{
    DseSplit split = old;
    const auto move = [&split](std::size_t at, int by)
    {
        if (split[at] <= kHighest)
        {
            split[at] = static_cast<uint8_t>((split[at] + by) % (kHighest + 1));
        }
    };
    move(edit::kDseVolume, 5);
    move(edit::kDsePan, 17);
    move(edit::kDseKeyGroup, 1);
    move(edit::kDseBendRange, 1);
    if (split[edit::kDseMultiplier] <= kHighest)
    {
        split[edit::kDseMultiplier] = static_cast<uint8_t>(split[edit::kDseMultiplier] % 5 + 1);
    }
    const auto tuning = static_cast<uint16_t>(split[edit::kDseTuning] | split[edit::kDseTuning + 1] << 8);
    const auto moved = static_cast<int16_t>(tuning);
    if (moved <= INT16_MAX - 100)
    {
        const auto bits = static_cast<uint16_t>(moved + 100);
        split[edit::kDseTuning] = static_cast<uint8_t>(bits);
        split[edit::kDseTuning + 1] = static_cast<uint8_t>(bits >> 8);
    }

    return split;
}

namespace
{

// A range of bytes at `low` and `high`, with their copies 2 bytes on: one step narrower at each end where it has room.
void Narrow(DseSplit& split, std::size_t low, std::size_t high)
{
    if (split[high] > kHighest || split[low] >= split[high])
    {
        return;
    }

    split[low] = static_cast<uint8_t>(split[low] + 1);
    split[low + 2] = split[low];
    if (split[low] < split[high])
    {
        split[high] = static_cast<uint8_t>(split[high] - 1);
    }
    split[high + 2] = split[high];
}

} // namespace

DseSplit Keys(const DseSplit& old, const std::vector<uint32_t>&)
{
    DseSplit split = old;
    Narrow(split, edit::kDseLowKey, edit::kDseHighKey);
    Narrow(split, edit::kDseLowVelocity, edit::kDseHighVelocity);

    // The driver reads the coarse tuning. The tools write the root key beside it, and the byte moves with it up to 127.
    const int root = 60 - static_cast<int8_t>(split[edit::kDseCoarseTuning]);
    if (root >= 0 && root <= kHighest)
    {
        const int next = (root + 5) % (kHighest + 1);
        split[edit::kDseCoarseTuning] = static_cast<uint8_t>(static_cast<int8_t>(60 - next));
        split[edit::kDseRootKey] =
            static_cast<uint8_t>(std::clamp(int{split[edit::kDseRootKey]} + next - root, 0, kHighest));
    }

    return split;
}

DseSplit Waves(const DseSplit& old, const std::vector<uint32_t>& waves)
{
    DseSplit split = old;
    const auto found = std::ranges::find(waves, static_cast<uint32_t>(Get16(split, edit::kDseWave)));
    if (waves.size() > 1 && found != waves.end())
    {
        const uint32_t next = found + 1 == waves.end() ? waves.front() : *(found + 1);
        split[edit::kDseWave] = static_cast<uint8_t>(next);
        split[edit::kDseWave + 1] = static_cast<uint8_t>(next >> 8);
    }

    return split;
}

namespace
{

// `split` with the bytes that the edit has to write itself as they were: the copies of the keys and velocities and the
// byte of the root key.
DseSplit Given(const DseSplit& split, const DseSplit& old)
{
    DseSplit given = split;
    for (const std::size_t at : {edit::kDseLowKey + 2, edit::kDseHighKey + 2, edit::kDseLowVelocity + 2,
                                 edit::kDseHighVelocity + 2, edit::kDseRootKey})
    {
        given[at] = old[at];
    }

    return given;
}

} // namespace

void Round(const Game& game, const char* name, const Change& change, Totals& totals)
{
    const DseSet& set = *game.set;
    File edited = game.file;
    std::size_t edits = 0;
    for (const uint32_t bank : DistinctBanks(set))
    {
        const dse::WaveBank& held = *set.banks[bank].bank;
        if (!held.instruments)
        {
            continue;
        }

        const std::vector<uint32_t> waves = hokuto::song::DseBankWaves(held);
        for (uint32_t program = 0; program < held.instruments->programs.size(); ++program)
        {
            if (!held.instruments->programs[program])
            {
                continue;
            }

            const std::vector<DseSplit>& splits = held.instruments->programs[program]->splits;
            for (std::size_t split = 0; split < splits.size(); ++split)
            {
                const DseSplit next = change(splits[split], waves);
                if (next == splits[split])
                {
                    continue;
                }

                edited =
                    edit::WithDseSplit(edited, set, {bank, program, split}, Given(next, splits[split]), game.starts);
                edits++;
            }
        }
    }
    totals.split_edits += edits;

    std::vector<uint8_t> expected = game.bytes;
    for (uint32_t bank = 0; bank < set.banks.size(); ++bank)
    {
        const std::vector<uint32_t> waves = hokuto::song::DseBankWaves(*set.banks[bank].bank);
        const auto at = static_cast<std::size_t>(set.banks[bank].file_offset);
        const std::span<const uint8_t> file = std::span(game.bytes).subspan(at);
        for (const SplitAt& place : WalkBank(file).splits)
        {
            DseSplit old;
            std::copy_n(file.begin() + static_cast<std::ptrdiff_t>(place.offset), old.size(), old.begin());
            const DseSplit next = change(old, waves);
            std::ranges::copy(next, expected.begin() + static_cast<std::ptrdiff_t>(at + place.offset));
        }
    }

    const std::vector<uint8_t> exported = hokuto::format::ExportArchive(edited);
    CheckExport(game, exported, expected, name, totals);

    const std::shared_ptr<const DseSet> read = hokuto::song::LoadDseSet(exported, game.build);
    if (read->banks.size() != set.banks.size())
    {
        Fail(game, totals, std::string(name) + ": the reader reads another count of banks");
        return;
    }
    for (const uint32_t bank : DistinctBanks(*read))
    {
        const dse::WaveBank& held = *read->banks[bank].bank;
        if (!held.instruments)
        {
            continue;
        }

        const auto at = static_cast<std::size_t>(read->banks[bank].file_offset);
        const std::span<const uint8_t> file = std::span(expected).subspan(at);
        const hokuto::app::DseBankView view = hokuto::app::MakeDseBankView(*read, bank);
        for (const SplitAt& place : WalkBank(file).splits)
        {
            DseSplit want;
            std::copy_n(file.begin() + static_cast<std::ptrdiff_t>(place.offset), want.size(), want.begin());
            if (held.instruments->programs.at(place.program)->splits.at(place.split) != want)
            {
                Fail(game, totals,
                     std::string(name) + ": the reader reads other bytes in program " + std::to_string(place.program) +
                         " of " + hokuto::song::DseBankName(held));
            }
            const auto shown = std::ranges::find_if(
                view.programs, [&](const hokuto::app::DseProgramView& one) { return one.program == place.program; });
            if (shown == view.programs.end() || shown->rows.at(place.split).values != want)
            {
                Fail(game, totals,
                     std::string(name) + ": the view shows other bytes in program " + std::to_string(place.program) +
                         " of " + hokuto::song::DseBankName(held));
            }
        }
    }
    CheckForms(game, edited, exported, name, totals);
    std::printf("%s: %s: %zu edits\n", game.name.c_str(), name, edits);
    std::fflush(stdout);
}

namespace
{

// `expected`, the bytes of the set, with `values` for wave `wave` of the bank whose file is `bank`, by the format's
// rules. Each table gets the rate and the loop, and each split of the wave in that file moves its tuning.
void WriteWave(const Game& game, uint32_t main_bank, uint32_t wave, const edit::DseWaveValues& values,
               const edit::DseWaveValues& old, std::vector<uint8_t>& expected)
{
    const DseSet& set = *game.set;
    for (uint32_t each = 0; each < set.banks.size(); ++each)
    {
        const hokuto::song::DseBank& bank = set.banks[each];
        if (bank.bank != set.banks[main_bank].bank && bank.main_bank != set.banks[main_bank].bank)
        {
            continue;
        }

        const auto at = static_cast<std::size_t>(bank.file_offset);
        const std::span<const uint8_t> file = std::span(game.bytes).subspan(at);
        const Walk walk = WalkBank(file);
        const auto found = walk.waves.find(wave);
        if (found == walk.waves.end())
        {
            continue;
        }

        const std::span<const uint8_t> info = file.subspan(found->second);
        const std::span<uint8_t> out = std::span(expected).subspan(at + found->second);
        const uint32_t rate = Get32(info, kWaveRate);
        if (values.rate != old.rate)
        {
            Put32(out, kWaveRate, values.rate);
            for (const SplitAt& place : walk.splits)
            {
                if (rate == 0 || Get16(file, place.offset + edit::kDseWave) != wave)
                {
                    continue;
                }

                const auto tuning = static_cast<int16_t>(Get16(file, place.offset + edit::kDseTuning));
                const int64_t moved =
                    std::llround(tuning + kTuningOfDoubledRate * std::log2(static_cast<double>(values.rate) / rate));
                if (!std::in_range<int16_t>(moved))
                {
                    throw std::runtime_error("wave " + std::to_string(wave) +
                                             " takes a rate that moves a tuning past its 16 bits, and the edit doesn't "
                                             "refuse it");
                }
                const auto bits = static_cast<uint16_t>(moved);
                expected[at + place.offset + edit::kDseTuning] = static_cast<uint8_t>(bits);
                expected[at + place.offset + edit::kDseTuning + 1] = static_cast<uint8_t>(bits >> 8);
            }
        }
        if (values.loop != old.loop || values.loop_start != old.loop_start)
        {
            const uint8_t format = info[kWaveFormat];
            const uint32_t end = Get32(info, kWaveLoopStart) + Get32(info, kWaveLoopLength);
            const uint32_t word = values.loop_start / SamplesInWord(format) + FirstWord(format);
            out[kWaveLoops] = 1;
            Put32(out, kWaveLoopStart, word);
            Put32(out, kWaveLoopLength, end - word);
        }
    }
}

} // namespace

void WaveRound(const Game& game, Totals& totals)
{
    const DseSet& set = *game.set;
    File edited = game.file;
    std::vector<uint8_t> expected = game.bytes;
    std::vector<std::pair<uint32_t, uint32_t>> edited_waves;
    for (const uint32_t bank : DistinctBanks(set))
    {
        const dse::WaveBank& held = *set.banks[bank].bank;
        if (!held.samples)
        {
            continue;
        }

        for (const uint32_t wave : hokuto::song::DseBankWaves(held))
        {
            totals.waves++;
            const edit::DseWaveValues old = edit::DseWaveValuesOf(set, bank, wave);
            const uint8_t format = held.waves[wave]->bytes[kWaveFormat];
            edit::DseWaveValues values = old;
            values.rate = std::max(old.rate / 4 * 3, edit::kLeastDseRate);
            if (format != kPcm8 && format != kPcm16 && format != kImaAdpcm)
            {
                try
                {
                    (void)edit::WithDseWave(game.file, set, bank, wave, values, game.starts);
                    Fail(game, totals, "a square wave or noise takes a new rate");
                }
                catch (const std::invalid_argument&)
                {
                    totals.waves_refused++;
                }
                continue;
            }

            const uint32_t end = held.waves[wave]->LoopStart() + held.waves[wave]->LoopLength();
            const uint32_t first = FirstWord(format);
            const uint32_t words = end > first ? end - first : 0;
            if (words < 2)
            {
                totals.waves_apart++;
                continue;
            }
            values.loop = true;
            values.loop_start = words / 2 * SamplesInWord(format);

            try
            {
                edited = edit::WithDseWave(edited, set, bank, wave, values, game.starts);
            }
            catch (const std::invalid_argument& error)
            {
                std::printf("%s: wave %u of %s: %s\n", game.name.c_str(), wave, hokuto::song::DseBankName(held).c_str(),
                            error.what());
                totals.edits_refused++;
                continue;
            }
            totals.wave_edits++;
            WriteWave(game, bank, wave, values, old, expected);
            edited_waves.emplace_back(bank, wave);
        }
    }

    const std::vector<uint8_t> exported = hokuto::format::ExportArchive(edited);
    CheckExport(game, exported, expected, "waves", totals);
    const std::shared_ptr<const DseSet> read = hokuto::song::LoadDseSet(exported, game.build);
    for (const auto& [bank, wave] : edited_waves)
    {
        const edit::DseWaveValues values = edit::DseWaveValuesOf(*read, bank, wave);
        const edit::DseWaveValues old = edit::DseWaveValuesOf(set, bank, wave);
        if (!values.loop || values.rate != std::max(old.rate / 4 * 3, edit::kLeastDseRate))
        {
            Fail(game, totals, "wave " + std::to_string(wave) + " reads other values");
        }
        const ntdlib::wave::Pcm before = hokuto::song::DseWavePcm(set, bank, wave);
        const ntdlib::wave::Pcm after = hokuto::song::DseWavePcm(*read, bank, wave);
        if (before.channels != after.channels)
        {
            Fail(game, totals, "wave " + std::to_string(wave) + " decodes to other samples");
        }
        (void)hokuto::app::MakeDseWaveView(*read, bank, wave);
    }
    CheckForms(game, edited, exported, "waves", totals);
    std::printf("%s: waves: %zu edits\n", game.name.c_str(), edited_waves.size());
    std::fflush(stdout);
}

} // namespace hokuto::tools::dse_banks
