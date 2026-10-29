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

#include "ntdlib/rsar/layout.h"
#include "ntdlib/rsar/tables.h"
#include "ntdlib/rvl/snd/rsar_options.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sar/groups.h"
#include "ntdlib/sar/layout.h"
#include "ntdlib/sar/sarc.h"
#include "ntdlib/sar/tables.h"
#include "seqlib/file_name.h"
#include "xsflib/binary.h"
#include "xsflib/bytes.h"
#include "xsflib/sha1.h"

#include "format/codes.h"
#include "format/file.h"

namespace hokuto::format
{
namespace
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
    try
    {
        return native.wii ? ntdlib::rsar::Join(BuildOfWiiArchive(native), sequences, pieces, fields)
                          : ntdlib::sar::Join(native.flavor, sequences, pieces, fields);
    }
    catch (const std::invalid_argument& error)
    {
        std::string message = error.what();
        if (!message.empty() && message[0] >= 'a' && message[0] <= 'z')
        {
            message[0] = static_cast<char>(message[0] - 'a' + 'A');
        }
        throw std::invalid_argument(message + ".");
    }
}

// What ntdlib reads of the tables of `read`.
NativeTables TablesOf(const ntdlib::sar::SoundArchive& read)
{
    NativeTables tables;
    for (const ntdlib::sar::SoundInfo& info : read.Sounds())
    {
        tables.sounds.push_back({info.name, info.type, info.player_id, info.volume, info.pan_mode, info.pan_curve,
                                 info.player_priority, info.actor_player_id, info.banks, info.allocate_track_flags,
                                 info.channel_priority, info.release_priority_fix});
    }
    for (const ntdlib::sar::BankInfo& bank : read.Banks())
    {
        tables.banks.push_back(bank.name);
    }
    for (const ntdlib::sar::WaveArchiveInfo& wave_archive : read.WaveArchives())
    {
        tables.wave_archives.push_back(wave_archive.name);
    }
    for (const ntdlib::sar::PlayerInfo& player : read.Players())
    {
        tables.players.push_back({player.name, player.playable_sound_max});
    }
    for (const ntdlib::sar::GroupInfo& group : read.Groups())
    {
        tables.groups.push_back(group.name);
    }

    return tables;
}

// The later consoles' kind for sound `type` of the Wii.
ntdlib::sar::SoundType KindOf(ntdlib::rsar::SoundType type)
{
    switch (type)
    {
    case ntdlib::rsar::SoundType::kSequence:
        return ntdlib::sar::SoundType::kSequence;
    case ntdlib::rsar::SoundType::kStream:
        return ntdlib::sar::SoundType::kStream;
    case ntdlib::rsar::SoundType::kWaveSound:
        return ntdlib::sar::SoundType::kWaveSound;
    case ntdlib::rsar::SoundType::kUnknown:
        break;
    }

    return ntdlib::sar::SoundType::kUnknown;
}

// What ntdlib reads of the tables of `read`, an archive of the Wii. The players and banks take the item IDs of the
// later consoles' archives. A sequence sound has one bank slot, and its details give its tracks and its channel
// priority. Another sound keeps the defaults. The Wii's tables hold no wave archives and no actor players, and ntdlib
// reads no groups' names there. Nothing when an index doesn't fit an item ID.
std::optional<NativeTables> WiiTablesOf(const ntdlib::rsar::SoundArchive& read)
{
    constexpr uint32_t kIndices = 1U << 24;
    if (read.Players().size() > kIndices || read.Banks().size() > kIndices)
    {
        return std::nullopt;
    }

    NativeTables tables;
    for (const ntdlib::rsar::SoundInfo& info : read.Sounds())
    {
        if (info.player_id >= kIndices || (info.bank >= kIndices && info.bank != ntdlib::sar::kNoItem))
        {
            return std::nullopt;
        }

        NativeSound& sound = tables.sounds.emplace_back();
        sound.name = info.name;
        sound.kind = KindOf(info.type);
        sound.player = ntdlib::sar::ItemId(ntdlib::sar::kPlayerItem, info.player_id);
        sound.volume = info.volume;
        sound.pan_mode = info.pan_mode;
        sound.pan_curve = info.pan_curve;
        sound.player_priority = info.player_priority;
        if (info.type == ntdlib::rsar::SoundType::kSequence)
        {
            sound.banks = {info.bank == ntdlib::sar::kNoItem ? ntdlib::sar::kNoItem
                                                             : ntdlib::sar::ItemId(ntdlib::sar::kBankItem, info.bank)};
            sound.tracks = info.allocate_track_flags;
            sound.channel_priority = info.channel_priority;
            sound.release_priority_fix = info.release_priority_fix;
        }
    }
    for (const ntdlib::rsar::BankInfo& bank : read.Banks())
    {
        tables.banks.push_back(bank.name);
    }
    for (const ntdlib::rsar::PlayerInfo& player : read.Players())
    {
        tables.players.push_back({player.name, player.sound_count});
    }

    return tables;
}

