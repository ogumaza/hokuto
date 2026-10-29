// SPDX-License-Identifier: MIT

// Tests the layout of the songs that NintendoWare's MIDI converter wrote (src/edit/converter.h), and the timelines of a
// file (src/edit/timelines.h). The tests write small songs as the converter writes them, at NintendoWare's default
// timebase of 48 ticks a quarter note. Each bar of 4/4 lasts 192 ticks. The main song's track 0 repeats a bar of 10
// commands. The converter calls three copies of it. The fourth copy holds the loop's start and stays in the track.
// Track 1 repeats a bar of 5 commands, too few to call. One of its rests is split in two with nothing between them.
// Both tracks loop from tick 384 to tick 768. The replacement's test puts the song in an archive of the Switch. The
// waited song's header waits 17 ticks before it starts its two tracks. A header written by hand can do that. Each track
// then loops over a bar from its start: the song's loop runs from tick 17 to tick 209.

#include "edit/converter.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/commands/units.h"
#include "ntdlib/nwsnd/commands.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/testing/sar_files.h"
#include "xsflib/testing/check.h"

#include "edit/notes.h"
#include "edit/replace.h"
#include "edit/timelines.h"
#include "edit/tracks.h"
#include "format/archive.h"
#include "format/file.h"
#include "song/archive.h"

using hokuto::edit::ConvertedLoop;
using hokuto::edit::ConvertedSong;
using hokuto::edit::ConvertedStart;
using hokuto::edit::ConverterHints;
using hokuto::edit::Edited;
using hokuto::edit::ItemsAfterJoins;
using hokuto::edit::LayOutConverted;
using hokuto::edit::LoopOfConverted;
using hokuto::edit::MatchingItems;
using hokuto::edit::ReadConverted;
using hokuto::edit::Settled;
using hokuto::edit::SettledLayout;
using hokuto::edit::SongLoopsOf;
using hokuto::edit::SongStartFrom;
using hokuto::edit::SoundPlace;
using hokuto::edit::TrackHints;
using hokuto::edit::TrackOpening;
using hokuto::edit::TrackRunFrom;
using hokuto::edit::TrackStartsFrom;
using hokuto::edit::WithLoop;
using hokuto::format::File;
using ntdlib::commands::Command;
using ntdlib::commands::Kind;
using ntdlib::commands::kOne;
using ntdlib::commands::Label;
using ntdlib::commands::Sequence;

namespace
{

// A line of a song: the labels at its command, the command, and the label of the command's target.
struct Line
{
    std::vector<std::string> labels;
    Command command;
    std::string target = {};
};

// A command of `kind` with the value `value`.
Command Of(Kind kind, int64_t value = 0)
{
    Command command;
    command.kind = kind;
    command.value.value = value * kOne;

    return command;
}

// A note of key `key` for `length` ticks, at velocity 100.
Command Note(int key, int64_t length)
{
    Command note = Of(Kind::kNote, length);
    note.key = static_cast<uint8_t>(key);
    note.velocity = 100 * kOne;

    return note;
}

// A command that opens track `track`.
Command OpenTrack(int track)
{
    Command open = Of(Kind::kOpenTrack);
    open.index = static_cast<uint8_t>(track);

    return open;
}

// The sequence that `lines` make, with the labels in the order of their names. The converter writes them in that order.
Sequence Build(const std::vector<Line>& lines)
{
    Sequence sequence;
    for (const Line& line : lines)
    {
        for (const std::string& name : line.labels)
        {
            sequence.labels.push_back({name, static_cast<uint32_t>(sequence.items.size())});
        }
        sequence.items.push_back(line.command);
    }
    for (std::size_t i = 0; i < lines.size(); ++i)
    {
        if (!lines[i].target.empty())
        {
            const auto label = std::find_if(sequence.labels.begin(), sequence.labels.end(),
                                            [&](const Label& l) { return l.name == lines[i].target; });
            sequence.items[i].target = label->item;
        }
    }
    std::stable_sort(sequence.labels.begin(), sequence.labels.end(),
                     [](const Label& a, const Label& b) { return a.name < b.name; });

    return sequence;
}

// The lines of a bar of track 0: two settings and four notes, 10 commands.
std::vector<Line> Bar0(const std::vector<std::string>& labels = {})
{
    return {{labels, Of(Kind::kVolume, 100)},
            {{}, Of(Kind::kPan, 64)},
            {{}, Note(60, 48)},
            {{}, Of(Kind::kWait, 48)},
            {{}, Note(62, 48)},
            {{}, Of(Kind::kWait, 48)},
            {{}, Note(64, 48)},
            {{}, Of(Kind::kWait, 48)},
            {{}, Note(65, 48)},
            {{}, Of(Kind::kWait, 48)}};
}

// The lines of a bar of track 1: a setting and two notes, 5 commands. With `split`, the rest after the first note is
// split in two.
std::vector<Line> Bar1(const std::vector<std::string>& labels = {}, bool split = false)
{
    std::vector<Line> bar = {{labels, Of(Kind::kPan, 64)},
                             {{}, Note(48, 96)},
                             {{}, Of(Kind::kWait, 96)},
                             {{}, Note(50, 96)},
                             {{}, Of(Kind::kWait, 96)}};
    if (split)
    {
        bar[2].command = Of(Kind::kWait, 48);
        bar.insert(bar.begin() + 3, Line{{}, Of(Kind::kWait, 48)});
    }

    return bar;
}

// Appends the lines `more` to `lines`.
void Append(std::vector<Line>& lines, const std::vector<Line>& more)
{
    lines.insert(lines.end(), more.begin(), more.end());
}

// The main song. With `inline_bar`, track 0 writes its second bar in the track instead of calling it.
Sequence MainSong(bool inline_bar = false)
{
    std::vector<Line> lines = {{{"SMF_T_Begin"}, Of(Kind::kAllocateTracks, 3)},
                               {{"SMF_T_Start"}, OpenTrack(1), "SMF_T_Track_1"},
                               {{"SMF_T_Track_0"}, Of(Kind::kNoteWait, 0)},
                               {{}, Of(Kind::kCall), "SMF_T_Track_0_0"}};
    if (inline_bar)
    {
        Append(lines, Bar0());
    }
    else
    {
        lines.push_back({{}, Of(Kind::kCall), "SMF_T_Track_0_0"});
    }
    Append(lines, Bar0({"SMF_T_Track_0_LoopStart"}));
    lines.push_back({{}, Of(Kind::kCall), "SMF_T_Track_0_0"});
    lines.push_back({{}, Of(Kind::kJump), "SMF_T_Track_0_LoopStart"});
    Append(lines, Bar0({"SMF_T_Track_0_0"}));
    lines.push_back({{}, Of(Kind::kReturn)});

    lines.push_back({{"SMF_T_Track_1"}, Of(Kind::kNoteWait, 0)});
    Append(lines, Bar1());
    Append(lines, Bar1());
    Append(lines, Bar1({"SMF_T_Track_1_LoopStart"}));
    Append(lines, Bar1({}, true));
    lines.push_back({{}, Of(Kind::kJump), "SMF_T_Track_1_LoopStart"});
    lines.push_back({{"SMF_T_End"}, Of(Kind::kEnd)});

    return Build(lines);
}

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
    XSFLIB_CHECK(converted.hints && converted.problem.empty());
    if (!converted.hints)
    {
        return;
    }

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
    if (converted.hints)
    {
        XSFLIB_CHECK(LayOutConverted(song, *converted.hints, true) == song);
        XSFLIB_CHECK(!(LayOutConverted(song, {{{192, 1}}, {}}, true) == song));
    }
}

