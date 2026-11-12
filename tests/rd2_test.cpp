// SPDX-License-Identifier: MIT

// Tests Hokuto's path for Nintendo R&D2's GBA driver: the import and the round trip, the songs that the arrangement
// shows, playback, the edits of notes, the mix, the instruments and the tracks, and the exports. ntdlib's test
// cartridge of the driver (rd2_cart.h in ntdlib's tests) contains no game data. It contains the code patterns that
// ntdlib's detection reads, the driver's tables, two samples, a bank and the sequences below.
//
// The expected ticks, keys, velocities and bytes come from the sequences' bytes below, worked out by hand. A song's
// header contains its count of tracks, a byte of 0 and the offset of each track's start in 2 bytes. A note command of
// 60 to BF contains its length and its velocity, and one of 00 to 5F repeats those of the track's last note. A wait of
// C1 contains its length, and one of C0 repeats it. The Key Editor shows the driver's velocity of 0 to 255 as 1 to 127.
//
// ntdlib's ArchivePlayer, run directly, makes the reference output. The model's output has to match it sample for
// sample, from the start and after a seek, in both mixing modes. So does the output with a muted track. The MIDI file
// and the SoundFont have to equal those of ntdlib's converter, run directly.

#include "song/rd2.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/agb/gba.h"
#include "ntdlib/commands/sequence.h"
#include "ntdlib/rd2/archive_player.h"
#include "ntdlib/rd2/commands.h"
#include "ntdlib/rd2/sequencer.h"
#include "ntdlib/sequence_export/agb/settings.h"
#include "ntdlib/sequence_export/convert.h"
#include "ntdlib/sequence_export/rd2/convert.h"
#include "ntdlib/testing/rd2_cart.h"
#include "xsflib/testing/check.h"

#include "driver_test_support.h"
#include "edit/instruments.h"
#include "edit/mix.h"
#include "edit/notes.h"
#include "edit/rd2.h"
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
namespace rd2 = ntdlib::rd2;
namespace cart = ntdlib::testing::rd2_cart;
using format::File;
using hokuto::testing::ClockDrift;
using hokuto::testing::ClockRun;
using hokuto::testing::HasSound;
using hokuto::testing::LoopsOver;
using hokuto::testing::ModelPlaysAsReference;
using hokuto::testing::Play;
using hokuto::testing::Played;
using hokuto::testing::RomBytes;
using hokuto::testing::RunDirect;
using hokuto::testing::SequenceAt;
using hokuto::testing::StartsOf;
using hokuto::testing::TrackOf;
using song::Mixing;
using song::Rd2Rom;
using song::Sequence;
using song::SequenceModel;
using xsflib::testing::ErrorOf;

constexpr uint32_t kRomBase = ntdlib::agb::kRomBase;

// Where the cartridge puts the first song's bytes, from the ROM's start: at the sequences' data.
constexpr uint32_t kSong0 = cart::kSequenceData - kRomBase;

// The first song's tracks, from the song's start:
//
//    0: 02 00 06 00 10 00  two tracks, at 6 and 16
//    6: C2 00              the looped sample's instrument
//    8: 9C 18 64           a note of key 60, 24 ticks long, at velocity 100
//   11: C1 18              a wait of 24 ticks
//   13: 3C                 a note of key 60 that repeats the length and the velocity, at tick 24
//   14: C0                 a wait that repeats its length
//   15: FF                 the end, at tick 48
//   16: C2 0A              the held sample's instrument
//   18: E0 60              a volume of 96
//   20: 9E 30 50           a note of key 62, 48 ticks long, at velocity 80
//   23: C1 30              a wait of 48 ticks
//   25: FF                 the end
//
// The second song's track loops: a note of 12 ticks at 6, a wait of 12 and a jump back to the note. No end follows
// the jump.
std::vector<uint8_t> Cart(bool effects = false)
{
    cart::Cart image;
    image.Sequences(
        {{cart::TrackData().Bytes({0xC2, cart::kLooped}).Note(60, 24, 100).Wait(24).Bytes({60}).Bytes({0xC0}).End(),
          cart::TrackData().Bytes({0xC2, cart::kHeld}).Bytes({0xE0, 0x60}).Note(62, 48, 80).Wait(48).End()},
         {{0xC2, cart::kHeld, 0x9C, 12, 100, 0xC1, 12, 0xF0, 6, 0}}});
    if (effects)
    {
        image.Effects({{cart::TrackData().Bytes({0xC2, cart::kPlain}).Note(64, 12, 100).Wait(12).End()}});
    }

    return RomBytes(image.ToRom());
}

