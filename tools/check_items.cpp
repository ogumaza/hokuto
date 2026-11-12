// SPDX-License-Identifier: MIT

// Checks the exports of single items (src/song/items.h) against the archive that they come from. The tool opens a sound
// archive of the 3DS, the Wii U or the Switch twice:
//
// - as the program opens it, with the group files beside it, and
// - as the program imports it into a Hokuto file. The file then builds the archive again, and the exports read that.
//
// Each item exports from the built archive as the browser's Export… exports it: each sequence sound's sequence file,
// each wave sound file under the name of its sound group, each bank, each wave archive and each wave of each wave
// archive. The browser lists the same wave sound files, with the same wave sounds, for both archives. Each file has to
// hold the bytes that the opened archive gives for the item. A wave's file has to start with its format's signature,
// and its header has to give the file's length. Each wave also goes out as a WAV file. The tool reads the WAV file back
// by itself and compares it with ntdlib's decoding of the wave in the opened archive: the channels, the rate, every
// sample and the loop. A wave that both sides fail to decode counts apart, and so does an item whose file the archive
// lacks. Each wave sound goes out as a WAV file too. The tool reads the opened archive's wave sound file with ntdlib,
// finds the wave of the sound's first note, and compares the WAV file with ntdlib's decoding of that wave at the rate
// that the sound's pitch and the note's give. A wave sound that lacks its file, its note or its wave on both sides
// counts apart.
//
// Each bank's instruments also go out as SoundFont 2 files and as SFZ files. They have to be the files that ntdlib's
// converter makes of the bank in the opened archive, with the settings of the archive's console. Each SoundFont has to
// pass seqlib's check. Together, the SoundFonts have to hold a preset for each program that holds an instrument, in
// order, at bank p / 128 and program p % 128. A bank past the format's limits takes several SoundFonts, each with a run
// of programs. The SFZ files have to pass ntdlib's check of a conversion's SFZ files: each sample that they play is
// among the conversion's samples, and each loop lies inside its sample.
//
// Each sequence file also goes out as a file of each other console among the 3DS, the Wii U and the Switch. ntdlib's
// reader of that console has to read it, and that console's player has to reach the commands that the opened archive's
// file holds, at the same offsets. The biquad filters can take other types and values there (song::MapBiquads), and the
// tool counts the files whose filters do. Converted back, the file has to hold the same labels, and the player has to
// reach the same commands again, but for the biquad filters. The tool counts the files that come back with other biquad
// filters, those that come back with another version, and those that come back with other bytes. Otherwise, only bytes
// that no command reaches can differ. The decoder reads what it can of them as commands. The byte order changes what it
// can read there. The 3DS lacks a few commands of the Wii U and the Switch. A file that reaches one has to fail for the
// 3DS, and the tool counts it apart.
//
// Each bank, wave sound file and wave archive also goes out as a file of each other console, with the versions of
// Export…'s files for that console. ntdlib's reader of that console has to read the same values from it: a bank's waves
// and regions, a wave sound file's waves and wave sounds, and each wave's samples, rate, loop and encoding. A wave that
// goes out alone has to give the bytes that the converted wave archive holds for it. Converted back with the versions
// of the archive's files, a bank file and a wave sound file have to give the archive's bytes. So does each wave archive
// whose waves come back whole. A wave can come back with another original loop start: the other console's version may
// hold none. A wave can also come back without the bytes that follow its last samples. The tool counts those apart, and
// checks that nothing else differs.
//
// The import also reads the streams beside the archive, at the paths of its file table. Each stream sound's stream goes
// out once for each file: as the file, as the file of the other console, and as a WAV file. The tool reads the file
// beside the archive by itself. The export has to give its bytes. ntdlib's reader of the other console has to read the
// other console's file in the version that goes to that console (song::ExportVersions), and its decoding has to give
// the same samples. Converted back in the stream's version, the file has to give the bytes beside the archive. A stream
// that leaves the 3DS drops the samples past its end, and an older version holds no original loop. A stream that goes
// to the 3DS or leaves it therefore has to give the same samples instead. Converted back in the version that goes to
// the stream's console, the file has to give the same samples. The WAV file has to hold ntdlib's decoding of the
// stream: every channel, the rate, every sample and the loop. A stream that the folder lacks counts apart. Hokuto's
// export has to fail for it. An AAC stream of the 3DS counts apart too. The system's decoder decodes it, with the loop
// that ntdlib's model reads from the sound's entry. The WAV file has to hold those samples within 2 steps, and the
// loop. The decoder can move a sample by a step from one decoding to the next. Each other console's file has to hold
// DSP-ADPCM at the stream's rate, with the loop and the length. The loop may move forward to a block, and the samples
// up to there then follow its end. Up to the loop's end, the error of the file's decoding has to lie 15 dB or more
// below the samples.
//
// An archive of the Wii takes a check of its own. As in an import, the Hokuto file takes the game's build of NW4R SND
// from the main.dol of the extracted disc that holds the archive. Each item exports from the built archive as the
// browser exports it: each sequence file and each wave sound file once, each bank, and each wave archive that is an
// RWAR, with each of its waves (song::WaveArchivesOf). Each file has to hold the bytes that ntdlib alone gives for the
// item in the opened archive, and a wave's file has to give its length. Wave data that a WAVE block of a bank or a wave
// sound file describes goes out as an RWAR of its waves, and each of its waves as an RWAV file (song::ItemBytes). The
// RWAR has to hold the RWAV files. Each RWAV file has to hold the encoding of the WAVE block's wave in the opened
// archive, and decode to ntdlib's decoding of that wave. The browser lists the same wave sound files and wave archives
// for both archives. Each wave of each wave archive goes out as a WAV file, those that the WAVE block of a bank or a
// wave sound file describes included. The tool compares it with ntdlib's decoding of the wave in the opened archive.
// Each wave sound goes out as a WAV file too. The tool reads the sound's file in the opened archive, with its wave
// data, and finds the wave of the sound's first note. The WAV file has to hold ntdlib's decoding of that wave at the
// rate that the sound's pitch gives: NW4R SND leaves the note's pitch unread. Each bank's instruments go out as
// SoundFont 2 and SFZ files. BankProblem checks them. The reference converts the bank of the opened archive with the
// game's build, and with the pan mode of the first sequence sound that plays the bank. Each program also plays a note
// as the browser plays it (song::NotePreview), on the built archive. Its output has to be the output of ntdlib's model
// of NW4R SND and AX run directly on the opened archive with the same note and the game's build, up to the preview's
// last frame. A preview that reaches its limit of 10 s fades out in that frame. The tool counts the notes that stay
// silent, and those that reach the limit. The tool leaves out the streams beside a Wii archive, and the Wii's items as
// files of the later consoles. It checks no item of a later console as a file of the Wii either.
// tests/other_console_test.cpp checks those conversions.
//
// The tool prints a line for each item that differs, a line for each warning of the converter but the attenuation that
// it counts, and a summary. It returns 1 when an item differs, and 2 when the archive can't be read.
//
// usage: hokuto_check_items <archive>

