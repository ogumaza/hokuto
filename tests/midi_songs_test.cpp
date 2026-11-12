// SPDX-License-Identifier: MIT

// Tests the songs of MIDI files (src/edit/midi_songs.h) and the banks of SoundFonts (src/edit/sound_font_banks.h). A
// MIDI file of two tracks on two channels sets the settings that the song supports, and leaves out a controller,
// aftertouch and a system exclusive event. Its loop markers give the loop. The song has to hold each event as a command
// at its tick in the song's timebase, and the converter's layout with subroutines has to give the song back. A MIDI
// file whose tempo isn't a whole number of beats a minute keeps its time with `keeps_time`: at each bar line, the
// song's time has to fall within half a bar's difference between the two whole tempos. A MIDI file that plays its loop
// twice ends at the loop's end with `ends_at_loop`. Its notes over the loop's end keep their lengths, and a note that
// the file's end cuts before the loop holds. A converter's MIDI file of 16 tracks and a layer, with control change 111
// on the conductor channel, joins the layer into its track with `joins_layers`. The joined track has to keep the length
// of each note and leave out the setting of the layer that repeats the value of its track. Control change 111 starts a
// loop without a marker and uses no track. With `selects_banks`, control change 0 selects a bank of 128 programs. A
// SoundFont of two presets in bank 0 and one in bank 1 becomes a bank of two instruments. Each region has to contain
// the wave of the sample of its zone, its root key, its tuning, 0.4 of its attenuation as a gain, its pan and its
// exclusive class. Its envelope has to be the one nearest the zone envelope, from ntdlib's ConvertEnvelope. The bank
// leaves out a filter and the volume envelope's delay with warnings.

#include "edit/midi_songs.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/commands/units.h"
#include "ntdlib/sar/bank_file.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sar/wave_file.h"
#include "ntdlib/sequence_export/consoles.h"
#include "ntdlib/sequence_export/envelope.h"
#include "seqlib/midi/builder.h"
#include "seqlib/sf2/soundfont.h"
#include "seqlib/sf2/writer.h"
#include "xsflib/bytes.h"
#include "xsflib/testing/check.h"

#include "edit/converter.h"
#include "edit/sound_font_banks.h"
#include "midi_test_support.h"

