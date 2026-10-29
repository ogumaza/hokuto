// SPDX-License-Identifier: MIT

#include "app/wave_window.h"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cfloat>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "ntdlib/sar/formats.h"

#include "app/item_views.h"
#include "app/theme.h"
#include "app/widgets.h"

namespace hokuto::app
{
namespace
{

namespace sar = ntdlib::sar;
using theme::Px;

// The window's sizes, before the display's scale: its first size and its least, the waveform's height, the ruler over
// it, the labels of the fields, the boxes of the typed values, the gap between the parts, and the band around a marker
// that a drag takes.
constexpr float kWindowWidth = 980.0f;
constexpr float kWindowHeight = 660.0f;
constexpr float kLeastWidth = 720.0f;
constexpr float kLeastHeight = 560.0f;
constexpr float kWaveformHeight = 230.0f;
constexpr float kRulerHeight = 20.0f;
constexpr float kLabelWidth = 96.0f;
constexpr float kBoxWidth = 120.0f;
constexpr float kGap = 10.0f;
constexpr float kMarkerBand = 5.0f;
constexpr float kFlagSize = 14.0f;
constexpr float kButtonHeight = 28.0f;

// The points around the pointer within which a dragged marker snaps to a zero crossing.
constexpr float kSnapReach = 8.0f;

// Each step of the mouse wheel zooms by this factor. The view shows at least kLeastAcross samples.
constexpr double kZoomStep = 1.25;
constexpr double kLeastAcross = 32.0;

// The share of the view that a step of the wheel scrolls.
constexpr double kScrollStep = 0.1;

// The least points between two labels of the ruler.
constexpr float kLabelSpacing = 90.0f;

// The highest rate that the field takes, in samples per second.
constexpr uint32_t kHighestRate = 192'000;

// What each field does, for the tooltips of the labels.
const char* const kLoopTip = "On, the wave plays from its start to the loop's end, then round the loop until it stops.";
const char* const kLoopStartTip =
    "The loop's first sample. A loop in DSP-ADPCM starts at a frame of 14 samples. A stream's loop starts at a block, "
    "so that the samples stay as they are. The 3DS's tools never start a loop at a stream's first block. Such a loop "
    "moves to the second block, and the first block's samples follow the end. The loop sounds the same.";
const char* const kLoopEndTip =
    "One past the last sample that plays. The samples after it stay in the wave, and a later edit can move the end "
    "back over them.";
const char* const kStreamEndTip =
    "One past the last sample that plays. A stream's loop runs to its end. A new end makes the stream again from its "
    "samples, and DSP-ADPCM is encoded again.";
const char* const kEndTip =
    "One past the last sample that plays. Without a loop, the wave stops there. The samples after it stay in the "
    "wave, and a later edit can move the end back over them.";
const char* const kRateTip =
    "The samples per second that the console plays the wave at. Another rate changes the pitch and the length, and the "
    "samples stay.";
const char* const kEncodingTip =
    "How the wave holds its samples. Another encoding makes the wave again from its samples. DSP-ADPCM takes 8 bytes "
    "for each 14 samples of a channel: 4/7 of the bytes of PCM8 and 2/7 of those of PCM16, with some noise.";
const char* const kTracksTip =
    "The tracks that the sound plays, from the archive's table. A track plays its channels with its volume and pan.";
const char* const kRegionsTip = "The regions of the stream. Hokuto reads no stream that holds regions.";

const char* EncodingName(sar::WaveEncoding encoding)
{
    switch (encoding)
    {
    case sar::WaveEncoding::kPcm8:
        return "PCM8";
    case sar::WaveEncoding::kPcm16:
        return "PCM16";
    case sar::WaveEncoding::kDspAdpcm:
        return "DSP-ADPCM";
    default:
        return "Other";
    }
}

// The name of an edit of the end in the Undo History. A stream's loop runs to its end. A wave without a loop plays up
// to the end.
const char* EndEditName(const WaveView& view)
{
    return view.stream || !view.values.loop ? "Change End" : "Change Loop End";
}

// The time of `samples` at `rate`, in seconds with three places.
std::string TimeOf(double samples, uint32_t rate)
{
    std::array<char, 32> text{};
    std::snprintf(text.data(), text.size(), "%.3f s", rate == 0 ? 0.0 : samples / rate);

    return text.data();
}

// The number in `text`, with its spaces and commas left out, or nothing.
std::optional<uint32_t> NumberIn(const std::string& text)
{
    std::string digits;
    for (const char c : text)
    {
        if (c != ' ' && c != ',')
        {
            digits += c;
        }
    }
    uint32_t value = 0;
    const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), value);
    if (error != std::errc() || end != digits.data() + digits.size() || digits.empty())
    {
        return std::nullopt;
    }

