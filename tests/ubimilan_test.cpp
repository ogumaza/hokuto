// SPDX-License-Identifier: MIT

// Tests Hokuto's path for Ubisoft Milan's GBA driver: the import and the round trip, the pieces that the arrangement
// shows, playback, the edits of notes and tracks, and the exports. ntdlib's test image of the driver (ubimilan_image.h
// in ntdlib's tests) contains no game data. It contains the code that ntdlib's detection reads, the PSG's tables, a
// sample for the kit's key 40 and the sequences below.
//
// The expected ticks, keys, velocities and bytes come from the sequences' bytes, worked out by hand by the driver's
// rules (ntdlib/ubimilan/commands.h). A sequence's first countdown of n runs its first command n frames into the
// piece. Each later countdown of n runs the next command n + 1 frames later, and one of 0 in the same frame. When the
// list of a piece starts again, its first countdown of n runs the first command n + 1 frames later too. A file
// records the length of each wait in ticks (format/ubimilan.h). The Key Editor shows a kit note's volume of 64 and a
// square's envelope of 15 as velocity 127.
//
// ntdlib's ArchivePlayer, run directly, makes the reference output. The model's output has to match it sample for
// sample, from the start, in both mixing modes. The MIDI file and the SoundFont have to equal those of ntdlib's
// converter, run directly.

#include "song/ubimilan.h"

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
#include "ntdlib/commands/sequence.h"
#include "ntdlib/sequence_export/agb/settings.h"
#include "ntdlib/sequence_export/convert.h"
#include "ntdlib/sequence_export/ubimilan/convert.h"
#include "ntdlib/testing/ubimilan_image.h"
#include "ntdlib/ubimilan/archive_player.h"
#include "ntdlib/ubimilan/driver.h"
#include "xsflib/testing/check.h"

#include "driver_test_support.h"
#include "edit/notes.h"
#include "edit/replace.h"
#include "edit/tracks.h"
#include "format/archive.h"
#include "format/drivers.h"
#include "format/file.h"
#include "format/text.h"
#include "song/archive.h"
#include "song/sequence.h"
#include "song/sequence_model.h"

