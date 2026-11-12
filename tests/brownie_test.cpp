// SPDX-License-Identifier: MIT

// Tests Hokuto's path for Brownie Brown's GBA driver: the import and the round trip, the sounds that the arrangement
// shows, playback, the edits of notes and settings, the mix, the tracks, the removal of a repeat and the exports.
// ntdlib's test cartridge of the driver (ntdlib/testing/brownie_cart.h) contains no game data. The cartridge
// includes the code patterns that ntdlib's detection reads, the driver's tables, a sample set and the sounds below.
//
// The expected ticks, keys and bytes come from the sounds' bytes below, worked out by hand. A length byte of 80 to DF
// chooses the channel's length from the driver's table: the table of the cartridge sets 24 frames for 89 and 32 for 8A.
// A note, a rest (EC or ED), a hold (FE) and a release wait for that length. A channel's first command runs in the
// frame after the sound's request, at tick 1.
//
// ntdlib's ArchivePlayer, run directly, produces the reference output. The model's output has to match it sample for
// sample. The MIDI file and the SoundFont have to equal those of ntdlib's converter, run directly.

#include "song/brownie.h"

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
#include "ntdlib/brownie/archive_player.h"
#include "ntdlib/brownie/driver.h"
#include "ntdlib/commands/sequence.h"
#include "ntdlib/sequence_export/agb/settings.h"
#include "ntdlib/sequence_export/brownie/convert.h"
#include "ntdlib/sequence_export/convert.h"
#include "ntdlib/testing/brownie_cart.h"
#include "xsflib/testing/check.h"

#include "driver_test_support.h"
#include "edit/brownie.h"
#include "edit/bytecode.h"
#include "edit/commands.h"
#include "edit/mix.h"
#include "edit/notes.h"
#include "edit/replace.h"
#include "edit/tracks.h"
#include "format/archive.h"
#include "format/brownie_items.h"
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
namespace brownie = ntdlib::brownie;
namespace cart = ntdlib::testing::brownie_cart;
using format::File;
using hokuto::testing::ClockDrift;
using hokuto::testing::ClockRun;
using hokuto::testing::ModelPlaysAsReference;
using hokuto::testing::PlaceOfKind;
using hokuto::testing::PlacesOf;
using hokuto::testing::RomBytes;
using hokuto::testing::RunDirect;
using hokuto::testing::SequenceOf;
using hokuto::testing::StartsOf;
using hokuto::testing::TrackOf;
using ntdlib::commands::Kind;
using song::BrownieRom;
using song::Sequence;
using xsflib::testing::ErrorOf;

constexpr uint32_t kRomBase = ntdlib::agb::kRomBase;

// The offset of the first channel's data from the start of the ROM.
constexpr uint32_t kData = cart::kSequences - kRomBase;

// Sound 1 plays square 1 and the first sample channel:
//
//   channel 0: F0 00  the envelope that holds
//              F1 80  NRx1
//              FD 11  both sides
//              89     a length of 24 frames
//              18     key 24 at tick 1
//              1A     key 26 at tick 25
//              ED     a rest at tick 49
//              FF     the end at tick 73
//   channel 8: E1 00  the sample set
//              EA FF  both sides at full level
//              F0 03  the sample channel's envelope that holds
//              F2 FF  the most volume
//              8A     a length of 32 frames
//              1E     key 30 at tick 1
//              EC     a rest at tick 33
//              FF     the end at tick 65
//
// Sound 2 loops on square 1: key 24 and key 26, 24 frames each, and F8 back to the start through word 0 of the
// channel's table.
std::vector<uint8_t> Cart()
{
    cart::Cart image;
    image.Sound(1, {{0, {0xF0, cart::kPsgHold, 0xF1, 0x80, 0xFD, 0x11, 0x89, 24, 26, 0xED, 0xFF}, {}},
                    {8, {0xE1, 0, 0xEA, 0xFF, 0xF0, cart::kSampleHold, 0xF2, 0xFF, 0x8A, 30, 0xEC, 0xFF}, {}}});
    image.Sound(2, {{0, {0xF0, cart::kPsgHold, 0xF1, 0x80, 0xFD, 0x11, 0x89, 24, 26, 0xF8, 0}, {0}}});

    return RomBytes(image.ToRom());
}

