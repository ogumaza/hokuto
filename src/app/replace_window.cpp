// SPDX-License-Identifier: MIT

// The drawing of the Replace window: the rows of choices, the groups, the waves, the warnings and the buttons.

#include "app/replace_window.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <array>
#include <cfloat>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "ntdlib/sar/formats.h"

#include "app/export_warnings.h"
#include "app/item_views.h"
#include "app/replace_common.h"
#include "app/replace_view.h"
#include "app/theme.h"
#include "app/widgets.h"
#include "edit/gba_midi.h"
#include "edit/groups.h"
#include "edit/variables.h"

namespace hokuto::app
{
namespace
{

using replace_common::EncodingText;
using replace_common::Holds;
using replace_common::NameInUse;
using replace_common::NameTaken;
using replace_common::TrackList;
using theme::Px;

// The ID of the Replace window. Its title names the sound.
constexpr char kReplaceWindow[] = "###Replace";

// The Replace window's margin from the edges of the main window, in points: the Export window's.
constexpr float kMargin = 32.0f;

// The encodings that the window offers for a new wave, in its menu's order.
constexpr std::array<ntdlib::sar::WaveEncoding, 2> kEncodings = {ntdlib::sar::WaveEncoding::kDspAdpcm,
                                                                 ntdlib::sar::WaveEncoding::kPcm16};
constexpr std::array<ntdlib::sar::WaveEncoding, 3> kDsEncodings = {
    ntdlib::sar::WaveEncoding::kImaAdpcm, ntdlib::sar::WaveEncoding::kPcm16, ntdlib::sar::WaveEncoding::kPcm8};

// What can become of a variable, in the order of the menu.
constexpr std::array kVariableChoices = {VariableChoice::kKeep, VariableChoice::kMove, VariableChoice::kFix};

const char* ChoiceText(VariableChoice choice)
{
    switch (choice)
    {
    case VariableChoice::kKeep:
        return "Keep";
    case VariableChoice::kMove:
        return "Move to";
    case VariableChoice::kFix:
        return "Fixed value";
    }

    return "";
}

// A fixed value in its menu. -1 is the value that a variable starts with.
std::string ValueText(int value)
{
    return value == -1 ? "-1 (initial value)" : std::to_string(value);
}

// What the new commands do with the variable of `row`.
std::string UseText(const VariableRow& row)
{
    const std::string global = row.number >= edit::kSoundVariables ? "Global. " : "";
    if (!row.read)
    {
        return global + "Set by the new commands.";
    }

    return global + (row.choices.empty() ? "Read by the new commands." : "Compared by the new commands.");
}

// Draws `count` rows, each with a label on its left and a menu that fills the rest. `widest` is the widest label.
// `label` gives each row's label, and `menu` draws its menu and says whether the user chose another item. A choice sets
// `choice`.
template <class Label, class Menu>
void DrawRows(const char* widest, std::size_t count, const Label& label, const Menu& menu, ReplaceChoice& choice)
{
    const float menu_x = ImGui::GetCursorPosX() + ImGui::CalcTextSize(widest).x + Px(16.0f);
    for (std::size_t i = 0; i < count; i++)
    {
        ImGui::PushID(static_cast<int>(i));
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label(i).c_str());
        ImGui::SameLine(menu_x);
        if (menu(i))
        {
            choice = ReplaceChoice::kChange;
        }
        ImGui::PopID();
    }
}

// Draws an import's choices under `heading`s: the new item's name and a menu of the sounds whose settings a new sound
// can copy. A field at the top of the menu filters its items by name. A change sets `choice`.
template <class Heading>
void DrawImportChoices(ReplaceView& view, const Heading& heading, ReplaceChoice& choice)
{
    // The name takes the keys as the window opens. The whole name is selected, and typing replaces it.
    if (view.names)
    {
        ImGui::Spacing();
        heading("Name");
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::IsWindowAppearing())
        {
            ImGui::SetKeyboardFocusHere();
        }
        if (ImGui::InputTextWithHint("##name", view.name_hint.c_str(), view.name.data(), view.name.size(),
                                     ImGuiInputTextFlags_AutoSelectAll))
        {
            choice = ReplaceChoice::kChange;
        }
    }

    // The sounds whose settings a new sound can copy.
    if (!view.models.empty())
    {
        ImGui::Spacing();
        heading("Copy settings from");
        int model = static_cast<int>(view.model);
        if (FilteredCombo("##model", view.models, model, nullptr, view.filter))
        {
            view.model = static_cast<std::size_t>(model);
            choice = ReplaceChoice::kChange;
        }
    }
}

// Draws a menu under a heading for each wave archive that the waves of a bank or a wave sound file name. A change sets
// `choice`.
template <class Heading>
void DrawWaveArchiveRows(ReplaceView& view, const Heading& heading, ReplaceChoice& choice)
{
    ImGui::Spacing();
    heading(view.rows_heading.c_str());
    const auto label = [&view](std::size_t row)
    {
        return view.wave_archive_rows[row].label;
    };
    const auto menu = [&view](std::size_t row)
    {
        return FilteredCombo("##wave_archive", view.wave_archives, view.wave_archive_rows[row].target, nullptr,
                             view.filter);
    };
    DrawRows("Wave archive 99 (999 waves)", view.wave_archive_rows.size(), label, menu, choice);
}

