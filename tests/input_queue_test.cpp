// SPDX-License-Identifier: MIT

// Tests the order and the frames in which Dear ImGui receives the window's input (app/input_queue.h), without a
// display: InputQueue, and the pointer's place after Dear ImGui cleared it or the window dropped the pointer's moves
// (ShowPointerPlace). The events are SDL3's structures, made by hand. A stand-in for Dear ImGui's back end gives them
// to Dear ImGui's queue of events as the back end does: a key event first sets the modifiers that it names.

#include "app/input_queue.h"

#include <imgui.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "xsflib/testing/check.h"

#include "headless_imgui.h"

namespace
{

namespace app = hokuto::app;

constexpr uint32_t kWake = SDL_EVENT_USER;

SDL_Event Key(SDL_Scancode scancode, bool down, SDL_Keymod mod, bool repeat = false)
{
    SDL_Event event{};
    event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    event.key.scancode = scancode;
    event.key.mod = mod;
    event.key.down = down;
    event.key.repeat = repeat;
    return event;
}

SDL_Event Text(const char* text)
{
    SDL_Event event{};
    event.type = SDL_EVENT_TEXT_INPUT;
    event.text.text = text;
    return event;
}

SDL_Event Of(uint32_t type)
{
    SDL_Event event{};
    event.type = type;
    return event;
}

const char* NameOf(SDL_Scancode scancode)
{
    switch (scancode)
    {
    case SDL_SCANCODE_A:
        return "A";

    case SDL_SCANCODE_B:
        return "B";

    case SDL_SCANCODE_Z:
        return "Z";

    case SDL_SCANCODE_RETURN:
        return "Return";

    default:
        return "?";
    }
}

// What an event shows of itself in a test's record: "+Z" and "-Z" for a key's press and release, the text of a text
// event in quotes, and a word for the others.
std::string Shown(const SDL_Event& event)
{
    switch (event.type)
    {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
        return std::string(event.key.down ? "+" : "-") + NameOf(event.key.scancode);

    case SDL_EVENT_TEXT_INPUT:
        return std::string("\"") + event.text.text + "\"";

    case SDL_EVENT_MOUSE_MOTION:
        return "move";

    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        return "button";

    case SDL_EVENT_WINDOW_FOCUS_LOST:
        return "unfocused";

    case kWake:
        return "wake";

    default:
        return "other";
    }
}

ImGuiKey KeyOf(SDL_Scancode scancode)
{
    switch (scancode)
    {
    case SDL_SCANCODE_LCTRL:
        return ImGuiKey_LeftCtrl;

    case SDL_SCANCODE_LSHIFT:
        return ImGuiKey_LeftShift;

    case SDL_SCANCODE_Z:
        return ImGuiKey_Z;

    default:
        return ImGuiKey_None;
    }
}

// Gives a key event to Dear ImGui as its back end for SDL3 does: the modifiers of the event, then the key.
void ToImGui(const SDL_Event& event)
{
    if (event.type != SDL_EVENT_KEY_DOWN && event.type != SDL_EVENT_KEY_UP)
    {
        return;
    }

    ImGuiIO& io = ImGui::GetIO();
    io.AddKeyEvent(ImGuiMod_Ctrl, (event.key.mod & SDL_KMOD_CTRL) != 0);
    io.AddKeyEvent(ImGuiMod_Shift, (event.key.mod & SDL_KMOD_SHIFT) != 0);
    io.AddKeyEvent(KeyOf(event.key.scancode), event.key.down);
}

// A key event after a key press waits for the next frame, and every input event after it waits behind it. Each frame
// passes the waiting events up to the next key event after a press. An event that isn't input passes at once. A text
// event keeps its text while it waits.
void TestOrder()
{
    app::InputQueue queue;
    std::vector<std::string> passed;
    const auto pass = [&passed](const SDL_Event& event)
    {
        passed.push_back(Shown(event));
    };
    const auto frame = [&]
    {
        passed.clear();
        queue.StartFrame(pass);
        return passed;
    };

    // The pointer and a first press pass at once, with the text of the press.
    queue.Add(Of(SDL_EVENT_MOUSE_MOTION), pass);
    queue.Add(Key(SDL_SCANCODE_A, true, SDL_KMOD_NONE), pass);
    queue.Add(Text("a"), pass);
    XSFLIB_CHECK((passed == std::vector<std::string>{"move", "+A", "\"a\""}));
    XSFLIB_CHECK(queue.Empty());

    // The next key event waits, and so does the input after it. The wake isn't input.
    std::string typed = "b";
    queue.Add(Key(SDL_SCANCODE_RETURN, true, SDL_KMOD_NONE), pass);
    queue.Add(Key(SDL_SCANCODE_B, true, SDL_KMOD_NONE), pass);
    queue.Add(Text(typed.c_str()), pass);
    typed = "x";
    queue.Add(Of(SDL_EVENT_MOUSE_MOTION), pass);
    queue.Add(Of(SDL_EVENT_MOUSE_BUTTON_DOWN), pass);
    queue.Add(Of(kWake), pass);
    queue.Add(Key(SDL_SCANCODE_A, false, SDL_KMOD_NONE), pass);
    XSFLIB_CHECK((passed == std::vector<std::string>{"move", "+A", "\"a\"", "wake"}));
    XSFLIB_CHECK(!queue.Empty());

    // One press a frame, each with the input that followed it.
    XSFLIB_CHECK((frame() == std::vector<std::string>{"+Return"}));
    XSFLIB_CHECK((frame() == std::vector<std::string>{"+B", "\"b\"", "move", "button"}));
    XSFLIB_CHECK(!queue.Empty());
    XSFLIB_CHECK((frame() == std::vector<std::string>{"-A"}));
    XSFLIB_CHECK(queue.Empty());
    XSFLIB_CHECK(frame().empty());

    // Releases pass together, and a held key's repeats aren't presses.
    passed.clear();
    queue.Add(Key(SDL_SCANCODE_B, false, SDL_KMOD_NONE), pass);
    queue.Add(Key(SDL_SCANCODE_RETURN, false, SDL_KMOD_NONE), pass);
    queue.Add(Key(SDL_SCANCODE_Z, true, SDL_KMOD_NONE, true), pass);
    queue.Add(Key(SDL_SCANCODE_Z, true, SDL_KMOD_NONE, true), pass);
    queue.Add(Key(SDL_SCANCODE_Z, false, SDL_KMOD_NONE), pass);
    XSFLIB_CHECK((passed == std::vector<std::string>{"-B", "-Return", "+Z", "+Z", "-Z"}));
    XSFLIB_CHECK(queue.Empty());
}

// The waiting events of the types that a message box leaves out drop, and the others keep their order.
void TestDrop()
{
    app::InputQueue queue;
    std::vector<std::string> passed;
    const auto pass = [&passed](const SDL_Event& event)
    {
        passed.push_back(Shown(event));
    };
    queue.Add(Key(SDL_SCANCODE_RETURN, true, SDL_KMOD_NONE), pass);
    queue.Add(Key(SDL_SCANCODE_A, true, SDL_KMOD_NONE), pass);
    queue.Add(Text("a"), pass);
    queue.Add(Key(SDL_SCANCODE_RETURN, false, SDL_KMOD_NONE), pass);
    queue.Add(Of(SDL_EVENT_WINDOW_FOCUS_LOST), pass);
    queue.Add(Key(SDL_SCANCODE_A, false, SDL_KMOD_NONE), pass);
    queue.Drop([](uint32_t type) { return type == SDL_EVENT_KEY_DOWN || type == SDL_EVENT_TEXT_INPUT; });

    passed.clear();
    queue.StartFrame(pass);
    XSFLIB_CHECK((passed == std::vector<std::string>{"-Return", "unfocused", "-A"}));
    XSFLIB_CHECK(queue.Empty());
}

// How often the shortcut `chord` counts while `events` arrive in the batches of `batches`, one batch before each frame.
// With `queued`, the events go through an InputQueue as the program's do. Without, each goes to Dear ImGui at once.
int ShortcutCount(ImGuiKeyChord chord, const std::vector<std::vector<SDL_Event>>& batches, bool queued)
{
    hokuto::testing::CreateHeadlessContext();
    // On macOS, Dear ImGui swaps Ctrl and Cmd in its key events, and ImGuiMod_Ctrl stands for Cmd. The events here press
    // Ctrl on every system.
    ImGui::GetIO().ConfigMacOSXBehaviors = false;
    app::InputQueue queue;
    int count = 0;
    const auto frame = [&]
    {
        ImGui::NewFrame();
        queue.StartFrame(ToImGui);
        if (ImGui::Shortcut(chord, ImGuiInputFlags_RouteGlobal))
        {
            count++;
        }
        hokuto::testing::EndHeadlessFrame();
    };
    frame();
    for (const std::vector<SDL_Event>& batch : batches)
    {
        for (const SDL_Event& event : batch)
        {
            if (queued)
            {
                queue.Add(event, ToImGui);
            }
            else
            {
                ToImGui(event);
            }
        }
        frame();
    }
    for (int i = 0; i < 8; i++)
    {
        frame();
    }
    const bool drained = queue.Empty();
    ImGui::DestroyContext();
    XSFLIB_CHECK(drained);

    return count;
}

// Ctrl+Z counts once however its four events fall between the frames, also when Ctrl's release follows Z's press
// before the next frame. Without the queue, the shortcut is lost then.
void TestShortcut()
{
    const SDL_Event ctrl_down = Key(SDL_SCANCODE_LCTRL, true, SDL_KMOD_LCTRL);
    const SDL_Event z_down = Key(SDL_SCANCODE_Z, true, SDL_KMOD_LCTRL);
    const SDL_Event ctrl_up = Key(SDL_SCANCODE_LCTRL, false, SDL_KMOD_NONE);
    const SDL_Event z_up = Key(SDL_SCANCODE_Z, false, SDL_KMOD_NONE);
    const ImGuiKeyChord undo = ImGuiMod_Ctrl | ImGuiKey_Z;

    // The modifier's release in the batch of the key's press.
    const std::vector<std::vector<SDL_Event>> released_early = {{ctrl_down}, {z_down, ctrl_up}, {z_up}};
    XSFLIB_CHECK(ShortcutCount(undo, released_early, false) == 0);
    XSFLIB_CHECK(ShortcutCount(undo, released_early, true) == 1);

    // Each event alone before a frame, all four in one batch, and the press with the modifier's.
    XSFLIB_CHECK(ShortcutCount(undo, {{ctrl_down}, {z_down}, {ctrl_up}, {z_up}}, true) == 1);
    XSFLIB_CHECK(ShortcutCount(undo, {{ctrl_down, z_down, ctrl_up, z_up}}, true) == 1);
    XSFLIB_CHECK(ShortcutCount(undo, {{ctrl_down, z_down}, {ctrl_up, z_up}}, true) == 1);
    XSFLIB_CHECK(ShortcutCount(undo, {{ctrl_down, z_down}, {z_up, ctrl_up}}, true) == 1);

    // Z before Ctrl is no shortcut.
    XSFLIB_CHECK(ShortcutCount(undo, {{Key(SDL_SCANCODE_Z, true, SDL_KMOD_NONE), ctrl_down, z_up, ctrl_up}}, true) ==
                 0);

    // Redo with two modifiers, each released in the batch of Z's press.
    const SDL_Keymod both = SDL_KMOD_LCTRL | SDL_KMOD_LSHIFT;
    const std::vector<std::vector<SDL_Event>> redo_events = {
        {ctrl_down, Key(SDL_SCANCODE_LSHIFT, true, both)},
        {Key(SDL_SCANCODE_Z, true, both), Key(SDL_SCANCODE_LSHIFT, false, SDL_KMOD_LCTRL), ctrl_up},
        {z_up}};
    const ImGuiKeyChord redo = ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z;
    XSFLIB_CHECK(ShortcutCount(redo, redo_events, false) == 0);
    XSFLIB_CHECK(ShortcutCount(redo, redo_events, true) == 1);
    XSFLIB_CHECK(ShortcutCount(undo, redo_events, true) == 0);
}

// Dear ImGui clears the pointer's place when the window loses the keyboard. A move to the place that it had, given
// before the frame that clears it, is dropped as a repeat, and a click on the button under the resting pointer does
// nothing. ShowPointerPlace gives the place again after that frame. It also gives a place that differs from the last
// move that the window passed on, and nothing while Dear ImGui has the place.
void TestPointerPlace()
{
    hokuto::testing::CreateHeadlessContext();
    ImGuiIO& io = ImGui::GetIO();
    int clicks = 0;
    const auto frame = [&]
    {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
        ImGui::SetNextWindowSize(ImVec2(400.0f, 300.0f));
        ImGui::Begin("Window");
        ImGui::SetCursorScreenPos(ImVec2(80.0f, 30.0f));
        if (ImGui::Button("Field", ImVec2(60.0f, 40.0f)))
        {
            clicks++;
        }
        ImGui::End();
        hokuto::testing::EndHeadlessFrame();
    };
    const auto click = [&]
    {
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
        frame();
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        frame();
        frame();
    };
    std::optional<ImVec2> seen;
    const ImVec2 place(100.0f, 50.0f);
    frame();

    // The first place, then nothing to give.
    const bool first = app::ShowPointerPlace(place, seen);
    frame();
    const bool again = app::ShowPointerPlace(place, seen);
    click();
    const int before = clicks;

    // The keyboard goes to a box and comes back a frame later. The pointer rests.
    io.AddFocusEvent(false);
    io.AddMousePosEvent(place.x, place.y);
    frame();
    const bool cleared = !ImGui::IsMousePosValid();
    io.AddFocusEvent(true);
    frame();
    click();
    const int ignored = clicks;
    const bool given = app::ShowPointerPlace(place, seen);
    frame();
    const bool back = ImGui::IsMousePosValid() && io.MousePos.x == place.x && io.MousePos.y == place.y;
    click();
    const int after = clicks;

    // The pointer moved while the window dropped its moves: off the button.
    const ImVec2 moved(300.0f, 200.0f);
    const bool given_moved = app::ShowPointerPlace(moved, seen);
    frame();
    const bool at_moved = io.MousePos.x == moved.x && io.MousePos.y == moved.y;
    click();

    XSFLIB_CHECK(first && !again);
    XSFLIB_CHECK(before == 1);
    XSFLIB_CHECK(cleared);
    XSFLIB_CHECK(ignored == 1);
    XSFLIB_CHECK(given && back);
    XSFLIB_CHECK(after == 2);
    XSFLIB_CHECK(given_moved && at_moved);
    XSFLIB_CHECK(clicks == 2);
    XSFLIB_CHECK(seen && seen->x == moved.x && seen->y == moved.y);
    ImGui::DestroyContext();
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("input_queue",
                                    []
                                    {
                                        TestOrder();
                                        TestDrop();
                                        TestShortcut();
                                        TestPointerPlace();
                                    });
}
