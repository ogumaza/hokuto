// SPDX-License-Identifier: MIT

// The main window: start-up and shutdown, the events from SDL3 and each frame. app_settings.cpp contains the settings,
// and app_menus.cpp the menus and the shortcuts. The files app_*.cpp contain the other members of App, one area to a
// file.

#include "app/app.h"

#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlgpu3.h>
#include <imgui_internal.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <future>
#include <initializer_list>
#include <span>
#include <utility>

#include "xsflib/files.h"

#include "app/app_internal.h"
#include "app/engine_names.h"
#include "app/preview_song.h"
#include "app/scene_values.h"
#include "app/sequence_view.h"
#include "app/system_fonts.h"
#include "app/system_ui.h"
#include "app/theme.h"
#include "app/widgets.h"
#include "edit/effects.h"
#include "edit/other_console.h"
#include "format/drivers.h"
#include "format/file.h"
#include "song/archive.h"
#include "song/driver.h"

namespace hokuto::app
{
namespace
{

// The ring holds about a third of a second at 48 kHz. The worker keeps 100 ms in it.
constexpr std::size_t kRingFrames = 16'384;
constexpr std::chrono::milliseconds kAhead{100};
constexpr std::size_t kBlockFrames = 256;

// The window's size on a first run, before the display's scale.
constexpr float kDefaultWidth = 1440.0f;
constexpr float kDefaultHeight = 900.0f;

// After each event, the window draws at the display's rate for a second. The time is in SDL3's nanoseconds. Dear
// ImGui's tooltips appear within it. So do the frames that Dear ImGui spreads a quick click across.
constexpr uint64_t kLinger = 1'000'000'000;

// Debug builds turn on the GPU's validation.
#ifdef NDEBUG
constexpr bool kGpuDebug = false;
#else
constexpr bool kGpuDebug = true;
#endif

// The version of the default layout. A layout saved from an older one lacks zones, and the program rebuilds it.
constexpr int kLayoutVersion = 4;

// Logs what failed with SDL3's reason, and shows both in a message box. A program started from Finder or Explorer has
// no console.
void ShowError(const char* what)
{
    const std::string message = std::string(what) + ": " + SDL_GetError();
    SDL_Log("%s", message.c_str());
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Hokuto", message.c_str(), nullptr);
}

// Whether the user holds a key or a mouse button. Dear ImGui counts the mouse buttons among its keys. Dear ImGui times
// the repeats of a held key by a clock that moves on only in frames.
bool Holding()
{
    for (int key = ImGuiKey_NamedKey_BEGIN; key < ImGuiKey_NamedKey_END; key++)
    {
        if (ImGui::IsKeyDown(static_cast<ImGuiKey>(key)))
        {
            return true;
        }
    }

    return false;
}

// Whether a stereo meter shows a level.
bool Shows(const std::array<float, 2>& meter)
{
    return meter[0] > kSilence || meter[1] > kSilence;
}

// Browser items without children, one for each of `names`.
std::vector<BrowserItem> ItemsNamed(std::initializer_list<std::string> names)
{
    std::vector<BrowserItem> items;
    for (const std::string& name : names)
    {
        BrowserItem item;
        item.name = name;
        items.push_back(std::move(item));
    }

    return items;
}

// Dear ImGui's back end gives SDL a text input area 1 pixel wide at the caret. On Windows, SDL makes that area the
// IME's composition window, and the composition then has no room to show. The area runs from the caret to the
// window's right edge instead.
void (*g_backend_set_ime_data)(ImGuiContext*, ImGuiViewport*, ImGuiPlatformImeData*) = nullptr;

void SetImeData(ImGuiContext* context, ImGuiViewport* viewport, ImGuiPlatformImeData* data)
{
    g_backend_set_ime_data(context, viewport, data);
    SDL_Window* window = SDL_GetKeyboardFocus();
    if (window == nullptr || !data->WantVisible)
    {
        return;
    }

    int width = 0, height = 0;
    SDL_GetWindowSize(window, &width, &height);
    SDL_Rect area;
    area.x = static_cast<int>(data->InputPos.x - viewport->Pos.x);
    area.y = static_cast<int>(data->InputPos.y - viewport->Pos.y);
    area.w = std::max(1, width - area.x);
    area.h = static_cast<int>(data->InputLineHeight);
    SDL_SetTextInputArea(window, &area, 0);
}

} // namespace

std::unique_ptr<App> App::Create(bool preview, const std::string& file)
{
    std::unique_ptr<App> app(new App(preview));
    if (!app->Init())
    {
        return nullptr;
    }

    if (!file.empty())
    {
        app->OpenOrImport(file);
    }

    return app;
}

App::App(bool preview)
    : ring_(kRingFrames),
      worker_(ring_, kAhead, kBlockFrames),
      audition_ring_(kRingFrames),
      audition_worker_(audition_ring_, kLeastAuditionLead, kAuditionBlockFrames),
      output_(ring_, audition_ring_, kDefaultRate),
      file_name_(preview ? "Preview" : kUntitled),
      browser_{{"Sounds", Icon::kSound, theme::kSky, {}},
               {"Sequences", Icon::kSequence, theme::kAccent, {}},
               {"Banks", Icon::kBank, theme::kCycle, {}},
               {"Waves", Icon::kWave, theme::kPlay, {}},
               {"Wave Sound Files", Icon::kSound, theme::kSky, {}}},
      preview_(preview)
{
    if (!preview)
    {
        return;
    }

    // The preview's song. The browser lists names like those in a game's sound archive.
    song_ = MakePreviewSong();
    transport_.SetTempo(song_->tempo);
    transport_.SetBeatsPerBar(song_->beats_per_bar);
    transport_.SetLocators(32.0, 64.0);
    browser_[kSoundsGroup].items = ItemsNamed({"SE_CURSOR", "SE_DECIDE", "SE_CANCEL", "SE_JUMP"});
    browser_[kSequencesGroup].items = ItemsNamed({"SEQ_TITLE", song_->name, "SEQ_FIELD", "SEQ_BATTLE", "SEQ_ENDING"});
    browser_[kBanksGroup].items = ItemsNamed({"BANK_FIELD", "BANK_BATTLE", "BANK_SE"});
    browser_[kBanksGroup].items[0].children = {"Program 0", "Program 1", "Program 5"};
    browser_[kWavesGroup].items = ItemsNamed({"WAVE_BRASS", "WAVE_STRINGS", "WAVE_DRUMS"});
    browser_state_.selected = {static_cast<int>(kSequencesGroup), 1, -1};
    browser_state_.open.insert({static_cast<int>(kBanksGroup), 0});

    // The trumpet's first part is selected. The inspector then shows a part's details.
    arrangement_.selected_part_track = 0;
    arrangement_.selected_part = 0;

    // The song has three scenes. Its variables start with the first one's values. FX A holds a reverb. The history of
    // the made-up song's effects shows three edits a minute apart, with the last one undone.
    format::File file;
    file.scenes = {
        {0, "Calm", {{0, 1}}, {}}, {0, "Storm", {{0, 2}}, {}}, {0, "Last lap", {{0, 1}, {16, 64}}, {{45'000, 0, 2}}}};
    start_values_[0] = SceneStartValues(file.scenes[0]);
    changes_[0] = SceneChanges(file.scenes[0]);
    history_.emplace(std::make_shared<const format::File>(file));
    const EffectPlace place{0, 0};
    const auto now = std::chrono::system_clock::now();
    const auto add = [this, &file, now](const ActionNames& names, int minutes_ago)
    {
        const auto time = now - std::chrono::minutes(minutes_ago);
        history_rows_.push_back({names.name, names.details, TimeOfDay(time)});
        history_->Do({names.name, names.details, time, std::make_shared<const format::File>(file)});
    };

    file.aux_effects[0].push_back(NewEffect(file, file.engine, 0));
    add({"Add Effect", EffectTitle(place, file.aux_effects[0][0])}, 3);
    for (const auto& [parameter, value] : {std::pair<std::size_t, float>{2, 2.5f}, {5, 0.03f}})
    {
        format::Effect changed = file.aux_effects[0][0];
        changed.parameters[parameter] = value;
        const ActionNames names = EffectChangeNames(place, EffectEdit::kChanged, file.aux_effects[0][0], changed);
        file.aux_effects[0][0] = changed;
        add(names, parameter == 2 ? 2 : 1);
    }
    history_->Undo();
}

App::~App()
{
    // The device stops taking frames before anything else closes. An export's measure stops too. Without that, the
    // destruction of its future would wait for the whole render.
    output_.Close();
    DropExport();

    if (gpu_ != nullptr)
    {
        SDL_WaitForGPUIdle(gpu_);
    }
    if (imgui_sdl3_)
    {
        ImGui_ImplSDL3_Shutdown();
    }
    if (imgui_gpu_)
    {
        ImGui_ImplSDLGPU3_Shutdown();
    }

    // Destroying the context saves the layout and the settings.
    if (ImGui::GetCurrentContext() != nullptr)
    {
        ImGui::DestroyContext();
    }

    if (window_claimed_)
    {
        SDL_ReleaseWindowFromGPUDevice(gpu_, window_);
    }
    if (gpu_ != nullptr)
    {
        SDL_DestroyGPUDevice(gpu_);
    }
    if (window_ != nullptr)
    {
        SDL_DestroyWindow(window_);
    }
}

bool App::Init()
{
    // The window's close button quits through its event alone. SDL3 would send SDL_EVENT_QUIT after it, and the
    // question about the document's changes would come twice.
    SDL_SetAppMetadata("Hokuto", HOKUTO_VERSION, HOKUTO_APP_ID);
    SDL_SetHint(SDL_HINT_QUIT_ON_LAST_WINDOW_CLOSE, "0");

    // The display sleeps when the system's settings say. SDL3 keeps it awake by default, for games and players. A
    // display left on can burn in, and nothing in the program requires it.
    SDL_SetHint(SDL_HINT_VIDEO_ALLOW_SCREENSAVER, "1");

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO))
    {
        ShowError("SDL_Init failed");
        return false;
    }

    // Dear ImGui's layout and the program's settings share one file in the user's settings folder. Without the folder,
    // nothing is saved. In a DAW, the keyboard drives commands, not the focus. Floating windows move by their title
    // bars.
    char* folder = SDL_GetPrefPath("", "Hokuto");
    if (folder != nullptr)
    {
        settings_path_ = std::string(folder) + "hokuto.ini";
        SDL_free(folder);
    }
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = settings_path_.empty() ? nullptr : settings_path_.c_str();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigDpiScaleFonts = true;
    io.ConfigWindowsMoveFromTitleBarOnly = true;
    AddSettingsHandler();
    if (io.IniFilename != nullptr)
    {
        ImGui::LoadIniSettingsFromDisk(io.IniFilename);
    }
    if (settings_.layout_version != kLayoutVersion)
    {
        settings_.layout_version = kLayoutVersion;
        reset_layout_ = true;
        views_ = OpenViews();
    }
    saved_views_ = views_;

    float scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    if (scale <= 0.0f)
    {
        scale = 1.0f;
    }
    if (!CreateMainWindow(scale) || !CreateGpu() || !StartImGuiBackends(scale))
    {
        return false;
    }

    LoadFonts();

    // The program runs without an audio device. Studio Setup says why there's none. The device rests after kOutputRest
    // without sound (RestOutput).
    if (!output_.Open(settings_.device))
    {
        SDL_Log("No audio device: %s", output_.Error().c_str());
    }
    output_sounded_ = SDL_GetTicksNS();

    // A quit signal ends a wait for events. The first frames draw at the display's rate. The program counts its start
    // as an event.
    wake_event_ = SDL_RegisterEvents(1);
    WakeOnQuitSignals(wake_event_);
    last_event_ = SDL_GetTicksNS();

    return true;
}

bool App::CreateMainWindow(float scale)
{
    // The main window opens maximised. Restored, the window takes its saved size, or a default size for the display.
    // SDL3 maximises the window when it's shown.
    if (settings_.window_width <= 0 || settings_.window_height <= 0)
    {
        settings_.window_width = static_cast<int>(kDefaultWidth * scale);
        settings_.window_height = static_cast<int>(kDefaultHeight * scale);
    }
    constexpr SDL_WindowFlags kWindowFlags =
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_MAXIMIZED;
    const std::string title = "Hokuto - " + file_name_;
    window_ = SDL_CreateWindow(title.c_str(), settings_.window_width, settings_.window_height, kWindowFlags);
    if (window_ == nullptr)
    {
        ShowError("SDL_CreateWindow failed");
        return false;
    }

    SDL_SetWindowPosition(window_, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    SDL_SetWindowMinimumSize(window_, static_cast<int>(960.0f * scale), static_cast<int>(600.0f * scale));
    SetTitleBarColour(window_, theme::kRaised);
    SDL_ShowWindow(window_);
    native_menus_ = InstallMenuBar(Menus(), *this);

    return true;
}

bool App::CreateGpu()
{
    // The GPU: Metal on the Mac, Direct3D 12 or Vulkan on Windows.
    gpu_ = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXIL | SDL_GPU_SHADERFORMAT_MSL |
                                   SDL_GPU_SHADERFORMAT_METALLIB,
                               kGpuDebug, nullptr);
    if (gpu_ == nullptr)
    {
        ShowError("SDL_CreateGPUDevice failed");
        return false;
    }

    window_claimed_ = SDL_ClaimWindowForGPUDevice(gpu_, window_);
    if (!window_claimed_)
    {
        ShowError("SDL_ClaimWindowForGPUDevice failed");
        return false;
    }

    SDL_SetGPUSwapchainParameters(gpu_, window_, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, SDL_GPU_PRESENTMODE_VSYNC);

    return true;
}

bool App::StartImGuiBackends(float scale)
{
    // The program's look, and Dear ImGui's back ends for SDL3 and SDL_GPU.
    theme::Apply(ImGui::GetStyle(), scale);
    imgui_sdl3_ = ImGui_ImplSDL3_InitForSDLGPU(window_);
    if (imgui_sdl3_)
    {
        g_backend_set_ime_data = ImGui::GetPlatformIO().Platform_SetImeDataFn;
        ImGui::GetPlatformIO().Platform_SetImeDataFn = SetImeData;
    }
    ImGui_ImplSDLGPU3_InitInfo info;
    info.Device = gpu_;
    info.ColorTargetFormat = SDL_GetGPUSwapchainTextureFormat(gpu_, window_);
    imgui_gpu_ = imgui_sdl3_ && ImGui_ImplSDLGPU3_Init(&info);
    if (!imgui_gpu_)
    {
        ShowError("Dear ImGui's back ends failed to start");
        return false;
    }

    return true;
}

void App::LoadFonts()
{
    // The system's fonts. Dear ImGui's font draws the text on a system without them, and the text font takes the place
    // of a font that the system lacks.
    ImGuiIO& io = ImGui::GetIO();
    theme::Fonts fonts = AddSystemFonts(*io.Fonts);
    if (fonts.text == nullptr)
    {
        fonts.text = io.Fonts->AddFontDefault();
    }
    for (ImFont** font : {&fonts.strong, &fonts.caps, &fonts.figures})
    {
        *font = *font != nullptr ? *font : fonts.text;
    }
    io.FontDefault = fonts.text;
    theme::SetFonts(fonts);
}

SDL_AppResult App::HandleEvent(const SDL_Event& event)
{
    // Each event, a wake included, makes the window draw on for a moment (see Pace).
    last_event_ = SDL_GetTicksNS();
    ImGui_ImplSDL3_ProcessEvent(&event);

    // A quit goes through the command Quit. A text field that's being typed in keeps its text. The user then decides
    // about the document's changes. The quit of macOS's menus, the Dock and a logout come as SDL_EVENT_QUIT. The
    // window's close button sends no quit after its event (Init).
    switch (event.type)
    {
    case SDL_EVENT_QUIT:
        Execute(Command::kQuit);
        break;

    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        if (event.window.windowID == SDL_GetWindowID(window_))
        {
            Execute(Command::kQuit);
        }
        break;

    case SDL_EVENT_WINDOW_RESIZED:
        // The size of a maximized window isn't the one to save.
        if ((SDL_GetWindowFlags(window_) & SDL_WINDOW_MAXIMIZED) == 0)
        {
            settings_.window_width = event.window.data1;
            settings_.window_height = event.window.data2;
        }
        break;

    case SDL_EVENT_AUDIO_DEVICE_ADDED:
    case SDL_EVENT_AUDIO_DEVICE_REMOVED:
        if (!event.adevice.recording)
        {
            output_.DevicesChanged();
        }
        break;

    // Files dropped on the Bank Editor wait for the drop to complete. Their waves then make regions or instruments
    // where the first of them landed. Elsewhere, a Hokuto file dropped on the window opens, and a sound archive is
    // imported. So does a file that Finder opens with the program.
    case SDL_EVENT_DROP_BEGIN:
        bank_drops_.clear();
        bank_drop_.reset();
        window_drops_.clear();
        break;

    case SDL_EVENT_DROP_FILE:
        TakeDroppedFile(event.drop);
        break;

    case SDL_EVENT_DROP_COMPLETE:
        FinishDrop();
        break;

    default:
        break;
    }

    // A minimized window draws no frames. The quit can't wait for one.
    return quit_ ? SDL_APP_SUCCESS : SDL_APP_CONTINUE;
}

void App::TakeDroppedFile(const SDL_DropEvent& drop)
{
    if (drop.data == nullptr)
    {
        return;
    }

    if (bank_editor_ && drop.windowID == SDL_GetWindowID(window_) &&
        OverBankEditor(bank_state_, ImVec2(drop.x, drop.y)))
    {
        if (bank_drops_.empty())
        {
            bank_drop_ = DropAt(bank_state_, ImVec2(drop.x, drop.y));
        }
        bank_drops_.emplace_back(drop.data);
    }
    else
    {
        window_drops_.emplace_back(drop.data);
    }
}

void App::FinishDrop()
{
    if (!bank_drops_.empty() && bank_drop_ && !played_.Dse() && !played_.Driver())
    {
        DropFilesOnBank(bank_drops_, *bank_drop_);
    }
    else if (!bank_drops_.empty() && played_.Driver())
    {
        ShowMessage("Hokuto doesn't support adding samples to games with " +
                        std::string(played_.Driver()->Rules().Title()) + " yet.",
                    MessageKind::kNote);
    }
    else if (!bank_drops_.empty() && played_.Dse())
    {
        ShowMessage("Hokuto doesn't support dropping files on DSE banks yet.", MessageKind::kNote);
    }
    else if (!bank_drops_.empty() && played_.Gba())
    {
        ShowMessage("Drag WAV files onto the program list.", MessageKind::kNote);
    }
    else if (!bank_drops_.empty())
    {
        ShowMessage(played_.Ds() ? "Drag WAV or SWAV files onto a region or the program list."
                                 : "Drag WAV or wave files onto the key map or the program list.",
                    MessageKind::kNote);
    }
    bank_drops_.clear();
    bank_drop_.reset();
    if (!window_drops_.empty())
    {
        DropFilesOnWindow(std::exchange(window_drops_, {}));
    }
}

SDL_AppResult App::Iterate()
{
    // A minimized window draws nothing. It waits for the event that restores it.
    if ((SDL_GetWindowFlags(window_) & SDL_WINDOW_MINIMIZED) != 0)
    {
        SetWaiting(true);
        RestOutput();
        return SDL_APP_CONTINUE;
    }

    ImGui_ImplSDLGPU3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    if (const char* window = std::exchange(focus_next_frame_, nullptr))
    {
        ImGui::SetWindowFocus(window);
    }
    HandleShortcuts();

    // A file from a dialog opens, or the file is saved to it, before the frame is drawn.
    // A file chosen for File > Import, other than a sound archive, waits while the program builds the archive of an
    // edit. The import reads the archive of the version that the history has reached.
    // A file chosen for Replace… waits too. The replacement reads the same archive.
    OpenChosenFile();

    // A file whose open has finished shows before the frame is drawn.
    FinishOpening();

    // A measure of the groups that a view dropped goes once it's done. Until then, its lambda holds a version and the
    // version's archive.
    std::erase_if(dropped_group_sizes_, Done<std::vector<std::string>>);
    // So does a dropped read of the songs of a ROM's samples. Until then, the lambda of the read keeps the ROM.
    std::erase_if(dropped_sample_songs_, Done<DriverSampleSongs>);

    // A sequence that the reader has read opens before the frame is drawn too. A read that built the archive of a
    // version brings the archive. A new read of the sequence that the zones show follows an edit.
    TakeReadSequence();

    // The transport, and the mixer's values and the tempo at its position.
    UpdatePlayback();
    if (sequence_ && song_)
    {
        const double ticks = transport_.Position() * sequence_->timebase;
        ShowMixerAt(*song_, *sequence_, ticks);
        transport_.SetTempo(song::TempoAt(*sequence_, static_cast<uint32_t>(std::max(ticks, 0.0))));
    }
    UpdateOutput();
    UpdateMeters();
    UpdateVariables();
    if (!native_menus_)
    {
        DrawMenus();
    }
    DrawControlBar();
    DrawZoneSpace();

    // While the reader builds the archive of another version, the mixer's controls and the Key Editor's notes wait for
    // it.
    SongView* song = song_ ? &*song_ : nullptr;
    const bool busy = reading_version_ != nullptr;
    ShowTrackColours();
    ShowSoundIn(sound_section_, ShownSound());
    ShowWaveSoundIn(sound_section_);
    DrawInspector(song, master_, arrangement_, sound_section_, busy, &views_.inspector);
    ApplySoundChange(sound_section_);
    ApplyTagChange(sound_section_);
    ApplyWaveSoundChange(sound_section_);
    ShowSoundIn(browser_sound_, BrowsedSound());
    ShowWaveSoundIn(browser_sound_);
    DrawBrowser(browser_, file_name_, browser_state_, browser_sound_, &views_.browser);
    ApplySoundChange(browser_sound_);
    ApplyTagChange(browser_sound_);
    ApplyWaveSoundChange(browser_sound_);
    OpenBrowsed();
    ApplyBrowserRequests();

    arrangement_.track_actions = {IsEnabled(Command::kAddTrack), IsEnabled(Command::kDuplicateTracks),
                                  IsEnabled(Command::kRemoveSelectedTracks), IsEnabled(Command::kCopyTrack),
                                  IsEnabled(Command::kPasteTrack)};
    arrangement_.empty_hint = ArrangementHint();
    DrawArrangement(song, transport_, arrangement_, ItemShown(ShownSound()), busy);
    ApplyColourPick();

    ApplyArrangementRequests();
    UpdateMixer(song, busy);
    ApplyInstrumentChoices();
    DrawEffects();
    UpdateBankEditor();
    UpdateProperties();
    UpdateKeyEditor();
    UpdateListEditor();
    UpdateScenes();
    DrawVariables(song, variables_state_, &views_.variables);
    ApplyVariableChoices();
    ApplySceneRequest();
    ApplyChangeRequest();
    ApplyModeRequest();
    ApplyMixingRequest();
    ApplyEngineRequest();
    const std::optional<std::size_t> chosen_place =
        DrawUndoHistory(history_rows_, history_ ? history_->Done() : 0, &views_.undo_history);
    if (chosen_place && history_)
    {
        MoveInHistory([&chosen_place](edit::History& history) { history.GoTo(*chosen_place); });
    }
    DrawWindows();

    // While a sequence is being read, the pointer shows that the program is busy. The window still takes clicks.
    if (reader_.Reading())
    {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Progress);
    }

    // The settings keep which zones show.
    if (views_ != saved_views_)
    {
        saved_views_ = views_;
        ImGui::MarkIniSettingsDirty();
    }

    RunWaitingCommands();
    ImGui::Render();
    Present();
    ShowWaitingMessages();
    Pace();

    return quit_ ? SDL_APP_SUCCESS : SDL_APP_CONTINUE;
}

void App::DrawZoneSpace()
{
    // The zones dock in a space that fills the window below the control bar. Without a saved layout, they take the
    // default one. The window's darkest colour shows between them.
    const ImGuiID dockspace = ImGui::GetID("Zones");
    const ImGuiDockNode* node = ImGui::DockBuilderGetNode(dockspace);
    if (reset_layout_ || node == nullptr || !node->IsSplitNode())
    {
        BuildLayout(dockspace);
        reset_layout_ = false;
    }
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::ColorConvertU32ToFloat4(theme::kWindow));
    ImGui::DockSpaceOverViewport(dockspace, ImGui::GetMainViewport());
    ImGui::PopStyleColor();
}

std::string App::ArrangementHint() const
{
    // Without a sequence sound in the browser, the hint names the menus that add one, or else the File menu.
    if (Document() == nullptr)
    {
        return "Use the File menu to open a file or import a sound archive.";
    }
    if (browser_[kSequencesGroup].items.empty())
    {
        return "This file has no sequence sounds. Use Project > Add Sequence Sound… or File > Import > Sequence… to "
               "add one.";
    }

    return "Choose a sequence in the browser to see its tracks here.";
}

void App::UpdateMixer(SongView* song, bool busy)
{
    // The mixer's FX channels show the effects on the aux buses that both the file's console and the engine that plays
    // the shown sound have. They offer the kinds of that engine. An effect of another console's kind stays idle there:
    // the engine's model leaves it out. A click in the Undo History moves through it.
    const BusEffects* effects = UpdateMixerEffects();

    // The mixer's switches show the shown sound's mixing mode and engine. They refuse a mode that Hokuto doesn't play
    // on the engine, and an engine that Hokuto can't play the sound on. The strips show the sends that both the file's
    // console's tracks and the engine's have.
    if (song != nullptr)
    {
        UpdateMixerSwitches(*song);
    }
    if (FrontOfLowerZone() == LowerZone::kMixer)
    {
        DrawMixer(song, master_, arrangement_, effects != nullptr ? &fx_ : nullptr, busy, &views_.mixer);
    }
    ApplyFxMove();
    ApplyMixMoves();
}

void App::DrawWindows()
{
    DrawStudioSetup();
    DrawPreferences();
    DrawProjectSetup();
    DrawNewProject();
    DrawExport();
    DrawReplace();
    DrawOpening();
    DrawPreparing();
}

void App::ApplyBrowserRequests()
{
    if (const std::optional<BrowserPlace> place = std::exchange(browser_state_.export_place, std::nullopt))
    {
        StartItemExport(*place);
    }
    if (const std::optional<BrowserPlace> place = std::exchange(browser_state_.replace_place, std::nullopt))
    {
        StartReplace(*place);
    }

    // The row's editor takes the place of its preview: a double-click's first click started it.
    if (const std::optional<BrowserPlace> place = std::exchange(browser_state_.edit_place, std::nullopt))
    {
        if (browser_state_.playing == *place)
        {
            worker_.Stop();
            browser_state_.playing.reset();
        }
        OpenEditor(*place);
    }
}

void App::ApplyArrangementRequests()
{
    // A double-click gives the editor the keyboard, also when the open editor shows another track now. The lanes take
    // clicks without an item, and Dear ImGui focuses the window under such a click at the frame's end. The editor
    // therefore takes the focus again at the start of the next frame.
    if (std::exchange(arrangement_.open_editor, false))
    {
        OpenKeyEditor();
        ImGui::SetWindowFocus(kKeyEditor);
        focus_next_frame_ = kKeyEditor;
    }
    if (const std::optional<MarkerPlace> place = std::exchange(arrangement_.open_marker, std::nullopt))
    {
        OpenMarker(*place);
    }
    if (const std::optional<TempoPointView> point = std::exchange(arrangement_.open_tempo, std::nullopt))
    {
        OpenCommands(point->track, std::span(&point->command, 1), point->tick);
    }
    if (const std::optional<TempoMove> move = std::exchange(arrangement_.tempo_move, std::nullopt))
    {
        ApplyTempoMove(*move);
    }
    if (const std::optional<LoopMove> move = std::exchange(arrangement_.loop_move, std::nullopt))
    {
        ApplyLoopMove(*move);
    }
    if (const std::optional<PartEdit> edit = std::exchange(arrangement_.part_edit, std::nullopt))
    {
        ApplyPartEdit(*edit);
    }
    if (const std::optional<TrackAction> action = std::exchange(arrangement_.track_action, std::nullopt))
    {
        EditTracks(*action);
    }
}

const BusEffects* App::UpdateMixerEffects()
{
    const BusEffects* effects = Effects();
    const format::Engine played = PlayedEngine();
    if (effects != nullptr)
    {
        fx_.count = std::min(FxChannelsOf(Document()->engine), FxChannelsOf(played));
        fx_.kinds = EffectKindNames(*Document(), played);
        fx_.idle_note = TheEngine(played) + " doesn't support this effect.";
        fx_.idle_note[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(fx_.idle_note[0])));
    }
    for (std::size_t channel = 0; effects != nullptr && channel < effects->size(); channel++)
    {
        fx_.effects[channel].clear();
        fx_.idle[channel].clear();
        for (const format::Effect& effect : (*effects)[channel])
        {
            fx_.effects[channel].emplace_back(EffectName(effect));
            fx_.idle[channel].push_back(!edit::RunsEffect(played, effect.kind));
        }
    }
    fx_.open = effect_window_;
    FollowBusVolumes();