namespace
{

namespace midi = seqlib::midi;
namespace sf2 = seqlib::sf2;
namespace sar = ntdlib::sar;
using hokuto::edit::MidiSong;
using hokuto::edit::SongOfMidi;
using hokuto::testing::OfKind;
using ntdlib::commands::Command;
using ntdlib::commands::Kind;
using ntdlib::commands::kOne;
using ntdlib::commands::Sequence;
using xsflib::testing::ErrorOf;

// A command of a track at its tick: its kind, its index, its key, its value and its velocity.
using Timed = std::tuple<uint32_t, Kind, uint8_t, uint8_t, int64_t, int64_t>;

// The item of each label of `sequence`, by the label's name.
std::map<std::string, uint32_t> LabelsOf(const Sequence& sequence)
{
    std::map<std::string, uint32_t> labels;
    for (const auto& label : sequence.labels)
    {
        labels[label.name] = label.item;
    }

    return labels;
}

// The commands of the track that starts at label `label` of `sequence`, a song laid out without subroutines, at their
// ticks, up to the track's jump or end. A wait moves the tick and yields no command. The jump comes last, at its tick.
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

// A MIDI file at 480 ticks a quarter note and 120 beats a minute. Its first track contains the tempo, a time signature
// of 4/4 and the loop's markers: from bar 2 to bar 3. The second track plays channel 1 and channel 10. Channel 1 sets
// its program, its volume, pan, second volume and sends, a bend range of 12 semitones and a pitch bend, and plays two
// notes. Its second pitch bend falls between two of NintendoWare's steps. It also sets control change 1 and aftertouch,
// and sends a system exclusive event. The song leaves them out.
// Channel 10 plays a drum twice.
std::vector<uint8_t> TwoTracks()
{
    midi::FileBuilder file(480);
    midi::TrackBuilder& conductor = file.AddTrack();
    conductor.Tempo(0, 500000);
    conductor.TimeSignature(0, 4, 2);
    conductor.Meta(1920, 0x06, "loopStart");
    conductor.Meta(3840, 0x06, "loopEnd");

    midi::TrackBuilder& track = file.AddTrack();
    track.Program(0, 0, 5);
    track.Control(0, 0, 7, 100);
    track.Control(0, 0, 10, 32);
    track.Control(0, 0, 11, 90);
    track.Control(0, 0, 91, 40);
    track.Control(0, 0, 93, 20);
    track.Control(0, 0, 1, 64);
    track.BendRange(0, 0, 12);
    track.NoteOn(0, 0, 60, 100);
    track.NoteOff(480, 0, 60);
    track.NoteOn(0, 9, 36, 110);
    track.NoteOff(240, 9, 36);
    track.NoteOn(1920, 0, 64, 80);
    track.NoteOff(2880, 0, 64);
    track.PitchBend(2400, 0, 8192 + 4096);
    track.PitchBend(2640, 0, 8192 + 100);
    track.NoteOn(1920, 9, 36, 110);
    track.NoteOff(2160, 9, 36);
    track.KeyPressure(2200, 0, 64, 50);
    track.SysEx(2000, {0xF0, 0x7E, 0x7F, 0x09, 0x01, 0xF7});
    track.SetEnd(3840);

    return file.Encode();
}

// A MIDI file of two tracks becomes a timeline of NintendoWare's MIDI converter: its tracks and loops, the settings at
// tick 0, the notes, the bars and the events that it leaves out. The converter's layout reproduces the song. The song
// can take another timebase, and starts at the first note or at the file's tick 0.
void TestSong()
{
    const MidiSong song = SongOfMidi(TwoTracks(), "test song");

    // The tracks: channel 1, then channel 10. Each loops from bar 2 to bar 3: from tick 192 to tick 384.
    const std::vector<Timed> first = TrackAt(song.timeline, "SMF_test_song_Track_0");
    const std::vector<Timed> drums = TrackAt(song.timeline, "SMF_test_song_Track_1");
    XSFLIB_CHECK(!first.empty() && !drums.empty());
    XSFLIB_CHECK(std::get<1>(first.back()) == Kind::kJump && std::get<0>(first.back()) == 384);
    XSFLIB_CHECK(std::get<1>(drums.back()) == Kind::kJump && std::get<0>(drums.back()) == 384);
    const std::map<std::string, uint32_t> labels = LabelsOf(song.timeline);
    XSFLIB_CHECK(labels.contains("SMF_test_song_Begin") && labels.contains("SMF_test_song_Start") &&
                 labels.contains("SMF_test_song_End") && labels.contains("SMF_test_song_Track_0_LoopStart") &&
                 labels.contains("SMF_test_song_Track_1_LoopStart"));
    XSFLIB_CHECK(song.timeline.items[labels.at("SMF_test_song_Begin")].kind == Kind::kAllocateTracks);
    XSFLIB_CHECK(song.timeline.items[labels.at("SMF_test_song_Begin")].value.value == 3 * kOne);

    // The settings at tick 0, with the tempo in the first track.
    const auto setting = [&first](Kind kind, uint8_t index = 0) -> std::optional<int64_t>
    {
        for (const Timed& command : first)
        {
            if (std::get<1>(command) == kind && std::get<2>(command) == index && std::get<0>(command) == 0)
            {
                return std::get<4>(command);
            }
        }
        return std::nullopt;
    };
    XSFLIB_CHECK(setting(Kind::kTempo) == 120 * kOne);
    XSFLIB_CHECK(setting(Kind::kProgram) == 5 * kOne);
    XSFLIB_CHECK(setting(Kind::kVolume) == 100 * kOne);
    XSFLIB_CHECK(setting(Kind::kPan) == -32 * kOne);
    XSFLIB_CHECK(setting(Kind::kVolume2) == 90 * kOne);
    XSFLIB_CHECK(setting(Kind::kSend, 0) == 40 * kOne);
    XSFLIB_CHECK(setting(Kind::kSend, 1) == 20 * kOne);
    XSFLIB_CHECK(setting(Kind::kBendRange) == 1200 * kOne);
    XSFLIB_CHECK(OfKind(drums, Kind::kTempo).empty());

    // The notes: their ticks, keys, lengths and velocities.
    XSFLIB_CHECK(OfKind(first, Kind::kNote) == (std::vector<Timed>{{0, Kind::kNote, 0, 60, 48 * kOne, 100 * kOne},
                                                                   {192, Kind::kNote, 0, 64, 96 * kOne, 80 * kOne}}));
    XSFLIB_CHECK(OfKind(drums, Kind::kNote) == (std::vector<Timed>{{0, Kind::kNote, 0, 36, 24 * kOne, 110 * kOne},
                                                                   {192, Kind::kNote, 0, 36, 24 * kOne, 110 * kOne}}));
    XSFLIB_CHECK(OfKind(first, Kind::kPitchBend) == (std::vector<Timed>{{240, Kind::kPitchBend, 0, 0, kOne / 2, 0},
                                                                        {264, Kind::kPitchBend, 0, 0, kOne / 64, 0}}));

    // The bars of 4/4, and the events that the song leaves out.
    XSFLIB_CHECK(!song.hints.bars.empty() && song.hints.bars.front().length == 192);
    XSFLIB_CHECK(song.warnings ==
                 (std::vector<std::string>{"1 event left out: control change 1.", "1 event left out: key aftertouch.",
                                           "1 event left out: system exclusive."}));

    // The converter's layout with subroutines gives the song back.
    const Sequence laid_out = hokuto::edit::LayOutConverted(song.timeline, song.hints, true);
    XSFLIB_CHECK(hokuto::edit::LayOutConverted(laid_out, song.hints, false) == song.timeline);

    // The song can take another timebase.
    const MidiSong fine = SongOfMidi(TwoTracks(), "fine", {.timebase = 96});
    XSFLIB_CHECK(OfKind(TrackAt(fine.timeline, "SMF_fine_Track_0"), Kind::kNote).size() == 2);
    XSFLIB_CHECK(std::get<0>(OfKind(TrackAt(fine.timeline, "SMF_fine_Track_0"), Kind::kNote).back()) == 384);

    // The song starts at the first note, or with `from_tick_0` at the MIDI file's tick 0. A note a beat in then starts
    // at tick 48.
    midi::FileBuilder late(480);
    midi::TrackBuilder& late_track = late.AddTrack();
    late_track.NoteOn(480, 0, 60, 100);
    late_track.NoteOff(960, 0, 60);
    late_track.SetEnd(960);
    const auto note_ticks = [&late](const std::string& name, const hokuto::edit::MidiOptions& options)
    {
        std::vector<uint32_t> ticks;
        const MidiSong converted = SongOfMidi(late.Encode(), name, options);
        for (const Timed& note : OfKind(TrackAt(converted.timeline, "SMF_" + name + "_Track_0"), Kind::kNote))
        {
            ticks.push_back(std::get<0>(note));
        }
        return ticks;
    };
    XSFLIB_CHECK(note_ticks("first", {}) == std::vector<uint32_t>{0});
    XSFLIB_CHECK(note_ticks("start", {.from_tick_0 = true}) == std::vector<uint32_t>{48});
}

// A MIDI file as a converter writes it, at 480 ticks a quarter note. The conductor sets the tempo, marks the loop from
// bar 2 to bar 3, and sets control change 111 on channel 1 at the loop's start and control change 1 on channel 3.
// Tracks "Track 0" to "Track 15" play a note each on channels 1 to 16. "Track 3" sets its volume to 100 and its pan to
// 32, and plays key 60 for two beats. "Track 3 (layer 2)" comes last, on channel 1. It sets the volume to 100 again,
// plays key 60 from the second eighth note for an eighth note, and sets its pan to 40 at the second beat.
std::vector<uint8_t> Layers()
{
    midi::FileBuilder file(480);
    midi::TrackBuilder& conductor = file.AddTrack();
    conductor.Name("layers");
    conductor.Tempo(0, 500000);
    conductor.Meta(1920, 0x06, "loopStart");
    conductor.Control(1920, 0, 111, 0);
    conductor.Control(0, 2, 1, 64);
    conductor.Meta(3840, 0x06, "loopEnd");
    for (int t = 0; t < 16; ++t)
    {
        midi::TrackBuilder& track = file.AddTrack();
        track.Name("Track " + std::to_string(t));
        if (t == 3)
        {
            track.Control(0, t, 7, 100);
            track.Control(0, t, 10, 32);
        }
        track.NoteOn(0, t, 60, 100);
        track.NoteOff(t == 3 ? 960 : 480, t, 60);
        track.SetEnd(3840);
    }
    midi::TrackBuilder& layer = file.AddTrack();
    layer.Name("Track 3 (layer 2)");
    layer.Control(0, 0, 7, 100);
    layer.NoteOn(240, 0, 60, 90);
    layer.NoteOff(480, 0, 60);
    layer.Control(480, 0, 10, 40);
    layer.SetEnd(3840);

    return file.Encode();
}

void TestLayers()
{
    // The conductor's control changes take no track, and the layer joins track 3. Its note keeps its length, and its
    // volume of 100 stays out.
    const MidiSong song = SongOfMidi(Layers(), "layers", {.joins_layers = true});
    const std::map<std::string, uint32_t> labels = LabelsOf(song.timeline);
    XSFLIB_CHECK(song.timeline.items[labels.at("SMF_layers_Begin")].value.value == 0xFFFF * kOne);
    XSFLIB_CHECK(labels.contains("SMF_layers_Track_15") && !labels.contains("SMF_layers_Track_16"));
    const std::vector<Timed> joined = TrackAt(song.timeline, "SMF_layers_Track_3");
    XSFLIB_CHECK(OfKind(joined, Kind::kNote) == (std::vector<Timed>{{0, Kind::kNote, 0, 60, 96 * kOne, 100 * kOne},
                                                                    {24, Kind::kNote, 0, 60, 24 * kOne, 90 * kOne}}));
    XSFLIB_CHECK(OfKind(joined, Kind::kVolume) == (std::vector<Timed>{{0, Kind::kVolume, 0, 0, 100 * kOne, 0}}));
    XSFLIB_CHECK(OfKind(joined, Kind::kPan) ==
                 (std::vector<Timed>{{0, Kind::kPan, 0, 0, -32 * kOne, 0}, {48, Kind::kPan, 0, 0, -24 * kOne, 0}}));
    XSFLIB_CHECK(std::get<1>(joined.back()) == Kind::kJump && std::get<0>(joined.back()) == 384);
    XSFLIB_CHECK(song.warnings ==
                 (std::vector<std::string>{"The MIDI file uses 17 channels, but NintendoWare sequences have at most 16 "
                                           "tracks. 1 layer was merged back into the track it came from.",
                                           "1 event left out: control change 1."}));
    const Sequence laid_out = hokuto::edit::LayOutConverted(song.timeline, song.hints, true);
    XSFLIB_CHECK(hokuto::edit::LayOutConverted(laid_out, song.hints, false) == song.timeline);

    // Without `joins_layers`, the 17 channels are too many.
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([] { SongOfMidi(Layers(), "x"); }) ==
                 "The MIDI file has events for more than 16 tracks. NintendoWare sequences have at most 16 tracks.");

