// SPDX-License-Identifier: MIT

// The Bank Editor for a GBA voice group: the programs, the rows of a key split or a drum kit, the fields of the
// selected voice with its pattern, and the facts.

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ntdlib/mp2k/song.h"

#include "app/bank_editor.h"
#include "app/bank_editor_parts.h"
#include "app/item_views.h"
#include "app/key_edits.h"
#include "app/theme.h"
#include "app/widgets.h"
#include "edit/gba_voices.h"

namespace hokuto::app
{
namespace
{

using bank_editor_parts::BankEditorLayout;
using bank_editor_parts::BankLayoutOf;
using bank_editor_parts::BeginBankEditorWindow;
using bank_editor_parts::BeginFacts;
using bank_editor_parts::BeginFieldTable;
using bank_editor_parts::DrawFactsDivider;
using bank_editor_parts::DrawKeyboard;
using bank_editor_parts::DrawListDivider;
using bank_editor_parts::DrawProgramList;
using bank_editor_parts::FactLine;
using bank_editor_parts::FieldLabel;
using bank_editor_parts::ForgetDropTargets;
using bank_editor_parts::kFieldPadding;
using bank_editor_parts::kGap;
using bank_editor_parts::kGbaFieldRows;
using bank_editor_parts::kHighest;
using bank_editor_parts::kKeyboardHeight;
using bank_editor_parts::kListWidth;
using bank_editor_parts::kMiddleC;
using bank_editor_parts::kRowHeight;
using bank_editor_parts::NeighbourProgram;
using bank_editor_parts::NumberField;
using bank_editor_parts::PanOf;
using bank_editor_parts::ProgramButtonWidths;
using bank_editor_parts::ProgramDetails;
using bank_editor_parts::SelectedOrFirst;
using bank_editor_parts::SwitchRow;
using bank_editor_parts::TypedItemOf;
using theme::Px;

// The GBA's tooltips. A sample's envelope counts 256ths of full level each frame, and a PSG voice's counts frames for
// each of the PSG's 16 levels.
const char* const kGbaKindTip =
    "Voice type: a sample or one of the four GBA PSG channels. A program can also be a key split, where each key range "
    "plays a voice, or a drum kit, where each key plays a voice. A new type keeps the key and the drum kit pan. A new "
    "sample starts as the first sample in the ROM. A new wave, key split or drum kit uses new data in the free space "
    "of the ROM.";
const char* const kGbaFirstKeyTip = "Lowest key that this row of the key split plays. Overlapping rows shrink to fit.";
const char* const kGbaLastKeyTip = "Highest key that this row of the key split plays. Overlapping rows shrink to fit.";
const char* const kGbaAtKeyTip = "Drum kit key that plays this voice.";
const char* const kGbaSampleTip =
    "Sample that the voice plays, from the samples that voices in the ROM use. Drag a WAV file onto the voice to add "
    "it as a new sample in the free space of the ROM.";
const char* const kGbaPitchTip =
    "Whether the sample follows the note key or always plays at the mixer rate. Drums often use a fixed pitch.";
const char* const kGbaRateTip = "Sample rate at key 60, length, and whether the sample loops.";
const char* const kGbaDutyTip = "Duty cycle: the part of each square wave cycle that is high.";
const char* const kGbaSweepTimeTip =
    "How often the sweep changes the pitch of square 1, in 128ths of a second. 0 turns the sweep off.";
const char* const kGbaSweepTip = "Whether the sweep raises or lowers the pitch of square 1.";
const char* const kGbaSweepShiftTip =
    "Size of each sweep step: the frequency changes by itself divided by 2 to the power of the shift.";
const char* const kGbaPanSweepTip = "The drum kit pans this voice. The pan byte also turns off the sweep of square 1.";
const char* const kGbaPatternTip =
    "Waveform: 32 steps, each 0–15. Drag to draw the steps. Other voices that use the same pattern change too.";
const char* const kGbaNoiseTip = "15-bit noise hisses. 7-bit noise repeats sooner and sounds metallic.";
const char* const kGbaLengthTip =
    "PSG length counter. Above 0, the PSG ends the note after (64 - length)/256 s, or (256 - length)/256 s on the "
    "wave channel. At 0 the note sounds until it's released.";
const char* const kGbaKeyTip = "Key at which the drum kit plays this voice, regardless of the note key.";
const char* const kGbaPanTip = "Pan at which the drum kit plays this voice. When off, the voice uses the track pan.";
const char* const kGbaSampleAttackTip =
    "Attack: how much the level rises each frame, 0–255. At 255 the note starts at full level.";
const char* const kGbaSampleDecayTip =
    "Decay: the part of the level kept each frame until it reaches the sustain level, in 256ths. At 0 it drops at "
    "once.";
const char* const kGbaSampleSustainTip = "Sustain level while the key is held, 0–255.";
const char* const kGbaSampleReleaseTip =
    "Release: the part of the level kept each frame after the key is released, in 256ths. At 0 the note ends at once.";
const char* const kGbaPsgAttackTip =
    "Attack: frames per step through the 16 PSG levels, 0–7. At 0 the note starts at full level.";
const char* const kGbaPsgDecayTip =
    "Decay: frames per level step down to the sustain level, 0–7. At 0 the level drops at once.";
const char* const kGbaPsgSustainTip = "Sustain level while the key is held, 0–15.";
const char* const kGbaPsgReleaseTip =
    "Release: frames per level step after the key is released, 0–7. At 0 the note ends at once.";

// What each kind of GBA voice plays, for the card above its fields.
std::string GbaKindText(GbaVoiceType type)
{
    switch (type)
    {
    case GbaVoiceType::kSample:
        return "Plays a sample from the ROM through the GBA DirectSound mixer.";
    case GbaVoiceType::kFixedSample:
        return "Plays a sample from the ROM through the GBA DirectSound mixer at a fixed rate, regardless of the key.";
    case GbaVoiceType::kSquare1:
        return "Plays PSG channel 1: a square wave with duty cycle and sweep.";
    case GbaVoiceType::kSquare2:
        return "Plays PSG channel 2: a square wave with duty cycle.";
    case GbaVoiceType::kWave:
        return "Plays PSG channel 3: a 32-step waveform.";
    case GbaVoiceType::kNoise:
        return "Plays PSG channel 4: noise.";
    case GbaVoiceType::kKeySplit:
    case GbaVoiceType::kDrumKit:
    case GbaVoiceType::kUnknown:
        break;
    }

    return "Its type doesn't match any GBA PSG channel.";
}

// The bits of square 1's sweep in a voice's byte: the time in bits 4 to 6, the direction in bit 3 and the shift in
// bits 0 to 2. Bit 7 marks a drum kit's pan. The pan turns the sweep off.
constexpr uint8_t kPanBit = 0x80;
constexpr uint8_t kSweepDown = 0x08;
constexpr int kSweepTimeShift = 4;
constexpr int kSweepPart = 7;

// A drum kit's pan in a voice's byte, with bit 7 set: 0xC0 is the centre.
constexpr int kDrumCentre = 0xC0;

// A field of a GBA voice that contains a number.
using GbaField = NumberField<ntdlib::mp2k::Voice>;

const GbaField kGbaSweepTime = {
    "Sweep time",
    "Sweep Time",
    kGbaSweepTimeTip,
    kSweepPart,
    0,
    [](const ntdlib::mp2k::Voice& voice) { return (voice.pan_sweep >> kSweepTimeShift) & kSweepPart; },
    [](ntdlib::mp2k::Voice& voice, int value)
    {
        voice.pan_sweep = static_cast<uint8_t>((voice.pan_sweep & ~(kPanBit | (kSweepPart << kSweepTimeShift))) |
                                               (value << kSweepTimeShift));
    }};
const GbaField kGbaSweepShift = {
    "Sweep shift",
    "Sweep Shift",
    kGbaSweepShiftTip,
    kSweepPart,
    0,
    [](const ntdlib::mp2k::Voice& voice) { return voice.pan_sweep & kSweepPart; },
    [](ntdlib::mp2k::Voice& voice, int value)
    { voice.pan_sweep = static_cast<uint8_t>((voice.pan_sweep & ~(kPanBit | kSweepPart)) | value); }};
const GbaField kGbaLength = {"Length",
                             "Length",
                             kGbaLengthTip,
                             edit::kHighestPsgLength,
                             0,
                             [](const ntdlib::mp2k::Voice& voice) { return static_cast<int>(voice.length); },
                             [](ntdlib::mp2k::Voice& voice, int value) { voice.length = static_cast<uint8_t>(value); }};
const GbaField kGbaKey = {"Key",
                          "Key",
                          kGbaKeyTip,
                          kHighest,
                          static_cast<int>(kMiddleC),
                          [](const ntdlib::mp2k::Voice& voice) { return static_cast<int>(voice.key); },
                          [](ntdlib::mp2k::Voice& voice, int value) { voice.key = static_cast<uint8_t>(value); }};
const GbaField kGbaPan = {"Pan",
                          "Pan",
                          kGbaPanTip,
                          kHighest,
                          kDrumCentre - kPanBit,
                          [](const ntdlib::mp2k::Voice& voice) { return voice.pan_sweep - kPanBit; },
                          [](ntdlib::mp2k::Voice& voice, int value)
                          { voice.pan_sweep = static_cast<uint8_t>(value + kPanBit); }};

// The envelope's fields of a sample's voice, and of a PSG voice.
constexpr int kHighestByte = 255;
const std::array<GbaField, 4> kGbaSampleEnvelope = {{
    {"Attack", "Attack", kGbaSampleAttackTip, kHighestByte, kHighestByte,
     [](const ntdlib::mp2k::Voice& voice) { return static_cast<int>(voice.attack); },
     [](ntdlib::mp2k::Voice& voice, int value) { voice.attack = static_cast<uint8_t>(value); }},
    {"Decay", "Decay", kGbaSampleDecayTip, kHighestByte, 0,
     [](const ntdlib::mp2k::Voice& voice) { return static_cast<int>(voice.decay); },
     [](ntdlib::mp2k::Voice& voice, int value) { voice.decay = static_cast<uint8_t>(value); }},
    {"Sustain", "Sustain", kGbaSampleSustainTip, kHighestByte, kHighestByte,
     [](const ntdlib::mp2k::Voice& voice) { return static_cast<int>(voice.sustain); },
     [](ntdlib::mp2k::Voice& voice, int value) { voice.sustain = static_cast<uint8_t>(value); }},
    {"Release", "Release", kGbaSampleReleaseTip, kHighestByte, 0,
     [](const ntdlib::mp2k::Voice& voice) { return static_cast<int>(voice.release); },
     [](ntdlib::mp2k::Voice& voice, int value) { voice.release = static_cast<uint8_t>(value); }},
}};
const std::array<GbaField, 4> kGbaPsgEnvelope = {{
    {"Attack", "Attack", kGbaPsgAttackTip, edit::kHighestPsgStep, 0, kGbaSampleEnvelope[0].get,
     kGbaSampleEnvelope[0].set},
    {"Decay", "Decay", kGbaPsgDecayTip, edit::kHighestPsgStep, 0, kGbaSampleEnvelope[1].get, kGbaSampleEnvelope[1].set},
    {"Sustain", "Sustain", kGbaPsgSustainTip, edit::kHighestPsgSustain, edit::kHighestPsgSustain,
     kGbaSampleEnvelope[2].get, kGbaSampleEnvelope[2].set},
    {"Release", "Release", kGbaPsgReleaseTip, edit::kHighestPsgStep, 0, kGbaSampleEnvelope[3].get,
     kGbaSampleEnvelope[3].set},
}};

// The voice that the GBA's fields show: the one that the user holds a slider of, or else `voice`.
const ntdlib::mp2k::Voice& GbaShown(const ntdlib::mp2k::Voice& voice, const BankEditorState& state)
{
    return state.gba_held ? *state.gba_held : voice;
}

// The row of `field` for `voice`, with the value as `format` writes it. ByteRow does the same for a region. With
// `grey_tip`, the row shows grey and takes no edits, and the tip says why.
void GbaRow(const GbaField& field, const ntdlib::mp2k::Voice& voice, const std::string& format, BankEditorState& state,
            bool* released, const char* grey_tip = nullptr)
{
    FieldLabel(field.label, grey_tip != nullptr ? grey_tip : field.tooltip, grey_tip != nullptr);
    ImGui::BeginDisabled(grey_tip != nullptr);
    const bool holding = state.gba_held && state.held_name == field.name;
    ntdlib::mp2k::Voice changed = holding ? *state.gba_held : voice;
    auto shown = static_cast<float>(field.get(changed));
    const auto high = static_cast<float>(field.high);
    const SliderRange typed{0.0f, high};
    ImGui::PushID(field.label);
    if (ValueSlider("##value", &shown, {0.0f, high}, static_cast<float>(field.reset), format.c_str(),
                    ImVec2(ImGui::GetContentRegionAvail().x, ImGui::GetFrameHeight()), &typed))
    {
        field.set(changed, static_cast<int>(std::lround(std::clamp(shown, 0.0f, high))));
        state.gba_held = changed;
        state.held_name = field.name;
    }
    *released = *released || (SliderReleased() && state.gba_held && state.held_name == field.name);
    ImGui::PopID();
    ImGui::EndDisabled();
}

// The steps of a wave's `pattern` in a cell of the fields' table: a bar for each of the 32 steps, from 0 at the bottom
// to 15 at the top. A press sets the step under the mouse to the level under it, and a drag draws across the steps.
// Returns the new pattern once the user lets go, if it differs.
std::optional<edit::GbaPattern> PatternRow(const edit::GbaPattern& pattern, BankEditorState& state, float height)
{
    FieldLabel("Pattern", kGbaPatternTip);
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const ImVec2 size(ImGui::GetContentRegionAvail().x, height);
    const ImVec2 max(min.x + size.x, min.y + size.y);
    ImGui::InvisibleButton("##pattern", size);
    constexpr auto kSteps = static_cast<int>(edit::kGbaPatternSteps);
    constexpr int kTop = 15;
    if (ImGui::IsItemActive())
    {
        edit::GbaPattern drawn = state.gba_steps ? *state.gba_steps : pattern;
        const ImVec2 mouse = ImGui::GetMousePos();
        const int step = std::clamp(static_cast<int>((mouse.x - min.x) / size.x * kSteps), 0, kSteps - 1);
        const int level = std::clamp(static_cast<int>(std::lround((max.y - mouse.y) / size.y * kTop)), 0, kTop);
        drawn[static_cast<std::size_t>(step)] = static_cast<uint8_t>(level);
        state.gba_steps = drawn;
    }
    std::optional<edit::GbaPattern> done;
    if (ImGui::IsItemDeactivated() && state.gba_steps)
    {
        if (*state.gba_steps != pattern)
        {
            done = state.gba_steps;
        }
        state.gba_steps.reset();
    }

    // A panel with a line at the middle level, and the steps as bars from it.
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const edit::GbaPattern& shown = state.gba_steps ? *state.gba_steps : pattern;
    draw->AddRectFilled(min, max, theme::kPanel, Px(3.0f));
    const float middle = max.y - size.y * (static_cast<float>(kTop) * 0.5f) / static_cast<float>(kTop);
    draw->AddLine(ImVec2(min.x, middle), ImVec2(max.x, middle), theme::kBorder, Px(1.0f));
    const float width = size.x / static_cast<float>(kSteps);
    for (int step = 0; step < kSteps; ++step)
    {
        const float level = static_cast<float>(shown[static_cast<std::size_t>(step)]) / static_cast<float>(kTop);
        const float top = max.y - size.y * level;
        const float left = min.x + width * static_cast<float>(step);
        draw->AddRectFilled(ImVec2(left + Px(1.0f), std::min(top, middle)),
                            ImVec2(left + width - Px(1.0f), std::max(top, middle) + Px(1.0f)), theme::kAccent);
    }
    if (ImGui::IsItemHovered())
    {
        const ImVec2 mouse = ImGui::GetMousePos();
        const int step = std::clamp(static_cast<int>((mouse.x - min.x) / size.x * kSteps), 0, kSteps - 1);
        WrappedTooltip("Step " + std::to_string(step + 1) + ": " +
                       std::to_string(shown[static_cast<std::size_t>(step)]));
    }

    return done;
}

// The program of `view` that the editor shows: the state's while the view contains it, or else the first. A new program
// selects the first row of a key split or a drum kit. While an edit waits for its version (`waiting`), the program
// that the edit selects can be missing from the view. The editor then shows the first program and keeps the selection.
const GbaProgram* SelectedGbaProgram(const GbaBankView& view, BankEditorState& state, bool waiting)
{
    const auto forget = [&state]
    {
        state.gba_row.reset();
        state.gba_held.reset();
        state.gba_steps.reset();
    };
    return SelectedOrFirst(view.programs, state.program, &GbaProgram::program, waiting, forget);
}

// The row of a key split or a drum kit that the editor shows: the state's while the program has it, or else the
// first. While an edit waits for its version (`waiting`), the row that the edit selects can be missing from the
// program. The editor then shows the first row and keeps the selection.
const GbaVoiceRow* SelectedGbaRow(const GbaProgram& program, BankEditorState& state, bool waiting)
{
    const auto forget = [&state]
    {
        state.gba_held.reset();
        state.gba_steps.reset();
    };
    return SelectedOrFirst(program.rows, state.gba_row, &GbaVoiceRow::first_key, waiting, forget);
}

// The details of a GBA edit in the Undo History: the voice group, the program and the row's keys.
std::string GbaDetails(const GbaBankView& view, const GbaProgram& program, const GbaVoiceRow* row)
{
    std::string details = ProgramDetails(view.name, program.program);
    if (row != nullptr)
    {
        details += KeysDetail(row->first_key, row->last_key);
    }

    return details;
}

// Places `voice` at the first free program after the selected one, or else at the first free program, as the edit
// `name`. `chosen` is true when a program is selected.
void GbaPlaceVoice(const GbaBankView& view, bool chosen, const char* name, const ntdlib::mp2k::Voice& voice,
                   BankEditorState& state, BankEdit& edit)
{
    const auto free =
        std::ranges::find_if(view.free_programs, [&](uint32_t program) { return !chosen || program > *state.program; });
    const auto first = free != view.free_programs.end() ? free : view.free_programs.begin();
    if (first == view.free_programs.end())
    {
        edit.refused = "The voice group has no free program.";
        return;
    }
    edit.gba_new.emplace_back(*first, voice);
    edit.name = name;
    edit.details = ProgramDetails(view.name, *first);
    state.program = *first;
    state.gba_row.reset();
    state.gba_held.reset();
    state.gba_steps.reset();
}

// The list of a GBA voice group's programs at `pos`, `size` large: each program's number and the kind of its voice. A
// click selects a program.
void DrawGbaPrograms(const GbaBankView& view, BankEditorState& state, bool busy, ImVec2 pos, ImVec2 size,
                     BankEdit& edit)
{
    ImDrawList* draw = ImGui::GetWindowDrawList();
    DrawCaps(draw, pos, theme::kTextMuted, "PROGRAMS");
    const float caps = CapsSize("PROGRAMS").y + Px(6.0f);
    const ImVec2 list_min(pos.x, pos.y + caps);
    const float buttons = ImGui::GetFrameHeight() + Px(kGap);
    const ImVec2 list_size(size.x, std::max(size.y - caps - buttons, Px(kRowHeight)));
    state.list_min = list_min;
    state.list_max = ImVec2(list_min.x + list_size.x, list_min.y + list_size.y);
    const auto label_of = [](const GbaProgram& program)
    {
        std::string kind = program.voice.kind;
        if (program.voice.voice.IsSplit())
        {
            kind += ", " + std::to_string(program.rows.size()) + (program.rows.size() == 1 ? " voice" : " voices");
        }
        return kind;
    };
    const auto select = [&state](uint32_t program)
    {
        state.program = program;
        state.gba_row.reset();
        state.gba_held.reset();
        state.gba_steps.reset();
    };
    DrawProgramList(view.programs, label_of, select, state, list_min, list_size, nullptr);

    // Add makes a voice at the first free program after the selected one: the ROM's first sample, or a square wave.
    // Duplicate copies the selected voice there, and Delete clears the selected voice's bytes. A new voice can move the
    // voice group's table into the ROM's free space (edit::WithGbaVoicesAt).
    ImGui::SetCursorScreenPos(ImVec2(pos.x, list_min.y + list_size.y + Px(kGap)));
    const std::array<float, 3> width = ProgramButtonWidths(size.x);
    const auto selected =
        state.program ? std::ranges::find(view.programs, *state.program, &GbaProgram::program) : view.programs.end();
    const bool chosen = selected != view.programs.end();
    ImGui::BeginDisabled(busy);
    if (ImGui::Button("Add", ImVec2(width[0], 0.0f)))
    {
        GbaPlaceVoice(
            view, chosen, "Add Voice",
            view.samples.empty() ? edit::NewGbaSquareVoice() : edit::NewGbaSampleVoice(view.samples.front().address),
            state, edit);
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!chosen);
    if (ImGui::Button("Duplicate", ImVec2(width[1], 0.0f)) && chosen)
    {
        GbaPlaceVoice(view, chosen, "Duplicate Voice", selected->voice.voice, state, edit);
    }
    ImGui::SameLine();
    if (ImGui::Button("Delete", ImVec2(width[2], 0.0f)) && chosen)
    {
        edit.gba_cleared = selected->voice.voice.address;
        edit.name = "Delete Voice";
        edit.details = ProgramDetails(view.name, selected->program);
        state.program = NeighbourProgram(view.programs, selected, &GbaProgram::program);
        state.gba_row.reset();
        state.gba_held.reset();
        state.gba_steps.reset();
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();
}

// The rows of `program` at `pos`, `size` large. `drums` is true for a drum kit.
void DrawGbaRowList(const GbaProgram& program, bool drums, BankEditorState& state, ImVec2 pos, ImVec2 size)
{
    ImGui::SetCursorScreenPos(pos);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::ColorConvertU32ToFloat4(theme::kPanel));
    ImGui::BeginChild("##rows", size, ImGuiChildFlags_Borders);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    ImFont* text = theme::GetFonts().text;
    const float column = TextSize(text, theme::kTextSize, "C#-1 to C#-1").x + Px(16.0f);
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(program.rows.size()));
    while (clipper.Step())
    {
        for (int index = clipper.DisplayStart; index < clipper.DisplayEnd; ++index)
        {
            const GbaVoiceRow& row = program.rows[static_cast<std::size_t>(index)];
            ImGui::PushID(index);
            const float left = ImGui::GetCursorScreenPos().x;
            if (ImGui::Selectable("##row", state.gba_row == row.first_key, 0,
                                  ImVec2(0.0f, Px(kRowHeight) - Px(6.0f))) &&
                state.gba_row != row.first_key)
            {
                state.gba_row = row.first_key;
                state.gba_held.reset();
                state.gba_steps.reset();
            }
            ImGui::PopID();
            std::string kind = row.kind;
            if (drums)
            {
                kind += ", plays " + KeyName(row.voice.key);
            }
            const float top = (ImGui::GetItemRectMin().y + ImGui::GetItemRectMax().y) * 0.5f -
                              TextSize(text, theme::kTextSize, kind).y * 0.5f;
            DrawText(draw, text, theme::kTextSize, ImVec2(left, top), theme::kText,
                     KeysText(row.first_key, row.last_key));
            DrawText(draw, text, theme::kTextSize, ImVec2(left + column, top), theme::kTextMuted, kind);
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

// Adds a row to the rows of `program`, after the row that starts at key `after`, or first.
void GbaAddRow(const GbaBankView& view, const GbaProgram& program, const std::vector<edit::GbaKeyRow>& rows,
               std::optional<int> after, BankEditorState& state, BankEdit& edit)
{
    const bool drums = (program.voice.voice.type & ntdlib::mp2k::kVoiceDrumKit) != 0;
    std::vector<edit::GbaKeyRow> made = edit::GbaRowsWithNew(rows, drums, after);
    for (const edit::GbaKeyRow& one : made)
    {
        if (std::ranges::none_of(rows, [&one](const edit::GbaKeyRow& old) { return old.first_key == one.first_key; }))
        {
            state.gba_row = one.first_key;
        }
    }
    edit.gba_rows = std::pair(program.voice.voice, std::move(made));
    edit.name = drums ? "Add Drum" : "Add Key Range";
    edit.details = GbaDetails(view, program, nullptr);
}

// Deletes `row`, one of the rows of `program`.
void GbaDeleteRow(const GbaBankView& view, const GbaProgram& program, const std::vector<edit::GbaKeyRow>& rows,
                  std::vector<GbaVoiceRow>::const_iterator row, BankEditorState& state, BankEdit& edit)
{
    const bool drums = (program.voice.voice.type & ntdlib::mp2k::kVoiceDrumKit) != 0;
    // The selection moves to the row that gets the keys of the deleted row. On a drum kit, it moves to the next
    // row, or else the row before.
    edit.name = drums ? "Delete Drum" : "Delete Key Range";
    edit.details = GbaDetails(view, program, nullptr);
    if (drums)
    {
        edit.gba_cleared = row->voice.address;
        const auto next = std::next(row) != program.rows.end() ? std::next(row)
                          : row != program.rows.begin()        ? std::prev(row)
                                                               : program.rows.end();
        state.gba_row = next != program.rows.end() ? std::optional<int>(next->first_key) : std::optional<int>();
    }
    else
    {
        std::vector<edit::GbaKeyRow> kept = edit::GbaSplitRowsWithout(rows, row->first_key);
        const int key = row->first_key;
        const auto taker = std::ranges::find_if(
            kept, [key](const edit::GbaKeyRow& one) { return one.first_key <= key && key <= one.last_key; });
        state.gba_row = taker != kept.end() ? std::optional<int>(taker->first_key) : std::optional<int>();
        edit.gba_rows = std::pair(program.voice.voice, std::move(kept));
    }
}

// The rows of a key split or a drum kit at `pos`, `size` large: each row's keys and the kind of its voice. A drum kit's
// row adds the key that the voice plays at. A click selects a row. Add, Delete and Make Single Voice stand under them.
void DrawGbaRows(const GbaBankView& view, const GbaProgram& program, BankEditorState& state, bool busy, ImVec2 pos,
                 ImVec2 full_size, BankEdit& edit)
{
    const ImVec2 size(full_size.x, std::max(full_size.y - ImGui::GetFrameHeight() - Px(kGap), Px(kRowHeight)));
    const bool drums = (program.voice.voice.type & ntdlib::mp2k::kVoiceDrumKit) != 0;
    DrawGbaRowList(program, drums, state, pos, size);

    // The new rows of a key split or a drum kit use new tables in the ROM's free space. Delete on a drum kit clears
    // the voice's bytes in its table.
    const auto row =
        state.gba_row ? std::ranges::find(program.rows, *state.gba_row, &GbaVoiceRow::first_key) : program.rows.begin();
    const bool chosen = row != program.rows.end();
    const std::vector<edit::GbaKeyRow> rows = KeyRowsOf(program);
    ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + size.y + Px(kGap)));
    ImGui::BeginDisabled(busy);
    ImGui::PushID("rows");
    try
    {
        if (ImGui::Button("Add"))
        {
            GbaAddRow(view, program, rows, chosen ? std::optional(row->first_key) : std::nullopt, state, edit);
        }
        ImGui::SameLine();
        if (ImGui::Button("Delete") && chosen)
        {
            GbaDeleteRow(view, program, rows, row, state, edit);
        }
    }
    catch (const std::invalid_argument& error)
    {
        edit.refused = error.what();
    }
    ImGui::SameLine();
    if (ImGui::Button("Make Single Voice") && chosen)
    {
        ntdlib::mp2k::Voice single = row->voice;
        single.address = program.voice.voice.address;
        edit.gba_program_voice = single;
        edit.name = "Make Single Voice";
        edit.details = GbaDetails(view, program, nullptr);
        state.gba_row.reset();
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip(
            "Makes the program play the voice of the selected row at every key. The old tables stay in "
            "the ROM.");
    }
    ImGui::PopID();
    ImGui::EndDisabled();
}

// A slider for a key of a row of a key split or a drum kit. Returns the new key once the user lets go.
std::optional<int> RowKeySlider(const char* label, const char* tooltip, int key, BankEditorState& state)
{
    FieldLabel(label, tooltip);
    const bool holding = state.gba_key_held && state.held_name == label;
    auto shown = static_cast<float>(holding ? *state.gba_key_held : key);
    const SliderRange typed{0.0f, 127.0f};
    const std::string format = "%.0f  (" + KeyName(static_cast<int>(std::lround(shown))) + ")";
    std::optional<int> released;
    ImGui::PushID(label);
    if (ValueSlider("##value", &shown, {0.0f, 127.0f}, static_cast<float>(key), format.c_str(),
                    ImVec2(ImGui::GetContentRegionAvail().x, ImGui::GetFrameHeight()), &typed))
    {
        state.gba_key_held = static_cast<int>(std::lround(std::clamp(shown, 0.0f, 127.0f)));
        state.held_name = label;
    }
    if (SliderReleased() && state.gba_key_held && state.held_name == label)
    {
        released = *std::exchange(state.gba_key_held, std::nullopt);
    }
    ImGui::PopID();

    return released != key ? released : std::nullopt;
}

// The time of a PSG voice's length as its row shows it: off at 0, and else the time before the PSG ends the note.
std::string GbaLengthText(const ntdlib::mp2k::Voice& voice)
{
    if (voice.length == 0)
    {
        return "%.0f  (off)";
    }

    // The wave channel's length register contains 8 bits, and the other channels' 6 bits.
    const bool wave = voice.PsgChannel() == 3;
    const int counts = (wave ? 256 : 64) - (wave ? int{voice.length} : std::min<int>(voice.length, 63));
    std::array<char, 32> text{};
    std::snprintf(text.data(), text.size(), "%%.0f  (%.0f ms)", 1000.0 * counts / 256.0);
    return text.data();
}

// What the fields of a GBA voice change in a frame: the new voice and the action's name, and whether the user let go of
// a slider.
struct GbaFieldEdit
{
    std::optional<ntdlib::mp2k::Voice> changed;
    std::string name;
    bool released = false;

