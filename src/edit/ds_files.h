// SPDX-License-Identifier: MIT

// The replacements of the files of a DS sound archive (SDAT): a sequence, a bank (SBNK), a wave archive (SWAR) and a
// wave. Each file of the archive is a run of its own in the file's native layout (ntdlib::sdat::Cut). A new file can
// take another size. The join then lays the files after it out again, and the archive's FAT and sizes follow
// (ntdlib::sdat::ReplaceFile). Every item that names the file reads the new bytes.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/sdat/formats.h"

#include "format/file.h"
#include "song/ds.h"

namespace hokuto::edit
{

// Throws std::invalid_argument with a message for the user where `records`, the records of one kind of a DS archive,
// such as its banks, has no record `index`. `kind` names the kind in the message, such as "bank".
template <typename Record>
void CheckDsRecord(const std::vector<std::optional<Record>>& records, uint32_t index, std::string_view kind)
{
    if (index >= records.size() || !records[index])
    {
        throw std::invalid_argument("The archive has no " + std::string(kind) + " " + std::to_string(index) + ".");
    }
}

// The DS files in `bytes`: a bank, a wave archive, a sequence file and a stream. Each throws std::invalid_argument with
// a message for the user for bytes that ntdlib can't read as such a file.
ntdlib::sdat::Bank ReadDsBank(std::span<const uint8_t> bytes);
ntdlib::sdat::WaveArchive ReadDsWaveArchive(std::span<const uint8_t> bytes);
ntdlib::commands::Sequence ReadDsSequenceFile(std::span<const uint8_t> bytes);
ntdlib::sdat::Stream ReadDsStream(std::span<const uint8_t> bytes);

// `file`, a file of a DS archive, with `bytes` for file `file_id` of the archive's FAT. Throws std::invalid_argument
// with a message for the user for a file past the FAT, for a file that sits among other bytes or shares its bytes with
// another file, and for an archive of another size whose file in the cartridge image contains other data too
// (format::ExportDsImage refuses it).
format::File WithDsFile(const format::File& file, uint32_t file_id, std::span<const uint8_t> bytes);

// `file` with `bytes`, a bank file, in the place of bank `bank`'s file. The bank keeps its slots of wave archives.
// `archive` is the archive that `file` builds. Throws std::invalid_argument with a message for the user for a bank that
// is missing from the archive, for bytes that aren't a bank file, and as WithDsFile does.
format::File ReplaceDsBank(const format::File& file, const song::DsArchive& archive, uint32_t bank,
                           std::span<const uint8_t> bytes);

// What a user should know about `bytes`, a bank file, in the place of bank `bank`'s file: the programs of the current
// bank that the new one doesn't hold, and the regions whose waves the bank's slots of wave archives don't hold. Each
// line is a sentence. Throws as ReplaceDsBank does.
std::vector<std::string> DsBankWarnings(const song::DsArchive& archive, uint32_t bank, std::span<const uint8_t> bytes);

// The instruments of `bytes`, a bank file. Throws as ReplaceDsBank does.
std::size_t DsBankInstruments(std::span<const uint8_t> bytes);

// `file` with `bytes`, a wave archive file, in the place of wave archive `wave_archive`'s file. Throws
// std::invalid_argument with a message for the user for a wave archive that is missing from the archive, for bytes that
// aren't a wave archive file, and as WithDsFile does.
format::File ReplaceDsWaveArchive(const format::File& file, const song::DsArchive& archive, uint32_t wave_archive,
                                  std::span<const uint8_t> bytes);

// What a user should know about `bytes`, a wave archive file, in the place of wave archive `wave_archive`'s file: the
// regions of the archive's banks that play a wave past the new file's waves. Each line is a sentence. Throws as
// ReplaceDsWaveArchive does.
std::vector<std::string> DsWaveArchiveWarnings(const song::DsArchive& archive, uint32_t wave_archive,
                                               std::span<const uint8_t> bytes);

// The waves of `bytes`, a wave archive file. Throws as ReplaceDsWaveArchive does.
std::size_t DsWaveArchiveWaves(std::span<const uint8_t> bytes);

// `file` with the commands of `bytes`, a sequence file (SSEQ), in the place of sound `sound`'s sequence. Every sound
// that plays the sequence gets the new commands. The sound keeps its bank and its other values. Throws
// std::invalid_argument with a message for the user for a sound that plays no sequence of its own, such as a sound of a
// sequence archive, and for bytes whose commands Hokuto can't read (ntdlib::sdat::ReadSequenceFile).
format::File ReplaceDsSequence(const format::File& file, const song::DsArchive& archive, uint32_t sound,
                               std::span<const uint8_t> bytes);

// The sounds that play the sequence of sound `sound`, `sound` first. Empty for a sound without a sequence of its own.
std::vector<uint32_t> DsSoundsOfSequence(const format::File& file, const song::DsArchive& archive, uint32_t sound);

// The sounds that play bank `bank`, in order.
std::vector<uint32_t> DsSoundsOfBank(const song::DsArchive& archive, uint32_t bank);

// The sounds whose banks name wave archive `wave_archive` in a slot, in order.
std::vector<uint32_t> DsSoundsOfWaveArchive(const song::DsArchive& archive, uint32_t wave_archive);

// The game of `file`'s DS archive: where the archive is in the cartridge image that the file keeps (song::DsGameOf). A
// file without a cartridge image has none. The file keeps the image without the 0xFF bytes at its end, and an image
// whose last file ends in such bytes is read from a copy padded back to its size.
std::optional<song::DsGame> DsGameOfFile(const format::File& file);

} // namespace hokuto::edit
