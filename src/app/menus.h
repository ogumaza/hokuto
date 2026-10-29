// SPDX-License-Identifier: MIT

// The program's menus as a table. The Mac's menu bar, the menus that the program draws on other systems and the
// keyboard shortcuts all come from it.

#pragma once

#include <imgui.h>

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace hokuto::app
{

// The commands in the menus.
enum class Command
{
    kNewProject,
    kOpen,
    kSave,
    kSaveAs,
    kImportSoundArchive,
    kImportSequence,
    kImportBank,
    kImportWaveArchive,
    kImportWave,
    kImportWaveSoundFile,
    kImportAudioFile,
    kImportStream,
    kExportSoundArchive,
    kExportMidiSoundFont,
    kExportMidiSfz,
    kExportOtherConsole,
    kRecentProjects,
    kQuit,
    kUndo,
    kRedo,
    kUndoHistory,
    kCut,
    kCopy,
    kPaste,
    kSelectAll,
    kOpenCloseEditor,
    kOpenListEditor,
    kQuantize,
    kZoomIn,
    kZoomOut,
    kZoomFull,
    kZoomInVertically,
    kZoomOutVertically,
    kAddSequenceSound,
    kAddBank,
    kAddTrack,
    kDuplicateTracks,
    kRemoveSelectedTracks,
    kCopyTrack,
    kPasteTrack,
    kTempoTrack,
    kProjectSetup,
    kStartStop,
    kStart,
    kStop,
    kCycle,
    kLocatorsToSelection,
    kGoToStart,
    kGoToLeftLocator,
    kGoToRightLocator,
    kLocateNextEvent,
    kLocatePreviousEvent,
    kRewind,
    kForward,
    kMixer,
    kStudioSetup,
    kPreferences,
    kInspector,
    kBrowser,
    kVariables,
    kResetLayout,
};

// An item in a menu, or a separator when `name` is null. `keys` are the item's shortcut, in Dear ImGui's terms. On the
// Mac, ImGuiMod_Ctrl stands for the Command key. Holding the keys of an item with `repeat` repeats the command. An item
// with `items` opens a submenu of them. Such an item has no command. An item with `none` opens a submenu of the titles
// that the target lists for its command, each time that the submenu opens. Without titles, the submenu shows `none`,
// greyed out.
struct MenuItem
{
    const char* name = nullptr;
    Command command = Command::kQuit;
    ImGuiKeyChord keys = ImGuiKey_None;
    bool repeat = false;
    std::vector<MenuItem> items = {};
    const char* none = nullptr;
};

struct Menu
{
    const char* name;
    std::vector<MenuItem> items;
};

// The menus, from left to right. The Mac's application menu isn't among them. The system's menu bar adds it, and the
// system's items in the Window menu.
std::span<const Menu> Menus();

// The program's side of the menus.
class MenuTarget
{
public:
    virtual ~MenuTarget() = default;

    virtual void Execute(Command command) = 0;

    // Whether the command's item has a check mark: a view that's open, or a mode that's on.
    virtual bool IsChecked(Command command) const = 0;

    // Whether the command can run. A disabled item is grey, and its shortcut does nothing.
    virtual bool IsEnabled(Command command) const = 0;

    // The title of the item of the command named `name` in the menus. Undo and Redo add the name of the action that
    // they'd undo or redo, such as Undo Change Parameter. Other items keep their names.
    virtual std::string Title(Command command, std::string_view name) const = 0;

    // The titles of the submenu of the command's item, for an item with `none`: File > Recent Projects lists the files.
    virtual std::vector<std::string> Listed(Command command) const = 0;

    // Runs the command on the submenu's item at `index` of Listed's titles.
    virtual void ExecuteListed(Command command, std::size_t index) = 0;
};

} // namespace hokuto::app
