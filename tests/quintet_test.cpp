// SPDX-License-Identifier: MIT

// Tests Hokuto's path for Quintet's GBA driver: the import and the round trip, the songs that the arrangement shows,
// playback, the edits of notes, the refused edits of tracks, the mixer's volume, the start tempos and the tempo row,
// the removal of a repeat, and the exports.
// ntdlib's test cartridge of the driver (ntdlib/testing/quintet_cart.h) holds no game data. It contains the code
// patterns that ntdlib's detection reads, the driver's tables and the songs below.
//
// The expected ticks, keys and bytes come from the songs' bytes, worked out by hand by the driver's rules
// (ntdlib/quintet/commands.h). A note's high nibble picks its note value from the length table: index 3 selects a
// quarter note of 96 of the driver's ticks, and index 4 a sixth of a whole note, 64 ticks. Its low nibble sets the
// pitch, and 13 rests. D1 after a note adds half its length. D4 between two notes of the same pitch ties them into one.
// 4F sets the octave, and a square channel's key is 36 more than its index in the pitch table: octave 2 and pitch 0
// give key 60. A file counts 37 ticks for each of the driver's ticks (format/quintet.h).
//
// ntdlib's ArchivePlayer, run directly, gives the reference output. The model's output has to match it sample for
// sample, from the start, in both mixing modes, at the player's rate. The MIDI file and the SoundFont have to equal
// those of ntdlib's converter, run directly.

#include "song/quintet.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "ntdlib/agb/apu.h"
#include "ntdlib/agb/gba.h"
#include "ntdlib/commands/sequence.h"
#include "ntdlib/quintet/archive_player.h"
#include "ntdlib/quintet/driver.h"
#include "ntdlib/sar/layout.h"
#include "ntdlib/sequence_export/agb/settings.h"
#include "ntdlib/sequence_export/convert.h"
#include "ntdlib/sequence_export/quintet/convert.h"
#include "ntdlib/testing/quintet_cart.h"
#include "xsflib/testing/check.h"

#include "app/list_edits.h"
#include "app/tempo_row.h"
#include "driver_test_support.h"
#include "edit/bytecode.h"
#include "edit/commands.h"
#include "edit/mix.h"
#include "edit/notes.h"
#include "edit/replace.h"
#include "edit/tracks.h"
#include "format/archive.h"
#include "format/drivers.h"
#include "format/file.h"
#include "format/quintet.h"
#include "format/text.h"
#include "song/archive.h"
#include "song/driver.h"
#include "song/sequence.h"
#include "song/sequence_model.h"