    return value;
}

// The samples that the view shows: from its first, and across its width.
struct Span
{
    double first = 0.0;
    double across = 1.0;
};

Span SpanOf(const WaveView& view, const WaveWindowState& state)
{
    const double held = std::max<double>(view.held, 1.0);
    if (state.across <= 0.0 || state.across >= held)
    {
        return {0.0, held};
    }

    const double across = std::max(state.across, std::min(kLeastAcross, held));
    return {std::clamp(state.first, 0.0, held - across), across};
}

float XOf(const Span& span, ImVec2 min, float width, double sample)
{
    return min.x + static_cast<float>((sample - span.first) / span.across) * width;
}

double SampleAt(const Span& span, ImVec2 min, float width, float x)
{
    return span.first + static_cast<double>(x - min.x) / width * span.across;
}

// Draws channel `channel` of `view` between `min` and `max`: the least and the most sample under each point, or a line
// through the samples where a point covers less than one.
void DrawChannel(ImDrawList* draw, const WaveView& view, std::size_t channel, const Span& span, ImVec2 min, ImVec2 max,
                 ImU32 colour)
{
    const std::vector<int16_t>& samples = view.samples[channel];
    const std::vector<std::pair<int16_t, int16_t>>& peaks = view.peaks[channel];
    const float width = max.x - min.x;
    const float middle = std::floor((min.y + max.y) * 0.5f);
    const float half = (max.y - min.y) * 0.5f - Px(2.0f);
    const auto y_of = [middle, half](int sample)
    {
        return middle - static_cast<float>(sample) / 32768.0f * half;
    };

    draw->AddLine(ImVec2(min.x, middle), ImVec2(max.x, middle), theme::kGridBar, Px(1.0f));
    if (samples.empty())
    {
        return;
    }

    // Zoomed in past a sample a point, a line joins the samples.
    const double per_point = span.across / width;
    if (per_point < 1.0)
    {
        const auto first = static_cast<std::size_t>(std::max(0.0, std::floor(span.first)));
        const auto last = std::min(samples.size() - 1, static_cast<std::size_t>(std::ceil(span.first + span.across)));
        std::vector<ImVec2> points;
        for (std::size_t sample = first; sample <= last; ++sample)
        {
            points.emplace_back(XOf(span, min, width, static_cast<double>(sample)), y_of(samples[sample]));
        }
        draw->AddPolyline(points.data(), static_cast<int>(points.size()), colour, Px(1.0f));
        return;
    }

    // Each point's least and most sample, from the peaks where a point covers whole runs.
    const auto columns = static_cast<int>(std::ceil(width));
    for (int column = 0; column < columns; ++column)
    {
        const double from = span.first + column * per_point;
        const auto start = static_cast<std::size_t>(std::max(0.0, std::floor(from)));
        const auto end = std::min(samples.size(), static_cast<std::size_t>(std::ceil(from + per_point)));
        if (start >= end)
        {
            continue;
        }
        int low = 0;
        int high = 0;
        if (per_point >= static_cast<double>(kPeakRun))
        {
            const std::size_t first_run = start / kPeakRun;
            const std::size_t last_run = std::min(peaks.size(), (end + kPeakRun - 1) / kPeakRun);
            low = INT16_MAX;
            high = INT16_MIN;
            for (std::size_t run = first_run; run < last_run; ++run)
            {
                low = std::min<int>(low, peaks[run].first);
                high = std::max<int>(high, peaks[run].second);
            }
        }
        else
        {
            const auto [least, most] = std::minmax_element(samples.begin() + static_cast<std::ptrdiff_t>(start),
                                                           samples.begin() + static_cast<std::ptrdiff_t>(end));
            low = *least;
            high = *most;
        }
        const float x = min.x + static_cast<float>(column) + 0.5f;
        draw->AddLine(ImVec2(x, y_of(high)), ImVec2(x, y_of(low) + 1.0f), colour);
    }
}

