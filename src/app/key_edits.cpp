// SPDX-License-Identifier: MIT

#include "app/key_edits.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <iterator>
#include <limits>
#include <string>
#include <system_error>
#include <utility>

namespace hokuto::app
{
namespace
{

// The names of the keys of an octave, from C.
constexpr std::array<const char*, 12> kKeyNames = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

// The semitones above C of each letter of the keys' names, from A.
constexpr std::array<int, 7> kLetters = {9, 11, 0, 2, 4, 5, 7};

// A velocity that the user typed: a whole number from 1 to 127, or nothing.
std::optional<int> ParseVelocity(std::string_view text)
{
    while (!text.empty() && text.front() == ' ')
    {
        text.remove_prefix(1);
    }
    while (!text.empty() && text.back() == ' ')
    {
        text.remove_suffix(1);
    }

    int velocity = 0;
    const bool digits = !text.empty() && text.size() <= 3 &&
                        std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; });
    if (!digits || std::from_chars(text.data(), text.data() + text.size(), velocity).ec != std::errc() ||
        velocity < 1 || velocity > kTopVelocity)
    {
        return std::nullopt;
    }

    return velocity;
}

// Whether every selected command has its limits. A sequence that Hokuto can't edit gives none. An edit of its notes
// then says why (App::UpdateKeyTrack).
bool LimitsKnown(const KeyEditorState& state)
{
    return std::all_of(state.selected.begin(), state.selected.end(),
                       [&state](uint32_t command) { return state.limits.contains(command); });
}

// The edit that moves the selected notes together, with `first` to `start`, in ticks. `quarter` is the ticks of a
// quarter note. Nothing when `first` starts there.
std::optional<KeyEdit> Moving(const KeyEditorState& state, const KeyNote& first, int64_t start, double quarter)
{
    const int64_t shift = start - int64_t{first.start};
    if (shift == 0)
    {
        return std::nullopt;
    }

    // The notes move only as far as the note with the least room.
    KeyEdit edit;
    edit.name = "Move";
    const std::optional<edit::ShiftRange> range = SharedRange(state, state.selected);
    if (!range && LimitsKnown(state))
    {
        edit.refused = "This track's notes set its timing. Hokuto can't move them in time yet.";
        return edit;
    }
    if (range && (shift < -int64_t{range->earlier} || shift > int64_t{range->later}))
    {
        const bool early = shift < 0;
        const int64_t bound = int64_t{first.start} + (early ? -int64_t{range->earlier} : int64_t{range->later});
        edit.refused = std::string("The first selected note can start no ") + (early ? "earlier" : "later") + " than " +
                       PlainPosition(static_cast<double>(bound) / quarter) +
                       ". A note stays between the jumps, calls and loop points before and after it.";
        return edit;
    }
    for (const uint32_t command : state.selected)
    {
        edit.changes.push_back({.command = command, .shift = static_cast<int32_t>(shift)});
    }

    return edit;
}

// The edit that sizes the selected notes together, with the command of `first` to `length` ticks. A note can sound for
// less than its command's length, such as a one-shot wave's note or a note cut at the loop's end. The other commands
// change by the same amount. Nothing when `first` shows that length already, or when its command has it.
std::optional<KeyEdit> Sizing(const KeyTrack& track, const KeyEditorState& state, const KeyNote& first, int64_t length)
{
    if (length == int64_t{first.length})
    {
        return std::nullopt;
    }

    // A note lasts a tick at least.
    KeyEdit edit;
    edit.name = "Size";
    if (length < 1)
    {
        edit.refused = "A note lasts a tick at least. Its end comes after its start.";
        return edit;
    }
    std::optional<std::map<uint32_t, uint32_t>> lengths = BaseLengths(track, state);
    if (!lengths && LimitsKnown(state))
    {
        edit.refused = "This track's notes set its timing. Hokuto can't change their lengths yet.";
        return edit;
    }

    // Without limits, the first note of each command gives the length.
    if (!lengths)
    {
        lengths.emplace();
        for (const KeyNote& note : track.notes)
        {
            if (state.selected.contains(note.command))
            {
                lengths->emplace(note.command, note.length);
            }
        }
    }

    // Each command's length changes by the same amount.
    const auto base = lengths->find(first.command);
    const int64_t resize = length - (base != lengths->end() ? int64_t{base->second} : int64_t{first.length});
    if (resize == 0)
    {
        return std::nullopt;
    }
    for (const auto& [command, old] : *lengths)
    {
        const int64_t kMost = std::numeric_limits<uint32_t>::max();
        const int64_t changed = std::clamp<int64_t>(int64_t{old} + resize, 1, kMost);
        edit.changes.push_back({.command = command, .length = static_cast<uint32_t>(changed)});
    }

    return edit;
}

