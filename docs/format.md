# The .hok format

Version 1.1. This document specifies Hokuto's files: what they hold, their binary form and their text form. The code
in `src/format/` is the reference implementation.

A `.hok` file holds a sound archive in Hokuto's form. Its sequences are commands of one instruction set, with values in
units that mean the same on every console. The rest of the archive stays as it was. Every file has a binary form and a
text form with the same content. The binary form is compact and quick to read. The text form is for reading, for diffs
and for editing by hand. Each form converts to the other without loss. Both use the extension `.hok`.

Version 1.0 holds what the sound archives of the Switch and the Wii U (`.bfsar`) and of the 3DS (`.bcsar`) require: the
engine, the sequences, the archive that the file came from, the values of its tables, and the group files and streams
that the game keeps beside it. It also holds the effects on the consoles' aux buses, the scenes: the values that a
sound's variables take as a game sets them, the colours that the user gave tracks, the tags of the file and of its
sounds, and the program's view when the file was saved. It says which sequences the program edits as timelines. It keeps
the sizes of the game's archive and its groups too. An export measures its growth from them. The archive's banks and
waves stay in the file as bytes until the user edits them. The file then holds models of the edited ones. The rest of
the tables stays as bytes too. An archive imported and exported again without edits comes back byte for byte. After an
edit, each sequence takes the size that its commands give. The export moves the files after it, and writes the offsets
and sizes in the archive's tables again. It writes the values of the tables too, such as each sound's volume.

Version 1.0 also holds the sound archives of the Wii (`.brsar`): the engine, the sequences, the archive that the file
came from, the values of its tables that the Wii's sound library reads for playback, the streams that the game keeps
beside it, and the game's build of that library. The rest of the archive's tables stays in the file as bytes. An export
writes their offsets and sizes again after an edit, and the values in place.

Version 1.1 adds a GBA ROM whose songs MusicPlayer2000 (MP2K), Nintendo's sound driver for the GBA, plays. The ROM has
no archive: its songs, voice groups and samples are stored at addresses among the game's code and data. The file holds
the engine, each song as a sequence, and the rest of the ROM as bytes. An export writes each song in its place where it
fits. A song that outgrows its place goes into the ROM's free space, and the song table names it there.

A file can also hold a sound archive of the DS (`.sdat`). NITRO-Composer, the sound driver of the DS's SDK, plays its
sequences. The file holds the engine, each sequence file (SSEQ) as a sequence, and the rest of the archive as bytes. A
sequence archive (SSAR) stays among those bytes. An export writes the files' offsets and sizes again after an edit. A
file can also keep the DS's cartridge image (`.nds`) that holds the archive. Its export writes the new archive into
the image. The archive's sound table lists the sequences of its INFO block by their indexes. From index 65,536, or
after those sequences where the block holds more, it lists the sequences of each sequence archive in the order of the
archive's table of sequence archives, each in the order of its table. A new sequence at the end of the INFO block's
table therefore moves no sound. A sound's index in the chunks below is its place in that list. An entry that holds no
sequence keeps its place. A sequence archive whose file can't be read adds nothing to the list.

A file can also hold the sound files of a DS game on Procyon Studio's driver (Digital Sound Elements, DSE). Such a game
keeps no sound archive. Its songs (smdl), wave banks (swdl) and banks of sound effects (sedl) are files of the
cartridge's file system. The file holds the engine and those files as one set: the tracks of each song and each sound
effect as a sequence, and the rest of the files as bytes. The file keeps the cartridge image (`.nds`) too. Its export
writes each changed file into the image.

A file can also hold a GBA ROM with Konami's sound driver. As with MP2K, the file holds the engine, the tracks of each
song as a sequence and the rest of the ROM as bytes. Each song has up to 16 tracks, each with a channel of its own, and
the driver reads no tempo. The song table names each song. A song that fits stays in its place, and a song that outgrows
its place moves. The song table then names the new place.

A file can hold a GBA ROM with Nintendo R&D2's sound driver in the same way. A song starts with a header that names up
to 10 tracks, and each track plays any of the driver's voices. A song sets its tempo. The tables of sequences name each
song's sequence. A song that outgrows its place moves, and the table then names the new place.

A file can hold a GBA ROM with Brownie Brown's sound driver in the same way. A sound is a list of the driver's channels,
and sounds share the bytes that their lists name. A sequence therefore holds a region of the ROM that a group of sounds
read. The song table names each sound's list. A region that outgrows its place moves, and the song table then names the
lists at the new place.

A file can contain a GBA ROM with Ubisoft Milan's sound driver in the same way. A sequence contains one track of
commands for the 16 channels of the driver, and each command names its channel. The driver reads no tempo. The resource
table of the sound bank points to each sequence. A sequence that outgrows its place moves, and the table then points to
the new place.

A file can contain a GBA ROM with Rare's sound driver in the same way. A tune's header names up to 16 tracks, and each
command of a track names one of 16 MIDI channels. The tune table names each tune's header, and the header's track list
names each track's start. A tune that outgrows its place moves. Its track list then names the new place.

A file can contain a GBA ROM with Quintet's sound driver in the same way. A song's header gives its length and the start
of each of its 6 channels. A music file contains its songs back to back. A song that grows takes the free bytes after
the last song, up to the end of the file's part.

A file can contain a GBA ROM with Krawall, a player of XM and S3M modules, in the same way. The patterns of a module are
stored together, and the table of patterns in its header names the place of each pattern. Patterns that outgrow their
place move, and the table then names the new places.

A file of any console but the GBA and the DS can hold its archive as a model instead: the items of the archive's tables
with their values, and its files, without the game's layout. The sounds' volumes, pans and sends are in Hokuto's units
there. A sequence sound starts at an item of its sequence. A reader lays the archive out from the model as
NintendoWare's tools lay one out. The archive plays the same as the game's. Its bytes can differ.

## What a file holds

- **The engine and the mixing mode.** The engine names a console. Hokuto plays the file's sounds by that console's
  rules. There are fourteen engines: the Switch, the Wii U, the 3DS, the Wii, the GBA, the DS, DSE on the DS, and
  Konami's driver, Nintendo R&D2's driver, Brownie Brown's driver, Ubisoft Milan's driver, Rare's driver, Quintet's
  driver and Krawall on the GBA. A sound can play on another engine than the file's. Hokuto then converts the sound for
  that console and plays the result by that console's rules. An export to that console makes the same conversion. The
  archive keeps the sound for the file's engine. A sound of the Wii goes to the Wii U first, and from there to the
  Switch or the 3DS. A sound of the 3DS, the Wii U or the Switch goes to the Wii directly, for a build of NW4R SND from
  June 2008. A sound of the Wii, the GBA, one of the GBA's other drivers or DSE goes to the DS through the Wii U. The
  mixing mode is strict or enhanced. Strict mixing follows the console's hardware, and enhanced mixing lifts its limits.
  Each sound takes the file's mode, unless the file gives the sound the other mode. The Wii's engine plays in strict
  mixing only.
- **The effects.** The effects on the console's aux buses A to C, like the effect plugins on a DAW's FX channels. A
  track's `send` commands set its levels into the buses. A bus's effects process it in order, and the bus then joins the
  output at its volume. On the Switch, a bus without effects joins the output as it is. On the Wii U, the Wii and the
  3DS, a bus reaches the output only through its effects. The AX libraries of the Wii U and the Wii drop the sends to a
  bus without one. The 3DS's nn::snd leaves such a bus off. The 3DS has buses A and B only. The format has sixteen
  kinds of effect: the I3DL2 reverb of the Switch's audio library, the Wii U's high reverb, standard reverb, chorus and
  delay, six effects of the Wii and five of the 3DS. A file may hold an effect of any console. A player plays only the
  effects of the console whose engine plays the sound. A sound archive holds no effects and no volumes of the buses. A
  game's code sets them up. An imported archive starts without effects, with each bus at full volume.
- **Scenes.** A game changes its music through a sound's variables. A scene names the values that a sound's variables
  start with, such as those of a race's last lap. It can also change a variable at a set time while the sound plays. A
  game does that when the last lap starts. A sound can have any number of scenes. A sound archive holds none. A game's
  code sets the variables.
- **The tracks' colours.** The colour that the user gave a track of a sound, for the program's views. A sound archive
  holds none. A player ignores them.
- **Instrument maps.** A song of a GBA ROM plays the voices of a voice group, a table of up to 128 instruments in the
  ROM. A MIDI file's notes play programs instead, on MIDI's drum channel or on another channel. An instrument map pairs
  each program of a MIDI file with a voice of a voice group. An import of a MIDI file into a song pairs each program
  that the file plays with a voice. The file keeps the pairings in the map of the song's voice group, and the next
  import into a song of that voice group proposes them. On the other consoles, a map belongs to a bank. It pairs each
  program of a MIDI file with a program of the bank, and each note of MIDI's drum channel with a program of the bank and
  a key. An archive and a ROM hold no maps. A player and an export ignore them.
- **The view.** What the program showed when the file was saved: a sound, the transport's position and cycle, the
  arrangement's zoom and scroll, its selected track and the tracks that are muted and soloed, the Key Editor's zoom,
  scroll and controller lanes, and the tracks that the open editors show. The file opens as it was saved. A sound
  archive holds none. A player and an export ignore it.
- **Timelines.** The sequences that the program edits as timelines, like the MIDI files that NintendoWare's MIDI
  converter turned into them. The program edits each other sequence as a program, with its subroutines and labels. A
  timeline's commands don't show what the converter read from the MIDI file: its bar lines, the bars before the song's
  start, and the bars whose treatment the converter's rules don't explain. The file keeps them as the timeline's
  hints. With them, Hokuto lays the song out as the converter does. A file without a word on timelines leaves the
  choice to the program. A player and an export ignore them.
- **Tags.** The file's tags and each sound's, such as a title, an artist or a game. A tag is a key and a value. The tags
  of PSF files have keys of the same names. A sound archive holds none. A player and an export ignore them.
- **Lengths.** How long a player plays each sound before its fade, and how long the fade lasts. A sound that loops plays
  its loop a number of times first. Hokuto measures the lengths on the model of the file's engine. The rips of the PSF
  family measure theirs the same way. The tags `length` and `fade` go before them. A sound archive holds none.
- **Sequences.** A sequence is a list of items in the order of the console's bytes: commands, and runs of bytes that no
  command reaches. Labels name the items where sounds start. Jumps, calls and track starts name their targets by item.
- **The native archive.** The archive that the file came from, cut into pieces. A piece is a run of the archive's
  bytes, a sequence file, or padding. A sequence file names one of the file's sequences, with the file's version.
  Padding aligns the piece after it. Each field of the archive's tables that holds an offset or a size is a distance
  between two places in the pieces, such as a file's start and its end, or a sequence's first item and the item where
  a sound starts. An export joins the pieces in order, encodes each sequence in the console's format, and writes each
  field. A file without a native archive or a model holds no archive to export.
- **The encoder's choices.** Some details of a console's bytes go beyond the commands' meaning. A value can take more
  bytes than it requires, or a prefix can stand before a command that ignores it. The encoder's choices record these
  details for each command. With them, an export gives back the archive's bytes. They belong to the native archive,
  with the sequence file of their sequence. A sequence that no sequence file names has none.
- **The tables.** The values of the native archive's tables: each sound's name, kind, player, volume, pan, priorities,
  banks and tracks, the names of the banks, wave archives and groups, and each player's name and sound limit. They are
  the console's values, in the console's units. An export writes them into the archive.
- **The game's sizes.** The size of the game's archive when Hokuto imported it, and the size of the files that each of
  its groups loads. A game loads each group into its sound heap, and the game's archive fits the heap. An export
  measures how much the archive and its groups grow from these sizes. Edits don't change them.
- **Group files.** A game can keep some of an archive's groups in files beside the archive. A group file holds the files
  of its group, such as banks and wave archives. The native archive cuts each group file into pieces and fields in the
  same way as it cuts the archive. A copy there of one of the archive's sequence files is a sequence file of the same
  sequence, and the edits of the sequence reach it. A sequence file that only group files hold is one of the file's
  sequences too. Each group file keeps its group and the size and the SHA-1 of the game's file. An export writes each
  group file whose bytes differ from the game's. It goes where the game keeps the group's file: beside the archive under
  the group's name, in a folder there, or in a SARC archive there. A player reads the group files with the archive that
  an export builds.
- **Streams.** A game keeps its streams in files beside the archive, at the paths that the archive's file table gives.
  The native archive keeps each stream whole, by its file's ID in the table. It holds no path. A stream that Hokuto
  made or changed is marked. An export writes each marked stream at the path that the table gives, beside the exported
  archive. The game holds the others already. On the 3DS, a stream sound can play an AAC file in place of a stream
  file: AAC-LC frames in ADTS. The sound's entry then marks the stream as AAC in its extension, and gives the loop's
  start and end in the decoder's samples, from the file's first frame on.
- **The model.** A file can hold its archive as a model in place of the native archive. The model holds each item of
  the archive's tables with its values: the sounds, the sound groups, the banks, the wave archives, the groups and the
  players. It also holds the settings of the archive's player, each file of the file table, the group files beside the
  archive and the streams. A sound's volume, pan and sends are in Hokuto's units. A sequence sound whose file holds one
  of the file's sequences starts at an item of the sequence. The file then takes its bytes from the sequence. The model
  keeps none of the encoder's choices and nothing of the archive's layout. A reader lays the archive out from the model
  as NintendoWare's tools lay one out. The edits work on that archive. An export writes it with every group file and
  every stream beside it. The file keeps no game's sizes. A file whose archive is a model is in the model form. It
  holds no native archive. A model of a Wii archive holds the sounds, the banks, the players, the groups and the files,
  and the game's build of the Wii's sound library. The Wii's archives hold no sound groups and no wave archives. A
  file of the Wii keeps no group files. It keeps the streams beside the archive in the model too.
- **Banks and waves.** The banks and the waves that the user edited, in Hokuto's models of them. A bank's model holds
  the bank's instruments by program, each instrument's key regions and their velocity regions, each region's values,
  and the bank's table of waves. A wave's model holds the wave's encoding, rate, loop and length. A wave from a WAV
  file, or one that took another encoding, also holds the samples that its encoded samples come from, as PCM16. The
  archive's bytes hold what the models lay out: each copy of a bank's file holds the bank, and each wave archive holds
  the waves. An unedited bank or wave has no model and stays as the game's bytes. So does an item whose file the user
  replaced whole.
- **Other chunks.** Hokuto keeps each chunk that it doesn't read, and writes it back unchanged.

### Commands

Each command has a kind. The table of kinds below gives the parts that each kind has:

- **index**: the track, bus, LFO or variable that the command addresses, from 0.
- **key and velocity**: a note's key, numbered as in MIDI (60 is middle C), and its velocity, a level.
- **value**: the command's argument, in the unit of its kind. A note's value is its length.
- **target**: the item that a jump, a call or a track start goes to, by its number in the sequence, from 0. The number
  of items stands for the end of the sequence. A target can instead be a byte offset past the end of the sequence's
  bytes. No item starts there.
- **opcode**: an `other` command's opcode: a byte, or 0xF000 plus the byte after an 0xF0 byte.
- **bytes**: a `data` item's bytes.

Any command can also have a condition and a time. A command with a condition runs only if the last comparison held. A
time is the time, in ticks, over which a setting moves to its value.

A value and a time are arguments. An argument comes from one of three sources:

| Source | Number | The argument |
|---|---|---|
| value | 0 | its value |
| random | 1 | a random value from its value to its highest value |
| variable | 2 | the value of the variable whose number is its value |

A command can hold a part that its kind doesn't have, such as an index on a volume command. Only an unusual sequence
holds one. Both forms keep it.

The kinds follow NintendoWare's sequence commands. Each kind's number and name never change. A new kind takes a new
number.

| Number | Name | Unit of the value | Other parts | Opcode |
|---|---|---|---|---|
| 1 | `note` | ticks | key, velocity | the key |
| 2 | `wait` | ticks |  | 0x80 |
| 3 | `program` | number |  | 0x81 |
| 4 | `open_track` | none | index, target | 0x88 |
| 5 | `jump` | none | target | 0x89 |
| 6 | `call` | none | target | 0x8A |
| 7 | `return` | none |  | 0xFD |
| 8 | `loop_start` | number |  | 0xD4 |
| 9 | `loop_end` | none |  | 0xFC |
| 10 | `end` | none |  | 0xFF |
| 11 | `allocate_tracks` | number |  | 0xFE |
| 12 | `timebase` | number |  | 0xB0 |
| 13 | `envelope_hold` | number |  | 0xB1 |
| 14 | `monophonic` | number |  | 0xB2 |
| 15 | `velocity_range` | number |  | 0xB3 |
| 16 | `biquad_type` | number |  | 0xB4 |
| 17 | `biquad_value` | number |  | 0xB5 |
| 18 | `bank_select` | number |  | 0xB6 |
| 19 | `front_bypass` | number |  | 0xBF |
| 20 | `pan` | pan |  | 0xC0 |
| 21 | `volume` | level |  | 0xC1 |
| 22 | `main_volume` | level |  | 0xC2 |
| 23 | `transpose` | cents |  | 0xC3 |
| 24 | `pitch_bend` | bend |  | 0xC4 |
| 25 | `bend_range` | cents |  | 0xC5 |
| 26 | `priority` | number |  | 0xC6 |
| 27 | `note_wait` | number |  | 0xC7 |
| 28 | `tie` | number |  | 0xC8 |
| 29 | `portamento_from` | number |  | 0xC9 |
| 30 | `portamento` | number |  | 0xCE |
| 31 | `portamento_time` | number |  | 0xCF |
| 32 | `attack` | number |  | 0xD0 |
| 33 | `decay` | number |  | 0xD1 |
| 34 | `sustain` | number |  | 0xD2 |
| 35 | `release` | number |  | 0xD3 |
| 36 | `envelope_reset` | none |  | 0xFB |
| 37 | `volume2` | level |  | 0xD5 |
| 38 | `print_variable` | number |  | 0xD6 |
| 39 | `surround_pan` | pan |  | 0xD7 |
| 40 | `low_pass` | fraction |  | 0xD8 |
| 41 | `send` | send | index | 0xD9, 0xDA, 0xDE |
| 42 | `main_send` | send |  | 0xDB |
| 43 | `initial_pan` | pan |  | 0xDC |
| 44 | `mute` | number |  | 0xDD |
| 45 | `damper` | number |  | 0xDF |
| 46 | `tempo` | bpm |  | 0xE1 |
| 47 | `sweep_pitch` | cents |  | 0xE3 |
| 48 | `lfo_curve` | number | index | 0xBE |
| 49 | `lfo_phase` | number | index | 0xBD |
| 50 | `lfo_depth` | fraction | index | 0xCA |
| 51 | `lfo_speed` | hz | index | 0xCB |
| 52 | `lfo_type` | number | index | 0xCC |
| 53 | `lfo_range` | number | index | 0xCD |
| 54 | `lfo_delay` | ms | index | 0xE0 |
| 55 | `lfo_period` | ms | index | 0xE4 |
| 56 | `set_variable` | number | index | 0xF0 0x80 |
| 57 | `add_variable` | number | index | 0xF0 0x81 |
| 58 | `subtract_variable` | number | index | 0xF0 0x82 |
| 59 | `multiply_variable` | number | index | 0xF0 0x83 |
| 60 | `divide_variable` | number | index | 0xF0 0x84 |
| 61 | `shift_variable` | number | index | 0xF0 0x85 |
| 62 | `random_variable` | number | index | 0xF0 0x86 |
| 63 | `and_variable` | number | index | 0xF0 0x87 |
| 64 | `or_variable` | number | index | 0xF0 0x88 |
| 65 | `xor_variable` | number | index | 0xF0 0x89 |
| 66 | `not_variable` | number | index | 0xF0 0x8A |
| 67 | `modulo_variable` | number | index | 0xF0 0x8B |
| 68 | `compare_equal` | number | index | 0xF0 0x90 |
| 69 | `compare_greater_or_equal` | number | index | 0xF0 0x91 |
| 70 | `compare_greater` | number | index | 0xF0 0x92 |
| 71 | `compare_less_or_equal` | number | index | 0xF0 0x93 |
| 72 | `compare_less` | number | index | 0xF0 0x94 |
| 73 | `compare_not_equal` | number | index | 0xF0 0x95 |
| 74 | `user_procedure` | number |  | 0xF0 0xE0 |
| 75 | `other` | number | opcode | its opcode |
| 76 | `data` | none | bytes | none |
| 77 | `note_off` | none | key | 0xCE |
| 78 | `repeat` | number | target | 0xB5 |
| 79 | `jump_if_equal` | number | index, target | 0xB9 6 |
| 80 | `jump_if_not_equal` | number | index, target | 0xB9 7 |
| 81 | `jump_if_greater` | number | index, target | 0xB9 8 |
| 82 | `jump_if_greater_or_equal` | number | index, target | 0xB9 9 |
| 83 | `jump_if_less_or_equal` | number | index, target | 0xB9 10 |
| 84 | `jump_if_less` | number | index, target | 0xB9 11 |
| 85 | `tune` | cents |  | 0xC8 |
| 86 | `register` | number | index | 0xCC |
| 87 | `sample_wave` | number |  | 0xCD 0x01 |
| 88 | `voice_type` | number |  | 0xCD 0x02 |
| 89 | `echo_volume` | number |  | 0xCD 0x08 |
| 90 | `echo_length` | number |  | 0xCD 0x09 |
| 91 | `psg_length` | number |  | 0xCD 0x0A |
| 92 | `sweep` | number |  | 0xCD 0x0B |
| 93 | `sample_offset` | number |  | 0xCD 0x0D |