namespace
{

namespace format = hokuto::format;
namespace song = hokuto::song;
namespace edit = hokuto::edit;
namespace cart = ntdlib::testing::quintet_cart;
using format::File;
using hokuto::testing::HasSound;
using hokuto::testing::KeysOf;
using hokuto::testing::ModelPlaysAsReference;
using hokuto::testing::PlaceOfKind;
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
using song::Mixing;
using song::QuintetRom;
using song::Sequence;
using song::SequenceModel;
using xsflib::testing::ErrorOf;

constexpr uint32_t kRomBase = ntdlib::agb::kRomBase;

// The file's ticks for a count of the driver's.
constexpr uint32_t Ticks(uint32_t driver_ticks)
{
    return static_cast<uint32_t>(driver_ticks * format::kQuintetSongTicks);
}

// Song 0, on square 1 at tempo 90, volume 15 and octave 2:
//
//   30        C, a quarter note: ticks 0 to 96
//   34 D1     E, a dotted quarter note: ticks 96 to 240
//   37 D4 47  G, a quarter note tied to a sixth: ticks 240 to 400
//   3D        a quarter rest: ticks 400 to 496
//   39        A, a quarter note: ticks 496 to 592
//   FF        the end, at tick 592
//
// Song 1 loops: its loop point (BF) comes before a quarter note of C. The two songs are back to back in one list.
const std::vector<uint8_t> kSetup = {0x7F, 90, 0x5E, 15, 0x4F, 2};

std::vector<uint8_t> Song0()
{
    return cart::Song({cart::Channel()
                           .Bytes(kSetup)
                           .Note(0, 3)
                           .Note(4, 3)
                           .Bytes({0xD1})
                           .Note(7, 3)
                           .Bytes({0xD4})
                           .Note(7, 4)
                           .Rest(3)
                           .Note(9, 3)
                           .End()});
}

std::vector<uint8_t> Song1()
{
    return cart::Song({cart::Channel().Bytes(kSetup).Bytes({0xBF}).Note(0, 3).End()});
}

std::vector<uint8_t> Image()
{
    cart::Cart image(ntdlib::quintet::Revision::kA);
    image.Songs({Song0(), Song1()});

    return RomBytes(image.ToRom());
}

Sequence ReadOf(const File& file, uint32_t sound)
{
    return song::LoadQuintetRom(format::ExportArchive(file))->ReadSequence(sound);
}

// The reference: ntdlib's player run directly up to `samples` stereo samples.
template <typename Sample>
std::vector<Sample> Direct(const QuintetRom& rom, int song, std::size_t samples, bool enhanced)
{
    ntdlib::quintet::ArchivePlayer player(rom.rom, rom.files);
    player.Start(song, {.enhanced = enhanced});
    return RunDirect<Sample>(player, samples);
}

// The model plays `file`'s sound `sound` as the reference does, in strict mixing.
bool PlaysAsReference(const File& file, uint32_t sound)
{
    const std::shared_ptr<const QuintetRom> rom = song::LoadQuintetRom(format::ExportArchive(file));
    return ModelPlaysAsReference(rom, sound, [&](std::size_t samples)
                                 { return Direct<int16_t>(*rom, static_cast<int>(sound), samples, false); });
}

// A ROM with the driver comes in as the format's engine of Quintet's driver, and comes back byte for byte from both
// forms of the file. The file folds the dot into the note before it, and keeps the tie.
void TestImport()
{
    const std::vector<uint8_t> bytes = Image();
    const format::Driver* driver = format::DriverOfRom(bytes);
    SEQLIB_REQUIRE(driver != nullptr);
    XSFLIB_CHECK(driver->Facts().engine == format::Engine::kQuintet);

    const File file = driver->Import(bytes);
    XSFLIB_CHECK(file.engine == format::Engine::kQuintet);
    XSFLIB_CHECK(format::ExportArchive(file) == bytes);
    const File binary = format::Read(format::Write(file));
    XSFLIB_CHECK(binary.engine == format::Engine::kQuintet);
    XSFLIB_CHECK(format::ExportArchive(binary) == bytes);
    const File text = format::ReadText(format::WriteText(file));
    XSFLIB_CHECK(format::ExportArchive(text) == bytes);
    XSFLIB_CHECK(format::Write(text) == format::Write(file));

    // The notes and the rest of song 0's first channel, with their lengths in the file's ticks.
    const ntdlib::commands::Sequence& commands = *file.sequences[SequenceOf(file, 0)];
    std::vector<std::pair<Kind, uint32_t>> timed;
    bool dot = false;
    for (const ntdlib::commands::Command& command : commands.items)
    {
        if (command.kind == Kind::kNote || command.kind == Kind::kNoteOff || command.kind == Kind::kTie)
        {
            timed.emplace_back(command.kind, static_cast<uint32_t>(command.value.value / kOne));
        }
        dot = dot || (command.kind == Kind::kOther && command.encoding.opcode == format::kQuintetDot);
        if (command.kind == Kind::kEnd)
        {
            break;
        }
    }
    XSFLIB_CHECK(!dot);
    XSFLIB_CHECK(timed == (std::vector<std::pair<Kind, uint32_t>>{{Kind::kNote, Ticks(96)},
                                                                  {Kind::kNote, Ticks(144)},
                                                                  {Kind::kNote, Ticks(96)},
                                                                  {Kind::kTie, 0},
                                                                  {Kind::kNote, Ticks(64)},
                                                                  {Kind::kNoteOff, Ticks(96)},
                                                                  {Kind::kNote, Ticks(96)}}));

    // An export can't put the driver's songs into filler runs.
    XSFLIB_CHECK(!format::LacksRoom(file));
}

// The songs that the arrangement shows. A track is a channel. The tied notes show as one note.
void TestRead()
{
    const std::shared_ptr<const QuintetRom> rom = song::LoadQuintetRom(Image());
    XSFLIB_CHECK(rom->Sounds() == (std::vector<uint32_t>{0, 1}));
    XSFLIB_CHECK(rom->Rules().DriverEngine() == format::Engine::kQuintet);

    const Sequence song0 = rom->ReadSequence(0);
    XSFLIB_CHECK(!song0.loop);
    XSFLIB_CHECK(StartsOf(song0, 0) == (std::vector<uint32_t>{0, Ticks(96), Ticks(240), Ticks(496)}));
    XSFLIB_CHECK(KeysOf(song0, 0) == (std::vector<int>{60, 64, 67, 69}));
    XSFLIB_CHECK(song0.length >= Ticks(592));
    for (int track = 1; track < 6; track++)
    {
        XSFLIB_CHECK(StartsOf(song0, track).empty());
    }

    const Sequence song1 = rom->ReadSequence(1);
    SEQLIB_REQUIRE(song1.loop.has_value());
    // The loop keeps the driver's frames: at tempo 90, a frame lasts 90 ticks. The quarter note's 3,552 ticks run out
    // in the 40th frame, and the loop's second pass starts with that frame, at tick 3,510.
    XSFLIB_CHECK(song1.loop->start == 0 && song1.loop->end == 39 * 90);
    XSFLIB_CHECK(KeysOf(song1, 0).front() == 60);
}

// The model plays as ntdlib's player, run directly, from the start in both mixing modes.
void TestPlays()
{
    const std::shared_ptr<const QuintetRom> rom = song::LoadQuintetRom(Image());
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

    // The model's clock counts the arrangement's ticks, as ntdlib's converter times the frames: each frame that the
    // driver plays takes the first channel's tempo, 90 in the A revision, in the performer's ticks. After 120 frames,
    // song 0 is past tick 9,000 of its 21,904, and the clock has moved in steps of 90. The driver's clock alone counted
    // 400 of its ticks there. The playhead and the seeks follow this clock.
    const auto sequence = std::make_shared<const Sequence>(rom->ReadSequence(0));
    SequenceModel model(rom, sequence);
    Play(model, 120);
    XSFLIB_CHECK(sequence->timebase == 96 * format::kQuintetSongTicks && sequence->length == 21904);
    XSFLIB_CHECK(model.After() >= 100.0 * 90.0 && model.After() <= 120.0 * 90.0);
    XSFLIB_CHECK(std::fmod(model.After(), 90.0) == 0.0);

    // The frame in which the song ends counts too, as in ntdlib's converter: the driver's clock stops after it at 90
    // ticks for each frame that ran, the last one included.
    const std::unique_ptr<song::DriverPlayer> player = rom->MakePlayer();
    player->Start(0, false);
    std::vector<int16_t> out;
    uint64_t frames = 0;
    while (!player->Finished() && frames < 10000)
    {
        player->RunFrame(out);
        frames++;
    }
    XSFLIB_CHECK(frames > 0 && frames < 10000);
    XSFLIB_CHECK(player->Tick() == frames * 90);
    player->RunFrame(out);
    XSFLIB_CHECK(player->Tick() == frames * 90);
}

// The S revision resets SOUNDBIAS to a resolution of 262,144 Hz. ntdlib's model of the GBA's sound mixes at 65,536 Hz
// at most (agb::kMaxOutputRate), and so does ntdlib's player run directly. In strict mixing, the model and the player
// play at that rate. The song sets the volume (8A, the S revision's 5E) and the octave (89, its 4F), and plays a
// quarter note of C: its pitch, then its note value. The note value divides a whole note.
void TestRevisionS()
{
    cart::Cart image(ntdlib::quintet::Revision::kS);
    image.Songs({cart::Song({cart::Channel().Bytes({0x8A, 15, 0x89, 2, 0, 4}).End()})});
    const ntdlib::agb::Rom image_rom = image.ToRom();
    const std::shared_ptr<const QuintetRom> rom = song::LoadQuintetRom(RomBytes(image_rom));
    XSFLIB_CHECK(rom->Rate() == ntdlib::agb::kMaxOutputRate);
    const auto sequence = std::make_shared<const Sequence>(rom->ReadSequence(0));
    XSFLIB_CHECK(StartsOf(*sequence, 0) == (std::vector<uint32_t>{0}));

    SequenceModel strict(rom, sequence, hokuto::song::ModelOptions{.mixing = Mixing::kStrict, .snapshot_interval = 20});
    XSFLIB_CHECK(strict.SampleRate() == ntdlib::agb::kMaxOutputRate);
    const Played played = Play(strict, 120);
    XSFLIB_CHECK(HasSound(std::span<const int16_t>(played.samples)));
    const std::vector<int16_t> expected = Direct<int16_t>(*rom, 0, played.samples.size() + 4096, false);
    XSFLIB_CHECK(std::equal(played.samples.begin(), played.samples.end(), expected.begin()));
}

// The edits of notes change notes and rests in place.
void TestNoteEdits()
{
    const std::vector<uint8_t> bytes = Image();
    const File file = format::DriverOfRom(bytes)->Import(bytes);
    const uint32_t sequence = SequenceOf(file, 0);
    const Sequence song0 = ReadOf(file, 0);
    const song::Track& square = *TrackOf(song0, 0);
    const song::Note& e = square.notes.at(1);
    const song::Note& g = square.notes.at(2);

    // As in the Key Editor, each edit takes the stretch that ran its note, and a new note the stretch at its tick. A
    // stretch of the driver ends at each note, rest and wait.
    const song::Stretch& e_stretch = square.stretches.at(e.stretch);
    const song::Stretch& g_stretch = square.stretches.at(g.stretch);
    const auto stretch_at = [&square](uint32_t tick)
    {
        return edit::StretchAt(square.stretches, tick).value();
    };

    // A removal makes the dotted E a rest of the same length: the notes after it keep their ticks.
    const edit::NoteEdit removed{.command = e.command, .stretch = e_stretch, .remove = true};
    const File without_e = edit::EditNotes(file, sequence, std::span(&removed, 1)).file;
    const Sequence without_e_song = ReadOf(without_e, 0);
    XSFLIB_CHECK(StartsOf(without_e_song, 0) == (std::vector<uint32_t>{0, Ticks(240), Ticks(496)}));
    XSFLIB_CHECK(PlaysAsReference(without_e, 0));

    // A removal of the tied G makes both of its notes rests.
    const edit::NoteEdit removed_g{.command = g.command, .stretch = g_stretch, .remove = true};
    const File without_g = edit::EditNotes(file, sequence, std::span(&removed_g, 1)).file;
    XSFLIB_CHECK(StartsOf(ReadOf(without_g, 0), 0) == (std::vector<uint32_t>{0, Ticks(96), Ticks(496)}));
    for (const ntdlib::commands::Command& command : without_g.sequences[sequence]->items)
    {
        XSFLIB_CHECK(command.kind != Kind::kNote || command.key != 7);
    }

    // A transpose of 3 takes E to G, within the octave. One of 8 would leave it.
    const edit::NoteEdit up{.command = e.command, .stretch = e_stretch, .transpose = 3};
    const File up_file = edit::EditNotes(file, sequence, std::span(&up, 1)).file;
    XSFLIB_CHECK(KeysOf(ReadOf(up_file, 0), 0) == (std::vector<int>{60, 67, 67, 69}));
    XSFLIB_CHECK(PlaysAsReference(up_file, 0));
    const edit::NoteEdit out{.command = e.command, .stretch = e_stretch, .transpose = 8};
    XSFLIB_CHECK(
        ErrorOf<std::invalid_argument>([&] { edit::EditNotes(file, sequence, std::span(&out, 1)); }).find("octave") !=
        std::string::npos);

    // A transpose of the tied G changes both of its notes.
    const edit::NoteEdit g_up{.command = g.command, .stretch = g_stretch, .transpose = 2};
    const File g_up_file = edit::EditNotes(file, sequence, std::span(&g_up, 1)).file;
    const Sequence g_up_song = ReadOf(g_up_file, 0);
    XSFLIB_CHECK(KeysOf(g_up_song, 0) == (std::vector<int>{60, 64, 69, 69}));
    XSFLIB_CHECK(StartsOf(g_up_song, 0) == StartsOf(song0, 0));

    // A new note of D, key 62, takes the place of the rest at tick 400. The Key Editor gives the edit the command's
    // key: the pitch 2, without the octave's 60 that the note before it shows.
    const edit::NewNote added{
        .stretch = stretch_at(Ticks(400)), .tick = Ticks(400), .key = 2, .velocity = 127, .length = Ticks(96)};
    const File added_file = edit::EditNotes(file, sequence, {}, std::span(&added, 1)).file;
    const Sequence with_new = ReadOf(added_file, 0);
    XSFLIB_CHECK(StartsOf(with_new, 0) == (std::vector<uint32_t>{0, Ticks(96), Ticks(240), Ticks(400), Ticks(496)}));
    XSFLIB_CHECK(KeysOf(with_new, 0) == (std::vector<int>{60, 64, 67, 62, 69}));
    XSFLIB_CHECK(PlaysAsReference(added_file, 0));

    // No rest starts at tick 300, and key 50 is below the octave.
    const edit::NewNote no_rest{
        .stretch = stretch_at(Ticks(300)), .tick = Ticks(300), .key = 2, .velocity = 127, .length = 1};
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { edit::EditNotes(file, sequence, {}, std::span(&no_rest, 1)); })
                     .find("rest") != std::string::npos);
    const edit::NewNote low{
        .stretch = stretch_at(Ticks(400)), .tick = Ticks(400), .key = 50 - 60, .velocity = 127, .length = 1};
    XSFLIB_CHECK(
        !ErrorOf<std::invalid_argument>([&] { edit::EditNotes(file, sequence, {}, std::span(&low, 1)); }).empty());

    // A note can't move, or change its length or its velocity.
    const edit::NoteEdit moved{.command = e.command, .stretch = e_stretch, .shift = static_cast<int32_t>(Ticks(1))};
    XSFLIB_CHECK(
        ErrorOf<std::invalid_argument>([&] { edit::EditNotes(file, sequence, std::span(&moved, 1)); }).find("move") !=
        std::string::npos);
    const edit::NoteEdit longer{.command = e.command, .stretch = e_stretch, .length = Ticks(192)};
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { edit::EditNotes(file, sequence, std::span(&longer, 1)); })
                     .find("length") != std::string::npos);
    const edit::NoteEdit quieter{.command = e.command, .stretch = e_stretch, .velocity = 64};
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { edit::EditNotes(file, sequence, std::span(&quieter, 1)); })
                     .find("velocity") != std::string::npos);

    // The note's limits give the Key Editor the reasons of the two refusals above, before an edit.
    const edit::NotePlace place{.command = e.command, .stretch = e_stretch};
    const std::vector<edit::NoteLimits> limits = edit::LimitsOf(file, sequence, std::span(&place, 1));
    SEQLIB_REQUIRE(limits.size() == 1);
    XSFLIB_CHECK(!limits[0].shift && !limits[0].length);
    XSFLIB_CHECK(limits[0].fixed ==
                 ErrorOf<std::invalid_argument>([&] { edit::EditNotes(file, sequence, std::span(&moved, 1)); }));
    XSFLIB_CHECK(limits[0].fixed_length ==
                 ErrorOf<std::invalid_argument>([&] { edit::EditNotes(file, sequence, std::span(&longer, 1)); }));
    XSFLIB_CHECK(limits[0].fixed.starts_with("A note of Quintet's driver can't move."));
}