// The ruler over the waveform: a label at each whole step of time that leaves room between the labels.
void DrawRuler(ImDrawList* draw, const WaveView& view, const Span& span, ImVec2 min, float width)
{
    const uint32_t rate = std::max<uint32_t>(view.values.rate, 1);
    const double seconds_across = span.across / rate;
    const double least_step = seconds_across * kLabelSpacing / std::max(width / Px(1.0f), 1.0f);
    double step = std::pow(10.0, std::floor(std::log10(std::max(least_step, 1e-6))));
    for (const double factor : {1.0, 2.0, 5.0, 10.0})
    {
        if (step * factor >= least_step)
        {
            step *= factor;
            break;
        }
    }
    const int places = std::clamp(static_cast<int>(-std::floor(std::log10(step))), 0, 6);
    ImFont* font = theme::GetFonts().figures;
    const double first_second = std::ceil(span.first / rate / step) * step;
    for (double second = first_second; second * rate <= span.first + span.across; second += step)
    {
        const float x = XOf(span, min, width, second * rate);
        std::array<char, 32> label{};
        std::snprintf(label.data(), label.size(), "%.*f", places, second);
        draw->AddLine(ImVec2(x, min.y + Px(kRulerHeight) - Px(5.0f)), ImVec2(x, min.y + Px(kRulerHeight)),
                      theme::kTextDim, Px(1.0f));
        DrawText(draw, font, theme::kSmallSize, ImVec2(x + Px(3.0f), min.y + Px(1.0f)), theme::kTextMuted,
                 label.data());
    }
}

// The sample under the pointer and its time, at the right end of the ruler over its labels.
void DrawPointer(ImDrawList* draw, double sample, uint32_t rate, ImVec2 min, float width)
{
    std::array<char, 64> text{};
    std::snprintf(text.data(), text.size(), "Sample %u, %s", static_cast<unsigned>(sample),
                  TimeOf(sample, rate).c_str());
    ImFont* font = theme::GetFonts().figures;
    const ImVec2 size = TextSize(font, theme::kSmallSize, text.data());
    const ImVec2 pos(min.x + width - size.x - Px(4.0f), min.y + Px(1.0f));
    draw->AddRectFilled(ImVec2(pos.x - Px(8.0f), min.y), ImVec2(min.x + width, min.y + Px(kRulerHeight) - Px(6.0f)),
                        theme::kRaised);
    DrawText(draw, font, theme::kSmallSize, pos, theme::kText, text.data());
}

// The place of `marker` for a drag to `target`, a sample, within the rules of the item's loop.
std::optional<uint32_t> DraggedPlace(const WaveView& view, const WaveValues& values, LoopMarker marker, double target,
                                     bool snap, double reach)
{
    if (marker == LoopMarker::kStart)
    {
        const uint32_t step = LoopStep(values.encoding, view.stream);
        return LoopMarkerPlace(view.samples, target, step, 0, values.loop_end - 1, snap, reach);
    }

    return LoopMarkerPlace(view.samples, target, 1, values.loop ? values.loop_start + 1 : 1, view.held, snap, reach);
}