// What ntdlib reads of the tables of `archive`, an archive of the Wii, with the program's options for playback
// (ntdlib::rvl::snd::kRsarOptions). Nothing if ntdlib can't read them.
std::optional<NativeTables> ReadWiiTables(std::span<const uint8_t> archive)
{
    try
    {
        return WiiTablesOf(ntdlib::rsar::SoundArchive::Load(std::vector<uint8_t>(archive.begin(), archive.end()),
                                                            ntdlib::rvl::snd::kRsarOptions));
    }
    catch (const xsflib::FormatError&)
    {
        return std::nullopt;
    }
}

// What ntdlib reads of the tables of `archive`, an archive of `flavor`, when it stops at the first problem. Nothing if
// it finds one.
std::optional<NativeTables> ReadTables(std::span<const uint8_t> archive, ntdlib::sar::Flavor flavor)
{
    const std::optional<ntdlib::sar::SoundArchive> read = ReadStrictly(archive, flavor);
    if (!read)
    {
        return std::nullopt;
    }

    return TablesOf(*read);
}

// The items of `tables` by kind and name, and each sound's kind: what an export keeps of the archive's tables.
std::vector<std::string> Items(const NativeTables& tables)
{
    std::vector<std::string> items;
    for (const NativeSound& sound : tables.sounds)
    {
        items.push_back("sound " + sound.name + " (" + std::string(NameOf(sound.kind)) + ")");
    }
    for (const std::string& name : tables.banks)
    {
        items.push_back("bank " + name);
    }
    for (const std::string& name : tables.wave_archives)
    {
        items.push_back("wave archive " + name);
    }
    for (const NativePlayer& player : tables.players)
    {
        items.push_back("player " + player.name);
    }
    for (const std::string& name : tables.groups)
    {
        items.push_back("group " + name);
    }

    return items;
}

// The first difference between the tables of a file and the tables that its export reads back, in a sentence for the
// user. Nothing if they match.
std::optional<std::string> Difference(const NativeTables& file, const NativeTables& archive)
{
    // The items, in order.
    const std::vector<std::string> wanted = Items(file);
    const std::vector<std::string> held = Items(archive);
    const auto [want, hold] = std::mismatch(wanted.begin(), wanted.end(), held.begin(), held.end());
    if (want != wanted.end() || hold != held.end())
    {
        return "The archive's tables hold " + (hold == held.end() ? "nothing more" : *hold) +
               " where the file's tables hold " + (want == wanted.end() ? "nothing more" : *want) +
               ". An edit of the tables' lines can't add, remove or rename an item, or change a sound's kind. "
               "File > Import and the Project menu add items.";
    }

    // The values of each sound.
    for (std::size_t i = 0; i < file.sounds.size(); ++i)
    {
        const NativeSound& a = file.sounds[i];
        const NativeSound& b = archive.sounds[i];
        const std::array<std::pair<std::string_view, bool>, 10> values = {{
            {"player", a.player != b.player},
            {"volume", a.volume != b.volume},
            {"pan mode", a.pan_mode != b.pan_mode},
            {"pan curve", a.pan_curve != b.pan_curve},
            {"player priority", a.player_priority != b.player_priority},
            {"actor player", a.actor_player != b.actor_player},
            {"banks", a.banks != b.banks},
            {"tracks", a.tracks != b.tracks},
            {"channel priority", a.channel_priority != b.channel_priority},
            {"release priority fix", a.release_priority_fix != b.release_priority_fix},
        }};
        for (const auto& [name, differs] : values)
        {
            if (differs)
            {
                return "The archive's tables have no place for the " + std::string(name) + " of sound " + a.name + ".";
            }
        }
    }

    if (file != archive)
    {
        return std::string("The archive's tables don't hold the values of the file's tables.");
    }

    return std::nullopt;
}

