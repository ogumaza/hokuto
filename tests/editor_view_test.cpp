// SPDX-License-Identifier: MIT

// Tests the view of a sound that a file keeps (src/app/editor_view.h): from the transport, the arrangement, the tracks'
// mutes and solos and the editors, and back into them. The states are made by hand. Tests the cycle's locators for a
// sound's sequence and after a new version of it too, with made-up sequences.

#include "app/editor_view.h"

#include <array>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "xsflib/testing/check.h"

#include "app/key_editor.h"
#include "app/song_view.h"
#include "app/transport.h"
#include "app/views.h"
#include "format/file.h"
#include "song/sequence.h"

using hokuto::app::ApplyView;
using hokuto::app::ArrangementState;
using hokuto::app::KeyEditorState;
using hokuto::app::LaneSetting;
using hokuto::app::Locators;
using hokuto::app::LocatorsAfter;
using hokuto::app::OpenEditors;
using hokuto::app::SongLocators;
using hokuto::app::SongView;
using hokuto::app::Transport;
using hokuto::app::ViewOf;
using hokuto::format::EditorView;
using hokuto::format::ViewFront;
using hokuto::song::Loop;
using hokuto::song::Sequence;
using ntdlib::commands::Kind;

namespace
{

// A view of sound 7 whose tracks are numbered 0, 2 and 5: its position and cycle, the arrangement's zoom and scroll,
// track 2 selected and muted, track 0 soloed, the Key Editor's zoom, scroll and two lanes, and the Key Editor open on
// track 2, in front.
EditorView TestView()
{
    return {.sound = 7,
            .position = 4.5f,
            .cycle_start = 2.0f,
            .cycle_end = 18.0f,
            .cycle = true,
            .beat_width = 24.0f,
            .track_height = 80.0f,
            .left = 3.5f,
            .top = 1.25f,
            .selected_track = 2,
            .muted = 1u << 2,
            .soloed = 1u << 0,
            .key_beat_width = 64.0f,
            .key_height = 12.0f,
            .key_left = 0.5f,
            .key_top = 96.0f,
            .lanes = {{Kind::kVolume, 0}, {Kind::kSend, 2}},
            .key_editor_track = 2,
            .front = ViewFront::kKeyEditor};
}

// The view takes the transport, the arrangement, the mutes and solos by the tracks' numbers, and the Key Editor. Track
// 40 lies past the view's sets: its mute stays out, it can't be the selected track, and an editor on it isn't open in
// the view. Closed editors give no tracks. The view keeps the window in front.
void TestViewOf()
{
    const std::vector<int> kNumbers = {0, 2, 40};
    Transport transport;
    transport.SetLocators(2.0, 18.0);
    transport.SetCycle(true);
    transport.SetPosition(4.5);
    ArrangementState arrangement;
    arrangement.beat_width = 24.0f;
    arrangement.track_height = 80.0f;
    arrangement.left_quarter = 3.5;
    arrangement.top_track = 1.25f;
    arrangement.selected_track = 1;
    SongView song;
    song.tracks.resize(3);
    song.tracks[0].solo = true;
    song.tracks[1].mute = true;
    song.tracks[2].mute = true;
    KeyEditorState key_editor;
    key_editor.beat_width = 64.0f;
    key_editor.key_height = 12.0f;
    key_editor.left_quarter = 0.5;
    key_editor.top_key = 96.0f;
    key_editor.lanes = {{Kind::kVolume, 0}, {Kind::kSend, 2}};
    ArrangementState far = arrangement;
    far.selected_track = 2;

    const EditorView view =
        ViewOf(7, kNumbers, transport, arrangement, song, key_editor, {2, 40, ViewFront::kKeyEditor});
    const EditorView closed = ViewOf(7, kNumbers, transport, arrangement, song, key_editor, {});
    const EditorView past = ViewOf(7, kNumbers, transport, far, song, key_editor, {40, 0, ViewFront::kListEditor});

    EditorView expected = TestView();
    XSFLIB_CHECK(view == expected);
    expected.key_editor_track.reset();
    expected.front = ViewFront::kMixer;
    XSFLIB_CHECK(closed == expected);
    expected.selected_track.reset();
    expected.list_editor_track = 0;
    expected.front = ViewFront::kListEditor;
    XSFLIB_CHECK(past == expected);
}

// The view goes back into the states. The tracks take their mutes and solos by their numbers, and the selected track by
// its number. An editor opens only on a track that the sound has. A selected track that the sound lacks leaves the
// selection as it was. The scroll waits for the next frames. Lanes that the menu doesn't offer stay out: a note's, a
// fourth send's and a fifth LFO's. An editor in front that doesn't open gives way to the List Editor, then the Key
// Editor, then the mixer. The mixer always opens.
void TestApplyView()
{
    const std::vector<int> kNumbers = {0, 2, 5};
    EditorView listed = TestView();
    listed.list_editor_track = 5;
    listed.key_editor_track = 3;
    EditorView missing = TestView();
    missing.selected_track = 9;
    missing.lanes = {{Kind::kNote, 0}, {Kind::kSend, 3}, {Kind::kLfoDepth, 4}, {Kind::kLfoDepth, 3}};
    Transport transport;
    ArrangementState arrangement;
    SongView song;
    song.tracks.resize(3);
    song.tracks[2].mute = true;
    song.tracks[2].solo = true;
    KeyEditorState key_editor;
    key_editor.lanes = {{Kind::kPan, 0}};
    Transport other_transport;
    ArrangementState other;
    other.selected_track = 2;
    SongView other_song = song;
    KeyEditorState other_editor;

    const OpenEditors editors = ApplyView(TestView(), kNumbers, transport, arrangement, song, key_editor);
    const OpenEditors list = ApplyView(listed, kNumbers, transport, arrangement, song, key_editor);
    ApplyView(missing, kNumbers, other_transport, other, other_song, other_editor);

    XSFLIB_CHECK((editors == OpenEditors{2, std::nullopt, ViewFront::kKeyEditor}) &&
                 (list == OpenEditors{std::nullopt, 5, ViewFront::kListEditor}));

    const auto front =
        [&kNumbers](std::optional<uint8_t> key_track, std::optional<uint8_t> list_track, ViewFront in_view)
    {
        EditorView view = TestView();
        view.key_editor_track = key_track;
        view.list_editor_track = list_track;
        view.front = in_view;
        Transport its_transport;
        ArrangementState its_arrangement;
        SongView its_song;
        its_song.tracks.resize(3);
        KeyEditorState its_editor;
        return ApplyView(view, kNumbers, its_transport, its_arrangement, its_song, its_editor).front;
    };

    XSFLIB_CHECK(front(2, 5, ViewFront::kKeyEditor) == ViewFront::kKeyEditor);
    XSFLIB_CHECK(front(2, 5, ViewFront::kMixer) == ViewFront::kMixer);
    XSFLIB_CHECK(front(2, std::nullopt, ViewFront::kListEditor) == ViewFront::kKeyEditor);
    XSFLIB_CHECK(front(std::nullopt, std::nullopt, ViewFront::kKeyEditor) == ViewFront::kMixer);
    XSFLIB_CHECK(front(std::nullopt, std::nullopt, ViewFront::kListEditor) == ViewFront::kMixer);
    XSFLIB_CHECK(front(2, 5, ViewFront::kListEditor) == ViewFront::kListEditor);
    XSFLIB_CHECK(transport.Position() == 4.5 && transport.CycleStart() == 2.0 && transport.CycleEnd() == 18.0 &&
                 transport.Cycle());
    XSFLIB_CHECK(arrangement.beat_width == 24.0f && arrangement.track_height == 80.0f);
    XSFLIB_CHECK((arrangement.scroll_to == std::pair<double, float>{3.5, 1.25f}) && arrangement.selected_track == 1);
    XSFLIB_CHECK(!song.tracks[0].mute && song.tracks[0].solo && song.tracks[1].mute && !song.tracks[1].solo);
    XSFLIB_CHECK(!song.tracks[2].mute && !song.tracks[2].solo);
    XSFLIB_CHECK(key_editor.beat_width == 64.0f && key_editor.key_height == 12.0f);
    XSFLIB_CHECK((key_editor.scroll_to == std::pair<double, float>{0.5, 96.0f}));
    XSFLIB_CHECK((key_editor.lanes == std::vector<LaneSetting>{{Kind::kVolume, 0}, {Kind::kSend, 2}}));
    XSFLIB_CHECK(other.selected_track == 2);
    XSFLIB_CHECK((other_editor.lanes == std::vector<LaneSetting>{{Kind::kLfoDepth, 3}}));
}

// A float rounds a position. A position within a float's step of a tick goes back to the tick: tick 335 of a sound of
// 48 ticks to the quarter, tick 1,003 of a sound of 96, and tick 5 of the display's 480. A float holds the first as
// 6.9791665 quarter notes. A position between the ticks stays.
void TestRoundedPositions()
{
    const std::vector<int> kNumbers = {0, 2, 5};
    const auto restored = [&kNumbers](double position, double cycle_start, double cycle_end, int ticks_per_quarter)
    {
        EditorView view = TestView();
        view.position = static_cast<float>(position);
        view.cycle_start = static_cast<float>(cycle_start);
        view.cycle_end = static_cast<float>(cycle_end);
        Transport transport;
        ArrangementState arrangement;
        SongView song;
        song.tracks.resize(3);
        song.ticks_per_quarter = ticks_per_quarter;
        KeyEditorState key_editor;
        ApplyView(view, kNumbers, transport, arrangement, song, key_editor);

        return std::array<double, 3>{transport.Position(), transport.CycleStart(), transport.CycleEnd()};
    };

    XSFLIB_CHECK(static_cast<float>(335.0 / 48) == 6.9791665f && 6.9791665f * 480.0 < 3350.0);
    XSFLIB_CHECK((restored(335.0 / 48, 5.0 / 480, 4.401, 48) ==
                  std::array<double, 3>{335.0 / 48, 5.0 / 480, static_cast<double>(4.401f)}));
    XSFLIB_CHECK(
        (restored(1003.0 / 96, 0.1, 2000.0 / 96, 96) == std::array<double, 3>{1003.0 / 96, 48.0 / 480, 2000.0 / 96}));
}

// A view put back and taken again comes back unchanged, once the next frames have taken the scroll.
void TestRoundTrip()
{
    const std::vector<int> kNumbers = {0, 2, 5};
    Transport transport;
    ArrangementState arrangement;
    SongView song;
    song.tracks.resize(3);
    KeyEditorState key_editor;
    const OpenEditors editors = ApplyView(TestView(), kNumbers, transport, arrangement, song, key_editor);
    SEQLIB_REQUIRE(arrangement.scroll_to && key_editor.scroll_to);
    arrangement.left_quarter = arrangement.scroll_to->first;
    arrangement.top_track = arrangement.scroll_to->second;
    key_editor.left_quarter = key_editor.scroll_to->first;
    key_editor.top_key = key_editor.scroll_to->second;

    const EditorView view = ViewOf(7, kNumbers, transport, arrangement, song, key_editor, editors);

    XSFLIB_CHECK(view == TestView());
}

// A sequence at 48 ticks to the quarter note that lasts `length` ticks, with `loop`. The last note of the sequence ends
// at tick `notes_end`.
Sequence SongOf(uint32_t length, uint32_t notes_end, std::optional<Loop> loop)
{
    Sequence sequence;
    sequence.timebase = 48;
    sequence.length = length;
    sequence.notes_end = notes_end;
    sequence.loop = loop;
    return sequence;
}

// The locators go round a sequence's loop. Without one, they run from the start to the beat on which the last note
// ends: 1,000 ticks end in beat 21. For a sequence without notes, they span its length.
void TestSongLocators()
{
    XSFLIB_CHECK((SongLocators(SongOf(3072, 3072, Loop{384, 3072})) == Locators{8.0, 64.0}));
    XSFLIB_CHECK((SongLocators(SongOf(1100, 1000, std::nullopt)) == Locators{0.0, 21.0}));
    XSFLIB_CHECK((SongLocators(SongOf(96, 0, std::nullopt)) == Locators{0.0, 2.0}));
}

// A song whose loop runs from beat 8 to beat 24 replaces one whose loop runs over its 64 beats. Replace… makes such a
// version. Locators on the old loop move to the new one, and an undo moves them back. So do locators on the old loop
// that a file's view kept as floats: 335 ticks come back as 6.9791665 quarter notes. The user's locators inside the
// new song stay. Those that start at or past its end take the new loop. A version with the same loop keeps the
// locators.
void TestLocatorsAfter()
{
    const Sequence long_song = SongOf(3072, 3072, Loop{0, 3072});
    const Sequence short_song = SongOf(1152, 1152, Loop{384, 1152});
    XSFLIB_CHECK((LocatorsAfter({0.0, 64.0}, long_song, short_song) == Locators{8.0, 24.0}));
    XSFLIB_CHECK((LocatorsAfter({8.0, 24.0}, short_song, long_song) == Locators{0.0, 64.0}));
    const Sequence odd_loop = SongOf(3072, 3072, Loop{335, 3072});
    const auto kept = static_cast<double>(static_cast<float>(335.0 / 48));
    XSFLIB_CHECK(kept != 335.0 / 48);
    XSFLIB_CHECK((LocatorsAfter({kept, 64.0}, odd_loop, short_song) == Locators{8.0, 24.0}));

    XSFLIB_CHECK((LocatorsAfter({4.0, 12.0}, long_song, short_song) == Locators{4.0, 12.0}));
    XSFLIB_CHECK((LocatorsAfter({24.0, 32.0}, long_song, short_song) == Locators{8.0, 24.0}));
    XSFLIB_CHECK((LocatorsAfter({40.0, 48.0}, long_song, short_song) == Locators{8.0, 24.0}));
    XSFLIB_CHECK((LocatorsAfter({0.0, 64.0}, long_song, long_song) == Locators{0.0, 64.0}));
    XSFLIB_CHECK((LocatorsAfter({2.0, 6.0}, long_song, long_song) == Locators{2.0, 6.0}));
}

// Without a loop, the locators that showing a song set only grow. Its last note ends at tick 1,000, in beat 21. They
// reach beat 23 once the last note ends at tick 1,100, and stay there when it moves back to tick 1,000. They reach beat
// 25 once it ends at tick 1,200: locators from the start that end past the last note count as grown. So do the user's
// locators from the start to beat 30. They stay while the notes end before them, and reach beat 32 once the last note
// ends at tick 1,500. Locators on a loop that is removed reach back to the start, and keep the loop's end where the
// last note ends before it. The user's locators elsewhere stay, unless they start at or past the song's end.
void TestLocatorsWithoutLoop()
{
    const Sequence ends_1000 = SongOf(1100, 1000, std::nullopt);
    const Sequence ends_1100 = SongOf(1200, 1100, std::nullopt);
    const Sequence ends_1200 = SongOf(1300, 1200, std::nullopt);
    const Sequence ends_1500 = SongOf(1600, 1500, std::nullopt);
    XSFLIB_CHECK((LocatorsAfter({0.0, 21.0}, ends_1000, ends_1100) == Locators{0.0, 23.0}));
    XSFLIB_CHECK((LocatorsAfter({0.0, 23.0}, ends_1100, ends_1000) == Locators{0.0, 23.0}));
    XSFLIB_CHECK((LocatorsAfter({0.0, 23.0}, ends_1000, ends_1200) == Locators{0.0, 25.0}));
    XSFLIB_CHECK((LocatorsAfter({0.0, 30.0}, ends_1000, ends_1100) == Locators{0.0, 30.0}));
    XSFLIB_CHECK((LocatorsAfter({0.0, 30.0}, ends_1100, ends_1500) == Locators{0.0, 32.0}));

    const Sequence looped = SongOf(1152, 1152, Loop{384, 1152});
    XSFLIB_CHECK((LocatorsAfter({8.0, 24.0}, looped, ends_1000) == Locators{0.0, 24.0}));
    XSFLIB_CHECK((LocatorsAfter({2.0, 6.0}, ends_1000, ends_1100) == Locators{2.0, 6.0}));
    XSFLIB_CHECK((LocatorsAfter({30.0, 40.0}, looped, ends_1000) == Locators{0.0, 21.0}));
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("editor_view",
                                    []
                                    {
                                        TestViewOf();
                                        TestApplyView();
                                        TestRoundedPositions();
                                        TestRoundTrip();
                                        TestSongLocators();
                                        TestLocatorsAfter();
                                        TestLocatorsWithoutLoop();
                                    });
}
