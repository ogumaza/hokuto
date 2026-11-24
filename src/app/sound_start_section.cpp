// SPDX-License-Identifier: MIT

// The Start rows of the Sound section: what a game sets through a sequence sound's handle as it starts the sound
// (format::SoundStart). Each row has a box that turns its value on or off, and a row without a value shows none.

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/sar/formats.h"

#include "app/sound_section_parts.h"
#include "app/theme.h"
#include "app/widgets.h"

namespace hokuto::app::sound_section_parts
{
namespace
{

using theme::Px;

// The rows of the start, by their places in the start's values.
enum class StartRow : int
{
    kSpeed,
    kTranspose,
    kFineTune,
    kGain,
    kSendA,
    kSendB,
    kLowPass,
};

// The labels of the rows, by StartRow, then the banks' label.
constexpr std::array<const char*, 8> kStartLabels = {
    "Speed", "Transpose", "Fine Tune", "Gain", "Send A", "Send B", "Low-Pass", "Banks",
};

// What each row sets, for the tooltips of the labels.
constexpr std::array<const char*, 8> kStartTips = {
    "Tempo ratio that the game sets when it starts the sound. 100% plays the sequence tempo.",
    "Pitch shift in semitones that the game sets when it starts the sound. Shares its box with Fine Tune.",
    "Pitch shift in cents that the game sets when it starts the sound. Shares its box with Transpose.",
    "Volume that the game sets when it starts the sound, on top of the sound volume.",
    "Send to aux bus A that the game sets when it starts the sound. Shares its box with Send B.",
    "Send to aux bus B that the game sets when it starts the sound. Shares its box with Send A.",
    "Low-pass that the game sets when it starts the sound, from -1 to 0. It adds to each track's low-pass.",
    "Banks that the game gives the bank slots when it starts the sound, in place of the sound's banks.",
};

// The gap between a row's box and its slider, and the box's width with its gap.
constexpr float kBoxGap = 8.0f;
constexpr float kBox = 18.0f;

// The lowest gain that the Gain slider shows, in dB. Below it, the start's volume is 0.
constexpr float kLowestGain = -60.0f;

// A pitch, a ratio of frequencies, in cents, and back. A pitch of 0 counts as the lowest that the rows show.
double CentsOf(float pitch)
{
    constexpr double kLowestPitch = 1.0 / 16.0;
    return 1200.0 * std::log2(std::max(static_cast<double>(pitch), kLowestPitch));
}

float PitchOf(double cents)
{
    return static_cast<float>(std::exp2(cents / 1200.0));
}

// The pitch of `start` split into whole semitones and the cents left over, from -50 to 50.
std::pair<double, double> TransposeOf(const format::SoundStart& start)
{
    const double cents = CentsOf(start.pitch.value_or(1.0f));
    const double semitones = std::round(cents / 100.0);

    return {semitones, cents - semitones * 100.0};
}

// The value that row `row` of `start` shows on its slider, and whether the start sets it.
std::pair<float, bool> RowValue(const format::SoundStart& start, StartRow row)
{
    switch (row)
    {
    case StartRow::kSpeed:
        return {start.tempo_ratio.value_or(1.0f) * 100.0f, start.tempo_ratio.has_value()};

    case StartRow::kTranspose:
        return {static_cast<float>(TransposeOf(start).first), start.pitch.has_value()};

    case StartRow::kFineTune:
        return {static_cast<float>(TransposeOf(start).second), start.pitch.has_value()};

    case StartRow::kGain:
        return {std::max(DecibelsOfGain(start.volume.value_or(1.0f)), kLowestGain), start.volume.has_value()};

    case StartRow::kSendA:
    case StartRow::kSendB:
        {
            const std::size_t bus = row == StartRow::kSendA ? 0 : 1;
            return {start.sends ? (*start.sends)[bus] : 0.0f, start.sends.has_value()};
        }

    case StartRow::kLowPass:
        return {start.low_pass.value_or(0.0f), start.low_pass.has_value()};
    }

    return {0.0f, false};
}

// `start` with row `row` at `value`, the value that the row's slider shows.
format::SoundStart WithRowValue(format::SoundStart start, StartRow row, float value)
{
    switch (row)
    {
    case StartRow::kSpeed:
        start.tempo_ratio = value / 100.0f;
        break;

    case StartRow::kTranspose:
        start.pitch = PitchOf(std::round(value) * 100.0 + TransposeOf(start).second);
        break;

    case StartRow::kFineTune:
        start.pitch = PitchOf(TransposeOf(start).first * 100.0 + value);
        break;

    case StartRow::kGain:
        start.volume = value <= kLowestGain ? 0.0f : GainOfDecibels(value);
        break;

    case StartRow::kSendA:
    case StartRow::kSendB:
        {
            format::StartSends sends = start.sends.value_or(format::StartSends{});
            sends[row == StartRow::kSendA ? 0 : 1] = std::clamp(value, 0.0f, 1.0f);
            start.sends = sends;
            break;
        }

    case StartRow::kLowPass:
        start.low_pass = std::clamp(value, format::kLowestStartLowPass, 0.0f);
        break;
    }

    return start;
}

// `start` with the value of row `row` set to what the sound plays without it, or cleared.
format::SoundStart WithRowSet(format::SoundStart start, StartRow row, bool set)
{
    switch (row)
    {
    case StartRow::kSpeed:
        start.tempo_ratio = set ? std::optional<float>(1.0f) : std::nullopt;
        break;

    case StartRow::kTranspose:
    case StartRow::kFineTune:
        start.pitch = set ? std::optional<float>(1.0f) : std::nullopt;
        break;

    case StartRow::kGain:
        start.volume = set ? std::optional<float>(1.0f) : std::nullopt;
        break;

    case StartRow::kSendA:
    case StartRow::kSendB:
        start.sends = set ? std::optional<format::StartSends>(format::StartSends{}) : std::nullopt;
        break;

    case StartRow::kLowPass:
        start.low_pass = set ? std::optional<float>(0.0f) : std::nullopt;
        break;
    }

    return start;
}

// The slider's range, its reset and the text of its value.
struct RowSlider
{
    SliderRange range;
    float reset = 0.0f;
    std::string format;
};

RowSlider SliderOf(StartRow row, float value)
{
    switch (row)
    {
    case StartRow::kSpeed:
        return {{50.0f, 200.0f, true}, 100.0f, "%.1f%%"};

    case StartRow::kTranspose:
        return {{-36.0f, 36.0f}, 0.0f, "%+.0f st"};

    case StartRow::kFineTune:
        return {{-50.0f, 50.0f}, 0.0f, "%+.1f ct"};

    case StartRow::kGain:
        return {{kLowestGain, kFaderTop}, 0.0f, value <= kLowestGain ? std::string("-\xE2\x88\x9E dB") : "%+.1f dB"};

    case StartRow::kLowPass:
        return {{format::kLowestStartLowPass, 0.0f}, 0.0f, "%+.2f"};

    case StartRow::kSendA:
    case StartRow::kSendB:
        break;
    }

    return {{0.0f, 1.0f}, 0.0f, "%.2f"};
}

// The box of a row at `control`, lit while the start sets the row's value. Returns the place left for the row's
// slider, and sets `clicked` when the user clicks the box.
Control Box(Control control, const char* id, bool on, bool& clicked)
{
    ImGui::SetCursorScreenPos(control.pos);
    bool shown = on;
    clicked = CheckBox(id, &shown);
    const float used = Px(kBox + kBoxGap);

    return {ImVec2(control.pos.x + used, control.pos.y), std::max(control.width - used, 0.0f)};
}

// A row of the start with its box and its slider. Returns the start after the change that the user made.
std::optional<StartChange> DrawStartRow(Rows& rows, SoundSection& section, StartRow row)
{
    const format::SoundStart& start = *section.view->start;
    const auto index = static_cast<std::size_t>(row);
    const Control control = Label(rows, kStartLabels[index], kStartTips[index]);
    const auto [current, set] = RowValue(start, row);
    ImGui::PushID(static_cast<int>(index));

    bool clicked = false;
    const Control slider = Box(control, "##set", set, clicked);
    std::optional<StartChange> change;
    if (clicked)
    {
        change = StartChange{WithRowSet(start, row, !set),
                             std::string(set ? "Clear Start " : "Set Start ") + kStartLabels[index]};
    }

    // An unset row shows its slider empty, where the sound plays without the value.
    const bool holding = section.start_held && section.start_held->first == static_cast<int>(index);
    float shown = holding ? section.start_held->second : current;
    const RowSlider kind = SliderOf(row, shown);
    ImGui::BeginDisabled(!set);
    ImGui::SetCursorScreenPos(slider.pos);
    if (ValueSlider("##slider", &shown, kind.range, kind.reset, set ? kind.format.c_str() : "",
                    ImVec2(slider.width, rows.control)))
    {
        section.start_held = std::pair(static_cast<int>(index), shown);
    }
    const bool released = SliderReleased();
    ImGui::EndDisabled();
    ImGui::PopID();
    if (released && holding)
    {
        const float value = std::exchange(section.start_held, std::nullopt)->second;
        change = StartChange{WithRowValue(start, row, value), std::string("Change Start ") + kStartLabels[index]};
    }

    return change;
}

// The Banks row: its box, then a menu of the archive's banks for each of the four slots, on lines of their own.
std::optional<StartChange> DrawStartBanks(Rows& rows, SoundSection& section)
{
    const SoundView& view = *section.view;
    const format::SoundStart& start = *view.start;
    constexpr std::size_t kBanks = std::tuple_size_v<format::StartBanks>;
    const Control control = Label(rows, kStartLabels.back(), kStartTips.back());
    std::optional<StartChange> change;
    ImGui::PushID("banks");

    // A start without banks plays the sound's banks. The box gives the start those banks to begin with.
    bool clicked = false;
    Box(control, "##set", start.banks.has_value(), clicked);
    if (clicked)
    {
        format::SoundStart changed = start;
        if (start.banks)
        {
            changed.banks.reset();
        }
        else
        {
            format::StartBanks banks{};
            for (std::size_t slot = 0; slot < kBanks; ++slot)
            {
                banks[slot] = edit::ValueOf(view.values, edit::SoundValue::kBank, static_cast<int>(slot));
            }
            changed.banks = banks;
        }
        change = StartChange{std::move(changed), start.banks ? "Clear Start Banks" : "Set Start Banks"};
    }

    ImGui::BeginDisabled(!start.banks);
    for (std::size_t slot = 0; slot < kBanks; ++slot)
    {
        ImGui::PushID(static_cast<int>(slot));
        const std::string label = std::to_string(slot);
        const Control menu = Label(rows, label.c_str(), kStartTips.back(), Px(kSlotLabelWidth));
        const uint32_t bank = start.banks ? (*start.banks)[slot] : format::kNoStartBank;
        const bool listed =
            ntdlib::sar::ItemType(bank) == ntdlib::sar::kBankItem && ntdlib::sar::ItemNumber(bank) < view.banks.size();
        int chosen = listed ? static_cast<int>(ntdlib::sar::ItemNumber(bank)) : -1;
        ImGui::SetCursorScreenPos(menu.pos);
        ImGui::SetNextWindowSizeConstraints(ImVec2(Px(kListWidth), 0.0f), ImVec2(FLT_MAX, FLT_MAX));
        if (FilteredCombo("##bank", view.banks, chosen, "None", section.filter, menu.width) && start.banks)
        {
            format::SoundStart changed = start;
            (*changed.banks)[slot] = chosen < 0
                                         ? format::kNoStartBank
                                         : ntdlib::sar::ItemId(ntdlib::sar::kBankItem, static_cast<uint32_t>(chosen));
            change = StartChange{std::move(changed), "Change Start Bank"};
        }
        ImGui::PopID();
    }
    ImGui::EndDisabled();
    ImGui::PopID();

    return change;
}

} // namespace

std::vector<const char*> StartRowLabels()
{
    return {kStartLabels.begin(), kStartLabels.end()};
}

std::optional<StartChange> DrawStart(Rows& rows, SoundSection& section)
{
    Heading(rows, "Start");
    std::optional<StartChange> change;
    ImGui::PushID("start");
    for (const StartRow row : {StartRow::kSpeed, StartRow::kTranspose, StartRow::kFineTune, StartRow::kGain,
                               StartRow::kSendA, StartRow::kSendB, StartRow::kLowPass})
    {
        if (std::optional<StartChange> made = DrawStartRow(rows, section, row))
        {
            change = std::move(made);
        }
    }
    if (section.view->start_banks)
    {
        if (std::optional<StartChange> made = DrawStartBanks(rows, section))
        {
            change = std::move(made);
        }
    }
    ImGui::PopID();

    return change;
}

} // namespace hokuto::app::sound_section_parts
