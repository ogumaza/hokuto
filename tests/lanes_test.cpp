// SPDX-License-Identifier: MIT

// Tests what the Key Editor's controller lanes work out without a window (src/app/lanes.h): the settings' names and
// menu, the lines of the lanes, their ranges, and the edits of their gestures. Tracks made by hand hold the commands of
// the settings. A small Switch archive then gives the commands of a sound's track, at the ticks where ntdlib's
// performer runs them.

#include "app/lanes.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/commands/units.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/testing/sar_files.h"
#include "xsflib/testing/check.h"

#include "app/key_editor.h"
#include "edit/notes.h"
#include "format/archive.h"
#include "format/file.h"
#include "song/archive.h"
#include "song/sequence.h"

using hokuto::app::ControlsOf;
using hokuto::app::DraggedControls;
using hokuto::app::KeyControl;
using hokuto::app::KeyEdit;
using hokuto::app::KeyEditorState;
using hokuto::app::KeyTrack;
using hokuto::app::Lane;
using hokuto::app::LaneDrawEdit;
using hokuto::app::LaneGroup;
using hokuto::app::LaneGroups;
using hokuto::app::LaneName;
using hokuto::app::LaneOf;
using hokuto::app::LanePiece;
using hokuto::app::LanePoint;
using hokuto::app::LanePointEdit;
using hokuto::app::LaneRange;
using hokuto::app::LaneRemoval;
using hokuto::app::LaneSetting;
using hokuto::app::LaneStartEdit;
using hokuto::app::LaneValueEdit;
using hokuto::app::NearestValue;
using hokuto::app::NextLane;
using hokuto::app::SettingOf;
using hokuto::app::StartingValue;
using hokuto::app::WhyPointsStay;
using hokuto::edit::ShiftRange;
using hokuto::edit::ValueRange;
using ntdlib::commands::Argument;
using ntdlib::commands::Command;
using ntdlib::commands::Kind;
using ntdlib::commands::kOne;
using ntdlib::commands::Source;

