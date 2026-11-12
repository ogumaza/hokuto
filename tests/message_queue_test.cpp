// SPDX-License-Identifier: MIT

// The messages that wait for a frame with no mouse button down.

#include "app/message_queue.h"

#include <vector>

#include "xsflib/testing/check.h"

namespace hokuto
{
namespace
{

using app::Message;
using app::MessageKind;
using app::MessageQueue;

// A message asked for while a button is down waits until no button is down. Two messages keep their order and their
// kinds, and each shows once.
void TestWaitForRelease()
{
    MessageQueue queue;
    queue.Add("Can't add a note there.", MessageKind::kError);
    XSFLIB_CHECK(queue.Ready(true).empty());
    queue.Add("Nothing plays.", MessageKind::kNote);
    XSFLIB_CHECK(queue.Ready(true).empty());

    const std::vector<Message> shown = queue.Ready(false);
    XSFLIB_CHECK(shown.size() == 2);
    if (shown.size() == 2)
    {
        XSFLIB_CHECK(shown[0].text == "Can't add a note there." && shown[0].kind == MessageKind::kError);
        XSFLIB_CHECK(shown[1].text == "Nothing plays." && shown[1].kind == MessageKind::kNote);
    }
    XSFLIB_CHECK(queue.Ready(false).empty());
}

// A message that already waits isn't added again: a caller that asks in each frame of a held button adds only one
// message. The same text of another kind, or one asked for after the box showed, is a message of its own.
void TestNoCopies()
{
    MessageQueue queue;
    XSFLIB_CHECK(queue.Add("Can't move this note there.", MessageKind::kError));
    XSFLIB_CHECK(!queue.Add("Can't move this note there.", MessageKind::kError));
    XSFLIB_CHECK(queue.Add("Can't move this note there.", MessageKind::kNote));
    XSFLIB_CHECK(queue.Ready(true).empty());
    XSFLIB_CHECK(!queue.Add("Can't move this note there.", MessageKind::kError));
    XSFLIB_CHECK(queue.Ready(false).size() == 2);
    XSFLIB_CHECK(queue.Add("Can't move this note there.", MessageKind::kError));
}

} // namespace
} // namespace hokuto

int main()
{
    return xsflib::testing::RunTest("message_queue",
                                    []
                                    {
                                        hokuto::TestWaitForRelease();
                                        hokuto::TestNoCopies();
                                    });
}