    // Control change 111 starts the loop where no marker does, and the loop ends at the file's end.
    midi::FileBuilder marked(480);
    midi::TrackBuilder& track = marked.AddTrack();
    track.NoteOn(0, 0, 60, 100);
    track.NoteOff(480, 0, 60);
    track.Control(1920, 0, 111, 0);
    track.NoteOn(1920, 0, 62, 100);
    track.NoteOff(2400, 0, 62);
    track.SetEnd(3840);
    const MidiSong looped = SongOfMidi(marked.Encode(), "looped");
    const std::vector<Timed> commands = TrackAt(looped.timeline, "SMF_looped_Track_0");
    XSFLIB_CHECK(std::get<1>(commands.back()) == Kind::kJump && std::get<0>(commands.back()) == 384);
    const std::map<std::string, uint32_t> looped_labels = LabelsOf(looped.timeline);
    XSFLIB_CHECK(looped_labels.contains("SMF_looped_Track_0_LoopStart"));
    XSFLIB_CHECK(std::get<0>(TrackAt(looped.timeline, "SMF_looped_Track_0_LoopStart").front()) == 0);
    XSFLIB_CHECK(std::get<3>(TrackAt(looped.timeline, "SMF_looped_Track_0_LoopStart").front()) == 62);
    XSFLIB_CHECK(looped.warnings.empty());

