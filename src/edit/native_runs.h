// SPDX-License-Identifier: MIT

// The runs of a file's native layout: the bytes of the game's archive or ROM between its sequences
// (ntdlib/sar/layout.h). An edit of bytes that no sequence contains, such as the bytes of a GBA voice or a DS bank,
// changes them in the run that contains them. The layout keeps its size. format::NativePieceStarts returns the places
// of the pieces.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include "format/file.h"

namespace hokuto::edit
{

// Why no run contains some bytes: a sequence or padding contains some of them or they cross a piece's edge, a field's 4
// bytes are among them, or they run past the layout's end.
enum class RunMiss
{
    kSequence,
    kField,
    kPastEnd,
};

// Where bytes are in a run: the run's piece, and the bytes' offset in it.
struct RunPlace
{
    std::size_t piece = 0;
    std::size_t offset = 0;
};

// The run of the layout of `native` that contains `size` bytes at `offset` from the layout's start, or why none does.
// `starts` contains each piece's start, then the layout's end (format::NativePieceStarts). Throws std::invalid_argument
// for starts of another count.
std::variant<RunPlace, RunMiss> RunOf(const format::NativeArchive& native, std::span<const uint64_t> starts,
                                      uint64_t offset, uint64_t size);

// `file` with `bytes` at `place` of its native layout. The bytes have to fit in the run (RunOf).
format::File WithRunBytes(const format::File& file, const RunPlace& place, std::span<const uint8_t> bytes);

// `file` with `bytes` at `offset` of its native layout. The pieces of the layout start at `starts`. In the messages,
// `item` names the item that contains the bytes, such as "The bank". Throws std::invalid_argument with a message for
// the user when no run contains the bytes.
format::File WithArchiveBytes(const format::File& file, std::span<const uint64_t> starts, uint64_t offset,
                              std::span<const uint8_t> bytes, const std::string& item);

// Bytes at an offset of a file's native layout.
struct ArchiveBytes
{
    uint64_t offset = 0;
    std::vector<uint8_t> bytes;
};

// The same for each of `changes` in one edit of the layout: the file's pieces are copied once.
format::File WithArchiveBytes(const format::File& file, std::span<const uint64_t> starts,
                              std::span<const ArchiveBytes> changes, const std::string& item);

} // namespace hokuto::edit
