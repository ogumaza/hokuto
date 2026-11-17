// SPDX-License-Identifier: MIT

// Tests the helpers for the paths that the file dialogs give (src/app/file_dialogs.h), without a window.

#include "app/file_dialogs.h"

#include "xsflib/testing/check.h"

using hokuto::app::NamesFile;
using hokuto::app::WithExtension;

namespace
{

// A list names a file when its first path isn't empty. An error gives no list, and a cancel an empty list. SDL3's
// zenity dialog gives a list of one empty path on a cancel.
void TestNamesFile()
{
    const char* const kChosen[] = {"/songs/theme.hok", nullptr};
    const char* const kEmpty[] = {nullptr};
    const char* const kZenityCancel[] = {"", nullptr};

    XSFLIB_CHECK(NamesFile(kChosen));
    XSFLIB_CHECK(!NamesFile(nullptr));
    XSFLIB_CHECK(!NamesFile(kEmpty));
    XSFLIB_CHECK(!NamesFile(kZenityCancel));
}

// A path gains the extension unless its name ends in it or in its long form, in capitals too.
void TestWithExtension()
{
    XSFLIB_CHECK(WithExtension("/songs/theme", ".hok") == "/songs/theme.hok");
    XSFLIB_CHECK(WithExtension("/songs/theme.hok", ".hok") == "/songs/theme.hok");
    XSFLIB_CHECK(WithExtension("/songs/theme.HOK", ".hok") == "/songs/theme.HOK");
    XSFLIB_CHECK(WithExtension("/songs/theme.mid", ".hok") == "/songs/theme.mid.hok");
    XSFLIB_CHECK(WithExtension("/samples/p14.aiff", ".aif") == "/samples/p14.aiff");
    XSFLIB_CHECK(WithExtension("/samples/p14.AIFF", ".aif") == "/samples/p14.AIFF");
    XSFLIB_CHECK(WithExtension("/samples/p14.aif", ".aif") == "/samples/p14.aif");
    XSFLIB_CHECK(WithExtension("/songs/theme.midi", ".mid") == "/songs/theme.midi");
    XSFLIB_CHECK(WithExtension("/songs/theme.aiff", ".mid") == "/songs/theme.aiff.mid");
    XSFLIB_CHECK(WithExtension("/songs/theme.aif", ".aiff") == "/songs/theme.aif.aiff");
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("file_dialogs",
                                    []
                                    {
                                        TestNamesFile();
                                        TestWithExtension();
                                    });
}
