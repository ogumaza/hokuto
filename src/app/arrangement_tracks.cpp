// SPDX-License-Identifier: MIT

// The arrangement's tracks: the headers in the track list, the parts on the canvas, and the marks of the PSG
// voices.

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "app/arrangement.h"
#include "app/arrangement_canvas.h"
#include "app/export_warnings.h"
#include "app/song_view.h"
#include "app/theme.h"
#include "app/transport.h"
#include "app/widgets.h"
#include "app/zone_parts.h"

namespace hokuto::app
{
namespace
{

using arrangement_canvas::Canvas;
using arrangement_canvas::XOf;
using theme::Px;
using zone_parts::kColourPopup;

// The width of a track's colour strip, and of the strip that a click on it takes while the tracks' colours are Custom.
constexpr float kColourStripWidth = 4.0f;
constexpr float kColourStripGrab = 10.0f;

// A track's bank and program in one line of text. The track's header shows that line.
std::string InstrumentText(const TrackView& track)
{
    const std::string program = track.program < 0 ? std::string() : "Program " + std::to_string(track.program);
    if (track.bank.empty() || program.empty())
    {
        return track.bank.empty() && program.empty() ? std::string("No instrument") : track.bank + program;
    }

    return track.bank + " · " + program;
}

// A track's bank and program, each on a line of the track's header.
std::vector<std::string> InstrumentLines(const TrackView& track)
{
    std::vector<std::string> lines;
    if (!track.bank.empty())
    {
        lines.push_back(track.bank);
    }
    if (track.program >= 0)
    {
        lines.push_back("Program " + std::to_string(track.program));
    }
    if (lines.empty())
    {
        lines.emplace_back("No instrument");
    }

    return lines;
}

// A square wave, the mark of the PSG voices, across `rect`.
void DrawPsgMark(ImDrawList* draw, const ImRect& rect, ImU32 colour)
{
    const float third = rect.GetWidth() / 3.0f;
    const std::array<ImVec2, 8> points = {
        ImVec2(rect.Min.x, rect.Max.y),         ImVec2(rect.Min.x, rect.Min.y),
        ImVec2(rect.Min.x + third, rect.Min.y), ImVec2(rect.Min.x + third, rect.Max.y),
        ImVec2(rect.Max.x - third, rect.Max.y), ImVec2(rect.Max.x - third, rect.Min.y),
        ImVec2(rect.Max.x, rect.Min.y),         ImVec2(rect.Max.x, rect.Max.y),
    };
    draw->AddPolyline(points.data(), static_cast<int>(points.size()), colour, Px(1.5f), ImDrawFlags_None);
}

// What the tooltip of a track's mark of the channels says: the PSG channels that its notes play, and the notes that the
// console's channels cut short or never start.
std::string PsgTooltip(const TrackView& track)
{
    static constexpr std::array<const char*, 4> kNames = {"Square 1", "Square 2", "Wave", "Noise"};
    static constexpr std::array<const char*, 8> kDsNames = {"Square 8",  "Square 9",  "Square 10", "Square 11",
                                                            "Square 12", "Square 13", "Noise 14",  "Noise 15"};
    const std::span<const char* const> names = track.ds ? std::span<const char* const>(kDsNames) : std::span(kNames);
    std::vector<std::string> channels;
    for (std::size_t c = 0; c < names.size(); c++)
    {
        if (track.psg_channels >> c & 1)
        {
            channels.emplace_back(names[c]);
        }
    }
    const std::string console = track.ds ? "DS" : "GBA";
    std::vector<std::string> sentences;
    if (!channels.empty())
    {
        sentences.push_back("This track uses these " + console + " PSG channels: " + NameList(channels) + ".");
    }
    if (track.cut_notes > 0)
    {
        sentences.push_back("Later notes cut " + std::to_string(track.cut_notes) +
                            (track.cut_notes == 1 ? " note" : " notes") + " of this track short.");
    }
    if (track.unstarted_notes > 0)
    {
        sentences.push_back("The " + console + " never plays " + std::to_string(track.unstarted_notes) +
                            (track.unstarted_notes == 1 ? " note" : " notes") + " of this track.");
    }
    std::string text;
    for (const std::string& sentence : sentences)
    {
        text += (text.empty() ? "" : " ") + sentence;
    }

    return text;
}

} // namespace

namespace arrangement_canvas
{

namespace
{

// The number, the PSG mark, the name, and the bank and program of `track`, row `i`, in `row`. `middle` is the middle
// of the row's first line, and `mute` the place of the M button.
void DrawTrackHeaderText(ImDrawList* draw, const Canvas& canvas, const TrackView& track, std::size_t i,
                         const ImRect& row, float middle, const ImRect& mute, bool hovered)
{
    const ImGuiIO& io = ImGui::GetIO();
    ImFont* text = theme::GetFonts().text;
    ImFont* figures = theme::GetFonts().figures;
    std::array<char, 16> number{};
    std::snprintf(number.data(), number.size(), "%zu", i + 1);
    const ImVec2 number_size = TextSize(figures, theme::kSmallSize, number.data());
    DrawText(draw, figures, theme::kSmallSize,
             ImVec2(row.Min.x + Px(22.0f) - number_size.x * 0.5f, middle - number_size.y * 0.5f), theme::kTextMuted,
             number.data());
    // A track that plays the console's PSG voices shows their mark before the M button: in the collision colour
    // where the console's channels cut its notes short or never start them. A track of other voices shows the mark
    // only where the channels do that.
    const float left = row.Min.x + Px(40.0f);
    const ImRect psg(ImVec2(mute.Min.x - Px(22.0f), middle - Px(4.0f)),
                     ImVec2(mute.Min.x - Px(8.0f), middle + Px(4.0f)));
    const bool collides = track.cut_notes > 0 || track.unstarted_notes > 0;
    const bool shows_psg = track.psg_channels != 0 || collides;
    const float name_right = shows_psg ? psg.Min.x - Px(6.0f) : mute.Min.x - Px(8.0f);
    if (shows_psg)
    {
        DrawPsgMark(draw, psg, collides ? theme::kCollision : theme::kTextMuted);
        const ImRect reach(ImVec2(psg.Min.x - Px(3.0f), psg.Min.y - Px(4.0f)),
                           ImVec2(psg.Max.x + Px(3.0f), psg.Max.y + Px(4.0f)));
        if (hovered && reach.Contains(io.MousePos))
        {
            WrappedTooltip(PsgTooltip(track));
        }
    }
    const float width = name_right - left;
    draw->PushClipRect(ImVec2(row.Min.x, row.Min.y), ImVec2(name_right, row.Max.y), true);
    if (canvas.row_height >= Px(kThreeLineHeight))
    {
        const std::vector<std::string> lines = InstrumentLines(track);
        const float name_top = std::floor(middle - Px(lines.size() > 1 ? 23.0f : 18.0f));
        DrawText(draw, text, theme::kTextSize, ImVec2(left, name_top), theme::kText,
                 FitText(text, theme::kTextSize, track.name, width));
        for (std::size_t line = 0; line < lines.size(); line++)
        {
            DrawText(draw, text, theme::kSmallSize,
                     ImVec2(left, name_top + Px(19.0f) + Px(15.0f) * static_cast<float>(line)), theme::kTextMuted,
                     FitText(text, theme::kSmallSize, lines[line], width));
        }
    }
    else if (canvas.row_height >= Px(kTwoLineHeight))
    {
        const float name_top = std::floor(middle - Px(18.0f));
        DrawText(draw, text, theme::kTextSize, ImVec2(left, name_top), theme::kText,
                 FitText(text, theme::kTextSize, track.name, width));
        DrawText(draw, text, theme::kSmallSize, ImVec2(left, name_top + Px(19.0f)), theme::kTextMuted,
                 FitText(text, theme::kSmallSize, InstrumentText(track), width));
    }
    else
    {
        const ImVec2 name_size = TextSize(text, theme::kTextSize, track.name);
        DrawText(draw, text, theme::kTextSize, ImVec2(left, middle - name_size.y * 0.5f), theme::kText,
                 FitText(text, theme::kTextSize, track.name, width));
    }
    draw->PopClipRect();
}

// The clicks on the header of `track`, row `i`: a press on the colour strip opens the palette, a click on M or S
// switches it, and a click or a right-click elsewhere selects the track.
void HandleTrackHeaderClick(TrackView& track, std::size_t i, ArrangementState& state, const ImRect& row,
                            bool strip_pressed, bool clicked, bool menu, bool over_mute, bool over_solo)
{
    const ImGuiIO& io = ImGui::GetIO();
    if (strip_pressed)
    {
        if (ImGui::GetActiveID() != 0)
        {
            ImGui::ClearActiveID();
        }
        state.colour_popup_track = static_cast<int>(i);
        ImGui::OpenPopup(kColourPopup);
    }
    else if (clicked && row.Contains(io.MousePos))
    {
        if (over_mute || over_solo)
        {
            bool& flag = over_mute ? track.mute : track.solo;
            flag = !flag;
        }
        else
        {
            state.selected_track = static_cast<int>(i);
        }
    }
    if (menu && row.Contains(io.MousePos))
    {
        state.selected_track = static_cast<int>(i);
    }
}

} // namespace

void DrawTrackHeaders(ImDrawList* draw, const Canvas& canvas, SongView& song, ArrangementState& state,
                      bool takes_clicks)
{
    const ImGuiIO& io = ImGui::GetIO();
    const bool hovered = takes_clicks && ImGui::IsWindowHovered() && canvas.headers.Contains(io.MousePos);
    const bool clicked = hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    const bool menu = hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right);
    for (std::size_t i = 0; i < song.tracks.size(); i++)
    {
        TrackView& track = song.tracks[i];
        const float top = RowTop(canvas, i);
        if (top + canvas.row_height <= canvas.headers.Min.y || top >= canvas.headers.Max.y)
        {
            continue;
        }

        // The row, lit in the accent's colour when it's selected. The track's colour runs down the left edge. While the
        // tracks' colours are Custom, a click on the strip offers the palette, and the strip widens under the mouse.
        const ImRect row(ImVec2(canvas.headers.Min.x, top), ImVec2(canvas.headers.Max.x, top + canvas.row_height));
        const bool selected = state.selected_track == static_cast<int>(i);
        const ImRect strip(row.Min, ImVec2(row.Min.x + Px(kColourStripGrab), row.Max.y));
        const bool over_strip = state.custom_colours && hovered && strip.Contains(io.MousePos);

        // The splitter between the arrangement and the zone on its left reaches a few points into the strip, and takes
        // a press there before the arrangement does. A press on the strip opens the palette all the same. ClearActiveID
        // first takes the press from the splitter.
        const bool strip_pressed = state.custom_colours && takes_clicks &&
                                   ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) &&
                                   canvas.headers.Contains(io.MousePos) && strip.Contains(io.MousePos) &&
                                   ImGui::IsMouseClicked(ImGuiMouseButton_Left);
        if (over_strip)
        {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            WrappedTooltip("Track colour");
        }
        draw->AddRectFilled(row.Min, row.Max,
                            selected ? theme::Mix(theme::kRaised, theme::kAccent, 0.2f) : theme::kRaised);
        const float strip_width = over_strip ? Px(kColourStripWidth + 2.0f) : Px(kColourStripWidth);
        draw->AddRectFilled(row.Min, ImVec2(row.Min.x + strip_width, row.Max.y), track.colour);
        draw->AddRectFilled(ImVec2(row.Min.x, row.Max.y - Px(1.0f)), row.Max, theme::kBorder);

