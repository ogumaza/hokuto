// SPDX-License-Identifier: MIT

// Measures the export to MIDI of each sequence sound of a sound archive of the 3DS, the Wii U, the Switch or the Wii,
// of each song of a GBA ROM with MusicPlayer2000, of each sequence of a sound archive of the DS, or of each DSE sound
// of a DS image. The Export window measures an export the same way (measure/measure.h). A synth renders the MIDI file
// with its SoundFont 2 file, or with its SFZ files with --sfz. The console's model renders the sound dry, in strict
// mixing, or in enhanced mixing with --enhanced. The tool opens the archive as the program does, and converts each
// sound with no variable set and no changes. It prints a line for each sound that the measure finds audible, with its
// worst place, and a summary: the sounds that measured without a finding, with places that may be audible, and with
// audible places, the windows within 1 dB and within 3 dB, the spread of the gains, and the time that the measures
// took. A sound that draws random numbers plays other notes in the export than in Hokuto (edit::DrawsRandom). Its line
// says so, and the summary counts such sounds apart. --most n measures the first n sequence sounds alone, and --jobs n
// measures n sounds at a time. With sounds' names, the tool measures those sounds alone, one after another. It prints
// every place that the measure finds, and the level of both renders in each half second, lined up. A Wii archive
// measures every sound at the Wii's 32 kHz, in strict mixing. The sounds play by the rules of the build of NW4R SND
// that the archive's version gives: the tool reads no main.dol. A 3DS archive measures on ntdlib's model of the DSP's
// audio firmware: at the 3DS's 32,728 Hz in strict mixing, and at about 48,070 Hz in enhanced mixing. A GBA ROM
// measures the songs that play (song::GbaSongs), with the export's MIDI file and SoundFont 2 file, against the GBA's
// dry render at the rate that the game sets. A song's name is its name in the browser, such as "Song 012". A song that
// plays no notes counts as silent: the program refuses its export. MusicPlayer2000 draws no random numbers. A GBA ROM
// with another GBA driver, such as Konami's driver, measures each of its sounds the same way, against the output of the
// driver without its echo (song::DriverRom::Sounds). Such a ROM accepts --sfz where the converter of its driver writes
// SFZ files. A DS cartridge image measures its largest sound archive, the one that the program imports, with the build
// of the driver that the ARM7 binary of the image contains. A sound archive of the DS alone uses the default build. A
// DS sequence measures at the model's rate: 32,728 Hz in strict mixing, and about 47,989 Hz in enhanced mixing. As the
// model does, ntdlib's converter of the DS draws the driver's random numbers from the driver's starting state. The tool
// counts no DS sequence apart for them. A DS cartridge image without a sound archive measures each of its DSE sounds
// the same way (song::DseSounds), with the build of the driver that the image contains. The tool makes no sound.
//
// usage: hokuto_check_measure [--sfz] [--enhanced] [--most n] [--jobs n] <archive, ROM or DS image> [sound]...

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "ntdlib/rsar/formats.h"
#include "ntdlib/rvl/snd/rsar_options.h"
#include "ntdlib/sar/formats.h"
#include "xsflib/console.h"
#include "xsflib/files.h"

#include "edit/commands.h"
#include "edit/ds_files.h"
#include "format/archive.h"
#include "format/drivers.h"
#include "format/file.h"
#include "measure/measure.h"
#include "song/archive.h"
#include "song/audibility.h"
#include "song/driver.h"
#include "song/dry_render.h"
#include "song/ds.h"
#include "song/dse.h"
#include "song/gba.h"
#include "song/midi_export.h"
#include "song/sequence.h"

