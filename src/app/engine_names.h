// SPDX-License-Identifier: MIT

// The names of the engines that a file plays on, for the program's text. A driver isn't a console: the text names an
// engine by its console, with the driver where the console has another one.

#pragma once

#include <string>

#include "format/engine.h"

namespace hokuto::app
{

// The console that `engine` runs on, such as "Wii U" or "DS".
const char* ConsoleName(format::Engine engine);

// The sound library or driver of `engine`, such as "nn::atk", "NITRO-Composer" or "Konami's driver".
const char* DriverName(format::Engine engine);

// Whether `engine` is the first engine of its console in format::kEngineFacts, such as NITRO-Composer on the DS and
// MusicPlayer2000 on the GBA. The text names the console alone for such an engine, and adds the driver for the others.
bool IsConsoleEngine(format::Engine engine);

// The name of `engine` for Project Setup, the Undo History, the List Editor and the messages: the console, such as
// "Wii U", followed by the driver where it isn't the console engine, such as "DS (DSE)" or "GBA (Konami's driver)".
std::string EngineName(format::Engine engine);

// The engine of `engine` in a sentence: "the Wii U engine", or "the GBA engine (Konami's driver)".
std::string TheEngine(format::Engine engine);

} // namespace hokuto::app