// The lengths of a track's notes.
std::vector<uint32_t> LengthsOf(const Sequence& sequence, int number)
{
    std::vector<uint32_t> lengths;
    if (const song::Track* track = TrackOf(sequence, number))
    {
        for (const song::Note& note : track->notes)
        {
            lengths.push_back(note.length);
        }
    }

    return lengths;
}

Sequence ReadOf(const File& file, uint32_t sound)
{
    return song::LoadBrownieRom(format::ExportArchive(file))->ReadSequence(sound);
}

// The MIDI key that the arrangement shows for the byte `byte` on square 1.
int ShownKey(const BrownieRom& rom, uint8_t byte)
{
    return song::BrownieShownKey(rom.info.revision, 0, brownie::PsgTuning(rom.rom, rom.info).base, 0, byte);
}

// The reference: ntdlib's player run directly up to `samples` stereo samples.
std::vector<int16_t> DirectStrict(const BrownieRom& rom, int song, std::size_t samples)
{
    brownie::ArchivePlayer player(rom.rom, rom.info);
    player.Start(song, {});
    return RunDirect<int16_t>(player, samples);
}

// The model plays `file`'s sound `sound` as the reference does, in strict mixing, and makes a sound.
bool PlaysAsReference(const File& file, uint32_t sound)
{
    const std::shared_ptr<const BrownieRom> rom = song::LoadBrownieRom(format::ExportArchive(file));
    return ModelPlaysAsReference(
        rom, sound, [&](std::size_t samples) { return DirectStrict(*rom, rom->SongOf(sound), samples); }, 90);
}

// The edit of a note of sound 1's track `track`.
edit::NoteEdit EditOf(const Sequence& read, int track, std::size_t note)
{
    const song::Note& played = TrackOf(read, track)->notes.at(note);
    return {.command = played.command, .stretch = TrackOf(read, track)->stretches.at(played.stretch)};
}

// A ROM with the driver comes in as engine 10 of the format, and comes back byte for byte from both forms of the file.
// Each command that waits lasts its frames. A ROM without the driver's code doesn't count as such a ROM.
void TestImport()
{
    const std::vector<uint8_t> bytes = Cart();
    const format::Driver* driver = format::DriverOfRom(bytes);
    SEQLIB_REQUIRE(driver != nullptr);
    XSFLIB_CHECK(driver->Facts().engine == format::Engine::kBrownie);
    cart::Cart without(false);
    const ntdlib::agb::Rom empty = without.ToRom();
    XSFLIB_CHECK(format::DriverOfRom(RomBytes(empty)) == nullptr);

    const File file = driver->Import(bytes);
    XSFLIB_CHECK(file.engine == format::Engine::kBrownie);
    SEQLIB_REQUIRE(file.native && file.native->driver);
    XSFLIB_CHECK(static_cast<brownie::Revision>(file.native->driver->revision) == brownie::Revision::kSwordOfMana);
    XSFLIB_CHECK(format::ExportArchive(file) == bytes);

    const File binary = format::Read(format::Write(file));
    XSFLIB_CHECK(binary.engine == format::Engine::kBrownie);
    XSFLIB_CHECK(format::ExportArchive(binary) == bytes);
    XSFLIB_CHECK(format::Write(binary) == format::Write(file));

    const File text = format::ReadText(format::WriteText(file));
    XSFLIB_CHECK(text.engine == format::Engine::kBrownie);
    XSFLIB_CHECK(format::ExportArchive(text) == bytes);
    XSFLIB_CHECK(format::Write(text) == format::Write(file));

    // Square 1's notes and rest wait 24 frames, and the sample channel's 32.
    const ntdlib::commands::Sequence& sequence = *file.sequences.at(SequenceOf(file, 1));
    std::vector<uint32_t> waits;
    for (const ntdlib::commands::Command& item : sequence.items)
    {
        if (format::BrownieWaits(item))
        {
            waits.push_back(format::BrownieWaitOf(item));
        }
    }
    XSFLIB_CHECK(std::ranges::count(waits, 24u) >= 3);
    XSFLIB_CHECK(std::ranges::count(waits, 32u) >= 2);
    XSFLIB_CHECK(std::ranges::find(waits, 0u) == waits.end());
}

