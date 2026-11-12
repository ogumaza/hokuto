// SPDX-License-Identifier: MIT

// The checks of hokuto_check_playback on archives of the Wii U.

#include "check_playback.h"

namespace hokuto::tools::playback
{

namespace
{

// The run of sound `sound` of `archive` on ntdlib's model of the Wii U for `frames` frames, with `effects` on the aux
// buses, in enhanced mixing when `enhanced` is true.
WiiURun RunWiiUModel(const sar::SoundArchive& archive, uint32_t sound, std::size_t frames,
                     const std::vector<cafe::EffectSetup>& effects, bool enhanced)
{
    WiiURun run;
    cafe::EngineOptions options;
    options.effects = effects;
    options.enhanced = enhanced;
    cafe::ArchiveModel model(archive, std::move(options));
    const auto* player = model.engine->StartSequence(sound);
    if (player == nullptr)
    {
        return run;
    }

    for (std::size_t frame = 0; frame < frames; frame++)
    {
        model.engine->RunFrame();
        run.after.push_back(player->TickCount());
        const std::span<const int16_t> output = model.system.Output();
        run.samples.insert(run.samples.end(), output.begin(), output.end());
        const std::span<const float> floats = model.system.OutputFloat();
        run.floats.insert(run.floats.end(), floats.begin(), floats.end());
    }

    return run;
}

// The closeness of `output` to the model's output for the frame after a seek to frame `frame`, with the model's output
// over the second before frame `lead_start` as the signal where that's louder (see SeekCloseness).
Closeness WiiUSeekCloseness(std::span<const int16_t> output, const WiiURun& expected, std::size_t frame,
                            std::size_t lead_start)
{
    const std::size_t kFloats = kWiiUFrameSamples * 2;

    const auto power = [&expected](std::size_t from, std::size_t to)
    {
        double sum = 0.0;
        for (std::size_t i = from; i < to && i < expected.samples.size(); i++)
        {
            const double sample = static_cast<double>(expected.samples[i]) / 32768.0;
            sum += sample * sample;
        }

        return to > from ? sum / static_cast<double>(to - from) : 0.0;
    };

    double difference = 0.0;
    for (std::size_t i = 0; i < output.size() && frame * kFloats + i < expected.samples.size(); i++)
    {
        const double error = static_cast<double>(output[i] - expected.samples[frame * kFloats + i]) / 32768.0;
        difference += error * error;
    }
    if (difference == 0.0)
    {
        return {};
    }

    const std::size_t from = lead_start > kWiiUFramesPerSecond ? lead_start - kWiiUFramesPerSecond : 0;
    const double signal =
        std::max(power(frame * kFloats, (frame + 1) * kFloats), power(from * kFloats, lead_start * kFloats));
    return ClosenessOf(signal * static_cast<double>(output.size()), difference, output.size());
}

// The closeness of `samples` to the model's output (ClosenessOf), over `count` frames of `samples` from frame `from`
// on, against `expected`'s frames from frame `first` + `from` on. In enhanced mixing, the expected output is in
// floating point. It stops at the end of either.
Closeness WiiUSwitchCloseness(std::span<const float> samples, std::size_t from, const WiiURun& expected,
                              std::size_t first, std::size_t count)
{
    const std::size_t kFloats = kWiiUFrameSamples * 2;
    const std::size_t start = std::min(from * kFloats, samples.size());
    std::vector<float> wanted;
    for (std::size_t i = start; i < samples.size() && i < start + count * kFloats; i++)
    {
        const std::size_t at = first * kFloats + i;
        if (at >= expected.samples.size())
        {
            break;
        }
        wanted.push_back(expected.floats.empty() ? static_cast<float>(expected.samples[at]) / 32768.0f
                                                 : expected.floats[at]);
    }

    return ClosenessOf(samples.subspan(start, wanted.size()), wanted);
}

} // namespace

bool RenderFrames(song::SequencePlayer& player, std::size_t frames, std::size_t frame_samples,
                  std::vector<float>& output)
{
    const std::size_t total = frames * frame_samples;
    while (output.size() / 2 < total)
    {
        const std::size_t block = std::min(kBlock, total - output.size() / 2);
        const std::size_t at = output.size();
        output.resize(at + block * 2);
        const std::size_t done = player.Render(output.data() + at, block);
        output.resize(at + done * 2);
        if (done < block)
        {
            return false;
        }
    }

    return true;
}

namespace
{

// The player's output for `before` frames on a model without effects in strict mixing, then on a model with `effects`
// in `mixing`, up to `frames` frames in all. A catch-up runs the new model up to the player's frame, and the player
// takes it at its next frame. The program does the same after an edit during playback.
std::vector<float> RunWiiUSwitch(const std::shared_ptr<const sar::SoundArchive>& archive,
                                 const std::shared_ptr<const song::Sequence>& sequence, const song::AuxEffects& effects,
                                 song::Mixing mixing, std::size_t before, std::size_t frames)
{
    const auto clock = std::make_shared<song::PlaybackClock>(kWiiUFrameSamples);
    song::SequencePlayer player(std::make_shared<song::SequenceModel>(archive, sequence), 0, clock);
    std::vector<float> output;
    if (!RenderFrames(player, before, kWiiUFrameSamples, output))
    {
        return output;
    }

    song::ModelCatchUp catch_up;
    catch_up.Start(std::make_shared<song::SequenceModel>(archive, sequence,
                                                         song::ModelOptions{.effects = effects, .mixing = mixing}),
                   clock);
    while (!catch_up.Finished())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    clock->Offer(catch_up.Take());
    RenderFrames(player, frames, kWiiUFrameSamples, output);

    return output;
}

// The seeks of `model` back and forth through its run of `played_frames` frames that fail against `expected`. A seek
// restores a snapshot that the run took. With `close`, the output after a seek only has to lie close to the model's
// (AddSeek), and otherwise each frame that differs counts.
std::size_t CheckSnapshotSeeks(song::SequenceModel& model, std::size_t played_frames, const WiiURun& expected,
                               bool close, Totals& totals)
{
    std::size_t seeks = 0;
    for (const std::size_t frame : {played_frames / 2, played_frames / 4, std::size_t{1}, played_frames * 3 / 4})
    {
        if (frame < played_frames && model.SeekFrame(frame))
        {
            const uint64_t lead = model.LeadFrames();
            const std::size_t lead_start = frame > lead ? frame - static_cast<std::size_t>(lead) : 0;
            if (close)
            {
                seeks += AddSeek({WiiUSeekCloseness(model.Output(), expected, frame, lead_start)}, totals) ? 0 : 1;
            }
            else
            {
                seeks += WiiUDifferences(model.Output(), 0, expected, frame, 1.0f);
            }
        }
    }

    return seeks;
}

// The text that the line of a sequence shows for a switch halfway through `sequence`, from a model without `effects` in
// strict mixing to one with them in `mixing`, and whether the switch fails against `expected`. With `switches`, the
// switch runs. `heard` is the length of the player's output without the switch, and `first` the model's first frame
// that runs a tick.
std::pair<std::string, bool> CheckWiiUSwitch(const std::shared_ptr<const sar::SoundArchive>& archive,
                                             const std::shared_ptr<const song::Sequence>& sequence,
                                             const WiiUEffects& effects, song::Mixing mixing, bool switches,
                                             std::size_t frames, std::size_t heard, std::size_t first,
                                             const WiiURun& expected, Totals& totals)
{
    std::string switch_text = "no switch";
    bool switch_differs = false;
    const auto halfway = static_cast<std::size_t>(song::MsOfTick(*sequence, sequence->length / 2.0) /
                                                  static_cast<double>(kWiiUFrameMs.numerator));
    std::size_t before = std::min(halfway, frames / 2);
    std::vector<float> switched;
    if (switches && before > 1)
    {
        switched = RunWiiUSwitch(archive, sequence, effects.effects, mixing, before, frames);
    }
    if (!switched.empty() && switched.size() < before * kWiiUFrameSamples * 2)
    {
        before = switched.size() / (kWiiUFrameSamples * 2) / 2;
        switched = before > 1 ? RunWiiUSwitch(archive, sequence, effects.effects, mixing, before, frames)
                              : std::vector<float>{};
    }
    if (!switched.empty())
    {
        const std::size_t switched_frames = switched.size() / (kWiiUFrameSamples * 2);
        switched.resize(switched_frames > 1 ? (switched_frames - 1) * kWiiUFrameSamples * 2 : 0);
        const std::span<const float> after(switched);
        const Closeness next = WiiUSwitchCloseness(after, before + 1, expected, first, kWiiUFramesPerSecond);
        const Closeness rest = WiiUSwitchCloseness(after, before + 1, expected, first, frames);
        const bool ends_elsewhere = std::min(switched_frames, frames) != std::min(heard, frames);
        switch_differs = !AddSwitch({next, rest}, totals) || ends_elsewhere;
        switch_text = "a switch " + RatioText(WorstRatio({next, rest})) + " below the model's" +
                      (ends_elsewhere ? " that ends elsewhere" : "");
    }

    return {switch_text, switch_differs};
}

} // namespace

void CheckWiiUSequence(const sar::SoundArchive& strict, const std::shared_ptr<const sar::SoundArchive>& archive,
                       uint32_t sound, std::size_t frames, const WiiUEffects& effects, song::Mixing mixing,
                       Totals& totals)
{
    const std::string name = strict.Sounds()[sound].name;
    std::shared_ptr<const song::Sequence> sequence;
    try
    {
        sequence = std::make_shared<const song::Sequence>(song::ReadSequence(*archive, sound));
    }
    catch (const std::exception& error)
    {
        std::printf("%s: can't be read. %s\n", name.c_str(), error.what());
        totals.unread++;
        return;
    }

    totals.sequences++;
    totals.looped += sequence->loop ? 1 : 0;
    const bool enhanced = mixing == song::Mixing::kEnhanced;
    const WiiURun expected = RunWiiUModel(strict, sound, frames, effects.setups, enhanced);

    // A run from the start, frame by frame. The model stops once nothing plays or sounds and a frame has come out
    // silent. Without an effect, the model's output after that is silent too. With an effect, a frame within ±8 counts
    // as silent once the effects have stopped ringing: ntdlib's threshold of a ringing effect. A delay that feeds back
    // can leave samples of ±1 for ever. The output after the end then has to stay within ±8.
    const auto model = std::make_shared<song::SequenceModel>(
        archive, sequence, song::ModelOptions{.effects = effects.effects, .mixing = mixing});
    const std::vector<int16_t> played = RunModelFrames(*model, frames * kWiiUFrameSamples * 2);
    std::size_t differences = WiiUDifferences(std::span<const int16_t>(played), 0, expected, 0, 1.0f);
    const std::size_t played_frames = played.size() / (kWiiUFrameSamples * 2);
    const std::size_t rest = std::min(played.size(), expected.samples.size());
    const auto louder_than = [&](int limit)
    {
        const auto louder = [limit](int16_t sample)
        {
            return std::abs(static_cast<int>(sample)) > limit;
        };

        return std::any_of(expected.samples.begin() + static_cast<std::ptrdiff_t>(rest), expected.samples.end(),
                           louder);
    };
    if (louder_than(effects.setups.empty() ? 0 : 8))
    {
        differences++;
    }
    else if (louder_than(0))
    {
        totals.quiet_tails++;
    }

    // Seeks back and forth restore the snapshots that the run took.
    const std::size_t seeks =
        CheckSnapshotSeeks(*model, played_frames, expected, !effects.setups.empty() || enhanced, totals);

    // The player starts at the model's first frame that runs a tick. The worker renders in blocks.
    const std::size_t first = FirstTickFrame(expected.after);
    std::vector<float> output = RunPlayerBlocks(model, frames * kWiiUFrameSamples * 2);
    const std::size_t heard = output.size() / (kWiiUFrameSamples * 2);
    // The output reaches the frame where the model's run ends. A looped sequence's output ends with the frame that
    // reaches the end of the loop's pass.
    const bool looped = sequence->loop && sequence->loop->end > sequence->loop->start;
    const bool early =
        EndsEarly(first, heard, frames, looped ? LoopEndFrames(expected.after, sequence->loop->end) : played_frames);
    output.resize(heard > 1 ? (heard - 1) * kWiiUFrameSamples * 2 : 0);
    std::size_t player_first = 0;
    const std::size_t player_differences =
        WiiUDifferences(std::span<const float>(output), 1, expected, first, 1.0f / 32768.0f, &player_first);

    // A switch halfway through the sequence, from a model without the effect in strict mixing to one with it in the
    // mode. The output before the switch can end before the sequence does. The switch then comes halfway through that
    // output. After the frame that crossfades, the output lies close to the model's with the effect: over the next
    // second, and over the rest. It ends where the output without the switch ends.
    const auto [switch_text, switch_differs] =
        CheckWiiUSwitch(archive, sequence, effects, mixing, !effects.setups.empty() || enhanced, frames, heard, first,
                        expected, totals);

    // A sequence that the model doesn't start compares against nothing, and fails as on the Switch.
    const bool unstarted = expected.after.empty();
    if (differences + seeks + player_differences > 0 || switch_differs || played.empty() || unstarted || early)
    {
        std::printf(
            "%s: %zu of %zu frames differ from the start, %zu after seeks, %zu of %zu from the player, first "
            "%zu, %s (loop %s).%s%s\n",
            name.c_str(), differences, played_frames, seeks, player_differences, heard, player_first,
            switch_text.c_str(), LoopText(sequence->loop).c_str(), unstarted ? " The model doesn't start it." : "",
            early ? " The player's output ends before the model's." : "");
        totals.failed++;
    }
    std::fflush(stdout);
}

} // namespace hokuto::tools::playback
