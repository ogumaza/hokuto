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

#include "edit/files_internal.h"
#include "edit/new_items.h"
#include "format/archive.h"
#include "format/kinds.h"

namespace hokuto::edit
{
namespace
{

namespace sar = ntdlib::sar;

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
        const std::optional<std::size_t> list = WaveArchiveListOf(entry, flavor);
        if (!list)
        {
            return listed;
        }

        const xsflib::Reader r(entry, flavor == sar::Flavor::kCafe);
        for (uint32_t i = 0; i < r.U32(*list); ++i)
        {
            listed.push_back(r.U32(*list + 4 + std::size_t{i} * 4));
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

// The files that the items of an archive require, with their parts, as PartsOfItem collects them.
class NeededParts
{
public:
    explicit NeededParts(const sar::SoundArchive& archive) : archive_(archive), flavor_(archive.ReadOptions().flavor)
    {
    }

    // Adds file `file` with part `part`, unless the file is there already.
    void Add(uint32_t file, uint32_t part)
    {
        if (std::ranges::none_of(needed_, [file](const NeededFile& n) { return n.file == file; }))
        {
            needed_.push_back({file, part});
        }
    }

    // Adds the wave archive of item ID `id`.
    void AddWaveArchive(uint32_t id)
    {
        const uint32_t index = sar::ItemNumber(id);
        if (sar::ItemType(id) == sar::kWaveArchiveItem && index < archive_.WaveArchives().size())
        {
            Add(archive_.WaveArchives()[index].file_id, kLoadWaveArchive);
        }
    }

    // Adds the wave archives that `data`, a bank or wave sound file, names.
    void AddNamed(std::span<const uint8_t> data)
    {
        try
        {
            for (const NamedWaveArchive& named : WaveArchivesNamed(data, flavor_))
            {
                AddWaveArchive(named.id);
            }
        }
        catch (const std::invalid_argument&)
        {
            // ntdlib can't read the file: it names no wave archives.
        }
    }

    // Adds bank `bank`, whose file contains `data` when that isn't empty, and the wave archives that it names or lists.
    void AddBank(uint32_t bank, std::span<const uint8_t> data)
    {
        if (bank < archive_.Banks().size())
        {
            const uint32_t file = archive_.Banks()[bank].file_id;
            Add(file, kLoadBank);
            AddNamed(data.empty() ? archive_.FileData(file) : data);
            for (const uint32_t id : ListedWaveArchives(archive_.Bytes(), flavor_, bank))
            {
                AddWaveArchive(id);
            }
        }
    }

    // Adds the parts of sound `index` after `change`.
    void AddSoundParts(uint32_t index, const ItemChange& change)
    {
        if (index < archive_.Sounds().size())
        {
            const sar::SoundInfo& sound = archive_.Sounds()[index];
            if (sound.type == sar::SoundType::kSequence)
            {
                Add(sound.file_id, kLoadSequence);
                for (const uint32_t bank : change.banks ? *change.banks : sound.banks)
                {
                    if (sar::ItemType(bank) == sar::kBankItem)
                    {
                        AddBank(sar::ItemNumber(bank), {});
                    }
                }
            }
            else if (sound.type == sar::SoundType::kWaveSound)
            {
                Add(sound.file_id, kLoadWaveSound);
                AddNamed(change.file.empty() ? archive_.FileData(sound.file_id) : change.file);
            }
        }
    }

    std::vector<NeededFile> Needed() &&
    {
        return std::move(needed_);
    }

private:
    const sar::SoundArchive& archive_;
    const sar::Flavor flavor_;
    std::vector<NeededFile> needed_;
};

// The files that item `item` of `archive` requires, with their parts (FilesOfItem).
std::vector<NeededFile> PartsOfItem(const sar::SoundArchive& archive, uint32_t item, const ItemChange& change)
{
    NeededParts needed(archive);

    const uint32_t index = sar::ItemNumber(item);
    switch (sar::ItemType(item))
    {
    case sar::kSoundItem:
        needed.AddSoundParts(index, change);
        break;

    case sar::kBankItem:
        needed.AddBank(index, change.file);
        break;

    case sar::kWaveArchiveItem:
        needed.AddWaveArchive(item);
        break;

    default:
        break;
    }

    return std::move(needed).Needed();
}

// What ntdlib reads of `bytes`, an archive of `flavor`, with the group files beside it, when it stops at the first
// problem: the reader of the groups. Throws xsflib::FormatError if ntdlib finds a problem.
sar::SoundArchive ReadStrictly(std::span<const uint8_t> bytes, sar::Flavor flavor,
                               std::span<const std::vector<uint8_t>> group_files)
{
    sar::SoundArchive archive =
        sar::SoundArchive::Load(std::vector<uint8_t>(bytes.begin(), bytes.end()), StrictOptions(flavor));
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
                group.unavailable = "The game stores this group in a separate file. The .hok file doesn't contain it.";
            }
        }
        catch (const std::exception&)
        {
            group.files.clear();
            group.unavailable = "Can't read the group file.";
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
            option.unavailable = "This group would require archive file " + std::to_string(*missing) +
                                 ", but Hokuto can't read that file.";
        }