namespace
{

namespace format = hokuto::format;
namespace song = hokuto::song;
namespace edit = hokuto::edit;
using format::File;
using hokuto::testing::ClockDrift;
using hokuto::testing::ClockRun;
using hokuto::testing::HasSound;
using hokuto::testing::ModelPlaysAsReference;
using hokuto::testing::Play;
using hokuto::testing::Played;
using hokuto::testing::RomBytes;
using hokuto::testing::RunDirect;
using hokuto::testing::SequenceAt;
using hokuto::testing::StartsOf;
using hokuto::testing::TrackOf;
using ntdlib::commands::kOne;
using ntdlib::testing::ubimilan_image::TestImage;
using song::Mixing;
using song::Sequence;
using song::SequenceModel;
using song::UbiMilanRom;
using xsflib::testing::ErrorOf;

// Piece 0, from the sequence's first command, after its 4 bytes:
//
//    4: 02 00        the first countdown: the first command runs 2 frames in
//    6: 99 28 40     a kit note of key 40 at volume 64, at tick 2
//    9: 0B 00        12 frames later
//   11: 89 28 00     the kit's note-off, at tick 14
//   14: 00 00        the same frame
//   16: 90 3C F0     a note of square 1 at key 60, with the envelope's top 4 bits at 15, at tick 14
//   19: 17 00        24 frames later
//   21: 80 3C 00     its note-off, at tick 38
//   24: 09 00        10 frames later
//   26: FC           the end, at tick 48
//
// Program 0 of square 1 and of square 2 sets a duty of 50 % and starts the channel (NRx4's bit 7): a PSG note is a run
// of frames in which its channel sounds.
//
// Piece 1 loops over one sequence: a kit note at tick 2 that lasts 48 frames, and the end 4 frames after its note-off,
// at tick 54. The list then starts again, and the next pass's kit note comes 3 frames in, at tick 57.
std::vector<uint8_t> Image()
{
    TestImage image;
    image.SetKey(40, image.AddSample(std::vector<int8_t>(40000, 48)));
    image.SetInstrument(0, 0, {{0, 0x80, 0, 0x80}});
    image.SetInstrument(1, 0, {{0x80, 0, 0x80}});
    image.AddPiece(0, {image.AddSequence({0x02, 0x00, 0x99, 40,   64,   0x0B, 0x00, 0x89, 40,   0,    0x00, 0x00,
                                          0x90, 60,   0xF0, 0x17, 0x00, 0x80, 60,   0,    0x09, 0x00, 0xFC})});
    image.AddPiece(1, {image.AddSequence({0x02, 0x00, 0x99, 40, 64, 0x2F, 0x00, 0x89, 40, 0, 0x03, 0x00, 0xFC})});

    return RomBytes(image.ToRom());
}

Sequence ReadOf(const File& file, uint32_t sound)
{
    return song::LoadUbiMilanRom(format::ExportArchive(file))->ReadSequence(sound);
}

// Where each sound of a file plays. The program finds each place the same way: the sequence that the resource table
// names for the piece's first sequence.
std::vector<std::optional<edit::SoundPlace>> PlacesOf(const File& file)
{
    const std::shared_ptr<const UbiMilanRom> rom = song::LoadUbiMilanRom(format::ExportArchive(file));
    std::vector<std::optional<uint32_t>> sequences(rom->SoundCount());
    std::vector<std::vector<uint32_t>> fields(rom->SoundCount());
    for (const uint32_t sound : rom->Sounds())
    {
        sequences[sound] = format::SequenceNamedAt(file, *rom->FieldsOf(sound).sequence);
    }

    return edit::GbaSoundPlaces(sequences, fields);
}

uint32_t SequenceOf(const File& file, uint32_t sound)
{
    return SequenceAt(PlacesOf(file), sound);
}

// The reference: ntdlib's player run directly up to `samples` stereo samples.
template <typename Sample>
std::vector<Sample> Direct(const UbiMilanRom& rom, int song, std::size_t samples, bool enhanced)
{
    ntdlib::ubimilan::ArchivePlayer player(rom.rom, rom.info);
    player.Start(song, {.enhanced = enhanced});
    return RunDirect<Sample>(player, samples);
}

// The frame in which ntdlib's player, run directly, stops sounding: its track has ended and its voices have fallen
// silent.
uint64_t EndFrame(const UbiMilanRom& rom, int song)
{
    ntdlib::ubimilan::ArchivePlayer player(rom.rom, rom.info);
    player.Start(song);
    while (player.Busy() && player.Frames() < 100000)
    {
        player.RunFrame();
    }

    return player.Frames();
}

// The model plays `file`'s sound `sound` as the reference does, in strict mixing.
bool PlaysAsReference(const File& file, uint32_t sound)
{
    const std::shared_ptr<const UbiMilanRom> rom = song::LoadUbiMilanRom(format::ExportArchive(file));
    return ModelPlaysAsReference(rom, sound, [&](std::size_t samples)
                                 { return Direct<int16_t>(*rom, static_cast<int>(sound), samples, false); });
}

// A ROM with the driver comes in as the format's engine of Ubisoft Milan's driver, and comes back byte for byte from
// both forms of the file. The file records the length of each wait in ticks.
void TestImport()
{
    const std::vector<uint8_t> bytes = Image();
    const format::Driver* driver = format::DriverOfRom(bytes);
    SEQLIB_REQUIRE(driver != nullptr);
    XSFLIB_CHECK(driver->Facts().engine == format::Engine::kUbiMilan);

    const File file = driver->Import(bytes);
    XSFLIB_CHECK(file.engine == format::Engine::kUbiMilan);
    XSFLIB_CHECK(format::ExportArchive(file) == bytes);
    const File binary = format::Read(format::Write(file));
    XSFLIB_CHECK(binary.engine == format::Engine::kUbiMilan);
    XSFLIB_CHECK(format::ExportArchive(binary) == bytes);
    const File text = format::ReadText(format::WriteText(file));
    XSFLIB_CHECK(format::ExportArchive(text) == bytes);
    XSFLIB_CHECK(format::Write(text) == format::Write(file));

    // The first wait lasts 2 ticks, and the others 12, 0, 24 and 10.
    const ntdlib::commands::Sequence& commands = *file.sequences[SequenceOf(file, 0)];
    std::vector<int64_t> waits;
    for (const ntdlib::commands::Command& command : commands.items)
    {
        if (command.kind == ntdlib::commands::Kind::kWait)
        {
            waits.push_back(command.value.value / kOne);
        }
    }
    XSFLIB_CHECK(waits == (std::vector<int64_t>{2, 12, 0, 24, 10}));

    // An export can't put the driver's sequences into filler runs.
    File filler = file;
    filler.native.Edit([](format::NativeArchive& native) { native.gba_filler = true; });
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { format::Write(filler); }).find("filler run") !=
                 std::string::npos);
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { format::WriteText(filler); }).find("filler run") !=
                 std::string::npos);
    XSFLIB_CHECK(!format::LacksRoom(file));
}

