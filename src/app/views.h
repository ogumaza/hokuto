// SPDX-License-Identifier: MIT

// The zones of the main window: the inspector, the arrangement, the mixer, the browser, the variables and the undo
// history. Each one is a Dear ImGui window, docked without a tab. The control bar's buttons show and hide them. The
// browser shows the file as a tree beside the other zones.

#pragma once

#include <imgui.h>

#include <string_view>
#include <vector>

#include "app/arrangement.h"
#include "app/browser.h"
#include "app/mixer.h"
#include "app/ruler.h"
#include "app/song_view.h"
#include "app/sound_section.h"
#include "app/transport.h"
#include "app/undo_history.h"
#include "app/variables_zone.h"
#include "app/widgets.h"

namespace hokuto::app
{

// The zones' names. Dear ImGui identifies each window by its name, and saves the window's place in the layout under it.
inline constexpr char kInspector[] = "Inspector";
inline constexpr char kBrowser[] = "Browser";
inline constexpr char kArrangement[] = "Arrangement";
inline constexpr char kMixer[] = "Mixer";
inline constexpr char kVariables[] = "Variables";
inline constexpr char kUndoHistory[] = "Undo History";

// The Key Editor and the List Editor dock with the mixer in the lower zone, and one of the three shows at a time. The
// IDs stay when the titles change.
inline constexpr char kKeyEditor[] = "Key Editor###KeyEditor";
inline constexpr char kListEditor[] = "List Editor###ListEditor";

// The keyboard shortcuts that show and hide the zones. Dear ImGui's Ctrl is the Command key on the Mac. The Undo
// History has none.
inline constexpr ImGuiKeyChord kInspectorKeys = ImGuiMod_Alt | ImGuiKey_I;
inline constexpr ImGuiKeyChord kBrowserKeys = ImGuiMod_Ctrl | ImGuiKey_P;
inline constexpr ImGuiKeyChord kMixerKeys = ImGuiKey_F3;

// The zones that are open. The program's settings keep them. The arrangement is always open. It lies in the dock
// space's central node, and Dear ImGui keeps that node's room while the node is empty.
struct OpenViews
{
    friend bool operator==(const OpenViews&, const OpenViews&) = default;

    bool inspector = true;
    bool browser = true;
    bool mixer = true;
    bool variables = true;
    bool undo_history = true;
};

// Docks the zones in `dockspace`: the inspector on the left, the browser, the variables and the undo history on the
// right, the arrangement in the middle, and the mixer below it.
void BuildLayout(ImGuiID dockspace);

// Docks the zone `window` with the mixer in the lower zone where the settings hold no place for it, before the window's
// Begin. A settings file from before the zone joined the layout holds none, and its layout stays: the List Editor
// joined on 2026-10-18 without a new layout version. A zone that the settings place stays where they place it.
void DockWithMixer(const char* window);

// Each of these draws a zone while its flag is set. The inspector shows the selected track, and the values of the shown
// sound in `sound`. The browser's header shows the file's name, and its Sound section the values of the selected sound
// in `sound`. The mixer shows the FX channels after the tracks' channels, unless `fx` is nullptr. Its header holds the
// switch of the sound's mixing mode. A move of a track's control waits in the track's view (TrackView::moved). While
// `busy`, the controls take no moves, and the arrangement's info line takes no typing.
void DrawInspector(SongView* song, MasterBus& master, ArrangementState& state, SoundSection& sound, bool busy,
                   bool* open);
void DrawBrowser(const std::vector<BrowserGroup>& groups, std::string_view file, BrowserState& state,
                 SoundSection& sound, bool* open);
// `shown` is the version and the sound that the arrangement's `song` comes from. Its info line types values for them.
void DrawArrangement(SongView* song, Transport& transport, ArrangementState& state, const ShownItem& shown, bool busy);
void DrawMixer(SongView* song, MasterBus& master, ArrangementState& state, FxChannels* fx, bool busy, bool* open);

} // namespace hokuto::app