    return effects;
}

void App::UpdateMixerSwitches(SongView& song)
{
    const format::File* document = Document();
    const format::Engine engine = ShownEngine();
    song.enhanced = ShownMixing() == song::Mixing::kEnhanced;
    song.enhanced_refused =
        document != nullptr ? format::MixingRefused(engine, format::Mixing::kEnhanced).value_or("") : "";
    song.sends = ShownSends();
    song.sends_missing = played_.Gba() || played_.Driver() ? "GBA tracks have no effect sends."
                         : played_.Ds() || played_.Dse()   ? "DS tracks have no effect sends."
                                                           : "3DS tracks send only to FX A and FX B.";
    song.volume_highest = song::OnGbaOrDs(played_) ? std::optional<float>(0.0f) : std::nullopt;
    song.engines = document != nullptr && document->native && sequence_ != nullptr && !preview_;
    song.engine = static_cast<int>(engine);
    song.file_engine = document != nullptr ? static_cast<int>(document->engine) : 0;
    for (std::size_t i = 0; document != nullptr && i < song.engine_refused.size(); i++)
    {
        song.engine_refused[i] = edit::EngineRefused(*document, static_cast<format::Engine>(i)).value_or("");
    }
    song.engine_notes =
        engine_sound_ && PlayedEngine() == engine ? engine_sound_->warnings : std::vector<std::string>();
    song.engine_error = engine_failed_ ? engine_error_ : "";
    song.failed_engine = engine_failed_ ? static_cast<int>(*engine_failed_) : -1;
}

