// SPDX-License-Identifier: MIT

// Tests Hokuto's path for Krawall: the import and the round trip, the pieces that the arrangement shows, playback, the
// edits of notes, the refused edits of tracks, the mix and instruments, and the exports. ntdlib's test cartridge of
// the player (ntdlib/testing/krawall_cart.h) contains no game data. It contains the code patterns that ntdlib's
// detection reads, the player's tables, three samples and the modules below.
//
// The expected ticks, keys and programs come from the modules' rows, worked out by hand by the rules of the player and
// of ntdlib's conversion. The test modules are S3M modules: a row's instrument n plays sample n - 1 of the game's
// table. They play at speed 3, the ticks of a row. At the module's one speed, the conversion maps each tick of the
// player to one MIDI tick (ntdlib/sequence_export/krawall/performance.h). A row therefore lasts 3 ticks. A note from 1
// for C-0 plays MIDI key 11 more, with the sample's relative note of 0: C-4, note 49, plays key 60.
//
// ntdlib's ArchivePlayer, run directly, makes the reference output. The model's output has to match it sample for
// sample, from the start, in both mixing modes. The MIDI file and the SoundFont have to equal those of ntdlib's
// converter, run directly.
//
// A module whose rows take two speeds checks the model's ticks. The expected ticks come from the timing rules of
// ntdlib's conversion (MakeTiming in ntdlib/src/sequence_export/krawall/convert.cpp), worked out by hand for the
// module's rows: see TwoSpeedImage.
//
// The expected notes and ticks of the edits of removals and moves come from the rules of the player in ntdlib's model
// (Player::PlayEntry, Player::Effect and Player::SetPos in ntdlib/src/krawall/player.cpp), worked out by hand for each
// module's rows. A note delay plays the channel's note at its tick, with or without a note in its entry. In a module
// without instruments (S3M), a row's instrument sets the channel's sample and volume, and a note without one uses
// them. In a module with instruments (XM), an instrument takes effect only with a note, and a note without one plays
// the channel's instrument. XmImage returns a module with instruments.

#include "song/krawall.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/agb/gba.h"
#include "ntdlib/agb/rom.h"
#include "ntdlib/commands/sequence.h"
#include "ntdlib/krawall/archive_player.h"
#include "ntdlib/krawall/commands.h"
#include "ntdlib/krawall/driver.h"
#include "ntdlib/krawall/instruments.h"
#include "ntdlib/sequence_export/agb/settings.h"
#include "ntdlib/sequence_export/convert.h"
#include "ntdlib/sequence_export/krawall/convert.h"
#include "ntdlib/testing/krawall_cart.h"
#include "seqlib/wav.h"
#include "xsflib/testing/check.h"

#include "driver_test_support.h"
#include "edit/bytecode.h"
#include "edit/commands.h"
#include "edit/instruments.h"
#include "edit/mix.h"
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
namespace cart = ntdlib::testing::krawall_cart;
using format::File;
using hokuto::testing::ClockDrift;
using hokuto::testing::ClockRun;
using hokuto::testing::HasSound;
using hokuto::testing::KeysOf;
using hokuto::testing::ModelPlaysAsReference;
using hokuto::testing::PlacesOf;
using hokuto::testing::Play;
using hokuto::testing::Played;
using hokuto::testing::RomBytes;
using hokuto::testing::RunDirect;
using hokuto::testing::SequenceOf;
using hokuto::testing::StartsOf;
using hokuto::testing::TrackOf;
using ntdlib::commands::Kind;
using ntdlib::commands::kOne;
using song::KrawallRom;
using song::Mixing;
using song::Sequence;
using song::SequenceModel;
using xsflib::testing::ErrorOf;

constexpr uint32_t kRomBase = ntdlib::agb::kRomBase;

// A row's ticks.
constexpr uint32_t kRowTicks = cart::kSpeed;

// Piece 0, a module of 2 channels and 8 rows:
//
//   row 0, channel 0: C-4 (note 49) of instrument 1, the sample that loops forwards
//   row 2, channel 1: E-4 (note 53) of instrument 2, the sample that doesn't loop
//   row 4, channel 0: D-4 (note 51) of instrument 1
//   row 6, channel 0: a note off
//
// Piece 1, a module of 1 channel and 4 rows: C-4 of instrument 3, the sample that loops back and forth.
std::vector<uint8_t> Image()
{
    cart::Cart image;
    image.Module(2, 8,
                 {{.row = 0, .channel = 0, .note = 49, .instrument = 1},
                  {.row = 2, .channel = 1, .note = 53, .instrument = 2},
                  {.row = 4, .channel = 0, .note = 51, .instrument = 1},
                  {.row = 6, .channel = 0, .note = ntdlib::krawall::kNoteOff}});
    image.Module(1, 4, {{.row = 0, .channel = 0, .note = 49, .instrument = 3}});

    return RomBytes(image.ToRom());
}

// A module of 1 channel and 8 rows whose rows take two speeds: rows 0 to 5 at speed 3, and rows 6 and 7 at speed 2. Row
// 0 sets speed 3. Every pass therefore plays the same. The main speed of the conversion's timing is the commonest
// speed, 3. Speed 2 doesn't fall on that speed's grid. The timing therefore doubles the ticks of the arrangement: a
// tick of the player at speed 3 takes 2 of the arrangement's ticks, and one at speed 2 takes 3. Each row takes 6, and a
// pass takes 22 ticks of the player and 48 of the arrangement.
constexpr uint32_t kTwoSpeedRow = 6;
constexpr uint32_t kTwoSpeedPass = 8 * kTwoSpeedRow;
constexpr uint64_t kTwoSpeedPlayerPass = 6 * 3 + 2 * 2;

