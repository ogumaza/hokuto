// SPDX-License-Identifier: MIT

// The parts of the browser that its files share (app_browser.cpp, app_browser_gba.cpp and app_browser_ds.cpp). The
// names are internal to those files.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace hokuto::app::browser_parts
{

// The browser's menu items that open a bank and its instruments in the Bank Editor, a wave in its properties and a
// stream sound's stream in its properties. A double-click does the same. Like Open Bank Editor, each opens a window at
// once. None takes an ellipsis.
inline constexpr char kOpenBankEditor[] = "Open Bank Editor";
inline constexpr char kWaveProperties[] = "Wave Properties";
inline constexpr char kStreamProperties[] = "Stream Properties";

// The browser's name for a child of an item of `group`: a bank's program or a wave archive's wave.
std::string ChildName(std::size_t group, uint32_t child);

} // namespace hokuto::app::browser_parts