void App::Pace()
{
    // Playback, the meters and the blinking cursor of a text field move. The window draws on while they move, while a
    // file opens, while a sequence is being read, an export is being prepared or measured, an import or the Replace
    // window's Play builds the archive, a preview plays or a thread measures the groups that load an item, while the
    // user holds a key or a button, and for a moment after each event. The end of such work counts as an event. A
    // window that fits its contents, such as the Export window after a measure, takes its new size a frame or two
    // later.
    // The window draws on too while DrawPreparing waits to prepare an Import or Replace window.
    const bool tracks_move = song_ && std::any_of(song_->tracks.begin(), song_->tracks.end(),
                                                  [](const TrackView& track) { return Shows(track.meter); });
    const bool fx_move = std::any_of(fx_.meters.begin(), fx_.meters.end(), Shows);

    const auto measuring = [](const std::future<std::vector<std::string>>& sizes)
    {
        return sizes.valid() && !Done(sizes);
    };
    const bool groups_measured = measuring(bank_group_sizes_) || measuring(wave_group_sizes_) ||
                                 measuring(sound_section_.wave_group_sizes) ||
                                 measuring(browser_sound_.wave_group_sizes);
    const bool moving = transport_.Playing() || Shows(master_.meter) || tracks_move || fx_move ||
                        ImGui::GetIO().WantTextInput || reader_.Reading() || (export_ && !export_->done) ||
                        (export_ && export_->measure.valid()) || (replace_.shown && replace_.shown->pending) ||
                        replace_.preview_work.has_value() || opening_.has_value() || preparing_.has_value() ||
                        browser_state_.playing.has_value() || replace_.previewing || groups_measured;
    if (std::exchange(moving_, moving) && !moving)
    {
        last_event_ = SDL_GetTicksNS();
    }
    SetWaiting(!moving && !Holding() && SDL_GetTicksNS() - last_event_ >= kLinger);
}

