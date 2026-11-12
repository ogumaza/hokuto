// SPDX-License-Identifier: MIT

// The layout of the zones in the main window. The files of the zones draw them.

#include "app/views.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cstddef>

#include "app/theme.h"
#include "app/zone_parts.h"

namespace hokuto::app
{
namespace
{

using theme::Px;
using zone_parts::kInspectorWidth;
using zone_parts::kRightZoneWidth;

} // namespace

void BuildLayout(ImGuiID dockspace)
{
    // Start from an empty node the size of the main window's work area. The splits take their sizes from it.
    const ImVec2 size = ImGui::GetMainViewport()->WorkSize;
    ImGui::DockBuilderRemoveNode(dockspace);
    ImGui::DockBuilderAddNode(dockspace, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspace, size);

    // The zones at the sides take their widths in points, and the arrangement takes the rest. The arrangement also
    // takes the room that the window gains or loses. The mixer takes part of the middle's height.
    ImGuiID middle = dockspace;
    const float left_width = std::min(Px(kInspectorWidth), size.x * 0.3f);
    const float right_width = std::min(Px(kRightZoneWidth), size.x * 0.3f);
    const ImGuiID left = ImGui::DockBuilderSplitNode(middle, ImGuiDir_Left, left_width / size.x, nullptr, &middle);
    ImGuiID right =
        ImGui::DockBuilderSplitNode(middle, ImGuiDir_Right, right_width / (size.x - left_width), nullptr, &middle);
    const ImGuiID right_bottom = ImGui::DockBuilderSplitNode(right, ImGuiDir_Down, 0.3f, nullptr, &right);
    const ImGuiID right_middle = ImGui::DockBuilderSplitNode(right, ImGuiDir_Down, 0.36f, nullptr, &right);
    const ImGuiID bottom = ImGui::DockBuilderSplitNode(middle, ImGuiDir_Down, 0.4f, nullptr, &middle);

    ImGui::DockBuilderDockWindow(kInspector, left);
    ImGui::DockBuilderDockWindow(kBrowser, right);
    ImGui::DockBuilderDockWindow(kVariables, right_middle);
    ImGui::DockBuilderDockWindow(kUndoHistory, right_bottom);
    ImGui::DockBuilderDockWindow(kArrangement, middle);
    ImGui::DockBuilderDockWindow(kMixer, bottom);
    ImGui::DockBuilderDockWindow(kKeyEditor, bottom);
    ImGui::DockBuilderDockWindow(kListEditor, bottom);
    ImGui::DockBuilderFinish(dockspace);
}

void DockWithMixer(const char* window)
{
    // The mixer's node: the window's, or else its place in the settings.
    const ImGuiID id = ImHashStr(window);
    if (ImGui::FindWindowByID(id) != nullptr || ImGui::FindWindowSettingsByID(id) != nullptr)
    {
        return;
    }
    ImGuiID node = 0;
    if (const ImGuiWindow* mixer = ImGui::FindWindowByName(kMixer))
    {
        node = mixer->DockId;
    }
    else if (const ImGuiWindowSettings* settings = ImGui::FindWindowSettingsByID(ImHashStr(kMixer)))
    {
        node = settings->DockId;
    }
    if (node != 0)
    {
        ImGui::SetNextWindowDockID(node, ImGuiCond_FirstUseEver);
    }
}

} // namespace hokuto::app
