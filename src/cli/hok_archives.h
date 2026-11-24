// SPDX-License-Identifier: MIT

// The archives of an input that a command of hok runs on. A GBA ROM, and a .hok file of one, can contain an archive for
// each driver of the ROM.

#pragma once

#include <optional>
#include <span>
#include <vector>

#include "format/engine.h"

namespace hokuto::cli
{

// A run of a command on one archive of its input. `driver` is the engine of the archive, or nothing for the first
// archive. With `engine_folder`, the run writes into a folder named after that engine, inside the command's folder.
struct ArchiveRun
{
    std::optional<format::Engine> driver;
    bool engine_folder = false;
};

// The runs of a command on an input whose archives have the engines `engines`. `driver` is the engine that --driver
// names. A command that takes chosen sounds (`takes_sounds`, such as hok midi with --sound) runs once. It takes the
// archive of `driver`, or else the first archive. So does any command on an input of one archive. A command that
// writes every sound runs once for each archive, with a folder for each.
inline std::vector<ArchiveRun> ArchiveRuns(std::span<const format::Engine> engines,
                                           std::optional<format::Engine> driver, bool takes_sounds)
{
    if (driver || takes_sounds || engines.size() < 2)
    {
        return {{driver, false}};
    }

    std::vector<ArchiveRun> runs;
    for (const format::Engine engine : engines)
    {
        runs.push_back({engine, true});
    }

    return runs;
}

} // namespace hokuto::cli
