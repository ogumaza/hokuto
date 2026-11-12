// SPDX-License-Identifier: MIT

// The checks of hokuto_check_items on archives of the Wii.

#include "check_items.h"
#include "checked_archive.h"

namespace hokuto::tools::items
{

namespace
{

// Whether the note that the browser plays of program `program` of bank `bank` of the Wii's `built` archive, on `build`
// of NW4R SND, gives the output of ntdlib's model run directly on `opened` with the same note, up to the preview's last
// frame. Counts the note in `totals`.
bool NotePlays(const std::shared_ptr<const rsar::SoundArchive>& built, const rsar::SoundArchive& opened, uint32_t bank,
               uint32_t program, const ntdlib::SndBuild& build, Totals& totals)
{
    song::NotePreview preview(built, bank, program, build);
    const int key = song::NotePreview::KeyOf(rsar::Bank::Parse(opened.FileData(opened.Banks()[bank].file_id)), program);
    std::vector<float> played(static_cast<std::size_t>(ntdlib::rvl::ax::kSamplesPerFrame) * 2);
    std::vector<float> samples;
    for (std::size_t got = 0; (got = preview.Render(played.data(), played.size() / 2)) > 0;)
    {
        samples.insert(samples.end(), played.begin(), played.begin() + static_cast<std::ptrdiff_t>(got * 2));
    }

    // The model run directly, with the preview's note-off and limit.
    ntdlib::rvl::snd::ArchiveModel model;
    model.build = build;
    model.StartWithRenderer(opened);
    ntdlib::rvl::snd::NoteOnInfo info;
    info.prg_no = program;
    info.key = key;
    info.velocity = song::NotePreview::kVelocity;
    std::vector<float> expected;
    if (key >= 0 && model.engine->NoteOn({bank}, 0, info) != nullptr)
    {
        std::vector<int16_t> output;
        for (uint64_t frame = 0; frame < preview.LongestFrames() && model.engine->IsBusy(); frame++)
        {
            if (frame == preview.HoldFrames())
            {
                ntdlib::rvl::snd::ChannelManager& channels = model.engine->Channels();
                const std::vector<ntdlib::nwsnd::ChannelId> places(channels.InUse().begin(), channels.InUse().end());
                for (const ntdlib::nwsnd::ChannelId place : places)
                {
                    channels.At(place).NoteOff(*model.engine);
                }
            }
            output.clear();
            if (!model.RunFrame(output))
            {
                break;
            }
            for (const int16_t sample : output)
            {
                expected.push_back(static_cast<float>(sample) / 32768.0f);
            }
        }
    }

    totals.notes++;
    const std::size_t frame = played.size();
    const bool endless = samples.size() == preview.LongestFrames() * frame;
    totals.endless_notes += endless ? 1 : 0;
    totals.silent_notes += std::ranges::all_of(samples, [](float sample) { return sample == 0.0f; }) ? 1 : 0;
    const std::size_t compared = endless ? samples.size() - frame : samples.size();
    return samples.size() == expected.size() &&
           std::equal(samples.begin(), samples.begin() + static_cast<std::ptrdiff_t>(compared), expected.begin());
}

// Checks the RWAV file that `built` exports for `item`, wave `item.wave` of a wave archive whose waves a WAVE block
// describes, against `block_rwavs`, the RWAV files of the RWAR that the wave data of the archive is exported as, and
// `wave`, the WAVE block's wave. `count` is the number of waves of the archive, `name` the name of the archive and
// `label` the label of the wave. The name of the file is added to `names`.
void CheckBlockRwav(const rsar::SoundArchive& built, const song::Item& item,
                    const std::vector<std::span<const uint8_t>>& block_rwavs, const rsar::Wave& wave, std::size_t count,
                    const std::string& name, const std::string& label, std::set<std::string>& names, Totals& totals)
{
    try
    {
        const std::vector<uint8_t> rwav = song::ItemBytes(built, item);
        totals.items++;
        const rsar::Wave read = rsar::Wave::ParseFile(rwav);
        if (item.wave >= block_rwavs.size() || !std::ranges::equal(rwav, block_rwavs[item.wave]) ||
            read.encoding != wave.encoding || rsar::DecodeWave(read) != rsar::DecodeWave(wave))
        {
            std::printf("%s: %s's wave file differs\n", name.c_str(), label.c_str());
            totals.differ++;
        }
        if (!names.insert(xsflib::AsciiLower(song::ItemFileName(built, item, count))).second)
        {
            totals.clashes++;
        }
    }
    catch (const std::exception& error)
    {
        std::printf("%s: %s's wave file can't be exported: %s\n", name.c_str(), label.c_str(), error.what());
        totals.differ++;
    }
}

// The archives of a check of a Wii archive and what it counts: `opened`, the archive as the program opens it, `built`,
// the one that the Hokuto file builds, and the names of the files that the export writes.
struct WiiItemsRun
{
    std::string name;
    std::shared_ptr<const rsar::SoundArchive> opened;
    std::shared_ptr<const rsar::SoundArchive> built;
    std::set<std::string> names;
    Totals totals;
};

// Checks the file of `item` from the built archive against `theirs`, the bytes that ntdlib alone gives for it in the
// opened archive. `waves` is the number of waves that the item's file name counts.
void CheckWiiItemFile(WiiItemsRun& run, const song::Item& item, std::span<const uint8_t> theirs, std::size_t waves)
{
    const std::string label = song::ItemName(*run.opened, item);
    if (theirs.empty())
    {
        const bool failed = Refuses([&] { song::ItemBytes(*run.built, item); });
        if (!failed)
        {
            std::printf("%s: %s exports without a file\n", run.name.c_str(), label.c_str());
            run.totals.differ++;
        }
        else
        {
            run.totals.missing++;
        }
        return;
    }

    const std::string signature = song::WiiItemFormat(item.kind).substr(1);
    try
    {
        const std::vector<uint8_t> ours = song::ItemBytes(*run.built, item);
        run.totals.items++;
        if (!std::equal(ours.begin(), ours.end(), theirs.begin(), theirs.end()) || ours.size() < 4 ||
            std::string(ours.begin(), ours.begin() + 4) != signature)
        {
            std::printf("%s: %s differs\n", run.name.c_str(), label.c_str());
            run.totals.differ++;
        }
        if (!run.names.insert(xsflib::AsciiLower(song::ItemFileName(*run.built, item, waves))).second)
        {
            run.totals.clashes++;
        }
    }
    catch (const std::exception& error)
    {
        std::printf("%s: %s can't be exported: %s\n", run.name.c_str(), label.c_str(), error.what());
        run.totals.differ++;
    }
}

// Checks the file of each sequence, wave sound file and bank (CheckWiiItemFile).
void CheckWiiItemFiles(WiiItemsRun& run)
{
    std::set<uint32_t> files;
    for (std::size_t i = 0; i < run.opened->Sounds().size(); i++)
    {
        const rsar::SoundInfo& sound = run.opened->Sounds()[i];
        if (sound.type == rsar::SoundType::kSequence && files.insert(sound.file_id).second)
        {
            CheckWiiItemFile(run, {song::ItemKind::kSequence, static_cast<uint32_t>(i), 0},
                             run.opened->FileData(sound.file_id), 0);
        }
    }

    const std::vector<song::WaveSoundSet> sets = song::WaveSoundSets(*run.built);
    const auto same_set = [](const song::WaveSoundSet& a, const song::WaveSoundSet& b)
    {
        return a.file_id == b.file_id && a.name == b.name && a.sounds == b.sounds;
    };
    if (!std::ranges::equal(sets, song::WaveSoundSets(*run.opened), same_set))
    {
        std::printf("%s: the wave sound files differ between the archives\n", run.name.c_str());
        run.totals.differ++;
    }
    for (const song::WaveSoundSet& set : sets)
    {
        CheckWiiItemFile(run, {song::ItemKind::kWaveSoundFile, set.sounds.front(), 0},
                         run.opened->FileData(set.file_id), 0);
        run.totals.wave_sound_files++;
    }

    for (std::size_t i = 0; i < run.opened->Banks().size(); i++)
    {
        CheckWiiItemFile(run, {song::ItemKind::kBank, static_cast<uint32_t>(i), 0},
                         run.opened->FileData(run.opened->Banks()[i].file_id), 0);
    }
}

// Each bank's instruments as SoundFont 2 and SFZ files (BankProblem). A bank that ntdlib can't read, or one without
// instruments, has to stop Hokuto's export. A bank whose file the archive lacks counts above.
void CheckWiiBankExports(WiiItemsRun& run, const std::optional<ntdlib::SndBuild>& build)
{
    for (std::size_t i = 0; i < run.opened->Banks().size(); i++)
    {
        const auto bank = static_cast<uint32_t>(i);
        const std::string& label = run.opened->Banks()[i].name;
        const std::span<const uint8_t> bank_file = run.opened->FileData(run.opened->Banks()[i].file_id);
        if (bank_file.empty())
        {
            continue;
        }

        std::vector<uint32_t> programs;
        bool unread = false;
        try
        {
            const rsar::Bank parsed = rsar::Bank::Parse(bank_file);
            for (uint32_t program = 0; program < parsed.InstrumentCount(); program++)
            {
                if (parsed.Plays(program))
                {
                    programs.push_back(program);
                }
            }
        }
        catch (const std::exception& error)
        {
            std::printf("%s: %s can't be read: %s\n", run.name.c_str(), label.c_str(), error.what());
            unread = true;
        }
        try
        {
            if (programs.empty())
            {
                const bool stopped = Refuses([&] { song::ConvertBank(*run.built, bank, "Hokuto", build); });
                if (!stopped)
                {
                    std::printf("%s: %s exports without instruments\n", run.name.c_str(), label.c_str());
                    run.totals.differ++;
                }
                (unread ? run.totals.unread_banks : run.totals.empty_banks)++;
                continue;
            }

            // The pan mode of the first sequence sound that plays the bank, or balance mode.
            uint8_t pan_mode = 1;
            for (const rsar::SoundInfo& sound : run.opened->Sounds())
            {
                if (sound.type == rsar::SoundType::kSequence && sound.bank == bank)
                {
                    pan_mode = sound.pan_mode;
                    break;
                }
            }
            const export_::BankConversion ours = song::ConvertBank(*run.built, bank, "Hokuto", build);
            const export_::BankConversion theirs =
                export_::ConvertBank(*run.opened, i, pan_mode, song::SettingsOf(*run.opened, build), "Hokuto");
            const std::string problem = BankProblem(ours, theirs, programs);
            if (!problem.empty())
            {
                std::printf("%s: %s %s\n", run.name.c_str(), label.c_str(), problem.c_str());
                run.totals.differ++;
            }

            for (const std::string& warning : ours.warnings)
            {
                if (warning.starts_with("region volume exceeds 127"))
                {
                    run.totals.louder_banks++;
                    continue;
                }

                std::printf("%s: %s: %s\n", run.name.c_str(), label.c_str(), warning.c_str());
                run.totals.bank_warnings++;
            }

            run.totals.banks++;
            run.totals.instruments += programs.size();
            if (ours.sound_fonts.size() > 1)
            {
                run.totals.split_banks++;
            }
        }
        catch (const std::exception& error)
        {
            std::printf("%s: %s's instruments can't be exported: %s\n", run.name.c_str(), label.c_str(), error.what());
            run.totals.differ++;
        }

        // A note of each program as the browser plays it.
        for (const uint32_t program : programs)
        {
            try
            {
                if (!NotePlays(run.built, *run.opened, bank, program,
                               build.value_or(export_::BuildOfRsarVersion(run.opened->Version())), run.totals))
                {
                    std::printf("%s: %s's note of program %u plays otherwise\n", run.name.c_str(), label.c_str(),
                                program);
                    run.totals.differ++;
                }
            }
            catch (const std::exception& error)
            {
                std::printf("%s: %s's note of program %u fails: %s\n", run.name.c_str(), label.c_str(), program,
                            error.what());
                run.totals.differ++;
            }
        }
    }
}

// The wave archives of a check, and how many of them are RWARs and how many WAVE blocks went out as RWARs.
struct WaveArchiveCounts
{
    std::size_t count = 0;
    std::size_t rwars = 0;
    std::size_t blocks = 0;
};

// Each wave archive, then each of its waves: an RWAR's file and its RWAV files, and each wave's WAV file against
// ntdlib's decoding.
WaveArchiveCounts CheckWiiWaveArchives(WiiItemsRun& run)
{
    const std::vector<song::WiiWaveArchive> wave_archives = song::WaveArchivesOf(*run.built);
    const auto same_wave_archive = [](const song::WiiWaveArchive& a, const song::WiiWaveArchive& b)
    {
        return a.file_id == b.file_id && a.name == b.name && a.rwar == b.rwar;
    };
    if (!std::ranges::equal(wave_archives, song::WaveArchivesOf(*run.opened), same_wave_archive))
    {
        std::printf("%s: the wave archives differ between the archives\n", run.name.c_str());
        run.totals.differ++;
    }
    std::size_t rwars = 0;
    std::size_t blocks = 0;
    for (const song::WiiWaveArchive& wave_archive : wave_archives)
    {
        const uint32_t file_id = wave_archive.file_id;
        const std::span<const uint8_t> wave_data = run.opened->WaveData(file_id);
        std::vector<std::span<const uint8_t>> rwavs;
        std::vector<rsar::Wave> waves;
        try
        {
            if (wave_archive.rwar)
            {
                CheckWiiItemFile(run, {song::ItemKind::kWaveArchive, file_id, 0}, wave_data, 0);
                rwavs = rsar::WaveArchive::Parse(wave_data).waves;
                rwars++;
            }
            else if (const std::span<const uint8_t> file = run.opened->FileData(file_id);
                     file.size() >= 4 && std::string(file.begin(), file.begin() + 4) == "RWSD")
            {
                waves = rsar::WaveSoundFile::Parse(file, wave_data).Waves();
            }
            else
            {
                waves = rsar::Bank::Parse(file, wave_data).Waves();
            }
        }
        catch (const std::exception& error)
        {
            std::printf("%s: the waves of %s can't be read: %s\n", run.name.c_str(), wave_archive.name.c_str(),
                        error.what());
            run.totals.differ++;
        }

        // Wave data that a WAVE block describes goes out as an RWAR of its waves (song::ItemBytes).
        std::vector<uint8_t> block_rwar;
        std::vector<std::span<const uint8_t>> block_rwavs;
        if (!wave_archive.rwar && !waves.empty())
        {
            const song::Item item{song::ItemKind::kWaveArchive, file_id, 0};
            try
            {
                block_rwar = song::ItemBytes(*run.built, item);
                block_rwavs = rsar::WaveArchive::Parse(block_rwar).waves;
                run.totals.items++;
                blocks++;
                if (block_rwavs.size() != waves.size())
                {
                    std::printf("%s: %s's RWAR holds %zu waves, and its WAVE block %zu\n", run.name.c_str(),
                                wave_archive.name.c_str(), block_rwavs.size(), waves.size());
                    run.totals.differ++;
                }
                if (!run.names.insert(xsflib::AsciiLower(song::ItemFileName(*run.built, item, 0))).second)
                {
                    run.totals.clashes++;
                }
            }
            catch (const std::exception& error)
            {
                std::printf("%s: %s can't be exported: %s\n", run.name.c_str(), wave_archive.name.c_str(),
                            error.what());
                run.totals.differ++;
            }
        }
        const std::size_t count = wave_archive.rwar ? rwavs.size() : waves.size();
        for (uint32_t wave = 0; wave < count; wave++)
        {
            // An RWAV file gives its length at 8, after its signature, its byte order mark and its version.
            const song::Item item{song::ItemKind::kWave, file_id, wave};
            const std::string label = song::ItemName(*run.opened, item);
            if (wave_archive.rwar)
            {
                const std::span<const uint8_t> rwav = rwavs[wave];
                CheckWiiItemFile(run, item, rwav, count);
                if (rwav.size() < 16 || std::string(rwav.begin(), rwav.begin() + 4) != "RWAV" ||
                    xsflib::LoadBe32(rwav.data() + 8) != rwav.size())
                {
                    std::printf("%s: %s isn't a whole wave file\n", run.name.c_str(), label.c_str());
                    run.totals.differ++;
                }
            }
            else if (!block_rwar.empty())
            {
                // The wave's RWAV file has to be the RWAR's, with the encoding and the samples of the WAVE block's
                // wave.
                CheckBlockRwav(*run.built, item, block_rwavs, waves[wave], count, run.name, label, run.names,
                               run.totals);
            }

            // The WAV file, read back, against the wave that ntdlib decodes from the opened archive.
            run.totals.waves++;
            std::optional<rsar::Pcm> decoded;
            try
            {
                decoded = rsar::DecodeWave(wave_archive.rwar ? rsar::Wave::ParseFile(rwavs[wave]) : waves[wave]);
            }
            catch (const std::exception&)
            {
                // Hokuto's side has to fail too.
            }
            try
            {
                const std::vector<uint8_t> wav = song::WaveAsWav(*run.built, file_id, wave);
                const std::optional<WavRead> read = ReadWav(wav);
                if (!decoded || !read || !Holds(*read, *decoded))
                {
                    std::printf("%s: %s's WAV file differs\n", run.name.c_str(), label.c_str());
                    run.totals.differ++;
                }
                if (song::WaveChannels(*run.built, file_id, wave) > 2)
                {
                    run.totals.wide++;
                }
            }
            catch (const std::exception& error)
            {
                if (decoded)
                {
                    std::printf("%s: %s's WAV file fails: %s\n", run.name.c_str(), label.c_str(), error.what());
                    run.totals.differ++;
                }
                else
                {
                    run.totals.undecoded++;
                }
            }
        }
    }

    return {wave_archives.size(), rwars, blocks};
}

// Each wave sound as a WAV file, read back, against the wave that the tool reads for it from the opened archive
// (WiiWaveSoundPcm). A wave sound without a file, a note or a wave there has to fail on Hokuto's side too.
void CheckWiiWaveSounds(WiiItemsRun& run)
{
    for (std::size_t i = 0; i < run.opened->Sounds().size(); i++)
    {
        const rsar::SoundInfo& sound = run.opened->Sounds()[i];
        if (sound.type != rsar::SoundType::kWaveSound)
        {
            continue;
        }

        run.totals.wave_sounds++;
        const std::optional<rsar::Pcm> expected = WiiWaveSoundPcm(*run.opened, sound);
        try
        {
            const rsar::Pcm ours = song::DecodeWaveSound(*run.built, static_cast<uint32_t>(i));
            const std::optional<WavRead> read = ReadWav(song::WaveSoundAsWav(*run.built, static_cast<uint32_t>(i)));
            if (!expected || !read || !Holds(*read, *expected))
            {
                std::printf("%s: %s's WAV file differs\n", run.name.c_str(), sound.name.c_str());
                run.totals.differ++;
            }
            const song::WaveSoundNote note = song::WaveSoundNoteOf(*run.built, static_cast<uint32_t>(i));
            if (ours.sample_rate != song::DecodeItemWave(*run.built, note.wave_archive, note.wave).sample_rate)
            {
                run.totals.pitched++;
            }
        }
        catch (const std::exception& error)
        {
            if (expected)
            {
                std::printf("%s: %s's WAV file fails: %s\n", run.name.c_str(), sound.name.c_str(), error.what());
                run.totals.differ++;
            }
            else
            {
                run.totals.silent++;
            }
        }
    }
}

} // namespace

int CheckWii(const std::filesystem::path& path, const std::vector<uint8_t>& bytes)
{
    // The opened archive, and the one that the Hokuto file builds, both as the program reads them.
    const std::string name = xsflib::Utf8(path.filename());
    std::shared_ptr<const rsar::SoundArchive> opened;
    std::shared_ptr<const rsar::SoundArchive> built;
    std::optional<ntdlib::SndBuild> build;
    try
    {
        build = hokuto::tools::GameBuildOf(path);
        const hokuto::format::File file = hokuto::format::ImportWiiArchive(bytes, build);
        opened = song::LoadWiiArchive(bytes, name, ntdlib::rvl::snd::kRsarOptions);
        built = song::LoadWiiArchive(hokuto::format::ExportArchive(file), name, ntdlib::rvl::snd::kRsarOptions);
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "%s: %s\n", name.c_str(), error.what());
        return 2;
    }

