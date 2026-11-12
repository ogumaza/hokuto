// SPDX-License-Identifier: MIT

// The program's icons: those of the transport, the zones, the groups of the browser and the tools of the editors.

#pragma once

#include <imgui.h>

namespace hokuto::app
{

enum class Icon
{
    kGoToStart,
    kRewind,
    kForward,
    kStop,
    kPlay,
    kCycle,
    kInspector,
    kBrowser,
    kVariables,
    kMixer,
    kHistory,
    kSetup,
    kSearch,
    kChevronRight,
    kChevronDown,
    kSound,
    kSequence,
    kBank,
    kWave,
    kPointer,
    kPencil,
    kEraser,
    kSnap,
    kAdd,
};

// Draws `icon` in a square of `size` around `centre`.
void DrawIcon(ImDrawList* draw, Icon icon, ImVec2 centre, float size, ImU32 colour);

} // namespace hokuto::app