// The sounds that the arrangement shows: the notes at their ticks, with their keys and lengths, at velocity 127.
void TestRead()
{
    const std::shared_ptr<const BrownieRom> rom = song::LoadBrownieRom(Cart());
    XSFLIB_CHECK(std::ranges::count(rom->Sounds(), 1u) == 1);
    XSFLIB_CHECK(rom->Rules().DriverEngine() == format::Engine::kBrownie);

    const Sequence sound1 = rom->ReadSequence(1);
    XSFLIB_CHECK(StartsOf(sound1, 0) == (std::vector<uint32_t>{1, 25}));
    XSFLIB_CHECK(LengthsOf(sound1, 0) == (std::vector<uint32_t>{24, 24}));
    SEQLIB_REQUIRE(TrackOf(sound1, 0) && TrackOf(sound1, 0)->notes.size() == 2);
    XSFLIB_CHECK(TrackOf(sound1, 0)->notes[0].key == ShownKey(*rom, 24));
    XSFLIB_CHECK(TrackOf(sound1, 0)->notes[1].key == ShownKey(*rom, 26));
    XSFLIB_CHECK(TrackOf(sound1, 0)->notes[0].velocity == 127);
    XSFLIB_CHECK(StartsOf(sound1, 8) == (std::vector<uint32_t>{1}));
    XSFLIB_CHECK(LengthsOf(sound1, 8) == (std::vector<uint32_t>{32}));
    XSFLIB_CHECK(!sound1.loop);

    const Sequence sound2 = rom->ReadSequence(2);
    SEQLIB_REQUIRE(sound2.loop.has_value());
    XSFLIB_CHECK(sound2.loop->end - sound2.loop->start == 48);
}

// The model plays as ntdlib's player, run directly.
void TestPlays()
{
    const std::vector<uint8_t> bytes = Cart();
    const File file = format::DriverOfRom(bytes)->Import(bytes);
    XSFLIB_CHECK(PlaysAsReference(file, 1));
    XSFLIB_CHECK(PlaysAsReference(file, 2));
}

