// SPDX-License-Identifier: MIT

// Checks the exports to MIDI with SoundFont 2 or SFZ (src/song/midi_export.h) against the files of hacsf2, of cafesf2
// for a Wii U archive, of citrusf2 for a 3DS archive, or of revosf2 for a Wii archive. ntdlib's converter makes the
// converters' files, and the tool runs that converter in its process. The tool opens a sound archive twice:
//
// - as the converter opens it, with the group files beside it, and
// - as the program imports it into a Hokuto file. The file then builds the archive again, and the export reads that.
//
// Each sequence sound converts on both sides with the converter's options: no variable set and no changes. Hokuto's
// side names the converter as the program that made the files. The tool compares the MIDI file, the SoundFont 2 file,
// each SFZ file and sample, and the report, byte for byte. A sound that fails on both sides with the same error counts
// as the same. The tests check that the files go where the converters put them.
//
// A Wii archive converts with the build of NW4R SND that its version gives. revosf2 converts an archive without the
// game's main.dol the same way. revosf2 converts a few games' sounds otherwise: with the build that the game's main.dol
// shows, as pieces of several sequences, or set up as the game's code starts them. Hokuto converts each sound by itself
// as the archive gives it.
//
// The tool prints a line for each sound that differs, and a summary. It returns 1 when a sound differs, and 2 when the
// archive can't be read. With a sound's name, it checks that sound alone.
//
// usage: hokuto_check_exports <archive> [sound]

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/rsar/formats.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sequence_export/archive_converter.h"
#include "ntdlib/sequence_export/consoles.h"
#include "ntdlib/sequence_export/convert.h"
#include "xsflib/bytes.h"
#include "xsflib/console.h"
#include "xsflib/files.h"

#include "format/archive.h"
#include "format/file.h"
#include "song/archive.h"
#include "song/midi_export.h"

namespace
{

namespace export_ = ntdlib::sequence_export;
namespace rsar = ntdlib::rsar;
namespace sar = ntdlib::sar;

// One side's conversion of a sound, or its error.
struct Side
{
    std::optional<export_::Conversion> conversion;
    std::string error;
};

// The parts of two conversions of a sound that differ, by name. Empty when they're the same.
std::vector<std::string> Differences(const export_::Conversion& theirs, const export_::Conversion& ours)
{
    std::vector<std::string> parts;
    if (theirs.midi != ours.midi)
    {
        parts.emplace_back("the MIDI file");
    }
    if (theirs.sf2 != ours.sf2)
    {
        parts.emplace_back("the SoundFont 2 file");
    }

    // The SFZ files and samples, in their order.
    const auto same_files = [](const export_::SfzInstruments& a, const export_::SfzInstruments& b)
    {
        if (a.files.size() != b.files.size())
        {
            return false;
        }

        for (std::size_t i = 0; i < a.files.size(); i++)
        {
            if (a.files[i].name != b.files[i].name || a.files[i].text != b.files[i].text)
            {
                return false;
            }
        }

        return true;
    };

    const auto same_samples = [](const export_::SfzInstruments& a, const export_::SfzInstruments& b)
    {
        if (a.samples.size() != b.samples.size())
        {
            return false;
        }

        for (std::size_t i = 0; i < a.samples.size(); i++)
        {
            if (a.samples[i].name != b.samples[i].name || a.samples[i].wav != b.samples[i].wav)
            {
                return false;
            }
        }

        return true;
    };

    if (!same_files(theirs.sfz, ours.sfz))
    {
        parts.emplace_back("the SFZ files");
    }
    if (!same_samples(theirs.sfz, ours.sfz))
    {
        parts.emplace_back("the SFZ samples");
    }

    // What the converter reports: the program shows it, and the converters print it.
    if (hokuto::song::ConversionReport(theirs) != hokuto::song::ConversionReport(ours) ||
        theirs.samples != ours.samples || theirs.channels != ours.channels)
    {
        parts.emplace_back("the report");
    }

    return parts;
}

// The parts joined into a list for a line: "a, b and c".
std::string Listed(const std::vector<std::string>& parts)
{
    std::string text;
    for (std::size_t i = 0; i < parts.size(); i++)
    {
        text += (i == 0 ? "" : i + 1 == parts.size() ? " and " : ", ") + parts[i];
    }

    return text;
}

// What a check of an archive's sounds found.
struct Counts
{
    std::size_t sounds = 0;
    std::size_t differ = 0;
    std::size_t failed = 0;
};

// Converts each sequence sound of `theirs`, the converter's archive, and of `ours`, the archive that a Hokuto file
// builds, with the converter's `settings` and the name `program`. Prints a line for each sound that differs. With
// `only`, converts the sound of that name alone. `name` names the archive.
template <typename Archive, typename SoundType>
Counts CheckSounds(const Archive& theirs_archive, const Archive& ours_archive, const export_::Settings& settings,
                   const std::string& program, const std::string& name, const std::string& only)
{
    Counts counts;
    for (uint32_t sound = 0; sound < theirs_archive.Sounds().size(); sound++)
    {
        const auto& info = theirs_archive.Sounds()[sound];
        if (info.type != SoundType::kSequence || (!only.empty() && info.name != only))
        {
            continue;
        }

        counts.sounds++;
        Side theirs;
        Side ours;
        try
        {
            theirs.conversion =
                export_::ConvertSequence(theirs_archive, sound, export_::PerformOptions(), settings, program);
        }
        catch (const std::exception& error)
        {
            theirs.error = error.what();
        }
        try
        {
            ours.conversion = hokuto::song::ConvertToMidi(ours_archive, sound, {}, {}, program);
        }
        catch (const std::exception& error)
        {
            ours.error = error.what();
        }

        // Hokuto's message names the sound before ntdlib's.
        if (!theirs.conversion && !ours.conversion)
        {
            const std::string expected = "Hokuto can't convert " + info.name + ". " + theirs.error;
            if (ours.error != expected)
            {
                std::printf("%s: %s fails with other errors: \"%s\" and \"%s\"\n", name.c_str(), info.name.c_str(),
                            theirs.error.c_str(), ours.error.c_str());
                counts.differ++;
            }
            counts.failed++;
            continue;
        }
        if (!theirs.conversion || !ours.conversion)
        {
            std::printf("%s: %s fails only in %s: %s\n", name.c_str(), info.name.c_str(),
                        theirs.conversion ? "Hokuto" : program.c_str(),
                        (theirs.conversion ? ours.error : theirs.error).c_str());
            counts.differ++;
            continue;
        }

        const std::vector<std::string> parts = Differences(*theirs.conversion, *ours.conversion);
        if (!parts.empty())
        {
            std::printf("%s: %s differs in %s\n", name.c_str(), info.name.c_str(), Listed(parts).c_str());
            counts.differ++;
        }
    }

    return counts;
}

// Checks revosf2's conversion of the sounds of the Wii archive at `path`, named `name`, in `bytes`. Returns the tool's
// exit status.
int CheckWiiArchive(std::vector<uint8_t> bytes, const std::filesystem::path& path, const std::string& name,
                    const std::string& only)
{
    std::optional<rsar::SoundArchive> converter_archive;
    std::shared_ptr<const rsar::SoundArchive> hokuto_archive;
    export_::Settings settings;
    try
    {
        settings = export_::RevolutionSettings(export_::BuildOfRsarVersion(xsflib::LoadBe16(bytes.data() + 6)));
        converter_archive = export_::OpenArchive<rsar::SoundArchive>(path, name, settings, false);
        if (!converter_archive)
        {
            std::fprintf(stderr, "%s can't be read\n", name.c_str());
            return 2;
        }

        const hokuto::format::File file = hokuto::format::ImportWiiArchive(bytes);
        hokuto_archive = hokuto::song::LoadWiiArchive(hokuto::format::ExportArchive(file), name);
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "%s: %s\n", name.c_str(), error.what());
        return 2;
    }

