// SPDX-License-Identifier: MIT

// The control bar and the setup windows: Studio Setup with the audio device, Project Setup, New Project and
// Preferences.

#include <imgui_internal.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <exception>
#include <filesystem>
#include <utility>

#include "xsflib/files.h"

#include "app/app.h"
#include "app/app_internal.h"
#include "app/control_bar.h"
#include "app/engine_names.h"
#include "app/theme.h"
#include "app/widgets.h"
#include "edit/other_console.h"
#include "format/archive.h"
#include "format/drivers.h"
#include "format/engines.h"
#include "format/file.h"

namespace hokuto::app
{
namespace
{

using theme::Px;

constexpr char kSystemDefault[] = "System Default";
constexpr char kStudioSetup[] = "Studio Setup";
constexpr char kProjectSetup[] = "Project Setup";
constexpr char kNewProject[] = "New Project";
constexpr char kPreferences[] = "Preferences";

// The build of NW4R SND that a Wii file plays by, and where it came from, for Project Setup. Without the game's
// main.dol, the archive's version gives a build of 2007 or of June 2008.
std::string WiiBuildText(const format::NativeArchive& native)
{
    constexpr std::array<const char*, 12> kMonths = {"January",   "February", "March",    "April",
                                                     "May",       "June",     "July",     "August",
                                                     "September", "October",  "November", "December"};
    const ntdlib::SndBuild build = format::BuildOfWiiArchive(native);
    if (!native.wii_build)
    {
        return std::string("NW4R SND, ") + (build.SinceMarch2008() ? "June 2008" : "2007") +
               " build (from the archive version)";
    }
    if (build.date == 0)
    {
        return build.october_2006
                   ? "NW4R SND, 2006 build with AX of October 2006 (from main.dol)"
                   : "NW4R SND, build without a version banner (from main.dol). Plays as the 2007 build.";
    }

    const uint32_t month = build.date / 100 % 100;
    return "NW4R SND, " + std::to_string(build.date % 100) + " " +
           (month >= 1 && month <= 12 ? kMonths[month - 1] : "?") + " " + std::to_string(build.date / 10000) +
           " build (from main.dol)";
}

// The sound driver that a file of the DS or the GBA plays by, for Project Setup, or nothing on another console. On the
// DS, the build in the image's ARM7 binary sets the level of a volume of 0.
std::optional<std::string> SoundDriverText(const format::File& document)
{
    if (document.engine == format::Engine::kGba)
    {
        return "MusicPlayer2000";
    }
    if (document.engine != format::Engine::kDs)
    {
        return std::nullopt;
    }
    if (!document.native || !document.native->cartridge)
    {
        return "NITRO-Composer, later build (the file has no cartridge image)";
    }

    return std::string("NITRO-Composer, ") +
           (DsBuildOfFile(document).zero_level == ntdlib::ntr::snd::kDefaultBuild.zero_level ? "later" : "earlier") +
           " build (from the ARM7 binary)";
}

// Begins a setup window over the middle of the main window, with a fixed width and an automatic height: Studio Setup,
// Project Setup, New Project or Preferences. Returns whether the window shows. A window that doesn't show has ended.
bool BeginSetupWindow(const char* title, bool* open)
{
    ImGui::SetNextWindowSize(ImVec2(Px(500.0f), 0.0f));
    const bool visible = BeginDialogWindow(title, open, ImGuiWindowFlags_NoResize);
    if (!visible)
    {
        ImGui::End();
    }

    return visible;
}

// The rows of Project Setup that name the engine of `document`, and its sound library or sound driver.
void DrawEngineRows(const format::File& document)
{
    SettingLabel("Engine");
    ImGui::TextUnformatted(EngineName(document.engine));
    if (document.native && document.native->engine == format::Engine::kWii)
    {
        // An import reads the game's build from its main.dol on the extracted disc. A new file, or an import without
        // the main.dol, uses the build that the archive's version indicates.
        SettingLabel("Sound library");
        TextWrappedAtSpaces(WiiBuildText(*document.native));
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(1);
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(theme::kTextMuted));
        TextWrappedAtSpaces(
            "The build determines how sequences play. An import reads it from sys/main.dol on the extracted "
            "disc that contains the archive. Otherwise the archive's version gives it.");
        ImGui::PopStyleColor();
    }
    else
    {
        if (const std::optional<std::string> driver = SoundDriverText(document))
        {
            SettingLabel("Sound driver");
            TextWrappedAtSpaces(*driver);
        }
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(1);
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(theme::kTextMuted));
        ImGui::TextWrapped("The engine switch in the mixer plays a sound on another console engine.");
        ImGui::PopStyleColor();
    }
}

