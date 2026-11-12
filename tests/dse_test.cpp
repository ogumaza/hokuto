// SPDX-License-Identifier: MIT

// Tests the playback of DSE's songs and sound effects (song/dse.h), in both mixing modes. A small cartridge image from
// ntdlib's builders contains a song, its wave bank, the main bank that contains the samples of that bank, and a bank of
// sound effects. The bank of the song is ntdlib's test bank: its program 0 plays a looping PCM16 wave of the main bank.
//
// The expected notes, offsets, stretches and settings come from the song's events, worked out by hand: a pause waits
// for its length, and a note's length is its ticks. A channel starts at volume 100 and the centre. Each offset is the
// event's place in the song's file. The song's header and its settings take 0x80 bytes. Each track's chunk has a
// header of 0x10 bytes, then its preamble of 4 bytes and its events, padded to 4 bytes.
//
// DSE's driver ticks every 335,104 system clocks, and the DS's sound driver runs a frame every 174,592. The model's
// frames hold two of the sound driver's frames each: 341 samples in strict mixing, and 500 in enhanced mixing. Each of
// the driver's ticks adds 48 ticks of 10,000 µs to the song's count, and the song runs a tick each time the count
// passes a quarter note: 500,000 µs at a tempo of 120. Song tick n therefore runs in the driver's first tick d with
// 480,000 d > 500,000 (n + 1). Tick 0 runs in the driver's tick 2, at clock 670,208, and tick 24 in its tick 27, at
// clock 9,047,808. The sound driver writes a note's channel in its next frame: at clock 698,368, the last clock of the
// model's frame 1, and at clock 9,078,784, the last of frame 25. The notes therefore sound from frames 2 and 26.
//
// ntdlib's dse::Player, run directly, produces the reference output. The model's output has to match it sample for
// sample, from the start and after a seek, in both modes. So does the output with a muted track.

#include "song/dse.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "ntdlib/dse/player.h"
#include "ntdlib/ntr/snd/driver.h"
#include "ntdlib/ntr/sound.h"
#include "ntdlib/sequence_export/dse/convert.h"
#include "ntdlib/testing/dse_song_files.h"
#include "ntdlib/testing/nds_files.h"
#include "seqlib/midi/reader.h"
#include "xsflib/testing/check.h"

#include "driver_test_support.h"
#include "song/items.h"
#include "song/midi_export.h"
#include "song/preview.h"
#include "song/sequence.h"
#include "song/sequence_model.h"
#include "song/sequence_reader.h"