std::vector<uint8_t> TwoSpeedImage()
{
    cart::Cart image;
    image.Module(1, 8,
                 {{.row = 0, .channel = 0, .note = 49, .instrument = 1, .effect = cart::kSetSpeed, .op = 3},
                  {.row = 6, .channel = 0, .effect = cart::kSetSpeed, .op = 2}});

    return RomBytes(image.ToRom());
}

// Krawall's effects that the cartridge leaves out, by the numbers of ntdlib's player: a slide to a note, a retrigger
// and a note delay.
constexpr int kPortaNote = 19;
constexpr int kRetrig = 29;
constexpr int kNoteDelay = 45;

// A module of 1 channel and 8 rows with note delays:
//
//   row 0: C-4 of instrument 1
//   row 2: E-4 of instrument 1, a note delay of 1 tick: it starts at tick 7
//   row 3: a note delay of 1 tick without a note: it plays E-4 again at tick 10
//   row 5: D-4 of instrument 1
//   row 7: a note off
std::vector<uint8_t> DelayImage()
{
    cart::Cart image;
    image.Module(1, 8,
                 {{.row = 0, .channel = 0, .note = 49, .instrument = 1},
                  {.row = 2, .channel = 0, .note = 53, .instrument = 1, .effect = kNoteDelay, .op = 1},
                  {.row = 3, .channel = 0, .effect = kNoteDelay, .op = 1},
                  {.row = 5, .channel = 0, .note = 51, .instrument = 1},
                  {.row = 7, .channel = 0, .note = ntdlib::krawall::kNoteOff}});
    return RomBytes(image.ToRom());
}

// A module of 1 channel and 8 rows with a slide to a note between a note and a note delay:
//
//   row 0: C-4 of instrument 1
//   row 2: E-4 of instrument 1
//   row 3: F-4 with a slide to it: E-4 slides towards F-4, and no note starts
//   row 4: a note delay of 1 tick without a note: it plays the sliding note again at tick 13
//   row 6: D-4 of instrument 1
//   row 7: a note off
//
// Without E-4, row 2 contains `second`.
std::vector<cart::Cell> SlideRows(cart::Cell second)
{
    return {{.row = 0, .channel = 0, .note = 49, .instrument = 1},
            second,
            {.row = 3, .channel = 0, .note = 54, .effect = kPortaNote, .op = 0x10},
            {.row = 4, .channel = 0, .effect = kNoteDelay, .op = 1},
            {.row = 6, .channel = 0, .note = 51, .instrument = 1},
            {.row = 7, .channel = 0, .note = ntdlib::krawall::kNoteOff}};
}

// A module of 1 channel and 8 rows whose third note has no instrument:
//
//   row 0: C-4 of instrument 1, the sample that loops forwards, at volume 64
//   row 2: E-4 of instrument `second`, at its sample's volume, with `effect`, if given
//   row 4: D-4 of instrument `fourth`, or of none: the instrument of row 2 then
//   row 6: a note off
std::vector<cart::Cell> InstrumentRows(int second, int fourth, int effect = -1, int op = 0)
{
    return {{.row = 0, .channel = 0, .note = 49, .instrument = 1},
            {.row = 2, .channel = 0, .note = 53, .instrument = second, .effect = effect, .op = op},
            {.row = 4, .channel = 0, .note = 51, .instrument = fourth},
            {.row = 6, .channel = 0, .note = ntdlib::krawall::kNoteOff}};
}

std::vector<uint8_t> InstrumentImage(const std::vector<cart::Cell>& cells)
{
    cart::Cart image;
    image.Module(1, 8, cells);
    return RomBytes(image.ToRom());
}

// A module with instruments, of 1 channel and the patterns `patterns`. Instrument n plays sample n - 1 at every note,
// without envelopes: a note off stops the channel.
std::vector<uint8_t> XmImage(const std::vector<cart::Pattern>& patterns)
{
    cart::Cart image;
    const uint32_t header = image.Module(1, patterns);
    image.Put8(header + ntdlib::krawall::kModuleFlags, 1);
    for (uint32_t instrument = 0; instrument < 2; instrument++)
    {
        const uint32_t at = kRomBase + 0x2400 + ntdlib::krawall::kInstrumentSize * instrument;
        image.Put32(cart::kInstrumentTable + 4 * instrument, at);
        for (uint32_t note = 0; note < ntdlib::krawall::kInstrumentNotes; note++)
        {
            image.Put16(at + 2 * note, static_cast<uint16_t>(instrument));
        }
    }
    return RomBytes(image.ToRom());
}

// A module of 1 channel and 8 rows at two speeds whose ticks fall on one grid: rows 0 to 5 at speed 3, and rows 6 and 7
// at speed 6. The conversion's timing uses the commonest speed, 3. Speed 6 is twice that, on its grid. Each tick of
// the player therefore takes a tick of the arrangement. Rows 0 to 7 start at ticks 0, 3, 6, 9, 12, 15, 18 and 24, and
// the pass ends at 30. Row `volume_row` sets the volume column to 0x30, and row 6 to 0x20.
constexpr uint32_t kSpeedsRows[8] = {0, 3, 6, 9, 12, 15, 18, 24};

std::vector<uint8_t> SpeedsImage(int volume_row = 3)
{
    std::vector<cart::Cell> cells = {
        {.row = 0, .channel = 0, .note = 49, .instrument = 1, .effect = cart::kSetSpeed, .op = 3},
        {.row = 6, .channel = 0, .volume = 0x20, .effect = cart::kSetSpeed, .op = 6}};
    if (volume_row == 0)
    {
        cells[0].volume = 0x30;
    }
    else
    {
        cells.push_back({.row = volume_row, .channel = 0, .volume = 0x30});
    }

    cart::Cart image;
    image.Module(1, 8, cells);
    return RomBytes(image.ToRom());
}

