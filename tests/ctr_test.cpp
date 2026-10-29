// SPDX-License-Identifier: MIT

// Tests the 3DS's playback (song/ctr_model.h), in both mixing modes. A small archive holds a sequence with one note on
// a constant wave. ntdlib's archive mode of the 3DS, run directly, gives the reference: the DSP's output after three
// frames of silence, or in enhanced mixing the enhanced mode's frames after four frames of silence. The player's first
// frame fades in. After that frame, the player's output has to be the reference, sample for sample, and the reference
// has to stay silent after the output's end. Seeks that restore snapshots have to give the reference's frames. The note
// has to start within 0.4 ms of the same time in both modes. A switch of the mode during playback keeps the player's
// rate: after the frame that crossfades, the player's frames have to be the new mode's reference through a
// FrameResampler. A muted track plays silence for as long as the track plays. The meter of the note's track has to
// reach the output's peaks, and the other tracks' meters stay at 0. A note of the bank sounds, and its preview ends
// once the note has faded.
//
// The effects of a file of the 3DS play as archive mode does with ntdlib's models of the libraries' effects in
// nn::snd's aux callbacks, run directly. Bus A holds a reverb, and bus B a delay. The buses also hold effects of the
// Switch and the Wii U. The 3DS's model leaves them out. The track sends the note to both buses. nw::snd's reverb plays
// in both modes, and nn::snd's two reverbs in strict mixing. The player's output, the frames after seeks and the output
// after a switch to a model with the effects have to be the reference's, sample for sample. The effects' tails keep an
// offset of a step or two, and the output never comes out silent. It has to end a second or more after the reference
// was last louder than ±8, and the reference has to stay within ±8 after it. A bus volume above 1 and one below 1 have
// to play as the reference gives them, and the buses' meters have to reach the reference's peaks.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <memory>
#include <span>
#include <utility>
#include <vector>

#include "ntdlib/cafe/snd/effect_setup.h"
#include "ntdlib/ctr/nnsnd/sound_system.h"
#include "ntdlib/ctr/snd/engine.h"
#include "ntdlib/ctr/snd/sar_options.h"
#include "ntdlib/ctr_fx/effects.h"
#include "ntdlib/nx/audio/i3dl2.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/testing/sar_files.h"
#include "xsflib/testing/check.h"

#include "edit/effects.h"
#include "format/file.h"
#include "song/archive.h"
#include "song/ctr_model.h"
#include "song/frame_resampler.h"
#include "song/player.h"
#include "song/preview.h"
#include "song/sequence.h"
#include "song/sequence_model.h"