        // The M and S buttons. A row taller than the default keeps its content at the top.
        const float middle = row.Min.y + std::min(canvas.row_height, Px(kDefaultTrackHeight)) * 0.5f;
        const ImVec2 button(Px(22.0f), Px(20.0f));
        const float button_top = std::floor(middle - button.y * 0.5f);
        const ImRect mute(ImVec2(row.Max.x - Px(62.0f), button_top),
                          ImVec2(row.Max.x - Px(62.0f) + button.x, button_top + button.y));
        const ImRect solo(ImVec2(mute.Max.x + Px(4.0f), button_top),
                          ImVec2(mute.Max.x + Px(4.0f) + button.x, button_top + button.y));
        const bool over_mute = hovered && mute.Contains(io.MousePos);
        const bool over_solo = hovered && solo.Contains(io.MousePos);
        DrawLetterButton(draw, mute.Min, mute.Max, "M", track.mute, theme::kMute, over_mute);
        DrawLetterButton(draw, solo.Min, solo.Max, "S", track.solo, theme::kSolo, over_solo);

        // The number, the name, and the bank and program under it. A tall row shows the bank and the program on lines
        // of their own, and a short row has room for the name alone. Text that doesn't fit ends in an ellipsis.
        DrawTrackHeaderText(draw, canvas, track, i, row, middle, mute, hovered);

