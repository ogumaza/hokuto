// SPDX-License-Identifier: MIT

// Checks the lengths that a .hok file keeps for players (the LNTH chunk that hok lengths writes) against what the
// program's model plays past each length. The tool builds the file's archive as the program does, and plays each sound
// with lengths on song::SequenceModel from its first frame, with the file's effects at the buses' volumes. A frame has
// sound when one of its samples is louder than ±8, as in the length analysis (xsflib::rip::Audible).
//
// A sound that ends has its last sound in the frame that ends 0.5 s before its length, within a frame, or in one of the
// idle frames after it. The analysis ends a sound at its first idle frame when the output still sounds there
// (xsflib::rip::LengthRules::end_at_idle_start). On the Wii, AX's depop fades out a stopped voice's last samples over
// the next frames. The tool counts such ends. No frame has sound for 10 s past its length. A sound that loops plays on
// through its fade: the model doesn't end before the fade ends, and a frame of the fade has sound. Where the lengths
// give the arrangement's loop, the tool also counts the lengths that come at the end of the loop's second pass, within
// two frames, and the lengths that come later or sooner. Other tracks that loop at other lengths put the fade later
// (xsflib::rip::MeasureLength). A sound that plays on past the limit still plays at 600 s. A silent sound has no frame
// with sound for 190 s, or until the model ends.
//
// The tool prints a line for each sound that fails, with what it found, and for each loop that comes before the
// arrangement's second pass ends. It ends with a summary. It returns 1 when a sound fails, and 2 when the file can't be
// read. With --part k/n, it checks every nth sound with lengths from the kth, for runs side by side. The tool leaves
// out the 3DS's sounds. Its measures of frames hold only frames of 3 ms and 5 ms, and a frame of the 3DS lasts about
// 4.889 ms. It refuses a file of the GBA, the DS or DSE too.
//
// usage: hokuto_check_lengths <file.hok> [--part k/n]

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
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "ntdlib/rsar/formats.h"
#include "ntdlib/rvl/snd/rsar_options.h"
#include "ntdlib/sar/formats.h"
#include "xsflib/console.h"
#include "xsflib/files.h"
#include "xsflib/rip/length.h"

#include "check_part.h"
#include "edit/effects.h"
#include "format/archive.h"
#include "format/file.h"
#include "format/text.h"
#include "song/archive.h"
#include "song/sequence.h"
#include "song/sequence_model.h"