// The Project section of Project Setup: the engine and the mixing mode of `document`. Returns the mixing mode that the
// user chose.
std::optional<format::Mixing> DrawProjectSection(const format::File* document)
{
    std::optional<format::Mixing> mixing;
    if (ImGui::BeginTable("##project", 2))
    {
        ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthFixed, Px(120.0f));
        ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch);
        DrawEngineRows(*document);
        SettingLabel("Mixing");
        constexpr std::array<std::pair<format::Mixing, const char*>, 2> kModes = {
            {{format::Mixing::kStrict, "Strict"}, {format::Mixing::kEnhanced, "Enhanced"}}};
        const bool shared = document->sound_mixing.empty();
        const char* shown = !shared                                         ? "Varies by sound"
                            : document->mixing == format::Mixing::kEnhanced ? "Enhanced"
                                                                            : "Strict";
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::BeginCombo("##mixing", shown))
        {
            for (const auto& [mode, name] : kModes)
            {
                const std::optional<std::string> refused = format::MixingRefused(document->engine, mode);
                if (ImGui::Selectable(name, shared && document->mixing == mode,
                                      refused ? ImGuiSelectableFlags_Disabled : ImGuiSelectableFlags_None))
                {
                    mixing = mode;
                }
                if (refused)
                {
                    ItemTooltip(*refused);
                }
            }
            ImGui::EndCombo();
        }
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(1);
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(theme::kTextMuted));
        std::string about =
            "Mixing mode for all sounds. Strict plays each sound as the console does. Enhanced removes the console "
            "limits. The switch in the mixer sets the mode for the current sound.";
        if (const std::optional<std::string> refused =
                format::MixingRefused(document->engine, format::Mixing::kEnhanced))
        {
            about += " " + *refused;
        }
        else if (std::ranges::any_of(
                     document->sound_engines, [document](const auto& sound)
                     { return sound.second == format::Engine::kWii && !edit::EngineRefused(*document, sound.second); }))
        {
            about += " Sounds on the Wii engine always use strict mixing.";
        }
        ImGui::TextWrapped("%s", about.c_str());
        ImGui::PopStyleColor();
        ImGui::EndTable();
    }

    return mixing;
}

} // namespace

void App::DrawControlBar()
{
    // The bar takes its height from the top of the main window's work area. The menu bar does the same.
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::ColorConvertU32ToFloat4(theme::kRaised));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    constexpr ImGuiWindowFlags kFlags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollWithMouse;
    if (ImGui::BeginViewportSideBar("##ControlBar", ImGui::GetMainViewport(), ImGuiDir_Up, Px(kControlBarHeight),
                                    kFlags))
    {
        // The mixer's button runs the mixer's command: it brings an open mixer to the front of the lower zone too.
        const bool mixer = views_.mixer;
        app::DrawControlBar(views_, transport_, PositionTime(), master_, studio_setup_open_);
        if (views_.mixer != mixer)
        {
            views_.mixer = mixer;
            Execute(Command::kMixer);
        }
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

void App::DrawAudioSection()
{
    const ImVec2 heading = ImGui::GetCursorScreenPos();
    DrawCaps(ImGui::GetWindowDrawList(), heading, theme::kTextMuted, "Audio System");
    ImGui::Dummy(CapsSize("Audio System"));
    ImGui::Spacing();
    if (ImGui::BeginTable("##audio", 2))
    {
        ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthFixed, Px(120.0f));
        ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch);
        SettingLabel("Output device");
        DrawDeviceCombo();

        // The format that SDL3 and the system settled on. Hokuto takes their buffer size.
        const int rate = output_.DeviceRate();
        const int frames = output_.DeviceBufferFrames();
        if (output_.IsOpen() && rate > 0 && frames > 0)
        {
            SettingLabel("Sample rate");
            ImGui::Text("%.1f kHz", rate / 1000.0);
            SettingLabel("Buffer size");
            ImGui::Text("%d samples, %.1f ms", frames, 1000.0 * frames / rate);
        }
        else if (!output_.IsOpen())
        {
            SettingLabel("Status");
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme::kSolo), "No device: %s", output_.Error().c_str());
        }

        SettingLabel("Test tone");
        if (PrimaryButton("Play"))
        {
            PlayTestTone();
        }
        ImGui::EndTable();
    }
}