// The waveform at `pos`, `size` large, with the ruler over it, the loop and the markers. A drag of a marker moves it,
// and the edit ends when the user lets go. Ctrl with the wheel zooms around the pointer, and the wheel scrolls. The
// arrangement takes the same gestures. A double-click shows the whole item.
void DrawWaveform(const WaveView& view, WaveWindowState& state, bool busy, ImVec2 pos, ImVec2 size, WaveEdit& edit)
{
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 min(pos.x, pos.y + Px(kRulerHeight));
    const ImVec2 max(pos.x + size.x, pos.y + size.y);
    const float width = max.x - min.x;
    Span span = SpanOf(view, state);
    const WaveValues before = state.held ? *state.held : view.values;
    draw->AddRectFilled(min, max, theme::kDisplay, Px(4.0f));
    DrawRuler(draw, view, span, pos, width);

    // The gestures: a drag of a marker, the wheel and a double-click.
    ImGui::SetCursorScreenPos(min);
    ImGui::InvisibleButton("##waveform", ImVec2(width, max.y - min.y));
    const bool hovered = ImGui::IsItemHovered();
    const ImVec2 mouse = ImGui::GetMousePos();
    const float start_x = XOf(span, min, width, before.loop_start);
    const float end_x = XOf(span, min, width, before.loop_end);

    // A marker takes a press within kMarkerBand of its line, or on its flag: the start's flag lies to the right of its
    // line, and the end's to the left. Where both markers take the press, the nearer line wins.
    const bool in_flags = mouse.y >= min.y && mouse.y <= min.y + Px(kFlagSize);
    const auto reach = [&mouse, in_flags](float x, float side) -> std::optional<float>
    {
        const float off = std::abs(mouse.x - x);
        const bool on_flag = in_flags && (mouse.x - x) * side >= 0.0f && off <= Px(kFlagSize);
        return off <= Px(kMarkerBand) || on_flag ? std::optional<float>(off) : std::nullopt;
    };
    const std::optional<float> end_reach = reach(end_x, -1.0f);
    const std::optional<float> start_reach = before.loop ? reach(start_x, 1.0f) : std::nullopt;
    std::optional<LoopMarker> near;
    if (end_reach)
    {
        near = LoopMarker::kEnd;
    }
    if (start_reach && (!end_reach || *start_reach < *end_reach))
    {
        near = LoopMarker::kStart;
    }
    if ((hovered && near && !busy) || state.dragging)
    {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    }
    if (ImGui::IsItemActivated() && near && !busy)
    {
        state.dragging = near;
        state.held = view.values;
    }
    if (ImGui::IsItemActive() && state.dragging && state.held)
    {
        const double reach = Px(kSnapReach) * span.across / width;
        const std::optional<uint32_t> place =
            DraggedPlace(view, *state.held, *state.dragging, SampleAt(span, min, width, mouse.x), state.snap, reach);
        if (place)
        {
            (*state.dragging == LoopMarker::kStart ? state.held->loop_start : state.held->loop_end) = *place;
        }
    }
    if (ImGui::IsItemDeactivated() && state.dragging)
    {
        if (state.held && *state.held != view.values && !busy)
        {
            edit.values = *state.held;
            edit.name = *state.dragging == LoopMarker::kStart ? "Change Loop Start" : EndEditName(view);
        }
        state.dragging.reset();
        state.held.reset();
    }
    if (hovered && !state.dragging)
    {
        const ImGuiIO& io = ImGui::GetIO();
        const float zoom = io.KeyCtrl ? io.MouseWheel : 0.0f;
        const float scroll = io.KeyCtrl ? io.MouseWheelH : io.MouseWheel + io.MouseWheelH;
        if (zoom != 0.0f)
        {
            const double pointer = SampleAt(span, min, width, mouse.x);
            const double across =
                std::clamp(span.across * std::pow(kZoomStep, -zoom),
                           std::min(kLeastAcross, static_cast<double>(view.held)), static_cast<double>(view.held));
            state.across = across;
            state.first = pointer - (mouse.x - min.x) / width * across;
        }
        if (scroll != 0.0f)
        {
            state.across = span.across;
            state.first = span.first - scroll * span.across * kScrollStep;
        }
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && !near)
        {
            state.across = 0.0;
            state.first = 0.0;
        }
        span = SpanOf(view, state);
    }

    // The loop's region and the channels. The samples after the end lie in shadow.
    const WaveValues values = state.held ? *state.held : view.values;
    draw->PushClipRect(min, max, true);
    const float loop_left = XOf(span, min, width, values.loop_start);
    const float loop_right = XOf(span, min, width, values.loop_end);
    if (values.loop)
    {
        draw->AddRectFilled(ImVec2(loop_left, min.y), ImVec2(loop_right, max.y),
                            theme::WithAlpha(theme::kCycle, 0.08f));
    }
    draw->AddRectFilled(ImVec2(loop_right, min.y), max, theme::WithAlpha(theme::kWindow, 0.55f));
    const std::size_t channels = view.samples.size();
    const float lane = (max.y - min.y) / static_cast<float>(std::max<std::size_t>(channels, 1));
    for (std::size_t channel = 0; channel < channels; ++channel)
    {
        const float top = min.y + lane * static_cast<float>(channel);
        DrawChannel(draw, view, channel, span, ImVec2(min.x, top), ImVec2(max.x, top + lane), theme::kAccent);
        if (channel > 0)
        {
            draw->AddLine(ImVec2(min.x, top), ImVec2(max.x, top), theme::kBorder, Px(1.0f));
        }
    }

    // The markers over them. A marker's flag sits at the top of its line, inside the loop: the start's to the right,
    // and the end's to the left.
    const auto marker = [draw, &min, &max](float x, float side, ImU32 colour, const char* letter)
    {
        const float flag = Px(kFlagSize) * side;
        draw->AddLine(ImVec2(x, min.y), ImVec2(x, max.y), colour, Px(1.5f));
        draw->AddRectFilled(ImVec2(std::min(x, x + flag), min.y), ImVec2(std::max(x, x + flag), min.y + Px(kFlagSize)),
                            colour, Px(2.0f));
        const ImVec2 letter_size = TextSize(theme::GetFonts().strong, theme::kSmallSize, letter);
        DrawText(draw, theme::GetFonts().strong, theme::kSmallSize,
                 ImVec2(x + flag * 0.5f - letter_size.x * 0.5f, min.y + (Px(kFlagSize) - letter_size.y) * 0.5f),
                 theme::kTextOnColour, letter);
    };
    if (values.loop)
    {
        marker(loop_left, 1.0f, theme::kCycle, "S");
    }
    marker(loop_right, -1.0f, values.loop ? theme::kCycle : theme::kTextMuted, "E");
    draw->PopClipRect();

    // The sample under the pointer, or under the dragged marker.
    if (state.dragging && state.held)
    {
        const LoopMarker dragged = *state.dragging;
        DrawPointer(draw, dragged == LoopMarker::kStart ? values.loop_start : values.loop_end, values.rate, pos, width);
    }
    else if (hovered)
    {
        const double sample = std::clamp(SampleAt(span, min, width, mouse.x), 0.0, static_cast<double>(view.held));
        DrawPointer(draw, sample, values.rate, pos, width);
    }
}

