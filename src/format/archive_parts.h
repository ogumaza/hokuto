// SPDX-License-Identifier: MIT

// What archive.cpp and archive_tables.cpp share (format/archive.h): the strict read of an archive, and the reads and
// writes of the native tables. Only those two files include it.

#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <variant>
#include <vector>

#include "ntdlib/sar/formats.h"

#include "format/file.h"

namespace hokuto::format::archive_parts
{

// The first piece of `native` when it is a run of bytes, or null.
inline const std::vector<uint8_t>* FirstRun(const NativeArchive& native)
{
    return native.pieces->empty() ? nullptr : std::get_if<std::vector<uint8_t>>(&native.pieces->front());
}

// Defined in archive.cpp.
std::optional<ntdlib::sar::SoundArchive> ReadStrictly(std::span<const uint8_t> archive, ntdlib::sar::Flavor flavor);

// Defined in archive_tables.cpp.
NativeTables TablesOf(const ntdlib::sar::SoundArchive& read);
std::optional<NativeTables> ReadWiiTables(std::span<const uint8_t> archive);
void WriteTables(std::vector<uint8_t>& archive, ntdlib::sar::Flavor flavor, const NativeTables& tables);
void WriteWiiTables(std::vector<uint8_t>& archive, const NativeTables& tables);
void WriteWiiStreamSizes(std::vector<uint8_t>& archive, const std::map<uint32_t, NativeStream>& streams);

} // namespace hokuto::format::archive_parts
