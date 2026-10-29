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
#include <cstring>
#include <exception>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "ntdlib/rsar/formats.h"
#include "ntdlib/rvl/snd/rsar_options.h"
#include "ntdlib/sar/formats.h"
#include "xsflib/ascii.h"
#include "xsflib/binary.h"
#include "xsflib/cli/tool.h"
#include "xsflib/console.h"
#include "xsflib/files.h"

#include "edit/lengths.h"
#include "edit/other_console.h"
#include "format/archive.h"
#include "format/codes.h"
#include "format/file.h"
#include "format/kinds.h"
#include "format/model.h"
#include "format/text.h"
#include "song/aac.h"
#include "song/archive.h"
#include "song/items.h"
#include "song/midi_export.h"
#include "song/streams.h"

namespace
{

constexpr const char* kUsage =
    "Usage:\n"
    "  hok import [--dol <main.dol>] <archive> <file.hok>\n"
    "                                    Make a .hok file from a sound archive of the Switch (.bfsar, .fsst or\n"
    "                                    .fspj), the Wii U (.bfsar), the 3DS (.bcsar) or the Wii (.brsar), and\n"
    "                                    the group files and streams beside it. A Wii archive takes the build of\n"
    "                                    NW4R SND from the game's main.dol: the one that --dol gives, or else\n"
    "                                    sys/main.dol of the extracted disc that holds the archive. --dol applies\n"
    "                                    to a Wii archive only.\n"
    "  hok export <file.hok> <archive>   Build the sound archive that a .hok file came from, with the streams\n"
    "                                    that the game lacks and the group files that changed beside it.\n"
    "  hok text <file.hok> <output>      Write a .hok file in the text form.\n"
    "  hok binary <file.hok> <output>    Write a .hok file in the binary form.\n"
    "  hok model <file.hok> <output>     Write a .hok file in the model form, without the game's layout. An export\n"
    "                                    lays the archive out as NintendoWare's tools do.\n"
    "  hok lengths [--threads <n>] <file.hok> <output>\n"
    "                                    Measure how long a player plays each sequence sound of a .hok file before\n"
    "                                    its fade, and write the file with the lengths in the binary form. The\n"
    "                                    sounds play n at a time, and one on each of the processor's threads at\n"
    "                                    most. Without --threads, they play one on each thread.\n"
    "  hok items <input> <folder>        Write each item of a sound archive or a .hok file into a folder: the\n"
    "                                    sequence files, wave sound files, banks, wave archives, waves and streams.\n"
    "  hok wav <input> <folder>          Write each wave, wave sound and stream of a sound archive or a .hok file\n"
    "                                    into a folder as a WAV file.\n"
    "  hok sf2 <input> <folder>          Write the instruments of each bank of a sound archive or a .hok file\n"
    "                                    into a folder as a SoundFont 2 file.\n"
    "  hok sfz <input> <folder>          Write the instruments of each bank into a folder as SFZ files: a folder\n"
    "                                    for each bank, with the samples in the folder samples.\n"
    "  hok other <file.hok> <sound> <output> [console] [mono | stereo]\n"
    "                                    Write a .hok file of another console that holds a sequence sound of a\n"
    "                                    .hok file, given by its name or its number. The number counts from 0\n"
    "                                    over the whole sound table, the sounds of every kind included. The\n"
    "                                    console is 3ds, wii, wii_u or switch. Without it, a song of the Switch,\n"
    "                                    the 3DS or the Wii goes to the Wii U, and one of the Wii U to the Switch.\n"
    "                                    With mono, a song for the 3DS takes its stereo waves as mono. With\n"
    "                                    stereo, it keeps them. Without either, they go as mono when the song\n"
    "                                    holds more channels at once than the 3DS plays. The warnings say what\n"
    "                                    changes on the way.\n"
    "  hok --version                     Print hok's version.\n"
    "  hok --help                        Print this text. -h and help do the same.\n"
    "Wherever hok reads a .hok file, the file can be in either form.\n";

std::vector<uint8_t> ReadInput(const std::string& path)
{
    std::vector<uint8_t> bytes;
    if (!xsflib::ReadFile(xsflib::PathFromUtf8(path), bytes))
    {
        throw std::runtime_error("can't read " + path);
    }
    if (bytes.empty())
    {
        throw std::runtime_error(path + ": It's empty.");
    }

    return bytes;
}

// A count and its noun, such as "1 bank" or "40 banks".
std::string Counted(std::size_t count, std::string_view one, std::string_view many)
{
    return std::to_string(count) + " " + std::string(count == 1 ? one : many);
}

void WriteOutput(const std::string& path, std::span<const uint8_t> bytes)
{
    if (const std::optional<std::string> error = xsflib::WriteFile(xsflib::PathFromUtf8(path), bytes))
    {
        throw std::runtime_error(*error);
    }
}

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
            throw std::runtime_error(dol + " isn't a main.dol with AX's microcode.");
        }

        return build;
    }

    std::vector<uint8_t> code;
    const std::optional<std::filesystem::path> found = hokuto::song::FindMainDol(archive);
    return found && xsflib::ReadFile(*found, code) ? hokuto::song::ReadWiiBuild(code) : std::nullopt;
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
        file.native->wii
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

    std::printf("Wrote %zu %s beside the archive.\n", files.size(), files.size() == 1 ? "stream" : "streams");
}

