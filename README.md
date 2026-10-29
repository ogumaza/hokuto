<h1 align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="assets/logo/logo-dark.svg">
    <img src="assets/logo/logo-light.svg" alt="Hokuto" width="440">
  </picture>
</h1>

Hokuto is a DAW for the sequenced music of Nintendo's consoles, from the Game Boy Advance to the Switch. Version 1.0.0
covers four of them: the Wii, the 3DS, the Wii U and the Switch. Hokuto opens a game's sound archive, plays its songs as
the game does, and edits them. It writes the result back to the console's format, to MIDI with SoundFont 2 or SFZ, or to
Hokuto's open format.

Hokuto plays every sound on a model of the console's sound library and audio hardware. The models come from ntdlib, a
library that Hokuto builds on. In strict mixing, a sound plays with the console's voice limit, resampling and rounding.
On the 3DS, the model covers the DSP's audio firmware too. You don't need to supply it. Enhanced mixing lifts the
console's limits: up to 1,024 voices, mixed at 48 kHz in floating point. A sound can also play on the engine of another
of the four consoles. That console then plays the sound's conversion.

The Key Editor edits a sequence's notes and its tracks' settings. The List Editor edits any of its commands. A song of
NintendoWare's MIDI converter is edited as a timeline, like the MIDI file that it came from. A hand-written sequence is
edited as a program, with its subroutines and labels. The mixer edits each track's volume, pan and sends, and holds the
effects on the console's aux buses. The inspector sets the instrument that a track's notes play. Its Sound section edits
a sound's values in the archive's tables, such as its volume and its banks. The Bank Editor edits a bank's instruments
and their regions. The properties of a wave or a stream edit its loop, its rate and its encoding. The Variables zone
sets the values that a sequence's variables start with, and changes them at set times while it plays. A game sets them
the same way. Every edit goes into the Undo History, and playback follows it without a stop.

Replace… puts a file of the 3DS, the Wii U or the Switch in the place of a sequence, a bank, a wave archive, a wave
sound file, a wave or a stream. A file of another console goes in converted. An archive of the Wii takes the Wii's files
too. A WAV file can take the place of a wave or a stream. File > Import brings such files in as new items. File > New
Project… starts an empty project for the Switch, the Wii U or the 3DS.

File > Export writes the edited archive back for the game, a sequence as MIDI with SoundFont 2 or SFZ, or a sequence
sound into a new file of another console. Before an export to MIDI or to another console, the Export window measures how
the export sounds against Hokuto's playback. It plays the worst places of both. The browser exports single items in the
console's formats, a wave or a wave sound as a WAV file, and a bank's instruments as SoundFont 2 or SFZ.

File > Save As keeps the work in a `.hok` file, in Hokuto's open format. [docs/format.md](docs/format.md) specifies the
format. A `.hok` file has a binary form and a text form. The text form can be edited by hand. The `hok` tool converts
archives and `.hok` files from the command line (see "The hok tool"). Starting the program with `--preview` fills the
window with a made-up song that shows the look.

## Platforms

Hokuto runs on macOS 11 or later on Apple Silicon, and on Windows 10 or later, x64. Hokuto and hok require Windows'
Media Foundation to start. An N edition of Windows lacks it until Microsoft's Media Feature Pack is installed. Linux
users can build Hokuto from source. A build for Linux doesn't play, export or replace the 3DS's AAC streams: it has no
system codec of AAC. A release's builds aren't signed with a certificate. On the Mac, the first start of Hokuto shows a
warning. Open System Settings > Privacy & Security, and click Open Anyway there. On Windows, SmartScreen's warning
offers Run anyway under More info.

## Building

Hokuto builds with CMake 3.24 or later and a C++20 compiler. Clone the repository with its submodules, using
`git clone --recursive`. In a clone made without it, run `git submodule update --init` first. CMake downloads SDL3, Dear
ImGui and sfizz the first time it configures the build. `ctest` runs the tests. They include the conformance suite in
[tests/conformance/](tests/conformance/). The suite's README describes it.

### macOS

Install Xcode or its Command Line Tools, and CMake. Then build and test:

```sh
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release
```

The build makes `build/Hokuto.app`. To see it with the made-up song:

```sh
open build/Hokuto.app --args --preview
```

Configuring with `-DHOKUTO_TOOLS=ON` also builds `hokuto_check_playback`. It plays every sequence of a Switch sound
archive with Hokuto's player and with the model run directly, and reports any difference. A Wii U archive gets simpler
checks: the model from the start and after seeks, and the player's output. With `--reverb n`, both play with the
console's I3DL2 preset n on aux bus A. With `--effect kind`, a Wii U archive plays with one of the console's effects on
aux bus A: `reverb_hi`, `reverb_std`, `chorus` or `delay`. With `--variables`, each sequence plays once for each choice
of each variable that it reads. The choice is the variable's starting value. With `--changes`, the choice comes as a
change while the sequence plays. `hokuto_check_exports` converts every sequence of an archive to MIDI, SoundFont 2 and
SFZ with Hokuto's export and with ntdlib's converter run directly, and reports any file that differs.
`hokuto_check_items` exports every item of an archive as the browser does, and compares each file with the archive's
bytes. It also reads each wave's WAV file back and compares its samples and its loop with the console's decoding. It
does the same for each wave sound's WAV file, at the rate that the sound's pitch gives. It converts each bank's
instruments too, and compares the SoundFont 2 and SFZ files with those of ntdlib's converter. It also converts each
sequence file for each other console and back. The player of each console has to reach the same commands at the same
offsets. `hokuto_check_replace` replaces each sequence of an archive as Replace… does: with the sequence's file, with
the same file for each other console, and with another sound's file. The first two have to leave the archive as it was.
After the third, the sound has to play the other sound's notes, and everything else has to stay as it was. The tool then
puts the file of each bank, wave archive and wave sound back in its place, and then the next file of the same kind. The
first has to leave the archive as it was, but for copies in groups that differ from the file in more than the places of
its waves. After the second, every copy has to hold the new file, and everything else has to stay as it was. A copy that
named the old file's waves at other places has to name the new file's waves at those places. Last, each wave goes out as
a WAV file and comes back in its place, as 16-bit PCM and as DSP-ADPCM. In 16-bit PCM, the new wave has to play the old
one's samples. In DSP-ADPCM, the tool measures how closely it plays them. Each pass of its loop has to play the same
samples. The wave archive's other waves have to keep their bytes. With `--files`, the tool checks only the files, and
with `--waves` only the waves. `hokuto_check_sounds` changes each of a sound's values in the archive's tables as the
inspector does, on up to 64 sounds of an archive. The archive has to hold the new values and keep every other value. The
old values have to give it back as it was. A sound at half its volume has to play quieter by the same ratio.
`hokuto_check_sfz` converts every sequence of an archive to MIDI and SFZ as File > Export does, and renders the files
with sfizz. It reports each conversion or render that fails, and each render whose samples aren't finite. It also gives
each render's peak level and the time that it took. Each of the other tools in `tools/` checks another part of the
program on a sound archive. The comment at the top of its source file describes it.

A Wii archive's playback check takes the game's `main.dol` with `--dol <main.dol>`. The check's model then runs the
game's AX microcode from the `main.dol` on an emulator of the DSP. The model follows the build of the Wii's sound
library that the `main.dol` holds. Hokuto's playback has to give the same samples. With `--archive-build`, the check
takes the build from the archive's version. Hokuto does the same.

A 3DS archive's playback check takes a DSP firmware with `--firmware <file>`, such as a game's `dspaudio.cdc`. The
check's reference then runs the firmware on Teakra, an emulator of the DSP. Hokuto's playback runs on ntdlib's model of
the firmware, and has to give the same samples. With `--enhanced` in place of `--firmware`, the reference and Hokuto's
playback run in enhanced mixing, both on the model. With `--effect kind`, they play one of the 3DS's effects on aux bus
A, with the library's default parameters: `3ds_reverb`, `3ds_delay`, `3ds_nn_reverb`, `3ds_nn_delay` or
`3ds_nn_reverb_2011`. The reference runs the effect in the bus's aux callback, on ntdlib's model of the effect.
Hokuto's playback has to give the same samples.

### Windows

Install Visual Studio 2022 or later with its "C++ Clang tools for Windows" component, and CMake. CMake takes the
newest Visual Studio installed. Where several are installed, give that one the component too, or name the one that has
it, such as `-G "Visual Studio 17 2022"` in the first command. Without the component, CMake stops at "No
CMAKE_CXX_COMPILER could be found". Then build and test in a Developer PowerShell:

```sh
cmake -S . -B build -A x64 -T ClangCL
cmake --build build --config Release
ctest --test-dir build -C Release
```

The build makes `build\Release\hokuto.exe`. On Windows 11, the tests pass, and the program has been tried with real
input on one display, at display scales of 100 % and 150 %, and with Windows' Japanese input method. A second monitor
hasn't been tried yet. After a change of the display's scale, restart Hokuto. Until then, some of its text and controls
keep the old size.

## Using Hokuto

The main window holds Hokuto's zones. The inspector lies on the left, and the arrangement in the middle. The mixer, the
Key Editor and the List Editor share the space under the arrangement. The browser, the Variables zone and the Undo
History lie on the right. The Window menu shows and hides the zones around the arrangement, and Window > Reset Layout
puts them back where they started. The control bar at the top holds the transport. Studio > Studio Setup… sets the audio
device and plays a test tone.

Edit > Preferences… sets the tracks' colours. On the Mac, Preferences… is in the Hokuto menu. Rainbow gives each track
the next colour of the rainbow, and Standard gives every track the same grey. With Custom, click a track's colour strip,
or its colour in the inspector, to choose its colour. Automatic in the palette gives the track its rainbow colour again.
The `.hok` file keeps the custom colours.