    void Change(ntdlib::mp2k::Voice after, std::string action)
    {
        changed = after;
        name = std::move(action);
    }
};

// The menu of the voice's kind.
void GbaKindCombo(const GbaBankView& view, const GbaProgram& program, const GbaVoiceRow* row,
                  const ntdlib::mp2k::Voice& voice, BankEditorState& state, GbaFieldEdit& fields, BankEdit& edit)
{
    namespace mp2k = ntdlib::mp2k;
    const int psg = voice.PsgChannel();
    static constexpr std::array<const char*, 7> kKinds = {"Sample", "Square 1",  "Square 2", "Wave",
                                                          "Noise",  "Key split", "Drum kit"};
    FieldLabel("Kind", kGbaKindTip);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    if (ImGui::BeginCombo("##kind", kKinds[static_cast<std::size_t>(std::clamp(psg, 0, 4))]))
    {
        const int kinds = row != nullptr ? 5 : 7;
        for (int kind = 0; kind < kinds; kind++)
        {
            if (!ImGui::Selectable(kKinds[static_cast<std::size_t>(kind)], kind == psg) || kind == psg)
            {
                continue;
            }
            if (kind == 0 && view.samples.empty())
            {
                edit.refused = "No voice in the ROM plays a sample. Drag a WAV file onto the voice.";
            }
            else if (kind >= 5)
            {
                const bool kit = kind == 6;
                const int first = kit ? 60 : 0;
                edit.gba_rows = std::pair(edit::GbaSplitOf(voice, kit ? mp2k::kVoiceDrumKit : mp2k::kVoiceKeySplit),
                                          std::vector<edit::GbaKeyRow>{{first, kit ? first : 127, voice}});
                edit.name = kit ? "Make Drum Kit" : "Make Key Split";
                edit.details = GbaDetails(view, program, row);
                state.gba_row.reset();
            }
            else
            {
                fields.Change(edit::GbaVoiceOfKind(voice, kind, kind == 0 ? view.samples.front().address : 0),
                              "Change Kind");
                edit.gba_new_pattern = kind == 3;
            }
        }
        ImGui::EndCombo();
    }
}

// The keys of `row`, a row of a key split or a drum kit.
void GbaRowKeys(const GbaBankView& view, const GbaProgram& program, const GbaVoiceRow& row, bool drums,
                BankEditorState& state, BankEdit& edit)
{
    const std::vector<edit::GbaKeyRow> rows = KeyRowsOf(program);
    const auto moved = [&](int first, int last, const char* action)
    {
        try
        {
            edit.gba_rows =
                std::pair(program.voice.voice, edit::GbaRowsWithKeys(rows, drums, row.first_key, first, last));
            edit.name = action;
            edit.details = GbaDetails(view, program, &row);
            state.gba_row = first;
        }
        catch (const std::invalid_argument& error)
        {
            edit.refused = error.what();
        }
    };
    if (drums)
    {
        if (const std::optional<int> key = RowKeySlider("At key", kGbaAtKeyTip, row.first_key, state))
        {
            moved(*key, *key, "Move Drum");
        }
    }
    else
    {
        if (const std::optional<int> key = RowKeySlider("First key", kGbaFirstKeyTip, row.first_key, state))
        {
            moved(std::min(*key, row.last_key), row.last_key, "Change Keys");
        }
        if (const std::optional<int> key = RowKeySlider("Last key", kGbaLastKeyTip, row.last_key, state))
        {
            moved(row.first_key, std::max(*key, row.first_key), "Change Keys");
        }
    }
}

// A sample's rows: the sample and its pitch, and its rate.
void GbaSampleRows(const GbaBankView& view, const ntdlib::mp2k::Voice& voice, BankEditorState& state,
                   GbaFieldEdit& fields)
{
    namespace mp2k = ntdlib::mp2k;
    // The sample, from a menu of the samples that the ROM's voices play, and whether it follows the key.
    FieldLabel("Sample", kGbaSampleTip);
    const auto found = std::ranges::find(view.samples, voice.wave, &GbaSample::address);
    int chosen = found != view.samples.end() ? static_cast<int>(found - view.samples.begin()) : -1;
    const std::string missing = GbaSampleName(voice.wave);
    ImGui::SetNextWindowSizeConstraints(ImVec2(Px(280.0f), 0.0f), ImVec2(FLT_MAX, FLT_MAX));
    if (FilteredCombo("##sample", view.sample_names, chosen, chosen < 0 ? missing.c_str() : nullptr, state.filter) &&
        chosen >= 0 && view.samples[static_cast<std::size_t>(chosen)].address != voice.wave)
    {
        mp2k::Voice after = voice;
        after.wave = view.samples[static_cast<std::size_t>(chosen)].address;
        fields.Change(after, "Change Sample");
    }
    const bool fixed = (voice.type & mp2k::kVoiceFixed) != 0;
    const int pitch =
        SwitchRow("Pitch", kGbaPitchTip, std::array<std::string, 2>{"Follows key", "Fixed"}, fixed ? 1 : 0);
    if (pitch >= 0)
    {
        mp2k::Voice after = voice;
        after.type =
            static_cast<uint8_t>(pitch == 1 ? voice.type | mp2k::kVoiceFixed : voice.type & ~mp2k::kVoiceFixed);
        fields.Change(after, "Change Pitch");
    }
    FieldLabel("Rate", kGbaRateTip);
    if (found != view.samples.end())
    {
        ImGui::Text("%s Hz, %s samples%s", Grouped(static_cast<uint64_t>(std::lround(found->rate))).c_str(),
                    Grouped(found->size).c_str(), found->loops ? ", loops" : "");
    }
}

// A square wave's rows: its duty, square 1's sweep, and the length.
void GbaSquareRows(const ntdlib::mp2k::Voice& voice, const ntdlib::mp2k::Voice& current, BankEditorState& state,
                   GbaFieldEdit& fields)
{
    namespace mp2k = ntdlib::mp2k;
    const int psg = voice.PsgChannel();

    const std::array<std::string, 4> kDuties = {"12.5 %", "25 %", "50 %", "75 %"};
    const int duty =
        SwitchRow("Duty", kGbaDutyTip, kDuties, voice.wave <= edit::kHighestDuty ? static_cast<int>(voice.wave) : -1);
    if (duty >= 0)
    {
        mp2k::Voice after = voice;
        after.wave = static_cast<uint32_t>(duty);
        fields.Change(after, "Change Duty");
    }
    if (psg == 1)
    {
        const char* grey = (voice.pan_sweep & kPanBit) != 0 ? kGbaPanSweepTip : nullptr;
        const int time = kGbaSweepTime.get(current);
        std::array<char, 32> format{};
        std::snprintf(format.data(), format.size(), time == 0 ? "%%.0f  (off)" : "%%.0f  (%.1f ms)",
                      1000.0 * time / 128.0);
        GbaRow(kGbaSweepTime, voice, format.data(), state, &fields.released, grey);
        const int down = SwitchRow("Sweep", kGbaSweepTip, std::array<std::string, 2>{"Up", "Down"},
                                   (voice.pan_sweep & kSweepDown) != 0 ? 1 : 0, grey != nullptr);
        if (down >= 0)
        {
            mp2k::Voice after = voice;
            after.pan_sweep =
                static_cast<uint8_t>(down == 1 ? (voice.pan_sweep | kSweepDown) : (voice.pan_sweep & ~kSweepDown));
            fields.Change(after, "Change Sweep");
        }
        GbaRow(kGbaSweepShift, voice, "%.0f", state, &fields.released, grey);
    }
    GbaRow(kGbaLength, voice, GbaLengthText(current), state, &fields.released);
}

// A wave's rows: its pattern and the length. Returns the pattern that the user drew, if any.
std::optional<edit::GbaPattern> GbaWaveRows(const GbaBankView& view, const ntdlib::mp2k::Voice& voice,
                                            const ntdlib::mp2k::Voice& current, BankEditorState& state,
                                            GbaFieldEdit& fields)
{
    std::optional<edit::GbaPattern> drawn;
    const auto pattern = view.patterns.find(voice.wave);
    if (pattern != view.patterns.end())
    {
        drawn = PatternRow(pattern->second, state, ImGui::GetFrameHeight() * 3.0f);
    }
    else
    {
        FieldLabel("Pattern", kGbaPatternTip, true);
        ImGui::TextUnformatted("Outside the ROM");
    }
    GbaRow(kGbaLength, voice, GbaLengthText(current), state, &fields.released);

    return drawn;
}

// The noise's rows: its mode and the length.
void GbaNoiseRows(const ntdlib::mp2k::Voice& voice, const ntdlib::mp2k::Voice& current, BankEditorState& state,
                  GbaFieldEdit& fields)
{
    namespace mp2k = ntdlib::mp2k;

    const int mode = SwitchRow("Noise", kGbaNoiseTip, std::array<std::string, 2>{"15-bit", "7-bit"},
                               voice.wave <= edit::kHighestNoiseMode ? static_cast<int>(voice.wave) : -1);
    if (mode >= 0)
    {
        mp2k::Voice after = voice;
        after.wave = static_cast<uint32_t>(mode);
        fields.Change(after, "Change Noise");
    }
    GbaRow(kGbaLength, voice, GbaLengthText(current), state, &fields.released);
}

// The rows of a voice in a drum kit: its key, and its pan.
void GbaDrumRows(const ntdlib::mp2k::Voice& voice, const ntdlib::mp2k::Voice& current, BankEditorState& state,
                 GbaFieldEdit& fields)
{
    namespace mp2k = ntdlib::mp2k;
    GbaRow(kGbaKey, voice, "%.0f  (" + KeyName(current.key) + ")", state, &fields.released);
    const bool panned = (voice.pan_sweep & kPanBit) != 0;
    const int pan = SwitchRow("Drum pan", kGbaPanTip, std::array<std::string, 2>{"Off", "On"}, panned ? 1 : 0);
    if (pan >= 0)
    {
        mp2k::Voice after = voice;
        after.pan_sweep = static_cast<uint8_t>(pan == 1 ? kDrumCentre : 0);
        fields.Change(after, pan == 1 ? "Pan Drum" : "Unpan Drum");
    }
    if (panned)
    {
        GbaRow(kGbaPan, voice, "%.0f  (" + PanOf(current.pan_sweep - kPanBit) + ")", state, &fields.released);
    }
}

// Makes the edit of the fields: the slider that the user let go of, the voice that a field changed or the pattern that
// the user drew. A busy editor makes none.
void GbaFinishEdit(const GbaBankView& view, const GbaProgram& program, const GbaVoiceRow* row,
                   const ntdlib::mp2k::Voice& voice, const std::optional<edit::GbaPattern>& drawn, bool busy,
                   BankEditorState& state, GbaFieldEdit& fields, BankEdit& edit)
{
    if (fields.released && state.gba_held)
    {
        if (edit::GbaVoiceBytes(*state.gba_held) != edit::GbaVoiceBytes(voice))
        {
            fields.Change(*state.gba_held, "Change " + state.held_name);
        }
        state.gba_held.reset();
    }
    if (busy)
    {
        return;
    }
    if (fields.changed)
    {
        edit.gba_voice = fields.changed;
        edit.name = fields.name;
        edit.details = GbaDetails(view, program, row);
    }
    else if (drawn)
    {
        edit.gba_pattern = std::pair(voice.wave, *drawn);
        edit.name = "Draw Pattern";
        edit.details = GbaDetails(view, program, row);
    }
}

// The fields of a GBA voice, in two tables side by side: what it plays on the left, with a drum kit's key and pan, and
// its envelope on the right. `row` is the shown row of a key split or a drum kit, or nothing for a program's voice.
void DrawGbaFields(const GbaBankView& view, const GbaProgram& program, const GbaVoiceRow* row, BankEditorState& state,
                   bool busy, ImVec2 pos, ImVec2 size, BankEdit& edit)
{
    namespace mp2k = ntdlib::mp2k;
    const mp2k::Voice& voice = row != nullptr ? row->voice : program.voice.voice;
    const mp2k::Voice& current = GbaShown(voice, state);
    const bool drums = row != nullptr && (program.voice.voice.type & mp2k::kVoiceDrumKit) != 0;
    const int psg = voice.PsgChannel();
    GbaFieldEdit fields;
    std::optional<edit::GbaPattern> drawn;
    const float half = (size.x - Px(kGap)) * 0.5f;
    ImGui::BeginDisabled(busy);
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(ImGui::GetStyle().CellPadding.x, Px(kFieldPadding)));
    if (BeginFieldTable("##gba-playing", pos, half))
    {
        // The voice's kind. A new wave uses a new pattern (BankEdit::gba_new_pattern). A program's voice can become a
        // key split or a drum kit that plays it. A row's voice can't: the driver plays no split inside another.
        GbaKindCombo(view, program, row, voice, state, fields, edit);

        // A row's keys. Other rows of a key split give way. A drum kit's voice moves to another key.
        if (row != nullptr)
        {
            GbaRowKeys(view, program, *row, drums, state, edit);
        }

        if (psg == 0)
        {
            GbaSampleRows(view, voice, state, fields);
        }
        else if (psg <= 2)
        {
            GbaSquareRows(voice, current, state, fields);
        }
        else if (psg == 3)
        {
            drawn = GbaWaveRows(view, voice, current, state, fields);
        }
        else
        {
            GbaNoiseRows(voice, current, state, fields);
        }

        // A drum kit's voice plays at its key, and at its pan where bit 7 of its byte is set.
        if (drums)
        {
            GbaDrumRows(voice, current, state, fields);
        }
        ImGui::EndTable();
    }

