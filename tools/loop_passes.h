// SPDX-License-Identifier: MIT

// The loops of two reads of a sound that lie a whole number of passes apart. ntdlib's performer finds a loop where the
// tracks' states repeat. After an edit, it can find the same loop a pass earlier or later.

#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>

#include "song/sequence.h"

namespace hokuto::tools
{

// The end of the earlier loop, where the loops of `before` and `after` lie a whole number of passes apart. Nothing for
// other loops.
inline std::optional<uint32_t> EarlierEnd(const song::Sequence& before, const song::Sequence& after)
{
    if (!before.loop || !after.loop || before.loop == after.loop)
    {
        return std::nullopt;
    }

    const uint32_t length = before.loop->end - before.loop->start;
    const uint32_t apart =
        std::max(before.loop->start, after.loop->start) - std::min(before.loop->start, after.loop->start);
    if (length == 0 || after.loop->end - after.loop->start != length || apart % length != 0)
    {
        return std::nullopt;
    }

    return std::min(before.loop->end, after.loop->end);
}

} // namespace hokuto::tools