To import a sound archive of the Switch, the Wii U, the 3DS or the Wii, choose File > Import > Sound Archive, drop the
file on the window, or give its path on the command line. Some games give their archives other names, such as `.fsst`
and `.fspj`. Hokuto reads those too. Some archives keep groups of their files outside them, in `.bfgrp` files, `.bcgrp`
files on the 3DS, or inside `.sarc` files. Hokuto reads those from the archive's folder, and from a
`userManagementFiles` folder there.

The 3DS mixes its sounds on a DSP, a separate processor that runs Nintendo's audio firmware. Hokuto plays a 3DS
archive's sequences on ntdlib's models of the 3DS's sound library and of that firmware. In strict mixing, the model
gives the samples that a 3DS game's firmware gives on Teakra, an emulator of the DSP. One build of the firmware, from
2011, gives its output a frame later, about 5 ms, with its samples inverted. Hokuto plays every game's sounds as the
other builds do. The model plays the 3DS's surround mode as stereo. It leaves out the firmware's built-in effects of the
aux buses.

On the 3DS, the output starts after three frames of silence, about 15 ms. In enhanced mixing, it starts after four,
about 20 ms. A track's mute reaches the output four frames after the change, about 20 ms. The mixer shows only FX A and
FX B, the 3DS's two aux buses. They hold the 3DS's effects. The browser plays a bank's notes, the waves, the wave sounds
and the streams of stream sounds. The editors and the mixer edit a 3DS archive's sequences. The 3DS's tracks have one
LFO, without a curve, a phase or a period. They send to FX A and FX B only. The mixer leaves the slot of FX C empty, and
the List Editor shows the kinds of commands that the 3DS skips in grey. The Sound sections edit a 3DS sound's values in
the archive's tables. The Bank Editor, the properties of a wave and the Sound sections edit the 3DS's banks, waves and
wave sounds as on the other consoles. The 3DS reads no filters of a wave sound. A 3DS wave sound sends to FX A and FX B
only. The Sound section shows the filters and the send to FX C grey. In strict mixing, the 3DS's DSP limits loud output.
A note that peaks at half of full scale or more therefore plays a little quieter than its volume gives. Replace… and
File > Import take the 3DS's items too, and the Wii U's and the Switch's items converted for the 3DS. An import also
reads the streams beside a 3DS archive. Some 3DS games keep their music in AAC files beside the archive, in place of
stream files. The browser plays such a stream with the loop that its sound gives, and exports it as its file (`.aac`),
as a WAV file, and as a stream file of the Wii U or the Switch. The system's decoder decodes it: Audio Toolbox on macOS
and Media Foundation on Windows. Replace… puts an AAC file, a WAV file or a stream file of any console in its place. An
AAC file at the stream's sample rate and with its channels goes in as it is. It keeps the present loop where the loop
ends within its samples. Any other file goes in encoded by the system's encoder, at the stream's sample rate, its
channels and the nearest of 96, 128, 160 and 192 kbit/s. On macOS, the encoder keeps that rate constant. Media
Foundation's encoder offers no such setting, and its rate is a target. A file at another sample rate is resampled first.
Where the `.hok` file lacks the stream, the new stream takes the channels that the sound gives in the archive's tables,
1 or 2, at 128 kbit/s. A mono file then fills both channels of a stereo stream. The new stream keeps the file's sample
rate if that is 44,100 or 48,000 Hz, and otherwise takes 48,000 Hz. The encoder puts a short silence before the samples:
2,112 samples on macOS. The loop moves later by the same amount. The sounds that play the stream take the new loop in
the archive's tables. The edit therefore puts the file in the model form (see "The hok tool"). A game's AAC files can
hold a checksum in each frame's header. Hokuto's encoder writes none. No 3DS has played a stream that Hokuto encoded.
The stream's properties don't take an AAC stream yet.

Hokuto plays the sequences of a Wii archive (`.brsar`) on ntdlib's model of the Wii's sound library, with a model of the
DSP's mixing in place of the DSP. The DSP's program and the sound library both come in several builds. Hokuto models the
build of the DSP's program that most Wii games hold, and an earlier build of October 2006 that a few early games hold.
An import takes the sound library's build from the game's `main.dol`, in the `sys` folder of the extracted disc that
holds the archive. Without it, the archive's version gives the build. A game of another build may then sound a little
different. Project > Project Setup shows the build. The Wii plays in strict mixing only. The editors, the mixer and the
inspector edit a Wii sequence as on the other consoles, by the rules of its build. The List Editor shows the kinds of
commands that the build lacks in grey, such as a bank select. A sound of the Wii plays one bank. A track from a sound of
another bank can't be pasted in. The mixer's FX channels hold the Wii's effects, those of the SDK's AXFX library: AXFX
Reverb Hi, AXFX Reverb Std, AXFX Chorus and AXFX Delay. From the builds of NW4R SND of 2008, the reverbs are AXFX Reverb
Hi Exp and AXFX Reverb Std Exp. A track's mute sets the track's volume to 0 in the model. A game can mute a track the
same way. A muted note can then end sooner than its release would sound. The browser lists a Wii archive's banks, wave
archives and wave sounds, and plays a note of an instrument, a wave or a wave sound. Its Export… writes each item's
file, a wave or a wave sound as a WAV file, and a bank's instruments as SoundFont 2 or SFZ. It also writes a sequence
file, a bank, a wave archive or a wave as a file of the Wii U, the Switch or the 3DS. A bank's waves go beside it, in a
wave archive file of its name. A wave sound file of the Wii goes to no other console yet. An import also reads the
streams beside a Wii archive (`.brstm`), at the paths that the archive gives them. The browser plays a stream sound as
the game's build plays it: before March 2008, the stream's first two channels, and from March 2008, the tracks that the
sound plays, with the stream file's volumes and pans. A game's code can change those volumes as the stream plays. Some
games fade layers of a stream in and out that way. Hokuto plays every layer at once. Export… writes a stream as its
file, as a WAV file, and as a file of the Wii U, the Switch or the 3DS. The properties of a Wii stream edit it as on the
other consoles. File > Export writes the edited archive. It also writes a sequence as MIDI and SoundFont, or MIDI and
SFZ. The Export window measures an export to MIDI at the Wii's rate of 32 kHz. The Sound sections edit a Wii sound's
values in the archive's tables, and an export writes them in place. A sound of the Wii has one bank slot, and Hokuto
reads no actor player there. An archive holds the pan mode and the pan curve from version 1.2 on, and the release
priority fix from version 1.3 on. The sections show a value that the archive doesn't hold grey. The Bank Editor, the
properties of a wave and the Sound sections edit a Wii archive's banks, waves and wave sounds as on the other consoles
(see below). Replace… in the browser's context menu puts a sequence file of the Wii (`.brseq`) in the place of a sound's
sequence, with the start and the bank chosen in the Replace window. It puts a bank (`.brbnk`) or a wave sound file
(`.brwsd`) of the Wii in the place of an item's file, and an RWAR (`.brwar`) in the place of the RWAR beside a bank or a
wave sound file. The new file has to keep its waves as the present one does: in a WAVE block over samples beside it, or
in an RWAR. A wave takes a wave file of the Wii (`.brwav`) or a WAV file, in DSP-ADPCM or in 16-bit PCM. Every group
that holds the file takes the new bytes. Replace… also takes the files of the 3DS, the Wii U and the Switch for a Wii
archive, and converts them for the Wii. A sequence file goes in as File > Export > Another Console… converts a sequence
for the Wii (see below). In an archive whose build of the Wii's sound library is older than March 2008, a sequence's
settings take their values at once, and the Replace window warns of it. A bank or a wave sound file of another console
takes the waves of a file of the archive in place of its wave archives. The Replace window pairs each of its wave
archives with a bank or a wave sound file of the archive, and proposes the present file. A wave archive file goes in as
an RWAR, and a wave file as a wave of the Wii. The Wii plays no IMA-ADPCM, and a wave in it is refused. File > Import >
Sequence…, Bank…, Wave Archive…, Wave Sound File…, Wave… and Audio File… add items to a Wii file too. They take the same
files as Replace… does. Project > Add Sequence Sound… and Add Bank… take a Wii file too. A bank file or a wave sound
file of the Wii comes in with its waves: the RWAR beside it under its name, or a copy of the waves of a file of the
archive. A wave archive comes in as a bank without instruments that holds its waves. On the Wii, every wave belongs to a
bank or a wave sound file. A new wave goes in after the waves of one of them. Before version 1.2 of an archive, the
Wii's sound library pans every sound of the archive in balance mode, with the square-root curve. A new sound there pans
that way too. Play in the Wii's Import window plays nothing yet. A new file joins the groups with names chosen in the
window, or else the archive's last group. Hokuto adds no group to a Wii archive. Export… writes a bank's or a wave sound
file's waves as an RWAR, and each of them as a wave file of the Wii (`.brwav`). Replace… and File > Import > Stream…
take the Wii's streams too (see below). File > Export > Another Console… writes a Wii sound into a new file of the Wii
U, the Switch or the 3DS, and the engine switch plays it on those consoles' engines (see below). `hok lengths` measures
how long a player plays each Wii sequence sound, on the Wii's model with the file's build (see "The hok tool").

File > New Project… starts an empty file for the Switch, the Wii U or the 3DS. Project > Add Sequence Sound… adds a
sound with an empty song. Draw its notes in the Key Editor. A note sounds only once a slot of the sound holds a bank
with an instrument for the note's program. Banks and wave archives come in through File > Import. Project > Add Bank…
adds an empty bank and opens it in the Bank Editor. WAV files dropped on the editor make its instruments (see below).
File > Export > Sound Archive then writes a whole archive of the console.