namespace
{

namespace sar = ntdlib::sar;
namespace format = hokuto::format;
namespace measure = hokuto::measure;
namespace song = hokuto::song;

// Where the tool writes its lines.
FILE* const kOut = stdout;

// The archive as the program opens it, the Hokuto file that it makes, and the sequence that each of its files holds.
struct Opened
{
    std::shared_ptr<const sar::SoundArchive> archive;
    format::File file;
    std::vector<std::optional<uint32_t>> sequences;
};

// Whether a sound whose sequence lies in file `id` of the archive draws random numbers. `sequence` is the sound as the
// program reads it.
bool Random(const Opened& opened, uint32_t id, const song::Sequence& sequence)
{
    return id < opened.sequences.size() && opened.sequences[id] &&
           hokuto::edit::DrawsRandom(opened.file, *opened.sequences[id], sequence);
}

// What one sound's measure gave.
struct Result
{
    std::string name;
    std::string error;
    song::LevelComparison comparison;
    double seconds = 0.0;
    double elapsed = 0.0;
    bool random = false;
};

// The measure of sound `sound` of `opened`'s archive, with `instruments`, against the model in `mixing`.
Result MeasureSound(const Opened& opened, uint32_t sound, measure::Instruments instruments, song::Mixing mixing)
{
    const std::shared_ptr<const sar::SoundArchive>& archive = opened.archive;
    Result result;
    result.name = archive->Sounds()[sound].name;
    const auto start = std::chrono::steady_clock::now();
    try
    {
        const auto sequence = std::make_shared<const song::Sequence>(song::ReadSequence(*archive, sound));
        const ntdlib::sequence_export::Conversion conversion = song::ConvertToMidi(*archive, sound, {}, {}, "Hokuto");
        const std::atomic<bool> kCancel{false};
        const measure::Measure found =
            measure::MeasureExport(conversion, instruments, song::DryOf(archive, sequence, mixing), kCancel);
        result.comparison = found.comparison;
        result.seconds = found.seconds;
        result.random = Random(opened, archive->Sounds()[sound].file_id, *sequence);
    }
    catch (const std::exception& error)
    {
        result.error = error.what();
    }
    result.elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

    return result;
}

// The measure of sound `sound` of the Wii's `archive`. `opened` holds the Hokuto file that the archive makes, and the
// sequence that each of its files holds.
Result MeasureWiiSound(const std::shared_ptr<const ntdlib::rsar::SoundArchive>& archive, const Opened& opened,
                       uint32_t sound, measure::Instruments instruments)
{
    Result result;
    result.name = archive->Sounds()[sound].name;
    const auto start = std::chrono::steady_clock::now();
    try
    {
        const auto sequence = std::make_shared<const song::Sequence>(song::ReadSequence(*archive, sound));
        const ntdlib::sequence_export::Conversion conversion = song::ConvertToMidi(*archive, sound, {}, {}, "Hokuto");
        const std::atomic<bool> kCancel{false};
        const measure::Measure found =
            measure::MeasureExport(conversion, instruments, song::DryOf(archive, sequence), kCancel);
        result.comparison = found.comparison;
        result.seconds = found.seconds;
        result.random = Random(opened, archive->Sounds()[sound].file_id, *sequence);
    }
    catch (const std::exception& error)
    {
        result.error = error.what();
    }
    result.elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

    return result;
}

// The measure of song `number` of the GBA's `rom`, with the export's files, against the model in `mixing`.
Result MeasureGbaSong(const std::shared_ptr<const song::GbaRom>& rom, int number, song::Mixing mixing)
{
    Result result;
    result.name = song::GbaSongName(*rom, number);
    const auto start = std::chrono::steady_clock::now();
    try
    {
        // A song that plays no notes counts as silent. Its MIDI file would be empty, and the program refuses it.
        const auto sequence = std::make_shared<const song::Sequence>(song::ReadSequence(*rom, number));
        if (std::ranges::any_of(sequence->tracks, [](const song::Track& track) { return !track.notes.empty(); }))
        {
            const ntdlib::sequence_export::Conversion conversion = song::ConvertToMidi(*rom, number, "Hokuto");
            const std::atomic<bool> kCancel{false};
            const measure::Measure found = measure::MeasureExport(conversion, measure::Instruments::kSoundFont,
                                                                  song::DryOf(rom, sequence, mixing), kCancel);
            result.comparison = found.comparison;
            result.seconds = found.seconds;
        }
    }
    catch (const std::exception& error)
    {
        result.error = error.what();
    }
    result.elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

    return result;
}

// The measure of sound `sound` of the GBA's `rom` with another of the GBA's drivers, with the export's MIDI file and
// `instruments`, against the driver in `mixing`.
Result MeasureDriverSound(const std::shared_ptr<const song::DriverRom>& rom, uint32_t sound,
                          measure::Instruments instruments, song::Mixing mixing)
{
    Result result;
    result.name = rom->SoundName(sound);
    const auto start = std::chrono::steady_clock::now();
    try
    {
        // A song that plays no notes counts as silent. Its MIDI file would be empty, and the program refuses it.
        const auto sequence = std::make_shared<const song::Sequence>(rom->ReadSequence(sound));
        if (std::ranges::any_of(sequence->tracks, [](const song::Track& track) { return !track.notes.empty(); }))
        {
            const ntdlib::sequence_export::Conversion conversion = rom->ConvertToMidi(sound, "Hokuto");
            const std::atomic<bool> kCancel{false};
            const measure::Measure found =
                measure::MeasureExport(conversion, instruments, song::DryOf(rom, sequence, mixing), kCancel);
            result.comparison = found.comparison;
            result.seconds = found.seconds;
        }
    }
    catch (const std::exception& error)
    {
        result.error = error.what();
    }
    result.elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

    return result;
}

// The measure of sound `sound` of the DS's `archive`, with `instruments`, against the model in `mixing`.
Result MeasureDsSound(const std::shared_ptr<const song::DsArchive>& archive, uint32_t sound,
                      measure::Instruments instruments, song::Mixing mixing)
{
    Result result;
    result.name = song::DsSoundName(*archive, sound);
    const auto start = std::chrono::steady_clock::now();
    try
    {
        const auto sequence = std::make_shared<const song::Sequence>(song::ReadSequence(*archive, sound));
        const ntdlib::sequence_export::Conversion conversion = song::ConvertToMidi(*archive, sound, {}, "Hokuto");
        const std::atomic<bool> kCancel{false};
        const measure::Measure found =
            measure::MeasureExport(conversion, instruments, song::DryOf(archive, sequence, mixing), kCancel);
        result.comparison = found.comparison;
        result.seconds = found.seconds;
    }
    catch (const std::exception& error)
    {
        result.error = error.what();
    }
    result.elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

    return result;
}

// The measure of sound `sound` of `set`, the DSE sounds of a DS image, with `instruments`, against the model in
// `mixing`.
Result MeasureDseSound(const std::shared_ptr<const song::DseSet>& set, uint32_t sound, measure::Instruments instruments,
                       song::Mixing mixing)
{
    Result result;
    result.name = song::DseSoundName(*set, sound);
    const auto start = std::chrono::steady_clock::now();
    try
    {
        const auto sequence = std::make_shared<const song::Sequence>(song::ReadSequence(*set, sound));
        const ntdlib::sequence_export::Conversion conversion = song::ConvertToMidi(*set, sound, "Hokuto");
        const std::atomic<bool> kCancel{false};
        const measure::Measure found =
            measure::MeasureExport(conversion, instruments, song::DryOf(set, sequence, mixing), kCancel);
        result.comparison = found.comparison;
        result.seconds = found.seconds;
    }
    catch (const std::exception& error)
    {
        result.error = error.what();
    }
    result.elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

    return result;
}

// The level of `frames` frames of `samples`, interleaved stereo, from frame `from`, in dB below full scale.
double Level(const std::vector<float>& samples, int64_t from, std::size_t frames)
{
    double sum = 0.0;
    for (std::size_t i = 0; i < frames * 2; i++)
    {
        const int64_t at = from * 2 + static_cast<int64_t>(i);
        if (at >= 0 && at < static_cast<int64_t>(samples.size()))
        {
            sum += static_cast<double>(samples[static_cast<std::size_t>(at)]) * samples[static_cast<std::size_t>(at)];
        }
    }

    return sum > 0.0 ? 10.0 * std::log10(sum / static_cast<double>(frames * 2)) : -200.0;
}

// Prints the measure of `renders` of sound `name`: its places, and the level of both renders in each half second.
void PrintRenders(const std::string& name, const measure::Renders& renders, bool random)
{
    const measure::Measure result = measure::MeasureRenders(renders);
    const song::LevelComparison& comparison = result.comparison;
    std::fprintf(kOut,
                 "%s: %.1f s compared, a gain of %.1f dB, a lag of %.1f ms. Of %zu windows, %zu are within 1 dB, %zu "
                 "from 1 to 3 dB and %zu beyond.\n",
                 name.c_str(), result.seconds, comparison.gain_db, comparison.lag * 1000.0, comparison.windows,
                 comparison.within_1db, comparison.from_1_to_3db, comparison.beyond_3db);
    if (random)
    {
        std::fprintf(kOut, "The sound draws random numbers.\n");
    }
    for (const song::LevelPlace& place : comparison.places)
    {
        std::fprintf(kOut, "%s of %.1f dB from %.2f s for %.2f s\n", place.run ? "A run" : "A span", place.db,
                     place.start, place.seconds);
    }

    // Each half second's level in Hokuto's render and in the export's, lined up.
    const auto half = static_cast<std::size_t>(renders.rate / 2);
    const auto lag = static_cast<int64_t>(std::lround(comparison.lag * renders.rate));
    const std::size_t frames = std::min(renders.hokuto.size(), renders.exported.size()) / 2;
    for (std::size_t from = 0; from < frames; from += half)
    {
        std::fprintf(kOut, "%6.1f s: Hokuto %6.1f dBFS, the export %6.1f dBFS\n",
                     static_cast<double>(from) / renders.rate, Level(renders.hokuto, static_cast<int64_t>(from), half),
                     Level(renders.exported, static_cast<int64_t>(from) + lag, half));
    }
}

// Measures song `name` of the GBA's `rom` alone as MeasureOne does.
int MeasureOneGba(const std::shared_ptr<const song::GbaRom>& rom, const std::string& name, song::Mixing mixing)
{
    const std::vector<int> songs = song::GbaSongs(*rom);
    const auto found = std::ranges::find_if(songs, [&](int number) { return song::GbaSongName(*rom, number) == name; });
    if (found == songs.end())
    {
        std::fprintf(stderr, "The ROM holds no song %s that plays.\n", name.c_str());
        return 2;
    }

    try
    {
        const auto sequence = std::make_shared<const song::Sequence>(song::ReadSequence(*rom, *found));
        const std::atomic<bool> kCancel{false};
        PrintRenders(
            name,
            measure::RenderExport(song::ConvertToMidi(*rom, *found, "Hokuto"), measure::Instruments::kSoundFont,
                                  song::DryOf(rom, sequence, mixing), kCancel),
            false);
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "%s: %s\n", name.c_str(), error.what());
        return 1;
    }