// Where the bytes of song `song` start in `bytes`, from the ROM's start: the table of sequences contains the place of
// each sequence from the table's start. An edit that makes a song longer moves it to the ROM's end.
uint32_t SongAt(const std::vector<uint8_t>& bytes, uint32_t song)
{
    const uint32_t field = cart::kSequences - kRomBase + 4 * song;
    const uint32_t offset = static_cast<uint32_t>(bytes.at(field)) | static_cast<uint32_t>(bytes.at(field + 1)) << 8 |
                            static_cast<uint32_t>(bytes.at(field + 2)) << 16 |
                            static_cast<uint32_t>(bytes.at(field + 3)) << 24;
    return cart::kSequences - kRomBase + offset;
}

Sequence ReadOf(const File& file, uint32_t sound)
{
    return song::LoadRd2Rom(format::ExportArchive(file))->ReadSequence(sound);
}

// Where each sound of a file plays. The program finds each place the same way: the sequence that the table's field
// names. The song header names the starts of its tracks.
std::vector<std::optional<edit::SoundPlace>> PlacesOf(const File& file)
{
    const std::shared_ptr<const Rd2Rom> rom = song::LoadRd2Rom(format::ExportArchive(file));
    std::vector<std::optional<uint32_t>> sequences(rom->SoundCount());
    std::vector<std::vector<uint32_t>> fields(rom->SoundCount());
    for (const uint32_t sound : rom->Sounds())
    {
        sequences[sound] = format::SequenceNamedAt(file, *rom->FieldsOf(sound).sequence);
    }

    return edit::GbaSoundPlaces(sequences, fields);
}

// The file's sequence that sound `sound` plays.
uint32_t SequenceOf(const File& file, uint32_t sound)
{
    return SequenceAt(PlacesOf(file), sound);
}

// The reference: ntdlib's player run directly up to `samples` stereo samples.
std::vector<int16_t> DirectStrict(const Rd2Rom& rom, int song, std::size_t samples, int muted = -1)
{
    rd2::ArchivePlayer player(rom.rom, rom.info);
    player.Start(song, {});
    if (muted >= 0)
    {
        player.MuteTrack(muted, true);
    }
    return RunDirect<int16_t>(player, samples);
}

std::vector<float> DirectEnhanced(const Rd2Rom& rom, int song, std::size_t samples)
{
    rd2::ArchivePlayer player(rom.rom, rom.info);
    player.Start(song, {.enhanced = true});
    return RunDirect<float>(player, samples);
}

// The model plays `file`'s sound `sound` as the reference does, in strict mixing.
bool PlaysAsReference(const File& file, uint32_t sound)
{
    const std::shared_ptr<const Rd2Rom> rom = song::LoadRd2Rom(format::ExportArchive(file));
    return ModelPlaysAsReference(
        rom, sound, [&](std::size_t samples) { return DirectStrict(*rom, static_cast<int>(sound), samples); });
}

