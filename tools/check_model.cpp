// SPDX-License-Identifier: MIT

// Checks Hokuto's model form (src/format/model.h) on a sound archive of the Switch, the Wii U, the 3DS or the Wii, with
// the group files and the streams beside it. The tool imports the archive as a .hok file, and puts the file in the
// model form. The file has to come back from its binary form and from its text form unchanged. The tables that ntdlib
// reads from the archive that the model builds have to be the game's. Each sound then plays on ntdlib's model of the
// console from both archives, by the strict rules, with the group files beside each archive: a sequence sound, a wave
// sound or a stream sound, until nothing sounds or for `seconds` seconds. A stream sound plays the stream beside the
// game's archive, and the stream that the file keeps. The output has to be the same, sample for sample.
//
// A 3DS archive whose model gives the game's bytes back plays the same from both, and the tool plays nothing. Its
// sounds otherwise play on ntdlib's archive mode of the 3DS, on ntdlib's model of the DSP's audio firmware.
//
// A Wii archive takes the game's build of NW4R SND from the main.dol of the extracted disc that holds it. An import
// does the same. The file has to keep the build, and the streams beside the archive. An archive whose model gives the
// game's bytes back plays the same from both. Otherwise each sequence sound plays dry on ntdlib's model of NW4R SND and
// AX from both archives (song::RenderDry), and the renders have to be the same. Each other file of the archive has to
// keep the game's bytes.
//
// The tool prints a line for each sound that plays otherwise, then a summary: the sounds of each kind, the commands
// whose encoder's choices the model leaves out, whether the model gave the game's bytes back, and the sizes of the file
// in its two forms. A sound that neither archive can start counts apart. It returns 1 when a sound plays otherwise, the
// file doesn't come back or the tables differ, and 2 when the archive can't be read or held in the model form.
//
// usage: hokuto_check_model <archive> [seconds]

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "ntdlib/cafe/snd/engine.h"
#include "ntdlib/ctr/snd/engine.h"
#include "ntdlib/ctr/snd/sar_options.h"
#include "ntdlib/nx/atk/engine.h"
#include "ntdlib/rsar/formats.h"
#include "ntdlib/rvl/snd/rsar_options.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sound_build.h"
#include "xsflib/console.h"
#include "xsflib/files.h"

#include "format/archive.h"
#include "format/file.h"
#include "format/model.h"
#include "format/text.h"
#include "song/archive.h"
#include "song/dry_render.h"
#include "song/sequence.h"
#include "song/streams.h"