    return 0;
}

// Measures sound `name` of the GBA's `rom` with another of the GBA's drivers alone as MeasureOne does.
int MeasureOneDriver(const std::shared_ptr<const song::DriverRom>& rom, const std::string& name,
                     measure::Instruments instruments, song::Mixing mixing)
{
    const std::vector<uint32_t> sounds = rom->Sounds();
    const auto found = std::ranges::find_if(sounds, [&](uint32_t sound) { return rom->SoundName(sound) == name; });
    if (found == sounds.end())
    {
        std::fprintf(stderr, "The ROM holds no song %s.\n", name.c_str());
        return 2;
    }

    try
    {
        const auto sequence = std::make_shared<const song::Sequence>(rom->ReadSequence(*found));
        const std::atomic<bool> kCancel{false};
        PrintRenders(name,
                     measure::RenderExport(rom->ConvertToMidi(*found, "Hokuto"), instruments,
                                           song::DryOf(rom, sequence, mixing), kCancel),
                     false);
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "%s: %s\n", name.c_str(), error.what());
        return 1;
    }

    return 0;
}

// Measures sound `name` of the DS's `archive` alone as MeasureOne does.
int MeasureOneDs(const std::shared_ptr<const song::DsArchive>& archive, const std::string& name,
                 measure::Instruments instruments, song::Mixing mixing)
{
    const std::vector<uint32_t> sounds = song::DsSounds(*archive);
    const auto found =
        std::ranges::find_if(sounds, [&](uint32_t sound) { return song::DsSoundName(*archive, sound) == name; });
    if (found == sounds.end())
    {
        std::fprintf(stderr, "The archive holds no sequence %s.\n", name.c_str());
        return 2;
    }

    try
    {
        const auto sequence = std::make_shared<const song::Sequence>(song::ReadSequence(*archive, *found));
        const std::atomic<bool> kCancel{false};
        PrintRenders(name,
                     measure::RenderExport(song::ConvertToMidi(*archive, *found, {}, "Hokuto"), instruments,
                                           song::DryOf(archive, sequence, mixing), kCancel),
                     false);
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "%s: %s\n", name.c_str(), error.what());
        return 1;
    }

    return 0;
}

