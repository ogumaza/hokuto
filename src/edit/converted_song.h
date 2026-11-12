// SPDX-License-Identifier: MIT

// A song that NintendoWare's MIDI converter wrote, as the files of edit/converter.h read it and lay it out: the events
// of its tracks at their ticks, the anchors between them, the calls, and the blocks of the layout. Only those files
// include it.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ntdlib/commands/sequence.h"

#include "edit/converter.h"

namespace hokuto::edit::converter_detail
{

using ntdlib::commands::Command;
using ntdlib::commands::Label;
using ntdlib::commands::Sequence;

// The start of a converted song's labels. The MIDI file's name follows.
inline constexpr std::string_view kLabelPrefix = "SMF_";

// The start of the names that the layout gives the places where a jump goes without a label. No label of a sequence
// file starts with it, and the layout leaves these names out.
inline constexpr std::string_view kPlacePrefix = "\x01";

// A command of a track at its tick: anything but a wait, a call, a return, a jump or an end. A split holds no command.
// It splits a rest in two with nothing between: the MIDI file held an event there that the converter left out. `item`
// is the command's item in the sequence that the track came from, or the item of the wait after a split. A command that
// a subroutine holds gives the item in the subroutine.
struct Event
{
    uint32_t tick = 0;
    Command command;
    bool split = false;
    uint32_t item = 0;
};

// A label, a jump or a loop's start or end in a track's code outside its subroutines: after `position` of the track's
// events, at `tick`. A label has a name. The other anchors hold their command, and a jump goes to the label `target`.
struct Anchor
{
    std::size_t position = 0;
    uint32_t tick = 0;
    std::string label;
    std::optional<Command> command;
    std::string target;
};

// A call as the converter wrote it: at `tick`, of the subroutine `name`. The subroutine lasts `length` ticks and plays
// the track's events `first` to `first + count - 1`.
struct Call
{
    uint32_t tick = 0;
    uint32_t length = 0;
    std::string name;
    std::size_t first = 0;
    std::size_t count = 0;
};

// A track of a converted song: its label and the other labels at its first command, the commands that open it, its
// events, its labels and its jump in their order, and its end. A track that ends with its jump has no end command.
// `calls` gives the calls that its code made. `after_end` holds the commands after the end that nothing reaches.
struct Track
{
    uint8_t number = 0;
    std::string label;
    std::vector<std::string> start_labels;
    std::vector<Command> prefix;
    std::vector<Event> events;
    std::vector<Anchor> anchors;
    std::optional<Command> end;
    uint32_t end_tick = 0;
    std::vector<Call> calls;
    std::vector<Command> after_end;
};

// A converted song: its name, the commands from its Begin label to its first track, the labels among them and the
// labels that their targets name, its tracks in the order of the file, the End label's command, and the items after the
// song with their labels. The items after the song give their targets and their labels' items from the first of them.
struct Song
{
    std::string name;
    std::vector<Command> header;
    std::vector<Label> header_labels;
    std::vector<std::string> header_targets;
    std::vector<Track> tracks;
    Command end;
    std::vector<Command> after;
    std::vector<Label> after_labels;
};

// A block of a track's events that the converter compared with the others: a bar of the MIDI file from the song's
// start, or a bar before it. A bar before the start takes no time. `body` holds the commands that a subroutine of the
// block holds.
struct Block
{
    uint32_t start = 0;
    uint32_t end = 0;
    std::size_t first = 0;
    std::size_t last = 0;
    bool timeless = false;
    std::optional<uint32_t> bar;
    bool marked = false;
    bool short_bar = false;
    std::vector<Command> body;
};

// The ticks of `argument`. Throws std::invalid_argument for a length that isn't a fixed number of ticks.
uint32_t TicksOf(const ntdlib::commands::Argument& argument);

// The converted song that `sequence` holds. Throws std::invalid_argument for a sequence that doesn't hold one.
Song ReadSong(const Sequence& sequence);

// The hints of track `track` in `hints`, or empty hints for a track that has none.
const TrackHints& HintsOfTrack(const ConverterHints& hints, uint8_t track);

// The blocks of `track`: the blocks before the song's start, then its bars.
std::vector<Block> BlocksOf(const Track& track, const TrackHints& hints, const ConverterHints& song);

// Whether the converter calls each block of `blocks`: the blocks that the rules let it call and that repeat, and those
// that `hints` names. A block that holds a label or the jump stays in its track, whatever the hints say. An edit of the
// loop can move the label or the jump into a block that the hints name.
std::vector<bool> CalledBlocks(const std::vector<Block>& blocks, const TrackHints& hints);

// The sequence of `song` laid out by `hints`: with the converter's subroutines, or with every bar in its track.
Sequence LayOut(const Song& song, const ConverterHints& hints, bool subroutines);

} // namespace hokuto::edit::converter_detail
