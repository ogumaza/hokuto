// SPDX-License-Identifier: MIT

#include "song/catch_up.h"

#include <algorithm>
#include <chrono>
#include <utility>

namespace hokuto::song
{
namespace
{

// The tick that a run in ticks reaches: the clock's position, less `behind` seconds of the model's frames at the tempo
// there.
double TargetTicks(const SequenceModel& model, const PlaybackClock& clock, double behind)
{
    const double ticks = clock.ModelTicks();
    const double frames = behind * static_cast<double>(model.SampleRate()) / static_cast<double>(model.FrameSamples());
    return std::max(ticks - frames * model.TicksPerFrame(ticks), 0.0);
}

// Whether `model` has run far enough: up to the clock's frame, up to its position in ticks with `by_ticks`, or through
// the sequence's first pass without a clock. A model whose clock has stopped never reaches another tick. A run in ticks
// ends there.
bool CaughtUp(const SequenceModel& model, const PlaybackClock* clock, bool by_ticks, double behind)
{
    if (clock != nullptr && by_ticks)
    {
        return model.After() >= TargetTicks(model, *clock, behind) || model.ClockStopped();
    }
    if (clock != nullptr)
    {
        return model.Frames() >= clock->ModelFrames();
    }

    return model.After() >= static_cast<double>(model.GetSequence().length) || model.ClockStopped();
}

// Whether the next frame of `model` mixes: whether it lies in the lead before the clock's frame, or before the tick
// that a run in ticks reaches, or after the lead's start. Without a clock, each frame mixes. The snapshots then settle.
bool Mixes(const SequenceModel& model, const PlaybackClock* clock, bool by_ticks, double behind)
{
    if (clock != nullptr && by_ticks)
    {
        return model.InLeadOfTick(TargetTicks(model, *clock, behind));
    }
    if (clock != nullptr)
    {
        return model.InLead(clock->ModelFrames());
    }

    return true;
}

} // namespace

ModelCatchUp::~ModelCatchUp()
{
    Cancel();
}

void ModelCatchUp::Start(std::shared_ptr<SequenceModel> model, std::shared_ptr<const PlaybackClock> clock,
                         bool by_ticks, double behind)
{
    Cancel();
    by_ticks_ = by_ticks;

    // The first frame starts the model. The run checks for a cancel after each frame: about every 20 µs.
    const auto run = [this, run_model = std::move(model), run_clock = std::move(clock), by_ticks, behind]
    {
        // The model takes the playback's mutes and buses' volumes before each frame.
        const auto follow = [&run_model, &run_clock]
        {
            run_model->SetMutes(run_clock ? run_clock->Mutes() : 0);
            if (run_clock)
            {
                run_model->SetBusVolumes(run_clock->BusVolumes());
            }
        };

        // Once the frames mix, they go on mixing, even when the clock's place moves back.
        follow();
        bool plays = run_model->SeekFrame(0);
        bool mixes = false;
        while (plays && !cancel_.load(std::memory_order_relaxed) &&
               !CaughtUp(*run_model, run_clock.get(), by_ticks, behind))
        {
            follow();
            mixes = mixes || Mixes(*run_model, run_clock.get(), by_ticks, behind);
            run_model->SetMixing(mixes);
            plays = run_model->RunFrame();
        }
        run_model->SetMixing(true);

        return run_model;
    };
    run_ = std::async(std::launch::async, run);
}

bool ModelCatchUp::ByTicks() const
{
    return by_ticks_;
}

bool ModelCatchUp::Pending() const
{
    return run_.valid();
}

bool ModelCatchUp::Finished() const
{
    return run_.valid() && run_.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
}

std::shared_ptr<SequenceModel> ModelCatchUp::Take()
{
    if (!run_.valid())
    {
        return nullptr;
    }

    cancel_.store(true, std::memory_order_relaxed);
    std::shared_ptr<SequenceModel> model = run_.get();
    cancel_.store(false, std::memory_order_relaxed);

    return model;
}

void ModelCatchUp::Cancel()
{
    Take();
}

} // namespace hokuto::song
