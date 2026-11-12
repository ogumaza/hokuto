// SPDX-License-Identifier: MIT

#include "edit/ds_tables.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "ntdlib/sar/formats.h"
#include "ntdlib/sdat/formats.h"
#include "xsflib/ascii.h"
#include "xsflib/binary.h"

#include "edit/ds_files.h"
#include "edit/native_runs.h"
#include "format/archive.h"

namespace hokuto::edit
{

namespace sar = ntdlib::sar;
namespace sdat = ntdlib::sdat;

namespace
{

// The header of the archive stores the offset of the INFO block here. The block's table of sequences follows its 8
// bytes of block header, and each of the table's offsets counts from the block's start.
constexpr std::size_t kInfoOffset = 0x18;
constexpr std::size_t kInfoTables = 8;

// A sequence archive's table of records starts after the file's header, the DATA block's header, the bytecode's offset
// and the count.
constexpr std::size_t kArchivedRecords = 0x20;
constexpr std::size_t kArchivedRecordSize = 12;

// The values start after a record's file ID, or its offset into the bytecode.
constexpr std::size_t kValuesInRecord = 4;

uint64_t ValuesOffset(const song::DsArchive& archive, uint32_t sound)
{
    const ntdlib::ntr::SequenceRef& ref = *archive.sounds[sound];
    if (ref.archived)
    {
        const sdat::SequenceArchiveInfo& info = *archive.archive.sequence_archives.at(ref.index);
        const uint64_t file = archive.archive.files.at(info.file_id).offset;
        return file + kArchivedRecords + uint64_t{*ref.archived} * kArchivedRecordSize + kValuesInRecord;
    }

    const xsflib::Reader reader(archive.bytes);
    const uint32_t info = reader.U32(kInfoOffset);
    const uint32_t table = reader.U32(info + kInfoTables);
    const uint32_t record = reader.U32(std::size_t{info} + table + 4 + std::size_t{ref.index} * 4);

    return uint64_t{info} + record + kValuesInRecord;
}

// The new value `change.to`, up to `highest`.
uint8_t ByteOf(const SoundChange& change, uint32_t highest)
{
    if (change.to > highest)
    {
        throw std::invalid_argument("The " + xsflib::AsciiLower(NameOf(change.value)) +
                                    " of a DS sound must be from 0 to " + std::to_string(highest) + ".");
    }

    return static_cast<uint8_t>(change.to);
}

// The number of the item that `change.to` names, of type `type`.
uint32_t NumberOf(const SoundChange& change, uint32_t type, const std::string& what)
{
    if (sar::ItemType(change.to) != type)
    {
        throw std::invalid_argument("A DS sound requires " + what + ".");
    }

    return sar::ItemNumber(change.to);
}

} // namespace

HeldValues DsHeldValues()
{
    HeldValues held;
    held.bank_slots = 1;
    held.actor_player = false;
    held.pan = false;
    held.release_priority_fix = false;
    held.tracks = false;
    held.highest_volume = kHighestDsVolume;

    return held;
}

format::NativeSound DsNativeSoundOf(const song::DsArchive& archive, uint32_t sound)
{
    const DsSoundValues values = DsSoundValuesOf(archive, sound);
    format::NativeSound native;
    native.name = song::DsSoundName(archive, sound);
    native.kind = sar::SoundType::kSequence;
    native.player = sar::ItemId(sar::kPlayerItem, values.player);
    native.volume = values.volume;
    native.player_priority = values.player_priority;
    native.banks = {sar::ItemId(sar::kBankItem, values.bank)};
    native.tracks = 0;
    native.channel_priority = values.channel_priority;

    return native;
}

std::vector<std::string> DsPlayerNames(const song::DsArchive& archive)
{
    std::vector<std::string> names;
    for (std::size_t player = 0; player < archive.archive.players.size(); player++)
    {
        const std::optional<sdat::PlayerInfo>& info = archive.archive.players[player];
        names.push_back(info && !info->name.empty() ? info->name : "PLAYER_" + std::to_string(player));
    }

    return names;
}

format::File WithDsSoundValues(const format::File& file, const song::DsArchive& archive, uint32_t sound,
                               const DsSoundValues& values)
{
    if (!file.native || file.native->engine != format::Engine::kDs)
    {
        throw std::invalid_argument("The file contains no DS sound archive.");
    }

    CheckDsRecord(archive.sounds, sound, "sound");

    // A game's sound can name a bank or a player that the tables have no record of. Only a new one has to have a
    // record.
    const sdat::Archive& tables = archive.archive;
    const DsSoundValues present = DsSoundValuesOf(archive, sound);
    if (values.bank != present.bank)
    {
        CheckDsRecord(tables.banks, values.bank, "bank");
    }
    if (values.player != present.player)
    {
        CheckDsRecord(tables.players, values.player, "player");
    }
    if (values.volume > kHighestDsVolume)
    {
        throw std::invalid_argument("The volume of a DS sound must be from 0 to " + std::to_string(kHighestDsVolume) +
                                    ".");
    }

    if (values.channel_priority > HighestOf(SoundValue::kChannelPriority) ||
        values.player_priority > HighestOf(SoundValue::kPlayerPriority))
    {
        throw std::invalid_argument("The priorities of a DS sound must be from 0 to " +
                                    std::to_string(HighestOf(SoundValue::kPlayerPriority)) + ".");
    }

    const std::array<uint8_t, 6> bytes = {static_cast<uint8_t>(values.bank),
                                          static_cast<uint8_t>(values.bank >> 8),
                                          values.volume,
                                          values.channel_priority,
                                          values.player_priority,
                                          values.player};

    return WithArchiveBytes(file, format::NativePieceStarts(file), ValuesOffset(archive, sound), bytes, "The sound");
}

format::File ChangeDsSound(const format::File& file, const song::DsArchive& archive, uint32_t sound,
                           const SoundChange& change)
{
    DsSoundValues values = DsSoundValuesOf(archive, sound);
    switch (change.value)
    {
    case SoundValue::kPlayer:
        {
            const uint32_t player = NumberOf(change, sar::kPlayerItem, "a player");
            if (player >= archive.archive.players.size())
            {
                throw std::invalid_argument("The archive has no player " + std::to_string(player) + ".");
            }

            values.player = static_cast<uint8_t>(player);
            break;
        }

    case SoundValue::kVolume:
        values.volume = ByteOf(change, kHighestDsVolume);
        break;

    case SoundValue::kPlayerPriority:
        values.player_priority = ByteOf(change, HighestOf(change.value));
        break;

    case SoundValue::kChannelPriority:
        values.channel_priority = ByteOf(change, HighestOf(change.value));
        break;

    case SoundValue::kBank:
        {
            if (change.slot != 0)
            {
                throw std::invalid_argument("A DS sound has a single bank slot (slot 0).");
            }

            const uint32_t bank = NumberOf(change, sar::kBankItem, "a bank");
            if (bank >= archive.archive.banks.size())
            {
                throw std::invalid_argument("The archive has no bank " + std::to_string(bank) + ".");
            }

            values.bank = static_cast<uint16_t>(bank);
            break;
        }

    default:
        throw std::invalid_argument("The " + xsflib::AsciiLower(NameOf(change.value)) +
                                    " setting isn't available for DS sounds.");
    }

    return WithDsSoundValues(file, archive, sound, values);
}

} // namespace hokuto::edit
