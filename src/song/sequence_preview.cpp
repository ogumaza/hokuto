// SPDX-License-Identifier: MIT

#include "song/sequence_preview.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>

namespace hokuto::song
{

SequencePreview::SequencePreview(std::shared_ptr<const ntdlib::sar::SoundArchive> archive,
                                 std::shared_ptr<const Sequence> sequence, AuxEffects effects,
                                 const AuxVolumes& volumes, Mixing mixing)
    : model_(std::move(archive), std::move(sequence), std::move(effects), mixing),
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
    std::size_t done = 0;
    while (done < frames)
    {
        if (frame_read_ == frame_.size() && !NextFrame())
        {
            break;
        }

        const std::size_t count = std::min((frames - done) * 2, frame_.size() - frame_read_);
        std::copy_n(frame_.data() + frame_read_, count, out + done * 2);
        frame_read_ += count;
        done += count / 2;
    }

    return done;
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
    for (std::size_t i = 0; i < frame_.size(); i++)
    {
        const float sample = i < samples.size() ? samples[i] : 0.0f;
        const float through = static_cast<float>(i / 2) / static_cast<float>(frame_.size() / 2);
        frame_[i] = ended_ ? sample * (1.0f - through) : sample;
    }
    frame_read_ = 0;

    return true;
}

} // namespace hokuto::song
