// SPDX-License-Identifier: MIT

// The checks of hokuto_check_playback on archives of the 3DS.

#include "check_playback.h"

namespace hokuto::tools::playback
{

namespace
{

// The stereo samples of a frame of the 3DS in strict mixing, or with `enhanced` in enhanced mixing.
std::size_t CtrFrameSamples(bool enhanced)
{
    return enhanced ? song::CtrModel::kEnhancedFrameSamples : song::CtrModel::kFrameSamples;
}

// The output of ntdlib's archive mode of the 3DS, run directly for `frames` frames with the variables' starting values
// `variables` and the game's `changes`, the tick count before and after each frame, and the notes that each frame
// started. `samples` holds the DSP's output after Hokuto's silence. The run's frames then line up with Hokuto's. In
// enhanced mixing, `floats` holds the enhanced mode's frames after Hokuto's silence, and `samples` the same rounded to
// 16 bits. The run stays empty when the firmware or the sequence doesn't start. Throws if the sequence's data is
// malformed.
struct CtrRun
{
    std::size_t frame_floats = 0;
    std::vector<int16_t> samples;
    std::vector<float> floats;
    std::vector<uint64_t> before;
    std::vector<uint64_t> after;
    std::vector<std::vector<NoteStart>> notes;
    bool sent = false; // whether bus A took sound, with an effect on it
};

// The effect in the form that Hokuto's model plays.
song::AuxEffects CtrModelEffects(const CtrEffect& effect)
{
    song::AuxEffects effects;
    if (effect.reverb)
    {
        effects[0].emplace_back(song::CtrReverb{effect.library, {}});
    }
    else
    {
        effects[0].emplace_back(song::CtrDelay{});
    }

    return effects;
}

// The frames that the effects process: 160 samples at 32,728 Hz, or in enhanced mixing 235 samples at 235 / 160 of that
// rate. The frames of both modes last as long.
ntdlib::ctr_fx::FrameFormat CtrEffectFormat(bool enhanced)
{
    if (!enhanced)
    {
        return {};
    }

    return {song::CtrModel::kEnhancedFrameSamples, 32728.0f * 235.0f / 160.0f};
}

// Whether a frame of an aux bus holds a sample other than 0.
bool Sounding(const std::array<int32_t*, 4>& channels, std::size_t samples)
{
    const auto sounding = [samples](const int32_t* channel)
    {
        return std::any_of(channel, channel + samples, [](int32_t sample) { return sample != 0; });
    };
    return std::any_of(channels.begin(), channels.end(), sounding);
}

// Teakra runs `firmware` in strict mixing. With `enhanced`, the model of the firmware runs in its enhanced mode. With
// `effect`, the effect runs in bus A's aux callback on ntdlib's model of the library's effect. That is how a game's
// callback and the SDK's effect run it. The callback goes on after the DSP has started, before the sequence starts.
CtrRun RunCtrModel(std::span<const uint8_t> firmware, const sar::SoundArchive& archive, uint32_t sound,
                   std::size_t frames, const song::StartValues& variables, const song::Changes& changes, bool enhanced,
                   const CtrEffect* effect)
{
    CtrRun run;
    run.frame_floats = CtrFrameSamples(enhanced) * 2;
    std::vector<int16_t> samples(song::CtrModel::kSilence * 2, 0);
    std::vector<float> floats(song::CtrModel::kEnhancedSilence * 2, 0.0f);
    ntdlib::ctr::snd::ArchiveModel model(enhanced ? ntdlib::ctr::snd::DspKind::kEnhancedModel
                                                  : ntdlib::ctr::snd::DspKind::kTeakra);
    const auto keep_sample = [&samples](std::array<int16_t, 2> sample)
    {
        samples.insert(samples.end(), sample.begin(), sample.end());
    };
    model.dsp->SetSampleSink(keep_sample);
    const auto keep_frame = [&floats](std::span<const float> frame)
    {
        floats.insert(floats.end(), frame.begin(), frame.end());
    };
    model.dsp->SetFrameSink(keep_frame);
    if (enhanced ? !model.Start(archive) : !model.Start(firmware, archive))
    {
        return run;
    }
    if (effect != nullptr)
    {
        const ntdlib::ctr_fx::FrameFormat format = CtrEffectFormat(enhanced);
        bool* const sent = &run.sent;
        if (effect->reverb)
        {
            model.snd.SetAuxCallback(0,
                                     [reverb = ntdlib::ctr_fx::Reverb(effect->library, {}, format), sent](
                                         const std::array<int32_t*, 4>& channels, std::size_t samples) mutable
                                     {
                                         *sent = *sent || Sounding(channels, samples);
                                         reverb.Process(channels, samples);
                                     });
        }
        else
        {
            model.snd.SetAuxCallback(0,
                                     [delay = ntdlib::ctr_fx::Delay({}, format), sent](
                                         const std::array<int32_t*, 4>& channels, std::size_t samples) mutable
                                     {
                                         *sent = *sent || Sounding(channels, samples);
                                         delay.Process(channels, samples);
                                     });
        }
    }
    ntdlib::ctr::snd::SequenceSoundPlayer* player = model.engine->StartSequence(sound);
    if (player == nullptr)
    {
        return run;
    }
    for (const auto& [number, value] : variables)
    {
        player->SetVariable(*model.engine, number, value);
    }

    // A channel plays a new note when it's new in its place, or when its keys change.
    std::map<int, std::pair<int, int>> previous;
    for (std::size_t frame = 0; frame < frames; frame++)
    {
        // The game's changes come before the frame.
        for (const song::VariableChange& change : changes)
        {
            if (song::ChangeFrame(change.time, kCtrFrameMs) == frame)
            {
                player->SetVariable(*model.engine, change.variable, change.value);
            }
        }

        const uint64_t before = player->TickCount();
        if (!model.engine->RunFrame())
        {
            break;
        }
        run.before.push_back(before);
        run.after.push_back(player->TickCount());

        // Each channel's track, from the tracks' lists of channels.
        const ntdlib::ctr::snd::ChannelManager& channels = model.engine->Channels();
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

    if (enhanced)
    {
        floats.resize(std::min(floats.size(), run.after.size() * run.frame_floats));
        run.floats = std::move(floats);
        for (const float sample : run.floats)
        {
            const double scaled = std::round(static_cast<double>(sample) * 32768.0);
            run.samples.push_back(static_cast<int16_t>(std::clamp(scaled, -32768.0, 32767.0)));
        }

        return run;
    }

    samples.resize(std::min(samples.size(), run.after.size() * run.frame_floats));
    run.samples = std::move(samples);

    return run;
}

// The frames of `samples` from frame `from` on that differ from the frames of `expected` from frame `first` + `from`
// on, up to the end of either. Each frame holds `frame_floats` samples. A frame matches when each of its samples is the
// expected one times `scale`.
template <class Sample, class Expected>
std::size_t CtrDifferences(std::span<const Sample> samples, std::size_t from, std::span<const Expected> expected,
                           std::size_t frame_floats, std::size_t first, float scale,
                           std::size_t* first_difference = nullptr)
{
    std::size_t differences = 0;
    for (std::size_t frame = from;
         (frame + 1) * frame_floats <= samples.size() && (first + frame + 1) * frame_floats <= expected.size(); frame++)
    {
        const std::size_t at = (first + frame) * frame_floats;
        for (std::size_t i = 0; i < frame_floats; i++)
        {
            if (static_cast<float>(samples[frame * frame_floats + i]) != static_cast<float>(expected[at + i]) * scale)
            {
                if (differences == 0 && first_difference != nullptr)
                {
                    *first_difference = frame;
                }
                differences++;
                break;
            }
        }
    }

    return differences;
}

// The frames of the player's output in `samples` from frame `from` on that differ from the run's frames from frame
// `first` + `from` on, in the player's floats: the enhanced mode's floats, or the DSP's samples at the player's scale.
std::size_t CtrPlayerDifferences(std::span<const float> samples, std::size_t from, const CtrRun& expected,
                                 std::size_t first, std::size_t* first_difference = nullptr)
{
    if (!expected.floats.empty())
    {
        return CtrDifferences(samples, from, std::span<const float>(expected.floats), expected.frame_floats, first,
                              1.0f, first_difference);
    }

    return CtrDifferences(samples, from, std::span<const int16_t>(expected.samples), expected.frame_floats, first,
                          1.0f / 32768.0f, first_difference);
}

// The player's output for `before` frames in mode `from`, then on a new model of the same sequence in mode `to` with
// `effects`, up to `frames` frames in all. A catch-up runs the new model up to the player's frame, and the player takes
// it at its next frame. The player keeps the rate of mode `from`.
std::vector<float> RunCtrSwitch(const std::shared_ptr<const sar::SoundArchive>& archive,
                                const std::shared_ptr<const song::Sequence>& sequence, std::size_t before,
                                std::size_t frames, song::Mixing from, song::Mixing to, const song::AuxEffects& effects)
{
    const std::size_t frame_samples = CtrFrameSamples(from == song::Mixing::kEnhanced);
    const auto clock = std::make_shared<song::PlaybackClock>(frame_samples);
    song::SequencePlayer player(
        std::make_shared<song::SequenceModel>(archive, sequence, song::ModelOptions{.mixing = from}), 0, clock);
    std::vector<float> output;
    if (!playback_steps::Render(player, before, frame_samples, output))
    {
        return output;
    }

    song::ModelCatchUp catch_up;
    catch_up.Start(
        std::make_shared<song::SequenceModel>(archive, sequence, song::ModelOptions{.effects = effects, .mixing = to}),
        clock);
    while (!catch_up.Finished())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    clock->Offer(catch_up.Take());
    playback_steps::Render(player, frames - before, frame_samples, output);

    return output;
}

// The frames of a switch's output from frame `before` + 1 on that differ from the reference of the new mode through a
// FrameResampler: in strict mixing, the player's frames have to be the enhanced mode's frames, from frame `first` +
// `before` on, at the rate of strict mixing. The resampler takes the frame that crossfades first. The output's last
// frame can fade out, and the comparison leaves it out.
std::size_t ResampledDifferences(std::span<const float> switched, const CtrRun& expected, std::size_t first,
                                 std::size_t before)
{
    const std::size_t kToFloats = song::CtrModel::kFrameSamples * 2;
    song::FrameResampler resampler(expected.frame_floats / 2, kToFloats / 2);
    std::vector<float> resampled(kToFloats);
    std::size_t differences = 0;
    for (std::size_t frame = before; (frame + 2) * kToFloats <= switched.size() &&
                                     (first + frame + 1) * expected.frame_floats <= expected.floats.size();
         frame++)
    {
        resampler.Process(std::span<const float>(expected.floats)
                              .subspan((first + frame) * expected.frame_floats, expected.frame_floats),
                          resampled);
        if (frame > before && !std::equal(resampled.begin(), resampled.end(),
                                          switched.begin() + static_cast<std::ptrdiff_t>(frame * kToFloats)))
        {
            differences++;
        }
    }

    return differences;
}

// The frames that differ from `expected` after a seek of `model` to the middle of the player's output of `heard`
// frames from frame `first`, then one back to the second frame past the model's silence. In `enhanced` mixing, the
// model's floats count. A seek that fails counts as one frame.
std::size_t CheckCtrSeeks(song::SequenceModel& model, std::size_t first, std::size_t heard, bool enhanced,
                          const CtrRun& expected)
{
    // The reference has the model's frames of silence too. A seek into them compares zeros with zeros.
    const std::size_t silence = enhanced ? song::CtrModel::kEnhancedSilence / song::CtrModel::kEnhancedFrameSamples
                                         : song::CtrModel::kSilence / song::CtrModel::kFrameSamples;
    const std::size_t frame_floats = expected.frame_floats;
    std::size_t seeks = 0;
    for (const std::size_t frame : {first + heard / 2, std::max(first, silence) + 1})
    {
        if (frame >= first + heard)
        {
            continue;
        }
        if (!model.SeekFrame(frame))
        {
            seeks++;
            continue;
        }

        seeks += enhanced ? CtrPlayerDifferences(model.FloatOutput(), 0, expected, frame)
                          : CtrDifferences(model.Output(), 0, std::span<const int16_t>(expected.samples), frame_floats,
                                           frame, 1.0f);
    }

    return seeks;
}

// The text that the line of a sequence adds for a switch halfway through the player's output of `heard` frames from
// frame `first`, to a new model of `sequence` in `mixing` with `effects`, or nothing when the output after the switch
// matches `expected`. `effect` is the effect on bus A, if any.
std::string CheckCtrSwitch(const std::shared_ptr<const sar::SoundArchive>& archive,
                           const std::shared_ptr<const song::Sequence>& sequence, song::Mixing mixing,
                           const song::AuxEffects& effects, const CtrEffect* effect, std::size_t frames,
                           std::size_t heard, std::size_t first, const CtrRun& expected, Totals& totals)
{
    const bool enhanced = mixing == song::Mixing::kEnhanced;
    const std::size_t frame_floats = expected.frame_floats;
    std::string switch_text;
    const std::size_t kStrictFloats = song::CtrModel::kFrameSamples * 2;
    std::size_t before = heard / 2;
    std::vector<float> switched;
    if (before > 1)
    {
        switched = RunCtrSwitch(archive, sequence, before, frames, song::Mixing::kStrict, mixing, effects);
    }
    if (effect != nullptr && !switched.empty() && switched.size() < before * kStrictFloats)
    {
        before = switched.size() / kStrictFloats / 2;
        switched = before > 1 ? RunCtrSwitch(archive, sequence, before, frames, song::Mixing::kStrict, mixing, effects)
                              : std::vector<float>{};
    }
    if (!switched.empty())
    {
        const std::size_t switched_frames = switched.size() / kStrictFloats;
        std::size_t switch_differences = 0;
        if (enhanced)
        {
            switch_differences = ResampledDifferences(switched, expected, first, before);
        }
        else
        {
            std::vector<int16_t> rounded(switched_frames > 1 ? (switched_frames - 1) * frame_floats : 0);
            for (std::size_t i = 0; i < rounded.size(); i++)
            {
                rounded[i] = static_cast<int16_t>(std::lround(switched[i] * 32768.0f));
            }
            switch_differences = CtrDifferences(std::span<const int16_t>(rounded), effect != nullptr ? before + 1 : 1,
                                                std::span<const int16_t>(expected.samples), frame_floats, first, 1.0f);
        }
        const bool ends_elsewhere = std::min(switched_frames, frames) != std::min(heard, frames);
        totals.switches++;
        totals.exact += switch_differences == 0 && !ends_elsewhere ? 1 : 0;
        if (switch_differences > 0)
        {
            switch_text += ", " + std::to_string(switch_differences) + " after a switch halfway";
        }
        if (ends_elsewhere)
        {
            switch_text += ", and the output after a switch ends at frame " + std::to_string(switched_frames);
        }
    }

    return switch_text;
}

} // namespace

void CheckCtrSequence(std::span<const uint8_t> firmware, const sar::SoundArchive& strict,
                      const std::shared_ptr<const sar::SoundArchive>& archive, uint32_t sound, std::size_t frames,
                      const song::StartValues& values, const song::Changes& changes, bool enhanced,
                      const CtrEffect* effect, Totals& totals)
{
    const std::string name = strict.Sounds()[sound].name + ValuesText(values) + ChangesText(changes);
    std::shared_ptr<const song::Sequence> sequence;
    try
    {
        sequence = std::make_shared<const song::Sequence>(song::ReadSequence(*archive, sound, values, changes));
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "%s: can't be read. %s\n", name.c_str(), error.what());
        totals.unread++;
        return;
    }