namespace
{

namespace atk = ntdlib::nx::atk;
namespace cafe = ntdlib::cafe::snd;
namespace format = hokuto::format;
namespace sar = ntdlib::sar;
namespace song = hokuto::song;

// The output's rate, and the samples of each channel in a frame of the 3DS's DSP.
constexpr std::size_t kOutputRate = 48000;
constexpr std::size_t kCtrFrameSamples = 160;

// The output of sound `sound` of `archive` on the console's model, until nothing sounds or for `limit` frames, or
// nothing if the sound doesn't start. A stream sound plays `stream`.
template <class ArchiveModel>
std::optional<std::vector<int16_t>> Play(const sar::SoundArchive& archive, uint32_t sound,
                                         const std::vector<uint8_t>& stream, std::size_t limit)
{
    ArchiveModel model(archive);
    bool started = false;
    switch (archive.Sounds()[sound].type)
    {
    case sar::SoundType::kSequence:
        started = model.engine->StartSequence(sound) != nullptr;
        break;
    case sar::SoundType::kWaveSound:
        started = model.engine->StartWaveSound(sound) != nullptr;
        break;
    case sar::SoundType::kStream:
        started = !stream.empty() && model.engine->StartStreamSound(sound, stream) != nullptr;
        break;
    default:
        break;
    }
    if (!started)
    {
        return std::nullopt;
    }

    std::vector<int16_t> samples;
    for (std::size_t frame = 0; frame < limit && model.engine->IsBusy(); frame++)
    {
        model.engine->RunFrame();
        std::span<const int16_t> output;
        if constexpr (std::is_same_v<ArchiveModel, cafe::ArchiveModel>)
        {
            output = model.system.Output();
        }
        else
        {
            output = model.renderer.DeviceOutput();
        }
        samples.insert(samples.end(), output.begin(), output.end());
    }

    return samples;
}

// The output of sound `sound` of a 3DS archive on archive mode, until nothing sounds or for `frames` frames, or nothing
// if the sound doesn't start. A stream sound plays `stream`.
std::optional<std::vector<int16_t>> PlayCtr(const sar::SoundArchive& archive, uint32_t sound,
                                            const std::vector<uint8_t>& stream, std::size_t frames)
{
    std::vector<int16_t> samples;
    ntdlib::ctr::snd::ArchiveModel model;
    const auto keep_sample = [&samples](std::array<int16_t, 2> sample)
    {
        samples.insert(samples.end(), sample.begin(), sample.end());
    };
    model.dsp->SetSampleSink(keep_sample);
    if (!model.Start(archive))
    {
        return std::nullopt;
    }
    bool started = false;
    switch (archive.Sounds()[sound].type)
    {
    case sar::SoundType::kSequence:
        started = model.engine->StartSequence(sound) != nullptr;
        break;
    case sar::SoundType::kWaveSound:
        started = model.engine->StartWaveSound(sound) != nullptr;
        break;
    case sar::SoundType::kStream:
        started = !stream.empty() && model.engine->StartStreamSound(sound, stream) != nullptr;
        break;
    default:
        break;
    }
    if (!started)
    {
        return std::nullopt;
    }
    for (std::size_t frame = 0; frame < frames && model.engine->IsBusy() && model.engine->RunFrame(); frame++)
    {
    }

    return samples;
}

// The archive in `bytes`, read by the strict rules, with `group_files` beside it. A 3DS archive takes archive mode's
// rules.
sar::SoundArchive Strict(std::vector<uint8_t> bytes, sar::Flavor flavor,
                         const std::vector<std::vector<uint8_t>>& group_files)
{
    sar::Options options;
    options.flavor = flavor;
    options.rules = sar::Rules::kStopAtFirstProblem;
    if (flavor == sar::Flavor::kCtr)
    {
        options = ntdlib::ctr::kSarOptions;
    }
    sar::SoundArchive archive = sar::SoundArchive::Load(std::move(bytes), options);
    for (const std::vector<uint8_t>& group_file : group_files)
    {
        archive.AddGroupFile(group_file);
    }

    return archive;
}

// The name of a kind of sound in the summary.
const char* KindName(sar::SoundType type)
{
    switch (type)
    {
    case sar::SoundType::kSequence:
        return "sequence";
    case sar::SoundType::kWaveSound:
        return "wave";
    case sar::SoundType::kStream:
        return "stream";
    default:
        return "unknown";
    }
}

// The commands of `file`'s sequences whose encoder's choices the model leaves out.
std::size_t ChoicesIn(const format::File& file)
{
    std::size_t choices = 0;
    for (const auto& sequence : file.sequences)
    {
        choices += static_cast<std::size_t>(std::ranges::count_if(sequence->items, format::HasChoices));
    }

    return choices;
}

// The check of `bytes`, an archive of the Wii at `path`.
int RunWii(const std::filesystem::path& path, const std::vector<uint8_t>& bytes, int seconds)
{
    const std::string name = xsflib::Utf8(path.filename());
    const auto began = std::chrono::steady_clock::now();
    format::File file;
    format::File model;
    std::map<uint32_t, std::vector<uint8_t>> streams;
    try
    {
        std::vector<uint8_t> dol;
        const std::optional<std::filesystem::path> found = song::FindMainDol(path);
        const std::optional<ntdlib::SndBuild> build =
            found && xsflib::ReadFile(*found, dol) ? song::ReadWiiBuild(dol) : std::nullopt;
        streams = song::ReadStreams(*song::LoadWiiArchive(bytes, name, ntdlib::rvl::snd::kRsarOptions), path);
        file = format::ImportWiiArchive(bytes, build, streams);
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "%s: %s\n", name.c_str(), error.what());
        return 2;
    }
    try
    {
        model = format::InModelForm(file);
    }
    catch (const std::exception& error)
    {
        std::printf("%s: the model form can't hold the archive. %s\n", name.c_str(), error.what());
        return 2;
    }

