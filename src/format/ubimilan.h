// SPDX-License-Identifier: MIT

// The waits of a sequence of Ubisoft Milan's driver in a Hokuto file. ntdlib's decoder sets each wait to the driver's
// countdown (ntdlib/ubimilan/commands.h). A file stores the length of each wait in ticks instead. A file of another
// driver stores its waits in ticks too. The first wait of a sequence, before the first command, runs that command as
// many frames into a piece as the countdown says. Every other countdown of n runs the next command n + 1 frames later,
// and a countdown of 0 runs it in the same frame. No countdown therefore stores a wait of one tick after a command. The
// library hokuto_driver_rules contains the functions (ubimilan_rules.cpp). The song layer links it without the format
// layer.

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/commands/units.h"
#include "ntdlib/ubimilan/commands.h"
#include "ntdlib/ubimilan/driver.h"

namespace hokuto::format
{

// The longest countdown: the largest value of its 2 bytes.
inline constexpr int64_t kUbiMilanLongestCountdown = 0xFFFF;

// Whether item `item` of `sequence` is its first wait: the wait after the 4 bytes that start a sequence.
bool IsUbiMilanFirstWait(const ntdlib::commands::Sequence& sequence, std::size_t item);

// `sequence` from ntdlib's decoder, with each wait converted to its length in ticks.
ntdlib::commands::Sequence UbiMilanTicks(ntdlib::commands::Sequence sequence);

// `sequence` with waits in ticks, converted to the countdowns that ntdlib's encoder uses. Throws std::invalid_argument
// with a message for the user for a wait that no countdown can store.
ntdlib::commands::Sequence UbiMilanCountdowns(ntdlib::commands::Sequence sequence);

// The offset of each item of `sequence`, with waits in ticks, in the driver's bytes, and the size of all the items. A
// whole sequence, from its 4 bytes to its end, uses the offsets from ntdlib's encoder. For other items, such as a
// single command, each item counts the bytes that the encoder writes for it, and no wait is a first wait. Throws
// std::invalid_argument with the reason for an item that the bytes can't store.
std::vector<uint32_t> UbiMilanItemOffsets(const ntdlib::commands::Sequence& sequence);

} // namespace hokuto::format