// An edit of a note reaches the sound's bytes. A note keeps its tick and a removed note leaves a rest. A length byte is
// inserted before a command that waits a length other than the length of its channel.
void TestNoteEdits()
{
    const std::vector<uint8_t> bytes = Cart();
    const File file = format::DriverOfRom(bytes)->Import(bytes);
    const uint32_t sequence = SequenceOf(file, 1);
    const Sequence sound1 = ReadOf(file, 1);

    // A transposed note changes its byte alone.
    edit::NoteEdit transposed = EditOf(sound1, 0, 0);
    transposed.transpose = 2;
    std::vector<uint8_t> expected = bytes;
    expected[kData + 7] = 26;
    XSFLIB_CHECK(format::ExportArchive(edit::EditNotes(file, sequence, std::span(&transposed, 1)).file) == expected);

    // A move of a tick leaves a rest of a frame before the note. The note sounds up to the next note.
    edit::NoteEdit moved = EditOf(sound1, 0, 0);
    moved.shift = 1;
    const File moved_file = edit::EditNotes(file, sequence, std::span(&moved, 1)).file;
    const Sequence after_move = ReadOf(moved_file, 1);
    XSFLIB_CHECK(StartsOf(after_move, 0) == (std::vector<uint32_t>{2, 25}));
    XSFLIB_CHECK(LengthsOf(after_move, 0) == (std::vector<uint32_t>{23, 24}));
    XSFLIB_CHECK(StartsOf(after_move, 8) == StartsOf(sound1, 8));
    XSFLIB_CHECK(PlaysAsReference(moved_file, 1));

    // A removed note leaves a rest.
    edit::NoteEdit removed = EditOf(sound1, 0, 0);
    removed.remove = true;
    XSFLIB_CHECK(StartsOf(ReadOf(edit::EditNotes(file, sequence, std::span(&removed, 1)).file, 1), 0) ==
                 (std::vector<uint32_t>{25}));

    // A shorter note puts a rest in at its new end. A longer one moves the rest that ends it.
    edit::NoteEdit shorter = EditOf(sound1, 0, 1);
    shorter.length = 12u;
    const File shorter_file = edit::EditNotes(file, sequence, std::span(&shorter, 1)).file;
    XSFLIB_CHECK(LengthsOf(ReadOf(shorter_file, 1), 0) == (std::vector<uint32_t>{24, 12}));
    XSFLIB_CHECK(ReadOf(shorter_file, 1).length == sound1.length);
    XSFLIB_CHECK(PlaysAsReference(shorter_file, 1));
    edit::NoteEdit longer = EditOf(sound1, 0, 1);
    longer.length = 30u;
    XSFLIB_CHECK(LengthsOf(ReadOf(edit::EditNotes(file, sequence, std::span(&longer, 1)).file, 1), 0) ==
                 (std::vector<uint32_t>{24, 30}));

    // A note sounds up to the next note at most, and has no velocity.
    edit::NoteEdit past = EditOf(sound1, 0, 0);
    past.length = 30u;
    XSFLIB_CHECK(
        !ErrorOf<std::invalid_argument>([&] { edit::EditNotes(file, sequence, std::span(&past, 1)); }).empty());
    edit::NoteEdit quieter = EditOf(sound1, 0, 0);
    quieter.velocity = 100;
    XSFLIB_CHECK(
        !ErrorOf<std::invalid_argument>([&] { edit::EditNotes(file, sequence, std::span(&quieter, 1)); }).empty());

    // A new note in the sample channel's rest, 8 frames long.
    const song::Track& samples = *TrackOf(sound1, 8);
    const edit::NewNote added{.stretch = samples.stretches.front(), .tick = 41, .key = 32, .length = 8};
    const File added_file = edit::EditNotes(file, sequence, {}, std::span(&added, 1)).file;
    const Sequence with_new = ReadOf(added_file, 1);
    XSFLIB_CHECK(StartsOf(with_new, 8) == (std::vector<uint32_t>{1, 41}));
    XSFLIB_CHECK(LengthsOf(with_new, 8) == (std::vector<uint32_t>{32, 8}));
    XSFLIB_CHECK(StartsOf(with_new, 0) == StartsOf(sound1, 0));
    XSFLIB_CHECK(PlaysAsReference(added_file, 1));

    // The limits of the second note: its block runs from tick 1 to 73, and the rest ends it.
    const edit::NotePlace place{.command = EditOf(sound1, 0, 1).command, .stretch = EditOf(sound1, 0, 1).stretch};
    const std::vector<edit::NoteLimits> limits = edit::LimitsOf(file, sequence, std::span(&place, 1));
    SEQLIB_REQUIRE(limits.size() == 1 && limits[0].shift && limits[0].length);
    XSFLIB_CHECK(limits[0].shift->earlier == 24 && limits[0].shift->later == 47);
    XSFLIB_CHECK(*limits[0].length == 24);
}

// The removal of a note removes its release too. Sound 1 plays square 1:
//
//   F0 00  the envelope that holds
//   F1 80  NRx1
//   FD 11  both sides
//   89     a length of 24 frames
//   18     key 24 at tick 1
//   1A     key 26 at tick 25
//   E0 01  a release at tick 49, with the envelope that steps
//   1C     key 28 at tick 73
//   FF     the end at tick 97
//
// A release steps the envelope in the next frame. The step writes NRx2 and starts the channel again at the frequency of
// its last note. After a rest, the release would sound key 24 from tick 50. The removal turns the note into a rest
// (ED), and the release into a hold (FE) of its frames. Square 1 then plays keys 24 and 28 alone.
void TestRemovedRelease()
{
    cart::Cart image;
    image.Sound(1, {{0,
                     {0xF0, cart::kPsgHold, 0xF1, 0x80, 0xFD, 0x11, 0x89, 24, 26, 0xE0,
                      static_cast<uint8_t>(cart::kPsgSteps), 28, 0xFF},
                     {}}});
    const std::vector<uint8_t> bytes = RomBytes(image.ToRom());
    const File file = format::DriverOfRom(bytes)->Import(bytes);
    const uint32_t sequence = SequenceOf(file, 1);
    const Sequence sound1 = ReadOf(file, 1);
    SEQLIB_REQUIRE(StartsOf(sound1, 0) == (std::vector<uint32_t>{1, 25, 73}));

    edit::NoteEdit removed = EditOf(sound1, 0, 1);
    removed.remove = true;
    const File removed_file = edit::EditNotes(file, sequence, std::span(&removed, 1)).file;
    const Sequence after = ReadOf(removed_file, 1);
    XSFLIB_CHECK(StartsOf(after, 0) == (std::vector<uint32_t>{1, 73}));
    XSFLIB_CHECK(LengthsOf(after, 0) == (std::vector<uint32_t>{24, 24}));
    XSFLIB_CHECK(after.length == sound1.length);
    XSFLIB_CHECK(PlaysAsReference(removed_file, 1));
}

