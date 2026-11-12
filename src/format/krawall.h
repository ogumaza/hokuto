// SPDX-License-Identifier: MIT

// What the rules of Krawall in the format layer and the song layer share (format/krawall.cpp, song/krawall.cpp).

#pragma once

#include "ntdlib/commands/sequence.h"
#include "ntdlib/commands/units.h"
#include "ntdlib/krawall/commands.h"

namespace hokuto::format
{

// A sequence that contains `item` as the bytes of a pattern can: the pattern's start, the item and the end of its row.
// A pattern's start and a wait require no more. A check of an item's values encodes it.
inline ntdlib::commands::Sequence KrawallAlone(const ntdlib::commands::Command& item)
{
    using ntdlib::commands::Kind;
    if (item.kind == Kind::kOther && item.encoding.opcode == ntdlib::krawall::kPatternOpcode)
    {
        return ntdlib::commands::Sequence{{item}, {}};
    }

    ntdlib::commands::Command start;
    start.kind = Kind::kOther;
    start.encoding.opcode = ntdlib::krawall::kPatternOpcode;
    ntdlib::commands::Sequence alone{{start, item}, {}};
    if (item.kind != Kind::kWait)
    {
        ntdlib::commands::Command wait;
        wait.kind = Kind::kWait;
        wait.value.value = ntdlib::commands::kOne;
        alone.items.push_back(wait);
    }

    return alone;
}

} // namespace hokuto::format