// `archive`, an archive of `flavor`, with a list of banks of the length that `tables` gives for each sequence sound
// whose list changes length. The tables then have a place for each bank slot. A list ends its sound's entry, and the
// entry grows or shrinks with it (ntdlib::sar::WithBankLists). Throws std::invalid_argument for a list that ntdlib
// can't write.
std::vector<uint8_t> WithListsOfBanks(std::vector<uint8_t> archive, ntdlib::sar::Flavor flavor,
                                      const NativeTables& tables)
{
    const std::optional<NativeTables> held = ReadTables(archive, flavor);
    std::map<uint32_t, std::vector<uint32_t>> lists;
    for (std::size_t i = 0; held && i < std::min(tables.sounds.size(), held->sounds.size()); ++i)
    {
        const NativeSound& sound = tables.sounds[i];
        if (sound.kind == ntdlib::sar::SoundType::kSequence && held->sounds[i].kind == sound.kind &&
            sound.banks.size() != held->sounds[i].banks.size())
        {
            lists[static_cast<uint32_t>(i)] = sound.banks;
        }
    }
    if (lists.empty())
    {
        return archive;
    }

    try
    {
        return ntdlib::sar::WithBankLists(archive, flavor, lists);
    }
    catch (const std::exception&)
    {
        throw std::invalid_argument("Hokuto can't write the bank slots of sound " +
                                    tables.sounds[lists.begin()->first].name + " into the archive's tables.");
    }
}

// `archive`, an archive of `flavor`, with the places that the values of `tables` require in the sounds' entries
// (NeededPlaces). An entry that leaves out such a value's option takes it, and grows (ntdlib::sar::WithValuePlaces).
// The archive comes back as it is when no entry lacks a place, or when ntdlib can't read its places. Throws
// std::invalid_argument for a place that ntdlib can't give.
std::vector<uint8_t> WithPlacesOfValues(std::vector<uint8_t> archive, ntdlib::sar::Flavor flavor,
                                        const NativeTables& tables)
{
    std::vector<ntdlib::sar::ValuePlaces> held;
    try
    {
        held = ntdlib::sar::TableValuePlaces(archive, flavor);
    }
    catch (const xsflib::FormatError&)
    {
        return archive;
    }

    std::map<uint32_t, ntdlib::sar::ValuePlaces> places;
    for (std::size_t i = 0; i < std::min(tables.sounds.size(), held.size()); ++i)
    {
        const ntdlib::sar::ValuePlaces needed = NeededPlaces(tables.sounds[i]);
        const ntdlib::sar::ValuePlaces lacked{needed.pan && !held[i].pan, needed.priority && !held[i].priority,
                                              needed.channel && !held[i].channel};
        if (lacked != ntdlib::sar::ValuePlaces{})
        {
            places[static_cast<uint32_t>(i)] = lacked;
        }
    }
    if (places.empty())
    {
        return archive;
    }

    try
    {
        return ntdlib::sar::WithValuePlaces(archive, flavor, places);
    }
    catch (const std::exception&)
    {
        const auto& [sound, lacked] = *places.begin();
        const std::string value = lacked.pan ? "pan" : lacked.priority ? "priorities" : "channel priority";
        throw std::invalid_argument("Hokuto can't give sound " + tables.sounds[sound].name + " a place for its " +
                                    value + " in the archive's tables.");
    }
}