// The pieces that the arrangement shows. A track is a channel, and each one contains the stretches of the piece. The
// second pass of piece 1's list starts a tick after the frame where the list starts again: its first countdown of 2
// runs the note 3 frames after that frame, and the file's first wait lasts 2 ticks.
void TestRead()
{
    const std::shared_ptr<const UbiMilanRom> rom = song::LoadUbiMilanRom(Image());
    XSFLIB_CHECK(rom->Sounds() == (std::vector<uint32_t>{0, 1}));
    XSFLIB_CHECK(rom->Rules().DriverEngine() == format::Engine::kUbiMilan);

    // The driver's bytes hold waits, notes, note-offs and programs, and no volume or pan.
    song::Console console;
    console.driver = &rom->Rules();
    using ntdlib::commands::Kind;
    for (const Kind kind : {Kind::kWait, Kind::kNote, Kind::kNoteOff, Kind::kProgram, Kind::kEnd})
    {
        XSFLIB_CHECK(song::Runs(console, kind, 9));
    }
    XSFLIB_CHECK(!song::Runs(console, Kind::kVolume, 0) && !song::Runs(console, Kind::kPan, 0));
    XSFLIB_CHECK(!song::Runs(console, Kind::kNote, 16));

    const Sequence piece0 = rom->ReadSequence(0);
    XSFLIB_CHECK(!piece0.loop);
    // The end runs in frame 48, and the piece stops with that frame. The player, run directly, sounds a frame longer:
    // the FIFOs play their last bytes then.
    XSFLIB_CHECK(piece0.length == 49);
    XSFLIB_CHECK(EndFrame(*rom, 0) == piece0.length + 1);
    SEQLIB_REQUIRE(TrackOf(piece0, 0) && TrackOf(piece0, 9));
    const song::Track& kit = *TrackOf(piece0, 9);
    SEQLIB_REQUIRE(kit.notes.size() == 1);
    XSFLIB_CHECK(kit.notes[0].start == 2 && kit.notes[0].length == 12 && kit.notes[0].key == 40);
    XSFLIB_CHECK(kit.notes[0].velocity == 127 && kit.notes[0].command == 6);
    const song::Track& square = *TrackOf(piece0, 0);
    SEQLIB_REQUIRE(square.notes.size() == 1);
    XSFLIB_CHECK(square.notes[0].start == 14 && square.notes[0].length == 24 && square.notes[0].key == 60);
    XSFLIB_CHECK(square.notes[0].velocity == 127 && square.notes[0].command == 16);
    XSFLIB_CHECK(square.stretches == kit.stretches);

    const Sequence piece1 = rom->ReadSequence(1);
    SEQLIB_REQUIRE(piece1.loop.has_value());
    XSFLIB_CHECK(piece1.loop->start == 54);
    XSFLIB_CHECK(StartsOf(piece1, 9) == (std::vector<uint32_t>{2, 57}));
    SEQLIB_REQUIRE(TrackOf(piece1, 9)->stretches.size() == 2);
    XSFLIB_CHECK(TrackOf(piece1, 9)->stretches[0].tick == 0);
    XSFLIB_CHECK(TrackOf(piece1, 9)->stretches[1].tick == 55);
}