// Whether `gesture` holds one of the track's notes (KeyGesture::note): a press on a note, its move or its size, or a
// drag of its velocity. A note being drawn isn't one of the track's notes yet.
bool HoldsNote(const KeyGesture& gesture)
{
    return gesture.kind == KeyGesture::Kind::kPress || gesture.kind == KeyGesture::Kind::kMove ||
           gesture.kind == KeyGesture::Kind::kSize || gesture.kind == KeyGesture::Kind::kVelocity;
}

} // namespace

std::string KeyName(int key)
{
    return std::string(kKeyNames[static_cast<std::size_t>(key % 12)]) + std::to_string(key / 12 - 2);
}

std::optional<int> ParseKeyName(std::string_view text)
{
    while (!text.empty() && text.front() == ' ')
    {
        text.remove_prefix(1);
    }
    while (!text.empty() && text.back() == ' ')
    {
        text.remove_suffix(1);
    }

    // The letter, in either case, then a sharp or a flat, then the octave.
    char letter = text.empty() ? '\0' : text.front();
    if (letter >= 'a' && letter <= 'g')
    {
        letter = static_cast<char>(letter - 'a' + 'A');
    }
    if (letter < 'A' || letter > 'G')
    {
        return std::nullopt;
    }
    text.remove_prefix(1);
    int key = kLetters[static_cast<std::size_t>(letter - 'A')];
    for (const auto& [sign, semitones] :
         {std::pair<std::string_view, int>{"#", 1}, {"\u266F", 1}, {"b", -1}, {"\u266D", -1}})
    {
        if (text.starts_with(sign))
        {
            text.remove_prefix(sign.size());
            key += semitones;
            break;
        }
    }
    int octave = 0;
    const bool digits = !text.empty() && std::all_of(text.begin() + (text.front() == '-' ? 1 : 0), text.end(),
                                                     [](char c) { return c >= '0' && c <= '9'; });
    if (!digits || text.size() > 3 || std::from_chars(text.data(), text.data() + text.size(), octave).ec != std::errc())
    {
        return std::nullopt;
    }
    key += (octave + 2) * 12;

    return key >= 0 && key < kKeys ? std::optional<int>(key) : std::nullopt;
}

std::string PlainPosition(double quarters)
{
    return WithoutFigureSpaces(PositionText(quarters, kBeatsPerBar));
}

double GridTicks(const KeyEditorState& state, int ticks_per_quarter)
{
    return ticks_per_quarter * 4.0 / state.grid;
}

int64_t Snapped(const KeyEditorState& state, int ticks_per_quarter, double tick, bool down)
{
    const double step = state.snap ? GridTicks(state, ticks_per_quarter) : 1.0;
    const double lines = down ? std::floor(tick / step) : std::round(tick / step);
    return std::llround(lines * step);
}

double RulerPosition(const KeyEditorState& state, int ticks_per_quarter, double tick)
{
    const double clicked = std::max(tick, 0.0);
    const double placed = state.snap ? static_cast<double>(Snapped(state, ticks_per_quarter, clicked, false)) : clicked;
    return placed / ticks_per_quarter;
}

std::optional<edit::ShiftRange> SharedRange(const KeyEditorState& state, const std::set<uint32_t>& commands)
{
    edit::ShiftRange shared{std::numeric_limits<uint32_t>::max(), std::numeric_limits<uint32_t>::max()};
    for (const uint32_t command : commands)
    {
        const auto limits = state.limits.find(command);
        if (limits == state.limits.end() || !limits->second.shift)
        {
            return std::nullopt;
        }

        shared.earlier = std::min(shared.earlier, limits->second.shift->earlier);
        shared.later = std::min(shared.later, limits->second.shift->later);
    }

    return shared;
}

