// SPDX-License-Identifier: MIT

// A MIDI file as a song of a GBA ROM, by the rules of MusicPlayer2000's bytes (edit/mp2k.h). The import follows the
// conventions of the GBA community's MIDI converters. A MIDI file made for them comes in as they read it.
//
// The song plays 24 ticks to a quarter note. Each event moves to the nearest tick of the song, and the warnings count
// the events that moved. The song starts at the MIDI file's tick 0. Each MIDI channel with notes becomes a track, in
// the order of the channels, whatever the track chunks of the file. The tempo events go into the first track.
// MusicPlayer2000 stores a tempo as half the beats a minute. A tempo is rounded to the nearest one. A file without a
// tempo at tick 0 starts at 120 beats a minute, MIDI's default.
//
// Each track opens with its volume and its voice. A track sets full volume first, unless its volume is set before its
// first note. It selects the voice of program 0 first, unless a program change comes before its first note. A program
// change selects the voice that the map pairs with its program (GbaVoiceMap). Control change 7 sets the volume and 11
// scales it: the volume is their product over 127. Control change 10 sets the pan, 1 the LFO's depth, 21 its speed, 22
// its type, 26 its delay and 24 the fine tune, with 64 the centre. Control change 20 and registered parameter 0 set the
// bend range in semitones. A pitch bend is rounded to the nearest of the 64 steps of the GBA on each side of the
// centre. Control changes 33 and 39 set the track's priority. Control change 13 sets the operation of a memory access,
// 14 its byte of the memory area and 15 the label that a conditional jump goes to. Control change 12 or 16 then makes
// the access with its value. Control change 17 puts the label of its value at its tick. Control change 30 chooses an
// extended command, and 29 or 31 makes it with its value: 8 the echo's level and 9 its length, the only two that the
// converters make.
//
// A note has the velocity of its note-on and lasts the ticks to its note-off: one at least. A note of up to 223 ticks
// keeps its length. A longer one becomes a tie, and an end of the tie at its note-off. Metroid Fusion's sequencer reads
// no extra length after a note (edit/mp2k.h). For it, a note whose length matches no code becomes a tie too. Settings
// at a tick come before its notes, and an end of a tie before both.
//
// A text event "[", or loopStart or loop_start in any case, starts the loop. "]", loopEnd or loop_end ends it. Without
// such a start, control change 111 starts it. A loop without an end ends at the file's end. Every track then gets a
// jump back to the start of the loop at its end. A tie that sounds over the loop's end ends there, and the events after
// it stay out. Without a loop, each track ends where the track chunks that contain the events of its channel end, or
// once its last note has ended where that comes later. A track's end cuts the releases of its notes. Text events also
// set the LFO's type ("modt="), its speed ("lfos="), its delay ("lfodl="), the fine tune ("tune=", from -64 to 63) and
// the track's priority ("prio=") on the tracks of the channels of their track chunk, or on every track from a chunk
// without channel events. "modt_global=", "lfos_global=" and "lfodl_global=" set them at the start of every track.
// "modscale_global=" scales the LFO's depth, "mvl=" scales every volume by its value over 128, and "nat=1" applies a
// natural curve to the volumes and the velocities: their fraction of the most to the power 10/6. "pri=" sets the song's
// priority and "rev=" its reverb. The warnings count what the song leaves out, such as other controllers, aftertouch,
// system exclusive events and the events after the loop's end.

#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/mp2k/song.h"

#include "edit/replace.h"
#include "format/file.h"
#include "song/gba.h"
#include "song/sequence.h"