namespace
{

namespace sar = ntdlib::sar;
namespace sar_files = ntdlib::testing::sar_files;
using hokuto::format::EffectKind;
using hokuto::song::CtrModel;
using hokuto::song::FrameResampler;
using hokuto::song::Mixing;
using hokuto::song::NotePreview;
using hokuto::song::PlaybackClock;
using hokuto::song::Sequence;
using hokuto::song::SequenceModel;
using hokuto::song::SequencePlayer;

// The floats of a frame: 160 stereo samples, or 235 in enhanced mixing.
constexpr std::size_t kFrameFloats = CtrModel::kFrameSamples * 2;
constexpr std::size_t kEnhancedFrameFloats = CtrModel::kEnhancedFrameSamples * 2;

// The scale that the player takes for a 16-bit sample in floating point.
constexpr float kSampleScale = 1.0f / 32768.0f;

// The frames that the reference runs: 3 s, past the end of the note's wave at 2 s. With the effects, the reference runs
// 7 s, past the end of their tails.
constexpr std::size_t kFrames = 615;
constexpr std::size_t kEffectFrames = 1435;

// A second of frames, rounded up. The frames of both modes last as long.
constexpr std::size_t kSecondFrames = 205;

// The archive's bytes: sequence sound 0 plays key 60 at velocity 127 for 96 ticks on program 0, waits 96 ticks and
// ends. Program 0 plays a constant wave of 2 s. With `sends`, the track first sends to buses A and B at full level.
std::vector<uint8_t> ArchiveBytes(bool sends = false)
{
    constexpr sar::Flavor kCtr = sar::Flavor::kCtr;
    std::vector<uint8_t> commands = {0x3C, 0x7F, 0x60, 0x80, 0x60, 0xFF};
    if (sends)
    {
        commands.insert(commands.begin(), {0xD9, 0x7F, 0xDA, 0x7F});
    }
    sar_files::ArchiveSpec spec;
    spec.sounds = {{.name = "SEQ_NOTE", .file = 0, .banks = {0}}};
    spec.banks = {{"BANK", 1}};
    spec.wave_archives = {{"WARC", 2}};
    spec.players = {{"PLAYER", 1}};
    spec.files = {{sar_files::Sequence(kCtr, commands, {{"SEQ_NOTE", 0}})},
                  {sar_files::Bank(kCtr, sar_files::OneRegionBank())},
                  {sar_files::WaveArchive(kCtr, {sar_files::Wave(kCtr, sar_files::ConstantWave(false, 64000))})}};

    return sar_files::Archive(kCtr, spec);
}

// The reference: the DSP's output after the silence, in Hokuto's frames, and the tick count after each frame. In
// enhanced mixing, `floats` holds the enhanced mode's frames after the silence, and `samples` the same rounded to 16
// bits. In strict mixing, `floats` holds `samples` at the player's scale. With effects, `bus_peaks` holds the largest
// peaks of each bus's front channels after its effect, at the bus's volume up to 1.
struct Reference
{
    std::vector<int16_t> samples;
    std::vector<float> floats;
    std::vector<uint64_t> after;
    std::array<std::array<int32_t, 2>, 2> bus_peaks{};
};

// The effects of a reference: a reverb of `library` on bus A and a delay on bus B, and the buses' volumes. A volume up
// to 1 is the bus's return volume. Above 1, the bus returns at full volume, and its samples take the volume after the
// effect.
struct ReferenceEffects
{
    ntdlib::ctr_fx::Library library = ntdlib::ctr_fx::Library::kNwSnd;
    ntdlib::ctr_fx::ReverbParam reverb;
    ntdlib::ctr_fx::DelayParam delay;
    std::array<float, 2> volumes = {1.0f, 1.0f};
};

// A bus's sample at `volume`, rounded to the nearest integer, halves away from zero, and held to 32 bits.
int32_t AtVolume(double sample, double volume)
{
    const double scaled = std::round(sample * volume);
    return static_cast<int32_t>(std::clamp(scaled, static_cast<double>(std::numeric_limits<int32_t>::min()),
                                           static_cast<double>(std::numeric_limits<int32_t>::max())));
}

// The end of a reference's callback, after the effect: a volume above 1, then the largest peaks so far of the front
// channels, at the volume up to 1.
void FinishBus(const std::array<int32_t*, 4>& channels, std::size_t samples, float volume,
               std::array<int32_t, 2>& peaks)
{
    if (volume > 1.0f)
    {
        for (int32_t* channel : channels)
        {
            for (std::size_t i = 0; i < samples; i++)
            {
                channel[i] = AtVolume(channel[i], volume);
            }
        }
    }

    for (std::size_t c = 0; c < peaks.size(); c++)
    {
        int64_t peak = 0;
        for (std::size_t i = 0; i < samples; i++)
        {
            peak = std::max(peak, std::abs(int64_t{channels[c][i]}));
        }
        peaks[c] = std::max(peaks[c], AtVolume(static_cast<double>(peak), std::min(volume, 1.0f)));
    }
}

// Puts the effects in nn::snd's aux callbacks of buses A and B, run directly on ntdlib's models of the libraries'
// effects, in frames of the mode. The enhanced mode's frames of 235 samples keep the strict frames' times, at 235 / 160
// of the strict rate.
void AddEffects(ntdlib::ctr::nnsnd::SoundSystem& snd, Mixing mixing, const ReferenceEffects& effects,
                std::array<std::array<int32_t, 2>, 2>& peaks)
{
    const ntdlib::ctr_fx::FrameFormat format =
        mixing == Mixing::kEnhanced
            ? ntdlib::ctr_fx::FrameFormat{CtrModel::kEnhancedFrameSamples, 32728.0f * 235.0f / 160.0f}
            : ntdlib::ctr_fx::FrameFormat{};

    auto bus_a = [reverb = ntdlib::ctr_fx::Reverb(effects.library, effects.reverb, format), volume = effects.volumes[0],
                  bus = &peaks[0]](const std::array<int32_t*, 4>& channels, std::size_t samples) mutable
    {
        reverb.Process(channels, samples);
        FinishBus(channels, samples, volume, *bus);
    };
    snd.SetAuxCallback(0, std::move(bus_a));

    auto bus_b = [delay = ntdlib::ctr_fx::Delay(effects.delay, format), volume = effects.volumes[1], bus = &peaks[1]](
                     const std::array<int32_t*, 4>& channels, std::size_t samples) mutable
    {
        delay.Process(channels, samples);
        FinishBus(channels, samples, volume, *bus);
    };
    snd.SetAuxCallback(1, std::move(bus_b));
}

// Runs archive mode for kFrames frames, or for kEffectFrames with `effects` on the buses. As in Hokuto, the buses'
// volumes below 1 reach nn::snd after the sound starts.
Reference RunReference(const sar::SoundArchive& archive, Mixing mixing = Mixing::kStrict,
                       const ReferenceEffects* effects = nullptr)
{
    Reference reference;
    const bool enhanced = mixing == Mixing::kEnhanced;
    const std::size_t frame_floats = enhanced ? kEnhancedFrameFloats : kFrameFloats;
    std::vector<int16_t> samples(CtrModel::kSilence * 2, 0);
    std::vector<float> floats(CtrModel::kEnhancedSilence * 2, 0.0f);
    ntdlib::ctr::snd::ArchiveModel model(enhanced ? ntdlib::ctr::snd::DspKind::kEnhancedModel
                                                  : ntdlib::ctr::snd::DspKind::kModel);

    // The DSP gives its output to `samples` and `floats`.
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

    const bool started = model.Start(archive);
    SEQLIB_CHECK(started);
    if (started && effects != nullptr)
    {
        AddEffects(model.snd, mixing, *effects, reference.bus_peaks);
    }

    const ntdlib::ctr::snd::SequenceSoundPlayer* player = started ? model.engine->StartSequence(0) : nullptr;
    SEQLIB_CHECK(player != nullptr);
    for (std::size_t bus = 0; effects != nullptr && bus < effects->volumes.size(); bus++)
    {
        if (effects->volumes[bus] < 1.0f)
        {
            model.snd.SetAuxReturnVolume(static_cast<int>(bus), effects->volumes[bus]);
        }
    }

    const std::size_t frames = effects != nullptr ? kEffectFrames : kFrames;
    for (std::size_t frame = 0; player != nullptr && frame < frames; frame++)
    {
        SEQLIB_CHECK(model.engine->RunFrame());
        reference.after.push_back(player->TickCount());
    }

    if (enhanced)
    {
        floats.resize(reference.after.size() * frame_floats);
        reference.floats = std::move(floats);
        for (const float sample : reference.floats)
        {
            const double scaled = std::round(static_cast<double>(sample) * 32768.0);
            reference.samples.push_back(static_cast<int16_t>(std::clamp(scaled, -32768.0, 32767.0)));
        }

        return reference;
    }

    samples.resize(reference.after.size() * frame_floats);
    reference.samples = std::move(samples);
    for (const int16_t sample : reference.samples)
    {
        reference.floats.push_back(static_cast<float>(sample) * kSampleScale);
    }

    return reference;
}

// The floats of a frame in `mixing`.
std::size_t FrameFloats(Mixing mixing)
{
    return mixing == Mixing::kEnhanced ? kEnhancedFrameFloats : kFrameFloats;
}

// Whether any of `samples` isn't 0.
bool Sounds(std::span<const int16_t> samples)
{
    return std::any_of(samples.begin(), samples.end(), [](int16_t sample) { return sample != 0; });
}

// The player plays the reference's frames from the frame that runs the first tick, and ends where the reference falls
// silent for good. Archive mode runs the first tick in its first frame (song::PerformOptionsOf). The model's frames and
// rate follow the mode.
void TestPlayer(const std::shared_ptr<const sar::SoundArchive>& archive, const Reference& reference, Mixing mixing)
{
    std::size_t first = 0;
    while (first < reference.after.size() && reference.after[first] == 0)
    {
        first++;
    }
    SEQLIB_CHECK_EQ(first, 0);

    const auto sequence = std::make_shared<const Sequence>(hokuto::song::ReadSequence(*archive, 0));
    const auto model = std::make_shared<SequenceModel>(archive, sequence, hokuto::song::AuxEffects{}, mixing);
    const bool enhanced = mixing == Mixing::kEnhanced;
    SEQLIB_CHECK_EQ(model->FrameSamples(), enhanced ? CtrModel::kEnhancedFrameSamples : CtrModel::kFrameSamples);
    SEQLIB_CHECK_EQ(model->SampleRate(), enhanced ? CtrModel::kEnhancedSampleRate : CtrModel::kSampleRate);
    const std::size_t frame_floats = FrameFloats(mixing);
    const auto clock = std::make_shared<PlaybackClock>(model->FrameSamples());
    SequencePlayer player(model, 0, clock);
    SEQLIB_CHECK_EQ(player.SampleRate(), model->SampleRate());
    std::vector<float> output(kFrames * frame_floats);

    const std::size_t rendered = player.Render(output.data(), output.size() / 2);
    const std::size_t heard = rendered / (frame_floats / 2);

    SEQLIB_REQUIRE(heard > 410 && heard < kFrames - first);
    std::size_t wrong = 0;
    for (std::size_t i = frame_floats; i < (heard - 1) * frame_floats; i++)
    {
        wrong += output[i] != reference.floats[first * frame_floats + i] ? 1 : 0;
    }
    SEQLIB_CHECK_EQ(wrong, 0);
    SEQLIB_CHECK(Sounds(std::span(reference.samples).first((first + heard) * frame_floats)));
    SEQLIB_CHECK(!Sounds(std::span(reference.samples).subspan((first + heard) * frame_floats)));
}

// Seeks back and forth restore the snapshots that a run took, every 50 frames here, and give the reference's frames.
void TestSeeks(const std::shared_ptr<const sar::SoundArchive>& archive, const Reference& reference, Mixing mixing)
{
    const auto sequence = std::make_shared<const Sequence>(hokuto::song::ReadSequence(*archive, 0));
    SequenceModel model(archive, sequence, {}, mixing, 50);
    const std::size_t frame_floats = FrameFloats(mixing);
    std::size_t wrong = 0;
    for (const uint64_t frame : {300u, 120u, 0u, 377u, 51u})
    {
        SEQLIB_CHECK(model.SeekFrame(frame));
        const std::span<const float> output = model.FloatOutput();
        const std::span<const float> wanted = std::span(reference.floats).subspan(frame * frame_floats, frame_floats);
        wrong += std::equal(output.begin(), output.end(), wanted.begin(), wanted.end()) ? 0 : 1;
        const std::span<const int16_t> rounded = model.Output();
        const std::span<const int16_t> samples =
            std::span(reference.samples).subspan(frame * frame_floats, frame_floats);
        wrong += std::equal(rounded.begin(), rounded.end(), samples.begin(), samples.end()) ? 0 : 1;
    }
    SEQLIB_CHECK_EQ(wrong, 0);
}

// The time of the first sample that sounds, in seconds, at `rate` samples a second.
double FirstSound(std::span<const int16_t> samples, double rate)
{
    const auto sounds = std::find_if(samples.begin(), samples.end(), [](int16_t sample) { return sample != 0; });
    return static_cast<double>((sounds - samples.begin()) / 2) / rate;
}

// The note starts at the same time in both modes, within 0.4 ms. The modes' rates are the DSP's, and 235 / 160 of it.
void TestAlignment(const Reference& strict, const Reference& enhanced)
{
    constexpr double kStrictRate = 268111856.0 / 8192.0;
    constexpr double kEnhancedRate = kStrictRate * CtrModel::kEnhancedFrameSamples / CtrModel::kFrameSamples;
    const double strict_start = FirstSound(strict.samples, kStrictRate);
    const double enhanced_start = FirstSound(enhanced.samples, kEnhancedRate);
    SEQLIB_CHECK(strict_start > 0.0 && std::fabs(strict_start - enhanced_start) < 0.0004);
}

// A switch of the mode at frame 100 of the player, as after a click on the mixer's switch. The player starts in mode
// `from` and keeps its rate. The new model's frames from frame 100 on go through a new FrameResampler. After the frame
// that crossfades, each of the player's frames is the resampler's frame of the new mode's reference. The output then
// ends where the reference of the new mode falls silent for good.
void TestSwitch(const std::shared_ptr<const sar::SoundArchive>& archive, const Reference& old_reference,
                const Reference& new_reference, Mixing from)
{
    constexpr std::size_t kBefore = 100;
    const Mixing to = from == Mixing::kStrict ? Mixing::kEnhanced : Mixing::kStrict;
    const auto sequence = std::make_shared<const Sequence>(hokuto::song::ReadSequence(*archive, 0));
    const auto model = std::make_shared<SequenceModel>(archive, sequence, hokuto::song::AuxEffects{}, from);
    const auto clock = std::make_shared<PlaybackClock>(model->FrameSamples());
    SequencePlayer player(model, 0, clock);
    const std::size_t frame_floats = FrameFloats(from);
    std::vector<float> output(kFrames * frame_floats);

    // The player switches to a model in the other mode after kBefore frames.
    std::size_t rendered = player.Render(output.data(), kBefore * frame_floats / 2);
    SEQLIB_REQUIRE(rendered == kBefore * frame_floats / 2);
    clock->Offer(std::make_shared<SequenceModel>(archive, sequence, hokuto::song::AuxEffects{}, to));
    rendered += player.Render(output.data() + rendered * 2, output.size() / 2 - rendered);

    SEQLIB_CHECK_EQ(player.SampleRate(), model->SampleRate());

    // The frames of the old mode up to the switch.
    std::size_t wrong = 0;
    for (std::size_t i = frame_floats; i < kBefore * frame_floats; i++)
    {
        wrong += output[i] != old_reference.floats[i] ? 1 : 0;
    }

    // The new mode's frames through the resampler, from the frame that crossfades on.
    const std::size_t new_floats = FrameFloats(to);
    FrameResampler resampler(new_floats / 2, frame_floats / 2);
    std::vector<float> resampled(frame_floats);
    const std::size_t heard = rendered / (frame_floats / 2);
    for (std::size_t frame = kBefore; frame < heard && (frame + 1) * new_floats <= new_reference.floats.size(); frame++)
    {
        resampler.Process(std::span(new_reference.floats).subspan(frame * new_floats, new_floats), resampled);
        if (frame > kBefore && frame + 1 < heard)
        {
            wrong += std::equal(resampled.begin(), resampled.end(), output.begin() + frame * frame_floats) ? 0 : 1;
        }
    }
    SEQLIB_CHECK_EQ(wrong, 0);
    SEQLIB_REQUIRE(heard > kBefore + 1 && heard < kFrames);
    SEQLIB_CHECK(Sounds(std::span(new_reference.samples).first(heard * new_floats)));
    SEQLIB_CHECK(!Sounds(std::span(new_reference.samples).subspan(heard * new_floats)));
}

// A muted track plays silence. The model runs as many frames as without the mute.
void TestMute(const std::shared_ptr<const sar::SoundArchive>& archive, Mixing mixing)
{
    const auto sequence = std::make_shared<const Sequence>(hokuto::song::ReadSequence(*archive, 0));
    std::array<std::size_t, 2> frames{};
    bool muted_sounds = false;
    for (const bool mute : {false, true})
    {
        SequenceModel model(archive, sequence, {}, mixing);
        model.SetMutes(mute ? 1u : 0u);
        for (bool more = model.SeekFrame(0); more; more = model.RunFrame())
        {
            frames[mute ? 1 : 0]++;
            muted_sounds = muted_sounds || (mute && Sounds(model.Output()));
        }
    }
    SEQLIB_CHECK(!muted_sounds && frames[0] > 410 && frames[1] == frames[0]);
}

// Track 0 plays the note. The meter of track 0 reaches the largest magnitude of each side of the DSP's main bus: the
// DSP mixes the note's track alone into the bus. In strict mixing, the output takes the bus through nn::snd's limiter.
// The limiter bends even a quiet sample a little. In enhanced mixing, the output takes the bus at the master volume,
// just under 1. The meter's peak therefore lies at or above the output's, within 1 %. The other tracks' meters stay at
// 0.
void TestMeters(const std::shared_ptr<const sar::SoundArchive>& archive, Mixing mixing)
{
    const auto sequence = std::make_shared<const Sequence>(hokuto::song::ReadSequence(*archive, 0));
    SequenceModel model(archive, sequence, {}, mixing);
    std::array<int32_t, 2> metered{};
    std::array<int32_t, 2> heard{};
    bool others = false;

    for (bool more = model.SeekFrame(0); more; more = model.RunFrame())
    {
        const std::span<const int16_t> output = model.Output();
        for (std::size_t i = 0; i < output.size(); i++)
        {
            heard[i % 2] = std::max(heard[i % 2], std::abs(static_cast<int32_t>(output[i])));
        }

        for (int track = 0; track < SequenceModel::kTracks; track++)
        {
            const std::array<int32_t, 2> peaks = model.TrackPeaks(track);
            if (track == 0)
            {
                metered = {std::max(metered[0], peaks[0]), std::max(metered[1], peaks[1])};
            }
            others = others || (track != 0 && (peaks[0] != 0 || peaks[1] != 0));
        }
    }

    for (std::size_t c = 0; c < 2; c++)
    {
        SEQLIB_CHECK(heard[c] > 0 && metered[c] >= heard[c] && metered[c] - heard[c] <= metered[c] / 100);
    }
    SEQLIB_CHECK(!others);
}

// A note of the bank sounds, and the preview ends once the note has faded, before its limit of 10 s. The preview's
// frames and rate follow the mode.
void TestNote(const std::shared_ptr<const sar::SoundArchive>& archive, Mixing mixing)
{
    NotePreview note(archive, 0, 0, mixing);
    const std::size_t frame_samples = FrameFloats(mixing) / 2;
    SEQLIB_CHECK_EQ(note.SampleRate(),
                    mixing == Mixing::kEnhanced ? CtrModel::kEnhancedSampleRate : CtrModel::kSampleRate);
    std::vector<float> output(note.LongestFrames() * frame_samples * 2);
    const std::size_t rendered = note.Render(output.data(), output.size() / 2);
    SEQLIB_CHECK(rendered > note.HoldFrames() * frame_samples);
    SEQLIB_CHECK(rendered < output.size() / 2);
    SEQLIB_CHECK(rendered % frame_samples == 0);
    SEQLIB_CHECK(std::any_of(output.begin(), output.end(), [](float sample) { return sample != 0.0f; }));
}

// The test's reverb: values that differ from the libraries' defaults, with high-pass damping where `kind` has it.
// nw::snd's reverb and nn::snd's later one compute the same. For nn::snd's later reverb, the early reflection of 0 ms
// takes the one frame that each buffer holds at least.
ntdlib::ctr_fx::ReverbParam TestReverb(EffectKind kind)
{
    ntdlib::ctr_fx::ReverbParam param;
    param.early_reflection_time = kind == EffectKind::kCtrNnReverb ? 0 : 30;
    param.fused_time = 1000;
    param.pre_delay_time = 50;
    param.coloration = 0.6f;
    param.damping = 0.3f;
    param.filter_sizes = {1600, 2400, 960};
    param.early_gain = 0.7f;
    param.fused_gain = 0.5f;
    param.high_pass_damping = kind == EffectKind::kCtrNnReverb;

    return param;
}

// The test's delay, with values that differ from the defaults too.
ntdlib::ctr_fx::DelayParam TestDelay()
{
    ntdlib::ctr_fx::DelayParam param;
    param.delay_time = 120;
    param.feedback = 0.3f;
    param.damping = 0.2f;

    return param;
}

// The effects that edit::ModelEffects gives the model for a file, in a file's order of their parameters: the test's
// reverb of `kind` on bus A, nn::snd's delay on bus B, and nw::snd's delay on bus C. The 3DS lacks bus C. A file holds
// the effects of every engine that its sounds play on. Bus A therefore also holds the Switch's I3DL2 reverb before the
// 3DS's reverb, and bus B the Wii U's delay after nn::snd's delay. The 3DS's model leaves out those two and the delay
// on bus C.
hokuto::song::AuxEffects FileEffects(EffectKind kind)
{
    const ntdlib::ctr_fx::ReverbParam reverb = TestReverb(kind);
    std::vector<float> parameters = {static_cast<float>(reverb.early_reflection_time),
                                     static_cast<float>(reverb.fused_time),
                                     static_cast<float>(reverb.pre_delay_time),
                                     reverb.coloration,
                                     reverb.damping,
                                     static_cast<float>(reverb.filter_sizes[0]),
                                     static_cast<float>(reverb.filter_sizes[1]),
                                     static_cast<float>(reverb.filter_sizes[2]),
                                     reverb.early_gain,
                                     reverb.fused_gain};
    if (kind == EffectKind::kCtrReverb)
    {
        parameters.push_back(0.0f);
    }
    if (kind != EffectKind::kCtrNnReverb2011)
    {
        parameters.push_back(reverb.high_pass_damping ? 1.0f : 0.0f);
    }
    const ntdlib::ctr_fx::DelayParam delay = TestDelay();
    const std::array<float, hokuto::song::kI3dl2Parameters> i3dl2 =
        hokuto::song::I3dl2Parameters(ntdlib::nx::audio::kI3dl2ReverbPresets[1]);
    hokuto::edit::BusEffects effects;
    effects[0] = {{EffectKind::kI3dl2Reverb, {i3dl2.begin(), i3dl2.end()}}, {kind, parameters}};
    effects[1] = {
        {EffectKind::kCtrNnDelay, {static_cast<float>(delay.delay_time), delay.feedback, delay.damping, 0.0f}},
        {EffectKind::kDelay, hokuto::song::WiiUStartingParameters(ntdlib::cafe::snd::EffectType::kDelay)}};
    effects[2] = {{EffectKind::kCtrDelay, {300.0f, 0.5f, 0.5f, 0.0f}}};

    // Each effect's parameters suit its kind, and the model takes every effect.
    hokuto::song::AuxEffects model = hokuto::edit::ModelEffects(effects);
    SEQLIB_CHECK(model[0].size() == 2 && model[1].size() == 2 && model[2].size() == 1);

    return model;
}

// Whether all of `samples` lie within ±8.
bool Quiet(std::span<const int16_t> samples)
{
    return std::all_of(samples.begin(), samples.end(), [](int16_t sample) { return std::abs(sample) <= 8; });
}

// Whether output of `heard` frames ends as it should after `reference` with effects: a second or more after the
// reference's last frame with a sample beyond ±8, and before the reference's end. The reference stays within ±8 after
// the output's end. The effects' tails keep an offset of a step or two once their sound has gone. The reference's last
// frame therefore isn't silent.
bool EndsAfterTails(std::size_t heard, const Reference& reference, std::size_t frame_floats)
{
    const std::span<const int16_t> samples(reference.samples);
    std::size_t last_loud = 0;
    for (std::size_t frame = 0; (frame + 1) * frame_floats <= samples.size(); frame++)
    {
        last_loud = Quiet(samples.subspan(frame * frame_floats, frame_floats)) ? last_loud : frame;
    }

    return heard > last_loud + kSecondFrames && heard < kEffectFrames && Quiet(samples.subspan(heard * frame_floats)) &&
           Sounds(samples.last(frame_floats));
}

// The test's reverb of `kind` on bus A and its delay on bus B play as the reference gives them, with the reverb of
// `library`: the player's output after its first frame, and the frames after seeks back and forth. The output ends once
// the tails have stayed within ±8 for long enough (EndsAfterTails).
void TestEffects(const std::shared_ptr<const sar::SoundArchive>& archive, const sar::SoundArchive& strict,
                 EffectKind kind, ntdlib::ctr_fx::Library library, Mixing mixing)
{
    const ReferenceEffects effects{library, TestReverb(kind), TestDelay()};
    const Reference reference = RunReference(strict, mixing, &effects);
    SEQLIB_REQUIRE(reference.after.size() == kEffectFrames);

    const auto sequence = std::make_shared<const Sequence>(hokuto::song::ReadSequence(*archive, 0));
    const auto model = std::make_shared<SequenceModel>(archive, sequence, FileEffects(kind), mixing);
    const auto clock = std::make_shared<PlaybackClock>(model->FrameSamples());
    SequencePlayer player(model, 0, clock);
    const std::size_t frame_floats = FrameFloats(mixing);
    std::vector<float> output(kEffectFrames * frame_floats);

    const std::size_t heard = player.Render(output.data(), output.size() / 2) / (frame_floats / 2);

    SEQLIB_REQUIRE(EndsAfterTails(heard, reference, frame_floats));
    std::size_t wrong = 0;
    for (std::size_t i = frame_floats; i < (heard - 1) * frame_floats; i++)
    {
        wrong += output[i] != reference.floats[i] ? 1 : 0;
    }
    SEQLIB_CHECK_EQ(wrong, 0);

    // Seeks restore the snapshots with the effects' state.
    SequenceModel seeking(archive, sequence, FileEffects(kind), mixing, 50);
    std::size_t seeks = 0;
    for (const uint64_t frame : {600u, 120u, 0u, 377u, 51u})
    {
        SEQLIB_CHECK(seeking.SeekFrame(frame));
        const std::span<const float> floats = seeking.FloatOutput();
        const std::span<const float> wanted = std::span(reference.floats).subspan(frame * frame_floats, frame_floats);
        seeks += std::equal(floats.begin(), floats.end(), wanted.begin(), wanted.end()) ? 0 : 1;
    }
    SEQLIB_CHECK_EQ(seeks, 0);
}

// Bus A at 1.5 and bus B at 0.5 play as the reference gives them. The buses' meters reach the reference's largest
// peaks, and bus C's stays at 0.
void TestEffectVolumes(const std::shared_ptr<const sar::SoundArchive>& archive, const sar::SoundArchive& strict,
                       Mixing mixing)
{
    ReferenceEffects effects{ntdlib::ctr_fx::Library::kNwSnd, TestReverb(EffectKind::kCtrReverb), TestDelay()};
    effects.volumes = {1.5f, 0.5f};
    const Reference reference = RunReference(strict, mixing, &effects);
    SEQLIB_REQUIRE(reference.after.size() == kEffectFrames);

    const auto sequence = std::make_shared<const Sequence>(hokuto::song::ReadSequence(*archive, 0));
    SequenceModel model(archive, sequence, FileEffects(EffectKind::kCtrReverb), mixing);
    model.SetBusVolumes({1.5f, 0.5f, 1.0f});
    const std::size_t frame_floats = FrameFloats(mixing);
    std::array<std::array<int32_t, 2>, 3> peaks{};
    std::size_t frames = 0;
    std::size_t wrong = 0;

    for (bool more = model.SeekFrame(0); more && frames < kEffectFrames; more = model.RunFrame())
    {
        const std::span<const float> floats = model.FloatOutput();
        const std::span<const float> wanted = std::span(reference.floats).subspan(frames * frame_floats, frame_floats);
        wrong += std::equal(floats.begin(), floats.end(), wanted.begin(), wanted.end()) ? 0 : 1;
        for (std::size_t bus = 0; bus < peaks.size(); bus++)
        {
            const std::array<int32_t, 2> bus_peaks = model.BusPeaks(static_cast<int>(bus));
            peaks[bus] = {std::max(peaks[bus][0], bus_peaks[0]), std::max(peaks[bus][1], bus_peaks[1])};
        }
        frames++;
    }

    SEQLIB_CHECK_EQ(wrong, 0);
    SEQLIB_CHECK(EndsAfterTails(frames, reference, frame_floats));
    SEQLIB_CHECK(peaks[0] == reference.bus_peaks[0] && peaks[1] == reference.bus_peaks[1]);
    SEQLIB_CHECK(peaks[0][0] > 0 && peaks[1][0] > 0 && peaks[2][0] == 0 && peaks[2][1] == 0);
}

// A switch at frame 100, from a model without effects to one with them, as after a new effect during playback. Up to
// the switch, the output is the reference's without effects. After the frame that crossfades, it's the reference's with
// them, and it ends once their tails have stayed quiet (EndsAfterTails).
void TestEffectSwitch(const std::shared_ptr<const sar::SoundArchive>& archive, const Reference& dry,
                      const Reference& wet, Mixing mixing)
{
    constexpr std::size_t kBefore = 100;
    const auto sequence = std::make_shared<const Sequence>(hokuto::song::ReadSequence(*archive, 0));
    const auto model = std::make_shared<SequenceModel>(archive, sequence, hokuto::song::AuxEffects{}, mixing);
    const auto clock = std::make_shared<PlaybackClock>(model->FrameSamples());
    SequencePlayer player(model, 0, clock);
    const std::size_t frame_floats = FrameFloats(mixing);
    std::vector<float> output(kEffectFrames * frame_floats);

    std::size_t rendered = player.Render(output.data(), kBefore * frame_floats / 2);
    SEQLIB_REQUIRE(rendered == kBefore * frame_floats / 2);
    clock->Offer(std::make_shared<SequenceModel>(archive, sequence, FileEffects(EffectKind::kCtrReverb), mixing));
    rendered += player.Render(output.data() + rendered * 2, output.size() / 2 - rendered);
    const std::size_t heard = rendered / (frame_floats / 2);

    SEQLIB_REQUIRE(EndsAfterTails(heard, wet, frame_floats));
    std::size_t wrong = 0;
    for (std::size_t i = frame_floats; i < kBefore * frame_floats; i++)
    {
        wrong += output[i] != dry.floats[i] ? 1 : 0;
    }
    for (std::size_t i = (kBefore + 1) * frame_floats; i < (heard - 1) * frame_floats; i++)
    {
        wrong += output[i] != wet.floats[i] ? 1 : 0;
    }
    SEQLIB_CHECK_EQ(wrong, 0);
}

} // namespace