// The arrangement's tick after `ticks` ticks of the player of TwoSpeedImage's module.
uint64_t TwoSpeedTick(uint64_t ticks)
{
    const uint64_t passes = ticks / kTwoSpeedPlayerPass;
    const uint64_t in_pass = ticks % kTwoSpeedPlayerPass;
    return passes * kTwoSpeedPass + (in_pass <= 18 ? 2 * in_pass : 36 + 3 * (in_pass - 18));
}

Sequence ReadOf(const File& file, uint32_t sound)
{
    return song::LoadKrawallRom(format::ExportArchive(file))->ReadSequence(sound);
}

// The reference: ntdlib's player run directly up to `samples` stereo samples.
template <typename Sample>
std::vector<Sample> Direct(const KrawallRom& rom, int song, std::size_t samples, bool enhanced)
{
    ntdlib::krawall::ArchivePlayer player(rom.rom, rom.info);
    player.Start(song, {.enhanced = enhanced});
    return RunDirect<Sample>(player, samples);
}

// The model plays `file`'s sound `sound` as the reference does, in strict mixing.
bool PlaysAsReference(const File& file, uint32_t sound)
{
    const std::shared_ptr<const KrawallRom> rom = song::LoadKrawallRom(format::ExportArchive(file));
    return ModelPlaysAsReference(rom, sound, [&](std::size_t samples)
                                 { return Direct<int16_t>(*rom, static_cast<int>(sound), samples, false); });
}

// A ROM with Krawall comes in as the format's engine of Krawall, and comes back byte for byte from both forms of the
// file. A sequence contains a module's patterns, and its waits count rows.
void TestImport()
{
    const std::vector<uint8_t> bytes = Image();
    const format::Driver* driver = format::DriverOfRom(bytes);
    SEQLIB_REQUIRE(driver != nullptr);
    XSFLIB_CHECK(driver->Facts().engine == format::Engine::kKrawall);

    const File file = driver->Import(bytes);
    XSFLIB_CHECK(file.engine == format::Engine::kKrawall);
    XSFLIB_CHECK(format::ExportArchive(file) == bytes);
    const File binary = format::Read(format::Write(file));
    XSFLIB_CHECK(binary.engine == format::Engine::kKrawall);
    XSFLIB_CHECK(format::ExportArchive(binary) == bytes);
    const File text = format::ReadText(format::WriteText(file));
    XSFLIB_CHECK(format::ExportArchive(text) == bytes);
    XSFLIB_CHECK(format::Write(text) == format::Write(file));

    // Piece 0's rows: the notes and the note off, and waits of 2 rows after each row with data.
    const ntdlib::commands::Sequence& commands = *file.sequences[SequenceOf(file, 0)];
    std::vector<std::pair<Kind, int>> rows;
    for (const ntdlib::commands::Command& command : commands.items)
    {
        if (command.kind == Kind::kNote || command.kind == Kind::kNoteOff)
        {
            rows.emplace_back(command.kind, command.key);
        }
        else if (command.kind == Kind::kWait)
        {
            rows.emplace_back(command.kind, static_cast<int>(command.value.value / kOne));
        }
    }
    XSFLIB_CHECK(rows == (std::vector<std::pair<Kind, int>>{{Kind::kNote, 49},
                                                            {Kind::kWait, 2},
                                                            {Kind::kNote, 53},
                                                            {Kind::kWait, 2},
                                                            {Kind::kNote, 51},
                                                            {Kind::kWait, 2},
                                                            {Kind::kNoteOff, 0},
                                                            {Kind::kWait, 2}}));
}

