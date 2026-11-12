// SPDX-License-Identifier: MIT

// The helpers that the track edits of every engine use (edit/track_common.h).

#include "edit/track_common.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/commands/units.h"

#include "edit/bytecode.h"

namespace hokuto::edit
{

namespace
{

using ntdlib::commands::Command;
using ntdlib::commands::Kind;
using ntdlib::commands::kOne;
using ntdlib::commands::Sequence;
using ntdlib::commands::Source;
using track_detail::SoundStart;

} // namespace

namespace track_detail
{

SoundStart StartOf(const format::File& file, std::span<const std::optional<SoundPlace>> places, uint32_t sound)
{
    if (sound >= places.size() || !places[sound])
    {
        throw std::invalid_argument(
            "This sound has no sequence that Hokuto can edit. Either it isn't a sequence sound, or its sequence file "
            "contains bytes that no command reaches.");
    }

    const SoundPlace& place = *places[sound];
    return {place.sequence, place.start_field ? file.native->fields.at(*place.start_field).to.offset : 0};
}

uint32_t TrailingDataStart(const Sequence& sequence)
{
    auto at = static_cast<uint32_t>(sequence.items.size());
    while (at > 0 && sequence.items[at - 1].kind == Kind::kData)
    {
        at--;
    }

    return at;
}

Command CommandOf(Kind kind, int64_t value)
{
    Command command;
    command.kind = kind;
    command.value.value = value * kOne;

    return command;
}

std::vector<Command> CopyOf(const Sequence& sequence, std::span<const uint32_t> starts,
                            std::map<uint32_t, uint32_t>& moved, std::span<const Kind> left_out)
{
    // The items that the track runs.
    const auto count = static_cast<uint32_t>(sequence.items.size());
    std::vector<bool> runs(count, false);
    for (const uint32_t start : starts)
    {
        const std::vector<bool> reached = Reached(sequence, start, false);
        for (uint32_t item = 0; item < count; item++)
        {
            runs[item] = runs[item] || reached[item];
        }
    }

    // The copy keeps them in their order. An item left out leads to the next one that the copy keeps.
    std::vector<Command> copy;
    std::vector<uint32_t> skipped;
    for (uint32_t item = 0; item < count; item++)
    {
        const Command& command = sequence.items[item];
        if (!runs[item])
        {
            continue;
        }

        if (std::ranges::find(left_out, command.kind) != left_out.end())
        {
            skipped.push_back(item);
            continue;
        }

        for (const uint32_t out : skipped)
        {
            moved[out] = static_cast<uint32_t>(copy.size());
        }

        skipped.clear();
        moved[item] = static_cast<uint32_t>(copy.size());
        copy.push_back(command);
    }

    for (const uint32_t out : skipped)
    {
        moved[out] = static_cast<uint32_t>(copy.size());
    }

    moved[count] = static_cast<uint32_t>(copy.size());

    // The copy's jumps and calls go to its items.
    for (Command& command : copy)
    {
        if (ntdlib::commands::HasTarget(command.kind) && !command.target_outside)
        {
            command.target = moved.at(command.target);
        }
    }

    return copy;
}

std::optional<std::vector<Command>> InTimebase(std::vector<Command> commands, int from, int to)
{
    const auto scale = [from, to](ntdlib::commands::Argument& argument)
    {
        if (argument.source == Source::kVariable)
        {
            return false;
        }

        for (int64_t* value : {&argument.value, &argument.high})
        {
            const int64_t product = *value * to;
            if (product % (int64_t{from} * kOne) != 0)
            {
                return false;
            }

            *value = product / from;
        }

        return true;
    };

    for (Command& command : commands)
    {
        const Command before = command;
        const bool ticks = ntdlib::commands::UnitOf(command.kind) == ntdlib::commands::Unit::kTicks;
        if ((ticks && !scale(command.value)) || (command.time && !scale(*command.time)))
        {
            return std::nullopt;
        }

        if (command.value != before.value || command.time != before.time)
        {
            command.encoding.value_bytes = 0;
        }
    }

    return commands;
}

} // namespace track_detail

} // namespace hokuto::edit