// The model plays as ntdlib's player, run directly, from the start in both mixing modes.
void TestPlays()
{
    const std::shared_ptr<const UbiMilanRom> rom = song::LoadUbiMilanRom(Image());
    for (const uint32_t sound : {0u, 1u})
    {
        const auto sequence = std::make_shared<const Sequence>(rom->ReadSequence(sound));
        SequenceModel strict(rom, sequence,
                             hokuto::song::ModelOptions{.mixing = Mixing::kStrict, .snapshot_interval = 20});
        const Played played = Play(strict, 120);
        XSFLIB_CHECK(HasSound(std::span<const int16_t>(played.samples)));
        const std::vector<int16_t> expected =
            Direct<int16_t>(*rom, static_cast<int>(sound), played.samples.size() + 4096, false);
        XSFLIB_CHECK(std::equal(played.samples.begin(), played.samples.end(), expected.begin()));

        SequenceModel enhanced(rom, sequence,
                               hokuto::song::ModelOptions{.mixing = Mixing::kEnhanced, .snapshot_interval = 20});
        const Played enhanced_played = Play(enhanced, 120);
        XSFLIB_CHECK(HasSound(std::span<const float>(enhanced_played.floats)));
        const std::vector<float> enhanced_expected =
            Direct<float>(*rom, static_cast<int>(sound), enhanced_played.floats.size() + 4096, true);
        XSFLIB_CHECK(
            std::equal(enhanced_played.floats.begin(), enhanced_played.floats.end(), enhanced_expected.begin()));
    }
}

// The edits of notes lay the sequence's commands out at their ticks.
void TestNoteEdits()
{
    const std::vector<uint8_t> bytes = Image();
    const File file = format::DriverOfRom(bytes)->Import(bytes);
    const uint32_t sequence = SequenceOf(file, 0);
    const Sequence piece0 = ReadOf(file, 0);
    const song::Note& kit = TrackOf(piece0, 9)->notes[0];
    const song::Note& square = TrackOf(piece0, 0)->notes[0];
    const song::Stretch& stretch = TrackOf(piece0, 0)->stretches.at(0);

    // A velocity of 90, 127 times the square root of 32 / 64, sets the kit's note to the volume 32.
    const edit::NoteEdit quieter{.command = kit.command, .stretch = stretch, .velocity = 90};
    const File quieter_file = edit::EditNotes(file, sequence, std::span(&quieter, 1)).file;
    XSFLIB_CHECK(quieter_file.sequences[sequence]->items[2].velocity == 32 * kOne);
    XSFLIB_CHECK(TrackOf(ReadOf(quieter_file, 0), 9)->notes[0].velocity == 90);
    XSFLIB_CHECK(PlaysAsReference(quieter_file, 0));

    // A move of 4 ticks moves the square's note and its note-off to ticks 18 and 42. The end stays at tick 48.
    const edit::NoteEdit later{.command = square.command, .stretch = stretch, .shift = 4};
    const File later_file = edit::EditNotes(file, sequence, std::span(&later, 1)).file;
    const Sequence later_piece = ReadOf(later_file, 0);
    XSFLIB_CHECK(StartsOf(later_piece, 0) == (std::vector<uint32_t>{18}));
    XSFLIB_CHECK(TrackOf(later_piece, 0)->notes[0].length == 24);
    XSFLIB_CHECK(StartsOf(later_piece, 9) == (std::vector<uint32_t>{2}));
    XSFLIB_CHECK(later_piece.length == 49);
    XSFLIB_CHECK(PlaysAsReference(later_file, 0));

    // A move of 1 tick would put the note a tick after the kit's note-off.
    const edit::NoteEdit tick{.command = square.command, .stretch = stretch, .shift = 1};
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { edit::EditNotes(file, sequence, std::span(&tick, 1)); })
                     .find("one tick apart") != std::string::npos);

    // A length of 10 moves the note-off to tick 24.
    const edit::NoteEdit shorter{.command = square.command, .stretch = stretch, .length = 10u};
    const File shorter_file = edit::EditNotes(file, sequence, std::span(&shorter, 1)).file;
    const Sequence shorter_piece = ReadOf(shorter_file, 0);
    XSFLIB_CHECK(TrackOf(shorter_piece, 0)->notes[0].length == 10);
    XSFLIB_CHECK(shorter_piece.length == 49);

    // A length that moves the note-off past the end is refused.
    const edit::NoteEdit longer{.command = square.command, .stretch = stretch, .length = 40u};
    XSFLIB_CHECK(
        !ErrorOf<std::invalid_argument>([&] { edit::EditNotes(file, sequence, std::span(&longer, 1)); }).empty());

    // A removal drops the note and the note-off of the kit. The kit channel then leaves the arrangement.
    const edit::NoteEdit removed{.command = kit.command, .stretch = stretch, .remove = true};
    const Sequence without = ReadOf(edit::EditNotes(file, sequence, std::span(&removed, 1)).file, 0);
    XSFLIB_CHECK(TrackOf(without, 9) == nullptr);
    XSFLIB_CHECK(StartsOf(without, 0) == (std::vector<uint32_t>{14}));

    // A new note of the kit at tick 30, 6 ticks long, at velocity 127: the volume 64.
    const edit::NewNote added{.stretch = stretch, .tick = 30, .key = 40, .velocity = 127, .length = 6, .index = 9};
    const File added_file = edit::EditNotes(file, sequence, {}, std::span(&added, 1)).file;
    const Sequence with_new = ReadOf(added_file, 0);
    XSFLIB_CHECK(StartsOf(with_new, 9) == (std::vector<uint32_t>{2, 30}));
    XSFLIB_CHECK(TrackOf(with_new, 9)->notes[1].length == 6 && TrackOf(with_new, 9)->notes[1].velocity == 127);
    XSFLIB_CHECK(StartsOf(with_new, 0) == (std::vector<uint32_t>{14}));
    XSFLIB_CHECK(PlaysAsReference(added_file, 0));

    // A wait of 2 ticks becomes a countdown of 1. The square's note still shows the velocity of its byte after a new
    // kit note 2 ticks after the square's note.
    const edit::NoteEdit softer{.command = square.command, .stretch = stretch, .velocity = 90};
    const File softer_file = edit::EditNotes(file, sequence, std::span(&softer, 1)).file;
    const Sequence softer_piece = ReadOf(softer_file, 0);
    const int softer_velocity = TrackOf(softer_piece, 0)->notes[0].velocity;
    XSFLIB_CHECK(softer_velocity < 127);
    const edit::NewNote close{.stretch = TrackOf(softer_piece, 0)->stretches.at(0),
                              .tick = 16,
                              .key = 40,
                              .velocity = 127,
                              .length = 6,
                              .index = 9};
    const Sequence close_piece = ReadOf(edit::EditNotes(softer_file, sequence, {}, std::span(&close, 1)).file, 0);
    XSFLIB_CHECK(StartsOf(close_piece, 9) == (std::vector<uint32_t>{2, 16}));
    XSFLIB_CHECK(TrackOf(close_piece, 0)->notes[0].velocity == softer_velocity);

    // A transpose can't take the kit's note to the noise's keys.
    const edit::NoteEdit noise{.command = kit.command, .stretch = stretch, .transpose = -35};
    XSFLIB_CHECK(
        ErrorOf<std::invalid_argument>([&] { edit::EditNotes(file, sequence, std::span(&noise, 1)); }).find("noise") !=
        std::string::npos);
}

