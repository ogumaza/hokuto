// SPDX-License-Identifier: MIT

// The joins of groups and the new groups of hokuto_check_replace.

#include "check_replace.h"

namespace hokuto::tools::replace
{

Choice ChosenGroups(const format::File& file, const sar::SoundArchive& built, std::span<const edit::Group> groups,
                    std::span<const uint32_t> before, std::span<const uint32_t> after, bool extra, Totals& totals)
{
    const auto beside = [&file](uint32_t group)
    {
        return std::ranges::any_of(file.native->group_files,
                                   [group](const format::NativeGroupFile& each) { return each.group == group; });
    };

    const std::vector<edit::GroupOption> options =
        edit::GroupOptions(groups, before, after, edit::UnreadableFiles(built, after));
    const std::vector<edit::GroupOption> readable = edit::GroupOptions(groups, before, after);
    for (std::size_t i = 0; i < options.size(); ++i)
    {
        totals.join_unreadable += readable[i].proposed && !options[i].proposed ? 1 : 0;
    }
    Choice chosen;
    const edit::GroupOption* other = nullptr;
    for (const edit::GroupOption& option : options)
    {
        if (option.proposed)
        {
            chosen.groups.push_back(option.group);
        }
        else if (extra && option.unavailable.empty() && !option.holds &&
                 (!other || (beside(option.group) && !beside(other->group))))
        {
            other = &option;
        }
    }
    totals.join_unproposed += chosen.groups.empty() ? 1 : 0;
    if (other)
    {
        chosen.groups.insert(std::ranges::upper_bound(chosen.groups, other->group), other->group);
        chosen.extra = other->name;
    }

    return chosen;
}

namespace
{

// Whether `files` lists `file`.
bool Has(std::span<const uint32_t> files, uint32_t file)
{
    return std::ranges::find(files, file) != files.end();
}

// The files of the file table `info_was`, a table of `flavor`, that a link can't reach (JoinProblem).
std::set<uint32_t> UnreachableFiles(const sar::InfoBlock& info_was, sar::Flavor flavor)
{
    std::set<uint32_t> unplaced;
    for (uint32_t id = 0; id < info_was.files.size(); id++)
    {
        const xsflib::Reader r(info_was.files[id], flavor == sar::Flavor::kCafe);
        if (r.U16(0) != kInternalFile || r.U32(r.U32(4) + 4) == 0xFFFFFFFF)
        {
            unplaced.insert(id);
        }
    }

    return unplaced;
}

// The files and groups that the chosen groups change, as JoinProblem gathers them group by group.
struct JoinedGroups
{
    std::set<uint32_t> changed_files;                // the files of the chosen groups inside the archive
    std::map<uint32_t, std::vector<uint32_t>> named; // the groups that each file's list gains
    std::set<uint32_t> took_beside;                  // the chosen groups beside the archive that took something
};

// What is wrong with the items that group `group_name` lists after `items` join it, or nothing. `old` and `contents`
// are the group before and after the join, `listing` is whether the file of the group contains an INFX block,
// `required` lists the files that each item requires, and `held` the files that the group contains, `held_before`
// before the join. Counts the items listed in `totals`.
std::string ListedItemsProblem(const sar::GroupContents& old, const sar::GroupContents& contents, bool listing,
                               std::span<const uint32_t> items, const std::vector<std::vector<uint32_t>>& required,
                               std::span<const uint32_t> held, std::span<const uint32_t> held_before,
                               const std::string& group_name, Totals& totals)
{
    // The group lists each item whose file it didn't hold and that it didn't list, once. The load flags name every
    // part exactly when the group holds every file that the item requires. A group's file without INFX lists none.
    std::size_t next = old.items.size();
    totals.join_unlisting += listing ? 0 : 1;
    for (std::size_t i = 0; i < items.size() && listing; i++)
    {
        const uint32_t item = items[i];
        const bool listed =
            std::ranges::any_of(old.items, [item](const sar::GroupItem& each) { return each.item == item; });
        if (listed || required[i].empty() || Has(held_before, required[i].front()))
        {
            continue;
        }
        if (next >= contents.items.size() || contents.items[next].item != item)
        {
            return "doesn't list item " + HexOf(item) + " in group " + group_name;
        }
        const uint32_t load = contents.items[next++].load;
        const bool whole = std::ranges::all_of(required[i], [&](uint32_t file) { return Has(held, file); });
        if (whole != (load == 0xFFFFFFFF) || (!whole && (load == 0 || (load & ~0xFu) != 0)))
        {
            return "lists item " + HexOf(item) + " in group " + group_name + " with the load flags " + HexOf(load);
        }
        totals.join_listed++;
        totals.join_partial += whole ? 0 : 1;
    }
    if (next != contents.items.size())
    {
        return "lists other items in group " + group_name;
    }

    return {};
}

// What is wrong with chosen group `g`, named `group_name`, after `items` join it (JoinProblem), or nothing. `was` and
// `now` are the archive before and after the join. `required` lists the files that each item requires, `unplaced` the
// files that a link can't reach and `wave_archives` the wave archives' files. Gathers what the group changes in
// `joined_groups` and counts the group in `totals`.
std::string JoinedGroupProblem(const StrictRead& was, const StrictRead& now, uint32_t g, const std::string& group_name,
                               std::span<const uint32_t> items, std::span<const uint32_t> before,
                               const std::vector<std::vector<uint32_t>>& required, const std::set<uint32_t>& unplaced,
                               const std::set<uint32_t>& wave_archives, JoinedGroups& joined_groups, Totals& totals)
{
    if (!was.groups[g] || !now.groups[g])
    {
        return "leaves group " + group_name + " unread";
    }

    // The group keeps its files and items first.
    const sar::GroupContents& old = *was.groups[g];
    const sar::GroupContents& contents = *now.groups[g];
    if (contents.files.size() < old.files.size() || contents.items.size() < old.items.size() ||
        !std::equal(old.files.begin(), old.files.end(), contents.files.begin()) ||
        !std::equal(old.items.begin(), old.items.end(), contents.items.begin()))
    {
        return "doesn't keep the files and items of group " + group_name + " first";
    }
    std::vector<uint32_t> held;
    for (const sar::GroupMember& member : contents.files)
    {
        held.push_back(member.file);
    }
    const std::span<const uint32_t> held_before = std::span<const uint32_t>(held).first(old.files.size());
    const bool loads = !before.empty() && Has(held_before, before.front());

    // The group holds each file that an item requires, but for the files of the old item or the model where it
    // loads them. It takes no other file. It takes copies where it held copies. A group without files takes copies
    // when a link can't reach one of the files, and links otherwise.
    for (const std::vector<uint32_t>& files : required)
    {
        for (const uint32_t file : files)
        {
            if (!Has(held, file) && !(loads && Has(before, file)))
            {
                return "leaves file " + std::to_string(file) + " out of group " + group_name;
            }
        }
    }
    const auto linkless = [&unplaced](const sar::GroupMember& member)
    {
        return unplaced.contains(member.file);
    };
    const bool copies =
        old.files.empty() ? std::ranges::any_of(contents.files, linkless) : old.files.front().copy.has_value();
    for (std::size_t m = old.files.size(); m < contents.files.size(); m++)
    {
        const sar::GroupMember& member = contents.files[m];
        const bool wanted = std::ranges::any_of(required, [&](const std::vector<uint32_t>& files)
                                                { return Has(files, member.file); }) &&
                            !(loads && Has(before, member.file));
        if (!wanted || std::ranges::count(held, member.file) != 1)
        {
            return "puts file " + std::to_string(member.file) + " into group " + group_name + " for no item";
        }
        if (member.copy.has_value() != copies)
        {
            return "gives group " + group_name + (copies ? " a link to file " : " a copy of file ") +
                   std::to_string(member.file);
        }
        if (member.copy && !std::ranges::equal(*member.copy, now.archive->FileData(member.file)))
        {
            return "gives group " + group_name + " a copy of file " + std::to_string(member.file) + " with other bytes";
        }
        (member.copy ? totals.join_copies : totals.join_links)++;
        if (member.copy || !wave_archives.contains(member.file))
        {
            joined_groups.named[member.file].push_back(sar::ItemId(sar::kGroupItem, g));
        }
    }

    if (std::string problem =
            ListedItemsProblem(old, contents, now.listing[g], items, required, held, held_before, group_name, totals);
        !problem.empty())
    {
        return problem;
    }

    const uint32_t group_file = now.archive->Groups()[g].file_id;
    if (group_file != sar::kExternalGroupFile)
    {
        joined_groups.changed_files.insert(group_file);
    }
    else if (contents != old)
    {
        joined_groups.took_beside.insert(g);
    }
    totals.join_groups++;
    totals.join_loading += loads ? 1 : 0;
    totals.join_beside += group_file == sar::kExternalGroupFile ? 1 : 0;

    return {};
}

// What is wrong with the files of `now`, the archive `was` after a join that changed `joined_groups`, or nothing. Both
// are archives of `flavor`.
std::string OtherFilesProblem(const StrictRead& was, const StrictRead& now, const JoinedGroups& joined_groups,
                              sar::Flavor flavor)
{
    for (uint32_t id = 0; id < now.archive->FileCount(); id++)
    {
        if (!joined_groups.changed_files.contains(id) &&
            !std::ranges::equal(now.archive->FileData(id), was.archive->FileData(id)))
        {
            return "changes file " + std::to_string(id);
        }
    }
    const std::vector<std::optional<std::vector<uint32_t>>> lists_was = ListsOfGroups(was.bytes, flavor);
    std::vector<std::optional<std::vector<uint32_t>>> lists_now = ListsOfGroups(now.bytes, flavor);
    for (uint32_t id = 0; id < lists_now.size() && id < lists_was.size(); id++)
    {
        std::optional<std::vector<uint32_t>> expected = lists_was[id];
        const auto gained = joined_groups.named.find(id);
        for (const uint32_t group : gained != joined_groups.named.end() ? gained->second : std::vector<uint32_t>())
        {
            if (!expected)
            {
                return "gives file " + std::to_string(id) + " a group without a list of groups";
            }

            if (!Has(*expected, group))
            {
                expected->push_back(group);
            }
        }
        if (expected)
        {
            std::ranges::sort(*expected);
        }
        if (lists_now[id])
        {
            std::ranges::sort(*lists_now[id]);
        }
        if (lists_now[id] != expected)
        {
            return "changes the list of groups of file " + std::to_string(id);
        }
    }

    return {};
}

// What is wrong with the tables and group files of `now`, the archive `was` after the join that made `joined`, or
// nothing. `tables_was` and `info_was` are the tables of `was`, and `took_beside` lists the chosen groups beside the
// archive that took something. Both archives are of `flavor`.
std::string TablesProblem(const StrictRead& was, const StrictRead& now, const format::File& joined,
                          const sar::Tables& tables_was, const sar::InfoBlock& info_was,
                          const std::set<uint32_t>& took_beside, sar::Flavor flavor)
{
    const sar::Tables tables_now = sar::TablesOf(now.bytes, flavor);
    const sar::InfoBlock info_now = sar::InfoBlock::Read(tables_now.info, flavor);
    if (info_now.sounds != info_was.sounds || info_now.sound_groups != info_was.sound_groups ||
        info_now.banks != info_was.banks || info_now.wave_archives != info_was.wave_archives ||
        info_now.groups != info_was.groups || info_now.players != info_was.players ||
        info_now.player_settings != info_was.player_settings ||
        tables_now.strings.has_value() != tables_was.strings.has_value() ||
        (tables_now.strings && !std::ranges::equal(*tables_now.strings, *tables_was.strings)))
    {
        return "changes the tables of the items";
    }
    const std::vector<format::ChangedGroupFile> changed = format::ChangedGroupFiles(joined);
    for (std::size_t k = 0; k < now.group_files.size(); k++)
    {
        const std::optional<uint32_t> group = joined.native->group_files[k].group;
        const bool took = group && took_beside.contains(*group);
        if (!took && now.group_files[k] != was.group_files[k])
        {
            return "changes group file " + std::to_string(k);
        }
        if (took &&
            std::ranges::none_of(changed, [k](const format::ChangedGroupFile& each) { return each.index == k; }))
        {
            return "leaves group file " + std::to_string(k) + " out of the export";
        }
    }

    return {};
}

// What is wrong with `joined`, the version of `added` in which groups `chosen` load `items` (edit::JoinGroups), or
// nothing. `before` lists the files that the old item or the model required. Counts the groups, files and items that
// joined in `totals`.
std::string JoinProblem(const format::File& added, const format::File& joined, std::span<const uint32_t> chosen,
                        std::span<const uint32_t> items, std::span<const uint32_t> before, Totals& totals)
{
    const sar::Flavor flavor = FlavorOf(*added.native);
    const StrictRead was = ReadStrictly(added);
    const StrictRead now = ReadStrictly(joined);
    if (now.groups.size() != was.groups.size() || now.archive->FileCount() != was.archive->FileCount())
    {
        return "changes the number of groups or files";
    }

    // The files that each item requires, and the wave archives' files: a link to one leaves its list of groups.
    std::vector<std::vector<uint32_t>> required;
    for (const uint32_t item : items)
    {
        required.push_back(edit::FilesOfItem(*now.archive, item));
    }
    std::set<uint32_t> wave_archives;
    for (const sar::WaveArchiveInfo& wave_archive : now.archive->WaveArchives())
    {
        wave_archives.insert(wave_archive.file_id);
    }

    // The files that a link can't reach: the file table gives them no place in the FILE block, or puts them beside the
    // archive.
    const sar::Tables tables_was = sar::TablesOf(was.bytes, flavor);
    const sar::InfoBlock info_was = sar::InfoBlock::Read(tables_was.info, flavor);
    const std::set<uint32_t> unplaced = UnreachableFiles(info_was, flavor);

    JoinedGroups joined_groups;
    for (uint32_t g = 0; g < now.groups.size(); g++)
    {
        const std::string& group_name = now.archive->Groups()[g].name;
        if (!Has(chosen, g))
        {
            if (now.groups[g] != was.groups[g])
            {
                return "changes group " + group_name + " outside the choice";
            }
            continue;
        }
        if (std::string problem = JoinedGroupProblem(was, now, g, group_name, items, before, required, unplaced,
                                                     wave_archives, joined_groups, totals);
            !problem.empty())
        {
            return problem;
        }
    }

    // The other files keep their bytes. Each file's list of groups gains the groups that took a copy of the file, or a
    // link to it but for a wave archive's file.
    if (std::string problem = OtherFilesProblem(was, now, joined_groups, flavor); !problem.empty())
    {
        return problem;
    }

    // The items' tables, the names and the other group files stay. An export writes each group file that changed.
    if (std::string problem = TablesProblem(was, now, joined, tables_was, info_was, joined_groups.took_beside, flavor);
        !problem.empty())
    {
        return problem;
    }

    // The file saves and opens again, and the same join again changes nothing.
    if (!hokuto::tools::SavesAndOpens(joined, true))
    {
        return "doesn't save and open again";
    }
    if (edit::JoinGroups(joined, chosen, items, before) != joined)
    {
        return "changes the groups when the items join them again";
    }

    return {};
}

} // namespace

std::string JoinCheck(const format::File& added, const Choice& choice, std::span<const uint32_t> items,
                      std::span<const uint32_t> before, Totals& totals)
{
    const std::span<const uint32_t> chosen = choice.groups;
    format::File joined;
    const auto start = std::chrono::steady_clock::now();
    try
    {
        joined = edit::JoinGroups(added, chosen, items, before);
    }
    catch (const std::invalid_argument& error)
    {
        // The window shows the edit's refusal of a link to a file that only groups hold, and of a file that Hokuto
        // can't read in the group that the windows don't propose.
        const std::string reason = error.what();
        const bool unreadable = !choice.extra.empty() && reason.starts_with("Group " + choice.extra + " would need") &&
                                reason.ends_with("but Hokuto can't read that file.");
        if (!unreadable && reason.find("Hokuto can't link to it") == std::string::npos)
        {
            throw;
        }
        totals.join_refused++;
        return {};
    }
    const std::chrono::duration<double> took = std::chrono::steady_clock::now() - start;
    totals.join_slowest = std::max(totals.join_slowest, took.count());
    totals.join_items++;

    const std::string problem = JoinProblem(added, joined, chosen, items, before, totals);
    totals.join_failed += problem.empty() ? 0 : 1;

    return problem.empty() ? problem : "joins its groups and " + problem;
}

namespace
{

// A name for a new group that no item of `archive` holds: GROUP_CHECK, or GROUP_CHECK_2 and on.
std::string NewGroupName(const sar::SoundArchive& archive)
{
    const sar::Flavor flavor = archive.ReadOptions().flavor;
    const std::optional<std::span<const uint8_t>> strings = sar::TablesOf(archive.Bytes(), flavor).strings;
    std::string name = "GROUP_CHECK";
    for (int k = 2; strings && sar::FindItem(*strings, flavor, name); k++)
    {
        name = "GROUP_CHECK_" + std::to_string(k);
    }

    return name;
}

// What is wrong with `with_group`, the version of `added` with a new group named `name` (edit::AddGroups), or nothing.
// The new group follows the others, with an empty group's file at the end of the file table. The other groups and files
// keep their bytes, the group files beside the archive stay, and every item keeps its name. The game's sizes give the
// new group a size of 0. The file saves and opens again.
std::string NewGroupProblem(const format::File& added, const format::File& with_group, const std::string& name)
{
    const StrictRead was = ReadStrictly(added);
    const StrictRead now = ReadStrictly(with_group);
    const std::size_t groups = was.groups.size();
    const std::size_t files = was.archive->FileCount();
    if (now.groups.size() != groups + 1 || now.archive->FileCount() != files + 1)
    {
        return "adds other groups or files than the new group's";
    }
    const sar::GroupInfo& group = now.archive->Groups()[groups];
    if (group.name != name || group.file_id != files)
    {
        return "gives the new group another name or file";
    }

    // On the 3DS, the new group's file takes the highest version of the groups' files inside the archive.
    const sar::Flavor flavor = FlavorOf(*added.native);
    std::optional<uint32_t> version;
    for (uint32_t g = 0; g < groups && flavor == sar::Flavor::kCtr; g++)
    {
        const uint32_t file_id = was.archive->Groups()[g].file_id;
        const std::span<const uint8_t> bytes =
            file_id != sar::kExternalGroupFile ? was.archive->FileData(file_id) : std::span<const uint8_t>();
        if (bytes.size() >= 12)
        {
            version = std::max(version.value_or(0), xsflib::Reader(bytes, false).U32(8));
        }
    }
    const std::vector<uint8_t> empty = sar::EmptyGroupFile(flavor, version);
    if (!now.groups[groups] || *now.groups[groups] != sar::GroupContents() ||
        !std::ranges::equal(now.archive->FileData(group.file_id),
                            std::span(empty).first(sar::GroupSizeInArchive(empty, flavor))))
    {
        return "gives the new group another file than an empty group's";
    }
    for (uint32_t g = 0; g < groups; g++)
    {
        if (now.groups[g] != was.groups[g] || now.archive->Groups()[g].name != was.archive->Groups()[g].name)
        {
            return "changes group " + was.archive->Groups()[g].name;
        }
    }
    for (uint32_t id = 0; id < files; id++)
    {
        if (!std::ranges::equal(now.archive->FileData(id), was.archive->FileData(id)))
        {
            return "changes file " + std::to_string(id);
        }
    }
    if (now.group_files != was.group_files)
    {
        return "changes the group files beside the archive";
    }

    // Every item keeps its name.
    const auto names = [](const auto& items)
    {
        std::vector<std::string> each;
        for (const auto& item : items)
        {
            each.push_back(item.name);
        }

        return each;
    };
    if (names(now.archive->Sounds()) != names(was.archive->Sounds()) ||
        names(now.archive->SoundGroups()) != names(was.archive->SoundGroups()) ||
        names(now.archive->Banks()) != names(was.archive->Banks()) ||
        names(now.archive->WaveArchives()) != names(was.archive->WaveArchives()) ||
        names(now.archive->Players()) != names(was.archive->Players()))
    {
        return "changes the names of the items";
    }

    // The game's sizes, and the saved file.
    const std::optional<format::GameSizes>& sizes = with_group.native->game_sizes;
    if (added.native->game_sizes &&
        (!sizes || sizes->archive != added.native->game_sizes->archive ||
         !std::ranges::equal(std::span(sizes->groups).first(groups), added.native->game_sizes->groups) ||
         sizes->groups.size() != groups + 1 || sizes->groups.back() != 0))
    {
        return "gives the new group a size among the game's sizes";
    }
    if (!hokuto::tools::SavesAndOpens(with_group, true))
    {
        return "doesn't save and open again";
    }

    return {};
}

} // namespace

std::string NewGroupCheck(const format::File& added, const sar::SoundArchive& archive, std::span<const uint32_t> items,
                          std::span<const uint32_t> before, Totals& totals)
{
    const std::vector<std::string> names = {NewGroupName(archive)};
    format::File joined;
    try
    {
        joined = edit::JoinGroups(added, {}, items, before, names);
    }
    catch (const std::invalid_argument& error)
    {
        // The window shows the edit's refusal of a file that Hokuto can't read, and of a name in an archive without
        // names.
        const std::string reason = error.what();
        const bool unreadable = reason.starts_with("Group " + names.front() + " would need") &&
                                reason.ends_with("but Hokuto can't read that file.");
        if (!unreadable && reason != "The archive has no item names. Can't name a new group in it.")
        {
            throw;
        }
        totals.made_refused++;
        return {};
    }

    const format::File with_group = edit::AddGroups(added, names);
    std::string problem = NewGroupProblem(added, with_group, names.front());
    if (problem.empty())
    {
        Totals joins;
        const auto group = static_cast<uint32_t>(added.native->tables->groups.size());
        problem = JoinProblem(with_group, joined, std::span(&group, 1), items, before, joins);
        totals.made_copies += joins.join_copies;
        totals.made_links += joins.join_links;
        totals.made_listed += joins.join_listed;
        totals.made_unlisting += joins.join_unlisting;
    }
    totals.made_groups++;
    totals.made_failed += problem.empty() ? 0 : 1;

    return problem.empty() ? problem : "joins a new group and " + problem;
}

bool JoinsGroups(const Joins& joins, std::span<const edit::Group> edit_groups)
{
    const bool joining = !edit_groups.empty() && joins.count < kJoins;

    return joining || !joins.new_group;
}

std::string JoinAndNewGroupCheck(const format::File& file, const sar::SoundArchive& built,
                                 std::span<const edit::Group> edit_groups, std::span<const uint32_t> items,
                                 std::span<const uint32_t> before, const std::function<std::vector<uint32_t>()>& after,
                                 const std::function<const format::File&()>& added, Joins& joins, Totals& totals)
{
    const bool joining = !edit_groups.empty() && joins.count < kJoins;
    const Choice chosen = joining ? ChosenGroups(file, built, edit_groups, before,
                                                 after ? after() : std::vector<uint32_t>(), joins.count == 0, totals)
                                  : Choice();
    if (chosen.groups.empty() && joins.new_group)
    {
        return {};
    }

    const format::File& with_items = added();
    std::string problem;
    if (!chosen.groups.empty())
    {
        joins.count++;
        problem = JoinCheck(with_items, chosen, items, before, totals);
    }
    if (problem.empty() && !joins.new_group)
    {
        joins.new_group = true;
        problem = NewGroupCheck(with_items, built, items, before, totals);
    }

    return problem;
}

void PrintJoins(const std::string& name, const Totals& totals, bool groups)
{
    if (groups)
    {
        std::printf(
            "%s: %zu items or sets of new items joined %zu groups. %zu of the groups loaded the old item or the model, "
            "and %zu lie beside the archive. The groups took %zu copies and %zu links, and listed %zu items, %zu of "
            "them with some of their parts. %zu times, the group's file held no INFX block and listed no items. %zu "
            "joins fail, and the edit refused %zu. For %zu items, the windows propose no group. %zu groups that load "
            "the old item or the model would take a file that Hokuto can't read, and the windows don't offer them. The "
            "slowest join took %.2f s.\n",
            name.c_str(), totals.join_items, totals.join_groups, totals.join_loading, totals.join_beside,
            totals.join_copies, totals.join_links, totals.join_listed, totals.join_partial, totals.join_unlisting,
            totals.join_failed, totals.join_refused, totals.join_unproposed, totals.join_unreadable,
            totals.join_slowest);
    }
    std::printf(
        "%s: %zu new groups took items: %zu copies and %zu links, and %zu items listed. %zu new groups' files held no "
        "INFX block. %zu fail, and the edit refused %zu.\n",
        name.c_str(), totals.made_groups, totals.made_copies, totals.made_links, totals.made_listed,
        totals.made_unlisting, totals.made_failed, totals.made_refused);
}

} // namespace hokuto::tools::replace
