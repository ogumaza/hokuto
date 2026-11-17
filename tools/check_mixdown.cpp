// SPDX-License-Identifier: MIT

// Checks the mixdown (song/mixdown.h) against playback. For each archive, ROM or image, the tool picks up to --count
// sequence sounds, spread evenly over them, and renders each in every mixing mode that the console supports: two passes
// of its loop and a fade of 1 s, or to its end with tails of up to 10 s. A sound that loops renders as a seamless loop
// too. The tool plays the same sound on a SequencePlayer of a new model in the render worker's blocks of 256 frames.
// The program plays it the same way with Cycle on the loop.
// - Every sample of a render up to its fade has to be the player's. The fade has to fall in a straight line over 1 s
//   of the player's samples.
// - The fade after the second pass has to start within two frames and a thousandth of the time at which the
//   arrangement's tempos put the second pass's end. A fade at the limit of 600 s starts there.
// - A sound without a loop has to be the player's output to its end, every sample of it, unless its tails sound on
//   past 10 s and fade.
// - A seamless loop has to be the player's output, every sample, and its loop has to start within the same margin of
//   the first pass's end and end with the render.
// The tool prints a line for each render that fails, and a summary with the time that the renders took against the
// time that they last. A sampler's jump back from a seamless loop's end has to land in step with playback: no place up
// to a model's frame earlier or later can repeat the loop's first frames twice as closely as the loop's end does. The
// summary prints how closely playback repeats them after the end. It returns 1 when a render fails, and 2
// when an input can't be read.
//
// usage: hokuto_check_mixdown [--count n] <input>...

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

#include "xsflib/console.h"
#include "xsflib/files.h"

#include "check_support.h"
#include "checked_archive.h"
#include "format/file.h"
#include "song/mixdown.h"
#include "song/player.h"
#include "song/sequence.h"
#include "song/sequence_model.h"

