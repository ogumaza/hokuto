// SPDX-License-Identifier: MIT

// hokuto_check_ds_banks: the rounds that edit the values of the instruments, and the round that edits the waves.

#include "check_ds_banks.h"
#include "checked_saves.h"

namespace hokuto::tools::ds_banks
{

std::map<std::size_t, sdat::Region> Envelopes(const Game& game, Totals& totals)
{
    std::map<std::size_t, sdat::Region> edits;
    for (const auto& [at, place] : game.places)
    {
        sdat::Region region = place.region;
        region.attack = Moved(region.attack, 3, kHighest, totals.kept);
        region.decay = Moved(region.decay, 5, kHighest, totals.kept);
        region.sustain = Moved(region.sustain, 7, kHighest, totals.kept);
        region.release = Moved(region.release, 2, kHighest, totals.kept);
        edits.emplace(at, region);
    }

    return edits;
}

std::map<std::size_t, sdat::Region> KeysAndPans(const Game& game, Totals& totals)
{
    std::map<std::size_t, sdat::Region> edits;
    for (const auto& [at, place] : game.places)
    {
        sdat::Region region = place.region;
        region.root_key = Moved(region.root_key, 5, kHighest, totals.kept);
        region.pan = Moved(region.pan, 17, kHighest, totals.kept);
        edits.emplace(at, region);
    }

    return edits;
}

std::map<std::size_t, sdat::Region> WavesAndDuties(const Game& game, Totals& totals)
{
    std::map<std::size_t, sdat::Region> edits;
    for (const auto& [at, place] : game.places)
    {
        sdat::Region region = place.region;
        if (region.type == kPcm && !place.waves.empty())
        {
            const auto found =
                std::ranges::find(place.waves, hokuto::app::DsWaveChoice{region.wave_archive, region.wave});
            const hokuto::app::DsWaveChoice next =
                found == place.waves.end() || found + 1 == place.waves.end() ? place.waves.front() : *(found + 1);
            region.wave_archive = next.slot;
            region.wave = next.wave;
        }
        else if (region.type == kPsg)
        {
            region.wave = Moved(region.wave, 1, kHighestDuty, totals.kept);
        }
        else
        {
            continue;
        }
        edits.emplace(at, region);
    }

    return edits;
}

std::map<std::size_t, sdat::Region> Kinds(const Game& game, std::set<std::size_t>& refusing)
{
    std::map<std::size_t, sdat::Region> edits;
    for (const auto& [at, place] : game.places)
    {
        sdat::Region region = place.region;
        constexpr uint16_t kDuty = 3;
        if (region.type == kPcm)
        {
            region.type = kPsg;
            region.wave = kDuty;
        }
        else if (region.type == kPsg)
        {
            region.type = kNoise;
        }
        else if (region.type == kNoise && !place.waves.empty())
        {
            region.type = kPcm;
            region.wave_archive = place.waves.front().slot;
            region.wave = place.waves.front().wave;
        }
        else
        {
            region.type = kPsg;
            region.wave = kDuty;
        }
        const bool playable = place.region.type == kPcm || place.region.type == kPsg || place.region.type == kNoise;
        if (place.shared || !playable)
        {
            refusing.insert(at);
        }
        edits.emplace(at, region);
    }

    return edits;
}

namespace
{

// The 10 bytes of `region`'s values, at `at` of `bytes`.
void PutValues(std::vector<uint8_t>& bytes, std::size_t at, const sdat::Region& region)
{
    const uint8_t values[] = {static_cast<uint8_t>(region.wave & 0xFF),
                              static_cast<uint8_t>(region.wave >> 8),
                              static_cast<uint8_t>(region.wave_archive & 0xFF),
                              static_cast<uint8_t>(region.wave_archive >> 8),
                              region.root_key,
                              region.attack,
                              region.decay,
                              region.sustain,
                              region.release,
                              region.pan};
    std::ranges::copy(values, bytes.begin() + static_cast<std::ptrdiff_t>(at));
}

} // namespace

void CheckForms(const Game& game, const File& edited, const std::vector<uint8_t>& exported, const char* round,
                Totals& totals)
{
    if (hokuto::format::ExportArchive(hokuto::format::Read(hokuto::format::Write(edited))) != exported ||
        hokuto::format::ExportArchive(hokuto::format::ReadText(hokuto::format::WriteText(edited))) != exported)
    {
        std::printf("%s: %s: a saved file gives another archive\n", game.name.c_str(), round);
        totals.failed++;
    }
}

std::shared_ptr<const DsArchive> Round(const Game& game, const char* round, std::map<std::size_t, sdat::Region>& edits,
                                       const std::set<std::size_t>& refusing, Totals& totals)
{
    File edited = game.file;
    for (auto it = edits.begin(); it != edits.end();)
    {
        const Place& place = game.places.at(it->first);
        try
        {
            edited = hokuto::edit::WithDsRegion(edited, *game.archive, place.place, it->second, game.starts);
            if (refusing.contains(it->first))
            {
                std::printf("%s: %s: %s took a kind that the edit should refuse\n", game.name.c_str(), round,
                            place.name.c_str());
                totals.failed++;
            }
            totals.edits++;
            ++it;
        }
        catch (const std::invalid_argument& error)
        {
            if (refusing.contains(it->first))
            {
                totals.kinds_kept++;
            }
            else
            {
                std::printf("%s: %s: %s refused its edit: %s\n", game.name.c_str(), round, place.name.c_str(),
                            error.what());
                totals.refused++;
                totals.failed++;
            }
            it = edits.erase(it);
        }
    }

    // The bytes that the archive should hold.
    std::vector<uint8_t> expected = game.bytes;
    for (const auto& [at, region] : edits)
    {
        const Place& place = game.places.at(at);
        PutValues(expected, place.values, region);
        if (region.type != place.region.type)
        {
            expected[place.kind] = static_cast<uint8_t>(region.type);
        }
    }
    const std::vector<uint8_t> exported = hokuto::format::ExportArchive(edited);
    if (exported != expected)
    {
        const std::size_t at = FirstDifferingIndex(exported, expected);
        std::printf(
            "%s: %s: the archive holds other bytes than the edits give, first at 0x%zX (%zu bytes, %zu "
            "expected)\n",
            game.name.c_str(), round, at, exported.size(), expected.size());
        totals.failed++;
        return nullptr;
    }

    // ntdlib reads the new regions, and the Bank Editor shows the new archive's banks.
    const std::shared_ptr<const DsArchive> after = hokuto::song::LoadDsArchive(exported, game.archive->build);
    for (const auto& [at, region] : edits)
    {
        const Place& place = game.places.at(at);
        if (RegionAt(*after, place.place) != region)
        {
            std::printf("%s: %s: ntdlib reads %s otherwise\n", game.name.c_str(), round, place.name.c_str());
            totals.failed++;
        }
    }
    for (const uint32_t bank : game.banks)
    {
        try
        {
            hokuto::app::MakeDsBankView(*after, bank);
        }
        catch (const std::exception& error)
        {
            std::printf("%s: %s: %s can't be shown: %s\n", game.name.c_str(), round,
                        hokuto::song::DsBankName(*after, bank).c_str(), error.what());
            totals.failed++;
        }
    }

    // Both forms of the file give the same archive.
    CheckForms(game, edited, exported, round, totals);

    return after;
}

namespace
{

// A wave that Waves edits, with its new values and its samples before the edit.
struct Edited
{
    uint32_t wave_archive = 0;
    uint32_t wave = 0;
    hokuto::edit::DsWaveValues values;
    std::vector<int16_t> samples;
};

// Sets a new rate and loop for each wave of the game's wave archives in `edited`, and returns the edits. `expected`
// receives the bytes that each edit writes to the archive.
std::vector<Edited> EditWaves(const Game& game, File& edited, std::vector<uint8_t>& expected, Totals& totals)
{
    constexpr uint64_t kMostStart = 0xFFFF;
    const sdat::Archive& tables = game.archive->archive;
    std::vector<Edited> edits;
    std::set<uint32_t> files;
    for (uint32_t wave_archive = 0; wave_archive < tables.wave_archives.size(); ++wave_archive)
    {
        if (!tables.wave_archives[wave_archive])
        {
            continue;
        }
        const uint32_t file_id = tables.wave_archives[wave_archive]->file_id & sdat::kFileIdBits;
        if (file_id >= tables.files.size() || !files.insert(file_id).second)
        {
            continue;
        }
        sdat::WaveArchive parsed;
        try
        {
            parsed = sdat::WaveArchive::Parse(tables.File(file_id));
        }
        catch (const std::exception&)
        {
            totals.unread_waves++;
            continue;
        }
        totals.wave_files++;
        for (uint32_t wave = 0; wave < parsed.waves.size(); ++wave)
        {
            const sdat::Wave& present = parsed.waves[wave];
            const uint32_t header = present.encoding == sdat::WaveEncoding::kImaAdpcm ? 1 : 0;
            const uint64_t words = uint64_t{present.loop_start} + present.loop_length;
            std::vector<int16_t> samples;
            try
            {
                samples = sdat::DecodeWave(present);
            }
            catch (const std::exception&)
            {
                totals.waves_apart++;
                continue;
            }
            if (words < header + 2)
            {
                totals.waves_apart++;
                continue;
            }

            hokuto::edit::DsWaveValues values;
            values.rate = std::max<uint32_t>(hokuto::edit::kLeastDsRate, uint32_t{present.sample_rate} * 3 / 4);
            values.loop = true;
            const auto start = static_cast<uint32_t>(std::min(header + (words - header) / 2, kMostStart));
            values.loop_start = (start - header) * sdat::SamplesPerWord(present.encoding);
            try
            {
                edited = hokuto::edit::WithDsWave(edited, *game.archive, wave_archive, wave, values, game.starts);
            }
            catch (const std::exception& error)
            {
                std::printf("%s: waves: %s, wave %u is refused: %s\n", game.name.c_str(),
                            hokuto::song::DsWaveArchiveName(*game.archive, wave_archive).c_str(), wave, error.what());
                totals.failed++;
                continue;
            }
            totals.wave_edits++;

            // The bytes of the parameters after the bytes of the encoding.
            const std::size_t at = tables.files[file_id].offset + parsed.offsets[wave];
            const uint32_t timer =
                values.rate == hokuto::song::DsWaveRate(present) ? present.timer : sdat::kTimerClockHz / values.rate;
            const auto length = static_cast<uint32_t>(words - start);
            const std::array<uint32_t, 11> bytes = {
                1,          values.rate & 0xFF, values.rate >> 8,     timer & 0xFF,          timer >> 8,  start & 0xFF,
                start >> 8, length & 0xFF,      (length >> 8) & 0xFF, (length >> 16) & 0xFF, length >> 24};
            for (std::size_t i = 0; i < bytes.size(); ++i)
            {
                expected[at + 1 + i] = static_cast<uint8_t>(bytes[i]);
            }
            edits.push_back({wave_archive, wave, values, std::move(samples)});
        }
    }

    return edits;
}

// Checks that ntdlib reads the new values of each wave of `edits` from `after`, and that each wave decodes to its
// samples before the edit.
void CheckEditedWaves(const Game& game, const DsArchive& after, std::span<const Edited> edits, Totals& totals)
{
    for (const Edited& each : edits)
    {
        const std::string name =
            hokuto::song::DsWaveArchiveName(after, each.wave_archive) + ", wave " + std::to_string(each.wave);
        try
        {
            const sdat::WaveArchive parsed = sdat::WaveArchive::Parse(
                after.archive.File(after.archive.wave_archives[each.wave_archive]->file_id & sdat::kFileIdBits));
            if (hokuto::edit::DsWaveValuesOf(after, each.wave_archive, each.wave) != each.values)
            {
                std::printf("%s: waves: ntdlib reads %s otherwise\n", game.name.c_str(), name.c_str());
                totals.failed++;
            }
            else if (sdat::DecodeWave(parsed.waves[each.wave]) != each.samples)
            {
                std::printf("%s: waves: %s decodes otherwise\n", game.name.c_str(), name.c_str());
                totals.failed++;
            }
        }
        catch (const std::exception& error)
        {
            std::printf("%s: waves: %s can't be read: %s\n", game.name.c_str(), name.c_str(), error.what());
            totals.failed++;
        }
    }
}

} // namespace

void Waves(const Game& game, Totals& totals)
{
    File edited = game.file;
    std::vector<uint8_t> expected = game.bytes;
    const std::vector<Edited> edits = EditWaves(game, edited, expected, totals);
    if (edits.empty())
    {
        return;
    }

    const std::vector<uint8_t> exported = hokuto::format::ExportArchive(edited);
    if (exported != expected)
    {
        const std::size_t at = FirstDifferingIndex(exported, expected);
        std::printf("%s: waves: the archive holds other bytes than the edits give, first at 0x%zX\n", game.name.c_str(),
                    at);
        totals.failed++;
        return;
    }

    // ntdlib reads the new values, and each wave decodes to its samples before the edit.
    const std::shared_ptr<const DsArchive> after = hokuto::song::LoadDsArchive(exported, game.archive->build);
    CheckEditedWaves(game, *after, edits, totals);

    // Both forms of the file give the same archive.
    CheckForms(game, edited, exported, "waves", totals);
}

} // namespace hokuto::tools::ds_banks
