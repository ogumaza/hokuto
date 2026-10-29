// SPDX-License-Identifier: MIT

#include "song/ctr_model.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

#include "ntdlib/ctr/snd/engine.h"
#include "ntdlib/sar/formats.h"

namespace hokuto::song
{
namespace
{

using ntdlib::ctr::snd::DspKind;

// A sample in floating point, with full scale at 1, as a 16-bit sample.
int16_t Rounded(float sample)
{
    const double scaled = std::round(static_cast<double>(sample) * 32768.0);
    return static_cast<int16_t>(std::clamp(scaled, -32768.0, 32767.0));
}

} // namespace

CtrModel::CtrModel(bool enhanced)
    : enhanced_(enhanced),
      model_(std::make_unique<ntdlib::ctr::snd::ArchiveModel>(enhanced ? DspKind::kEnhancedModel : DspKind::kModel)),
      output_(2 * FrameSamples(), 0)
{
    // The DSP's samples wait from the moment the DSP starts. The 3DS's archive mode keeps them the same way. In
    // enhanced mixing, the firmware's output stays silent, and each frame comes from the frame sink.
    if (enhanced_)
    {
        pending_floats_.assign(2 * kEnhancedSilence, 0.0f);
        float_output_.assign(2 * kEnhancedFrameSamples, 0.0f);
        model_->dsp->SetFrameSink([this](std::span<const float> frame)
                                  { pending_floats_.insert(pending_floats_.end(), frame.begin(), frame.end()); });
        return;
    }

    pending_.assign(2 * kSilence, 0);
    model_->dsp->SetSampleSink([this](std::array<int16_t, 2> sample)
                               { pending_.insert(pending_.end(), sample.begin(), sample.end()); });
}

bool CtrModel::Start(const ntdlib::sar::SoundArchive& archive)
{
    return model_->Start(archive);
}

bool CtrModel::RunFrame()
{
    const bool ran = model_->engine->RunFrame();
    idle_frames_ = model_->engine->IsBusy() ? 0 : std::min(idle_frames_ + 1, kTailFrames);

    // A frame that the DSP hasn't filled ends in silence. The silence at the start keeps that from happening while the
    // DSP answers.
    if (enhanced_)
    {
        const std::size_t count = std::min(pending_floats_.size(), float_output_.size());
        std::copy_n(pending_floats_.begin(), count, float_output_.begin());
        std::fill(float_output_.begin() + static_cast<std::ptrdiff_t>(count), float_output_.end(), 0.0f);
        pending_floats_.erase(pending_floats_.begin(), pending_floats_.begin() + static_cast<std::ptrdiff_t>(count));
        std::transform(float_output_.begin(), float_output_.end(), output_.begin(), Rounded);
        return ran;
    }

    const std::size_t count = std::min(pending_.size(), output_.size());
    std::copy_n(pending_.begin(), count, output_.begin());
    std::fill(output_.begin() + static_cast<std::ptrdiff_t>(count), output_.end(), 0);
    pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(count));

    return ran;
}

CtrModel::Snapshot CtrModel::Save(const Snapshot* previous)
{
    return {model_->Save(previous != nullptr ? &previous->model : nullptr),
            pending_,
            pending_floats_,
            output_,
            float_output_,
            idle_frames_};
}

void CtrModel::Restore(const Snapshot& snapshot)
{
    model_->Restore(snapshot.model);
    pending_ = snapshot.pending;
    pending_floats_ = snapshot.pending_floats;
    output_ = snapshot.output;
    float_output_ = snapshot.float_output;
    idle_frames_ = snapshot.idle_frames;
}

} // namespace hokuto::song
