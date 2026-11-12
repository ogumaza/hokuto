// SPDX-License-Identifier: MIT

#include "format/archive.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "ntdlib/dse/layout.h"
#include "ntdlib/dse/sets.h"
#include "ntdlib/mp2k/commands.h"
#include "ntdlib/mp2k/driver.h"
#include "ntdlib/mp2k/layout.h"
#include "ntdlib/ntr/snd/commands.h"
#include "ntdlib/rsar/layout.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sar/groups.h"
#include "ntdlib/sar/layout.h"
#include "ntdlib/sar/sarc.h"
#include "ntdlib/sar/tables.h"
#include "ntdlib/sdat/cut.h"
#include "ntdlib/sdat/formats.h"
#include "seqlib/file_name.h"
#include "xsflib/binary.h"
#include "xsflib/bytes.h"
#include "xsflib/sha1.h"

#include "format/archive_parts.h"
#include "format/codes.h"
#include "format/drivers.h"
#include "format/file.h"
#include "format/kinds.h"
#include "format/text.h"

namespace hokuto::format
{
namespace archive_parts
{

// What ntdlib reads of `archive`, an archive of `flavor`, when it stops at the first problem. Nothing if it finds one.
std::optional<ntdlib::sar::SoundArchive> ReadStrictly(std::span<const uint8_t> archive, ntdlib::sar::Flavor flavor)
{
    ntdlib::sar::Options options;
    options.flavor = flavor;
    options.rules = ntdlib::sar::Rules::kStopAtFirstProblem;
    try
    {
        return ntdlib::sar::SoundArchive::Load(std::vector<uint8_t>(archive.begin(), archive.end()), options);
    }
    catch (const xsflib::FormatError&)
    {
        return std::nullopt;
    }
}

} // namespace archive_parts

namespace
{

using archive_parts::FirstRun;
using archive_parts::ReadStrictly;
using archive_parts::ReadWiiTables;
using archive_parts::TablesOf;
using archive_parts::WriteTables;
using archive_parts::WriteWiiStreamSizes;
using archive_parts::WriteWiiTables;

// The size in bytes of the files that each group of `archive` loads, in the order of its group table. The game loads
// each of a group's files, whether the group's file holds a copy or links to the archive's. A group outside the archive
// counts the files of its group file in `beside`, by the group's index, and none without one. Nothing when ntdlib can't
// read a group's list of files.
std::optional<std::vector<uint64_t>> GroupLoads(const ntdlib::sar::SoundArchive& archive,
                                                const std::map<uint32_t, std::span<const uint8_t>>& beside)
{
    std::vector<uint64_t> loads;
    try
    {
        for (std::size_t group = 0; group < archive.Groups().size(); ++group)
        {
            uint64_t bytes = 0;
            const auto group_file = beside.find(static_cast<uint32_t>(group));
            if (archive.Groups()[group].file_id == ntdlib::sar::kExternalGroupFile && group_file != beside.end())
            {
                const ntdlib::sar::Flavor flavor = archive.ReadOptions().flavor;
                for (const ntdlib::sar::GroupMember& member :
                     ntdlib::sar::ReadGroupFile(group_file->second, flavor).files)
                {
                    bytes += member.copy ? member.copy->size() : archive.FileData(member.file).size();
                }
            }

            for (const uint32_t id : archive.GroupFileIds(group))
            {
                bytes += archive.FileData(id).size();
            }

            loads.push_back(bytes);
        }
    }
    catch (const xsflib::FormatError&)
    {
        return std::nullopt;
    }

    return loads;
}

// The index of the group of `archive` that the game keeps outside it with label `label`: its name, or GROUP_<index> for
// a group without one (ntdlib::sar::SoundArchive::ExternalGroups). Nothing when no such group has the label.
std::optional<uint32_t> GroupLabelled(const ntdlib::sar::SoundArchive& archive, const std::string& label)
{
    const std::vector<ntdlib::sar::GroupInfo>& groups = archive.Groups();
    for (uint32_t group = 0; group < groups.size(); ++group)
    {
        const std::string name = groups[group].name.empty() ? "GROUP_" + std::to_string(group) : groups[group].name;
        if (groups[group].file_id == ntdlib::sar::kExternalGroupFile && name == label)
        {
            return group;
        }
    }

    return std::nullopt;
}

// The group files of `beside` as spans of their bytes.
std::map<uint32_t, std::span<const uint8_t>> Spans(const std::map<uint32_t, std::vector<uint8_t>>& beside)
{
    std::map<uint32_t, std::span<const uint8_t>> spans;
    for (const auto& [group, bytes] : beside)
    {
        spans.emplace(group, bytes);
    }

    return spans;
}

// The bytes of `pieces` with `fields`, joined as ntdlib joins an archive of `native`'s kind, with ntdlib's message as a
// sentence for the user.
std::vector<uint8_t> JoinPieces(const NativeArchive& native, std::span<const ntdlib::commands::Sequence> sequences,
                                std::span<const ntdlib::sar::Piece> pieces, std::span<const ntdlib::sar::Field> fields)
{
    const ntdlib::mp2k::JoinOptions mp2k_options{.filler = native.gba_filler,
                                                 .note_arguments = native.gba_note_arguments};
    try
    {
        return native.engine == Engine::kWii ? ntdlib::rsar::Join(BuildOfWiiArchive(native), sequences, pieces, fields)
               : native.engine == Engine::kGba ? ntdlib::mp2k::Join(sequences, pieces, fields, mp2k_options)
               : native.engine == Engine::kDs  ? ntdlib::sdat::Join(sequences, pieces, fields)
               : native.engine == Engine::kDse ? ntdlib::dse::Join(sequences, pieces, fields)
               : native.driver                 ? DriverOf(native.engine)->Join(native, sequences, pieces, fields)
                                               : ntdlib::sar::Join(FlavorOf(native), sequences, pieces, fields);
    }
    catch (const std::invalid_argument& error)
    {
        throw std::invalid_argument(AsSentence(error.what()));
    }
}

// The first piece of `native` when it holds the archive's header and its tables. The tables there lie where the
// layout's fields lie. An export's tables can lie elsewhere (WriteTables). Nothing for another piece, and nothing for
// an archive of the Wii: an export writes no tables there.
std::optional<std::span<const uint8_t>> TablesPiece(const NativeArchive& native)
{
    const std::vector<uint8_t>* head = FirstRun(native);
    if (head == nullptr || native.engine == Engine::kWii)
    {
        return std::nullopt;
    }

    try
    {
        ntdlib::sar::TablesOf(*head, FlavorOf(native));
    }
    catch (const xsflib::FormatError&)
    {
        return std::nullopt;
    }

    return std::span<const uint8_t>(*head);
}

// Whether the files of `bytes`, the archive that `native` builds, lie elsewhere than the layout's pieces put them. An
// export's tables can grow or shrink (WriteTables). The FILE block follows the INFO block. The files move when the INFO
// block ends elsewhere.
bool FilesMoved(const NativeArchive& native, std::span<const uint8_t> bytes)
{
    const std::optional<std::span<const uint8_t>> head = TablesPiece(native);

    const auto end = [&native](std::span<const uint8_t> archive)
    {
        const std::span<const uint8_t> info = ntdlib::sar::TablesOf(archive, FlavorOf(native)).info;
        return static_cast<std::size_t>(info.data() - archive.data()) + info.size();
    };
    try
    {
        return head && end(*head) != end(bytes);
    }
    catch (const xsflib::FormatError&)
    {
        return false;
    }
}

// The game's stream files `streams` as a native archive's streams.
std::map<uint32_t, NativeStream> GamesStreams(std::map<uint32_t, std::vector<uint8_t>> streams)
{
    std::map<uint32_t, NativeStream> games_streams;
    for (auto& [id, bytes] : streams)
    {
        games_streams[id] = NativeStream{std::move(bytes), true};
    }

    return games_streams;
}

} // namespace

File ImportArchive(std::span<const uint8_t> archive, ntdlib::sar::Flavor flavor,
                   std::vector<std::vector<uint8_t>> group_files, std::map<uint32_t, std::vector<uint8_t>> streams,
                   std::span<const std::string> groups, const ntdlib::sar::KnownSequences& known)
{
    ntdlib::sar::Options options;
    options.flavor = flavor;
    ntdlib::sar::Layout layout = ntdlib::sar::Cut(archive, group_files, options, known);
    const std::optional<ntdlib::sar::SoundArchive> read = ReadStrictly(archive, layout.flavor);

    // Each group file's group, and the game's file.
    std::vector<NativeGroupFile> native_group_files;
    std::map<uint32_t, std::span<const uint8_t>> beside;
    for (std::size_t g = 0; g < group_files.size() && g < layout.group_files.size(); ++g)
    {
        NativeGroupFile& group_file = native_group_files.emplace_back();
        try
        {
            group_file.group = ntdlib::sar::GroupOfFile(archive, flavor, group_files[g]);
        }
        catch (const xsflib::FormatError&)
        {
            // A group file whose group ntdlib can't find takes the group that its name gives (`groups`).
        }

        if (!group_file.group && read && g < groups.size() && !groups[g].empty())
        {
            group_file.group = GroupLabelled(*read, groups[g]);
        }

        group_file.game_size = group_files[g].size();
        group_file.game_hash = xsflib::Sha1::Of(group_files[g]);
        group_file.pieces = std::move(layout.group_files[g].pieces);
        group_file.fields = std::move(layout.group_files[g].fields);
        if (group_file.group)
        {
            beside.emplace(*group_file.group, group_files[g]);
        }
    }

    // The tables, and the game's sizes that an export measures the growth from. Without tables, the game's sizes hold
    // the archive's size alone. Where ntdlib can't list a group's files, the file keeps no game's sizes.
    std::optional<NativeTables> tables;
    std::optional<GameSizes> sizes = GameSizes{archive.size(), {}};
    if (read)
    {
        tables = TablesOf(*read);
        const std::optional<std::vector<uint64_t>> loads = GroupLoads(*read, beside);
        sizes = loads ? std::optional<GameSizes>(GameSizes{archive.size(), *loads}) : std::nullopt;
    }

    File file;
    file.engine = EngineOf(flavor);
    file.sequences = ShareSequences(std::move(layout.sequences));
    file.native = NativeArchive{.engine = file.engine,
                                .pieces = std::move(layout.pieces),
                                .fields = std::move(layout.fields),
                                .group_files = std::move(native_group_files),
                                .tables = std::move(tables),
                                .game_sizes = std::move(sizes),
                                .streams = GamesStreams(std::move(streams))};

    return file;
}

File ImportWiiArchive(std::span<const uint8_t> archive, const std::optional<ntdlib::SndBuild>& build,
                      std::map<uint32_t, std::vector<uint8_t>> streams, const ntdlib::sar::KnownSequences& known)
{
    // The version follows the magic and the byte order mark.
    if (archive.size() < 8 || std::memcmp(archive.data(), "RSAR", 4) != 0)
    {
        throw xsflib::FormatError("not a Wii sound archive (RSAR)");
    }

    ntdlib::rsar::Layout layout =
        ntdlib::rsar::Cut(archive, build.value_or(BuildOfWiiVersion(xsflib::LoadBe16(&archive[6]))), known);

    File file;
    file.engine = Engine::kWii;
    file.sequences = ShareSequences(std::move(layout.sequences));
    file.native = NativeArchive{.engine = Engine::kWii,
                                .pieces = std::move(layout.pieces),
                                .fields = std::move(layout.fields),
                                .tables = ReadWiiTables(archive),
                                .game_sizes = GameSizes{archive.size(), {}},
                                .streams = GamesStreams(std::move(streams)),
                                .wii_build = build};

    return file;
}

std::vector<uint64_t> NativePieceStarts(const File& file)
{
    if (file.native && file.native->engine == Engine::kDse)
    {
        return ntdlib::dse::Starts(CopySequences(file), *file.native->pieces);
    }

    if (!file.native ||
        (file.native->engine != Engine::kGba && file.native->engine != Engine::kDs && !file.native->driver))
    {
        return {};
    }

    // On a driver whose songs are in lists, the join fills each list's room up to its end. The piece after the room
    // starts there.
    const NativeDriver* native_driver = file.native->driver ? &*file.native->driver : nullptr;
    const Driver* driver = native_driver ? DriverOf(file.native->engine) : nullptr;
    std::map<uint32_t, uint64_t> room_ends;
    if (native_driver)
    {
        for (const DriverRoom& room : native_driver->rooms)
        {
            room_ends[room.piece] = room.end;
        }
    }

    const auto past_room = [&room_ends](std::size_t piece, uint64_t at)
    {
        const auto room = room_ends.find(static_cast<uint32_t>(piece));
        return room == room_ends.end() ? at : std::max(at, room->second);
    };

    std::vector<uint64_t> starts;
    uint64_t at = 0;
    const std::vector<ntdlib::sar::Piece>& pieces = *file.native->pieces;
    for (std::size_t p = 0; p < pieces.size(); p++)
    {
        at = past_room(p, at);
        starts.push_back(at);
        const ntdlib::sar::Piece& piece = pieces[p];
        if (const auto* run = std::get_if<std::vector<uint8_t>>(&piece))
        {
            at += run->size();
        }
        else if (const auto* sequence = std::get_if<ntdlib::sar::SequencePiece>(&piece))
        {
            at += driver
                      ? driver->PieceBytes(*file.sequences.at(sequence->sequence), *sequence, native_driver->revision)
                  : file.native->engine == Engine::kGba
                      ? sequence->room
                      : ntdlib::ntr::snd::EncodeSequence(*file.sequences.at(sequence->sequence)).bytes.size();
        }
        else
        {
            const uint64_t alignment = std::max<uint32_t>(std::get<ntdlib::sar::Padding>(piece).alignment, 1);
            at = (at + alignment - 1) / alignment * alignment;
        }
    }

    starts.push_back(past_room(pieces.size(), at));

    return starts;
}

std::vector<uint8_t> PlayedBytes(const File& file)
{
    if (!file.native || (file.native->engine != Engine::kGba && !file.native->driver) || file.native->gba_filler)
    {
        return ExportArchive(file);
    }

    File filler = file;
    filler.native.Edit([](NativeArchive& native) { native.gba_filler = true; });
    return ExportArchive(filler);
}

ntdlib::SndBuild BuildOfWiiArchive(const NativeArchive& native)
{
    if (native.wii_build)
    {
        return *native.wii_build;
    }

    return BuildOfWiiVersion(WiiVersionOf(native));
}

ntdlib::SndBuild BuildOfWiiVersion(uint16_t version)
{
    // A date of 0 means a build without a banner. ntdlib plays it as the build of October 2006
    // (ntdlib::SndBuildWithAx). Every date before 20080308 counts as the build of 2007.
    ntdlib::SndBuild build;
    build.date = version >= 0x0104 ? 20080620 : 20070101;

    return build;
}

uint16_t WiiVersionOf(const NativeArchive& native)
{
    const std::vector<uint8_t>* head = FirstRun(native);
    if (head == nullptr || head->size() < 8)
    {
        throw std::invalid_argument("The Wii archive has no header in its first piece.");
    }

    return xsflib::LoadBe16(&(*head)[6]);
}

std::vector<uint8_t> ExportArchive(const File& file)
{
    if (!file.native)
    {
        throw std::invalid_argument("The file contains no archive to export.");
    }

    // Join takes the sequences in one array. ntdlib's message names the sequence and what it can't encode.
    std::vector<uint8_t> archive =
        JoinPieces(*file.native, CopySequences(file), *file.native->pieces, file.native->fields);

    if (file.native->tables && file.native->engine == Engine::kWii)
    {
        WriteWiiTables(archive, *file.native->tables);
    }
    else if (file.native->tables)
    {
        WriteTables(archive, FlavorOf(*file.native), *file.native->tables);
    }

    if (file.native->engine == Engine::kWii)
    {
        WriteWiiStreamSizes(archive, file.native->streams);
    }

    return archive;
}

std::vector<std::vector<uint8_t>> ExportGroupFiles(const File& file)
{
    if (!file.native)
    {
        throw std::invalid_argument("The file contains no archive to export.");
    }
    if (file.native->group_files.empty())
    {
        return {};
    }

    const std::vector<ntdlib::commands::Sequence> sequences = CopySequences(file);
    std::vector<std::vector<uint8_t>> group_files;
    for (const NativeGroupFile& group_file : file.native->group_files)
    {
        group_files.push_back(JoinPieces(*file.native, sequences, *group_file.pieces, group_file.fields));
    }

    return group_files;
}

std::vector<ntdlib::sar::GroupFileLayout> GroupFileLayouts(const NativeArchive& native)
{
    std::vector<ntdlib::sar::GroupFileLayout> layouts;
    for (const NativeGroupFile& group_file : native.group_files)
    {
        layouts.push_back({*group_file.pieces, group_file.fields});
    }

    return layouts;
}

std::map<uint32_t, std::vector<uint8_t>> GroupFilesBeside(const File& file)
{
    std::vector<std::vector<uint8_t>> joined = ExportGroupFiles(file);
    std::map<uint32_t, std::vector<uint8_t>> beside;
    for (std::size_t g = 0; g < joined.size(); ++g)
    {
        if (const std::optional<uint32_t> group = file.native->group_files[g].group)
        {
            beside.emplace(*group, std::move(joined[g]));
        }
    }

    return beside;
}

std::vector<ChangedGroupFile> ChangedGroupFiles(const File& file)
{
    std::vector<std::vector<uint8_t>> joined = ExportGroupFiles(file);
    std::vector<ChangedGroupFile> changed;
    for (std::size_t g = 0; g < joined.size(); ++g)
    {
        const NativeGroupFile& group_file = file.native->group_files[g];
        if (joined[g].size() == group_file.game_size && xsflib::Sha1::Of(joined[g]) == group_file.game_hash)
        {
            continue;
        }

        ChangedGroupFile& entry = changed.emplace_back();
        entry.index = g;
        entry.group = group_file.group;
        const std::optional<NativeTables>& tables = file.native->tables;
        if (group_file.group && tables && *group_file.group < tables->groups.size())
        {
            entry.name = tables->groups[*group_file.group];
        }

        entry.bytes = std::move(joined[g]);
    }

    return changed;
}

std::vector<FileBeside> GroupFileOutputs(std::vector<ChangedGroupFile> changed, std::span<const uint8_t> archive,
                                         ntdlib::sar::Flavor flavor, std::span<const FileBeside> sarcs)
{
    // The SARC archives that take group files, by their places in `sarcs`, with their new bytes.
    std::map<std::size_t, std::vector<uint8_t>> taken;
    std::vector<FileBeside> outputs;
    for (ChangedGroupFile& group_file : changed)
    {
        if (!group_file.group)
        {
            continue;
        }

        bool placed = false;
        for (std::size_t s = 0; s < sarcs.size() && !placed; ++s)
        {
            const auto now = taken.find(s);
            const std::span<const uint8_t> sarc = now != taken.end() ? now->second : sarcs[s].second;
            for (const ntdlib::sar::SarcFile& file : ntdlib::sar::SarcFiles(sarc))
            {
                if (ntdlib::sar::GroupOfFile(archive, flavor, file.data) == group_file.group)
                {
                    std::vector<uint8_t> bytes = ntdlib::sar::WithSarcFile(sarc, file.data, group_file.bytes);
                    taken[s] = std::move(bytes);
                    placed = true;
                    break;
                }
            }
        }
        if (!placed)
        {
            const std::string group =
                group_file.name.empty() ? "GROUP_" + std::to_string(*group_file.group) : group_file.name;
            outputs.emplace_back(ntdlib::sar::GroupFileName(seqlib::SafeFileName(group), flavor),
                                 std::move(group_file.bytes));
        }
    }

    for (auto& [s, bytes] : taken)
    {
        outputs.emplace_back(sarcs[s].first, std::move(bytes));
    }

    return outputs;
}

ArchiveGrowth MeasureGrowth(const GameSizes& before, std::span<const uint8_t> after, ntdlib::sar::Flavor flavor,
                            const std::map<uint32_t, std::vector<uint8_t>>& beside)
{
    ArchiveGrowth growth{before.archive, after.size(), {}};
    const std::optional<ntdlib::sar::SoundArchive> read = ReadStrictly(after, flavor);
    const std::optional<std::vector<uint64_t>> loads = read ? GroupLoads(*read, Spans(beside)) : std::nullopt;
    if (!loads)
    {
        return growth;
    }

    for (std::size_t i = 0; i < loads->size() && i < before.groups.size(); ++i)
    {
        if ((*loads)[i] > before.groups[i])
        {
            growth.groups.push_back({read->Groups()[i].name, before.groups[i], (*loads)[i]});
        }
    }

    return growth;
}

GameSizes SizesOf(std::span<const uint8_t> archive, ntdlib::sar::Flavor flavor,
                  const std::map<uint32_t, std::vector<uint8_t>>& beside)
{
    const std::optional<ntdlib::sar::SoundArchive> read = ReadStrictly(archive, flavor);
    const std::optional<std::vector<uint64_t>> loads = read ? GroupLoads(*read, Spans(beside)) : std::nullopt;
    return {archive.size(), loads.value_or(std::vector<uint64_t>())};
}

namespace
{

// A sequence file that only group files hold has its pieces there. The first group file that holds a copy of it
// gives its sequence: the piece at the copy's start. The group files' tables give the copies' starts.
void SequencesInGroupFiles(const File& file, const std::map<std::size_t, uint32_t>& sequence_at,
                           std::vector<std::optional<uint32_t>>& sequences)
{
    const NativeArchive& native = *file.native;
    std::set<uint32_t> in_archive;
    for (const auto& [start, sequence] : sequence_at)
    {
        in_archive.insert(sequence);
    }

    std::optional<std::vector<ntdlib::commands::Sequence>> copied;
    for (const NativeGroupFile& group_file : native.group_files)
    {
        const auto elsewhere = [&in_archive](const ntdlib::sar::Piece& piece)
        {
            const auto* sequence_piece = std::get_if<ntdlib::sar::SequencePiece>(&piece);
            return sequence_piece && !in_archive.contains(sequence_piece->sequence);
        };
        if (std::ranges::none_of(*group_file.pieces, elsewhere))
        {
            continue;
        }

        if (!copied)
        {
            copied = CopySequences(file);
        }

        const std::vector<uint8_t> joined = JoinPieces(native, *copied, *group_file.pieces, group_file.fields);
        const std::vector<std::size_t> group_starts = ntdlib::sar::Starts(joined, FlavorOf(native), *group_file.pieces);
        const std::map<std::size_t, uint32_t> group_sequence_at =
            SequenceStarts<std::size_t>(*group_file.pieces, group_starts);
        for (const ntdlib::sar::GroupCopy& copy : ntdlib::sar::GroupCopies(joined, FlavorOf(native)))
        {
            const auto found = group_sequence_at.find(copy.start);
            if (copy.file < sequences.size() && !sequences[copy.file] && found != group_sequence_at.end())
            {
                sequences[copy.file] = found->second;
            }
        }
    }
}

} // namespace

std::vector<std::optional<uint32_t>> SequencesOfFiles(const File& file, std::span<const uint8_t> bytes,
                                                      const ntdlib::sar::SoundArchive& archive)
{
    if (!file.native)
    {
        throw std::invalid_argument("The file contains no archive.");
    }
    if (file.native->engine == Engine::kWii)
    {
        throw std::invalid_argument("The file contains a Wii archive.");
    }

    // Each sequence piece's start in the bytes. After an export's tables grew or shrank, the files lie elsewhere. The
    // pieces then lie where ntdlib::sar::Starts finds them in the archive that they join to. The files after the tables
    // move as a whole. They move by the difference between the two archives' sizes.
    const NativeArchive& native = *file.native;
    std::vector<std::size_t> starts;
    std::size_t joined_size = bytes.size();
    if (FilesMoved(native, bytes))
    {
        const std::vector<uint8_t> joined =
            ntdlib::sar::Join(FlavorOf(native), CopySequences(file), *native.pieces, native.fields);
        starts = ntdlib::sar::Starts(joined, FlavorOf(native), *native.pieces);
        joined_size = joined.size();
    }
    else
    {
        starts = ntdlib::sar::Starts(bytes, FlavorOf(native), *native.pieces);
    }

    const std::map<std::size_t, uint32_t> sequence_at =
        SequenceStarts<std::size_t>(*native.pieces, starts, bytes.size() - joined_size);

    // The piece at each file's start.
    std::vector<std::optional<uint32_t>> sequences(archive.FileCount());
    for (uint32_t id = 0; id < archive.FileCount(); ++id)
    {
        const std::optional<std::size_t> offset = archive.FileOffset(id);
        const auto found = offset ? sequence_at.find(*offset) : sequence_at.end();
        if (found != sequence_at.end())
        {
            sequences[id] = found->second;
        }
    }

    SequencesInGroupFiles(file, sequence_at, sequences);

    return sequences;
}

std::vector<std::optional<uint32_t>> SequencesOfFiles(const File& file, std::span<const uint8_t> bytes,
                                                      const ntdlib::rsar::SoundArchive& archive)
{
    if (!file.native || file.native->engine != Engine::kWii)
    {
        throw std::invalid_argument("The file contains no Wii archive.");
    }

    // Each sequence piece's start in the bytes. The tables keep their size, and the files keep their places in them.
    const NativeArchive& native = *file.native;
    const std::vector<std::size_t> starts = ntdlib::rsar::Starts(bytes, *native.pieces);
    const std::map<std::size_t, uint32_t> sequence_at = SequenceStarts<std::size_t>(*native.pieces, starts);

    // The piece at the start of each file's copy in the archive's bytes. The files of the sounds and the banks, and
    // those that the groups hold, give the count of files.
    uint32_t files = 0;
    for (const ntdlib::rsar::SoundInfo& sound : archive.Sounds())
    {
        files = std::max(files, sound.file_id + 1);
    }

    for (const ntdlib::rsar::BankInfo& bank : archive.Banks())
    {
        files = std::max(files, bank.file_id + 1);
    }

    for (const ntdlib::rsar::SoundArchive::Group& group : archive.Groups())
    {
        for (const ntdlib::rsar::SoundArchive::Group::Item& item : group.items)
        {
            files = std::max(files, item.file_id + 1);
        }
    }

    std::vector<std::optional<uint32_t>> sequences(files);
    for (uint32_t id = 0; id < files; ++id)
    {
        const std::span<const uint8_t> data = archive.FileData(id);
        if (data.empty())
        {
            continue;
        }

        const auto offset = static_cast<std::size_t>(data.data() - archive.Bytes().data());
        if (const auto found = sequence_at.find(offset); found != sequence_at.end())
        {
            sequences[id] = found->second;
        }
    }

    return sequences;
}

std::vector<std::optional<uint32_t>> StartFieldsOfSounds(const File& file, std::span<const uint8_t> bytes)
{
    if (!file.native)
    {
        throw std::invalid_argument("The file contains no archive.");
    }

    // Where each field lies. When the first piece holds the tables, the fields in that piece count, at their offsets.
    // An export's tables can grow or shrink (WriteTables). In the bytes, the entries after a change then lie elsewhere.
    // Without the tables in the first piece, the pieces lie where ntdlib::sar::Starts finds them in the bytes.
    const NativeArchive& native = *file.native;
    const std::optional<std::span<const uint8_t>> head = TablesPiece(native);
    const std::vector<std::size_t> starts = head ? std::vector<std::size_t>(1, 0)
                                            : native.engine == Engine::kWii
                                                ? ntdlib::rsar::Starts(bytes, *native.pieces)
                                                : ntdlib::sar::Starts(bytes, FlavorOf(native), *native.pieces);
    std::map<std::size_t, uint32_t> field_at;
    for (std::size_t i = 0; i < native.fields.size(); ++i)
    {
        const ntdlib::sar::Place& at = native.fields[i].at;
        if (at.piece < starts.size() && at.piece < native.pieces->size() && !at.item)
        {
            field_at.emplace(starts[at.piece] + at.offset, static_cast<uint32_t>(i));
        }
    }

    // The field at each sound's start.
    std::vector<std::optional<uint32_t>> fields;
    const std::vector<std::optional<std::size_t>> positions =
        native.engine == Engine::kWii ? ntdlib::rsar::StartFields(bytes)
                                      : ntdlib::sar::StartFields(head.value_or(bytes), FlavorOf(native));
    for (const std::optional<std::size_t>& position : positions)
    {
        const auto found = position ? field_at.find(*position) : field_at.end();
        fields.push_back(found != field_at.end() ? std::optional(found->second) : std::nullopt);
    }

    return fields;
}

} // namespace hokuto::format
