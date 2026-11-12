// SPDX-License-Identifier: MIT

// The checks of hokuto_check_playback on archives of the Wii.

#include "check_playback.h"

namespace hokuto::tools::playback
{

namespace
{

// The Wii's frames: 96 stereo samples at 32 kHz, in 3 ms.
constexpr std::size_t kWiiFrameSamples = ntdlib::rvl::ax::kSamplesPerFrame;

// The output of WSF's archive mode for a sequence of the Wii, for up to `frames` frames, and the tick count after each
// frame, with the variables' starting values `variables`, the game's `changes` and `effects` on the aux buses. Without
// `microcode`, ntdlib's model of the microcode of the build of AX that `build` gives mixes in place of the DSP. The run
// stops after the first frame in which nothing plays or sounds, or rings on an effect. With an effect, a run goes on
// after the output last had sound for as long as Hokuto's model does (song::WiiRingingFrames). Empty if the sequence
// doesn't start.
WiiURun RunWiiModel(const ntdlib::rsar::SoundArchive& archive, uint32_t sound, std::size_t frames,
                    const ntdlib::Microcode* microcode, ntdlib::SndBuild build, const song::StartValues& variables,
                    const song::Changes& changes, const WiiEffects& effects)
{
    WiiURun run;
    auto model = std::make_unique<ntdlib::rvl::snd::ArchiveModel>();
    model->build = build;
    if (microcode == nullptr)
    {
        model->StartWithRenderer(archive);
    }
    else if (!model->Start(*microcode, archive))
    {
        return run;
    }
    model->engine->SetEffects(effects.setups, {}, sound);
    ntdlib::rvl::snd::SeqPlayer* player = model->engine->StartSequence(sound);
    if (player == nullptr)
    {
        return run;
    }
    for (const auto& [number, value] : variables)
    {
        player->SetVariable(*model->engine, number, value);
    }

    const int ringing_frames = song::WiiRingingFrames(effects.effects);
    int ringing = 0;
    for (std::size_t frame = 0; frame < frames; frame++)
    {
        // The game's changes come before the frame.
        for (const song::VariableChange& change : changes)
        {
            if (song::ChangeFrame(change.time, song::kWiiFrameMs) == frame)
            {
                player->SetVariable(*model->engine, change.variable, change.value);
            }
        }

        if (!model->RunFrame(run.samples))
        {
            break;
        }
        run.after.push_back(player->TickCount());
        const auto last = std::span<const int16_t>(run.samples).last(kWiiFrameSamples * 2);
        ringing = xsflib::rip::Audible(last) ? ringing_frames : std::max(ringing - 1, 0);
        if (!model->engine->IsBusy() && !(model->engine->HasEffects() && ringing > 0))
        {
            break;
        }
    }

    return run;
}

// The player's output for `before` frames on a model of the Wii without effects, then on a model with `effects`, up to
// `frames` frames in all. A catch-up runs the new model up to the player's frame, and the player takes it at its next
// frame. The program does the same after an edit during playback.
std::vector<float> RunWiiSwitch(const std::shared_ptr<const ntdlib::rsar::SoundArchive>& archive,
                                const std::shared_ptr<const song::Sequence>& sequence, const song::AuxEffects& effects,
                                std::size_t before, std::size_t frames)
{
    const auto clock = std::make_shared<song::PlaybackClock>(kWiiFrameSamples);
    song::SequencePlayer player(std::make_shared<song::SequenceModel>(archive, sequence), 0, clock);
    std::vector<float> output;
    if (!RenderFrames(player, before, kWiiFrameSamples, output))
    {
        return output;
    }

    song::ModelCatchUp catch_up;
    catch_up.Start(std::make_shared<song::SequenceModel>(archive, sequence, song::ModelOptions{.effects = effects}),
                   clock);
    while (!catch_up.Finished())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    clock->Offer(catch_up.Take());
    RenderFrames(player, frames, kWiiFrameSamples, output);

    return output;
}

// The frames of `samples` from frame `from` on that differ from `expected`'s frames from `first` on, up to the end of
// either, for the Wii's frames (see WiiUDifferences).
template <class Sample>
std::size_t WiiDifferences(std::span<const Sample> samples, std::size_t from, const WiiURun& expected,
                           std::size_t first, float scale, std::size_t* first_difference = nullptr)
{
    std::size_t differences = 0;
    const std::size_t kFloats = kWiiFrameSamples * 2;
    for (std::size_t frame = from;
         (frame + 1) * kFloats <= samples.size() && (first + frame + 1) * kFloats <= expected.samples.size(); frame++)
    {
        const std::size_t at = (first + frame) * kFloats;
        for (std::size_t i = 0; i < kFloats; i++)
        {
            if (static_cast<float>(samples[frame * kFloats + i]) !=
                static_cast<float>(expected.samples[at + i]) * scale)
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

// The frames that differ from `expected` after seeks of `model` back and forth through its run of `played_frames`
// frames. A seek restores a snapshot that the run took.
std::size_t CheckSnapshotSeeks(song::SequenceModel& model, std::size_t played_frames, const WiiURun& expected)
{
    std::size_t seeks = 0;
    for (const std::size_t frame : {played_frames / 2, played_frames / 4, std::size_t{1}, played_frames * 3 / 4})
    {
        if (frame < played_frames && model.SeekFrame(frame))
        {
            seeks += WiiDifferences(model.Output(), 0, expected, frame, 1.0f);
        }
    }

    return seeks;
}

// The text that the line of a sequence shows for a switch halfway through `sequence` with `effects`, and the frames
// that differ from `expected` after it. `heard` is the length of the player's output without the switch, and `first`
// the model's first frame that runs a tick.
std::pair<std::string, std::size_t> CheckWiiSwitch(const std::shared_ptr<const ntdlib::rsar::SoundArchive>& archive,
                                                   const std::shared_ptr<const song::Sequence>& sequence,
                                                   const WiiEffects& effects, std::size_t frames, std::size_t heard,
                                                   std::size_t first, const WiiURun& expected, Totals& totals)
{
    std::string switch_text = "no switch";
    std::size_t switch_differences = 0;
    const auto halfway = static_cast<std::size_t>(
        song::MsOfTick(*sequence, sequence->length / 2.0) /
        (static_cast<double>(song::kWiiFrameMs.numerator) / static_cast<double>(song::kWiiFrameMs.denominator)));
    std::size_t before = std::min({halfway, frames / 2, heard / 2});
    std::vector<float> switched;
    if (!effects.setups.empty() && before > 1)
    {
        switched = RunWiiSwitch(archive, sequence, effects.effects, before, frames);
    }
    if (!switched.empty() && switched.size() < before * kWiiFrameSamples * 2)
    {
        before = switched.size() / (kWiiFrameSamples * 2) / 2;
        switched = before > 1 ? RunWiiSwitch(archive, sequence, effects.effects, before, frames) : std::vector<float>{};
    }
    if (!switched.empty())
    {
        const std::size_t switched_frames = switched.size() / (kWiiFrameSamples * 2);
        switched.resize(switched_frames > 1 ? (switched_frames - 1) * kWiiFrameSamples * 2 : 0);
        switch_differences =
            WiiDifferences(std::span<const float>(switched), before + 1, expected, first, 1.0f / 32768.0f);
        switch_differences += switched_frames != heard ? 1 : 0;
        totals.switches++;
        totals.exact += switch_differences == 0 ? 1 : 0;
        switch_text = std::to_string(switch_differences) + " frames that differ after a switch";
    }

    return {switch_text, switch_differences};
}

// Checks Hokuto's playback of sequence sound `sound` of a Wii archive against WSF's archive mode, with the variables'
// starting values `values`, the game's `changes` and `effects`: the frames of a run from the start, the frames after
// seeks back and forth, and the player's output after its first frame. That frame fades in. Hokuto's run ends after a
// silent frame once nothing plays, sounds or rings on an effect. The expected output after it has to be silent, and so
// does Hokuto's output after the reference's end. With an effect, a frame within ±8 counts as silent, and the run has
// to end in the reference's last frame: the frame in which the effects stop ringing. A switch halfway through the
// sequence follows, from a model without the effect. Every frame of the Wii mixes: after the frame that crossfades, the
// output has to be the reference's. Prints a line when anything differs. Without `microcode`, the reference mixes with
// ntdlib's model of the microcode.
void CheckWiiSequence(const std::shared_ptr<const ntdlib::rsar::SoundArchive>& archive, uint32_t sound,
                      std::size_t frames, const ntdlib::Microcode* microcode, ntdlib::SndBuild build,
                      const song::StartValues& values, const song::Changes& changes, const WiiEffects& effects,
                      Totals& totals)
{
    const std::string name = archive->Sounds()[sound].name + ValuesText(values) + ChangesText(changes);
    std::shared_ptr<const song::Sequence> sequence;
    try
    {
        sequence = std::make_shared<const song::Sequence>(song::ReadSequence(*archive, sound, values, changes, build));
    }
    catch (const std::exception& error)
    {
        std::printf("%s: can't be read. %s\n", name.c_str(), error.what());
        totals.unread++;
        return;
    }

    totals.sequences++;
    totals.looped += sequence->loop ? 1 : 0;
    const WiiURun expected = RunWiiModel(*archive, sound, frames, microcode, build, values, changes, effects);
    if (!changes.empty() &&
        RunWiiModel(*archive, sound, frames, microcode, build, values, {}, effects).samples != expected.samples)
    {
        totals.affected++;
    }
    const auto model =
        std::make_shared<song::SequenceModel>(archive, sequence, song::ModelOptions{.effects = effects.effects});
    const std::vector<int16_t> played = RunModelFrames(*model, frames * kWiiFrameSamples * 2);

    // The reference stops once nothing plays or sounds. The output of either past the end of the other has to be
    // silent.
    std::size_t differences = WiiDifferences(std::span<const int16_t>(played), 0, expected, 0, 1.0f);
    const std::size_t played_frames = played.size() / (kWiiFrameSamples * 2);
    const std::size_t rest = std::min(played.size(), expected.samples.size());
    const auto silent_from = [rest](std::span<const int16_t> samples)
    {
        return std::all_of(samples.begin() + static_cast<std::ptrdiff_t>(rest), samples.end(),
                           [](int16_t sample) { return sample == 0; });
    };
    if (!silent_from(expected.samples) || !silent_from(played) ||
        (!effects.setups.empty() && played.size() != expected.samples.size()))
    {
        differences++;
    }

    // Seeks back and forth restore the snapshots that the run took.
    const std::size_t seeks = CheckSnapshotSeeks(*model, played_frames, expected);

    // The player starts at the model's first frame that runs a tick. The worker renders in blocks.
    const std::size_t first = FirstTickFrame(expected.after);
    std::vector<float> output = RunPlayerBlocks(model, frames * kWiiFrameSamples * 2);
    const std::size_t heard = output.size() / (kWiiFrameSamples * 2);
    // The output reaches the frame where the model's run ends. A looped sequence's output ends with the frame that
    // reaches the end of the loop's pass.
    const bool looped = sequence->loop && sequence->loop->end > sequence->loop->start;
    const bool early =
        EndsEarly(first, heard, frames, looped ? LoopEndFrames(expected.after, sequence->loop->end) : played_frames);
    output.resize(heard > 1 ? (heard - 1) * kWiiFrameSamples * 2 : 0);
    std::size_t player_first = 0;
    std::size_t player_differences =
        WiiDifferences(std::span<const float>(output), 1, expected, first, 1.0f / 32768.0f, &player_first);
    const std::size_t compared = std::min(output.size(), expected.samples.size() - first * kWiiFrameSamples * 2);
    if (std::any_of(output.begin() + static_cast<std::ptrdiff_t>(compared), output.end(),
                    [](float sample) { return sample != 0.0f; }))
    {
        player_differences++;
    }

    // The effect alters a run whose output differs from the model's without the effect.
    if (!effects.setups.empty())
    {
        const auto dry = std::make_shared<song::SequenceModel>(archive, sequence);
        const std::vector<int16_t> dry_played = RunModelFrames(*dry, frames * kWiiFrameSamples * 2);
        totals.altered += dry_played != played ? 1 : 0;
    }

    // A switch halfway through the sequence, or halfway through the output when the output ends sooner. It ends where
    // the output without the switch ends. The output before the switch has no effect, and can end sooner than the
    // output with it. The switch then comes halfway through that output, as on the Wii U.
    const auto [switch_text, switch_differences] =
        CheckWiiSwitch(archive, sequence, effects, frames, heard, first, expected, totals);

    if (differences + seeks + player_differences + switch_differences > 0 || expected.samples.empty() || early)
    {
        std::printf(
            "%s: %zu of %zu frames differ from the start, %zu after seeks, %zu of %zu from the player, first "
            "%zu, %s (loop %s).%s\n",
            name.c_str(), differences, played_frames, seeks, player_differences, heard, player_first,
            switch_text.c_str(), LoopText(sequence->loop).c_str(),
            early ? " The player's output ends before the model's." : "");
        totals.failed++;
    }
    std::fflush(stdout);
}

} // namespace

int CheckWiiArchive(const std::filesystem::path& path, const std::filesystem::path& dol_path, int seconds,
                    bool variables, bool changes, bool archive_build, std::size_t part, std::size_t parts,
                    const WiiEffects& effects, std::string_view effect_name)
{
    std::vector<uint8_t> bytes;
    std::vector<uint8_t> dol;
    if (!xsflib::ReadFile(path, bytes) || !xsflib::ReadFile(dol_path, dol))
    {
        std::fprintf(stderr, "The archive or the main.dol can't be read.\n");
        return 2;
    }
    const std::optional<ntdlib::Microcode> microcode = ntdlib::FindMicrocode(dol);
    if (!microcode)
    {
        std::fprintf(stderr, "The main.dol holds no AX microcode.\n");
        return 2;
    }
    std::shared_ptr<const ntdlib::rsar::SoundArchive> archive;
    try
    {
        archive = song::LoadWiiArchive(std::move(bytes), xsflib::Utf8(path.filename()), ntdlib::rvl::snd::kRsarOptions);
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "%s\n", error.what());
        return 2;
    }

    Totals totals;
    const ntdlib::SndBuild build = archive_build ? ntdlib::sequence_export::BuildOfRsarVersion(archive->Version())
                                                 : song::ReadWiiBuild(dol).value_or(ntdlib::SndBuild{});
    const auto frames =
        static_cast<std::size_t>(song::FramesIn(static_cast<uint64_t>(seconds) * 1000, song::kWiiFrameMs));
    std::size_t reading = 0;
    for (std::size_t sound = 0; sound < archive->Sounds().size(); sound++)
    {
        if (archive->Sounds()[sound].type != ntdlib::rsar::SoundType::kSequence || sound % parts != part)
        {
            continue;
        }

        const auto index = static_cast<uint32_t>(sound);
        if (!variables && !changes)
        {
            CheckWiiSequence(archive, index, frames, &*microcode, build, {}, {}, effects, totals);
            continue;
        }

        // A sequence that can't be read gets a line from the run without values.
        song::Sequence plain;
        try
        {
            plain = song::ReadSequence(*archive, index, {}, {}, build);
        }
        catch (const std::exception&)
        {
            CheckWiiSequence(archive, index, frames, nullptr, build, {}, {}, effects, totals);
            continue;
        }

        // A change comes 2 ms into the frame halfway through the sequence's length or through the check.
        const std::vector<song::Variable>& read = changes ? plain.read_variables : plain.variables;
        const double middle = std::min(song::MsOfTick(plain, plain.length / 2.0), seconds * 500.0);
        const uint64_t frame = song::FramesIn(static_cast<uint64_t>(middle), song::kWiiFrameMs);
        const auto time =
            static_cast<uint32_t>(frame * song::kWiiFrameMs.numerator / song::kWiiFrameMs.denominator + 2);
        reading += read.empty() ? 0 : 1;
        ForEachChoice(read, changes, time, [&](const song::StartValues& values, const song::Changes& changed)
                      { CheckWiiSequence(archive, index, frames, nullptr, build, values, changed, effects, totals); });
    }

    const std::string runs = variables || changes ? std::to_string(reading) + " sequences read variables. Their " : "";
    const char* checked = changes ? "runs with a change were" : variables ? "runs with a choice were" : "Wii sequences";
    const std::string build_text = build.date != 0      ? "NW4R SND of " + std::to_string(build.date)
                                   : build.october_2006 ? std::string("NW4R SND of 2006, without a banner")
                                                        : std::string("NW4R SND of 2007, without a banner");
    std::printf("%s%zu %s checked for %d s against %s and %s, %zu with a loop. %zu differ, and %zu can't be read.\n",
                runs.c_str(), totals.sequences, checked, seconds,
                variables || changes                                                 ? "ntdlib's model of the microcode"
                : ntdlib::IdentifyBuild(*microcode) == ntdlib::AxBuild::kOctober2006 ? "the microcode of October 2006"
                                                                                     : "the microcode of December 2006",
                build_text.c_str(), totals.looped, totals.failed, totals.unread);
    if (changes)
    {
        std::printf("The change altered the model's output in %zu runs.\n", totals.affected);
    }
    if (!effects.setups.empty())
    {
        std::printf(
            "With %s on bus A, the effect altered the output of %zu runs, and %zu of %zu switches came out "
            "exact.\n",
            std::string(effect_name).c_str(), totals.altered, totals.exact, totals.switches);
    }

    return totals.failed == 0 && totals.unread == 0 ? 0 : 1;
}

} // namespace hokuto::tools::playback
