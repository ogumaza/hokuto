// SPDX-License-Identifier: MIT

// Renders every sequence sound of a sound archive of the Switch or the Wii U through its export to MIDI and SFZ, with
// sfizz (src/synth/sfz_synth.h). Like the program, the tool opens the archive with the group files beside it. Each
// sequence sound converts as File > Export converts it, with no variable set and no changes. sfizz then plays the MIDI
// file through the SFZ files at 48 kHz, up to 2 s past the file's last event. A sequence that plays no notes has no SFZ
// files, and the tool counts it apart.
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
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "ntdlib/sar/formats.h"
#include "ntdlib/sequence_export/convert.h"
#include "xsflib/console.h"
#include "xsflib/files.h"

#include "song/archive.h"
#include "song/midi_export.h"
#include "synth/sfz_synth.h"

namespace
{

namespace export_ = ntdlib::sequence_export;
namespace sar = ntdlib::sar;

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

// Reads "k/n" into `part` and `parts`. Returns false when the text isn't such a part.
bool ReadPart(const std::string& text, std::size_t& part, std::size_t& parts)
{
    const std::size_t slash = text.find('/');
    if (slash == std::string::npos)
    {
        return false;
    }
    try
    {
        part = std::stoul(text.substr(0, slash));
        parts = std::stoul(text.substr(slash + 1));
    }
    catch (const std::exception&)
    {
        return false;
    }

    return parts > 0 && part >= 1 && part <= parts;
}

} // namespace

int main(int argc, char** argv)
{
    const std::vector<std::string> arguments = xsflib::console::Arguments(argc, argv);
    std::string only;
    std::size_t part = 1;
    std::size_t parts = 1;
    std::vector<std::string> positional;
    for (std::size_t i = 1; i < arguments.size(); i++)
    {
        if (arguments[i] == "--part" && i + 1 < arguments.size() && ReadPart(arguments[i + 1], part, parts))
        {
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
        return 2;
    }
    if (positional.size() > 1)
    {
        only = positional[1];
    }

    const std::filesystem::path path = xsflib::PathFromUtf8(positional[0]);
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
    const auto start = std::chrono::steady_clock::now();
    for (uint32_t sound = 0; sound < archive->Sounds().size(); sound++)
    {
        const sar::SoundInfo& info = archive->Sounds()[sound];
        if (info.type != sar::SoundType::kSequence || (!only.empty() && info.name != only))
        {
            continue;
        }
        sequences++;
        if ((sequences - 1) % parts != part - 1)
        {
            continue;
        }

        export_::Conversion conversion;
        try
        {
            conversion = hokuto::song::ConvertToMidi(*archive, sound, {}, {}, "Hokuto");
        }
        catch (const std::exception& error)
        {
            std::printf("%s: %s: the conversion fails: %s\n", name.c_str(), info.name.c_str(), error.what());
            failed_conversions++;
            continue;
        }
        if (conversion.sfz.files.empty())
        {
            without_notes++;
            continue;
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
            failed_renders++;
            not_finite += message.find("aren't finite") != std::string::npos ? 1 : 0;
            continue;
        }
        const double took = SecondsSince(render_start);

        const double peak = PeakDb(render.samples);
        std::printf("%s: %s: %.1f s, peak %.2f dBFS, rendered in %.2f s\n", name.c_str(), info.name.c_str(), seconds,
                    peak, took);
        rendered++;
        audio_seconds += seconds;
        render_seconds += took;
        above_full_scale += peak > 0.0 ? 1 : 0;
        silent += peak < kSilentDb ? 1 : 0;
        if (peak > loudest)
        {
            loudest = peak;
            loudest_name = info.name;
        }
        if (took > slowest)
        {
            slowest = took;
            slowest_name = info.name;
        }
    }

    std::printf(
        "%s: %zu sequence sounds, part %zu of %zu. %zu rendered, %zu without notes. %zu conversions and %zu "
        "renders failed, %zu of them with samples that aren't finite.\n",
        name.c_str(), sequences, part, parts, rendered, without_notes, failed_conversions, failed_renders, not_finite);
    if (rendered > 0)
    {
        std::printf(
            "%s: %zu renders above full scale, %zu silent. The loudest, %s, peaks at %.2f dBFS. %.0f s of audio "
            "rendered in %.1f s, %.0f times as fast as it plays. The slowest, %s, took %.2f s. The run took "
            "%.1f s.\n",
            name.c_str(), above_full_scale, silent, loudest_name.c_str(), loudest, audio_seconds, render_seconds,
            audio_seconds / std::max(render_seconds, 1e-9), slowest_name.c_str(), slowest, SecondsSince(start));
    }

    return failed_conversions + failed_renders > 0 ? 1 : 0;
}