    totals.sequences++;
    totals.looped += sequence->loop ? 1 : 0;
    CtrRun expected;
    try
    {
        expected = RunCtrModel(firmware, strict, sound, frames, values, changes, enhanced, effect);
        if (!changes.empty() &&
            RunCtrModel(firmware, strict, sound, frames, values, {}, enhanced, effect).samples != expected.samples)
        {
            totals.affected++;
        }
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "%s: archive mode can't play it. %s\n", name.c_str(), error.what());
        totals.failed++;
        return;
    }

    // Archive mode runs every frame of the check. It stops short only when it can't start the sequence, or when the DSP
    // stops answering. The comparisons below cover only the frames that both sides hold, and would pass a short run.
    if (expected.after.size() < frames)
    {
        if (expected.after.empty())
        {
            std::fprintf(stderr, "%s: archive mode doesn't start it.\n", name.c_str());
        }
        else
        {
            std::fprintf(stderr, "%s: archive mode stops after %zu of %zu frames.\n", name.c_str(),
                         expected.after.size(), frames);
        }
        totals.failed++;
        return;
    }

    // The player starts at the model's first frame that runs a tick. The worker renders in blocks.
    const std::size_t first = FirstTickFrame(expected.after);

    const song::Mixing mixing = enhanced ? song::Mixing::kEnhanced : song::Mixing::kStrict;
    const std::size_t frame_floats = expected.frame_floats;
    const song::AuxEffects effects = effect != nullptr ? CtrModelEffects(*effect) : song::AuxEffects{};
    const auto model = std::make_shared<song::SequenceModel>(archive, sequence,
                                                             song::ModelOptions{.effects = effects, .mixing = mixing});
    std::vector<float> output = RunPlayerBlocks(model, frames * frame_floats);
    const std::size_t heard = output.size() / frame_floats;
    const bool ended = heard < frames;
    output.resize(heard > 1 ? (heard - 1) * frame_floats : 0);
    std::size_t player_first = 0;

