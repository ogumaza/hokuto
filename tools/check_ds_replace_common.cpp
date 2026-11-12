// SPDX-License-Identifier: MIT

// The helpers that the rounds of hokuto_check_ds_replace share (check_ds_replace.h).

#include "check_ds_replace.h"

namespace hokuto::tools::ds_replace
{

std::vector<std::vector<uint8_t>> WavesOf(const sdat::WaveArchive& parsed, std::span<const uint8_t> bytes)
{
    std::vector<uint32_t> ordered = parsed.offsets;
    std::ranges::sort(ordered);
    std::vector<std::vector<uint8_t>> waves;
    for (const uint32_t offset : parsed.offsets)
    {
        const auto next = std::ranges::upper_bound(ordered, offset);
        waves.emplace_back(bytes.begin() + offset, next == ordered.end() ? bytes.end() : bytes.begin() + *next);
    }

    return waves;
}

std::vector<uint32_t> SequenceSounds(const Game& game)
{
    const std::vector<std::optional<uint32_t>> sequences = hokuto::format::DsSoundSequences(game.file, game.bytes);
    std::vector<uint32_t> sounds;
    std::set<uint32_t> seen;
    for (uint32_t sound = 0; sound < sequences.size(); sound++)
    {
        if (sequences[sound] && seen.insert(*sequences[sound]).second)
        {
            sounds.push_back(sound);
        }
    }

    return sounds;
}

std::vector<std::pair<uint32_t, uint32_t>> BanksOf(const Game& game)
{
    std::vector<std::pair<uint32_t, uint32_t>> found;
    std::set<uint32_t> seen;
    const sdat::Archive& tables = game.archive->archive;
    for (const uint32_t bank : song::DsBanks(*game.archive))
    {
        const uint32_t id = tables.banks[bank]->file_id;
        if (id < tables.files.size() && seen.insert(id).second)
        {
            found.emplace_back(bank, id);
        }
    }

    return found;
}

std::vector<std::pair<uint32_t, uint32_t>> WaveArchivesOf(const Game& game)
{
    std::vector<std::pair<uint32_t, uint32_t>> found;
    std::set<uint32_t> seen;
    const sdat::Archive& tables = game.archive->archive;
    for (const uint32_t wave_archive : song::DsWaveArchives(*game.archive))
    {
        const uint32_t id = tables.wave_archives[wave_archive]->file_id & sdat::kFileIdBits;
        if (id < tables.files.size() && seen.insert(id).second)
        {
            found.emplace_back(wave_archive, id);
        }
    }

    return found;
}

void Fail(const Game& game, const std::string& round, const std::string& what, Totals& totals)
{
    std::printf("%s: %s: %s\n", game.name.c_str(), round.c_str(), what.c_str());
    totals.failed++;
}

bool SharesBytes(const sdat::Archive& tables, uint32_t id)
{
    const uint64_t start = tables.files[id].offset;
    const uint64_t end = start + tables.files[id].size;
    for (uint32_t other = 0; other < tables.files.size(); other++)
    {
        const uint64_t other_start = tables.files[other].offset;
        const uint64_t other_end = other_start + tables.files[other].size;
        if (other != id && other_end > other_start && other_start < end && start < other_end)
        {
            return true;
        }
    }

    return false;
}

std::optional<double> Ratio(std::span<const int16_t> samples, std::span<const int16_t> decoded)
{
    double signal = 0.0;
    double noise = 0.0;
    for (std::size_t i = 0; i < std::min(samples.size(), decoded.size()); i++)
    {
        const double difference = double(samples[i]) - double(decoded[i]);
        signal += double(samples[i]) * double(samples[i]);
        noise += difference * difference;
    }
    if (noise <= 0.0 || signal <= 0.0)
    {
        return std::nullopt;
    }

    return 10.0 * std::log10(signal / noise);
}

sar::WaveEncoding ToSar(sdat::WaveEncoding encoding)
{
    return encoding == sdat::WaveEncoding::kPcm8    ? sar::WaveEncoding::kPcm8
           : encoding == sdat::WaveEncoding::kPcm16 ? sar::WaveEncoding::kPcm16
                                                    : sar::WaveEncoding::kImaAdpcm;
}

} // namespace hokuto::tools::ds_replace
