# Hokuto user guide

Hokuto opens the sound data of Nintendo games, from the Game Boy Advance to the Switch. It plays the songs the way the
console does and lets you edit them. You can write the result back for the game, to MIDI with SoundFont 2 or SFZ, or to
the open `.hok` format. To build Hokuto from source, see [development.md](development.md).

## Contents

1. [The window](#the-window)
2. [Opening files](#opening-files): [Switch, Wii U and 3DS](#switch-wii-u-and-3ds) · [Wii](#wii) · [GBA with
   MusicPlayer2000](#gba-with-musicplayer2000) · [GBA with Konami's driver](#gba-with-konamis-driver) · [GBA with
   Nintendo R&D2's driver](#gba-with-nintendo-rd2s-driver) · [GBA with Brownie Brown's
   driver](#gba-with-brownie-browns-driver) · [GBA with Ubisoft Milan's driver](#gba-with-ubisoft-milans-driver) · [GBA
   with Rare's driver](#gba-with-rares-driver) · [GBA with Quintet's driver](#gba-with-quintets-driver) · [GBA with
   Krawall](#gba-with-krawall) · [DS with NITRO-Composer](#ds-with-nitro-composer) · [DS with DSE](#ds-with-dse) · [MIDI
   files and new projects](#midi-files-and-new-projects)
3. [The browser](#the-browser)
4. [Playing](#playing): [cycle](#cycle-and-locators) · [markers](#the-marker-row) · [tempo track](#the-tempo-track) ·
   [mixing modes](#mixing-modes) · [engines](#engines)
5. [Editing](#editing): [undo](#undo) · [arrangement](#the-arrangement) · [timelines and
   programs](#timelines-and-programs) · [Key Editor](#the-key-editor) · [List Editor](#the-list-editor) ·
   [tracks](#tracks) · [mixer](#the-mixer) · [effects](#effects) · [inspector](#the-inspector) · [Bank
   Editor](#the-bank-editor) · [waves and streams](#waves-and-streams) · [variables and scenes](#variables-and-scenes)
6. [Replacing and importing items](#replacing-and-importing-items)
7. [Exporting](#exporting)
8. [Saving](#saving)
9. [The MIDI files](#the-midi-files)
10. [The hok tool](#the-hok-tool)
11. [Keyboard shortcuts](#keyboard-shortcuts)

## The window

| Zone | Place | Shows |
|---|---|---|
| Inspector | Left | The selected track and part, its instrument, and the Sound section |
| Arrangement | Middle | The tracks and parts of the current sound |
| Mixer, Key Editor, List Editor | Bottom, one at a time | Levels and effects, notes, and commands |
| Browser, Variables, Undo History | Right | The archive items, the song variables, and every change |

The buttons in the control bar show and hide the zones. So do the Window menu, Studio > Mixer and Edit > Undo History.
Window > Reset Layout puts everything back. On the Mac, Preferences… and Quit are in the Hokuto menu.

Edit > Preferences… sets the track colours: Rainbow, Standard (all grey) or Custom. With Custom, click the colour strip
of a track to choose a colour, and Automatic brings the rainbow colour back.

Studio > Studio Setup… chooses the output device and plays a test tone. It also keeps the location of your DSi digest
key file for [DS exports](#ds-images).

On Windows, restart Hokuto after you change the display scale. Until then, some text and controls keep the old size.

## Opening files

Choose File > Import > Sound Archive…, drop a file on the window, or give its path on the command line. Hokuto opens one
file at a time. While a large file loads, Cancel keeps the current file open.

| Console | Files |
|---|---|
| Switch | `.bfsar`, and `.fsst` and `.fspj` archives |
| Wii U | `.bfsar` |
| 3DS | `.bcsar` |
| Wii | `.brsar` |
| GBA | ROMs (`.gba`) with MusicPlayer2000, Konami's driver, Nintendo R&D2's driver, Brownie Brown's driver, Ubisoft Milan's driver, Rare's driver, Quintet's driver or Krawall |
| DS | Sound archives (`.sdat`), and cartridge images (`.nds`) with a sound archive or DSE files |

Hokuto also opens [MIDI files](#midi-files-and-new-projects), and [`.hok` files](#saving) through File > Open…. It
doesn't open GameCube games yet.

A sound archive contains no effects. The game sets them up in code. An imported archive plays dry until you [add
effects](#effects).

### Switch, Wii U and 3DS

Some archives keep groups of files outside the archive: in `.bfgrp` files (`.bcgrp` on the 3DS), in `.sarc` files, or in
a `userManagementFiles` folder. Hokuto reads them, and the streams, from the folder that contains the archive.

**The 3DS** plays without a firmware file. A few things differ from the other consoles:

- Surround plays as stereo. The built-in effects of the DSP firmware aren't modelled.
- There are two aux buses, FX A and FX B. Tracks have one LFO, without a curve, a phase or a period. The List Editor
  greys out commands that the 3DS skips.
- Wave sounds have no filters and no send to FX C.
- In strict mixing, the 3DS limits loud output. A note that peaks at half of full scale or more plays a little quieter
  than its volume says.

**AAC streams.** Some 3DS games keep their music in AAC files beside the archive. Hokuto plays them with the loop that
the sound sets, and exports them as `.aac`, as WAV, or as a stream file of the Wii U or the Switch. It uses the AAC
codec of macOS or Windows. A Linux build can't play, export or replace AAC streams. Replace… accepts an AAC file, a WAV
file or a stream file of any console:

- An AAC file at the sample rate and channel count of the stream goes in unchanged. It keeps the current loop if the
  loop ends within its samples.
- Anything else is encoded at the rate and channels of the stream, at the nearest of 96, 128, 160 and 192 kbit/s. If the
  `.hok` file has no copy of the stream, the new one is mono or stereo as the sound says, at 128 kbit/s, and at 44,100
  or 48,000 Hz.
- The loop moves later to match the silence that the encoder adds before the samples. The edit puts the file in the
  [model form](#the-model-form).
- Hokuto writes no frame checksums, and no 3DS has played a stream that it encoded. The stream properties window doesn't
  support AAC yet.

### Wii

Hokuto imports a Wii sound archive (`.brsar`) with the streams beside it (`.brstm`). Builds of the Wii sound library
play sequences differently. Hokuto reads the build from `main.dol` in the `sys` folder of the extracted disc. Without
it, the archive version decides, and a game with another build may sound a little different. Project Setup shows the
build.

- Sounds play in strict mixing only.
- The editors follow the rules of the build. The List Editor greys out commands that the build doesn't have, such as a
  bank select.
- A sound has one bank slot. You can't paste in a track that uses another bank. Hokuto reads no actor player.
- Archives before version 1.2 have no pan mode, pan curve or release priority fix, and version 1.2 has no release
  priority fix. These values are grey.
- A track mute sets the track volume to 0. A muted note can end sooner than its release would.
- Before the builds of March 2008, a stream sound plays the first two channels of its stream. Later builds play its
  tracks. Some games fade layers of a stream in and out. Hokuto plays every layer at once.

### GBA with MusicPlayer2000

Hokuto opens GBA ROMs whose songs use MusicPlayer2000, the sound driver from Nintendo. The browser lists the songs by
number. Banks lists the voice groups by address, with a row for each program that the songs use. Click a voice to hear
it, or double-click a voice group to [edit it](#gba-voice-groups).

- **Mixer.** A track volume runs from 0 to 127, and the fader shows 127 at 0 dB. A track is silent until a command sets
  its volume. There are no sends.
- **Notes.** A note lasts 223 ticks at most. For a tie, a note that lasts until a later command ends it, you can only
  change the velocity. Some games' drivers read no extra length after a note, and a note there lasts 1 to 24 ticks, or
  28 to 96 ticks in steps of 2 to 4.
- **Commands.** A wait that no single command can hold becomes several. Moving a volume can leave notes at volume 0. The
  List Editor edits labels and patterns, the subroutines of the driver, nested three deep at most. Labels stay in the
  `.hok` file.
- **Instruments.** The Program menu of the inspector moves notes to another voice of the voice group. A song uses one
  voice group.
- **Sound section.** Each song has a music player, a priority and a reverb. A music player without tracks can't play a
  song. Songs that share a header share the priority and reverb. Reverb set to Driver keeps whatever the game or an
  earlier song set. Song sets the reverb when the song starts, and it stays set for later songs.

**PSG channels.** The GBA has two square wave channels, a programmable wave channel and a noise channel. A later note
can take a channel and cut a note short. The Key Editor marks a cut note with a red bar at its end, and a note that
never plays with a red outline. A lane above the velocity lane shows which notes use each channel. Tracks and parts with
PSG voices show a square wave, red where notes are cut. When you export, the Export window lists the songs where your
edits cut more notes.

### GBA with Konami's driver

Hokuto detects GBA ROMs that use Konami's sound driver. The browser lists the songs by number. A live version, such as
`Song 00 (live)`, shares its commands with the song, and an edit of one changes both.

Each track plays one channel: Square 1, Square 2, Wave, Noise, then Voice 0 and up. The driver echo plays in both mixing
modes, and there are no FX channels. A tick is one frame (59.73 Hz), and songs have no tempo. The beat of the ruler
comes from the notes and the loop, and the tempo row stays empty.

- **Mixer.** Volumes use the steps of the driver: 0 to 15, or 0 to 127 on some sample tracks. The fader shows the top
  step at 0 dB. Revisions without a track pan refuse the pan knob. There are no sends.
- **Notes.** Velocities use the volume steps. A note sounds until the next note or rest, and a new length moves or adds
  a rest. A sample note shows at the pitch of its sample.
- **Notes that a volume starts.** A volume command can start a PSG note again, at the key of the note command before
  it. Such notes show in one of two ways:
  - After a note command at volume 0, each volume command shows as a note, with its volume as the velocity. Where
    several of them take their key from one note command, the Key Editor won't move one or give it a new key: "This
    note takes its key from the same command as another note." To move one, drag its point in the volume lane. The
    point can't move past a note command: "It would start another note again."
  - After an audible note command, that note and the notes that volume commands start again show as one note. A click
    selects all of them, and a new key changes them all. A new velocity changes only the first. Each later one plays
    at the level of its volume command, which the volume lane shows.

  A length can change, and a note can be removed unless a later volume command starts it again. A new or moved note
  can't go between a note command and a volume command that starts it again ("It would change another note on the
  track."), or where "the song loops before it would play". In a song that loops before one of its tracks has played
  all its notes, the Key Editor takes only a new velocity, length or key. To change the commands themselves, use the
  List Editor.
- **Commands.** There are no comparisons. A call returns right after its last command. Move or remove that command, and
  the wait after the call changes to keep the length. Where other commands start at that point, the edit is refused. So
  is an edit that leaves a song looping without a wait: that hangs the driver. Labels stay in the `.hok` file. A song
  that Hokuto can't lay out again plays and exports unchanged, but can't be edited.
- **Instruments.** On a square wave track, the Program menu offers the programs that its channel uses in the ROM. On a
  sample track, it moves notes to another sample.
- **Like the console.** On revisions with PSG pan, a track off centre plays its left side on the next channel, and that
  track loses its notes. On some revisions, a PSG note without a new volume continues the previous note. A new velocity
  can then join or split notes. On many, a volume right after a note in the same tick replaces the note start, and can
  silence it.
- **Tracks.** Each channel has a track. Add Track names an empty track for you to draw in. Duplicate Tracks and Paste
  Track use the first empty track on the same kind of channel. A pasted track restarts at each loop. Remove Selected
  Tracks empties the track.

The Bank Editor edits a sample's rate and loop where the driver's sample table holds them. Two numbers of the table can
name the same sample, and the Bank Editor lists the other numbers that an edit changes. On some revisions, the driver
plays notes of one sample at another sample's rate, such as after a bend or a volume change. The Bank Editor lists the
songs whose notes a rate edit changes that way under "Rate edits also change". A packed sample can't loop. On
the revision without a mixer, no sample loops, and a note plays at its sample's rate only at the few pitches where the
driver's table of periods gives none.

Replace… on a sample under Waves takes a WAV file, at the file's rate and with its loop. The new sample goes into the
free space at the ROM's end, and the ROM grows. A ROM that already fills the GBA's 32 MiB has no room for it. No game or
emulator has played an edited or replaced sample yet. Replace… refuses songs, and File > Import opens whole ROMs only.
A song exports as MIDI with SoundFont 2 or SFZ, and the samples export as SoundFont 2 or SFZ.

### GBA with Nintendo R&D2's driver

Hokuto detects GBA ROMs that use Nintendo R&D2's sound driver. A ROM can hold MusicPlayer2000 as well: Hokuto then opens
it with R&D2's driver, and MusicPlayer2000's songs don't show. The browser lists the songs by number, the music first
and then the sound effects. A sound effect is one track of a sequence of sound effects. Where the game plays a song in
more than one way, such as with a track muted, each way is a version of the song, named after its number. The versions
share their commands, and an edit of one changes them all.

A song names up to 10 tracks, and any track plays any of the voices: the PSG channels and the voices of the driver
mixer. In enhanced mixing, a note that finds every voice busy gets a new voice. A tempo of 150 plays one tick in each
frame (59.73 Hz). Each track sends to the driver echo, and the game sets the echo level. Hokuto keeps the starting
level. The echo then adds nothing, and there are no FX channels.

- **Mixer.** Volumes run from 0 to 255, and a track starts at 128.
- **Notes.** As on the later consoles, a note holds its length. Velocities run up to 255, and the Key Editor shows them
  from 1 to 127.
- **Commands.** The List Editor edits every command and the labels. Labels stay in the `.hok` file.
- **Instruments.** The Bank and Program menus give a track's notes another instrument of the song's banks.
- **Tracks.** Add Track, Duplicate Tracks and Paste Track use a free place in the song's header. Remove Selected Tracks
  empties its place, and its commands stay in the song. A sound effect takes no edit of its tracks.

A song that Hokuto can't lay out again plays and exports unchanged, but can't be edited. The Bank Editor shows the
samples without editing them, Replace… refuses songs and samples, and File > Import opens whole ROMs only.

### GBA with Brownie Brown's driver

Hokuto detects GBA ROMs that use Brownie Brown's sound driver, in either of its two revisions. The browser lists the
sounds by their numbers in the song table, such as `Sound 012`, the music and the sound effects together. A sound names
a list of channels, and the List Editor shows a label at the start of each list. Sounds can share their commands, and an
edit of one reaches the others.

Each channel is a track of the same number. Tracks 0 to 3 play the PSG for the music: two square waves, the wave
channel and the noise channel. Tracks 4 to 7 play the same channels for the sound effects, and while they play, the
music's tracks on those channels fall silent. Tracks from 8 on play samples: the driver mixer mixes them in revision 0,
and the GBA's two DMA sound channels play them in revision 1. A tick is one frame (59.73 Hz), and the ruler takes the
beat that the MIDI export gives the sound. The driver has no echo, and there are no FX channels.

- **Mixer.** The fader edits a sample track's volume, from 0 to 255. On a PSG track, a higher volume plays quieter, and
  the fader refuses it. The driver sets a pan as a level for each side, or as the sides that a PSG channel plays on, and
  the pan knob refuses it. The List Editor changes those commands. There are no sends.
- **Notes.** A note, a rest, a hold and a release each wait for a length from the driver's table of lengths. A note has
  no length or velocity of its own: it sounds until the channel's next note, rest or release, and it can sound a frame
  after its command. An edit writes a length as a sum of the table's lengths, and refuses a length that no sum gives.
  Velocities stay at 127. The noise channel's notes give the noise's settings, and the Key Editor shows them at their
  bytes.
- **Commands.** The List Editor edits every command and the labels. Labels stay in the `.hok` file.
- **Instruments.** On a sample track, the Program menu gives the notes of one sample set another. In revision 1, it
  selects a sample. On a square wave's track, it selects a duty, and on the wave channel's track a wave. The noise
  channel plays no instrument.
- **Tracks.** Add Track, Duplicate Tracks and Paste Track put a free channel in the sound's list. Add Track takes a
  channel of the selected track's kind. Duplicate Tracks and Paste Track give the new channel the copied track's
  commands where they are. Paste Track therefore takes only a track whose commands are stored with the shown sound's,
  until an edit changes them. Remove Selected Tracks takes a channel out of the list, and its commands stay. A sound
  whose list another sound plays refuses these edits.

An export keeps a sound that fits in its place, and moves a sound that outgrows it to room that other moved sounds left,
or to the free space at the ROM's end. A sound can't move into a filler run. The Bank Editor shows the samples without
editing them, Replace… refuses sounds and samples, and File > Import opens whole ROMs only.

### GBA with Ubisoft Milan's driver

Hokuto detects GBA ROMs that use Ubisoft Milan's sound driver. The browser lists the pieces of music by number. A piece
plays a list of sequences one after another, and a piece that loops starts the list again after the last sequence.

A sequence holds one track of commands for the 16 channels of the driver, and each command names its channel. Each
channel shows as a track. Channels 0 to 2 play Square 1, Square 2 and Wave, and channel 9 plays the noise and the
samples of the drum kit. The other channels make no sound. In strict mixing, the driver mixer plays 4 voices as in the
game, and the hardware outputs at 65,536 Hz. Enhanced mixing plays the kit on 32 voices. A tick is one frame (59.73 Hz),
and pieces have no tempo. The driver has no echo, and there are no FX channels.

- **Mixer.** The driver has no volume, pan or sends. The faders and knobs refuse an edit.
- **Notes.** The Key Editor edits the notes of a piece's first sequence. Notes of the other sequences show, but can't
  be edited. A note sounds until a note-off of its key on its channel, and a kit note until any note-off on channel 9.
  A later note on a PSG channel ends the note before it. You can't move a note without a note-off, or change its
  length. On channel 9, a transpose can't move a note between the noise and the kit. A velocity sets the level byte of
  the note command, and the Key Editor shows it from 1 to 127. The driver can't play a command a tick after another, and
  Hokuto refuses an edit that would put two commands a tick apart.
- **Commands.** The List Editor edits every command of a sequence.
- **Tracks.** Add Track takes the first free channel of 0, 1, 2 and 9, with a program change at the sequence's first
  command. Duplicate Tracks and Paste Track take the track's channel if it's free. A square otherwise takes the other
  square channel. Remove Selected Tracks deletes the channel's commands.

As for Konami's driver, an export moves a sequence that outgrows its place. A sequence can't move into a filler run, and
an export refuses a ROM that would grow past the 32 MiB that the GBA can address. SFZ export isn't done yet. The Bank
Editor shows the samples without editing them, Replace… refuses pieces and samples, and File > Import opens whole ROMs
only.

### GBA with Rare's driver

Hokuto detects GBA ROMs that use Rare's sound driver. The browser lists the tunes by number. The game's code can hold
some of a tune's channels silent, and Hokuto plays them silent too. Donkey Kong Country 3 plays a tune as two
arrangements, above and under water. Each arrangement is a version of the tune, named after its number. The versions
share their commands, and an edit of one changes both.

A tune's header names up to 16 tracks. Each command of a track names one of 16 MIDI channels, and the tune's program map
gives each program an instrument. In strict mixing, the driver mixer works as in the game, and the hardware outputs at
65,536 Hz. Enhanced mixing mixes the same voices in floating point at about 48 kHz. The driver has no echo, and there
are no FX channels.

- **Mixer.** The fader edits the volume of a track's channels, from 0 to 127. The driver has no pan or sends.
- **Notes.** A note sounds until a note-off of its key on its channel. You can't move a note whose note-off is on
  another track, or change its length. A new note plays on the channel of the track's nearest note.
- **Commands.** The List Editor edits every command of a tune.
- **Tracks.** A tune keeps its count of tracks. Add Track, Duplicate Tracks and Paste Track refuse with a message, and
  Remove Selected Tracks deletes the track's notes.

As for Konami's driver, an export moves a tune that outgrows its place, or puts it into a filler run. SFZ export isn't
done yet. The Bank Editor shows the samples without editing them, Replace… refuses tunes and samples, and File > Import
opens whole ROMs only.

### GBA with Quintet's driver

Hokuto detects GBA ROMs that use Quintet's sound driver. The browser lists the songs and the sound effects by number. A
game can keep its music in several music files, and each file numbers its songs after the file before.

A song has 6 tracks, one for each channel of the driver: two squares, the wave channel, the noise channel and the two
FIFOs. A channel's commands select its instrument: a square's duty, a wave, a noise macro or a sample. In strict mixing,
the driver works as in the game, and the hardware outputs at the rate that the driver sets. Enhanced mixing mixes the
same voices in floating point at about 48 kHz. The driver has no echo, and there are no FX channels.

- **Mixer.** The fader edits a channel's volume: from 0 to 15 on a square and the noise channel, to 4 on the wave
  channel and to 1 on a FIFO. A track whose volume changes in steps refuses the fader. The driver has no pan or sends.
- **Notes.** A note and a rest each last a note value, and the channel waits for it. A note therefore can't move or
  change its length, and it has no velocity. A transpose moves a note within its octave. A removal makes the note a
  rest, and a new note takes the place of a rest at its tick. It has to be in the octave that the channel plays there.
  Hokuto refuses a new note where the channel has no volume yet, and one that a tie would join to the note before it.
- **Commands.** The List Editor edits every command of a song. A setting, such as a volume or a tempo, can't move off
  its tick, and a new one can't go in at a tick inside a note: the driver's notes always wait for their lengths.
- **Tracks.** A song keeps its 6 tracks. Add Track, Duplicate Tracks and Paste Track refuse with a message, and Remove
  Selected Tracks makes the track's notes rests.

The songs of a music file are stored back to back. On export, a song that grows takes the free bytes after them, up to
the end of the file's part. An export that requires more room fails with a message, and a song can't move into a filler
run. SFZ export isn't done yet. The Bank Editor shows the samples without editing them, Replace… refuses songs and
samples, and File > Import opens whole ROMs only.

### GBA with Krawall

Hokuto detects GBA ROMs that use Krawall, a player of XM and S3M modules. The browser lists the pieces by number: each
module, or each song that a module's orders separate. Some games load their modules from packed music files. Hokuto
plays and exports their pieces, but can't edit them.

A piece has up to 16 tracks, one for each channel of its module. A note names its instrument, and the arrangement shows
the sample that the instrument plays at the note. In strict mixing, the Krawall mixer works as in the game. Enhanced
mixing mixes the same voices in floating point at about 48 kHz. Krawall has no echo, and there are no FX channels.

- **Mixer and inspector.** Both refuse their edits. Change the rows' volumes and instruments in the List Editor.
- **Notes.** A note sits on its row: it can't move, and it has no length or velocity. It sounds until the next note or
  note off of its channel. A transpose keeps a note from C-0 to B-7, and a removal makes the note a note off. In an S3M
  module, the note off keeps the note's instrument. The channel's later notes without an instrument therefore play as
  before. In an XM module, a note off can't hold an instrument. Hokuto refuses the removal when a later note of the
  channel has no instrument and plays the removed note's: give that note an instrument in the List Editor first. The
  effects that would play the channel's note again go with a removed note, from its row and from the channel's later
  rows up to its next note in the pattern: note delays, and in an XM module, retriggers and sample offsets too. A note
  with a tone portamento doesn't count: it only slides the pitch. A note that an effect plays on a row without a note,
  such as a note delay or a retrigger, can't be edited in the Key Editor. Edit the effect in the List Editor.
- **Commands.** The List Editor edits every entry of a pattern, the volume column and the effects included. A setting,
  such as a volume, moves by whole rows: it goes to the row that plays at the tick it's moved to, within its pattern.
  There it joins the channel's entry. An entry holds one note, one volume and one effect. A volume can't move onto a row
  whose entry already has one, and neither can an effect.
- **Tracks.** A module keeps the channels of its header. Add Track, Duplicate Tracks and Paste Track refuse with a
  message, and Remove Selected Tracks makes the track's notes note offs.

When a module's patterns outgrow their place, an export moves them to free space, and the ROM can grow up to 32 MiB.
Past that, the patterns can go into filler runs. SFZ export isn't done yet. The Bank Editor shows the samples without
editing them, Replace… refuses pieces and samples, and File > Import opens whole ROMs only.

### DS with NITRO-Composer

Hokuto opens DS sound archives (`.sdat`). Their sequences use NITRO-Composer, the sound driver from Nintendo. Hokuto
also opens DS cartridge images (`.nds`), with the largest sound archive in each. The browser lists the sequences, then
those of each sequence archive (SSAR). Hokuto reads the driver build from the image. An `.sdat` file alone plays by the
rules of the later builds.

- Tracks have no sends. A track volume stops at 127, shown as 0 dB on the fader.
- Sequences open as programs, never as timelines.
- A sequence plays the one bank that its entry names. The [Bank Editor](#ds-banks) edits it.
- The Sound section edits bank, volume, priorities and player where the archive has them. The other values are grey.
- Square waves play on channels 8 to 13 only, and noise on 14 and 15 only. As on the GBA, the Key Editor marks notes
  that lose their channel.
- Some games set up a sequence in their code as they start it: its variables, the channels of its player and tracks, and
  the SDK's reverb or output effect. Hokuto has these settings for a number of games, by the game code in the image, and
  plays and exports the sequences with them. In its other states, a game can play a sequence another way. Those states
  aren't included.

### DS with DSE

Some DS images use DSE (Digital Sound Elements), the driver from Procyon Studio, in place of a sound archive. Hokuto
opens them the same way. The browser lists the songs, sound effects and wave banks.

- Sequences open as programs, never as timelines.
- Tracks on the same driver channel share volume and pan. A mixer edit changes the channel.
- A program edit changes the bank that the track selects, and you can't change which bank that is. There are no sends.
- Add Track requires a free channel. A sound effect has 4 tracks at most. Paste Track requires the same timebase and
  banks.
- Replace… puts a song file or a MIDI file in place of a sound, a bank file in place of a bank, and a WAV file in place
  of a wave. See [DSE items](#dse-items). File > Import adds nothing to DSE files yet, a bank can't come from a
  SoundFont 2 or SFZ file, and you can't drop files on a DSE bank.

### MIDI files and new projects

With no file open, File > Import > Sound Archive… or a drop opens a MIDI file as a new Switch file. The notes sound once
a bank plays them. Import a SoundFont 2 or SFZ file with File > Import > Bank…, then choose it in a bank slot of the
Sound section. With a file open, a dropped MIDI file comes in as a [new sequence](#midi-files-as-sequences).

File > New Project… makes an empty file for the Switch, the Wii U, the 3DS, the Wii or the DS. Project > Add Sequence
Sound… adds a sound with an empty song. A note sounds only when a bank slot has an instrument for its program. Import
banks, or choose Project > Add Bank… and drop WAV files on the Bank Editor. A new DS file starts with an empty bank, and
exports as a bare SDAT.

## The browser

| Group | Contains |
|---|---|
| Sounds | Wave sounds and stream sounds |
| Sequences | Sequence sounds |
| Banks | Banks and their instruments |
| Waves | Wave archives and their waves |
| Wave Sound Files | Wave sound files and their wave sounds, under the sound group name |

Click a sequence to show it. Click an instrument to hear a second of middle C, or the nearest key that sounds. Click a
wave, a wave sound or a stream sound to hear it, and click again to stop. Double-click a bank or an instrument to open
the [Bank Editor](#the-bank-editor), and a wave or a stream sound for its [properties](#waves-and-streams). Right-click
an item for Export… and Replace…. The [Sound section](#the-sound-section) under the tree shows the selected item.

## Playing

Space starts and stops playback. Click the lower half of the ruler to move the position. Playback starts at once from
anywhere. The Transport menu lists the other commands. The arrangement and the editors keep the playhead in view: they
turn the page as playback runs off the right edge, and after a jump such as Go to Project Start.

### Cycle and locators

Cycle is off at first, and a song plays through once. The locators start round the loop, or round the whole song if it
doesn't loop. With Cycle on, playback loops as the game does when the locators are on the loop, and jumps back to the
left locator anywhere else.

Drag in the upper half of the ruler to draw a range and turn Cycle on. Drag an end to move that locator, or click the
range to toggle Cycle. Ctrl-click sets the left locator, and Alt-click the right one. Locators snap to beats and to the
loop.

### The marker row

The marker row under the ruler shows the intro and the loop. Double-click either to set the locators round it, or
Alt-double-click to zoom to it. The MARKERS menu at the left end locates, cycles and zooms too.

Blue flags mark where tracks read variables or branch, labelled with the variables, such as Var 3, 14. "if" marks a
command that runs only if the last comparison held. Hover over a flag to list its commands, or double-click to open them
in the List Editor.

### The tempo track

Project > Tempo Track shows the tempo row. Its line follows the tempo, and each point is a command that sets it. A tempo
from a random number or a variable shows as a bar.

- Drag a point up or down to change it in whole beats a minute, with Shift for finer steps.
- Drag a point sideways to move it to another beat, or another tick with Shift. It can't pass the commands around it.
- A command in a subroutine changes everywhere it runs, and the tooltip names other sounds that run it.
- A new tempo can change what a sound with random numbers plays.
- On the GBA, tempos are even numbers. MusicPlayer2000 plays them 0.45 % slower than written. The point shows the
  command, and the line what plays.

### Mixing modes

- **Strict** plays a sound as the console does, with its voice limit, resampling and rounding. That's what you hear in
  the game.
- **Enhanced** lifts those limits: up to 1,024 voices, mixed at about 48 kHz in floating point with a long windowed-sinc
  resampler. On the GBA, it also smooths the PSG channels. On the GBA with MusicPlayer2000 and on the DS, a note gets a
  new channel where the console would drop it or cut it. A note in its release still gives way.

The switch in the mixer header sets the mode of the current sound, and Project > Project Setup… sets every sound. The
Wii plays in strict mixing only.

### Engines

The engine switch in the mixer header plays a sound on another console: the 3DS, the Wii U, the Switch, the Wii or the
DS. You hear the [conversion](#another-console) that File > Export > Another Console… writes, and the tooltip lists what
changed. The editors still show and edit the original. The List Editor greys out commands that the conversion drops. A
MIDI export converts what you hear. The archive export keeps the original. On the Wii engine, sounds play in strict
mixing.

GBA songs, DS sequences and DSE sounds play on these engines without effects. Their tracks pass through a MIDI file and
get new numbers. Mutes and meters follow the notes. A DS sequence ignores the Variables zone there. No sound plays on
the GBA, DSE, Konami, R&D2, Brownie Brown, Ubisoft Milan, Rare, Quintet or Krawall engines.

## Editing

Every edit goes into the Undo History, and playback follows it without stopping. When Hokuto can't make an edit, it says
why.

### Undo

Ctrl+Z undoes and Ctrl+Shift+Z redoes. In the Undo History, click a change to undo or redo up to it. The window title
ends in `*` while there are unsaved changes. Undo and redo bring back the selection in the editors.

### The arrangement

The arrangement shows every track that runs commands. Each subroutine call is a part named after the subroutine. An
equals sign after the name marks a linked part. Other calls play the same subroutine, and editing its notes changes them
all.

The info line shows the selected part. Type a new start to move its notes, or type semitones under Transpose or an
amount under Velocity.

### Timelines and programs

A song from the NintendoWare MIDI converter opens as a **timeline**, the way its MIDI file was: each note stands alone,
and each bar plays in its track. Drag an edge of the loop in the marker row to move it by beats, or by ticks with Shift.
Saving writes the subroutines of the converter again.

Anything else opens as a **program**, with the commands as stored and each subroutine call as a part. The Sequence
section of the inspector switches between the two. Its tooltip says when a sequence can't be a timeline.

### The Key Editor

Double-click a part, or select it and press Ctrl+E.

- Click or drag round notes to select them. Shift-click adds or removes one.
- Drag notes to move them, or the end of a note to change the length. Both snap while the magnet button is on.
- Up and Down move notes by a semitone, or an octave with Shift. Q quantizes the selection, or every note.
- Ctrl+X, Ctrl+C and Ctrl+V cut, copy and paste. Paste goes to the playback position.
- The pencil (8) draws notes. The pencil or the eraser (5) deletes a note you click.
- Drag a line in the velocity lane to change a velocity. Alt with the mouse wheel changes the key height.

The info line shows the first selected note. Click a value to type a start such as 5. 3. 0 (the third sixteenth of beat
5), a length, a pitch such as F#2, or a velocity from 1 to 127. With several notes selected, the others change by the
same amount. Click or drag the miniature under the ruler to move the view.

The speaker button turns Acoustic Feedback on, and notes sound while you hold or move them. Click the keyboard to hear
keys at any time. Notes play without the volume, pan and envelope of the track.

An edit changes the command that played the note. If a subroutine plays the command more than once, those notes change
and select together. A note can't move past the jumps, calls and loop points around it. On a track whose notes wait for
their length, you can't move or resize a note that waits for the others to end, or one whose length the sequence picks
as it plays.

**Controller lanes** show settings such as volume, pan, sends, pitch bend, envelope and LFOs. Click + to add a lane, and
click a lane name to change or remove it. An asterisk marks settings that the track uses. Each point is a command.

- Drag a point to change its value or move it in time. Selected points move together and can't pass the commands around
  them. A point after a comparison, or with a value from a variable, stays at its tick.
- Click with the pencil to add a point, and drag to set it. It comes before the notes at its tick.
- Delete points with the eraser, or with Delete or Backspace. Type a start or a value in the info line.
- The note wait, timebase, program and bank select have no lane. Use the List Editor.

### The List Editor

Edit > Open List Editor lists the commands of the selected part, in the order the track runs them: type, start, length,
two data values and a comment. Labels show as separate rows, with the number of calls and jumps to them. Rows inside
calls and loops are indented, and "if" marks a conditional command. The Track menu shows other tracks. The list ends
where the loop comes round. It shows only the commands that the track runs with the current variable values, up to
that point. A command behind a branch that isn't taken, or one that runs only in a later pass, has no row. Choose other
values in the Variables zone to see other branches.

- Click a value, type and press Enter. Type "random 0 to 127" or "variable 3" where the console allows it.
- Click the target of a jump, a call or a track start to choose another label.
- Choose a kind in the Insert menu and click + to add a command before the selected row.
- Right-click a row to insert before it, make it conditional, go to its target or delete it.
- An edit changes the command for every pass that runs it.

In a program, right-click for Insert Label and Convert to Subroutine, and drag a label row to move the label. A label
offers Rename Label, Open Subroutine and Delete Label. A call offers Open Subroutine, Dissolve Call and Convert to Real
Copy. Dissolve Call puts the commands of the subroutine in place of the call. Convert to Real Copy gives the call a
private copy of the subroutine to edit.

### Tracks

The Project menu, or a right-click in the track list, edits the tracks of the current sound.

- **Add Track** adds a track with the instrument of the selected one. Double-click its lane to draw notes.
- **Duplicate Tracks** copies the selected track.
- **Remove Selected Tracks** stops the sound from playing it. Track 0 stays, and so does a track that starts others
  until those are gone.
- **Copy Track** and **Paste Track** copy a track to another sound. It plays the same notes from the same beat, at the
  tempo of the new sound. A free bank slot gets any missing bank.

On the **GBA**, new tracks go after the last one. They have to fit in the music player of every song that shares the
header, and a header lists 16 tracks at most. You can only paste between songs with the same voice group. Track 0 can
go, but a song keeps one track. If the header lists more tracks than the player plays, removing one brings in the next.
A copy and its original can't both play a PSG voice at once.

### The mixer

The mixer shows the volume, pan and sends of each track at the playback position. The three slots above the pan knob
send to FX A, FX B and FX C.

Drag a fader, knob or send to change it, with Shift for fine moves, and Ctrl-click to reset it. A change rewrites the
commands that set the value. A fade or other change over time moves as a whole: by the same decibels, or the same pan
distance. If the commands are in a shared subroutine, the change reaches every track and sound that uses it. An equals
sign after the track name marks such a track, and the tooltips list what else changes.

M and S mute and solo tracks. A muted track keeps playing silently. The Stereo Out fader sets the output level.

### Effects

FX A to FX C are the aux buses of the console. Click + in the inserts of an FX channel to add an effect:

| File | Effects |
|---|---|
| Switch | I3DL2 Reverb |
| Wii U | nw::snd Reverb Hi, nw::snd Reverb Std, nw::snd Chorus, nw::snd Delay (Wii U) |
| 3DS (FX A and B) | nw::snd Reverb (3DS), nw::snd Delay (3DS), nn::snd Reverb, nn::snd Delay, nn::snd Reverb (2011) |
| Wii | AXFX Reverb Hi, AXFX Reverb Std, AXFX Chorus, AXFX Delay |
| GBA, DS | No FX channels. The MusicPlayer2000 reverb is a song setting. |

On the 2008 builds of the Wii, the two reverbs are AXFX Reverb Hi Exp and AXFX Reverb Std Exp. On another engine, the
menu offers the effects of that console. On the Wii U and the 3DS, a send to an empty FX channel is lost, the same as on
the console.

Click an effect to open its window. Double-click a slider to type a value, and press Tab for the next. Ctrl-click resets
a parameter to the first preset on the Switch, a value from Hokuto on the Wii U, or the library default on the 3DS. The
I3DL2 Reverb offers five presets. Raising a delay past its maximum raises the maximum. Remove deletes the effect. During
playback, a change takes up to a second for each minute played to reach the output.

The fader of an FX channel sets the bus level, from silence to +6 dB. The meters show what the bus adds. Click the peak
to clear it. MIDI files can't store bus levels or effects.

### The inspector

The Bank and Program menus of the Track section show the instrument of the first note. Choose another, and the notes
that played the old one play the new one. If other tracks or sounds share those commands, they change too, and an equals
sign marks the track. Type in an open menu to filter it.

### The Sound section

The Sound section in the inspector shows the values of the current sound in the archive tables. The one in the browser
shows the selected item.

- **Player:** the sound player, the priority in it, and the actor player. These decide which sounds a game plays at
  once. Hokuto plays one sound at a time, and they don't change what you hear.
- **Output:** volume, from 0 to 255 with full level at 127, and pan.
- A sequence sound also has the note priority, the release priority fix, the tracks it allocates and four bank slots.
- **Tags:** title, artist, game, year, comment, copyright, length and fade. An empty box removes a tag. MIDI exports use
  the title and copyright. Hokuto ignores the length and fade. PSF players read them.

Ctrl-click a slider for 127, or 64 for a priority. A value that the entry leaves out, such as a pan, gets a place in it
when you change it.

**Wave sound** shows values from the wave sound file: the wave, pitch, pans, sends, filters and envelope. Values that
the console doesn't read, such as the root key, are grey, and so are values that the file has no room for. The Wii U and
the Switch read filters only from file version 1.1.0 on.

### The Bank Editor

Double-click a bank or an instrument in the browser, or choose Open Bank Editor.

**Switch, Wii U, 3DS and Wii.** The list shows the instruments by program. Add, Duplicate and Delete work on the
selected instrument. Type in the Program field to move it. The map shows regions with keys across and velocities up.
Drag a region to move it, or an edge to resize it. The fields show every value of the region, and fields that the
console doesn't read are grey. Ctrl-click a slider for the NintendoWare default. Click the keyboard to hear the
instrument, lower on a key for a louder note.

Drag waves from the browser, or WAV and `.bfwav` files from your desktop:

- One wave on a region replaces its wave.
- Several on the map make one-key regions upward from the drop point, like a drum kit.
- Waves on the program list make an instrument each, over every key, with the root key at C3.

WAV files come in as DSP-ADPCM. A Wii bank keeps its waves inside the bank, listed in Waves under the bank name. A
region that uses another wave gets a copy. A Wii bank can't keep an instrument without regions. In a Wii bank, Add
therefore makes an instrument that plays the bank's first wave over every key. In a Wii bank without waves, drag a wave
onto the list instead. The editor won't open a Wii bank whose regions have a surround pan or other values it doesn't
show. The lines at the bottom name the sequences that play the bank, groups that load it without its waves, and sizes.

#### GBA voice groups

The list shows the programs that the songs select, by kind: sample, PSG channel, key split or drum kit. Key ranges and
drum keys show as rows. A drum kit that runs on into other data shows only its real keys. The fields show the sample,
pitch mode and envelope of a sample voice, the duty and sweep of a square wave, the 32-step pattern of the wave channel
(drag to draw), and the noise mode. PSG voices show length and envelope in frames. The keyboard plays with the music
player of the first song that uses the group.

Edits change the ROM in place, and the lines at the bottom name everything that shares the voice or pattern.

- The Kind menu turns a voice into a sample or a PSG voice, and a program into a key split or a drum kit.
- Add puts a voice at a free program, one that no song selects. Duplicate copies the selected voice there. Delete clears
  a voice. If no program is free, delete one first.
- Drop WAV files to add samples, rounded to 8 bits. AIFF files and raw ROM samples keep their rates and loops.
- Make Single Voice gives a key split or drum kit one voice for every key.

New samples, patterns and tables go into the free space after the ROM data, and the ROM grows. Old tables stay. Each
edit of drum kit rows adds about 1.5 KiB. A voice group whose table is followed by other data moves with its first new
voice, and its songs follow it. Test the ROM in an emulator. At 32 MiB, new bytes can go only into [filler
runs](#gba-roms).

#### DS banks

Each program shows its kind: wave, square wave, noise, wave by address, silent instrument, drum set or key split. Drum
keys and key ranges show as rows.

- The Kind switch makes a region a wave, a square wave or noise. A square wave duty is in eighths: 0 is a 12.5 % pulse,
  and 7 is silent. Root key, pan and envelope run from 0 to 127.
- Make Key Split and Make Drum Set add regions, and Keep This Region goes back to one. Split, Delete Region and the Keys
  fields change the ranges.
- Wave Archives… chooses the wave archives in the four slots of the bank.
- Drop WAV or SWAV files, or drag a wave from a wave archive in those slots.

Value edits keep the archive size. Region edits rewrite the bank file.

#### DSE banks

The editor edits the keys, velocities, wave, root key, tuning, volume, pan, key group, bend range and envelope of each
split. DSE ignores the sample rate of a wave and uses the tuning instead. A new wave rate therefore changes the tuning
of every split that plays it. Every file with the bank gets the edit.

### Waves and streams

Double-click a wave, or choose Wave Properties, to open its properties. Double-click a stream sound, or choose Stream
Properties, for its stream.

The window shows the waveform and the loop, and doesn't edit samples. Drag the S and E markers to move the loop. With
the magnet on, they snap near zero crossings. Ctrl with the wheel zooms, and a double-click shows the whole wave. Play
loops until Stop.

- **Loop** turns the loop on or off. Type a start or end and press Enter. A DSP-ADPCM loop starts on a 14-sample frame,
  and a stream loop on a block. The window says where a typed start moves to, and where a new encoding moves a stream's
  loop and end. On the Wii, a loop from the game can start between frames until you move it.
- **Rate** changes pitch and length, and keeps the samples.
- **Encoding** offers DSP-ADPCM, 16-bit PCM and 8-bit PCM for waves, and DSP-ADPCM and 16-bit PCM for streams.
  DSP-ADPCM is about a quarter of the size of 16-bit PCM, and adds some noise.

Stream tracks are listed but not editable yet.

**DS waves** loop on 32-bit words: 4 samples of 8-bit PCM, 2 of 16-bit PCM or 8 of IMA-ADPCM. The loop runs to the end,
and the rate runs from 256 to 192,000 Hz. **DS streams** loop on blocks of 512 bytes per channel, and the rate runs from 256 to
65,535 Hz. A new encoding moves the loop forward to a block. Nobody has checked how a game plays a stream at a rate
other than the DS mix rate of about 32,728 Hz. The window doesn't edit the stream entry in the archive tables.

### Variables and scenes

Games steer sequences through variables. A game sets them before the song starts, and the commands test them, for
example to add a layer on the final lap. The Variables zone lists the variables that can change the current sequence.

A variable starts at -1 unless the game sets it. Click a switch to choose a starting value. Each switch stands for one
outcome of the tests. Variables with many choices have a menu, and variables used as values, such as a volume, have a
slider from -1 to 127. Double-click it to type any value from -32,768 to 32,767. During playback, the zone shows the
current value of each variable that the song changes. Variables 16 to 31 are marked Global. In a game, all sequences
share them. In Hokuto, the choice stays with its sound.

**Scenes** save sets of starting values in the `.hok` file. Click + beside Scenes to store one, and click a scene to
apply it. Right-click it to update, rename or delete it. These edits can be undone.

**Changes** set a variable at a time during playback, such as the start of the final lap. Click + beside Changes to add
one at the playback position. Choose the variable and value, and type a time such as 90, 1:30 or 0:01:30.000.
Right-click a change to remove it. Flags in the marker row show the changes.

Starting values and changes aren't edits. Ctrl+Z doesn't undo them, and they're gone when you open another file. Store a
scene to keep them.

## Replacing and importing items

Right-click an item in the browser and choose Replace… to put a file in its place. File > Import adds a file as a new
item. Both open a window that shows what changes, warns about problems, and plays the result where it can. Undo reverses
the edit. Large imports can take a few seconds, and Cancel stops them.

The windows accept files of the 3DS, the Wii U and the Switch, and convert files from the other two. Sequences get the
[biquad filters](#biquad-filters) of the target console.

### Sequences

Replace… on a sequence accepts a `.bcseq` or `.bfseq` file, or a [MIDI file](#midi-files-as-sequences).

- **Start** offers the labels of the file. A file with the commands of a sequence in the archive, such as one from
  Export…, also offers the starts of its sounds.
- **Banks** sets the bank in each slot.
- **Instruments** maps each program of the new commands to a program of the bank in its slot.
- **Variables** lists the variables that the new commands read before setting them. Keep, renumber or fix each one.
- **Play** plays the sound for up to 30 s.

Other sounds that share the file keep their commands. If the entry of the sound has no start, the sounds that start at
the first command of the file play the new commands too. A game sometimes leaves a track out of a sound on purpose to
keep it silent. If the new commands open such a track, the window offers to allocate it. A 3DS archive refuses commands
that the 3DS doesn't have.

### Banks, wave archives and wave sound files

Replace… on a bank, wave archive or wave sound accepts a file of the same kind (`.bfbnk`, `.bcbnk`, `.bfwar`, `.bcwar`,
`.bfwsd`, `.bcwsd`). It replaces every copy, in the archive, its groups and the group files beside it. A group copy that
points to its waves elsewhere keeps pointing there.

For a bank, the window lists the programs that the sounds use and the new bank is missing. Leave each silent, or copy in
an instrument from the current or the new bank. The warnings cover other items that share the file, missing waves, and
version changes.

### Waves

Open a wave archive and choose Replace… on a wave. It accepts a WAV file (PCM up to 32 bits or float, mono or stereo) or
a `.bcwav` or `.bfwav` file. The wave gets the samples, the rate and the first loop of the file.

- **Sample rate** offers lower rates, down to 8,000 Hz.
- **Loop** can give a WAV file without a loop the current loop.
- **Encoding** chooses DSP-ADPCM or 16-bit PCM. 16-bit PCM uses about 3.5 times the space. The window shows the
  signal-to-noise ratio of DSP-ADPCM.

A DSP-ADPCM loop start moves forward to the next 14-sample frame, and the loop still sounds the same. Samples after the
loop end are dropped. A game reserves memory for its wave archives, and a larger one may not fit. The window lists what
the WAV file has that the wave drops, such as a second loop or more than 16 bits.

### Streams

Replace… on a stream sound accepts a `.bcstm` or `.bfstm` file or a WAV file. File > Import > Stream… adds one as a new
stream sound. Exports write new and changed streams beside the archive. For AAC streams, see [the
3DS](#switch-wii-u-and-3ds).

On the 3DS before archive version 2.3.0.0, a stream sound plays the tracks of its stream file. A new stream keeps the
current tracks if its channels allow, or gets a track for each channel pair. A 3DS stream sound plays 4 stereo tracks at
most. Hokuto adds no stream sound to versions 2.1.0.0 and 2.3.1.0, and no sound to an archive without names.

### New items

| File > Import | Adds |
|---|---|
| Sequence… | A sound that plays a sequence or MIDI file, with settings copied from the sound you choose |
| Bank… | A bank file, or a [SoundFont 2 or SFZ file](#soundfonts-and-sfz-files-as-banks) |
| Wave Archive… | A wave archive file |
| Wave Sound File… | A wave sound file. Each wave sound becomes a sound with settings from the sound you choose. |
| Wave… | A wave file, after the waves of a wave archive |
| Audio File… | A new wave from a WAV file |
| Stream… | A stream sound |

Import the wave archives of a bank file before the bank. New items go at the end of their table, named after the file.
Wave sounds get an index, such as `SE_HIT_00` to `SE_HIT_11`. A new wave plays once you choose it in the Bank Editor or
the Sound section, or import a file that uses it.

### Groups

Games load most files in groups. The Groups list proposes the groups that load the replaced item, or the sound that a
new item copies. Check the groups that should get the files, or type a name to make a new group. The code of a game asks
for sounds and groups by number. A new sound or group does nothing in the game until the code asks for it.

### SoundFonts and SFZ files as banks

File > Import > Bank… accepts a SoundFont 2 file on the 3DS, the Wii U, the Switch, the Wii and the DS. Each MIDI bank
becomes a bank, and each preset an instrument. Each key and velocity gets the voice that a SoundFont player would start
there, and layered voices keep only the first. Samples become 16-bit PCM waves, stereo at the centre, or mono on the DS.
Regions keep the root key, tuning, attenuation, pan and exclusive class, and get the nearest envelope. The window lists
what's left out, such as filters and LFOs.

Bank… also accepts SFZ files on the same consoles, one instrument each. A name ending in an underscore and a program,
such as `BANK_GM_005.sfz`, brings in every file of the folder with the same stem, each at its program. Other files come
in at program 0. Hokuto reads `<control>`, `<global>`, `<master>`, `<group>`, `<region>`, `#define`,
`#include`, and these opcodes as sfzformat.com defines them: `sample`, `default_path`, `note_offset`, `octave_offset`,
`key`, `lokey`, `hikey`, `lovel`, `hivel`, `pitch_keycenter`, `transpose`, `tune`, `pitch_keytrack`, `volume`,
`amplitude`, `pan`, `ampeg_delay`, `ampeg_attack`, `ampeg_hold`, `ampeg_decay`, `ampeg_sustain`, `ampeg_release`,
`group` with a matching `off_by`, `loop_mode`, `loop_start`, `loop_end`, `offset` and `end`.

- Samples have to be WAV files.
- Release triggers, and later round-robin or random regions, are left out.
- A `one_shot` note ignores its note-off.
- A positive `volume` lowers the whole file by that much.
- If there are too many regions for one SoundFont, move some files to another folder.

### MIDI files as sequences

File > Import > Sequence… and Replace… accept MIDI files on the 3DS, the Wii U, the Switch, the Wii and the DS. Each
channel with notes becomes a track, up to 16. The song uses 48 or 96 ticks to a quarter note, whichever moves fewer
events, and always 48 on the DS. Events snap to the nearest tick, and tempos round to whole beats a minute. The
`loopStart` and `loopEnd` markers, or CC111, set the loop. The sound opens as a timeline, or as a program on the DS or
when its file is shared.

| MIDI | Becomes |
|---|---|
| Program change, CC0 | Program, bank slot |
| CC1 | LFO depth |
| CC5, CC65, CC84 | Portamento |
| CC7, CC10, CC11, CC64 | Volume, pan, second volume, damper |
| CC72, CC73, CC75 | Release, attack, decay |
| CC74, CC102, CC103 | Low-pass filter |
| CC104 | Biquad filter value. Set the type in the List Editor. |
| CC91, CC93 | Sends to buses A and B |
| CC126, CC127 | Mono and poly |
| Pitch bend, RPN 0, RPN 2 | Bend, bend range, transposition |

A note that outlasts a pass of the loop ends where the next pass starts. The warnings list such notes and anything left
out.

**Instruments** maps each program of the file to a program of the bank, starting with the same number. **Drum notes**
maps each note on channel 10 to a program and key, for banks that keep each drum as a separate instrument. The `.hok`
file remembers the mappings for each bank.

### Wii items

Replace… on a Wii item accepts `.brseq`, `.brbnk`, `.brwsd`, `.brwar` and `.brwav` files, and WAV files for a wave. A
new bank or wave sound file has to keep its waves the same way as the old one, in an RWAR or in a WAVE block. Files of
the 3DS, the Wii U and the Switch are converted as for [another console](#to-the-wii). Every group with the file gets
the new bytes.

- In builds older than March 2008, every setting of a converted sequence takes effect at once. The window warns.
- A bank or wave sound file from another console uses the waves of a file of the archive. The window pairs them.
- The Wii plays no IMA-ADPCM, and refuses waves in it.

File > Import and Project > Add Sequence Sound… and Add Bank… work on the Wii too. A bank or wave sound file comes in
with the RWAR of the same name beside it, or with copies of waves from the archive. A wave archive becomes a bank
without instruments, and new waves go into a bank or wave sound file. Archives before version 1.2 pan every sound in
balance mode with the square-root curve. Play in the Import window plays nothing yet. New files join the groups you
choose, or the last group. Hokuto can't add groups on the Wii.

Wii stream sounds also accept `.brstm`. A Wii stream sound plays 8 tracks and 16 channels at most. A new stream goes
into the `stream` folder beside the archive. Games reserve room for the streams that play at once, and Hokuto doesn't
check it yet. Hokuto adds no stream sound to an archive that NintendoWare didn't lay out.

### DS items

Replace… on a DS item accepts SSEQ, SBNK, SWAR, SWAV and STRM files, and WAV files for waves and streams. An archive
that shares its file with other data, such as one inside a NARC file, refuses a file that grows. File > Import, Add
Sequence Sound… and Add Bank… add items at the end of their tables, if the SDK tools laid out the archive. Only a change
to the game code plays a new sound.

### DSE items

Replace… on a DSE item accepts a song file (`.smd`) or a MIDI file for a sound, a bank file (`.swd`) for a bank, and a
WAV file for a wave of a bank with samples. The Wave window's Replace… does the same for its wave.

- **Sounds.** The new tracks play the sound's banks. Tracks that select none of the sound's banks select its first bank
  instead. Tracks that select one of them keep every bank select: the tracks of a game can also select a bank that the
  sound doesn't load. **Instruments** maps each program of the new tracks to a program of the first bank, and **Drum
  notes** maps a MIDI file's drum notes. The sound takes the ticks to a quarter note and the counts of tracks and
  channels from the new tracks. A sound effect has 4 tracks at most.
- **MIDI files.** A MIDI file comes in at 48 ticks to a quarter note. Each channel with notes becomes a track on a
  separate driver channel, up to 16. The `loopStart` marker, or CC111, sets the loop point. DSE sets volumes, pans and
  other values at once, and the window counts the settings that lose their time. A MIDI file that Hokuto exported
  numbers its programs by the presets of its SoundFont, in the order the sound first plays them. Pair them with the
  bank's programs under **Instruments**.
- **Banks.** Every file of the image that holds the same bank gets the new one. The new bank keeps the ID and the name
  of the current bank, and the songs and the game still find it. A bank whose waves are in the main bank keeps the
  current main bank. The window lists the sounds that play the bank, the programs that the sounds use and the new bank
  is missing, and the waves that the new splits play and the main bank is missing.
- **Waves.** **Encoding** chooses IMA-ADPCM, 16-bit PCM or 8-bit PCM, and **Sample rate** offers lower rates. DSE
  ignores the rate of a wave and uses the tuning instead. A new rate therefore changes the tuning of each split that
  plays the wave, and the notes keep their pitch. Samples that don't fit in the place of the old ones go after the
  bank's samples.

### GBA songs and voices

**MIDI files.** File > Import > Sequence…, or Replace… on a song, puts a MIDI file in place of the commands of a song.
Choose a voice of the voice group for each program. Like the community MIDI converters, the window proposes the voice
with the program number, and the first drum kit for channel 10. Every song that shares the commands plays the new ones.
The `.hok` file remembers your choices.

- The song uses 24 ticks to a quarter note. No tempo means 120 BPM. Notes longer than 223 ticks become ties. In a game
  whose notes last only the driver's note lengths, a note of another length becomes a tie too.
- The text events "[" and "]", the `loopStart` and `loopEnd` markers, or CC111 set the loop. Without a loop, the end of
  a track cuts its releases.
- CC7, CC10, CC11 and CC1 set volume, pan, expression and LFO depth, and RPN 0 the bend range. The control changes and
  text events of the community converters, such as "modt=" and "tune=", work too.
- You can't add songs to the song table.

**SoundFonts and SFZ.** File > Import > Bank… adds presets as voices of the voice group of the song chosen in the Song
menu. Each preset gets a free program, and the window lists any left without one. Bank 128 and presets with an unusual
scale tuning become drum kits. A preset with one sample and envelope becomes one voice, and others become key splits.
Voices play one sample at every velocity, without filters or LFOs. Samples go in at 8 bits, mono, after the ROM data,
and the Sample rate menu can lower their rate. A SoundFont exported from a voice group comes back at the same level.

**Assembly.** Export… writes a song in the assembly form that GBA projects use, with the names of MPlayDef.s. The GNU
and LLVM assemblers rebuild the same bytes. Import > Sequence… and Replace… read such files, with the priority and
reverb of the header. Expressions can use + - * / and parentheses. Macros are refused, with the line number. A song
whose commands overlap, or whose tracks come after its header, can't be exported this way.

Export… also writes a voice group as plain `.byte` and `.4byte` lines. Samples are named by address, such as
`DirectSoundWaveData_08123456`. Drop a `.s` file on the Bank Editor, or choose Replace… on the voice group, to bring one
back. Tables that match the ROM stay in place, and the rest go into the free space. Voice macros and undefined names
aren't supported. A voice sample exports as raw ROM data or as AIFF. Compressed samples can't be exported.

## Exporting

Every export opens the Export window. It warns about what the export can't keep, and for an archive, how much it and
each group grow. A game loads groups into memory of a size that no file records. Only the game or an emulator shows
whether a grown archive still fits.

### Sound archives

File > Export > Sound Archive… writes the archive with your edits, to replace the original.

- Changed group files go beside it as `<group>.bfgrp`, or `.bcgrp` on the 3DS, or into the `userManagementFiles` folder
  if the game keeps them there.
- If a SARC archive beside the archive held the group file, a copy of the SARC archive gets the new one.
- New and changed streams go beside the archive.

#### GBA ROMs

A song that outgrows its place moves into the free space after the ROM data, and the ROM grows. When a ROM already fills
the 32 MiB that the GBA can address, **Move songs into filler runs** decides. Off by default, it leaves the edits to
those songs out. On, it moves those songs into runs of filler bytes inside the ROM: 16 KiB or more of 0xFF for
MusicPlayer2000, or 4 KiB or more of 0xFF or zeros for Konami's, Nintendo R&D2's and Rare's drivers and Krawall. The
GBA's other drivers take no filler runs. Nothing shows whether other game code reads those bytes, or the old place of a
moved song. Always test an exported ROM in an emulator.

#### DS images

The export writes the image with the edited archive, or the archive alone if you opened an `.sdat` file. DSE files go
back into their image too. An image that also runs on the DSi contains hashes. With the DSi digest key set in Studio
Setup, Hokuto updates them. Without it, they stay out of date. Either way, the signature covers the old hashes, and a
DSi in DSi mode may refuse the image. A DS image can also contain a signature and two whitelist hashes that no key
updates. The Export window lists the ones that the export leaves out of date, and a DSi or a 3DS may refuse to run
such an image.

### MIDI with SoundFont 2 or SFZ

File > Export > MIDI and SoundFont… writes the current sequence as a MIDI file with a SoundFont 2 file beside it. MIDI
and SFZ… writes SFZ files instead, one per track in `sfz/<name>`, with samples in `sfz/samples`. The files follow the
variables. GBA songs export with SoundFont 2 only. See [The MIDI files](#the-midi-files).

The Export window then compares the export with the console model, without effects. Hokuto plays SoundFont 2 files with
a built-in renderer, and SFZ files with sfizz.

- A second more than 1 dB off may be audible. More than 3 dB is audible.
- So is a 200 ms stretch where every 50 ms window is more than 6 dB off.

Play Export and Play Hokuto play the three worst places. The overall level doesn't count, and only the first 5 minutes
are measured. Sounds with random numbers play differently each time, and the window says so. Other players may sound
different. On the GBA, the model plays without the reverb or the echo of the driver.

### Another console

File > Export > Another Console… copies the current sequence sound into a new `.hok` file for the console in the Target
menu: the 3DS, the DS, the Wii U, the Switch or the Wii. The sound, its sequence, banks and waves are converted. It
keeps its values, tags, scenes, track colours and any effects that the target supports. The Export window lists every
change, then compares the result with the original. Wii, GBA, DS and DSE sounds go through the Wii U first.

#### To and from the 3DS

- The 3DS skips the curve, phase and period of the first LFO, the other LFOs, and the send to FX C. A sequence whose
  commands have to stay in place keeps them, and can't go where a command has another length.
- Volume ramps become instant.
- Wave archives get 64 MB. A song that doesn't fit keeps only the instruments it selects. If its programs come from a
  variable, or it still doesn't fit, it can't go.
- The 3DS plays 24 channels at once and drops the lowest priority notes past that. **Convert stereo waves to mono**
  halves the channels of stereo waves. Hokuto turns it on when a song uses more than 24.

#### To the Wii

The new file is for the June 2008 build.

- A Wii sound plays one bank. The banks merge into one, with each slot in a range of programs. A warning counts notes
  that may play another instrument.
- A bank gets 64 MB. One that doesn't fit keeps only the instruments it selects. Waves in IMA-ADPCM are left out.
- Commands that the Wii skips are left out, and only AXFX effects go along.
- A sequence whose commands have to stay in place can't go yet.

#### From the GBA

The song becomes a MIDI file and a SoundFont, as File > Export > MIDI and SoundFont… writes them, and then a
NintendoWare song that opens as a timeline. The loop plays once, and a note held for ever before the loop lasts 200
beats. NintendoWare tempos are whole numbers. The song alternates between neighbouring tempos bar by bar to keep the GBA
timing. The reverb send goes to bus A without an effect. Add one to FX A.

#### To the DS

Every console can send sounds to the DS. The new file is a bare SDAT with one sequence, one bank and one wave archive.

- As for the Wii, the banks merge into one. NITRO-Composer always uses 48 ticks to a quarter note, and other timebases
  change their tempos. A tempo from a variable can't change.
- Sends, filters, and LFOs after the first, are left out. Calls and loops nest 3 deep at most. A sequence whose commands
  have to stay in place can't go yet.
- A key keeps only its widest velocity range.
- Waves become mono IMA-ADPCM at 32,728 Hz at most, and loops snap to 8 samples.
- The DS model has 4 MiB. A song that doesn't fit keeps only its instruments, then drops its waves to 24,000, 16,364,
  12,000 or 8,182 Hz. If it still doesn't fit, it can't go.
- The DS has no effects, and drops the lowest priority notes past 16 channels.
- A centred channel plays about 3 dB quieter. The 10-bit output leaves quiet tails at about -60 dBFS. The measure lists
  them as louder.

No game or emulator has played such a file.

#### Biquad filters

Every console uses the same biquad presets, made for 32 kHz. The Switch mixes at 48 kHz, and its types 6 to 10 are the
same filters made for 48 kHz. A song for the Switch gets types 6 to 10 in place of 1 to 5, and a song from the Switch
gets the reverse. Values of types 1 to 5 from the Switch take the nearest passband. A low-pass or high-pass keeps its
cutoff, but a band-pass ends up a little over half an octave lower. Filters from variables don't change.

### Single items

Right-click an item and choose Export…. Unedited items come out byte for byte. The Target menu offers:

| Item | Targets |
|---|---|
| Sequence | Its file, for each other console, MIDI with SoundFont 2 or SFZ |
| Bank | Its file, for each other console, SoundFont 2, SFZ |
| Instrument | SoundFont 2 or SFZ |
| Wave archive | Its file, for each other console, a folder of WAV files |
| Wave | Its file, for each other console, WAV |
| Wave sound | Its wave sound file, for each other console, WAV |
| Stream sound | Its stream file, for each other console, WAV |

- **Other consoles.** Files use the oldest version found in games of the target. An older game may refuse a newer
  version. A sequence with the extended LFO commands or the third send can't go to the 3DS.
- **The Wii.** Banks and wave sound files go out with their waves in a `.brwar` beside them. The Wii plays only two
  channels and no IMA-ADPCM. A Wii wave sound file goes to no other console yet.
- **WAV.** 16-bit PCM with the loop in a `smpl` chunk, named after the archive and index, such as `WARC_SE_007.wav`. A
  wave sound plays at its pitch. The window warns of a pan or envelope that the WAV file can't keep.
- **SoundFont 2 and SFZ.** Each instrument becomes a preset at its program, and program 130 becomes bank 1, preset 2.
  Instruments play centred and unfiltered, and CC1 sets the vibrato. SFZ files go into a folder named after the bank,
  with samples in `samples`. Choose the `sfz` folder of a sequence export to share its samples. A bank too large for one
  SoundFont splits into several, such as `BANK_GM_000-048.sf2`.
- **GBA.** Voice groups export as SoundFont 2 or assembly, and songs as MIDI with SoundFont 2 or as
  [assembly](#gba-songs-and-voices). With another of the GBA's drivers, Banks and Waves list the samples. They export
  as WAV files or as one SoundFont 2 file.
- **DSE.** Sounds export as MIDI with SoundFont 2 or SFZ, and songs also as their file (`.smd`). Banks export as their
  file, SoundFont 2 or SFZ, and waves as WAV.

## Saving

File > Save As… saves a `.hok` file, and [format.md](format.md) specifies the format. File > Open…, a drop, the command
line and a double-click in Finder open one. File > Save keeps the form that the file was opened in. Hokuto asks before
closing a file with unsaved changes. File > Recent Projects lists the last 10 files.

A `.hok` file keeps the group files beside the archive, and the view: position, cycle, zoom, selected and muted tracks,
lanes and open editors. A change of the view alone doesn't count as an unsaved change.

**Text form.** A `.hok` file is binary or text, and Hokuto opens both. In the text form, you can edit by hand:

- Sequences can gain or lose commands. Hokuto refuses a value that the console can't store, with its line number.
- The lines after `tables` set the values of each sound. A sound that starts at a label keeps starting there. A value
  that the entry of a sound leaves out gets a place only if NintendoWare laid out the archive.
- You can't add, remove or rename items, or change the kind of a sound.

#### The model form

The model form keeps the archive as items and files, without the original layout. An export lays the archive out the way
NintendoWare does, and writes every group file and stream beside it. It sounds the same, but the bytes can differ. Only
archives that NintendoWare laid out can use it, and GBA and DS files can't. In the text form, the model is base64 and
can't be edited by hand. New projects, `hok model` and AAC replacements use the model form.

## The MIDI files

File > Export > MIDI and SoundFont… and MIDI and SFZ… write a Standard MIDI File of format 1. GBA songs follow other
rules: a looping song plays its loop twice, with `loopStart` and `loopEnd` markers round one pass, and the events land
on the song's beat. Everything else works like this:

- **Timing.** A quarter note is at least 7,680 MIDI ticks. Bends and releases land where the game plays them.
- **First track.** Tempo changes, the title tag or the sound name, and the copyright tag. A looping song has `loopStart`
  and `loopEnd` markers round one pass, and CC111 = 0 at the loop start.
- **Channels.** Each track with notes gets a MIDI channel and a MIDI track. A track uses extra channels where notes
  overlap on a key, or earlier notes still sound with other settings, up to 16. Channel 10 comes last, and its presets
  are also in bank 128 of the SoundFont.
- **Controllers.** CC7 is the track volume. CC11 holds the rest of the level: volume above 127, second volume, main
  volume and sound volume. CC10 is pan, CC1 LFO depth, and CC91 the send to FX A. On the Wii, CC93 is the send to FX B.
  Other Switch sends go into the dry mix, and 3DS files have no sends. Pitch bend follows bends and sweeps, with RPN 0
  for the range. CC120 cuts notes. A filter that changes during a note uses CC102 with SoundFont 2, or CC103 and CC104
  with SFZ.
- **Instruments.** Settings that can't change mid-note, such as the envelope and the LFO type, go into the instrument,
  and each combination becomes a preset. A legato becomes a pitch bend of the sounding note.
- **One path.** MIDI has no variables. The file plays the path that the starting values and changes give, with random
  numbers from one performance.

## The hok tool

The build also makes `hok`, a command-line tool for `.hok` files: `build/hok` on the Mac, and `build\Release\hok.exe` on
Windows. It reads `.hok` files in either form.

```sh
hok import [--dol <main.dol> | --sdat <path>] <archive> <file.hok>
hok export [--filler] [--dsi-key <file>] [--game <folder>] <file.hok> <archive>
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

- **import** reads anything that File > Import > Sound Archive… opens, with the group files and streams beside it.
  `--dol` names the `main.dol` of a Wii game. Without it, `import` looks in the `sys` folder of the extracted disc.
  `--sdat` picks the file of a DS image to search, such as `sound/sound_data.sdat`.
- **export** writes the archive, ROM or image back, with changed group files and missing streams. An unchanged archive
  comes back byte for byte. `--filler` allows [filler runs](#gba-roms). Without it, `export` refuses a ROM that requires
  them, unless the `.hok` file allows them. A ROM with Brownie Brown's, Ubisoft Milan's or Quintet's driver takes no
  `--filler`. `--dsi-key` names the 64-byte DSi digest key file. Without it, the changed hashes become zeros.
  A changed group file goes where the game keeps it: into a copy of the SARC archive or the `userManagementFiles`
  folder that holds the game's file. `export` looks next to the new archive and next to the `.hok` file. `--game` names
  one more folder to look in, such as the folder of the game's archive. File > Export looks next to an archive that
  Hokuto imported, but a `.hok` file doesn't record where its archive came from.
- **text** and **binary** convert between the two forms. **model** writes the binary form with the archive in the [model
  form](#the-model-form).
- **lengths** measures how long each sequence sound plays before its fade. A sound plays with the effects of the file
  until it has looped twice, ends, or stays silent for 180 s, for 600 s at most. A looping sound then fades over 10 s,
  and an ending one gets 0.5 s of silence. 3DS sounds play without effects. An edit that can change a sound drops its
  lengths. `--threads` limits how many sounds run at once.
- **items**, **wav**, **sf2** and **sfz** write every item, wave, SoundFont or SFZ bank of an archive or `.hok` file
  into a folder. The files match what Export… writes. For a GBA ROM, `items` writes assembly and raw samples. With
  another of the GBA's drivers, it writes MIDI with SoundFont 2 and WAV samples. Of the GBA's drivers, `sfz` takes
  only Konami's. It writes a folder of SFZ files for each song, and one for all the samples, `All Samples`. A failure
  is reported, and `hok` exits with 1. When two names differ only in case, the second gets its index after its name.
- **other** writes a sequence sound into a new `.hok` file of another console. Give the sound by name or by number from
  0, or a GBA song as `Song 012`. The console is `3ds`, `ds`, `wii`, `wii_u` or `switch`. By default, sounds go to the
  Wii U, and Wii U sounds to the Switch. `mono` and `stereo` choose the wave format for the 3DS. `other` prints what
  changes.

## Keyboard shortcuts

On the Mac, use Command for Ctrl and Option for Alt.

| Command | Keys |
|---|---|
| New Project, Open | Ctrl+N, Ctrl+O |
| Save, Save As | Ctrl+S, Ctrl+Shift+S |
| Preferences, Quit | Ctrl+Comma, Ctrl+Q |
| Undo, Redo | Ctrl+Z, Ctrl+Shift+Z |
| Start and stop | Space |
| Start | Enter on the keypad |
| Stop (twice to go back to where playback started) | 0 on the keypad |
| Go to Project Start | Decimal point on the keypad, or Return |
| Go to the left or right locator | 1 or 2 on the keypad |
| Next or previous part start or end on the selected track | N or B |
| Rewind or forward a beat | - or + on the keypad |
| Cycle | / on the keypad |
| Set the left or right locator | Ctrl-click or Alt-click the ruler |
| Locators round the selected part | P |
| Locators round the intro or loop, or zoom to it | Double-click or Alt-double-click it in the marker row |
| Open a marker flag or tempo point in the List Editor | Double-click it |
| Tempo row | Ctrl+T |
| Project Setup | Shift+S |
| Zoom in or out | H or G, or Ctrl with the mouse wheel |
| Show the whole song | Shift+F |
| Taller or shorter tracks | Shift+H or Shift+G |
| Inspector, browser, mixer | Alt+I, Ctrl+P, F3 |
| Open or close the Key Editor | Ctrl+E, or double-click a part |
| Selection tool, pencil, eraser | 1, 8, 5 |
| Select all notes, or none | Ctrl+A, Escape |
| Move notes a semitone or an octave | Up or Down, with Shift for an octave |
| Delete notes, commands or a region | Delete or Backspace |
| Quantize | Q |
| Cut, copy, paste notes | Ctrl+X, Ctrl+C, Ctrl+V |
| Reset a fader, send or pan | Ctrl-click it |
| Reset a region value, effect parameter or variable | Ctrl-click its slider |
| Type an effect parameter or a variable | Double-click its slider |
| Close a window, or cancel a dialog | Escape, twice in a text field |