// A ROM with the driver comes in as engine 9 of the format, and comes back byte for byte from both forms of the file.
// A ROM without the driver's code doesn't count as such a ROM.
void TestImport()
{
    const std::vector<uint8_t> bytes = Cart();
    const format::Driver* driver = format::DriverOfRom(bytes);
    SEQLIB_REQUIRE(driver != nullptr);
    XSFLIB_CHECK(driver->Facts().engine == format::Engine::kRd2);
    cart::Cart without(false);
    const ntdlib::agb::Rom empty = without.ToRom();
    XSFLIB_CHECK(format::DriverOfRom(RomBytes(empty)) == nullptr);

    const File file = driver->Import(bytes);
    XSFLIB_CHECK(file.engine == format::Engine::kRd2);
    SEQLIB_REQUIRE(file.native && file.native->driver);
    XSFLIB_CHECK(static_cast<rd2::Revision>(file.native->driver->revision) == rd2::Revision::kLinkToThePast);
    XSFLIB_CHECK(format::ExportArchive(file) == bytes);

    const File binary = format::Read(format::Write(file));
    XSFLIB_CHECK(binary.engine == format::Engine::kRd2);
    XSFLIB_CHECK(format::ExportArchive(binary) == bytes);
    XSFLIB_CHECK(format::Write(binary) == format::Write(file));

    const File text = format::ReadText(format::WriteText(file));
    XSFLIB_CHECK(text.engine == format::Engine::kRd2);
    XSFLIB_CHECK(format::ExportArchive(text) == bytes);
    XSFLIB_CHECK(format::Write(text) == format::Write(file));

    // A wait of 24 ticks in two bytes, where one would hold it, keeps its bytes in both forms.
    cart::Cart wide;
    wide.Sequences(
        {{cart::TrackData().Bytes({0xC2, cart::kLooped}).Note(60, 24, 100).Bytes({0xC1, 0x80, 0x18}).End()}});
    const std::vector<uint8_t> wide_bytes = RomBytes(wide.ToRom());
    const File wide_file = driver->Import(wide_bytes);
    XSFLIB_CHECK(format::ExportArchive(wide_file) == wide_bytes);
    XSFLIB_CHECK(format::ExportArchive(format::Read(format::Write(wide_file))) == wide_bytes);
    XSFLIB_CHECK(format::ExportArchive(format::ReadText(format::WriteText(wide_file))) == wide_bytes);

    const std::vector<std::optional<edit::SoundPlace>> places = PlacesOf(file);
    SEQLIB_REQUIRE(places.size() == 2 && places[0] && places[1]);
    XSFLIB_CHECK(places[0]->sequence != places[1]->sequence);
}

// The songs that the arrangement shows: the notes at their ticks, with their keys, velocities and instruments, and the
// tracks' volumes. The driver plays a track at volume 128 until a command sets it.
void TestRead()
{
    const std::shared_ptr<const Rd2Rom> rom = song::LoadRd2Rom(Cart());
    XSFLIB_CHECK(rom->Sounds() == (std::vector<uint32_t>{0, 1}));
    XSFLIB_CHECK(rom->SoundName(0) == "Song 00");
    XSFLIB_CHECK(rom->Rules().DriverEngine() == format::Engine::kRd2);

    // The error for a sound that the ROM doesn't have names the number of the sound.
    const std::string missing = ErrorOf<std::runtime_error>([&] { rom->ReadSequence(5); });
    XSFLIB_CHECK(missing == "The ROM has no song 5.");

    const Sequence song0 = rom->ReadSequence(0);
    XSFLIB_CHECK(song0.timebase == 24);
    XSFLIB_CHECK(!song0.loop);
    XSFLIB_CHECK(song0.length == 48);
    SEQLIB_REQUIRE(TrackOf(song0, 0) && TrackOf(song0, 1));
    const song::Track& first = *TrackOf(song0, 0);
    SEQLIB_REQUIRE(first.notes.size() == 2);
    for (const song::Note& note : first.notes)
    {
        XSFLIB_CHECK(note.key == 60 && note.length == 24 && note.velocity == 50);
        XSFLIB_CHECK(note.bank_slot == 0 && note.program == cart::kLooped);
    }
    XSFLIB_CHECK(StartsOf(song0, 0) == (std::vector<uint32_t>{0, 24}));
    XSFLIB_CHECK(first.notes[0].command == 8 && first.notes[1].command == 13);
    SEQLIB_REQUIRE(!first.volume.empty());
    XSFLIB_CHECK(first.volume.front().tick == 0 && first.volume.front().value == 128);

    const song::Track& second = *TrackOf(song0, 1);
    SEQLIB_REQUIRE(second.notes.size() == 1);
    XSFLIB_CHECK(second.notes[0].key == 62 && second.notes[0].length == 48 && second.notes[0].velocity == 40);
    XSFLIB_CHECK(second.notes[0].program == cart::kHeld);
    SEQLIB_REQUIRE(!second.volume.empty());
    XSFLIB_CHECK(second.volume.front().tick == 0 && second.volume.front().value == 0x60);

    const Sequence song1 = rom->ReadSequence(1);
    XSFLIB_CHECK(LoopsOver(song1, 0, 12));
    XSFLIB_CHECK(!StartsOf(song1, 0).empty() && StartsOf(song1, 0).front() == 0);

    // The inspector's menus offer the bank of the song's one slot, with its instruments that play.
    const std::vector<song::DriverBankSlot> slots = rom->BankSlots(0);
    SEQLIB_REQUIRE(slots.size() == 1);
    XSFLIB_CHECK(slots[0].bank == "Bank 000");
    XSFLIB_CHECK(slots[0].programs == (std::vector<uint32_t>{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10}));
}

