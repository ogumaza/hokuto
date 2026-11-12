// SPDX-License-Identifier: MIT

// The Variables zone: its state between frames, and the function that draws it.

#pragma once

#include <array>
#include <cstddef>
#include <optional>

#include "app/song_view.h"

namespace hokuto::app
{

// The Variables zone's state between frames: the place of the scene whose name the user edits, the name so far, and
// whether the name's field takes the keyboard in the next frame. Then the place of the change whose time the user
// types, and the time so far.
struct VariablesState
{
    std::optional<std::size_t> renaming;
    std::array<char, 256> name{};
    bool focus = false;
    std::optional<std::size_t> timing;
    std::array<char, 32> time{};
};

// Draws the Variables zone while `*open` is set: the song's scenes, its changes of variables while it plays, then the
// song's variables that a game can set, each with switches for the value that the song starts with. A variable without
// choices takes any value from a slider. While the song plays, a variable that changes shows its value at the position.
// A choice waits in the variable's view (VariableView::chosen), an action on a scene in the song's view
// (SongView::scene_request), and an action on a change there too (SongView::change_request).
void DrawVariables(SongView* song, VariablesState& state, bool* open);

} // namespace hokuto::app
