// SPDX-License-Identifier: MIT

// The faders, sliders and meters that widgets.h declares: the mixer's level faders, pan knobs and send sliders, the
// value sliders of the editors, and the level meters.

#include <imgui_internal.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#include "app/icons_parts.h"
#include "app/theme.h"
#include "app/widgets.h"

namespace hokuto::app
{
namespace
{

using icon_parts::CurveWidth;
using icon_parts::IconLine;
using icon_parts::RingRadius;
using theme::Px;

// The bottom of the fader's range in dB, and the curve that gives the levels near 0 dB more of its travel.
constexpr float kFaderBottom = -60.0f;
constexpr float kFaderCurve = 1.5f;

// The colours of a meter's sections, and the levels where they start.
constexpr float kMeterMidLevel = -18.0f;
constexpr float kMeterHighLevel = -6.0f;

// The level in dB for a height on a fader, from 0 to 1 (the inverse of FaderPosition).
float FaderLevel(float position)
{
    if (position <= 0.0f)
    {
        return kSilence;
    }

    return kFaderBottom + std::pow(std::min(position, 1.0f), 1.0f / kFaderCurve) * (kFaderTop - kFaderBottom);
}

// Where `value` lies on a slider of `range`, from 0 at the left to 1 at the right. A value outside the range lies at
// the nearer end.
float SliderPosition(float value, const SliderRange& range)
{
    const float position = range.logarithmic ? std::log(value / range.low) / std::log(range.high / range.low)
                                             : (value - range.low) / (range.high - range.low);
    return position > 0.0f ? std::min(position, 1.0f) : 0.0f;
}

// The value at `position` on a slider of `range`.
float SliderValue(float position, const SliderRange& range)
{
    if (range.logarithmic)
    {
        return range.low * std::pow(range.high / range.low, position);
    }

    return range.low + (range.high - range.low) * position;
}

// Follows a press or a drag of the slider `item`, `size` large. Ctrl with a press sets the value to `reset`. Returns
// whether the value changed.
bool DragSlider(ImGuiID item, float* value, const SliderRange& range, float reset, ImVec2 size, bool held)
{
    // A drag moves the slider from where the drag started. The position stays unrounded in Dear ImGui's storage while
    // the caller rounds the value. A slow drag then still moves the value.
    const ImGuiIO& io = ImGui::GetIO();
    ImGuiStorage* storage = ImGui::GetStateStorage();
    bool changed = false;
    if (ImGui::IsItemActivated())
    {
        if (io.KeyCtrl)
        {
            *value = reset;
            changed = true;
        }
        storage->SetFloat(item, SliderPosition(*value, range));
    }
    else if (held && io.MouseDelta.x != 0.0f && !io.KeyCtrl)
    {
        const float step = io.MouseDelta.x / std::max(size.x, 1.0f) * (io.KeyShift ? 0.1f : 1.0f);
        const float position = std::clamp(storage->GetFloat(item) + step, 0.0f, 1.0f);
        storage->SetFloat(item, position);
        *value = SliderValue(position, range);
        changed = true;
    }
    if (changed)
    {
        ImGui::MarkItemEdited(item);
    }

    return changed;
}

// Draws the track of a slider in `box`, `size` large, filled up to `value`.
void DrawSliderTrack(ImDrawList* draw, const ImRect& box, ImVec2 size, float value, const SliderRange& range, bool held,
                     bool hovered)
{
    // The track, filled from the left up to the value, with a line at the value's end. Inside a disabled block, the
    // style's alpha fades it.
    const float rounding = Px(4.0f);
    draw->AddRectFilled(box.Min, box.Max, ImGui::GetColorU32(theme::kDisplay), rounding);
    const float end = std::floor(box.Min.x + size.x * SliderPosition(value, range));
    if (end > box.Min.x)
    {
        const float alpha = held ? 0.5f : hovered ? 0.42f : 0.34f;
        const ImDrawFlags corners = end >= box.Max.x ? ImDrawFlags_RoundCornersAll : ImDrawFlags_RoundCornersLeft;
        draw->AddRectFilled(box.Min, ImVec2(end, box.Max.y), ImGui::GetColorU32(theme::kAccent, alpha), rounding,
                            corners);
    }
    const float line = std::clamp(end, box.Min.x + Px(2.0f), box.Max.x - Px(2.0f));
    draw->AddRectFilled(ImVec2(line - Px(1.0f), box.Min.y + Px(3.0f)), ImVec2(line + Px(1.0f), box.Max.y - Px(3.0f)),
                        ImGui::GetColorU32(theme::kAccent));
    draw->AddRect(box.Min, box.Max, ImGui::GetColorU32(held || hovered ? theme::kBorderStrong : theme::kBorder),
                  rounding, Px(1.0f));
}

// Draws the value in the middle of a slider's box.
void DrawSliderText(ImDrawList* draw, const ImRect& box, const float* value, const char* format)
{
    // A value too long for the slider, such as one far outside its range, is cut off at the slider's ends.
    char text[64];
    ImGui::DataTypeFormatString(text, IM_COUNTOF(text), ImGuiDataType_Float, value, format);
    ImFont* font = theme::GetFonts().figures;
    const ImVec2 text_size = TextSize(font, theme::kTextSize, text);
    draw->PushClipRect(box.Min, box.Max, true);
    DrawText(draw, font, theme::kTextSize,
             ImVec2((box.Min.x + box.Max.x - text_size.x) * 0.5f, (box.Min.y + box.Max.y - text_size.y) * 0.5f),
             ImGui::GetColorU32(theme::kText), text);
    draw->PopClipRect();
}

// The values typed into sliders' fields while the sliders were disabled (ValueSlider), each slider's under its ID, and
// the items that the sliders show (PushSliderItem), the innermost last. Then the slider whose edit came to an end in
// frame `frame`: with `released`, a value that waited came back, and without, a value began to wait (SliderReleased).
struct SliderWaits
{
    std::map<ImGuiID, WaitingValues<float>> values;
    std::vector<ShownItem> items;
    ImGuiID item = 0;
    int frame = -1;
    bool released = false;
};

SliderWaits& Waits()
{
    static SliderWaits waits;
    return waits;
}

// The item that the sliders show in this frame: the innermost of PushSliderItem, or an empty item outside them.
ShownItem SliderItem(const SliderWaits& waits)
{
    return waits.items.empty() ? ShownItem{} : waits.items.back();
}

// Drops the sliders' values that no slider can take any more (WaitingValues::DropStale), and the sliders left without a
// value. The map then contains only the sliders whose values still wait.
void DropStaleWaits(SliderWaits& waits)
{
    for (auto entry = waits.values.begin(); entry != waits.values.end();)
    {
        entry->second.DropStale();
        entry = entry->second.Empty() ? waits.values.erase(entry) : std::next(entry);
    }
}

} // namespace

bool Fader(const char* id, float* level, ImVec2 size, float highest)
{
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, size);
    const bool hovered = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();
    const ImGuiIO& io = ImGui::GetIO();

