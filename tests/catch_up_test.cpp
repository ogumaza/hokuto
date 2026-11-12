// SPDX-License-Identifier: MIT

// Tests the switch from one model to another during playback (src/song/catch_up.cpp).

#include "song_runs.h"

namespace
{

// A model with a reverb on bus A takes over from a model without it, in the send sequence's note. A catch-up runs the
// new model up to the player's frame first. The frame of the switch fades the old output out and the new one in. From
// the next frame on, the output is the reverb's, up to the end of its tail. With track 0 muted, a model that hasn't run
// takes over in silence. The muted note sends nothing to the reverb, and the output ends with the note.
void TestSwitch()
{
    constexpr std::size_t kBefore = 50;
    const std::shared_ptr<const sar::SoundArchive> archive = OpenSendArchive();
    const auto sequence = std::make_shared<const Sequence>(ReadSequence(*archive, kSendSound));
    const ModelRun wet = RunModel(*archive, kSendSound, 1600, AuxReverb());
    const ModelRun dry = RunModel(*archive, kSendSound, 1600);
    const std::size_t first = FirstFrameReaching(dry, 1);
    const std::size_t switched = first + kBefore;
    const std::size_t end = TailEnd(wet, first);
    const auto clock = std::make_shared<PlaybackClock>(kFrameSamples);
    SequencePlayer player(ModelOf(archive, sequence), 0, clock);
    ModelCatchUp catch_up;

    const PlayerRun before = RunPlayer(player, *clock, kBefore);
    const uint64_t player_frame = clock->ModelFrames();
    catch_up.Start(ModelOf(archive, sequence, SequenceModel::kSnapshotInterval, AuxEffectsWithReverb()), clock);
    const std::shared_ptr<SequenceModel> caught_up = WaitForCatchUp(catch_up);
    SEQLIB_REQUIRE(caught_up != nullptr);
    const uint64_t caught_up_frame = caught_up->Frames();
    clock->Offer(caught_up);
    const PlayerRun after = RunPlayer(player, *clock, 1600);
    const auto muted_clock = std::make_shared<PlaybackClock>(kFrameSamples);
    muted_clock->SetMutes(1u << 0);
    SequencePlayer muted_player(ModelOf(archive, sequence), 0, muted_clock);
    RunPlayer(muted_player, *muted_clock, kBefore);
    muted_clock->Offer(ModelOf(archive, sequence, SequenceModel::kSnapshotInterval, AuxEffectsWithReverb()));
    const PlayerRun muted = RunPlayer(muted_player, *muted_clock, 100);

    SEQLIB_REQUIRE(!std::ranges::equal(ModelFrame(wet, switched), ModelFrame(dry, switched)));
    SEQLIB_CHECK(Matches(before.samples, dry, first, 0));
    SEQLIB_CHECK(player_frame == switched && caught_up_frame == switched);
    SEQLIB_REQUIRE(after.samples.size() > kFrameSamples * 2);
    SEQLIB_CHECK(Crossfades(after.samples, 0, dry, wet, switched));
    SEQLIB_CHECK(Matches(after.samples, wet, switched, kFrameSamples * 2));
    SEQLIB_CHECK(after.ended && after.samples.size() == (end - switched) * kFrameSamples * 2);
    SEQLIB_CHECK(!HasSound(std::span<const float>(muted.samples)));
    SEQLIB_CHECK(muted.ended && muted.samples.size() == (EndFramesOf(dry).idle - switched) * kFrameSamples * 2);
}

// A model whose output has ended before the player's frame takes over with nothing to play. The send sequence plays
// with the reverb into its tail, after the note's release, and a model without the reverb takes over. The old output
// fades out across the next frame, and the output ends. Inside the cycle, silence follows up to the right locator,
// about 43 ticks later. No frame starts on it exactly. The next pass plays the new model from the start, after a frame
// that fades in.
void TestSwitchAfterEnd()
{
    const std::shared_ptr<const sar::SoundArchive> archive = OpenSendArchive();
    const auto sequence = std::make_shared<const Sequence>(ReadSequence(*archive, kSendSound));
    const ModelRun wet = RunModel(*archive, kSendSound, 1600, AuxReverb());
    const ModelRun dry = RunModel(*archive, kSendSound, 1600);
    const EndFrames ends = EndFramesOf(dry);
    const std::size_t first = ends.first;
    const std::size_t played = ends.idle + 50 - first;
    const auto right = static_cast<uint32_t>(PositionBefore(dry, ends, first + played + 90));
    const std::size_t second = FramePlayingPastEnd(dry, ends, right) + 1 - first;

    const auto play = [&](const PlaybackClock::CycleRange& cycle, std::size_t frames)
    {
        const auto clock = std::make_shared<PlaybackClock>(kFrameSamples);
        clock->SetCycle(cycle);
        SequencePlayer player(ModelOf(archive, sequence, SequenceModel::kSnapshotInterval, AuxEffectsWithReverb()), 0,
                              clock);

        const PlayerRun before = RunPlayer(player, *clock, played);
        clock->Offer(ModelOf(archive, sequence));
        const PlayerRun after = RunPlayer(player, *clock, frames);
        return std::make_tuple(before, after, clock->Ended());
    };

    const auto [before, after, ended] = play({}, 100);
    const auto [cycle_before, cycle_after, cycle_ended] = play({true, 0, right}, second - played + 20);

    SEQLIB_REQUIRE(first + played + 100 < TailEnd(wet, first) && second > played + 80);
    SEQLIB_CHECK(Matches(before.samples, wet, first, 0));
    SEQLIB_CHECK(HasSound(std::span<const float>(after.samples)));
    SEQLIB_CHECK(after.ended && after.samples.size() == kFrameSamples * 2);
    SEQLIB_CHECK(Fades(after.samples, 0, wet, first + played, false));
    SEQLIB_CHECK(ended);
    SEQLIB_CHECK(cycle_before.samples == before.samples && !cycle_after.ended && !cycle_ended);
    SEQLIB_CHECK(Fades(cycle_after.samples, 0, wet, first + played, false));
    SEQLIB_CHECK(!HasSound(std::span<const float>(FramesOf(cycle_after, 1, second - played - 1))));
    SEQLIB_CHECK(Fades(cycle_after.samples, second - played, dry, first, true));
    SEQLIB_CHECK(Matches(FramesOf(cycle_after, second - played, 20), dry, first, kFrameSamples * 2));
}

// In the silence after the end of the sequence inside the cycle, a model takes over as it is. The send sequence plays
// without effects, with the right locator 100 ticks past its end, and a model with the reverb takes over in the
// silence. The silence goes on to the right locator. The second pass then plays the reverb's output from the start,
// after a frame that fades in.
void TestSwitchInSilence()
{
    const std::shared_ptr<const sar::SoundArchive> archive = OpenSendArchive();
    const auto sequence = std::make_shared<const Sequence>(ReadSequence(*archive, kSendSound));
    const ModelRun wet = RunModel(*archive, kSendSound, 1600, AuxReverb());
    const ModelRun dry = RunModel(*archive, kSendSound, 1600);
    const EndFrames ends = EndFramesOf(dry);
    const uint32_t right = sequence->length + 100;
    const std::size_t second = FramePlayingPastEnd(dry, ends, right) + 1 - ends.first;
    const std::size_t silent = ends.idle + 10 - ends.first;
    const auto clock = std::make_shared<PlaybackClock>(kFrameSamples);
    clock->SetCycle({true, 0, right});
    SequencePlayer player(ModelOf(archive, sequence), 0, clock);

    const PlayerRun before = RunPlayer(player, *clock, silent);
    clock->Offer(ModelOf(archive, sequence, SequenceModel::kSnapshotInterval, AuxEffectsWithReverb()));
    const PlayerRun after = RunPlayer(player, *clock, second - silent + 20);

    SEQLIB_REQUIRE(silent < second && !after.ended);
    SEQLIB_CHECK(!HasSound(std::span<const float>(FramesOf(before, ends.idle - ends.first, 10))));
    SEQLIB_CHECK(!HasSound(std::span<const float>(FramesOf(after, 0, second - silent))));
    SEQLIB_CHECK(Fades(after.samples, second - silent, wet, ends.first, true));
    SEQLIB_CHECK(Matches(FramesOf(after, second - silent, 20), wet, ends.first, kFrameSamples * 2));
}

// A frame that seeks takes an offered model as it is: the first frame, and the first frame of each pass of the cycle
// after the first. A model with the reverb, offered before playback starts, plays the send sequence's first pass, with
// the locators in the note. A model without the reverb, offered at the end of the first pass, plays the second pass
// from the left locator, after a frame that fades in.
void TestSwitchAtSeek()
{
    constexpr PlaybackClock::CycleRange kCycle{true, 10, 30};
    const std::shared_ptr<const sar::SoundArchive> archive = OpenSendArchive();
    const auto sequence = std::make_shared<const Sequence>(ReadSequence(*archive, kSendSound));
    const ModelRun wet = RunModel(*archive, kSendSound, 1600, AuxReverb());
    const ModelRun dry = RunModel(*archive, kSendSound, 1600);
    const std::size_t first = FirstFrameReaching(dry, 1);
    const std::size_t left = FirstFrameReaching(dry, kCycle.start + 1);
    const std::size_t right = FirstFrameReaching(dry, kCycle.end);
    const std::size_t first_pass = right - first + 1;
    const std::size_t pass = right - left + 1;
    const auto clock = std::make_shared<PlaybackClock>(kFrameSamples);
    clock->SetCycle(kCycle);
    SequencePlayer player(ModelOf(archive, sequence), 0, clock);

    clock->Offer(ModelOf(archive, sequence, SequenceModel::kSnapshotInterval, AuxEffectsWithReverb()));
    const PlayerRun first_part = RunPlayer(player, *clock, first_pass);
    clock->Offer(ModelOf(archive, sequence));
    const PlayerRun second_part = RunPlayer(player, *clock, pass);

    SEQLIB_REQUIRE(!std::ranges::equal(ModelFrame(wet, left + 1), ModelFrame(dry, left + 1)));
    SEQLIB_CHECK(Matches(FramesOf(first_part, 0, first_pass - 1), wet, first, 0));
    SEQLIB_CHECK(Fades(first_part.samples, first_pass - 1, wet, right, false));
    SEQLIB_CHECK(Fades(second_part.samples, 0, dry, left, true));
    SEQLIB_CHECK(Matches(FramesOf(second_part, 0, pass - 1), dry, left, kFrameSamples * 2));
}

// The program's switch: the render worker plays the send sequence without effects into a ring, and the test reads the
// ring as the device would. A catch-up runs a model with the reverb up to the player's frame on its thread. The worker
// plays meanwhile. The clock then offers the model. The output is the model's without effects up to one frame that
// crossfades, and the reverb's after it, up to the end of its tail.
void TestSwitchWhilePlaying()
{
    constexpr std::size_t kBefore = 20;
    const std::shared_ptr<const sar::SoundArchive> archive = OpenSendArchive();
    const auto sequence = std::make_shared<const Sequence>(ReadSequence(*archive, kSendSound));
    const ModelRun wet = RunModel(*archive, kSendSound, 1600, AuxReverb());
    const ModelRun dry = RunModel(*archive, kSendSound, 1600);
    const std::size_t first = FirstFrameReaching(dry, 1);
    const std::size_t end = TailEnd(wet, first);
    hokuto::audio::FrameRing ring(16'384);
    hokuto::audio::RenderWorker worker(ring, std::chrono::milliseconds(100), 256);
    const auto clock = std::make_shared<PlaybackClock>(kFrameSamples);
    ModelCatchUp catch_up;
    std::vector<float> samples;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);

    // Reads up to `frames` frames of the model, or up to the end of the output.
    const auto read = [&](std::size_t frames)
    {
        std::vector<float> block(256 * 2);
        while (samples.size() < frames * kFrameSamples * 2 && std::chrono::steady_clock::now() < deadline &&
               (worker.Playing() || ring.Available() > 0))
        {
            const std::size_t got = ring.Read(block.data(), block.size() / 2);
            samples.insert(samples.end(), block.begin(), block.begin() + static_cast<std::ptrdiff_t>(got * 2));
            if (got == 0)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
    };

    worker.Play(std::make_unique<SequencePlayer>(ModelOf(archive, sequence), 0, clock));
    read(kBefore);
    catch_up.Start(ModelOf(archive, sequence, SequenceModel::kSnapshotInterval, AuxEffectsWithReverb()), clock);
    const std::shared_ptr<SequenceModel> caught_up = WaitForCatchUp(catch_up);
    SEQLIB_REQUIRE(caught_up != nullptr);
    clock->Offer(caught_up);
    read(1600);

    // The frame that crossfades is the first that differs from the output without effects.
    std::size_t switched = 0;
    while (
        (switched + 1) * kFrameSamples * 2 <= samples.size() &&
        Matches(std::vector<float>(samples.begin() + static_cast<std::ptrdiff_t>(switched * kFrameSamples * 2),
                                   samples.begin() + static_cast<std::ptrdiff_t>((switched + 1) * kFrameSamples * 2)),
                dry, first + switched, 0))
    {
        switched++;
    }
    const std::vector<float> after(samples.begin() + static_cast<std::ptrdiff_t>(switched * kFrameSamples * 2),
                                   samples.end());
    SEQLIB_REQUIRE(switched >= kBefore && switched + first + 10 < EndFramesOf(dry).idle);
    SEQLIB_CHECK(Crossfades(after, 0, dry, wet, first + switched));
    SEQLIB_CHECK(Matches(after, wet, first + switched, kFrameSamples * 2));
    SEQLIB_CHECK_EQ(samples.size(), (end - first) * kFrameSamples * 2);
}

// A model that plays the ticks at other times takes over at the player's position in ticks. The loop's sequence plays
// at tempo 120 for 150 frames, up to about tick 72. A catch-up runs the sequence at tempo 60 up to that tick: about 300
// frames. After the switch, the position goes on from there at 0.24 ticks a frame, and the output is the slow model's.
// A switch by frames would have gone back to about tick 36.
void TestSwitchByTicks()
{
    constexpr std::size_t kBefore = 150;
    const std::shared_ptr<const sar::SoundArchive> archive = OpenSlowArchive();
    const auto loop = std::make_shared<const Sequence>(ReadSequence(*archive, kLoopSound));
    const auto slow = std::make_shared<const Sequence>(ReadSequence(*archive, kSlowSound));
    const ModelRun slow_run = RunModel(*archive, kSlowSound, 800);
    const auto clock = std::make_shared<PlaybackClock>(kFrameSamples);
    SequencePlayer player(ModelOf(archive, loop), 0, clock);
    ModelCatchUp catch_up;

    RunPlayer(player, *clock, kBefore);
    const double ticks = clock->ModelTicks();

    // A model of another timebase runs up to the clock's position in its ticks.
    catch_up.Start(ModelOf(archive, slow), clock, true, 0.0, 1.5);
    const std::shared_ptr<SequenceModel> scaled = WaitForCatchUp(catch_up);
    SEQLIB_REQUIRE(scaled != nullptr);
    SEQLIB_CHECK(scaled->After() >= ticks * 1.5 && scaled->After() <= ticks * 1.5 + 1.0);

    catch_up.Start(ModelOf(archive, slow), clock, true);
    const std::shared_ptr<SequenceModel> caught_up = WaitForCatchUp(catch_up);
    SEQLIB_REQUIRE(caught_up != nullptr);
    const double caught_up_ticks = caught_up->After();
    clock->Offer(caught_up, true);
    const PlayerRun switched = RunPlayer(player, *clock, 1);
    const double switched_ticks = clock->ModelTicks();
    const uint64_t switched_frame = clock->ModelFrames() - 1;
    const PlayerRun after = RunPlayer(player, *clock, 100);

    SEQLIB_CHECK(!SameTiming(*loop, *slow) && SameTiming(*loop, *loop) && catch_up.ByTicks());
    SEQLIB_CHECK(ticks > 60.0 && ticks < 80.0);
    SEQLIB_CHECK(caught_up_ticks >= ticks && caught_up_ticks <= ticks + 1.0);
    SEQLIB_REQUIRE(switched.samples.size() == kFrameSamples * 2);
    SEQLIB_CHECK(switched_ticks >= ticks && switched_ticks <= ticks + 2.0);
    SEQLIB_CHECK(switched_frame > kBefore * 2 - 10 && switched_frame < kBefore * 2 + 10);
    SEQLIB_CHECK(Matches(after.samples, slow_run, switched_frame + 1, 0));
    SEQLIB_CHECK(std::abs(clock->ModelTicks() - switched_ticks - 24.0) <= 1.0);
}

// A catch-up runs a new model up to the clock's frame, with the clock's mutes. The model's frame then matches the model
// run directly. Without a clock, it runs through the sequence's first pass, and its snapshots cover it. A run stops
// where the model's output ends. A take cancels a run that's going.
void TestCatchUp()
{
    const std::shared_ptr<const sar::SoundArchive> archive = OpenTestArchive();
    const auto loop = std::make_shared<const Sequence>(ReadSequence(*archive, kLoopSound));
    const auto note = std::make_shared<const Sequence>(ReadSequence(*archive, kNoteSound));
    const ModelRun model = RunModel(*archive, kLoopSound, 800);
    const auto clock = std::make_shared<PlaybackClock>(kFrameSamples);
    clock->SetModelFrames(300);
    ModelCatchUp catch_up;

    catch_up.Start(ModelOf(archive, loop), clock);
    const std::shared_ptr<SequenceModel> caught_up = WaitForCatchUp(catch_up);
    clock->SetMutes(1u << 0);
    catch_up.Start(ModelOf(archive, loop), clock);
    const std::shared_ptr<SequenceModel> muted = WaitForCatchUp(catch_up);
    catch_up.Start(ModelOf(archive, loop, 10), nullptr);
    const std::shared_ptr<SequenceModel> first_pass = WaitForCatchUp(catch_up);
    clock->SetModelFrames(std::numeric_limits<uint64_t>::max());
    catch_up.Start(ModelOf(archive, note), clock);
    const std::shared_ptr<SequenceModel> ended = WaitForCatchUp(catch_up);
    catch_up.Start(ModelOf(archive, loop), clock);
    const bool running = catch_up.Pending() && !catch_up.Finished();
    const std::shared_ptr<SequenceModel> cancelled = catch_up.Take();

    SEQLIB_REQUIRE(caught_up && muted && first_pass && ended && cancelled);
    SEQLIB_CHECK(caught_up->Frames() == 300 && std::ranges::equal(caught_up->Output(), ModelFrame(model, 299)));
    SEQLIB_CHECK(HasSound(caught_up->Output()) && muted->Frames() == 300 && !HasSound(muted->Output()));
    SEQLIB_CHECK(first_pass->Frames() == FirstFrameReaching(model, loop->length) + 1);
    SEQLIB_CHECK(first_pass->SnapshotCount() > 30);
    SEQLIB_CHECK(ended->Frames() > 0 && !ended->RunFrame());
    SEQLIB_CHECK(running && !catch_up.Pending() && cancelled->Frames() > 0);
}

// A catch-up far from the model's start runs the frames before the lead without mixing. The loop's sequence catches up
// to frame 1450, and by ticks to tick 700. Its last frame is then the model's run directly. With nn::audio's I3DL2
// preset 1 on the send loop's sequence, the 20 frames from there lie 60 dB or more below the model's.
void TestCatchUpWithoutMixing()
{
    constexpr std::size_t kSamples = 20 * kFrameSamples * 2;
    const std::shared_ptr<const sar::SoundArchive> archive = OpenTestArchive();
    const auto loop = std::make_shared<const Sequence>(ReadSequence(*archive, kLoopSound));
    const ModelRun model = RunModel(*archive, kLoopSound, 1500);
    const std::shared_ptr<const sar::SoundArchive> send_archive = OpenSendArchive(kSendLoopBytecode);
    const auto send = std::make_shared<const Sequence>(ReadSequence(*send_archive, kSendSound));
    const ModelRun wet = RunModel(*send_archive, kSendSound, 1500, AuxReverb());
    const auto clock = std::make_shared<PlaybackClock>(kFrameSamples);
    clock->SetModelFrames(1450);
    clock->SetModelTicks(700.0);
    ModelCatchUp catch_up;

    catch_up.Start(ModelOf(archive, loop), clock);
    const std::shared_ptr<SequenceModel> by_frames = WaitForCatchUp(catch_up);
    catch_up.Start(ModelOf(archive, loop), clock, true);
    const std::shared_ptr<SequenceModel> by_ticks = WaitForCatchUp(catch_up);
    catch_up.Start(ModelOf(send_archive, send, SequenceModel::kSnapshotInterval, AuxEffectsWithReverb()), clock);
    const std::shared_ptr<SequenceModel> reverb = WaitForCatchUp(catch_up);
    SEQLIB_REQUIRE(by_frames && by_ticks && reverb);
    std::vector<int16_t> played(reverb->Output().begin(), reverb->Output().end());
    while (played.size() < kSamples && reverb->RunFrame())
    {
        played.insert(played.end(), reverb->Output().begin(), reverb->Output().end());
    }
    const std::size_t tick_frame = FirstFrameReaching(model, 700);

    SEQLIB_CHECK(by_frames->Frames() == 1450 && std::ranges::equal(by_frames->Output(), ModelFrame(model, 1449)));
    SEQLIB_CHECK(by_ticks->Frames() == tick_frame + 1 &&
                 std::ranges::equal(by_ticks->Output(), ModelFrame(model, tick_frame)));
    SEQLIB_REQUIRE(reverb->Frames() == 1469 && played.size() == kSamples);
    SEQLIB_CHECK(
        SignalToDifference(played, std::span<const int16_t>(wet.samples).subspan(1449 * kFrameSamples * 2, kSamples)) >=
        60.0);
}

// A catch-up in ticks can stop short of the clock's position. A model of another engine then stops at the sample that
// the audio device plays. 0.1 s at the loop's tempo is 20 frames of 5 ms. The model stops in the frame that reaches
// that tick. The model counts whole ticks. A seek to its next tick goes on from there, up to the first frame past that
// tick. The frame is the model's run directly.
void TestCatchUpBehind()
{
    const std::shared_ptr<const sar::SoundArchive> archive = OpenTestArchive();
    const auto loop = std::make_shared<const Sequence>(ReadSequence(*archive, kLoopSound));
    const ModelRun model = RunModel(*archive, kLoopSound, 1500);
    const auto clock = std::make_shared<PlaybackClock>(kFrameSamples);
    clock->SetModelTicks(700.0);
    ModelCatchUp catch_up;

    catch_up.Start(ModelOf(archive, loop), clock, true, 0.1);
    const std::shared_ptr<SequenceModel> behind = WaitForCatchUp(catch_up);
    SEQLIB_REQUIRE(behind != nullptr);
    const double per_frame = behind->TicksPerFrame(700.0);
    const double target = 700.0 - 20.0 * per_frame;
    const double after = behind->After();
    const auto next = static_cast<uint64_t>(std::ceil(after));
    const bool sought = behind->Seek(next);
    std::size_t tick_frame = 0;
    while (tick_frame < model.after.size() && model.after[tick_frame] <= static_cast<double>(next))
    {
        tick_frame++;
    }

    SEQLIB_CHECK(per_frame > 0.0 && after >= target && after < target + std::max(per_frame, 1.0));
    SEQLIB_CHECK(sought && behind->Frames() == tick_frame + 1 &&
                 std::ranges::equal(behind->Output(), ModelFrame(model, tick_frame)));
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("catch_up",
                                    []
                                    {
                                        TestSwitch();
                                        TestSwitchByTicks();
                                        TestSwitchAfterEnd();
                                        TestSwitchInSilence();
                                        TestSwitchAtSeek();
                                        TestSwitchWhilePlaying();
                                        TestCatchUp();
                                        TestCatchUpWithoutMixing();
                                        TestCatchUpBehind();
                                    });
}
