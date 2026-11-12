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
project(hokuto VERSION 2.0.0 LANGUAGES C CXX)
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

# Hokuto's designated initializers leave the fields they don't name at their defaults on purpose. Clang 19 and later
# warn about each such field under -Wextra, and clang-cl under /W4. Apple's Clang doesn't yet. The warning stays off
# for the targets below, so that every platform builds without warnings.
include(CheckCXXCompilerFlag)
check_cxx_compiler_flag(-Wmissing-designated-field-initializers HOKUTO_WARNS_DESIGNATED_FIELDS)
if (HOKUTO_WARNS_DESIGNATED_FIELDS)
    add_compile_options(-Wno-missing-designated-field-initializers)
endif()

# A switch over an enum that leaves out one of its values fails the build, where the compiler supports the option. A
# new engine or kind then can't go unnoticed in a switch without a default.
check_cxx_compiler_flag(-Werror=switch HOKUTO_SWITCH_ERRORS)
if (HOKUTO_SWITCH_ERRORS)
    list(APPEND HOKUTO_WARNINGS -Werror=switch)
endif()

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
    URL https://github.com/libsdl-org/SDL/releases/download/release-3.4.20/SDL3-3.4.20.tar.gz
    URL_HASH SHA256=f3bb06f55bed9557227a700af2ea62e49497ebdf58037732a731a5fb3ef8581f
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

# The rules of the commands of Brownie Brown's, Quintet's and Ubisoft Milan's GBA drivers in a Hokuto file. The format
# layer, the song layer and the edit layer share them. The song layer links them without the format layer.
add_library(hokuto_driver_rules STATIC src/format/brownie_items.cpp src/format/quintet_rules.cpp
            src/format/ubimilan_rules.cpp)
target_include_directories(hokuto_driver_rules PUBLIC src)
target_link_libraries(hokuto_driver_rules PUBLIC ntdlib::brownie ntdlib::commands ntdlib::quintet ntdlib::ubimilan)
hokuto_set_options(hokuto_driver_rules)

# The songs: a sound archive of any of the four consoles opened from a file, its sequences as the zones show them, and
# the player that plays a sequence on ntdlib's model of the console, with snapshots for seeking. ntdlib's performer
# reads the sequences, away from the program's thread. The browser's previews play a wave, a note of an instrument or a
# stream, and the Replace and Import windows play a sequence sound. The items go out as files, a wave and a stream as
# WAV files too. The streams sit in files beside an archive. ntdlib's converter exports a sequence to MIDI, SoundFont 2
# and SFZ, from the Wii's archives too. ntdlib's length analysis measures how long a player plays a sequence sound. The
# level measure compares the render of an export with the model's, for the Export window's warnings. The 3DS plays on
# ntdlib's archive mode of the 3DS, on ntdlib's model of the DSP's audio firmware. A player whose model changes its rate
# brings the model's frames to its rate, with ntdlib's windowed sinc. A sequence's biquad filters take their types and
# values on another console. A flow analysis follows the settings that a sequence's tracks hold. A GBA ROM's songs play
# on ntdlib's models of MusicPlayer2000 and of the GBA's sound hardware. A DS archive's sequences play on ntdlib's
# models of NITRO-Composer and of the DS's sound unit.
add_library(hokuto_song STATIC src/song/aac.cpp src/song/archive.cpp src/song/audibility.cpp src/song/aux_effects.cpp
            src/song/biquads.cpp src/song/catch_up.cpp src/song/ctr_model.cpp src/song/dry_render.cpp src/song/ds.cpp
            src/song/dse.cpp src/song/frame_resampler.cpp src/song/gba.cpp src/song/gba_assembler.cpp
            src/song/gba_assembly.cpp src/song/gba_samples.cpp src/song/driver.cpp src/song/items.cpp
            src/song/items_convert.cpp src/song/items_wii.cpp src/song/konami.cpp src/song/lengths.cpp
            src/song/brownie.cpp src/song/krawall.cpp src/song/quintet.cpp src/song/rare.cpp src/song/rd2.cpp
            src/song/ubimilan.cpp src/song/midi_export.cpp src/song/played.cpp src/song/player.cpp src/song/preview.cpp
            src/song/preview_ds.cpp src/song/preview_gba.cpp src/song/sequence.cpp src/song/sequence_brownie.cpp
            src/song/sequence_ds.cpp src/song/sequence_konami.cpp src/song/sequence_krawall.cpp
            src/song/sequence_quintet.cpp src/song/sequence_rare.cpp src/song/sequence_rd2.cpp
            src/song/sequence_timing.cpp src/song/sequence_ubimilan.cpp src/song/sequence_variables.cpp
            src/song/console_ctr.cpp src/song/console_ds.cpp src/song/console_gba.cpp src/song/console_switch_wiiu.cpp
            src/song/console_wii.cpp src/song/sequence_model.cpp src/song/sequence_preview.cpp
            src/song/sequence_reader.cpp src/song/streams.cpp src/song/track_flow.cpp)
target_include_directories(hokuto_song PUBLIC src)
target_link_libraries(hokuto_song PUBLIC hokuto_audio hokuto_driver_rules ntdlib::cafe ntdlib::ctr ntdlib::ctr_fx
                      ntdlib::dse_layout ntdlib::brownie_player ntdlib::konami_player ntdlib::krawall_player
                      ntdlib::mp2k_player
                      ntdlib::quintet_player ntdlib::rare_player ntdlib::rd2_player ntdlib::ntr_profiles
                      ntdlib::ubimilan_player
                      ntdlib::nwsnd_commands ntdlib::nwsnd_lengths ntdlib::nx ntdlib::rsar ntdlib::rsar_layout
                      ntdlib::rvl_snd ntdlib::sar ntdlib::sar_layout ntdlib::sequence_export
                      ntdlib::sequence_export_dse ntdlib::sequence_export_konami ntdlib::sequence_export_mp2k
                      ntdlib::sequence_export_brownie ntdlib::sequence_export_krawall ntdlib::sequence_export_quintet
                      ntdlib::sequence_export_rare ntdlib::sequence_export_rd2 ntdlib::sequence_export_ubimilan
                      ntdlib::sequence_export_ntr
                      ntdlib::wave)
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

