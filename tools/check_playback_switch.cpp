// SPDX-License-Identifier: MIT

// The checks of hokuto_check_playback on archives of the Switch.

#include "check_playback.h"

namespace hokuto::tools::playback
{

namespace
{

static_assert(kFrameMs.denominator == 1);

constexpr double kFramesPerMinute = 60.0 * kFramesPerSecond;

// Runs `frames` frames of sequence sound `sound` on the model with `options`, with the variables' starting values
// `variables` and the game's `changes`. A channel plays a new note when it's new in its place, or when its keys change.
ModelRun RunModel(const sar::SoundArchive& archive, uint32_t sound, std::size_t frames,
                  const atk::EngineOptions& options, const song::StartValues& variables, const song::Changes& changes)
{
    ModelRun run;
    atk::ArchiveModel model(archive, options);
    auto* player = model.engine->StartSequence(sound);
    if (player == nullptr)
    {
        return run;
    }

    for (const auto& [number, value] : variables)
    {
        player->SetVariable(*model.engine, number, value);
    }

    std::map<int, std::pair<int, int>> previous;
    for (std::size_t frame = 0; frame < frames; frame++)
    {
        // The game's changes come before the frame.
        for (const song::VariableChange& change : changes)
        {
            if (song::ChangeFrame(change.time, kFrameMs) == frame)
            {
                player->SetVariable(*model.engine, change.variable, change.value);
            }
        }

        const bool ended = player->IsFinished();
        const double position = ended ? run.position_after.back() : static_cast<double>(player->TickCount());
        run.before.push_back(player->TickCount());
        run.position_before.push_back(position);
        run.busy.push_back(model.engine->IsBusy());
        model.engine->RunFrame();
        run.after.push_back(player->TickCount());
        run.position_after.push_back(ended ? position + player->Tempo() * player->Timebase() / kFramesPerMinute
                                           : static_cast<double>(player->TickCount()));
        const std::span<const int16_t> output = model.renderer.DeviceOutput();
        run.samples.insert(run.samples.end(), output.begin(), output.end());
        const std::span<const float> floats = model.renderer.DeviceOutputFloat();
        run.floats.insert(run.floats.end(), floats.begin(), floats.end());

        // Each channel's track, from the tracks' lists of channels.
        const auto& channels = model.engine->Channels();
        std::map<int, int> track_of;
        for (int t = 0; t < kTracks; t++)
        {
            const auto* track = player->Track(t);
            for (int place = track != nullptr ? track->ChannelList() : ntdlib::nwsnd::kNoChannel;
                 place != ntdlib::nwsnd::kNoChannel; place = channels.At(place).next_in_track)
            {
                track_of[place] = t;
            }
        }

        std::map<int, std::pair<int, int>> current;
        std::vector<NoteStart> started;
        for (const int place : channels.InUse())
        {
            const auto& channel = channels.At(place);
            const std::pair<int, int> keys = {channel.key, channel.original_key};
            current[place] = keys;
            const auto old = previous.find(place);
            if (old == previous.end() || old->second != keys)
            {
                const auto track = track_of.find(place);
                started.push_back({track != track_of.end() ? track->second : -1, channel.key, old != previous.end()});
            }
        }
        run.notes.push_back(std::move(started));
        previous = std::move(current);
    }

    return run;
}

// The first frame of `run` after which the position has passed `tick`, or the number of frames.
std::size_t FirstFrameAfter(const ModelRun& run, uint64_t tick)
{
    std::size_t frame = 0;
    while (frame < run.position_after.size() && run.position_after[frame] <= static_cast<double>(tick))
    {
        frame++;
    }

    return frame;
}

// Whether frame `frame` of `run` came out silent.
bool Silent(const ModelRun& run, std::size_t frame)
{
    const std::span<const int16_t> samples =
        std::span<const int16_t>(run.samples).subspan(frame * kFrameSamples * 2, kFrameSamples * 2);
    return std::all_of(samples.begin(), samples.end(), [](int16_t sample) { return sample == 0; });
}

// The first frame of `run` before which nothing played or sounded and after a frame that came out silent, or the number
// of frames. From there on, the model's output is silent.
std::size_t FadedFrame(const ModelRun& run)
{
    std::size_t frame = 0;
    while (frame < run.busy.size() && (run.busy[frame] || (frame > 0 && !Silent(run, frame - 1))))
    {
        frame++;
    }

    return frame;
}

// A clock for the playback of `sequence`. A looped sequence goes round its loop. The game plays it that way.
std::shared_ptr<song::PlaybackClock> ClockFor(const song::Sequence& sequence)
{
    const auto clock = std::make_shared<song::PlaybackClock>(kFrameSamples);
    if (const std::optional<song::Loop>& loop = sequence.loop)
    {
        clock->SetCycle({true, loop->start, loop->end});
    }

    return clock;
}

} // namespace

void Render(song::SequencePlayer& player, const song::PlaybackClock& clock, std::size_t frames, PlayerRun& run)
{
    const std::size_t total = run.samples.size() / 2 + frames * kFrameSamples;
    std::size_t done = run.samples.size() / 2;
    run.samples.resize(total * 2);
    while (done < total && !run.ended)
    {
        const std::size_t block = std::min(kBlock, total - done);
        const std::size_t rendered = player.Render(run.samples.data() + done * 2, block);
        done += rendered;
        run.ended = rendered < block;
        while (run.marks.size() * kFrameSamples < done)
        {
            run.marks.push_back(clock.PositionAt(run.marks.size() * kFrameSamples));
        }
    }
    run.samples.resize(done * 2);
}

namespace
{

// The output of a player of `model`'s sequence from tick `start`, for `frames` frames.
PlayerRun RunPlayer(const std::shared_ptr<song::SequenceModel>& model, uint32_t start, std::size_t frames)
{
    const std::shared_ptr<song::PlaybackClock> clock = ClockFor(model->GetSequence());
    song::SequencePlayer player(model, start, clock);
    PlayerRun run;
    Render(player, *clock, frames, run);

    return run;
}

// The player's output for `before` frames on a model without effects in strict mixing, then on a model with `effects`
// in `mixing`, up to `frames` frames in all. A catch-up runs the new model up to the player's frame, and the player
// takes it at its next frame. The program does the same after an edit during playback.
PlayerRun RunSwitch(const std::shared_ptr<const sar::SoundArchive>& archive,
                    const std::shared_ptr<const song::Sequence>& sequence, const song::AuxEffects& effects,
                    song::Mixing mixing, std::size_t before, std::size_t frames)
{
    const std::shared_ptr<song::PlaybackClock> clock = ClockFor(*sequence);
    song::SequencePlayer player(std::make_shared<song::SequenceModel>(archive, sequence), 0, clock);
    PlayerRun run;
    Render(player, *clock, before, run);

    song::ModelCatchUp catch_up;
    catch_up.Start(std::make_shared<song::SequenceModel>(archive, sequence,
                                                         song::ModelOptions{.effects = effects, .mixing = mixing}),
                   clock);
    while (!catch_up.Finished())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    clock->Offer(catch_up.Take());
    Render(player, *clock, frames - before, run);

    return run;
}

// The samples of `run` that differ from the model's output from frame `first` on. The first `skip` samples don't count.
// Samples past the end of the model's run differ. The enhanced mode's samples have to match in floating point.
std::size_t Differences(const PlayerRun& run, const ModelRun& model, std::size_t first, std::size_t skip)
{
    const std::size_t offset = first * kFrameSamples * 2;
    std::size_t differences = 0;
    for (std::size_t i = skip; i < run.samples.size(); i++)
    {
        const bool past_end = offset + i >= model.samples.size();
        if (past_end || (model.floats.empty() ? std::lround(run.samples[i] * 32768.0f) != model.samples[offset + i]
                                              : run.samples[i] != model.floats[offset + i]))
        {
            differences++;
        }
    }

    return differences;
}

// The closeness of `run`'s output to the model's (ClosenessOf), over `frames` frames of `run` after its first `skip`
// samples, against the model's output from frame `first` on. It stops at the end of either.
Closeness SwitchCloseness(const PlayerRun& run, const ModelRun& model, std::size_t first, std::size_t skip,
                          std::size_t frames)
{
    const std::size_t offset = first * kFrameSamples * 2;
    const std::size_t end = std::min({run.samples.size(), skip + frames * kFrameSamples * 2,
                                      model.samples.size() - std::min(offset, model.samples.size())});
    std::vector<float> expected;
    for (std::size_t i = skip; i < end; i++)
    {
        expected.push_back(model.floats.empty() ? static_cast<float>(model.samples[offset + i]) / 32768.0f
                                                : model.floats[offset + i]);
    }

    return ClosenessOf(std::span<const float>(run.samples).subspan(skip, expected.size()), expected);
}

} // namespace

Closeness SeekCloseness(const PlayerRun& run, const ModelRun& model, std::size_t first, std::size_t skip,
                        std::size_t frames, std::size_t lead_start)
{
    const std::size_t offset = first * kFrameSamples * 2;
    const std::size_t end = std::min({run.samples.size(), skip + frames * kFrameSamples * 2,
                                      model.samples.size() - std::min(offset, model.samples.size())});

    const auto wanted = [&model](std::size_t i)
    {
        return model.floats.empty() ? static_cast<double>(model.samples[i]) / 32768.0
                                    : static_cast<double>(model.floats[i]);
    };

    double signal = 0.0;
    double difference = 0.0;
    for (std::size_t i = skip; i < end; i++)
    {
        const double expected = wanted(offset + i);
        signal += expected * expected;
        difference += (run.samples[i] - expected) * (run.samples[i] - expected);
    }
    if (difference == 0.0)
    {
        return {};
    }

    const std::size_t from = (lead_start > kFramesPerSecond ? lead_start - kFramesPerSecond : 0) * kFrameSamples * 2;
    const std::size_t to = std::min(lead_start * kFrameSamples * 2, model.samples.size());
    double before = 0.0;
    for (std::size_t i = from; i < to; i++)
    {
        before += wanted(i) * wanted(i);
    }
    const std::size_t count = end > skip ? end - skip : 1;
    const double power =
        std::max(signal / static_cast<double>(count), to > from ? before / static_cast<double>(to - from) : 0.0);
    return ClosenessOf(power * static_cast<double>(count), difference, count);
}

namespace
{

// The frames from the second on whose position differs from the model's position before them, round the loop. A looped
// sequence's position is a whole tick count.
std::size_t WrongMarks(const PlayerRun& run, const ModelRun& model, std::size_t first, const song::Sequence& sequence)
{
    std::size_t wrong = 0;
    for (std::size_t frame = 1; frame < run.marks.size() && first + frame < model.before.size(); frame++)
    {
        const double position = model.position_before[first + frame];
        const double expected =
            sequence.loop ? song::PositionOfTicks(sequence, static_cast<uint64_t>(position)) : position;
        if (!run.marks[frame] || std::abs(*run.marks[frame] - expected) > 1e-9)
        {
            wrong++;
        }
    }

    return wrong;
}

// The problems of `shared_model`'s output from the start, against `model`, the model's run from frame `first`, for
// `frames` frames.
std::string CheckFromStart(const std::shared_ptr<song::SequenceModel>& shared_model, const ModelRun& model,
                           const song::Sequence& sequence, std::size_t first, std::size_t frames)
{
    std::string problems;
    const PlayerRun run = RunPlayer(shared_model, 0, frames);
    const std::span<const int16_t> lead(model.samples.data(),
                                        std::min(model.samples.size(), first * 2 * kFrameSamples));
    if (std::any_of(lead.begin(), lead.end(), [](int16_t sample) { return sample != 0; }))
    {
        problems += " sound before the first tick.";
    }
    if (const std::size_t differences = Differences(run, model, first, 0); differences > 0)
    {
        problems += " " + std::to_string(differences) + " samples differ from the start.";
    }
    if (const std::size_t wrong = WrongMarks(run, model, first, sequence); wrong > 0)
    {
        problems += " " + std::to_string(wrong) + " positions differ from the start.";
    }
    if (run.ended && sequence.loop)
    {
        problems += " a looped sequence ended.";
    }
    if (run.ended && first + run.samples.size() / (kFrameSamples * 2) < FadedFrame(model))
    {
        problems += " the output ended before the model's faded.";
    }

    return problems;
}

// The problems of `shared_model`'s output after a seek to the loop's start, or else to the middle of `sequence`,
// against `model`, the model's run, for `frames` frames at most. With `close`, the output only has to lie close to the
// model's (AddSeek).
std::string CheckFromSeek(const std::shared_ptr<song::SequenceModel>& shared_model, const ModelRun& model,
                          const song::Sequence& sequence, std::size_t frames, bool close, Totals& totals)
{
    std::string problems;
    const uint32_t start = sequence.loop ? sequence.loop->start : sequence.length / 2;
    const std::size_t seek_first = FirstFrameAfter(model, start);
    if (start > 0 && seek_first < model.after.size() && seek_first >= FadedFrame(model))
    {
        const PlayerRun run = RunPlayer(shared_model, start, 1);
        if (!run.samples.empty() || !run.ended)
        {
            problems += " a seek past the last note's fade plays.";
        }
    }
    else if (start > 0 && seek_first < model.after.size())
    {
        const PlayerRun run = RunPlayer(shared_model, start, std::min(frames, model.after.size() - seek_first));
        if (close)
        {
            const std::size_t lead = shared_model->LeadFrames();
            const std::size_t lead_start = seek_first > lead ? seek_first - lead : 0;
            const Closeness next =
                SeekCloseness(run, model, seek_first, kFrameSamples * 2, kFramesPerSecond, lead_start);
            const Closeness rest = SeekCloseness(run, model, seek_first, kFrameSamples * 2, frames, lead_start);
            if (!AddSeek({next, rest}, totals))
            {
                problems +=
                    " the output after a seek lies only " + RatioText(WorstRatio({next, rest})) + " below the model's.";
            }
        }
        else if (const std::size_t differences = Differences(run, model, seek_first, kFrameSamples * 2);
                 differences > 0)
        {
            problems += " " + std::to_string(differences) + " samples differ after a seek.";
        }
        if (run.marks.empty() || run.marks[0] != static_cast<double>(song::PositionOfTicks(sequence, start)))
        {
            problems += " the seek's first position differs.";
        }
        if (run.ended && seek_first + run.samples.size() / (kFrameSamples * 2) < FadedFrame(model))
        {
            problems += " the output ended before the model's faded after a seek.";
        }
    }

    return problems;
}

// The problems of the output after a switch halfway through `frames` frames, from a model without `effects` in strict
// mixing to one with them in `mixing`, against `model`, the model's run from frame `first`.
std::string CheckSwitch(const std::shared_ptr<const sar::SoundArchive>& archive,
                        const std::shared_ptr<const song::Sequence>& sequence, const song::AuxEffects& effects,
                        song::Mixing mixing, const ModelRun& model, std::size_t first, std::size_t frames,
                        Totals& totals)
{
    std::string problems;
    const PlayerRun run = RunSwitch(archive, sequence, effects, mixing, frames / 2, frames);
    const std::size_t skip = (frames / 2 + 1) * kFrameSamples * 2;
    if (run.samples.size() > skip)
    {
        const Closeness next = SwitchCloseness(run, model, first, skip, kFramesPerSecond);
        const Closeness rest = SwitchCloseness(run, model, first, skip, frames);
        if (!AddSwitch({next, rest}, totals))
        {
            problems +=
                " the output after a switch lies only " + RatioText(WorstRatio({next, rest})) + " below the model's.";
        }
    }
    if (run.ended && run.samples.size() > skip && first + run.samples.size() / (kFrameSamples * 2) < FadedFrame(model))
    {
        problems += " the output ended before the model's faded after a switch.";
    }

    return problems;
}

} // namespace

void CheckSequence(const sar::SoundArchive& strict, const std::shared_ptr<const sar::SoundArchive>& archive,
                   uint32_t sound, std::size_t frames, const atk::EngineOptions& options,
                   const song::AuxEffects& effects, const song::StartValues& values, const song::Changes& changes,
                   Totals& totals)
{
    const song::Mixing mixing = options.enhanced ? song::Mixing::kEnhanced : song::Mixing::kStrict;
    const std::string name = strict.Sounds()[sound].name + ValuesText(values) + ChangesText(changes);
    std::shared_ptr<const song::Sequence> sequence;
    try
    {
        sequence = std::make_shared<const song::Sequence>(song::ReadSequence(*archive, sound, values, changes));
    }
    catch (const std::exception& error)
    {
        std::printf("%s: unread: %s\n", name.c_str(), error.what());
        totals.unread++;
        return;
    }

    std::string problems;
    const std::size_t model_frames = frames + 2 * kFramesPerSecond;
    const ModelRun model = RunModel(strict, sound, model_frames, options, values, changes);
    if (model.after.empty())
    {
        problems += " the model doesn't start it.";
    }
    if (!changes.empty() && RunModel(strict, sound, model_frames, options, values, {}).samples != model.samples)
    {
        totals.affected++;
    }

    // The runs share a model. The program's playbacks do the same. The seek then starts from a snapshot of the run from
    // the start.
    const auto shared_model = std::make_shared<song::SequenceModel>(
        archive, sequence, song::ModelOptions{.effects = effects, .mixing = mixing});

    // From the start.
    const std::size_t first = FirstFrameAfter(model, 0);
    if (!model.after.empty())
    {
        problems += CheckFromStart(shared_model, model, *sequence, first, frames);
    }

    // From a seek. The comparison stops at the end of the model's run. Nothing plays from a seek past the point where
    // the last note has faded. The sequence's length can run past that point. With an effect or in enhanced mixing, the
    // ratio measures the output.
    const bool with_effects = std::any_of(effects.begin(), effects.end(), [](const auto& bus) { return !bus.empty(); });
    problems += CheckFromSeek(shared_model, model, *sequence, frames, with_effects || options.enhanced, totals);

    // A switch halfway, from a model without the effects in strict mixing to one with them in the mode. After the frame
    // that crossfades, the output lies close to the model's with the effects: over the next second, and over the rest.
    if ((with_effects || options.enhanced) && !model.after.empty())
    {
        problems += CheckSwitch(archive, sequence, effects, mixing, model, first, frames, totals);
    }

    // The notes, up to `frames` frames from the start. A note that the model cuts in the tick it starts is missing from
    // the arrangement, and inaudible. Missing notes are listed apart from the differences.
    const NoteCheck notes = CheckNotes(model, *sequence, first + frames);
    totals.sequences++;
    totals.looped += sequence->loop ? 1 : 0;
    totals.notes += notes.checked;
    totals.missed_notes += notes.missed;
    totals.missed_later += notes.missed_later;
    if (!problems.empty())
    {
        totals.failed++;
        std::printf("%s (loop %s):%s\n", name.c_str(), LoopText(sequence->loop).c_str(), problems.c_str());
    }
    if (notes.missed > 0)
    {
        std::printf("%s (loop %s): %zu of %zu notes missing from the arrangement, %zu of them after the first pass.\n",
                    name.c_str(), LoopText(sequence->loop).c_str(), notes.missed, notes.checked, notes.missed_later);
    }
    std::fflush(stdout);
}

} // namespace hokuto::tools::playback
