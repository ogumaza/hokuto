// SPDX-License-Identifier: MIT

// The order and the frames in which Dear ImGui receives the window's input events, and the pointer's place after the
// window dropped its moves. Nothing here opens a window, and the tests run without a display.

#pragma once

#include <SDL3/SDL.h>
#include <imgui.h>

#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <utility>

namespace hokuto::app
{

// Whether Dear ImGui's back end for SDL3 makes input of an event of type `type`: a key, typed text, the pointer's
// moves, its buttons and the wheel, and the window's gain or loss of the keyboard or the pointer. Dear ImGui applies
// these in the order in which it receives them.
constexpr bool IsInput(uint32_t type)
{
    switch (type)
    {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
    case SDL_EVENT_TEXT_INPUT:
    case SDL_EVENT_MOUSE_MOTION:
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
    case SDL_EVENT_MOUSE_WHEEL:
    case SDL_EVENT_WINDOW_MOUSE_ENTER:
    case SDL_EVENT_WINDOW_MOUSE_LEAVE:
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        return true;

    default:
        return false;
    }
}

// The input events on their way to Dear ImGui. Dear ImGui applies the events that it received at the start of a frame.
// Of two changes of one key, the second waits for the next frame. A change of another key doesn't wait. A shortcut
// such as Ctrl+Z counts in the frame of Z's press, and only while Ctrl is down in that frame. When Z's press and Ctrl's
// release arrive between two frames, Dear ImGui applies both in one frame: Z goes down without Ctrl, and the shortcut
// is lost. A frame longer than the time between the two events is enough. Software rendering and a busy machine draw
// such frames, and some input tools release the modifier 30 ms after the key's press.
//
// At most one key press therefore reaches Dear ImGui between two frames. A key event after that press waits here for
// the next frame. So does every input event after a waiting one: typed text, the pointer's moves and its buttons keep
// their order with the keys. A held key's repeats aren't presses: Dear ImGui repeats a held key by itself. An event
// that isn't input never waits.
class InputQueue
{
public:
    // Passes `event` to `pass` now, or keeps it for a later frame. `pass` is called with a const SDL_Event&.
    template <typename Pass>
    void Add(const SDL_Event& event, const Pass& pass)
    {
        if (!IsInput(event.type) || (waiting_.empty() && !Waits(event)))
        {
            Give(event, pass);
            return;
        }

        // The text of a text event is SDL3's temporary memory. A copy stays while the event waits.
        Waiting waiting{event, {}};
        if (event.type == SDL_EVENT_TEXT_INPUT)
        {
            waiting.text = event.text.text != nullptr ? event.text.text : "";
        }
        waiting_.push_back(std::move(waiting));
    }

    // Starts a frame, after ImGui::NewFrame has applied the events of the last one: passes the waiting events to
    // `pass` in their order, up to the first key event after a key press among them.
    template <typename Pass>
    void StartFrame(const Pass& pass)
    {
        pressed_ = false;
        while (!waiting_.empty() && !Waits(waiting_.front().event))
        {
            Waiting waiting = std::move(waiting_.front());
            waiting_.pop_front();
            if (waiting.event.type == SDL_EVENT_TEXT_INPUT)
            {
                waiting.event.text.text = waiting.text.c_str();
            }
            Give(waiting.event, pass);
        }
    }

    // Drops the waiting events of the types for which `leaves_out` returns true, such as the input that the window
    // leaves out while a message box is open (LeftOutDuringBox).
    template <typename LeavesOut>
    void Drop(const LeavesOut& leaves_out)
    {
        std::erase_if(waiting_, [&leaves_out](const Waiting& waiting) { return leaves_out(waiting.event.type); });
    }

    // Whether no event waits.
    bool Empty() const
    {
        return waiting_.empty();
    }

private:
    // An event that waits, with the text of a text event.
    struct Waiting
    {
        SDL_Event event;
        std::string text;
    };

    static bool IsKey(const SDL_Event& event)
    {
        return event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_KEY_UP;
    }

    // Whether `event` waits for the next frame by itself: a key event after a key press that went to Dear ImGui since
    // the last frame started.
    bool Waits(const SDL_Event& event) const
    {
        return pressed_ && IsKey(event);
    }

    template <typename Pass>
    void Give(const SDL_Event& event, const Pass& pass)
    {
        pressed_ = pressed_ || (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat);
        pass(event);
    }

    std::deque<Waiting> waiting_;
    bool pressed_ = false;
};

// Gives Dear ImGui the pointer's place, `place`, where Dear ImGui has none or another one. Returns whether it did.
// `seen` is the place of the last move that the window passed on, and receives `place`. Dear ImGui receives the
// pointer's place only with the pointer's moves, and two things make its place wrong while the pointer rests over the
// window. Dear ImGui clears the place when the window loses the keyboard, as it does to a message box of zenity or to
// another window. And on systems other than macOS and Windows, the window drops the pointer's moves while a box or a
// file dialog is open. Until the next move, a click then does nothing, or lands at the old place. Call it after
// ImGui::NewFrame, while no input waits for a later frame and the pointer is over the window.
inline bool ShowPointerPlace(ImVec2 place, std::optional<ImVec2>& seen)
{
    const bool known = ImGui::IsMousePosValid() && seen && seen->x == place.x && seen->y == place.y;
    seen = place;
    if (known)
    {
        return false;
    }

    ImGui::GetIO().AddMousePosEvent(place.x, place.y);
    return true;
}

} // namespace hokuto::app
