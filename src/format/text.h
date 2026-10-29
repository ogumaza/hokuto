// SPDX-License-Identifier: MIT

// The text form of Hokuto's files. It holds what the binary form holds, one statement a line: the engine, each
// sequence's labels and commands, the native archive's pieces with their bytes in base64, and the chunks that Hokuto
// doesn't read. Commands name their targets by label. A target without a label gets a local one, such as .L1. The text
// form converts to the binary form and back without loss. docs/format.md describes it.

#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "format/file.h"

namespace hokuto::format
{

// The text form of `file`. Throws std::invalid_argument for a file that the form can't hold: one with two labels of the
// same name in a sequence.
std::string WriteText(const File& file);

// Reads a file in the text form. A UTF-8 byte-order mark may start it. Throws xsflib::FormatError with the line's
// number and a message for the user.
File ReadText(std::string_view text);

// Whether `bytes` start as a file in the text form, after a byte-order mark if there is one.
bool IsText(std::span<const uint8_t> bytes);

// Reads a file in either form. Throws xsflib::FormatError for bytes in neither.
File ReadEitherForm(std::span<const uint8_t> bytes);

} // namespace hokuto::format