// In the J revision, a channel starts at volume 0, and a note plays at the channel's volume (ntdlib's
// quintet::Sequencer). At 0, it plays nothing, and ntdlib's model gives no note for it: a D before the volume command
// (song 7). The A revision's model plays that D. A new note that would play at volume 0 is refused, and one that plays
// goes in. Each song sets tempo 145, then plays C after
// a quarter note or two. In the J revision:
// - song 0: a rest, then 5E 06. Refused.
// - song 1: 5E 06, then a rest. Goes in.
// - song 2: 5F, the volume up, then a rest. Goes in, at volume 1.
// - song 3: 5E 00, then a rest. Refused.
// - song 4: two rests, then 5E 06. A note on the second rest, in a later stretch, is refused.
// - song 5: 5E 06, two rests. A note on the second rest goes in.
// - song 6: a rest, then 5E 06, in the A revision. Goes in.
// A key out of the octave is refused for that reason where a note in the octave would sound. Where no note would sound,
// as in songs 0, 3 and 4, the volume is the reason given: a change of octave wouldn't help.
void TestNoteAtVolume()
{
    using ntdlib::quintet::Revision;
    const auto channel = [](std::vector<uint8_t> before, int rests, std::vector<uint8_t> after)
    {
        cart::Channel built;
        built.Bytes({0x7F, 145}).Bytes(std::move(before));
        for (int i = 0; i < rests; i++)
        {
            built.Rest(3);
        }
        return cart::Song({built.Bytes(std::move(after)).Bytes({0x4F, 2}).Note(0, 3).End()});
    };
    const std::vector<std::vector<uint8_t>> songs = {
        channel({}, 1, {0x5E, 6}),
        channel({0x5E, 6}, 1, {}),
        channel({0x5F}, 1, {0x5E, 6}),
        channel({0x5E, 0}, 1, {0x5E, 6}),
        channel({}, 2, {0x5E, 6}),
        channel({0x5E, 6}, 2, {}),
        channel({}, 1, {0x5E, 6}),
        cart::Song({cart::Channel().Bytes({0x7F, 145}).Note(2, 3).Bytes({0x5E, 6, 0x4F, 2}).Note(0, 3).End()})};
    const std::string silent =
        "A note there wouldn't sound. The channel has no volume yet. Draw it after the channel's volume command.";
    for (const Revision revision : {Revision::kJ, Revision::kA})
    {
        cart::Cart image(revision);
        image.Songs(songs);
        const std::vector<uint8_t> bytes = RomBytes(image.ToRom());
        const File file = format::DriverOfRom(bytes)->Import(bytes);
        XSFLIB_CHECK(StartsOf(ReadOf(file, 7), 0) == (revision == Revision::kJ ? std::vector<uint32_t>{Ticks(96)}
                                                                               : std::vector<uint32_t>{0, Ticks(96)}));
        const std::vector<uint32_t> sounds =
            revision == Revision::kJ ? std::vector<uint32_t>{0, 1, 2, 3, 4, 5} : std::vector<uint32_t>{6};
        for (const uint32_t sound : sounds)
        {
            const Sequence song = ReadOf(file, sound);
            const song::Track& square = *TrackOf(song, 0);
            const uint32_t rests = sound == 4 || sound == 5 ? 2 : 1;
            XSFLIB_CHECK(StartsOf(song, 0) == (std::vector<uint32_t>{Ticks(96) * rests}));

            // The pitch D, 2, on the last rest.
            const uint32_t tick = Ticks(96) * (rests - 1);
            const edit::NewNote added{.stretch = edit::StretchAt(square.stretches, tick).value(),
                                      .tick = tick,
                                      .key = 2,
                                      .velocity = 127,
                                      .length = Ticks(96)};
            const uint32_t sequence = SequenceOf(file, sound);
            const auto edit = [&]
            {
                return edit::EditNotes(file, sequence, {}, std::span(&added, 1)).file;
            };
            edit::NewNote low = added;
            low.key = -1;
            edit::NewNote high = added;
            high.key = 12;
            const auto error_of = [&](const edit::NewNote& note)
            {
                return ErrorOf<std::invalid_argument>([&]
                                                      { edit::EditNotes(file, sequence, {}, std::span(&note, 1)); });
            };
            if (sound == 0 || sound == 3 || sound == 4)
            {
                XSFLIB_CHECK(ErrorOf<std::invalid_argument>(edit) == silent);
                XSFLIB_CHECK(error_of(low) == silent);
                XSFLIB_CHECK(error_of(high) == silent);
                continue;
            }

            // A key below or above the channel's octave there is refused, with or without a note before it.
            const std::string octave =
                "A new note of Quintet's driver has to be in the octave that the channel plays "
                "there. Change the channel's octave in the List Editor first.";
            XSFLIB_CHECK(error_of(low) == octave);
            XSFLIB_CHECK(error_of(high) == octave);

            const Sequence with_new = ReadOf(edit(), sound);
            XSFLIB_CHECK(StartsOf(with_new, 0) == (std::vector<uint32_t>{tick, Ticks(96) * rests}));
        }
    }
}

