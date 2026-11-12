// SPDX-License-Identifier: MIT

// Tests the songs of MIDI files for GBA ROMs (src/edit/gba_midi.h). A MIDI file of three channels sets the settings
// that a track uses, with a long note, a bend and events between the GBA's ticks. Each event has to come as a command
// at its nearest tick, and the warnings have to count what moved and what stays out. Loop markers, control change 111
// and a loop without an end give the loop: each track has to jump back to its start, and a tie over its end has to end
// there. Memory accesses with labels, extended commands and the text events of the GBA community's converters have to
// give their commands. A MIDI file imports into a song of a small cartridge: ntdlib's performer has to play its notes,
// tempo and volumes from the exported ROM, and the header has to get its priority and reverb. Every wait has to be a
// length that a code of MusicPlayer2000 encodes. The count of the new song's notes that never start has to follow
// MusicPlayer2000's rule for the PSG's channels.

#include "edit/gba_midi.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

#include "ntdlib/agb/rom.h"
#include "ntdlib/commands/sequence.h"
#include "ntdlib/commands/units.h"
#include "ntdlib/mp2k/commands.h"
#include "ntdlib/mp2k/song.h"
#include "ntdlib/testing/mp2k_cart.h"
#include "seqlib/midi/builder.h"
#include "xsflib/testing/check.h"

#include "cart_bytes.h"
#include "edit/replace.h"
#include "format/archive.h"
#include "format/file.h"
#include "midi_test_support.h"
#include "song/gba.h"
#include "song/sequence.h"