File > Save As saves the archive as a `.hok` file. File > Open opens a `.hok` file in either of its forms, binary or
text. Dropping the file on the window or giving its path on the command line opens it too. On the Mac, so does a
double-click on the file in Finder. File > Recent Projects lists the 10 `.hok` files opened or saved last, newest first,
by name. Choose one to open it as File > Open does. A file that can't be opened leaves the list, with a message. Hokuto
keeps the list between runs. A large file or archive can take a few seconds to open. A window says so meanwhile. Its
Cancel button keeps the file that's open. File > Save saves to the file that's open, in the form that it was opened in.
While the file has changes, Hokuto asks before it quits, or before another file takes its place. Save saves the changes
first, and Don't Save drops them. A `.hok` file holds the group files that Hokuto found beside the archive. Their sounds
play from the saved file too. A `.hok` file also keeps the view of the sound that Hokuto shows: the position and the
cycle, the zoom and the scroll, the selected, muted and soloed tracks, the Key Editor's controller lanes and the open
editors. The file opens with that sound as it was saved. A change of the view alone doesn't count as a change of the
file: Hokuto doesn't ask to save it.

File > Export > Sound Archive writes the archive with the file's edits, for the game to load in place of the original.
Each group file that the edits change goes beside the archive, under its group's name and `.bfgrp`, or `.bcgrp` on the
3DS. When the game keeps the group's file in the `userManagementFiles` folder there, the new group file goes into that
folder. When a SARC archive beside the archive, or beside the file that Hokuto opened, holds the group's file, the new
group file goes into a copy of that SARC archive instead. The copy goes beside the archive under the SARC archive's
name. File > Export > MIDI and SoundFont writes the shown sequence as a MIDI file, with a SoundFont 2 file of its
instruments beside it. MIDI and SFZ writes SFZ files instead: one for each track in `sfz/<name>` beside the MIDI file,
with their samples in `sfz/samples`. ntdlib's converter writes the files (see "The MIDI files" below). A sound's title
tag names the MIDI file's sequence in place of the sound's name. The sound's copyright tag, or else the file's, goes in
as the MIDI file's copyright notice. The files follow the values that the variables start with, and their changes at set
times. Before it writes anything, the Export window lists what the export can't keep, such as the effects and the levels
of the FX channels. For an archive, it says how much the archive and the files of each group grow from the game's
archive. The file keeps the game's sizes from the import. A game loads a group's files into a part of its memory whose
size no file gives. Only the game, or an emulator, shows whether a grown archive still fits.

For MIDI with SoundFont 2 or SFZ, the Export window then measures the export. Hokuto's SoundFont 2 renderer, or sfizz
for SFZ, plays the MIDI file. The console's model plays the sound as Hokuto does. Neither plays the effects on the aux
buses: MIDI can't hold them. The window compares the two renders in windows of 50 ms. A second that lies more than 1 dB
off may be audible, and one more than 3 dB off is audible. So is a stretch of 200 ms whose windows each lie more than 6
dB off. The window names the three worst places. Play Export and Play Hokuto play each place, from a second before it to
a second after it. The measure leaves out the overall difference in level: a listener sets the volume. It covers the
first 5 minutes at most. A sound that draws random numbers plays other notes in each render, and the window says so.
Other players can play the files otherwise. Hokuto's renderer follows the SoundFont 2.04 specification where the common
players do, and takes their conventions where they depart from it.

File > Export > Another Console… writes the shown sequence sound into a new `.hok` file for another console: the 3DS,
the Wii U, the Switch or the Wii. The Export window's Target menu sets the console. The new file holds the sound, its
sequence file, the banks in its slots and the wave archives that their waves name, converted for that console. It keeps
the sound's values in the archive's tables, its tags, scenes and track colours, and the effects that the other console
runs. The Export window lists what changes on the way. It then measures the new sound on the other console's model
against the sound as Hokuto plays it, both without effects, and plays the worst places.

A sound of the Wii goes to the Wii U first, and from there to the Switch or the 3DS. Its bank goes in as a bank of the
Wii U, after a wave archive of its waves. The commands that the build of the Wii's sound library skips stay out. The
Wii's sound library sets a track's second volume and main volume at once, and the new file's commands do the same. A
low-pass filter takes the value whose cutoff on the later consoles lies nearest. The Wii starts the global variables at
0, and the later consoles start them at -1. When the sound reads a global variable before it sets it, commands at its
start set the variable to 0 while it holds -1. The Wii plays a sound's volume above 127 at 127, and the new sound's
volume goes in at 127 at most. A sound of the build of October 2006 takes the pan mode and the pan curve that the build
pans by. The remote filter, the user parameters and the Wii's effects stay behind. The measure plays the sound on the
Wii's model at 32 kHz.

A sound of the 3DS, the Wii U or the Switch goes to the Wii for the build of the Wii's sound library of June 2008. A
sound of the Wii plays one bank, and the Wii's sound library has no bank select. The banks of the slots that the sound's
notes play go in as one bank of the Wii, with their waves. Each slot's instruments take a range of that bank's programs,
one slot after another. The program changes select the programs of their slots' ranges. A bank select becomes a program
change into its slot's range where the track's program is known there. A bank in a slot that no note plays stays behind.
A note may play another instrument on the Wii where a variable or a random number gives its bank slot, where a variable
gives its program, or where tracks of two slots run its program change. A warning counts such notes. Hokuto's model of
the Wii keeps a bank's waves in the Wii's 64 MB of MEM2. When they don't fit, the bank takes only the instruments that
the sound selects. A wave that the Wii doesn't play, such as a wave in IMA-ADPCM, stays behind. Its regions play
nothing. The commands that the Wii's player skips stay out. The Wii's end after a comparison ends the track whatever the
comparison gave. The new file's sequence jumps to an end there instead. The Wii's sound library sets a track's second
volume, main volume and pitch bend at once. The new file's commands do the same. A low-pass filter takes the value whose
cutoff on the Wii lies nearest. When the sound reads a global variable before it sets it, commands at its start set the
variable to -1 while it holds 0. The sound's volume goes in at 127 at most. The 3D settings, single play and the user
parameters stay behind. Of the effects, only those of the Wii's AXFX library go along. A sequence whose commands have to
keep their places doesn't go to the Wii yet. The measure plays the new file on the Wii's model at 32 kHz.

The 3DS's player skips some commands of the other consoles: the curve, the phase and the period of the first LFO, the
other LFOs and the send to FX C. A song for the 3DS or from the 3DS leaves those commands out. In a sequence whose
commands have to keep their places, those commands stay. A warning says so. Such a sequence can't go when the other
console reads one of its commands with another length. The 3DS's sound library sets a track's second volume and main
volume at once. A song from the 3DS takes those settings without their times, and the other console then sets them at
once too. In a sequence whose commands have to keep their places, the times stay, and the other console moves the values
over them. A song for the 3DS loses the ramps of those settings. The 3DS reads the settings of the other LFOs without
their values. A song for the 3DS keeps the effects that the 3DS runs. The other effects stay behind. In strict mixing,
the 3DS mixes at 32,728 Hz. The measure compares the two renders at the higher rate. Hokuto's model of the 3DS keeps the
wave archives that a sound plays in 64 MB of memory. A game keeps them in its sound heap the same way. When a song's
wave archives don't fit there, its banks go to the 3DS with only the instruments of program 0 and of the programs that
the song's program changes select. The wave archives then hold only those instruments' waves. A song whose programs come
from a variable or a random number can't go then. Neither can a song whose waves still don't fit. The 3DS plays 23 notes
at once, and 24 channels of their waves. A stereo wave takes two channels. A song that holds more notes or channels at
once gets a warning. The 3DS then drops the notes of the lowest priority. It drops them the same way in a game. A song's
stereo waves can go to the 3DS as mono waves instead. Each mono wave holds the mean of the two channels. A mono wave
takes one channel. The Export window's check box Stereo waves as mono sets this. Hokuto turns it on when the song holds
more than 24 channels at once. A mono wave and the regions that play it play louder. The wave then keeps the power of
its two channels at the pans of its notes. A region's volume stops at 255, the highest.

Every console takes the coefficients of a biquad filter from the same presets: a low-pass, a high-pass and band-passes
around 512, 1024 and 2048 Hz, as types 1 to 5. The presets are made for 32 kHz. The Wii and the Wii U mix at 32 kHz,
and the 3DS at 32,728 Hz. The Switch mixes at 48 kHz, and the same coefficients filter 1.5 times higher there. The
Switch also has types 6 to 10: the same filters, made for 48 kHz. A song for the Switch takes types 6 to 10 in the place
of types 1 to 5, and keeps their values. A song from the Switch takes types 1 to 5 in the place of types 6 to 10. Its
filters of types 1 to 5 take the values whose passbands on the other console lie nearest. A low-pass keeps its cutoff
within 0.04 octaves that way, and a high-pass within 0.06 octaves up to about 6.5 kHz. No value moves the band of a
band-pass: the band lies 0.58 octaves lower on the Wii and the Wii U, and 0.55 octaves lower on the 3DS. A filter whose
type or value a variable gives stays as it is. So does a value that plays with more than one type. A warning counts the
changes.

The browser lists the archive's items in groups. Sounds holds the wave sounds and the stream sounds, Sequences the
sequence sounds, Banks the banks, and Waves the wave archives. Wave Sound Files lists each wave sound file with its wave
sounds, under the name of the sound group that holds them. Click a sequence to see its tracks and their parts in the
arrangement, and each track's volume, pan and sends in the mixer. Space plays it. A click in the lower half of the ruler
moves the playback position, and playback starts at once from anywhere in a song.

Each call of a subroutine shows as a separate part, named after the subroutine. An equals sign at the end of the name
marks a linked part: other calls play the same subroutine, in this sequence or in other sounds. An edit of a linked
part's notes changes what every call plays.

In the browser, a bank opens to list its instruments, and a wave archive to list its waves. Click an instrument to hear
a note of it as the console plays it: middle C, or the nearest key with a sound, held for a second. Click a wave to hear
it as the archive holds it. Click a wave sound among the other sounds to hear its wave at the sound's pitch. A second
click stops it. A wave sound plays a wave by itself, with a pitch, a pan and an envelope. A file of wave sounds holds
many of them.

