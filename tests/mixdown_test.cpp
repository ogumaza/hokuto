// SPDX-License-Identifier: MIT

// Tests the mixdown (src/song/mixdown.cpp and mixdown_files.cpp) and the batches (src/song/batch.cpp): the ranges of a
// whole song and of the locators, the render through the player, the fade, the tails, the seamless loop, the stems,
// the files and their names. A
// render has to contain the samples that playback writes with the same settings, sample for sample, and those samples
// have to be ntdlib's model's output run directly. The test archive's looped sequence has its loop from tick 96 to tick
// 192, and plays on track 0 alone.

#include "song/mixdown.h"

#include <algorithm>
#include <bit>
#include <cstdlib>
#include <span>

#include "audio/resampler.h"
#include "edit/sound_exports.h"
#include "song/batch.h"
#include "song/mixdown_files.h"
#include "song_runs.h"

namespace
{

using hokuto::song::BatchFileNames;
using hokuto::song::BatchSound;
using hokuto::song::BatchSounds;
using hokuto::song::LocatorRange;
using hokuto::song::MixdownLoop;
using hokuto::song::MixdownMix;
using hokuto::song::MixdownRange;
using hokuto::song::MixdownWav;
using hokuto::song::NameParts;
using hokuto::song::NameToken;
using hokuto::song::NamingScheme;
using hokuto::song::RenderMixdown;
using hokuto::song::SampleFormat;
using hokuto::song::SchemeName;
using hokuto::song::SeamlessRange;
using hokuto::song::SongRange;
using hokuto::song::StemEffects;
using hokuto::song::StemMix;
using hokuto::song::StemTracks;
using hokuto::song::WavForm;

// The test archive's rate: the Switch's 48 kHz.
constexpr int kRate = 48000;

// A fade of 0.25 s: 12,000 frames, 50 of the model's frames.
constexpr double kFadeSeconds = 0.25;
constexpr std::size_t kFadeFrames = 12000;

// The output of a player of `sequence` from tick `start`, with `cycle` and `mutes`, for up to `frames` of the model's
// frames, in the render worker's blocks.
std::vector<float> PlayedSamples(const std::shared_ptr<const sar::SoundArchive>& archive,
                                 const std::shared_ptr<const Sequence>& sequence, uint32_t start,
                                 PlaybackClock::CycleRange cycle, uint32_t mutes, std::size_t frames)
{
    const auto clock = std::make_shared<PlaybackClock>(kFrameSamples);
    clock->SetCycle(cycle);
    clock->SetMutes(mutes);
    SequencePlayer player(ModelOf(archive, sequence), start, clock);

    return RunPlayer(player, *clock, frames).samples;
}

// The stereo frame at which the fade of `run` starts, in a player's output from the model's frame `first`: the sample
// of the frame that reaches tick `ticks`, at the share of the frame where the tick count passes it.
std::size_t FadeStart(const ModelRun& run, std::size_t first, uint64_t ticks)
{
    const std::size_t frame = FirstFrameReaching(run, ticks);
    const auto before = static_cast<double>(run.before[frame]);
    const auto after = static_cast<double>(run.after[frame]);
    const double share = (static_cast<double>(ticks) - before) / (after - before);

    return (frame - first) * kFrameSamples + static_cast<std::size_t>(std::llround(share * kFrameSamples));
}

// Whether `rendered` contains `played` up to stereo frame `fade_start`, then `played` faded in a straight line over
// `fade` frames, and nothing after the fade.
bool FadesFrom(const std::vector<float>& rendered, const std::vector<float>& played, std::size_t fade_start,
               std::size_t fade)
{
    if (rendered.size() != (fade_start + fade) * 2 || played.size() < rendered.size())
    {
        return false;
    }

    for (std::size_t i = 0; i < rendered.size(); i++)
    {
        const std::size_t frame = i / 2;
        const float gain =
            frame < fade_start
                ? 1.0f
                : static_cast<float>(1.0 - static_cast<double>(frame - fade_start) / static_cast<double>(fade));
        if (rendered[i] != (frame < fade_start ? played[i] : played[i] * gain))
        {
            return false;
        }
    }

    return true;
}

// A whole song that loops plays its loop the given number of times, at least once, and then fades. A song without a
// loop plays to its end and its tails, and keeps its fade for the limit. A seamless loop plays two passes without a
// fade. The locators run from the left one to the right one, in either order.
void TestRanges()
{
    const std::shared_ptr<const sar::SoundArchive> archive = OpenTestArchive();
    const Sequence looped = ReadSequence(*archive, kLoopSound);
    const Sequence ending = ReadSequence(*archive, kNoteSound);

    const MixdownRange two = SongRange(looped);
    const MixdownRange three = SongRange(looped, 3, 4.0);
    const MixdownRange none = SongRange(looped, 0, -1.0);
    const MixdownRange to_end = SongRange(ending);
    const MixdownRange locators = LocatorRange(144, 48, 1.5);
    const MixdownRange seamless = SeamlessRange(looped);
    const MixdownRange seamless_end = SeamlessRange(ending, 2.0);

    SEQLIB_CHECK(two == (MixdownRange{.start = 0, .fade_at = 288, .fade_seconds = 10.0}));
    SEQLIB_CHECK(three == (MixdownRange{.start = 0, .fade_at = 384, .fade_seconds = 4.0}));
    SEQLIB_CHECK(none == (MixdownRange{.start = 0, .fade_at = 192, .fade_seconds = 0.0}));
    SEQLIB_CHECK(to_end == (MixdownRange{.fade_seconds = 10.0, .tail_seconds = 10.0}));
    SEQLIB_CHECK(locators == (MixdownRange{.start = 48, .fade_at = 144, .fade_seconds = 1.5}));
    SEQLIB_CHECK(seamless == (MixdownRange{.fade_at = 288, .fade_seconds = 0.0, .seamless = true}));
    SEQLIB_CHECK(seamless_end == (MixdownRange{.fade_seconds = 10.0, .tail_seconds = 2.0}));
}

// Two passes and a fade contain what playback writes with Cycle on the loop, and fade from the sample where the tick
// count passes the second pass's end. Up to the fade, the samples are ntdlib's model's output run directly.
void TestLoopsAndFade()
{
    const std::shared_ptr<const sar::SoundArchive> archive = OpenTestArchive();
    const auto sequence = std::make_shared<const Sequence>(ReadSequence(*archive, kLoopSound));
    const ModelRun model = RunModel(*archive, kLoopSound, 1000);
    const std::size_t first = FirstFrameReaching(model, 1);
    const std::size_t fade_start = FadeStart(model, first, 288);
    const std::vector<float> played = PlayedSamples(archive, sequence, 0, kGameLoop, 0, 900);

    const hokuto::song::Mixdown rendered =
        RenderMixdown(ModelOf(archive, sequence), SongRange(*sequence, 2, kFadeSeconds));

    std::vector<float> unfaded = rendered.samples;
    unfaded.resize(fade_start * 2);
    SEQLIB_CHECK_EQ(rendered.sample_rate, kRate);
    SEQLIB_CHECK(!rendered.limited && !rendered.cancelled);
    SEQLIB_CHECK(FadesFrom(rendered.samples, played, fade_start, kFadeFrames));
    SEQLIB_CHECK(HasSound(std::span<const float>(unfaded)));
    SEQLIB_CHECK(Matches(unfaded, model, first, 0));
}

// Without a fade, the render stops at the sample where the tick count passes the pass's end. Up to that sample, one
// pass contains what playback writes with Cycle on the loop.
void TestOnePassWithoutFade()
{
    const std::shared_ptr<const sar::SoundArchive> archive = OpenTestArchive();
    const auto sequence = std::make_shared<const Sequence>(ReadSequence(*archive, kLoopSound));
    const ModelRun model = RunModel(*archive, kLoopSound, 1000);
    const std::size_t first = FirstFrameReaching(model, 1);
    const std::size_t end = FadeStart(model, first, 192);
    const std::vector<float> played = PlayedSamples(archive, sequence, 0, kGameLoop, 0, 700);

    const hokuto::song::Mixdown rendered = RenderMixdown(ModelOf(archive, sequence), SongRange(*sequence, 1, 0.0));

    SEQLIB_CHECK(FadesFrom(rendered.samples, played, end, 0));
}

// A song without a loop plays to its end, through its last note's release: what playback writes with Cycle off, every
// sample of it.
void TestToTheEnd()
{
    const std::shared_ptr<const sar::SoundArchive> archive = OpenTestArchive();
    for (const uint32_t sound : {kNoteSound, kReleaseSound})
    {
        const auto sequence = std::make_shared<const Sequence>(ReadSequence(*archive, sound));
        const std::vector<float> played = PlayedSamples(archive, sequence, 0, {}, 0, 4000);

        const hokuto::song::Mixdown rendered = RenderMixdown(ModelOf(archive, sequence), SongRange(*sequence));

        SEQLIB_CHECK(!sequence->loop.has_value());
        SEQLIB_CHECK(played.size() < 4000 * kFrameSamples * 2);
        SEQLIB_CHECK(HasSound(std::span<const float>(rendered.samples)));
        SEQLIB_CHECK(rendered.samples == played);
    }
}

// Tails that sound on past their limit fade over the last second of it, or over the whole of a shorter limit. The limit
// counts from the end of the model's frame in which the sequence ends.
void TestTailLimit()
{
    const std::shared_ptr<const sar::SoundArchive> archive = OpenTestArchive();
    const auto sequence = std::make_shared<const Sequence>(ReadSequence(*archive, kReleaseSound));
    const std::vector<float> played = PlayedSamples(archive, sequence, 0, {}, 0, 4000);

    // The end: the end of the model's frame in which the sequence ends.
    const ModelRun model = RunModel(*archive, kReleaseSound, 4000);
    const EndFrames ends = EndFramesOf(model);
    const std::size_t end = (ends.finished - ends.first) * kFrameSamples;
    SEQLIB_REQUIRE(played.size() / 2 > end + 2);
    const std::size_t tail = (played.size() / 2 - end) / 2;

    const hokuto::song::Mixdown cut =
        RenderMixdown(ModelOf(archive, sequence),
                      SongRange(*sequence, 2, 10.0, static_cast<double>(tail) / static_cast<double>(kRate)));
    const hokuto::song::Mixdown whole = RenderMixdown(ModelOf(archive, sequence), SongRange(*sequence));

    SEQLIB_CHECK(cut.tails_cut && !cut.limited);
    SEQLIB_CHECK(FadesFrom(cut.samples, played, end, tail));
    SEQLIB_CHECK(!whole.tails_cut);
    SEQLIB_CHECK(whole.samples == played);
}

// A seamless loop contains what playback writes with Cycle on the loop, without a fade. Its loop runs from where the
// tick count passes the first pass's end, for a whole number of the model's frames: the jump back lands at the same
// point of a frame. Its end lies within a frame of where the tick count passes the second pass's end, where playback
// repeats the loop's start. The WAV file keeps the loop in its smpl chunk, at another rate too.
void TestSeamless()
{
    const std::shared_ptr<const sar::SoundArchive> archive = OpenTestArchive();
    const auto sequence = std::make_shared<const Sequence>(ReadSequence(*archive, kLoopSound));
    const ModelRun model = RunModel(*archive, kLoopSound, 1000);
    const std::size_t first = FirstFrameReaching(model, 1);
    const std::size_t pass_end = FadeStart(model, first, 192);
    const std::size_t end = FadeStart(model, first, 288);
    const std::vector<float> played = PlayedSamples(archive, sequence, 0, kGameLoop, 0, 900);

    const hokuto::song::Mixdown rendered = RenderMixdown(ModelOf(archive, sequence), SeamlessRange(*sequence));
    SEQLIB_REQUIRE(rendered.loop.has_value());
    const seqlib::WavContents file = seqlib::ReadWav(MixdownWav(rendered.samples, kRate, {}, rendered.loop));
    const seqlib::WavContents moved =
        seqlib::ReadWav(MixdownWav(rendered.samples, kRate, {.rate = 32000}, rendered.loop));

    const std::size_t loop_end = rendered.loop->end;
    SEQLIB_CHECK(FadesFrom(rendered.samples, played, loop_end, 0));
    SEQLIB_CHECK(rendered.loop->start == pass_end);
    // The test archive's next pass repeats the loop's start where the tick count places the end: a whole number of
    // frames after the start.
    SEQLIB_CHECK((loop_end - pass_end) % kFrameSamples == 0);
    SEQLIB_CHECK(loop_end + kFrameSamples > end && loop_end < end + kFrameSamples);
    // Playback repeats the loop's first frames after its end: a sampler's jump back lands in step.
    constexpr std::size_t kRepeated = 4 * kFrameSamples * 2;
    SEQLIB_REQUIRE(played.size() >= loop_end * 2 + kRepeated);
    SEQLIB_CHECK(std::ranges::equal(std::span<const float>(played).subspan(loop_end * 2, kRepeated),
                                    std::span<const float>(rendered.samples).subspan(pass_end * 2, kRepeated)));
    SEQLIB_CHECK(file.pcm.loop && file.pcm.loop_start == pass_end && file.pcm.loop_end == loop_end);
    const std::size_t moved_start = hokuto::audio::ResampledPlace(pass_end, kRate, 32000);
    const std::size_t moved_end = hokuto::audio::ResampledPlace(loop_end, kRate, 32000);
    SEQLIB_CHECK(moved.rate == 32000 && moved.pcm.loop && moved.pcm.loop_start == moved_start);
    SEQLIB_CHECK(moved.pcm.loop_end == moved_end && moved.pcm.channels[0].size() == moved_end);
}

// Every file of a seamless loop ends where the whole mix ends, a silent stem included, and with stems alone too.
// MixdownRange::seamless_end sets the end in place of the search.
void TestSeamlessStems()
{
    const std::shared_ptr<const sar::SoundArchive> archive = OpenTestArchive();
    const auto sequence = std::make_shared<const Sequence>(ReadSequence(*archive, kLoopSound));
    const MixdownRange range = SeamlessRange(*sequence);
    const hokuto::song::Mixdown whole = RenderMixdown(ModelOf(archive, sequence), range);
    SEQLIB_REQUIRE(whole.loop.has_value());

    MixdownRange later = range;
    later.seamless_end = whole.loop->end + kFrameSamples;
    const hokuto::song::Mixdown moved = RenderMixdown(ModelOf(archive, sequence), later);
    SEQLIB_CHECK(moved.loop == (MixdownLoop{.start = whole.loop->start, .end = whole.loop->end + kFrameSamples}));

    hokuto::song::MixdownSettings settings;
    settings.seamless = true;
    std::vector<uint32_t> ends;
    const auto write = [&](const std::string&, std::span<const uint8_t> bytes)
    {
        const seqlib::WavContents file = seqlib::ReadWav(std::vector<uint8_t>(bytes.begin(), bytes.end()));
        ends.push_back(file.pcm.loop ? file.pcm.loop_end : 0);
    };
    const auto make_model = [&](bool)
    {
        return ModelOf(archive, sequence);
    };
    const std::vector<hokuto::song::MixdownFile> with_mix = {{.name = "mix"}, {.name = "silent", .track = 5}};
    const std::vector<hokuto::song::MixdownFile> stems = {{.name = "silent", .track = 5}, {.name = "stem", .track = 0}};
    hokuto::song::RenderSoundFiles(make_model, *sequence, with_mix, settings, write);
    hokuto::song::RenderSoundFiles(make_model, *sequence, stems, settings, write);

    const auto end = static_cast<uint32_t>(whole.loop->end);
    SEQLIB_CHECK(ends == (std::vector<uint32_t>{end, end, end, end}));
}

// The locators start the render at the left one, with the fade in of a seek. Playback starts there the same way.
void TestLocators()
{
    const std::shared_ptr<const sar::SoundArchive> archive = OpenTestArchive();
    const auto sequence = std::make_shared<const Sequence>(ReadSequence(*archive, kLoopSound));
    const ModelRun model = RunModel(*archive, kLoopSound, 1000);
    const std::size_t first = FirstFrameReaching(model, 49);
    const std::size_t fade_start = FadeStart(model, first, 144);
    const std::vector<float> played = PlayedSamples(archive, sequence, 48, kGameLoop, 0, 400);

    const hokuto::song::Mixdown rendered =
        RenderMixdown(ModelOf(archive, sequence), LocatorRange(48, 144, kFadeSeconds));

    SEQLIB_CHECK(FadesFrom(rendered.samples, played, fade_start, kFadeFrames));
}

// A stem plays its track alone. Playback with the other tracks muted plays the same. The test sequence plays track 0
// alone. Its stem is therefore the whole mix, and a stem of another track is silent. Only track 0 gets a stem.
void TestStems()
{
    const std::shared_ptr<const sar::SoundArchive> archive = OpenTestArchive();
    const auto sequence = std::make_shared<const Sequence>(ReadSequence(*archive, kLoopSound));
    const MixdownRange range = SongRange(*sequence, 1, kFadeSeconds);

    const hokuto::song::Mixdown whole = RenderMixdown(ModelOf(archive, sequence), range);
    const hokuto::song::Mixdown stem = RenderMixdown(ModelOf(archive, sequence), range, StemMix(0));
    const hokuto::song::Mixdown other = RenderMixdown(ModelOf(archive, sequence), range, StemMix(5));

    const MixdownMix mix = {.mutes = 2, .bus_volumes = {0.5f, 0.25f, 1.0f}, .gain = 0.5f};
    const hokuto::song::Mixdown muted = RenderMixdown(ModelOf(archive, sequence), range, StemMix(0, {.mutes = 1}));
    const hokuto::song::Mixdown quiet = RenderMixdown(ModelOf(archive, sequence), range, StemMix(0, mix));
    std::vector<float> halved = whole.samples;
    for (float& sample : halved)
    {
        sample *= 0.5f;
    }

    SEQLIB_CHECK(StemTracks(*sequence) == std::vector<int>{0});
    SEQLIB_CHECK(StemMix(3).mutes == ~(1u << 3));
    SEQLIB_CHECK(StemMix(3, mix, StemEffects::kDry) == (MixdownMix{.mutes = ~(1u << 3), .bus_volumes = {}}));
    SEQLIB_CHECK(StemMix(3, mix) == (MixdownMix{.mutes = ~(1u << 3), .bus_volumes = mix.bus_volumes}));
    SEQLIB_CHECK(StemMix(3, mix, StemEffects::kStereoOut) ==
                 (MixdownMix{.mutes = ~(1u << 3), .bus_volumes = mix.bus_volumes, .gain = 0.5f}));
    SEQLIB_CHECK(quiet.samples == whole.samples);
    SEQLIB_CHECK(StemMix(0, {.mutes = 1}).mutes == ~0u);
    SEQLIB_CHECK(muted.samples.size() == whole.samples.size() && !HasSound(std::span<const float>(muted.samples)));
    SEQLIB_CHECK(RenderMixdown(ModelOf(archive, sequence), range, {.gain = 0.5f}).samples == halved);
    SEQLIB_CHECK(HasSound(std::span<const float>(whole.samples)));
    SEQLIB_CHECK(stem.samples == whole.samples);
    SEQLIB_CHECK(other.samples.size() == whole.samples.size());
    SEQLIB_CHECK(!HasSound(std::span<const float>(other.samples)));
}

// A render stops after the frame in which the caller says to stop.
void TestCancel()
{
    const std::shared_ptr<const sar::SoundArchive> archive = OpenTestArchive();
    const auto sequence = std::make_shared<const Sequence>(ReadSequence(*archive, kLoopSound));
    const auto stop_after_ten = [](std::size_t frames)
    {
        return frames < 10 * kFrameSamples;
    };

    const hokuto::song::Mixdown rendered =
        RenderMixdown(ModelOf(archive, sequence), SongRange(*sequence), MixdownMix{}, stop_after_ten);

    SEQLIB_CHECK(rendered.cancelled);
    SEQLIB_CHECK_EQ(rendered.samples.size(), 10 * kFrameSamples * 2);
}

// The archive's sequence sounds, by their numbers and names.
void TestSounds()
{
    const std::shared_ptr<const sar::SoundArchive> archive = OpenTestArchive();

    const std::vector<BatchSound> sounds = BatchSounds(hokuto::song::Played(archive));

    SEQLIB_REQUIRE_EQ(sounds.size(), 4);
    for (std::size_t i = 0; i < sounds.size(); i++)
    {
        SEQLIB_CHECK_EQ(sounds[i].sound, i);
        SEQLIB_CHECK(sounds[i].name == archive->Sounds()[i].name);
    }
}

// A batch reads each sound's sequence, and names the whole mix and the stem of each track that plays notes by the
// naming scheme, uniquely. A sound that can't be read gets no files. An archive plays strictly and without effects,
// unless the batch sets the mixing mode.
void TestBatchFiles()
{
    const std::shared_ptr<const sar::SoundArchive> archive = OpenTestArchive();
    const hokuto::edit::BatchSource source = {.played = hokuto::song::Played(archive)};
    std::vector<BatchSound> sounds = BatchSounds(source.played);
    sounds.push_back({.sound = 99, .name = "MISSING"});

    const std::vector<hokuto::edit::BatchSequence> sequences = hokuto::edit::ReadBatchSequences(source, sounds, 2);
    const NamingScheme scheme = {.tokens = {NameToken::kNumber, NameToken::kSong}};
    const auto files = hokuto::edit::MixdownFileNames(sounds, sequences, scheme, "test", true, true, {});
    const auto midi =
        hokuto::edit::MidiFileNames(sounds, {}, "test", [](const std::string& file) { return file == "SEQ_LOOP.mid"; });

    SEQLIB_REQUIRE_EQ(files.size(), 5);
    SEQLIB_CHECK(sequences[0].sequence && sequences[4].sequence == nullptr && !sequences[4].error.empty());
    SEQLIB_CHECK(files[0] == (std::vector<hokuto::song::MixdownFile>{
                                 {.name = "000 - " + sounds[0].name + ".wav"},
                                 {.name = "000 - " + sounds[0].name + " - Track 0.wav", .track = 0}}));
    SEQLIB_CHECK(files[4].empty());
    SEQLIB_CHECK(midi[0] == sounds[0].name + (sounds[0].name == "SEQ_LOOP" ? " 2.mid" : ".mid"));
    SEQLIB_CHECK(hokuto::edit::BatchModelOptions(source, 0, std::nullopt).mixing == hokuto::song::Mixing::kStrict);

    // The file sets the mixing mode: the last edit's file, with the sound in enhanced mixing.
    auto file = std::make_shared<hokuto::format::File>();
    file->engine = hokuto::format::Engine::kSwitch;
    file->sound_mixing[0] = hokuto::format::Mixing::kEnhanced;
    const hokuto::edit::BatchSource edited = {.played = source.played, .file = file};
    SEQLIB_CHECK(hokuto::edit::BatchModelOptions(edited, 0, std::nullopt).mixing == hokuto::song::Mixing::kEnhanced);
    SEQLIB_CHECK(hokuto::edit::BatchModelOptions(edited, 1, std::nullopt).mixing == hokuto::song::Mixing::kStrict);
    SEQLIB_CHECK(hokuto::edit::BatchModelOptions(source, 0, hokuto::song::Mixing::kEnhanced).mixing ==
                 hokuto::song::Mixing::kEnhanced);
}

// A batch reads each sound with the variables' starting values and changes that the user set for it. Playback reads
// it the same way. Another sound reads with none.
void TestBatchVariables()
{
    const std::shared_ptr<const sar::SoundArchive> archive = OpenVariablesArchive();
    hokuto::edit::BatchSource source = {.played = hokuto::song::Played(archive)};
    const hokuto::song::StartValues values = {{1, 5}, {3, 5}};
    const hokuto::song::Changes changes = {{.time = 10, .variable = 9, .value = 2}};
    source.start_values[kVariablesSound] = values;
    source.changes[kVariablesSound] = changes;
    const std::vector<BatchSound> sounds = {{.sound = kVariablesSound, .name = "Variables"},
                                            {.sound = kNoteSound, .name = "SEQ_NOTE"}};

    const std::vector<hokuto::edit::BatchSequence> read = hokuto::edit::ReadBatchSequences(source, sounds, 1);

    SEQLIB_REQUIRE(read[0].sequence && read[1].sequence);
    const Sequence direct = ReadSequence(*archive, kVariablesSound, values, changes);
    SEQLIB_CHECK(read[0].sequence->start_values == values && read[0].sequence->changes == changes);
    SEQLIB_CHECK(*read[0].sequence == direct);
    SEQLIB_CHECK(read[1].sequence->start_values.empty() && read[1].sequence->changes.empty());
}

// The bytes of the little-endian value of `size` bytes at `offset` of `bytes`.
uint32_t LittleAt(const std::vector<uint8_t>& bytes, std::size_t offset, std::size_t size)
{
    uint32_t value = 0;
    for (std::size_t i = 0; i < size; i++)
    {
        value |= static_cast<uint32_t>(bytes[offset + i]) << (8 * i);
    }

    return value;
}

// 16-bit and 24-bit PCM round to the nearest step and clamp. Floating point keeps each sample, past full scale too. A
// reader reads each file back at its rate. Another rate keeps the length. Normalising brings the peak to full scale. A
// dither moves each 16-bit sample by a step at most, the same way each time.
void TestWav()
{
    const std::vector<float> samples = {0.0f, 0.5f, -0.25f, 1.5f, -2.0f, 1.0f / 65536.0f, 3.0f / 65536.0f, -1.0f};
    const std::vector<int16_t> left16 = {0, -8192, -32768, 2};
    const std::vector<int16_t> right16 = {16384, 32767, 1, -32768};

    const std::vector<uint8_t> pcm16 = MixdownWav(samples, kRate, {});
    const std::vector<uint8_t> pcm24 = MixdownWav(samples, kRate, {.format = SampleFormat::kPcm24});
    const std::vector<uint8_t> floats = MixdownWav(samples, kRate, {.format = SampleFormat::kFloat32});
    const std::vector<float> long_samples(2 * 32728, 0.25f);
    const std::vector<uint8_t> resampled = MixdownWav(long_samples, 32728, {.rate = 48000});
    const std::vector<uint8_t> normalised =
        MixdownWav(samples, kRate, {.format = SampleFormat::kFloat32, .normalise = true});
    const std::vector<uint8_t> dithered = MixdownWav(samples, kRate, {.dither = true});
    const std::vector<uint8_t> looped_floats =
        MixdownWav(samples, kRate, {.format = SampleFormat::kFloat32}, MixdownLoop{.start = 1, .end = 3});

    const seqlib::WavContents read16 = seqlib::ReadWav(pcm16);
    const seqlib::WavContents read24 = seqlib::ReadWav(pcm24);
    const seqlib::WavContents read_floats = seqlib::ReadWav(floats);
    const seqlib::WavContents read_resampled = seqlib::ReadWav(resampled);
    SEQLIB_CHECK(read16.rate == kRate && read16.bits == 16 && !read16.floating);
    SEQLIB_CHECK(read16.pcm.channels.size() == 2 && read16.pcm.channels[0] == left16);
    SEQLIB_CHECK(read16.pcm.channels.size() == 2 && read16.pcm.channels[1] == right16);
    SEQLIB_CHECK(read24.rate == kRate && read24.bits == 24 && !read24.floating);
    SEQLIB_CHECK(read_floats.rate == kRate && read_floats.bits == 32 && read_floats.floating);
    SEQLIB_CHECK(read_resampled.rate == 48000 && read_resampled.pcm.channels[0].size() == 48000);

    // The 24-bit samples: 0.5 is 0x400000, -2.0 clamps to -0x800000, and 3/65536 is 384.
    const std::size_t data24 = pcm24.size() - samples.size() * 3;
    SEQLIB_CHECK_EQ(LittleAt(pcm24, data24 + 3, 3), 0x400000u);
    SEQLIB_CHECK_EQ(LittleAt(pcm24, data24 + 4 * 3, 3), 0x800000u);
    SEQLIB_CHECK_EQ(LittleAt(pcm24, data24 + 6 * 3, 3), 384u);
    const std::size_t data32 = floats.size() - samples.size() * 4;
    SEQLIB_CHECK_EQ(LittleAt(floats, data32 + 3 * 4, 4), std::bit_cast<uint32_t>(1.5f));
    SEQLIB_CHECK_EQ(LittleAt(floats, data32 + 4 * 4, 4), std::bit_cast<uint32_t>(-2.0f));
    SEQLIB_CHECK_EQ(LittleAt(normalised, data32 + 3 * 4, 4), std::bit_cast<uint32_t>(0.75f));
    SEQLIB_CHECK_EQ(LittleAt(normalised, data32 + 4 * 4, 4), std::bit_cast<uint32_t>(-1.0f));

    const seqlib::WavContents read_dithered = seqlib::ReadWav(dithered);
    SEQLIB_CHECK(dithered == MixdownWav(samples, kRate, {.dither = true}));
    for (std::size_t i = 0; i < left16.size(); i++)
    {
        SEQLIB_CHECK(std::abs(read_dithered.pcm.channels[0][i] - left16[i]) <= 1);
        SEQLIB_CHECK(std::abs(read_dithered.pcm.channels[1][i] - right16[i]) <= 1);
    }

    const seqlib::WavContents read_looped = seqlib::ReadWav(looped_floats);
    SEQLIB_CHECK(read_looped.floating && read_looped.pcm.loop);
    SEQLIB_CHECK(read_looped.pcm.loop_start == 1 && read_looped.pcm.loop_end == 3);
}

// The names of a batch's files are safe on every system, unique whatever their case and against files that are there
// already, and never empty. A naming scheme leaves out an empty token with its separator, and a stem's name always
// contains its track.
void TestNames()
{
    const std::vector<std::string> names = {"BGM_TITLE", "bgm_title", "A/B", "", "BGM_TITLE"};

    const std::vector<std::string> files = BatchFileNames(names, ".wav");
    const std::vector<std::string> midi = BatchFileNames(names, ".mid");

    SEQLIB_CHECK(files == (std::vector<std::string>{"BGM_TITLE.wav", "bgm_title 2.wav", "A_B.wav", "Sound 4.wav",
                                                    "BGM_TITLE 3.wav"}));
    SEQLIB_CHECK(midi[1] == "bgm_title 2.mid" && midi[3] == "Sound 4.mid");
    const auto there = [](const std::string& file)
    {
        return file == "BGM_TITLE.wav" || file == "BGM_TITLE 2.wav";
    };
    SEQLIB_CHECK(BatchFileNames(std::vector<std::string>{"BGM_TITLE"}, ".wav", there)[0] == "BGM_TITLE 3.wav");

    const NamingScheme scheme = {.tokens = {NameToken::kProject, NameToken::kNumber, NameToken::kSong}};
    const NamingScheme texts = {.tokens = {NameToken::kText, NameToken::kTrack, NameToken::kSong}, .separator = "_"};
    const NameParts mix = {.project = "", .song = "BGM_TITLE", .number = 7};
    const NameParts stem = {.project = "mset", .song = "BGM_TITLE", .number = 1234, .track = "Track 3"};
    SEQLIB_CHECK(SchemeName(scheme, mix) == "007 - BGM_TITLE");
    SEQLIB_CHECK(SchemeName(scheme, stem) == "mset - 1234 - BGM_TITLE - Track 3");
    SEQLIB_CHECK(SchemeName(texts, stem) == "Track 3_BGM_TITLE");
    SEQLIB_CHECK(SchemeName({.tokens = {}}, mix).empty());
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("mixdown",
                                    []
                                    {
                                        TestRanges();
                                        TestLoopsAndFade();
                                        TestOnePassWithoutFade();
                                        TestToTheEnd();
                                        TestTailLimit();
                                        TestSeamless();
                                        TestSeamlessStems();
                                        TestLocators();
                                        TestStems();
                                        TestCancel();
                                        TestSounds();
                                        TestBatchFiles();
                                        TestBatchVariables();
                                        TestWav();
                                        TestNames();
                                    });
}