    // With `selects_banks`, control change 0 selects a bank of 128 programs. Without it, the bank selects stay out.
    midi::FileBuilder banked(480);
    midi::TrackBuilder& banks = banked.AddTrack();
    banks.Bank(0, 0, 1);
    banks.Program(0, 0, 2);
    banks.NoteOn(0, 0, 60, 100);
    banks.NoteOff(480, 0, 60);
    banks.Bank(480, 0, 0);
    banks.Program(480, 0, 5);
    banks.SetEnd(960);
    const MidiSong selected = SongOfMidi(banked.Encode(), "selected", {.selects_banks = true});
    XSFLIB_CHECK(
        OfKind(TrackAt(selected.timeline, "SMF_selected_Track_0"), Kind::kProgram) ==
        (std::vector<Timed>{{0, Kind::kProgram, 0, 0, 130 * kOne, 0}, {48, Kind::kProgram, 0, 0, 5 * kOne, 0}}));
    XSFLIB_CHECK(selected.warnings.empty());
    const MidiSong plain = SongOfMidi(banked.Encode(), "plain");
    XSFLIB_CHECK(OfKind(TrackAt(plain.timeline, "SMF_plain_Track_0"), Kind::kProgram) ==
                 (std::vector<Timed>{{0, Kind::kProgram, 0, 0, 2 * kOne, 0}, {48, Kind::kProgram, 0, 0, 5 * kOne, 0}}));
    XSFLIB_CHECK(plain.warnings == (std::vector<std::string>{"2 events left out: control change 0.",
                                                             "2 events left out: control change 32."}));
}

// A MIDI file of `bars` bars of 4/4 at 480 ticks a quarter note, with a tempo of `micros` microseconds a quarter note
// and a note on each beat. With `loop`, the loop runs from bar 3 to the end.
std::vector<uint8_t> Steady(uint32_t micros, uint32_t bars, bool loop = false)
{
    midi::FileBuilder file(480);
    midi::TrackBuilder& track = file.AddTrack();
    track.Tempo(0, micros);
    if (loop)
    {
        track.Meta(2 * 1920, 0x06, "loopStart");
        track.Meta(bars * 1920, 0x06, "loopEnd");
    }
    for (uint32_t beat = 0; beat < bars * 4; ++beat)
    {
        track.NoteOn(beat * 480, 0, 60, 100);
        track.NoteOff(beat * 480 + 240, 0, 60);
    }
    track.SetEnd(bars * 1920);

    return file.Encode();
}

void TestKeepsTime()
{
    // 103.53 beats a minute: the tempo of 104 that MP2K plays at the GBA's 59.73 Hz.
    constexpr uint32_t kMicros = 579538;
    constexpr uint32_t kBars = 16;
    const double midi_bar = 4.0 * kMicros / 1e6;

    // Without `keeps_time`, the song uses the nearest whole tempo.
    const MidiSong plain = SongOfMidi(Steady(kMicros, kBars), "plain");
    const std::vector<Timed> plain_tempos = OfKind(TrackAt(plain.timeline, "SMF_plain_Track_0"), Kind::kTempo);
    XSFLIB_CHECK(plain_tempos.size() == 1 && std::get<4>(plain_tempos[0]) == 104 * kOne);
    XSFLIB_CHECK(plain.warnings == std::vector<std::string>{"NintendoWare tempos must be whole BPM values. The MIDI "
                                                            "tempos are rounded to the nearest whole BPM."});

    // Whole tempos keep the time at every bar line.
    const MidiSong kept = SongOfMidi(Steady(kMicros, kBars), "kept", {.keeps_time = true});
    const std::vector<Timed> tempos = OfKind(TrackAt(kept.timeline, "SMF_kept_Track_0"), Kind::kTempo);
    XSFLIB_CHECK(tempos.size() > 2);
    std::map<uint32_t, int64_t> at;
    for (const Timed& tempo : tempos)
    {
        XSFLIB_CHECK(std::get<4>(tempo) % kOne == 0);
        const int64_t bpm = std::get<4>(tempo) / kOne;
        XSFLIB_CHECK(bpm == 103 || bpm == 104);
        XSFLIB_CHECK(std::get<0>(tempo) % 192 == 0);
        at[std::get<0>(tempo)] = bpm;
    }
    double seconds = 0.0;
    int64_t bpm = 0;
    const double step = 4.0 * 60.0 / 103.0 - 4.0 * 60.0 / 104.0;
    for (uint32_t bar = 0; bar < kBars; ++bar)
    {
        if (const auto change = at.find(bar * 192); change != at.end())
        {
            bpm = change->second;
        }
        seconds += 4.0 * 60.0 / static_cast<double>(bpm);
        XSFLIB_CHECK(std::abs(seconds - (bar + 1) * midi_bar) <= step / 2 + 1e-9);
    }
    XSFLIB_CHECK(kept.warnings ==
                 std::vector<std::string>{"NintendoWare tempos must be whole BPM values. From bar to bar, the song "
                                          "alternates between the whole tempos below and above each MIDI tempo, and "
                                          "keeps the timing."});

    // A song for the DS names the DS's tempos. NITRO-Composer's tempo command stores whole beats a minute too.
    const MidiSong ds_plain = SongOfMidi(Steady(kMicros, kBars), "plain", {.engine = "DS"});
    const MidiSong ds_kept = SongOfMidi(Steady(kMicros, kBars), "kept", {.keeps_time = true, .engine = "DS"});

    XSFLIB_CHECK(ds_plain.warnings ==
                 std::vector<std::string>{"DS tempos must be whole BPM values. The MIDI tempos are "
                                          "rounded to the nearest whole BPM."});
    XSFLIB_CHECK(ds_kept.warnings ==
                 std::vector<std::string>{"DS tempos must be whole BPM values. From bar to bar, the song alternates "
                                          "between the whole tempos below and above each MIDI tempo, and keeps the "
                                          "timing."});
    XSFLIB_CHECK(ds_kept.timeline == kept.timeline);

    // A loop sets the tempo of its first bar again at its start when its last bar uses another.
    const MidiSong looped = SongOfMidi(Steady(kMicros, 7, true), "looped", {.keeps_time = true});
    const std::vector<Timed> loop_track = TrackAt(looped.timeline, "SMF_looped_Track_0");
    const std::vector<Timed> loop_tempos = OfKind(loop_track, Kind::kTempo);
    int64_t first_in_loop = 0;
    int64_t last_in_loop = 0;
    bool at_start = false;
    for (const Timed& tempo : loop_tempos)
    {
        if (std::get<0>(tempo) == 384)
        {
            at_start = true;
        }
        if (std::get<0>(tempo) <= 384)
        {
            first_in_loop = std::get<4>(tempo);
        }
        last_in_loop = std::get<4>(tempo);
    }
    XSFLIB_CHECK(at_start || first_in_loop == last_in_loop);

    // A whole tempo requires no changes.
    const MidiSong whole = SongOfMidi(Steady(500000, kBars), "whole", {.keeps_time = true});
    XSFLIB_CHECK(OfKind(TrackAt(whole.timeline, "SMF_whole_Track_0"), Kind::kTempo).size() == 1);
    XSFLIB_CHECK(whole.warnings.empty());
}

