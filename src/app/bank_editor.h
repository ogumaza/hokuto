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
//
// A GBA voice group shows its programs the same way. A key split shows its runs of keys, and a drum kit its keys, each
// with the voice that it plays. The fields edit a voice's values in place: a sample's sample and pitch, a square wave's
// duty, sweep and length, a wave's 32 steps, the noise's mode, a drum kit's key and pan, and the envelope.
//
// A DS bank shows its programs that way too. A drum set shows its keys, and a key split its runs of keys, each with the
// region that it plays. The fields edit a region's values in place: its kind among a wave, a square wave and noise, its
// wave or duty, its root key, its envelope and its pan.

#pragma once

#include <imgui.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/mp2k/song.h"
#include "ntdlib/sar/bank_file.h"
#include "ntdlib/sdat/formats.h"

#include "app/item_views.h"
#include "app/views.h"
#include "app/widgets.h"
#include "edit/banks.h"
#include "edit/ds_banks.h"
#include "edit/dse_banks.h"
#include "edit/gba_voices.h"
#include "edit/konami_samples.h"

namespace hokuto::app
{

// What the user did in the Bank Editor. `model` is the bank's new model, with the action's name and details for the
// Undo History. `refused` holds a message for an edit that the bank can't take. `wave` names a wave that the user
// dropped from the browser, and `drop` where it landed. On the GBA, `gba_voice` contains a voice's new values at its
// address, and gets a new pattern when `gba_new_pattern` is set. `gba_pattern` contains a wave's new pattern at the
// pattern's address. `gba_new` contains new voices at their programs, and `gba_cleared` the address of a voice to
// clear. `gba_rows` contains a key split or a drum kit with the rows that its new tables contain, and
// `gba_program_voice` a voice for a program of any kind. On the DS, `ds_region` contains a region's new kind and values
// at its place. With Konami's driver, `konami_sample` contains a sample's new rate and loop, by the sample's number.
struct BankEdit
{
    std::optional<ntdlib::sar::BankFile> model;
    std::optional<ntdlib::mp2k::Voice> gba_voice;
    bool gba_new_pattern = false;
    std::optional<std::pair<uint32_t, edit::GbaPattern>> gba_pattern;
    std::vector<std::pair<uint32_t, ntdlib::mp2k::Voice>> gba_new;
    std::optional<uint32_t> gba_cleared;
    std::optional<std::pair<ntdlib::mp2k::Voice, std::vector<edit::GbaKeyRow>>> gba_rows;
    std::optional<ntdlib::mp2k::Voice> gba_program_voice;
    std::optional<std::pair<edit::DsRegionPlace, ntdlib::sdat::Region>> ds_region;
    std::optional<ntdlib::sdat::Bank> ds_model;
    std::optional<std::array<uint16_t, 4>> ds_slots;
    std::optional<std::pair<edit::DseSplitPlace, edit::DseSplit>> dse_split;
    std::optional<std::pair<int, edit::KonamiSampleValues>> konami_sample;
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
// next, and Dear ImGui keeps their scroll with the names. On the GBA, `gba_row` is the first key of the selected row of
// a key split or a drum kit, `gba_held` the voice as a field that the user holds sets it, `gba_key_held` a row's key as
// the user holds its slider, and `gba_steps` a wave's
// pattern as the user draws it. On the DS, `ds_row` is the first key of the selected row of a drum set or a key split,
// `ds_held` the region as a field that the user holds sets it, and `typed_key` and `typed_last` the keys that the user
// types for a split or for a drum set's keys. On DSE, `dse_row` is the selected split, `dse_held` the split as a field
// that the user holds sets it, and `typed_key`, `typed_last`, `typed_velocity` and `typed_top_velocity` the split's
// keys and velocities that the user types. A sample of another of the GBA's drivers uses `typed_rate` and
// `typed_loop`, the rate and the loop's start that the user types.
// `typed_waiting` keeps the text of a typed field that the user ended while the program was busy (WaitingValues).
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
    std::optional<int> gba_row;
    std::optional<ntdlib::mp2k::Voice> gba_held;
    std::optional<edit::GbaPattern> gba_steps;
    std::optional<int> gba_key_held;
    std::optional<int> ds_row;
    std::optional<ntdlib::sdat::Region> ds_held;
    std::array<char, 8> typed_key{};
    bool typing_key = false;
    std::array<char, 8> typed_last{};
    bool typing_last = false;
    std::optional<std::size_t> dse_row;
    std::optional<edit::DseSplit> dse_held;
    std::array<char, 8> typed_velocity{};
    bool typing_velocity = false;
    std::array<char, 8> typed_top_velocity{};
    bool typing_top_velocity = false;
    std::array<char, 16> typed_rate{};
    bool typing_rate = false;
    std::array<char, 16> typed_loop{};
    bool typing_loop = false;
    WaitingValues<std::string> typed_waiting;
};

// Whether `point` lies on the Bank Editor's window.
bool OverBankEditor(const BankEditorState& state, ImVec2 point);

// Where a file that the system drops at `point` lands on the Bank Editor, or nothing outside its map and its list.
std::optional<BankDrop> DropAt(const BankEditorState& state, ImVec2 point);

// The voice of a GBA voice group that `drop` lands on: the program's voice, or the selected row of a key split or a
// drum kit. A drop on the list of programs lands on none.
std::optional<ntdlib::mp2k::Voice> DroppedGbaVoice(const GbaBankView& view, const BankEditorState& state,
                                                   const BankDrop& drop);

// The region of a DS bank that `drop` lands on: the selected row of the drop's program, by the program and the region's
// index. A drop on the list of programs, or on a program without rows, lands on none.
std::optional<std::pair<uint32_t, std::size_t>> DroppedDsRegion(const DsBankView& view, const BankEditorState& state,
                                                                const BankDrop& drop);

// Draws the Bank Editor's window for `view`. The window's close button clears `*open`. While `busy`, the window takes
// no edits. `bank` is the version and the bank that the view comes from. A value typed while busy waits for its program
// and region there. Returns what the user did.
BankEdit DrawBankEditor(const BankView& view, BankEditorState& state, const ShownItem& bank, bool busy, bool* open);

// Draws the Bank Editor window for `view`, the view of a GBA voice group. DrawBankEditor does the same for a bank.
BankEdit DrawGbaBankEditor(const GbaBankView& view, BankEditorState& state, const ShownItem& bank, bool busy,
                           bool* open);

// Draws the Bank Editor window for `view`, the view of a DS bank. DrawBankEditor does the same for a bank.
BankEdit DrawDsBankEditor(const DsBankView& view, BankEditorState& state, const ShownItem& bank, bool busy, bool* open);

// Draws the Bank Editor's window for a DSE bank's `view`. DrawBankEditor draws the window for a bank.
BankEdit DrawDseBankEditor(const DseBankView& view, BankEditorState& state, const ShownItem& bank, bool busy,
                           bool* open);

// Draws the Bank Editor window for the samples of a ROM with another of the GBA's drivers, such as Konami's driver. The
// window shows them, and its keyboard plays the selected one. A sample whose view is editable accepts a new rate and
// loop. While `busy`, the window takes no edits. The window's close button clears `*open`. Returns what the user did.
BankEdit DrawDriverBankEditor(const DriverBankView& view, BankEditorState& state, const ShownItem& bank, bool busy,
                              bool* open);

} // namespace hokuto::app
