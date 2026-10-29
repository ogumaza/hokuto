# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.24)
# The oldest macOS that the Mac's build runs on. The compiler and the linker require it before project(). Without it,
# they take the SDK's version, and the program refuses to start on any older macOS. The app's Info.plist gives the same
# version. A value on the command line or in MACOSX_DEPLOYMENT_TARGET takes its place.
if (CMAKE_HOST_APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET AND NOT DEFINED ENV{MACOSX_DEPLOYMENT_TARGET})
    set(CMAKE_OSX_DEPLOYMENT_TARGET 11.0 CACHE STRING "The oldest macOS that the program runs on" FORCE)
endif()
# The program shows this version. The file properties of the Windows program and the Info.plist of the macOS app show
# it too.
project(hokuto VERSION 1.0.0 LANGUAGES C CXX)
# On macOS, the menu bar and the title bar come from AppKit, through Objective-C++.
if (APPLE)
    enable_language(OBJCXX)
endif()

# The identifier of the macOS app. SDL3 reports it to the system on every platform.
set(HOKUTO_APP_ID org.hokuto.Hokuto)
set(HOKUTO_COPYRIGHT "Copyright (c) 2026 the Hokuto authors. MIT licence.")

foreach (submodule seqlib xsflib ntdlib)
    if (NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/externals/${submodule}/CMakeLists.txt")
        message(FATAL_ERROR "externals/${submodule} is missing. Run: git submodule update --init")
    endif()
endforeach()

# The C++ standard, a release build by default, the static C runtime, and the compiler options that keep output the
# same on every platform. xsflib sets them the same way for the players. HOKUTO_WARNINGS holds the warnings for
# Hokuto's targets.
include(externals/xsflib/cmake/XsflibProject.cmake)
xsflib_project(CHECK_COMPILERS NAME Hokuto PREFIX HOKUTO NOMINMAX)

# The shared libraries, each a submodule in externals/: seqlib for MIDI, SoundFont 2 and SFZ, xsflib for the player
# framework and the tests' checks, and ntdlib for the consoles. ntdlib's performer comes with its sequence export, and
# its model of the 3DS with Teakra, an emulator of the 3DS's DSP.
set(NTDLIB_SEQUENCE_EXPORT ON CACHE BOOL "" FORCE)
set(NTDLIB_CTR ON CACHE BOOL "" FORCE)
xsflib_add_libraries(seqlib xsflib ntdlib)
find_package(Threads REQUIRED)

include(FetchContent)

# SDL3 supplies the window, input, the GPU, audio output and file dialogs. Hokuto links it statically. The subsystems
# that Hokuto doesn't use stay out of the build. They would only add code and licences to the program.
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
foreach (subsystem CAMERA HAPTIC HIDAPI JOYSTICK POWER RENDER SENSOR TRAY)
    set(SDL_${subsystem} OFF CACHE BOOL "" FORCE)
endforeach()
FetchContent_Declare(SDL3
    URL https://github.com/libsdl-org/SDL/releases/download/release-3.4.18/SDL3-3.4.18.tar.gz
    URL_HASH SHA256=9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(SDL3)

# Dear ImGui draws the interface. Its docking branch lets the user arrange the views. Dear ImGui has no CMake build.
# This library builds its core and its back ends for SDL3 and SDL_GPU.
FetchContent_Declare(imgui
    URL https://github.com/ocornut/imgui/archive/refs/tags/v1.92.9b-docking.tar.gz
    URL_HASH SHA256=90ded916bd57db2e0e171b6b098940a47c6f5042725dcdc67fb19940ca8bfdcc
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(imgui)
add_library(hokuto_imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlgpu3.cpp
)
# SYSTEM keeps Dear ImGui's headers out of the warnings of Hokuto's code.
target_include_directories(hokuto_imgui SYSTEM PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
# 32-bit characters reach the glyphs outside the Basic Multilingual Plane, such as rare kanji. Every file that includes
# Dear ImGui's headers requires the same definitions.
target_compile_definitions(hokuto_imgui PUBLIC IMGUI_USE_WCHAR32 IMGUI_DISABLE_OBSOLETE_FUNCTIONS)
target_link_libraries(hokuto_imgui PUBLIC SDL3::SDL3)

# The tests' switch, BUILD_TESTING, comes before sfizz. sfizz's build would otherwise turn it off.
include(CTest)

# sfizz renders SFZ files. The Export window measures with it what an export to SFZ would sound like. Hokuto links it
# statically. sfizz builds in C++14, its default. Its clients, plugins, tests and benchmarks stay out of the build, and
# so do libsndfile, OpenMP and Qt. Only the parts that Hokuto links build. Hokuto's warnings stay off sfizz's sources,
# and so do sfizz's warnings. The download takes one fix that sfizz made after 1.2.3 (cmake/sfizz_loop_fix.cmake). The
# build refuses sources without it, such as a folder of FETCHCONTENT_SOURCE_DIR_SFIZZ from before the fix. Hokuto
# changes nothing else of sfizz's code. atomic_queue, a library inside sfizz, calls a member template without template
# arguments. Recent versions of Clang reject that unless its warning goes off. sfizz 1.2.3 turns its assertions and
# their messages on in every build. A failed assertion would stop the program. They go off below. On Apple's arm64,
# sfizz would pass the compiler the options of 32-bit ARM. The processor's name keeps them away. WavPack's assembly
# stays out too. sfizz decodes WavPack files with WavPack, and Hokuto's samples are WAV files. clang-cl ignores sfizz's
# -ffast-math, and /fp:fast takes its place there. Microsoft's C++ library brings in <math.h> without <cmath>. SIMDe, a
# library inside sfizz, then calls std::isnan. Only <cmath> declares it there. Under clang-cl, each of sfizz's C++ files
# therefore includes <cmath> first. sfizz puts its C and C++ standards in the cache. They leave it again: SDL3 would
# otherwise build as C99 from the next configure on.
function(hokuto_add_sfizz)
    set(CMAKE_CXX_STANDARD 14)
    set(CMAKE_DISABLE_FIND_PACKAGE_OpenMP ON)
    set(CMAKE_DISABLE_FIND_PACKAGE_Qt5 ON)
    set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
    set(BUILD_TESTING OFF)
    foreach (option SFIZZ_BENCHMARKS SFIZZ_DEMOS SFIZZ_DEVTOOLS SFIZZ_JACK SFIZZ_RENDER SFIZZ_SHARED SFIZZ_TESTS
                    SFIZZ_USE_SNDFILE WAVPACK_ENABLE_ASM)
        set(${option} OFF CACHE BOOL "" FORCE)
    endforeach()
    set(ABSL_PROPAGATE_CXX_STD ON CACHE BOOL "" FORCE)
    set(architectures ${CMAKE_OSX_ARCHITECTURES})
    if (NOT architectures)
        set(architectures ${CMAKE_SYSTEM_PROCESSOR})
    endif()
    if (APPLE AND architectures STREQUAL "arm64")
        set(PROJECT_SYSTEM_PROCESSOR aarch64)
    endif()
    string(APPEND CMAKE_C_FLAGS " -w")
    string(APPEND CMAKE_CXX_FLAGS " -w -Wno-missing-template-arg-list-after-template-kw")
    set(standards)
    foreach (language C CXX)
        if (NOT DEFINED CACHE{CMAKE_${language}_STANDARD})
            list(APPEND standards CMAKE_${language}_STANDARD)
        endif()
    endforeach()
    FetchContent_Declare(sfizz
        URL https://github.com/sfztools/sfizz/releases/download/1.2.3/sfizz-1.2.3.tar.gz
        URL_HASH SHA256=a9339eac7620d7f0f6b44bdfe860680fab73e66efad4b5f15b21198dd9436822
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        PATCH_COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/cmake/sfizz_loop_fix.cmake)
    FetchContent_MakeAvailable(sfizz)
    file(READ ${sfizz_SOURCE_DIR}/src/sfizz/Voice.cpp voice)
    string(FIND "${voice}" "while (wrappedIndex > loop.end) {" fixed)
    if (fixed EQUAL -1)
        message(FATAL_ERROR "The sfizz sources in ${sfizz_SOURCE_DIR} lack the fix that the build takes into 1.2.3 "
                            "(cmake/sfizz_loop_fix.cmake). Point FETCHCONTENT_SOURCE_DIR_SFIZZ at the _deps/sfizz-src "
                            "folder of a build folder that downloaded sfizz since the fix.")
    endif()
    set_property(DIRECTORY ${sfizz_SOURCE_DIR} PROPERTY EXCLUDE_FROM_ALL TRUE)
    foreach (standard ${standards})
        unset(${standard} CACHE)
    endforeach()
    foreach (target sfizz_internal sfizz_messaging sfizz_parser)
        foreach (property COMPILE_DEFINITIONS INTERFACE_COMPILE_DEFINITIONS)
            get_target_property(definitions ${target} ${property})
            if (definitions)
                list(REMOVE_ITEM definitions SFIZZ_ENABLE_RELEASE_ASSERT=1 SFIZZ_ENABLE_RELEASE_DBG=1)
                set_target_properties(${target} PROPERTIES ${property} "${definitions}")
            endif()
        endforeach()
    endforeach()
    if (HOKUTO_CLANG_CL)
        get_target_property(options sfizz_internal COMPILE_OPTIONS)
        list(TRANSFORM options REPLACE "^-ffast-math$" "/fp:fast")
        set_target_properties(sfizz_internal PROPERTIES COMPILE_OPTIONS "${options}")
        target_compile_options(sfizz_internal PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/FIcmath>)
    endif()
endfunction()
hokuto_add_sfizz()

# Hokuto's warnings for `target`. On Windows, its sources are UTF-8, and windows.h leaves out the parts they don't use.
function(hokuto_set_options target)
    target_compile_options(${target} PRIVATE ${HOKUTO_WARNINGS})
    if (MSVC)
        target_compile_options(${target} PRIVATE /utf-8)
    endif()
    if (WIN32)
        target_compile_definitions(${target} PRIVATE WIN32_LEAN_AND_MEAN)
    endif()
endfunction()

# The audio code that doesn't use SDL3: the ring buffer that the device's callback reads, the worker that renders ahead
# into it, the declicker that smooths the joins in the output, the resampler, the test tone and the clips that play
# once. Its tests run without an audio device.
add_library(hokuto_audio STATIC src/audio/clip.cpp src/audio/declicker.cpp src/audio/frame_ring.cpp
            src/audio/render_worker.cpp src/audio/resampler.cpp src/audio/test_tone.cpp)
target_include_directories(hokuto_audio PUBLIC src)
target_link_libraries(hokuto_audio PUBLIC Threads::Threads)
hokuto_set_options(hokuto_audio)
# The resampler's waves have to come out the same on every platform (xsflib_float_options).
xsflib_float_options(hokuto_float_options)
target_compile_options(hokuto_audio PRIVATE ${hokuto_float_options})

# The songs: a sound archive of any of the four consoles opened from a file, its sequences as the zones show them, and
# the player that plays a sequence on ntdlib's model of the console, with snapshots for seeking. ntdlib's performer
# reads the sequences, away from the program's thread. The browser's previews play a wave, a note of an instrument or a
# stream, and the Replace and Import windows play a sequence sound. The items go out as files, a wave and a stream as
# WAV files too. The streams sit in files beside an archive. ntdlib's converter exports a sequence to MIDI, SoundFont 2
# and SFZ, from the Wii's archives too. ntdlib's length analysis measures how long a player plays a sequence sound. The
# level measure compares the render of an export with the model's, for the Export window's warnings. The 3DS plays on
# ntdlib's archive mode of the 3DS, on ntdlib's model of the DSP's audio firmware. A player whose model changes its rate
# brings the model's frames to its rate, with ntdlib's windowed sinc. A sequence's biquad filters take their types and
# values on another console. A flow analysis follows the settings that a sequence's tracks hold.
add_library(hokuto_song STATIC src/song/aac.cpp src/song/archive.cpp src/song/audibility.cpp src/song/biquads.cpp
            src/song/catch_up.cpp src/song/ctr_model.cpp src/song/dry_render.cpp src/song/frame_resampler.cpp
            src/song/items.cpp src/song/lengths.cpp src/song/midi_export.cpp src/song/player.cpp src/song/preview.cpp
            src/song/sequence.cpp src/song/sequence_model.cpp src/song/sequence_preview.cpp
            src/song/sequence_reader.cpp src/song/streams.cpp src/song/track_flow.cpp)
target_include_directories(hokuto_song PUBLIC src)
target_link_libraries(hokuto_song PUBLIC hokuto_audio ntdlib::cafe ntdlib::ctr ntdlib::ctr_fx ntdlib::nwsnd_commands
                      ntdlib::nwsnd_lengths ntdlib::nx ntdlib::rsar ntdlib::rsar_layout ntdlib::rvl_snd ntdlib::sar
                      ntdlib::sar_layout ntdlib::sequence_export ntdlib::wave)
hokuto_set_options(hokuto_song)

# The system's codec decodes and encodes AAC streams: Audio Toolbox on macOS and Media Foundation on Windows. Other
# systems get none.
if (APPLE)
    target_sources(hokuto_song PRIVATE src/song/aac_mac.cpp)
    target_link_libraries(hokuto_song PUBLIC "-framework AudioToolbox" "-framework CoreFoundation")
elseif (WIN32)
    target_sources(hokuto_song PRIVATE src/song/aac_windows.cpp)
    target_link_libraries(hokuto_song PUBLIC mfplat mfuuid ole32)
else()
    target_sources(hokuto_song PRIVATE src/song/aac_other.cpp)
endif()

# The synths play an export the way its players would. The Export window renders an export with them to measure how
# the export would sound. Hokuto's renderer plays a MIDI file through a SoundFont 2 file. It follows the SoundFont 2.04
# specification and the conventions that the common SoundFont players share. sfizz plays a sequence's MIDI file through
# its SFZ files.
add_library(hokuto_synth STATIC src/synth/sf2_synth.cpp src/synth/sfz_synth.cpp src/synth/sound_font.cpp)
target_include_directories(hokuto_synth PUBLIC src)
target_link_libraries(hokuto_synth PUBLIC ntdlib::sequence_export seqlib::seqlib PRIVATE sfizz::sfizz xsflib::common)
hokuto_set_options(hokuto_synth)

# The measure of an export, for the Export window: a synth's render of an export to MIDI, or the render of a sound sent
# to another console on that console's model, against the model's dry render of the sequence, by the audibility method's
# level measure.
add_library(hokuto_measure STATIC src/measure/measure.cpp)
target_include_directories(hokuto_measure PUBLIC src)
target_link_libraries(hokuto_measure PUBLIC hokuto_song hokuto_synth)
hokuto_set_options(hokuto_measure)

# Hokuto's files (.hok): the model, the binary form and the text form, and the sound archives of the four consoles into
# files and back. ntdlib cuts an archive into pieces, through its rsar layout on the Wii. seqlib names the group files
# that an export writes.
add_library(hokuto_format STATIC src/format/archive.cpp src/format/codes.cpp src/format/command_values.cpp
                                 src/format/file.cpp src/format/kinds.cpp src/format/model.cpp src/format/text.cpp)
target_include_directories(hokuto_format PUBLIC src)
target_link_libraries(hokuto_format PUBLIC ntdlib::commands ntdlib::rsar_layout ntdlib::sar ntdlib::sar_layout
                      xsflib::common PRIVATE seqlib::seqlib)
hokuto_set_options(hokuto_format)

# hok, the command-line tool: sound archives into .hok files and back, the text form and the model form. It also writes
# an archive's items, its waves as WAV files and its banks as SoundFont 2 or SFZ files, measures the lengths of the
# sequence sounds, and sends a sequence sound to another console. Its import finds the group files beside an archive as
# the program does.
add_executable(hok src/cli/hok.cpp)
target_link_libraries(hok PRIVATE hokuto_edit hokuto_format hokuto_song)
hokuto_set_options(hok)
xsflib_add_version(hok VERSION ${PROJECT_VERSION} DEFINE HOKUTO_VERSION DESCRIPTION "Hokuto's command-line tool"
                   PRODUCT Hokuto COPYRIGHT ${HOKUTO_COPYRIGHT})

# The history of a file's versions, for undo and redo, the edits of a sequence's notes, of its tracks' mix and
# instruments and of any of its commands, what else such an edit reaches, new tracks, copies and removals of tracks, the
# replacement of a sound's sequence, an item's file or a stream, new sequence sounds, new items in groups, a sound's
# values in the archive's tables, a sequence sound as a file of another console, with the banks of its slots as one bank
# on the Wii, the layout of the songs that NintendoWare's MIDI converter wrote, and the timelines that edit them. The
# edits of banks, waves, streams and wave sound files go through models of them. Every edit goes through the history.
# Its tests run without a window.
add_library(hokuto_edit STATIC src/edit/banks.cpp src/edit/bytecode.cpp src/edit/commands.cpp src/edit/converter.cpp
            src/edit/effects.cpp src/edit/files.cpp src/edit/groups.cpp src/edit/history.cpp src/edit/instruments.cpp
            src/edit/lengths.cpp src/edit/mix.cpp src/edit/new_items.cpp src/edit/notes.cpp src/edit/other_console.cpp
            src/edit/reach.cpp src/edit/replace.cpp src/edit/streams.cpp src/edit/subroutines.cpp src/edit/tables.cpp
            src/edit/timelines.cpp src/edit/tracks.cpp src/edit/variables.cpp src/edit/waves.cpp
            src/edit/wii_programs.cpp)
target_include_directories(hokuto_edit PUBLIC src)
target_link_libraries(hokuto_edit PUBLIC hokuto_format hokuto_song PRIVATE ntdlib::nwsnd_commands ntdlib::wave
                      seqlib::seqlib)
hokuto_set_options(hokuto_edit)

# The transport and the ruler's gestures. They use neither SDL3 nor Dear ImGui, and their tests run without a window.
add_library(hokuto_transport STATIC src/app/ruler.cpp src/app/transport.cpp)
target_include_directories(hokuto_transport PUBLIC src)
hokuto_set_options(hokuto_transport)

# The list of File > Recent Projects. Its tests run without a window.
add_library(hokuto_recent_projects STATIC src/app/recent_projects.cpp)
target_include_directories(hokuto_recent_projects PUBLIC src)
target_link_libraries(hokuto_recent_projects PUBLIC xsflib::common)
hokuto_set_options(hokuto_recent_projects)

# What the Key Editor works out about its notes: the grid, the edits of the keys, the menu and the info line, the
# selection in a new version, and the note that sounds. It uses Dear ImGui's types but none of its functions, and its
# tests run without a window.
add_library(hokuto_key_edits STATIC src/app/key_edits.cpp)
target_include_directories(hokuto_key_edits PUBLIC src)
target_link_libraries(hokuto_key_edits PUBLIC hokuto_edit hokuto_imgui hokuto_transport)
hokuto_set_options(hokuto_key_edits)

# What the List Editor works out about a track's commands: the rows, the text of each column, the typed values, and the
# commands that it inserts and deletes. Its tests run without a window. The Key Editor's tests do too.
add_library(hokuto_list_edits STATIC src/app/list_edits.cpp)
target_include_directories(hokuto_list_edits PUBLIC src)
target_link_libraries(hokuto_list_edits PUBLIC hokuto_key_edits)
hokuto_set_options(hokuto_list_edits)

# What the Key Editor's controller lanes work out about a track's settings: the commands that set them, the line of each
# lane's values, and the edits of the lanes' gestures. Its tests run without a window.
add_library(hokuto_lanes STATIC src/app/lanes.cpp)
target_include_directories(hokuto_lanes PUBLIC src)
target_link_libraries(hokuto_lanes PUBLIC hokuto_list_edits)
hokuto_set_options(hokuto_lanes)

# The places where a song's tracks read variables or branch, for the arrangement's marker row, and the commands that set
# the tempo, for its tempo row. Its tests run without a window.
add_library(hokuto_markers STATIC src/app/markers.cpp src/app/tempo_row.cpp)
target_include_directories(hokuto_markers PUBLIC src)
target_link_libraries(hokuto_markers PUBLIC hokuto_list_edits)
hokuto_set_options(hokuto_markers)

# The view of a sound that a file keeps: from the transport, the arrangement and the editors, and back. Its tests run
# without a window.
add_library(hokuto_editor_view STATIC src/app/editor_view.cpp)
target_include_directories(hokuto_editor_view PUBLIC src)
target_link_libraries(hokuto_editor_view PUBLIC hokuto_lanes)
hokuto_set_options(hokuto_editor_view)

# What the Bank Editor and the properties of a wave or a stream show of their items, the waves that a drop places on a
# bank, and the places of a loop's markers. Its tests run without a window.
add_library(hokuto_item_views STATIC src/app/item_views.cpp)
target_include_directories(hokuto_item_views PUBLIC src)
target_link_libraries(hokuto_item_views PUBLIC hokuto_key_edits)
hokuto_set_options(hokuto_item_views)

# The audio device, through SDL3. Its test plays through SDL3's disk driver. The driver writes the device's frames to a
# file and makes no sound.
add_library(hokuto_audio_output STATIC src/app/audio_output.cpp)
target_include_directories(hokuto_audio_output PUBLIC src)
target_link_libraries(hokuto_audio_output PUBLIC hokuto_audio SDL3::SDL3)
hokuto_set_options(hokuto_audio_output)

# The program. On macOS it's the app bundle Hokuto.app.
add_executable(hokuto WIN32 MACOSX_BUNDLE
    src/app/app.cpp
    src/app/bank_editor.cpp
    src/app/control_bar.cpp
    src/app/effect_editor.cpp
    src/app/export_window.cpp
    src/app/key_editor.cpp
    src/app/list_editor.cpp
    src/app/main.cpp
    src/app/menus.cpp
    src/app/preview_song.cpp
    src/app/replace_window.cpp
    src/app/sequence_view.cpp
    src/app/sound_section.cpp
    src/app/theme.cpp
    src/app/views.cpp
    src/app/wave_window.cpp
    src/app/widgets.cpp
)
target_link_libraries(hokuto PRIVATE hokuto_editor_view hokuto_item_views hokuto_lanes hokuto_markers hokuto_list_edits
                      hokuto_key_edits hokuto_edit hokuto_format hokuto_measure hokuto_recent_projects hokuto_song
                      hokuto_transport hokuto_audio_output hokuto_imgui SDL3::SDL3)
target_compile_definitions(hokuto PRIVATE HOKUTO_APP_ID="${HOKUTO_APP_ID}")
hokuto_set_options(hokuto)
xsflib_add_version(hokuto VERSION ${PROJECT_VERSION} DEFINE HOKUTO_VERSION DESCRIPTION Hokuto PRODUCT Hokuto
                   COPYRIGHT ${HOKUTO_COPYRIGHT})

# The system's fonts come from Core Text on macOS and DirectWrite on Windows. Other systems get Dear ImGui's font. The
# Mac's menu bar and title bar come from AppKit. Other systems get the menus that the program draws. On Windows, the
# Desktop Window Manager colours the title bar, and the manifest gives the message boxes the system's look.
if (APPLE)
    target_sources(hokuto PRIVATE src/app/system_fonts_mac.cpp src/app/system_ui_mac.mm)
    set_source_files_properties(src/app/system_ui_mac.mm PROPERTIES COMPILE_OPTIONS -fobjc-arc)
    target_link_libraries(hokuto PRIVATE "-framework AppKit" "-framework CoreFoundation" "-framework CoreGraphics"
                          "-framework CoreText")
elseif (WIN32)
    target_sources(hokuto PRIVATE src/app/system_fonts_windows.cpp src/app/system_ui_windows.cpp
                   src/app/hokuto.manifest)
    target_link_libraries(hokuto PRIVATE dwmapi dwrite)
else()
    target_sources(hokuto PRIVATE src/app/system_fonts_other.cpp src/app/system_ui_other.cpp)
endif()

# The icon. macOS takes it from the bundle's resources. Windows takes the first icon in the program's resources, for
# Explorer and for the window.
set(hokuto_icons ${CMAKE_CURRENT_SOURCE_DIR}/assets/logo)
if (APPLE)
    target_sources(hokuto PRIVATE ${hokuto_icons}/hokuto.icns)
    set_source_files_properties(${hokuto_icons}/hokuto.icns PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    set_target_properties(hokuto PROPERTIES
        OUTPUT_NAME Hokuto
        MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/src/app/Info.plist.in
        MACOSX_BUNDLE_BUNDLE_NAME Hokuto
        MACOSX_BUNDLE_GUI_IDENTIFIER ${HOKUTO_APP_ID}
        MACOSX_BUNDLE_ICON_FILE hokuto.icns
        MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION}
        MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
        MACOSX_BUNDLE_COPYRIGHT ${HOKUTO_COPYRIGHT}
    )
    # The linker signs the executable ad hoc. Signing the bundle ad hoc seals its Info.plist and icon too. An ad hoc
    # signature names nobody.
    add_custom_command(TARGET hokuto POST_BUILD COMMAND codesign --force --sign - $<TARGET_BUNDLE_DIR:hokuto>
                       VERBATIM)
elseif (WIN32)
    set(icon ${hokuto_icons}/hokuto.ico)
    file(CONFIGURE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/hokuto_icon.rc
         CONTENT "#pragma code_page(65001)\n1 ICON \"@icon@\"\n" @ONLY)
    target_sources(hokuto PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/hokuto_icon.rc)
endif()

# The tools. hokuto_check_playback checks the playback of every sequence of a sound archive against ntdlib's model run
# directly. hokuto_check_layout checks ntdlib's cutting and joining of sound archives and its additions to their tables:
# the round trip, the tables after every sequence grows, and new files, sounds, banks, wave archives and groups.
# hokuto_check_edits edits every note command of a sound archive, and checks what ntdlib's performer plays afterwards.
# hokuto_check_mix edits the volume, pan and sends of a track of each sequence, and checks the same.
# hokuto_check_exports compares the exports to MIDI with the converters' files. hokuto_check_items checks the exports of
# single items, waves, wave sounds, streams and the instruments of banks, and the conversions of each file for the other
# console. hokuto_check_replace makes the edits of Replace… and File > Import for every kind of item, and checks the
# archive and what ntdlib's performer or model plays afterwards. hokuto_check_sounds edits the sounds' values in the
# archive's tables, and checks the archive and the level of what ntdlib's model plays. hokuto_check_commands edits
# commands as the List Editor does, hokuto_check_tracks adds, copies and removes tracks, and hokuto_check_instruments
# gives a track's notes another instrument. Each checks what ntdlib's performer plays afterwards. hokuto_check_converter
# lays out every song of NintendoWare's MIDI converter again, and checks that each comes back byte for byte.
# hokuto_check_subroutines edits the labels and the subroutines of every sequence as the List Editor does, and checks
# the sequences and what ntdlib's performer plays afterwards. hokuto_check_players checks the browser's players of wave
# sounds and stream sounds. hokuto_check_model puts an archive's file in the model form, and checks that every sound
# plays the same from the archive that the model builds. hokuto_check_banks edits every bank, wave, stream and wave
# sound through its model, and checks the exports and what ntdlib's model plays afterwards. hokuto_check_sfz renders
# every sequence of an archive through its export to MIDI and SFZ with sfizz, and reports the failures, the peak levels
# and the times. hokuto_check_synth does the same through the export to MIDI and SoundFont 2 with Hokuto's renderer. It
# can also compare the renders with FluidSynth's, run as a black box. hokuto_check_measure measures the export of every
# sequence of an archive to MIDI as the Export window does, and reports the places that may be audible.
# hokuto_check_other_console sends sequence sounds to another console, and compares the levels of their dry renders on
# the models of both consoles. hokuto_check_lengths plays each sound of a file with lengths past its length, and checks
# that an end stays silent and a loop plays on. hokuto_make_conformance makes the files of the conformance suite in
# tests/conformance/.
option(HOKUTO_TOOLS "Build the tools in tools/" OFF)
if (HOKUTO_TOOLS)
    add_executable(hokuto_check_playback tools/check_playback.cpp)
    target_link_libraries(hokuto_check_playback PRIVATE hokuto_song)
    hokuto_set_options(hokuto_check_playback)
    add_executable(hokuto_check_layout tools/check_layout.cpp)
    target_link_libraries(hokuto_check_layout PRIVATE hokuto_format hokuto_song)
    hokuto_set_options(hokuto_check_layout)
    add_executable(hokuto_check_edits tools/check_edits.cpp)
    target_link_libraries(hokuto_check_edits PRIVATE hokuto_edit)
    hokuto_set_options(hokuto_check_edits)
    add_executable(hokuto_check_mix tools/check_mix.cpp)
    target_link_libraries(hokuto_check_mix PRIVATE hokuto_edit)
    hokuto_set_options(hokuto_check_mix)
    add_executable(hokuto_check_exports tools/check_exports.cpp)
    target_link_libraries(hokuto_check_exports PRIVATE hokuto_song hokuto_format)
    hokuto_set_options(hokuto_check_exports)
    add_executable(hokuto_check_items tools/check_items.cpp)
    target_link_libraries(hokuto_check_items PRIVATE hokuto_song hokuto_format)
    hokuto_set_options(hokuto_check_items)
    add_executable(hokuto_check_replace tools/check_replace.cpp)
    target_link_libraries(hokuto_check_replace PRIVATE hokuto_edit)
    hokuto_set_options(hokuto_check_replace)
    add_executable(hokuto_check_sounds tools/check_sounds.cpp)
    target_link_libraries(hokuto_check_sounds PRIVATE hokuto_edit)
    hokuto_set_options(hokuto_check_sounds)
    add_executable(hokuto_check_commands tools/check_commands.cpp)
    target_link_libraries(hokuto_check_commands PRIVATE hokuto_markers)
    hokuto_set_options(hokuto_check_commands)
    add_executable(hokuto_check_tracks tools/check_tracks.cpp)
    target_link_libraries(hokuto_check_tracks PRIVATE hokuto_edit)
    hokuto_set_options(hokuto_check_tracks)
    add_executable(hokuto_check_instruments tools/check_instruments.cpp)
    target_link_libraries(hokuto_check_instruments PRIVATE hokuto_edit)
    hokuto_set_options(hokuto_check_instruments)
    add_executable(hokuto_check_converter tools/check_converter.cpp)
    target_link_libraries(hokuto_check_converter PRIVATE hokuto_edit)
    hokuto_set_options(hokuto_check_converter)
    add_executable(hokuto_check_subroutines tools/check_subroutines.cpp)
    target_link_libraries(hokuto_check_subroutines PRIVATE hokuto_list_edits)
    hokuto_set_options(hokuto_check_subroutines)
    add_executable(hokuto_check_players tools/check_players.cpp)
    target_link_libraries(hokuto_check_players PRIVATE hokuto_song)
    hokuto_set_options(hokuto_check_players)
    add_executable(hokuto_check_model tools/check_model.cpp)
    target_link_libraries(hokuto_check_model PRIVATE hokuto_song hokuto_format)
    hokuto_set_options(hokuto_check_model)
    add_executable(hokuto_check_banks tools/check_banks.cpp)
    target_link_libraries(hokuto_check_banks PRIVATE hokuto_edit)
    hokuto_set_options(hokuto_check_banks)
    add_executable(hokuto_check_sfz tools/check_sfz.cpp)
    target_link_libraries(hokuto_check_sfz PRIVATE hokuto_song hokuto_synth)
    hokuto_set_options(hokuto_check_sfz)
    add_executable(hokuto_check_synth tools/check_synth.cpp)
    target_link_libraries(hokuto_check_synth PRIVATE hokuto_song hokuto_synth)
    hokuto_set_options(hokuto_check_synth)
    add_executable(hokuto_check_measure tools/check_measure.cpp)
    target_link_libraries(hokuto_check_measure PRIVATE hokuto_measure hokuto_edit)
    hokuto_set_options(hokuto_check_measure)
    add_executable(hokuto_check_other_console tools/check_other_console.cpp)
    target_link_libraries(hokuto_check_other_console PRIVATE hokuto_edit)
    hokuto_set_options(hokuto_check_other_console)
    add_executable(hokuto_check_lengths tools/check_lengths.cpp)
    target_link_libraries(hokuto_check_lengths PRIVATE hokuto_edit)
    hokuto_set_options(hokuto_check_lengths)
    add_executable(hokuto_make_conformance tools/make_conformance.cpp)
    target_include_directories(hokuto_make_conformance PRIVATE tests)
    target_link_libraries(hokuto_make_conformance PRIVATE hokuto_edit)
    hokuto_set_options(hokuto_make_conformance)
endif()

if (BUILD_TESTING)
    # The ring buffer, rendering ahead through it, and the declicker. None of these tests requires an audio device. The
    # audio output's test suspends and resumes a device of SDL3's disk driver, and reads the frames that it wrote. The
    # format's test and the songs' test make their archives, without game data. The songs' test plays its sequences. The
    # ruler's test drives its gestures without a window, and the transport's test its commands. The recent projects'
    # test keeps the list of File > Recent Projects. The edit test steps through a history of versions. The notes' test,
    # the mix's test, the instruments' test, the tracks' test and the commands' test edit an archive that they make, and
    # ntdlib's performer plays the result. The reach's test finds what else such edits reach. The subroutines' test
    # edits a program's labels and subroutines. The other console's test sends a song to each other console and back.
    # The replacement's test puts a sequence file in the place of a sound's sequence. The files' test puts banks, wave
    # archives and wave sound files in the place of others. The streams' test reads the streams beside an archive and
    # puts others in their place. The new items' test adds sequence sounds with new files, wave archives, banks and wave
    # sounds, on the Wii too. The tables' test changes a sound's values in the archive's tables. The variables' test
    # moves and fixes the variables of a new sequence. The resampler's test brings sines to other rates. The groups'
    # test finds the groups that load an item, and puts the item's files into groups. The lengths' test measures the
    # lengths of sequence sounds for players. The Key Editor's test works out its edits and the note that sounds without
    # a window, the List Editor's test its rows and edits, the lanes' test the controller lanes' lines and edits, the
    # markers' test the places where the tracks read variables or branch, the tempo row's test the commands that set the
    # tempo, and the editor view's test the view that a file keeps. The converter's test lays out songs that it writes
    # as NintendoWare's MIDI converter does. The model's test puts files in the model form, edits them there and reads
    # them back. The banks' test edits banks, waves and wave sounds through their models. The item views' test works out
    # what the Bank Editor and a wave's properties show, the waves that a drop places on a bank, and the places of a
    # loop's markers. The audibility test measures how a render's levels differ from a reference's. The clip's test
    # plays a clip once, with its fades. The SoundFont 2 synth's test renders MIDI files through SoundFonts with
    # Hokuto's renderer, and the SFZ synth's test through SFZ files with sfizz. The measure's test measures the exports
    # of a small archive against the model's renders. The 3DS's test plays a 3DS archive on ntdlib's model of the DSP's
    # audio firmware, in both mixing modes, dry and with the 3DS's effects. The frame resampler's test brings a sine
    # between the frames of the 3DS's two modes. The biquads' test works out the passbands of the biquad filters'
    # presets at each console's rate, and the types and values that a sequence's filters take on another console. The
    # Wii's programs' test gives the bank slots of made-up sequences ranges of one bank's programs, as a sound that goes
    # to the Wii takes them. The conformance test renders the files of the conformance suite, and checks the hashes of
    # the renders. The AAC test decodes an AAC stream on the system's decoder, and exports and plays it as Hokuto does.
    # It also encodes on the system's encoder, and checks what the encoder and the decoder refuse.
    xsflib_add_project_test(hokuto aac hokuto_song)
    target_compile_definitions(hokuto_aac_test PRIVATE HOKUTO_AAC_DIR="${PROJECT_SOURCE_DIR}/tests/aac")
    xsflib_add_project_test(hokuto audibility hokuto_song)
    xsflib_add_project_test(hokuto audio_output hokuto_audio_output xsflib::common)
    xsflib_add_project_test(hokuto banks hokuto_edit)
    xsflib_add_project_test(hokuto biquads hokuto_song)
    xsflib_add_project_test(hokuto clip hokuto_audio)
    xsflib_add_project_test(hokuto conformance hokuto_edit)
    target_compile_definitions(hokuto_conformance_test
                               PRIVATE HOKUTO_CONFORMANCE_DIR="${PROJECT_SOURCE_DIR}/tests/conformance")
    xsflib_add_project_test(hokuto commands hokuto_edit)
    xsflib_add_project_test(hokuto converter hokuto_edit)
    xsflib_add_project_test(hokuto ctr hokuto_edit)
    xsflib_add_project_test(hokuto declicker hokuto_audio)
    xsflib_add_project_test(hokuto edit hokuto_edit)
    xsflib_add_project_test(hokuto editor_view hokuto_editor_view)
    xsflib_add_project_test(hokuto files hokuto_edit)
    xsflib_add_project_test(hokuto format hokuto_format)
    xsflib_add_project_test(hokuto frame_resampler hokuto_song)
    xsflib_add_project_test(hokuto frame_ring hokuto_audio)
    xsflib_add_project_test(hokuto groups hokuto_edit)
    xsflib_add_project_test(hokuto instruments hokuto_edit)
    xsflib_add_project_test(hokuto item_views hokuto_item_views)
    xsflib_add_project_test(hokuto key_edits hokuto_key_edits)
    xsflib_add_project_test(hokuto lanes hokuto_lanes)
    xsflib_add_project_test(hokuto lengths hokuto_edit)
    xsflib_add_project_test(hokuto list_edits hokuto_list_edits)
    xsflib_add_project_test(hokuto markers hokuto_markers)
    xsflib_add_project_test(hokuto measure hokuto_measure hokuto_edit)
    xsflib_add_project_test(hokuto mix hokuto_edit)
    xsflib_add_project_test(hokuto model hokuto_edit)
    xsflib_add_project_test(hokuto new_items hokuto_edit)
    xsflib_add_project_test(hokuto notes hokuto_edit)
    xsflib_add_project_test(hokuto other_console hokuto_edit)
    xsflib_add_project_test(hokuto reach hokuto_edit)
    xsflib_add_project_test(hokuto recent_projects hokuto_recent_projects)
    xsflib_add_project_test(hokuto render_worker hokuto_audio)
    xsflib_add_project_test(hokuto resampler hokuto_audio)
    xsflib_add_project_test(hokuto replace hokuto_edit)
    xsflib_add_project_test(hokuto ruler hokuto_transport)
    xsflib_add_project_test(hokuto sf2_synth hokuto_synth)
    xsflib_add_project_test(hokuto sfz_synth hokuto_song hokuto_synth)
    xsflib_add_project_test(hokuto song hokuto_song)
    xsflib_add_project_test(hokuto streams hokuto_edit)
    xsflib_add_project_test(hokuto subroutines hokuto_edit)
    xsflib_add_project_test(hokuto tables hokuto_edit)
    xsflib_add_project_test(hokuto tempo_row hokuto_markers)
    xsflib_add_project_test(hokuto tracks hokuto_edit)
    xsflib_add_project_test(hokuto transport hokuto_transport)
    xsflib_add_project_test(hokuto variables hokuto_edit)
    xsflib_add_project_test(hokuto wii_programs hokuto_edit)
endif()
