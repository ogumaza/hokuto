// SPDX-License-Identifier: MIT

// Tests Hokuto's path for the GBA driver of Rare: the import and the round trip, the tunes that the arrangement shows,
// playback, the edits of notes, the mix, the refused edits of tracks, and the exports. ntdlib's test cartridge of the
// driver (ntdlib/testing/rare_cart.h) contains no game data. It contains the code patterns that ntdlib's detection
// reads, the driver's tables, a sample, a program map and the tunes below.
//
// The expected ticks, keys, velocities and bytes come from the tracks' bytes below, worked out by hand. In the format
// with a channel byte, a note-on is 4 bytes: the command, the channel, the key and the velocity. A note-off is the
// command, the channel, the key and a byte that the driver ignores. A delay of 1 to 3 bytes follows its command. A note
// sounds from its note-on to the next note-off of its key on its channel.
//
// ntdlib's ArchivePlayer, run directly, makes the reference output. The model's output has to match it sample for
// sample, from the start and after a seek, in both mixing modes. So does the output with a muted track. The MIDI file
// and the SoundFont have to equal those of ntdlib's converter, run directly.

#include "song/rare.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "ntdlib/agb/gba.h"
#include "ntdlib/rare/archive_player.h"
#include "ntdlib/rare/song.h"
#include "ntdlib/sequence_export/agb/settings.h"
#include "ntdlib/sequence_export/convert.h"
#include "ntdlib/sequence_export/rare/convert.h"
#include "ntdlib/testing/rare_cart.h"
#include "xsflib/testing/check.h"

#include "driver_test_support.h"
#include "edit/mix.h"
#include "edit/notes.h"
#include "edit/rare.h"
#include "edit/replace.h"
#include "edit/tracks.h"
#include "format/archive.h"
#include "format/drivers.h"
#include "format/file.h"
#include "format/text.h"
#include "song/sequence.h"
#include "song/sequence_model.h"