Double-click a bank or one of its instruments, or choose Open Bank Editor in its context menu, to open the bank in the
Bank Editor. The list on the left shows the bank's instruments by program. Click an instrument to select it. Add puts an
empty instrument at the first free program after the selected one, Duplicate copies the selected instrument there, and
Delete removes it. Type a program in the Program field and press Enter to move the instrument there. The map shows the
selected instrument's regions, with the keys across and the velocities up. Click a region to select it. Drag it to move
it, or drag one of its edges to change its keys or its velocities. The root key moves with the region. Delete or
Backspace deletes the selected region.

The fields under the map show every value of the selected region: its keys and velocities, its wave, its root key and
tuning, its volume, pan and key group, its envelope, its interpolation and its note-off. A region plays its wave at the
wave's rate at the root key. Drag a slider to change a value, and Ctrl-click it to set NintendoWare's default. The
Switch and the Wii don't read the interpolation. Its field shows grey there. The Wii's sound library reads the hold,
the key group and the note-off only in its builds from March 2008 on. In a file of an older build, their fields show
grey too. The keyboard between the map and the fields plays the selected instrument on the console's model. Click a key
to hear it, or drag along the keys. A click near the bottom of a key plays it louder. The keys that the selected region
covers are tinted, and a dot marks its root key.

To make regions, drag waves from a wave archive's list in the browser onto the map. WAV files and wave files (`.bfwav`)
dropped from Finder or File Explorer work the same way. One wave dropped on a region gives the region that wave. Other
drops make a region of one key for each wave, from the key under the drop upward, over every velocity. Each such region
plays its wave at its key. A drum kit is such a map. Waves dropped on the list of programs make an instrument each, at
the first free programs. Each such instrument holds one region over every key and velocity, with its root key at C3. A
WAV file comes in as DSP-ADPCM. A wave file keeps its encoding. The new waves go after the waves of the wave archive
that the bank names, or into a new wave archive. A bank of the Wii keeps its waves in its wave data. The browser lists
the wave data as a wave archive under the bank's name. New waves go after the bank's waves there. A region that takes
a wave of another bank or of a wave sound file takes a copy of the wave in the bank's wave data. The Bank Editor
doesn't open a bank of the Wii whose regions hold a surround pan or other values that it doesn't show.

Each edit of a bank goes into the Undo History. The lines at the bottom of the editor name the sequence sounds that play
the bank. An edit changes them all. The lines also name each group that loads the bank but lacks a wave archive that its
waves name. A game that loads such a group plays those waves only once another group has loaded them. The last lines
give the size of the bank's file, and what each group that loads the bank loads, beside the game's group. A bank that
nothing edited keeps the game's bytes.

Double-click a wave in a wave archive's list, or choose Wave Properties in its context menu, to open its properties.
Double-click a stream sound, or choose Stream Properties, for the properties of its stream. The window doesn't edit the
samples. It shows the waveform with the loop. The samples after the end lie in shadow. Drag the S or the E marker to
move the loop's start or its end. While the magnet button is on, a dragged marker snaps to the place nearby where the
channels lie nearest zero. Ctrl with the mouse wheel zooms around the pointer, and the wheel scrolls. Double-click the
waveform to show the whole wave. Play plays the wave from its start, then round its loop until you click Stop. Replace…
and Export… open the browser's windows for the item. They take and write WAV files too.

The fields under the waveform edit the loop, the rate and the encoding. Loop turns the loop on or off. Click the start
or the end, type a sample, and press Enter. A loop in DSP-ADPCM starts at a frame of 14 samples, and a stream's loop at
a block of the stream. A start between them moves to the nearest place that the encoding allows. On the Wii, a game's
loop in DSP-ADPCM can start between frames. It keeps its start until you move it. The end can move back
over samples that an earlier edit left out. The rate gives the samples per second that the console plays. A new rate
changes the pitch and the length, and the samples stay. The encoding offers DSP-ADPCM, PCM16 and PCM8 for a wave, and
DSP-ADPCM and PCM16 for a stream. A new encoding makes the wave again from its samples. DSP-ADPCM takes 8 bytes for each
14 samples of a channel, and adds some noise. A change of the loop or the rate keeps the samples that the game stores. A
new end of a stream makes the stream again from its samples. So does a loop from the first block of a 3DS stream: the
3DS's tools never start a loop there. A stream's properties also list its tracks: each track's channels, volume and pan,
and whether it plays when the sound starts. They don't edit the tracks yet. The lines at the bottom name what else plays
the wave or the stream, give its size, and give what each group that loads it loads.

Right-click an item in the browser and choose Export… to write it as a file of the console's format: a sequence, a wave
sound, a bank, a wave archive or a single wave. An item that nothing edited comes out byte for byte. A sequence goes out
with the file's edits. The Target menu also offers a sequence as MIDI with SoundFont 2 or SFZ. File > Export writes the
shown sequence in these forms too. The Export window's Target menu also offers the item's file for each other console
among the 3DS, the Wii U and the Switch, and for the Wii (see below). The three consoles' files hold the same
structures. Their sequences run the same commands. The 3DS lacks the extended LFO commands and the third send. A
sequence that runs one of those doesn't go to the 3DS. As in File > Export > Another Console… (see above), a sequence's
biquad filters take the types and values of the other console. The Wii U stores the numbers in its files big-endian. The
3DS and the Switch store them little-endian. The arguments of a 3DS command are big-endian all the same. So are those of
a Wii U command. The new file takes the oldest version found in the target's games. A sequence file takes 1.0.0.0 on the
3DS, 1.0.0 on the Wii U and 2.0.0 on the Switch. An older game may refuse a newer version. The Target menu offers a wave
as a WAV file too, and a wave archive as a folder of WAV files, one for each wave. A WAV file holds 16-bit PCM, with the
wave's loop in a `smpl` chunk. Waves have no names. Each WAV file takes the wave archive's name and the wave's index,
such as `WARC_SE_007.wav`. A wave sound goes out as a WAV file of its wave too, under the sound's name. The file takes
the rate at which the sound plays the wave: the wave's rate times the sound's pitch. It plays at the same pitch as the
game. The Export window lists what the WAV file leaves out: a pan away from the centre, and an envelope that fades the
sound in or down. The consoles play only a wave sound's first note. A sequence file can hold the sequences of several
sounds, and a wave sound file the wave sounds of several. The Export window then lists the other sounds. A stream
sound's stream goes out as its file (`.brstm` on the Wii, `.bcstm` or `.aac` on the 3DS, `.bfstm` on the Wii U and the
Switch), as a file of each other console, or as a WAV file.

An item of the 3DS, the Wii U or the Switch goes out for the Wii too. A sequence file goes out as a `.brseq` file for
the build of the Wii's sound library that a new file of the Wii plays. The commands that the build skips stay out, and
the Export window lists them. A sequence whose commands have to keep their places doesn't go. A bank or a wave sound
file goes out with its waves in a `.brwar` file beside it. The Wii keeps them that way. File > Import on the Wii reads
them there. A wave goes out as a `.brwav` file, a wave archive as a `.brwar` file and a stream as a `.brstm` file. The
Wii plays the first two channels of a wave, and no wave in IMA-ADPCM. A wave that the Wii doesn't play stays out of a
bank's waves, and its regions play nothing. The Export window counts such waves.

For a bank, the Target menu also offers its instruments as a SoundFont 2 file, or as SFZ files. Each instrument becomes
the preset of its program. Programs from 128 on go into the next banks of presets: program 130 becomes bank 1, preset 2.
An instrument plays as a track plays it before the sequence changes anything: with the bank's envelopes, at the centre
and without a filter. The modulation wheel (CC1) sets the depth of its vibrato. Each SFZ file holds one instrument and
takes the bank's name and the program, such as `BANK_GM_005.sfz`. The files go into a folder with the bank's name,
inside the folder that you choose. Their samples go into the folder `samples` beside it. A sequence's SFZ export keeps
its samples in the same way, in `sfz/samples` beside the MIDI file. Choose that `sfz` folder for a bank's SFZ files, and
they share the samples. A SoundFont 2 file holds at most 65,535 zones, generators and modulators. The instruments of a
larger bank go out as several SoundFonts, each with a run of programs. Each file's name ends in its programs, such as
`BANK_GM_000-048.sf2`. The Export window lists the files first. Open a bank in the browser to list its instruments. An
instrument's Export… writes it by itself, as a SoundFont 2 file or an SFZ file, at its program. Its files take the
bank's name and the program, such as `BANK_GM_005.sf2`.

Choose Replace… in a sequence's context menu to put a sequence file (`.bcseq` or `.bfseq`) of the 3DS, the Wii U or the
Switch in its place. A file of another console goes in converted. As in File > Export > Another Console… (see above),
its biquad filters take the types and values of the archive's console. The Replace window offers the file's labels as
starts. For a file with the commands of one of the archive's sequences, such as one that Export… wrote, it also offers
the places where that sequence's sounds start. The menu first shows the start of the sound named after the file. A menu
for each of the sound's bank slots sets its bank. The window lists each program that the new commands select, with the
bank slot that it plays from. Each program can take another program of the sound's bank in that slot. Hokuto then
rewrites the program changes. The warnings say what else changes. A sound whose entry gives no start begins at the
file's first command. A jump to the new commands then goes there, and the other sounds that start there play them too.
When other sounds play the same file, the new commands go after the file's commands, and the other sounds keep their
commands. The sound also allocates the tracks that the new commands open. A game can leave a track out of a sound on
purpose, to keep the track silent. The window then offers to allocate it too. Edit > Undo takes the replacement back. A
3DS archive refuses the commands that the 3DS lacks. The window also lists the variables that the new commands read
before they set them. Each one keeps its number, moves to another number, or takes a fixed value. A command at the
sound's start then sets the fixed value. Play plays the sound with the new commands, from its start, for 30 s at most.