        HandleTrackHeaderClick(track, i, state, row, strip_pressed, clicked, menu, over_mute, over_solo);
    }
    draw->AddRectFilled(ImVec2(canvas.headers.Max.x - Px(1.0f), canvas.headers.Min.y), canvas.headers.Max,
                        theme::kBorder);
    if (menu)
    {
        ImGui::OpenPopup(kTrackMenu);
    }
}

} // namespace arrangement_canvas

namespace
{

// The name of `part` in its strip, `strip` tall and in `strip_colour`, with the marks of a linked part and of the PSG.
void DrawPartName(ImDrawList* draw, const ImRect& rect, const PartView& part, const TrackView& track, float strip,
                  ImU32 strip_colour)
{
    ImFont* font = theme::GetFonts().text;
    const std::string& name = part.name.empty() ? track.name : part.name;
    const ImVec2 name_size = TextSize(font, theme::kSmallSize, name);
    const ImVec2 mark_size = TextSize(font, theme::kSmallSize, "=");
    const float mark_left = rect.Max.x - Px(6.0f) - mark_size.x;
    const bool linked = part.linked && rect.GetWidth() > mark_size.x * 4.0f;
    const float psg_right = linked ? mark_left - Px(4.0f) : rect.Max.x - Px(6.0f);
    const float psg_width = Px(10.0f);
    const bool psg = part.psg && psg_right - rect.Min.x > psg_width * 4.0f;
    const float name_right = psg ? psg_right - psg_width - Px(4.0f) : linked ? mark_left - Px(4.0f) : rect.Max.x;
    draw->PushClipRect(rect.Min, ImVec2(name_right, rect.Min.y + strip), true);
    const ImU32 text_colour = theme::TextOn(strip_colour);
    DrawText(draw, font, theme::kSmallSize, ImVec2(rect.Min.x + Px(6.0f), rect.Min.y + (strip - name_size.y) * 0.5f),
             text_colour, name);
    draw->PopClipRect();
    if (linked)
    {
        DrawText(draw, font, theme::kSmallSize, ImVec2(mark_left, rect.Min.y + (strip - mark_size.y) * 0.5f),
                 text_colour, "=");
    }
    if (psg)
    {
        const float middle = rect.Min.y + strip * 0.5f;
        DrawPsgMark(draw,
                    ImRect(ImVec2(psg_right - psg_width, middle - Px(3.0f)), ImVec2(psg_right, middle + Px(3.0f))),
                    text_colour);
    }
}

// The notes of `part` in miniature under its strip, `strip` tall, between the part's lowest and highest keys.
void DrawPartNotes(ImDrawList* draw, const Canvas& canvas, const ImRect& rect, const PartView& part, ImU32 colour,
                   bool silent, float strip)
{
    const float top = rect.Min.y + strip + Px(4.0f);
    const float bottom = rect.Max.y - Px(4.0f);
    if ((!part.notes.empty() || !part.unstarted.empty()) && bottom - top >= Px(6.0f))
    {
        int low = 127;
        int high = 0;
        for (const std::vector<NoteView>* notes : {&part.notes, &part.unstarted})
        {
            for (const NoteView& note : *notes)
            {
                low = std::min(low, note.key);
                high = std::max(high, note.key);
            }
        }
        const int span = std::max(high - low + 1, 12);
        const float step = (bottom - top) / static_cast<float>(span);
        const float height = std::clamp(step, Px(1.5f), Px(3.0f));
        const ImU32 note_colour = theme::Mix(colour, theme::kText, silent ? 0.25f : 0.6f);
        const float origin = XOf(canvas, static_cast<double>(part.start) / canvas.ticks_per_quarter);
        const float tick_width = canvas.beat_width / static_cast<float>(canvas.ticks_per_quarter);
        const auto rect_of = [&](const NoteView& note)
        {
            const float x = origin + static_cast<float>(note.start) * tick_width;
            const float width = std::max(static_cast<float>(note.length) * tick_width, Px(1.5f));
            const float y = bottom - static_cast<float>(note.key - low + 1) * step + (step - height) * 0.5f;
            return ImRect(ImVec2(x, y), ImVec2(x + width, y + height));
        };
        for (const NoteView& note : part.unstarted)
        {
            const ImRect note_rect = rect_of(note);
            draw->AddRect(note_rect.Min, note_rect.Max, theme::WithAlpha(theme::kCollision, 0.85f), Px(0.75f),
                          Px(1.0f));
        }
        for (const NoteView& note : part.notes)
        {
            const ImRect note_rect = rect_of(note);
            draw->AddRectFilled(note_rect.Min, note_rect.Max, note.cut ? theme::kCollision : note_colour, Px(0.75f));
        }
    }
}

// Draws a part: a body in the track's colour, a strip with the name across the top, and the notes in miniature. The
// notes fill the part's range of keys. A part too short for them shows the strip alone. A silent track's parts are
// dimmed. A part that plays the console's PSG voices shows their mark at the end of its strip, and its notes that a
// later note cut short are drawn in the collision colour.
void DrawPart(ImDrawList* draw, const Canvas& canvas, const ImRect& rect, const PartView& part, const TrackView& track,
              bool silent, bool selected)
{
    const ImU32 colour = silent ? theme::Mix(track.colour, theme::kPanel, 0.7f) : track.colour;
    const float rounding = Px(4.0f);
    const float strip = Px(15.0f);
    draw->AddRectFilled(rect.Min, rect.Max, theme::Mix(colour, theme::kWindow, 0.62f), rounding);
    const ImU32 strip_colour = theme::Mix(colour, theme::kWindow, 0.08f);
    draw->AddRectFilled(rect.Min, ImVec2(rect.Max.x, rect.Min.y + strip), strip_colour, rounding,
                        ImDrawFlags_RoundCornersTop);

    // The name: the subroutine's for a call's part, and the track's for another. A linked part shows an equals sign at
    // the end of its strip. A silent track's dimmed strip, or a dark colour, takes light text.
    draw->PushClipRect(rect.Min, rect.Max, true);
    DrawPartName(draw, rect, part, track, strip, strip_colour);

    // The notes, between the part's lowest and highest keys. On the GBA and the DS, a note that the driver never
    // started shows as an outline in the collision colour. The Key Editor shows such a note the same way.
    DrawPartNotes(draw, canvas, rect, part, colour, silent, strip);
    draw->PopClipRect();

    draw->AddRect(rect.Min, rect.Max, selected ? theme::kText : theme::Mix(colour, theme::kWindow, 0.25f), rounding,
                  selected ? Px(1.5f) : Px(1.0f));
}

} // namespace

