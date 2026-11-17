// SPDX-License-Identifier: MIT

#include "app/zenity_box.h"

#include <charconv>

namespace hokuto::app
{

namespace
{

// Whether the zenity of `version` has the option `--icon`. zenity 3.90 renamed `--icon-name` to `--icon`. A version
// that doesn't parse as numbers is treated as 3.90 or later.
bool TakesIconOption(std::string_view version)
{
    int major = 0;
    int minor = 0;
    const char* const end = version.data() + version.size();
    const std::from_chars_result first = std::from_chars(version.data(), end, major);
    if (first.ec != std::errc())
    {
        return true;
    }

    if (first.ptr != end && *first.ptr == '.')
    {
        std::from_chars(first.ptr + 1, end, minor);
    }

    return major > 3 || (major == 3 && minor >= 90);
}

// The name of the icon that zenity shows for the flags of a box.
const char* IconOf(SDL_MessageBoxFlags flags)
{
    if ((flags & SDL_MESSAGEBOX_ERROR) != 0)
    {
        return "dialog-error";
    }

    return (flags & SDL_MESSAGEBOX_WARNING) != 0 ? "dialog-warning" : "dialog-information";
}

// Adds an option with its value to `arguments`. zenity accepts an empty value only in the form `--option=`.
void AddOption(std::vector<std::string>& arguments, const char* option, const char* value)
{
    if (value != nullptr && value[0] != '\0')
    {
        arguments.emplace_back(option);
        arguments.emplace_back(value);
    }
    else
    {
        arguments.push_back(std::string(option) + "=");
    }
}

} // namespace

std::vector<std::string> ZenityArguments(const SDL_MessageBoxData& box, std::string_view version)
{
    // `--switch` removes the two buttons of a question. Each `--extra-button` adds one of the box's buttons.
    std::vector<std::string> arguments = {"zenity", "--question", "--switch", "--no-wrap", "--no-markup"};
    arguments.emplace_back(TakesIconOption(version) ? "--icon" : "--icon-name");
    arguments.emplace_back(IconOf(box.flags));
    AddOption(arguments, "--title", box.title);
    AddOption(arguments, "--text", box.message);
    for (int i = 0; i < box.numbuttons; i++)
    {
        AddOption(arguments, "--extra-button", box.buttons[i].text);
    }
    if (box.numbuttons <= 0)
    {
        arguments.emplace_back("--extra-button=OK");
    }

    return arguments;
}

bool ReadZenityAnswer(const SDL_MessageBoxData& box, int exit_code, std::string_view output, int& chosen)
{
    if (exit_code < 0 || exit_code == 255)
    {
        return false;
    }

    // zenity ends the button's text with a new line.
    const std::string_view text = output.substr(0, output.rfind('\n'));
    for (int i = 0; i < box.numbuttons; i++)
    {
        if (box.buttons[i].text != nullptr && text == box.buttons[i].text)
        {
            chosen = box.buttons[i].buttonID;
            break;
        }
    }

    return true;
}

bool LeftOutDuringBox(uint32_t type)
{
    switch (type)
    {
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_TEXT_EDITING:
    case SDL_EVENT_TEXT_INPUT:
    case SDL_EVENT_TEXT_EDITING_CANDIDATES:
    case SDL_EVENT_MOUSE_MOTION:
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_WHEEL:
    case SDL_EVENT_FINGER_DOWN:
    case SDL_EVENT_FINGER_UP:
    case SDL_EVENT_FINGER_MOTION:
    case SDL_EVENT_FINGER_CANCELED:
    case SDL_EVENT_PINCH_BEGIN:
    case SDL_EVENT_PINCH_UPDATE:
    case SDL_EVENT_PINCH_END:
    case SDL_EVENT_DROP_FILE:
    case SDL_EVENT_DROP_TEXT:
    case SDL_EVENT_DROP_BEGIN:
    case SDL_EVENT_DROP_COMPLETE:
    case SDL_EVENT_DROP_POSITION:
    case SDL_EVENT_PEN_PROXIMITY_IN:
    case SDL_EVENT_PEN_PROXIMITY_OUT:
    case SDL_EVENT_PEN_DOWN:
    case SDL_EVENT_PEN_UP:
    case SDL_EVENT_PEN_BUTTON_DOWN:
    case SDL_EVENT_PEN_BUTTON_UP:
    case SDL_EVENT_PEN_MOTION:
    case SDL_EVENT_PEN_AXIS:
        return true;

    default:
        return false;
    }
}

} // namespace hokuto::app
