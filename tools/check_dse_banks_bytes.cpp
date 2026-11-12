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
    const std::size_t wave_base = kBankHeader + kChunkHeader;
    const std::size_t wave_end = wave_base + Get32(file, kBankHeader + kChunkSize);
    for (uint32_t slot = 0; slot < Get16(file, kWaveCount); ++slot)
    {
        const uint16_t offset = Get16(file, wave_base + 2 * std::size_t{slot});
        if (offset != 0)
        {
            walk.waves[slot] = wave_base + offset;
        }
    }

    const std::size_t next = (wave_end + 15) / 16 * 16;
    if (std::memcmp(file.data() + next, "prgi", 4) != 0)
    {
        return walk;
    }

    const std::size_t base = next + kChunkHeader;
    for (uint32_t slot = 0; slot < Get16(file, kProgramCount); ++slot)
    {
        const uint16_t offset = Get16(file, base + 2 * std::size_t{slot});
        if (offset == 0)
        {
            continue;
        }

        const std::size_t program = base + offset;
        const std::size_t first = program + kProgramHeader + file[program + kProgramLfos] * kLfoSize + kLfoSize;
        for (std::size_t split = 0; split < Get16(file, program + kProgramSplits); ++split)
        {
            walk.splits.push_back({slot, split, first + split * kSplitSize});
        }
    }

    return walk;
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
