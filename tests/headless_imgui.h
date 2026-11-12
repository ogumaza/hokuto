// SPDX-License-Identifier: MIT

// Dear ImGui without a display, for the tests that draw the program's windows and widgets.

#pragma once

#include <imgui.h>

#include "app/theme.h"

namespace hokuto::testing
{

// Creates Dear ImGui's context for a display of 1280 by 860 pixels at 60 frames a second, with no settings file, the
// default font in each of the theme's roles and the theme's style. The renderer accepts textures, but nothing draws
// them. With `move_from_title_bar_only`, a window moves only when its title bar is dragged.
inline void CreateHeadlessContext(bool move_from_title_bar_only = false)
{
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1280.0f, 860.0f);
    io.DeltaTime = 1.0f / 60.0f;
    io.IniFilename = nullptr;
    io.ConfigWindowsMoveFromTitleBarOnly = move_from_title_bar_only;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    ImFont* font = io.Fonts->AddFontDefault();
    app::theme::SetFonts({.text = font, .strong = font, .caps = font, .figures = font});
    app::theme::Apply(ImGui::GetStyle(), 1.0f);
}

// Ends a frame that ImGui::NewFrame began. Nothing draws it, and its textures count as made, updated or destroyed.
inline void EndHeadlessFrame()
{
    ImGui::Render();
    for (ImTextureData* texture : *ImGui::GetDrawData()->Textures)
    {
        if (texture->Status == ImTextureStatus_WantCreate)
        {
            texture->SetTexID(1);
            texture->SetStatus(ImTextureStatus_OK);
        }
        else if (texture->Status == ImTextureStatus_WantUpdates)
        {
            texture->SetStatus(ImTextureStatus_OK);
        }
        else if (texture->Status == ImTextureStatus_WantDestroy)
        {
            texture->SetStatus(ImTextureStatus_Destroyed);
        }
    }
}

} // namespace hokuto::testing
