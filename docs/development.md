# Building and testing Hokuto

This page covers building Hokuto from source and running its checks. To use the program, see the
[user guide](guide.md).

## What you need

- CMake 3.24 or later.
- A C++20 compiler: Xcode or its Command Line Tools on the Mac, Visual Studio with Clang on Windows, or Clang on
  Linux.
- The repository with its submodules. Clone it with `git clone --recursive`. If you cloned it without that, run
  `git submodule update --init` first.

The first configure downloads SDL3, Dear ImGui and sfizz.

## macOS

```sh
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release
```

The build makes `build/Hokuto.app` and the command-line tool `build/hok`. To open the program with a made-up song in
every zone:

```sh
open build/Hokuto.app --args --preview
```

## Windows

Install Visual Studio 2022 or later with the "C++ Clang tools for Windows" component, and CMake. CMake picks the newest
Visual Studio on the machine. If you have several, add the component to that one, or name the one that has it, such as
`-G "Visual Studio 17 2022"` in the first command. Without the component, CMake stops with "No CMAKE_CXX_COMPILER could
be found".

Build and test in a Developer PowerShell:

```sh
cmake -S . -B build -A x64 -T ClangCL
cmake --build build --config Release
ctest --test-dir build -C Release
```

The build makes `build\Release\hokuto.exe` and `build\Release\hok.exe`.

On Windows 11, the tests pass. The program has been tried with real input on one display, at display scales of 100 %
and 150 %, and with the Japanese input method. Nobody has tried a second monitor yet. After you change the display
scale, restart Hokuto. Until then, some text and controls keep the old size.

## Linux

Hokuto builds on Linux in two ways: as a native program, or as its Windows program to run under Wine 10 or later. The
Windows program under Wine is the supported one. Ubuntu 26.04 is the only distribution that Hokuto has been tested on,
for both the native program and the Windows program under Wine. Other distributions may work, but nobody has tested
them. The native program is a preview: it opens files and plays them, but nobody has tried the rest of it on Linux yet.

### The native program

1. Install Clang, CMake, Ninja, FFmpeg, zenity, fontconfig, FreeType and the libraries that SDL3 uses for the window
   and the sound. On Ubuntu 26.04:

   ```sh
   sudo apt install clang cmake ninja-build git pkg-config ffmpeg zenity libfontconfig-dev libfreetype-dev \
       libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxfixes-dev libxi-dev libxss-dev libxtst-dev \
       libxkbcommon-dev libwayland-dev libdecor-0-dev libegl-dev \
       libpipewire-0.3-dev libpulse-dev libasound2-dev libdbus-1-dev libudev-dev
   ```

2. Build and test:

   ```sh
   cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
   cmake --build build
   ctest --test-dir build
   ```

The build makes `build/hokuto` and `build/hok`. Run `build/hokuto`, or `build/hokuto <archive>` to open a file at once.
SDL3 opens the window on Wayland or X11. It plays the sound through PipeWire, PulseAudio or ALSA, from those that the
system has. SDL3's GPU code requires a Vulkan driver, such as Mesa's drivers or NVIDIA's. zenity draws Hokuto's message
boxes, and FFmpeg's `ffmpeg` program decodes the AAC streams. Nobody has tried GCC yet.

zenity's message box has no parent window. A click on Hokuto's window can raise Hokuto over an open box, and Hokuto
then ignores input and the close button until the box closes.

On GNOME, libdecor draws the title bar of a Wayland window. libdecor's GTK plugin draws the title bar and the window's
shadow again for each size of a resize, and the sizes can arrive faster than the plugin draws them. The build changes
SDL3's Wayland driver so that the window changes to the newest size once for each event pump
(`cmake/sdl3_libdecor_resize.cmake`). Without the change, the window froze during a resize and for seconds after it. The
change was written for SDL3 3.4.20. The configure refuses SDL3 sources of another version, and sources without the
latest revision of the change. A build folder's copy of SDL3 made before that revision is refused too. Run the script
with `cmake -P` in sources without the change. For sources with an older revision, download SDL3 again or run the script
in a fresh copy of SDL3 3.4.20's sources.

The native program draws its text with the system's fonts, as the Windows program does under Wine. fontconfig finds the
files, and FreeType draws them in grey levels, with the light hinting that GNOME's text has by default. The first of
Noto Sans, DejaVu Sans and Liberation Sans that the system has draws the text. The others that it has fill in the
characters that are missing from the first. The first of Noto Sans CJK JP, Noto Sans JP, Source Han Sans JP, IPAPGothic,
VL PGothic, TakaoPGothic and Droid Sans Fallback that the system has draws Japanese. Most desktops install some of them.
On a system with none of the three, the family that fontconfig matches to `sans-serif` draws the text. On a system with
none of the Japanese families, the family that fontconfig matches to `sans-serif:lang=ja` draws Japanese. On a system
without fonts, Dear ImGui's built-in font draws the text. Japanese and some symbols, such as the minus sign, are missing
from that font. Dear ImGui draws `?` in their place.

### The Windows program under Wine