// The pieces that the arrangement shows. A track is a channel, and a note's program is the sample that it plays.
void TestRead()
{
    const std::shared_ptr<const KrawallRom> rom = song::LoadKrawallRom(Image());
    XSFLIB_CHECK(rom->Sounds() == (std::vector<uint32_t>{0, 1}));
    XSFLIB_CHECK(rom->Rules().DriverEngine() == format::Engine::kKrawall);
    XSFLIB_CHECK(rom->Rules().Revision() == 0);
    XSFLIB_CHECK(rom->Samples() == (std::vector<int>{0, 1, 2}));
    XSFLIB_CHECK(rom->SamplesOfSound(0) == (std::vector<int>{0, 1}));
    XSFLIB_CHECK(rom->SamplesOfSound(1) == (std::vector<int>{2}));
    XSFLIB_CHECK(rom->ShapeOf(0).length == cart::kForwardPoints);
    XSFLIB_CHECK(rom->ShapeOf(0).loop_start == cart::kForwardPoints - cart::kForwardLoop);
    XSFLIB_CHECK(!rom->ShapeOf(1).loop_start);
    XSFLIB_CHECK(static_cast<uint32_t>(rom->SampleRate(1)) == cart::kC4Rate);

    // A sample's WAV file loops as ShapeOf says. A loop back and forth becomes a forward loop over its points.
    const seqlib::WavContents forward = seqlib::ReadWav(rom->SampleWav(0));
    SEQLIB_REQUIRE(forward.pcm.channels.size() == 1 && forward.pcm.channels[0].size() == cart::kForwardPoints);
    XSFLIB_CHECK(forward.pcm.loop && forward.pcm.loop_start == cart::kForwardPoints - cart::kForwardLoop &&
                 forward.pcm.loop_end == cart::kForwardPoints);
    XSFLIB_CHECK(!seqlib::ReadWav(rom->SampleWav(1)).pcm.loop);
    const seqlib::WavContents both_ways = seqlib::ReadWav(rom->SampleWav(2));
    XSFLIB_CHECK(both_ways.pcm.loop && both_ways.pcm.loop_start == cart::kBidiPoints - cart::kBidiLoop &&
                 both_ways.pcm.loop_end == cart::kBidiPoints);

    // In the modified build, a sample's WAV file gets its points from the unpacked copy of the ROM. The copy here
    // contains silence (0x80) in each point of sample 1.
    const std::optional<ntdlib::krawall::Sample> sample1 = ntdlib::krawall::ReadSample(rom->rom, rom->info, 1);
    SEQLIB_REQUIRE(sample1.has_value());
    std::vector<uint8_t> silenced = Image();
    std::fill(silenced.begin() + (sample1->data - ntdlib::agb::kRomBase),
              silenced.begin() + (sample1->end - ntdlib::agb::kRomBase), uint8_t{0x80});
    auto unpacked = std::make_shared<ntdlib::agb::Rom>();
    unpacked->Assign(std::move(silenced));
    KrawallRom modified = *rom;
    modified.info.unpacked = unpacked;
    const auto silent = [](const seqlib::WavContents& wav)
    {
        return std::ranges::all_of(wav.pcm.channels[0], [](int16_t point) { return point == 0; });
    };
    XSFLIB_CHECK(!silent(seqlib::ReadWav(rom->SampleWav(1))));
    XSFLIB_CHECK(silent(seqlib::ReadWav(modified.SampleWav(1))));

    const Sequence piece0 = rom->ReadSequence(0);
    XSFLIB_CHECK(StartsOf(piece0, 0) == (std::vector<uint32_t>{0, 4 * kRowTicks}));
    XSFLIB_CHECK(KeysOf(piece0, 0) == (std::vector<int>{60, 62}));
    XSFLIB_CHECK(StartsOf(piece0, 1) == (std::vector<uint32_t>{2 * kRowTicks}));
    XSFLIB_CHECK(KeysOf(piece0, 1) == (std::vector<int>{64}));
    SEQLIB_REQUIRE(TrackOf(piece0, 0) != nullptr && TrackOf(piece0, 1) != nullptr);
    XSFLIB_CHECK(TrackOf(piece0, 0)->notes[0].program == 0 && TrackOf(piece0, 0)->notes[1].program == 0);
    XSFLIB_CHECK(TrackOf(piece0, 1)->notes[0].program == 1);
    // C-4 sounds until D-4 starts, and D-4 until the note off.
    XSFLIB_CHECK(TrackOf(piece0, 0)->notes[0].length == 4 * kRowTicks);
    XSFLIB_CHECK(TrackOf(piece0, 0)->notes[1].length == 2 * kRowTicks);

    const Sequence piece1 = rom->ReadSequence(1);
    XSFLIB_CHECK(KeysOf(piece1, 0) == (std::vector<int>{60}));
    XSFLIB_CHECK(TrackOf(piece1, 0)->notes[0].program == 2);
}