// Writes the group files of `file` whose bytes differ from the game's beside `archive`, the archive at `path`. A SARC
// archive beside the archive, or beside the .hok file at `input`, takes the group file of a group whose file it holds
// (format::GroupFileOutputs). A group file whose game's file lies in the userManagementFiles folder there goes into
// that folder (song::PlaceGroupFiles). The game holds the other group files already. A group file whose group Hokuto
// couldn't find stays out, with a message.
void WriteGroupFiles(const hokuto::format::File& file, std::span<const uint8_t> archive, const std::string& input,
                     const std::string& path)
{
    const std::filesystem::path folder = xsflib::PathFromUtf8(path).parent_path();
    std::vector<hokuto::format::ChangedGroupFile> changed = hokuto::format::ChangedGroupFiles(file);
    for (const hokuto::format::ChangedGroupFile& group_file : changed)
    {
        if (!group_file.group)
        {
            std::fprintf(stderr,
                         "The edits change group file %zu, and Hokuto can't tell which group it holds. It stays out.\n",
                         group_file.index + 1);
        }
    }
    if (std::ranges::none_of(changed,
                             [](const hokuto::format::ChangedGroupFile& each) { return each.group.has_value(); }))
    {
        return;
    }

    const std::array<std::filesystem::path, 2> folders = {folder, xsflib::PathFromUtf8(input).parent_path()};
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
        std::printf("Wrote %zu %s beside the archive.\n", written, written == 1 ? "group file" : "group files");
    }
    if (in_folder > 0)
    {
        std::printf("Wrote %zu %s in the %s folder beside the archive.\n", in_folder,
                    in_folder == 1 ? "group file" : "group files", hokuto::song::kUserManagementFolder);
    }
    if (sarcs > 0)
    {
        std::printf("Wrote %zu SARC %s with new group files beside the archive.\n", sarcs,
                    sarcs == 1 ? "archive" : "archives");
    }
}

// A sound archive with its streams, by their file IDs. An archive of the Wii comes as `wii` in place of `archive`, with
// the game's build of NW4R SND when the game's main.dol gives it.
struct ArchiveInput
{
    std::shared_ptr<const ntdlib::sar::SoundArchive> archive;
    std::map<uint32_t, std::vector<uint8_t>> streams;
    std::shared_ptr<const ntdlib::rsar::SoundArchive> wii;
    std::optional<ntdlib::SndBuild> build;
};

