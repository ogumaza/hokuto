// SPDX-License-Identifier: MIT

// The inspector: the selected track's card, its instruments, mode and colour, and its channel strip.

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "app/arrangement.h"
#include "app/channel_strip.h"
#include "app/mixer.h"
#include "app/song_view.h"
#include "app/sound_section.h"
#include "app/theme.h"
#include "app/time_text.h"
#include "app/transport.h"
#include "app/views.h"
#include "app/widgets.h"
#include "app/zone_parts.h"

namespace hokuto::app
{
namespace
{

using channel_strip::DrawMuteSolo;
using channel_strip::DrawStrip;
using channel_strip::OutputStrip;
using channel_strip::SongTrackStrip;
using channel_strip::Strip;
using theme::Px;
using zone_parts::BeginZone;
using zone_parts::DrawPalette;
using zone_parts::DrawPropertyRows;
using zone_parts::DrawSectionHeading;
using zone_parts::kColourPopup;
using zone_parts::kInspectorMargin;
using zone_parts::kPreferencesItem;
using zone_parts::kPropertyHeight;
using zone_parts::kSwitchHeight;
using zone_parts::Property;
using zone_parts::SectionHeading;
using zone_parts::SelectedTrack;

// The sizes of the inspector's menus of a track's bank and program, before the display's scale: the width of the
// program's label, the gap round a menu, and the least width of the menus' lists. The Sound section's rows take the
// same sizes.
constexpr float kInstrumentLabelWidth = 84.0f;
constexpr float kInstrumentGap = 6.0f;
constexpr float kInstrumentListWidth = 240.0f;

// The least height of the inspector's channel strips, before the display's scale. The strips show their controls at
// that height.
constexpr float kLeastStripHeight = 340.0f;

// The selected part when it's on the selected track, or nullptr.
const PartView* SelectedPart(const TrackView& track, const ArrangementState& state)
{
    if (state.selected_part_track != state.selected_track || state.selected_part < 0 ||
        state.selected_part >= static_cast<int>(track.parts.size()))
    {
        return nullptr;
    }

    return &track.parts[static_cast<std::size_t>(state.selected_part)];
}

// The top of the inspector: the track's name in a box of its colour, with its M and S buttons. Returns the bottom of
// the box.
float DrawTrackCard(ImDrawList* draw, ImVec2 pos, float width, TrackView& track)
{
    const float height = Px(36.0f);
    const ImVec2 end(pos.x + width, pos.y + height);
    draw->AddRectFilled(pos, end, track.colour, Px(6.0f));

    const ImVec2 button(Px(24.0f), Px(20.0f));
    const float gap = Px(4.0f);
    const ImVec2 mute(end.x - Px(8.0f) - button.x * 2.0f - gap, std::floor(pos.y + (height - button.y) * 0.5f));
    DrawMuteSolo(draw, mute, button, gap, &track.mute, &track.solo);

    ImFont* strong = theme::GetFonts().strong;
    const std::string shown = FitText(strong, theme::kTextSize, track.name, mute.x - Px(6.0f) - pos.x - Px(10.0f));
    const ImVec2 name_size = TextSize(strong, theme::kTextSize, shown);
    DrawText(draw, strong, theme::kTextSize, ImVec2(pos.x + Px(10.0f), pos.y + (height - name_size.y) * 0.5f),
             theme::TextOn(track.colour), shown);

    return end.y;
}

// The names that the inspector's menu of banks gives the bank slots of `slots` that hold banks, and the slot of each. A
// bank in two slots shows its slot after its name.
struct BankMenu
{
    std::vector<std::string> names;
    std::vector<int> slots;
};

BankMenu BankMenuOf(std::span<const SlotView> slots)
{
    BankMenu menu;
    for (std::size_t slot = 0; slot < slots.size(); slot++)
    {
        const std::string& bank = slots[slot].bank;
        if (bank.empty())
        {
            continue;
        }

        const auto same = [&bank](const SlotView& other)
        {
            return other.bank == bank;
        };
        const bool twice = std::count_if(slots.begin(), slots.end(), same) > 1;
        menu.names.push_back(twice ? bank + " (slot " + std::to_string(slot) + ")" : bank);
        menu.slots.push_back(static_cast<int>(slot));
    }

    return menu;
}

// Draws the inspector's rows of a track's instrument: its bank and its program. While the song lists the sound's bank
// slots, each row holds a menu: the banks in the slots, and the programs of the track's bank. The bank's menu lies
// under its label, at the rows' width: banks have long names. The Sound section lays out its pan curve the same way. A
// choice waits in the track's view (TrackView::instrument_choice). A bank keeps the track's program where it has one,
// and takes its first program otherwise. While `busy`, and for a track without notes, the rows show text. Returns the
// bottom of the rows.
float DrawInstrumentRows(ImDrawList* draw, ImVec2 pos, float width, const SongView& song, TrackView& track,
                         ArrangementState& state, bool busy)
{
    const std::size_t slot = static_cast<std::size_t>(std::max(track.slot, 0));
    if (busy || track.slot < 0 || slot >= song.slots.size() ||
        (song.slots[slot].programs.empty() && track.programs.empty()))
    {
        const std::array<Property, 2> instrument = {
            Property{"Bank", track.bank},
            Property{"Program", track.program < 0 ? std::string() : std::to_string(track.program)},
        };
        return DrawPropertyRows(draw, pos, width, instrument);
    }

    // The bank's label, with its menu under it. Another bank keeps the program where it has it.
    ImFont* text = theme::GetFonts().text;
    const float frame = ImGui::GetFrameHeight();
    const float gap = Px(kInstrumentGap);
    const ImVec2 bank_size = TextSize(text, theme::kTextSize, "Bank");
    const float label_height = std::floor(bank_size.y + gap);
    DrawText(draw, text, theme::kTextSize, ImVec2(pos.x, pos.y + std::floor((label_height - bank_size.y) * 0.5f)),
             theme::kTextMuted, "Bank");

    // A linked instrument shows an equals sign at the end of the label's row. Linked parts and mixes show the same
    // sign. The menus say what else an edit changes.
    const std::string also =
        track.instrument_reach.empty() ? std::string() : "An edit also changes " + track.instrument_reach + ".";
    if (!also.empty())
    {
        const ImVec2 mark_size = TextSize(text, theme::kTextSize, "=");
        const ImVec2 mark_min(pos.x + width - mark_size.x, pos.y + std::floor((label_height - mark_size.y) * 0.5f));
        DrawText(draw, text, theme::kTextSize, mark_min, theme::kText, "=");
        ImGui::SetCursorScreenPos(mark_min);
        ImGui::InvisibleButton("##linked", mark_size);
        ItemTooltip("Linked instrument. Other tracks or sounds share some commands with this track. " + also);
    }

    const BankMenu banks = BankMenuOf(song.slots);
    const auto found = std::find(banks.slots.begin(), banks.slots.end(), track.slot);
    int chosen_bank = found != banks.slots.end() ? static_cast<int>(found - banks.slots.begin()) : -1;
    ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + label_height));
    ImGui::SetNextWindowSizeConstraints(ImVec2(Px(kInstrumentListWidth), 0.0f), ImVec2(FLT_MAX, FLT_MAX));
    const bool bank_chosen = FilteredCombo("##bank", banks.names, chosen_bank, nullptr, state.instrument_filter, width);
    ItemTooltip(also);
    if (bank_chosen && chosen_bank >= 0)
    {
        const int to = banks.slots[static_cast<std::size_t>(chosen_bank)];
        const std::vector<uint32_t>& programs = song.slots[static_cast<std::size_t>(to)].programs;
        const bool kept = std::find(programs.begin(), programs.end(), track.program) != programs.end();
        if (!programs.empty())
        {
            track.instrument_choice = InstrumentChoice{to, kept ? track.program : static_cast<int>(programs.front())};
        }
    }

