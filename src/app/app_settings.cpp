// SPDX-License-Identifier: MIT

// The program's settings: the lines that Dear ImGui's settings file keeps for Hokuto, read at start-up and written
// with the layout.

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <string>
#include <string_view>
#include <system_error>

#include "app/app.h"
#include "app/app_internal.h"
#include "app/recent_projects.h"

namespace hokuto::app
{

namespace
{

// The zones' lines in the program's settings: each line's key, and the zone's flag.
struct ZoneSetting
{
    std::string_view key;
    bool OpenViews::* shown;
};
constexpr std::array<ZoneSetting, 5> kZoneSettings = {{{"ShowInspector", &OpenViews::inspector},
                                                       {"ShowBrowser", &OpenViews::browser},
                                                       {"ShowMixer", &OpenViews::mixer},
                                                       {"ShowVariables", &OpenViews::variables},
                                                       {"ShowUndoHistory", &OpenViews::undo_history}}};

} // namespace

void App::AddSettingsHandler()
{
    // The file holds one entry for the program, [Hokuto][Settings], with one setting a line.
    ImGuiSettingsHandler handler;
    handler.TypeName = "Hokuto";
    handler.TypeHash = ImHashStr("Hokuto");
    handler.UserData = this;
    handler.ReadOpenFn = [](ImGuiContext*, ImGuiSettingsHandler* self, const char* name) -> void*
    {
        return std::strcmp(name, "Settings") == 0 ? self->UserData : nullptr;
    };
    handler.ReadLineFn = [](ImGuiContext*, ImGuiSettingsHandler*, void* entry, const char* line)
    {
        static_cast<App*>(entry)->ReadSetting(line);
    };
    handler.WriteAllFn = [](ImGuiContext*, ImGuiSettingsHandler* self, ImGuiTextBuffer* out)
    {
        static_cast<const App*>(self->UserData)->WriteSettings(*out);
    };
    ImGui::AddSettingsHandler(&handler);
}

void App::ReadSetting(std::string_view line)
{
    const std::size_t equals = line.find('=');
    if (equals == std::string_view::npos)
    {
        return;
    }

    const std::string_view key = line.substr(0, equals);
    const std::string_view value = line.substr(equals + 1);
    int number = 0;
    const bool is_number = std::from_chars(value.data(), value.data() + value.size(), number).ec == std::errc();
    for (const ZoneSetting& zone : kZoneSettings)
    {
        if (key == zone.key && is_number)
        {
            views_.*zone.shown = number != 0;
        }
    }
    if (key == "AudioDevice")
    {
        settings_.device = value;
    }
    else if (key == "WindowWidth" && is_number)
    {
        settings_.window_width = number;
    }
    else if (key == "WindowHeight" && is_number)
    {
        settings_.window_height = number;
    }
    else if (key == "LayoutVersion" && is_number)
    {
        settings_.layout_version = number;
    }
    else if (key == "TrackListWidth" && is_number)
    {
        arrangement_.track_list_width = std::clamp(static_cast<float>(number), kMinTrackListWidth, kMaxTrackListWidth);
    }
    else if (key == "TempoTrack" && is_number)
    {
        arrangement_.tempo_row = number != 0;
    }
    else if (key == "RecentProject" && settings_.recent_projects.size() < kMostRecentProjects &&
             std::ranges::none_of(settings_.recent_projects, [&value](const std::string& listed)
                                  { return SameProject(listed, std::string(value)); }))
    {
        settings_.recent_projects.emplace_back(value);
    }
    else if (key == "DsiDigestKey")
    {
        settings_.dsi_key = value;
    }
    else if (key == "TrackColours")
    {
        for (std::size_t i = 0; i < kTrackColourNames.size(); i++)
        {
            if (value == kTrackColourNames[i])
            {
                settings_.track_colours = static_cast<TrackColours>(i);
            }
        }
    }
}

void App::WriteSettings(ImGuiTextBuffer& out) const
{
    out.appendf("[Hokuto][Settings]\n");
    out.appendf("AudioDevice=%s\n", settings_.device.c_str());
    out.appendf("WindowWidth=%d\n", settings_.window_width);
    out.appendf("WindowHeight=%d\n", settings_.window_height);
    out.appendf("LayoutVersion=%d\n", settings_.layout_version);
    out.appendf("TrackListWidth=%d\n", static_cast<int>(std::lround(arrangement_.track_list_width)));
    out.appendf("TempoTrack=%d\n", arrangement_.tempo_row ? 1 : 0);
    for (const ZoneSetting& zone : kZoneSettings)
    {
        out.appendf("%.*s=%d\n", static_cast<int>(zone.key.size()), zone.key.data(), views_.*zone.shown ? 1 : 0);
    }
    for (const std::string& path : settings_.recent_projects)
    {
        out.appendf("RecentProject=%s\n", path.c_str());
    }
    out.appendf("DsiDigestKey=%s\n", settings_.dsi_key.c_str());
    out.appendf("TrackColours=%s\n\n", kTrackColourNames[static_cast<std::size_t>(settings_.track_colours)]);
}

} // namespace hokuto::app