// The sound archive in the file at `path`: a sound archive with the group files and the streams beside it, or the
// archive that a .hok file in either form builds, with the group files and the streams that the file keeps. An archive
// of the Wii takes the game's build from the main.dol of the extracted disc that holds it. An import takes the build
// the same way. A .hok file of the Wii keeps the build.
ArchiveInput ReadArchive(const std::string& path)
{
    std::vector<uint8_t> bytes = ReadInput(path);
    const std::filesystem::path file = xsflib::PathFromUtf8(path);
    const std::string name = xsflib::Utf8(file.filename());
    ArchiveInput input;
    if (bytes.size() >= 4 && std::memcmp(bytes.data(), "RSAR", 4) == 0)
    {
        input.build = GameBuild(file, "");
        input.wii = hokuto::song::LoadWiiArchive(std::move(bytes), name, ntdlib::rvl::snd::kRsarOptions);
        input.streams = hokuto::song::ReadStreams(*input.wii, file);
        return input;
    }

    if (!hokuto::format::IsBinary(bytes) && !hokuto::format::IsText(bytes))
    {
        input.archive = hokuto::song::LoadArchive(std::move(bytes), name, file);
        input.streams = hokuto::song::ReadStreams(*input.archive, file);
        return input;
    }

    const hokuto::format::File hok = ReadHok(path);
    if (!hok.native)
    {
        throw std::runtime_error(path + " holds no sound archive.");
    }

    for (const auto& [id, stream] : hok.native->streams)
    {
        input.streams[id] = *stream.bytes;
    }

    if (hok.native->wii)
    {
        input.wii =
            hokuto::song::LoadWiiArchive(hokuto::format::ExportArchive(hok), name, ntdlib::rvl::snd::kRsarOptions);
        input.build = hok.native->wii_build;
        return input;
    }

    input.archive =
        hokuto::song::LoadArchive(hokuto::format::ExportArchive(hok), name, hokuto::format::ExportGroupFiles(hok));

    return input;
}

// The stream sounds of `archive` whose streams `input` holds, once for each stream file: the first sound that plays the
// file.
template <class Archive>
std::vector<uint32_t> StreamSounds(const ArchiveInput& input, const Archive& archive)
{
    std::vector<uint32_t> sounds;
    std::set<uint32_t> files;
    for (uint32_t sound = 0; sound < archive.Sounds().size(); sound++)
    {
        const std::optional<uint32_t> file = hokuto::song::StreamFileOf(archive, sound);
        if (file && input.streams.contains(*file) && files.insert(*file).second)
        {
            sounds.push_back(sound);
        }
    }

    return sounds;
}

// Writes `bytes` into `folder` as `name`, and counts it in `written`. Two items whose names differ only in case would
// share a file on macOS and Windows. The second one then takes `index` after its name: its index in the archive's
// table, or its wave archive's index for a wave.
void WriteItem(const std::filesystem::path& folder, std::string name, std::span<const uint8_t> bytes,
               std::set<std::string>& written, uint32_t index)
{
    if (written.contains(xsflib::AsciiLower(name)))
    {
        const std::size_t dot = name.rfind('.');
        name.insert(dot, "_" + std::to_string(index));
    }
    written.insert(xsflib::AsciiLower(name));
    WriteOutput(xsflib::Utf8(folder / xsflib::PathFromUtf8(name)), bytes);
}

// Writes the items of `input` into `folder`: a sequence file and a wave sound file once each, under the name of the
// first sound that plays it, then the banks, the wave archives and each wave, and last each stream under the name of
// its file. An item that can't be read is reported and left out. Returns the number of items left out.
std::size_t WriteItems(const ArchiveInput& input, const std::filesystem::path& folder)
{
    namespace song = hokuto::song;
    const ntdlib::sar::SoundArchive& archive = *input.archive;
    std::set<std::string> written;
    std::set<uint32_t> files;
    std::map<song::ItemKind, std::size_t> counts;
    std::size_t failed = 0;
    const auto write = [&](const song::Item& item, std::size_t waves)
    {
        try
        {
            WriteItem(folder, song::ItemFileName(archive, item, waves), song::ItemFile(archive, item), written,
                      item.index);
            counts[item.kind]++;
        }
        catch (const std::exception& error)
        {
            std::fprintf(stderr, "%s\n", error.what());
            failed++;
        }
    };

    for (std::size_t i = 0; i < archive.Sounds().size(); i++)
    {
        const ntdlib::sar::SoundInfo& sound = archive.Sounds()[i];
        const bool sequence = sound.type == ntdlib::sar::SoundType::kSequence;
        if ((sequence || sound.type == ntdlib::sar::SoundType::kWaveSound) && files.insert(sound.file_id).second)
        {
            write({sequence ? song::ItemKind::kSequence : song::ItemKind::kWaveSound, static_cast<uint32_t>(i), 0}, 0);
        }
    }

    for (std::size_t i = 0; i < archive.Banks().size(); i++)
    {
        write({song::ItemKind::kBank, static_cast<uint32_t>(i), 0}, 0);
    }

    for (std::size_t i = 0; i < archive.WaveArchives().size(); i++)
    {
        const auto index = static_cast<uint32_t>(i);
        write({song::ItemKind::kWaveArchive, index, 0}, 0);
        std::size_t waves = 0;
        try
        {
            waves = song::WaveCountOf(archive, i);
        }
        catch (const std::exception& error)
        {
            std::fprintf(stderr, "%s\n", error.what());
            failed++;
        }
        for (std::size_t wave = 0; wave < waves; wave++)
        {
            write({song::ItemKind::kWave, index, static_cast<uint32_t>(wave)}, waves);
        }
    }

    for (const uint32_t sound : StreamSounds(input, archive))
    {
        const uint32_t file = archive.Sounds()[sound].file_id;
        WriteItem(folder, song::StreamFileName(archive, sound), input.streams.at(file), written, sound);
        counts[song::ItemKind::kStream]++;
    }

    std::printf("Wrote %s, %s, %s, %s, %s and %s.\n",
                Counted(counts[song::ItemKind::kSequence], "sequence file", "sequence files").c_str(),
                Counted(counts[song::ItemKind::kWaveSound], "wave sound file", "wave sound files").c_str(),
                Counted(counts[song::ItemKind::kBank], "bank", "banks").c_str(),
                Counted(counts[song::ItemKind::kWaveArchive], "wave archive", "wave archives").c_str(),
                Counted(counts[song::ItemKind::kWave], "wave", "waves").c_str(),
                Counted(counts[song::ItemKind::kStream], "stream", "streams").c_str());

    return failed;
}

