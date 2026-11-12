// SPDX-License-Identifier: MIT

// The checks of hokuto_check_layout on the runs of files, the SARC archives and the files of groups.

#include "check_layout.h"

namespace hokuto::tools::layout
{

namespace
{

// Checks that the group files in `changed` go out as an export writes them beside `archive`, an archive of `flavor` at
// `path` (format::GroupFileOutputs and song::PlaceGroupFiles). `sarcs` are the SARC archives beside the archive, and
// `sarc_of` gives the one that holds the game's copy of each group file in `group_files`, if any. That SARC archive has
// to hold the new group file in the copy's place, and keep its other files and their names. Another group file goes out
// by itself. One that the tool found by its group's label in `labels` goes out under that name
// (ntdlib::sar::SoundArchive::GroupFileGroups). When the archive's folder holds the game's file of that name only in
// its userManagementFiles folder, the new file goes there.
void CheckSarcs(const std::filesystem::path& path, const std::vector<hokuto::format::ChangedGroupFile>& changed,
                std::span<const uint8_t> archive, sar::Flavor flavor,
                const std::vector<hokuto::format::FileBeside>& sarcs,
                const std::vector<std::optional<std::size_t>>& sarc_of,
                const std::vector<std::vector<uint8_t>>& group_files, const std::vector<std::string>& labels,
                Totals& totals)
{
    const std::array<std::filesystem::path, 1> folder = {path.parent_path()};
    const std::vector<hokuto::format::FileBeside> outputs =
        hokuto::song::PlaceGroupFiles(hokuto::format::GroupFileOutputs(changed, archive, flavor, sarcs), folder);
    for (const hokuto::format::ChangedGroupFile& group_file : changed)
    {
        const std::string name = "group file " + std::to_string(group_file.index);
        const std::optional<std::size_t> s = sarc_of[group_file.index];
        const auto by_name = [&](const hokuto::format::FileBeside& output)
        {
            return s && output.first == sarcs[*s].first;
        };
        if (!s && std::ranges::none_of(outputs, [&](const auto& output) { return output.second == group_file.bytes; }))
        {
            Problem(path, "the export leaves " + name + " out", totals);
        }
        const std::string& label = labels.at(group_file.index);
        std::string file_name = sar::GroupFileName(seqlib::SafeFileName(label), flavor);
        const std::filesystem::path file = xsflib::PathFromUtf8(file_name);
        std::error_code error;
        const bool managed = !std::filesystem::exists(folder[0] / file, error) &&
                             std::filesystem::exists(folder[0] / hokuto::song::kUserManagementFolder / file, error);
        if (managed)
        {
            file_name = std::string(hokuto::song::kUserManagementFolder) + "/" + file_name;
        }
        const auto named = [&](const hokuto::format::FileBeside& output)
        {
            return output.first == file_name && output.second == group_file.bytes;
        };
        if (!s && !label.empty() && std::ranges::none_of(outputs, named))
        {
            Problem(path, "the export doesn't write " + name + " as " + file_name, totals);
        }
        else if (!s && !label.empty() && managed)
        {
            ++totals.managed_writes;
        }
        if (s && std::ranges::none_of(outputs, by_name))
        {
            Problem(path, "the export leaves " + name + " out of " + sarcs[*s].first, totals);
        }
    }

    // Each SARC archive that the export writes: the copies of the changed group files take their new bytes.
    for (const hokuto::format::FileBeside& output : outputs)
    {
        const auto sarc = std::ranges::find_if(sarcs, [&](const auto& each) { return each.first == output.first; });
        if (sarc == sarcs.end())
        {
            continue;
        }

        const std::vector<sar::SarcFile> was = sar::SarcFiles(sarc->second);
        const std::vector<sar::SarcFile> now = sar::SarcFiles(output.second);
        bool same = was.size() == now.size();
        for (std::size_t i = 0; same && i < was.size(); ++i)
        {
            std::span<const uint8_t> expected = was[i].data;
            for (const hokuto::format::ChangedGroupFile& group_file : changed)
            {
                const std::optional<std::size_t> s = sarc_of[group_file.index];
                if (s && sarcs[*s].first == output.first &&
                    std::ranges::equal(was[i].data, group_files[group_file.index]))
                {
                    expected = group_file.bytes;
                }
            }
            same = was[i].name == now[i].name && std::ranges::equal(now[i].data, expected);
        }
        if (!same)
        {
            Problem(path, output.first + " doesn't hold the new group files in the place of the game's copies", totals);
        }
        ++totals.sarc_writes;
    }
}

// Checks that GroupWith returns each group file inside `original`, an archive of `flavor` that `read` reads with the
// strict rules.
void CheckGroupsInArchive(const std::filesystem::path& path, const std::vector<uint8_t>& original,
                          const std::optional<sar::SoundArchive>& read, sar::Flavor flavor, Totals& totals)
{
    const xsflib::Reader r(original, flavor == sar::Flavor::kCafe);
    for (std::size_t g = 0; read && g < read->Groups().size(); ++g)
    {
        const uint32_t file = read->Groups()[g].file_id;
        const std::optional<std::size_t> offset =
            file == sar::kExternalGroupFile ? std::nullopt : read->FileOffset(file);
        if (!offset)
        {
            continue;
        }

        const std::size_t size = r.U32(*offset + 0x0C);
        const std::span<const uint8_t> bytes =
            std::span(original).subspan(*offset, std::min(size, original.size() - *offset));
        if (!std::ranges::equal(sar::GroupWith(bytes, flavor, {}, {}), bytes))
        {
            Problem(path, "GroupWith doesn't give the file of group " + read->Groups()[g].name + " back", totals);
        }
        ++totals.groups_inside;
    }
}

// The cut of `original` with `group_files`, or nothing when its pieces, sequences or fields differ from those of
// `layout`. Counts the sequences and the starts that only the group files hold.
std::optional<sar::Layout> CutWithGroupFiles(const std::filesystem::path& path, const std::vector<uint8_t>& original,
                                             const std::vector<std::vector<uint8_t>>& group_files,
                                             const sar::Layout& layout, const sar::Options& options, Totals& totals)
{
    const sar::Layout cut = sar::Cut(original, group_files, options);
    const auto group_only = [&layout](const sar::Field& field)
    {
        return field.from.sequence && field.to.sequence && field.from.piece >= layout.sequences.size();
    };
    std::vector<sar::Field> archive_fields;
    std::ranges::copy_if(cut.fields, std::back_inserter(archive_fields), std::not_fn(group_only));
    const bool prefix = cut.sequences.size() >= layout.sequences.size() &&
                        std::equal(layout.sequences.begin(), layout.sequences.end(), cut.sequences.begin());
    if (cut.pieces != layout.pieces || archive_fields != layout.fields || !prefix)
    {
        Problem(path, "the cut with the group files gives the archive other pieces, sequences or fields", totals);
        return std::nullopt;
    }
    totals.group_only_sequences += static_cast<int>(cut.sequences.size() - layout.sequences.size());
    totals.group_only_starts += static_cast<int>(cut.fields.size() - archive_fields.size());

    return cut;
}

// For each of `group_files`, the SARC archive of `sarcs` that contains it, if any. Checks that each SARC archive comes
// back with its files.
std::vector<std::optional<std::size_t>> CheckSarcGroups(const std::filesystem::path& path,
                                                        const std::vector<hokuto::format::FileBeside>& sarcs,
                                                        const std::vector<std::vector<uint8_t>>& group_files,
                                                        Totals& totals)
{
    std::vector<std::optional<std::size_t>> sarc_of(group_files.size());
    for (std::size_t s = 0; s < sarcs.size(); ++s)
    {
        for (const sar::SarcFile& file : sar::SarcFiles(sarcs[s].second))
        {
            const std::vector<uint8_t> same(file.data.begin(), file.data.end());
            if (sar::WithSarcFile(sarcs[s].second, file.data, same) != sarcs[s].second)
            {
                Problem(path, sarcs[s].first + " doesn't come back with its files", totals);
            }
            for (std::size_t k = 0; k < group_files.size(); ++k)
            {
                if (!sarc_of[k] && std::ranges::equal(file.data, group_files[k]))
                {
                    sarc_of[k] = s;
                }
            }
        }
        ++totals.sarcs;
    }

    return sarc_of;
}

// What CheckLongerCopy reads of the group files beside an archive: the cut of the archive with them, the sequence files
// of the archive, the group files, the group of each that the import finds and its name, the SARC archives beside the
// archive and the one that contains each group file, and the labels of the groups.
struct GroupFilesBeside
{
    const sar::Layout& cut;
    const std::set<uint32_t>& sequence_files;
    const std::vector<std::vector<uint8_t>>& group_files;
    const std::vector<std::optional<uint32_t>>& group_of;
    const std::vector<std::string>& group_name;
    const std::vector<hokuto::format::FileBeside>& sarcs;
    const std::vector<std::optional<std::size_t>>& sarc_of;
    const std::vector<std::string>& labels;
};

// Checks that `member`, a file of which group file `i` contains a copy, named `name`, reads back with 40 more bytes in
// every copy after ReplaceFile, and that the export writes the changed group files beside the archive.
void CheckLongerCopy(const std::filesystem::path& path, const GroupFilesBeside& beside, std::size_t i,
                     const sar::GroupMember& member, const std::string& name, const sar::Options& options,
                     Totals& totals)
{
    const sar::Flavor flavor = options.flavor;

    // A copy that is a piece of the archive's sequence isn't a run.
    const std::string what = "file " + std::to_string(member.file) + " of " + name;
    std::vector<uint8_t> longer = *member.copy;
    longer.resize(longer.size() + 40, 0x5A);
    sar::ReplacedPieces replaced;
    try
    {
        replaced = sar::ReplaceFile(flavor, beside.cut.sequences, beside.cut.pieces, beside.cut.fields,
                                    beside.cut.group_files, member.file, longer);
    }
    catch (const std::invalid_argument& error)
    {
        if (!beside.sequence_files.contains(member.file) ||
            !std::string_view(error.what()).ends_with("isn't a piece of its own"))
        {
            throw;
        }
        ++totals.group_sequences;
        return;
    }
    std::vector<sar::GroupFileLayout> after = beside.cut.group_files;
    const std::vector<uint8_t> joined = sar::Join(flavor, beside.cut.sequences, replaced.pieces, beside.cut.fields);
    sar::SoundArchive check = sar::SoundArchive::Load(joined, options);
    std::vector<hokuto::format::ChangedGroupFile> changed;
    for (std::size_t k = 0; k < after.size(); ++k)
    {
        after[k].pieces = replaced.group_files.at(k);
        std::vector<uint8_t> group_file = sar::Join(flavor, beside.cut.sequences, after[k].pieces, after[k].fields);
        if (group_file != beside.group_files[k])
        {
            changed.push_back({k, beside.group_of[k], beside.group_name[k], group_file});
        }
        check.AddGroupFile(std::move(group_file));
    }
    CheckSarcs(path, changed, joined, flavor, beside.sarcs, beside.sarc_of, beside.group_files, beside.labels, totals);
    if (!std::ranges::equal(check.FileData(member.file), longer))
    {
        Problem(path, what + " doesn't read back with longer bytes", totals);
    }

    // The group file in hand contains a copy, and the list of copies has to name it.
    bool in_group_file = false;
    for (const sar::FileCopy& copy :
         sar::FileCopies(flavor, beside.cut.sequences, replaced.pieces, beside.cut.fields, after, member.file))
    {
        if (copy.bytes != longer)
        {
            Problem(path, what + " keeps a copy without the longer bytes", totals);
        }
        in_group_file = in_group_file || copy.group_file == i;
        ++totals.group_copies;
    }
    if (!in_group_file)
    {
        Problem(path, what + " has no copy in its group file after the edit", totals);
    }
    ++totals.group_replaced;
}

} // namespace

void CheckGroupFiles(const std::filesystem::path& path, const std::vector<uint8_t>& original, const sar::Layout& layout,
                     const sar::Options& options, Totals& totals)
{
    const sar::Flavor flavor = options.flavor;

    // Each group's file in the archive, at the size that the header gives. A group's file that ends the archive can end
    // without the zeros that pad INFX.
    sar::Options strict = options;
    strict.rules = sar::Rules::kStopAtFirstProblem;
    std::optional<sar::SoundArchive> read;
    try
    {
        read = sar::SoundArchive::Load(original, strict);
    }
    catch (const std::exception&)
    {
        read.reset();
    }
    std::set<uint32_t> sequence_files;
    for (std::size_t i = 0; read && i < read->Sounds().size(); ++i)
    {
        if (read->Sounds()[i].type == sar::SoundType::kSequence)
        {
            sequence_files.insert(read->Sounds()[i].file_id);
        }
    }

    CheckGroupsInArchive(path, original, read, flavor, totals);

    // The group files beside the archive, cut with it.
    const std::shared_ptr<const sar::SoundArchive> opened = hokuto::song::OpenArchive(path);
    const std::vector<std::vector<uint8_t>>& group_files = opened->GroupFiles();
    if (group_files.empty())
    {
        return;
    }
    ++totals.group_archives;

    // A sequence file that only the group files hold becomes a sequence after the archive's. The fields of its sounds'
    // starts name items of it.
    const std::optional<sar::Layout> cut_with_groups =
        CutWithGroupFiles(path, original, group_files, layout, options, totals);
    if (!cut_with_groups)
    {
        return;
    }
    const sar::Layout& cut = *cut_with_groups;

    // The SARC archives beside the archive, and the one that holds each group file. Each SARC archive comes back with
    // its files.
    const std::array<std::filesystem::path, 1> folder = {path.parent_path()};
    const std::vector<hokuto::format::FileBeside> sarcs = hokuto::song::SarcArchivesIn(folder);
    const std::vector<std::optional<std::size_t>> sarc_of = CheckSarcGroups(path, sarcs, group_files, totals);

    // Each group file's group as the program's import finds it, and the group's name.
    const std::vector<std::string>& labels = opened->GroupFileGroups();
    const hokuto::format::File imported = hokuto::format::ImportArchive(original, flavor, group_files, {}, labels);
    std::vector<std::optional<uint32_t>> group_of(group_files.size());
    std::vector<std::string> group_name(group_files.size());
    for (std::size_t k = 0; k < group_files.size(); ++k)
    {
        group_of[k] = imported.native->group_files.at(k).group;
        if (group_of[k] && read)
        {
            group_name[k] = read->Groups().at(*group_of[k]).name;
        }
    }

    const GroupFilesBeside beside{.cut = cut,
                                  .sequence_files = sequence_files,
                                  .group_files = group_files,
                                  .group_of = group_of,
                                  .group_name = group_name,
                                  .sarcs = sarcs,
                                  .sarc_of = sarc_of,
                                  .labels = labels};
    for (std::size_t i = 0; i < group_files.size(); ++i)
    {
        const std::string name = "group file " + std::to_string(i);
        const sar::GroupFileLayout& group = cut.group_files.at(i);
        if (sar::Join(flavor, cut.sequences, group.pieces, group.fields) != group_files[i])
        {
            Problem(path, name + " doesn't join back", totals);
            continue;
        }
        const std::optional<uint32_t> found = group_of[i];
        if (!found || (read && read->Groups().at(*found).file_id != sar::kExternalGroupFile))
        {
            Problem(path, "the import finds no group beside the archive for " + name, totals);
        }
        if (sar::GroupWith(group_files[i], flavor, {}, {}) != group_files[i])
        {
            Problem(path, "GroupWith doesn't give " + name + " back", totals);
        }
        ++totals.group_files;

        // Each file whose copy the group file holds takes 40 more bytes, in every copy.
        for (const sar::GroupMember& member : sar::ReadGroupFile(group_files[i], flavor).files)
        {
            if (!member.copy)
            {
                continue;
            }

            CheckLongerCopy(path, beside, i, member, name, options, totals);
        }
    }
}

namespace
{

// Checks that each file of `before` in one of `runs`, the pieces of `layout` that grew, gained its bytes in `after`,
// and that every other file keeps its bytes, but for a group that holds a run. Returns the files in runs.
std::vector<uint32_t> CheckGrownFiles(const std::filesystem::path& path, const sar::SoundArchive& before,
                                      const sar::SoundArchive& after, const sar::Layout& layout,
                                      const std::set<std::size_t>& runs, Totals& totals)
{
    const std::map<std::size_t, std::size_t> starts = PieceStarts(layout);
    std::vector<uint32_t> in_runs;
    for (uint32_t id = 0; id < before.FileCount(); ++id)
    {
        const std::span<const uint8_t> data = before.FileData(id);
        const std::optional<std::size_t> offset = before.FileOffset(id);
        if (data.empty() || !offset)
        {
            continue;
        }

        const auto found = starts.find(*offset);
        const bool run = found != starts.end() && runs.contains(found->second);
        auto holds_run = [&](const auto& start)
        {
            return runs.contains(start.second);
        };
        if (!run && std::any_of(starts.upper_bound(*offset), starts.lower_bound(*offset + data.size()), holds_run))
        {
            ++totals.run_groups;
            continue;
        }

        std::vector<uint8_t> expected(data.begin(), data.end());
        std::pair<int, int>& kind = totals.kinds[KindOf(data)];
        ++kind.second;
        if (run)
        {
            const std::vector<uint8_t> gain = Gain(data.size());
            expected.insert(expected.end(), gain.begin(), gain.end());
            in_runs.push_back(id);
            ++totals.run_files;
            ++kind.first;
        }
        if (!std::ranges::equal(after.FileData(id), expected))
        {
            Problem(path, "file " + std::to_string(id) + " differs after the files grow", totals);
        }
    }

    return in_runs;
}

// Checks ReplaceFile in every copy of the first, the middle and the last file of `in_runs`, files of `before` in runs
// of `layout`.
void CheckReplaceInRuns(const std::filesystem::path& path, const sar::SoundArchive& before, const sar::Layout& layout,
                        const std::vector<uint32_t>& in_runs, const sar::Options& options, Totals& totals)
{
    for (const uint32_t id : std::set<uint32_t>{in_runs.front(), in_runs[in_runs.size() / 2], in_runs.back()})
    {
        const std::span<const uint8_t> data = before.FileData(id);
        std::vector<uint8_t> bytes(data.begin(), data.end());
        bytes.resize(bytes.size() + 40, 0x5A);
        const std::vector<sar::Piece> pieces =
            sar::ReplaceFile(layout.flavor, layout.sequences, layout.pieces, layout.fields, id, bytes);
        const sar::SoundArchive replaced =
            sar::SoundArchive::Load(sar::Join(layout.flavor, layout.sequences, pieces, layout.fields), options);
        ++totals.replaced;
        if (!std::ranges::equal(replaced.FileData(id), bytes))
        {
            Problem(path, "file " + std::to_string(id) + " lacks its new bytes", totals);
        }

        // Every copy of the file, in the FILE block and in the groups of the archive, gets the new bytes.
        const std::vector<sar::FileCopy> copies =
            sar::FileCopies(layout.flavor, layout.sequences, pieces, layout.fields, id);
        if (copies.empty())
        {
            Problem(path, "file " + std::to_string(id) + " has no copy after ReplaceFile", totals);
        }
        for (const sar::FileCopy& copy : copies)
        {
            if (copy.bytes != bytes)
            {
                Problem(path, "file " + std::to_string(id) + " keeps a copy without its new bytes", totals);
            }
        }
    }
}

} // namespace

void CheckRuns(const std::filesystem::path& path, const std::vector<uint8_t>& original, const sar::Layout& layout,
               const sar::Options& options, Totals& totals)
{
    // A run that padding follows holds a file.
    sar::Layout grown = layout;
    std::set<std::size_t> runs;
    for (std::size_t i = 0; i + 1 < grown.pieces.size(); ++i)
    {
        auto* run = std::get_if<std::vector<uint8_t>>(&grown.pieces[i]);
        if (run && std::holds_alternative<sar::Padding>(grown.pieces[i + 1]))
        {
            const std::vector<uint8_t> gain = Gain(run->size());
            run->insert(run->end(), gain.begin(), gain.end());
            runs.insert(i);
        }
    }
    totals.runs += static_cast<int>(runs.size());
    const std::vector<uint8_t> joined = sar::Join(grown.flavor, grown.sequences, grown.pieces, grown.fields);
    const sar::SoundArchive before = sar::SoundArchive::Load(original, options);
    const sar::SoundArchive after = sar::SoundArchive::Load(joined, options);
    if (after.GroupErrors().size() != before.GroupErrors().size() || after.Truncated() != before.Truncated() ||
        after.FileCount() != before.FileCount() || after.Sounds().size() != before.Sounds().size())
    {
        Problem(path, "the archive with longer files reads otherwise", totals);
        return;
    }

    // Each file in a run gained its bytes. Every other file keeps its bytes, but for a group that holds a run.
    const std::vector<uint32_t> in_runs = CheckGrownFiles(path, before, after, layout, runs, totals);

    for (std::size_t i = 0; i < before.Sounds().size(); ++i)
    {
        if (after.Sounds()[i].start_offset != before.Sounds()[i].start_offset)
        {
            Problem(path, "sound " + std::to_string(i) + " moved its start after the files grow", totals);
        }
    }

    // The cut of the archive with longer files. Its runs between the files hold the fields' new values. Its sequences,
    // sequence pieces and padding don't move.
    const sar::Layout cut = sar::Cut(joined, options);
    bool same =
        cut.pieces.size() == grown.pieces.size() && cut.fields == grown.fields && cut.sequences == grown.sequences;
    for (std::size_t i = 0; same && i < cut.pieces.size(); ++i)
    {
        const bool whole = runs.contains(i) || !std::holds_alternative<std::vector<uint8_t>>(grown.pieces[i]);
        same = cut.pieces[i].index() == grown.pieces[i].index() && (!whole || cut.pieces[i] == grown.pieces[i]);
    }
    if (!same)
    {
        Problem(path, "the cut of the archive with longer files has other pieces or fields", totals);
    }

    // ReplaceFile, in every copy of the first, the middle and the last file in a run.
    if (in_runs.empty())
    {
        return;
    }

    CheckReplaceInRuns(path, before, layout, in_runs, options, totals);
}

} // namespace hokuto::tools::layout