std::optional<std::map<uint32_t, uint32_t>> BaseLengths(const KeyTrack& track, const KeyEditorState& state)
{
    std::map<uint32_t, uint32_t> lengths;
    for (const uint32_t command : state.selected)
    {
        const auto limits = state.limits.find(command);
        if (limits == state.limits.end() || !limits->second.length)
        {
            return std::nullopt;
        }

        lengths[command] = *limits->second.length;
    }
    for (const KeyNote& note : track.notes)
    {
        const auto length = lengths.find(note.command);
        if (length != lengths.end() && length->second == 0)
        {
            length->second = std::max(note.length, 1u);
        }
    }

    return lengths;
}

std::map<uint32_t, int> Velocities(const KeyTrack& track, const KeyEditorState& state)
{
    std::map<uint32_t, int> velocities;
    for (const KeyNote& note : track.notes)
    {
        if (state.selected.contains(note.command))
        {
            velocities.emplace(note.command, note.velocity);
        }
    }

    return velocities;
}

std::pair<int, int> TransposeRange(const KeyTrack& track, const KeyEditorState& state)
{
    int down = kKeys - 1;
    int up = kKeys - 1;
    for (const KeyNote& note : track.notes)
    {
        if (state.selected.contains(note.command))
        {
            down = std::min(down, note.key);
            up = std::min(up, kKeys - 1 - note.key);
        }
    }

    return {-down, up};
}

KeyEdit Removal(const std::set<uint32_t>& commands)
{
    KeyEdit edit;
    edit.name = "Delete";
    for (const uint32_t command : commands)
    {
        edit.changes.push_back({.command = command, .remove = true});
    }

    return edit;
}

std::optional<KeyEdit> Transposition(const KeyTrack& track, const KeyEditorState& state, int semitones)
{
    const auto [down, up] = TransposeRange(track, state);
    if (state.selected.empty() || semitones < down || semitones > up)
    {
        return std::nullopt;
    }

    KeyEdit edit;
    edit.name = semitones < 0 ? "Down" : "Up";
    for (const uint32_t command : state.selected)
    {
        edit.changes.push_back({.command = command, .transpose = semitones});
    }

    return edit;
}

std::optional<KeyEdit> Quantization(const KeyTrack& track, const KeyEditorState& state)
{
    // Each command moves by its first note's distance from the nearest line. A sequence that Hokuto can't edit has no
    // limits. Its notes go into the edit, and the sequence then refuses the edit with a message.
    const double step = GridTicks(state, std::max(track.ticks_per_quarter, 1));
    KeyEdit edit;
    edit.name = "Quantize";
    std::set<uint32_t> seen;
    bool timed = false;
    bool kept = false;
    for (const KeyNote& note : track.notes)
    {
        if ((!state.selected.empty() && !state.selected.contains(note.command)) || !seen.insert(note.command).second)
        {
            continue;
        }

        const int64_t line = std::llround(std::round(note.start / step) * step);
        const int64_t shift = line - static_cast<int64_t>(note.start);
        if (shift == 0)
        {
            continue;
        }

        const auto limits = state.limits.find(note.command);
        if (limits != state.limits.end() && !limits->second.shift)
        {
            timed = true;
            continue;
        }
        if (limits != state.limits.end() && (shift < -static_cast<int64_t>(limits->second.shift->earlier) ||
                                             shift > static_cast<int64_t>(limits->second.shift->later)))
        {
            kept = true;
            continue;
        }
        edit.changes.push_back({.command = note.command, .shift = static_cast<int32_t>(shift)});
    }

    // Notes that can't move stay where they are. When none can, the edit says why.
    if (!edit.changes.empty())
    {
        return edit;
    }
    if (timed)
    {
        edit.refused = "This track's notes set its timing. Hokuto can't move them in time yet.";
        return edit;
    }
    if (kept)
    {
        edit.refused =
            "Hokuto can't move these notes onto the grid. A note stays between the jumps, calls and loop "
            "points before and after it.";
        return edit;
    }

    return std::nullopt;
}

