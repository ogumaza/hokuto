// SPDX-License-Identifier: MIT

// The messages for the user that wait for a native box.

#pragma once

#include <string>
#include <utility>
#include <vector>

namespace hokuto::app
{

// A message box's kind: an error shows the error icon, and a note, such as why nothing plays, the information icon.
enum class MessageKind
{
    kError,
    kNote,
};

struct Message
{
    std::string text;
    MessageKind kind = MessageKind::kError;
};

// The messages that wait for a box, in the order that they came. A box opens only in a frame in which no mouse button
// is down. A box that opens while a button is down keeps the release from the window. SDL then still counts the button
// as down and drops the next press.
class MessageQueue
{
public:
    // Adds a message, unless one of the same kind and text already waits. A caller that asks in each frame of a held
    // button then adds only one message. Returns whether the message was added.
    bool Add(std::string text, MessageKind kind)
    {
        for (const Message& waiting : waiting_)
        {
            if (waiting.kind == kind && waiting.text == text)
            {
                return false;
            }
        }

        waiting_.push_back({std::move(text), kind});
        return true;
    }

    // The messages to show now, in the order that they came: every waiting one while no mouse button is down, and none
    // while one is.
    std::vector<Message> Ready(bool button_down)
    {
        return button_down ? std::vector<Message>() : std::exchange(waiting_, {});
    }

private:
    std::vector<Message> waiting_;
};

} // namespace hokuto::app
