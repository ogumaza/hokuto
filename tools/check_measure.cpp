// SPDX-License-Identifier: MIT

// Measures the export to MIDI of each sequence sound of a sound archive of the 3DS, the Wii U, the Switch or the Wii.
// The Export window measures an export the same way (measure/measure.h). A synth renders the MIDI file with its
// SoundFont 2 file, or with its SFZ files with --sfz. The console's model renders the sound dry, in strict mixing, or
// in enhanced mixing with --enhanced. The tool opens the archive as the program does, and converts each sound with no
// variable set and no changes. It prints a line for each sound that the measure finds audible, with its worst place,
// and a summary: the sounds that measured without a finding, with places that may be audible, and with audible places,
// the windows within 1 dB and within 3 dB, the spread of the gains, and the time that the measures took. A sound that
// draws random numbers plays other notes in the export than in Hokuto (edit::DrawsRandom). Its line says so, and the
// summary counts such sounds apart. --most n measures the first n sequence sounds alone, and --jobs n measures n sounds
// at a time. With a sound's name, the tool measures that sound alone. It prints every place that the measure finds, and
// the level of both renders in each half second, lined up. A Wii archive measures every sound at the Wii's 32 kHz, in
// strict mixing. The sounds play by the rules of the build of NW4R SND that the archive's version gives: the tool reads
// no main.dol. A 3DS archive measures on ntdlib's model of the DSP's audio firmware: at the 3DS's 32,728 Hz in strict
// mixing, and at about 48,070 Hz in enhanced mixing. The tool makes no sound.
//
// usage: hokuto_check_measure [--sfz] [--enhanced] [--most n] [--jobs n] <archive> [sound]

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
#include "xsflib/files.h"

#include "edit/commands.h"
#include "format/archive.h"
#include "format/file.h"
#include "measure/measure.h"
#include "song/archive.h"
#include "song/audibility.h"
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
            measure::MeasureExport(conversion, instruments, archive, sequence, mixing, kCancel);
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
        const measure::Measure found = measure::MeasureExport(conversion, instruments, archive, sequence, kCancel);
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
        const measure::Renders renders =
            measure::RenderExport(conversion, instruments, archive, sequence, mixing, kCancel);
        const measure::Measure result = measure::MeasureRenders(renders);
        const song::LevelComparison& comparison = result.comparison;
        std::fprintf(kOut,
                     "%s: %.1f s compared, a gain of %.1f dB, a lag of %.1f ms. Of %zu windows, %zu lie within 1 dB, "
                     "%zu from 1 to 3 dB and %zu beyond.\n",
                     name.c_str(), result.seconds, comparison.gain_db, comparison.lag * 1000.0, comparison.windows,
                     comparison.within_1db, comparison.from_1_to_3db, comparison.beyond_3db);
        if (Random(opened, found->file_id, *sequence))
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
                         static_cast<double>(from) / renders.rate,
                         Level(renders.hokuto, static_cast<int64_t>(from), half),
                         Level(renders.exported, static_cast<int64_t>(from) + lag, half));
        }
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

} // namespace

int main(int argc, char** argv)
{
    measure::Instruments instruments = measure::Instruments::kSoundFont;
    song::Mixing mixing = song::Mixing::kStrict;
    std::size_t most = SIZE_MAX;
    std::size_t jobs = 1;
    std::string path;
    std::string name;
    for (int i = 1; i < argc; i++)
    {
        const std::string argument = argv[i];
        if (argument == "--sfz")
        {
            instruments = measure::Instruments::kSfz;
        }
        else if (argument == "--enhanced")
        {
            mixing = song::Mixing::kEnhanced;
        }
        else if ((argument == "--most" || argument == "--jobs") && i + 1 < argc)
        {
            (argument == "--most" ? most : jobs) = static_cast<std::size_t>(std::strtoull(argv[++i], nullptr, 10));
        }
        else
        {
            (path.empty() ? path : name) = argument;
        }
    }

    if (path.empty() || jobs == 0)
    {
        std::fprintf(stderr,
                     "usage: hokuto_check_measure [--sfz] [--enhanced] [--most n] [--jobs n] <archive> [sound]\n");
        return 2;
    }

    // The archive and the group files beside it make a Hokuto file the way an import makes one. A Wii archive opens as
    // the program opens a Wii file's archive, and makes a file as an import of it without the game's main.dol does.
    Opened opened;
    std::shared_ptr<const ntdlib::rsar::SoundArchive> wii;
    try
    {
        std::vector<uint8_t> bytes;
        xsflib::ReadFile(xsflib::PathFromUtf8(path), bytes);
        if (bytes.size() >= 4 && std::memcmp(bytes.data(), "RSAR", 4) == 0)
        {
            wii = song::LoadWiiArchive(bytes, xsflib::Utf8(xsflib::PathFromUtf8(path).filename()),
                                       ntdlib::rvl::snd::kRsarOptions);
            opened.file = format::ImportWiiArchive(bytes);
            opened.sequences = format::SequencesOfFiles(opened.file, bytes, *wii);
        }
        else
        {
            opened.archive = song::OpenArchive(path);
            opened.file = format::ImportArchive(bytes, opened.archive->ReadOptions().flavor,
                                                opened.archive->GroupFiles(), {}, opened.archive->GroupFileGroups());
            opened.sequences = format::SequencesOfFiles(opened.file, bytes, *opened.archive);
        }
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "%s: %s\n", path.c_str(), error.what());
        return 2;
    }
    if (wii && (!name.empty() || mixing != song::Mixing::kStrict))
    {
        std::fprintf(stderr, "A Wii archive measures every sound, in strict mixing.\n");
        return 2;
    }

    if (!name.empty())
    {
        return MeasureOne(opened, name, instruments, mixing);
    }

    // The sounds go out to the jobs in order. Each job takes the next one.
    std::vector<uint32_t> sounds;
    const std::size_t count = wii ? wii->Sounds().size() : opened.archive->Sounds().size();
    for (uint32_t sound = 0; sound < count && sounds.size() < most; sound++)
    {
        if (wii ? wii->Sounds()[sound].type == ntdlib::rsar::SoundType::kSequence
                : opened.archive->Sounds()[sound].type == sar::SoundType::kSequence)
        {
            sounds.push_back(sound);
        }
    }
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
                                             results[i] = wii ? MeasureWiiSound(wii, opened, sounds[i], instruments)
                                                              : MeasureSound(opened, sounds[i], instruments, mixing);
                                         }
                                     }));
    }
    for (std::future<void>& worker : workers)
    {
        worker.get();
    }
    const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

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