// The model plays as ntdlib's player, run directly, from the start in both mixing modes.
void TestPlays()
{
    const std::shared_ptr<const KrawallRom> rom = song::LoadKrawallRom(Image());
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

// Where a tick of the player lasts several ticks of the arrangement, the model counts the ticks of the arrangement. A
// player with the arrangement and one without it run frame by frame over three passes. The ticks of the first follow
// from the ticks of the second. A pass of the arrangement then lasts as long as a pass of the player.
void TestTicks()
{
    const std::shared_ptr<const KrawallRom> rom = song::LoadKrawallRom(TwoSpeedImage());
    const auto sequence = std::make_shared<const Sequence>(rom->ReadSequence(0));
    SEQLIB_REQUIRE(sequence->loop.has_value() && TrackOf(*sequence, 0) != nullptr);
    XSFLIB_CHECK(sequence->loop->start == 0 && sequence->loop->end == kTwoSpeedPass);
    std::vector<uint32_t> rows;
    for (const song::Stretch& stretch : TrackOf(*sequence, 0)->stretches)
    {
        rows.push_back(stretch.tick);
    }
    XSFLIB_CHECK(rows == (std::vector<uint32_t>{0, 6, 12, 18, 24, 30, 36, 42}));

    const std::unique_ptr<song::DriverPlayer> arranged = rom->MakePlayer();
    const std::unique_ptr<song::DriverPlayer> player = rom->MakePlayer();
    arranged->Start(0, false);
    arranged->SetArrangement(*sequence);
    player->Start(0, false);
    std::vector<int16_t> out;
    std::size_t wrong = 0;
    while (player->Tick() < 3 * kTwoSpeedPlayerPass && player->Frames() < 10000)
    {
        arranged->RunFrame(out);
        player->RunFrame(out);
        wrong += arranged->Tick() == TwoSpeedTick(player->Tick()) ? 0 : 1;
    }
    XSFLIB_CHECK(player->Tick() >= 3 * kTwoSpeedPlayerPass);
    XSFLIB_CHECK(wrong == 0);

    // The model reaches the loop's end after a pass of the player's ticks, within two frames.
    SequenceModel model(rom, sequence);
    std::size_t frames = 0;
    for (bool more = model.SeekFrame(0); more && model.After() < kTwoSpeedPass && frames < 1000;
         more = model.RunFrame())
    {
        frames++;
    }
    const double frame_seconds = static_cast<double>(model.FrameSamples()) / model.SampleRate();
    const double pass_seconds = static_cast<double>(kTwoSpeedPlayerPass * cart::kTickSamples) / rom->info.mix_rate;
    XSFLIB_CHECK(model.After() >= kTwoSpeedPass);
    XSFLIB_CHECK(std::abs(static_cast<double>(frames) * frame_seconds - pass_seconds) <= 2.0 * frame_seconds);
}

// The edits of notes change a row's entry in place.
void TestNoteEdits()
{
    const std::vector<uint8_t> bytes = Image();
    const File file = format::DriverOfRom(bytes)->Import(bytes);
    const uint32_t sequence = SequenceOf(file, 0);
    const Sequence piece0 = ReadOf(file, 0);
    const song::Track& first = *TrackOf(piece0, 0);
    const song::Stretch& stretch = first.stretches.at(0);
    const song::Note& c4 = first.notes.at(0);
    const song::Note& d4 = first.notes.at(1);

    // A removal makes D-4 a note off: C-4 still ends at row 4, and channel 1 plays as before.
    const edit::NoteEdit removed{.command = d4.command, .stretch = stretch, .remove = true};
    const File without = edit::EditNotes(file, sequence, std::span(&removed, 1)).file;
    const Sequence without_piece = ReadOf(without, 0);
    XSFLIB_CHECK(StartsOf(without_piece, 0) == (std::vector<uint32_t>{0}));
    XSFLIB_CHECK(TrackOf(without_piece, 0)->notes[0].length == 4 * kRowTicks);
    XSFLIB_CHECK(StartsOf(without_piece, 1) == StartsOf(piece0, 1));
    XSFLIB_CHECK(format::ExportArchive(without).size() == bytes.size());
    XSFLIB_CHECK(PlaysAsReference(without, 0));

    // A transpose of 2 moves C-4 to D-4. One that moves it past B-7, note 96, is refused.
    const edit::NoteEdit up{.command = c4.command, .stretch = stretch, .transpose = 2};
    const File up_file = edit::EditNotes(file, sequence, std::span(&up, 1)).file;
    XSFLIB_CHECK(KeysOf(ReadOf(up_file, 0), 0) == (std::vector<int>{62, 62}));
    XSFLIB_CHECK(PlaysAsReference(up_file, 0));
    const edit::NoteEdit past{.command = c4.command, .stretch = stretch, .transpose = 48};
    XSFLIB_CHECK(
        ErrorOf<std::invalid_argument>([&] { edit::EditNotes(file, sequence, std::span(&past, 1)); }).find("B-7") !=
        std::string::npos);

    // A note can't move, or change its length or its velocity, and the Key Editor can't draw one.
    const edit::NoteEdit moved{.command = c4.command, .stretch = stretch, .shift = static_cast<int32_t>(kRowTicks)};
    XSFLIB_CHECK(
        ErrorOf<std::invalid_argument>([&] { edit::EditNotes(file, sequence, std::span(&moved, 1)); }).find("move") !=
        std::string::npos);
    const edit::NoteEdit longer{.command = c4.command, .stretch = stretch, .length = 6 * kRowTicks};
    XSFLIB_CHECK(
        !ErrorOf<std::invalid_argument>([&] { edit::EditNotes(file, sequence, std::span(&longer, 1)); }).empty());
    const edit::NoteEdit quieter{.command = c4.command, .stretch = stretch, .velocity = 64};
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { edit::EditNotes(file, sequence, std::span(&quieter, 1)); })
                     .find("velocity") != std::string::npos);
    const edit::NewNote added{.stretch = stretch, .tick = 6 * kRowTicks, .key = 49, .velocity = 127, .length = 1};
    XSFLIB_CHECK(
        !ErrorOf<std::invalid_argument>([&] { edit::EditNotes(file, sequence, {}, std::span(&added, 1)); }).empty());

    // The rows set the volumes: the mixer refuses a volume.
    const edit::MixEdit volume{.setting = song::Mix::kVolume, .from = 64, .to = 32};
    XSFLIB_CHECK(
        !ErrorOf<std::invalid_argument>([&] { edit::EditMix(file, sequence, 0, first.stretches, volume); }).empty());
}

// A module keeps its channels. A removal of a track makes its notes note offs.
void TestTracks()
{
    const std::vector<uint8_t> bytes = Image();
    const File file = format::DriverOfRom(bytes)->Import(bytes);
    const std::vector<std::optional<edit::SoundPlace>> places = PlacesOf(file);
    const Sequence piece0 = ReadOf(file, 0);

    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { edit::AddTrack(file, places, 0, piece0, std::nullopt); })
                     .find("channels") != std::string::npos);
    XSFLIB_CHECK(!ErrorOf<std::invalid_argument>([&] { edit::DuplicateTrack(file, places, 0, 0); }).empty());
    XSFLIB_CHECK(!ErrorOf<std::invalid_argument>([&] { edit::CopyTrack(file, places, 0, piece0, 0); }).empty());

    const File without = edit::RemoveTrack(file, places, 0, 1);
    const Sequence without_piece = ReadOf(without, 0);
    XSFLIB_CHECK(StartsOf(without_piece, 1).empty());
    XSFLIB_CHECK(StartsOf(without_piece, 0) == StartsOf(piece0, 0));
    XSFLIB_CHECK(!ErrorOf<std::invalid_argument>([&] { edit::RemoveTrack(without, PlacesOf(without), 0, 1); }).empty());
}

// The stereo samples that ntdlib's player, run directly, plays of piece 0 of `bytes`.
std::vector<int16_t> Rendered(const std::vector<uint8_t>& bytes)
{
    return Direct<int16_t>(*song::LoadKrawallRom(bytes), 0, 16384, false);
}

