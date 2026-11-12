// SPDX-License-Identifier: MIT

// Renders each sequence sound of a sound archive of the Switch or the Wii U through Hokuto's renderer of SoundFonts
// (src/synth/sf2_synth.h). Each sound converts to MIDI and SoundFont 2 files as File > Export converts it. The
// conversion sets no variable and makes no change. The renderer then plays the MIDI file through the SoundFont at 48
// kHz. The tool prints a line for each sound that doesn't convert, fails to render, gives a sample that isn't a number
// or is infinite, or peaks above full scale. The tool's summary gives the sounds rendered, the silent ones, the highest
// peak, and the render's speed against real time. The tool returns 1 when a conversion or a render fails, or a render
// gives a sample that isn't finite. It returns 2 when the archive can't be read. With `--part k/n`, the tool renders
// only part k of n of the sequence sounds: those whose places among the archive's sounds leave k - 1 over when divided
// by n. n runs side by side cover a large archive faster than one run.
//
// With `--fluidsynth <every> <folder>`, FluidSynth renders every nth sound that sounds. FluidSynth runs as a black box.
// The tool writes the sound's files into `folder` and runs the fluidsynth program on them at 48 kHz. The program runs
// without reverb, chorus or a limit of voices, and at a gain of 1. The tool reads the program's output and prints each
// sound's level in both renders over their common length. It also prints the median of the differences. The figures
// pass or fail nothing. FluidSynth departs from the specification in ways that move them:
//
// - FluidSynth's filter plays every voice 1.5 dB above the SoundFont's level.
// - FluidSynth's decays and releases fall 96 dB in their times. The specification gives 100 dB. FluidSynth's sustains
//   attenuate by 0.96 of their values.
// - FluidSynth's exclusive classes cut notes over 0.31 s from version 2.4.3 on, and over 0.89 s before. The
//   specification asks for a rapid end.
// - FluidSynth lengthens a note to 10 ms at least.
// - FluidSynth starts each event at a block of 64 samples.
//
// With a sound's name, the files stay in the folder. Each render goes beside them as raw 32-bit floats, left and right
// in turn. FluidSynth's render goes in a .raw file. Hokuto's render goes in a .hokuto.raw file.
//
// usage: hokuto_check_synth [--part <k>/<n>] [--fluidsynth <every> <folder>] <archive> [sound]

#include <algorithm>
#include <bit>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ntdlib/sar/formats.h"
#include "ntdlib/sequence_export/convert.h"
#include "xsflib/console.h"
#include "xsflib/files.h"

#include "check_part.h"
#include "song/archive.h"
#include "song/midi_export.h"
#include "synth/sf2_synth.h"