Build the Windows program on Linux with clang-cl and lld-link, against the Windows SDK and the Visual C++ runtime that
[xwin](https://github.com/Jake-Shadle/xwin) downloads.

1. Install Clang, LLD, CMake, Ninja, Wine and FFmpeg. On Ubuntu 26.04:

   ```sh
   sudo apt install clang lld llvm cmake ninja-build wine ffmpeg
   ```

2. Install xwin 0.10.0 or later from its releases page. Then download the SDK and the runtime. `--accept-license` accepts
   Microsoft's licence terms for them:

   ```sh
   xwin --accept-license --manifest-version 18 --crt-version 14.44.17.14 --sdk-version 10.0.26100 --arch x86_64 \
       splat --use-winsysroot-style --output ~/.local/share/xwin
   ```

   Take `--manifest-version 18`. xwin's default manifest brings a C runtime of 2015, whose `fmaf` rounds twice. The Wii
   U's renders then differ from Windows' in some samples, and the toolchain file refuses that runtime.

3. Build and test. `ctest` runs each test under Wine:

   ```sh
   export WINEDLLOVERRIDES="mscoree,mshtml="
   cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=cmake/clang-cl-xwin.cmake \
       -DXWIN_DIR=$HOME/.local/share/xwin
   cmake --build build
   ctest --test-dir build
   ```

   `WINEDLLOVERRIDES` keeps Wine from offering to install Mono and Gecko when it first starts. Hokuto needs neither.
   Where several versions of LLVM are installed, `-DLLVM_BIN_DIR=/usr/lib/llvm-22/bin` picks one.

The build makes `build/hokuto.exe` and `build/hok.exe`. Run them with `wine build/hokuto.exe`.

Under Wine, Hokuto plays, exports and replaces AAC streams with FFmpeg's `ffmpeg` program, because Wine's Media
Foundation has no AAC encoder. Without FFmpeg, those streams don't play, and the AAC tests are skipped.

## Tests

`ctest` runs the unit tests and the conformance suite in [tests/conformance/](../tests/conformance/). The suite sets out
what a player of `.hok` files plays, and its README describes the files and the hashes of their renders.

## Check tools

Configure with `-DHOKUTO_TOOLS=ON` to build the check tools in `tools/`. Each one runs a part of the program over a
whole sound archive and reports what goes wrong. The comment at the top of each source file describes the tool. These
are the main ones.

| Tool | What it checks |
|---|---|
| `hokuto_check_playback` | Plays every sequence in Hokuto and on the console model run directly, and compares them |
| `hokuto_check_exports` | Exports every sequence to MIDI, SoundFont 2 and SFZ, and compares with the ntdlib converter |
| `hokuto_check_items` | Exports every item the way the browser does, and compares each file with the archive |
| `hokuto_check_replace` | Replaces every sequence, file and wave the way Replace… does |
| `hokuto_check_sounds` | Changes the values of up to 64 sounds the way the Sound section does |
| `hokuto_check_sfz` | Exports every sequence to MIDI and SFZ, renders the files with sfizz, and reports failures |

### hokuto_check_playback

```text
hokuto_check_playback [--reverb n | --effect kind] [--variables | --changes] [--enhanced] <archive> [seconds]
hokuto_check_playback --firmware <dsp firmware> [--part k/n] [--effect kind] [--variables | --changes]
                      <3DS archive> [seconds]
hokuto_check_playback --enhanced [--part k/n] [--effect kind] [--variables | --changes] <3DS archive> [seconds]
hokuto_check_playback --dol <main.dol> [--archive-build] [--part k/n] [--effect kind] [--variables | --changes]
                      <Wii archive> [seconds]
```

Each sequence plays for 30 seconds unless you give another length.

- **Switch.** Every sequence plays from the start, after seeks, and through the player. `--reverb n` adds the I3DL2
  preset n on aux bus A. `--variables` plays each sequence once for each choice of each variable that it reads. The
  choice is the starting value of the variable. `--changes` makes each choice a change during playback.
- **Wii U.** The checks are simpler: the model from the start and after seeks, and the player output. `--effect` adds
  one effect on aux bus A: `reverb_hi`, `reverb_std`, `chorus` or `delay`.
- **3DS.** `--firmware` takes a DSP firmware file, such as the `dspaudio.cdc` of a game. The reference then runs that
  firmware on Teakra, a DSP emulator, and Hokuto has to give the same samples on its model of the firmware. With
  `--enhanced` in place of `--firmware`, both sides play in enhanced mixing on the model. `--effect` adds one effect on
  aux bus A at the library defaults: `3ds_reverb`, `3ds_delay`, `3ds_nn_reverb`, `3ds_nn_delay` or `3ds_nn_reverb_2011`.
- **Wii.** `--dol` takes the `main.dol` of the game. The reference then runs the AX microcode from that file on a DSP
  emulator, with the build of the Wii sound library in the file. Hokuto has to give the same samples. With
  `--archive-build`, both sides use the build that the archive version gives instead. `--effect` adds one effect on aux
  bus A: `wii_reverb_hi`, `wii_reverb_hi_2008`, `wii_reverb_std`, `wii_reverb_std_2008`, `wii_chorus` or `wii_delay`.
- `--part k/n` checks part k of n of a 3DS or Wii archive, for runs side by side.

### The other main tools

- **hokuto_check_items** also checks the WAV files of waves and wave sounds against the console decoding, and the
  SoundFont 2 and SFZ files of each bank against the ntdlib converter. It converts each sequence file for each other
  console and back, and the players of both consoles have to reach the same commands.
- **hokuto_check_replace** puts each sequence, bank, wave archive, wave sound file and wave back in its place. It then
  replaces each with another item of the same kind. Nothing else in the archive may change. A wave also comes back from
  its WAV file twice. As 16-bit PCM it has to keep the samples, and as DSP-ADPCM the tool measures how close it comes.
  `--files` checks only the files, and `--waves` only the waves.
- **hokuto_check_sounds** checks that the archive holds each new value and keeps the rest. A sound at half volume has to
  play quieter by the same ratio.
- **hokuto_check_sfz** also reports samples that aren't finite, and gives the peak level and the time of each render.
