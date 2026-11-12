// SPDX-License-Identifier: MIT

// The browser's items, groups and places, and its state between frames. DrawBrowser in views.h draws the zone.

#pragma once

#include <imgui.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "app/widgets.h"

namespace hokuto::app
{

// An item of the browser, with the names of its children. A bank's children are its instruments, and a wave archive's
// are its waves. The user opens an item to show its children. Export… in an item's context menu exports the item while
// `exports` is set, and shows disabled otherwise, with `export_note` in its tooltip. Replace… puts a file in the item's
// place while `replaces` is set, and shows disabled otherwise, with `replace_note` in its tooltip if it has one. The
// children have a context menu with Export… while `children_export` is set: a bank's instruments, a wave archive's
// waves and a wave sound file's wave sounds export. The menu holds Replace… too, enabled while `children_replace` is
// set. A click plays a child. A click plays an item without children too while `plays` is set, such as a wave sound.
// `edit` names the menu item that opens the item in its editor, such as a bank in the Bank Editor, and a double-click
// does the same. Without `edits`, the menu item shows disabled, with `edit_note` in its tooltip, and a double-click
// opens nothing. `children_edit` does this for the children. The children drag while `children_drag` is set: a wave
// archive's waves drag to the Bank Editor. The search finds an item by its children's names while `children_found` is
// set: a wave sound file's children name its wave sounds. An item whose file the archive lacks shows grey with its
// children, and `missing` says why in their tooltips (song::MissingGroupNote). `number` is the item's index among the
// archive's sounds, banks or wave archives, and `child_numbers` contains the numbers of the children: a bank's
// programs, or a wave archive's waves.
struct BrowserItem
{
    std::string name;
    std::vector<std::string> children;
    uint32_t number = 0;
    std::vector<uint32_t> child_numbers;
    bool exports = false;
    std::string export_note;
    bool replaces = false;
    std::string replace_note;
    bool children_export = false;
    bool children_replace = false;
    bool plays = false;
    std::string edit;
    bool edits = true;
    std::string edit_note;
    std::string children_edit;
    bool children_drag = false;
    bool children_found = false;
    std::string missing;
};

// The type of the payload of a wave that the user drags from the browser. The payload holds the wave's BrowserPlace.
inline constexpr char kWavePayload[] = "HOKUTO_WAVE";

// A group of the file's items in the browser, such as its sequences.
struct BrowserGroup
{
    std::string name;
    Icon icon = Icon::kSequence;
    ImU32 colour = 0;
    std::vector<BrowserItem> items;
    bool hidden = false; // the file's console has no such items, and the tree leaves the group out
    // The tooltip of a child's greyed Replace…, such as a bank's instrument, where the item itself has no reason.
    std::string children_replace_note = {};
};

// A place in the browser's tree: a group, an item of the group, and a child of the item, or -1 for the item itself.
struct BrowserPlace
{
    friend bool operator==(const BrowserPlace&, const BrowserPlace&) = default;

    int group = -1;
    int item = -1;
    int child = -1;
};

// The browser's search text, the open items, and the selection. A click on a row that plays sets `clicked`. The program
// then plays the row's child or item, or stops it if it's playing, and clears `clicked`. The browser marks the row
// that's playing.
// Export… in a row's context menu sets `export_place`. The program then opens the Export window for the row's item, and
// clears it. Replace… sets `replace_place` the same way. The program then asks for the file to put in the item's place.
// A double-click on a row with an editor, or the menu item that opens it, sets `edit_place` the same way. The program
// then opens the row's editor. The program sets `reveal` to scroll the selected row into view in the next frame, such
// as after it selects the sound of a file's view. The browser then clears it. The program sets `top` when a new file
// shows. The tree then scrolls to its top in the next frame, and the browser clears it.
struct BrowserState
{
    std::array<char, 128> filter{};
    std::set<std::pair<int, int>> open; // each a group and an item
    BrowserPlace selected;
    bool reveal = false;
    bool top = false;
    bool clicked = false;
    std::optional<BrowserPlace> playing;
    std::optional<BrowserPlace> export_place;
    std::optional<BrowserPlace> replace_place;
    std::optional<BrowserPlace> edit_place;
};

} // namespace hokuto::app
