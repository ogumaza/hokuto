// SPDX-License-Identifier: MIT

// hok, Hokuto's command-line tool. It makes a .hok file from a sound archive of the Switch, the Wii U, the 3DS or the
// Wii with the group files and the streams beside it, builds the archive back from the file, and writes a .hok file in
// its text form or its binary form, or in the model form.
// It also writes an archive's items as files, its waves as WAV files, and its banks' instruments as SoundFont 2 or SFZ
// files, and measures the lengths of a file's sounds for players. It also writes a sequence sound of a file as a .hok
// file of another console. Wherever hok reads a .hok file, the file can be in either form.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "ntdlib/ntr/rom_writer.h"
#include "ntdlib/rsar/formats.h"
#include "ntdlib/rvl/snd/rsar_options.h"
#include "ntdlib/sar/formats.h"
#include "seqlib/file_name.h"
#include "xsflib/ascii.h"
#include "xsflib/binary.h"
#include "xsflib/cli/tool.h"
#include "xsflib/console.h"
#include "xsflib/files.h"

#include "cli/hok_parts.h"
#include "edit/ds_files.h"
#include "edit/lengths.h"
#include "edit/other_console.h"
#include "edit/timelines.h"
#include "format/archive.h"
#include "format/codes.h"
#include "format/drivers.h"
#include "format/engines.h"
#include "format/file.h"
#include "format/kinds.h"
#include "format/model.h"
#include "format/text.h"
#include "song/aac.h"
#include "song/archive.h"
#include "song/driver.h"
#include "song/ds.h"
#include "song/dse.h"
#include "song/gba.h"
#include "song/gba_assembly.h"
#include "song/gba_samples.h"
#include "song/items.h"
#include "song/midi_export.h"
#include "song/streams.h"

namespace
{

using hokuto::cli::hok_parts::ArchiveInput;
using hokuto::cli::hok_parts::Counted;
using hokuto::cli::hok_parts::WriteBanks;
using hokuto::cli::hok_parts::WriteDriverItems;
using hokuto::cli::hok_parts::WriteDriverSoundFonts;
using hokuto::cli::hok_parts::WriteDriverWavs;
using hokuto::cli::hok_parts::WriteDseItems;
using hokuto::cli::hok_parts::WriteDseWavs;
using hokuto::cli::hok_parts::WriteDsItems;
using hokuto::cli::hok_parts::WriteDsWavs;
using hokuto::cli::hok_parts::WriteGbaItems;
using hokuto::cli::hok_parts::WriteGbaWavs;
using hokuto::cli::hok_parts::WriteItems;
using hokuto::cli::hok_parts::WriteOutput;
using hokuto::cli::hok_parts::WriteVoiceGroups;
using hokuto::cli::hok_parts::WriteWavs;
using hokuto::cli::hok_parts::WriteWiiItems;
using hokuto::cli::hok_parts::WriteWiiWavs;

constexpr const char* kUsage =
    "Usage:\n"
    "  hok import [--dol <main.dol> | --sdat <path>] <archive> <file.hok>\n"
    "                                    Import a sound archive into a .hok file, with the group files and streams\n"
    "                                    next to it. Supported inputs: sound archives of the Switch (.bfsar, .fsst,\n"
    "                                    .fspj), Wii U (.bfsar), 3DS (.bcsar), Wii (.brsar) and DS (.sdat), GBA ROMs\n"
    "                                    (.gba) whose songs use MusicPlayer2000, Konami's driver, Nintendo R&D2's\n"
    "                                    driver, Brownie Brown's driver, Ubisoft Milan's driver, Rare's driver,\n"
    "                                    Quintet's driver or Krawall, and DS cartridge images (.nds).\n"
    "                                    For a Wii archive, the NW4R SND build comes from the game's main.dol: the\n"
    "                                    file that --dol names, or else sys/main.dol in the extracted disc that\n"
    "                                    contains the archive. --dol works only with Wii archives.\n"
    "                                    For a DS image, hok imports the largest sound archive, or the largest one in\n"
    "                                    the file that --sdat names, such as sound/sound_data.sdat. --sdat works only\n"
    "                                    with DS images.\n"
    "  hok export [--filler] [--dsi-key <file>] [--game <folder>] <file.hok> <archive>\n"
    "                                    Build the sound archive, GBA ROM or DS image that a .hok file came from.\n"
    "                                    Streams missing from the game and changed group files are written next to\n"
    "                                    it.\n"
    "                                    A changed group file goes where the game keeps it: into a copy of the SARC\n"
    "                                    archive or the userManagementFiles folder that holds the game's file. hok\n"
    "                                    looks next to the new archive and next to the .hok file. --game names one\n"
    "                                    more folder to look in, such as the folder of the game's archive.\n"
    "                                    A GBA ROM may have no free space after its data for songs that no longer fit\n"
    "                                    in their original places. With --filler, or if the .hok file allows it,\n"
    "                                    those songs move into runs of filler bytes inside the ROM: 16 KiB or more of\n"
    "                                    0xFF bytes that the music driver never reads, or for Konami's, Nintendo\n"
    "                                    R&D2's and Rare's drivers and Krawall, 4 KiB or more of 0xFF or zero bytes.\n"
    "                                    Brownie Brown's, Ubisoft Milan's and Quintet's drivers take none. Hokuto\n"
    "                                    can't check that no other game code reads those bytes.\n"
    "                                    A DS image that also runs on the DSi contains hashes of its data. With\n"
    "                                    --dsi-key, a file with the DSi's 64-byte digest key, the export updates the\n"
    "                                    hashes. The RSA signature of the image still covers the old hashes.\n"
    "  hok text <file.hok> <output>      Save a .hok file in the text form.\n"
    "  hok binary <file.hok> <output>    Save a .hok file in the binary form.\n"
    "  hok model <file.hok> <output>     Save a .hok file in the model form, without the original layout. On export,\n"
    "                                    the archive is laid out as NintendoWare's tools lay it out.\n"
    "  hok lengths [--threads <n>] <file.hok> <output>\n"
    "                                    Measure how long each sequence sound plays before its fade-out, and save the\n"
    "                                    file with the lengths in the binary form. Measures n sounds at a time, at\n"
    "                                    most one per processor thread. Without --threads, uses every thread.\n"
    "  hok items <input> <folder>        Export every item of a sound archive or .hok file to a folder: sequence\n"
    "                                    files, wave sound files, banks, wave archives, waves and streams. For a DS\n"
    "                                    archive: banks, wave archives, waves and streams. For a DS image, the\n"
    "                                    largest sound archive is used. For DSE files: banks and waves. For a GBA\n"
    "                                    ROM: each song and voice group as assembly, and each sample that the voice\n"
    "                                    groups use in its raw form from the ROM. For a ROM with another GBA driver:\n"
    "                                    each song as MIDI with SoundFont 2, and each sample that the songs use as a\n"
    "                                    WAV file.\n"
    "  hok wav <input> <folder>          Export every wave, wave sound and stream of a sound archive or .hok file to\n"
    "                                    a folder as WAV files. For a GBA ROM: every sample that the voice groups\n"
    "                                    use, or with another GBA driver, every sample that the songs use.\n"
    "  hok sf2 <input> <folder>          Convert the instruments of each bank of a sound archive or .hok file to a\n"
    "                                    SoundFont 2 file in a folder. For a GBA ROM, or a .hok file of one: one file\n"
    "                                    per voice group, with the voices that the songs use. For a ROM with another\n"
    "                                    GBA driver: one file with all the samples, and one file per song.\n"
    "  hok sfz <input> <folder>          Convert the instruments of each bank to SFZ files in a folder: one folder\n"
    "                                    per bank, with the samples in a folder named samples. For a ROM with\n"
    "                                    Konami's driver: one folder with all the samples, and one folder per song.\n"
    "  hok other <file.hok> <sound> <output> [console] [mono | stereo]\n"
    "                                    Convert a sequence sound of a .hok file for another console, and save the\n"
    "                                    result as a new .hok file. Give the sound by name or by number. Numbers\n"
    "                                    count from 0 across the whole sound table, including sounds of every type.\n"
    "                                    The console is 3ds, ds, wii, wii_u or switch. By default, songs of the\n"
    "                                    Switch, 3DS, Wii, GBA, DS and DSE convert to the Wii U, and Wii U songs\n"
    "                                    convert to the Switch. A GBA song is given by name or by its number in the\n"
    "                                    song table. A song of another GBA driver, a DS sequence or a DSE sound is\n"
    "                                    given by name or number.\n"
    "                                    With mono, stereo waves become mono for the 3DS. With stereo, they stay\n"
    "                                    stereo. Without either, they become mono only if the song plays more\n"
    "                                    channels at once than the 3DS supports. Warnings list what changes in the\n"
    "                                    conversion.\n"
    "  hok --version                     Print the version.\n"
    "  hok --help                        Print this help. -h and help do the same.\n"
    "Any .hok file that hok reads can be in the text form or the binary form.\n";

std::vector<uint8_t> ReadInput(const std::string& path)
{
    std::vector<uint8_t> bytes;
    if (!xsflib::ReadFile(xsflib::PathFromUtf8(path), bytes))
    {
        throw std::runtime_error("Can't read " + path + ".");
    }
    if (bytes.empty())
    {
        throw std::runtime_error(path + " is empty.");
    }

    return bytes;
}

} // namespace

