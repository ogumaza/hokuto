// SPDX-License-Identifier: MIT

// A MIDI file as a song of NintendoWare's MIDI converter (edit/converter.h). The song contains the events of the MIDI
// file as commands at their ticks, every bar in its track. The converter's layout then puts the bars that repeat into
// subroutines. NintendoWare's converter does the same. The program edits the song as a timeline.
//
// Each MIDI track's channel events go into a track of the song, in the order of the file's tracks. A track with events
// on several channels makes a track for each channel, in the order of their first events. A channel gets a track at the
// first of its events that the song keeps. A channel whose events all stay out gets none. A song has 16 tracks at most.
// With `joins_layers`, a file of more channels joins its layers: a MIDI track named after an earlier one and
// " (layer n)" goes into the track of the earlier one. The MIDI file of a converter gives the further channels of a
// track such names (seqlib's MIDI writer). A setting of a layer that only repeats the value that its track has there is
// left out. As in NintendoWare's converter, the song starts at its first note, or at a marker before it. The events
// before the start go to tick 0 in their order, a block for each bar of the MIDI file. The ticks take the song's
// timebase and round down. The time signatures set the bar lines, and a file without them gets bars of 4 quarter notes.
//
// A note has the velocity of its note-on and lasts the ticks to its note-off: one tick at least. NintendoWare's engines
// hold a note of no ticks until its track ends. The tempo events go into the first track. NintendoWare's tempos are
// whole numbers of beats a minute. The song uses the nearest one. With `keeps_time`, the tempo moves between the whole
// tempos below and above the MIDI file's from bar to bar instead, and keeps the MIDI file's time. Control change 7 sets
// the volume, 11 the second volume, 10 the pan, 91 the send to bus A, 93 the send to bus B and 64 the damper.
// Registered parameter 0 sets the bend range, in the nearest whole semitones. With `selects_banks`, control change 0
// selects a bank of 128 programs, and a program change then selects program 128 times the bank plus its number. A
// converter's MIDI file selects a program past 127 of the source's bank so (BankOfSoundFont's kEveryMidiBank). Control
// change 32 of 0 then changes nothing, and stays out of the warnings. A pitch bend is rounded to the nearest of
// NintendoWare's 128 steps on each side of the centre. A program change keeps its program. A marker named loopStart,
// loop_start or Loop Start starts the loop, and one named loopEnd, loop_end or Loop End ends it, in any case and with
// any underscores and spaces (NamesLoop). Without a marker of its start, control change 111 starts it. Every track then
// gets a label at the loop's start and a jump back to it at the loop's end. With `ends_at_loop`, the song leaves out
// the events from the loop's end on, such as a second pass of the loop. A note that sounds over the loop's end still
// lasts until its note-off, and sounds on after the jump. A note before the loop's start whose note-off comes at the
// MIDI file's end sounds on past it. It lasts 200 beats, the length of NintendoWare's held notes. Such a note sounds
// until the game stops the sound. The warnings count the events that the song leaves out, such as other controllers,
// aftertouch and system exclusive events, and the layers that join their tracks.
//
// With `imports`, the song also contains what an import of a MIDI file accepts (edit/midi_imports.h). An event between
// two of the song's ticks moves to the nearer one, and the warnings and `moved` count them. A note of the loop that
// outlasts a pass ends where its next pass starts, and the warnings count such notes. Of more than 16 channels, the
// song keeps the first 16 that play notes, and the warnings name the others. Control change 0 selects a sound's bank
// slot, 0 to 3, and a higher value stays out. A SoundFont's MIDI banks go into new banks in the same way
// (edit/sound_font_imports.h). Control change 1 sets the first LFO's depth, 5 the portamento's time, 65 turns the
// portamento on and off and 84 sets its starting key. 72, 73 and 75 set the release, the attack and the decay, each
// value as the consoles' commands hold it. 74 sets the low-pass as the consoles' command does, with 64 for none. The
// 103 of the converters contains 64 less the same value, and their 102 lowers a cutoff by 75 cents a step: without any
// 103, the song uses the low-pass nearest it. 104 sets the biquad filter's value, and no event sets its type. 126 turns
// monophonic play on, and 127 off. Registered parameter 2 sets the transposition in semitones from 64.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ntdlib/commands/sequence.h"

#include "format/file.h"

namespace hokuto::edit
{

// How a MIDI file becomes a song.
struct MidiOptions
{
    uint32_t timebase = 48;     // the song's ticks a quarter note
    bool from_tick_0 = false;   // the song starts at the MIDI file's tick 0, before its first note
    bool keeps_time = false;    // whole tempos from bar to bar keep the MIDI file's time (TimeKeepingTempos)
    bool ends_at_loop = false;  // the events from the loop's end on stay out, but a note-off there ends its note
    bool joins_layers = false;  // more than 16 channels join the layers into their tracks
    bool selects_banks = false; // control change 0 selects a bank of 128 programs
    bool imports = false;       // an import's rules: more controllers, the nearest ticks, the first 16 channels

    // The engine that plays the song. The warning about whole tempos names it: "NintendoWare", or "DS" and "DSE". The
    // tempo commands of the DS and DSE hold whole beats a minute too.
    std::string_view engine = "NintendoWare";

    // What plays the notes of MIDI's drum channel, 10. A note with a pairing gets the program and the key of the
    // pairing. Each other note on the channel plays as it stands in the file.
    const format::MidiDrumNotes* drum_notes = nullptr;
};

// A MIDI file's song, laid out with every bar in its track, and the converter's hints for its layout with subroutines
// (LayOutConverted). The warnings say what the song leaves out, in words for the user.
struct MidiSong
{
    ntdlib::commands::Sequence timeline;
    format::ConverterHints hints;
    std::vector<std::string> warnings;
    std::size_t moved = 0; // with `imports`, the events that moved to the nearer tick
};

// The song of the MIDI file `midi`, with labels from `name`: SMF_, the name, an underscore and the label's part, such
// as SMF_name_Track_0. A character of the name other than a letter, a digit or an underscore becomes an underscore.
// Throws std::invalid_argument with a message for the user for bytes that aren't a MIDI file, a file timed in SMPTE
// frames, more than 16 tracks of events, no notes, and a loop that doesn't end after its start.
MidiSong SongOfMidi(std::span<const uint8_t> midi, const std::string& name, const MidiOptions& options = {});

// The notes that a MIDI file plays on MIDI's drum channel, 10, from the lowest, and the bank slot that control change 0
// selects on that channel before its first note.
struct MidiDrums
{
    int slot = 0;
    std::vector<int> notes;
};

// The drum notes of the MIDI file `midi`. A file that can't be read has none.
MidiDrums DrumNotesOf(std::span<const uint8_t> midi);

} // namespace hokuto::edit
