# SPDX-License-Identifier: MIT

# Lets SDL3's WASAPI driver keep its own buffer size under Wine. SDL3 asks WASAPI's shared mode for a period of its
# buffer size, 1,024 frames at 48 kHz, held to the longest period that the audio engine reports. Windows reports 10 ms
# and takes no longer one. Wine reports its default of 10 ms too, but grants any longer period.
#
# A Wine stream is a PulseAudio stream that a timer of that period feeds, on threads without real-time priority.
# PipeWire's PulseAudio server keeps one of its quanta and two of the stream's periods for it. In a virtual machine,
# PipeWire's quantum is 1,024 frames (21.3 ms) at least. With periods of 10 ms, the stream then holds 20 ms after each
# quantum: 64 frames less than the next quantum takes. Whenever the timer's thread, Wine's PulseAudio thread or the
# server runs a few milliseconds late, the server plays a quantum of silence in the stream's place. On a busy
# processor that happened every few seconds, and Hokuto's own drawing keeps the processor busy where Vulkan has only a
# software driver. With a period of SDL3's 1,024 frames, the stream holds two quanta after each, and the gaps go. The
# output's latency under Wine grows by about 34 ms. Without a virtual machine, with PipeWire's default quantum and a
# hardware driver for Vulkan, the stream without the change took periods of 480 frames. It still ran dry while every
# thread of the processor ran busy. With the change, it didn't.
#
# The change keeps a period above the reported longest one only under Wine. On Windows, such a period fails, and
# Microsoft's documentation warns that an audio client can refuse every initialization after one that failed. If a
# later Wine refuses the longer period, SDL3 falls back on the default period, as before the change. CMake runs the
# script in the sources' folder after it downloads them. Sources that hold the change stay as they are.
set(wasapi src/audio/wasapi/SDL_wasapi.c)
set(clamp "period_in_frames = SDL_clamp(period_in_frames, min_period_in_frames, max_period_in_frames);")
set(indent "                ")
string(CONCAT wine_clamp
       "if (!WIN_IsWine() || period_in_frames < max_period_in_frames) { // Hokuto: cmake/sdl3_wine_period.cmake\n"
       "${indent}    ${clamp}\n"
       "${indent}}")
file(READ ${wasapi} text)
string(FIND "${text}" "${wine_clamp}" changed_at)
if (NOT changed_at EQUAL -1)
    return()
endif()
string(FIND "${text}" "${clamp}" at)
if (at EQUAL -1)
    message(FATAL_ERROR "${wasapi} holds neither SDL3's limit of the shared mode's period nor Hokuto's change to it.")
endif()
string(REPLACE "${clamp}" "${wine_clamp}" text "${text}")
file(WRITE ${wasapi} "${text}")