#include "check_items.h"

#include "checked_archive.h"

namespace hokuto::tools::items
{

namespace
{

// The wave that wave sound `sound` of `archive` plays. The tool reads it with ntdlib alone: the wave of the sound's
// note 0, decoded, at the wave's rate times the sound's pitch and the note's, rounded to whole hertz. The consoles play
// note 0, and read none of the note events of the sound's tracks. Nothing when the archive lacks the sound's file, its
// note or its wave, and when one of them can't be read.
std::optional<sar::Pcm> WaveSoundPcm(const sar::SoundArchive& archive, const sar::SoundInfo& sound,
                                     const sar::Options& options)
{
    try
    {
        const sar::WaveSoundFile file = sar::WaveSoundFile::Parse(archive.FileData(sound.file_id), options);
        const sar::WaveSoundFile::WaveSound& wave_sound = file.sounds.at(sound.wave_sound_index);
        const sar::VelocityRegion& note = wave_sound.notes.at(0);
        const sar::WaveId& wave = file.waves.at(note.wave_id_index);
        const std::optional<std::size_t> wave_archive =
            sar::ItemIndex(wave.wave_archive_item, sar::kWaveArchiveItem, archive.WaveArchives().size());
        if (!wave_archive)
        {
            return std::nullopt;
        }
        const std::vector<std::span<const uint8_t>> waves =
            sar::WaveArchive::Parse(archive.FileData(archive.WaveArchives()[*wave_archive].file_id), options).waves;
        sar::Pcm pcm = sar::DecodeWave(waves.at(wave.index), options);
        const float pitch = wave_sound.pitch * note.pitch;
        pcm.sample_rate =
            static_cast<uint32_t>(std::lround(static_cast<double>(pcm.sample_rate) * static_cast<double>(pitch)));

        return pcm;
    }
    catch (const std::exception&)
    {
        // The archive lacks the file, the note or the wave, or one of them can't be read.
    }

    return std::nullopt;
}

} // namespace

std::optional<rsar::Pcm> WiiWaveSoundPcm(const rsar::SoundArchive& archive, const rsar::SoundInfo& sound)
{
    try
    {
        const rsar::WaveSoundFile file = rsar::WaveSoundFile::Parse(
            archive.FileData(sound.file_id), archive.WaveData(sound.file_id), ntdlib::rvl::snd::kRsarOptions);
        const rsar::WaveSoundFile::WaveSound& wave_sound = file.Sounds().at(static_cast<std::size_t>(sound.sub_index));
        const int32_t wave = wave_sound.notes.at(0).wave_index;
        rsar::Pcm pcm = rsar::DecodeWave(file.Waves().at(static_cast<std::size_t>(wave)));
        pcm.sample_rate = static_cast<uint32_t>(
            std::lround(static_cast<double>(pcm.sample_rate) * static_cast<double>(wave_sound.pitch)));

        return pcm;
    }
    catch (const std::exception&)
    {
        // The archive lacks the file, the note or the wave, or one of them can't be read.
    }

    return std::nullopt;
}

std::string BankProblem(const export_::BankConversion& ours, const export_::BankConversion& theirs,
                        const std::vector<uint32_t>& programs)
{
    bool same = ours.warnings == theirs.warnings && ours.sound_fonts.size() == theirs.sound_fonts.size() &&
                ours.sfz.files.size() == theirs.sfz.files.size() &&
                ours.sfz.samples.size() == theirs.sfz.samples.size();
    for (std::size_t i = 0; same && i < ours.sound_fonts.size(); i++)
    {
        same = ours.sound_fonts[i].sf2 == theirs.sound_fonts[i].sf2;
    }
    for (std::size_t i = 0; same && i < ours.sfz.files.size(); i++)
    {
        same = ours.sfz.files[i].name == theirs.sfz.files[i].name && ours.sfz.files[i].text == theirs.sfz.files[i].text;
    }
    for (std::size_t i = 0; same && i < ours.sfz.samples.size(); i++)
    {
        same = ours.sfz.samples[i].name == theirs.sfz.samples[i].name &&
               ours.sfz.samples[i].wav == theirs.sfz.samples[i].wav;
    }
    if (!same)
    {
        return "differs from the converter's files";
    }

    // The SoundFonts hold the programs in order, each SoundFont a run from its first program to its last. Program p's
    // preset sits at bank p / 128, program p % 128. The last record ends the list.
    if (ours.sfz.files.size() != programs.size())
    {
        return "has " + std::to_string(ours.sfz.files.size()) + " SFZ files for " + std::to_string(programs.size()) +
               " instruments";
    }
    std::size_t next = 0;
    for (const export_::BankSoundFont& sound_font : ours.sound_fonts)
    {
        const std::vector<std::string> problems = seqlib::sf2::Check(sound_font.sf2);
        if (!problems.empty())
        {
            return "has a broken SoundFont: " + problems[0];
        }

        const seqlib::sf2::Records records = seqlib::sf2::Read(sound_font.sf2);
        const std::size_t presets = records.Count("phdr", 38) - 1;
        if (presets == 0 || next + presets > programs.size() || programs[next] != sound_font.first ||
            programs[next + presets - 1] != sound_font.last)
        {
            return "has a SoundFont of programs " + std::to_string(sound_font.first) + " to " +
                   std::to_string(sound_font.last) + " with " + std::to_string(presets) + " presets";
        }
        for (std::size_t i = 0; i < presets; i++)
        {
            const uint32_t program = programs[next + i];
            const uint8_t* preset = records.Record("phdr", 38, i);
            if (Le16({preset + 20, 2}, 0) != program % 128 || Le16({preset + 22, 2}, 0) != program / 128)
            {
                return "puts program " + std::to_string(program) + " in the wrong place";
            }
        }
        next += presets;
    }
    if (next != programs.size())
    {
        return "leaves " + std::to_string(programs.size() - next) + " instruments out of its SoundFonts";
    }

    // ntdlib's check pairs each SFZ file with a MIDI track of the same name. The first name is the conductor track's.
    std::vector<std::string> names = {""};
    for (const export_::SfzFile& file : ours.sfz.files)
    {
        names.push_back(file.name.substr(0, file.name.size() - 4));
    }
    const std::string sfz = ntdlib::testing::SfzProblem(ours.sfz, names);
    return sfz.empty() ? "" : "has broken SFZ files: " + sfz;
}

} // namespace hokuto::tools::items