namespace
{

namespace format = hokuto::format;
namespace song = hokuto::song;
namespace edit = hokuto::edit;
namespace rare = ntdlib::rare;
namespace cart = ntdlib::testing::rare_cart;
using format::File;
using hokuto::testing::ClockDrift;
using hokuto::testing::ClockRun;
using hokuto::testing::HasSound;
using hokuto::testing::ModelPlaysAsReference;
using hokuto::testing::PlacesOf;
using hokuto::testing::Play;
using hokuto::testing::Played;
using hokuto::testing::RomBytes;
using hokuto::testing::RunDirect;
using hokuto::testing::SequenceOf;
using hokuto::testing::StartsOf;
using hokuto::testing::TrackOf;
using song::Mixing;
using song::RareRom;
using song::Sequence;
using song::SequenceModel;
using xsflib::testing::ErrorOf;

// The tunes, at 480 ticks to the quarter note, with programs 0 and 5 on the cart's one sample:
//
//   Tune 0, track 0: tempo, program 0 on channel 0, key 60 at velocity 100 for 240 ticks, then key 62 at 80 for 240
//                    ticks, then the end at tick 480.
//   Tune 0, track 1: program 5 on channel 1, key 67 at velocity 90 for 480 ticks, then the end.
//   Tune 1, track 0: tempo, program 0 on channel 2, key 64 at velocity 70 for 120 ticks, then the end.
std::vector<uint8_t> Cart(rare::Format format = rare::Format::kChannelByte)
{
    cart::Cart image(format);
    const uint32_t sample = image.Sample(cart::Saw(256), 64);
    const std::pair<uint32_t, uint32_t> bank = image.Bank({{0, sample}, {5, sample}});
    image.Tune({cart::Track(format)
                    .Tempo(500000)
                    .Program(0, 0)
                    .On(0, 60, 100)
                    .Wait(240)
                    .Off(0, 60)
                    .On(0, 62, 80)
                    .Wait(240)
                    .Off(0, 62)
                    .End(),
                cart::Track(format).Program(1, 5).On(1, 67, 90).Wait(480).Off(1, 67).End()},
               bank);
    image.Tune({cart::Track(format).Tempo(500000).Program(2, 0).On(2, 64, 70).Wait(120).Off(2, 64).End()}, bank);

    return RomBytes(image.ToRom());
}

Sequence ReadOf(const File& file, uint32_t sound)
{
    return song::LoadRareRom(format::ExportArchive(file))->ReadSequence(sound);
}

// The reference: ntdlib's player run directly up to `samples` stereo samples.
std::vector<int16_t> DirectStrict(const RareRom& rom, int tune, std::size_t samples, int muted = -1)
{
    rare::ArchivePlayer player(rom.rom, rom.info);
    player.Start(tune, {});
    if (muted >= 0)
    {
        player.MuteTrack(muted, true);
    }
    return RunDirect<int16_t>(player, samples);
}

std::vector<float> DirectEnhanced(const RareRom& rom, int tune, std::size_t samples)
{
    rare::ArchivePlayer player(rom.rom, rom.info);
    player.Start(tune, {.enhanced = true});
    return RunDirect<float>(player, samples);
}

// The model plays `file`'s sound `sound` as the reference does, in strict mixing.
bool PlaysAsReference(const File& file, uint32_t sound)
{
    const std::shared_ptr<const RareRom> rom = song::LoadRareRom(format::ExportArchive(file));
    return ModelPlaysAsReference(
        rom, sound, [&](std::size_t samples) { return DirectStrict(*rom, static_cast<int>(sound), samples); });
}

// A ROM with the driver comes in as engine 12 of the format, and comes back byte for byte from both forms of the file.
// The format of the commands determines the revision. A ROM without the driver's code doesn't count as such a ROM.
void TestImport()
{
    for (const rare::Format format : {rare::Format::kChannelByte, rare::Format::kChannelNibble})
    {
        const std::vector<uint8_t> bytes = Cart(format);
        const format::Driver* driver = format::DriverOfRom(bytes);
        SEQLIB_REQUIRE(driver != nullptr);
        XSFLIB_CHECK(driver->Facts().engine == format::Engine::kRare);

        const File file = driver->Import(bytes);
        XSFLIB_CHECK(file.engine == format::Engine::kRare);
        SEQLIB_REQUIRE(file.native && file.native->driver);
        XSFLIB_CHECK(file.native->driver->revision == (format == rare::Format::kChannelNibble ? 1 : 0));
        XSFLIB_CHECK(format::ExportArchive(file) == bytes);

        const File binary = format::Read(format::Write(file));
        XSFLIB_CHECK(binary.engine == format::Engine::kRare);
        XSFLIB_CHECK(format::ExportArchive(binary) == bytes);
        XSFLIB_CHECK(format::Write(binary) == format::Write(file));

        const File text = format::ReadText(format::WriteText(file));
        XSFLIB_CHECK(text.engine == format::Engine::kRare);
        XSFLIB_CHECK(format::ExportArchive(text) == bytes);
        XSFLIB_CHECK(format::Write(text) == format::Write(file));
    }

    cart::Cart without(rare::Format::kChannelByte, 6, false);
    const ntdlib::agb::Rom empty = without.ToRom();
    XSFLIB_CHECK(format::DriverOfRom(RomBytes(empty)) == nullptr);

    // A delay of 240 ticks in 2 bytes, where 1 would hold it, keeps its bytes in both forms.
    cart::Cart wide(rare::Format::kChannelByte);
    const uint32_t sample = wide.Sample(cart::Saw(64));
    const std::vector<uint8_t> track = {cart::kCmdNoteOn,  0, 60, 100,  cart::kCmdDelay2, 240, 0,
                                        cart::kCmdNoteOff, 0, 60, 0x40, cart::kCmdEnd};
    wide.Tune({track}, wide.Bank({{0, sample}}));
    const std::vector<uint8_t> wide_bytes = RomBytes(wide.ToRom());
    const File wide_file = format::DriverOfRom(wide_bytes)->Import(wide_bytes);
    XSFLIB_CHECK(format::ExportArchive(wide_file) == wide_bytes);
    XSFLIB_CHECK(format::ExportArchive(format::Read(format::Write(wide_file))) == wide_bytes);
    XSFLIB_CHECK(format::ExportArchive(format::ReadText(format::WriteText(wide_file))) == wide_bytes);

    const std::vector<std::optional<edit::SoundPlace>> places = PlacesOf(format::DriverOfRom(Cart())->Import(Cart()));
    SEQLIB_REQUIRE(places.size() == 2 && places[0] && places[1]);
    XSFLIB_CHECK(places[0]->sequence != places[1]->sequence);
}

// The tunes that the arrangement shows: the notes at their ticks, with their keys, velocities, lengths and programs.
// A channel's volume is 127 until a command sets it.
void TestRead()
{
    const std::shared_ptr<const RareRom> rom = song::LoadRareRom(Cart());
    XSFLIB_CHECK(rom->Sounds() == (std::vector<uint32_t>{0, 1}));
    XSFLIB_CHECK(rom->SoundName(0) == "Song 00");
    XSFLIB_CHECK(rom->Rules().DriverEngine() == format::Engine::kRare);

    const Sequence tune0 = rom->ReadSequence(0);
    XSFLIB_CHECK(tune0.timebase == 480);
    XSFLIB_CHECK(!tune0.loop);
    XSFLIB_CHECK(tune0.length == 480);
    SEQLIB_REQUIRE(TrackOf(tune0, 0) && TrackOf(tune0, 1));
    const song::Track& first = *TrackOf(tune0, 0);
    SEQLIB_REQUIRE(first.notes.size() == 2);
    XSFLIB_CHECK(StartsOf(tune0, 0) == (std::vector<uint32_t>{0, 240}));
    XSFLIB_CHECK(first.notes[0].key == 60 && first.notes[0].length == 240 && first.notes[0].velocity == 100);
    XSFLIB_CHECK(first.notes[1].key == 62 && first.notes[1].length == 240 && first.notes[1].velocity == 80);
    XSFLIB_CHECK(first.notes[0].program == 0 && first.notes[1].program == 0);
    SEQLIB_REQUIRE(!first.volume.empty());
    XSFLIB_CHECK(first.volume.front().tick == 0 && first.volume.front().value == 127);

    const song::Track& second = *TrackOf(tune0, 1);
    SEQLIB_REQUIRE(second.notes.size() == 1);
    XSFLIB_CHECK(second.notes[0].key == 67 && second.notes[0].length == 480 && second.notes[0].velocity == 90);
    XSFLIB_CHECK(second.notes[0].program == 5);

    const Sequence tune1 = rom->ReadSequence(1);
    XSFLIB_CHECK(StartsOf(tune1, 0) == (std::vector<uint32_t>{0}));
    XSFLIB_CHECK(TrackOf(tune1, 0)->notes[0].length == 120);

    // The inspector's menus offer the tune's program map as its one bank.
    const std::vector<song::DriverBankSlot> slots = rom->BankSlots(0);
    SEQLIB_REQUIRE(slots.size() == 1);
    XSFLIB_CHECK(slots[0].programs == (std::vector<uint32_t>{0, 5}));

    // The browser lists the one sample. Both programs play it.
    XSFLIB_CHECK(rom->Samples() == (std::vector<int>{0}));
    XSFLIB_CHECK(rom->SamplesOfSound(0) == (std::vector<int>{0}));
    XSFLIB_CHECK(rom->ShapeOf(0).length == 256 && rom->ShapeOf(0).loop_start == 192u);
}

// The model plays as ntdlib's player, run directly, from the start and after a seek, in both mixing modes, and with a
// track muted.
void TestPlays()
{
    const std::shared_ptr<const RareRom> rom = song::LoadRareRom(Cart());
    for (const uint32_t sound : {0u, 1u})
    {
        const auto sequence = std::make_shared<const Sequence>(rom->ReadSequence(sound));
        SequenceModel strict(rom, sequence,
                             hokuto::song::ModelOptions{.mixing = Mixing::kStrict, .snapshot_interval = 20});
        const std::size_t frame = 2 * strict.FrameSamples();
        const Played played = Play(strict, 120);
        SEQLIB_REQUIRE(played.samples.size() >= frame);
        XSFLIB_CHECK(HasSound(std::span<const int16_t>(played.samples)));
        const std::vector<int16_t> expected =
            DirectStrict(*rom, static_cast<int>(sound), played.samples.size() + frame);
        XSFLIB_CHECK(std::equal(played.samples.begin(), played.samples.end(), expected.begin()));
        // A seek to the last frame played. It restores the snapshot of frame 20 when the sound plays past it.
        const std::size_t last = played.samples.size() / frame - 1;
        SEQLIB_REQUIRE(last > 0);
        XSFLIB_CHECK(strict.SeekFrame(last));
        SEQLIB_REQUIRE(strict.Output().size() == frame);
        XSFLIB_CHECK(std::equal(strict.Output().begin(), strict.Output().end(), expected.begin() + last * frame));

        SequenceModel enhanced(rom, sequence,
                               hokuto::song::ModelOptions{.mixing = Mixing::kEnhanced, .snapshot_interval = 20});
        const std::size_t enhanced_frame = 2 * enhanced.FrameSamples();
        const Played enhanced_played = Play(enhanced, 120);
        SEQLIB_REQUIRE(enhanced_played.floats.size() >= enhanced_frame);
        XSFLIB_CHECK(HasSound(std::span<const float>(enhanced_played.floats)));
        const std::vector<float> enhanced_expected =
            DirectEnhanced(*rom, static_cast<int>(sound), enhanced_played.floats.size() + enhanced_frame);
        XSFLIB_CHECK(
            std::equal(enhanced_played.floats.begin(), enhanced_played.floats.end(), enhanced_expected.begin()));
    }

    const auto sequence = std::make_shared<const Sequence>(rom->ReadSequence(0));
    SequenceModel muted(rom, sequence);
    muted.SetMutes(1u << 1);
    const Played muted_played = Play(muted, 40);
    const std::vector<int16_t> muted_expected = DirectStrict(*rom, 0, muted_played.samples.size() + 4096, 1);
    SEQLIB_REQUIRE(!muted_played.samples.empty());
    XSFLIB_CHECK(std::equal(muted_played.samples.begin(), muted_played.samples.end(), muted_expected.begin()));
    XSFLIB_CHECK(muted_expected != DirectStrict(*rom, 0, muted_played.samples.size() + 4096));
}

// The model's clock counts the arrangement's ticks in the frames from the first. The driver counts 1/60 s of the song
// in a frame: at 120 BPM and 480 ticks to the quarter note, 16 ticks. The arrangement times the GBA's frames, and has a
// tempo of 119.455 BPM. A track that waits 40 quarter notes counts the wait's ticks when it starts, and the clock took
// the most ticks of any track: 19,200 at once. After 600 frames, 10 s of the song, the clock reads 599 x 16. In
// ntdlib's model, track 0 starts its second note, at tick 240, in frame 17. The clock reads 16 x 16 there.
void TestClock()
{
    const auto format = rare::Format::kChannelByte;
    cart::Cart image(format);
    const uint32_t saw = image.Sample(cart::Saw(256), 64);
    const std::pair<uint32_t, uint32_t> bank = image.Bank({{0, saw}});
    image.Tune({cart::Track(format)
                    .Tempo(500000)
                    .Program(0, 0)
                    .On(0, 60, 100)
                    .Wait(240)
                    .Off(0, 60)
                    .On(0, 62, 100)
                    .Wait(20000)
                    .Off(0, 62)
                    .End(),
                cart::Track(format).Program(1, 0).Wait(480 * 40).On(1, 67, 90).Wait(480).Off(1, 67).End()},
               bank);
    const std::vector<uint8_t> bytes = RomBytes(image.ToRom());
    const std::shared_ptr<const RareRom> rom = song::LoadRareRom(bytes);
    const Sequence sequence = rom->ReadSequence(0);
    SEQLIB_REQUIRE(sequence.timebase == 480 && !sequence.tempo.empty());
    XSFLIB_CHECK(std::abs(sequence.tempo.front().bpm - 119.455) < 0.001);

    const std::unique_ptr<song::DriverPlayer> player = rom->MakePlayer();
    player->Start(0, false);
    player->SetArrangement(sequence);
    rare::ArchivePlayer reference(rom->rom, rom->info);
    reference.Start(0, {});
    std::vector<int16_t> out;
    std::vector<int16_t> reference_out;
    std::optional<uint32_t> second_note;
    for (uint32_t frame = 1; frame <= 600; frame++)
    {
        player->RunFrame(out);
        reference.RunFrame(reference_out);
        if (!second_note && reference.Driver().TrackTick(0) > 240)
        {
            second_note = player->Tick();
        }
    }

    XSFLIB_CHECK(second_note == 256u);
    XSFLIB_CHECK(player->Tick() == 599 * 16);
}

// The clock through a loop with two tempos. Track 0 loops from tick 0 (controller 102) to tick 480 (103): 120 BPM for
// 240 ticks, then 240 BPM for 240 ticks. A pass lasts 375,000 microseconds of the driver's time. Frame f starts f x
// kFrameTime into the song (TickClock::FrameTick in ntdlib's converter). After 50 frames, 49 have passed: 816,666.5
// microseconds, two passes and 66,666.5 microseconds of the third, at 120 BPM. The clock counts every pass: 960 + 64
// ticks. A snapshot of frame 20 restores the clock of frame 20, and 30 more frames then reach the clock of frame 50.
void TestClockThroughLoop()
{
    const auto format = rare::Format::kChannelByte;
    cart::Cart image(format);
    const uint32_t saw = image.Sample(cart::Saw(256), 64);
    const std::pair<uint32_t, uint32_t> bank = image.Bank({{0, saw}});
    image.Tune({cart::Track(format)
                    .Control(0, rare::kCtrlLoopStart, 0)
                    .Tempo(500000)
                    .Program(0, 0)
                    .On(0, 60, 100)
                    .Wait(240)
                    .Off(0, 60)
                    .Tempo(250000)
                    .On(0, 62, 100)
                    .Wait(240)
                    .Off(0, 62)
                    .Control(0, rare::kCtrlLoopEnd, 0)
                    .End()},
               bank);
    const std::shared_ptr<const RareRom> rom = song::LoadRareRom(RomBytes(image.ToRom()));
    const Sequence sequence = rom->ReadSequence(0);
    SEQLIB_REQUIRE(sequence.loop && sequence.tempo.size() == 2);
    XSFLIB_CHECK(sequence.loop->start == 0 && sequence.loop->end == 480);

    const std::unique_ptr<song::DriverPlayer> player = rom->MakePlayer();
    player->Start(0, false);
    player->SetArrangement(sequence);
    std::vector<int16_t> out;
    std::shared_ptr<const song::DriverPlayer::Snapshot> saved;
    uint32_t at_20 = 0;
    for (uint32_t frame = 1; frame <= 50; frame++)
    {
        player->RunFrame(out);
        if (frame == 20)
        {
            saved = player->Save(nullptr);
            at_20 = player->Tick();
        }
    }

    const uint32_t at_50 = player->Tick();
    XSFLIB_CHECK(at_50 == 960 + 64);
    player->Restore(*saved);
    XSFLIB_CHECK(player->Tick() == at_20);
    for (uint32_t frame = 21; frame <= 50; frame++)
    {
        player->RunFrame(out);
    }
    XSFLIB_CHECK(player->Tick() == at_50);
}

// An edit of a note reaches the tune's bytes: a new velocity, a move, a longer note, a removal and a new note.
void TestNoteEdits()
{
    const std::vector<uint8_t> bytes = Cart();
    const File file = format::DriverOfRom(bytes)->Import(bytes);
    const uint32_t sequence = SequenceOf(file, 0);
    const Sequence tune0 = ReadOf(file, 0);
    const song::Track& track = *TrackOf(tune0, 0);
    const song::Note& first = track.notes[0];
    const song::Note& second = track.notes[1];
    const song::Stretch& stretch = track.stretches.at(first.stretch);

    // A new velocity sets the note-on's byte. The bytes keep their size.
    const edit::NoteEdit velocity{.command = first.command, .stretch = stretch, .velocity = 40};
    const File quieter = edit::EditNotes(file, sequence, std::span(&velocity, 1)).file;
    XSFLIB_CHECK(format::ExportArchive(quieter).size() == bytes.size());
    const Sequence quieter_tune = ReadOf(quieter, 0);
    SEQLIB_REQUIRE(TrackOf(quieter_tune, 0) && TrackOf(quieter_tune, 0)->notes.size() == 2);
    XSFLIB_CHECK(TrackOf(quieter_tune, 0)->notes[0].velocity == 40);
    XSFLIB_CHECK(TrackOf(quieter_tune, 0)->notes[1].velocity == 80);
    XSFLIB_CHECK(StartsOf(quieter_tune, 1) == StartsOf(tune0, 1));
    XSFLIB_CHECK(PlaysAsReference(quieter, 0));

    // A move of 10 ticks moves the note-off too. The track's end stays at tick 480.
    const edit::NoteEdit moved{.command = first.command, .stretch = stretch, .shift = 10};
    const Sequence moved_tune = ReadOf(edit::EditNotes(file, sequence, std::span(&moved, 1)).file, 0);
    XSFLIB_CHECK(StartsOf(moved_tune, 0) == (std::vector<uint32_t>{10, 240}));
    XSFLIB_CHECK(TrackOf(moved_tune, 0)->notes[0].length == 240);
    XSFLIB_CHECK(moved_tune.length == 480);

    // A shorter note moves its note-off earlier.
    const edit::NoteEdit shorter{
        .command = second.command, .stretch = track.stretches.at(second.stretch), .length = 120u};
    const Sequence shorter_tune = ReadOf(edit::EditNotes(file, sequence, std::span(&shorter, 1)).file, 0);
    XSFLIB_CHECK(TrackOf(shorter_tune, 0)->notes[1].length == 120);
    XSFLIB_CHECK(StartsOf(shorter_tune, 0) == StartsOf(tune0, 0));

    // A removal drops the note-off with the note-on.
    const edit::NoteEdit removed{
        .command = second.command, .stretch = track.stretches.at(second.stretch), .remove = true};
    const File removed_file = edit::EditNotes(file, sequence, std::span(&removed, 1)).file;
    XSFLIB_CHECK(StartsOf(ReadOf(removed_file, 0), 0) == (std::vector<uint32_t>{0}));
    XSFLIB_CHECK(format::ExportArchive(removed_file).size() == bytes.size());

    // A new note on the second track plays on the channel of the track's notes.
    const song::Track& other = *TrackOf(tune0, 1);
    const edit::NewNote added{
        .stretch = other.stretches.at(other.notes[0].stretch), .tick = 120, .key = 72, .velocity = 60, .length = 60};
    const File added_file = edit::EditNotes(file, sequence, {}, std::span(&added, 1)).file;
    const Sequence with_new = ReadOf(added_file, 0);
    XSFLIB_CHECK(StartsOf(with_new, 1) == (std::vector<uint32_t>{0, 120}));
    SEQLIB_REQUIRE(TrackOf(with_new, 1)->notes.size() == 2);
    const song::Note& new_note = TrackOf(with_new, 1)->notes[1];
    XSFLIB_CHECK(new_note.key == 72 && new_note.length == 60 && new_note.velocity == 60 && new_note.program == 5);
    XSFLIB_CHECK(StartsOf(with_new, 0) == StartsOf(tune0, 0));
    XSFLIB_CHECK(PlaysAsReference(added_file, 0));
}

// The mixer's volume of a track sets controller 7 of the track's channel. The driver has no pan.
void TestMix()
{
    const std::vector<uint8_t> bytes = Cart();
    const File file = format::DriverOfRom(bytes)->Import(bytes);
    const uint32_t sequence = SequenceOf(file, 0);
    const Sequence tune0 = ReadOf(file, 0);

    const edit::MixEdit half{.setting = song::Mix::kVolume, .from = 127, .to = 64};
    const File half_file = edit::EditMix(file, sequence, 1, TrackOf(tune0, 1)->stretches, half).file;
    const Sequence with_half = ReadOf(half_file, 0);
    SEQLIB_REQUIRE(TrackOf(with_half, 1) && !TrackOf(with_half, 1)->volume.empty());
    XSFLIB_CHECK(TrackOf(with_half, 1)->volume.front().tick == 0 && TrackOf(with_half, 1)->volume.front().value == 64);
    XSFLIB_CHECK(StartsOf(with_half, 1) == StartsOf(tune0, 1));
    XSFLIB_CHECK(StartsOf(with_half, 0) == StartsOf(tune0, 0));
    XSFLIB_CHECK(PlaysAsReference(half_file, 0));

    const edit::MixEdit panned{.setting = song::Mix::kPan, .from = 0, .to = 32};
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>(
                     [&] { edit::EditMix(file, sequence, 1, TrackOf(tune0, 1)->stretches, panned); }) ==
                 "Tracks of Rare's driver have no pan.");
}