// Measures sound `name` of `set`, the DSE sounds of a DS image, alone as MeasureOne does.
int MeasureOneDse(const std::shared_ptr<const song::DseSet>& set, const std::string& name,
                  measure::Instruments instruments, song::Mixing mixing)
{
    const std::vector<uint32_t> sounds = song::DseSounds(*set);
    const auto found =
        std::ranges::find_if(sounds, [&](uint32_t sound) { return song::DseSoundName(*set, sound) == name; });
    if (found == sounds.end())
    {
        std::fprintf(stderr, "The image holds no sound %s.\n", name.c_str());
        return 2;
    }

    try
    {
        const auto sequence = std::make_shared<const song::Sequence>(song::ReadSequence(*set, *found));
        const std::atomic<bool> kCancel{false};
        PrintRenders(name,
                     measure::RenderExport(song::ConvertToMidi(*set, *found, "Hokuto"), instruments,
                                           song::DryOf(set, sequence, mixing), kCancel),
                     false);
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "%s: %s\n", name.c_str(), error.what());
        return 1;
    }

    return 0;
}

// Measures sound `name` alone, and prints its places and the level of both renders in each half second.
int MeasureOne(const Opened& opened, const std::string& name, measure::Instruments instruments, song::Mixing mixing)
{
    const std::shared_ptr<const sar::SoundArchive>& archive = opened.archive;
    const std::vector<sar::SoundInfo>& sounds = archive->Sounds();
    const auto found =
        std::find_if(sounds.begin(), sounds.end(), [&name](const sar::SoundInfo& sound) { return sound.name == name; });
    if (found == sounds.end() || found->type != sar::SoundType::kSequence)
    {
        std::fprintf(stderr, "The archive holds no sequence sound %s.\n", name.c_str());
        return 2;
    }

    const auto sound = static_cast<uint32_t>(found - sounds.begin());
    try
    {
        const auto sequence = std::make_shared<const song::Sequence>(song::ReadSequence(*archive, sound));
        const ntdlib::sequence_export::Conversion conversion = song::ConvertToMidi(*archive, sound, {}, {}, "Hokuto");
        const std::atomic<bool> kCancel{false};
        PrintRenders(name,
                     measure::RenderExport(conversion, instruments, song::DryOf(archive, sequence, mixing), kCancel),
                     Random(opened, found->file_id, *sequence));
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "%s: %s\n", name.c_str(), error.what());
        return 1;
    }

    return 0;
}

