// SPDX-License-Identifier: MIT

// Helpers that the tests of the songs of MIDI files share. Each test lists a track's commands as tuples whose second
// element is the command's kind.

#pragma once

#include <tuple>
#include <vector>

#include "ntdlib/commands/sequence.h"

namespace hokuto::testing
{

// The commands of `commands` of kind `kind`.
template <typename Timed>
std::vector<Timed> OfKind(const std::vector<Timed>& commands, ntdlib::commands::Kind kind)
{
    std::vector<Timed> found;
    for (const Timed& command : commands)
    {
        if (std::get<1>(command) == kind)
        {
            found.push_back(command);
        }
    }

    return found;
}

} // namespace hokuto::testing
