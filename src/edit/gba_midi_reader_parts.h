// SPDX-License-Identifier: MIT

// What gba_midi_reader.cpp and gba_midi_tracks.cpp share: the GbaMidiReader class of the MIDI import as a GBA song,
// the events that it places on each track, and the state of a channel during the read. Only those files include it.

#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "seqlib/midi/reader.h"

#include "edit/gba_midi.h"

namespace hokuto::edit::gba_midi_reader_parts
{

using ntdlib::commands::Command;
using ntdlib::commands::Kind;

// Where an event is placed among the events of its tick: an end of a tie first, then the loop's start, the settings and
// labels, and the notes. A jump back to the loop's start comes after the ends of the ties at the loop's end.
enum Order
{
    kTieEnd,
    kLoop,
    kSetting,
    kNote,
};

// An event of a track at the song's tick, with its order among the events of its tick, the order of the MIDI file, and
// its command. A label of control change 17 has no command. A memory access that jumps goes to label `label`. A program
// change contains the instrument that it selects.
struct Placed
{
    uint64_t tick = 0;
    Order order = kSetting;
    uint64_t serial = 0;
    std::optional<Command> command;
    int label = -1;
    std::optional<GbaMidiInstrument> instrument;
};

// A placed event of `command`, or a label of control change 17 without one.
Placed Place(uint64_t tick, Order order, uint64_t serial, std::optional<Command> command, int label = -1,
             std::optional<GbaMidiInstrument> instrument = std::nullopt);

// A setting of `kind` to the console's value `value`, in the kind's unit.
Command Setting(Kind kind, int64_t value, uint8_t index = 0);

// What the import keeps for the whole song: the loop's ticks in the MIDI file, and the settings of the text events
// for the whole song.
struct Global
{
    std::optional<uint64_t> loop_start;
    std::optional<uint64_t> loop_end;
    std::optional<uint64_t> loop_cc; // the first control change 111
    int master_volume = 128;
    bool natural = false;
    double mod_scale = 1.0;
    std::optional<uint8_t> priority;
    std::optional<uint8_t> reverb;
    std::optional<int> lfo_type;
    std::optional<int> lfo_speed;
    std::optional<int> lfo_delay;
};

// An event of a channel in the order of the MIDI file: its tick, its track chunk and its place there.
struct ChannelEvent
{
    uint64_t tick = 0;
    std::size_t chunk = 0;
    std::size_t index = 0;
    const seqlib::midi::Event* event = nullptr;
};

// What a channel's events set while they become the song's commands (GbaMidiReader::ChannelCommands): the commands at
// their ticks, the instrument, the volume and expression, the registered parameter, the memory access's operation,
// byte and label, and the extended command.
struct ChannelState
{
    std::vector<Placed> placed;
    GbaMidiInstrument instrument;
    int volume = 127;
    int expression = 127;
    int parameter = 127;
    int parameter_fine = 127;
    int operation = 0;
    int memory_byte = 0;
    int memory_label = 0;
    int extended = 0;
    std::map<uint8_t, std::deque<std::size_t>> open; // each key's notes without a note-off, by their places
};

// Reads a MIDI file into a GBA song (GbaSongOfMidi). Read runs the steps in order, and each step uses the members that
// the steps before it set.
class GbaMidiReader
{
public:
    GbaMidiReader(const seqlib::midi::File& file, const GbaVoiceMap& voices, int note_arguments)
        : file_(file),
          voices_(voices),
          note_arguments_(note_arguments),
          division_(file.division),
          chunk_channels_(file.tracks.size())
    {
    }

    GbaMidiSong Read();

private:
    // The song's tick nearest a MIDI tick, and whether the MIDI tick falls between two of the song's ticks.
    uint64_t TickOf(uint64_t midi_tick) const;
    bool Moves(uint64_t midi_tick) const;

    // Whether the song's tick `tick` comes at or after the loop's end.
    bool PastLoop(uint64_t tick) const;

    // Reads the loop, the settings for the whole song, the file's end, and the channels of each track chunk.
    void ScanTracks();

    // Reads text event `text` at MIDI tick `tick` for the whole song.
    void ScanText(const std::string& text, uint64_t tick);

