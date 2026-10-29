// SPDX-License-Identifier: MIT

// Tests the groups of a file's archive (src/edit/groups.h). A small archive of the Switch and of the Wii U, laid out as
// NintendoWare's tools lay it out, holds two sequence sounds, a wave sound, two banks, two wave archives and four
// groups. SEQ_A plays BANK_A. BANK_A's waves lie in WARC_A. SEQ_B plays BANK_B. BANK_B's waves name no wave archive,
// but its entry lists WARC_B. The wave sound's file names WARC_A, and only the group file beside the archive holds it.
// GROUP_A links to the files of SEQ_A and lists the sound. GROUP_COMMON holds copies of BANK_B's files. It lists the
// bank, and SEQ_B with its bank alone. GROUP_B holds a copy of SEQ_B's sequence file alone, and lists the sound with
// its sequence alone. GROUP_BESIDE is the group file beside the archive: it holds copies of the wave sound's files and
// lists the sound. The model form keeps the groups too. New groups go after GROUP_BESIDE.

#include "edit/groups.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sar/groups.h"
#include "ntdlib/sar/info_block.h"
#include "ntdlib/sar/string_block.h"
#include "ntdlib/sar/tables.h"
#include "ntdlib/testing/sar_files.h"
#include "ntdlib/testing/sar_tools.h"
#include "xsflib/binary.h"
#include "xsflib/testing/check.h"

#include "edit/new_items.h"
#include "edit/replace.h"
#include "edit/tables.h"
#include "format/archive.h"
#include "format/file.h"
#include "format/model.h"
#include "format/text.h"
#include "song/archive.h"
#include "song/items.h"

using hokuto::edit::FilesOfItem;
using hokuto::edit::Group;
using hokuto::edit::GroupOption;
using hokuto::edit::GroupOptions;
using hokuto::edit::GroupsOf;
using hokuto::edit::JoinGroups;
using hokuto::edit::UnreadableFiles;
using hokuto::format::File;
using ntdlib::sar::Flavor;
using ntdlib::sar::SoundArchive;

