// SPDX-License-Identifier: MIT

// Checks the exports to MIDI with SoundFont 2 or SFZ (src/song/midi_export.h) against the files of ntdlib's converter
// of each console's sequences, with that console's settings: the Switch's, the Wii U's, the 3DS's or the Wii's. The
// tool runs the converter in its process. The tool opens a sound archive twice:
//
// - as the converter opens it, with the group files beside it, and
// - as the program imports it into a Hokuto file. The file then builds the archive again, and the export reads that.
//
// Each sequence sound converts on both sides with the converter's options. A sound of the 3DS or the Wii U starts as
// the game starts it, as the converters of those consoles play it: the converter's side takes the start from ntdlib's
// lists by the archive's SHA-1, and Hokuto's side from the file that the import made. The tool compares the MIDI file,
// the SoundFont 2 file, each SFZ file and sample, and the report, byte for byte. A sound that fails on both sides with
// the same error counts as the same. The tests check that the files go where the converters put them.
//
// A GBA ROM converts with agblib's converter of MusicPlayer2000 and the default settings of
// agblib::sequence_export::agb::ConvertSettings. The converter's side reads the ROM, and Hokuto's side the ROM that the
// Hokuto file builds. Each song that plays converts on both sides. The tool compares the MIDI file, the SoundFont 2
// file and the warnings. It then checks the voice groups that the browser lists (song::GbaVoiceGroups). Each program
// that ntdlib's performer selects as it plays a song has to be among the programs of the song's voice group. Each voice
// group is exported as the browser's Export… writes it (song::ConvertVoiceGroup). Its SoundFont has to contain a preset
// in bank 0 at each program at which a song's SoundFont contains one, and each listed program has to get a preset or a
// warning.
//
// A GBA ROM with another of the GBA's drivers, such as Konami's, converts with ntdlib's converter of that driver and
// the same settings: each sound of the driver (song::DriverRom::Sounds), such as a song's live version. With
// --reference and a folder, the tool also compares each MIDI file and SoundFont 2 file with the file of the same name
// in that folder. Konami's converter writes SFZ files too. Each SFZ file and sample of Hokuto's export has to match the
// files of the converter, and the summary counts them. A ROM with MusicPlayer2000 beside another driver imports with an
// archive for each (format::ImportGbaArchives), and each archive converts as above.
//
// A DS cartridge image, or a sound archive of the DS (SDAT), converts with ntdlib's converter of NITRO-Composer's
// sequences, with the build of the driver that the ARM7 binary of the image contains. ntdlib's frame of a DS converter
// runs the converter the same way. Each sound archive of an image converts on both sides: the converter's side reads
// the archive in the image, and Hokuto's side the archive that the Hokuto file of that archive builds. The tool
// compares the MIDI file, the SoundFont 2 file, each SFZ file and sample, and the report. A loose archive uses the
// default build of the driver. The converter side uses the profile of the game of the image
// (ntdlib::ntr::GameProfileOf) as ntdlib's DS converter uses it, without the output effect: the image's game code, the
// path of the archive's file and its place among the file's archives. Hokuto's side uses the game that the Hokuto file
// stores.
//
// A DS cartridge image without a sound archive uses its DSE files (song::DseSounds). Each sound converts with ntdlib's
// converter of DSE's sequences, with the build of the driver that the image contains and each song in the first state
// that ntdlib's dse::GameStarts gives it. ntdlib's frame of a DSE converter runs the converter the same way. The
// converter's side reads the set of the game's image, and Hokuto's side the set that the Hokuto file builds, with the
// file's starts. The tool compares the same files as on the DS.
//
// A Wii archive converts with the build of NW4R SND that its version gives. Hokuto converts each sound by itself as the
// archive gives it.
//
// The tool prints a line for each sound that differs, and a summary. It returns 1 when a sound differs, and 2 when the
// archive can't be read. With a sound's name, it checks that sound alone.
//
// Both sides write the name of the program that made the files into them: Hokuto, or the name that --program sets. With
// --reference, --program has to give the name that the reference files carry.
//
// usage: hokuto_check_exports [--reference <folder>] [--program <name>] <archive, GBA ROM or DS image> [sound]

#include "check_exports.h"

#include <type_traits>

#include "ntdlib/cafe/snd/sequence_starts.h"
#include "ntdlib/ctr/snd/starting_tracks.h"
#include "xsflib/sha1.h"

#include "format/project.h"
#include "song/starts.h"

namespace hokuto::tools::exports
{

bool SameSfzFiles(const export_::SfzInstruments& a, const export_::SfzInstruments& b)
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
}