// Writes the values of `tables` into `archive`, an archive of `flavor`. A sequence sound's list of banks takes the
// length of the sound's banks in `tables` first, and a sound's entry takes the places that its values require. Throws
// std::invalid_argument when the archive then reads back otherwise.
void WriteTables(std::vector<uint8_t>& archive, ntdlib::sar::Flavor flavor, const NativeTables& tables)
{
    archive = WithListsOfBanks(std::move(archive), flavor, tables);
    archive = WithPlacesOfValues(std::move(archive), flavor, tables);
    std::vector<ntdlib::sar::SoundInfo> sounds;
    for (const NativeSound& sound : tables.sounds)
    {
        ntdlib::sar::SoundInfo& info = sounds.emplace_back();
        info.player_id = sound.player;
        info.volume = sound.volume;
        info.pan_mode = sound.pan_mode;
        info.pan_curve = sound.pan_curve;
        info.player_priority = sound.player_priority;
        info.actor_player_id = sound.actor_player;
        info.banks = sound.banks;
        info.allocate_track_flags = sound.tracks;
        info.channel_priority = sound.channel_priority;
        info.release_priority_fix = sound.release_priority_fix;
    }
    std::vector<ntdlib::sar::PlayerInfo> players;
    for (const NativePlayer& player : tables.players)
    {
        players.push_back({player.name, player.sound_limit});
    }

    try
    {
        ntdlib::sar::WriteTableValues(archive, flavor, sounds, players);
    }
    catch (const xsflib::FormatError&)
    {
        throw std::invalid_argument("The file holds tables, and ntdlib can't read the tables of its archive.");
    }

    const std::optional<NativeTables> written = ReadTables(archive, flavor);
    if (!written)
    {
        throw std::invalid_argument("The file holds tables, and ntdlib can't read the tables of its archive.");
    }
    if (const std::optional<std::string> difference = Difference(tables, *written))
    {
        throw std::invalid_argument(*difference);
    }
}

// Writes the values of `tables` into `archive`, an archive of the Wii, in place (ntdlib::rsar::WriteTableValues).
// Nothing moves. A value that the archive's version doesn't hold, such as a pan mode before version 1.2, stays as it
// is. Throws std::invalid_argument when the archive then reads back otherwise.
void WriteWiiTables(std::vector<uint8_t>& archive, const NativeTables& tables)
{
    std::vector<ntdlib::rsar::SoundInfo> sounds;
    for (const NativeSound& sound : tables.sounds)
    {
        ntdlib::rsar::SoundInfo& info = sounds.emplace_back();
        info.player_id = ntdlib::sar::ItemNumber(sound.player);
        info.volume = sound.volume;
        info.player_priority = sound.player_priority;
        info.pan_mode = sound.pan_mode;
        info.pan_curve = sound.pan_curve;
        if (!sound.banks.empty())
        {
            info.bank =
                sound.banks[0] == ntdlib::sar::kNoItem ? ntdlib::sar::kNoItem : ntdlib::sar::ItemNumber(sound.banks[0]);
        }
        info.allocate_track_flags = sound.tracks;
        info.channel_priority = sound.channel_priority;
        info.release_priority_fix = sound.release_priority_fix;
    }
    std::vector<ntdlib::rsar::PlayerInfo> players;
    for (const NativePlayer& player : tables.players)
    {
        if (player.sound_limit > UINT8_MAX)
        {
            throw std::invalid_argument("A player of the Wii plays " + std::to_string(UINT8_MAX) +
                                        " sounds at once at most. Player " + player.name + " can't play " +
                                        std::to_string(player.sound_limit) + ".");
        }

        players.push_back({player.name, static_cast<uint8_t>(player.sound_limit), 0});
    }

    try
    {
        ntdlib::rsar::WriteTableValues(archive, sounds, players);
    }
    catch (const xsflib::FormatError&)
    {
        throw std::invalid_argument("The file holds tables, and ntdlib can't read the tables of its archive.");
    }

    const std::optional<NativeTables> written = ReadWiiTables(archive);
    if (!written)
    {
        throw std::invalid_argument("The file holds tables, and ntdlib can't read the tables of its archive.");
    }
    if (const std::optional<std::string> difference = Difference(tables, *written))
    {
        throw std::invalid_argument(*difference);
    }
}

// Writes the size of each of `streams` that Hokuto changed or added into the stream's entry in the file table of
// `archive`, an archive of the Wii (ntdlib::rsar::WriteExternalFileSizes). The Wii's file table gives the size of each
// file beside the archive. The entries of the game's streams stay as they are.
void WriteWiiStreamSizes(std::vector<uint8_t>& archive, const std::map<uint32_t, NativeStream>& streams)
{
    std::vector<std::pair<uint32_t, uint32_t>> sizes;
    for (const auto& [id, stream] : streams)
    {
        if (!stream.from_game)
        {
            sizes.emplace_back(id, static_cast<uint32_t>(stream.bytes->size()));
        }
    }
    if (sizes.empty())
    {
        return;
    }

    try
    {
        ntdlib::rsar::WriteExternalFileSizes(archive, sizes);
    }
    catch (const xsflib::FormatError&)
    {
        throw std::invalid_argument("The file holds streams, and ntdlib can't read the file table of its archive.");
    }
    catch (const std::invalid_argument&)
    {
        throw std::invalid_argument("The file holds a stream of a file that doesn't lie beside its archive.");
    }
}