std::vector<KeyNote> CopiedNotes(const KeyTrack& track, const KeyEditorState& state)
{
    // The track's notes come in the order that they start.
    std::vector<KeyNote> copied;
    std::set<uint32_t> seen;
    for (const KeyNote& note : track.notes)
    {
        if (state.selected.contains(note.command) && seen.insert(note.command).second)
        {
            copied.push_back(note);
        }
    }
    const uint32_t first = copied.empty() ? 0 : copied.front().start;
    for (KeyNote& note : copied)
    {
        note.start -= first;
    }

    return copied;
}

std::optional<KeyEdit> Pasting(const std::vector<KeyNote>& clipboard, uint32_t tick)
{
    if (clipboard.empty())
    {
        return std::nullopt;
    }

    KeyEdit edit;
    edit.name = "Paste";
    for (KeyNote note : clipboard)
    {
        note.start += tick;
        edit.added.push_back(note);
    }

    return edit;
}

std::optional<KeyEdit> RequestedEdit(const KeyTrack& track, KeyEditorState& state, double position)
{
    switch (std::exchange(state.request, KeyRequest::kNone))
    {
    case KeyRequest::kQuantize:
        return Quantization(track, state);

    case KeyRequest::kCut:
        {
            if (state.selected.empty())
            {
                return std::nullopt;
            }

            // The Undo History names a cut Delete.
            state.clipboard = CopiedNotes(track, state);
            return Removal(state.selected);
        }

    case KeyRequest::kCopy:
        if (!state.selected.empty())
        {
            state.clipboard = CopiedNotes(track, state);
        }
        return std::nullopt;

    case KeyRequest::kPaste:
        {
            const double kMost = std::numeric_limits<uint32_t>::max();
            return Pasting(state.clipboard, static_cast<uint32_t>(std::llround(std::clamp(position, 0.0, kMost))));
        }

    // Select All selects every note, and no point of the controller lanes.
    case KeyRequest::kSelectAll:
        state.selected_controls.clear();
        for (const KeyNote& note : track.notes)
        {
            state.selected.insert(note.command);
        }
        return std::nullopt;

    case KeyRequest::kNone:
        return std::nullopt;
    }

    return std::nullopt;
}

std::optional<KeyEdit> TypedEdit(const KeyTrack& track, const KeyEditorState& state, InfoValue value,
                                 std::string_view text)
{
    const auto first = std::find_if(track.notes.begin(), track.notes.end(),
                                    [&state](const KeyNote& note) { return state.selected.contains(note.command); });
    if (first == track.notes.end())
    {
        return std::nullopt;
    }

    // A typed position or length in the track's ticks.
    const double quarter = std::max(track.ticks_per_quarter, 1);
    const auto ticks = [quarter](std::optional<double> quarters) -> std::optional<int64_t>
    {
        return quarters ? std::optional<int64_t>(std::llround(*quarters * quarter)) : std::nullopt;
    };

    switch (value)
    {
    case InfoValue::kStart:
        {
            const std::optional<int64_t> start = ticks(ParsePosition(text, kBeatsPerBar));
            return start ? Moving(state, *first, *start, quarter)
                         : Unreadable(text, "a start", PositionHint("A start"));
        }

    case InfoValue::kEnd:
        {
            const std::optional<int64_t> end = ticks(ParsePosition(text, kBeatsPerBar));
            return end ? Sizing(track, state, *first, *end - int64_t{first->start})
                       : Unreadable(text, "an end", PositionHint("An end"));
        }

    case InfoValue::kLength:
        {
            const std::optional<int64_t> length = ticks(ParseLength(text, kBeatsPerBar));
            return length ? Sizing(track, state, *first, *length) : Unreadable(text, "a length", LengthHint());
        }

    case InfoValue::kPitch:
        {
            const std::optional<int> key = ParseKeyName(text);
            if (!key)
            {
                return Unreadable(text, "a pitch", "A pitch is a key's name from C-2 to G8, such as C3 for middle C.");
            }
            if (*key == first->key)
            {
                return std::nullopt;
            }

            std::optional<KeyEdit> edit = Transposition(track, state, *key - first->key);
            if (!edit)
            {
                edit = KeyEdit{.refused = "Hokuto can't move these notes that far. Every key stays from C-2 to G8."};
            }
            edit->name = "Set Note Pitch";
            return edit;
        }

    case InfoValue::kVelocity:
        {
            const std::optional<int> velocity = ParseVelocity(text);
            if (!velocity)
            {
                return Unreadable(text, "a velocity", "A velocity is a number from 1 to 127.");
            }
            if (*velocity == first->velocity)
            {
                return std::nullopt;
            }

            KeyEdit edit;
            edit.name = "Set Note Velocity";
            for (const auto& [command, old] : Velocities(track, state))
            {
                edit.changes.push_back(
                    {.command = command, .velocity = std::clamp(old + *velocity - first->velocity, 1, kTopVelocity)});
            }
            return edit;
        }
    }

    return std::nullopt;
}

