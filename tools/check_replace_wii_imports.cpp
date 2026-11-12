// SPDX-License-Identifier: MIT

// The new items of hokuto_check_replace on archives of the Wii.

#include "check_replace.h"

namespace hokuto::tools::replace
{

std::vector<uint32_t> WiiModels(const ntdlib::rsar::SoundArchive& archive, ntdlib::rsar::SoundType type,
                                std::size_t limit)
{
    std::vector<uint32_t> all;
    for (uint32_t s = 0; s < archive.Sounds().size(); s++)
    {
        const ntdlib::rsar::SoundInfo& info = archive.Sounds()[s];
        if (info.type == type && archive.File(info.file_id))
        {
            all.push_back(s);
        }
    }
    std::vector<uint32_t> chosen;
    for (const std::size_t index : SpreadIndices(all.size(), limit))
    {
        chosen.push_back(all[index]);
    }

    return chosen;
}

namespace
{

// The groups with names of the Wii's `file` that hold file `id`: the groups that the Import window proposes.
std::vector<uint32_t> WiiGroupsHolding(const format::File& file, uint32_t id)
{
    std::vector<uint32_t> holding;
    for (const edit::Group& group : edit::WiiGroupsOf(file))
    {
        if (std::ranges::find(group.files, id) != group.files.end())
        {
            holding.push_back(group.group);
        }
    }

    return holding;
}

} // namespace

std::string WiiOthersProblem(const ntdlib::rsar::ArchiveModel& before, const ntdlib::rsar::ArchiveModel& after,
                             std::span<const uint32_t> joining)
{
    const auto keeps = [](const auto& old, const auto& now)
    {
        return now.size() >= old.size() && std::equal(old.begin(), old.end(), now.begin());
    };

    if (after.version != before.version || after.player_settings != before.player_settings ||
        after.players != before.players)
    {
        return "changes the players or their settings";
    }
    if (!keeps(before.sounds, after.sounds) || !keeps(before.banks, after.banks))
    {
        return "changes another sound or bank";
    }
    if (!keeps(before.files, after.files))
    {
        return "changes another file";
    }
    if (after.groups.size() != before.groups.size())
    {
        return "changes the groups";
    }
    for (std::size_t g = 0; g < before.groups.size(); g++)
    {
        const std::vector<uint32_t>& old = before.groups[g].file_ids;
        const std::vector<uint32_t>& now = after.groups[g].file_ids;
        const auto joins = [&joining](uint32_t id)
        {
            return std::ranges::find(joining, id) != joining.end();
        };
        if (after.groups[g].name != before.groups[g].name || !keeps(old, now) ||
            !std::all_of(now.begin() + static_cast<std::ptrdiff_t>(old.size()), now.end(), joins))
        {
            return "changes group " + before.groups[g].name;
        }
    }

    return {};
}

namespace
{

// The counts of the Wii's imports.
struct WiiImportTotals
{
    std::size_t imports = 0;
    std::size_t failed = 0;
    std::size_t without_start = 0; // models that start where no command of their files starts
    std::size_t untracked = 0;     // models whose commands open tracks that they don't allocate
    std::size_t joins = 0;         // groups that took a new file
    std::size_t sounds = 0;        // new wave sounds
    std::size_t waves = 0;         // new waves
    std::size_t sounding = 0;      // copies of models with new banks whose renders hold sound
    std::map<std::string, std::size_t> refused;
};

// What is wrong when the Wii's `file` takes a copy of sequence sound `model` that plays the model's file from its start
// (`source`), or nothing. `built` is the archive that `file` builds, and `before` is that archive's model. The copy
// joins the groups that load the model. Without `bank`, the copy plays the model's bank and has to play the model's
// notes, read by the rules of `build`. With `bank`, the copy plays that bank: a copy of the model's bank that the file
// holds. The copy then has to play the model's samples for 10 s on ntdlib's model of NW4R SND and AX. With `save`, the
// file has to save and open again in both forms. Throws a Refusal when the import refuses the sound.
std::string WiiNewSoundProblem(const format::File& file, const ntdlib::rsar::SoundArchive& built,
                               const ntdlib::rsar::ArchiveModel& before, const ntdlib::SndBuild& build, uint32_t model,
                               const Source& source, std::optional<uint32_t> bank, bool save, WiiImportTotals& totals)
{
    const ntdlib::rsar::SoundInfo& info = built.Sounds()[model];
    const std::vector<uint32_t> groups = WiiGroupsHolding(file, info.file_id);
    const std::string sound_name = "HOKUTO_CHECK_" + std::to_string(model);
    const std::optional<std::vector<uint32_t>> banks =
        bank ? std::optional(std::vector<uint32_t>{sar::ItemId(sar::kBankItem, *bank)}) : std::nullopt;
    const auto add_sound = [&]
    {
        return edit::AddWiiSequenceSound(file, model, sound_name, source.incoming.sequence, *source.start, {}, banks,
                                         groups);
    };
    const format::File added = Refusable(add_sound);
    totals.imports++;
    const std::vector<uint8_t> after_bytes = format::ExportArchive(added);
    const WiiArchive after = song::LoadWiiArchive(after_bytes, "after.brsar", ntdlib::rvl::snd::kRsarOptions);
    const ntdlib::rsar::ArchiveModel now = ntdlib::rsar::ReadArchiveModel(after_bytes);

    // The new sound holds the model's values, with the new file, its start and the tracks that its commands open. The
    // groups that load the model hold the new file.
    const auto id = static_cast<uint32_t>(now.files.size() - 1);
    const auto sound = static_cast<uint32_t>(now.sounds.size() - 1);
    ntdlib::rsar::SoundModel expected = now.sounds.at(model);
    expected.name = sound_name;
    expected.file_id = id;
    expected.start_offset = now.sounds.back().start_offset;
    expected.bank = bank.value_or(expected.bank);
    expected.allocate_track_flags = edit::TracksOpened(source.incoming.sequence, *source.start);
    if (sound != before.sounds.size())
    {
        return "adds more than one sound";
    }
    if (now.sounds.back() != expected)
    {
        return "doesn't give the new sound the model's values";
    }
    for (const uint32_t g : groups)
    {
        totals.joins++;
        if (std::ranges::find(now.groups[g].file_ids, id) == now.groups[g].file_ids.end())
        {
            return "leaves the new file out of " + now.groups[g].name;
        }
    }
    std::vector<uint32_t> joining = {id};
    if (bank)
    {
        joining.push_back(now.banks.at(*bank).file_id);
    }
    if (std::string problem = WiiOthersProblem(before, now, joining); !problem.empty())
    {
        return problem;
    }

    // The file's sequences keep their numbers. The new sound plays the model's notes, or its samples with the copy of
    // its bank.
    for (std::size_t i = 0; i < file.sequences.size(); i++)
    {
        if (added.sequences[i] != file.sequences[i])
        {
            return "changes the file's sequence " + std::to_string(i);
        }
    }
    if (!bank && WiiPlayed(*after, sound, build) != WiiPlayed(built, model, build))
    {
        return "doesn't play the model's notes";
    }
    if (bank)
    {
        const std::atomic<bool> kCancel{false};
        const auto render = [&build, &kCancel](const WiiArchive& archive, uint32_t which)
        {
            const auto sequence =
                std::make_shared<const song::Sequence>(song::ReadSequence(*archive, which, {}, {}, build));
            return song::RenderDry(archive, sequence, song::Mixing::kStrict, 10.0, kCancel).samples;
        };

        const WiiArchive original =
            song::LoadWiiArchive(format::ExportArchive(file), "before.brsar", ntdlib::rvl::snd::kRsarOptions);
        const std::vector<float> samples = render(original, model);
        if (render(after, sound) != samples)
        {
            return "doesn't play the model's samples with the copy of its bank";
        }
        totals.sounding += std::ranges::any_of(samples, [](float sample) { return sample != 0.0f; }) ? 1 : 0;
    }
    if (save && !hokuto::tools::SavesAndOpens(added, true))
    {
        return "doesn't save and open again";
    }

    return {};
}

// Prints `problem` of `item` in archive `name`, and counts the failure in `totals`.
void Fail(const std::string& name, const std::string& item, const std::string& problem, WiiImportTotals& totals)
{
    std::printf("%s: %s %s\n", name.c_str(), item.c_str(), problem.c_str());
    totals.failed++;
}

// Runs `check` of `item` in archive `name`. Prints the problem that it returns or throws (Fail), and counts a refusal
// of the edit in `totals`.
template <typename Check>
void Attempt(const std::string& name, const std::string& item, const Check& check, WiiImportTotals& totals)
{
    try
    {
        if (const std::string problem = check(); !problem.empty())
        {
            Fail(name, item, problem, totals);
        }
    }
    catch (const Refusal& error)
    {
        totals.refused[error.what()]++;
    }
    catch (const std::exception& error)
    {
        Fail(name, item, std::string("fails: ") + error.what(), totals);
    }
}

// The source of sound `model` of `built`, or nothing for a model that starts where no command of its file starts or
// that opens tracks that it doesn't allocate. Counts those models in `totals`.
std::optional<Source> ModelSource(const WiiArchive& built, const ntdlib::SndBuild& build, uint32_t model,
                                  WiiImportTotals& totals)
{
    const ntdlib::rsar::SoundInfo& info = built->Sounds()[model];
    Source source = WiiSourceOf(built->FileData(info.file_id), info.start_offset, build);
    if (!source.start)
    {
        totals.without_start++;
        return std::nullopt;
    }
    if (Untracked(source, info))
    {
        totals.untracked++;
        return std::nullopt;
    }

    return source;
}

// The new sounds of --new in archive `name`, whose model is `before`.
void ImportNewSounds(const format::File& file, const WiiArchive& built, const ntdlib::rsar::ArchiveModel& before,
                     const ntdlib::SndBuild& build, const std::string& name, WiiImportTotals& totals)
{
    // Up to 32 sequence sounds serve as models. Each new sound plays its model's file from the model's start.
    bool first = true;
    for (const uint32_t model : WiiModels(*built, ntdlib::rsar::SoundType::kSequence, 32))
    {
        const std::optional<Source> source = ModelSource(built, build, model, totals);
        if (source)
        {
            const auto check_sound = [&]
            {
                return WiiNewSoundProblem(file, *built, before, build, model, *source, {}, first, totals);
            };
            Attempt(name, built->Sounds()[model].name, check_sound, totals);
            first = false;
        }
    }
}

// The new banks of --banks in archive `name`, whose model is `before`.
void ImportBanks(const format::File& file, const WiiArchive& built, const ntdlib::rsar::ArchiveModel& before,
                 const ntdlib::SndBuild& build, const std::string& name, WiiImportTotals& totals)
{
    // Up to 8 sequence sounds serve as models. Each one's bank goes in again with its wave data, and a copy of the
    // model plays the new bank.
    for (const uint32_t model : WiiModels(*built, ntdlib::rsar::SoundType::kSequence, 8))
    {
        const ntdlib::rsar::SoundInfo& info = built->Sounds()[model];
        const std::optional<Source> source = ModelSource(built, build, model, totals);
        const std::optional<ntdlib::rsar::FileData> data =
            info.bank < built->Banks().size() ? built->File(built->Banks()[info.bank].file_id) : std::nullopt;
        if (!source || !data)
        {
            continue;
        }
        Attempt(
            name, built->Sounds()[model].name,
            [&]
            {
                const uint32_t bank_file = built->Banks()[info.bank].file_id;
                const auto add_bank = [&]
                {
                    return edit::AddWiiBank(file, "HOKUTO_CHECK_BANK", data->file, data->wave_data,
                                            WiiGroupsHolding(file, bank_file));
                };
                const format::File with_bank = Refusable(add_bank);
                hokuto::tools::CheckSaved(with_bank, true);
                const ntdlib::rsar::ArchiveModel banked =
                    ntdlib::rsar::ReadArchiveModel(format::ExportArchive(with_bank));
                const auto id = static_cast<uint32_t>(before.files.size());
                if (banked.banks.size() != before.banks.size() + 1 || banked.banks.back().file_id != id ||
                    banked.files.at(id).bytes != before.files.at(bank_file).bytes ||
                    banked.files.at(id).wave_data != before.files.at(bank_file).wave_data)
                {
                    return std::string("doesn't hold the copy of its bank");
                }
                if (std::string problem = WiiOthersProblem(before, banked, std::span(&id, 1)); !problem.empty())
                {
                    return problem;
                }
                const WiiArchive with = song::LoadWiiArchive(format::ExportArchive(with_bank), "bank.brsar",
                                                             ntdlib::rvl::snd::kRsarOptions);
                return WiiNewSoundProblem(with_bank, *with, banked, build, model, *source,
                                          static_cast<uint32_t>(before.banks.size()), false, totals);
            },
            totals);
    }
}

// What is wrong when the file `data` of wave sound `model`, whose entry is `info`, is imported again with sound
// `copied` as the model, or nothing. `before` is the model of `built`.
std::string ImportWaveSoundFile(const format::File& file, const WiiArchive& built,
                                const ntdlib::rsar::ArchiveModel& before, const ntdlib::rsar::SoundInfo& info,
                                uint32_t model, uint32_t copied, const std::optional<ntdlib::rsar::FileData>& data,
                                WiiImportTotals& totals)
{
    const std::vector<uint32_t> groups = WiiGroupsHolding(file, built->Sounds()[copied].file_id);
    const auto add_wave_sounds = [&]
    {
        return edit::AddWiiWaveSounds(file, copied, "HOKUTO_CHECK_WSD", data->file, data->wave_data, groups);
    };
    const format::File added = Refusable(add_wave_sounds);
    hokuto::tools::CheckSaved(added, true);
    totals.imports++;
    const std::vector<uint8_t> after_bytes = format::ExportArchive(added);
    const WiiArchive after = song::LoadWiiArchive(after_bytes, "after.brsar", ntdlib::rvl::snd::kRsarOptions);
    const ntdlib::rsar::ArchiveModel now = ntdlib::rsar::ReadArchiveModel(after_bytes);
    const auto id = static_cast<uint32_t>(before.files.size());
    const std::size_t count =
        ntdlib::rsar::WaveSoundFile::Parse(data->file, ntdlib::rvl::snd::kRsarOptions).Sounds().size();
    if (now.sounds.size() != before.sounds.size() + count)
    {
        return std::string("adds another number of sounds");
    }
    totals.sounds += count;
    totals.joins += groups.size();
    for (std::size_t k = 0; k < count; k++)
    {
        const ntdlib::rsar::SoundModel& sound = now.sounds[before.sounds.size() + k];
        const ntdlib::rsar::SoundModel& like = before.sounds[copied];
        if (sound.type != ntdlib::rsar::SoundType::kWaveSound || sound.file_id != id ||
            sound.wave_sound_index != static_cast<int32_t>(k) || sound.volume != like.volume ||
            sound.player_id != like.player_id || sound.player_priority != like.player_priority ||
            sound.channel_priority != like.channel_priority || sound.settings_3d != like.settings_3d)
        {
            return "doesn't give new sound " + std::to_string(k) + " the model's values";
        }
    }
    for (const uint32_t g : groups)
    {
        if (std::ranges::find(now.groups[g].file_ids, id) == now.groups[g].file_ids.end())
        {
            return "leaves the new file out of " + now.groups[g].name;
        }
    }
    if (std::string problem = WiiOthersProblem(before, now, std::span(&id, 1)); !problem.empty())
    {
        return problem;
    }
    const auto placed = static_cast<uint32_t>(before.sounds.size()) + static_cast<uint32_t>(info.sub_index);
    if (song::DecodeWaveSound(*after, placed) != song::DecodeWaveSound(*built, model))
    {
        return std::string("doesn't play the wave sound's wave");
    }

    return std::string();
}

// The wave sounds' files of --wave-sounds in archive `name`, whose model is `before`.
void ImportWaveSoundFiles(const format::File& file, const WiiArchive& built, const ntdlib::rsar::ArchiveModel& before,
                          const std::string& name, WiiImportTotals& totals)
{
    // Up to 8 wave sounds' files go in again, once with the wave sound as the model, and once with the archive's
    // first sequence sound. The new sound in the wave sound's place in the file plays the wave sound's wave.
    const std::vector<uint32_t> sequences = WiiModels(*built, ntdlib::rsar::SoundType::kSequence, 1);
    for (const uint32_t model : WiiModels(*built, ntdlib::rsar::SoundType::kWaveSound, 8))
    {
        const ntdlib::rsar::SoundInfo& info = built->Sounds()[model];
        const std::optional<ntdlib::rsar::FileData> data = built->File(info.file_id);
        if (!data)
        {
            continue;
        }

        std::vector<uint32_t> models = {model};
        models.insert(models.end(), sequences.begin(), sequences.end());
        for (const uint32_t copied : models)
        {
            const auto import_file = [&]
            {
                return ImportWaveSoundFile(file, built, before, info, model, copied, data, totals);
            };
            Attempt(name, info.name, import_file, totals);
        }
    }
}

// What is wrong when the first wave of `wave_archive` is imported again after the waves of `built`, from its wave file
// and from its WAV file, or nothing. `built_bytes` are the bytes of `built`.
std::string ImportFirstWave(const format::File& file, const WiiArchive& built, std::span<const uint8_t> built_bytes,
                            const song::WiiWaveArchive& wave_archive, WiiImportTotals& totals)
{
    const std::vector<ntdlib::rsar::WaveFile> present =
        Refusable([&] { return edit::WavesOf(*built, wave_archive.file_id); });
    if (present.empty())
    {
        return std::string();
    }
    const ntdlib::rsar::WaveFile& old = present.front();
    for (const bool wav : {false, true})
    {
        const edit::IncomingWave incoming =
            wav ? edit::ReadIncomingWiiWave(song::WaveAsWav(*built, wave_archive.file_id, 0), old.version,
                                            sar::WaveEncoding::kPcm16)
                : edit::ReadIncomingWiiWaveFile(old.Write());
        const auto add_wave = [&]
        {
            return edit::WithIncomingWiiWave(file, *built, wave_archive.file_id, std::nullopt, incoming);
        };
        const format::File added = Refusable(add_wave);
        hokuto::tools::CheckSaved(added, true);
        totals.imports++;
        const std::vector<uint8_t> after_bytes = format::ExportArchive(added);
        const WiiArchive after = song::LoadWiiArchive(after_bytes, "after.brsar", ntdlib::rvl::snd::kRsarOptions);
        const std::vector<ntdlib::rsar::WaveFile> waves = edit::WavesOf(*after, wave_archive.file_id);
        if (waves.size() != present.size() + 1 || !std::equal(present.begin(), present.end(), waves.begin()))
        {
            return std::string("doesn't keep the waves before the new one");
        }
        const ntdlib::rsar::WaveFile& now = waves.back();
        const bool empty_loop = old.loop && old.loop_start >= old.loop_end;
        if (!wav && now != old)
        {
            return std::string("doesn't take the wave from its wave file");
        }
        if (wav && (now.sample_rate != old.sample_rate || now.channels.size() != old.channels.size() ||
                    now.loop != (old.loop && !empty_loop) || (!empty_loop && now.loop_start != old.loop_start) ||
                    now.loop_end != old.loop_end || now.version != old.version ||
                    edit::DecodedSamples(now, old.loop_end) != edit::DecodedSamples(old, old.loop_end)))
        {
            return std::string("doesn't take the wave from its WAV file");
        }
        if (std::string problem =
                OtherWiiFilesProblem(built_bytes, *built, after_bytes, *after, wave_archive.file_id, true, true);
            !problem.empty())
        {
            return problem;
        }
        totals.waves++;
    }

    return std::string();
}

// The first waves of the wave data in archive `name` (the last mode). `built_bytes` are the bytes of `built`.
void ImportFirstWaves(const format::File& file, const WiiArchive& built, std::span<const uint8_t> built_bytes,
                      const std::string& name, WiiImportTotals& totals)
{
    // The first wave of each wave data goes in again after the waves, from its wave file and from its WAV file.
    for (const song::WiiWaveArchive& wave_archive : song::WaveArchivesOf(*built))
    {
        const auto import_wave = [&]
        {
            return ImportFirstWave(file, built, built_bytes, wave_archive, totals);
        };
        Attempt(name, wave_archive.name, import_wave, totals);
    }
}

} // namespace

int CheckWiiImports(const format::File& file, const WiiArchive& built, std::span<const uint8_t> built_bytes,
                    const ntdlib::SndBuild& build, const std::string& name, const std::string& mode)
{
    // An archive that the tools didn't lay out takes no new item.
    ntdlib::rsar::ArchiveModel before;
    try
    {
        before = ntdlib::rsar::ReadArchiveModel(built_bytes);
    }
    catch (const std::exception& error)
    {
        std::printf("%s: takes no new item: %s\n", name.c_str(), error.what());
        return 0;
    }

    WiiImportTotals totals;
    if (mode == "--new")
    {
        ImportNewSounds(file, built, before, build, name, totals);
    }
    else if (mode == "--banks")
    {
        ImportBanks(file, built, before, build, name, totals);
    }
    else if (mode == "--wave-sounds")
    {
        ImportWaveSoundFiles(file, built, before, name, totals);
    }
    else
    {
        ImportFirstWaves(file, built, built_bytes, name, totals);
    }

    std::printf(
        "%s: %zu imports of the Wii (%s). %zu fail. %zu models start where no command of their files starts, "
        "and %zu open tracks that they don't allocate. %zu groups took a new file. %zu new wave sounds and %zu "
        "new waves. %zu renders of copies with a new bank hold sound.\n",
        name.c_str(), totals.imports, mode.c_str(), totals.failed, totals.without_start, totals.untracked, totals.joins,
        totals.sounds, totals.waves, totals.sounding);
    for (const auto& [reason, count] : totals.refused)
    {
        std::printf("%s: refused %zu times: %s\n", name.c_str(), count, reason.c_str());
    }

    return totals.failed == 0 ? 0 : 1;
}

} // namespace hokuto::tools::replace
