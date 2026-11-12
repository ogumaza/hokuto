// SPDX-License-Identifier: MIT

// The helpers that the modes of hokuto_check_replace share (check_replace.h).

#include "check_replace.h"
#include "loop_passes.h"

namespace hokuto::tools::replace
{

std::vector<Group> GroupsOf(std::span<const uint8_t> bytes, sar::Flavor flavor)
{
    sar::Options options;
    options.flavor = flavor;
    options.rules = sar::Rules::kStopAtFirstProblem;
    const sar::SoundArchive strict = sar::SoundArchive::Load(std::vector<uint8_t>(bytes.begin(), bytes.end()), options);
    std::vector<Group> groups;
    for (std::size_t group = 0; group < strict.Groups().size(); group++)
    {
        groups.push_back({strict.Groups()[group].file_id, strict.GroupFileIds(group)});
    }

    return groups;
}

namespace
{

// Whether `group`, a group's file of `flavor`, holds an INFX block: its header names a third block. The 3DS's version
// 1.0.0.0 holds none.
bool Listing(std::span<const uint8_t> group, sar::Flavor flavor)
{
    return xsflib::Reader(group, flavor == sar::Flavor::kCafe).U16(0x10) > 2;
}

} // namespace

StrictRead ReadStrictly(const format::File& file)
{
    StrictRead read;
    read.bytes = format::ExportArchive(file);
    read.group_files = format::ExportGroupFiles(file);
    sar::Options options;
    options.flavor = FlavorOf(*file.native);
    options.rules = sar::Rules::kStopAtFirstProblem;
    read.archive = sar::SoundArchive::Load(read.bytes, options);
    for (const std::vector<uint8_t>& group_file : read.group_files)
    {
        read.archive->AddGroupFile(group_file);
    }
    for (uint32_t g = 0; g < read.archive->Groups().size(); g++)
    {
        std::optional<sar::GroupContents>& contents = read.groups.emplace_back();
        const uint32_t file_id = read.archive->Groups()[g].file_id;
        std::span<const uint8_t> group;
        if (file_id != sar::kExternalGroupFile)
        {
            group = read.archive->FileData(file_id);
        }
        for (std::size_t k = 0; k < read.group_files.size() && file_id == sar::kExternalGroupFile; k++)
        {
            if (file.native->group_files[k].group == g)
            {
                group = read.group_files[k];
            }
        }
        try
        {
            contents = sar::ReadGroupFile(group, options.flavor);
        }
        catch (const std::exception&)
        {
            contents.reset();
        }
        read.listing.push_back(contents && Listing(group, options.flavor));
    }

    return read;
}

std::vector<std::optional<std::vector<uint32_t>>> ListsOfGroups(std::span<const uint8_t> bytes, sar::Flavor flavor)
{
    std::vector<std::optional<std::vector<uint32_t>>> lists;
    if (flavor == sar::Flavor::kCtr)
    {
        return lists;
    }
    const sar::InfoBlock info = sar::InfoBlock::Read(sar::TablesOf(bytes, flavor).info, flavor);
    for (const std::vector<uint8_t>& entry : info.files)
    {
        std::optional<std::vector<uint32_t>>& list = lists.emplace_back();
        const xsflib::Reader r(entry, flavor == sar::Flavor::kCafe);
        if (r.U16(0) != kInternalFile)
        {
            continue;
        }

        // The list follows the file's place: a reference to the bytes, their size and a reference to the list.
        const std::size_t location = r.U32(4);
        const std::size_t at = location + r.U32(location + 16);
        list.emplace();
        for (uint32_t k = 0; k < r.U32(at); k++)
        {
            list->push_back(r.U32(at + 4 + std::size_t{k} * 4));
        }
    }

    return lists;
}

std::string HexOf(uint32_t id)
{
    char text[16];
    std::snprintf(text, sizeof(text), "0x%08X", id);

    return text;
}

namespace
{

// What is wrong with `now`, a group's copy of a file after `bytes` took the place of `present`, the file that the
// archive's file table names. The copy held `old`. A copy that held `present`'s bytes, or other bytes besides those of
// its table of waves, has to hold `bytes`. A copy of a bank file or a wave sound file that named `present`'s waves at
// other places has to hold `bytes` with the old copy's places: each wave of `bytes` that `present` names at the place
// where the old copy named it, and each other wave at its place in `bytes`. Such a copy counts in `placed`. A wave of
// `present` with two places in the old copy makes the copy hold `bytes`.
std::string CopyProblem(std::span<const uint8_t> now, std::span<const uint8_t> old, std::span<const uint8_t> present,
                        std::span<const uint8_t> bytes, sar::Flavor flavor, std::size_t& placed)
{
    std::vector<sar::WaveId> was;
    std::vector<sar::WaveId> old_places;
    bool table = false;
    try
    {
        was = sar::WaveTable(present, flavor);
        old_places = sar::WaveTable(old, flavor);
        table = !std::ranges::equal(old, present) && was.size() == old_places.size() &&
                std::ranges::equal(sar::WithWaveTable(present, flavor, old_places), old);
    }
    catch (const std::exception&)
    {
        // A wave archive, a sequence file, or a file whose table ntdlib can't read.
    }
    for (std::size_t i = 0; table && i < was.size(); i++)
    {
        for (std::size_t j = 0; j < i; j++)
        {
            table = table && (was[j] != was[i] || old_places[j] == old_places[i]);
        }
    }
    if (!table)
    {
        return std::ranges::equal(now, bytes) ? "" : "doesn't hold the new bytes";
    }

    placed++;
    const std::vector<sar::WaveId> waves = sar::WaveTable(bytes, flavor);
    const std::vector<sar::WaveId> places = sar::WaveTable(now, flavor);
    if (places.size() != waves.size() || !std::ranges::equal(sar::WithWaveTable(now, flavor, waves), bytes))
    {
        return "doesn't hold the new bytes besides its table of waves";
    }
    for (std::size_t i = 0; i < waves.size(); i++)
    {
        const auto named = std::ranges::find(was, waves[i]);
        const sar::WaveId expected =
            named != was.end() ? old_places[static_cast<std::size_t>(named - was.begin())] : waves[i];
        if (places[i] != expected)
        {
            return "names wave " + std::to_string(i) + " at another place than the old copy";
        }
    }

    return {};
}

} // namespace

std::string GroupFileProblem(std::span<const uint8_t> was, std::span<const uint8_t> now, sar::Flavor flavor,
                             uint32_t file_id, std::span<const uint8_t> present, std::span<const uint8_t> bytes,
                             const std::string& name, std::size_t& placed)
{
    const sar::GroupContents after = sar::ReadGroupFile(now, flavor);
    const sar::GroupContents before = sar::ReadGroupFile(was, flavor);
    if (after.items != before.items || after.files.size() != before.files.size())
    {
        return "changes the lists of " + name;
    }
    for (std::size_t m = 0; m < after.files.size(); m++)
    {
        const sar::GroupMember& member = before.files[m];
        if (after.files[m].file != member.file || after.files[m].copy.has_value() != member.copy.has_value())
        {
            return "changes the copy of file " + std::to_string(member.file) + " in " + name;
        }
        if (member.copy && member.file == file_id)
        {
            const std::string problem = CopyProblem(*after.files[m].copy, *member.copy, present, bytes, flavor, placed);
            if (!problem.empty())
            {
                return "has a copy in " + name + " that " + problem;
            }
        }
        else if (after.files[m].copy != member.copy)
        {
            return "changes the copy of file " + std::to_string(member.file) + " in " + name;
        }
    }

    return {};
}

std::string GroupFilesProblem(const sar::SoundArchive& built, const format::File& replaced, uint32_t file_id,
                              std::span<const uint8_t> bytes, std::size_t& placed)
{
    const sar::Flavor flavor = built.ReadOptions().flavor;
    const std::vector<std::vector<uint8_t>> group_files = format::ExportGroupFiles(replaced);
    if (group_files.size() != built.GroupFiles().size())
    {
        return "loses a group file";
    }
    for (std::size_t g = 0; g < group_files.size(); g++)
    {
        const std::string problem =
            GroupFileProblem(built.GroupFiles()[g], group_files[g], flavor, file_id, built.FileData(file_id), bytes,
                             "group file " + std::to_string(g), placed);
        if (!problem.empty())
        {
            return problem;
        }
    }

    return {};
}

Source SourceOf(std::span<const uint8_t> bytes, uint32_t start_offset, const std::optional<song::Console>& into)
{
    Source source{edit::ReadIncoming(bytes, into), std::nullopt};
    source.start =
        edit::ItemAtOffset(edit::Offsets(source.incoming.sequence,
                                         song::Console{.engine = hokuto::format::EngineOf(source.incoming.flavor)}),
                           start_offset);

    return source;
}

bool Untracked(const Source& source, const sar::SoundInfo& info)
{
    return (edit::TracksOpened(source.incoming.sequence, *source.start) & ~info.allocate_track_flags) != 0;
}

song::Sequence Placeless(song::Sequence played)
{
    played.name.clear();
    played.sound = 0;
    played.labels.clear();
    played.calls.clear();
    for (song::Track& track : played.tracks)
    {
        track.stretches.clear();
        for (song::Note& note : track.notes)
        {
            note.command = 0;
            note.stretch = 0;
        }
    }

    return played;
}

song::Sequence Played(const sar::SoundArchive& archive, uint32_t sound, const song::StartValues& values)
{
    return Placeless(song::ReadSequence(archive, sound, values));
}

bool StartsAtLabel(const sar::SoundArchive& archive, uint32_t sound)
{
    const sar::SoundInfo& info = archive.Sounds()[sound];
    const std::vector<sar::Sequence::Label> labels = sar::SequenceFile::Read(archive.FileData(info.file_id)).labels;
    const auto at_start = [&info](const sar::Sequence::Label& label)
    {
        return label.offset == info.start_offset;
    };

    return std::ranges::any_of(labels, at_start);
}

bool SameUpToLoop(const song::Sequence& before, const song::Sequence& after)
{
    const std::optional<uint32_t> earlier = EarlierEnd(before, after);
    if (!earlier)
    {
        return false;
    }

    const uint32_t end = *earlier;
    const auto starts = [end](const song::Sequence& sequence)
    {
        std::vector<std::tuple<int, uint32_t, uint8_t, uint8_t, uint8_t, uint32_t>> notes;
        for (const song::Track& track : sequence.tracks)
        {
            for (const song::Note& note : track.notes)
            {
                if (note.start < end)
                {
                    notes.emplace_back(track.number, note.start, note.key, note.velocity, note.bank_slot, note.program);
                }
            }
        }
        std::ranges::sort(notes);

        return notes;
    };

    return starts(after) == starts(before);
}

std::vector<FileItem> FileItemsOf(const sar::SoundArchive& archive)
{
    std::vector<FileItem> items;
    std::set<std::pair<edit::FileKind, uint32_t>> seen;
    const auto add = [&](edit::FileKind kind, std::size_t item, uint32_t file_id)
    {
        if (seen.insert({kind, file_id}).second)
        {
            items.push_back({kind, static_cast<uint32_t>(item), file_id});
        }
    };
    for (std::size_t i = 0; i < archive.Banks().size(); i++)
    {
        add(edit::FileKind::kBank, i, archive.Banks()[i].file_id);
    }
    for (std::size_t i = 0; i < archive.WaveArchives().size(); i++)
    {
        add(edit::FileKind::kWaveArchive, i, archive.WaveArchives()[i].file_id);
    }
    for (std::size_t i = 0; i < archive.Sounds().size(); i++)
    {
        if (archive.Sounds()[i].type == sar::SoundType::kWaveSound)
        {
            add(edit::FileKind::kWaveSoundFile, i, archive.Sounds()[i].file_id);
        }
    }

    return items;
}

std::string ReplacedProblem(const sar::SoundArchive& built, std::span<const Group> groups, const FileItem& item,
                            std::span<const uint8_t> bytes, const format::File& replaced, Archive& after,
                            std::size_t* counted)
{
    hokuto::tools::CheckSaved(replaced, true);
    std::size_t uncounted = 0;
    std::size_t& placed = counted ? *counted : uncounted;
    after = song::LoadArchive(format::ExportArchive(replaced), "after.bfsar", format::ExportGroupFiles(replaced));
    if (!std::ranges::equal(after->FileData(item.file_id), bytes))
    {
        return "doesn't hold the new bytes";
    }

    // Each copy in a group's file inside the archive holds what CopyProblem requires.
    const sar::Flavor flavor = built.ReadOptions().flavor;
    for (const Group& group : groups)
    {
        if (std::ranges::find(group.loads, item.file_id) == group.loads.end() || built.FileData(group.file_id).empty())
        {
            continue;
        }

        const std::string problem =
            GroupFileProblem(built.FileData(group.file_id), after->FileData(group.file_id), flavor, item.file_id,
                             built.FileData(item.file_id), bytes, "group " + HexOf(group.file_id), placed);
        if (!problem.empty())
        {
            return problem;
        }
    }

    // The other files keep their bytes, and the sounds their starts. A group's file can hold a copy of the file.
    std::vector<uint32_t> copies = {item.file_id};
    for (const Group& group : groups)
    {
        if (std::ranges::find(group.loads, item.file_id) != group.loads.end())
        {
            copies.push_back(group.file_id);
        }
    }
    for (uint32_t id = 0; id < built.FileCount(); id++)
    {
        if (std::ranges::find(copies, id) == copies.end() &&
            !std::ranges::equal(built.FileData(id), after->FileData(id)))
        {
            return "changes file " + std::to_string(id);
        }
    }
    for (std::size_t i = 0; i < built.Sounds().size(); i++)
    {
        if (after->Sounds()[i].start_offset != built.Sounds()[i].start_offset)
        {
            return "moves the start of " + built.Sounds()[i].name;
        }
    }

    // Each copy of the file in the group files beside the archive holds the new bytes, with the old copy's places where
    // it named the file's waves at other places.
    return GroupFilesProblem(built, replaced, item.file_id, bytes, placed);
}

std::string FileProblem(const format::File& file, const sar::SoundArchive& built, std::span<const Group> groups,
                        const FileItem& item, std::span<const uint8_t> bytes, Archive& after, std::size_t* placed)
{
    // The Replace window reads the file with the versions of the present file, and finds what it changes. Then the
    // replacement.
    const edit::IncomingFile incoming = edit::ReadIncomingFile(bytes, item.kind, built.ReadOptions().flavor,
                                                               edit::IncomingVersions(built, item.kind, item.item));
    edit::ChangesOf(file, built, item.item, incoming);
    const format::File replaced = edit::ReplaceFile(file, item.file_id, incoming.bytes);
    return ReplacedProblem(built, groups, item, incoming.bytes, replaced, after, placed);
}

std::string NameOf(const sar::SoundArchive& archive, const FileItem& item)
{
    switch (item.kind)
    {
    case edit::FileKind::kBank:
        return archive.Banks()[item.item].name;
    case edit::FileKind::kWaveArchive:
        return archive.WaveArchives()[item.item].name;
    case edit::FileKind::kWaveSoundFile:
        break;
    }

    return "the wave sound file of " + archive.Sounds()[item.item].name;
}

bool SharedFile(const sar::SoundArchive& archive, uint32_t file_id)
{
    const std::optional<std::size_t> offset = archive.FileOffset(file_id);
    for (uint32_t id = 0; id < archive.FileCount() && offset; id++)
    {
        if (id != file_id && archive.FileOffset(id) == offset &&
            archive.FileData(id).size() == archive.FileData(file_id).size())
        {
            return true;
        }
    }

    return false;
}

bool Outside(const sar::SoundArchive& archive, uint32_t file_id)
{
    return archive.FileData(file_id).empty();
}

bool Beside(const sar::SoundArchive& archive, uint32_t file_id)
{
    return !Outside(archive, file_id) && !archive.FileOffset(file_id);
}

song::Item SongItemOf(const FileItem& item)
{
    switch (item.kind)
    {
    case edit::FileKind::kBank:
        return {song::ItemKind::kBank, item.item, 0};
    case edit::FileKind::kWaveArchive:
        return {song::ItemKind::kWaveArchive, item.item, 0};
    case edit::FileKind::kWaveSoundFile:
        break;
    }

    return {song::ItemKind::kWaveSound, item.item, 0};
}

uint32_t ItemIdOf(const FileItem& item)
{
    switch (item.kind)
    {
    case edit::FileKind::kBank:
        return sar::ItemId(sar::kBankItem, item.item);
    case edit::FileKind::kWaveArchive:
        return sar::ItemId(sar::kWaveArchiveItem, item.item);
    case edit::FileKind::kWaveSoundFile:
        break;
    }

    return sar::ItemId(sar::kSoundItem, item.item);
}

uint32_t HeaderVersion(std::span<const uint8_t> file, sar::Flavor flavor)
{
    return xsflib::Reader(file, flavor == sar::Flavor::kCafe).U32(0x08);
}

std::string OtherItemsProblem(const sar::SoundArchive& built, const sar::SoundArchive& after, bool new_banks,
                              bool wave_sound_indices)
{
    const auto same_item = [](const auto& a, const auto& b)
    {
        return a.name == b.name && a.file_id == b.file_id;
    };
    for (uint32_t i = 0; i < built.Sounds().size(); i++)
    {
        const sar::SoundInfo& old = built.Sounds()[i];
        const sar::SoundInfo& now = after.Sounds()[i];
        if (now.name != old.name || now.file_id != old.file_id || now.start_offset != old.start_offset ||
            now.allocate_track_flags != old.allocate_track_flags ||
            (wave_sound_indices && now.wave_sound_index != old.wave_sound_index) || !SameValues(now, old))
        {
            return "changes " + old.name;
        }
    }
    const auto after_banks =
        new_banks ? std::span(after.Banks()).first(built.Banks().size()) : std::span(after.Banks());
    if (!std::ranges::equal(built.Banks(), after_banks, same_item) ||
        !std::ranges::equal(built.WaveArchives(), std::span(after.WaveArchives()).first(built.WaveArchives().size()),
                            same_item))
    {
        return "changes the archive's banks or wave archives";
    }
    for (uint32_t id = 0; id < built.FileCount(); id++)
    {
        if (!std::ranges::equal(built.FileData(id), after.FileData(id)))
        {
            return "changes file " + std::to_string(id);
        }
    }

    return {};
}

} // namespace hokuto::tools::replace
