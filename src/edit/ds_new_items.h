// SPDX-License-Identifier: MIT

// New items in a file's DS sound archive (SDAT): File > Import > Sequence…, Bank…, Wave Archive… and Stream…, and
// Project > Add Sequence Sound… and Add Bank…. ntdlib reads the fields of an archive that the SDK's tools laid out, and
// lays one out again (ntdlib::sdat::ReadArchiveModel and BuildArchive). The record of the new item is added at the end
// of its table, and its file at the end of the FAT. Each other item keeps its number, and each other file its bytes. A
// sound of a sequence archive keeps its number too (song::kFirstDsArchivedSound). The file's sequences keep theirs, and
// the new one follows them. An archive that the tools didn't lay out accepts no new item. The game's code plays a sound
// by its number. Only a change to that code plays a new sound in the game.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "format/file.h"
#include "song/ds.h"

namespace hokuto::edit
{

// The kinds of new item that come from a file.
enum class DsNewItem
{
    kSequence,
    kBank,
    kWaveArchive,
    kStream,
};

// The values of a sound of the DS that a new sequence sound copies.
struct DsSoundValues
{
    friend bool operator==(const DsSoundValues&, const DsSoundValues&) = default;

    uint16_t bank = 0;
    uint8_t volume = 127;
    uint8_t channel_priority = 64;
    uint8_t player_priority = 64;
    uint8_t player = 0;
};

// The values of sound `sound` of `archive`, from its record in the table of sequences or in its sequence archive.
// Throws std::invalid_argument with a message for the user for a sound that is missing from the archive.
DsSoundValues DsSoundValuesOf(const song::DsArchive& archive, uint32_t sound);

// Why the archive of `file` accepts no new item, or nothing when it accepts one.
std::optional<std::string> DsAdditionsRefused(const format::File& file);

// The names of `archive`'s items, sorted. A new item's name can't be among them.
std::vector<std::string> DsTakenNames(const song::DsArchive& archive);

// `file` once its archive gains a sequence sound named `name` that plays `bytes`, a sequence file, with `values`. The
// new sound's number is the length of the archive's table of sequences. An archive without names keeps none: `name`
// counts for nothing there. Throws std::invalid_argument with a message for the user for bytes that aren't a sequence
// file that Hokuto reads, a name that is empty or that the archive has, a bank or a player past the archive's tables, a
// table of sequences with no room below song::kFirstDsArchivedSound, and an archive that accepts no new item.
format::File AddDsSequenceSound(const format::File& file, const std::string& name, std::span<const uint8_t> bytes,
                                const DsSoundValues& values);

// `file` once its archive gains a bank named `name` with `bytes`, a bank file. `wave_archives` contains the wave
// archive of each of the four slots of the bank, or 0xFFFF for none. The new bank's number is the length of the
// archive's table of banks. Throws std::invalid_argument as AddDsSequenceSound does, for bytes that aren't a bank file
// and for a wave archive past the archive's table.
format::File AddDsBank(const format::File& file, const std::string& name, std::span<const uint8_t> bytes,
                       const std::array<uint16_t, 4>& wave_archives);

// `file` once its archive gains a wave archive named `name` with `bytes`, a wave archive file. The new wave archive's
// number is the length of the archive's table of wave archives. A file of more than 2 MiB, half of the DS's main
// memory, gets flag bit 24: its waves load one at a time. Throws std::invalid_argument as AddDsSequenceSound does, and
// for bytes that aren't a wave archive file.
format::File AddDsWaveArchive(const format::File& file, const std::string& name, std::span<const uint8_t> bytes);

// The wave archives that the archive of `file` contains. A new wave archive gets this number.
std::size_t DsWaveArchiveCount(const format::File& file);

// `file` once its bank `bank` uses `wave_archives` in its four slots, in the form that AddDsBank accepts. Throws
// std::invalid_argument as AddDsBank does, and for a bank that is missing from the archive.
format::File WithDsBankSlots(const format::File& file, uint32_t bank, const std::array<uint16_t, 4>& wave_archives);

// `file` once its bank `bank` gets the name `name`, `bytes`, a bank file, and `wave_archives` in its four slots. Throws
// std::invalid_argument as AddDsBank does, and for a bank that is missing from the archive.
format::File WithDsBankFile(const format::File& file, uint32_t bank, const std::string& name,
                            std::span<const uint8_t> bytes, const std::array<uint16_t, 4>& wave_archives);

// The values of a stream of the DS that a new stream copies.
struct DsStreamSettings
{
    friend bool operator==(const DsStreamSettings&, const DsStreamSettings&) = default;

    uint8_t volume = 127;
    uint8_t player_priority = 64;
    uint8_t player = 0;
    uint8_t flags = 0;
};

// The values of stream `stream` of `archive`, from its record. Throws std::invalid_argument with a message for the user
// for a stream that is missing from the archive.
DsStreamSettings DsStreamSettingsOf(const song::DsArchive& archive, uint32_t stream);

// `file` once its archive gains a stream named `name` that plays `bytes`, a stream file, with `settings`. The new
// stream's number is the length of the archive's table of streams. Throws std::invalid_argument as AddDsSequenceSound
// does, for bytes that aren't a stream file, and for a stream player past the archive's table.
format::File AddDsStream(const format::File& file, const std::string& name, std::span<const uint8_t> bytes,
                         const DsStreamSettings& settings);

// The bank file of a bank without instruments.
std::vector<uint8_t> EmptyDsBankFile();

// The sequence file of a song without notes: track 0 waits for 16 bars of 4 beats and ends. The editors write its notes
// in that wait. A note can't go past a track's end on the DS.
std::vector<uint8_t> EmptyDsSequenceFile();

// The slots of a wave archive that the regions of `bytes`, a bank file, name, in their order, and the waves of each
// that they require: the highest wave's number plus 1. Throws std::invalid_argument with a message for the user for
// bytes that aren't a bank file.
struct DsBankSlot
{
    uint16_t slot = 0;
    uint32_t waves = 0;
};
std::vector<DsBankSlot> DsBankSlots(std::span<const uint8_t> bytes);

// What the Import window says of `bytes`, a sequence file, on bank `bank`: the programs that its commands select and
// the bank doesn't hold. A program from a variable or a random number doesn't count.
std::vector<std::string> DsSequenceWarnings(const song::DsArchive& archive, uint32_t bank,
                                            std::span<const uint8_t> bytes);

// What the Import window says of `bytes`, a bank file, on `wave_archives`: the regions whose slots name no wave
// archive, or a wave archive without the wave.
std::vector<std::string> DsNewBankWarnings(const song::DsArchive& archive, std::span<const uint8_t> bytes,
                                           const std::array<uint16_t, 4>& wave_archives);

} // namespace hokuto::edit