void App::DrawDsKeySection()
{
    const ImVec2 ds = ImGui::GetCursorScreenPos();
    DrawCaps(ImGui::GetWindowDrawList(), ds, theme::kTextMuted, "DS");
    ImGui::Dummy(CapsSize("DS"));
    ImGui::Spacing();
    if (ImGui::BeginTable("##ds", 2))
    {
        ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthFixed, Px(120.0f));
        ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch);
        SettingLabel("DSi digest key");
        const std::string name = settings_.dsi_key.empty()
                                     ? std::string("None")
                                     : xsflib::Utf8(xsflib::PathFromUtf8(settings_.dsi_key).filename());
        ImGui::TextUnformatted(name.c_str());
        ItemTooltip(settings_.dsi_key);
        ImGui::SameLine();
        if (ImGui::Button("Choose…"))
        {
            ShowDialog(Dialog::kDsiKey);
        }
        if (!settings_.dsi_key.empty())
        {
            ImGui::SameLine();
            if (ImGui::Button("Clear"))
            {
                SetDsiKey("");
            }
        }

        // What the key does, and where Hokuto keeps its location.
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(1);
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(theme::kTextMuted));
        ImGui::TextWrapped(
            "DSi-enhanced images contain hashes of their data. Hokuto uses this 64-byte key to recompute them when "
            "it exports such an image. Hokuto remembers the location of the key file on this computer. The key is "
            "never saved in a .hok file.");
        ImGui::PopStyleColor();
        ImGui::EndTable();
    }
}

void App::DrawStudioSetup()
{
    if (!studio_setup_open_)
    {
        return;
    }

    // A window over the middle of the main window. The labels sit on the left.
    if (!BeginSetupWindow(kStudioSetup, &studio_setup_open_))
    {
        return;
    }

    // Escape closes the window while it has the focus, unless a text field is being typed in. The New Project window
    // does the same. An open menu of the window takes Escape first (HandleShortcuts).
    if (EscapeCloses())
    {
        studio_setup_open_ = false;
    }

    // The audio device.
    DrawAudioSection();

    // The DSi's digest key, for an export into a DS image that also runs on the DSi.
    ImGui::Spacing();
    ImGui::Spacing();
    DrawDsKeySection();
    ImGui::End();
}

void App::SetDsiKey(const std::string& path)
{
    if (!path.empty())
    {
        std::vector<uint8_t> bytes;
        if (!xsflib::ReadFile(xsflib::PathFromUtf8(path), bytes))
        {
            ShowMessage("Can't read " + xsflib::Utf8(xsflib::PathFromUtf8(path).filename()) + ".");
            return;
        }
        try
        {
            format::DsiKeysOf(bytes);
        }
        catch (const std::exception& error)
        {
            ShowMessage(error.what());
            return;
        }
    }
    settings_.dsi_key = path;
    ImGui::MarkIniSettingsDirty();
}

std::optional<std::pair<std::string, std::string>> App::DrawTagsSection(const format::File* document)
{
    std::optional<std::pair<std::string, std::string>> change;
    if (ImGui::BeginTable("##tags", 2))
    {
        ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthFixed, Px(120.0f));
        ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch);
        for (const std::string_view key_view : format::kTagKeys)
        {
            const std::string key(key_view);
            const std::string label = TagLabel(key);
            SettingLabel(label.c_str());
            ItemTooltip(TagTip(key));
            const auto found = document->tags.find(key);
            const std::string_view value = found != document->tags.end() ? std::string_view(found->second) : "";
            std::optional<std::string> typed = TextEntry(key.c_str(), value, ImGui::GetCursorScreenPos(),
                                                         ImGui::GetContentRegionAvail().x, project_typing_);
            if (typed && *typed != value)
            {
                change.emplace(key, std::move(*typed));
            }
        }
        ImGui::EndTable();
    }

    return change;
}

