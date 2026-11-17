// SPDX-License-Identifier: MIT

// hok midi: exports sequence sounds as MIDI files with their instruments. The Export window exports the shown sound
// the same way. Each MIDI file gets a SoundFont 2 file beside it, or with --sfz, SFZ files in the folder sfz. A .hok
// file's sounds take their tags.

#include <cstdio>
#include <exception>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

#include "xsflib/files.h"

#include "cli/hok_parts.h"
#include "edit/sound_exports.h"
#include "song/batch.h"
#include "song/driver.h"
#include "song/midi_export.h"

namespace hokuto::cli::hok_parts
{
namespace
{

// What hok midi exports, and how.
struct MidiOptions
{
    bool sfz = false;
    std::vector<std::string> sounds; // names or numbers; none exports every sequence sound
    std::string input;
    std::string folder;
};

// The options of hok midi from `arguments`: the options, then the input and the folder.
MidiOptions ParseMidi(std::span<const std::string> arguments)
{
    if (arguments.size() < 4)
    {
        throw std::invalid_argument("hok midi requires an input and a folder.");
    }

    MidiOptions options;
    const std::size_t end = arguments.size() - 2;
    for (std::size_t at = 2; at < end; at++)
    {
        if (arguments[at] == "--sfz")
        {
            options.sfz = true;
        }
        else if (arguments[at] == "--sound" && at + 1 < end)
        {
            options.sounds.push_back(arguments[++at]);
        }
        else
        {
            throw std::invalid_argument(arguments[at] == "--sound" ? "--sound requires a value."
                                                                   : "Unknown option: " + arguments[at] + ".");
        }
    }

    options.input = arguments[end];
    options.folder = arguments[end + 1];

    return options;
}

// Throws the reason why `input` makes no SFZ files, if it makes none: MP2K's converter and some drivers write none.
void CheckSfz(const ArchiveInput& input)
{
    if (input.played.Gba())
    {
        throw std::runtime_error(
            "hok midi --sfz doesn't support GBA ROMs with MusicPlayer2000. Leave out --sfz to "
            "write SoundFont 2 files.");
    }
    if (input.played.Driver() && !input.played.Driver()->WritesSfz())
    {
        throw std::runtime_error("hok midi --sfz doesn't support ROMs with " +
                                 std::string(input.played.Driver()->Rules().Title()) +
                                 ". Leave out --sfz to write SoundFont 2 files.");
    }
}

} // namespace

int RunMidi(std::span<const std::string> arguments, const std::function<ArchiveInput(const std::string&)>& read)
{
    const MidiOptions options = ParseMidi(arguments);
    const ArchiveInput input = read(options.input);
    if (options.sfz)
    {
        CheckSfz(input);
    }

    const std::vector<song::BatchSound> sounds =
        ChosenSounds(song::BatchSounds(input.played), options.sounds, options.input);
    if (sounds.empty())
    {
        throw std::runtime_error(options.input + " contains no sequence sound.");
    }

    const std::filesystem::path folder = MadeFolder(options.folder);
    const edit::BatchSource source = {
        .played = input.played, .build = input.build, .file = input.hok, .name = options.input};
    const std::vector<std::string> files = edit::MidiFileNames(sounds, {}, "", {});

    // A sound that fails is left out, and the others go on. A sound that plays no notes makes no MIDI file.
    std::size_t written = 0;
    std::size_t silent = 0;
    std::size_t failed = 0;
    for (std::size_t i = 0; i < sounds.size(); i++)
    {
        try
        {
            edit::WriteBatchMidi(source, sounds[i].sound, folder / xsflib::PathFromUtf8(files[i]), options.sfz);
            written++;
        }
        catch (const song::EmptyConversion&)
        {
            silent++;
        }
        catch (const std::exception& error)
        {
            std::fprintf(stderr, "Can't export %s: %s\n", sounds[i].name.c_str(), error.what());
            failed++;
        }
    }

    std::printf("Exported %s. Skipped %s.\n", Counted(written, "sequence sound", "sequence sounds").c_str(),
                Counted(silent, "silent sound", "silent sounds").c_str());

    return failed > 0 ? 1 : 0;
}

} // namespace hokuto::cli::hok_parts
