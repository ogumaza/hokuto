// SPDX-License-Identifier: MIT

// The parts of the Sound section that its files share (sound_section.cpp and wave_sound_section.cpp): the rows and
// their labels. The names are internal to those files.

#pragma once

#include <imgui.h>

#include <optional>
#include <vector>

#include "app/sound_section.h"

namespace hokuto::app::sound_section_parts
{

// The section's sizes, before the display's scale: the gap around a control in its row, a sub-heading's row, the
// labels' least column, the column of the bank slots' numbers, and the least size of a menu's list.
constexpr float kRowGap = 6.0f;
constexpr float kHeadingHeight = 26.0f;
constexpr float kLabelWidth = 84.0f;
constexpr float kSlotLabelWidth = 24.0f;
constexpr float kListWidth = 240.0f;
constexpr float kListHeight = 320.0f;

// The rows as the section draws them: the left edge, the width, the top of the next row, and the height of a control.
struct Rows
{
    ImDrawList* draw = nullptr;
    float left = 0.0f;
    float width = 0.0f;
    float y = 0.0f;
    float control = 0.0f;
};

// The place of a row's control: its top left corner, and its width.
struct Control
{
    ImVec2 pos;
    float width = 0.0f;
};

// The labels' column: the widest label of the section and a gap, and kLabelWidth at least. Each system's font gives the
// labels other widths.
float LabelWidth();

// Draws a sub-heading in capitals, and moves to the next row.
void Heading(Rows& rows, const char* text);

// Draws a row's label in a column `label_width` wide, and moves to the next row. Returns the place of the row's
// control, at the column's right. A grey control takes no input, and the label's tooltip shows over it too.
Control Label(Rows& rows, const char* label, const char* tooltip, float label_width = LabelWidth(), bool grey = false);

// The labels of the rows of a wave sound's values, for LabelWidth.
std::vector<const char*> WaveRowLabels();

// The rows of a wave sound's values in its wave sound file: its wave, its root key, its pitch, its pans and its sends,
// then its filters and its envelope. A value that the file has no place for shows greyed. So does the root key: the
// consoles don't read it. So does the hold where the Wii's build of NW4R SND doesn't read it. What else an edit changes
// and the sizes follow. Returns the change that the user made.
std::optional<WaveSoundChange> DrawWaveSound(Rows& rows, SoundSection& section);

} // namespace hokuto::app::sound_section_parts