namespace
{

namespace midi = seqlib::midi;
namespace mp2k_cart = ntdlib::testing::mp2k_cart;
using hokuto::edit::GbaMidiInstrument;
using hokuto::edit::GbaMidiSong;
using hokuto::edit::GbaSongOfMidi;
using hokuto::edit::GbaVoiceMap;
using hokuto::format::File;
using hokuto::testing::OfKind;
using ntdlib::commands::Command;
using ntdlib::commands::Kind;
using ntdlib::commands::kOne;
using ntdlib::commands::Source;
using xsflib::testing::ErrorOf;

// A command of a track at its tick: its kind, its index, its key, the console's value and its velocity. A value from a
// variable is the number of the variable.
using Timed = std::tuple<uint32_t, Kind, uint8_t, uint8_t, int64_t, int64_t>;

// The item of `song` that has each command of track `track`, and the command at its tick, up to the track's jump or
// end. A wait moves the tick, and makes no command. Each wait has to be the length of a code of MusicPlayer2000.
struct TrackCommands
{
    std::vector<Timed> commands;
    std::vector<uint32_t> items;
};

TrackCommands TrackOf(const GbaMidiSong& song, std::size_t track)
{
    TrackCommands found;
    uint32_t tick = 0;
    for (uint32_t item = song.entries.at(track); item < song.sequence.items.size(); ++item)
    {
        const Command& command = song.sequence.items[item];
        if (command.kind == Kind::kWait)
        {
            const auto length = static_cast<int>(command.value.value / kOne);
            bool coded = false;
            for (int code = 0; code <= 48; code++)
            {
                coded = coded || ntdlib::mp2k::ClockLength(code) == length;
            }
            XSFLIB_CHECK(coded && command.value.value % kOne == 0);
            tick += static_cast<uint32_t>(length);
            continue;
        }
        const int64_t value =
            command.value.source == Source::kVariable
                ? command.value.value
                : ntdlib::commands::FromUnit(command.value.value, ntdlib::mp2k::ScaleOf(command.kind));
        found.commands.emplace_back(tick, command.kind, command.index, command.key, value, command.velocity / kOne);
        found.items.push_back(item);
        if (command.kind == Kind::kJump || command.kind == Kind::kEnd)
        {
            break;
        }
    }

    return found;
}

// The Timed tuple of a setting at a tick.
Timed At(uint32_t tick, Kind kind, int64_t value, uint8_t index = 0)
{
    return {tick, kind, index, 0, value, 0};
}

// The Timed tuple of a note at a tick. A tie lasts 0 ticks.
Timed NoteAt(uint32_t tick, uint8_t key, int64_t length, int64_t velocity)
{
    return {tick, Kind::kNote, 0, key, length, velocity};
}

// A MIDI file at 480 ticks a quarter note. Its first track contains the tempos: 120, 150 and 101 beats a minute, at
// bars 1, 2 and 2.5. The second track plays channel 1 and channel 10, and sets a volume on channel 3. Channel 3 plays
// no note. Channel 1 selects program 5, sets its volume to 100 and its expression to 64, its pan to 32 and its bend
// range to 12, and plays a quarter note. A pitch bend halfway up follows, and a note between the GBA's ticks. At bar 2,
// it sets the LFO, the fine tune and its priority, and plays a note of 10 beats. It also sets control change 91 and
// aftertouch, and sends a system exclusive event. The song leaves them out. Channel 10 plays an eighth note.
std::vector<uint8_t> ThreeChannels()
{
    midi::FileBuilder file(480);
    midi::TrackBuilder& conductor = file.AddTrack();
    conductor.Tempo(0, 500000);
    conductor.TimeSignature(0, 4, 2);
    conductor.Tempo(1920, 400000);
    conductor.Tempo(2880, 594059);

    midi::TrackBuilder& track = file.AddTrack();
    track.Program(0, 0, 5);
    track.Control(0, 0, 7, 100);
    track.Control(0, 0, 11, 64);
    track.Control(0, 0, 10, 32);
    track.BendRange(0, 0, 12);
    track.Control(0, 2, 7, 50);
    track.NoteOn(0, 0, 60, 100);
    track.NoteOff(480, 0, 60);
    track.PitchBend(240, 0, 8192 + 4096);
    track.NoteOn(510, 0, 62, 90);
    track.NoteOff(990, 0, 62);
    track.Control(1920, 0, 1, 40);
    track.Control(1920, 0, 21, 20);
    track.Control(1920, 0, 22, 1);
    track.Control(1920, 0, 26, 10);
    track.Control(1920, 0, 24, 70);
    track.Control(1920, 0, 33, 5);
    track.Control(1920, 0, 91, 40);
    track.NoteOn(1920, 0, 64, 80);
    track.NoteOff(6720, 0, 64);
    track.KeyPressure(2000, 0, 64, 50);
    track.SysEx(2000, {0xF0, 0x7E, 0x7F, 0x09, 0x01, 0xF7});
    track.NoteOn(0, 9, 36, 110);
    track.NoteOff(240, 9, 36);

    return file.Encode();
}

// Each channel with notes makes a track, in the order of the channels. The first track gets the tempos, and 101 beats a
// minute becomes 102. The volume is the product of control changes 7 and 11, and the opening of the drum track sets
// full volume and the voice that the map sets for program 0 of the drums. A note of 10 beats becomes a tie and the
// tie's end. The note between the GBA's ticks moves to the nearest: its note-on and its note-off count. Each track ends
// where its track chunk ends. Each instrument plays the voice that the map pairs with it, or the voice of its program.
void TestTracks()
{
    const GbaMidiSong song = GbaSongOfMidi(ThreeChannels(), GbaVoiceMap{{{true, 0}, 127}});
    XSFLIB_CHECK(song.entries.size() == 2);
    XSFLIB_CHECK(song.instruments == (std::vector<GbaMidiInstrument>{{false, 5}, {true, 0}}));
    XSFLIB_CHECK(song.voices == (GbaVoiceMap{{{false, 5}, 5}, {{true, 0}, 127}}));
    XSFLIB_CHECK(!song.priority && !song.reverb);

    const std::vector<Timed> first = TrackOf(song, 0).commands;
    XSFLIB_CHECK(first == (std::vector<Timed>{
                              At(0, Kind::kTempo, 60),
                              At(0, Kind::kProgram, 5),
                              At(0, Kind::kVolume, 100),
                              At(0, Kind::kVolume, 50),
                              At(0, Kind::kPan, 32),
                              At(0, Kind::kBendRange, 12),
                              NoteAt(0, 60, 24, 100),
                              At(12, Kind::kPitchBend, 96),
                              NoteAt(26, 62, 24, 90),
                              At(96, Kind::kTempo, 75),
                              At(96, Kind::kLfoDepth, 40),
                              At(96, Kind::kLfoSpeed, 20),
                              At(96, Kind::kLfoType, 1),
                              At(96, Kind::kLfoDelay, 10),
                              At(96, Kind::kTune, 70),
                              At(96, Kind::kPriority, 5),
                              NoteAt(96, 64, 0, 80),
                              At(144, Kind::kTempo, 51),
                              {336, Kind::kNoteOff, 0, 64, 0, 0},
                              At(336, Kind::kEnd, 0),
                          }));

    const std::vector<Timed> drums = TrackOf(song, 1).commands;
    XSFLIB_CHECK(drums == (std::vector<Timed>{At(0, Kind::kVolume, 127), At(0, Kind::kProgram, 127),
                                              NoteAt(0, 36, 12, 110), At(336, Kind::kEnd, 0)}));

    XSFLIB_CHECK(
        song.warnings ==
        (std::vector<std::string>{"1 tempo is rounded to the nearest even BPM. GBA tempos must be even.",
                                  "2 events are moved to the nearest tick. The GBA uses 24 ticks per quarter note.",
                                  "1 event left out: channels without notes.", "1 event left out: control change 91.",
                                  "1 event left out: key aftertouch.", "1 event left out: system exclusive."}));

    // Without a map, each instrument plays the voice of its program's number. MusicPlayer2000's bytes hold the song.
    const GbaMidiSong plain = GbaSongOfMidi(ThreeChannels());
    XSFLIB_CHECK(OfKind(TrackOf(plain, 1).commands, Kind::kProgram) == (std::vector<Timed>{At(0, Kind::kProgram, 0)}));
    XSFLIB_CHECK(
        ErrorOf<std::invalid_argument>([&] { ntdlib::mp2k::EncodeSequence(plain.sequence, 0x08100000); }).empty());
}

// Without a loop, a track ends where the track chunks that contain the events of its channel end. Channel 1's chunk
// ends at bar 2, and channel 2's at bar 3. Channel 3's notes are in both chunks. Channel 4's chunk ends with its note.
void TestTrackEnds()
{
    midi::FileBuilder file(96);
    midi::TrackBuilder& first = file.AddTrack();
    first.NoteOn(0, 0, 60, 100);
    first.NoteOff(96, 0, 60);
    first.NoteOn(0, 2, 62, 100);
    first.NoteOff(96, 2, 62);
    first.SetEnd(384);
    midi::TrackBuilder& second = file.AddTrack();
    second.NoteOn(0, 1, 64, 100);
    second.NoteOff(96, 1, 64);
    second.NoteOn(192, 2, 65, 100);
    second.NoteOff(288, 2, 65);
    second.SetEnd(768);
    midi::TrackBuilder& third = file.AddTrack();
    third.NoteOn(0, 3, 67, 100);
    third.NoteOff(1152, 3, 67);

    const GbaMidiSong song = GbaSongOfMidi(file.Encode());
    XSFLIB_CHECK(song.entries.size() == 4);
    const auto end_of = [&song](std::size_t track)
    {
        const std::vector<Timed> commands = TrackOf(song, track).commands;
        return commands.empty() || std::get<1>(commands.back()) != Kind::kEnd ? UINT32_MAX
                                                                              : std::get<0>(commands.back());
    };
    XSFLIB_CHECK(end_of(0) == 96);
    XSFLIB_CHECK(end_of(1) == 192);
    XSFLIB_CHECK(end_of(2) == 192);
    XSFLIB_CHECK(end_of(3) == 288);
}

// A loop from tick 192 to tick 576 of a MIDI file at 96 ticks a quarter note. Channel 1 plays a note of 12 beats from
// the start, a note at the loop's start and a note after the loop's end. Channel 2 plays a quarter note at the start.
// Each track jumps back to the loop's start at the loop's end. The long note's tie ends at the loop's end, before the
// jump, and the note after the loop stays out.
std::vector<uint8_t> Looped(const std::string& start, const std::string& end)
{
    midi::FileBuilder file(96);
    midi::TrackBuilder& conductor = file.AddTrack();
    conductor.Meta(192, 0x06, start);
    if (!end.empty())
    {
        conductor.Meta(576, 0x01, end);
    }

    midi::TrackBuilder& track = file.AddTrack();
    track.NoteOn(0, 0, 60, 100);
    track.NoteOff(1152, 0, 60);
    track.NoteOn(192, 0, 64, 100);
    track.NoteOff(288, 0, 64);
    track.NoteOn(672, 0, 67, 100);
    track.NoteOff(720, 0, 67);
    track.NoteOn(0, 1, 48, 90);
    track.NoteOff(96, 1, 48);
    track.SetEnd(1152);

    return file.Encode();
}

// The loop of a MIDI song for the GBA: a loop that starts after a wait, a loop without an end marker that ends at the
// file's end, and control change 111. A loop that ends before it starts makes none.
void TestLoop()
{
    for (const std::string& start : {"[", "loopStart", "LOOP_START"})
    {
        const GbaMidiSong song = GbaSongOfMidi(Looped(start, "]"));
        const TrackCommands first = TrackOf(song, 0);
        XSFLIB_CHECK(first.commands == (std::vector<Timed>{At(0, Kind::kVolume, 127),
                                                           At(0, Kind::kProgram, 0),
                                                           At(0, Kind::kTempo, 60),
                                                           NoteAt(0, 60, 0, 100),
                                                           NoteAt(48, 64, 24, 100),
                                                           {144, Kind::kNoteOff, 0, 60, 0, 0},
                                                           At(144, Kind::kJump, 0)}));
        XSFLIB_CHECK(song.sequence.items.at(first.items.back()).target == first.items.at(4));

        const TrackCommands second = TrackOf(song, 1);
        XSFLIB_CHECK(second.commands == (std::vector<Timed>{At(0, Kind::kVolume, 127), At(0, Kind::kProgram, 0),
                                                            NoteAt(0, 48, 24, 90), At(144, Kind::kJump, 0)}));

        // The second track's loop starts after a wait of 48 ticks.
        const uint32_t target = song.sequence.items.at(second.items.back()).target;
        uint32_t ticks = 0;
        for (uint32_t item = song.entries.at(1); item < target; item++)
        {
            const Command& command = song.sequence.items[item];
            ticks += command.kind == Kind::kWait ? static_cast<uint32_t>(command.value.value / kOne) : 0;
        }
        XSFLIB_CHECK(ticks == 48);
        XSFLIB_CHECK(song.warnings == (std::vector<std::string>{"1 event left out: after the loop end."}));
    }

    // Without a marker of its end, the loop ends at the file's end: the long note's tie ends there too.
    const GbaMidiSong open = GbaSongOfMidi(Looped("loop_start", ""));
    const std::vector<Timed> first = TrackOf(open, 0).commands;
    XSFLIB_CHECK(first.size() == 8);
    XSFLIB_CHECK(first.at(5) == NoteAt(168, 67, 12, 100));
    XSFLIB_CHECK(first.at(6) == (Timed{288, Kind::kNoteOff, 0, 60, 0, 0}) && first.at(7) == At(288, Kind::kJump, 0));

    // Control change 111 starts a loop without a marker. A loop that ends before it starts makes none, and the track
    // ends at the file's end.
    midi::FileBuilder cc_file(96);
    midi::TrackBuilder& cc_track = cc_file.AddTrack();
    cc_track.Control(96, 0, 111, 0);
    cc_track.NoteOn(0, 0, 60, 100);
    cc_track.NoteOff(96, 0, 60);
    cc_track.SetEnd(384);
    const GbaMidiSong cc = GbaSongOfMidi(cc_file.Encode());
    const TrackCommands cc_commands = TrackOf(cc, 0);
    XSFLIB_CHECK(cc_commands.commands.back() == At(96, Kind::kJump, 0));
    cc_track.Meta(48, 0x06, "]");
    const GbaMidiSong backwards = GbaSongOfMidi(cc_file.Encode());
    XSFLIB_CHECK(TrackOf(backwards, 0).commands.back() == At(96, Kind::kEnd, 0));
    XSFLIB_CHECK(backwards.warnings ==
                 (std::vector<std::string>{
                     "The loop end in the MIDI file doesn't come after the loop start. The song plays once."}));
}

// Memory accesses: control change 13 chooses the operation, 14 the byte and 15 the label, and 12 or 16 makes the access
// with its value. A jump leads to the item after its label, and a jump to a label that is missing from the track stays
// out. Control change 30 chooses the echo's level or length, and 29 or 31 sets it. Text events set the LFO, the fine
// tune and the priority on the tracks of their chunk, and the song's priority, reverb and volumes for the whole song.
void TestCommands()
{
    midi::FileBuilder file(24);
    midi::TrackBuilder& conductor = file.AddTrack();
    conductor.Meta(0, 0x01, "pri=40");
    conductor.Meta(0, 0x01, "rev=50");
    conductor.Meta(0, 0x01, "mvl=64");
    conductor.Meta(0, 0x01, "lfos_global=30");
    conductor.Meta(0, 0x01, "vgr=voicegroup005");

    midi::TrackBuilder& track = file.AddTrack();
    track.Meta(0, 0x01, "tune=-10");
    track.Meta(0, 0x01, "modt=2");
    track.Control(0, 0, 7, 127);
    track.Control(0, 0, 13, 1);
    track.Control(0, 0, 14, 4);
    track.Control(0, 0, 16, 5);
    track.Control(0, 0, 13, 3);
    track.Control(0, 0, 12, 6);
    track.Control(0, 0, 17, 2);
    track.NoteOn(0, 0, 60, 100);
    track.NoteOff(24, 0, 60);
    track.Control(24, 0, 13, 7);
    track.Control(24, 0, 14, 3);
    track.Control(24, 0, 15, 2);
    track.Control(24, 0, 12, 10);
    track.Control(24, 0, 15, 9);
    track.Control(24, 0, 12, 11);
    track.Control(24, 0, 30, 8);
    track.Control(24, 0, 29, 50);
    track.Control(24, 0, 30, 9);
    track.Control(24, 0, 31, 10);
    track.Control(24, 0, 30, 3);
    track.Control(24, 0, 29, 1);

    midi::TrackBuilder& other = file.AddTrack();
    other.Meta(0, 0x01, "prio=9");
    other.NoteOn(0, 1, 48, 64);
    other.NoteOff(24, 1, 48);

    const GbaMidiSong song = GbaSongOfMidi(file.Encode());
    XSFLIB_CHECK(song.priority == 40 && song.reverb == 0x80 + 50);

    const TrackCommands first = TrackOf(song, 0);
    XSFLIB_CHECK(first.commands == (std::vector<Timed>{
                                       At(0, Kind::kProgram, 0),
                                       At(0, Kind::kTempo, 60),
                                       At(0, Kind::kLfoSpeed, 30),
                                       At(0, Kind::kTune, 54),
                                       At(0, Kind::kLfoType, 2),
                                       At(0, Kind::kVolume, 64),
                                       At(0, Kind::kAddVariable, 5, 4),
                                       {0, Kind::kSetVariable, 4, 0, 6, 0},
                                       NoteAt(0, 60, 24, 100),
                                       At(24, Kind::kJumpIfNotEqual, 10, 3),
                                       At(24, Kind::kEchoVolume, 50),
                                       At(24, Kind::kEchoLength, 10),
                                       At(24, Kind::kEnd, 0),
                                   }));
    XSFLIB_CHECK(song.sequence.items.at(first.items.at(6)).value.source == Source::kValue);
    XSFLIB_CHECK(song.sequence.items.at(first.items.at(7)).value.source == Source::kVariable);
    XSFLIB_CHECK(song.sequence.items.at(first.items.at(9)).target == first.items.at(8));

    // The second track gets the global LFO speed and the chunk priority, but not the text events of the first track.
    const std::vector<Timed> second = TrackOf(song, 1).commands;
    XSFLIB_CHECK(second ==
                 (std::vector<Timed>{At(0, Kind::kVolume, 64), At(0, Kind::kProgram, 0), At(0, Kind::kLfoSpeed, 30),
                                     At(0, Kind::kPriority, 9), NoteAt(0, 48, 24, 64), At(24, Kind::kEnd, 0)}));

    XSFLIB_CHECK(song.warnings ==
                 (std::vector<std::string>{"1 event left out: extended commands other than echo.",
                                           "1 event left out: memory access to labels missing from the track.",
                                           "1 event left out: voice group names (vgr=)."}));

    // "nat=1" curves the volumes and the velocities: a volume of 64 gives 127 times 64/127 to the power 10/6, and a
    // velocity of 1 keeps the least.
    midi::FileBuilder natural(24);
    midi::TrackBuilder& curved = natural.AddTrack();
    curved.Meta(0, 0x01, "nat=1");
    curved.Control(0, 0, 7, 64);
    curved.NoteOn(0, 0, 60, 127);
    curved.NoteOff(24, 0, 60);
    curved.NoteOn(24, 0, 62, 1);
    curved.NoteOff(48, 0, 62);
    const std::vector<Timed> notes = TrackOf(GbaSongOfMidi(natural.Encode()), 0).commands;
    XSFLIB_CHECK(OfKind(notes, Kind::kVolume) == (std::vector<Timed>{At(0, Kind::kVolume, 41)}));
    XSFLIB_CHECK(OfKind(notes, Kind::kNote) == (std::vector<Timed>{NoteAt(0, 60, 24, 127), NoteAt(24, 62, 24, 1)}));
}

// A MIDI file whose channel 1 sets a bend range of 2 semitones through registered parameter 0, then selects a
// non-registered parameter at tick 12, without the null parameter, and sets it to 70 through data entry. The track
// keeps the bend range of 2 semitones, and the warnings count the data entry as left out.
void TestNonRegistered()
{
    midi::FileBuilder file(24);
    midi::TrackBuilder& track = file.AddTrack();
    track.Control(0, 0, 101, 0);
    track.Control(0, 0, 100, 0);
    track.Control(0, 0, 6, 2);
    track.Control(12, 0, 99, 1);
    track.Control(12, 0, 98, 8);
    track.Control(12, 0, 6, 70);
    track.NoteOn(0, 0, 60, 100);
    track.NoteOff(24, 0, 60);

    const GbaMidiSong song = GbaSongOfMidi(file.Encode());

    const std::vector<Timed> bends = OfKind(TrackOf(song, 0).commands, Kind::kBendRange);
    XSFLIB_CHECK(bends == (std::vector<Timed>{At(0, Kind::kBendRange, 2)}));
    XSFLIB_CHECK(std::ranges::find(song.warnings, "1 event left out: data entry for other parameters.") !=
                 song.warnings.end());
}

// A file that isn't a MIDI file, a file timed in SMPTE frames and a file without notes give refusals.
void TestRefusals()
{
    XSFLIB_CHECK(!ErrorOf<std::invalid_argument>([] { GbaSongOfMidi(std::vector<uint8_t>{1, 2, 3}); }).empty());

    midi::FileBuilder silent(96);
    silent.AddTrack().Control(0, 0, 7, 100);
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { GbaSongOfMidi(silent.Encode()); }) ==
                 "The MIDI file has no notes.");

    midi::FileBuilder frames(96);
    midi::TrackBuilder& track = frames.AddTrack();
    track.NoteOn(0, 0, 60, 100);
    track.NoteOff(96, 0, 60);
    std::vector<uint8_t> smpte = frames.Encode();
    smpte.at(12) = 0xE7;
    smpte.at(13) = 40;
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { GbaSongOfMidi(smpte); }).find("SMPTE") != std::string::npos);
}

