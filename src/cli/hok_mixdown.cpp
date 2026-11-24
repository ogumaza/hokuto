// SPDX-License-Identifier: MIT

// hok mixdown: renders sequence sounds to WAV files the way the program plays them. Each sound plays on the model of
// what plays it, in its mixing mode, with the file's effects and the volumes of its FX channels. This source file also
// contains what the batch commands share: the choice of sounds and the output folder.

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#include "xsflib/files.h"

#include "cli/hok_parts.h"
#include "edit/sound_exports.h"
#include "format/engines.h"
#include "format/file.h"
#include "song/batch.h"
#include "song/mixdown.h"
#include "song/mixdown_files.h"
#include "song/sequence_model.h"

namespace hokuto::cli::hok_parts
{
namespace
{

// What hok mixdown renders, and how.
struct MixdownOptions
{
    song::MixdownSettings settings;
    std::optional<song::Mixing> mixing; // none uses each sound's mode
    bool stems = false;
    std::vector<std::string> sounds; // names or numbers; none renders every sequence sound
    std::size_t threads = 1;
    std::string input;
    std::string folder;
};

// `text` as a whole number from `low` to `high`, or nothing.
std::optional<int> WholeNumber(std::string_view text, int low, int high)
{
    int value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc() || end != text.data() + text.size() || value < low || value > high)
    {
        return std::nullopt;
    }

    return value;
}

// `text` as a number of seconds from 0 to 60, such as 10 or 2.5, or nothing.
std::optional<double> Seconds(std::string_view text)
{
    // std::from_chars of a double requires macOS 26. The C locale reads a full stop as the decimal point.
    const std::string copy(text);
    char* end = nullptr;
    const double value = std::strtod(copy.c_str(), &end);
    if (copy.empty() || end != copy.c_str() + copy.size() || !(value >= 0.0 && value <= 60.0))
    {
        return std::nullopt;
    }

    return value;
}

// The sample format that `text` names: 16, 24 or 32f.
std::optional<song::SampleFormat> FormatNamed(std::string_view text)
{
    if (text == "16" || text == "24" || text == "32f")
    {
        return text == "16"   ? song::SampleFormat::kPcm16
               : text == "24" ? song::SampleFormat::kPcm24
                              : song::SampleFormat::kFloat32;
    }

    return std::nullopt;
}

// The mixing mode that `text` names: strict or enhanced.
std::optional<song::Mixing> MixingNamed(std::string_view text)
{
    if (text == "strict" || text == "enhanced")
    {
        return text == "strict" ? song::Mixing::kStrict : song::Mixing::kEnhanced;
    }

    return std::nullopt;
}

// What a stem keeps that `text` names: dry or effects. A file contains no Stereo Out level. The command therefore has
// no stereo-out.
std::optional<song::StemEffects> StemEffectsNamed(std::string_view text)
{
    if (text == "dry" || text == "effects")
    {
        return text == "dry" ? song::StemEffects::kDry : song::StemEffects::kEffects;
    }

    return std::nullopt;
}

// The value `read` of `option`. Throws the message of a value that doesn't fit, with what the option accepts.
template <typename Value>
Value Taken(const std::string& option, const std::string& value, std::optional<Value> read, const std::string& takes)
{
    if (!read)
    {
        throw std::invalid_argument(option + " takes " + takes + ", not " + (value.empty() ? "nothing" : value) + ".");
    }

    return *read;
}

// The options of hok mixdown from `arguments`: the options, then the input and the folder.
MixdownOptions ParseMixdown(std::span<const std::string> arguments)
{
    if (arguments.size() < 4)
    {
        throw std::invalid_argument("hok mixdown requires an input and a folder.");
    }

    MixdownOptions options;
    const std::size_t end = arguments.size() - 2;
    const unsigned most = std::max(1u, std::thread::hardware_concurrency());
    constexpr std::array kValued = {"--loops",  "--fade",  "--tail",         "--rate",   "--format",
                                    "--mixing", "--sound", "--stem-effects", "--threads"};
    song::MixdownSettings& settings = options.settings;
    for (std::size_t at = 2; at < end; at++)
    {
        const std::string& option = arguments[at];
        bool* const flag = option == "--stems"       ? &options.stems
                           : option == "--seamless"  ? &settings.seamless
                           : option == "--normalise" ? &settings.form.normalise
                           : option == "--dither"    ? &settings.form.dither
                                                     : nullptr;
        if (flag != nullptr)
        {
            *flag = true;
            continue;
        }
        if (std::ranges::find(kValued, option) == kValued.end())
        {
            throw std::invalid_argument("Unknown option: " + option + ".");
        }
        if (at + 1 >= end)
        {
            throw std::invalid_argument(option + " requires a value.");
        }

        const std::string& value = arguments[++at];
        if (option == "--loops")
        {
            settings.loops = Taken(option, value, WholeNumber(value, 1, 99), "a whole number from 1 to 99");
        }
        else if (option == "--fade")
        {
            settings.fade_seconds = Taken(option, value, Seconds(value), "seconds from 0 to 60");
        }
        else if (option == "--tail")
        {
            settings.tail_seconds = Taken(option, value, Seconds(value), "seconds from 0 to 60");
        }
        else if (option == "--rate")
        {
            settings.form.rate =
                Taken(option, value, WholeNumber(value, 8000, 192000), "a rate from 8000 to 192000 Hz");
        }
        else if (option == "--format")
        {
            settings.form.format = Taken(option, value, FormatNamed(value), "16, 24 or 32f");
        }
        else if (option == "--stem-effects")
        {
            settings.stem_effects = Taken(option, value, StemEffectsNamed(value), "dry or effects");
        }
        else if (option == "--mixing")
        {
            options.mixing = Taken(option, value, MixingNamed(value), "strict or enhanced");
        }
        else if (option == "--sound")
        {
            options.sounds.push_back(value);
        }
        else
        {
            const std::string takes = "a whole number from 1 to " + std::to_string(most);
            options.threads =
                static_cast<std::size_t>(Taken(option, value, WholeNumber(value, 1, static_cast<int>(most)), takes));
        }
    }

    options.input = arguments[end];
    options.folder = arguments[end + 1];

    return options;
}

// Renders `sound` of `source` into `files` in `folder`, with warnings for a render that stopped at the limit or cut
// its tails. Returns the count of files written.
std::size_t WriteSound(const edit::BatchSource& source, const song::BatchSound& sound, const edit::BatchSequence& read,
                       std::span<const song::MixdownFile> files, const MixdownOptions& options,
                       const std::filesystem::path& folder)
{
    song::MixdownSettings settings = options.settings;
    settings.mix.bus_volumes = source.file ? source.file->aux_volumes : song::kFullVolumes;
    const song::SoundFiles written =
        edit::WriteMixdownSound(source, sound.sound, read, files, settings, options.mixing, folder);
    if (written.limited)
    {
        std::fprintf(stderr, "%s plays on past 10 minutes, and fades there.\n", sound.name.c_str());
    }
    if (written.tails_cut)
    {
        std::fprintf(stderr, "%s still sounds %g s after its end, and fades there.\n", sound.name.c_str(),
                     settings.tail_seconds);
    }

    return written.files;
}

} // namespace