// A tie (D4) joins a note to the note of the same pitch after it: ntdlib's model plays one note for C, a tie and C
// (song 1). In song 0, C and a tie come before a rest. A C on that rest would join the C before it and make no note of
// its own: the edit is refused with the reason. A D there goes in as a note of its own. A dot (D1) may come between a
// note and its tie: song 2 does the same after a dotted C. In song 3, C, a dot, a tie, C, a dot, a tie and C play one
// note of 1.5 + 1.5 + 1 quarter notes. A key edit of it moves the whole note.
void TestNoteAfterTie()
{
    cart::Cart image(ntdlib::quintet::Revision::kA);
    image.Songs({cart::Song({cart::Channel().Bytes(kSetup).Note(0, 3).Bytes({0xD4}).Rest(3).Note(4, 3).End()}),
                 cart::Song({cart::Channel().Bytes(kSetup).Note(0, 3).Bytes({0xD4}).Note(0, 3).Note(4, 3).End()}),
                 cart::Song({cart::Channel().Bytes(kSetup).Note(0, 3).Bytes({0xD1, 0xD4}).Rest(3).Note(4, 3).End()}),
                 cart::Song({cart::Channel()
                                 .Bytes(kSetup)
                                 .Note(0, 3)
                                 .Bytes({0xD1, 0xD4})
                                 .Note(0, 3)
                                 .Bytes({0xD1, 0xD4})
                                 .Note(0, 3)
                                 .Note(4, 3)
                                 .End()})});
    const std::vector<uint8_t> bytes = RomBytes(image.ToRom());
    const File file = format::DriverOfRom(bytes)->Import(bytes);
    XSFLIB_CHECK(StartsOf(ReadOf(file, 1), 0) == (std::vector<uint32_t>{0, Ticks(192)}));

    const Sequence song = ReadOf(file, 0);
    XSFLIB_CHECK(StartsOf(song, 0) == (std::vector<uint32_t>{0, Ticks(192)}));
    const song::Track& square = *TrackOf(song, 0);
    const uint32_t sequence = SequenceOf(file, 0);
    const auto added = [&](int key)
    {
        return edit::NewNote{.stretch = edit::StretchAt(square.stretches, Ticks(96)).value(),
                             .tick = Ticks(96),
                             .key = key,
                             .velocity = 127,
                             .length = Ticks(96)};
    };
    const edit::NewNote c = added(0);
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { edit::EditNotes(file, sequence, {}, std::span(&c, 1)); }) ==
                 "Can't add a note of this key here. The tie before the rest would join it to the note before it.");
    const edit::NewNote d = added(2);
    const Sequence with_d = ReadOf(edit::EditNotes(file, sequence, {}, std::span(&d, 1)).file, 0);
    XSFLIB_CHECK(StartsOf(with_d, 0) == (std::vector<uint32_t>{0, Ticks(96), Ticks(192)}));

    const Sequence dotted = ReadOf(file, 2);
    XSFLIB_CHECK(StartsOf(dotted, 0) == (std::vector<uint32_t>{0, Ticks(240)}));
    const song::Track& dotted_square = *TrackOf(dotted, 0);
    const edit::NewNote dotted_c{.stretch = edit::StretchAt(dotted_square.stretches, Ticks(144)).value(),
                                 .tick = Ticks(144),
                                 .key = 0,
                                 .velocity = 127,
                                 .length = Ticks(96)};
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>(
                     [&] { edit::EditNotes(file, SequenceOf(file, 2), {}, std::span(&dotted_c, 1)); }) ==
                 "Can't add a note of this key here. The tie before the rest would join it to the note before it.");

    const Sequence long_c = ReadOf(file, 3);
    XSFLIB_CHECK(StartsOf(long_c, 0) == (std::vector<uint32_t>{0, Ticks(384)}));
    const song::Track& long_square = *TrackOf(long_c, 0);
    const song::Note& first = long_square.notes.front();
    XSFLIB_CHECK(first.length == Ticks(384));
    const edit::NoteEdit up{
        .command = first.command, .stretch = long_square.stretches.at(first.stretch), .transpose = 2};
    const Sequence moved = ReadOf(edit::EditNotes(file, SequenceOf(file, 3), std::span(&up, 1)).file, 3);
    XSFLIB_CHECK(StartsOf(moved, 0) == (std::vector<uint32_t>{0, Ticks(384)}));
    XSFLIB_CHECK(KeysOf(moved, 0) == (std::vector<int>{first.key + 2, long_square.notes.back().key}));
    XSFLIB_CHECK(TrackOf(moved, 0)->notes.front().length == Ticks(384));
}