    // The envelope: a sample's in 256ths of full level, and a PSG voice's in frames for each of its levels.
    if (BeginFieldTable("##gba-envelope", ImVec2(pos.x + half + Px(kGap), pos.y), half))
    {
        for (const GbaField& field : psg == 0 ? kGbaSampleEnvelope : kGbaPsgEnvelope)
        {
            GbaRow(field, voice, "%.0f", state, &fields.released);
        }
        ImGui::EndTable();
    }
    ImGui::PopStyleVar();
    ImGui::EndDisabled();

    // A slider that the user let go of makes its change, unless it ended on its old value. A drawn pattern is applied
    // the same way.
    GbaFinishEdit(view, program, row, voice, drawn, busy, state, fields, edit);
}

// The facts of a GBA voice group at `pos`, `size` large: the songs that play it, the other places that play the shown
// voice's bytes, and the voices that play the shown wave's pattern.
void DrawGbaFacts(const GbaBankView& view, const ntdlib::mp2k::Voice* voice, const GbaVoiceRow* shown, ImVec2 pos,
                  ImVec2 size, bool top)
{
    BeginFacts(pos, size, top);
    FactLine("Edits affect " + std::to_string(view.songs.size()) + (view.songs.size() == 1 ? " song: " : " songs: "),
             view.songs);
    if (shown != nullptr && !shown->shared.empty())
    {
        FactLine("Editing the voice also changes it at ", shown->shared);
    }
    if (voice != nullptr && voice->PsgChannel() == 3)
    {
        const auto places = view.pattern_places.find(voice->wave);
        if (places != view.pattern_places.end() && places->second.size() > 1)
        {
            FactLine("Editing the pattern changes every voice that uses it: ", places->second);
        }
    }
    ImGui::PopTextWrapPos();
    ImGui::EndChild();
}

// What the right side shows for a program: the selected row of a key split or a drum kit, the voice and the row that
// the fields and the facts show, and the keys that the selected row covers.
struct GbaProgramSide
{
    const GbaVoiceRow* row = nullptr;
    const ntdlib::mp2k::Voice* voice = nullptr;
    const GbaVoiceRow* shown = nullptr;
    std::optional<std::pair<int, int>> covered;
};

// The right side of the window for `program`: its rows, or a card for a voice that isn't a key split or a drum kit,
// and its fields.
GbaProgramSide DrawGbaProgramSide(const GbaBankView& view, const GbaProgram& program, BankEditorState& state,
                                  const ShownItem& bank, bool busy, const BankEditorLayout& layout, BankEdit& edit)
{
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 rows_min(layout.right, layout.rows_top);
    const ImVec2 rows_max(layout.right + layout.right_width, layout.keyboard_top - Px(2.0f));
    const GbaVoiceRow* row = nullptr;
    const ntdlib::mp2k::Voice* voice = nullptr;
    const GbaVoiceRow* shown = nullptr;
    std::optional<std::pair<int, int>> covered;
    // A file that the system drops over the rows or the fields lands on the shown voice (DroppedGbaVoice).
    state.map_min = rows_min;
    state.map_max = ImVec2(layout.right + layout.right_width, layout.fields_top + layout.fields_height);
    DrawCaps(draw, ImVec2(layout.right, layout.origin.y), theme::kTextMuted,
             "Program " + std::to_string(program.program) + ": " + program.voice.kind);
    const ImVec2 fields_min(layout.right, layout.fields_top);
    const ImVec2 fields_max(layout.right + layout.right_width, layout.fields_top + layout.fields_height);
    if (program.voice.voice.IsSplit())
    {
        row = SelectedGbaRow(program, state, busy || !edit.name.empty());
        DrawGbaRows(view, program, state, busy, rows_min, ImVec2(rows_max.x - rows_min.x, rows_max.y - rows_min.y),
                    edit);
        if (row != nullptr)
        {
            covered = std::pair(row->first_key, row->last_key);
            voice = &row->voice;
            shown = row;
        }
        else
        {
            EmptyState(draw, fields_min, fields_max, "No voice", "No key of this program plays anything.");
        }
    }
    else
    {
        EmptyState(draw, rows_min, rows_max, program.voice.kind, GbaKindText(GbaVoiceTypeOf(program.voice.voice)));
        voice = &program.voice.voice;
        shown = &program.voice;
    }

    if (shown != nullptr && GbaVoiceTypeOf(shown->voice) == GbaVoiceType::kUnknown)
    {
        EmptyState(draw, fields_min, fields_max, "Unknown voice", GbaKindText(GbaVoiceType::kUnknown));
    }
    else if (shown != nullptr)
    {
        PushSliderItem(
            TypedItemOf(bank, program.program,
                        row != nullptr ? std::optional(static_cast<std::size_t>(row->first_key)) : std::nullopt));
        DrawGbaFields(view, program, row, state, busy, fields_min, ImVec2(layout.right_width, layout.fields_height),
                      edit);
        PopSliderItem();
    }

    return {row, voice, shown, covered};
}

} // namespace

