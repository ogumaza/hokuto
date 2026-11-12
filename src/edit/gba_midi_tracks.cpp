// SPDX-License-Identifier: MIT

// The second half of the MIDI import as a GBA song: the steps of GbaMidiReader that turn the events placed on each
// track into the song's items, with the ties, the track opening, the loop and the labels, and the import warnings.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "ntdlib/commands/units.h"
#include "ntdlib/mp2k/commands.h"

#include "edit/gba_midi.h"
#include "edit/gba_midi_reader_parts.h"
#include "edit/midi_events.h"
#include "edit/mp2k.h"

namespace hokuto::edit::gba_midi_reader_parts
{
namespace
{

using ntdlib::commands::kOne;

// Whether a note code of MusicPlayer2000 stands for a length of `ticks`.
bool IsCodeLength(uint64_t ticks)
{
    return ticks > 0 && static_cast<uint64_t>(ntdlib::mp2k::ClockLength(
                            ntdlib::mp2k::UsualLengthCode(static_cast<int64_t>(ticks)))) == ticks;
}

// Writes waits from tick `now` up to `tick` into `items`, each in a length that MusicPlayer2000's codes give, the
// longest first. `now` becomes `tick`.
void WaitTo(std::vector<Command>& items, uint64_t& now, uint64_t tick)
{
    uint64_t ticks = tick - now;
    while (ticks > 0)
    {
        const auto length = static_cast<uint64_t>(
            ntdlib::mp2k::ClockLength(ntdlib::mp2k::UsualLengthCode(static_cast<int64_t>(ticks))));
        Command wait;
        wait.kind = Kind::kWait;
        wait.value.value = static_cast<int64_t>(length) * kOne;
        items.push_back(wait);
        ticks -= length;
    }

    now = tick;
}

} // namespace

void GbaMidiReader::TrackCommands(std::size_t t)
{
    std::vector<Placed>& placed = tracks_[t];
    SplitTies(placed);
    AddOpening(placed, drum_tracks_[t]);
    DropAfterLoop(placed);
    AddInstruments(placed);
    DropMissingLabels(placed);
    WriteItems(placed, track_ends_[t]);
}

void GbaMidiReader::SplitTies(std::vector<Placed>& placed) const
{
    // A note longer than MusicPlayer2000's bytes hold becomes a tie and the tie's end. Where a note reads two bytes, so
    // does a note whose length matches no code. The events from the loop's end on stay out, and a tie that sounds over
    // it ends there.
    std::vector<Placed> ends;
    for (Placed& event : placed)
    {
        if (!event.command || event.command->kind != Kind::kNote)
        {
            continue;
        }

        const auto length = static_cast<uint64_t>(event.command->value.value / kOne);
        if (note_arguments_ < 3 ? IsCodeLength(length) : length <= kLongestMp2kNote)
        {
            continue;
        }

        uint64_t end = event.tick + length;
        if (loop_end_ && end > *loop_end_)
        {
            end = *loop_end_;
        }

        event.command->value.value = 0;
        Command tie_end;
        tie_end.kind = Kind::kNoteOff;
        tie_end.key = event.command->key;
        ends.push_back(Place(end, kTieEnd, event.serial, tie_end));
    }

    placed.insert(placed.end(), ends.begin(), ends.end());
}

void GbaMidiReader::AddOpening(std::vector<Placed>& placed, bool drums) const
{
    // The track's opening: its volume and its voice, where no setting comes before its first note. A track whose
    // notes all fall past the loop's end has none.
    const auto order_of = [](const Placed& p)
    {
        return std::tuple(p.tick, p.order, p.serial);
    };
    std::optional<std::tuple<uint64_t, Order, uint64_t>> first_note;
    for (const Placed& event : placed)
    {
        if (event.command && event.command->kind == Kind::kNote && (!first_note || order_of(event) < *first_note))
        {
            first_note = order_of(event);
        }
    }

    const auto before_first = [&](Kind kind)
    {
        return std::any_of(placed.begin(), placed.end(), [&](const Placed& p)
                           { return p.command && p.command->kind == kind && order_of(p) < *first_note; });
    };
    if (first_note && !before_first(Kind::kProgram))
    {
        const GbaMidiInstrument opening{drums, 0};
        placed.insert(placed.begin(), Place(0, kSetting, 0, Setting(Kind::kProgram, VoiceOf(opening)), -1, opening));
    }

    if (first_note && !before_first(Kind::kVolume))
    {
        placed.insert(placed.begin(), Place(0, kSetting, 0, Setting(Kind::kVolume, VolumeOf(127, 127))));
    }

    std::stable_sort(placed.begin(), placed.end(), [](const Placed& a, const Placed& b)
                     { return std::tie(a.tick, a.order, a.serial) < std::tie(b.tick, b.order, b.serial); });
}

void GbaMidiReader::DropAfterLoop(std::vector<Placed>& placed)
{
    // The events from the loop's end on stay out, but the ends of the ties there.
    const auto after_loop = [&](const Placed& p)
    {
        if (!PastLoop(p.tick) || (p.order == kTieEnd && p.tick == *loop_end_))
        {
            return false;
        }

        ++left_out_["after the loop end"];
        return true;
    };
    std::erase_if(placed, after_loop);
}

void GbaMidiReader::AddInstruments(const std::vector<Placed>& placed)
{
    // The instruments that the notes play.
    std::optional<GbaMidiInstrument> playing;
    for (const Placed& event : placed)
    {
        if (event.command && event.command->kind == Kind::kProgram)
        {
            playing = event.instrument;
        }
        else if (event.command && event.command->kind == Kind::kNote && playing &&
                 std::find(song_.instruments.begin(), song_.instruments.end(), *playing) == song_.instruments.end())
        {
            song_.instruments.push_back(*playing);
        }
    }
}

void GbaMidiReader::DropMissingLabels(std::vector<Placed>& placed)
{
    // The labels of control change 17 that the track contains. A jump to another label stays out.
    std::set<int> labels;
    for (const Placed& event : placed)
    {
        if (!event.command && event.label >= 0)
        {
            labels.insert(event.label);
        }
    }

    const auto label_missing = [&](const Placed& p)
    {
        if (!p.command || p.label < 0 || labels.contains(p.label))
        {
            return false;
        }

        ++left_out_["memory access to labels missing from the track"];
        return true;
    };
    std::erase_if(placed, label_missing);
}

void GbaMidiReader::WriteItems(const std::vector<Placed>& placed, uint64_t track_end)
{
    // The items. A label moves to the next item, and a wait splits into the lengths of the codes of MusicPlayer2000,
    // the longest first.
    std::vector<Command>& items = song_.sequence.items;
    song_.entries.push_back(static_cast<uint32_t>(items.size()));
    uint64_t now = 0;
    std::map<int, uint32_t> label_items;
    std::vector<std::pair<std::size_t, int>> jumps; // each memory access that jumps, and its label
    std::optional<uint32_t> loop_item;
    uint64_t end = 0;
    bool looped = false;
    for (const Placed& event : placed)
    {
        if (loop_start_ && !looped &&
            (event.tick > *loop_start_ || (event.tick == *loop_start_ && event.order > kTieEnd)))
        {
            WaitTo(items, now, *loop_start_);
            loop_item = static_cast<uint32_t>(items.size());
            looped = true;
        }

        WaitTo(items, now, event.tick);
        end = std::max(end, event.tick);
        if (!event.command)
        {
            label_items[event.label] = static_cast<uint32_t>(items.size());
            continue;
        }

        if (event.command->kind == Kind::kNote && event.command->value.value > 0)
        {
            end = std::max(end, event.tick + static_cast<uint64_t>(event.command->value.value / kOne));
        }

        if (event.label >= 0)
        {
            jumps.emplace_back(items.size(), event.label);
        }

        items.push_back(*event.command);
    }

    // A track with a loop jumps back to its start. A track without one ends where the track chunks of its channel end,
    // or once its last event or note has ended where that comes later. The end of the track cuts the releases of its
    // notes.
    if (loop_start_)
    {
        if (!looped)
        {
            WaitTo(items, now, *loop_start_);
            loop_item = static_cast<uint32_t>(items.size());
        }

        WaitTo(items, now, *loop_end_);
        Command jump;
        jump.kind = Kind::kJump;
        jump.target = *loop_item;
        items.push_back(jump);
    }
    else
    {
        WaitTo(items, now, std::max<uint64_t>(end, track_end));
        Command fine;
        fine.kind = Kind::kEnd;
        items.push_back(fine);
    }

    for (const auto& [item, label] : jumps)
    {
        items[item].target = label_items.at(label);
    }
}

void GbaMidiReader::AddWarnings()
{
    if (moved_ > 0)
    {
        song_.warnings.push_back(std::to_string(moved_) + (moved_ == 1 ? " event is" : " events are") +
                                 " moved to the nearest tick. The GBA uses 24 ticks per quarter note.");
    }

    for (const GbaMidiInstrument& instrument : song_.instruments)
    {
        song_.voices[instrument] = VoiceOf(instrument);
    }

    AddLeftOutWarnings(left_out_, song_.warnings);
}

} // namespace hokuto::edit::gba_midi_reader_parts
