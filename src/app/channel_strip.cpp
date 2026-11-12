// SPDX-License-Identifier: MIT

// The channel strips of the inspector and the mixer: the sends, the pan, M and S, the fader with its meters, and
// the name.

#include "app/channel_strip.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <string>
#include <string_view>

#include "app/mixer.h"
#include "app/song_view.h"
#include "app/theme.h"
#include "app/widgets.h"

namespace hokuto::app
{
namespace
{

using theme::Px;

// Follows the user's hold of the control that the last item drew, in `track`'s view. The control showed `before` and
// shows `after`. A hold that ends with a new value becomes a move.
void FollowHold(TrackView* track, MixControl control, float before, float after)
{
    if (track == nullptr)
    {
        return;
    }

    if (ImGui::IsItemActivated())
    {
        track->held = MixMove{control, before, before};
    }
    if (ImGui::IsItemDeactivated() && track->held && track->held->control == control)
    {
        if (after != track->held->from)
        {
            track->moved = MixMove{control, track->held->from, after};
        }
        else
        {
            track->held.reset();
        }
    }
}

// Starts and ends the controls of a strip that are disabled while the program is busy. The controls keep their look.
void BeginHeldOff(bool busy)
{
    ImGui::PushStyleVar(ImGuiStyleVar_DisabledAlpha, 1.0f);
    ImGui::BeginDisabled(busy);
}

void EndHeldOff()
{
    ImGui::EndDisabled();
    ImGui::PopStyleVar();
}

// Draws the decibels of the faders and the meters, right-aligned at `right`. `top` is the height of 6 dB, and `bottom`
// the height of silence. A mark above `highest`, the level where the fader stops, is left out. So is a mark that would
// crowd the one above it, or the mark for silence at the bottom.
void DrawDecibelScale(ImDrawList* draw, float right, float top, float bottom, float highest)
{
    constexpr std::array<float, 9> kMarks = {6.0f, 0.0f, -6.0f, -12.0f, -18.0f, -24.0f, -36.0f, -48.0f, kSilence};
    ImFont* figures = theme::GetFonts().figures;
    const float silence_top = std::floor(bottom - TextSize(figures, theme::kCapsSize, "∞").y * 0.5f);
    float free_from = top - Px(20.0f);
    for (const float mark : kMarks)
    {
        const std::string text = mark == kSilence ? std::string("∞") : std::to_string(static_cast<int>(mark));
        const ImVec2 size = TextSize(figures, theme::kCapsSize, text);
        const float text_top = std::floor(top + (bottom - top) * (1.0f - FaderPosition(mark)) - size.y * 0.5f);
        if (mark > highest || text_top < free_from || (mark != kSilence && text_top + size.y > silence_top))
        {
            continue;
        }

        DrawText(draw, figures, theme::kCapsSize, ImVec2(right - size.x, text_top), theme::kTextMuted, text);
        free_from = text_top + size.y;
    }
}

} // namespace

namespace channel_strip
{

void DrawMuteSolo(ImDrawList* draw, ImVec2 pos, ImVec2 button, float gap, bool* mute, bool* solo)
{
    const ImVec2 solo_pos(pos.x + button.x + gap, pos.y);
    ImGui::SetCursorScreenPos(pos);
    if (ImGui::InvisibleButton("##mute", button))
    {
        *mute = !*mute;
    }
    DrawLetterButton(draw, pos, ImVec2(pos.x + button.x, pos.y + button.y), "M", *mute, theme::kMute,
                     ImGui::IsItemHovered());
    ImGui::SetCursorScreenPos(solo_pos);
    if (ImGui::InvisibleButton("##solo", button))
    {
        *solo = !*solo;
    }
    DrawLetterButton(draw, solo_pos, ImVec2(solo_pos.x + button.x, solo_pos.y + button.y), "S", *solo, theme::kSolo,
                     ImGui::IsItemHovered());
}

} // namespace channel_strip

namespace
{

// Draws a value in a dark box.
void DrawValueBox(ImDrawList* draw, ImVec2 min, ImVec2 max, const std::string& value, ImU32 colour)
{
    draw->AddRectFilled(min, max, theme::kDisplay, Px(4.0f));
    ImFont* figures = theme::GetFonts().figures;
    const ImVec2 size = TextSize(figures, theme::kSmallSize, value);
    draw->PushClipRect(min, max, true);
    DrawText(draw, figures, theme::kSmallSize, ImVec2((min.x + max.x - size.x) * 0.5f, (min.y + max.y - size.y) * 0.5f),
             colour, value);
    draw->PopClipRect();
}

// The tooltip of control `control` of `track`'s strip: the control's name, and what else an edit of it changes. Without
// a track, or where an edit changes the track alone, the name stands alone.
std::string ControlTooltip(const TrackView* track, MixControl control)
{
    constexpr std::array<const char*, kMixControlCount> kNames = {"Volume", "Pan", "Send to FX A", "Send to FX B",
                                                                  "Send to FX C"};
    const auto index = static_cast<std::size_t>(control);
    const std::string name = kNames[index];
    if (track == nullptr || track->mix_reach[index].empty())
    {
        return name;
    }

    return name + ". An edit also changes " + track->mix_reach[index] + ".";
}

// The tooltip of the equals sign on the name of `track`'s strip: what else an edit of each of its controls changes.
// Empty where every control's edit changes the track alone.
std::string LinkedMixTooltip(const TrackView& track)
{
    std::string lines;
    for (std::size_t control = 0; control < kMixControlCount; control++)
    {
        if (!track.mix_reach[control].empty())
        {
            const std::string name = ControlTooltip(nullptr, static_cast<MixControl>(control));
            lines += "\n" + name + ": " + track.mix_reach[control] + ".";
        }
    }
    if (lines.empty())
    {
        return lines;
    }

    const std::string heading =
        "Linked mix. Other tracks or sounds share some commands with this track. Editing a control also changes:";
    return heading + lines;
}

} // namespace

namespace channel_strip
{

void DrawLevels(ImDrawList* draw, float centre, float y, float level, float* peak)
{
    const float shown = peak != nullptr ? *peak : kSilence;
    const ImVec2 box(Px(36.0f), Px(19.0f));
    const ImVec2 level_min(centre - box.x - Px(1.0f), y);
    const ImVec2 peak_min(centre + Px(1.0f), y);
    DrawValueBox(draw, level_min, ImVec2(level_min.x + box.x, y + box.y), LevelText(level), theme::kText);
    DrawValueBox(draw, peak_min, ImVec2(peak_min.x + box.x, y + box.y), LevelText(shown),
                 shown > 0.0f ? theme::kMeterHigh : theme::kTextMuted);
    if (peak != nullptr)
    {
        ImGui::SetCursorScreenPos(peak_min);
        if (ImGui::InvisibleButton("##peak", box))
        {
            *peak = kSilence;
        }
        ItemTooltip("Peak. Click to clear it.");
    }
}

void DrawFaderMeters(ImDrawList* draw, float centre, float y, float height, const std::array<float, 2>& meter,
                     float highest)
{
    const float scale_top = y + Px(12.0f);
    const float scale_bottom = y + height - Px(12.0f);
    const float meter_x = centre + Px(23.0f);
    DrawMeter(draw, ImVec2(meter_x, scale_top), ImVec2(meter_x + Px(4.0f), scale_bottom), meter[0]);
    DrawMeter(draw, ImVec2(meter_x + Px(6.0f), scale_top), ImVec2(meter_x + Px(10.0f), scale_bottom), meter[1]);
    DrawDecibelScale(draw, meter_x - Px(4.0f), scale_top, scale_bottom, highest);
}

namespace
{

// The slots of the sends of `strip` from `y`, in the strip at `pos`, `size` large.
void DrawStripSends(ImDrawList* draw, const Strip& strip, ImVec2 pos, ImVec2 size, float y, ImFont* text)
{
    if (strip.sends != nullptr && (strip.buses > 0 || strip.sends_missing[0] != '\0'))
    {
        BeginHeldOff(strip.busy);
        for (std::size_t bus = 0; bus < kSendCount; bus++)
        {
            const std::string name = FxName(bus);
            const auto control = static_cast<MixControl>(static_cast<int>(MixControl::kSendA) + static_cast<int>(bus));
            float& level = (*strip.sends)[bus];
            const float before = level;
            const ImVec2 slot(pos.x + Px(6.0f), y + static_cast<float>(bus) * Px(21.0f));
            const ImVec2 slot_size(size.x - Px(12.0f), Px(18.0f));
            ImGui::PushID(static_cast<int>(bus));
            ImGui::SetCursorScreenPos(slot);
            if (bus < strip.buses)
            {
                SendSlider("##send", name.c_str(), &level, slot_size);
                FollowHold(strip.track, control, before, level);
                ItemTooltip(ControlTooltip(strip.track, control));
            }
            else
            {
                // A send that the console's tracks lack: an empty slot with the bus's name.
                ImGui::InvisibleButton("##send", slot_size);
                draw->AddRect(slot, ImVec2(slot.x + slot_size.x, slot.y + slot_size.y), theme::kBorder, Px(4.0f),
                              Px(1.0f));
                const ImVec2 name_size = TextSize(text, theme::kSmallSize, name);
                DrawText(draw, text, theme::kSmallSize,
                         ImVec2(slot.x + Px(6.0f), std::floor(slot.y + (slot_size.y - name_size.y) * 0.5f)),
                         theme::kTextMuted, name);
                ItemTooltip(strip.sends_missing);
            }
            ImGui::PopID();
        }
        EndHeldOff();
    }
}

// The pan knob of `strip` and its value at `y`, or the output's label, about `centre`.
void DrawStripPan(ImDrawList* draw, const Strip& strip, float centre, float y)
{
    const float radius = Px(13.0f);
    if (strip.pan != nullptr)
    {
        const float before = *strip.pan;
        ImGui::SetCursorScreenPos(ImVec2(centre - radius, y));
        BeginHeldOff(strip.busy);
        PanKnob("##pan", strip.pan, radius);
        FollowHold(strip.track, MixControl::kPan, before, *strip.pan);
        if (strip.track != nullptr && !strip.track->mix_reach[static_cast<std::size_t>(MixControl::kPan)].empty())
        {
            ItemTooltip(ControlTooltip(strip.track, MixControl::kPan));
        }
        EndHeldOff();
        const std::string pan = PanText(*strip.pan);
        ImFont* figures = theme::GetFonts().figures;
        const ImVec2 pan_size = TextSize(figures, theme::kSmallSize, pan);
        DrawText(draw, figures, theme::kSmallSize, ImVec2(centre - pan_size.x * 0.5f, y + radius * 2.0f + Px(3.0f)),
                 theme::kTextMuted, pan);
    }
    else
    {
        const ImVec2 label_size = CapsSize("Output");
        DrawCaps(draw, ImVec2(centre - label_size.x * 0.5f, y + Px(10.0f)), theme::kTextMuted, "Output");
    }
}

// The name of `strip` in a box of its colour at the bottom of the strip that ends at `end`, with the mark of a linked
// mix.
void DrawStripName(ImDrawList* draw, const Strip& strip, ImVec2 pos, ImVec2 end, float centre, float name_height,
                   ImFont* text)
{
    const ImVec2 name_min(pos.x + Px(5.0f), end.y - name_height);
    const ImVec2 name_max(end.x - Px(5.0f), end.y - Px(5.0f));
    draw->AddRectFilled(name_min, name_max, strip.colour, Px(4.0f));
    const std::string linked = strip.track != nullptr ? LinkedMixTooltip(*strip.track) : std::string();
    const ImVec2 mark_size = TextSize(text, theme::kSmallSize, "=");
    const float mark_left = name_max.x - Px(6.0f) - mark_size.x;
    const float name_right = linked.empty() ? name_max.x - Px(4.0f) : mark_left - Px(3.0f);
    const std::string shown = FitText(text, theme::kSmallSize, strip.name, name_right - name_min.x - Px(4.0f));
    const ImVec2 name_size = TextSize(text, theme::kSmallSize, shown);
    DrawText(draw, text, theme::kSmallSize,
             ImVec2(std::max(centre - name_size.x * 0.5f, name_min.x + Px(4.0f)),
                    name_min.y + (name_max.y - name_min.y - name_size.y) * 0.5f),
             theme::TextOn(strip.colour), shown);
    if (!linked.empty())
    {
        DrawText(draw, text, theme::kSmallSize,
                 ImVec2(mark_left, name_min.y + (name_max.y - name_min.y - mark_size.y) * 0.5f),
                 theme::TextOn(strip.colour), "=");
        ImGui::SetCursorScreenPos(name_min);
        ImGui::InvisibleButton("##linked", ImVec2(name_max.x - name_min.x, name_max.y - name_min.y));
        ItemTooltip(linked);
    }
}

} // namespace

bool DrawStrip(const Strip& strip, ImVec2 size)
{
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const ImVec2 end(pos.x + size.x, pos.y + size.y);
    const ImGuiIO& io = ImGui::GetIO();
    const bool clicked = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) &&
                         ImGui::IsMouseClicked(ImGuiMouseButton_Left) && ImRect(pos, end).Contains(io.MousePos);
    draw->AddRectFilled(pos, end, strip.selected ? theme::Mix(theme::kRaised, theme::kAccent, 0.12f) : theme::kRaised,
                        Px(8.0f));
    draw->AddRect(pos, end, strip.selected ? theme::WithAlpha(theme::kAccent, 0.6f) : theme::kBorder, Px(8.0f),
                  Px(1.0f));
    const float centre = std::floor(pos.x + size.x * 0.5f);
    ImFont* text = theme::GetFonts().text;

