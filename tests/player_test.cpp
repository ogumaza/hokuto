// SPDX-License-Identifier: MIT

// Tests the player (src/song/player.cpp): its clock, its output, seeks, cycles, mutes, meters and buses.

#include "song_runs.h"

namespace
{

// Between the starts of two frames, the position moves on in a straight line. Across the end of the loop, it stays at
// the frame's mark. A sample not yet written has no position. The cycle starts off, and keeps the locators it's given.
void TestPlaybackClock()
{
    PlaybackClock clock(kFrameSamples);
    clock.Mark(0, 10.0, {});
    clock.Mark(1, 12.0, {});
    clock.Mark(2, 2.0, {});
    clock.SetWritten(3 * kFrameSamples);

    SEQLIB_CHECK(clock.PositionAt(0) == 10.0);
    SEQLIB_CHECK(clock.PositionAt(kFrameSamples / 2) == 11.0);
    SEQLIB_CHECK(clock.PositionAt(kFrameSamples * 3 / 2) == 12.0);
    SEQLIB_CHECK(clock.PositionAt(2 * kFrameSamples + 1) == 2.0);
    SEQLIB_CHECK(!clock.PositionAt(3 * kFrameSamples).has_value());
    SEQLIB_CHECK(!clock.Cycle().on);
    SEQLIB_CHECK(!clock.Ended());

    clock.SetEnded();
    clock.SetCycle({true, 48, 4000000000u});
    const PlaybackClock::CycleRange cycle = clock.Cycle();

    SEQLIB_CHECK(clock.Ended());
    SEQLIB_CHECK(cycle.on && cycle.start == 48 && cycle.end == 4000000000u);
}

// A sample whose frame later frames have replaced in the ring has no position. The peaks and the variables work the
// same way. The frames that the ring still keeps give theirs.
void TestPositionAtReplaced()
{
    PlaybackClock clock(kFrameSamples);
    for (uint64_t frame = 0; frame < 300; frame++)
    {
        clock.Mark(frame, static_cast<double>(frame), {});
    }
    clock.SetWritten(300 * kFrameSamples);

    SEQLIB_CHECK(!clock.PositionAt(0).has_value());
    SEQLIB_CHECK(!clock.PositionAt(44 * kFrameSamples).has_value());
    SEQLIB_CHECK(clock.PositionAt(45 * kFrameSamples) == 45.0);
    SEQLIB_CHECK(clock.PositionAt(299 * kFrameSamples) == 299.0);
}

// The peaks of the tracks and the aux buses over a range of samples are the highest in the frames that hold those
// samples. An empty range, and frames not yet marked, add nothing. So do frames that later frames have replaced in the
// ring.
void TestPeaksBetween()
{
    constexpr std::array<float, 2> kBoth = {0.75f, 0.5f};
    const PlaybackClock::Peaks none;
    PlaybackClock::Peaks quiet;
    quiet.tracks[0] = {0.25f, 0.5f};
    quiet.buses[0] = {0.125f, 0.0f};
    PlaybackClock::Peaks loud;
    loud.tracks[0] = {0.75f, 0.125f};
    loud.tracks[15] = {1.5f, 1.0f};
    loud.buses[2] = {0.5f, 0.25f};
    PlaybackClock clock(kFrameSamples);
    clock.Mark(0, 0.0, quiet);
    clock.Mark(1, 1.0, loud);
    clock.Mark(2, 2.0, none);
    PlaybackClock wrapped(kFrameSamples);
    for (uint64_t frame = 0; frame < 300; frame++)
    {
        wrapped.Mark(frame, static_cast<double>(frame), frame == 299 ? loud : quiet);
    }

    const PlaybackClock::Peaks first = clock.PeaksBetween(0, kFrameSamples);
    const PlaybackClock::Peaks both = clock.PeaksBetween(kFrameSamples - 1, kFrameSamples + 1);
    const PlaybackClock::Peaks past_end = clock.PeaksBetween(2 * kFrameSamples, 10 * kFrameSamples);
    const PlaybackClock::Peaks empty = clock.PeaksBetween(kFrameSamples / 2, kFrameSamples / 2);
    const PlaybackClock::Peaks replaced = wrapped.PeaksBetween(43 * kFrameSamples, 44 * kFrameSamples);
    const PlaybackClock::Peaks kept = wrapped.PeaksBetween(299 * kFrameSamples, 302 * kFrameSamples);

    SEQLIB_CHECK(first == quiet);
    SEQLIB_CHECK(both.tracks[0] == kBoth && both.tracks[15] == loud.tracks[15]);
    SEQLIB_CHECK(both.buses[0] == quiet.buses[0] && both.buses[2] == loud.buses[2]);
    SEQLIB_CHECK(past_end == none);
    SEQLIB_CHECK(empty == none);
    SEQLIB_CHECK(replaced == none);
    SEQLIB_CHECK(kept == loud);
}

// The clock gives the variables' values after the frame that holds a sample. A frame not yet marked, and one that later
// frames have replaced in the ring, give nothing.
void TestVariablesAt()
{
    PlaybackClock::Variables set = PlaybackClock::kUnset;
    set[0] = 1;
    set[31] = -300;
    PlaybackClock clock(kFrameSamples);
    clock.Mark(0, 0.0, {});
    clock.Mark(1, 1.0, {}, set);
    PlaybackClock wrapped(kFrameSamples);
    for (uint64_t frame = 0; frame < 300; frame++)
    {
        wrapped.Mark(frame, static_cast<double>(frame), {}, frame == 299 ? set : PlaybackClock::kUnset);
    }

    const std::optional<PlaybackClock::Variables> first = clock.VariablesAt(kFrameSamples - 1);
    const std::optional<PlaybackClock::Variables> second = clock.VariablesAt(kFrameSamples);
    const std::optional<PlaybackClock::Variables> unmarked = clock.VariablesAt(2 * kFrameSamples);
    const std::optional<PlaybackClock::Variables> replaced = wrapped.VariablesAt(43 * kFrameSamples);
    const std::optional<PlaybackClock::Variables> kept = wrapped.VariablesAt(299 * kFrameSamples + 1);

    SEQLIB_CHECK(first == PlaybackClock::kUnset);
    SEQLIB_CHECK(second == set);
    SEQLIB_CHECK(!unmarked.has_value());
    SEQLIB_CHECK(!replaced.has_value());
    SEQLIB_CHECK(kept == set);
}

// From the start, the player writes the model's output from the model's first frame that runs a tick. The model makes
// no sound before it. Each frame's position is the tick count before it, round the loop. In 3.5 s, the output goes
// round the loop twice and goes on.
void TestPlayFromStart()
{
    const std::shared_ptr<const sar::SoundArchive> archive = OpenTestArchive();
    const auto sequence = std::make_shared<const Sequence>(ReadSequence(*archive, kLoopSound));
    const ModelRun model = RunModel(*archive, kLoopSound, 800);
    const std::size_t first = FirstFrameReaching(model, 1);
    const auto clock = std::make_shared<PlaybackClock>(kFrameSamples);
    clock->SetCycle(kGameLoop);
    SequencePlayer player(ModelOf(archive, sequence), 0, clock);

    const PlayerRun run = RunPlayer(player, *clock, 700);

    SEQLIB_CHECK(!run.ended);
    SEQLIB_CHECK(!clock->Ended());
    SEQLIB_CHECK(HasSound(std::span<const float>(run.samples)));
    SEQLIB_CHECK(!HasSound(std::span<const int16_t>(model.samples).first(first * kFrameSamples * 2)));
    SEQLIB_CHECK(Matches(run.samples, model, first, 0));
    SEQLIB_REQUIRE_EQ(run.marks.size(), 700);

    std::size_t wrong = 0;
    std::size_t wraps = 0;
    for (std::size_t i = 0; i < run.marks.size(); i++)
    {
        const double expected = PositionOfTicks(*sequence, model.before[first + i]);
        if (!run.marks[i] || *run.marks[i] != expected)
        {
            wrong++;
        }
        if (i > 0 && run.marks[i] && run.marks[i - 1] && *run.marks[i] < *run.marks[i - 1])
        {
            wraps++;
        }
    }
    SEQLIB_CHECK_EQ(wrong, 0);
    SEQLIB_CHECK_EQ(wraps, 2);
}

// A sequence plays with the values that its variables start with. The player's output matches ntdlib's model run
// directly, with the values set before the start as a game sets them. Before the model starts, its variables hold the
// starting values. The clock gives each frame's values, the global ones included: the sound sets variable 1 to 5 in its
// first tick, unless variable 0 starts at 1.
void TestPlayVariables()
{
    const std::shared_ptr<const sar::SoundArchive> archive = OpenVariablesArchive();
    const auto unset = std::make_shared<const Sequence>(ReadSequence(*archive, kVariablesSound));
    const auto chosen = std::make_shared<const Sequence>(ReadSequence(*archive, kVariablesSound, {{0, 1}, {16, 3}}));
    const ModelRun unset_model = RunModel(*archive, kVariablesSound, 100);
    const ModelRun chosen_model = RunModel(*archive, kVariablesSound, 100, {}, {{0, 1}, {16, 3}});
    const auto unset_clock = std::make_shared<PlaybackClock>(kFrameSamples);
    const auto chosen_clock = std::make_shared<PlaybackClock>(kFrameSamples);
    const std::shared_ptr<SequenceModel> waiting = ModelOf(archive, chosen);
    SequencePlayer unset_player(ModelOf(archive, unset), 0, unset_clock);
    SequencePlayer chosen_player(ModelOf(archive, chosen), 0, chosen_clock);

    const std::array<int16_t, 32> before = waiting->Variables();
    const PlayerRun unset_run = RunPlayer(unset_player, *unset_clock, 60);
    const PlayerRun chosen_run = RunPlayer(chosen_player, *chosen_clock, 60);

    SEQLIB_CHECK(before[0] == 1 && before[1] == -1 && before[16] == 3 && before[17] == -1);
    SEQLIB_CHECK(HasSound(std::span<const float>(unset_run.samples)));
    SEQLIB_CHECK(HasSound(std::span<const float>(chosen_run.samples)));
    SEQLIB_CHECK(Matches(unset_run.samples, unset_model, FirstFrameReaching(unset_model, 1), 0));
    SEQLIB_CHECK(Matches(chosen_run.samples, chosen_model, FirstFrameReaching(chosen_model, 1), 0));
    SEQLIB_CHECK(unset_model.samples != chosen_model.samples);
    SEQLIB_REQUIRE(!unset_run.variables.empty() && !chosen_run.variables.empty());
    SEQLIB_REQUIRE(unset_run.variables[0] && chosen_run.variables[0]);
    SEQLIB_CHECK((*unset_run.variables[0])[0] == -1 && (*unset_run.variables[0])[1] == 5);
    SEQLIB_CHECK((*chosen_run.variables[0])[0] == 1 && (*chosen_run.variables[0])[1] == -1);
    SEQLIB_CHECK((*unset_run.variables[0])[16] == -1 && (*chosen_run.variables[0])[16] == 3);
}

// The model makes a change at the start of its frame. ntdlib's model run directly does the same with the game's call.
// The output follows the change from frame 203. That frame plays tick 97. The player's first frame is the model's frame
// 1: the first that plays a tick. The clock shows the variable's new value after frame 203, and the old one after frame
// 202.
void TestPlayChanges()
{
    const std::shared_ptr<const sar::SoundArchive> archive = OpenChangesArchive();
    const Changes kChanges = {{1012, 0, 1}};
    const auto sequence = std::make_shared<const Sequence>(ReadSequence(*archive, kChangesSound, {}, kChanges));
    const ModelRun model = RunModel(*archive, kChangesSound, 300, {}, {}, kChanges);
    const ModelRun unchanged = RunModel(*archive, kChangesSound, 300);
    const auto clock = std::make_shared<PlaybackClock>(kFrameSamples);
    SequencePlayer player(ModelOf(archive, sequence), 0, clock);

    const PlayerRun run = RunPlayer(player, *clock, 260);

    const std::size_t first = FirstFrameReaching(model, 1);
    SEQLIB_REQUIRE(first == 1 && run.variables.size() > 210);
    SEQLIB_CHECK(model.samples != unchanged.samples);
    SEQLIB_CHECK(
        std::equal(model.samples.begin(), model.samples.begin() + 203 * kFrameSamples * 2, unchanged.samples.begin()));
    SEQLIB_CHECK(Matches(run.samples, model, first, 0));
    SEQLIB_REQUIRE(run.variables[202 - first] && run.variables[203 - first]);
    SEQLIB_CHECK((*run.variables[202 - first])[0] == -1 && (*run.variables[203 - first])[0] == 1);
}

// A snapshot keeps the changes before it. A seek back before a change makes the change again, and a seek past it
// restores the snapshot after it. Snapshots come every 100 frames here, in the frames up to the loop's end at tick 145:
// before frames 0, 100, 200 and 300. The change comes in frame 203.
void TestSeekAcrossChange()
{
    const std::shared_ptr<const sar::SoundArchive> archive = OpenChangesArchive();
    const Changes kChanges = {{1012, 0, 1}};
    const auto sequence = std::make_shared<const Sequence>(ReadSequence(*archive, kChangesSound, {}, kChanges));
    const ModelRun model = RunModel(*archive, kChangesSound, 400, {}, {}, kChanges);
    const std::shared_ptr<SequenceModel> shared = ModelOf(archive, sequence, 100);

    std::size_t wrong = 0;
    for (const uint64_t frame : {350u, 120u, 250u, 205u, 203u, 202u, 300u})
    {
        if (!shared->SeekFrame(frame) || !std::ranges::equal(shared->Output(), ModelFrame(model, frame)))
        {
            wrong++;
        }
    }

    SEQLIB_CHECK_EQ(wrong, 0);
    SEQLIB_CHECK(shared->SnapshotCount() == 4);
}

// From a seek into the loop, the first frame fades in from silence. The frames after it are the model's. The first
// frame's position is the tick that playback starts from.
void TestPlayFromSeek()
{
    constexpr uint32_t kStart = 120;
    const std::shared_ptr<const sar::SoundArchive> archive = OpenTestArchive();
    const auto sequence = std::make_shared<const Sequence>(ReadSequence(*archive, kLoopSound));
    const ModelRun model = RunModel(*archive, kLoopSound, 600);
    const std::size_t first = FirstFrameReaching(model, kStart + 1);
    const auto clock = std::make_shared<PlaybackClock>(kFrameSamples);
    clock->SetCycle(kGameLoop);
    SequencePlayer player(ModelOf(archive, sequence), kStart, clock);

    const PlayerRun run = RunPlayer(player, *clock, 300);

    SEQLIB_REQUIRE_EQ(run.samples.size(), 300 * kFrameSamples * 2);
    SEQLIB_CHECK(HasSound(std::span<const float>(run.samples).first(kFrameSamples * 2)));
    SEQLIB_CHECK(Fades(run.samples, 0, model, first, true));
    SEQLIB_CHECK(Matches(run.samples, model, first, kFrameSamples * 2));
    SEQLIB_REQUIRE(!run.marks.empty() && run.marks[0].has_value());
    SEQLIB_CHECK(*run.marks[0] == kStart);
}

// With the loop off, the output ends with the frame that reaches the end of the pass, and that frame fades out. Turned
// off during the second pass, the loop ends the output with that pass.
void TestLoopOff()
{
    const std::shared_ptr<const sar::SoundArchive> archive = OpenTestArchive();
    const auto sequence = std::make_shared<const Sequence>(ReadSequence(*archive, kLoopSound));
    const ModelRun model = RunModel(*archive, kLoopSound, 800);
    const std::size_t first = FirstFrameReaching(model, 1);
    const std::size_t first_pass_end = FirstFrameReaching(model, 192);
    const std::size_t second_pass_end = FirstFrameReaching(model, 288);

    const auto clock = std::make_shared<PlaybackClock>(kFrameSamples);
    SequencePlayer player(ModelOf(archive, sequence), 0, clock);
    const PlayerRun run = RunPlayer(player, *clock, 800);

    const auto later_clock = std::make_shared<PlaybackClock>(kFrameSamples);
    later_clock->SetCycle(kGameLoop);
    SequencePlayer later_player(ModelOf(archive, sequence), 0, later_clock);
    const PlayerRun first_part = RunPlayer(later_player, *later_clock, first_pass_end - first + 50);
    later_clock->SetCycle({});
    const PlayerRun second_part = RunPlayer(later_player, *later_clock, 800);

    const std::size_t frames = first_pass_end - first + 1;
    SEQLIB_CHECK(run.ended);
    SEQLIB_CHECK(clock->Ended());
    SEQLIB_REQUIRE_EQ(run.samples.size(), frames * kFrameSamples * 2);
    std::vector<float> before_end = run.samples;
    before_end.resize((frames - 1) * kFrameSamples * 2);
    SEQLIB_CHECK(Matches(before_end, model, first, 0));
    SEQLIB_CHECK(Fades(run.samples, frames - 1, model, first_pass_end, false));

    SEQLIB_CHECK(!first_part.ended);
    SEQLIB_CHECK(second_part.ended);
    SEQLIB_CHECK(later_clock->Ended());
    SEQLIB_CHECK_EQ((first_part.samples.size() + second_part.samples.size()) / 2,
                    (second_pass_end - first + 1) * kFrameSamples);
}

// A sequence without a loop ends once its last note has faded: when the model is no longer busy.
void TestPlayToEnd()
{
    const std::shared_ptr<const sar::SoundArchive> archive = OpenTestArchive();
    const auto sequence = std::make_shared<const Sequence>(ReadSequence(*archive, kNoteSound));
    const ModelRun model = RunModel(*archive, kNoteSound, 400);
    const std::size_t first = FirstFrameReaching(model, 1);
    const auto idle = std::find(model.busy.begin() + static_cast<std::ptrdiff_t>(first), model.busy.end(), false);
    const auto end = static_cast<std::size_t>(idle - model.busy.begin());
    const auto clock = std::make_shared<PlaybackClock>(kFrameSamples);
    SequencePlayer player(ModelOf(archive, sequence), 0, clock);

    const PlayerRun run = RunPlayer(player, *clock, 400);
    float after_end[2] = {};
    const std::size_t rendered_after_end = player.Render(after_end, 1);

    SEQLIB_CHECK(!sequence->loop.has_value());
    SEQLIB_CHECK(end < model.busy.size());
    SEQLIB_CHECK(run.ended);
    SEQLIB_CHECK(clock->Ended());
    SEQLIB_CHECK_EQ(run.samples.size(), (end - first) * kFrameSamples * 2);
    SEQLIB_CHECK(HasSound(std::span<const float>(run.samples)));
    SEQLIB_CHECK(Matches(run.samples, model, first, 0));
    SEQLIB_CHECK_EQ(rendered_after_end, 0);
}

// A sequence that the model can't start ends the output at once.
void TestUnplayable()
{
    const std::shared_ptr<const sar::SoundArchive> archive = OpenTestArchive();
    Sequence missing = ReadSequence(*archive, kNoteSound);
    missing.sound = kMissingSound;
    const auto clock = std::make_shared<PlaybackClock>(kFrameSamples);
    SequencePlayer player(ModelOf(archive, std::make_shared<const Sequence>(missing)), 0, clock);
    float out[2] = {};

    const std::size_t rendered = player.Render(out, 1);

    SEQLIB_CHECK_EQ(rendered, 0);
    SEQLIB_CHECK(clock->Ended());
}

// With the locators at ticks 48 and 144, inside the intro and the loop, playback from the start runs to the frame that
// reaches tick 144, and that frame fades out. The frame that plays tick 48 follows, faded in. Each pass from there is
// the model's output from that frame on, and the passes are the same. The position goes back to 48 at each pass.
void TestCycleLocators()
{
    const std::shared_ptr<const sar::SoundArchive> archive = OpenTestArchive();
    const auto sequence = std::make_shared<const Sequence>(ReadSequence(*archive, kLoopSound));
    const ModelRun model = RunModel(*archive, kLoopSound, 800);
    const std::size_t first = FirstFrameReaching(model, 1);
    const std::size_t left = FirstFrameReaching(model, 49);
    const std::size_t right = FirstFrameReaching(model, 144);
    const std::size_t first_pass = right - first + 1;
    const std::size_t pass = right - left + 1;
    const auto clock = std::make_shared<PlaybackClock>(kFrameSamples);
    clock->SetCycle({true, 48, 144});
    SequencePlayer player(ModelOf(archive, sequence), 0, clock);

    const PlayerRun run = RunPlayer(player, *clock, first_pass + 3 * pass);

    SEQLIB_REQUIRE_EQ(run.samples.size(), (first_pass + 3 * pass) * kFrameSamples * 2);
    SEQLIB_CHECK(Matches(FramesOf(run, 0, first_pass - 1), model, first, 0));
    SEQLIB_CHECK(Fades(run.samples, first_pass - 1, model, right, false));
    SEQLIB_CHECK(Fades(run.samples, first_pass, model, left, true));
    SEQLIB_CHECK(Matches(FramesOf(run, first_pass, pass - 1), model, left, kFrameSamples * 2));
    SEQLIB_CHECK(Fades(run.samples, first_pass + pass - 1, model, right, false));
    SEQLIB_CHECK(FramesOf(run, first_pass + pass, pass) == FramesOf(run, first_pass, pass));
    SEQLIB_CHECK(FramesOf(run, first_pass + 2 * pass, pass) == FramesOf(run, first_pass, pass));
    SEQLIB_REQUIRE(run.marks.size() > first_pass + pass && run.marks[first_pass] && run.marks[first_pass + pass]);
    SEQLIB_CHECK(*run.marks[first_pass] == 48.0 && *run.marks[first_pass + pass] == 48.0);
}

// The release sequence, with the right locator 100 ticks past its end. The position goes on past the end at the tempo
// there, through the note's release and the silence after it. The pass ends with the frame that reaches the right
// locator. No frame ends on the locator exactly. The cycle then goes back to the start. The second pass's first frame
// fades in, and after it the second pass repeats the first. Track 0's meter matches the output in each frame but the
// two that start the second and the third pass with a fade. In the silence, it's silent.
void TestCycleSilence()
{
    const std::shared_ptr<const sar::SoundArchive> archive = OpenTestArchive();
    const auto sequence = std::make_shared<const Sequence>(ReadSequence(*archive, kReleaseSound));
    const ModelRun model = RunModel(*archive, kReleaseSound, 600);
    const EndFrames ends = EndFramesOf(model);
    const uint32_t right = sequence->length + 100;
    const std::size_t second = FramePlayingPastEnd(model, ends, right) + 1 - ends.first;
    const auto clock = std::make_shared<PlaybackClock>(kFrameSamples);
    clock->SetCycle({true, 0, right});
    SequencePlayer player(ModelOf(archive, sequence), 0, clock);

    const PlayerRun run = RunPlayer(player, *clock, 2 * second + 10);

    const std::size_t sounding = ends.idle - ends.first;
    SEQLIB_CHECK(!run.ended);
    SEQLIB_REQUIRE(ends.idle < model.busy.size() && sounding < second && run.marks.size() == 2 * second + 10);
    SEQLIB_CHECK(Matches(FramesOf(run, 0, sounding), model, ends.first, 0));
    SEQLIB_CHECK(!HasSound(std::span<const float>(FramesOf(run, sounding, second - sounding))));
    SEQLIB_CHECK(run.marks[second] && *run.marks[second] == 0.0);
    SEQLIB_CHECK(Fades(run.samples, second, model, ends.first, true));
    SEQLIB_CHECK(FramesOf(run, second + 1, second - 1) == FramesOf(run, 1, second - 1));
    SEQLIB_CHECK_EQ(MeterMismatches(run, 0), 2);
}

// With the locators at ticks 40 and 80, in the release sequence's release after its end at tick 24, each pass ends with
// the frame that reaches the right locator. That frame fades out. The next pass starts with the frame that plays the
// left locator, faded in, and goes on as the model does from there. No frame ends on a locator exactly. The model takes
// a snapshot every 10 frames up to the end. A snapshot in the release would hold the wrong position.
void TestCycleInRelease()
{
    constexpr PlaybackClock::CycleRange kCycle{true, 40, 80};
    const std::shared_ptr<const sar::SoundArchive> archive = OpenTestArchive();
    const auto sequence = std::make_shared<const Sequence>(ReadSequence(*archive, kReleaseSound));
    const ModelRun model = RunModel(*archive, kReleaseSound, 600);
    const EndFrames ends = EndFramesOf(model);
    const std::size_t left = FramePlayingPastEnd(model, ends, kCycle.start);
    const std::size_t right = FramePlayingPastEnd(model, ends, kCycle.end);
    const std::size_t first_pass = right - ends.first + 1;
    const std::size_t pass = right - left + 1;
    const auto clock = std::make_shared<PlaybackClock>(kFrameSamples);
    clock->SetCycle(kCycle);
    SequencePlayer player(ModelOf(archive, sequence, 10), 0, clock);

    const PlayerRun run = RunPlayer(player, *clock, first_pass + pass);

    SEQLIB_REQUIRE(ends.finished < left && right + 5 < ends.idle && run.marks.size() == first_pass + pass);
    SEQLIB_CHECK(Matches(FramesOf(run, 0, first_pass - 1), model, ends.first, 0));
    SEQLIB_CHECK(Fades(run.samples, first_pass - 1, model, right, false));
    SEQLIB_CHECK(Fades(run.samples, first_pass, model, left, true));
    SEQLIB_CHECK(Matches(FramesOf(run, first_pass, pass - 1), model, left, kFrameSamples * 2));
    SEQLIB_CHECK(Fades(run.samples, first_pass + pass - 1, model, right, false));
    SEQLIB_CHECK(run.marks[first_pass] && *run.marks[first_pass] == 40.0);
}

// Once the release sequence has ended, the position goes on at the tempo at the end while the note's release sounds.
// The output ends with the release.
void TestPositionPastEnd()
{
    const std::shared_ptr<const sar::SoundArchive> archive = OpenTestArchive();
    const auto sequence = std::make_shared<const Sequence>(ReadSequence(*archive, kReleaseSound));
    const ModelRun model = RunModel(*archive, kReleaseSound, 600);
    const EndFrames ends = EndFramesOf(model);
    const auto clock = std::make_shared<PlaybackClock>(kFrameSamples);
    SequencePlayer player(ModelOf(archive, sequence), 0, clock);

    const PlayerRun run = RunPlayer(player, *clock, 600);

    SEQLIB_REQUIRE(ends.first < ends.finished && ends.finished + 10 < ends.idle && ends.idle < model.busy.size());
    SEQLIB_REQUIRE_EQ(run.marks.size(), ends.idle - ends.first);
    std::size_t wrong = 0;
    for (std::size_t frame = ends.finished; frame < ends.idle; frame++)
    {
        const std::optional<double>& mark = run.marks[frame - ends.first];
        if (!mark || std::abs(*mark - PositionBefore(model, ends, frame)) > 1e-9)
        {
            wrong++;
        }
    }
    SEQLIB_CHECK_EQ(wrong, 0);
    SEQLIB_CHECK(run.ended);
}

// Playback that starts at tick 80, in the release sequence's release after its end at tick 24, plays the release from
// the frame that plays the start. That frame fades in. No frame ends on the start exactly.
void TestPlayFromRelease()
{
    constexpr uint32_t kStart = 80;
    const std::shared_ptr<const sar::SoundArchive> archive = OpenTestArchive();
    const auto sequence = std::make_shared<const Sequence>(ReadSequence(*archive, kReleaseSound));
    const ModelRun model = RunModel(*archive, kReleaseSound, 600);
    const EndFrames ends = EndFramesOf(model);
    const std::size_t from = FramePlayingPastEnd(model, ends, kStart);
    const auto clock = std::make_shared<PlaybackClock>(kFrameSamples);
    SequencePlayer player(ModelOf(archive, sequence), kStart, clock);

    const PlayerRun run = RunPlayer(player, *clock, 600);

    SEQLIB_REQUIRE(ends.finished < from && from + 5 < ends.idle && ends.idle < model.busy.size());
    SEQLIB_CHECK(run.ended);
    SEQLIB_CHECK_EQ(run.samples.size(), (ends.idle - from) * kFrameSamples * 2);
    SEQLIB_CHECK(HasSound(std::span<const float>(run.samples)));
    SEQLIB_CHECK(Fades(run.samples, 0, model, from, true));
    SEQLIB_CHECK(Matches(run.samples, model, from, kFrameSamples * 2));
    SEQLIB_CHECK(!run.marks.empty() && run.marks[0] == static_cast<double>(kStart));
}

// Playback that starts after the right locator plays on. The loop sequence's output ends with the pass of its loop.
void TestCycleAfterRightLocator()
{
    constexpr uint32_t kStart = 120;
    const std::shared_ptr<const sar::SoundArchive> archive = OpenTestArchive();
    const auto sequence = std::make_shared<const Sequence>(ReadSequence(*archive, kLoopSound));
    const ModelRun model = RunModel(*archive, kLoopSound, 800);
    const std::size_t first = FirstFrameReaching(model, kStart + 1);
    const std::size_t pass_end = FirstFrameReaching(model, 192);
    const auto clock = std::make_shared<PlaybackClock>(kFrameSamples);
    clock->SetCycle({true, 48, 96});
    SequencePlayer player(ModelOf(archive, sequence), kStart, clock);

    const PlayerRun run = RunPlayer(player, *clock, 800);

    SEQLIB_CHECK(run.ended);
    SEQLIB_CHECK_EQ(run.samples.size(), (pass_end - first + 1) * kFrameSamples * 2);
}

// A muted track plays on. With track 0 muted from the model's start, the loop sequence's output is silent, and its
// positions are those of the playback without the mute. Unmuted at tick 120, inside a note, the output ramps in across
// a frame and is then the model's output again. The sequence lacks track 5, and a mute of that track changes nothing. A
// seek keeps the mutes.
void TestMutes()
{
    constexpr std::size_t kMuted = 250;
    const std::shared_ptr<const sar::SoundArchive> archive = OpenTestArchive();
    const auto sequence = std::make_shared<const Sequence>(ReadSequence(*archive, kLoopSound));
    const ModelRun model = RunModel(*archive, kLoopSound, 800);
    const std::size_t unmuted = FirstFrameReaching(model, 1) + kMuted;
    const std::shared_ptr<SequenceModel> shared = ModelOf(archive, sequence, 10);
    const auto player_from = [&shared](uint32_t start, uint32_t mutes)
    {
        auto clock = std::make_shared<PlaybackClock>(kFrameSamples);
        clock->SetCycle(kGameLoop);
        clock->SetMutes(mutes);

        return std::make_pair(std::make_unique<SequencePlayer>(shared, start, clock), clock);
    };

    const auto [muted, clock] = player_from(0, 1u << 0);
    const PlayerRun silent = RunPlayer(*muted, *clock, kMuted);
    clock->SetMutes(0);
    const PlayerRun back = RunPlayer(*muted, *clock, 100);
    const auto [plain, plain_clock] = player_from(0, 0);
    const PlayerRun heard = RunPlayer(*plain, *plain_clock, kMuted);
    const auto [other, other_clock] = player_from(0, 1u << 5);
    const PlayerRun other_track = RunPlayer(*other, *other_clock, kMuted);
    const auto [sought, sought_clock] = player_from(120, 1u << 0);
    const PlayerRun silent_seek = RunPlayer(*sought, *sought_clock, 100);

    SEQLIB_CHECK(HasSound(std::span<const float>(heard.samples)) && other_track.samples == heard.samples);
    SEQLIB_CHECK(!HasSound(std::span<const float>(silent.samples)) && silent.marks == heard.marks);
    SEQLIB_CHECK(HasSound(std::span<const float>(FramesOf(back, 0, 1))) && RampsIn(back.samples, 0, model, unmuted));
    SEQLIB_CHECK(Matches(back.samples, model, unmuted, kFrameSamples * 2));
    SEQLIB_CHECK(silent_seek.samples.size() == 100 * kFrameSamples * 2);
    SEQLIB_CHECK(!HasSound(std::span<const float>(silent_seek.samples)));
}

// Track 0's meter holds the peaks of the output in each frame from the start, and the other tracks' meters stay silent.
// From a seek, the first frame fades in. Its meter shows the frame before the fade, and the frames after it match the
// output again. A muted track's meter stays silent.
void TestTrackMeters()
{
    constexpr std::size_t kFrames = 700;
    const std::shared_ptr<const sar::SoundArchive> archive = OpenTestArchive();
    const auto sequence = std::make_shared<const Sequence>(ReadSequence(*archive, kLoopSound));
    const std::shared_ptr<SequenceModel> shared = ModelOf(archive, sequence, 10);
    const auto player_from = [&shared](uint32_t start, uint32_t mutes)
    {
        auto clock = std::make_shared<PlaybackClock>(kFrameSamples);
        clock->SetCycle(kGameLoop);
        clock->SetMutes(mutes);

        return std::make_pair(std::make_unique<SequencePlayer>(shared, start, clock), clock);
    };

    const auto [plain, plain_clock] = player_from(0, 0);
    const PlayerRun heard = RunPlayer(*plain, *plain_clock, kFrames);
    const auto [sought, sought_clock] = player_from(120, 0);
    const PlayerRun seek = RunPlayer(*sought, *sought_clock, kFrames);
    const auto [muted, muted_clock] = player_from(0, 1u << 0);
    const PlayerRun silent = RunPlayer(*muted, *muted_clock, kFrames);

    SEQLIB_REQUIRE_EQ(heard.peaks.size(), kFrames);
    SEQLIB_CHECK(HasSound(std::span<const float>(heard.samples)));
    SEQLIB_CHECK_EQ(MeterMismatches(heard, 0), 0);
    SEQLIB_CHECK(TracksSilent(heard, 1));
    SEQLIB_REQUIRE_EQ(seek.peaks.size(), kFrames);
    const std::array<float, 2> faded = OutputPeaks(seek.samples, 0);
    SEQLIB_CHECK(seek.peaks[0].tracks[0][0] > faded[0] && seek.peaks[0].tracks[0][1] > faded[1]);
    SEQLIB_CHECK_EQ(MeterMismatches(seek, 1), 0);
    SEQLIB_CHECK(TracksSilent(seek, 1));
    SEQLIB_CHECK_EQ(silent.peaks.size(), kFrames);
    SEQLIB_CHECK(TracksSilent(silent, 0));
}

// The meter of a track panned to the left is louder on the left in each frame with sound. Each side of the meter holds
// the peaks of that side of the output.
void TestPannedMeter()
{
    const std::shared_ptr<const sar::SoundArchive> archive = OpenTestArchive();
    const auto sequence = std::make_shared<const Sequence>(ReadSequence(*archive, kPanSound));
    const auto clock = std::make_shared<PlaybackClock>(kFrameSamples);
    SequencePlayer player(ModelOf(archive, sequence), 0, clock);

    const PlayerRun run = RunPlayer(player, *clock, 400);

    // The frames with sound, and those of them that aren't louder on the left.
    std::size_t sounding = 0;
    std::size_t not_left = 0;
    for (const PlaybackClock::Peaks& peaks : run.peaks)
    {
        if (peaks.tracks[0][0] == 0.0f && peaks.tracks[0][1] == 0.0f)
        {
            continue;
        }

        sounding++;
        if (peaks.tracks[0][0] <= peaks.tracks[0][1])
        {
            not_left++;
        }
    }

    SEQLIB_CHECK(run.ended);
    SEQLIB_CHECK(sounding > 0 && not_left == 0);
    SEQLIB_CHECK_EQ(MeterMismatches(run, 0), 0);
    SEQLIB_CHECK(TracksSilent(run, 1));
}

// A reverb on aux bus A plays as ntdlib's model plays it, given the same reverb, and its tail sounds on for seconds
// after the note's release. Playback ends at the first silent frame after that. A reverb on bus B changes nothing. The
// sequence doesn't send to bus B. A seek restores the reverb's state from a snapshot.
void TestAuxEffects()
{
    const std::shared_ptr<const sar::SoundArchive> archive = OpenSendArchive();
    const auto sequence = std::make_shared<const Sequence>(ReadSequence(*archive, kSendSound));
    const ModelRun model = RunModel(*archive, kSendSound, 1600, AuxReverb());
    const ModelRun dry = RunModel(*archive, kSendSound, 1600);
    const std::size_t first = FirstFrameReaching(model, 1);
    const std::size_t idle = EndFramesOf(dry).idle;
    const std::size_t end = TailEnd(model, first);

    const auto play = [&](std::shared_ptr<SequenceModel> shared, uint32_t start, std::size_t frames)
    {
        const auto clock = std::make_shared<PlaybackClock>(kFrameSamples);
        SequencePlayer player(std::move(shared), start, clock);
        return RunPlayer(player, *clock, frames);
    };

    const PlayerRun on_a =
        play(ModelOf(archive, sequence, SequenceModel::kSnapshotInterval, AuxEffectsWithReverb()), 0, 1500);
    const PlayerRun on_b =
        play(ModelOf(archive, sequence, SequenceModel::kSnapshotInterval, {{{}, {Reverb()}, {}}}), 0, 1500);

    SEQLIB_CHECK(Matches(on_a.samples, model, first, 0) && !Matches(on_a.samples, dry, first, 0));
    SEQLIB_CHECK(on_a.ended && end > idle + 200 && end < model.busy.size());
    SEQLIB_CHECK_EQ(on_a.samples.size(), (end - first) * kFrameSamples * 2);
    SEQLIB_CHECK(on_b.ended && Matches(on_b.samples, dry, first, 0));
    SEQLIB_CHECK(on_b.samples.size() < (idle + 10 - first) * kFrameSamples * 2);

    // Seeks into the note on a model with a snapshot every 10 frames.
    const auto shared = ModelOf(archive, sequence, 10, AuxEffectsWithReverb());
    play(shared, 0, 200);
    std::size_t wrong = 0;
    for (const uint32_t start : {30u, 10u, 45u})
    {
        const PlayerRun run = play(shared, start, 100);
        const std::size_t from = FirstFrameReaching(model, start + 1);
        if (!Fades(run.samples, 0, model, from, true) || !Matches(run.samples, model, from, kFrameSamples * 2))
        {
            wrong++;
        }
    }
    SEQLIB_CHECK_EQ(wrong, 0);
    SEQLIB_CHECK(shared->SnapshotCount() > 5);
}

// The peaks of each channel of what frame `frame` of `run` adds to the same frame of `without`, as amplitudes.
std::array<float, 2> DifferencePeaks(const ModelRun& run, const ModelRun& without, std::size_t frame)
{
    std::array<float, 2> peaks = {0.0f, 0.0f};
    for (std::size_t i = 0; i < kFrameSamples * 2; i++)
    {
        float& peak = peaks[i % 2];
        peak = std::max(peak, std::abs(SampleOf(run, frame, i) - SampleOf(without, frame, i)));
    }

    return peaks;
}

// A bus's volume scales what the bus adds to the output. With aux bus A at 0.5, the player plays the model run directly
// with sub-mix 0's channels of bus A at 0.5. It does so from the start, after a seek to a snapshot taken at full
// volume, and from the frame after a change during playback. A catch-up runs at the clock's volumes. A change that
// comes with a new model takes effect in the frame that crossfades to the new model, in both models. Bus A's meter
// holds the peaks of what the bus adds: the output at full volume less the output with the bus silent. At 0.5, it holds
// half of them, within a sample's rounding. The meters of buses B and C stay silent. The sequence doesn't send there.
void TestBusVolumes()
{
    constexpr AuxVolumes kHalf = {0.5f, 1.0f, 1.0f};
    constexpr std::array<float, 2> kSilent = {0.0f, 0.0f};
    const std::shared_ptr<const sar::SoundArchive> archive = OpenSendArchive();
    const auto sequence = std::make_shared<const Sequence>(ReadSequence(*archive, kSendSound));
    const ModelRun full = RunModel(*archive, kSendSound, 600, AuxReverb());
    const ModelRun half = RunModel(*archive, kSendSound, 600, AuxReverb(), {}, {}, kHalf);
    const ModelRun silent = RunModel(*archive, kSendSound, 600, AuxReverb(), {}, {}, {0.0f, 1.0f, 1.0f});
    const std::size_t first = FirstFrameReaching(full, 1);
    const auto shared = ModelOf(archive, sequence, 10, AuxEffectsWithReverb());
    const auto play = [&shared](uint32_t start, const AuxVolumes& volumes, std::size_t frames)
    {
        const auto clock = std::make_shared<PlaybackClock>(kFrameSamples);
        clock->SetBusVolumes(volumes);
        SequencePlayer player(shared, start, clock);
        return RunPlayer(player, *clock, frames);
    };

    const PlayerRun at_full = play(0, kFullVolumes, 400);
    const PlayerRun at_half = play(0, kHalf, 400);
    const PlayerRun sought = play(30, kHalf, 100);
    const auto clock = std::make_shared<PlaybackClock>(kFrameSamples);
    SequencePlayer player(shared, 0, clock);
    const PlayerRun before_change = RunPlayer(player, *clock, 100);
    clock->SetBusVolumes(kHalf);
    const PlayerRun after_change = RunPlayer(player, *clock, 100);
    const auto catch_up_clock = std::make_shared<PlaybackClock>(kFrameSamples);
    catch_up_clock->SetBusVolumes(kHalf);
    catch_up_clock->SetModelFrames(first + 50);
    ModelCatchUp catch_up;
    catch_up.Start(ModelOf(archive, sequence, 10, AuxEffectsWithReverb()), catch_up_clock);
    const std::shared_ptr<SequenceModel> caught_up = WaitForCatchUp(catch_up);
    const auto switch_clock = std::make_shared<PlaybackClock>(kFrameSamples);
    SequencePlayer switching(shared, 0, switch_clock);
    RunPlayer(switching, *switch_clock, 50);
    switch_clock->SetBusVolumes(kHalf);
    switch_clock->Offer(ModelOf(archive, sequence, 10, AuxEffectsWithReverb()));
    const PlayerRun switched = RunPlayer(switching, *switch_clock, 50);

    const std::size_t from = FirstFrameReaching(half, 31);
    SEQLIB_CHECK(Matches(at_full.samples, full, first, 0) && Matches(at_half.samples, half, first, 0));
    SEQLIB_CHECK(!Matches(at_half.samples, full, first, 0));
    SEQLIB_CHECK(Fades(sought.samples, 0, half, from, true) && Matches(sought.samples, half, from, kFrameSamples * 2));
    SEQLIB_CHECK(before_change.samples.size() == 100 * kFrameSamples * 2 &&
                 after_change.samples.size() == 100 * kFrameSamples * 2);
    SEQLIB_CHECK(Matches(before_change.samples, full, first, 0) && Matches(after_change.samples, half, first + 100, 0));
    SEQLIB_REQUIRE(caught_up != nullptr);
    SEQLIB_CHECK(caught_up->Frames() == first + 50 &&
                 std::ranges::equal(caught_up->Output(), ModelFrame(half, first + 49)));
    SEQLIB_CHECK(Crossfades(switched.samples, 0, half, half, first + 50) &&
                 Matches(switched.samples, half, first + 50, kFrameSamples * 2));
    SEQLIB_REQUIRE(at_full.peaks.size() == 400 && at_half.peaks.size() == 400);
    std::size_t sounding = 0;
    std::size_t wrong = 0;
    for (std::size_t frame = 0; frame < at_full.peaks.size(); frame++)
    {
        const std::array<float, 2> expected = DifferencePeaks(full, silent, first + frame);
        const PlaybackClock::Peaks& at = at_full.peaks[frame];
        const PlaybackClock::Peaks& halved = at_half.peaks[frame];
        const auto near_half = [&expected, &halved](std::size_t side)
        {
            return std::abs(halved.buses[0][side] - expected[side] * 0.5f) <= 1.0f / 32768.0f;
        };

        sounding += expected != kSilent ? 1 : 0;
        const bool right = at.buses[0] == expected && near_half(0) && near_half(1) && at.buses[1] == kSilent &&
                           at.buses[2] == kSilent && halved.buses[1] == kSilent && halved.buses[2] == kSilent;
        wrong += right ? 0 : 1;
    }
    SEQLIB_CHECK(sounding > 200);
    SEQLIB_CHECK_EQ(wrong, 0);
}

// Bus C takes its volume as bus A does, and its meter holds what it adds to the output. With the note sent to bus C and
// the reverb there, at 0.5, the player plays the model run directly with sub-mix 0's channels of bus C at 0.5. The
// clock keeps each bus's volume.
void TestBusCVolume()
{
    constexpr AuxVolumes kHalfC = {1.0f, 1.0f, 0.5f};
    constexpr AuxVolumes kEach = {0.25f, 0.5f, 2.0f};
    constexpr std::array<float, 2> kSilent = {0.0f, 0.0f};
    const std::shared_ptr<const sar::SoundArchive> archive = OpenSendArchive(kSendCBytecode);
    const auto sequence = std::make_shared<const Sequence>(ReadSequence(*archive, kSendSound));
    atk::EngineOptions options;
    options.aux_reverbs[2] = Reverb();
    const ModelRun full = RunModel(*archive, kSendSound, 400, options);
    const ModelRun half = RunModel(*archive, kSendSound, 400, options, {}, {}, kHalfC);
    const ModelRun silent = RunModel(*archive, kSendSound, 400, options, {}, {}, {1.0f, 1.0f, 0.0f});
    const std::size_t first = FirstFrameReaching(full, 1);
    const auto clock = std::make_shared<PlaybackClock>(kFrameSamples);
    clock->SetBusVolumes(kHalfC);
    SequencePlayer player(ModelOf(archive, sequence, 10, {{{}, {}, {Reverb()}}}), 0, clock);
    PlaybackClock each(kFrameSamples);

    const PlayerRun run = RunPlayer(player, *clock, 300);
    each.SetBusVolumes(kEach);

    SEQLIB_CHECK(Matches(run.samples, half, first, 0) && !Matches(run.samples, full, first, 0));
    SEQLIB_REQUIRE(run.peaks.size() == 300);
    std::size_t sounding = 0;
    std::size_t wrong = 0;
    for (std::size_t frame = 0; frame < run.peaks.size(); frame++)
    {
        const std::array<float, 2> expected = DifferencePeaks(full, silent, first + frame);
        const PlaybackClock::Peaks& at = run.peaks[frame];
        const auto near_half = [&expected, &at](std::size_t side)
        {
            return std::abs(at.buses[2][side] - expected[side] * 0.5f) <= 1.0f / 32768.0f;
        };

        sounding += expected != kSilent ? 1 : 0;
        wrong += near_half(0) && near_half(1) && at.buses[0] == kSilent && at.buses[1] == kSilent ? 0 : 1;
    }
    SEQLIB_CHECK(sounding > 200);
    SEQLIB_CHECK_EQ(wrong, 0);
    SEQLIB_CHECK(each.BusVolumes() == kEach);
}

// On the Wii U, a bus's volume is its return volume on the TV. With aux bus A's effect at 0.5, the model plays as
// ntdlib's model plays with the TV's bus A returning at 0x4000. Bus A's meter holds half its peaks at full volume,
// within a sample's rounding. The meters of buses B and C stay silent. The quieter tail ends the output sooner.
void TestWiiUBusVolumes()
{
    constexpr std::size_t kFrames = 1500;
    const std::shared_ptr<const sar::SoundArchive> archive = OpenWiiUSendArchive();
    const auto sequence = std::make_shared<const Sequence>(ReadSequence(*archive, kNoteSound));
    const hokuto::song::WiiUEffect reverb = hokuto::song::WiiUEffectOf(
        cafe::EffectType::kReverbHi, hokuto::song::WiiUStartingParameters(cafe::EffectType::kReverbHi));
    const std::vector<cafe::EffectSetup> setups = {
        {cafe::kEffectDeviceTv, 0, cafe::EffectType::kReverbHi, reverb.words, {}}};
    const std::vector<int16_t> full = RunWiiUModel(*archive, kNoteSound, kFrames, setups);
    const std::vector<int16_t> half = RunWiiUModel(*archive, kNoteSound, kFrames, setups, 0x4000);

    const auto peaks_of = [&](const AuxVolumes& volumes)
    {
        const auto model = ModelOf(archive, sequence, 50, {{{reverb}, {}, {}}});
        model->SetBusVolumes(volumes);
        std::vector<int16_t> played;
        std::vector<std::array<std::array<int32_t, 2>, kAuxBuses>> peaks;
        for (bool more = model->SeekFrame(0); more && played.size() < full.size(); more = model->RunFrame())
        {
            const std::span<const int16_t> output = model->Output();
            played.insert(played.end(), output.begin(), output.end());
            peaks.push_back({model->BusPeaks(0), model->BusPeaks(1), model->BusPeaks(2)});
        }

        return std::make_pair(played, peaks);
    };

    const auto [played_full, peaks_full] = peaks_of(kFullVolumes);
    const auto [played_half, peaks_half] = peaks_of({0.5f, 1.0f, 1.0f});

    SEQLIB_REQUIRE(played_half.size() > 400 * cafe::ArchiveModel::kFrameSamples * 2 &&
                   played_half.size() < half.size());
    SEQLIB_CHECK(played_half.size() < played_full.size());
    SEQLIB_CHECK(std::equal(played_full.begin(), played_full.end(), full.begin()));
    SEQLIB_CHECK(std::equal(played_half.begin(), played_half.end(), half.begin()));
    SEQLIB_CHECK(!std::equal(played_half.begin(), played_half.end(), full.begin()));
    std::size_t sounding = 0;
    std::size_t wrong = 0;
    constexpr std::array<int32_t, 2> kSilent = {0, 0};
    for (std::size_t frame = 0; frame < std::min(peaks_full.size(), peaks_half.size()); frame++)
    {
        const std::array<int32_t, 2>& at_full = peaks_full[frame][0];
        const std::array<int32_t, 2>& at_half = peaks_half[frame][0];
        sounding += at_full != kSilent ? 1 : 0;
        const bool halved = std::abs(2 * at_half[0] - at_full[0]) <= 2 && std::abs(2 * at_half[1] - at_full[1]) <= 2;
        const bool quiet = peaks_full[frame][1] == kSilent && peaks_full[frame][2] == kSilent &&
                           peaks_half[frame][1] == kSilent && peaks_half[frame][2] == kSilent;
        wrong += halved && quiet ? 0 : 1;
    }
    SEQLIB_CHECK(sounding > 300);
    SEQLIB_CHECK_EQ(wrong, 0);
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("player",
                                    []
                                    {
                                        TestPlaybackClock();
                                        TestPositionAtReplaced();
                                        TestPeaksBetween();
                                        TestVariablesAt();
                                        TestPlayFromStart();
                                        TestPlayVariables();
                                        TestPlayChanges();
                                        TestSeekAcrossChange();
                                        TestPlayFromSeek();
                                        TestLoopOff();
                                        TestPlayToEnd();
                                        TestUnplayable();
                                        TestCycleLocators();
                                        TestCycleSilence();
                                        TestCycleInRelease();
                                        TestPositionPastEnd();
                                        TestPlayFromRelease();
                                        TestCycleAfterRightLocator();
                                        TestMutes();
                                        TestTrackMeters();
                                        TestPannedMeter();
                                        TestAuxEffects();
                                        TestBusVolumes();
                                        TestBusCVolume();
                                        TestWiiUBusVolumes();
                                    });
}
