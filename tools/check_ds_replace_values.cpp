// SPDX-License-Identifier: MIT

// The round Sound values of hokuto_check_ds_replace.

#include "check_ds_replace.h"

namespace hokuto::tools::ds_replace
{

namespace
{

// The index after `index` of a record that `table` contains, round the table's end.
template <class Table>
uint32_t NextIn(const Table& table, uint32_t index)
{
    for (std::size_t step = 1; step <= table.size(); step++)
    {
        const std::size_t next = (index + step) % table.size();
        if (table[next])
        {
            return static_cast<uint32_t>(next);
        }
    }

    return index;
}

// The level of `samples` in dBFS.
double LevelOf(std::span<const float> samples)
{
    double sum = 0.0;
    for (const float sample : samples)
    {
        sum += static_cast<double>(sample) * sample;
    }

    return 10.0 * std::log10(std::max(sum / static_cast<double>(std::max<std::size_t>(samples.size(), 1)), 1e-20));
}

// The values of each sound of `archive` (edit::DsSoundValuesOf), by the sound's number. A sound of a sequence archive
// uses the values of its record in the archive file. Each sequence archive's file is read once. A sound whose record
// ntdlib can't read gets nothing.
std::vector<std::optional<hokuto::edit::DsSoundValues>> ValuesOf(const song::DsArchive& archive)
{
    const sdat::Archive& tables = archive.archive;
    std::map<uint32_t, std::vector<sdat::ArchivedSequence>> records; // by the sequence archive's index
    std::vector<std::optional<hokuto::edit::DsSoundValues>> values(archive.sounds.size());
    for (const uint32_t sound : song::DsSounds(archive))
    {
        try
        {
            const ntdlib::ntr::SequenceRef& ref = archive.sounds.at(sound).value();
            if (!ref.archived)
            {
                values[sound] = hokuto::edit::DsSoundValuesOf(archive, sound);
                continue;
            }

            if (!records.contains(ref.index))
            {
                const uint32_t file = tables.sequence_archives.at(ref.index).value().file_id;
                records[ref.index] = sdat::SequenceArchive::Parse(tables.File(file)).sequences;
            }
            const sdat::ArchivedSequence& record = records[ref.index].at(*ref.archived);
            values[sound] = hokuto::edit::DsSoundValues{record.bank, record.volume, record.channel_priority,
                                                        record.player_priority, record.player};
        }
        catch (const std::exception&)
        {
            continue;
        }
    }

    return values;
}

// Checks that each sound of `after`, the archive of `edited`, but those of `expected` keeps its values, and that a
// saved `edited` exports `exported` again.
void CheckOtherSoundsKept(const Game& game, const song::DsArchive& after, const File& edited,
                          const std::vector<uint8_t>& exported,
                          const std::map<uint32_t, hokuto::edit::DsSoundValues>& expected, Totals& totals)
{
    // Every other sound keeps its values.
    const std::vector<std::optional<hokuto::edit::DsSoundValues>> old_values = ValuesOf(*game.archive);
    const std::vector<std::optional<hokuto::edit::DsSoundValues>> new_values = ValuesOf(after);
    for (const uint32_t sound : song::DsSounds(*game.archive))
    {
        const bool kept = sound < new_values.size() && new_values[sound] == old_values[sound];
        if (!expected.contains(sound) && !kept)
        {
            Fail(game, "sound values", song::DsSoundName(*game.archive, sound) + " takes other values", totals);
        }
    }
    if (hokuto::format::ExportArchive(hokuto::format::Read(hokuto::format::Write(edited))) != exported ||
        hokuto::format::ExportArchive(hokuto::format::ReadText(hokuto::format::WriteText(edited))) != exported)
    {
        Fail(game, "sound values", "a saved file gives another archive", totals);
    }
}

// Checks that each of up to kMostLevels sequence sounds of the game, with its volume halved, falls by the ratio of its
// volumes.
void CheckLevels(const Game& game, Totals& totals)
{
    constexpr std::size_t kMostLevels = 8;
    constexpr uint8_t kLeastVolume = 64;
    constexpr double kFloor = -50.0;
    constexpr double kTolerance = 0.5;

    const sdat::Archive& tables = game.archive->archive;
    // The levels, in another version.
    const std::vector<uint32_t> sounds = SequenceSounds(game);
    File halved = game.file;
    std::map<uint32_t, std::pair<uint8_t, uint8_t>> volumes;
    const std::size_t step = std::max<std::size_t>(sounds.size() / (kMostLevels * 8), 1);
    for (std::size_t i = 0; i < sounds.size() && volumes.size() < kMostLevels; i += step)
    {
        hokuto::edit::DsSoundValues values = hokuto::edit::DsSoundValuesOf(*game.archive, sounds[i]);
        const uint8_t volume = values.volume;
        if (volume < kLeastVolume || volume > hokuto::edit::kHighestDsVolume || values.bank >= tables.banks.size() ||
            !tables.banks[values.bank] || values.player >= tables.players.size() || !tables.players[values.player])
        {
            continue;
        }
        values.volume = volume / 2;
        halved = hokuto::edit::WithDsSoundValues(halved, *game.archive, sounds[i], values);
        volumes.emplace(sounds[i], std::pair(volume, values.volume));
    }
    const std::shared_ptr<const song::DsArchive> quieter =
        song::LoadDsArchive(hokuto::format::ExportArchive(halved), game.archive->build);
    for (const auto& [sound, volume] : volumes)
    {
        const std::string name = song::DsSoundName(*game.archive, sound);
        std::vector<float> before;
        try
        {
            before = RenderedDs(game.archive, sound, kPlaySeconds);
        }
        catch (const std::exception&)
        {
            totals.levels_apart++;
            continue;
        }
        const std::vector<float> played = RenderedDs(quieter, sound, kPlaySeconds);
        const double level = LevelOf(before);
        const bool reaches = std::ranges::any_of(before, [](float sample) { return std::abs(sample) >= 0.999f; });
        if (level < kFloor || reaches)
        {
            totals.levels_apart++;
            continue;
        }
        const double ratio = 20.0 * std::log10(static_cast<double>(volume.second) / volume.first);
        const double off = std::abs(LevelOf(played) - level - ratio);
        totals.levels++;
        totals.worst_level = std::max(totals.worst_level, off);
        if (off > kTolerance)
        {
            Fail(game, "sound values", name + " falls " + std::to_string(off) + " dB off the ratio of its volumes",
                 totals);
        }
    }
}

} // namespace

void SoundValues(const Game& game, Totals& totals)
{
    constexpr std::size_t kMostInTable = 32;

    const sdat::Archive& tables = game.archive->archive;
    std::vector<uint32_t> in_table;
    std::vector<uint32_t> archived;
    for (const uint32_t sound : song::DsSounds(*game.archive))
    {
        (sound < song::kFirstDsArchivedSound ? in_table : archived).push_back(sound);
    }

    File edited = game.file;
    std::map<uint32_t, hokuto::edit::DsSoundValues> expected;
    for (const std::vector<uint32_t>* sounds : {&in_table, &archived})
    {
        const std::size_t step = std::max<std::size_t>(sounds->size() / kMostInTable, 1);
        for (std::size_t i = 0; i < sounds->size() && i / step < kMostInTable; i += step)
        {
            const uint32_t sound = (*sounds)[i];
            hokuto::edit::DsSoundValues values = hokuto::edit::DsSoundValuesOf(*game.archive, sound);
            if (values.bank >= tables.banks.size() || values.player >= tables.players.size())
            {
                continue;
            }
            values.volume = std::min<uint8_t>(values.volume, 127) / 2;
            values.channel_priority = (std::min<uint8_t>(values.channel_priority, 126) + 1) % 128;
            values.player_priority = (std::min<uint8_t>(values.player_priority, 126) + 1) % 128;
            values.bank = static_cast<uint16_t>(NextIn(tables.banks, values.bank));
            values.player = static_cast<uint8_t>(NextIn(tables.players, values.player));
            if (!tables.banks[values.bank] || !tables.players[values.player])
            {
                continue;
            }
            edited = hokuto::edit::WithDsSoundValues(edited, *game.archive, sound, values);
            expected.emplace(sound, values);
            totals.value_edits++;
        }
    }

    const std::vector<uint8_t> exported = hokuto::format::ExportArchive(edited);
    if (exported.size() != game.bytes.size())
    {
        Fail(game, "sound values", "the archive takes another size", totals);
        return;
    }
    std::size_t differing = 0;
    for (std::size_t i = 0; i < exported.size(); i++)
    {
        differing += exported[i] != game.bytes[i] ? 1 : 0;
    }
    if (differing > expected.size() * 6)
    {
        Fail(game, "sound values", std::to_string(differing) + " bytes differ", totals);
    }
    const std::shared_ptr<const song::DsArchive> after = song::LoadDsArchive(exported, game.archive->build);
    for (const auto& [sound, values] : expected)
    {
        if (!(hokuto::edit::DsSoundValuesOf(*after, sound) == values))
        {
            Fail(game, "sound values", "ntdlib reads " + song::DsSoundName(*game.archive, sound) + " otherwise",
                 totals);
        }
    }

    CheckOtherSoundsKept(game, *after, edited, exported, expected, totals);
    CheckLevels(game, totals);
}

} // namespace hokuto::tools::ds_replace