// A new setting is inserted before the command that waits at its tick. The note there continues with a hold.
void TestSettings()
{
    const std::vector<uint8_t> bytes = Cart();
    const File file = format::DriverOfRom(bytes)->Import(bytes);
    const uint32_t sequence = SequenceOf(file, 1);
    const Sequence sound1 = ReadOf(file, 1);

    ntdlib::commands::Command volume;
    volume.kind = Kind::kVolume;
    volume.value.value = 0x20 * ntdlib::commands::kOne;
    const edit::NewSetting setting{.stretch = TrackOf(sound1, 0)->stretches.front(), .tick = 13, .command = volume};
    const File with_volume = edit::AddSettings(file, sequence, std::span(&setting, 1)).file;
    const Sequence read = ReadOf(with_volume, 1);
    XSFLIB_CHECK(StartsOf(read, 0) == StartsOf(sound1, 0));
    SEQLIB_REQUIRE(TrackOf(read, 0) && !TrackOf(read, 0)->volume.empty());
    XSFLIB_CHECK(TrackOf(read, 0)->volume.back().tick == 13 && TrackOf(read, 0)->volume.back().value == 0x20);
    XSFLIB_CHECK(PlaysAsReference(with_volume, 1));
}

// The byte of a note that the Key Editor draws on a track without notes (edit::BrownieCommandKey). A note of the noise
// shows at its byte.
void TestCommandKeys()
{
    const std::vector<uint8_t> bytes = Cart();
    const File file = format::DriverOfRom(bytes)->Import(bytes);
    const std::shared_ptr<const BrownieRom> rom = song::LoadBrownieRom(bytes);
    XSFLIB_CHECK(edit::BrownieCommandKey(file, 0, 0, ShownKey(*rom, 24)) == 24);
    XSFLIB_CHECK(edit::BrownieCommandKey(file, 0, 3, ShownKey(*rom, 27)) == 24);
    XSFLIB_CHECK(edit::BrownieCommandKey(file, brownie::kNoiseChannel, 0, 50) == 50);
    XSFLIB_CHECK(!edit::BrownieCommandKey(file, 0, 0, ShownKey(*rom, 0) - 1));
}

// The mixer's volume changes the F2 of sample channel 8 from FF to 20. On a PSG channel, F2 lowers the envelope's
// level, and the mixer refuses the edit. The mixer refuses a pan on both kinds of channel.
void TestMix()
{
    const std::vector<uint8_t> bytes = Cart();
    const File file = format::DriverOfRom(bytes)->Import(bytes);
    const uint32_t sequence = SequenceOf(file, 1);
    const Sequence sound1 = ReadOf(file, 1);

    const edit::MixEdit volume{.setting = song::Mix::kVolume, .from = 0xFF, .to = 0x20};
    XSFLIB_CHECK(!ErrorOf<std::invalid_argument>(
                      [&] { edit::EditMix(file, sequence, 0, TrackOf(sound1, 0)->stretches, volume); })
                      .empty());
    const edit::MixEdit pan{.setting = song::Mix::kPan, .from = 64, .to = 32};
    XSFLIB_CHECK(
        !ErrorOf<std::invalid_argument>([&] { edit::EditMix(file, sequence, 0, TrackOf(sound1, 0)->stretches, pan); })
             .empty());
    XSFLIB_CHECK(
        !ErrorOf<std::invalid_argument>([&] { edit::EditMix(file, sequence, 8, TrackOf(sound1, 8)->stretches, pan); })
             .empty());

    const File edited = edit::EditMix(file, sequence, 8, TrackOf(sound1, 8)->stretches, volume).file;
    XSFLIB_CHECK(edited.sequences[sequence]->items.size() == file.sequences[sequence]->items.size());
    const Sequence read = ReadOf(edited, 1);
    SEQLIB_REQUIRE(TrackOf(read, 8) && !TrackOf(read, 8)->volume.empty());
    XSFLIB_CHECK(TrackOf(read, 8)->volume.back().value == 0x20);
    XSFLIB_CHECK(StartsOf(read, 8) == StartsOf(sound1, 8));
    XSFLIB_CHECK(StartsOf(read, 0) == StartsOf(sound1, 0));
    XSFLIB_CHECK(PlaysAsReference(edited, 1));
}

