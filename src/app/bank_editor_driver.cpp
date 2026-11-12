// SPDX-License-Identifier: MIT

// The Bank Editor for the samples of a ROM with another of the GBA's drivers, such as Konami's driver: the list of
// samples, and the rate and loop of the selected one.

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "app/bank_editor.h"
#include "app/bank_editor_parts.h"
#include "app/item_views.h"
#include "app/theme.h"
#include "app/time_text.h"
#include "app/widgets.h"
#include "edit/konami_samples.h"

namespace hokuto::app
{
namespace
{

using bank_editor_parts::BankEditorLayout;
using bank_editor_parts::BankLayoutOf;
using bank_editor_parts::BeginBankEditorWindow;
using bank_editor_parts::BeginFacts;
using bank_editor_parts::DrawFactsDivider;
using bank_editor_parts::DrawKeyboard;
using bank_editor_parts::DrawListDivider;
using bank_editor_parts::FactLine;
using bank_editor_parts::FieldLabel;
using bank_editor_parts::ForgetDropTargets;
using bank_editor_parts::kGap;
using bank_editor_parts::kKeyboardHeight;
using bank_editor_parts::kLabelWidth;
using bank_editor_parts::kListWidth;
using bank_editor_parts::kRowHeight;
using bank_editor_parts::SwitchRow;
using bank_editor_parts::TypedField;
using bank_editor_parts::TypedItemOf;
using theme::Px;

// The list of the samples of the GBA's other drivers at `pos`, `size` large. A click selects a sample.
void DrawDriverSamples(const DriverBankView& view, BankEditorState& state, ImVec2 pos, ImVec2 size)
{
    ImDrawList* draw = ImGui::GetWindowDrawList();
    DrawCaps(draw, pos, theme::kTextMuted, "SAMPLES");
    const float caps = CapsSize("SAMPLES").y + Px(6.0f);
    const ImVec2 list_min(pos.x, pos.y + caps);
    const ImVec2 list_size(size.x, std::max(size.y - caps, Px(kRowHeight)));
    ImGui::SetCursorScreenPos(list_min);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::ColorConvertU32ToFloat4(theme::kPanel));
    const bool reveal = std::exchange(state.reveal, false);
    if (reveal)
    {
        ImGui::SetNextWindowScroll(ImVec2(0.0f, 0.0f));
    }
    ImGui::BeginChild("##samples", list_size, ImGuiChildFlags_Borders);
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    ImFont* text = theme::GetFonts().text;
    ImFont* figures = theme::GetFonts().figures;
    const float column = TextSize(figures, theme::kTextSize, "00000").x + Px(16.0f);
    const auto revealed = reveal && state.program
                              ? std::ranges::find(view.samples, *state.program, &DriverSampleView::sample)
                              : view.samples.end();
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(view.samples.size()));
    if (revealed != view.samples.end())
    {
        clipper.IncludeItemByIndex(static_cast<int>(revealed - view.samples.begin()));
    }
    while (clipper.Step())
    {
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row)
        {
            const DriverSampleView& sample = view.samples[static_cast<std::size_t>(row)];
            std::array<char, 16> id{};
            std::snprintf(id.data(), id.size(), "##%u", sample.sample);
            const float left = ImGui::GetCursorScreenPos().x;
            if (ImGui::Selectable(id.data(), state.program == sample.sample, 0,
                                  ImVec2(0.0f, Px(kRowHeight) - Px(6.0f))))
            {
                state.program = sample.sample;
            }
            if (revealed != view.samples.end() && sample.sample == revealed->sample)
            {
                ImGui::SetScrollHereY(0.5f);
            }
            std::array<char, 8> number{};
            std::snprintf(number.data(), number.size(), "%04u", sample.sample);
            const std::string rate = std::to_string(sample.rate) + " Hz";
            const float top = (ImGui::GetItemRectMin().y + ImGui::GetItemRectMax().y) * 0.5f -
                              TextSize(text, theme::kTextSize, rate).y * 0.5f;
            DrawText(draw_list, figures, theme::kTextSize, ImVec2(left, top), theme::kTextMuted, number.data());
            DrawText(draw_list, text, theme::kTextSize, ImVec2(left + column, top), theme::kText, rate);
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

constexpr char kSampleLoopTip[] = "Whether the sample loops. The loop runs from its start to the end of the sample.";
constexpr char kSampleLoopStartTip[] = "First sample of the loop.";

// A field for a whole number that shows `shown` until the user types. Enter returns the number that the user typed,
// with any commas left out. Other text is refused with `refusal`. While `busy`, the number that the user typed waits in
// `waiting` for `item`.
std::optional<uint32_t> NumberField(const char* id, std::array<char, 16>& text, bool& typing, uint32_t shown, bool busy,
                                    WaitingValues<std::string>& waiting, const ShownItem& item, BankEdit& edit,
                                    const std::string& refusal)
{
    const std::optional<std::string> ended = TypedField(id, text, typing, std::to_string(shown), busy, waiting, item);
    if (!ended)
    {
        return std::nullopt;
    }

    typing = false;
    std::string digits;
    for (const char c : *ended)
    {
        if (c != ',')
        {
            digits += c;
        }
    }
    const std::optional<uint64_t> value = WholeNumber(digits);
    if (!value || *value > std::numeric_limits<uint32_t>::max())
    {
        edit.refused = refusal;
        return std::nullopt;
    }

    return static_cast<uint32_t>(*value);
}

// Sets `edit` to change sample `sample` of a driver to `values`, under the name `name`.
void ChangeDriverSample(const DriverSampleView& sample, std::string name, edit::KonamiSampleValues values,
                        BankEdit& edit)
{
    edit.konami_sample = std::pair(static_cast<int>(sample.sample), values);
    edit.name = std::move(name);
    edit.details = "Sample " + std::to_string(sample.sample);
}

// The facts of a sample that can't be edited.
void DrawDriverSampleReadOnly(const DriverSampleView& sample)
{
    ImGui::Text("Rate: %s Hz at semitone 0", Grouped(static_cast<uint64_t>(std::max(sample.rate, 0))).c_str());
    ImGui::Text("Length: %s samples", Grouped(sample.length).c_str());
    if (sample.loop_start)
    {
        ImGui::Text("Loop: from sample %s to the end", Grouped(*sample.loop_start).c_str());
    }
    else
    {
        ImGui::TextUnformatted("Loop: none. The sample plays once.");
    }
}

// The row of the rate of an editable sample.
void DrawDriverSampleRate(const DriverSampleView& sample, BankEditorState& state, const ShownItem& item, bool busy,
                          BankEdit& edit)
{
    const edit::KonamiSampleValues held{static_cast<uint32_t>(std::max(sample.rate, 0)), sample.loop_start};
    const std::string rate_range = Grouped(sample.least_rate) + "–" + Grouped(sample.most_rate) + " Hz";
    const std::string rate_tip =
        "Playback sample rate at semitone 0, " + rate_range +
        ". Changing it changes the pitch and length but keeps the samples. The driver plays only some rates. A "
        "typed rate changes to the nearest of them." +
        (sample.every_pitch ? ""
                            : " Notes take this rate only at the few pitches that the driver's period table "
                              "leaves empty.");
    FieldLabel("Rate (Hz)", rate_tip.c_str());
    const std::string rate_refusal = "The rate must be " + rate_range + ".";
    if (const std::optional<uint32_t> rate = NumberField("##rate", state.typed_rate, state.typing_rate, held.rate, busy,
                                                         state.typed_waiting, item, edit, rate_refusal);
        rate && *rate != held.rate)
    {
        if (*rate < sample.least_rate || *rate > sample.most_rate)
        {
            edit.refused = rate_refusal;
        }
        else
        {
            edit::KonamiSampleValues values = held;
            values.rate = *rate;
            ChangeDriverSample(sample, "Change Sample Rate", values, edit);
        }
    }
}

// The rows of the loop and the loop start of an editable sample.
void DrawDriverSampleLoop(const DriverSampleView& sample, BankEditorState& state, const ShownItem& item, bool busy,
                          BankEdit& edit)
{
    const edit::KonamiSampleValues held{static_cast<uint32_t>(std::max(sample.rate, 0)), sample.loop_start};
    const int loop = SwitchRow("Loop", sample.loops ? kSampleLoopTip : sample.no_loop_reason.c_str(),
                               std::array<std::string, 2>{"Off", "On"}, sample.loop_start ? 1 : 0, !sample.loops);
    if (loop >= 0)
    {
        edit::KonamiSampleValues values = held;
        values.loop_start = loop == 1 ? std::optional<uint32_t>(0) : std::nullopt;
        ChangeDriverSample(sample, loop == 1 ? "Loop Sample" : "Remove Sample Loop", values, edit);
    }

    const bool no_loop = !sample.loop_start;
    FieldLabel("Loop start", kSampleLoopStartTip, no_loop);
    ImGui::BeginDisabled(no_loop);
    const std::string loop_refusal =
        "The loop must start from 0 to " + Grouped(sample.length > 0 ? sample.length - 1 : 0) + ".";
    if (const std::optional<uint32_t> start =
            NumberField("##loop-start", state.typed_loop, state.typing_loop, sample.loop_start.value_or(0), busy,
                        state.typed_waiting, item, edit, loop_refusal);
        start && !no_loop && *start != *sample.loop_start)
    {
        if (*start >= sample.length)
        {
            edit.refused = loop_refusal;
        }
        else
        {
            edit::KonamiSampleValues values = held;
            values.loop_start = *start;
            ChangeDriverSample(sample, "Move Sample Loop", values, edit);
        }
    }
    ImGui::EndDisabled();
}

// The lines that name the songs that use the sample, and the other items that its edits change. Until the songs are
// read, a line says so. Songs that can't be read give no line.
void DrawDriverSampleUsers(const DriverSampleView& sample)
{
    if (sample.songs_state == DriverSongsState::kReading)
    {
        MutedText("Reading songs…");
    }
    else if (sample.songs_state == DriverSongsState::kRead && sample.songs.empty())
    {
        MutedText("No song uses this sample.");
    }
    else if (sample.songs_state == DriverSongsState::kRead)
    {
        FactLine("Used by " + std::to_string(sample.songs.size()) + (sample.songs.size() == 1 ? " song: " : " songs: "),
                 sample.songs);
    }
    if (!sample.rate_songs.empty())
    {
        FactLine("Rate edits also change " + std::to_string(sample.rate_songs.size()) +
                     (sample.rate_songs.size() == 1 ? " song: " : " songs: "),
                 sample.rate_songs);
    }
    if (!sample.sharing.empty())
    {
        FactLine("Edits also change: ", sample.sharing);
    }
}

// The facts of sample `sample` of one of the GBA's other drivers at `pos`, `size` large: its rate, its length, its loop
// and its songs. An editable sample accepts a typed rate, a loop and a typed loop start. A value typed while busy waits
// for `item`.
void DrawDriverSampleFacts(const DriverSampleView& sample, BankEditorState& state, const ShownItem& item, bool busy,
                           ImVec2 pos, ImVec2 size, BankEdit& edit)
{
    ImGui::SetCursorScreenPos(pos);
    ImGui::BeginChild("##sample", size);
    ImGui::PushTextWrapPos(0.0f);
    if (!sample.editable)
    {
        DrawDriverSampleReadOnly(sample);
    }
    else if (ImGui::BeginTable("##driver-sample", 2, ImGuiTableFlags_None,
                               ImVec2(std::min(size.x, Px(kLabelWidth) + Px(240.0f)), 0.0f)))
    {
        ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthFixed, Px(kLabelWidth));
        ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch);
        ImGui::BeginDisabled(busy);
        DrawDriverSampleRate(sample, state, item, busy, edit);
        DrawDriverSampleLoop(sample, state, item, busy, edit);
        ImGui::EndDisabled();

        FieldLabel("Length", "Length of the sample.");
        ImGui::Text("%s samples", Grouped(sample.length).c_str());
        ImGui::EndTable();
    }