namespace
{

using namespace hokuto;

// The fade of the check's renders: 1 s.
constexpr double kFadeSeconds = 1.0;

// The counts of the check: the renders, those that failed, the time that the renders took and the audio that they
// contain. `seam` is the largest difference at a seamless loop's jump, in `seam_label`.
struct Counts
{
    std::size_t renders = 0;
    std::size_t failed = 0;
    double rendered_seconds = 0.0;
    double played_seconds = 0.0;
    double seam = 0.0;
    std::string seam_label;
    std::vector<double> seams;
};

// The frames that the check compares after a seamless loop's end.
constexpr std::size_t kSeamFrames = 4;

// How closely playback repeats a seamless loop's first frames after the loop's end: the RMS of the difference over
// kSeamFrames of the model's frames, relative to the RMS of the loop's first frames. `at_end` compares the frames
// after the loop's end, and `shifted` the closest of the places up to a model's frame earlier or later.
struct Seam
{
    double at_end = 0.0;
    double shifted = 0.0;
};

// What plays the sounds of `archive`.
song::Played PlayedOf(const tools::CheckedArchive& archive)
{
    return archive.dse      ? song::Played(archive.dse)
           : archive.ds     ? song::Played(archive.ds)
           : archive.gba    ? song::Played(archive.gba)
           : archive.driver ? song::Played(archive.driver)
           : archive.wii    ? song::Played(archive.wii)
                            : song::Played(archive.sar);
}

// The output of a player of `sequence` on a new model with `options`, from its start, with Cycle on the loop when it
// has one, in blocks of 256 frames, up to `most` stereo frames.
std::vector<float> Played(const song::Played& played, const std::shared_ptr<const song::Sequence>& sequence,
                          const song::ModelOptions& options, std::size_t most)
{
    constexpr std::size_t kBlock = 256;
    const auto model = std::make_shared<song::SequenceModel>(played, sequence, options);
    const auto clock = std::make_shared<song::PlaybackClock>(model->FrameSamples());
    if (sequence->loop && sequence->loop->end > sequence->loop->start)
    {
        clock->SetCycle({.on = true, .start = sequence->loop->start, .end = sequence->loop->end});
    }

    song::SequencePlayer player(model, 0, clock);
    std::vector<float> samples;
    std::vector<float> block(kBlock * 2);
    while (samples.size() / 2 < most)
    {
        const std::size_t done = player.Render(block.data(), kBlock);
        samples.insert(samples.end(), block.begin(), block.begin() + static_cast<std::ptrdiff_t>(done * 2));
        if (done < kBlock)
        {
            break;
        }
    }

    return samples;
}

// The first stereo frame at which `rendered` differs from `played`, or the render's length.
std::size_t FirstDifference(const std::vector<float>& rendered, const std::vector<float>& played)
{
    for (std::size_t i = 0; i < rendered.size(); i++)
    {
        if (i >= played.size() || rendered[i] != played[i])
        {
            return i / 2;
        }
    }

    return rendered.size() / 2;
}

// Why the render of `sequence` isn't what the player writes, or nothing.
std::optional<std::string> MixdownFault(const song::Sequence& sequence, const song::Mixdown& mixdown,
                                        const std::vector<float>& played, std::size_t frame_samples)
{
    const std::vector<float>& rendered = mixdown.samples;
    const std::size_t start = FirstDifference(rendered, played);
    const song::MixdownRange range = song::SongRange(sequence, 2, kFadeSeconds);
    if (!range.fade_at && !mixdown.limited && !mixdown.tails_cut)
    {
        return start == rendered.size() / 2 && rendered.size() == played.size()
                   ? std::nullopt
                   : std::optional<std::string>("differs from playback at frame " + std::to_string(start) + " of " +
                                                std::to_string(played.size() / 2));
    }

    // The fade lasts 1 s, and ends the render. Before it, every sample is the player's. It has to start near the second
    // pass's end, or at the limit, and fall over the player's samples to silence. Cut tails fade over their last 1 s.
    const double rate = mixdown.sample_rate;
    const auto fade = static_cast<std::size_t>(std::llround(kFadeSeconds * rate));
    if (rendered.size() / 2 < fade || played.size() < rendered.size())
    {
        return "lasts " + std::to_string(rendered.size() / 2) + " frames, and playback " +
               std::to_string(played.size() / 2);
    }

    const std::size_t fade_start = rendered.size() / 2 - fade;
    if (start < fade_start)
    {
        return "differs from playback at frame " + std::to_string(start) + ", before the fade at " +
               std::to_string(fade_start);
    }

    // The arrangement's tempos place the second pass's end to within a frame or two, and the drivers' tempos drift from
    // them by up to a thousandth. A render that reaches the limit fades within a frame of it.
    const double expected = mixdown.limited ? song::kMixdownLimitSeconds * 1000.0
                                            : song::MsOfTick(sequence, sequence.loop->end) * 2.0 -
                                                  song::MsOfTick(sequence, sequence.loop->start);
    const double off = static_cast<double>(fade_start) - expected * rate / 1000.0;
    if (!mixdown.tails_cut && std::abs(off) > 2.0 * static_cast<double>(frame_samples) + expected * rate / 1e6)
    {
        return "fades at frame " + std::to_string(fade_start) + ", " + std::to_string(off / rate * 1000.0) +
               " ms from the second pass's end";
    }

    for (std::size_t i = fade_start * 2; i < rendered.size(); i++)
    {
        const auto gain = static_cast<float>(1.0 - static_cast<double>(i / 2 - fade_start) / static_cast<double>(fade));
        if (rendered[i] != played[i] * gain)
        {
            return "the fade differs at frame " + std::to_string(i / 2);
        }
    }

    return std::nullopt;
}

// Why the seamless render of `sequence` isn't what the player writes, or nothing.
std::optional<std::string> SeamlessFault(const song::Sequence& sequence, const song::Mixdown& mixdown,
                                         const std::vector<float>& played, std::size_t frame_samples)
{
    const std::vector<float>& rendered = mixdown.samples;
    const std::size_t start = FirstDifference(rendered, played);
    if (start < rendered.size() / 2)
    {
        return "the seamless loop differs from playback at frame " + std::to_string(start);
    }
    if (!mixdown.loop || mixdown.loop->end != rendered.size() / 2)
    {
        return std::string("the seamless loop has no loop to its end");
    }

    const double rate = mixdown.sample_rate;
    const double pass_end = song::MsOfTick(sequence, sequence.loop->end);
    const double off = static_cast<double>(mixdown.loop->start) - pass_end * rate / 1000.0;
    if (std::abs(off) > 2.0 * static_cast<double>(frame_samples) + pass_end * rate / 1e6)
    {
        return "the seamless loop starts at frame " + std::to_string(mixdown.loop->start) + ", " +
               std::to_string(off / rate * 1000.0) + " ms from the first pass's end";
    }

    return std::nullopt;
}

// The seam of `mixdown`, a seamless render, against `played`, the player's output past the render's end. Nothing when
// the loop is too short, silent at its start, or `played` ends too soon.
std::optional<Seam> SeamOf(const song::Mixdown& mixdown, const std::vector<float>& played, std::size_t frame_samples)
{
    const std::size_t compared = kSeamFrames * frame_samples * 2;
    const std::size_t start = mixdown.loop->start * 2;
    const std::size_t end = mixdown.loop->end * 2;
    if (end < start + compared + frame_samples * 2 || played.size() < end + compared + frame_samples * 2)
    {
        return std::nullopt;
    }

    double signal = 0.0;
    for (std::size_t i = 0; i < compared; i++)
    {
        signal += static_cast<double>(mixdown.samples[start + i]) * mixdown.samples[start + i];
    }
    if (signal <= 0.0)
    {
        return std::nullopt;
    }
    const auto difference = [&](std::size_t after)
    {
        double sum = 0.0;
        for (std::size_t i = 0; i < compared; i++)
        {
            const double d = static_cast<double>(played[after + i]) - mixdown.samples[start + i];
            sum += d * d;
        }
        return std::sqrt(sum / signal);
    };

    Seam seam{.at_end = difference(end), .shifted = std::numeric_limits<double>::max()};
    for (std::size_t place = end - frame_samples * 2; place <= end + frame_samples * 2; place += 2)
    {
        if (place != end)
        {
            seam.shifted = std::min(seam.shifted, difference(place));
        }
    }

    return seam;
}

// Renders sound `sound` of `archive` in `mixing`, and checks the render against playback.
void CheckSound(const tools::CheckedArchive& archive, const song::Played& played, uint32_t sound, song::Mixing mixing,
                const std::string& name, Counts& counts)
{
    const std::string label =
        name + " " + archive.NameOf(sound) + (mixing == song::Mixing::kEnhanced ? " (enhanced)" : "");
    try
    {
        const auto sequence = std::make_shared<const song::Sequence>(archive.Read(sound));
        const song::ModelOptions options{.mixing = mixing};
        const auto model = std::make_shared<song::SequenceModel>(played, sequence, options);
        const std::size_t frame_samples = model->FrameSamples();

        const auto started = std::chrono::steady_clock::now();
        const song::Mixdown mixdown = song::RenderMixdown(model, song::SongRange(*sequence, 2, kFadeSeconds));
        const std::chrono::duration<double> took = std::chrono::steady_clock::now() - started;
        const std::vector<float> heard = Played(played, sequence, options, mixdown.samples.size() / 2 + frame_samples);

        counts.renders++;
        counts.rendered_seconds += took.count();
        counts.played_seconds += static_cast<double>(mixdown.samples.size() / 2) / mixdown.sample_rate;
        if (const std::optional<std::string> fault = MixdownFault(*sequence, mixdown, heard, frame_samples))
        {
            std::printf("FAIL %s: %s\n", label.c_str(), fault->c_str());
            counts.failed++;
        }
        if (!sequence->loop || sequence->loop->end <= sequence->loop->start)
        {
            return;
        }

        const auto seamless_started = std::chrono::steady_clock::now();
        const song::Mixdown seamless = song::RenderMixdown(
            std::make_shared<song::SequenceModel>(played, sequence, options), song::SeamlessRange(*sequence));
        const std::chrono::duration<double> seamless_took = std::chrono::steady_clock::now() - seamless_started;
        const std::vector<float> seamless_heard =
            Played(played, sequence, options, seamless.samples.size() / 2 + frame_samples * (kSeamFrames + 2));

        counts.renders++;
        counts.rendered_seconds += seamless_took.count();
        counts.played_seconds += static_cast<double>(seamless.samples.size() / 2) / seamless.sample_rate;
        if (seamless.limited)
        {
            return;
        }
        if (const std::optional<std::string> fault = SeamlessFault(*sequence, seamless, seamless_heard, frame_samples))
        {
            std::printf("FAIL %s: %s\n", label.c_str(), fault->c_str());
            counts.failed++;
        }
        else if (const std::optional<Seam> seam = SeamOf(seamless, seamless_heard, frame_samples))
        {
            counts.seams.push_back(seam->at_end);
            if (seam->at_end > counts.seam)
            {
                counts.seam = seam->at_end;
                counts.seam_label = label;
            }
            if (seam->at_end > 0.05 && seam->shifted < seam->at_end / 2.0)
            {
                std::printf(
                    "FAIL %s: the jump back from the seamless loop's end lands a frame off: %.3f there, %.3f a "
                    "frame away\n",
                    label.c_str(), seam->at_end, seam->shifted);
                counts.failed++;
            }
        }
    }
    catch (const std::exception& error)
    {
        std::printf("FAIL %s: %s\n", label.c_str(), error.what());
        counts.failed++;
    }
}

} // namespace

