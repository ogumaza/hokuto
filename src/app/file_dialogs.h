// SPDX-License-Identifier: MIT

// Helpers for the paths that the file dialogs give.

#pragma once

#include <string>
#include <string_view>

namespace hokuto::app
{

// The file from a dialog, with `extension` added unless its name ends in it. Every Hokuto file ends in .hok, in either
// form. An export's files end in the extension of their target.
std::string WithExtension(const std::string& path, std::string_view extension);

} // namespace hokuto::app