// The List Editor names a note by the key that it plays, as the Key Editor does: C3, E3, G3 and A3, and not the pitches
// C-2, E-2, G-2 and A-2 that the commands contain in octave 2. The G after the tie plays no note of its own, and takes
// the octave of the G before it. A typed key moves the pitch by its distance from the named key: D3 makes C3's pitch 2.
// A key below the octave is refused before an edit, and one above it by the note edits.
void TestListKeys()
{
    using hokuto::app::ListColumn;
    const std::vector<uint8_t> bytes = Image();
    const File file = format::DriverOfRom(bytes)->Import(bytes);
    const uint32_t sequence = SequenceOf(file, 0);
    const hokuto::app::ListTrack track = hokuto::app::MakeListTrack(file, sequence, ReadOf(file, 0), 0);
    std::vector<std::string> names;
    std::vector<std::size_t> notes;
    for (std::size_t i = 0; i < track.rows.size(); i++)
    {
        if (track.rows[i].command.kind == Kind::kNote && track.rows[i].label.empty())
        {
            names.push_back(hokuto::app::CellText(track, track.rows[i], ListColumn::kData1));
            notes.push_back(i);
        }
    }
    XSFLIB_CHECK(names == (std::vector<std::string>{"C3", "E3", "G3", "G3", "A3"}));
    SEQLIB_REQUIRE(!notes.empty());

    const std::optional<hokuto::app::ListEdit> up =
        hokuto::app::TypedListEdit(track, notes[0], ListColumn::kData1, "D3");
    SEQLIB_REQUIRE(up.has_value());
    XSFLIB_CHECK(up->refused.empty() && up->command.key == 2);
    const std::optional<hokuto::app::ListEdit> below =
        hokuto::app::TypedListEdit(track, notes[0], ListColumn::kData1, "B2");
    SEQLIB_REQUIRE(below.has_value());
    XSFLIB_CHECK(below->refused.find("keeps its octave") != std::string::npos);
}

// The driver's bytes have no command that turns the note wait off: each note waits for its length. A setting therefore
// can't move off its tick, and no new setting goes in at a tick inside a note. The tempo (7F) at tick 0 has no room in
// LimitsOf, and the moves and the new volume at tick 48, inside the first quarter note, are refused.
void TestFixedSettings()
{
    const std::vector<uint8_t> bytes = Image();
    const File file = format::DriverOfRom(bytes)->Import(bytes);
    const uint32_t sequence = SequenceOf(file, 0);
    const Sequence song0 = ReadOf(file, 0);
    const song::Stretch& first = TrackOf(song0, 0)->stretches.front();
    const ntdlib::commands::Command& tempo = file.sequences[sequence]->items.at(
        *edit::ItemAtOffset(edit::Offsets(*file.sequences[sequence], edit::ConsoleOf(file)), first.first));
    XSFLIB_CHECK(tempo.kind == Kind::kTempo);

    const edit::NotePlace place{.command = first.first, .stretch = first};
    const std::vector<edit::NoteLimits> limits = edit::LimitsOf(file, sequence, std::span(&place, 1));
    XSFLIB_CHECK(limits.size() == 1 && !limits.front().shift);

    for (const int32_t shift : {static_cast<int32_t>(Ticks(96)), static_cast<int32_t>(Ticks(48))})
    {
        const edit::SettingMove move{first.first, first, shift};
        XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { edit::MoveSettings(file, sequence, std::span(&move, 1)); }) ==
                     "The notes of this track set its timing here. Can't move this command.");
    }

    ntdlib::commands::Command volume;
    volume.kind = Kind::kVolume;
    volume.value = ntdlib::commands::Argument{ntdlib::commands::Source::kValue, 8 * kOne, 0};
    const edit::NewSetting added{first, Ticks(48), volume};
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { edit::AddSettings(file, sequence, std::span(&added, 1)); }) ==
                 "The notes of this track set its timing here. Can't add a command at that tick.");
}

// A note's audition plays the instrument that its channel selects before it. The driver's 2F sets a square channel's
// duty. In this song, 2F sets 0x80 before a quarter rest, and a quarter note of C follows the rest. A stretch of the
// driver ends at the rest's wait. The note therefore runs in the channel's second stretch.
void TestAuditionInstrument()
{
    cart::Cart image(ntdlib::quintet::Revision::kA);
    image.Songs({cart::Song({cart::Channel().Bytes(kSetup).Bytes({0x2F, 0x80}).Rest(3).Note(0, 3).End()})});
    const std::vector<uint8_t> bytes = RomBytes(image.ToRom());
    const File file = format::DriverOfRom(bytes)->Import(bytes);
    const std::shared_ptr<const QuintetRom> rom = song::LoadQuintetRom(bytes);
    const Sequence song0 = rom->ReadSequence(0);
    const song::Track& square = *TrackOf(song0, 0);
    SEQLIB_REQUIRE(square.notes.size() == 1);
    const song::Note& note = square.notes.front();
    XSFLIB_CHECK(note.start == Ticks(96) && note.key == 60);
    XSFLIB_CHECK(note.stretch > 0);

    const std::optional<song::DriverAudition> audition = rom->NoteAudition(
        *file.sequences[SequenceOf(file, 0)], song0, 0, static_cast<int>(note.program), note.key, 127);
    SEQLIB_REQUIRE(audition.has_value() && audition->setup.size() >= 2);
    XSFLIB_CHECK(audition->setup[1].value.value == 0x80 * kOne);
}