    // The file comes back from both forms. The archive that the model builds holds the game's tables and build.
    int result = 0;
    const std::vector<uint8_t> binary = format::Write(model);
    const std::string text = format::WriteText(model);
    if (!(format::Read(binary) == model))
    {
        std::printf("%s: the model form doesn't come back from the binary form.\n", name.c_str());
        result = 1;
    }
    if (!(format::ReadText(text) == model))
    {
        std::printf("%s: the model form doesn't come back from the text form.\n", name.c_str());
        result = 1;
    }
    if (!(model.native->tables == file.native->tables) || model.native->wii_build != file.native->wii_build)
    {
        std::printf("%s: the model's archive holds other tables or another build than the game's.\n", name.c_str());
        result = 1;
    }

    // The model form keeps each stream beside the archive.
    bool same_streams = model.native->streams.size() == streams.size();
    for (const auto& [id, stream] : streams)
    {
        const auto kept = model.native->streams.find(id);
        same_streams = same_streams && kept != model.native->streams.end() && *kept->second.bytes == stream;
    }
    if (!same_streams)
    {
        std::printf("%s: the model form doesn't keep the streams beside the archive.\n", name.c_str());
        result = 1;
    }

    std::vector<uint8_t> built = format::ExportArchive(model);
    const bool same_bytes = built == bytes;
    const double taken_so_far = std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
    if (same_bytes)
    {
        std::printf(
            "%s: the model gives the game's bytes back, and they play as the game's. The model leaves out the "
            "choices of %zu commands, and keeps %zu streams. The file takes %zu bytes in the model form, %zu in the "
            "text form. %.1f s.\n",
            name.c_str(), ChoicesIn(file), streams.size(), binary.size(), text.size(), taken_so_far);
        return result;
    }

    // Each other file keeps the game's bytes, and each sequence sound plays the same from both archives.
    const ntdlib::SndBuild build = format::BuildOfWiiArchive(*file.native);
    const auto game = song::LoadWiiArchive(bytes, name, ntdlib::rvl::snd::kRsarOptions);
    const auto ours = song::LoadWiiArchive(built, name, ntdlib::rvl::snd::kRsarOptions);
    const std::vector<std::optional<uint32_t>> sequences = format::SequencesOfFiles(file, bytes, *game);
    for (uint32_t id = 0; id < sequences.size(); ++id)
    {
        const auto a = game->FileData(id);
        const auto b = ours->FileData(id);
        if (!sequences[id] && !std::ranges::equal(a, b))
        {
            std::printf("%s: file %u holds other bytes in the model's archive.\n", name.c_str(), id);
            result = 1;
        }
    }

    const std::atomic<bool> kCancel = false;
    std::size_t played = 0;
    std::size_t differ = 0;
    for (uint32_t sound = 0; sound < game->Sounds().size(); ++sound)
    {
        if (game->Sounds()[sound].type != ntdlib::rsar::SoundType::kSequence)
        {
            continue;
        }

        song::DryRender expected;
        song::DryRender output;
        try
        {
            const auto read = [&](const auto& archive)
            {
                return song::RenderDry(
                    archive, std::make_shared<const song::Sequence>(song::ReadSequence(*archive, sound, {}, {}, build)),
                    seconds, kCancel);
            };
            expected = read(game);
            output = read(ours);
        }
        catch (const std::exception& error)
        {
            std::printf("%s: sequence sound %s doesn't play. %s\n", name.c_str(), game->Sounds()[sound].name.c_str(),
                        error.what());
            differ++;
            result = 1;
            continue;
        }
        played++;
        if (expected.samples != output.samples)
        {
            std::printf("%s: sequence sound %s plays otherwise.\n", name.c_str(), game->Sounds()[sound].name.c_str());
            differ++;
            result = 1;
        }
    }