// A song whose two bars before its start hold the same 8 settings. Each is a block at tick 0, and both call one
// subroutine. The bars after them hold too few commands to call.
Sequence OpeningSong()
{
    std::vector<Line> lines = {{{"SMF_O_Begin"}, Of(Kind::kAllocateTracks, 1)},
                               {{"SMF_O_Start", "SMF_O_Track_0"}, Of(Kind::kNoteWait, 0)},
                               {{}, Of(Kind::kCall), "SMF_O_Track_0_0"},
                               {{}, Of(Kind::kCall), "SMF_O_Track_0_0"},
                               {{}, Note(60, 192)},
                               {{}, Of(Kind::kWait, 192)},
                               {{}, Note(60, 192)},
                               {{}, Of(Kind::kWait, 192)},
                               {{}, Of(Kind::kEnd)},
                               {{"SMF_O_Track_0_0"}, Of(Kind::kVolume, 100)},
                               {{}, Of(Kind::kPan, 64)}};
    for (int i = 0; i < 3; ++i)
    {
        lines.push_back({{}, Of(Kind::kVolume, 100)});
        lines.push_back({{}, Of(Kind::kPan, 64)});
    }
    lines.push_back({{}, Of(Kind::kReturn)});
    lines.push_back({{"SMF_O_End"}, Of(Kind::kEnd)});

    return Build(lines);
}

// The opening song's two bars before its start are blocks at tick 0 that call one subroutine. The hints give each block
// 8 settings. The layout without subroutines holds no call, and the layout with them gives the song back.
void TestOpening()
{
    const Sequence song = OpeningSong();
    const ConvertedSong converted = ReadConverted(song);
    XSFLIB_CHECK((converted.hints == ConverterHints{{{192, 1}}, {TrackHints{0, {8, 8}, {}, {}}}}));
    if (converted.hints)
    {
        const Sequence unfolded = LayOutConverted(song, *converted.hints, false);
        XSFLIB_CHECK(!HasCall(unfolded) && LayOutConverted(unfolded, *converted.hints, true) == song);
    }
}

// A sequence without a converted song gives no hints, and neither does a track whose notes wait for their length. Each
// gives its problem. The layout of a sequence without a converted song is refused.
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

    bool refused = false;
    try
    {
        LayOutConverted(other, {}, true);
    }
    catch (const std::invalid_argument&)
    {
        refused = true;
    }
    XSFLIB_CHECK(refused);
}

// `sequence` without item `item`. The labels and the targets after it move back.
Sequence Without(Sequence sequence, uint32_t item)
{
    sequence.items.erase(sequence.items.begin() + item);
    for (Command& command : sequence.items)
    {
        command.target -= command.target > item ? 1 : 0;
    }
    for (Label& label : sequence.labels)
    {
        label.item -= label.item > item ? 1 : 0;
    }

    return sequence;
}

