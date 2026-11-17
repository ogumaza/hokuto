// SPDX-License-Identifier: MIT

// The message boxes through zenity: the arguments of a box, the answer, and the events that the window leaves out while
// a box is open.

#include "app/zenity_box.h"

#include <cstdint>
#include <string>
#include <vector>

#include "xsflib/testing/check.h"

namespace hokuto
{
namespace
{

using app::LeftOutDuringBox;
using app::ReadZenityAnswer;
using app::ZenityArguments;

// The question about a document's changes: a warning with three buttons.
constexpr SDL_MessageBoxButtonData kSaveButtons[] = {{0, 0, "Don't Save"},
                                                     {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 1, "Cancel"},
                                                     {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 2, "Save"}};

SDL_MessageBoxData SaveQuestion()
{
    SDL_MessageBoxData box{};
    box.flags = SDL_MESSAGEBOX_WARNING | SDL_MESSAGEBOX_BUTTONS_RIGHT_TO_LEFT;
    box.title = "Hokuto";
    box.message = "Do you want to save the changes?";
    box.numbuttons = 3;
    box.buttons = kSaveButtons;
    return box;
}

// A box's arguments are those that SDL3 passes to zenity: a question without its two buttons and without markup, the
// icon of the box's kind, the title, the text, and the box's buttons in their order.
void TestArguments()
{
    const std::vector<std::string> expected = {"zenity",
                                               "--question",
                                               "--switch",
                                               "--no-wrap",
                                               "--no-markup",
                                               "--icon",
                                               "dialog-warning",
                                               "--title",
                                               "Hokuto",
                                               "--text",
                                               "Do you want to save the changes?",
                                               "--extra-button",
                                               "Don't Save",
                                               "--extra-button",
                                               "Cancel",
                                               "--extra-button",
                                               "Save"};
    XSFLIB_CHECK(ZenityArguments(SaveQuestion(), "4.2.1\n") == expected);
}

// zenity 3.90 renamed the option of the icon. An error and a note take their icons. A box without a title, a text or a
// button text passes the empty value in one argument, and a box without buttons gets OK.
void TestOtherArguments()
{
    SDL_MessageBoxData box{};
    box.flags = SDL_MESSAGEBOX_ERROR;
    const std::vector<std::string> old = {"zenity",      "--question",   "--switch", "--no-wrap", "--no-markup",
                                          "--icon-name", "dialog-error", "--title=", "--text=",   "--extra-button=OK"};
    XSFLIB_CHECK(ZenityArguments(box, "3.44.0\n") == old);
    XSFLIB_CHECK(ZenityArguments(box, "3.32").at(5) == "--icon-name");
    XSFLIB_CHECK(ZenityArguments(box, "3").at(5) == "--icon-name");
    XSFLIB_CHECK(ZenityArguments(box, "3.90.0\n").at(5) == "--icon");
    XSFLIB_CHECK(ZenityArguments(box, "3.92.0\n").at(5) == "--icon");
    XSFLIB_CHECK(ZenityArguments(box, "5.0\n").at(5) == "--icon");
    XSFLIB_CHECK(ZenityArguments(box, "").at(5) == "--icon");

    box.flags = SDL_MESSAGEBOX_INFORMATION;
    XSFLIB_CHECK(ZenityArguments(box, "4.2.1\n").at(6) == "dialog-information");

    const SDL_MessageBoxButtonData unnamed[] = {{0, 0, ""}, {0, 1, nullptr}};
    box.numbuttons = 2;
    box.buttons = unnamed;
    const std::vector<std::string> arguments = ZenityArguments(box, "4.2.1\n");
    XSFLIB_CHECK(arguments.size() == 11);
    if (arguments.size() == 11)
    {
        XSFLIB_CHECK(arguments[9] == "--extra-button=" && arguments[10] == "--extra-button=");
    }
}

// zenity writes the chosen button's text and a new line, and the box's answer is that button's number. A box closed
// without a button keeps the caller's value. An exit code of 255 and a signal say that zenity showed no box.
void TestAnswer()
{
    const SDL_MessageBoxData box = SaveQuestion();
    int chosen = 1;
    XSFLIB_CHECK(ReadZenityAnswer(box, 1, "Save\n", chosen) && chosen == 2);
    XSFLIB_CHECK(ReadZenityAnswer(box, 1, "Don't Save\n", chosen) && chosen == 0);
    XSFLIB_CHECK(ReadZenityAnswer(box, 1, "Cancel", chosen) && chosen == 1);

    chosen = 7;
    XSFLIB_CHECK(ReadZenityAnswer(box, 1, "", chosen) && chosen == 7);
    XSFLIB_CHECK(ReadZenityAnswer(box, 1, "Sav\n", chosen) && chosen == 7);
    XSFLIB_CHECK(ReadZenityAnswer(box, 0, "\n", chosen) && chosen == 7);
    XSFLIB_CHECK(!ReadZenityAnswer(box, 255, "Save\n", chosen) && chosen == 7);
    XSFLIB_CHECK(!ReadZenityAnswer(box, -15, "", chosen) && chosen == 7);
    XSFLIB_CHECK(!ReadZenityAnswer(box, -1, "", chosen) && chosen == 7);
}

// While a box is open, the window drops what the user does to it. The releases of keys and buttons stay, so that no
// key or button that went down before the box stays down. The window's changes, a quit and the program's events stay.
void TestEvents()
{
    const uint32_t left_out[] = {
        SDL_EVENT_KEY_DOWN,    SDL_EVENT_TEXT_INPUT,   SDL_EVENT_TEXT_EDITING,  SDL_EVENT_MOUSE_BUTTON_DOWN,
        SDL_EVENT_MOUSE_WHEEL, SDL_EVENT_MOUSE_MOTION, SDL_EVENT_FINGER_DOWN,   SDL_EVENT_PEN_DOWN,
        SDL_EVENT_DROP_BEGIN,  SDL_EVENT_DROP_FILE,    SDL_EVENT_DROP_COMPLETE, SDL_EVENT_WINDOW_CLOSE_REQUESTED};
    for (const uint32_t type : left_out)
    {
        XSFLIB_CHECK(LeftOutDuringBox(type));
    }
    const uint32_t kept[] = {SDL_EVENT_KEY_UP,
                             SDL_EVENT_MOUSE_BUTTON_UP,
                             SDL_EVENT_QUIT,
                             SDL_EVENT_WINDOW_RESIZED,
                             SDL_EVENT_WINDOW_EXPOSED,
                             SDL_EVENT_WINDOW_FOCUS_GAINED,
                             SDL_EVENT_WINDOW_FOCUS_LOST,
                             SDL_EVENT_WINDOW_MOUSE_ENTER,
                             SDL_EVENT_WINDOW_MOUSE_LEAVE,
                             SDL_EVENT_WINDOW_RESTORED,
                             SDL_EVENT_AUDIO_DEVICE_ADDED,
                             SDL_EVENT_AUDIO_DEVICE_REMOVED,
                             SDL_EVENT_USER,
                             SDL_EVENT_USER + 1};
    for (const uint32_t type : kept)
    {
        XSFLIB_CHECK(!LeftOutDuringBox(type));
    }
}

} // namespace
} // namespace hokuto

int main()
{
    return xsflib::testing::RunTest("zenity_box",
                                    []
                                    {
                                        hokuto::TestArguments();
                                        hokuto::TestOtherArguments();
                                        hokuto::TestAnswer();
                                        hokuto::TestEvents();
                                    });
}
