// SPDX-License-Identifier: MIT

// A message box through zenity, the program that draws SDL3's message boxes on Linux: the arguments that show a box,
// the answer that zenity gives, and the events that the window leaves out while a box is open. system_ui_other.cpp
// runs zenity with them. Nothing here opens a window or runs a program, and the tests run without a display.

#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace hokuto::app
{

// The arguments that show `box` with zenity, the program's name first. They are the ones that SDL3 passes: a question
// with the box's icon, title and text, and one button for each of the box's buttons, in their order. `version` is what
// `zenity --version` writes. zenity 3.90 renamed the option of the icon.
std::vector<std::string> ZenityArguments(const SDL_MessageBoxData& box, std::string_view version);

// Reads the answer of `box` from zenity's exit code and from what zenity wrote. zenity writes the text of the chosen
// button, and `chosen` receives that button's number. A box closed without a button leaves `chosen` as it is. Returns
// false when zenity showed no box: it failed (exit code 255), or a signal ended it (a negative code).
bool ReadZenityAnswer(const SDL_MessageBoxData& box, int exit_code, std::string_view output, int& chosen);

// Whether the window leaves out an event of type `type` that arrives while a message box is open. The boxes of macOS
// and Windows block input to the window. zenity's box can't block it. The window therefore drops the input to it during
// a box: key presses, typed text, mouse presses, the wheel, pen and touch input, dropped files, and a click on the
// close button. The pointer's moves are dropped too, and the program reads the pointer's place once the box closes. The
// releases of keys and mouse buttons stay. Without the release of a key or a button that went down before the box, Dear
// ImGui would hold it down. Every other event stays too, such as the window's changes, a quit signal and the program's
// wake event.
bool LeftOutDuringBox(uint32_t type);

} // namespace hokuto::app
