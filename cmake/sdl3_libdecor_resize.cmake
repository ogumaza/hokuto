# SPDX-License-Identifier: MIT

# Keeps a window with libdecor's title bar smooth while the user resizes it on Wayland. GNOME draws no title bars for
# Wayland programs. SDL3 then draws the title bar and the shadow with libdecor.
#
# During a resize by the pointer or the keyboard, the compositor sends the window a configure event with each new size.
# It doesn't wait for the program. SDL3 commits libdecor's frame at the new size for every such event. libdecor's GTK
# plugin then draws the title bar and the shadow again on the processor, and the shadow's buffer covers the whole
# window. One such drawing took 8 ms for a window of 1400 by 800 pixels and 16 ms for Hokuto's window near the size of a
# display of 2560 by 1440. Besides a read in libdecor's dispatch, a call of SDL3's event pump reads the compositor's
# socket once. All the events that the reads found are handled before the call returns, and the program draws its
# frames between the calls. When the events arrive faster than the plugin draws, more of them pile up before each read
# than before the last, and each pump runs longer than the one before. A test sent Hokuto the events of a resize of 2 s.
# They came at the display's rate of 240 a second. The window showed no new size for 7.8 s. SDL3 defers the
# events of a resize only with libdecor 0.3 or later, and Ubuntu 26.04 has 0.2.5. The Vulkan driver makes no
# difference. The window froze the same way with NVIDIA's driver and with a software driver from Mesa.
#
# With the change, a configure event that changes only the size of a shown window is acknowledged at once, in a commit
# of libdecor's frame at the size that the window has. The plugin draws nothing for such a commit. The new size waits.
# At the end of the event pump, Wayland_CommitDeferredResizes commits the newest size of each such window in one commit
# of libdecor's frame. The program draws a frame after each pump, and each frame shows one size at most. In the same
# test, Hokuto showed each size 30 ms after its event on average and 72 ms at most. As before, the compositor receives
# the acknowledgement and the size in the same commit of the window's surface. A configure event that changes the
# window's state is handled as before, and so is one with the size that the window has already.
#
# CMake runs the script in the sources' folder after it downloads them. Sources with the current revision of the change
# stay as they are. The change was written for SDL3 3.4.20, and the script refuses the sources of any other version. It
# also refuses sources with an older revision of the change, because the script never changes the lines of an older
# revision. A change to the lines that the script writes raises the revision in the mark. CMakeLists.txt then checks
# for the mark of the new revision. Only the Wayland driver changes. The builds for Windows and macOS compile none of
# these files.
set(sdl3_version 3.4.20)
set(mark_prefix "Hokuto: cmake/sdl3_libdecor_resize.cmake")
set(mark "${mark_prefix}, revision 2")
set(header src/video/wayland/SDL_waylandwindow.h)
set(window src/video/wayland/SDL_waylandwindow.c)
set(events src/video/wayland/SDL_waylandevents.c)

# CMakeLists.txt passes the version that it downloads as pinned_version. A run by hand passes none.
if (DEFINED pinned_version AND NOT pinned_version STREQUAL sdl3_version)
    message(FATAL_ERROR "CMakeLists.txt pins SDL3 ${pinned_version}, and cmake/sdl3_libdecor_resize.cmake was written "
                        "for SDL3 ${sdl3_version}. Check the change against the Wayland driver of SDL3 "
                        "${pinned_version}, then set sdl3_version in the script to it.")
endif()
file(READ include/SDL3/SDL_version.h version_text)
set(source_version "")
foreach (part MAJOR MINOR MICRO)
    if (version_text MATCHES "#define SDL_${part}_VERSION +([0-9]+)")
        list(APPEND source_version ${CMAKE_MATCH_1})
    endif()
endforeach()
list(JOIN source_version . source_version)
if (NOT source_version STREQUAL sdl3_version)
    message(FATAL_ERROR "The SDL3 sources in ${CMAKE_CURRENT_SOURCE_DIR} are version ${source_version}, and "
                        "cmake/sdl3_libdecor_resize.cmake changes SDL3 ${sdl3_version} only.")
endif()

file(READ ${header} header_text)
file(READ ${window} window_text)
file(READ ${events} events_text)
string(FIND "${events_text}" "${mark}" changed_at)
if (NOT changed_at EQUAL -1)
    return()
endif()
foreach (text header_text window_text events_text)
    string(FIND "${${text}}" "${mark_prefix}" changed_at)
    if (NOT changed_at EQUAL -1)
        message(FATAL_ERROR "The SDL3 sources in ${CMAKE_CURRENT_SOURCE_DIR} have an older revision of the change of "
                            "cmake/sdl3_libdecor_resize.cmake. Download SDL3 again, or run the script on a fresh copy "
                            "of SDL3 ${sdl3_version}'s sources.")
    endif()
endforeach()