    const double taken = std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
    std::printf(
        "%s: %zu sequence sounds played for %d s at most, and %zu differ. The model leaves out the choices of "
        "%zu commands, keeps %zu streams and gives other bytes. The file takes %zu bytes in the model form, %zu in "
        "the text form. %.1f s.\n",
        name.c_str(), played, seconds, differ, ChoicesIn(file), streams.size(), binary.size(), text.size(), taken);

    return result;
}

// The tool, with the command line's `arguments`. Returns the tool's exit status.
int Run(const std::vector<std::string>& arguments)
{
    if (arguments.size() < 2 || arguments.size() > 3)
    {
        std::fprintf(stderr, "usage: hokuto_check_model <archive> [seconds]\n");
        return 2;
    }

    FILE* const kOut = stdout;

    const std::filesystem::path path = xsflib::PathFromUtf8(arguments[1]);
    const int seconds = arguments.size() > 2 ? std::max(std::atoi(arguments[2].c_str()), 1) : 30;
    const std::string name = xsflib::Utf8(path.filename());
    const auto began = std::chrono::steady_clock::now();
    {
        std::vector<uint8_t> head;
        if (xsflib::ReadFile(path, head) && head.size() >= 4 &&
            std::string_view(reinterpret_cast<const char*>(head.data()), 4) == "RSAR")
        {
            return RunWii(path, head, seconds);
        }
    }

    // The game's archive as a file, and the file in the model form.
    std::vector<uint8_t> bytes;
    format::File file;
    format::File model;
    std::map<uint32_t, std::vector<uint8_t>> streams;
    std::vector<std::vector<uint8_t>> group_files;
    sar::Flavor flavor = sar::Flavor::kNx;
    try
    {
        xsflib::ReadFile(path, bytes);
        const auto archive = song::LoadArchive(bytes, name, path);
        flavor = archive->ReadOptions().flavor;
        group_files = archive->GroupFiles();
        streams = song::ReadStreams(*archive, path);
        file = format::ImportArchive(bytes, flavor, group_files, streams, archive->GroupFileGroups());
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "%s: %s\n", name.c_str(), error.what());
        return 2;
    }
    try
    {
        model = format::InModelForm(file);
    }
    catch (const std::exception& error)
    {
        std::fprintf(kOut, "%s: the model form can't hold the archive. %s\n", name.c_str(), error.what());
        return 2;
    }

    // The file comes back from both forms. The archive that the model builds holds the game's tables.
    int result = 0;
    const std::size_t choices = ChoicesIn(file);
    const std::vector<uint8_t> binary = format::Write(model);
    const std::string text = format::WriteText(model);
    if (!(format::Read(binary) == model))
    {
        std::fprintf(kOut, "%s: the model form doesn't come back from the binary form.\n", name.c_str());
        result = 1;
    }
    if (!(format::ReadText(text) == model))
    {
        std::fprintf(kOut, "%s: the model form doesn't come back from the text form.\n", name.c_str());
        result = 1;
    }
    if (!(model.native->tables == file.native->tables))
    {
        std::fprintf(kOut, "%s: the model's archive holds other tables than the game's.\n", name.c_str());
        result = 1;
    }
    std::vector<uint8_t> built = format::ExportArchive(model);
    const std::vector<std::vector<uint8_t>> built_groups = format::ExportGroupFiles(model);
    const bool same_bytes = built == bytes && built_groups == group_files;

    // Each sound plays the same from both archives. The same bytes of the 3DS play the same.
    const sar::SoundArchive game = Strict(bytes, flavor, group_files);
    const sar::SoundArchive ours = Strict(std::move(built), flavor, built_groups);
    const bool wii_u = flavor == sar::Flavor::kCafe;
    const bool ctr = flavor == sar::Flavor::kCtr;
    const std::size_t frame_samples = wii_u ? cafe::ArchiveModel::kFrameSamples : atk::ArchiveModel::kFrameSamples;
    const std::size_t limit =
        ctr ? song::FramesIn(static_cast<uint64_t>(seconds) * 1000, song::FrameMsOf(sar::Flavor::kCtr))
            : static_cast<std::size_t>(seconds) * kOutputRate / frame_samples;
    std::map<sar::SoundType, std::size_t> played;
    std::size_t silent = 0;
    std::size_t differ = 0;
    for (uint32_t sound = 0; sound < game.Sounds().size() && !(ctr && same_bytes); sound++)
    {
        const sar::SoundInfo& info = game.Sounds()[sound];
        std::vector<uint8_t> game_stream;
        std::vector<uint8_t> our_stream;
        if (info.type == sar::SoundType::kStream)
        {
            if (const auto found = streams.find(info.file_id); found != streams.end())
            {
                game_stream = found->second;
            }
            if (const auto found = model.native->streams.find(info.file_id); found != model.native->streams.end())
            {
                our_stream = *found->second.bytes;
            }
        }

        const auto play = [&](const sar::SoundArchive& archive, const std::vector<uint8_t>& stream)
        {
            return ctr     ? PlayCtr(archive, sound, stream, limit)
                   : wii_u ? Play<cafe::ArchiveModel>(archive, sound, stream, limit)
                           : Play<atk::ArchiveModel>(archive, sound, stream, limit);
        };
        const std::optional<std::vector<int16_t>> expected = play(game, game_stream);
        const std::optional<std::vector<int16_t>> output = play(ours, our_stream);
        if (!expected && !output)
        {
            silent++;
            continue;
        }

        played[info.type]++;
        if (expected != output)
        {
            const std::size_t length = std::min(expected ? expected->size() : 0, output ? output->size() : 0);
            std::size_t first = 0;
            while (first < length && (*expected)[first] == (*output)[first])
            {
                first++;
            }
            std::fprintf(kOut, "%s: %s sound %s plays otherwise from frame %zu.\n", name.c_str(), KindName(info.type),
                         info.name.c_str(), first / 2 / (ctr ? kCtrFrameSamples : frame_samples));
            differ++;
            result = 1;
        }
    }

    const double taken = std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
    if (ctr && same_bytes)
    {
        std::fprintf(kOut,
                     "%s: the model gives the game's bytes back, and they play as the game's. The model leaves out the "
                     "choices of %zu commands. The file takes %zu bytes in the model form, %zu in the text form. %.1f "
                     "s.\n",
                     name.c_str(), choices, binary.size(), text.size(), taken);
        return result;
    }
    std::fprintf(
        kOut,
        "%s: %zu sequence, %zu wave and %zu stream sounds played for %d s at most, and %zu differ. %zu start on "
        "neither. The model leaves out the choices of %zu commands, and gives %s. The file takes %zu bytes in the "
        "model form, %zu in the text form. %.1f s.\n",
        name.c_str(), played[sar::SoundType::kSequence], played[sar::SoundType::kWaveSound],
        played[sar::SoundType::kStream], seconds, differ, silent, choices,
        same_bytes ? "the game's bytes back" : "other bytes", binary.size(), text.size(), taken);

    return result;
}

} // namespace

int main(int argc, char** argv)
{
    try
    {
        return Run(xsflib::console::Arguments(argc, argv));
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "%s\n", error.what());
        return 2;
    }
}