// A song keeps its 6 channels. A removal makes the channel's notes rests.
void TestTracks()
{
    const std::vector<uint8_t> bytes = Image();
    const File file = format::DriverOfRom(bytes)->Import(bytes);
    const std::vector<std::optional<edit::SoundPlace>> places = PlacesOf(file);
    const Sequence song0 = ReadOf(file, 0);

    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { edit::AddTrack(file, places, 0, song0, std::nullopt); })
                     .find("6 channels") != std::string::npos);
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { edit::DuplicateTrack(file, places, 0, 0); }).find("6 channels") !=
                 std::string::npos);
    XSFLIB_CHECK(
        ErrorOf<std::invalid_argument>([&] { edit::CopyTrack(file, places, 0, song0, 0); }).find("6 channels") !=
        std::string::npos);

    const File without = edit::RemoveTrack(file, places, 0, 0);
    XSFLIB_CHECK(StartsOf(ReadOf(without, 0), 0).empty());
    XSFLIB_CHECK(format::ExportArchive(without).size() == bytes.size());
    XSFLIB_CHECK(!ErrorOf<std::invalid_argument>([&] { edit::RemoveTrack(file, places, 0, 1); }).empty());
}

// A song on square 1 that plays two quarter notes of C, with `step` between them.
std::vector<uint8_t> StepImage(const std::vector<uint8_t>& step)
{
    cart::Cart image(ntdlib::quintet::Revision::kA);
    image.Songs({cart::Song({cart::Channel().Bytes(kSetup).Note(0, 3).Bytes(step).Note(0, 3).End()})});
    return RomBytes(image.ToRom());
}

// The mixer's volume edit of square 1 in song 0 from 15 to 8 changes one byte of the ROM: the 15 after 5E. A volume
// up (5F), a volume down (6E) or a volume step (F1) adds to the volume, and a track that runs one refuses a new volume.
void TestMix()
{
    const std::vector<uint8_t> bytes = Image();
    const File file = format::DriverOfRom(bytes)->Import(bytes);
    const Sequence song0 = ReadOf(file, 0);
    const edit::MixEdit quieter{.setting = song::Mix::kVolume, .from = 15, .to = 8};
    const File quieter_file = edit::EditMix(file, SequenceOf(file, 0), 0, TrackOf(song0, 0)->stretches, quieter).file;
    const std::vector<uint8_t> exported = format::ExportArchive(quieter_file);
    SEQLIB_REQUIRE(exported.size() == bytes.size());
    std::vector<std::size_t> changed;
    for (std::size_t i = 0; i < bytes.size(); i++)
    {
        if (exported[i] != bytes[i])
        {
            changed.push_back(i);
        }
    }
    SEQLIB_REQUIRE(changed.size() == 1 && changed[0] > 0);
    XSFLIB_CHECK(bytes[changed[0] - 1] == 0x5E && bytes[changed[0]] == 15 && exported[changed[0]] == 8);

    const std::vector<std::vector<uint8_t>> steps = {{0x5F}, {0x6E}, {0xF1, 0xFF}};
    for (const std::vector<uint8_t>& step : steps)
    {
        const std::vector<uint8_t> image = StepImage(step);
        const File stepped = format::DriverOfRom(image)->Import(image);
        const Sequence read = ReadOf(stepped, 0);
        const edit::MixEdit edit{.setting = song::Mix::kVolume, .from = 15, .to = 8};
        XSFLIB_CHECK(ErrorOf<std::invalid_argument>(
                         [&] { edit::EditMix(stepped, SequenceOf(stepped, 0), 0, TrackOf(read, 0)->stretches, edit); })
                         .find("in steps") != std::string::npos);
    }
}

// Two songs back to back in one list. Song 0's data takes 13 bytes, and a zero after them brings the song to an even
// size. Song 1 plays a D.
std::vector<uint8_t> OddSong()
{
    return cart::Song({cart::Channel().Bytes(kSetup).Note(0, 3).End()});
}

std::vector<uint8_t> NextSong()
{
    return cart::Song({cart::Channel().Bytes(kSetup).Note(2, 3).End()});
}

std::vector<uint8_t> ListImage()
{
    cart::Cart image(ntdlib::quintet::Revision::kA);
    image.Songs({OddSong(), NextSong()});
    return RomBytes(image.ToRom());
}

// The songs of a list lie back to back, and a song's place keeps no size. No song moves out of its list: the Export ROM
// window then names no song that no longer fits its space, unedited or after an edit of a note.
void TestMovedSongs()
{
    const std::vector<uint8_t> bytes = ListImage();
    const File file = format::DriverOfRom(bytes)->Import(bytes);
    XSFLIB_CHECK(format::MovedSongs(file).empty());

    const Sequence next = ReadOf(file, 1);
    const song::Track& square = *TrackOf(next, 0);
    const song::Note& d = square.notes.at(0);
    const edit::NoteEdit up{.command = d.command, .stretch = square.stretches.at(d.stretch), .transpose = 2};
    const File up_file = edit::EditNotes(file, SequenceOf(file, 1), std::span(&up, 1)).file;
    XSFLIB_CHECK(format::MovedSongs(up_file).empty());
    XSFLIB_CHECK(!format::LacksRoom(up_file));
    XSFLIB_CHECK(format::ExportArchive(file) == bytes);
}