// The proposed voice is the voice of the program, but on the drum channel, where that voice isn't a drum kit: the first
// drum kit of the voice group then.
void TestProposedVoices()
{
    std::vector<ntdlib::mp2k::Voice> voices(128);
    XSFLIB_CHECK(hokuto::edit::ProposedGbaVoice({true, 0}, voices) == 0);
    voices[5].type = ntdlib::mp2k::kVoiceDrumKit;
    voices[9].type = ntdlib::mp2k::kVoiceDrumKit;
    XSFLIB_CHECK(hokuto::edit::ProposedGbaVoice({false, 3}, voices) == 3);
    XSFLIB_CHECK(hokuto::edit::ProposedGbaVoice({true, 0}, voices) == 5);
    XSFLIB_CHECK(hokuto::edit::ProposedGbaVoice({true, 9}, voices) == 9);
    XSFLIB_CHECK(hokuto::edit::ProposedGbaVoice({false, 5}, voices) == 5);

    // The file's instrument map of the voice group comes first. Another voice group's map does nothing.
    File file;
    file.instrument_maps = {{0x08001000, {{{false, 3}, 40}, {{true, 0}, 9}}}, {0x08002000, {{{false, 7}, 1}}}};
    const std::vector<GbaMidiInstrument> instruments = {{false, 3}, {false, 7}, {true, 0}, {true, 1}};
    XSFLIB_CHECK(hokuto::edit::ProposedGbaVoices(file, 0x08001000, instruments, voices) ==
                 (GbaVoiceMap{{{false, 3}, 40}, {{false, 7}, 7}, {{true, 0}, 9}, {{true, 1}, 5}}));
    XSFLIB_CHECK(hokuto::edit::ProposedGbaVoices(file, 0x08003000, instruments, voices) ==
                 (GbaVoiceMap{{{false, 3}, 3}, {{false, 7}, 7}, {{true, 0}, 5}, {{true, 1}, 5}}));
}