    // Each item's file from the built archive against the bytes that ntdlib alone gives for it in the opened archive.
    // Each file starts with its format's signature, the format's name without its first letter. An item whose file the
    // opened archive lacks has to fail on Hokuto's side too.
    WiiItemsRun run{name, opened, built, {}, {}};
    CheckWiiItemFiles(run);
    CheckWiiBankExports(run, build);
    const WaveArchiveCounts waves = CheckWiiWaveArchives(run);
    CheckWiiWaveSounds(run);

    std::printf(
        "%s: %zu items checked, and %zu waves as WAV files. %zu differ. %zu lack their files on both sides. %zu "
        "waves can't be decoded, and %zu have more than two channels. %zu names clash.\n",
        run.name.c_str(), run.totals.items, run.totals.waves, run.totals.differ, run.totals.missing,
        run.totals.undecoded, run.totals.wide, run.totals.clashes);
    std::printf(
        "%s: %zu wave sounds as WAV files, %zu of them at another rate than their wave's. %zu lack their file, their "
        "note or their wave on both sides.\n",
        run.name.c_str(), run.totals.wave_sounds, run.totals.pitched, run.totals.silent);
    std::printf(
        "%s: %zu wave sound files, and %zu wave archives, %zu of them RWARs. %zu WAVE blocks went out as RWARs "
        "of their waves.\n",
        run.name.c_str(), run.totals.wave_sound_files, waves.count, waves.rwars, waves.blocks);
    const std::string build_text = hokuto::tools::BuildText(build);
    std::printf(
        "%s: %zu banks' instruments checked, %zu instruments in all, by NW4R SND of %s. %zu banks hold no "
        "instruments, and %zu can't be read. %zu need several SoundFonts. %zu take an attenuation in their SoundFont "
        "for a region above volume 127. The converter warned %zu times more.\n",
        run.name.c_str(), run.totals.banks, run.totals.instruments, build_text.c_str(), run.totals.empty_banks,
        run.totals.unread_banks, run.totals.split_banks, run.totals.louder_banks, run.totals.bank_warnings);
    std::printf("%s: %zu notes played as the browser plays them. %zu stay silent, and %zu reach the limit of 10 s.\n",
                run.name.c_str(), run.totals.notes, run.totals.silent_notes, run.totals.endless_notes);

    return run.totals.differ > 0 ? 1 : 0;
}

} // namespace hokuto::tools::items