// The ticks of the wait at `item`, or -1 for another command.
int64_t WaitAt(const Sequence& sequence, std::size_t item)
{
    const Command& command = sequence.items.at(item);
    return command.kind == Kind::kWait ? command.value.value / kOne : -1;
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
    XSFLIB_CHECK(settled.items.size() == 66 && WaitAt(settled, 6) == 96 && settled.items[7].key == 64);
    XSFLIB_CHECK(WaitAt(settled, 60) == 48 && WaitAt(settled, 61) == 48 && settled_hints == kHints);
    XSFLIB_CHECK(SettledLayout(unfolded, unfolded, settled_hints) == unfolded);

    // Each item of the edit leads to its place in the settled layout. The two rests lead to one.
    const std::vector<std::optional<uint32_t>> items = ItemsAfterJoins(edited, settled);
    XSFLIB_CHECK(items.size() == 68 && items[5] == 5u && items[6] == 6u && items[7] == 6u && items[8] == 7u);
    XSFLIB_CHECK(items[61] == 60u && items[62] == 61u && items.back() == 66u);

    // A call is no part of a timeline.
    Sequence called = unfolded;
    called.items[4] = Of(Kind::kCall);
    called.items[4].target = 30;
    bool refused = false;
    try
    {
        SettledLayout(unfolded, called, settled_hints);
    }
    catch (const std::invalid_argument&)
    {
        refused = true;
    }
    XSFLIB_CHECK(refused);
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
    XSFLIB_CHECK(items.size() == song.items.size() + 1 && items[0] == 0u && items[1] == 1u && items[2] == 2u);
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

// A song whose header waits 17 ticks, then starts track 1 and jumps to track 0. Each track loops over a bar.
Sequence WaitedSong()
{
    const std::vector<Line> lines = {{{"SMF_W_Begin"}, Of(Kind::kAllocateTracks, 3)},
                                     {{"SMF_W_Start"}, Of(Kind::kWait, 17)},
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
                                     {{"SMF_W_End"}, Of(Kind::kEnd)}};
    return Build(lines);
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
        try
        {
            WithLoop(song, kHints, loop);
        }
        catch (const std::invalid_argument&)
        {
            ++refused;
        }
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
    XSFLIB_CHECK(converted.hints && (LoopOfConverted(song) == ConvertedLoop{0, 384}));
    if (!converted.hints)
    {
        return;
    }
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

// A file that doesn't say makes the converted song a timeline without subroutines, and the other sequence a program. A
// save holds the song with its subroutines. A timeline whose hints don't give the song back opens as a program. The
// switch between the modes gives each form back, and a sequence that isn't a converted song is refused as a timeline.
// An edit of a timeline settles, and one of a program stays as the edit left it. A moved loop keeps each note's place
// among the notes, and stays where it moved after the settle.
void TestTimelines()
{
    // A file that doesn't say makes the converted song a timeline, and the other sequence a program.
    File file;
    file.sequences = hokuto::format::ShareSequences({MainSong(), Sequence{}});
    const ConverterHints kHints{{{192, 1}}, {}};
    const Sequence unfolded = LayOutConverted(MainSong(), kHints, false);
    const File opened = hokuto::edit::OpenTimelines(file);
    XSFLIB_CHECK(opened.timelines && opened.timelines->size() == 1 && opened.timelines->at(0) == kHints);
    XSFLIB_CHECK(*opened.sequences[0] == unfolded && opened.sequences[1]->items.empty());
    XSFLIB_CHECK(hokuto::edit::IsTimeline(opened, 0) && !hokuto::edit::IsTimeline(opened, 1));

    // A save holds the song with its subroutines. A timeline whose hints don't give the song back is a program.
    const File folded = hokuto::edit::FoldTimelines(opened);
    XSFLIB_CHECK(*folded.sequences[0] == MainSong() && folded.timelines == opened.timelines);
    File other = file;
    other.timelines.emplace();
    (*other.timelines)[0] = {{{144, 1}}, {}};
    const File program = hokuto::edit::OpenTimelines(other);
    XSFLIB_CHECK(program.timelines && program.timelines->empty() && *program.sequences[0] == MainSong());

    // The switch between the modes.
    const File as_program = hokuto::edit::AsProgram(opened, 0);
    XSFLIB_CHECK(*as_program.sequences[0] == MainSong() && !hokuto::edit::IsTimeline(as_program, 0));
    XSFLIB_CHECK(hokuto::edit::AsTimeline(as_program, 0) == opened);
    bool refused = false;
    try
    {
        hokuto::edit::AsTimeline(opened, 1);
    }
    catch (const std::invalid_argument& error)
    {
        refused = std::string(error.what()).starts_with("Hokuto edits this sequence as a program only.");
    }
    XSFLIB_CHECK(refused);

    // An edit of the timeline settles. One of a program stays as the edit left it.
    File edited = opened;
    edited.sequences[0] = Without(unfolded, 7);
    const Settled settled = hokuto::edit::SettleTimelines(opened, edited);
    XSFLIB_CHECK(settled.file.sequences[0]->items.size() == 66 && settled.items.at(0).at(7) == 6u);
    File programs = as_program;
    programs.sequences[0] = Without(MainSong(), 7);
    XSFLIB_CHECK(hokuto::edit::SettleTimelines(as_program, programs).items.empty());

    // The loop moves, and each note keeps its place among the notes.
    const Edited moved = hokuto::edit::MoveLoop(opened, 0, {192, 576});
    XSFLIB_CHECK((hokuto::edit::TimelineLoop(moved.file, 0) == ConvertedLoop{192, 576}));
    XSFLIB_CHECK(moved.items[5] == 5u && moved.items[35] == 36u && !moved.items[43]);

    // The program settles the moved loop as it settles every edit. The loop stays where it moved.
    for (const ConvertedLoop loop : {ConvertedLoop{192, 576}, ConvertedLoop{384, 960}, ConvertedLoop{432, 768}})
    {
        const Edited moved_loop = hokuto::edit::MoveLoop(opened, 0, loop);
        const Settled settled_loop = hokuto::edit::SettleTimelines(opened, moved_loop.file);
        XSFLIB_CHECK(hokuto::edit::TimelineLoop(settled_loop.file, 0) == loop);
        XSFLIB_CHECK(*settled_loop.file.sequences[0] == *moved_loop.file.sequences[0]);
    }
}

namespace sar_files = ntdlib::testing::sar_files;

// A sequence file of the Switch that holds `song`.
std::vector<uint8_t> SongFile(const Sequence& song)
{
    const ntdlib::nwsnd::Encoded encoded =
        ntdlib::nwsnd::EncodeSequence(song, hokuto::song::GrammarOf(ntdlib::sar::Flavor::kNx));
    std::vector<std::pair<std::string, uint32_t>> labels;
    for (const Label& label : song.labels)
    {
        labels.emplace_back(label.name, encoded.offsets.at(label.item));
    }

    return sar_files::Sequence(false, encoded.bytes, labels);
}

// Where each sound plays in `file`. The program finds the places in the archive that the file builds.
std::vector<std::optional<SoundPlace>> PlacesOf(const File& file)
{
    const std::vector<uint8_t> built = hokuto::format::ExportArchive(file);
    const auto archive = hokuto::song::LoadArchive(built, "test.bfsar");
    return hokuto::edit::SoundPlaces(*archive, hokuto::format::SequencesOfFiles(file, built, *archive),
                                     hokuto::format::StartFieldsOfSounds(file, built));
}

// The commands of `sequence` with its labels in the order of their names.
Sequence Sorted(Sequence sequence)
{
    std::stable_sort(sequence.labels.begin(), sequence.labels.end(),
                     [](const Label& a, const Label& b) { return a.name < b.name; });

    return sequence;
}

// A replacement of a sound's sequence in a timeline gives the program's commands and places. A saved file holds the
// same. SEQ_MAIN and SEQ_TWIN play the main song, from its start and from track 1's label. SEQ_ALONE plays a copy of it
// alone.
void TestReplaced()
{
    constexpr uint32_t kTwin = 1;
    constexpr uint32_t kAlone = 2;
    const Sequence song = MainSong();
    const ntdlib::nwsnd::Encoded encoded =
        ntdlib::nwsnd::EncodeSequence(song, hokuto::song::GrammarOf(ntdlib::sar::Flavor::kNx));
    const auto track_1 = std::find_if(song.labels.begin(), song.labels.end(),
                                      [](const Label& label) { return label.name == "SMF_T_Track_1"; });
    SEQLIB_REQUIRE(track_1 != song.labels.end());
    sar_files::SoundSpec main;
    main.name = "SEQ_MAIN";
    main.start_offset = 0;
    sar_files::SoundSpec twin = main;
    twin.name = "SEQ_TWIN";
    twin.start_offset = encoded.offsets.at(track_1->item);
    sar_files::SoundSpec alone = main;
    alone.name = "SEQ_ALONE";
    alone.file = 1;
    sar_files::ArchiveSpec spec;
    spec.sounds = {main, twin, alone};
    spec.players = {{"PLAYER_TEST", 1}};
    spec.files = {{SongFile(song), "", false}, {SongFile(song), "", false}};

    // The program opens both copies of the song as timelines.
    const File file = hokuto::format::ImportSwitchArchive(sar_files::Archive(false, spec));
    const File opened = hokuto::edit::OpenTimelines(file);
    XSFLIB_CHECK(hokuto::edit::IsTimeline(opened, 0) && hokuto::edit::IsTimeline(opened, 1));
    const std::vector<std::optional<SoundPlace>> places = PlacesOf(opened);
    const std::vector<std::optional<SoundPlace>> program_places = PlacesOf(file);

    // The song's file offers the sounds' starts at its items.
    const Sequence same = hokuto::edit::ReadIncoming(SongFile(song)).sequence;
    const Sequence other = hokuto::edit::ReadIncoming(SongFile(MainSong(true))).sequence;
    const std::vector<hokuto::edit::StartingSound> starts = hokuto::edit::SoundStarts(opened, places, same);
    SEQLIB_REQUIRE(starts.size() == 3);
    XSFLIB_CHECK(starts == hokuto::edit::SoundStarts(file, program_places, same));
    XSFLIB_CHECK(starts[1].item == track_1->item);

    // The file's commands from a sound's start change nothing.
    const auto replaced = [](const File& from, const std::vector<std::optional<SoundPlace>>& at, uint32_t sound,
                             const Sequence& incoming, uint32_t start)
    {
        return hokuto::edit::ReplaceSequence(from, at, sound, incoming, start, {}, "Other", 0);
    };
    for (const hokuto::edit::StartingSound& start : starts)
    {
        const File held = replaced(opened, places, start.sound, same, start.item);
        XSFLIB_CHECK(held.sequences == opened.sequences && held.timelines == opened.timelines);
        XSFLIB_CHECK(held.native->fields == opened.native->fields);
    }

    // Other commands replace SEQ_ALONE's whole. The sequence goes in as a program, and becomes a timeline again.
    const File whole = replaced(opened, places, kAlone, other, 0);
    XSFLIB_CHECK(!hokuto::edit::IsTimeline(whole, 1) && *whole.sequences[1] == other);
    const Settled settled = hokuto::edit::SettleTimelines(opened, whole, std::vector<uint32_t>{1});
    XSFLIB_CHECK(settled.replaced == std::vector<uint32_t>{1} && settled.items.empty());
    const File decided = hokuto::edit::WithNewModes(settled.file, settled.replaced);
    XSFLIB_CHECK(hokuto::edit::IsTimeline(decided, 1) && hokuto::edit::IsTimeline(decided, 0));
    const File program_whole = replaced(file, program_places, kAlone, other, 0);
    XSFLIB_CHECK(hokuto::format::ExportArchive(hokuto::edit::FoldTimelines(decided)) ==
                 hokuto::format::ExportArchive(program_whole));

    // SEQ_TWIN shares its file. The new commands go after the song's, and the timeline holds them. A saved file then
    // holds the program's commands and places, with the labels in the order of their names.
    const File shared = replaced(opened, places, kTwin, other, 0);
    const Settled settled_shared = hokuto::edit::SettleTimelines(opened, shared);
    XSFLIB_CHECK(settled_shared.replaced.empty() && hokuto::edit::IsTimeline(settled_shared.file, 0));
    const File saved = hokuto::edit::FoldTimelines(settled_shared.file);
    const File program_shared = replaced(file, program_places, kTwin, other, 0);
    XSFLIB_CHECK(*saved.sequences[0] == Sorted(*program_shared.sequences[0]));
    XSFLIB_CHECK(saved.native->fields == program_shared.native->fields);
    XSFLIB_CHECK(hokuto::edit::SettleTimelines(opened, shared, std::vector<uint32_t>{0}).replaced ==
                 std::vector<uint32_t>{0});
}

// An archive of the Switch whose sound SEQ_MAIN plays the main song, and SEQ_OPENING the opening song. Their bank's
// instrument plays every key. With `at_start`, the sounds start at the songs' Start labels, and SEQ_MAIN's entry
// allocates both tracks. Otherwise the sounds start at the songs' first commands. Those commands allocate the tracks.
std::vector<uint8_t> SongsArchive(bool at_start, const Sequence& main_song = MainSong(),
                                  const Sequence& opening_song = OpeningSong())
{
    sar_files::WaveSpec wave;
    wave.encoding = 1;
    wave.loop = true;
    wave.sample_rate = 48000;
    wave.loop_end = 64;
    wave.channels = {std::vector<uint8_t>(128, 0x10)};
    sar_files::Region region;
    region.original_key = 60;
    sar_files::VelocityTable velocities;
    velocities.entries = {{0, region}};
    sar_files::KeyTable keys;
    keys.entries = {{0, velocities}};
    sar_files::BankSpec bank;
    bank.waves = {{0x05000000, 0}};
    bank.instruments = {keys};

    // A sound starts at the Start label's offset.
    const auto start_of = [](const Sequence& song)
    {
        const ntdlib::nwsnd::Encoded encoded =
            ntdlib::nwsnd::EncodeSequence(song, hokuto::song::GrammarOf(ntdlib::sar::Flavor::kNx));
        const auto start = std::find_if(song.labels.begin(), song.labels.end(),
                                        [](const Label& label) { return label.name.ends_with("_Start"); });
        return encoded.offsets.at(start->item);
    };

    sar_files::SoundSpec main;
    main.name = "SEQ_MAIN";
    main.volume = 127;
    main.banks = {0};
    sar_files::SoundSpec opening = main;
    opening.name = "SEQ_OPENING";
    opening.file = 1;
    if (at_start)
    {
        main.start_offset = start_of(main_song);
        main.allocate_track_flags = 3;
        opening.start_offset = start_of(opening_song);
    }
    sar_files::ArchiveSpec spec;
    spec.sounds = {main, opening};
    spec.banks = {{"BANK_TEST", 2}};
    spec.wave_archives = {{"WARC_TEST", 3}};
    spec.players = {{"PLAYER_TEST", 1}};
    spec.files = {{SongFile(main_song), "", false},
                  {SongFile(opening_song), "", false},
                  {sar_files::Bank(false, bank), "", false},
                  {sar_files::WaveArchive(false, {sar_files::Wave(false, wave)}), "", false}};

    return sar_files::Archive(false, spec);
}

// What ntdlib's performer plays of sound `sound` of the archive that `file` builds. The program plays the timelines
// without subroutines, and a save holds them with subroutines.
hokuto::song::Sequence Read(const File& file, uint32_t sound)
{
    return hokuto::song::ReadSequence(*hokuto::song::LoadArchive(hokuto::format::ExportArchive(file), "test.bfsar"),
                                      sound);
}

// A timeline's notes move as in a MIDI file. SEQ_MAIN plays the main song and SEQ_OPENING the opening song. Track 0's
// last note of the intro, key 65 at tick 336, moves a beat later, to tick 384: the loop's start. It then plays in every
// pass of the loop, after key 60. A note can't start at or after the loop's end, at tick 768. The opening song has no
// loop. Its second note moves from tick 192 to 384, the track's old end. The track then lasts to the note's end.
void TestTimelineMoves()
{
    const File opened = hokuto::edit::OpenTimelines(hokuto::format::ImportSwitchArchive(SongsArchive(false)));
    XSFLIB_CHECK(hokuto::edit::IsTimeline(opened, 0) && hokuto::edit::IsTimeline(opened, 1));

    const auto starts = [](const hokuto::song::Sequence& sequence, int key)
    {
        std::vector<uint32_t> ticks;
        for (const hokuto::song::Note& note : sequence.tracks.at(0).notes)
        {
            if (note.key == key)
            {
                ticks.push_back(note.start);
            }
        }

        return ticks;
    };

    const auto edit_of = [](const hokuto::song::Sequence& sequence, uint32_t start, int32_t shift)
    {
        const hokuto::song::Track& track = sequence.tracks.at(0);
        const auto note = std::find_if(track.notes.begin(), track.notes.end(),
                                       [start](const hokuto::song::Note& n) { return n.start == start; });
        return hokuto::edit::NoteEdit{
            .command = note->command, .stretch = track.stretches.at(note->stretch), .shift = shift};
    };

    const hokuto::song::Sequence song = Read(opened, 0);
    const hokuto::edit::NoteEdit later = edit_of(song, 336, 48);
    const std::vector<hokuto::edit::NoteLimits> limits =
        hokuto::edit::LimitsOf(opened, 0, std::vector<hokuto::edit::NotePlace>{{later.command, later.stretch}});
    XSFLIB_CHECK((limits.at(0).shift == hokuto::edit::ShiftRange{336, 431}));
    const Settled settled =
        hokuto::edit::SettleTimelines(opened, hokuto::edit::EditNotes(opened, 0, std::vector{later}).file);
    const hokuto::song::Sequence moved = Read(hokuto::edit::FoldTimelines(settled.file), 0);
    XSFLIB_CHECK(moved.loop && moved.loop->start == 384 && moved.loop->end == 768);
    XSFLIB_CHECK((starts(moved, 65) == std::vector<uint32_t>{144, 384, 528, 720}));
    bool refused = false;
    try
    {
        hokuto::edit::EditNotes(opened, 0, std::vector{edit_of(song, 720, 48)});
    }
    catch (const std::invalid_argument& error)
    {
        refused = std::string(error.what()).starts_with("A note can't start at or after the loop's end.");
    }
    XSFLIB_CHECK(refused);

    const hokuto::song::Sequence alone = Read(opened, 1);
    const Settled longer = hokuto::edit::SettleTimelines(
        opened, hokuto::edit::EditNotes(opened, 1, std::vector{edit_of(alone, 192, 192)}).file);
    const hokuto::song::Sequence lasting = Read(hokuto::edit::FoldTimelines(longer.file), 1);
    XSFLIB_CHECK((starts(lasting, 60) == std::vector<uint32_t>{0, 384}));
    XSFLIB_CHECK(alone.notes_end == 384 && lasting.notes_end == 576 && lasting.length == alone.length + 192);
}

// A song whose track 0 loops over a bar of four notes from tick 0 to tick 384, and whose track 1 plays two notes and
// ends at tick 192.
Sequence EndingSong()
{
    const std::vector<Line> lines = {{{"SMF_E_Begin"}, Of(Kind::kAllocateTracks, 3)},
                                     {{"SMF_E_Start"}, OpenTrack(1), "SMF_E_Track_1"},
                                     {{"SMF_E_Track_0"}, Of(Kind::kNoteWait, 0)},
                                     {{"SMF_E_Track_0_LoopStart"}, Note(60, 96)},
                                     {{}, Of(Kind::kWait, 96)},
                                     {{}, Note(64, 96)},
                                     {{}, Of(Kind::kWait, 96)},
                                     {{}, Note(67, 96)},
                                     {{}, Of(Kind::kWait, 96)},
                                     {{}, Note(72, 96)},
                                     {{}, Of(Kind::kWait, 96)},
                                     {{}, Of(Kind::kJump), "SMF_E_Track_0_LoopStart"},
                                     {{"SMF_E_Track_1"}, Of(Kind::kNoteWait, 0)},
                                     {{}, Note(48, 96)},
                                     {{}, Of(Kind::kWait, 96)},
                                     {{}, Note(50, 96)},
                                     {{}, Of(Kind::kWait, 96)},
                                     {{}, Of(Kind::kEnd)},
                                     {{"SMF_E_End"}, Of(Kind::kEnd)}};
    return Build(lines);
}

// In a song that loops, a note of a track that ends moves past the track's end up to the tick before the loop's end, as
// on a track that loops. Track 1's last note of the ending song, at tick 96, moves at most 287 ticks later. At tick
// 383, the track lasts to the note's end, and the song keeps its loop.
void TestEndingTrackMoves()
{
    const File opened =
        hokuto::edit::OpenTimelines(hokuto::format::ImportSwitchArchive(SongsArchive(false, EndingSong())));
    XSFLIB_CHECK(hokuto::edit::IsTimeline(opened, 0));
    XSFLIB_CHECK((hokuto::edit::TimelineLoop(opened, 0) == ConvertedLoop{0, 384}));
    const hokuto::song::Sequence song = Read(opened, 0);
    const auto track = std::ranges::find(song.tracks, 1, &hokuto::song::Track::number);
    if (track == song.tracks.end() || track->notes.size() != 2)
    {
        XSFLIB_CHECK(false);
        return;
    }
    const hokuto::song::Note& last = track->notes.back();
    XSFLIB_CHECK(last.start == 96);

    const auto edit_of = [&](int32_t shift)
    {
        return hokuto::edit::NoteEdit{
            .command = last.command, .stretch = track->stretches.at(last.stretch), .shift = shift};
    };

    const std::vector<hokuto::edit::NoteLimits> limits = hokuto::edit::LimitsOf(
        opened, 0, std::vector<hokuto::edit::NotePlace>{{last.command, track->stretches.at(last.stretch)}});
    XSFLIB_CHECK(limits.size() == 1 && limits[0].shift && limits[0].shift->later == 287);

    bool refused = false;
    try
    {
        hokuto::edit::EditNotes(opened, 0, std::vector{edit_of(288)});
    }
    catch (const std::invalid_argument& error)
    {
        refused = std::string(error.what()) ==
                  "A note can't start at or after the loop's end. The song goes back to the loop's start there.";
    }
    XSFLIB_CHECK(refused);

    const File settled =
        hokuto::edit::SettleTimelines(opened, hokuto::edit::EditNotes(opened, 0, std::vector{edit_of(287)}).file).file;
    XSFLIB_CHECK((hokuto::edit::TimelineLoop(settled, 0) == ConvertedLoop{0, 384}));
    const hokuto::song::Sequence moved =
        hokuto::edit::TimelineRead(settled, 0, Read(hokuto::edit::FoldTimelines(settled), 0));
    const auto moved_track = std::ranges::find(moved.tracks, 1, &hokuto::song::Track::number);
    XSFLIB_CHECK(moved.loop && moved.loop->start == 0 && moved.loop->end == 384 && moved_track != moved.tracks.end() &&
                 moved_track->notes.size() == 2 && moved_track->notes.back().start == 383);
}

// The ticks of each note of track `number` of `song`, with its place for LimitsOf and EditNotes.
std::vector<std::pair<uint32_t, hokuto::edit::NotePlace>> NotesOf(const hokuto::song::Sequence& song, int number)
{
    std::vector<std::pair<uint32_t, hokuto::edit::NotePlace>> notes;
    for (const hokuto::song::Track& track : song.tracks)
    {
        if (track.number != number)
        {
            continue;
        }

        for (const hokuto::song::Note& note : track.notes)
        {
            notes.emplace_back(note.start, hokuto::edit::NotePlace{note.command, track.stretches.at(note.stretch)});
        }
    }

    return notes;
}

// Whether EditNotes refuses to move the note at `place` of sequence `sequence` by `shift` ticks with `message`.
bool RefusesMove(const File& file, uint32_t sequence, const hokuto::edit::NotePlace& place, int32_t shift,
                 const std::string& message)
{
    try
    {
        hokuto::edit::EditNotes(
            file, sequence,
            std::vector{hokuto::edit::NoteEdit{.command = place.command, .stretch = place.stretch, .shift = shift}});
    }
    catch (const std::invalid_argument& error)
    {
        return error.what() == message;
    }

    return false;
}

// Paste Track opens a pasted track where the sound starts, before a header's waits. The waited song in SEQ_MAIN's place
// takes the opening song's track 0. That track ends. The waited song's tracks still loop from tick 17 to 209. The
// pasted track's note at tick 192 moves up to tick 208, like a note of a looping track. The ending song in SEQ_MAIN's
// place takes the waited song's track 1. That track loops from tick 17 to 209, and the ending song's track 0 from tick
// 0 to 384. The song then has no single loop. Its looping tracks have each gone back once by tick 384. Track 1's last
// note, at tick 96, moves up to tick 383 and stays in the arrangement there. The waited song with its track 1 pasted
// again moves its loop. Each track counts the loop's ticks from where the header starts it.
void TestPastedLoops()
{
    constexpr uint32_t kMain = 0;
    constexpr uint32_t kOpening = 1;
    const std::string kLoopEnd =
        "A note can't start at or after the loop's end. The song goes back to the loop's start there.";

    const auto paste = [](const File& opened, int track)
    {
        const std::vector<std::optional<SoundPlace>> places = PlacesOf(opened);
        const hokuto::edit::CopiedTrack copied =
            hokuto::edit::CopyTrack(opened, places, kOpening, Read(opened, kOpening), track);
        const hokuto::edit::TrackEdit pasted =
            hokuto::edit::PasteTrack(opened, places, kMain, Read(opened, kMain), copied);
        return std::pair(hokuto::edit::SettleTimelines(opened, pasted.file).file, pasted.track);
    };

    const File waited =
        hokuto::edit::OpenTimelines(hokuto::format::ImportSwitchArchive(SongsArchive(true, WaitedSong())));
    const auto [with_ending, ending_track] = paste(waited, 0);
    XSFLIB_CHECK(hokuto::edit::IsTimeline(with_ending, kMain));
    XSFLIB_CHECK((hokuto::edit::TimelineLoop(with_ending, kMain) == ConvertedLoop{17, 209}));
    const auto pasted_notes = NotesOf(Read(with_ending, kMain), ending_track);
    XSFLIB_CHECK(pasted_notes.size() == 2 && pasted_notes.back().first == 192);
    if (pasted_notes.size() == 2)
    {
        const hokuto::edit::NotePlace& last = pasted_notes.back().second;
        const auto limits = hokuto::edit::LimitsOf(with_ending, kMain, std::span(&last, 1));
        XSFLIB_CHECK(limits.at(0).shift && limits.at(0).shift->later == 16);
        XSFLIB_CHECK(RefusesMove(with_ending, kMain, last, 17, kLoopEnd));
    }

    const File ending = hokuto::edit::OpenTimelines(
        hokuto::format::ImportSwitchArchive(SongsArchive(false, EndingSong(), WaitedSong())));
    const auto [with_loop, looping_track] = paste(ending, 1);
    XSFLIB_CHECK(hokuto::edit::IsTimeline(with_loop, kMain) && !hokuto::edit::TimelineLoop(with_loop, kMain));
    const hokuto::edit::SongEnd song_end = hokuto::edit::TimelineSongEnd(with_loop, kMain);
    XSFLIB_CHECK(song_end.loops && song_end.tick == 384u);
    const auto track_1 = NotesOf(Read(with_loop, kMain), 1);
    XSFLIB_CHECK(looping_track == 2 && track_1.size() == 2 && track_1.back().first == 96);
    if (track_1.size() == 2)
    {
        const hokuto::edit::NotePlace& last = track_1.back().second;
        const auto limits = hokuto::edit::LimitsOf(with_loop, kMain, std::span(&last, 1));
        XSFLIB_CHECK(limits.at(0).shift && limits.at(0).shift->later == 287);
        XSFLIB_CHECK(RefusesMove(with_loop, kMain, last, 288, kLoopEnd));
        const std::vector moves{hokuto::edit::NoteEdit{.command = last.command, .stretch = last.stretch, .shift = 287}};
        const File edited = hokuto::edit::EditNotes(with_loop, kMain, moves).file;
        const File moved = hokuto::edit::SettleTimelines(with_loop, edited).file;
        const auto moved_notes = NotesOf(Read(moved, kMain), 1);
        XSFLIB_CHECK(moved_notes.size() == 2 && moved_notes.back().first == 383);
    }

    const File twice = hokuto::edit::OpenTimelines(
        hokuto::format::ImportSwitchArchive(SongsArchive(false, WaitedSong(), WaitedSong())));
    const auto [with_copy, copy_track] = paste(twice, 1);
    XSFLIB_CHECK(copy_track == 2 && (hokuto::edit::TimelineLoop(with_copy, kMain) == ConvertedLoop{17, 209}));
    const Edited moved_loop = hokuto::edit::MoveLoop(with_copy, kMain, {113, 209});
    const File settled_loop = hokuto::edit::SettleTimelines(with_copy, moved_loop.file).file;
    XSFLIB_CHECK((hokuto::edit::TimelineLoop(settled_loop, kMain) == ConvertedLoop{113, 209}));
}

// Tracks that loop at other ticks. The ending song in SEQ_MAIN's place takes the main song's track 0. That track loops
// from tick 384 to 768. The ending song's track 0 loops from tick 0 to 384. Both loops last 384 ticks. The song
// therefore repeats from tick 384 to 768. Track 1 ends at tick 192. Its last note, at tick 96, moves up to tick 767.
// The note then plays in the first pass of the song's loop. ntdlib's performer finds the loop a pass later. The
// arrangement still shows the loop from tick 384 to 768, with the note in it.
//
// The waited song in SEQ_MAIN's place takes the ending song's track 0. The track's loop lasts 384 ticks, twice the
// waited song's. ntdlib's performer finds the waited song's loop, from tick 17 to 209. The arrangement shows the pasted
// track's notes up to tick 209. Without the loop that the arrangement shows, the pasted track's note at tick 192 moves
// up to tick 383, before its track's loop ends. With it, the note moves up to tick 208. It stays in the arrangement
// there, and the loop stays.
void TestOtherLoops()
{
    constexpr uint32_t kMain = 0;
    constexpr uint32_t kOpening = 1;
    const auto paste = [](const File& opened, int track)
    {
        const std::vector<std::optional<SoundPlace>> places = PlacesOf(opened);
        const hokuto::edit::CopiedTrack copied =
            hokuto::edit::CopyTrack(opened, places, kOpening, Read(opened, kOpening), track);
        const hokuto::edit::TrackEdit pasted =
            hokuto::edit::PasteTrack(opened, places, kMain, Read(opened, kMain), copied);
        return hokuto::edit::SettleTimelines(opened, pasted.file).file;
    };

    const auto shown = [](const File& file)
    {
        return hokuto::edit::TimelineRead(file, kMain, Read(hokuto::edit::FoldTimelines(file), kMain));
    };

    const File ending = paste(
        hokuto::edit::OpenTimelines(hokuto::format::ImportSwitchArchive(SongsArchive(false, EndingSong(), MainSong()))),
        0);
    XSFLIB_CHECK(hokuto::edit::IsTimeline(ending, kMain) && !hokuto::edit::TimelineLoop(ending, kMain));
    XSFLIB_CHECK((SongLoopsOf(*ending.sequences[kMain]) == std::vector<ConvertedLoop>{{0, 384}, {384, 768}}));
    XSFLIB_CHECK((hokuto::edit::TimelineSongLoop(ending, kMain) == ConvertedLoop{384, 768}));
    const hokuto::song::Sequence before = shown(ending);
    XSFLIB_CHECK(before.loop && before.loop->start == 384 && before.loop->end == 768);
    const auto track_1 = NotesOf(before, 1);
    XSFLIB_CHECK(track_1.size() == 2 && track_1.back().first == 96);
    if (track_1.size() == 2 && before.loop)
    {
        const hokuto::edit::NotePlace& last = track_1.back().second;
        const auto limits = hokuto::edit::LimitsOf(ending, kMain, std::span(&last, 1), before.loop->end);
        XSFLIB_CHECK(limits.at(0).shift && limits.at(0).shift->later == 671);

        const File moved =
            hokuto::edit::SettleTimelines(
                ending, hokuto::edit::EditNotes(ending, kMain,
                                                std::vector{hokuto::edit::NoteEdit{
                                                    .command = last.command, .stretch = last.stretch, .shift = 671}})
                            .file)
                .file;

        const hokuto::song::Sequence after = shown(moved);
        const auto moved_notes = NotesOf(after, 1);
        XSFLIB_CHECK(after.loop && after.loop->start == 384 && after.loop->end == 768);
        XSFLIB_CHECK(moved_notes.size() == 2 && moved_notes.back().first == 767);
    }

    const File waited = paste(hokuto::edit::OpenTimelines(
                                  hokuto::format::ImportSwitchArchive(SongsArchive(false, WaitedSong(), EndingSong()))),
                              0);
    XSFLIB_CHECK(!hokuto::edit::TimelineLoop(waited, kMain) && !hokuto::edit::TimelineSongLoop(waited, kMain));
    XSFLIB_CHECK((SongLoopsOf(*waited.sequences[kMain]) == std::vector<ConvertedLoop>{{17, 209}, {17, 209}, {0, 384}}));
    const hokuto::song::Sequence waited_before = shown(waited);
    XSFLIB_CHECK(waited_before.loop && waited_before.loop->start == 17 && waited_before.loop->end == 209);
    const auto pasted = NotesOf(waited_before, 2);
    XSFLIB_CHECK(pasted.size() == 3 && pasted.back().first == 192);
    if (pasted.size() == 3 && waited_before.loop)
    {
        const hokuto::edit::NotePlace& last = pasted.back().second;
        const auto alone = hokuto::edit::LimitsOf(waited, kMain, std::span(&last, 1));
        const auto limits = hokuto::edit::LimitsOf(waited, kMain, std::span(&last, 1), waited_before.loop->end);
        XSFLIB_CHECK(alone.at(0).shift && alone.at(0).shift->later == 191);
        XSFLIB_CHECK(limits.at(0).shift && limits.at(0).shift->later == 16);

        const File moved =
            hokuto::edit::SettleTimelines(
                waited, hokuto::edit::EditNotes(waited, kMain,
                                                std::vector{hokuto::edit::NoteEdit{
                                                    .command = last.command, .stretch = last.stretch, .shift = 16}})
                            .file)
                .file;

        const hokuto::song::Sequence after = shown(moved);
        const auto moved_notes = NotesOf(after, 2);
        XSFLIB_CHECK(after.loop && after.loop->start == 17 && after.loop->end == 209);
        XSFLIB_CHECK(moved_notes.size() == 3 && moved_notes.back().first == 208);
    }
}

// A timeline takes a new track, a copy and a pasted track as NintendoWare's MIDI converter lays out a song of more
// tracks. The sounds start at the songs' Start labels. In the opening song, the label names track 0's first command.
// The new track start takes the sound's start and the Start label there, and track 0 keeps its label. In the main song,
// the new track's code goes after track 1's, and the track loops with the song. Each save holds the converter's layout.
void TestTimelineTracks()
{
    constexpr uint32_t kMain = 0;
    constexpr uint32_t kOpening = 1;
    const File opened = hokuto::edit::OpenTimelines(hokuto::format::ImportSwitchArchive(SongsArchive(true)));
    XSFLIB_CHECK(hokuto::edit::IsTimeline(opened, kMain) && hokuto::edit::IsTimeline(opened, kOpening));
    const std::vector<std::optional<SoundPlace>> places = PlacesOf(opened);
    const hokuto::song::Sequence main = Read(opened, kMain);
    const hokuto::song::Sequence opening = Read(opened, kOpening);

    const auto item_of = [](const Sequence& sequence, const std::string& name)
    {
        const auto label = std::find_if(sequence.labels.begin(), sequence.labels.end(),
                                        [&name](const Label& l) { return l.name == name; });
        return label == sequence.labels.end() ? std::nullopt : std::optional<uint32_t>(label->item);
    };

    const auto notes_of = [](const hokuto::song::Sequence& song, int number)
    {
        std::vector<std::pair<uint32_t, int>> notes;
        for (const hokuto::song::Track& track : song.tracks)
        {
            if (track.number != number)
            {
                continue;
            }

            for (const hokuto::song::Note& note : track.notes)
            {
                notes.emplace_back(note.start, note.key);
            }
        }

        return notes;
    };

    const auto has_track = [](const hokuto::song::Sequence& song, int number)
    {
        return std::any_of(song.tracks.begin(), song.tracks.end(),
                           [number](const hokuto::song::Track& track) { return track.number == number; });
    };

    // A settled edit keeps the timeline, and the converter's rules read the save again.
    const auto settled = [&opened](const File& edited, uint32_t sequence)
    {
        const Settled result = hokuto::edit::SettleTimelines(opened, edited);
        XSFLIB_CHECK(hokuto::edit::IsTimeline(result.file, sequence));
        const File saved = hokuto::edit::FoldTimelines(result.file);
        XSFLIB_CHECK(ReadConverted(*saved.sequences[sequence]).hints.has_value());

        return result.file;
    };

    // The opening song's new track start takes the Start label, and the track's code goes before the End label.
    const hokuto::edit::TrackEdit added =
        hokuto::edit::AddTrack(opened, places, kOpening, opening, hokuto::edit::Program{0, 0});
    XSFLIB_CHECK(added.track == 1);
    const File with_track = settled(added.file, kOpening);
    const Sequence& song = *with_track.sequences[kOpening];
    const std::optional<uint32_t> start = item_of(song, "SMF_O_Start");
    XSFLIB_CHECK(start && song.items.at(*start).kind == Kind::kOpenTrack && song.items.at(*start).index == 1);
    XSFLIB_CHECK(item_of(song, "SMF_O_Track_0") == std::optional<uint32_t>(*start + 1));
    const std::optional<uint32_t> track_1 = item_of(song, "SMF_O_Track_1");
    XSFLIB_CHECK(track_1 && song.items.at(*start).target == *track_1 && *track_1 < *item_of(song, "SMF_O_End"));
    const hokuto::song::Sequence with_added = Read(hokuto::edit::FoldTimelines(with_track), kOpening);
    XSFLIB_CHECK(has_track(with_added, 1) && notes_of(with_added, 0) == notes_of(opening, 0));

    // The main song's new track loops with the song.
    const hokuto::edit::TrackEdit looping = hokuto::edit::AddTrack(opened, places, kMain, main, std::nullopt);
    XSFLIB_CHECK(looping.track == 2);
    const File with_loop = settled(looping.file, kMain);
    const Sequence& main_song = *with_loop.sequences[kMain];
    const std::optional<uint32_t> track_2 = item_of(main_song, "SMF_T_Track_2");
    XSFLIB_CHECK(track_2 && *item_of(main_song, "SMF_T_Track_1") < *track_2 &&
                 *track_2 < *item_of(main_song, "SMF_T_End"));
    XSFLIB_CHECK((LoopOfConverted(main_song) == ConvertedLoop{384, 768}));
    const hokuto::song::Sequence with_loop_read = Read(hokuto::edit::FoldTimelines(with_loop), kMain);
    XSFLIB_CHECK(has_track(with_loop_read, 2) && with_loop_read.loop && with_loop_read.loop->start == 384 &&
                 with_loop_read.loop->end == 768);
    XSFLIB_CHECK(notes_of(with_loop_read, 0) == notes_of(main, 0) && notes_of(with_loop_read, 1) == notes_of(main, 1));

    // A copy of the opening song's track 0 plays its notes.
    const hokuto::edit::TrackEdit copied = hokuto::edit::DuplicateTrack(opened, places, kOpening, 0);
    const hokuto::song::Sequence with_copy =
        Read(hokuto::edit::FoldTimelines(settled(copied.file, kOpening)), kOpening);
    XSFLIB_CHECK(copied.track == 1 && notes_of(with_copy, 1) == notes_of(opening, 0));

    // The main song's track 1 pasted into the opening song plays its notes, and its loop's label takes the opening
    // song's name of the track.
    const hokuto::edit::CopiedTrack track = hokuto::edit::CopyTrack(opened, places, kMain, main, 1);
    const hokuto::edit::TrackEdit pasted = hokuto::edit::PasteTrack(opened, places, kOpening, opening, track);
    const File with_paste = settled(pasted.file, kOpening);
    XSFLIB_CHECK(item_of(*with_paste.sequences[kOpening], "SMF_O_Track_1_LoopStart").has_value());
    const hokuto::song::Sequence pasted_read = Read(hokuto::edit::FoldTimelines(with_paste), kOpening);

    // The opening song's track 0 ends before the loop. ntdlib's performer then finds the loop a pass later.
    const std::vector<std::pair<uint32_t, int>> played = notes_of(pasted_read, 1);
    const std::vector<std::pair<uint32_t, int>> pass = notes_of(main, 1);
    XSFLIB_CHECK(pasted.track == 1 && played.size() >= pass.size() &&
                 std::equal(pass.begin(), pass.end(), played.begin()));
}

// The waited song in SEQ_MAIN's place. The timeline's loop counts the header's wait, and a drag of an edge moves it by
// the same ticks. The loop can't start before the tracks. A new track starts with the song's tracks, before the
// header's jump at tick 17, and loops with them.
void TestWaitedTimeline()
{
    constexpr uint32_t kMain = 0;
    const File opened =
        hokuto::edit::OpenTimelines(hokuto::format::ImportSwitchArchive(SongsArchive(true, WaitedSong())));
    XSFLIB_CHECK(hokuto::edit::IsTimeline(opened, kMain));
    XSFLIB_CHECK((hokuto::edit::TimelineLoop(opened, kMain) == ConvertedLoop{17, 209}));
    const hokuto::song::Sequence main = hokuto::edit::TimelineRead(opened, kMain, Read(opened, kMain));
    XSFLIB_CHECK(main.loop && main.loop->start == 17 && main.loop->end == 209);

    const Edited moved = hokuto::edit::MoveLoop(opened, kMain, {113, 209});
    XSFLIB_CHECK((hokuto::edit::TimelineLoop(moved.file, kMain) == ConvertedLoop{113, 209}));
    bool refused = false;
    try
    {
        hokuto::edit::MoveLoop(opened, kMain, {10, 209});
    }
    catch (const std::invalid_argument& error)
    {
        refused = std::string(error.what()) == "The loop can't start before the song's tracks start.";
    }
    XSFLIB_CHECK(refused);

    const hokuto::edit::TrackEdit added = hokuto::edit::AddTrack(opened, PlacesOf(opened), kMain, main, std::nullopt);
    const File settled = hokuto::edit::SettleTimelines(opened, added.file).file;
    const Sequence& song = *settled.sequences[kMain];
    XSFLIB_CHECK(added.track == 2 && hokuto::edit::IsTimeline(settled, kMain));
    XSFLIB_CHECK(song.items.at(3).kind == Kind::kOpenTrack && song.items.at(3).index == 2 &&
                 song.items.at(4).kind == Kind::kJump);
    XSFLIB_CHECK((hokuto::edit::TimelineLoop(settled, kMain) == ConvertedLoop{17, 209}));
    const hokuto::song::Sequence read =
        hokuto::edit::TimelineRead(settled, kMain, Read(hokuto::edit::FoldTimelines(settled), kMain));
    XSFLIB_CHECK(read.loop && read.loop->start == 17 && read.loop->end == 209 &&
                 std::ranges::any_of(read.tracks, [](const hokuto::song::Track& track) { return track.number == 2; }));
}

} // namespace

int main()
{
    TestRoundTrip();
    TestExceptions();
    TestOpening();
    TestRefusals();
    TestSettled();
    TestMatching();
    TestRenumbered();
    TestSongStart();
    TestTrackStarts();
    TestLoop();
    TestTwoJumps();
    TestTimelines();
    TestReplaced();
    TestTimelineMoves();
    TestEndingTrackMoves();
    TestTimelineTracks();
    TestWaitedTimeline();
    TestPastedLoops();
    TestOtherLoops();

    return xsflib::testing::Summary("converter");
}
