// SPDX-License-Identifier: MIT

// Renders every sequence sound of a sound archive of the Switch or the Wii U through its export to MIDI and SFZ, with
// sfizz (src/synth/sfz_synth.h). Like the program, the tool opens the archive with the group files beside it. Each
// sequence sound converts as File > Export converts it, with no variable set and no changes. sfizz then plays the MIDI
// file through the SFZ files at 48 kHz, up to 2 s past the file's last event. A sequence that plays no notes has no SFZ
// files, and the tool counts it apart. From a GBA ROM with a driver whose converter writes SFZ files, such as Konami's
// driver, the tool checks each sound of the driver (song::DriverRom::Sounds), from the ROM that its Hokuto file builds.
// A song that plays no notes counts apart there too.
//
// The tool prints a line for each sound: the length of the render, its peak level and the time that it took. A failed
// conversion or render gets a line with the message. sfizz's samples that aren't finite fail the render too. A render
// above full scale, and a silent one, count apart. A summary follows. The tool returns 1 when a conversion or a render
// fails, and 2 when the archive can't be read. With a sound's name, it renders that sound alone. With --part k/n, it
// renders every nth sequence sound from the kth, for runs side by side.
//
// usage: hokuto_check_sfz <archive> [sound] [--part k/n]

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "ntdlib/sar/formats.h"
#include "ntdlib/sequence_export/convert.h"
#include "xsflib/console.h"
#include "xsflib/files.h"

#include "check_part.h"
#include "format/archive.h"
#include "format/drivers.h"
#include "format/file.h"
#include "song/archive.h"
#include "song/driver.h"
#include "song/midi_export.h"
#include "synth/sfz_synth.h"