namespace
{

namespace format = hokuto::format;
namespace song = hokuto::song;
using hokuto::tools::Part;
using hokuto::tools::PartOf;

// The silence after the last sound of a sound that ends, and how long past its length the tool listens.
constexpr double kTailMs = xsflib::rip::LengthRules{}.tail_ms;
constexpr double kPastEndMs = 10000.0;

// How long the tool listens to a silent sound, and the limit of the length analysis.
constexpr double kSilentMs = 190000.0;
constexpr double kLimitMs = xsflib::rip::LengthRules{}.max_ms;

// The seconds since `start`.
double SecondsSince(std::chrono::steady_clock::time_point start)
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

// What the model played of a sound: the frames that it ran, whether its output ended within them, the last frame with
// sound, the first frame with sound from frame `from` on, and the first frame whose position reached tick `ticks`.
struct Played
{
    bool started = false;
    uint64_t frames = 0;
    bool ended = false;
    std::optional<uint64_t> last_sound;
    std::optional<uint64_t> sound_from;
    std::optional<uint64_t> reached;
};

// Plays `model` from its first frame for `frames` frames, or until its output ends. Throws std::runtime_error when the
// model fails: its frames then end as an output that ends would.
Played Play(song::SequenceModel& model, uint64_t frames, uint64_t from, std::optional<double> ticks)
{
    Played played;
    played.started = model.SeekFrame(0);
    if (!played.started && !model.Failure().empty())
    {
        throw std::runtime_error(model.Failure());
    }
    if (!played.started)
    {
        return played;
    }

    for (;;)
    {
        const uint64_t frame = model.Frames() - 1;
        played.frames = frame + 1;
        if (xsflib::rip::Audible(model.Output()))
        {
            played.last_sound = frame;
            if (frame >= from && !played.sound_from)
            {
                played.sound_from = frame;
            }
        }
        if (ticks && !played.reached && model.After() >= *ticks)
        {
            played.reached = frame;
        }
        if (played.frames >= frames)
        {
            break;
        }
        if (!model.RunFrame())
        {
            if (!model.Failure().empty())
            {
                throw std::runtime_error(model.Failure());
            }

            played.ended = true;
            break;
        }
    }

    return played;
}

// The counts of the run.
struct Counts
{
    std::size_t sounds = 0;
    std::size_t ends = 0;
    std::size_t loops = 0;
    std::size_t limits = 0;
    std::size_t silent = 0;
    std::size_t failed = 0;
    std::size_t idle_ends = 0;
    double idle_ms = 0.0;
    std::size_t at_second_pass = 0;
    std::size_t after_second_pass = 0;
    std::size_t before_second_pass = 0;
    std::size_t without_ticks = 0;
};

// The frame of `frame_ms` milliseconds in which `ms` milliseconds end.
uint64_t FrameAt(double ms, double frame_ms)
{
    return static_cast<uint64_t>(std::ceil(ms / frame_ms));
}

// The milliseconds of `frames` frames of `frame_ms` milliseconds.
double MsOf(uint64_t frames, double frame_ms)
{
    return static_cast<double>(frames) * frame_ms;
}

// Plays `model`, a sound whose length says that it stays silent. Returns what's wrong where a frame has sound, or
// nothing.
std::optional<std::string> SilentFailure(song::SequenceModel& model, double frame_ms)
{
    const Played played = Play(model, FrameAt(kSilentMs, frame_ms), 0, std::nullopt);
    if (played.last_sound)
    {
        return "silent, but frame " + std::to_string(*played.last_sound) + " has sound";
    }

    return std::nullopt;
}

// Plays `model`, a sound that `length` describes as one that ends: the last sound's frame ends at the length less the
// tail. Returns what's wrong where it doesn't, or nothing. Counts a sound that ends at its first idle frame in
// `counts`.
std::optional<std::string> EndFailure(song::SequenceModel& model, const format::SoundLength& length, double frame_ms,
                                      Counts& counts)
{
    const Played played =
        Play(model, FrameAt(static_cast<double>(length.length) + kPastEndMs, frame_ms), 0, std::nullopt);
    const double expected = static_cast<double>(length.length) - kTailMs;
    if (!played.started || !played.last_sound)
    {
        return "ends at " + std::to_string(length.length) + " ms, but the model plays no sound";
    }

    // The idle frames that the analysis waits for before a sound ends.
    const double idle_ms = static_cast<double>(xsflib::rip::LengthRules{}.idle_frames + 1) * frame_ms;
    const double last_sound_end = MsOf(*played.last_sound + 1, frame_ms);
    if (last_sound_end < expected - frame_ms || last_sound_end > expected + idle_ms)
    {
        return "ends at " + std::to_string(length.length) + " ms, but the last sound's frame ends at " +
               std::to_string(last_sound_end) + " ms";
    }
    if (last_sound_end > expected + frame_ms)
    {
        counts.idle_ends++;
        counts.idle_ms = std::max(counts.idle_ms, last_sound_end - expected);
    }

    return std::nullopt;
}

// Plays `model`, a sound that `length` describes as one that plays past the limit: the model still plays at the limit.
// Returns what's wrong where it doesn't, or nothing.
std::optional<std::string> LimitFailure(song::SequenceModel& model, const format::SoundLength& length, double frame_ms)
{
    const Played played =
        Play(model, FrameAt(kLimitMs + length.fade, frame_ms), FrameAt(kLimitMs, frame_ms), std::nullopt);
    if (played.ended || !played.sound_from)
    {
        return std::string("plays past the limit, but the model ") +
               (played.ended ? "ends at " + std::to_string(MsOf(played.frames, frame_ms)) + " ms"
                             : "has no sound in the fade");
    }

    return std::nullopt;
}

// Plays `model`, a sound that `length` describes as one that loops: the model plays on through the fade, with sound.
// Returns what's wrong where it doesn't, or nothing. Counts where the loop comes against the arrangement's second pass
// in `counts`, and prints a line under `name` and `sound_name` for a loop that comes sooner.
std::optional<std::string> LoopFailure(song::SequenceModel& model, const format::SoundLength& length, double frame_ms,
                                       const std::string& name, const std::string& sound_name, Counts& counts)
{
    std::optional<double> second_pass;
    if (length.loop)
    {
        second_pass = static_cast<double>(length.loop->end) +
                      (static_cast<double>(length.loop->end) - static_cast<double>(length.loop->start));
    }
    const uint64_t fade_end = FrameAt(static_cast<double>(length.length) + length.fade, frame_ms);
    const Played played = Play(model, fade_end, FrameAt(length.length, frame_ms), second_pass);
    if (!played.started || played.ended || !played.sound_from)
    {
        return "loops, but the model " + (played.ended
                                              ? "ends at " + std::to_string(MsOf(played.frames, frame_ms)) + " ms"
                                              : std::string("has no sound in the fade"));
    }

    // The analysis counts a loop from the frame after the one that runs it. A second pass that the fade's end
    // doesn't reach ends later than the length.
    const double length_ms = static_cast<double>(length.length);
    const double at = played.reached ? MsOf(*played.reached + 1, frame_ms) : std::numeric_limits<double>::infinity();
    if (!second_pass)
    {
        counts.without_ticks++;
    }
    else if (std::abs(at - length_ms) <= 2.0 * frame_ms)
    {
        counts.at_second_pass++;
    }
    else if (length_ms > at)
    {
        counts.after_second_pass++;
    }
    else
    {
        counts.before_second_pass++;
        if (played.reached)
        {
            std::printf("%s: %s: loops at %u ms, before the arrangement's second pass ends at %.0f ms\n", name.c_str(),
                        sound_name.c_str(), length.length, at);
        }
        else
        {
            std::printf(
                "%s: %s: loops at %u ms, before the arrangement's second pass. The pass ends after the fade's "
                "end, %.0f ms\n",
                name.c_str(), sound_name.c_str(), length.length, MsOf(fade_end, frame_ms));
        }
    }

    return std::nullopt;
}

// Plays sound `sound` of the archive that `file` builds, `wii` on the Wii and `archive` elsewhere, and checks that it
// ends, loops or stays silent as `length` says. `build` is the Wii's build of NW4R SND, `effects` the file's effects on
// the aux buses, and `frame_ms` the length of a frame. `name` is the file's name. Counts the sound in `counts`.
void CheckSoundLength(const format::File& file, const std::shared_ptr<const ntdlib::rsar::SoundArchive>& wii,
                      const std::shared_ptr<const ntdlib::sar::SoundArchive>& archive,
                      const std::optional<ntdlib::SndBuild>& build, const song::AuxEffects& effects, double frame_ms,
                      const std::string& name, uint32_t sound, const format::SoundLength& length, Counts& counts)
{
    counts.sounds++;
    const std::string sound_name = wii ? (sound < wii->Sounds().size() ? wii->Sounds()[sound].name : "")
                                       : (sound < archive->Sounds().size() ? archive->Sounds()[sound].name : "");
    const auto fail = [&](const std::string& what)
    {
        std::printf("%s: %s: %s\n", name.c_str(), sound_name.c_str(), what.c_str());
        counts.failed++;
    };

    std::shared_ptr<const song::Sequence> sequence;
    try
    {
        sequence = std::make_shared<const song::Sequence>(wii ? song::ReadSequence(*wii, sound, {}, {}, build)
                                                              : song::ReadSequence(*archive, sound));
    }
    catch (const std::exception& error)
    {
        fail(std::string("the performer can't read the sound: ") + error.what());
        return;
    }
    std::unique_ptr<song::SequenceModel> model =
        wii ? std::make_unique<song::SequenceModel>(
                  wii, sequence,
                  song::ModelOptions{.effects = effects, .snapshot_interval = std::numeric_limits<uint32_t>::max()})
            : std::make_unique<song::SequenceModel>(
                  archive, sequence,
                  song::ModelOptions{.effects = effects,
                                     .mixing = song::Mixing::kStrict,
                                     .snapshot_interval = std::numeric_limits<uint32_t>::max()});
    model->SetBusVolumes(file.aux_volumes);

    try
    {
        std::optional<std::string> failure;
        if (length.silent)
        {
            counts.silent++;
            failure = SilentFailure(*model, frame_ms);
        }
        else if (length.fade == 0)
        {
            counts.ends++;
            failure = EndFailure(*model, length, frame_ms, counts);
        }
        else if (static_cast<double>(length.length) >= kLimitMs)
        {
            counts.limits++;
            failure = LimitFailure(*model, length, frame_ms);
        }
        else
        {
            counts.loops++;
            failure = LoopFailure(*model, length, frame_ms, name, sound_name, counts);
        }
        if (failure)
        {
            fail(*failure);
        }
    }
    catch (const std::exception& error)
    {
        fail(std::string("the model fails: ") + error.what());
    }
}

// The options of the command line: the part of the sounds that the run checks, and the file.
struct Options
{
    std::size_t part = 1;
    std::size_t parts = 1;
    std::filesystem::path path;
};

// The options of the command line's `arguments`, or nothing after the usage.
std::optional<Options> ParseArguments(const std::vector<std::string>& arguments)
{
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

    if (positional.size() != 1)
    {
        std::fprintf(stderr, "usage: hokuto_check_lengths <file.hok> [--part k/n]\n");
        return std::nullopt;
    }

    return Options{part, parts, xsflib::PathFromUtf8(positional[0])};
}

// The file of a run, and the archive that it builds: for the Wii, read for playback, with the game's build of NW4R
// SND, and otherwise with the group files that the file builds beside it.
struct LengthsFile
{
    format::File file;
    std::shared_ptr<const ntdlib::rsar::SoundArchive> wii;
    std::shared_ptr<const ntdlib::sar::SoundArchive> archive;
    std::optional<ntdlib::SndBuild> build;
};

// Reads the file at `path`, `name`, and builds its archive. Throws where the file can't be read, or where the tool
// doesn't check its console.
LengthsFile OpenLengthsFile(const std::filesystem::path& path, const std::string& name)
{
    LengthsFile opened;
    std::vector<uint8_t> bytes;
    if (!xsflib::ReadFile(path, bytes))
    {
        throw std::runtime_error("can't read the file");
    }
    opened.file = format::ReadEitherForm(bytes);
    if (!opened.file.native)
    {
        throw std::runtime_error("the file holds no native archive");
    }
    if (opened.file.native->engine == format::Engine::kWii)
    {
        opened.wii = song::LoadWiiArchive(format::ExportArchive(opened.file), name, ntdlib::rvl::snd::kRsarOptions);
        opened.build = format::BuildOfWiiArchive(*opened.file.native);
    }
    else if (FlavorOf(*opened.file.native) == ntdlib::sar::Flavor::kCtr)
    {
        throw std::runtime_error("the tool doesn't check the 3DS's sounds");
    }
    else if (opened.file.native->engine == format::Engine::kGba || opened.file.native->engine == format::Engine::kDs ||
             opened.file.native->engine == format::Engine::kDse || opened.file.native->driver)
    {
        throw std::runtime_error("the tool checks only the sounds of the Switch, the Wii U and the Wii");
    }
    else
    {
        opened.archive =
            song::LoadArchive(format::ExportArchive(opened.file), name, format::ExportGroupFiles(opened.file));
    }

    return opened;
}

} // namespace

