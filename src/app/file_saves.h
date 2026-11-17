// SPDX-License-Identifier: MIT

// The save of a file over the last save: the bytes go to a new file beside it, and the new file then replaces the
// last save.

#pragma once

#include <cstdint>
#include <filesystem>
#include <span>

namespace hokuto::app
{

// How a save ended.
enum class SaveResult
{
    kSaved,
    kReadOnly, // the file exists and the user may not write to it
    kFailed,   // the disk is full, the folder is read-only, or the system refused for another reason
};

// The file that a save to `file` writes. On Linux and the Mac, that's the file at the end of the symbolic links that
// `file` starts, or `file` itself. A link's relative target starts at the link's folder. A link that can't be read, or
// a chain of more than 40 links, gives an empty path. On Windows, it's `file`.
std::filesystem::path SaveTarget(const std::filesystem::path& file);

// Saves `bytes` to `file`. The bytes go to a new file beside the target (SaveTarget). Then the new file replaces the
// target. A save that fails partway leaves the last save whole, and the new file goes. On Linux and the Mac, a save
// through a symbolic link keeps the link and changes the file that it leads to. A file that the user may not write to
// stays as it is, though its folder would let the new file take its place. The new file gets the old file's
// permissions.
SaveResult SaveFile(const std::filesystem::path& file, std::span<const uint8_t> bytes);

} // namespace hokuto::app