// Writes each wave of `input` into `folder` as a WAV file, then each wave sound under the sound's name, and each stream
// under the name of its file. A wave, a wave sound or a stream that can't be decoded is reported and left out. Returns
// the number left out.
std::size_t WriteWavs(const ArchiveInput& input, const std::filesystem::path& folder)
{
    namespace song = hokuto::song;
    const ntdlib::sar::SoundArchive& archive = *input.archive;
    std::set<std::string> written;
    std::size_t count = 0;
    std::size_t wave_sounds = 0;
    std::size_t streams = 0;
    std::size_t failed = 0;
    for (std::size_t i = 0; i < archive.WaveArchives().size(); i++)
    {
        const auto index = static_cast<uint32_t>(i);
        try
        {
            const std::size_t waves = song::WaveCountOf(archive, i);
            for (std::size_t wave = 0; wave < waves; wave++)
            {
                try
                {
                    const auto number = static_cast<uint32_t>(wave);
                    WriteItem(folder, song::WavFileName(archive, index, number, waves),
                              song::WaveAsWav(archive, index, number), written, index);
                    count++;
                }
                catch (const std::exception& error)
                {
                    std::fprintf(stderr, "%s\n", error.what());
                    failed++;
                }
            }
        }
        catch (const std::exception& error)
        {
            std::fprintf(stderr, "%s\n", error.what());
            failed++;
        }
    }

    for (std::size_t i = 0; i < archive.Sounds().size(); i++)
    {
        if (archive.Sounds()[i].type != ntdlib::sar::SoundType::kWaveSound)
        {
            continue;
        }

        const auto sound = static_cast<uint32_t>(i);
        try
        {
            WriteItem(folder, song::WaveSoundWavFileName(archive, sound), song::WaveSoundAsWav(archive, sound), written,
                      sound);
            wave_sounds++;
        }
        catch (const std::exception& error)
        {
            std::fprintf(stderr, "%s\n", error.what());
            failed++;
        }
    }

    for (const uint32_t sound : StreamSounds(input, archive))
    {
        try
        {
            // An AAC stream takes the loop that the sound's entry gives.
            std::string name = song::StreamFileName(archive, sound);
            name = name.substr(0, name.rfind('.')) + ".wav";
            const std::vector<uint8_t>& bytes = input.streams.at(archive.Sounds()[sound].file_id);
            const song::AacStream aac =
                song::IsAdts(bytes) ? song::AacStreamOf(archive, sound).value_or(song::AacStream()) : song::AacStream();
            WriteItem(folder, name, song::StreamAsWav(bytes, aac), written, sound);
            streams++;
        }
        catch (const std::exception& error)
        {
            std::fprintf(stderr, "%s\n", error.what());
            failed++;
        }
    }

    std::printf("Wrote %s of waves, %zu of wave sounds and %zu of streams.\n",
                Counted(count, "WAV file", "WAV files").c_str(), wave_sounds, streams);

    return failed;
}

