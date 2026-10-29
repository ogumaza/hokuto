// SPDX-License-Identifier: MIT

// The List Editor: a track's commands in a list, the logic included. A row shows a command as the track ran it, in the
// order that it ran, with its type, start, length, two data values and a comment. A toolbar sets the track and the kind
// of command that Insert adds. A click on a value types a new one. A click on the target of a jump, a call or a track
// start opens a menu of the sequence's labels. A row's context menu inserts a command before it, switches its condition
// and deletes it. The editor docks in the lower zone, in the mixer's place. The Key Editor docks there too. The program
// turns each edit into an edit of the commands (src/edit/commands.h).
//
// In a program, each label has a row before its command. A double-click on its name types a new one, and a drag onto
// another row moves the label to that row's command. The context menus insert and delete labels, make a subroutine of
// the selected commands, and make a call give way to its subroutine's commands or take a copy of the subroutine
// (src/edit/subroutines.h). Open Subroutine shows a subroutine by itself: the rows of one call's run, with a menu of
// the other calls in the toolbar.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <utility>

#include "ntdlib/commands/sequence.h"

#include "app/list_edits.h"
#include "app/transport.h"

namespace hokuto::app
{

// The List Editor's state between frames.
struct ListEditorState
{
    // A row to scroll to in the next frame. The window takes the focus with it.
    std::optional<std::size_t> scroll_row;

    // The selected commands, by their offsets, and the row that a click last selected, for a Shift-click's range.
    std::set<uint32_t> selected;
    std::optional<std::size_t> anchor;

    // The cell that the user types in: its row and column, the text so far, and whether the field takes the keyboard's
    // focus in the next frame.
    std::optional<std::pair<std::size_t, ListColumn>> typing;
    std::array<char, 96> text{};
    bool focus = false;

    // The row whose context menu is open, the row whose menu of targets is open, and that menu's search text. The menu
    // of targets opens again in the next frame after the second click of a double-click closed it.
    std::optional<std::size_t> menu_row;
    std::optional<std::size_t> target_row;
    std::array<char, 128> filter{};
    bool reopen_targets = false;

    // The kind of command that Insert adds, and a track chosen in the toolbar's menu. The program then shows that
    // track, and clears it.
    ntdlib::commands::Kind insert_kind = ntdlib::commands::Kind::kVolume;
    std::optional<int> chosen_track;

    // The selected label, by its name. A selected label leaves no command selected.
    std::optional<std::string> selected_label;

    // A request of the Edit menu's Select All. The editor carries it out in its next frame, and clears it.
    bool select_all = false;

    // A subroutine to show by itself, from a menu: the offset of its first command and the run to show, or the first
    // run without one. A request to show the track again. The program carries both out, and clears them.
    struct SubroutineChoice
    {
        uint32_t first = 0;
        std::optional<ListCall> run;
    };
    std::optional<SubroutineChoice> open_subroutine;
    bool close_subroutine = false;

    // Whether the editor's window had the keyboard focus in the last frame.
    bool focused = false;
};

// Draws the List Editor while `*open` is set. A line between the rows marks the transport's position. `busy` holds the
// edits off while the program reads the result of the last edit. Returns the edit that the user made.
std::optional<ListEdit> DrawListEditor(const ListTrack& track, ListEditorState& state, const Transport& transport,
                                       bool busy, bool* open);

} // namespace hokuto::app
