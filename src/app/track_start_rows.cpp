// SPDX-License-Identifier: MIT

#include "app/track_start_rows.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

#include "app/theme.h"
#include "app/widgets.h"
#include "app/zone_parts.h"
#include "format/starts.h"

namespace hokuto::app
{
namespace
{

using theme::Px;

// The share of the width that the labels take, the box's width and its gap, and the lowest volume that the Volume
// slider shows, in dB. Below it, the track's volume is 0.
constexpr float kLabelShare = 0.4f;
constexpr float kBox = 18.0f;
constexpr float kBoxGap = 8.0f;
constexpr float kLowestVolume = -60.0f;

// The rows' slider IDs past the variables' numbers.
constexpr int kVolumeRow = 0;

// A row's label, with `tooltip` over it. Returns the left edge of the row's box.
float DrawRowLabel(ImDrawList* draw, ImVec2 pos, float width, const char* label, const char* tooltip)
{
    ImFont* text = theme::GetFonts().text;
    const float height = Px(zone_parts::kPropertyHeight);
    const ImVec2 size = TextSize(text, theme::kTextSize, label);
    DrawText(draw, text, theme::kTextSize, ImVec2(pos.x, pos.y + std::floor((height - size.y) * 0.5f)),
             theme::kTextMuted, label);
    RectTooltip(pos, ImVec2(pos.x + width * kLabelShare, pos.y + height), tooltip);

    return pos.x + std::floor(width * kLabelShare);
}

// The box of a row at `x`, lit while `on`. Returns true when the user clicks it.
bool DrawRowBox(ImVec2 pos, float x, const char* id, bool on)
{
    const float height = Px(zone_parts::kPropertyHeight);
    ImGui::SetCursorScreenPos(ImVec2(x, pos.y + std::floor((height - ImGui::GetFrameHeight()) * 0.5f)));
    bool shown = on;

    return CheckBox(id, &shown);
}

// The slider of a row after its box, at `x`. Returns the value once the user lets it go.
std::optional<float> DrawRowSlider(ImVec2 pos, float x, float right, int id, float value, const SliderRange& range,
                                   float reset, const char* format, const SliderRange* typed, bool on,
                                   ArrangementState& state)
{
    const float height = Px(zone_parts::kPropertyHeight);
    const float left = x + Px(kBox + kBoxGap);
    const bool holding = state.track_start_held && state.track_start_held->first == id;
    float shown = holding ? state.track_start_held->second : value;
    ImGui::BeginDisabled(!on);
    ImGui::SetCursorScreenPos(ImVec2(left, pos.y + std::floor((height - ImGui::GetFrameHeight()) * 0.5f)));
    if (ValueSlider("##slider", &shown, range, reset, on ? format : "",
                    ImVec2(std::max(right - left, 0.0f), ImGui::GetFrameHeight()), typed))
    {
        state.track_start_held = std::pair(id, shown);
    }
    const bool released = SliderReleased();
    ImGui::EndDisabled();
    if (released && holding)
    {
        return std::exchange(state.track_start_held, std::nullopt)->second;
    }

    return std::nullopt;
}

} // namespace

float DrawTrackStart(ImDrawList* draw, ImVec2 pos, float width, const TrackView& track, int row,
                     ArrangementState& state)
{
    const float height = Px(zone_parts::kPropertyHeight);
    const float right = pos.x + width;
    const TrackStartEdit kept{.track = row, .volume = track.start_volume, .variables = track.start_variables};
    ImGui::PushID("track start");

    // The rows' heading, in capitals as the Sound section's.
    const ImVec2 caps = CapsSize("Start");
    DrawCaps(draw, ImVec2(pos.x, pos.y + height - caps.y - Px(6.0f)), theme::kTextMuted, "Start");
    pos.y += height;

    // Mute at Start: the 3DS mutes the track, and the Wii U and the Switch turn it down to a volume of 0.
    float x = DrawRowLabel(draw, pos, width, "Mute at Start",
                           "Mutes the track when the sound starts. Games play one sequence as several arrangements "
                           "this way.");
    if (DrawRowBox(pos, x, "##mute", track.start_muted))
    {
        state.start_mute = row;
    }
    pos.y += height;

    // The volume.
    x = DrawRowLabel(draw, pos, width, "Volume", "Volume that the game gives this track when it starts the sound.");
    ImGui::PushID(kVolumeRow);
    const bool set = track.start_volume.has_value();
    if (DrawRowBox(pos, x, "##set", set))
    {
        TrackStartEdit edit = kept;
        edit.volume = set ? std::nullopt : std::optional<float>(1.0f);
        edit.name = set ? "Clear Start Track Volume" : "Set Start Track Volume";
        state.track_start = std::move(edit);
    }
    const float level = std::max(DecibelsOfGain(track.start_volume.value_or(1.0f)), kLowestVolume);
    const char* format = level <= kLowestVolume ? "-\xE2\x88\x9E dB" : "%+.1f dB";
    if (const std::optional<float> moved = DrawRowSlider(pos, x, right, kVolumeRow, level, {kLowestVolume, kFaderTop},
                                                         0.0f, format, nullptr, set, state))
    {
        TrackStartEdit edit = kept;
        edit.volume = *moved <= kLowestVolume ? 0.0f : GainOfDecibels(*moved);
        edit.name = "Change Start Track Volume";
        state.track_start = std::move(edit);
    }
    ImGui::PopID();
    pos.y += height;

    // A row for each track variable that the start sets on the track alone.
    const SliderRange kVariableRange{-1.0f, 127.0f};
    const SliderRange kTypedRange{-32768.0f, 32767.0f};
    for (const auto& [variable, value] : track.start_variables)
    {
        const std::string label = "Variable " + std::to_string(variable);
        x = DrawRowLabel(draw, pos, width, label.c_str(),
                         "Track variable that the game sets on this track alone when it starts the sound.");
        ImGui::PushID(variable);
        if (DrawRowBox(pos, x, "##set", true))
        {
            TrackStartEdit edit = kept;
            edit.variables.erase(variable);
            edit.name = "Clear Start Track Variable";
            state.track_start = std::move(edit);
        }
        if (const std::optional<float> moved = DrawRowSlider(pos, x, right, variable, static_cast<float>(value),
                                                             kVariableRange, -1.0f, "%.0f", &kTypedRange, true, state))
        {
            TrackStartEdit edit = kept;
            edit.variables[variable] = static_cast<int16_t>(std::lround(std::clamp(*moved, -32768.0f, 32767.0f)));
            edit.name = "Change Start Track Variable";
            state.track_start = std::move(edit);
        }
        ImGui::PopID();
        pos.y += height;
    }
    ImGui::PopID();

    return pos.y;
}

} // namespace hokuto::app
