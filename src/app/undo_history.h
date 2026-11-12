// SPDX-License-Identifier: MIT

// The Undo History: its rows, and the function that draws it.

#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string>

namespace hokuto::app
{

// An action in the Undo History: its name, what it acted on, and its time of day as the system writes times.
struct HistoryRow
{
    std::string name;
    std::string details;
    std::string time;
};

// Draws the Undo History while `*open` is set: `rows`, the oldest first, with a line under the first `done` of them,
// the actions that are done. Returns the number of actions that the user chose to have done. A click on a row moves the
// line under it, and a drag moves the line too.
std::optional<std::size_t> DrawUndoHistory(std::span<const HistoryRow> rows, std::size_t done, bool* open);

} // namespace hokuto::app
