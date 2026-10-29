// SPDX-License-Identifier: MIT

// Tests the edits of a track's mix (src/edit/mix.h). A small Switch archive holds two sequence files. In the first,
// track 0 sets its volume to 100 and its pan, calls a subroutine that sets sends A and B, and plays a note. Its volume
// then falls to 50 for a second note. Track 1 plays a note before it calls the same subroutine and pans to the right.
// It sets no volume. In the second file, one sound starts at a note, and a jump back to the note loops it. Another
// sound starts at the tempo before it and runs on into it. Each edit goes through an export of the archive. ntdlib's
// performer then plays the edited archive, and the mix that it plays has to show the edit. A small 3DS archive checks
// the sends of the 3DS. The 3DS's tracks send to buses A and B only.

#include "edit/mix.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/commands/units.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/testing/sar_files.h"
#include "xsflib/testing/check.h"

#include "edit/notes.h"
#include "format/archive.h"
#include "format/file.h"
#include "song/archive.h"
#include "song/sequence.h"

using hokuto::edit::Edited;
using hokuto::edit::EditMix;
using hokuto::edit::MixEdit;
using hokuto::format::File;
using hokuto::song::Mix;
using hokuto::song::Sequence;
using hokuto::song::Setting;
using hokuto::song::Track;
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

// ntdlib's performer plays sound `sound` of the archive that `file` builds.
Sequence Played(const File& file, uint32_t sound)
{
    const std::shared_ptr<const sar::SoundArchive> archive =
        hokuto::song::LoadArchive(hokuto::format::ExportArchive(file), "test.bfsar");
    return hokuto::song::ReadSequence(*archive, sound);
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
    try
    {
        EditMix(file, sequence, track.number, track.stretches, edit);
    }
    catch (const std::invalid_argument&)
    {
        return true;
    }

    return false;
}

// A setting that holds one value takes the new one. The pan's command changes in place.
void TestSet()
{
    const File file = hokuto::format::ImportSwitchArchive(TestArchive());
    const Sequence before = Played(file, kMix);
    const Track& track = before.tracks.at(0);

    const File edited = EditMix(file, 0, track.number, track.stretches, {Mix::kPan, -32, 10}).file;

    const Sequence after = Played(edited, kMix);
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
    const Sequence before = Played(file, kMix);
    const Track& track = before.tracks.at(1);

    const File edited = EditMix(file, 0, track.number, track.stretches, {Mix::kPan, 32, 42}).file;

    const Sequence after = Played(edited, kMix);
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
    const Sequence before = Played(file, kMix);
    const Track& track = before.tracks.at(0);

    const File edited = EditMix(file, 0, track.number, track.stretches, {Mix::kVolume, 100, 50}).file;

    const Sequence after = Played(edited, kMix);
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
    const Sequence before = Played(file, kMix);
    const Track& track = before.tracks.at(1);

    const File edited = EditMix(file, 0, track.number, track.stretches, {Mix::kVolume, 127, 64}).file;

    const Sequence after = Played(edited, kMix);
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
    const Sequence before = Played(file, kMix);
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
    const Sequence before = Played(file, kMix);
    const Track& track = before.tracks.at(1);

    const File edited = EditMix(file, 0, track.number, track.stretches, {Mix::kSendA, 40, 80}).file;

    const Sequence after = Played(edited, kMix);
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
    const Sequence before = Played(file, kLoop);
    const Track& track = before.tracks.at(0);

    const File edited = EditMix(file, 1, track.number, track.stretches, {Mix::kVolume, 127, 90}).file;

    const Sequence after = Played(edited, kLoop);
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
    const Sequence loop = Played(file, kLoop);
    const Sequence intro = Played(file, kIntro);
    const Track& track = loop.tracks.at(0);

    const File edited = EditMix(file, 1, track.number, track.stretches, {Mix::kVolume, 127, 90}).file;

    const Sequence after = Played(edited, kIntro);
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
    const Sequence before = Played(file, kMix);
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
    const Sequence before = Played(file, kMix);
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
    const Sequence before = Played(file, 0);
    const Track& track = before.tracks.at(0);

    const File edited = EditMix(file, 0, track.number, track.stretches, {Mix::kSendA, 40, 20}).file;

    const Sequence after = Played(edited, 0);
    XSFLIB_CHECK(before.tracks[0].sends[0] == (std::vector<Setting>{{0, 40}}));
    XSFLIB_CHECK(after.tracks.at(0).sends[0] == (std::vector<Setting>{{0, 20}}));
    XSFLIB_CHECK(Refuses(file, 0, track, {Mix::kSendC, 0, 40}));
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
    const Sequence song = Played(file, kMix);
    const Sequence varied = Played(variable, kMix);
    const Sequence loop = Played(file, kLoop);

    XSFLIB_CHECK(Refuses(file, 0, song.tracks.at(1), {Mix::kSendA, 0, 40}));
    XSFLIB_CHECK(Refuses(variable, 0, varied.tracks.at(0), {Mix::kVolume, 100, 90}));
    XSFLIB_CHECK(Refuses(fixed, 1, loop.tracks.at(0), {Mix::kVolume, 127, 90}));
    XSFLIB_CHECK(!Refuses(fixed, 1, loop.tracks[0], {Mix::kPan, 0, 0}));
}

} // namespace

int main()
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
    TestRefusals();
    Test3ds();

    return xsflib::testing::Summary("mix");
}