KeyEdit Unreadable(std::string_view text, std::string_view what, std::string_view hint)
{
    return KeyEdit{.refused = "Hokuto can't read \"" + std::string(text) + "\" as " + std::string(what) + ". " +
                              std::string(hint)};
}

const KeyNote* InstrumentNoteAt(const KeyTrack& track, double tick)
{
    const auto after = std::upper_bound(track.notes.begin(), track.notes.end(), tick,
                                        [](double at, const KeyNote& note) { return at < note.start; });
    if (after != track.notes.begin())
    {
        return &*std::prev(after);
    }

    return track.notes.empty() ? nullptr : &track.notes.front();
}

KeyNote WithInstrumentAt(const KeyTrack& track, double tick, KeyNote note)
{
    if (const KeyNote* playing = InstrumentNoteAt(track, tick))
    {
        note.bank_slot = playing->bank_slot;
        note.program = playing->program;
    }

    return note;
}

bool InLane(const KeyGesture& gesture)
{
    return gesture.kind == KeyGesture::Kind::kControl || gesture.kind == KeyGesture::Kind::kLaneDraw ||
           gesture.kind == KeyGesture::Kind::kLaneBox;
}

bool GestureGone(const KeyGesture& gesture, const KeyTrack& track, std::size_t lanes)
{
    if (InLane(gesture))
    {
        return gesture.lane >= lanes ||
               (gesture.kind == KeyGesture::Kind::kControl && gesture.control >= track.controls.size());
    }

    return HoldsNote(gesture) && gesture.note >= track.notes.size();
}

std::set<uint32_t> FollowedSelection(KeySelection current, std::optional<KeySelection> waiting,
                                     const format::Shared<ntdlib::commands::Sequence>& shown)
{
    if (shown.SharesWith(current.sequence))
    {
        return std::move(current.commands);
    }
    if (waiting && waiting->sequence.SharesWith(shown) && waiting->track == current.track)
    {
        return std::move(waiting->commands);
    }

    return {};
}

std::optional<KeyNote> SoundingNote(const KeyTrack& track, const KeyEditorState& state)
{
    const KeyGesture& gesture = state.gesture;
    if (gesture.kind == KeyGesture::Kind::kKeyboard)
    {
        return gesture.played;
    }
    if (!state.feedback)
    {
        return std::nullopt;
    }

    switch (gesture.kind)
    {
    case KeyGesture::Kind::kPress:
    case KeyGesture::Kind::kMove:
        {
            if (gesture.at_end || gesture.note >= track.notes.size())
            {
                return std::nullopt;
            }

            KeyNote note = track.notes[gesture.note];
            note.key = std::clamp(note.key + gesture.transpose, 0, kKeys - 1);
            return note;
        }

    case KeyGesture::Kind::kDraw:
        return gesture.drawn;

    case KeyGesture::Kind::kNone:
        return state.transposed;

    default:
        return std::nullopt;
    }
}

} // namespace hokuto::app