    // A slot for each send. A send that is missing from the console's tracks shows an empty slot, with the reason in
    // its tooltip. The output's strip leaves the space empty, and the faders line up.
    float y = pos.y + Px(8.0f);
    DrawStripSends(draw, strip, pos, size, y, text);
    y += Px(70.0f);

    // The pan knob and its value.
    DrawStripPan(draw, strip, centre, y);
    y += Px(46.0f);

    // M and S.
    if (strip.mute != nullptr && strip.solo != nullptr)
    {
        const ImVec2 button(Px(26.0f), Px(20.0f));
        DrawMuteSolo(draw, ImVec2(centre - button.x - Px(2.0f), y), button, Px(4.0f), strip.mute, strip.solo);
    }

    // The level and the peak. The FX channels' strips put theirs in the same place.
    y = pos.y + Px(kLevelsTop);
    DrawLevels(draw, centre, y, *strip.volume, strip.peak);
    y += Px(27.0f);

    // The fader, the meters, and the scale that they share between them.
    const float name_height = Px(28.0f);
    const float fader_height = std::max(end.y - name_height - Px(8.0f) - y, Px(48.0f));
    ImGui::SetCursorScreenPos(ImVec2(centre - Px(34.0f), y));
    const float before = *strip.volume;
    BeginHeldOff(strip.busy);
    Fader("##fader", strip.volume, ImVec2(Px(30.0f), fader_height), strip.highest);
    FollowHold(strip.track, MixControl::kVolume, before, *strip.volume);
    if (strip.track != nullptr && !strip.track->mix_reach[static_cast<std::size_t>(MixControl::kVolume)].empty())
    {
        ItemTooltip(ControlTooltip(strip.track, MixControl::kVolume));
    }
    EndHeldOff();
    DrawFaderMeters(draw, centre, y, fader_height, strip.meter, strip.highest);

