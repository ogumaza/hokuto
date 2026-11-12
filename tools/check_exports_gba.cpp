// SPDX-License-Identifier: MIT

// hokuto_check_exports: the checks of the songs of GBA ROMs.

#include "check_exports.h"

namespace hokuto::tools::exports
{

namespace
{

// The programs of the presets in bank 0 of the SoundFont `sf2`.
std::set<int> PresetPrograms(const std::vector<uint8_t>& sf2)
{
    // Each preset header uses 38 bytes: its name, its program and its bank. The last one ends the list.
    const seqlib::sf2::Records records = seqlib::sf2::Read(sf2);
    std::set<int> programs;
    for (std::size_t i = 0; i + 1 < records.Count("phdr", 38); i++)
    {
        const uint8_t* header = records.Record("phdr", 38, i);
        if (xsflib::LoadLe16(header + 22) == 0)
        {
            programs.insert(xsflib::LoadLe16(header + 20));
        }
    }

    return programs;
}

// Checks the conversion of song `song`, named `song_name`, of `ours_rom` against the converter's of `theirs_rom`, with
// the converter's `settings` and the name `program`. Prints a line for a song that differs, and counts it in `counts`.
// Notes the programs at which the song's SoundFont contains a preset in bank 0 in `song_presets`.
void CheckGbaSong(const hokuto::song::GbaRom& theirs_rom, const hokuto::song::GbaRom& ours_rom, int song,
                  const std::string& song_name, const ntdlib::sequence_export::agb::ConvertSettings& settings,
                  const std::string& program, const std::string& name, Counts& counts,
                  std::map<int, std::set<int>>& song_presets)
{
    counts.sounds++;
    const ntdlib::sequence_export::mp2k::SongFiles theirs =
        ntdlib::sequence_export::mp2k::ConvertSongFiles(theirs_rom.rom, theirs_rom.info, song, settings);
    if (theirs.report.ok && !theirs.sf2.empty())
    {
        song_presets[song] = PresetPrograms(theirs.sf2);
    }
    Side ours;
    try
    {
        ours.conversion = hokuto::song::ConvertToMidi(ours_rom, song, program);
    }
    catch (const std::exception& error)
    {
        ours.error = error.what();
    }

    // A song that plays no notes, or that the converter can't convert, has to fail on Hokuto's side too. The song
    // counts in `failed` only when both sides fail.
    if (!theirs.report.ok || theirs.report.silent)
    {
        if (ours.conversion)
        {
            std::printf("%s: %s fails only in ntdlib's converter\n", name.c_str(), song_name.c_str());
            counts.differ++;
        }
        else
        {
            counts.failed++;
        }
        return;
    }
    if (!ours.conversion)
    {
        std::printf("%s: %s fails only in Hokuto: %s\n", name.c_str(), song_name.c_str(), ours.error.c_str());
        counts.differ++;
        return;
    }

    std::vector<std::string> parts;
    if (theirs.midi != ours.conversion->midi)
    {
        parts.emplace_back("the MIDI file");
    }
    if (theirs.sf2 != ours.conversion->sf2)
    {
        parts.emplace_back("the SoundFont 2 file");
    }
    if (theirs.report.warnings != ours.conversion->warnings)
    {
        parts.emplace_back("the warnings");
    }
    if (!parts.empty())
    {
        std::printf("%s: %s differs in %s\n", name.c_str(), song_name.c_str(), Listed(parts).c_str());
        counts.differ++;
    }
}

// The programs that the performer selects in each song of `theirs_rom`, with the converter's `settings`.
std::map<int, std::set<int>> SelectedPrograms(const hokuto::song::GbaRom& theirs_rom,
                                              const ntdlib::sequence_export::agb::ConvertSettings& settings)
{
    std::map<int, std::set<int>> selected;
    for (const int song : hokuto::song::GbaSongs(theirs_rom))
    {
        const ntdlib::sequence_export::mp2k::Performance performance =
            ntdlib::sequence_export::mp2k::PerformSong(theirs_rom.rom, theirs_rom.info, song, settings);
        for (const std::vector<seqlib::midi::TrackEvent>& events : performance.tracks)
        {
            for (const seqlib::midi::TrackEvent& event : events)
            {
                if (event.kind == seqlib::midi::EventKind::kPreset)
                {
                    selected[song].insert(static_cast<int>(event.preset));
                }
            }
        }
    }

    return selected;
}

// Checks the conversion of each voice group of `ours_rom`: its programs contain the programs that its songs select
// (`selected`) and the presets of the SoundFonts of its songs (`song_presets`), and each program has a preset or a
// warning. Prints the summary, and returns how many groups differ.
std::size_t CheckVoiceGroups(const hokuto::song::GbaRom& ours_rom, std::map<int, std::set<int>>& selected,
                             std::map<int, std::set<int>>& song_presets, const std::string& name)
{
    std::size_t groups = 0;
    std::size_t programs = 0;
    std::size_t presets = 0;
    std::size_t silent = 0;
    std::size_t unplayed = 0;
    std::size_t group_differ = 0;
    for (const hokuto::song::GbaVoiceGroup& group : hokuto::song::GbaVoiceGroups(ours_rom))
    {
        groups++;
        programs += group.programs.size();
        const std::string group_name = hokuto::song::GbaVoiceGroupName(group.address);
        const std::set<int> listed(group.programs.begin(), group.programs.end());
        std::set<int> played;
        std::set<int> wanted;
        bool differs = false;
        for (const int song : group.songs)
        {
            played.insert(selected[song].begin(), selected[song].end());
            wanted.insert(song_presets[song].begin(), song_presets[song].end());
        }
        for (const int program : played)
        {
            if (!listed.contains(program))
            {
                std::printf("%s: %s has no program %d. A song selects it.\n", name.c_str(), group_name.c_str(),
                            program);
                differs = true;
            }
        }
        for (const int program : listed)
        {
            unplayed += played.contains(program) ? 0 : 1;
        }

        std::set<int> held;
        std::vector<std::string> warnings;
        try
        {
            const ntdlib::sequence_export::BankConversion conversion =
                hokuto::song::ConvertVoiceGroup(ours_rom, group.address, group.programs, "Hokuto");
            held = PresetPrograms(conversion.sound_fonts.at(0).sf2);
            warnings = conversion.warnings;
        }
        catch (const hokuto::song::BankWithoutInstruments&)
        {
            // Each program has to have warned. The conversion keeps no warnings then: the checks below count them.
        }
        catch (const std::exception& error)
        {
            std::printf("%s: %s fails: %s\n", name.c_str(), group_name.c_str(), error.what());
            differs = true;
        }
        presets += held.size();
        for (const int program : wanted)
        {
            if (!held.contains(program))
            {
                std::printf("%s: %s has no preset at program %d. The SoundFont of a song holds one.\n", name.c_str(),
                            group_name.c_str(), program);
                differs = true;
            }
        }
        for (const int program : listed)
        {
            // A group without a preset throws. Its warnings stay behind.
            const std::string warning = "voice " + std::to_string(program) + " has no samples to play";
            const bool warned = held.empty() || std::ranges::find(warnings, warning) != warnings.end();
            if (held.contains(program) == warned)
            {
                std::printf("%s: %s has %s for program %d\n", name.c_str(), group_name.c_str(),
                            warned ? "a preset and a warning" : "neither a preset nor a warning", program);
                differs = true;
            }
            silent += held.contains(program) ? 0 : 1;
        }
        group_differ += differs ? 1 : 0;
    }
    std::printf(
        "%s: %zu voice groups checked, with %zu programs: %zu presets, %zu voices without samples and %zu "
        "programs that no note plays. %zu groups differ.\n",
        name.c_str(), groups, programs, presets, silent, unplayed, group_differ);

    return group_differ;
}

} // namespace

int CheckGbaRom(std::vector<uint8_t> bytes, const std::string& name, const std::string& only,
                const std::string& program)
{
    std::shared_ptr<const hokuto::song::GbaRom> theirs_rom;
    std::shared_ptr<const hokuto::song::GbaRom> ours_rom;
    try
    {
        const hokuto::format::File file = hokuto::format::ImportGbaRom(bytes);
        ours_rom = hokuto::song::LoadGbaRom(hokuto::format::ExportArchive(file));
        theirs_rom = hokuto::song::LoadGbaRom(std::move(bytes));
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "%s: %s\n", name.c_str(), error.what());
        return 2;
    }

    // The programs at which each song's SoundFont contains a preset in bank 0.
    ntdlib::sequence_export::agb::ConvertSettings settings;
    settings.program = program;
    Counts counts;
    std::map<int, std::set<int>> song_presets;
    for (const int song : hokuto::song::GbaSongs(*theirs_rom))
    {
        const std::string song_name = hokuto::song::GbaSongName(*theirs_rom, song);
        if (!only.empty() && song_name != only)
        {
            continue;
        }

        CheckGbaSong(*theirs_rom, *ours_rom, song, song_name, settings, program, name, counts, song_presets);
    }

    std::printf("%s: %zu songs checked. %zu differ, and %zu fail on both sides.\n", name.c_str(), counts.sounds,
                counts.differ, counts.failed);
    if (!only.empty())
    {
        return counts.differ > 0 ? 1 : 0;
    }

    // The programs that the performer selects in each song.
    std::map<int, std::set<int>> selected = SelectedPrograms(*theirs_rom, settings);

    const std::size_t group_differ = CheckVoiceGroups(*ours_rom, selected, song_presets, name);

    return counts.differ > 0 || group_differ > 0 ? 1 : 0;
}

} // namespace hokuto::tools::exports
