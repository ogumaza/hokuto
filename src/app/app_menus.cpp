// SPDX-License-Identifier: MIT

// The menus of the main window, the keyboard shortcuts, and the commands that wait for a text field to finish typing.

#include <imgui.h>
#include <imgui_internal.h>

#include <cstddef>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "app/app.h"
#include "app/app_internal.h"
#include "app/theme.h"

namespace hokuto::app
{

namespace
{

using theme::Px;

// Whether `keys` type into a text field. Chords with Ctrl or Command don't, and neither do the function keys.
bool TypesText(ImGuiKeyChord keys)
{
    const auto key = static_cast<ImGuiKey>(keys & ~ImGuiMod_Mask_);
    return (keys & (ImGuiMod_Ctrl | ImGuiMod_Super)) == 0 && (key < ImGuiKey_F1 || key > ImGuiKey_F24);
}

// Calls `visit` with each of `items` and each item of their submenus.
template <typename Visit>
void VisitItems(const std::vector<MenuItem>& items, const Visit& visit)
{
    for (const MenuItem& item : items)
    {
        visit(item);
        VisitItems(item.items, visit);
    }
}

} // namespace

void App::HandleShortcuts()
{
    // Escape closes the menu, the context menu or the list that's open on top. Every system does the same. Dear ImGui
    // does it only with keyboard navigation. Hokuto leaves keyboard navigation off. The windows that wait for an answer
    // take Escape themselves. The key closes the popup alone: nothing else takes it in this frame.
    ImGuiContext& context = *ImGui::GetCurrentContext();
    if (!context.OpenPopupStack.empty() && ImGui::IsKeyPressed(ImGuiKey_Escape, false))
    {
        const ImGuiWindow* top = context.OpenPopupStack.back().Window;
        if (top == nullptr || (top->Flags & ImGuiWindowFlags_Modal) == 0)
        {
            ImGui::ClosePopupToLevel(context.OpenPopupStack.Size - 1, true);
            ImGui::SetKeyOwner(ImGuiKey_Escape, ImGui::GetID("##escape"), ImGuiInputFlags_LockThisFrame);
        }
    }

    // The menus' shortcuts work wherever the mouse is. A text field that's being typed in keeps the keys that type. On
    // the Mac, the menu bar handles the keys with Command.
    const bool typing = ImGui::GetIO().WantTextInput;
    const auto handle = [this, typing](const MenuItem& item)
    {
        if (item.keys == ImGuiKey_None || (native_menus_ && (item.keys & ImGuiMod_Ctrl) != 0) ||
            (typing && TypesText(item.keys)) || !IsEnabled(item.command))
        {
            return;
        }

        const ImGuiInputFlags flags = ImGuiInputFlags_RouteGlobal | (item.repeat ? ImGuiInputFlags_Repeat : 0);
        if (ImGui::Shortcut(item.keys, flags))
        {
            Execute(item.command);
        }
    };
    for (const Menu& menu : Menus())
    {
        VisitItems(menu.items, handle);
    }

    // Return goes to the start too. Keyboards without a keypad need it.
    if (!typing && ImGui::Shortcut(ImGuiKey_Enter, ImGuiInputFlags_RouteGlobal))
    {
        Execute(Command::kGoToStart);
    }
}

void App::DrawMenus()
{
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(Px(10.0f), Px(6.0f)));
    const bool open = ImGui::BeginMainMenuBar();
    ImGui::PopStyleVar();
    if (!open)
    {
        return;
    }

    // A window that waits for an answer, such as the Replace window or the Opening window, keeps the clicks to itself.
    // The menus in the program's window then show what they do: nothing. They grey. The Mac's menu bar greys its items
    // too. The shortcuts of the commands that wait for no window still run.
    const bool waits = ImGui::GetTopMostPopupModal() != nullptr;
    if (waits)
    {
        ImGui::BeginDisabled();
    }
    for (const Menu& menu : Menus())
    {
        if (ImGui::BeginMenu(menu.name))
        {
            DrawMenuItems(menu.items);
            ImGui::EndMenu();
        }
    }
    if (waits)
    {
        ImGui::EndDisabled();
    }

    ImGui::EndMainMenuBar();
}

void App::DrawMenuItems(const std::vector<MenuItem>& items)
{
    for (const MenuItem& item : items)
    {
        if (item.name == nullptr)
        {
            ImGui::Separator();
            continue;
        }

        if (!item.items.empty())
        {
            if (ImGui::BeginMenu(item.name))
            {
                DrawMenuItems(item.items);
                ImGui::EndMenu();
            }
            continue;
        }

        // A listed submenu's items come from Listed each time that it opens. On the Mac, the menu bar's submenus take
        // their items at each opening too.
        if (item.none != nullptr)
        {
            DrawListedMenu(item);
            continue;
        }

        const std::string title = Title(item.command, item.name);
        const std::string keys = item.keys == ImGuiKey_None ? std::string() : ShortcutText(item.keys);
        if (ImGui::MenuItem(title.c_str(), keys.empty() ? nullptr : keys.c_str(), IsChecked(item.command),
                            IsEnabled(item.command)))
        {
            Execute(item.command);
        }
    }
}

void App::DrawListedMenu(const MenuItem& item)
{
    if (ImGui::BeginMenu(item.name))
    {
        const std::vector<std::string> titles = Listed(item.command);
        if (titles.empty())
        {
            ImGui::MenuItem(item.none, nullptr, false, false);
        }
        for (std::size_t i = 0; i < titles.size(); i++)
        {
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::MenuItem(titles[i].c_str(), nullptr, false, IsEnabled(item.command)))
            {
                ExecuteListed(item.command, i);
            }
            ImGui::PopID();
        }
        ImGui::EndMenu();
    }
}

bool App::WaitForTyping(std::function<void()> run)
{
    // The field keeps its text: it ends in the next frame that draws it. A click elsewhere does the same. A save, an
    // export or a question about the changes then includes the text. A minimized window draws no frames. It comes back,
    // and the field ends there.
    if (ImGui::GetInputTextState(ImGui::GetActiveID()) == nullptr)
    {
        return false;
    }

    ImGui::ClearActiveID();
    waiting_commands_.push_back({.run = std::move(run), .frame = ImGui::GetFrameCount()});
    if ((SDL_GetWindowFlags(window_) & SDL_WINDOW_MINIMIZED) != 0)
    {
        SDL_RestoreWindow(window_);
    }

    return true;
}

void App::RunWaitingCommands()
{
    // The commands run at the end of a frame after the one in which they came, once no field is being typed in. A field
    // that the user has started since then ends too.
    if (waiting_commands_.empty())
    {
        return;
    }
    if (ImGui::GetInputTextState(ImGui::GetActiveID()) != nullptr)
    {
        ImGui::ClearActiveID();
        return;
    }
    if (waiting_commands_.front().frame >= ImGui::GetFrameCount())
    {
        return;
    }

    for (const WaitingCommand& waiting : std::exchange(waiting_commands_, {}))
    {
        waiting.run();
    }
}

} // namespace hokuto::app