namespace
{

namespace sar = ntdlib::sar;
namespace sar_files = ntdlib::testing::sar_files;
namespace sar_tools = ntdlib::testing::sar_tools;

// A group's files, each with whether it's a copy, and its items with their load flags.
using Members = std::vector<std::pair<uint32_t, bool>>;
using Listed = std::vector<std::pair<uint32_t, uint32_t>>;

// The files that only group files beside the archive hold, with the names of their groups (FilesInMissingGroups).
using Missing = std::map<uint32_t, std::vector<std::string>>;

// The archive's items, by their item IDs.
const uint32_t kSeqA = sar::ItemId(sar::kSoundItem, 0);
const uint32_t kSeqB = sar::ItemId(sar::kSoundItem, 1);
const uint32_t kWaveSound = sar::ItemId(sar::kSoundItem, 2);
const uint32_t kNewSound = sar::ItemId(sar::kSoundItem, 3);
const uint32_t kBankA = sar::ItemId(sar::kBankItem, 0);
const uint32_t kBankB = sar::ItemId(sar::kBankItem, 1);
const uint32_t kWarcA = sar::ItemId(sar::kWaveArchiveItem, 0);
const uint32_t kWarcB = sar::ItemId(sar::kWaveArchiveItem, 1);

// The archive's files, by their IDs, and the ID of the file that an import adds.
constexpr uint32_t kSeqAFile = 0;
constexpr uint32_t kSeqBFile = 1;
constexpr uint32_t kWaveSoundFile = 2;
constexpr uint32_t kBankAFile = 3;
constexpr uint32_t kBankBFile = 4;
constexpr uint32_t kWarcAFile = 5;
constexpr uint32_t kWarcBFile = 6;
constexpr uint32_t kNewFile = 10;

// The groups, by their places in the archive's list. A new group goes after GROUP_BESIDE.
constexpr uint32_t kGroupA = 0;
constexpr uint32_t kGroupCommon = 1;
constexpr uint32_t kGroupB = 2;
constexpr uint32_t kGroupBeside = 3;
constexpr uint32_t kGroupNew = 4;

// The load flags of a group's items: every part of an item, its sequence alone, or its banks alone.
constexpr uint32_t kAll = 0xFFFFFFFF;
constexpr uint32_t kSequenceAlone = 1;
constexpr uint32_t kBanksAlone = 4;

const std::vector<uint8_t> kBytecode = {
    0xB0, 48,      // 0: timebase 48
    60,   100, 24, // 2: key 60 for 24 ticks
    0xFF,          // 5: the end
};

// A wave archive with a wave of 16-bit PCM whose 16 samples hold `value` in each byte.
std::vector<uint8_t> TestWaveArchive(Flavor flavor, uint8_t value)
{
    sar_files::WaveSpec wave;
    wave.encoding = 1;
    wave.channels = {std::vector<uint8_t>(32, value)};

    return sar_files::WaveArchive(flavor, {sar_files::Wave(flavor, wave)});
}

// A bank whose instrument at program 0 plays wave 0 of `wave_archive`, or a bank without waves.
std::vector<uint8_t> TestBank(Flavor flavor, std::optional<uint32_t> wave_archive)
{
    if (!wave_archive)
    {
        return sar_files::Bank(flavor, {});
    }

    sar_files::Region region;
    region.original_key = 60;
    sar_files::VelocityTable velocities;
    velocities.entries = {{0, region}};
    sar_files::KeyTable keys;
    keys.entries = {{0, velocities}};
    sar_files::BankSpec bank;
    bank.waves = {{*wave_archive, 0}};
    bank.instruments = {keys};

    return sar_files::Bank(flavor, bank);
}

// A wave sound file with `count` wave sounds that play wave 0 of `wave_archive`.
std::vector<uint8_t> TestWaveSounds(Flavor flavor, uint32_t wave_archive, std::size_t count = 1)
{
    sar_files::WaveSoundFileSpec spec;
    spec.waves = {{wave_archive, 0}};
    sar_files::WaveSoundSpec sound;
    sound.tracks = {{{0.0f, 1.0f, 0}}};
    sound.notes = {sar_files::Region()};
    spec.sounds.assign(count, sound);

    return sar_files::WaveSoundFile(flavor, spec);
}

// `archive` with WARC_B in the list of wave archives of BANK_B's entry. The list ends the entry.
std::vector<uint8_t> WithListedWaveArchive(const std::vector<uint8_t>& archive, Flavor flavor)
{
    const sar::Tables tables = sar::TablesOf(archive, flavor);
    sar::InfoBlock info = sar::InfoBlock::Read(tables.info, flavor);
    std::optional<sar::StringBlock> strings;
    if (tables.strings)
    {
        strings = sar::StringBlock::Read(*tables.strings, flavor);
    }

    sar_files::Bytes entry(flavor == Flavor::kCafe);
    entry.data = info.banks[1];
    const std::size_t list = xsflib::Reader(entry.data, flavor == Flavor::kCafe).U32(8);
    entry.U32(list, 1);
    entry.U32(entry.Add(4), kWarcB);
    info.banks[1] = entry.data;

    return sar::WithTables(archive, flavor, strings, info);
}

// The archive, and the group file beside it.
std::pair<std::vector<uint8_t>, std::vector<uint8_t>> TestArchive(Flavor flavor)
{
    sar_files::SoundSpec seq_a;
    seq_a.name = "SEQ_A";
    seq_a.file = kSeqAFile;
    seq_a.banks = {0};
    seq_a.start_offset = 0;
    sar_files::SoundSpec seq_b = seq_a;
    seq_b.name = "SEQ_B";
    seq_b.file = kSeqBFile;
    seq_b.banks = {1};
    sar_files::SoundSpec wave_sound;
    wave_sound.name = "WSD_A";
    wave_sound.kind = sar_files::SoundSpec::kWaveSound;
    wave_sound.file = kWaveSoundFile;

    const std::vector<uint8_t> sequence_a = sar_files::Sequence(flavor, kBytecode, {{"SEQ_A", 0}});
    const std::vector<uint8_t> sequence_b = sar_files::Sequence(flavor, kBytecode, {{"SEQ_B", 0}});
    const std::vector<uint8_t> wave_sounds = TestWaveSounds(flavor, kWarcA);
    const std::vector<uint8_t> bank_b = TestBank(flavor, std::nullopt);
    const std::vector<uint8_t> warc_a = TestWaveArchive(flavor, 10);
    const std::vector<uint8_t> warc_b = TestWaveArchive(flavor, 20);

    const auto group_in_archive = [flavor](const sar_tools::GroupSpec& group)
    {
        return sar_tools::GroupInArchive(flavor, group);
    };

    sar_files::ArchiveSpec spec;
    spec.sounds = {seq_a, seq_b, wave_sound};
    spec.banks = {{"BANK_A", kBankAFile}, {"BANK_B", kBankBFile}};
    spec.wave_archives = {{"WARC_A", kWarcAFile}, {"WARC_B", kWarcBFile}};
    spec.groups = {{"GROUP_A", 7}, {"GROUP_COMMON", 8}, {"GROUP_B", 9}, {"GROUP_BESIDE", sar::kExternalGroupFile}};
    spec.players = {{"PLAYER_TEST", 1}};
    spec.files = {
        {.bytes = sequence_a, .groups = {kGroupA}},
        {.bytes = sequence_b, .groups = {kGroupB}},
        {.group_only = true, .groups = {kGroupBeside}},
        {.bytes = TestBank(flavor, kWarcA), .groups = {kGroupA}},
        {.bytes = bank_b, .groups = {kGroupCommon}},
        {.bytes = warc_a, .groups = {kGroupBeside}},
        {.bytes = warc_b, .groups = {kGroupCommon}},
        {.bytes = group_in_archive(
             {.files = {{kSeqAFile, std::nullopt}, {kBankAFile, std::nullopt}, {kWarcAFile, std::nullopt}},
              .items = {{kSeqA, kAll}}})},
        {.bytes = group_in_archive(
             {.files = {{kBankBFile, bank_b}, {kWarcBFile, warc_b}}, .items = {{kBankB, kAll}, {kSeqB, kBanksAlone}}})},
        {.bytes = group_in_archive({.files = {{kSeqBFile, sequence_b}}, .items = {{kSeqB, kSequenceAlone}}})},
    };
    spec.names = true;
    const std::vector<uint8_t> beside = sar_tools::Group(
        flavor, {.files = {{kWaveSoundFile, wave_sounds}, {kWarcAFile, warc_a}}, .items = {{kWaveSound, kAll}}});
    return {WithListedWaveArchive(sar_tools::Archive(flavor, spec), flavor), beside};
}

// The file of `flavor`'s archive, with the group file beside it unless `beside` is false.
File TestFile(Flavor flavor, bool beside = true)
{
    auto [archive, group] = TestArchive(flavor);
    std::vector<std::vector<uint8_t>> group_files;
    if (beside)
    {
        group_files.push_back(std::move(group));
    }

    return hokuto::format::ImportArchive(archive, flavor, std::move(group_files));
}

// The archive that `file` builds, with its group files.
std::shared_ptr<const SoundArchive> ArchiveOf(const File& file)
{
    return hokuto::song::LoadArchive(hokuto::format::ExportArchive(file), "test.bfsar",
                                     hokuto::format::ExportGroupFiles(file));
}

// What ntdlib reads of the archive that `file` builds when it stops at the first problem, with the file's group files.
// It reads the groups.
SoundArchive ReadStrictly(const File& file)
{
    sar::Options options;
    options.flavor = file.native->flavor;
    options.rules = sar::Rules::kStopAtFirstProblem;
    SoundArchive archive = SoundArchive::Load(hokuto::format::ExportArchive(file), options);
    for (const std::vector<uint8_t>& group_file : hokuto::format::ExportGroupFiles(file))
    {
        archive.AddGroupFile(group_file);
    }

    return archive;
}

// The files and items of group `group` of the archive that `file` builds.
sar::GroupContents ContentsOf(const File& file, uint32_t group)
{
    const SoundArchive archive = ReadStrictly(file);
    const sar::Flavor flavor = file.native->flavor;
    if (archive.Groups().at(group).file_id == sar::kExternalGroupFile)
    {
        return sar::ReadGroupFile(hokuto::format::ExportGroupFiles(file).at(0), flavor);
    }

    return sar::ReadGroupFile(archive.FileData(archive.Groups()[group].file_id), flavor);
}

// The groups that the entry of file `id` lists in the archive that `file` builds, by their item IDs. The entry holds a
// reference to the file's location. The reference to the list follows the location, and counts from it.
std::vector<uint32_t> GroupsListedBy(const File& file, uint32_t id)
{
    const Flavor flavor = file.native->flavor;
    const std::vector<uint8_t> archive = hokuto::format::ExportArchive(file);
    const sar::InfoBlock info = sar::InfoBlock::Read(sar::TablesOf(archive, flavor).info, flavor);
    const std::vector<uint8_t>& entry = info.files.at(id);
    const xsflib::Reader r(entry, flavor == Flavor::kCafe);
    const std::size_t location = r.U32(4);
    const std::size_t list = location + r.U32(location + 16);
    std::vector<uint32_t> groups;
    for (uint32_t k = 0; k < r.U32(list); ++k)
    {
        groups.push_back(r.U32(list + 4 + std::size_t{k} * 4));
    }

    return groups;
}

// The IDs of the files of `contents`, and whether each is a copy of the archive's file.
Members FilesOf(const sar::GroupContents& contents)
{
    Members files;
    for (const sar::GroupMember& member : contents.files)
    {
        files.emplace_back(member.file, member.copy.has_value());
    }

    return files;
}

// The IDs of the items of `contents`, each with its load flags.
Listed ItemsOf(const sar::GroupContents& contents)
{
    Listed items;
    for (const sar::GroupItem& item : contents.items)
    {
        items.emplace_back(item.item, item.load);
    }

    return items;
}

// Whether each copy of `contents` holds the bytes of the file in `archive`.
bool CopiesMatch(const sar::GroupContents& contents, const SoundArchive& archive)
{
    const auto same_copy = [&archive](const sar::GroupMember& member)
    {
        return !member.copy || std::ranges::equal(*member.copy, archive.FileData(member.file));
    };
    return std::ranges::all_of(contents.files, same_copy);
}

// The groups that `options` propose, by their places.
std::vector<uint32_t> Proposed(const std::vector<GroupOption>& options)
{
    std::vector<uint32_t> groups;
    for (const GroupOption& option : options)
    {
        if (option.proposed)
        {
            groups.push_back(option.group);
        }
    }

    return groups;
}

// The groups of `options` that hold the item, by their places.
std::vector<uint32_t> Holding(const std::vector<GroupOption>& options)
{
    std::vector<uint32_t> groups;
    for (const GroupOption& option : options)
    {
        if (option.holds)
        {
            groups.push_back(option.group);
        }
    }

    return groups;
}

// The message of the std::invalid_argument that `call` throws, or an empty string when it throws none.
template <typename Call>
std::string Refusal(Call call)
{
    try
    {
        call();
    }
    catch (const std::invalid_argument& error)
    {
        return error.what();
    }

    return "";
}

// A new sequence sound that copies sound `model` of `file`, with the archive's sequence.
File WithNewSound(const File& file, uint32_t model)
{
    const ntdlib::commands::Sequence sequence =
        hokuto::edit::ReadIncoming(sar_files::Sequence(file.native->flavor, kBytecode, {{"NEW", 0}})).sequence;
    return hokuto::edit::AddSequenceSound(file, model, "SEQ_NEW", sequence, 0, {}, std::nullopt);
}

// Each item requires its file first, then what its banks or its waves require. A bank requires the wave archives that
// its waves name and those that its entry lists. A change gives the item other banks or another file. A group and an
// item past its table require no file.
void TestFilesOfItem(Flavor flavor)
{
    const File file = TestFile(flavor);
    const std::shared_ptr<const SoundArchive> archive = ArchiveOf(file);
    const std::vector<uint32_t> kOnlyBankB = {kBankB};

    XSFLIB_CHECK(FilesOfItem(*archive, kSeqA) == std::vector<uint32_t>({kSeqAFile, kBankAFile, kWarcAFile}));
    XSFLIB_CHECK(FilesOfItem(*archive, kSeqB) == std::vector<uint32_t>({kSeqBFile, kBankBFile, kWarcBFile}));
    XSFLIB_CHECK(FilesOfItem(*archive, kWaveSound) == std::vector<uint32_t>({kWaveSoundFile, kWarcAFile}));
    XSFLIB_CHECK(FilesOfItem(*archive, kBankA) == std::vector<uint32_t>({kBankAFile, kWarcAFile}));
    XSFLIB_CHECK(FilesOfItem(*archive, kWarcB) == std::vector<uint32_t>({kWarcBFile}));
    XSFLIB_CHECK(FilesOfItem(*archive, kSeqA, {.banks = kOnlyBankB}) ==
                 std::vector<uint32_t>({kSeqAFile, kBankBFile, kWarcBFile}));
    XSFLIB_CHECK(FilesOfItem(*archive, kSeqA, {.banks = std::vector<uint32_t>{kBankA, kBankA}}) ==
                 std::vector<uint32_t>({kSeqAFile, kBankAFile, kWarcAFile}));
    XSFLIB_CHECK(FilesOfItem(*archive, kBankA, {.file = TestBank(flavor, kWarcB)}) ==
                 std::vector<uint32_t>({kBankAFile, kWarcBFile}));
    XSFLIB_CHECK(FilesOfItem(*archive, kWaveSound, {.file = TestWaveSounds(flavor, kWarcB)}) ==
                 std::vector<uint32_t>({kWaveSoundFile, kWarcBFile}));
    XSFLIB_CHECK(FilesOfItem(*archive, sar::ItemId(sar::kSoundItem, 7)).empty());
    XSFLIB_CHECK(FilesOfItem(*archive, sar::ItemId(sar::kGroupItem, kGroupA)).empty());
}

// The groups hold their files, GROUP_BESIDE in the group file. Without its group file, GROUP_BESIDE is unavailable.
void TestGroups(Flavor flavor)
{
    const File file = TestFile(flavor);
    const std::vector<Group> groups = GroupsOf(file, *ArchiveOf(file));
    XSFLIB_CHECK((groups == std::vector<Group>{
                                {kGroupA, "GROUP_A", {kSeqAFile, kBankAFile, kWarcAFile}, ""},
                                {kGroupCommon, "GROUP_COMMON", {kBankBFile, kWarcBFile}, ""},
                                {kGroupB, "GROUP_B", {kSeqBFile}, ""},
                                {kGroupBeside, "GROUP_BESIDE", {kWaveSoundFile, kWarcAFile}, ""},
                            }));

    const File alone = TestFile(flavor, false);
    const std::vector<Group> without = GroupsOf(alone, *ArchiveOf(alone));
    XSFLIB_CHECK(without.size() == 4);
    if (without.size() == 4)
    {
        XSFLIB_CHECK(without[kGroupBeside].files.empty() &&
                     without[kGroupBeside].unavailable ==
                         "The game keeps this group in a file beside the archive, and the .hok file doesn't hold it.");
        XSFLIB_CHECK(without[kGroupA] == groups[kGroupA]);
    }
}

// A new sound that copies SEQ_A: the window proposes GROUP_A, the group that loads SEQ_A. No group holds a new sound.
// With BANK_B, the new sound's file takes an ID past the archive's files. GROUP_A then takes the new file and BANK_B's
// files, and GROUP_COMMON the new file alone. A replacement of SEQ_B's bank: GROUP_B loads SEQ_B and lacks BANK_A's
// files. With its bank, GROUP_B holds SEQ_B. A replacement of BANK_A's file with one that names WARC_B: GROUP_A loads
// BANK_A and lacks WARC_B. The window proposes no group that the edit can't reach.
void TestOptions(Flavor flavor)
{
    const File file = TestFile(flavor);
    const std::shared_ptr<const SoundArchive> archive = ArchiveOf(file);
    const std::vector<Group> groups = GroupsOf(file, *archive);
    const std::vector<uint32_t> seq_a = FilesOfItem(*archive, kSeqA);
    const std::vector<uint32_t> seq_b = FilesOfItem(*archive, kSeqB);
    const std::vector<uint32_t> kOnlyBankA = {kBankA};

    const std::vector<GroupOption> copy = GroupOptions(groups, seq_a, {});
    XSFLIB_CHECK(copy.size() == 4 && Proposed(copy) == std::vector<uint32_t>{kGroupA} && Holding(copy).empty());
    if (copy.size() == 4)
    {
        XSFLIB_CHECK(copy[kGroupBeside].name == "GROUP_BESIDE" && copy[kGroupBeside].unavailable.empty());
    }

    std::vector<uint32_t> added = {kNewFile};
    std::ranges::copy(FilesOfItem(*archive, kBankB), std::back_inserter(added));
    const std::vector<GroupOption> with_bank = GroupOptions(groups, seq_a, added);
    XSFLIB_CHECK(with_bank.size() == 4 && Proposed(with_bank) == std::vector<uint32_t>{kGroupA} &&
                 Holding(with_bank).empty());
    if (with_bank.size() == 4)
    {
        XSFLIB_CHECK((with_bank[kGroupA].taken == std::vector<uint32_t>{kNewFile, kBankBFile, kWarcBFile}));
        XSFLIB_CHECK(with_bank[kGroupCommon].taken == std::vector<uint32_t>{kNewFile});
    }

    const std::vector<GroupOption> banks =
        GroupOptions(groups, seq_b, FilesOfItem(*archive, kSeqB, {.banks = kOnlyBankA}));
    XSFLIB_CHECK(banks.size() == 4 && Proposed(banks) == std::vector<uint32_t>{kGroupB} && Holding(banks).empty());
    if (banks.size() == 4)
    {
        XSFLIB_CHECK((banks[kGroupB].taken == std::vector<uint32_t>{kBankAFile, kWarcAFile}));
        XSFLIB_CHECK(banks[kGroupA].taken == std::vector<uint32_t>{kSeqBFile});
    }

    const std::vector<GroupOption> same = GroupOptions(groups, seq_b, seq_b);
    XSFLIB_CHECK(Proposed(same).empty() && Holding(same) == std::vector<uint32_t>{kGroupB});
    const std::vector<GroupOption> bank = GroupOptions(
        groups, FilesOfItem(*archive, kBankA), FilesOfItem(*archive, kBankA, {.file = TestBank(flavor, kWarcB)}));
    XSFLIB_CHECK(Proposed(bank) == std::vector<uint32_t>{kGroupA} && Holding(bank).empty());
    XSFLIB_CHECK(bank.size() == 4 && bank[kGroupA].taken == std::vector<uint32_t>{kWarcBFile});

    std::vector<Group> unreachable = groups;
    unreachable[kGroupA].unavailable = "Hokuto can't read this group's file.";
    const std::vector<GroupOption> none = GroupOptions(unreachable, seq_a, {});
    SEQLIB_REQUIRE(none.size() == groups.size());
    XSFLIB_CHECK(Proposed(none).empty() && none[kGroupA].unavailable == "Hokuto can't read this group's file.");
}

// Without the group file beside the archive, Hokuto can't read the wave sound's file. The file table lists GROUP_BESIDE
// as the file's group, and a message names it. A group that would take a file that Hokuto can't read is unavailable.
// Hokuto reads BANK_A's WARC_A here. The last check gives it to GroupOptions as a file that Hokuto can't read. In a
// replacement of SEQ_B's bank with BANK_A, GROUP_B loads SEQ_B and would take WARC_A, and so would GROUP_COMMON.
// GROUP_A would take SEQ_B's file alone, and GROUP_BESIDE takes no wave archive.
void TestUnreadable(Flavor flavor)
{
    const std::vector<uint32_t> kFiles = {kSeqAFile, kWaveSoundFile, kWarcAFile};
    const File file = TestFile(flavor);
    const std::shared_ptr<const SoundArchive> archive = ArchiveOf(file);
    const std::shared_ptr<const SoundArchive> alone = ArchiveOf(TestFile(flavor, false));
    XSFLIB_CHECK(UnreadableFiles(*alone, kFiles) == std::vector<uint32_t>{kWaveSoundFile});
    XSFLIB_CHECK(UnreadableFiles(*archive, kFiles).empty());
    XSFLIB_CHECK((hokuto::song::FilesInMissingGroups(*alone) == Missing{{kWaveSoundFile, {"GROUP_BESIDE"}}}));
    XSFLIB_CHECK(hokuto::song::FilesInMissingGroups(*archive).empty());
    std::string message;
    try
    {
        hokuto::song::ItemFile(*alone, {hokuto::song::ItemKind::kWaveSound, 2, 0});
    }
    catch (const std::runtime_error& error)
    {
        message = error.what();
    }
    const std::string kExpected =
        "The game keeps the file of WSD_A in the group file of GROUP_BESIDE. That file wasn't beside the archive when "
        "Hokuto imported it. Import the archive again with the group file beside it.";
    XSFLIB_CHECK(message == kExpected);

    const std::vector<uint32_t> kOnlyBankA = {kBankA};
    const std::vector<uint32_t> kUnreadable = {kWarcAFile};
    const std::vector<GroupOption> options =
        GroupOptions(GroupsOf(file, *archive), FilesOfItem(*archive, kSeqB),
                     FilesOfItem(*archive, kSeqB, {.banks = kOnlyBankA}), kUnreadable);
    const std::string kReason = "This group would take file 5 of the archive, and Hokuto can't read it.";
    XSFLIB_CHECK(options.size() == 4 && Proposed(options).empty() && Holding(options).empty());
    if (options.size() == 4)
    {
        XSFLIB_CHECK(options[kGroupB].unavailable == kReason && options[kGroupCommon].unavailable == kReason);
        XSFLIB_CHECK(options[kGroupA].unavailable.empty() && options[kGroupBeside].unavailable.empty());
    }
}

// A new sound that copies SEQ_A joins GROUP_A and GROUP_B. GROUP_A loads SEQ_A, and takes a link to the new sound's
// file alone. GROUP_B takes copies of all three files. Each lists the new sound with all of its parts. The file keeps
// its tags, and saves and opens again in both forms.
void TestJoinCopy(Flavor flavor)
{
    File file = TestFile(flavor);
    file.tags = {{"title", "Demo"}};
    const std::vector<uint32_t> seq_a = FilesOfItem(*ArchiveOf(file), kSeqA);
    const File added = WithNewSound(file, 0);
    const std::vector<uint32_t> kGroups = {kGroupA, kGroupB};
    const std::vector<uint32_t> kItems = {kNewSound};

    const File joined = JoinGroups(added, kGroups, kItems, seq_a);
    const std::shared_ptr<const SoundArchive> archive = ArchiveOf(joined);
    XSFLIB_CHECK(archive->Sounds().size() == 4 && archive->Sounds()[3].file_id == kNewFile);
    const sar::GroupContents group_a = ContentsOf(joined, kGroupA);
    const sar::GroupContents group_b = ContentsOf(joined, kGroupB);
    XSFLIB_CHECK(FilesOf(group_a) ==
                 Members({{kSeqAFile, false}, {kBankAFile, false}, {kWarcAFile, false}, {kNewFile, false}}));
    XSFLIB_CHECK(ItemsOf(group_a) == Listed({{kSeqA, kAll}, {kNewSound, kAll}}));
    XSFLIB_CHECK(FilesOf(group_b) ==
                 Members({{kSeqBFile, true}, {kNewFile, true}, {kBankAFile, true}, {kWarcAFile, true}}));
    XSFLIB_CHECK(ItemsOf(group_b) == Listed({{kSeqB, kSequenceAlone}, {kNewSound, kAll}}));
    XSFLIB_CHECK(CopiesMatch(group_b, *archive));
    XSFLIB_CHECK(joined.tags == file.tags);
    XSFLIB_CHECK(hokuto::format::Read(hokuto::format::Write(joined)) == joined);
    XSFLIB_CHECK(hokuto::format::ReadText(hokuto::format::WriteText(joined)) == joined);

    // The strict reader reads the new archive.
    XSFLIB_CHECK(ReadStrictly(joined).Sounds().size() == 4);
}

// A new sound that copies SEQ_B joins GROUP_B, the group that loads SEQ_B. GROUP_B takes a copy of the new sound's file
// alone. GROUP_COMMON holds the bank's files. GROUP_B lists the new sound with its sequence alone.
void TestJoinModel(Flavor flavor)
{
    const File file = TestFile(flavor);
    const std::vector<uint32_t> kGroups = {kGroupB};
    const std::vector<uint32_t> kItems = {kNewSound};

    const File joined = JoinGroups(WithNewSound(file, 1), kGroups, kItems, FilesOfItem(*ArchiveOf(file), kSeqB));
    const sar::GroupContents group_b = ContentsOf(joined, kGroupB);
    XSFLIB_CHECK(FilesOf(group_b) == Members({{kSeqBFile, true}, {kNewFile, true}}));
    XSFLIB_CHECK(ItemsOf(group_b) == Listed({{kSeqB, kSequenceAlone}, {kNewSound, kSequenceAlone}}));
    XSFLIB_CHECK(CopiesMatch(group_b, *ArchiveOf(joined)));
    XSFLIB_CHECK(ContentsOf(joined, kGroupCommon) == ContentsOf(file, kGroupCommon));

    // With BANK_A in its second slot, the new sound requires BANK_A's files too. GROUP_B takes them. The group holds
    // the banks and the wave archives of the new sound in part: it lists the new sound with its sequence alone again.
    const ntdlib::commands::Sequence sequence =
        hokuto::edit::ReadIncoming(sar_files::Sequence(flavor, kBytecode, {{"NEW", 0}})).sequence;
    const File two_banks = JoinGroups(
        hokuto::edit::AddSequenceSound(file, 1, "SEQ_NEW", sequence, 0, {}, std::vector<uint32_t>{kBankB, kBankA}),
        kGroups, kItems, FilesOfItem(*ArchiveOf(file), kSeqB));
    const sar::GroupContents both = ContentsOf(two_banks, kGroupB);
    XSFLIB_CHECK(FilesOf(both) ==
                 Members({{kSeqBFile, true}, {kNewFile, true}, {kBankAFile, true}, {kWarcAFile, true}}));
    XSFLIB_CHECK(ItemsOf(both) == Listed({{kSeqB, kSequenceAlone}, {kNewSound, kSequenceAlone}}));
}

// Two new wave sounds of one file copy WSD_A and join GROUP_BESIDE. The group file takes a copy of the new wave sound
// file, lists both sounds, and the export writes it again. The file's entry lists the group. WARC_A joins the group
// again, and the group stays as it was: it holds WARC_A's file already.
void TestJoinBeside(Flavor flavor)
{
    const File file = TestFile(flavor);
    const File added =
        hokuto::edit::AddWaveSounds(file, 2, "WSD_NEW", TestWaveSounds(flavor, kWarcA, 2), {{kWarcA, 0}});
    const std::vector<uint32_t> kGroups = {kGroupBeside};
    const uint32_t kSecondSound = sar::ItemId(sar::kSoundItem, 4);
    const std::vector<uint32_t> kItems = {kNewSound, kSecondSound};

    const File joined = JoinGroups(added, kGroups, kItems, FilesOfItem(*ArchiveOf(file), kWaveSound));
    const sar::GroupContents beside = ContentsOf(joined, kGroupBeside);
    XSFLIB_CHECK(FilesOf(beside) == Members({{kWaveSoundFile, true}, {kWarcAFile, true}, {kNewFile, true}}));
    XSFLIB_CHECK(ItemsOf(beside) == Listed({{kWaveSound, kAll}, {kNewSound, kAll}, {kSecondSound, kAll}}));
    XSFLIB_CHECK(CopiesMatch(beside, *ArchiveOf(joined)));
    XSFLIB_CHECK(GroupsListedBy(joined, kNewFile) == std::vector<uint32_t>{sar::ItemId(sar::kGroupItem, kGroupBeside)});
    const std::vector<hokuto::format::ChangedGroupFile> changed = hokuto::format::ChangedGroupFiles(joined);
    XSFLIB_CHECK(changed.size() == 1 && hokuto::format::ChangedGroupFiles(added).empty());
    if (changed.size() == 1)
    {
        XSFLIB_CHECK(changed[0].name == "GROUP_BESIDE" && changed[0].group == kGroupBeside);
    }

    const std::vector<uint32_t> kOnlyWarcA = {kWarcA};
    const File again = JoinGroups(file, kGroups, kOnlyWarcA, {});
    XSFLIB_CHECK(ContentsOf(again, kGroupBeside) == ContentsOf(file, kGroupBeside));
    XSFLIB_CHECK(hokuto::format::ChangedGroupFiles(again).empty());
}

// SEQ_B takes BANK_A in its slot. GROUP_B loads SEQ_B. The group takes copies of BANK_A's files and lists SEQ_B as
// before. SEQ_B then joins GROUP_COMMON. That group takes a copy of SEQ_B's sequence file and lists SEQ_B as before:
// once, with its bank alone.
void TestJoinReplacement(Flavor flavor)
{
    const File file = TestFile(flavor);
    const File changed = hokuto::edit::ChangeSound(file, 1, {.value = hokuto::edit::SoundValue::kBank, .to = kBankA});
    const std::vector<uint32_t> kGroups = {kGroupB};
    const std::vector<uint32_t> kItems = {kSeqB};

    const File joined = JoinGroups(changed, kGroups, kItems, FilesOfItem(*ArchiveOf(file), kSeqB));
    const sar::GroupContents group_b = ContentsOf(joined, kGroupB);
    XSFLIB_CHECK(FilesOf(group_b) == Members({{kSeqBFile, true}, {kBankAFile, true}, {kWarcAFile, true}}));
    XSFLIB_CHECK(ItemsOf(group_b) == Listed({{kSeqB, kSequenceAlone}}));
    XSFLIB_CHECK(CopiesMatch(group_b, *ArchiveOf(joined)));

    const std::vector<uint32_t> kCommon = {kGroupCommon};
    const File both = JoinGroups(file, kCommon, kItems, FilesOfItem(*ArchiveOf(file), kSeqB));
    const sar::GroupContents group_common = ContentsOf(both, kGroupCommon);
    XSFLIB_CHECK(FilesOf(group_common) == Members({{kBankBFile, true}, {kWarcBFile, true}, {kSeqBFile, true}}));
    XSFLIB_CHECK(ItemsOf(group_common) == Listed({{kBankB, kAll}, {kSeqB, kBanksAlone}}));
    XSFLIB_CHECK(CopiesMatch(group_common, *ArchiveOf(both)));
}

// The wave sound joins a new group, GROUP_NEW. Only the group file beside the archive holds the wave sound's file, and
// a link can't reach it. The new group therefore takes copies of the wave sound's file and WARC_A's. It lists the wave
// sound with all of its parts. Each file's list of groups names GROUP_BESIDE, then GROUP_NEW. GROUP_BESIDE stays.
void TestJoinUnplaced(Flavor flavor)
{
    const File file = TestFile(flavor);
    const std::vector<uint32_t> kOnlyWaveSound = {kWaveSound};
    const std::vector<std::string> kNames = {"GROUP_NEW"};

    const File joined = JoinGroups(file, {}, kOnlyWaveSound, {}, kNames);

    const sar::GroupContents contents = ContentsOf(joined, kGroupNew);
    XSFLIB_CHECK(FilesOf(contents) == Members({{kWaveSoundFile, true}, {kWarcAFile, true}}));
    XSFLIB_CHECK(ItemsOf(contents) == Listed({{kWaveSound, kAll}}));
    XSFLIB_CHECK(CopiesMatch(contents, *ArchiveOf(joined)));
    const std::vector<uint32_t> kLists = {sar::ItemId(sar::kGroupItem, kGroupBeside),
                                          sar::ItemId(sar::kGroupItem, kGroupNew)};
    XSFLIB_CHECK(GroupsListedBy(joined, kWaveSoundFile) == kLists && GroupsListedBy(joined, kWarcAFile) == kLists);
    XSFLIB_CHECK(ContentsOf(joined, kGroupBeside) == ContentsOf(file, kGroupBeside));
    XSFLIB_CHECK(hokuto::format::Read(hokuto::format::Write(joined)) == joined);
}

// Without groups or items, the file stays. The edit refuses a group past the table, a group file that the Hokuto file
// lacks, and a link to a file that only a group holds.
void TestRefusals(Flavor flavor)
{
    const File file = TestFile(flavor);
    const std::vector<uint32_t> kNone;
    const std::vector<uint32_t> kPast = {9};
    const std::vector<uint32_t> kBeside = {kGroupBeside};
    const std::vector<uint32_t> kOnlyGroupA = {kGroupA};
    const std::vector<uint32_t> kOnlySeqA = {kSeqA};
    const std::vector<uint32_t> kOnlyWaveSound = {kWaveSound};

    XSFLIB_CHECK(JoinGroups(file, kNone, kOnlySeqA, {}) == file && JoinGroups(file, kOnlyGroupA, kNone, {}) == file);
    XSFLIB_CHECK(Refusal([&] { JoinGroups(file, kPast, kOnlySeqA, {}); }) == "The archive has no group 9.");
    XSFLIB_CHECK(Refusal([&] { JoinGroups(TestFile(flavor, false), kBeside, kOnlySeqA, {}); }) ==
                 "The game keeps group GROUP_BESIDE in a file beside the archive, and the .hok file doesn't hold it.");
    XSFLIB_CHECK(Refusal([&] { JoinGroups(file, kOnlyGroupA, kOnlyWaveSound, {}); }) ==
                 "Group GROUP_A links to the archive's files, and a file that it would take lies only in groups. "
                 "Hokuto can't link to it.");
}

// A new sound that copies SEQ_A joins GROUP_A and a new group, GROUP_NEW. The new group goes after GROUP_BESIDE, with a
// new file after the new sound's. It links to the new sound's files and lists the sound with all of its parts. The new
// sound's file lists both groups. The game's sizes give the new group a size of 0. The file saves and opens again in
// both forms. The model form takes a new group too, and so does a new file: its new sound's file joins the group. The
// edit refuses a name that is empty or taken, and two new groups of one name.
void TestNewGroups(Flavor flavor)
{
    const File file = TestFile(flavor);
    const std::vector<uint32_t> seq_a = FilesOfItem(*ArchiveOf(file), kSeqA);
    const std::vector<uint32_t> kGroups = {kGroupA};
    const std::vector<uint32_t> kItems = {kNewSound};
    const std::vector<std::string> kNames = {"GROUP_NEW"};

    const File joined = JoinGroups(WithNewSound(file, 0), kGroups, kItems, seq_a, kNames);
    const SoundArchive read = ReadStrictly(joined);
    XSFLIB_CHECK(read.Groups().size() == 5 && joined.native->tables->groups.size() == 5);
    if (read.Groups().size() == 5)
    {
        XSFLIB_CHECK(read.Groups()[kGroupNew].name == "GROUP_NEW" && read.Groups()[kGroupNew].file_id == kNewFile + 1);
    }

    const sar::GroupContents group_new = ContentsOf(joined, kGroupNew);
    XSFLIB_CHECK(FilesOf(group_new) == Members({{kNewFile, false}, {kBankAFile, false}, {kWarcAFile, false}}));
    XSFLIB_CHECK(ItemsOf(group_new) == Listed({{kNewSound, kAll}}));
    XSFLIB_CHECK(FilesOf(ContentsOf(joined, kGroupA)) ==
                 Members({{kSeqAFile, false}, {kBankAFile, false}, {kWarcAFile, false}, {kNewFile, false}}));
    XSFLIB_CHECK(GroupsListedBy(joined, kNewFile) == std::vector<uint32_t>({sar::ItemId(sar::kGroupItem, kGroupA),
                                                                            sar::ItemId(sar::kGroupItem, kGroupNew)}));
    XSFLIB_CHECK(hokuto::format::Read(hokuto::format::Write(joined)) == joined);
    XSFLIB_CHECK(hokuto::format::ReadText(hokuto::format::WriteText(joined)) == joined);

    // The game's sizes give the new group a size of 0.
    std::vector<uint64_t> sizes = file.native->game_sizes.value_or(hokuto::format::GameSizes()).groups;
    sizes.push_back(0);
    XSFLIB_CHECK(sizes.size() == 5 && joined.native->game_sizes && joined.native->game_sizes->groups == sizes);

    // The model form.
    const File model = JoinGroups(WithNewSound(hokuto::format::InModelForm(file), 0), kGroups, kItems, seq_a, kNames);
    const File back = hokuto::format::Read(hokuto::format::Write(model));
    XSFLIB_CHECK(back.form == hokuto::format::ArchiveForm::kModel);
    XSFLIB_CHECK(hokuto::format::ExportArchive(back) == hokuto::format::ExportArchive(model));
    XSFLIB_CHECK(ContentsOf(back, kGroupNew) == group_new);

    // A new file's sound, in a new group alone.
    const hokuto::format::File empty = hokuto::format::NewFile(flavor == Flavor::kNx ? hokuto::format::Engine::kSwitch
                                                                                     : hokuto::format::Engine::kWiiU);
    const ntdlib::commands::Sequence sequence =
        hokuto::edit::ReadIncoming(sar_files::Sequence(flavor, kBytecode, {{"NEW", 0}})).sequence;
    const uint32_t kFirstSound = sar::ItemId(sar::kSoundItem, 0);
    const File made = JoinGroups(hokuto::edit::AddSequenceSound(empty, std::nullopt, "SEQ_NEW", sequence, 0, {}), {},
                                 std::span(&kFirstSound, 1), {}, kNames);
    XSFLIB_CHECK(ReadStrictly(made).Groups().size() == 1 && FilesOf(ContentsOf(made, 0)) == Members({{0, false}}));
    XSFLIB_CHECK(ItemsOf(ContentsOf(made, 0)) == Listed({{kFirstSound, kAll}}));
    XSFLIB_CHECK(hokuto::format::Read(hokuto::format::Write(made)) == made);

    // The refusals.
    const auto refusal = [&file](std::vector<std::string> wanted)
    {
        return Refusal([&] { hokuto::edit::AddGroups(file, wanted); });
    };

    XSFLIB_CHECK(hokuto::edit::AddGroups(file, {}) == file);
    XSFLIB_CHECK(refusal({""}) == "Give the new group a name.");
    XSFLIB_CHECK(refusal({"GROUP_A"}) == "The archive holds the name GROUP_A already.");
    XSFLIB_CHECK(refusal({"SEQ_B"}) == "The archive holds the name SEQ_B already.");
    XSFLIB_CHECK(refusal({"GROUP_NEW", "GROUP_NEW"}) == "Two new groups have the name GROUP_NEW.");
}

// The model form keeps the groups. The model gives the archive and the group file beside it back. The group file counts
// as Hokuto's: an export writes it. A new sound that copies SEQ_A joins GROUP_A and GROUP_B in the model form, and the
// file comes back from both forms with the same groups.
void TestModelForm(Flavor flavor)
{
    const File file = TestFile(flavor);
    const File model = hokuto::format::InModelForm(file);
    XSFLIB_CHECK(hokuto::format::ExportArchive(model) == hokuto::format::ExportArchive(file));
    XSFLIB_CHECK(hokuto::format::ExportGroupFiles(model) == hokuto::format::ExportGroupFiles(file));
    XSFLIB_CHECK(hokuto::format::ChangedGroupFiles(file).empty());
    XSFLIB_CHECK(hokuto::format::ChangedGroupFiles(model).size() == 1);
    XSFLIB_CHECK(hokuto::format::Read(hokuto::format::Write(model)) == model);
    XSFLIB_CHECK(hokuto::format::ReadText(hokuto::format::WriteText(model)) == model);

    const std::vector<uint32_t> kGroups = {kGroupA, kGroupB};
    const std::vector<uint32_t> kItems = {kNewSound};
    const File joined = JoinGroups(WithNewSound(model, 0), kGroups, kItems, FilesOfItem(*ArchiveOf(model), kSeqA));
    const File back = hokuto::format::Read(hokuto::format::Write(joined));
    const File from_text = hokuto::format::ReadText(hokuto::format::WriteText(joined));
    XSFLIB_CHECK(back.form == hokuto::format::ArchiveForm::kModel);
    XSFLIB_CHECK(hokuto::format::ExportArchive(back) == hokuto::format::ExportArchive(joined));
    XSFLIB_CHECK(ContentsOf(back, kGroupA) == ContentsOf(joined, kGroupA));
    XSFLIB_CHECK(ContentsOf(back, kGroupB) == ContentsOf(joined, kGroupB));
    XSFLIB_CHECK(hokuto::format::Read(hokuto::format::Write(back)) == back);
    XSFLIB_CHECK(from_text == back &&
                 hokuto::format::ExportArchive(from_text) == hokuto::format::ExportArchive(joined));
    XSFLIB_CHECK(ContentsOf(from_text, kGroupA) == ContentsOf(joined, kGroupA));
    XSFLIB_CHECK(ContentsOf(from_text, kGroupB) == ContentsOf(joined, kGroupB));
}

} // namespace

int main()
{
    for (const Flavor flavor : {Flavor::kNx, Flavor::kCafe})
    {
        TestModelForm(flavor);
        TestFilesOfItem(flavor);
        TestGroups(flavor);
        TestOptions(flavor);
        TestUnreadable(flavor);
        TestJoinCopy(flavor);
        TestJoinModel(flavor);
        TestJoinBeside(flavor);
        TestJoinReplacement(flavor);
        TestJoinUnplaced(flavor);
        TestRefusals(flavor);
        TestNewGroups(flavor);
    }

    return xsflib::testing::Summary("groups");
}