// In the ROM that a file builds, ntdlib's join lays out the songs of a list back to back. A song's piece starts after
// the song before it. The join brings the commands of that song to an even size. The field of a song's first channel
// then names the song's sequence, and the Key Editor's edit of a note of the second song changes that song.
void TestPieceStarts()
{
    const std::vector<uint8_t> bytes = ListImage();
    const File file = format::DriverOfRom(bytes)->Import(bytes);
    const std::vector<ntdlib::sar::Piece>& pieces = *file.native->pieces;
    std::vector<std::size_t> songs;
    for (std::size_t piece = 0; piece < pieces.size(); piece++)
    {
        if (std::holds_alternative<ntdlib::sar::SequencePiece>(pieces[piece]))
        {
            songs.push_back(piece);
        }
    }
    SEQLIB_REQUIRE(songs.size() == 2);
    SEQLIB_REQUIRE(OddSong().size() == 14 + 14);

    // Each song's commands follow its header of 14 bytes.
    const uint64_t music = cart::FileAddress(cart::kMusicFile) - kRomBase;
    const uint64_t next_header = music + OddSong().size();
    const std::vector<uint64_t> starts = format::NativePieceStarts(file);
    XSFLIB_CHECK(starts.at(songs[0]) == music + 14);
    XSFLIB_CHECK(starts.at(songs[1]) == next_header + 14);
    XSFLIB_CHECK(starts.back() == bytes.size());

    const uint32_t first = std::get<ntdlib::sar::SequencePiece>(pieces[songs[0]]).sequence;
    const uint32_t second = std::get<ntdlib::sar::SequencePiece>(pieces[songs[1]]).sequence;
    XSFLIB_CHECK(first != second);
    XSFLIB_CHECK(SequenceOf(file, 0) == first);
    XSFLIB_CHECK(SequenceOf(file, 1) == second);

    // Song 1's D goes up to E. Song 0 keeps its C.
    const Sequence next = ReadOf(file, 1);
    const song::Track& square = *TrackOf(next, 0);
    const song::Note& d = square.notes.at(0);
    XSFLIB_CHECK(d.key == 62);
    const edit::NoteEdit up{.command = d.command, .stretch = square.stretches.at(d.stretch), .transpose = 2};
    const File up_file = edit::EditNotes(file, SequenceOf(file, 1), std::span(&up, 1)).file;
    XSFLIB_CHECK(KeysOf(ReadOf(up_file, 1), 0) == (std::vector<int>{64}));
    XSFLIB_CHECK(KeysOf(ReadOf(up_file, 0), 0) == (std::vector<int>{60}));

    // Without the zero, song 0's commands take an odd size. The field of the song's length names the new end. Song 1's
    // piece starts where it did: the join adds a zero after song 0, and song 1 follows with the bytes that it had.
    File odd = file;
    ntdlib::commands::Sequence shorter = *file.sequences[first];
    SEQLIB_REQUIRE(!shorter.items.empty() && shorter.items.back().kind == Kind::kData &&
                   shorter.items.back().bytes == std::vector<uint8_t>{0x00});
    const auto end = static_cast<uint32_t>(shorter.items.size());
    shorter.items.pop_back();
    odd.sequences[first] = std::move(shorter);
    format::NativeArchive native = *file.native;
    for (ntdlib::sar::Field& field : native.fields)
    {
        if (field.to.item && !field.to.sequence && field.to.piece == songs[0] && field.to.offset == end)
        {
            field.to.offset = end - 1;
        }
    }
    odd.native = std::move(native);
    XSFLIB_CHECK(format::NativePieceStarts(odd).at(songs[1]) == next_header + 14);
    const std::vector<uint8_t> joined = format::ExportArchive(odd);
    const std::vector<uint8_t> next_bytes = NextSong();
    SEQLIB_REQUIRE(joined.size() == bytes.size());
    XSFLIB_CHECK(joined[next_header - 1] == 0);
    XSFLIB_CHECK(
        std::equal(next_bytes.begin(), next_bytes.end(), joined.begin() + static_cast<std::ptrdiff_t>(next_header)));
    XSFLIB_CHECK(format::SequenceNamedAt(odd, next_header + 2) == second);
}

// A song whose first channel sets tempo 90 and whose second sets tempo 60. Each plays a quarter note of C.
std::vector<uint8_t> TempoSong()
{
    return cart::Song({cart::Channel().Bytes(kSetup).Note(0, 3).End(),
                       cart::Channel().Bytes({0x7F, 60, 0x5E, 15, 0x4F, 2}).Note(0, 3).End()});
}

// A ROM of the driver's revision `revision` with a song that ends at once.
std::vector<uint8_t> RevisionImage(ntdlib::quintet::Revision revision)
{
    cart::Cart image(revision);
    image.Songs({cart::Song({cart::Channel().End()})});
    return RomBytes(image.ToRom());
}

// Each revision of the driver starts a song at the tempo that ntdlib's traits of the revision give: 90 in the A
// revision, 120 in the J revision and 150 in the S revision. A tempo T plays T of the performer's 3,552 ticks of a
// quarter note in a frame, and counts 3,600 / 3,552 beats a minute for each step at 60 frames a second. The rules of
// a file's revision alone give no start tempo: the A and J revisions share revision 0. The tempo commands of the first
// channel set the tempo of every channel. The tempo row shows them and leaves out those of the second channel.
void TestTempos()
{
    using ntdlib::quintet::Revision;
    for (const auto& [revision, start] :
         {std::pair(Revision::kA, 90), std::pair(Revision::kJ, 120), std::pair(Revision::kS, 150)})
    {
        const std::shared_ptr<const QuintetRom> rom = song::LoadQuintetRom(RevisionImage(revision));
        const std::optional<double> bpm = rom->Rules().StartTempoBpm();
        SEQLIB_REQUIRE(bpm.has_value());
        XSFLIB_CHECK(std::abs(*bpm - start * 3600.0 / 3552.0) < 1e-9);
        const Sequence read = rom->ReadSequence(0);
        XSFLIB_CHECK(read.driver != nullptr && read.driver->StartTempoBpm() == bpm);
    }
    XSFLIB_CHECK(!song::RulesOf(format::Engine::kQuintet, 0).StartTempoBpm());

    cart::Cart image(Revision::kA);
    image.Songs({TempoSong()});
    const std::vector<uint8_t> bytes = RomBytes(image.ToRom());
    const File file = format::DriverOfRom(bytes)->Import(bytes);
    const Sequence read = ReadOf(file, 0);
    const std::vector<hokuto::app::TempoPointView> points = hokuto::app::TempoPoints(file, SequenceOf(file, 0), read);
    SEQLIB_REQUIRE(points.size() == 1);
    XSFLIB_CHECK(points[0].track == 0 && points[0].tick == 0);

    // The point shows the tempo that plays: the driver's frames run at the GBA's frame rate, 16,777,216 / 280,896 a
    // second, and not at 60. The line and the transport show the same tempo, from ntdlib's converter
    // (ntdlib::agb::QuarterMicros, rounded to whole microseconds).
    SEQLIB_REQUIRE(points[0].bpm.has_value() && !read.tempo.empty());
    XSFLIB_CHECK(std::abs(*points[0].bpm - 90 * 3600.0 / 3552.0 * ntdlib::agb::kFrameRate / 60.0) < 1e-9);
    XSFLIB_CHECK(std::abs(*points[0].bpm - 60'000'000.0 / ntdlib::agb::QuarterMicros(3552, 90)) < 1e-3);
    XSFLIB_CHECK(std::abs(*points[0].bpm - read.tempo.front().bpm) < 1e-3);

    // A drag to the point's tempo, rounded to a whole beat a minute, keeps the command's value. One to 30 more sets the
    // value that plays nearest to it.
    const ntdlib::commands::Command& command = file.sequences[SequenceOf(file, 0)]->items.at(*edit::ItemAtOffset(
        edit::Offsets(*file.sequences[SequenceOf(file, 0)], edit::ConsoleOf(file)), points[0].command));
    const std::optional<ntdlib::commands::Command> same =
        hokuto::app::TempoCommand(file, SequenceOf(file, 0), points[0], std::round(*points[0].bpm));
    XSFLIB_CHECK(same && same->value.value == command.value.value);
    const std::optional<ntdlib::commands::Command> faster =
        hokuto::app::TempoCommand(file, SequenceOf(file, 0), points[0], std::round(*points[0].bpm) + 30.0);
    XSFLIB_CHECK(faster && faster->value.value == 120 * kOne);
}

