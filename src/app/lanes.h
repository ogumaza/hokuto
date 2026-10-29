// SPDX-License-Identifier: MIT

// What the Key Editor's controller lanes work out without a window: the settings that a lane can show, the commands of
// the shown track that set them, the line of a lane's values, and the edits of a lane's gestures. Their tests run
// without a window.
//
// A lane shows one setting of the shown track, such as its volume or its send to bus A. Each command that sets it is a
// point at the tick at which the track ran it, at the height of its value. The value holds until the next point. A
// value that moves over a time slopes to it from the value before. Where the console starts a track with a value, the
// lane shows that value before the first point. A value from a random number or a variable has no one height. The lane
// leaves the line out up to the next point.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/commands/units.h"

#include "app/key_editor.h"
#include "edit/commands.h"
#include "format/file.h"
#include "song/archive.h"
#include "song/sequence.h"

namespace hokuto::app
{

// The name of `setting`, such as Volume, Send A or LFO 1 Depth.
std::string LaneName(LaneSetting setting);

// The settings that the lanes' menu offers, in groups. Only the List Editor edits the note wait, the timebase, the
// program and the bank select. The first two change the timing, and the others choose the instrument.
struct LaneGroup
{
    const char* name;
    std::vector<LaneSetting> settings;
};

std::span<const LaneGroup> LaneGroups();

// The setting that `command` sets, or nothing for a command that no lane shows.
std::optional<LaneSetting> SettingOf(const ntdlib::commands::Command& command);

// Whether `console` has commands of `setting`, and whether `track` runs any.
bool Holds(const song::Console& console, LaneSetting setting);
bool Sets(const KeyTrack& track, LaneSetting setting);

// The commands of the lanes' settings that `track` ran, from `song`, the sound as ntdlib's performer played it, and
// sequence `sequence` of `file`, its commands. They stop at the end of the loop's first pass. The arrangement stops
// there too. A command whose tick isn't known stays out (edit::CommandRows).
std::vector<KeyControl> ControlsOf(const format::File& file, uint32_t sequence, const song::Sequence& song,
                                   const song::Track& track);

// The setting that a new lane shows: the first of the menu's settings that the track sets and no lane shows, or else
// the first that the track's console holds and no lane shows.
LaneSetting NextLane(const KeyTrack& track, std::span<const LaneSetting> lanes);

// The value that the console gives a track's `setting` before a command sets it. ntdlib's performer starts a track with
// the same values. Nothing where the instrument sets it, such as the envelope's, or where the starting value isn't
// known.
std::optional<ntdlib::commands::Fixed> StartingValue(LaneSetting setting);

// The values that the lane of `setting` spans on `track`, in the setting's unit: the setting's usual range, widened to
// hold each value that the track's commands give it, within what the console's bytes hold.
edit::ValueRange LaneRange(const KeyTrack& track, LaneSetting setting);

// The value nearest `value` that `console` holds for `setting`, within `range`.
ntdlib::commands::Fixed NearestValue(const song::Console& console, LaneSetting setting, ntdlib::commands::Fixed value,
                                     const edit::ValueRange& range);

// A piece of a lane's line, from tick `start` to tick `end`, over which the value moves from `from` to `to`. A piece
// without values follows a value from a random number or a variable.
struct LanePiece
{
    friend bool operator==(const LanePiece&, const LanePiece&) = default;

    double start = 0.0;
    double end = 0.0;
    std::optional<ntdlib::commands::Fixed> from;
    std::optional<ntdlib::commands::Fixed> to;
};

// Where a lane shows the point of `track.controls[control]`: at `tick`, the tick at which the command's value is
// reached. A value that moves over a time is reached at the end of its slope, or where the next point stops it.
struct LanePoint
{
    friend bool operator==(const LanePoint&, const LanePoint&) = default;

    std::size_t control = 0;
    double tick = 0.0;
};

// The lane of `setting` on `track`: its line up to the song's end, and its points in the order that the track ran them.
// The line starts at tick 0 with the starting value, or else at the first point. A value that moves over a time starts
// from the value before. The next point can stop it halfway.
struct Lane
{
    std::vector<LanePiece> line;
    std::vector<LanePoint> points;
};

Lane LaneOf(const KeyTrack& track, LaneSetting setting);

// The commands that a drag of the point of `track.controls[pressed]` changes, by their offsets: the point's command,
// and when the point is selected, every selected command of its setting.
std::set<uint32_t> DraggedControls(const KeyTrack& track, const KeyEditorState& state, std::size_t pressed);

// The edit of a drag of the point of `track.controls[pressed]` to `value`. The commands of DraggedControls change by
// the same amount, each within what the console's bytes hold. A value from a random number or a variable stays. The
// Undo History names the edit Edit Controller. Nothing when no value changes.
std::optional<KeyEdit> LaneValueEdit(const KeyTrack& track, const KeyEditorState& state, std::size_t pressed,
                                     ntdlib::commands::Fixed value);

// The edit of a drag of the point of `track.controls[pressed]` by `shift` ticks and to `value`. The commands of
// DraggedControls move by the shift together, and their values change as LaneValueEdit changes them. A drag of a value
// from a random number or a variable moves it. The Undo History names the edit Edit Controller. Nothing when neither
// their ticks nor their values change.
std::optional<KeyEdit> LanePointEdit(const KeyTrack& track, const KeyEditorState& state, std::size_t pressed,
                                     int32_t shift, ntdlib::commands::Fixed value);

// Why the commands of DraggedControls can't move in time together, for the user, when SharedRange gives nothing for
// them.
std::string WhyPointsStay(const KeyTrack& track, const KeyEditorState& state, std::size_t pressed);

// The edit of a start typed for the point of `track.controls[index]`, at `tick`. The commands of DraggedControls move
// together, by the point's distance from the tick. When they can't move that far, or can't move at all, the edit says
// why. The Undo History names the edit Move. Nothing when the point starts at the tick.
std::optional<KeyEdit> LaneStartEdit(const KeyTrack& track, const KeyEditorState& state, std::size_t index,
                                     int64_t tick);

// The edit that puts a command of `setting` with `value` in at `tick`. The Undo History names it Edit Controller.
KeyEdit LaneDrawEdit(LaneSetting setting, uint32_t tick, ntdlib::commands::Fixed value);

// The edit that deletes the commands of `offsets`. The Undo History names it Delete.
KeyEdit LaneRemoval(const std::set<uint32_t>& offsets);

} // namespace hokuto::app
