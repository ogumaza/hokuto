// SPDX-License-Identifier: MIT

// Tests the sequences of the songs (src/song/sequence.cpp): ticks, tempos, settings, parts and reading.

#include "song_runs.h"

namespace
{

// Before the loop's end, a tick count is its position. From there on, the count goes round the loop.
void TestPositionOfTicks()
{
    Sequence looped;
    looped.loop = Loop{96, 480};
    Sequence empty_loop;
    empty_loop.loop = Loop{96, 96};

    SEQLIB_CHECK_EQ(PositionOfTicks(looped, 0), 0);
    SEQLIB_CHECK_EQ(PositionOfTicks(looped, 479), 479);
    SEQLIB_CHECK_EQ(PositionOfTicks(looped, 480), 96);
    SEQLIB_CHECK_EQ(PositionOfTicks(looped, 863), 479);
    SEQLIB_CHECK_EQ(PositionOfTicks(looped, 864), 96);
    SEQLIB_CHECK_EQ(PositionOfTicks(Sequence{}, 1000), 1000);
    SEQLIB_CHECK_EQ(PositionOfTicks(empty_loop, 1000), 1000);
}

// A sequence of `timebase` ticks a quarter note with `tempos`, a loop over `loop`, and a note of key 60 on track 0 at
// each tick of `starts`.
Sequence TimedSong(int timebase, std::vector<hokuto::song::Tempo> tempos, Loop loop, std::vector<uint32_t> starts)
{
    Sequence sequence;
    sequence.timebase = timebase;
    sequence.tempo = std::move(tempos);
    sequence.loop = loop;
    sequence.length = loop.end;
    Track& track = sequence.tracks.emplace_back();
    for (const uint32_t start : starts)
    {
        track.notes.push_back(Note{.start = start, .length = 1, .key = 60});
    }

    return sequence;
}

// A conversion whose quarter notes play at the arrangement's times maps ticks by the timebases. One whose quarter note
// has another length maps them by time, through the notes and the loops. The arrangement below counts ticks of 20 ms,
// at 20 ticks to a quarter note of 150 beats a minute. Its conversion plays the first 400 ms as a quarter note of 150,
// and the rest at 50 beats a minute, 25 ms a tick at 48 ticks to the quarter note. The whole tempos of a conversion can
// put a note a tick late: the note at the arrangement's tick 60 (1,200 ms) starts at 81 for 80 in the conversion.
void TestTickMap()
{
    using hokuto::song::CountedTicksOn;
    using hokuto::song::PlayedTickOf;
    using hokuto::song::ShownTickOf;
    using hokuto::song::TickMap;
    const auto near = [](double a, double b)
    {
        return std::abs(a - b) < 1e-6;
    };

    // The default map, and a map by the timebases: a song of 24 ticks a quarter note and its conversion at 48.
    SEQLIB_CHECK(TickMap().Played(37.5) == 37.5);
    const Sequence gba = TimedSong(24, {{0, 150.0}}, Loop{24, 96}, {0, 24, 48});
    Sequence gba_conversion = TimedSong(48, {{0, 150.0}}, Loop{48, 192}, {0, 48, 96});
    const TickMap by_timebases(gba, gba_conversion);
    SEQLIB_CHECK(!by_timebases.ByTime());
    SEQLIB_CHECK(by_timebases.Played(24.0) == 48.0);
    SEQLIB_CHECK(by_timebases.Played(25.0) == 50.0);
    SEQLIB_CHECK(by_timebases.Shown(97.0) == 48.5);
    gba_conversion.arrangement_ticks = by_timebases;
    SEQLIB_CHECK(PlayedTickOf(gba, gba_conversion, 30.0) == 60.0);
    SEQLIB_CHECK(ShownTickOf(gba, gba_conversion, 60.0) == 30.0);
    SEQLIB_CHECK(PlayedTickOf(gba, gba, 30.0) == 30.0);

    // A map by time passes through the notes and the loops.
    const Sequence frames = TimedSong(20, {{0, 150.0}}, Loop{30, 270}, {0, 30, 60, 90, 150});
    Sequence beat = TimedSong(48, {{0, 150.0}, {48, 50.0}}, Loop{56, 248}, {0, 56, 81, 104, 152});
    const TickMap by_time(frames, beat);
    SEQLIB_CHECK(by_time.ByTime());
    SEQLIB_CHECK(by_time.Played(0.0) == 0.0);
    SEQLIB_CHECK(by_time.Played(30.0) == 56.0);
    SEQLIB_CHECK(by_time.Played(60.0) == 81.0);
    SEQLIB_CHECK(by_time.Played(270.0) == 248.0);
    SEQLIB_CHECK(by_time.Shown(104.0) == 90.0);
    SEQLIB_CHECK(by_time.Shown(248.0) == 270.0);

    // Between two points, a tick maps to the same share of the time between them. 1,000 ms is 2/3 of the way from the
    // note at 600 ms to the note at 1,200 ms. The conversion's notes are 625 ms apart, and 2/3 of the way is 1,016.7 ms
    // there. 300 ms is halfway to the note at 600 ms: tick 36 at 150 beats a minute. Past the last point, a tick maps
    // to the same time after it.
    SEQLIB_CHECK(near(by_time.Played(50.0), 48.0 + (1'000.0 + 2.0 / 3.0 * 25.0 - 400.0) / 25.0));
    SEQLIB_CHECK(near(by_time.Shown(48.0 + (1'000.0 + 2.0 / 3.0 * 25.0 - 400.0) / 25.0), 50.0));
    SEQLIB_CHECK(near(by_time.Played(15.0), 36.0));
    SEQLIB_CHECK(near(by_time.Played(300.0), 272.0));
    SEQLIB_CHECK(near(by_time.Shown(272.0), 300.0));

    // A model's count wraps at each loop. Two passes and 15 ticks into the next, at tick 45 halfway between the
    // notes at 600 ms and 1,200 ms, give two passes of the conversion's loop and tick 68.5: 912.5 ms there.
    beat.arrangement_ticks = by_time;
    SEQLIB_CHECK(near(CountedTicksOn(frames, frames, beat, 525.0), 68.5 + 2.0 * 192.0));
    SEQLIB_CHECK(near(CountedTicksOn(frames, beat, frames, 68.5 + 2.0 * 192.0), 525.0));
    SEQLIB_CHECK(near(CountedTicksOn(frames, frames, beat, 15.0), 36.0));

    // A conversion whose loop the performer found a pass late, at 5,400 ms for 600 ms: its loop counts in its first
    // pass, from 56 to 248. A loop half a pass off counts as it is.
    using hokuto::song::FirstLoopOfConversion;
    const Sequence late =
        TimedSong(48, {{0, 150.0}, {48, 50.0}}, Loop{248, 440}, {0, 56, 81, 104, 152, 248, 273, 296, 344});
    const Sequence half_off = TimedSong(48, {{0, 150.0}, {48, 50.0}}, Loop{150, 342}, {0});
    SEQLIB_CHECK(FirstLoopOfConversion(frames, late) == (Loop{56, 248}));
    SEQLIB_CHECK(!FirstLoopOfConversion(frames, beat));
    SEQLIB_CHECK(!FirstLoopOfConversion(frames, half_off));
    const TickMap late_map(frames, late);
    SEQLIB_CHECK(late_map.Played(30.0) == 56.0);
    SEQLIB_CHECK(late_map.Played(60.0) == 81.0);
    SEQLIB_CHECK(late_map.Played(270.0) == 248.0);
}

// A setting holds from its tick to the next one. Before the first, the default holds.
void TestSettingAt()
{
    const std::vector<Setting> kSettings = {{48, 100}, {96, 50}};

    SEQLIB_CHECK_EQ(SettingAt(kSettings, 0, 127), 127);
    SEQLIB_CHECK_EQ(SettingAt(kSettings, 48, 127), 100);
    SEQLIB_CHECK_EQ(SettingAt(kSettings, 95, 127), 100);
    SEQLIB_CHECK_EQ(SettingAt(kSettings, 96, 127), 50);
    SEQLIB_CHECK_EQ(SettingAt({}, 96, 127), 127);
}

// A tempo holds from its tick to the next one. A sequence without one plays at 120 beats a minute.
void TestTempoAt()
{
    Sequence sequence;
    sequence.tempo = {{0, 140.0}, {96, 90.0}};

    SEQLIB_CHECK(TempoAt(sequence, 95) == 140.0);
    SEQLIB_CHECK(TempoAt(sequence, 96) == 90.0);
    SEQLIB_CHECK(TempoAt(Sequence{}, 0) == 120.0);
}

// At timebase 48, a tick lasts 10.4 ms at 120 BPM and 20.8 ms at 60 BPM. From tick 96 on, the ticks go at 60 BPM.
// Before the first tempo, they go at 120 BPM.
void TestMsOfTick()
{
    Sequence sequence;
    sequence.timebase = 48;
    sequence.tempo = {{0, 120.0}, {96, 60.0}};

    const auto near = [](double a, double b)
    {
        return std::abs(a - b) < 1e-9;
    };

    SEQLIB_CHECK(near(MsOfTick(sequence, 48.0), 500.0));
    SEQLIB_CHECK(near(MsOfTick(sequence, 96.0), 1000.0));
    SEQLIB_CHECK(near(MsOfTick(sequence, 120.0), 1500.0));
    SEQLIB_CHECK(near(TickOfMs(sequence, 500.0), 48.0));
    SEQLIB_CHECK(near(TickOfMs(sequence, 1500.0), 120.0));
    SEQLIB_CHECK(near(TickOfMs(sequence, 3000.0), 192.0));
    SEQLIB_CHECK(near(MsOfTick(Sequence{}, 96.0), 1000.0));
}

// A change comes in the first frame that starts at its time or later: a frame of 5 ms on the Switch, of 3 ms on the Wii
// U, and of 81920000 / 16756991 ms on the 3DS, about 4.889 ms. The 3DS's DSP runs at 134,055,928 Hz, and makes a sample
// in 4096 of its cycles and a frame in 160 samples.
void TestChangeFrame()
{
    SEQLIB_CHECK(ChangeFrame(0, kFrameMs) == 0u);
    SEQLIB_CHECK(ChangeFrame(1, kFrameMs) == 1u);
    SEQLIB_CHECK(ChangeFrame(5, kFrameMs) == 1u);
    SEQLIB_CHECK(ChangeFrame(6, kFrameMs) == 2u);
    SEQLIB_CHECK(ChangeFrame(760, kFrameMs) == 152u);
    SEQLIB_CHECK(ChangeFrame(4'294'967'295, kFrameMs) == 858'993'459u);

    const hokuto::song::FrameMs wii_u = hokuto::song::FrameMsOf(ntdlib::sar::Flavor::kCafe);
    SEQLIB_CHECK(ChangeFrame(3, wii_u) == 1u);
    SEQLIB_CHECK(ChangeFrame(4, wii_u) == 2u);
    SEQLIB_CHECK(ChangeFrame(4'294'967'295, wii_u) == 1'431'655'765u);

    const hokuto::song::FrameMs ctr = hokuto::song::FrameMsOf(ntdlib::sar::Flavor::kCtr);
    SEQLIB_CHECK(ChangeFrame(0, ctr) == 0u);
    SEQLIB_CHECK(ChangeFrame(4, ctr) == 1u);
    SEQLIB_CHECK(ChangeFrame(5, ctr) == 2u);
    SEQLIB_CHECK(ChangeFrame(9, ctr) == 2u);
    SEQLIB_CHECK(ChangeFrame(10, ctr) == 3u);
    SEQLIB_CHECK(ChangeFrame(1000, ctr) == 205u);
    SEQLIB_CHECK(ChangeFrame(4'294'967'295, ctr) == 878'548'930u);
    SEQLIB_CHECK(hokuto::song::FramesIn(1000, ctr) == 204u);
    SEQLIB_CHECK(hokuto::song::FramesIn(10000, ctr) == 2045u);
}

// ntdlib's performer counts its frames from the frame of the first tick: frame 1 of the models of the Switch and the
// Wii U, and frame 0 of the 3DS's archive mode (ctr_test checks that). A change comes in the same frame of the model
// either way.
void TestPerformOptions()
{
    const hokuto::song::Changes kChanges = {{0, 4, 1}, {12, 3, 7}};
    for (const auto& [flavor, first_tick] :
         {std::pair{sar::Flavor::kNx, 1u}, std::pair{sar::Flavor::kCafe, 1u}, std::pair{sar::Flavor::kCtr, 0u}})
    {
        const export_::PerformOptions options = hokuto::song::PerformOptionsOf({{2, 5}}, kChanges, flavor);
        SEQLIB_CHECK(options.variables.at(2) == 5);
        SEQLIB_REQUIRE_EQ(options.changes.size(), 2);
        SEQLIB_CHECK(options.changes[0].frame == 0 && options.changes[0].index == 4);
        const uint64_t frame = ChangeFrame(12, hokuto::song::FrameMsOf(flavor));
        SEQLIB_CHECK(options.changes[1].frame == frame - first_tick && options.changes[1].index == 3 &&
                     options.changes[1].value == 7);
    }
}

// On a grid of bars of 4 beats, a bar at timebase 48 is 192 ticks. Notes with 8 beats, two bars, or more between them
// go in separate parts. A part starts at a bar and ends at one, and the loop's start and end divide parts too: here at
// ticks 768 and 1536.
void TestSplitParts()
{
    Sequence sequence;
    sequence.timebase = 48;
    sequence.loop = Loop{768, 1536};
    Track track;
    track.notes = {{0, 48}, {200, 48}, {500, 100}, {800, 48}, {1400, 48}};

    const std::vector<Part> parts = SplitParts(sequence, track, 4, 8);

    SEQLIB_REQUIRE_EQ(parts.size(), 3);
    SEQLIB_CHECK(IsPart(parts[0], 0, 768, 0, 3));
    SEQLIB_CHECK(IsPart(parts[1], 768, 960, 3, 1));
    SEQLIB_CHECK(IsPart(parts[2], 1344, 1536, 4, 1));
}

// On a grid of beats, 48 ticks apart at timebase 48, a part starts at the beat before its first note and ends at the
// beat after its last note. The notes above, with the same gap, give the same three parts on beats: the first ends at
// 624 after the note that ends at 600, and the last runs from 1392 to 1488 around its note at 1400.
void TestSplitPartsOnBeats()
{
    Sequence sequence;
    sequence.timebase = 48;
    sequence.loop = Loop{768, 1536};
    Track track;
    track.notes = {{0, 48}, {200, 48}, {500, 100}, {800, 48}, {1400, 48}};

    const std::vector<Part> parts = SplitParts(sequence, track, 1, 8);

    SEQLIB_REQUIRE_EQ(parts.size(), 3);
    SEQLIB_CHECK(IsPart(parts[0], 0, 624, 0, 3));
    SEQLIB_CHECK(IsPart(parts[1], 768, 864, 3, 1));
    SEQLIB_CHECK(IsPart(parts[2], 1392, 1488, 4, 1));
}

// A track that plays a note, calls the subroutine at offset 100 twice with a note between, and then calls the one at
// 300. That subroutine never returns. The first call makes a second call inside it, to offset 200. On a grid of beats,
// each call makes a separate part from the call to the return, with the notes of the call inside it. The note between
// the calls gets a part from the beat before it to the next call. The call that never returns ends at the beat after
// its note.
void TestSplitPartsWithCalls()
{
    using hokuto::song::Stretch;
    using Entry = ntdlib::sequence_export::Entry;
    Sequence sequence;
    sequence.timebase = 48;
    Track track;
    track.stretches = {Stretch{.tick = 0, .first = 0},
                       Stretch{.tick = 96, .first = 100, .entry = Entry::kCall, .depth = 1},
                       Stretch{.tick = 120, .first = 200, .entry = Entry::kCall, .depth = 2},
                       Stretch{.tick = 130, .first = 110, .entry = Entry::kReturn, .depth = 1},
                       Stretch{.tick = 192, .first = 10, .entry = Entry::kReturn},
                       Stretch{.tick = 288, .first = 100, .entry = Entry::kCall, .depth = 1},
                       Stretch{.tick = 384, .first = 14, .entry = Entry::kReturn},
                       Stretch{.tick = 400, .first = 300, .entry = Entry::kCall, .depth = 1}};
    track.notes = {{.start = 0, .length = 48, .stretch = 0},   {.start = 96, .length = 24, .stretch = 1},
                   {.start = 120, .length = 10, .stretch = 2}, {.start = 144, .length = 24, .stretch = 3},
                   {.start = 200, .length = 48, .stretch = 4}, {.start = 288, .length = 48, .stretch = 5},
                   {.start = 400, .length = 30, .stretch = 7}};

    const std::vector<Part> parts = SplitParts(sequence, track, 1, 8);

    SEQLIB_REQUIRE_EQ(parts.size(), 5);
    SEQLIB_CHECK(IsPart(parts[0], 0, 48, 0, 1) && !parts[0].call);
    SEQLIB_CHECK(IsPart(parts[1], 96, 192, 1, 3) && parts[1].call == 100u);
    SEQLIB_CHECK(IsPart(parts[2], 192, 288, 4, 1) && !parts[2].call);
    SEQLIB_CHECK(IsPart(parts[3], 288, 384, 5, 1) && parts[3].call == 100u);
    SEQLIB_CHECK(IsPart(parts[4], 400, 432, 6, 1) && parts[4].call == 300u);

    // A note that the driver never started joins the call's part while the call hasn't returned, and outside
    // calls after the return: at tick 150 into the first call's part, and at tick 250 into the part after it.
    track.unstarted = {{.start = 150, .length = 10}, {.start = 250, .length = 10}};
    const std::vector<Part> with_unstarted = SplitParts(sequence, track, 1, 8);
    SEQLIB_REQUIRE_EQ(with_unstarted.size(), 5);
    for (std::size_t i = 0; i < parts.size(); i++)
    {
        SEQLIB_CHECK(IsPart(with_unstarted[i], parts[i].start, parts[i].end, parts[i].first, parts[i].count) &&
                     with_unstarted[i].call == parts[i].call);
    }
    SEQLIB_CHECK(with_unstarted[0].unstarted_count == 0);
    SEQLIB_CHECK(with_unstarted[1].unstarted_first == 0 && with_unstarted[1].unstarted_count == 1);
    SEQLIB_CHECK(with_unstarted[2].unstarted_first == 1 && with_unstarted[2].unstarted_count == 1);
    SEQLIB_CHECK(with_unstarted[3].unstarted_count == 0 && with_unstarted[4].unstarted_count == 0);
}

// The notes that the driver never started count among a track's notes for its parts. On a grid of bars, 192 ticks at
// timebase 48, with gaps of 8 beats, the track plays notes at ticks 0, 200 and 1400, and never starts notes at 100 and
// 900. The note at 100 joins the first part. The note at 900 makes a part by itself, from the bar before it to the bar
// after it, and the note at 1400 another. A part that contains only notes that never started starts at the next note of
// the track.
void TestSplitPartsWithUnstartedNotes()
{
    Sequence sequence;
    sequence.timebase = 48;
    Track track;
    track.notes = {{0, 48}, {200, 48}, {1400, 48}};
    track.unstarted = {{100, 24}, {900, 48}};

    const std::vector<Part> parts = SplitParts(sequence, track, 4, 8);

    SEQLIB_REQUIRE_EQ(parts.size(), 3);
    SEQLIB_CHECK(IsPart(parts[0], 0, 384, 0, 2) && parts[0].unstarted_first == 0 && parts[0].unstarted_count == 1);
    SEQLIB_CHECK(IsPart(parts[1], 768, 960, 2, 0) && parts[1].unstarted_first == 1 && parts[1].unstarted_count == 1);
    SEQLIB_CHECK(IsPart(parts[2], 1344, 1536, 2, 1) && parts[2].unstarted_first == 2 && parts[2].unstarted_count == 0);

    // A track whose notes never start still has a part.
    Track silent;
    silent.unstarted = {{100, 24}, {200, 24}};
    const std::vector<Part> alone = SplitParts(sequence, silent, 4, 8);
    SEQLIB_REQUIRE_EQ(alone.size(), 1);
    SEQLIB_CHECK(IsPart(alone[0], 0, 384, 0, 0) && alone[0].unstarted_first == 0 && alone[0].unstarted_count == 2);
}

// The performer reads the intro and one pass of the loop. The track's instrument is the one its first note plays.
void TestReadSequence()
{
    const std::shared_ptr<const sar::SoundArchive> archive = OpenTestArchive();

    const Sequence sequence = ReadSequence(*archive, kLoopSound);

    SEQLIB_CHECK(sequence.name == "SEQ_LOOP");
    SEQLIB_CHECK_EQ(sequence.sound, kLoopSound);
    SEQLIB_CHECK_EQ(sequence.timebase, 48);
    SEQLIB_CHECK(TempoAt(sequence, 0) == 120.0);
    SEQLIB_CHECK_EQ(sequence.length, 192);
    SEQLIB_CHECK_EQ(sequence.notes_end, 144);
    SEQLIB_REQUIRE(sequence.loop.has_value());
    SEQLIB_CHECK_EQ(sequence.loop->start, 96);
    SEQLIB_CHECK_EQ(sequence.loop->end, 192);
    SEQLIB_REQUIRE_EQ(sequence.tracks.size(), 1);
    const Track& track = sequence.tracks[0];
    SEQLIB_CHECK(track.name == "Track 0");
    SEQLIB_CHECK(track.bank == "BANK_TEST");
    SEQLIB_CHECK_EQ(track.program, 0);
    SEQLIB_REQUIRE_EQ(track.notes.size(), 2);
    SEQLIB_CHECK(IsNote(track.notes[0], 0, 48, 60, 100));
    SEQLIB_CHECK(IsNote(track.notes[1], 96, 48, 64, 100));
}

// A note that never ends lasts to the end of the loop. The performer repeats the loop of 48 ticks until the performance
// lasts 10 s, but the sequence ends with the loop.
void TestHeldNote()
{
    const std::shared_ptr<const sar::SoundArchive> archive = OpenHeldArchive(kHeldBytecode);

    const Sequence sequence = ReadSequence(*archive, kHeldSound);

    SEQLIB_REQUIRE(sequence.loop.has_value());
    SEQLIB_CHECK_EQ(sequence.loop->start, 24);
    SEQLIB_CHECK_EQ(sequence.loop->end, 72);
    SEQLIB_CHECK_EQ(sequence.length, 72);
    SEQLIB_CHECK_EQ(sequence.notes_end, 72);
    SEQLIB_REQUIRE(sequence.tracks.size() == 1 && sequence.tracks[0].notes.size() == 1);
    SEQLIB_CHECK(IsNote(sequence.tracks[0].notes[0], 0, 72, 60, 100));
}

// A note that ends in a later pass lasts its length, past the loop's end. Only a note that still sounds where the
// performer stops lasts to the loop's end. The sequence then lasts to the end of its last note.
void TestNoteAcrossTheLoop()
{
    const std::shared_ptr<const sar::SoundArchive> archive = OpenHeldArchive(kAcrossBytecode);

    const Sequence sequence = ReadSequence(*archive, kHeldSound);

    SEQLIB_REQUIRE(sequence.loop.has_value());
    SEQLIB_REQUIRE(sequence.tracks.size() == 1 && !sequence.tracks[0].notes.empty());
    const Note& last = sequence.tracks[0].notes.back();
    SEQLIB_CHECK(last.start + 48 > sequence.loop->end);
    SEQLIB_CHECK_EQ(sequence.notes_end, last.start + 48);
    SEQLIB_CHECK_EQ(sequence.length, last.start + 48);
    // The intro rests until tick 24. Each pass of the loop then rests 24 ticks and starts its note. The first note
    // therefore starts at tick 48, and each later one a pass of 48 ticks after the one before.
    uint32_t start = 48;
    for (const Note& note : sequence.tracks[0].notes)
    {
        SEQLIB_CHECK(IsNote(note, start, 48, 62, 100));
        start += 48;
    }
}

// Each note plays the instrument that the commands chose before it: its program and its slot of the sound's banks. The
// track's instrument is the first note's.
void TestNoteInstruments()
{
    const std::shared_ptr<const sar::SoundArchive> archive = OpenInstrumentsArchive();

    const Sequence sequence = ReadSequence(*archive, kInstrumentsSound);

    SEQLIB_REQUIRE(sequence.tracks.size() == 1 && sequence.tracks[0].notes.size() == 3);
    const std::vector<Note>& notes = sequence.tracks[0].notes;
    SEQLIB_CHECK(IsNote(notes[0], 0, 24, 60, 100) && notes[0].bank_slot == 0 && notes[0].program == 0);
    SEQLIB_CHECK(IsNote(notes[1], 24, 24, 62, 100) && notes[1].bank_slot == 0 && notes[1].program == 3);
    SEQLIB_CHECK(IsNote(notes[2], 48, 24, 64, 100) && notes[2].bank_slot == 1 && notes[2].program == 3);
    SEQLIB_CHECK(sequence.tracks[0].bank == "BANK_TEST");
    SEQLIB_CHECK_EQ(sequence.tracks[0].program, 0);
}

// A track's mix is what its commands set: the pan sound's track pans to -32 at tick 0, and the send sound's track sends
// to aux bus A at full level. A setting that no command sets keeps its default.
void TestReadMix()
{
    const std::shared_ptr<const sar::SoundArchive> archive = OpenSendArchive();

    const Sequence panned = ReadSequence(*archive, kPanSound);
    const Sequence sent = ReadSequence(*archive, kSendSound);

    SEQLIB_REQUIRE(panned.tracks.size() == 1 && sent.tracks.size() == 1);
    const Track& left = panned.tracks[0];
    const Track& sending = sent.tracks[0];
    const std::vector<Setting> kPanned = {{0, -32}};
    const std::vector<Setting> kSent = {{0, 127}};
    const auto no_sends = [](const std::vector<Setting>& sends)
    {
        return sends.empty();
    };
    SEQLIB_CHECK(left.pan == kPanned && left.volume.empty() && MixAt(left, Mix::kVolume, 24) == 127);
    SEQLIB_CHECK(std::ranges::all_of(left.sends, no_sends));
    SEQLIB_CHECK(sending.sends[0] == kSent && MixAt(sending, Mix::kSendA, 24) == 127);
    SEQLIB_CHECK(sending.pan.empty() && sending.sends[1].empty() && sending.sends[2].empty());
}

// A sequence holds its file's labels by their offsets. The pan sound's file has one label, at its start.
void TestReadLabels()
{
    const std::shared_ptr<const sar::SoundArchive> archive = OpenSendArchive();

    const Sequence panned = ReadSequence(*archive, kPanSound);

    SEQLIB_CHECK(panned.labels == (std::map<uint32_t, std::string>{{0, "Pan"}}));
}

// Each call from outside any call makes a part from the call to the return, with the subroutine's offset. The fill's
// part holds the note of the phrase that the fill calls. Both sounds of the file count its calls: three of the phrase,
// one from each sound and one from the fill, and one of the fill. The call that no sound reaches doesn't count.
void TestReadCalls()
{
    const std::shared_ptr<const sar::SoundArchive> archive = OpenCallsArchive();

    const Sequence calls = ReadSequence(*archive, kCallsSound);
    const Sequence again = ReadSequence(*archive, kCallsAgainSound);
    SEQLIB_REQUIRE(!calls.tracks.empty());
    const std::vector<Part> parts = SplitParts(calls, calls.tracks[0], 1, 8);

    const std::map<uint32_t, int> kExpected = {{24, 3}, {28, 1}};
    SEQLIB_CHECK(calls.calls == kExpected);
    SEQLIB_CHECK(again.calls == kExpected);
    SEQLIB_REQUIRE(calls.labels.size() == 4 && calls.labels.contains(24) && calls.labels.contains(28));
    SEQLIB_CHECK(calls.labels.at(24) == "Phrase" && calls.labels.at(28) == "Fill");
    SEQLIB_REQUIRE(parts.size() == 2);
    SEQLIB_CHECK(parts[0].start == 0 && parts[0].end == 48 && parts[0].count == 1 && parts[0].call == 24u);
    SEQLIB_CHECK(parts[1].start == 48 && parts[1].end == 144 && parts[1].count == 2 && parts[1].call == 28u);
    SEQLIB_CHECK(again.tracks.size() == 1 && again.tracks[0].notes.size() == 1);
}

// The variables that the sound reads before it sets them, by their numbers. Each choice gives the comparisons of its
// variable other outcomes than the other choices give: one value is equal to 1 and one isn't, and so on. The sound sets
// variables 1, 7 and 8 before it reads them, and they don't count. Variable 6 counts: a command behind an if prefix
// sets nothing for certain. Variable 9 counts too. One of the two paths to its comparison sets it, and the other
// doesn't. A wait takes variable 6's exact value, and the addition to variable 4 works from variable 4's value. Neither
// has choices. The sound sets variable 9. The variable has choices all the same: only a comparison with a fixed value
// reads it. The sequence takes the branch that its starting values choose. A value for a variable past the game's 32 is
// left out.
void TestReadVariables()
{
    const std::shared_ptr<const sar::SoundArchive> archive = OpenVariablesArchive();

    const Sequence unset = ReadSequence(*archive, kVariablesSound);
    const Sequence chosen = ReadSequence(*archive, kVariablesSound, {{0, 1}, {40, 7}});

    const std::vector<Variable> kExpected = {
        {0, {-1, 1}, false}, {2, {-1, 0, 2, 4}, false}, {3, {-1, 3, 5}, false}, {4, {}, true},
        {6, {}, true},       {9, {-1, 2}, true},        {16, {-1, 3}, false},
    };
    SEQLIB_CHECK(unset.variables == kExpected);
    SEQLIB_CHECK(chosen.variables == kExpected);
    SEQLIB_CHECK(unset.start_values.empty());
    SEQLIB_CHECK(chosen.start_values == StartValues({{0, 1}}));
    SEQLIB_REQUIRE(unset.tracks.size() == 1 && chosen.tracks.size() == 1);
    SEQLIB_REQUIRE(unset.tracks[0].notes.size() == 1 && chosen.tracks[0].notes.size() == 1);
    SEQLIB_CHECK(unset.tracks[0].notes[0].key == 60 && unset.tracks[0].notes[0].start == 8);
    SEQLIB_CHECK(chosen.tracks[0].notes[0].key == 64 && chosen.tracks[0].notes[0].start == 0);
}

// On the Wii, a global variable starts at 0 (SeqPlayer::SetGlobalVariable), and a player variable at -1. The choices of
// each start from its starting value.
void TestWiiGlobalsStartAtZero()
{
    using ntdlib::commands::Argument;
    using ntdlib::commands::Command;
    using ntdlib::commands::Kind;
    using ntdlib::commands::Source;

    const auto compare = [](uint8_t variable, int value)
    {
        Command command;
        command.kind = Kind::kCompareEqual;
        command.index = variable;
        command.value = Argument{Source::kValue, value * ntdlib::commands::kOne, 0};

        return command;
    };

    Command end;
    end.kind = Kind::kEnd;
    const ntdlib::commands::Sequence commands{{compare(0, 3), compare(16, 3), end}, {}};

    SEQLIB_CHECK(hokuto::song::VariablesOf(commands, 0).starting ==
                 std::vector<Variable>({{0, {-1, 3}, false}, {16, {-1, 3}, false}}));
    SEQLIB_CHECK(hokuto::song::VariablesOf(commands, 0, 0).starting ==
                 std::vector<Variable>({{0, {-1, 3}, false}, {16, {0, 3}, false}}));

    Sequence song;
    SEQLIB_CHECK(hokuto::song::StartingValue(song, 16) == -1);

    song.wii_build = ntdlib::SndBuild{};
    SEQLIB_CHECK(hokuto::song::StartingValue(song, 15) == -1 && hokuto::song::StartingValue(song, 16) == 0 &&
                 hokuto::song::StartingValue(song, 31) == 0);
}

// The paths of a sound's commands decide which variables count. A variable counts where one path to a command that
// reads it sets it and another doesn't. A call sets what its subroutine sets before every return, but not behind an if
// prefix. A track that a command opens runs only after the opening track stops for the tick: at a note, at a wait, in a
// call of a subroutine that waits, or in a loop that waits for another track. What the opening track set before then
// doesn't count in the opened track. A loop that sets a variable doesn't wait, such as a loop that draws a random value
// until the value isn't 0. A variable that a command takes as an argument has no choices, even where a comparison tests
// it against a fixed value. The model check below leaves out Branches. Its variable 1 changes the sound only while
// variable 0 is 1.
void TestReadVariablePaths()
{
    const std::shared_ptr<const sar::SoundArchive> archive = OpenVariablePathsArchive();

    const Sequence set_in_call = ReadSequence(*archive, kSetInCallSound);
    const Sequence set_before_wait = ReadSequence(*archive, kSetBeforeWaitSound);
    const Sequence wait_in_call = ReadSequence(*archive, kWaitInCallSound);
    const Sequence idle_loop = ReadSequence(*archive, kIdleLoopSound);
    const Sequence compare_then_transpose = ReadSequence(*archive, kCompareThenTransposeSound);
    const Sequence retry_in_call = ReadSequence(*archive, kRetryInCallSound);
    const Sequence branches = ReadSequence(*archive, kBranchesSound);

    SEQLIB_CHECK(set_in_call.variables == std::vector<Variable>({{2, {}, true}, {3, {-1, 1}, false}}));
    SEQLIB_CHECK(set_before_wait.variables == std::vector<Variable>({{5, {}, true}}));
    SEQLIB_CHECK(wait_in_call.variables == std::vector<Variable>({{4, {}, true}}));
    SEQLIB_CHECK(idle_loop.variables == std::vector<Variable>({{6, {-1, 1}, true}, {7, {}, true}}));
    SEQLIB_CHECK(compare_then_transpose.variables == std::vector<Variable>({{3, {}, false}}));
    SEQLIB_CHECK(retry_in_call.variables.empty());
    SEQLIB_CHECK(branches.variables == std::vector<Variable>({{0, {-1, 1}, false}, {1, {}, true}, {2, {}, true}}));
}

// The starting values of the variables that a sound lists change what ntdlib's model of nn::atk plays, and those of the
// variables that it leaves out don't. A variable with choices takes each choice, and any other variable 1 and 7.
void TestListedVariablesChangeTheSound()
{
    const std::shared_ptr<const sar::SoundArchive> archive = OpenVariablePathsArchive();
    constexpr std::size_t kFrames = 150;

    for (const uint32_t sound : {kSetInCallSound, kSetBeforeWaitSound, kWaitInCallSound, kIdleLoopSound,
                                 kCompareThenTransposeSound, kRetryInCallSound})
    {
        const Sequence sequence = ReadSequence(*archive, sound);
        const std::vector<int16_t> unset = RunModel(*archive, sound, kFrames).samples;
        for (int number = 0; number < 8; number++)
        {
            const auto numbered = [number](const Variable& variable)
            {
                return variable.number == number;
            };
            const auto listed = std::find_if(sequence.variables.begin(), sequence.variables.end(), numbered);
            const bool counts = listed != sequence.variables.end();
            const std::vector<int> values = counts && !listed->choices.empty() ? listed->choices : std::vector{1, 7};

            bool changes = false;
            for (const int value : values)
            {
                const StartValues start = {{number, static_cast<int16_t>(value)}};
                changes = changes || RunModel(*archive, sound, kFrames, {}, start).samples != unset;
            }

            SEQLIB_CHECK(changes == counts);
        }
    }
}

// The model starts the sound in its frame 0 and plays the first tick in frame 1. Its frame F plays the ticks from 5 ms
// before the frame's start up to its start. A change at 1,012 ms comes in frame 203. That frame plays tick 97, at
// 1,010.4 ms. The pass from tick 49 has compared the variable already, and it plays key 60 to its end. The pass from
// tick 97 plays key 72, and it's the loop. The two notes before it make the intro. Without the change, the first pass
// is the loop. With a change to 2 at 760 ms and one to 1 at 2,015 ms, the loop starts at tick 193. Frame 403 plays that
// tick. The changes come in the order of their times. A change of a variable past the game's is left out. The sound
// reads variable 0 only.
void TestReadChanges()
{
    const std::shared_ptr<const sar::SoundArchive> archive = OpenChangesArchive();
    const ModelRun model = RunModel(*archive, kChangesSound, 410);

    const Sequence unchanged = ReadSequence(*archive, kChangesSound);
    const Sequence changed = ReadSequence(*archive, kChangesSound, {}, {{1012, 0, 1}, {500, 40, 7}});
    const Sequence twice = ReadSequence(*archive, kChangesSound, {}, {{2015, 0, 1}, {760, 0, 2}});

    SEQLIB_REQUIRE(model.after[202] == 97 && model.after[203] == 98);
    SEQLIB_REQUIRE(model.after[402] == 193 && model.after[403] == 194);
    SEQLIB_CHECK(unchanged.loop == Loop({1, 49}));
    SEQLIB_CHECK(changed.loop == Loop({97, 145}));
    SEQLIB_CHECK(twice.loop == Loop({193, 241}));
    SEQLIB_CHECK(unchanged.changes.empty());
    SEQLIB_CHECK(changed.changes == Changes({{1012, 0, 1}}));
    SEQLIB_CHECK(twice.changes == Changes({{760, 0, 2}, {2015, 0, 1}}));
    SEQLIB_CHECK(changed.read_variables == std::vector<Variable>({{0, {-1, 1}, false}}));
    SEQLIB_CHECK(changed.variables == changed.read_variables);
    SEQLIB_REQUIRE(changed.tracks.size() == 1 && changed.tracks[0].notes.size() == 3);
    SEQLIB_CHECK(IsNote(changed.tracks[0].notes[0], 1, 48, 60, 100));
    SEQLIB_CHECK(IsNote(changed.tracks[0].notes[1], 49, 48, 60, 100));
    SEQLIB_CHECK(IsNote(changed.tracks[0].notes[2], 97, 48, 72, 100));
}

// A file that isn't an archive, a file that's missing, a sound past the archive's sounds and a sound that isn't a
// sequence give errors for the user. The wave sound plays a sequence file here: only its kind is refused.
void TestReadErrors()
{
    const std::shared_ptr<const sar::SoundArchive> archive = OpenTestArchive();
    sar_files::ArchiveSpec wave_sound_spec = MadeUpWaveSoundSpec();
    wave_sound_spec.sounds.back().file = 1;
    const std::shared_ptr<const sar::SoundArchive> with_wave_sound =
        LoadArchive(sar_files::Archive(false, wave_sound_spec), "test.bfsar");
    const std::vector<uint8_t> kText = {'H', 'o', 'k', 'u', 't', 'o'};
    const TemporaryFile not_archive(kText);
    const std::filesystem::path missing = not_archive.Path().parent_path() / "hokuto_song_test_missing.bfsar";

    SEQLIB_CHECK(ThrowsRuntimeError([&] { OpenArchive(not_archive.Path()); }));
    SEQLIB_CHECK(ThrowsRuntimeError([&] { OpenArchive(missing); }));
    SEQLIB_CHECK(ThrowsRuntimeError([&] { ReadSequence(*archive, kMissingSound); }));
    SEQLIB_CHECK(ThrowsRuntimeError([&] { ReadSequence(*with_wave_sound, kWaveSound); }));
}

// ntdlib's model of the Wii starts the sequence when the sound starts, and plays the first tick in its frame 0. The
// performer counts its frames from that frame (PerformOptionsOf). The sequence of kWiiChangesBytecode compares variable
// 0 with 1 at ticks 1, 49 and 97, every 48 ticks. A tick lasts 10.4167 ms at 120 BPM and 48 ticks to a quarter note.
// Frame F runs the ticks from 3F ms up to 3 (F + 1) ms. Frame 336 therefore runs tick 97, at 1,010.4 ms. A change at
// 1,008 ms comes in frame 336, before the comparison, and the pass from tick 97 is the loop. A change at 1,011 ms comes
// in frame 337, after it, and the loop starts at tick 145, in frame 503. Hokuto's model makes the changes in the same
// frames as the model run directly. The sound's commands read variable 0, with the choices -1 and 1.
void TestWiiReadChanges()
{
    const std::shared_ptr<const ntdlib::rsar::SoundArchive> archive = OpenWiiArchive(kWiiChangesBytecode);
    const auto run = [&archive](std::optional<uint64_t> change_frame)
    {
        ntdlib::rvl::snd::ArchiveModel direct;
        direct.build = export_::BuildOfRsarVersion(archive->Version());
        direct.StartWithRenderer(*archive);
        ntdlib::rvl::snd::SeqPlayer* player = direct.engine->StartSequence(0);
        std::pair<std::vector<uint32_t>, std::vector<int16_t>> ticks_and_output;
        for (uint64_t frame = 0; player != nullptr && frame < 600; frame++)
        {
            if (frame == change_frame)
            {
                player->SetVariable(*direct.engine, 0, 1);
            }
            if (!direct.RunFrame(ticks_and_output.second))
            {
                break;
            }
            ticks_and_output.first.push_back(player->TickCount());
        }

        return ticks_and_output;
    };

    const auto played = [&archive](const Changes& changes)
    {
        Sequence sequence = ReadSequence(*archive, 0);
        sequence.changes = changes;
        const auto model = std::make_shared<SequenceModel>(archive, std::make_shared<const Sequence>(sequence),
                                                           hokuto::song::ModelOptions{.snapshot_interval = 50});
        std::vector<int16_t> output;
        for (bool more = model->SeekFrame(0); more && output.size() < 600 * 96 * 2; more = model->RunFrame())
        {
            output.insert(output.end(), model->Output().begin(), model->Output().end());
        }

        return output;
    };

    const auto [ticks, unchanged] = run(std::nullopt);
    const std::vector<int16_t> in_336 = run(336).second;
    const std::vector<int16_t> in_337 = run(337).second;
    const Sequence before = ReadSequence(*archive, 0, {}, {{1008, 0, 1}});
    const Sequence after = ReadSequence(*archive, 0, {}, {{1011, 0, 1}});

    SEQLIB_REQUIRE(ticks.size() == 600);
    SEQLIB_CHECK(ticks[0] == 1 && ticks[335] == 97 && ticks[336] == 98);
    SEQLIB_CHECK(before.loop == Loop({97, 145}));
    SEQLIB_CHECK(after.loop == Loop({145, 193}));
    SEQLIB_CHECK(after.read_variables == std::vector<Variable>({{0, {-1, 1}, false}}));
    SEQLIB_CHECK(after.variables == after.read_variables);
    SEQLIB_REQUIRE(after.tracks.size() == 1 && after.tracks[0].notes.size() == 4);
    SEQLIB_CHECK(IsNote(after.tracks[0].notes[3], 145, 48, 72, 100));
    SEQLIB_CHECK(in_336 != unchanged && in_337 != unchanged && in_336 != in_337);
    SEQLIB_CHECK(played({{1008, 0, 1}}) == in_336);
    SEQLIB_CHECK(played({{1011, 0, 1}}) == in_337);
}

// The 3DS's player skips the commands of the LFOs past the first, of the first LFO's curve, phase and period, and of
// the send to bus C. The players of the Wii U and the Switch run them.
void TestRuns()
{
    using hokuto::song::Runs;
    using ntdlib::commands::Kind;
    SEQLIB_CHECK(Runs(sar::Flavor::kCtr, Kind::kSend, 1) && !Runs(sar::Flavor::kCtr, Kind::kSend, 2));
    SEQLIB_CHECK(Runs(sar::Flavor::kCtr, Kind::kLfoDepth, 0) && !Runs(sar::Flavor::kCtr, Kind::kLfoDepth, 1));
    SEQLIB_CHECK(!Runs(sar::Flavor::kCtr, Kind::kLfoCurve, 0) && !Runs(sar::Flavor::kCtr, Kind::kLfoPhase, 0));
    SEQLIB_CHECK(!Runs(sar::Flavor::kCtr, Kind::kLfoPeriod, 0) && Runs(sar::Flavor::kCtr, Kind::kLfoDelay, 0));
    SEQLIB_CHECK(Runs(sar::Flavor::kCtr, Kind::kTempo, 0) && Runs(sar::Flavor::kCtr, Kind::kNote, 0));
    SEQLIB_CHECK(Runs(sar::Flavor::kNx, Kind::kLfoCurve, 3) && Runs(sar::Flavor::kCafe, Kind::kSend, 2));
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("sequence",
                                    []
                                    {
                                        TestPositionOfTicks();
                                        TestTickMap();
                                        TestSettingAt();
                                        TestTempoAt();
                                        TestMsOfTick();
                                        TestChangeFrame();
                                        TestPerformOptions();
                                        TestSplitParts();
                                        TestSplitPartsOnBeats();
                                        TestSplitPartsWithUnstartedNotes();
                                        TestSplitPartsWithCalls();
                                        TestReadSequence();
                                        TestHeldNote();
                                        TestNoteAcrossTheLoop();
                                        TestNoteInstruments();
                                        TestWiiReadChanges();
                                        TestReadMix();
                                        TestReadLabels();
                                        TestReadCalls();
                                        TestReadVariables();
                                        TestWiiGlobalsStartAtZero();
                                        TestReadVariablePaths();
                                        TestListedVariablesChangeTheSound();
                                        TestReadChanges();
                                        TestReadErrors();
                                        TestRuns();
                                    });
}
