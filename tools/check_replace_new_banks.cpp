// SPDX-License-Identifier: MIT

// The mode --banks of hokuto_check_replace: new banks.

#include "check_replace.h"

namespace hokuto::tools::replace
{

namespace
{

// The most sequence sounds that --banks copies in an archive, and the seconds of each render.
constexpr std::size_t kBankModels = 8;
constexpr double kBankSeconds = 10.0;

} // namespace

std::string FreeTableName(const format::NativeTables& tables, std::string stem)
{
    const auto taken = [&tables](const std::string& name)
    {
        const auto named = [&name](const auto& item)
        {
            return item.name == name;
        };
        const auto same = [&name](const std::string& other)
        {
            return other == name;
        };
        return std::ranges::any_of(tables.sounds, named) || std::ranges::any_of(tables.banks, same) ||
               std::ranges::any_of(tables.wave_archives, same) || std::ranges::any_of(tables.players, named) ||
               std::ranges::any_of(tables.groups, same);
    };
    while (taken(stem))
    {
        stem += "_";
    }

    return stem;
}

namespace
{

// The samples of sound `sound` of `archive` on ntdlib's model of the console, for up to kBankSeconds.
std::vector<int16_t> Rendered(const Archive& archive, uint32_t sound)
{
    const auto sequence = std::make_shared<const song::Sequence>(song::ReadSequence(*archive, sound));
    song::SequenceModel model(archive, sequence);
    std::vector<int16_t> samples;
    const auto frames = static_cast<uint64_t>(kBankSeconds * model.SampleRate()) / model.FrameSamples();
    for (bool running = model.SeekFrame(0); running; running = model.Frames() < frames && model.RunFrame())
    {
        const std::span<const int16_t> output = model.Output();
        samples.insert(samples.end(), output.begin(), output.end());
    }

    return samples;
}

// The new items of one model: the new banks' item IDs by the old banks' item IDs, the new wave archives' places by the
// old wave archives' item IDs, and the new sound's place.
struct NewItems
{
    std::map<uint32_t, uint32_t> banks;
    std::map<uint32_t, uint32_t> wave_archives;
    uint32_t sound = 0;
};

} // namespace

std::span<const uint8_t> BankFile(const sar::SoundArchive& archive, uint32_t bank)
{
    const uint32_t index = bank & kIndexBits;
    return index < archive.Banks().size() ? archive.FileData(archive.Banks()[index].file_id)
                                          : std::span<const uint8_t>{};
}

namespace
{

// The banks in the slots of `info`, a sound of `archive`, each once. Nothing when a bank or a wave archive that its
// waves name lacks its file.
std::optional<std::vector<uint32_t>> ModelBanks(const sar::SoundArchive& archive, const sar::SoundInfo& info)
{
    const sar::Flavor flavor = archive.ReadOptions().flavor;
    std::vector<uint32_t> banks;
    for (const uint32_t bank : info.banks)
    {
        if (bank == sar::kNoItem || std::ranges::find(banks, bank) != banks.end())
        {
            continue;
        }

        const std::span<const uint8_t> bytes = BankFile(archive, bank);
        if (bytes.empty())
        {
            return std::nullopt;
        }
        for (const edit::NamedWaveArchive& wave_archive : edit::WaveArchivesNamed(bytes, flavor))
        {
            const uint32_t index = wave_archive.id & kIndexBits;
            if (index >= archive.WaveArchives().size() ||
                archive.FileData(archive.WaveArchives()[index].file_id).empty())
            {
                return std::nullopt;
            }
        }
        banks.push_back(bank);
    }

    return banks;
}

// The file after its archive gains copies of `banks`, banks of `built`, through the edits of the Import windows. Copies
// of the wave archives that their waves name come first. A copy of a wave archive with a name takes a new name. The new
// banks' waves lie in the new wave archives. Then the archive gains a copy of sound `model` that plays the new banks
// from `source`. Keeps the longest time that an edit took in `slowest`.
std::pair<format::File, NewItems> WithCopies(const format::File& file, const sar::SoundArchive& built, uint32_t model,
                                             std::span<const uint32_t> banks, const Source& source, double& slowest)
{
    const sar::Flavor flavor = built.ReadOptions().flavor;

    const auto timed = [&slowest](std::chrono::steady_clock::time_point start)
    {
        const std::chrono::duration<double> took = std::chrono::steady_clock::now() - start;
        slowest = std::max(slowest, took.count());
    };

    // Each bank after the wave archives that it requires.
    format::File added = file;
    NewItems items;
    for (const uint32_t bank : banks)
    {
        const std::span<const uint8_t> bytes = BankFile(built, bank);
        std::map<uint32_t, uint32_t> map;
        for (const edit::NamedWaveArchive& wave_archive : edit::WaveArchivesNamed(bytes, flavor))
        {
            const uint32_t index = wave_archive.id & kIndexBits;
            if (!items.wave_archives.contains(wave_archive.id))
            {
                const format::NativeTables& tables = *added.native->tables;
                const std::string wave_archive_name =
                    tables.wave_archives[index].empty() ? "" : FreeTableName(tables, "HOKUTO_NEW_WARC");
                items.wave_archives[wave_archive.id] = static_cast<uint32_t>(tables.wave_archives.size());
                const auto start = std::chrono::steady_clock::now();
                added =
                    edit::AddWaveArchive(added, wave_archive_name, built.FileData(built.WaveArchives()[index].file_id));
                timed(start);
            }
            map[wave_archive.id] = items.wave_archives[wave_archive.id];
        }

        items.banks[bank] = sar::ItemId(sar::kBankItem, static_cast<uint32_t>(added.native->tables->banks.size()));
        const std::string bank_name = FreeTableName(*added.native->tables, "HOKUTO_NEW_BANK");
        const auto start = std::chrono::steady_clock::now();
        added = edit::AddBank(added, bank_name, bytes, map);
        timed(start);
    }

    // The copy of the model, with the new banks in the model's slots.
    std::vector<uint32_t> slots = built.Sounds()[model].banks;
    for (uint32_t& slot : slots)
    {
        slot = slot == sar::kNoItem ? slot : items.banks.at(slot);
    }
    items.sound = static_cast<uint32_t>(added.native->tables->sounds.size());
    const std::string sound_name = FreeTableName(*added.native->tables, "HOKUTO_NEW_SOUND");
    const auto start = std::chrono::steady_clock::now();
    added = edit::AddSequenceSound(added, model, sound_name, source.incoming.sequence, *source.start, {}, slots);
    timed(start);

    return {std::move(added), std::move(items)};
}

// What is wrong with `added`, the version of `file` whose archive gains `items`: copies of the banks in sound `model`'s
// slots and of their wave archives, and a copy of the model that plays the new banks. `built` is the archive that
// `file` builds. Returns nothing when nothing is wrong. Counts a new sound that stays silent.
std::string BankProblem(const format::File& added, const Archive& built, uint32_t model, const NewItems& items,
                        Totals& totals)
{
    const sar::Flavor flavor = built->ReadOptions().flavor;
    hokuto::tools::CheckSaved(added, true);
    const Archive after =
        song::LoadArchive(format::ExportArchive(added), "after.bfsar", format::ExportGroupFiles(added));
    const std::size_t files = items.banks.size() + items.wave_archives.size() + 1;
    if (after->Sounds().size() != built->Sounds().size() + 1 ||
        after->Banks().size() != built->Banks().size() + items.banks.size() ||
        after->WaveArchives().size() != built->WaveArchives().size() + items.wave_archives.size() ||
        after->FileCount() != built->FileCount() + files)
    {
        return "doesn't give the archive the new items";
    }

    // The other items and files.
    if (const std::string problem = OtherItemsProblem(*built, *after, true, false); !problem.empty())
    {
        return problem;
    }

    // The new wave archives hold the old files' bytes, and the new banks the old banks' bytes but for the wave archives
    // that their waves name.
    std::map<uint32_t, uint32_t> ids;
    for (const auto& [old, place] : items.wave_archives)
    {
        ids[old] = sar::ItemId(sar::kWaveArchiveItem, place);
        const std::span<const uint8_t> before = built->FileData(built->WaveArchives()[old & kIndexBits].file_id);
        if (!std::ranges::equal(after->FileData(after->WaveArchives()[place].file_id), before))
        {
            return "gives a new wave archive other bytes";
        }
    }
    for (const auto& [old, now] : items.banks)
    {
        const std::vector<uint8_t> expected = sar::WithWaveArchives(BankFile(*built, old), flavor, ids);
        if (!std::ranges::equal(BankFile(*after, now), expected))
        {
            return "gives a new bank other bytes";
        }
    }

    // The new sound holds the model's values with the new banks, and plays the model's samples.
    sar::SoundInfo expected = built->Sounds()[model];
    for (uint32_t& bank : expected.banks)
    {
        bank = bank == sar::kNoItem ? bank : items.banks.at(bank);
    }
    if (!SameValues(after->Sounds()[items.sound], expected))
    {
        return "gives the new sound other values than the model's with the new banks";
    }
    const std::vector<int16_t> samples = Rendered(after, items.sound);
    if (samples != Rendered(built, model))
    {
        return "plays other samples than the model";
    }
    totals.bank_silent += std::ranges::all_of(samples, [](int16_t sample) { return sample == 0; }) ? 1 : 0;

    return {};
}

} // namespace

void CheckNewBanks(const format::File& file, const Archive& built,
                   std::span<const std::optional<edit::SoundPlace>> places, const std::string& name, Totals& totals)
{
    const std::vector<uint32_t> models = NewModels(places);
    const std::size_t stride = std::max<std::size_t>(1, models.size() / kBankModels);
    std::vector<uint32_t> order;
    for (std::size_t first = 0; first < stride; first++)
    {
        for (std::size_t i = first; i < models.size(); i += stride)
        {
            order.push_back(models[i]);
        }
    }

    for (std::size_t k = 0; k < order.size() && totals.bank_checked < kBankModels; k++)
    {
        const uint32_t model = order[k];
        const sar::SoundInfo& info = built->Sounds()[model];
        std::string problem;
        try
        {
            const Source source = SourceOf(built->FileData(info.file_id), info.start_offset);
            const std::optional<std::vector<uint32_t>> banks = ModelBanks(*built, info);
            if (!source.start || Untracked(source, info))
            {
                totals.bank_skipped++;
                continue;
            }
            if (!banks)
            {
                totals.bank_missing++;
                continue;
            }
            if (banks->empty())
            {
                totals.bankless++;
                continue;
            }

            totals.bank_checked++;
            const auto [added, items] = WithCopies(file, *built, model, *banks, source, totals.bank_slowest);
            problem = BankProblem(added, built, model, items, totals);
            totals.new_banks += items.banks.size();
            totals.new_wave_archives += items.wave_archives.size();
        }
        catch (const std::exception& error)
        {
            problem = std::string("fails: ") + error.what();
        }

        if (!problem.empty())
        {
            std::printf("%s: %s %s\n", name.c_str(), info.name.c_str(), problem.c_str());
            totals.bank_failed++;
        }
    }
}

} // namespace hokuto::tools::replace