namespace
{

namespace export_ = ntdlib::sequence_export;
namespace rsar = ntdlib::rsar;
namespace sar = ntdlib::sar;
namespace song = hokuto::song;

using hokuto::tools::items::AacStreamProblem;
using hokuto::tools::items::BankProblem;
using hokuto::tools::items::CheckWii;
using hokuto::tools::items::Holds;
using hokuto::tools::items::Le32;
using hokuto::tools::items::OtherConsoleFileProblem;
using hokuto::tools::items::OtherConsoleProblem;
using hokuto::tools::items::ReadWav;
using hokuto::tools::items::Refuses;
using hokuto::tools::items::StreamBeside;
using hokuto::tools::items::StreamProblem;
using hokuto::tools::items::Totals;
using hokuto::tools::items::WaveSoundPcm;
using hokuto::tools::items::WavRead;

// The archives of a check and what it counts: `opened`, the archive as ntdlib opens it, `built`, the one that the
// Hokuto file builds, the options that the items' files take, and the names of the files that the export writes.
struct ItemsRun
{
    std::string name;
    std::shared_ptr<const sar::SoundArchive> opened;
    std::shared_ptr<const sar::SoundArchive> built;
    sar::Options options;
    std::set<std::string> names;
    Totals totals;
};

// Checks the file of `item` from the built archive against `theirs`, the bytes that the opened archive's tables give
// it. `waves` is the number of waves that the item's file name counts.
void CheckItemFile(ItemsRun& run, const song::Item& item, std::span<const uint8_t> theirs, std::size_t waves)
{
    // An item whose file the archive lacks has to fail on Hokuto's side too. A group file that the game keeps
    // beside the archive can hold the file, and the archive can come without it.
    const std::string label = song::ItemName(*run.opened, item);
    if (theirs.empty())
    {
        const bool failed = Refuses([&] { song::ItemFile(*run.built, item); });
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

    const std::string signature = song::ItemFormat(item.kind, run.options.flavor).substr(1);
    try
    {
        const std::span<const uint8_t> ours = song::ItemFile(*run.built, item);
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

// Checks the file of each sequence, wave sound file and bank (CheckItemFile).
//
// Each item's file from the built archive against the bytes that the opened archive's tables give it, read with
// ntdlib alone: the file of the item's file ID, or the wave that the wave archive's file holds. Each file starts
// with its format's signature. A sequence file and a wave sound file count once, under the first sound that plays
// them. The hok tool writes them that way too. Names that differ only in case would share a file on macOS and
// Windows.
void CheckItemFiles(ItemsRun& run)
{
    std::set<uint32_t> files;
    for (std::size_t i = 0; i < run.opened->Sounds().size(); i++)
    {
        const sar::SoundInfo& sound = run.opened->Sounds()[i];
        if (sound.type == sar::SoundType::kSequence && files.insert(sound.file_id).second)
        {
            CheckItemFile(run, {song::ItemKind::kSequence, static_cast<uint32_t>(i), 0},
                          run.opened->FileData(sound.file_id), 0);
        }
    }

    const std::vector<song::WaveSoundSet> sets = song::WaveSoundSets(*run.built);
    const std::vector<song::WaveSoundSet> opened_sets = song::WaveSoundSets(*run.opened);
    const auto same_set = [](const song::WaveSoundSet& a, const song::WaveSoundSet& b)
    {
        return a.file_id == b.file_id && a.name == b.name && a.sounds == b.sounds;
    };
    if (!std::ranges::equal(sets, opened_sets, same_set))
    {
        std::printf("%s: the wave sound files differ between the archives\n", run.name.c_str());
        run.totals.differ++;
    }
    for (const song::WaveSoundSet& set : sets)
    {
        CheckItemFile(run, {song::ItemKind::kWaveSoundFile, set.sounds.front(), 0}, run.opened->FileData(set.file_id),
                      0);
        const auto named = [&set](const sar::SoundGroupInfo& group)
        {
            return group.name == set.name;
        };
        run.totals.named_files += std::ranges::any_of(run.opened->SoundGroups(), named) ? 1 : 0;
        run.totals.wave_sound_files++;
    }

    for (std::size_t i = 0; i < run.opened->Banks().size(); i++)
    {
        CheckItemFile(run, {song::ItemKind::kBank, static_cast<uint32_t>(i), 0},
                      run.opened->FileData(run.opened->Banks()[i].file_id), 0);
    }
}

// Each sequence file for each other console, once, under the first sound that plays it (OtherConsoleProblem).
void CheckOtherConsoleSequences(ItemsRun& run)
{
    const std::vector<sar::Flavor> consoles = song::OtherConsoles(run.options.flavor);
    std::set<uint32_t> converted;
    for (std::size_t i = 0; i < run.opened->Sounds().size(); i++)
    {
        const sar::SoundInfo& sound = run.opened->Sounds()[i];
        if (sound.type != sar::SoundType::kSequence || run.opened->FileData(sound.file_id).empty() ||
            !converted.insert(sound.file_id).second)
        {
            continue;
        }

        for (const sar::Flavor other : consoles)
        {
            const std::string problem =
                OtherConsoleProblem(*run.built, *run.opened, static_cast<uint32_t>(i), other, run.totals);
            if (!problem.empty())
            {
                std::printf("%s: %s %s for the %s\n", run.name.c_str(), sound.name.c_str(), problem.c_str(),
                            song::ConsoleName(other).c_str());
                run.totals.differ++;
            }
            run.totals.converted++;
        }
    }
}

// Each bank, wave sound file and wave archive for each other console, once (OtherConsoleFileProblem). An item whose
// file the archive lacks counts above.
void CheckOtherConsoleItems(ItemsRun& run)
{
    const std::vector<sar::Flavor> consoles = song::OtherConsoles(run.options.flavor);
    std::vector<song::Item> others;
    std::set<uint32_t> wave_sound_files;
    for (std::size_t i = 0; i < run.opened->Sounds().size(); i++)
    {
        const sar::SoundInfo& sound = run.opened->Sounds()[i];
        if (sound.type == sar::SoundType::kWaveSound && wave_sound_files.insert(sound.file_id).second)
        {
            others.push_back({song::ItemKind::kWaveSound, static_cast<uint32_t>(i), 0});
        }
    }
    for (std::size_t i = 0; i < run.opened->Banks().size(); i++)
    {
        others.push_back({song::ItemKind::kBank, static_cast<uint32_t>(i), 0});
    }
    for (std::size_t i = 0; i < run.opened->WaveArchives().size(); i++)
    {
        others.push_back({song::ItemKind::kWaveArchive, static_cast<uint32_t>(i), 0});
    }
    for (const song::Item& item : others)
    {
        const bool lacked = Refuses([&] { song::ItemFile(*run.opened, item); });
        for (const sar::Flavor other : consoles)
        {
            const std::string problem =
                lacked ? "" : OtherConsoleFileProblem(*run.built, *run.opened, item, other, run.totals);
            if (!problem.empty())
            {
                std::printf("%s: %s %s for the %s\n", run.name.c_str(), song::ItemName(*run.opened, item).c_str(),
                            problem.c_str(), song::ConsoleName(other).c_str());
                run.totals.differ++;
            }
        }
    }
}

// Each bank's instruments as SoundFont 2 and SFZ files (BankProblem). A bank that ntdlib can't read, or one without
// instruments, has to stop Hokuto's export. A bank whose file the archive lacks counts above.
void CheckBankExports(ItemsRun& run)
{
    for (std::size_t i = 0; i < run.opened->Banks().size(); i++)
    {
        const auto bank = static_cast<uint32_t>(i);
        const std::string& label = run.opened->Banks()[i].name;
        if (run.opened->FileData(run.opened->Banks()[i].file_id).empty())
        {
            continue;
        }

        std::vector<uint32_t> programs;
        bool unread = false;
        try
        {
            programs = song::ProgramsOf(*run.opened, i);
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
                const bool stopped = Refuses([&] { song::ConvertBank(*run.built, bank, "Hokuto"); });
                if (!stopped)
                {
                    std::printf("%s: %s exports without instruments\n", run.name.c_str(), label.c_str());
                    run.totals.differ++;
                }
                (unread ? run.totals.unread_banks : run.totals.empty_banks)++;
                continue;
            }

            const export_::BankConversion ours = song::ConvertBank(*run.built, bank, "Hokuto");
            const export_::BankConversion theirs =
                export_::ConvertBank(*run.opened, i, song::SettingsOf(*run.opened), "Hokuto");
            const std::string problem = BankProblem(ours, theirs, programs);
            if (!problem.empty())
            {
                std::printf("%s: %s %s\n", run.name.c_str(), label.c_str(), problem.c_str());
                run.totals.differ++;
            }

            // The converter's warnings: the attenuation of a bank above volume 127, and what it can't read.
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
    }
}

// Each wave archive, then each of its waves: its file, and its WAV file against ntdlib's decoding.
void CheckWaveArchives(ItemsRun& run)
{
    for (std::size_t i = 0; i < run.opened->WaveArchives().size(); i++)
    {
        const auto archive = static_cast<uint32_t>(i);
        const std::span<const uint8_t> archive_file = run.opened->FileData(run.opened->WaveArchives()[i].file_id);
        CheckItemFile(run, {song::ItemKind::kWaveArchive, archive, 0}, archive_file, 0);
        if (archive_file.empty())
        {
            continue;
        }
        std::vector<std::span<const uint8_t>> waves;
        try
        {
            waves = sar::WaveArchive::Parse(archive_file, run.options).waves;
        }
        catch (const std::exception& error)
        {
            std::printf("%s: %s can't be read: %s\n", run.name.c_str(), run.opened->WaveArchives()[i].name.c_str(),
                        error.what());
            run.totals.differ++;
        }
        for (uint32_t wave = 0; wave < waves.size(); wave++)
        {
            const song::Item item{song::ItemKind::kWave, archive, wave};
            CheckItemFile(run, item, waves[wave], waves.size());
            const std::string label = song::ItemName(*run.opened, item);

            // The wave's file gives its length at 0x0C, after its signature, in the console's byte order.
            const std::span<const uint8_t> wave_file = waves[wave];
            const std::string signature = song::ItemFormat(song::ItemKind::kWave, run.options.flavor).substr(1);
            uint32_t length = 0;
            if (wave_file.size() >= 16)
            {
                length = run.options.flavor == sar::Flavor::kCafe
                             ? static_cast<uint32_t>(wave_file[12]) << 24 | static_cast<uint32_t>(wave_file[13]) << 16 |
                                   static_cast<uint32_t>(wave_file[14]) << 8 | wave_file[15]
                             : Le32(wave_file, 12);
            }
            if (wave_file.size() < 16 || std::string(wave_file.begin(), wave_file.begin() + 4) != signature ||
                length != wave_file.size())
            {
                std::printf("%s: %s isn't a whole wave file\n", run.name.c_str(), label.c_str());
                run.totals.differ++;
            }

            // The WAV file, read back, against the wave that ntdlib decodes from the opened archive.
            run.totals.waves++;
            std::optional<sar::Pcm> decoded;
            try
            {
                decoded = sar::DecodeWave(wave_file, run.options);
            }
            catch (const std::exception&)
            {
                // Hokuto's side has to fail too.
            }
            try
            {
                const std::vector<uint8_t> wav = song::WaveAsWav(*run.built, archive, wave);
                const std::optional<WavRead> read = ReadWav(wav);
                if (!decoded || !read || !Holds(*read, *decoded))
                {
                    std::printf("%s: %s's WAV file differs\n", run.name.c_str(), label.c_str());
                    run.totals.differ++;
                }
                if (song::WaveChannels(*run.built, archive, wave) > 2)
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
}

// Each wave sound as a WAV file, read back, against the wave that the tool reads for it from the opened archive
// (WaveSoundPcm). A wave sound without a file, a note or a wave there has to fail on Hokuto's side too.
void CheckWaveSounds(ItemsRun& run)
{
    for (std::size_t i = 0; i < run.opened->Sounds().size(); i++)
    {
        const sar::SoundInfo& sound = run.opened->Sounds()[i];
        if (sound.type != sar::SoundType::kWaveSound)
        {
            continue;
        }

        run.totals.wave_sounds++;
        const std::optional<sar::Pcm> expected = WaveSoundPcm(*run.opened, sound, run.options);
        try
        {
            const sar::Pcm ours = song::DecodeWaveSound(*run.built, static_cast<uint32_t>(i));
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

// Each stream sound's stream, once for each file (StreamProblem). The Hokuto file keeps the stream by its file ID.
// An AAC stream's loop lies in the sound's entry. ntdlib's model reads it there (AacStreamProblem).
void CheckStreams(ItemsRun& run, const std::filesystem::path& path, const hokuto::format::File& file)
{
    std::set<uint32_t> stream_files;
    std::optional<sar::ArchiveModel> model;
    for (std::size_t i = 0; i < run.opened->Sounds().size(); i++)
    {
        const sar::SoundInfo& sound = run.opened->Sounds()[i];
        if (sound.type != sar::SoundType::kStream || !stream_files.insert(sound.file_id).second)
        {
            continue;
        }

        const std::optional<std::vector<uint8_t>> theirs = StreamBeside(*run.opened, sound.file_id, path);
        const auto kept = file.native->streams.find(sound.file_id);
        if (!theirs)
        {
            if (kept != file.native->streams.end())
            {
                std::printf("%s: %s's stream exports without a file\n", run.name.c_str(), sound.name.c_str());
                run.totals.differ++;
            }
            run.totals.lacked_streams++;
            continue;
        }
        if (kept == file.native->streams.end())
        {
            std::printf("%s: %s's stream is missing from the file\n", run.name.c_str(), sound.name.c_str());
            run.totals.differ++;
            continue;
        }

        try
        {
            if (song::IsAdts(*theirs) && !model)
            {
                model = sar::ReadArchiveModel(run.opened->Bytes(), sar::Flavor::kCtr);
            }
            const std::string problem =
                song::IsAdts(*theirs)
                    ? (*kept->second.bytes != *theirs
                           ? std::string("'s file differs")
                           : AacStreamProblem(*kept->second.bytes, *model, static_cast<uint32_t>(i), run.totals))
                    : StreamProblem(*kept->second.bytes, *theirs, run.totals);
            if (!problem.empty())
            {
                std::printf("%s: %s%s\n", run.name.c_str(), sound.name.c_str(), problem.c_str());
                run.totals.differ++;
            }
        }
        catch (const std::exception& error)
        {
            std::printf("%s: %s's stream fails: %s\n", run.name.c_str(), sound.name.c_str(), error.what());
            run.totals.differ++;
        }
        run.totals.streams++;
    }
}

// Prints the summary of `run`, under the archive's name.
void PrintItemTotals(const ItemsRun& run)
{
    std::printf(
        "%s: %zu items checked, and %zu waves as WAV files. %zu differ. %zu lack their files on both sides. %zu "
        "waves can't be decoded, and %zu have more than two channels. %zu names clash.\n",
        run.name.c_str(), run.totals.items, run.totals.waves, run.totals.differ, run.totals.missing,
        run.totals.undecoded, run.totals.wide, run.totals.clashes);
    std::printf(
        "%s: %zu wave sounds as WAV files, %zu of them at another rate than their wave's. %zu lack their file, their "
        "note or their wave on both sides.\n",
        run.name.c_str(), run.totals.wave_sounds, run.totals.pitched, run.totals.silent);
    std::printf("%s: %zu wave sound files, %zu of them under the names of their sound groups.\n", run.name.c_str(),
                run.totals.wave_sound_files, run.totals.named_files);
    std::printf(
        "%s: %zu banks' instruments checked, %zu instruments in all. %zu banks hold no instruments, and %zu can't be "
        "read. %zu need several SoundFonts. %zu take an attenuation in their SoundFont for a region above volume 127. "
        "The converter warned %zu times more.\n",
        run.name.c_str(), run.totals.banks, run.totals.instruments, run.totals.empty_banks, run.totals.unread_banks,
        run.totals.split_banks, run.totals.louder_banks, run.totals.bank_warnings);
    std::printf(
        "%s: %zu conversions of sequence files for another console and back. %zu take other biquad filters there, and "
        "%zu come back with other biquad filters. %zu come back with another version. %zu come back with other bytes "
        "where no command reaches. %zu reach a command that the 3DS lacks, and fail for it. %zu hold commands that "
        "overlap, and fail for every console.\n",
        run.name.c_str(), run.totals.converted, run.totals.biquads, run.totals.biquads_back, run.totals.versions,
        run.totals.unreached, run.totals.refused, run.totals.overlapping);
    std::printf(
        "%s: %zu conversions of banks, wave sound files and wave archives for another console and back, with %zu "
        "waves. %zu files and %zu waves take another version there. %zu waves come back with another original loop "
        "start, and %zu without the bytes after their last samples.\n",
        run.name.c_str(), run.totals.converted_files, run.totals.converted_waves, run.totals.file_versions,
        run.totals.wave_versions, run.totals.loop_starts, run.totals.trimmed);
    std::printf(
        "%s: %zu streams checked as files, as files of the other consoles and as WAV files, with %zu channels and %zu "
        "conversions. %zu of them are AAC. %zu lie outside the archive's folder or are missing from it.\n",
        run.name.c_str(), run.totals.streams, run.totals.stream_channels, run.totals.stream_conversions,
        run.totals.aac_streams, run.totals.lacked_streams);
}

// The archive of the Switch, the Wii U or the 3DS as it opens, and the Hokuto file that it makes (OpenItemsFile).
struct ItemsFile
{
    std::shared_ptr<const sar::SoundArchive> opened;
    std::shared_ptr<const sar::SoundArchive> built;
    hokuto::format::File file;
};

// Opens the archive at `path`, in `bytes`, and builds the archive of its Hokuto file. Both have the group files beside
// the archive. Throws where the archive can't be read.
ItemsFile OpenItemsFile(const std::filesystem::path& path, const std::string& name, std::span<const uint8_t> bytes)
{
    ItemsFile items;
    items.opened = song::OpenArchive(path);
    items.file = hokuto::format::ImportArchive(bytes, items.opened->ReadOptions().flavor, items.opened->GroupFiles(),
                                               song::ReadStreams(*items.opened, path), items.opened->GroupFileGroups());
    items.built = song::LoadArchive(hokuto::format::ExportArchive(items.file), name,
                                    hokuto::format::ExportGroupFiles(items.file));

    return items;
}

} // namespace

int main(int argc, char** argv)
{
    const std::vector<std::string> arguments = xsflib::console::Arguments(argc, argv);
    if (arguments.size() != 2)
    {
        std::fprintf(stderr, "usage: hokuto_check_items <archive>\n");
        return 2;
    }

    // The opened archive, and the one that the Hokuto file builds. Both have the group files beside the archive. An
    // archive of the Wii takes a check of its own.
    const std::filesystem::path path = xsflib::PathFromUtf8(arguments[1]);
    const std::string name = xsflib::Utf8(path.filename());
    ItemsFile items;
    try
    {
        std::vector<uint8_t> bytes;
        if (!xsflib::ReadFile(path, bytes))
        {
            std::fprintf(stderr, "%s can't be read\n", name.c_str());
            return 2;
        }
        if (hokuto::tools::IsWiiArchive(bytes))
        {
            return CheckWii(path, bytes);
        }
        items = OpenItemsFile(path, name, bytes);
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "%s: %s\n", name.c_str(), error.what());
        return 2;
    }

    // A step that throws ends the run with a line, the totals so far and the status of an archive that can't be read.
    ItemsRun run{name, items.opened, items.built, song::ItemOptions(*items.opened), {}, {}};
    try
    {
        CheckItemFiles(run);
        CheckOtherConsoleSequences(run);
        CheckOtherConsoleItems(run);
        CheckBankExports(run);
        CheckWaveArchives(run);
        CheckWaveSounds(run);
        CheckStreams(run, path, items.file);
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "%s: %s\n", name.c_str(), error.what());
        PrintItemTotals(run);
        return 2;
    }
    PrintItemTotals(run);

    return run.totals.differ > 0 ? 1 : 0;
}
