// SPDX-License-Identifier: MIT

// The drawing of the Key Editor's canvas: the rows, the grid, the ruler, the keyboard, the notes with their marks,
// the overview, the lanes of channels and of controllers, and the playhead.

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/commands/units.h"

#include "app/arrangement.h"
#include "app/key_canvas.h"
#include "app/key_editor.h"
#include "app/key_edits.h"
#include "app/lanes.h"
#include "app/list_edits.h"
#include "app/tempo_row.h"
#include "app/theme.h"
#include "app/time_text.h"
#include "app/transport.h"
#include "app/widgets.h"
#include "edit/commands.h"
#include "song/archive.h"

namespace hokuto::app
{
namespace
{

using key_canvas::Canvas;
using key_canvas::LaneBottom;
using key_canvas::LaneTop;
using key_canvas::NoteRect;
using ntdlib::commands::Source;
using theme::Px;

} // namespace

namespace key_canvas
{

int ChannelRows(const song::Console& console)
{
    return console.engine == format::Engine::kDs ? 8 : 4;
}

} // namespace key_canvas

namespace
{

// The name of PSG channel `channel` of `console` (song::Note::psg).
std::string PsgChannelName(const song::Console& console, int channel)
{
    if (console.engine == format::Engine::kDs)
    {
        return (channel <= 6 ? "Square " : "Noise ") + std::to_string(channel + 7);
    }

    switch (channel)
    {
    case 1:
        return "Square 1";
    case 2:
        return "Square 2";
    case 3:
        return "Wave";
    default:
        return "Noise";
    }
}

} // namespace

namespace key_canvas
{

std::string CutText(const song::Console& console, const KeyNote& note)
{
    if (console.engine == format::Engine::kDse)
    {
        return "Cut short: a later note steals the voice.";
    }
    if (console.engine == format::Engine::kDs)
    {
        return "Cut short: a later note steals the channel.";
    }

    return note.psg != 0 ? "Cut short: a later note steals the PSG channel (" + PsgChannelName(console, note.psg) + ")."
                         : "Cut short: a later note steals the DirectSound channel.";
}

std::string UnstartedText(const song::Console& console, const KeyNote& note)
{
    if (console.engine == format::Engine::kDse)
    {
        return "Never plays on the DS. Either no voice is free, or the program has no sound on this key.";
    }
    if (console.engine == format::Engine::kDs)
    {
        return "Never plays on the DS. Either every channel that the track can use is busy with a higher-priority "
               "note, or the program has no sound on this key.";
    }

    const std::string channel = note.psg != 0 ? PsgChannelName(console, note.psg) + " is held by another note"
                                              : "every DirectSound channel is held by another note";
    return "Never plays on the GBA. Either " + channel +
           ", a later note gets the channel first, or the voice is silent.";
}

std::string ChannelBarText(const song::Console& console, const ChannelBar& bar)
{
    std::string text = bar.track + ": " + KeyName(bar.key) + " on " + PsgChannelName(console, bar.channel);
    if (bar.cut)
    {
        text += "\nCut short: a later note steals the channel.";
    }
    else if (bar.unstarted)
    {
        text += console.engine == format::Engine::kDs ? "\nNever plays on the DS." : "\nNever plays on the GBA.";
    }

    return text;
}

void DrawRows(ImDrawList* draw, const Canvas& canvas)
{
    const int highest = KeyAt(canvas, canvas.notes.Min.y);
    const int lowest = KeyAt(canvas, canvas.notes.Max.y);
    for (int key = lowest; key <= highest; key++)
    {
        const float top = RowTop(canvas, key);
        draw->AddRectFilled(ImVec2(canvas.notes.Min.x, top), ImVec2(canvas.notes.Max.x, top + canvas.key_height),
                            IsBlack(key) ? theme::kPanel : theme::kPanelAlt);
        if (key % 12 == 0)
        {
            draw->AddRectFilled(ImVec2(canvas.notes.Min.x, top + canvas.key_height - Px(1.0f)),
                                ImVec2(canvas.notes.Max.x, top + canvas.key_height), theme::kBorderStrong);
        }
    }
}

void DrawGrid(ImDrawList* draw, const Canvas& canvas, const KeyEditorState& state, const ImRect& area)
{
    const double step = GridTicks(state, canvas.ticks_per_quarter);
    const bool steps = step / canvas.ticks_per_quarter * canvas.beat_width >= Px(8.0f);
    const double first = std::max(std::floor(TickAt(canvas, area.Min.x) / step), 0.0);
    const double last = std::ceil(TickAt(canvas, area.Max.x) / step);
    for (double line = first; line <= last; line++)
    {
        const double tick = line * step;
        const bool beat = std::fmod(tick, canvas.ticks_per_quarter) < 1e-6;
        if (!beat && !steps)
        {
            continue;
        }

        const float x = std::floor(XOf(canvas, tick));
        draw->AddRectFilled(ImVec2(x, area.Min.y), ImVec2(x + Px(1.0f), area.Max.y),
                            beat ? theme::kGridBeatOnly : theme::kGridBeat);
    }
}

void DrawRuler(ImDrawList* draw, const Canvas& canvas, const Transport& transport, ImU32 colour)
{
    const ImRect& ruler = canvas.ruler;
    const double quarter = canvas.ticks_per_quarter;
    draw->AddRectFilled(ruler.Min, ruler.Max, theme::kRaised);
    const ImU32 range =
        transport.Cycle() ? theme::WithAlpha(theme::kCycle, 0.9f) : theme::WithAlpha(theme::kTextDim, 0.4f);
    draw->AddRectFilled(ImVec2(XOf(canvas, transport.CycleStart() * quarter), ruler.Min.y + Px(3.0f)),
                        ImVec2(XOf(canvas, transport.CycleEnd() * quarter), ruler.Min.y + Px(11.0f)), range, Px(3.0f));
    draw->AddRectFilled(ImVec2(ruler.Min.x, ruler.Max.y - Px(3.0f)), ruler.Max, theme::WithAlpha(colour, 0.85f));

    // A number and a long mark at each labelled beat, and short marks at the others.
    const int step = BeatStep(canvas.beat_width);
    const int first = std::max(static_cast<int>(std::floor(TickAt(canvas, ruler.Min.x) / quarter)) - 1, 0);
    const int last = static_cast<int>(std::ceil(TickAt(canvas, ruler.Max.x) / quarter));
    ImFont* figures = theme::GetFonts().figures;
    for (int beat = first; beat <= last; beat++)
    {
        const float x = std::floor(XOf(canvas, beat * quarter));
        if (LabelledBeat(beat, step))
        {
            std::array<char, 16> label{};
            std::snprintf(label.data(), label.size(), "%d", beat + 1);
            draw->AddRectFilled(ImVec2(x, ruler.Max.y - Px(16.0f)), ImVec2(x + Px(1.0f), ruler.Max.y - Px(3.0f)),
                                theme::kTextDim);
            DrawText(draw, figures, theme::kSmallSize, ImVec2(x + Px(4.0f), ruler.Max.y - Px(19.0f)), theme::kTextMuted,
                     label.data());
        }
        else if (canvas.beat_width >= Px(4.0f))
        {
            draw->AddRectFilled(ImVec2(x, ruler.Max.y - Px(8.0f)), ImVec2(x + Px(1.0f), ruler.Max.y - Px(3.0f)),
                                theme::kBorderStrong);
        }
    }
}

void DrawKeyboard(ImDrawList* draw, const Canvas& canvas, const KeyTrack& track, const KeyEditorState& state)
{
    std::array<bool, kKeys> lit{};
    const auto light = [&lit](int key)
    {
        if (key >= 0 && key < kKeys)
        {
            lit[static_cast<std::size_t>(key)] = true;
        }
    };
    for (const KeyNote& note : track.notes)
    {
        if (state.selected.contains(note.command))
        {
            light(note.key);
        }
    }
    if (const std::optional<KeyNote> sounding = SoundingNote(track, state))
    {
        light(sounding->key);
    }

    // The white keys, each with a line along its bottom edge, then the black keys over them.
    const ImU32 kWhite = theme::Mix(theme::kText, theme::kPanel, 0.2f);
    const ImU32 kBlack = theme::kWindow;
    const ImU32 kEdge = theme::Mix(kWhite, kBlack, 0.45f);
    const ImRect& keys = canvas.keyboard;
    const float black_right = keys.Min.x + std::floor(keys.GetWidth() * 0.6f);
    const int highest = std::min(KeyAt(canvas, keys.Min.y) + 1, kKeys - 1);
    const int lowest = std::max(KeyAt(canvas, keys.Max.y) - 1, 0);

    const auto middle = [&canvas](int key)
    {
        return std::floor(RowTop(canvas, key) + canvas.key_height * 0.5f);
    };

    ImFont* text = theme::GetFonts().text;
    for (int key = lowest; key <= highest; key++)
    {
        if (IsBlack(key))
        {
            continue;
        }

        const float top = key + 1 < kKeys && IsBlack(key + 1) ? middle(key + 1) : RowTop(canvas, key);
        const float bottom = key > 0 && IsBlack(key - 1) ? middle(key - 1) : RowTop(canvas, key) + canvas.key_height;
        const ImU32 colour = lit[static_cast<std::size_t>(key)] ? track.colour : kWhite;
        draw->AddRectFilled(ImVec2(keys.Min.x, top), ImVec2(keys.Max.x, bottom), colour);
        draw->AddRectFilled(ImVec2(keys.Min.x, bottom - Px(1.0f)), ImVec2(keys.Max.x, bottom), kEdge);
        if (key % 12 == 0 && canvas.key_height >= Px(9.0f))
        {
            const std::string name = KeyName(key);
            const ImVec2 size = TextSize(text, theme::kSmallSize, name);
            DrawText(draw, text, theme::kSmallSize,
                     ImVec2(keys.Max.x - size.x - Px(7.0f), std::floor((top + bottom - size.y) * 0.5f)),
                     theme::TextOn(colour), name);
        }
    }
    for (int key = lowest; key <= highest; key++)
    {
        if (IsBlack(key))
        {
            const float top = RowTop(canvas, key);
            const ImU32 colour = lit[static_cast<std::size_t>(key)] ? track.colour : kBlack;
            draw->AddRectFilled(ImVec2(keys.Min.x, top), ImVec2(black_right, top + canvas.key_height), colour, Px(2.0f),
                                ImDrawFlags_RoundCornersRight);
        }
    }
    draw->AddRectFilled(ImVec2(keys.Max.x - Px(1.0f), keys.Min.y), keys.Max, theme::kBorder);
}

} // namespace key_canvas

namespace
{

// A note is a bar in the track's colour. A selected note turns dark, with an outline in the colour.
void DrawNote(ImDrawList* draw, const ImRect& rect, ImU32 colour, bool selected, bool ghost)
{
    if (ghost)
    {
        draw->AddRect(rect.Min, rect.Max, theme::WithAlpha(colour, 0.5f), Px(2.0f), Px(1.0f));
        return;
    }

    const float rounding = Px(2.0f);
    if (selected)
    {
        draw->AddRectFilled(rect.Min, rect.Max, theme::Mix(colour, theme::kWindow, 0.6f), rounding);
        draw->AddRect(rect.Min, rect.Max, colour, rounding, Px(1.5f));
    }
    else
    {
        draw->AddRectFilled(rect.Min, rect.Max, colour, rounding);
        draw->AddRect(rect.Min, rect.Max, theme::Mix(colour, theme::kWindow, 0.35f), rounding, Px(1.0f));
    }
}

// The colour of a note of `velocity` (1 to 127) while the notes take their velocities' colours: from blue for the
// softest through green and yellow to red for the loudest.
ImU32 VelocityColour(int velocity)
{
    // The stops, at velocities 1, 32, 64, 96, 112 and 127, in the tracks' colours.
    struct Stop
    {
        int velocity;
        ImU32 colour;
    };
    static constexpr std::array<Stop, 6> kStops = {{
        {1, theme::kTrackColours[0]},
        {32, theme::kTrackColours[1]},
        {64, theme::kTrackColours[2]},
        {96, theme::kTrackColours[4]},
        {112, theme::kTrackColours[5]},
        {127, theme::kTrackColours[6]},
    }};
    const int clamped = std::clamp(velocity, kStops.front().velocity, kStops.back().velocity);
    for (std::size_t i = 1; i < kStops.size(); i++)
    {
        if (clamped <= kStops[i].velocity)
        {
            const Stop& low = kStops[i - 1];
            const Stop& high = kStops[i];
            const float amount =
                static_cast<float>(clamped - low.velocity) / static_cast<float>(high.velocity - low.velocity);
            return theme::Mix(low.colour, high.colour, amount);
        }
    }

    return kStops.back().colour;
}

// The colour of a note of `velocity`: the track's, or the velocity's while the notes take their velocities' colours.
ImU32 NoteColour(const KeyTrack& track, const KeyEditorState& state, int velocity)
{
    return state.colours == NoteColours::kVelocity ? VelocityColour(velocity) : track.colour;
}

} // namespace

namespace key_canvas
{

double OverviewTicks(const KeyTrack& track)
{
    double end = track.song_length;
    for (const KeyNote& note : track.notes)
    {
        end = std::max(end, static_cast<double>(note.start) + note.length);
    }

    return end;
}

void DrawOverview(ImDrawList* draw, const Canvas& canvas, const KeyTrack& track, const KeyEditorState& state)
{
    const ImRect& strip = canvas.overview;
    draw->AddRectFilled(strip.Min, strip.Max, theme::kDisplay);
    draw->AddRectFilled(ImVec2(strip.Min.x, strip.Max.y - Px(1.0f)), strip.Max, theme::kBorder);
    const double total = OverviewTicks(track);
    if (total <= 0.0)
    {
        return;
    }

    const auto x_of = [&strip, total](double tick)
    {
        return strip.Min.x + static_cast<float>(tick / total) * strip.GetWidth();
    };

    const auto by_key = [](const KeyNote& a, const KeyNote& b)
    {
        return a.key < b.key;
    };
    const auto [lowest, highest] = std::minmax_element(track.notes.begin(), track.notes.end(), by_key);
    if (lowest != track.notes.end())
    {
        const int range = highest->key - lowest->key;
        const float top = strip.Min.y + Px(3.0f);
        const float span = strip.Max.y - Px(4.0f) - top;
        const float bar = std::clamp(span / static_cast<float>(range + 1), Px(1.0f), Px(3.0f));
        for (const KeyNote& note : track.notes)
        {
            const float place = range == 0 ? 0.5f : static_cast<float>(highest->key - note.key) / range;
            const float y = top + place * (span - bar);
            const float left = x_of(note.start);
            const float right = std::max(x_of(static_cast<double>(note.start) + note.length), left + Px(1.0f));
            const ImU32 colour =
                state.selected.contains(note.command) ? theme::kText : NoteColour(track, state, note.velocity);
            draw->AddRectFilled(ImVec2(left, y), ImVec2(right, y + bar), colour);
        }
    }

    // The frame round the part in view.
    const float from = std::clamp(x_of(TickAt(canvas, canvas.notes.Min.x)), strip.Min.x, strip.Max.x);
    const float to = std::clamp(x_of(TickAt(canvas, canvas.notes.Max.x)), from, strip.Max.x);
    const ImVec2 min(from, strip.Min.y + Px(1.0f));
    const ImVec2 max(std::max(to, from + Px(2.0f)), strip.Max.y - Px(2.0f));
    draw->AddRectFilled(min, max, theme::WithAlpha(theme::kText, 0.08f), Px(2.0f));
    draw->AddRect(min, max, theme::WithAlpha(theme::kText, 0.55f), Px(2.0f), Px(1.0f));
}

} // namespace key_canvas

namespace
{

// A note that a later note cut short ends in a bar of the collision colour, set off from the note by a dark line.
void DrawCutMark(ImDrawList* draw, const ImRect& rect)
{
    const float width = std::min(Px(3.0f), rect.GetWidth() * 0.5f);
    draw->AddRectFilled(ImVec2(rect.Max.x - width - Px(1.0f), rect.Min.y), rect.Max, theme::kWindow, Px(2.0f),
                        ImDrawFlags_RoundCornersRight);
    draw->AddRectFilled(ImVec2(rect.Max.x - width, rect.Min.y), rect.Max, theme::kCollision, Px(2.0f),
                        ImDrawFlags_RoundCornersRight);
}

// A note that the driver never started shows as an outline in the collision colour, with the length of its command.
void DrawUnstarted(ImDrawList* draw, const Canvas& canvas, const KeyTrack& track)
{
    for (const KeyNote& note : track.unstarted)
    {
        const ImRect rect = NoteRect(canvas, note, 0, 0, 0);
        if (rect.Max.x < canvas.notes.Min.x || rect.Min.x > canvas.notes.Max.x || rect.Max.y < canvas.notes.Min.y ||
            rect.Min.y > canvas.notes.Max.y)
        {
            continue;
        }

        draw->AddRectFilled(rect.Min, rect.Max, theme::WithAlpha(theme::kCollision, 0.12f), Px(2.0f));
        draw->AddRect(rect.Min, rect.Max, theme::WithAlpha(theme::kCollision, 0.85f), Px(2.0f), Px(1.0f));
    }
}

} // namespace

namespace key_canvas
{

void DrawNotes(ImDrawList* draw, const Canvas& canvas, const KeyTrack& track, const KeyEditorState& state)
{
    DrawUnstarted(draw, canvas, track);
    const KeyGesture& gesture = state.gesture;
    const bool moving = gesture.kind == KeyGesture::Kind::kMove;
    const bool sizing = gesture.kind == KeyGesture::Kind::kSize && gesture.resize != 0;
    const std::optional<std::map<uint32_t, uint32_t>> lengths =
        sizing ? BaseLengths(track, state) : std::optional<std::map<uint32_t, uint32_t>>();
    for (const KeyNote& note : track.notes)
    {
        const bool selected = state.selected.contains(note.command);
        const int32_t shift = selected && moving ? gesture.shift : 0;
        const int transpose = selected && moving ? gesture.transpose : 0;
        int32_t resize = 0;
        if (selected && lengths)
        {
            const auto base = lengths->find(note.command);
            const int64_t length = base != lengths->end() ? int64_t{base->second} : int64_t{note.length};
            resize = static_cast<int32_t>(std::max<int64_t>(length + gesture.resize, 1) - note.length);
        }
        const ImRect rect = NoteRect(canvas, note, shift, transpose, resize);
        if (rect.Max.x < canvas.notes.Min.x || rect.Min.x > canvas.notes.Max.x || rect.Max.y < canvas.notes.Min.y ||
            rect.Min.y > canvas.notes.Max.y)
        {
            continue;
        }

        const ImU32 colour = NoteColour(track, state, note.velocity);
        if (selected && moving && (shift != 0 || transpose != 0))
        {
            DrawNote(draw, NoteRect(canvas, note, 0, 0, 0), colour, false, true);
        }
        DrawNote(draw, rect, colour, selected, false);
        if (note.cut && resize == 0)
        {
            DrawCutMark(draw, rect);
        }
    }

    if (gesture.kind == KeyGesture::Kind::kDraw)
    {
        DrawNote(draw, NoteRect(canvas, gesture.drawn, 0, 0, 0), NoteColour(track, state, gesture.drawn.velocity), true,
                 false);
    }
}

void DrawLane(ImDrawList* draw, const Canvas& canvas, const KeyTrack& track, const KeyEditorState& state)
{
    draw->AddRectFilled(canvas.lane.Min, canvas.lane.Max, theme::kPanel);
    DrawGrid(draw, canvas, state, canvas.lane);
    draw->AddRectFilled(canvas.lane.Min, ImVec2(canvas.lane.Max.x, canvas.lane.Min.y + Px(1.0f)), theme::kBorderStrong);

    // The gesture's change of velocity: the pressed note's new value, and the same change to the other selected notes.
    const KeyGesture& gesture = state.gesture;
    const bool changing = gesture.kind == KeyGesture::Kind::kVelocity;
    const int pressed = changing ? track.notes[gesture.note].velocity : 0;
    const bool together = changing && state.selected.contains(track.notes[gesture.note].command);
    const float top = LaneTop(canvas.lane);
    const float bottom = LaneBottom(canvas.lane);
    for (const KeyNote& note : track.notes)
    {
        const float x = std::floor(XOf(canvas, note.start));
        if (x < canvas.lane.Min.x - Px(4.0f) || x > canvas.lane.Max.x)
        {
            continue;
        }

        const bool selected = state.selected.contains(note.command);
        int velocity = note.velocity;
        if (changing && (note.command == track.notes[gesture.note].command || (together && selected)))
        {
            velocity = std::clamp(note.velocity + gesture.velocity - pressed, 1, kTopVelocity);
        }
        const float y = bottom - (bottom - top) * static_cast<float>(velocity) / kTopVelocity;
        const ImU32 base = NoteColour(track, state, velocity);
        const ImU32 colour = selected ? theme::Mix(base, theme::kText, 0.45f) : base;
        draw->AddRectFilled(ImVec2(x, y), ImVec2(x + Px(2.0f), bottom), colour);
        draw->AddRectFilled(ImVec2(x - Px(2.0f), y - Px(1.0f)), ImVec2(x + Px(4.0f), y + Px(1.0f)), colour);
    }
}

void DrawChannelLane(ImDrawList* draw, const Canvas& canvas, const KeyTrack& track, const KeyEditorState& state)
{
    const ImRect& rect = canvas.channels;
    draw->AddRectFilled(rect.Min, rect.Max, theme::kPanel);
    DrawGrid(draw, canvas, state, rect);
    draw->AddRectFilled(rect.Min, ImVec2(rect.Max.x, rect.Min.y + Px(1.0f)), theme::kBorderStrong);
    const float row = ChannelRowHeight();
    for (int channel = 1; channel < ChannelRows(track.console); channel++)
    {
        const float y = std::floor(rect.Min.y + Px(1.0f) + row * static_cast<float>(channel));
        draw->AddRectFilled(ImVec2(rect.Min.x, y), ImVec2(rect.Max.x, y + Px(1.0f)), theme::kGridBar);
    }

    for (const ChannelBar& bar : track.channels)
    {
        const ImRect box = ChannelBarRect(canvas, bar);
        if (box.Max.x < rect.Min.x || box.Min.x > rect.Max.x)
        {
            continue;
        }

        if (bar.unstarted)
        {
            draw->AddRect(box.Min, box.Max, theme::WithAlpha(theme::kCollision, 0.85f), Px(2.0f), Px(1.0f));
            continue;
        }

        const ImU32 colour = bar.shown ? bar.colour : theme::Mix(bar.colour, theme::kPanel, 0.55f);
        draw->AddRectFilled(box.Min, box.Max, colour, Px(2.0f));
        if (bar.cut)
        {
            DrawCutMark(draw, box);
        }
    }
}

void DrawChannelLabel(ImDrawList* draw, const Canvas& canvas, const song::Console& console)
{
    const ImRect& label = canvas.channels_label;
    draw->AddRectFilled(label.Min, label.Max, theme::kRaised);
    draw->AddRectFilled(label.Min, ImVec2(label.Max.x, label.Min.y + Px(1.0f)), theme::kBorderStrong);
    const float row = ChannelRowHeight();
    for (int channel = 1; channel <= ChannelRows(console); channel++)
    {
        const std::string name = PsgChannelName(console, channel);
        const float top = label.Min.y + Px(1.0f) + row * static_cast<float>(channel - 1);
        DrawCaps(draw, ImVec2(label.Min.x + Px(10.0f), top + (row - CapsSize(name).y) * 0.5f), theme::kTextMuted, name);
    }
}

void DrawLaneLabel(ImDrawList* draw, const Canvas& canvas)
{
    const ImRect& label = canvas.lane_label;
    draw->AddRectFilled(label.Min, label.Max, theme::kRaised);
    draw->AddRectFilled(label.Min, ImVec2(label.Max.x, label.Min.y + Px(1.0f)), theme::kBorderStrong);
    DrawCaps(draw, ImVec2(label.Min.x + Px(10.0f), label.Min.y + Px(10.0f)), theme::kTextMuted, "Velocity");
}

std::string PointText(const KeyTrack& track, const KeyControl& control)
{
    const ntdlib::commands::Command& command = control.command;
    const double quarter = std::max(track.ticks_per_quarter, 1);
    std::string text = LaneName(SettingOf(command).value_or(LaneSetting{command.kind})) + " " +
                       ArgumentText(track.console, command.kind, command.value) + "\nStart " +
                       WithoutFigureSpaces(PositionText(control.tick / quarter, kBeatsPerBar));
    if (command.time && command.time->source == Source::kValue)
    {
        text += "\nRamp time " +
                LengthText(static_cast<double>(command.time->value) / ntdlib::commands::kOne / quarter, kBeatsPerBar);
    }
    if (command.conditional)
    {
        text += "\nRuns only if the last comparison was true";
    }

    return text;
}

} // namespace key_canvas

namespace
{

// The commands of the lanes' settings as the gesture under way leaves them, in the order of their ticks. A drag of a
// point shows the ticks and the values that its edit gives the points (LanePointEdit).
std::vector<KeyControl> ShownControls(const KeyTrack& track, const KeyEditorState& state)
{
    std::vector<KeyControl> controls = track.controls;
    const KeyGesture& gesture = state.gesture;
    if (gesture.kind != KeyGesture::Kind::kControl || gesture.control >= track.controls.size())
    {
        return controls;
    }

    const std::optional<KeyEdit> edit = LanePointEdit(track, state, gesture.control, gesture.shift, gesture.value);
    if (!edit)
    {
        return controls;
    }

    for (KeyControl& control : controls)
    {
        const auto changed = edit->changed_controls.find(control.offset);
        if (changed != edit->changed_controls.end())
        {
            control.command = changed->second;
        }
        const auto moved = edit->moved_controls.find(control.offset);
        if (moved != edit->moved_controls.end())
        {
            control.tick = static_cast<uint32_t>(int64_t{control.tick} + moved->second);
        }
    }
    std::ranges::stable_sort(controls, {}, &KeyControl::tick);

    return controls;
}

} // namespace

namespace key_canvas
{

namespace
{

// The line of a lane in `rect` from `line`, with the lead-in before the `first` point fainter.
void DrawLaneLine(ImDrawList* draw, const Canvas& canvas, const ImRect& rect, const edit::ValueRange& range,
                  const std::vector<LanePiece>& line, std::optional<uint32_t> first, ImU32 colour)
{
    const float bottom = LaneBottom(rect);
    for (std::size_t i = 0; i < line.size(); i++)
    {
        const LanePiece& piece = line[i];
        const float left = std::floor(XOf(canvas, piece.start));
        const float right = std::floor(XOf(canvas, piece.end));
        if (right < rect.Min.x || left > rect.Max.x)
        {
            continue;
        }

        if (!piece.from || !piece.to)
        {
            draw->AddRectFilled(ImVec2(left, LaneTop(rect)), ImVec2(right, bottom), theme::WithAlpha(colour, 0.1f));
            continue;
        }

        const bool lead_in = !first || piece.end <= *first;
        const float from = ValueY(rect, range, *piece.from);
        const float to = ValueY(rect, range, *piece.to);
        draw->AddQuadFilled(ImVec2(left, from), ImVec2(right, to), ImVec2(right, bottom), ImVec2(left, bottom),
                            theme::WithAlpha(colour, lead_in ? 0.06f : 0.16f));
        const ImU32 stroke = theme::WithAlpha(colour, lead_in ? 0.45f : 1.0f);
        draw->AddLine(ImVec2(left, from), ImVec2(right, to), stroke, Px(1.5f));
        if (i > 0 && line[i - 1].to && line[i - 1].end == piece.start)
        {
            draw->AddLine(ImVec2(left, ValueY(rect, range, *line[i - 1].to)), ImVec2(left, from), stroke, Px(1.5f));
        }
    }
}

// The points of a lane in `rect`, with the source of each value that has no one height where it fits.
void DrawLanePoints(ImDrawList* draw, const Canvas& canvas, const ImRect& rect, const edit::ValueRange& range,
                    const KeyTrack& track, const KeyEditorState& state, const KeyTrack& shown, const Lane& shape,
                    ImU32 colour)
{
    ImFont* font = theme::GetFonts().text;
    std::vector<float> xs;
    std::vector<float> widths;
    std::vector<std::string> labels;
    for (const LanePoint& point : shape.points)
    {
        const KeyControl& control = shown.controls[point.control];
        xs.push_back(std::floor(XOf(canvas, point.tick)));
        const bool shown_point = xs.back() >= rect.Min.x - Px(8.0f) && xs.back() <= rect.Max.x + Px(8.0f);
        labels.push_back(shown_point && control.command.value.source != Source::kValue
                             ? ArgumentText(track.console, control.command.kind, control.command.value)
                             : std::string());
        widths.push_back(labels.back().empty() ? 0.0f : TextSize(font, theme::kSmallSize, labels.back()).x);
    }
    const std::vector<bool> fits = FittingLabels(xs, widths, Px(6.0f));
    for (std::size_t i = 0; i < shape.points.size(); i++)
    {
        const KeyControl& control = shown.controls[shape.points[i].control];
        const float x = xs[i];
        if (x < rect.Min.x - Px(8.0f) || x > rect.Max.x + Px(8.0f))
        {
            continue;
        }

        const auto [top, low] = PointHeights(rect, range, control);
        DrawPoint(draw, x, top, low, colour, state.selected_controls.contains(control.offset),
                  control.command.conditional);
        if (!labels[i].empty() && fits[i])
        {
            DrawText(draw, font, theme::kSmallSize, ImVec2(x + Px(6.0f), LaneTop(rect) - Px(2.0f)), theme::kTextMuted,
                     labels[i]);
        }
    }
}

} // namespace

void DrawControlLane(ImDrawList* draw, const Canvas& canvas, const KeyTrack& track, const KeyEditorState& state,
                     std::size_t lane)
{
    const ImRect& rect = canvas.lanes[lane];
    const LaneSetting setting = state.lanes[lane];
    draw->AddRectFilled(rect.Min, rect.Max, theme::kPanel);
    DrawGrid(draw, canvas, state, rect);
    draw->AddRectFilled(rect.Min, ImVec2(rect.Max.x, rect.Min.y + Px(1.0f)), theme::kBorderStrong);

    // A lane kept from a sound of another console can show a setting that this console's player skips.
    if (!Holds(track.console, setting))
    {
        DrawText(draw, theme::GetFonts().text, theme::kSmallSize,
                 ImVec2(rect.Min.x + Px(12.0f), LaneTop(rect) + Px(4.0f)), theme::kTextMuted,
                 "This console ignores this setting. Choose another one from the lane menu.");
        return;
    }

    // The line keeps the lane's range during a drag.
    const edit::ValueRange range = LaneRange(track, setting);
    KeyTrack shown;
    shown.console = track.console;
    shown.number = track.number;
    shown.song_length = track.song_length;
    shown.controls = ShownControls(track, state);
    const Lane shape = LaneOf(shown, setting);
    const std::vector<LanePiece>& line = shape.line;
    std::optional<uint32_t> first;
    if (!shape.points.empty())
    {
        first = shown.controls[shape.points.front().control].tick;
    }
    const ImU32 colour = track.colour;
    DrawLaneLine(draw, canvas, rect, range, line, first, colour);

    // The points, with the source of each value that has no one height where it fits (FittingLabels).
    DrawLanePoints(draw, canvas, rect, range, track, state, shown, shape, colour);

    // A point being drawn.
    const KeyGesture& gesture = state.gesture;
    if (gesture.kind == KeyGesture::Kind::kLaneDraw && gesture.lane == lane)
    {
        const float x = std::floor(XOf(canvas, gesture.tick));
        const float y = ValueY(rect, range, gesture.value);
        DrawPoint(draw, x, y, y, theme::kAccent, false, false);
    }
}

void DrawControlLabel(ImDrawList* draw, const Canvas& canvas, const KeyTrack& track, const KeyEditorState& state,
                      std::size_t lane, bool hovered)
{
    const ImRect& label = canvas.lane_labels[lane];
    const LaneSetting setting = state.lanes[lane];
    draw->AddRectFilled(label.Min, label.Max, hovered ? theme::kRaisedHover : theme::kRaised);
    draw->AddRectFilled(label.Min, ImVec2(label.Max.x, label.Min.y + Px(1.0f)), theme::kBorderStrong);

    // The name, cut short to fit beside the chevron. The tooltip gives a name that's cut short.
    const float chevron = Px(12.0f);
    const float width = label.GetWidth() - Px(10.0f) - chevron - Px(4.0f);
    const std::string full = LaneName(setting);
    const std::string name = FitCaps(full, width);
    DrawCaps(draw, ImVec2(label.Min.x + Px(10.0f), label.Min.y + Px(10.0f)), theme::kTextMuted, name);
    if (hovered && CapsSize(full).x > width)
    {
        WrappedTooltip(full);
    }
    DrawIcon(draw, Icon::kChevronDown, ImVec2(label.Max.x - Px(6.0f) - chevron * 0.5f, label.Min.y + Px(16.0f)),
             chevron, theme::kTextDim);

    // The range, when the lane has room for it.
    const edit::ValueRange range = LaneRange(track, setting);
    ImFont* figures = theme::GetFonts().figures;
    const std::string top = ValueText(track.console, setting.kind, range.highest);
    const std::string low = ValueText(track.console, setting.kind, range.lowest);
    const ImVec2 low_size = TextSize(figures, theme::kSmallSize, low);
    if (label.GetHeight() >= Px(60.0f))
    {
        const ImVec2 top_size = TextSize(figures, theme::kSmallSize, top);
        DrawText(draw, figures, theme::kSmallSize,
                 ImVec2(label.Max.x - Px(6.0f) - top_size.x, LaneTop(label) + Px(14.0f)), theme::kTextMuted, top);
        DrawText(draw, figures, theme::kSmallSize,
                 ImVec2(label.Max.x - Px(6.0f) - low_size.x, LaneBottom(label) - low_size.y), theme::kTextMuted, low);
    }
}

void DrawAddLane(ImDrawList* draw, const Canvas& canvas, bool hovered)
{
    if (hovered)
    {
        draw->AddRectFilled(canvas.add.Min, canvas.add.Max, theme::kControlHover, Px(3.0f));
    }
    DrawIcon(draw, Icon::kAdd, canvas.add.GetCenter(), canvas.add.GetWidth() * 0.7f,
             hovered ? theme::kText : theme::kTextMuted);
}

void DrawPlayhead(ImDrawList* draw, const Canvas& canvas, const Transport& transport)
{
    const float x = std::floor(XOf(canvas, transport.Position() * canvas.ticks_per_quarter));
    if (x < canvas.notes.Min.x || x > canvas.notes.Max.x)
    {
        return;
    }

    draw->AddRectFilled(ImVec2(x, canvas.ruler.Max.y - Px(2.0f)), ImVec2(x + Px(1.0f), LanesBottom(canvas)),
                        theme::WithAlpha(theme::kText, 0.85f));
    draw->AddTriangleFilled(ImVec2(x - Px(5.5f), canvas.ruler.Max.y - Px(11.0f)),
                            ImVec2(x + Px(6.5f), canvas.ruler.Max.y - Px(11.0f)),
                            ImVec2(x + Px(0.5f), canvas.ruler.Max.y - Px(2.0f)), theme::kText);
}

} // namespace key_canvas
} // namespace hokuto::app