void App::DrawProjectSetup()
{
    const format::File* document = Document();
    if (document == nullptr || preview_)
    {
        project_setup_open_ = false;
    }
    if (!project_setup_open_)
    {
        return;
    }

    // A window over the middle of the main window, like Studio Setup.
    if (!BeginSetupWindow(kProjectSetup, &project_setup_open_))
    {
        return;
    }

    // Escape closes the window while it has the focus, unless a text field is being typed in. The New Project window
    // does the same. An open menu of the window takes Escape first (HandleShortcuts).
    if (EscapeCloses())
    {
        project_setup_open_ = false;
    }

    // The console whose rules the file plays by, and how its sounds mix. The file's engine can't change. The mixer's
    // switch plays the shown sound on another console's engine. A choice of a mixing mode gives it to every sound. The
    // mixer's switch gives the shown sound another. A mode that Hokuto doesn't play on the file's console shows grey,
    // and its tooltip says why.
    DrawCaps(ImGui::GetWindowDrawList(), ImGui::GetCursorScreenPos(), theme::kTextMuted, "Project");
    ImGui::Dummy(CapsSize("Project"));
    ImGui::Spacing();
    const std::optional<format::Mixing> mixing = DrawProjectSection(document);

    // The file's tags, a box of text for each key that the program offers. A box that the user empties removes its tag.
    ImGui::Spacing();
    ImGui::Spacing();
    DrawCaps(ImGui::GetWindowDrawList(), ImGui::GetCursorScreenPos(), theme::kTextMuted, "Tags");
    ImGui::Dummy(CapsSize("Tags"));
    ImGui::Spacing();
    const std::optional<std::pair<std::string, std::string>> change = DrawTagsSection(document);
    ImGui::End();

    if (change)
    {
        Do("Change Tag", "File: " + file_name_ + " - " + TagLabel(change->first),
           format::WithTag(*document, std::nullopt, change->first, change->second));
    }
    else if (mixing && (!document->sound_mixing.empty() || *mixing != document->mixing))
    {
        Do(*mixing == format::Mixing::kEnhanced ? "Enhanced Mixing" : "Strict Mixing", "File: " + file_name_,
           format::WithMixing(*document, *mixing));
    }
}

void App::DrawNewProjectEngine()
{
    // The console whose rules the new file plays by. Project Setup shows it as the engine.
    DrawCaps(ImGui::GetWindowDrawList(), ImGui::GetCursorScreenPos(), theme::kTextMuted, "Project");
    ImGui::Dummy(CapsSize("Project"));
    ImGui::Spacing();
    if (ImGui::BeginTable("##new", 2))
    {
        ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthFixed, Px(120.0f));
        ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch);
        SettingLabel("Engine");
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::BeginCombo("##engine", EngineName(new_project_engine_)))
        {
            // The consoles of a new file: the engines that format::NewFile makes a file of. A file of the GBA comes
            // from a game's ROM.
            for (const format::EngineFacts& facts : format::kEngineFacts)
            {
                if (facts.creatable && ImGui::Selectable(EngineName(facts.engine), facts.engine == new_project_engine_))
                {
                    new_project_engine_ = facts.engine;
                }
            }
            ImGui::EndCombo();
        }
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(1);
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(theme::kTextMuted));
        ImGui::TextWrapped(
            new_project_engine_ == format::Engine::kDs
                ? "An empty DS sound archive. It exports as a standalone SDAT. Use Project > Add Sequence Sound… to "
                  "add a sound with an empty song on the empty bank. Use File > Import to add banks, wave archives "
                  "and other items."
                : "An empty sound archive for this console. Use Project > Add Sequence Sound… to add a sound with an "
                  "empty song. Use File > Import to add banks, wave archives and other items.");
        ImGui::PopStyleColor();
        ImGui::EndTable();
    }
}

