// SPDX-License-Identifier: MIT

// Runs a new model of a sequence on a thread of its own, up to the frame that playback has reached. An edit during
// playback makes a new model for the new version. The old model plays on until the new one has caught up, and the
// player then takes the new one (see SequencePlayer). On the Switch's music, a model that mixes runs 60 to 250 times
// faster than real time on the Mac. The frames before the lead don't mix and run faster still. With nn::audio's I3DL2
// preset 1 on bus A, a catch-up to minute 1 took up to 0.85 s, and 2.1 s in the enhanced mode. Six catch-ups ran side
// by side.

#pragma once

#include <atomic>
#include <future>
#include <memory>

#include "song/player.h"
#include "song/sequence_model.h"

namespace hokuto::song
{

class ModelCatchUp
{
public:
    ModelCatchUp() = default;
    ModelCatchUp(const ModelCatchUp&) = delete;
    ModelCatchUp& operator=(const ModelCatchUp&) = delete;

    // Cancels the run that's going, and waits for it to stop.
    ~ModelCatchUp();

    // Starts to run `model` from its start, with the mutes and the buses' volumes of the playback. Without a clock, it
    // runs with no mutes, at the buses' volumes that it has. With a clock, the model runs up to the clock's frame (see
    // PlaybackClock::ModelFrames), and follows it as playback moves on. With `by_ticks`, it runs up to the clock's
    // position in ticks instead (see PlaybackClock::ModelTicks): the time of the ticks differs between the two models
    // (SameTiming). A run in ticks stops `behind` seconds short of that position, at the tempo there. A model of
    // another engine then stops at the sample that the audio device plays, and playback starts again there on it. The
    // frames before the lead don't mix (see SequenceModel::InLead). The model then reaches a settled state there
    // sooner. Without a clock, the model runs through the sequence's first pass, and every frame mixes. Its snapshots
    // then cover any seek, settled. The run stops early if the model's output ends. A run in ticks, or without a clock,
    // also stops where the model's clock stops (SequenceModel::ClockStopped). A run that's still going is cancelled,
    // and its model goes.
    void Start(std::shared_ptr<SequenceModel> model, std::shared_ptr<const PlaybackClock> clock, bool by_ticks = false,
               double behind = 0.0);

    // Whether the last run that started runs up to the clock's position in ticks. The program offers its model to the
    // player the same way (PlaybackClock::Offer).
    bool ByTicks() const;

    // Whether a run has started and its model hasn't been taken, and whether that run has stopped. The model belongs to
    // the run's thread until it's taken.
    bool Pending() const;
    bool Finished() const;

    // The run's model, or nullptr without a run. A run that's still going is cancelled first. Cancel lets the model go.
    std::shared_ptr<SequenceModel> Take();
    void Cancel();

private:
    std::future<std::shared_ptr<SequenceModel>> run_;
    std::atomic<bool> cancel_{false};
    bool by_ticks_ = false;
};

} // namespace hokuto::song
