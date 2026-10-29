// SPDX-License-Identifier: MIT

#include "app/control_bar.h"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <string>
#include <vector>

#include "app/theme.h"
#include "app/widgets.h"

namespace hokuto::app
{
namespace
{

using theme::Px;

// One field of the display: a value over its label.
struct Field
{
    const char* label;
    std::string value;
    float width;
    float size;
};

void DrawDisplay(ImDrawList* draw, ImVec2 pos, ImVec2 size, const Transport& transport, double time)
{
    draw->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), theme::kDisplay, Px(9.0f));
    draw->AddRect(pos, ImVec2(pos.x + size.x, pos.y + size.y), theme::kBorder, Px(9.0f), Px(1.0f));

    // The signature shows only with bars. The other fields then share its width.
    std::array<char, 16> tempo{};
    std::snprintf(tempo.data(), tempo.size(), "%.3f", transport.Tempo());
    std::vector<Field> fields = {
        Field{"Position", PositionText(transport.Position(), transport.BeatsPerBar()), 0.34f, theme::kPositionSize},
        Field{"Time", TimeText(WholeMs(time)), 0.26f, theme::kTextSize},
        Field{"Tempo", tempo.data(), 0.19f, theme::kTextSize},
    };
    if (kShowBars)
    {
        std::array<char, 16> signature{};
        std::snprintf(signature.data(), signature.size(), "%d/4", transport.BeatsPerBar());
        fields.push_back(Field{"Signature", signature.data(), 0.21f, theme::kTextSize});
    }
    float total = 0.0f;
    for (const Field& field : fields)
    {
        total += field.width;
    }

    // The values sit on one line, with their labels below.
    ImFont* figures = theme::GetFonts().figures;
    const float label_top = pos.y + size.y - Px(7.0f) - CapsSize("A").y;
    float x = pos.x;
    for (std::size_t i = 0; i < fields.size(); i++)
    {
        const Field& field = fields[i];
        const float width = size.x * field.width / total;
        const ImVec2 value_size = TextSize(figures, field.size, field.value);
        const float value_bottom = label_top - Px(1.0f);
        DrawText(draw, figures, field.size, ImVec2(x + (width - value_size.x) * 0.5f, value_bottom - value_size.y),
                 theme::kText, field.value);
        const ImVec2 label_size = CapsSize(field.label);
        DrawCaps(draw, ImVec2(x + (width - label_size.x) * 0.5f, label_top), theme::kTextMuted, field.label);
        if (i + 1 < fields.size())
        {
            draw->AddRectFilled(ImVec2(std::floor(x + width), pos.y + Px(9.0f)),
                                ImVec2(std::floor(x + width) + Px(1.0f), pos.y + size.y - Px(9.0f)), theme::kBorder);
        }
        x += width;
    }
}

void DrawTransportButtons(ImVec2 pos, Transport& transport)
{
    const ImVec2 button(Px(34.0f), Px(32.0f));
    ImGui::SetCursorScreenPos(pos);
    if (IconButton("##start", Icon::kGoToStart, button, false, 0, "Go to Project Start"))
    {
        transport.GoToStart();
    }
    ImGui::SameLine(0.0f, 0.0f);
    if (IconButton("##rewind", Icon::kRewind, button, false, 0, kShowBars ? "Back a Bar" : "Rewind"))
    {
        transport.Step(-1);
    }
    ImGui::SameLine(0.0f, 0.0f);
    if (IconButton("##forward", Icon::kForward, button, false, 0, kShowBars ? "Forward a Bar" : "Forward"))
    {
        transport.Step(1);
    }
    ImGui::SameLine(0.0f, 0.0f);
    if (IconButton("##stop", Icon::kStop, button, !transport.Playing(), theme::kText, "Stop"))
    {
        transport.Stop();
    }
    ImGui::SameLine(0.0f, 0.0f);
    if (IconButton("##play", Icon::kPlay, button, transport.Playing(), theme::kPlay, "Start (Space starts and stops)"))
    {
        transport.Start();
    }
    ImGui::SameLine(0.0f, 0.0f);
    if (IconButton("##cycle", Icon::kCycle, button, transport.Cycle(), theme::kCycle, "Cycle"))
    {
        transport.SetCycle(!transport.Cycle());
    }
}

} // namespace