// A row of a fields' table: its label with a tooltip, then the cursor in the value's cell.
void FieldLabel(const char* label, const char* tooltip)
{
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme::kTextMuted), "%s", label);
    ItemTooltip(tooltip);
    ImGui::TableNextColumn();
}

// A box in the value's cell that shows `value` and takes a typed number. Once the user ends the typing, returns the
// typed text if it differs from the value's.
std::optional<std::string> NumberBox(const char* id, uint32_t value, WaveWindowState& state, bool enabled)
{
    const std::string text = std::to_string(value);
    if (!enabled)
    {
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme::kTextDim), "%s", text.c_str());
        return std::nullopt;
    }

    const std::optional<std::string> entered =
        TextEntry(id, text, ImGui::GetCursorScreenPos(), Px(kBoxWidth), state.typing);
    if (!entered || *entered == text)
    {
        return std::nullopt;
    }

    return entered;
}

// The number in `typed` where it lies from `least` to `most`. Otherwise nothing, and `refused` holds the message that
// says what `what` takes, such as "The rate is a whole number of hertz from 1 to 192000.".
std::optional<uint32_t> NumberFrom(const std::string& typed, uint32_t least, uint32_t most, const std::string& what,
                                   std::string& refused)
{
    const std::optional<uint32_t> number = NumberIn(typed);
    if (number && *number >= least && *number <= most)
    {
        return number;
    }

    refused = what + " from " + std::to_string(least) + " to " + std::to_string(most) + ".";

    return std::nullopt;
}