// A removal drops the effects that would start the channel's note again: a note delay in the note's entry, and a note
// delay without a note on the channel's later rows. Otherwise they play the note before. A note that a row's effect
// starts can't take an edit of the Key Editor.
void TestDelayedRemovals()
{
    const std::vector<uint8_t> bytes = DelayImage();
    const File file = format::DriverOfRom(bytes)->Import(bytes);
    const uint32_t sequence = SequenceOf(file, 0);
    const Sequence piece = ReadOf(file, 0);
    const song::Track& track = *TrackOf(piece, 0);
    XSFLIB_CHECK(StartsOf(piece, 0) == (std::vector<uint32_t>{0, 7, 10, 15}));
    XSFLIB_CHECK(KeysOf(piece, 0) == (std::vector<int>{60, 64, 64, 62}));

    // Without E-4, C-4 ends at row 2's note off, and nothing plays until D-4.
    const edit::NoteEdit removed{
        .command = track.notes.at(1).command, .stretch = track.stretches.at(2), .remove = true};
    const File without = edit::EditNotes(file, sequence, std::span(&removed, 1)).file;
    const Sequence without_piece = ReadOf(without, 0);
    XSFLIB_CHECK(StartsOf(without_piece, 0) == (std::vector<uint32_t>{0, 15}));
    XSFLIB_CHECK(KeysOf(without_piece, 0) == (std::vector<int>{60, 62}));
    XSFLIB_CHECK(TrackOf(without_piece, 0)->notes.at(0).length == 2 * kRowTicks);
    XSFLIB_CHECK(PlaysAsReference(without, 0));

    // The note of row 3 comes from its effect.
    const edit::NoteEdit echo{.command = track.notes.at(2).command, .stretch = track.stretches.at(3), .transpose = 1};
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { edit::EditNotes(file, sequence, std::span(&echo, 1)); }) ==
                 "This note comes from a row effect. Edit the effect in the List Editor.");
}

// A note with a slide to it starts no note, and a removal drops the note delays after it too. Otherwise the delay would
// play the note before the removed one, sliding.
void TestSlideRemovals()
{
    const std::vector<uint8_t> bytes =
        InstrumentImage(SlideRows({.row = 2, .channel = 0, .note = 53, .instrument = 1}));
    const File file = format::DriverOfRom(bytes)->Import(bytes);
    const uint32_t sequence = SequenceOf(file, 0);
    const Sequence piece = ReadOf(file, 0);
    const song::Track& track = *TrackOf(piece, 0);
    XSFLIB_CHECK(StartsOf(piece, 0) == (std::vector<uint32_t>{0, 2 * kRowTicks, 4 * kRowTicks + 1, 6 * kRowTicks}));

    const edit::NoteEdit removed{
        .command = track.notes.at(1).command, .stretch = track.stretches.at(2), .remove = true};
    const File without = edit::EditNotes(file, sequence, std::span(&removed, 1)).file;
    XSFLIB_CHECK(StartsOf(ReadOf(without, 0), 0) == (std::vector<uint32_t>{0, 6 * kRowTicks}));
    std::vector<cart::Cell> expected = SlideRows({.row = 2, .channel = 0, .note = ntdlib::krawall::kNoteOff});
    expected.erase(expected.begin() + 3);
    const std::vector<int16_t> played = Rendered(format::ExportArchive(without));
    XSFLIB_CHECK(HasSound(std::span<const int16_t>(played)));
    XSFLIB_CHECK(played == Rendered(InstrumentImage(expected)));
    XSFLIB_CHECK(PlaysAsReference(without, 0));
}

// In a module without instruments, a removal's note off keeps the note's instrument. The note after it without one
// uses the instrument's sample and volume as before: as if it named the instrument.
void TestRemovedInstruments()
{
    const std::vector<uint8_t> bytes = InstrumentImage(InstrumentRows(2, 0));
    const File file = format::DriverOfRom(bytes)->Import(bytes);
    const uint32_t sequence = SequenceOf(file, 0);
    const Sequence piece = ReadOf(file, 0);
    const song::Track& track = *TrackOf(piece, 0);
    const edit::NoteEdit removed{
        .command = track.notes.at(1).command, .stretch = track.stretches.at(2), .remove = true};
    const File without = edit::EditNotes(file, sequence, std::span(&removed, 1)).file;
    const Sequence without_piece = ReadOf(without, 0);
    XSFLIB_CHECK(StartsOf(without_piece, 0) == (std::vector<uint32_t>{0, 4 * kRowTicks}));
    XSFLIB_CHECK(TrackOf(without_piece, 0)->notes.at(1).program == 1);

    std::vector<cart::Cell> expected = InstrumentRows(2, 2);
    expected[1] = {.row = 2, .channel = 0, .note = ntdlib::krawall::kNoteOff};
    const std::vector<int16_t> played = Rendered(format::ExportArchive(without));
    XSFLIB_CHECK(HasSound(std::span<const int16_t>(played)));
    XSFLIB_CHECK(played == Rendered(InstrumentImage(expected)));
    XSFLIB_CHECK(PlaysAsReference(without, 0));
}