void App::DrawNewProject()
{
    if (!new_project_open_)
    {
        return;
    }

    // A window over the middle of the main window, like Project Setup.
    if (!BeginSetupWindow(kNewProject, &new_project_open_))
    {
        return;
    }

    // Escape cancels while the window has the focus, unless a text field is being typed in. The Import window does the
    // same. An open menu of the window takes Escape first (HandleShortcuts).
    if (EscapeCloses())
    {
        new_project_open_ = false;
    }

    DrawNewProjectEngine();

    // Cancel and Create sit at the right. The Import window's buttons sit there too.
    ImGui::Spacing();
    ImGui::Spacing();
    const ImGuiStyle& style = ImGui::GetStyle();
    const float buttons = ImGui::CalcTextSize("Cancel").x + ImGui::CalcTextSize("Create").x +
                          style.FramePadding.x * 4.0f + style.ItemSpacing.x;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - buttons);
    bool create = false;
    if (ImGui::Button("Cancel"))
    {
        new_project_open_ = false;
    }
    ImGui::SameLine();
    if (PrimaryButton("Create"))
    {
        create = true;
    }
    ImGui::End();

    // The user decides about the document's changes once Create replaces it, edits made meanwhile included.
    if (create)
    {
        new_project_open_ = false;
        AfterChanges([this, engine = new_project_engine_] { NewDocument(engine); });
    }
}

LowerZone App::FrontOfLowerZone() const
{
    const bool key_editor = key_editor_open_ && sequence_;
    const bool list_editor = list_editor_open_ && sequence_;
    if ((lower_front_ == LowerZone::kMixer && views_.mixer) || (lower_front_ == LowerZone::kKeyEditor && key_editor) ||
        (lower_front_ == LowerZone::kListEditor && list_editor))
    {
        return lower_front_;
    }

    return key_editor ? LowerZone::kKeyEditor : list_editor ? LowerZone::kListEditor : LowerZone::kMixer;
}

void App::DrawPreferences()
{
    if (!preferences_open_)
    {
        return;
    }

    // A window over the middle of the main window, like Studio Setup.
    if (!BeginSetupWindow(kPreferences, &preferences_open_))
    {
        return;
    }

    // Escape closes the window while it has the focus, unless a text field is being typed in. The New Project window
    // does the same. An open menu of the window takes Escape first (HandleShortcuts).
    if (EscapeCloses())
    {
        preferences_open_ = false;
    }

    // The tracks' colours.
    const ImVec2 heading = ImGui::GetCursorScreenPos();
    DrawCaps(ImGui::GetWindowDrawList(), heading, theme::kTextMuted, "Tracks");
    ImGui::Dummy(CapsSize("Tracks"));
    ImGui::Spacing();
    if (ImGui::BeginTable("##tracks", 2))
    {
        ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthFixed, Px(120.0f));
        ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch);
        SettingLabel("Colours");
        ImGui::SetNextItemWidth(-FLT_MIN);
        const auto chosen = static_cast<std::size_t>(settings_.track_colours);
        if (ImGui::BeginCombo("##colours", kTrackColourNames[chosen]))
        {
            for (std::size_t i = 0; i < kTrackColourNames.size(); i++)
            {
                if (ImGui::Selectable(kTrackColourNames[i], i == chosen))
                {
                    settings_.track_colours = static_cast<TrackColours>(i);
                    ImGui::MarkIniSettingsDirty();
                }
            }
            ImGui::EndCombo();
        }

        // What the choice does.
        constexpr std::array<const char*, 3> kHints = {
            "Each track gets the next colour of the rainbow.",
            "All tracks are the same grey.",
            "Click the colour strip of a track, or its colour in the inspector, to choose a colour. The file saves it.",
        };
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(1);
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(theme::kTextMuted));
        ImGui::TextWrapped("%s", kHints[static_cast<std::size_t>(settings_.track_colours)]);
        ImGui::PopStyleColor();
        ImGui::EndTable();
    }
    ImGui::End();
}

void App::DrawDeviceCombo()
{
    // Devices can share a name. Their indexes tell them apart.
    const char* preview = settings_.device.empty() ? kSystemDefault : settings_.device.c_str();
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (!ImGui::BeginCombo("##device", preview))
    {
        return;
    }

    if (ImGui::Selectable(kSystemDefault, settings_.device.empty()))
    {
        ChooseDevice("");
    }
    const std::vector<std::string> names = AudioOutput::DeviceNames();
    for (std::size_t i = 0; i < names.size(); i++)
    {
        ImGui::PushID(static_cast<int>(i));
        if (ImGui::Selectable(names[i].c_str(), names[i] == settings_.device))
        {
            ChooseDevice(names[i]);
        }
        ImGui::PopID();
    }
    ImGui::EndCombo();
}

void App::ChooseDevice(const std::string& name)
{
    settings_.device = name;
    output_.Open(name);
    ImGui::MarkIniSettingsDirty();
}

} // namespace hokuto::app
