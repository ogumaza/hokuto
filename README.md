<h1 align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="assets/logo/logo-dark.svg">
    <img src="assets/logo/logo-light.svg" alt="Hokuto" width="440">
  </picture>
</h1>

Hokuto is a DAW for the music in Nintendo games, from the Game Boy Advance to the Switch. Open a game's sound archive
to play its songs the way the console plays them, edit them, and save them back for the game. You can also export a
song as MIDI with SoundFont 2 or SFZ, or move it to another console.

## What it opens

| Console | Sound driver | Files |
|---|---|---|
| Switch | NintendoWare (`nn::atk`) | `.bfsar`, `.fsst`, `.fspj` |
| Wii U | NintendoWare (`nw::snd`) | `.bfsar` |
| 3DS | NintendoWare (`nw::snd`) | `.bcsar` |
| Wii | NintendoWare for the Wii (NW4R SND) | `.brsar` |
| DS | NITRO-Composer | `.sdat`, or a cartridge image (`.nds`) |
| DS | Procyon Studio's Digital Sound Elements (DSE) | a cartridge image (`.nds`) |
| GBA | MusicPlayer2000, Konami's driver, Nintendo R&D2's driver, Brownie Brown's driver, Ubisoft Milan's driver, Rare's driver, Quintet's driver or Krawall | a ROM (`.gba`) |

Hokuto works with files that you've extracted from your games. It doesn't decrypt anything. Keep the extracted folders
as they are: Hokuto also reads the group files and streams next to an archive. For a Wii game, it reads `main.dol` from
the disc's `sys` folder to find which version of the sound library the game uses.

## What you can do

- **Play.** Hokuto emulates each console's sound driver and mixer. A song sounds the way it does in the game. You can
  also lift the console's limits with enhanced mixing (up to 1,024 voices at 48 kHz), or play a song on another
  console's engine.
- **Edit notes and commands.** The Key Editor edits notes and controller lanes. The List Editor edits the commands that
  a track runs, jumps, calls and comparisons included. Songs that NintendoWare's MIDI converter made open as a
  timeline. Hand-written sequences open as programs, with their labels and subroutines.
- **Mix.** Set each track's volume, pan and sends, and put the console's effects on the aux buses.
- **Edit banks, waves and streams.** The Bank Editor edits instruments and their regions. You can change the loop
  points, sample rate and encoding of waves and streams.
- **Set variables.** Games steer their music through variables. Set their starting values, change them while a song
  plays, and save sets of values as scenes.
- **Replace and import.** Replace a sequence, bank, wave or stream with another file, from the same console or another
  one. Bring in new items too, from WAV files, MIDI files, SoundFonts and SFZ files. On the GBA with
  MusicPlayer2000, a MIDI file or a song in assembly can replace a song, and a SoundFont can add voices. With Konami's
  driver, a WAV file can replace a sample.
- **Export.** Write the edited archive back for the game, a song as MIDI with SoundFont 2 or SFZ, a song for another
  console, or single items as console files or WAV files. A GBA ROM or a DS cartridge image goes back whole, and the
  export into a DSi-enhanced cart can update its hashes. Before a MIDI export or a move to another console, the Export
  window compares the result with Hokuto's playback, and lets you hear the places where they differ most.
- **Save.** Your work is a `.hok` file. The format is open and has a text form that you can edit by hand. See
  [docs/format.md](docs/format.md).

## Download

Builds for macOS (Apple Silicon, macOS 11 or later) and Windows (x64, Windows 10 or later) are on the repository's
Releases page. They aren't signed, and the first start shows a warning. On the Mac, open System Settings > Privacy &
Security and click Open Anyway. On Windows, click More info, then Run anyway.

On the N and KN editions of Windows, install Microsoft's Media Feature Pack first. Hokuto requires Media Foundation.

On Linux, build from source. A Linux build can't play, export or replace the AAC streams that some 3DS games use.

## Getting started

1. Choose File > Import > Sound Archive… and pick an archive, a GBA ROM or a DS image. You can also drop the file on
   the window.
2. Click a sequence in the browser on the right. Press Space to play and stop.
3. Double-click a part in the arrangement to open its track in the Key Editor. Press F3 for the mixer.
4. Choose File > Save As… to keep your work as a `.hok` file.
5. Choose File > Export to write the archive for the game, or the song as MIDI.

The [user guide](docs/guide.md) covers the rest, the `hok` command-line tool and the keyboard shortcuts included.

## Building from source

You need CMake 3.24 or later and a C++20 compiler. Clone the repository with `git clone --recursive`. The first
configure downloads SDL3, Dear ImGui and sfizz.

On macOS, install Xcode or its Command Line Tools, then:

```sh
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release
```

On Windows, install Visual Studio 2022 or later with the "C++ Clang tools for Windows" component. In a Developer
PowerShell:

```sh
cmake -S . -B build -A x64 -T ClangCL
cmake --build build --config Release
ctest --test-dir build -C Release
```

[docs/development.md](docs/development.md) has more on building, the tests and the check tools.

## Known limits

- The GameCube isn't supported yet.
- An imported archive plays dry. A game sets up its reverb and other effects in its code, not in the archive. Add them
  in the mixer.
- The Wii plays in strict mixing only.
- Each sound driver of the GBA and the DS stores only some kinds of edit, and Hokuto refuses the rest with a message.
  For example, Krawall's tracks take no volume, pan or sends from the mixer, and on Quintet's driver a setting can't
  move and a new one can't go inside a note. The user guide lists what each driver refuses.
- With the GBA's drivers other than MusicPlayer2000 and Konami's, the Bank Editor shows the samples without editing
  them, and Replace… takes no songs or samples. On Konami's driver, Replace… takes samples but no songs. GBA songs
  export as MIDI with SoundFont 2, and only those of Konami's driver export as SFZ too.
- File > Import adds nothing to DSE files yet, and a DSE bank can't come from a SoundFont 2 or SFZ file.
- An export into a DSi-enhanced cart can't sign it again. A DSi in DSi mode may refuse the image.
- Not every kind of edit has been tried in a game or an emulator yet. The user guide says which ones haven't.
- On Windows, restart Hokuto after you change the display scale.

## Licence

Hokuto is released under the MIT licence. See [LICENSE](LICENSE).

The program links in SDL3, under the zlib licence, Dear ImGui, under the MIT licence, and sfizz, under the BSD 2-clause
licence, with code that they include. The build takes one fix from sfizz's later sources into sfizz 1.2.3. It also
links in seqlib, xsflib and ntdlib, under the MIT licence. ntdlib includes Teakra, also under the MIT licence, and a
decoder adapted from puff, under the zlib licence.
[THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md) lists each component with its licence.
