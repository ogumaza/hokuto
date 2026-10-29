// SPDX-License-Identifier: MIT

#include "edit/groups.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/sar/formats.h"
#include "ntdlib/sar/groups.h"
#include "ntdlib/sar/info_block.h"
#include "ntdlib/sar/tables.h"
#include "xsflib/binary.h"

#include "edit/new_items.h"
#include "format/archive.h"
#include "format/kinds.h"

namespace hokuto::edit
{
namespace
{

namespace sar = ntdlib::sar;

// A bank entry of the INFO block: its file at 0, then a reference to its list of wave archives. The reference's type
// sits at 4 and its offset from the entry's start at 8. The list holds a count, then the wave archives' item IDs.
constexpr std::size_t kListReference = 4;
constexpr uint16_t kListType = 0x0100;

// A file entry of the INFO block: a reference to the file's location, with its type at 0 and its offset from the
// entry's start at 4. A file in the archive has type 0x220C. Its location holds the file's offset from the FILE block's
// body at 4, or 0xFFFFFFFF where only groups hold the file.
constexpr uint16_t kInternalFile = 0x220C;
constexpr uint32_t kNoPlace = 0xFFFFFFFF;

// The load flags of an item that a group lists: the parts of the item that the game loads with the group. The tools
// write 0xFFFFFFFF for every part.
constexpr uint32_t kLoadSequence = 1;
constexpr uint32_t kLoadWaveSound = 2;
constexpr uint32_t kLoadBank = 4;
constexpr uint32_t kLoadWaveArchive = 8;
constexpr uint32_t kLoadAll = 0xFFFFFFFF;

// A file that an item requires, and the part of the item that it belongs to, by its load flag.
struct NeededFile
{
    uint32_t file = 0;
    uint32_t part = 0;
};

// The wave archives that the entry of bank `bank` lists in `bytes`, an archive of `flavor`, by their item IDs. None
// when the tables can't be read.
std::vector<uint32_t> ListedWaveArchives(std::span<const uint8_t> bytes, sar::Flavor flavor, uint32_t bank)
{
    std::vector<uint32_t> listed;
    try
    {
        const sar::Tables tables = sar::TablesOf(bytes, flavor);
        const sar::InfoBlock info = sar::InfoBlock::Read(tables.info, flavor);
        if (bank >= info.banks.size())
        {
            return listed;
        }

        const std::vector<uint8_t>& entry = info.banks[bank];
        const xsflib::Reader r(entry, flavor == sar::Flavor::kCafe);
        if (entry.size() < kListReference + 8 || r.U16(kListReference) != kListType)
        {
            return listed;
        }
        const std::size_t list = r.U32(kListReference + 4);
        for (uint32_t i = 0; list + 4 <= entry.size() && i < r.U32(list); ++i)
        {
            listed.push_back(r.U32(list + 4 + std::size_t{i} * 4));
        }
    }
    catch (const std::exception&)
    {
        listed.clear();
    }

    return listed;
}

// The files of `bytes`, an archive of `flavor`, that the FILE block doesn't hold at a place in the file table. A group
// can't link to such a file: only groups hold it, or it lies beside the archive. Throws xsflib::FormatError for an
// archive whose tables can't be read.
std::vector<uint32_t> FilesWithoutPlaces(std::span<const uint8_t> bytes, sar::Flavor flavor)
{
    std::vector<uint32_t> files;
    const sar::InfoBlock info = sar::InfoBlock::Read(sar::TablesOf(bytes, flavor).info, flavor);
    for (std::size_t id = 0; id < info.files.size(); ++id)
    {
        const std::vector<uint8_t>& entry = info.files[id];
        const xsflib::Reader r(entry, flavor == sar::Flavor::kCafe);
        const std::size_t location = entry.size() >= 8 && r.U16(0) == kInternalFile ? r.U32(4) : entry.size();
        if (location + 8 > entry.size() || r.U32(location + 4) == kNoPlace)
        {
            files.push_back(static_cast<uint32_t>(id));
        }
    }

    return files;
}

// The files that item `item` of `archive` requires, with their parts (FilesOfItem).
std::vector<NeededFile> PartsOfItem(const sar::SoundArchive& archive, uint32_t item, const ItemChange& change)
{
    std::vector<NeededFile> needed;
    const sar::Flavor flavor = archive.ReadOptions().flavor;
    const auto add = [&needed](uint32_t file, uint32_t part)
    {
        if (std::ranges::none_of(needed, [file](const NeededFile& n) { return n.file == file; }))
        {
            needed.push_back({file, part});
        }
    };
    const auto add_wave_archive = [&](uint32_t id)
    {
        const uint32_t index = id & 0xFFFFFF;
        if (id >> 24 == sar::kWaveArchiveItem && index < archive.WaveArchives().size())
        {
            add(archive.WaveArchives()[index].file_id, kLoadWaveArchive);
        }
    };
    const auto add_named = [&](std::span<const uint8_t> data)
    {
        try
        {
            for (const NamedWaveArchive& named : WaveArchivesNamed(data, flavor))
            {
                add_wave_archive(named.id);
            }
        }
        catch (const std::invalid_argument&)
        {
            // ntdlib can't read the file: it names no wave archives.
        }
    };
    const auto add_bank = [&](uint32_t bank, std::span<const uint8_t> data)
    {
        if (bank < archive.Banks().size())
        {
            const uint32_t file = archive.Banks()[bank].file_id;
            add(file, kLoadBank);
            add_named(data.empty() ? archive.FileData(file) : data);
            for (const uint32_t id : ListedWaveArchives(archive.Bytes(), flavor, bank))
            {
                add_wave_archive(id);
            }
        }
    };

    const uint32_t index = item & 0xFFFFFF;
    switch (item >> 24)
    {
    case sar::kSoundItem:
        if (index < archive.Sounds().size())
        {
            const sar::SoundInfo& sound = archive.Sounds()[index];
            if (sound.type == sar::SoundType::kSequence)
            {
                add(sound.file_id, kLoadSequence);
                for (const uint32_t bank : change.banks ? *change.banks : sound.banks)
                {
                    if (bank >> 24 == sar::kBankItem)
                    {
                        add_bank(bank & 0xFFFFFF, {});
                    }
                }
            }
            else if (sound.type == sar::SoundType::kWaveSound)
            {
                add(sound.file_id, kLoadWaveSound);
                add_named(change.file.empty() ? archive.FileData(sound.file_id) : change.file);
            }
        }
        break;

    case sar::kBankItem:
        add_bank(index, change.file);
        break;

    case sar::kWaveArchiveItem:
        add_wave_archive(item);
        break;

    default:
        break;
    }

    return needed;
}

// What ntdlib reads of `bytes`, an archive of `flavor`, with the group files beside it, when it stops at the first
// problem: the reader of the groups. Throws xsflib::FormatError if ntdlib finds a problem.
sar::SoundArchive ReadStrictly(std::span<const uint8_t> bytes, sar::Flavor flavor,
                               std::span<const std::vector<uint8_t>> group_files)
{
    sar::Options options;
    options.flavor = flavor;
    options.rules = sar::Rules::kStopAtFirstProblem;
    sar::SoundArchive archive = sar::SoundArchive::Load(std::vector<uint8_t>(bytes.begin(), bytes.end()), options);
    for (const std::vector<uint8_t>& group_file : group_files)
    {
        archive.AddGroupFile(group_file);
    }

    return archive;
}

// The place among `file`'s group files of the group file of group `group`. Nothing if the file lacks it.
std::optional<std::size_t> GroupFileOf(const format::File& file, uint32_t group)
{
    for (std::size_t g = 0; g < file.native->group_files.size(); ++g)
    {
        if (file.native->group_files[g].group == group)
        {
            return g;
        }
    }

    return std::nullopt;
}

// Whether `files` holds `file`.
bool Contains(std::span<const uint32_t> files, uint32_t file)
{
    return std::ranges::find(files, file) != files.end();
}

// The files of `needed` that a group that holds `held` takes: those that it lacks. A group that loads the old item or
// the model, the first file of `before`, takes only those that `before` lacks.
std::vector<uint32_t> Taken(std::span<const uint32_t> needed, std::span<const uint32_t> held,
                            std::span<const uint32_t> before)
{
    const bool loads = !before.empty() && Contains(held, before.front());
    std::vector<uint32_t> taken;
    for (const uint32_t file : needed)
    {
        if (!Contains(held, file) && !(loads && Contains(before, file)) && !Contains(taken, file))
        {
            taken.push_back(file);
        }
    }

    return taken;
}

// The load flags of an item whose files are `parts` in a group that holds `held`: all of them when the group holds each
// file, or else the parts whose files it holds.
uint32_t LoadFlags(std::span<const NeededFile> parts, std::span<const uint32_t> held)
{
    uint32_t whole = 0;
    uint32_t partial = 0;
    for (const NeededFile& needed : parts)
    {
        (Contains(held, needed.file) ? whole : partial) |= needed.part;
    }

    return partial == 0 ? kLoadAll : whole & ~partial;
}

} // namespace

std::vector<uint32_t> FilesOfItem(const sar::SoundArchive& archive, uint32_t item, const ItemChange& change)
{
    std::vector<uint32_t> files;
    for (const NeededFile& needed : PartsOfItem(archive, item, change))
    {
        files.push_back(needed.file);
    }

    return files;
}

std::vector<uint32_t> UnreadableFiles(const sar::SoundArchive& archive, std::span<const uint32_t> files)
{
    std::vector<uint32_t> unreadable;
    for (const uint32_t id : files)
    {
        if (archive.FileData(id).empty())
        {
            unreadable.push_back(id);
        }
    }

    return unreadable;
}

std::vector<Group> GroupsOf(const format::File& file, const sar::SoundArchive& archive)
{
    std::vector<Group> groups;
    const sar::Flavor flavor = archive.ReadOptions().flavor;
    std::optional<sar::SoundArchive> read;
    try
    {
        read = ReadStrictly(archive.Bytes(), flavor, archive.GroupFiles());
    }
    catch (const std::exception&)
    {
        return groups;
    }

    for (std::size_t g = 0; g < read->Groups().size(); ++g)
    {
        Group& group = groups.emplace_back();
        group.group = static_cast<uint32_t>(g);
        group.name = read->Groups()[g].name;
        try
        {
            if (read->Groups()[g].file_id != sar::kExternalGroupFile)
            {
                group.files = read->GroupFileIds(g);
            }
            else if (const std::optional<std::size_t> place =
                         file.native ? GroupFileOf(file, group.group) : std::nullopt;
                     place && *place < archive.GroupFiles().size())
            {
                for (const sar::GroupMember& member : sar::ReadGroupFile(archive.GroupFiles()[*place], flavor).files)
                {
                    group.files.push_back(member.file);
                }
            }
            else
            {
                group.unavailable =
                    "The game keeps this group in a file beside the archive, and the .hok file doesn't hold it.";
            }
        }
        catch (const std::exception&)
        {
            group.files.clear();
            group.unavailable = "Hokuto can't read this group's file.";
        }
    }

    return groups;
}

std::vector<GroupOption> GroupOptions(std::span<const Group> groups, std::span<const uint32_t> before,
                                      std::span<const uint32_t> after, std::span<const uint32_t> unreadable)
{
    std::vector<GroupOption> options;
    for (const Group& group : groups)
    {
        GroupOption& option = options.emplace_back();
        option.group = group.group;
        option.name = group.name;
        option.unavailable = group.unavailable;
        const bool loads = !before.empty() && Contains(group.files, before.front());
        const std::vector<uint32_t> taken = Taken(after, group.files, before);
        option.holds = !after.empty() && taken.empty();
        option.taken = taken;
        const auto missing = std::ranges::find_if(taken, [&](uint32_t id) { return Contains(unreadable, id); });
        if (option.unavailable.empty() && missing != taken.end())
        {
            option.unavailable =
                "This group would take file " + std::to_string(*missing) + " of the archive, and Hokuto can't read it.";
        }
        option.proposed = option.unavailable.empty() && !option.holds && loads;
    }

    return options;
}

format::File JoinGroups(const format::File& file, std::span<const uint32_t> groups, std::span<const uint32_t> items,
                        std::span<const uint32_t> before, std::span<const std::string> new_groups)
{
    if (!new_groups.empty())
    {
        // The new groups go after the others, and load the items too.
        const format::File added = AddGroups(file, new_groups);
        std::vector<uint32_t> all(groups.begin(), groups.end());
        for (std::size_t k = 0; k < new_groups.size(); k++)
        {
            all.push_back(static_cast<uint32_t>(file.native->tables->groups.size() + k));
        }

        return JoinGroups(added, all, items, before);
    }
    if (groups.empty() || items.empty())
    {
        return file;
    }
    if (!file.native)
    {
        throw std::invalid_argument("The file holds no archive of a game.");
    }

    // The archive with the group files, the files that a group can't link to, and the files that each item requires.
    const sar::Flavor flavor = file.native->flavor;
    std::vector<uint8_t> bytes = format::ExportArchive(file);
    std::vector<std::vector<uint8_t>> group_files = format::ExportGroupFiles(file);
    std::optional<sar::SoundArchive> archive;
    std::vector<uint32_t> unplaced;
    try
    {
        archive = ReadStrictly(bytes, flavor, group_files);
        unplaced = FilesWithoutPlaces(bytes, flavor);
    }
    catch (const xsflib::FormatError& error)
    {
        throw std::invalid_argument(std::string("Hokuto can't read the archive's groups. ") +
                                    format::AsSentence(error.what()));
    }
    std::vector<std::vector<NeededFile>> parts;
    std::vector<uint32_t> needed;
    for (const uint32_t item : items)
    {
        parts.push_back(PartsOfItem(*archive, item, {}));
        for (const NeededFile& part : parts.back())
        {
            needed.push_back(part.file);
        }
    }

    for (const uint32_t group : groups)
    {
        if (group >= archive->Groups().size())
        {
            throw std::invalid_argument("The archive has no group " + std::to_string(group) + ".");
        }

        // What the group holds and lists now. Another group's edit leaves this group's file as it was.
        const std::string& name = archive->Groups()[group].name;
        const bool beside = archive->Groups()[group].file_id == sar::kExternalGroupFile;
        const std::optional<std::size_t> place = beside ? GroupFileOf(file, group) : std::nullopt;
        if (beside && !place)
        {
            throw std::invalid_argument("The game keeps group " + name +
                                        " in a file beside the archive, and the .hok file doesn't hold it.");
        }
        sar::GroupContents contents;
        try
        {
            contents = sar::ReadGroupFile(beside ? std::span<const uint8_t>(group_files[*place])
                                                 : archive->FileData(archive->Groups()[group].file_id),
                                          flavor);
        }
        catch (const std::exception& error)
        {
            throw std::invalid_argument("Hokuto can't read the file of group " + name + ". " +
                                        format::AsSentence(error.what()));
        }
        std::vector<uint32_t> held;
        for (const sar::GroupMember& member : contents.files)
        {
            held.push_back(member.file);
        }

        // The files that the group takes, as copies or links. A group holds one kind (ntdlib::sar::GroupWith). A group
        // without files takes copies when it can't link to one of the files, and links otherwise. The items whose files
        // it didn't hold.
        const std::vector<uint32_t> taken = Taken(needed, held, before);
        const auto linkless = [&unplaced](uint32_t id)
        {
            return Contains(unplaced, id);
        };
        const bool copies =
            contents.files.empty() ? std::ranges::any_of(taken, linkless) : contents.files.front().copy.has_value();
        std::vector<sar::GroupMember> members;
        for (const uint32_t id : taken)
        {
            sar::GroupMember& member = members.emplace_back();
            member.file = id;
            if (copies)
            {
                const std::span<const uint8_t> data = archive->FileData(id);
                if (data.empty())
                {
                    throw std::invalid_argument("Group " + name + " would take file " + std::to_string(id) +
                                                " of the archive, and Hokuto can't read it.");
                }
                member.copy = std::vector<uint8_t>(data.begin(), data.end());
            }
        }
        std::vector<uint32_t> after = held;
        for (const sar::GroupMember& member : members)
        {
            after.push_back(member.file);
        }
        std::vector<sar::GroupItem> listed;
        for (std::size_t i = 0; i < items.size(); ++i)
        {
            const uint32_t item = items[i];
            const bool lists =
                std::ranges::any_of(contents.items, [item](const sar::GroupItem& g) { return g.item == item; });
            if (!lists && !parts[i].empty() && !Contains(held, parts[i].front().file))
            {
                listed.push_back({.item = item, .load = LoadFlags(parts[i], after)});
            }
        }
        if (members.empty() && listed.empty())
        {
            continue;
        }

        try
        {
            if (beside)
            {
                group_files[*place] = sar::GroupWith(group_files[*place], flavor, members, listed);
                bytes = sar::WithGroupLists(bytes, flavor, group, members);
            }
            else
            {
                bytes = sar::WithGroupMembers(bytes, flavor, group, members, listed);
            }
        }
        catch (const std::invalid_argument& error)
        {
            const std::string reason = error.what();
            if (reason.find("a link to file") != std::string::npos)
            {
                throw std::invalid_argument("Group " + name +
                                            " links to the archive's files, and a file that it would take lies only "
                                            "in groups. Hokuto can't link to it.");
            }
            throw std::invalid_argument("Hokuto can't put the files into group " + name + ". " +
                                        format::AsSentence(reason));
        }
        catch (const xsflib::FormatError& error)
        {
            throw std::invalid_argument("Hokuto can't put the files into group " + name +
                                        ". Its file is laid out otherwise than NintendoWare's tools lay one out. " +
                                        format::AsSentence(error.what()));
        }
    }

    return Rebuilt(file, bytes, std::move(group_files));
}

} // namespace hokuto::edit
