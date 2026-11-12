// SPDX-License-Identifier: MIT

// The reading of an SFZ file into regions, for sfz_imports.cpp. Only sfz_imports.cpp and sfz_parser.cpp include it.

#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "edit/sfz_imports.h"

namespace hokuto::edit::sfz_parser
{

using Opcodes = std::map<std::string, std::string, std::less<>>;

// `text` in lower case.
std::string Lowered(std::string_view text);

// The regions of an SFZ file, each with the opcodes that its sections give it, and the count of each section that
// Hokuto doesn't read.
struct SfzRegions
{
    std::vector<Opcodes> regions;
    std::map<std::string, std::size_t> other_sections;
};

// Reads the SFZ file at `path` through `read`. Its #include lines name files in `folder`. Throws std::invalid_argument
// with a message for the user for a file that `read` can't read, and for includes deeper than 16 files.
SfzRegions ReadSfzRegions(const std::filesystem::path& folder, const SfzReader& read,
                          const std::filesystem::path& path);

} // namespace hokuto::edit::sfz_parser
