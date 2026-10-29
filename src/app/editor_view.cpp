// SPDX-License-Identifier: MIT

#include "app/editor_view.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <optional>
#include <span>

#include "app/lanes.h"

namespace hokuto::app
{
namespace
{

// Track `number` as the view holds it, or nothing for a number past the view's sets.
std::optional<uint8_t> ViewTrack(int number)
{
    if (number < 0 || number > format::kLastViewTrack)
    {
        return std::nullopt;
    }

    return static_cast<uint8_t>(number);
}

// Whether the lanes' menu offers `setting`.
bool Offered(LaneSetting setting)
{
    const auto holds = [setting](const LaneGroup& group)
    {
        return std::ranges::find(group.settings, setting) != group.settings.end();
    };
    return std::ranges::any_of(LaneGroups(), holds);
}

// The quarter notes of `kept`, a position that a float keeps. The float rounds the position that the program held to
// its nearest float. A position within a float's step of a tick of the sound, or of a tick of the display, goes back to
// that tick. 335 ticks at 48 to the quarter come back as 6.9791665 quarter notes, a tick early in the display.
double Unrounded(float kept, int ticks_per_quarter)
{
    const float next = std::nextafter(kept, std::numeric_limits<float>::infinity());
    const double step = static_cast<double>(next) - static_cast<double>(kept);
    for (const int ticks : {ticks_per_quarter, kDisplayTicksPerQuarter})
    {
        if (ticks <= 0)
        {
            continue;
        }

        const double tick = std::round(static_cast<double>(kept) * ticks) / ticks;
        if (std::abs(tick - static_cast<double>(kept)) <= step)
        {
            return tick;
        }
    }

    return kept;
}

} // namespace

format::EditorView ViewOf(uint32_t sound, std::span<const int> numbers, const Transport& transport,
                          const ArrangementState& arrangement, const SongView& song, const KeyEditorState& key_editor,
                          const OpenEditors& editors)
{
    // The sound and the transport.
    format::EditorView view;
    view.sound = sound;
    view.position = static_cast<float>(transport.Position());
    view.cycle_start = static_cast<float>(transport.CycleStart());
    view.cycle_end = static_cast<float>(transport.CycleEnd());
    view.cycle = transport.Cycle();

    // The arrangement.
    view.beat_width = arrangement.beat_width;
    view.track_height = arrangement.track_height;
    view.left = static_cast<float>(arrangement.left_quarter);
    view.top = arrangement.top_track;
    for (std::size_t i = 0; i < numbers.size() && i < song.tracks.size(); i++)
    {
        const std::optional<uint8_t> track = ViewTrack(numbers[i]);
        if (!track)
        {
            continue;
        }

        if (static_cast<int>(i) == arrangement.selected_track)
        {
            view.selected_track = track;
        }
        view.muted |= song.tracks[i].mute ? uint32_t{1} << *track : 0;
        view.soloed |= song.tracks[i].solo ? uint32_t{1} << *track : 0;
    }

    // The editors.
    view.key_beat_width = key_editor.beat_width;
    view.key_height = key_editor.key_height;
    view.key_left = static_cast<float>(key_editor.left_quarter);
    view.key_top = key_editor.top_key;
    for (const LaneSetting& lane : key_editor.lanes)
    {
        view.lanes.push_back({lane.kind, lane.index});
    }
    view.key_editor_track = editors.key_editor ? ViewTrack(*editors.key_editor) : std::nullopt;
    view.list_editor_track = editors.list_editor ? ViewTrack(*editors.list_editor) : std::nullopt;
    view.front = editors.front;

    return view;
}

OpenEditors ApplyView(const format::EditorView& view, std::span<const int> numbers, Transport& transport,
                      ArrangementState& arrangement, SongView& song, KeyEditorState& key_editor)
{
    // The transport, at the ticks that the floats rounded.
    const int ticks = song.ticks_per_quarter;
    transport.SetLocators(Unrounded(view.cycle_start, ticks), Unrounded(view.cycle_end, ticks));
    transport.SetCycle(view.cycle);
    transport.SetPosition(Unrounded(view.position, ticks));

    // The arrangement. A track past the view's sets plays, and isn't selected.
    arrangement.beat_width = view.beat_width;
    arrangement.track_height = view.track_height;
    arrangement.scroll_to = {view.left, view.top};
    for (std::size_t i = 0; i < numbers.size() && i < song.tracks.size(); i++)
    {
        const std::optional<uint8_t> track = ViewTrack(numbers[i]);
        song.tracks[i].mute = track && (view.muted >> *track & 1) != 0;
        song.tracks[i].solo = track && (view.soloed >> *track & 1) != 0;
        if (track && view.selected_track == track)
        {
            arrangement.selected_track = static_cast<int>(i);
        }
    }

    // The Key Editor keeps its zoom, its scroll and its lanes while it's closed. A lane of a setting that the lanes'
    // menu doesn't offer stays out.
    key_editor.beat_width = view.key_beat_width;
    key_editor.key_height = view.key_height;
    key_editor.scroll_to = {view.key_left, view.key_top};
    key_editor.lanes.clear();
    for (const format::ViewLane& lane : view.lanes)
    {
        const LaneSetting setting{lane.kind, lane.index};
        if (Offered(setting))
        {
            key_editor.lanes.push_back(setting);
        }
    }

    // The editors open on tracks that the sound has.
    const auto shown = [numbers](std::optional<uint8_t> track) -> std::optional<int>
    {
        if (!track || std::ranges::find(numbers, int{*track}) == numbers.end())
        {
            return std::nullopt;
        }

        return *track;
    };

    // The window in front, or the first of the List Editor, the Key Editor and the mixer that opens.
    OpenEditors editors{shown(view.key_editor_track), shown(view.list_editor_track), view.front};
    const bool opens = editors.front == format::ViewFront::kMixer ||
                       (editors.front == format::ViewFront::kKeyEditor && editors.key_editor) ||
                       (editors.front == format::ViewFront::kListEditor && editors.list_editor);
    if (!opens)
    {
        editors.front = editors.list_editor  ? format::ViewFront::kListEditor
                        : editors.key_editor ? format::ViewFront::kKeyEditor
                                             : format::ViewFront::kMixer;
    }

    return editors;
}

} // namespace hokuto::app
