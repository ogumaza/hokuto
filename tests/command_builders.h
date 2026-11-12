// SPDX-License-Identifier: MIT

// Builders of the commands of a sequence. The tests of the edits and the conversions of commands share them. Each test
// includes this header and names the builders that it uses with using-declarations.

#pragma once

#include <cstdint>
#include <utility>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/commands/units.h"

namespace hokuto::testing
{

// A command of `kind` with `value`.
inline ntdlib::commands::Command Setting(ntdlib::commands::Kind kind, int value)
{
    ntdlib::commands::Command command;
    command.kind = kind;
    command.value = {.source = ntdlib::commands::Source::kValue, .value = value * ntdlib::commands::kOne, .high = 0};

    return command;
}

// A command of `kind` that takes its value from `variable`.
inline ntdlib::commands::Command FromVariable(ntdlib::commands::Kind kind, int variable)
{
    ntdlib::commands::Command command;
    command.kind = kind;
    command.value = {.source = ntdlib::commands::Source::kVariable, .value = variable, .high = 0};

    return command;
}

// A command of `kind` with a random value from `low` to `high`.
inline ntdlib::commands::Command Random(ntdlib::commands::Kind kind, int low, int high)
{
    ntdlib::commands::Command command;
    command.kind = kind;
    command.value = {.source = ntdlib::commands::Source::kRandom,
                     .value = low * ntdlib::commands::kOne,
                     .high = high * ntdlib::commands::kOne};

    return command;
}

// A command of `kind` that goes to item `target`, with `index`, such as a track's number.
inline ntdlib::commands::Command WithTarget(ntdlib::commands::Kind kind, uint32_t target, uint8_t index = 0)
{
    ntdlib::commands::Command command;
    command.kind = kind;
    command.target = target;
    command.index = index;

    return command;
}

// A command of `kind` without a value.
inline ntdlib::commands::Command Plain(ntdlib::commands::Kind kind)
{
    ntdlib::commands::Command command;
    command.kind = kind;

    return command;
}

// `command`, run only if the last comparison held.
inline ntdlib::commands::Command Conditional(ntdlib::commands::Command command)
{
    command.conditional = true;

    return command;
}

// A sequence of `items`, without labels.
inline ntdlib::commands::Sequence Of(std::vector<ntdlib::commands::Command> items)
{
    return {.items = std::move(items), .labels = {}};
}

} // namespace hokuto::testing