// Whether a place is audible: a run, or a span more than 3 dB off.
bool Audible(const song::LevelPlace& place)
{
    return place.run || std::abs(place.db) > 3.0;
}

// The sequence sounds to measure, in order, up to `most` of them: those of the ROM, image or archive that is open.
std::vector<uint32_t> SoundsToMeasure(const Opened& opened,
                                      const std::shared_ptr<const ntdlib::rsar::SoundArchive>& wii,
                                      const std::shared_ptr<const song::GbaRom>& gba,
                                      const std::shared_ptr<const song::DriverRom>& driver,
                                      const std::shared_ptr<const song::DsArchive>& ds,
                                      const std::shared_ptr<const song::DseSet>& dse, std::size_t most)
{
    std::vector<uint32_t> sounds;
    if (gba)
    {
        for (const int number : song::GbaSongs(*gba))
        {
            if (sounds.size() < most)
            {
                sounds.push_back(static_cast<uint32_t>(number));
            }
        }
    }

    if (driver)
    {
        for (const uint32_t sound : driver->Sounds())
        {
            if (sounds.size() < most)
            {
                sounds.push_back(sound);
            }
        }
    }

    if (ds)
    {
        for (const uint32_t sound : song::DsSounds(*ds))
        {
            if (sounds.size() < most)
            {
                sounds.push_back(sound);
            }
        }
    }

    if (dse)
    {
        for (const uint32_t sound : song::DseSounds(*dse))
        {
            if (sounds.size() < most)
            {
                sounds.push_back(sound);
            }
        }
    }

    const std::size_t count = gba || driver || ds || dse ? 0
                              : wii                      ? wii->Sounds().size()
                                                         : opened.archive->Sounds().size();
    for (uint32_t sound = 0; sound < count && sounds.size() < most; sound++)
    {
        if (wii ? wii->Sounds()[sound].type == ntdlib::rsar::SoundType::kSequence
                : opened.archive->Sounds()[sound].type == sar::SoundType::kSequence)
        {
            sounds.push_back(sound);
        }
    }

    return sounds;
}