    // The fader follows the mouse from where it is. It doesn't jump to the click.
    const float cap_height = Px(24.0f);
    const float travel = std::max(size.y - cap_height, 1.0f);
    bool changed = false;
    if (ImGui::IsItemActivated() && io.KeyCtrl)
    {
        *level = 0.0f;
        changed = true;
    }
    else if (held && io.MouseDelta.y != 0.0f && !io.KeyCtrl)
    {
        const float step = io.MouseDelta.y / travel * (io.KeyShift ? 0.1f : 1.0f);
        *level = std::min(FaderLevel(std::clamp(FaderPosition(*level) - step, 0.0f, 1.0f)), highest);
        changed = true;
    }

    // The slot, a mark at 0 dB, and the cap.
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float centre = std::floor(pos.x + size.x * 0.5f);
    const float top = pos.y + cap_height * 0.5f;
    draw->AddRectFilled(ImVec2(centre - Px(2.0f), top), ImVec2(centre + Px(2.0f), top + travel), theme::kDisplay,
                        Px(2.0f));
    const float unity = std::floor(top + travel * (1.0f - FaderPosition(0.0f)));
    draw->AddRectFilled(ImVec2(centre - Px(9.0f), unity), ImVec2(centre - Px(4.0f), unity + Px(1.0f)), theme::kTextDim);
    draw->AddRectFilled(ImVec2(centre + Px(4.0f), unity), ImVec2(centre + Px(9.0f), unity + Px(1.0f)), theme::kTextDim);

