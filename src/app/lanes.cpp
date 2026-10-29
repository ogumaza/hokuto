// SPDX-License-Identifier: MIT

#include "app/lanes.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/commands/units.h"

#include "app/key_edits.h"
#include "app/list_edits.h"
#include "edit/bytecode.h"
#include "edit/commands.h"
#include "edit/notes.h"

namespace hokuto::app
{
namespace
{

using ntdlib::commands::Argument;
using ntdlib::commands::Command;
using ntdlib::commands::Fixed;
using ntdlib::commands::Kind;
using ntdlib::commands::kOne;
using ntdlib::commands::Source;

// The number of the LFOs that a track has, and of the buses that it sends to.
constexpr uint8_t kLfos = 4;
constexpr uint8_t kBuses = 3;

// Whether a command of `kind` addresses a bus or an LFO by its index.
bool Addresses(Kind kind)
{
    switch (kind)
    {
    case Kind::kSend:
    case Kind::kLfoCurve:
    case Kind::kLfoPhase:
    case Kind::kLfoDepth:
    case Kind::kLfoSpeed:
    case Kind::kLfoType:
    case Kind::kLfoRange:
    case Kind::kLfoDelay:
    case Kind::kLfoPeriod:
        return true;
    default:
        return false;
    }
}

// The settings of LFO `lfo`, from 0.
std::vector<LaneSetting> LfoSettings(uint8_t lfo)
{
    std::vector<LaneSetting> settings;
    for (const Kind kind : {Kind::kLfoDepth, Kind::kLfoSpeed, Kind::kLfoRange, Kind::kLfoType, Kind::kLfoDelay,
                            Kind::kLfoCurve, Kind::kLfoPhase, Kind::kLfoPeriod})
    {
        settings.push_back({kind, lfo});
    }

    return settings;
}

// The values that a lane of `setting` spans before the track's values widen it, in the setting's unit. A setting whose
// values are a few choices spans them, such as the LFO's three types. A setting whose bytes reach far past their usual
// values spans those usual values: a pan from -64 to 63, a tempo up to 240 BPM, and a transpose of an octave either
// way.
std::pair<double, double> UsualRange(LaneSetting setting)
{
    if (Switches(setting.kind))
    {
        return {0.0, 1.0};
    }

    switch (setting.kind)
    {
    case Kind::kPan:
    case Kind::kInitialPan:
        return {-64.0, 63.0};
    case Kind::kTranspose:
    case Kind::kSweepPitch:
        return {-1200.0, 1200.0};
    case Kind::kBendRange:
        return {0.0, 1200.0};
    case Kind::kPitchBend:
    case Kind::kLowPass:
        return {-1.0, 1.0};
    case Kind::kTempo:
        return {0.0, 240.0};
    case Kind::kLfoDepth:
        return {0.0, 1.0};
    case Kind::kLfoSpeed:
        return {0.0, 25.0};
    case Kind::kLfoDelay:
    case Kind::kLfoPeriod:
        return {0.0, 1000.0};
    case Kind::kLfoRange:
        return {0.0, 16.0};
    case Kind::kLfoType:
        return {0.0, 2.0};
    case Kind::kLfoCurve:
        return {0.0, 4.0};
    case Kind::kMute:
        return {0.0, 3.0};
    case Kind::kBiquadType:
        return {0.0, 5.0};
    default:
        return {0.0, 127.0};
    }
}

// A fixed-point value of `units`.
Fixed FixedOf(double units)
{
    return static_cast<Fixed>(std::llround(units * kOne));
}

// A command of `setting` with `value`.
Command CommandOf(LaneSetting setting, Fixed value)
{
    Command command;
    command.kind = setting.kind;
    command.index = setting.index;
    command.value = Argument{Source::kValue, value, 0};

    return command;
}

} // namespace

std::string LaneName(LaneSetting setting)
{
    const std::string kind(KindName(setting.kind));
    if (setting.kind == Kind::kSend)
    {
        return kind + " " + static_cast<char>('A' + std::min<int>(setting.index, kBuses - 1));
    }
    if (Addresses(setting.kind))
    {
        // "LFO Depth" becomes "LFO 1 Depth" for LFO 1. The LFOs count from 0. The commands address them that way.
        return "LFO " + std::to_string(setting.index) + kind.substr(3);
    }

    return kind;
}

std::span<const LaneGroup> LaneGroups()
{
    static const std::vector<LaneGroup> kGroups = {
        {"Mix",
         {{Kind::kVolume},
          {Kind::kVolume2},
          {Kind::kPan},
          {Kind::kInitialPan},
          {Kind::kSurroundPan},
          {Kind::kSend, 0},
          {Kind::kSend, 1},
          {Kind::kSend, 2},
          {Kind::kMainSend},
          {Kind::kFrontBypass},
          {Kind::kMute}}},
        {"Pitch",
         {{Kind::kPitchBend},
          {Kind::kBendRange},
          {Kind::kTranspose},
          {Kind::kSweepPitch},
          {Kind::kPortamento},
          {Kind::kPortamentoFrom},
          {Kind::kPortamentoTime}}},
        {"Sequence", {{Kind::kTempo}, {Kind::kMainVolume}}},
        {"Instrument", {{Kind::kPriority}, {Kind::kVelocityRange}, {Kind::kMonophonic}, {Kind::kTie}, {Kind::kDamper}}},
        {"Envelope", {{Kind::kAttack}, {Kind::kDecay}, {Kind::kSustain}, {Kind::kRelease}, {Kind::kEnvelopeHold}}},
        {"Filter", {{Kind::kLowPass}, {Kind::kBiquadType}, {Kind::kBiquadValue}}},
        {"LFO 0", LfoSettings(0)},
        {"LFO 1", LfoSettings(1)},
        {"LFO 2", LfoSettings(2)},
        {"LFO 3", LfoSettings(3)},
    };
    return kGroups;
}

std::optional<LaneSetting> SettingOf(const Command& command)
{
    // The kinds that the menu offers, once.
    static const std::array<bool, ntdlib::commands::kKindCount> kOffered = []
    {
        std::array<bool, ntdlib::commands::kKindCount> offered{};
        for (const LaneGroup& group : LaneGroups())
        {
            for (const LaneSetting setting : group.settings)
            {
                offered[static_cast<std::size_t>(setting.kind)] = true;
            }
        }

        return offered;
    }();

    const auto kind = static_cast<std::size_t>(command.kind);
    const uint8_t limit = command.kind == Kind::kSend ? kBuses : kLfos;
    if (kind >= kOffered.size() || !kOffered[kind] || (Addresses(command.kind) && command.index >= limit))
    {
        return std::nullopt;
    }

    return LaneSetting{command.kind, Addresses(command.kind) ? command.index : uint8_t{0}};
}

bool Holds(const song::Console& console, LaneSetting setting)
{
    return edit::ArgumentRange(console, CommandOf(setting, 0), Source::kValue).has_value();
}

bool Sets(const KeyTrack& track, LaneSetting setting)
{
    return std::any_of(track.controls.begin(), track.controls.end(),
                       [setting](const KeyControl& control) { return SettingOf(control.command) == setting; });
}

std::vector<KeyControl> ControlsOf(const format::File& file, uint32_t sequence, const song::Sequence& song,
                                   const song::Track& track)
{
    const ntdlib::commands::Sequence& commands = edit::SequenceToEdit(file, sequence);
    const std::vector<uint32_t> offsets = edit::Offsets(commands, edit::ConsoleOf(file));
    const std::optional<uint32_t> end = song.loop ? std::optional<uint32_t>(song.loop->end) : std::nullopt;
    std::vector<KeyControl> controls;
    for (const edit::CommandRow& row : edit::CommandRows(file, sequence, track.stretches, track.notes, end))
    {
        const std::optional<uint32_t> item = edit::ItemAtOffset(offsets, row.command);
        if (!row.tick || !item || *item >= commands.items.size() || !SettingOf(commands.items[*item]))
        {
            continue;
        }

        controls.push_back({commands.items[*item], row.command, *row.tick, row.stretch});
    }

    return controls;
}

LaneSetting NextLane(const KeyTrack& track, std::span<const LaneSetting> lanes)
{
    const auto shown = [&lanes](LaneSetting setting)
    {
        return std::find(lanes.begin(), lanes.end(), setting) != lanes.end();
    };

    // A setting that the track sets comes first.
    std::optional<LaneSetting> unshown;
    for (const LaneGroup& group : LaneGroups())
    {
        for (const LaneSetting setting : group.settings)
        {
            if (shown(setting) || !Holds(track.console, setting))
            {
                continue;
            }
            if (Sets(track, setting))
            {
                return setting;
            }

            if (!unshown)
            {
                unshown = setting;
            }
        }
    }

    return unshown.value_or(LaneSetting{});
}

std::optional<Fixed> StartingValue(LaneSetting setting)
{
    switch (setting.kind)
    {
    case Kind::kVolume:
    case Kind::kVolume2:
    case Kind::kMainVolume:
    case Kind::kMainSend:
    case Kind::kVelocityRange:
        return 127 * kOne;
    case Kind::kBendRange:
        return 200 * kOne;
    case Kind::kPriority:
        return 64 * kOne;
    case Kind::kTempo:
        return 120 * kOne;
    case Kind::kLfoSpeed:
        return FixedOf(6.25);
    case Kind::kLfoRange:
        return kOne;
    case Kind::kPan:
    case Kind::kInitialPan:
    case Kind::kSurroundPan:
    case Kind::kSend:
    case Kind::kPitchBend:
    case Kind::kTranspose:
    case Kind::kSweepPitch:
    case Kind::kLowPass:
    case Kind::kBiquadType:
    case Kind::kBiquadValue:
    case Kind::kTie:
    case Kind::kMonophonic:
    case Kind::kDamper:
    case Kind::kPortamento:
    case Kind::kFrontBypass:
    case Kind::kMute:
    case Kind::kLfoDepth:
    case Kind::kLfoDelay:
    case Kind::kLfoCurve:
    case Kind::kLfoPhase:
    case Kind::kLfoType:
        return 0;
    default:
        return std::nullopt;
    }
}

edit::ValueRange LaneRange(const KeyTrack& track, LaneSetting setting)
{
    // The usual range, widened to hold the starting value and the track's values.
    const auto [usual_low, usual_high] = UsualRange(setting);
    Fixed low = FixedOf(usual_low);
    Fixed high = FixedOf(usual_high);
    const auto widen = [&low, &high](Fixed value)
    {
        low = std::min(low, value);
        high = std::max(high, value);
    };
    if (const std::optional<Fixed> start = StartingValue(setting))
    {
        widen(*start);
    }
    for (const KeyControl& control : track.controls)
    {
        const Argument& value = control.command.value;
        if (SettingOf(control.command) != setting || value.source == Source::kVariable)
        {
            continue;
        }

        widen(value.value);
        if (value.source == Source::kRandom)
        {
            widen(value.high);
        }
    }

    // Within the console's range.
    if (const std::optional<edit::ValueRange> console =
            edit::ArgumentRange(track.console, CommandOf(setting, 0), Source::kValue))
    {
        low = std::clamp(low, console->lowest, console->highest);
        high = std::clamp(high, console->lowest, console->highest);
    }
    if (high <= low)
    {
        high = low + kOne;
    }

    return {low, high};
}

Fixed NearestValue(const song::Console& console, LaneSetting setting, Fixed value, const edit::ValueRange& range)
{
    const Command nearest = edit::Nearest(console, CommandOf(setting, std::clamp(value, range.lowest, range.highest)));
    return std::clamp(nearest.value.value, range.lowest, range.highest);
}

Lane LaneOf(const KeyTrack& track, LaneSetting setting)
{
    std::vector<std::size_t> points;
    for (std::size_t i = 0; i < track.controls.size(); i++)
    {
        if (SettingOf(track.controls[i].command) == setting)
        {
            points.push_back(i);
        }
    }

    // The value from each point holds up to the next point, or up to the song's end. It moves there first over the
    // point's time, from the value before.
    Lane lane;
    std::optional<Fixed> value = StartingValue(setting);
    const double first = points.empty() ? static_cast<double>(track.song_length) : track.controls[points[0]].tick;
    if (value && first > 0.0)
    {
        lane.line.push_back({0.0, first, value, value});
    }
    for (std::size_t i = 0; i < points.size(); i++)
    {
        const KeyControl& point = track.controls[points[i]];
        const double start = point.tick;
        const double end =
            i + 1 < points.size() ? track.controls[points[i + 1]].tick : std::max<double>(track.song_length, start);
        const Argument& argument = point.command.value;
        if (argument.source != Source::kValue)
        {
            value.reset();
            lane.line.push_back({start, end, std::nullopt, std::nullopt});
            lane.points.push_back({points[i], start});
            continue;
        }

        // A value that moves over a time.
        const std::optional<Argument>& time = point.command.time;
        const double ticks = time && time->source == Source::kValue ? static_cast<double>(time->value) / kOne : 0.0;
        if (value && ticks > 0.0 && end > start)
        {
            const double ramp_end = std::min(start + ticks, end);
            const double amount = (ramp_end - start) / ticks;
            const Fixed reached = *value + FixedOf(static_cast<double>(argument.value - *value) / kOne * amount);
            lane.line.push_back({start, ramp_end, value, reached});
            lane.points.push_back({points[i], ramp_end});
            value = reached;
            if (ramp_end < end)
            {
                value = argument.value;
                lane.line.push_back({ramp_end, end, value, value});
            }
            continue;
        }

        value = argument.value;
        lane.points.push_back({points[i], start});
        if (end > start)
        {
            lane.line.push_back({start, end, value, value});
        }
    }

    return lane;
}

std::set<uint32_t> DraggedControls(const KeyTrack& track, const KeyEditorState& state, std::size_t pressed)
{
    const KeyControl& point = track.controls.at(pressed);
    std::set<uint32_t> offsets = {point.offset};
    if (!state.selected_controls.contains(point.offset))
    {
        return offsets;
    }

    const std::optional<LaneSetting> setting = SettingOf(point.command);
    for (const KeyControl& control : track.controls)
    {
        if (state.selected_controls.contains(control.offset) && SettingOf(control.command) == setting)
        {
            offsets.insert(control.offset);
        }
    }

    return offsets;
}

std::optional<KeyEdit> LaneValueEdit(const KeyTrack& track, const KeyEditorState& state, std::size_t pressed,
                                     Fixed value)
{
    const KeyControl& point = track.controls.at(pressed);
    const std::optional<LaneSetting> setting = SettingOf(point.command);
    if (!setting || point.command.value.source != Source::kValue || value == point.command.value.value)
    {
        return std::nullopt;
    }

    // Each command changes once: the pressed one to the value, and with it the other selected commands of its setting.
    const edit::ValueRange console =
        edit::ArgumentRange(track.console, point.command, Source::kValue).value_or(edit::ValueRange{value, value});
    const Fixed change = value - point.command.value.value;
    const std::set<uint32_t> dragged = DraggedControls(track, state, pressed);
    KeyEdit edit;
    edit.name = "Edit Controller";
    for (const KeyControl& control : track.controls)
    {
        if (!dragged.contains(control.offset) || control.command.value.source != Source::kValue ||
            edit.changed_controls.contains(control.offset))
        {
            continue;
        }

        Command command = control.command;
        command.value.value = control.offset == point.offset
                                  ? value
                                  : NearestValue(track.console, *setting, command.value.value + change, console);
        if (command.value != control.command.value)
        {
            edit.changed_controls.emplace(control.offset, std::move(command));
        }
    }
    if (edit.changed_controls.empty())
    {
        return std::nullopt;
    }

    return edit;
}

std::optional<KeyEdit> LanePointEdit(const KeyTrack& track, const KeyEditorState& state, std::size_t pressed,
                                     int32_t shift, Fixed value)
{
    std::optional<KeyEdit> edit = LaneValueEdit(track, state, pressed, value);
    if (shift == 0)
    {
        return edit;
    }

    if (!edit)
    {
        edit.emplace();
    }
    edit->name = "Edit Controller";
    for (const uint32_t offset : DraggedControls(track, state, pressed))
    {
        edit->moved_controls.emplace(offset, shift);
    }

    return edit;
}

std::string WhyPointsStay(const KeyTrack& track, const KeyEditorState& state, std::size_t pressed)
{
    const std::set<uint32_t> dragged = DraggedControls(track, state, pressed);
    for (const KeyControl& control : track.controls)
    {
        if (!dragged.contains(control.offset))
        {
            continue;
        }

        if (const std::optional<std::string> why = edit::WhyFixedInTime(control.command))
        {
            return *why;
        }
    }

    return std::string(kTimedByNotes);
}

std::optional<KeyEdit> LaneStartEdit(const KeyTrack& track, const KeyEditorState& state, std::size_t index,
                                     int64_t tick)
{
    const KeyControl& point = track.controls.at(index);
    const int64_t shift = tick - int64_t{point.tick};
    if (shift == 0)
    {
        return std::nullopt;
    }

    // The commands move only as far as the command with the least room.
    KeyEdit edit;
    edit.name = "Move";
    const std::optional<edit::ShiftRange> range = SharedRange(state, DraggedControls(track, state, index));
    if (!range)
    {
        edit.refused = WhyPointsStay(track, state, index);
        return edit;
    }
    if (shift < -int64_t{range->earlier} || shift > int64_t{range->later})
    {
        const bool early = shift < 0;
        const int64_t bound = int64_t{point.tick} + (early ? -int64_t{range->earlier} : int64_t{range->later});
        const double quarter = std::max(track.ticks_per_quarter, 1);
        edit.refused = std::string("The point can start no ") + (early ? "earlier" : "later") + " than " +
                       PlainPosition(static_cast<double>(bound) / quarter) +
                       ". A command stays between the jumps, calls and loop points before and after it.";
        return edit;
    }

    // A typed start makes a Move. A drag makes an Edit Controller.
    std::optional<KeyEdit> moved =
        LanePointEdit(track, state, index, static_cast<int32_t>(shift), point.command.value.value);
    if (moved)
    {
        moved->name = "Move";
    }

    return moved;
}

KeyEdit LaneDrawEdit(LaneSetting setting, uint32_t tick, Fixed value)
{
    KeyEdit edit;
    edit.name = "Edit Controller";
    edit.added_controls.emplace_back(tick, CommandOf(setting, value));

    return edit;
}

KeyEdit LaneRemoval(const std::set<uint32_t>& offsets)
{
    KeyEdit edit;
    edit.name = "Delete";
    edit.removed_controls = offsets;

    return edit;
}

} // namespace hokuto::app