// Prints the results of the measures of `sounds`, made with `instruments` in `mixing` by `jobs` jobs in `elapsed`
// seconds. Returns the tool's exit status.
int PrintResults(const std::vector<Result>& results, const std::vector<uint32_t>& sounds,
                 measure::Instruments instruments, song::Mixing mixing, std::size_t jobs, double elapsed)
{
    // A line for each sound that failed or has an audible place, then the summary.
    std::size_t failed = 0, silent = 0, clean = 0, maybe = 0, audible = 0, random = 0, audible_random = 0;
    std::size_t windows = 0, within_1db = 0, within_3db = 0;
    std::vector<double> gains;
    double measured = 0.0, slowest = 0.0;
    for (const Result& result : results)
    {
        slowest = std::max(slowest, result.elapsed);
        if (!result.error.empty())
        {
            std::fprintf(kOut, "%s: %s\n", result.name.c_str(), result.error.c_str());
            failed++;
            continue;
        }

        const song::LevelComparison& comparison = result.comparison;
        random += result.random ? 1 : 0;
        measured += result.seconds;
        windows += comparison.windows;
        within_1db += comparison.within_1db;
        within_3db += comparison.within_1db + comparison.from_1_to_3db;
        if (comparison.windows == 0)
        {
            silent++;
            continue;
        }

        gains.push_back(comparison.gain_db);
        const std::vector<song::LevelPlace> worst = song::WorstPlaces(comparison, song::kSpanFindingDb, 1);
        if (worst.empty())
        {
            clean++;
        }
        else if (std::any_of(comparison.places.begin(), comparison.places.end(), Audible))
        {
            audible++;
            audible_random += result.random ? 1 : 0;
            std::fprintf(kOut, "%s: %s %.1f dB at %.2f s for %.2f s (gain %.1f dB, lag %.1f ms)%s\n",
                         result.name.c_str(), worst[0].run ? "run" : "span", worst[0].db, worst[0].start,
                         worst[0].seconds, comparison.gain_db, comparison.lag * 1000.0,
                         result.random ? ", draws random numbers" : "");
        }
        else
        {
            maybe++;
        }
    }
    std::sort(gains.begin(), gains.end());

    const auto share = [](std::size_t part, std::size_t whole)
    {
        return whole > 0 ? 100.0 * static_cast<double>(part) / static_cast<double>(whole) : 0.0;
    };

    std::fprintf(
        kOut,
        "%zu sequence sounds measured with %s in %s mixing: %zu without a finding, %zu that may be audible, %zu "
        "audible, %zu silent, %zu failed.\n",
        sounds.size(), instruments == measure::Instruments::kSoundFont ? "SoundFont 2" : "SFZ",
        mixing == song::Mixing::kStrict ? "strict" : "enhanced", clean, maybe, audible, silent, failed);
    if (!gains.empty())
    {
        std::fprintf(
            kOut,
            "%.1f %% of the windows within 1 dB, %.1f %% within 3 dB. Gains from %.1f to %.1f dB, median %.1f dB.\n",
            share(within_1db, windows), share(within_3db, windows), gains.front(), gains.back(),
            gains[gains.size() / 2]);
    }
    std::fprintf(kOut, "%zu sounds draw random numbers, %zu of them with audible places.\n", random, audible_random);
    std::fprintf(kOut, "%.0f s of sound in %.0f s, %zu at a time. The slowest sound took %.1f s.\n", measured, elapsed,
                 jobs, slowest);

    return failed == 0 ? 0 : 1;
}