// Writes the items of the Wii's archive of `input` into `folder` as WriteItems writes those of the other consoles: a
// sequence file and a wave sound file once each, then the banks, then the wave data of each bank and wave sound file as
// an RWAR with each of its waves, and last each stream under the name of its file. Wave data that a WAVE block
// describes goes out as an RWAR of the same waves (song::ItemBytes). A wave sound file takes the browser's name,
// WSD_FILE_ and its file ID. Its RWAR takes the same name. File > Import > Wave Sound File… then finds the RWAR beside
// the file. Returns the number of items left out.
std::size_t WriteWiiItems(const ArchiveInput& input, const std::filesystem::path& folder)
{
    namespace song = hokuto::song;
    const ntdlib::rsar::SoundArchive& archive = *input.wii;
    std::set<std::string> written;
    std::set<uint32_t> files;
    std::map<song::ItemKind, std::size_t> counts;
    std::size_t failed = 0;
    const auto write = [&](const song::Item& item, std::size_t waves)
    {
        try
        {
            WriteItem(folder, song::ItemFileName(archive, item, waves), song::ItemBytes(archive, item), written,
                      item.index);
            counts[item.kind]++;
        }
        catch (const std::exception& error)
        {
            std::fprintf(stderr, "%s\n", error.what());
            failed++;
        }
    };

    for (std::size_t i = 0; i < archive.Sounds().size(); i++)
    {
        const ntdlib::rsar::SoundInfo& sound = archive.Sounds()[i];
        const bool sequence = sound.type == ntdlib::rsar::SoundType::kSequence;
        if ((sequence || sound.type == ntdlib::rsar::SoundType::kWaveSound) && files.insert(sound.file_id).second)
        {
            const song::ItemKind kind = sequence ? song::ItemKind::kSequence : song::ItemKind::kWaveSoundFile;
            write({kind, static_cast<uint32_t>(i), 0}, 0);
        }
    }

    for (std::size_t i = 0; i < archive.Banks().size(); i++)
    {
        write({song::ItemKind::kBank, static_cast<uint32_t>(i), 0}, 0);
    }

    for (const song::WiiWaveArchive& wave_archive : song::WaveArchivesOf(archive))
    {
        write({song::ItemKind::kWaveArchive, wave_archive.file_id, 0}, 0);
        std::size_t waves = 0;
        try
        {
            waves = song::WaveCountOf(archive, wave_archive.file_id);
        }
        catch (const std::exception& error)
        {
            std::fprintf(stderr, "%s\n", error.what());
            failed++;
        }
        for (std::size_t wave = 0; wave < waves; wave++)
        {
            write({song::ItemKind::kWave, wave_archive.file_id, static_cast<uint32_t>(wave)}, waves);
        }
    }

    for (const uint32_t sound : StreamSounds(input, archive))
    {
        const uint32_t file = archive.Sounds()[sound].file_id;
        WriteItem(folder, song::StreamFileName(archive, sound), input.streams.at(file), written, sound);
        counts[song::ItemKind::kStream]++;
    }

    std::printf("Wrote %s, %s, %s, %s, %s and %s.\n",
                Counted(counts[song::ItemKind::kSequence], "sequence file", "sequence files").c_str(),
                Counted(counts[song::ItemKind::kWaveSoundFile], "wave sound file", "wave sound files").c_str(),
                Counted(counts[song::ItemKind::kBank], "bank", "banks").c_str(),
                Counted(counts[song::ItemKind::kWaveArchive], "wave archive", "wave archives").c_str(),
                Counted(counts[song::ItemKind::kWave], "wave", "waves").c_str(),
                Counted(counts[song::ItemKind::kStream], "stream", "streams").c_str());

    return failed;
}

