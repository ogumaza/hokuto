// SPDX-License-Identifier: MIT

// The tables of the native archives of the Switch, the Wii U, the 3DS and the Wii: what ntdlib reads of them, how a
// file's tables differ from those that its export reads back, and how an export writes them (format/archive.h).

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
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
#include "ntdlib/sar/layout.h"
#include "ntdlib/sar/tables.h"
#include "xsflib/binary.h"

#include "format/archive.h"
#include "format/archive_parts.h"
#include "format/codes.h"
#include "format/file.h"

namespace hokuto::format
{
namespace
{

using archive_parts::FirstRun;
using archive_parts::ReadStrictly;
using archive_parts::TablesOf;

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
        return "The archive tables have " + (hold == held.end() ? "nothing more" : *hold) +
               " where the file tables have " + (want == wanted.end() ? "nothing more" : *want) +
               ". Editing the table lines can't add, remove or rename items, or change the type of a sound. Use "
               "File > Import or the Project menu to add items.";
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
                return "The archive tables have no field for the " + std::string(name) + " of sound " + a.name + ".";
            }
        }
    }

    if (file != archive)
    {
        return std::string("The archive tables don't match the values in the file.");
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
        throw std::invalid_argument("Can't write the bank slots of sound " + tables.sounds[lists.begin()->first].name +
                                    " to the archive tables.");
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
        throw std::invalid_argument("Can't add a field for the " + value + " of sound " + tables.sounds[sound].name +
                                    " to the archive tables.");
    }
}

} // namespace

namespace archive_parts
{

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
        throw std::invalid_argument("The file has tables, but ntdlib can't read the archive tables.");
    }

    const std::optional<NativeTables> written = ReadTables(archive, flavor);
    if (!written)
    {
        throw std::invalid_argument("The file has tables, but ntdlib can't read the archive tables.");
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
            throw std::invalid_argument("A Wii player can play at most " + std::to_string(UINT8_MAX) +
                                        " sounds at once. Player " + player.name + " is set to " +
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
        throw std::invalid_argument("The file has tables, but ntdlib can't read the archive tables.");
    }

    const std::optional<NativeTables> written = ReadWiiTables(archive);
    if (!written)
    {
        throw std::invalid_argument("The file has tables, but ntdlib can't read the archive tables.");
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
        throw std::invalid_argument("The file has streams, but ntdlib can't read the archive file table.");
    }
    catch (const std::invalid_argument&)
    {
        throw std::invalid_argument("The file has a stream for a file that isn't next to its archive.");
    }
}

} // namespace archive_parts

std::optional<std::vector<ntdlib::sar::ValuePlaces>> TableValuePlacesOf(const NativeArchive& native)
{
    const std::vector<uint8_t>* head = FirstRun(native);
    if (head == nullptr || native.engine == Engine::kWii)
    {
        return std::nullopt;
    }

    try
    {
        return ntdlib::sar::TableValuePlaces(*head, FlavorOf(native));
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