    // A sequence can stop the clock with an unset tempo variable (song::SequenceModel::ClockStopped). Archive mode then
    // runs no tick, and no frame starts the player. The player's output after its first frame then has to be archive
    // mode's from frame 0. After the output's end, archive mode has to stay silent, within ±8 with an effect. Such a
    // sequence counts apart, and fails where the two differ.
    if (first == expected.after.size())
    {
        const std::size_t differences = CtrPlayerDifferences(output, 1, expected, 0, &player_first);
        const int limit = effect != nullptr ? 8 : 0;
        const bool sounds_after = std::any_of(
            expected.samples.begin() +
                static_cast<std::ptrdiff_t>(std::min(heard * frame_floats, expected.samples.size())),
            expected.samples.end(), [limit](int16_t sample) { return std::abs(static_cast<int>(sample)) > limit; });
        std::fprintf(stderr, "%s: archive mode runs no tick. %zu of %zu frames differ from the player, first %zu%s.\n",
                     name.c_str(), differences, heard, player_first,
                     sounds_after ? ", and the reference sounds after the output's end" : "");
        totals.tickless++;
        totals.failed += differences > 0 || heard == 0 || sounds_after ? 1 : 0;
        return;
    }
    totals.sent += expected.sent ? 1 : 0;
    std::size_t differences = CtrPlayerDifferences(output, 1, expected, first, &player_first);