// A cartridge of three songs. Song 0 plays two tracks of a voice of a sawtooth, and song 1 one track. Song 2's header
// starts its track at song 1's track. The bytes from 0x38000 on are 0xFF: the ROM's free space. With `fusion`, the
// cartridge has Metroid Fusion's sequencer. The notes of that sequencer read a key and a velocity and no third byte.
std::vector<uint8_t> GbaRom(bool fusion = false)
{
    using enum ntdlib::mp2k::Command;

    mp2k_cart::Cart cart;
    if (fusion)
    {
        cart.WriteMetroid(16, true);
    }
    const uint32_t voices = cart.VoiceGroup({{0, mp2k_cart::Cart::Voice(0, cart.Wave(mp2k_cart::Saw(), 13379, 0))}});
    mp2k_cart::Track first;
    first.Cmd(kCmdTempo, 75).Cmd(kCmdVoice, 0).Cmd(kCmdVol, 100).Note(24, 60, 100).Wait(24).Fine();
    mp2k_cart::Track second;
    second.Cmd(kCmdVoice, 0).Note(24, 64, 100).Wait(24).Fine();
    cart.Song({first, second}, voices);
    mp2k_cart::Track third;
    third.Cmd(kCmdTempo, 60).Cmd(kCmdVoice, 0).Cmd(kCmdVol, 90).Note(12, 67, 80).Wait(12).Fine();
    const uint32_t shared = cart.Song({third}, voices);
    const uint32_t sharing = cart.Song({mp2k_cart::Track().Fine()}, voices);
    cart.Put32(sharing + 8, cart.Get16(shared + 8) | uint32_t{cart.Get16(shared + 10)} << 16);
    for (uint32_t a = 0x38000; a < 0x40000; a++)
    {
        cart.Put8(mp2k_cart::kRomBase + a, 0xFF);
    }

    const ntdlib::agb::Rom rom = cart.ToRom();
    return hokuto::testing::RomBytes(rom);
}

