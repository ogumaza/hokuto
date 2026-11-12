// SPDX-License-Identifier: MIT

// Tests the edits of a track's mix (src/edit/mix.h). A small Switch archive holds two sequence files. In the first,
// track 0 sets its volume to 100 and its pan, calls a subroutine that sets sends A and B, and plays a note. Its volume
// then falls to 50 for a second note. Track 1 plays a note before it calls the same subroutine and pans to the right.
// It sets no volume. In the second file, one sound starts at a note, and a jump back to the note loops it. Another
// sound starts at the tempo before it and runs on into it. Each edit goes through an export of the archive. ntdlib's
// performer then plays the edited archive, and the mix that it plays has to show the edit. A small 3DS archive checks
// the sends of the 3DS. The 3DS's tracks send to buses A and B only. A small GBA ROM checks MusicPlayer2000's volume
// and pan. A GBA track plays at volume 0 until a command sets it, and has no sends. A small DS archive checks a new
// command at the opening of track 0, after the allocation of tracks at the sequence's start. Each edit counts the
// values that the end of the setting's range stops.

#include "edit/mix.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "ntdlib/agb/rom.h"
#include "ntdlib/commands/sequence.h"
#include "ntdlib/commands/units.h"
#include "ntdlib/mp2k/song.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sdat/layout.h"
#include "ntdlib/testing/mp2k_cart.h"
#include "ntdlib/testing/sar_files.h"
#include "ntdlib/testing/sdat_files.h"
#include "xsflib/testing/check.h"

#include "cart_bytes.h"
#include "edit/notes.h"
#include "format/archive.h"
#include "format/file.h"
#include "song/archive.h"
#include "song/ds.h"
#include "song/gba.h"
#include "song/sequence.h"
#include "sound_places.h"

using hokuto::edit::Edited;
using hokuto::edit::EditMix;
using hokuto::edit::MixEdit;
using hokuto::edit::MixEdited;
using hokuto::format::File;
using hokuto::song::Mix;
using hokuto::song::Sequence;
using hokuto::song::Setting;
using hokuto::song::Track;
using hokuto::testing::PlayedSequence;
using ntdlib::commands::Argument;
using ntdlib::commands::Command;
using ntdlib::commands::Kind;
using ntdlib::commands::kOne;
using ntdlib::commands::Source;

