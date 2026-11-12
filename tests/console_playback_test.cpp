// SPDX-License-Identifier: MIT

// Tests the playback of the Wii U, the Wii and the 3DS on their models, with their effects.

#include "song_runs.h"

namespace
{

// A Wii U archive plays on ntdlib's model of nw::snd and AX, in frames of 3 ms: 144 samples at 48 kHz. The model's
// frames match the model run directly, from the start and after a seek back to the snapshot at frame 50. So does the
// player's output after its first frame, from the model's first frame that runs a tick. The player's first frame fades
// in. The note lasts 0.25 s, and the output ends once it has faded.
void TestWiiUPlays()
{
    constexpr std::size_t kWiiUFrame = cafe::ArchiveModel::kFrameSamples * 2;
    const std::shared_ptr<const sar::SoundArchive> archive = OpenWiiUArchive();
    const auto sequence = std::make_shared<const Sequence>(ReadSequence(*archive, kNoteSound));
    cafe::ArchiveModel direct(*archive);
    const cafe::SequenceSoundPlayer* sound = direct.engine->StartSequence(kNoteSound);
    SEQLIB_REQUIRE(sound != nullptr);
    std::vector<int16_t> expected;
    std::size_t first = 0;
    for (std::size_t frame = 0; frame < 600; frame++)
    {
        direct.engine->RunFrame();
        const std::span<const int16_t> output = direct.system.Output();
        expected.insert(expected.end(), output.begin(), output.end());
        first += sound->TickCount() == 0 ? 1 : 0;
    }

    const std::shared_ptr<SequenceModel> model = ModelOf(archive, sequence, 50);
    std::vector<int16_t> played;
    for (bool more = model->SeekFrame(0); more && played.size() < expected.size(); more = model->RunFrame())
    {
        const std::span<const int16_t> output = model->Output();
        played.insert(played.end(), output.begin(), output.end());
    }
    const bool sought = model->SeekFrame(70);
    const std::vector<int16_t> after_seek(model->Output().begin(), model->Output().end());
    const auto clock = std::make_shared<PlaybackClock>(model->FrameSamples());
    SequencePlayer player(model, 0, clock);
    std::vector<float> heard(expected.size());
    const std::size_t rendered = player.Render(heard.data(), heard.size() / 2);
    heard.resize(rendered * 2);

    SEQLIB_CHECK(archive->ReadOptions().flavor == sar::Flavor::kCafe);
    SEQLIB_CHECK_EQ(model->FrameSamples(), cafe::ArchiveModel::kFrameSamples);
    SEQLIB_CHECK_EQ(model->SampleRate(), 48000);
    SEQLIB_CHECK_EQ(sequence->timebase, 48);
    SEQLIB_REQUIRE(played.size() > 80 * kWiiUFrame && played.size() < expected.size());
    SEQLIB_CHECK(HasSound(std::span<const int16_t>(played)));
    SEQLIB_CHECK(std::equal(played.begin(), played.end(), expected.begin()));
    SEQLIB_CHECK(!HasSound(std::span<const int16_t>(expected).subspan(played.size())));
    SEQLIB_CHECK(sought && after_seek.size() == kWiiUFrame &&
                 std::equal(after_seek.begin(), after_seek.end(), expected.begin() + 70 * kWiiUFrame));
    SEQLIB_REQUIRE(rendered < expected.size() / 2 && heard.size() > 2 * kWiiUFrame);
    std::size_t wrong = 0;
    for (std::size_t i = kWiiUFrame; i < heard.size() - kWiiUFrame; i++)
    {
        wrong += heard[i] != static_cast<float>(expected[first * kWiiUFrame + i]) / 32768.0f ? 1 : 0;
    }
    SEQLIB_CHECK_EQ(wrong, 0);
}

// A Wii archive's sequence reads with revosf2's performer, and plays as ntdlib's model of NW4R SND and AX plays it run
// directly, with ntdlib's model of AX's microcode in place of the DSP: 96 stereo samples at 32 kHz in each frame, for
// 2.1 s from the start and after a seek. A muted track goes silent, also after a seek, and sounds again once the mute
// ends. The reader gives the same sequence.
void TestWiiPlays()
{
    constexpr std::size_t kWiiFrame = 96 * 2;
    const std::shared_ptr<const ntdlib::rsar::SoundArchive> archive = OpenWiiArchive();
    const auto sequence = std::make_shared<const Sequence>(ReadSequence(*archive, 0));
    ntdlib::rvl::snd::ArchiveModel direct;
    direct.build = export_::BuildOfRsarVersion(archive->Version());
    direct.StartWithRenderer(*archive);
    SEQLIB_REQUIRE(direct.engine->StartSequence(0) != nullptr);
    std::vector<int16_t> expected;
    for (int frame = 0; frame < 700; frame++)
    {
        SEQLIB_REQUIRE(direct.RunFrame(expected));
    }

    const auto model =
        std::make_shared<SequenceModel>(archive, sequence, hokuto::song::ModelOptions{.snapshot_interval = 50});
    std::vector<int16_t> played;
    for (bool more = model->SeekFrame(0); more && played.size() < expected.size(); more = model->RunFrame())
    {
        const std::span<const int16_t> output = model->Output();
        played.insert(played.end(), output.begin(), output.end());
    }
    const bool sought = model->SeekFrame(70);
    const std::vector<int16_t> after_seek(model->Output().begin(), model->Output().end());

    // A muted track goes silent, and comes back once the mute ends. A seek keeps the mute. The mute reaches the engine:
    // with the only track muted, the sound ends with its note's 48 ticks.
    const auto muted =
        std::make_shared<SequenceModel>(archive, sequence, hokuto::song::ModelOptions{.snapshot_interval = 50});
    muted->SetMutes(1);
    std::vector<int16_t> silent;
    for (bool more = muted->SeekFrame(0); more && silent.size() < 300 * kWiiFrame; more = muted->RunFrame())
    {
        silent.insert(silent.end(), muted->Output().begin(), muted->Output().end());
    }
    const bool muted_seek = muted->SeekFrame(70) && !HasSound(muted->Output());
    muted->SetMutes(0);
    std::vector<int16_t> unmuted;
    for (int frame = 0; frame < 10 && muted->RunFrame(); frame++)
    {
        unmuted.insert(unmuted.end(), muted->Output().begin(), muted->Output().end());
    }
    SequenceReader reader;
    reader.Request(archive, 0);
    const std::optional<SequenceReader::Result> read = TakeRead(reader);

    SEQLIB_CHECK_EQ(model->FrameSamples(), 96);
    SEQLIB_CHECK_EQ(model->SampleRate(), 32000);
    SEQLIB_CHECK_EQ(sequence->timebase, 48);
    SEQLIB_REQUIRE(sequence->tracks.size() == 1 && sequence->tracks[0].notes.size() == 1);
    const hokuto::song::Note& note = sequence->tracks[0].notes[0];
    SEQLIB_CHECK(note.start == 0 && note.length == 48 && note.key == 60 && note.velocity == 100);
    SEQLIB_REQUIRE(played.size() == expected.size());
    SEQLIB_CHECK(HasSound(std::span<const int16_t>(played)));
    SEQLIB_CHECK(played == expected);
    SEQLIB_CHECK(sought && after_seek.size() == kWiiFrame &&
                 std::equal(after_seek.begin(), after_seek.end(), expected.begin() + 70 * kWiiFrame));
    SEQLIB_CHECK(read && read->sequence && *read->sequence == *sequence && !read->archive);
    SEQLIB_CHECK(silent.size() > 100 * kWiiFrame && silent.size() < 300 * kWiiFrame);
    SEQLIB_CHECK(!HasSound(std::span<const int16_t>(silent)));
    SEQLIB_CHECK(muted_seek && HasSound(std::span<const int16_t>(unmuted)));
}

// The index after the last sample of `samples` that's louder than ±8, or 0.
std::size_t SoundEnd(std::span<const int16_t> samples)
{
    const auto loud = [](int16_t sample)
    {
        return std::abs(sample) > 8;
    };
    const auto last = std::find_if(samples.rbegin(), samples.rend(), loud);
    return static_cast<std::size_t>(samples.rend() - last);
}

// Each effect of the Wii U on aux bus A plays as ntdlib's model plays it on the TV's bus A, and changes the output. Its
// tail sounds after the note's release, and the output ends a second after the tail: a delay's echoes can come back
// after a quiet gap. A seek back to a snapshot restores the effect's state. An effect on bus B changes nothing. The
// sequence doesn't send there, and ntdlib's model of AX drops a send to a bus without an effect. The output then ends a
// second after the note. An I3DL2 reverb plays on the Switch only. Without it, the output ends with the note.
void TestWiiUEffects()
{
    constexpr std::size_t kWiiUFrame = cafe::ArchiveModel::kFrameSamples * 2;
    constexpr std::size_t kFrames = 3000;
    const std::shared_ptr<const sar::SoundArchive> archive = OpenWiiUSendArchive();
    const auto sequence = std::make_shared<const Sequence>(ReadSequence(*archive, kNoteSound));
    const std::vector<int16_t> dry = RunWiiUModel(*archive, kNoteSound, kFrames);
    const std::size_t dry_end = SoundEnd(dry);

    std::size_t wrong = 0;
    for (const cafe::EffectType type : {cafe::EffectType::kReverbHi, cafe::EffectType::kReverbStd,
                                        cafe::EffectType::kChorus, cafe::EffectType::kDelay})
    {
        const hokuto::song::WiiUEffect effect =
            hokuto::song::WiiUEffectOf(type, hokuto::song::WiiUStartingParameters(type));
        const std::vector<int16_t> direct =
            RunWiiUModel(*archive, kNoteSound, kFrames, {{cafe::kEffectDeviceTv, 0, type, effect.words, {}}});
        const auto model = ModelOf(archive, sequence, 50, {{{effect}, {}, {}}});
        const std::vector<int16_t> played = RunToEnd(*model, direct.size());
        const bool sought = model->SeekFrame(70);
        const std::span<const int16_t> after_seek = model->Output();

        const std::size_t end = SoundEnd(direct);
        const bool matches = played.size() > end && played.size() < direct.size() &&
                             std::equal(played.begin(), played.end(), direct.begin());
        const bool changes = !std::equal(dry.begin(), dry.begin() + static_cast<std::ptrdiff_t>(end), direct.begin());
        const bool tail = end > dry_end && played.size() >= end + 330 * kWiiUFrame;
        const bool restores = sought && after_seek.size() == kWiiUFrame &&
                              std::equal(after_seek.begin(), after_seek.end(), direct.begin() + 70 * kWiiUFrame);
        wrong += matches && changes && tail && restores ? 0 : 1;
    }

    const hokuto::song::WiiUEffect reverb = hokuto::song::WiiUEffectOf(
        cafe::EffectType::kReverbHi, hokuto::song::WiiUStartingParameters(cafe::EffectType::kReverbHi));
    const auto on_b = ModelOf(archive, sequence, 50, {{{}, {reverb}, {}}});
    const auto i3dl2 = ModelOf(archive, sequence, 50, {{{Reverb()}, {}, {}}});
    const std::vector<int16_t> played_on_b = RunToEnd(*on_b, dry.size());
    const std::vector<int16_t> played_i3dl2 = RunToEnd(*i3dl2, dry.size());

    SEQLIB_CHECK(dry_end > 100 * kWiiUFrame);
    SEQLIB_CHECK_EQ(wrong, 0);
    SEQLIB_CHECK(played_on_b.size() > dry_end && std::equal(played_on_b.begin(), played_on_b.end(), dry.begin()));
    SEQLIB_CHECK(played_i3dl2.size() > dry_end && played_i3dl2.size() < played_on_b.size() &&
                 std::equal(played_i3dl2.begin(), played_i3dl2.end(), dry.begin()));
    SEQLIB_CHECK(!HasSound(std::span<const int16_t>(dry).subspan(played_i3dl2.size())));
}

// The longest run of frames of `samples` from frame `from` up to sample `to` that hold no sound (nothing louder than
// ±8), in frames of `frame` samples.
std::size_t LongestQuiet(std::span<const int16_t> samples, std::size_t frame, std::size_t from, std::size_t to)
{
    std::size_t longest = 0;
    std::size_t quiet = 0;
    for (std::size_t at = from; (at + 1) * frame <= to; at++)
    {
        quiet = xsflib::rip::Audible(samples.subspan(at * frame, frame)) ? 0 : quiet + 1;
        longest = std::max(longest, quiet);
    }

    return longest;
}

// The first frame of `samples` from frame `from` whose samples are all 0, in frames of `frame` samples, or the number
// of whole frames if none is.
std::size_t FirstSilentFrame(std::span<const int16_t> samples, std::size_t frame, std::size_t from)
{
    const auto sound = [](int16_t sample)
    {
        return sample != 0;
    };

    std::size_t at = from;
    while ((at + 1) * frame <= samples.size() && std::ranges::any_of(samples.subspan(at * frame, frame), sound))
    {
        at++;
    }

    return at;
}

// A delay of 1.5 s on the Wii U's bus A leaves gaps of more than a second between a note's echoes. Like ntdlib's model
// run directly, the output plays on through the gaps. The effect may ring for a second and the delay's time after the
// last echo louder than ±8: 834 frames of 3 ms. The output then ends. Quieter echoes still give samples of ±8 at most
// there, and a delay that feeds back can give them for ever.
void TestWiiUEffectsRing()
{
    constexpr std::size_t kWiiUFrame = cafe::ArchiveModel::kFrameSamples * 2;
    constexpr std::size_t kFrames = 8000;
    const std::shared_ptr<const sar::SoundArchive> archive = OpenWiiUSendArchive();
    const auto sequence = std::make_shared<const Sequence>(ReadSequence(*archive, kNoteSound));
    std::vector<float> parameters = hokuto::song::WiiUStartingParameters(cafe::EffectType::kDelay);
    parameters[0] = 1500.0f;
    parameters[3] = 1500.0f;
    const hokuto::song::WiiUEffect delay = hokuto::song::WiiUEffectOf(cafe::EffectType::kDelay, parameters);
    const std::vector<int16_t> dry = RunWiiUModel(*archive, kNoteSound, kFrames);
    const std::vector<int16_t> direct =
        RunWiiUModel(*archive, kNoteSound, kFrames, {{cafe::kEffectDeviceTv, 0, delay.type, delay.words, {}}});

    const auto model = ModelOf(archive, sequence, 50, {{{delay}, {}, {}}});
    const std::vector<int16_t> played = RunToEnd(*model, direct.size());

    const std::size_t end = SoundEnd(direct);
    SEQLIB_REQUIRE(end > 0);
    const std::size_t gap = LongestQuiet(direct, kWiiUFrame, SoundEnd(dry) / kWiiUFrame + 1, end);
    const std::size_t after_last = played.size() > end ? (played.size() - end) / kWiiUFrame : 0;
    const std::size_t silence = FirstSilentFrame(direct, kWiiUFrame, (end - 1) / kWiiUFrame + 834);
    SEQLIB_CHECK(gap > 334);
    SEQLIB_CHECK(played.size() < direct.size() && std::equal(played.begin(), played.end(), direct.begin()));
    SEQLIB_CHECK(after_last >= 834);
    SEQLIB_CHECK_EQ(played.size(), ((end - 1) / kWiiUFrame + 835) * kWiiUFrame);
    SEQLIB_CHECK(silence > (end - 1) / kWiiUFrame + 834);
}

// In enhanced mixing, a Wii U archive plays on ntdlib's model of AX in its enhanced mode. The model's frames match the
// model run directly, in floating point, after a seek back to the snapshot at frame 50. So does the player's output
// after its first frame, from the model's first frame that runs a tick. The player's first frame fades in. The output
// differs from strict mixing.
void TestWiiUEnhancedMixing()
{
    constexpr std::size_t kWiiUFrame = cafe::ArchiveModel::kFrameSamples * 2;
    const std::shared_ptr<const sar::SoundArchive> archive = OpenWiiUArchive();
    const auto sequence = std::make_shared<const Sequence>(ReadSequence(*archive, kNoteSound));
    cafe::EngineOptions options;
    options.enhanced = true;
    cafe::ArchiveModel direct(*archive, options);
    const cafe::SequenceSoundPlayer* sound = direct.engine->StartSequence(kNoteSound);
    SEQLIB_REQUIRE(sound != nullptr);
    std::vector<float> expected;
    std::vector<int16_t> rounded;
    std::size_t first = 0;
    for (std::size_t frame = 0; frame < 600; frame++)
    {
        direct.engine->RunFrame();
        const std::span<const float> output = direct.system.OutputFloat();
        expected.insert(expected.end(), output.begin(), output.end());
        rounded.insert(rounded.end(), direct.system.Output().begin(), direct.system.Output().end());
        first += sound->TickCount() == 0 ? 1 : 0;
    }
    const std::vector<int16_t> strict = RunWiiUModel(*archive, kNoteSound, 600);

    const std::shared_ptr<SequenceModel> model = ModelOf(archive, sequence, 50, {}, Mixing::kEnhanced);
    const bool sought = model->SeekFrame(70);
    const std::span<const float> sought_output = model->FloatOutput();
    const std::vector<float> after_seek(sought_output.begin(), sought_output.end());
    const auto clock = std::make_shared<PlaybackClock>(model->FrameSamples());
    SequencePlayer player(model, 0, clock);
    std::vector<float> heard(expected.size());
    const std::size_t rendered = player.Render(heard.data(), heard.size() / 2);
    heard.resize(rendered * 2);

    SEQLIB_REQUIRE(expected.size() == rounded.size() && strict.size() == rounded.size());
    SEQLIB_CHECK(sought && after_seek.size() == kWiiUFrame &&
                 std::equal(after_seek.begin(), after_seek.end(), expected.begin() + 70 * kWiiUFrame));
    SEQLIB_REQUIRE(rendered < expected.size() / 2 && heard.size() > 2 * kWiiUFrame);
    SEQLIB_CHECK(std::equal(heard.begin() + kWiiUFrame, heard.end() - kWiiUFrame,
                            expected.begin() + static_cast<std::ptrdiff_t>((first + 1) * kWiiUFrame)));
    SEQLIB_CHECK(rounded != strict && HasSound(std::span<const int16_t>(rounded)));
}

// A reverb's modes become their whole numbers, and the other parameters their floats' bits. A negative mode becomes 0.
// Parameters past the structure's words are left out.
void TestWiiUEffectWords()
{
    const std::vector<float> kParameters = {0.5f, 2.0f, 0.25f, 0.75f, 1.0f, 3.0f, 0.5f, -2.0f, 0.0f, 1.0f, 9.0f};

    const hokuto::song::WiiUEffect reverb = hokuto::song::WiiUEffectOf(cafe::EffectType::kReverbStd, kParameters);
    const hokuto::song::WiiUEffect delay = hokuto::song::WiiUEffectOf(cafe::EffectType::kDelay, kParameters);

    SEQLIB_REQUIRE(reverb.words.size() == 10 && delay.words.size() == 5);
    SEQLIB_CHECK(reverb.type == cafe::EffectType::kReverbStd && delay.type == cafe::EffectType::kDelay);
    SEQLIB_CHECK(reverb.words[0] == 0x3F000000 && reverb.words[4] == 0x3F800000 && reverb.words[9] == 0x3F800000);
    SEQLIB_CHECK(reverb.words[5] == 3 && reverb.words[6] == 0x3F000000 && reverb.words[7] == 0);
    SEQLIB_CHECK(delay.words[2] == 0x3E800000 && delay.words[3] == 0x3F400000 && delay.words[4] == 0x3F800000);
    for (const cafe::EffectType type : {cafe::EffectType::kReverbHi, cafe::EffectType::kReverbStd,
                                        cafe::EffectType::kChorus, cafe::EffectType::kDelay})
    {
        SEQLIB_CHECK_EQ(hokuto::song::WiiUStartingParameters(type).size(), cafe::EffectParamWords(type));
    }
}

// A note sent to aux bus A (0xD9 with 127) as the Wii holds it.
const std::vector<uint8_t> kWiiSendBytecode = {0xD9, 127, 0x3C, 0x64, 0x30, 0xFF};

// The same note after a wait of 12 ticks (0x80 with 12). The effects are on the buses by then.
const std::vector<uint8_t> kWiiLateSendBytecode = {0xD9, 127, 0x80, 12, 0x3C, 0x64, 0x30, 0xFF};

// The setup of `effect` on aux bus A in the form that ntdlib's engine takes.
ntdlib::rvl::snd::EffectSetup SetupOnA(const hokuto::song::WiiEffect& effect)
{
    ntdlib::rvl::snd::EffectSetup setup;
    setup.type = effect.type;
    std::copy(effect.words.begin(), effect.words.end(), setup.params.begin());

    return setup;
}

// Each effect of the Wii on aux bus A plays as ntdlib's model of NW4R SND and AX plays it run directly, and changes the
// output. A seek back to a snapshot restores the effect's state. At half volume, bus A returns as AX returns it at
// 0x4000. An effect of the Wii U changes nothing on the Wii.
void TestWiiEffects()
{
    constexpr std::size_t kWiiFrame = 96 * 2;
    constexpr std::size_t kFrames = 700;
    const std::shared_ptr<const ntdlib::rsar::SoundArchive> archive = OpenWiiArchive(kWiiSendBytecode);
    const auto sequence = std::make_shared<const Sequence>(ReadSequence(*archive, 0));
    const std::vector<int16_t> dry = RunWiiModel(*archive, kFrames);

    std::size_t wrong = 0;
    for (const ntdlib::rvl::snd::EffectType type :
         {ntdlib::rvl::snd::EffectType::kReverbHi, ntdlib::rvl::snd::EffectType::kReverbHi2008,
          ntdlib::rvl::snd::EffectType::kChorus, ntdlib::rvl::snd::EffectType::kDelay,
          ntdlib::rvl::snd::EffectType::kReverbStd, ntdlib::rvl::snd::EffectType::kReverbStd2008})
    {
        const hokuto::song::WiiEffect effect =
            hokuto::song::WiiEffectOf(type, hokuto::song::WiiStartingParameters(type));
        const std::vector<int16_t> direct = RunWiiModel(*archive, kFrames, {SetupOnA(effect)});
        const auto model = std::make_shared<SequenceModel>(
            archive, sequence,
            hokuto::song::ModelOptions{.effects = AuxEffects{{{effect}, {}, {}}}, .snapshot_interval = 50});
        const std::vector<int16_t> played = RunToEnd(*model, direct.size());
        const bool sought = model->SeekFrame(70);
        const std::span<const int16_t> after_seek = model->Output();

        const bool restores = sought && after_seek.size() == kWiiFrame &&
                              std::equal(after_seek.begin(), after_seek.end(), direct.begin() + 70 * kWiiFrame);
        wrong += played == direct && direct != dry && restores ? 0 : 1;
    }

    const hokuto::song::WiiEffect reverb =
        hokuto::song::WiiEffectOf(ntdlib::rvl::snd::EffectType::kReverbHi2008,
                                  hokuto::song::WiiStartingParameters(ntdlib::rvl::snd::EffectType::kReverbHi2008));
    const std::vector<int16_t> full = RunWiiModel(*archive, kFrames, {SetupOnA(reverb)});
    const std::vector<int16_t> half = RunWiiModel(*archive, kFrames, {SetupOnA(reverb)}, 0x4000);
    const auto halved = std::make_shared<SequenceModel>(
        archive, sequence,
        hokuto::song::ModelOptions{.effects = AuxEffects{{{reverb}, {}, {}}}, .snapshot_interval = 50});
    halved->SetBusVolumes({0.5f, 1.0f, 1.0f});
    const std::vector<int16_t> played_half = RunToEnd(*halved, half.size());
    const hokuto::song::WiiUEffect wii_u = hokuto::song::WiiUEffectOf(
        cafe::EffectType::kReverbHi, hokuto::song::WiiUStartingParameters(cafe::EffectType::kReverbHi));
    const auto other = std::make_shared<SequenceModel>(
        archive, sequence,
        hokuto::song::ModelOptions{.effects = AuxEffects{{{wii_u}, {}, {}}}, .snapshot_interval = 50});
    const std::vector<int16_t> played_other = RunToEnd(*other, dry.size());

    SEQLIB_REQUIRE(dry.size() == kFrames * kWiiFrame);
    SEQLIB_CHECK(HasSound(std::span<const int16_t>(dry)));
    SEQLIB_CHECK_EQ(wrong, 0);
    SEQLIB_CHECK(played_half == half && half != full && half != dry);
    SEQLIB_CHECK(played_other == dry);
}

// A short note's echoes on a delay on bus A go on after the dry sound has ended, and after the model without the delay
// ends. Quiet gaps lie between them. The note waits until the bus has the delay: a game appends its effects in the
// first frame after the sound starts. The output plays on through the gaps as the model run directly plays them. It
// ends a second and the delay's time after the last echo louder than ±8. With a delay of 160 ms, that's 388 frames of 3
// ms. A delay of 1.5 s leaves gaps of more than a second between its echoes, and the output ends 834 frames after the
// last.
void TestWiiEffectsRing()
{
    constexpr std::size_t kWiiFrame = 96 * 2;
    constexpr std::size_t kFrames = 8000;
    const std::shared_ptr<const ntdlib::rsar::SoundArchive> archive = OpenWiiArchive(kWiiLateSendBytecode, false);
    const auto sequence = std::make_shared<const Sequence>(ReadSequence(*archive, 0));
    const auto dry =
        std::make_shared<SequenceModel>(archive, sequence, hokuto::song::ModelOptions{.snapshot_interval = 50});
    const std::vector<int16_t> played_dry = RunToEnd(*dry, kFrames * kWiiFrame);
    std::vector<float> parameters = hokuto::song::WiiStartingParameters(ntdlib::rvl::snd::EffectType::kDelay);

    std::vector<std::size_t> gaps;
    std::vector<std::size_t> after_last;
    std::size_t wrong = 0;
    for (const float delay_ms : {160.0f, 1500.0f})
    {
        parameters[0] = delay_ms;
        const hokuto::song::WiiEffect delay =
            hokuto::song::WiiEffectOf(ntdlib::rvl::snd::EffectType::kDelay, parameters);
        const AuxEffects effects{{{delay}, {}, {}}};
        const std::vector<int16_t> direct = RunWiiModel(*archive, kFrames, {SetupOnA(delay)});
        const auto model = std::make_shared<SequenceModel>(
            archive, sequence, hokuto::song::ModelOptions{.effects = effects, .snapshot_interval = 50});
        const std::vector<int16_t> played = RunToEnd(*model, direct.size());

        const std::size_t end = SoundEnd(direct);
        gaps.push_back(LongestQuiet(direct, kWiiFrame, played_dry.size() / kWiiFrame, end));
        after_last.push_back(played.size() > end ? (played.size() - end) / kWiiFrame : 0);
        wrong += end > played_dry.size() + 100 * kWiiFrame && end + 900 * kWiiFrame < direct.size() &&
                         std::equal(played.begin(), played.end(), direct.begin())
                     ? 0
                     : 1;
    }

    SEQLIB_CHECK_EQ(wrong, 0);
    SEQLIB_REQUIRE(gaps.size() == 2);
    SEQLIB_CHECK(gaps[0] > 10 && gaps[0] < 334 && gaps[1] > 334);
    SEQLIB_CHECK(after_last[0] >= 386 && after_last[0] <= 390);
    SEQLIB_CHECK(after_last[1] >= 832 && after_last[1] <= 836);
}

// A reverb of 2008's modes become their whole numbers, and the other parameters their floats' bits. A negative mode
// becomes 0. Parameters past the structure's words are left out. The starting values fill each structure.
void TestWiiEffectWords()
{
    const std::vector<float> kParameters = {0.5f, 2.0f, 0.25f, 0.75f, 1.0f, 3.0f, 0.5f, -2.0f, 0.0f, 1.0f, 9.0f};

    const hokuto::song::WiiEffect reverb =
        hokuto::song::WiiEffectOf(ntdlib::rvl::snd::EffectType::kReverbStd2008, kParameters);
    const hokuto::song::WiiEffect older =
        hokuto::song::WiiEffectOf(ntdlib::rvl::snd::EffectType::kReverbStd, kParameters);
    const hokuto::song::WiiEffect delay = hokuto::song::WiiEffectOf(ntdlib::rvl::snd::EffectType::kDelay, kParameters);

    SEQLIB_REQUIRE(reverb.words.size() == 10 && older.words.size() == 5 && delay.words.size() == 3);
    SEQLIB_CHECK(reverb.words[0] == 0x3F000000 && reverb.words[4] == 0x3F800000 && reverb.words[9] == 0x3F800000);
    SEQLIB_CHECK(reverb.words[5] == 3 && reverb.words[6] == 0x3F000000 && reverb.words[7] == 0);
    SEQLIB_CHECK(older.words[4] == 0x3F800000 && delay.words[2] == 0x3E800000);
    for (const ntdlib::rvl::snd::EffectType type :
         {ntdlib::rvl::snd::EffectType::kReverbHi, ntdlib::rvl::snd::EffectType::kReverbHi2008,
          ntdlib::rvl::snd::EffectType::kChorus, ntdlib::rvl::snd::EffectType::kDelay,
          ntdlib::rvl::snd::EffectType::kReverbStd, ntdlib::rvl::snd::EffectType::kReverbStd2008})
    {
        SEQLIB_CHECK_EQ(hokuto::song::WiiStartingParameters(type).size(), ntdlib::rvl::snd::EffectParamWords(type));
    }
}

// A stream of the 3DS of 0.5 s at 32,728 Hz, in DSP-ADPCM, without a loop. Both channels hold the same sawtooth.
std::vector<uint8_t> CtrStream()
{
    std::vector<std::vector<int16_t>> channels(2, std::vector<int16_t>(16364));
    for (std::vector<int16_t>& channel : channels)
    {
        for (std::size_t i = 0; i < channel.size(); i++)
        {
            channel[i] = static_cast<int16_t>(static_cast<int>(i % 80) * 100 - 4000);
        }
    }

    return sar::MakeStream(sar::Flavor::kCtr, 0x02030100, sar::WaveEncoding::kDspAdpcm, 32728, channels, std::nullopt)
        .Write();
}

// A 3DS archive with sequence sound 0: key 60 at velocity 127 for 96 ticks on program 0, a wait of 96 ticks and the
// end. Program 0 plays a constant wave of 2 s. Wave sound 1 plays the wave once. Stream sound 2 plays CtrStream on a
// track of both channels.
std::shared_ptr<const sar::SoundArchive> CtrArchive()
{
    constexpr sar::Flavor kCtr = sar::Flavor::kCtr;
    sar::StreamSoundInfo stream;
    stream.valid_tracks = 1;
    stream.channel_count = 2;
    stream.tracks = {{.channels = {0, 1}}};
    sar_files::WaveSoundFileSpec wave_sounds;
    wave_sounds.waves = {{sar::kWaveArchiveItem << 24, 0}};
    wave_sounds.sounds = {{.notes = {{.wave = 0}}}};
    sar_files::ArchiveSpec spec;
    spec.sounds = {{.name = "SEQ_NOTE", .file = 0, .banks = {0}},
                   {.name = "WSD_NOTE", .kind = sar_files::SoundSpec::kWaveSound, .file = 3},
                   {.name = "STM_SAW", .kind = sar_files::SoundSpec::kStream, .file = 4, .stream = stream}};
    spec.banks = {{"BANK", 1}};
    spec.wave_archives = {{"WARC", 2}};
    spec.players = {{"PLAYER", 1}};
    spec.files = {{sar_files::Sequence(kCtr, {0x3C, 0x7F, 0x60, 0x80, 0x60, 0xFF}, {{"SEQ_NOTE", 0}})},
                  {sar_files::Bank(kCtr, sar_files::OneRegionBank())},
                  {sar_files::WaveArchive(kCtr, {sar_files::Wave(kCtr, sar_files::ConstantWave(false, 64000))})},
                  {sar_files::WaveSoundFile(kCtr, wave_sounds)},
                  {.external = "stream/STM_SAW.bcstm"}};

    return LoadArchive(sar_files::Archive(kCtr, spec), "test");
}

// The output of sound `sound` of the 3DS's `archive` on ntdlib's model run directly, without a preview, for `frames`
// frames at most. A stream sound plays `stream`. The test compares a preview's output with it.
std::vector<float> CtrSoundOnModel(const sar::SoundArchive& archive, uint32_t sound, std::span<const uint8_t> stream,
                                   std::size_t frames)
{
    hokuto::song::CtrModel model;
    std::vector<float> samples;
    const bool wave_sound = archive.Sounds()[sound].type == sar::SoundType::kWaveSound;
    if (!model.Start(archive) || (wave_sound ? model.Engine().StartWaveSound(sound) == nullptr
                                             : model.Engine().StartStreamSound(sound, stream) == nullptr))
    {
        return samples;
    }

    std::vector<float> frame(2 * hokuto::song::CtrModel::kFrameSamples);
    for (std::size_t i = 0; i < frames && model.IsBusy() && model.RunFrame(); i++)
    {
        hokuto::song::CopyOutput(model, frame);
        samples.insert(samples.end(), frame.begin(), frame.end());
    }

    return samples;
}

// A sequence of the 3DS plays on ntdlib's model of the DSP's audio firmware, and the player's output sounds. A note of
// a 3DS bank sounds too. A wave sound and a stream sound play as ntdlib's model plays them run directly, sample for
// sample, and end with their waves. A sequence sound refuses a sound's preview. ctr_test compares the 3DS's playback
// with ntdlib's archive mode.
void TestCtrPlayback()
{
    const std::shared_ptr<const sar::SoundArchive> archive = CtrArchive();
    const auto sounds = [](std::span<const float> samples)
    {
        return std::any_of(samples.begin(), samples.end(), [](float sample) { return sample != 0.0f; });
    };

    // The sequence plays on the model.
    const auto sequence = std::make_shared<const Sequence>(ReadSequence(*archive, 0));
    SEQLIB_CHECK_EQ(sequence->tracks.size(), 1);
    const auto model = std::make_shared<SequenceModel>(archive, sequence);
    SEQLIB_CHECK(model->FrameSamples() == 160 && model->SampleRate() == 32728);
    const auto clock = std::make_shared<PlaybackClock>(model->FrameSamples());
    SequencePlayer player(model, 0, clock);
    std::vector<float> out(2 * 160 * 100);

    SEQLIB_CHECK_EQ(player.Render(out.data(), 160 * 100), 160 * 100);

    SEQLIB_CHECK(sounds(out) && !clock->Ended());

    // A note sounds.
    NotePreview note(archive, 0, 0);
    SEQLIB_CHECK(note.SampleRate() == 32728 && note.LongestFrames() == 2045 && note.HoldFrames() == 204);
    std::fill(out.begin(), out.end(), 0.0f);

    SEQLIB_CHECK_EQ(note.Render(out.data(), 160 * 100), 160 * 100);

    SEQLIB_CHECK(sounds(out));

    // The wave sound and the stream sound play as the model plays them.
    const std::vector<uint8_t> stream = CtrStream();
    SoundPreview wave_sound(archive, 1);
    SoundPreview stream_sound(archive, 2, stream);
    SEQLIB_CHECK(wave_sound.SampleRate() == 32728 && stream_sound.SampleRate() == 32728);

    const std::vector<float> wave_samples = RunPreview(wave_sound, 1000, 160);
    const std::vector<float> stream_samples = RunPreview(stream_sound, 1000, 160);

    SEQLIB_CHECK(sounds(wave_samples) && wave_samples == CtrSoundOnModel(*archive, 1, {}, 1000));
    SEQLIB_CHECK(sounds(stream_samples) && stream_samples == CtrSoundOnModel(*archive, 2, stream, 1000));

    // The wave lasts 2 s at 32,000 Hz, and the stream 0.5 s. Both end within a few frames of their samples.
    const std::size_t wave_frames = wave_samples.size() / (2 * 160);
    const std::size_t stream_frames = stream_samples.size() / (2 * 160);
    SEQLIB_CHECK(wave_frames > 64000 * 32728 / 32000 / 160 && wave_frames < 64000 * 32728 / 32000 / 160 + 16);
    SEQLIB_CHECK(stream_frames > 16364 / 160 && stream_frames < 16364 / 160 + 16);

    SEQLIB_CHECK(ErrorOf<std::runtime_error>([&] { const SoundPreview sound(archive, 0); }) ==
                 "SEQ_NOTE is neither a wave sound nor a stream sound.");
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("console_playback",
                                    []
                                    {
                                        TestWiiUPlays();
                                        TestWiiPlays();
                                        TestWiiUEffects();
                                        TestWiiUEffectsRing();
                                        TestWiiUEnhancedMixing();
                                        TestWiiUEffectWords();
                                        TestWiiEffects();
                                        TestWiiEffectsRing();
                                        TestWiiEffectWords();
                                        TestCtrPlayback();
                                    });
}
