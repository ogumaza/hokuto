// SPDX-License-Identifier: MIT

// The walks of hokuto_check_dse_banks over the bytes of a bank file, and the checks of its forms.

#include "check_dse_banks.h"

namespace hokuto::tools::dse_banks
{

uint16_t Get16(std::span<const uint8_t> bytes, std::size_t at)
{
    return static_cast<uint16_t>(bytes[at] | bytes[at + 1] << 8);
}

uint32_t Get32(std::span<const uint8_t> bytes, std::size_t at)
{
    return bytes[at] | static_cast<uint32_t>(bytes[at + 1]) << 8 | static_cast<uint32_t>(bytes[at + 2]) << 16 |
           static_cast<uint32_t>(bytes[at + 3]) << 24;
}

void Put32(std::span<uint8_t> bytes, std::size_t at, uint32_t value)
{
    for (std::size_t i = 0; i < 4; ++i)
    {
        bytes[at + i] = static_cast<uint8_t>(value >> (8 * i));
    }
}

Walk WalkBank(std::span<const uint8_t> file)
{
    Walk walk;
    walk.oldest = Get16(file, kVersion) == kOldestVersion;
    if (walk.oldest)
    {
        walk.wave_places = kOldestWavePlaces;
    }
    const std::size_t wave_count = walk.oldest ? file[kWaveCount] : Get16(file, kWaveCount);
    const std::size_t program_count = walk.oldest ? file[kOldestProgramCount] : Get16(file, kProgramCount);
    const std::size_t wave_base = kBankHeader + kChunkHeader;
    const std::size_t wave_end = wave_base + Get32(file, kBankHeader + kChunkSize);
    for (uint32_t slot = 0; slot < wave_count; ++slot)
    {
        const uint16_t offset = Get16(file, wave_base + 2 * std::size_t{slot});
        if (offset != 0)
        {
            walk.waves[slot] = wave_base + offset;
        }
    }

    const std::size_t next = (wave_end + 15) / 16 * 16;
    if (next + 4 > file.size() || std::memcmp(file.data() + next, "prgi", 4) != 0)
    {
        return walk;
    }

    const std::size_t base = next + kChunkHeader;
    for (uint32_t slot = 0; slot < program_count; ++slot)
    {
        const uint16_t offset = Get16(file, base + 2 * std::size_t{slot});
        if (offset == 0)
        {
            continue;
        }

        // Version 0x415 keeps an empty LFO after the program's LFOs, and version 0x402 its map of keys before them.
        const std::size_t program = base + offset;
        const std::size_t lfos = file[program + kProgramLfos] * kLfoSize;
        const std::size_t first = walk.oldest ? program + kProgramHeader + kOldestEntriesOf8Bytes + lfos
                                              : program + kProgramHeader + lfos + kLfoSize;
        const std::size_t splits =
            walk.oldest ? file[program + kOldestProgramSplits] : Get16(file, program + kProgramSplits);
        for (std::size_t split = 0; split < splits; ++split)
        {
            walk.splits.push_back({slot, split, first + split * kSplitSize});
        }
    }

    return walk;
}

namespace
{

// The places of a split of version 0x402 that differ from version 0x415: each pair gives the place in a split as
// ntdlib's reader lays it out, and the place in the file. The first 16 bytes and the envelope keep their places.
constexpr std::array<std::pair<std::size_t, std::size_t>, 8> kOldestSplitPlaces{{
    {edit::kDseWave, 0x11},
    {edit::kDseTuning, 0x12},
    {edit::kDseTuning + 1, 0x13},
    {edit::kDseRootKey, 0x14},
    {edit::kDseCoarseTuning, 0x15},
    {edit::kDseVolume, 0x16},
    {edit::kDsePan, 0x17},
    {edit::kDseKeyGroup, 0x18},
}};
constexpr std::size_t kSplitStart = 0x10;
constexpr std::size_t kEnvelope = 0x20;
constexpr std::size_t kEnvelopeSize = 0x10;

} // namespace

DseSplit SplitIn(std::span<const uint8_t> file, const Walk& walk, std::size_t offset)
{
    DseSplit split{};
    const std::span<const uint8_t> bytes = file.subspan(offset, split.size());
    if (!walk.oldest)
    {
        std::ranges::copy(bytes, split.begin());
        return split;
    }

    std::copy_n(bytes.begin(), kSplitStart, split.begin());
    std::copy_n(bytes.begin() + kEnvelope, kEnvelopeSize, split.begin() + kEnvelope);
    for (const auto& [read, held] : kOldestSplitPlaces)
    {
        split[read] = bytes[held];
    }

    return split;
}

void PutSplit(std::span<uint8_t> file, const Walk& walk, std::size_t offset, const DseSplit& split)
{
    const std::span<uint8_t> bytes = file.subspan(offset, split.size());
    if (!walk.oldest)
    {
        std::ranges::copy(split, bytes.begin());
        return;
    }

    if (split[edit::kDseWave + 1] != 0)
    {
        throw std::runtime_error("a split of version 0x402 takes a wave past 255");
    }
    std::copy_n(split.begin(), kSplitStart, bytes.begin());
    std::copy_n(split.begin() + kEnvelope, kEnvelopeSize, bytes.begin() + kEnvelope);
    for (const auto& [read, held] : kOldestSplitPlaces)
    {
        bytes[held] = split[read];
    }
}

uint32_t RateIn(std::span<const uint8_t> info, const WavePlaces& places)
{
    return places.rate_bytes == 2 ? Get16(info, places.rate) : Get32(info, places.rate);
}

void PutRate(std::span<uint8_t> info, const WavePlaces& places, uint32_t rate)
{
    if (places.rate_bytes == 4)
    {
        Put32(info, places.rate, rate);
        return;
    }
    if (rate > UINT16_MAX)
    {
        throw std::runtime_error("a wave of version 0x402 takes a rate past 65,535 Hz");
    }
    info[places.rate] = static_cast<uint8_t>(rate);
    info[places.rate + 1] = static_cast<uint8_t>(rate >> 8);
}

std::vector<uint32_t> DistinctBanks(const DseSet& set)
{
    std::vector<uint32_t> banks;
    for (uint32_t bank = 0; bank < set.banks.size(); ++bank)
    {
        const auto same = [&](const hokuto::song::DseBank& other)
        {
            return other.bank == set.banks[bank].bank;
        };
        const bool first = std::ranges::none_of(std::span(set.banks).first(bank), same);
        if (first)
        {
            banks.push_back(bank);
        }
    }

    return banks;
}

void CheckForms(const Game& game, const File& file, const std::vector<uint8_t>& exported, const char* round,
                Totals& totals)
{
    if (hokuto::format::ExportArchive(hokuto::format::Read(hokuto::format::Write(file))) != exported)
    {
        Fail(game, totals, std::string(round) + ": the binary form gives another set");
    }
    if (hokuto::format::ExportArchive(hokuto::format::ReadText(hokuto::format::WriteText(file))) != exported)
    {
        Fail(game, totals, std::string(round) + ": the text form gives another set");
    }
}

void CheckExport(const Game& game, const std::vector<uint8_t>& exported, const std::vector<uint8_t>& expected,
                 const char* round, Totals& totals)
{
    if (exported.size() != expected.size())
    {
        Fail(game, totals, std::string(round) + ": the set's size changes");
        return;
    }

    const auto [one, other] = std::ranges::mismatch(exported, expected);
    if (one != exported.end())
    {
        const auto at = static_cast<std::size_t>(one - exported.begin());
        Fail(game, totals,
             std::string(round) + ": the byte at " + std::to_string(at) + " holds " + std::to_string(*one) +
                 " and should hold " + std::to_string(*other));
    }
}

uint32_t SamplesInWord(uint8_t format)
{
    return format == kPcm8 ? 4 : format == kPcm16 ? 2 : 8;
}

uint32_t FirstWord(uint8_t format)
{
    return format == kImaAdpcm ? 1 : 0;
}

} // namespace hokuto::tools::dse_banks
