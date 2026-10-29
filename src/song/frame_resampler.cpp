// SPDX-License-Identifier: MIT

#include "song/frame_resampler.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <utility>

#include "ntdlib/wave/sinc.h"

#include "song/ctr_model.h"

namespace hokuto::song
{

FrameResampler::FrameResampler(std::size_t from, std::size_t to)
    : from_(from),
      to_(to),
      kernel_(&ntdlib::wave::SincKernel::ForStep(static_cast<double>(from) / static_cast<double>(to == 0 ? 1 : to)))
{
    if (from == 0 || to == 0)
    {
        throw std::invalid_argument("A frame for the resampler holds no samples.");
    }

    for (std::vector<float>& channel : input_)
    {
        channel.assign(History() + from_, 0.0f);
    }
}

std::size_t FrameResampler::Delay() const
{
    return static_cast<std::size_t>(kernel_->HalfWidth()) + 1;
}

std::size_t FrameResampler::History() const
{
    return 2 * static_cast<std::size_t>(kernel_->HalfWidth());
}

void FrameResampler::Process(std::span<const float> in, std::span<float> out)
{
    // The last inputs of the frames before move to the front, and the frame's inputs follow them.
    const std::size_t history = History();
    for (std::size_t c = 0; c < input_.size(); c++)
    {
        std::vector<float>& input = input_[c];
        std::copy(input.end() - static_cast<std::ptrdiff_t>(history), input.end(), input.begin());
        for (std::size_t i = 0; i < from_; i++)
        {
            const std::size_t at = 2 * i + c;
            input[history + i] = at < in.size() ? in[at] : 0.0f;
        }
    }

    // Output j lies j * from / to inputs after the first output's place. That place lies Delay() inputs before the
    // frame's first input. The kernel starts HalfWidth() - 1 inputs before the input below the place.
    const auto half = static_cast<std::size_t>(kernel_->HalfWidth());
    const std::size_t start = history - Delay() - (half - 1);
    for (std::size_t j = 0; j < to_ && 2 * j + 1 < out.size(); j++)
    {
        const uint64_t place = (static_cast<uint64_t>(j) * from_ << 16) / to_;
        const std::size_t index = start + static_cast<std::size_t>(place >> 16);
        const auto fraction = static_cast<uint32_t>(place & 0xffff);
        for (std::size_t c = 0; c < input_.size(); c++)
        {
            out[2 * j + c] = kernel_->At(input_[c].data() + index, fraction);
        }
    }
}

ResampledRenderer::ResampledRenderer(std::unique_ptr<audio::Renderer> source, std::size_t from, std::size_t to,
                                     int rate)
    : source_(std::move(source)),
      resampler_(from, to),
      rate_(rate),
      input_(2 * from),
      output_(2 * to),
      output_read_(output_.size()),
      flush_((resampler_.Delay() + from - 1) / from)
{
}

std::size_t ResampledRenderer::Render(float* out, std::size_t frames)
{
    std::size_t done = 0;
    while (done < frames)
    {
        if (output_read_ == output_.size() && !NextFrame())
        {
            break;
        }

        const std::size_t count = std::min(frames - done, (output_.size() - output_read_) / 2);
        std::copy_n(output_.begin() + static_cast<std::ptrdiff_t>(output_read_), 2 * count, out + 2 * done);
        output_read_ += 2 * count;
        done += count;
    }

    return done;
}

bool ResampledRenderer::NextFrame()
{
    // A frame that the source leaves short ends in silence. Once the source has ended, frames of silence follow until
    // the resampler's delay has come out.
    std::size_t taken = 0;
    if (!ended_)
    {
        taken = source_->Render(input_.data(), resampler_.From());
        ended_ = taken < resampler_.From();
    }
    if (taken == 0)
    {
        if (flush_ == 0)
        {
            return false;
        }
        flush_--;
    }

    resampler_.Process(std::span<const float>(input_).first(2 * taken), output_);
    output_read_ = 0;

    return true;
}

std::unique_ptr<audio::Renderer> AtRate(std::unique_ptr<audio::Renderer> renderer, int rate)
{
    // The stereo samples in a frame of the 3DS at `each`, or 0 for another rate.
    const auto frame_samples = [](int each) -> std::size_t
    {
        if (each == CtrModel::kSampleRate)
        {
            return CtrModel::kFrameSamples;
        }

        return each == CtrModel::kEnhancedSampleRate ? CtrModel::kEnhancedFrameSamples : 0;
    };
    const std::size_t from = frame_samples(renderer->SampleRate());
    const std::size_t to = frame_samples(rate);
    if (renderer->SampleRate() == rate || from == 0 || to == 0)
    {
        return renderer;
    }

    return std::make_unique<ResampledRenderer>(std::move(renderer), from, to, rate);
}

} // namespace hokuto::song