// Where each of the first `songs` songs of `file` plays. The program finds each place the same way.
std::vector<std::optional<hokuto::edit::SoundPlace>> PlacesOf(const File& file, int songs = 3)
{
    const std::shared_ptr<const hokuto::song::GbaRom> rom =
        hokuto::song::LoadGbaRom(hokuto::format::ExportArchive(file));
    std::vector<std::optional<uint32_t>> sequences;
    for (int song = 0; song < songs; song++)
    {
        sequences.push_back(hokuto::format::GbaSongSequence(file, rom->info.song_table, song));
    }

    return hokuto::edit::GbaSoundPlaces(sequences,
                                        hokuto::format::GbaTrackFields(file, rom->info.song_table, sequences.size()));
}

// The notes of `track`: their starts, lengths, keys and velocities.
std::vector<std::vector<uint32_t>> Notes(const hokuto::song::Track& track)
{
    std::vector<std::vector<uint32_t>> notes;
    for (const hokuto::song::Note& note : track.notes)
    {
        notes.push_back({note.start, note.length, note.key, note.velocity});
    }

    return notes;
}

// A MIDI file at 48 ticks a quarter note and 150 beats a minute, with the song's priority and reverb. Channel 1 sets
// its volume to 100 and plays three notes, the last of 10 beats. Channel 10 plays two eighth notes. With `extra`,
// channel 2 plays a note too.
std::vector<uint8_t> Song(bool extra)
{
    midi::FileBuilder file(48);
    midi::TrackBuilder& conductor = file.AddTrack();
    conductor.Tempo(0, 400000);
    conductor.Meta(0, 0x01, "pri=40");
    conductor.Meta(0, 0x01, "rev=50");

    midi::TrackBuilder& track = file.AddTrack();
    track.Control(0, 0, 7, 100);
    track.NoteOn(0, 0, 60, 100);
    track.NoteOff(48, 0, 60);
    track.NoteOn(48, 0, 62, 80);
    track.NoteOff(96, 0, 62);
    track.NoteOn(96, 0, 64, 90);
    track.NoteOff(576, 0, 64);
    track.NoteOn(0, 9, 36, 110);
    track.NoteOff(24, 9, 36);
    track.NoteOn(48, 9, 38, 100);
    track.NoteOff(72, 9, 38);
    if (extra)
    {
        track.NoteOn(0, 1, 50, 100);
        track.NoteOff(48, 1, 50);
    }

    return file.Encode();
}