namespace hokuto::edit
{

// The ticks of a GBA song's quarter note.
inline constexpr uint32_t kGbaTimebase = 24;

// An instrument that a MIDI file's notes play: a program, on MIDI's drum channel, 10, or on another channel.
using GbaMidiInstrument = format::MidiInstrument;

// The voice of the song's voice group that each instrument plays. An instrument that the map leaves out plays the voice
// of the number of its program. The converters give it the same voice.
using GbaVoiceMap = format::InstrumentMap;

// The song of a MIDI file: its commands, the item where each track starts, the instruments that its notes play in the
// order of their first notes, the voice that each of them plays, the priority and the reverb of the song where the file
// sets them, and the warnings, in words for the user. The reverb is the header's byte: 0x80 and the reverb's level, or
// 0 for none.
struct GbaMidiSong
{
    ntdlib::commands::Sequence sequence;
    std::vector<uint32_t> entries;
    std::vector<GbaMidiInstrument> instruments;
    GbaVoiceMap voices;
    std::optional<uint8_t> priority;
    std::optional<uint8_t> reverb;
    std::vector<std::string> warnings;
};

// The song of the MIDI file `midi`, with the voices of `voices`, for a driver whose notes read up to `note_arguments`
// bytes (format::NativeArchive::gba_note_arguments): 2 on Metroid Fusion's sequencer. Throws std::invalid_argument with
// a message for the user for bytes that aren't a MIDI file, a file timed in SMPTE frames, and a file without notes.
GbaMidiSong GbaSongOfMidi(std::span<const uint8_t> midi, const GbaVoiceMap& voices = {}, int note_arguments = 3);

// Whether `bytes` read as a file of assembly (song/gba_assembly.h) rather than a MIDI file: they start neither as a
// MIDI file nor as a RIFF file, and their first 4 KiB hold no zero byte.
bool IsGbaAssembly(std::span<const uint8_t> bytes);

// The song of a file of assembly in the same form as the song of a MIDI file: its commands, the item where each track
// starts, and the priority and the reverb of the header. The song keeps its voice group, and the file's voice group
// stays out. A target that the file states as an address stays that address. The commands decode with notes of up to
// `note_arguments` bytes. Throws std::invalid_argument with a message for the user as song::ReadGbaAssembly does, and
// for commands that overlap or read otherwise from two tracks.
GbaMidiSong GbaSongOfAssembly(std::string_view text, int note_arguments = 3);

// The voice that the import proposes for `instrument`, with the 128 voices of the song's voice group `voices`: the
// voice of the program's number. The converters give an instrument the same voice. On the drum channel, a program whose
// voice isn't a drum kit gets the first drum kit of the voice group, where the group has one.
int ProposedGbaVoice(const GbaMidiInstrument& instrument, std::span<const ntdlib::mp2k::Voice> voices);

// The voices that the import proposes for `instruments` in a song of `file` that plays the voice group at address
// `voice_group`, with its 128 voices `voices`: the voices that the file's instrument map of the voice group pairs with
// them (format::File::instrument_maps), or else the voices of ProposedGbaVoice.
GbaVoiceMap ProposedGbaVoices(const format::File& file, uint32_t voice_group,
                              std::span<const GbaMidiInstrument> instruments,
                              std::span<const ntdlib::mp2k::Voice> voices);

// The address of the voice group that song `song` of `file`, a GBA file, plays: the address that its header states.
// `places` contains where each song plays (GbaSoundPlaces). Nothing for a song whose commands the file keeps among the
// ROM's bytes.
std::optional<uint32_t> GbaVoiceGroupOf(const format::File& file, std::span<const std::optional<SoundPlace>> places,
                                        uint32_t song);

// `file`, a GBA file, with `midi` in the place of the commands of song `song`. `places` contains where each song plays
// (GbaSoundPlaces). Each track of the song's header starts a track of `midi`, in order. A header that has more tracks
// than `midi` ends the others at once. The header gets the priority and the reverb of `midi` where `midi` sets them.
// Every song that plays the same header plays the new commands too. The instrument map of the file for the voice group
// of the song gets the pairings of the voices of `midi`, in place of those that it has for the same instruments. Throws
// std::invalid_argument with a message for the user for a song whose commands the file keeps among the ROM's bytes, and
// a song whose header has fewer tracks than `midi`.
format::File WithGbaMidiSong(const format::File& file, std::span<const std::optional<SoundPlace>> places, uint32_t song,
                             const GbaMidiSong& midi);

// The notes of song `song` of `file`, a GBA file, that collide on the console's channels as the program plays the file
// (format::PlayedBytes): those that later notes cut short, and those that the driver never starts (song::CollisionsOf).
// `rom` is the ROM of an earlier version of the file. An edit of songs leaves the driver's code and tables in place.
// The count therefore uses the detection of MP2K in `rom` (song::GbaRom::info). The Import window counts the notes of
// the new song each time that it makes its view. Nothing for a song that the ROM can't play.
std::optional<song::Collisions> GbaSongCollisions(const format::File& file, const song::GbaRom& rom, int song);

} // namespace hokuto::edit