std::optional<ntdlib::mp2k::Voice> DroppedGbaVoice(const GbaBankView& view, const BankEditorState& state,
                                                   const BankDrop& drop)
{
    if (!drop.program)
    {
        return std::nullopt;
    }
    const auto program = std::ranges::find(view.programs, *drop.program, &GbaProgram::program);
    if (program == view.programs.end())
    {
        return std::nullopt;
    }
    if (!program->voice.voice.IsSplit())
    {
        return program->voice.voice;
    }
    const auto row = state.gba_row ? std::ranges::find(program->rows, *state.gba_row, &GbaVoiceRow::first_key)
                                   : program->rows.begin();

    return row != program->rows.end() ? std::optional(row->voice) : std::nullopt;
}

BankEdit DrawGbaBankEditor(const GbaBankView& view, BankEditorState& state, const ShownItem& bank, bool busy,
                           bool* open)
{
    // The window of a bank, with the same parts. WAV files drop on a voice and on the list of programs.
    const std::string title = "Bank Editor - " + view.name + "###BankEditor";
    BankEdit edit;
    ForgetDropTargets(state);
    if (!BeginBankEditorWindow(title, state, open))
    {
        return edit;
    }

    // The list of programs on the left. On the right, the rows of a key split or a drum kit, or a card for another
    // voice, then the keyboard and the fields. The facts go across the bottom.
    const BankEditorLayout layout = BankLayoutOf(kGbaFieldRows, "PROGRAM");
    const ImVec2 rows_min(layout.right, layout.rows_top);
    const ImVec2 rows_max(layout.right + layout.right_width, layout.keyboard_top - Px(2.0f));
    const bool top = state.reveal;
    DrawGbaPrograms(view, state, busy, layout.origin,
                    ImVec2(Px(kListWidth), layout.facts_top - layout.origin.y - Px(kGap)), edit);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    DrawListDivider(draw, layout);

    const GbaProgram* program = SelectedGbaProgram(view, state, busy || !edit.name.empty());
    GbaProgramSide side;
    if (program != nullptr)
    {
        side = DrawGbaProgramSide(view, *program, state, bank, busy, layout, edit);
    }
    else
    {
        EmptyState(draw, rows_min, rows_max, "No programs", "No song uses a program from this voice group.");
    }
    DrawKeyboard(side.covered, std::nullopt, state, ImVec2(layout.right, layout.keyboard_top),
                 ImVec2(layout.right + layout.right_width, layout.keyboard_top + Px(kKeyboardHeight)));
    if (program == nullptr)
    {
        state.sounding = -1;
    }

    DrawFactsDivider(draw, layout);
    DrawGbaFacts(view, side.voice, side.shown, ImVec2(layout.origin.x, layout.facts_top),
                 ImVec2(layout.room.x, layout.origin.y + layout.room.y - layout.facts_top), top);
    ImGui::End();

    return edit;
}

} // namespace hokuto::app
