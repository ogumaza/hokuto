// SPDX-License-Identifier: MIT

// The mode --wave-sounds of hokuto_check_replace: new wave sounds.

#include "check_replace.h"

namespace hokuto::tools::replace
{

namespace
{

// The most wave sound files that --wave-sounds copies from a donor.
constexpr std::size_t kWaveSoundFiles = 8;

// The wave sound files of `archive` whose wave archives the reader reads, each once, in the order of the sounds that
// play them: each file's ID, and the first sound that plays it. Counts the others in `missing`.
std::vector<std::pair<uint32_t, uint32_t>> WaveSoundFilesOf(const sar::SoundArchive& archive, std::size_t& missing)
{
    const sar::Flavor flavor = archive.ReadOptions().flavor;
    std::vector<std::pair<uint32_t, uint32_t>> files;
    std::set<uint32_t> seen;
    for (uint32_t i = 0; i < archive.Sounds().size(); i++)
    {
        const sar::SoundInfo& sound = archive.Sounds()[i];
        if (sound.type != sar::SoundType::kWaveSound || !seen.insert(sound.file_id).second)
        {
            continue;
        }

        // The file, and the files of the wave archives that its waves name.
        const std::span<const uint8_t> bytes = archive.FileData(sound.file_id);
        bool readable = !bytes.empty();
        if (readable)
        {
            for (const edit::NamedWaveArchive& wave_archive : edit::WaveArchivesNamed(bytes, flavor))
            {
                const uint32_t index = wave_archive.id & kIndexBits;
                readable = readable && index < archive.WaveArchives().size() &&
                           !archive.FileData(archive.WaveArchives()[index].file_id).empty();
            }
        }
        if (readable)
        {
            files.emplace_back(sound.file_id, i);
        }
        else
        {
            missing++;
        }
    }

    return files;
}

// A stem of names, from `stem`, such that no item of `tables` holds the stem or any name that edit::WaveSoundNames
// makes from it for `count` sounds.
std::string FreeStem(const format::NativeTables& tables, std::string stem, std::size_t count)
{
    const auto unused = [&tables](const std::string& name)
    {
        return FreeTableName(tables, name) == name;
    };
    while (!unused(stem) || !std::ranges::all_of(edit::WaveSoundNames(stem, count), unused))
    {
        stem += "_";
    }

    return stem;
}

// The new items of a wave sound file: the new wave archives' places by the donor's wave archives' item IDs, and the new
// sounds' names and the place of the first. The bytes of each new wave archive and of the file as the Import window
// reads them, and the files among them that went in written again with the archive's versions.
struct NewWaveSounds
{
    std::map<uint32_t, uint32_t> wave_archives;
    std::vector<std::string> names;
    uint32_t first = 0;
    std::map<uint32_t, std::vector<uint8_t>> wave_archive_bytes;
    std::vector<uint8_t> file_bytes;
    std::size_t written = 0;
};

// The file after its archive gains copies of the wave archives that the waves of file `donor_file` of `donor` name,
// then that file as new wave sounds that copy sound `model` of the archive, through the reads and the edits of the
// Import windows. `built` is the archive that `file` builds. A copy of a wave archive takes a new name where the
// archive's wave archives have names. Keeps the longest time that an edit took in `slowest`.
std::pair<format::File, NewWaveSounds> WithWaveSounds(const format::File& file, const sar::SoundArchive& built,
                                                      const sar::SoundArchive& donor, uint32_t donor_file,
                                                      uint32_t model, double& slowest)
{
    const sar::Flavor flavor = donor.ReadOptions().flavor;

    const auto timed = [&slowest](std::chrono::steady_clock::time_point start)
    {
        const std::chrono::duration<double> took = std::chrono::steady_clock::now() - start;
        slowest = std::max(slowest, took.count());
    };

    // The wave archives that the file's waves name.
    const std::span<const uint8_t> bytes = donor.FileData(donor_file);
    format::File added = file;
    NewWaveSounds items;
    std::map<uint32_t, uint32_t> map;
    for (const edit::NamedWaveArchive& wave_archive : edit::WaveArchivesNamed(bytes, flavor))
    {
        const format::NativeTables& tables = *added.native->tables;
        const bool named =
            std::ranges::any_of(tables.wave_archives, [](const std::string& each) { return !each.empty(); });
        const std::string wave_archive_name = named ? FreeTableName(tables, "HOKUTO_NEW_WARC") : "";
        items.wave_archives[wave_archive.id] = static_cast<uint32_t>(tables.wave_archives.size());
        const sar::WaveArchiveInfo& old = donor.WaveArchives()[wave_archive.id & kIndexBits];
        const edit::IncomingFile incoming =
            edit::ReadIncomingFile(donor.FileData(old.file_id), edit::FileKind::kWaveArchive, flavor,
                                   edit::IncomingVersions(built, edit::FileKind::kWaveArchive));
        items.written += incoming.converted ? 1 : 0;
        const auto start = std::chrono::steady_clock::now();
        added = edit::AddWaveArchive(added, wave_archive_name, incoming.bytes);
        timed(start);
        items.wave_archive_bytes[wave_archive.id] = incoming.bytes;
        map[wave_archive.id] = items.wave_archives[wave_archive.id];
    }

    // The file as new wave sounds.
    const edit::IncomingFile incoming = edit::ReadIncomingFile(
        bytes, edit::FileKind::kWaveSoundFile, flavor, edit::IncomingVersions(built, edit::FileKind::kWaveSoundFile));
    items.written += incoming.converted ? 1 : 0;
    items.file_bytes = incoming.bytes;
    const std::size_t count = edit::WaveSoundCount(items.file_bytes, flavor);
    const std::string stem = FreeStem(*added.native->tables, "HOKUTO_NEW_WSD", count);
    items.names = edit::WaveSoundNames(stem, count);
    items.first = static_cast<uint32_t>(added.native->tables->sounds.size());
    const auto start = std::chrono::steady_clock::now();
    added = edit::AddWaveSounds(added, model, stem, items.file_bytes, map);
    timed(start);

    return {std::move(added), std::move(items)};
}

// What is wrong with the files of `after`, the archive with the new wave sounds of `items`, or nothing. The new wave
// archives hold the bytes that the Import window read, and the new file those bytes but for the wave archives that its
// waves name. `flavor` is the flavor of the archive.
std::string NewFilesProblem(const sar::SoundArchive& after, const NewWaveSounds& items, sar::Flavor flavor)
{
    std::map<uint32_t, uint32_t> ids;
    for (const auto& [old, place] : items.wave_archives)
    {
        ids[old] = sar::ItemId(sar::kWaveArchiveItem, place);
        if (!std::ranges::equal(after.FileData(after.WaveArchives()[place].file_id), items.wave_archive_bytes.at(old)))
        {
            return "gives a new wave archive other bytes";
        }
    }
    const auto new_file = static_cast<uint32_t>(after.FileCount() - 1);
    if (!std::ranges::equal(after.FileData(new_file), sar::WithWaveArchives(items.file_bytes, flavor, ids)))
    {
        return "gives the new file other bytes";
    }

    return {};
}

// Wave sound `index` of `archive`, decoded, or nothing when the decoder refuses it.
std::optional<sar::Pcm> DecodedWaveSound(const sar::SoundArchive& archive, uint32_t index)
{
    try
    {
        return song::DecodeWaveSound(archive, index);
    }
    catch (const std::runtime_error&)
    {
        return std::nullopt;
    }
}

// What is wrong with the new wave sounds of `items` in `after`, or nothing: each has to have the values of sound
// `model` of `built` and play the samples that the donor's first sound of its wave sound in file `donor_file` plays.
// Counts the sounds compared, those that neither side decodes, and those that no sound of the donor plays.
std::string NewSoundsProblem(const sar::SoundArchive& built, const sar::SoundArchive& after,
                             const sar::SoundArchive& donor, uint32_t donor_file, uint32_t model,
                             const NewWaveSounds& items, Totals& totals)
{
    const std::size_t count = items.names.size();
    const auto new_file = static_cast<uint32_t>(after.FileCount() - 1);

    // Each new sound holds the model's values and plays the donor's samples. A wave sound has no banks. A copy of a
    // sequence sound keeps the model's channel priority and release priority fix in its details.
    sar::SoundInfo expected = built.Sounds()[model];
    if (expected.type == sar::SoundType::kSequence)
    {
        expected.type = sar::SoundType::kWaveSound;
        expected.banks = sar::SoundInfo().banks;
    }
    for (uint32_t k = 0; k < count; k++)
    {
        const uint32_t sound = items.first + k;
        const sar::SoundInfo& info = after.Sounds()[sound];
        if (info.name != items.names[k] || info.file_id != new_file || info.wave_sound_index != k ||
            !SameValues(info, expected))
        {
            return "gives " + items.names[k] + " other values than the model's";
        }

        // The donor's first sound that plays the wave sound, decoded on each side.
        const auto plays = [donor_file, k](const sar::SoundInfo& other)
        {
            return other.type == sar::SoundType::kWaveSound && other.file_id == donor_file &&
                   other.wave_sound_index == k;
        };
        const auto player = std::ranges::find_if(donor.Sounds(), plays);
        if (player == donor.Sounds().end())
        {
            totals.wsd_unplayed++;
            continue;
        }

        const std::optional<sar::Pcm> theirs =
            DecodedWaveSound(donor, static_cast<uint32_t>(player - donor.Sounds().begin()));
        const std::optional<sar::Pcm> ours = DecodedWaveSound(after, sound);
        if (theirs.has_value() != ours.has_value() || (theirs && *theirs != *ours))
        {
            return items.names[k] + " plays other samples than " + player->name;
        }
        totals.wsd_compared += theirs ? 1 : 0;
        totals.wsd_undecoded += theirs ? 0 : 1;
    }

    return {};
}

// What is wrong with `added`, the version of `file` whose archive gains `items`: copies of the wave archives that the
// waves of file `donor_file` of `donor` name, and the file's wave sounds as copies of sound `model`. `built` is the
// archive that `file` builds. Returns nothing when nothing is wrong. Counts the new sounds whose samples are compared.
std::string WaveSoundProblem(const format::File& added, const sar::SoundArchive& built, const sar::SoundArchive& donor,
                             uint32_t donor_file, uint32_t model, const NewWaveSounds& items, Totals& totals)
{
    const sar::Flavor flavor = built.ReadOptions().flavor;
    const Archive after =
        song::LoadArchive(format::ExportArchive(added), "after.bfsar", format::ExportGroupFiles(added));
    const std::size_t count = items.names.size();
    if (after->Sounds().size() != built.Sounds().size() + count ||
        after->WaveArchives().size() != built.WaveArchives().size() + items.wave_archives.size() ||
        after->FileCount() != built.FileCount() + items.wave_archives.size() + 1)
    {
        return "doesn't give the archive the new items";
    }

    // The other items and files.
    if (const std::string problem = OtherItemsProblem(built, *after, false, true); !problem.empty())
    {
        return problem;
    }

    // The new wave archives hold the bytes that the Import window read, and the new file those bytes but for the wave
    // archives that its waves name.
    if (const std::string problem = NewFilesProblem(*after, items, flavor); !problem.empty())
    {
        return problem;
    }

    // The new sounds.
    if (const std::string problem = NewSoundsProblem(built, *after, donor, donor_file, model, items, totals);
        !problem.empty())
    {
        return problem;
    }

    // The new version saves and opens again.
    if (!hokuto::tools::SavesAndOpens(added, true))
    {
        return "doesn't save and open again";
    }

    return {};
}

// The first sound of `built` of type `type`, if any.
std::optional<uint32_t> FirstOfType(const sar::SoundArchive& built, sar::SoundType type)
{
    const auto found = std::ranges::find(built.Sounds(), type, &sar::SoundInfo::type);
    if (found == built.Sounds().end())
    {
        return std::nullopt;
    }

    return static_cast<uint32_t>(found - built.Sounds().begin());
}

// What is wrong when the wave sounds of file `donor_file` of `donor` go into `file` as copies of sound `model`, or
// nothing. `built` is the archive that `file` builds. The new sounds then join groups (JoinAndNewGroupCheck). Counts
// the new sounds, wave archives and joins.
std::string AddWaveSounds(const format::File& file, const sar::SoundArchive& built,
                          std::span<const edit::Group> edit_groups, const sar::SoundArchive& donor, uint32_t donor_file,
                          uint32_t model, Joins& joins, Totals& totals)
{
    const auto [added, items] = WithWaveSounds(file, built, donor, donor_file, model, totals.wsd_slowest);
    std::string problem = WaveSoundProblem(added, built, donor, donor_file, model, items, totals);
    totals.wsd_sounds += items.names.size();
    totals.wsd_written += items.written;
    const bool copied_sequence = built.Sounds()[model].type == sar::SoundType::kSequence;
    totals.wsd_from_sequences += copied_sequence ? items.names.size() : 0;
    totals.wsd_wave_archives += items.wave_archives.size();

    // The new sounds join the groups that load the model. The first import's sounds also join a new group.
    if (problem.empty() && JoinsGroups(joins, edit_groups))
    {
        std::vector<uint32_t> sounds;
        for (std::size_t k = 0; k < items.names.size(); k++)
        {
            sounds.push_back(sar::ItemId(sar::kSoundItem, static_cast<uint32_t>(items.first + k)));
        }
        const std::vector<uint32_t> files_before = edit::FilesOfItem(built, sar::ItemId(sar::kSoundItem, model));
        const auto with_sounds = [&]() -> const format::File&
        {
            return added;
        };
        problem = JoinAndNewGroupCheck(file, built, edit_groups, sounds, files_before, {}, with_sounds, joins, totals);
    }

    return problem;
}

} // namespace

void CheckNewWaveSounds(const format::File& file, const sar::SoundArchive& built,
                        std::span<const edit::Group> edit_groups, const sar::SoundArchive& donor, bool self,
                        const std::string& name, Totals& totals)
{
    Joins joins;

    // The models: the sound that plays the file in the archive itself, or else the archive's first wave sound, then the
    // archive's first sequence sound.
    const std::optional<uint32_t> wave_sound = FirstOfType(built, sar::SoundType::kWaveSound);
    const std::optional<uint32_t> sequence = FirstOfType(built, sar::SoundType::kSequence);
    const std::vector<std::pair<uint32_t, uint32_t>> files = WaveSoundFilesOf(donor, totals.wsd_missing);
    for (std::size_t f = 0; f < files.size() && f < kWaveSoundFiles; f++)
    {
        const auto [donor_file, player] = files[f];
        std::vector<uint32_t> models;
        if (self || wave_sound)
        {
            models.push_back(self ? player : *wave_sound);
        }
        if (sequence)
        {
            models.push_back(*sequence);
        }

        for (const uint32_t model : models)
        {
            std::string problem;
            try
            {
                problem = AddWaveSounds(file, built, edit_groups, donor, donor_file, model, joins, totals);
            }
            catch (const std::exception& error)
            {
                problem = std::string("fails: ") + error.what();
            }

            if (!problem.empty())
            {
                std::printf("%s: file %u with %s as the model %s\n", name.c_str(), donor_file,
                            built.Sounds()[model].name.c_str(), problem.c_str());
                totals.wsd_failed++;
            }
            totals.wsd_files++;
        }
    }
}

} // namespace hokuto::tools::replace