// The song replaces song 0, and ntdlib's performer plays it from the exported ROM: the tempo of the old song's command
// of 75, the volumes, and the notes at their ticks, the tie included. The header gets the priority and the reverb. Song
// 1 plays as before. A song with fewer tracks ends the header's others at once. A song with more tracks than the header
// is refused. The track of song 2 falls within the place of song 1: the cut gives song 2 no place, and song 2 accepts
// no song.
void TestImport()
{
    const File file = hokuto::format::ImportGbaRom(GbaRom());
    const auto places = PlacesOf(file);
    const std::shared_ptr<const hokuto::song::GbaRom> original =
        hokuto::song::LoadGbaRom(hokuto::format::ExportArchive(file));
    const hokuto::song::Sequence before = hokuto::song::ReadSequence(*original, 1);

    const GbaMidiSong midi = GbaSongOfMidi(Song(false), GbaVoiceMap{{{true, 0}, 0}});
    const File edited = hokuto::edit::WithGbaMidiSong(file, places, 0, midi);
    const std::vector<uint8_t> exported = hokuto::format::ExportArchive(edited);
    XSFLIB_CHECK(hokuto::format::ExportArchive(hokuto::format::ImportGbaRom(exported)) == exported);
    const std::shared_ptr<const hokuto::song::GbaRom> rom = hokuto::song::LoadGbaRom(exported);
    const hokuto::song::Sequence played = hokuto::song::ReadSequence(*rom, 0);
    XSFLIB_CHECK(played.tracks.size() == 2);
    XSFLIB_CHECK(played.tempo == hokuto::song::ReadSequence(*original, 0).tempo);
    XSFLIB_CHECK(Notes(played.tracks.at(0)) ==
                 (std::vector<std::vector<uint32_t>>{{0, 24, 60, 100}, {24, 24, 62, 80}, {48, 240, 64, 90}}));
    XSFLIB_CHECK(Notes(played.tracks.at(1)) ==
                 (std::vector<std::vector<uint32_t>>{{0, 12, 36, 110}, {24, 12, 38, 100}}));
    XSFLIB_CHECK(played.tracks[0].volume == (std::vector<hokuto::song::Setting>{{0, 100}}));
    XSFLIB_CHECK(played.tracks[1].volume == (std::vector<hokuto::song::Setting>{{0, 127}}));
    ntdlib::mp2k::SongHeader header;
    XSFLIB_CHECK(ntdlib::mp2k::ReadSong(rom->rom, rom->info, 0, header));
    XSFLIB_CHECK(header.track_count == 2 && header.priority == 40 && header.reverb == 0x80 + 50);
    const hokuto::song::Sequence after = hokuto::song::ReadSequence(*rom, 1);
    XSFLIB_CHECK(Notes(after.tracks.at(0)) == Notes(before.tracks.at(0)));

    // The file keeps the pairings in the map of the voice group that the song's header names. A later import into a
    // song of the same voice group replaces the pairings of its instruments, and keeps the others.
    ntdlib::mp2k::SongHeader old_header;
    XSFLIB_CHECK(ntdlib::mp2k::ReadSong(original->rom, original->info, 0, old_header));
    XSFLIB_CHECK(hokuto::edit::GbaVoiceGroupOf(file, places, 0) == old_header.voices);
    XSFLIB_CHECK(hokuto::edit::GbaVoiceGroupOf(file, places, 1) == old_header.voices);
    XSFLIB_CHECK(!hokuto::edit::GbaVoiceGroupOf(file, places, 2));
    XSFLIB_CHECK(midi.voices == (GbaVoiceMap{{{false, 0}, 0}, {{true, 0}, 0}}));
    XSFLIB_CHECK(edited.instrument_maps == (std::map<uint32_t, GbaVoiceMap>{{old_header.voices, midi.voices}}));

    // A song of one track: the header's second track ends at once.
    midi::FileBuilder single(48);
    midi::TrackBuilder& alone = single.AddTrack();
    alone.NoteOn(0, 0, 72, 70);
    alone.NoteOff(24, 0, 72);
    const File shorter = hokuto::edit::WithGbaMidiSong(file, places, 0, GbaSongOfMidi(single.Encode()));
    const std::shared_ptr<const hokuto::song::GbaRom> short_rom =
        hokuto::song::LoadGbaRom(hokuto::format::ExportArchive(shorter));
    const hokuto::song::Sequence one = hokuto::song::ReadSequence(*short_rom, 0);
    XSFLIB_CHECK(Notes(one.tracks.at(0)) == (std::vector<std::vector<uint32_t>>{{0, 12, 72, 70}}));
    XSFLIB_CHECK(one.tracks.size() == 1 || one.tracks.at(1).notes.empty());
    const File both =
        hokuto::edit::WithGbaMidiSong(edited, places, 1, GbaSongOfMidi(single.Encode(), {{{false, 0}, 7}}));
    XSFLIB_CHECK(both.instrument_maps ==
                 (std::map<uint32_t, GbaVoiceMap>{{old_header.voices, {{{false, 0}, 7}, {{true, 0}, 0}}}}));
    ntdlib::mp2k::SongHeader short_header;
    XSFLIB_CHECK(ntdlib::mp2k::ReadSong(short_rom->rom, short_rom->info, 0, short_header));
    XSFLIB_CHECK(short_header.priority == 0 && short_header.reverb == 0);

    XSFLIB_CHECK(ErrorOf<std::invalid_argument>(
                     [&] { hokuto::edit::WithGbaMidiSong(file, places, 0, GbaSongOfMidi(Song(true))); })
                     .find("has room for 2 tracks") != std::string::npos);
    XSFLIB_CHECK(!places.at(2));
    XSFLIB_CHECK(
        !ErrorOf<std::invalid_argument>([&] { hokuto::edit::WithGbaMidiSong(file, places, 2, midi); }).empty());
    XSFLIB_CHECK(
        !ErrorOf<std::invalid_argument>([&] { hokuto::edit::WithGbaMidiSong(file, places, 7, midi); }).empty());
}