namespace hokuto::cli::hok_parts
{

// A count and its noun, such as "1 bank" or "40 banks".
std::string Counted(std::size_t count, std::string_view one, std::string_view many)
{
    return std::to_string(count) + " " + std::string(count == 1 ? one : many);
}

void WriteOutput(const std::string& path, std::span<const uint8_t> bytes)
{
    // A folder that isn't there stays missing: a mistyped folder then makes no new folder.
    const std::filesystem::path folder = xsflib::PathFromUtf8(path).parent_path();
    std::error_code missing;
    if (!folder.empty() && !std::filesystem::is_directory(folder, missing))
    {
        throw std::runtime_error("Can't write " + path + ". The folder " + xsflib::Utf8(folder) + " doesn't exist.");
    }
    if (const std::optional<std::string> error = xsflib::WriteFile(xsflib::PathFromUtf8(path), bytes))
    {
        throw std::runtime_error(*error);
    }
}

} // namespace hokuto::cli::hok_parts

namespace
{

// The game's build of NW4R SND for the Wii archive at `archive`: from the main.dol at `dol`, or else from the main.dol
// of the extracted disc that holds the archive. The program looks for the build there too. Nothing without a main.dol.
// Throws for a `dol` that can't be read or holds no AX microcode.
std::optional<ntdlib::SndBuild> GameBuild(const std::filesystem::path& archive, const std::string& dol)
{
    if (!dol.empty())
    {
        const std::optional<ntdlib::SndBuild> build = hokuto::song::ReadWiiBuild(ReadInput(dol));
        if (!build)
        {
            throw std::runtime_error(dol + " isn't a main.dol with the AX microcode.");
        }

        return build;
    }

    std::vector<uint8_t> code;
    const std::optional<std::filesystem::path> found = hokuto::song::FindMainDol(archive);
    return found && xsflib::ReadFile(*found, code) ? hokuto::song::ReadWiiBuild(code) : std::nullopt;
}

// The sound archive that --sdat names in `image`, a DS cartridge image: the largest in the file of that path. Nothing
// without a path. Throws std::runtime_error when the file contains no sound archive.
std::optional<hokuto::format::DsArchivePlace> DsArchiveNamed(std::span<const uint8_t> image, const std::string& path)
{
    if (path.empty())
    {
        return std::nullopt;
    }

    std::optional<hokuto::format::DsArchivePlace> named;
    for (const hokuto::format::DsArchivePlace& place : hokuto::format::DsArchivesIn(image))
    {
        if (place.path == path && (!named || place.size > named->size))
        {
            named = place;
        }
    }
    if (!named)
    {
        throw std::runtime_error("The image has no sound archive at " + path + ".");
    }

    return named;
}

// Prints the place of the sound archive that `file` took from `image`, a DS cartridge image, where the image contains
// several: the path of the archive's file in the image, and its offset where that file contains several archives.
// Prints nothing for another file.
void PrintDsArchivePlace(std::span<const uint8_t> image, const hokuto::format::File& file)
{
    if (!file.native || file.native->engine != hokuto::format::Engine::kDs || !file.native->cartridge ||
        !hokuto::format::IsDsImage(image))
    {
        return;
    }

    const std::vector<hokuto::format::DsArchivePlace> places = hokuto::format::DsArchivesIn(image);
    const hokuto::format::Cartridge& cartridge = *file.native->cartridge;
    const auto taken =
        std::ranges::find_if(places, [&cartridge](const hokuto::format::DsArchivePlace& place)
                             { return place.file == cartridge.file && place.offset == cartridge.offset; });
    if (places.size() < 2 || taken == places.end())
    {
        return;
    }

    const bool shared = std::ranges::count(places, taken->path, &hokuto::format::DsArchivePlace::path) > 1;
    std::printf("The image holds %zu sound archives. Imported the one in %s%s.\n", places.size(), taken->path.c_str(),
                shared ? (" at offset " + std::to_string(taken->offset)).c_str() : "");
}

hokuto::format::File ReadHok(const std::string& path)
{
    try
    {
        return hokuto::format::ReadEitherForm(ReadInput(path));
    }
    catch (const xsflib::FormatError& error)
    {
        throw std::runtime_error(path + ": " + error.what());
    }
}

// Writes the streams of `file` that the game lacks beside the archive at `path`. `archive` holds the archive's bytes,
// and its file table gives the streams' places. The game holds the other streams already.
void WriteStreams(const hokuto::format::File& file, std::span<const uint8_t> archive, const std::string& path)
{
    std::vector<uint32_t> files;
    for (const auto& [id, stream] : file.native->streams)
    {
        if (!stream.from_game)
        {
            files.push_back(id);
        }
    }
    if (files.empty())
    {
        return;
    }

    const std::filesystem::path output = xsflib::PathFromUtf8(path);
    std::vector<uint8_t> bytes(archive.begin(), archive.end());
    const std::string name = xsflib::Utf8(output.filename());
    const std::vector<std::filesystem::path> paths =
        file.native->engine == hokuto::format::Engine::kWii
            ? hokuto::song::StreamPaths(
                  *hokuto::song::LoadWiiArchive(std::move(bytes), name, ntdlib::rvl::snd::kRsarOptions), files)
            : hokuto::song::StreamPaths(
                  *hokuto::song::LoadArchive(std::move(bytes), name, hokuto::format::ExportGroupFiles(file)), files);
    for (std::size_t i = 0; i < files.size(); i++)
    {
        const std::vector<uint8_t>& bytes = *file.native->streams.at(files[i]).bytes;
        if (const std::optional<std::string> error = hokuto::song::WriteStream(output.parent_path() / paths[i], bytes))
        {
            throw std::runtime_error(*error);
        }
    }

    std::printf("Wrote %zu %s next to the archive.\n", files.size(), files.size() == 1 ? "stream" : "streams");
}

// Writes the group files of `file` whose bytes differ from the game's beside `archive`, the archive at `path`. A SARC
// archive beside the archive, or beside the .hok file at `input`, takes the group file of a group whose file it holds
// (format::GroupFileOutputs). A group file whose game's file lies in the userManagementFiles folder there goes into
// that folder (song::PlaceGroupFiles). The game holds the other group files already. A group file whose group Hokuto
// couldn't find stays out, with a message. A non-empty `game` names one more folder to look in after those two, such as
// the folder of the game's archive. The program's File > Export looks in the folder of an archive that the program
// imported. A .hok file stores no path.
void WriteGroupFiles(const hokuto::format::File& file, std::span<const uint8_t> archive, const std::string& input,
                     const std::string& path, const std::string& game)
{
    const std::filesystem::path folder = xsflib::PathFromUtf8(path).parent_path();
    std::vector<hokuto::format::ChangedGroupFile> changed = hokuto::format::ChangedGroupFiles(file);
    for (const hokuto::format::ChangedGroupFile& group_file : changed)
    {
        if (!group_file.group)
        {
            std::fprintf(
                stderr, "Group file %zu changed, but Hokuto can't tell which group it belongs to. It wasn't written.\n",
                group_file.index + 1);
        }
    }

    if (std::ranges::none_of(changed,
                             [](const hokuto::format::ChangedGroupFile& each) { return each.group.has_value(); }))
    {
        return;
    }

    std::vector<std::filesystem::path> folders = {folder, xsflib::PathFromUtf8(input).parent_path()};
    if (!game.empty())
    {
        folders.push_back(xsflib::PathFromUtf8(game));
    }

    const std::vector<hokuto::format::FileBeside> outputs = hokuto::song::PlaceGroupFiles(
        hokuto::format::GroupFileOutputs(std::move(changed), archive, hokuto::song::FlavorOfArchive(archive),
                                         hokuto::song::SarcArchivesIn(folders)),
        folders);
    const std::string managed = std::string(hokuto::song::kUserManagementFolder) + "/";
    std::size_t sarcs = 0;
    std::size_t in_folder = 0;
    for (const auto& [name, bytes] : outputs)
    {
        if (const std::optional<std::string> error =
                hokuto::song::WriteStream(folder / xsflib::PathFromUtf8(name), bytes))
        {
            throw std::runtime_error(*error);
        }

        sarcs += name.ends_with(".sarc") ? 1 : 0;
        in_folder += name.starts_with(managed) ? 1 : 0;
    }
    if (const std::size_t written = outputs.size() - sarcs - in_folder; written > 0)
    {
        std::printf("Wrote %zu %s next to the archive.\n", written, written == 1 ? "group file" : "group files");
    }

    if (in_folder > 0)
    {
        std::printf("Wrote %zu %s to the %s folder next to the archive.\n", in_folder,
                    in_folder == 1 ? "group file" : "group files", hokuto::song::kUserManagementFolder);
    }

    if (sarcs > 0)
    {
        std::printf("Wrote %zu SARC %s with new group files next to the archive.\n", sarcs,
                    sarcs == 1 ? "archive" : "archives");
    }
}

// The ROM or the set of sounds that `hok` builds: a GBA ROM, with MusicPlayer2000 or another driver, a DS archive or a
// set of DSE's files. Nothing for an archive of the Wii or of a later console.
hokuto::song::Played GameOfHok(const hokuto::format::File& hok)
{
    switch (hokuto::format::FactsOf(hok.engine).family)
    {
    case hokuto::format::Family::kMp2k:
        return hokuto::song::LoadGbaRom(hokuto::format::ExportArchive(hok));

    case hokuto::format::Family::kGbaDriver:
        return hokuto::song::LoadDriverRom(hok.engine, hokuto::format::ExportArchive(hok));

    case hokuto::format::Family::kDse:
        return hokuto::song::LoadDseSet(hokuto::format::ExportArchive(hok),
                                        hok.native->cartridge ? hokuto::song::DseBuildOf(*hok.native->cartridge->image)
                                                              : ntdlib::dse::Build::kLater);

    case hokuto::format::Family::kNitroComposer:
        return hokuto::song::LoadDsArchive(hokuto::format::ExportArchive(hok),
                                           hok.native->cartridge
                                               ? hokuto::song::DsBuildOf(*hok.native->cartridge->image)
                                               : ntdlib::ntr::snd::kDefaultBuild,
                                           hokuto::edit::DsGameOfFile(hok));

    case hokuto::format::Family::kNintendoWare:
    case hokuto::format::Family::kNw4r:
        break;
    }

    return {};
}

// The archive of the .hok file at `path`, whose file name is `name`.
ArchiveInput InputOfHok(const std::string& path, const std::string& name)
{
    ArchiveInput input;
    const hokuto::format::File hok = ReadHok(path);
    if (!hok.native)
    {
        throw std::runtime_error(path + " contains no sound archive.");
    }

    input.played = GameOfHok(hok);
    if (input.played)
    {
        return input;
    }

    // An archive of the Wii or of a later console keeps its streams. The engine marks an archive of the Wii.
    for (const auto& [id, stream] : hok.native->streams)
    {
        input.streams[id] = *stream.bytes;
    }

    if (hok.native->engine == hokuto::format::Engine::kWii)
    {
        input.played =
            hokuto::song::LoadWiiArchive(hokuto::format::ExportArchive(hok), name, ntdlib::rvl::snd::kRsarOptions);
        input.build = hok.native->wii_build;
        return input;
    }

    input.played =
        hokuto::song::LoadArchive(hokuto::format::ExportArchive(hok), name, hokuto::format::ExportGroupFiles(hok));

    return input;
}

// The sound archive in the file at `path`: a sound archive with the group files and the streams beside it, or the
// archive that a .hok file in either form builds, with the group files and the streams that the file keeps. An archive
// of the Wii takes the game's build from the main.dol of the extracted disc that holds it. An import takes the build
// the same way. A .hok file of the Wii keeps the build. A GBA ROM, or a .hok file of the GBA, reads as the ROM. A DS
// image reads as the sound archive that an import extracts from it, played by the build of the driver in its ARM7
// binary. A .hok file of the DS keeps the image when it came from one.
ArchiveInput ReadArchive(const std::string& path)
{
    std::vector<uint8_t> bytes = ReadInput(path);
    const std::filesystem::path file = xsflib::PathFromUtf8(path);
    const std::string name = xsflib::Utf8(file.filename());
    ArchiveInput input;

    // A GBA ROM and a DS image are known by a byte or two of their headers. A .hok file can hold the same bytes there.
    using hokuto::format::InputKind;
    switch (hokuto::format::FirstInputKind(bytes, hokuto::format::kInputOrder).value_or(InputKind::kLaterArchive))
    {
    case InputKind::kWiiArchive:
        input.build = GameBuild(file, "");
        input.played = hokuto::song::LoadWiiArchive(std::move(bytes), name, ntdlib::rvl::snd::kRsarOptions);
        input.streams = hokuto::song::ReadStreams(*input.played.Wii(), file);
        break;

    case InputKind::kGbaRom:
        if (const hokuto::format::Driver* driver = hokuto::format::DriverOfRom(bytes))
        {
            input.played = hokuto::song::LoadDriverRom(driver->Facts().engine, std::move(bytes));
        }
        else
        {
            input.played = hokuto::song::LoadGbaRom(std::move(bytes));
        }
        break;

    case InputKind::kDsArchive:
        input.played = hokuto::song::LoadDsArchive(std::move(bytes));
        break;

    case InputKind::kDsImage:
        if (hokuto::format::HoldsDseOnly(bytes))
        {
            input.played = hokuto::song::LoadDseSet(bytes);
        }
        else
        {
            const hokuto::format::File imported = hokuto::format::ImportDsImage(bytes, std::nullopt);
            input.played =
                hokuto::song::LoadDsArchive(hokuto::format::ExportArchive(imported), hokuto::song::DsBuildOf(bytes),
                                            hokuto::edit::DsGameOfFile(imported));
        }
        break;

    case InputKind::kHokuto:
        return InputOfHok(path, name);

    // An input of no other kind reads as a sound archive of the 3DS, the Wii U or the Switch, or fails with the reason.
    case InputKind::kLaterArchive:
    case InputKind::kMidi:
        input.played = hokuto::song::LoadArchive(std::move(bytes), name, file);
        input.streams = hokuto::song::ReadStreams(*input.played.Sar(), file);
        break;
    }

    return input;
}

// Whether `text` is a whole number in digits alone, at most nine of them. Such a number fits in 32 bits.
bool IsShortNumber(std::string_view text)
{
    return !text.empty() && text.size() <= 9 && std::ranges::all_of(text, [](char c) { return c >= '0' && c <= '9'; });
}

// The sound of `sounds` whose name (`name_of`) is `sound`, or else the sound whose number is `sound` when `number` is
// true.
template <typename Sounds, typename NameOf>
std::optional<std::size_t> FindSound(const Sounds& sounds, NameOf name_of, const std::string& sound, bool number)
{
    using Sound = typename Sounds::value_type;
    const auto named = std::ranges::find_if(sounds, [&](Sound each) { return name_of(each) == sound; });
    const auto numbered = number ? std::ranges::find(sounds, static_cast<Sound>(std::stoul(sound))) : sounds.end();
    const auto found = named != sounds.end() ? named : numbered;
    if (found == sounds.end())
    {
        return std::nullopt;
    }

    return static_cast<std::size_t>(*found);
}

// The index that `found` contains, or else throws std::runtime_error that `input` contains no `what` named or numbered
// `sound`.
std::size_t Found(std::optional<std::size_t> found, const std::string& input, std::string_view what,
                  const std::string& sound)
{
    if (!found)
    {
        throw std::runtime_error(input + " contains no " + std::string(what) + " " + sound + ".");
    }

    return *found;
}

// The index of the sound named or numbered `sound` in `file`, read from `input`. A GBA song's index is its number in
// the song table.
std::size_t SoundIndex(const hokuto::format::File& file, const std::string& input, const std::string& sound)
{
    // A GBA song is chosen by its name or its number in the song table.
    const bool number = IsShortNumber(sound);
    const hokuto::song::Played played = GameOfHok(file);
    std::size_t index = 0;
    if (const std::shared_ptr<const hokuto::song::GbaRom>& rom = played.Gba())
    {
        const auto name_of = [&](int song)
        {
            return hokuto::song::GbaSongName(*rom, song);
        };
        index = Found(FindSound(hokuto::song::GbaSongs(*rom), name_of, sound, number), input, "playable song", sound);
    }
    else if (const std::shared_ptr<const hokuto::song::DriverRom>& rom = played.Driver())
    {
        // A song of the other drivers of the GBA is chosen by its name or its number among the sounds
        // (song::DriverRom::Sounds).
        const auto name_of = [&](uint32_t each)
        {
            return rom->SoundName(each);
        };
        index = Found(FindSound(rom->Sounds(), name_of, sound, number), input, "playable song", sound);
    }
    else if (const std::shared_ptr<const hokuto::song::DsArchive>& archive = played.Ds())
    {
        // A DS sequence is chosen by its name or its number among the sounds (song::DsSounds).
        const auto name_of = [&](uint32_t each)
        {
            return hokuto::song::DsSoundName(*archive, each);
        };
        index = Found(FindSound(hokuto::song::DsSounds(*archive), name_of, sound, number), input, "sequence", sound);
    }
    else if (const std::shared_ptr<const hokuto::song::DseSet>& set = played.Dse())
    {
        // A sound of DSE is chosen by its name or its number among the sounds (song::DseSounds).
        const auto name_of = [&](uint32_t each)
        {
            return hokuto::song::DseSoundName(*set, each);
        };
        index = Found(FindSound(hokuto::song::DseSounds(*set), name_of, sound, number), input, "sound", sound);
    }
    else
    {
        const std::vector<hokuto::format::NativeSound>& sounds = file.native->tables->sounds;
        const auto named = std::ranges::find(sounds, sound, &hokuto::format::NativeSound::name);
        index = named != sounds.end() ? static_cast<std::size_t>(named - sounds.begin())
                : number              ? std::stoul(sound)
                                      : sounds.size();
        if (index >= sounds.size())
        {
            throw std::runtime_error(input + " contains no sound " + sound + ".");
        }
    }

    return index;
}

// Writes the file of the console named `console` that holds sequence sound `sound` of the .hok file at `input` to
// `output`, in the binary form, and prints the warnings. `sound` is the sound's name, or else its number in the sound
// table. For a GBA file, it is the song's name or its number in the song table. `console` is a name of the text form,
// such as 3ds or wii, or empty for the console whose files are nearest (song::OtherConsole). `mono` sets how the stereo
// waves go to the 3DS. The command converts the file as File > Export > Another Console… does: as the program opens it,
// with each sequence in the mode that the program finds for it, and laid out as a save lays it out. The new file keeps
// the mode of the sound's sequence.
void WriteOtherConsole(const std::string& input, const std::string& sound, const std::string& output,
                       const std::string& console, hokuto::edit::MonoWaves mono)
{
    hokuto::format::File file = ReadHok(input);
    // The Wii's archive and a later console's have tables. A ROM or a set of sounds doesn't.
    const bool tables =
        file.native && (hokuto::format::FamilyOf(*file.native) == hokuto::format::Family::kNintendoWare ||
                        hokuto::format::FamilyOf(*file.native) == hokuto::format::Family::kNw4r);
    if (!file.native || (tables && !file.native->tables))
    {
        throw std::runtime_error(input + " contains no sound archive with tables that Hokuto can read.");
    }

    if (tables)
    {
        file = hokuto::edit::FoldTimelines(hokuto::edit::OpenTimelines(std::move(file)));
    }

    // A song of the Wii, the GBA, the DS or DSE goes to the Wii U unless the console is named.
    const ntdlib::sar::Flavor other = file.native->engine == hokuto::format::Engine::kWii || !tables
                                          ? ntdlib::sar::Flavor::kCafe
                                          : hokuto::song::OtherConsole(FlavorOf(*file.native));
    const bool to_wii = console == hokuto::format::kWiiArchiveName;
    const bool to_ds = console == hokuto::format::kDsArchiveName;
    const std::optional<ntdlib::sar::Flavor> to =
        console.empty() ? std::optional(other) : hokuto::format::FlavorNamed(console);
    if (!to && !to_wii && !to_ds)
    {
        throw std::runtime_error("Unknown console: " + console + ". Use 3ds, ds, wii, wii_u or switch.");
    }

    const std::size_t index = SoundIndex(file, input, sound);

    const hokuto::edit::OtherConsoleSong song =
        to_ds    ? hokuto::edit::ForDs(file, static_cast<uint32_t>(index))
        : to_wii ? hokuto::edit::ForWii(file, static_cast<uint32_t>(index))
                 : hokuto::edit::ForOtherConsole(file, static_cast<uint32_t>(index), *to, mono);
    WriteOutput(output, hokuto::format::Write(song.file));
    for (const std::string& warning : song.warnings)
    {
        std::printf("%s\n", warning.c_str());
    }
}

// Runs hok other with `arguments`: the input, the output and the sound, then the console and the choice of mono or
// stereo waves. Returns 2 for arguments that don't fit, or else 0.
int RunOther(const std::vector<std::string>& arguments)
{
    // The console, then the choice of mono or stereo waves, each optional.
    std::string console;
    hokuto::edit::MonoWaves mono = hokuto::edit::MonoWaves::kAuto;
    for (std::size_t at = 5; at < arguments.size(); ++at)
    {
        if (arguments[at] == "mono" || arguments[at] == "stereo")
        {
            mono = arguments[at] == "mono" ? hokuto::edit::MonoWaves::kOn : hokuto::edit::MonoWaves::kOff;
        }
        else if (at == 5)
        {
            console = arguments[at];
        }
        else
        {
            std::fputs(kUsage, stderr);
            return 2;
        }
    }

    WriteOtherConsole(arguments[2], arguments[3], arguments[4], console, mono);
    return 0;
}

// Takes the count of --threads out of `arguments` into `threads`, at most the processor's `threads`. Returns false for
// a count that isn't a whole number of 1 or more.
bool TakeThreads(std::vector<std::string>& arguments, std::size_t& threads)
{
    // A whole number of 1 or more, in digits alone. Each thread holds a model of the console. The count therefore
    // stops at the processor's threads.
    const std::string& count = arguments[3];
    const std::size_t asked = IsShortNumber(count) ? std::stoul(count) : 0;
    if (asked == 0)
    {
        const std::string given = count.empty() ? "an empty argument" : count;
        std::fprintf(stderr, "error: --threads requires a whole number of 1 or more, not %s.\n", given.c_str());
        return false;
    }
    if (asked > threads)
    {
        std::fprintf(stderr, "The processor has %zu threads. Measuring %zu sounds at a time.\n", threads, threads);
    }

    threads = std::min(asked, threads);
    arguments.erase(arguments.begin() + 2, arguments.begin() + 4);

    return true;
}

// Imports the archive, ROM or image `input` into the .hok file `output`. `dol` names a Wii game's main.dol, and `sdat`
// the archive of a DS image.
void RunImport(const std::string& input, const std::string& output, const std::string& dol, const std::string& sdat)
{
    // The file keeps the group files and the streams beside the archive, and a Wii archive's game's build of NW4R
    // SND. The program finds them the same way.
    const std::vector<uint8_t> bytes = ReadInput(input);
    const std::filesystem::path path = xsflib::PathFromUtf8(input);

    using hokuto::format::InputKind;
    const InputKind kind =
        hokuto::format::FirstInputKind(bytes, hokuto::format::kInputOrder).value_or(InputKind::kLaterArchive);
    const bool wii = kind == InputKind::kWiiArchive;
    const bool ds_image = kind == InputKind::kDsImage;
    if (!dol.empty() && !wii)
    {
        throw std::runtime_error("--dol works only with Wii sound archives. " + input + " isn't one.");
    }
    if (!sdat.empty() && !ds_image)
    {
        throw std::runtime_error("--sdat works only with DS cartridge images. " + input + " isn't one.");
    }

    hokuto::format::File file;
    try
    {
        switch (kind)
        {
        case InputKind::kDsImage:
            // A game without a sound archive can hold DSE's files. --sdat names an archive.
            file = sdat.empty() && hokuto::format::HoldsDseOnly(bytes)
                       ? hokuto::format::ImportDseImage(bytes)
                       : hokuto::format::ImportDsImage(bytes, DsArchiveNamed(bytes, sdat));
            break;

        case InputKind::kDsArchive:
            file = hokuto::format::ImportDsArchive(bytes);
            break;

        case InputKind::kGbaRom:
            {
                const hokuto::format::Driver* driver = hokuto::format::DriverOfRom(bytes);
                file = driver ? driver->Import(bytes) : hokuto::format::ImportGbaRom(bytes);
            }
            break;

        case InputKind::kWiiArchive:
            {
                const auto archive =
                    hokuto::song::LoadWiiArchive(bytes, xsflib::Utf8(path.filename()), ntdlib::rvl::snd::kRsarOptions);
                file = hokuto::format::ImportWiiArchive(bytes, GameBuild(path, dol),
                                                        hokuto::song::ReadStreams(*archive, path));
            }
            break;

        // An input of no other kind imports as a sound archive of the 3DS, the Wii U or the Switch, or fails with the
        // reason. That includes a .hok file.
        case InputKind::kLaterArchive:
        case InputKind::kHokuto:
        case InputKind::kMidi:
            {
                const auto archive = hokuto::song::LoadArchive(bytes, xsflib::Utf8(path.filename()), path);
                file = hokuto::format::ImportArchive(bytes, archive->ReadOptions().flavor, archive->GroupFiles(),
                                                     hokuto::song::ReadStreams(*archive, path),
                                                     archive->GroupFileGroups());
            }
            break;
        }
    }
    catch (const xsflib::FormatError& error)
    {
        throw std::runtime_error("Can't import " + input + ". " + hokuto::format::AsSentence(error.what()));
    }

    WriteOutput(output, hokuto::format::Write(file));
    PrintDsArchivePlace(bytes, file);
}

// Exports the .hok file `input` to the archive, ROM or image `output`. `filler` lets a GBA ROM's songs move into runs
// of filler bytes, and `dsi_key` names the file of a DSi's key.
// `game` names a folder to look in for the SARC archives and the userManagementFiles folder of changed group files
// (WriteGroupFiles).
void RunExport(const std::string& input, const std::string& output, bool filler, const std::string& dsi_key,
               const std::string& game)
{
    // A GBA ROM without room for the songs that outgrow their places uses the file's choice, or --filler.
    hokuto::format::File file = ReadHok(input);
    if (filler && (!file.native || (file.native->engine != hokuto::format::Engine::kGba && !file.native->driver)))
    {
        throw std::runtime_error("--filler works only with .hok files of GBA ROMs. " + input + " isn't one.");
    }
    if (filler && file.native->driver && !hokuto::format::DriverOf(file.native->engine)->Facts().filler_runs)
    {
        throw std::runtime_error("--filler doesn't apply to a ROM with " +
                                 std::string(hokuto::format::DriverOf(file.native->engine)->Facts().title) + ".");
    }
    if (filler)
    {
        file.native.Edit([](hokuto::format::NativeArchive& native) { native.gba_filler = true; });
    }

    if (!dsi_key.empty() && (!file.native || !file.native->cartridge))
    {
        throw std::runtime_error("--dsi-key works only with .hok files of DS images. " + input + " isn't one.");
    }

    if (file.native && file.native->cartridge)
    {
        // The digest of a DSi image contains hashes that only the DSi key can compute. The header's RSA signature
        // covers them, and only Nintendo can sign them again.
        std::optional<ntdlib::ntr::DsiKeys> keys;
        if (!dsi_key.empty())
        {
            std::vector<uint8_t> bytes;
            if (!xsflib::ReadFile(xsflib::PathFromUtf8(dsi_key), bytes))
            {
                throw std::runtime_error("Can't read " + dsi_key + ".");
            }

            keys = hokuto::format::DsiKeysOf(bytes);
        }

        const hokuto::format::DsImageExport exported = hokuto::format::ExportDsImage(file, keys);
        WriteOutput(output, exported.image);
        for (const ntdlib::ntr::StaleHash& stale : exported.stale)
        {
            std::fprintf(stderr, "warning: out of date after the export: %s (%u at 0x%X).\n", stale.what.c_str(),
                         stale.count, stale.offset);
        }

        if (hokuto::format::IsDsiImage(exported.image) &&
            exported.image != hokuto::format::CartridgeImage(*file.native->cartridge))
        {
            std::fprintf(stderr,
                         "warning: the RSA signature of the image still covers the old hashes. A DSi in DSi "
                         "mode may refuse to run the image.\n");
        }

        return;
    }

    if (hokuto::format::LacksRoom(file))
    {
        const std::size_t moved = hokuto::format::MovedSongs(file).size();
        const std::string songs = Counted(moved, "song no longer fits in its original place",
                                          "songs no longer fit in their original places") +
                                  ". The ROM has no free space after its data.";
        const std::string them = moved == 1 ? "it" : "them";
        if (!file.native->gba_filler)
        {
            throw std::runtime_error("Can't export the ROM. " + songs + " With --filler, hok moves " + them +
                                     " into runs of filler bytes inside the ROM. Hokuto can't check that no other "
                                     "game code reads those bytes.");
        }

        std::fprintf(stderr,
                     "%s Moved %s into runs of filler bytes inside the ROM. Hokuto can't check that no other game "
                     "code reads those bytes. Play the ROM in an emulator to check that the game still works.\n",
                     songs.c_str(), them.c_str());
    }

    const std::vector<uint8_t> archive = hokuto::format::ExportArchive(file);
    WriteOutput(output, archive);
    WriteStreams(file, archive, output);
    WriteGroupFiles(file, archive, input, output, game);
}

// Writes the .hok file `input` in the model form to `output`.
void RunModel(const std::string& input, const std::string& output)
{
    // The model's refusal is a clause, such as "the archive is cut short, and a file in the model form requires the
    // whole archive".
    hokuto::format::File file;
    try
    {
        file = hokuto::format::InModelForm(ReadHok(input));
    }
    catch (const xsflib::FormatError& error)
    {
        throw std::runtime_error("Can't convert " + input + " to the model form. " +
                                 hokuto::format::AsSentence(error.what()));
    }

    WriteOutput(output, hokuto::format::Write(file));
}

// Measures the lengths of the sequence sounds of the .hok file `input` on `threads` threads, and writes the file with
// them to `output`.
void RunLengths(const std::string& input, const std::string& output, std::size_t threads)
{
    // The sounds play from the archive that the file builds, with the group files that it builds beside it. A sound
    // of the Wii plays on the model of the file's build of NW4R SND.
    const hokuto::format::File file = ReadHok(input);
    if (!file.native)
    {
        throw std::runtime_error(input + " contains no sound archive.");
    }

    if (file.native->engine == hokuto::format::Engine::kGba || file.native->engine == hokuto::format::Engine::kDs ||
        file.native->engine == hokuto::format::Engine::kDse)
    {
        const std::string kind = file.native->engine == hokuto::format::Engine::kGba  ? "GBA ROMs with MusicPlayer2000"
                                 : file.native->engine == hokuto::format::Engine::kDs ? "DS sound archives"
                                                                                      : "DSE files";
        throw std::runtime_error("hok lengths doesn't support " + kind + ".");
    }

    const std::string name = xsflib::Utf8(xsflib::PathFromUtf8(input).filename());
    std::optional<hokuto::format::File> measured;
    if (file.native->driver)
    {
        const auto rom = hokuto::song::LoadDriverRom(file.native->engine, hokuto::format::ExportArchive(file));
        measured = hokuto::edit::WithLengths(file, *rom, threads);
    }
    else if (file.native->engine == hokuto::format::Engine::kWii)
    {
        const auto archive =
            hokuto::song::LoadWiiArchive(hokuto::format::ExportArchive(file), name, ntdlib::rvl::snd::kRsarOptions);
        measured = hokuto::edit::WithLengths(file, *archive, hokuto::format::BuildOfWiiArchive(*file.native), threads);
    }
    else
    {
        const auto archive = hokuto::song::LoadArchive(hokuto::format::ExportArchive(file), name,
                                                       hokuto::format::ExportGroupFiles(file));
        measured = hokuto::edit::WithLengths(file, *archive, threads);
    }

    WriteOutput(output, hokuto::format::Write(*measured));
    std::printf("Measured the lengths of %s.\n",
                Counted(measured->lengths.size(), "sequence sound", "sequence sounds").c_str());
}

// Writes the items, the waves or the banks of `input` into the folder `output`, for `command` items, wav, sf2 or sfz.
// Returns 1 when an item failed, or else 0.
int RunItems(const std::string& command, const std::string& input, const std::string& output)
{
    // The folder is made if it's missing. A failure leaves out the item, and the others go on.
    const ArchiveInput archive = ReadArchive(input);
    if (archive.played.Gba() && command == "sfz")
    {
        throw std::runtime_error("hok sfz doesn't support GBA ROMs. Use hok sf2 to convert their voice groups.");
    }
    if (archive.played.Driver() && !archive.played.Driver()->WritesSfz() && command == "sfz")
    {
        throw std::runtime_error("hok sfz doesn't support ROMs with " +
                                 std::string(archive.played.Driver()->Rules().Title()) +
                                 ". Use hok sf2 to convert their instruments.");
    }

    const std::filesystem::path folder = xsflib::PathFromUtf8(output);
    std::error_code error;
    std::filesystem::create_directories(folder, error);
    if (error)
    {
        throw std::runtime_error("Can't create " + output + ": " + error.message() + ".");
    }

    std::size_t failed = 0;
    if (archive.played.Driver())
    {
        failed = command == "items" ? WriteDriverItems(archive, folder)
                 : command == "wav" ? WriteDriverWavs(archive, folder)
                                    : WriteDriverSoundFonts(archive, folder, command == "sfz");
    }
    else if (archive.played.Gba())
    {
        failed = command == "items" ? WriteGbaItems(archive, folder)
                 : command == "wav" ? WriteGbaWavs(archive, folder)
                                    : WriteVoiceGroups(archive, folder);
    }
    else if (command == "items")
    {
        failed = archive.played.Dse()   ? WriteDseItems(archive, folder)
                 : archive.played.Ds()  ? WriteDsItems(archive, folder)
                 : archive.played.Wii() ? WriteWiiItems(archive, folder)
                                        : WriteItems(archive, folder);
    }
    else if (command == "wav")
    {
        failed = archive.played.Dse()   ? WriteDseWavs(archive, folder)
                 : archive.played.Ds()  ? WriteDsWavs(archive, folder)
                 : archive.played.Wii() ? WriteWiiWavs(archive, folder)
                                        : WriteWavs(archive, folder);
    }
    else
    {
        failed = WriteBanks(archive, folder, command == "sfz");
    }

    return failed > 0 ? 1 : 0;
}

// The options that come before the input and the output of a command.
struct Options
{
    bool filler = false;
    std::string dsi_key;
    std::string game;
    std::string dol;
    std::string sdat;
    std::size_t threads = hokuto::edit::MostLengthThreads();
};

// Takes the options out of `arguments`. Returns false for an option with a wrong value. TakeThreads says why.
bool TakeOptions(std::vector<std::string>& arguments, Options& options)
{
    while (arguments.size() >= 5 && arguments[1] == "export" &&
           (arguments[2] == "--filler" ||
            ((arguments[2] == "--dsi-key" || arguments[2] == "--game") && arguments.size() >= 6)))
    {
        if (arguments[2] == "--filler")
        {
            options.filler = true;
            arguments.erase(arguments.begin() + 2);
        }
        else
        {
            (arguments[2] == "--dsi-key" ? options.dsi_key : options.game) = arguments[3];
            arguments.erase(arguments.begin() + 2, arguments.begin() + 4);
        }
    }

    if (arguments.size() == 6 && arguments[1] == "import" && (arguments[2] == "--dol" || arguments[2] == "--sdat"))
    {
        (arguments[2] == "--dol" ? options.dol : options.sdat) = arguments[3];
        arguments.erase(arguments.begin() + 2, arguments.begin() + 4);
    }

    if (arguments.size() == 6 && arguments[1] == "lengths" && arguments[2] == "--threads")
    {
        if (!TakeThreads(arguments, options.threads))
        {
            return false;
        }
    }

    return true;
}

// Runs `command` on `input`, writing to `output`. Returns the exit code.
int RunCommand(const std::string& command, const std::string& input, const std::string& output, const Options& options)
{
    if (command == "import")
    {
        RunImport(input, output, options.dol, options.sdat);
    }
    else if (command == "export")
    {
        RunExport(input, output, options.filler, options.dsi_key, options.game);
    }
    else if (command == "text")
    {
        const std::string text = hokuto::format::WriteText(ReadHok(input));
        WriteOutput(output, std::span(reinterpret_cast<const uint8_t*>(text.data()), text.size()));
    }
    else if (command == "binary")
    {
        WriteOutput(output, hokuto::format::Write(ReadHok(input)));
    }
    else if (command == "model")
    {
        RunModel(input, output);
    }
    else if (command == "lengths")
    {
        RunLengths(input, output, options.threads);
    }
    else if (command == "items" || command == "wav" || command == "sf2" || command == "sfz")
    {
        return RunItems(command, input, output);
    }
    else
    {
        std::fputs(kUsage, stderr);
        return 2;
    }

    return 0;
}

int Run(std::vector<std::string> arguments)
{
    if (arguments.size() == 2 && arguments[1] == "--version")
    {
        std::printf("hok %s\n", HOKUTO_VERSION);
        return 0;
    }

    if (arguments.size() == 2 && (arguments[1] == "--help" || arguments[1] == "-h" || arguments[1] == "help"))
    {
        std::fputs(kUsage, stdout);
        return 0;
    }

    if (arguments.size() >= 5 && arguments.size() <= 7 && arguments[1] == "other")
    {
        return RunOther(arguments);
    }

    Options options;
    if (!TakeOptions(arguments, options))
    {
        return 2;
    }

    if (arguments.size() != 4)
    {
        // The message for a command that hok doesn't have comes first, before the usage.
        constexpr std::array kCommands = {"import", "export", "text", "binary", "model", "lengths",
                                          "items",  "wav",    "sf2",  "sfz",    "other"};
        if (arguments.size() >= 2 && std::ranges::find(kCommands, arguments[1]) == kCommands.end())
        {
            std::fprintf(stderr, "Unknown command: %s.\n", arguments[1].c_str());
        }

        std::fputs(kUsage, stderr);
        return 2;
    }

    return RunCommand(arguments[1], arguments[2], arguments[3], options);
}

} // namespace

int main(int argc, char** argv)
{
    const std::vector<std::string> arguments = xsflib::console::Arguments(argc, argv);
    const auto run = [&]
    {
        return Run(arguments);
    };
    return xsflib::cli::RunTool(run);
}