namespace
{

namespace sar = ntdlib::sar;
using hokuto::synth::StereoRender;
using hokuto::tools::Part;
using hokuto::tools::PartOf;

constexpr int kRate = 48000;

// The longest render, in seconds: the Export window's limit.
constexpr double kMostSeconds = 300.0;

// What the tool measures in a render: its length, its peak and its samples that aren't finite.
struct Measures
{
    double seconds = 0.0;
    double peak = 0.0;
    std::size_t not_finite = 0;
};

// The measures of `samples`, a stereo render at `rate` with the channels interleaved.
Measures Measure(const std::vector<float>& samples, int rate)
{
    Measures measures;
    measures.seconds = static_cast<double>(samples.size() / 2) / rate;
    for (const float sample : samples)
    {
        if (!std::isfinite(sample))
        {
            measures.not_finite++;
            continue;
        }

        measures.peak = std::max(measures.peak, static_cast<double>(std::abs(sample)));
    }

    return measures;
}

// `level` in decibels relative to full scale.
double Dbfs(double level)
{
    return 20.0 * std::log10(std::max(level, 1e-12));
}

// The RMS level of the first `count` samples of `samples`.
double Rms(const std::vector<float>& samples, std::size_t count)
{
    double sum = 0.0;
    for (std::size_t i = 0; i < count && i < samples.size(); i++)
    {
        sum += static_cast<double>(samples[i]) * samples[i];
    }

    return count == 0 ? 0.0 : std::sqrt(sum / static_cast<double>(count));
}

// `samples` as little-endian 32-bit floats.
std::vector<uint8_t> RawFloats(const std::vector<float>& samples)
{
    std::vector<uint8_t> bytes;
    bytes.reserve(4 * samples.size());
    for (const float sample : samples)
    {
        const auto word = std::bit_cast<uint32_t>(sample);
        for (int shift = 0; shift < 32; shift += 8)
        {
            bytes.push_back(static_cast<uint8_t>(word >> shift));
        }
    }

    return bytes;
}

// FluidSynth's render of `conversion`, or nothing when the program fails. The files go into `folder` under `stem`. They
// stay there when `keep` is set.
std::optional<std::vector<float>> FluidSynthRender(const ntdlib::sequence_export::Conversion& conversion,
                                                   const std::filesystem::path& folder, const std::string& stem,
                                                   bool keep)
{
    const std::filesystem::path midi = folder / (stem + ".mid");
    const std::filesystem::path sf2 = folder / (stem + ".sf2");
    const std::filesystem::path raw = folder / (stem + ".raw");
    if (xsflib::WriteFile(midi, conversion.midi) || xsflib::WriteFile(sf2, conversion.sf2))
    {
        return std::nullopt;
    }

    const std::string command =
        "fluidsynth -n -i -q -R 0 -C 0 -g 1 -r 48000 -o synth.polyphony=65535 -T raw -O float "
        "-E little -F \"" +
        xsflib::Utf8(raw) + "\" \"" + xsflib::Utf8(sf2) + "\" \"" + xsflib::Utf8(midi) + "\" > \"" +
        xsflib::Utf8(folder / (stem + ".log")) + "\" 2>&1";
    std::vector<uint8_t> bytes;
    if (std::system(command.c_str()) != 0 || !xsflib::ReadFile(raw, bytes))
    {
        return std::nullopt;
    }

    // Little-endian 32-bit floats, left and right in turn.
    std::vector<float> samples(bytes.size() / 4);
    for (std::size_t i = 0; i < samples.size(); i++)
    {
        const uint32_t word = static_cast<uint32_t>(bytes[4 * i]) | static_cast<uint32_t>(bytes[4 * i + 1]) << 8 |
                              static_cast<uint32_t>(bytes[4 * i + 2]) << 16 |
                              static_cast<uint32_t>(bytes[4 * i + 3]) << 24;
        samples[i] = std::bit_cast<float>(word);
    }
    if (!keep)
    {
        std::filesystem::remove(midi);
        std::filesystem::remove(sf2);
        std::filesystem::remove(raw);
        std::filesystem::remove(folder / (stem + ".log"));
    }

    return samples;
}

// The median of `values`, or 0 for none.
double Median(std::vector<double> values)
{
    if (values.empty())
    {
        return 0.0;
    }

    std::sort(values.begin(), values.end());
    const std::size_t middle = values.size() / 2;
    return values.size() % 2 == 1 ? values[middle] : (values[middle - 1] + values[middle]) / 2.0;
}

// The options of a run: the part of the sounds that it checks, how often a sound also renders through FluidSynth and
// where its files are written, the sound to check, if only one, and the archive.
struct Options
{
    std::optional<Part> part;
    int every = 0;
    std::filesystem::path folder;
    std::string only;
    std::string archive;
};

// The options in the command line's `arguments`, or nothing after the usage when they don't fit it.
std::optional<Options> ParseOptions(const std::vector<std::string>& arguments)
{
    std::optional<Part> part = Part{};
    int every = 0;
    std::filesystem::path folder;
    std::vector<std::string> rest;
    for (std::size_t i = 1; i < arguments.size(); i++)
    {
        if (arguments[i] == "--part" && i + 1 < arguments.size())
        {
            part = PartOf(arguments[++i]);
        }
        else if (arguments[i] == "--fluidsynth" && i + 2 < arguments.size())
        {
            every = std::atoi(arguments[i + 1].c_str());
            folder = xsflib::PathFromUtf8(arguments[i + 2]);
            i += 2;
        }
        else
        {
            rest.push_back(arguments[i]);
        }
    }

    if (rest.empty() || rest.size() > 2 || !part || (every < 0) || (!folder.empty() && every == 0))
    {
        std::fprintf(stderr,
                     "usage: hokuto_check_synth [--part <k>/<n>] [--fluidsynth <every> <folder>] <archive> [sound]\n");
        return std::nullopt;
    }
    const std::string only = rest.size() > 1 ? rest[1] : std::string();

    return Options{part, every, folder, only, rest[0]};
}

// Renders sound `sound` of `info` through FluidSynth too, and adds the difference in level from `render`, Hokuto's
// render of `conversion`, to `differences`. FluidSynth's files go in `folder`, and stay there with `only`. `name` is
// the archive's name.
void CompareWithFluidSynth(const ntdlib::sequence_export::Conversion& conversion, const StereoRender& render,
                           const std::string& name, const sar::SoundInfo& info, uint32_t sound, const std::string& only,
                           const std::filesystem::path& folder, std::vector<double>& differences)
{
    const std::string stem = name + "-" + std::to_string(sound);
    const bool keep = !only.empty();
    const std::optional<std::vector<float>> theirs = FluidSynthRender(conversion, folder, stem, keep);
    if (!theirs)
    {
        std::printf("%s: FluidSynth fails to render %s\n", name.c_str(), info.name.c_str());
        return;
    }
    if (keep)
    {
        xsflib::WriteFile(folder / (stem + ".hokuto.raw"), RawFloats(render.samples));
    }

    const std::size_t common = std::min(render.samples.size(), theirs->size());
    const double ours = Dbfs(Rms(render.samples, common));
    const double fluidsynth = Dbfs(Rms(*theirs, common));
    std::printf("%s: %s plays at %.2f dBFS, and %.2f dBFS in FluidSynth, over %.1f s\n", name.c_str(),
                info.name.c_str(), ours, fluidsynth, static_cast<double>(common / 2) / kRate);
    if (ours > -100.0 && fluidsynth > -100.0)
    {
        differences.push_back(fluidsynth - ours);
    }
}

// The counts of a run, the highest peak, the times of the renders and the differences from FluidSynth.
struct Tallies
{
    std::size_t sounds = 0;
    std::size_t not_converted = 0;
    std::size_t failed = 0;
    std::size_t not_finite = 0;
    std::size_t silent = 0;
    std::size_t above_full_scale = 0;
    std::size_t sounding = 0;
    double highest = 0.0;
    std::string highest_sound;
    double audio_seconds = 0.0;
    double render_seconds = 0.0;
    double slowest = 0.0;
    std::string slowest_sound;
    std::vector<double> differences;
};

// Converts sound `sound` of `archive`, `info`, and renders it, with the options `o`. Prints a line for each finding,
// under the archive's `name`, and counts the sound in `tallies`.
void CheckSynthSound(const sar::SoundArchive& archive, const std::string& name, uint32_t sound,
                     const sar::SoundInfo& info, const Options& o, Tallies& tallies)
{
    tallies.sounds++;
    std::optional<ntdlib::sequence_export::Conversion> conversion;
    try
    {
        conversion = hokuto::song::ConvertToMidi(archive, sound, {}, {}, "Hokuto");
    }
    catch (const std::exception& error)
    {
        std::printf("%s: %s doesn't convert: %s\n", name.c_str(), info.name.c_str(), error.what());
        tallies.not_converted++;
        return;
    }

    StereoRender render;
    const auto began = std::chrono::steady_clock::now();
    try
    {
        render = hokuto::synth::RenderSoundFont(conversion->midi, conversion->sf2, kRate, kMostSeconds);
    }
    catch (const std::exception& error)
    {
        std::printf("%s: %s fails to render: %s\n", name.c_str(), info.name.c_str(), error.what());
        tallies.failed++;
        return;
    }
    const double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();

    const Measures measures = Measure(render.samples, render.rate);
    tallies.audio_seconds += measures.seconds;
    tallies.render_seconds += took;
    if (took > tallies.slowest)
    {
        tallies.slowest = took;
        tallies.slowest_sound = info.name;
    }
    if (measures.not_finite > 0)
    {
        std::printf("%s: %s gives %zu samples that aren't finite\n", name.c_str(), info.name.c_str(),
                    measures.not_finite);
        tallies.not_finite++;
    }
    if (measures.peak == 0.0)
    {
        tallies.silent++;
        return;
    }
    if (measures.peak > 1.0)
    {
        std::printf("%s: %s peaks at %+.2f dBFS\n", name.c_str(), info.name.c_str(), Dbfs(measures.peak));
        tallies.above_full_scale++;
    }
    if (measures.peak > tallies.highest)
    {
        tallies.highest = measures.peak;
        tallies.highest_sound = info.name;
    }

    // Every nth sound that sounds goes through FluidSynth too.
    if (o.every > 0 && tallies.sounding++ % static_cast<std::size_t>(o.every) == 0)
    {
        CompareWithFluidSynth(*conversion, render, name, info, sound, o.only, o.folder, tallies.differences);
    }
}

// Prints the summary of `tallies`, under the archive's `name`.
void PrintTallies(const std::string& name, const Tallies& tallies)
{
    std::printf(
        "%s: %zu sequence sounds. %zu don't convert, %zu fail to render and %zu give samples that aren't "
        "finite. %zu are silent, and %zu peak above full scale.\n",
        name.c_str(), tallies.sounds, tallies.not_converted, tallies.failed, tallies.not_finite, tallies.silent,
        tallies.above_full_scale);
    if (!tallies.highest_sound.empty())
    {
        std::printf("%s: the highest peak is %+.2f dBFS, in %s.\n", name.c_str(), Dbfs(tallies.highest),
                    tallies.highest_sound.c_str());
    }
    if (tallies.render_seconds > 0.0)
    {
        std::printf("%s: %.0f s of audio took %.1f s, %.0f times real time. The slowest render took %.2f s, for %s.\n",
                    name.c_str(), tallies.audio_seconds, tallies.render_seconds,
                    tallies.audio_seconds / tallies.render_seconds, tallies.slowest, tallies.slowest_sound.c_str());
    }
    if (!tallies.differences.empty())
    {
        const std::vector<double>& differences = tallies.differences;
        std::printf(
            "%s: FluidSynth played %zu sounds a median of %+.2f dB from Hokuto's renderer, from %+.2f to %+.2f "
            "dB.\n",
            name.c_str(), differences.size(), Median(differences),
            *std::min_element(differences.begin(), differences.end()),
            *std::max_element(differences.begin(), differences.end()));
    }
}

// The tool, with the command line's `arguments`. Returns the tool's exit status.
int Run(const std::vector<std::string>& arguments)
{
    // The options, the archive and the sound.
    const std::optional<Options> options = ParseOptions(arguments);
    if (!options)
    {
        return 2;
    }
    const Options& o = *options;

    const std::filesystem::path path = xsflib::PathFromUtf8(o.archive);
    const std::string name = xsflib::Utf8(path.filename());
    std::shared_ptr<const sar::SoundArchive> archive;
    try
    {
        archive = hokuto::song::OpenArchive(path);
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "%s: %s\n", name.c_str(), error.what());
        return 2;
    }

    // Each sequence sound of the part: its conversion, then its render.
    Tallies tallies;
    for (uint32_t sound = 0; sound < archive->Sounds().size(); sound++)
    {
        const sar::SoundInfo& info = archive->Sounds()[sound];
        if (info.type != sar::SoundType::kSequence || (!o.only.empty() && info.name != o.only) ||
            sound % o.part->n != o.part->k - 1)
        {
            continue;
        }

        CheckSynthSound(*archive, name, sound, info, o, tallies);
    }
    PrintTallies(name, tallies);

    return tallies.not_converted > 0 || tallies.failed > 0 || tallies.not_finite > 0 ? 1 : 0;
}

} // namespace

int main(int argc, char** argv)
{
    return Run(xsflib::console::Arguments(argc, argv));
}