// The model plays as ntdlib's player, run directly, from the start and after a seek, in both mixing modes, and with a
// track muted.
void TestPlays()
{
    const std::shared_ptr<const Rd2Rom> rom = song::LoadRd2Rom(Cart());
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

// An edit of a note reaches the song's bytes. A note of one byte after an edited note keeps its values: it stores them
// in its bytes once the track's last note has others.
void TestNoteEdits()
{
    const std::vector<uint8_t> bytes = Cart();
    const File file = format::DriverOfRom(bytes)->Import(bytes);
    const uint32_t sequence = SequenceOf(file, 0);
    const Sequence song0 = ReadOf(file, 0);
    const song::Track& track = *TrackOf(song0, 0);
    const song::Note& first = track.notes[0];
    const song::Note& second = track.notes[1];
    const song::Stretch& stretch = track.stretches.at(first.stretch);

    // A new velocity of 40 is the driver's 80. The note of one byte after it stores 24 ticks and 100 in its bytes. The
    // song grows by 2 bytes, and moves. The second track starts 2 bytes later.
    const edit::NoteEdit velocity{.command = first.command, .stretch = stretch, .velocity = 40};
    const File quieter = edit::EditNotes(file, sequence, std::span(&velocity, 1)).file;
    const std::vector<uint8_t> quieter_bytes = format::ExportArchive(quieter);
    const uint32_t quieter_at = SongAt(quieter_bytes, 0);
    XSFLIB_CHECK(quieter_at != kSong0);
    XSFLIB_CHECK(quieter_bytes[quieter_at + 4] == 18);
    XSFLIB_CHECK(quieter_bytes[quieter_at + 10] == 80);
    XSFLIB_CHECK(std::equal(quieter_bytes.begin() + quieter_at + 13, quieter_bytes.begin() + quieter_at + 16,
                            std::array<uint8_t, 3>{0x9C, 24, 100}.begin()));
    const Sequence quieter_song = ReadOf(quieter, 0);
    SEQLIB_REQUIRE(TrackOf(quieter_song, 0) && TrackOf(quieter_song, 0)->notes.size() == 2);
    XSFLIB_CHECK(TrackOf(quieter_song, 0)->notes[0].velocity == 40);
    XSFLIB_CHECK(TrackOf(quieter_song, 0)->notes[1].velocity == 50);
    XSFLIB_CHECK(TrackOf(quieter_song, 0)->notes[1].length == 24);
    XSFLIB_CHECK(StartsOf(quieter_song, 1) == StartsOf(song0, 1));
    XSFLIB_CHECK(PlaysAsReference(quieter, 0));

    // A shorter note of one byte stores its length in its bytes. The wait after it keeps its length.
    const edit::NoteEdit shorter{
        .command = second.command, .stretch = track.stretches.at(second.stretch), .length = 12u};
    const Sequence shorter_song = ReadOf(edit::EditNotes(file, sequence, std::span(&shorter, 1)).file, 0);
    XSFLIB_CHECK(TrackOf(shorter_song, 0)->notes[1].length == 12);
    XSFLIB_CHECK(shorter_song.length == 48);

    // A move of a tick, and a removal.
    const edit::NoteEdit moved{.command = first.command, .stretch = stretch, .shift = 1};
    XSFLIB_CHECK(StartsOf(ReadOf(edit::EditNotes(file, sequence, std::span(&moved, 1)).file, 0), 0) ==
                 (std::vector<uint32_t>{1, 24}));
    const edit::NoteEdit removed{
        .command = second.command, .stretch = track.stretches.at(second.stretch), .remove = true};
    XSFLIB_CHECK(StartsOf(ReadOf(edit::EditNotes(file, sequence, std::span(&removed, 1)).file, 0), 0) ==
                 (std::vector<uint32_t>{0}));

    // A new note on the second track, at tick 24. A key past 0x5F is refused.
    const song::Track& other = *TrackOf(song0, 1);
    const edit::NewNote added{
        .stretch = other.stretches.at(other.notes[0].stretch), .tick = 24, .key = 64, .velocity = 100, .length = 12};
    const File added_file = edit::EditNotes(file, sequence, {}, std::span(&added, 1)).file;
    const Sequence with_new = ReadOf(added_file, 0);
    XSFLIB_CHECK(StartsOf(with_new, 1) == (std::vector<uint32_t>{0, 24}));
    SEQLIB_REQUIRE(TrackOf(with_new, 1)->notes.size() == 2);
    XSFLIB_CHECK(TrackOf(with_new, 1)->notes[1].key == 64 && TrackOf(with_new, 1)->notes[1].length == 12);
    XSFLIB_CHECK(TrackOf(with_new, 1)->notes[1].velocity == 100);
    XSFLIB_CHECK(StartsOf(with_new, 0) == StartsOf(song0, 0));
    XSFLIB_CHECK(PlaysAsReference(added_file, 0));

    edit::NewNote high = added;
    high.key = 0x60;
    XSFLIB_CHECK(
        !ErrorOf<std::invalid_argument>([&] { edit::EditNotes(file, sequence, {}, std::span(&high, 1)); }).empty());
}

// The key of a note that the Key Editor draws on a track without notes (edit::Rd2CommandKey). The arrangement shows a
// square's note at the pitch that ntdlib's converter assigns to it. The region of square 2 is tuned to 0x30. A note of
// key 60 therefore plays pitch index 60, and a square's sample plays that index on key 72. A transpose of 3 subtracts 3
// from the note's key. A sample's note and the noise's show at their keys. Each of those notes plays the voice of the
// type of its region (edit::Rd2VoiceTypeOf). Rd2VoiceTypeOf returns nothing for a key that no note shows at.
void TestCommandKeys()
{
    const std::vector<uint8_t> bytes = Cart();
    const File file = format::DriverOfRom(bytes)->Import(bytes);
    const uint32_t sequence = SequenceOf(file, 0);
    XSFLIB_CHECK(edit::Rd2CommandKey(file, sequence, 0, cart::kSquare, 0, 72) == 60);
    XSFLIB_CHECK(edit::Rd2CommandKey(file, sequence, 0, cart::kSquare, 3, 72) == 57);
    XSFLIB_CHECK(edit::Rd2CommandKey(file, sequence, 0, cart::kPlain, 0, 72) == 72);
    XSFLIB_CHECK(edit::Rd2CommandKey(file, sequence, 0, cart::kNoise, 0, 50) == 50);
    XSFLIB_CHECK(edit::Rd2VoiceTypeOf(file, sequence, 0, cart::kSquare, 72) == ntdlib::rd2::kSquare2Type);
    XSFLIB_CHECK(edit::Rd2VoiceTypeOf(file, sequence, 0, cart::kPlain, 72) == ntdlib::rd2::kSampleType);
    XSFLIB_CHECK(edit::Rd2VoiceTypeOf(file, sequence, 0, cart::kNoise, 50) == ntdlib::rd2::kNoiseType);
    XSFLIB_CHECK(edit::Rd2VoiceTypeOf(file, sequence, 0, cart::kWave, 50) == ntdlib::rd2::kWaveType);
    XSFLIB_CHECK(!edit::Rd2VoiceTypeOf(file, sequence, 0, cart::kSquare, 200));
}

// The mixer's volume of a track without a volume command starts from 128, and the edit puts the new volume in at the
// track's start: the song's header then starts the track at the new command. A track's volume command changes to the
// new value. A new pan and a send to the echo go in too.
void TestMix()
{
    const std::vector<uint8_t> bytes = Cart();
    const File file = format::DriverOfRom(bytes)->Import(bytes);
    const uint32_t sequence = SequenceOf(file, 0);
    const Sequence song0 = ReadOf(file, 0);

    const edit::MixEdit half{.setting = song::Mix::kVolume, .from = 128, .to = 64};
    const File half_file = edit::EditMix(file, sequence, 0, TrackOf(song0, 0)->stretches, half).file;
    const Sequence with_half = ReadOf(half_file, 0);
    SEQLIB_REQUIRE(TrackOf(with_half, 0) && !TrackOf(with_half, 0)->volume.empty());
    XSFLIB_CHECK(TrackOf(with_half, 0)->volume.front().tick == 0 && TrackOf(with_half, 0)->volume.front().value == 64);
    const std::vector<uint8_t> half_bytes = format::ExportArchive(half_file);
    const uint32_t half_at = SongAt(half_bytes, 0);
    XSFLIB_CHECK(half_bytes[half_at + 2] == 6 && half_bytes[half_at + 6] == 0xE0 && half_bytes[half_at + 7] == 64);
    XSFLIB_CHECK(PlaysAsReference(half_file, 0));
    XSFLIB_CHECK(StartsOf(with_half, 0) == StartsOf(song0, 0));

    std::vector<uint8_t> expected = bytes;
    expected[kSong0 + 19] = 0x30;
    const edit::MixEdit lower{.setting = song::Mix::kVolume, .from = 0x60, .to = 0x30};
    XSFLIB_CHECK(format::ExportArchive(edit::EditMix(file, sequence, 1, TrackOf(song0, 1)->stretches, lower).file) ==
                 expected);

    const edit::MixEdit panned{.setting = song::Mix::kPan, .from = 0, .to = 32};
    const Sequence with_pan = ReadOf(edit::EditMix(file, sequence, 1, TrackOf(song0, 1)->stretches, panned).file, 0);
    SEQLIB_REQUIRE(!TrackOf(with_pan, 1)->pan.empty());
    XSFLIB_CHECK(TrackOf(with_pan, 1)->pan.back().value == 32);

    const edit::MixEdit echo{.setting = song::Mix::kSendA, .from = 0, .to = 20};
    const File echo_file = edit::EditMix(file, sequence, 1, TrackOf(song0, 1)->stretches, echo).file;
    const Sequence with_echo = ReadOf(echo_file, 0);
    SEQLIB_REQUIRE(!TrackOf(with_echo, 1)->sends[0].empty());
    XSFLIB_CHECK(TrackOf(with_echo, 1)->sends[0].back().value == 20);
    XSFLIB_CHECK(PlaysAsReference(echo_file, 0));
}

// The inspector's Program menu assigns a track's notes another instrument of the bank in their slot.
void TestInstrument()
{
    const std::vector<uint8_t> bytes = Cart();
    const File file = format::DriverOfRom(bytes)->Import(bytes);
    const uint32_t sequence = SequenceOf(file, 0);
    const Sequence song0 = ReadOf(file, 0);
    std::vector<uint8_t> expected = bytes;
    expected[kSong0 + 7] = cart::kHeld;
    const File held = edit::EditInstrument(file, sequence, 0, TrackOf(song0, 0)->stretches,
                                           edit::Program{0, cart::kLooped}, edit::Program{0, cart::kHeld})
                          .file;
    XSFLIB_CHECK(format::ExportArchive(held) == expected);
    for (const song::Note& note : TrackOf(ReadOf(held, 0), 0)->notes)
    {
        XSFLIB_CHECK(note.program == cart::kHeld);
    }
}

// The Project menu's edits of tracks change the song's header. A sound effect allows none of them.
void TestTracks()
{
    const std::vector<uint8_t> bytes = Cart(true);
    const File file = format::DriverOfRom(bytes)->Import(bytes);
    const std::vector<std::optional<edit::SoundPlace>> places = PlacesOf(file);
    const Sequence song0 = ReadOf(file, 0);
    const Sequence song1 = ReadOf(file, 1);

    // A new track waits as long as the song. The header counts 3 tracks.
    const edit::TrackEdit added = edit::AddTrack(file, places, 0, song0, std::nullopt);
    XSFLIB_CHECK(added.track == 2);
    const std::vector<uint8_t> added_bytes = format::ExportArchive(added.file);
    XSFLIB_CHECK(added_bytes[SongAt(added_bytes, 0)] == 3);
    const Sequence with_new = ReadOf(added.file, 0);
    XSFLIB_CHECK(StartsOf(with_new, 0) == StartsOf(song0, 0));
    XSFLIB_CHECK(StartsOf(with_new, 1) == StartsOf(song0, 1));
    XSFLIB_CHECK(with_new.length == song0.length);

    // A new track with an instrument selects it, and a note there plays it.
    const edit::TrackEdit chosen = edit::AddTrack(file, places, 0, song0, edit::Program{0, cart::kHeld});
    const Sequence with_chosen = ReadOf(chosen.file, 0);
    const song::Track* empty = TrackOf(with_chosen, chosen.track);
    SEQLIB_REQUIRE(empty != nullptr && !empty->stretches.empty());
    const edit::NewNote first{.stretch = empty->stretches.front(), .tick = 0, .key = 60, .velocity = 100, .length = 12};
    const Sequence with_note =
        ReadOf(edit::EditNotes(chosen.file, SequenceOf(file, 0), {}, std::span(&first, 1)).file, 0);
    SEQLIB_REQUIRE(TrackOf(with_note, chosen.track)->notes.size() == 1);
    XSFLIB_CHECK(TrackOf(with_note, chosen.track)->notes[0].program == cart::kHeld);
    XSFLIB_CHECK(StartsOf(with_note, 0) == StartsOf(song0, 0));

    // A copy plays the track's notes on a new track.
    const edit::TrackEdit copy = edit::DuplicateTrack(file, places, 0, 1);
    XSFLIB_CHECK(copy.track == 2);
    const Sequence with_copy = ReadOf(copy.file, 0);
    XSFLIB_CHECK(StartsOf(with_copy, 2) == StartsOf(song0, 1));
    XSFLIB_CHECK(StartsOf(with_copy, 1) == StartsOf(song0, 1));
    XSFLIB_CHECK(PlaysAsReference(copy.file, 0));

    // A removed track's place in the header names no track. The song's last track stays.
    const File without = edit::RemoveTrack(file, places, 0, 1);
    const Sequence without_second = ReadOf(without, 0);
    XSFLIB_CHECK(StartsOf(without_second, 1).empty());
    XSFLIB_CHECK(StartsOf(without_second, 0) == StartsOf(song0, 0));
    XSFLIB_CHECK(!ErrorOf<std::invalid_argument>([&] { edit::RemoveTrack(without, PlacesOf(without), 0, 0); }).empty());

    // A track of the first song is copied into the second, with the bank of its slot.
    const edit::CopiedTrack copied = edit::CopyTrack(file, places, 0, song0, 0);
    XSFLIB_CHECK(copied.driver == format::Engine::kRd2);
    const edit::TrackEdit pasted = edit::PasteTrack(file, places, 1, song1, copied);
    XSFLIB_CHECK(pasted.track == 1);
    const Sequence with_paste = ReadOf(pasted.file, 1);
    XSFLIB_CHECK(StartsOf(with_paste, 1) == StartsOf(song0, 0));
    XSFLIB_CHECK(TrackOf(with_paste, 1)->notes[0].program == cart::kLooped);
    XSFLIB_CHECK(!StartsOf(with_paste, 0).empty() && StartsOf(with_paste, 0).front() == 0);
    XSFLIB_CHECK(StartsOf(ReadOf(pasted.file, 0), 0) == StartsOf(song0, 0));

    // The sound effect plays its track of the sound effects' sequence.
    const Sequence effect = ReadOf(file, 2);
    XSFLIB_CHECK(!effect.tracks.empty() && effect.tracks.front().notes.size() == 1);
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { edit::AddTrack(file, places, 2, effect, std::nullopt); })
                     .find("sound effect") != std::string::npos);
}

