// SPDX-License-Identifier: MIT

// Tests the DS's playback (song/ds.h), in both mixing modes. A small sound archive from ntdlib's builders contains two
// sequences, an empty entry and a sequence archive. Program 0 of its bank plays a square wave in PCM8 that loops. The
// wave is 4 words long: GBATEK names that as the shortest wave that the channels of the DS play.
//
// The sounds are numbered in the order of the sequences of the INFO block, then of the sequence archive. The expected
// notes, ticks and lengths come from the driver's commands, worked out by hand: a note waits for its length while the
// track's note wait is on. A track opens with the note wait on. The driver ticks each time its tempo counter reaches
// 240. The counter starts at 240 when the sequence starts, and a tempo of 120 adds 120 a frame. The first frame plays
// silence, then starts the sequence and runs its tick 0. The ticks follow in every second frame of the driver: one in
// each of the model's frames. A frame of the driver lasts 174,592 of the system's 33,513,982 clocks a second, and the
// sound unit makes a sample each 1,024 clocks. The driver writes a note's channel at the start of its next frame,
// before it runs the sequences, and the channel sounds in the samples after that frame. A note of tick 0 therefore
// sounds from the model's frame 1, and a note of tick 24 from frame 25.
//
// ntdlib's ArchivePlayer, run directly, produces the reference output: the player has to match the reference, sample
// for sample, from the start and after a seek, in both modes. So does the output with a muted track and with a
// variable's starting value. The reference reads the variable from the game settings at the start. The model sends
// it to the driver after the start, before the first frame.
//
// The export to MIDI (song/midi_export.h) has to give the files of ntdlib's converter of the DS, run directly with the
// archive's build and the variables' starting values.
//
// The browser's note preview (song/preview.h) has to play as ntdlib's ArchivePlayer, run directly, plays a sequence of
// the note written out by hand, on the same bank and wave archive.
//
// The same archive in a cartridge image of ntdlib's builders gets the game of the image. The SDK's output effect of two
// games has to play as ntdlib's ArchivePlayer plays it with ntdlib::ntr::OutputEffect{}. ntdlib checks that player
// against the games' code on its model of the DS. An image of another game plays as before.
//
// The browser's items (song/ds.h) come from a second archive with a wave archive of two waves and two streams. Their
// files have to be the bytes of ntdlib's builders. Their samples come from the builders' bytes, worked out by hand:
// PCM8 uses a byte as the high byte of a 16-bit sample. The DS's channel plays at 16,756,991 Hz over the timer. The
// builders give the timer as that over the rate, rounded down.

#include "song/ds.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "ntdlib/ntr/archive_player.h"
#include "ntdlib/ntr/game_profiles.h"
#include "ntdlib/ntr/sound.h"
#include "ntdlib/sdat/formats.h"
#include "ntdlib/sdat/layout.h"
#include "ntdlib/sequence_export/ntr/convert.h"
#include "ntdlib/testing/nds_files.h"
#include "ntdlib/testing/sdat_files.h"
#include "seqlib/midi/reader.h"
#include "xsflib/bytes.h"
#include "xsflib/testing/check.h"

#include "driver_test_support.h"
#include "song/midi_export.h"
#include "song/preview.h"
#include "song/sequence.h"
#include "song/sequence_model.h"
#include "song/sequence_reader.h"