The opcodes are those of the consoles' bytecode, NintendoWare's sequence format. A note's opcode is its key, from 0x00
to 0x7F. Prefixes come before an opcode, in this order: 0xA2 for a condition, then 0xA3, 0xA4 or 0xA5 for a time with a
value, a random value or a variable, then 0xA0 for a random argument or 0xA1 for an argument from a variable. A time of
16 bits follows the command's argument. 0xF0 starts an opcode of two bytes. A `send` takes 0xD9 for bus A, 0xDA for bus
B and 0xDE for bus C. The table gives the opcodes of LFO 0. For LFO n from 1 to 3, an opcode is 0xF0 and a second byte.
The second byte is 0xA0 + 6 (n − 1) for `lfo_curve`. The next five bytes are for `lfo_phase`, `lfo_depth`, `lfo_speed`,
`lfo_type` and `lfo_range`. The second byte is 0xE1 + 2 (n − 1) for `lfo_delay`, and one more for `lfo_period`. The
engines differ in the commands that they run (see "The engines").

Kinds 77 to 93 are MP2K's, and the table gives their bytes on the GBA. A `note_off` ends the newest note of its key
that has length 0: MP2K's end of a tie. A `repeat` goes back to its target until it has run its value's times in all,
or for ever at 0. A conditional jump goes to its target when byte n of MP2K's memory area, n being its index, compares
with its value as its name says. A `register` writes its value to the sound register n bytes past the first. The
others set a voice's sample by its address, its type, the echo after a PSG note's release, a PSG voice's length and
sweep, and the point in the sample where the track's notes start. Each of these commands does nothing on the other
engines, and the others' encoders refuse them.

On the GBA, the kinds that NintendoWare shares take MP2K's opcodes: 0xCF to 0xFF for a `note`, 0xCF for one of length
0, 0x80 to 0xB0 for a `wait`, 0xB1 for an `end`, 0xB2 for a `jump`, 0xB3 for a `call` and 0xB4 for a `return`. 0xBA
to 0xC5 give `priority`, `tempo`, `transpose`, `program`, `volume`, `pan`, `pitch_bend`, `bend_range`, `lfo_speed`,
`lfo_delay`, `lfo_depth` and `lfo_type`. 0xCD and 0x04 to 0x07 give `attack`, `decay`, `sustain` and `release`, and
0xCD 0x0C a `wait` of 16 bits. 0xB9 with operations 0 to 5 gives `set_variable`, `add_variable` and
`subtract_variable`, with a value or from a variable. A byte below 0x80 repeats the track's last command from 0xBD
up, with that byte as its first argument.

On the DS, the kinds take NITRO-Composer's opcodes. A `note`'s opcode is its key, from 0x00 to 0x7F, and its velocity
and its length follow. 0x80 gives a `wait`, 0x81 a `program`, 0x93 an `open_track`, 0x94 a `jump` and 0x95 a `call`. A
target takes 3 bytes, little-endian. 0xB0 to 0xB6 give the kinds from `set_variable` to `random_variable`, and 0xB8 to
0xBD the kinds from `compare_equal` to `compare_not_equal`. 0xC0 to 0xD7 give `pan`, `volume`, `main_volume`,
`transpose`, `pitch_bend`, `bend_range`, `priority`, `note_wait`, `tie`, `portamento_from`, `lfo_depth`, `lfo_speed`,
`lfo_type`, `lfo_range`, `portamento`, `portamento_time`, `attack`, `decay`, `sustain`, `release`, `loop_start`,
`volume2`, `print_variable` and `mute`. 0xE0 gives an `lfo_delay`, 0xE1 a `tempo` and 0xE3 a `sweep_pitch`. 0xFC, 0xFD
and 0xFF give `loop_end`, `return` and `end`. 0xFE gives `allocate_tracks`, at a sequence's start only. The DS has one
LFO. A note's length, a wait and a program take a variable-length number: 7 bits in each byte, with the high bit set in
every byte but the last. The opcodes from 0xB0 to 0xBF and from 0xE0 to 0xEF take 16 bits, little-endian. Those from
0xC0 to 0xDF take a byte. Prefixes come before an opcode, in this order: 0xA2 for a condition, then 0xA0 for a random
value or 0xA1 for a value from a variable. They give the source of a note's length and of the value of the opcodes
from 0x80 to 0x8F and from 0xB0 to 0xEF. The other opcodes from 0x80 up are `other` commands.

## Values and units

A value is a signed 64-bit fixed-point number with 16 fraction bits: 65,536 is 1. A variable's number is a whole
number, not fixed point. A key and an index are whole numbers from 0 to 255.

The table of kinds gives the unit of each kind's value. A note's velocity is a level, and a time is in ticks.

| Unit | Meaning |
|---|---|
| `number` | a count, a number, or a setting as the Switch's sound library stores it |
| `ticks` | ticks of the sequence's timebase |
| `level` | a level on MIDI's volume curve: a gain of (level / 127)². 127 is 0 dB |
| `send` | a send's level: a gain of send / 127 |
| `cents` | a pitch in cents |
| `bend` | a fraction of the bend range |
| `pan` | a position in 63rds: from the centre for a pan, from the front for a surround pan |
| `bpm` | beats a minute |
| `ms` | milliseconds |
| `hz` | hertz |
| `fraction` | a fraction of the setting's range |

Each engine converts its console's values to the units, and back. Where a quantity has no usual unit, its unit takes
the Switch's scale. Every Switch value then converts exactly. A Switch value v stays v in its unit, except in these
kinds:

| Kinds | Unit | The value in the unit |
|---|---|---|
| `pan`, `initial_pan` | pan | v − 64 |
| `transpose`, `bend_range` | cents | 100 v |
| `sweep_pitch` | cents | 100 v / 64 |
| `pitch_bend` | bend | v / 128 |
| `low_pass` | fraction | v / 64 − 1 |
| `lfo_depth` | fraction | v / 128 |
| `lfo_speed` | hz | 100 v / 256 |
| `lfo_delay` | ms | 5 v |
| `lfo_period` | ms | 10 v |

The other consoles' values take the same scales. The Wii's builds of NW4R SND before 20080308 differ in one: they take a
sweep's pitch in 32nds of a semitone. Their `sweep_pitch` is 100 v / 32 cents.

MP2K's values take other scales. Its `tempo` holds half the BPM. Its value is therefore 2 v BPM. Its `pan` and
`pitch_bend` take 0x40 off, and its bend is in 64ths of the range: v − 64 and (v − 64) / 64. Its `transpose` and
`bend_range` are in semitones: 100 v cents. Its `tune` is in 64ths of a semitone, with 0x40 the centre: 100 (v − 64) /
64 cents. The LFO's values stay in the driver's units: its speed in 256ths of a cycle each tick, its delay in ticks and
its depth in the driver's steps. A song counts 24 ticks to a quarter note.

The DS's values take the other consoles' scales, but for the LFO's. Its `transpose` and `pitch_bend` are signed bytes.
An LFO's depth is in steps of 127/16,384 of its range: 127 v / 16384. Its speed is in 128ths of a cycle each 4 frames
of the driver: about 0.37491 v Hz. Its delay is in frames of the driver: about 5.20953 v ms.

## The engines

An engine plays a file's sounds by the rules of one console's sound library. The table gives each engine's library and
sizes. Every engine plays 16 tracks, 0 to 15.

| | Switch | Wii U | 3DS | Wii |
|---|---|---|---|---|
| sound library | nn::atk | nw::snd | nw::snd | NW4R SND |
| frame | 5 ms | 3 ms | 160 samples at 268,111,856 / 8,192 Hz: about 4.889 ms | 3 ms |
| 16- and 24-bit arguments | little-endian | big-endian | big-endian | big-endian |
| entries of a track's stack | 10 | 10 | 3 | 3 |
| LFOs of a track | 4 | 4 | 1 | 1 |
| aux buses | A to C | A to C | A and B | A to C |
| commands of a track in a tick | 10,000 | 10,000 | 10,000 | 100,000 |

The Switch and the Wii U run every command. The 3DS and the Wii lack the commands below. Some of them depend on the
build of NW4R SND. BULD gives a file's build. On an engine that lacks a command, the command does nothing. Its prefixes
and arguments still take their bytes.

| Opcodes | Commands | The engines that lack them |
|---|---|---|
| 0xA3 to 0xA5 | the time prefixes | the Wii's builds before 20080308 |
| 0xB1 and 0xB2 | `envelope_hold` and `monophonic` | the Wii's builds before 20080308 |
| 0xB3 to 0xB5, and 0xFB | `velocity_range`, `biquad_type`, `biquad_value` and `envelope_reset` | the Wii's builds before 20080620 |
| 0xB6 and 0xBF | `bank_select` and `front_bypass` | the Wii |
| 0xBD, 0xBE and 0xE4 | `lfo_phase`, `lfo_curve` and `lfo_period` of LFO 0 | the 3DS and the Wii |
| 0xDE | `send` to bus C | the 3DS |
| 0xF0 0xA0 to 0xF0 0xBF, and 0xF0 0xE1 to 0xF0 0xE6 | the settings of LFOs 1 to 3 | the 3DS and the Wii |

On the 3DS and the Wii, the opcodes from 0xF0 0xA0 to 0xF0 0xBF take no argument. Such a command is an `other` command.
On the Wii's builds before 20080308, 0xA3 to 0xA5 are commands of one byte, not prefixes. They are `other` commands too,
and no command there has a time.

On the Switch, the Wii U and the 3DS, 0xA1 after 0xA0 is a command that does nothing. A false condition skips an `end`
too. On the Wii, both prefixes apply, and the argument comes from the variable. An `end` there ends the track after a
false condition too.

A tick runs a track's commands until one waits. After the number of commands that the table gives, the tick ends. The
track then reads on in the next tick. On the Wii, the track ends there instead. NW4R SND hangs on a track that loops
without a wait.

A track's stack holds its calls and its loops. A call or a loop start on a full stack does nothing. A loop start of n
plays its loop n times, and a loop start of 0 loops for ever. On the Switch, the Wii U and the 3DS, a return goes back
after the newest call on the stack. It drops the loops above that call. Without a call on the stack, the return drops
every loop, and the track runs on past it. A loop end on a call's entry does nothing. On the Wii, a return and a loop
end take the stack's top entry, whether a call or a loop start put it there. With an empty stack, the track runs on past
a return. An `open_track` clears the opened track's wait, its stack and its wait for notes to end. On the Wii, the
opened track keeps them.

Variables 0 to 15 are the sound's player variables, and 16 to 31 the global variables that every sound shares. Variables
32 to 47 are the track's variables. Each variable starts at −1 until a game or a command sets it. On the Wii, the global
variables start at 0.

A random argument takes low + (((high − low + 1) × r) >> 16) in 32-bit arithmetic. Here r is the next number of the
engine's generator. The generator sets x to x × 0x19660D + 0x3C6EF35F, modulo 2³², from x = 0x12345678. Each number is
the top 16 bits of the new x. After each frame's commands, the engine draws one more number and drops it. On the 3DS,
the generator starts 17 numbers on.

A tempo is held to 0 to 1,023. The Wii's builds before 20080308 keep its 16 bits instead: a variable at −1 gives 65,535.
A track's `attack`, `decay`, `sustain`, `release` and `envelope_hold` replace the bank's values at a note's start when
they lie from 0 to 127. On the Wii's builds before 20080308, any value but 255 replaces them.

Six settings move to their values over a command's time: `pan`, `volume`, `main_volume`, `pitch_bend`, `volume2` and
`surround_pan`. Every other command ignores a time. On the 3DS and the Wii, `main_volume` and `volume2` take their
values at once. So does `pitch_bend` on the Wii's builds before 20090924.

The Wii's builds before 20080308 ignore a region's hold, key group and note-off type. From 20081120, a low-pass takes
its cutoff from the Wii U's table. Before 20090924, a voice's volume and its envelope's gain are held to full level. On
every build, a sound's volume above 127 plays at 127. A sound of the Wii has one bank slot.

The build of October 2006 differs in two more ways. Its clock counts in 416ths of a tick. Each frame first takes the
whole ticks out of the count, then adds the tempo, then runs the ticks that it took out. The timebase plays no part. The
build also raises a `pan` or an `initial_pan` of −64 or −63 by 1, and a `surround_pan` of 0 or 1.

The GBA's engine plays a song as MP2K does. Each frame of the GBA's display, about 59.73 Hz, adds the tempo to a counter,
and each 150 in the counter is a tick. A song plays up to 16 tracks, as many as its header gives. A track's stack holds
3 calls. A `repeat` keeps its count apart from the stack. The variables are the 256 bytes of MP2K's memory area.
`set_variable` and the conditional jumps address them. They start at 0, and a game's code can set them. MP2K draws no
random numbers.

In DSE's files, a sequence holds the track chunks of a song or a sound effect, in the order of their bytes. Each track
starts with a `data` item of 20 bytes: the chunk's header and the track's preamble. An encoder writes the chunk's size
into its copy of those bytes, and pads the chunk to a multiple of 4 bytes with 0x98. A code below 0x80 gives a `note`:
the code is its velocity, and the byte after it holds the number of bytes of its length, its change of octave and its
key's place in the octave. A note can leave its length out and take the length before it. A `note` holds its key and its
length, and an encoder works out the bytes from the state before the note. An octave that a note's change can't reach
puts 0xA0 before the note. 0x80 to 0x8F give a `wait` of a length from the driver's table. 0x90 gives a `wait` of the
length before it, and 0x91 changes that length by a signed byte. 0x92, 0x93 and 0x94 give a `wait` with its length in 1,
2 or 3 bytes, little-endian. 0x98 gives an `end`. 0x9C gives the `loop_start` of a repeat, and 0x9D its `loop_end`. 0xA4
and 0xA5 give a `tempo`, 0xAC a `program`, 0xDB a `bend_range`, 0xE0 a `volume`, 0xE3 a `volume2` and 0xE8 a `pan`. The
other codes from 0x90 up are `other` commands with their bytes, 0x99 among them: a track's end goes back to its last
0x99. An `end` can stand in the chunk's padding, with a code that the driver takes as invalid. A `data` item after a
track's end holds bytes that the chunk's size counts and the driver never reads. Those forms come back as they were
read. An encoder refuses other kinds, conditions, times and values from random numbers or variables.

DSE's values take the other consoles' scales. Its `pan` is the byte less 64.

Konami's engine plays a song as the driver does, in the revision of the file's ROM (see NATV). The revisions differ in
their commands, their tracks and their mixer. A song's tracks are 0 to 15, and a revision can have fewer. Tracks 0 to 3
play the GBA's PSG channels: two squares, the wave and the noise. Track t + 4 plays the driver's mixer voice t. A
track's number therefore names its channel, and a song always holds all its tracks. The mixer's output runs at 65,536
Hz. A revision without a mixer plays its samples through a FIFO for each timer, at 32,768 Hz. One tick is one frame of
the GBA's display, about 59.73 Hz, and a `wait` of n lasts n ticks. A song holds no `tempo` and no variables, and the
driver draws no random numbers.

A track's bytes are a delay and then, in turn, a command and a delay. A delay becomes a `wait`, and a delay of 0 is a
`wait` of 0. A command becomes an item of its kind. Bytes that no track reaches stay in `data` items.

- **`note`:** its velocity holds the command's volume, on the scale of 65,536 for a volume of 1. Its length stays 0: a
  note sounds until the track's next note or its next `note_off`. A `note_off` is a rest. A note of a PSG track holds
  its key. A note of the mixer holds its sample in the part that bit 12 of an item's flags gives (`[sample n]` in the
  text form), and the sample's change in semitones as a signed byte in its key. Where the next track plays the note, its
  index is 1.
- **`program`:** index 0 holds a PSG track's duty byte, or a wave in the driver's first revision. Index 1 holds a wave,
  and index 2 an entry of the table of instruments that the game gives the driver.
- **`volume`:** index 0 and index 1 hold the two forms of the command. Index 2 holds a volume of the track and, in its
  velocity, a volume of the next track. A volume starts a PSG note again in some revisions.
- **`release`:** index 0 ends a note, and indexes 1 and 2 fade this track and the next.
- **`pan`:** in the revisions whose byte holds a position, the byte less 64. In the others, the byte holds a left level
  and a right level. The value is the right less the left, in steps of 63/15, and the velocity holds the larger level. A
  PSG track off centre plays its right side on its channel, and its left side on the next channel, in place of the next
  track.
- **`loop_start`:** the track's loop point. Its bytes hold what the driver skips after it. A `loop_end` is a loop of the
  whole song: every track goes back to its loop point, or to its start.
- **`end`:** index 0 ends the track until the song loops, index 1 for good, and index 2 stops the song.
- **`call`:** its value counts the commands to read from its target, and its index names the track whose start the
  target counts from. Its key holds the opcode. A target starts with a delay. The driver reads no delay after the last
  command of a call. The delay after the call plays instead.
- **`lfo_depth`, `pitch_bend`, `attack`, `decay`, `echo_volume`, `echo_length`, `mute` and `main_volume`:** the command's
  byte, on the driver's scale. An index of a `pitch_bend` names one of the forms of the command.
- **`other`:** the opcode and its bytes: a command without another kind, such as a change of the echo's route.

An `opcode` of 0x100 plus a byte keeps a command's bytes: the byte starts them, and the item's bytes hold the rest. A
reader uses them only where they read as the item's values, such as a delay in a longer form than its length requires.
After an edit that changes the values, the encoder writes the usual bytes. The encoder refuses other kinds and
conditions, times and values from random numbers or variables. It also refuses a track that would read a delay at the
bytes of a command, or a command at the bytes of a delay.

Nintendo R&D2's engine plays a song as the driver does, in the revision of the file's ROM (see NATV). A sequence starts
with a header. A sequence of music holds a count of tracks and a byte of 0, and then 2 bytes for each track: the track's
start, counted from the sequence's start. The count and the byte of 0 stay a `data` item of 2 bytes. A sequence of sound
effects holds only the starts. Each start is an `open_track` item with an `opcode` of 0x100 and the track's number in
its index. A start of 0 names no track: its item has no target. A song of the music plays tracks 0 to 9, and a sound
effect plays one track of a sequence of sound effects. A track plays any of the driver's voices: the PSG channels and
the voices of the driver's mixer. The mixer's output runs at 65,536 Hz. A `tempo` of 150 plays a tick in each frame of
the GBA's display, about 59.73 Hz. A song holds no variables, and the driver draws no random numbers.

Each command becomes an item of its kind. Bytes that no track reaches stay in `data` items.

- **`note`:** opcodes 0x00 to 0xBF. A note of 0x60 to 0xBF holds its key less 0x60, its length and its velocity. A note
  of 0x00 to 0x5F holds its key, and takes its length and its velocity from the track's last note that gave them. The
  velocity stays in the driver's units, 0 to 255.
- **`wait`:** 0xC1 holds its length, and 0xC0 takes the track's last wait.
- **`program`, `pan`, `priority`, `bank_select`, `volume`, `pitch_bend`, `bend_range`, `send`, `tempo`, `lfo_delay`,
  `lfo_speed`, `lfo_depth`, `transpose` and `main_volume`:** 0xC2, 0xC3, 0xC4, 0xC7, 0xE0, 0xE1, 0xE2, 0xE3, 0xE4,
  0xE5, 0xE6, 0xE7, 0xE9 and 0xEA. A `pan` takes a position from the centre at 64, a `pitch_bend` a fraction of the
  bend's range, and a `transpose` and a `bend_range` cents. The other values stay in the driver's units. A `send` is the
  track's send to the driver's echo. A `main_volume` is the player's volume. A `tempo` takes a length's bytes. In one
  revision, a `tempo` takes a byte, and 0xEA has no operand: it is an `other` item.
- **`tie`:** 0xC5 turns legato on, with a value of 1, and 0xC6 turns it off. **`note_wait`:** 0xC8 makes notes wait for
  their lengths, with a value of 1, and 0xC9 stops it.
- **`portamento_from`:** 0xD0 to 0xDF, a slide for the next note. Its index holds the opcode's low digit, its key the
  other note, its value the slide's length in 256ths of the note's, and its velocity the delay in frames where the digit
  is odd. **`portamento`:** 0xE8, no slide for the next note.
- **`user_procedure`:** 0xCA, with a value for the game's routine.
- **`jump`, `call` and `open_track`:** 0xF0, 0xF4 and 0xF8, with a target counted from the sequence's start. An
  `open_track` starts the track that its index names.
- **`return` and `end`:** 0xFF, a `return` where a track reaches it inside a call, and an `end` outside every call.
- **`other`:** the other opcodes from 0xC2 up. They do nothing and take no operand.

