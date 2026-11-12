// SPDX-License-Identifier: MIT

// The steps that the playback checks of the GBA, the DS, DSE and the GBA's other drivers share.

#include "check_playback_steps.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>

#include "song/catch_up.h"

namespace hokuto::tools::playback_steps
{

std::size_t CheckFrames(const song::SequenceModel& model, int seconds)
{
    return static_cast<std::size_t>(seconds) * static_cast<std::size_t>(model.SampleRate()) / model.FrameSamples();
}

bool Render(song::SequencePlayer& player, std::size_t frames, std::size_t frame_samples, std::vector<float>& output)
{
    std::vector<float> block(kBlock * 2);
    const std::size_t total = frames * frame_samples;
    for (std::size_t done = 0; done < total;)
    {
        const std::size_t wanted = std::min(kBlock, total - done);
        const std::size_t rendered = player.Render(block.data(), wanted);
        output.insert(output.end(), block.begin(), block.begin() + static_cast<std::ptrdiff_t>(rendered * 2));
        done += rendered;
        if (rendered < wanted)
        {
            return false;
        }
    }

    return true;
}

std::size_t Differences(std::span<const float> output, std::size_t from, std::span<const float> reference,
                        std::size_t frame_floats, std::size_t offset)
{
    std::size_t differences = 0;
    for (std::size_t frame = from; (frame + 1) * frame_floats <= output.size(); frame++)
    {
        const std::size_t at = offset + frame * frame_floats;
        if (at + frame_floats > reference.size() ||
            !std::equal(output.begin() + static_cast<std::ptrdiff_t>(frame * frame_floats),
                        output.begin() + static_cast<std::ptrdiff_t>((frame + 1) * frame_floats),
                        reference.begin() + static_cast<std::ptrdiff_t>(at)))
        {
            differences++;
        }
    }

    return differences;
}

StartResult StartCheck(const std::shared_ptr<song::SequenceModel>& model, std::size_t frames,
                       std::span<const float> reference, std::size_t offset)
{
    const std::size_t frame_samples = model->FrameSamples();
    const std::size_t frame_floats = frame_samples * 2;
    const auto clock = std::make_shared<song::PlaybackClock>(frame_samples);
    song::SequencePlayer player(model, 0, clock);
    std::vector<float> output;
    StartResult result;
    result.ended = !Render(player, frames, frame_samples, output);
    result.heard = output.size() / frame_floats;
    output.resize(result.heard > 1 ? (result.heard - 1) * frame_floats : 0);
    result.differences = Differences(output, 1, reference, frame_floats, offset);

    return result;
}

SeekResult SeekCheck(song::SequenceModel& model, std::size_t start, std::size_t heard, std::span<const float> reference,
                     std::size_t frame_floats, StepTotals& totals)
{
    SeekResult result;
    for (const std::size_t frame : {start + heard / 2, start + 1})
    {
        if (frame >= start + heard)
        {
            continue;
        }

        totals.seeks++;
        if (!model.SeekFrame(frame))
        {
            result.failed++;
            continue;
        }

        const std::span<const float> sought = model.FloatOutput();
        result.differences += Differences(sought, 0, reference, frame_floats, frame * frame_floats);
    }

    return result;
}

std::vector<float> RunSwitch(const song::Played& played, const std::shared_ptr<const song::Sequence>& sequence,
                             song::Mixing mixing, std::size_t before, std::size_t frames)
{
    const auto model = std::make_shared<song::SequenceModel>(played, sequence, song::ModelOptions{.mixing = mixing});
    const std::size_t frame_samples = model->FrameSamples();
    const auto clock = std::make_shared<song::PlaybackClock>(frame_samples);
    song::SequencePlayer player(model, 0, clock);
    std::vector<float> output;
    if (!Render(player, before, frame_samples, output))
    {
        return output;
    }

    song::ModelCatchUp catch_up;
    catch_up.Start(std::make_shared<song::SequenceModel>(played, sequence, song::ModelOptions{.mixing = mixing}),
                   clock);
    while (!catch_up.Finished())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    clock->Offer(catch_up.Take());
    Render(player, frames - before, frame_samples, output);

    return output;
}

std::string SwitchCheck(const song::Played& played, const std::shared_ptr<const song::Sequence>& sequence,
                        song::Mixing mixing, std::span<const float> reference, std::size_t heard, std::size_t frames,
                        std::size_t frame_floats, std::size_t offset, StepTotals& totals)
{
    std::string switch_text;
    const std::size_t before = heard / 2;
    if (before > 1)
    {
        std::vector<float> switched = RunSwitch(played, sequence, mixing, before, frames);
        const std::size_t switched_frames = switched.size() / frame_floats;
        switched.resize(switched_frames > 1 ? (switched_frames - 1) * frame_floats : 0);
        const std::size_t switch_differences = Differences(switched, before + 1, reference, frame_floats, offset);
        const bool ends_elsewhere = switched_frames != heard;
        totals.switches++;
        totals.exact += switch_differences == 0 && !ends_elsewhere ? 1 : 0;
        if (switch_differences > 0)
        {
            switch_text += ", " + std::to_string(switch_differences) + " after a switch halfway";
        }
        if (ends_elsewhere)
        {
            switch_text += ", and the output after a switch ends at frame " + std::to_string(switched_frames);
        }
    }

    return switch_text;
}

EndLevels LevelsAfterEnd(std::span<const float> after, std::size_t frame_floats, float span)
{
    EndLevels levels;
    for (std::size_t side = 0; side < 2; side++)
    {
        float low = after[side];
        float high = after[side];
        for (std::size_t frame = 0; frame * frame_floats < after.size(); frame++)
        {
            float frame_low = after[frame * frame_floats + side];
            float frame_high = frame_low;
            for (std::size_t i = frame * frame_floats + side; i < std::min(after.size(), (frame + 1) * frame_floats);
                 i += 2)
            {
                frame_low = std::min(frame_low, after[i]);
                frame_high = std::max(frame_high, after[i]);
            }
            if (frame_high - frame_low > levels.widest)
            {
                levels.widest = frame_high - frame_low;
                levels.widest_at = frame;
            }
            low = std::min(low, frame_low);
            high = std::max(high, frame_high);
        }
        levels.sounds_on = levels.sounds_on || high - low > span;
        levels.range = std::max(levels.range, high - low);
        levels.level = std::max({levels.level, std::abs(low), std::abs(high)});
    }

    return levels;
}

bool ArrangedAt(const song::Sequence& sequence, int track, int key, uint32_t tick,
                std::vector<song::Note> song::Track::* list)
{
    const uint32_t arranged_tick = song::PositionOfTicks(sequence, tick);
    for (const song::Track& arranged : sequence.tracks)
    {
        if (arranged.number != track)
        {
            continue;
        }

        for (const song::Note& note : arranged.*list)
        {
            if (note.key == key && note.start == arranged_tick)
            {
                return true;
            }
        }
    }

    return false;
}

std::string FailedSeeksText(std::size_t failed)
{
    if (failed == 0)
    {
        return "";
    }

    return ", " + std::to_string(failed) + (failed == 1 ? " seek gives" : " seeks give") + " no output";
}

void PrintDifferences(const std::string& name, std::size_t differences, std::size_t heard, std::size_t seeks,
                      const std::string& after_text, const std::string& switch_text, const std::string& loop)
{
    std::printf("%s: %zu of %zu frames differ from the player, %zu after seeks%s%s (loop %s).\n", name.c_str(),
                differences, heard, seeks, after_text.c_str(), switch_text.c_str(), loop.c_str());
}

} // namespace hokuto::tools::playback_steps