    // The program's label, with its menu of the programs of the track's bank beside it.
    const float row = pos.y + label_height + frame + std::floor(gap * 0.5f);
    const ImVec2 program_size = TextSize(text, theme::kTextSize, "Program");
    DrawText(draw, text, theme::kTextSize, ImVec2(pos.x, row + std::floor((frame + gap - program_size.y) * 0.5f)),
             theme::kTextMuted, "Program");
    const std::vector<uint32_t>& programs = track.programs.empty() ? song.slots[slot].programs : track.programs;
    std::vector<std::string> names;
    int chosen_program = -1;
    for (std::size_t i = 0; i < programs.size(); i++)
    {
        names.push_back("Program " + std::to_string(programs[i]));
        chosen_program = static_cast<int>(programs[i]) == track.program ? static_cast<int>(i) : chosen_program;
    }
    // The track's program heads the menu where the bank doesn't contain it. The notes of that program play nothing.
    const std::string missing =
        chosen_program < 0 && track.program >= 0 ? "Program " + std::to_string(track.program) + " (not in bank)" : "";
    const float menu_left = pos.x + Px(kInstrumentLabelWidth);
    ImGui::SetCursorScreenPos(ImVec2(menu_left, row + std::floor(gap * 0.5f)));
    ImGui::SetNextWindowSizeConstraints(ImVec2(Px(kInstrumentListWidth), 0.0f), ImVec2(FLT_MAX, FLT_MAX));
    const bool program_chosen =
        FilteredCombo("##program", names, chosen_program, missing.empty() ? nullptr : missing.c_str(),
                      state.instrument_filter, pos.x + width - menu_left);
    ItemTooltip(also);
    if (program_chosen && chosen_program >= 0)
    {
        track.instrument_choice =
            InstrumentChoice{track.slot, static_cast<int>(programs[static_cast<std::size_t>(chosen_program)])};
    }