// The first piece of `native` when it holds the archive's header and its tables. The tables there lie where the
// layout's fields lie. An export's tables can lie elsewhere (WriteTables). Nothing for another piece, and nothing for
// an archive of the Wii: an export writes no tables there.
std::optional<std::span<const uint8_t>> TablesPiece(const NativeArchive& native)
{
    const auto* head = native.pieces->empty() ? nullptr : std::get_if<std::vector<uint8_t>>(&native.pieces->front());
    if (head == nullptr || native.wii)
    {
        return std::nullopt;
    }

    try
    {
        ntdlib::sar::TablesOf(*head, native.flavor);
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
        const std::span<const uint8_t> info = ntdlib::sar::TablesOf(archive, native.flavor).info;
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
    file.native = NativeArchive{.flavor = layout.flavor,
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
        throw xsflib::FormatError("not a sound archive of the Wii (RSAR)");
    }

    ntdlib::rsar::Layout layout =
        ntdlib::rsar::Cut(archive, build.value_or(BuildOfWiiVersion(xsflib::LoadBe16(&archive[6]))), known);

    File file;
    file.engine = Engine::kWii;
    file.sequences = ShareSequences(std::move(layout.sequences));
    file.native = NativeArchive{.pieces = std::move(layout.pieces),
                                .fields = std::move(layout.fields),
                                .tables = ReadWiiTables(archive),
                                .game_sizes = GameSizes{archive.size(), {}},
                                .streams = GamesStreams(std::move(streams)),
                                .wii = true,
                                .wii_build = build};

    return file;
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
    ntdlib::SndBuild build;
    build.date = version >= 0x0104 ? 20080620 : 0;

    return build;
}

uint16_t WiiVersionOf(const NativeArchive& native)
{
    const auto* head = native.pieces->empty() ? nullptr : std::get_if<std::vector<uint8_t>>(&native.pieces->front());
    if (head == nullptr || head->size() < 8)
    {
        throw std::invalid_argument("The archive of the Wii has no header in its first piece.");
    }

    return xsflib::LoadBe16(&(*head)[6]);
}

std::vector<uint8_t> ExportArchive(const File& file)
{
    if (!file.native)
    {
        throw std::invalid_argument("The file holds no archive to export.");
    }

    // Join takes the sequences in one array. ntdlib's message names the sequence and what it can't encode.
    std::vector<uint8_t> archive =
        JoinPieces(*file.native, CopySequences(file), *file.native->pieces, file.native->fields);

    if (file.native->tables && file.native->wii)
    {
        WriteWiiTables(archive, *file.native->tables);
    }
    else if (file.native->tables)
    {
        WriteTables(archive, file.native->flavor, *file.native->tables);
    }
    if (file.native->wii)
    {
        WriteWiiStreamSizes(archive, file.native->streams);
    }

    return archive;
}

std::vector<std::vector<uint8_t>> ExportGroupFiles(const File& file)
{
    if (!file.native)
    {
        throw std::invalid_argument("The file holds no archive to export.");
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

std::vector<std::optional<uint32_t>> SequencesOfFiles(const File& file, std::span<const uint8_t> bytes,
                                                      const ntdlib::sar::SoundArchive& archive)
{
    if (!file.native)
    {
        throw std::invalid_argument("The file holds no archive.");
    }
    if (file.native->wii)
    {
        throw std::invalid_argument("The file holds an archive of the Wii.");
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
            ntdlib::sar::Join(native.flavor, CopySequences(file), *native.pieces, native.fields);
        starts = ntdlib::sar::Starts(joined, native.flavor, *native.pieces);
        joined_size = joined.size();
    }
    else
    {
        starts = ntdlib::sar::Starts(bytes, native.flavor, *native.pieces);
    }
    std::map<std::size_t, uint32_t> sequence_at;
    for (std::size_t i = 0; i < native.pieces->size(); ++i)
    {
        if (const auto* piece = std::get_if<ntdlib::sar::SequencePiece>(&(*native.pieces)[i]))
        {
            sequence_at.emplace(starts[i] + bytes.size() - joined_size, piece->sequence);
        }
    }

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

    // A sequence file that only group files hold has its pieces there. The first group file that holds a copy of it
    // gives its sequence: the piece at the copy's start. The group files' tables give the copies' starts.
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
        const std::vector<std::size_t> group_starts = ntdlib::sar::Starts(joined, native.flavor, *group_file.pieces);
        std::map<std::size_t, uint32_t> group_sequence_at;
        for (std::size_t i = 0; i < group_file.pieces->size(); ++i)
        {
            if (const auto* piece = std::get_if<ntdlib::sar::SequencePiece>(&(*group_file.pieces)[i]))
            {
                group_sequence_at.emplace(group_starts[i], piece->sequence);
            }
        }
        for (const ntdlib::sar::GroupCopy& copy : ntdlib::sar::GroupCopies(joined, native.flavor))
        {
            const auto found = group_sequence_at.find(copy.start);
            if (copy.file < sequences.size() && !sequences[copy.file] && found != group_sequence_at.end())
            {
                sequences[copy.file] = found->second;
            }
        }
    }

    return sequences;
}

std::vector<std::optional<uint32_t>> SequencesOfFiles(const File& file, std::span<const uint8_t> bytes,
                                                      const ntdlib::rsar::SoundArchive& archive)
{
    if (!file.native || !file.native->wii)
    {
        throw std::invalid_argument("The file holds no archive of the Wii.");
    }

    // Each sequence piece's start in the bytes. The tables keep their size, and the files keep their places in them.
    const NativeArchive& native = *file.native;
    const std::vector<std::size_t> starts = ntdlib::rsar::Starts(bytes, *native.pieces);
    std::map<std::size_t, uint32_t> sequence_at;
    for (std::size_t i = 0; i < native.pieces->size(); ++i)
    {
        if (const auto* piece = std::get_if<ntdlib::sar::SequencePiece>(&(*native.pieces)[i]))
        {
            sequence_at.emplace(starts[i], piece->sequence);
        }
    }

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
        throw std::invalid_argument("The file holds no archive.");
    }

    // Where each field lies. When the first piece holds the tables, the fields in that piece count, at their offsets.
    // An export's tables can grow or shrink (WriteTables). In the bytes, the entries after a change then lie elsewhere.
    // Without the tables in the first piece, the pieces lie where ntdlib::sar::Starts finds them in the bytes.
    const NativeArchive& native = *file.native;
    const std::optional<std::span<const uint8_t>> head = TablesPiece(native);
    const std::vector<std::size_t> starts = head         ? std::vector<std::size_t>(1, 0)
                                            : native.wii ? ntdlib::rsar::Starts(bytes, *native.pieces)
                                                         : ntdlib::sar::Starts(bytes, native.flavor, *native.pieces);
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
        native.wii ? ntdlib::rsar::StartFields(bytes) : ntdlib::sar::StartFields(head.value_or(bytes), native.flavor);
    for (const std::optional<std::size_t>& position : positions)
    {
        const auto found = position ? field_at.find(*position) : field_at.end();
        fields.push_back(found != field_at.end() ? std::optional(found->second) : std::nullopt);
    }

    return fields;
}

std::optional<std::vector<ntdlib::sar::ValuePlaces>> TableValuePlacesOf(const NativeArchive& native)
{
    const auto* head = native.pieces->empty() ? nullptr : std::get_if<std::vector<uint8_t>>(&native.pieces->front());
    if (head == nullptr || native.wii)
    {
        return std::nullopt;
    }

    try
    {
        return ntdlib::sar::TableValuePlaces(*head, native.flavor);
    }
    catch (const xsflib::FormatError&)
    {
        return std::nullopt;
    }
}

ntdlib::sar::ValuePlaces NeededPlaces(const NativeSound& sound)
{
    const ntdlib::sar::SoundInfo unset;
    ntdlib::sar::ValuePlaces needed;
    needed.pan = sound.pan_mode != unset.pan_mode || sound.pan_curve != unset.pan_curve;
    needed.priority = sound.player_priority != unset.player_priority || sound.actor_player != unset.actor_player_id;
    needed.channel =
        sound.kind == ntdlib::sar::SoundType::kSequence &&
        (sound.channel_priority != unset.channel_priority || sound.release_priority_fix != unset.release_priority_fix);

    return needed;
}

} // namespace hokuto::format