// The wave's velocity byte sets the top 3 bits of NR32. Bits 5 and 6 set 100 %, 50 % or 25 %, and bit 7 sets 75 %
// whatever their values (GBATEK, SOUND3CNT_H). The Key Editor shows 127 times the square root of the level, and a
// velocity converts back to the nearest level. The byte keeps its low 5 bits.
void TestWaveLevels()
{
    const int wave = ntdlib::ubimilan::kWaveChannel;
    XSFLIB_CHECK(song::UbiMilanVelocityOf(wave, 60, 0x20) == 127);
    XSFLIB_CHECK(song::UbiMilanVelocityOf(wave, 60, 0x40) == 90);
    XSFLIB_CHECK(song::UbiMilanVelocityOf(wave, 60, 0x60) == 64);
    XSFLIB_CHECK(song::UbiMilanVelocityOf(wave, 60, 0x80) == 110);
    XSFLIB_CHECK(song::UbiMilanVelocityOf(wave, 60, 0xE0) == 110);
    XSFLIB_CHECK(song::UbiMilanVelocityOf(wave, 60, 0x00) == 0);

    XSFLIB_CHECK(song::UbiMilanDriverVelocity(wave, 60, 110, 0x20) == 0x80);
    XSFLIB_CHECK(song::UbiMilanDriverVelocity(wave, 60, 110, 0x3F) == 0x9F);
    XSFLIB_CHECK(song::UbiMilanDriverVelocity(wave, 60, 127, 0x80) == 0x20);
    XSFLIB_CHECK(song::UbiMilanDriverVelocity(wave, 60, 90, 0x80) == 0x40);
    XSFLIB_CHECK(song::UbiMilanDriverVelocity(wave, 60, 64, 0x80) == 0x60);
    XSFLIB_CHECK(song::UbiMilanDriverVelocity(wave, 60, 1, 0x20) == 0x60);
}