// A MIDI file at 480 ticks a quarter note and 120 beats a minute that plays its loop twice. A GBA song's MIDI export
// also plays its loop twice. The loop is bar 2, and the second pass is bar 3, where the file ends. Channel 1 holds a
// note from tick 0 to the file's end, and plays three notes in each pass. The first lasts a beat. The second sounds on
// to the file's end. The third sounds over the loop's end, up to the next bar's second beat. Channel 1 sets control
// change 1 at the start and in the second pass, with a new tempo there too. Channel 2 plays only in the second pass.
std::vector<uint8_t> TwoPasses()
{
    midi::FileBuilder file(480);
    midi::TrackBuilder& conductor = file.AddTrack();
    conductor.Tempo(0, 500000);
    conductor.TimeSignature(0, 4, 2);
    conductor.Meta(1920, 0x06, "loopStart");
    conductor.Meta(3840, 0x06, "loopEnd");
    conductor.Tempo(4000, 400000);

    midi::TrackBuilder& track = file.AddTrack();
    track.Control(0, 0, 1, 64);
    track.NoteOn(0, 0, 48, 100);
    track.NoteOff(5760, 0, 48);
    for (const uint32_t pass : {1920u, 3840u})
    {
        track.NoteOn(pass, 0, 62, 100);
        track.NoteOff(pass + 480, 0, 62);
        track.NoteOn(pass + 960, 0, 64, 100);
        track.NoteOff(5760, 0, 64);
        track.NoteOn(pass + 1440, 0, 60, 100);
        track.NoteOff(std::min(pass + 2400, 5760u), 0, 60);
    }
    track.Control(4000, 0, 1, 32);
    track.NoteOn(4000, 1, 72, 100);
    track.NoteOff(4480, 1, 72);
    track.SetEnd(5760);

    return file.Encode();
}

void TestEndsAtLoop()
{
    // The song ends at the loop's end, and leaves out the second pass. The held note lasts NintendoWare's length of a
    // held note: 200 beats. The note over the loop's end ends where the second pass ends it, and the note that sounds
    // on to the file's end in the loop ends there too.
    const MidiSong song = SongOfMidi(TwoPasses(), "passes", {.ends_at_loop = true});
    const std::vector<Timed> track = TrackAt(song.timeline, "SMF_passes_Track_0");
    XSFLIB_CHECK(!track.empty() && std::get<1>(track.back()) == Kind::kJump && std::get<0>(track.back()) == 384);
    XSFLIB_CHECK(OfKind(track, Kind::kNote) == (std::vector<Timed>{{0, Kind::kNote, 0, 48, 9600 * kOne, 100 * kOne},
                                                                   {192, Kind::kNote, 0, 62, 48 * kOne, 100 * kOne},
                                                                   {288, Kind::kNote, 0, 64, 288 * kOne, 100 * kOne},
                                                                   {336, Kind::kNote, 0, 60, 96 * kOne, 100 * kOne}}));
    XSFLIB_CHECK(OfKind(track, Kind::kTempo).size() == 1);
    const std::map<std::string, uint32_t> labels = LabelsOf(song.timeline);
    XSFLIB_CHECK(labels.contains("SMF_passes_Begin") && !labels.contains("SMF_passes_Track_1"));
    XSFLIB_CHECK(song.timeline.items[labels.at("SMF_passes_Begin")].value.value == kOne);
    XSFLIB_CHECK(song.warnings == std::vector<std::string>{"1 event left out: control change 1."});
    const Sequence laid_out = hokuto::edit::LayOutConverted(song.timeline, song.hints, true);
    XSFLIB_CHECK(hokuto::edit::LayOutConverted(laid_out, song.hints, false) == song.timeline);

    // Without `ends_at_loop`, the second pass stays after the loop's end, and channel 2 has a track.
    const MidiSong both = SongOfMidi(TwoPasses(), "both");
    const std::map<std::string, uint32_t> both_labels = LabelsOf(both.timeline);
    XSFLIB_CHECK(both_labels.contains("SMF_both_Track_1"));
    XSFLIB_CHECK(std::get<4>(OfKind(TrackAt(both.timeline, "SMF_both_Track_0"), Kind::kNote).front()) == 576 * kOne);
}