// A song whose track 1 nests calls 4 deep (F4 calls, at 14, 18, 22 and 26). The driver keeps 3 places
// (ntdlib::rd2::kMaxDepth) and frees a track that nests deeper. The fourth subroutine and the ends after the calls are
// then bytes that no track reads. Hokuto can't tell what such a track does, and the refusal of its removal says so.
void TestRemoveUnreadTrack()
{
    cart::Cart image;
    image.Sequences({{{0xC2, cart::kHeld, 0x9C, 24, 100, 0xC1, 24, 0xFF},
                      {0xF4, 18, 0, 0xFF, 0xF4, 22, 0, 0xFF, 0xF4, 26, 0, 0xFF, 0xF4, 30, 0, 0xFF, 0xC1, 24, 0xFF}}});
    const std::vector<uint8_t> bytes = RomBytes(image.ToRom());
    const File file = format::DriverOfRom(bytes)->Import(bytes);

    const std::string refusal = ErrorOf<std::invalid_argument>([&] { edit::RemoveTrack(file, PlacesOf(file), 0, 1); });

    XSFLIB_CHECK(refusal.ends_with("Hokuto can't remove it."));
}

// The exports equal those of ntdlib's converter, run directly.
void TestExports()
{
    const std::shared_ptr<const Rd2Rom> rom = song::LoadRd2Rom(Cart());
    for (const uint32_t sound : {0u, 1u})
    {
        ntdlib::sequence_export::agb::ConvertSettings settings;
        settings.program = "Hokuto";
        const ntdlib::sequence_export::rd2::SongFiles expected =
            ntdlib::sequence_export::rd2::ConvertSongFiles(rom->rom, rom->info, static_cast<int>(sound), settings);
        const ntdlib::sequence_export::Conversion conversion = rom->ConvertToMidi(sound, "Hokuto");
        XSFLIB_CHECK(expected.report.ok);
        XSFLIB_CHECK(conversion.midi == expected.midi);
        XSFLIB_CHECK(conversion.sf2 == expected.sf2);
        XSFLIB_CHECK(!conversion.midi.empty() && !conversion.sf2.empty());
    }

    const ntdlib::sequence_export::BankConversion bank = rom->ConvertSamples(rom->Samples(), "Samples");
    XSFLIB_CHECK(!bank.sound_fonts.empty() && !bank.sound_fonts.front().sf2.empty());
}