namespace
{

namespace files = ntdlib::testing::sdat_files;
namespace sdat = ntdlib::sdat;
using files::Bytes;
using hokuto::song::DsArchive;
using hokuto::song::Mixing;
using hokuto::song::Sequence;
using hokuto::song::SequenceModel;
using hokuto::song::SequenceReader;
using hokuto::song::StartValues;
using hokuto::testing::HasSound;
using hokuto::testing::TakeRead;
using xsflib::testing::Check;

constexpr uint16_t kNone = 0xFFFF;

// Sequence 0: two tracks.
//
//    0: FE 03 00        allocate tracks 0 and 1
//    3: 93 01 10 00 00  open track 1 at 16
//    8: C1 64           volume 100
//   10: C0 20           pan 0x20: 32 to the left of the centre
//   12: 3C 7F 30        note 60, velocity 127, 48 ticks
//   15: FF              end
//   16: C1 40           track 1: volume 64
//   18: 80 18           wait 24 ticks
//   20: 43 50 18        note 67, velocity 80, 24 ticks
//   23: FF              end
//
// The allocation belongs to the sequence's start: track 0 runs from offset 3.
const Bytes kTwoTracks = {0xFE, 0x03, 0x00, 0x93, 0x01, 0x10, 0x00, 0x00, 0xC1, 0x64, 0xC0, 0x20,
                          0x3C, 0x7F, 0x30, 0xFF, 0xC1, 0x40, 0x80, 0x18, 0x43, 0x50, 0x18, 0xFF};

// Sequence 1: variable 0 chooses the notes.
//
//    0: B8 00 01 00     compare variable 0 with 1
//    4: A2 94 0C 00 00  if equal, jump to 12
//    9: 3C 7F 18        note 60, 24 ticks
//   12: 43 7F 18        note 67, 24 ticks
//   15: FF              end
const Bytes kChoice = {0xB8, 0x00, 0x01, 0x00, 0xA2, 0x94, 0x0C, 0x00, 0x00, 0x3C, 0x7F, 0x18, 0x43, 0x7F, 0x18, 0xFF};

// The sequence archive's bytecode: note 60 at offset 0 and note 67 at offset 4, each followed by an end. Its second
// entry has no sequence.
const Bytes kArchived = {0x3C, 0x7F, 0x18, 0xFF, 0x43, 0x7F, 0x18, 0xFF};

// The bank's wave: a square wave in PCM8 that loops.
const files::Wave kSquare{
    .loop = 1,
    .loop_length = 4,
    .data = {0x40, 0x40, 0xC0, 0xC0, 0x40, 0x40, 0xC0, 0xC0, 0x40, 0x40, 0xC0, 0xC0, 0x40, 0x40, 0xC0, 0xC0}};

// The test archive. `first_name`, when it isn't empty, names its first sequence in a SYMB block.
Bytes TestArchiveBytes(const std::string& first_name = "")
{
    sdat::ArchiveModel model;
    if (!first_name.empty())
    {
        model.symbols = sdat::Symbols{};
        model.symbols->names[0] = {first_name, std::nullopt, std::nullopt};
        model.symbols->names[1] = {std::nullopt};
        model.symbols->sequence_names = {std::nullopt};
    }
    model.sequences = {sdat::SequenceInfo{0, 0, 127, 64, 64, 0, 0, ""}, sdat::SequenceInfo{1, 0, 127, 64, 64, 0, 0, ""},
                       std::nullopt};
    model.sequence_archives = {sdat::SequenceArchiveInfo{2, "", {}}};
    model.banks = {sdat::BankInfo{3, {0, kNone, kNone, kNone}, ""}};
    model.wave_archives = {sdat::WaveArchiveInfo{4, ""}};
    model.players = {sdat::PlayerInfo{1, 0, 0, 0, ""}};
    model.files = {files::Sequence(kTwoTracks), files::Sequence(kChoice),
                   files::SequenceArchive({{.offset = 0}, {.offset = 0xFFFFFFFF}, {.offset = 4}}, kArchived),
                   files::Bank({{.type = 1, .region = {.wave = 0}}}), files::WaveArchive({kSquare})};

    return sdat::BuildArchive(model);
}

std::shared_ptr<const DsArchive> TestArchive()
{
    return hokuto::song::LoadDsArchive(TestArchiveBytes());
}

// A note by itself, written out by hand from the driver's commands:
//
//    0: 81 00           program 0
//    2: C1 7F           volume 127
//    4: C0 40           pan 64, the centre
//    6: C8 01           ties on
//    8: 3C 64 01        note 60, velocity 100, 1 tick
//   11: 80 01           wait 1 tick
//   13: B8 00 01 00     compare variable 0 with 1
//   17: A2 94 1A 00 00  if equal, jump to 26
//   22: 94 0B 00 00     jump to 11
//   26: FF              end
const Bytes kNote = {0x81, 0x00, 0xC1, 0x7F, 0xC0, 0x40, 0xC8, 0x01, 0x3C, 0x64, 0x01, 0x80, 0x01, 0xB8,
                     0x00, 0x01, 0x00, 0xA2, 0x94, 0x1A, 0x00, 0x00, 0x94, 0x0B, 0x00, 0x00, 0xFF};

// ntdlib's ArchivePlayer, run directly on kNote with the test archive's bank and wave archive, started with `settings`.
// Variable 0 of the player is set to 1 before driver frame `hold`. The player runs until nothing sounds, or for
// `longest` frames. Strict mixing's samples come as floats, 1/32768 a step.
std::vector<float> DirectNote(bool enhanced, uint64_t hold, uint64_t longest,
                              const ntdlib::ntr::GameSettings& settings = {})
{
    sdat::ArchiveModel model;
    model.sequences = {sdat::SequenceInfo{0, 0, 127, 64, 64, 0, 0, ""}};
    model.banks = {sdat::BankInfo{1, {0, kNone, kNone, kNone}, ""}};
    model.wave_archives = {sdat::WaveArchiveInfo{2, ""}};
    model.players = {sdat::PlayerInfo{1, 0, 0, 0, ""}};
    model.files = {files::Sequence(kNote), files::Bank({{.type = 1, .region = {.wave = 0}}}),
                   files::WaveArchive({kSquare})};
    const std::shared_ptr<const DsArchive> archive = hokuto::song::LoadDsArchive(sdat::BuildArchive(model));

    ntdlib::ntr::ArchivePlayerOptions options{archive->build};
    options.enhanced = enhanced;
    ntdlib::ntr::ArchivePlayer player(archive->archive, options);
    player.Start(ntdlib::ntr::SequenceRef{0}, settings);
    std::vector<float> out;
    std::vector<ntdlib::ntr::SoundOutput> strict;
    for (uint64_t frame = 0; frame < longest && player.Busy(); frame++)
    {
        if (frame == hold)
        {
            ntdlib::ntr::Variables variables;
            variables.local[0] = 1;
            player.Send(ntdlib::ntr::VariableCommands(variables));
        }
        if (enhanced)
        {
            player.RunFrame(out);
        }
        else
        {
            player.RunFrame(strict);
        }
    }
    std::vector<int16_t> samples;
    ntdlib::ntr::AppendPcm16(strict, samples);
    for (const int16_t sample : samples)
    {
        out.push_back(static_cast<float>(sample) / 32768.0f);
    }

    return out;
}

// The output of `preview`, 1,024 samples at a time, until it ends or reaches `most` samples.
std::vector<float> PreviewOutput(hokuto::audio::Renderer& preview, std::size_t most)
{
    std::vector<float> out;
    std::vector<float> block(2 * 1024);
    while (out.size() < 2 * most)
    {
        const std::size_t got = preview.Render(block.data(), 1024);
        if (got == 0)
        {
            break;
        }
        out.insert(out.end(), block.begin(), block.begin() + static_cast<std::ptrdiff_t>(2 * got));
    }

    return out;
}

// The output of ntdlib's ArchivePlayer run directly for `driver_frames` of the driver's frames, with `muted` tracks and
// the game's `variables`, in strict mixing as 16-bit samples or in enhanced mixing as floats.
std::vector<int16_t> DirectStrict(const DsArchive& archive, const ntdlib::ntr::SequenceRef& sequence, int driver_frames,
                                  uint16_t muted = 0, const ntdlib::ntr::GameSettings& settings = {})
{
    ntdlib::ntr::ArchivePlayer player(archive.archive, {archive.build});
    for (int track = 0; track < 16; track++)
    {
        player.MuteTrack(0, track, (muted >> track) & 1);
    }
    player.Start(sequence, settings);
    std::vector<ntdlib::ntr::SoundOutput> out;
    for (int frame = 0; frame < driver_frames; frame++)
    {
        player.RunFrame(out);
    }
    std::vector<int16_t> samples;
    ntdlib::ntr::AppendPcm16(out, samples);

    return samples;
}

std::vector<float> DirectEnhanced(const DsArchive& archive, const ntdlib::ntr::SequenceRef& sequence, int driver_frames)
{
    ntdlib::ntr::ArchivePlayerOptions options{archive.build};
    options.enhanced = true;
    ntdlib::ntr::ArchivePlayer player(archive.archive, options);
    player.Start(sequence);
    std::vector<float> out;
    for (int frame = 0; frame < driver_frames; frame++)
    {
        player.RunFrame(out);
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

// The sounds' numbers and names. Without names in the archive, a sequence gets the name SEQ_ and its number, and a
// sequence of a sequence archive the label of the archive, an underscore and its number.
void TestSounds()
{
    const std::shared_ptr<const DsArchive> archive = TestArchive();

    // A sequence archive's sequences start at kFirstDsArchivedSound, 65,536.
    const uint32_t archived = hokuto::song::kFirstDsArchivedSound;
    Check(archived == 0x10000, "the first number of a sequence archive's sequences");
    Check(hokuto::song::DsSounds(*archive) == std::vector<uint32_t>{0, 1, archived, archived + 2},
          "the sounds with sequences");
    Check(archive->sounds.size() == archived + 3, "a number for each entry");
    Check(hokuto::song::DsSoundName(*archive, 1) == "SEQ_1", "a sequence's name");
    Check(hokuto::song::DsSoundName(*archive, archived + 2) == "SEQARC_0_2", "a sequence archive's sequence's name");
    Check(hokuto::song::DsSoundName(*archive, 9) == "Sound 9", "a number past the sounds");

    // Bytes that end before the ARM7 binary, or that hold no driver, give the default build.
    Check(hokuto::song::DsBuildOf(Bytes(0x20)) == ntdlib::ntr::snd::kDefaultBuild, "a short image");
    Bytes image(0x200);
    xsflib::StoreLe32(&image[0x30], 0x100);
    xsflib::StoreLe32(&image[0x3C], 0x100);
    Check(hokuto::song::DsBuildOf(image) == ntdlib::ntr::snd::kDefaultBuild, "an image without the driver's tables");
    xsflib::StoreLe32(&image[0x3C], 0x1000);
    Check(hokuto::song::DsBuildOf(image) == ntdlib::ntr::snd::kDefaultBuild, "an ARM7 binary past the image's end");
}

// The arrangement of sequence 0, and of the sequence archive's third sequence.
void TestRead()
{
    const std::shared_ptr<const DsArchive> archive = TestArchive();
    const Sequence sequence = hokuto::song::ReadSequence(*archive, 0);

    Check(sequence.name == "SEQ_0" && sequence.sound == 0, "the name and the number");
    Check(sequence.timebase == 48, "48 ticks to a quarter note");
    Check(!sequence.loop, "no loop");
    Check(sequence.notes_end == 48, "the last note ends at tick 48");
    SEQLIB_REQUIRE(sequence.tracks.size() == 2);

    const hokuto::song::Track& first = sequence.tracks[0];
    const hokuto::song::Track& second = sequence.tracks[1];
    SEQLIB_REQUIRE(first.notes.size() == 1 && second.notes.size() == 1);
    Check(first.number == 0 && second.number == 1, "the tracks' numbers");
    Check(first.notes[0].start == 0 && first.notes[0].length == 48 && first.notes[0].key == 60 &&
              first.notes[0].velocity == 127 && first.notes[0].command == 12,
          "track 0's note and its command");
    Check(second.notes[0].start == 24 && second.notes[0].length == 24 && second.notes[0].key == 67 &&
              second.notes[0].velocity == 80 && second.notes[0].command == 20,
          "track 1's note and its command");
    Check(first.program == 0 && first.bank == "BANK_0", "the instrument");

    // Track 0 sets volume 100 and pan -32 at tick 0. Track 1 sets volume 64 at tick 0.
    Check(first.volume == std::vector<hokuto::song::Setting>{{0, 100}}, "track 0's volume");
    Check(first.pan == std::vector<hokuto::song::Setting>{{0, -32}}, "track 0's pan");
    Check(second.volume == std::vector<hokuto::song::Setting>{{0, 64}} && second.pan.empty(), "track 1's mix");
    Check(std::ranges::all_of(first.sends, [](const auto& sends) { return sends.empty(); }), "no sends");

    // Each track's commands run in one stretch from its start to its end. The end runs after the note's wait.
    SEQLIB_REQUIRE(first.stretches.size() == 1 && second.stretches.size() == 1);
    Check(first.stretches[0].tick == 0 && first.stretches[0].first == 3 && first.stretches[0].last == 15,
          "track 0's stretch");
    Check(second.stretches[0].tick == 0 && second.stretches[0].first == 16 && second.stretches[0].last == 23,
          "track 1's stretch");

    const Sequence archived = hokuto::song::ReadSequence(*archive, hokuto::song::kFirstDsArchivedSound + 2);
    SEQLIB_REQUIRE(archived.tracks.size() == 1 && archived.tracks[0].notes.size() == 1);
    Check(archived.tracks[0].notes[0].key == 67 && archived.tracks[0].notes[0].command == 4 &&
              archived.tracks[0].notes[0].length == 24,
          "the archived sequence's note, at its offset in the archive's bytecode");

    const bool refused =
        !xsflib::testing::ErrorOf<std::runtime_error>([&] { hokuto::song::ReadSequence(*archive, 4); }).empty();
    Check(refused, "an entry without a sequence can't be read");
}

// Three notes on channel 4 alone, the first in the order of the driver. The player allows its sequences that channel
// only.
//
//    0: C7 00     note wait off
//    2: 3C 7F 30  note 60 for 48 ticks
//    5: 80 18     wait 24
//    7: 3E 7F 18  note 62 for 24 ticks
//   10: C6 00     priority 0
//   12: 40 7F 18  note 64 for 24 ticks
//   15: 80 30     wait 48
//   17: FF        end
//
// The driver takes the channel of the lowest priority for a note whose priority is as high: the player's priority plus
// the track's. Note 62 takes the channel from note 60 at tick 24, and note 60 ends there. The track's priority then
// falls by 64, and note 64 gets no channel. The track lists it among the notes that the driver never started.
void TestChannels()
{
    const files::Wave square{
        .loop = 1,
        .loop_length = 4,
        .data = {0x40, 0x40, 0xC0, 0xC0, 0x40, 0x40, 0xC0, 0xC0, 0x40, 0x40, 0xC0, 0xC0, 0x40, 0x40, 0xC0, 0xC0}};
    sdat::ArchiveModel model;
    model.sequences = {sdat::SequenceInfo{0, 0, 127, 64, 64, 0, 0, ""}};
    model.banks = {sdat::BankInfo{1, {0, kNone, kNone, kNone}, ""}};
    model.wave_archives = {sdat::WaveArchiveInfo{2, ""}};
    model.players = {sdat::PlayerInfo{1, 0, 0x0010, 0, ""}};
    model.files = {files::Sequence({0xC7, 0x00, 0x3C, 0x7F, 0x30, 0x80, 0x18, 0x3E, 0x7F, 0x18, 0xC6, 0x00, 0x40, 0x7F,
                                    0x18, 0x80, 0x30, 0xFF}),
                   files::Bank({{.type = 1, .region = {.wave = 0}}}), files::WaveArchive({square})};
    const Sequence sequence = hokuto::song::ReadSequence(*hokuto::song::LoadDsArchive(sdat::BuildArchive(model)), 0);

    SEQLIB_REQUIRE(sequence.tracks.size() == 1);
    const hokuto::song::Track& track = sequence.tracks[0];
    SEQLIB_REQUIRE(track.notes.size() == 2);
    Check(track.notes[0].start == 0 && track.notes[0].length == 24 && track.notes[0].key == 60 && track.notes[0].cut,
          "note 62 cuts note 60 short at tick 24");
    Check(track.notes[1].start == 24 && track.notes[1].length == 24 && track.notes[1].key == 62 && !track.notes[1].cut,
          "note 62 plays its length");
    Check(track.notes[0].psg == 0 && track.notes[1].psg == 0, "a wave takes no PSG channel");
    SEQLIB_REQUIRE(track.unstarted.size() == 1);
    const hokuto::song::Note& unstarted = track.unstarted[0];
    Check(unstarted.start == 24 && unstarted.length == 24 && unstarted.key == 64 && unstarted.velocity == 127 &&
              unstarted.command == 12,
          "note 64 gets no channel");
}

// Bytes of a sequence of one track: `program` with the note wait off, the notes of `keys` at tick 0 and a note of
// `late` at tick 12, each for 48 ticks.
Bytes PsgNotes(uint8_t program, const std::vector<uint8_t>& keys, std::optional<uint8_t> late = std::nullopt)
{
    Bytes bytes = {0xC7, 0x00, 0x81, program};
    for (const uint8_t key : keys)
    {
        bytes.insert(bytes.end(), {key, 0x7F, 0x30});
    }
    if (late)
    {
        bytes.insert(bytes.end(), {0x80, 0x0C, *late, 0x7F, 0x30});
    }
    bytes.insert(bytes.end(), {0x80, 0x60, 0xFF});

    return bytes;
}

// A bank's programs: 0 a square wave, 1 noise, 2 a wave, 3 a drum set of a square wave, noise and a wave from key 60,
// and 4 a key split with a square wave up to key 60 and a wave above it.
Bytes PsgBank()
{
    const files::Region square{.type = 2, .wave = 3};
    const files::Region noise{.type = 3};
    const files::Region wave{.type = 1, .wave = 0};
    return files::Bank({{.type = 2, .region = square},
                        {.type = 3, .region = noise},
                        {.type = 1, .region = wave},
                        {.type = 16, .min_key = 60, .regions = {square, noise, wave}},
                        {.type = 17, .split_keys = {60, 127}, .regions = {square, wave}}});
}

// An archive of `sequences`, each on its player. Player 0 uses every channel, and player 1 channels 8 to 15.
std::vector<uint8_t> PsgArchive(const std::vector<Bytes>& sequences, const Bytes& waves, uint8_t priority = 64,
                                uint8_t volume = 127)
{
    sdat::ArchiveModel model;
    const auto count = static_cast<uint32_t>(sequences.size());
    for (uint32_t i = 0; i < count; i++)
    {
        model.sequences.push_back(
            sdat::SequenceInfo{i, 0, volume, priority, 64, static_cast<uint8_t>(i == 1 ? 1 : 0), 0, ""});
        model.files.push_back(files::Sequence(sequences[i]));
    }
    model.banks = {sdat::BankInfo{count, {0, kNone, kNone, kNone}, ""}};
    model.wave_archives = {sdat::WaveArchiveInfo{count + 1, ""}};
    model.players = {sdat::PlayerInfo{1, 0, 0, 0, ""}, sdat::PlayerInfo{1, 0, 0xFF00, 0, ""}};
    model.files.push_back(PsgBank());
    model.files.push_back(waves);

    return sdat::BuildArchive(model);
}

std::vector<uint8_t> PsgChannelsOf(const hokuto::song::Track& track)
{
    std::vector<uint8_t> channels;
    for (const hokuto::song::Note& note : track.notes)
    {
        channels.push_back(note.psg);
    }

    return channels;
}

// A note of a square wave uses one of channels 8 to 13, and a note of noise channel 14 or 15. A wave on those channels
// isn't a PSG voice. A drum set and a key split count by the note's key.
void TestPsgChannels()
{
    const auto read = [](const Bytes& sequence, uint32_t sound = 0)
    {
        const std::vector<Bytes> sequences =
            sound == 1 ? std::vector<Bytes>{{0xFF}, sequence} : std::vector<Bytes>{sequence};
        const Sequence played = hokuto::song::ReadSequence(
            *hokuto::song::LoadDsArchive(PsgArchive(sequences, files::WaveArchive({kSquare}))), sound);
        return played.tracks.at(0);
    };

    std::vector<uint8_t> channels = PsgChannelsOf(read(PsgNotes(0, {60, 61, 62, 63, 64, 65})));
    std::ranges::sort(channels);
    Check(channels == std::vector<uint8_t>{1, 2, 3, 4, 5, 6}, "six square waves take channels 8 to 13");
    channels = PsgChannelsOf(read(PsgNotes(1, {60, 61})));
    std::ranges::sort(channels);
    Check(channels == std::vector<uint8_t>{7, 8}, "two notes of noise take channels 14 and 15");

    const hokuto::song::Track wave = read(PsgNotes(2, {60}), 1);
    Check(PsgChannelsOf(wave) == std::vector<uint8_t>{0}, "a wave on channels 8 to 15 is no PSG voice");

    const hokuto::song::Track drums = read(PsgNotes(3, {60, 61, 62}));
    SEQLIB_REQUIRE(drums.notes.size() == 3);
    Check(drums.notes[0].psg >= 1 && drums.notes[0].psg <= 6, "a drum set's key plays a square wave");
    Check(drums.notes[1].psg >= 7, "a drum set's key plays noise");
    Check(drums.notes[2].psg == 0, "a drum set's key plays a wave");

    const hokuto::song::Track split = read(PsgNotes(4, {60, 61}));
    SEQLIB_REQUIRE(split.notes.size() == 2);
    Check(split.notes[0].psg >= 1 && split.notes[0].psg <= 6, "a key split's first run plays a square wave");
    Check(split.notes[1].psg == 0, "a key split's second run plays a wave");
    Check(!read(PsgNotes(0, {60}), 1).notes.empty(), "a square wave plays on a player of channels 8 to 15");
}

// A seventh square wave takes the channel of a note that still sounds. The sounds that an edit can change count by the
// bytes of their files.
void TestDsCollisions()
{
    using hokuto::song::AddedCollisions;
    using hokuto::song::Collisions;

    const Bytes waves = files::WaveArchive({kSquare});
    const Bytes clear = PsgNotes(0, {60, 61, 62, 63, 64, 65});
    const Bytes crowded = PsgNotes(0, {60, 61, 62, 63, 64, 65}, 66);
    const auto load = [](const std::vector<uint8_t>& bytes)
    {
        return hokuto::song::LoadDsArchive(bytes);
    };
    const auto before = load(PsgArchive({clear, clear}, waves));
    const auto after = load(PsgArchive({crowded, clear}, waves));

    Check(hokuto::song::CollisionsOf(hokuto::song::ReadSequence(*before, 0)) == Collisions{0, 0},
          "six square waves fit");
    Check(hokuto::song::CollisionsOf(hokuto::song::ReadSequence(*after, 0)) == Collisions{1, 0},
          "a seventh cuts one short");
    Check(hokuto::song::AddedDsCollisions(*before, *after, {0, 1}) == std::vector<AddedCollisions>{{0, {1, 0}}},
          "the seventh note adds a cut note to sound 0");
    Check(hokuto::song::AddedDsCollisions(*after, *before, {0, 1}).empty(),
          "an edit that clears a collision adds none");

    Check(hokuto::song::DsSoundsThatChange(*before, *before).empty(), "the same archive changes no sound");
    Check(hokuto::song::DsSoundsThatChange(*before, *after) == std::vector<uint32_t>{0},
          "only sound 0 takes other commands");

    files::Wave other = kSquare;
    other.data[0] = 0x20;
    const auto other_waves = load(PsgArchive({clear, clear}, files::WaveArchive({other})));
    Check(hokuto::song::DsSoundsThatChange(*before, *other_waves) == std::vector<uint32_t>{0, 1},
          "another wave archive changes the sounds of its bank");
    const auto other_priority = load(PsgArchive({clear, clear}, waves, 70));
    Check(hokuto::song::DsSoundsThatChange(*before, *other_priority) == std::vector<uint32_t>{0, 1},
          "a channel priority can change the notes' channels");
    const auto other_volume = load(PsgArchive({clear, clear}, waves, 64, 100));
    Check(hokuto::song::DsSoundsThatChange(*before, *other_volume).empty(), "a volume leaves the channels alone");
}

// Variable 0 starts at -1. Its comparison with 1 has two outcomes: -1 and 1 stand for them. With 1, the track jumps
// over note 60, and note 67 plays at tick 0.
void TestVariables()
{
    const std::shared_ptr<const DsArchive> archive = TestArchive();
    const Sequence plain = hokuto::song::ReadSequence(*archive, 1);
    const Sequence chosen = hokuto::song::ReadSequence(*archive, 1, {{0, 1}});

    SEQLIB_REQUIRE(plain.variables.size() == 1);
    Check(plain.variables[0].number == 0 && plain.variables[0].choices == std::vector<int>{-1, 1} &&
              !plain.variables[0].written,
          "variable 0 and its choices");
    SEQLIB_REQUIRE(plain.read_variables.size() == 1);
    Check(plain.read_variables[0].number == 0, "the variable that a change can set");
    SEQLIB_REQUIRE(plain.tracks.size() == 1 && plain.tracks[0].notes.size() == 2);
    Check(plain.tracks[0].notes[0].key == 60 && plain.tracks[0].notes[1].start == 24, "both notes without the value");
    SEQLIB_REQUIRE(chosen.tracks.size() == 1 && chosen.tracks[0].notes.size() == 1);
    Check(chosen.tracks[0].notes[0].key == 67 && chosen.tracks[0].notes[0].start == 0, "note 67 alone with 1");
    Check(chosen.start_values == StartValues{{0, 1}}, "the starting value");

    // The model plays the value as the settings of the game set it at the start.
    const auto model = std::make_shared<SequenceModel>(archive, std::make_shared<const Sequence>(chosen));
    const Played played = Play(*model, 200);
    ntdlib::ntr::GameSettings settings;
    settings.variables.local[0] = 1;
    const std::vector<int16_t> expected = DirectStrict(*archive, {1}, 400, 0, settings);
    const std::vector<int16_t> without = DirectStrict(*archive, {1}, 400);
    SEQLIB_REQUIRE(played.samples.size() > 20 * 682 && played.samples.size() < expected.size());
    Check(std::equal(played.samples.begin(), played.samples.end(), expected.begin()), "the value's output");
    Check(!std::equal(played.samples.begin(), played.samples.end(), without.begin()), "another output without it");
    Check(model->Variables()[0] == 1, "the variable's value");
    Check(model->Variables()[1] == -1 && model->Variables()[16] == -1, "the others start at -1");
}

// A sequence that plays a note in each pass of 24 ticks where variable 0 is 1. A change while it plays sets the
// variable. The model applies the change before its frame, and the driver before the first of the two driver frames in
// that frame. Ticks run in each of the model's frames. The change at frame 96 therefore starts the notes at the pass
// from tick 96 on.
void TestChanges()
{
    sdat::ArchiveModel source;
    source.sequences = {sdat::SequenceInfo{0, 0, 127, 64, 64, 0, 0, ""}};
    source.banks = {sdat::BankInfo{1, {0, kNone, kNone, kNone}, ""}};
    source.wave_archives = {sdat::WaveArchiveInfo{2, ""}};
    source.players = {sdat::PlayerInfo{1, 0, 0, 0, ""}};
    source.files = {
        files::Sequence({0xB8, 0x00, 0x01, 0x00, 0xA2, 0x3C, 0x7F, 0x18, 0x80, 0x18, 0x94, 0x00, 0x00, 0x00}),
        files::Bank({{.type = 1, .region = {.wave = 0}}}), files::WaveArchive({kSquare})};
    const std::shared_ptr<const DsArchive> archive = hokuto::song::LoadDsArchive(sdat::BuildArchive(source));
    const Sequence plain = hokuto::song::ReadSequence(*archive, 0);
    SEQLIB_REQUIRE(plain.tracks.size() == 1);
    Check(plain.tracks[0].notes.empty(), "no note without the change");

    const auto time =
        static_cast<uint32_t>(96 * hokuto::song::kDsFrameMs.numerator / hokuto::song::kDsFrameMs.denominator + 1);
    const uint64_t model_frame = hokuto::song::ChangeFrame(time, hokuto::song::kDsFrameMs);
    const hokuto::song::Changes changes{{time, 0, 1}};
    const Sequence changed = hokuto::song::ReadSequence(*archive, 0, {}, changes);
    Check(changed.changes == changes, "the sequence keeps its change");
    SEQLIB_REQUIRE(changed.tracks.size() == 1 && !changed.tracks[0].notes.empty());
    Check(changed.tracks[0].notes[0].start >= 97 && changed.tracks[0].notes[0].start <= 120,
          "the first note after the change");
    Check(changed.loop && changed.loop->start > 96, "the loop starts after the change");

    // The model plays the change as the driver applies it directly.
    const auto model = std::make_shared<SequenceModel>(archive, std::make_shared<const Sequence>(changed));
    const Played played = Play(*model, 200);
    ntdlib::ntr::ArchivePlayer player(archive->archive, {.build = archive->build});
    player.Start(*archive->sounds[0]);
    std::vector<int16_t> expected;
    std::vector<ntdlib::ntr::SoundOutput> strict;
    for (uint64_t frame = 0; frame < 2 * 200; frame++)
    {
        if (frame == 2 * model_frame)
        {
            ntdlib::ntr::Variables variables;
            variables.local[0] = 1;
            player.Send(ntdlib::ntr::VariableCommands(variables));
        }
        strict.clear();
        player.RunFrame(strict);
        ntdlib::ntr::AppendPcm16(strict, expected);
    }
    SEQLIB_REQUIRE(played.samples.size() > 150 * 682 && played.samples.size() <= expected.size());
    Check(std::equal(played.samples.begin(), played.samples.end(), expected.begin()), "the change's output");
    const std::span<const int16_t> samples(played.samples);
    Check(HasSound(samples.subspan(120 * 682)), "the note sounds after the change");
    Check(!HasSound(samples.first(90 * 682)), "and not before it");
    Check(model->Variables()[0] == 1, "the variable's value after the change");
}

// The strict mode's frames hold 341 samples at 32,728 Hz, and the enhanced mode's 500 at 47,989 Hz. Each lasts two of
// the driver's frames: 349,184 / 33,513,982 s, about 10.42 ms. The first driver frame lasts 171 samples, or 250. Its
// samples come before the sequence starts, and are silent. A tick plays in each of the model's frames. The note of 48
// ticks and its release end within 50 frames, and the output ends with silence after them. With track 0 muted, the
// output stays silent until track 1's note sounds in frame 25.
void TestPlays()
{
    const std::shared_ptr<const DsArchive> archive = TestArchive();
    const auto sequence = std::make_shared<const Sequence>(hokuto::song::ReadSequence(*archive, 0));
    const auto model = std::make_shared<SequenceModel>(
        archive, sequence, hokuto::song::ModelOptions{.mixing = Mixing::kStrict, .snapshot_interval = 20});
    const Played played = Play(*model, 400);
    const std::vector<int16_t> expected = DirectStrict(*archive, {0}, 800);
    const std::size_t frame = 2 * model->FrameSamples();
    const bool sought = model->SeekFrame(30);
    const std::vector<int16_t> after_seek(model->Output().begin(), model->Output().end());

    Check(model->SampleRate() == 32728 && model->FrameSamples() == 341, "the strict mode's frames");
    Check(model->TicksPerFrame(0.0) > 0.999 && model->TicksPerFrame(0.0) < 1.001, "a tick in each frame");
    SEQLIB_REQUIRE(played.samples.size() > 48 * frame && played.samples.size() < 100 * frame);
    Check(!HasSound(std::span<const int16_t>(played.samples).first(frame)), "frame 0 is silent");
    Check(HasSound(std::span<const int16_t>(played.samples).subspan(frame, frame)), "the note sounds in frame 1");
    Check(std::equal(played.samples.begin(), played.samples.end(), expected.begin()), "the reference's samples");
    Check(!HasSound(std::span<const int16_t>(expected).subspan(played.samples.size())), "silence after the end");
    Check(played.after[0] == 1.0 && played.after[9] == 10.0, "the position after each frame");
    Check(sought && after_seek.size() == frame &&
              std::equal(after_seek.begin(), after_seek.end(), expected.begin() + 30 * frame),
          "a seek gives the reference's frame");

    // A muted track's notes leave the output.
    const auto muted = std::make_shared<SequenceModel>(
        archive, sequence, hokuto::song::ModelOptions{.mixing = Mixing::kStrict, .snapshot_interval = 20});
    muted->SetMutes(1);
    const Played muted_played = Play(*muted, 400);
    const std::vector<int16_t> muted_expected = DirectStrict(*archive, {0}, 800, 1);
    SEQLIB_REQUIRE(muted_played.samples.size() > 48 * frame && muted_played.samples.size() <= muted_expected.size());
    Check(std::equal(muted_played.samples.begin(), muted_played.samples.end(), muted_expected.begin()),
          "the muted output");
    Check(!HasSound(std::span<const int16_t>(muted_played.samples).first(25 * frame)), "silence until track 1's note");
    Check(HasSound(std::span<const int16_t>(muted_played.samples).subspan(25 * frame, frame)), "track 1's note");

    // The enhanced mode plays the same sequence at its rate.
    const auto enhanced = std::make_shared<SequenceModel>(
        archive, sequence, hokuto::song::ModelOptions{.mixing = Mixing::kEnhanced, .snapshot_interval = 20});
    const Played enhanced_played = Play(*enhanced, 400);
    const std::vector<float> enhanced_expected = DirectEnhanced(*archive, {0}, 800);
    Check(enhanced->SampleRate() == 47989 && enhanced->FrameSamples() == 500, "the enhanced mode's frames");
    SEQLIB_REQUIRE(enhanced_played.floats.size() > 48 * 1000 && enhanced_played.floats.size() < 100 * 1000);
    Check(!HasSound(std::span<const float>(enhanced_played.floats).first(500)), "the first driver frame is silent");
    Check(std::equal(enhanced_played.floats.begin(), enhanced_played.floats.end(), enhanced_expected.begin()),
          "the enhanced mode's samples");
    Check(enhanced_played.after[9] == 10.0, "the enhanced mode's position");
}

// A track's meter adds up the output of its notes on the output's scale. The output's 10-bit samples are 64 steps apart
// there. While track 0's note plays alone, from frame 1 to frame 24, its meter is within 64 of the output's peaks, and
// the meter of track 1 stays at 0.
void TestMeters()
{
    const std::shared_ptr<const DsArchive> archive = TestArchive();
    const auto sequence = std::make_shared<const Sequence>(hokuto::song::ReadSequence(*archive, 0));
    SequenceModel model(archive, sequence);
    bool near = true;
    bool quiet = true;
    int frame = 0;
    for (bool more = model.SeekFrame(0); more && frame < 25; more = model.RunFrame(), frame++)
    {
        int32_t peak = 0;
        for (const int16_t sample : model.Output())
        {
            peak = std::max(peak, std::abs(static_cast<int32_t>(sample)));
        }
        const std::array<int32_t, 2> meter = model.TrackPeaks(0);
        near = near && (frame == 0 || std::abs(std::max(meter[0], meter[1]) - peak) < 64);
        quiet = quiet && model.TrackPeaks(1) == std::array<int32_t, 2>{};
    }

    Check(frame == 25, "the frames play");
    Check(near, "track 0's meter follows the output's peaks");
    Check(quiet, "track 1's meter stays at 0");
}

// The reader reads a DS sound away from the program's thread, with the archive.
void TestReader()
{
    const std::shared_ptr<const DsArchive> archive = TestArchive();
    SequenceReader reader;
    reader.Request(archive, 1, {{0, 1}});
    std::optional<SequenceReader::Result> read;
    read = TakeRead(reader);

    Check(read && read->sequence && *read->sequence == hokuto::song::ReadSequence(*archive, 1, {{0, 1}}),
          "the read sequence");
    Check(read && read->ds == archive && !read->archive && !read->gba, "the archive");

    // A conversion that fails leaves the sound on the DS, with the conversion's message.
    reader.Request(archive, 1, {{0, 1}}, {},
                   []() -> SequenceReader::EngineSound { throw std::invalid_argument("No."); });
    read.reset();
    read = TakeRead(reader);
    Check(read && read->sequence && read->engine_error == "No." && !read->engine, "the conversion's message");

    // A conversion for the DS plays on the DS's model. Its sequence contains the map of the arrangement's ticks.
    reader.Request(archive, 1, {{0, 1}}, {},
                   [&]() { return SequenceReader::EngineSound{.played = archive, .sound = 0}; });
    read.reset();
    read = TakeRead(reader);
    std::optional<hokuto::song::Sequence> without_map;
    if (read && read->engine_sequence)
    {
        without_map = *read->engine_sequence;
        without_map->arrangement_ticks.reset();
    }
    Check(read && read->engine && read->engine->played.Ds() == archive && read->engine_sequence &&
              read->engine_sequence->arrangement_ticks &&
              without_map == hokuto::song::ReadSequence(*archive, 0, {{0, 1}}),
          "the conversion's sequence");

    // A sound that is missing from the archive returns no sequence, and the archive. The program closes the sequence
    // then.
    reader.Request(archive, static_cast<uint32_t>(archive->sounds.size()), {});
    read.reset();
    read = TakeRead(reader);
    Check(read && !read->sequence && read->ds == archive && !read->error.empty(), "the archive without the sound");
}

// Sequence 1 exports as MIDI with SoundFont 2 and SFZ as ntdlib's converter writes it. Without a value, its notes 60
// and 67 start at ticks 0 and 24. With variable 0 at 1, note 67 alone starts at tick 0.
void TestMidi()
{
    namespace ntr_export = ntdlib::sequence_export::ntr;
    const std::shared_ptr<const DsArchive> archive = TestArchive();
    const auto direct = [&archive](const std::map<uint8_t, int16_t>& variables)
    {
        ntr_export::PerformOptions options;
        options.build = archive->build;
        options.variables = variables;
        return ntr_export::ConvertSequence(archive->archive, {1}, "SEQ_1", options, "Hokuto");
    };
    const auto same = [](const ntdlib::sequence_export::Conversion& conversion, const ntr_export::Conversion& expected)
    {
        return conversion.midi == expected.midi && conversion.sf2 == expected.sf2 &&
               conversion.sfz.files.size() == expected.sfz.files.size() &&
               conversion.sfz.samples.size() == expected.sfz.samples.size() && conversion.samples == expected.samples &&
               conversion.channels == expected.channels && conversion.warnings == expected.warnings;
    };
    const ntdlib::sequence_export::Conversion plain = hokuto::song::ConvertToMidi(*archive, 1, {}, "Hokuto");
    const ntdlib::sequence_export::Conversion chosen = hokuto::song::ConvertToMidi(*archive, 1, {{0, 1}}, "Hokuto");

    Check(same(plain, direct({})), "the converter's files");
    Check(same(chosen, direct({{0, 1}})), "the converter's files with the value");
    Check(!plain.sf2.empty() && !plain.sfz.files.empty(), "a SoundFont and SFZ files");

    const seqlib::midi::File file = seqlib::midi::Read(plain.midi);
    const std::vector<seqlib::midi::Note> notes = seqlib::midi::Notes(file);
    const std::vector<seqlib::midi::Note> chosen_notes = seqlib::midi::Notes(seqlib::midi::Read(chosen.midi));
    SEQLIB_REQUIRE(notes.size() == 2 && chosen_notes.size() == 1);
    Check(notes[0].key == 60 && notes[0].on == 0 && notes[1].key == 67 && notes[1].on == 24u * file.division / 48,
          "both notes without the value");
    Check(chosen_notes[0].key == 67 && chosen_notes[0].on == 0, "note 67 alone with the value");

    const bool refused =
        !xsflib::testing::ErrorOf<std::runtime_error>([&] { hokuto::song::ConvertToMidi(*archive, 4, {}, "Hokuto"); })
             .empty();
    Check(refused, "an entry without a sequence doesn't convert");
}

// A note of program 0 plays by itself as ntdlib's ArchivePlayer plays kNote: key 60 at velocity 100, at volume 127 in
// the centre, tied, held for the 191 whole frames of the driver in a second, then let go. The output is the same in
// both mixing modes, and the preview ends with the note's release. A note of the Key Editor holds until its flag lets
// it go. KeyOf finds the nearest key whose region plays something: the lowest key of a drum set from key 62, the last
// key of a key split's one region up to key 50, and key 60 of a PSG instrument. A wave that is missing from the wave
// archive plays at no key, and neither does an empty program or a program past the bank's instruments.
void TestNotePreview()
{
    using hokuto::song::DsNotePreview;

    const std::shared_ptr<const DsArchive> archive = TestArchive();
    DsNotePreview strict(*archive, 0, 0);
    DsNotePreview enhanced(*archive, 0, 0, Mixing::kEnhanced);
    const auto release = std::make_shared<std::atomic<bool>>(false);
    DsNotePreview held(*archive, 0, 0, 64, 90, release);

    const uint64_t hold = DsNotePreview::HoldFrames();
    const uint64_t longest = DsNotePreview::LongestFrames();
    const std::vector<float> strict_played = PreviewOutput(strict, longest * 512);
    const std::vector<float> enhanced_played = PreviewOutput(enhanced, longest * 512);
    const std::vector<float> before = PreviewOutput(held, 2 * hold * 171);
    release->store(true);
    const std::vector<float> after = PreviewOutput(held, longest * 512);

    Check(hold == 191 && longest == 1919, "the hold and the limit count the driver's frames");
    Check(strict.SampleRate() == 32728, "the strict preview plays at the DS's rate");
    Check(enhanced.SampleRate() == static_cast<int>(std::lround(ntdlib::ntr::kEnhancedRate)),
          "the enhanced preview plays at the enhanced mode's rate");
    Check(HasSound(std::span<const float>(strict_played)) && strict_played == DirectNote(false, hold, longest),
          "the strict preview plays the note written out by hand");
    Check(HasSound(std::span<const float>(enhanced_played)) && enhanced_played == DirectNote(true, hold, longest),
          "the enhanced preview plays the note written out by hand");
    Check(strict_played.size() < 2 * (hold + 100) * 171, "the preview ends with the note's release");
    Check(before.size() >= 2 * hold * 170 && HasSound(std::span<const float>(before).last(2048)),
          "the Key Editor's note holds until its flag lets it go");
    Check(!after.empty() && after.size() < 2 * 100 * 171, "the Key Editor's note ends once let go");

    sdat::ArchiveModel model;
    model.banks = {sdat::BankInfo{0, {0, kNone, kNone, kNone}, ""}};
    model.wave_archives = {sdat::WaveArchiveInfo{1, ""}};
    const files::Region pcm{.wave = 0};
    model.files = {files::Bank({{.type = 16, .min_key = 62, .regions = {pcm, pcm}},
                                {.type = 17, .split_keys = {50}, .regions = {pcm}},
                                {.type = 2, .region = {.type = 2, .wave = 3}},
                                {.type = 1, .region = {.wave = 5}},
                                {}}),
                   files::WaveArchive({kSquare})};
    const std::shared_ptr<const DsArchive> keys = hokuto::song::LoadDsArchive(sdat::BuildArchive(model));
    Check(DsNotePreview::KeyOf(*archive, 0, 0) == 60, "a wave plays at key 60");
    Check(DsNotePreview::KeyOf(*keys, 0, 0) == 62, "a drum set plays at its nearest key");
    Check(DsNotePreview::KeyOf(*keys, 0, 1) == 50, "a key split plays at its nearest key");
    Check(DsNotePreview::KeyOf(*keys, 0, 2) == 60, "a PSG instrument plays at key 60");
    Check(DsNotePreview::KeyOf(*keys, 0, 3) == -1, "a wave past the wave archive plays at no key");
    Check(DsNotePreview::KeyOf(*keys, 0, 4) == -1, "an empty program plays at no key");
    Check(DsNotePreview::KeyOf(*keys, 0, 5) == -1, "a program past the bank plays at no key");
    Check(DsNotePreview::KeyOf(*keys, 1, 0) == -1, "a bank past the archive plays at no key");
    DsNotePreview silent(*keys, 0, 3);
    Check(PreviewOutput(silent, 2048).empty(), "an instrument that plays at no key plays nothing");
}

// The test archive in file `path` of a cartridge image of game `code`. The program loads such an archive the same way:
// the game comes from the image (DsGameOf). `first_name` names the archive's first sequence (TestArchiveBytes). A file
// of 0xFF bytes after a byte of 1 comes last in the image. Like the image that a .hok file keeps, the image leaves the
// 0xFF bytes out.
std::shared_ptr<const DsArchive> ArchiveOfGame(const std::string& code,
                                               const std::string& path = "sound/sound_data.sdat",
                                               const std::string& first_name = "")
{
    namespace nds_files = ntdlib::testing::nds_files;
    const Bytes archive = TestArchiveBytes(first_name);
    nds_files::Image spec;
    spec.game_code = code;
    spec.files = {{path, archive}, {"sound/zz.bin", {0x01, 0xFF, 0xFF, 0xFF}}};
    const auto [image, ids] = nds_files::BuildImage(spec);
    const std::size_t kept = image.size() - 3;
    const std::optional<hokuto::song::DsGame> game =
        hokuto::song::DsGameOf(std::span(image).first(kept), static_cast<uint32_t>(image.size()), ids[0], 0);

    return hokuto::song::LoadDsArchive(archive, ntdlib::ntr::snd::kDefaultBuild, game);
}

// Games A2DE and AMCE start the SDK's output effect at boot, in the surround mode with the values of
// ntdlib::ntr::OutputEffect's defaults. Every sound of theirs plays through it. In strict mixing, the model has to
// match ntdlib's ArchivePlayer, run directly with OutputEffect{}, from the start and after a seek. So does the
// browser's note preview. The effect's channels play the mixer's output a half buffer later, and the output has to
// differ from the plain run. The enhanced mode, the dry render and an image of another game play as before.
void TestOutputEffect()
{
    const std::shared_ptr<const DsArchive> surround = ArchiveOfGame("A2DE");
    const std::shared_ptr<const DsArchive> other = ArchiveOfGame("ATST");
    SEQLIB_REQUIRE(surround->game && other->game);
    Check(*surround->game == hokuto::song::DsGame{"A2DE", "sound/sound_data.sdat", 0},
          "the game of an image without its last 0xFF bytes");
    Check(hokuto::song::DsGameSettingsOf(*surround).effect.has_value() &&
              hokuto::song::DsGameSettingsOf(*ArchiveOfGame("AMCE")).effect.has_value(),
          "both games start the output effect");
    Check(!hokuto::song::DsGameSettingsOf(*other).effect && !hokuto::song::DsGameSettingsOf(*TestArchive()).effect,
          "another game and an archive without a game start none");
    Check(hokuto::song::DsProfileOf(*surround, 0).settings.effect.has_value() &&
              !hokuto::song::DsProfileOf(*surround, 4).settings.effect,
          "a sound with a sequence plays through the effect");

    ntdlib::ntr::GameSettings effect;
    effect.effect = ntdlib::ntr::OutputEffect{};
    const std::vector<int16_t> expected = DirectStrict(*surround, {0}, 800, 0, effect);
    const std::vector<int16_t> plain = DirectStrict(*surround, {0}, 800);
    const auto sequence = std::make_shared<const Sequence>(hokuto::song::ReadSequence(*surround, 0));
    const auto model = std::make_shared<SequenceModel>(
        surround, sequence, hokuto::song::ModelOptions{.mixing = Mixing::kStrict, .snapshot_interval = 20});
    const Played played = Play(*model, 400);
    const std::size_t frame = 2 * model->FrameSamples();
    const bool sought = model->SeekFrame(30);
    const std::vector<int16_t> after_seek(model->Output().begin(), model->Output().end());

    SEQLIB_REQUIRE(played.samples.size() > 48 * frame && played.samples.size() <= expected.size());
    Check(expected != plain, "the effect changes the output");
    Check(std::equal(played.samples.begin(), played.samples.end(), expected.begin()), "the output through the effect");
    Check(!HasSound(std::span<const int16_t>(expected).subspan(played.samples.size())),
          "silence after the effect's end");
    Check(sought && after_seek.size() == frame &&
              std::equal(after_seek.begin(), after_seek.end(), expected.begin() + 30 * frame),
          "a seek through the effect");

    // The dry render and another game's image play without the effect.
    const auto dry = std::make_shared<SequenceModel>(
        surround, sequence,
        hokuto::song::ModelOptions{.mixing = Mixing::kStrict, .snapshot_interval = 20, .dry = true});
    const Played dry_played = Play(*dry, 400);
    const auto other_model =
        std::make_shared<SequenceModel>(other, std::make_shared<const Sequence>(hokuto::song::ReadSequence(*other, 0)),
                                        hokuto::song::ModelOptions{.mixing = Mixing::kStrict, .snapshot_interval = 20});
    const Played other_played = Play(*other_model, 400);
    SEQLIB_REQUIRE(dry_played.samples.size() > 48 * frame && other_played.samples.size() > 48 * frame);
    Check(std::equal(dry_played.samples.begin(), dry_played.samples.end(), plain.begin()) &&
              !HasSound(std::span<const int16_t>(plain).subspan(dry_played.samples.size())),
          "the dry render plays without the effect");
    Check(std::equal(other_played.samples.begin(), other_played.samples.end(), plain.begin()) &&
              !HasSound(std::span<const int16_t>(plain).subspan(other_played.samples.size())),
          "another game plays without the effect");

    // The enhanced mode leaves the effect out.
    const auto enhanced = std::make_shared<SequenceModel>(
        surround, sequence, hokuto::song::ModelOptions{.mixing = Mixing::kEnhanced, .snapshot_interval = 20});
    const Played enhanced_played = Play(*enhanced, 400);
    const std::vector<float> enhanced_expected = DirectEnhanced(*surround, {0}, 800);
    SEQLIB_REQUIRE(enhanced_played.floats.size() > 48 * 1000);
    Check(std::equal(enhanced_played.floats.begin(), enhanced_played.floats.end(), enhanced_expected.begin()),
          "the enhanced mode plays without the effect");

    // The note preview plays through the effect too.
    using hokuto::song::DsNotePreview;
    DsNotePreview preview(*surround, 0, 0);
    const std::vector<float> previewed = PreviewOutput(preview, DsNotePreview::LongestFrames() * 512);
    const std::vector<float> note =
        DirectNote(false, DsNotePreview::HoldFrames(), DsNotePreview::LongestFrames(), effect);
    Check(HasSound(std::span<const float>(previewed)) && previewed == note, "the note preview through the effect");
    Check(note != DirectNote(false, DsNotePreview::HoldFrames(), DsNotePreview::LongestFrames()),
          "the effect changes the note");
}

// ntdlib's output of the test archive's sequence 0, run directly for `driver_frames` of the driver's frames in strict
// mixing: started with the settings of `profile`, with the profile's commands right after the start.
std::vector<int16_t> DirectProfile(const DsArchive& archive, int driver_frames, const ntdlib::ntr::GameProfile& profile)
{
    ntdlib::ntr::ArchivePlayer player(archive.archive, {archive.build});
    player.Start(ntdlib::ntr::SequenceRef{0}, profile.settings);
    if (!profile.commands.empty())
    {
        player.Send(profile.commands);
    }
    std::vector<ntdlib::ntr::SoundOutput> out;
    for (int frame = 0; frame < driver_frames; frame++)
    {
        player.RunFrame(out);
    }
    std::vector<int16_t> samples;
    ntdlib::ntr::AppendPcm16(out, samples);

    return samples;
}

// The model's output of sound 0 of `archive` in strict mixing over 400 of its frames, or the dry render's with `dry`.
std::vector<int16_t> ModelOutput(const std::shared_ptr<const DsArchive>& archive, bool dry)
{
    const auto sequence = std::make_shared<const Sequence>(hokuto::song::ReadSequence(*archive, 0));
    SequenceModel model(archive, sequence,
                        hokuto::song::ModelOptions{.mixing = Mixing::kStrict, .snapshot_interval = 20, .dry = dry});

    return Play(model, 400).samples;
}

// Whether `a` and `b` both hold a reverb, with the same values.
bool SameReverb(const std::optional<ntdlib::ntr::Reverb>& a, const std::optional<ntdlib::ntr::Reverb>& b)
{
    return a && b && a->size == b->size && a->pcm8 == b->pcm8 && a->sample_rate == b->sample_rate &&
           a->volume == b->volume;
}

// A game's profile (ntdlib::ntr::GameProfileOf) contains what the code of the game sets as it starts a sequence of its
// archive. A2DE's code mutes tracks 1 and 8 of BGM_WORLD8 in its file sound_data.sdat once the sequence has started.
// APHE's code plays every sequence of its file sound.sbin with the capture units' reverb. The model has to match
// ntdlib's ArchivePlayer, run directly with the profile, and has to differ from the plain run. The dry render leaves
// the reverb out, and the note preview keeps it. The performer uses the profile too, and the starting values of the
// Variables zone follow the values of the profile. An archive in another file of the image gets no such profile.
void TestProfiles()
{
    // 48 of the model's frames, each 341 samples in stereo.
    constexpr std::size_t kEnough = 48 * 341 * 2;
    const std::shared_ptr<const DsArchive> world = ArchiveOfGame("A2DE", "sound_data.sdat", "BGM_WORLD8");
    const ntdlib::ntr::GameProfile muted = ntdlib::ntr::GameProfileOf("A2DE", "sound_data.sdat", 0, "BGM_WORLD8");
    SEQLIB_REQUIRE(!muted.commands.empty());
    const std::vector<int16_t> plain = DirectStrict(*world, {0}, 800);
    const std::vector<int16_t> world_expected = DirectProfile(*world, 800, muted);
    const std::vector<int16_t> world_played = ModelOutput(world, false);
    SEQLIB_REQUIRE(world_played.size() > kEnough && world_played.size() <= world_expected.size());
    Check(hokuto::song::DsProfileOf(*world, 0).commands.size() == muted.commands.size() &&
              hokuto::song::DsPerformOptions(*world, 0, {}).commands.size() == muted.commands.size(),
          "the profile's commands");
    Check(world_expected != DirectStrict(*world, {0}, 800, 0, muted.settings), "the commands change the output");
    Check(std::equal(world_played.begin(), world_played.end(), world_expected.begin()), "the output with the commands");
    Check(hokuto::song::DsProfileOf(*ArchiveOfGame("A2DE", "sound/sound_data.sdat", "BGM_WORLD8"), 0).commands.empty(),
          "an archive in another file takes no commands");

    const std::shared_ptr<const DsArchive> rescue = ArchiveOfGame("APHE", "sound.sbin");
    const ntdlib::ntr::GameProfile reverb =
        ntdlib::ntr::GameProfileOf("APHE", "sound.sbin", 0, hokuto::song::DsSoundName(*rescue, 0));
    SEQLIB_REQUIRE(reverb.settings.reverb.has_value());
    const std::vector<int16_t> rescue_expected = DirectProfile(*rescue, 800, reverb);
    const std::vector<int16_t> rescue_played = ModelOutput(rescue, false);
    const std::vector<int16_t> rescue_dry = ModelOutput(rescue, true);
    SEQLIB_REQUIRE(rescue_played.size() > kEnough && rescue_played.size() <= rescue_expected.size() &&
                   rescue_dry.size() > kEnough && rescue_dry.size() <= plain.size());
    Check(rescue_expected != plain, "the reverb changes the output");
    Check(std::equal(rescue_played.begin(), rescue_played.end(), rescue_expected.begin()),
          "the output with the reverb");
    Check(std::equal(rescue_dry.begin(), rescue_dry.end(), plain.begin()), "the dry render without the reverb");
    Check(SameReverb(hokuto::song::DsGameSettingsOf(*rescue).reverb, reverb.settings.reverb),
          "the note preview plays with the game's reverb");

    const ntdlib::sequence_export::ntr::PerformOptions options = hokuto::song::DsPerformOptions(*rescue, 0, {{0, 5}});
    Check(SameReverb(options.reverb, reverb.settings.reverb) && options.variables.size() == 1 &&
              options.variables.at(0) == 5,
          "the performer takes the profile and the starting values");
}

// The items of a DS archive in the browser: its banks, wave archives and streams with their names, their file names,
// their bytes and their samples.
struct ItemsFixture
{
    files::Wave ramp;
    Bytes bank;
    Bytes waves;
    Bytes stream;
    std::shared_ptr<const DsArchive> archive;
};

// The archive of the tests of items, the files of its bank, its wave archive and its first stream, and the wave in
// PCM16.
ItemsFixture MakeItemsFixture()
{
    // A wave in PCM16 without a loop, and two streams: one in PCM16 that loops from sample 2, and one in PCM8 in
    // stereo.
    const files::Wave ramp{.encoding = 1,
                           .sample_rate = 16000,
                           .loop_length = 2,
                           .data = {0x00, 0x10, 0x00, 0xF0, 0x34, 0x12, 0xCC, 0xED}};
    const files::Stream looped{.encoding = 1,
                               .loop = 1,
                               .loop_start = 2,
                               .samples = 4,
                               .blocks = 1,
                               .block_bytes = 8,
                               .block_samples = 4,
                               .last_block_bytes = 8,
                               .last_block_samples = 4,
                               .data = {0x00, 0x10, 0x00, 0xF0, 0x34, 0x12, 0xCC, 0xED}};
    const files::Stream stereo{.encoding = 0,
                               .channels = 2,
                               .samples = 4,
                               .blocks = 1,
                               .block_bytes = 4,
                               .block_samples = 4,
                               .last_block_bytes = 4,
                               .last_block_samples = 4,
                               .data = {0x10, 0x20, 0x30, 0x40, 0xF0, 0xE0, 0xD0, 0xC0}};
    const Bytes bank = files::Bank({{.type = 1, .region = {.wave = 0}}});
    const Bytes waves = files::WaveArchive({kSquare, ramp});
    const Bytes stream = files::StreamFile(looped);

    sdat::ArchiveModel model;
    model.banks = {sdat::BankInfo{0, {0, kNone, kNone, kNone}, ""}};
    model.wave_archives = {sdat::WaveArchiveInfo{1, ""}, std::nullopt, sdat::WaveArchiveInfo{1, ""}};
    model.streams = {sdat::StreamInfo{.file_id = 2, .volume = 127}, sdat::StreamInfo{.file_id = 3, .volume = 127}};
    model.files = {bank, waves, stream, files::StreamFile(stereo)};
    model.symbols = sdat::Symbols();
    model.symbols->names[2] = {"BANK_SQUARE"};
    model.symbols->names[3] = {"WAVE_SE", std::nullopt, std::nullopt};
    model.symbols->names[7] = {"STRM_BGM", std::nullopt};
    const std::shared_ptr<const DsArchive> archive = hokuto::song::LoadDsArchive(sdat::BuildArchive(model));

    return {ramp, bank, waves, stream, archive};
}

// Whether `call` throws std::runtime_error.
template <typename Call>
bool Throws(const Call& call)
{
    return !xsflib::testing::ErrorOf<std::runtime_error>([&] { call(); }).empty();
}

// The archive lists its banks, wave archives and streams, names them and counts the waves of each wave archive.
void TestItemTables()
{
    const ItemsFixture fixture = MakeItemsFixture();
    const std::shared_ptr<const DsArchive>& archive = fixture.archive;

    Check(hokuto::song::DsBanks(*archive) == std::vector<uint32_t>{0} &&
              hokuto::song::DsWaveArchives(*archive) == std::vector<uint32_t>{0, 2} &&
              hokuto::song::DsStreams(*archive) == std::vector<uint32_t>{0, 1},
          "the items are the entries of their tables");
    Check(hokuto::song::DsWaveArchiveName(*archive, 0) == "WAVE_SE" &&
              hokuto::song::DsWaveArchiveName(*archive, 2) == "WAVEARC_2" &&
              hokuto::song::DsStreamName(*archive, 0) == "STRM_BGM" &&
              hokuto::song::DsStreamName(*archive, 1) == "STRM_1",
          "an item without a name takes its kind and its index");
    Check(hokuto::song::DsWaveCount(*archive, 0) == 2 && hokuto::song::DsWaveCount(*archive, 1) == 0 &&
              hokuto::song::DsWaveCount(*archive, 3) == 0,
          "a wave archive counts its waves, and a missing one none");
}

// The waves and the streams decode to PCM, and a missing one throws.
void TestItemPcm()
{
    const ItemsFixture fixture = MakeItemsFixture();
    const std::shared_ptr<const DsArchive>& archive = fixture.archive;

    const ntdlib::wave::Pcm square = hokuto::song::DsWavePcm(*archive, 0, 0);
    std::vector<int16_t> high(16);
    for (std::size_t i = 0; i < high.size(); i++)
    {
        high[i] = static_cast<int16_t>(i % 4 < 2 ? 0x4000 : -0x4000);
    }
    Check(square.channels == std::vector<std::vector<int16_t>>{high} && square.sample_rate == 32792 && square.loop &&
              square.loop_start == 0 && square.loop_end == 16,
          "a wave in PCM8 decodes with its loop, at the timer's rate");
    const ntdlib::wave::Pcm decoded = hokuto::song::DsWavePcm(*archive, 0, 1);
    Check(decoded.channels == std::vector<std::vector<int16_t>>{{0x1000, -0x1000, 0x1234, -0x1234}} &&
              decoded.sample_rate == 16004 && !decoded.loop && decoded.loop_end == 4,
          "a wave in PCM16 without a loop decodes whole");
    const ntdlib::wave::Pcm streamed = hokuto::song::DsStreamPcm(*archive, 0);
    Check(streamed.channels == std::vector<std::vector<int16_t>>{{0x1000, -0x1000, 0x1234, -0x1234}} &&
              streamed.sample_rate == 32728 && streamed.loop && streamed.loop_start == 2 && streamed.loop_end == 4,
          "a stream decodes with its loop");
    const ntdlib::wave::Pcm both = hokuto::song::DsStreamPcm(*archive, 1);
    Check(both.channels == std::vector<std::vector<int16_t>>{{0x1000, 0x2000, 0x3000, 0x4000},
                                                             {-0x1000, -0x2000, -0x3000, -0x4000}} &&
              !both.loop,
          "a stream in stereo decodes each channel");

    Check(Throws([&] { hokuto::song::DsWavePcm(*archive, 0, 2); }) &&
              Throws([&] { hokuto::song::DsWavePcm(*archive, 1, 0); }) &&
              Throws([&] { hokuto::song::DsStreamPcm(*archive, 2); }),
          "a wave or a stream missing from the archive throws");
}

// The bytes, the file name and the name of each item.
void TestItemFiles()
{
    using hokuto::song::Item;
    using hokuto::song::ItemKind;

    const ItemsFixture fixture = MakeItemsFixture();
    const files::Wave& ramp = fixture.ramp;
    const Bytes& bank = fixture.bank;
    const Bytes& waves = fixture.waves;
    const Bytes& stream = fixture.stream;
    const std::shared_ptr<const DsArchive>& archive = fixture.archive;

    const Item bank_item{.kind = ItemKind::kBank, .index = 0};
    const Item archive_item{.kind = ItemKind::kWaveArchive, .index = 0};
    const Item wave_item{.kind = ItemKind::kWave, .index = 0, .wave = 1};
    const Item stream_item{.kind = ItemKind::kStream, .index = 0};
    Check(hokuto::song::DsItemBytes(*archive, bank_item) == bank &&
              hokuto::song::DsItemBytes(*archive, archive_item) == waves &&
              hokuto::song::DsItemBytes(*archive, stream_item) == stream,
          "a bank, a wave archive and a stream go out as the archive holds their files");
    Check(hokuto::song::DsItemBytes(*archive, wave_item) == files::WaveFile(ramp),
          "a wave goes out as a wave file of its bytes");
    Check(hokuto::song::DsItemFileName(*archive, bank_item) == "BANK_SQUARE.sbnk" &&
              hokuto::song::DsItemFileName(*archive, archive_item) == "WAVE_SE.swar" &&
              hokuto::song::DsItemFileName(*archive, wave_item, 2) == "WAVE_SE_1.swav" &&
              hokuto::song::DsItemFileName(*archive, wave_item, 12) == "WAVE_SE_01.swav" &&
              hokuto::song::DsItemFileName(*archive, {.kind = ItemKind::kStream, .index = 1}) == "STRM_1.strm",
          "each file takes its item's name and its format's extension");
    Check(hokuto::song::DsItemName(*archive, wave_item) == "WAVE_SE, wave 1", "a wave names its wave archive");
}

// A bank converts to a SoundFont and an SFZ file, and a missing one throws.
void TestBankConversion()
{
    const ItemsFixture fixture = MakeItemsFixture();
    const std::shared_ptr<const DsArchive>& archive = fixture.archive;

    const std::vector<uint32_t> programs{0};
    const ntdlib::sequence_export::BankConversion converted = hokuto::song::ConvertDsBank(*archive, 0, programs);
    Check(converted.sound_fonts.size() == 1 && converted.sfz.files.size() == 1,
          "a bank's instrument goes out as a SoundFont and an SFZ file");
    Check(Throws([&] { hokuto::song::ConvertDsBank(*archive, 1, programs); }),
          "a bank missing from the archive throws");
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("ds",
                                    []
                                    {
                                        TestSounds();
                                        TestRead();
                                        TestChannels();
                                        TestPsgChannels();
                                        TestDsCollisions();
                                        TestVariables();
                                        TestChanges();
                                        TestPlays();
                                        TestMeters();
                                        TestReader();
                                        TestMidi();
                                        TestNotePreview();
                                        TestOutputEffect();
                                        TestProfiles();
                                        TestItemTables();
                                        TestItemPcm();
                                        TestItemFiles();
                                        TestBankConversion();
                                    });
}