Banks, wave archives and wave sounds have Replace… in their context menus too. A bank takes a bank file (`.bfbnk`, or
`.bcbnk` on the 3DS), and a wave archive takes a wave archive file (`.bfwar` or `.bcwar`). A wave sound takes a wave
sound file (`.bfwsd` or `.bcwsd`). That file can hold the wave sounds of several sounds, and each of them then plays the
wave sound at its place in the new file. A file of another console goes in converted. The new file takes the place of
every copy of the old one: in the archive, in its groups and in the group files beside it. The window of a bank lists
each program that the bank's sounds select and the new bank lacks. That program's notes stay silent, or the present
bank's instrument or one of the new bank's goes into the new bank at the program. A group's copy of a bank or a wave
sound file can name the old file's waves at other places, such as in other wave archives. That copy takes the new file
with its places for those waves. The Replace window warns about what else changes: other items with the same file,
copies in groups that name the waves at other places or held other bytes, waves that the archive lacks, programs that a
new bank lacks, notes that play past the end of a new wave archive, and a version other than the old file's.

A wave takes a WAV file: open its wave archive in the browser and choose Replace… in the wave's context menu. It also
takes a wave file (`.bcwav` or `.bfwav`) of the 3DS, the Wii U or the Switch. A wave file of another console goes in
converted. Hokuto reads WAV files of PCM up to 32 bits, or of 32-bit or 64-bit floats, with one or two channels. The
wave takes the WAV file's samples, its rate and the first loop of its `smpl` chunk. The Sample rate menu offers lower
rates too: the usual rates down to 8,000 Hz, and the present wave's rate. Hokuto's resampler then brings the samples to
the chosen rate. A WAV file without a loop can take the present wave's loop from the Loop menu. The Replace window's
Encoding menu sets how the wave holds the samples: DSP-ADPCM, the consoles' usual encoding, or 16-bit PCM. A wave of
16-bit PCM takes about 3.5 times the space of the same wave in DSP-ADPCM. DSP-ADPCM loses some detail. The window gives
the new wave's signal-to-noise ratio. A loop in DSP-ADPCM starts at a frame of 14 samples. A loop's start between frames
moves forward to the next frame. The samples between repeat after the loop's end. The loop therefore plays the same
samples. A wave plays up to the end of its loop. The samples after the loop's end stay out. The wave archive's other
waves keep their bytes. The Replace window lists the banks and wave sounds that play the wave. It warns about what
changes: the rate, the channels, the loop and the size. A game sets memory aside for its wave archives, and a larger
wave archive may not fit. The window also lists what the WAV file holds and the wave leaves out, such as a second loop
or samples of more than 16 bits. Its Play button plays the new wave before it goes in.

A stream sound takes a stream file of the 3DS (`.bcstm`), the Wii U or the Switch (`.bfstm`), or a WAV file, through
Replace… in its context menu. A file of another console goes in converted. In a 3DS archive of a version before 2.3.0.0,
a stream sound plays the tracks of its stream file. A new stream there takes the present stream's tracks when its
channels allow, or a track for each pair of channels. File > Import > Stream… brings a stream file or a WAV file in as a
new stream sound. In a 3DS archive before version 2.3.0.0, a new stream without tracks takes a track for each pair of
its channels. A 3DS stream sound plays 4 tracks of 2 channels at most, and 8 channels in all. Hokuto adds no stream
sound to a 3DS archive of version 2.1.0.0 or 2.3.1.0, and no sound to an archive without names. File > Export > Sound
Archive writes each new or changed stream beside the archive.

A stream sound of the Wii also takes a stream file of the Wii (`.brstm`). On the Wii, a stream without tracks, such as
one of a WAV file, takes the present stream's tracks when its channels allow, or a track for each pair of channels. The
Replace window lists the tracks that the sound plays, and the channels that its entry keeps. File > Import > Stream…
takes the same files. A stream without tracks then takes a track for each pair of its channels. A Wii stream sound plays
8 tracks and 16 channels at most. The import refuses a stream with more. The new stream goes into a new file in the
`stream` folder beside the archive. The file joins no group: a game plays a stream from the disc. A Wii game sets room
aside for the streams that play at once. Hokuto doesn't check a new stream against that room yet. Hokuto adds no stream
sound to a Wii archive that NintendoWare's tools didn't lay out. File > Export > Sound Archive also writes the size of
each new or changed stream into the archive's file table.

File > Import also brings single items in as new ones. File > Import > Sequence… takes a sequence file and adds a new
sound that plays it. The new sound copies the settings of a sequence sound of the archive: its player, volume, pan,
priorities, 3D settings and bank slots. The Import window's Settings from menu selects that sound. As in the Replace
window, the window offers the file's starts, sets the bank in each slot and maps the programs. File > Import > Bank… and
Wave Archive… add a bank file (`.bfbnk`) or a wave archive file (`.bfwar`) as a new bank or wave archive. A bank names
its waves by their wave archive and their index. The Import window pairs each wave archive that the bank's waves name
with a wave archive of the archive. Bring those wave archives in first. File > Import > Wave Sound File… adds a wave
sound file (`.bfwsd`) as a new file. Each wave sound that it holds becomes a new sound. The new sounds copy the settings
of a wave sound or a sequence sound of the archive. The window pairs the file's wave archives as it does for a bank.
File > Import > Wave… adds a wave file (`.bfwav`) as a new wave after the waves of a wave archive. File > Import > Audio
File… does the same with a WAV file. It reads the WAV file as Replace… of a wave does. The Import window sets the wave
archive, and a WAV file's encoding. Its Play button plays the new wave. Banks and wave sounds name a wave by its index
in its wave archive. No bank or wave sound of the archive plays the new wave. A bank or a wave sound file that comes in
later can name it.

Each import but Audio File… takes a file of the 3DS, the Wii U or the Switch. A file of another console goes in
converted. The window names a new sound, bank or wave archive after the file. With several wave sounds, each new sound
takes the name and its wave sound's index: `SE_HIT_00` to `SE_HIT_11` for 12 wave sounds. A new item goes at the end of
its table, and its file at the end of the archive. A new wave goes after its wave archive's waves, in every copy of the
wave archive. A game loads most of its files in groups. The Groups menu of the Replace and Import windows lists the
archive's groups. The menu proposes the groups that load the replaced item, or the sound whose settings a new item
copies. The item's files join the groups that you check there. A name typed in the menu's field makes a new group. A
game's code calls its sounds by their numbers or names. The code loads groups by their numbers. The game plays a new
sound only once its code calls the sound. A new group loads only once the code asks for its number. Edit > Undo takes an
import back. On a large archive, an import of a sequence, a bank, a wave archive or a wave sound file can take a few
seconds once you choose Import. The window says so meanwhile, and Cancel stops the import.

Cycle starts off, and a song plays through once. The locators go round a sequence's loop, or from the start to the
end of its last note. With Cycle on, playback goes round between the locators. Locators on a sequence's loop go round
it as the game does. Other locators send playback back to the left locator once it reaches the right one. Past the
end of a sequence, the last notes' releases and then silence fill the cycle. Playback that starts after the right
locator plays on.

The ruler sets the locators. A drag in its upper half draws a new range and turns Cycle on. A drag on either end of the
range moves that locator, and a click on the range turns Cycle on or off. The locators snap to beats, and to the start
and the end of the sequence's loop. The marker row under the ruler shows a looping sequence's intro and its loop.
Double-click either one to set the locators round it. Alt-double-click it to zoom to it. Click MARKERS at the row's left
end for a menu of the markers. Its Locate menu moves the position to a marker. Its Cycle menu sets the locators round
the intro or the loop, and its Zoom menu zooms to either.

The marker row also shows where the tracks read variables or branch. A command reads a variable when it compares the
variable, works out the variable's new value from its value, or takes a value from it. A command after "if" runs only
if the last comparison held. A blue flag marks each such place. Its label names the variables, such as Var 3, 14, with
"if" before them at a branch. The commands that run at one tick make a set. Where the tracks run the same set again,
such as in a loop that waits for a variable, a thin line marks each later place. The tooltip of a flag or a line lists
its commands. Double-click it to open them in the List Editor, on the track of the first one.

Project > Project Setup (Shift+S) shows the file's engine and mixing mode, and the file's tags. Its boxes work like
those of the Sound section (see below).

Each sound plays in one of two mixing modes. Strict mixing plays it as the console does, with the console's voice limit,
resampling and rounding. A game plays the sound this way. Enhanced mixing lifts those limits: up to 1,024 voices, mixed
at 48 kHz in floating point and resampled with a long windowed-sinc filter. The notes, the instruments and the effects
play as on the console. The switch in the mixer's header sets the shown sound's mode, and Project Setup the file's. A
sound plays, previews and auditions in its mode, and the file keeps it.

A sound plays on the engine of its file's console, or on the engine of another console among the 3DS, the Wii U, the
Switch and the Wii. The engine switch in the mixer's header sets the shown sound's engine, and the file keeps it. On
another console's engine, Hokuto plays the sound's conversion for that console. File > Export > Another Console… writes
the same conversion (see below). The tooltip of the sound's engine lists what changed in the conversion. The List Editor
shows grey the commands that the engine's player skips, such as the 3DS's other LFOs, and the commands that the
conversion leaves out because the file's console skips them. The arrangement and the editors show the file's sound, and
an edit changes the file's sound. Playback follows an edit without a stop. After a change of the engine, the old engine
goes on playing until the new one is ready. Playback then moves to the new engine at the same place. An export to MIDI
converts the conversion that Hokuto plays. The archive keeps the sound for the file's console. On the Wii's engine, a
sound plays in strict mixing.

Project > Tempo Track shows the tempo row under the marker row, and hides it again. Its line follows the tempo as the
sequence plays it, against the scale at the row's left end. A point marks each command that sets the tempo, at the tick
where its track ran it. A tempo from a random number or a variable has a bar across the row instead, with the source
beside it. Drag a point up or down to change its tempo, in whole beats a minute. Hold Shift to drag more finely. Drag a
point to the side to move its command to another beat, or to another tick with Shift. Like a point in the Key Editor's
lanes, it moves only as far as the commands around it allow. A command that runs at several places, such as in a
subroutine, changes at all of them. Other sounds can run the command too, and the point's tooltip names them. A new
tempo can change what a sound that draws random numbers plays. Double-click a point to open its command in the List
Editor.

