// SPDX-License-Identifier: MIT

#include "app/engine_names.h"

#include "format/engines.h"

namespace hokuto::app
{
namespace
{

// Whether every engine's title ends in a null character. EngineName returns a title as a C string.
constexpr bool TitlesEndInNull()
{
    for (const format::EngineFacts& facts : format::kEngineFacts)
    {
        if (facts.title.data()[facts.title.size()] != '\0')
        {
            return false;
        }
    }

    return true;
}

static_assert(TitlesEndInNull(), "EngineName gives each engine's title as a C string");

} // namespace

const char* EngineName(format::Engine engine)
{
    return format::FactsOf(engine).title.data();
}

std::string TheEngine(format::Engine engine)
{
    const std::string name = EngineName(engine);
    return name.ends_with("'s driver") ? name : "the " + name + " engine";
}

} // namespace hokuto::app