// The options of a run: the instruments and the mixing to measure with, the most sounds to measure, the jobs that
// measure them at once, the ROM, image or archive, and the sounds to measure alone, if any.
struct Options
{
    measure::Instruments instruments = measure::Instruments::kSoundFont;
    song::Mixing mixing = song::Mixing::kStrict;
    std::size_t most = SIZE_MAX;
    std::size_t jobs = 1;
    std::string path;
    std::vector<std::string> names;
};

// The options in the command line's `arguments`, or nothing after the usage when they don't fit it.
std::optional<Options> ParseArguments(const std::vector<std::string>& arguments)
{
    measure::Instruments instruments = measure::Instruments::kSoundFont;
    song::Mixing mixing = song::Mixing::kStrict;
    std::size_t most = SIZE_MAX;
    std::size_t jobs = 1;
    std::string path;
    std::vector<std::string> names;
    for (std::size_t i = 1; i < arguments.size(); i++)
    {
        const std::string& argument = arguments[i];
        if (argument == "--sfz")
        {
            instruments = measure::Instruments::kSfz;
        }
        else if (argument == "--enhanced")
        {
            mixing = song::Mixing::kEnhanced;
        }
        else if ((argument == "--most" || argument == "--jobs") && i + 1 < arguments.size())
        {
            (argument == "--most" ? most : jobs) =
                static_cast<std::size_t>(std::strtoull(arguments[++i].c_str(), nullptr, 10));
        }
        else if (path.empty())
        {
            path = argument;
        }
        else
        {
            names.push_back(argument);
        }
    }

    if (path.empty() || jobs == 0)
    {
        std::fprintf(
            stderr,
            "usage: hokuto_check_measure [--sfz] [--enhanced] [--most n] [--jobs n] <archive, ROM or DS image> "
            "[sound]\n");
        return std::nullopt;
    }

    return Options{instruments, mixing, most, jobs, path, names};
}

// What a run opens: a Switch, Wii U or 3DS archive with its Hokuto file, a Wii archive, a GBA ROM, a ROM with a
// driver, a DS archive or a DSE set.
struct MeasureFile
{
    Opened opened;
    std::shared_ptr<const ntdlib::rsar::SoundArchive> wii;
    std::shared_ptr<const song::GbaRom> gba;
    std::shared_ptr<const song::DriverRom> driver;
    std::shared_ptr<const song::DsArchive> ds;
    std::shared_ptr<const song::DseSet> dse;
};

// Opens the ROM, image or archive at `path`. Throws where the tool can't read it.
MeasureFile OpenMeasureFile(const std::string& path)
{
    // The archive and the group files beside it make a Hokuto file the way an import makes one. A Wii archive opens as
    // the program opens a Wii file's archive, and makes a file as an import of it without the game's main.dol does.
    MeasureFile m;
    std::vector<uint8_t> bytes;
    xsflib::ReadFile(xsflib::PathFromUtf8(path), bytes);
    if (const format::Driver* found = format::IsGbaRom(bytes) ? format::DriverOfRom(bytes) : nullptr)
    {
        m.driver = song::LoadDriverRom(found->Facts().engine, format::ExportArchive(found->Import(bytes)));
    }
    else if (format::IsGbaRom(bytes))
    {
        m.gba = song::LoadGbaRom(std::move(bytes));
    }
    else if (format::IsDsImage(bytes) && format::DsArchivesIn(bytes).empty() && format::HoldsDseFiles(bytes))
    {
        m.dse = song::LoadDseSet(format::ExportArchive(format::ImportDseImage(bytes)), song::DseBuildOf(bytes));
    }
    else if (format::IsDsImage(bytes))
    {
        const format::File file = format::ImportDsImage(bytes);
        m.ds =
            song::LoadDsArchive(format::ExportArchive(file), song::DsBuildOf(bytes), hokuto::edit::DsGameOfFile(file));
    }
    else if (format::IsDsArchive(bytes))
    {
        m.ds = song::LoadDsArchive(format::ExportArchive(format::ImportDsArchive(bytes)));
    }
    else if (bytes.size() >= 4 && std::memcmp(bytes.data(), "RSAR", 4) == 0)
    {
        m.wii = song::LoadWiiArchive(bytes, xsflib::Utf8(xsflib::PathFromUtf8(path).filename()),
                                     ntdlib::rvl::snd::kRsarOptions);
        m.opened.file = format::ImportWiiArchive(bytes);
        m.opened.sequences = format::SequencesOfFiles(m.opened.file, bytes, *m.wii);
    }
    else
    {
        m.opened.archive = song::OpenArchive(path);
        m.opened.file = format::ImportArchive(bytes, m.opened.archive->ReadOptions().flavor,
                                              m.opened.archive->GroupFiles(), {}, m.opened.archive->GroupFileGroups());
        m.opened.sequences = format::SequencesOfFiles(m.opened.file, bytes, *m.opened.archive);
    }

    return m;
}

