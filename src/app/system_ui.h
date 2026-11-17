// SPDX-License-Identifier: MIT

// The parts of the interface that the system draws: the menu bar and the main window's title bar. system_ui_mac.mm
// builds them with AppKit. On Windows, system_ui_windows.cpp leaves the menus to the program and colours the title bar
// through the Desktop Window Manager. On other systems, system_ui_other.cpp leaves the menus to the program and the
// title bar to the system. The Mac also requires a way to end a wait for events at a quit signal. Times of day follow
// the system's format, and the paths of the system's dialogs one form. While a message box of macOS or Windows is open,
// the window still replies to the system's pings. On other systems, system_ui_other.cpp pumps the window's events
// during a box.

#pragma once

#include <SDL3/SDL.h>
#include <imgui.h>

#include <chrono>
#include <cstdint>
#include <span>
#include <string>

#include "app/menus.h"

namespace hokuto::app
{

// Puts `menus` in the system's menu bar, after the application menu. The Window menu gets the system's items too. The
// menu bar keeps `target`. The target has to outlive the menu bar. Returns false on a system without a menu bar at the
// top of the screen. The program draws the menus there.
bool InstallMenuBar(std::span<const Menu> menus, MenuTarget& target);

// Gives the title bar of `window` the colour `colour`. The system draws the bar's controls and title for a dark
// background. Does nothing on a system that doesn't allow it.
void SetTitleBarColour(SDL_Window* window, ImU32 colour);

// Sends SDL3 an event of type `event_type` when the program gets SIGINT or SIGTERM. SDL3 turns those signals into a
// quit event the next time that it polls its events. On the Mac, a signal doesn't end a wait for events. The event
// does. A Windows program with a window gets no such signals. There, the function does nothing.
void WakeOnQuitSignals(uint32_t event_type);

// `time` as a time of day in the user's format, such as 5:44:45 pm. The Undo History shows when each edit happened.
std::string TimeOfDay(std::chrono::system_clock::time_point time);

// `path` from a dialog of the system, in the one form that the program shows and keeps. On Windows, the shell's dialogs
// give a path past 260 characters with Win32's prefix of a long path, or with the short 8.3 names of its folders and
// file. The program then gets the long names, with the prefix only where the path requires it. Other systems give one
// form, and the path stays as it is.
std::string SystemPath(const std::string& path);

// Shows a message box of the system and waits for its answer, as SDL_ShowMessageBox does. `chosen` receives the number
// of the chosen button, and keeps its value when the box closes without one. It can be null. Returns false when no box
// can be shown. On macOS and Windows, SDL3 shows the box. On other systems, SDL_ShowMessageBox runs zenity and pumps no
// events of the window until zenity ends. The window then misses the desktop's pings, and GNOME offers to force the
// program to quit. There, this function runs zenity and pumps the window's events during the box. No frame is drawn
// and no other code of the program runs before the answer, as on the other systems.
bool ShowSystemBox(const SDL_MessageBoxData& box, int* chosen);

} // namespace hokuto::app