    return row + frame + gap;
}

// Draws the inspector's Sequence section under `pos`: a switch between the two ways to edit the shown sequence. A
// timeline holds each note by itself. NintendoWare's MIDI converter laid the sequence out that way from a MIDI file. A
// program holds the commands as the console's bytes hold them. A click on the other way sets the song's `mode_request`.
// A sequence that the converter didn't write can't be a timeline, and the tooltip says why. Returns the bottom of the
// switch.
float DrawModeSwitch(ImVec2 pos, float width, SongView& song, bool busy)
{
    const std::array<std::string, 2> kLabels = {"Timeline", "Program"};
    const std::array<uint8_t, 2> lit = {static_cast<uint8_t>(song.timeline ? 1 : 0),
                                        static_cast<uint8_t>(song.timeline ? 0 : 1)};
    const ImVec2 size(width, Px(kSwitchHeight));
    const float top = pos.y + Px(4.0f);
    ImGui::SetCursorScreenPos(ImVec2(pos.x, top));
    const int clicked = SegmentedSwitch("##mode", kLabels, lit, size);

    // The half under the mouse explains its way, or why the sequence can't take it.
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const bool over = ImGui::IsWindowHovered() && mouse.y >= top && mouse.y < top + size.y && mouse.x >= pos.x &&
                      mouse.x < pos.x + width;
    const bool timeline_half = mouse.x < pos.x + width * 0.5f;
    if (over && !ImGui::IsAnyMouseDown())
    {
        WrappedTooltip(timeline_half ? (song.timeline_refused.empty()
                                            ? "Edit the sequence as a timeline, the way the NintendoWare MIDI "
                                              "converter wrote it: separate notes and movable loop points. Saving "
                                              "rebuilds the subroutines as the converter would."
                                            : song.timeline_refused)
                                     : "Edit the commands as stored: subroutines, calls, jumps and labels.");
    }
    if (!busy && clicked == 0 && !song.timeline && song.timeline_refused.empty())
    {
        song.mode_request = true;
    }
    else if (!busy && clicked == 1 && song.timeline)
    {
        song.mode_request = false;
    }

    return top + size.y + Px(4.0f);
}

