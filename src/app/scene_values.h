// SPDX-License-Identifier: MIT

// The scenes of the Variables zone as the starting values and changes of a sound's variables, and back.

#pragma once

#include <vector>

#include "format/file.h"
#include "song/sequence.h"

namespace hokuto::app
{

// A scene's values as the starting values of its sound's variables. The starting values leave out a player variable's
// -1, the value that it has without a choice. A global variable keeps -1: on the Wii, it starts at 0 without a choice.
song::StartValues SceneStartValues(const format::Scene& scene);

// A scene's changes as the changes of its sound's variables, and the other way round.
song::Changes SceneChanges(const format::Scene& scene);
std::vector<format::VariableChange> FileChanges(const song::Changes& changes);

} // namespace hokuto::app