void App::SetWaiting(bool waiting)
{
    if (waiting == waiting_)
    {
        return;
    }

    // "0" calls Iterate again at once. Presenting a frame then waits for the display.
    waiting_ = waiting;
    SDL_SetHint(SDL_HINT_MAIN_CALLBACK_RATE, waiting ? "waitevent" : "0");
}

void App::Wake() const
{
    SDL_Event event{};
    event.type = wake_event_;
    SDL_PushEvent(&event);
}

void App::ShowMessage(const std::string& message, MessageKind kind)
{
    if (messages_.Add(message, kind))
    {
        SDL_Log("%s", message.c_str());
        Wake();
    }
}

void App::OpenMessageBoxes(bool button_down)
{
    for (const Message& message : messages_.Ready(button_down))
    {
        SDL_ShowSimpleMessageBox(message.kind == MessageKind::kNote ? SDL_MESSAGEBOX_INFORMATION : SDL_MESSAGEBOX_ERROR,
                                 "Hokuto", message.text.c_str(), window_);
    }
}

void App::ShowWaitingMessages()
{
    OpenMessageBoxes(SDL_GetMouseState(nullptr, nullptr) != 0);
}

void App::ShowMessagesBeforeBox()
{
    if (SDL_GetMouseState(nullptr, nullptr) != 0)
    {
        SDL_Log("A box opens while a mouse button is down. The window may drop the next click.");
    }
    OpenMessageBoxes(false);
}