    // The output of a sequence without a loop that ends before the check does ends where the reference has fallen
    // silent for good. A loop's output ends at the end of the loop's pass, and the reference plays on round the loop.
    // With an effect, the reference can keep an offset of a step or two, and stays within ±8.
    const std::size_t rest = std::min((first + heard) * frame_floats, expected.samples.size());
    const auto louder_than = [&expected, rest](int limit)
    {
        return std::any_of(expected.samples.begin() + static_cast<std::ptrdiff_t>(rest), expected.samples.end(),
                           [limit](int16_t sample) { return std::abs(static_cast<int>(sample)) > limit; });
    };

    const bool tail = ended && !sequence->loop;
    const bool sounds_on = tail && louder_than(effect != nullptr ? 8 : 0);
    totals.quiet_tails += tail && !sounds_on && louder_than(0) ? 1 : 0;

    // A loop's output reaches the end of the loop's pass.
    const uint64_t loop_end_ms =
        sequence->loop ? static_cast<uint64_t>(song::MsOfTick(*sequence, sequence->loop->end)) : 0;
    const bool early =
        sequence->loop && EndsEarly(first, heard, frames, first + song::FramesIn(loop_end_ms, kCtrFrameMs));

    // A seek to the middle of the output, then one back to the second frame past the silence.
    const std::size_t seeks = CheckCtrSeeks(*model, first, heard, enhanced, expected);

