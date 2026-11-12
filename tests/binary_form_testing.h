// SPDX-License-Identifier: MIT

// Reads and patches of the table of chunks of a Hokuto file in the binary form. The tests of the format and of the
// models share them. The table starts at byte 16 with the count of chunks. Each entry from byte 24 is 24 bytes long:
// the chunk's ID, its CRC-32, its start and its size.

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "xsflib/binary.h"
#include "xsflib/testing/check.h"

#include "format/file.h"
#include "format/text.h"

namespace hokuto::testing
{

// The IDs in the table of chunks of `binary`, a file in the binary form.
inline std::vector<std::string> ChunkIds(std::span<const uint8_t> binary)
{
    std::vector<std::string> ids;
    const uint32_t count = xsflib::LoadLe32(&binary[16]);
    for (std::size_t c = 0; c < count; ++c)
    {
        const uint8_t* entry = &binary[24 + 24 * c];
        ids.emplace_back(entry, entry + 4);
    }

    return ids;
}

// The bytes of chunk `chunk` of `binary`, by its place in the table of chunks.
inline std::span<const uint8_t> ChunkBytes(std::span<const uint8_t> binary, std::size_t chunk)
{
    const uint8_t* entry = &binary[24 + 24 * chunk];
    return binary.subspan(static_cast<std::size_t>(xsflib::LoadLe64(entry + 8)),
                          static_cast<std::size_t>(xsflib::LoadLe64(entry + 16)));
}

// The message that Read throws for `binary` with byte `at` of chunk `chunk` set to `value`, or nothing. The chunk's
// CRC-32 matches its new bytes.
inline std::string Patched(std::vector<uint8_t> binary, std::size_t chunk, std::size_t at, uint8_t value)
{
    uint8_t* entry = &binary[24 + 24 * chunk];
    const auto start = static_cast<std::size_t>(xsflib::LoadLe64(entry + 8));
    const auto size = static_cast<std::size_t>(xsflib::LoadLe64(entry + 16));
    binary[start + at] = value;
    xsflib::StoreLe32(entry + 4, format::Crc32(std::span(binary).subspan(start, size)));

    return xsflib::testing::ErrorOf<xsflib::FormatError>([&] { format::Read(binary); });
}

// The message that Read throws for `binary` with byte `at` of chunk `id` set to `value`, or nothing. The chunk's CRC-32
// matches its new bytes.
inline std::string Patched(std::vector<uint8_t> binary, std::string_view id, std::size_t at, uint8_t value)
{
    const std::vector<std::string> ids = ChunkIds(binary);
    const auto chunk = static_cast<std::size_t>(std::ranges::find(ids, id) - ids.begin());
    return Patched(std::move(binary), chunk, at, value);
}

// Whether `file` comes back from both forms unchanged.
inline bool ComesBack(const format::File& file)
{
    return format::Read(format::Write(file)) == file && format::ReadText(format::WriteText(file)) == file;
}

} // namespace hokuto::testing
