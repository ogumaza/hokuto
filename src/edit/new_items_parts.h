// SPDX-License-Identifier: MIT

// What new_items.cpp and new_stream_items.cpp share: the archive's tables, the check of a new item's name and the
// rebuilt archive. Only those files include it.

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>

#include "ntdlib/sar/formats.h"
#include "ntdlib/sar/layout.h"

#include "format/file.h"

namespace hokuto::edit::new_items_parts
{

namespace sar = ntdlib::sar;

// The tables of `file`'s archive. Throws std::invalid_argument with a message for the user for a file without them.
// `item` names the new item in the message, such as "sound".
const format::NativeTables& TablesOf(const format::File& file, const std::string& item);

// Checks the name of a new item, `item` in the messages, for an archive of `flavor` whose STRG block is `strings`.
// Throws std::invalid_argument with a message for the user for a name that the archive holds already, and for a name in
// an archive without names. With `required`, the item requires a name: an empty one and an archive without names throw
// too.
void CheckName(const std::optional<std::span<const uint8_t>>& strings, sar::Flavor flavor, const std::string& name,
               const std::string& item, bool required);

// The file that `archive` makes with the group files of `file`, with the new files' sequences of `added`.
format::File RebuiltArchive(const format::File& file, std::span<const uint8_t> archive,
                            const sar::KnownSequences& added = {});

} // namespace hokuto::edit::new_items_parts
