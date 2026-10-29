# The .hok format

Version 1.0. This document specifies Hokuto's files: what they hold, their binary form and their text form. The code
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

A file of any console can hold its archive as a model instead: the items of the archive's tables with their values,
and its files, without the game's layout. The sounds' volumes, pans and sends are in Hokuto's units there. A sequence
sound starts at an item of its sequence. A reader lays the archive out from the model as NintendoWare's tools lay one
out. The archive plays the same as the game's. Its bytes can differ.

## What a file holds

- **The engine and the mixing mode.** The engine names a console. Hokuto plays the file's sounds by that console's
  rules. Version 1.0 has four engines: the Switch, the Wii U, the 3DS and the Wii. A sound can play on another engine
  than the file's. Hokuto then converts the sound for that console and plays the result by that console's rules. An
  export to that console makes the same conversion. The archive keeps the sound for the file's engine. A sound of the
  Wii goes to the Wii U first, and from there to the Switch or the 3DS. A sound of the 3DS, the Wii U or the Switch goes
  to the Wii directly, for a build of NW4R SND from June 2008. The mixing mode is strict or enhanced. Strict mixing
  follows the console's hardware, and enhanced mixing lifts its limits. Each sound takes the file's mode, unless the
  file gives the sound the other mode. The Wii's engine plays in strict mixing only.
- **The effects.** The effects on the console's aux buses A to C, like the effect plugins on a DAW's FX channels. A
  track's `send` commands set its levels into the buses. A bus's effects process it in order, and the bus then joins the
  output at its volume. On the Switch, a bus without effects joins the output as it is. On the Wii U, the Wii and the
  3DS, a bus reaches the output only through its effects. The AX libraries of the Wii U and the Wii drop the sends to a
  bus without one. The 3DS's nn::snd leaves such a bus off. The 3DS has buses A and B only. Version 1.0 has sixteen
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

The opcodes are those of the consoles' bytecode, NintendoWare's sequence format. A note's opcode is its key, from 0x00
to 0x7F. Prefixes come before an opcode, in this order: 0xA2 for a condition, then 0xA3, 0xA4 or 0xA5 for a time with a
value, a random value or a variable, then 0xA0 for a random argument or 0xA1 for an argument from a variable. A time of
16 bits follows the command's argument. 0xF0 starts an opcode of two bytes. A `send` takes 0xD9 for bus A, 0xDA for bus
B and 0xDE for bus C. The table gives the opcodes of LFO 0. For LFO n from 1 to 3, an opcode is 0xF0 and a second byte.
The second byte is 0xA0 + 6 (n − 1) for `lfo_curve`. The next five bytes are for `lfo_phase`, `lfo_depth`, `lfo_speed`,
`lfo_type` and `lfo_range`. The second byte is 0xE1 + 2 (n − 1) for `lfo_delay`, and one more for `lfo_period`. The
engines differ in the commands that they run (see "The engines").

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

## The binary form

Numbers are little-endian. A file starts with a header and a table of its chunks. The chunks follow.

### The header

| Offset | Size | Contents |
|---|---|---|
| 0 | 8 | the signature: 89 48 4F 4B 0D 0A 1A 0A |
| 8 | 2 | the major version: 1 |
| 10 | 2 | the minor version: 0 |
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
SEQS, SNDS, ITMS, NATV, BULD, TABL, SIZE, GRPF, STRM, BANK, WAVE, SCEN, TCOL, EDTR, TAGS, LNTH and TMLN, then the other
chunks in the order of the file that they came from. The CRC-32 is zlib's and PNG's: the reflected polynomial
0xEDB88320, starting from 0xFFFFFFFF, with the result inverted. The CRC-32 of the ASCII text `123456789` is 0xCBF43926.

A reader refuses a file:

- whose major version isn't 1
- with a chunk that lies past the end of the file, or whose bytes don't match its CRC-32
- with two chunks of the same ID among ENGN, MIXR, SEQS, SNDS, ITMS, NATV, BULD, TABL, SIZE, GRPF, STRM, BANK, WAVE,
  SCEN, TCOL, EDTR, TAGS, LNTH and TMLN
- with a TABL, SIZE, GRPF or STRM chunk and no NATV chunk
- with a BULD chunk and no NATV chunk of the Wii's kind
- with only one of SNDS and ITMS, or with them and a NATV chunk
- with a BANK or WAVE chunk and no archive: neither a NATV chunk nor SNDS and ITMS
- without an ENGN chunk

MIXR, SEQS, SNDS, ITMS, NATV, BULD, TABL, SIZE, GRPF, STRM, BANK, WAVE, SCEN, TCOL, EDTR, TAGS, LNTH and TMLN are
optional. Hokuto keeps a chunk with any other ID and writes it back unchanged.