// Draws a menu under a heading for each program that the sounds of a bank select and the new bank lacks. Its first item
// leaves the notes silent. A change sets `choice`. The rows open at their top in a window that is `appearing`.
template <class Heading>
void DrawProgramFixRows(ReplaceView& view, const Heading& heading, bool appearing, ReplaceChoice& choice)
{
    ImGui::Spacing();
    heading("Instruments");
    const auto label = [&view](std::size_t row)
    {
        return "Program " + std::to_string(view.program_fixes[row].program);
    };
    const auto menu = [&view](std::size_t row)
    {
        ProgramFixRow& fix = view.program_fixes[row];
        return FilteredCombo("##fix", fix.items, fix.choice, "Silent", view.filter);
    };
    ImGui::SetNextWindowSizeConstraints(ImVec2(0.0f, 0.0f), ImVec2(FLT_MAX, Px(220.0f)));
    if (appearing)
    {
        ImGui::SetNextWindowScroll(ImVec2(0.0f, 0.0f));
    }
    ImGui::BeginChild("##fixes", ImVec2(0.0f, 0.0f), ImGuiChildFlags_AutoResizeY);
    DrawRows("Program 65535", view.program_fixes.size(), label, menu, choice);
    ImGui::EndChild();
}

// Draws the choices of a MIDI file that comes into a GBA file under `heading`s: a menu of the songs, and a row for each
// instrument that the file's notes play, with a menu of the voices of the song's voice group. A field at the top of
// each menu filters its items by name. A change sets `choice`. The rows open at their top in a window that is
// `appearing`.
template <class Heading>
void DrawGbaChoices(ReplaceView& view, const Heading& heading, bool appearing, ReplaceChoice& choice)
{
    ImGui::Spacing();
    heading("Song");
    int song = static_cast<int>(view.gba_song);
    if (FilteredCombo("##song", view.gba_songs, song, nullptr, view.filter) && song >= 0)
    {
        view.gba_song = static_cast<std::size_t>(song);
        choice = ReplaceChoice::kChange;
    }

    // A song of assembly names its voices by their numbers, and a SoundFont pairs nothing.
    if (view.gba_assembly || view.gba_voices.empty())
    {
        return;
    }

    ImGui::Spacing();
    heading("Instruments");
    if (view.gba_rows.empty())
    {
        MutedText("The MIDI file has no notes.");
        return;
    }

    const auto label = [&view](std::size_t row)
    {
        const edit::GbaMidiInstrument& instrument = view.gba_rows[row].instrument;
        return (instrument.drums ? "Drums, program " : "Program ") + std::to_string(instrument.program);
    };
    const auto menu = [&view](std::size_t row)
    {
        return FilteredCombo("##voice", view.gba_voices, view.gba_rows[row].voice, nullptr, view.filter);
    };
    ImGui::SetNextWindowSizeConstraints(ImVec2(0.0f, 0.0f), ImVec2(FLT_MAX, Px(220.0f)));
    if (appearing)
    {
        ImGui::SetNextWindowScroll(ImVec2(0.0f, 0.0f));
    }
    ImGui::BeginChild("##voices", ImVec2(0.0f, 0.0f), ImGuiChildFlags_AutoResizeY);
    DrawRows("Drums, program 127", view.gba_rows.size(), label, menu, choice);
    ImGui::EndChild();
}

