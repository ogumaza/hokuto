// SPDX-License-Identifier: MIT

#include "app/theme.h"

#include <algorithm>
#include <cmath>

namespace hokuto::app::theme
{
namespace
{

float display_scale = 1.0f;
Fonts fonts;

ImVec4 Vec(ImU32 colour)
{
    return ImGui::ColorConvertU32ToFloat4(colour);
}

ImVec4 Vec(ImU32 colour, float alpha)
{
    return ImGui::ColorConvertU32ToFloat4(WithAlpha(colour, alpha));
}

} // namespace

const Fonts& GetFonts()
{
    return fonts;
}

void SetFonts(const Fonts& loaded)
{
    fonts = loaded;
}

void Apply(ImGuiStyle& style, float scale)
{
    display_scale = scale;

    // Roomy padding and rounded controls in the manner of a web page. Floating windows get no buttons in their title
    // bars.
    style.WindowPadding = kWindowPadding;
    style.FramePadding = kFramePadding;
    style.CellPadding = ImVec2(8.0f, 7.0f);
    style.ItemSpacing = ImVec2(kItemSpacing, kItemSpacing);
    style.ItemInnerSpacing = ImVec2(8.0f, 6.0f);
    style.IndentSpacing = 18.0f;
    style.ScrollbarSize = 10.0f;
    style.GrabMinSize = 10.0f;
    style.WindowBorderSize = 1.0f;
    style.ChildBorderSize = 0.0f;
    style.PopupBorderSize = 1.0f;
    style.FrameBorderSize = 1.0f;
    style.TabBorderSize = 0.0f;
    style.WindowRounding = 10.0f;
    style.ChildRounding = 8.0f;
    style.FrameRounding = 6.0f;
    style.PopupRounding = 8.0f;
    style.ScrollbarRounding = 6.0f;
    style.GrabRounding = 4.0f;
    style.TabRounding = 6.0f;
    style.WindowTitleAlign = ImVec2(0.5f, 0.5f);
    style.WindowMenuButtonPosition = ImGuiDir_None;
    style.SeparatorTextBorderSize = 1.0f;
    style.DockingSeparatorSize = 3.0f;
    style.ScaleAllSizes(scale);
    style.FontScaleDpi = scale;
    style.FontSizeBase = kTextSize;

    ImVec4* colours = style.Colors;
    colours[ImGuiCol_Text] = Vec(kText);
    // Dear ImGui draws the hints of empty fields and the shortcuts of menus in this colour. A disabled item takes the
    // style's alpha besides.
    colours[ImGuiCol_TextDisabled] = Vec(kTextMuted);
    colours[ImGuiCol_WindowBg] = Vec(kPanel);
    colours[ImGuiCol_ChildBg] = Vec(kPanel, 0.0f);
    colours[ImGuiCol_PopupBg] = Vec(kRaised);
    colours[ImGuiCol_Border] = Vec(kBorder);
    colours[ImGuiCol_BorderShadow] = Vec(kWindow, 0.0f);
    colours[ImGuiCol_FrameBg] = Vec(kControl);
    colours[ImGuiCol_FrameBgHovered] = Vec(kControlHover);
    colours[ImGuiCol_FrameBgActive] = Vec(kControlActive);
    colours[ImGuiCol_TitleBg] = Vec(kRaised);
    colours[ImGuiCol_TitleBgActive] = Vec(kRaised);
    colours[ImGuiCol_TitleBgCollapsed] = Vec(kRaised);
    colours[ImGuiCol_MenuBarBg] = Vec(kPanel);
    colours[ImGuiCol_ScrollbarBg] = Vec(kPanel, 0.0f);
    colours[ImGuiCol_ScrollbarGrab] = Vec(kText, 0.12f);
    colours[ImGuiCol_ScrollbarGrabHovered] = Vec(kText, 0.2f);
    colours[ImGuiCol_ScrollbarGrabActive] = Vec(kText, 0.28f);
    colours[ImGuiCol_CheckMark] = Vec(kAccent);
    colours[ImGuiCol_SliderGrab] = Vec(kAccent);
    colours[ImGuiCol_SliderGrabActive] = Vec(Mix(kAccent, kText, 0.3f));
    colours[ImGuiCol_Button] = Vec(kControl);
    colours[ImGuiCol_ButtonHovered] = Vec(kControlHover);
    colours[ImGuiCol_ButtonActive] = Vec(kControlActive);
    colours[ImGuiCol_Header] = Vec(kAccent, 0.2f);
    colours[ImGuiCol_HeaderHovered] = Vec(kText, 0.07f);
    colours[ImGuiCol_HeaderActive] = Vec(kAccent, 0.3f);
    colours[ImGuiCol_Separator] = Vec(kBorder);
    colours[ImGuiCol_SeparatorHovered] = Vec(kAccent, 0.6f);
    colours[ImGuiCol_SeparatorActive] = Vec(kAccent);
    colours[ImGuiCol_ResizeGrip] = Vec(kAccent, 0.0f);
    colours[ImGuiCol_ResizeGripHovered] = Vec(kAccent, 0.4f);
    colours[ImGuiCol_ResizeGripActive] = Vec(kAccent, 0.7f);
    colours[ImGuiCol_Tab] = Vec(kRaised);
    colours[ImGuiCol_TabHovered] = Vec(kControlHover);
    colours[ImGuiCol_TabSelected] = Vec(kControl);
    colours[ImGuiCol_TabSelectedOverline] = Vec(kAccent);
    colours[ImGuiCol_TabDimmed] = Vec(kRaised);
    colours[ImGuiCol_TabDimmedSelected] = Vec(kControl);
    colours[ImGuiCol_DockingPreview] = Vec(kAccent, 0.3f);
    colours[ImGuiCol_DockingEmptyBg] = Vec(kWindow);
    colours[ImGuiCol_TableHeaderBg] = Vec(kRaised);
    colours[ImGuiCol_TableBorderStrong] = Vec(kBorder);
    colours[ImGuiCol_TableBorderLight] = Vec(kBorder);
    colours[ImGuiCol_TableRowBg] = Vec(kPanel, 0.0f);
    colours[ImGuiCol_TableRowBgAlt] = Vec(kText, 0.02f);
    colours[ImGuiCol_TextSelectedBg] = Vec(kAccent, 0.35f);
    colours[ImGuiCol_NavCursor] = Vec(kAccent);
    colours[ImGuiCol_ModalWindowDimBg] = Vec(kWindow, 0.6f);
    colours[ImGuiCol_InputTextCursor] = Vec(kText);
}

float Px(float points)
{
    return points * display_scale;
}

ImVec2 Px(ImVec2 points)
{
    return ImVec2(Px(points.x), Px(points.y));
}

float FontPx(float points)
{
    return std::round(Px(points));
}

float OnPixels(float units)
{
    const float density = ImGui::GetIO().DisplayFramebufferScale.x;
    return density > 0.0f ? std::round(units * density) / density : units;
}

ImVec2 OnPixels(ImVec2 units)
{
    return ImVec2(OnPixels(units.x), OnPixels(units.y));
}

ImU32 WithAlpha(ImU32 colour, float alpha)
{
    const auto value = static_cast<ImU32>(std::clamp(alpha, 0.0f, 1.0f) * 255.0f + 0.5f);
    return (colour & ~IM_COL32_A_MASK) | (value << IM_COL32_A_SHIFT);
}

ImU32 Mix(ImU32 from, ImU32 to, float amount)
{
    const ImVec4 a = ImGui::ColorConvertU32ToFloat4(from);
    const ImVec4 b = ImGui::ColorConvertU32ToFloat4(to);
    const float t = std::clamp(amount, 0.0f, 1.0f);
    return ImGui::ColorConvertFloat4ToU32(
        ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t));
}

ImU32 TextOn(ImU32 background)
{
    // The background's relative luminance, from its channels in linear light. WCAG measures contrast with it. Above
    // about 0.17, dark text has more contrast than light.
    const ImVec4 colour = ImGui::ColorConvertU32ToFloat4(background);

    const auto linear = [](float channel)
    {
        return channel <= 0.04045f ? channel / 12.92f : std::pow((channel + 0.055f) / 1.055f, 2.4f);
    };
    const float luminance = 0.2126f * linear(colour.x) + 0.7152f * linear(colour.y) + 0.0722f * linear(colour.z);
    return luminance > 0.17f ? kTextOnColour : kText;
}

} // namespace hokuto::app::theme