int main(int argc, char** argv)
{
    const std::optional<Options> options = ParseArguments(xsflib::console::Arguments(argc, argv));
    if (!options)
    {
        return 2;
    }
    const auto& [part, parts, path] = *options;

    // The archive that the file builds: for the Wii, read for playback, and otherwise with the group files that the
    // file builds beside it.
    const std::string name = xsflib::Utf8(path.filename());
    LengthsFile opened;
    try
    {
        opened = OpenLengthsFile(path, name);
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "%s: %s\n", name.c_str(), error.what());
        return 2;
    }
    const auto& [file, wii, archive, build] = opened;

    const song::AuxEffects effects = hokuto::edit::ModelEffects(file.aux_effects);
    const double frame_ms = wii ? 3.0 : (FlavorOf(*file.native) == ntdlib::sar::Flavor::kCafe ? 3.0 : 5.0);

    Counts counts;
    const auto start = std::chrono::steady_clock::now();
    std::size_t index = 0;
    for (const auto& [sound, length] : file.lengths)
    {
        if (index++ % parts != part - 1)
        {
            continue;
        }

        CheckSoundLength(file, wii, archive, build, effects, frame_ms, name, sound, length, counts);
    }

    std::printf(
        "%s: %zu sounds with lengths, part %zu of %zu. %zu end, %zu loop, %zu play past the limit and %zu are silent. "
        "%zu failed. %zu end at their first idle frame, up to %.0f ms before their last sound's frame ends. Of the "
        "loops with the arrangement's ticks, %zu come at the end of its second pass, %zu later and %zu sooner. %zu "
        "loop without ticks. The run took %.1f s.\n",
        name.c_str(), counts.sounds, part, parts, counts.ends, counts.loops, counts.limits, counts.silent,
        counts.failed, counts.idle_ends, counts.idle_ms, counts.at_second_pass, counts.after_second_pass,
        counts.before_second_pass, counts.without_ticks, SecondsSince(start));

    return counts.failed > 0 ? 1 : 0;
}