The mixer shows each track's volume, pan and sends as the sequence sets them at the playback position. The three slots
above the pan knob are the sends to FX A, FX B and FX C, with their levels. Drag a fader, a pan knob or a send to change
it. Dragging a send right raises it. Hold Shift to move a control finely. A change writes the commands of the sequence
that set the track's value. Where the track sets one value throughout, every command takes the new value. Where the
value changes as the track plays, such as in a fade, every value moves with the change: a level by the same number of
decibels, and a pan by the same distance. A track that plays notes before its first command gets a new command at its
start. Where a subroutine sets the value for several tracks or sounds, the change reaches all of them. An equals sign at
the end of the track's name, at the bottom of its channel strip, marks such a track. The tooltip of each control names
the other tracks and sounds that a change of it reaches, and the sign's tooltip lists them all. Hokuto says why when it
can't make a change, such as a volume that the sequence reads from a variable.

Each track's meter shows the level of the track's notes in the output. The M and S buttons mute and solo tracks, in the
arrangement's track list, the inspector and the mixer. A muted track goes on playing, unheard. Its meter stays silent.
While any track is soloed, only the soloed tracks are heard. The Stereo Out fader sets the output's level.

The mixer's FX channels, FX A to FX C, hold the effects on the console's three aux buses. Each track sends to them at
the levels that the sequence sets. An archive holds no effects: a game's code sets them up. An imported archive
therefore plays dry until you add them. To add one, click the + in an FX channel's inserts and choose an effect. A
Switch file offers I3DL2 Reverb, the Switch's reverb. A Wii U file offers the Wii U's four effects: nw::snd Reverb Hi,
nw::snd Reverb Std, nw::snd Chorus and nw::snd Delay (Wii U). A 3DS file has FX A and FX B only. It offers the 3DS's
five effects: nw::snd Reverb (3DS), nw::snd Delay (3DS), nn::snd Reverb, nn::snd Delay and nn::snd Reverb (2011). The
last is the older reverb that games of 2011 link. 3DS games run these effects on the console's main processor. Hokuto
runs them on models that give the same samples as the games' code. On another console's engine (see above), a sound
plays that console's effects, and the menu offers them. The other effects stay on their buses and show grey. The mixer
shows only the buses that the file's console and the engine's console both have. On the Wii U and the 3DS, a send to an
FX channel without an effect is lost. The console loses it too. Click an effect to open its window. There you can set
each parameter. The I3DL2 Reverb also offers the Switch's five presets. The early and fused modes of the Wii U's reverbs
are menus. A reverb of the 3DS shows the sizes of its comb and all-pass filters as times. On the 3DS, Surround and
High-pass damping are menus. A delay can't be longer than its maximum. Raising the delay past the maximum raises the
maximum too. Lowering the maximum below the delay lowers the delay. Drag a slider, or hold Shift to move it finely.
Double-click a slider to type a value, and Tab goes on to the next one. Ctrl-click sets a parameter back to its starting
value. On the Switch, that's the first preset's value. On the Wii U, Hokuto chooses it. On the 3DS, it's the library's
default. Remove takes the effect out. During playback, a change takes effect without a stop. Hokuto first runs the new
version up to where playback has reached, in the background. That can take up to a second for each minute played. A
`.hok` file keeps the effects. The main window's title ends in `*` while the file has changes that aren't saved.

Each FX channel's fader sets the level at which its bus joins the output, after the bus's effects, from silence to +6
dB. The 3DS returns a bus at 0 dB at most. Above 0 dB, Hokuto raises the bus's samples after its effects. A game can set
the levels of its aux buses too. Drag the fader, or hold Shift to move it finely. Ctrl-click sets it to 0 dB. During
playback, the output follows the fader as it moves. The change goes into the Undo History when you let go. The meters
beside the fader show what the bus adds to the output, at the fader's level. Above the fader are the level and the
loudest peak since a click on the peak cleared it. A `.hok` file keeps each bus's level. MIDI files can't hold it. The
Export window warns of a level other than 0 dB.

The inspector, on the left of the main window, shows the selected track and part, and the channel strips of the track
and the output. The Track section's Bank and Program menus show the instrument of the track's first note. Choose
another, and the track's notes that play that instrument play the new one. Another bank keeps the program where the bank
has it. Where other tracks or sounds run the commands that set the instrument, their instrument changes too. An equals
sign beside Bank marks such a track, and the menus' tooltips name them. Type in an open menu to filter it. The change
goes into the Undo History. The Sound section shows the shown sound's values in the archive's tables. Click a section's
heading to open or close it. Under Player are the sound player that plays the sound in the game, the sound's priority in
that player, and its actor player. Under Output are the sound's volume and its pan. The volume goes from 0 to 255, and
at 127 the sound plays at full level. A sequence sound adds the priority of its notes, the release priority fix, the
tracks that it allocates and its four bank slots. Each label's tooltip says what the value does. A change goes into the
Undo History, and playback follows it without a stop. Ctrl-click a slider to set it to 127 for the volume, or 64 for a
priority. A value that a sound's entry in the archive's tables leaves out, such as its pan, takes a place there once it
changes. The player, its priority and the actor player determine which sounds a game plays at once. Hokuto plays one
sound at a time, and they don't change what it plays. Under Tags are the sound's title, artist, game, year, comment,
copyright, length and fade. Click a box to type in it. Enter, Tab or a click elsewhere keeps the text, and Escape puts
the old text back. An empty box removes its tag. The `.hok` file keeps the tags. A MIDI export writes the title and the
copyright. Hokuto's playback ignores the length and the fade: players of PSF files read tags of the same names.

The browser has a Sound section too, under its tree. It shows the same values for the sound that you select in the
browser: a sequence, a wave sound or a stream. Click its heading to open it. Drag the line above the heading to give
the tree or the section more of the browser's height.

For a wave sound, the section adds the values that its wave sound file holds for it, under Wave sound. The Wave menu
sets the wave that it plays. Its pitch, its pans, its sends, its filters and its envelope have sliders. The Switch, the
Wii U and the Wii play the wave at its rate times the pitch. The root key shows grey: those consoles don't read it. A
value that the file has no place for shows grey too, and can't change. The Wii U and the Switch read the filters from
version 1.1.0 of the wave sound file on. On the Wii, the Wave menu lists the waves of the sound's wave sound file. The
Wii has no filters, and their sliders show grey. In a file of a build of the Wii's sound library before March 2008, the
hold shows grey too. The last lines name the other sounds that play the same wave sound, give the size of the wave sound
file, and give what each group that loads it loads.

Double-click a part in the arrangement, or select it and press Ctrl+E, to open its track in the Key Editor. Click a note
to select it, or drag round notes to select them. Shift-click adds a note to the selection or takes it away. Drag a
selected note to move the selection, and drag a note's end to change the notes' length. Both snap to the grid while the
magnet button is on. The up and down arrows move the selected notes by a semitone, and with Shift by an octave. Delete
or Backspace deletes them. Q quantizes them: each moves to the nearest line of the grid. With no note selected, Q
quantizes every note of the track. Ctrl+X, Ctrl+C and Ctrl+V cut, copy and paste the selected notes. Paste puts them at
the playback position. A click on the Key Editor's ruler moves the position there, to the grid's nearest line while the
magnet button is on. Under the notes, the velocity lane shows each note's velocity as a line. Drag a line to change
the velocity. With the pencil, a click draws a note one step of the grid long, and a drag draws a longer one. A click on
a note with the pencil or the eraser deletes it. While the editor has the focus, the zoom keys zoom the editor, and Alt
with the mouse wheel changes the height of its keys.

Under the velocity lane, controller lanes show the track's settings, such as its volume, its pan, its sends, pitch bend,
its envelope and its LFOs. Click the + under the lanes' names to add a lane, and click a lane's name to choose its
setting or to remove the lane. An asterisk in the menu marks each setting that the track sets. Each command that sets
the value is a point, and a line shows the value between them. Drag a point up or down to change its value, and to the
side to move it in time. It moves to the grid while the magnet button is on. Like a note, it moves only as far as the
commands around it allow. Every selected point of the lane changes and moves with it. A point stays at its tick when its
command runs only if the last comparison held, or when its value comes from a variable. With the pencil, click to add a
point on the grid's line before the mouse, and drag to set its value. A new point goes in before the notes that start at
its tick, and those notes play with it. Click a point with the eraser to delete it, or select points and press Delete or
Backspace. The info line shows a selected point, and you can type its start or its value there. The note wait, the
timebase, the program and the bank select have no lane: the List Editor edits them.

The info line under the toolbar shows the first selected note. Click a value there to type a new one: a start or an
end as the display shows it, such as 5. 3. 0 for the third sixteenth of beat 5, a length in the same form, a pitch such
as C3 or F#2, or a velocity from 1 to 127. Enter or a click elsewhere keeps the value, and Escape keeps the old one.
With several notes selected, the first takes the new value, and the others change by the same amount.

Under the ruler, a miniature of the track shows every note of the song, with a frame round the part in view. Click or
drag there to move the view.

The speaker button at the left end of the editor's toolbar turns Acoustic Feedback on and off. While it's on, a note
sounds as long as you hold it with the mouse: when you click it, move it or draw it. Notes that the arrow keys move
sound while you hold the key. Click the keyboard on the left to hear a key, whether the button is on or off, and drag
along it to hear the keys in turn. A note plays on the sequence's instrument, without the track's volume, pan and
envelope. During playback, it sounds over the song.

