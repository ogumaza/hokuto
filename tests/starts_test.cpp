// SPDX-License-Identifier: MIT

// Tests a sound's start (format/starts.h) in playback, in the MIDI export and in the measure of lengths. The 3DS's
// model mutes or silences the quieted tracks through nw::snd's handle, and the Wii U's model sets their volume to 0.
// The test archives play one note on track 0. Quieting track 0 in either way leaves the output and the track's meter
// silent. Quieting a track that plays nothing changes no sample. So does a volume of 0 on track 0, and the start's
// volume scales the output. The read keeps the start, and the MIDI export plays the quieted tracks at a volume of 0. So
// does the export of a sound on another console's engine, with a SoundFont or with SFZ files. A sound's length is
// measured with its start, and a slower tempo makes it longer. The models of the Wii U and the Switch take a start's
// variables before the player starts and its changes after, in playback and in the measure. The models also take the
// sends, the pitch, the generator's state and, on the 3DS, the banks. A start's low-pass plays as a track's low-pass
// command of the same value, on the models and in the MIDI export. The 3DS's volume is pinned sample by sample, and
// the measure hears it. The performer's options take the rest of the start, the variables of every track before those
// of a single track. A file of version 1 keeps the STRT chunk of a later version on opening.

#include "song/starts.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "xsflib/files.h"

#include "edit/sound_exports.h"
#include "format/archive.h"
#include "played_fixtures.h"
#include "song/lengths.h"
#include "song/sequence_model.h"
#include "song_runs.h"

