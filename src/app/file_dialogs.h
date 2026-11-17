// SPDX-License-Identifier: MIT

// Helpers for the paths that the file dialogs give.

#pragma once

#include <string>
#include <string_view>

namespace hokuto::app
{

// The file from a dialog, with `extension` added unless its name ends in it. Every Hokuto file ends in .hok, in either
// form. An export's files end in the extension of their target. A name that ends in the long form of `extension`
// keeps it: .aiff for .aif, and .midi for .mid.
std::string WithExtension(const std::string& path, std::string_view extension);

// Whether `files`, the list of paths that a file dialog's callback gets, names a file. No list means an error, and an
// empty list means that the user cancelled. SDL3's zenity dialog gives a list of one empty path on a cancel, and an
// empty path counts as a cancel too.
bool NamesFile(const char* const* files);

} // namespace hokuto::app