bool SameSfzSamples(const export_::SfzInstruments& a, const export_::SfzInstruments& b)
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
}

namespace
{

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
    if (!SameSfzFiles(theirs.sfz, ours.sfz))
    {
        parts.emplace_back("the SFZ files");
    }
    if (!SameSfzSamples(theirs.sfz, ours.sfz))
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

} // namespace

std::string Listed(const std::vector<std::string>& parts)
{
    std::string text;
    for (std::size_t i = 0; i < parts.size(); i++)
    {
        text += (i == 0 ? "" : i + 1 == parts.size() ? " and " : ", ") + parts[i];
    }

    return text;
}

namespace
{

// Sets `options` and `settings` to the start that ntdlib's converter gives sound `sound` of the 3DS's or the Wii U's
// archive whose SHA-1 is `sha1`, as the converter of each console sets them up from ntdlib's lists: the tracks that
// play, the tracks' volumes, the variables at their frames, the generator's state, the tempo ratio, the pitch, the
// volume, the banks and the sends.
void SetUpConvertersStart(sar::Flavor flavor, const std::string& sha1, const std::string& sound,
                          export_::PerformOptions& options, export_::Settings& settings)
{
    if (flavor == sar::Flavor::kCtr)
    {
        const ntdlib::ctr::snd::SequenceStart start = ntdlib::ctr::snd::StartingSequenceTracks(sha1, sound);
        options.tracks = static_cast<uint16_t>(options.tracks & ~(start.muted | start.silenced));
        options.track_volumes = start.track_volumes;
        for (const ntdlib::ctr::snd::SequenceVariable& variable : start.variables)
        {
            if (variable.index == ntdlib::ctr::snd::SequenceVariable::kNone)
            {
                continue;
            }

            if (variable.frame == 0)
            {
                options.variables[variable.index] = variable.value;
            }
            else
            {
                options.changes.push_back({.frame = variable.frame, .index = variable.index, .value = variable.value});
            }
        }
        if (start.seed)
        {
            options.seed = *start.seed;
            settings.random_steps_before_start = 0;
        }

        options.tempo_ratio = start.tempo_ratio;
        options.pitch = start.pitch;
        options.volume = start.volume;
        options.lpf = start.lpf;
        options.banks = start.banks;
        options.fx_sends[0] = start.fx_send[0];
        options.fx_sends[1] = start.fx_send[1];
    }
    else if (flavor == sar::Flavor::kCafe)
    {
        const ntdlib::cafe::snd::SequenceStart start = ntdlib::cafe::snd::StartOfSequence(sha1, sound);
        for (const ntdlib::cafe::snd::SequenceVariable& variable : start.variables)
        {
            if (variable.index == ntdlib::cafe::snd::SequenceVariable::kNone)
            {
                continue;
            }

            if (variable.track == ntdlib::cafe::snd::SequenceVariable::kNoTrack)
            {
                options.variables[variable.index] = variable.value;
            }
            else
            {
                options.track_variables.push_back(
                    {.track = static_cast<uint8_t>(variable.track), .index = variable.index, .value = variable.value});
            }
        }
        options.tracks = static_cast<uint16_t>(options.tracks & start.tracks);
    }
}

// Converts each sequence sound of `theirs`, the converter's archive, and of `ours`, the archive that a Hokuto file
// builds, with the converter's `settings` and the name `program`. Prints a line for each sound that differs. With
// `only`, converts the sound of that name alone. `name` names the archive. A sound of the 3DS or the Wii U starts as
// the game starts it: the converter's side by `sha1`, the SHA-1 of the game's archive, and Hokuto's side by `file`, the
// file that the import made.
template <typename Archive, typename SoundType>
Counts CheckSounds(const Archive& theirs_archive, const Archive& ours_archive, const export_::Settings& settings,
                   const std::string& program, const std::string& name, const std::string& only,
                   const std::string& sha1 = {}, const hokuto::format::File* file = nullptr)
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
        export_::PerformOptions options;
        export_::Settings started = settings;
        if constexpr (std::is_same_v<Archive, sar::SoundArchive>)
        {
            SetUpConvertersStart(theirs_archive.ReadOptions().flavor, sha1, info.name, options, started);
        }
        try
        {
            theirs.conversion = export_::ConvertSequence(theirs_archive, sound, options, started, program);
        }
        catch (const std::exception& error)
        {
            theirs.error = error.what();
        }
        try
        {
            if constexpr (std::is_same_v<Archive, sar::SoundArchive>)
            {
                const hokuto::format::SoundStart start =
                    file != nullptr ? hokuto::format::StartOf(*file, sound) : hokuto::format::SoundStart();
                ours.conversion = hokuto::song::ConvertToMidi(ours_archive, sound, hokuto::song::StartValuesOf(start),
                                                              hokuto::song::ChangesOf(start), program, start);
            }
            else
            {
                ours.conversion = hokuto::song::ConvertToMidi(ours_archive, sound, {}, {}, program);
            }
        }
        catch (const std::exception& error)
        {
            ours.error = error.what();
        }
        CompareSides(theirs, ours, name, info.name, counts);
    }

    return counts;
}

} // namespace