namespace
{

namespace sar = ntdlib::sar;
namespace sar_files = ntdlib::testing::sar_files;

// A command of `kind` that sets the value `value`, in the setting's unit, and addresses `index`.
Command Valued(Kind kind, Argument value, uint8_t index = 0)
{
    Command command;
    command.kind = kind;
    command.index = index;
    command.value = value;

    return command;
}

// A plain value of `units` in fixed point.
Argument Plain(int64_t units)
{
    return Argument{Source::kValue, units * kOne, 0};
}

// A point of a lane: the command, at `offset` and `tick`, in the track's first stretch.
KeyControl Point(Command command, uint32_t offset, uint32_t tick)
{
    return KeyControl{std::move(command), offset, tick, 0};
}

// A track of the Switch, 96 ticks long, with `controls`.
KeyTrack TrackOf(std::vector<KeyControl> controls)
{
    KeyTrack track;
    track.ticks_per_quarter = 48;
    track.song_length = 96;
    track.controls = std::move(controls);

    return track;
}

// A setting's name names its bus or its LFO. Each setting of the menu is one that a command can set, and a command sets
// a setting only where the menu offers it.
void TestNamesAndSettings()
{
    std::size_t settings = 0;
    bool each_set = true;
    for (const LaneGroup& group : LaneGroups())
    {
        for (const LaneSetting setting : group.settings)
        {
            settings++;
            each_set = each_set && SettingOf(Valued(setting.kind, Plain(1), setting.index)) == setting;
        }
    }

    XSFLIB_CHECK(LaneName({Kind::kVolume}) == "Volume" && LaneName({Kind::kSend, 1}) == "Send B");
    XSFLIB_CHECK(LaneName({Kind::kLfoDepth, 1}) == "LFO 1 Depth" && LaneName({Kind::kPitchBend}) == "Pitch Bend");
    XSFLIB_CHECK(settings == 65 && each_set);
    XSFLIB_CHECK(!SettingOf(Valued(Kind::kNote, Plain(24))) && !SettingOf(Valued(Kind::kNoteWait, Plain(0))));
    XSFLIB_CHECK(!SettingOf(Valued(Kind::kProgram, Plain(3))) && !SettingOf(Valued(Kind::kSend, Plain(3), 3)));
    XSFLIB_CHECK(!SettingOf(Valued(Kind::kLfoSpeed, Plain(3), 4)));
    XSFLIB_CHECK((SettingOf(Valued(Kind::kVolume, Plain(3), 2)) == LaneSetting{Kind::kVolume, 0}));
}

// A value holds up to the next point. A value that moves over a time slopes from the value before, and the next point
// stops it halfway. Its point sits where the slope ends. The value that a track starts with leads in, where there's
// one. A random value or a variable's value has no line up to the next point.
void TestLines()
{
    Command ramp = Valued(Kind::kVolume, Plain(80));
    ramp.time = Plain(24);
    const KeyTrack volumes = TrackOf({Point(Valued(Kind::kVolume, Plain(100)), 0, 0), Point(ramp, 2, 48),
                                      Point(Valued(Kind::kVolume, Plain(127)), 4, 60)});
    Command fade = Valued(Kind::kVolume, Plain(0));
    fade.time = Plain(12);
    const KeyTrack faded = TrackOf({Point(fade, 0, 0)});
    const KeyTrack pans = TrackOf({Point(Valued(Kind::kPan, Plain(-32)), 0, 24),
                                   Point(Valued(Kind::kPan, Argument{Source::kRandom, -10 * kOne, 10 * kOne}), 2, 48),
                                   Point(Valued(Kind::kVolume, Plain(1)), 4, 50)});
    const KeyTrack empty = TrackOf({});

    const Lane volume = LaneOf(volumes, {Kind::kVolume});
    const Lane fade_lane = LaneOf(faded, {Kind::kVolume});
    const Lane pan = LaneOf(pans, {Kind::kPan});
    const Lane attack = LaneOf(pans, {Kind::kAttack});
    const Lane tempo = LaneOf(empty, {Kind::kTempo});

    const std::vector<LanePiece> kVolumeLine = {
        {0.0, 48.0, 100 * kOne, 100 * kOne}, {48.0, 60.0, 100 * kOne, 90 * kOne}, {60.0, 96.0, 127 * kOne, 127 * kOne}};
    XSFLIB_CHECK(volume.line == kVolumeLine);
    XSFLIB_CHECK((volume.points == std::vector<LanePoint>{{0, 0.0}, {1, 60.0}, {2, 60.0}}));
    const std::vector<LanePiece> kFadedLine = {{0.0, 12.0, 127 * kOne, 0}, {12.0, 96.0, 0, 0}};
    XSFLIB_CHECK(fade_lane.line == kFadedLine && (fade_lane.points == std::vector<LanePoint>{{0, 12.0}}));
    const std::vector<LanePiece> kPanLine = {
        {0.0, 24.0, 0, 0}, {24.0, 48.0, -32 * kOne, -32 * kOne}, {48.0, 96.0, std::nullopt, std::nullopt}};
    XSFLIB_CHECK(pan.line == kPanLine && (pan.points == std::vector<LanePoint>{{0, 24.0}, {1, 48.0}}));
    XSFLIB_CHECK(attack.line.empty() && attack.points.empty() && !StartingValue({Kind::kAttack}));
    XSFLIB_CHECK((tempo.line == std::vector<LanePiece>{{0.0, 96.0, 120 * kOne, 120 * kOne}}));
}

// A lane spans the setting's usual values, widened to hold the track's values and kept within the console's bytes. A
// value at a height takes the nearest value that the console holds, within the lane.
void TestRanges()
{
    const KeyTrack plain = TrackOf({});
    const KeyTrack loud = TrackOf({Point(Valued(Kind::kVolume, Plain(200)), 0, 0),
                                   Point(Valued(Kind::kSend, Argument{Source::kRandom, 0, 150 * kOne}), 2, 0)});

    XSFLIB_CHECK((LaneRange(plain, {Kind::kVolume}) == ValueRange{0, 127 * kOne}));
    XSFLIB_CHECK((LaneRange(loud, {Kind::kVolume}) == ValueRange{0, 200 * kOne}));
    XSFLIB_CHECK((LaneRange(loud, {Kind::kSend}) == ValueRange{0, 150 * kOne}));
    XSFLIB_CHECK((LaneRange(loud, {Kind::kSend, 1}) == ValueRange{0, 127 * kOne}));
    XSFLIB_CHECK((LaneRange(plain, {Kind::kPan}) == ValueRange{-64 * kOne, 63 * kOne}));
    XSFLIB_CHECK((LaneRange(plain, {Kind::kTempo}) == ValueRange{kOne, 240 * kOne}));
    XSFLIB_CHECK((LaneRange(plain, {Kind::kPitchBend}).highest < kOne));
    XSFLIB_CHECK((LaneRange(plain, {Kind::kTie}) == ValueRange{0, kOne}));
    const ValueRange kVolumeRange{0, 127 * kOne};
    XSFLIB_CHECK(NearestValue(hokuto::song::Console{.flavor = sar::Flavor::kNx}, {Kind::kVolume}, 100 * kOne + kOne / 3,
                              kVolumeRange) == 100 * kOne);
    XSFLIB_CHECK(NearestValue(hokuto::song::Console{.flavor = sar::Flavor::kNx}, {Kind::kVolume}, 140 * kOne,
                              kVolumeRange) == 127 * kOne);
    XSFLIB_CHECK(
        NearestValue(hokuto::song::Console{.flavor = sar::Flavor::kNx}, {Kind::kVolume}, -kOne, kVolumeRange) == 0);
}

// A drag of a point changes its value. A drag of a selected point changes every selected command of its setting by the
// same amount, within the console's bytes. A random value and a command of another setting stay. A command that two
// points show changes once.
void TestValueEdits()
{
    const KeyTrack track =
        TrackOf({Point(Valued(Kind::kVolume, Plain(100)), 0, 0), Point(Valued(Kind::kVolume, Plain(5)), 2, 24),
                 Point(Valued(Kind::kVolume, Plain(60)), 4, 48),
                 Point(Valued(Kind::kVolume, Argument{Source::kRandom, 0, 127 * kOne}), 6, 60),
                 Point(Valued(Kind::kPan, Plain(10)), 8, 60), Point(Valued(Kind::kVolume, Plain(5)), 2, 72)});
    KeyEditorState state;
    state.selected_controls = {0, 2, 6, 8};

    const std::optional<KeyEdit> together = LaneValueEdit(track, state, 0, 90 * kOne);
    const std::optional<KeyEdit> alone = LaneValueEdit(track, state, 2, 70 * kOne);
    const std::optional<KeyEdit> same = LaneValueEdit(track, state, 2, 60 * kOne);
    const std::optional<KeyEdit> random = LaneValueEdit(track, state, 3, 60 * kOne);

    SEQLIB_REQUIRE(together && alone);
    XSFLIB_CHECK(together->name == "Edit Controller" && together->changed_controls.size() == 2);
    XSFLIB_CHECK(together->changed_controls.at(0).value == Plain(90) &&
                 together->changed_controls.at(2).value == Plain(0));
    XSFLIB_CHECK(alone->changed_controls.size() == 1 && alone->changed_controls.at(4).value == Plain(70));
    XSFLIB_CHECK(!same && !random);
}

// A drag of a point moves its command in time. A drag of a selected point moves every selected command of its setting
// with it, a random value included, and changes their values. A start typed for a point moves the same commands, as far
// as the command with the least room allows. When they can't move that far, or at all, the edit says why.
void TestMoves()
{
    Command compared = Valued(Kind::kPan, Plain(0));
    compared.conditional = true;
    const KeyTrack track =
        TrackOf({Point(Valued(Kind::kVolume, Plain(100)), 0, 0), Point(Valued(Kind::kVolume, Plain(5)), 2, 24),
                 Point(Valued(Kind::kVolume, Plain(60)), 4, 48),
                 Point(Valued(Kind::kVolume, Argument{Source::kRandom, 0, 127 * kOne}), 6, 60),
                 Point(Valued(Kind::kPan, Plain(10)), 8, 60), Point(compared, 10, 72),
                 Point(Valued(Kind::kVolume, Argument{Source::kVariable, 3, 0}), 12, 84)});
    KeyEditorState state;
    state.selected_controls = {0, 2, 6, 8};
    state.limits = {{0, {ShiftRange{0, 36}, std::nullopt}},
                    {2, {ShiftRange{24, 40}, std::nullopt}},
                    {4, {}},
                    {6, {ShiftRange{12, 24}, std::nullopt}},
                    {8, {ShiftRange{60, 36}, std::nullopt}},
                    {10, {}},
                    {12, {}}};

    const std::optional<KeyEdit> dragged = LanePointEdit(track, state, 0, 12, 90 * kOne);
    const std::optional<KeyEdit> random = LanePointEdit(track, state, 3, 6, 0);
    const std::optional<KeyEdit> earlier = LanePointEdit(track, state, 1, -12, 5 * kOne);
    const std::optional<KeyEdit> still = LanePointEdit(track, state, 0, 0, 100 * kOne);
    const std::optional<KeyEdit> typed = LaneStartEdit(track, state, 0, 12);
    const std::optional<KeyEdit> farthest = LaneStartEdit(track, state, 0, 24);
    const std::optional<KeyEdit> first = LaneStartEdit(track, state, 4, 0);
    const std::optional<KeyEdit> late = LaneStartEdit(track, state, 0, 36);
    const std::optional<KeyEdit> early = LaneStartEdit(track, state, 1, 12);
    const std::optional<KeyEdit> held = LaneStartEdit(track, state, 2, 60);
    const std::optional<KeyEdit> conditional = LaneStartEdit(track, state, 5, 84);
    const std::optional<KeyEdit> there = LaneStartEdit(track, state, 0, 0);

    const std::map<uint32_t, int32_t> kLater = {{0, 12}, {2, 12}, {6, 12}};
    XSFLIB_CHECK((DraggedControls(track, state, 0) == std::set<uint32_t>{0, 2, 6}));
    XSFLIB_CHECK((DraggedControls(track, state, 2) == std::set<uint32_t>{4}));
    XSFLIB_CHECK((DraggedControls(track, state, 4) == std::set<uint32_t>{8}));
    SEQLIB_REQUIRE(dragged && random && earlier && typed && farthest && first && late && early && held && conditional);
    XSFLIB_CHECK(dragged->name == "Edit Controller" && dragged->moved_controls == kLater &&
                 dragged->changed_controls.size() == 2);
    XSFLIB_CHECK(dragged->changed_controls.at(0).value == Plain(90) &&
                 dragged->changed_controls.at(2).value == Plain(0));
    XSFLIB_CHECK(random->name == "Edit Controller" && random->changed_controls.empty() &&
                 (random->moved_controls == std::map<uint32_t, int32_t>{{0, 6}, {2, 6}, {6, 6}}));
    XSFLIB_CHECK(earlier->name == "Edit Controller" && earlier->changed_controls.empty() &&
                 (earlier->moved_controls == std::map<uint32_t, int32_t>{{0, -12}, {2, -12}, {6, -12}}));
    XSFLIB_CHECK(!still && !there && !LaneStartEdit(track, state, 2, 48));
    XSFLIB_CHECK(farthest->refused.empty() &&
                 (farthest->moved_controls == std::map<uint32_t, int32_t>{{0, 24}, {2, 24}, {6, 24}}));
    XSFLIB_CHECK(first->refused.empty() && (first->moved_controls == std::map<uint32_t, int32_t>{{8, -60}}));
    XSFLIB_CHECK(typed->moved_controls == kLater && typed->changed_controls.empty() && typed->refused.empty() &&
                 typed->name == "Move");
    XSFLIB_CHECK(late->moved_controls.empty() && late->refused.find("no later than 1. 3. 0.") != std::string::npos);
    XSFLIB_CHECK(early->moved_controls.empty() && early->refused.find("no earlier than 1. 3. 0.") != std::string::npos);
    XSFLIB_CHECK(held->moved_controls.empty() &&
                 held->refused == "This track's notes set its timing here. Hokuto can't move the command in time yet.");
    XSFLIB_CHECK(conditional->refused == "Hokuto can't move a command that runs only if the last comparison held.");
    XSFLIB_CHECK(WhyPointsStay(track, state, 6) == "Hokuto can't move a command that reads or changes a variable.");
}

// A drawn point puts a command of its setting in at its tick, and Delete removes the commands of the points.
void TestDrawAndDelete()
{
    const KeyEdit drawn = LaneDrawEdit({Kind::kSend, 2}, 36, 64 * kOne);
    const KeyEdit removed = LaneRemoval({4, 8});

    SEQLIB_REQUIRE(drawn.added_controls.size() == 1);
    XSFLIB_CHECK(drawn.name == "Edit Controller" && drawn.added_controls[0].first == 36);
    XSFLIB_CHECK(drawn.added_controls[0].second == Valued(Kind::kSend, Plain(64), 2));
    XSFLIB_CHECK(removed.name == "Delete" && removed.removed_controls == (std::set<uint32_t>{4, 8}));
}

// A new lane shows the first setting that the track sets and no lane shows, or else the first that no lane shows.
void TestNextLane()
{
    const KeyTrack track =
        TrackOf({Point(Valued(Kind::kPan, Plain(10)), 0, 0), Point(Valued(Kind::kLfoDepth, Plain(0), 1), 2, 0)});
    const std::vector<LaneSetting> kPanAlone = {{Kind::kPan}};
    const std::vector<LaneSetting> kBoth = {{Kind::kPan}, {Kind::kLfoDepth, 1}};

    XSFLIB_CHECK((NextLane(track, {}) == LaneSetting{Kind::kPan}));
    XSFLIB_CHECK((NextLane(track, kPanAlone) == LaneSetting{Kind::kLfoDepth, 1}));
    XSFLIB_CHECK((NextLane(track, kBoth) == LaneSetting{Kind::kVolume}));
}

// The bytecode of the archive's sound. Each command's offset comes first in its comment. The Switch's sequences give
// arguments in little-endian order.
const std::vector<uint8_t> kBytecode = {
    0xB0, 48,         // 0: timebase 48
    0xE1, 120,  0x00, // 2: tempo 120, at tick 0
    0xC7, 0x00,       // 5: note wait off
    0xC1, 100,        // 7: volume 100, at tick 0
    60,   100,  24,   // 9: key 60 for 24 ticks
    0x80, 48,         // 12: wait 48 ticks
    0xC0, 32,         // 14: pan 32, 32 left of the centre, at tick 48
    0xC1, 80,         // 16: volume 80, at tick 48
    62,   100,  24,   // 18: key 62 for 24 ticks
    0x80, 48,         // 21: wait 48 ticks
    0xFF,             // 23: the end
};

// The archive. Its bank plays a looped wave for every key.
std::vector<uint8_t> TestArchive()
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