# Hokuto's reader of SoundFont 2 files: the renderer reads them, and the imports make banks of them.
add_library(hokuto_sound_font STATIC src/synth/sound_font.cpp)
target_include_directories(hokuto_sound_font PUBLIC src)
target_link_libraries(hokuto_sound_font PUBLIC seqlib::seqlib PRIVATE xsflib::common)
hokuto_set_options(hokuto_sound_font)

# The synths play an export the way its players would. The Export window renders an export with them to measure how
# the export would sound. Hokuto's renderer plays a MIDI file through a SoundFont 2 file. It follows the SoundFont 2.04
# specification and the conventions that the common SoundFont players share. sfizz plays a sequence's MIDI file through
# its SFZ files.
add_library(hokuto_synth STATIC src/synth/sf2_synth.cpp src/synth/sfz_synth.cpp)
target_include_directories(hokuto_synth PUBLIC src)
target_link_libraries(hokuto_synth PUBLIC hokuto_sound_font ntdlib::sequence_export seqlib::seqlib PRIVATE sfizz::sfizz
                      xsflib::common)
hokuto_set_options(hokuto_synth)

# The measure of an export, for the Export window: a synth's render of an export to MIDI, or the render of a sound sent
# to another console on that console's model, against the model's dry render of the sequence, by the audibility method's
# level measure.
add_library(hokuto_measure STATIC src/measure/measure.cpp)
target_include_directories(hokuto_measure PUBLIC src)
target_link_libraries(hokuto_measure PUBLIC hokuto_song hokuto_synth)
hokuto_set_options(hokuto_measure)

# Hokuto's files (.hok): the model, the binary form and the text form, and the sound archives of the four consoles and
# the GBA's ROMs into files and back. ntdlib cuts an archive into pieces, through its rsar layout on the Wii and its
# mp2k layout on the GBA. seqlib names the group files that an export writes.
add_library(hokuto_format STATIC src/format/archive.cpp src/format/archive_ds.cpp src/format/archive_gba.cpp
            src/format/archive_tables.cpp src/format/brownie.cpp src/format/codes.cpp src/format/command_values.cpp
            src/format/drivers.cpp src/format/file.cpp src/format/file_checks.cpp src/format/file_chunks.cpp
            src/format/file_sequences.cpp src/format/kinds.cpp src/format/konami.cpp src/format/krawall.cpp
            src/format/model.cpp src/format/model_chunks.cpp src/format/model_wii.cpp src/format/quintet.cpp
            src/format/rare.cpp src/format/rd2.cpp src/format/text.cpp src/format/text_commands.cpp
            src/format/text_read.cpp src/format/text_sections.cpp src/format/text_values.cpp src/format/ubimilan.cpp)
target_include_directories(hokuto_format PUBLIC src)
target_link_libraries(hokuto_format PUBLIC hokuto_driver_rules ntdlib::brownie ntdlib::commands ntdlib::konami
                      ntdlib::krawall ntdlib::mp2k ntdlib::quintet ntdlib::rare ntdlib::rd2 ntdlib::ubimilan ntdlib::ntr
                      ntdlib::rsar_layout ntdlib::sar ntdlib::sar_layout ntdlib::sdat_cut ntdlib::dse_layout
                      xsflib::common PRIVATE ntdlib::ntr_snd seqlib::seqlib)
hokuto_set_options(hokuto_format)

# hok, the command-line tool: sound archives into .hok files and back, the text form and the model form. It also writes
# an archive's items, its waves as WAV files and its banks as SoundFont 2 or SFZ files, measures the lengths of the
# sequence sounds, and sends a sequence sound to another console. Its import finds the group files beside an archive as
# the program does.
add_executable(hok src/cli/hok.cpp src/cli/hok_items.cpp)
target_link_libraries(hok PRIVATE hokuto_edit hokuto_format hokuto_song)
# On Windows, UTF-8 is hok's code page, so the paths that it's given can hold any character.
xsflib_add_utf8_manifest(hok)
hokuto_set_options(hok)
xsflib_add_version(hok VERSION ${PROJECT_VERSION} DEFINE HOKUTO_VERSION DESCRIPTION "Hokuto's command-line tool"
                   PRODUCT Hokuto COPYRIGHT ${HOKUTO_COPYRIGHT})