### ENGN: the engine

| Size | Contents |
|---|---|
| 1 | the engine: 1 for the Switch, 2 for the Wii U, 3 for the 3DS, 4 for the Wii |
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
doesn't define, with a message that a later version of Hokuto made the file. The mode or the engine of a sound that the
archive lacks does nothing.

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
| 12–15 | 0 |

The parts follow in this order:

| Part | Size | Contents |
|---|---|---|
| value | 8, or 16 when random | the value, then the highest value for a random one |
| time | 8, or 16 when random | the time, in the same way |
| key and velocity | 9 | the key in 1 byte, then the velocity |
| target | 4 | the item's number, or a byte offset |
| opcode | 2 | the opcode |
| bytes | 4 + the bytes | the number of bytes, then the bytes |

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
| 1 | the kind of archive: 1 for the Switch's, 2 for the Wii U's, 3 for the 3DS's, 4 for the Wii's |
| 3 | 0 |
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
| 1 | flags. Bit 0 says that the place is an item. Bit 1 says that the place names a sequence, and comes with bit 0 |
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
line of `hok` and the format's version, such as `hok 1.0`: its first four bytes are `hok `. A reader also takes a UTF-8
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
hok 1.0
engine switch
mixing strict
sound_mixing 12 enhanced # BGM_CIRCUIT
sound_engine 12 wii_u # BGM_CIRCUIT
```

The first line gives the format's version. A reader refuses a major version other than 1. `engine` names the engine:
`switch`, `wii_u`, `3ds` or `wii`. `mixing` names the mixing mode: `strict` or `enhanced`. Without a mixing line, the
mode is strict. Each of the two comes once, before the first sequence. A line `sound_mixing` gives a sound the other
mode: the number of its sound and the mode. Hokuto writes the sound's name from the tables in a comment, and the sounds
in the order of ENGN, after the mixing line. A sound gives its mode once. A line `sound_engine` puts a sound on another
engine: the number of its sound and the engine. Hokuto writes those lines in the order of ENGN too, after the lines
of the modes. A sound gives its engine once. The lines come before the first sequence.

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

The view follows the tracks' colours, after a blank line:

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
| key and velocity | a whole number and a value, for `note` |
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

A reader checks each command's values by the rules of the console of the native archive or the model. On the Wii, the
archive's build of the Wii's sound library gives the rules. A value has to be one that the console's bytes hold for the
command. A reader refuses any other value, such as a negative wait or a fraction of a volume. Its message names the line
and the values that the command takes. A file without an archive takes any value. The text form takes every value that
the bytes hold. A game's sequence can hold any of them. Hokuto's editors can take fewer, such as velocities from 0 to
127 and tempos from 1 to 1023 BPM.

### The native archive

`native` and the kind of archive (`switch`, `wii_u`, `3ds` or `wii`) start the native archive. Where the file holds the
game's sizes, `game_size n` follows: the size of the game's archive in bytes. A native archive gives it once at most,
before the tables. A native archive of the Wii can then give the game's build of NW4R SND, once, before its pieces:
`build d` with the date of the BULD chunk, and `build d october_2006` with its flag. Its pieces follow in order,
numbered from 0. Hokuto writes each piece's number in a comment.

- `bytes n` is a run of n bytes. The bytes follow on the next lines in base64, with padding. Each line holds whole
  groups of four characters, and padding can end any line. Hokuto writes 76 characters a line.
- `sequence_file s version v` is a sequence file of sequence s, with the file's version.
- `padding a` is padding to a multiple of a bytes.

`field p from q to r` is a field that lies at place p and holds the distance from q to r. Hokuto writes the fields
after the pieces, and the tables after the fields. A place takes one of these forms:

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
volumes, scenes, tracks' colours, view and timelines, the sequences, the native archive with its group files and streams
or the model, the banks and the waves, and the other chunks.

### An example

```
hok 1.0
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
without data from any game: one for each engine and each mixing mode that the engine plays. The suite gives the SHA-256
of the reference implementation's render of each file's sound. A player conforms when its renders give the same hashes.
The suite's `README.md` gives the details.

## Versions

A major version changes when a reader of an earlier version can't read the new files. A reader refuses a file of another
major version. A minor version adds chunks that a reader of an earlier version can keep without reading them. A new kind
of command, engine, archive or sound takes a new number. A reader refuses a number that its version of the specification
doesn't define, with a message that a later version of Hokuto made the file.

Version 1.0 is the first.

## The hok tool

The `hok` tool converts between the two forms, and between the consoles' sound archives and `.hok` files. It also puts a
file's archive in the model form, and measures the lengths of a file's sounds. The README describes it.
