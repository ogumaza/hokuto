// SPDX-License-Identifier: MIT

// Tests the layout of the songs that NintendoWare's MIDI converter wrote (src/edit/converter.h), and the timelines of a
// file (src/edit/timelines.h). The tests write small songs as the converter writes them, at NintendoWare's default
// timebase of 48 ticks a quarter note. Each bar of 4/4 lasts 192 ticks. The main song's track 0 repeats a bar of 10
// commands. The converter calls three copies of it. The fourth copy holds the loop's start and stays in the track.
// Track 1 repeats a bar of 5 commands, too few to call. One of its rests is split in two with nothing between them.
// Both tracks loop from tick 384 to tick 768. The replacement's test puts the song in an archive of the Switch. The
// waited song's header waits 17 ticks before it starts its two tracks. A header written by hand can do that. Each track
// then loops over a bar from its start: the song's loop runs from tick 17 to tick 209.

#include "converter_fixtures.h"

namespace
{

using xsflib::testing::ErrorOf;

// Whether `sequence` holds a call.
bool HasCall(const Sequence& sequence)
{
    return std::any_of(sequence.items.begin(), sequence.items.end(),
                       [](const Command& command) { return command.kind == Kind::kCall; });
}

// The main song reads as a converted song. The calls show bars of 192 ticks, and the rules give every call: the layout
// with subroutines gives the song back. Without subroutines, every bar lies in its track and no call is left. The
// layout with subroutines gives the song back from there too.
void TestRoundTrip()
{
    const Sequence song = MainSong();
    const ConvertedSong converted = ReadConverted(song);
    SEQLIB_REQUIRE(converted.hints);
    XSFLIB_CHECK(converted.problem.empty());

    // The calls show bars of 192 ticks, and the rules give every call.
    const ConverterHints& hints = *converted.hints;
    XSFLIB_CHECK((hints == ConverterHints{{{192, 1}}, {}}));
    XSFLIB_CHECK(LayOutConverted(song, hints, true) == song);

    // Without subroutines, every bar is in its track. The header keeps its 2 items, and track 0 holds its 4 bars of 10
    // commands between the note wait and the jump. Track 1 keeps its 23 items, the split rest included. The end
    // follows. The layout with subroutines gives the song back.
    const Sequence unfolded = LayOutConverted(song, hints, false);
    XSFLIB_CHECK(!HasCall(unfolded) && unfolded.items.size() == 2 + 42 + 23 + 1);
    XSFLIB_CHECK(LayOutConverted(unfolded, hints, true) == song);
}

// The main song with track 0's second bar in the track. The rules would call the bar instead. The hints name the bar,
// and the layout gives the song back with them. Without the exception, the layout gives another song.
void TestExceptions()
{
    // The second bar of track 0 stays in the track although the rules would call it. The hints name it.
    const Sequence song = MainSong(true);
    const ConvertedSong converted = ReadConverted(song);
    XSFLIB_CHECK((converted.hints == ConverterHints{{{192, 1}}, {TrackHints{0, {}, {1}, {}}}}));
    SEQLIB_REQUIRE(converted.hints);
    XSFLIB_CHECK(LayOutConverted(song, *converted.hints, true) == song);
    XSFLIB_CHECK(!(LayOutConverted(song, {{{192, 1}}, {}}, true) == song));
}

// The opening song's two bars before its start are blocks at tick 0 that call one subroutine. The hints give each block
// 8 settings. The layout without subroutines holds no call, and the layout with them gives the song back.
void TestOpening()
{
    const Sequence song = OpeningSong();
    const ConvertedSong converted = ReadConverted(song);
    XSFLIB_CHECK((converted.hints == ConverterHints{{{192, 1}}, {TrackHints{0, {8, 8}, {}, {}}}}));
    SEQLIB_REQUIRE(converted.hints);
    const Sequence unfolded = LayOutConverted(song, *converted.hints, false);
    XSFLIB_CHECK(!HasCall(unfolded) && LayOutConverted(unfolded, *converted.hints, true) == song);
}

// A song whose track 1 comes before track 0 in the file. The header starts track 1 and jumps to track 0. Each track
// contains the track of the opening song: two bars before its start call one subroutine of 8 settings.
Sequence SwappedSong()
{
    std::vector<Line> lines = {{{"SMF_S_Begin"}, Of(Kind::kAllocateTracks, 3)},
                               {{"SMF_S_Start"}, OpenTrack(1), "SMF_S_Track_1"},
                               {{}, Of(Kind::kJump), "SMF_S_Track_0"}};
    for (const std::string track : {"SMF_S_Track_1", "SMF_S_Track_0"})
    {
        Append(lines, {{{track}, Of(Kind::kNoteWait, 0)},
                       {{}, Of(Kind::kCall), track + "_0"},
                       {{}, Of(Kind::kCall), track + "_0"},
                       {{}, Note(60, 192)},
                       {{}, Of(Kind::kWait, 192)},
                       {{}, Note(60, 192)},
                       {{}, Of(Kind::kWait, 192)},
                       {{}, Of(Kind::kEnd)},
                       {{track + "_0"}, Of(Kind::kVolume, 100)},
                       {{}, Of(Kind::kPan, 64)}});
        for (int i = 0; i < 3; ++i)
        {
            lines.push_back({{}, Of(Kind::kVolume, 100)});
            lines.push_back({{}, Of(Kind::kPan, 64)});
        }
        lines.push_back({{}, Of(Kind::kReturn)});
    }
    lines.push_back({{"SMF_S_End"}, Of(Kind::kEnd)});

    return Build(lines);
}

// The hints of a song whose track 1 comes first in the file list the tracks in the order of their numbers, as a saved
// file has to.
void TestSwappedTracks()
{
    const Sequence song = SwappedSong();
    const ConvertedSong converted = ReadConverted(song);
    XSFLIB_CHECK((converted.hints ==
                  ConverterHints{{{192, 1}}, {TrackHints{0, {8, 8}, {}, {}}, TrackHints{1, {8, 8}, {}, {}}}}));
    SEQLIB_REQUIRE(converted.hints);
    XSFLIB_CHECK(LayOutConverted(song, *converted.hints, true) == song);
}

// An empty song's labels take the name with an underscore for its space. The song reads as a converted song.
void TestEmptySong()
{
    const Sequence song = hokuto::edit::EmptySong("My Song");

    const auto named = [](const Label& label)
    {
        return label.name.starts_with("SMF_My_Song_");
    };
    XSFLIB_CHECK(song.labels.size() == 4 && std::ranges::all_of(song.labels, named));
    XSFLIB_CHECK(ReadConverted(song).hints.has_value());
}

// A sequence without a converted song gives no hints, and neither does a track whose notes wait for their length. Each
// gives its problem. The layout of a sequence without a converted song is refused.
// A song whose End label follows its last command produces no hints either.
void TestRefusals()
{
    // A sequence without a converted song, and a track whose notes wait for their length.
    Sequence other = MainSong();
    other.labels.erase(other.labels.begin());
    const ConvertedSong without = ReadConverted(other);
    XSFLIB_CHECK(!without.hints && without.problem == "no converted song");

    Sequence waiting = MainSong();
    waiting.items[2] = Of(Kind::kVolume, 100);
    const ConvertedSong notes_wait = ReadConverted(waiting);
    XSFLIB_CHECK(!notes_wait.hints && notes_wait.problem == "a note that waits for its length");

    // An End label after the last command names no end.
    Sequence endless = MainSong();
    endless.items.pop_back();
    const ConvertedSong no_end = ReadConverted(endless);
    XSFLIB_CHECK(!no_end.hints && no_end.problem == "no End label");

    XSFLIB_CHECK(!ErrorOf<std::invalid_argument>([&] { LayOutConverted(other, {}, true); }).empty());
}

// The settled layout of track 0 without its note at tick 48 joins the rests around the note. Track 1's split rest
// stays, and so do the hints. A layout without an edit settles as it is. Each item of the edit leads to its place in
// the settled layout, and the two rests lead to one. A call in a timeline is refused.
void TestSettled()
{
    // Without the note at tick 48, track 0's rests around it join. Track 1's split rest stays.
    const ConverterHints kHints{{{192, 1}}, {}};
    const Sequence unfolded = LayOutConverted(MainSong(), kHints, false);
    const Sequence edited = Without(unfolded, 7);
    ConverterHints settled_hints = kHints;
    const Sequence settled = SettledLayout(unfolded, edited, settled_hints);
    SEQLIB_REQUIRE(settled.items.size() == 66);
    XSFLIB_CHECK(WaitAt(settled, 6) == 96 && settled.items[7].key == 64);
    XSFLIB_CHECK(WaitAt(settled, 60) == 48 && WaitAt(settled, 61) == 48 && settled_hints == kHints);
    XSFLIB_CHECK(SettledLayout(unfolded, unfolded, settled_hints) == unfolded);

    // Each item of the edit leads to its place in the settled layout. The two rests lead to one.
    const std::vector<std::optional<uint32_t>> items = ItemsAfterJoins(edited, settled);
    SEQLIB_REQUIRE(items.size() == 68);
    XSFLIB_CHECK(items[5] == 5u && items[6] == 6u && items[7] == 6u && items[8] == 7u);
    XSFLIB_CHECK(items[61] == 60u && items[62] == 61u && items.back() == 66u);

    // A call is no part of a timeline.
    Sequence called = unfolded;
    called.items[4] = Of(Kind::kCall);
    called.items[4].target = 30;
    XSFLIB_CHECK(!ErrorOf<std::invalid_argument>([&] { SettledLayout(unfolded, called, settled_hints); }).empty());
}

// The header and the labels that both layouts of the main song hold match. A call and a subroutine's label don't. A
// track's first settings go after its note wait, and after the blocks before the song's start. The player runs track 0
// from the sound's start through the header, and track 1 from its label.
void TestMatching()
{
    // The header and the labels that both layouts hold match. A call and a subroutine's label don't.
    const Sequence song = MainSong();
    const ConverterHints kHints{{{192, 1}}, {}};
    const Sequence unfolded = LayOutConverted(song, kHints, false);
    const std::vector<std::optional<uint32_t>> items = MatchingItems(song, unfolded);
    SEQLIB_REQUIRE(items.size() == song.items.size() + 1 && items.size() > 17);
    XSFLIB_CHECK(items[0] == 0u && items[1] == 1u && items[2] == 2u);
    XSFLIB_CHECK(items[5] == 23u && !items[3] && !items[17] && items.back() == 68u);

    // A track's first settings go after its note wait, and after the blocks before the song's start.
    XSFLIB_CHECK(TrackOpening(unfolded, kHints, 0) == 3u && TrackOpening(unfolded, kHints, 1) == 45u);
    XSFLIB_CHECK(!TrackOpening(unfolded, kHints, 2));
    const ConverterHints kOpeningHints{{{192, 1}}, {TrackHints{0, {8, 8}, {}, {}}}};
    XSFLIB_CHECK(TrackOpening(LayOutConverted(OpeningSong(), kOpeningHints, false), kOpeningHints, 0) == 18u);

    // The player runs track 0 from the sound's start through the header, and track 1 from its label.
    const auto track_1 = std::find_if(unfolded.labels.begin(), unfolded.labels.end(),
                                      [](const Label& label) { return label.name == "SMF_T_Track_1"; });
    SEQLIB_REQUIRE(track_1 != unfolded.labels.end());
    XSFLIB_CHECK(TrackRunFrom(unfolded, 1) == 0 && TrackRunFrom(unfolded, track_1->item) == 1);
    XSFLIB_CHECK(!TrackRunFrom(unfolded, track_1->item + 1));
}

// A song whose header opens track 13 as track 1. Track 0 ends in the header, or with `jump`, jumps to its track.
Sequence RenumberedSong(bool jump)
{
    const std::vector<Line> lines = {{{"SMF_R_Begin"}, Of(Kind::kAllocateTracks, 3)},
                                     {{"SMF_R_Start"}, OpenTrack(1), "SMF_R_Track_13"},
                                     jump ? Line{{}, Of(Kind::kJump), "SMF_R_Track_0"} : Line{{}, Of(Kind::kEnd)},
                                     {{"SMF_R_Track_0"}, Of(Kind::kNoteWait, 0)},
                                     {{}, Note(60, 96)},
                                     {{}, Of(Kind::kWait, 96)},
                                     {{}, Of(Kind::kEnd)},
                                     {{"SMF_R_Track_13"}, Of(Kind::kNoteWait, 0)},
                                     {{}, Note(48, 96)},
                                     {{}, Of(Kind::kWait, 96)},
                                     {{}, Of(Kind::kEnd)},
                                     {{"SMF_R_End"}, Of(Kind::kEnd)}};
    return Build(lines);
}

// In the renumbered song, the player runs track 13 from its label, and a new setting of track 1 goes after that track's
// note wait. The header runs as track 0 only when it jumps to track 0's commands.
void TestRenumbered()
{
    // A new setting of track 1 goes after the note wait of the song's track 13.
    const Sequence song = RenumberedSong(false);
    XSFLIB_CHECK(TrackRunFrom(song, 7) == 13 && TrackOpening(song, {}, 13) == 8u);
    XSFLIB_CHECK(!TrackRunFrom(song, 1) && !TrackRunFrom(song, 0));
    XSFLIB_CHECK(TrackRunFrom(RenumberedSong(true), 1) == 0);
}

// The main song loops from tick 384 to 768, with subroutines or without. The opening song has no loop. An earlier loop
// ends the bar after the jump with an end at the track's old end. Even where the hints call a bar, a loop's label
// inside the bar keeps it in its track. A later end lets each track's last rest run on to its jump. A loop that doesn't
// end after its start, a song without a loop and a loop that holds no note are refused.
void TestLoop()
{
    const ConverterHints kHints{{{192, 1}}, {}};
    const Sequence unfolded = LayOutConverted(MainSong(), kHints, false);
    XSFLIB_CHECK((LoopOfConverted(unfolded) == ConvertedLoop{384, 768}));
    XSFLIB_CHECK((LoopOfConverted(MainSong()) == ConvertedLoop{384, 768}));
    XSFLIB_CHECK(!LoopOfConverted(OpeningSong()));

    // An earlier loop: the bar after the jump ends with an end at the track's old end.
    const Sequence earlier = WithLoop(unfolded, kHints, {192, 576});
    XSFLIB_CHECK((LoopOfConverted(earlier) == ConvertedLoop{192, 576}));
    XSFLIB_CHECK(earlier.items.size() == 70 && earlier.items[33].kind == Kind::kJump &&
                 earlier.items[44].kind == Kind::kEnd);
    XSFLIB_CHECK((LoopOfConverted(LayOutConverted(earlier, kHints, true)) == ConvertedLoop{192, 576}));

    // Even where the hints call a bar, a loop's label inside the bar keeps it in its track.
    const Sequence inside = WithLoop(unfolded, kHints, {288, 576});
    const ConverterHints kCalledHints{{{192, 1}}, {TrackHints{0, {}, {}, {1}}}};
    XSFLIB_CHECK((LoopOfConverted(LayOutConverted(inside, kHints, true)) == ConvertedLoop{288, 576}));
    XSFLIB_CHECK((LoopOfConverted(LayOutConverted(inside, kCalledHints, true)) == ConvertedLoop{288, 576}));

    // A later end: each track's last rest runs on to its jump.
    const Sequence later = WithLoop(unfolded, kHints, {384, 960});
    XSFLIB_CHECK((LoopOfConverted(later) == ConvertedLoop{384, 960}));
    XSFLIB_CHECK(later.items.size() == 68 && WaitAt(later, 42) == 240 && later.items[43].kind == Kind::kJump);
    XSFLIB_CHECK(WithLoop(earlier, kHints, {384, 768}) == unfolded);

    // A loop that doesn't end after its start can't move, and neither can a song without a loop. A loop from tick 400
    // to 420 holds no note: the notes there start at ticks 384 and 432.
    int refused = 0;
    for (const auto& [song, loop] :
         {std::pair(unfolded, ConvertedLoop{384, 384}), std::pair(OpeningSong(), ConvertedLoop{0, 192}),
          std::pair(unfolded, ConvertedLoop{400, 420})})
    {
        refused += ErrorOf<std::invalid_argument>([&] { WithLoop(song, kHints, loop); }).empty() ? 0 : 1;
    }
    XSFLIB_CHECK(refused == 3);
}

// A song whose track loops from tick 0 to tick 384, jumps back to the loop's start twice at the loop's end, then ends.
// A song of the Wii's converter does that.
Sequence TwoJumpsSong()
{
    const std::vector<Line> lines = {{{"SMF_J_Begin"}, Of(Kind::kAllocateTracks, 1)},
                                     {{"SMF_J_Start", "SMF_J_Track_0"}, Of(Kind::kNoteWait, 0)},
                                     {{"SMF_J_Track_0_LoopStart"}, Note(60, 192)},
                                     {{}, Of(Kind::kWait, 192)},
                                     {{}, Note(62, 192)},
                                     {{}, Of(Kind::kWait, 192)},
                                     {{}, Of(Kind::kJump), "SMF_J_Track_0_LoopStart"},
                                     {{}, Of(Kind::kJump), "SMF_J_Track_0_LoopStart"},
                                     {{}, Of(Kind::kEnd)},
                                     {{"SMF_J_End"}, Of(Kind::kEnd)}};
    return Build(lines);
}

// The waited song with track 2 as Paste Track opens a pasted track: where the sound starts, before the header's wait of
// 17 ticks. Track 2 waits 17 ticks, then loops over a bar as the other tracks do. With `elsewhere`, it loops over its
// first half bar instead.
Sequence PastedSong(bool elsewhere)
{
    std::vector<Line> lines = {{{"SMF_W_Begin"}, Of(Kind::kAllocateTracks, 7)},
                               {{"SMF_W_Start"}, OpenTrack(2), "SMF_W_Track_2"},
                               {{}, Of(Kind::kWait, 17)},
                               {{}, OpenTrack(1), "SMF_W_Track_1"},
                               {{}, Of(Kind::kJump), "SMF_W_Track_0"},
                               {{"SMF_W_Track_0"}, Of(Kind::kNoteWait, 0)},
                               {{"SMF_W_Track_0_LoopStart"}, Note(60, 96)},
                               {{}, Of(Kind::kWait, 96)},
                               {{}, Note(64, 96)},
                               {{}, Of(Kind::kWait, 96)},
                               {{}, Of(Kind::kJump), "SMF_W_Track_0_LoopStart"},
                               {{"SMF_W_Track_1"}, Of(Kind::kNoteWait, 0)},
                               {{"SMF_W_Track_1_LoopStart"}, Note(48, 192)},
                               {{}, Of(Kind::kWait, 192)},
                               {{}, Of(Kind::kJump), "SMF_W_Track_1_LoopStart"},
                               {{"SMF_W_Track_2"}, Of(Kind::kNoteWait, 0)}};
    if (elsewhere)
    {
        lines.push_back({{"SMF_W_Track_2_LoopStart"}, Note(55, 96)});
        lines.push_back({{}, Of(Kind::kWait, 96)});
    }
    else
    {
        lines.push_back({{}, Of(Kind::kWait, 17)});
        lines.push_back({{"SMF_W_Track_2_LoopStart"}, Note(55, 192)});
        lines.push_back({{}, Of(Kind::kWait, 192)});
    }
    lines.push_back({{}, Of(Kind::kJump), "SMF_W_Track_2_LoopStart"});
    lines.push_back({{"SMF_W_End"}, Of(Kind::kEnd)});

    return Build(lines);
}

// The header starts the song's tracks after its waits: at the first command that opens one, at a jump to a track, or
// where it runs into a track.
void TestSongStart()
{
    XSFLIB_CHECK((SongStartFrom(WaitedSong(), 0) == ConvertedStart{2, 17}));
    XSFLIB_CHECK((SongStartFrom(WaitedSong(), 1) == ConvertedStart{2, 17}));
    XSFLIB_CHECK((SongStartFrom(MainSong(), 0) == ConvertedStart{1, 0}));
    XSFLIB_CHECK((SongStartFrom(RenumberedSong(false), 0) == ConvertedStart{1, 0}));

    // A header that jumps to its first track starts it at the jump. One that ends before it starts a track starts none.
    Sequence jumping = RenumberedSong(true);
    jumping.items[1] = Of(Kind::kWait, 5);
    XSFLIB_CHECK((SongStartFrom(jumping, 0) == ConvertedStart{2, 5}));
    Sequence ended = RenumberedSong(false);
    ended.items[1] = Of(Kind::kWait, 5);
    XSFLIB_CHECK(!SongStartFrom(ended, 0));

    // A sound that starts at a later track's label runs no header. A song without a header starts at its first track.
    const Sequence main = MainSong();
    const auto track_1 = std::ranges::find(main.labels, std::string("SMF_T_Track_1"), &Label::name);
    XSFLIB_CHECK(track_1 != main.labels.end() && track_1->item == 28 && !SongStartFrom(main, track_1->item));
    XSFLIB_CHECK((SongStartFrom(TwoJumpsSong(), 1) == ConvertedStart{1, 0}));

    // A wait whose time a variable decides leaves the start unknown.
    Sequence variable = WaitedSong();
    variable.items[1].value.source = ntdlib::commands::Source::kVariable;
    XSFLIB_CHECK(!SongStartFrom(variable, 0));
    XSFLIB_CHECK(!TrackStartsFrom(variable, 0));
}

// Each track's ticks count from where the header starts it. A track opened before the header's wait loops with the
// other tracks when it waits as long first. The converter's loop counts each track's ticks from the track's start, and
// finds none there. The song's loops count them from the song's first command.
void TestTrackStarts()
{
    using Starts = std::map<std::string, uint32_t>;
    XSFLIB_CHECK((TrackStartsFrom(WaitedSong(), 0) == Starts{{"SMF_W_Track_0", 17}, {"SMF_W_Track_1", 17}}));
    XSFLIB_CHECK((TrackStartsFrom(MainSong(), 0) == Starts{{"SMF_T_Track_0", 0}, {"SMF_T_Track_1", 0}}));
    XSFLIB_CHECK((SongLoopsOf(WaitedSong()) == std::vector<ConvertedLoop>{{17, 209}, {17, 209}}));
    XSFLIB_CHECK((SongLoopsOf(OpeningSong()) == std::vector<ConvertedLoop>{}));

    const Sequence pasted = PastedSong(false);
    XSFLIB_CHECK((SongStartFrom(pasted, 0) == ConvertedStart{1, 0}));
    XSFLIB_CHECK(
        (TrackStartsFrom(pasted, 0) == Starts{{"SMF_W_Track_0", 17}, {"SMF_W_Track_1", 17}, {"SMF_W_Track_2", 0}}));
    XSFLIB_CHECK(!LoopOfConverted(pasted));
    XSFLIB_CHECK((SongLoopsOf(pasted) == std::vector<ConvertedLoop>{{17, 209}, {17, 209}, {17, 209}}));
    XSFLIB_CHECK((SongLoopsOf(PastedSong(true)) == std::vector<ConvertedLoop>{{17, 209}, {17, 209}, {0, 96}}));

    // A wait whose time a variable decides leaves the looping tracks' ticks unknown.
    Sequence variable = pasted;
    variable.items[2].value.source = ntdlib::commands::Source::kVariable;
    XSFLIB_CHECK(!TrackStartsFrom(variable, 0) && !SongLoopsOf(variable));
}

// The two jumps' song reads as a converted song that loops from tick 0 to 384, and the layout gives the song back. A
// later end of the loop moves both jumps, and the last rest runs on to them. An earlier end ends the bar after the
// jumps with an end.
void TestTwoJumps()
{
    const Sequence song = TwoJumpsSong();
    const ConvertedSong converted = ReadConverted(song);
    SEQLIB_REQUIRE(converted.hints);
    XSFLIB_CHECK((LoopOfConverted(song) == ConvertedLoop{0, 384}));
    XSFLIB_CHECK(LayOutConverted(song, *converted.hints, true) == song);

    // Both jumps move with the loop's end. The last rest runs on to them.
    const Sequence later = WithLoop(song, *converted.hints, {0, 576});
    XSFLIB_CHECK((LoopOfConverted(later) == ConvertedLoop{0, 576}));
    XSFLIB_CHECK(later.items.size() == 9 && WaitAt(later, 5) == 384 && later.items[6].kind == Kind::kJump &&
                 later.items[7].kind == Kind::kJump);

    // An earlier end: the bar after the jumps ends with an end.
    const Sequence earlier = WithLoop(song, *converted.hints, {0, 192});
    XSFLIB_CHECK((LoopOfConverted(earlier) == ConvertedLoop{0, 192}));
    XSFLIB_CHECK(earlier.items.size() == 10 && earlier.items[4].kind == Kind::kJump &&
                 earlier.items[5].kind == Kind::kJump && earlier.items[6].kind == Kind::kNote &&
                 earlier.items[8].kind == Kind::kEnd);
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("converter",
                                    []
                                    {
                                        TestRoundTrip();
                                        TestExceptions();
                                        TestOpening();
                                        TestSwappedTracks();
                                        TestEmptySong();
                                        TestRefusals();
                                        TestSettled();
                                        TestMatching();
                                        TestRenumbered();
                                        TestSongStart();
                                        TestTrackStarts();
                                        TestLoop();
                                        TestTwoJumps();
                                    });
}
