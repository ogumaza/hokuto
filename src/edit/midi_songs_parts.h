// SPDX-License-Identifier: MIT

// What midi_songs.cpp and midi_import.cpp share: a track of an imported MIDI file, the settings and tempos that
// its events make, and the handlers of its channel messages. Only those files include them.

#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/commands/sequence.h"

#include "edit/midi_songs.h"

namespace hokuto::edit::midi_songs_parts
{

using ntdlib::commands::Command;
using ntdlib::commands::Kind;

// A command of a track at the MIDI file's tick. `off` contains the note-off tick of a note once the reader reaches the
// note-off in the file. A held note sounds past the MIDI file's end.
struct Placed
{
    uint64_t tick = 0;
    Command command;
    std::optional<uint64_t> off;
    bool held = false;
    bool layer = false; // a command of a layer that joined the track (JoinLayers)
};

// A track of the song as it stands in the MIDI file: its commands in the order of the file, and the tick of its end.
struct MidiTrack
{
    std::vector<Placed> placed;
    uint64_t end = 0;
    std::size_t midi_track = 0; // the track of the MIDI file that contains the channel

    // The registered parameter that data entry sets, and the bend range's cents from its fine data entry.
    uint8_t parameter = 127;
    uint8_t parameter_fine = 127;
    int64_t bend_cents = 0;

    // The bank that control change 0 selects for the next program change, with `selects_banks`.
    uint8_t bank = 0;

    // The program that the file selects on the channel, and the program that the song's commands select: on MIDI's drum
    // channel, a drum note's pairing can select another.
    int file_program = 0;
    int played_program = 0;

    // The notes that sound on each key, by their places in `placed`, the earliest first.
    std::map<uint8_t, std::deque<std::size_t>> sounding;

    void Add(uint64_t tick, Command command)
    {
        placed.push_back({tick, std::move(command), std::nullopt});
    }
};

// A setting of `kind` to `value`, in the unit of the kind in fixed point.
Command Setting(Kind kind, int64_t value, uint8_t index = 0);

// Tempo commands in whole beats a minute that keep the time of `tempos`, a MIDI file's tempos in microseconds a quarter
// note at their ticks, in order from tick 0. The ticks of a song with `timebase` ticks a quarter note count from MIDI
// tick `start`, `division` MIDI ticks a quarter note. Each stretch between a tempo, a bar line of `lines`, the start
// and the end of the loop, and the end of the file gets the whole tempo below or above the tempo of the MIDI file. Of
// the two, the stretch gets the tempo that leaves the time of the song nearest the time of the MIDI file at the end of
// the stretch. The time counts again from the loop's start. A command there sets the loop's first tempo for each pass,
// unless the loop's end and the bar before its start play it already. A command is inserted where the tempo changes.
std::vector<Placed> TimeKeepingTempos(const std::vector<std::pair<uint64_t, uint32_t>>& tempos,
                                      const std::vector<uint64_t>& lines, uint64_t start,
                                      std::optional<uint64_t> loop_start, std::optional<uint64_t> loop_end,
                                      uint64_t file_end, uint16_t division, uint32_t timebase);

// Whether the song keeps a channel message of `status` with the data bytes `data` and `value`, a note-off aside, with
// `options`.
bool Holds(uint8_t status, uint8_t data, uint8_t value, const MidiOptions& options);

// Adds a note-on of key `data` at velocity `value` at `tick` to `track`, a track of MIDI channel `channel`
// (SongOfMidi). On MIDI's drum channel, a pairing of `options` can select the note's program and key.
void PlaceNoteOn(MidiTrack& track, uint64_t tick, uint8_t channel, uint8_t data, uint8_t value,
                 const MidiOptions& options);

// Ends the earliest note of key `data` that sounds in `track` at `tick` (SongOfMidi).
void PlaceNoteOff(MidiTrack& track, uint64_t tick, uint8_t data);

// Adds control change `data` at `value` at `tick` to `track`, with `status` the event's status (SongOfMidi). With
// `exact_low_pass`, control change 102 stays out. `left_out` counts the control changes that stay out, and
// `biquad_values` the values of the biquad filter.
void PlaceControlChange(MidiTrack& track, uint64_t tick, uint8_t status, uint8_t data, uint8_t value,
                        const MidiOptions& options, bool exact_low_pass, std::map<std::string, std::size_t>& left_out,
                        std::size_t& biquad_values);

// Adds the program change to program `data` of `track`'s bank to `track` at `tick` (SongOfMidi).
void PlaceProgramChange(MidiTrack& track, uint64_t tick, uint8_t data);

// Adds the pitch bend of `data` and `value` to `track` at `tick` (SongOfMidi).
void PlacePitchBend(MidiTrack& track, uint64_t tick, uint8_t data, uint8_t value);

} // namespace hokuto::edit::midi_songs_parts
