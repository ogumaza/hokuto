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
// 4.889 ms.
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

// The silence after the last sound of a sound that ends, and how long past its length the tool listens.
constexpr double kTailMs = 500.0;
constexpr double kPastEndMs = 10000.0;

// How long the tool listens to a silent sound, and the limit of the length analysis.
constexpr double kSilentMs = 190000.0;
constexpr double kLimitMs = 600000.0;

// The seconds since `start`.
double SecondsSince(std::chrono::steady_clock::time_point start)
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
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

// Plays `model` from its first frame for `frames` frames, or until its output ends.
Played Play(song::SequenceModel& model, uint64_t frames, uint64_t from, std::optional<double> ticks)
{
    Played played;
    played.started = model.SeekFrame(0);
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

} // namespace

int main(int argc, char** argv)
{
    const std::vector<std::string> arguments = xsflib::console::Arguments(argc, argv);
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

    if (positional.size() != 1)
    {
        std::fprintf(stderr, "usage: hokuto_check_lengths <file.hok> [--part k/n]\n");
        return 2;
    }

    // The archive that the file builds: for the Wii, read for playback, and otherwise with the group files that the
    // file builds beside it.
    const std::filesystem::path path = xsflib::PathFromUtf8(positional[0]);
    const std::string name = xsflib::Utf8(path.filename());
    format::File file;
    std::shared_ptr<const ntdlib::rsar::SoundArchive> wii;
    std::shared_ptr<const ntdlib::sar::SoundArchive> archive;
    std::optional<ntdlib::SndBuild> build;
    try
    {
        std::vector<uint8_t> bytes;
        if (!xsflib::ReadFile(path, bytes))
        {
            throw std::runtime_error("can't read the file");
        }
        file = format::ReadEitherForm(bytes);
        if (!file.native)
        {
            throw std::runtime_error("the file holds no native archive");
        }
        if (file.native->wii)
        {
            wii = song::LoadWiiArchive(format::ExportArchive(file), name, ntdlib::rvl::snd::kRsarOptions);
            build = format::BuildOfWiiArchive(*file.native);
        }
        else if (file.native->flavor == ntdlib::sar::Flavor::kCtr)
        {
            throw std::runtime_error("the tool doesn't check the 3DS's sounds");
        }
        else
        {
            archive = song::LoadArchive(format::ExportArchive(file), name, format::ExportGroupFiles(file));
        }
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "%s: %s\n", name.c_str(), error.what());
        return 2;
    }

    const song::AuxEffects effects = hokuto::edit::ModelEffects(file.aux_effects);
    const double frame_ms = wii ? 3.0 : (file.native->flavor == ntdlib::sar::Flavor::kCafe ? 3.0 : 5.0);
    const auto frame_at = [frame_ms](double ms)
    {
        return static_cast<uint64_t>(std::ceil(ms / frame_ms));
    };

    Counts counts;
    const auto start = std::chrono::steady_clock::now();
    std::size_t index = 0;
    for (const auto& [sound, length] : file.lengths)
    {
        if (index++ % parts != part - 1)
        {
            continue;
        }

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
            continue;
        }
        std::unique_ptr<song::SequenceModel> model =
            wii ? std::make_unique<song::SequenceModel>(wii, sequence, effects, std::numeric_limits<uint32_t>::max())
                : std::make_unique<song::SequenceModel>(archive, sequence, effects, song::Mixing::kStrict,
                                                        std::numeric_limits<uint32_t>::max());
        model->SetBusVolumes(file.aux_volumes);

        const auto ms_of = [frame_ms](uint64_t frames)
        {
            return static_cast<double>(frames) * frame_ms;
        };
        try
        {
            if (length.silent)
            {
                counts.silent++;
                const Played played = Play(*model, frame_at(kSilentMs), 0, std::nullopt);
                if (played.last_sound)
                {
                    fail("silent, but frame " + std::to_string(*played.last_sound) + " has sound");
                }
                continue;
            }

            if (length.fade == 0)
            {
                // The last sound's frame ends at the length less the tail.
                counts.ends++;
                const Played played =
                    Play(*model, frame_at(static_cast<double>(length.length) + kPastEndMs), 0, std::nullopt);
                const double expected = static_cast<double>(length.length) - kTailMs;
                if (!played.started || !played.last_sound)
                {
                    fail("ends at " + std::to_string(length.length) + " ms, but the model plays no sound");
                    continue;
                }

                // The idle frames that the analysis waits for before a sound ends.
                const double idle_ms = static_cast<double>(xsflib::rip::LengthRules{}.idle_frames + 1) * frame_ms;
                const double last_sound_end = ms_of(*played.last_sound + 1);
                if (last_sound_end < expected - frame_ms || last_sound_end > expected + idle_ms)
                {
                    fail("ends at " + std::to_string(length.length) + " ms, but the last sound's frame ends at " +
                         std::to_string(last_sound_end) + " ms");
                }
                else if (last_sound_end > expected + frame_ms)
                {
                    counts.idle_ends++;
                    counts.idle_ms = std::max(counts.idle_ms, last_sound_end - expected);
                }
                continue;
            }

            if (static_cast<double>(length.length) >= kLimitMs)
            {
                // The model still plays at the limit.
                counts.limits++;
                const Played played = Play(*model, frame_at(kLimitMs + length.fade), frame_at(kLimitMs), std::nullopt);
                if (played.ended || !played.sound_from)
                {
                    fail(std::string("plays past the limit, but the model ") +
                         (played.ended ? "ends at " + std::to_string(ms_of(played.frames)) + " ms"
                                       : "has no sound in the fade"));
                }
                continue;
            }

            // The model plays on through the fade, with sound.
            counts.loops++;
            std::optional<double> second_pass;
            if (length.loop)
            {
                second_pass = static_cast<double>(length.loop->end) +
                              (static_cast<double>(length.loop->end) - static_cast<double>(length.loop->start));
            }
            const uint64_t fade_end = frame_at(static_cast<double>(length.length) + length.fade);
            const Played played = Play(*model, fade_end, frame_at(length.length), second_pass);
            if (!played.started || played.ended || !played.sound_from)
            {
                fail("loops, but the model " + (played.ended ? "ends at " + std::to_string(ms_of(played.frames)) + " ms"
                                                             : std::string("has no sound in the fade")));
                continue;
            }

            // The analysis counts a loop from the frame after the one that runs it. A second pass that the fade's end
            // doesn't reach ends later than the length.
            const double length_ms = static_cast<double>(length.length);
            const double at = played.reached ? ms_of(*played.reached + 1) : std::numeric_limits<double>::infinity();
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
                    std::printf("%s: %s: loops at %u ms, before the arrangement's second pass ends at %.0f ms\n",
                                name.c_str(), sound_name.c_str(), length.length, at);
                }
                else
                {
                    std::printf(
                        "%s: %s: loops at %u ms, before the arrangement's second pass. The pass ends after the fade's "
                        "end, %.0f ms\n",
                        name.c_str(), sound_name.c_str(), length.length, ms_of(fade_end));
                }
            }
        }
        catch (const std::exception& error)
        {
            fail(std::string("the model fails: ") + error.what());
        }
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
