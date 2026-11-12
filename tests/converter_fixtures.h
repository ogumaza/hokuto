// SPDX-License-Identifier: MIT

// This header contains the songs and helpers that the tests of the converter and the timelines share.

#pragma once

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

#include "edit/converter.h"
#include "edit/notes.h"
#include "edit/replace.h"
#include "edit/timelines.h"
#include "edit/tracks.h"
#include "format/archive.h"
#include "format/file.h"
#include "song/archive.h"

namespace
{

namespace sar_files = ntdlib::testing::sar_files;
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

// A line of a song: the labels at its command, the command, and the label of the command's target.
struct Line
{
    std::vector<std::string> labels;
    Command command;
    std::string target = {};
};

// A command of `kind` with the value `value`.
inline Command Of(Kind kind, int64_t value = 0)
{
    Command command;
    command.kind = kind;
    command.value.value = value * kOne;

    return command;
}

// A note of key `key` for `length` ticks, at velocity 100.
inline Command Note(int key, int64_t length)
{
    Command note = Of(Kind::kNote, length);
    note.key = static_cast<uint8_t>(key);
    note.velocity = 100 * kOne;

    return note;
}

// A command that opens track `track`.
inline Command OpenTrack(int track)
{
    Command open = Of(Kind::kOpenTrack);
    open.index = static_cast<uint8_t>(track);

    return open;
}

// The sequence that `lines` make, with the labels in the order of their names. The converter writes them in that order.
inline Sequence Build(const std::vector<Line>& lines)
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
inline std::vector<Line> Bar0(const std::vector<std::string>& labels = {})
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
inline std::vector<Line> Bar1(const std::vector<std::string>& labels = {}, bool split = false)
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
inline void Append(std::vector<Line>& lines, const std::vector<Line>& more)
{
    lines.insert(lines.end(), more.begin(), more.end());
}

// The main song. With `inline_bar`, track 0 writes its second bar in the track instead of calling it.
inline Sequence MainSong(bool inline_bar = false)
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

// A song whose two bars before its start hold the same 8 settings. Each is a block at tick 0, and both call one
// subroutine. The bars after them hold too few commands to call.
inline Sequence OpeningSong()
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

// `sequence` without item `item`. The labels and the targets after it move back.
inline Sequence Without(Sequence sequence, uint32_t item)
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
inline int64_t WaitAt(const Sequence& sequence, std::size_t item)
{
    const Command& command = sequence.items.at(item);
    return command.kind == Kind::kWait ? command.value.value / kOne : -1;
}

// A song whose header waits 17 ticks, then starts track 1 and jumps to track 0. Each track loops over a bar.
inline Sequence WaitedSong()
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

} // namespace