void DrawControlBar(OpenViews& views, Transport& transport, double time, const MasterBus& master,
                    bool& studio_setup_open)
{
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 pos = ImGui::GetWindowPos();
    const ImVec2 size = ImGui::GetWindowSize();
    const float middle = pos.y + size.y * 0.5f;
    draw->AddRectFilled(ImVec2(pos.x, pos.y + size.y - Px(1.0f)), ImVec2(pos.x + size.x, pos.y + size.y),
                        theme::kBorder);

    // The buttons of the zones on the left and at the bottom: the inspector and the mixer. They sit at the bar's left
    // end.
    const ImVec2 square(Px(34.0f), Px(34.0f));
    const float button_top = middle - square.y * 0.5f;
    const std::string inspector_tip = std::string("Inspector (") + ShortcutText(kInspectorKeys) + ")";
    const std::string mixer_tip = std::string("Mixer (") + ShortcutText(kMixerKeys) + ")";
    ImGui::SetCursorScreenPos(ImVec2(pos.x + Px(12.0f), button_top));
    if (IconButton("##inspector", Icon::kInspector, square, views.inspector, theme::kAccent, inspector_tip.c_str()))
    {
        views.inspector = !views.inspector;
    }
    ImGui::SameLine(0.0f, Px(2.0f));
    if (IconButton("##mixer", Icon::kMixer, square, views.mixer, theme::kAccent, mixer_tip.c_str()))
    {
        views.mixer = !views.mixer;
    }

    // The buttons of the zones on the right, at the bar's right end: the browser, the variables and the undo history.
    // The zones sit in that order from the top.
    const float right = pos.x + size.x - Px(12.0f);
    const float right_buttons = square.x * 3.0f + Px(4.0f);
    const std::string browser_tip = std::string("Browser (") + ShortcutText(kBrowserKeys) + ")";
    ImGui::SetCursorScreenPos(ImVec2(right - right_buttons, button_top));
    if (IconButton("##browser", Icon::kBrowser, square, views.browser, theme::kAccent, browser_tip.c_str()))
    {
        views.browser = !views.browser;
    }
    ImGui::SameLine(0.0f, Px(2.0f));
    if (IconButton("##variables", Icon::kVariables, square, views.variables, theme::kAccent, "Variables"))
    {
        views.variables = !views.variables;
    }
    ImGui::SameLine(0.0f, Px(2.0f));
    if (IconButton("##history", Icon::kHistory, square, views.undo_history, theme::kAccent, "Undo History"))
    {
        views.undo_history = !views.undo_history;
    }

    // Studio Setup, to the left of the zones' buttons.
    const float setup_right = right - right_buttons - Px(20.0f);
    const float setup_left = setup_right - square.x;
    ImGui::SetCursorScreenPos(ImVec2(setup_left, button_top));
    if (IconButton("##setup", Icon::kSetup, square, studio_setup_open, theme::kAccent, "Studio Setup"))
    {
        studio_setup_open = !studio_setup_open;
    }

    // The transport sits in a group of its own. The display follows the group, and the two are centred together. In a
    // narrow window, they move left to keep clear of Studio Setup.
    const float group_width = Px(34.0f) * 6.0f + Px(8.0f);
    const float group_height = Px(40.0f);
    const ImVec2 display_size(Px(440.0f), Px(44.0f));
    const float gap = Px(12.0f);
    const float transport_width = group_width + gap + display_size.x;
    const float centred = std::floor(pos.x + (size.x - transport_width) * 0.5f);
    const float left = std::min(centred, setup_left - Px(16.0f) - transport_width);
    const ImVec2 group(left, std::floor(middle - group_height * 0.5f));
    draw->AddRectFilled(group, ImVec2(group.x + group_width, group.y + group_height), theme::kPanel, Px(10.0f));
    draw->AddRect(group, ImVec2(group.x + group_width, group.y + group_height), theme::kBorder, Px(10.0f), Px(1.0f));
    DrawTransportButtons(ImVec2(group.x + Px(4.0f), group.y + Px(4.0f)), transport);
    DrawDisplay(draw, ImVec2(group.x + group_width + gap, std::floor(middle - display_size.y * 0.5f)), display_size,
                transport, time);

    // The output's meter fills the room between the display and Studio Setup, up to 150 points. With less than 40
    // points of room, the bar leaves it out.
    const ImVec2 label_size = CapsSize("Out");
    const float meter_right = setup_left - Px(14.0f);
    const float meter_left =
        std::max(meter_right - Px(150.0f), left + transport_width + Px(16.0f) + label_size.x + Px(8.0f));
    if (meter_right - meter_left < Px(40.0f))
    {
        return;
    }

    const float bar_height = Px(5.0f);
    DrawHorizontalMeter(draw, ImVec2(meter_left, middle - bar_height - Px(1.5f)),
                        ImVec2(meter_right, middle - Px(1.5f)), master.meter[0]);
    DrawHorizontalMeter(draw, ImVec2(meter_left, middle + Px(1.5f)),
                        ImVec2(meter_right, middle + bar_height + Px(1.5f)), master.meter[1]);
    DrawCaps(draw, ImVec2(meter_left - Px(8.0f) - label_size.x, middle - label_size.y * 0.5f), theme::kTextMuted,
             "Out");
}

} // namespace hokuto::app