// On Metroid Fusion's sequencer, a note reads no third byte: a byte below 0x80 after its velocity starts the next
// command (the read of a note's arguments that ntdlib's WriteMetroid writes). The file of a cart with that sequencer
// keeps a count of 2 note arguments, and a plain cart's file 3. For the sequencer, a note of 24 ticks keeps its code. A
// note of 25 ticks and one of 111 ticks become ties, each with an end of the tie at its note-off. The performer plays
// the three notes at their lengths from the exported ROM. The plain song's notes of 25 and 111 ticks use a code and an
// extra length in a third byte: the export of the cart with the sequencer refuses them, and a plain cart plays them.
void TestFusionTies()
{
    midi::FileBuilder file(48);
    midi::TrackBuilder& track = file.AddTrack();
    track.NoteOn(0, 0, 60, 100);
    track.NoteOff(50, 0, 60);
    track.NoteOn(50, 0, 62, 80);
    track.NoteOff(98, 0, 62);
    track.NoteOn(98, 0, 64, 90);
    track.NoteOff(320, 0, 64);
    const std::vector<uint8_t> bytes = file.Encode();
    const File fusion = hokuto::format::ImportGbaRom(GbaRom(true));
    const File cart = hokuto::format::ImportGbaRom(GbaRom());
    const auto fusion_places = PlacesOf(fusion);
    const auto cart_places = PlacesOf(cart);

    const GbaMidiSong song = GbaSongOfMidi(bytes, {}, 2);
    const GbaMidiSong plain = GbaSongOfMidi(bytes);
    const std::shared_ptr<const hokuto::song::GbaRom> tied = hokuto::song::LoadGbaRom(
        hokuto::format::ExportArchive(hokuto::edit::WithGbaMidiSong(fusion, fusion_places, 0, song)));
    const std::string refusal = ErrorOf<std::invalid_argument>(
        [&] { hokuto::format::ExportArchive(hokuto::edit::WithGbaMidiSong(fusion, fusion_places, 0, plain)); });
    const std::shared_ptr<const hokuto::song::GbaRom> extra = hokuto::song::LoadGbaRom(
        hokuto::format::ExportArchive(hokuto::edit::WithGbaMidiSong(cart, cart_places, 0, plain)));

    XSFLIB_CHECK(fusion.native->gba_note_arguments == 2 && cart.native->gba_note_arguments == 3);
    const std::vector<Timed> commands = TrackOf(song, 0).commands;
    XSFLIB_CHECK(OfKind(commands, Kind::kNote) ==
                 (std::vector<Timed>{NoteAt(0, 60, 0, 100), NoteAt(25, 62, 24, 80), NoteAt(49, 64, 0, 90)}));
    XSFLIB_CHECK((OfKind(commands, Kind::kNoteOff) ==
                  std::vector<Timed>{{25, Kind::kNoteOff, 0, 60, 0, 0}, {160, Kind::kNoteOff, 0, 64, 0, 0}}));
    XSFLIB_CHECK(OfKind(TrackOf(plain, 0).commands, Kind::kNote) ==
                 (std::vector<Timed>{NoteAt(0, 60, 25, 100), NoteAt(25, 62, 24, 80), NoteAt(49, 64, 111, 90)}));
    XSFLIB_CHECK(tied->info.metroid_short_notes && !extra->info.metroid_short_notes);
    XSFLIB_CHECK(Notes(hokuto::song::ReadSequence(*tied, 0).tracks.at(0)) ==
                 (std::vector<std::vector<uint32_t>>{{0, 25, 60, 100}, {25, 24, 62, 80}, {49, 111, 64, 90}}));
    XSFLIB_CHECK(refusal.find("A note of 25 ticks") == 0);
    XSFLIB_CHECK(Notes(hokuto::song::ReadSequence(*extra, 0).tracks.at(0)) ==
                 (std::vector<std::vector<uint32_t>>{{0, 25, 60, 100}, {25, 24, 62, 80}, {49, 111, 64, 90}}));
}