void CompareSides(const Side& theirs, const Side& ours, const std::string& name, const std::string& sound_name,
                  Counts& counts)
{
    if (!theirs.conversion && !ours.conversion)
    {
        const std::string expected = "Can't convert " + sound_name + ". " + theirs.error;
        if (ours.error != expected)
        {
            std::printf("%s: %s fails with other errors: \"%s\" and \"%s\"\n", name.c_str(), sound_name.c_str(),
                        theirs.error.c_str(), ours.error.c_str());
            counts.differ++;
        }
        counts.failed++;
        return;
    }
    if (!theirs.conversion || !ours.conversion)
    {
        std::printf("%s: %s fails only in %s: %s\n", name.c_str(), sound_name.c_str(),
                    theirs.conversion ? "Hokuto" : "ntdlib's converter",
                    (theirs.conversion ? ours.error : theirs.error).c_str());
        counts.differ++;
        return;
    }

    const std::vector<std::string> parts = Differences(*theirs.conversion, *ours.conversion);
    if (!parts.empty())
    {
        std::printf("%s: %s differs in %s\n", name.c_str(), sound_name.c_str(), Listed(parts).c_str());
        counts.differ++;
    }
}

namespace
{

// Checks the conversion of the sounds of the Wii archive at `path`, named `name`, in `bytes`. Returns the tool's exit
// status. Both sides name `program` as the program that made the files.
int CheckWiiArchive(std::vector<uint8_t> bytes, const std::filesystem::path& path, const std::string& name,
                    const std::string& only, const std::string& program)
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
                                                                           settings, program, name, only);
    std::printf("%s: %zu sequence sounds checked. %zu differ, and %zu fail on both sides.\n", name.c_str(),
                counts.sounds, counts.differ, counts.failed);

    return counts.differ > 0 ? 1 : 0;
}

// Checks the conversion of the sounds of the archive of the Switch, the Wii U or the 3DS at `path`, named `name`, in
// `bytes`. Returns the tool's exit status. Both sides name `program` as the program that made the files.
int CheckSarArchive(std::vector<uint8_t> bytes, const std::filesystem::path& path, const std::string& name,
                    const std::string& only, const std::string& program)
{
    // The converter's archive, and the one that the Hokuto file builds. Both have the group files beside the archive.
    std::optional<sar::SoundArchive> converter_archive;
    std::shared_ptr<const sar::SoundArchive> hokuto_archive;
    hokuto::format::File file;
    export_::Settings settings = export_::SwitchSettings();
    try
    {
        const sar::Flavor flavor = hokuto::song::FlavorOfArchive(bytes);
        if (flavor == sar::Flavor::kCafe)
        {
            settings = export_::CafeSettings();
        }
        else if (flavor == sar::Flavor::kCtr)
        {
            settings = export_::CtrSettings();
        }
        converter_archive = export_::OpenArchive<sar::SoundArchive>(path, name, settings, true);
        if (!converter_archive)
        {
            std::fprintf(stderr, "%s can't be read\n", name.c_str());
            return 2;
        }

        const std::shared_ptr<const sar::SoundArchive> opened = hokuto::song::OpenArchive(path);
        file = hokuto::format::ImportArchive(bytes, flavor, opened->GroupFiles(), {}, opened->GroupFileGroups());
        hokuto_archive = hokuto::song::LoadArchive(hokuto::format::ExportArchive(file), name,
                                                   hokuto::format::ExportGroupFiles(file));
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "%s: %s\n", name.c_str(), error.what());
        return 2;
    }

    const std::string sha1 = xsflib::Sha1::Hex(xsflib::Sha1::Of(bytes));
    const Counts counts = CheckSounds<sar::SoundArchive, sar::SoundType>(*converter_archive, *hokuto_archive, settings,
                                                                         program, name, only, sha1, &file);
    std::printf("%s: %zu sequence sounds checked. %zu differ, and %zu fail on both sides.\n", name.c_str(),
                counts.sounds, counts.differ, counts.failed);

    return counts.differ > 0 ? 1 : 0;
}