    const float cap_top = std::floor(pos.y + travel * (1.0f - FaderPosition(*level)));
    const float half_width = std::min(size.x * 0.5f, Px(14.0f));
    const ImU32 cap = held ? theme::kFaderCapHeld : hovered ? theme::kFaderCapHover : theme::kFaderCap;
    draw->AddRectFilled(ImVec2(centre - half_width, cap_top), ImVec2(centre + half_width, cap_top + cap_height), cap,
                        Px(4.0f));
    draw->AddRectFilled(ImVec2(centre - half_width + Px(3.0f), cap_top + cap_height * 0.5f - Px(1.0f)),
                        ImVec2(centre + half_width - Px(3.0f), cap_top + cap_height * 0.5f), theme::kDisplay);

    return changed;
}

bool PanKnob(const char* id, float* pan, float radius)
{
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, ImVec2(radius * 2.0f, radius * 2.0f));
    const bool hovered = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();
    const ImGuiIO& io = ImGui::GetIO();

    // Dragging up turns the knob right.
    bool changed = false;
    if (ImGui::IsItemActivated() && io.KeyCtrl)
    {
        *pan = 0.0f;
        changed = true;
    }
    else if (held && io.MouseDelta.y != 0.0f && !io.KeyCtrl)
    {
        *pan = std::clamp(*pan - io.MouseDelta.y * (io.KeyShift ? 0.001f : 0.01f), -1.0f, 1.0f);
        changed = true;
    }

    // The knob turns through 270 degrees. The centre position is at the top. The arc runs from the centre to the value.
    // The knob's centre lies on the corner of a pixel, and its edges and its track's edges on pixels.
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 centre = theme::OnPixels(ImVec2(pos.x + radius, pos.y + radius));
    draw->AddCircleFilled(centre, theme::OnPixels(radius), hovered || held ? theme::kControlHover : theme::kControl,
                          32);
    const float track = CurveWidth(Px(2.5f));
    const float track_radius = RingRadius(radius - Px(2.5f), track);
    draw->PathArcTo(centre, track_radius, IM_PI * 0.75f, IM_PI * 2.25f, 32);
    draw->PathStroke(theme::kDisplay, track);
    const float angle = IM_PI * 1.5f + *pan * IM_PI * 0.75f;
    const ImVec2 inner(centre.x + std::cos(angle) * radius * 0.2f, centre.y + std::sin(angle) * radius * 0.2f);
    const ImVec2 outer(centre.x + std::cos(angle) * radius * 0.62f, centre.y + std::sin(angle) * radius * 0.62f);
    if (std::abs(*pan) > 0.005f)
    {
        draw->PathArcTo(centre, track_radius, std::min(angle, IM_PI * 1.5f), std::max(angle, IM_PI * 1.5f), 24);
        draw->PathStroke(theme::kAccent, track);
        draw->AddLine(inner, outer, theme::kText, Px(2.0f));
    }
    else
    {
        IconLine(draw, ImVec2(centre.x, theme::OnPixels(outer.y)), ImVec2(centre.x, theme::OnPixels(inner.y)),
                 theme::kText, Px(2.0f));
    }

    return changed;
}

