// SPDX-License-Identifier: MIT

#include "song/dry_render.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>

#include "audio/resampler.h"

namespace hokuto::song
{
namespace
{

// The output of `model` from its first frame, for `seconds` or until the output ends, or until `cancel` is set.
DryRender Render(SequenceModel& model, double seconds, const std::atomic<bool>& cancel)
{
    DryRender render{model.SampleRate(), {}};
    const auto frames = static_cast<uint64_t>(
        std::ceil(std::max(seconds, 0.0) * model.SampleRate() / static_cast<double>(model.FrameSamples())));
    if (frames == 0)
    {
        return render;
    }

    render.samples.reserve(static_cast<std::size_t>(frames) * model.FrameSamples() * 2);
    for (bool plays = model.SeekFrame(0); plays && !cancel.load(std::memory_order_relaxed); plays = model.RunFrame())
    {
        const std::span<const float> output = model.FloatOutput();
        render.samples.insert(render.samples.end(), output.begin(), output.end());
        if (model.Frames() >= frames)
        {
            break;
        }
    }

    return render;
}

} // namespace

// A render runs once from the start and never seeks. A snapshot would only take memory.
DryRender RenderDry(std::shared_ptr<const ntdlib::sar::SoundArchive> archive, std::shared_ptr<const Sequence> sequence,
                    Mixing mixing, double seconds, const std::atomic<bool>& cancel)
{
    SequenceModel model(std::move(archive), std::move(sequence), {}, mixing, std::numeric_limits<uint32_t>::max());
    model.SetBusVolumes({0.0f, 0.0f, 0.0f});

    return Render(model, seconds, cancel);
}

DryRender RenderDry(std::shared_ptr<const ntdlib::rsar::SoundArchive> archive, std::shared_ptr<const Sequence> sequence,
                    double seconds, const std::atomic<bool>& cancel)
{
    SequenceModel model(std::move(archive), std::move(sequence), {}, std::numeric_limits<uint32_t>::max());
    return Render(model, seconds, cancel);
}

DryRender AtRate(const DryRender& render, int rate)
{
    if (render.rate == rate)
    {
        return render;
    }

    return {rate,
            audio::ResampleFrames(render.samples, 2, static_cast<uint32_t>(render.rate), static_cast<uint32_t>(rate))};
}

} // namespace hokuto::song
