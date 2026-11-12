// SPDX-License-Identifier: MIT

// Tests the model of a sequence (src/song/sequence_model.cpp): seeks, meters, frames and mixing.

#include <cstdio>

#include "played_fixtures.h"
#include "song/dse.h"
#include "song/gba.h"
#include "song/krawall.h"
#include "song/played.h"
#include "song_runs.h"

namespace
{

// The note with its main send at 0 (0xDB with 0) and its send to aux bus A at 127, after a wait of 12 ticks (0x80 with
// 12). Only bus A's return reaches the output.
const std::vector<uint8_t> kWiiWetBytecode = {0xDB, 0, 0xD9, 127, 0x80, 12, 0x3C, 0x64, 0x30, 0xFF};

// The peaks of a frame of a model's output, left and right.
std::array<int32_t, 2> FramePeaks(std::span<const int16_t> frame)
{
    std::array<int32_t, 2> peaks = {0, 0};
    for (std::size_t i = 0; i < frame.size(); i++)
    {
        int32_t& peak = peaks[i % 2];
        peak = std::max(peak, std::abs(static_cast<int32_t>(frame[i])));
    }

    return peaks;
}

// A run of a model's meters: the frames in which a meter differs from the peaks of the output, the frames with sound,
// and the frames in which another meter isn't silent.
struct MeterRun
{
    std::size_t wrong = 0;
    std::size_t sounding = 0;
    std::size_t noisy = 0;
};

// Runs `model` from frame `first`, for `frames` frames at most. In each frame, the meter of track `track`, or else of
// aux bus `bus`, has to hold the output's peaks. Every other meter has to stay silent. With neither, the output has to
// stay silent too.
MeterRun RunMeters(SequenceModel& model, uint64_t first, std::size_t frames, int track, int bus = -1)
{
    constexpr std::array<int32_t, 2> kSilent = {0, 0};
    MeterRun run;
    std::size_t frame = 0;
    for (bool more = model.SeekFrame(first); more && frame < frames; more = model.RunFrame(), frame++)
    {
        const std::array<int32_t, 2> peaks = FramePeaks(model.Output());
        const std::array<int32_t, 2> meter = track >= 0 ? model.TrackPeaks(track) : model.BusPeaks(bus);
        bool quiet = true;
        for (int other = 0; other < SequenceModel::kTracks; other++)
        {
            quiet = quiet && (other == track || model.TrackPeaks(other) == kSilent);
        }
        for (int other = 0; other < static_cast<int>(kAuxBuses); other++)
        {
            quiet = quiet && (other == bus || model.BusPeaks(other) == kSilent);
        }
        run.sounding += peaks != kSilent ? 1 : 0;
        run.wrong += meter == peaks ? 0 : 1;
        run.noisy += quiet ? 0 : 1;
    }

    return run;
}

// On the Wii, the note plays on the main bus alone, and track 0's meter holds the peaks of the output in each frame,
// from the start and after a seek. The other meters stay silent. Once the track is muted, its meter stays at 0. The
// output fades meanwhile. With the note's main send at 0, and no effect on bus A, the output and the meters stay
// silent. With the high-quality reverb on bus A, bus A's meter holds the peaks of the output in each frame, at full
// volume and at half volume, and the other meters stay silent. The reverb's crosstalk puts sound on the surround
// channel. The surround channel reaches the left and right a frame later. The model takes a snapshot every 50 frames
// while the sequence plays, and a seek back to each frame from 205 to 45 starts from one: the meter holds the output's
// peaks there too.
void TestWiiMeters()
{
    constexpr std::size_t kFrames = 700;
    const std::shared_ptr<const ntdlib::rsar::SoundArchive> archive = OpenWiiArchive();
    const auto sequence = std::make_shared<const Sequence>(ReadSequence(*archive, 0));
    const auto model =
        std::make_shared<SequenceModel>(archive, sequence, hokuto::song::ModelOptions{.snapshot_interval = 50});
    const MeterRun heard = RunMeters(*model, 0, kFrames, 0);
    const MeterRun sought = RunMeters(*model, 70, 100, 0);
    const MeterRun before_mute = RunMeters(*model, 0, 100, 0);
    model->SetMutes(1);
    const MeterRun muted = RunMeters(*model, 100, kFrames, -1);

    const std::shared_ptr<const ntdlib::rsar::SoundArchive> wet = OpenWiiArchive(kWiiWetBytecode);
    const auto wet_sequence = std::make_shared<const Sequence>(ReadSequence(*wet, 0));
    const auto dry =
        std::make_shared<SequenceModel>(wet, wet_sequence, hokuto::song::ModelOptions{.snapshot_interval = 50});
    const MeterRun silent = RunMeters(*dry, 0, kFrames, -1);
    const hokuto::song::WiiEffect reverb =
        hokuto::song::WiiEffectOf(ntdlib::rvl::snd::EffectType::kReverbHi2008,
                                  hokuto::song::WiiStartingParameters(ntdlib::rvl::snd::EffectType::kReverbHi2008));
    const auto full = std::make_shared<SequenceModel>(
        wet, wet_sequence,
        hokuto::song::ModelOptions{.effects = AuxEffects{{{reverb}, {}, {}}}, .snapshot_interval = 50});
    const MeterRun returned = RunMeters(*full, 0, kFrames, -1, 0);
    std::size_t seeks = 0;
    for (uint64_t frame = 205; frame >= 45; frame--)
    {
        const MeterRun seek = RunMeters(*full, frame, 1, -1, 0);
        seeks += seek.wrong == 0 && seek.noisy == 0 ? 1 : 0;
    }
    const auto half = std::make_shared<SequenceModel>(
        wet, wet_sequence,
        hokuto::song::ModelOptions{.effects = AuxEffects{{{reverb}, {}, {}}}, .snapshot_interval = 50});
    half->SetBusVolumes({0.5f, 1.0f, 1.0f});
    const MeterRun halved = RunMeters(*half, 0, kFrames, -1, 0);
    const bool quieter = half->SeekFrame(300) && full->SeekFrame(300) && half->BusPeaks(0)[0] < full->BusPeaks(0)[0];

    SEQLIB_CHECK(heard.sounding > 600 && heard.wrong == 0 && heard.noisy == 0);
    SEQLIB_CHECK(sought.sounding == 100 && sought.wrong == 0 && sought.noisy == 0);
    SEQLIB_CHECK(before_mute.sounding > 90 && before_mute.wrong == 0 && before_mute.noisy == 0);
    SEQLIB_CHECK(muted.sounding > 0 && muted.noisy == 0);
    SEQLIB_CHECK(silent.sounding == 0 && silent.wrong == 0 && silent.noisy == 0);
    SEQLIB_CHECK(returned.sounding > 500 && returned.wrong == 0 && returned.noisy == 0);
    SEQLIB_CHECK_EQ(full->SnapshotCount(), 5);
    SEQLIB_CHECK_EQ(seeks, 161);
    SEQLIB_CHECK(halved.sounding > 500 && halved.wrong == 0 && halved.noisy == 0);
    SEQLIB_CHECK(quieter);
}

// A model with a snapshot every 10 frames plays the loop sequence past the end of its first pass. Seeks back into the
// intro, into the loop and to the start then restore snapshots. After the first frame's fade, each seek's output is the
// output of the model run directly. Snapshots cover the first pass only.
void TestSeekWithSnapshots()
{
    const std::shared_ptr<const sar::SoundArchive> archive = OpenTestArchive();
    const auto sequence = std::make_shared<const Sequence>(ReadSequence(*archive, kLoopSound));
    const ModelRun model = RunModel(*archive, kLoopSound, 800);
    const std::shared_ptr<SequenceModel> shared = ModelOf(archive, sequence, 10);
    const auto clock = std::make_shared<PlaybackClock>(kFrameSamples);
    clock->SetCycle(kGameLoop);
    SequencePlayer from_start(shared, 0, clock);
    RunPlayer(from_start, *clock, 600);

    std::size_t wrong = 0;
    for (const uint32_t start : {150u, 40u, 120u, 0u, 191u})
    {
        const auto seek_clock = std::make_shared<PlaybackClock>(kFrameSamples);
        seek_clock->SetCycle(kGameLoop);
        SequencePlayer player(shared, start, seek_clock);
        const PlayerRun run = RunPlayer(player, *seek_clock, 100);
        const std::size_t first = FirstFrameReaching(model, start + 1);
        const bool right = start > 0 ? Fades(run.samples, 0, model, first, true) &&
                                           Matches(run.samples, model, first, kFrameSamples * 2)
                                     : Matches(run.samples, model, first, 0);
        if (!right || run.samples.size() != 100 * kFrameSamples * 2)
        {
            wrong++;
        }
    }

    SEQLIB_CHECK_EQ(wrong, 0);
    SEQLIB_CHECK(shared->SnapshotCount() > 30 && shared->SnapshotCount() < 45);
}

// A seek to a frame runs the model up to that frame: from its start, from a snapshot, from where it is, or not at all
// when its last frame is the one. Its output is then that frame of the model run directly. A seek past the end of the
// output fails.
void TestSeekFrame()
{
    const std::shared_ptr<const sar::SoundArchive> archive = OpenTestArchive();
    const auto sequence = std::make_shared<const Sequence>(ReadSequence(*archive, kReleaseSound));
    const ModelRun model = RunModel(*archive, kReleaseSound, 600);
    const EndFrames ends = EndFramesOf(model);
    const std::shared_ptr<SequenceModel> shared = ModelOf(archive, sequence, 10);

    std::size_t wrong = 0;
    for (const uint64_t frame : {120u, 35u, 35u, 36u, 35u, 0u, 200u})
    {
        const bool plays = shared->SeekFrame(frame);
        if (!plays || shared->Frames() != frame + 1 || !std::ranges::equal(shared->Output(), ModelFrame(model, frame)))
        {
            wrong++;
        }
    }
    const bool past_end = shared->SeekFrame(ends.idle + 10);

    SEQLIB_REQUIRE(ends.idle > 200 && ends.idle + 10 < model.busy.size());
    SEQLIB_CHECK_EQ(wrong, 0);
    SEQLIB_CHECK(!past_end);
    SEQLIB_CHECK(shared->SnapshotCount() > 3);
}

// The lead lasts 0.5 s, or as long as the longest tail of the effects takes to fall by 60 dB, up to 20 s. Without
// effects, it lasts 100 frames of 5 ms. nn::audio's I3DL2 preset 1 falls in 1.7492 s: 350 frames. On the Wii U, an
// effect's decay counts a quarter longer. A high reverb with a pre-delay of 0.5 s and a fused time of 4 s then falls in
// 5.5 s: 1,834 frames of 3 ms. A delay of 500 ms that halves its echoes falls in 1.25 * 1.5 s / log10(2), about 6.23 s:
// 2,077 frames. A delay whose echoes never fall gives 20 s: 6,667 frames. With a chorus, every frame mixes. A model is
// in the lead before frame `frame` from frame `frame` + 1 - the lead on.
void TestLeadFrames()
{
    const std::shared_ptr<const sar::SoundArchive> archive = OpenTestArchive();
    const auto sequence = std::make_shared<const Sequence>(ReadSequence(*archive, kLoopSound));
    const std::shared_ptr<const sar::SoundArchive> wii_u = OpenWiiULoopArchive();
    const auto wii_u_loop = std::make_shared<const Sequence>(ReadSequence(*wii_u, kLoopSound));
    std::vector<float> reverb = hokuto::song::WiiUStartingParameters(cafe::EffectType::kReverbHi);
    reverb[0] = 0.5f;
    reverb[1] = 4.0f;
    std::vector<float> delay = hokuto::song::WiiUStartingParameters(cafe::EffectType::kDelay);
    delay[0] = 500.0f;
    delay[1] = 0.5f;
    delay[3] = 500.0f;
    std::vector<float> endless = delay;
    endless[1] = 1.0f;
    const std::vector<float> chorus = hokuto::song::WiiUStartingParameters(cafe::EffectType::kChorus);

    const auto lead = [&wii_u, &wii_u_loop](cafe::EffectType type, const std::vector<float>& parameters)
    {
        const hokuto::song::WiiUEffect effect = hokuto::song::WiiUEffectOf(type, parameters);
        return ModelOf(wii_u, wii_u_loop, SequenceModel::kSnapshotInterval, {{{effect}, {}, {}}})->LeadFrames();
    };

    const std::shared_ptr<SequenceModel> model = ModelOf(archive, sequence);
    const bool from_start = model->InLead(99) && !model->InLead(100);
    const bool sought = model->SeekFrame(0);
    const bool after_first = model->InLead(100) && !model->InLead(101);
    const bool by_ticks = model->InLeadOfTick(40.0) && !model->InLeadOfTick(1000.0);
    const auto with_chorus = ModelOf(wii_u, wii_u_loop, SequenceModel::kSnapshotInterval,
                                     {{{hokuto::song::WiiUEffectOf(cafe::EffectType::kChorus, chorus)}, {}, {}}});

    SEQLIB_CHECK_EQ(model->LeadFrames(), 100);
    SEQLIB_CHECK_EQ(ModelOf(archive, sequence, SequenceModel::kSnapshotInterval, AuxEffectsWithReverb())->LeadFrames(),
                    350);
    SEQLIB_CHECK_EQ(lead(cafe::EffectType::kReverbHi, reverb), 1834);
    SEQLIB_CHECK_EQ(lead(cafe::EffectType::kDelay, delay), 2077);
    SEQLIB_CHECK_EQ(lead(cafe::EffectType::kDelay, endless), 6667);
    SEQLIB_CHECK(with_chorus->LeadFrames() == std::numeric_limits<uint64_t>::max() && with_chorus->InLead(1'000'000) &&
                 with_chorus->InLeadOfTick(1'000'000.0));
    SEQLIB_CHECK(from_start && sought && after_first && by_ticks);
}

// Frames without mixing play the sequence in silence. The loop's sequence plays from its start with the mixing off from
// frame 1 to frame 299. Its position follows the model run directly, and its output and meters stay silent. The mixing
// then comes on again. Once the lead's 100 frames have mixed, the output is the model's. The mixing then goes off once
// more. A seek to the model's last frame leaves the model where it is, and turns the mixing on: the next frame is the
// model's.
void TestFramesWithoutMixing()
{
    constexpr std::size_t kStretchEnd = 300;
    constexpr std::size_t kFrames = 1050;
    constexpr std::array<int32_t, 2> kSilent = {0, 0};
    const std::shared_ptr<const sar::SoundArchive> archive = OpenTestArchive();
    const auto sequence = std::make_shared<const Sequence>(ReadSequence(*archive, kLoopSound));
    const ModelRun model = RunModel(*archive, kLoopSound, kFrames + 1);
    const std::shared_ptr<SequenceModel> shared = ModelOf(archive, sequence);

    bool silent = shared->SeekFrame(0);
    shared->SetMixing(false);
    std::size_t moved_wrong = 0;
    for (std::size_t frame = 1; frame < kStretchEnd; frame++)
    {
        silent = silent && shared->RunFrame() && !HasSound(shared->Output()) && shared->TrackPeaks(0) == kSilent;
        moved_wrong += shared->After() == static_cast<double>(model.after[frame]) ? 0 : 1;
    }
    shared->SetMixing(true);
    const uint64_t settled = kStretchEnd + shared->LeadFrames();
    std::size_t wrong = 0;
    for (std::size_t frame = kStretchEnd; frame < kFrames; frame++)
    {
        const bool ran = shared->RunFrame();
        wrong += !ran || (frame >= settled && !std::ranges::equal(shared->Output(), ModelFrame(model, frame))) ? 1 : 0;
    }
    shared->SetMixing(false);
    const bool stayed = shared->SeekFrame(kFrames - 1) && shared->Frames() == kFrames;
    const bool mixes = shared->RunFrame() && std::ranges::equal(shared->Output(), ModelFrame(model, kFrames));

    SEQLIB_REQUIRE(HasSound(
        std::span<const int16_t>(model.samples).subspan(kFrameSamples * 2, (kStretchEnd - 1) * kFrameSamples * 2)));
    SEQLIB_REQUIRE(HasSound(ModelFrame(model, kFrames)));
    SEQLIB_CHECK_EQ(shared->LeadFrames(), 100);
    SEQLIB_CHECK(silent);
    SEQLIB_CHECK_EQ(moved_wrong, 0);
    SEQLIB_CHECK_EQ(wrong, 0);
    SEQLIB_CHECK(stayed && mixes);
}

// The lowest ratio of signal to difference, in dB, of 20 frames of `model` from each of `frames`, after a seek to it,
// against the same frames of `run`. A seek that fails, or an output that ends sooner, gives minus infinity.
double SeekRatio(SequenceModel& model, const ModelRun& run, std::initializer_list<uint64_t> frames)
{
    constexpr std::size_t kSamples = 20 * kFrameSamples * 2;
    double lowest = std::numeric_limits<double>::infinity();
    for (const uint64_t frame : frames)
    {
        std::vector<int16_t> played;
        for (bool more = model.SeekFrame(frame); more && played.size() < kSamples; more = model.RunFrame())
        {
            played.insert(played.end(), model.Output().begin(), model.Output().end());
        }
        const std::size_t at = frame * kFrameSamples * 2;
        if (played.size() != kSamples || at + kSamples > run.samples.size())
        {
            return -std::numeric_limits<double>::infinity();
        }
        lowest =
            std::min(lowest, SignalToDifference(played, std::span<const int16_t>(run.samples).subspan(at, kSamples)));
    }

    return lowest;
}

// A seek far from the snapshots runs the frames before the lead without mixing. The loop's sequence seeks to frame 1450
// from its start, with a snapshot every 10 frames of its first pass. The frames before frame 1351 don't mix, and the
// lead's 100 frames do. The output is then the model's run directly, at that frame and the 19 after it. So it is after
// seeks back: to frame 1050, past the first pass's snapshots, and to frame 250, from a snapshot that didn't mix. A seek
// to tick 700 gives the frame that plays it. With nn::audio's I3DL2 preset 1 on the send loop's sequence, the lead
// lasts 350 frames. The reverb's tail falls by 60 dB in them. After the same seeks, the output lies 60 dB or more below
// the model's. A model of that sequence that has run 300 frames without mixing seeks to frame 400, with its mixing
// still off. The seek restores a snapshot before the lead and mixes the lead, and the frames after the seek mix. The
// output then lies 60 dB or more below the model's too.
void TestSeekWithoutMixing()
{
    constexpr std::size_t kFrames = 1500;
    const std::shared_ptr<const sar::SoundArchive> archive = OpenTestArchive();
    const auto sequence = std::make_shared<const Sequence>(ReadSequence(*archive, kLoopSound));
    const ModelRun model = RunModel(*archive, kLoopSound, kFrames);
    const std::shared_ptr<const sar::SoundArchive> send_archive = OpenSendArchive(kSendLoopBytecode);
    const auto send = std::make_shared<const Sequence>(ReadSequence(*send_archive, kSendSound));
    const ModelRun wet = RunModel(*send_archive, kSendSound, kFrames, AuxReverb());
    const ModelRun dry = RunModel(*send_archive, kSendSound, kFrames);

    const double exact = SeekRatio(*ModelOf(archive, sequence, 10), model, {1450, 1050, 250});
    const std::shared_ptr<SequenceModel> by_ticks = ModelOf(archive, sequence, 10);
    const bool sought = by_ticks->Seek(700);
    const std::size_t tick_frame = FirstFrameReaching(model, 701);
    const double reverb = SeekRatio(*ModelOf(send_archive, send, 10, AuxEffectsWithReverb()), wet, {1450, 1050, 250});
    const std::shared_ptr<SequenceModel> unsettled = ModelOf(send_archive, send, 10, AuxEffectsWithReverb());
    bool ran = unsettled->SeekFrame(0);
    unsettled->SetMixing(false);
    for (int frame = 1; frame < 301; frame++)
    {
        ran = ran && unsettled->RunFrame();
    }
    const double after_unmixed = SeekRatio(*unsettled, wet, {400});

    SEQLIB_REQUIRE(HasSound(ModelFrame(model, 1450)) && HasSound(ModelFrame(model, 1050)) &&
                   HasSound(ModelFrame(model, 250)));
    SEQLIB_REQUIRE(!std::ranges::equal(ModelFrame(wet, 1450), ModelFrame(dry, 1450)));
    SEQLIB_CHECK(std::isinf(exact) && exact > 0.0);
    SEQLIB_CHECK(sought && by_ticks->Frames() == tick_frame + 1 &&
                 std::ranges::equal(by_ticks->Output(), ModelFrame(model, tick_frame)));
    SEQLIB_CHECK(reverb >= 60.0);
    SEQLIB_CHECK(ran && after_unmixed >= 60.0);
}

// A tempo of 0 stops the player's clock with a note sounding: the sequence turns the note wait off, and sets a tempo of
// 0 at tick 24. The model plays on without ticks. Like a catch-up without a clock, a seek past the tick and a catch-up
// in ticks stop there. A catch-up in frames runs to its frame.
void TestStoppedClock()
{
    const std::vector<uint8_t> kStopping = {0xB0, 48,   0xE1, 120, 0x00, 0xC7, 0,  60,   100, 96,  0x80,
                                            24,   0xE1, 0,    0,   60,   100,  48, 0x80, 48,  0xFF};
    const std::shared_ptr<const sar::SoundArchive> archive = OpenSendArchive(kStopping);
    const auto sequence = std::make_shared<const Sequence>(ReadSequence(*archive, kSendSound));
    const auto by_ticks = std::make_shared<PlaybackClock>(kFrameSamples);
    by_ticks->SetModelTicks(200.0);
    const auto by_frames = std::make_shared<PlaybackClock>(kFrameSamples);
    by_frames->SetModelFrames(300);

    const auto sought = ModelOf(archive, sequence);
    const bool reached = sought->Seek(200);
    ModelCatchUp ticks_catch_up;
    ticks_catch_up.Start(ModelOf(archive, sequence), by_ticks, true);
    const std::shared_ptr<SequenceModel> in_ticks = WaitForCatchUp(ticks_catch_up);
    ModelCatchUp pass_catch_up;
    pass_catch_up.Start(ModelOf(archive, sequence), nullptr);
    const std::shared_ptr<SequenceModel> through_pass = WaitForCatchUp(pass_catch_up);
    ModelCatchUp frames_catch_up;
    frames_catch_up.Start(ModelOf(archive, sequence), by_frames);
    const std::shared_ptr<SequenceModel> in_frames = WaitForCatchUp(frames_catch_up);

    SEQLIB_CHECK(!reached && sought->ClockStopped() && sought->After() <= 25.0);
    SEQLIB_REQUIRE(in_ticks != nullptr && through_pass != nullptr && in_frames != nullptr);
    SEQLIB_CHECK(in_ticks->ClockStopped() && in_ticks->After() <= 25.0);
    SEQLIB_CHECK(through_pass->ClockStopped() && through_pass->After() <= 25.0);
    SEQLIB_CHECK(in_frames->ClockStopped() && in_frames->Frames() == 300 && HasSound(in_frames->Output()));
}

// On the Wii U, a seek far from the snapshots runs the frames before the lead without mixing too. The lead lasts 0.5 s:
// 167 frames of 3 ms. The loop's sequence seeks to frame 2417 from its start, with a snapshot every 50 frames of its
// first pass. It then seeks back to frame 1417, past the first pass's snapshots, and to frame 100. The output is the
// model's run directly, at each frame and the 19 after it. So it is in enhanced mixing.
void TestWiiUSeekWithoutMixing()
{
    constexpr std::size_t kWiiUFrame = cafe::ArchiveModel::kFrameSamples * 2;
    constexpr std::size_t kSamples = 20 * kWiiUFrame;
    const std::shared_ptr<const sar::SoundArchive> archive = OpenWiiULoopArchive();
    const auto sequence = std::make_shared<const Sequence>(ReadSequence(*archive, kLoopSound));

    std::size_t wrong = 0;
    std::size_t silent = 0;
    for (const Mixing mixing : {Mixing::kStrict, Mixing::kEnhanced})
    {
        const std::vector<int16_t> direct =
            RunWiiUModel(*archive, kLoopSound, 2500, {}, ntdlib::cafe::ax::kUnityVolume, mixing == Mixing::kEnhanced);
        const std::shared_ptr<SequenceModel> model = ModelOf(archive, sequence, 50, {}, mixing);
        for (const uint64_t frame : {2417u, 1417u, 100u})
        {
            std::vector<int16_t> played;
            for (bool more = model->SeekFrame(frame); more && played.size() < kSamples; more = model->RunFrame())
            {
                played.insert(played.end(), model->Output().begin(), model->Output().end());
            }
            SEQLIB_REQUIRE(direct.size() >= frame * kWiiUFrame + kSamples);
            const auto at = direct.begin() + static_cast<std::ptrdiff_t>(frame * kWiiUFrame);
            wrong += played.size() == kSamples && std::equal(played.begin(), played.end(), at) ? 0 : 1;
            silent += HasSound(std::span<const int16_t>(at, at + static_cast<std::ptrdiff_t>(kSamples))) ? 0 : 1;
        }
        wrong += model->LeadFrames() == 167 ? 0 : 1;
    }

    SEQLIB_REQUIRE_EQ(silent, 0);
    SEQLIB_CHECK_EQ(wrong, 0);
}

// In enhanced mixing, the player writes the output of ntdlib's model in its enhanced mode, run directly, in floating
// point: from the start, and from a seek that restores a snapshot, after the frame that fades in. The output differs
// from strict mixing. A model of the other mode takes over playback at the player's frame with a crossfade, as after a
// switch in the mixer.
void TestEnhancedMixing()
{
    const std::shared_ptr<const sar::SoundArchive> archive = OpenTestArchive();
    const auto sequence = std::make_shared<const Sequence>(ReadSequence(*archive, kLoopSound));
    atk::EngineOptions options;
    options.enhanced = true;
    const ModelRun enhanced = RunModel(*archive, kLoopSound, 800, options);
    const ModelRun strict = RunModel(*archive, kLoopSound, 800);
    const std::shared_ptr<SequenceModel> shared = ModelOf(archive, sequence, 10, {}, Mixing::kEnhanced);
    const auto clock = std::make_shared<PlaybackClock>(kFrameSamples);
    clock->SetCycle(kGameLoop);
    SequencePlayer from_start(shared, 0, clock);
    const auto seek_clock = std::make_shared<PlaybackClock>(kFrameSamples);
    seek_clock->SetCycle(kGameLoop);
    SequencePlayer from_seek(shared, 120, seek_clock);
    const auto switch_clock = std::make_shared<PlaybackClock>(kFrameSamples);
    switch_clock->SetCycle(kGameLoop);
    SequencePlayer switching(ModelOf(archive, sequence), 0, switch_clock);

    const PlayerRun run = RunPlayer(from_start, *clock, 300);
    const PlayerRun seek = RunPlayer(from_seek, *seek_clock, 100);
    const PlayerRun before = RunPlayer(switching, *switch_clock, 50);
    switch_clock->Offer(ModelOf(archive, sequence, SequenceModel::kSnapshotInterval, {}, Mixing::kEnhanced));
    const PlayerRun after = RunPlayer(switching, *switch_clock, 50);

    constexpr std::size_t kFrame = kFrameSamples * 2;
    const std::size_t first = FirstFrameReaching(enhanced, 1);
    const std::size_t seek_first = FirstFrameReaching(enhanced, 121);
    SEQLIB_REQUIRE(enhanced.floats.size() == enhanced.samples.size() && strict.floats.empty());
    SEQLIB_REQUIRE(run.samples.size() == 300 * kFrame && seek.samples.size() == 100 * kFrame);
    SEQLIB_REQUIRE(before.samples.size() == 50 * kFrame && after.samples.size() == 50 * kFrame);

    const auto from = [&enhanced](std::size_t model_frame)
    {
        return enhanced.floats.begin() + static_cast<std::ptrdiff_t>(model_frame * kFrame);
    };
    SEQLIB_CHECK(std::equal(run.samples.begin(), run.samples.end(), from(first)));
    SEQLIB_CHECK(std::equal(seek.samples.begin() + static_cast<std::ptrdiff_t>(kFrame), seek.samples.end(),
                            from(seek_first + 1)));
    SEQLIB_CHECK(enhanced.samples != strict.samples && HasSound(std::span<const int16_t>(enhanced.samples)));
    SEQLIB_CHECK(Matches(before.samples, strict, first, 0));
    bool crossfades = true;
    for (std::size_t i = 0; i < kFrame; i++)
    {
        const float through = static_cast<float>(i / 2) / static_cast<float>(kFrameSamples);
        const float out = SampleOf(strict, first + 50, i) * (1.0f - through);
        const float in = enhanced.floats[(first + 50) * kFrame + i] * through;
        crossfades = crossfades && after.samples[i] == in + out;
    }
    SEQLIB_CHECK(crossfades);
    SEQLIB_CHECK(
        std::equal(after.samples.begin() + static_cast<std::ptrdiff_t>(kFrame), after.samples.end(), from(first + 51)));
}

// One kind of archive, ROM or set that a model plays, with options that the console supports in part, and the options
// that it supports among them.
struct ModelRow
{
    const char* name;
    hokuto::song::Played played;
    std::shared_ptr<const Sequence> sequence;
    hokuto::song::ModelOptions options;
    hokuto::song::ModelOptions supported;
};

// A row for each kind that Played can contain, the three later consoles among them, in each mixing mode. The options
// add effects on the GBA, Krawall, the DS and DSE, enhanced mixing on the Wii, and `dry` where it has no effect. A new
// console adds its row here.
std::vector<ModelRow> ModelRows()
{
    namespace song = hokuto::song;
    using song::ModelOptions;

    const AuxEffects reverb = AuxEffectsWithReverb();
    const std::shared_ptr<const sar::SoundArchive> nx = OpenTestArchive();
    const auto nx_sequence = std::make_shared<const Sequence>(ReadSequence(*nx, kLoopSound));
    const std::shared_ptr<const sar::SoundArchive> cafe = OpenWiiUArchive();
    const auto cafe_sequence = std::make_shared<const Sequence>(ReadSequence(*cafe, kNoteSound));
    const std::shared_ptr<const sar::SoundArchive> ctr = LoadArchive(hokuto::testing::CtrNoteArchiveBytes(), "test");
    const auto ctr_sequence = std::make_shared<const Sequence>(ReadSequence(*ctr, 0));
    const std::shared_ptr<const ntdlib::rsar::SoundArchive> wii = OpenWiiArchive();
    const auto wii_sequence = std::make_shared<const Sequence>(ReadSequence(*wii, 0));
    const std::shared_ptr<const song::GbaRom> gba = song::LoadGbaRom(hokuto::testing::GbaMeasureCart(0x80 | 100));
    const auto gba_sequence = std::make_shared<const Sequence>(ReadSequence(*gba, 0));
    const std::shared_ptr<const song::DriverRom> driver = hokuto::testing::KrawallMeasureRom();
    const auto driver_sequence = std::make_shared<const Sequence>(driver->ReadSequence(0));
    const std::shared_ptr<const song::DsArchive> ds = hokuto::testing::DsMeasureArchive();
    const auto ds_sequence = std::make_shared<const Sequence>(ReadSequence(*ds, 0));
    const std::shared_ptr<const song::DseSet> dse = song::LoadDseSet(hokuto::testing::DseMeasureImage());
    const auto dse_sequence = std::make_shared<const Sequence>(ReadSequence(*dse, 0));

    std::vector<ModelRow> rows;
    for (const Mixing mixing : {Mixing::kStrict, Mixing::kEnhanced})
    {
        rows.push_back({"Switch",
                        nx,
                        nx_sequence,
                        {.effects = reverb, .mixing = mixing, .dry = true},
                        {.effects = reverb, .mixing = mixing}});
        rows.push_back({"Wii U",
                        cafe,
                        cafe_sequence,
                        {.mixing = mixing, .snapshot_interval = 7, .dry = true},
                        {.mixing = mixing, .snapshot_interval = 7}});
        rows.push_back({"3DS", ctr, ctr_sequence, {.mixing = mixing, .dry = true}, {.mixing = mixing}});
        rows.push_back(
            {"GBA", gba, gba_sequence, {.effects = reverb, .mixing = mixing, .dry = true}, {.mixing = mixing}});
        for (const bool dry : {false, true})
        {
            rows.push_back({"Krawall",
                            driver,
                            driver_sequence,
                            {.effects = reverb, .mixing = mixing, .dry = dry},
                            {.mixing = mixing, .dry = dry}});
            rows.push_back({"DS",
                            ds,
                            ds_sequence,
                            {.effects = reverb, .mixing = mixing, .dry = dry},
                            {.mixing = mixing, .dry = dry}});
        }
        rows.push_back(
            {"DSE", dse, dse_sequence, {.effects = reverb, .mixing = mixing, .dry = true}, {.mixing = mixing}});
    }
    rows.push_back(
        {"Wii", wii, wii_sequence, {.effects = reverb, .mixing = Mixing::kEnhanced, .dry = true}, {.effects = reverb}});

    return rows;
}

// Each kind plays, and uses only the options that its console supports: the model with a row's options plays as the
// model with the supported ones, with the same sample rate, frames, lead and output, frame by frame, for 300 frames or
// to the end.
void TestEachKind()
{
    std::string failures;
    for (const ModelRow& row : ModelRows())
    {
        SequenceModel with_options(row.played, row.sequence, row.options);
        SequenceModel supported(row.played, row.sequence, row.supported);

        bool same = with_options.SampleRate() == supported.SampleRate() &&
                    with_options.FrameSamples() == supported.FrameSamples() &&
                    with_options.LeadFrames() == supported.LeadFrames();
        bool more = with_options.SeekFrame(0);
        same = same && more == supported.SeekFrame(0);
        bool sounds = false;
        for (std::size_t frame = 0; more && frame < 300; frame++)
        {
            same = same && std::ranges::equal(with_options.Output(), supported.Output()) &&
                   std::ranges::equal(with_options.FloatOutput(), supported.FloatOutput());
            sounds = sounds || HasSound(with_options.Output());
            more = with_options.RunFrame();
            same = same && more == supported.RunFrame();
        }
        if (!same || !sounds)
        {
            failures += std::string(row.name) + (row.options.mixing == Mixing::kEnhanced ? ", enhanced" : ", strict") +
                        (row.options.dry ? ", dry" : "") + (same ? ", is silent. " : ", differs. ");
        }
    }

    if (!failures.empty())
    {
        std::fprintf(stderr, "%s\n", failures.c_str());
    }
    SEQLIB_CHECK(failures.empty());
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("sequence_model",
                                    []
                                    {
                                        TestWiiMeters();
                                        TestSeekWithSnapshots();
                                        TestSeekFrame();
                                        TestLeadFrames();
                                        TestFramesWithoutMixing();
                                        TestSeekWithoutMixing();
                                        TestStoppedClock();
                                        TestWiiUSeekWithoutMixing();
                                        TestEnhancedMixing();
                                        TestEachKind();
                                    });
}