// A cart whose one song plays the drum kit's first drum, note 36: the plain sample (sample 1). The plain instrument and
// the key split play the looped sample (sample 0) in its place. Only the drum kit and the instrument with a sample for
// each key play sample 1.
std::vector<uint8_t> DrumCart()
{
    cart::Cart image;
    image.Sequences({{cart::TrackData().Bytes({0xC2, cart::kDrums}).Note(36, 24, 100).Wait(24).End()}});
    std::vector<uint8_t> bytes = RomBytes(image.ToRom());
    const auto at = [](uint32_t address)
    {
        return static_cast<std::size_t>(address - ntdlib::agb::kRomBase);
    };
    const std::size_t looped = at(cart::kBank + 2 * cart::kLooped);
    for (const uint8_t instrument : {cart::kPlain, cart::kSplit})
    {
        bytes[at(cart::kBank + 2 * instrument)] = bytes[looped];
        bytes[at(cart::kBank + 2 * instrument) + 1] = bytes[looped + 1];
    }

    return bytes;
}

// The program and the key of a sample's preview.
std::pair<int64_t, int> ProgramAndKey(const song::DriverAudition& audition)
{
    return {audition.setup.at(1).value.value / ntdlib::commands::kOne, audition.note.key};
}

// The samples of a sound are those that its notes play, not every sample of the sample sets of its banks. A drum's note
// counts. The preview of a sample plays it on an instrument of the bank at the key that the region's tuning gives. A
// sample that only a drum plays sounds on the drum's key, whatever the key pressed.
void TestSamplesOfSounds()
{
    const std::shared_ptr<const Rd2Rom> rom = song::LoadRd2Rom(Cart());
    XSFLIB_CHECK(rom->SamplesOfSound(0) == (std::vector<int>{0}));
    XSFLIB_CHECK(rom->SamplesOfSound(1) == (std::vector<int>{0}));
    XSFLIB_CHECK(ProgramAndKey(rom->SampleAudition(0, 60, 100)) == (std::pair<int64_t, int>{cart::kLooped, 60}));
    XSFLIB_CHECK(ProgramAndKey(rom->SampleAudition(1, 60, 100)) == (std::pair<int64_t, int>{cart::kPlain, 48}));

    const std::shared_ptr<const Rd2Rom> drum = song::LoadRd2Rom(DrumCart());
    XSFLIB_CHECK(drum->SamplesOfSound(0) == (std::vector<int>{1}));
    for (const int key : {36, 50, 72})
    {
        XSFLIB_CHECK(ProgramAndKey(drum->SampleAudition(1, key, 100)) == (std::pair<int64_t, int>{cart::kDrums, 36}));
    }
}

// The model's clock follows the arrangement: after 10 s of play, or at the song's end, the arrangement's time at the
// clock is within 2 frames of the time played.
void TestClockFollowsArrangement()
{
    const std::shared_ptr<const Rd2Rom> rom = song::LoadRd2Rom(Cart(true));
    int long_runs = 0;
    for (const uint32_t sound : rom->Sounds())
    {
        const ClockRun run = ClockDrift(rom, sound, 600);
        XSFLIB_CHECK(std::abs(run.drift) <= 2.0);
        long_runs += run.played >= 500 ? 1 : 0;
    }

    // One sound at least plays on: a clock at 60 frames a second in place of 59.73 drifts 2.7 frames by 600.
    XSFLIB_CHECK(long_runs > 0);
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("rd2",
                                    []
                                    {
                                        TestImport();
                                        TestRead();
                                        TestPlays();
                                        TestNoteEdits();
                                        TestCommandKeys();
                                        TestMix();
                                        TestInstrument();
                                        TestTracks();
                                        TestRemoveUnreadTrack();
                                        TestExports();
                                        TestSamplesOfSounds();
                                        TestClockFollowsArrangement();
                                    });
}
