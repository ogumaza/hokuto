// SPDX-License-Identifier: MIT

// Tests the new items of a file's archive (src/edit/new_items.h). A small archive of the Switch and one of the Wii U,
// laid out as NintendoWare's tools lay them out, each hold two sequence sounds of one file, a wave sound and two banks.
// The first sequence sound selects both banks and allocates two tracks. The second starts at a note. The new sequence
// comes from a file with two labels. The first plays a note after a program change. The second selects bank slot 1,
// changes the program and calls a subroutine. A second archive holds a bank whose waves lie in its two wave archives.
// The first wave archive has a name, and the second has none. The new bank comes from another archive: its waves name
// that archive's wave archives 3 and 7. The new wave sounds come from a file whose waves name the same two, in another
// order. A bank or a wave sound file of the Wii U or the 3DS comes in as a file of the Switch. A sequence file that
// only a group file beside the archive holds keeps its sequence's number when a new sound comes in. A Wii archive takes
// a new sequence sound, a bank and wave sounds through its model. The checks read the exported archive with ntdlib.

#include "edit/new_items.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/commands/units.h"
#include "ntdlib/rsar/archive_model.h"
#include "ntdlib/rsar/sequence_file.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sar/info_block.h"
#include "ntdlib/sar/sequence_file.h"
#include "ntdlib/sar/string_block.h"
#include "ntdlib/sar/tables.h"
#include "ntdlib/testing/rsar_files.h"
#include "ntdlib/testing/sar_files.h"
#include "ntdlib/testing/sar_tools.h"
#include "xsflib/binary.h"
#include "xsflib/testing/check.h"

#include "edit/replace.h"
#include "format/archive.h"
#include "format/file.h"
#include "format/model.h"
#include "song/archive.h"

using hokuto::edit::AddBank;
using hokuto::edit::AddSequenceSound;
using hokuto::edit::AddWaveArchive;
using hokuto::edit::AddWaveSounds;
using hokuto::edit::HoldsNames;
using hokuto::edit::NoNamesReason;
using hokuto::edit::ProgramMap;
using hokuto::edit::ReadIncoming;
using hokuto::edit::SoundPlace;
using hokuto::format::File;
using ntdlib::commands::kOne;
using ntdlib::commands::Sequence;
using ntdlib::sar::Flavor;
using ntdlib::sar::SoundArchive;