// In a module with instruments, a note off's instrument reads no sample. A removal that would leave a later note of
// the channel without its instrument is refused, in the note's pattern and in the pattern that the orders play next. A
// removal whose next note names an instrument is allowed. A retrigger in the removed note's entry is removed with the
// note: it would start the note before again in its fade.
void TestRemovedXmInstruments()
{
    const std::vector<uint8_t> bytes = XmImage({{8, InstrumentRows(2, 0)}});
    const File file = format::DriverOfRom(bytes)->Import(bytes);
    const uint32_t sequence = SequenceOf(file, 0);
    const Sequence piece = ReadOf(file, 0);
    const song::Track& track = *TrackOf(piece, 0);
    XSFLIB_CHECK(KeysOf(piece, 0) == (std::vector<int>{60, 64, 62}));
    const auto removal = [&](std::size_t note, std::size_t row)
    {
        return edit::NoteEdit{
            .command = track.notes.at(note).command, .stretch = track.stretches.at(row), .remove = true};
    };
    const edit::NoteEdit second = removal(1, 2);
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { edit::EditNotes(file, sequence, std::span(&second, 1)); })
                     .find("instrument") != std::string::npos);
    const edit::NoteEdit first = removal(0, 0);
    const File without_first = edit::EditNotes(file, sequence, std::span(&first, 1)).file;
    XSFLIB_CHECK(StartsOf(ReadOf(without_first, 0), 0) == (std::vector<uint32_t>{2 * kRowTicks, 4 * kRowTicks}));
    const std::vector<edit::NoteEdit> both = {second, removal(2, 4)};
    XSFLIB_CHECK(StartsOf(ReadOf(edit::EditNotes(file, sequence, both).file, 0), 0) == (std::vector<uint32_t>{0}));

    // D-4 in the next pattern.
    const std::vector<uint8_t> split = XmImage({{4,
                                                 {{.row = 0, .channel = 0, .note = 49, .instrument = 1},
                                                  {.row = 2, .channel = 0, .note = 53, .instrument = 2}}},
                                                {4, {{.row = 1, .channel = 0, .note = 51}}}});
    const File split_file = format::DriverOfRom(split)->Import(split);
    const Sequence split_piece = ReadOf(split_file, 0);
    const song::Track& split_track = *TrackOf(split_piece, 0);
    const edit::NoteEdit split_second{
        .command = split_track.notes.at(1).command, .stretch = split_track.stretches.at(2), .remove = true};
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>(
                     [&] { edit::EditNotes(split_file, SequenceOf(split_file, 0), std::span(&split_second, 1)); })
                     .find("instrument") != std::string::npos);

    // A removal drops the retrigger of the note in a module with instruments, and keeps it in a module without them.
    const auto retriggers = [](const File& edited)
    {
        return std::ranges::count_if(edited.sequences[SequenceOf(edited, 0)]->items,
                                     [](const ntdlib::commands::Command& command)
                                     {
                                         return command.kind == Kind::kOther &&
                                                command.encoding.opcode == ntdlib::krawall::kEffectOpcode + kRetrig;
                                     });
    };
    for (const bool xm : {true, false})
    {
        const std::vector<uint8_t> retriggered =
            xm ? XmImage({{8, InstrumentRows(1, 1, kRetrig, 1)}}) : InstrumentImage(InstrumentRows(1, 1, kRetrig, 1));
        const File retriggered_file = format::DriverOfRom(retriggered)->Import(retriggered);
        const Sequence retriggered_piece = ReadOf(retriggered_file, 0);
        const auto note = std::ranges::find(TrackOf(retriggered_piece, 0)->notes, 64, &song::Note::key);
        SEQLIB_REQUIRE(note != TrackOf(retriggered_piece, 0)->notes.end());
        const edit::NoteEdit retriggered_removal{
            .command = note->command, .stretch = TrackOf(retriggered_piece, 0)->stretches.at(2), .remove = true};
        const File edited =
            edit::EditNotes(retriggered_file, SequenceOf(retriggered_file, 0), std::span(&retriggered_removal, 1)).file;
        XSFLIB_CHECK(retriggers(retriggered_file) == 1 && retriggers(edited) == (xm ? 0 : 1));
        XSFLIB_CHECK(PlaysAsReference(edited, 0));
    }
}

// A note delay starts its note later in its row, and every command of the row runs at the row's tick. The List Editor
// and the lanes then show a row's settings at the row's tick, from which a move counts.
void TestRowTicks()
{
    const std::vector<uint8_t> bytes = DelayImage();
    const File file = format::DriverOfRom(bytes)->Import(bytes);
    const uint32_t sequence = SequenceOf(file, 0);
    const Sequence piece = ReadOf(file, 0);
    const song::Track& track = *TrackOf(piece, 0);
    XSFLIB_CHECK(track.notes.at(1).start == 2 * kRowTicks + 1);

    // Row 2: E-4, its note delay and the wait.
    std::vector<std::pair<Kind, std::optional<uint32_t>>> row2;
    for (const edit::CommandRow& row : edit::CommandRows(file, sequence, track.stretches, track.notes, std::nullopt))
    {
        if (row.stretch == 2)
        {
            row2.emplace_back(edit::CommandAt(file, sequence, row.command).kind, row.tick);
        }
    }
    const std::optional<uint32_t> tick = 2 * kRowTicks;
    XSFLIB_CHECK(row2 == (std::vector<std::pair<Kind, std::optional<uint32_t>>>{
                             {Kind::kNote, tick}, {Kind::kOther, tick}, {Kind::kWait, tick}}));
}