    const Counts counts = CheckSounds<rsar::SoundArchive, rsar::SoundType>(*converter_archive, *hokuto_archive,
                                                                           settings, "revosf2", name, only);
    std::printf("%s: %zu sequence sounds checked. %zu differ, and %zu fail on both sides.\n", name.c_str(),
                counts.sounds, counts.differ, counts.failed);

    return counts.differ > 0 ? 1 : 0;
}

} // namespace

int main(int argc, char** argv)
{
    const std::vector<std::string> arguments = xsflib::console::Arguments(argc, argv);
    if (arguments.size() < 2 || arguments.size() > 3)
    {
        std::fprintf(stderr, "usage: hokuto_check_exports <archive> [sound]\n");
        return 2;
    }
    const std::string only = arguments.size() > 2 ? arguments[2] : std::string();

    // The converter's archive, and the one that the Hokuto file builds. Both have the group files beside the archive. A
    // Wii U archive converts as cafesf2 converts it, a 3DS archive as citrusf2 does, and a Switch archive as hacsf2
    // does.
    const std::filesystem::path path = xsflib::PathFromUtf8(arguments[1]);
    const std::string name = xsflib::Utf8(path.filename());
    std::optional<sar::SoundArchive> converter_archive;
    std::shared_ptr<const sar::SoundArchive> hokuto_archive;
    export_::Settings settings = export_::SwitchSettings();
    std::string program = "hacsf2";
    try
    {
        std::vector<uint8_t> bytes;
        if (!xsflib::ReadFile(path, bytes))
        {
            std::fprintf(stderr, "%s can't be read\n", name.c_str());
            return 2;
        }
        if (bytes.size() >= 8 && std::equal(bytes.begin(), bytes.begin() + 4, "RSAR"))
        {
            return CheckWiiArchive(std::move(bytes), path, name, only);
        }
        const sar::Flavor flavor = hokuto::song::FlavorOfArchive(bytes);
        if (flavor == sar::Flavor::kCafe)
        {
            settings = export_::CafeSettings();
            program = "cafesf2";
        }
        else if (flavor == sar::Flavor::kCtr)
        {
            settings = export_::CtrSettings();
            program = "citrusf2";
        }
        converter_archive = export_::OpenArchive<sar::SoundArchive>(path, name, settings, true);
        if (!converter_archive)
        {
            std::fprintf(stderr, "%s can't be read\n", name.c_str());
            return 2;
        }

        const std::shared_ptr<const sar::SoundArchive> opened = hokuto::song::OpenArchive(path);
        const hokuto::format::File file =
            hokuto::format::ImportArchive(bytes, flavor, opened->GroupFiles(), {}, opened->GroupFileGroups());
        hokuto_archive = hokuto::song::LoadArchive(hokuto::format::ExportArchive(file), name,
                                                   hokuto::format::ExportGroupFiles(file));
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "%s: %s\n", name.c_str(), error.what());
        return 2;
    }

    const Counts counts = CheckSounds<sar::SoundArchive, sar::SoundType>(*converter_archive, *hokuto_archive, settings,
                                                                         program, name, only);
    std::printf("%s: %zu sequence sounds checked. %zu differ, and %zu fail on both sides.\n", name.c_str(),
                counts.sounds, counts.differ, counts.failed);

    return counts.differ > 0 ? 1 : 0;
}
