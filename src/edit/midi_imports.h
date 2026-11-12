// SPDX-License-Identifier: MIT

// File > Import > Sequence… and Replace… of a sequence sound take a MIDI file. The file becomes a song of
// NintendoWare's MIDI converter (SongOfMidi in edit/midi_songs.h, with `imports`). It goes into a file of the Wii U,
// the Switch or the 3DS as that console's commands, with the converter's subroutines, and into a file of the Wii as the
// commands that the file's build reads. The program then edits it as a timeline. It goes into a file of the DS as the
// commands that the driver reads, and the program edits it as a program.
//
// A song uses a timebase of 48 ticks a quarter note, or 96 where that moves fewer of the MIDI file's events. On the DS,
// it uses 48: the DS supports no other.

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "edit/midi_songs.h"
#include "edit/replace.h"
#include "song/archive.h"

namespace hokuto::edit
{

// The bank slots of a sequence's song: control change 0 selects among them.
inline constexpr std::size_t kMidiBankSlots = 4;

// The name of a file without its folders and its extension, for the labels of an imported song.
std::string StemOf(const std::string& file_name);

// Whether `bytes` start as a Standard MIDI File.
bool IsMidi(std::span<const uint8_t> bytes);

// The song of the MIDI file `midi` as an import makes it, with labels from `name`, for timebases up to `most_timebase`.
// `drum_notes` names what plays the notes of MIDI's drum channel. `engine` names the engine that plays the song
// (MidiOptions::engine). Throws std::invalid_argument as SongOfMidi does.
MidiSong ImportedSong(std::span<const uint8_t> midi, const std::string& name, uint32_t most_timebase = 96,
                      const format::MidiDrumNotes& drum_notes = {}, std::string_view engine = "NintendoWare");

// The MIDI file `midi` of the name `file_name` as a sequence to bring into a file of the console `into`. `drum_notes`
// names what plays the notes of MIDI's drum channel. The warnings say what the song leaves out. Throws
// std::invalid_argument with a message for the user.
Incoming ReadIncomingMidi(std::span<const uint8_t> midi, const std::string& file_name, const song::Console& into,
                          const format::MidiDrumNotes& drum_notes = {});

// A MIDI file as a sequence file of the DS, what the song leaves out and the DS changes, and the song's commands before
// the programs of `programs` take their places. The program rows of the windows come from those commands.
struct DsMidiFile
{
    std::vector<uint8_t> file;
    std::vector<std::string> warnings;
    ntdlib::commands::Sequence song;
};

// The MIDI file `midi` of the name `file_name` as a sequence file of the DS. `programs` maps each program of the file
// to the program of the bank that plays it, and `drum_notes` names what plays the notes of MIDI's drum channel. Throws
// std::invalid_argument with a message for the user.
DsMidiFile DsFileOfMidi(std::span<const uint8_t> midi, const std::string& file_name, const ProgramMap& programs = {},
                        const format::MidiDrumNotes& drum_notes = {});

} // namespace hokuto::edit