// Muted text in the next cell of the loop's table, such as the time of the value before it.
void NoteCell(const std::string& note)
{
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme::kTextMuted), "%s", note.c_str());
}

// The fields of the loop: whether the item loops, the loop's start and its end, and the samples that the item holds.
void DrawLoopFields(const WaveView& view, WaveWindowState& state, float width, WaveEdit& edit)
{
    if (!ImGui::BeginTable("##loop", 3, ImGuiTableFlags_None, ImVec2(width, 0.0f)))
    {
        return;
    }

    ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthFixed, Px(kLabelWidth));
    ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthFixed, Px(kBoxWidth));
    ImGui::TableSetupColumn("##after", ImGuiTableColumnFlags_WidthStretch);
    const WaveValues values = state.held ? *state.held : view.values;

    // The loop's switch. A loop that comes on starts where the loop that went off started, or else at the item's start.
    FieldLabel("Loop", kLoopTip);
    const std::array<std::string, 2> kLabels = {"On", "Off"};
    std::array<uint8_t, 2> lit{};
    lit[values.loop ? 0 : 1] = 1;
    const int clicked = SegmentedSwitch("##loop", kLabels, lit, ImVec2(Px(kBoxWidth), ImGui::GetFrameHeight()));
    if (clicked >= 0 && (clicked == 0) != values.loop)
    {
        if (view.values.loop)
        {
            state.loop_start_off = view.values.loop_start;
        }
        edit.values = WithLoopSwitched(view, clicked == 0, state.loop_start_off);
        edit.name = "Change Loop";
    }

    // The loop's start and its end, typed. A typed start moves to the place that the encoding allows.
    FieldLabel("Loop start", kLoopStartTip);
    const std::optional<std::string> typed_start = NumberBox("##start", values.loop_start, state, values.loop);
    NoteCell(TimeOf(values.loop_start, values.rate));
    const std::optional<uint32_t> start =
        typed_start && values.loop ? NumberFrom(*typed_start, 0, values.loop_end - 1,
                                                "The loop's start is a whole number of samples", edit.refused)
                                   : std::nullopt;
    if (start)
    {
        const uint32_t step = LoopStep(values.encoding, view.stream);
        const std::optional<uint32_t> place =
            LoopMarkerPlace(view.samples, *start, step, 0, values.loop_end - 1, false, 0.0);
        if (place && *place != values.loop_start)
        {
            WaveValues changed = view.values;
            changed.loop_start = *place;
            edit.values = changed;
            edit.name = "Change Loop Start";
        }
    }

    // Without a loop, the end is where the wave stops. It takes input either way.
    const bool ends_loop = !view.stream && values.loop;
    FieldLabel(ends_loop ? "Loop end" : "End", view.stream ? kStreamEndTip : (ends_loop ? kLoopEndTip : kEndTip));
    const std::optional<std::string> typed_end = NumberBox("##end", values.loop_end, state, true);
    NoteCell(TimeOf(values.loop_end, values.rate));
    const uint32_t least = values.loop ? values.loop_start + 1 : 1;
    const std::string end_text =
        ends_loop ? "The loop's end is a whole number of samples" : "The end is a whole number of samples";
    const std::optional<uint32_t> end =
        typed_end ? NumberFrom(*typed_end, least, view.held, end_text, edit.refused) : std::nullopt;
    if (end && *end != values.loop_end)
    {
        WaveValues changed = view.values;
        changed.loop_end = *end;
        edit.values = changed;
        edit.name = EndEditName(view);
    }

    // The samples that the item holds.
    FieldLabel("Samples", "The samples that each channel holds. The end can move over all of them.");
    ImGui::AlignTextToFramePadding();
    ImGui::Text("%u", view.held);
    NoteCell(TimeOf(view.held, values.rate));
    ImGui::EndTable();
}

