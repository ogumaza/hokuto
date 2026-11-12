// SPDX-License-Identifier: MIT

// A file and a folder in the system's temporary folder, for the tests that read from a path. Their names start with
// "hokuto_test_" and a random number.

#pragma once

#include <cstdint>
#include <filesystem>
#include <random>
#include <span>
#include <string>
#include <system_error>

#include "xsflib/files.h"

namespace hokuto::testing
{

// The path of a new name in the system's temporary folder, with `extension` at its end.
inline std::filesystem::path TemporaryPath(const std::string& extension)
{
    return std::filesystem::temp_directory_path() /
           ("hokuto_test_" + std::to_string(std::random_device{}()) + extension);
}

// A file in the system's temporary folder with `bytes` in it, deleted again with the object.
class TemporaryFile
{
public:
    explicit TemporaryFile(std::span<const uint8_t> bytes) : path_(TemporaryPath(".bfsar"))
    {
        xsflib::WriteFile(path_, bytes);
    }

    TemporaryFile(const TemporaryFile&) = delete;
    TemporaryFile& operator=(const TemporaryFile&) = delete;

    ~TemporaryFile()
    {
        std::error_code error;
        std::filesystem::remove(path_, error);
    }

    const std::filesystem::path& Path() const
    {
        return path_;
    }

private:
    std::filesystem::path path_;
};

// A folder in the system's temporary folder, deleted again with the object and the files in it.
class TemporaryFolder
{
public:
    TemporaryFolder() : path_(TemporaryPath(""))
    {
        std::filesystem::create_directory(path_);
    }

    TemporaryFolder(const TemporaryFolder&) = delete;
    TemporaryFolder& operator=(const TemporaryFolder&) = delete;

    ~TemporaryFolder()
    {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    const std::filesystem::path& Path() const
    {
        return path_;
    }

private:
    std::filesystem::path path_;
};

} // namespace hokuto::testing