namespace
{

namespace files = ntdlib::testing::dse_song_files;
using files::Bytes;
using files::Join;
using files::Note;
using hokuto::song::DseSet;
using hokuto::song::Mixing;
using hokuto::song::Sequence;
using hokuto::song::SequenceModel;
using hokuto::song::SequenceReader;
using hokuto::testing::HasSound;
using hokuto::testing::TakeRead;
using xsflib::testing::Check;

// The system clocks of the model's frames.
constexpr uint64_t kFrameClocks = 2 * ntdlib::ntr::snd::kFrameClocks;

// Track 0 sets the tempo to 120 and ends. At 48 ticks to a quarter note, a tick lasts about 10.42 ms: about one of
// the model's frames.
const Bytes kConductor = {0, 0, 0, 0, 0xA4, 0x78, 0x98};

// Track 1, on channel 1, at 0xAC in the file:
//
//    0: A9 7F AA 00  bank 0x7F00
//    4: AC 00        program 0
//    6: E0 64        volume 100
//    8: E8 20        pan 0x20: 32 to the left of the centre
//   10: 7F 60 30     note 48 at velocity 127 for 48 ticks
//   13: 92 30        pause 48 ticks
//   15: 98           end
const Bytes kFirst = Join({{1, 1, 0, 0},
                           {0xA9, 0x7F, 0xAA, 0x00, 0xAC, 0x00, 0xE0, 0x64, 0xE8, 0x20},
                           Note(0x7F, 0, 0, 0x30, 1),
                           {0x92, 0x30, 0x98}});

// Track 2, on channel 2, at 0xD0 in the file:
//
//    0: A9 7F AA 00  bank 0x7F00
//    4: AC 00        program 0
//    6: E0 40        volume 64
//    8: 92 18        pause 24 ticks
//   10: 50 67 18     note 55 at velocity 80 for 24 ticks
//   13: 92 18        pause 24 ticks
//   15: 98           end
const Bytes kSecond = Join({{2, 2, 0, 0},
                            {0xA9, 0x7F, 0xAA, 0x00, 0xAC, 0x00, 0xE0, 0x40, 0x92, 0x18},
                            Note(0x50, 7, 0, 0x18, 1),
                            {0x92, 0x18, 0x98}});

// The places of the tracks' chunks from the start of the song's tracks, and of their events, after each chunk's header
// (0x10) and the track's preamble (4). The conductor's 7 bytes take 8 with their padding, and track 1's take 20.
constexpr uint32_t kConductorChunk = 0;
constexpr uint32_t kFirstChunk = kConductorChunk + 0x10 + 8;
constexpr uint32_t kSecondChunk = kFirstChunk + 0x10 + 20;
constexpr uint32_t kConductorEvents = kConductorChunk + 0x14;
constexpr uint32_t kFirstEvents = kFirstChunk + 0x14;
constexpr uint32_t kSecondEvents = kSecondChunk + 0x14;

// A song of 18 tracks: more than the 16 that the other drivers' sequences hold. Tracks 1 to 16 end at once. Track 17
// plays note 48 on channel 3 for 48 ticks.
Bytes WideSong()
{
    std::vector<Bytes> tracks = {kConductor};
    for (uint8_t track = 1; track <= 16; track++)
    {
        tracks.push_back({track, static_cast<uint8_t>(track % 16), 0, 0, 0x98});
    }
    tracks.push_back(Join({{17, 3, 0, 0},
                           {0xA9, 0x7F, 0xAA, 0x00, 0xAC, 0x00, 0xE0, 0x64},
                           Note(0x7F, 0, 0, 0x30, 1),
                           {0x92, 0x30, 0x98}}));
    return files::SongFile(tracks);
}

// A song of `count` tracks. Each track after the conductor ends at once.
Bytes SongOfEndingTracks(uint8_t count)
{
    std::vector<Bytes> tracks = {kConductor};
    for (uint8_t track = 1; track < count; track++)
    {
        tracks.push_back({track, static_cast<uint8_t>(track % 16), 0, 0, 0x98});
    }

    return files::SongFile(tracks);
}

// The sound effect plays note 48 on channel 0 for 24 ticks.
Bytes Effects()
{
    const Bytes track = Join(
        {{0, 0, 0, 0, 0xA9, 0x7F, 0xAA, 0x00, 0xAC, 0x00, 0xE0, 0x7F}, Note(0x60, 0, 0, 0x18, 1), {0x92, 0x18, 0x98}});
    return files::EffectsFile({files::Effect{.tracks = {track}}}, 0x0101);
}

std::shared_ptr<const DseSet> TestSet()
{
    ntdlib::testing::nds_files::Image image;
    image.files = {
        {"SOUND/BGM/bgm.swd", files::MainBankFile()},
        {"SOUND/BGM/bgm0001.smd", files::SongFile({kConductor, kFirst, kSecond})},
        {"SOUND/BGM/bgm0001.swd", files::BankFile()},
        {"SOUND/BGM/bgm0002.smd", WideSong()},
        {"SOUND/BGM/bgm0002.swd", files::BankFile()},
        {"SOUND/SE/se.sed", Effects()},
    };

    return hokuto::song::LoadDseSet(ntdlib::testing::nds_files::BuildImage(image).first);
}

// The output of ntdlib's dse::Player run directly for `frames` of the model's frames, with `muted` tracks, in strict
// mixing as 16-bit samples or in enhanced mixing as floats.
std::vector<int16_t> DirectStrict(const DseSet& set, uint32_t sound, int frames, uint32_t muted = 0)
{
    ntdlib::dse::Player player({.build = set.build});
    for (int track = 0; track < ntdlib::dse::Player::kHostTracks; track++)
    {
        player.MuteTrack(track, (muted >> track & 1) != 0);
    }

    const ntdlib::dse::SequenceFiles sequence = set.sounds[sound].Files();
    player.LoadBanks(sequence);
    player.Start(sequence);

    std::vector<ntdlib::ntr::SoundOutput> out;
    for (int frame = 0; frame < frames; frame++)
    {
        player.Run(kFrameClocks, out);
    }

    std::vector<int16_t> samples;
    ntdlib::ntr::AppendPcm16(out, samples);

    return samples;
}

std::vector<float> DirectEnhanced(const DseSet& set, uint32_t sound, int frames)
{
    ntdlib::dse::Player player({.build = set.build, .enhanced = true});
    const ntdlib::dse::SequenceFiles sequence = set.sounds[sound].Files();
    player.LoadBanks(sequence);
    player.Start(sequence);

    std::vector<float> out;
    for (int frame = 0; frame < frames; frame++)
    {
        player.Run(kFrameClocks, out);
    }

    return out;
}

// The model's frames from the start, up to `frames` of them, and the position after each.
struct Played
{
    std::vector<int16_t> samples;
    std::vector<float> floats;
    std::vector<double> after;
};

Played Play(SequenceModel& model, int frames)
{
    Played played;
    int frame = 0;
    for (bool more = model.SeekFrame(0); more && frame < frames; more = model.RunFrame(), frame++)
    {
        played.samples.insert(played.samples.end(), model.Output().begin(), model.Output().end());
        const std::span<const float> floats = model.FloatOutput();
        played.floats.insert(played.floats.end(), floats.begin(), floats.end());
        played.after.push_back(model.After());
    }

    return played;
}

// The sounds come in the order of ntdlib's ReadSequences: the songs, then the slots of each bank of sound effects. A
// sound effect gets the name of its bank and its slot.
void TestSounds()
{
    const std::shared_ptr<const DseSet> set = TestSet();

    Check(set->warnings.empty(), "every file reads");
    Check(set->build == ntdlib::dse::Build::kLater, "an ARM9 binary without the earlier build's code");
    Check(hokuto::song::DseSounds(*set) == std::vector<uint32_t>{0, 1, 2}, "two songs and a sound effect");
    Check(hokuto::song::DseSoundName(*set, 0) == "bgm0001" && hokuto::song::DseSoundName(*set, 1) == "bgm0002",
          "the songs' names");
    Check(hokuto::song::DseSoundName(*set, 2) == "se_000", "the sound effect's name");
    Check(hokuto::song::DseSoundName(*set, 9) == "Sound 9", "a number past the sounds");
    Check(hokuto::song::DseSoundFolder(*set, 0) == "SOUND/BGM/" && hokuto::song::DseSoundFolder(*set, 2) == "SOUND/SE/",
          "the folders");
    Check(hokuto::song::DseSoundFolder(*set, 9).empty(), "no folder past the sounds");

    // An image without DSE's files has no sounds.
    const std::shared_ptr<const DseSet> empty =
        hokuto::song::LoadDseSet(ntdlib::testing::nds_files::BuildImage({}).first);
    Check(empty->sounds.empty() && empty->warnings.empty(), "no sounds");
}

// The arrangement of the song, and of the sound effect.
void TestRead()
{
    const std::shared_ptr<const DseSet> set = TestSet();
    const Sequence sequence = hokuto::song::ReadSequence(*set, 0);

    Check(sequence.name == "bgm0001" && sequence.sound == 0, "the name and the number");
    Check(sequence.timebase == 48, "48 ticks to a quarter note");
    SEQLIB_REQUIRE(!sequence.tempo.empty());
    Check(sequence.tempo[0].tick == 0 && std::abs(sequence.tempo[0].bpm - 120.0) < 0.1, "the tempo");
    Check(!sequence.loop, "no loop");
    Check(sequence.notes_end == 48, "the last note ends at tick 48");
    SEQLIB_REQUIRE(sequence.tracks.size() == 3);

    // The conductor runs its tempo and its end at tick 0.
    const hokuto::song::Track& conductor = sequence.tracks[0];
    Check(conductor.number == 0 && conductor.notes.empty(), "the conductor plays no notes");
    SEQLIB_REQUIRE(conductor.stretches.size() == 1);
    Check(conductor.stretches[0].tick == 0 && conductor.stretches[0].first == kConductorEvents &&
              conductor.stretches[0].last == kConductorEvents + 2,
          "the conductor's stretch");

    const hokuto::song::Track& first = sequence.tracks[1];
    const hokuto::song::Track& second = sequence.tracks[2];
    Check(first.number == 1 && second.number == 2, "the tracks' numbers");
    SEQLIB_REQUIRE(first.notes.size() == 1 && second.notes.size() == 1);
    Check(first.notes[0].start == 0 && first.notes[0].length == 48 && first.notes[0].key == 48 &&
              first.notes[0].velocity == 127 && first.notes[0].command == kFirstEvents + 10,
          "track 1's note and its event");
    Check(second.notes[0].start == 24 && second.notes[0].length == 24 && second.notes[0].key == 55 &&
              second.notes[0].velocity == 80 && second.notes[0].command == kSecondEvents + 10,
          "track 2's note and its event");
    Check(first.program == 0 && first.bank == "B_TEST", "the instrument, by the bank's name");

    // Each track runs its events in one stretch, from its first to its end.
    SEQLIB_REQUIRE(first.stretches.size() == 1 && second.stretches.size() == 1);
    Check(first.stretches[0].tick == 0 && first.stretches[0].first == kFirstEvents &&
              first.stretches[0].last == kFirstEvents + 15,
          "track 1's stretch");
    Check(second.stretches[0].tick == 0 && second.stretches[0].first == kSecondEvents &&
              second.stretches[0].last == kSecondEvents + 15,
          "track 2's stretch");

    // Track 1 sets volume 100 and pan -32 at tick 0, and track 2 volume 64. Channel 0 keeps its starting volume.
    Check(conductor.volume == std::vector<hokuto::song::Setting>{{0, 100}}, "the conductor's volume");
    Check(first.volume == std::vector<hokuto::song::Setting>{{0, 100}}, "track 1's volume");
    Check(first.pan == std::vector<hokuto::song::Setting>{{0, -32}}, "track 1's pan");
    Check(second.volume == std::vector<hokuto::song::Setting>{{0, 64}} && second.pan.empty(), "track 2's mix");

    // The song of 18 tracks keeps track 17's number. Tracks 1 to 16 run only their end.
    const Sequence wide = hokuto::song::ReadSequence(*set, 1);
    SEQLIB_REQUIRE(!wide.tracks.empty());
    const hokuto::song::Track& last = wide.tracks.back();
    Check(last.number == 17 && last.notes.size() == 1 && last.notes[0].length == 48, "track 17's note");

    // The sound effect's events count from the start of its tracks: the track's chunk header and its preamble take 0x14
    // bytes. Its note follows the bank, the program and the volume.
    const Sequence effect = hokuto::song::ReadSequence(*set, 2);
    Check(effect.name == "se_000", "the sound effect's name");
    SEQLIB_REQUIRE(effect.tracks.size() == 1 && effect.tracks[0].notes.size() == 1);
    Check(effect.tracks[0].number == 0 && effect.tracks[0].notes[0].key == 48 &&
              effect.tracks[0].notes[0].length == 24 && effect.tracks[0].notes[0].command == 0x14 + 8,
          "the sound effect's note and its event");

    const bool refused =
        !xsflib::testing::ErrorOf<std::runtime_error>([&] { hokuto::song::ReadSequence(*set, 3); }).empty();
    Check(refused, "a number past the sounds can't be read");
}

// A model mutes and meters 32 tracks (SequenceModel::kTracks). A song of 32 tracks reads, and one of 33 can't be read.
void TestTrackLimit()
{
    ntdlib::testing::nds_files::Image image;
    image.files = {
        {"SOUND/BGM/bgm.swd", files::MainBankFile()}, {"SOUND/BGM/bgm0001.smd", SongOfEndingTracks(32)},
        {"SOUND/BGM/bgm0001.swd", files::BankFile()}, {"SOUND/BGM/bgm0002.smd", SongOfEndingTracks(33)},
        {"SOUND/BGM/bgm0002.swd", files::BankFile()},
    };
    const std::shared_ptr<const DseSet> set =
        hokuto::song::LoadDseSet(ntdlib::testing::nds_files::BuildImage(image).first);

    const std::string widest = xsflib::testing::ErrorOf([&] { hokuto::song::ReadSequence(*set, 0); });
    const std::string too_wide =
        xsflib::testing::ErrorOf<std::runtime_error>([&] { hokuto::song::ReadSequence(*set, 1); });

    Check(widest.empty(), "a song of 32 tracks reads");
    Check(too_wide == "Can't read bgm0002. It has 33 tracks, and Hokuto plays at most 32.", "a song of 33 tracks");
}

// A frame here is one of the model's frames. A tick of the song lasts about one of them. Song tick n runs in the
// model's frame k when the driver's tick of it (see above) comes by the frame's last clock, 349,184 (k + 1). After
// frame 0 the position is 0, after frame 1 it is 1, and after frame 48 it is 48. Tick 48 runs the tracks' ends in frame
// 49. The releases then sound on, and the position continues at the tempo. The output ends with silence after them.
// With track 1 muted, the output stays silent until track 2's note sounds in frame 26.
void TestPlays()
{
    const std::shared_ptr<const DseSet> set = TestSet();
    const auto sequence = std::make_shared<const Sequence>(hokuto::song::ReadSequence(*set, 0));
    const auto model = std::make_shared<SequenceModel>(
        set, sequence, hokuto::song::ModelOptions{.mixing = Mixing::kStrict, .snapshot_interval = 20});
    const Played played = Play(*model, 1000);
    const std::vector<int16_t> expected = DirectStrict(*set, 0, 1100);
    const std::size_t frame = 2 * model->FrameSamples();
    const bool sought = model->SeekFrame(30);
    const std::vector<int16_t> after_seek(model->Output().begin(), model->Output().end());

    Check(model->SampleRate() == 32728 && model->FrameSamples() == 341, "the strict mode's frames");
    Check(model->TicksPerFrame(0.0) > 0.999 && model->TicksPerFrame(0.0) < 1.002, "a tick in each frame");
    SEQLIB_REQUIRE(played.samples.size() > 48 * frame && played.samples.size() < 1000 * frame);
    Check(!HasSound(std::span<const int16_t>(played.samples).first(2 * frame)), "frames 0 and 1 are silent");
    Check(HasSound(std::span<const int16_t>(played.samples).subspan(2 * frame, frame)), "the note sounds in frame 2");
    Check(std::equal(played.samples.begin(), played.samples.end(), expected.begin()), "the reference's samples");
    Check(!HasSound(std::span<const int16_t>(expected).subspan(played.samples.size())), "silence after the end");
    Check(played.after[0] == 0.0 && played.after[1] == 1.0 && played.after[48] == 48.0,
          "the position counts the song's ticks");
    SEQLIB_REQUIRE(played.after.size() > 50);
    Check(played.after[49] == 49.0 && played.after[50] == 49.0 + model->TicksPerFrame(49.0),
          "the song ends after tick 48, and the position goes on at the tempo");
    Check(sought && after_seek.size() == frame &&
              std::equal(after_seek.begin(), after_seek.end(), expected.begin() + 30 * frame),
          "a seek gives the reference's frame");

    // A muted track's notes leave the output.
    const auto muted = std::make_shared<SequenceModel>(
        set, sequence, hokuto::song::ModelOptions{.mixing = Mixing::kStrict, .snapshot_interval = 20});
    muted->SetMutes(1u << 1);
    const Played muted_played = Play(*muted, 1000);
    const std::vector<int16_t> muted_expected = DirectStrict(*set, 0, 1100, 1u << 1);
    SEQLIB_REQUIRE(muted_played.samples.size() > 48 * frame && muted_played.samples.size() <= muted_expected.size());
    Check(std::equal(muted_played.samples.begin(), muted_played.samples.end(), muted_expected.begin()),
          "the muted output");
    Check(!HasSound(std::span<const int16_t>(muted_played.samples).first(26 * frame)), "silence until track 2's note");
    Check(HasSound(std::span<const int16_t>(muted_played.samples).subspan(26 * frame, frame)), "track 2's note");

    // The enhanced mode plays the same song at its rate.
    const auto enhanced = std::make_shared<SequenceModel>(
        set, sequence, hokuto::song::ModelOptions{.mixing = Mixing::kEnhanced, .snapshot_interval = 20});
    const Played enhanced_played = Play(*enhanced, 1000);
    const std::vector<float> enhanced_expected = DirectEnhanced(*set, 0, 1100);
    Check(enhanced->SampleRate() == 47989 && enhanced->FrameSamples() == 500, "the enhanced mode's frames");
    SEQLIB_REQUIRE(enhanced_played.floats.size() > 48 * 1000 && enhanced_played.floats.size() < 1000 * 1000);
    Check(!HasSound(std::span<const float>(enhanced_played.floats).first(2 * 1000)), "frames 0 and 1 are silent");
    Check(HasSound(std::span<const float>(enhanced_played.floats).subspan(2 * 1000, 1000)), "frame 2 sounds");
    Check(std::equal(enhanced_played.floats.begin(), enhanced_played.floats.end(), enhanced_expected.begin()),
          "the enhanced mode's samples");
    const bool enhanced_sought = enhanced->SeekFrame(30);
    const std::span<const float> enhanced_seek = enhanced->FloatOutput();
    Check(enhanced_sought && enhanced_seek.size() == 1000 &&
              std::equal(enhanced_seek.begin(), enhanced_seek.end(), enhanced_expected.begin() + 30 * 1000),
          "a seek in the enhanced mode");

    // The sound effect plays as the reference does.
    const auto effect = std::make_shared<const Sequence>(hokuto::song::ReadSequence(*set, 2));
    SequenceModel effect_model(set, effect,
                               hokuto::song::ModelOptions{.mixing = Mixing::kStrict, .snapshot_interval = 20});
    const Played effect_played = Play(effect_model, 1000);
    const std::vector<int16_t> effect_expected = DirectStrict(*set, 2, 1100);
    SEQLIB_REQUIRE(effect_played.samples.size() > 24 * frame && effect_played.samples.size() < 1000 * frame);
    Check(std::equal(effect_played.samples.begin(), effect_played.samples.end(), effect_expected.begin()),
          "the sound effect's samples");
}

// A track's meter adds up the output of its notes on the output's scale. The output's 10-bit samples are 64 steps apart
// there. While track 1's note plays alone, from frame 2 to frame 25, its meter is within 64 of the output's peaks, and
// the meters of the other tracks stay at 0. Track 17 of the song of 18 tracks has a meter and a mute too.
void TestMeters()
{
    const std::shared_ptr<const DseSet> set = TestSet();
    const auto sequence = std::make_shared<const Sequence>(hokuto::song::ReadSequence(*set, 0));
    SequenceModel model(set, sequence);
    bool near = true;
    bool quiet = true;
    int frame = 0;
    for (bool more = model.SeekFrame(0); more && frame < 26; more = model.RunFrame(), frame++)
    {
        int32_t peak = 0;
        for (const int16_t sample : model.Output())
        {
            peak = std::max(peak, std::abs(static_cast<int32_t>(sample)));
        }
        const std::array<int32_t, 2> meter = model.TrackPeaks(1);
        near = near && std::abs(std::max(meter[0], meter[1]) - peak) < 64;
        quiet =
            quiet && model.TrackPeaks(0) == std::array<int32_t, 2>{} && model.TrackPeaks(2) == std::array<int32_t, 2>{};
    }

    Check(frame == 26, "the frames play");
    Check(near, "track 1's meter follows the output's peaks");
    Check(quiet, "the other tracks' meters stay at 0");

    const auto wide = std::make_shared<const Sequence>(hokuto::song::ReadSequence(*set, 1));
    SequenceModel wide_model(set, wide);
    bool metered = false;
    frame = 0;
    for (bool more = wide_model.SeekFrame(0); more && frame < 20; more = wide_model.RunFrame(), frame++)
    {
        const std::array<int32_t, 2> meter = wide_model.TrackPeaks(17);
        metered = metered || meter[0] > 0 || meter[1] > 0;
    }
    Check(metered, "track 17's meter");

    SequenceModel muted(set, wide);
    muted.SetMutes(1u << 17);
    const Played muted_played = Play(muted, 1000);
    Check(!muted_played.samples.empty() && !HasSound(std::span<const int16_t>(muted_played.samples)),
          "track 17's mute silences the song");
}

// The reader reads a DSE sound away from the program's thread, with the set.
void TestReader()
{
    const std::shared_ptr<const DseSet> set = TestSet();
    SequenceReader reader;
    reader.Request(set, 0);
    std::optional<SequenceReader::Result> read;
    read = TakeRead(reader);

    Check(read && read->sequence && *read->sequence == hokuto::song::ReadSequence(*set, 0), "the read sequence");
    Check(read && read->dse == set && !read->archive && !read->ds, "the set");

    reader.Request(set, 7);
    read.reset();
    read = TakeRead(reader);
    Check(read && !read->sequence && !read->error.empty(), "a number past the sounds gives an error");
}

// A sound's MIDI file, SoundFont and SFZ files are those of ntdlib's converter with the set's build. A number past the
// sounds doesn't convert.
void TestMidi()
{
    namespace dse_export = ntdlib::sequence_export::dse;
    const std::shared_ptr<const DseSet> set = TestSet();
    for (const uint32_t sound : {0u, 1u, 2u})
    {
        dse_export::PerformOptions options;
        options.build = set->build;
        const dse_export::Conversion expected = dse_export::ConvertSequence(
            set->sounds[sound].Files(), hokuto::song::DseSoundName(*set, sound), options, "Hokuto");
        const ntdlib::sequence_export::Conversion conversion = hokuto::song::ConvertToMidi(*set, sound, "Hokuto");
        Check(conversion.midi == expected.midi && conversion.sf2 == expected.sf2 &&
                  conversion.sfz.files.size() == expected.sfz.files.size() &&
                  conversion.sfz.samples.size() == expected.sfz.samples.size() &&
                  conversion.samples == expected.samples && conversion.channels == expected.channels &&
                  conversion.warnings == expected.warnings,
              "the converter's files");
    }

    const ntdlib::sequence_export::Conversion song = hokuto::song::ConvertToMidi(*set, 0, "Hokuto");
    const seqlib::midi::File file = seqlib::midi::Read(song.midi);
    const std::vector<seqlib::midi::Note> notes = seqlib::midi::Notes(file);
    Check(!song.sf2.empty() && !song.sfz.files.empty(), "a SoundFont and SFZ files");
    SEQLIB_REQUIRE(notes.size() == 2);
    Check(notes[0].key == 48 && notes[0].on == 0 && notes[1].key == 55 && notes[1].on == 24u * file.division / 48,
          "the song's two notes");

    const std::string past_the_end =
        xsflib::testing::ErrorOf<std::runtime_error>([&] { hokuto::song::ConvertToMidi(*set, 99, "Hokuto"); });
    Check(past_the_end == "The image has no DSE sound 99.", "a number past the sounds doesn't convert");
}

// Program 2 of ntdlib's test bank plays nothing at velocity 16. The note at tick 24 therefore starts no voice. The
// track lists it among the notes that the driver never started, with its place from the start of the song's tracks. The
// note after it plays.
void TestUnstarted()
{
    const Bytes track = Join({{1, 1, 0, 0},
                              {0xA9, 0x7F, 0xAA, 0x00, 0xAC, 0x02, 0x92, 0x18, 0xA0, 0x06},
                              Note(0x10, 0, 0, 0x18, 1),
                              {0x92, 0x18},
                              Note(0x7F, 0, 0, 0x18, 1),
                              {0x92, 0x18, 0x98}});
    ntdlib::testing::nds_files::Image image;
    image.files = {
        {"SOUND/BGM/bgm.swd", files::MainBankFile()},
        {"SOUND/BGM/bgm0001.smd", files::SongFile({kConductor, track})},
        {"SOUND/BGM/bgm0001.swd", files::BankFile()},
    };
    const Sequence sequence =
        hokuto::song::ReadSequence(*hokuto::song::LoadDseSet(ntdlib::testing::nds_files::BuildImage(image).first), 0);
    SEQLIB_REQUIRE(sequence.tracks.size() == 2);
    const hokuto::song::Track& played = sequence.tracks[1];
    SEQLIB_REQUIRE(played.unstarted.size() == 1 && played.notes.size() == 1);
    Check(played.unstarted[0].start == 24 && played.unstarted[0].length == 24 && played.unstarted[0].key == 72 &&
              played.unstarted[0].velocity == 0x10 && played.unstarted[0].command == kFirstEvents + 10,
          "the note that starts no voice, and its event");
    Check(played.notes[0].start == 48 && played.notes[0].velocity == 0x7F, "the note after it plays");
}

// A key group of one voice: the later note takes the voice of the note that sounds on its channel. The arrangement
// marks the earlier note as cut.
void TestCutNote()
{
    const Bytes track = Join({{1, 1, 0, 0},
                              {0xA9, 0x7F, 0xAA, 0x00, 0xAC, 0x01},
                              Note(0x60, 4, 1, 0x30, 1),
                              {0x92, 0x0C},
                              Note(0x60, 5, 0, 0x0C, 1),
                              {0x92, 0x60, 0x98}});
    ntdlib::dse::WaveBank bank = ntdlib::dse::WaveBank::Parse(files::BankFile());
    bank.instruments->key_groups.at(1)[2] = 1;
    ntdlib::testing::nds_files::Image image;
    image.files = {
        {"SOUND/BGM/bgm.swd", files::MainBankFile()},
        {"SOUND/BGM/bgm0001.smd", files::SongFile({kConductor, track})},
        {"SOUND/BGM/bgm0001.swd", bank.Write()},
    };
    const Sequence sequence =
        hokuto::song::ReadSequence(*hokuto::song::LoadDseSet(ntdlib::testing::nds_files::BuildImage(image).first), 0);
    SEQLIB_REQUIRE(sequence.tracks.size() == 2 && sequence.tracks[1].notes.size() == 2);
    const hokuto::song::Track& played = sequence.tracks[1];
    Check(played.notes[0].cut && played.notes[0].length == 12, "the later note cuts the note that its channel holds");
    Check(!played.notes[1].cut, "the later note isn't cut");
}

// A set's banks: every bank of its files, each with the main bank that contains its waves. A bank that a sound plays
// keeps its copy. The banks come in the order of the image's files.
void TestBanks()
{
    const std::shared_ptr<const DseSet> set = TestSet();
    SEQLIB_REQUIRE(set->banks.size() == 3);
    Check(hokuto::song::DseProgramBanks(*set) == std::vector<uint32_t>{1, 2}, "the banks with programs");
    Check(hokuto::song::DseSampleBanks(*set) == std::vector<uint32_t>{0}, "the bank with samples");
    Check(set->banks[0].main_bank == nullptr && set->banks[1].main_bank == set->banks[0].bank &&
              set->banks[2].main_bank == set->banks[0].bank,
          "the main bank of the banks whose waves are in one");
    Check(set->banks[1].bank == set->sounds[0].banks[0], "a bank that a sound plays keeps its copy");
    Check(hokuto::song::DseBankPrograms(*set->banks[1].bank) == std::vector<uint32_t>{0, 1, 2, 3}, "the programs");
    Check(hokuto::song::DseBankWaves(*set->banks[0].bank) == std::vector<uint32_t>{0, 1, 2}, "the main bank's waves");

    // Wave 1 of the main bank is PCM8 at 16,000 Hz, without a loop. Its 32 words from byte 0x100 hold 128 samples. An
    // 8-bit sample s becomes the 16-bit sample s * 256. The main bank's even bytes hold 7 times their half, and its odd
    // bytes 13 times their half plus 5.
    const ntdlib::wave::Pcm pcm = hokuto::song::DseWavePcm(*set, 0, 1);
    SEQLIB_REQUIRE(pcm.channels.size() == 1);
    Check(pcm.sample_rate == 16000 && !pcm.loop && pcm.channels[0].size() == 128, "the wave's rate, loop and length");
    bool same = true;
    for (std::size_t i = 0; i < pcm.channels[0].size(); i++)
    {
        const std::size_t place = 0x100 + i;
        const auto byte = static_cast<uint8_t>(place % 2 == 0 ? place / 2 * 7 : place / 2 * 13 + 5);
        same = same && pcm.channels[0][i] == static_cast<int8_t>(byte) * 256;
    }
    Check(same, "the wave's samples");
    Check(
        xsflib::testing::ErrorOf<std::runtime_error>([&] { hokuto::song::DseWavePcm(*set, 0, 9); }) ==
            "Can't decode " + hokuto::song::DseItemName(*set, {.kind = hokuto::song::ItemKind::kWave, .wave = 9}) + ".",
        "a wave missing from the bank");

    // The items' names and files.
    const std::string bank_name = hokuto::song::DseBankName(*set->banks[1].bank);
    const hokuto::song::Item bank{.kind = hokuto::song::ItemKind::kBank, .index = 1};
    const hokuto::song::Item main_bank{.kind = hokuto::song::ItemKind::kWaveArchive, .index = 0};
    const hokuto::song::Item wave{.kind = hokuto::song::ItemKind::kWave, .index = 0, .wave = 1};
    Check(hokuto::song::DseItemName(*set, bank) == bank_name, "a bank's name");
    Check(hokuto::song::DseItemFileName(*set, bank) == bank_name + ".swd", "a bank's file name");
    Check(hokuto::song::DseItemFileName(*set, wave, 12) == hokuto::song::DseBankName(*set->banks[0].bank) + "_01.wav",
          "a wave's file name, with the digits of the last wave");
    Check(hokuto::song::DseItemBytes(*set, bank) == files::BankFile(), "a bank's file");
    Check(hokuto::song::DseItemBytes(*set, main_bank) == files::MainBankFile(), "a main bank's file");
    Check(hokuto::song::DseItemBytes(*set, wave).size() > 128, "a wave's WAV file");

    // The instruments of a bank export as ntdlib's conversion writes them.
    const std::vector<uint32_t> programs = {0, 1};
    const ntdlib::sequence_export::BankConversion converted = hokuto::song::ConvertDseBank(*set, 1, programs);
    const ntdlib::dse::WaveBank bank_file = ntdlib::dse::WaveBank::Parse(files::BankFile());
    const ntdlib::dse::WaveBank main_file = ntdlib::dse::WaveBank::Parse(files::MainBankFile());
    const ntdlib::sequence_export::BankConversion direct =
        ntdlib::sequence_export::dse::ConvertBank(bank_file, &main_file, programs, bank_name, "Hokuto");
    SEQLIB_REQUIRE(converted.sound_fonts.size() == 1 && direct.sound_fonts.size() == 1);
    Check(converted.sound_fonts[0].sf2 == direct.sound_fonts[0].sf2, "a bank's SoundFont");
    Check(converted.sfz.files.size() == 2, "a bank's SFZ files");
    Check(xsflib::testing::ErrorOf<hokuto::song::BankWithoutInstruments>(
              [&] { hokuto::song::ConvertDseBank(*set, 1, std::vector<uint32_t>{}); }) ==
              bank_name + " has no instruments.",
          "a bank's conversion without programs");
}

// A note's preview plays what dse::Player plays, run directly on a song with the note's events written out by hand, up
// to the release and after it, in both mixing modes. The song waits after the note. A host then lets the note go.
void TestNotePreview()
{
    const std::shared_ptr<const DseSet> set = TestSet();
    const hokuto::song::DseBank& bank = set->banks[1];
    Check(hokuto::song::DseNotePreview::KeyOf(bank, 0) == 60, "a program that plays key 60");
    Check(hokuto::song::DseNotePreview::KeyOf(bank, 9) == -1, "a program that the bank lacks");

    const Bytes track = Join({{0, 0, 0, 0},
                              {0xA4, 0xFF, 0xA9, 0x7F, 0xAA, 0x00, 0xAC, 0x00, 0xE0, 0x7F, 0xA0, 0x05},
                              Note(hokuto::song::NotePreview::kVelocity, 0, 0, 0xFFFFFF, 3),
                              {0x94, 0xFF, 0xFF, 0xFF, 0x98}});
    const Bytes song_bytes = files::SongFile({track});
    const ntdlib::dse::Song song = ntdlib::dse::Song::Parse(song_bytes);
    const ntdlib::dse::SequenceFiles sequence{
        .song = &song, .banks = {bank.bank.get()}, .main_bank = bank.main_bank.get()};

    for (const hokuto::song::Mixing mixing : {hokuto::song::Mixing::kStrict, hokuto::song::Mixing::kEnhanced})
    {
        const bool enhanced = mixing == hokuto::song::Mixing::kEnhanced;
        ntdlib::dse::Player player({.enhanced = enhanced});
        player.LoadBanks(sequence);
        player.Start(sequence);

        std::vector<float> expected;
        const uint64_t frames = hokuto::song::DseNotePreview::HoldFrames() + 100;
        for (uint64_t frame = 0; frame < frames; frame++)
        {
            if (frame >= hokuto::song::DseNotePreview::HoldFrames())
            {
                player.Driver().ReleaseFollowedNotes();
            }
            if (enhanced)
            {
                player.Run(kFrameClocks, expected);
            }
            else
            {
                std::vector<ntdlib::ntr::SoundOutput> out;
                player.Run(kFrameClocks, out);
                std::vector<int16_t> samples;
                ntdlib::ntr::AppendPcm16(out, samples);
                for (const int16_t sample : samples)
                {
                    expected.push_back(static_cast<float>(sample) / 32768.0f);
                }
            }
        }

        hokuto::song::DseNotePreview preview(bank, 0, ntdlib::dse::Build::kLater, mixing);
        std::vector<float> rendered(expected.size());
        const std::size_t done = preview.Render(rendered.data(), rendered.size() / 2);
        rendered.resize(done * 2);
        Check(std::ranges::any_of(rendered, [](float sample) { return sample != 0.0f; }), "the preview sounds");
        expected.resize(rendered.size());
        Check(rendered == expected, "the preview plays what the player plays");
        Check(preview.SampleRate() > 0, "the preview's rate");
    }

    // A note that the Key Editor lets go at once still ends: its release waits for the driver to start it.
    const auto release = std::make_shared<std::atomic<bool>>(true);
    hokuto::song::DseNotePreview tap(bank, 0, 60, 100, release, ntdlib::dse::Build::kLater);
    std::vector<float> tapped(2 * 48000 * 12);
    const std::size_t tap_frames = tap.Render(tapped.data(), tapped.size() / 2);
    Check(tap_frames < tapped.size() / 2, "a note let go at once ends");

    // A program that plays at no key ends at once.
    hokuto::song::DseNotePreview nothing(bank, 9, ntdlib::dse::Build::kLater);
    std::vector<float> silent(2);
    Check(nothing.Render(silent.data(), 1) == 0, "a program missing from the bank plays nothing");
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("dse",
                                    []
                                    {
                                        TestSounds();
                                        TestRead();
                                        TestTrackLimit();
                                        TestPlays();
                                        TestMeters();
                                        TestReader();
                                        TestMidi();
                                        TestUnstarted();
                                        TestCutNote();
                                        TestBanks();
                                        TestNotePreview();
                                    });
}