namespace
{

using hokuto::format::SoundStart;

// The frames that a run of a sound that may wait for ever plays at most.
constexpr std::size_t kMostFrames = 200;

// A run of a sound on its model with a start: the output from the first frame to the end, and whether track 0's
// meter or aux bus A's meter ever left 0.
struct QuietRun
{
    std::vector<int16_t> output;
    bool metered = false;
    bool sent = false;
};

// The QuietRun of sound `sound` of `archive`, with `start`, for `frames` frames at most, on a model with `options`. The
// start's variables become the sequence's starting values and changes, as playback gives them.
QuietRun RunQuieted(const std::shared_ptr<const sar::SoundArchive>& archive, uint32_t sound, const SoundStart& start,
                    std::size_t frames = SIZE_MAX, const hokuto::song::ModelOptions& options = {})
{
    const auto sequence = std::make_shared<const Sequence>(
        ReadSequence(*archive, sound, hokuto::song::StartValuesOf(start), hokuto::song::ChangesOf(start), start));
    SequenceModel model(archive, sequence, options);
    QuietRun run;

    std::size_t frame = 0;
    for (bool more = model.SeekFrame(0); more && frame < frames; more = model.RunFrame(), ++frame)
    {
        const std::span<const int16_t> output = model.Output();
        run.output.insert(run.output.end(), output.begin(), output.end());
        const std::array<int32_t, 2> peaks = model.TrackPeaks(0);
        run.metered = run.metered || peaks[0] != 0 || peaks[1] != 0;
        const std::array<int32_t, 2> sent = model.BusPeaks(0);
        run.sent = run.sent || sent[0] != 0 || sent[1] != 0;
    }

    return run;
}

// Whether every sample of `samples` is 0.
bool Silent(std::span<const int16_t> samples)
{
    return std::ranges::all_of(samples, [](int16_t sample) { return sample == 0; });
}

// The sum of the magnitudes of `samples`.
int64_t Level(std::span<const int16_t> samples)
{
    int64_t level = 0;
    for (const int16_t sample : samples)
    {
        level += sample < 0 ? -int64_t{sample} : int64_t{sample};
    }

    return level;
}

// The times that the left channel of `samples`, interleaved stereo, changes its sign.
std::size_t SignChanges(std::span<const int16_t> samples)
{
    std::size_t changes = 0;
    for (std::size_t i = 2; i < samples.size(); i += 2)
    {
        if ((samples[i] < 0) != (samples[i - 2] < 0))
        {
            changes++;
        }
    }

    return changes;
}

// The index of the first sample of `samples` that isn't 0, or the number of samples.
std::size_t FirstSound(std::span<const int16_t> samples)
{
    return static_cast<std::size_t>(std::ranges::find_if(samples, [](int16_t sample) { return sample != 0; }) -
                                    samples.begin());
}

// A sequence that waits until variable `variable` is 1, then plays key 60 for 24 ticks and ends. It compares the
// variable with 1 at byte 5. If they're equal, it jumps to the note at byte 21. Otherwise it waits a tick and jumps
// back to byte 5. In the Switch's byte order or, with `big_endian`, the Wii U's.
std::vector<uint8_t> WaitForBytecode(uint8_t variable, bool big_endian)
{
    if (big_endian)
    {
        return {0xB0, 48, 0xE1, 0x00, 120,  0xF0, 0x90, variable, 0x00, 0x01, 0xA2, 0x89, 0x00,
                0x00, 21, 0x80, 1,    0x89, 0x00, 0x00, 5,        60,   100,  24,   0xFF};
    }

    return {0xB0, 48,   0xE1, 120, 0x00, 0xF0, 0x90, variable, 0x01, 0x00, 0xA2, 0x89, 21,
            0x00, 0x00, 0x80, 1,   0x89, 5,    0x00, 0x00,     60,   100,  24,   0xFF};
}

// A sequence that waits from 1 to 96 ticks at random (the random prefix, 0xA0, before a wait), then plays key 60 for
// 24 ticks and ends. In the Wii U's byte order.
const std::vector<uint8_t> kRandomWaitBytecode = {0xB0, 48,   0xE1, 0x00, 120, 0xA0, 0x80, 0x00,
                                                  0x01, 0x00, 0x60, 60,   100, 24,   0xFF};

// The test archive of the Switch or, with `wii_u`, of the Wii U, with `bytecode` as the note's sequence.
std::shared_ptr<const sar::SoundArchive> OpenNoteArchive(const std::vector<uint8_t>& bytecode, bool wii_u)
{
    sar_files::ArchiveSpec spec = TestArchiveSpec(wii_u);
    spec.files[1].bytes = sar_files::Sequence(wii_u, bytecode, {{"Note", 0}});
    const TemporaryFile file(sar_files::Archive(wii_u, spec));
    return OpenArchive(file.Path());
}

// Muted and silenced tracks, the tracks' volumes and the volume on `archive`, whose sound `sound` plays a note on
// track 0.
void CheckQuiets(const std::shared_ptr<const sar::SoundArchive>& archive, uint32_t sound)
{
    const QuietRun all = RunQuieted(archive, sound, {});
    const QuietRun muted = RunQuieted(archive, sound, SoundStart{.muted = 0x0001});
    const QuietRun silenced = RunQuieted(archive, sound, {.silenced = 0x0001});
    const QuietRun others = RunQuieted(archive, sound, {.muted = 0x0002, .silenced = 0xFFF0});
    const QuietRun turned_down = RunQuieted(archive, sound, {.track_volumes = {{0, 0.0f}}});
    const QuietRun other_volume = RunQuieted(archive, sound, {.track_volumes = {{1, 0.0f}}});
    const QuietRun half = RunQuieted(archive, sound, {.volume = 0.5f});

    SEQLIB_CHECK(!Silent(all.output) && all.metered);
    SEQLIB_CHECK(!muted.output.empty() && Silent(muted.output) && !muted.metered);
    SEQLIB_CHECK(!silenced.output.empty() && Silent(silenced.output) && !silenced.metered);
    SEQLIB_CHECK(others.output == all.output && other_volume.output == all.output);
    SEQLIB_CHECK(!turned_down.output.empty() && Silent(turned_down.output));
    const int64_t level = Level(all.output);
    const int64_t half_level = Level(half.output);
    SEQLIB_CHECK(half_level > level * 4 / 10 && half_level < level * 6 / 10);
}

// The 3DS mutes through SetTrackMute and silences through SetTrackSilence.
void TestCtrQuiets()
{
    CheckQuiets(LoadArchive(hokuto::testing::CtrNoteArchiveBytes(), "test"), 0);
}

// The Wii U sets the volume of both kinds of quieted track to 0.
void TestWiiUQuiets()
{
    CheckQuiets(OpenWiiUArchive(), kNoteSound);
}

// The read keeps the start. The MIDI export keeps the quieted track's note, and writes another file for it. A track's
// volume and the tempo ratio change the file too.
void TestMidiQuiets()
{
    const std::shared_ptr<const sar::SoundArchive> archive = OpenTestArchive();
    const SoundStart start{.silenced = 0x0001};

    const Sequence read = ReadSequence(*archive, kNoteSound, {}, {}, start);
    const ntdlib::sequence_export::Conversion all = ConvertToMidi(*archive, kNoteSound, {}, {}, "test");
    const ntdlib::sequence_export::Conversion quieted = ConvertToMidi(*archive, kNoteSound, {}, {}, "test", start);
    const auto midi = [&archive](const SoundStart& other)
    {
        return ConvertToMidi(*archive, kNoteSound, {}, {}, "test", other).midi;
    };

    SEQLIB_CHECK(read.start == start);
    SEQLIB_CHECK(ReadSequence(*archive, kNoteSound).start == SoundStart{});
    SEQLIB_CHECK(!all.midi.empty() && quieted.midi != all.midi);
    SEQLIB_CHECK(midi({.silenced = 0x0002}) == all.midi);
    SEQLIB_CHECK((midi({.track_volumes = {{0, 0.5f}}}) != all.midi && midi({.tempo_ratio = 0.5f}) != all.midi));
}

// A sound on another console's engine goes out as the conversion for that console. The conversion for the Wii U quiets
// the tracks that the sound's start quiets in the MIDI file beside a SoundFont and beside SFZ files.
void TestOtherEngineQuiets()
{
    namespace format = hokuto::format;
    const format::File file =
        format::WithEngine(format::ImportSwitchArchive(TestArchiveBytes()), kNoteSound, format::Engine::kWiiU);
    const SoundStart start{.sound = kNoteSound, .muted = 0x0001};
    const hokuto::song::Played played(LoadArchive(format::ExportArchive(file), "test"));
    const hokuto::edit::BatchSource all = {.played = played, .file = std::make_shared<const format::File>(file)};
    const hokuto::edit::BatchSource quieted = {
        .played = played,
        .file = std::make_shared<const format::File>(format::WithStart(file, start)),
        .starts = {start}};
    const TemporaryFolder folder;
    const auto midi = [&folder](const hokuto::edit::BatchSource& source, bool sfz, const std::string& name)
    {
        const std::filesystem::path path = folder.Path() / (name + ".mid");
        hokuto::edit::WriteBatchMidi(source, kNoteSound, path, sfz);
        std::vector<uint8_t> bytes;
        xsflib::ReadFile(path, bytes);
        return bytes;
    };

    const std::vector<uint8_t> all_sf2 = midi(all, false, "all_sf2");
    const std::vector<uint8_t> quieted_sf2 = midi(quieted, false, "quieted_sf2");
    const std::vector<uint8_t> all_sfz = midi(all, true, "all_sfz");
    const std::vector<uint8_t> quieted_sfz = midi(quieted, true, "quieted_sfz");

    SEQLIB_CHECK(!all_sf2.empty() && quieted_sf2 != all_sf2);
    SEQLIB_CHECK(!all_sfz.empty() && quieted_sfz != all_sfz);
}

// A file of version 1.2 that keeps an STRT chunk keeps its starts on opening. The archive's SHA-1 finds none in the
// lists of known games.
void TestOlderFileKeepsStarts()
{
    namespace format = hokuto::format;
    format::File file = format::ImportArchive(hokuto::testing::CtrNoteArchiveBytes(), sar::Flavor::kCtr);
    file.starts = {{.sound = 0, .muted = 0x0001, .tempo_ratio = 1.5f}};
    std::vector<uint8_t> older = format::Write(file);
    older[8] = 1;
    older[10] = 2;

    const format::File opened = format::WithOlderStarts(format::Read(older), older);

    SEQLIB_CHECK(opened.starts == file.starts);
}

// A sound's length is measured with its start. The test archives' note on track 0 sounds, and the sound comes out
// silent once the track is muted on the 3DS or silenced on the Wii U. At half the tempo, the sound lasts longer.
void TestLengthsQuiet()
{
    const std::shared_ptr<const sar::SoundArchive> ctr = LoadArchive(hokuto::testing::CtrNoteArchiveBytes(), "test");
    const std::shared_ptr<const sar::SoundArchive> wii_u = OpenWiiUArchive();

    const std::optional<hokuto::song::Length> ctr_all = hokuto::song::MeasureLength(*ctr, 0);
    const std::optional<hokuto::song::Length> ctr_muted =
        hokuto::song::MeasureLength(*ctr, 0, {}, hokuto::song::kFullVolumes, SoundStart{.muted = 0x0001});
    const std::optional<hokuto::song::Length> wii_u_all = hokuto::song::MeasureLength(*wii_u, kNoteSound);
    const std::optional<hokuto::song::Length> wii_u_silenced =
        hokuto::song::MeasureLength(*wii_u, kNoteSound, {}, hokuto::song::kFullVolumes, SoundStart{.silenced = 0x0001});

    const std::optional<hokuto::song::Length> ctr_slow =
        hokuto::song::MeasureLength(*ctr, 0, {}, hokuto::song::kFullVolumes, SoundStart{.tempo_ratio = 0.5f});
    const std::optional<hokuto::song::Length> wii_u_slow = hokuto::song::MeasureLength(
        *wii_u, kNoteSound, {}, hokuto::song::kFullVolumes, SoundStart{.tempo_ratio = 0.5f});

    SEQLIB_REQUIRE(ctr_all && ctr_muted && wii_u_all && wii_u_silenced && ctr_slow && wii_u_slow);
    SEQLIB_CHECK(!ctr_all->silent && ctr_muted->silent);
    SEQLIB_CHECK(!wii_u_all->silent && wii_u_silenced->silent);
    SEQLIB_CHECK(ctr_slow->length > ctr_all->length && wii_u_slow->length > wii_u_all->length);
}

// A variable that a start sets reaches the sound on the models of the Wii U and the Switch, in playback and in the
// measure: a track variable of every track (32) before the player starts, a player's variable (0), and both as changes
// after the start. The sound waits for the variable, and stays silent without it.
void TestNwVariables()
{
    for (const bool wii_u : {false, true})
    {
        for (const uint8_t variable : {uint8_t{0}, uint8_t{32}})
        {
            const std::shared_ptr<const sar::SoundArchive> archive =
                OpenNoteArchive(WaitForBytecode(variable, wii_u), wii_u);
            const SoundStart at_start{.variables = {{.variable = variable, .value = 1}}};
            const SoundStart later{.variables = {{.time = 100, .variable = variable, .value = 1}}};

            const QuietRun none = RunQuieted(archive, kNoteSound, {}, kMostFrames);
            const QuietRun set = RunQuieted(archive, kNoteSound, at_start, kMostFrames);
            const QuietRun changed = RunQuieted(archive, kNoteSound, later, kMostFrames);
            const std::optional<hokuto::song::Length> none_length = hokuto::song::MeasureLength(*archive, kNoteSound);
            const std::optional<hokuto::song::Length> set_length =
                hokuto::song::MeasureLength(*archive, kNoteSound, {}, hokuto::song::kFullVolumes, at_start);
            const std::optional<hokuto::song::Length> changed_length =
                hokuto::song::MeasureLength(*archive, kNoteSound, {}, hokuto::song::kFullVolumes, later);

            SEQLIB_CHECK(Silent(none.output) && !Silent(set.output) && !Silent(changed.output));
            SEQLIB_CHECK(FirstSound(changed.output) > FirstSound(set.output));
            SEQLIB_REQUIRE(none_length && set_length && changed_length);
            SEQLIB_CHECK(none_length->silent && !set_length->silent && !changed_length->silent);
            SEQLIB_CHECK(changed_length->length > set_length->length);
        }
    }
}

// The rest of a start on a console's model. On the Wii U, a send reaches aux bus A and its reverb, a pitch of 2 doubles
// the saw's frequency, and the generator's state sets a random wait: the same state gives the same output. On the 3DS,
// a start with empty bank slots plays nothing, and one with the archive's bank in slot 0 plays as the archive does.
void TestModelStarts()
{
    const std::shared_ptr<const sar::SoundArchive> wii_u = OpenWiiUArchive();
    const std::shared_ptr<const sar::SoundArchive> random = OpenNoteArchive(kRandomWaitBytecode, true);
    const std::shared_ptr<const sar::SoundArchive> ctr = LoadArchive(hokuto::testing::CtrNoteArchiveBytes(), "test");
    constexpr uint32_t kBank = sar::kBankItem << 24;
    const hokuto::format::StartBanks none{hokuto::format::kNoStartBank, hokuto::format::kNoStartBank,
                                          hokuto::format::kNoStartBank, hokuto::format::kNoStartBank};
    hokuto::format::StartBanks own = none;
    own[0] = kBank;
    const hokuto::song::WiiUEffect reverb = hokuto::song::WiiUEffectOf(
        cafe::EffectType::kReverbHi, hokuto::song::WiiUStartingParameters(cafe::EffectType::kReverbHi));
    const hokuto::song::ModelOptions wet{.effects = hokuto::song::AuxEffects{{{reverb}, {}, {}}}};

    const QuietRun plain = RunQuieted(wii_u, kNoteSound, {});
    const QuietRun dry = RunQuieted(wii_u, kNoteSound, {}, SIZE_MAX, wet);
    const QuietRun sent =
        RunQuieted(wii_u, kNoteSound, {.sends = hokuto::format::StartSends{1.0f, 0.0f, 0.0f}}, SIZE_MAX, wet);
    const QuietRun higher = RunQuieted(wii_u, kNoteSound, {.pitch = 2.0f});
    std::vector<std::size_t> onsets;
    for (const uint32_t seed : {1u, 2u, 3u, 4u})
    {
        onsets.push_back(FirstSound(RunQuieted(random, kNoteSound, {.seed = seed}).output));
    }
    const std::size_t again = FirstSound(RunQuieted(random, kNoteSound, {.seed = 1}).output);
    const QuietRun ctr_plain = RunQuieted(ctr, 0, {});
    const QuietRun no_banks = RunQuieted(ctr, 0, {.banks = none});
    const QuietRun own_bank = RunQuieted(ctr, 0, {.banks = own});

    SEQLIB_CHECK(!dry.sent && sent.sent);
    SEQLIB_CHECK(SignChanges(higher.output) > SignChanges(plain.output) * 3 / 2);
    SEQLIB_CHECK(again == onsets[0] && std::ranges::count(onsets, onsets[0]) < 4);
    SEQLIB_CHECK(!Silent(ctr_plain.output) && Silent(no_banks.output) && own_bank.output == ctr_plain.output);
}

// A start's low-pass adds to each track's low-pass. A start of -0.25 plays as command 0xD8 with argument 48 before the
// note, since (48 - 64) / 64 is -0.25: on the Wii U's model, on the 3DS's model and in the MIDI export. The filter
// changes the saw. The games' -0.4 lies between two arguments, and the MIDI export takes argument 38, whose step of the
// low-pass is the step of -0.4.
void TestLowPass()
{
    const SoundStart start{.low_pass = -0.25f};
    const auto commanded = [](std::vector<uint8_t> bytecode, uint8_t argument = 48)
    {
        constexpr std::size_t kAfterTempo = 5;
        bytecode.insert(bytecode.begin() + kAfterTempo, {0xD8, argument});
        return bytecode;
    };
    const std::shared_ptr<const sar::SoundArchive> wii_u = OpenWiiUArchive();
    const std::shared_ptr<const sar::SoundArchive> wii_u_command = OpenNoteArchive(commanded(kWiiUNoteBytecode), true);
    const std::shared_ptr<const sar::SoundArchive> ctr = LoadArchive(hokuto::testing::CtrNoteArchiveBytes(), "test");
    const std::shared_ptr<const sar::SoundArchive> ctr_command =
        LoadArchive(hokuto::testing::CtrNoteArchiveBytes(false, true), "test");
    const std::shared_ptr<const sar::SoundArchive> nx = OpenTestArchive();
    const std::shared_ptr<const sar::SoundArchive> nx_command = OpenNoteArchive(commanded(kNoteBytecode), false);
    const std::shared_ptr<const sar::SoundArchive> nx_step = OpenNoteArchive(commanded(kNoteBytecode, 38), false);

    const QuietRun plain = RunQuieted(wii_u, kNoteSound, {});
    const QuietRun started = RunQuieted(wii_u, kNoteSound, start);
    const QuietRun by_command = RunQuieted(wii_u_command, kNoteSound, {});
    const QuietRun ctr_started = RunQuieted(ctr, 0, start);
    const QuietRun ctr_by_command = RunQuieted(ctr_command, 0, {});
    const std::vector<uint8_t> midi = ConvertToMidi(*nx, kNoteSound, {}, {}, "test").midi;
    const std::vector<uint8_t> midi_started = ConvertToMidi(*nx, kNoteSound, {}, {}, "test", start).midi;
    const std::vector<uint8_t> midi_by_command = ConvertToMidi(*nx_command, kNoteSound, {}, {}, "test").midi;
    const std::vector<uint8_t> midi_games =
        ConvertToMidi(*nx, kNoteSound, {}, {}, "test", SoundStart{.low_pass = -0.4f}).midi;
    const std::vector<uint8_t> midi_by_step = ConvertToMidi(*nx_step, kNoteSound, {}, {}, "test").midi;

    SEQLIB_CHECK(!Silent(plain.output) && started.output != plain.output);
    SEQLIB_CHECK(started.output == by_command.output);
    SEQLIB_CHECK(!Silent(ctr_started.output) && ctr_started.output == ctr_by_command.output);
    SEQLIB_CHECK(midi_started != midi && midi_started == midi_by_command);
    SEQLIB_CHECK(midi_games == midi_by_step);
}

// The 3DS plays the start's volume on every sample. At half the volume, each sample is half the sample at full volume,
// rounded to the nearest integer with halves away from zero. A volume that leaves every sample within the level of
// silence makes the measure find the sound silent.
void TestCtrVolume()
{
    const std::shared_ptr<const sar::SoundArchive> ctr = LoadArchive(hokuto::testing::CtrNoteArchiveBytes(), "test");

    const QuietRun full = RunQuieted(ctr, 0, {});
    const QuietRun half = RunQuieted(ctr, 0, {.volume = 0.5f});
    const std::optional<hokuto::song::Length> loud = hokuto::song::MeasureLength(*ctr, 0);
    const std::optional<hokuto::song::Length> faint =
        hokuto::song::MeasureLength(*ctr, 0, {}, hokuto::song::kFullVolumes, SoundStart{.volume = 1.0f / 8192.0f});

    const std::size_t common = std::min(full.output.size(), half.output.size());
    SEQLIB_REQUIRE(common > 0 && !Silent(full.output));
    std::size_t exact = 0;
    for (std::size_t i = 0; i < common; ++i)
    {
        const int sample = full.output[i];
        const int expected = sample >= 0 ? (sample + 1) / 2 : -((1 - sample) / 2);
        exact += half.output[i] == expected ? 1 : 0;
    }
    SEQLIB_CHECK(exact == common);
    SEQLIB_REQUIRE(loud && faint);
    SEQLIB_CHECK(!loud->silent && faint->silent);
}

// The performer's options take the start: the tracks that play and their volumes, the ratios, the sends, the banks,
// the generator's state with no steps before the first tick, and the low-pass. A variable of every track goes to each
// track before the variables of single tracks where both set it, and stays among the variables where only it does. A
// track past 15 takes nothing.
void TestOptions()
{
    namespace export_ = ntdlib::sequence_export;
    const SoundStart start{.muted = 0x10001,
                           .silenced = 0x0004,
                           .track_volumes = {{2, 0.5f}, {20, 0.25f}},
                           .tempo_ratio = 1.5f,
                           .pitch = 2.0f,
                           .volume = 0.75f,
                           .sends = hokuto::format::StartSends{0.1f, 0.2f, 0.3f},
                           .banks = hokuto::format::StartBanks{1, 2, 3, 4},
                           .seed = 7,
                           .low_pass = -0.5f,
                           .variables = {{.variable = 33, .value = 5},
                                         {.variable = 34, .value = 6},
                                         {.variable = 33, .track = 2, .value = 9},
                                         {.variable = 40, .track = 3, .value = 1},
                                         {.variable = 40, .track = 20, .value = 1}}};
    export_::PerformOptions options;
    for (const auto& [variable, value] : hokuto::song::StartValuesOf(start))
    {
        options.variables[static_cast<uint8_t>(variable)] = value;
    }
    export_::Settings settings;
    settings.random_steps_before_start = 3;

    hokuto::song::ApplyStart(options, settings, start);

    SEQLIB_CHECK(options.tracks == 0xFFFA && options.track_volumes[2] == 0.5f && options.track_volumes[1] == 1.0f);
    SEQLIB_CHECK(options.tempo_ratio == 1.5f && options.pitch == 2.0f && options.volume == 0.75f);
    SEQLIB_CHECK(options.lpf == -0.5f);
    SEQLIB_CHECK((options.fx_sends == std::array<float, 3>{0.1f, 0.2f, 0.3f}));
    SEQLIB_CHECK((options.banks == std::array<uint32_t, 4>{1, 2, 3, 4}));
    SEQLIB_CHECK(options.seed == 7 && settings.random_steps_before_start == 0);
    SEQLIB_CHECK((options.variables == std::map<uint8_t, int16_t>{{34, 6}}));
    SEQLIB_REQUIRE(options.track_variables.size() == 16 + 2);
    for (uint8_t track = 0; track < 16; ++track)
    {
        const export_::TrackVariable& every = options.track_variables[track];
        SEQLIB_CHECK(every.track == track && every.index == 1 && every.value == 5);
    }
    const export_::TrackVariable& single = options.track_variables[16];
    const export_::TrackVariable& other = options.track_variables[17];
    SEQLIB_CHECK(single.track == 2 && single.index == 1 && single.value == 9);
    SEQLIB_CHECK(other.track == 3 && other.index == 8 && other.value == 1);
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("starts",
                                    []
                                    {
                                        TestCtrQuiets();
                                        TestWiiUQuiets();
                                        TestMidiQuiets();
                                        TestOtherEngineQuiets();
                                        TestOlderFileKeepsStarts();
                                        TestLengthsQuiet();
                                        TestNwVariables();
                                        TestModelStarts();
                                        TestLowPass();
                                        TestCtrVolume();
                                        TestOptions();
                                    });
}