    // The name, in a box of the track's colour. A linked mix shows an equals sign at the end of the box. The
    // arrangement marks a linked part the same way.
    DrawStripName(draw, strip, pos, end, centre, name_height, text);

    // The strip takes its place in the layout.
    ImGui::SetCursorScreenPos(pos);
    ImGui::Dummy(size);

    return clicked;
}

Strip TrackStrip(TrackView& track, bool selected, bool busy)
{
    Strip strip;
    strip.name = track.name.c_str();
    strip.colour = track.colour;
    strip.volume = &track.volume;
    strip.pan = &track.pan;
    strip.sends = &track.sends;
    strip.mute = &track.mute;
    strip.solo = &track.solo;
    strip.meter = track.meter;
    strip.peak = &track.peak;
    strip.track = &track;
    strip.selected = selected;
    strip.busy = busy;

    return strip;
}

Strip SongTrackStrip(const SongView& song, TrackView& track, bool selected, bool busy)
{
    Strip strip = TrackStrip(track, selected, busy);
    strip.highest = song.volume_highest.value_or(kFaderTop);
    strip.buses = song.sends;
    strip.sends_missing = song.sends_missing.c_str();

    return strip;
}

Strip OutputStrip(MasterBus& master)
{
    Strip strip;
    strip.name = "Stereo Out";
    strip.colour = theme::kControlActive;
    strip.volume = &master.volume;
    strip.meter = master.meter;
    strip.peak = &master.peak;

    return strip;
}

} // namespace channel_strip
} // namespace hokuto::app