A length takes a byte, or two bytes with the first byte's top bit set. The choices of a sequence piece keep a length in
two bytes that one byte would hold (`value_bytes` 2), and a note or a wait of one byte (`repeated`). The encoder refuses
other kinds, conditions, times and values from random numbers or variables, and a target more than 0xFFFF bytes from
the sequence's start.

Brownie Brown's engine plays a sound as the driver does, in the revision of the file's ROM (see NATV). A sound is a
list of the driver's channels, and a channel's commands can call and jump to bytes that other sounds read. A sequence
therefore holds a region of the ROM: the bytes from the first to the last that a group of sounds read, where no other
sound reads a byte. A region can hold one sound or hundreds. Labels name the items where the sounds' lists start.
Channels 0 to 3 play the music on the PSG's square 1, square 2, wave and noise channels. Channels 4 to 7 play sound
effects on the same PSG channels, and take each one from the music while they sound. The other channels play samples:
channels 8 to 12 on the voices of the driver's mixer in revision 0, and channels 8 to 11 on the GBA's two FIFOs in
revision 1. There, channels 10 and 11 play a sound effect's samples, and take a FIFO from channel 8 or 9. The driver
counts time in frames of the GBA's display, about 59.73 Hz, and a tick is a frame. A sound holds no tempo and no
variables, and the driver draws no random numbers.

Each list, each command that a channel reaches and each word of a table that a command reads becomes an item. Bytes that
nothing reads stay in `data` items. A word starts on a multiple of 4 bytes in the ROM. The encoder puts zeros before a
word that would start elsewhere.

- **A list:** for each channel, an `open_track` item and then a `jump` with an `opcode` of 0x200. The `open_track`'s
  index is the channel, and its target is the item where the channel's data starts. The `jump`'s target is the channel's
  table. An `end` with an index of 1 is the word of 0xFF that ends the list. A channel's data can start at that word.
  The first byte of the word reads as 0xFF.
- **A table:** a run of `jump` items with an `opcode` of 0x200, each a word with the address of an item. A word whose
  address names no item of the sequence sets bit 9 of its flags, and its target holds the address.
- **`note`:** its key holds the byte: a key before the channel's transpose, or a noise note's setting. A note holds no
  velocity. 0xF2 sets a channel's volume.
- **A length byte:** 0x80 to 0xDF, an `other` item with the byte in its `opcode`. It chooses the channel's length from
  the driver's table of lengths. In revision 0, 0xEE chooses the set of 16 lengths that the byte reads. A length of 0 in
  the table lasts 256 ticks.
- **`wait`, `note_off` and `release`:** 0xFE is a `wait`: the note goes on. 0xEC is a sample channel's rest, a
  `note_off` with an index of 0, and 0xED a PSG channel's, with an index of 1. 0xE0 is a `release`: the envelope goes on
  from the release envelope in its value.
- **`program`:** index 0 is 0xE1's sample set, or in revision 1 its sample. Index 1 is 0xF0's envelope, index 2 0xF1's
  byte for the PSG channel's NRx1 register, and index 3 0xFC's wave.
- **`pan`:** index 0 is 0xEA's byte, with the right side's level in the low nibble and the left's in the high. Index 1
  is 0xFD's byte, with the PSG channel's NR51 bits for the right side in the low nibble and for the left in the high.
- **`volume`:** 0xF2's byte: the level that a PSG channel's envelope loses, or a sample channel's volume.
- **`transpose`:** 0xF3's signed byte, with 100 cents for each step of the byte.
- **`tune`:** 0xF9's signed byte: an addition to a PSG channel's frequency setting, or in revision 1 12ths of a
  semitone on a sample channel.
- **`sweep`:** index 0 is 0xFA, with the PSG's NR10 byte. Index 1 is 0xFB, the command that turns the sweep off.
- **`loop_start` and `loop_end`:** 0xF4 with its count, where 0 counts 256, and 0xF5.
- **`jump` and `call`:** 0xF8 and 0xF6. The index names the word of the channel's table, and the target is the item
  that the word names. **`return`:** 0xF7. **`end`:** 0xFF, with an index of 0.
- **`other`:** the opcode and its bytes. A length byte is one. So are revision 0's 0xE2 (the mixer's mode), 0xE3 (the
  sound comes back after a fade), 0xE8 and 0xE9 (a sound effect gives the music's PSG channel back, or takes it) and
  0xEE (the set of lengths), and each command that does nothing.

A `note`, a `wait`, a `note_off` and a `release` each wait for the channel's length. Their bytes hold no number of
ticks, and a channel that reads them with another length waits for another time. A file gives each such item the whole
ticks that every channel that reads it waits there: a `release` in its time, and the others in their value. An item
holds 0 where its channels wait for other lengths, or where no channel reaches it. The encoder ignores those ticks: the
length bytes give them. Hokuto's edits put length bytes in where an item's ticks change.

In revision 1, 0xE2 to 0xE9 read as 0xF2 to 0xF9, and 0xEF as 0xFF. An item that a channel reads through such a byte
holds 0x100 plus the byte in its `opcode`, and the encoder writes the byte. The encoder refuses other kinds, conditions,
times and values from random numbers or variables. It also refuses a sequence that the driver wouldn't read as its
items say: a channel that reaches a word or data, a channel that runs past the items, and a call or a jump that finds
another target in its table.

Ubisoft Milan's engine plays a piece of music as the driver does. A piece plays a list of sequences one after another,
and a piece that loops starts the list again after the last sequence. A sequence contains one track of commands for 16
channels. The index of each command is its channel. Channels 0 to 2 play square 1, square 2 and the wave channel of the
PSG. Channel 9 plays the noise of the PSG at keys up to 11, and the samples of the kit at higher keys, on the mixer of
the driver. The other channels make no sound. The mixer plays 4 voices at the rate that the game sets, and both FIFOs of
the GBA play the same samples. One tick is one frame of the GBA display, about 59.73 Hz. A piece has no `tempo` and no
variables, and the driver draws no random numbers.

A sequence starts with 4 bytes in a `data` item: the kind of resource and three bytes that the driver doesn't read. A
`wait` follows. After that, each command has a `wait` after it, up to the `end`. The bytes contain no jumps or calls.

- **`wait`:** a length in ticks. The bytes store a countdown of 2 bytes, low byte first. The first wait of a sequence
  stores the countdown n as it is. The sequence that starts a piece runs its first command n ticks in. A sequence that
  follows another runs the first command n + 1 ticks after the end of the sequence before it, or in the same tick for a
  countdown of 0. Any other countdown of n is a wait of n + 1 ticks, and a countdown of 0 is a wait of 0 ticks. No
  countdown stores a wait of 1 tick after a command, and the encoder refuses one. A wait lasts at most 65,536 ticks, and
  a first wait at most 65,535.
- **`note`:** 0x90, with the key and the velocity in the units of the driver, 0 to 255. The length stays 0. A note
  sounds until a `note_off` of the same key on the same channel, or until the sample ends. A note of the kit sounds
  until any `note_off` on channel 9. A later note on a PSG channel ends the note before it.
- **`note_off`:** 0x80, with the key, and in the velocity a second byte that the driver doesn't read.
- **`program`:** 0xC0, an instrument of a PSG channel, or the offset of the keys of channel 9 into the kit.
- **`other`:** 0xE0, a command without an effect.
- **`end`:** 0xFC, the end of the sequence.

The encoder refuses other kinds, conditions, times and values from random numbers or variables, and a command without a
`wait` after it.

Rare's engine plays a tune as the driver does, with the traits of the file's build of the driver (see NATV). A tune's
header names its tracks, and each track runs from its start to its end, or to its loop's end after its loop's start. The
tune's tracks become one sequence, from the first track's start to the last byte that a track reads. A tune's ticks run
at the tempo of its tempo commands, with the header's ticks to a quarter note. The driver mixes its notes into Direct
Sound A, at the rate that the driver's init sets. The hardware's output runs at 65,536 Hz. A tune holds no variables,
and the driver draws no random numbers.

Each command becomes an item of its kind, with its channel in its index. In the build that names the channel in the
command byte's high nibble, the index of a command without a channel holds the nibble that the driver ignores. Bytes
that no track reaches stay in `data` items.

- **`tempo`:** command 0, microseconds a quarter note in 3 bytes.
- **`wait`:** commands 1, 2 and 3, a delay in ticks in 1, 2 or 3 bytes.
- **`note`:** commands 5 and 4, with a key and a velocity up to 127, and a length of 0. A note of command 4 holds 4 in
  its `opcode`. The driver plays both the same.
- **`note_off`:** command 6. It ends the channel's first note of its key that sounds.
- **`program` and `pitch_bend`:** commands 8 and 10. A `pitch_bend` takes a fraction of the instrument's bend range.
- **`end`:** command 11.
- **`lfo_depth`, `volume`, `attack`, `decay`, `sustain`, `release`, `loop_start`, `loop_end` and `monophonic`:**
  controllers 1, 7, 20 to 23, 102, 103, 126 and 127. Controllers 20 to 23 set a channel's envelope in the build that
  reads them. A `loop_start` and a `loop_end` hold a value of 0: the loop runs for ever. A `monophonic` turns the mode
  on with a value of 1 (126), and off with a value of 0 (127).
- **`other`:** command 9, a channel pressure that the driver ignores, and command 12, a command that does nothing, with
  the command in `opcode`. Another controller, with 0x100 plus the controller in `opcode`.

The second byte of a note-off, a channel pressure and controllers 102, 103, 126 and 127 does nothing. The choices of a
sequence piece keep it where it isn't 0 (`arguments`), and a delay in more bytes than it requires (`value_bytes`). The
encoder refuses other kinds, conditions, times and values from random numbers or variables.

Quintet's engine plays a song as the driver does, in the revision of the file's ROM (see NATV). A song has 6 channels:
two squares, the wave channel, the noise channel and FIFOs A and B. Each channel runs from its start to its end (FF).
That command ends the channel or goes back to the channel's loop point. A whole note lasts 384 of the driver's ticks,
and a channel's tempo command sets the ticks that each frame plays. The driver plays the FIFOs as Direct Sound A and B,
and the hardware's output runs at the rate that the driver's init sets. A song holds no variables.

Each command becomes an item of its kind: the item that ntdlib's decoder gives. A `note`, a `note_off` and a `wait` hold
their length in the performer's ticks: 37 for each of the driver's, and 3,552 a quarter note. The other values stay in
the driver's units. Bytes that no channel reaches stay in `data` items. A dot (D1, or 9B in the revision of notes of two
bytes) right after a note, a rest or a wait adds half its length again, rounded down to the driver's ticks. The file
folds such a dot into the item before it: the item's `value` holds the whole length, and its `opcode` holds 0xD1. A
dotted length that no length gives with half of it again is refused. A tie (D4) followed by a note of the same pitch
adds that note's length to the note before the tie, and a tie followed by a rest does the same after a rest. A tie stays
a `tie` item.

- **`note`:** a note, with its pitch from 0 to 11 in `key` and its length in `value`. `index` 1 plays the last note
  again, or on a FIFO the sample at its rate.
- **`note_off`:** a rest, with its length.
- **`wait`:** in the revision of notes of two bytes, a note of another pitch that only waits, with its byte in `key`.
- **`tempo`:** 7F.
- **`volume`:** `index` 0, 5E, a channel's volume. `index` 1, F1, a signed step of the volume.
- **`transpose`:** `index` 0, E4, and `index` 1, E9, the second transpose. Signed.
- **`tune`:** `index` 0, D5, a signed byte, and `index` 1, E6, a signed halfword.
- **`program`:** `index` 0, DC, or DD with DD in `opcode`, a FIFO's sample. The driver plays both the same. `index` 1,
  DB, the noise macro. `index` 2, DA, the wave.
- **`tie`:** D4.
- **`loop_start` and `loop_end`:** D6 and D7, a repeat. A `loop_end` holds its passes, from 1 to 256. The byte 0 makes
  256 passes.
- **`end`:** FF.
- **`other`:** every other command, with its number in `opcode` and its arguments in `bytes`. In the revision of notes
  of two bytes, a byte that the driver skips takes 0x100 plus the byte in `opcode`.

A note's and a rest's length is a note value: 1/n of a whole note. The choices of a sequence piece keep the code of a
note value where it isn't the lowest code that gives the length (`length_code`). The encoder refuses other kinds,
conditions, times, values from random numbers or variables, and a length that isn't a whole number of the driver's
ticks or a note value of the driver.

Krawall's engine plays a piece as Krawall plays it, in the revision of the file's ROM (see NATV). A sequence holds a
module's patterns, back to back, each padded with bytes of 0 to a multiple of 4 bytes from the sequence's start. A
pattern starts with an index of every 4th row and its count of rows. Its rows follow, and a row lists the channels that
it has data for. The player plays each row for the ticks of the module's speed, and a tick lasts as long as the
module's tempo sets. A piece plays the module's orders from the first, or from the first order of a song that the
orders separate. Each of the module's first 16 channels is a track. A piece holds no variables, and the player draws no
random numbers.

Each item of a pattern takes a kind:

- **`other` with `opcode` 0x200:** the pattern's start. The encoder works the index and the count of rows out from the
  rows. In a pattern of fewer than 64 rows, the index's entries past the rows are spare: `bytes` holds them where they
  aren't 0.
- **`note`:** a note, with its number from 1 for C-0 in `key` and its instrument in `[sample n]`, from 1, or 0 for none.
  A note has no velocity or length: both hold 0.
- **`note_off`:** a note off, with its instrument in `[sample n]`, or 0 for none.
- **`program`:** an instrument without a note, in `value`.
- **`volume`:** the volume column's byte, in the module's codes.
- **`other` with `opcode` 0x100 plus an effect's number:** an effect, with its operand in `bytes`.
- **`wait`:** the end of `value` rows: the last row that has items, and the empty rows after it.

Each item but a wait and a pattern's start names its channel in `index`, from 0 to 31. A row's items of one channel make
one entry of the row, in the order note, volume and effect. A pattern's count of rows is the sum of its waits. Revision
1, for the builds from 2004/09/17 on, counts a pattern's rows in a halfword and takes an instrument of up to 16 bits.
Revision 0 counts them in a byte and takes an instrument of up to 9 bits. In revision 1, an instrument above 255 takes a
third byte, and the choices of a sequence piece keep a third byte of 0 (`value_bytes` 3). The encoder refuses other
kinds, conditions, times, values from random numbers or variables, a channel past 31 and a pattern of more rows than its
count holds.

The DS's engine plays a sequence as NITRO-Composer's driver plays it on the DS's ARM7. The driver runs a frame every
2,728 ticks of the ARM7's timer, each 64 clocks of the system's 33,513,982 Hz: about 5.2095 ms. Each frame adds the
tempo to a counter, and each 240 in the counter is a tick. A sequence plays up to 16 tracks. A track's stack holds 3
calls. Variables 0 to 15 belong to the sequence's player, and 16 to 31 to every player. They start at −1, and a game's
code can set them. The DS's sound unit mixes 16 channels. The DS's bytecode holds only the kinds that its opcodes give
(see "Commands"), and its encoder refuses the others.

## The mixing modes

Each sound plays in one of two mixing modes. Strict mixing follows the console's hardware and system software. Enhanced
mixing keeps what the console's sound library determines: which voices play, how fast each voice steps through its wave,
and each voice's envelope, volumes, pans, filters and sends. It keeps the effects' algorithms too. It lifts the limits
of the hardware's arithmetic and of its voice count. The rules below fix the enhanced mode. Every platform renders the
same samples in both modes. ntdlib's models of the consoles are the reference implementation.

The engines of the Switch, the Wii U and the 3DS follow these rules in enhanced mixing:

- **Voices.** Where the console drops a voice because every voice is in use, the enhanced mode takes another voice, up
  to 1,024 in all.
- **Arithmetic.** Samples, gains and filters' states are single-precision floating-point numbers (IEEE 754 binary32)
  on the scale of 16-bit samples. Each operation rounds to the nearest value, without fused multiply-adds, and takes its
  operands in a fixed order. Nothing holds a sample to 16 bits or a sum to 32 bits.
- **The interpolator.** A voice's samples reach the output's rate through a windowed sinc in place of the console's
  filters. The kernel is a sinc under a Kaiser window with a beta of 8. At a step of one input sample to each output
  sample or less, an output takes the 32 inputs on each side of its place. The cutoff then lies at 0.92 of the input's
  Nyquist frequency. A larger step rounds up to the next eighth of an octave, up to a step of 8. The kernel's half
  width is then 32 times the step, rounded up to a multiple of 4, and 128 at most. Its cutoff is the smaller of 0.92 /
  step and 1 / step − 2.56 / half width, as a share of the input's Nyquist frequency. A table holds the kernel's weights
  at 257 places from one input sample to the next, 256ths of a sample apart. Each place's weights are scaled to sum to
  1. An output's weights lie in proportion between the two places on each side of its place. Product k of an output
  goes into sum k mod 8 of eight sums, and the output is ((s0 + s1) + (s2 + s3)) + ((s4 + s5) + (s6 + s7)). The
  reference implementation works out the weights in double precision, without a library's mathematical functions.
- **Gains.** A gain that changes ramps across a frame from its value at the end of the frame before to its value at
  the end of this one. Sample i of a frame of n samples takes from + i × ((to − from) / n). A voice that starts takes
  its gains at once.
- **The output.** The output is the sum of the mixes divided by 32,768, without a limit. In 16 bits, each sample rounds
  to the nearest integer, halves away from zero, and stays between −32,768 and 32,767.

On the Switch:

- The renderer runs at 48 kHz in frames of 240 samples. So does the console's.
- A voice steps through its wave buffers with the console's ratio and batches. Output n of a voice lies at the voice's
  position in its input, less 3 samples at the default quality, 5 at high quality and 4 at low quality. The console's
  filters take their outputs at those places. Without a conversion, the outputs at the default quality all take the
  first output's place within its sample, one input sample apart. The console's filter does the same.
- A biquad filter works on the voice's samples in floating point, without the console's conversions around it.
- A voice that stops leaves the last product of each of its mixes to its destination's depop. Each sample, a depop
  multiplies by 31,529 / 32,768 at 48 kHz, or by 30,923 / 32,768 at another rate, and adds to its buffer. A depop below
  2⁻¹⁶ at the end of a frame becomes 0.
- An aux bus's effect takes each sample rounded to the nearest integer, halves away from zero, and held to 32 bits. Its
  output comes back as it is.

On the Wii U:

- AX renders at 48 kHz in frames of 144 samples. The console renders 96 samples at 32 kHz and converts its output to
  48 kHz.
- A voice reads its samples at the console's steps. nw::snd works out each voice's ratio for 48 kHz. An output lies
  between the voice's third and second newest samples with the 4-tap filters, and between the fourth and third newest
  with linear interpolation. A voice without a conversion steps two thirds of a sample at a time from its newest
  sample. Its samples then play at 32 kHz. The console plays them at that rate too. The interpolator's later inputs are
  the samples that the voice reads next. A voice that stops gives zeros after its end.
- The low-pass filter takes its a0 as an unsigned 1.15 number and its b0 as a signed one. The biquad filter takes its
  coefficients as 2.14 numbers. nw::snd's biquad presets take its tables for 48 kHz.
- nw::snd sets each volume without a delta. The gains then ramp by the rule above.
- As on the console, the aux buses' effects run at 32 kHz in frames of 96 samples. The interpolator takes a bus's
  sends down to 32 kHz. Input sample k of a frame lies 48 + 1.5 k samples after the start of the sends of the frame
  before. Each sample rounds to the nearest integer, halves away from zero, and is held to 32 bits. The interpolator
  takes the effect's output back up to 48 kHz, 288 samples after the sends. That delay of two frames matches the
  console. The return's volume ramps across the frame in equal steps, and the last sample takes the new volume. A
  channel whose sends of the last three frames are all 0 gives the effect zeros. A channel whose returns of the last two
  frames are all 0 adds nothing.
- No conversion follows the mix. A remix matrix works in floating point.

On the 3DS:

- The DSP's firmware mixes 160 samples in each frame, at 268,111,856 / 8,192 Hz, about 32,728 Hz. The enhanced mode
  mixes 235 samples in each of the firmware's frames, at about 48,070 Hz. A frame lasts as long in both modes.
- In strict mixing, nn::snd drops a voice below the highest priority when the DSP's frame has no cycles left for it.
  The enhanced mode drops no voice for the cycles.
- A voice steps through its wave buffers in the firmware's steps, with the firmware's decoders. Its phase p and its
  step r for each of the console's samples count in 2⁻²⁴ samples. Output m of a frame lies (47 p + 32 m r) / 47 of
  those units after the third sample before the frame's first new sample, rounded towards zero. The kernel takes 16
  bits of the place's fraction. The interpolator's later inputs are the samples that the voice decodes next. A voice
  that stops gives zeros after its end.
- The biquad filter takes its coefficients as 2.14 numbers, and the one-pole filter its b0 and a1 as 1.15 numbers. The
  biquad filter comes first. nw::snd works out the one-pole filter's coefficients for 48 kHz. Its biquad presets take
  the library's tables for 48 kHz.
- A gain ramps by the rule above where the firmware ramps it: in a voice that played in the frame before, or whose
  parameters set the ramp. Elsewhere each sample takes the new gain.
