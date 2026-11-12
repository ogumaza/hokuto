// SPDX-License-Identifier: MIT

#include "app/scene_values.h"

#include <cstddef>

#include "format/file.h"

namespace hokuto::app
{

song::StartValues SceneStartValues(const format::Scene& scene)
{
    song::StartValues values;
    for (const auto& [variable, value] : scene.values)
    {
        if (value != -1 || variable >= song::kFirstGlobal)
        {
            values[variable] = value;
        }
    }

    return values;
}

song::Changes SceneChanges(const format::Scene& scene)
{
    song::Changes changes;
    for (const format::VariableChange& change : scene.changes)
    {
        changes.push_back({change.time, change.variable, change.value});
    }

    return changes;
}

std::vector<format::VariableChange> FileChanges(const song::Changes& changes)
{
    std::vector<format::VariableChange> scene;
    for (const song::VariableChange& change : changes)
    {
        if (change.variable >= 0 && change.variable < format::kSceneVariables)
        {
            scene.push_back({change.time, static_cast<uint8_t>(change.variable), change.value});
        }
    }

    return scene;
}

} // namespace hokuto::app
