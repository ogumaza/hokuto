// SPDX-License-Identifier: MIT

#include "edit/tables.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "ntdlib/sar/formats.h"
#include "ntdlib/sar/layout.h"
#include "xsflib/ascii.h"

#include "edit/replace.h"
#include "format/archive.h"

namespace hokuto::edit
{
namespace
{

constexpr int kBankSlots = 4;
constexpr uint32_t kTrackFlags = 0xFFFF;

// Whether only a sequence sound has `value`.
bool OfSequenceSounds(SoundValue value)
{
    return value == SoundValue::kBank || value == SoundValue::kTracks || value == SoundValue::kChannelPriority ||
           value == SoundValue::kReleasePriorityFix;
}

// Throws for `change` of a sound of `tables` that the value's range or the tables refuse.
void CheckChange(const format::NativeTables& tables, const SoundChange& change)
{
    switch (change.value)
    {
    case SoundValue::kPlayer:
        if (ntdlib::sar::ItemType(change.to) != ntdlib::sar::kPlayerItem ||
            ntdlib::sar::ItemNumber(change.to) >= tables.players.size())
        {
            throw std::invalid_argument("The archive's tables hold no such player.");
        }
        break;

    case SoundValue::kBank:
        if (change.slot < 0 || change.slot >= kBankSlots)
        {
            throw std::invalid_argument("A sound has four bank slots. Each holds one of the archive's banks, or none.");
        }

        CheckBankSlots(tables, std::vector<uint32_t>{change.to});
        break;

    case SoundValue::kTracks:
        if (change.to == 0 || change.to > kTrackFlags)
        {
            throw std::invalid_argument("A sound allocates at least one track, of tracks 0 to 15.");
        }
        break;

    default:
        if (change.to > HighestOf(change.value))
        {
            throw std::invalid_argument("The " + xsflib::AsciiLower(NameOf(change.value)) + " goes from 0 to " +
                                        std::to_string(HighestOf(change.value)) + ".");
        }
        break;
    }
}

// The version `version` of an archive of the Wii as the user reads it, such as 1.4.
std::string VersionText(uint16_t version)
{
    return std::to_string(version >> 8) + "." + std::to_string(version & 0xFF);
}

// Throws for `change` of a value that `file`'s archive of the Wii doesn't hold.
void CheckHeld(const format::File& file, const SoundChange& change)
{
    const HeldValues held = HeldValuesOf(file);
    const std::string version = VersionText(format::WiiVersionOf(*file.native));
    switch (change.value)
    {
    case SoundValue::kActorPlayer:
        if (!held.actor_player)
        {
            throw std::invalid_argument("Hokuto reads no actor player in an archive of the Wii.");
        }
        break;

    case SoundValue::kBank:
        if (change.slot < 0 || change.slot >= held.bank_slots)
        {
            throw std::invalid_argument("A sound of the Wii plays one bank, in slot 0.");
        }
        break;

    case SoundValue::kPanMode:
    case SoundValue::kPanCurve:
        if (!held.pan)
        {
            throw std::invalid_argument(
                "An archive of the Wii holds the pan mode and the pan curve from version 1.2 on. This archive holds "
                "version " +
                version + ".");
        }
        break;

    case SoundValue::kReleasePriorityFix:
        if (!held.release_priority_fix)
        {
            throw std::invalid_argument(
                "An archive of the Wii holds the release priority fix from version 1.3 on. This archive holds "
                "version " +
                version + ".");
        }
        break;

    default:
        break;
    }
}

// `banks`, a sound's bank slots, with `bank` in slot `slot`. The list ends at its last bank, and keeps one slot at
// least. NintendoWare's tools write a list the same way.
std::vector<uint32_t> WithBank(std::vector<uint32_t> banks, int slot, uint32_t bank)
{
    const auto at = static_cast<std::size_t>(slot);
    if (at >= banks.size())
    {
        banks.resize(at + 1, ntdlib::sar::kNoItem);
    }
    banks[at] = bank;
    while (banks.size() > 1 && banks.back() == ntdlib::sar::kNoItem)
    {
        banks.pop_back();
    }

    return banks;
}

} // namespace

HeldValues HeldValuesOf(const format::File& file)
{
    if (!file.native || !file.native->wii)
    {
        return {};
    }

    // Without the header, the values of the oldest version count.
    uint16_t version = 0;
    try
    {
        version = format::WiiVersionOf(*file.native);
    }
    catch (const std::invalid_argument&)
    {
    }

    HeldValues held;
    held.bank_slots = 1;
    held.actor_player = false;
    held.pan = version >= 0x0102;
    held.release_priority_fix = version >= 0x0103;

    return held;
}

uint32_t HighestOf(SoundValue value)
{
    switch (value)
    {
    case SoundValue::kVolume:
        return 255;
    case SoundValue::kPanMode:
    case SoundValue::kReleasePriorityFix:
        return 1;
    case SoundValue::kPanCurve:
        return 8;
    case SoundValue::kPlayerPriority:
    case SoundValue::kChannelPriority:
        return 127;
    case SoundValue::kActorPlayer:
        return 3;
    case SoundValue::kTracks:
        return kTrackFlags;
    case SoundValue::kPlayer:
    case SoundValue::kBank:
        break;
    }

    return ntdlib::sar::kNoItem;
}

uint32_t ValueOf(const format::NativeSound& sound, SoundValue value, int slot)
{
    switch (value)
    {
    case SoundValue::kPlayer:
        return sound.player;
    case SoundValue::kVolume:
        return sound.volume;
    case SoundValue::kPanMode:
        return sound.pan_mode;
    case SoundValue::kPanCurve:
        return sound.pan_curve;
    case SoundValue::kPlayerPriority:
        return sound.player_priority;
    case SoundValue::kActorPlayer:
        return sound.actor_player;
    case SoundValue::kBank:
        return slot >= 0 && static_cast<std::size_t>(slot) < sound.banks.size()
                   ? sound.banks[static_cast<std::size_t>(slot)]
                   : ntdlib::sar::kNoItem;
    case SoundValue::kTracks:
        return sound.tracks;
    case SoundValue::kChannelPriority:
        return sound.channel_priority;
    case SoundValue::kReleasePriorityFix:
        break;
    }

    return sound.release_priority_fix ? 1 : 0;
}

std::string NameOf(SoundValue value)
{
    switch (value)
    {
    case SoundValue::kPlayer:
        return "Player";
    case SoundValue::kVolume:
        return "Volume";
    case SoundValue::kPanMode:
        return "Pan Mode";
    case SoundValue::kPanCurve:
        return "Pan Curve";
    case SoundValue::kPlayerPriority:
        return "Player Priority";
    case SoundValue::kActorPlayer:
        return "Actor Player";
    case SoundValue::kBank:
        return "Bank Slot";
    case SoundValue::kTracks:
        return "Tracks";
    case SoundValue::kChannelPriority:
        return "Channel Priority";
    case SoundValue::kReleasePriorityFix:
        break;
    }

    return "Release Priority Fix";
}

format::File ChangeSound(const format::File& file, uint32_t sound, const SoundChange& change)
{
    if (!file.native || !file.native->tables)
    {
        throw std::invalid_argument("This file holds no tables of an archive.");
    }
    if (sound >= file.native->tables->sounds.size())
    {
        throw std::invalid_argument("The archive's tables hold no such sound.");
    }

    // The sound has to have the value.
    const format::NativeTables& tables = *file.native->tables;
    const format::NativeSound& old = tables.sounds[sound];
    if (OfSequenceSounds(change.value) && old.kind != ntdlib::sar::SoundType::kSequence)
    {
        throw std::invalid_argument("Sound " + old.name +
                                    " isn't a sequence sound. Only a sequence sound has bank slots, tracks, a "
                                    "channel priority and a release priority fix.");
    }
    if (file.native->wii)
    {
        CheckHeld(file, change);
    }
    CheckChange(tables, change);

    format::File result = file;
    const auto edit = [&](format::NativeArchive& native)
    {
        format::NativeSound& edited = native.tables->sounds[sound];
        const auto byte = static_cast<uint8_t>(change.to);
        switch (change.value)
        {
        case SoundValue::kPlayer:
            edited.player = change.to;
            break;
        case SoundValue::kVolume:
            edited.volume = byte;
            break;
        case SoundValue::kPanMode:
            edited.pan_mode = byte;
            break;
        case SoundValue::kPanCurve:
            edited.pan_curve = byte;
            break;
        case SoundValue::kPlayerPriority:
            edited.player_priority = byte;
            break;
        case SoundValue::kActorPlayer:
            edited.actor_player = byte;
            break;
        case SoundValue::kBank:
            edited.banks = WithBank(edited.banks, change.slot, change.to);
            break;
        case SoundValue::kTracks:
            edited.tracks = change.to;
            break;
        case SoundValue::kChannelPriority:
            edited.channel_priority = byte;
            break;
        case SoundValue::kReleasePriorityFix:
            edited.release_priority_fix = change.to != 0;
            break;
        }
    };
    result.native.Edit(edit);

    // A value whose option the sound's entry leaves out takes a place in the entry at the export. The new version has
    // to export then. An archive that NintendoWare's tools didn't lay out can't take the place. Without the places of
    // the first piece, every value that requires a place counts. Each value of an archive of the Wii has a fixed place.
    if (file.native->wii)
    {
        return result;
    }
    const std::optional<std::vector<ntdlib::sar::ValuePlaces>> places = format::TableValuePlacesOf(*file.native);
    const ntdlib::sar::ValuePlaces held =
        places && sound < places->size() ? (*places)[sound] : ntdlib::sar::ValuePlaces{};
    const ntdlib::sar::ValuePlaces before = format::NeededPlaces(old);
    const ntdlib::sar::ValuePlaces now = format::NeededPlaces(result.native->tables->sounds[sound]);
    if ((now.pan && !before.pan && !held.pan) || (now.priority && !before.priority && !held.priority) ||
        (now.channel && !before.channel && !held.channel))
    {
        format::ExportArchive(result);
    }

    return result;
}

} // namespace hokuto::edit
