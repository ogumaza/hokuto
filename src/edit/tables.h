// SPDX-License-Identifier: MIT

// The edits of a sound's values in the archive's tables: its player, volume, pan mode, pan curve, player priority and
// actor player, and a sequence sound's bank slots, tracks, channel priority and release priority fix. The Sound
// sections of the inspector and the browser make them. An export writes them into the archive (format/archive.h), and
// the program then plays the sound with them. ntdlib's model of nn::atk or nw::snd plays a sequence sound with its
// volume, pan, banks, tracks, channel priority and release priority fix. The player, the player priority and the actor
// player determine which sounds play at once in a game. Hokuto plays one sound at a time.

#pragma once

#include <cstdint>
#include <string>

#include "format/file.h"

namespace hokuto::edit
{

// A value of a sound in the archive's tables. Every sound has the values up to the actor player. A sequence sound has
// the others too.
enum class SoundValue
{
    kPlayer,
    kVolume,
    kPanMode,
    kPanCurve,
    kPlayerPriority,
    kActorPlayer,
    kBank,
    kTracks,
    kChannelPriority,
    kReleasePriorityFix,
};

// A new value of a sound: a player's item ID, a bank's item ID or 0xFFFFFFFF for an empty bank slot, a number, the
// tracks' flags with a bit for each track, or 0 and 1 for the release priority fix. `slot` is a bank's slot, from 0 to
// 3.
struct SoundChange
{
    SoundValue value = SoundValue::kVolume;
    int slot = 0;
    uint32_t to = 0;
};

// The values of its sounds that an archive's tables hold. An archive of the Wii holds one bank slot and no actor player
// that ntdlib reads. It holds the pan mode and the pan curve from version 1.2 on, and the release priority fix from
// version 1.3 on. The other consoles' archives hold every value: a value without a place in a sound's entry takes one
// at the export.
struct HeldValues
{
    int bank_slots = 4;
    bool actor_player = true;
    bool pan = true;
    bool release_priority_fix = true;
};

// The values that the archive of `file` holds.
HeldValues HeldValuesOf(const format::File& file);

// The highest number that `value` takes: 255 for the volume, 1 for the pan mode and the release priority fix, 8 for the
// pan curve, 127 for the priorities, 3 for the actor player and 0xFFFF for the tracks' flags. A player and a bank take
// item IDs instead.
uint32_t HighestOf(SoundValue value);

// The value of `sound` that `value` names, in a SoundChange's terms. A bank slot past the sound's list of banks holds
// 0xFFFFFFFF.
uint32_t ValueOf(const format::NativeSound& sound, SoundValue value, int slot);

// The name of `value` in the Undo History and in messages, such as "Pan Curve".
std::string NameOf(SoundValue value);

// `file` after `change` of sound `sound`'s values. A bank in a slot past the sound's list of banks makes the list
// longer, with empty slots before the bank. Empty slots at the list's end leave it, but a list keeps its first slot.
// NintendoWare's tools write a sound's list up to the last bank that the sound plays. A value whose option the sound's
// entry leaves out takes a place in the entry at the export (format::NeededPlaces). The edit then exports the new
// version to check that the archive can take the place. Throws std::invalid_argument with a message for the user for a
// file without the archive's tables, a sound past them, a value that the sound doesn't have, a player or a bank that
// the tables lack, a slot past 3, a number past its range, tracks' flags without a track, and a place that the archive
// can't take. On the Wii, the export writes each value in place, and the edit throws for a value that the archive
// doesn't hold (HeldValuesOf).
format::File ChangeSound(const format::File& file, uint32_t sound, const SoundChange& change);

} // namespace hokuto::edit