# The history of a file's versions, for undo and redo, the edits of a sequence's notes, of its tracks' mix and
# instruments and of any of its commands, what else such an edit reaches, new tracks, copies and removals of tracks,
# the replacement of a sound's sequence, an item's file or a stream, new sequence sounds, new items in groups, a
# sound's values in the archive's tables, a sequence sound as a file of another console, with the banks of its slots
# as one bank on the Wii, the layout of the songs that NintendoWare's MIDI converter wrote, and the timelines that
# edit them. A MIDI file becomes such a song, and a SoundFont's presets a bank. A GBA song's edits follow the rules of
# MusicPlayer2000's bytes, and a GBA song goes to another console through the MIDI file and the SoundFont of its export.
# The edits of banks, waves, streams and wave sound files go through models of them. A DS bank's regions and a DS wave's
# parameters change in place. Every edit goes through the history. Its tests run without a window.
add_library(hokuto_edit STATIC src/edit/aac_streams.cpp src/edit/banks.cpp src/edit/bytecode.cpp
            src/edit/command_changes.cpp src/edit/command_ranges.cpp src/edit/command_removals.cpp
            src/edit/command_rows.cpp src/edit/console_commands.cpp src/edit/converted_song.cpp
            src/edit/converter.cpp src/edit/converter_header.cpp src/edit/converter_hints.cpp
            src/edit/converter_layout.cpp src/edit/converter_loops.cpp src/edit/ctr_memory.cpp
            src/edit/ds_bank_conversion.cpp src/edit/ds_banks.cpp src/edit/ds_conversion.cpp src/edit/ds_files.cpp
            src/edit/dse_bank_files.cpp src/edit/dse_banks.cpp src/edit/dse_files.cpp src/edit/dse_tracks.cpp
            src/edit/ds_new_items.cpp src/edit/ds_streams.cpp src/edit/ds_tables.cpp src/edit/ds_waves.cpp
            src/edit/effects.cpp src/edit/file_replace.cpp src/edit/files.cpp src/edit/files_internal.cpp
            src/edit/from_converters.cpp src/edit/from_wii.cpp src/edit/gba_midi.cpp src/edit/gba_midi_reader.cpp
            src/edit/gba_midi_tracks.cpp src/edit/gba_sound_fonts.cpp src/edit/gba_tables.cpp src/edit/gba_tracks.cpp
            src/edit/gba_voices.cpp src/edit/groups.cpp src/edit/history.cpp src/edit/instruments.cpp
            src/edit/lengths.cpp src/edit/midi_events.cpp src/edit/midi_import.cpp src/edit/midi_imports.cpp
            src/edit/midi_songs.cpp src/edit/mix.cpp src/edit/konami.cpp src/edit/konami_notes.cpp
            src/edit/konami_samples.cpp src/edit/konami_tracks.cpp src/edit/brownie.cpp src/edit/brownie_lengths.cpp
            src/edit/brownie_notes.cpp src/edit/brownie_timeline.cpp src/edit/drivers.cpp src/edit/engine_edits.cpp
            src/edit/krawall.cpp src/edit/krawall_patterns.cpp src/edit/mp2k.cpp src/edit/native_runs.cpp
            src/edit/quintet.cpp src/edit/rare.cpp src/edit/rd2.cpp src/edit/rd2_tracks.cpp src/edit/ubimilan.cpp
            src/edit/new_items.cpp src/edit/new_stream_items.cpp src/edit/note_blocks.cpp src/edit/note_rewrite.cpp
            src/edit/note_wait_edits.cpp src/edit/notes.cpp src/edit/other_console.cpp src/edit/other_console_parts.cpp
            src/edit/reach.cpp src/edit/replace.cpp src/edit/single_bank.cpp src/edit/sound_font_banks.cpp
            src/edit/sound_font_imports.cpp src/edit/sound_font_voices.cpp src/edit/sfz_imports.cpp
            src/edit/sfz_parser.cpp src/edit/streams.cpp src/edit/subroutines.cpp src/edit/tables.cpp
            src/edit/timelines.cpp src/edit/to_ds.cpp src/edit/to_wii.cpp src/edit/track_common.cpp
            src/edit/track_copies.cpp src/edit/tracks.cpp src/edit/variables.cpp src/edit/wav_waves.cpp
            src/edit/waves.cpp src/edit/wii_files.cpp src/edit/wii_new_items.cpp src/edit/wii_programs.cpp
            src/edit/wii_streams.cpp src/edit/wii_waves.cpp)
target_include_directories(hokuto_edit PUBLIC src)
target_link_libraries(hokuto_edit PUBLIC hokuto_format hokuto_song PRIVATE hokuto_sound_font ntdlib::nwsnd_commands
                      ntdlib::brownie ntdlib::sequence_export ntdlib::rd2 ntdlib::wave seqlib::seqlib)
hokuto_set_options(hokuto_edit)