- The voices' sends mix into the two aux buses in floating point, in frames of 235 samples. At the end of each frame,
  the bus's effects take its samples as 32-bit integers on the scale of the firmware's sums. Each sample rounds to the
  nearest integer, halves away from zero. The bus joins the main bus at its return volume two frames after its mix.
  The firmware adds it at the same time.
- The effects keep the libraries' frames: each buffer holds as many frames as in strict mixing, of 235 samples each.
  Their times therefore stay the same. They work out their coefficients from their buffers' lengths at 32,728 × 235 /
  160 Hz, or 48,069.25 Hz. The libraries do the same at 32,728 Hz. The comb filters' gains then match those of strict
  mixing in all but 1 of the 4,000,200 pairs of a fused time and a filter's size that the limits allow. The damping
  filters keep their coefficients, and their cutoffs move with the rate.
- The output is the main bus, with its rear channels at the rear ratio, in mono or in stereo, at the master volume. The
  surround mode mixes as in stereo. The declick of a voice that stops adds nothing. A voice of two channels gives
  nothing either. The firmware's mono and stereo outputs leave it out too.

On the Wii:

- The Wii takes strict mixing only. A file of the Wii can give the enhanced mode in ENGN. Its sounds still play in
  strict mixing. So does a sound of another console on the Wii's engine.
- AX's microcode mixes 96 samples in each frame of 3 ms, at 32 kHz. The engine mixes as the microcode of AX's build of
  December 2006 does. With BULD's flag, it mixes as the microcode of October 2006 does.

On the DS:

- The driver runs a frame every 174,592 clocks of the system's 33,513,982 Hz, about 5.2095 ms. In strict mixing, the
  sound unit makes a sample every 1,024 clocks, at 32,728.5 Hz: 170 or 171 samples in a frame. Each sample holds every
  channel's sample at its end, without interpolation. The mixer works in fixed point, adds a bias and clips the sum to
  10 bits. In enhanced mixing, the engine makes 250 samples in each frame, at about 47,989 Hz.
- **Channels.** Where the console's driver would take a channel that plays, or would give a note no channel for its
  priority, the enhanced mode takes a channel past the console's 16 instead, up to 1,024 in all. The new channel plays
  as the channel that the console's driver chose: a wave, a rectangular wave or noise. Only the sequences' notes take
  such channels.
- **The interpolator.** A channel that plays PCM or IMA-ADPCM gives the windowed sinc above its wave's samples, its
  loops included. The interpolator takes them at the channel's place: the sample that plays, and the share of the
  timer's period that has passed. A rectangular wave and noise change their levels at each overflow of their timers,
  through band-limited steps. The steps run 32 samples late, and the waves pass through a delay of as many samples. The
  output therefore runs 32 samples behind the console's timing.
- **Gains.** A channel's volume, divider and pan, and the master volume, give gains in floating point. A gain that
  changes ramps across the frame's 250 samples. A channel that starts takes its gains at once.
- **The output.** Nothing adds the bias, cuts the fixed point's bits or clips the sum. The output is the sum on the
  scale of 16-bit samples divided by 32,768, without a limit. A channel that stops, and a one-shot wave at its end,
  ring out through the interpolator.

## The binary form

Numbers are little-endian. A file starts with a header and a table of its chunks. The chunks follow.

### The header

| Offset | Size | Contents |
|---|---|---|
| 0 | 8 | the signature: 89 48 4F 4B 0D 0A 1A 0A |
| 8 | 2 | the major version: 1 |
| 10 | 2 | the minor version: 1 |
| 12 | 4 | flags: 0 |
| 16 | 4 | the number of chunks |
| 20 | 4 | 0 |

The signature works like PNG's. A transfer that changes line endings or clears the top bit of each byte also changes
the signature.

The table of chunks follows the header, with 24 bytes for each chunk:

| Offset | Size | Contents |
|---|---|---|
| 0 | 4 | the chunk's ID |
| 4 | 4 | the CRC-32 of the chunk's bytes |
| 8 | 8 | where the chunk starts, from the start of the file |
| 16 | 8 | the chunk's size in bytes |

Each chunk starts on a multiple of 8 bytes. Zero bytes fill the gaps. Hokuto writes its chunks in the order ENGN, MIXR,
SEQS, SNDS, ITMS, NATV, BULD, TABL, SIZE, GRPF, STRM, CART, BANK, WAVE, SCEN, TCOL, IMAP, BMAP, EDTR, TAGS, LNTH and TMLN,
then the other chunks in the order of the file that they came from. The CRC-32 is zlib's and PNG's: the reflected
polynomial 0xEDB88320, starting from 0xFFFFFFFF, with the result inverted. The CRC-32 of the ASCII text `123456789` is
0xCBF43926.

A reader refuses a file:

- whose major version isn't 1
- with a chunk that lies past the end of the file, or whose bytes don't match its CRC-32
- with two chunks of the same ID among ENGN, MIXR, SEQS, SNDS, ITMS, NATV, BULD, TABL, SIZE, GRPF, STRM, CART, BANK,
  WAVE, SCEN, TCOL, IMAP, BMAP, EDTR, TAGS, LNTH and TMLN
- with a TABL, SIZE, GRPF or STRM chunk and no NATV chunk
- with a BULD chunk and no NATV chunk of the Wii's kind
- with a CART chunk and no NATV chunk of the DS's kinds
- with a NATV chunk whose kind of archive isn't the engine of its ENGN chunk
- with only one of SNDS and ITMS, or with them and a NATV chunk
- with a BANK or WAVE chunk and no archive: neither a NATV chunk nor SNDS and ITMS
- without an ENGN chunk

MIXR, SEQS, SNDS, ITMS, NATV, BULD, TABL, SIZE, GRPF, STRM, CART, BANK, WAVE, SCEN, TCOL, IMAP, BMAP, EDTR, TAGS, LNTH
and TMLN are optional. Hokuto keeps a chunk with any other ID and writes it back unchanged.

### ENGN: the engine

| Size | Contents |
|---|---|
| 1 | the engine: 1 for the Switch, 2 for the Wii U, 3 for the 3DS, 4 for the Wii, 5 for the GBA, 6 for the DS, 7 for DSE, 8 for Konami's driver, 9 for Nintendo R&D2's driver, 10 for Brownie Brown's driver, 11 for Ubisoft Milan's driver, 12 for Rare's driver, 13 for Quintet's driver, 14 for Krawall |
| 1 | the mixing mode: 0 for strict, 1 for enhanced |
| 2 | 0 |
| 4 | the number of sounds in another mode |
| 8 | for each sound: its index in the archive's sound table in 4 bytes, its mode in 1, and 3 zeros |
| 4 | the number of sounds on another engine |
| 8 | for each sound: its index in the archive's sound table in 4 bytes, its engine in 1, and 3 zeros |

Each sound takes the file's mode unless the chunk lists it. Hokuto writes the number and the sounds only when a sound
takes the other mode, or when a sound plays on another engine, in the order of the sounds' indexes. A chunk of 4 bytes
lists no sounds. Each sound plays on the file's engine unless the second list holds it. Hokuto writes that list only
when a sound plays on another engine, in the order of the sounds' indexes. A chunk without it plays every sound on the
file's engine. A reader refuses a sound twice in a list. It also refuses a mode or an engine that this specification
doesn't define, with a message that a later version of Hokuto made the file. The mode or the engine of a sound that
isn't in the archive does nothing. No sound of another console plays on the GBA's engine, DSE's, Konami's, Nintendo
R&D2's, Brownie Brown's, Ubisoft Milan's, Rare's, Quintet's or Krawall's: a reader refuses engines 5, 7, 8, 9, 10, 11,
12, 13 and 14 in the second list.

### MIXR: the mixer

The chunk holds the effects on each aux bus: for bus A, then B, then C, the number of the bus's effects in 4 bytes, and
the effects in the order that they process the bus. The volumes of buses A, B and C follow bus C's effects, each as a
32-bit float. Hokuto writes the volumes only when a bus's volume isn't 1, and the chunk only when a bus has an effect or
such a volume.

| Size | Contents |
|---|---|
| 1 | the kind of effect |
| 3 | 0 |
| 4 | the number of parameters |
| 4 for each | a parameter: a 32-bit float |

The kinds:

| Kind | Effect | Parameters |
|---|---|---|
| 1 | the Switch's I3DL2 reverb | 12 |
| 2 | the Wii U's high reverb | 11 |
| 3 | the Wii U's standard reverb | 10 |
| 4 | the Wii U's chorus | 5 |
| 5 | the Wii U's delay | 5 |
| 6 | the Wii's high reverb | 6 |
| 7 | the Wii's high reverb of 2008 | 11 |
| 8 | the Wii's chorus | 3 |
| 9 | the Wii's delay | 3 |
| 10 | the Wii's standard reverb | 5 |
| 11 | the Wii's standard reverb of 2008 | 10 |
| 12 | the 3DS's reverb of nw::snd | 12 |
| 13 | the 3DS's delay of nw::snd | 4 |
| 14 | the 3DS's reverb of nn::snd | 11 |
| 15 | the 3DS's delay of nn::snd | 4 |
| 16 | the 3DS's reverb of nn::snd of 2011 | 10 |

The I3DL2 reverb has 12 parameters. They're the fields of nn::audio's `I3dl2ReverbParameterSet`, in its order and its
units:

| Parameter | Name | Unit |
|---|---|---|
| 0 | `room_gain` | millibels |
| 1 | `room_hf_gain` | millibels |
| 2 | `late_reverb_decay_time` | seconds |
| 3 | `late_reverb_hf_decay_ratio` | a ratio |
| 4 | `reflections_gain` | millibels |
| 5 | `reflections_delay` | seconds |
| 6 | `reverb_gain` | millibels |
| 7 | `late_reverb_delay` | seconds |
| 8 | `late_reverb_diffusion` | percent |
| 9 | `late_reverb_density` | percent |
| 10 | `hf_reference` | hertz |
| 11 | `dry_gain` | a gain |

The Wii U's effects are those of the system library's AXFX. NintendoWare's classes `FxReverbHi`, `FxReverbStd`,
`FxChorus` and `FxDelay` set them up. Their parameters are the fields of each class's parameter structure, in its order
and its units. The high reverb's parameters:

| Parameter | Name | Unit |
|---|---|---|
| 0 | `pre_delay_time` | seconds |
| 1 | `fused_time` | seconds |
| 2 | `coloration` | a factor from 0 to 1 |
| 3 | `damping` | a factor from 0 to 1 |
| 4 | `crosstalk` | a factor from 0 to 1 |
| 5 | `out_gain` | a gain from 0 to 1 |
| 6 | `early_mode` | a mode from 0 to 7 |
| 7 | `pre_delay_time_max` | seconds |
| 8 | `fused_mode` | a mode from 0 to 5 |
| 9 | `early_gain` | a gain from 0 to 1 |
| 10 | `fused_gain` | a gain from 0 to 1 |

The standard reverb has the same parameters in the same order, without `crosstalk`. Its `out_gain` is parameter 4,
and its `fused_gain` parameter 9. The chorus's parameters are `delay_time` in milliseconds, `depth` (a factor from 0
to 1), `rate` in hertz, `feedback` (a factor below 1) and `out_gain` (a gain from 0 to 1). The delay's are `delay` in
milliseconds, `feedback` (a factor below 1), `out_gain` (a gain from 0 to 1), `max_delay` in milliseconds and `lpf` (a
factor from 0 to 1).

A mode is a whole number. The early mode sets the delay of the reverb's first early reflection, at the 32 kHz of the
console's renderer: 5 ms for mode 0, and 5 ms more for each mode after it, up to 40 ms for mode 7. The fused mode sets
the room that the reverb's delay lines are sized for: 0 for the library's older reverb, 1 for a metal tank, 2 for a
small room, 3 for a large room, 4 for a hall and 5 for a cavern. The library limits some values as it sets an effect up.
It holds each factor and gain between 0 and 1, and each feedback at most 0.99. It holds the chorus's delay between 0.1
ms and 50 ms and its rate between 0.1 Hz and 2 Hz. A pre-delay longer than its maximum takes the maximum. So does a
delay longer than its `max_delay`. The library leaves out an effect whose lines it can't allocate. A player follows the
library.

The Wii's effects are those of the SDK's AXFX library. NW4R SND's classes `FxReverbHi`, `FxReverbStd`, `FxChorus`
and `FxDelay` set them up. NW4R SND came in builds of 2006 to 2008. Before 2008, a reverb takes five or six parameters:
kinds 6 and 10. From 2008, the reverbs take the parameters of the Wii U's reverbs, modes included: kinds 7 and 11. The
parameters are the fields of each class's parameter structure, in its order and its units, with the names of the Wii
U's parameters:

| Kind | Name | Parameters |
|---|---|---|
| 6 | `wii_reverb_hi` | `pre_delay_time`, `fused_time`, `coloration`, `damping`, `crosstalk` and `out_gain` |
| 7 | `wii_reverb_hi_2008` | the Wii U's high reverb's |
| 8 | `wii_chorus` | `delay_time`, `depth` and `rate` |
| 9 | `wii_delay` | `delay`, `feedback` and `out_gain` |
| 10 | `wii_reverb_std` | `pre_delay_time`, `fused_time`, `coloration`, `damping` and `out_gain` |
| 11 | `wii_reverb_std_2008` | the Wii U's standard reverb's |

The units are those of the Wii U's parameters of the same names. The standard reverb's `out_gain` is AXFX's mix. The
reverbs of 2008 have the Wii U's modes. The library limits values as it sets an effect up. Kinds 7 and 11 take the Wii
U's limits. Kinds 6 and 10 hold the pre-delay between 0 and 0.1 s, the fused time between 0 and 10 s, and each factor
and gain between 0 and 1. The chorus and the delay take whole milliseconds. The chorus's delay runs from 1 ms to 50 ms.
The chorus varies the delay by its depth times the delay, up to 50 ms. Its rate gives a period from 500 ms to 10 s: a
rate from 0.1 Hz to 2 Hz. The delay runs from 1 ms to 5 s. Its feedback and its gain take whole hundredths: the feedback
at most 0.99, and the gain at most 1. A player follows the library.

The 3DS's effects are those that its sound libraries run on the ARM11 over an aux bus: NintendoWare's `FxReverb` and
`FxDelay` in nw::snd, and the SDK's `FxReverb` and `FxDelay` in nn::snd. nn::snd's reverb comes in two versions: the
one that games of 2011 link, and the later one. The parameters are the fields of each class's `Param`, in its order and
its units, with a reverb's three filter sizes in place of the pointer to them. The reverb of nw::snd has 12:

| Parameter | Name | Unit |
|---|---|---|
| 0 | `early_reflection_time` | milliseconds, a whole number from 0 to 1,000 |
| 1 | `fused_time` | milliseconds, a whole number from 0 to 20,000 |
| 2 | `pre_delay_time` | milliseconds, a whole number from 0 to 1,000 |
| 3 | `coloration` | a factor from 0 to 1 |
| 4 | `damping` | a factor from 0 to 1 |
| 5 | `comb_size_0` | samples, a multiple of 160 from 160 to 32,000 |
| 6 | `comb_size_1` | samples, a multiple of 160 from 160 to 32,000 |
| 7 | `all_pass_size` | samples, a multiple of 160 from 160 to 32,000 |
| 8 | `early_gain` | a gain from 0 to 1 |
| 9 | `fused_gain` | a gain from 0 to 1 |
| 10 | `surround` | a switch: 0 or 1 |
| 11 | `high_pass_damping` | a switch: 0 or 1 |

nn::snd's later reverb has the same parameters without `surround`. Its `high_pass_damping` is parameter 10. The reverb
of 2011 has neither switch. The two delays have the same 4 parameters: `delay_time` in milliseconds, a whole number from
1 to 5,000, then `feedback` and `damping`, factors from 0 to 1, and `surround`, a switch.

The libraries keep each buffer in frames of 160 samples, a frame for each 4.888 ms. A reverb's early reflection and
pre-delay take their time, 4.888 ms at least, times 0.20458265 frames for each millisecond. The product rounds to the
nearest single-precision number, and its whole part gives the frames. Each of the two buffers therefore holds a frame at
least. A delay takes its time in microseconds over 4,888, rounded down, and 1 frame at least. A filter's size is a whole
number of frames. The comb filters' gains fall by 60 dB over the fused time at 32,728 Hz. A damping above 0.95 counts as
0.95. With `surround` on, an effect processes all four of the bus's channels. Otherwise it processes the front left and
right channels, and the rear channels come back as they went in. With `high_pass_damping` on, the reverb's damping
filter keeps the highs in place of the lows. The damping filter of nn::snd's reverb of 2011 always keeps the highs. The
libraries take any time and size that fits the game's memory. The limits above keep each effect's buffers within a few
megabytes.

The 3DS has two aux buses, A and B. A player of the 3DS leaves out the effects on bus C. A bus without an effect stays
off: nn::snd turns a bus on only once a game processes it. What the tracks send to such a bus never reaches the
output.

A bus's volume is the gain at which the bus joins the output, after its effects: 1 for full volume, and 0 for silence.
It runs from 0 to 2, about +6 dB. On the Switch, the bus's channels reach the output at that gain. On the Wii U and the
Wii, it's the bus's return volume, held just below 2 in AX's 1.15 format. On the 3DS, a volume up to 1 is the bus's
return volume. The DSP's firmware takes it in 32768ths, below 32,768. Above 1, the bus returns at the firmware's highest
volume, and its samples take the volume after its effects. Each sample is then rounded to the nearest integer, halves
away from zero, and held to 32 bits. A chunk that ends after bus C's effects gives each bus a volume of 1.

A reader refuses an effect of a kind that this specification doesn't define, with a message that a later version of
Hokuto made the file. It also refuses an effect whose number of parameters differs from its kind's, a parameter that
isn't a finite number, a mode or a switch that isn't one of its choices, and a parameter of the 3DS outside its limits.
It refuses a volume that isn't a finite number from 0 to 2 too.

### SEQS: the sequences

The chunk holds the number of sequences in 4 bytes. Each sequence follows as a record: its size in 4 bytes, then its
contents. The size doesn't count itself.

| Size | Contents |
|---|---|
| 4 | the number of items |
| varies | the items |
| 4 | the number of labels |
| varies | the labels |

A label is the number of the item that it names (4 bytes), the length of its name in bytes (4 bytes), and its name in
UTF-8. A label can name the end of the sequence: the number of items. The labels of a sequence have different names.

An item starts with 5 bytes:

| Size | Contents |
|---|---|
| 2 | the kind's number |
| 2 | flags: which parts follow |
| 1 | the index: 0 for a kind without one |

| Bits of the flags | Meaning |
|---|---|
| 0 | the command has a condition |
| 1 | a value follows |
| 2–3 | the value's source |
| 4 | a time follows |
| 5–6 | the time's source |
| 7 | a key and a velocity follow |
| 8 | a target follows |
| 9 | the target is a byte offset past the end of the sequence's bytes |
| 10 | an opcode follows |
| 11 | bytes follow |
| 12 | a sample follows, on Konami's driver |
| 13–15 | 0 |

The parts follow in this order:

| Part | Size | Contents |
|---|---|---|
| value | 8, or 16 when random | the value, then the highest value for a random one |
| time | 8, or 16 when random | the time, in the same way |
| key and velocity | 9 | the key in 1 byte, then the velocity |
| target | 4 | the item's number, or a byte offset |
| opcode | 2 | the opcode |
| bytes | 4 + the bytes | the number of bytes, then the bytes |
| sample | 2 | the number of the sample in the driver's table, on Konami's driver |

A part that the kind has is always there. Any other part is there only when it isn't zero or empty. A time is there only
when the command has one. A reader refuses an item with a kind, a source or a flag that this specification doesn't
define: a later version made the file.

### SNDS: the sounds

SNDS and ITMS hold the model of a file's archive. A file holds both or neither.

| Size | Contents |
|---|---|
| 4 | the number of sounds |
| varies | the sounds, in the order of the archive's sound table |

A name is its length in bytes (4 bytes), then the name in UTF-8. A sound without a name has an empty name. A list is
the number of its entries (4 bytes), then 4 bytes for each entry. As in TABL, an item's place in its list gives its
item ID. A file's ID is its place in ITMS's files. A sound of the Wii takes the form that the end of ITMS's section
gives. Any other sound takes this form:

| Size | Contents |
|---|---|
| varies | its name |
| 1 | its kind: 1 for a sequence, 2 for a wave sound, 3 for a stream |
| 1 | flags |
| 1 | the user's parameters that its entry holds: bit n for parameter n, from 0 to 3 |
| 1 | 0 |
| 4 | its file's ID |
| 4 | its player's item ID |
| 8 | its volume, as a level |
| 1 | its remote filter |
| 1 | its pan mode |
| 1 | its pan curve |
| 1 | its player priority |
| 1 | its actor player |
| 1 | its channel priority |
| 2 | 0 |
| 4 | with bit 1 of the flags, its single play: the type of the single play and its time |
| 4 | with bit 2 of the flags, the value of option 17 of its entry |
| 4 for each | the values of the user's parameters that its entry holds, in order |
| 4 | the flags of its 3D settings: the values that the distance changes |
| 4 | the ratio by which its volume falls with the distance, as a 32-bit float |
| 1 | the curve of that fall |
| 1 | its doppler factor |
| 2 | 0 |
| 4 | the value that a sequence sound's or a wave sound's entry holds after the 3D settings. A stream sound of a 3DS archive of CSAR 2.0.0.0 holds one too. Any other stream sound's is 0 |
| varies | the details of its kind |