bool App::ReadChosenFile(const std::filesystem::path& file, std::vector<uint8_t>& bytes)
{
    if (!xsflib::ReadFile(file, bytes))
    {
        const std::string name = xsflib::Utf8(file.filename());
        ShowMessage("Can't read " + name + ".");
        return false;
    }

    return true;
}

void App::UpdateTitle()
{
    const bool modified = history_ && !preview_ && history_->Modified();
    const std::string title = "Hokuto - " + file_name_ + (modified ? "*" : "");
    SDL_SetWindowTitle(window_, title.c_str());
}

const format::File* App::Document() const
{
    return history_ ? history_->Current().get() : nullptr;
}

bool App::ArchiveOpen() const
{
    return static_cast<bool>(played_);
}

void App::Present()
{
    ImDrawData* draw_data = ImGui::GetDrawData();
    const bool minimized = draw_data->DisplaySize.x <= 0.0f || draw_data->DisplaySize.y <= 0.0f;
    SDL_GPUCommandBuffer* commands = SDL_AcquireGPUCommandBuffer(gpu_);
    if (commands == nullptr)
    {
        return;
    }

    // The swapchain has no texture while the window is hidden. The command buffer goes to the GPU anyway.
    SDL_GPUTexture* swapchain = nullptr;
    if (SDL_WaitAndAcquireGPUSwapchainTexture(commands, window_, &swapchain, nullptr, nullptr) &&
        swapchain != nullptr && !minimized)
    {
        // The vertex and index buffers go up before the render pass starts.
        ImGui_ImplSDLGPU3_PrepareDrawData(draw_data, commands);

        SDL_GPUColorTargetInfo target{};
        target.texture = swapchain;
        target.clear_color = SDL_FColor{0.0f, 0.0f, 0.0f, 1.0f};
        target.load_op = SDL_GPU_LOADOP_CLEAR;
        target.store_op = SDL_GPU_STOREOP_STORE;
        SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(commands, &target, 1, nullptr);
        ImGui_ImplSDLGPU3_RenderDrawData(draw_data, commands, pass);
        SDL_EndGPURenderPass(pass);
    }
    SDL_SubmitGPUCommandBuffer(commands);
}

} // namespace hokuto::app