// Writes each wave of each wave archive of the Wii's archive of `input` into `folder` as a WAV file: the waves of an
// RWAR, and those that a WAVE block describes. Each wave sound then goes out at the rate at which it plays its wave,
// and each stream under the name of its file. A wave, a wave sound or a stream that can't be decoded is reported and
// left out. Returns the number left out.
std::size_t WriteWiiWavs(const ArchiveInput& input, const std::filesystem::path& folder)
{
    namespace song = hokuto::song;
    const ntdlib::rsar::SoundArchive& archive = *input.wii;
    std::set<std::string> written;
    std::size_t count = 0;
    std::size_t failed = 0;
    for (const song::WiiWaveArchive& wave_archive : song::WaveArchivesOf(archive))
    {
        try
        {
            const std::size_t waves = song::WaveCountOf(archive, wave_archive.file_id);
            for (std::size_t wave = 0; wave < waves; wave++)
            {
                try
                {
                    const auto number = static_cast<uint32_t>(wave);
                    WriteItem(folder, song::WavFileName(archive, wave_archive.file_id, number, waves),
                              song::WaveAsWav(archive, wave_archive.file_id, number), written, wave_archive.file_id);
                    count++;
                }
                catch (const std::exception& error)
                {
                    std::fprintf(stderr, "%s\n", error.what());
                    failed++;
                }
            }
        }
        catch (const std::exception& error)
        {
            std::fprintf(stderr, "%s\n", error.what());
            failed++;
        }
    }

    std::size_t wave_sounds = 0;
    for (std::size_t i = 0; i < archive.Sounds().size(); i++)
    {
        if (archive.Sounds()[i].type != ntdlib::rsar::SoundType::kWaveSound)
        {
            continue;
        }

        const auto sound = static_cast<uint32_t>(i);
        try
        {
            WriteItem(folder, song::WaveSoundWavFileName(archive, sound), song::WaveSoundAsWav(archive, sound), written,
                      sound);
            wave_sounds++;
        }
        catch (const std::exception& error)
        {
            std::fprintf(stderr, "%s\n", error.what());
            failed++;
        }
    }

    std::size_t streams = 0;
    for (const uint32_t sound : StreamSounds(input, archive))
    {
        try
        {
            std::string name = song::StreamFileName(archive, sound);
            name = name.substr(0, name.rfind('.')) + ".wav";
            WriteItem(folder, name, song::StreamAsWav(input.streams.at(archive.Sounds()[sound].file_id)), written,
                      sound);
            streams++;
        }
        catch (const std::exception& error)
        {
            std::fprintf(stderr, "%s\n", error.what());
            failed++;
        }
    }

    std::printf("Wrote %s of waves, %zu of wave sounds and %zu of streams.\n",
                Counted(count, "WAV file", "WAV files").c_str(), wave_sounds, streams);

    return failed;
}

// Writes the instruments of each bank of `input` into `folder`: a SoundFont for each bank, or with `sfz`, a folder of
// SFZ files for each bank, with the samples in the folder samples. The banks share the samples. The files take the
// bank's name, with its index after it when the name differs only in case from an earlier bank's. A bank past the
// SoundFont format's limits takes several SoundFonts, named after their programs (song::SoundFontNames). A bank that
// can't be converted is reported and left out. A bank without instruments is reported too, and gives no file. What the
// converter reports of a bank goes to stderr too. A bank of the Wii plays by the rules of the game's build. Returns the
// number of banks that couldn't be converted.
std::size_t WriteBanks(const ArchiveInput& input, const std::filesystem::path& folder, bool sfz)
{
    namespace song = hokuto::song;
    std::set<std::string> written;
    std::size_t count = 0;
    std::size_t empty = 0;
    std::size_t failed = 0;
    const std::size_t banks = input.wii ? input.wii->Banks().size() : input.archive->Banks().size();
    for (std::size_t i = 0; i < banks; i++)
    {
        const auto index = static_cast<uint32_t>(i);
        try
        {
            const song::Item item = {song::ItemKind::kBank, index, 0};
            const ntdlib::sequence_export::BankConversion bank =
                input.wii ? song::ConvertBank(*input.wii, index, "Hokuto", input.build)
                          : song::ConvertBank(*input.archive, index, "Hokuto");
            std::string name =
                input.wii ? song::ItemFileName(*input.wii, item) : song::ItemFileName(*input.archive, item);
            name.erase(name.rfind('.'));
            if (!sfz)
            {
                const std::vector<std::string> names = song::SoundFontNames(bank, name);
                for (std::size_t k = 0; k < names.size(); k++)
                {
                    WriteItem(folder, names[k], bank.sound_fonts[k].sf2, written, index);
                }
            }
            else
            {
                if (written.contains(xsflib::AsciiLower(name)))
                {
                    name += "_" + std::to_string(index);
                }
                written.insert(xsflib::AsciiLower(name));
                const auto replace = [](const std::vector<std::string>&)
                {
                    return true;
                };
                song::WriteBankSfz(bank, folder, name, replace);
            }
            for (const std::string& warning : bank.warnings)
            {
                std::fprintf(stderr, "%s: %s\n", name.c_str(), warning.c_str());
            }
            count++;
        }
        catch (const song::BankWithoutInstruments& error)
        {
            std::fprintf(stderr, "%s\n", error.what());
            empty++;
        }
        catch (const std::exception& error)
        {
            std::fprintf(stderr, "%s\n", error.what());
            failed++;
        }
    }

    std::printf("Wrote the instruments of %s.", Counted(count, "bank", "banks").c_str());
    if (empty > 0)
    {
        std::printf(" %zu %s no instruments.", empty, empty == 1 ? "bank holds" : "banks hold");
    }
    std::printf("\n");

    return failed;
}