bool SendSlider(const char* id, const char* bus, float* level, ImVec2 size)
{
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, size);
    const bool hovered = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();
    const ImGuiIO& io = ImGui::GetIO();

    // Dragging right raises the level. The slider follows the mouse from where it is.
    bool changed = false;
    if (ImGui::IsItemActivated() && io.KeyCtrl)
    {
        *level = 0.0f;
        changed = true;
    }
    else if (held && io.MouseDelta.x != 0.0f && !io.KeyCtrl)
    {
        const float step = io.MouseDelta.x / std::max(size.x, 1.0f) * (io.KeyShift ? 0.1f : 1.0f);
        *level = FaderLevel(std::clamp(FaderPosition(*level) + step, 0.0f, 1.0f));
        changed = true;
    }

    // The slot, a bar in the FX channels' colour up to the level, the bus on the left and the level on the right.
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 max(pos.x + size.x, pos.y + size.y);
    draw->AddRectFilled(pos, max, hovered || held ? theme::kControlHover : theme::kControl, Px(4.0f));
    const float fill = std::floor(pos.x + size.x * FaderPosition(*level));
    if (fill > pos.x)
    {
        const ImDrawFlags corners = fill >= max.x ? ImDrawFlags_RoundCornersAll : ImDrawFlags_RoundCornersLeft;
        draw->AddRectFilled(pos, ImVec2(fill, max.y), theme::Mix(theme::kAccent, theme::kRaised, 0.45f), Px(4.0f),
                            corners);
    }
    ImFont* text = theme::GetFonts().text;
    ImFont* figures = theme::GetFonts().figures;
    const std::string value = LevelText(*level);
    const ImVec2 bus_size = TextSize(text, theme::kSmallSize, bus);
    const ImVec2 value_size = TextSize(figures, theme::kSmallSize, value);
    draw->PushClipRect(pos, max, true);
    DrawText(draw, text, theme::kSmallSize, ImVec2(pos.x + Px(6.0f), std::floor(pos.y + (size.y - bus_size.y) * 0.5f)),
             theme::kText, bus);
    DrawText(draw, figures, theme::kSmallSize,
             ImVec2(max.x - Px(6.0f) - value_size.x, std::floor(pos.y + (size.y - value_size.y) * 0.5f)),
             theme::kTextMuted, value);
    draw->PopClipRect();

    return changed;
}

std::string PanText(float pan)
{
    const int value = static_cast<int>(std::lround(std::abs(pan) * 100.0f));
    if (value == 0)
    {
        return "C";
    }

    return (pan < 0.0f ? "L" : "R") + std::to_string(value);
}

std::string LevelText(float level)
{
    if (level < -59.95f)
    {
        return "-∞";
    }

    std::array<char, 16> text{};
    const float rounded = std::round(level * 10.0f) / 10.0f;
    std::snprintf(text.data(), text.size(), rounded > 0.0f ? "+%.1f" : "%.1f", rounded == 0.0f ? 0.0f : rounded);

    return text.data();
}

std::string VolumeLevel(double volume)
{
    return LevelText(static_cast<float>(20.0 * std::log10(volume / kFullVolume))) + " dB";
}

float DecibelsOfGain(float gain)
{
    return gain > 0.0f ? 20.0f * std::log10(gain) : kSilence;
}

float GainOfDecibels(float level)
{
    return level > kSilence ? std::pow(10.0f, level / 20.0f) : 0.0f;
}

bool ValueSlider(const char* id, float* value, const SliderRange& range, float reset, const char* format, ImVec2 size,
                 const SliderRange* typed)
{
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    const ImGuiID item = window->GetID(id);
    const ImVec2 pos = window->DC.CursorPos;
    const ImRect box(pos, ImVec2(pos.x + size.x, pos.y + size.y));
    ImGui::ItemSize(box);
    if (!ImGui::ItemAdd(box, item, nullptr, ImGuiItemFlags_Inputable))
    {
        return false;
    }

    // A double-click types a value, and so does Tab from the field before. Dear ImGui's text input takes the slider's
    // place until Return, Escape or a click elsewhere closes it. Inside a disabled block, an open field still receives
    // the keys, and the value that it ends with waits.
    const ImGuiContext& context = *ImGui::GetCurrentContext();
    const bool disabled = (context.CurrentItemFlags & ImGuiItemFlags_Disabled) != 0;
    const bool typing = ImGui::TempInputIsActive(item);
    bool hovered = false;
    bool held = false;
    if (!typing)
    {
        ImGui::ButtonBehavior(box, item, &hovered, &held);
    }
    const bool tabbed =
        context.NavActivateId == item && (context.NavActivateFlags & ImGuiActivateFlags_PreferInput) != 0;
    SliderWaits& waits = Waits();
    DropStaleWaits(waits);
    WaitingValues<float>& waiting = waits.values[item];
    const ShownItem shown_item = SliderItem(waits);
    if (typing || tabbed || (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)))
    {
        const SliderRange& limits = typed != nullptr ? *typed : range;
        const float before = *value;
        const bool reenabled = BeginOpenField(typing, disabled);
        if (reenabled)
        {
            // ItemAdd marked the slider's item disabled, and the field uses that item. Dear ImGui's ItemHoverable
            // closes a disabled field once the pointer rests on it.
            ImGui::GetCurrentContext()->LastItemData.ItemFlags &= ~ImGuiItemFlags_Disabled;
        }
        const bool entered =
            ImGui::TempInputScalar(box, item, id, ImGuiDataType_Float, value, format, &limits.low, &limits.high);
        EndOpenField(reenabled);
        if (!entered || !disabled)
        {
            return entered;
        }

        (void)waiting.Pass(shown_item, item, before, *value, true);
        waits.item = item;
        waits.frame = ImGui::GetFrameCount();
        waits.released = false;
        *value = before;
        return false;
    }

    // A value that waited comes back once the slider is enabled. Until then, the slider shows it.
    const std::optional<float> waited = waiting.Pass(shown_item, item, *value, std::nullopt, disabled);
    if (waited)
    {
        *value = *waited;
        waits.item = item;
        waits.frame = ImGui::GetFrameCount();
        waits.released = true;
    }
    const bool changed = DragSlider(item, value, range, reset, size, held) || waited.has_value();
    const float shown = disabled ? waiting.Shown(shown_item, item, *value) : *value;

    ImDrawList* draw = window->DrawList;
    DrawSliderTrack(draw, box, size, shown, range, held, hovered);
    DrawSliderText(draw, box, &shown, format);

    return changed;
}

