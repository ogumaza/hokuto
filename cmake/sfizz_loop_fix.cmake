# SPDX-License-Identifier: MIT

# Takes into sfizz 1.2.3's sources a fix that sfizz made after 1.2.3: its commit 4055582671 of 2024-02-23, "Fixes an
# issue that caused a crash when using a loop". In each output sample, sfizz 1.2.3 moves a looped note's position back
# by one loop at most. A note that moves through its loop by more than the loop's length in one sample then reads ever
# further past the loop. The fix moves the position back as often as it takes. CMake runs the script in the sources'
# folder after it downloads them. Sources that hold the fix stay as they are.
set(voice src/sfizz/Voice.cpp)
set(wrap "if (wrappedIndex > loop.end) {")
set(fixed_wrap "while (wrappedIndex > loop.end) {")
file(READ ${voice} text)
string(FIND "${text}" "${wrap}" at)
if (at EQUAL -1)
    string(FIND "${text}" "${fixed_wrap}" fixed_at)
    if (fixed_at EQUAL -1)
        message(FATAL_ERROR "${voice} holds neither sfizz 1.2.3's wrap of a loop nor its fix.")
    endif()
    return()
endif()
string(REPLACE "${wrap}" "${fixed_wrap}" text "${text}")
file(WRITE ${voice} "${text}")