    sar_files::SoundSpec song;
    song.name = "SEQ_SONG";
    song.volume = 127;
    song.banks = {0};

    sar_files::ArchiveSpec spec;
    spec.sounds = {song};
    spec.banks = {{"BANK_TEST", 1}};
    spec.wave_archives = {{"WARC_TEST", 2}};
    spec.players = {{"PLAYER_TEST", 1}};
    spec.files = {{sar_files::Sequence(false, kBytecode, {{"Song", 0}}), "", false},
                  {sar_files::Bank(false, bank), "", false},
                  {sar_files::WaveArchive(false, {sar_files::Wave(false, wave)}), "", false}};

    return sar_files::Archive(false, spec);
}

// The track's settings come in the order that the track ran them, at their ticks. The timebase and the note wait have
// no lane.
void TestControlsOfArchive()
{
    const std::vector<uint8_t> bytes = TestArchive();
    const hokuto::format::File file = hokuto::format::ImportSwitchArchive(bytes);
    const std::shared_ptr<const sar::SoundArchive> archive = hokuto::song::LoadArchive(bytes, "test.bfsar");
    const hokuto::song::Sequence song = hokuto::song::ReadSequence(*archive, 0);

    const std::vector<KeyControl> controls = ControlsOf(file, 0, song, song.tracks.at(0));

    std::vector<uint32_t> offsets;
    std::vector<uint32_t> ticks;
    for (const KeyControl& control : controls)
    {
        offsets.push_back(control.offset);
        ticks.push_back(control.tick);
    }
    XSFLIB_CHECK((offsets == std::vector<uint32_t>{2, 7, 14, 16}));
    XSFLIB_CHECK((ticks == std::vector<uint32_t>{0, 0, 48, 48}));
    SEQLIB_REQUIRE(controls.size() == 4);
    XSFLIB_CHECK(controls[2].command.kind == Kind::kPan && controls[2].command.value == Plain(-32));
    XSFLIB_CHECK(controls[1].command.kind == Kind::kVolume && controls[1].command.value == Plain(100));
}

} // namespace

int main()
{
    TestNamesAndSettings();
    TestLines();
    TestRanges();
    TestValueEdits();
    TestMoves();
    TestDrawAndDelete();
    TestNextLane();
    TestControlsOfArchive();

    return xsflib::testing::Summary("lanes");
}