An edit in the Key Editor changes the command that played the note. Where a subroutine plays the same command more than
once, every note that it plays changes together, and they're selected together. A note moves only as far as the
commands around it allow: it stays between the jumps, calls and loop points before and after it. On a track whose
notes wait for their length, a note that waits until the track's notes have ended can't move or change its length, and
neither can a note whose length the sequence picks as it plays. The notes beside it move only up to it. Hokuto says why
when it can't make an edit.

Choose Edit > Open List Editor to list the commands of the selected part's track in the List Editor. It selects the row
where the part starts: the call of a subroutine's part, or the part's first note. It shows a row for each command in the
order that the track runs them: the command's type, start, length, two data values and a comment. A note's data are its
pitch and its velocity. Each label shows as a row before its command. A label's comment counts the calls, jumps and
track starts that go there. A command that a jump reaches without a label counts them in its comment too. The list holds
the commands that play no notes too: the settings, the waits, and the logic of jumps, calls, loops, variables and
comparisons. An "if" before a command's type means that it runs only if the last comparison held. The rows inside a call
or a loop sit further in. The Track menu on the toolbar selects another track, one that plays no notes included. The
rows end where the sequence's loop comes round again.

Click a value to type a new one, and press Enter. A new start moves a note, or a setting such as a volume, the way the
Key Editor moves it. A value can come from a random number, typed as "random 0 to 127", or from a variable, typed as
"variable 3", wherever the console allows it. Hokuto says why when the console can't hold a value. Click the target of a
jump, a call or a track start to choose another label. To add a command, choose its kind in the Insert menu on the
toolbar and click +. It goes in before the selected row. Right-click a row to insert a command before it, to make it run
only if the last comparison held, to go to its target or to delete it. Delete or Backspace deletes the selected
commands. Undo puts back the rows that were selected before the edit, and Redo the rows that the edit left selected. An
edit elsewhere, such as in the Key Editor or the mixer, keeps the selection. An edit changes the command for every pass
that runs it. The Key Editor's edits work the same way.

Each sequence opens in one of two modes. A song that NintendoWare's MIDI converter wrote from a MIDI file opens as a
timeline. Each note stands alone, and each bar plays in its track. The MIDI file held the song the same way. Drag an
edge of the loop in the marker row to move the loop's start or its end, by beats or, with Shift, by ticks. A save writes
the converter's subroutines again. Any other sequence opens as a program, with the commands as the console's bytes hold
them. The arrangement then shows each call of a subroutine as a part. The inspector's Sequence section switches a
timeline to a program and back. Its tooltip says why a sequence can't be a timeline. The `.hok` file keeps the choice.

The arrangement's info line shows the selected part: its name, start, end, length and number of notes. Click the start
and type a new one to move the part's notes. Type a number of semitones under Transpose, or an amount under Velocity, to
change the part's notes by that much. The field then shows 0 again.

In a program, the List Editor also edits the labels and the subroutines. Right-click a row for Insert Label and Convert
to Subroutine. Insert Label puts a new label before the row's command. Convert to Subroutine moves the selected commands
into a new subroutine after the sequence's commands, with a call in their place. Drag a label's row onto the row of
another command to move the label there. A label's row offers Rename Label, Open Subroutine and Delete Label. Open
Subroutine shows the subroutine at the label by itself, with its calls. Its toolbar counts the calls that the shown
sound runs, and then the other calls in the sequence. Those lie in other sounds' commands, or in branches that the shown
sound didn't take. A call's row offers Open Subroutine, Dissolve Call and Convert to Real Copy. Dissolve Call puts a
copy of the subroutine's commands in the call's place. Convert to Real Copy sends the call to a copy of its subroutine
that no other call plays. An edit of the copy then changes that call alone.

The arrangement shows every track that runs commands, the tracks that play no notes included. Project > Add Track adds a
track to the sound that's shown. The new track plays the instrument of the selected track's first note. Double-click its
lane to open it in the Key Editor at that place, and draw notes into it. Project > Duplicate Tracks copies the selected
track, and Project > Remove Selected Tracks takes it out of the sound. Right-click a track in the track list for the
same commands. A removed track's commands stay in the sequence, and the sound no longer plays them. Track 0 starts the
other tracks and stays. So does a track that starts other tracks of the sound, until those have gone.

Project > Copy Track keeps the selected track, and Project > Paste Track gives a copy of it to the sound that's shown.
Show another sound of the file in between to take a track from one sound to another. The pasted track plays the notes
that the track played, from the same beat, with the same instruments. A free bank slot of the sound takes a bank that
its slots lack. The pasted track plays at the sound's tempo: it leaves out the commands that set the tempo, the
timebase and the volume of the whole sound. A track from a sound that counts other ticks to a beat takes the sound's
ticks.

A game can change a sequence through its variables. The game sets them before the sequence starts, and the sequence's
commands test them to choose what to play, such as an extra layer on the final lap. The Variables zone lists the
variables whose starting values can change the sequence that's shown. Window > Variables shows and hides it, and so does
the middle one of the three buttons at the right end of the control bar. A variable starts at -1 unless the game sets
it. Click one of a variable's switches to choose the value that it starts with. Each switch stands for one set of
outcomes of the sequence's tests. A variable with more choices than fit has a menu instead. A variable whose exact value
counts, such as one that sets a volume, has a slider from -1 to 127. Double-click the slider to type any value from
-32,768 to 32,767, and Ctrl-click it to set -1. The arrangement and the mixer then show the sequence as it plays with
those values. During playback, the change takes effect without a stop. While the sequence plays, the zone also shows the
value at the playback position of each variable that the sequence changes. Variables 16 to 31 are marked Global. In the
game, every sequence that plays shares them. In Hokuto, the value that you choose for one stays with its sound. So does
the value of any other variable. The values last until another file opens.

A scene keeps a set of starting values in the `.hok` file under a name. Scenes are listed at the top of the Variables
zone. Click + beside them to store the values that the variables start with as a new scene. Click a scene to start the
variables with its values again. The scene whose values the variables start with is lit. Double-click a scene to rename
it. Right-click it to update it with the values that the variables start with, to rename it or to delete it. Each of
these changes the file, and Ctrl+Z undoes it.

A game can also change a variable while the sequence plays, such as at the start of the final lap. The Variables zone
lists such changes under Changes, below the scenes. Click + beside them to add a change at the playback position. Each
change has a menu of the variables that the sequence reads, a field for its time, and switches, a menu or a slider for
its value. Click the time to type a new one, in seconds, in minutes and seconds, or in hours, minutes and seconds, such
as 90, 1:30 or 0:01:30.000. Enter or a click elsewhere keeps it, and Escape keeps the old time. Right-click a change to
remove it. The marker row under the ruler shows each change as a flag. The arrangement and the mixer show the sequence
as it plays with the changes, and playback goes on without a stop. A scene keeps the changes with the starting values.
Like a choice of a starting value, a change to the list doesn't edit the file, and Ctrl+Z doesn't undo it. Store or
update a scene to keep the changes. They last until another file opens.

Ctrl+Z undoes a change, and Ctrl+Shift+Z redoes it. The Edit menu names the change that each would undo or redo. The
Undo History lists every change, with its time and what it changed. A line lies under the last change that's done. Click
a change to undo or redo up to it, or drag the line's handle. Undoing back to the saved file takes the `*` away. In the
Key Editor, an undo selects the notes that were selected before the change, and a redo the notes that were selected
after it.

On the Mac, the menus are in the menu bar at the top of the screen. Command takes the place of Ctrl, and Option the
place of Alt.

| Command | Keys |
|---|---|
| New Project | Ctrl+N |
| Open | Ctrl+O |
| Save | Ctrl+S |
| Save As | Ctrl+Shift+S |
| Preferences | Ctrl+Comma |
| Quit | Ctrl+Q |
| Undo, or redo | Ctrl+Z, or Ctrl+Shift+Z |
| Start and stop | Space |
| Start | Enter on the numeric keypad |
| Stop | 0 on the keypad. A second Stop goes back to where playback started. |
| Go to Project Start | The keypad's decimal point, or Return |
| Go to the left locator, or the right one | 1 or 2 on the keypad |
| Go to the next start or end of a part on the selected track, or the previous one | N or B |
| Cycle | / on the keypad |
| Set the left locator | Ctrl-click in the ruler |
| Set the right locator | Alt-click in the ruler |
| Set the locators round the selected part | P |
| Set the locators round a sequence's intro or its loop | Double-click it in the marker row |
| Zoom to a sequence's intro or its loop | Alt-double-click it in the marker row |
| Open the commands where the tracks read variables or branch in the List Editor | Double-click their flag or line in the marker row |
| Show or hide the tempo row | Ctrl+T |
| Project Setup: the file's engine, mixing mode and tags | Shift+S |
| Open a tempo's command in the List Editor | Double-click its point in the tempo row |
| Rewind and Forward, a beat at a time | - and + on the keypad |
| Zoom in and out | H and G, or Ctrl with the mouse wheel |
| Show the whole song | Shift+F |
| Make the tracks taller or shorter | Shift+H and Shift+G |
| Show or hide the inspector | Alt+I |
| Show or hide the browser | Ctrl+P |
| Show or hide the mixer | F3 |
| Open the selected part in the Key Editor, or close the editor | Ctrl+E, or double-click the part, or a track's lane |
| Choose the Key Editor's selection tool, pencil or eraser | 1, 8 or 5 |
| Select every note in the Key Editor, or none | Ctrl+A, or Escape |
| Move the selected notes by a semitone, or by an octave | Up or Down, or Shift+Up or Shift+Down |
| Delete the selected notes, the List Editor's selected commands or the Bank Editor's selected region | Delete or Backspace |
| Move the selected notes to the grid, or every note | Q |
| Cut, copy or paste notes in the Key Editor | Ctrl+X, Ctrl+C or Ctrl+V |
| Hear a key in the Key Editor or the Bank Editor | Click it on the editor's keyboard |
| Open a bank in the Bank Editor, or the properties of a wave or a stream | Double-click it in the browser |
| Set a region's value to NintendoWare's default | Ctrl-click its slider |
| Zoom a wave's waveform in or out | Ctrl with the mouse wheel |
| Show the whole wave | Double-click the waveform |
| Set a fader or a send to 0 dB, or centre a pan knob | Ctrl-click |
| Set an effect's parameter back to its starting value | Ctrl-click the slider |
| Type an effect's parameter | Double-click the slider |
| Set a variable's starting value back to -1 | Ctrl-click its slider |
| Type a variable's starting value | Double-click its slider |
| Rename a scene | Double-click it |
| Type a change's time | Click its field |
| Clear a channel's peak | Click the peak, beside the level |
| Close a setup window or the properties of a wave or a stream, or cancel a dialog | Escape, twice in a text field |

