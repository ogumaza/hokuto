// SPDX-License-Identifier: MIT

// The Bank Editor. It shows a bank's instruments by program, the map of the selected instrument's regions, keys across
// and velocities up, and every value of the selected region. A keyboard under the map plays the instrument on the
// console's model. Each edit makes a new model of the bank (edit/banks.h). The program then makes it an action in the
// history.
//
// A wave dropped from the browser, or a WAV file or a wave file dropped from the system, makes a region. On the map,
// one wave dropped on a region gives the region the wave. Other drops make a region of one key at each wave, from the
// key under the drop up. A drum kit is such a map. Waves dropped on the list of programs make an instrument each, at
// the free programs, with one region over every key and velocity.

#pragma once

#include <imgui.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/sar/bank_file.h"

#include "app/item_views.h"
#include "app/views.h"
#include "edit/banks.h"

namespace hokuto::app
{

// What the user did in the Bank Editor. `model` is the bank's new model, with the action's name and details for the
// Undo History. `refused` holds a message for an edit that the bank can't take. `wave` names a wave that the user
// dropped from the browser, and `drop` where it landed.
struct BankEdit
{
    std::optional<ntdlib::sar::BankFile> model;
    std::string name;
    std::string details;
    std::string refused;
    std::optional<BrowserPlace> wave;
    BankDrop drop;
};

// The part of a region that a drag moves: the whole region, or one of its edges.
enum class RegionPart
{
    kBody,
    kLeft,
    kRight,
    kTop,
    kBottom,
};

// The Bank Editor's state between frames: the selected instrument, and the selected region by its first key and first
// velocity. No two regions of an instrument share both. Then a drag of a region: the part that it moves, the region's
// place among the instrument's regions, where the drag started, and the regions before and during the drag. Then the
// key that the keyboard sounds and its velocity, the selected region as a field that the user holds sets it, with the
// field's name, the program that the user types, and the text of the menus' search fields. The corners of the window,
// of the map and of the list of programs in the last frame place the files that the system drops (DropAt). While
// `reveal` is set, the list of programs scrolls the selected program into view, or else to its top, and the facts
// scroll to their top. The window then clears it. A new state sets it: the panes keep their names from one bank to the
// next, and Dear ImGui keeps their scroll with the names.
struct BankEditorState
{
    bool reveal = true;
    std::optional<uint32_t> program;
    std::optional<std::pair<int, int>> region;
    std::optional<RegionPart> dragging;
    std::size_t dragged = 0;
    ImVec2 drag_from;
    std::vector<edit::Region> before;
    std::vector<edit::Region> moved;
    int sounding = -1;
    int velocity = 100;
    std::optional<edit::Region> held;
    std::string held_name;
    std::array<char, 8> typed_program{};
    bool typing_program = false;
    std::array<char, 128> filter{};
    ImVec2 window_min;
    ImVec2 window_max;
    ImVec2 map_min;
    ImVec2 map_max;
    ImVec2 list_min;
    ImVec2 list_max;
};

// Whether `point` lies on the Bank Editor's window.
bool OverBankEditor(const BankEditorState& state, ImVec2 point);

// Where a file that the system drops at `point` lands on the Bank Editor, or nothing outside its map and its list.
std::optional<BankDrop> DropAt(const BankEditorState& state, ImVec2 point);

// Draws the Bank Editor's window for `view`. The window's close button clears `*open`. While `busy`, the window takes
// no edits. Returns what the user did.
BankEdit DrawBankEditor(const BankView& view, BankEditorState& state, bool busy, bool* open);

} // namespace hokuto::app
