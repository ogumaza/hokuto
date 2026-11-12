// SPDX-License-Identifier: MIT

#include "app/file_dialogs.h"

#include <cstddef>
#include <cstring>
#include <filesystem>

#include "xsflib/ascii.h"
#include "xsflib/files.h"

namespace hokuto::app
{

std::string WithExtension(const std::string& path, std::string_view extension)
{
    if (xsflib::AsciiLower(xsflib::Utf8(xsflib::PathFromUtf8(path).extension())) == extension)
    {
        return path;
    }

    return path + std::string(extension);
}

} // namespace hokuto::app