// The options of the command line: the folder of the converter's references, the name of the program that made the
// files, the file, and the sound to check, if only one.
struct Options
{
    std::filesystem::path reference_folder;
    std::string program = "Hokuto";
    std::filesystem::path path;
    std::string only;
};

// The options in the command line's `arguments`, or nothing after the usage.
std::optional<Options> ParseArguments(std::vector<std::string> arguments)
{
    std::filesystem::path reference_folder;
    std::string program = "Hokuto";
    while (arguments.size() > 2 && (arguments[1] == "--reference" || arguments[1] == "--program"))
    {
        if (arguments[1] == "--reference")
        {
            reference_folder = xsflib::PathFromUtf8(arguments[2]);
        }
        else
        {
            program = arguments[2];
        }
        arguments.erase(arguments.begin() + 1, arguments.begin() + 3);
    }
    if (arguments.size() < 2 || arguments.size() > 3)
    {
        std::fprintf(stderr,
                     "usage: hokuto_check_exports [--reference <folder>] [--program <name>] "
                     "<archive, GBA ROM or DS image> [sound]\n");
        return std::nullopt;
    }
    const std::string only = arguments.size() > 2 ? arguments[2] : std::string();

    return Options{reference_folder, program, xsflib::PathFromUtf8(arguments[1]), only};
}

// Checks each archive of the GBA ROM `bytes`, as the program imports it (format::ImportGbaArchives). A ROM that the
// import refuses checks as the archive of its first driver, or else of MusicPlayer2000, and the check names the
// failure. Returns the worst result of the archives.
int CheckGbaArchives(std::vector<uint8_t> bytes, const std::string& name, const std::string& only,
                     const std::filesystem::path& reference_folder, const std::string& stem, const std::string& program)
{
    bool mp2k = false;
    bool driver = false;
    try
    {
        for (const hokuto::format::File& archive : hokuto::format::ImportGbaArchives(bytes).project.archives)
        {
            (archive.engine == hokuto::format::Engine::kGba ? mp2k : driver) = true;
        }
    }
    catch (const std::exception&)
    {
        driver = hokuto::format::DriverOfRom(bytes) != nullptr;
        mp2k = !driver;
    }

    const int mp2k_result = mp2k ? CheckGbaRom(bytes, name, only, program) : 0;
    const int driver_result =
        driver ? CheckDriverRom(std::move(bytes), name, only, reference_folder, stem, program) : 0;

    return std::max(mp2k_result, driver_result);
}

} // namespace

} // namespace hokuto::tools::exports

using hokuto::tools::exports::CheckDs;
using hokuto::tools::exports::CheckDse;
using hokuto::tools::exports::CheckGbaArchives;
using hokuto::tools::exports::CheckSarArchive;
using hokuto::tools::exports::CheckWiiArchive;
using hokuto::tools::exports::Options;
using hokuto::tools::exports::ParseArguments;

int main(int argc, char** argv)
{
    const std::optional<Options> options = ParseArguments(xsflib::console::Arguments(argc, argv));
    if (!options)
    {
        return 2;
    }
    const auto& [reference_folder, program, path, only] = *options;

    const std::string name = xsflib::Utf8(path.filename());
    std::vector<uint8_t> bytes;
    try
    {
        if (!xsflib::ReadFile(path, bytes))
        {
            std::fprintf(stderr, "%s can't be read\n", name.c_str());
            return 2;
        }
        if (bytes.size() >= 8 && std::equal(bytes.begin(), bytes.begin() + 4, "RSAR"))
        {
            return CheckWiiArchive(std::move(bytes), path, name, only, program);
        }
        if (hokuto::format::IsGbaRom(bytes))
        {
            return CheckGbaArchives(std::move(bytes), name, only, reference_folder, xsflib::Utf8(path.stem()), program);
        }
        if (hokuto::format::IsDsImage(bytes) && hokuto::format::DsArchivesIn(bytes).empty() &&
            hokuto::format::HoldsDseFiles(bytes))
        {
            return CheckDse(bytes, name, only, program);
        }
        if (hokuto::format::IsDsImage(bytes) || hokuto::format::IsDsArchive(bytes))
        {
            return CheckDs(std::move(bytes), name, only, program);
        }
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "%s: %s\n", name.c_str(), error.what());
        return 2;
    }

    return CheckSarArchive(std::move(bytes), path, name, only, program);
}