// A MIDI file whose channel 1 sets a bend range of 2 semitones through registered parameter 0, then selects a
// non-registered parameter at the second beat, without the null parameter, and sets it to 70 through data entry. The
// song keeps the bend range of 2 semitones, and the warnings count the data entry as left out.
void TestNonRegistered()
{
    midi::FileBuilder file(480);
    midi::TrackBuilder& track = file.AddTrack();
    track.Control(0, 0, 101, 0);
    track.Control(0, 0, 100, 0);
    track.Control(0, 0, 6, 2);
    track.Control(480, 0, 99, 1);
    track.Control(480, 0, 98, 8);
    track.Control(480, 0, 6, 70);
    track.NoteOn(0, 0, 60, 100);
    track.NoteOff(960, 0, 60);
    track.SetEnd(960);

    const MidiSong song = SongOfMidi(file.Encode(), "nrpn");

    const std::vector<Timed> bends = OfKind(TrackAt(song.timeline, "SMF_nrpn_Track_0"), Kind::kBendRange);
    XSFLIB_CHECK(bends == (std::vector<Timed>{{0, Kind::kBendRange, 0, 0, 200 * kOne, 0}}));
    XSFLIB_CHECK(std::ranges::find(song.warnings, "1 event left out: data entry for other parameters.") !=
                 song.warnings.end());
}

// A MIDI file that can't be read is refused with seqlib's reason as a sentence. A file without notes, a loop end that
// doesn't come after the loop start, events for more than 16 tracks and SMPTE time are refused, each with its message.
void TestRefusals()
{
    XSFLIB_CHECK(!ErrorOf<std::invalid_argument>([] { SongOfMidi(std::vector<uint8_t>{1, 2, 3}, "x"); }).empty());

    // seqlib's reason follows as a sentence: with a capital letter and a full stop.
    const std::string unreadable =
        ErrorOf<std::invalid_argument>([] { SongOfMidi(std::vector<uint8_t>{1, 2, 3}, "x"); });
    constexpr std::string_view kUnreadable = "Can't read the MIDI file. ";
    XSFLIB_CHECK(unreadable.starts_with(kUnreadable) && unreadable.size() > kUnreadable.size() + 1 &&
                 std::isupper(static_cast<unsigned char>(unreadable[kUnreadable.size()])) && unreadable.ends_with("."));

    midi::FileBuilder silent(480);
    silent.AddTrack().Tempo(0, 500000);
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { SongOfMidi(silent.Encode(), "x"); }) ==
                 "The MIDI file has no notes.");

    midi::FileBuilder backwards(480);
    midi::TrackBuilder& track = backwards.AddTrack();
    track.Meta(1920, 0x06, "loopStart");
    track.Meta(960, 0x06, "loopEnd");
    track.NoteOn(0, 0, 60, 100);
    track.NoteOff(480, 0, 60);
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { SongOfMidi(backwards.Encode(), "x"); }) ==
                 "The loop end in the MIDI file doesn't come after the loop start.");

    midi::FileBuilder many(480);
    midi::TrackBuilder& wide = many.AddTrack();
    for (int t = 0; t < 17; ++t)
    {
        midi::TrackBuilder& next = t < 16 ? wide : many.AddTrack();
        next.NoteOn(0, t % 16, 60, 100);
        next.NoteOff(480, t % 16, 60);
    }
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { SongOfMidi(many.Encode(), "x"); }) ==
                 "The MIDI file has events for more than 16 tracks. NintendoWare sequences have at most 16 tracks.");

    std::vector<uint8_t> smpte = TwoTracks();
    smpte[12] = 0xE7;
    smpte[13] = 0x28;
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { SongOfMidi(smpte, "x"); }) ==
                 "The MIDI file uses SMPTE time. Hokuto supports only ticks per quarter note.");
}

// A sample of `points` points of a saw, with its root key at `root` and a loop over its second half.
sf2::Sample Saw(const std::string& name, int root, uint32_t rate, std::size_t points)
{
    sf2::Sample sample;
    sample.name = name;
    sample.sample_rate = rate;
    sample.original_key = static_cast<uint8_t>(root);
    sample.loop_start = static_cast<uint32_t>(points / 2);
    sample.loop_end = static_cast<uint32_t>(points);
    for (std::size_t i = 0; i < points; ++i)
    {
        sample.data.push_back(static_cast<int16_t>(static_cast<int>(i % 64) * 1000 - 32000));
    }

    return sample;
}

// A zone of `generators` that plays sample `sample`.
sf2::Zone SampleZone(std::vector<sf2::Generator> generators, int sample)
{
    generators.push_back(sf2::Generator::Value(sf2::kSampleId, sample));
    return {std::move(generators), {}};
}

// A preset zone of instrument `instrument`.
sf2::Zone InstrumentZone(int instrument)
{
    return {{sf2::Generator::Value(sf2::kInstrument, instrument)}, {}};
}