// Measures the sounds named `names` of `m` alone, with `instruments` in `mixing`. Returns the tool's exit status.
int MeasureNamed(const MeasureFile& m, const std::vector<std::string>& names, measure::Instruments instruments,
                 song::Mixing mixing)
{
    int status = 0;
    for (const std::string& name : names)
    {
        status = std::max(status, m.driver ? MeasureOneDriver(m.driver, name, instruments, mixing)
                                  : m.gba  ? MeasureOneGba(m.gba, name, mixing)
                                  : m.ds   ? MeasureOneDs(m.ds, name, instruments, mixing)
                                  : m.dse  ? MeasureOneDse(m.dse, name, instruments, mixing)
                                           : MeasureOne(m.opened, name, instruments, mixing));
    }

    return status;
}

// The measure of sound `sound` of `m`, with `instruments` in `mixing`.
Result MeasureOf(const MeasureFile& m, uint32_t sound, measure::Instruments instruments, song::Mixing mixing)
{
    return m.driver ? MeasureDriverSound(m.driver, sound, instruments, mixing)
           : m.gba  ? MeasureGbaSong(m.gba, static_cast<int>(sound), mixing)
           : m.ds   ? MeasureDsSound(m.ds, sound, instruments, mixing)
           : m.dse  ? MeasureDseSound(m.dse, sound, instruments, mixing)
           : m.wii  ? MeasureWiiSound(m.wii, m.opened, sound, instruments)
                    : MeasureSound(m.opened, sound, instruments, mixing);
}

} // namespace

int main(int argc, char** argv)
{
    const std::vector<std::string> arguments = xsflib::console::Arguments(argc, argv);
    const std::optional<Options> options = ParseArguments(arguments);
    if (!options)
    {
        return 2;
    }
    const measure::Instruments instruments = options->instruments;
    const song::Mixing mixing = options->mixing;
    const std::size_t most = options->most;
    const std::size_t jobs = options->jobs;
    const std::string& path = options->path;
    const std::vector<std::string>& names = options->names;

    MeasureFile m;
    try
    {
        m = OpenMeasureFile(path);
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "%s: %s\n", path.c_str(), error.what());
        return 2;
    }
    if (m.wii && (!names.empty() || mixing != song::Mixing::kStrict))
    {
        std::fprintf(stderr, "A Wii archive measures every sound, in strict mixing.\n");
        return 2;
    }
    if ((m.gba || (m.driver && !m.driver->WritesSfz())) && instruments == measure::Instruments::kSfz)
    {
        std::fprintf(stderr, "A GBA song of this driver goes out with SoundFont 2 alone.\n");
        return 2;
    }
    if (!names.empty())
    {
        return MeasureNamed(m, names, instruments, mixing);
    }

    // The sounds go out to the jobs in order. Each job takes the next one.
    const std::vector<uint32_t> sounds = SoundsToMeasure(m.opened, m.wii, m.gba, m.driver, m.ds, m.dse, most);
    std::vector<Result> results(sounds.size());
    std::atomic<std::size_t> next{0};
    const auto start = std::chrono::steady_clock::now();
    std::vector<std::future<void>> workers;
    for (std::size_t j = 0; j < jobs; j++)
    {
        workers.push_back(std::async(std::launch::async,
                                     [&]()
                                     {
                                         for (std::size_t i = next++; i < sounds.size(); i = next++)
                                         {
                                             results[i] = MeasureOf(m, sounds[i], instruments, mixing);
                                         }
                                     }));
    }
    for (std::future<void>& worker : workers)
    {
        worker.get();
    }
    const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

    return PrintResults(results, sounds, instruments, mixing, jobs, elapsed);
}
