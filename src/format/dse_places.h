// SPDX-License-Identifier: MIT

// Where each file of a set of DSE files sits in the game's cartridge image. A file of a set (Cartridge::places) keeps
// the record. The import makes it, and the image export reads it in place of a new search of the image.

#pragma once

#include <cstdint>
#include <string>

namespace hokuto::format
{

// The place of a file of a set of DSE files in the cartridge image. `image_file` names the image's file that holds the
// DSE file: its path, or "file " and its number for a file without a path (DseFileName). The DSE file's bytes are the
// `size` bytes at `offset` there. A file that the image keeps compressed sits in the decompressed bytes of that file.
// Where the image's file is a GFSA archive, the archive's file that holds it comes in `path` after `image_file`, and
// the offset counts in that file's decompressed bytes.
struct DsePlace
{
    friend bool operator==(const DsePlace&, const DsePlace&) = default;

    std::string path; // the DSE file's path in the set
    std::string image_file;
    uint32_t offset = 0;
    uint32_t size = 0;
    bool compressed = false;
};

} // namespace hokuto::format