namespace arrangement_canvas
{

namespace
{

// The lanes of the tracks, the grid and the cycle's range over them, and a line at the loop's start and at its end.
void DrawLaneBackground(ImDrawList* draw, const Canvas& canvas, const SongView& song, const Transport& transport)
{
    for (std::size_t i = 0; i < song.tracks.size(); i++)
    {
        const float top = RowTop(canvas, i);
        draw->AddRectFilled(ImVec2(canvas.lanes.Min.x, top), ImVec2(canvas.lanes.Max.x, top + canvas.row_height),
                            i % 2 == 0 ? theme::kPanel : theme::kPanelAlt);
        draw->AddRectFilled(ImVec2(canvas.lanes.Min.x, top + canvas.row_height - Px(1.0f)),
                            ImVec2(canvas.lanes.Max.x, top + canvas.row_height), theme::kBorder);
    }
    DrawGrid(draw, canvas);
    if (transport.Cycle())
    {
        draw->AddRectFilled(ImVec2(XOf(canvas, transport.CycleStart()), canvas.lanes.Min.y),
                            ImVec2(XOf(canvas, transport.CycleEnd()), canvas.lanes.Max.y),
                            theme::WithAlpha(theme::kCycle, 0.05f));
    }

    // A line down the lanes at the loop's start and at its end.
    if (song.loop_start >= 0)
    {
        for (const int32_t tick : {song.loop_start, song.loop_end})
        {
            const float x = std::floor(XOf(canvas, static_cast<double>(tick) / canvas.ticks_per_quarter));
            draw->AddRectFilled(ImVec2(x, canvas.lanes.Min.y), ImVec2(x + Px(1.0f), canvas.lanes.Max.y),
                                theme::WithAlpha(theme::kCycle, 0.45f));
        }
    }
}

// The parts of the tracks in view. A click on one selects it, and a double-click opens it in the Key Editor. Returns
// whether a click hit a part.
bool DrawTrackParts(ImDrawList* draw, const Canvas& canvas, const SongView& song, ArrangementState& state, bool clicked)
{
    const ImGuiIO& io = ImGui::GetIO();
    bool hit = false;
    for (std::size_t i = 0; i < song.tracks.size(); i++)
    {
        const TrackView& track = song.tracks[i];
        const float top = RowTop(canvas, i);
        if (top + canvas.row_height <= canvas.lanes.Min.y || top >= canvas.lanes.Max.y)
        {
            continue;
        }

        const bool silent = !IsHeard(song, i);
        for (std::size_t p = 0; p < track.parts.size(); p++)
        {
            const PartView& part = track.parts[p];
            const float left = XOf(canvas, static_cast<double>(part.start) / canvas.ticks_per_quarter);
            const float right = XOf(canvas, static_cast<double>(part.start + part.length) / canvas.ticks_per_quarter);
            if (right < canvas.lanes.Min.x || left > canvas.lanes.Max.x)
            {
                continue;
            }

            const ImRect rect(ImVec2(std::floor(left) + Px(1.0f), top + Px(3.0f)),
                              ImVec2(std::floor(right) - Px(1.0f), top + canvas.row_height - Px(4.0f)));
            const bool selected =
                state.selected_part_track == static_cast<int>(i) && state.selected_part == static_cast<int>(p);
            DrawPart(draw, canvas, rect, part, track, silent, selected);
            if (!part.name.empty() && rect.Contains(io.MousePos) && ImGui::IsWindowHovered() &&
                !ImGui::IsAnyMouseDown())
            {
                WrappedTooltip(part.linked ? "Linked part: subroutine " + part.name +
                                                 ". Other calls in this or other sounds play it too. Editing its "
                                                 "notes changes every call."
                                           : "Subroutine " + part.name + ". No other call plays it.");
            }
            if (clicked && rect.Contains(io.MousePos))
            {
                state.selected_part_track = static_cast<int>(i);
                state.selected_part = static_cast<int>(p);
                state.selected_track = static_cast<int>(i);
                state.open_editor = state.open_editor || ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
                hit = true;
            }
        }
    }

    return hit;
}

// Selects the track under the mouse after a click between the parts. A double-click opens the track in the Key Editor
// at the beat under the mouse.
void SelectTrackAt(const Canvas& canvas, const SongView& song, ArrangementState& state)
{
    const ImGuiIO& io = ImGui::GetIO();
    state.selected_part_track = -1;
    state.selected_part = -1;
    const auto row =
        static_cast<std::size_t>((io.MousePos.y - canvas.lanes.Min.y + canvas.scroll_y) / canvas.row_height);
    if (row < song.tracks.size())
    {
        state.selected_track = static_cast<int>(row);
    }
    if (row < song.tracks.size() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
    {
        state.open_editor = true;
        state.open_beat = std::max(QuartersAt(canvas, io.MousePos.x), 0.0);
    }
}

} // namespace

void DrawLanes(ImDrawList* draw, const Canvas& canvas, SongView& song, const Transport& transport,
               ArrangementState& state)
{
    // The lanes, then the grid and the cycle's range over them.
    DrawLaneBackground(draw, canvas, song, transport);

    // The parts. A click on one selects it, and a double-click opens it in the Key Editor. A click between them selects
    // the track, and a double-click there opens the track in the Key Editor at the beat under the mouse.
    const ImGuiIO& io = ImGui::GetIO();
    const bool clicked = ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
                         canvas.lanes.Contains(io.MousePos) && !state.resizing_track_list;
    const bool hit = DrawTrackParts(draw, canvas, song, state, clicked);

    if (clicked && !hit)
    {
        SelectTrackAt(canvas, song, state);
    }
}

} // namespace arrangement_canvas
} // namespace hokuto::app