    // A switch halfway through the output, to a new model of the same sequence. In strict mixing, the frame that
    // crossfades sums the two models' frames, and only rounds to the reference's samples. In enhanced mixing, the
    // switch comes from strict mixing, and the player's frames after it are the reference's through a resampler. With
    // an effect, only the new model plays it, and the frames after the one that crossfades have to be the reference's.
    // The output without the effect can end sooner. The switch then comes halfway through that output.
    const std::string switch_text =
        CheckCtrSwitch(archive, sequence, mixing, effects, effect, frames, heard, first, expected, totals);

    const std::string loop = LoopText(sequence->loop);
    if (differences + seeks > 0 || sounds_on || early || heard == 0 || !switch_text.empty())
    {
        std::fprintf(stderr,
                     "%s: %zu of %zu frames differ from the player, first %zu, %zu after seeks%s%s%s (loop %s).\n",
                     name.c_str(), differences, heard, player_first, seeks,
                     sounds_on ? ", and the reference sounds after the output's end" : "",
                     early ? ", and the output ends before the loop's end" : "", switch_text.c_str(), loop.c_str());
        totals.failed++;
    }

    // The notes, up to the output's end. A note that the model cuts in the tick it starts is missing from the
    // arrangement, and inaudible. Missing notes are listed apart from the differences.
    const NoteCheck notes = CheckNotes(expected, *sequence, first + heard);
    totals.notes += notes.checked;
    totals.missed_notes += notes.missed;
    totals.missed_later += notes.missed_later;
    totals.missed_legatos += notes.missed_legatos;
    if (notes.missed > 0)
    {
        std::fprintf(
            stderr,
            "%s (loop %s): %zu of %zu notes missing from the arrangement, %zu of them legatos and %zu after the "
            "first pass.\n",
            name.c_str(), loop.c_str(), notes.missed, notes.checked, notes.missed_legatos, notes.missed_later);
    }
}

} // namespace hokuto::tools::playback