// The sounds of `all` that `chosen` names, by name or by number, in the order of `all`. With none chosen, it returns
// them all.
std::vector<song::BatchSound> ChosenSounds(const std::vector<song::BatchSound>& all,
                                           const std::vector<std::string>& chosen, const std::string& input,
                                           const std::string& driver)
{
    if (chosen.empty())
    {
        return all;
    }

    std::vector<bool> taken(all.size());
    for (const std::string& wanted : chosen)
    {
        const std::optional<int> number = WholeNumber(wanted, 0, 999999999);
        const auto named = [&](const song::BatchSound& sound)
        {
            return sound.name == wanted || (number && sound.sound == static_cast<uint32_t>(*number));
        };
        const auto found = std::ranges::find_if(all, named);
        if (found == all.end())
        {
            const std::string where =
                driver.empty() ? "." : " in the archive of " + driver + ". --driver chooses another archive.";
            throw std::runtime_error(input + " contains no sequence sound " + wanted + where);
        }

        taken[static_cast<std::size_t>(found - all.begin())] = true;
    }

    std::vector<song::BatchSound> sounds;
    for (std::size_t i = 0; i < all.size(); i++)
    {
        if (taken[i])
        {
            sounds.push_back(all[i]);
        }
    }

    return sounds;
}

std::filesystem::path MadeFolder(const std::string& path)
{
    const std::filesystem::path folder = xsflib::PathFromUtf8(path);
    std::error_code made;
    std::filesystem::create_directories(folder, made);
    if (made)
    {
        throw std::runtime_error("Can't create " + path + ": " + made.message() + ".");
    }

    return folder;
}

int RunMixdown(std::span<const std::string> arguments, const std::function<ArchiveInput(const std::string&)>& read)
{
    const MixdownOptions options = ParseMixdown(arguments);
    const ArchiveInput input = read(options.input);
    const std::vector<song::BatchSound> sounds =
        ChosenSounds(song::BatchSounds(input.played), options.sounds, options.input, input.driver);
    if (sounds.empty())
    {
        throw std::runtime_error(options.input + " contains no sequence sound.");
    }

    const std::filesystem::path folder = MadeFolder(options.folder);
    const edit::BatchSource source = {
        .played = input.played, .build = input.build, .file = input.hok, .starts = input.starts, .name = options.input};
    const std::vector<edit::BatchSequence> sequences = edit::ReadBatchSequences(source, sounds, options.threads);
    const std::vector<std::vector<song::MixdownFile>> files =
        edit::MixdownFileNames(sounds, sequences, {}, "", true, options.stems, {});

    // A sound that fails is left out, and the others go on.
    std::atomic<std::size_t> written{0};
    std::atomic<std::size_t> failed{0};
    std::mutex report;
    edit::ForEachOnThreads(sounds.size(), options.threads,
                           [&](std::size_t i)
                           {
                               try
                               {
                                   if (!sequences[i].sequence)
                                   {
                                       throw std::runtime_error(sequences[i].error);
                                   }
                                   written += WriteSound(source, sounds[i], sequences[i], files[i], options, folder);
                               }
                               catch (const std::exception& error)
                               {
                                   const std::lock_guard lock(report);
                                   std::fprintf(stderr, "Can't render %s: %s\n", sounds[i].name.c_str(), error.what());
                                   failed++;
                               }
                           });

    std::printf("Rendered %s to %s.\n", Counted(sounds.size() - failed, "sequence sound", "sequence sounds").c_str(),
                Counted(written, "WAV file", "WAV files").c_str());

    return failed > 0 ? 1 : 0;
}

} // namespace hokuto::cli::hok_parts
