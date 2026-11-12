# Building and testing Hokuto

This page covers building Hokuto from source and running its checks. To use the program, see the
[user guide](guide.md).

## What you need

- CMake 3.24 or later.
- A C++20 compiler: Xcode or its Command Line Tools on the Mac, or Visual Studio with Clang on Windows.
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
