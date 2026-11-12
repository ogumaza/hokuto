// SPDX-License-Identifier: MIT

// Small helpers that several check tools use: comparisons of decoded waves, the choice of a spread of items, and the
// tests of the messages that the edits throw. The library hokuto_check_support contains them.

#pragma once

#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "ntdlib/sar/decode.h"

#include "song/ds.h"
#include "song/sequence.h"

namespace hokuto::tools
{

// Whether `error`, or the error nested in it, is ntdlib::sdat::HeapFullError: a sound's files don't fit the DS's
// memory.
bool HeapFull(const std::exception& error);

// Whether `error` is edit::WithGbaNewBytes's refusal of new bytes for want of room in the ROM.
bool LacksRoom(const std::invalid_argument& error);

// Up to `most` indices from 0 to `count`, spread evenly.
std::vector<std::size_t> SpreadIndices(std::size_t count, std::size_t most);

// The loop from `start` to `end`, or "none".
std::string LoopText(const std::optional<song::Loop>& loop);

// The track numbered `number` of `sound`, or nullptr.
const song::Track* TrackNumbered(const song::Sequence& sound, int number);

// The track of `sound` with the most notes, the lowest first. Nothing for a sound without notes.
const song::Track* BusiestTrack(const song::Sequence& sound);

// The most ticks that a frame of Rare's driver runs in `sequence`: the driver counts each frame as 1/60 s of the song's
// time, at the fastest tempo.
uint32_t RareFrameTicks(const song::Sequence& sequence);

// The output of `sound` of a DS archive for `seconds` on the console's model.
std::vector<float> RenderedDs(const std::shared_ptr<const song::DsArchive>& archive, uint32_t sound, double seconds);

// `value` moved by `step`, wrapping past `highest`, or as it is when it is past `highest`. Counts such a value in
// `kept`.
template <typename Value>
Value Moved(Value value, int step, int highest, std::size_t& kept)
{
    if (static_cast<long long>(value) > highest)
    {
        kept++;
        return value;
    }

    return static_cast<Value>((static_cast<int>(value) + step) % (highest + 1));
}

} // namespace hokuto::tools
