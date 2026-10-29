// SPDX-License-Identifier: MIT

// The zones of the main window: the inspector, the arrangement, the mixer, the browser, the variables and the undo
// history. Each one is a Dear ImGui window, docked without a tab. The control bar's buttons show and hide them. The
// browser shows the file as a tree beside the other zones.

#pragma once

#include <imgui.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "app/ruler.h"
#include "app/song_view.h"
#include "app/sound_section.h"
#include "app/transport.h"
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
// children, and `missing` says why in their tooltips (song::MissingGroupNote).
struct BrowserItem
{
    std::string name;
    std::vector<std::string> children;
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

// The arrangement's zoom on a first run and after Reset Layout, in points. A track of the default height shows its
// name, its bank and its program on three lines.
inline constexpr float kDefaultBeatWidth = 18.0f;
inline constexpr float kDefaultTrackHeight = 66.0f;

// The width of the track list on a first run and after Reset Layout, and the narrowest and widest that a drag of its
// edge makes it, in points.
inline constexpr float kDefaultTrackListWidth = 240.0f;
inline constexpr float kMinTrackListWidth = 160.0f;
inline constexpr float kMaxTrackListWidth = 600.0f;

// A colour that the user picked for a track, from the colour strip of the track's header or the inspector: a colour of
// theme::kTrackPalette, or nothing for the track's automatic colour.
struct TrackColourPick
{
    int track = 0; // the track's index in the song
    std::optional<ImU32> colour;
};

// A value of the selected part that the arrangement's info line takes: a new start, a transposition, or a change of
// velocity.
enum class PartValue
{
    kStart,
    kTranspose,
    kVelocity,
};

// An edit of a part's notes that the arrangement's info line asks for: the ticks that the notes move by, the semitones
// that they move by, or the change of their velocities. `refused` says why the line can't take the typed text, and the
// edit then changes nothing.
struct PartEdit
{
    int track = 0; // the track's index in the song
    int part = 0;
    PartValue value = PartValue::kStart;
    int32_t amount = 0;
    std::string refused;
};

// What the track list's context menu offers, in its order: Add Track, Duplicate Tracks and Remove Selected Tracks, then
// Copy Track and Paste Track. Copy Track keeps the selected track for a paste into another sound.
enum class TrackAction
{
    kAdd,
    kDuplicate,
    kRemove,
    kCopy,
    kPaste,
};

inline constexpr std::size_t kTrackActions = 5;

// A place where a song's tracks read variables or branch: a set of commands, by its index in SongView::logic, and one
// of the set's ticks.
struct MarkerPlace
{
    std::size_t marker = 0;
    int32_t tick = 0;
};

struct ArrangementState
{
    float beat_width = kDefaultBeatWidth;
    float track_height = kDefaultTrackHeight;
    float track_list_width = kDefaultTrackListWidth;
    bool resizing_track_list = false;
    float track_list_grab = 0.0f;
    int selected_track = 0;
    int selected_part_track = -1;
    int selected_part = -1;
    Ruler ruler;

    // The zooms asked for since the last frame: in for each step above 0, and out for each step below. Zoom Full fits
    // the whole song into the lanes.
    int zoom_steps = 0;
    int vertical_zoom_steps = 0;
    bool zoom_full = false;

    // A range to fit into the lanes in the next frame, in quarter notes. Alt with a double-click on a region of the
    // marker row sets it, and so does the row's Zoom menu.
    std::optional<std::pair<double, double>> zoom_range;

    // The scroll in the last frame: the quarter note at the left edge of the lanes, and the track at their top, in
    // tracks from the first. A file keeps them (format::EditorView). `scroll_to` asks for a scroll to such a place in
    // the next frame.
    double left_quarter = 0.0;
    float top_track = 0.0f;
    std::optional<std::pair<double, float>> scroll_to;

    // A double-click on a place where the tracks read variables or branch sets `open_marker`. The program then opens
    // the place's commands in the List Editor, and clears it.
    std::optional<MarkerPlace> open_marker;

    // Whether the tempo row shows under the marker row. Project > Tempo Track shows it and hides it.
    bool tempo_row = false;

    // The drag of a point of the tempo row, while the mouse holds the point. Its tempo `to` follows the mouse,
    // unrounded, and so does its shift. A drag that ends at another whole tempo or another tick sets `tempo_move`. The
    // program then gives the point's command the tempo and the tick, and clears it. A double-click on a point sets
    // `open_tempo`. The program then opens the point's command in the List Editor, and clears it.
    std::optional<TempoMove> tempo_drag;
    std::optional<TempoMove> tempo_move;
    std::optional<TempoPointView> open_tempo;

    // The drag of an edge of a timeline's loop in the marker row, while the mouse holds the edge. The edge follows the
    // mouse on the beats, or on the ticks with Shift. A drag that ends at another tick sets `loop_move`. The program
    // then moves the loop, and clears it.
    std::optional<LoopMove> loop_drag;
    std::optional<LoopMove> loop_move;

