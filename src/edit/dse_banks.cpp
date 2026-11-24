// SPDX-License-Identifier: MIT

#include "edit/dse_banks.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "ntdlib/dse/formats.h"
#include "ntdlib/sdat/files.h"
#include "ntdlib/sdat/formats.h"

#include "edit/dse_files_parts.h"
#include "edit/native_runs.h"
#include "format/archive.h"
#include "format/file.h"
#include "song/dse.h"

namespace hokuto::edit
{

namespace dse = ntdlib::dse;

namespace
{

using dse_files_parts::BankAt;
using dse_files_parts::BankFileBytes;
using dse_files_parts::HeldBankBytes;
using dse_files_parts::kImaAdpcm;
using dse_files_parts::kPcm16;
using dse_files_parts::kPcm8;
using dse_files_parts::kTuningOfDoubledRate;
using dse_files_parts::RetuneSplits;

const dse::WaveInfo& WaveAt(const song::DseSet& set, uint32_t bank, uint32_t wave)
{
    if (bank >= set.banks.size())
    {
        throw std::runtime_error("The image has no bank " + std::to_string(bank) + ".");
    }

    const dse::WaveBank& held = *set.banks[bank].bank;
    if (wave >= held.waves.size() || !held.waves[wave])
    {
        throw std::runtime_error(song::DseBankName(held) + " has no wave " + std::to_string(wave) + ".");
    }

    return *held.waves[wave];
}

// A value of a split that an edit of the Bank Editor can change: the byte's place, and its name in a message.
struct SplitByte
{
    std::size_t at;
    const char* name;
};

constexpr std::array<SplitByte, 14> kSplitBytes = {{
    {kDseBendRange, "Bend range"},
    {kDseVolume, "Volume"},
    {kDsePan, "Pan"},
    {kDseKeyGroup, "Key group"},
    {kDseMultiplier, "Envelope multiplier"},
    {kDseAttackLevel, "Attack level"},
    {kDseAttack, "Attack"},
    {kDseDecay, "Decay"},
    {kDseSustain, "Sustain"},
    {kDseHold, "Hold"},
    {kDseDecay2, "Second decay"},
    {kDseRelease, "Release"},
    {kDseLowKey, "Low key"},
    {kDseHighKey, "High key"},
}};

// Writes `model` into each bank file of the set that contains the bytes of `bank`, where its bytes change, into
// `changes`. Each file keeps its version (BankFileBytes). Throws std::invalid_argument for a bank whose size changes:
// the bytes of the set store its size.
void AddChanges(const song::DseSet& set, uint32_t bank, const dse::WaveBank& model, std::vector<ArchiveBytes>& changes)
{
    for (const song::DseBank& each : set.banks)
    {
        if (each.bank != set.banks[bank].bank)
        {
            continue;
        }

        const std::vector<uint8_t> from = HeldBankBytes(each);
        const std::vector<uint8_t> to = BankFileBytes(each, model);
        if (from.size() != to.size())
        {
            throw std::invalid_argument(
                "This edit would change the size of the bank. Hokuto doesn't support that yet.");
        }

        for (std::size_t at = 0; at < from.size();)
        {
            if (from[at] == to[at])
            {
                ++at;
                continue;
            }

            std::size_t end = at;
            while (end < from.size() && from[end] != to[end])
            {
                ++end;
            }

            changes.push_back(
                {each.file_offset + at, std::vector<uint8_t>(to.begin() + static_cast<std::ptrdiff_t>(at),
                                                             to.begin() + static_cast<std::ptrdiff_t>(end))});
            at = end;
        }
    }
}

// The bytes of `old`, a split of a DSE bank, with the values of `split` that the editor shows (WithDseSplit). Throws
// std::invalid_argument with a message for the user for a changed value out of its range.
DseSplit NewSplitBytes(const DseSplit& old, const DseSplit& split)
{
    // Only the values that the editor shows change. A value that stays isn't checked: a game's bank can hold others.
    DseSplit next = old;
    for (const SplitByte& value : kSplitBytes)
    {
        if (split[value.at] != old[value.at] && split[value.at] > kHighestDseByte)
        {
            throw std::invalid_argument(std::string(value.name) + " must be from 0 to " +
                                        std::to_string(kHighestDseByte) + ".");
        }

        next[value.at] = split[value.at];
    }

    if ((split[kDseLowVelocity] != old[kDseLowVelocity] && split[kDseLowVelocity] > kHighestDseByte) ||
        (split[kDseHighVelocity] != old[kDseHighVelocity] && split[kDseHighVelocity] > kHighestDseByte))
    {
        throw std::invalid_argument("Velocities must be from 0 to " + std::to_string(kHighestDseByte) + ".");
    }

    next[kDseLowVelocity] = split[kDseLowVelocity];
    next[kDseHighVelocity] = split[kDseHighVelocity];

    return next;
}

// Copies the changed key and velocity ranges of `next`, a split of a DSE bank, to the bytes after them (WithDseSplit).
// `old` is the split before the edit. Throws std::invalid_argument with a message for the user for a range whose low
// end is above its high end.
void CopyRanges(const DseSplit& old, DseSplit& next)
{
    const bool keys = next[kDseLowKey] != old[kDseLowKey] || next[kDseHighKey] != old[kDseHighKey];
    if (keys && next[kDseLowKey] > next[kDseHighKey])
    {
        throw std::invalid_argument("The low key of a split can't be above its high key.");
    }

    if (keys)
    {
        next[kDseLowKey + 2] = next[kDseLowKey];
        next[kDseHighKey + 2] = next[kDseHighKey];
    }

    const bool velocities =
        next[kDseLowVelocity] != old[kDseLowVelocity] || next[kDseHighVelocity] != old[kDseHighVelocity];
    if (velocities && next[kDseLowVelocity] > next[kDseHighVelocity])
    {
        throw std::invalid_argument("The low velocity of a split can't be above its high velocity.");
    }

    if (velocities)
    {
        next[kDseLowVelocity + 2] = next[kDseLowVelocity];
        next[kDseHighVelocity + 2] = next[kDseHighVelocity];
    }
}

// Sets the wave, the tuning and the root key of `split` in `next`, a split of a DSE bank in `model`, where they changed
// from `old` (WithDseSplit). Throws std::invalid_argument with a message for the user for a wave that the bank doesn't
// hold or a root key out of its range.
void SetWaveAndTuning(const DseSplit& old, const DseSplit& split, const dse::WaveBank& model, DseSplit& next)
{
    const int wave = DseSplitWave(split);
    if (wave != DseSplitWave(old))
    {
        if (static_cast<std::size_t>(wave) >= model.waves.size() || !model.waves[static_cast<std::size_t>(wave)])
        {
            throw std::invalid_argument(song::DseBankName(model) + " has no wave " + std::to_string(wave) + ".");
        }

        SetDseSplitWave(next, wave);
    }

    SetDseTuning(next, DseTuning(split));

    if (split[kDseCoarseTuning] != old[kDseCoarseTuning])
    {
        const int root = DseRootKey(split);
        if (root < 0 || root > kHighestDseByte)
        {
            throw std::invalid_argument("Root key must be from 0 to " + std::to_string(kHighestDseByte) + ".");
        }

        next[kDseCoarseTuning] = split[kDseCoarseTuning];
        const int moved = int{old[kDseRootKey]} + DseRootKey(split) - DseRootKey(old);
        next[kDseRootKey] = static_cast<uint8_t>(std::clamp(moved, 0, kHighestDseByte));
    }
}

void RequireSet(const format::File& file)
{
    if (!file.native || file.native->engine != format::Engine::kDse)
    {
        throw std::invalid_argument("The file contains no DSE files.");
    }
}

} // namespace

int DseRootKey(const DseSplit& split)
{
    return kDseMiddleKey - dse::SplitView(split).CoarseTuning();
}

void SetDseRootKey(DseSplit& split, int key)
{
    dse::MutableSplitView(split).SetCoarseTuning(static_cast<int8_t>(kDseMiddleKey - key));
}

int DseTuning(const DseSplit& split)
{
    return dse::SplitView(split).Tuning();
}

void SetDseTuning(DseSplit& split, int tuning)
{
    dse::MutableSplitView(split).SetTuning(static_cast<int16_t>(tuning));
}

int DseSplitWave(const DseSplit& split)
{
    return dse::SplitView(split).Wave();
}

void SetDseSplitWave(DseSplit& split, int wave)
{
    dse::MutableSplitView(split).SetWave(static_cast<uint16_t>(wave));
}

uint8_t DseWaveFormat(const dse::WaveInfo& info)
{
    return static_cast<uint8_t>(info.Format() >> 8);
}

void SetDseWaveFormat(dse::WaveInfo& info, uint8_t format)
{
    info.SetFormat(static_cast<uint16_t>(format << 8 | (info.Format() & 0xFF)));
}

format::File WithDseSplit(const format::File& file, const song::DseSet& set, const DseSplitPlace& place,
                          const DseSplit& split)
{
    RequireSet(file);

    return WithDseSplit(file, set, place, split, format::NativePieceStarts(file));
}

format::File WithDseSplit(const format::File& file, const song::DseSet& set, const DseSplitPlace& place,
                          const DseSplit& split, std::span<const uint64_t> starts)
{
    RequireSet(file);
    const song::DseBank& found = BankAt(set, place.bank);
    dse::WaveBank model = *found.bank;
    const std::string program_name = "program " + std::to_string(place.program);
    if (!model.instruments || place.program >= model.instruments->programs.size() ||
        !model.instruments->programs[place.program])
    {
        throw std::invalid_argument(song::DseBankName(model) + " has no " + program_name + ".");
    }

    dse::Program& program = *model.instruments->programs[place.program];
    if (place.split >= program.splits.size())
    {
        throw std::invalid_argument("Program " + std::to_string(place.program) + " has no split " +
                                    std::to_string(place.split) + ".");
    }

    const DseSplit old = program.splits[place.split];
    DseSplit next = NewSplitBytes(old, split);
    CopyRanges(old, next);
    SetWaveAndTuning(old, split, model, next);
    if (next == old)
    {
        return file;
    }

    program.splits[place.split] = next;
    std::vector<ArchiveBytes> changes;
    AddChanges(set, place.bank, model, changes);

    return WithArchiveBytes(file, starts, changes, "The bank");
}

DseWaveValues DseWaveValuesOf(const song::DseSet& set, uint32_t bank, uint32_t wave)
{
    const dse::WaveInfo& info = WaveAt(set, bank, wave);
    const auto encoding = static_cast<ntdlib::sdat::WaveEncoding>(DseWaveFormat(info));
    DseWaveValues values;
    values.rate = info.SampleRate();
    values.loop = info.Loops();
    values.loop_start = values.loop ? ntdlib::sdat::SamplesInWords(encoding, info.LoopStart()) : 0;

    return values;
}

namespace dse_files_parts
{

// Moves the tuning of each split of `model` that plays wave `wave` by `moved` 256ths of a semitone, for a new sample
// rate. Throws std::invalid_argument with a message for the user for a tuning out of range.
void RetuneSplits(dse::WaveBank& model, uint32_t wave, double moved)
{
    for (std::optional<dse::Program>& program : model.instruments->programs)
    {
        if (!program)
        {
            continue;
        }

        for (DseSplit& split : program->splits)
        {
            if (DseSplitWave(split) != static_cast<int>(wave))
            {
                continue;
            }

            const int64_t tuning = std::llround(DseTuning(split) + moved);
            if (tuning < std::numeric_limits<int16_t>::min() || tuning > std::numeric_limits<int16_t>::max())
            {
                throw std::invalid_argument("The new sample rate would push the tuning of a split in " +
                                            song::DseBankName(model) + " out of range.");
            }

            SetDseTuning(split, static_cast<int>(tuning));
        }
    }
}

} // namespace dse_files_parts

namespace
{

// The bank's table, and the table of each bank that reads its waves from it. A table can hold other values than the
// bank's for the wave, and each keeps those that the edit leaves.
std::vector<ArchiveBytes> RewriteWaveTables(const song::DseSet& set, uint32_t bank, uint32_t wave,
                                            const DseWaveValues& values, const DseWaveValues& old)
{
    std::vector<ArchiveBytes> changes;
    std::vector<std::shared_ptr<const dse::WaveBank>> done;
    for (std::size_t each = 0; each < set.banks.size(); ++each)
    {
        const song::DseBank& other = set.banks[each];
        if ((each != bank && other.bank != set.banks[bank].bank && other.main_bank != set.banks[bank].bank) ||
            std::ranges::find(done, other.bank) != done.end() || wave >= other.bank->waves.size() ||
            !other.bank->waves[wave])
        {
            continue;
        }

        // A bank that shares the main bank can hold a square wave or noise in the slot. Those have no samples, and the
        // edit leaves them as they are.
        if (DseWaveFormat(*other.bank->waves[wave]) > kImaAdpcm)
        {
            continue;
        }

        done.push_back(other.bank);
        dse::WaveBank model = *other.bank;
        dse::WaveInfo& held = *model.waves[wave];
        const auto held_encoding = static_cast<ntdlib::sdat::WaveEncoding>(DseWaveFormat(held));
        if (values.rate != old.rate)
        {
            const uint32_t was = held.SampleRate();
            held.SetSampleRate(values.rate);
            if (was != 0 && model.instruments)
            {
                RetuneSplits(model, wave, kTuningOfDoubledRate * std::log2(static_cast<double>(values.rate) / was));
            }
        }

        if (values.loop != old.loop || values.loop_start != old.loop_start)
        {
            const uint32_t held_end = held.LoopStart() + held.LoopLength();
            const uint32_t first = ntdlib::sdat::HeaderWords(held_encoding);
            const uint32_t word =
                values.loop ? values.loop_start / ntdlib::sdat::SamplesPerWord(held_encoding) + first : first;
            if (word >= held_end)
            {
                throw std::invalid_argument("The loop must start before the end of the wave in " +
                                            song::DseBankName(model) + ".");
            }

            held.SetLoops(values.loop);
            held.SetLoopStart(word);
            held.SetLoopLength(held_end - word);
        }

        AddChanges(set, static_cast<uint32_t>(each), model, changes);
    }

    return changes;
}

} // namespace

format::File WithDseWave(const format::File& file, const song::DseSet& set, uint32_t bank, uint32_t wave,
                         const DseWaveValues& values)
{
    RequireSet(file);

    return WithDseWave(file, set, bank, wave, values, format::NativePieceStarts(file));
}

format::File WithDseWave(const format::File& file, const song::DseSet& set, uint32_t bank, uint32_t wave,
                         const DseWaveValues& values, std::span<const uint64_t> starts)
{
    RequireSet(file);
    const dse::WaveInfo& info = WaveAt(set, bank, wave);
    const DseWaveValues old = DseWaveValuesOf(set, bank, wave);
    if (values == old)
    {
        return file;
    }

    const uint8_t format = DseWaveFormat(info);
    if (format != kPcm8 && format != kPcm16 && format != kImaAdpcm)
    {
        throw std::invalid_argument("Square waves and noise have no sample rate or loop.");
    }

    if (values.rate != old.rate)
    {
        CheckDsRate(values.rate, "DSE");
    }

    const uint32_t end = info.LoopStart() + info.LoopLength();
    if (values.loop)
    {
        const auto encoding = static_cast<ntdlib::sdat::WaveEncoding>(format);
        const uint32_t step = ntdlib::sdat::SamplesPerWord(encoding);
        if (values.loop_start % step != 0)
        {
            throw std::invalid_argument("The loop start of this wave must be a multiple of " + std::to_string(step) +
                                        " samples.");
        }
        if (values.loop_start >= ntdlib::sdat::SamplesInWords(encoding, end))
        {
            throw std::invalid_argument("The loop must start before the end of the wave.");
        }
    }

    return WithArchiveBytes(file, starts, RewriteWaveTables(set, bank, wave, values, old), "The bank");
}

} // namespace hokuto::edit