| Bit of the flags | Meaning |
|---|---|
| 0 | a released channel keeps its priority |
| 1 | the entry holds a single play |
| 2 | the entry holds option 17 |
| 3 | a sequence sound starts at an item of its file's sequence |
| 4 | a stream sound holds an extension |

The values without a unit are the console's values from the archive's entry. TABL describes the pan mode, the pan
curve, the priorities and the actor player. A sequence sound's details take this form:

| Size | Contents |
|---|---|
| 4 | its start: with bit 3 of the flags, an item of its file's sequence. Otherwise an offset in its file's bytecode |
| 4 | the tracks that it allocates: bit n for track n |
| varies | its bank slots: a list of the slots' banks by item ID, with 0xFFFFFFFF for an empty slot |

A wave sound's details are its index among its wave sound file's sounds, in 4 bytes. A stream sound's details take this
form:

| Size | Contents |
|---|---|
| 2 | the tracks that play when it starts: bit n for track n |
| 2 | the number of the stream's channels |
| 4 | its pitch, as a 32-bit float: a ratio of the stream's rate |
| 32 | its main send, then its sends to the aux buses A to C, as sends of 8 bytes each |
| 1 | the fields that its entry holds after the sends: 0 for none, 1 for a reference to an extension, 2 for the reference and a prefetch file |
| 1 | the options of a stream sound of a 3DS archive of CSAR 2.3.0.0: bit n says that its entry holds value n of its extension, from 0 to 2. Otherwise 0 |
| 2 | 0 |
| 4 | with fields 2, its prefetch file's ID, or 0xFFFFFFFF for none. Otherwise 0xFFFFFFFF |
| 12 | with bit 4 of the flags, its extension: three values of 4 bytes. NintendoWare's sound library takes them as the stream's type and the frames where its loop starts and ends. A value that the options leave out is 0 |
| 4 | the number of its tracks |
| varies | the tracks |

A stream sound's track takes this form:

| Size | Contents |
|---|---|
| 4 | the number of the stream's channels that it plays |
| 1 for each | the channels |
| 8 | its volume, as a level |
| 8 | its pan, as a pan |
| 8 | its surround pan, as a pan from the front |
| 1 | its flags, as the console stores them |
| 1 | the type of its biquad filter |
| 1 | the value of its biquad filter |
| 1 | 0 |
| 32 | its main send, then its sends to the aux buses A to C, as sends of 8 bytes each |
| 8 | its low-pass filter, as a fraction |

The volumes, pans, sends and low-pass filters are values in Hokuto's units (see "Values and units"). The console's
volume v is a gain of v / 127. Its level is √(127 v): a volume of 127 is a level of 127, and a volume of 64 a level of
about 90.2. The other values take the scales of the commands of the same kinds. A pan p becomes p − 64. A surround pan
and a send stay as they are. A low-pass value v becomes v / 64 − 1. A reader takes the console's value nearest each
value, from 0 to 255. Each value of the console comes back from its unit.

An item's offset is where the item starts once its sequence is encoded without the encoder's choices. A sequence sound
without bit 3 plays a file that holds none of the file's sequences, such as a file whose copies in the archive and the
group files hold other bytes. A reader refuses a kind, a flag or an option that this specification doesn't define, and
an extension of another kind of sound. It also refuses bit 3 on a sound whose file holds none of the file's sequences,
its lack on a sequence sound whose file holds one, and a start past the sequence's last item.

### ITMS: the items

| Size | Contents |
|---|---|
| 1 | the kind of archive: 1 for the Switch's, 2 for the Wii U's, 3 for the 3DS's. The Wii's take kind 4 and the form at the section's end |
| 3 | 0 |
| 4 | the archive's version, from its header: 0x00020200 for FSAR 2.2.0, and 0x02000000 for CSAR 2.0.0.0 |
| 14 | what the archive's player reserves room for: the sequence sounds, the sequence tracks, the stream sounds, the stream tracks, the stream channels, the wave sounds and the wave tracks that play at once, 2 bytes each |
| 1 | the size of each stream's buffer, in blocks |
| 1 | 0 |
| 4 | the number of sound groups |
| varies | the sound groups |
| 4 | the number of banks |
| varies | the banks |
| 4 | the number of wave archives |
| varies | the wave archives |
| 4 | the number of groups |
| varies | the groups |
| 4 | the number of players |
| varies | the players |
| 4 | the number of files |
| varies | the files, in the order of their IDs |
| 4 | the number of group files |
| varies | the group files beside the archive: each one's size in bytes (8 bytes), then its bytes |
| 4 for each | the group files' groups: each one's index in the groups, or 0xFFFFFFFF where Hokuto couldn't find the group |

Names and lists take SNDS's forms. A sound group takes this form:

| Size | Contents |
|---|---|
| varies | its name |
| 4 | its first sound's item ID, or 0xFFFFFFFF for a group without sounds |
| 4 | its last sound's item ID, or 0xFFFFFFFF for a group without sounds |
| varies | the files that its sounds play: a list of file IDs |
| 1 | flags. Bit 0 says that the group lists wave archives |
| 3 | 0 |
| varies | with bit 0, the wave archives of its wave sound file: a list of item IDs |

A bank is its name, its file's ID in 4 bytes, and the wave archives that its entry lists, as a list of item IDs. A wave
archive takes this form:

| Size | Contents |
|---|---|
| varies | its name |
| 4 | its file's ID |
| 1 | flags. Bit 0 says that a game loads its waves one at a time. Bit 1 says that its entry gives the number of its waves |
| 3 | 0 |
| 4 | with bit 1, the number of its waves. Otherwise 0 |

A group is its name, then its file's ID in 4 bytes, or 0xFFFFFFFF for a group that the game keeps beside the archive. A
player is its name, its sound limit and the size of its heap in bytes, 4 bytes each. A file takes this form:

| Size | Contents |
|---|---|
| 1 | its place |
| 1 | flags. Bit 0 says that the file holds one of the file's sequences. Bit 1 says that the chunk holds the bytes of a file beside the archive |
| 2 | 0 |
| varies | the groups that hold it: a list of their item IDs in the order of its entry. A 3DS archive's entries list no groups. The list is then empty |
| 4 | with bit 0, the number of its sequence in SEQS |
| 4 | with bit 0, the version of the sequence file, from its header |
| varies | the details of its place |

| Place | Where its bytes lie | Its details |
|---|---|---|
| 0 | in the FILE block | without bit 0, its size in bytes (8 bytes), then its bytes. With bit 0, its sequence gives them |
| 1 | in a copy that a group's file in the FILE block holds | the group's index among the groups, in 4 bytes |
| 2 | in the group files beside the archive only | none |
| 3 | in a file beside the archive, such as a stream | its path, as a name. With bit 1, its size in bytes (8 bytes), then its bytes |

A reader builds the archive from the model. Each file with bit 0 takes the bytes of a sequence file of its version: in
the FILE block for a file of place 0, and in each copy that the group files beside the archive hold. The sequence file
holds the file's sequence, encoded in the archive's format without the encoder's choices, and the sequence's labels. A
sound with bit 3 takes its item's offset in that bytecode as its start. The reader then lays the archive out as
NintendoWare's tools lay one out. A header of 64 bytes lists the STRG, INFO and FILE blocks. They follow it in that
order, without gaps. STRG holds the items' names in the order of INFO's tables. INFO holds the tables. A sound's entry
there holds its options in the order of their bits. STRG and INFO end on multiples of 32 bytes. FILE holds the bytes of
the files of place 0 in the order of their IDs. Each file there starts on a multiple of 64 bytes from the archive's
start on the Switch, and of 32 bytes on the 3DS and the Wii U. Zeros fill the gaps.

A 3DS archive (CSAR) takes the same layout. Its files' entries hold no lists of groups. The tools lay a few entries out
by the archive's version. A wave sound of CSAR 2.0.0.0 holds sends after its details' options: 127 to the main bus, a
count of 2 and six zeros. Option 8 of the details gives their offset. Before 2.3.0.0, a stream sound's details hold only
the tracks that play and the number of the stream's channels. In 2.3.0.0, the details hold the values of the extension
as options. The table of the stream's tracks follows them. It holds no sends. From 2.3.2.0 on, the details take the
layout of FSAR 2.2.0. Each send there takes 4 bytes. Before 2.3.0.0, the entry of a file of place 2 is a reference to
nothing. From 2.3.2.0 on, it's an internal file's entry whose place refers to nothing.

The reader then cuts the archive into a native archive. An import makes the same cut of a game's archive. The cut takes
the sequence of each file with bit 0 in the place of the commands that it decodes there. The sequence's commands have to
give the file's bytes back, with each sound's start at a command. A decoder can miss commands that no label or start
leads to, such as those after a return outside every call: the player runs on past such a return. The native archive's
group files are those of ITMS, with their groups. A chunk that ends after the group files gives no groups. The group
files then take the groups that the file table's lists of groups give, and none on the 3DS. The native archive's streams
are the files with bit 1. Each one counts as Hokuto's. An export therefore writes every group file and every stream
beside the archive. The native archive keeps no game's sizes. An export measures the growth from the archive as the file
was opened.

A reader refuses a place or a flag that this specification doesn't define, and bit 1 on a file that doesn't lie beside
the archive. It also refuses a model that names a sequence or a file that doesn't exist, and a model whose archive the
tools' layout can't give, such as one with two items of the same name. On the 3DS, a reader takes a wave sound only in
CSAR 2.0.0.0 and from 2.3.1.0 on. It takes a stream sound only in 2.0.0.0, 2.2.0.0 and 2.3.0.0 and from 2.3.2.0 on. It
takes a file of place 2 only before 2.3.0.0 and from 2.3.2.0 on. The tools' layout of such an item in another version is
unknown. The reader refuses it. A model whose sequence files don't give their sequences back is refused too.

A writer makes the model of the archive that the file's native archive builds, with the sequences without the
encoder's choices. The archive has to be laid out as NintendoWare's tools lay one out. Hokuto refuses to put another
archive in the model form.

An archive of the Wii (RSAR) takes kind 4 in ITMS, and another form of each sound in SNDS:

| Size | Contents |
|---|---|
| varies | its name |
| 1 | its kind: 1 for a sequence, 2 for a wave sound, 3 for a stream |
| 1 | flags. Bit 0 says that a released channel keeps its priority. Bit 3 says that a sequence sound starts at an item of its file's sequence |
| 2 | 0 |
| 4 | its file's ID |
| 4 | its player's index among ITMS's players |
| 8 | its volume, as a level |
| 1 | its remote filter |
| 1 | its pan mode |
| 1 | its pan curve |
| 1 | its player priority |
| 1 | its actor player |
| 1 | its channel priority |
| 2 | 0 |
| 8 | the values of its two user's parameters, 4 bytes each |
| 4 | the flags of its 3D settings: the values that the distance changes |
| 1 | the curve by which its volume falls with the distance |
| 1 | the ratio of that fall, as the console stores it |
| 1 | its doppler factor |
| 1 | 0 |
| 8 or 12 | the details of its kind |

A sequence sound's details are its start, its bank's index among ITMS's banks and the tracks that it allocates (bit n
for track n), 4 bytes each. With bit 3 of the flags, the start is an item of its file's sequence. Otherwise it's an
offset in its file's bytecode. A wave sound's details are its index among its wave sound file's sounds and the number of
tracks that it allocates, 4 bytes each. A stream sound's details are its start position (4 bytes), the number of
channels that it allocates (2 bytes) and the tracks that it allocates (2 bytes, bit n for track n). A stream sound's
entry holds no channel priority and no release priority fix. A writer gives it a channel priority of 64 and leaves bit
0 clear, and the archive leaves both out. The volume takes Hokuto's units, and the other values are the console's.

The Wii's ITMS chunk takes this form:

| Size | Contents |
|---|---|
| 1 | the kind of archive: 4 |
| 1 | flags. Bit 0 says that the chunk holds the game's build of NW4R SND. Bit 1 says that the trees of the archive's SYMB block start on a multiple of 4 bytes. Bit 2 says that a path whose length is a multiple of 4 bytes takes no zero after it |
| 2 | 0 |
| 4 | the archive's version, from its header: 0x0104 for RSAR 1.4 |
| 4 | with bit 0, the date of the game's build, as in BULD. Otherwise 0 |
| 1 | with bit 0, the flags of the game's build, as in BULD. Otherwise 0 |
| 3 | 0 |
| 14 | what the archive's player reserves room for: the sequence sounds, the sequence tracks, the stream sounds, the stream tracks, the stream channels, the wave sounds and the wave tracks that play at once, 2 bytes each |
| 2 | 0 |
| 4 | the number of banks |
| varies | the banks: each one's name, then its file's ID in 4 bytes |
| 4 | the number of players |
| varies | the players: each one's name, then the number of sounds that it plays at once and the size of its heap in bytes, 4 bytes each |
| 4 | the number of groups with names |
| varies | the groups: each one's name, then the files that it holds in order, as a list of file IDs. A group can name a file more than once |
| 4 | the number of files |
| varies | the files, in the order of their IDs |

A file takes this form:

| Size | Contents |
|---|---|
| 1 | flags. Bit 0 says that the file holds one of the file's sequences. Bit 1 says that the file lies beside the archive, such as a stream. Bit 2 says that the chunk holds the bytes of the file beside the archive |
| 3 | 0 |
| 4 | with bit 0, the number of its sequence in SEQS |
| 4 | with bit 0, the version of the sequence file, from its header |
| varies | with bit 1, its path as a name, then the size that its entry gives, in 4 bytes. With bit 2, its size in bytes (8 bytes), then its bytes |
| varies | without bits 0 and 1, its size in bytes (8 bytes), then its bytes |
| varies | without bit 1, the size of its wave data in bytes (8 bytes), then its wave data |

A reader builds the Wii's archive from the model. Each file with bit 0 takes the bytes of an RSEQ file of its version.
The RSEQ file holds the file's sequence, encoded without the encoder's choices by the rules of the game's build, and the
sequence's labels. Without bit 0 of ITMS's flags, the archive's version gives the build by NATV's rule. A sound with bit
3 takes its item's offset in that bytecode as its start. The reader then lays the archive out as NW4R's tools lay one
out. A header of 64 bytes lists the SYMB, INFO and FILE blocks. They follow it in that order, without gaps. SYMB holds
the names, each ended by a zero: the players', the groups', the banks' and then the sounds', each kind in the order of
its table. Four Patricia trees of the names follow, one for each kind. They take the form of the other consoles' STRG
trees. INFO holds the tables of the sounds, the banks, the players, the files and the groups, then the player's
settings. A file beside the archive has its path after its entry, then zeros to a multiple of 4 bytes. SYMB and INFO end
on multiples of 32 bytes. FILE holds the groups' files: each group's files one after another, then their wave data. A
group without a name follows the groups of ITMS. It holds the files that the other groups don't hold, in the order of
their IDs. A file beside the archive lies in no group.

The reader then cuts the archive into a native archive. An import cuts a game's archive the same way. As on the other
consoles, the cut takes the sequence of each file with bit 0 in the place of the commands that it decodes there. The
native archive keeps the game's build. Its streams are the files with bit 2. An export treats each one as a stream that
the game lacks, and writes every stream beside the archive. A reader refuses a kind or a flag that this specification
doesn't define, a player that plays more than 255 sounds at once, bit 0 on a file beside the archive, and bit 2 on a
file without bit 1. It refuses the starts that the other consoles' models refuse. It also refuses a model whose archive
the tools' layout can't give: a version before 1.1 or after 1.4, an item that names a file, a bank or a player that the
model lacks, a group that names a file beside the archive, a file or wave data whose size isn't a multiple of 32 bytes,
and two items of one kind with the same name. A writer makes the model of the Wii's archive that the file's native
archive builds, with the sequences without the encoder's choices and the native archive's build.

### NATV: the native archive

| Size | Contents |
|---|---|
| 1 | the kind of archive: 1 for the Switch's, 2 for the Wii U's, 3 for the 3DS's, 4 for the Wii's, 5 for a GBA ROM, 6 for the DS's, 7 for a set of DSE's files, 8 for a GBA ROM with Konami's driver, 9 for a GBA ROM with Nintendo R&D2's driver, 10 for a GBA ROM with Brownie Brown's driver, 11 for a GBA ROM with Ubisoft Milan's driver, 12 for a GBA ROM with Rare's driver, 13 for a GBA ROM with Quintet's driver, 14 for a GBA ROM with Krawall |
| 1 | flags. Bit 0 says that the songs of a GBA ROM can go into its filler runs, with MP2K, Konami's driver, Nintendo R&D2's, Rare's or Krawall. Bit 1 says that a note of a GBA ROM with MP2K reads two argument bytes: its key and its velocity. An archive of another kind holds 0 |
| 2 | 0 |
| 4 | the number of pieces |
| varies | the pieces |
| 4 | the number of fields |
| 36 for each | the fields |

Each piece starts with its type (1 byte), 3 zero bytes and the size of its body (8 bytes). Its body follows. A piece
of type 0 is a run of bytes, and its body is the bytes. A piece of type 2 is padding, and its body is the alignment
in 4 bytes. A piece of type 1 is a sequence file:

| Size | Contents |
|---|---|
| 4 | the number of its sequence in SEQS |
| 4 | the file's version, from its header |
| 4 | the number of items with the encoder's choices |
| 8 for each | the choices: an item's number (4 bytes), its value_bytes (1), its unused_time (1), flags (1) and 0 (1). Bit 0 of the flags is unused_random, and bit 1 unused_variable |

| Choice | Meaning |
|---|---|
| value_bytes | the bytes that a variable-length value took, where it took more than it requires. Otherwise 0 |
| unused_time | the byte of a time prefix that the command reads no time for. Otherwise 0 |
| unused_random | a random prefix stands before a command that reads no argument from it |
| unused_variable | a variable prefix stands before a command that reads no argument from it |

An archive's groups can hold copies of a sequence file. Each copy is a sequence file of the same sequence. The first
sequence file of a sequence holds its choices, and the others hold none.

A field is three places: where the field lies, then the two places whose distance it holds. Each place takes 12
bytes:

| Size | Contents |
|---|---|
| 4 | a piece's number, the number of pieces for the archive's end, or a sequence's number in SEQS |
| 4 | a byte of the piece, counted from its start, or an item of the piece's sequence or of the sequence |
| 1 | flags. Bit 0 says that the place is an item. Bit 1 says that the place names a sequence, and comes with bit 0. Bit 2, on where a field lies in a set of DSE's files or a ROM with Konami's or Quintet's driver, says that the field holds 2 bytes |
| 3 | 0 |

The field lies in a run of bytes, and its 4 bytes lie within the run. A place names a byte of a run, the start of any
piece, an item of a sequence file or of a sequence, or the archive's end. An item's place is where the item starts once
its sequence is encoded. The number of items names the end of the bytecode. A place at the archive's end can count bytes
past the end: an archive cut short has fields that point past its end. A place that names a sequence names an item of a
sequence that no piece of the archive holds, such as a sequence file that only the group files beside the archive hold
(GRPF). Both places of a field with such a place name items of the same sequence. The field holds the distance between
the two items once the sequence is encoded.

An export puts the pieces together in order. A sequence file becomes its sequence, encoded in the archive's format with
the encoder's choices. Padding adds zeros until the archive's length so far is a multiple of its alignment. Each field
then gets the distance from its first place to its second, in 4 bytes in the archive's byte order: little-endian for
the Switch and the 3DS, and big-endian for the Wii U and the Wii. An export fails for a distance below zero or past 4
bytes.

An archive of the Wii (RSAR) keeps its files in its groups. A group holds its files one after another, then their wave
data. A file that several groups hold has a copy in each. Its sequence files are RSEQ files, and a sequence file's
version is the 2 bytes at 6 in the RSEQ file's header. The sequences take the commands of a build of NW4R SND, the Wii's
sound library: the game's build that the BULD chunk gives. Without that chunk, an archive of version 1.4 or later takes
a build of June 2008, and an older one a build of 2007. The version is the 2 bytes at 6 in the archive's header, in the
first piece. The fields hold the archive's size, each block's offset and size, each group's offsets and sizes, those of
each item of a group, each file's sizes in the file table, and each sequence sound's start. Padding aligns a file to 32
bytes at most. NW4R's tools use the same padding. A file of the Wii holds no GRPF chunk. Its STRM chunk holds the
streams beside the archive: RSTM files at the paths that the file table gives, relative to the archive's folder. It can
take the model form (see ITMS). Its TABL chunk lists no wave archives and no groups, and its SIZE chunk gives the
archive's size alone.

A GBA ROM (kind 5) keeps its songs among the game's code and data, and code and tables all over the ROM hold addresses
in it. Its pieces therefore keep their places. A sequence piece holds a song: the bytes from its first track's start up
to its header, where MP2K's tools put the tracks. The header starts the run after the piece. In place of the file's
version, a sequence piece holds its room: the size of the song's place in the ROM. The choices of a sequence piece of a
GBA ROM are MP2K's:

| Size | Contents |
|---|---|
| 4 | an item's number |
| 1 | the argument bytes plus 1, or 0 |
| 1 | the length code plus 1, or 0 |
| 1 | flags. Bit 0 is repeated |
| 1 | value_bytes, on Nintendo R&D2's driver, Rare's and Krawall. MP2K's and Konami's hold 0 |

| Choice | Meaning |
|---|---|
| repeated | the command repeats the running status, and its bytes leave the opcode out. On Nintendo R&D2's driver, a `note` or a `wait` of one byte that takes the track's last values |
| arguments | the argument bytes of a `note` or a `note_off`, where they aren't the usual ones. A note's usual ones are its key, its velocity and, where its length requires it, its extra length. A `note_off`'s usual one is its key. The bytes that a command leaves out take the track's last key and velocity. On Rare's driver, the byte that the driver ignores after a `note_off`, an `other` channel pressure, a `loop_start`, a `loop_end` and a `monophonic` |
| length_code | a note's length code, 0 to 48, where it isn't the code of the longest length up to the note's length. On Quintet's driver, the code of a `note`'s or a `note_off`'s note value, where it isn't the lowest code that gives the length: its index in the driver's table of note values, or the byte of the note value in the revision of notes of two bytes |
| value_bytes | on Nintendo R&D2's driver, 2 for a length in two bytes that one byte would hold. On Rare's driver, the bytes of a `wait` where they're more than its delay requires. On Krawall, 3 for an instrument in three bytes that two bytes would hold |

A note of MP2K reads up to three argument bytes after its length code: its key, its velocity and its extra length. In
some games, a note reads two: its key and its velocity. Bit 1 of NATV's flags says so. A note there lasts its code's
length, and a byte below 0x80 after its velocity starts a command that repeats the running status. Such a ROM can't hold
a note of another length. A tie holds it instead: a `note` of length 0 and a `note_off` at the note's end.

A GBA ROM's fields hold addresses: the distance from the first place, the ROM's start, to the second, plus 0x08000000,
little-endian. Each field of a track's address in a header names the item where the track starts. Each entry of the song
table names the start of the run that holds its song's header. A GBA ROM holds no padding.

An export writes a song in its place when its bytes fit there, and zeros after them up to the header. A song that
outgrows its place goes into the ROM's free space, with a copy of its header after it, on a multiple of 4 bytes. Its
place holds zeros, and a field that names the header's run names the copy. A field in the header goes into both. The
free space starts 256 bytes or more into the run of 0xFF bytes at the ROM's end, on a multiple of 256 bytes, or else at
the ROM's end. The songs that move follow one another there, in the order of their pieces. A ROM that grows fills with
0xFF bytes. An export fails for a ROM past 32 MiB, unless bit 0 of NATV's flags allows filler runs. A file of the GBA
holds no GRPF, STRM, TABL or BULD chunk, and it can't take the model form. Its SIZE chunk gives the ROM's size.

With bit 0 of NATV's flags, a song that would take the ROM past 32 MiB goes into a filler run instead: the first one
with room for the song and its header's copy, after the songs that went there before it, on a multiple of 4 bytes. A
filler run falls within a run of 16 KiB or more of 0xFF bytes that ends before the ROM's end. It starts 256 bytes or
more into the run, on a multiple of 256 bytes, and ends 256 bytes before the run's end. No byte of the run may be one
that MusicPlayer2000 reads for the ROM's songs. Those bytes are the song table and the music players' table, each song's
header and the commands that its tracks reach, the 128 voices of the song's voice group with their key split and drum
tables, and the samples and wave patterns that those voices play. The runs come from the ROM as the export has written
it when a song first requires one. It holds the songs that keep their places, the songs at the ROM's end, and the
headers of the songs that move, in their places. Nothing shows that a game's other code doesn't read a filler run: the
user chooses it. An export fails when the filler runs can't hold the songs either.

An archive of the DS (SDAT, kind 6) keeps its files in its FILE block, at the offsets and sizes that its file table
(FAT) gives. Its sequence files are SSEQ files. A sequence piece holds a sequence file's bytecode, from its start, and
its version is the 2 bytes at 6 in the SSEQ file's header. The file's header of 0x1C bytes ends the run before the
piece. A sequence archive (SSAR) stays in a run. The fields hold the archive's size in its header, the FILE block's size
in the header and in the block, each file's offset and size in the file table, and each sequence file's size and its
DATA block's size in its header. An offset counts from the archive's start. Padding of 4 bytes ends a sequence file, and
padding of 32 bytes puts each file on a multiple of 32 bytes from the archive's start. The SDK's tools lay an archive
out that way. Each field holds its distance little-endian. A file of the DS holds no GRPF, STRM, TABL or BULD chunk, and
it can't take the model form. Its SIZE chunk gives the archive's size. Its CART chunk can hold the cartridge image that
holds the archive.

A set of DSE's files (kind 7) isn't a file of the game. It starts with a directory: the magic word DSET, the set's size,
the number of files and 4 zeros, then each file's offset from the set's start, its size and its path's offset, then the
paths, each ended by a zero. All these numbers take 4 bytes, little-endian. Each file follows on a multiple of 16 bytes,
in the order of the list. The bytes of the tracks of a song or a sound effect make up a sequence piece. The piece has
version 0. The file's other bytes stay in runs. A file whose bytes the encoder can't give back stays one run. The fields
hold each file's size in its header and in the directory, its offset in the directory, the size of a bank of sound
effects' seq chunk, and each slot of that chunk's table. A slot holds a 16-bit offset from the table's start, and a
field in a slot holds 2 bytes. Every other field holds 4. An export refuses a slot past 16 bits. A file of DSE holds no
GRPF, STRM, TABL or BULD chunk, and it can't take the model form. Its SIZE chunk gives the set's size. Its CART chunk
holds the cartridge image, with the file ID and the offset at 0. A sequence piece's choices take the same 8 bytes each:
the item's number (4 bytes), `arguments` plus 1 or 0 without them (1), `value_bytes` (1), flags (1) and 0 (1). Bit 0 of
the flags is `repeated`. Those three are the only choices that a set holds, with an item's `opcode`: see "Commands".

Like a GBA ROM of MP2K, a GBA ROM with Konami's driver (kind 8) keeps its pieces in their places. A sequence piece holds
a song's bytes, from the song's base to its last byte, and holds its room in place of the file's version. Entries of the
song table that give the same base and the same tracks share a piece. The song table stays in a run. Each entry gives a
field of 4 bytes for the song's base, and then a field of 2 bytes for each of its tracks. The base's field holds the
distance from the ROM's start to the song's first item, plus 0x08000000. A track's field holds the distance from the
song's first item to the item where the track starts. A sequence piece holds no choices of the encoder: an item's
`opcode` keeps what its values don't give.

After the fields, NATV holds what the driver's export requires:

| Size | Contents |
|---|---|
| 1 | the driver's revision, from 0 to 10, in the order of ntdlib's `konami::Revision` |
| 1 | the byte that fills what the ROM grows by, up to a song |
| 2 | 0 |
| 4 | where the free space at the ROM's end starts, from the ROM's start |
| 4 | the number of filler runs |
| 8 for each | a filler run: its start and its size, each in 4 bytes |

An export writes a song in its place when its bytes fit there, and zeros after them. A song that outgrows its place
moves, largest first. It goes into the first place of a moved song with room for it, where neighbouring places count as
one. Failing that, it goes into the free space, and the ROM grows when the songs run past its end. A song's place holds
zeros in the bytes that no moved song takes. The free space starts 256 bytes or more into the run of 0xFF or of zeros at
the ROM's end, on a multiple of 256 bytes, and past every byte of that run that the driver reads. Without such a run, it
starts at the ROM's end, on a multiple of 4 bytes.

An export fails for a ROM past 32 MiB, unless bit 0 of NATV's flags allows filler runs. A song that would take the ROM
past 32 MiB then goes into the first filler run with room for it. A filler run falls within a run of 4 KiB or more of
0xFF or of zeros that ends before the ROM's end. It starts 256 bytes or more into the run, on a multiple of 256 bytes,
and ends 256 bytes before the run's end. No byte of the run may be one that the driver reads: the song table, each
song's bytes, the sample tables with every sample that they name, and the driver's other tables. The runs of 0xFF come
first, then the runs of zeros, each in the order of their places. Zeros can be data of the game, and nothing shows that
a game's other code doesn't read a filler run: the user chooses it.

A file with Konami's driver holds no GRPF, STRM, TABL, BULD or CART chunk, and it can't take the model form. Its SIZE
chunk gives the ROM's size.

A GBA ROM with Nintendo R&D2's driver (kind 9) keeps its pieces in their places too. A sequence piece holds a sequence's
bytes, from its header to the last byte that a track reads, with the few bytes up to the next sequence that no track
reads. It holds its room in place of the file's version. Entries of the tables that name the same address share a piece.
The table of the music's sequences and the table of the sound effects' sequences stay in runs. Each entry gives a field
of 4 bytes: the distance from the table's start to the sequence's first item. The choices of a sequence piece take the
GBA's form above: `repeated` and `value_bytes`. After the fields, NATV holds what the driver's export requires, in the
form that Konami's driver takes. The revision runs from 0 to 2, in the order of ntdlib's `rd2::Revision`. A sequence
that outgrows its place moves as a song of Konami's driver does, and the free space and the filler runs follow the same
rules. A run that holds a byte that the driver reads isn't a filler run. Those bytes are the settings, the tables, each
sequence's bytes, the 64 KiB that a bank's offsets reach, and every sample that a sample set names. A sequence starts on
a multiple of 4 bytes.

A file with Nintendo R&D2's driver holds no GRPF, STRM, TABL, BULD or CART chunk, and it can't take the model form. Its
SIZE chunk gives the ROM's size.

A GBA ROM with Brownie Brown's driver (kind 10) keeps its pieces in their places too. A sequence piece holds a region's
bytes, from the first byte that its sounds read to the last, and holds its room in place of the file's version. The
song table stays in a run. Each entry with a list gives a field of 4 bytes: the distance from the ROM's start to the
item where the sound's list starts, plus 0x08000000. A sequence piece holds no choices of the encoder: an item's
`opcode` keeps what its values don't give. After the fields, NATV holds what the driver's export requires, in the form
that Konami's driver takes. The revision runs from 0 to 1, in the order of ntdlib's `brownie::Revision`. A region that
outgrows its place moves as a song of Konami's driver does, and the free space follows the same rules. A region that
moves keeps its start's remainder modulo 4. NATV lists no filler runs: a ROM with this driver takes none.

A file with Brownie Brown's driver holds no GRPF, STRM, TABL, BULD or CART chunk, and it can't take the model form. Its
SIZE chunk gives the ROM's size.

A GBA ROM with Ubisoft Milan's driver (kind 11) keeps its pieces in their places too. A sequence piece contains the
bytes of a sequence, from the kind of resource to the end. The size of its place replaces the version of the file.
Entries of the resource table that point to the same sequence share a piece. The header of the sound bank and the
resource table stay in runs. Each entry that points to a sequence is a field of 4 bytes: the distance from the start of
the bank to the first item of the sequence. A sequence piece contains no choices of the encoder. After the fields, NATV
contains what the export of the driver requires, in the same form as for Konami's driver, with a revision of 0. A
sequence that outgrows its place moves as a song of Konami's driver does, and the free space follows the same rules. A
sequence can start at any byte. An export can't put a sequence into a filler run: bit 0 of the flags is 0, and NATV
lists no filler runs. A reader refuses either.

A file with Ubisoft Milan's driver contains no GRPF, STRM, TABL, BULD or CART chunk, and it can't use the model form.
Its SIZE chunk stores the size of the ROM.

A GBA ROM with Rare's driver (kind 12) keeps its pieces in their places too. A sequence piece contains a tune's tracks,
from the first track's start to the last byte that a track reads. It contains its room in place of the file's version.
Tunes whose track lists name the same tracks share a piece. The tune table, the tunes' headers and their track lists
stay in runs. Each word of a track list gives a field of 4 bytes: the address of the item where the track starts. The
choices of a sequence piece take the GBA's form above: `arguments` and `value_bytes`. After the fields, NATV contains
what the driver's export requires, in the form that Konami's driver takes. The revision gives the traits of the driver's
build as bits: bit 0 for a channel in the command byte's high nibble, and bit 1 for controllers 20 to 23. A sequence
that outgrows its place moves as a song of Konami's driver does, and the free space and the filler runs follow the same
rules. A run that contains a byte that the driver reads isn't a filler run. A sequence starts on a 4-byte boundary.
A tune keeps its tracks: the track list and the count of tracks stay in their runs.

A file with Rare's driver contains no GRPF, STRM, TABL, BULD or CART chunk, and it can't use the model form. Its SIZE
chunk gives the ROM's size.

A GBA ROM with Quintet's driver (kind 13) keeps its pieces in their places too. A song's header is a run of 14 bytes,
and its commands a sequence piece: the bytes after the header, up to the song's length. A song that the decoder can't
read in full, or whose length is odd, stays a run, header and all. Each field of a header contains 2 bytes: the distance
from the header's start to the next piece for the song's length, and to the item where a channel starts for each of the
6 channels. The choices of a sequence piece take the GBA's form above: `length_code`. After the fields, NATV contains
what the driver's export requires, in the form that Konami's driver takes. The revision is 0 for notes of one byte and 1
for notes of two bytes. The high nibble of a note of one byte picks a note value from the driver's table. The free space
starts at 0, and the ROM has no filler runs. Then come the rooms:

| Size | Contents |
|---|---|
| 4 | the number of rooms |
| 12 for each | a room: the first piece after the list of songs, in 4 bytes. That piece starts at the room's end. The room's end, from the ROM's start, in 4 bytes. The byte that fills the room after the list's last piece, and 3 bytes of 0 |

A list of songs is the songs of a music file or its sound effects, back to back. The game finds a song by skipping the
songs before it. A list therefore keeps its place, and its songs keep their order. A list can grow into its room, up to
the room's end, and an export fails for a list that outgrows it. A list that shrinks leaves the fill byte at the end of
its room. A song's bytes take an even size: the export adds a byte of 0 after an odd sequence. A reader refuses a room
that names a piece past the pieces.

A file with Quintet's driver contains no GRPF, STRM, TABL, BULD or CART chunk, and it can't use the model form. Its SIZE
chunk gives the ROM's size.

A GBA ROM with Krawall (kind 14) keeps its pieces in their places too. A sequence piece holds a module's patterns, from
the first pattern to the end of the pattern that starts last. It holds its room in place of the file's version. In the
games, a module's header follows its patterns. The header stays in a run, and so do the game's tables. Each address in a
header's table of patterns gives a field of 4 bytes: the distance from the ROM's start to the item where the pattern
starts. Two fields can name one pattern. The choices of a sequence piece take the GBA's form above: `value_bytes`. After
the fields, NATV contains what Krawall's export requires, in the form that Konami's driver takes. The revision is 1 for
the builds from 2004/09/17 on and 0 for the earlier builds. Patterns that outgrow their place move as a song of Konami's
driver does, and the free space and the filler runs follow the same rules. A moved sequence starts on a multiple of 4
bytes. In the modified build, the modules are stored packed in the game's music files, and they stay in the runs.

A file with Krawall contains no GRPF, STRM, TABL, BULD or CART chunk, and it can't use the model form. Its SIZE chunk
gives the ROM's size.

### BULD: the game's build

| Size | Contents |
|---|---|
| 4 | the date of the game's build of NW4R SND, as YYYYMMDD, or 0 for a build without a banner |
| 1 | flags. Bit 0 says that the build comes with the build of AX of October 2006 |
| 3 | 0 |

A game's main.dol links one build of NW4R SND and one of AX, the Wii's audio library. NW4R SND's version banner in the
main.dol gives its build's date, such as `final build: Jun  8 2007` for 20070608. The builds of 2006 have no banner.
AX's microcode in the main.dol identifies AX's build: the build of October 2006 reads parameter blocks of 0x120 bytes,
and every later build blocks of 0x140 bytes. A file of the Wii holds the chunk where its archive came with the game's
main.dol. A file in the model form holds the build in ITMS instead. The builds differ in the commands that the sequences
take and in how they play them. A build plays as the latest of these dates that it reaches: 20080308, 20080620, 20081120
and 20090924. A build without a banner plays as a build of 2007, or of 2006 with the flag. A reader refuses a flag that
this specification doesn't define, with a message that a later version of Hokuto made the file.

### TABL: the tables

| Size | Contents |
|---|---|
| 4 | the number of sounds |
| varies | the sounds |
| 4 | the number of banks |
| varies | the banks' names |
| 4 | the number of wave archives |
| varies | the wave archives' names |
| 4 | the number of players |
| varies | the players |
| 4 | the number of groups |
| varies | the groups' names |

A name is its length in bytes (4 bytes), then the name in UTF-8. An item's place in its list gives its item ID: its
type in the top byte, and its place in the low 24 bits. The types are 1 for a sound, 3 for a bank, 4 for a player, 5
for a wave archive and 6 for a group. A player is its name, then its sound limit in 4 bytes: how many of its sounds
play at once. A sound takes this form:

| Size | Contents |
|---|---|
| varies | its name |
| 1 | its kind: 0 for unknown, 1 for a sequence, 2 for a wave sound, 3 for a stream |
| 1 | its volume: a gain of the volume over 127 |
| 1 | its pan mode: 0 pans each channel of a stereo sound apart, and 1 balances them |
| 1 | its pan curve, by the console's number. 0 is the default |
| 1 | its player priority |
| 1 | its actor player |
| 1 | its channel priority. Each of its tracks adds it to the track's priority |
| 1 | flags. Bit 0 says that a released channel keeps its priority |
| 4 | its player's item ID |
| 4 | the tracks that it allocates: bit n for track n |
| 4 | the number of its bank slots |
| 4 for each | the item ID of the slot's bank, or 0xFFFFFFFF for an empty slot |

Only a sequence sound has bank slots, tracks, a channel priority and the flag. Another sound keeps their defaults: no
bank slots, track 0, a channel priority of 64 and no flags.

An archive of the Wii holds the values that the Wii's sound library reads for playback. A sequence sound of the Wii
has one bank slot. The players and banks take the item IDs above, by their places in the archive's tables. The Wii's
tables hold no wave archives and no actor players. TABL lists no wave archives and no groups for a Wii archive, and
each sound's actor player is 0. The export writes each value in place, and nothing moves. A Wii sound's list of bank
slots keeps its one slot. An archive before version 1.2 has no place for a pan mode or a pan curve. Every sound there
balances its channels, with the default curve. One before version 1.3 has no place for the flag. The export fails for
other values there.

The tables give each value. The bytes of NATV's runs hold the archive's tables too. A sequence sound's list of bank
slots in TABL can be longer or shorter than its list in the archive's tables, up to four slots. An export then first
writes the archive's tables again with a list of the new length. A sound's entry in the archive's tables can also leave
out the option of its pan, of its priorities or of a sequence sound's channel priority. A reader then gives the sound a
pan mode and a pan curve of 0, a player priority of 64 with actor player 0, and a channel priority of 64 without the
flag. When TABL gives such a sound other values, the export first writes the archive's tables again with the option in
the entry. The option's value takes its place among the entry's options in the order of their bits. NintendoWare's
tools lay an entry out the same way. Either way, the INFO block grows or shrinks, and the FILE block moves after it.
That requires an archive laid out as NintendoWare's tools lay one out. The export fails for another archive. An export
writes each value of TABL over the bytes where the archive's tables hold it. It then reads the tables again, and fails
when a value reads back otherwise, such as a fifth bank slot or a wave sound's channel priority. It also fails when the
file's tables differ from the archive's in an item's name, a sound's kind or the number of items. An export writes none
of those. An archive whose tables Hokuto can't read without a problem has no TABL chunk, and its export keeps the bytes
of NATV.

### SIZE: the game's sizes

| Size | Contents |
|---|---|
| 8 | the size of the game's archive in bytes, when Hokuto imported it |
| 4 | the number of groups |
| 8 for each | the size in bytes of the files that the group loads, in the order of TABL's groups |

A group's files are the copies that its file holds and the archive's files that it links to. Where GRPF holds the group
file with its group, a group whose file lies beside the archive counts the files of its group file. Hokuto counts no
files for such a group without one. A game loads each group into its sound heap, and the game's archive fits the heap.
An export measures how much the archive and each group grow from these sizes. Edits don't change them. A group that an
edit adds takes a size of 0: the game's archive has no such group. SIZE gives a size for each group of TABL. Without
TABL, it gives the archive's size alone, and no groups. A reader refuses a SIZE chunk whose number of groups differs.
Where Hokuto can't read a group's list of files, the file has no SIZE chunk. Without one, Hokuto measures the growth
from the file as it was opened.

### GRPF: the group files

| Size | Contents |
|---|---|
| 4 | the number of group files |
| varies | the group files |

A group file takes this form:

| Size | Contents |
|---|---|
| 4 | the group's index in TABL's groups, or 0xFFFFFFFF where Hokuto couldn't find the group |
| 8 | the size of the game's group file in bytes |
| 20 | the SHA-1 of the game's group file |
| 4 | the number of pieces |
| varies | the pieces |
| 4 | the number of fields |
| 36 for each | the fields |

