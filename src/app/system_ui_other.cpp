// SPDX-License-Identifier: MIT

// Systems other than macOS and Windows have no global menu bar. The program draws its menus in the window, and the
// system draws the title bar. The C library writes times of day. zenity draws the message boxes. SDL3 pumps the
// window's events while a box is open.

#include "app/system_ui.h"

#include <array>
#include <clocale>
#include <cstddef>
#include <ctime>
#include <optional>
#include <vector>

#include "app/zenity_box.h"

namespace hokuto::app
{

namespace
{

// The time between two polls of a running program's output and of the window's events, in milliseconds. A box's answer
// arrives within it. GNOME pings a window to learn whether it responds, and waits 5 s for the reply.
constexpr uint32_t kPollEveryMs = 10;

// How a program ended: its exit code, negative for a signal, and what it wrote to its standard output.
struct Ended
{
    int exit_code = -1;
    std::string output;
};

bool SDLCALL KeptDuringBox(void* /*userdata*/, SDL_Event* event)
{
    return !LeftOutDuringBox(event->type);
}

// Runs the program of `arguments` and waits for it to end. Returns nothing when the program can't start. Until the
// program ends or closes its output, SDL3 pumps the window's events every few milliseconds. The pump replies to the
// desktop's pings of the window, and records the keys, the buttons and the window's changes. The events wait in SDL3's
// queue. A filter removes the events that the window leaves out during a box. The program receives the rest once the
// caller returns to SDL3's loop. SDL3's loop calls into the program only from there. No frame and no command runs here.
//
// A box opened from SDL_AppEvent runs the pump inside the delivery of an event. SDL3 delivers events to SDL_AppEvent in
// batches of up to 16. The strings of the events, such as typed text, dropped files and the clipboard's types, are
// SDL3's temporary memory. The pump frees that memory, and with it the strings of the batch's later events. Those
// events reach SDL_AppEvent after the box closes, with their strings freed. The filter also removes the key and mouse
// presses that were in the queue before the box opened.
std::optional<Ended> RunAndWait(const std::vector<std::string>& arguments)
{
    std::vector<const char*> pointers;
    for (const std::string& argument : arguments)
    {
        pointers.push_back(argument.c_str());
    }
    pointers.push_back(nullptr);

    const SDL_PropertiesID properties = SDL_CreateProperties();
    SDL_SetPointerProperty(properties, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, pointers.data());
    SDL_SetNumberProperty(properties, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_APP);
    SDL_Process* const process = SDL_CreateProcessWithProperties(properties);
    SDL_DestroyProperties(properties);
    SDL_IOStream* const written = process != nullptr ? SDL_GetProcessOutput(process) : nullptr;
    if (written == nullptr)
    {
        SDL_DestroyProcess(process);
        return std::nullopt;
    }

    // The output is a stream that doesn't block. After a read of nothing, the stream's status is
    // SDL_IO_STATUS_NOT_READY while the program can still write, and another status once its output is closed. A child
    // of the program can hold the output open after the program ends. The wait then ends with the program, after one
    // more read of the output.
    Ended ended;
    for (;;)
    {
        const bool exited = SDL_WaitProcess(process, false, &ended.exit_code);
        std::array<char, 256> bytes{};
        for (std::size_t count = SDL_ReadIO(written, bytes.data(), bytes.size()); count > 0;
             count = SDL_ReadIO(written, bytes.data(), bytes.size()))
        {
            ended.output.append(bytes.data(), count);
        }
        if (exited || SDL_GetIOStatus(written) != SDL_IO_STATUS_NOT_READY)
        {
            break;
        }

        SDL_PumpEvents();
        SDL_FilterEvents(KeptDuringBox, nullptr);
        SDL_Delay(kPollEveryMs);
    }
    SDL_WaitProcess(process, true, &ended.exit_code);
    SDL_DestroyProcess(process);
    return ended;
}

// What `zenity --version` writes, or nothing on a system where zenity doesn't run. The first answer is kept.
std::optional<std::string> ZenityVersion()
{
    static std::optional<std::string> version;
    if (!version)
    {
        const std::optional<Ended> ended = RunAndWait({"zenity", "--version"});
        if (ended && ended->exit_code == 0)
        {
            version = ended->output;
        }
    }

    return version;
}

// Shows `box` with zenity. Returns false when zenity showed no box.
bool ShowZenityBox(const SDL_MessageBoxData& box, int& chosen)
{
    const std::optional<std::string> version = ZenityVersion();
    if (!version)
    {
        return false;
    }

    // SDL_ShowMessageBox resets the keyboard before a box and raises the focused window after it. This function does
    // the same. It also releases a mouse capture that Dear ImGui took for a held button. The box is another program's
    // window, and a captured mouse can't click its buttons.
    SDL_Window* const focused = SDL_GetKeyboardFocus();
    SDL_ResetKeyboard();
    SDL_CaptureMouse(false);
    const std::optional<Ended> ended = RunAndWait(ZenityArguments(box, *version));
    if (focused != nullptr)
    {
        SDL_RaiseWindow(focused);
    }

    // The filter removed the pointer's moves during the box. A motion event brings the pointer's place to the window
    // under the pointer.
    if (SDL_Window* const under = SDL_GetMouseFocus())
    {
        SDL_Event moved{};
        moved.type = SDL_EVENT_MOUSE_MOTION;
        moved.motion.windowID = SDL_GetWindowID(under);
        moved.motion.state = SDL_GetMouseState(&moved.motion.x, &moved.motion.y);
        SDL_PushEvent(&moved);
    }

    return ended && ReadZenityAnswer(box, ended->exit_code, ended->output, chosen);
}

} // namespace

bool InstallMenuBar(std::span<const Menu> /*menus*/, MenuTarget& /*target*/)
{
    return false;
}

void SetTitleBarColour(SDL_Window* /*window*/, ImU32 /*colour*/)
{
}

void WakeOnQuitSignals(uint32_t /*event_type*/)
{
}

std::string TimeOfDay(std::chrono::system_clock::time_point time)
{
    // %X writes the time in the format of the user's locale for times. The program takes that locale the first time.
    static const bool kLocale = std::setlocale(LC_TIME, "") != nullptr;
    static_cast<void>(kLocale);

    const std::time_t seconds = std::chrono::system_clock::to_time_t(time);
    std::tm local{};
    localtime_r(&seconds, &local);
    std::array<char, 64> text{};
    const std::size_t length = std::strftime(text.data(), text.size(), "%X", &local);
    return std::string(text.data(), length);
}

std::string SystemPath(const std::string& path)
{
    return path;
}

bool ShowSystemBox(const SDL_MessageBoxData& box, int* chosen)
{
    int unused = 0;
    if (ShowZenityBox(box, chosen != nullptr ? *chosen : unused))
    {
        return true;
    }

    // Without zenity, SDL3 draws the box on X11. SDL3's wait for that box pumps no events of the program's window.
    return SDL_ShowMessageBox(&box, chosen);
}

} // namespace hokuto::app