// The fields of the format: the rate, the encoding and the channels, and a stream's tracks and regions. A value takes
// the rest of the table's width.
void DrawFormatFields(const WaveView& view, WaveWindowState& state, float width, WaveEdit& edit)
{
    if (!ImGui::BeginTable("##format", 2, ImGuiTableFlags_None, ImVec2(width, 0.0f)))
    {
        return;
    }

    ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthFixed, Px(kLabelWidth));
    ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch);
    const WaveValues& values = view.values;
    const ImVec4 kMuted = ImGui::ColorConvertU32ToFloat4(theme::kTextMuted);

    // The rate, typed.
    FieldLabel("Rate", kRateTip);
    const std::optional<std::string> typed_rate = NumberBox("##rate", values.rate, state, true);
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(kMuted, "Hz");
    const std::optional<uint32_t> rate =
        typed_rate ? NumberFrom(*typed_rate, 1, kHighestRate, "The rate is a whole number of hertz", edit.refused)
                   : std::nullopt;
    if (rate && *rate != values.rate)
    {
        WaveValues changed = values;
        changed.rate = *rate;
        edit.values = changed;
        edit.name = "Change Rate";
    }

    // The encoding, from the encodings that the item can take. Each switch is as wide as the longest name.
    FieldLabel("Encoding", kEncodingTip);
    std::vector<std::string> names;
    std::vector<uint8_t> lit;
    float widest = 0.0f;
    for (const sar::WaveEncoding encoding : view.encodings)
    {
        names.emplace_back(EncodingName(encoding));
        lit.push_back(encoding == values.encoding ? 1 : 0);
        widest = std::max(widest, TextSize(theme::GetFonts().figures, theme::kTextSize, names.back()).x);
    }
    const float across = (widest + ImGui::GetStyle().FramePadding.x * 2.0f) * static_cast<float>(names.size());
    const float switch_width = std::min(across + Px(4.0f), ImGui::GetContentRegionAvail().x);
    const int clicked = SegmentedSwitch("##encoding", names, lit, ImVec2(switch_width, ImGui::GetFrameHeight()));
    if (clicked >= 0 && view.encodings[static_cast<std::size_t>(clicked)] != values.encoding)
    {
        WaveValues changed = values;
        changed.encoding = view.encodings[static_cast<std::size_t>(clicked)];
        edit.values = changed;
        edit.name = "Change Encoding";
    }

    // The channels, and a stream's tracks and regions. A track's line wraps at the table's edge.
    FieldLabel("Channels", "The channels that the item holds.");
    ImGui::AlignTextToFramePadding();
    ImGui::Text("%zu", view.samples.size());
    if (view.stream)
    {
        FieldLabel("Tracks", kTracksTip);
        ImGui::AlignTextToFramePadding();
        ImGui::PushTextWrapPos(0.0f);
        for (const std::string& track : view.tracks)
        {
            ImGui::TextUnformatted(track.c_str());
        }
        if (view.tracks.empty())
        {
            ImGui::TextColored(kMuted, "None");
        }
        ImGui::PopTextWrapPos();
        FieldLabel("Regions", kRegionsTip);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(view.regions.c_str());
    }
    ImGui::EndTable();
}

} // namespace