    // A double-click on a part sets `open_editor`. The program then opens the part in the Key Editor, and clears it. A
    // double-click on a track's lane away from its parts sets it too, with `open_beat` at the beat under the mouse. The
    // program then opens the track there. A track without notes has no parts.
    bool open_editor = false;
    std::optional<double> open_beat;

    // While the tracks' colours are Custom, a click on a track's colour strip, or on its colour in the inspector,
    // offers the palette. A pick sets `colour_pick`. The program then gives the track the colour, and clears it.
    bool custom_colours = false;
    int colour_popup_track = -1;
    std::optional<TrackColourPick> colour_pick;

    // A right-click on the track list selects the track under the mouse and opens the list's context menu. The program
    // sets which of the menu's actions can run, in the order of TrackAction. A choice sets `track_action`. The program
    // then runs it, and clears it.
    std::array<bool, kTrackActions> track_actions = {};
    std::optional<TrackAction> track_action;

    // The text of the search field of the inspector's menus of a track's bank and program.
    std::array<char, 128> instrument_filter{};

    // A value typed in the arrangement's info line sets `part_edit`. The program then edits the selected part's notes,
    // and clears it.
    InfoTyping part_typing;
    std::optional<PartEdit> part_edit;

    // The lanes' hint while no sequence shows: the way to one that the file offers. The program sets it.
    std::string empty_hint;
};

// The output's channel in the mixer. The fader sets the output's level.
struct MasterBus
{
    float volume = 0.0f;                               // dB
    std::array<float, 2> meter = {kSilence, kSilence}; // dB, left and right
    float peak = kSilence;                             // dB, the loudest level since the user cleared it
};

// The mixer's FX channels, one for each of the aux buses A to C.
inline constexpr std::size_t kFxChannels = 3;

// An insert of an FX channel: the channel, from 0 for A, and the insert's place among the channel's effects.
struct EffectPlace
{
    friend bool operator==(const EffectPlace&, const EffectPlace&) = default;

    std::size_t channel = 0;
    std::size_t index = 0;
};

// A move of an FX channel's fader: the channel, and its level before and after the move, in dB.
struct FxMove
{
    std::size_t channel = 0;
    float from = 0.0f;
    float to = 0.0f;
};

// The FX channels' inserts, by the names of their effects. An insert's tooltip gives its effect's whole name: the
// insert cuts a long name short. An effect that `idle` marks shows grey, and its tooltip adds `idle_note`: the engine
// that plays the shown sound doesn't run it. A click on an effect sets `clicked` to its place. A click on a channel's
// empty insert opens a menu of `kinds`. A choice there sets `clicked` to the empty insert's place, and `kind` to the
// kind's index. The program then adds or opens the effect, and clears `clicked`. The insert at `open` shows that its
// window is open.
//
// Like a track's fader, each channel's fader sets the volume of its bus: `volumes`, in dB. The meters and the peaks
// show what each bus adds to the output. `held` names the fader that the user holds, with its level when the hold
// began. A hold that ends with a new level becomes `moved`. The program then makes the edit, and clears `moved`. The
// mixer shows the first `count` channels: the buses that both the file's console and the engine have.
struct FxChannels
{
    std::size_t count = kFxChannels;
    std::array<std::vector<std::string>, kFxChannels> effects;
    std::array<std::vector<bool>, kFxChannels> idle;
    std::string idle_note;
    std::vector<std::string> kinds;
    std::optional<EffectPlace> clicked;
    std::size_t kind = 0;
    std::optional<EffectPlace> open;
    std::array<float, kFxChannels> volumes{};
    std::array<std::array<float, 2>, kFxChannels> meters = {
        {{kSilence, kSilence}, {kSilence, kSilence}, {kSilence, kSilence}}}; // dB, left and right
    std::array<float, kFxChannels> peaks = {kSilence, kSilence, kSilence}; // dB, the loudest since the user cleared it
    std::optional<FxMove> held;
    std::optional<FxMove> moved;
};

// An action in the Undo History: its name, what it acted on, and its time of day as the system writes times.
struct HistoryRow
{
    std::string name;
    std::string details;
    std::string time;
};

// The number of beats from one label on a ruler to the next while Hokuto counts beats, for beats `beat_width` pixels
// apart: 1, 2 or 5 times a power of 10. The labels then stay apart.
int BeatStep(float beat_width);

// Whether a ruler labels the beat at `quarter`: the first beat, and every `step` beats, counting from 1.
bool LabelledBeat(int quarter, int step);

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
void DrawArrangement(SongView* song, Transport& transport, ArrangementState& state, bool busy);
void DrawMixer(SongView* song, MasterBus& master, ArrangementState& state, FxChannels* fx, bool busy, bool* open);

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

// Draws the Undo History while `*open` is set: `rows`, the oldest first, with a line under the first `done` of them,
// the actions that are done. Returns the number of actions that the user chose to have done. A click on a row moves the
// line under it, and a drag moves the line too.
std::optional<std::size_t> DrawUndoHistory(std::span<const HistoryRow> rows, std::size_t done, bool* open);

} // namespace hokuto::app