    // Sets the loop's start and end in the song's ticks.
    void FindLoop();

    // The voice of an instrument.
    int VoiceOf(const GbaMidiInstrument& instrument) const;

    // A volume from control changes 7 and 11, scaled by "mvl=" and curved by "nat=1".
    int VolumeOf(int volume, int expression) const;
    int VelocityOf(int velocity) const;

    // Adds the commands of channel `channel` with `events` as a track. `tempos` contains the tempo events of the file.
    void ChannelCommands(uint8_t channel, const std::vector<ChannelEvent>& events,
                         const std::vector<ChannelEvent>& tempos);

    // Adds `command` at MIDI tick `midi_tick` to the channel's commands.
    void Add(ChannelState& channel, uint64_t midi_tick, Order order, Command command);

    // Adds the tempos of `tempos` to the first track.
    void AddTempos(ChannelState& channel, const std::vector<ChannelEvent>& tempos);

    // Adds the settings for the whole song at the track's opening.
    void AddGlobalSettings(ChannelState& channel);

    // Adds the commands of `event` to the channel's commands.
    void PlaceEvent(ChannelState& channel, const seqlib::midi::Event& event);

    // Ends the channel's earliest open note of key `data` at `event`.
    void EndNote(ChannelState& channel, const seqlib::midi::Event& event, uint8_t data);

    // Adds the setting of a text event of the tracks.
    void PlaceText(ChannelState& channel, const seqlib::midi::Event& event);

    // Adds control change `data` at `value`, with `status` the event's status.
    void PlaceControlChange(ChannelState& channel, const seqlib::midi::Event& event, uint8_t status, uint8_t data,
                            uint8_t value);

    // Selects the parameter of control changes 98 to 101, or adds the bend range of the data entry of control changes 6
    // and 38.
    void PlaceParameter(ChannelState& channel, const seqlib::midi::Event& event, uint8_t status, uint8_t data,
                        uint8_t value);

    // Adds the memory access of control changes 12 and 16 at `value`.
    void PlaceMemoryAccess(ChannelState& channel, const seqlib::midi::Event& event, uint8_t value);

    // Writes the commands of track `t`, with its opening, its waits and its end.
    void TrackCommands(std::size_t t);

    // Splits the notes of `placed` that are too long for a note's bytes into ties and the ties' ends.
    void SplitTies(std::vector<Placed>& placed) const;

    // Adds the track's opening to `placed`, a track on the drum channel with `drums`.
    void AddOpening(std::vector<Placed>& placed, bool drums) const;

    // Leaves out the events of `placed` from the loop's end on.
    void DropAfterLoop(std::vector<Placed>& placed);

    // Adds the instruments that the notes of `placed` play to the song.
    void AddInstruments(const std::vector<Placed>& placed);

    // Leaves out the jumps of `placed` to labels that the track doesn't hold.
    void DropMissingLabels(std::vector<Placed>& placed);

    // Writes the items of `placed` into the song's sequence, the track's end at `track_end` at the earliest.
    void WriteItems(const std::vector<Placed>& placed, uint64_t track_end);

    // Adds the warnings that count what the song moved or left out, and the voices of the song's instruments.
    void AddWarnings();

    const seqlib::midi::File& file_;
    const GbaVoiceMap& voices_;
    const int note_arguments_;
    const uint64_t division_;
    GbaMidiSong song_;
    std::map<std::string, std::size_t> left_out_;
    std::size_t moved_ = 0;

    // The loop, the settings for the whole song, the file's end, and the channels of each track chunk.
    Global global_;
    uint64_t file_end_ = 0;
    std::vector<std::set<uint8_t>> chunk_channels_;
    std::set<uint8_t> note_channels_;

    // The loop's start and end, in the song's ticks.
    std::optional<uint64_t> loop_start_;
    std::optional<uint64_t> loop_end_;

    // Each channel's events as the song's commands at their ticks.
    uint64_t serial_ = 0;
    std::size_t tempo_steps_ = 0;
    std::vector<std::vector<Placed>> tracks_;
    std::vector<bool> drum_tracks_;
    std::vector<uint64_t> track_ends_; // the latest end of the track chunks that hold each channel's events
};

} // namespace hokuto::edit::gba_midi_reader_parts