        option.proposed = option.unavailable.empty() && !option.holds && loads;
    }

    return options;
}

namespace
{

// The files that each of `items` requires, with their parts (PartsOfItem), and all of their files.
struct ItemParts
{
    std::vector<std::vector<NeededFile>> parts;
    std::vector<uint32_t> needed;
};

ItemParts ItemFiles(const sar::SoundArchive& archive, std::span<const uint32_t> items)
{
    ItemParts result;
    for (const uint32_t item : items)
    {
        result.parts.push_back(PartsOfItem(archive, item, {}));
        for (const NeededFile& part : result.parts.back())
        {
            result.needed.push_back(part.file);
        }
    }

    return result;
}

// The contents of group `group` of `archive`, named `name`, from `group_files` at `place` where the game stores it
// beside the archive (RebuildGroup). Throws std::invalid_argument with a message for the user where they can't be
// read.
sar::GroupContents GroupContentsOf(const sar::SoundArchive& archive, uint32_t group, const std::string& name,
                                   std::optional<std::size_t> place,
                                   const std::vector<std::vector<uint8_t>>& group_files, sar::Flavor flavor)
{
    try
    {
        return sar::ReadGroupFile(
            place ? std::span<const uint8_t>(group_files[*place]) : archive.FileData(archive.Groups()[group].file_id),
            flavor);
    }
    catch (const std::exception& error)
    {
        throw std::invalid_argument("Can't read the file of group " + name + ". " + format::AsSentence(error.what()));
    }
}

// The members that group `name` of `archive` adds for the files `taken`: copies where the group contains copies, links
// otherwise (RebuildGroup). `contents` is what the group contains now, and `unplaced` the files that a group can't link
// to.
std::vector<sar::GroupMember> NewMembers(const sar::SoundArchive& archive, const std::string& name,
                                         std::span<const uint32_t> taken, const sar::GroupContents& contents,
                                         std::span<const uint32_t> unplaced)
{
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
            const std::span<const uint8_t> data = archive.FileData(id);
            if (data.empty())
            {
                throw std::invalid_argument("Group " + name + " would require archive file " + std::to_string(id) +
                                            ", but Hokuto can't read that file.");
            }

            member.copy = std::vector<uint8_t>(data.begin(), data.end());
        }
    }

    return members;
}

// The items of `items` that the group lists from now on: those whose files `held`, the files that the group held
// before, didn't hold (RebuildGroup). `parts` contains the files of the items, `contents` what the group contains now,
// and `after` the files that it contains after the edit.
std::vector<sar::GroupItem> NewListedItems(std::span<const uint32_t> items,
                                           std::span<const std::vector<NeededFile>> parts,
                                           const sar::GroupContents& contents, std::span<const uint32_t> held,
                                           std::span<const uint32_t> after)
{
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

    return listed;
}

