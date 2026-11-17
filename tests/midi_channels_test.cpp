// SPDX-License-Identifier: MIT

// Tests the settings of a MIDI channel that two tracks of a MIDI file play (src/edit/midi_import.cpp). In a MIDI file,
// a program change, a control change or a pitch bend sets the channel, whichever track contains it. The song gives
// each pair of a MIDI track and a channel a track. Each track of a channel has to contain the settings of the channel's
// other tracks at their ticks. At a tick, the settings of an earlier MIDI track come before the track's commands, and
// those of a later one after them. A copied setting that repeats the value that the track has there is left out, but
// for the first setting of each kind in the loop, which the track plays again after the loop's end. A portamento's
// starting key, which acts on one note, stays. A track of another channel, or of the same channel on another MIDI
// port, takes none of them. A drum note's pairing follows the program that another track of the drum channel selects.

#include <algorithm>
#include <cstdint>
#include <string>
#include <tuple>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/commands/units.h"
#include "seqlib/midi/builder.h"
#include "xsflib/testing/check.h"

#include "edit/converter.h"
#include "edit/midi_songs.h"
#include "format/file.h"
#include "midi_test_support.h"

namespace
{

namespace format = hokuto::format;
namespace midi = seqlib::midi;
using hokuto::edit::MidiSong;
using hokuto::edit::SongOfMidi;
using hokuto::testing::OfKind;
using ntdlib::commands::Command;
using ntdlib::commands::Kind;
using ntdlib::commands::kOne;
using ntdlib::commands::Sequence;

// A command of a track at its tick: its kind, its index, its key, its value and its velocity.
using Timed = std::tuple<uint32_t, Kind, uint8_t, uint8_t, int64_t, int64_t>;

// The commands of the track that starts at label `label` of `sequence`, a song laid out without subroutines, at their
// ticks, up to the track's end. A wait moves the tick and yields no command.
std::vector<Timed> TrackAt(const Sequence& sequence, const std::string& label)
{
    std::vector<Timed> commands;
    uint32_t item = 0;
    for (const auto& known : sequence.labels)
    {
        if (known.name == label)
        {
            item = known.item;
        }
    }

    uint32_t tick = 0;
    for (; item < sequence.items.size(); ++item)
    {
        const Command& command = sequence.items[item];
        if (command.kind == Kind::kWait)
        {
            tick += static_cast<uint32_t>(command.value.value / kOne);
            continue;
        }

        commands.emplace_back(tick, command.kind, command.index, command.key, command.value.value, command.velocity);
        if (command.kind == Kind::kJump || command.kind == Kind::kEnd)
        {
            break;
        }
    }

    return commands;
}

// The commands of `commands` other than notes and the end, in their order.
std::vector<Timed> SettingsOf(const std::vector<Timed>& commands)
{
    std::vector<Timed> settings;
    for (const Timed& command : commands)
    {
        const Kind kind = std::get<1>(command);
        if (kind != Kind::kNote && kind != Kind::kEnd && kind != Kind::kNoteWait && kind != Kind::kTempo)
        {
            settings.push_back(command);
        }
    }

    return settings;
}

// A setting of `kind` at `tick` to `value`, with `index`.
Timed SettingAt(uint32_t tick, Kind kind, int64_t value, uint8_t index = 0)
{
    return {tick, kind, index, 0, value, 0};
}

// A MIDI file at 480 ticks a quarter note, as a converter writes a driver whose tracks share MIDI channels. The first
// track contains the tempo. The second track sets channel 1: its program, volume, second volume, pan, send A, a bend
// range of 12 semitones and a pitch bend at tick 0, and a volume of 60 at the third beat. It plays a note on the first
// and third beats. The third track plays channel 1 too. At tick 0 it sets the volume to 90 again and the pan to the
// centre, and it plays a note on the second and fourth beats. The fourth track plays a note on channel 2.
std::vector<uint8_t> SharedChannel()
{
    midi::FileBuilder file(480);
    midi::TrackBuilder& conductor = file.AddTrack();
    conductor.Tempo(0, 500000);

    midi::TrackBuilder& first = file.AddTrack();
    first.Program(0, 0, 5);
    first.Control(0, 0, 7, 90);
    first.Control(0, 0, 11, 100);
    first.Control(0, 0, 10, 32);
    first.Control(0, 0, 91, 40);
    first.BendRange(0, 0, 12);
    first.PitchBend(0, 0, 8192 + 4096);
    first.NoteOn(0, 0, 60, 100);
    first.NoteOff(480, 0, 60);
    first.Control(960, 0, 7, 60);
    first.NoteOn(960, 0, 62, 100);
    first.NoteOff(1440, 0, 62);

    midi::TrackBuilder& second = file.AddTrack();
    second.Control(0, 0, 7, 90);
    second.Control(0, 0, 10, 64);
    second.NoteOn(480, 0, 64, 80);
    second.NoteOff(960, 0, 64);
    second.NoteOn(1440, 0, 65, 80);
    second.NoteOff(1920, 0, 65);

    midi::TrackBuilder& other = file.AddTrack();
    other.NoteOn(0, 1, 67, 90);
    other.NoteOff(480, 1, 67);

    return file.Encode();
}

// Both tracks of channel 1 contain the channel's settings, and the track of channel 2 contains none.
void TestSharedChannel()
{
    const MidiSong song = SongOfMidi(SharedChannel(), "shared");
    const std::vector<Timed> first = TrackAt(song.timeline, "SMF_shared_Track_0");
    const std::vector<Timed> second = TrackAt(song.timeline, "SMF_shared_Track_1");
    const std::vector<Timed> other = TrackAt(song.timeline, "SMF_shared_Track_2");

    // The settings at tick 0 in the order of the MIDI file's tracks, then the volume of the third beat. The second
    // track's volume of 90 repeats the first track's, and only the track that contains it keeps it. Its pan sets the
    // centre after the first track's pan, in both tracks.
    const std::vector<Timed> opening = {
        SettingAt(0, Kind::kProgram, 5 * kOne),   SettingAt(0, Kind::kVolume, 90 * kOne),
        SettingAt(0, Kind::kVolume2, 100 * kOne), SettingAt(0, Kind::kPan, -32 * kOne),
        SettingAt(0, Kind::kSend, 40 * kOne),     SettingAt(0, Kind::kBendRange, 1200 * kOne),
        SettingAt(0, Kind::kPitchBend, kOne / 2),
    };
    const std::vector<Timed> first_settings = SettingsOf(first);
    const std::vector<Timed> second_settings = SettingsOf(second);

    // Each track has every setting of the opening, the volume once, and the pans in the order of the MIDI file.
    for (const std::vector<Timed>* settings : {&first_settings, &second_settings})
    {
        for (const Timed& setting : opening)
        {
            XSFLIB_CHECK(std::ranges::count(*settings, setting) == 1);
        }

        XSFLIB_CHECK(OfKind(*settings, Kind::kPan) ==
                     (std::vector<Timed>{SettingAt(0, Kind::kPan, -32 * kOne), SettingAt(0, Kind::kPan, 0)}));
        XSFLIB_CHECK(OfKind(*settings, Kind::kVolume) == (std::vector<Timed>{SettingAt(0, Kind::kVolume, 90 * kOne),
                                                                             SettingAt(96, Kind::kVolume, 60 * kOne)}));
        XSFLIB_CHECK(settings->size() == opening.size() + 2);
    }

    // In the second track, the first track's settings come first at tick 0, and the notes keep their ticks.
    XSFLIB_CHECK(second_settings.front() == SettingAt(0, Kind::kProgram, 5 * kOne));
    XSFLIB_CHECK(OfKind(second, Kind::kNote) == (std::vector<Timed>{{48, Kind::kNote, 0, 64, 48 * kOne, 80 * kOne},
                                                                    {144, Kind::kNote, 0, 65, 48 * kOne, 80 * kOne}}));
    XSFLIB_CHECK(OfKind(first, Kind::kNote) == (std::vector<Timed>{{0, Kind::kNote, 0, 60, 48 * kOne, 100 * kOne},
                                                                   {96, Kind::kNote, 0, 62, 48 * kOne, 100 * kOne}}));

    // Channel 2 takes none of channel 1's settings.
    XSFLIB_CHECK(SettingsOf(other).empty());
    XSFLIB_CHECK(OfKind(other, Kind::kNote).size() == 1);

    // The converter's layout with subroutines gives the song back.
    const Sequence laid_out = hokuto::edit::LayOutConverted(song.timeline, song.hints, true);
    XSFLIB_CHECK(hokuto::edit::LayOutConverted(laid_out, song.hints, false) == song.timeline);
}

// A channel that one track plays keeps its commands: the same file without the second track gives the first track's
// commands alone.
void TestOneTrack()
{
    midi::FileBuilder file(480);
    midi::TrackBuilder& track = file.AddTrack();
    track.Program(0, 0, 5);
    track.Control(0, 0, 7, 90);
    track.Control(0, 0, 7, 90);
    track.NoteOn(0, 0, 60, 100);
    track.NoteOff(480, 0, 60);
    const MidiSong song = SongOfMidi(file.Encode(), "one");
    XSFLIB_CHECK(SettingsOf(TrackAt(song.timeline, "SMF_one_Track_0")) ==
                 (std::vector<Timed>{SettingAt(0, Kind::kProgram, 5 * kOne), SettingAt(0, Kind::kVolume, 90 * kOne),
                                     SettingAt(0, Kind::kVolume, 90 * kOne)}));
}

// The volume that a loop restates at its start reaches the other track of the channel, and so does each value that
// changes inside the loop. From the second pass on, the loop's start plays the restated volume.
void TestLoop()
{
    midi::FileBuilder file(480);
    midi::TrackBuilder& conductor = file.AddTrack();
    conductor.Tempo(0, 500000);
    conductor.Meta(1920, 0x06, "loopStart");
    conductor.Meta(3840, 0x06, "loopEnd");
    midi::TrackBuilder& setter = file.AddTrack();
    setter.Control(0, 0, 7, 100);
    setter.NoteOn(0, 0, 60, 100);
    setter.NoteOff(480, 0, 60);
    setter.Control(1920, 0, 7, 100);
    setter.Control(2880, 0, 7, 50);
    setter.SetEnd(3840);
    midi::TrackBuilder& player = file.AddTrack();
    player.NoteOn(1920, 0, 64, 80);
    player.NoteOff(2400, 0, 64);
    player.NoteOn(2880, 0, 65, 80);
    player.NoteOff(3360, 0, 65);
    player.SetEnd(3840);

    const MidiSong song = SongOfMidi(file.Encode(), "loop");

    const std::vector<Timed> volumes = {SettingAt(0, Kind::kVolume, 100 * kOne),
                                        SettingAt(192, Kind::kVolume, 100 * kOne),
                                        SettingAt(288, Kind::kVolume, 50 * kOne)};
    XSFLIB_CHECK(OfKind(TrackAt(song.timeline, "SMF_loop_Track_0"), Kind::kVolume) == volumes);
    XSFLIB_CHECK(OfKind(TrackAt(song.timeline, "SMF_loop_Track_1"), Kind::kVolume) == volumes);
}

// A portamento's starting key reaches the other track each time, and a track of channel 1 on another MIDI port takes
// no setting of port 0's channel 1.
void TestPortamentoAndPorts()
{
    midi::FileBuilder file(480);
    midi::TrackBuilder& first = file.AddTrack();
    first.Meta(0, 0x21, std::string(1, '\0'));
    first.Control(0, 0, 7, 90);
    first.Control(0, 0, 84, 60);
    first.NoteOn(0, 0, 62, 100);
    first.NoteOff(480, 0, 62);
    first.Control(480, 0, 84, 60);
    first.NoteOn(480, 0, 62, 100);
    first.NoteOff(960, 0, 62);
    midi::TrackBuilder& second = file.AddTrack();
    second.NoteOn(0, 0, 64, 80);
    second.NoteOff(960, 0, 64);
    midi::TrackBuilder& other_port = file.AddTrack();
    other_port.Meta(0, 0x21, std::string(1, '\1'));
    other_port.NoteOn(0, 0, 67, 80);
    other_port.NoteOff(960, 0, 67);

    const MidiSong song = SongOfMidi(file.Encode(), "ports", {.imports = true});

    const std::vector<Timed> second_track = TrackAt(song.timeline, "SMF_ports_Track_1");
    XSFLIB_CHECK(OfKind(second_track, Kind::kPortamentoFrom) ==
                 (std::vector<Timed>{SettingAt(0, Kind::kPortamentoFrom, 60 * kOne),
                                     SettingAt(48, Kind::kPortamentoFrom, 60 * kOne)}));
    XSFLIB_CHECK(OfKind(second_track, Kind::kVolume).size() == 1);
    XSFLIB_CHECK(SettingsOf(TrackAt(song.timeline, "SMF_ports_Track_2")).empty());
}

// Two tracks play MIDI's drum channel. The first selects program 5 and plays key 38. The second plays key 36, which a
// pairing plays as key 40 of program 7, and then key 38 without a pairing. That note plays the file's program 5 again.
void TestDrumPairs()
{
    midi::FileBuilder file(480);
    midi::TrackBuilder& first = file.AddTrack();
    first.Program(0, 9, 5);
    first.NoteOn(0, 9, 38, 100);
    first.NoteOff(240, 9, 38);
    midi::TrackBuilder& second = file.AddTrack();
    second.NoteOn(480, 9, 36, 100);
    second.NoteOff(720, 9, 36);
    second.NoteOn(960, 9, 38, 100);
    second.NoteOff(1200, 9, 38);
    const format::MidiDrumNotes pairs = {{36, {.program = 7, .key = 40}}};

    const MidiSong song = SongOfMidi(file.Encode(), "drums", {.drum_notes = &pairs});

    const std::vector<Timed> second_track = TrackAt(song.timeline, "SMF_drums_Track_1");
    XSFLIB_CHECK(OfKind(second_track, Kind::kProgram) ==
                 (std::vector<Timed>{SettingAt(0, Kind::kProgram, 5 * kOne), SettingAt(48, Kind::kProgram, 7 * kOne),
                                     SettingAt(96, Kind::kProgram, 5 * kOne)}));
    XSFLIB_CHECK(OfKind(second_track, Kind::kNote) ==
                 (std::vector<Timed>{{48, Kind::kNote, 0, 40, 24 * kOne, 100 * kOne},
                                     {96, Kind::kNote, 0, 38, 24 * kOne, 100 * kOne}}));
    XSFLIB_CHECK(OfKind(TrackAt(song.timeline, "SMF_drums_Track_0"), Kind::kProgram) ==
                 (std::vector<Timed>{SettingAt(0, Kind::kProgram, 5 * kOne)}));
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("midi_channels",
                                    []
                                    {
                                        TestSharedChannel();
                                        TestOneTrack();
                                        TestLoop();
                                        TestPortamentoAndPorts();
                                        TestDrumPairs();
                                    });
}