namespace
{

namespace export_ = ntdlib::sequence_export;
namespace sar = ntdlib::sar;
using hokuto::tools::Part;
using hokuto::tools::PartOf;

constexpr int kRate = 48000;

// The render goes on this far past the MIDI file's last event, for the notes' releases.
constexpr double kTailSeconds = 2.0;

// A render quieter than this counts as silent.
constexpr double kSilentDb = -90.0;

// The seconds since `start`.
double SecondsSince(std::chrono::steady_clock::time_point start)
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

// The peak level of `samples` in dB relative to full scale. Silence gives negative infinity.
double PeakDb(const std::vector<float>& samples)
{
    float peak = 0.0f;
    for (const float sample : samples)
    {
        peak = std::max(peak, std::abs(sample));
    }

    return 20.0 * std::log10(static_cast<double>(peak));
}

// The options of a run: the sound to check, if only one, the part of the sounds that the run checks, and the archive.
struct Options
{
    std::string only;
    std::size_t part = 1;
    std::size_t parts = 1;
    std::string archive;
};

// The options in `arguments`, or nothing after the usage when they don't fit it.
std::optional<Options> ParseArguments(const std::vector<std::string>& arguments)
{
    std::string only;
    std::size_t part = 1;
    std::size_t parts = 1;
    std::vector<std::string> positional;
    for (std::size_t i = 1; i < arguments.size(); i++)
    {
        const std::optional<Part> given =
            arguments[i] == "--part" && i + 1 < arguments.size() ? PartOf(arguments[i + 1]) : std::nullopt;
        if (given)
        {
            part = given->k;
            parts = given->n;
            i++;
        }
        else
        {
            positional.push_back(arguments[i]);
        }
    }

    if (positional.empty() || positional.size() > 2)
    {
        std::fprintf(stderr, "usage: hokuto_check_sfz <archive> [sound] [--part k/n]\n");
        return std::nullopt;
    }
    if (positional.size() > 1)
    {
        only = positional[1];
    }

    return Options{only, part, parts, positional[0]};
}

// A sound: its name, and its conversion. A GBA ROM's driver converts a song without notes with an error that says
// so (song::AgbConversion), and such a song counts as one without notes.
struct Sound
{
    std::string name;
    std::function<export_::Conversion()> convert;
};

// The sequence sounds of the archive or GBA ROM at `path`, with their conversions.
std::vector<Sound> SoundsOf(const std::filesystem::path& path)
{
    std::vector<Sound> sounds;
    std::vector<uint8_t> bytes;
    const hokuto::format::Driver* driver = nullptr;
    if (xsflib::ReadFile(path, bytes) && hokuto::format::IsGbaRom(bytes))
    {
        driver = hokuto::format::DriverOfRom(bytes);
    }
    if (driver != nullptr)
    {
        std::shared_ptr<const hokuto::song::DriverRom> rom =
            hokuto::song::LoadDriverRom(driver->Facts().engine, hokuto::format::ExportArchive(driver->Import(bytes)));
        if (!rom->WritesSfz())
        {
            throw std::runtime_error("The converter of " + std::string(rom->Rules().Title()) + " writes no SFZ files.");
        }
        for (const uint32_t sound : rom->Sounds())
        {
            sounds.push_back({rom->SoundName(sound), [rom, sound] { return rom->ConvertToMidi(sound, "Hokuto"); }});
        }
    }
    else
    {
        const std::shared_ptr<const sar::SoundArchive> archive = hokuto::song::OpenArchive(path);
        for (uint32_t sound = 0; sound < archive->Sounds().size(); sound++)
        {
            const sar::SoundInfo& info = archive->Sounds()[sound];
            if (info.type == sar::SoundType::kSequence)
            {
                sounds.push_back({info.name, [archive, sound]
                                  { return hokuto::song::ConvertToMidi(*archive, sound, {}, {}, "Hokuto"); }});
            }
        }
    }

    return sounds;
}

// The counts of a run, its loudest render and the times of its renders.
struct Tallies
{
    std::size_t sequences = 0;
    std::size_t rendered = 0;
    std::size_t without_notes = 0;
    std::size_t failed_conversions = 0;
    std::size_t failed_renders = 0;
    std::size_t not_finite = 0;
    std::size_t above_full_scale = 0;
    std::size_t silent = 0;
    double audio_seconds = 0.0;
    double render_seconds = 0.0;
    double loudest = -std::numeric_limits<double>::infinity();
    std::string loudest_name;
    double slowest = 0.0;
    std::string slowest_name;
};

// Converts `info` and renders it. Prints its line, under the archive's `name`, and counts it in `tallies`.
void RenderSound(const Sound& info, const std::string& name, Tallies& tallies)
{
    export_::Conversion conversion;
    try
    {
        conversion = info.convert();
    }
    catch (const hokuto::song::EmptyConversion&)
    {
        tallies.without_notes++;
        return;
    }
    catch (const std::exception& error)
    {
        std::printf("%s: %s: the conversion fails: %s\n", name.c_str(), info.name.c_str(), error.what());
        tallies.failed_conversions++;
        return;
    }
    if (conversion.sfz.files.empty())
    {
        tallies.without_notes++;
        return;
    }

    double seconds = 0.0;
    hokuto::synth::StereoRender render;
    const auto render_start = std::chrono::steady_clock::now();
    try
    {
        seconds = hokuto::synth::MidiSeconds(conversion.midi) + kTailSeconds;
        render = hokuto::synth::RenderSfz(conversion.midi, conversion.sfz, kRate, seconds);
    }
    catch (const std::exception& error)
    {
        const std::string message = error.what();
        std::printf("%s: %s: the render fails: %s\n", name.c_str(), info.name.c_str(), message.c_str());
        tallies.failed_renders++;
        tallies.not_finite += message.find("aren't finite") != std::string::npos ? 1 : 0;
        return;
    }
    const double took = SecondsSince(render_start);

    const double peak = PeakDb(render.samples);
    std::printf("%s: %s: %.1f s, peak %.2f dBFS, rendered in %.2f s\n", name.c_str(), info.name.c_str(), seconds, peak,
                took);
    tallies.rendered++;
    tallies.audio_seconds += seconds;
    tallies.render_seconds += took;
    tallies.above_full_scale += peak > 0.0 ? 1 : 0;
    tallies.silent += peak < kSilentDb ? 1 : 0;
    if (peak > tallies.loudest)
    {
        tallies.loudest = peak;
        tallies.loudest_name = info.name;
    }
    if (took > tallies.slowest)
    {
        tallies.slowest = took;
        tallies.slowest_name = info.name;
    }
}

// Prints the summary of `tallies`, under the archive's `name`, for part `part` of `parts` of a run that began at
// `start`.
void PrintTallies(const std::string& name, const Tallies& tallies, std::size_t part, std::size_t parts,
                  std::chrono::steady_clock::time_point start)
{
    std::printf(
        "%s: %zu sequence sounds, part %zu of %zu. %zu rendered, %zu without notes. %zu conversions and %zu "
        "renders failed, %zu of them with samples that aren't finite.\n",
        name.c_str(), tallies.sequences, part, parts, tallies.rendered, tallies.without_notes,
        tallies.failed_conversions, tallies.failed_renders, tallies.not_finite);
    if (tallies.rendered > 0)
    {
        std::printf(
            "%s: %zu renders above full scale, %zu silent. The loudest, %s, peaks at %.2f dBFS. %.0f s of audio "
            "rendered in %.1f s, %.0f times as fast as it plays. The slowest, %s, took %.2f s. The run took "
            "%.1f s.\n",
            name.c_str(), tallies.above_full_scale, tallies.silent, tallies.loudest_name.c_str(), tallies.loudest,
            tallies.audio_seconds, tallies.render_seconds,
            tallies.audio_seconds / std::max(tallies.render_seconds, 1e-9), tallies.slowest_name.c_str(),
            tallies.slowest, SecondsSince(start));
    }
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
    const auto& [only, part, parts, archive_path] = *options;

    const std::filesystem::path path = xsflib::PathFromUtf8(archive_path);
    const std::string name = xsflib::Utf8(path.filename());
    std::vector<Sound> sounds;
    try
    {
        sounds = SoundsOf(path);
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "%s: %s\n", name.c_str(), error.what());
        return 2;
    }

    Tallies tallies;
    const auto start = std::chrono::steady_clock::now();
    std::size_t index = 0; // the sound's number among those that the run may take, for the part
    for (const Sound& info : sounds)
    {
        if (!only.empty() && info.name != only)
        {
            continue;
        }
        index++;
        if ((index - 1) % parts != part - 1)
        {
            continue;
        }
        tallies.sequences++;
        RenderSound(info, name, tallies);
    }
    PrintTallies(name, tallies, part, parts, start);

    return tallies.failed_conversions + tallies.failed_renders > 0 ? 1 : 0;
}
