// SPDX-License-Identifier: MIT

// hokuto_check_ds_banks: the rounds that change the structure of the banks, and the drops.

#include "check_ds_banks.h"

namespace hokuto::tools::ds_banks
{

namespace
{

// Whether a bank's instruments hold the same values, wherever their data is in the file.
bool SameInstruments(const sdat::Bank& a, const sdat::Bank& b)
{
    const auto same = [](const sdat::Instrument& x, const sdat::Instrument& y)
    {
        return x.type == y.type && x.region == y.region && x.min_key == y.min_key && x.max_key == y.max_key &&
               x.split_keys == y.split_keys && x.regions == y.regions;
    };

    return std::ranges::equal(a.instruments, b.instruments, same);
}

bool PlaysOneRegion(const sdat::Instrument& instrument)
{
    return instrument.type == kPcm || instrument.type == kPsg || instrument.type == kNoise;
}

// The first bank of each bank file that ntdlib reads, with its model.
std::vector<std::pair<uint32_t, sdat::Bank>> BankModels(const Game& game)
{
    const sdat::Archive& tables = game.archive->archive;
    std::vector<std::pair<uint32_t, sdat::Bank>> models;
    std::set<uint32_t> files;
    for (uint32_t bank = 0; bank < tables.banks.size(); bank++)
    {
        if (tables.banks[bank] && files.insert(tables.banks[bank]->file_id).second)
        {
            try
            {
                models.emplace_back(bank, hokuto::edit::DsBankModelOf(*game.archive, bank));
            }
            catch (const std::invalid_argument&)
            {
                continue;
            }
        }
    }

    return models;
}

} // namespace

void Structure(const Game& game, Totals& totals)
{
    constexpr uint32_t kSoundStep = 32;
    constexpr int kLeastLastKey = 3;
    constexpr std::size_t kMostRuns = 8;

    const sdat::Archive& tables = game.archive->archive;
    File edited = game.file;
    std::vector<std::pair<uint32_t, sdat::Bank>> expected;
    for (auto& [bank, model] : BankModels(game))
    {
        bool changed = false;
        for (uint32_t program = 0; program < model.instruments.size(); program++)
        {
            const sdat::Instrument& instrument = model.instruments[program];
            if (PlaysOneRegion(instrument))
            {
                model = hokuto::edit::WithDsKeySplit(std::move(model), program);
                totals.key_splits++;
                changed = true;
            }
            else if (instrument.type == kKeySplit && instrument.regions.size() < kMostRuns &&
                     instrument.split_keys[0] >= kLeastLastKey)
            {
                const int key = (instrument.split_keys[0] + 1) / 2 + 1;
                model = hokuto::edit::WithDsSplit(std::move(model), program, 0, key);
                totals.split_runs++;
                changed = true;
            }
        }
        if (changed)
        {
            edited = hokuto::edit::WithDsFile(edited, tables.banks[bank]->file_id, sdat::BankFile(model));
            expected.emplace_back(bank, std::move(model));
            totals.structure_files++;
        }
    }
    if (expected.empty())
    {
        return;
    }

    const std::vector<uint8_t> exported = hokuto::format::ExportArchive(edited);
    const std::shared_ptr<const DsArchive> after = hokuto::song::LoadDsArchive(exported, game.archive->build);
    for (const auto& [bank, model] : expected)
    {
        if (!SameInstruments(hokuto::edit::DsBankModelOf(*after, bank), model))
        {
            std::printf("%s: structure: ntdlib reads %s otherwise\n", game.name.c_str(),
                        hokuto::song::DsBankName(*after, bank).c_str());
            totals.failed++;
        }
    }
    CheckForms(game, edited, exported, "structure", totals);

    const std::vector<std::optional<uint32_t>> sequences = hokuto::format::DsSoundSequences(game.file, game.bytes);
    for (uint32_t sound = 0; sound < sequences.size(); sound += kSoundStep)
    {
        if (!sequences[sound])
        {
            continue;
        }

        std::vector<float> before;
        try
        {
            before = RenderedDs(game.archive, sound, kPlaySeconds);
        }
        catch (const std::exception&)
        {
            totals.unplayed++;
            continue;
        }
        std::vector<float> played;
        try
        {
            played = RenderedDs(after, sound, kPlaySeconds);
        }
        catch (const std::exception& error)
        {
            const std::string name = hokuto::song::DsSoundName(*game.archive, sound);
            if (HeapFull(error))
            {
                std::printf("%s: structure: %s stays apart: %s\n", game.name.c_str(), name.c_str(), error.what());
                totals.too_large++;
            }
            else
            {
                std::printf("%s: structure: %s: %s\n", game.name.c_str(), name.c_str(), error.what());
                totals.failed++;
            }
            continue;
        }
        if (played != before)
        {
            std::printf("%s: structure: %s plays otherwise\n", game.name.c_str(),
                        hokuto::song::DsSoundName(*game.archive, sound).c_str());
            totals.failed++;
        }
        else if (std::ranges::all_of(before, [](float sample) { return sample == 0.0f; }))
        {
            totals.silent_sounds++;
        }
        else
        {
            totals.same_sounds++;
        }
    }
}

namespace
{

// The first second of the Bank Editor's note of `program`, or nothing where a preview can't play it.
std::optional<std::vector<float>> PreviewNote(const DsArchive& archive, uint32_t bank, uint32_t program)
{
    try
    {
        hokuto::song::DsNotePreview preview(archive, bank, static_cast<int>(program));
        const auto frames = static_cast<std::size_t>(preview.SampleRate());
        std::vector<float> out(frames * 2);
        out.resize(preview.Render(out.data(), frames) * 2);
        return out;
    }
    catch (const std::runtime_error&)
    {
        return std::nullopt;
    }
}

} // namespace

void Copies(const Game& game, Totals& totals)
{
    constexpr std::size_t kMostCopies = 64;

    struct Copy
    {
        uint32_t bank = 0;
        uint32_t from = 0;
        uint32_t to = 0;
    };

    const sdat::Archive& tables = game.archive->archive;
    std::vector<std::pair<uint32_t, sdat::Bank>> models = BankModels(game);
    File edited = game.file;
    std::vector<Copy> copies;
    const std::size_t step = std::max<std::size_t>(models.size() / kMostCopies, 1);
    for (std::size_t i = 0; i < models.size() && copies.size() < kMostCopies; i += step)
    {
        const auto& [bank, model] = models[i];
        const auto from = std::ranges::find_if(model.instruments, PlaysOneRegion);
        const std::optional<uint32_t> to = hokuto::edit::FreeDsProgram(model, 0);
        if (from != model.instruments.end() && to)
        {
            const auto program = static_cast<uint32_t>(from - model.instruments.begin());
            edited = hokuto::edit::WithDsFile(edited, tables.banks[bank]->file_id,
                                              sdat::BankFile(hokuto::edit::WithDsInstrumentCopy(model, program, *to)));
            copies.push_back({bank, program, *to});
        }
    }
    if (copies.empty())
    {
        return;
    }

    const std::shared_ptr<const DsArchive> after =
        hokuto::song::LoadDsArchive(hokuto::format::ExportArchive(edited), game.archive->build);
    for (const Copy& copy : copies)
    {
        const std::optional<std::vector<float>> before = PreviewNote(*game.archive, copy.bank, copy.from);
        const std::optional<std::vector<float>> copied = PreviewNote(*after, copy.bank, copy.to);
        if (!before)
        {
            totals.copies_apart++;
        }
        else if (!copied || *copied != *before)
        {
            std::printf("%s: copies: program %u of %s plays otherwise at program %u\n", game.name.c_str(), copy.from,
                        hokuto::song::DsBankName(*after, copy.bank).c_str(), copy.to);
            totals.failed++;
        }
        else if (std::ranges::all_of(*before, [](float sample) { return sample == 0.0f; }))
        {
            totals.copies_silent++;
            totals.copies++;
        }
        else
        {
            totals.copies++;
        }
    }
}

namespace
{

// A wave that Drops drops: program `from` of `bank` moves to program `to`, and `reference` plays the wave as before.
struct Drop
{
    uint32_t bank = 0;
    uint32_t from = 0;
    uint32_t to = 0;
    uint32_t reference = 0;
};

// Drops a PCM wave of up to kMostDrops bank models in `edited`, and returns the drops. `changed` receives the files
// that the drops change.
std::vector<Drop> FindDrops(const Game& game, File& edited, std::set<uint32_t>& changed, Totals& totals)
{
    constexpr std::size_t kMostDrops = 64;

    const sdat::Archive& tables = game.archive->archive;
    std::vector<std::pair<uint32_t, sdat::Bank>> models = BankModels(game);
    std::vector<Drop> drops;
    const std::size_t step = std::max<std::size_t>(models.size() / kMostDrops, 1);
    for (std::size_t i = 0; i < models.size() && drops.size() < kMostDrops; i += step)
    {
        const auto& [bank, model] = models[i];
        const auto from = std::ranges::find_if(
            model.instruments, [](const sdat::Instrument& instrument) { return instrument.type == kPcm; });
        if (from == model.instruments.end() || from->region.wave_archive >= tables.banks[bank]->wave_archives.size())
        {
            continue;
        }

        const uint32_t source = tables.banks[bank]->wave_archives[from->region.wave_archive];
        try
        {
            const hokuto::edit::DsNewWaves added = hokuto::edit::DsNewWavePlaces(*game.archive, bank, 1);
            const uint32_t file_id = tables.wave_archives[added.wave_archive]->file_id & sdat::kFileIdBits;
            if (source >= tables.wave_archives.size() || !tables.wave_archives[source] || changed.contains(file_id))
            {
                continue;
            }
            const sdat::WaveArchive waves =
                sdat::WaveArchive::Parse(tables.File(tables.wave_archives[source]->file_id & sdat::kFileIdBits));
            if (from->region.wave >= waves.waves.size())
            {
                continue;
            }

            const auto program = static_cast<uint32_t>(from - model.instruments.begin());
            const hokuto::edit::DroppedDsWaves listed =
                hokuto::edit::WithDroppedDsWaves(model, std::nullopt, added.places);
            const std::optional<uint32_t> reference =
                hokuto::edit::FreeDsProgram(listed.bank, listed.programs.front() + 1);
            if (!reference)
            {
                continue;
            }
            const hokuto::edit::DsWavePlace old_wave{from->region.wave_archive, from->region.wave};
            const hokuto::edit::DroppedDsWaves on_region = hokuto::edit::WithDroppedDsWaves(
                hokuto::edit::WithNewDsInstrument(listed.bank, *reference, hokuto::edit::NewDsRegion(old_wave)),
                std::pair(program, std::size_t{0}), added.places);
            const std::vector<std::vector<uint8_t>> bytes = {sdat::WaveBytes(waves.waves[from->region.wave])};
            edited = hokuto::edit::WithDsBankModel(
                hokuto::edit::WithDsNewWaves(edited, *game.archive, added.wave_archive, bytes), *game.archive, bank,
                on_region.bank);
            changed.insert(file_id);
            changed.insert(tables.banks[bank]->file_id);
            drops.push_back({bank, program, listed.programs.front(), *reference});
        }
        catch (const std::invalid_argument& error)
        {
            std::printf("%s: drops: %s refuses a wave: %s\n", game.name.c_str(),
                        hokuto::song::DsBankName(*game.archive, bank).c_str(), error.what());
            totals.drops_apart++;
        }
        catch (const xsflib::FormatError&)
        {
            totals.drops_apart++;
        }
    }

    return drops;
}

} // namespace

void Drops(const Game& game, Totals& totals)
{
    const sdat::Archive& tables = game.archive->archive;
    File edited = game.file;
    std::set<uint32_t> changed;
    const std::vector<Drop> drops = FindDrops(game, edited, changed, totals);
    if (drops.empty())
    {
        return;
    }

    const std::shared_ptr<const DsArchive> after =
        hokuto::song::LoadDsArchive(hokuto::format::ExportArchive(edited), game.archive->build);
    for (uint32_t id = 0; id < tables.files.size(); id++)
    {
        if (!changed.contains(id) && !std::ranges::equal(tables.File(id), after->archive.File(id)))
        {
            std::printf("%s: drops: file %u changed\n", game.name.c_str(), id);
            totals.failed++;
        }
    }
    for (const Drop& drop : drops)
    {
        const std::optional<std::vector<float>> before = PreviewNote(*game.archive, drop.bank, drop.from);
        if (!before)
        {
            totals.drops_apart++;
            continue;
        }

        const std::optional<std::vector<float>> changed_wave = PreviewNote(*after, drop.bank, drop.from);
        const std::optional<std::vector<float>> dropped = PreviewNote(*after, drop.bank, drop.to);
        const std::optional<std::vector<float>> reference = PreviewNote(*after, drop.bank, drop.reference);
        if (!changed_wave || *changed_wave != *before || !dropped || !reference || *dropped != *reference)
        {
            std::printf("%s: drops: %s plays otherwise after a drop: program %u, %u and %u\n", game.name.c_str(),
                        hokuto::song::DsBankName(*after, drop.bank).c_str(), drop.from, drop.to, drop.reference);
            totals.failed++;
            continue;
        }
        totals.drops++;
        if (std::ranges::all_of(*before, [](float sample) { return sample == 0.0f; }))
        {
            totals.drops_silent++;
        }
    }
}

} // namespace hokuto::tools::ds_banks