// The SoundFont of the bank test. Preset 0 of bank 0 plays instrument 0: keys up to 59 play the first saw with a loop,
// 100 cB of attenuation, a pan of 250, an envelope and exclusive class 7. Keys from 60 play the second saw once, tuned
// 2 semitones and 30 cents up, with a filter. Preset 9 of bank 0 plays instrument 1: the first saw once, at every key
// with a scale tuning of 0 and a delay of the volume envelope. Preset 0 of bank 1 plays the second saw.
std::vector<uint8_t> TestFont()
{
    using G = sf2::Generator;
    sf2::SoundFont font;
    font.samples.push_back(Saw("first", 60, 22050, 512));
    font.samples.push_back(Saw("second", 72, 32000, 300));
    font.instruments.push_back({"two zones",
                                {SampleZone({G::Range(sf2::kKeyRange, 0, 59), G::Value(sf2::kSampleModes, 1),
                                             G::Value(sf2::kInitialAttenuation, 100), G::Value(sf2::kPan, 250),
                                             G::Value(sf2::kAttackVolEnv, -1200), G::Value(sf2::kDecayVolEnv, 0),
                                             G::Value(sf2::kSustainVolEnv, 200), G::Value(sf2::kReleaseVolEnv, 1200),
                                             G::Value(sf2::kExclusiveClass, 7)},
                                            0),
                                 SampleZone({G::Range(sf2::kKeyRange, 60, 127), G::Value(sf2::kCoarseTune, 2),
                                             G::Value(sf2::kFineTune, 30), G::Value(sf2::kInitialFilterFc, 6000)},
                                            1)}});
    font.instruments.push_back(
        {"fixed", {SampleZone({G::Value(sf2::kScaleTuning, 0), G::Value(sf2::kDelayVolEnv, 0)}, 0)}});
    font.instruments.push_back({"other bank", {SampleZone({}, 1)}});
    font.presets.push_back({"two zones", 0, 0, {InstrumentZone(0)}});
    font.presets.push_back({"fixed", 9, 0, {InstrumentZone(1)}});
    font.presets.push_back({"other bank", 0, 1, {InstrumentZone(2)}});

    return sf2::Write(font);
}

// The region of `bank`'s program `program` at `key`, velocity 64.
const sar::VelocityRegion* RegionAt(const sar::BankFile& bank, uint32_t program, int key)
{
    if (program >= bank.instruments.size() || !bank.instruments[program])
    {
        return nullptr;
    }
    for (const sar::BankFile::KeyRegion& keys : bank.instruments[program]->key_regions)
    {
        if (key >= keys.first_key && key <= keys.last_key)
        {
            for (const sar::BankFile::Region& velocities : keys.regions)
            {
                if (64 >= velocities.first_velocity && 64 <= velocities.last_velocity)
                {
                    return &velocities.values;
                }
            }
        }
    }

    return nullptr;
}

// A SoundFont whose sample's byOriginalPitch is 255, unpitched, in a zone without an overriding root key. By section
// 7.10 of the SoundFont 2.04 specification, such a sample plays as key 60. The region of the bank plays the sample at
// its pitch at key 60.
void TestUnpitchedBank()
{
    sf2::SoundFont font;
    font.samples.push_back(Saw("unpitched", 255, 22050, 512));
    font.instruments.push_back({"drum", {SampleZone({}, 0)}});
    font.presets.push_back({"drum", 0, 0, {InstrumentZone(0)}});

    const hokuto::edit::SoundFontBank bank = hokuto::edit::BankOfSoundFont(sf2::Write(font), sar::Flavor::kNx);

    const sar::BankFile read = sar::BankFile::Read(bank.bank);
    const sar::VelocityRegion* region = RegionAt(read, 0, 60);
    XSFLIB_CHECK(region != nullptr && region->original_key == 60 && std::abs(region->pitch - 1.0f) < 1e-6f);
}