// A setting moves to the row that plays at its new tick, with the arrangement's ticks of the rows. Its room covers the
// rows of its pattern, in the arrangement's ticks. A note has none. A row has one entry for each channel: a setting
// that moves to a row with an entry of its channel joins the entry, after its note. It then plays as a module whose
// row contains the setting in the entry. A move to a row whose entry has the same field is refused.
void TestSettingMoves()
{
    const std::vector<uint8_t> bytes = SpeedsImage();
    const File file = format::DriverOfRom(bytes)->Import(bytes);
    const uint32_t sequence = SequenceOf(file, 0);
    const Sequence piece = ReadOf(file, 0);
    const song::Track& track = *TrackOf(piece, 0);
    std::vector<uint32_t> rows;
    for (const song::Stretch& stretch : track.stretches)
    {
        rows.push_back(stretch.tick);
    }
    XSFLIB_CHECK(rows == std::vector<uint32_t>(std::begin(kSpeedsRows), std::end(kSpeedsRows)));

    // The volume 0x30 of row 3, and its tick after an edit.
    const auto moved_volume = [](const ntdlib::commands::Command& command)
    {
        return command.kind == Kind::kVolume && command.value.value == 0x30 * kOne;
    };
    const std::vector<uint32_t> offsets = edit::Offsets(*file.sequences[sequence], edit::ConsoleOf(file));
    const auto volume_item = std::ranges::find_if(file.sequences[sequence]->items, moved_volume);
    SEQLIB_REQUIRE(volume_item != file.sequences[sequence]->items.end());
    const uint32_t volume = offsets.at(static_cast<std::size_t>(volume_item - file.sequences[sequence]->items.begin()));
    const song::Stretch& row3 = track.stretches.at(3);
    const auto volume_tick = [&moved_volume](const File& edited) -> std::optional<uint32_t>
    {
        const uint32_t moved_sequence = SequenceOf(edited, 0);
        const Sequence moved = ReadOf(edited, 0);
        const song::Track& moved_track = *TrackOf(moved, 0);
        for (const edit::CommandRow& row :
             edit::CommandRows(edited, moved_sequence, moved_track.stretches, moved_track.notes, std::nullopt))
        {
            if (moved_volume(edit::CommandAt(edited, moved_sequence, row.command)))
            {
                return row.tick;
            }
        }
        return std::nullopt;
    };
    XSFLIB_CHECK(volume_tick(file) == 9u);

    const std::vector<edit::NotePlace> places = {
        {.command = volume, .stretch = row3}, {.command = track.notes.at(0).command, .stretch = track.stretches.at(0)}};
    const std::vector<edit::NoteLimits> limits = edit::LimitsOf(file, sequence, places, std::nullopt, track.stretches);
    XSFLIB_CHECK(limits.at(0).shift == (edit::ShiftRange{9, 15}));
    XSFLIB_CHECK(!limits.at(1).shift && !limits.at(1).length);

    // The note's limits give the Key Editor the edits' reasons, before an edit.
    XSFLIB_CHECK(limits.at(1).fixed == "A note of Krawall can't move. Its row sets its tick.");
    XSFLIB_CHECK(limits.at(1).fixed_length ==
                 "A note of Krawall sounds until the next note or note off of its "
                 "channel. Change those rows in the List Editor.");
    const edit::NoteEdit note_moved{.command = places[1].command, .stretch = places[1].stretch, .shift = 3};
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { edit::EditNotes(file, sequence, std::span(&note_moved, 1)); }) ==
                 limits.at(1).fixed);
    const edit::NoteEdit note_longer{.command = places[1].command, .stretch = places[1].stretch, .length = 48};
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { edit::EditNotes(file, sequence, std::span(&note_longer, 1)); }) ==
                 limits.at(1).fixed_length);

    // 15 ticks on is row 7, 17 ticks on falls in row 7, and 9 ticks back is row 0.
    for (const auto& [shift, tick] : std::vector<std::pair<int32_t, uint32_t>>{{15, 24}, {17, 24}, {-9, 0}})
    {
        const edit::SettingMove move{.command = volume, .stretch = row3, .shift = shift};
        const File moved = edit::MoveSettings(file, sequence, std::span(&move, 1), track.stretches).file;
        XSFLIB_CHECK(volume_tick(moved) == tick);
        XSFLIB_CHECK(PlaysAsReference(moved, 0));
    }

    // In row 0, the volume joins the entry of C-4, and C-4 plays at it.
    const edit::SettingMove first{.command = volume, .stretch = row3, .shift = -9};
    const File joined = edit::MoveSettings(file, sequence, std::span(&first, 1), track.stretches).file;
    const std::vector<int16_t> played = Rendered(format::ExportArchive(joined));
    XSFLIB_CHECK(HasSound(std::span<const int16_t>(played)));
    XSFLIB_CHECK(played == Rendered(SpeedsImage(0)));

    // Row 6's entry has a volume, and the move is refused. So is a move without the rows.
    const edit::SettingMove onto{.command = volume, .stretch = row3, .shift = 9};
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>(
                     [&] { edit::MoveSettings(file, sequence, std::span(&onto, 1), track.stretches); })
                     .find("already has a volume") != std::string::npos);
    const edit::SettingMove move{.command = volume, .stretch = row3, .shift = 15};
    XSFLIB_CHECK(
        !ErrorOf<std::invalid_argument>([&] { edit::MoveSettings(file, sequence, std::span(&move, 1)); }).empty());
}

// The exports equal those of ntdlib's converter, run directly.
void TestExports()
{
    const std::shared_ptr<const KrawallRom> rom = song::LoadKrawallRom(Image());
    for (const uint32_t sound : {0u, 1u})
    {
        ntdlib::sequence_export::agb::ConvertSettings settings;
        settings.program = "Hokuto";
        const ntdlib::sequence_export::krawall::SongFiles expected =
            ntdlib::sequence_export::krawall::ConvertSongFiles(rom->rom, rom->info, static_cast<int>(sound), settings);
        const ntdlib::sequence_export::Conversion conversion = rom->ConvertToMidi(sound, "Hokuto");
        XSFLIB_CHECK(expected.report.ok);
        XSFLIB_CHECK(conversion.midi == expected.midi);
        XSFLIB_CHECK(conversion.sf2 == expected.sf2);
        XSFLIB_CHECK(!conversion.midi.empty());
    }

    const ntdlib::sequence_export::BankConversion bank = rom->ConvertSamples(rom->Samples(), "Samples");
    XSFLIB_CHECK(!bank.sound_fonts.empty() && !bank.sound_fonts.front().sf2.empty());
}

// The model's clock follows the arrangement: after 10 s of play, or at the song's end, the arrangement's time at the
// clock is within 2 frames of the time played.
void TestClockFollowsArrangement()
{
    int long_runs = 0;
    const std::shared_ptr<const KrawallRom> rom = song::LoadKrawallRom(Image());
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
    return xsflib::testing::RunTest("krawall",
                                    []
                                    {
                                        TestImport();
                                        TestRead();
                                        TestPlays();
                                        TestClockFollowsArrangement();
                                        TestTicks();
                                        TestNoteEdits();
                                        TestDelayedRemovals();
                                        TestSlideRemovals();
                                        TestRemovedInstruments();
                                        TestRemovedXmInstruments();
                                        TestRowTicks();
                                        TestSettingMoves();
                                        TestTracks();
                                        TestExports();
                                    });
}