The pieces and the fields take the forms of NATV's, among the group file's pieces. A place at the group file's end names
the number of its pieces. A sequence file there names a sequence of SEQS. The archive's pieces hold a sequence file of
the same sequence, unless only group files hold the sequence's file. The archive's fields then name items of the
sequence itself (see NATV). The first sequence file of a sequence holds its choices, and NATV comes first. An export
joins each group file's pieces as it joins the archive's, and writes its fields. A group file whose bytes then differ
from the game's, by their size or their SHA-1, goes out with the export. A SARC archive beside the archive that holds a
file of the group takes the new group file in that file's place. The SARC archive then goes out under its name.
Otherwise the group file goes beside the archive under its group's name and .bcgrp on the 3DS, or .bfgrp on the Wii U
and the Switch. Where the game keeps that file in the folder `userManagementFiles` beside the archive, the new file goes
into that folder. A group file whose group Hokuto couldn't find stays out. A player adds the group files to the archive
in order. Where two group files hold the same file, the player takes the first one's copy.

### STRM: the streams

| Size | Contents |
|---|---|
| 4 | the number of streams |
| varies | the streams, in the order of their file IDs |

A stream takes this form:

| Size | Contents |
|---|---|
| 4 | the ID of the stream's file in the archive's file table |
| 4 | the flags |
| 8 | the stream's size in bytes |
| varies | the stream's bytes |

Flag 1 marks a stream that Hokuto made or changed. An export writes it beside the archive. On the Wii, the export also
writes the stream's size into the file table. The other flags are 0. A reader refuses file IDs that don't rise, and a
flag that this specification doesn't define. A stream's bytes are those of its file: a stream file, or on the 3DS an AAC
file in ADTS.

### CART: the cartridge image

| Size | Contents |
|---|---|
| 4 | the ID of the file in the image's file system that holds the archive, or 0 for a set of DSE's files |
| 4 | the archive's offset in that file, or 0 for a set of DSE's files |
| 4 | the image's size in bytes |
| 4 | 0 |
| varies | the image's bytes, up to its last byte that isn't 0xFF |

A DS cartridge image fills its chip with 0xFF bytes after its data. The chunk leaves those bytes out, and 0xFF bytes
follow its bytes up to the image's size. The image holds the game's archive in its file. NATV gives the archive itself.
An export writes the archive that NATV builds in its place in the file. When the archive holds the game's bytes, the
image comes back byte for byte. Otherwise the file takes the new archive's bytes. The files after it move by multiples
of 0x200 bytes, and the file table, the header and its CRC-16 follow. The export fails for an archive of another size
than the game's when the file holds other bytes than the archive: nothing tells Hokuto what points into those bytes.
A DSi image holds hashes of its bytes, and only the DSi's keys give the new hashes. The export leaves the hashes of the
bytes that change as they were, and reports them. A reader refuses a CART chunk whose image's bytes run past its size,
or end with 0xFF, and a flag other than 0.

A set of DSE's files names each file by its path. An export finds each path in the image's file system, and writes each
file whose bytes differ from the image's. The files after it move as above.

### BANK: the banks

| Size | Contents |
|---|---|
| 4 | the number of banks |
| varies | the banks, in the order of their file IDs |

A bank takes this form:

| Size | Contents |
|---|---|
| 4 | the ID of the bank's file in the archive's file table |
| 1 | the bank's console: 1 for the Switch, 2 for the Wii U, 3 for the 3DS |
| 3 | 0 |
| 4 | the version of the bank's file, from its header |
| 4 | the number of entries in the bank's table of waves |
| 8 for each | the entries: a wave archive's item ID (4 bytes), then a wave's index among its waves (4 bytes) |
| 4 | the number of programs |
| 4 | the number of programs in the layout's order, or 0 |
| 4 for each | the programs of the instruments, in the order that the bank's file lays them out |
| varies | the programs, from program 0 on |

The layout's order is empty when the bank's file lays its instruments out in the order of their programs. A program
starts with flags (1 byte) and 3 zero bytes. Without bit 0, the program holds no instrument, and nothing follows. With
bit 0, the instrument follows: the number of its key regions (4 bytes), then the key regions in the order of their
keys. A key region takes this form:

| Size | Contents |
|---|---|
| 1 | its first key |
| 1 | its last key |
| 2 | 0 |
| 4 | the number of its velocity regions |
| 28 for each | the velocity regions, in the order of their velocities |

A velocity region takes this form:

| Size | Contents |
|---|---|
| 1 | its first velocity |
| 1 | its last velocity |
| 2 | 0 |
| 4 | its wave, as an entry of the bank's table of waves |
| 1 | the key at which its wave plays at its rate |
| 1 | its volume: 127 for full volume |
| 1 | its pan: 0 for the left, 64 for the centre and 127 for the right |
| 1 | flags. Bit 0 says that the region ignores a note-off |
| 4 | its pitch, a 32-bit float: the ratio by which it raises its wave's rate |
| 1 | its key group, or 0 for none |
| 1 | its interpolation, as the bank's file holds it |
| 2 | 0 |
| 5 | its envelope: the attack, the decay, the sustain, the hold and the release |
| 3 | 0 |

The values are the bank's file's, in the console's units. A writer lays the bank out from its model as NintendoWare's
tools lay a bank out. Each copy of the bank's file in the archive holds those bytes. A reader refuses file IDs that
don't rise, a console or a flag that this specification doesn't define, and a model that can't be laid out, such as one
whose key regions overlap.

A file of the Wii keeps no bank in BANK. The archive holds the bytes that an edited bank's model lays out, and a reader
takes the model from those bytes.

### WAVE: the waves

| Size | Contents |
|---|---|
| 4 | the number of waves |
| varies | the waves, in the order of their places |

A wave takes this form:

| Size | Contents |
|---|---|
| 4 | the ID of its wave archive's file in the archive's file table |
| 4 | its index among the wave archive's waves |
| 1 | its encoding: 0 for PCM8, 1 for PCM16 and 2 for DSP-ADPCM |
| 1 | flags. Bit 0 says that the wave loops, and bit 1 that its samples follow |
| 2 | 0 |
| 4 | the rate at which the console plays it, in Hz |
| 4 | its loop's start, in samples |
| 4 | its loop's end, in samples |
| 4 | its length: the samples in each channel |

A place is the wave archive's file ID, then the wave's index. With bit 1, the samples follow: the number of channels
(4 bytes), 1 or 2, then each channel's samples as PCM16, as many as the length. The channels come one after another.

A wave plays up to its loop's end, with a loop or without one. The samples after it stay in the wave where the user
moved the end earlier. A wave without samples keeps the encoded samples that its wave archive held. A wave with samples
takes its encoded samples from them. A loop in DSP-ADPCM starts at a frame of 14 samples. The wave archive in the
archive holds the wave that the model gives. A reader refuses places that don't rise, an encoding or a flag that this
specification doesn't define, and samples of more than two channels. It also refuses a wave that plays at a rate of 0 or
plays no samples, a loop that ends after the last sample or doesn't end after its start, and a start other than 0
without a loop.

On the Wii, a bank or a wave sound file holds its waves in its wave data, and the wave data takes the place of a wave
archive. A place there is the ID of the bank's file or of the wave sound file, then the wave's index in the wave data.
The encodings take the same numbers. A loop in DSP-ADPCM of a Wii wave can start between frames. A writer keeps such a
start until an edit moves the loop.

### SCEN: the scenes

| Size | Contents |
|---|---|
| 4 | the number of scenes |
| varies | the scenes, in the order that they were stored |

A scene takes this form:

| Size | Contents |
|---|---|
| 4 | its sound: the sound's index in the archive's sound table |
| varies | its name: its length in bytes (4 bytes), then the name in UTF-8 |
| 4 | the number of its starting values |
| 4 for each | a starting value: the variable's number (1 byte), 0 (1 byte) and the value (2 bytes, signed) |
| 4 | the number of its changes |
| 8 for each | a change: its time (4 bytes), the variable's number (1 byte), 0 (1 byte) and the value (2 bytes, signed) |

The variables are those that a game sets through a sound's handle: the sound's player variables 0 to 15 and the global
variables 16 to 31. Every sound that plays shares the global variables. Each variable starts at -1 until the game or a
command sets it. On the Wii, the global variables start at 0. A scene sets the variables that it starts before the
sound's first frame. Hokuto writes the starting values in the order of the variables' numbers.

A change's time counts milliseconds from the sound's start. The engine makes the change at the start of its first frame
that starts at that time or later. A frame lasts 5 ms on the Switch, and 3 ms on the Wii U and the Wii. On the 3DS, a
frame holds 160 samples at 268,111,856 / 8,192 Hz, and lasts about 4.889 ms. The changes come in the order of their
times. Two changes at the same time take effect in their order.

A reader refuses a scene that starts a variable twice, a variable's number above 31, and changes out of the order of
their times. It refuses a byte after a variable's number that isn't 0, with a message that a later version of Hokuto
made the file. A scene of a sound that the archive lacks does nothing.

### TCOL: the tracks' colours

| Size | Contents |
|---|---|
| 4 | the number of colours |
| 8 for each | a track's colour: the sound (4 bytes), the track (1 byte), and the colour's red, green and blue (1 byte each) |

The sound is the sound's index in the archive's sound table. The track is the track's number in the sound's sequence.
The colour is in 8-bit sRGB. Hokuto writes the colours in the order of their sounds, and of their tracks within a
sound. A reader takes them in any order. It refuses a track with two colours. A colour of a sound or a track that the
archive lacks does nothing.

### IMAP: the instrument maps

| Size | Contents |
|---|---|
| 4 | the number of maps |
| 8 for each | a map's voice group: the voice group's address in the ROM's memory map, from 0x08000000 (4 bytes), and the number of the map's pairings (4 bytes) |
| 4 for each pairing | 1 for MIDI's drum channel or 0 for another channel, the program, the voice, and a byte of 0 (1 byte each) |

Each map's pairings follow the map's 8 bytes. A pairing gives the voice of the voice group that the MIDI file's notes of
the program play. Programs and voices are numbers from 0 to 127. Hokuto writes the maps in the order of their addresses,
and each map's pairings with the other channels' programs first, in the order of the programs. A reader takes them in
any order. It refuses a voice group with two maps, a program or a voice past 127, and a map that pairs a program of a
channel twice. A map of a voice group that is missing from the ROM does nothing.

### BMAP: the banks' instrument maps

| Size | Contents |
|---|---|
| 4 | the number of maps |
| 12 for each | a map's bank: its index in the archive's bank table (4 bytes), the number of the map's programs (4 bytes), and the number of its drum notes (4 bytes) |
| 8 for each program | the MIDI file's program (4 bytes), and the bank's program that plays it (4 bytes) |
| 8 for each drum note | the note, the key that it takes, and two bytes of 0 (1 byte each), and the bank's program that plays it (4 bytes) |

Each map's pairings follow the map's 12 bytes: its programs, then its drum notes. A MIDI file's programs, its notes and
the keys are numbers from 0 to 127. A bank's programs are numbers from 0 to 65,535. Hokuto writes the maps in the order
of their banks, and each map's pairings in the order of their programs and their notes. A reader takes them in any
order. It refuses a bank with two maps, a number past its range, and a map that pairs a program or a note twice. A map
of a bank that is missing from the archive does nothing.

### EDTR: the view

| Offset | Size | Contents |
|---|---|---|
| 0 | 4 | the sound: its index in the archive's sound table |
| 4 | 4 | the transport's position |
| 8 | 4 | the cycle's start |
| 12 | 4 | the cycle's end |
| 16 | 1 | the cycle: 1 while it's on, 0 while it's off |
| 17 | 1 | the selected track, or 0xFF for none |
| 18 | 1 | the track that the Key Editor shows, or 0xFF while the editor is closed |
| 19 | 1 | the track that the List Editor shows, or 0xFF while the editor is closed |
| 20 | 4 | the arrangement's width of a beat |
| 24 | 4 | the arrangement's height of a track |
| 28 | 4 | the quarter note at the arrangement's left edge |
| 32 | 4 | the track at the arrangement's top edge |
| 36 | 4 | the muted tracks |
| 40 | 4 | the soloed tracks |
| 44 | 4 | the Key Editor's width of a beat |
| 48 | 4 | the Key Editor's height of a key |
| 52 | 4 | the quarter note at the Key Editor's left edge |
| 56 | 4 | the key at the Key Editor's top edge |
| 60 | 4 | the number of controller lanes |
| 64 | 4 for each | a lane: the kind's number (2 bytes), the index (1 byte) and 0 (1 byte) |
| after the lanes | 1 | the window in front: 0 for the mixer, 1 for the Key Editor, 2 for the List Editor |

The view is what the program showed when the file was saved. The positions, the widths, the heights and the edges are
32-bit floats. Positions count quarter notes from the sound's start. Sizes count points at a display scale of 1. The
track at the top edge counts tracks from the first: at 1.5, the edge cuts the second track's row in half. The key at the
top edge counts keys down from 127: at 120.5, the edge cuts key 121's row in half. A track is the track's number in the
sound's sequence, from 0 to 31. Bit n of the muted or the soloed tracks stands for track n.

A lane shows the commands of one kind, by the kind's number in the table of kinds. Its index is the bus of a `send`,
from 0 for bus A, or the LFO of an LFO's command, from 0. Other kinds take 0. The lanes come in order from the top. The
Key Editor keeps its sizes, its scroll and its lanes while it's closed.

The mixer, the Key Editor and the List Editor share a zone of the main window, and one of them shows there at a time:
the window in front. An editor in front that the view leaves closed, or that opens on a track that the sound lacks,
gives way to the List Editor where it opens, then the Key Editor, then the mixer. A chunk without the byte puts the List
Editor in front: Hokuto opened such a file that way.

Hokuto writes the chunk when it shows a sound of the file. A reader refuses a cycle byte other than 0 and 1, a track
above 31 other than 0xFF, a number that isn't finite, and a size of 0 or less. It refuses a kind or a window in front
that this specification doesn't define, with a message that a later version of Hokuto made the file. It skips any bytes
after the window in front. A later version can add to the chunk there. A view of a sound that the archive lacks, or of a
sound without a sequence, does nothing. A track that the sound lacks gets no selection and no editor. Hokuto takes the
nearest sizes that it offers, and leaves out a lane that its menu of lanes doesn't offer.

### TAGS: the tags

| Size | Contents |
|---|---|
| 4 | the number of the file's tags |
| varies | the file's tags |
| 4 | the number of sounds with tags |
| varies | each sound: its index in the archive's sound table (4 bytes), the number of its tags (4 bytes), and its tags |

A tag is its key, then its value, each as its length in bytes (4 bytes) and its bytes. A key holds 1 to 64 lower-case
ASCII letters, digits and underscores, and starts with a letter. A value is UTF-8, and isn't empty. Hokuto writes a set
of tags in the order of their keys' bytes, and the sounds in the order of their indexes. It writes the chunk when the
file or a sound has a tag.

The keys `title`, `artist`, `game`, `year`, `comment`, `copyright`, `length` and `fade` mean what the same keys of PSF
files' tags mean. `length` is how long a player plays before the fade, such as `2:30`, and `fade` how long the fade
lasts, in seconds. A file can hold any other key.

A reader refuses a key twice in one set, a key outside the rule above, a value of no bytes, and a sound's tags twice. A
sound without tags adds nothing. The tags of a sound that the archive lacks do nothing.

### LNTH: the lengths

| Size | Contents |
|---|---|
| 4 | the number of sounds with lengths |
| 16 or 24 for each | a sound's lengths |

A sound's lengths:

| Offset | Size | Contents |
|---|---|---|
| 0 | 4 | the sound: its index in the archive's sound table |
| 4 | 1 | the flags: 1 for a sound that never makes a sound, and 2 when the loop's ticks follow |
| 5 | 1 | the loops: how many times the sound plays its loop before the fade |
| 6 | 2 | 0 |
| 8 | 4 | the length: the milliseconds before the fade |
| 12 | 4 | the fade's milliseconds |
| 16 | 4 | with flag 2, the tick where the loop starts |
| 20 | 4 | with flag 2, the tick where the loop ends |

A player plays a sound for its length, then fades it out over its fade. A sound that loops plays its loop the number of
times that the loops give before the fade. Its ticks are those of the loop that the program's arrangement shows, in the
sound's sequence. A sound whose notes hold until the game stops it loops without ticks: the arrangement shows no loop. A
sound that plays on past the measure's limit fades there, with 0 loops. A sound that ends has 0 loops and no fade. Its
length runs to the end of its last sound and a short silence.

Hokuto measures the lengths of the sequence sounds on the model of the file's engine in strict mixing, with the file's
effects at the buses' volumes. On the 3DS, the model plays without effects. Each sound's variables take the engine's
starting values, without the scenes. A sound plays until it has played its loop twice, ends, or stays silent for 180 s,
for 600 s at most. When a sound reaches the limit inside counted loops, it plays again. Each open track's outermost loop
then loops for ever where it's a counted loop. The passes of those loops count as the sound's loops. A sound that loops
then fades over 10 s. A sound that ends gets 0.5 s of silence after its last sound. The rips of the PSF family measure
their sequences the same way. The tags `length` and `fade` go before the chunk. After an edit that can change what a
sound plays, a writer drops the chunk: a change of the engine, the mixer, a sequence or the archive. A change of a
mixing mode keeps the chunk.

Hokuto writes the sounds in the order of their indexes. A reader takes them in any order, and skips the two zeros. It
refuses a sound twice, a flag that this specification doesn't define, the loop's ticks with 0 loops, and a loop that
doesn't end after its start. Such a flag gets a message that a later version of Hokuto made the file. The lengths of a
sound that the archive lacks do nothing.

### TMLN: the timelines

| Size | Contents |
|---|---|
| 4 | the number of timelines |
| varies | each timeline |

A timeline:

| Size | Contents |
|---|---|
| 4 | the sequence: its number in SEQS |
| 4 | the number of runs of bars |
| 8 for each | a run: the bars' length in ticks (4 bytes) and the number of bars (4 bytes) |
| 4 | the number of tracks with hints |
| varies | each track's hints |

A track's hints:

| Size | Contents |
|---|---|
| 1 | the track's number |
| 3 | 0 |
| 4, and 4 for each | the blocks before the song's start: their number, then each block's number of commands |
| 4, and 4 for each | the bars kept in the track: their number, then each bar |
| 4, and 4 for each | the bars called: their number, then each bar |

The chunk names the sequences that the program edits as timelines. Each sequence holds a song of NintendoWare's MIDI
converter as the console's bytes hold it. The program edits each other sequence as a program. A file without the
chunk leaves the choice to the program. A chunk without timelines says that the program edits every sequence as a
program.

The hints hold what the song's commands don't show. The converter cut each track of the MIDI file at its bar lines, and
put the bars that repeat into subroutines. The runs give the bar lines from the song's start at tick 0. Bars of the last
run's length go on to the end of every track. The converter put the commands of the bars before the song's start at
tick 0, without their rests: the blocks before the song's start, in order. Each block's number counts the track's
commands at tick 0, from the first. A bar counts from 0 at the song's start. The bars kept in the track are bars that
the converter's rules would put into a subroutine, but the converter left in the track. The bars called are bars that
the rules would leave in the track, but the converter called. A track without such hints has no entry.

Hokuto writes the timelines in the order of their sequences, and the tracks in the order of their numbers. A reader
refuses a timeline of a sequence that the file lacks, a sequence's second timeline, a run with no ticks or no bars,
tracks out of the order of their numbers or a track twice, and a list of bars that doesn't rise.

## The text form

The text form is UTF-8, one statement a line. Lines end in LF, and a reader also takes CR LF. A file starts with a
line of `hok` and the format's version, such as `hok 1.1`: its first four bytes are `hok `. A reader also takes a UTF-8
byte-order mark before them. Hokuto writes none. Indentation doesn't matter. Hokuto indents a label by 4 spaces and a
command by 8. A `#` outside quotes starts a comment that runs to the end of the line. Blank lines don't count.

### Tokens

Spaces and tabs separate a line's tokens:

- **Words** are runs of characters other than spaces, tabs, `#`, `[`, `]`, `:` and `"`. A word runs on through
  parentheses: `random(80,100)` is one word.
- **Marks** are `[`, `]` and `:`.
- **Quoted names** stand between double quotes. In the quotes, `\"` is a quote, `\\` a backslash, and `\x` with two hex
  digits a byte.

A name stands bare when it consists of letters, digits and underscores and isn't one of the words `bytes`, `chunk`,
`engine`, `field`, `from`, `hok`, `if`, `item`, `labels`, `mixing`, `native`, `over`, `padding`, `sequence`,
`sequence_file` and `to`. Any other name takes quotes. Hokuto writes each byte that isn't part of a printable UTF-8
character as `\x` and two hex digits.

### Numbers

A value is a decimal number with an optional minus sign and an optional fraction, such as `100`, `-1.5` or `0.0078125`.
Hokuto writes each value exactly. A fraction of 2^16 takes at most 16 decimal places. A reader rounds a number to the
nearest 1/65,536, and a number halfway between two goes away from zero. The whole part is at most 2^47.

A whole number, such as a key, an index, a size or an offset, is decimal, or hex after `0x`.