// Draws a row for each program that the new commands select: the program in its slot, and a menu of the programs of the
// bank in that slot. A window that is `appearing` opens the rows at their top. A file of the DS has no slots.
void DrawProgramRows(ReplaceView& view, bool slots, bool appearing, ReplaceChoice& choice)
{
    ImGui::SetNextWindowSizeConstraints(ImVec2(0.0f, 0.0f), ImVec2(FLT_MAX, Px(220.0f)));
    if (appearing)
    {
        ImGui::SetNextWindowScroll(ImVec2(0.0f, 0.0f));
    }
    ImGui::BeginChild("##programs", ImVec2(0.0f, 0.0f), ImGuiChildFlags_AutoResizeY);
    const float label_width = ImGui::GetCursorPosX() + ImGui::CalcTextSize("Slot 3, program 127").x + Px(16.0f);
    for (std::size_t i = 0; i < view.programs.size(); i++)
    {
        ProgramRow& row = view.programs[i];
        ImGui::PushID(static_cast<int>(i));
        ImGui::AlignTextToFramePadding();
        if (slots)
        {
            ImGui::Text("Slot %d, program %d", row.program.slot, row.program.number);
        }
        else
        {
            ImGui::Text("Program %d", row.program.number);
        }
        ImGui::SameLine(label_width);
        ImGui::SetNextItemWidth(-FLT_MIN);
        const bool held = Holds(row);
        const std::string target = row.bank.empty() ? "No bank in this slot"
                                   : held           ? row.bank + ": program " + std::to_string(row.target)
                                                    : row.bank + " has no program " + std::to_string(row.target);
        ImGui::BeginDisabled(row.bank.empty());
        if (ImGui::BeginCombo("##target", target.c_str()))
        {
            for (const uint32_t program : row.choices)
            {
                const std::string name = "Program " + std::to_string(program);
                if (ImGui::Selectable(name.c_str(), static_cast<int>(program) == row.target) &&
                    static_cast<int>(program) != row.target)
                {
                    row.target = static_cast<int>(program);
                    choice = ReplaceChoice::kChange;
                }
            }
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();
        ImGui::PopID();
    }
    ImGui::EndChild();
}

// Draws a row for each note of MIDI's drum channel under `heading`: a menu of the programs of the channel's bank, and a
// menu of the key that the note maps to. A window that is `appearing` opens the rows at their top.
template <class Heading>
void DrawDrumRows(ReplaceView& view, const Heading& heading, bool appearing, ReplaceChoice& choice)
{
    if (view.drums.empty())
    {
        return;
    }

    ImGui::Spacing();
    heading("Drum notes");
    ImGui::SetNextWindowSizeConstraints(ImVec2(0.0f, 0.0f), ImVec2(FLT_MAX, Px(220.0f)));
    if (appearing)
    {
        ImGui::SetNextWindowScroll(ImVec2(0.0f, 0.0f));
    }
    ImGui::BeginChild("##drums", ImVec2(0.0f, 0.0f), ImGuiChildFlags_AutoResizeY);
    const ImGuiStyle& style = ImGui::GetStyle();
    const float label_width = ImGui::GetCursorPosX() + ImGui::CalcTextSize("Note 127").x + Px(16.0f);
    const float key_width = ImGui::CalcTextSize("Key 127").x + style.FramePadding.x * 2.0f + ImGui::GetFrameHeight();
    for (std::size_t i = 0; i < view.drums.size(); i++)
    {
        DrumRow& row = view.drums[i];
        ImGui::PushID(static_cast<int>(i));
        ImGui::AlignTextToFramePadding();
        ImGui::Text("Note %d", row.note);
        ImGui::SameLine(label_width);
        ImGui::SetNextItemWidth(-(key_width + style.ItemSpacing.x));
        const std::string target =
            row.program < 0 ? "Program from MIDI file" : row.bank + ": program " + std::to_string(row.program);
        ImGui::BeginDisabled(row.bank.empty());
        if (ImGui::BeginCombo("##program", target.c_str()))
        {
            if (ImGui::Selectable("Program from MIDI file", row.program < 0) && row.program >= 0)
            {
                row.program = -1;
                row.key = row.note;
                view.drums_changed = true;
                choice = ReplaceChoice::kChange;
            }
            for (const uint32_t program : row.choices)
            {
                const std::string name = "Program " + std::to_string(program);
                if (ImGui::Selectable(name.c_str(), static_cast<int>(program) == row.program) &&
                    static_cast<int>(program) != row.program)
                {
                    row.program = static_cast<int>(program);
                    view.drums_changed = true;
                    choice = ReplaceChoice::kChange;
                }
            }
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::BeginDisabled(row.program < 0);
        if (ImGui::BeginCombo("##key", ("Key " + std::to_string(row.key)).c_str()))
        {
            for (int key = 0; key < 128; key++)
            {
                const std::string name = "Key " + std::to_string(key);
                if (ImGui::Selectable(name.c_str(), key == row.key) && key != row.key)
                {
                    row.key = key;
                    view.drums_changed = true;
                    choice = ReplaceChoice::kChange;
                }
                if (key == row.key && ImGui::IsWindowAppearing())
                {
                    ImGui::SetScrollHereY();
                }
            }
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();
        ImGui::PopID();
    }
    ImGui::EndChild();
}

// The menu of the file's starts. A change sets `choice`.
void DrawStartChoice(ReplaceView& view, ReplaceChoice& choice)
{
    if (view.starts.empty())
    {
        return;
    }

    std::vector<std::string> names;
    for (const ReplaceStart& start : view.starts)
    {
        names.push_back(start.name);
    }
    int chosen = static_cast<int>(view.chosen);
    if (FilteredCombo("##start", names, chosen, nullptr, view.filter))
    {
        view.chosen = static_cast<std::size_t>(chosen);
        choice = ReplaceChoice::kChange;
    }
}

// A row for each bank slot, with a menu of the banks. A change sets `choice`.
void DrawSlotRows(ReplaceView& view, ReplaceChoice& choice)
{
    const auto label = [](std::size_t slot)
    {
        return "Slot " + std::to_string(slot);
    };
    const auto menu = [&view](std::size_t slot)
    {
        const bool changed = FilteredCombo("##slot", view.banks, view.slots[slot], "None", view.filter);
        view.slots_changed = view.slots_changed || changed;
        return changed;
    };
    DrawRows("Slot 3, program 127", view.slots.size(), label, menu, choice);
}

// The second control of a variable's row: the number that the variable moves to, or the value of a fixed variable. A
// change sets `choice`.
void DrawVariableValue(VariableRow& row, ReplaceView& view, ReplaceChoice& choice)
{
    if (row.choice == VariableChoice::kMove)
    {
        if (FilteredCombo("##to", view.numbers, row.to, nullptr, view.filter))
        {
            choice = ReplaceChoice::kChange;
        }
    }
    else if (row.choice == VariableChoice::kFix && !row.choices.empty())
    {
        // Each choice plays otherwise. Every other value plays as one of them.
        if (ImGui::BeginCombo("##value", ValueText(row.value).c_str()))
        {
            for (const int value : row.choices)
            {
                if (ImGui::Selectable(ValueText(value).c_str(), value == row.value) && value != row.value)
                {
                    row.value = value;
                    choice = ReplaceChoice::kChange;
                }
            }
            ImGui::EndCombo();
        }
    }
    else if (row.choice == VariableChoice::kFix)
    {
        PushFigures();
        const bool typed = ImGui::InputInt("##value", &row.value, 0, 0);
        ImGui::PopFont();
        if (typed)
        {
            row.value = std::clamp(row.value, -32768, 32767);
            choice = ReplaceChoice::kChange;
        }
    }
    else
    {
        MutedText(UseText(row).c_str());
    }
}

// A row for each variable, with the menus after the widest label. A change sets `choice`.
void DrawVariableRows(ReplaceView& view, ReplaceChoice& choice)
{
    const ImGuiStyle& style = ImGui::GetStyle();
    float widest = ImGui::CalcTextSize("Variable 31").x;
    for (const VariableRow& row : view.variables)
    {
        widest = std::max(widest, ImGui::CalcTextSize(("Variable " + std::to_string(row.number)).c_str()).x);
    }
    const float label_width = ImGui::GetCursorPosX() + widest + Px(16.0f);
    const float choice_width =
        ImGui::CalcTextSize("Fixed value").x + style.FramePadding.x * 2.0f + ImGui::GetFrameHeight();
    for (std::size_t i = 0; i < view.variables.size(); i++)
    {
        VariableRow& row = view.variables[i];
        ImGui::PushID(static_cast<int>(i));
        ImGui::AlignTextToFramePadding();
        ImGui::Text("Variable %d", row.number);
        ImGui::SameLine(label_width);
        ImGui::SetNextItemWidth(choice_width);
        if (ImGui::BeginCombo("##choice", ChoiceText(row.choice)))
        {
            for (const VariableChoice option : kVariableChoices)
            {
                if ((option != VariableChoice::kFix || row.read) &&
                    ImGui::Selectable(ChoiceText(option), option == row.choice) && option != row.choice)
                {
                    row.choice = option;
                    choice = ReplaceChoice::kChange;
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-FLT_MIN);
        DrawVariableValue(row, view, choice);
        ImGui::PopID();
    }
}

// Draws the choices of a sequence's replacement under `heading`s: a menu of the file's starts, a menu for each bank
// slot, a row for each program that the new commands select, and the tracks that the sound leaves out. A field at the
// top of each menu filters its items by name. A change sets `choice`. The programs open at their top in a window that
// is `appearing`.
template <class Heading>
void DrawSequenceChoices(ReplaceView& view, const Heading& heading, bool appearing, ReplaceChoice& choice)
{
    ImGui::Spacing();
    heading("Start");
    DrawStartChoice(view, choice);

    // A row for each bank slot.
    if (!view.slots.empty())
    {
        ImGui::Spacing();
        heading("Banks");
        DrawSlotRows(view, choice);
    }

    // A row for each program that the new commands select: the program in its slot, and a menu of the programs of the
    // sound's bank in that slot.
    ImGui::Spacing();
    heading("Instruments");
    if (view.programs.empty())
    {
        MutedText("The new commands don't select any programs.");
    }
    else
    {
        DrawProgramRows(view, true, appearing, choice);
    }
    DrawDrumRows(view, heading, appearing, choice);

    // A row for each variable: a menu of what becomes of it, then the number of a move or the value of a fixed
    // variable. The menus start after the widest label. SameLine counts from the window's edge, and the labels start at
    // the cursor.
    if (!view.variables.empty())
    {
        ImGui::Spacing();
        heading("Variables");
        DrawVariableRows(view, choice);
    }

    // The tracks that the sound leaves out, and a choice to allocate them.
    if (view.left_out != 0)
    {
        ImGui::Spacing();
        heading("Tracks");
        const std::string label = "Allocate " + TrackList(view.left_out) + " too";
        if (CheckBox(label.c_str(), &view.all_tracks))
        {
            choice = ReplaceChoice::kChange;
        }
    }
}

// Whether the user checked `group` for the item to join.
bool Joined(const ReplaceView& view, uint32_t group)
{
    return std::ranges::find(view.joined, group) != view.joined.end();
}

// The lines that say how the item changes the size of each group that it joins, and the size of each new group's files
// (SizeGroups).
std::vector<std::string> GroupGrowthLines(const ReplaceView& view)
{
    std::vector<std::string> lines;
    for (std::size_t i = 0; i < view.groups.size() && i < view.group_growth.size(); i++)
    {
        if (!view.group_growth[i].empty() && Joined(view, view.groups[i].group))
        {
            lines.push_back(view.group_growth[i]);
        }
    }
    for (const std::string& name : view.new_groups)
    {
        lines.push_back("New group " + name + " loads " + SizeText(view.new_group_size) + ".");
    }

    return lines;
}

// The names of the groups that load the item after the edit, the new groups last.
std::vector<std::string> LoadingGroups(const ReplaceView& view)
{
    std::vector<std::string> loading;
    for (const edit::GroupOption& option : view.groups)
    {
        if (option.holds || Joined(view, option.group))
        {
            loading.push_back(option.name);
        }
    }
    loading.insert(loading.end(), view.new_groups.begin(), view.new_groups.end());

    return loading;
}

// Makes the new group `typed`, and empties the field.
void MakeNewGroup(ReplaceView& view, const std::string& typed)
{
    view.new_groups.push_back(typed);
    view.filter.fill('\0');
    view.groups_changed = true;
}

// The typed state of the field over the groups: its text, whether it offers a new group, and whether another item has
// the name.
struct GroupFilter
{
    std::string typed;
    bool offered = false;
    bool taken = false;
};

// Draws the field that filters the groups. Enter in the field makes the new group that it offers.
GroupFilter DrawGroupFilter(ReplaceView& view)
{
    // The field takes the keys as the menu opens, empty.
    if (ImGui::IsWindowAppearing())
    {
        view.filter.fill('\0');
        ImGui::SetKeyboardFocusHere();
    }
    ImGui::SetNextItemWidth(-FLT_MIN);
    const bool entered =
        ImGui::InputTextWithHint("##filter", view.makes_groups ? "Search or type a new group name" : "Search",
                                 view.filter.data(), view.filter.size(), ImGuiInputTextFlags_EnterReturnsTrue);

    // A name that no group has offers a new group, unless another item holds the name. Enter makes the new group,
    // and the field keeps the keys. Without `makes_groups`, the field only filters the rows.
    GroupFilter filter{.typed = view.filter.data()};
    const std::string& typed = filter.typed;
    const auto named = [&typed](const edit::GroupOption& option)
    {
        return option.name == typed;
    };
    filter.offered = view.makes_groups && !typed.empty() && !std::ranges::any_of(view.groups, named) &&
                     std::ranges::find(view.new_groups, typed) == view.new_groups.end();
    filter.taken = filter.offered && NameTaken(view.taken_names, typed);
    if (entered && filter.offered && !filter.taken)
    {
        MakeNewGroup(view, typed);
        ImGui::SetKeyboardFocusHere(-1);
    }

    return filter;
}

// The rows of the archive's groups and of the new groups whose names match the field.
std::pair<std::vector<std::size_t>, std::vector<std::size_t>> ShownGroups(const ReplaceView& view)
{
    std::vector<std::size_t> shown;
    for (std::size_t i = 0; i < view.groups.size(); i++)
    {
        if (Matches(view.groups[i].name, view.filter.data()))
        {
            shown.push_back(i);
        }
    }
    std::vector<std::size_t> shown_new;
    for (std::size_t k = 0; k < view.new_groups.size(); k++)
    {
        if (Matches(view.new_groups[k], view.filter.data()))
        {
            shown_new.push_back(k);
        }
    }

    return {std::move(shown), std::move(shown_new)};
}

// The offer's row of a new group named `typed`, or a line that says that no row matches.
void DrawGroupOffer(ReplaceView& view, const std::string& typed, bool offered, bool taken,
                    const std::vector<std::size_t>& shown, const std::vector<std::size_t>& shown_new)
{
    // The offer's row. A click on it makes the new group, and the menu stays open.
    if (offered && view.filter[0] != '\0')
    {
        const std::string label = "New group \"" + typed + "\"";
        ImGui::PushItemFlag(ImGuiItemFlags_AutoClosePopups, false);
        if (ImGui::MenuItem(label.c_str(), taken ? "name taken" : "", false, !taken))
        {
            MakeNewGroup(view, typed);
        }
        ImGui::PopItemFlag();
        if (taken && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        {
            WrappedTooltip(NameInUse(typed));
        }
    }
    else if (shown.empty() && shown_new.empty())
    {
        const bool none = view.groups.empty() && view.new_groups.empty();
        MutedText(!none ? "No matches" : view.makes_groups ? "No groups. Type a name for a new group." : "No groups");
    }
}

// The row of the archive's group `option`. A click checks or unchecks it.
void DrawGroupRow(ReplaceView& view, const edit::GroupOption& option)
{
    const bool reachable = !option.holds && option.unavailable.empty();
    const char* note = option.holds                  ? "loads it"
                       : !option.unavailable.empty() ? "unavailable"
                       : option.proposed             ? "proposed"
                                                     : "";
    if (ImGui::MenuItem(option.name.c_str(), note, option.holds || Joined(view, option.group), reachable))
    {
        if (Joined(view, option.group))
        {
            std::erase(view.joined, option.group);
        }
        else
        {
            view.joined.insert(std::ranges::upper_bound(view.joined, option.group), option.group);
        }
        view.groups_changed = true;
    }
    if (!option.unavailable.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    {
        WrappedTooltip(option.unavailable);
    }
}

// The rows of the `shown` groups of the archive, then the `shown_new` new groups, in a child that scrolls.
void DrawGroupRows(ReplaceView& view, const std::vector<std::size_t>& shown, const std::vector<std::size_t>& shown_new)
{
    const std::size_t count = shown.size() + shown_new.size();
    if (count > 0)
    {
        const float rows = ImGui::GetTextLineHeightWithSpacing() * static_cast<float>(count);
        ImGui::BeginChild("##items", ImVec2(0.0f, std::min(rows, Px(260.0f))));
        ImGui::PushItemFlag(ImGuiItemFlags_AutoClosePopups, false);
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(shown.size()));
        while (clipper.Step())
        {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++)
            {
                const edit::GroupOption& option = view.groups[shown[static_cast<std::size_t>(row)]];
                ImGui::PushID(row);
                DrawGroupRow(view, option);
                ImGui::PopID();
            }
        }
        std::optional<std::size_t> dropped;
        for (const std::size_t k : shown_new)
        {
            ImGui::PushID(view.new_groups[k].c_str());
            if (ImGui::MenuItem(view.new_groups[k].c_str(), "new", true))
            {
                dropped = k;
            }
            ImGui::PopID();
        }
        if (dropped)
        {
            view.new_groups.erase(view.new_groups.begin() + static_cast<std::ptrdiff_t>(*dropped));
            view.groups_changed = true;
        }
        ImGui::PopItemFlag();
        ImGui::EndChild();
    }
}

// The note under the menu, with `loading` the groups that load the item.
void DrawGroupNotes(const ReplaceView& view, const std::vector<std::string>& loading)
{
    const std::string& note = loading.empty() ? view.groups_none : view.groups_note;
    if (!note.empty())
    {
        MutedText(note.c_str());
    }
    if (!view.new_groups.empty())
    {
        MutedText("Games load groups by number. A new group loads only if the game code is changed to load it.");
    }
}

// Draws the groups under `heading`: a menu with a row for each group of the archive, and a field at its top that
// filters the rows by name. A row's check says whether the group loads the item after the edit. The user checks the
// groups that the item joins. A group that holds the item keeps its check, and a group that the edit can't reach takes
// none. A name in the field that no group has offers a new group of that name, unless another item holds the name. A
// click on the offer or Enter in the field makes the new group. Its row follows the archive's groups, checked, and a
// click on it takes the new group out again. A choice sets `view.groups_changed`.
template <class Heading>
void DrawGroupChoices(ReplaceView& view, const Heading& heading)
{
    ImGui::Spacing();
    heading("Groups");

    const std::vector<std::string> loading = LoadingGroups(view);
    const std::string preview = loading.empty() ? "None" : NameList(loading);
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::BeginCombo("##groups", preview.c_str(), ImGuiComboFlags_HeightLargest))
    {
        const GroupFilter filter = DrawGroupFilter(view);
        const auto [shown, shown_new] = ShownGroups(view);

        // The offer's row. A click on it makes the new group, and the menu stays open.
        DrawGroupOffer(view, filter.typed, filter.offered, filter.taken, shown, shown_new);

        // The rows scroll under the field. A click checks or unchecks a row, and the menu stays open. The new groups
        // follow the archive's groups. Without rows, the menu ends under the field: a child of no height would fill the
        // menu.
        DrawGroupRows(view, shown, shown_new);
        ImGui::EndCombo();
    }

    // Which groups the window proposes, or what follows when no group loads the item. A new group loads nothing until
    // the game's code loads it.
    DrawGroupNotes(view, loading);
}

// Draws the choices of the item under `heading`s: those of an import, of a sequence, and of the songs, the wave
// archives, the programs, the drums, the fixes and the groups. A change sets `choice`.
template <class Heading>
void DrawChoiceSections(ReplaceView& view, const Heading& heading, bool appearing, ReplaceChoice& choice)
{
    if (view.import)
    {
        DrawImportChoices(view, heading, choice);
    }
    if (view.sequence)
    {
        DrawSequenceChoices(view, heading, appearing, choice);
    }
    if (!view.gba_songs.empty())
    {
        DrawGbaChoices(view, heading, appearing, choice);
    }
    if (!view.wave_archive_rows.empty())
    {
        DrawWaveArchiveRows(view, heading, choice);
    }
    if (!view.sequence && !view.programs.empty())
    {
        ImGui::Spacing();
        heading("Instruments");
        DrawProgramRows(view, false, appearing, choice);
    }
    if (!view.sequence)
    {
        DrawDrumRows(view, heading, appearing, choice);
    }
    if (!view.program_fixes.empty())
    {
        DrawProgramFixRows(view, heading, appearing, choice);
    }
    if (view.offers_groups)
    {
        DrawGroupChoices(view, heading);
    }
}

// The menu of the encodings of a WAV file's wave. A change sets `choice`.
void DrawEncodingMenu(ReplaceView& view, ReplaceChoice& choice)
{
    if (ImGui::BeginCombo("##encoding", EncodingText(*view.encoding)))
    {
        for (const ntdlib::sar::WaveEncoding encoding : view.ds_encodings
                                                            ? std::span<const ntdlib::sar::WaveEncoding>(kDsEncodings)
                                                            : std::span<const ntdlib::sar::WaveEncoding>(kEncodings))
        {
            if (ImGui::Selectable(EncodingText(encoding), encoding == *view.encoding) && encoding != *view.encoding)
            {
                view.encoding = encoding;
                choice = ReplaceChoice::kChange;
            }
        }
        ImGui::EndCombo();
    }
}

// Draws the choices of a new wave or a WAV file's wave under `heading`s: the wave archive, the encoding, the sample
// rate and the loop. A change sets `choice`.
template <class Heading>
void DrawWaveChoices(ReplaceView& view, const Heading& heading, ReplaceChoice& choice)
{
    // A new wave: a menu of the wave archives.
    if (view.wave_archive)
    {
        ImGui::Spacing();
        heading("Wave archive");
        if (FilteredCombo("##wave_archive", view.wave_archives, *view.wave_archive, nullptr, view.filter))
        {
            choice = ReplaceChoice::kChange;
        }
    }

    // A WAV file's wave: a menu of the encodings.
    if (view.encoding)
    {
        ImGui::Spacing();
        heading("Encoding");
        ImGui::SetNextItemWidth(-FLT_MIN);
        DrawEncodingMenu(view, choice);
    }

    // A WAV file's wave: a menu of the rates, and one of the loops.
    if (view.rates.size() > 1)
    {
        ImGui::Spacing();
        heading("Sample rate");
        std::vector<std::string> names;
        for (std::size_t i = 0; i < view.rates.size(); i++)
        {
            names.push_back(Grouped(view.rates[i]) + " Hz" + (i == 0 && !view.first_rate_lower ? " (WAV file)" : ""));
        }
        int rate = static_cast<int>(view.rate);
        if (FilteredCombo("##rate", names, rate, nullptr, view.filter) && rate >= 0)
        {
            view.rate = static_cast<std::size_t>(rate);
            choice = ReplaceChoice::kChange;
        }
    }
    if (view.present_loop)
    {
        ImGui::Spacing();
        heading("Loop");
        const std::vector<std::string> names = {"None", "Samples " + Grouped(view.present_loop->start) + " to " +
                                                            Grouped(view.present_loop->end) + " (current loop)"};
        int loop = view.take_loop ? 1 : 0;
        if (FilteredCombo("##loop", names, loop, nullptr, view.filter) && loop >= 0)
        {
            view.take_loop = loop == 1;
            choice = ReplaceChoice::kChange;
        }
    }
}

// The list of the warnings, or the reason why the item can't go in, in a child that scrolls. The list opens at its top
// in a window that is `appearing`.
void DrawWarningList(const ReplaceView& view, bool appearing)
{
    ImGui::SetNextWindowSizeConstraints(ImVec2(0.0f, 0.0f), ImVec2(FLT_MAX, Px(220.0f)));
    if (appearing)
    {
        ImGui::SetNextWindowScroll(ImVec2(0.0f, 0.0f));
    }
    ImGui::BeginChild("##warnings", ImVec2(0.0f, 0.0f), ImGuiChildFlags_AutoResizeY);
    ImGui::PushTextWrapPos(0.0f);
    if (!view.error.empty())
    {
        ImGui::Bullet();
        TextWrappedAtSpaces(view.error);
    }
    else
    {
        // The growth of the chosen groups follows the choice without a new view.
        const std::vector<std::string> growth = GroupGrowthLines(view);
        if (view.warnings.empty() && growth.empty())
        {
            MutedText("None");
        }
        for (const std::string& warning : view.warnings)
        {
            ImGui::Bullet();
            TextWrappedAtSpaces(warning);
        }
        for (const std::string& line : growth)
        {
            ImGui::Bullet();
            TextWrappedAtSpaces(line);
        }
    }
    ImGui::PopTextWrapPos();
    ImGui::EndChild();
}

// Play, with a menu of the sounds or the waves that can play beside it, and a note while the preview is being
// prepared. The menu ends before `right`, where Cancel starts. A click sets `choice`.
void DrawPlayControls(ReplaceView& view, float right, ReplaceChoice& choice)
{
    const ImGuiStyle& style = ImGui::GetStyle();
    // A menu of the sounds or the waves that can play sits beside Play. While the preview is being prepared, Play
    // waits. While the preview plays, the button reads Stop and stops it. The browser's Play does the same.
    ImGui::BeginDisabled(!view.previewing &&
                         (view.preparing || !(view.result || view.ready || !view.wave.empty() || !view.waves.empty())));
    if (ImGui::Button(view.previewing ? "Stop###play" : "Play###play"))
    {
        choice = view.previewing ? ReplaceChoice::kStop : ReplaceChoice::kPlay;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (view.plays.size() > 1)
    {
        int play = static_cast<int>(view.play);
        const float preparing = view.preparing ? ImGui::CalcTextSize("Preparing…").x + style.ItemSpacing.x : 0.0f;
        const float width = std::max(right - ImGui::GetCursorPosX() - style.ItemSpacing.x - preparing, Px(80.0f));
        if (FilteredCombo("##play", view.plays, play, nullptr, view.filter, width))
        {
            view.play = static_cast<std::size_t>(play);
        }
        ImGui::SameLine();
    }
    if (view.preparing)
    {
        ImGui::AlignTextToFramePadding();
        MutedText("Preparing…");
        ImGui::SameLine();
    }
}

// Opens the modal window `title`, in the style of the dialogs, up to `tallest` high. Returns whether it shows.
bool BeginReplaceModal(const std::string& title, float tallest)
{
    ImGui::SetNextWindowSize(ImVec2(Px(560.0f), 0.0f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(0.0f, 0.0f), ImVec2(FLT_MAX, tallest));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, Px(theme::kDialogPadding));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImGui::ColorConvertU32ToFloat4(theme::kRaised));
    ImGui::PushStyleColor(ImGuiCol_Border, ImGui::ColorConvertU32ToFloat4(theme::kBorderStrong));
    constexpr ImGuiWindowFlags kFlags =
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings;
    bool open = true;
    const bool visible = ImGui::BeginPopupModal(title.c_str(), &open, kFlags);
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar();

    return visible;
}

// The pane of the choices and the warnings of `view`, below the heading and the source. The window is up to `tallest`
// high. The choices take no input while the edit's work runs (`working`).
void DrawChoicesPane(ReplaceView& view, float tallest, bool appearing, bool working, ReplaceChoice& choice)
{
    // The choices and the warnings scroll where they don't fit. The title bar, the padding, the buttons and the space
    // above them keep their room below the window's tallest height.
    const float kept =
        ImGui::GetFrameHeight() * 2.0f + Px(theme::kDialogPadding).y * 2.0f + ImGui::GetStyle().ItemSpacing.y * 3.0f;
    ImGui::SetNextWindowSizeConstraints(ImVec2(0.0f, 0.0f), ImVec2(FLT_MAX, std::max(tallest - kept, Px(80.0f))));
    if (appearing)
    {
        ImGui::SetNextWindowScroll(ImVec2(0.0f, 0.0f));
    }
    ImGui::BeginChild("##choices", ImVec2(0.0f, 0.0f), ImGuiChildFlags_AutoResizeY);

    // The file, and a sequence's choices. The headings follow the Export window's.
    ImGui::PushTextWrapPos(0.0f);

    const auto heading = [](const char* text)
    {
        DrawDialogHeading(ImGui::GetWindowDrawList(), text);
    };

    heading(view.heading.c_str());
    TextWrappedAtSpaces(view.source);
    // While the edit's work runs, the choices keep their look and take no input. The mixer's strips do the same.
    ImGui::PushStyleVar(ImGuiStyleVar_DisabledAlpha, 1.0f);
    ImGui::BeginDisabled(working);
    ImGui::PopStyleVar();
    DrawChoiceSections(view, heading, appearing, choice);

    DrawWaveChoices(view, heading, choice);
    ImGui::EndDisabled();

    // The warnings, or the reason why the sequence or the file can't go in. The reason shows alone: the warnings
    // describe an edit that the window can't make. They scroll once they fill the window's height.
    ImGui::Spacing();
    heading("Warnings");
    DrawWarningList(view, appearing);
    ImGui::PopTextWrapPos();
    ImGui::EndChild();
}

// The buttons at the bottom of the window, with the work of an edit while it runs (`working`).
void DrawReplaceButtons(ReplaceView& view, bool working, ReplaceChoice& choice)
{
    // Cancel and the action sit at the bottom right, with the action on the right. Play sits at the bottom left. The
    // work of a confirmed import shows there too, and so does the work of a replacement that puts files into groups. An
    // import without a result and such a replacement close the window once their work is done.
    ImGui::Spacing();
    ImGui::Spacing();
    const ImGuiStyle& style = ImGui::GetStyle();
    const float buttons = ImGui::CalcTextSize("Cancel").x + ImGui::CalcTextSize(view.action.c_str()).x +
                          style.FramePadding.x * 4.0f + style.ItemSpacing.x;
    const float right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - buttons;
    if (working)
    {
        ImGui::AlignTextToFramePadding();
        MutedText(view.working.c_str());
        ImGui::SameLine();
    }
    else if (!view.wave.empty() || !view.waves.empty() || view.playable)
    {
        DrawPlayControls(view, right, choice);
    }
    ImGui::SetCursorPosX(right);
    if (ImGui::Button("Cancel"))
    {
        ImGui::CloseCurrentPopup();
        choice = ReplaceChoice::kCancel;
    }
    ImGui::SameLine();
    ImGui::BeginDisabled((!view.result && !view.ready) || choice == ReplaceChoice::kChange || working);
    if (PrimaryButton(view.action.c_str()))
    {
        if (view.result && view.joined.empty() && view.new_groups.empty())
        {
            ImGui::CloseCurrentPopup();
        }
        choice = ReplaceChoice::kReplace;
    }
    ImGui::EndDisabled();
}

} // namespace

ReplaceChoice DrawReplaceWindow(ReplaceView& view, std::optional<ImVec2>& place, std::optional<float>& top)
{
    if (!ImGui::IsPopupOpen(kReplaceWindow))
    {
        ImGui::OpenPopup(kReplaceWindow);
    }

    // A window over the middle of the main window, in the style of the Export window. It fits its contents, up to the
    // main window's height less the margins. The user can drag it by its title bar. It then opens where the drag left
    // it until Hokuto quits: `place` keeps its centre. The warnings make the window taller after it opens, such as a
    // line for each group ticked in the menu of groups. The window keeps its top and grows down, like the Export
    // window. It moves up only to stay inside the main window. The menu and the rows under the mouse then stay where
    // they were. While the drag goes on, the drag places the window.
    const std::string title = view.title + kReplaceWindow;
    const DialogPlace placed = PlaceDialog(title, place, top, Px(kMargin));
    const float tallest = std::max(ImGui::GetMainViewport()->WorkSize.y - Px(kMargin) * 2.0f, Px(200.0f));
    if (!BeginReplaceModal(title, tallest))
    {
        return ReplaceChoice::kCancel;
    }

    // Escape cancels, unless the new item's name is being typed in. The first Escape there ends the field alone.
    if (EscapeCloses())
    {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return ReplaceChoice::kCancel;
    }

    // Each window opens with its choices, its warnings and the panes inside the choices scrolled to the top. The panes
    // keep their names from one window to the next, and Dear ImGui keeps their scroll with the names.
    const bool appearing = ImGui::IsWindowAppearing();

    // A drag that moves the window gives its new place. A click on the title bar that moves nothing keeps the old one.
    // The first frame after the window appears records the top of the window, and so does each frame of a drag.
    FollowDialogDrag(placed, place, top);

    ReplaceChoice choice = ReplaceChoice::kNone;
    const bool working = !view.working.empty();
    DrawChoicesPane(view, tallest, appearing, working, choice);
    DrawReplaceButtons(view, working, choice);
    ImGui::EndPopup();

    return choice;
}

void CloseReplaceWindow()
{
    if (ImGui::IsPopupOpen(kReplaceWindow) && ImGui::BeginPopupModal(kReplaceWindow))
    {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

} // namespace hokuto::app