# Replaces one text with another in the variable with a file's text. The text to replace has to occur exactly once in
# the file. No file is written until every replacement has succeeded.
function(hokuto_replace_once text_variable before after)
    string(FIND "${${text_variable}}" "${before}" at)
    string(FIND "${${text_variable}}" "${before}" last REVERSE)
    if (at EQUAL -1 OR NOT at EQUAL last)
        message(FATAL_ERROR "The lines that cmake/sdl3_libdecor_resize.cmake changes occur in SDL3's Wayland "
                            "driver twice or not at all: ${before}")
    endif()
    string(REPLACE "${before}" "${after}" changed "${${text_variable}}")
    set(${text_variable} "${changed}" PARENT_SCOPE)
endfunction()

# The window's data: whether a size waits for the end of the pump, and the state of the last configure event.
hokuto_replace_once(header_text
    "    bool pending_config_ack;\n"
    "    bool pending_config_ack;\n    bool libdecor_resize_deferred; // ${mark}\n    int libdecor_window_state;\n")
string(CONCAT sync_window "extern bool Wayland_SyncWindow(SDL_VideoDevice *_this, SDL_Window *window);\n")
hokuto_replace_once(header_text
    "${sync_window}"
    "${sync_window}extern void Wayland_CommitDeferredResizes(SDL_VideoDevice *_this); // ${mark}\n")

# The configure event of libdecor's frame.
string(CONCAT state_read
       "    const bool has_window_state = libdecor_configuration_get_window_state(configuration, &window_state);\n"
       "    if (has_window_state) { // ${mark}\n")
hokuto_replace_once(window_text
    "    if (libdecor_configuration_get_window_state(configuration, &window_state)) {\n"
    "${state_read}")
string(CONCAT frame_commit
       "        struct libdecor_state *state = libdecor_state_new(wind->current.logical_width, wind->current.logical_height);\n"
       "        libdecor_frame_commit(frame, state, configuration);\n"
       "        libdecor_state_free(state);\n")
string(CONCAT deferred_commit
       "        // ${mark}\n"
       "        // A configure event that changes only the size of a shown window is acknowledged at the size that the\n"
       "        // window has. The new size is committed at the end of the event pump (Wayland_CommitDeferredResizes).\n"
       "        const bool size_only = wind->shell_surface_status == WAYLAND_SHELL_SURFACE_STATUS_SHOWN &&\n"
       "                               has_window_state && (int)window_state == wind->libdecor_window_state &&\n"
       "                               (wind->requested.logical_width != wind->current.logical_width ||\n"
       "                                wind->requested.logical_height != wind->current.logical_height);\n"
       "        wind->libdecor_resize_deferred = size_only;\n"
       "        if (has_window_state) {\n"
       "            wind->libdecor_window_state = (int)window_state;\n"
       "        }\n"
       "        if (!size_only) {\n"
       "            ConfigureWindowGeometry(window);\n"
       "        }\n"
       "${frame_commit}")
hokuto_replace_once(window_text "        ConfigureWindowGeometry(window);\n${frame_commit}" "${deferred_commit}")

# The commit of the sizes that wait, after CommitLibdecorFrame.
string(CONCAT next_function
       "static void fullscreen_deadline_handler(void *data, struct wl_callback *callback, uint32_t callback_data)\n")
string(CONCAT commit_deferred
       "// ${mark}\n"
       "// Commits the newest size of each window whose configure events waited for the end of the event pump.\n"
       "void Wayland_CommitDeferredResizes(SDL_VideoDevice *_this)\n"
       "{\n"
       "#ifdef HAVE_LIBDECOR_H\n"
       "    for (SDL_Window *window = _this->windows; window; window = window->next) {\n"
       "        SDL_WindowData *wind = window->internal;\n"
       "        if (wind && wind->libdecor_resize_deferred) {\n"
       "            wind->libdecor_resize_deferred = false;\n"
       "            if (wind->shell_surface_type == WAYLAND_SHELL_SURFACE_TYPE_LIBDECOR && wind->shell_surface.libdecor.frame) {\n"
       "                ConfigureWindowGeometry(window);\n"
       "                CommitLibdecorFrame(window);\n"
       "            }\n"
       "        }\n"
       "    }\n"
       "#endif\n"
       "}\n"
       "\n")
hokuto_replace_once(window_text "${next_function}" "${commit_deferred}${next_function}")

# The end of the event pump. CMakeLists.txt checks the sources for this line with its mark.
hokuto_replace_once(events_text
    "    if (ret >= 0) {\n        // Synthesize key repeat events.\n"
    "    if (ret >= 0) {\n        Wayland_CommitDeferredResizes(_this); // ${mark}\n\n        // Synthesize key repeat events.\n")

file(WRITE ${header} "${header_text}")
file(WRITE ${window} "${window_text}")
file(WRITE ${events} "${events_text}")