// A tune keeps its count of tracks: a new track, a copy and a paste are refused. A removal drops the track's
// notes, and the other track plays as before.
void TestTracks()
{
    const std::vector<uint8_t> bytes = Cart();
    const File file = format::DriverOfRom(bytes)->Import(bytes);
    const std::vector<std::optional<edit::SoundPlace>> places = PlacesOf(file);
    const Sequence tune0 = ReadOf(file, 0);

    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { edit::AddTrack(file, places, 0, tune0, std::nullopt); })
                     .find("count of tracks") != std::string::npos);
    XSFLIB_CHECK(!ErrorOf<std::invalid_argument>([&] { edit::DuplicateTrack(file, places, 0, 1); }).empty());

    const File without = edit::RemoveTrack(file, places, 0, 1);
    const Sequence without_second = ReadOf(without, 0);
    XSFLIB_CHECK(StartsOf(without_second, 1).empty());
    XSFLIB_CHECK(StartsOf(without_second, 0) == StartsOf(tune0, 0));
}

// The exports equal those of ntdlib's converter, run directly.
void TestExports()
{
    const std::shared_ptr<const RareRom> rom = song::LoadRareRom(Cart());
    for (const uint32_t sound : {0u, 1u})
    {
        ntdlib::sequence_export::agb::ConvertSettings settings;
        settings.program = "Hokuto";
        const ntdlib::sequence_export::rare::SongFiles expected =
            ntdlib::sequence_export::rare::ConvertSongFiles(rom->rom, rom->info, static_cast<int>(sound), settings);
        const ntdlib::sequence_export::Conversion conversion = rom->ConvertToMidi(sound, "Hokuto");
        XSFLIB_CHECK(expected.report.ok);
        XSFLIB_CHECK(conversion.midi == expected.midi);
        XSFLIB_CHECK(conversion.sf2 == expected.sf2);
        XSFLIB_CHECK(!conversion.midi.empty() && !conversion.sf2.empty());
    }

    const ntdlib::sequence_export::BankConversion bank = rom->ConvertSamples(rom->Samples(), "Samples");
    XSFLIB_CHECK(!bank.sound_fonts.empty() && !bank.sound_fonts.front().sf2.empty());
}

// The model's clock follows the arrangement: after 10 s of play, or at the song's end, the arrangement's time at the
// clock is within 2 frames of the time played. The cart's tunes end within 32 frames. This test therefore checks only
// the gross rate. TestClock and TestClockThroughLoop check the clock against ntdlib's frame ticks.
void TestClockFollowsArrangement()
{
    const std::shared_ptr<const RareRom> rom = song::LoadRareRom(Cart());
    for (const uint32_t sound : rom->Sounds())
    {
        const ClockRun run = ClockDrift(rom, sound, 600);
        XSFLIB_CHECK(std::abs(run.drift) <= 2.0);
    }
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("rare",
                                    []
                                    {
                                        TestImport();
                                        TestRead();
                                        TestPlays();
                                        TestClock();
                                        TestClockThroughLoop();
                                        TestClockFollowsArrangement();
                                        TestNoteEdits();
                                        TestMix();
                                        TestTracks();
                                        TestExports();
                                    });
}