// The Project menu's edits of tracks take channels of the PSG.
void TestTracks()
{
    const std::vector<uint8_t> bytes = Image();
    const File file = format::DriverOfRom(bytes)->Import(bytes);
    const std::vector<std::optional<edit::SoundPlace>> places = PlacesOf(file);
    const Sequence piece0 = ReadOf(file, 0);
    const Sequence piece1 = ReadOf(file, 1);

    // A new track uses square 2, channel 1, with a program change at tick 2. A note there plays.
    const edit::TrackEdit added = edit::AddTrack(file, places, 0, piece0, std::nullopt);
    XSFLIB_CHECK(added.track == 1);
    const Sequence with_new = ReadOf(added.file, 0);
    SEQLIB_REQUIRE(TrackOf(with_new, 1) != nullptr);
    XSFLIB_CHECK(TrackOf(with_new, 1)->notes.empty());
    XSFLIB_CHECK(StartsOf(with_new, 0) == StartsOf(piece0, 0));
    XSFLIB_CHECK(StartsOf(with_new, 9) == StartsOf(piece0, 9));
    const edit::NewNote note{.stretch = TrackOf(with_new, 1)->stretches.at(0),
                             .tick = 30,
                             .key = 64,
                             .velocity = 127,
                             .length = 4,
                             .index = 1};
    const Sequence with_note =
        ReadOf(edit::EditNotes(added.file, SequenceOf(added.file, 0), {}, std::span(&note, 1)).file, 0);
    XSFLIB_CHECK(StartsOf(with_note, 1) == (std::vector<uint32_t>{30}));

    // A copy of square 1 plays its notes on square 2.
    const edit::TrackEdit copy = edit::DuplicateTrack(file, places, 0, 0);
    XSFLIB_CHECK(copy.track == 1);
    XSFLIB_CHECK(StartsOf(ReadOf(copy.file, 0), 1) == StartsOf(piece0, 0));
    XSFLIB_CHECK(PlaysAsReference(copy.file, 0));

    // The driver has one kit channel.
    XSFLIB_CHECK(!ErrorOf<std::invalid_argument>([&] { edit::DuplicateTrack(file, places, 0, 9); }).empty());

    // A removal drops the commands of square 1.
    const Sequence without = ReadOf(edit::RemoveTrack(file, places, 0, 0), 0);
    XSFLIB_CHECK(TrackOf(without, 0) == nullptr);
    XSFLIB_CHECK(StartsOf(without, 9) == StartsOf(piece0, 9));

    // The track of square 1 in piece 0 is copied into piece 1 at the same ticks. Its note runs at tick 14 of the first
    // pass, and at tick 69 of the second: 14 ticks and the frame of the first countdown after the list starts again.
    const edit::CopiedTrack copied = edit::CopyTrack(file, places, 0, piece0, 0);
    XSFLIB_CHECK(copied.driver == format::Engine::kUbiMilan);
    const edit::TrackEdit pasted = edit::PasteTrack(file, places, 1, piece1, copied);
    XSFLIB_CHECK(pasted.track == 0);
    const Sequence with_paste = ReadOf(pasted.file, 1);
    XSFLIB_CHECK(StartsOf(with_paste, 0) == (std::vector<uint32_t>{14, 69}));
    XSFLIB_CHECK(StartsOf(with_paste, 9) == StartsOf(piece1, 9));
}

// The exports equal those of ntdlib's converter, run directly.
void TestExports()
{
    const std::shared_ptr<const UbiMilanRom> rom = song::LoadUbiMilanRom(Image());
    for (const uint32_t sound : {0u, 1u})
    {
        ntdlib::sequence_export::agb::ConvertSettings settings;
        settings.program = "Hokuto";
        const ntdlib::sequence_export::ubimilan::SongFiles expected =
            ntdlib::sequence_export::ubimilan::ConvertSongFiles(rom->rom, rom->info, static_cast<int>(sound), settings);
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
// clock is within 2 frames of the time played.
void TestClockFollowsArrangement()
{
    int long_runs = 0;
    const std::shared_ptr<const UbiMilanRom> rom = song::LoadUbiMilanRom(Image());
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
    return xsflib::testing::RunTest("ubimilan",
                                    []
                                    {
                                        TestImport();
                                        TestRead();
                                        TestPlays();
                                        TestClockFollowsArrangement();
                                        TestNoteEdits();
                                        TestWaveLevels();
                                        TestTracks();
                                        TestExports();
                                    });
}