// The Project menu's edits of tracks change the sound's list of channels.
void TestTracks()
{
    const std::vector<uint8_t> bytes = Cart();
    const File file = format::DriverOfRom(bytes)->Import(bytes);
    const std::vector<std::optional<edit::SoundPlace>> places = PlacesOf(file);
    const Sequence sound1 = ReadOf(file, 1);
    const Sequence sound2 = ReadOf(file, 2);

    // A new track uses the first free sample channel, and rests as long as the sound.
    const edit::TrackEdit added = edit::AddTrack(file, places, 1, sound1, std::nullopt);
    XSFLIB_CHECK(added.track == brownie::kFirstSampleChannel + 1);
    const Sequence with_new = ReadOf(added.file, 1);
    XSFLIB_CHECK(StartsOf(with_new, 0) == StartsOf(sound1, 0));
    XSFLIB_CHECK(StartsOf(with_new, 8) == StartsOf(sound1, 8));
    XSFLIB_CHECK(TrackOf(with_new, added.track) != nullptr);
    // The new track's first command runs at tick 1, and so do the first commands of the sound's other channels. The
    // sound ends as before.
    XSFLIB_CHECK(with_new.length == sound1.length);
    XSFLIB_CHECK(with_new.loop == sound1.loop);

    // With the instrument of square 1's first note, a new track uses square 2, with the duty of square 1 there (F1).
    const song::Note& first = TrackOf(sound1, 0)->notes.front();
    const edit::TrackEdit square =
        edit::AddTrack(file, places, 1, sound1, edit::Program{first.bank_slot, static_cast<int>(first.program)});
    XSFLIB_CHECK(square.track == 1);
    const std::vector<ntdlib::commands::Command>& items = square.file.sequences[SequenceOf(file, 1)]->items;
    XSFLIB_CHECK(std::ranges::any_of(items,
                                     [&first](const ntdlib::commands::Command& item)
                                     {
                                         return item.kind == ntdlib::commands::Kind::kProgram && item.index == 2 &&
                                                item.value.value ==
                                                    static_cast<int64_t>(first.program) * ntdlib::commands::kOne;
                                     }));
    XSFLIB_CHECK(ReadOf(square.file, 1).length == sound1.length);

    // A copy of square 1 plays on square 2.
    const edit::TrackEdit copy = edit::DuplicateTrack(file, places, 1, 0);
    XSFLIB_CHECK(copy.track == 1);
    const Sequence with_copy = ReadOf(copy.file, 1);
    XSFLIB_CHECK(StartsOf(with_copy, 1) == StartsOf(sound1, 0));
    XSFLIB_CHECK(StartsOf(with_copy, 0) == StartsOf(sound1, 0));
    XSFLIB_CHECK(PlaysAsReference(copy.file, 1));

    // A removed channel leaves the list.
    const File without = edit::RemoveTrack(file, places, 1, 8);
    XSFLIB_CHECK(StartsOf(ReadOf(without, 1), 8).empty());
    XSFLIB_CHECK(StartsOf(ReadOf(without, 1), 0) == StartsOf(sound1, 0));
    XSFLIB_CHECK(StartsOf(ReadOf(without, 2), 0) == StartsOf(sound2, 0));

    // Where both sounds are in one sequence, a track of sound 1 is added to sound 2 on a free channel of its kind.
    const edit::CopiedTrack copied = edit::CopyTrack(file, places, 1, sound1, 8);
    XSFLIB_CHECK(copied.driver == format::Engine::kBrownie);
    if (places[1]->sequence == places[2]->sequence)
    {
        const edit::TrackEdit pasted = edit::PasteTrack(file, places, 2, sound2, copied);
        XSFLIB_CHECK(pasted.track == brownie::kFirstSampleChannel);
        XSFLIB_CHECK(StartsOf(ReadOf(pasted.file, 2), pasted.track) == StartsOf(sound1, 8));
        XSFLIB_CHECK(StartsOf(ReadOf(pasted.file, 1), 8) == StartsOf(sound1, 8));
    }
    else
    {
        XSFLIB_CHECK(
            !ErrorOf<std::invalid_argument>([&] { edit::PasteTrack(file, places, 2, sound2, copied); }).empty());
    }
}