An argument is a value, `random(low,high)` with no spaces, or `var(n)` for variable n.

### The header

```
hok 1.1
engine switch
mixing strict
sound_mixing 12 enhanced # BGM_CIRCUIT
sound_engine 12 wii_u # BGM_CIRCUIT
```

The first line gives the format's version. A reader refuses a major version other than 1. `engine` names the engine:
`switch`, `wii_u`, `3ds`, `wii`, `gba`, `ds`, `dse`, `konami`, `rd2`, `brownie`, `ubimilan`, `rare`, `quintet` or
`krawall`. `mixing` names the mixing mode: `strict` or `enhanced`. Without a mixing line, the mode is strict. Each of
the two comes once, before the first sequence. A line `sound_mixing` gives a sound the other mode: the number of its
sound and the mode. Hokuto writes the sound's name from the tables in a comment, and the sounds in the order of ENGN,
after the mixing line. A sound gives its mode once. A line `sound_engine` puts a sound on another engine: the number of
its sound and the engine. Hokuto writes those lines in the order of ENGN too, after the lines of the modes. A sound
gives its engine once, and never `gba`, `dse`, `konami`, `rd2`, `brownie`, `ubimilan`, `rare`, `quintet` or `krawall`.
The lines come before the first sequence.

The tags follow, a line for each:

```
tag title "Hokuto demo"
tag year 2026
sound_tag 12 title "Circuit (last lap)" # BGM_CIRCUIT
```

A tag of the file is `tag`, its key and its value. A tag of a sound is `sound_tag`, the number of its sound, its key and
its value. Hokuto writes the sound's name from the tables in a comment. A value is a name: a word, or text in quotes.
Hokuto writes the file's tags first, then each sound's, in the order of TAGS. A file or a sound gives each key once. The
tags come before the first sequence.

The lengths follow, a line for each sound:

```
sound_length 12 151234 10000 2 96 1632 # BGM_CIRCUIT
sound_length 13 780 0 0 # SE_JUMP
```

A sound's lengths are `sound_length`, the number of its sound, its length and its fade in milliseconds, and its loops.
The ticks where its loop starts and ends follow when LNTH gives them. `silent` ends the line of a sound that never makes
a sound. Hokuto writes the sound's name from the tables in a comment, and the sounds in the order of LNTH. A sound gives
its lengths once. The lengths come before the first sequence.

The effects follow, a line for each, with each bus's effects in the order that they process it:

```
effect A i3dl2_reverb room_gain -1045 room_hf_gain -1000 late_reverb_decay_time 1 late_reverb_hf_decay_ratio 0.2 reflections_gain -310 reflections_delay 0.0046 reverb_gain -602 late_reverb_delay 0.012 late_reverb_diffusion 100 late_reverb_density 61 hf_reference 5000 dry_gain 0.24
```

An effect's line is `effect`, the bus (`A`, `B` or `C`) and the kind of effect, then each parameter's name and value.
The kind `i3dl2_reverb` is the I3DL2 reverb, with the names of MIXR's table. The kinds `reverb_hi`, `reverb_std`,
`chorus` and `delay` are the Wii U's effects, with the names of MIXR's tables and text. The kinds that start with `wii_`
are the Wii's, with the names of MIXR's table of the Wii's kinds. The kinds `3ds_reverb`, `3ds_delay`, `3ds_nn_reverb`,
`3ds_nn_delay` and `3ds_nn_reverb_2011` are the 3DS's effects 12 to 16, with the names of MIXR's tables and text. The
parameters come in any order, each at most once. A parameter that the line leaves out is 0. A reader refuses the line
when 0 lies outside that parameter's limits. A filter's size is such a parameter. Hokuto writes every parameter, in the
table's order. The effects come before the first sequence.

A parameter's value is a decimal number with an optional minus sign, fraction and exponent, such as `-1045`,
`0.0046` or `1e-06`. A reader rounds it to a 32-bit float. It refuses a number that's too large for one. Hokuto writes
each value with the fewest significant digits that read back as the same float. A value of at least 0.00001 and below
10^15 in size has no exponent. Any other value takes an exponent as C's `printf` writes it, such as `1e-06` or
`-3.4028235e+38`.

The buses' volumes follow the effects, a line for each bus whose volume isn't 1:

```
bus_volume A 0.5
```

A bus's volume is `bus_volume`, the bus and the volume, from 0 to 2. The volume is a decimal number, written as a
parameter's value. A bus without a line has a volume of 1. Each bus takes one volume at most. Hokuto writes the lines
in the order of the buses. The volumes come before the first sequence.

The scenes follow the buses' volumes, each after a blank line:

```
scene 12 "Last lap" # BGM_CIRCUIT
    variable 3 1
    variable 16 2
    at 90 variable 3 2
```

A scene's line is `scene`, the number of its sound and its name. Hokuto writes the sound's name from the tables in a
comment. The lines after it give the scene's values. `variable`, a variable's number and a value give the value that
the variable starts with. `at` and a time before them give a change at that time. A time is in seconds, with three
decimal places at most: `90` or `90.005`. A value is a whole number from -32768 to 32767. A scene starts each variable
once at most, and its changes come in the order of their times. Hokuto writes the starting values in the order of the
variables' numbers, then the changes. The scenes come before the first sequence.

The tracks' colours follow the scenes, after a blank line, a line for each:

```
track_colour 12 0 74 127 224 # BGM_CIRCUIT
```

A track's colour is `track_colour`, the number of its sound, the track's number, and the colour's red, green and blue,
each from 0 to 255. Hokuto writes the sound's name from the tables in a comment, and the lines in TCOL's order. A track
takes one colour at most. The colours come before the first sequence.

The instrument maps follow the tracks' colours, after a blank line, a line for each pairing:

```
instrument_map 0x81F2A2C program 33 voice 12
instrument_map 0x81F2A2C drums 36 voice 127
```

A pairing is `instrument_map`, the voice group's address, `program` for a program of another channel than MIDI's drum
channel or `drums` for one of the drum channel, the program, `voice` and the voice, each from 0 to 127. Hokuto writes
the lines in IMAP's order. A map pairs a program of a channel once at most. The banks' maps follow:

```
bank_map 3 program 33 instrument 12
bank_map 3 drum_note 36 instrument 5 key 60
```

A pairing is `bank_map`, the bank's index, then `program` and the MIDI file's program, and `instrument` and the bank's
program. A drum note's is `drum_note` and the note, `instrument` and the bank's program, and `key` and the key that it
takes. Hokuto writes the lines in BMAP's order. A map pairs a program or a note once at most. The maps come before the
first sequence.

The view follows the instrument maps, after a blank line:

```
view sound 12 # BGM_CIRCUIT
view position 16
view cycle 8 40 on
view zoom 18 66
view scroll 4 0
view track 2
view mute 5 6
view solo 2
view key_zoom 64 12
view key_scroll 8 84.5
view lane volume 0
view lane send 1
view key_editor 2
view list_editor 2
view front key_editor
```

A line of the view is `view`, the name of a part of the view, and the part's values, in EDTR's units. `sound` gives the
sound's number. Hokuto writes the sound's name from the tables in a comment. `position` gives the transport's position,
and `cycle` the cycle's start and end and `on` or `off`. `zoom` gives the arrangement's width of a beat and height of a
track, and `scroll` the quarter note at its left edge and the track at its top edge. `track` gives the selected track,
and `mute` and `solo` the numbers of the muted and soloed tracks. `key_zoom` and `key_scroll` give the same for the Key
Editor, with the height of a key and the key at the top edge. Each `lane` gives a lane's kind of command by its name,
and its index. `key_editor` and `list_editor` give the tracks that the open editors show, and `front` gives the window
in front: `mixer`, `key_editor` or `list_editor`. The sound, the tracks and the indexes are whole numbers. The other
values are decimal numbers, written as a parameter's value.

A view gives each part once, and `lane` once for each lane. It gives `sound`, `position`, `cycle`, `zoom`, `scroll`,
`key_zoom` and `key_scroll`. Hokuto leaves out `track`, `mute`, `solo` and each editor's line when they have no value,
and writes the lines in the order above. A view without `front` puts the List Editor in front. So does a chunk without
its byte. The view comes before the first sequence.

The timelines follow the view, after a blank line:

```
timeline 3 bars 192 12 144 1 192 1
timeline 3 track 0 opening 8 8 kept 1 4
timeline 3 track 2 called 7
timeline 7 bars 192 1
```

A timeline's first line is `timeline`, its sequence's number and `bars`, then each run of TMLN as two whole numbers:
the bars' length in ticks and the number of bars. A line for each track with hints follows: `timeline`, the sequence's
number, `track` and the track's number, then the track's lists, each after its word. `opening` gives the blocks before
the song's start, `kept` the bars kept in the track and `called` the bars called. Hokuto leaves out a list without
numbers, and writes the lines in TMLN's order. A file that says that it has no timelines has the line `timeline none`
instead. A timeline gives its first line once, and each track's line once, after the first line. The timelines come
before the first sequence.

### Sequences

`sequence n` starts sequence n. The sequences are numbered from 0, in order. The lines that follow it are its labels
and its commands.

A label's line is its name and a colon. The label names the item that follows it, or the end of the sequence after the
last item. A name that starts with a dot, outside quotes, is local. A local label names a target that has no label, and
the file doesn't keep it. Hokuto names local labels `.L1` onwards, in the order of the items.

The file keeps a sequence's labels in a list. Without a labels line, the list follows the order of the labels' names,
compared byte by byte. A labels line gives the list, with each label once: `labels Song Intro "Final lap"`.

Where its kind has them, a command's line holds these parts in this order:

| Part | Form |
|---|---|
| condition | `if` |
| kind | the kind's name |
| opcode | a whole number, for `other` |
| index | a whole number |
| key and velocity | a whole number and a value, for `note`, and a whole number, for `note_off` |
| value | an argument |
| time | `over` and an argument |
| target | a label, or `@` and a byte offset |
| bytes | two hex digits for each byte, for `data` |

The parts in brackets come after them, in any order:

| Part | Meaning |
|---|---|
| `[index n]`, `[key n]`, `[velocity x]`, `[value a]`, `[opcode n]`, `[bytes HH HH]` | a part that the kind doesn't have |
| `[target n]` or `[target @n]` | a target on a kind without one: an item's number, or a byte offset |
| `[value_bytes n]`, `[unused_time n]`, `[unused_random]`, `[unused_variable]` | the encoder's choices. They require a `sequence_file` of the sequence in the native archive or its group files |
| `[repeated]`, `[arguments n]`, `[length_code n]` | MP2K's choices of the encoder, `[repeated]` of Nintendo R&D2's driver, `[arguments]` of Rare's and `[length_code]` of Quintet's. They require a `sequence_file` of the sequence in a GBA ROM |
| `[sample n]` | the sample that a `note` of Konami's driver plays, and the instrument of a `note` or a `note_off` of Krawall |

A reader checks each command's values by the rules of the console of the native archive or the model. On the Wii, the
archive's build of the Wii's sound library gives the rules. In a GBA ROM, MP2K's bytes give them, and the bytes of
Konami's driver, Nintendo R&D2's, Brownie Brown's, Ubisoft Milan's, Rare's, Quintet's or Krawall in a ROM with that
driver. A value has to be one that the console's bytes hold for the command. A reader refuses any other value, such as a
negative wait or a fraction of a volume. Its message names the line and the values that the command takes. A file
without an archive takes any value. The text form takes every value that the bytes hold. A game's sequence can hold any
of them. Hokuto's editors can take fewer, such as velocities from 0 to 127 and tempos from 1 to 1023 BPM.

### The native archive

`native` and the kind of archive (`switch`, `wii_u`, `3ds`, `wii`, `gba`, `ds`, `dse`, `konami`, `rd2`, `brownie`,
`ubimilan`, `rare`, `quintet` or `krawall`) start the native archive. Where the file holds the game's sizes,
`game_size n` follows: the size of the game's archive in bytes. A native archive gives it once at most, before the
tables. A native archive of the Wii can then give the game's build of NW4R SND, once, before its pieces: `build d` with
the date of the BULD chunk, and `build d october_2006` with its flag. A native archive of a GBA ROM can give `filler`
there, once, before its pieces: bit 0 of NATV's flags. A ROM with Brownie Brown's driver, Ubisoft Milan's or Quintet's
has no `filler` line. A ROM with Konami's driver, Nintendo R&D2's, Brownie Brown's, Ubisoft Milan's, Rare's, Quintet's
or Krawall then gives `revision n`, the driver's revision, and `free_space s padding 0xHH`: where the free space starts,
and the byte that fills what the ROM grows by. `filler_run s n` follows for each filler run, with its start and its
size. A ROM with Quintet's driver then gives `list_room p e fill 0xHH` for each room: the first piece after the list,
the room's end and the byte that fills it. A native archive of a GBA ROM with MP2K can give `note_arguments n`, once,
before its pieces: the argument bytes that a note reads at most, 2 or 3. `note_arguments 2` is bit 1 of NATV's flags,
and a native archive without the line reads 3. The pieces follow in order, numbered from 0. Hokuto writes each piece's
number in a comment.
A reader refuses a kind of archive that isn't the file's engine.

- `bytes n` is a run of n bytes. The bytes follow on the next lines in base64, with padding. Each line holds whole
  groups of four characters, and padding can end any line. Hokuto writes 76 characters a line.
- `sequence_file s version v` is a sequence file of sequence s, with the file's version. In a GBA ROM, it is
  `sequence_file s room n`: a song of sequence s, with the n bytes of its place. A ROM with Konami's driver, Nintendo
  R&D2's, Brownie Brown's, Ubisoft Milan's, Rare's, Quintet's or Krawall takes the same form.
- `padding a` is padding to a multiple of a bytes.

`field p from q to r` is a field that lies at place p and holds the distance from q to r. In a set of DSE's files and a
ROM with Konami's or Quintet's driver, `bytes 2` ends the line of a field of 2 bytes. Hokuto writes the fields after the
pieces, and the tables after the fields. A place takes one of these forms:

| Place | Form |
|---|---|
| the start of piece n | `n` |
| byte b of piece n | `n+b`, with b in hex after `0x` or in decimal |
| an item of the sequence of piece n | `n item i`, where i is the item's number or a label of the sequence |
| an item of sequence s | `sequence s item i`, where i is the item's number or a label of the sequence |
| the archive's end, or b bytes past it | `end` or `end+b` |

Hokuto names an item past the first by a label. Targets go by labels the same way. A local label names an item that
has no label, and a label whose name reads as a number takes quotes. An edit can then add or remove commands before the
item, and the field keeps its item. Item 0 stays a number: a sound's start counts from the bytecode's first item.

```
native switch
    game_size 4096
    bytes 1984 # piece 0
        ...
    sequence_file 0 version 0x20000 # piece 1
    padding 64 # piece 2
    bytes 832 # piece 3
        ...
    field 0+0xC from 0 to end
    field 0+0x6A8 from 1 to 2
    field 0+0x6C4 from 0+0x750 to 3
    field 0+0x4E4 from 1 item 0 to 1 item Song_Loop
    tables
        sound Song sequence player 0x4000000 volume 100 tracks 0x3 banks 0x3000000 none # ID 0x1000000
        sound Click wave_sound player 0x4000000 # ID 0x1000001
        bank BANK_SONG # ID 0x3000000
        wave_archive WARC_SONG # ID 0x5000000
        player PLAYER_ALL sound_limit 4 # ID 0x4000000
        group GROUP_SONG game_size 1088 # ID 0x6000000
```

The first field holds the archive's size. The second holds the size of sequence 0's file: from its start to the
padding after it. The third holds where the next file starts, counted from a point in piece 0. The last holds where a
sound starts in the bytecode of sequence 0: at the label `Song_Loop`.

`tables` starts the tables. A line follows for each item, with the item's ID in a comment:

- `sound name kind`, then the sound's values. The kinds are `sequence`, `wave_sound`, `stream` and `unknown`. A value
  is its name and a number: `player`, `volume`, `pan_mode`, `pan_curve`, `player_priority`, `actor_player`,
  `channel_priority` or `tracks`. `release_priority_fix` stands alone and sets the flag. `banks` takes the item IDs of
  the bank slots after it, with `none` for an empty slot. The values come in any order. A value that the line leaves
  out takes its default: 127 for the volume, 64 for the priorities, 1 for the tracks, no bank slots, and 0 for the
  rest. Hokuto writes the values that differ from their defaults, with the bank slots last.
- `bank name`, `wave_archive name` and `group name`. Where the native archive gives its `game_size`, each group's line
  ends with `game_size n` too: the size of the files that the group loads. Without it, no group's line gives one.
- `player name sound_limit n`.

In the example, the game's archive had 4,096 bytes, and the group GROUP_SONG loaded 1,088. The sound Song plays on the
player PLAYER_ALL at a volume of 100 and allocates tracks 0 and 1. Its first bank slot holds the bank BANK_SONG, and the
second is empty. The sound Click is a wave sound.

### Group files

The group files take the form of their chunk: `chunk GRPF n`, then the chunk's n bytes in base64 on the next lines.
They belong to the native archive and come after it. Their sequence files name the sequences by their numbers.

### Streams

The streams take the form of their chunk too: `chunk STRM n`, then the chunk's n bytes in base64. They belong to the
native archive and come after its group files.

### The cartridge image

The cartridge image takes the form of its chunk: `chunk CART n`, then the chunk's n bytes in base64. It belongs to a
native archive of the DS or a set of DSE's files, and comes after the archive, once.

### The model

The model takes the form of its two chunks: `chunk SNDS n` and `chunk ITMS n`, each with the chunk's n bytes in base64
on the next lines. They come after the sequences, in place of the native archive. A reader refuses either chunk in a
file with a native archive, either chunk twice, and one without the other.

### Banks and waves

The models of the edited banks and waves take the form of their chunks: `chunk BANK n` and `chunk WAVE n`, each with
the chunk's n bytes in base64 on the next lines. They come after the archive: after the native archive's group files
and streams, or after the model. A reader refuses either chunk before the archive, and either chunk twice.

### Other chunks

`chunk ID n` holds a chunk that Hokuto doesn't read: its ID, then its n bytes in base64 on the next lines. An ID that
isn't a bare name takes quotes.

The parts of a file come in this order: the header with its engine and mixing lines, tags, lengths, effects, buses'
volumes, scenes, tracks' colours, instrument maps, view and timelines, the sequences, the native archive with its group
files, streams and cartridge image or the model, the banks and the waves, and the other chunks.

### An example

```
hok 1.1
engine switch
mixing strict

sequence 0
    "Main theme":
        allocate_tracks 3
        open_track 1 .L2
        tempo 132
    .L1:
        volume 100
        note 60 100 48
        wait 48
        volume random(80,100) over 24
        note 62 90.5 96
        wait 96
        jump .L1
    .L2:
        pan -32
        if note 55 100 var(3)
        end
```

The sound "Main theme" starts sequence 0 on tracks 0 and 1. Track 0 sets the tempo. From `.L1` on, it sets the
volume, plays two notes and jumps back to `.L1`. Before the second note, the volume moves over 24 ticks to a random
level from 80 to 100. Track 1 starts at `.L2` and pans to the left. If the last comparison held, it plays a note whose
length is the value of variable 3. The file has no native archive and no model. It holds no archive to export.

## Conformance

The conformance suite in `tests/conformance/` checks a player against this specification. It holds synthetic files, made
without data from any game: one for each mixing mode of the engines of the Switch, the Wii U, the 3DS and the Wii. The
engines of the GBA and the DS have no file yet. The suite gives the SHA-256 of the reference implementation's render of
each file's sound. A player conforms when its renders give the same hashes. The suite's `README.md` gives the details.

## Versions

A major version changes when a reader of an earlier version can't read the new files. A reader refuses a file of another
major version. A minor version adds chunks that a reader of an earlier version can keep without reading them. A new kind
of command, engine, archive or sound takes a new number. A reader refuses a number that its version of the specification
doesn't define, with a message that a later version of Hokuto made the file.

Version 1.0 is the first. Version 1.1 adds GBA ROMs: engine 5, archive kind 5, command kinds 77 to 93 and the choices of
a GBA ROM's sequence pieces. Version 1.1 also adds the DS (engine 6, archive kind 6), DSE (engine 7, archive kind 7), a
ROM with Konami's driver (engine 8, archive kind 8, and bit 12 of an item's flags), a ROM with Nintendo R&D2's driver
(engine 9, archive kind 9), a ROM with Brownie Brown's driver (engine 10, archive kind 10), a ROM with Ubisoft Milan's
driver (engine 11, archive kind 11), a ROM with Rare's driver (engine 12, archive kind 12), a ROM with Quintet's driver
(engine 13, archive kind 13) and a ROM with Krawall (engine 14, archive kind 14). A reader of version 1.0 refuses a
file of the GBA or the DS, and reads every other file of version 1.1.

Hokuto 1.0.0 writes version 1.0. Hokuto 2.0.0 writes version 1.1, and reads both.

## The hok tool

The `hok` tool converts between the two forms, and between `.hok` files and the consoles' sound archives, GBA ROMs or DS
cartridge images. It also puts a file's archive in the model form, and measures the lengths of a file's sounds. The
[user guide](guide.md) describes it.
