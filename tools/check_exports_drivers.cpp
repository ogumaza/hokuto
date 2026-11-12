// SPDX-License-Identifier: MIT

// hokuto_check_exports: the checks of the GBA's other drivers.

#include "check_exports.h"

namespace hokuto::tools::exports
{

namespace
{

// The conversion of sound `sound` of `rom`, a GBA ROM with another of the GBA's drivers, with ntdlib's converter of
// that driver, and the name of its files in the reference folder, without the extension: `stem`, an underscore, the
// song's number and the suffix of its version. The SFZ files are those of ntdlib's converter of a driver that writes
// them (DriverRom::WritesSfz). The tool compares no SFZ files with the reference folder. `program` names the program
// that made the files.
struct TheirFiles
{
    bool ok = false;
    bool silent = false;
    std::vector<std::string> warnings;
    std::vector<uint8_t> midi;
    std::vector<uint8_t> sf2;
    export_::SfzInstruments sfz;
    std::string base;
};

TheirFiles TheirFilesOf(const hokuto::song::DriverRom& rom, uint32_t sound, const std::string& stem,
                        const std::string& program)
{
    ntdlib::sequence_export::agb::ConvertSettings settings;
    settings.program = program;
    TheirFiles files;
    const auto take = [&files](auto&& converted)
    {
        files.ok = converted.report.ok;
        files.silent = converted.report.silent;
        files.warnings = std::move(converted.report.warnings);
        files.midi = std::move(converted.midi);
        files.sf2 = std::move(converted.sf2);
    };
    if (const hokuto::song::KonamiRom* konami = hokuto::song::KonamiOf(&rom))
    {
        const bool live = hokuto::song::KonamiSoundIsLive(*konami, sound);
        const int song = hokuto::song::KonamiSongOf(*konami, sound);
        settings.version = live ? 1 : 0;
        ntdlib::sequence_export::konami::SongFiles converted =
            ntdlib::sequence_export::konami::ConvertSongFiles(konami->rom, konami->info, song, settings);
        files.sfz = std::move(converted.sfz);
        take(std::move(converted));
        files.base = stem + "_" + ntdlib::sequence_export::agb::SongNumber(song, 1) + (live ? "_live" : "");
    }
    else if (const hokuto::song::Rd2Rom* rd2 = hokuto::song::Rd2Of(&rom))
    {
        const int song = rd2->SongOf(sound);
        const int version = rd2->VersionOf(sound);
        settings.version = version;
        take(ntdlib::sequence_export::rd2::ConvertSongFiles(rd2->rom, rd2->info, song, settings));
        const std::vector<ntdlib::sequence_export::agb::SongVersion> versions =
            ntdlib::sequence_export::rd2::SongVersions(rd2->info, song);
        const std::size_t picked = std::min(static_cast<std::size_t>(std::max(version, 0)), versions.size() - 1);
        files.base = stem + "_" + ntdlib::sequence_export::agb::SongNumber(song, 0) + versions[picked].suffix;
    }
    else if (const hokuto::song::RareRom* rare = hokuto::song::RareOf(&rom))
    {
        const int song = rare->SongOf(sound);
        const int version = rare->VersionOf(sound);
        settings.version = version;
        take(ntdlib::sequence_export::rare::ConvertSongFiles(rare->rom, rare->info, song, settings));
        const std::vector<ntdlib::sequence_export::agb::SongVersion> versions =
            ntdlib::sequence_export::rare::TuneVersions(rare->info, song);
        const std::size_t picked = std::min(static_cast<std::size_t>(std::max(version, 0)), versions.size() - 1);
        files.base = stem + "_" + ntdlib::sequence_export::agb::SongNumber(song, 0) + versions[picked].suffix;
    }
    else if (const hokuto::song::BrownieRom* brownie = hokuto::song::BrownieOf(&rom))
    {
        const int song = brownie->SongOf(sound);
        take(ntdlib::sequence_export::brownie::ConvertSongFiles(brownie->rom, brownie->info, song, settings));
        files.base = stem + "_" + ntdlib::sequence_export::agb::SongNumber(song, brownie->info.song_count);
    }
    else if (const hokuto::song::UbiMilanRom* milan = hokuto::song::UbiMilanOf(&rom))
    {
        const int song = static_cast<int>(sound);
        take(ntdlib::sequence_export::ubimilan::ConvertSongFiles(milan->rom, milan->info, song, settings));
        files.base =
            stem + "_" + ntdlib::sequence_export::agb::SongNumber(song, static_cast<int>(milan->info.songs.size()));
    }
    else if (const hokuto::song::QuintetRom* quintet = hokuto::song::QuintetOf(&rom))
    {
        const auto file = quintet->FileOf(static_cast<int>(sound));
        if (!file)
        {
            throw std::invalid_argument("The ROM has no song " + std::to_string(sound) + ".");
        }
        take(ntdlib::sequence_export::quintet::ConvertSongFiles(quintet->rom, *file->first, file->second, settings));
        files.base = stem + "_" + ntdlib::sequence_export::agb::SongNumber(static_cast<int>(sound), 0);
    }
    else if (const hokuto::song::KrawallRom* krawall = hokuto::song::KrawallOf(&rom))
    {
        take(ntdlib::sequence_export::krawall::ConvertSongFiles(krawall->ModulesRom(), krawall->info,
                                                                static_cast<int>(sound), settings));
        files.base = stem + "_" + ntdlib::sequence_export::agb::SongNumber(static_cast<int>(sound), 0);
    }
    else
    {
        throw std::invalid_argument("The tool has no converter for this driver.");
    }

    return files;
}

// The reference files and the SFZ files and samples that CheckDriverRom compares.
struct DriverTallies
{
    std::size_t files_read = 0;
    std::size_t sfz_files = 0;
    std::size_t sfz_samples = 0;
};

// Checks the conversion of sound `sound`, named `sound_name`, of `ours_rom` against the converter's of `theirs_rom`,
// and against the reference files where `reference_folder` names a folder. `stem` and `program` are as for
// CheckDriverRom. Prints a line for a sound that differs, and counts it in `counts` and `tallies`.
void CheckDriverSound(const hokuto::song::DriverRom& theirs_rom, const hokuto::song::DriverRom& ours_rom,
                      uint32_t sound, const std::string& sound_name, const std::string& name,
                      const std::filesystem::path& reference_folder, const std::string& stem,
                      const std::string& program, Counts& counts, DriverTallies& tallies)
{
    counts.sounds++;
    const TheirFiles theirs = TheirFilesOf(theirs_rom, sound, stem, program);
    Side ours;
    try
    {
        ours.conversion = ours_rom.ConvertToMidi(sound, program);
    }
    catch (const std::exception& error)
    {
        ours.error = error.what();
    }

    // The reference files of the song, where the folder contains them.
    std::vector<uint8_t> disk_midi;
    std::vector<uint8_t> disk_sf2;
    bool on_disk = false;
    if (!reference_folder.empty())
    {
        on_disk = xsflib::ReadFile(reference_folder / xsflib::PathFromUtf8(theirs.base + ".mid"), disk_midi) &&
                  xsflib::ReadFile(reference_folder / xsflib::PathFromUtf8(theirs.base + ".sf2"), disk_sf2);
        tallies.files_read += on_disk ? 2 : 0;
    }

    // A song that plays no notes, or that the converter can't convert, has to fail on Hokuto's side too. A song counts
    // once in `differ`, and in `failed` only when both sides fail.
    if (!theirs.ok || theirs.silent)
    {
        if (ours.conversion)
        {
            std::printf("%s: %s fails only in ntdlib's converter\n", name.c_str(), sound_name.c_str());
        }
        else
        {
            counts.failed++;
        }
        if (on_disk)
        {
            std::printf("%s: %s fails in the converter, and the reference folder holds its files\n", name.c_str(),
                        sound_name.c_str());
        }
        if (ours.conversion || on_disk)
        {
            counts.differ++;
        }
        return;
    }
    if (!ours.conversion)
    {
        std::printf("%s: %s fails only in Hokuto: %s\n", name.c_str(), sound_name.c_str(), ours.error.c_str());
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
    if (theirs.warnings != ours.conversion->warnings)
    {
        parts.emplace_back("the warnings");
    }
    if (!SameSfzFiles(theirs.sfz, ours.conversion->sfz))
    {
        parts.emplace_back("the SFZ files");
    }
    if (!SameSfzSamples(theirs.sfz, ours.conversion->sfz))
    {
        parts.emplace_back("the SFZ samples");
    }
    if (ours_rom.WritesSfz() && ours.conversion->sfz.files.empty())
    {
        parts.emplace_back("the missing SFZ files");
    }
    tallies.sfz_files += ours.conversion->sfz.files.size();
    tallies.sfz_samples += ours.conversion->sfz.samples.size();
    if (!reference_folder.empty() && (!on_disk || disk_midi != ours.conversion->midi))
    {
        parts.emplace_back("the reference MIDI file");
    }
    if (!reference_folder.empty() && (!on_disk || disk_sf2 != ours.conversion->sf2))
    {
        parts.emplace_back("the reference SoundFont 2 file");
    }
    if (!parts.empty())
    {
        std::printf("%s: %s differs in %s\n", name.c_str(), sound_name.c_str(), Listed(parts).c_str());
        counts.differ++;
    }
}

} // namespace

int CheckDriverRom(std::vector<uint8_t> bytes, const std::string& name, const std::string& only,
                   const std::filesystem::path& reference_folder, const std::string& stem, const std::string& program)
{
    std::shared_ptr<const hokuto::song::DriverRom> theirs_rom;
    std::shared_ptr<const hokuto::song::DriverRom> ours_rom;
    try
    {
        const hokuto::format::Driver* driver = hokuto::format::DriverOfRom(bytes);
        const hokuto::format::File file = driver->Import(bytes);
        ours_rom = hokuto::song::LoadDriverRom(driver->Facts().engine, hokuto::format::ExportArchive(file));
        theirs_rom = hokuto::song::LoadDriverRom(driver->Facts().engine, std::move(bytes));
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "%s: %s\n", name.c_str(), error.what());
        return 2;
    }

    Counts counts;
    DriverTallies tallies;
    for (const uint32_t sound : theirs_rom->Sounds())
    {
        const std::string sound_name = theirs_rom->SoundName(sound);
        if (!only.empty() && sound_name != only)
        {
            continue;
        }

        CheckDriverSound(*theirs_rom, *ours_rom, sound, sound_name, name, reference_folder, stem, program, counts,
                         tallies);
    }

    std::printf("%s: %zu songs checked. %zu differ, and %zu fail on both sides. %zu reference files compared.\n",
                name.c_str(), counts.sounds, counts.differ, counts.failed, tallies.files_read);
    if (ours_rom->WritesSfz())
    {
        std::printf("%s: %zu SFZ files and %zu SFZ samples compared.\n", name.c_str(), tallies.sfz_files,
                    tallies.sfz_samples);
    }

    return counts.differ > 0 ? 1 : 0;
}

} // namespace hokuto::tools::exports