int main(int argc, char** argv)
{
    std::vector<std::string> arguments = xsflib::console::Arguments(argc, argv);
    std::size_t count = 4;
    if (arguments.size() >= 3 && arguments[1] == "--count")
    {
        count = std::max<std::size_t>(1, std::stoul(arguments[2]));
        arguments.erase(arguments.begin() + 1, arguments.begin() + 3);
    }
    if (arguments.size() < 2)
    {
        std::fputs("usage: hokuto_check_mixdown [--count n] <input>...\n", stderr);
        return 2;
    }

    Counts counts;
    for (std::size_t i = 1; i < arguments.size(); i++)
    {
        const std::filesystem::path path = xsflib::PathFromUtf8(arguments[i]);
        const std::string name = xsflib::Utf8(path.filename());
        tools::CheckedArchive archive;
        try
        {
            std::vector<uint8_t> bytes;
            if (!xsflib::ReadFile(path, bytes))
            {
                throw std::runtime_error("can't read the file");
            }
            archive = tools::OpenChecked(path, bytes);
        }
        catch (const std::exception& error)
        {
            std::fprintf(stderr, "%s: %s\n", name.c_str(), error.what());
            return 2;
        }

        // The sequence sounds, spread evenly, in each mode that the console supports.
        std::vector<uint32_t> sounds;
        for (uint32_t sound = 0; sound < archive.SoundCount(); sound++)
        {
            if (archive.IsSequence(sound))
            {
                sounds.push_back(sound);
            }
        }

        const song::Played played = PlayedOf(archive);
        const bool enhanced = !format::MixingRefused(played.Engine(), format::Mixing::kEnhanced);
        for (const std::size_t index : tools::SpreadIndices(sounds.size(), count))
        {
            CheckSound(archive, played, sounds[index], song::Mixing::kStrict, name, counts);
            if (enhanced)
            {
                CheckSound(archive, played, sounds[index], song::Mixing::kEnhanced, name, counts);
            }
        }
    }

    std::printf("%zu renders, %zu failed. They hold %.1f s of audio and took %.1f s: %.0f times real time.\n",
                counts.renders, counts.failed, counts.played_seconds, counts.rendered_seconds,
                counts.rendered_seconds > 0.0 ? counts.played_seconds / counts.rendered_seconds : 0.0);
    if (!counts.seams.empty())
    {
        std::ranges::sort(counts.seams);
        std::printf(
            "Seamless loops' jumps, the RMS of the difference from the loop's start, relative: median %.4f, "
            "largest %.4f in %s.\n",
            counts.seams[counts.seams.size() / 2], counts.seam, counts.seam_label.c_str());
    }

    return counts.failed > 0 ? 1 : 0;
}