bool SliderReleased()
{
    const SliderWaits& waits = Waits();
    if (waits.item == ImGui::GetItemID() && waits.frame == ImGui::GetFrameCount())
    {
        return waits.released;
    }

    return ImGui::IsItemDeactivatedAfterEdit();
}

void PushSliderItem(const ShownItem& item)
{
    Waits().items.push_back(item);
}

void PopSliderItem()
{
    Waits().items.pop_back();
}

void DrawMeter(ImDrawList* draw, ImVec2 min, ImVec2 max, float level)
{
    draw->AddRectFilled(min, max, theme::kDisplay, Px(1.5f));
    const float position = FaderPosition(level);
    if (position <= 0.0f)
    {
        return;
    }

    // Fill from the bottom, one section at a time.
    const float height = max.y - min.y;
    const float top = max.y - height * position;
    const float mid = max.y - height * FaderPosition(kMeterMidLevel);
    const float high = max.y - height * FaderPosition(kMeterHighLevel);
    draw->AddRectFilled(ImVec2(min.x, std::max(top, mid)), max, theme::kPlay);
    if (top < mid)
    {
        draw->AddRectFilled(ImVec2(min.x, std::max(top, high)), ImVec2(max.x, mid), theme::kMeterMid);
    }
    if (top < high)
    {
        draw->AddRectFilled(ImVec2(min.x, top), ImVec2(max.x, high), theme::kMeterHigh);
    }
}

float FaderPosition(float level)
{
    if (level <= kFaderBottom)
    {
        return 0.0f;
    }

    return std::pow((std::min(level, kFaderTop) - kFaderBottom) / (kFaderTop - kFaderBottom), kFaderCurve);
}

void DrawHorizontalMeter(ImDrawList* draw, ImVec2 min, ImVec2 max, float level)
{
    draw->AddRectFilled(min, max, theme::kDisplay, Px(1.5f));
    const float position = FaderPosition(level);
    if (position <= 0.0f)
    {
        return;
    }

    // Fill from the left, one section at a time.
    const float width = max.x - min.x;
    const float end = min.x + width * position;
    const float mid = min.x + width * FaderPosition(kMeterMidLevel);
    const float high = min.x + width * FaderPosition(kMeterHighLevel);
    draw->AddRectFilled(min, ImVec2(std::min(end, mid), max.y), theme::kPlay);
    if (end > mid)
    {
        draw->AddRectFilled(ImVec2(mid, min.y), ImVec2(std::min(end, high), max.y), theme::kMeterMid);
    }
    if (end > high)
    {
        draw->AddRectFilled(ImVec2(high, min.y), ImVec2(end, max.y), theme::kMeterHigh);
    }
}

} // namespace hokuto::app
