// SPDX-License-Identifier: MIT

#include "song/sequence_preview.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>

#include "song/preview_parts.h"

namespace hokuto::song
{

SequencePreview::SequencePreview(Played played, std::shared_ptr<const Sequence> sequence, ModelOptions options,
                                 const AuxVolumes& volumes)
    : model_(std::move(played), std::move(sequence), std::move(options)),
      frame_(model_.FrameSamples() * 2),
      frame_read_(frame_.size())
{
    model_.SetBusVolumes(volumes);
}

uint64_t SequencePreview::LongestFrames() const
{
    return kLongestMs * static_cast<uint64_t>(model_.SampleRate()) / (1000 * model_.FrameSamples());
}

int SequencePreview::SampleRate() const
{
    return model_.SampleRate();
}

std::size_t SequencePreview::Render(float* out, std::size_t frames)
{
    return preview_parts::RenderFrames(out, frames, frame_, frame_read_, [this] { return NextFrame(); });
}

bool SequencePreview::NextFrame()
{
    // The first frame starts the model.
    const bool ran = !ended_ && (started_ ? model_.RunFrame() : model_.SeekFrame(0));
    started_ = true;
    if (!ran)
    {
        ended_ = true;
        return false;
    }

    frames_++;

    // The last frame of a preview that runs to the limit fades out.
    const std::span<const float> samples = model_.FloatOutput();
    ended_ = frames_ == LongestFrames();
    const std::size_t copied = std::min(samples.size(), frame_.size());
    std::copy_n(samples.begin(), copied, frame_.begin());
    std::fill(frame_.begin() + static_cast<std::ptrdiff_t>(copied), frame_.end(), 0.0f);
    if (ended_)
    {
        preview_parts::FadeOut(frame_);
    }

    frame_read_ = 0;

    return true;
}

} // namespace hokuto::song