// A SoundFont becomes a bank file of `flavor`: its instruments, their key regions and envelopes, the waves at their
// rates, and the presets of the other MIDI banks.
void TestBank(sar::Flavor flavor)
{
    const hokuto::edit::SoundFontBank bank = hokuto::edit::BankOfSoundFont(TestFont(), flavor);
    const sar::BankFile read = sar::BankFile::Read(bank.bank);
    XSFLIB_CHECK(read.instruments.size() == 10);
    XSFLIB_CHECK(read.instruments[0] && read.instruments[9]);
    for (std::size_t program = 1; program < 9; ++program)
    {
        XSFLIB_CHECK(!read.instruments[program]);
    }

    // Instrument 0: the looped saw up to key 59, and the tuned saw from key 60.
    const sar::VelocityRegion* low = RegionAt(read, 0, 40);
    const sar::VelocityRegion* high = RegionAt(read, 0, 90);
    XSFLIB_CHECK(low != nullptr && high != nullptr);
    XSFLIB_CHECK(low->original_key == 60 && high->original_key == 72);
    XSFLIB_CHECK(std::abs(low->pitch - 1.0f) < 1e-6f);
    XSFLIB_CHECK(std::abs(high->pitch - static_cast<float>(std::exp2(230.0 / 1200.0))) < 1e-5f);
    XSFLIB_CHECK(low->volume == std::lround(127.0 * std::pow(10.0, -40.0 / 200.0)) && high->volume == 127);
    XSFLIB_CHECK(low->pan == 64 + 32 && high->pan == 64);
    XSFLIB_CHECK(low->key_group == 1 && high->key_group == 0);

    // The envelope of the looped saw comes nearest the zone envelope.
    const ntdlib::sequence_export::Settings settings = flavor == sar::Flavor::kCafe
                                                           ? ntdlib::sequence_export::CafeSettings()
                                                           : ntdlib::sequence_export::SwitchSettings();
    const auto envelope = ntdlib::sequence_export::ConvertEnvelope({.attack = low->adshr.attack,
                                                                    .decay = low->adshr.decay,
                                                                    .sustain = low->adshr.sustain,
                                                                    .hold = low->adshr.hold,
                                                                    .release = low->adshr.release},
                                                                   settings);
    XSFLIB_CHECK(std::abs(envelope.attack + 1200) < 300 && std::abs(envelope.release - 1200) < 300);
    XSFLIB_CHECK(std::abs(envelope.sustain - 200) < 30);

    // The waves: the looped saw up to the end of its loop, the other saw whole, and the first saw again without its
    // loop for instrument 9, at their rates. Instrument 9 plays its wave at the sample's pitch at every key.
    const sar::WaveArchiveFile archive = sar::WaveArchiveFile::Read(bank.wave_archive);
    XSFLIB_CHECK(read.waves.size() == 3 && archive.waves.size() == 3 && archive.flavor == flavor);
    for (std::size_t i = 0; i < read.waves.size(); ++i)
    {
        XSFLIB_CHECK(read.waves[i].wave_archive_item == hokuto::edit::kSoundFontWaves && read.waves[i].index == i);
    }
    const sar::WaveFile looped = sar::WaveFile::Read(archive.waves.at(low->wave_id_index));
    const sar::WaveFile once = sar::WaveFile::Read(archive.waves.at(high->wave_id_index));
    XSFLIB_CHECK(low->wave_id_index != high->wave_id_index);
    XSFLIB_CHECK(looped.encoding == sar::WaveEncoding::kPcm16 && looped.sample_rate == 22050 && looped.loop &&
                 looped.loop_start == 256 && looped.loop_end == 512 && looped.channels.size() == 1);
    XSFLIB_CHECK(!once.loop && once.sample_rate == 32000 && once.loop_end == 300);
    const auto point = [flavor](const sar::WaveFile& wave, std::size_t i)
    {
        const uint8_t* at = &wave.channels[0].samples[i * 2];
        return static_cast<int16_t>(flavor == sar::Flavor::kCafe ? xsflib::LoadBe16(at) : xsflib::LoadLe16(at));
    };
    XSFLIB_CHECK(point(looped, 0) == -32000 && point(looped, 65) == -31000 && point(once, 299) == 11000);
    const sar::VelocityRegion* fixed_40 = RegionAt(read, 9, 40);
    const sar::VelocityRegion* fixed_80 = RegionAt(read, 9, 80);
    XSFLIB_CHECK(fixed_40 != nullptr && fixed_80 != nullptr);
    XSFLIB_CHECK(fixed_40->wave_id_index == fixed_80->wave_id_index);
    XSFLIB_CHECK(fixed_40->wave_id_index != low->wave_id_index && fixed_40->wave_id_index != high->wave_id_index);
    const sar::WaveFile fixed = sar::WaveFile::Read(archive.waves.at(fixed_40->wave_id_index));
    XSFLIB_CHECK(!fixed.loop && fixed.sample_rate == 22050 && fixed.loop_end == 512 && point(fixed, 65) == -31000);
    XSFLIB_CHECK(fixed_40->original_key == 40 && fixed_80->original_key == 80);
    XSFLIB_CHECK(std::abs(fixed_40->pitch - 1.0f) < 1e-6f && std::abs(fixed_80->pitch - 1.0f) < 1e-6f);

    XSFLIB_CHECK(bank.warnings == (std::vector<std::string>{"Ignored at 68 keys: low-pass filter.",
                                                            "Ignored at 128 keys: volume envelope delay."}));

    // Bank 1 has one preset. A bank without presets has none to convert. Every bank puts bank 1's preset 0 at
    // program 128.
    const sar::BankFile other = sar::BankFile::Read(hokuto::edit::BankOfSoundFont(TestFont(), flavor, 1).bank);
    XSFLIB_CHECK(other.instruments.size() == 1 && other.instruments[0]);
    const sar::BankFile every =
        sar::BankFile::Read(hokuto::edit::BankOfSoundFont(TestFont(), flavor, hokuto::edit::kEveryMidiBank).bank);
    XSFLIB_CHECK(every.instruments.size() == 129 && every.instruments[0] && every.instruments[9] &&
                 every.instruments[128]);
    XSFLIB_CHECK(RegionAt(every, 128, 40) != nullptr && RegionAt(every, 128, 40)->original_key == 72);
    XSFLIB_CHECK(RegionAt(every, 0, 40) != nullptr && RegionAt(every, 0, 40)->original_key == 60);
    XSFLIB_CHECK(
        !ErrorOf<std::invalid_argument>([&] { hokuto::edit::BankOfSoundFont(TestFont(), flavor, 2); }).empty());
    XSFLIB_CHECK(
        !ErrorOf<std::invalid_argument>([&] { hokuto::edit::BankOfSoundFont(std::vector<uint8_t>{1, 2, 3}, flavor); })
             .empty());
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("midi_songs",
                                    []
                                    {
                                        TestSong();
                                        TestKeepsTime();
                                        TestEndsAtLoop();
                                        TestLayers();
                                        TestNonRegistered();
                                        TestRefusals();
                                        TestBank(sar::Flavor::kCafe);
                                        TestBank(sar::Flavor::kNx);
                                        TestUnpitchedBank();
                                    });
}