// A ROM whose sound 1 repeats key 24 on square 1: F4 02 starts a repeat of 2 passes, and F5 ends it.
std::vector<uint8_t> RepeatCart()
{
    cart::Cart image;
    image.Sound(1, {{0, {0xF0, cart::kPsgHold, 0xF1, 0x80, 0xFD, 0x11, 0x89, 0xF4, 2, 24, 0xF5, 0xFF}, {}}});

    return RomBytes(image.ToRom());
}

// A removal deletes the start and the end of a repeat together. The note then plays once. The removal of either alone
// is refused with a reason that names the repeat.
void TestRepeatRemoval()
{
    const std::vector<uint8_t> bytes = RepeatCart();
    const File file = format::DriverOfRom(bytes)->Import(bytes);
    const uint32_t sequence = SequenceOf(file, 1);
    const edit::CommandPlace start = PlaceOfKind(file, sequence, Kind::kLoopStart);
    const edit::CommandPlace end = PlaceOfKind(file, sequence, Kind::kLoopEnd);
    const std::vector<edit::CommandPlace> both = {start, end};

    const File removed = edit::RemoveCommands(file, sequence, both).file;

    XSFLIB_CHECK(StartsOf(ReadOf(file, 1), 0) == (std::vector<uint32_t>{1, 25}));
    XSFLIB_CHECK(StartsOf(ReadOf(removed, 1), 0) == std::vector<uint32_t>{1});
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { edit::RemoveCommands(file, sequence, std::span(&start, 1)); })
                     .find("repeat") != std::string::npos);
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { edit::RemoveCommands(file, sequence, std::span(&end, 1)); })
                     .find("repeat") != std::string::npos);
}

// The exports equal those of ntdlib's converter, run directly.
void TestExports()
{
    const std::shared_ptr<const BrownieRom> rom = song::LoadBrownieRom(Cart());
    for (const uint32_t sound : {1u, 2u})
    {
        ntdlib::sequence_export::agb::ConvertSettings settings;
        settings.program = "Hokuto";
        const ntdlib::sequence_export::brownie::SongFiles expected =
            ntdlib::sequence_export::brownie::ConvertSongFiles(rom->rom, rom->info, rom->SongOf(sound), settings);
        const ntdlib::sequence_export::Conversion conversion = rom->ConvertToMidi(sound, "Hokuto");
        XSFLIB_CHECK(expected.report.ok);
        XSFLIB_CHECK(conversion.midi == expected.midi);
        XSFLIB_CHECK(conversion.sf2 == expected.sf2);
        XSFLIB_CHECK(!conversion.midi.empty() && !conversion.sf2.empty());
    }
}

// The model's clock follows the arrangement: after 10 s of play, or at the song's end, the arrangement's time at the
// clock is within 2 frames of the time played.
void TestClockFollowsArrangement()
{
    int long_runs = 0;
    const std::shared_ptr<const BrownieRom> rom = song::LoadBrownieRom(Cart());
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
    return xsflib::testing::RunTest("brownie",
                                    []
                                    {
                                        TestImport();
                                        TestRead();
                                        TestPlays();
                                        TestClockFollowsArrangement();
                                        TestNoteEdits();
                                        TestRemovedRelease();
                                        TestSettings();
                                        TestCommandKeys();
                                        TestMix();
                                        TestTracks();
                                        TestRepeatRemoval();
                                        TestExports();
                                    });
}
