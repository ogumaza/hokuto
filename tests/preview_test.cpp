// SPDX-License-Identifier: MIT

// Tests the previews of waves, notes, sounds and sequences (src/song/preview.cpp and sequence_preview.cpp).

#include "song_runs.h"

namespace
{

// The samples of a wave preview's output that differ from the saw that the test wrote into the wave. The right channel
// falls when `right_falls` is set, and rises like the left channel otherwise.
std::size_t WrongSawSamples(const std::vector<float>& samples, bool right_falls)
{
    std::size_t wrong = 0;
    for (std::size_t i = 0; i < samples.size(); i++)
    {
        const int written = SawSample(static_cast<int>(i / 2), i % 2 == 1 && right_falls);
        if (samples[i] != static_cast<float>(written) / 32768.0f)
        {
            wrong++;
        }
    }

    return wrong;
}

// A wave plays once at its rate, with the samples that the test wrote into it. A mono wave plays in both channels. A
// wave or a wave archive that the archive lacks throws.
void TestWavePreview()
{
    const std::shared_ptr<const sar::SoundArchive> archive = OpenTestArchive();
    WavePreview mono(*archive, 0, 0);
    WavePreview stereo(*archive, 0, 1);

    const std::vector<float> mono_samples = RunPreview(mono, 10);
    const std::vector<float> stereo_samples = RunPreview(stereo, 10);

    SEQLIB_CHECK_EQ(mono.SampleRate(), 48000);
    SEQLIB_CHECK_EQ(mono_samples.size(), 480 * 2);
    SEQLIB_CHECK_EQ(WrongSawSamples(mono_samples, false), 0);
    SEQLIB_CHECK_EQ(stereo_samples.size(), 480 * 2);
    SEQLIB_CHECK_EQ(WrongSawSamples(stereo_samples, true), 0);
    SEQLIB_CHECK(ThrowsRuntimeError([&] { WavePreview missing(*archive, 0, 2); }));
    SEQLIB_CHECK(ThrowsRuntimeError([&] { WavePreview missing(*archive, 1, 0); }));
}

// A wave with a loop plays once without `loops`. With it, the wave plays to its loop's end, then round its loop until
// the preview stops. A wave without a loop plays once either way. The samples after the loop's end never play.
void TestLoopingWavePreview()
{
    sar::Pcm pcm;
    pcm.sample_rate = 32000;
    pcm.loop = true;
    pcm.loop_start = 3;
    pcm.loop_end = 7;
    pcm.channels = {{0, 1000, 2000, 3000, 4000, 5000, 6000, 7000}};
    sar::Pcm without = pcm;
    without.loop = false;
    WavePreview once(pcm);
    WavePreview round(pcm, true);
    WavePreview unlooped(without, true);

    const std::vector<float> once_samples = RunPreview(once, 1, 20);
    const std::vector<float> round_samples = RunPreview(round, 1, 20);
    const std::vector<float> unlooped_samples = RunPreview(unlooped, 1, 20);

    SEQLIB_CHECK_EQ(once_samples.size(), 7 * 2);
    SEQLIB_CHECK_EQ(unlooped_samples.size(), 7 * 2);
    SEQLIB_CHECK_EQ(round_samples.size(), 20 * 2);
    const std::array<int, 20> kPlayed = {0, 1, 2, 3, 4, 5, 6, 3, 4, 5, 6, 3, 4, 5, 6, 3, 4, 5, 6, 3};
    std::size_t wrong = 0;
    for (std::size_t i = 0; i < round_samples.size(); i++)
    {
        wrong += round_samples[i] != static_cast<float>(kPlayed[i / 2] * 1000) / 32768.0f ? 1 : 0;
    }
    SEQLIB_CHECK_EQ(wrong, 0);
}

// A note of the bank's instrument sounds as the loop sequence's first note: key 60 at velocity 100, held for 48 ticks.
// The frame that brings the sequence's tick count to 48 releases that note. Up to that frame, the preview's output is
// the model's output for the sequence. The preview's note holds for 1 s. The preview ends once the release has faded,
// long before its limit of 10 s.
void TestNotePreview()
{
    const std::shared_ptr<const sar::SoundArchive> archive = OpenTestArchive();
    const ModelRun model = RunModel(*archive, kLoopSound, 400);
    const std::size_t first = FirstFrameReaching(model, 1);
    const std::size_t held_together = FirstFrameReaching(model, 48) - first;
    NotePreview preview(archive, 0, 0);

    const std::vector<float> samples = RunPreview(preview, preview.LongestFrames() + 1);

    const std::size_t frames = samples.size() / (kFrameSamples * 2);
    SEQLIB_REQUIRE(frames > held_together && held_together > 90);
    const std::vector<float> held(samples.begin(),
                                  samples.begin() + static_cast<std::ptrdiff_t>(held_together * kFrameSamples * 2));
    SEQLIB_CHECK(HasSound(std::span<const float>(held)));
    SEQLIB_CHECK(Matches(held, model, first, 0));
    SEQLIB_CHECK(frames > preview.HoldFrames() && frames < preview.LongestFrames());
}

// A note of a sound's instrument sounds as the loop sequence's first note: key 60 at velocity 100 on program 0 of the
// bank in slot 0. Up to the frame that releases the sequence's note, the preview's output is the model's. The note
// holds until its flag lets it go, past the browser's hold of 1 s. The preview then ends once the release has faded.
// The bank in slot 1 plays the same note. An empty slot and a program without an instrument play nothing. A softer
// velocity plays more quietly, and another key plays other samples.
void TestHeldNotePreview()
{
    const std::shared_ptr<const sar::SoundArchive> archive = OpenTestArchive();
    const ModelRun model = RunModel(*archive, kLoopSound, 400);
    const std::size_t first = FirstFrameReaching(model, 1);
    const std::size_t held_together = FirstFrameReaching(model, 48) - first;
    const std::vector<uint32_t> banks = archive->Sounds()[kLoopSound].banks;
    const std::vector<uint32_t> in_slot_1 = {sar::kNoItem, banks.at(0)};
    const auto release = std::make_shared<std::atomic<bool>>(false);
    const auto never = std::make_shared<std::atomic<bool>>(false);
    NotePreview note(archive, banks, 0, 0, 60, 100, release);
    NotePreview second_slot(archive, in_slot_1, 1, 0, 60, 100, never);
    NotePreview empty_slot(archive, in_slot_1, 0, 0, 60, 100, never);
    NotePreview no_instrument(archive, banks, 0, 2, 60, 100, never);
    NotePreview soft(archive, banks, 0, 0, 60, 50, never);
    NotePreview higher(archive, banks, 0, 0, 72, 100, never);
    const std::size_t hold = note.HoldFrames() + 100;

    const std::vector<float> held = RunPreview(note, hold);
    release->store(true);
    const std::vector<float> after = RunPreview(note, note.LongestFrames());
    const std::vector<float> second_samples = RunPreview(second_slot, hold);
    const std::vector<float> soft_samples = RunPreview(soft, 100);
    const std::vector<float> higher_samples = RunPreview(higher, 100);

    SEQLIB_REQUIRE(held_together > 90 && held.size() == hold * kFrameSamples * 2);
    const std::vector<float> together(held.begin(),
                                      held.begin() + static_cast<std::ptrdiff_t>(held_together * kFrameSamples * 2));
    const std::span<const float> start = std::span<const float>(held).first(100 * kFrameSamples * 2);
    SEQLIB_CHECK(Matches(together, model, first, 0));
    SEQLIB_CHECK(HasSound(std::span<const float>(held).last(kFrameSamples * 2)));
    const std::size_t after_frames = after.size() / (kFrameSamples * 2);
    SEQLIB_CHECK(after_frames > 0 && hold + after_frames < note.LongestFrames());
    SEQLIB_CHECK(second_samples == held);
    SEQLIB_CHECK(RunPreview(empty_slot, 10).empty());
    SEQLIB_CHECK(RunPreview(no_instrument, 10).empty());
    SEQLIB_REQUIRE(soft_samples.size() == start.size() && higher_samples.size() == start.size());
    SEQLIB_CHECK(Peak(soft_samples) > 0.0f && Peak(soft_samples) < Peak(start));
    SEQLIB_CHECK(HasSound(std::span<const float>(higher_samples)));
    SEQLIB_CHECK(!std::equal(higher_samples.begin(), higher_samples.end(), start.begin()));
}

// A note that ignores note-off sounds until the preview's limit of 10 s. The last frame fades out to silence, from the
// level of the frame before it.
void TestNotePreviewLimit()
{
    const std::shared_ptr<const sar::SoundArchive> archive = OpenTestArchive();
    NotePreview preview(archive, 0, 1);

    const std::vector<float> samples = RunPreview(preview, preview.LongestFrames() + 10);

    const std::size_t frames = samples.size() / (kFrameSamples * 2);
    SEQLIB_REQUIRE_EQ(frames, preview.LongestFrames());
    const std::span<const float> all(samples);
    const std::span<const float> before = all.subspan((frames - 2) * kFrameSamples * 2, kFrameSamples * 2);
    const std::span<const float> last = all.subspan((frames - 1) * kFrameSamples * 2, kFrameSamples * 2);
    SEQLIB_CHECK(HasSound(before) && HasSound(last.first(kFrameSamples)));
    SEQLIB_CHECK(std::abs(last[last.size() - 1]) <= 1.0f / static_cast<float>(kFrameSamples));
    SEQLIB_CHECK(std::abs(last[last.size() - 2]) <= 1.0f / static_cast<float>(kFrameSamples));
}

// The Wii U's archive holds the same bank and waves, in the Wii U's byte order. A wave plays the samples that the test
// wrote into it. A note plays on the Wii U's model, in frames of 3 ms at 48 kHz. It holds for 333 frames, sounds, and
// ends once its release has faded, before the limit of 3,333 frames. The bank in a sound's slot 1 plays the same note
// while both hold.
void TestWiiUPreviews()
{
    const std::shared_ptr<const sar::SoundArchive> archive = OpenWiiUArchive();
    constexpr std::size_t kWiiUFrameSamples = cafe::ArchiveModel::kFrameSamples;
    const std::vector<uint32_t> in_slot_1 = {sar::kNoItem, archive->Sounds()[kNoteSound].banks.at(0)};
    WavePreview wave(*archive, 0, 1);
    NotePreview note(archive, 0, 0);
    NotePreview held(archive, in_slot_1, 1, 0, 60, 100, std::make_shared<std::atomic<bool>>(false));

    const std::vector<float> wave_samples = RunPreview(wave, 10);
    const std::vector<float> note_samples = RunPreview(note, note.LongestFrames() + 1, kWiiUFrameSamples);
    const std::vector<float> held_samples = RunPreview(held, 300, kWiiUFrameSamples);

    SEQLIB_CHECK(ProgramsOf(*archive, 0) == std::vector<uint32_t>({0, 1, 3}));
    SEQLIB_CHECK_EQ(WaveCountOf(*archive, 0), 2);
    SEQLIB_CHECK_EQ(wave_samples.size(), 480 * 2);
    SEQLIB_CHECK_EQ(WrongSawSamples(wave_samples, true), 0);
    SEQLIB_CHECK_EQ(note.SampleRate(), 48000);
    SEQLIB_CHECK_EQ(note.HoldFrames(), uint64_t{333});
    SEQLIB_CHECK_EQ(note.LongestFrames(), uint64_t{3333});
    const std::size_t frames = note_samples.size() / (kWiiUFrameSamples * 2);
    SEQLIB_CHECK(HasSound(std::span<const float>(note_samples)));
    SEQLIB_CHECK(frames > note.HoldFrames() && frames < note.LongestFrames());
    SEQLIB_REQUIRE_EQ(held_samples.size(), 300 * kWiiUFrameSamples * 2);
    SEQLIB_REQUIRE(note_samples.size() >= held_samples.size());
    SEQLIB_CHECK(std::equal(held_samples.begin(), held_samples.end(), note_samples.begin()));
}

// The output of ntdlib's model of the Wii run directly, as floats, with key 60 at velocity 100 on program `program` of
// the archive's bank, on `build` of NW4R SND: a note-off before frame `hold`, and frames until nothing sounds or
// `frames` have run.
std::vector<float> WiiNoteOnModel(const ntdlib::rsar::SoundArchive& archive, uint32_t program,
                                  const ntdlib::SndBuild& build, std::size_t hold, std::size_t frames)
{
    ntdlib::rvl::snd::ArchiveModel model;
    model.build = build;
    model.StartWithRenderer(archive);
    ntdlib::rvl::snd::NoteOnInfo info;
    info.prg_no = program;
    info.key = 60;
    info.velocity = 100;
    std::vector<float> samples;
    if (model.engine->NoteOn({0}, 0, info) == nullptr)
    {
        return samples;
    }

    std::vector<int16_t> output;
    for (std::size_t frame = 0; frame < frames && model.engine->IsBusy(); frame++)
    {
        if (frame == hold)
        {
            ntdlib::rvl::snd::ChannelManager& channels = model.engine->Channels();
            const std::vector<ntdlib::nwsnd::ChannelId> places(channels.InUse().begin(), channels.InUse().end());
            for (const ntdlib::nwsnd::ChannelId place : places)
            {
                channels.At(place).NoteOff(*model.engine);
            }
        }
        output.clear();
        if (!model.RunFrame(output))
        {
            break;
        }
        for (const int16_t sample : output)
        {
            samples.push_back(static_cast<float>(sample) / 32768.0f);
        }
    }

    return samples;
}

// A Wii bank's note plays on ntdlib's model of NW4R SND and AX, with the model of AX's microcode, in frames of 3 ms at
// 32 kHz. Its output is the model's output run directly with the same note: key 60 at velocity 100 on program 0, with a
// note-off after 333 frames. The preview ends with the model's output, before its limit of 3,333 frames. Here each
// region holds its level for a while after its attack, then decays, and its release ends soon. The archive's version,
// 1.4, gives the build of June 2008. That build follows the region's hold. A build of 2007 decays at once. A preview on
// that build plays as the model on that build, and otherwise than on the archive's. A note of a sound's instrument
// plays the same samples while it holds. A bank slot that the sound lacks, and a program without a sound, play nothing.
// A wave plays the samples that ntdlib decodes from it, at its rate. A mono wave plays in both channels.
void TestWiiPreviews()
{
    constexpr std::size_t kWiiFrameSamples = 96;
    std::vector<uint8_t> bytes = WiiArchiveBytes();
    constexpr std::array<uint8_t, 4> kTune = {0x3F, 0x80, 0x00, 0x00};
    std::size_t regions = 0;
    for (std::size_t at = 0; at + 0x14 <= bytes.size(); at++)
    {
        // A region: its wave's index, then its attack of 127 at byte 4, its volume of 100 at byte 0xD, its pan of 64 at
        // byte 0xE, and at byte 0x10 its tuning of 1.0. The decay lies at byte 5, the release at byte 7 and the hold at
        // byte 8.
        if (bytes[at + 4] == 127 && bytes[at + 0xD] == 100 && bytes[at + 0xE] == 64 &&
            std::memcmp(&bytes[at + 0x10], kTune.data(), kTune.size()) == 0)
        {
            bytes[at + 5] = 120;
            bytes[at + 7] = 120;
            bytes[at + 8] = 100;
            regions++;
        }
    }
    const std::shared_ptr<const ntdlib::rsar::SoundArchive> archive =
        LoadWiiArchive(std::move(bytes), "SOUND.brsar", ntdlib::rvl::snd::kRsarOptions);
    const ntdlib::SndBuild june_2008 = export_::BuildOfRsarVersion(archive->Version());
    const ntdlib::SndBuild of_2007 = export_::BuildOfRsarVersion(0x0103);
    const auto never = std::make_shared<std::atomic<bool>>(false);
    NotePreview note(archive, 0, 0);
    NotePreview older(archive, 0, 0, of_2007);
    NotePreview held(archive, std::vector<uint32_t>{0}, 0, 0, 60, 100, never);
    NotePreview no_slot(archive, std::vector<uint32_t>{0}, 1, 0, 60, 100, never);
    NotePreview silent(archive, 0, 3);
    const uint32_t bank_file = archive->Banks().at(0).file_id;
    const ntdlib::rsar::Pcm pcm = hokuto::song::DecodeItemWave(*archive, bank_file, 1);
    WavePreview wave(pcm);

    const std::vector<float> note_samples = RunPreview(note, note.LongestFrames() + 1, kWiiFrameSamples);
    const std::vector<float> older_samples = RunPreview(older, older.LongestFrames() + 1, kWiiFrameSamples);
    const std::vector<float> held_samples = RunPreview(held, 300, kWiiFrameSamples);
    const std::vector<float> expected = WiiNoteOnModel(*archive, 0, june_2008, 333, 3333);
    const std::vector<float> expected_older = WiiNoteOnModel(*archive, 0, of_2007, 333, 3333);
    const std::vector<float> wave_samples = RunPreview(wave, 1, pcm.Length() + 10);

    SEQLIB_CHECK_EQ(regions, 8);
    SEQLIB_CHECK(june_2008.SinceMarch2008() && !of_2007.SinceMarch2008());
    SEQLIB_CHECK_EQ(note.SampleRate(), 32000);
    SEQLIB_CHECK_EQ(note.HoldFrames(), uint64_t{333});
    SEQLIB_CHECK_EQ(note.LongestFrames(), uint64_t{3333});
    const std::size_t frames = note_samples.size() / (kWiiFrameSamples * 2);
    SEQLIB_CHECK(frames > note.HoldFrames() && frames < note.LongestFrames());
    SEQLIB_CHECK(HasSound(std::span<const float>(note_samples)));
    SEQLIB_CHECK(note_samples == expected);
    SEQLIB_CHECK(older_samples == expected_older);
    SEQLIB_CHECK(older_samples != note_samples);
    SEQLIB_REQUIRE_EQ(held_samples.size(), 300 * kWiiFrameSamples * 2);
    SEQLIB_REQUIRE(note_samples.size() >= held_samples.size());
    SEQLIB_CHECK(std::equal(held_samples.begin(), held_samples.end(), note_samples.begin()));
    SEQLIB_CHECK(RunPreview(no_slot, 10, kWiiFrameSamples).empty());
    SEQLIB_CHECK(RunPreview(silent, 10, kWiiFrameSamples).empty());
    const std::vector<uint32_t> programs = ProgramsOf(*archive, 0);
    SEQLIB_CHECK(!programs.empty() && programs.front() == 0);
    SEQLIB_CHECK(std::ranges::find(programs, 3u) == programs.end());
    SEQLIB_CHECK_EQ(wave.SampleRate(), 32000);
    SEQLIB_REQUIRE(pcm.channels.size() == 1 && wave_samples.size() == pcm.Length() * 2);
    std::size_t wrong = 0;
    for (std::size_t i = 0; i < wave_samples.size(); i++)
    {
        wrong += wave_samples[i] != static_cast<float>(pcm.channels[0][i / 2]) / 32768.0f ? 1 : 0;
    }
    SEQLIB_CHECK_EQ(wrong, 0);
    SEQLIB_CHECK(HasSound(std::span<const float>(wave_samples)));
}

// The output of the model of the archive's console for `frames` frames, with wave sound `sound` started on it, as
// floats.
template <class ArchiveModel>
std::vector<float> WaveSoundOnModel(const sar::SoundArchive& archive, uint32_t sound, std::size_t frames)
{
    ArchiveModel model(archive);
    model.engine->StartWaveSound(sound);
    std::vector<float> samples;
    for (std::size_t frame = 0; frame < frames; frame++)
    {
        model.engine->RunFrame();
        std::span<const int16_t> output;
        if constexpr (std::is_same_v<ArchiveModel, cafe::ArchiveModel>)
        {
            output = model.system.Output();
        }
        else
        {
            output = model.renderer.DeviceOutput();
        }
        for (const int16_t sample : output)
        {
            samples.push_back(static_cast<float>(sample) / 32768.0f);
        }
    }

    return samples;
}

// The peaks of the left and the right samples of interleaved stereo.
std::array<float, 2> SidePeaks(const std::vector<float>& samples)
{
    std::array<float, 2> peaks = {0.0f, 0.0f};
    for (std::size_t i = 0; i < samples.size(); i++)
    {
        peaks[i % 2] = std::max(peaks[i % 2], std::abs(samples[i]));
    }

    return peaks;
}

// A wave sound plays on the console's model as ntdlib's player of wave sounds plays it: the preview's output is the
// model's, sample for sample. The wave of the sound's first note plays: WSD_HIGH's stereo saw, and WSD_PLAIN's mono saw
// at the centre. The saws loop, and the previews play on. The wave sounds without a wave to play throw with a message
// for the user, and so does a sequence sound. A wave sound at a pitch of -1 plays without a failure. The Wii U's wave
// sounds play on its model the same way.
void TestSoundPreview()
{
    const TemporaryFile switch_file(sar_files::Archive(false, WaveSoundArchiveSpec(false)));
    const TemporaryFile wii_u_file(sar_files::Archive(true, WaveSoundArchiveSpec(true)));
    const std::shared_ptr<const sar::SoundArchive> archive = OpenArchive(switch_file.Path());
    const std::shared_ptr<const sar::SoundArchive> wii_u = OpenArchive(wii_u_file.Path());
    constexpr std::size_t kWiiUFrameSamples = cafe::ArchiveModel::kFrameSamples;
    SoundPreview high(archive, kHighSound);
    SoundPreview plain(archive, kPlainSound);
    SoundPreview backwards(archive, kBackwardsSound);
    SoundPreview wii_u_high(wii_u, kHighSound);

    const std::vector<float> high_samples = RunPreview(high, 200);
    const std::vector<float> plain_samples = RunPreview(plain, 200);
    const std::vector<float> wii_u_samples = RunPreview(wii_u_high, 200, kWiiUFrameSamples);
    const std::size_t backwards_frames = RunPreview(backwards, 50).size() / (kFrameSamples * 2);

    const auto message = [](const std::shared_ptr<const sar::SoundArchive>& from, uint32_t sound)
    {
        return ErrorOf<std::runtime_error>([&] { const SoundPreview preview(from, sound); });
    };

    SEQLIB_CHECK(high.SampleRate() == 48000 && high.LongestFrames() == 120000 && wii_u_high.LongestFrames() == 200000);
    SEQLIB_REQUIRE(high_samples.size() == 200 * kFrameSamples * 2 && plain_samples.size() == high_samples.size());
    SEQLIB_CHECK(high_samples == WaveSoundOnModel<atk::ArchiveModel>(*archive, kHighSound, 200));
    SEQLIB_CHECK(plain_samples == WaveSoundOnModel<atk::ArchiveModel>(*archive, kPlainSound, 200));
    SEQLIB_CHECK(wii_u_samples == WaveSoundOnModel<cafe::ArchiveModel>(*wii_u, kHighSound, 200));
    const std::array<float, 2> high_peaks = SidePeaks(high_samples);
    const std::array<float, 2> plain_peaks = SidePeaks(plain_samples);
    SEQLIB_CHECK(high_peaks[0] > 0.0f && high_peaks[1] > 0.0f);
    SEQLIB_CHECK(plain_peaks[0] > 0.0f && plain_peaks[0] == plain_peaks[1]);
    SEQLIB_CHECK(SidePeaks(wii_u_samples)[0] > 0.0f);
    SEQLIB_CHECK(backwards_frames > 0);
    SEQLIB_CHECK(message(archive, kSilentSound) == "WSD_SILENT has no notes.");
    SEQLIB_CHECK(message(archive, kPastSound) == "The wave sound file of WSD_PAST has no wave sound 8.");
    SEQLIB_CHECK(message(archive, kLostSound) == "The archive is missing the wave archive that WSD_LOST uses.");
    SEQLIB_CHECK(message(archive, kNoNoteSound) == "WSD_NO_NOTE has no notes.");
    SEQLIB_CHECK(message(archive, kNoWaveSound) ==
                 "The wave sound file of WSD_NO_WAVE is missing the wave that the sound uses.");
    SEQLIB_CHECK(message(archive, kLoopSound) ==
                 archive->Sounds()[kLoopSound].name + " is neither a wave sound nor a stream sound.");
    SEQLIB_CHECK(message(wii_u, kSilentSound) == "WSD_SILENT has no notes.");
}

// A preview plays key 60, or the nearest key with a sound. Of two keys equally near, the lower one plays. An instrument
// without a sound plays no key.
void TestPreviewKey()
{
    SEQLIB_CHECK_EQ(NotePreview::KeyOf(InstrumentOn({{0, 127}})), 60);
    SEQLIB_CHECK_EQ(NotePreview::KeyOf(InstrumentOn({{70, 80}})), 70);
    SEQLIB_CHECK_EQ(NotePreview::KeyOf(InstrumentOn({{50, 55}, {66, 70}})), 55);
    SEQLIB_CHECK_EQ(NotePreview::KeyOf(InstrumentOn({{62, 62}, {58, 58}})), 58);
    SEQLIB_CHECK_EQ(NotePreview::KeyOf(InstrumentOn({})), -1);
}

// A sequence sound plays on the model from its first frame. The preview's output is the model's output for the sound,
// frame for frame, with the effects on the aux buses at the buses' volumes. The loop sequence sends its notes to bus A,
// and the reverb there at half volume plays otherwise than at full volume. It plays on to the preview's limit of 30 s,
// and the last frame fades out to silence. The note sequence ends once its last note has faded, long before the limit.
void TestSequencePreview()
{
    const std::shared_ptr<const sar::SoundArchive> archive = OpenSendArchive(kSendLoopBytecode);
    constexpr AuxVolumes kHalf = {0.5f, 0.5f, 0.5f};
    const ModelRun model = RunModel(*archive, kSendSound, 400, AuxReverb(), {}, {}, kHalf);
    const ModelRun full = RunModel(*archive, kSendSound, 400, AuxReverb());
    const auto loop = std::make_shared<const Sequence>(ReadSequence(*archive, kSendSound));
    const auto note = std::make_shared<const Sequence>(ReadSequence(*archive, kNoteSound));
    SequencePreview preview(archive, loop, hokuto::song::ModelOptions{.effects = AuxEffectsWithReverb()}, kHalf);
    SequencePreview ending(archive, note);

    const std::vector<float> samples = RunPreview(preview, preview.LongestFrames() + 10);
    const std::vector<float> ending_samples = RunPreview(ending, ending.LongestFrames());

    SEQLIB_CHECK_EQ(preview.SampleRate(), 48000);
    SEQLIB_CHECK_EQ(preview.LongestFrames(), uint64_t{6000});
    SEQLIB_REQUIRE_EQ(samples.size(), preview.LongestFrames() * kFrameSamples * 2);
    const std::vector<float> start(samples.begin(), samples.begin() + 400 * kFrameSamples * 2);
    SEQLIB_CHECK(HasSound(std::span<const float>(start)));
    SEQLIB_CHECK(Matches(start, model, 0, 0));
    SEQLIB_CHECK(model.samples != full.samples);
    const std::span<const float> last = std::span<const float>(samples).last(kFrameSamples * 2);
    SEQLIB_CHECK(std::abs(last[last.size() - 1]) <= 1.0f / static_cast<float>(kFrameSamples));
    const std::size_t ending_frames = ending_samples.size() / (kFrameSamples * 2);
    SEQLIB_CHECK(ending_frames > 0 && ending_frames < ending.LongestFrames());
    SEQLIB_CHECK(HasSound(std::span<const float>(ending_samples)));
}

// The output of ntdlib's model of the Wii run directly, as floats, with wave sound `sound` started on `build` of NW4R
// SND: frames until nothing sounds or `frames` have run.
std::vector<float> WiiWaveSoundOnModel(const ntdlib::rsar::SoundArchive& archive, uint32_t sound,
                                       const ntdlib::SndBuild& build, std::size_t frames)
{
    ntdlib::rvl::snd::ArchiveModel model;
    model.build = build;
    model.StartWithRenderer(archive);
    std::vector<float> samples;
    if (model.engine->StartWaveSound(sound) == nullptr)
    {
        return samples;
    }

    std::vector<int16_t> output;
    for (std::size_t frame = 0; frame < frames && model.engine->IsBusy(); frame++)
    {
        output.clear();
        if (!model.RunFrame(output))
        {
            break;
        }
        for (const int16_t sample : output)
        {
            samples.push_back(static_cast<float>(sample) / 32768.0f);
        }
    }

    return samples;
}

// The Wii's wave sounds of MakeWiiWaveSoundArchives. The browser's preview plays a wave sound as ntdlib's model of NW4R
// SND and AX run directly, sample for sample, on the build that the archive's version gives. A wave that doesn't loop
// ends the preview, long before its limit. A wave that loops plays on. A wave sound without a note, one whose note's
// wave the file lacks and a sound past the table can't start a preview.
void TestWiiWaveSoundPreview()
{
    constexpr std::size_t kWiiFrameSamples = 96;
    const WiiWaveSoundArchives made = MakeWiiWaveSoundArchives();
    const std::shared_ptr<const ntdlib::rsar::SoundArchive>& archive = made.archive;
    const std::shared_ptr<const ntdlib::rsar::SoundArchive>& old_archive = made.old_archive;
    const ntdlib::SndBuild build = export_::BuildOfRsarVersion(archive->Version());

    // The previews against the model. 9,600 samples at a pitch of 1.5 take 6,400 samples of the output: about 67
    // frames.
    SoundPreview once_preview(archive, 0);
    SoundPreview round_preview(archive, 1);
    SoundPreview old_preview(old_archive, 0);

    const std::vector<float> once_samples =
        RunPreview(once_preview, once_preview.LongestFrames() + 1, kWiiFrameSamples);
    const std::vector<float> round_samples = RunPreview(round_preview, 200, kWiiFrameSamples);
    const std::vector<float> old_samples = RunPreview(old_preview, 200, kWiiFrameSamples);

    SEQLIB_CHECK_EQ(once_preview.SampleRate(), 32000);
    SEQLIB_CHECK_EQ(once_preview.LongestFrames(), uint64_t{200000});
    SEQLIB_CHECK(HasSound(std::span<const float>(once_samples)));
    SEQLIB_CHECK(once_samples == WiiWaveSoundOnModel(*archive, 0, build, 1000));
    const std::size_t frames = once_samples.size() / (kWiiFrameSamples * 2);
    SEQLIB_CHECK(frames >= 67 && frames < 75);
    SEQLIB_REQUIRE_EQ(round_samples.size(), 200 * kWiiFrameSamples * 2);
    SEQLIB_CHECK(round_samples == WiiWaveSoundOnModel(*archive, 1, build, 200));
    SEQLIB_CHECK(HasSound(std::span<const float>(round_samples).subspan(190 * kWiiFrameSamples * 2)));
    SEQLIB_CHECK(old_samples == WiiWaveSoundOnModel(*old_archive, 0, build, 200));
    SEQLIB_CHECK(HasSound(std::span<const float>(old_samples)));
    SEQLIB_CHECK(ThrowsRuntimeError([&] { SoundPreview missing(archive, 2); }));
    SEQLIB_CHECK(ThrowsRuntimeError([&] { SoundPreview silent(archive, 3); }));
    SEQLIB_CHECK(ThrowsRuntimeError([&] { SoundPreview past(archive, 4); }));
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("preview",
                                    []
                                    {
                                        TestWavePreview();
                                        TestLoopingWavePreview();
                                        TestNotePreview();
                                        TestHeldNotePreview();
                                        TestNotePreviewLimit();
                                        TestWiiUPreviews();
                                        TestWiiPreviews();
                                        TestSoundPreview();
                                        TestPreviewKey();
                                        TestSequencePreview();
                                        TestWiiWaveSoundPreview();
                                    });
}
