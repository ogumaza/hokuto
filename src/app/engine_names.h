// SPDX-License-Identifier: MIT

// The names of the engines that a file plays on, for the program's text.

#pragma once

#include <string>

#include "format/engine.h"

namespace hokuto::app
{

// The name of the console whose rules `engine` follows, such as "Wii U", for Project Setup, the mixer and the messages.
const char* EngineName(format::Engine engine);

// The engine of `engine` in a sentence: "the Wii U engine", or "Konami's driver". The name of a driver, such as
// Konami's, takes no article and no noun.
std::string TheEngine(format::Engine engine);

} // namespace hokuto::app