WaveEdit DrawWaveWindow(const WaveView& view, WaveWindowState& state, bool busy, bool* open)
{
    // A window over the middle of the main window, in the style of the Bank Editor. The user can make it larger.
    const std::string title =
        std::string(view.stream ? "Stream Properties - " : "Wave Properties - ") + view.name + "###WaveProperties";
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(Px(kWindowWidth), Px(kWindowHeight)), ImGuiCond_Appearing);
    ImGui::SetNextWindowSizeConstraints(ImVec2(Px(kLeastWidth), Px(kLeastHeight)), ImVec2(FLT_MAX, FLT_MAX));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, Px(theme::kDialogPadding));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::ColorConvertU32ToFloat4(theme::kRaised));
    ImGui::PushStyleColor(ImGuiCol_Border, ImGui::ColorConvertU32ToFloat4(theme::kBorderStrong));
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking |
                                        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar |
                                        ImGuiWindowFlags_NoScrollWithMouse;
    const bool visible = ImGui::Begin(title.c_str(), open, kFlags);
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar();
    WaveEdit edit;
    if (!visible)
    {
        ImGui::End();
        return edit;
    }

    // Escape closes the window while it has the focus. Project Setup does the same.
    if (EscapeCloses())
    {
        *open = false;
    }

    // The buttons across the top: Play or Stop, Replace… and Export…, and the snap on the right.
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 room = ImGui::GetContentRegionAvail();
    const ImVec2 button(Px(kButtonHeight), Px(kButtonHeight));
    const char* play_tip = state.playing ? "Stop" : "Play the wave from its start, then round its loop until Stop";
    if (IconButton("##play", state.playing ? Icon::kStop : Icon::kPlay, button, state.playing, theme::kPlay, play_tip))
    {
        (state.playing ? edit.stop : edit.play) = true;
    }
    ImGui::SameLine(0.0f, Px(kGap));
    ImGui::BeginDisabled(busy);
    if (ImGui::Button("Replace…", ImVec2(0.0f, Px(kButtonHeight))))
    {
        edit.replace = true;
    }
    ItemTooltip(std::string("Puts a WAV file or a ") + (view.stream ? "stream" : "wave") +
                " file in this one's place.");
    ImGui::SameLine(0.0f, Px(kGap) * 0.5f);
    if (ImGui::Button("Export…", ImVec2(0.0f, Px(kButtonHeight))))
    {
        edit.export_item = true;
    }
    ItemTooltip(std::string("Writes the ") + (view.stream ? "stream" : "wave") + " as its file or as a WAV file.");
    ImGui::EndDisabled();
    ImGui::SameLine(origin.x + room.x - button.x - ImGui::GetWindowPos().x);
    if (IconButton("##snap", Icon::kSnap, button, state.snap, theme::kAccent,
                   "Snap the loop's markers to zero crossings"))
    {
        state.snap = !state.snap;
    }

    // The waveform, then the fields in two tables side by side, then the facts across the bottom.
    const float waveform_top = origin.y + Px(kButtonHeight) + Px(kGap);
    DrawWaveform(view, state, busy, ImVec2(origin.x, waveform_top), ImVec2(room.x, Px(kWaveformHeight)), edit);
    const float fields_top = waveform_top + Px(kWaveformHeight) + Px(kGap) * 1.5f;
    const float half = (room.x - Px(kGap) * 2.0f) * 0.5f;
    ImGui::BeginDisabled(busy);
    ImGui::SetCursorScreenPos(ImVec2(origin.x, fields_top));
    DrawLoopFields(view, state, half, edit);
    const float loop_bottom = ImGui::GetCursorScreenPos().y;
    ImGui::SetCursorScreenPos(ImVec2(origin.x + half + Px(kGap) * 2.0f, fields_top));
    DrawFormatFields(view, state, half, edit);
    const float format_bottom = ImGui::GetCursorScreenPos().y;
    ImGui::EndDisabled();
    const float facts_top = std::max(loop_bottom, format_bottom) + Px(kGap);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddLine(ImVec2(origin.x, facts_top), ImVec2(origin.x + room.x, facts_top), theme::kBorder, Px(1.0f));

    // The facts take the rest of the window, and scroll where they don't fit.
    const float facts_height =
        std::max(origin.y + room.y - facts_top - Px(kGap), ImGui::GetTextLineHeightWithSpacing() * 2.0f);
    ImGui::SetCursorScreenPos(ImVec2(origin.x, facts_top + Px(kGap)));
    if (std::exchange(state.top, false))
    {
        ImGui::SetNextWindowScroll(ImVec2(0.0f, 0.0f));
    }
    ImGui::BeginChild("##facts", ImVec2(room.x, facts_height));
    ImGui::PushTextWrapPos(0.0f);
    const ImVec4 kMuted = ImGui::ColorConvertU32ToFloat4(theme::kTextMuted);
    ImGui::TextColored(kMuted, "%s", view.reach.c_str());
    for (const std::string& line : view.sizes)
    {
        ImGui::TextColored(kMuted, "%s", line.c_str());
    }
    ImGui::PopTextWrapPos();
    ImGui::EndChild();
    ImGui::End();

    // A busy window takes no edits. Play and Stop still work.
    if (busy)
    {
        edit.values.reset();
        edit.replace = false;
        edit.export_item = false;
    }

    return edit;
}

} // namespace hokuto::app