namespace
{

namespace sar = ntdlib::sar;
namespace sar_files = ntdlib::testing::sar_files;

// The archive's sounds.
constexpr uint32_t kMix = 0;
constexpr uint32_t kLoop = 1;
constexpr uint32_t kIntro = 2;

// The first file's bytecode. Each command's offset comes first in its comment. The Switch's sequences give arguments in
// little-endian order. Each note waits for its length.
const std::vector<uint8_t> kMixBytecode = {
    0xB0, 48,              // 0: timebase 48
    0xE1, 120, 0x00,       // 2: tempo 120
    0x88, 1,   27,   0, 0, // 5: open track 1 at 27
    0xC1, 100,             // 10: volume 100
    0xC0, 32,              // 12: pan 32, or -32 from the centre
    0x8A, 40,  0,    0,    // 14: call 40
    60,   100, 24,         // 18: key 60 for 24 ticks, at tick 0
    0xC1, 50,              // 21: volume 50, at tick 24
    62,   100, 24,         // 23: key 62 for 24 ticks
    0xFF,                  // 26: the end
    64,   100, 24,         // 27: track 1: key 64 for 24 ticks, at tick 0
    0x8A, 40,  0,    0,    // 30: call 40, at tick 24
    0xC0, 96,              // 34: pan 96, or 32 from the centre
    65,   100, 24,         // 36: key 65 for 24 ticks
    0xFF,                  // 39: the end
    0xD9, 40,              // 40: send A 40
    0xDA, 20,              // 42: send B 20
    0xFD,                  // 44: return
};

// The second file's bytecode. One sound starts at offset 5, and the jump goes back to its first command. The other
// starts at offset 2.
const std::vector<uint8_t> kLoopBytecode = {
    0xB0, 48,           // 0: timebase 48
    0xE1, 120, 0x00,    // 2: tempo 120
    60,   100, 24,      // 5: key 60 for 24 ticks
    0x89, 5,   0,    0, // 8: jump 5
};

// A 3DS sequence's bytecode. The 3DS's sequences give 16- and 24-bit arguments in big-endian order.
const std::vector<uint8_t> kCtrBytecode = {
    0xB0, 48,        // 0: timebase 48
    0xE1, 0x00, 120, // 2: tempo 120
    0xD9, 40,        // 5: send A 40
    60,   100,  24,  // 7: key 60 for 24 ticks
    0xFF,            // 10: the end
};

// The archive. Its bank plays a looped wave for every key.
std::vector<uint8_t> TestArchive()
{
    sar_files::WaveSpec wave;
    wave.encoding = 1;
    wave.loop = true;
    wave.sample_rate = 48000;
    wave.loop_end = 64;
    wave.channels = {std::vector<uint8_t>(128, 0x10)};

    sar_files::Region region;
    region.original_key = 60;
    sar_files::VelocityTable velocities;
    velocities.entries = {{0, region}};
    sar_files::KeyTable keys;
    keys.entries = {{0, velocities}};
    sar_files::BankSpec bank;
    bank.waves = {{0x05000000, 0}};
    bank.instruments = {keys};

    sar_files::SoundSpec mix;
    mix.name = "SEQ_MIX";
    mix.volume = 127;
    mix.banks = {0};
    mix.allocate_track_flags = 3;
    sar_files::SoundSpec loop = mix;
    loop.name = "SEQ_LOOP";
    loop.file = 1;
    loop.allocate_track_flags = 1;
    loop.start_offset = 5;
    sar_files::SoundSpec intro = loop;
    intro.name = "SEQ_INTRO";
    intro.start_offset = 2;

    sar_files::ArchiveSpec spec;
    spec.sounds = {mix, loop, intro};
    spec.banks = {{"BANK_TEST", 2}};
    spec.wave_archives = {{"WARC_TEST", 3}};
    spec.players = {{"PLAYER_TEST", 1}};
    spec.files = {{sar_files::Sequence(false, kMixBytecode, {{"Mix", 0}}), "", false},
                  {sar_files::Sequence(false, kLoopBytecode, {{"Intro", 2}, {"Loop", 5}}), "", false},
                  {sar_files::Bank(false, bank), "", false},
                  {sar_files::WaveArchive(false, {sar_files::Wave(false, wave)}), "", false}};

    return sar_files::Archive(false, spec);
}

// `file` with `value` in each volume command of its first sequence.
File WithVolumes(const File& file, const Argument& value)
{
    File changed = file;
    ntdlib::commands::Sequence sequence = *file.sequences[0];
    for (Command& command : sequence.items)
    {
        if (command.kind == Kind::kVolume)
        {
            command.value = value;
        }
    }
    changed.sequences[0] = sequence;

    return changed;
}

// The start, length, key and velocity of each note of `track`. A new command moves the commands of the notes after it.
std::vector<std::vector<uint32_t>> Notes(const Track& track)
{
    std::vector<std::vector<uint32_t>> notes;
    for (const hokuto::song::Note& note : track.notes)
    {
        notes.push_back({note.start, note.length, note.key, note.velocity});
    }

    return notes;
}

// Whether `edit` of `track` of `sequence` refuses with a message for the user.
bool Refuses(const File& file, uint32_t sequence, const Track& track, const MixEdit& edit)
{
    return !xsflib::testing::ErrorOf<std::invalid_argument>(
                [&] { EditMix(file, sequence, track.number, track.stretches, edit); })
                .empty();
}

// A setting that holds one value takes the new one. The pan's command changes in place.
void TestSet()
{
    const File file = hokuto::format::ImportSwitchArchive(TestArchive());
    const Sequence before = PlayedSequence(file, kMix);
    const Track& track = before.tracks.at(0);

    const File edited = EditMix(file, 0, track.number, track.stretches, {Mix::kPan, -32, 10}).file;

    const Sequence after = PlayedSequence(edited, kMix);
    const std::vector<Setting> kPan = {{0, 10}};
    XSFLIB_CHECK(before.tracks[0].pan == (std::vector<Setting>{{0, -32}}));
    XSFLIB_CHECK(after.tracks.at(0).pan == kPan && after.tracks[1].pan == before.tracks[1].pan);
    XSFLIB_CHECK(edited.sequences[0]->items.size() == file.sequences[0]->items.size());
}

// Track 1 plays a note at the default pan before it pans to 32. The pan moves by the edit's amount from the default
// too, and a new command at the track's start sets that.
void TestNoteBeforeCommand()
{
    const File file = hokuto::format::ImportSwitchArchive(TestArchive());
    const Sequence before = PlayedSequence(file, kMix);
    const Track& track = before.tracks.at(1);

    const File edited = EditMix(file, 0, track.number, track.stretches, {Mix::kPan, 32, 42}).file;

    const Sequence after = PlayedSequence(edited, kMix);
    const std::vector<Setting> kBefore = {{24, 32}};
    const std::vector<Setting> kAfter = {{0, 10}, {24, 42}};
    XSFLIB_CHECK(before.tracks[1].pan == kBefore && after.tracks.at(1).pan == kAfter);
    XSFLIB_CHECK(after.tracks[0].pan == before.tracks[0].pan);
}

// A volume that changes as the track plays takes the edit's factor in each command: a cut from 100 to 50 halves the
// fade's 50 too.
void TestTrim()
{
    const File file = hokuto::format::ImportSwitchArchive(TestArchive());
    const Sequence before = PlayedSequence(file, kMix);
    const Track& track = before.tracks.at(0);

    const File edited = EditMix(file, 0, track.number, track.stretches, {Mix::kVolume, 100, 50}).file;

    const Sequence after = PlayedSequence(edited, kMix);
    const std::vector<Setting> kBefore = {{0, 100}, {24, 50}};
    const std::vector<Setting> kAfter = {{0, 50}, {24, 25}};
    XSFLIB_CHECK(before.tracks[0].volume == kBefore && after.tracks.at(0).volume == kAfter);
    XSFLIB_CHECK(after.tracks[1].volume.empty());
}

// Track 1 sets no volume. A new command at its start sets it, and the command that opens the track leads to it. The
// notes keep their ticks.
void TestNewCommand()
{
    const File file = hokuto::format::ImportSwitchArchive(TestArchive());
    const Sequence before = PlayedSequence(file, kMix);
    const Track& track = before.tracks.at(1);

    const File edited = EditMix(file, 0, track.number, track.stretches, {Mix::kVolume, 127, 64}).file;

    const Sequence after = PlayedSequence(edited, kMix);
    const std::vector<Setting> kVolume = {{0, 64}};
    XSFLIB_CHECK(after.tracks.at(1).volume == kVolume && after.tracks[0].volume == before.tracks[0].volume);
    XSFLIB_CHECK(Notes(after.tracks[1]) == Notes(before.tracks[1]) &&
                 Notes(after.tracks[0]) == Notes(before.tracks[0]));
    XSFLIB_CHECK(edited.sequences[0]->items.size() == file.sequences[0]->items.size() + 1);
}

// An edit that changes values leaves each item at its index. A new command moves the items from the track's first
// command on, and the commands of the notes after it move with them: track 1's last note from offset 36 to 38.
void TestItems()
{
    const File file = hokuto::format::ImportSwitchArchive(TestArchive());
    const Sequence before = PlayedSequence(file, kMix);
    const Track& track = before.tracks.at(1);
    const std::vector<Command>& items = file.sequences[0]->items;
    constexpr uint32_t kFirst = 10; // track 1's first command, the note at offset 27
    const std::vector<uint32_t> kNotes = {18, 36};

    const Edited set = EditMix(file, 0, before.tracks[0].number, before.tracks[0].stretches, {Mix::kPan, -32, 10});
    const Edited added = EditMix(file, 0, track.number, track.stretches, {Mix::kVolume, 127, 64});
    const std::vector<uint32_t> notes = hokuto::edit::CommandsAfter(file, 0, added, kNotes);

    SEQLIB_REQUIRE(set.items.size() == items.size() && added.items.size() == items.size());
    for (uint32_t item = 0; item < items.size(); item++)
    {
        const std::optional<uint32_t> moved = added.items[item];
        XSFLIB_CHECK(set.items[item] == item);
        XSFLIB_CHECK(moved == (item < kFirst ? item : item + 1));
        XSFLIB_CHECK(moved && added.file.sequences[0]->items.at(*moved).kind == items[item].kind);
    }
    XSFLIB_CHECK(notes == (std::vector<uint32_t>{18, 38}));
}

// Track 1's send A comes from the subroutine that both tracks call, after a note at the default of 0. The edit doubles
// the subroutine's value, and track 0's send changes with it. Send B stays as it was. A default that the edit leaves at
// 0 takes no new command.
void TestShared()
{
    const File file = hokuto::format::ImportSwitchArchive(TestArchive());
    const Sequence before = PlayedSequence(file, kMix);
    const Track& track = before.tracks.at(1);

    const File edited = EditMix(file, 0, track.number, track.stretches, {Mix::kSendA, 40, 80}).file;

    const Sequence after = PlayedSequence(edited, kMix);
    const std::vector<Setting> kTrack0 = {{0, 80}};
    const std::vector<Setting> kTrack1 = {{24, 80}};
    XSFLIB_CHECK(before.tracks[1].sends[0] == (std::vector<Setting>{{24, 40}}));
    XSFLIB_CHECK(after.tracks.at(0).sends[0] == kTrack0 && after.tracks[1].sends[0] == kTrack1);
    XSFLIB_CHECK(after.tracks[0].sends[1] == (std::vector<Setting>{{0, 20}}));
    XSFLIB_CHECK(edited.sequences[0]->items.size() == file.sequences[0]->items.size());
}

// A jump back to the sound's first command goes on to the command that was first. The sound starts at the new command.
// The label at the sound's start goes on naming the command that the jump reaches, and the new command runs once. The
// tempo before the sound's start runs on into it, and a jump takes that path past the new command.
void TestJumpBack()
{
    const File file = hokuto::format::ImportSwitchArchive(TestArchive());
    const Sequence before = PlayedSequence(file, kLoop);
    const Track& track = before.tracks.at(0);

    const File edited = EditMix(file, 1, track.number, track.stretches, {Mix::kVolume, 127, 90}).file;

    const Sequence after = PlayedSequence(edited, kLoop);
    const std::vector<Command>& items = edited.sequences[1]->items;
    const std::vector<Setting> kVolume = {{0, 90}};
    XSFLIB_CHECK(after.tracks.at(0).volume == kVolume && Notes(after.tracks[0]) == Notes(before.tracks[0]));
    XSFLIB_CHECK(items.size() == 6 && items[2].kind == Kind::kJump && items[2].target == 4 &&
                 items[3].kind == Kind::kVolume && items[4].kind == Kind::kNote && items[5].target == 4);
    const std::vector<ntdlib::commands::Label>& labels = edited.sequences[1]->labels;
    XSFLIB_CHECK(labels.size() == 2 && labels[0].item == 1 && labels[1].item == 4);
}

// The second sound starts before the first and runs on into its start. A new command at the first sound's start leaves
// the second sound's volume as it was.
void TestRunOn()
{
    const File file = hokuto::format::ImportSwitchArchive(TestArchive());
    const Sequence loop = PlayedSequence(file, kLoop);
    const Sequence intro = PlayedSequence(file, kIntro);
    const Track& track = loop.tracks.at(0);

    const File edited = EditMix(file, 1, track.number, track.stretches, {Mix::kVolume, 127, 90}).file;

    const Sequence after = PlayedSequence(edited, kIntro);
    XSFLIB_CHECK(intro.tracks.at(0).volume.empty() && after.tracks.at(0).volume.empty());
    XSFLIB_CHECK(Notes(after.tracks[0]) == Notes(intro.tracks[0]) && !intro.tracks[0].notes.empty());
}

// An end that runs only when a comparison held can run on into the start of track 1. A new command at the start then
// gets a jump before it, and the items after the end move past both.
void TestConditionalEnd()
{
    File file = hokuto::format::ImportSwitchArchive(TestArchive());
    ntdlib::commands::Sequence sequence = *file.sequences[0];
    constexpr uint32_t kEnd = 9;
    XSFLIB_CHECK(sequence.items.at(kEnd).kind == Kind::kEnd);
    sequence.items[kEnd].conditional = true;
    file.sequences[0] = sequence;
    const Sequence before = PlayedSequence(file, kMix);
    const Track& track = before.tracks.at(1);

    const Edited edited = EditMix(file, 0, track.number, track.stretches, {Mix::kVolume, 127, 64});

    const std::vector<Command>& items = edited.file.sequences[0]->items;
    SEQLIB_REQUIRE(items.size() == sequence.items.size() + 2);
    XSFLIB_CHECK(items[kEnd + 1].kind == Kind::kJump && items[kEnd + 1].target == kEnd + 3 &&
                 items[kEnd + 2].kind == Kind::kVolume && items[2].target == kEnd + 2);
    XSFLIB_CHECK(edited.items.at(kEnd) == kEnd && edited.items.at(kEnd + 1) == kEnd + 3);
}

// A volume that a random number picks counts as one that changes. The edit trims both ends of its range.
void TestRandom()
{
    const File file =
        WithVolumes(hokuto::format::ImportSwitchArchive(TestArchive()), {Source::kRandom, 80 * kOne, 120 * kOne});
    const Sequence before = PlayedSequence(file, kMix);
    const Track& track = before.tracks.at(0);

    const File edited = EditMix(file, 0, track.number, track.stretches, {Mix::kVolume, 100, 50}).file;

    const Argument kTrimmed = {Source::kRandom, 40 * kOne, 60 * kOne};
    std::vector<Argument> volumes;
    for (const Command& command : edited.sequences[0]->items)
    {
        if (command.kind == Kind::kVolume)
        {
            volumes.push_back(command.value);
        }
    }
    XSFLIB_CHECK(volumes == (std::vector<Argument>{kTrimmed, kTrimmed}));
}

// A value that a raise would take past the end of its range stops there, and the edit counts it. Track 0's volume falls
// from 100 to 50. A raise from 50 to 150 triples both: 100 stops at 255, the highest volume of the Switch, and 50
// becomes 150. A cut leaves every value inside the range. Track 1's pan of 32 comes after a note at the default of 0. A
// pan 40 to the right from 0 stops 32 at 63, and the default becomes 40.
void TestStopped()
{
    const File file = hokuto::format::ImportSwitchArchive(TestArchive());
    const Sequence before = PlayedSequence(file, kMix);
    const Track& first = before.tracks.at(0);
    const Track& second = before.tracks.at(1);

    const MixEdited louder = EditMix(file, 0, first.number, first.stretches, {Mix::kVolume, 50, 150});
    const MixEdited quieter = EditMix(file, 0, first.number, first.stretches, {Mix::kVolume, 100, 50});
    const MixEdited panned = EditMix(file, 0, second.number, second.stretches, {Mix::kPan, 0, 40});

    XSFLIB_CHECK(louder.stopped == 1 && quieter.stopped == 0 && panned.stopped == 1);
    XSFLIB_CHECK(PlayedSequence(louder.file, kMix).tracks.at(0).volume == (std::vector<Setting>{{0, 255}, {24, 150}}));
    XSFLIB_CHECK(PlayedSequence(panned.file, kMix).tracks.at(1).pan == (std::vector<Setting>{{0, 40}, {24, 63}}));
}

// A 3DS track's send to bus A takes an edit. The 3DS's tracks send to buses A and B only, and an edit of a send to bus
// C is refused.
void Test3ds()
{
    sar_files::BankSpec bank = sar_files::OneRegionBank();
    bank.waves = {{0x05000000, 0}};
    sar_files::SoundSpec sound;
    sound.name = "SEQ_CTR";
    sound.volume = 127;
    sound.banks = {0};
    sound.allocate_track_flags = 1;
    sar_files::ArchiveSpec spec;
    spec.sounds = {sound};
    spec.banks = {{"BANK_CTR", 2}};
    spec.wave_archives = {{"WARC_CTR", 3}};
    spec.players = {{"PLAYER_CTR", 1}};
    spec.files = {{sar_files::Sequence(sar::Flavor::kCtr, kCtrBytecode, {{"Ctr", 0}}), "", false},
                  {sar_files::Bank(sar::Flavor::kCtr, bank), "", false},
                  {sar_files::WaveArchive(sar::Flavor::kCtr,
                                          {sar_files::Wave(sar::Flavor::kCtr, sar_files::ConstantWave(false))}),
                   "", false}};
    const File file = hokuto::format::ImportArchive(sar_files::Archive(sar::Flavor::kCtr, spec), sar::Flavor::kCtr);
    const Sequence before = PlayedSequence(file, 0);
    const Track& track = before.tracks.at(0);

    const File edited = EditMix(file, 0, track.number, track.stretches, {Mix::kSendA, 40, 20}).file;

    const Sequence after = PlayedSequence(edited, 0);
    XSFLIB_CHECK(before.tracks[0].sends[0] == (std::vector<Setting>{{0, 40}}));
    XSFLIB_CHECK(after.tracks.at(0).sends[0] == (std::vector<Setting>{{0, 20}}));
    XSFLIB_CHECK(Refuses(file, 0, track, {Mix::kSendC, 0, 40}));
}

// A cartridge whose song 0 plays two tracks. Track 0 sets its volume to 100 and its pan to 0x30, 16 to the left, and
// plays key 60 at tick 0. Its volume then falls to 50 for key 62 at tick 24. Track 1 sets no pan. It plays key 64 at
// tick 0, at volume 0, before it sets its volume to 90 for key 65 at tick 24. The bytes from 0x38000 on are 0xFF: the
// ROM's free space.
std::vector<uint8_t> GbaRom()
{
    namespace mp2k_cart = ntdlib::testing::mp2k_cart;
    using enum ntdlib::mp2k::Command;

    mp2k_cart::Cart cart;
    const uint32_t voices = cart.VoiceGroup({{0, mp2k_cart::Cart::Voice(0, cart.Wave(mp2k_cart::Saw(), 13379, 0))}});
    mp2k_cart::Track first;
    first.Cmd(kCmdTempo, 75).Cmd(kCmdVoice, 0).Cmd(kCmdVol, 100).Cmd(kCmdPan, 0x30).Note(24, 60, 100).Wait(24);
    first.Cmd(kCmdVol, 50).Note(24, 62, 100).Wait(24).Fine();
    mp2k_cart::Track second;
    second.Cmd(kCmdVoice, 0).Note(24, 64, 100).Wait(24).Cmd(kCmdVol, 90).Note(24, 65, 100).Wait(24).Fine();
    cart.Song({first, second}, voices);
    for (uint32_t a = 0x38000; a < 0x40000; a++)
    {
        cart.Put8(mp2k_cart::kRomBase + a, 0xFF);
    }

    const ntdlib::agb::Rom rom = cart.ToRom();
    return hokuto::testing::RomBytes(rom);
}

// The sequence of the file of a GBA ROM that contains song 0, and the song as ntdlib's performer plays it from the ROM.
struct GbaSong
{
    uint32_t sequence = 0;
    Sequence played;
};

// Song 0 of the ROM that `file` builds.
GbaSong GbaSongOf(const File& file)
{
    const std::shared_ptr<const hokuto::song::GbaRom> rom =
        hokuto::song::LoadGbaRom(hokuto::format::ExportArchive(file));
    return {hokuto::format::GbaSongSequence(file, rom->info.song_table, 0).value(),
            hokuto::song::ReadSequence(*rom, 0)};
}

// A GBA track's volume runs from 0 to 127, and starts at 0. A cut of track 0's volume from 100 to 50 halves the fall to
// 50 too. Track 1's volume of 0 before its command stays at 0, and no new command is added. A pan moves track 1's
// default, and a new command at the track's start sets it. The field of the song's header that starts the track leads
// to it. A volume stays inside its range: a doubling raises 100 to 127, and the edit counts that value. Each ROM comes
// back byte for byte from its import.
void TestGba()
{
    const File file = hokuto::format::ImportGbaRom(GbaRom());
    const GbaSong song = GbaSongOf(file);
    const Track& first = song.played.tracks.at(0);
    const Track& second = song.played.tracks.at(1);
    XSFLIB_CHECK(first.volume == (std::vector<Setting>{{0, 100}, {24, 50}}));
    XSFLIB_CHECK(first.pan == (std::vector<Setting>{{0, -16}}) && second.pan.empty());
    XSFLIB_CHECK(second.volume == (std::vector<Setting>{{0, 0}, {24, 90}}));
    XSFLIB_CHECK(hokuto::edit::MixHighest(Mix::kVolume, true) == 127);

    const auto check = [&song](const File& edited, std::size_t track, const std::vector<Setting>& volume,
                               const std::vector<Setting>& pan)
    {
        const GbaSong after = GbaSongOf(edited);
        XSFLIB_CHECK(after.played.tracks.at(track).volume == volume && after.played.tracks[track].pan == pan);
        XSFLIB_CHECK(after.played.tracks.size() == song.played.tracks.size());
        for (std::size_t i = 0; i < song.played.tracks.size(); i++)
        {
            XSFLIB_CHECK(Notes(after.played.tracks[i]) == Notes(song.played.tracks[i]));
            if (i != track)
            {
                XSFLIB_CHECK(after.played.tracks[i].volume == song.played.tracks[i].volume &&
                             after.played.tracks[i].pan == song.played.tracks[i].pan);
            }
        }
        const std::vector<uint8_t> exported = hokuto::format::ExportArchive(edited);
        XSFLIB_CHECK(hokuto::format::ExportArchive(hokuto::format::ImportGbaRom(exported)) == exported);
    };

    const MixEdited cut = EditMix(file, song.sequence, 0, first.stretches, {Mix::kVolume, 100, 50});
    check(cut.file, 0, {{0, 50}, {24, 25}}, {{0, -16}});
    const Edited quieter = EditMix(file, song.sequence, 1, second.stretches, {Mix::kVolume, 90, 45});
    check(quieter.file, 1, {{0, 0}, {24, 45}}, {});
    XSFLIB_CHECK(quieter.file.sequences[song.sequence]->items.size() == file.sequences[song.sequence]->items.size());
    const Edited panned = EditMix(file, song.sequence, 1, second.stretches, {Mix::kPan, 0, 20});
    check(panned.file, 1, {{0, 0}, {24, 90}}, {{0, 20}});
    XSFLIB_CHECK(panned.file.sequences[song.sequence]->items.size() == file.sequences[song.sequence]->items.size() + 1);
    const MixEdited loud = EditMix(file, song.sequence, 0, first.stretches, {Mix::kVolume, 100, 200});
    check(loud.file, 0, {{0, 127}, {24, 100}}, {{0, -16}});
    XSFLIB_CHECK(loud.stopped == 1 && cut.stopped == 0);

    XSFLIB_CHECK(Refuses(file, song.sequence, first, {Mix::kSendA, 0, 40}));
}

// A DS archive with one sequence. Its first command allocates tracks 0 and 1. Track 0 opens track 1 and plays a note
// without a pan. Track 1 sets its volume and plays a note.
std::vector<uint8_t> DsArchive()
{
    namespace sdat_files = ntdlib::testing::sdat_files;
    const sdat_files::Bytes two_tracks = {0xFE, 0x03, 0x00, 0x93, 0x01, 0x0C, 0x00, 0x00, 0x3C,
                                          0x7F, 0x30, 0xFF, 0xC1, 0x40, 0x43, 0x50, 0x18, 0xFF};
    const sdat_files::Wave square{
        .loop = 1,
        .loop_length = 4,
        .data = {0x40, 0x40, 0xC0, 0xC0, 0x40, 0x40, 0xC0, 0xC0, 0x40, 0x40, 0xC0, 0xC0, 0x40, 0x40, 0xC0, 0xC0}};
    constexpr uint16_t kNone = 0xFFFF;

    ntdlib::sdat::ArchiveModel model;
    model.sequences = {ntdlib::sdat::SequenceInfo{0, 0, 127, 64, 64, 0, 0, ""}};
    model.banks = {ntdlib::sdat::BankInfo{1, {0, kNone, kNone, kNone}, ""}};
    model.wave_archives = {ntdlib::sdat::WaveArchiveInfo{2, ""}};
    model.players = {ntdlib::sdat::PlayerInfo{1, 0, 0, 0, ""}};
    model.files = {sdat_files::Sequence(two_tracks), sdat_files::Bank({{.type = 1, .region = {.wave = 0}}}),
                   sdat_files::WaveArchive({square})};

    return ntdlib::sdat::BuildArchive(model);
}

// On the DS, track 0 runs the allocation of tracks before its first command. A new pan at its opening is placed after
// the allocation, without a jump past it: the pan applies to track 0, and track 1 still plays. A jump to the allocation
// would run the new command elsewhere, and the edit refuses it.
void TestDs()
{
    const File file = hokuto::format::ImportDsArchive(DsArchive());
    const auto played = [](const File& of)
    {
        return hokuto::song::ReadSequence(*hokuto::song::LoadDsArchive(hokuto::format::ExportArchive(of)), 0);
    };
    const Sequence song = played(file);
    SEQLIB_REQUIRE(song.tracks.size() == 2 && song.tracks[0].pan.empty());

    const Edited panned = EditMix(file, 0, 0, song.tracks[0].stretches, {Mix::kPan, 0, 16});
    const Sequence after = played(panned.file);
    const std::vector<Command>& items = panned.file.sequences[0]->items;
    XSFLIB_CHECK(after.tracks.size() == 2 && after.tracks[0].pan == (std::vector<Setting>{{0, 16}}));
    XSFLIB_CHECK(after.tracks.size() == 2 && Notes(after.tracks[1]) == Notes(song.tracks[1]));
    XSFLIB_CHECK(items.size() == file.sequences[0]->items.size() + 1 && items[0].kind == Kind::kAllocateTracks &&
                 items[1].kind == Kind::kPan);

    File looped = file;
    ntdlib::commands::Sequence back = *file.sequences[0];
    back.items.insert(back.items.begin() + 3, Command{.kind = Kind::kJump, .target = 0});
    for (Command& item : back.items)
    {
        item.target += item.kind == Kind::kOpenTrack ? 1 : 0;
    }
    looped.sequences[0] = back;
    const std::string refusal = xsflib::testing::ErrorOf<std::invalid_argument>(
        [&] { EditMix(looped, 0, 0, song.tracks[0].stretches, {Mix::kPan, 0, 16}); });
    XSFLIB_CHECK(refusal.find("track allocation") != std::string::npos);

    // A DS track's volume stops at 127: the driver's table of levels ends there (SND_CalcDecibelSquare).
    const Edited quiet = EditMix(file, 0, 0, song.tracks[0].stretches, {Mix::kVolume, 127, 100});
    const Sequence quiet_song = played(quiet.file);
    XSFLIB_CHECK(quiet_song.tracks.at(0).volume == (std::vector<Setting>{{0, 100}}));
    const Edited loud = EditMix(quiet.file, 0, 0, quiet_song.tracks[0].stretches, {Mix::kVolume, 100, 200});
    XSFLIB_CHECK(loud.file.sequences[0]->items.at(1).kind == Kind::kVolume &&
                 loud.file.sequences[0]->items[1].value.value == 127 * kOne);
    XSFLIB_CHECK(hokuto::song::MixAt(played(loud.file).tracks.at(0), Mix::kVolume, 0) == 127);
}

// A trim from 0 has no factor. A volume that a variable sets can't change, and a sequence whose bytes keep their places
// takes no new command.
void TestRefusals()
{
    const File file = hokuto::format::ImportSwitchArchive(TestArchive());
    const File variable = WithVolumes(file, {Source::kVariable, 3, 0});
    File fixed = file;
    ntdlib::commands::Sequence data = *file.sequences[1];
    data.items.push_back(Command{.kind = Kind::kData, .bytes = {0}});
    fixed.sequences[1] = data;
    const Sequence song = PlayedSequence(file, kMix);
    const Sequence varied = PlayedSequence(variable, kMix);
    const Sequence loop = PlayedSequence(file, kLoop);

    XSFLIB_CHECK(Refuses(file, 0, song.tracks.at(1), {Mix::kSendA, 0, 40}));
    XSFLIB_CHECK(Refuses(variable, 0, varied.tracks.at(0), {Mix::kVolume, 100, 90}));
    XSFLIB_CHECK(Refuses(fixed, 1, loop.tracks.at(0), {Mix::kVolume, 127, 90}));
    XSFLIB_CHECK(!Refuses(fixed, 1, loop.tracks[0], {Mix::kPan, 0, 0}));
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("mix",
                                    []
                                    {
                                        TestSet();
                                        TestTrim();
                                        TestNoteBeforeCommand();
                                        TestNewCommand();
                                        TestItems();
                                        TestShared();
                                        TestJumpBack();
                                        TestRunOn();
                                        TestConditionalEnd();
                                        TestRandom();
                                        TestStopped();
                                        TestRefusals();
                                        Test3ds();
                                        TestGba();
                                        TestDs();
                                    });
}