namespace
{

namespace sar = ntdlib::sar;
namespace sar_files = ntdlib::testing::sar_files;
namespace sar_tools = ntdlib::testing::sar_tools;

// The archive's sounds, and the first new one.
constexpr uint32_t kFirst = 0;
constexpr uint32_t kSecond = 1;
constexpr uint32_t kWaveSound = 2;
constexpr uint32_t kAdded = 3;

// The archive's sequence file. The second sound starts at offset 4.
const std::vector<uint8_t> kBytecode = {
    0xB0, 48,      // 0: timebase 48
    0xC1, 100,     // 2: volume 100
    60,   100, 24, // 4: key 60 for 24 ticks
    0xFF,          // 7: the end
};

// The new sequence's bytecode in the Switch's byte order. Intro starts at 0 and Main at 8.
const std::vector<uint8_t> kNewBytecode = {
    0xB0, 48,         // 0: timebase 48
    0x81, 5,          // 2: program 5
    60,   100, 24,    // 4: key 60 for 24 ticks
    0xFF,             // 7: the end
    0xB6, 1,          // 8: bank slot 1
    0x81, 7,          // 10: program 7
    0x8A, 17,  0,  0, // 12: call 17
    0xFF,             // 16: the end
    62,   100, 24,    // 17: key 62 for 24 ticks
    0xFD,             // 20: return
};

// The same as the Wii U holds it: the call's offset is big-endian.
const std::vector<uint8_t> kWiiUNewBytecode = {0xB0, 48,   0x81, 5, 60, 100,  24, 0xFF, 0xB6, 1,   0x81,
                                               7,    0x8A, 0,    0, 17, 0xFF, 62, 100,  24,   0xFD};

// The item of the new sequence where Main starts.
constexpr uint32_t kMain = 4;

// The version of the archive's sequence file. The Wii U's file holds an older one than the files that ntdlib's builders
// write.
uint32_t VersionOf(Flavor flavor)
{
    return flavor == Flavor::kCafe ? 0x00010000 : 0x00020000;
}

// An archive of `flavor`, laid out as NintendoWare's tools lay one out. SEQ_FIRST and SEQ_SECOND play kBytecode from
// the labels First and Second, and WSD_SOUND plays an empty wave sound file. The entry of SEQ_FIRST gives a volume, a
// pan, priorities and the tracks to allocate, with the banks BANK_A and BANK_B. SEQ_SECOND plays BANK_B. With `names`
// false, the archive holds no names.
std::vector<uint8_t> TestArchive(Flavor flavor, bool names = true)
{
    sar_files::SoundSpec first;
    first.name = "SEQ_FIRST";
    first.volume = 90;
    first.pan = 0x0101;
    first.priority = 0x0230;
    first.banks = {0, 1};
    first.allocate_track_flags = 3;
    first.start_offset = 0;
    first.channel_priority = 0x0150;
    sar_files::SoundSpec second;
    second.name = "SEQ_SECOND";
    second.banks = {1};
    second.start_offset = 4;
    sar_files::SoundSpec wave_sound;
    wave_sound.name = "WSD_SOUND";
    wave_sound.kind = sar_files::SoundSpec::kWaveSound;
    wave_sound.file = 1;

    std::vector<uint8_t> sequence = sar_files::Sequence(flavor, kBytecode, {{"First", 0}, {"Second", 4}});
    if (flavor == Flavor::kCafe)
    {
        sequence[9] = 0x01; // the version's second byte in the Wii U's byte order
    }

    sar_files::ArchiveSpec spec;
    spec.sounds = {first, second, wave_sound};
    spec.banks = {{"BANK_A", 2}, {"BANK_B", 3}};
    spec.players = {{"PLAYER_TEST", 1}};
    spec.files = {{sequence, "", false},
                  {sar_files::WaveSoundFile(flavor, {}), "", false},
                  {sar_files::Bank(flavor, {}), "", false},
                  {sar_files::Bank(flavor, {}), "", false}};
    spec.names = names;

    return sar_tools::Archive(flavor, spec);
}

// The new sequence from a file of `flavor`.
Sequence NewSequence(Flavor flavor)
{
    const bool wii_u = flavor == Flavor::kCafe;
    return ReadIncoming(
               sar_files::Sequence(flavor, wii_u ? kWiiUNewBytecode : kNewBytecode, {{"Intro", 0}, {"Main", 8}}))
        .sequence;
}

// `archive`, an archive of `flavor`, read by ntdlib's strict rules.
SoundArchive Load(const std::vector<uint8_t>& archive, Flavor flavor)
{
    sar::Options options;
    options.flavor = flavor;
    options.rules = sar::Rules::kStopAtFirstProblem;

    return SoundArchive::Load(archive, options);
}

// The second archive's wave archives, and those that the new bank's waves name in its archive.
const uint32_t kNamedArchive = sar::ItemId(sar::kWaveArchiveItem, 0);
const uint32_t kUnnamedArchive = sar::ItemId(sar::kWaveArchiveItem, 1);
const uint32_t kOtherArchive3 = sar::ItemId(sar::kWaveArchiveItem, 3);
const uint32_t kOtherArchive7 = sar::ItemId(sar::kWaveArchiveItem, 7);

// A wave archive with `count` waves of 16-bit PCM. Wave i holds 16 samples, each of whose bytes is `first` + i.
std::vector<uint8_t> TestWaveArchive(Flavor flavor, uint8_t count, uint8_t first)
{
    std::vector<std::vector<uint8_t>> waves;
    for (uint8_t i = 0; i < count; ++i)
    {
        sar_files::WaveSpec wave;
        wave.encoding = 1;
        wave.channels = {std::vector<uint8_t>(32, static_cast<uint8_t>(first + i))};
        waves.push_back(sar_files::Wave(flavor, wave));
    }

    return sar_files::WaveArchive(flavor, waves);
}

// A bank with `waves`, by their wave archives' item IDs and their indices, and an instrument at program 0 that plays
// the first wave.
std::vector<uint8_t> TestBank(Flavor flavor, const std::vector<std::pair<uint32_t, uint32_t>>& waves)
{
    sar_files::Region region;
    region.original_key = 60;
    sar_files::VelocityTable velocities;
    velocities.entries = {{0, region}};
    sar_files::KeyTable keys;
    keys.entries = {{0, velocities}};
    sar_files::BankSpec bank;
    bank.waves = waves;
    bank.instruments = {keys};

    return sar_files::Bank(flavor, bank);
}

// A wave sound file with a wave sound for each of `notes`. Each sound's note plays that wave of `waves`. The waves are
// given by their wave archives' item IDs and their indices.
std::vector<uint8_t> TestWaveSounds(Flavor flavor, const std::vector<std::pair<uint32_t, uint32_t>>& waves,
                                    const std::vector<uint32_t>& notes)
{
    sar_files::WaveSoundFileSpec spec;
    spec.waves = waves;
    for (const uint32_t note : notes)
    {
        sar_files::WaveSoundSpec sound;
        sound.tracks = {{{0.0f, 1.0f, 0}}};
        sar_files::Region region;
        region.wave = note;
        sound.notes = {region};
        spec.sounds.push_back(sound);
    }

    return sar_files::WaveSoundFile(flavor, spec);
}

// The second archive: a sequence sound that plays BANK_A, BANK_A with waves in both wave archives, the named wave
// archive WARC_A with two waves, and a wave archive without a name with three.
std::vector<uint8_t> BankArchive(Flavor flavor, bool names = true)
{
    sar_files::SoundSpec sound;
    sound.name = "SEQ_FIRST";
    sound.banks = {0};
    sound.start_offset = 0;
    sar_files::ArchiveSpec spec;
    spec.sounds = {sound};
    spec.banks = {{"BANK_A", 1}};
    spec.wave_archives = {{"WARC_A", 2}, {"", 3}};
    spec.players = {{"PLAYER_TEST", 1}};
    spec.files = {{sar_files::Sequence(flavor, kBytecode, {{"First", 0}}), "", false},
                  {TestBank(flavor, {{kNamedArchive, 0}, {kUnnamedArchive, 2}}), "", false},
                  {TestWaveArchive(flavor, 2, 10), "", false},
                  {TestWaveArchive(flavor, 3, 20), "", false}};
    spec.names = names;

    return sar_tools::Archive(flavor, spec);
}

// The wave archives that the entry of bank `bank` of `archive` lists, by their item IDs.
std::vector<uint32_t> ListedWaveArchives(const std::vector<uint8_t>& archive, Flavor flavor, uint32_t bank)
{
    const sar::InfoBlock info = sar::InfoBlock::Read(sar::TablesOf(archive, flavor).info, flavor);
    const xsflib::Reader entry(info.banks.at(bank), flavor == Flavor::kCafe);
    const std::size_t list = entry.U32(8);
    std::vector<uint32_t> listed;
    for (uint32_t k = 0; k < entry.U32(list); ++k)
    {
        listed.push_back(entry.U32(list + 4 + std::size_t{k} * 4));
    }

    return listed;
}

// The sequence file of sound `sound` of `archive`.
sar::SequenceFile FileOf(const SoundArchive& archive, uint32_t sound)
{
    return sar::SequenceFile::Read(archive.FileData(archive.Sounds()[sound].file_id));
}

// `data` without the zeros that pad it.
std::vector<uint8_t> Stripped(std::vector<uint8_t> data)
{
    while (!data.empty() && data.back() == 0)
    {
        data.pop_back();
    }

    return data;
}

// The labels of `file`, by name and offset.
std::vector<std::pair<std::string, uint32_t>> LabelsOf(const sar::SequenceFile& file)
{
    std::vector<std::pair<std::string, uint32_t>> labels;
    for (const sar::Sequence::Label& label : file.labels)
    {
        labels.emplace_back(label.name, label.offset);
    }

    return labels;
}

// Where each sound plays in `file`. The program finds the places the same way.
std::vector<std::optional<SoundPlace>> PlacesOf(const File& file)
{
    const std::vector<uint8_t> built = hokuto::format::ExportArchive(file);
    const std::shared_ptr<const SoundArchive> archive = hokuto::song::LoadArchive(built, "test.bfsar");
    return hokuto::edit::SoundPlaces(*archive, hokuto::format::SequencesOfFiles(file, built, *archive),
                                     hokuto::format::StartFieldsOfSounds(file, built));
}

// Whether `call` throws std::invalid_argument: the edit refuses.
template <typename Call>
bool Refused(Call call)
{
    try
    {
        call();
    }
    catch (const std::invalid_argument&)
    {
        return true;
    }

    return false;
}

// The message of the std::invalid_argument that `call` throws, or nothing when it throws none.
template <typename Call>
std::string Message(Call call)
{
    try
    {
        call();
    }
    catch (const std::invalid_argument& error)
    {
        return error.what();
    }

    return {};
}

// The new sound copies the model's values and plays the new file from Main, with the map's program. The new file holds
// the new commands with their labels, in the version of the model's file, and takes the size that NewSequenceFileSize
// gives. A file of the other console gives the same archive. The other items and the file's other parts stay, the
// game's sizes and the tags among them, and the program finds the new sound's sequence.
void TestAdd(Flavor flavor)
{
    File file = hokuto::format::ImportArchive(TestArchive(flavor), flavor);
    file.scenes.push_back({.sound = kSecond, .name = "Loud", .values = {{3, 1}}});
    file.aux_effects[0].push_back({});
    file.aux_volumes[1] = 0.5f;
    file.track_colours[{kFirst, 0}] = {200, 40, 40};
    file.tags = {{"title", "Demo"}};
    file.sound_tags[kSecond] = {{"comment", "Loud"}};
    const SoundArchive before = Load(hokuto::format::ExportArchive(file), flavor);
    const ProgramMap kPrograms = {{{1, 7}, 9}};
    const Flavor other = flavor == Flavor::kCafe ? Flavor::kNx : Flavor::kCafe;
    std::vector<uint8_t> expected = flavor == Flavor::kCafe ? kWiiUNewBytecode : kNewBytecode;
    expected[11] = 9;

    const File added = AddSequenceSound(file, kFirst, "SEQ_NEW", NewSequence(flavor), kMain, kPrograms);
    const File crossed = AddSequenceSound(file, kFirst, "SEQ_NEW", NewSequence(other), kMain, kPrograms);

    const std::vector<uint8_t> built = hokuto::format::ExportArchive(added);
    const SoundArchive after = Load(built, flavor);
    SEQLIB_REQUIRE(after.Sounds().size() == 4);
    const sar::SoundInfo& model = after.Sounds()[kFirst];
    const sar::SoundInfo& sound = after.Sounds()[kAdded];
    const sar::SequenceFile new_file = FileOf(after, kAdded);
    const std::vector<std::optional<SoundPlace>> places = PlacesOf(added);
    XSFLIB_CHECK(sound.name == "SEQ_NEW" && sound.type == sar::SoundType::kSequence);
    XSFLIB_CHECK(sound.file_id == before.FileCount() && after.FileCount() == before.FileCount() + 1);
    XSFLIB_CHECK(sound.start_offset == 8 && sound.allocate_track_flags == 1 && model.allocate_track_flags == 3);
    XSFLIB_CHECK(sound.player_id == model.player_id && sound.volume == 90 && sound.pan_mode == 1 &&
                 sound.pan_curve == 1);
    XSFLIB_CHECK(sound.player_priority == 0x30 && sound.actor_player_id == 2 && sound.channel_priority == 0x50);
    XSFLIB_CHECK(sound.release_priority_fix && sound.banks == model.banks && sound.banks.size() == 2);
    XSFLIB_CHECK(Stripped(new_file.data) == expected && new_file.version == VersionOf(flavor));
    XSFLIB_CHECK((LabelsOf(new_file) == std::vector<std::pair<std::string, uint32_t>>{{"Intro", 0}, {"Main", 8}}));
    const Sequence mapped = hokuto::edit::WithPrograms(NewSequence(flavor), kMain, kPrograms);
    XSFLIB_CHECK(hokuto::edit::NewSequenceFileSize(file, mapped) == after.FileData(sound.file_id).size());
    for (uint32_t i = 0; i < kAdded; ++i)
    {
        XSFLIB_CHECK(after.Sounds()[i].name == before.Sounds()[i].name);
        XSFLIB_CHECK(after.Sounds()[i].file_id == before.Sounds()[i].file_id);
        XSFLIB_CHECK(after.Sounds()[i].start_offset == before.Sounds()[i].start_offset);
    }
    for (uint32_t i = 0; i < before.FileCount(); ++i)
    {
        const std::span<const uint8_t> old_bytes = before.FileData(i);
        const std::span<const uint8_t> new_bytes = after.FileData(i);
        XSFLIB_CHECK(std::equal(old_bytes.begin(), old_bytes.end(), new_bytes.begin(), new_bytes.end()));
    }
    XSFLIB_CHECK(hokuto::format::ExportArchive(crossed) == built);
    XSFLIB_CHECK(added.engine == file.engine && added.scenes == file.scenes && added.aux_effects == file.aux_effects);
    XSFLIB_CHECK(added.aux_volumes == file.aux_volumes && added.track_colours == file.track_colours);
    XSFLIB_CHECK(added.tags == file.tags && added.sound_tags == file.sound_tags);
    XSFLIB_CHECK(file.native->game_sizes && file.native->game_sizes->archive == TestArchive(flavor).size());
    XSFLIB_CHECK(added.native->game_sizes == file.native->game_sizes && built.size() > TestArchive(flavor).size());
    XSFLIB_CHECK(added.sequences.size() == file.sequences.size() + 1);
    for (std::size_t i = 0; i < file.sequences.size(); ++i)
    {
        XSFLIB_CHECK(added.sequences[i].SharesWith(file.sequences[i]));
    }
    SEQLIB_REQUIRE(places.size() == 4 && places[kAdded]);
    XSFLIB_CHECK(places[kAdded]->sequence == file.sequences.size());
    XSFLIB_CHECK(places[kAdded]->start_field.has_value());
}

// A sequence file that only a group file beside the archive holds keeps its sequence's number when a new sequence sound
// comes in. The cut numbers the archive's sequences first, the new file's among them, and then the group file's. The
// group file stays as it was. A sound whose sequence file only the group file holds serves as a model too: the new file
// takes the version of the group file's copy.
void TestGroupSequences()
{
    sar_files::SoundSpec archived;
    archived.name = "SEQ_ARCHIVED";
    archived.start_offset = 0;
    sar_files::SoundSpec grouped;
    grouped.name = "SEQ_GROUPED";
    grouped.file = 1;
    grouped.start_offset = 4;
    sar_files::ArchiveSpec spec;
    spec.sounds = {archived, grouped};
    spec.players = {{"PLAYER_TEST", 1}};
    spec.groups = {{"GROUP_BESIDE", sar::kExternalGroupFile}};
    spec.files = {{.bytes = sar_files::Sequence(Flavor::kNx, kBytecode, {{"Archived", 0}})},
                  {.group_only = true, .groups = {0}}};
    std::vector<uint8_t> grouped_file = sar_files::Sequence(Flavor::kNx, kBytecode, {{"Grouped", 4}});
    grouped_file.at(0x08 + 2) = 0x01;
    const std::vector<uint8_t> group = sar_tools::Group(Flavor::kNx, {.files = {{1, grouped_file}}});
    const File file = hokuto::format::ImportArchive(sar_tools::Archive(Flavor::kNx, spec), Flavor::kNx, {group});

    const File added = AddSequenceSound(file, kFirst, "SEQ_NEW", NewSequence(Flavor::kNx), kMain, {});
    const std::vector<std::optional<SoundPlace>> places = PlacesOf(added);
    XSFLIB_CHECK(file.sequences.size() == 2 && added.sequences.size() == 3);
    if (file.sequences.size() != 2 || added.sequences.size() != 3)
    {
        return;
    }
    XSFLIB_CHECK(added.sequences[0].SharesWith(file.sequences[0]) && added.sequences[1].SharesWith(file.sequences[1]));
    XSFLIB_CHECK(*added.sequences[2] == NewSequence(Flavor::kNx));
    XSFLIB_CHECK(places.size() == 3 && places[0] && places[1] && places[2]);
    if (places.size() == 3 && places[0] && places[1] && places[2])
    {
        XSFLIB_CHECK(places[0]->sequence == 0 && places[1]->sequence == 1 && places[2]->sequence == 2);
        XSFLIB_CHECK((places[1]->start_field &&
                      added.native->fields.at(*places[1]->start_field).to == sar::Place{1, 2, true, true}));
    }
    XSFLIB_CHECK(hokuto::format::ExportGroupFiles(added) == std::vector<std::vector<uint8_t>>{group});

    const File copied = AddSequenceSound(file, 1, "SEQ_COPY", NewSequence(Flavor::kNx), kMain, {});
    const SoundArchive read = Load(hokuto::format::ExportArchive(copied), Flavor::kNx);
    XSFLIB_CHECK(read.Sounds().size() == 3 && read.Sounds()[2].name == "SEQ_COPY");
    if (read.Sounds().size() == 3)
    {
        XSFLIB_CHECK(FileOf(read, 2).version == 0x00010000);
    }
}

// The new sound allocates the tracks that its commands open, whatever tracks the model allocates.
void TestTracks()
{
    const File file = hokuto::format::ImportArchive(TestArchive(Flavor::kNx), Flavor::kNx);
    const std::vector<uint8_t> kTracksBytecode = {
        0xFE, 0x05, 0x00,       // 0: allocate tracks 0 and 2
        0x88, 3,    9,    0, 0, // 3: open track 3 at 9
        0xFF,                   // 8: the end
        60,   100,  24,         // 9: key 60 for 24 ticks
        0xFF,                   // 12: the end
    };
    const Sequence tracks = ReadIncoming(sar_files::Sequence(Flavor::kNx, kTracksBytecode, {{"Tracks", 0}})).sequence;

    const File added = AddSequenceSound(file, kFirst, "SEQ_TRACKS", tracks, 0, {});

    const SoundArchive after = Load(hokuto::format::ExportArchive(added), Flavor::kNx);
    SEQLIB_REQUIRE(after.Sounds().size() > kAdded);
    XSFLIB_CHECK(after.Sounds()[kAdded].allocate_track_flags == 0b1101);
    XSFLIB_CHECK(after.Sounds()[kFirst].allocate_track_flags == 3);
}

// Each item keeps its name, and the tree finds each name. A second new sound goes after the first. A name that the
// archive holds already, for an item of any kind, is refused, and so is an empty name.
void TestNames()
{
    const File file = hokuto::format::ImportArchive(TestArchive(Flavor::kNx), Flavor::kNx);
    const Sequence sequence = NewSequence(Flavor::kNx);

    const File once = AddSequenceSound(file, kFirst, "SEQ_NEW", sequence, kMain, {});
    const File twice = AddSequenceSound(once, kSecond, "SEQ_NEW_2", sequence, 0, {});

    const std::vector<uint8_t> built = hokuto::format::ExportArchive(twice);
    const SoundArchive after = Load(built, Flavor::kNx);
    const std::span<const uint8_t> strings = *sar::TablesOf(built, Flavor::kNx).strings;
    const auto found = [&strings](const std::string& name)
    {
        return sar::FindItem(strings, Flavor::kNx, name);
    };

    std::vector<std::string> names;
    for (const sar::SoundInfo& sound : after.Sounds())
    {
        names.push_back(sound.name);
    }
    SEQLIB_REQUIRE((names == std::vector<std::string>{"SEQ_FIRST", "SEQ_SECOND", "WSD_SOUND", "SEQ_NEW", "SEQ_NEW_2"}));
    XSFLIB_CHECK(after.Banks().size() == 2 && after.Banks()[0].name == "BANK_A" && after.Banks()[1].name == "BANK_B");
    XSFLIB_CHECK(after.Players().size() == 1 && after.Players()[0].name == "PLAYER_TEST");
    XSFLIB_CHECK(found("SEQ_NEW") == sar::ItemId(sar::kSoundItem, 3));
    XSFLIB_CHECK(found("SEQ_NEW_2") == sar::ItemId(sar::kSoundItem, 4));
    XSFLIB_CHECK(found("WSD_SOUND") == sar::ItemId(sar::kSoundItem, 2));
    XSFLIB_CHECK(found("BANK_B") == sar::ItemId(sar::kBankItem, 1));
    XSFLIB_CHECK(found("PLAYER_TEST") == sar::ItemId(sar::kPlayerItem, 0));
    XSFLIB_CHECK(after.Sounds()[4].banks == after.Sounds()[kSecond].banks && after.Sounds()[4].start_offset == 0);
    XSFLIB_CHECK(Refused([&] { AddSequenceSound(once, kFirst, "SEQ_NEW", sequence, 0, {}); }));
    XSFLIB_CHECK(Refused([&] { AddSequenceSound(file, kFirst, "BANK_B", sequence, 0, {}); }));
    XSFLIB_CHECK(Refused([&] { AddSequenceSound(file, kFirst, "", sequence, 0, {}); }));
}

// A model past the sound table or of another kind, a start past the end of the sequence, a file without the archive's
// tables, an archive without names, and a program that the console's bytecode can't hold. The windows find an archive
// without names before the edit (HoldsNames), and give the edit's reason.
void TestRefusals()
{
    const File file = hokuto::format::ImportArchive(TestArchive(Flavor::kNx), Flavor::kNx);
    const Sequence sequence = NewSequence(Flavor::kNx);
    const auto start = static_cast<uint32_t>(sequence.items.size());
    File bare = file;
    bare.native.Edit([](hokuto::format::NativeArchive& native) { native.tables.reset(); });
    const File unnamed = hokuto::format::ImportArchive(TestArchive(Flavor::kNx, false), Flavor::kNx);
    Sequence wide = sequence;
    wide.items[1].value.value = (int64_t{1} << 35) * kOne;

    XSFLIB_CHECK(!Refused([&] { AddSequenceSound(file, kSecond, "SEQ_NEW", sequence, 0, {}); }));
    XSFLIB_CHECK(Refused([&] { AddSequenceSound(file, 7, "SEQ_NEW", sequence, 0, {}); }));
    XSFLIB_CHECK(Refused([&] { AddSequenceSound(file, kWaveSound, "SEQ_NEW", sequence, 0, {}); }));
    XSFLIB_CHECK(Refused([&] { AddSequenceSound(file, kFirst, "SEQ_NEW", sequence, start, {}); }));
    XSFLIB_CHECK(Refused([&] { AddSequenceSound(bare, kFirst, "SEQ_NEW", sequence, 0, {}); }));
    XSFLIB_CHECK(Refused([&] { AddSequenceSound(unnamed, kFirst, "SEQ_NEW", sequence, 0, {}); }));
    XSFLIB_CHECK(Refused([&] { AddSequenceSound(file, kFirst, "SEQ_NEW", wide, 0, {}); }));
    XSFLIB_CHECK(HoldsNames(TestArchive(Flavor::kNx), Flavor::kNx));
    XSFLIB_CHECK(!HoldsNames(TestArchive(Flavor::kNx, false), Flavor::kNx));
    try
    {
        AddSequenceSound(unnamed, kFirst, "SEQ_NEW", sequence, 0, {});
    }
    catch (const std::invalid_argument& error)
    {
        XSFLIB_CHECK(error.what() == NoNamesReason("sound"));
    }
}

// The new sound's bank slots: the model's, or those chosen for the import. More than four slots, a bank past the table
// and an item of another kind are refused.
void TestBankSlots()
{
    const File file = hokuto::format::ImportArchive(TestArchive(Flavor::kNx), Flavor::kNx);
    const Sequence sequence = NewSequence(Flavor::kNx);
    const uint32_t kBankA = sar::ItemId(sar::kBankItem, 0);
    const uint32_t kBankB = sar::ItemId(sar::kBankItem, 1);
    const std::vector<uint32_t> kSlots = {kBankB, sar::kNoItem, kBankA};

    const File chosen = AddSequenceSound(file, kSecond, "SEQ_NEW", sequence, 0, {}, kSlots);

    const SoundArchive after = Load(hokuto::format::ExportArchive(chosen), Flavor::kNx);
    SEQLIB_REQUIRE(after.Sounds().size() > kAdded && chosen.native->tables->sounds.size() > kAdded);
    XSFLIB_CHECK(after.Sounds()[kAdded].banks == kSlots && after.Sounds()[kSecond].banks == std::vector{kBankB});
    XSFLIB_CHECK(chosen.native->tables->sounds[kAdded].banks == kSlots);
    XSFLIB_CHECK(Refused([&] { AddSequenceSound(file, kFirst, "SEQ_NEW", sequence, 0, {}, {{kBankA, 0, 0, 0, 0}}); }));
    XSFLIB_CHECK(Refused([&] { AddSequenceSound(file, kFirst, "SEQ_NEW", sequence, 0, {}, {{kBankA + 2}}); }));
    XSFLIB_CHECK(Refused([&] { AddSequenceSound(file, kFirst, "SEQ_NEW", sequence, 0, {}, {{kNamedArchive}}); }));
}

// The new wave archives: one with a name and one without, each with its file's bytes. The other items keep their names
// and files. The tree finds each name. A name that the archive holds already is refused, and so is a bank file.
void TestNewWaveArchives(Flavor flavor)
{
    const File file = hokuto::format::ImportArchive(BankArchive(flavor), flavor);
    const SoundArchive before = Load(hokuto::format::ExportArchive(file), flavor);
    const std::vector<uint8_t> named = TestWaveArchive(flavor, 4, 40);
    const std::vector<uint8_t> unnamed = TestWaveArchive(flavor, 1, 50);

    const File once = AddWaveArchive(file, "WARC_NEW", named);
    const File twice = AddWaveArchive(once, "", unnamed);

    const std::vector<uint8_t> built = hokuto::format::ExportArchive(twice);
    const SoundArchive after = Load(built, flavor);
    const std::span<const uint8_t> strings = *sar::TablesOf(built, flavor).strings;

    const auto bytes_of = [&after](uint32_t wave_archive)
    {
        const std::span<const uint8_t> data = after.FileData(after.WaveArchives().at(wave_archive).file_id);
        return std::vector<uint8_t>(data.begin(), data.end());
    };

    XSFLIB_CHECK(after.WaveArchives().size() == 4 && after.FileCount() == before.FileCount() + 2);
    XSFLIB_CHECK(after.WaveArchives()[0].name == "WARC_A" && after.WaveArchives()[2].name == "WARC_NEW");
    XSFLIB_CHECK(after.WaveArchives()[1].name.empty() && after.WaveArchives()[3].name.empty());
    XSFLIB_CHECK(bytes_of(2) == named && bytes_of(3) == unnamed);
    XSFLIB_CHECK(after.WaveArchives()[0].file_id == before.WaveArchives()[0].file_id);
    XSFLIB_CHECK(after.Banks().size() == 1 && after.Banks()[0].name == "BANK_A");
    XSFLIB_CHECK(sar::FindItem(strings, flavor, "WARC_NEW") == sar::ItemId(sar::kWaveArchiveItem, 2));
    XSFLIB_CHECK(sar::FindItem(strings, flavor, "WARC_A") == kNamedArchive);
    XSFLIB_CHECK(sar::FindItem(strings, flavor, "PLAYER_TEST") == sar::ItemId(sar::kPlayerItem, 0));
    XSFLIB_CHECK(twice.native->tables->wave_archives.size() == 4);
    XSFLIB_CHECK(twice.sequences.size() == file.sequences.size() && twice.sequences[0].SharesWith(file.sequences[0]));
    XSFLIB_CHECK(Refused([&] { AddWaveArchive(file, "WARC_A", named); }));
    XSFLIB_CHECK(Refused([&] { AddWaveArchive(file, "SEQ_FIRST", named); }));
    XSFLIB_CHECK(Refused([&] { AddWaveArchive(file, "WARC_NEW", TestBank(flavor, {})); }));
}

// The new bank names the wave archives of this archive that the map gives, in its file and nowhere else. Its file
// otherwise holds the bank's bytes: the builder's bank with the new IDs. Its entry lists no wave archives. The
// archive's bank lists none either.
void TestNewBank(Flavor flavor)
{
    const File file = hokuto::format::ImportArchive(BankArchive(flavor), flavor);
    const SoundArchive before = Load(hokuto::format::ExportArchive(file), flavor);
    const std::vector<uint8_t> bank =
        TestBank(flavor, {{kOtherArchive7, 0}, {kOtherArchive3, 1}, {kOtherArchive7, 1}, {kOtherArchive3, 0}});
    const std::vector<uint8_t> expected =
        TestBank(flavor, {{kUnnamedArchive, 0}, {kNamedArchive, 1}, {kUnnamedArchive, 1}, {kNamedArchive, 0}});
    const std::vector<hokuto::edit::NamedWaveArchive> named = hokuto::edit::WaveArchivesNamed(bank, flavor);

    const File added = AddBank(file, "BANK_NEW", bank, {{kOtherArchive7, 1}, {kOtherArchive3, 0}});

    const std::vector<uint8_t> built = hokuto::format::ExportArchive(added);
    const SoundArchive after = Load(built, flavor);
    const std::span<const uint8_t> data = after.FileData(after.Banks().at(1).file_id);
    XSFLIB_CHECK(named.size() == 2 && named[0].id == kOtherArchive7 && named[1].id == kOtherArchive3);
    XSFLIB_CHECK((named[0].waves == std::vector<uint32_t>{0, 1} && named[1].waves == std::vector<uint32_t>{0, 1}));
    XSFLIB_CHECK(after.Banks().size() == 2 && after.Banks()[1].name == "BANK_NEW");
    XSFLIB_CHECK(after.Banks()[1].file_id == before.FileCount() && after.FileCount() == before.FileCount() + 1);
    XSFLIB_CHECK(std::equal(data.begin(), data.end(), expected.begin(), expected.end()));
    XSFLIB_CHECK(ListedWaveArchives(built, flavor, 1).empty() && ListedWaveArchives(built, flavor, 0).empty());
    XSFLIB_CHECK(after.WaveArchives().size() == 2 && after.WaveArchives()[0].name == "WARC_A");
    XSFLIB_CHECK(after.Sounds()[0].banks == before.Sounds()[0].banks);
    XSFLIB_CHECK(sar::FindItem(*sar::TablesOf(built, flavor).strings, flavor, "BANK_NEW") ==
                 sar::ItemId(sar::kBankItem, 1));
    XSFLIB_CHECK(sar::FindItem(*sar::TablesOf(built, flavor).strings, flavor, "WARC_A") == kNamedArchive);
    XSFLIB_CHECK(added.native->tables->banks.size() == 2);
}

// Where another bank's entry lists its wave archives, the new bank's entry lists those that the map gives, in the order
// of their first use in its file, each once.
void TestListedWaveArchives()
{
    const std::vector<uint8_t> archive = BankArchive(Flavor::kCafe);
    const sar::ArchiveWithFile with = sar::WithNewFile(archive, Flavor::kCafe, TestBank(Flavor::kCafe, {}));
    const sar::NewBank listing{.name = "BANK_LISTS", .file = with.file, .wave_archives = {kUnnamedArchive}};
    const File file =
        hokuto::format::ImportArchive(sar::WithNewBank(with.archive, Flavor::kCafe, listing), Flavor::kCafe);
    const std::vector<uint8_t> bank = TestBank(Flavor::kCafe, {{kOtherArchive7, 0}, {kOtherArchive3, 0}});

    const File added = AddBank(file, "BANK_NEW", bank, {{kOtherArchive7, 1}, {kOtherArchive3, 1}});
    const File split = AddBank(file, "BANK_NEW", bank, {{kOtherArchive7, 1}, {kOtherArchive3, 0}});

    const std::vector<uint8_t> built = hokuto::format::ExportArchive(added);
    XSFLIB_CHECK(ListedWaveArchives(built, Flavor::kCafe, 2) == std::vector{kUnnamedArchive});
    XSFLIB_CHECK((ListedWaveArchives(hokuto::format::ExportArchive(split), Flavor::kCafe, 2) ==
                  std::vector{kUnnamedArchive, kNamedArchive}));
    XSFLIB_CHECK(ListedWaveArchives(built, Flavor::kCafe, 1) == std::vector{kUnnamedArchive});
}

// A wave archive that the map leaves out or gives past the list, a name that is empty or that the archive holds
// already, a file of another kind, a file without the archive's tables, and an archive without names. A bank of the Wii
// U or the 3DS goes in as the same bank of the archive's console. A wave archive goes in without a name in an archive
// without names, and can't take one there.
void TestNewBankRefusals()
{
    const File file = hokuto::format::ImportArchive(BankArchive(Flavor::kNx), Flavor::kNx);
    const File unnamed = hokuto::format::ImportArchive(BankArchive(Flavor::kNx, false), Flavor::kNx);
    File bare = file;
    bare.native.Edit([](hokuto::format::NativeArchive& native) { native.tables.reset(); });
    const std::vector<uint8_t> bank = TestBank(Flavor::kNx, {{kOtherArchive3, 0}, {kOtherArchive7, 0}});
    const std::vector<uint8_t> wii_u_bank = TestBank(Flavor::kCafe, {{kOtherArchive3, 0}, {kOtherArchive7, 0}});
    const std::map<uint32_t, uint32_t> kMap = {{kOtherArchive3, 0}, {kOtherArchive7, 1}};
    const std::vector<uint8_t> wave_archive = TestWaveArchive(Flavor::kNx, 1, 60);

    XSFLIB_CHECK(!Refused([&] { AddBank(file, "BANK_NEW", bank, kMap); }));
    XSFLIB_CHECK(Refused([&] { AddBank(file, "BANK_NEW", bank, {{kOtherArchive3, 0}}); }));
    XSFLIB_CHECK(Refused([&] { AddBank(file, "BANK_NEW", bank, {{kOtherArchive3, 0}, {kOtherArchive7, 2}}); }));
    XSFLIB_CHECK(Refused([&] { AddBank(file, "", bank, kMap); }));
    XSFLIB_CHECK(Refused([&] { AddBank(file, "BANK_A", bank, kMap); }));
    XSFLIB_CHECK(Refused([&] { AddBank(file, "WARC_A", bank, kMap); }));
    XSFLIB_CHECK(Refused([&] { AddBank(file, "BANK_NEW", wave_archive, kMap); }));
    XSFLIB_CHECK(hokuto::format::ExportArchive(AddBank(file, "BANK_NEW", wii_u_bank, kMap)) ==
                 hokuto::format::ExportArchive(AddBank(file, "BANK_NEW", bank, kMap)));
    const std::vector<uint8_t> ctr_bank = TestBank(Flavor::kCtr, {{kOtherArchive3, 0}, {kOtherArchive7, 0}});
    XSFLIB_CHECK(hokuto::format::ExportArchive(AddBank(file, "BANK_NEW", ctr_bank, kMap)) ==
                 hokuto::format::ExportArchive(AddBank(file, "BANK_NEW", bank, kMap)));
    XSFLIB_CHECK(Refused([&] { AddBank(bare, "BANK_NEW", bank, kMap); }));
    XSFLIB_CHECK(Refused([&] { AddBank(unnamed, "BANK_NEW", bank, kMap); }));
    XSFLIB_CHECK(!Refused([&] { AddWaveArchive(unnamed, "", wave_archive); }));
    XSFLIB_CHECK(Refused([&] { AddWaveArchive(unnamed, "WARC_NEW", wave_archive); }));
    XSFLIB_CHECK(Refused([&] { AddWaveArchive(bare, "", wave_archive); }));
}

// A bank or a wave sound file for Replace… names the wave archives that the map gives, and keeps its other bytes: the
// builder's file with the new IDs. A map without a wave archive that the file names, a place past the list and a file
// without the archive's tables are refused. The file's magic gives its kind in the message: "wave sound file".
void TestWaveArchiveMap(Flavor flavor)
{
    using hokuto::edit::WithWaveArchiveMap;
    const File file = hokuto::format::ImportArchive(BankArchive(flavor), flavor);
    File bare = file;
    bare.native.Edit([](hokuto::format::NativeArchive& native) { native.tables.reset(); });
    const std::vector<uint8_t> bank = TestBank(flavor, {{kOtherArchive7, 0}, {kOtherArchive3, 1}});
    const std::vector<uint8_t> wave_sounds = TestWaveSounds(flavor, {{kOtherArchive3, 1}, {kOtherArchive7, 0}}, {1, 0});
    const std::map<uint32_t, uint32_t> kMap = {{kOtherArchive7, 1}, {kOtherArchive3, 0}};

    XSFLIB_CHECK(WithWaveArchiveMap(file, bank, kMap) == TestBank(flavor, {{kUnnamedArchive, 0}, {kNamedArchive, 1}}));
    XSFLIB_CHECK(WithWaveArchiveMap(file, wave_sounds, kMap) ==
                 TestWaveSounds(flavor, {{kNamedArchive, 1}, {kUnnamedArchive, 0}}, {1, 0}));
    XSFLIB_CHECK(WithWaveArchiveMap(file, bank, {{kOtherArchive7, 0}, {kOtherArchive3, 1}}) ==
                 TestBank(flavor, {{kNamedArchive, 0}, {kUnnamedArchive, 1}}));
    XSFLIB_CHECK(Refused([&] { WithWaveArchiveMap(file, bank, {{kOtherArchive7, 1}}); }));
    XSFLIB_CHECK(Refused([&] { WithWaveArchiveMap(file, bank, {{kOtherArchive7, 1}, {kOtherArchive3, 2}}); }));
    XSFLIB_CHECK(Refused([&] { WithWaveArchiveMap(bare, bank, kMap); }));
    XSFLIB_CHECK(Message([&] { WithWaveArchiveMap(file, wave_sounds, {}); }).find("wave sound file") !=
                 std::string::npos);
}

// The names of the new sounds: the name for one wave sound. For more, each name takes the wave sound's index in as many
// digits as the count has.
void TestWaveSoundNames()
{
    using hokuto::edit::WaveSoundNames;
    XSFLIB_CHECK(WaveSoundNames("SE", 1) == std::vector<std::string>{"SE"});
    XSFLIB_CHECK((WaveSoundNames("SE", 3) == std::vector<std::string>{"SE_0", "SE_1", "SE_2"}));
    const std::vector<std::string> twelve = WaveSoundNames("SE", 12);
    XSFLIB_CHECK(twelve.size() == 12 && twelve.front() == "SE_00" && twelve[9] == "SE_09" && twelve.back() == "SE_11");
    XSFLIB_CHECK(WaveSoundNames("SE", 10).back() == "SE_09");
}

// The new wave sounds of a file of three: each copies the sequence sound's settings, plays its wave sound of the new
// file, and takes its name. The new file names the wave archives of this archive that the map gives, and holds the
// file's bytes otherwise. The first new sound's volume then changes, and a second file of one wave sound goes in with a
// copy of that sound's settings.
void TestNewWaveSounds(Flavor flavor)
{
    const File file = hokuto::format::ImportArchive(BankArchive(flavor), flavor);
    const SoundArchive before = Load(hokuto::format::ExportArchive(file), flavor);
    const std::vector<uint8_t> wave_sounds =
        TestWaveSounds(flavor, {{kOtherArchive3, 1}, {kOtherArchive7, 0}, {kOtherArchive3, 0}}, {2, 0, 1});
    const std::vector<uint8_t> expected =
        TestWaveSounds(flavor, {{kNamedArchive, 1}, {kUnnamedArchive, 0}, {kNamedArchive, 0}}, {2, 0, 1});
    const std::vector<hokuto::edit::NamedWaveArchive> named = hokuto::edit::WaveArchivesNamed(wave_sounds, flavor);
    const std::vector<uint8_t> one = TestWaveSounds(flavor, {{kOtherArchive7, 2}}, {0});

    File added = AddWaveSounds(file, 0, "WSD_NEW", wave_sounds, {{kOtherArchive3, 0}, {kOtherArchive7, 1}});
    added.native.Edit([](hokuto::format::NativeArchive& native) { native.tables->sounds[1].volume = 37; });
    const File again = AddWaveSounds(added, 1, "WSD_ONE", one, {{kOtherArchive7, 1}});

    // The sounds that ntdlib reads: the copies of the sequence sound, then the copy of the first new sound.
    const std::vector<uint8_t> built = hokuto::format::ExportArchive(again);
    const SoundArchive after = Load(built, flavor);
    XSFLIB_CHECK(named.size() == 2 && named[0].id == kOtherArchive3 && named[1].id == kOtherArchive7);
    XSFLIB_CHECK((named.size() == 2 && named[0].waves == std::vector<uint32_t>{0, 1}));
    XSFLIB_CHECK(after.Sounds().size() == 5 && after.FileCount() == before.FileCount() + 2);
    const std::vector<std::string> kNames = {"SEQ_FIRST", "WSD_NEW_0", "WSD_NEW_1", "WSD_NEW_2", "WSD_ONE"};
    for (uint32_t i = 1; i < 5 && after.Sounds().size() == 5; ++i)
    {
        const sar::SoundInfo& sound = after.Sounds()[i];
        XSFLIB_CHECK(sound.name == kNames[i] && sound.type == sar::SoundType::kWaveSound);
        XSFLIB_CHECK(sound.volume == (i == 1 || i == 4 ? 37 : before.Sounds()[0].volume));
        XSFLIB_CHECK(sound.player_id == before.Sounds()[0].player_id);
        XSFLIB_CHECK(sound.file_id == before.FileCount() + (i == 4 ? 1 : 0));
        XSFLIB_CHECK(sound.wave_sound_index == (i == 4 ? 0 : i - 1));
        XSFLIB_CHECK(sar::FindItem(*sar::TablesOf(built, flavor).strings, flavor, kNames[i]) ==
                     sar::ItemId(sar::kSoundItem, i));
    }
    const std::span<const uint8_t> data = after.FileData(static_cast<uint32_t>(before.FileCount()));
    XSFLIB_CHECK(std::equal(data.begin(), data.end(), expected.begin(), expected.end()));
    XSFLIB_CHECK(after.Banks().size() == 1 && after.Banks()[0].name == "BANK_A");
    XSFLIB_CHECK(after.WaveArchives().size() == 2 && after.WaveArchives()[0].name == "WARC_A");
    XSFLIB_CHECK(again.native->tables->sounds.size() == 5);
    XSFLIB_CHECK(again.sequences.size() == file.sequences.size() && again.sequences[0].SharesWith(file.sequences[0]));

    // The copy of a wave sound keeps its model's entry, but for the file, the index and the name.
    const sar::InfoBlock info = sar::InfoBlock::Read(sar::TablesOf(built, flavor).info, flavor);
    const xsflib::Reader copy(info.sounds.at(4), flavor == Flavor::kCafe);
    const xsflib::Reader model(info.sounds.at(1), flavor == Flavor::kCafe);
    XSFLIB_CHECK(info.sounds[4].size() == info.sounds[1].size() && copy.U32(28) == model.U32(28));
}

// A model past the table, a name that is empty or that the archive holds already, a wave archive that the map leaves
// out or gives past the list, a file of another kind or without wave sounds, a file without the archive's tables, and
// an archive without names. A wave sound file of the Wii U or the 3DS goes in as the same file of the archive's
// console.
void TestNewWaveSoundRefusals()
{
    const File file = hokuto::format::ImportArchive(BankArchive(Flavor::kNx), Flavor::kNx);
    const File unnamed = hokuto::format::ImportArchive(BankArchive(Flavor::kNx, false), Flavor::kNx);
    File bare = file;
    bare.native.Edit([](hokuto::format::NativeArchive& native) { native.tables.reset(); });
    const std::vector<std::pair<uint32_t, uint32_t>> kWaves = {{kOtherArchive3, 0}, {kOtherArchive7, 0}};
    const std::vector<uint8_t> wave_sounds = TestWaveSounds(Flavor::kNx, kWaves, {0, 1});
    const std::vector<uint8_t> wii_u = TestWaveSounds(Flavor::kCafe, kWaves, {0, 1});
    const std::vector<uint8_t> ctr = TestWaveSounds(Flavor::kCtr, kWaves, {0, 1});
    const std::map<uint32_t, uint32_t> kMap = {{kOtherArchive3, 0}, {kOtherArchive7, 1}};
    const File added = AddWaveSounds(file, 0, "WSD_NEW", wave_sounds, kMap);

    XSFLIB_CHECK(Refused([&] { AddWaveSounds(file, 1, "WSD_NEW", wave_sounds, kMap); }));
    XSFLIB_CHECK(Refused([&] { AddWaveSounds(file, 0, "", wave_sounds, kMap); }));
    XSFLIB_CHECK(Refused([&] { AddWaveSounds(file, 0, "SEQ_FIRST", TestWaveSounds(Flavor::kNx, kWaves, {0}), kMap); }));
    XSFLIB_CHECK(Refused([&] { AddWaveSounds(added, 0, "WSD_NEW", wave_sounds, kMap); }));
    XSFLIB_CHECK(!Refused([&] { AddWaveSounds(added, 1, "WSD_NEW", TestWaveSounds(Flavor::kNx, kWaves, {0}), kMap); }));
    XSFLIB_CHECK(Refused([&] { AddWaveSounds(file, 0, "WSD_NEW", wave_sounds, {{kOtherArchive3, 0}}); }));
    XSFLIB_CHECK(
        Refused([&] { AddWaveSounds(file, 0, "WSD_NEW", wave_sounds, {{kOtherArchive3, 0}, {kOtherArchive7, 2}}); }));
    XSFLIB_CHECK(Refused([&] { AddWaveSounds(file, 0, "WSD_NEW", TestBank(Flavor::kNx, {}), kMap); }));
    XSFLIB_CHECK(Refused([&] { AddWaveSounds(file, 0, "WSD_NEW", TestWaveSounds(Flavor::kNx, {}, {}), {}); }));
    XSFLIB_CHECK(Refused([&] { AddWaveSounds(bare, 0, "WSD_NEW", wave_sounds, kMap); }));
    XSFLIB_CHECK(Refused([&] { AddWaveSounds(unnamed, 0, "WSD_NEW", wave_sounds, kMap); }));
    XSFLIB_CHECK(hokuto::format::ExportArchive(AddWaveSounds(file, 0, "WSD_NEW", wii_u, kMap)) ==
                 hokuto::format::ExportArchive(added));
    XSFLIB_CHECK(hokuto::format::ExportArchive(AddWaveSounds(file, 0, "WSD_NEW", ctr, kMap)) ==
                 hokuto::format::ExportArchive(added));
}

// `bytes` with zeros up to a multiple of 32 bytes. The Wii's groups hold their files that way.
std::vector<uint8_t> PaddedForWii(std::vector<uint8_t> bytes)
{
    bytes.resize((bytes.size() + 31) & ~std::size_t{31}, 0);

    return bytes;
}

// A Wii archive as NW4R's tools lay one out: two songs, a bank of version 1.2 with an RWAR of two waves, and a wave
// sound file of version 1.3 with an RWAR of one wave. GROUP_A holds the first song, the bank and the wave sound file,
// and GROUP_B the bank and the second song.
std::vector<uint8_t> WiiArchive()
{
    namespace rsar = ntdlib::rsar;
    namespace rsar_files = ntdlib::testing::rsar_files;
    rsar::SequenceFile first;
    first.data = {0xFF};
    first.labels = {{"SEQ_SONG", 0}};
    rsar::SequenceFile second;
    second.data = {0xFE, 0x01, 0x00, 0xFF};
    second.labels = {{"SEQ_TWO", 0}};
    rsar_files::WaveSoundSpec spec;
    spec.notes.push_back({});
    rsar::ArchiveModel model;
    model.players = {{"PLAYER_TEST", 1, 0}};
    model.sounds.resize(3);
    model.sounds[0] = {.name = "SEQ_SONG", .volume = 100, .channel_priority = 70};
    model.sounds[1] = {.name = "WSD_ONE", .type = rsar::SoundType::kWaveSound, .file_id = 2, .volume = 90};
    model.sounds[2] = {.name = "SEQ_TWO", .file_id = 3};
    model.banks = {{"BANK_TEST", 1}};
    model.groups = {{"GROUP_A", {0, 1, 2}}, {"GROUP_B", {1, 3}}};
    model.files = {{PaddedForWii(first.Write())},
                   {PaddedForWii(rsar_files::MakeNewBank()), PaddedForWii(rsar_files::MakeRwar())},
                   {PaddedForWii(rsar_files::MakeWaveSoundFile(0x0103, {spec})),
                    PaddedForWii(rsar_files::MakeRwarOf({rsar_files::MakeRwav(0x33)}))},
                   {PaddedForWii(second.Write())}};

    return rsar::BuildArchive(model);
}

// The model of the archive that `file` builds.
ntdlib::rsar::ArchiveModel WiiModelOf(const File& file)
{
    return ntdlib::rsar::ReadArchiveModel(hokuto::format::ExportArchive(file));
}

// A new sequence sound, bank and wave sounds in the Wii's archive. Each goes in through the model, and the archive's
// other items keep their entries and their files. A new file goes into the chosen groups, or else into the last group.
// A group that loads the model takes only what the model doesn't require. The file's sequences keep their numbers when
// a new file comes before one of them. A file in the model form stays in it, and the game's sizes stay.
void TestWiiItems()
{
    namespace rsar = ntdlib::rsar;
    namespace rsar_files = ntdlib::testing::rsar_files;
    const File file = hokuto::format::ImportWiiArchive(WiiArchive());
    const rsar::ArchiveModel before = WiiModelOf(file);
    const std::vector<uint8_t> song = before.files[0].bytes;
    const Sequence incoming =
        ReadIncoming(song, hokuto::song::Console{.wii = hokuto::format::BuildOfWiiArchive(*file.native)}).sequence;

    // The tools laid the archive out, and it takes new items. The Import window refuses a file without a Wii archive.
    XSFLIB_CHECK(!hokuto::edit::WiiAdditionsRefused(file));
    XSFLIB_CHECK(hokuto::edit::WiiAdditionsRefused(File()) == "The file holds no archive of the Wii.");

    // The groups, without the last.
    const std::vector<hokuto::edit::Group> groups = hokuto::edit::WiiGroupsOf(file);
    XSFLIB_CHECK(groups.size() == 2 && groups[0].name == "GROUP_A" && (groups[1].files == std::vector<uint32_t>{1, 3}));

    // A copy of SEQ_SONG in GROUP_A, before SEQ_TWO's file. GROUP_A loads the model, and takes only the new file. The
    // new file takes the size that NewSequenceFileSize gives. The second song keeps its number.
    const std::vector<uint32_t> kGroupA = {0};
    const File added = hokuto::edit::AddWiiSequenceSound(file, 0, "SEQ_NEW", incoming, 0, {}, std::nullopt, kGroupA);
    const rsar::ArchiveModel with_sound = WiiModelOf(added);
    rsar::SoundModel expected = before.sounds[0];
    expected.name = "SEQ_NEW";
    expected.file_id = 4;
    SEQLIB_REQUIRE(with_sound.files.size() > 4 && with_sound.groups.size() > 1);
    XSFLIB_CHECK(with_sound.sounds.size() == 4 && with_sound.sounds[3] == expected);
    XSFLIB_CHECK(std::equal(before.sounds.begin(), before.sounds.end(), with_sound.sounds.begin()));
    XSFLIB_CHECK(std::equal(before.files.begin(), before.files.end(), with_sound.files.begin()));
    XSFLIB_CHECK((with_sound.groups[0].file_ids == std::vector<uint32_t>{0, 1, 2, 4}));
    XSFLIB_CHECK(hokuto::edit::NewSequenceFileSize(file, incoming) == with_sound.files[4].bytes.size());
    XSFLIB_CHECK(with_sound.groups[1] == before.groups[1]);
    XSFLIB_CHECK(added.sequences.size() == 3 && added.sequences[0].SharesWith(file.sequences[0]) &&
                 added.sequences[1].SharesWith(file.sequences[1]) && *added.sequences[2] == *file.sequences[0]);
    XSFLIB_CHECK(hokuto::format::Read(hokuto::format::Write(added)) == added);
    XSFLIB_CHECK(added.native->game_sizes == file.native->game_sizes);

    // GROUP_B doesn't load the model and holds its bank. It takes the new file alone. Without groups, the last group
    // holds it.
    const std::vector<uint32_t> kGroupB = {1};
    const rsar::ArchiveModel in_b =
        WiiModelOf(hokuto::edit::AddWiiSequenceSound(file, 0, "SEQ_NEW", incoming, 0, {}, std::nullopt, kGroupB));
    SEQLIB_REQUIRE(in_b.groups.size() > 1);
    XSFLIB_CHECK((in_b.groups[1].file_ids == std::vector<uint32_t>{1, 3, 4}) && in_b.groups[0] == before.groups[0]);
    const rsar::ArchiveModel alone = WiiModelOf(hokuto::edit::AddWiiSequenceSound(file, 0, "SEQ_NEW", incoming, 0, {}));
    XSFLIB_CHECK(alone.groups == before.groups && alone.files.size() == 5);

    // Without a model, Hokuto's settings and the first bank. The refusals.
    const rsar::ArchiveModel plain =
        WiiModelOf(hokuto::edit::AddWiiSequenceSound(file, std::nullopt, "SEQ_NEW", incoming, 0, {}));
    SEQLIB_REQUIRE(plain.sounds.size() == 4);
    XSFLIB_CHECK(plain.sounds[3].volume == 127 && plain.sounds[3].bank == 0 && plain.sounds[3].player_id == 0);
    XSFLIB_CHECK(Message([&] { hokuto::edit::AddWiiSequenceSound(file, 0, "SEQ_TWO", incoming, 0, {}); }) ==
                 "The archive holds the name SEQ_TWO already.");
    XSFLIB_CHECK(Message([&] { hokuto::edit::AddWiiSequenceSound(file, 1, "SEQ_NEW", incoming, 0, {}); }) ==
                 "The sound to copy isn't a sequence sound.");
    XSFLIB_CHECK(Message(
                     [&]
                     {
                         hokuto::edit::AddWiiSequenceSound(file, 0, "SEQ_NEW", incoming, 0, {},
                                                           std::vector<uint32_t>{sar::kNoItem});
                     }) == "A sequence sound of the Wii plays a bank. Choose one for its slot.");
    XSFLIB_CHECK(Refused(
        [&]
        {
            hokuto::edit::AddWiiSequenceSound(file, 0, "SEQ_NEW", incoming, 0, {},
                                              std::vector<uint32_t>{sar::ItemId(sar::kBankItem, 1)});
        }));
    const std::vector<uint32_t> kPast = {2};
    XSFLIB_CHECK(Refused([&] { hokuto::edit::AddWiiSequenceSound(file, 0, "SEQ_NEW", incoming, 0, {}, {}, kPast); }));

    // A bank with its RWAR, and a bank whose wave data doesn't hold its waves.
    const File with_bank =
        hokuto::edit::AddWiiBank(file, "BANK_NEW", rsar_files::MakeNewBank(), rsar_files::MakeRwar());
    const rsar::ArchiveModel banked = WiiModelOf(with_bank);
    XSFLIB_CHECK(banked.banks.size() == 2 && banked.banks[1].name == "BANK_NEW" && banked.banks[1].file_id == 4);
    SEQLIB_REQUIRE(banked.files.size() > 4);
    XSFLIB_CHECK(banked.files[4].bytes == before.files[1].bytes &&
                 banked.files[4].wave_data == before.files[1].wave_data);
    XSFLIB_CHECK(banked.groups == before.groups && banked.sounds == before.sounds);
    XSFLIB_CHECK(Refused([&] { hokuto::edit::AddWiiBank(file, "BANK_NEW", rsar_files::MakeNewBank(), {}); }));
    XSFLIB_CHECK(Refused(
        [&] { hokuto::edit::AddWiiBank(file, "SEQ_SONG", rsar_files::MakeNewBank(), rsar_files::MakeRwar()); }));

    // Wave sounds that copy WSD_ONE and SEQ_SONG. GROUP_A loads WSD_ONE and takes the new file.
    const std::vector<uint8_t> wave_sounds = before.files[2].bytes;
    const std::vector<uint8_t> wave_data = before.files[2].wave_data;
    const rsar::ArchiveModel sounded =
        WiiModelOf(hokuto::edit::AddWiiWaveSounds(file, 1, "WSD_NEW", wave_sounds, wave_data, kGroupA));
    rsar::SoundModel wave_sound = before.sounds[1];
    wave_sound.name = "WSD_NEW";
    wave_sound.file_id = 4;
    XSFLIB_CHECK(sounded.sounds.size() == 4 && sounded.sounds[3] == wave_sound);
    SEQLIB_REQUIRE(!sounded.groups.empty());
    XSFLIB_CHECK((sounded.groups[0].file_ids == std::vector<uint32_t>{0, 1, 2, 4}));
    const rsar::ArchiveModel from_sequence =
        WiiModelOf(hokuto::edit::AddWiiWaveSounds(file, 0, "WSD_NEW", wave_sounds, wave_data));
    SEQLIB_REQUIRE(from_sequence.sounds.size() == 4);
    const rsar::SoundModel& copied = from_sequence.sounds[3];
    XSFLIB_CHECK(copied.type == rsar::SoundType::kWaveSound && copied.volume == 100 && copied.channel_priority == 70 &&
                 copied.wave_sound_index == 0 && copied.allocate_track_count == 1);
    XSFLIB_CHECK(Refused([&] { hokuto::edit::AddWiiWaveSounds(file, 0, "WSD_ONE", wave_sounds, wave_data); }));
    XSFLIB_CHECK(
        Refused([&] { hokuto::edit::AddWiiWaveSounds(file, 0, "WSD_NEW", rsar_files::MakeNewBank(), wave_data); }));

    // A file in the model form stays in it. A file of another console takes no new item.
    const File modelled = hokuto::format::InModelForm(file);
    const File modelled_bank =
        hokuto::edit::AddWiiBank(modelled, "BANK_NEW", rsar_files::MakeNewBank(), rsar_files::MakeRwar());
    XSFLIB_CHECK(modelled_bank.form == hokuto::format::ArchiveForm::kModel &&
                 WiiModelOf(modelled_bank).banks.size() == 2);
    const File later = hokuto::format::ImportArchive(BankArchive(Flavor::kNx), Flavor::kNx);
    XSFLIB_CHECK(
        Message([&]
                { hokuto::edit::AddWiiBank(later, "BANK_NEW", rsar_files::MakeNewBank(), rsar_files::MakeRwar()); }) ==
        "The file holds no archive of the Wii.");
}

} // namespace

int main()
{
    TestAdd(Flavor::kNx);
    TestAdd(Flavor::kCafe);
    TestTracks();
    TestGroupSequences();
    TestNames();
    TestRefusals();
    TestBankSlots();
    TestNewWaveArchives(Flavor::kNx);
    TestNewWaveArchives(Flavor::kCafe);
    TestNewBank(Flavor::kNx);
    TestNewBank(Flavor::kCafe);
    TestListedWaveArchives();
    TestNewBankRefusals();
    TestWaveArchiveMap(Flavor::kNx);
    TestWaveArchiveMap(Flavor::kCafe);
    TestWaveSoundNames();
    TestNewWaveSounds(Flavor::kNx);
    TestNewWaveSounds(Flavor::kCafe);
    TestNewWaveSoundRefusals();
    TestWiiItems();

    return xsflib::testing::Summary("new_items");
}