// A cartridge of one song of two tracks that play a voice of the PSG's second square wave. The voice sustains at full
// level and ends at its note-off. The bytes from 0x38000 on are 0xFF: the ROM's free space.
std::vector<uint8_t> PsgRom()
{
    using enum ntdlib::mp2k::Command;

    mp2k_cart::Cart cart;
    const uint32_t voices = cart.VoiceGroup({{0, mp2k_cart::Cart::Voice(2, 2, 60, {0, 0, 15, 0})}});
    mp2k_cart::Track first;
    first.Cmd(kCmdTempo, 75).Cmd(kCmdVoice, 0).Cmd(kCmdVol, 100).Note(24, 60, 100).Wait(24).Fine();
    mp2k_cart::Track second;
    second.Cmd(kCmdVoice, 0).Cmd(kCmdVol, 100).Wait(48).Note(12, 64, 90).Wait(12).Fine();
    cart.Song({first, second}, voices);
    for (uint32_t a = 0x38000; a < 0x40000; a++)
    {
        cart.Put8(mp2k_cart::kRomBase + a, 0xFF);
    }

    const ntdlib::agb::Rom rom = cart.ToRom();
    return hokuto::testing::RomBytes(rom);
}

// A MIDI file at 48 ticks a quarter note whose channels 1 and 2 each play one note of program 0, a quarter note apart.
// With `overlap`, channel 2's note starts an eighth note into channel 1's half note.
std::vector<uint8_t> TwoPsgNotes(bool overlap)
{
    midi::FileBuilder file(48);
    midi::TrackBuilder& track = file.AddTrack();
    track.NoteOn(0, 0, 60, 100);
    track.NoteOff(overlap ? 96 : 48, 0, 60);
    track.NoteOn(overlap ? 24 : 96, 1, 64, 90);
    track.NoteOff(overlap ? 48 : 120, 1, 64);

    return file.Encode();
}

// The Import window counts the notes of the new song that lose their channel. Both tracks play the voice on the PSG's
// second square channel at the same priority. On MusicPlayer2000, the earlier track keeps a PSG channel that a later
// track's note asks for at the same priority, and that note never starts. Channel 2's note during channel 1's half note
// is such a note. Apart, both notes play. A song that is missing from the ROM has no count.
void TestImportCollisions()
{
    const File file = hokuto::format::ImportGbaRom(PsgRom());
    const auto places = PlacesOf(file, 1);
    const std::shared_ptr<const hokuto::song::GbaRom> rom =
        hokuto::song::LoadGbaRom(hokuto::format::ExportArchive(file));

    const File overlapping = hokuto::edit::WithGbaMidiSong(file, places, 0, GbaSongOfMidi(TwoPsgNotes(true)));
    const File apart = hokuto::edit::WithGbaMidiSong(file, places, 0, GbaSongOfMidi(TwoPsgNotes(false)));
    XSFLIB_CHECK(hokuto::edit::GbaSongCollisions(overlapping, *rom, 0) == (hokuto::song::Collisions{0, 1}));
    XSFLIB_CHECK(hokuto::edit::GbaSongCollisions(apart, *rom, 0) == (hokuto::song::Collisions{0, 0}));
    XSFLIB_CHECK(!hokuto::edit::GbaSongCollisions(apart, *rom, 7));
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("gba_midi",
                                    []
                                    {
                                        TestTracks();
                                        TestTrackEnds();
                                        TestLoop();
                                        TestCommands();
                                        TestNonRegistered();
                                        TestRefusals();
                                        TestProposedVoices();
                                        TestImport();
                                        TestFusionTies();
                                        TestImportCollisions();
                                    });
}
