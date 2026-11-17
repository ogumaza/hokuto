// SPDX-License-Identifier: MIT

#include "app/file_dialogs.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <utility>

#include "xsflib/ascii.h"
#include "xsflib/files.h"

namespace hokuto::app
{

namespace
{

// Two spellings of one kind of file, the short one first: an AIFF file and a MIDI file. The save dialogs' filters list
// both.
constexpr std::array<std::pair<std::string_view, std::string_view>, 2> kLongForms = {{
    {".aif", ".aiff"},
    {".mid", ".midi"},
}};

} // namespace

std::string WithExtension(const std::string& path, std::string_view extension)
{
    const std::string ending = xsflib::AsciiLower(xsflib::Utf8(xsflib::PathFromUtf8(path).extension()));
    const bool long_form =
        std::ranges::any_of(kLongForms, [&](const std::pair<std::string_view, std::string_view>& forms)
                            { return forms.first == extension && forms.second == ending; });
    if (ending == extension || long_form)
    {
        return path;
    }

    return path + std::string(extension);
}

bool NamesFile(const char* const* files)
{
    return files != nullptr && files[0] != nullptr && files[0][0] != '\0';
}

} // namespace hokuto::app
