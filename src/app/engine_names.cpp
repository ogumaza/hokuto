// SPDX-License-Identifier: MIT

#include "app/engine_names.h"

#include <array>
#include <cstddef>

#include "app/engine_rows.h"
#include "format/engines.h"

namespace hokuto::app
{
namespace
{

// The consoles' names, in the order of format::Platform.
constexpr std::array<const char*, 6> kConsoleNames = {"Switch", "Wii U", "3DS", "Wii", "GBA", "DS"};

static_assert(static_cast<std::size_t>(format::Platform::kDs) + 1 == kConsoleNames.size(),
              "kConsoleNames needs a name for each console, in the order of format::Platform");

} // namespace

const char* ConsoleName(format::Engine engine)
{
    return kConsoleNames[static_cast<std::size_t>(format::FactsOf(engine).platform)];
}

const char* DriverName(format::Engine engine)
{
    return RowOf(engine).driver;
}

bool IsConsoleEngine(format::Engine engine)
{
    const format::Platform platform = format::FactsOf(engine).platform;
    for (const format::EngineFacts& facts : format::kEngineFacts)
    {
        if (facts.platform == platform)
        {
            return facts.engine == engine;
        }
    }

    return true;
}

std::string EngineName(format::Engine engine)
{
    const std::string console = ConsoleName(engine);
    return IsConsoleEngine(engine) ? console : console + " (" + DriverName(engine) + ")";
}

std::string TheEngine(format::Engine engine)
{
    const std::string console = std::string("the ") + ConsoleName(engine) + " engine";
    return IsConsoleEngine(engine) ? console : console + " (" + DriverName(engine) + ")";
}

} // namespace hokuto::app