// Draws the inspector's row of a track's colour, under the track's properties: the label on the left and a swatch of
// the colour on the right. While the tracks' colours are Custom, a click on the swatch offers the palette. Returns the
// bottom of the row.
float DrawColourProperty(ImDrawList* draw, ImVec2 pos, float width, const TrackView& track, ArrangementState& state)
{
    ImFont* text = theme::GetFonts().text;
    const float height = Px(kPropertyHeight);
    const ImVec2 label_size = TextSize(text, theme::kTextSize, "Colour");
    DrawText(draw, text, theme::kTextSize, ImVec2(pos.x, pos.y + (height - label_size.y) * 0.5f), theme::kTextMuted,
             "Colour");

    // The swatch.
    const ImVec2 size(Px(28.0f), Px(14.0f));
    const ImVec2 min(pos.x + width - size.x, std::floor(pos.y + (height - size.y) * 0.5f));
    const ImVec2 max(min.x + size.x, min.y + size.y);
    ImGui::SetCursorScreenPos(min);
    ImGui::InvisibleButton("##colour", size);
    const bool hovered = state.custom_colours && ImGui::IsItemHovered();
    draw->AddRectFilled(min, max, track.colour, Px(3.0f));
    if (hovered)
    {
        const float out = Px(2.0f);
        draw->AddRect(ImVec2(min.x - out, min.y - out), ImVec2(max.x + out, max.y + out), theme::kText, Px(4.0f),
                      Px(1.5f));
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }
    if (!state.custom_colours)
    {
        ItemTooltip(std::string("Choose Custom track colours in ") + kPreferencesItem + " to set this colour");
    }
    else if (ImGui::IsItemClicked())
    {
        ImGui::OpenPopup(kColourPopup);
    }

    // The palette.
    if (ImGui::BeginPopup(kColourPopup))
    {
        if (std::optional<TrackColourPick> pick = DrawPalette(state.selected_track, track.colour))
        {
            state.colour_pick = pick;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    return pos.y + height;
}

// The sections of the selected track from `y`: its card, its instrument and colour, and the selected part on it, or
// a line that says no track is selected. Returns the top of the next section.
float DrawTrackSections(ImDrawList* draw, float left, float y, float width, float gap, SongView* song, TrackView* track,
                        ArrangementState& state, bool busy)
{
    if (track != nullptr)
    {
        y = DrawTrackCard(draw, ImVec2(left, y), width, *track) + gap;
        const SectionHeading track_heading = DrawSectionHeading(draw, ImVec2(left, y), width, "Track", "", true);
        y = track_heading.bottom;
        if (track_heading.open)
        {
            y = DrawInstrumentRows(draw, ImVec2(left, y), width, *song, *track, state, busy);
            y = DrawColourProperty(draw, ImVec2(left, y), width, *track, state);
        }
        y += gap;

        const SectionHeading part_heading = DrawSectionHeading(draw, ImVec2(left, y), width, "Part", "", true);
        y = part_heading.bottom;
        const PartView* part = SelectedPart(*track, state);
        std::array<Property, 3> details = {Property{"Start", ""}, Property{"Length", ""}, Property{"Notes", ""}};
        if (part != nullptr)
        {
            const double ticks_per_quarter = song->ticks_per_quarter;
            details[0].value = PositionText(part->start / ticks_per_quarter, song->beats_per_bar);
            details[1].value = LengthText(part->length / ticks_per_quarter, song->beats_per_bar);
            details[2].value = std::to_string(part->notes.size() + part->unstarted.size());
        }
        if (part_heading.open)
        {
            y = DrawPropertyRows(draw, ImVec2(left, y), width, details);
        }
        y += gap;
    }
    else
    {
        ImFont* font = theme::GetFonts().text;
        const ImVec2 size = TextSize(font, theme::kTextSize, "No track is selected");
        DrawText(draw, font, theme::kTextSize, ImVec2(left + (width - size.x) * 0.5f, y + Px(8.0f)), theme::kTextMuted,
                 "No track is selected");
        y += Px(8.0f) + size.y + gap;
    }

    return y;
}

// The channel strips of `track` and of the output at `left` and `y`, each `strip` large, in a row `width` wide.
void DrawInspectorStrips(SongView* song, MasterBus& master, TrackView* track, float left, float y, float width,
                         ImVec2 strip, bool busy)
{
    if (track != nullptr)
    {
        ImGui::SetCursorScreenPos(ImVec2(left, y));
        ImGui::PushID("track");
        const Strip track_strip = SongTrackStrip(*song, *track, false, busy);
        DrawStrip(track_strip, strip);
        ImGui::PopID();
    }
    ImGui::SetCursorScreenPos(ImVec2(left + width - strip.x, y));
    ImGui::PushID("output");
    DrawStrip(OutputStrip(master), strip);
    ImGui::PopID();
}

} // namespace

void DrawInspector(SongView* song, MasterBus& master, ArrangementState& state, SoundSection& sound, bool busy,
                   bool* open)
{
    if (!*open)
    {
        return;
    }

    if (BeginZone(kInspector, open))
    {
        // The header names the selected track as the mixer and the Key Editor name it.
        TrackView* track = SelectedTrack(song, state);
        ZoneHeader("Inspector", track != nullptr ? track->name : std::string());

        // The sections scroll when they're taller than the zone.
        ImGui::BeginChild("##sections", ImVec2(0.0f, 0.0f));
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        const float margin = Px(kInspectorMargin);
        const float left = origin.x + margin;
        const float width = avail.x - margin * 2.0f;
        const float gap = Px(18.0f);

        // The track's name, its instrument, and the selected part on it.
        float y = DrawTrackSections(draw, left, origin.y + margin, width, gap, song, track, state, busy);

        // How the program edits the shown sequence.
        if (song != nullptr)
        {
            const SectionHeading sequence_heading = DrawSectionHeading(draw, ImVec2(left, y), width, "Sequence",
                                                                       song->timeline ? "Timeline" : "Program", true);
            y = sequence_heading.bottom;
            if (sequence_heading.open)
            {
                y = DrawModeSwitch(ImVec2(left, y), width, *song, busy);
            }
            y += gap;
        }

        // The values of the shown sound in the archive's tables. The section starts closed, and its heading names the
        // sound.
        if (sound.view)
        {
            const SectionHeading sound_heading =
                DrawSectionHeading(draw, ImVec2(left, y), width, "Sound", sound.view->values.name, false);
            y = sound_heading.bottom;
            if (sound_heading.open)
            {
                y = DrawSoundValues(draw, ImVec2(left, y), width, sound);
            }
            y += gap;
        }

        // The channel strips of the track and the output fill the rest of the zone, and keep a height that shows their
        // controls.
        const float between = Px(6.0f);
        const float rest = std::floor(origin.y + avail.y - margin - y);
        const ImVec2 strip(std::floor((width - between) * 0.5f), std::max(rest, Px(kLeastStripHeight)));
        DrawInspectorStrips(song, master, track, left, y, width, strip, busy);

        // The strips and the margin below them set the height that scrolls.
        ImGui::SetCursorScreenPos(ImVec2(origin.x, y + strip.y));
        ImGui::Dummy(ImVec2(width, margin));
        ImGui::EndChild();
    }
    ImGui::End();
}

} // namespace hokuto::app