// Writes the new `members` and `listed` items of group `group`, named `name`, into `bytes`, and into `group_files` at
// `place` where the game stores the group beside the archive (RebuildGroup). Throws std::invalid_argument with a
// message for the user where the group can't take them.
void WriteGroupChanges(uint32_t group, const std::string& name, std::optional<std::size_t> place, sar::Flavor flavor,
                       std::span<const sar::GroupMember> members, std::span<const sar::GroupItem> listed,
                       std::vector<uint8_t>& bytes, std::vector<std::vector<uint8_t>>& group_files)
{
    try
    {
        if (place)
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
                                        " contains links to archive files. A file that it would require exists only "
                                        "inside groups. Hokuto can't link to it.");
        }

        throw std::invalid_argument("Can't add the files to group " + name + ". " + format::AsSentence(reason));
    }
    catch (const xsflib::FormatError& error)
    {
        throw std::invalid_argument("Can't add the files to group " + name +
                                    ". The group file doesn't use the layout of NintendoWare's tools. " +
                                    format::AsSentence(error.what()));
    }
}

// Group `group` of `archive`, the archive of `file` as `bytes` and `group_files` describe it, after it gains the files
// of `items` that it doesn't contain (JoinGroups). `parts` and `needed` give the items' files (ItemFiles). `unplaced`
// contains the files that a group can't link to. Writes the group's changes into `bytes` and `group_files`.
void RebuildGroup(const format::File& file, const sar::SoundArchive& archive, uint32_t group,
                  std::span<const uint32_t> items, std::span<const uint32_t> before,
                  std::span<const std::vector<NeededFile>> parts, std::span<const uint32_t> needed,
                  std::span<const uint32_t> unplaced, std::vector<uint8_t>& bytes,
                  std::vector<std::vector<uint8_t>>& group_files)
{
    const sar::Flavor flavor = FlavorOf(*file.native);
    if (group >= archive.Groups().size())
    {
        throw std::invalid_argument("The archive has no group " + std::to_string(group) + ".");
    }

    // What the group holds and lists now. Another group's edit leaves this group's file as it was.
    const std::string& name = archive.Groups()[group].name;
    const bool beside = archive.Groups()[group].file_id == sar::kExternalGroupFile;
    const std::optional<std::size_t> place = beside ? GroupFileOf(file, group) : std::nullopt;
    if (beside && !place)
    {
        throw std::invalid_argument("The game stores group " + name +
                                    " in a separate file. The .hok file doesn't contain it.");
    }

    const sar::GroupContents contents = GroupContentsOf(archive, group, name, place, group_files, flavor);
    std::vector<uint32_t> held;
    for (const sar::GroupMember& member : contents.files)
    {
        held.push_back(member.file);
    }

    // The files that the group takes, as copies or links. A group holds one kind (ntdlib::sar::GroupWith). A group
    // without files takes copies when it can't link to one of the files, and links otherwise. The items whose files
    // it didn't hold.
    const std::vector<sar::GroupMember> members =
        NewMembers(archive, name, Taken(needed, held, before), contents, unplaced);
    std::vector<uint32_t> after = held;
    for (const sar::GroupMember& member : members)
    {
        after.push_back(member.file);
    }

    const std::vector<sar::GroupItem> listed = NewListedItems(items, parts, contents, held, after);
    if (members.empty() && listed.empty())
    {
        return;
    }

    WriteGroupChanges(group, name, place, flavor, members, listed, bytes, group_files);
}

} // namespace

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
        throw std::invalid_argument("The file contains no game archive.");
    }

    // The archive with the group files, the files that a group can't link to, and the files that each item requires.
    const sar::Flavor flavor = FlavorOf(*file.native);
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
        throw std::invalid_argument(std::string("Can't read the archive groups. ") + format::AsSentence(error.what()));
    }

    const auto [parts, needed] = ItemFiles(*archive, items);

    for (const uint32_t group : groups)
    {
        RebuildGroup(file, *archive, group, items, before, parts, needed, unplaced, bytes, group_files);
    }

    return Rebuilt(file, bytes, std::move(group_files));
}

} // namespace hokuto::edit