    DrawDriverSampleUsers(sample);
    ImGui::PopTextWrapPos();
    ImGui::EndChild();
}

} // namespace

BankEdit DrawDriverBankEditor(const DriverBankView& view, BankEditorState& state, const ShownItem& bank, bool busy,
                              bool* open)
{
    BankEdit edit;
    // The list of the samples on the left, then the selected sample's facts and the keyboard. The note across the
    // bottom says what the window leaves out.
    const std::string title = "Bank Editor - Samples###BankEditor";
    ForgetDropTargets(state);
    if (!BeginBankEditorWindow(title, state, open))
    {
        return edit;
    }

    const BankEditorLayout layout = BankLayoutOf(0, "SAMPLE");
    DrawDriverSamples(view, state, layout.origin,
                      ImVec2(Px(kListWidth), layout.facts_top - layout.origin.y - Px(kGap)));
    ImDrawList* draw = ImGui::GetWindowDrawList();
    DrawListDivider(draw, layout);

    if (!state.program && !view.samples.empty())
    {
        state.program = view.samples.front().sample;
    }
    const auto found =
        state.program ? std::ranges::find(view.samples, *state.program, &DriverSampleView::sample) : view.samples.end();
    const ImVec2 rows_min(layout.right, layout.rows_top);
    const ImVec2 rows_max(layout.right + layout.right_width, layout.keyboard_top - Px(kGap));
    if (found != view.samples.end())
    {
        DrawCaps(draw, ImVec2(layout.right, layout.origin.y), theme::kTextMuted,
                 "Sample " + std::to_string(found->sample));
        DrawDriverSampleFacts(*found, state, TypedItemOf(bank, found->sample), busy, rows_min,
                              ImVec2(rows_max.x - rows_min.x, rows_max.y - rows_min.y), edit);
    }
    else
    {
        EmptyState(draw, rows_min, rows_max, "No samples", "No song in the ROM plays a sample.");
    }
    DrawKeyboard(std::nullopt, 60, state, ImVec2(layout.right, layout.keyboard_top),
                 ImVec2(layout.right + layout.right_width, layout.keyboard_top + Px(kKeyboardHeight)));
    if (found == view.samples.end())
    {
        state.sounding = -1;
    }

    DrawFactsDivider(draw, layout);
    BeginFacts(ImVec2(layout.origin.x, layout.facts_top),
               ImVec2(layout.room.x, layout.origin.y + layout.room.y - layout.facts_top), false);
    const bool editable = std::ranges::any_of(view.samples, &DriverSampleView::editable);
    MutedText(editable ? "To change the sample of a note, use the Program menu in the inspector. To replace a "
                         "sample, choose Replace… on it in the browser's Waves."
                       : "Hokuto can show and play these samples but doesn't support editing them yet. To "
                         "change the sample of a note, use the Program menu in the inspector.");
    ImGui::PopTextWrapPos();
    ImGui::EndChild();
    ImGui::End();

    return edit;
}

} // namespace hokuto::app