// A tempo (7F) on the first channel sets every channel's tempo, and one on another channel sets that channel's alone.
// In this song, the first two channels set tempo 90 at tick 0, and the third sets 60. Each plays two quarter notes of
// C. A new tempo at the tempo row's point changes the second channel's 90 too, and leaves the third channel's 60. The
// two channels then start their second notes at one tick. With the first channel's command alone, the second channel
// keeps 90, and its second note starts elsewhere.
void TestTempoPartners()
{
    const std::vector<uint8_t> setup = {0x5E, 15, 0x4F, 2};
    cart::Cart image(ntdlib::quintet::Revision::kA);
    image.Songs({cart::Song({cart::Channel().Bytes({0x7F, 90}).Bytes(setup).Note(0, 3).Note(0, 3).End(),
                             cart::Channel().Bytes({0x7F, 90}).Bytes(setup).Note(0, 3).Note(0, 3).End(),
                             cart::Channel().Bytes({0x7F, 60}).Bytes(setup).Note(0, 3).Note(0, 3).End()})});
    const std::vector<uint8_t> bytes = RomBytes(image.ToRom());
    const File file = format::DriverOfRom(bytes)->Import(bytes);
    const uint32_t sequence = SequenceOf(file, 0);
    const Sequence read = ReadOf(file, 0);
    XSFLIB_CHECK(StartsOf(read, 0) == StartsOf(read, 1));
    const std::vector<hokuto::app::TempoPointView> points = hokuto::app::TempoPoints(file, sequence, read);
    SEQLIB_REQUIRE(points.size() == 1 && points[0].bpm.has_value());

    const std::optional<ntdlib::commands::Command> faster =
        hokuto::app::TempoCommand(file, sequence, points[0], *points[0].bpm + 30.0);
    SEQLIB_REQUIRE(faster.has_value());
    const hokuto::app::TempoChange change = hokuto::app::TempoChangeOf(file, sequence, read, points[0], *faster);
    SEQLIB_REQUIRE(change.places.size() == 2 && change.commands.size() == 2);
    XSFLIB_CHECK(change.places[0].command == points[0].command);
    XSFLIB_CHECK(change.commands[1].kind == Kind::kTempo && change.commands[1].value.value == faster->value.value);

    const File both = edit::ChangeCommands(file, sequence, change.places, change.commands).file;
    const Sequence both_read = ReadOf(both, 0);
    XSFLIB_CHECK(StartsOf(both_read, 0) == StartsOf(both_read, 1));
    XSFLIB_CHECK(PlaysAsReference(both, 0));
    std::size_t sixties = 0;
    for (const ntdlib::commands::Command& command : both.sequences[sequence]->items)
    {
        sixties += command.kind == Kind::kTempo && command.value.value == 60 * kOne ? 1 : 0;
    }
    XSFLIB_CHECK(sixties == 1);

    const File alone = edit::ChangeCommand(file, sequence, change.places[0], *faster).file;
    const Sequence alone_read = ReadOf(alone, 0);
    XSFLIB_CHECK(StartsOf(alone_read, 0) != StartsOf(alone_read, 1));
}

// A ROM whose song repeats a quarter note of C: D6 starts the repeat, and D7 02 ends it after 2 passes.
std::vector<uint8_t> RepeatImage()
{
    cart::Cart image(ntdlib::quintet::Revision::kA);
    image.Songs({cart::Song({cart::Channel().Bytes(kSetup).Bytes({0xD6}).Note(0, 3).Bytes({0xD7, 2}).End()})});

    return RomBytes(image.ToRom());
}

// A removal takes a repeat's start and its end together. The note then plays once. The removal of either alone is
// refused with a reason that names the repeat.
void TestRepeatRemoval()
{
    const std::vector<uint8_t> bytes = RepeatImage();
    const File file = format::DriverOfRom(bytes)->Import(bytes);
    const uint32_t sequence = SequenceOf(file, 0);
    const edit::CommandPlace start = PlaceOfKind(file, sequence, Kind::kLoopStart);
    const edit::CommandPlace end = PlaceOfKind(file, sequence, Kind::kLoopEnd);
    const std::vector<edit::CommandPlace> both = {start, end};

    const File removed = edit::RemoveCommands(file, sequence, both).file;

    XSFLIB_CHECK(StartsOf(ReadOf(file, 0), 0) == (std::vector<uint32_t>{0, Ticks(96)}));
    XSFLIB_CHECK(StartsOf(ReadOf(removed, 0), 0) == std::vector<uint32_t>{0});
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { edit::RemoveCommands(file, sequence, std::span(&start, 1)); })
                     .find("repeat") != std::string::npos);
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { edit::RemoveCommands(file, sequence, std::span(&end, 1)); })
                     .find("repeat") != std::string::npos);
}

// The exports equal those of ntdlib's converter, run directly.
void TestExports()
{
    const std::shared_ptr<const QuintetRom> rom = song::LoadQuintetRom(Image());
    SEQLIB_REQUIRE(rom->files.size() == 1);
    for (const uint32_t sound : {0u, 1u})
    {
        ntdlib::sequence_export::agb::ConvertSettings settings;
        settings.program = "Hokuto";
        const ntdlib::sequence_export::quintet::SongFiles expected = ntdlib::sequence_export::quintet::ConvertSongFiles(
            rom->rom, rom->files.front(), static_cast<int>(sound), settings);
        const ntdlib::sequence_export::Conversion conversion = rom->ConvertToMidi(sound, "Hokuto");
        XSFLIB_CHECK(expected.report.ok);
        XSFLIB_CHECK(conversion.midi == expected.midi);
        XSFLIB_CHECK(conversion.sf2 == expected.sf2);
        XSFLIB_CHECK(!conversion.midi.empty());
    }
}

// The wave channel of the A revision has the volume codes of 4 levels, 0 to 3, and the J revision's 5 (ntdlib's
// RevisionTraits::wave_levels). A level above them keeps the code before (ntdlib's quintet::Sequencer::Output). The
// rules of each revision's ROM give its top for a volume (5E) on the wave channel (song::QuintetMostVolume), and the
// other channels' tops.
void TestWaveTop()
{
    using ntdlib::quintet::Revision;
    for (const Revision revision : {Revision::kA, Revision::kJ})
    {
        cart::Cart image(revision);
        image.Songs({Song0()});
        const std::shared_ptr<const QuintetRom> rom = song::LoadQuintetRom(RomBytes(image.ToRom()));
        XSFLIB_CHECK(rom->Rules().MostVolume(ntdlib::quintet::kWave) == (revision == Revision::kA ? 3 : 4));
        XSFLIB_CHECK(rom->Rules().MostVolume(0) == 15 && rom->Rules().MostVolume(3) == 15);
        XSFLIB_CHECK(rom->Rules().MostVolume(4) == 1 && rom->Rules().MostVolume(5) == 1);
    }
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("quintet",
                                    []
                                    {
                                        TestImport();
                                        TestRead();
                                        TestPlays();
                                        TestRevisionS();
                                        TestNoteEdits();
                                        TestNoteAtVolume();
                                        TestWaveTop();
                                        TestNoteAfterTie();
                                        TestListKeys();
                                        TestFixedSettings();
                                        TestAuditionInstrument();
                                        TestTracks();
                                        TestMix();
                                        TestPieceStarts();
                                        TestMovedSongs();
                                        TestTempos();
                                        TestTempoPartners();
                                        TestRepeatRemoval();
                                        TestExports();
                                    });
}