# The transport and the ruler's gestures. They use neither SDL3 nor Dear ImGui, and their tests run without a window.
add_library(hokuto_transport STATIC src/app/ruler.cpp src/app/time_text.cpp src/app/transport.cpp)
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
add_library(hokuto_list_edits STATIC src/app/list_edits.cpp src/app/list_edits_rows.cpp src/app/list_edits_text.cpp
                                     src/app/list_edits_typing.cpp)
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
add_library(hokuto_item_views STATIC src/app/item_views.cpp src/app/item_views_ds.cpp src/app/item_views_gba.cpp
                                     src/app/item_views_sar.cpp)
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
    src/app/app_bank_editor.cpp
    src/app/app_bank_editor_ds.cpp
    src/app/app_bank_editor_gba.cpp
    src/app/app_browser.cpp
    src/app/app_browser_ds.cpp
    src/app/app_browser_gba.cpp
    src/app/app_commands.cpp
    src/app/app_dialogs.cpp
    src/app/app_engines.cpp
    src/app/app_export.cpp
    src/app/app_export_measure.cpp
    src/app/app_export_midi.cpp
    src/app/app_export_write.cpp
    src/app/app_files.cpp
    src/app/app_history.cpp
    src/app/app_import.cpp
    src/app/app_import_items.cpp
    src/app/app_import_waves.cpp
    src/app/app_internal.cpp
    src/app/app_item_export.cpp
    src/app/app_item_export_ds_gba.cpp
    src/app/app_key_editor.cpp
    src/app/app_key_feedback.cpp
    src/app/app_list_editor.cpp
    src/app/app_menus.cpp
    src/app/app_output.cpp
    src/app/app_playback.cpp
    src/app/app_previews.cpp
    src/app/app_properties.cpp
    src/app/app_replace.cpp
    src/app/app_replace_ds_gba.cpp
    src/app/app_replace_window.cpp
    src/app/app_settings.cpp
    src/app/app_setup.cpp
    src/app/app_shown_song.cpp
    src/app/app_song_edits.cpp
    src/app/app_sound_section.cpp
    src/app/app_variables.cpp
    src/app/arrangement.cpp
    src/app/arrangement_markers.cpp
    src/app/arrangement_tempo.cpp
    src/app/arrangement_tracks.cpp
    src/app/bank_editor.cpp
    src/app/bank_editor_driver.cpp
    src/app/bank_editor_ds.cpp
    src/app/bank_editor_dse.cpp
    src/app/bank_editor_gba.cpp
    src/app/bank_editor_sar.cpp
    src/app/browser.cpp
    src/app/channel_strip.cpp
    src/app/control_bar.cpp
    src/app/effect_editor.cpp
    src/app/engine_names.cpp
    src/app/export_warnings.cpp
    src/app/export_window.cpp
    src/app/faders.cpp
    src/app/file_dialogs.cpp
    src/app/icons.cpp
    src/app/info_line.cpp
    src/app/inspector.cpp
    src/app/key_canvas.cpp
    src/app/key_editor.cpp
    src/app/key_editor_draw.cpp
    src/app/key_editor_mouse.cpp
    src/app/list_editor.cpp
    src/app/main.cpp
    src/app/menus.cpp
    src/app/mixer.cpp
    src/app/preview_song.cpp
    src/app/replace_common.cpp
    src/app/replace_driver_views.cpp
    src/app/replace_ds_views.cpp
    src/app/replace_file_views.cpp
    src/app/replace_flow.cpp
    src/app/replace_midi_maps.cpp
    src/app/replace_sequence_views.cpp
    src/app/replace_stream_views.cpp
    src/app/replace_view.cpp
    src/app/replace_wave_views.cpp
    src/app/replace_wii_file_views.cpp
    src/app/replace_window.cpp
    src/app/scene_values.cpp
    src/app/sequence_view.cpp
    src/app/sound_section.cpp
    src/app/theme.cpp
    src/app/undo_history.cpp
    src/app/variables_zone.cpp
    src/app/views.cpp
    src/app/wave_sound_section.cpp
    src/app/wave_window.cpp
    src/app/widgets.cpp
    src/app/zone_parts.cpp
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
# directly. hokuto_check_gba_playback does the same for the songs of GBA ROMs, and hokuto_check_ds_playback for the
# sequences of the DS's sound archives. hokuto_check_dse_playback checks the playback of DSE's songs and sound effects
# in DS images against ntdlib's dse::Player, run directly. hokuto_check_driver_playback checks the playback of the songs
# of GBA ROMs with one of the GBA's other drivers against the driver's ArchivePlayer in ntdlib, run directly.
# hokuto_check_konami_mix checks the edits of the mixer and the inspector on a GBA ROM with Konami's driver.
# hokuto_check_konami_starts compares the order of a track's commands that the edits of Konami's driver walk with the
# read of each song of a ROM.
# hokuto_check_konami_samples checks the edits of the samples of Konami's driver on a ROM. hokuto_check_ds_layout checks
# the import and the export of the sound archives of DS images, with longer sequences, and hokuto_check_dse_layout of
# their DSE files. hokuto_check_layout checks ntdlib's cutting and joining of sound archives and its additions to their
# tables: the round trip, the tables after every sequence grows, and new files, sounds, banks, wave archives and groups.
# hokuto_check_edits edits every note command of a sound archive, and checks what ntdlib's performer plays afterwards.
# hokuto_check_gba_edits does the same for the songs of GBA ROMs. hokuto_check_gba_voices edits the voices and patterns
# of GBA ROMs as the Bank Editor does, and checks the ROM and the level of a held note. hokuto_check_gba_sources writes
# the songs of GBA ROMs as assembly, and checks the bytes that the text gives. hokuto_check_ds_banks does the same for
# the regions of the banks of DS images. hokuto_check_dse_banks does the same for the splits and waves of the banks of
# DSE's images. hokuto_check_gba_sound_fonts sends the voices of GBA ROMs out as a SoundFont and back in, and plays
# them. hokuto_check_midi_imports sends the sequence sounds of sound archives out as MIDI and SoundFont 2 files and back
# into a new file, and compares what ntdlib's models play. hokuto_check_sound_font_imports sends the banks of sound
# archives out as SoundFont 2 or SFZ files and back in, and plays them. hokuto_check_ds_replace checks Replace… on the
# sequences, the banks, the wave archives and the waves of DS images. hokuto_check_gba_midi writes a MIDI file of each
# song of GBA ROMs, imports it in the song's place, and checks what ntdlib's performer plays afterwards.
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
    # The helpers that several tools use.
    add_library(hokuto_check_support STATIC tools/check_support.cpp tools/checked_archive.cpp tools/checked_saves.cpp
                tools/notes_apart.cpp tools/check_playback_steps.cpp)
    target_link_libraries(hokuto_check_support PUBLIC hokuto_edit hokuto_format hokuto_song)
    hokuto_set_options(hokuto_check_support)

    add_executable(hokuto_check_playback tools/check_playback.cpp tools/check_playback_common.cpp
                   tools/check_playback_ctr.cpp tools/check_playback_switch.cpp tools/check_playback_wii.cpp
                   tools/check_playback_wii_u.cpp)
    target_link_libraries(hokuto_check_playback PRIVATE hokuto_check_support)
    hokuto_set_options(hokuto_check_playback)
    add_executable(hokuto_check_gba_playback tools/check_gba_playback.cpp)
    target_link_libraries(hokuto_check_gba_playback PRIVATE hokuto_check_support)
    hokuto_set_options(hokuto_check_gba_playback)
    add_executable(hokuto_check_ds_playback tools/check_ds_playback.cpp)
    target_link_libraries(hokuto_check_ds_playback PRIVATE hokuto_check_support)
    hokuto_set_options(hokuto_check_ds_playback)
    add_executable(hokuto_check_dse_playback tools/check_dse_playback.cpp)
    target_link_libraries(hokuto_check_dse_playback PRIVATE hokuto_check_support)
    hokuto_set_options(hokuto_check_dse_playback)
    add_executable(hokuto_check_ds_layout tools/check_ds_layout.cpp)
    target_link_libraries(hokuto_check_ds_layout PRIVATE hokuto_format xsflib::console)
    hokuto_set_options(hokuto_check_ds_layout)

    add_executable(hokuto_check_dse_layout tools/check_dse_layout.cpp)
    target_link_libraries(hokuto_check_dse_layout PRIVATE hokuto_format hokuto_song)
    hokuto_set_options(hokuto_check_dse_layout)
    add_executable(hokuto_check_driver_playback tools/check_driver_playback.cpp tools/check_driver_playback_commands.cpp
                   tools/check_driver_playback_reference.cpp)
    target_link_libraries(hokuto_check_driver_playback PRIVATE hokuto_check_support)
    hokuto_set_options(hokuto_check_driver_playback)
    add_executable(hokuto_check_konami_mix tools/check_konami_mix.cpp)
    target_link_libraries(hokuto_check_konami_mix PRIVATE hokuto_check_support hokuto_edit)
    hokuto_set_options(hokuto_check_konami_mix)
    add_executable(hokuto_check_konami_starts tools/check_konami_starts.cpp)
    target_link_libraries(hokuto_check_konami_starts PRIVATE hokuto_check_support hokuto_edit)
    hokuto_set_options(hokuto_check_konami_starts)
    add_executable(hokuto_check_konami_samples tools/check_konami_samples.cpp)
    target_link_libraries(hokuto_check_konami_samples PRIVATE hokuto_edit)
    hokuto_set_options(hokuto_check_konami_samples)
    add_executable(hokuto_check_gba_edits tools/check_gba_edits.cpp tools/check_gba_edits_compare.cpp)
    target_link_libraries(hokuto_check_gba_edits PRIVATE hokuto_check_support hokuto_markers)
    hokuto_set_options(hokuto_check_gba_edits)
    add_executable(hokuto_check_gba_midi tools/check_gba_midi.cpp tools/check_gba_midi_write.cpp)
    target_link_libraries(hokuto_check_gba_midi PRIVATE hokuto_check_support hokuto_edit)
    hokuto_set_options(hokuto_check_gba_midi)
    add_executable(hokuto_check_gba_sources tools/check_gba_sources.cpp)
    target_link_libraries(hokuto_check_gba_sources PRIVATE hokuto_edit)
    hokuto_set_options(hokuto_check_gba_sources)
    add_executable(hokuto_check_gba_voices tools/check_gba_voices.cpp tools/check_gba_voices_new.cpp
                   tools/check_gba_voices_rounds.cpp tools/check_gba_voices_songs.cpp)
    target_link_libraries(hokuto_check_gba_voices PRIVATE hokuto_check_support hokuto_item_views)
    hokuto_set_options(hokuto_check_gba_voices)
    add_executable(hokuto_check_gba_sound_fonts tools/check_gba_sound_fonts.cpp)
    target_link_libraries(hokuto_check_gba_sound_fonts PRIVATE hokuto_check_support hokuto_edit hokuto_song)
    hokuto_set_options(hokuto_check_gba_sound_fonts)
    add_executable(hokuto_check_midi_imports tools/check_midi_imports.cpp)
    target_link_libraries(hokuto_check_midi_imports PRIVATE hokuto_check_support hokuto_edit hokuto_song)
    hokuto_set_options(hokuto_check_midi_imports)
    add_executable(hokuto_check_sound_font_imports tools/check_sound_font_imports.cpp)
    target_link_libraries(hokuto_check_sound_font_imports PRIVATE hokuto_check_support hokuto_edit hokuto_song)
    hokuto_set_options(hokuto_check_sound_font_imports)
    add_executable(hokuto_check_ds_banks tools/check_ds_banks.cpp tools/check_ds_banks_levels.cpp
                   tools/check_ds_banks_structure.cpp tools/check_ds_banks_values.cpp)
    target_link_libraries(hokuto_check_ds_banks PRIVATE hokuto_check_support hokuto_item_views)
    hokuto_set_options(hokuto_check_ds_banks)
    add_executable(hokuto_check_dse_banks tools/check_dse_banks.cpp tools/check_dse_banks_bytes.cpp
                   tools/check_dse_banks_edits.cpp tools/check_dse_banks_renders.cpp tools/check_dse_banks_replace.cpp
                   tools/check_dse_banks_sounds.cpp)
    target_link_libraries(hokuto_check_dse_banks PRIVATE hokuto_item_views)
    hokuto_set_options(hokuto_check_dse_banks)
    add_executable(hokuto_check_ds_replace tools/check_ds_replace.cpp tools/check_ds_replace_common.cpp
                   tools/check_ds_replace_files.cpp tools/check_ds_replace_new_items.cpp
                   tools/check_ds_replace_streams.cpp tools/check_ds_replace_values.cpp
                   tools/check_ds_replace_waves.cpp)
    target_link_libraries(hokuto_check_ds_replace PRIVATE hokuto_check_support hokuto_edit)
    hokuto_set_options(hokuto_check_ds_replace)
    add_executable(hokuto_check_layout tools/check_layout.cpp tools/check_layout_common.cpp
                   tools/check_layout_files.cpp tools/check_layout_instruments.cpp tools/check_layout_new_items.cpp
                   tools/check_layout_tables.cpp tools/check_layout_wii.cpp)
    target_link_libraries(hokuto_check_layout PRIVATE hokuto_format hokuto_song)
    hokuto_set_options(hokuto_check_layout)
    add_executable(hokuto_check_edits tools/check_edits.cpp tools/check_edits_compare.cpp tools/check_edits_plans.cpp
                   tools/check_edits_sounds.cpp)
    target_link_libraries(hokuto_check_edits PRIVATE hokuto_check_support hokuto_edit)
    hokuto_set_options(hokuto_check_edits)
    add_executable(hokuto_check_mix tools/check_mix.cpp tools/check_mix_compare.cpp tools/check_mix_plans.cpp
                   tools/check_mix_sounds.cpp)
    target_link_libraries(hokuto_check_mix PRIVATE hokuto_check_support hokuto_edit)
    hokuto_set_options(hokuto_check_mix)
    add_executable(hokuto_check_exports tools/check_exports.cpp tools/check_exports_drivers.cpp tools/check_exports_ds.cpp
                   tools/check_exports_gba.cpp)
    target_link_libraries(hokuto_check_exports PRIVATE hokuto_edit hokuto_format hokuto_song)
    hokuto_set_options(hokuto_check_exports)
    add_executable(hokuto_check_items tools/check_items.cpp tools/check_items_other_console.cpp
                   tools/check_items_streams.cpp tools/check_items_wav.cpp tools/check_items_wii.cpp)
    target_link_libraries(hokuto_check_items PRIVATE hokuto_check_support)
    hokuto_set_options(hokuto_check_items)
    add_executable(hokuto_check_replace tools/check_replace.cpp tools/check_replace_common.cpp
                   tools/check_replace_files.cpp tools/check_replace_joins.cpp tools/check_replace_new_banks.cpp
                   tools/check_replace_new_sounds.cpp tools/check_replace_new_waves.cpp
                   tools/check_replace_sequences.cpp tools/check_replace_streams.cpp tools/check_replace_variables.cpp
                   tools/check_replace_wave_sounds.cpp tools/check_replace_waves.cpp tools/check_replace_wii.cpp
                   tools/check_replace_wii_donor.cpp tools/check_replace_wii_imports.cpp
                   tools/check_replace_wii_streams.cpp)
    target_link_libraries(hokuto_check_replace PRIVATE hokuto_check_support hokuto_edit)
    hokuto_set_options(hokuto_check_replace)
    add_executable(hokuto_check_sounds tools/check_sounds.cpp tools/check_sounds_wii.cpp)
    target_link_libraries(hokuto_check_sounds PRIVATE hokuto_check_support hokuto_edit)
    hokuto_set_options(hokuto_check_sounds)
    add_executable(hokuto_check_commands tools/check_commands.cpp tools/check_commands_common.cpp
                   tools/check_commands_frames.cpp tools/check_commands_moves.cpp tools/check_commands_settings.cpp
                   tools/check_commands_tempo.cpp tools/check_commands_values.cpp)
    target_link_libraries(hokuto_check_commands PRIVATE hokuto_check_support hokuto_markers)
    hokuto_set_options(hokuto_check_commands)
    add_executable(hokuto_check_tracks tools/check_tracks.cpp tools/check_tracks_add.cpp tools/check_tracks_compare.cpp
                   tools/check_tracks_paste.cpp tools/check_tracks_remove.cpp)
    target_link_libraries(hokuto_check_tracks PRIVATE hokuto_check_support hokuto_edit)
    hokuto_set_options(hokuto_check_tracks)
    add_executable(hokuto_check_instruments tools/check_instruments.cpp tools/check_instruments_commands.cpp
                   tools/check_instruments_compare.cpp)
    target_link_libraries(hokuto_check_instruments PRIVATE hokuto_check_support hokuto_edit)
    hokuto_set_options(hokuto_check_instruments)
    add_executable(hokuto_check_converter tools/check_converter.cpp tools/check_converter_edits.cpp)
    target_link_libraries(hokuto_check_converter PRIVATE hokuto_check_support hokuto_edit)
    hokuto_set_options(hokuto_check_converter)
    add_executable(hokuto_check_subroutines tools/check_subroutines.cpp)
    target_link_libraries(hokuto_check_subroutines PRIVATE hokuto_check_support hokuto_list_edits)
    hokuto_set_options(hokuto_check_subroutines)
    add_executable(hokuto_check_players tools/check_players.cpp tools/check_players_fit.cpp
                   tools/check_players_tallies.cpp)
    target_link_libraries(hokuto_check_players PRIVATE hokuto_check_support)
    hokuto_set_options(hokuto_check_players)
    add_executable(hokuto_check_model tools/check_model.cpp)
    target_link_libraries(hokuto_check_model PRIVATE hokuto_check_support)
    hokuto_set_options(hokuto_check_model)
    add_executable(hokuto_check_banks tools/check_banks.cpp tools/check_banks_common.cpp tools/check_banks_sar.cpp
                   tools/check_banks_wii.cpp)
    target_link_libraries(hokuto_check_banks PRIVATE hokuto_check_support hokuto_edit)
    hokuto_set_options(hokuto_check_banks)
    add_executable(hokuto_check_sfz tools/check_sfz.cpp)
    target_link_libraries(hokuto_check_sfz PRIVATE hokuto_format hokuto_song hokuto_synth)
    hokuto_set_options(hokuto_check_sfz)
    add_executable(hokuto_check_synth tools/check_synth.cpp)
    target_link_libraries(hokuto_check_synth PRIVATE hokuto_song hokuto_synth)
    hokuto_set_options(hokuto_check_synth)
    add_executable(hokuto_check_measure tools/check_measure.cpp)
    target_link_libraries(hokuto_check_measure PRIVATE hokuto_measure hokuto_edit)
    hokuto_set_options(hokuto_check_measure)
    add_executable(hokuto_check_other_console tools/check_other_console.cpp tools/check_other_console_choose.cpp
                   tools/check_other_console_play.cpp)
    target_link_libraries(hokuto_check_other_console PRIVATE hokuto_check_support hokuto_measure hokuto_edit)
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
    # audio firmware, in both mixing modes, dry and with the 3DS's effects. The DS's test plays a small DS archive on
    # ntdlib's models of NITRO-Composer and the DS's sound unit, in both mixing modes. The frame resampler's test brings
    # a sine between the frames of the 3DS's two modes. The biquads' test works out the passbands of the biquad filters'
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
    xsflib_add_project_test(hokuto timelines hokuto_edit)
    xsflib_add_project_test(hokuto ctr hokuto_edit)
    xsflib_add_project_test(hokuto declicker hokuto_audio)
    xsflib_add_project_test(hokuto ds hokuto_song)
    xsflib_add_project_test(hokuto dse hokuto_song)
    xsflib_add_project_test(hokuto konami hokuto_edit)
    xsflib_add_project_test(hokuto konami_edits hokuto_edit)
    xsflib_add_project_test(hokuto konami_samples hokuto_edit)
    xsflib_add_project_test(hokuto rd2 hokuto_edit)
    xsflib_add_project_test(hokuto brownie hokuto_edit)
    xsflib_add_project_test(hokuto ubimilan hokuto_edit)
    xsflib_add_project_test(hokuto rare hokuto_edit)
    xsflib_add_project_test(hokuto quintet hokuto_markers)
    xsflib_add_project_test(hokuto krawall hokuto_edit)
    xsflib_add_project_test(hokuto ds_files hokuto_edit)
    xsflib_add_project_test(hokuto ds_new_items hokuto_edit)
    xsflib_add_project_test(hokuto ds_conversion hokuto_edit)
    xsflib_add_project_test(hokuto ds_tables hokuto_edit)
    xsflib_add_project_test(hokuto gba_tables hokuto_edit)
    xsflib_add_project_test(hokuto hangs hokuto_edit)
    xsflib_add_project_test(hokuto ds_streams hokuto_edit)
    xsflib_add_project_test(hokuto edit hokuto_edit)
    xsflib_add_project_test(hokuto editor_view hokuto_editor_view)
    xsflib_add_project_test(hokuto files hokuto_edit)
    xsflib_add_project_test(hokuto wii_files hokuto_edit)
    xsflib_add_project_test(hokuto codes hokuto_format)
    xsflib_add_project_test(hokuto file hokuto_format)
    xsflib_add_project_test(hokuto file_chunks hokuto_format)
    xsflib_add_project_test(hokuto format_archive hokuto_format)
    xsflib_add_project_test(hokuto format_mix hokuto_format)
    xsflib_add_project_test(hokuto format_roms hokuto_format)
    xsflib_add_project_test(hokuto input_kinds hokuto_format)
    xsflib_add_project_test(hokuto frame_resampler hokuto_song)
    xsflib_add_project_test(hokuto played hokuto_format hokuto_song)
    xsflib_add_project_test(hokuto frame_ring hokuto_audio)
    xsflib_add_project_test(hokuto gba_assembly hokuto_edit)
    xsflib_add_project_test(hokuto gba_midi hokuto_edit)
    xsflib_add_project_test(hokuto gba_voices hokuto_edit)
    xsflib_add_project_test(hokuto groups hokuto_edit)
    xsflib_add_project_test(hokuto instruments hokuto_edit)
    xsflib_add_project_test(hokuto item_views hokuto_item_views)
    xsflib_add_project_test(hokuto gba_item_views hokuto_item_views)
    xsflib_add_project_test(hokuto ds_item_views hokuto_item_views)
    xsflib_add_project_test(hokuto dse_banks hokuto_item_views)
    xsflib_add_project_test(hokuto dse_files hokuto_edit)
    # The windows of DSE's banks and waves, drawn without a display. Their sources belong to the program.
    xsflib_add_project_test(hokuto bank_editor hokuto_item_views hokuto_key_edits hokuto_imgui)
    target_sources(hokuto_bank_editor_test PRIVATE src/app/bank_editor.cpp src/app/bank_editor_driver.cpp
                   src/app/bank_editor_ds.cpp src/app/bank_editor_dse.cpp src/app/bank_editor_gba.cpp
                   src/app/bank_editor_sar.cpp src/app/faders.cpp src/app/icons.cpp src/app/info_line.cpp
                   src/app/theme.cpp src/app/wave_window.cpp src/app/widgets.cpp)
    target_include_directories(hokuto_bank_editor_test PRIVATE src)
    # The messages that wait for a frame with no mouse button down.
    xsflib_add_project_test(hokuto message_queue)
    target_include_directories(hokuto_message_queue_test PRIVATE src)
    # The Key Editor's window, drawn without a display. Its sources belong to the program.
    xsflib_add_project_test(hokuto key_editor hokuto_item_views hokuto_key_edits hokuto_measure hokuto_imgui)
    target_sources(hokuto_key_editor_test PRIVATE src/app/arrangement.cpp src/app/arrangement_markers.cpp
                   src/app/arrangement_tempo.cpp src/app/arrangement_tracks.cpp src/app/export_warnings.cpp
                   src/app/export_window.cpp src/app/faders.cpp src/app/icons.cpp src/app/info_line.cpp
                   src/app/key_canvas.cpp src/app/key_editor.cpp src/app/key_editor_draw.cpp
                   src/app/key_editor_mouse.cpp src/app/lanes.cpp src/app/list_edits_rows.cpp
                   src/app/list_edits_text.cpp src/app/list_edits_typing.cpp src/app/markers.cpp src/app/tempo_row.cpp
                   src/app/theme.cpp src/app/views.cpp src/app/widgets.cpp src/app/zone_parts.cpp)
    target_include_directories(hokuto_key_editor_test PRIVATE src)
    # The Export window's notes, without a window. Its sources belong to the program.
    xsflib_add_project_test(hokuto export_window hokuto_item_views hokuto_measure)
    target_sources(hokuto_export_window_test PRIVATE src/app/export_warnings.cpp src/app/export_window.cpp
                   src/app/faders.cpp src/app/icons.cpp src/app/theme.cpp src/app/widgets.cpp)
    target_include_directories(hokuto_export_window_test PRIVATE src)
    # What each layer says about each engine, against one table. The names and the Export window's targets come from
    # the program's sources.
    xsflib_add_project_test(hokuto engines hokuto_item_views hokuto_measure)
    target_sources(hokuto_engines_test PRIVATE src/app/engine_names.cpp src/app/export_warnings.cpp
                   src/app/export_window.cpp src/app/faders.cpp src/app/icons.cpp src/app/theme.cpp
                   src/app/widgets.cpp)
    target_include_directories(hokuto_engines_test PRIVATE src)
    # The controls' text, measured without a display. Their sources belong to the program.
    xsflib_add_project_test(hokuto widgets hokuto_imgui hokuto_format)
    target_sources(hokuto_widgets_test PRIVATE src/app/faders.cpp src/app/icons.cpp src/app/info_line.cpp
                   src/app/theme.cpp src/app/widgets.cpp)
    target_include_directories(hokuto_widgets_test PRIVATE src)
    # The Undo History, drawn without a display. Its sources belong to the program.
    xsflib_add_project_test(hokuto undo_history hokuto_imgui hokuto_song)
    target_sources(hokuto_undo_history_test PRIVATE src/app/faders.cpp src/app/icons.cpp src/app/theme.cpp
                   src/app/undo_history.cpp src/app/widgets.cpp src/app/zone_parts.cpp)
    target_include_directories(hokuto_undo_history_test PRIVATE src)
    xsflib_add_project_test(hokuto key_edits hokuto_key_edits)
    xsflib_add_project_test(hokuto lanes hokuto_lanes)
    xsflib_add_project_test(hokuto lengths hokuto_edit)
    xsflib_add_project_test(hokuto list_edits hokuto_list_edits)
    xsflib_add_project_test(hokuto markers hokuto_markers)
    xsflib_add_project_test(hokuto measure hokuto_measure hokuto_edit)
    xsflib_add_project_test(hokuto midi_imports hokuto_edit)
    xsflib_add_project_test(hokuto midi_songs hokuto_edit)
    xsflib_add_project_test(hokuto sound_font_imports hokuto_edit)
    xsflib_add_project_test(hokuto gba_sound_fonts hokuto_edit)
    xsflib_add_project_test(hokuto sfz_imports hokuto_edit)
    xsflib_add_project_test(hokuto mix hokuto_edit)
    xsflib_add_project_test(hokuto model hokuto_edit)
    xsflib_add_project_test(hokuto new_items hokuto_edit)
    xsflib_add_project_test(hokuto notes hokuto_edit)
    xsflib_add_project_test(hokuto gba_notes hokuto_edit)
    xsflib_add_project_test(hokuto ds_notes hokuto_edit)
    xsflib_add_project_test(hokuto dse_notes hokuto_edit)
    xsflib_add_project_test(hokuto other_console hokuto_edit)
    xsflib_add_project_test(hokuto other_console_wii hokuto_edit)
    xsflib_add_project_test(hokuto other_console_ds hokuto_edit)
    xsflib_add_project_test(hokuto other_console_gba_dse hokuto_edit)
    xsflib_add_project_test(hokuto reach hokuto_edit)
    xsflib_add_project_test(hokuto recent_projects hokuto_recent_projects)
    xsflib_add_project_test(hokuto render_worker hokuto_audio)
    xsflib_add_project_test(hokuto resampler hokuto_audio)
    xsflib_add_project_test(hokuto replace hokuto_edit)
    xsflib_add_project_test(hokuto ruler hokuto_transport)
    xsflib_add_project_test(hokuto sf2_synth hokuto_synth)
    xsflib_add_project_test(hokuto sfz_synth hokuto_song hokuto_synth)
    xsflib_add_project_test(hokuto catch_up hokuto_song)
    xsflib_add_project_test(hokuto console_playback hokuto_song)
    xsflib_add_project_test(hokuto gba_song hokuto_song)
    xsflib_add_project_test(hokuto items hokuto_song)
    xsflib_add_project_test(hokuto midi_export hokuto_song)
    xsflib_add_project_test(hokuto player hokuto_song)
    xsflib_add_project_test(hokuto preview hokuto_song)
    xsflib_add_project_test(hokuto sequence hokuto_song)
    xsflib_add_project_test(hokuto sequence_model hokuto_song)
    xsflib_add_project_test(hokuto sequence_reader hokuto_song)
    xsflib_add_project_test(hokuto song_archive hokuto_song)
    xsflib_add_project_test(hokuto streams hokuto_edit)
    xsflib_add_project_test(hokuto ctr_streams hokuto_edit)
    xsflib_add_project_test(hokuto wii_streams hokuto_edit)
    xsflib_add_project_test(hokuto subroutines hokuto_edit)
    xsflib_add_project_test(hokuto tables hokuto_edit)
    xsflib_add_project_test(hokuto tempo_row hokuto_markers)
    xsflib_add_project_test(hokuto tracks hokuto_edit)
    xsflib_add_project_test(hokuto engine_tracks hokuto_edit)
    xsflib_add_project_test(hokuto transport hokuto_transport)
    xsflib_add_project_test(hokuto variables hokuto_edit)
    xsflib_add_project_test(hokuto wii_programs hokuto_edit)
endif()