// Writes the file of the console named `console` that holds sequence sound `sound` of the .hok file at `input` to
// `output`, in the binary form, and prints the warnings. `sound` is the sound's name, or else its number in the sound
// table. `console` is a name of the text form, such as 3ds or wii, or empty for the console whose files are nearest
// (song::OtherConsole). `mono` sets how the stereo waves go to the 3DS.
void WriteOtherConsole(const std::string& input, const std::string& sound, const std::string& output,
                       const std::string& console, hokuto::edit::MonoWaves mono)
{
    const hokuto::format::File file = ReadHok(input);
    if (!file.native || !file.native->tables)
    {
        throw std::runtime_error(input + " holds no sound archive whose tables Hokuto reads.");
    }

    // A song of the Wii goes to the Wii U unless the console is named.
    const ntdlib::sar::Flavor other =
        file.native->wii ? ntdlib::sar::Flavor::kCafe : hokuto::song::OtherConsole(file.native->flavor);
    const bool to_wii = console == hokuto::format::kWiiArchiveName;
    const std::optional<ntdlib::sar::Flavor> to =
        console.empty() ? std::optional(other) : hokuto::format::FlavorNamed(console);
    if (!to && !to_wii)
    {
        throw std::runtime_error("No console is named " + console + ". The consoles are 3ds, wii, wii_u and switch.");
    }

    const std::vector<hokuto::format::NativeSound>& sounds = file.native->tables->sounds;
    const auto named = std::ranges::find(sounds, sound, &hokuto::format::NativeSound::name);
    std::size_t index = static_cast<std::size_t>(named - sounds.begin());
    if (named == sounds.end())
    {
        const bool number = !sound.empty() && std::ranges::all_of(sound, [](char c) { return c >= '0' && c <= '9'; });
        index = number && sound.size() < 10 ? std::stoul(sound) : sounds.size();
    }
    if (index >= sounds.size())
    {
        throw std::runtime_error(input + " holds no sound " + sound + ".");
    }

    const hokuto::edit::OtherConsoleSong song =
        to_wii ? hokuto::edit::ForWii(file, static_cast<uint32_t>(index))
               : hokuto::edit::ForOtherConsole(file, static_cast<uint32_t>(index), *to, mono);
    WriteOutput(output, hokuto::format::Write(song.file));
    for (const std::string& warning : song.warnings)
    {
        std::printf("%s\n", warning.c_str());
    }
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

    std::string dol;
    if (arguments.size() == 6 && arguments[1] == "import" && arguments[2] == "--dol")
    {
        dol = arguments[3];
        arguments.erase(arguments.begin() + 2, arguments.begin() + 4);
    }

    std::size_t threads = hokuto::edit::MostLengthThreads();
    if (arguments.size() == 6 && arguments[1] == "lengths" && arguments[2] == "--threads")
    {
        // A whole number of 1 or more, in digits alone. Each thread holds a model of the console. The count therefore
        // stops at the processor's threads.
        const std::string& count = arguments[3];
        const bool digits = !count.empty() && count.size() <= 9 &&
                            std::ranges::all_of(count, [](char c) { return c >= '0' && c <= '9'; });
        const std::size_t asked = digits ? std::stoul(count) : 0;
        if (asked == 0)
        {
            const std::string given = count.empty() ? "an empty argument" : count;
            std::fprintf(stderr, "error: --threads takes a whole number of 1 or more, not %s.\n", given.c_str());
            return 2;
        }
        if (asked > threads)
        {
            std::fprintf(stderr, "The processor runs %zu threads. The sounds play %zu at a time.\n", threads, threads);
        }
        threads = std::min(asked, threads);
        arguments.erase(arguments.begin() + 2, arguments.begin() + 4);
    }

    if (arguments.size() != 4)
    {
        std::fputs(kUsage, stderr);
        return 2;
    }

    const std::string& command = arguments[1];
    const std::string& input = arguments[2];
    const std::string& output = arguments[3];
    if (command == "import")
    {
        // The file keeps the group files and the streams beside the archive, and a Wii archive's game's build of NW4R
        // SND. The program finds them the same way.
        const std::vector<uint8_t> bytes = ReadInput(input);
        const std::filesystem::path path = xsflib::PathFromUtf8(input);
        const bool wii = bytes.size() >= 4 && std::memcmp(bytes.data(), "RSAR", 4) == 0;
        if (!dol.empty() && !wii)
        {
            throw std::runtime_error("--dol applies to a sound archive of the Wii only, and " + input + " isn't one.");
        }

        hokuto::format::File file;
        try
        {
            if (wii)
            {
                const auto archive =
                    hokuto::song::LoadWiiArchive(bytes, xsflib::Utf8(path.filename()), ntdlib::rvl::snd::kRsarOptions);
                file = hokuto::format::ImportWiiArchive(bytes, GameBuild(path, dol),
                                                        hokuto::song::ReadStreams(*archive, path));
            }
            else
            {
                const auto archive = hokuto::song::LoadArchive(bytes, xsflib::Utf8(path.filename()), path);
                file = hokuto::format::ImportArchive(bytes, archive->ReadOptions().flavor, archive->GroupFiles(),
                                                     hokuto::song::ReadStreams(*archive, path),
                                                     archive->GroupFileGroups());
            }
        }
        catch (const xsflib::FormatError& error)
        {
            throw std::runtime_error("Hokuto can't import " + input + ". " + hokuto::format::AsSentence(error.what()));
        }

        WriteOutput(output, hokuto::format::Write(file));
    }
    else if (command == "export")
    {
        const hokuto::format::File file = ReadHok(input);
        const std::vector<uint8_t> archive = hokuto::format::ExportArchive(file);
        WriteOutput(output, archive);
        WriteStreams(file, archive, output);
        WriteGroupFiles(file, archive, input, output);
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
        // The model's refusal is a clause, such as "an archive laid out another way: the archive is cut short".
        hokuto::format::File file;
        try
        {
            file = hokuto::format::InModelForm(ReadHok(input));
        }
        catch (const xsflib::FormatError& error)
        {
            throw std::runtime_error(input + " can't take the model form. " + hokuto::format::AsSentence(error.what()));
        }

        WriteOutput(output, hokuto::format::Write(file));
    }
    else if (command == "lengths")
    {
        // The sounds play from the archive that the file builds, with the group files that it builds beside it. A sound
        // of the Wii plays on the model of the file's build of NW4R SND.
        const hokuto::format::File file = ReadHok(input);
        if (!file.native)
        {
            throw std::runtime_error(input + " holds no sound archive.");
        }

        const std::string name = xsflib::Utf8(xsflib::PathFromUtf8(input).filename());
        std::optional<hokuto::format::File> measured;
        if (file.native->wii)
        {
            const auto archive =
                hokuto::song::LoadWiiArchive(hokuto::format::ExportArchive(file), name, ntdlib::rvl::snd::kRsarOptions);
            measured =
                hokuto::edit::WithLengths(file, *archive, hokuto::format::BuildOfWiiArchive(*file.native), threads);
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
    else if (command == "items" || command == "wav" || command == "sf2" || command == "sfz")
    {
        // The folder is made if it's missing. A failure leaves out the item, and the others go on.
        const ArchiveInput archive = ReadArchive(input);
        const std::filesystem::path folder = xsflib::PathFromUtf8(output);
        std::error_code error;
        std::filesystem::create_directories(folder, error);
        if (error)
        {
            throw std::runtime_error("can't make " + output + ": " + error.message());
        }

        std::size_t failed = 0;
        if (command == "items")
        {
            failed = archive.wii ? WriteWiiItems(archive, folder) : WriteItems(archive, folder);
        }
        else if (command == "wav")
        {
            failed = archive.wii ? WriteWiiWavs(archive, folder) : WriteWavs(archive, folder);
        }
        else
        {
            failed = WriteBanks(archive, folder, command == "sfz");
        }

        return failed > 0 ? 1 : 0;
    }
    else
    {
        std::fputs(kUsage, stderr);
        return 2;
    }

    return 0;
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