int main()
{
    const std::shared_ptr<const sar::SoundArchive> archive = hokuto::song::LoadArchive(ArchiveBytes(), "test");
    const sar::SoundArchive strict = sar::SoundArchive::Load(ArchiveBytes(), ntdlib::ctr::kSarOptions);
    const Reference reference = RunReference(strict);
    const Reference enhanced = RunReference(strict, Mixing::kEnhanced);
    if (reference.after.size() == kFrames && enhanced.after.size() == kFrames)
    {
        TestPlayer(archive, reference, Mixing::kStrict);
        TestPlayer(archive, enhanced, Mixing::kEnhanced);
        TestSeeks(archive, reference, Mixing::kStrict);
        TestSeeks(archive, enhanced, Mixing::kEnhanced);
        TestAlignment(reference, enhanced);
        TestSwitch(archive, reference, enhanced, Mixing::kStrict);
        TestSwitch(archive, enhanced, reference, Mixing::kEnhanced);
    }
    for (const Mixing mixing : {Mixing::kStrict, Mixing::kEnhanced})
    {
        TestMute(archive, mixing);
        TestMeters(archive, mixing);
        TestNote(archive, mixing);
    }

    // The track of the effects' archive sends to buses A and B.
    const std::shared_ptr<const sar::SoundArchive> sends = hokuto::song::LoadArchive(ArchiveBytes(true), "sends");
    const sar::SoundArchive strict_sends = sar::SoundArchive::Load(ArchiveBytes(true), ntdlib::ctr::kSarOptions);
    for (const Mixing mixing : {Mixing::kStrict, Mixing::kEnhanced})
    {
        TestEffects(sends, strict_sends, EffectKind::kCtrReverb, ntdlib::ctr_fx::Library::kNwSnd, mixing);
        TestEffectVolumes(sends, strict_sends, mixing);
        const ReferenceEffects effects{ntdlib::ctr_fx::Library::kNwSnd, TestReverb(EffectKind::kCtrReverb),
                                       TestDelay()};
        TestEffectSwitch(sends, RunReference(strict_sends, mixing), RunReference(strict_sends, mixing, &effects),
                         mixing);
    }
    TestEffects(sends, strict_sends, EffectKind::kCtrNnReverb, ntdlib::ctr_fx::Library::kNnSnd, Mixing::kStrict);
    TestEffects(sends, strict_sends, EffectKind::kCtrNnReverb2011, ntdlib::ctr_fx::Library::kNnSnd2011,
                Mixing::kStrict);

    return xsflib::testing::Summary("ctr");
}