## The MIDI files

File > Export > MIDI and SoundFont… and MIDI and SFZ… write a sequence as a Standard MIDI File of format 1, with its
instruments as a SoundFont 2 file or as SFZ files. ntdlib's converter writes the files. They follow the same rules on
every console.

The timing. A quarter note holds a whole multiple of the sequence's ticks, and at least 7,680 MIDI ticks. Events between
the console's ticks, such as bends and releases, land where the game plays them.

The first track. It holds the tempo changes and the sequence's name: the sound's title tag, or else its name. The
sound's copyright tag, or else the file's, goes in as a copyright notice. A sequence that loops has a `loopStart` marker
at the loop's start and a `loopEnd` marker at its end, around one pass of the loop. CC111 with the value 0 marks the
loop's start too, on the first channel that plays. A player that follows the markers or CC111 loops the song as the game
does.

The tracks. Each track of the sequence that plays notes takes a MIDI channel, with a MIDI track for each of its
channels. A track takes more channels where its notes overlap on one key, or where its earlier notes still sound with
other settings, up to MIDI's 16 channels. Channel 10 comes last. Many players play that channel from bank 128. The
SoundFont 2 file therefore holds a copy of that channel's presets in bank 128. CC7 sets a track's volume up to 127, and
CC10 its pan. CC11 holds the rest of the level: the volume above 127, the second volume, the main volume and the sound's
volume. CC1 sets the depth of the track's LFO. CC91 holds the send to FX A, and on the Wii CC93 holds the send to FX B.
On the Switch, the other sends reach the mix dry, through CC11. The files of the 3DS leave the sends out. The pitch bend
follows the track's bends and sweeps, with its range set by RPN 0. CC120 stops a track's notes at once where the
sequence stops them. A filter that changes during a note takes CC102 in a SoundFont 2 export, and CC103 and CC104 in an
SFZ export.

The instruments. A setting that a MIDI channel can't change during a note goes into the instrument: a track's envelope,
its LFO's type, speed and delay, its initial pan and a filter that stays the same. Each combination of an instrument and
such settings becomes a separate preset. CC0 and the program change select it. A legato, a new key on a sounding note of
a track with tie or mono on, goes out as a bend of that note.

One path. MIDI holds no variables or conditions. The files play one path through the sequence: the one that the
variables' starting values and their changes at set times give. A sound that draws random numbers takes the numbers of
one performance.

## The hok tool

The build also makes `hok`, a command-line tool for `.hok` files: `build/hok` on the Mac, and `build\Release\hok.exe`
on Windows.

```sh
hok import [--dol <main.dol>] <archive> <file.hok>
hok export <file.hok> <archive>
hok text <file.hok> <output>
hok binary <file.hok> <output>
hok model <file.hok> <output>
hok lengths [--threads <n>] <file.hok> <output>
hok items <input> <folder>
hok wav <input> <folder>
hok sf2 <input> <folder>
hok sfz <input> <folder>
hok other <file.hok> <sound> <output> [console] [mono | stereo]
hok --version
hok --help
```

`import` makes a `.hok` file from a sound archive of the Switch (`.bfsar`, `.fsst` or `.fspj`), the Wii U (`.bfsar`) or
the 3DS (`.bcsar`), with the group files beside it, and `export` builds the archive back. An archive that nothing
changed comes back byte for byte. `import` also takes a sound archive of the Wii (`.brsar`), with the game's build of
the Wii's sound library from its `main.dol`: the one that `--dol` gives, or else `sys/main.dol` of the extracted disc
that holds the archive. `--dol` applies to a Wii archive only. `export` builds the Wii's archive back. `items`, `wav`,
`sf2` and `sfz` take a Wii archive or its `.hok` file too, and `other`, `model` and `lengths` take its `.hok` file. Like
File > Export, the export also writes each group file that the edits changed. A SARC archive beside the archive or
beside the `.hok` file takes the group's new file. The game's other group files stay as they were. `text` writes a
`.hok` file in the text form, and `binary` in the binary form. Wherever `hok` reads a `.hok` file, the file can be in
either form.

`model` writes a `.hok` file in the binary form, with its archive in the model form: the items of the archive's tables
and its files, without the game's layout. The sounds' volumes, pans and sends are in Hokuto's units there. An export
lays the archive out as NintendoWare's tools lay one out. It also writes every group file and every stream beside the
archive. The archive plays the same as the game's. Its bytes can differ. Only an archive that NintendoWare's tools laid
out can take the model form. The Wii's archives take it too, with the game's build of the Wii's sound library. `text`
and `binary` keep the model form. The text form holds the model in base64 for now. Its sounds can't be edited by hand
there. `docs/format.md` describes the model.

`lengths` measures how long a player plays each sequence sound of a `.hok` file before its fade, and how long the fade
lasts. It writes the file with the lengths in the binary form. A sound plays on the model of its console, with the
file's effects, until it has played its loop twice, ends, or stays silent for 180 s, for 600 s at most. A sound that
loops then fades over 10 s. A sound that ends gets 0.5 s of silence after its last sound. The rips of the PSF family
measure their sequences the same way. A Wii sound plays with the build of the Wii's sound library that the file keeps.
The file keeps the lengths for players, with the loop that the arrangement shows. An edit in Hokuto that can change what
a sound plays drops them. `--threads` measures n sounds at a time, and one on each of the processor's threads at most.
Without it, each of the processor's threads measures one. On the 3DS, a sound plays without the file's effects.

The text form can be edited by hand. A sequence can gain or lose commands. Each command's values have to be values that
the console's sequences hold. A read of the text refuses any other value, and names its line and the values that the
command takes. The export then moves the files after it, and writes the offsets and sizes in the archive's tables again.
A sound that starts at a label keeps starting there. The lines after `tables` give each sound's values, such as its
volume, its player and its banks. The export writes them into the archive's tables. A sound's entry takes a place for a
value that it leaves out, such as its pan. That requires an archive laid out as NintendoWare's tools lay one out. An
edit of these lines can't add, remove or rename an item, or change a sound's kind. File > Import and the Project menu
add items.

`items` writes every item of a sound archive or a `.hok` file into a folder: each sequence file and wave sound file,
each bank, each wave archive and each of its waves, and each stream that the input holds. The files are those that the
browser's Export… writes. A sequence file that holds the sequences of several sounds goes out once, under the first
sound's name. `wav` writes each wave as a WAV file, each wave sound under the sound's name, and each stream under the
name of its file. An item or a wave that fails to go out is reported, and `hok` then exits with 1. The input can be an
archive, with the group files beside it, or a `.hok` file in either form. Both commands make the folder if it's missing.
macOS and Windows treat two names that differ only in case as one file. The second item of such a pair takes its index
in the archive's table after its name.

`sf2` writes the instruments of each bank as a SoundFont 2 file, and `sfz` as SFZ files: a folder for each bank, with
the samples in the folder `samples`. The files are those that the browser's Export… writes. These commands take the
same inputs as `items` and make the folder too. A bank without instruments is reported, and gives no file. A bank that
can't be read is reported and left out, and `hok` then exits with 1.

`other` writes a `.hok` file of another console that holds one sequence sound of a `.hok` file. `[console]` names the
console: `3ds`, `wii`, `wii_u` or `switch`. Without it, a sound of the Switch, the 3DS or the Wii goes to the Wii U, and
a sound of the Wii U to the Switch. A sound of the Wii goes to the Wii U first, and from there to the Switch or the 3DS.
`<sound>` is the sound's name, or its number in the sound table. The number counts from 0 over the whole table, the
sounds of every kind included. The new file keeps its archive in the model form. It holds the sound, its sequence file,
the banks in its slots and the wave archives that their waves name. Each file takes the versions that the browser's
Export… writes for the other console. The sound keeps its values in the archive's tables, such as its volume and its 3D
settings. The new file keeps the file's tags, the sound's tags, scenes and tracks' colours, the aux buses' volumes and
the effects that the other console runs. `other` prints what changes on the way, such as a file that goes in with an
older version or an effect that stays behind. A bank stays behind, and its slot stays empty, when the archive lacks its
file or its wave archives. So does a bank whose waves Hokuto can't read, such as the 3DS's waves in IMA-ADPCM. A song
for the 3DS or from the 3DS leaves out the commands that the 3DS's player skips. File > Export leaves them out too. A
song for the 3DS whose wave archives don't fit in the 3DS's model takes only the instruments that it selects. File >
Export does that too. A song for the Wii takes the banks of the slots that its notes play as one bank. A bank whose
waves don't fit in the Wii's model takes only the instruments that the song selects. With `mono`, a song for the 3DS
takes its stereo waves as mono waves. With `stereo`, it keeps them in stereo. Without either, the stereo waves go as
mono when the song holds more channels at once than the 3DS plays. The Export window makes the same choice.

## Licence

Hokuto is released under the MIT licence. See [LICENSE](LICENSE).

The program links in SDL3, under the zlib licence, Dear ImGui, under the MIT licence, and sfizz, under the BSD 2-clause
licence, with code that they include. The build takes one fix from sfizz's later sources into sfizz 1.2.3. It also
links in seqlib, xsflib and ntdlib, under the MIT licence. ntdlib includes Teakra, also under the MIT licence.
[THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md) lists each component with its licence.
