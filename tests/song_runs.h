// SPDX-License-Identifier: MIT

// This header contains the runs of the models and players, and the checks of their output, that the tests of the songs
// share.

#pragma once

#include "driver_test_support.h"
#include "song_archives.h"

namespace
{

using hokuto::testing::HasSound;
using hokuto::testing::TakeRead;
using xsflib::testing::ErrorOf;

// A model for a sequence's players, with snapshots every `interval` frames.
inline std::shared_ptr<SequenceModel> ModelOf(std::shared_ptr<const sar::SoundArchive> archive,
                                              std::shared_ptr<const Sequence> sequence,
                                              uint64_t interval = SequenceModel::kSnapshotInterval,
                                              AuxEffects effects = {}, Mixing mixing = Mixing::kStrict)
{
    return std::make_shared<SequenceModel>(
        std::move(archive), std::move(sequence),
        hokuto::song::ModelOptions{.effects = std::move(effects), .mixing = mixing, .snapshot_interval = interval});
}

// The model's output for `frames` frames of a sequence sound, run directly, with the aux buses at `volumes`. The
// enhanced mode's output in floating point too. For each frame, the sequence player's tick count before and after it,
// whether the engine was busy before it, and whether the sequence had ended before it.
struct ModelRun
{
    std::vector<int16_t> samples;
    std::vector<float> floats; // none in the console's mode
    std::vector<uint64_t> before;
    std::vector<uint64_t> after;
    std::vector<bool> busy;
    std::vector<bool> finished;
};

// The ModelRun of sound `sound` of `archive` for `frames` frames, with the engine's `options`, the starting values
// `variables`, the `changes` and the aux buses at `volumes`.
inline ModelRun RunModel(const sar::SoundArchive& archive, uint32_t sound, std::size_t frames,
                         const atk::EngineOptions& options = {}, const StartValues& variables = {},
                         const Changes& changes = {}, const AuxVolumes& volumes = kFullVolumes)
{
    // Sub-mix 0 holds the main bus, then the aux buses. Each bus's channels reach the final mix at the bus's volume.
    ModelRun run;
    atk::ArchiveModel model(archive, options);
    const int channels = model.sub_mix.channel_count;
    for (std::size_t bus = 0; bus < kAuxBuses; bus++)
    {
        for (int channel = 0; channel < channels; channel++)
        {
            const int buffer = (1 + static_cast<int>(bus)) * channels + channel;
            model.renderer.SetSubMixMixVolume(model.sub_mix.mix, volumes[bus], buffer, channel);
        }
    }
    atk::SequenceSoundPlayer* player = model.engine->StartSequence(sound);
    if (player == nullptr)
    {
        return run;
    }

    // A game sets the variables through the sound's handle before the sound's first frame.
    for (const auto& [number, value] : variables)
    {
        player->SetVariable(*model.engine, number, value);
    }

    // A game makes a change through the sound's handle too. The change comes before the ticks of its frame.
    for (std::size_t i = 0; i < frames; i++)
    {
        for (const VariableChange& change : changes)
        {
            if (ChangeFrame(change.time, kFrameMs) == i)
            {
                player->SetVariable(*model.engine, change.variable, change.value);
            }
        }
        run.busy.push_back(model.engine->IsBusy());
        run.finished.push_back(player->IsFinished());
        run.before.push_back(player->TickCount());
        model.engine->RunFrame();
        run.after.push_back(player->TickCount());
        const std::span<const int16_t> output = model.renderer.DeviceOutput();
        run.samples.insert(run.samples.end(), output.begin(), output.end());
        const std::span<const float> floats = model.renderer.DeviceOutputFloat();
        run.floats.insert(run.floats.end(), floats.begin(), floats.end());
    }

    return run;
}

// The first frame of `run` after which the tick count has reached `ticks`, or the number of frames.
inline std::size_t FirstFrameReaching(const ModelRun& run, uint64_t ticks)
{
    std::size_t frame = 0;
    while (frame < run.after.size() && run.after[frame] < ticks)
    {
        frame++;
    }

    return frame;
}

// The frames of a run of a sequence without a loop that matter at its end: the first frame that plays a tick, the first
// frame after the end of the sequence, and the first frame after the end of the last note's release.
struct EndFrames
{
    std::size_t first = 0;
    std::size_t finished = 0;
    std::size_t idle = 0;
};

// The EndFrames of `model`, a run of a sequence without a loop.
inline EndFrames EndFramesOf(const ModelRun& model)
{
    EndFrames ends;
    ends.first = FirstFrameReaching(model, 1);
    const auto finished = std::find(model.finished.begin(), model.finished.end(), true);
    ends.finished = static_cast<std::size_t>(finished - model.finished.begin());
    const auto idle = std::find(model.busy.begin() + static_cast<std::ptrdiff_t>(ends.first), model.busy.end(), false);
    ends.idle = static_cast<std::size_t>(idle - model.busy.begin());

    return ends;
}

// The position before frame `frame` of the run, from the first frame after the end of the sequence on. The position
// goes on from the end at the tempo there: 0.48 ticks a frame.
inline double PositionBefore(const ModelRun& model, const EndFrames& ends, std::size_t frame)
{
    const auto frames_past_end = static_cast<double>(frame - ends.finished);
    return static_cast<double>(model.after[ends.finished - 1]) + frames_past_end * 0.48;
}

// The first frame of the run, from the end of the sequence on, after which the position is past `ticks`. That frame
// plays tick `ticks`.
inline std::size_t FramePlayingPastEnd(const ModelRun& model, const EndFrames& ends, double ticks)
{
    std::size_t frame = ends.finished;
    while (PositionBefore(model, ends, frame + 1) <= ticks)
    {
        frame++;
    }

    return frame;
}

// A player's output, rendered in blocks of 256 stereo frames. The render worker renders in blocks too. The clock's
// position at the start of each frame is read after each block. The program reads it soon after the block too. `ended`
// says whether a block came back short.
struct PlayerRun
{
    std::vector<float> samples;
    std::vector<std::optional<double>> marks;
    std::vector<PlaybackClock::Peaks> peaks;
    std::vector<std::optional<PlaybackClock::Variables>> variables;
    bool ended = false;
};

// The PlayerRun of `player` for `frames` frames, with the positions, peaks and variables that `clock` gives.
inline PlayerRun RunPlayer(SequencePlayer& player, const PlaybackClock& clock, std::size_t frames)
{
    constexpr std::size_t kBlock = 256;
    PlayerRun run;
    const std::size_t total = frames * kFrameSamples;
    run.samples.resize(total * 2);
    std::size_t done = 0;
    while (done < total && !run.ended)
    {
        const std::size_t block = std::min(kBlock, total - done);
        const std::size_t rendered = player.Render(run.samples.data() + done * 2, block);
        done += rendered;
        run.ended = rendered < block;
        while (run.marks.size() * kFrameSamples < done)
        {
            const uint64_t start = run.marks.size() * kFrameSamples;
            run.marks.push_back(clock.PositionAt(start));
            run.peaks.push_back(clock.PeaksBetween(start, start + kFrameSamples));
            run.variables.push_back(clock.VariablesAt(start));
        }
    }
    run.samples.resize(done * 2);

    return run;
}

// `count` frames of a player's output from frame `from`.
inline std::vector<float> FramesOf(const PlayerRun& run, std::size_t from, std::size_t count)
{
    const auto at = [&run](std::size_t frame)
    {
        return run.samples.begin() + static_cast<std::ptrdiff_t>(frame * kFrameSamples * 2);
    };

    return std::vector<float>(at(from), at(from + count));
}

// The peaks of frame `frame` of a player's output, left and right.
inline std::array<float, 2> OutputPeaks(const std::vector<float>& samples, std::size_t frame)
{
    std::array<float, 2> peaks = {0.0f, 0.0f};
    for (std::size_t i = 0; i < kFrameSamples * 2; i++)
    {
        float& peak = peaks[i % 2];
        peak = std::max(peak, std::abs(samples[frame * kFrameSamples * 2 + i]));
    }

    return peaks;
}

// The frames of a run, from frame `first` on, in which track 0's meter differs from the peaks of the output.
inline std::size_t MeterMismatches(const PlayerRun& run, std::size_t first)
{
    std::size_t wrong = 0;
    for (std::size_t frame = first; frame < run.peaks.size(); frame++)
    {
        if (run.peaks[frame].tracks[0] != OutputPeaks(run.samples, frame))
        {
            wrong++;
        }
    }

    return wrong;
}

// Whether the meters of tracks `first` to 15 stay silent in each frame of a run.
inline bool TracksSilent(const PlayerRun& run, std::size_t first)
{
    constexpr std::array<float, 2> kSilent = {0.0f, 0.0f};
    for (const PlaybackClock::Peaks& peaks : run.peaks)
    {
        for (std::size_t track = first; track < peaks.tracks.size(); track++)
        {
            if (peaks.tracks[track] != kSilent)
            {
                return false;
            }
        }
    }

    return true;
}

// A sample of the model's output as the player writes it.
inline float SampleOf(const ModelRun& run, std::size_t frame, std::size_t index)
{
    return static_cast<float>(run.samples[frame * kFrameSamples * 2 + index]) / 32768.0f;
}

// Frame `frame` of the model's output.
inline std::span<const int16_t> ModelFrame(const ModelRun& run, std::size_t frame)
{
    return std::span<const int16_t>(run.samples).subspan(frame * kFrameSamples * 2, kFrameSamples * 2);
}

// Whether `samples` are the model's output from frame `first` of `run`, after the first `skip` samples. Output that
// ends within the first `skip` samples doesn't match.
inline bool Matches(const std::vector<float>& samples, const ModelRun& run, std::size_t first, std::size_t skip)
{
    if (samples.size() <= skip || first * kFrameSamples * 2 + samples.size() > run.samples.size())
    {
        return false;
    }

    for (std::size_t i = skip; i < samples.size(); i++)
    {
        if (samples[i] != SampleOf(run, first, i))
        {
            return false;
        }
    }

    return true;
}

// Whether `samples` hold frame `frame`, and `run` holds frame `model_frame`.
inline bool HoldsFrames(const std::vector<float>& samples, std::size_t frame, const ModelRun& run,
                        std::size_t model_frame)
{
    const std::size_t kLength = kFrameSamples * 2;
    return (frame + 1) * kLength <= samples.size() && (model_frame + 1) * kLength <= run.samples.size();
}

// Whether a frame of output fades in from silence or out to it, in a straight line over the frame: `frame` of the
// samples against frame `model_frame` of the model's output. A frame that either lacks doesn't fade.
inline bool Fades(const std::vector<float>& samples, std::size_t frame, const ModelRun& run, std::size_t model_frame,
                  bool fade_in)
{
    if (!HoldsFrames(samples, frame, run, model_frame))
    {
        return false;
    }

    for (std::size_t i = 0; i < kFrameSamples * 2; i++)
    {
        const float through = static_cast<float>(i / 2) / static_cast<float>(kFrameSamples);
        const float expected = SampleOf(run, model_frame, i) * (fade_in ? through : 1.0f - through);
        if (samples[frame * kFrameSamples * 2 + i] != expected)
        {
            return false;
        }
    }

    return true;
}

// Whether frame `frame` of `samples` crossfades from frame `model_frame` of `from` to the same frame of `to`, in a
// straight line over the frame. A frame that any of them lacks doesn't crossfade.
inline bool Crossfades(const std::vector<float>& samples, std::size_t frame, const ModelRun& from, const ModelRun& to,
                       std::size_t model_frame)
{
    if (!HoldsFrames(samples, frame, from, model_frame) || !HoldsFrames(samples, frame, to, model_frame))
    {
        return false;
    }

    for (std::size_t i = 0; i < kFrameSamples * 2; i++)
    {
        const float through = static_cast<float>(i / 2) / static_cast<float>(kFrameSamples);
        const float out = SampleOf(from, model_frame, i) * (1.0f - through);
        const float in = SampleOf(to, model_frame, i) * through;
        if (samples[frame * kFrameSamples * 2 + i] != in + out)
        {
            return false;
        }
    }

    return true;
}

// Whether frame `frame` of `samples` ramps in from silence to frame `model_frame` of the model's output, in a straight
// line over the frame: the ramp after an unmute. The model ramps each voice before it mixes them, in fixed point. The
// samples therefore lie within 3/32768 of the line, not on it. A frame that either lacks doesn't ramp in.
inline bool RampsIn(const std::vector<float>& samples, std::size_t frame, const ModelRun& run, std::size_t model_frame)
{
    if (!HoldsFrames(samples, frame, run, model_frame))
    {
        return false;
    }

    for (std::size_t i = 0; i < kFrameSamples * 2; i++)
    {
        const float through = static_cast<float>(i / 2) / static_cast<float>(kFrameSamples);
        const float expected = SampleOf(run, model_frame, i) * through;
        if (std::abs(samples[frame * kFrameSamples * 2 + i] - expected) > 3.0f / 32768.0f)
        {
            return false;
        }
    }

    return true;
}

// The largest sample of `samples`, positive or negative.
inline float Peak(std::span<const float> samples)
{
    float peak = 0.0f;
    for (const float sample : samples)
    {
        peak = std::max(peak, std::abs(sample));
    }

    return peak;
}

// The ratio of the power of `expected` to the power of the difference of `samples` from it, in dB. Samples that are the
// same give infinity.
inline double SignalToDifference(std::span<const int16_t> samples, std::span<const int16_t> expected)
{
    double signal = 0.0;
    double difference = 0.0;
    for (std::size_t i = 0; i < samples.size() && i < expected.size(); i++)
    {
        const double wanted = expected[i];
        const double error = samples[i] - wanted;
        signal += wanted * wanted;
        difference += error * error;
    }

    return difference > 0.0 ? 10.0 * std::log10(signal / difference) : std::numeric_limits<double>::infinity();
}

// Whether `part` runs from tick `start` to tick `end`, with the `count` notes from note `first`.
inline bool IsPart(const Part& part, uint32_t start, uint32_t end, std::size_t first, std::size_t count)
{
    return part.start == start && part.end == end && part.first == first && part.count == count;
}

// Whether `note` starts at tick `start` and lasts `length` ticks, with key `key` and velocity `velocity`.
inline bool IsNote(const Note& note, uint32_t start, uint32_t length, uint8_t key, uint8_t velocity)
{
    return note.start == start && note.length == length && note.key == key && note.velocity == velocity;
}

// Whether `function` throws std::runtime_error, the error that the program shows the user.
template <typename Function>
bool ThrowsRuntimeError(Function function)
{
    return !ErrorOf<std::runtime_error>(function).empty();
}

// `bytes` cut short halfway through the first run of `file` in them.
inline std::vector<uint8_t> CutInside(std::vector<uint8_t> bytes, std::span<const uint8_t> file)
{
    const auto found = std::ranges::search(bytes, file);
    bytes.resize(static_cast<std::size_t>(found.begin() - bytes.begin()) + file.size() / 2);

    return bytes;
}

// A preview's output, rendered in blocks of 256 stereo frames, up to `frames` frames of the model or to its end. A
// frame of the model holds `frame_samples` stereo frames.
inline std::vector<float> RunPreview(hokuto::audio::Renderer& preview, std::size_t frames,
                                     std::size_t frame_samples = kFrameSamples)
{
    constexpr std::size_t kBlock = 256;
    const std::size_t total = frames * frame_samples;
    std::vector<float> samples(total * 2);
    std::size_t done = 0;
    while (done < total)
    {
        const std::size_t block = std::min(kBlock, total - done);
        const std::size_t rendered = preview.Render(samples.data() + done * 2, block);
        done += rendered;
        if (rendered < block)
        {
            break;
        }
    }
    samples.resize(done * 2);

    return samples;
}

// An instrument with a sound on each key from `lo` to `hi` of each of `ranges`.
inline sar::Instrument InstrumentOn(std::initializer_list<std::pair<int, int>> ranges)
{
    sar::Instrument instrument;
    for (const auto& [lo, hi] : ranges)
    {
        sar::KeyRegion key;
        key.lo = lo;
        key.hi = hi;
        key.velocities.push_back({0, 127, sar::VelocityRegion{}});
        instrument.keys.push_back(key);
    }

    return instrument;
}

// Whether the reader's conversion `converted` is `direct`, a read of the same sound, with the map of the arrangement's
// ticks that scales them by the timebases (song::TickMap). The sequences of the tests play their quarter notes at the
// same times as the arrangement does.
inline bool ConvertedAs(const Sequence& converted, const Sequence& direct)
{
    Sequence without = converted;
    without.arrangement_ticks.reset();
    return without == direct && converted.arrangement_ticks && !converted.arrangement_ticks->ByTime();
}

// Waits up to 30 s for the catch-up's run to stop, and takes its model. Returns nullptr if the run goes on.
inline std::shared_ptr<SequenceModel> WaitForCatchUp(ModelCatchUp& catch_up)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (catch_up.Finished())
        {
            return catch_up.Take();
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    return nullptr;
}

// The reverb that the tests of effects put on aux bus A: nn::audio's preset 1, with a decay of 1.5 s. AuxReverb gives
// it to the model run directly, and AuxEffectsWithReverb to a SequenceModel.
const ntdlib::nx::audio::I3dl2ReverbParameterSet& Reverb()
{
    return ntdlib::nx::audio::kI3dl2ReverbPresets[1];
}

// The engine's options of a model run directly, with Reverb on aux bus A.
inline atk::EngineOptions AuxReverb()
{
    atk::EngineOptions options;
    options.aux_reverbs[0] = Reverb();

    return options;
}

// The effects of a SequenceModel, with Reverb on aux bus A.
inline AuxEffects AuxEffectsWithReverb()
{
    return {{{Reverb()}, {}, {}}};
}

// A file that an export writes beside an archive, with its name and its bytes. PlaceGroupFiles takes and gives files in
// this form.
using Output = std::pair<std::string, std::vector<uint8_t>>;

// A note of 48 ticks.
const std::vector<uint8_t> kWiiNoteBytecode = {0x3C, 0x64, 0x30, 0xFF};

// kChangesBytecode as the Wii holds it, after a command that allocates track 0 (0xFE): the arguments of the allocation,
// the tempo, the comparison and the jumps are big-endian. The test archive's labels lie at bytes 0 and 8, at the starts
// of commands. The comparison starts at byte 10, and the note of key 72 at byte 27.
const std::vector<uint8_t> kWiiChangesBytecode = {0xFE, 0x00, 0x01, 0xB0, 48,   0xE1, 0x00, 120,  0x80, 1,   0xF0, 0x90,
                                                  0,    0x00, 1,    0xA2, 0x89, 0x00, 0x00, 27,   60,   100, 48,   0x89,
                                                  0x00, 0x00, 10,   72,   100,  48,   0x89, 0x00, 0x00, 10};

// The bytes of a Wii archive whose sequence plays `commands` on a looped wave: by default, a note of 48 ticks. ntdlib's
// test archive holds silent waves of DSP-ADPCM. Here each frame of the waves gives the samples of its nibbles at a
// scale of 2^11. The note's release sounds on for longer than the tests play it. Without `loop`, the wave plays its 112
// samples once. OpenWiiArchive opens the archive.
inline std::vector<uint8_t> WiiArchiveBytes(const std::vector<uint8_t>& commands = kWiiNoteBytecode, bool loop = true)
{
    namespace rsar_files = ntdlib::testing::rsar_files;
    std::vector<uint8_t> bytes = rsar_files::MakeArchive(rsar_files::MakeSequence(commands), 0, 1, loop);
    constexpr std::array<uint8_t, 7> kNibbles = {0x7F, 0x19, 0xE3, 0x5A, 0xC6, 0x2D, 0x91};
    for (std::size_t at = 0; at + 4 <= bytes.size(); at++)
    {
        if (std::memcmp(&bytes[at], "RWAV", 4) != 0)
        {
            continue;
        }

        // The wave's DATA block holds 0x40 bytes of samples after its header of 8 bytes.
        const uint32_t data = xsflib::LoadBe32(&bytes[at + 0x18]);
        for (std::size_t frame = 0; frame < 0x40; frame += 8)
        {
            uint8_t* samples = &bytes[at + data + 8 + frame];
            samples[0] = 0x0B;
            std::copy(kNibbles.begin(), kNibbles.end(), samples + 1);
        }
    }

    return bytes;
}

// The Wii archive of WiiArchiveBytes with `commands` and `loop`. The program opens an archive the same way.
inline std::shared_ptr<const ntdlib::rsar::SoundArchive> OpenWiiArchive(
    const std::vector<uint8_t>& commands = kWiiNoteBytecode, bool loop = true)
{
    return LoadWiiArchive(WiiArchiveBytes(commands, loop), "SOUND.brsar", ntdlib::rvl::snd::kRsarOptions);
}

// The output of `frames` frames of ntdlib's model of the Wii U, run directly from the start of sound `sound`, with
// `effects` on the aux buses. The TV's bus A returns at `return_volume`, in 1.15. With `enhanced`, the model mixes in
// its enhanced mode.
inline std::vector<int16_t> RunWiiUModel(const sar::SoundArchive& archive, uint32_t sound, std::size_t frames,
                                         std::vector<cafe::EffectSetup> effects = {},
                                         uint16_t return_volume = ntdlib::cafe::ax::kUnityVolume, bool enhanced = false)
{
    cafe::EngineOptions options;
    options.enhanced = enhanced;
    options.effects = std::move(effects);
    cafe::ArchiveModel model(archive, std::move(options));
    model.system.SetAuxReturnVolume(ntdlib::cafe::ax::System::Device::kTv, 0, 0, return_volume);
    std::vector<int16_t> samples;
    if (model.engine->StartSequence(sound) == nullptr)
    {
        return samples;
    }

    for (std::size_t frame = 0; frame < frames; frame++)
    {
        model.engine->RunFrame();
        const std::span<const int16_t> output = model.system.Output();
        samples.insert(samples.end(), output.begin(), output.end());
    }

    return samples;
}

// The output of `model` from its first frame until its output ends, or until `samples` samples.
inline std::vector<int16_t> RunToEnd(SequenceModel& model, std::size_t samples)
{
    std::vector<int16_t> played;
    for (bool more = model.SeekFrame(0); more && played.size() < samples; more = model.RunFrame())
    {
        const std::span<const int16_t> output = model.Output();
        played.insert(played.end(), output.begin(), output.end());
    }

    return played;
}

// The output of `frames` frames of ntdlib's model of the Wii, run directly from the start of the archive's sound 0,
// with `effects` on the aux buses. Bus A returns at `return_volume`, in 1.15. The model follows `build` of NW4R SND, or
// else the build that the archive's version gives.
inline std::vector<int16_t> RunWiiModel(const ntdlib::rsar::SoundArchive& archive, std::size_t frames,
                                        const std::vector<ntdlib::rvl::snd::EffectSetup>& effects = {},
                                        uint16_t return_volume = ntdlib::rvl::ax::kMaxVolume,
                                        const std::optional<ntdlib::SndBuild>& build = {})
{
    ntdlib::rvl::snd::ArchiveModel model;
    model.build = build.value_or(export_::BuildOfRsarVersion(archive.Version()));
    model.StartWithRenderer(archive);
    model.engine->SetEffects(effects, {}, 0);
    std::vector<int16_t> samples;
    if (model.engine->StartSequence(0) == nullptr)
    {
        return samples;
    }

    // NW4R SND returns the bus at full volume as it appends the bus's first effect, in the first frame. A game sets the
    // return volume after that.
    for (std::size_t frame = 0; frame < frames; frame++)
    {
        model.ax.SetAuxReturnVolume(0, return_volume);
        if (!model.RunFrame(samples))
        {
            break;
        }
    }

    return samples;
}

// The frame of a run before which a model with effects stops: the first one after the start before which the engine is
// idle and after a frame that came out silent.
inline std::size_t TailEnd(const ModelRun& run, std::size_t first)
{
    std::size_t frame = first + 1;

    const auto silent = [&run](std::size_t f)
    {
        return !HasSound(std::span<const int16_t>(run.samples).subspan(f * kFrameSamples * 2, kFrameSamples * 2));
    };
    while (frame < run.busy.size() && (run.busy[frame] || !silent(frame - 1)))
    {
        frame++;
    }

    return frame;
}

// The sounds of WaveSoundArchiveSpec, after the test archive's four sequence sounds.
constexpr uint32_t kHighSound = 4;
constexpr uint32_t kPannedSound = 5;
constexpr uint32_t kSilentSound = 6;
constexpr uint32_t kPastSound = 7;
constexpr uint32_t kLostSound = 8;
constexpr uint32_t kPlainSound = 9;
constexpr uint32_t kNoNoteSound = 10;
constexpr uint32_t kNoWaveSound = 11;
constexpr uint32_t kBackwardsSound = 12;

// The test archive with a wave sound file, file 6, after the four sequence sounds, and a second wave archive, file 7,
// with the test's waves the other way round. The wave sounds play waves of the second wave archive. WSD_HIGH plays the
// stereo wave at the sound's pitch of 1.5, with the sound's pan. Its note's envelope sustains below full level.
// WSD_PANNED's WAV file holds the saw at its note's pitch of 0.5, and the note sets a pan. The sound's envelope fades
// it in. Its first note event comes in its second track, and its third track holds two more. WSD_PLAIN plays the saw at
// a pitch just over 1, at 48,000.7 Hz. Each of the others fails. WSD_SILENT plays no note. WSD_PAST names a wave sound
// past the file's eight. WSD_LOST plays a wave of a third wave archive, and the archive lacks it. WSD_NO_NOTE's event
// names a note that the sound lacks. WSD_NO_WAVE's note names a wave that the file lacks. WSD_BACKWARDS's pitch is -1.
inline sar_files::ArchiveSpec WaveSoundArchiveSpec(bool big_endian)
{
    sar_files::Region stereo;
    stereo.wave = 1;
    stereo.adshr = std::array<uint8_t, 5>{127, 127, 100, 127, 127};
    sar_files::Region low;
    low.pitch = 0.5f;
    low.pan = 100;
    sar_files::Region other;
    other.pitch = 2.0f;
    sar_files::Region lost;
    lost.wave = 2;
    sar_files::Region past;
    past.wave = 3;
    const sar_files::NoteEventSpec kFirst{0.0f, 0.0f, 0};
    sar_files::WaveSoundSpec high;
    high.pan = 30;
    high.pitch = 1.5f;
    high.tracks = {{kFirst}};
    high.notes = {stereo};
    sar_files::WaveSoundSpec panned;
    panned.adshr = std::array<uint8_t, 5>{100, 127, 127, 127, 127};
    panned.tracks = {{}, {kFirst}, {{0.0f, 0.0f, 1}, {0.5f, 0.0f, 0}}};
    panned.notes = {low, other};
    sar_files::WaveSoundSpec silent;
    silent.tracks = {{}};
    sar_files::WaveSoundSpec lost_sound = high;
    lost_sound.notes = {lost};
    sar_files::WaveSoundSpec plain;
    plain.pitch = 1.0000146f;
    plain.tracks = {{kFirst}};
    plain.notes = {sar_files::Region{}};
    sar_files::WaveSoundSpec no_note;
    no_note.tracks = {{kFirst}};
    sar_files::WaveSoundSpec no_wave = plain;
    no_wave.notes = {past};
    sar_files::WaveSoundSpec backwards = plain;
    backwards.pitch = -1.0f;
    sar_files::WaveSoundFileSpec file;
    file.waves = {{0x05000001, 1}, {0x05000001, 0}, {0x05000002, 0}};
    file.sounds = {high, panned, silent, lost_sound, plain, no_note, no_wave, backwards};

    sar_files::ArchiveSpec spec = TestArchiveSpec(big_endian);
    const std::array<std::pair<const char*, uint32_t>, 9> kSounds = {{{"WSD_HIGH", 0},
                                                                      {"WSD_PANNED", 1},
                                                                      {"WSD_SILENT", 2},
                                                                      {"WSD_PAST", 8},
                                                                      {"WSD_LOST", 3},
                                                                      {"WSD_PLAIN", 4},
                                                                      {"WSD_NO_NOTE", 5},
                                                                      {"WSD_NO_WAVE", 6},
                                                                      {"WSD_BACKWARDS", 7}}};
    for (const auto& [name, index] : kSounds)
    {
        sar_files::SoundSpec sound;
        sound.name = name;
        sound.kind = sar_files::SoundSpec::kWaveSound;
        sound.file = 6;
        sound.wave_sound_index = index;
        spec.sounds.push_back(sound);
    }
    const std::vector<std::vector<uint8_t>> waves = TestWaveFiles(big_endian);
    spec.wave_archives.push_back({"WARC_SECOND", 7});
    spec.files.push_back({sar_files::WaveSoundFile(big_endian, file), "", false});
    spec.files.push_back({sar_files::WaveArchive(big_endian, {waves[1], waves[0]}), "", false});

    return spec;
}

// The ramp of `count` samples of a Wii wave that WiiRampWave makes: 200 steps of 100 from -10,000, again and again.
inline std::vector<int16_t> WiiRamp(uint32_t count)
{
    std::vector<int16_t> samples(count);
    for (uint32_t i = 0; i < count; i++)
    {
        samples[i] = static_cast<int16_t>(static_cast<int>(i % 200) * 100 - 10000);
    }

    return samples;
}

// An RWAV of WiiRamp's `count` samples, mono 16-bit PCM at 32 kHz. A wave that loops does so from its start.
inline std::vector<uint8_t> WiiRampWave(uint32_t count, bool loop)
{
    return ntdlib::testing::rsar_files::MakePcm16Rwav(count, WiiRamp(count), 1, loop);
}

// The Wii's archives of wave sounds that TestWiiWaveSoundFiles and TestWiiWaveSoundPreview read: the archive of version
// 1.3 with its RWAR, and the archive of version 1.2 with the samples of its WAVE block.
struct WiiWaveSoundArchives
{
    std::vector<uint8_t> rwar;
    std::shared_ptr<const ntdlib::rsar::SoundArchive> archive;
    std::shared_ptr<const ntdlib::rsar::SoundArchive> old_archive;
};

// The archives of WiiWaveSoundArchives.
inline WiiWaveSoundArchives MakeWiiWaveSoundArchives()
{
    namespace rsar_files = ntdlib::testing::rsar_files;

    // WSD_0 plays wave 0 at a pitch of 1.5, away from the centre, through a note whose key, pan and pitch the player
    // leaves unread. WSD_1 plays the looped wave 1. WSD_2's note plays wave 5, and the file lacks that wave. WSD_3 has
    // no note.
    rsar_files::WaveSoundNote unread;
    unread.original_key = 62;
    unread.pan = 70;
    unread.pitch = 0.5f;
    rsar_files::WaveSoundSpec high;
    high.pitch = 1.5f;
    high.pan = 32;
    high.notes = {unread};
    rsar_files::WaveSoundSpec looped;
    looped.notes = {rsar_files::WaveSoundNote{.wave = 1}};
    rsar_files::WaveSoundSpec lost;
    lost.notes = {rsar_files::WaveSoundNote{.wave = 5}};
    const std::vector<rsar_files::WaveSoundSpec> specs = {high, looped, lost, rsar_files::WaveSoundSpec{}};
    std::vector<rsar_files::ArchiveWaveSound> entries(specs.size());
    for (std::size_t i = 0; i < entries.size(); i++)
    {
        entries[i].index = static_cast<int32_t>(i);
    }
    const std::vector<uint8_t> rwar = rsar_files::MakeRwarOf({WiiRampWave(9600, false), WiiRampWave(960, true)});

    // The samples of the file of version 1.2: two waves of 0x40 bytes of DSP-ADPCM. Each starts with its predictor and
    // scale. Each later frame of 8 bytes starts with predictor 0 and scale 11, and its nibbles hold 7 and -7 in turn.
    std::vector<uint8_t> old_waves = rsar_files::MakeOldWaveData();
    for (std::size_t i = 1; i < old_waves.size(); i++)
    {
        if (i % 0x40 != 0)
        {
            old_waves[i] = i % 8 == 0 ? 0x0B : 0x79;
        }
    }
    const std::shared_ptr<const ntdlib::rsar::SoundArchive> archive =
        LoadWiiArchive(rsar_files::MakeWaveSoundArchive(rsar_files::MakeWaveSoundFile(0x0103, specs), rwar, entries),
                       "SOUND.brsar", ntdlib::rvl::snd::kRsarOptions);
    const std::shared_ptr<const ntdlib::rsar::SoundArchive> old_archive = LoadWiiArchive(
        rsar_files::MakeWaveSoundArchive(rsar_files::MakeWaveSoundFile(0x0102, specs), old_waves, entries), "OLD.brsar",
        ntdlib::rvl::snd::kRsarOptions);

    return {rwar, archive, old_archive};
}

} // namespace
