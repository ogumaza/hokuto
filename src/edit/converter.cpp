// SPDX-License-Identifier: MIT

#include "edit/converter.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "ntdlib/commands/units.h"

namespace hokuto::edit
{
namespace
{

using format::BarRun;
using ntdlib::commands::Command;
using ntdlib::commands::Kind;
using ntdlib::commands::kOne;
using ntdlib::commands::Label;
using ntdlib::commands::Sequence;
using ntdlib::commands::Source;

// The start of a converted song's labels. The MIDI file's name follows.
constexpr std::string_view kLabelPrefix = "SMF_";

// The start of the names that the layout gives the places where a jump goes without a label. No label of a sequence
// file starts with it, and the layout leaves these names out.
constexpr std::string_view kPlacePrefix = "\x01";

// The fewest commands of a bar that the converter calls. A block that takes no time counts one more, such as the rest
// that the converter took out of it.
constexpr std::size_t kFewestCalled = 8;

// The timebase of a song without a timebase command, in ticks a quarter note: NintendoWare's default.
constexpr uint32_t kDefaultTimebase = 48;

// The lengths of bar to try for a song without calls, in quarter notes, the likeliest first.
constexpr uint32_t kBarsInQuarterNotes[] = {4, 3, 2, 6, 5, 7, 1, 8};

// The most rounds of exceptions that a track takes before its calls settle.
constexpr int kRounds = 8;

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

// How a candidate's bars fill a stretch between two calls that no length of the song's calls fills.
enum class Fill
{
    kLongFirst,  // bars of the commonest length, then a shorter one
    kShortFirst, // a shorter bar, then bars of the commonest length
    kOneBar,     // one bar
};

// Bar lines to try for a song: the starts of its bars, from tick 0. `whole` marks one bar for a whole song without
// calls.
struct BarCandidate
{
    friend bool operator==(const BarCandidate&, const BarCandidate&) = default;

    std::vector<uint32_t> starts;
    bool whole = false;
};

uint32_t TicksOf(const ntdlib::commands::Argument& argument)
{
    if (argument.source != Source::kValue || argument.value < 0 || argument.value % kOne != 0 ||
        argument.value / kOne > UINT32_MAX)
    {
        throw std::invalid_argument("a length that isn't a fixed number of ticks");
    }

    return static_cast<uint32_t>(argument.value / kOne);
}

Command Wait(uint32_t ticks)
{
    Command wait;
    wait.kind = Kind::kWait;
    wait.value.value = static_cast<int64_t>(ticks) * kOne;

    return wait;
}

// Whether a track can hold `command` as one of its events: a command that moves neither the track's time nor its place
// in the code. The converter writes notes and settings. A song edited by hand can hold logic among them too, such as
// variables, comparisons and the commands that run only if a comparison held.
bool IsEvent(const Command& command)
{
    switch (command.kind)
    {
    case Kind::kWait:
    case Kind::kOpenTrack:
    case Kind::kJump:
    case Kind::kCall:
    case Kind::kReturn:
    case Kind::kLoopStart:
    case Kind::kLoopEnd:
    case Kind::kEnd:
    case Kind::kAllocateTracks:
    case Kind::kNoteWait:
    case Kind::kData:
        return false;
    default:
        return true;
    }
}

// The labels of `sequence` by their items.
std::multimap<uint32_t, std::string> LabelsByItem(const Sequence& sequence)
{
    std::multimap<uint32_t, std::string> labels;
    for (const Label& label : sequence.labels)
    {
        labels.emplace(label.item, label.name);
    }

    return labels;
}

// The name of the label at `item`, or an empty one.
std::string LabelAt(const std::multimap<uint32_t, std::string>& labels, uint32_t item)
{
    const auto found = labels.find(item);
    return found == labels.end() ? std::string() : found->second;
}

// Whether `text` is a number in decimal digits.
bool IsNumber(std::string_view text)
{
    const auto digit = [](char c)
    {
        return c >= '0' && c <= '9';
    };
    return !text.empty() && std::all_of(text.begin(), text.end(), digit);
}

// Whether `name` names a track, such as Track_3, a subroutine of a track, such as Track_3_0, or the song's end.
// `prefix` starts the song's labels.
bool IsPartName(std::string_view name, const std::string& prefix)
{
    if (!name.starts_with(prefix))
    {
        return false;
    }

    name.remove_prefix(prefix.size());
    if (name == "End")
    {
        return true;
    }
    if (!name.starts_with("Track_"))
    {
        return false;
    }

    name.remove_prefix(6);
    const std::size_t underscore = name.find('_');
    if (underscore == std::string_view::npos)
    {
        return IsNumber(name);
    }

    return IsNumber(name.substr(0, underscore)) && IsNumber(name.substr(underscore + 1));
}

// Whether a label at `item` starts a track, a subroutine or the song's end. `prefix` starts the song's labels.
bool StartsPart(const std::multimap<uint32_t, std::string>& labels, uint32_t item, const std::string& prefix)
{
    const auto [first, last] = labels.equal_range(item);
    return std::any_of(first, last, [&prefix](const auto& label) { return IsPartName(label.second, prefix); });
}

// The track `number` of `sequence`, from its label at item `start`. `prefix` starts the song's labels.
Track WalkTrack(const Sequence& sequence, const std::multimap<uint32_t, std::string>& labels, const std::string& prefix,
                uint8_t number, uint32_t start)
{
    Track track;
    track.number = number;
    track.label = prefix + "Track_" + std::to_string(number);
    const std::string subroutines = track.label + "_";

    uint32_t tick = 0;
    std::optional<Call> call;
    uint32_t back = 0;
    bool after_wait = false;
    bool notes_wait = true;
    for (uint32_t item = start;;)
    {
        if (item >= sequence.items.size())
        {
            throw std::invalid_argument("a track runs past the end of the sequence");
        }

        // The track's labels outside its subroutines. A subroutine has a label at its first command only.
        const auto [first, last] = labels.equal_range(item);
        for (auto label = first; label != last; ++label)
        {
            after_wait = false;
            if (call)
            {
                if (label->second != call->name || track.events.size() != call->first)
                {
                    throw std::invalid_argument("a label inside a subroutine");
                }
                continue;
            }
            if (item != start)
            {
                track.anchors.push_back({track.events.size(), tick, label->second, std::nullopt, {}});
            }
            else if (label->second != track.label)
            {
                track.start_labels.push_back(label->second);
            }
        }

        // A jump can run only if a comparison held. Any other command that moves the track's time or its place in the
        // code runs always.
        const Command& command = sequence.items[item];
        if (command.conditional && !IsEvent(command) && command.kind != Kind::kJump)
        {
            throw std::invalid_argument("a command that runs only if a comparison held: " +
                                        std::string(ntdlib::commands::Name(command.kind)));
        }
        const bool wait = command.kind == Kind::kWait;
        if (wait && after_wait)
        {
            track.events.push_back({tick, {}, true, item});
        }
        after_wait = wait;
        switch (command.kind)
        {
        case Kind::kWait:
            tick += TicksOf(command.value);
            ++item;
            break;

        case Kind::kCall:
            {
                const std::string name = command.target_outside ? std::string() : LabelAt(labels, command.target);
                if (call || !name.starts_with(subroutines))
                {
                    throw std::invalid_argument("a call that isn't the converter's");
                }
                call = Call{tick, 0, name, track.events.size(), 0};
                back = item + 1;
                item = command.target;
                break;
            }

        case Kind::kReturn:
            if (!call)
            {
                throw std::invalid_argument("a return outside a subroutine");
            }
            call->length = tick - call->tick;
            call->count = track.events.size() - call->first;
            track.calls.push_back(*call);
            call.reset();
            item = back;
            break;

        case Kind::kJump:
            {
                const std::string target = command.target_outside ? std::string() : LabelAt(labels, command.target);
                if (call || !(target.starts_with(prefix) || target.starts_with(kPlacePrefix)))
                {
                    throw std::invalid_argument("a jump that isn't the converter's");
                }
                track.anchors.push_back({track.events.size(), tick, {}, command, target});
                ++item;

                // A track can end with a jump that runs always. Its subroutines, the next track or the song's end
                // follow.
                if (!command.conditional && (item == sequence.items.size() || StartsPart(labels, item, prefix)))
                {
                    track.end_tick = tick;
                    return track;
                }
                break;
            }

        case Kind::kEnd:
            if (call)
            {
                throw std::invalid_argument("an end inside a subroutine");
            }
            track.end = command;
            track.end_tick = tick;

            // Commands that nothing reaches can follow the end, up to the next part of the song, when a hand cut the
            // track short.
            for (++item; item < sequence.items.size() && !StartsPart(labels, item, prefix); ++item)
            {
                if (labels.contains(item) || ntdlib::commands::HasTarget(sequence.items[item].kind))
                {
                    throw std::invalid_argument("a label or a target after a track's end");
                }

                track.after_end.push_back(sequence.items[item]);
            }
            return track;

        case Kind::kLoopStart:
        case Kind::kLoopEnd:
            if (call)
            {
                throw std::invalid_argument("a loop inside a subroutine");
            }
            track.anchors.push_back({track.events.size(), tick, {}, command, {}});
            ++item;
            break;

        case Kind::kNoteWait:
            // The converter turns the note wait off at the track's first command. A song edited by hand can set up the
            // track before it.
            if (call || command.value.value != 0 || !notes_wait)
            {
                throw std::invalid_argument("a note wait that isn't the converter's");
            }
            notes_wait = false;
            if (item == start)
            {
                track.prefix.push_back(command);
            }
            else
            {
                track.events.push_back({tick, command, false, item});
            }
            ++item;
            break;

        default:
            if (!IsEvent(command))
            {
                throw std::invalid_argument("a command that a converted track can't hold: " +
                                            std::string(ntdlib::commands::Name(command.kind)));
            }
            if (command.kind == Kind::kNote && notes_wait)
            {
                throw std::invalid_argument("a note that waits for its length");
            }
            track.events.push_back({tick, command, false, item});
            ++item;
            break;
        }
    }
}

// The converted song that `sequence` holds. Throws std::invalid_argument for a sequence that doesn't hold one.
Song ReadSong(const Sequence& sequence)
{
    // The song's name, from its Begin label at the first item. A place where a jump goes without a label takes a name
    // of its own.
    std::multimap<uint32_t, std::string> labels = LabelsByItem(sequence);
    for (const Command& command : sequence.items)
    {
        if (command.kind == Kind::kJump && !command.target_outside && !labels.contains(command.target))
        {
            labels.emplace(command.target, std::string(kPlacePrefix) + std::to_string(command.target));
        }
    }
    std::string prefix;
    for (const Label& label : sequence.labels)
    {
        if (label.name.starts_with(kLabelPrefix) && label.name.ends_with("_Begin"))
        {
            if (!prefix.empty() || label.item != 0)
            {
                throw std::invalid_argument("no single song at the sequence's start");
            }
            prefix = label.name.substr(0, label.name.size() - 5);
        }
    }
    if (prefix.empty())
    {
        throw std::invalid_argument("no converted song");
    }

    Song song;
    song.name = prefix.substr(kLabelPrefix.size(), prefix.size() - kLabelPrefix.size() - 1);
    std::map<std::string, uint32_t> items;
    for (const Label& label : sequence.labels)
    {
        if (!items.emplace(label.name, label.item).second)
        {
            throw std::invalid_argument("two labels with one name");
        }
    }

    // The tracks, by their labels, in the order of the file. The first track follows the header.
    std::vector<std::pair<uint32_t, uint8_t>> tracks;
    for (const auto& [name, item] : items)
    {
        if (!name.starts_with(prefix + "Track_"))
        {
            continue;
        }

        const std::string number = name.substr(prefix.size() + 6);
        if (number.size() <= 2 && IsNumber(number))
        {
            tracks.emplace_back(item, static_cast<uint8_t>(std::stoi(number)));
        }
    }
    std::sort(tracks.begin(), tracks.end());
    if (tracks.empty())
    {
        throw std::invalid_argument("no tracks");
    }

    // The header, from the Begin label to the first track. A command that goes into the header keeps its target. One
    // that goes past it names a label.
    const uint32_t header_end = tracks.front().first;
    song.header.assign(sequence.items.begin(), sequence.items.begin() + header_end);
    for (uint32_t item = 0; item < header_end; ++item)
    {
        const Command& command = sequence.items[item];
        std::string target;
        if (ntdlib::commands::HasTarget(command.kind) && (command.target_outside || command.target >= header_end))
        {
            target = command.target_outside ? std::string() : LabelAt(labels, command.target);
            if (!items.contains(target))
            {
                throw std::invalid_argument("a header that isn't the converter's");
            }
        }
        song.header_targets.push_back(target);
    }
    for (const Label& label : sequence.labels)
    {
        if (label.item < header_end)
        {
            song.header_labels.push_back(label);
        }
    }

    for (const auto& [item, number] : tracks)
    {
        song.tracks.push_back(WalkTrack(sequence, labels, prefix, number, item));
    }

    // The End label, and the items after it. Nothing in the song names them, and they name nothing in the song.
    const auto end = items.find(prefix + "End");
    if (end == items.end() || sequence.items[end->second].kind != Kind::kEnd)
    {
        throw std::invalid_argument("no End label");
    }
    song.end = sequence.items[end->second];
    const uint32_t after = end->second + 1;
    song.after.assign(sequence.items.begin() + after, sequence.items.end());
    for (Command& command : song.after)
    {
        if (ntdlib::commands::HasTarget(command.kind) && !command.target_outside)
        {
            if (command.target < after)
            {
                throw std::invalid_argument("items after the song that name the song");
            }
            command.target -= after;
        }
    }
    for (const Label& label : sequence.labels)
    {
        if (label.item >= after)
        {
            song.after_labels.push_back({label.name, label.item - after});
        }
    }

    return song;
}

// A key for `commands` that distinguishes any two lists of commands, for maps of bodies.
std::string KeyOf(const std::vector<Command>& commands)
{
    std::string key;
    const auto add = [&key](const auto& value)
    {
        key.append(reinterpret_cast<const char*>(&value), sizeof(value));
    };
    for (const Command& command : commands)
    {
        add(command.kind);
        add(command.conditional);
        add(command.key);
        add(command.velocity);
        add(command.index);
        add(command.value.source);
        add(command.value.value);
        add(command.value.high);
        add(command.time.has_value());
        if (command.time)
        {
            add(command.time->source);
            add(command.time->value);
            add(command.time->high);
        }
        add(command.target);
        add(command.target_outside);
        add(command.encoding.opcode);
        add(command.encoding.value_bytes);
        add(command.encoding.unused_time);
        add(command.encoding.unused_random);
        add(command.encoding.unused_variable);
        add(command.bytes.size());
        key.append(command.bytes.begin(), command.bytes.end());
    }

    return key;
}

// The ticks at which the bars of `hints` start, up to and including the first one at or after `end`.
std::vector<uint32_t> BarStarts(const ConverterHints& hints, uint32_t end)
{
    std::vector<uint32_t> starts;
    uint32_t tick = 0;
    for (const BarRun& run : hints.bars)
    {
        for (uint32_t bar = 0; bar < run.count; ++bar)
        {
            starts.push_back(tick);
            if (tick >= end)
            {
                return starts;
            }
            tick += run.length;
        }
    }

    const uint32_t length = hints.bars.empty() ? 0 : hints.bars.back().length;
    if (length == 0)
    {
        starts.push_back(tick);
        if (tick < end)
        {
            starts.push_back(end);
        }
        return starts;
    }
    while (true)
    {
        starts.push_back(tick);
        if (tick >= end)
        {
            return starts;
        }
        tick += length;
    }
}

const TrackHints& HintsOfTrack(const ConverterHints& hints, uint8_t track)
{
    static const TrackHints kNone;
    for (const TrackHints& candidate : hints.tracks)
    {
        if (candidate.track == track)
        {
            return candidate;
        }
    }

    return kNone;
}

// The index of the bar that holds `tick`: the last one that starts at or before it.
std::size_t BarAt(const std::vector<uint32_t>& starts, uint32_t tick)
{
    const auto found = std::upper_bound(starts.begin(), starts.end(), tick);
    return static_cast<std::size_t>(std::max<std::ptrdiff_t>(found - starts.begin() - 1, 0));
}

// The blocks of `track`: the blocks before the song's start, then its bars.
std::vector<Block> BlocksOf(const Track& track, const TrackHints& hints, const ConverterHints& song)
{
    std::vector<Block> blocks;

    // The blocks before the song's start, at tick 0.
    std::size_t event = 0;
    for (const uint32_t count : hints.opening)
    {
        if (event + count > track.events.size() || (count > 0 && track.events[event + count - 1].tick != 0))
        {
            throw std::invalid_argument("an opening past the commands at tick 0");
        }

        Block block;
        block.first = event;
        block.last = event + count;
        block.timeless = true;
        for (std::size_t i = block.first; i < block.last; ++i)
        {
            if (!track.events[i].split)
            {
                block.body.push_back(track.events[i].command);
            }
        }
        blocks.push_back(std::move(block));
        event += count;
    }

    // The bars, each up to the next bar line or the track's end.
    const uint32_t last_tick = track.events.empty() ? 0 : track.events.back().tick;
    const std::vector<uint32_t> starts = BarStarts(song, std::max(track.end_tick, last_tick + 1));
    for (std::size_t bar = 0; bar + 1 < starts.size() && starts[bar] <= std::max(track.end_tick, last_tick); ++bar)
    {
        Block block;
        block.start = starts[bar];
        block.end = std::min(starts[bar + 1], std::max(track.end_tick, block.start));
        block.bar = static_cast<uint32_t>(bar);
        block.short_bar = bar == 0 && starts.size() > 2 && starts[1] - starts[0] < starts[2] - starts[1];
        block.first = event;
        while (event < track.events.size() && track.events[event].tick < starts[bar + 1])
        {
            ++event;
        }
        block.last = event;

        uint32_t tick = block.start;
        for (std::size_t i = block.first; i < block.last; ++i)
        {
            if (track.events[i].tick > tick)
            {
                block.body.push_back(Wait(track.events[i].tick - tick));
                tick = track.events[i].tick;
            }
            if (!track.events[i].split)
            {
                block.body.push_back(track.events[i].command);
            }
        }
        if (block.end > tick)
        {
            block.body.push_back(Wait(block.end - tick));
        }
        blocks.push_back(std::move(block));
    }
    if (event != track.events.size())
    {
        throw std::invalid_argument("events past the bars");
    }

    // The bars that hold a label or the jump. A label or a jump between two blocks belongs to the later one.
    for (const Anchor& anchor : track.anchors)
    {
        const std::size_t bar = BarAt(starts, anchor.tick);
        for (Block& block : blocks)
        {
            if (block.bar == bar || (block.timeless && anchor.position > block.first && anchor.position < block.last))
            {
                block.marked = true;
            }
        }
    }

    return blocks;
}

// Whether the converter calls each block of `blocks`: the blocks that the rules let it call and that repeat, and those
// that `hints` names. A block that holds a label or the jump stays in its track, whatever the hints say. An edit of the
// loop can move the label or the jump into a block that the hints name.
std::vector<bool> CalledBlocks(const std::vector<Block>& blocks, const TrackHints& hints)
{
    const auto listed = [](const std::vector<uint32_t>& list, const Block& block)
    {
        return block.bar && std::binary_search(list.begin(), list.end(), *block.bar);
    };

    std::vector<std::string> keys;
    std::map<std::string, int> copies;
    std::vector<bool> may(blocks.size(), false);
    for (std::size_t i = 0; i < blocks.size(); ++i)
    {
        const Block& block = blocks[i];
        keys.push_back(KeyOf(block.body));
        const std::size_t counted = block.body.size() + (block.end == block.start ? 1 : 0);
        may[i] = !block.marked && !block.short_bar && counted >= kFewestCalled && !listed(hints.kept_inline, block);
        if (listed(hints.called, block) && !block.marked)
        {
            may[i] = true;
            copies[keys[i]] += 2;
        }
        if (may[i])
        {
            ++copies[keys[i]];
        }
    }

    std::vector<bool> called(blocks.size(), false);
    for (std::size_t i = 0; i < blocks.size(); ++i)
    {
        called[i] = may[i] && copies[keys[i]] >= 2;
    }

    return called;
}

// Writes a song's items into a sequence, with labels on the next item and targets by labels' names.
class Writer
{
public:
    explicit Writer(Sequence& sequence) : sequence_(sequence)
    {
    }

    void Label(const std::string& name)
    {
        pending_.push_back(name);
    }

    void Item(const Command& command, const std::string& target = {})
    {
        for (std::string& name : pending_)
        {
            sequence_.labels.push_back({std::move(name), static_cast<uint32_t>(sequence_.items.size())});
        }
        pending_.clear();
        if (!target.empty())
        {
            targets_.emplace_back(static_cast<uint32_t>(sequence_.items.size()), target);
        }
        sequence_.items.push_back(command);
    }

    // Gives each command its target's item, and sorts the labels by their names.
    void Finish()
    {
        if (!pending_.empty())
        {
            throw std::invalid_argument("a label after the last item");
        }

        std::map<std::string, uint32_t> items;
        for (const ntdlib::commands::Label& label : sequence_.labels)
        {
            items[label.name] = label.item;
        }
        for (const auto& [item, target] : targets_)
        {
            const auto found = items.find(target);
            if (found == items.end())
            {
                throw std::invalid_argument("a target without its label");
            }
            sequence_.items[item].target = found->second;
            sequence_.items[item].target_outside = false;
        }

        const auto place = [](const ntdlib::commands::Label& label)
        {
            return label.name.starts_with(kPlacePrefix);
        };
        std::erase_if(sequence_.labels, place);
        const auto by_name = [](const ntdlib::commands::Label& a, const ntdlib::commands::Label& b)
        {
            return a.name < b.name;
        };
        std::stable_sort(sequence_.labels.begin(), sequence_.labels.end(), by_name);
    }

private:
    Sequence& sequence_;
    std::vector<std::string> pending_;
    std::vector<std::pair<uint32_t, std::string>> targets_;
};

// Writes `track`'s code and its subroutines.
void WriteTrack(Writer& writer, const Track& track, const TrackHints& hints, const ConverterHints& song,
                bool subroutines)
{
    const std::vector<Block> blocks = BlocksOf(track, hints, song);
    const std::vector<bool> called = subroutines ? CalledBlocks(blocks, hints) : std::vector<bool>(blocks.size());

    // The rests in the track's code join across bar lines. A call, a label and the jump split them.
    uint32_t tick = 0;
    std::size_t anchor = 0;
    const auto rest_to = [&](uint32_t to)
    {
        if (to > tick)
        {
            writer.Item(Wait(to - tick));
            tick = to;
        }
    };
    const auto anchors_to = [&](std::size_t position, uint32_t up_to_tick)
    {
        while (anchor < track.anchors.size() && track.anchors[anchor].position <= position &&
               track.anchors[anchor].tick <= up_to_tick)
        {
            const Anchor& next = track.anchors[anchor];
            rest_to(next.tick);
            if (next.command)
            {
                writer.Item(*next.command, next.target);
            }
            else
            {
                writer.Label(next.label);
            }
            ++anchor;
        }
    };

    writer.Label(track.label);
    for (const std::string& label : track.start_labels)
    {
        writer.Label(label);
    }
    for (const Command& command : track.prefix)
    {
        writer.Item(command);
    }

    std::map<std::string, std::string> names;
    std::vector<const Block*> bodies;
    for (std::size_t i = 0; i < blocks.size(); ++i)
    {
        const Block& block = blocks[i];
        if (called[i])
        {
            anchors_to(block.first, block.start);
            rest_to(block.start);
            auto [name, added] = names.emplace(KeyOf(block.body), std::string());
            if (added)
            {
                name->second = track.label + "_" + std::to_string(bodies.size());
                bodies.push_back(&block);
            }
            writer.Item(Command{.kind = Kind::kCall}, name->second);
            tick = std::max(tick, block.end);
            continue;
        }

        for (std::size_t event = block.first; event < block.last; ++event)
        {
            anchors_to(event, track.events[event].tick);
            rest_to(track.events[event].tick);
            if (!track.events[event].split)
            {
                writer.Item(track.events[event].command);
            }
        }
    }
    anchors_to(track.events.size(), track.end_tick);
    rest_to(track.end_tick);
    if (anchor != track.anchors.size())
    {
        throw std::invalid_argument("a label or a jump past the track's end");
    }
    if (track.end)
    {
        writer.Item(*track.end);
    }
    for (const Command& command : track.after_end)
    {
        writer.Item(command);
    }

    // The subroutines, in the order of their first calls.
    for (std::size_t i = 0; i < bodies.size(); ++i)
    {
        writer.Label(track.label + "_" + std::to_string(i));
        for (const Command& command : bodies[i]->body)
        {
            writer.Item(command);
        }
        writer.Item(Command{.kind = Kind::kReturn});
    }
}

Sequence LayOut(const Song& song, const ConverterHints& hints, bool subroutines)
{
    Sequence sequence;
    Writer writer(sequence);

    for (std::size_t i = 0; i < song.header.size(); ++i)
    {
        for (const Label& label : song.header_labels)
        {
            if (label.item == i)
            {
                writer.Label(label.name);
            }
        }
        writer.Item(song.header[i], song.header_targets[i]);
    }

    for (const Track& track : song.tracks)
    {
        WriteTrack(writer, track, HintsOfTrack(hints, track.number), hints, subroutines);
    }

    writer.Label(std::string(kLabelPrefix) + song.name + "_End");
    writer.Item(song.end);

    const auto after = static_cast<uint32_t>(sequence.items.size());
    for (std::size_t i = 0; i < song.after.size(); ++i)
    {
        for (const Label& label : song.after_labels)
        {
            if (label.item == i)
            {
                writer.Label(label.name);
            }
        }
        Command command = song.after[i];
        if (ntdlib::commands::HasTarget(command.kind) && !command.target_outside)
        {
            command.target += after;
        }
        writer.Item(command);
    }

    writer.Finish();

    return sequence;
}

// Whether the converter called each of `blocks`, from the calls of `track`: a call plays a block's events over the
// block's ticks. Nothing when a call plays no block.
std::optional<std::vector<bool>> ActualCalls(const Track& track, const std::vector<Block>& blocks)
{
    std::map<std::tuple<uint32_t, uint32_t, std::size_t, std::size_t>, std::size_t> places;
    for (std::size_t i = 0; i < blocks.size(); ++i)
    {
        places.emplace(std::tuple(blocks[i].start, blocks[i].end, blocks[i].first, blocks[i].last), i);
    }

    std::vector<bool> called(blocks.size(), false);
    for (const Call& call : track.calls)
    {
        const auto found =
            places.find(std::tuple(call.tick, call.tick + call.length, call.first, call.first + call.count));
        if (found == places.end())
        {
            return std::nullopt;
        }
        called[found->second] = true;
    }

    return called;
}

// The blocks before the song's start that the calls of `track` show, by the numbers of their commands. They hold the
// commands at tick 0 before a call of the first bar, or else up to the last call at tick 0 that takes no time. Each
// call that takes no time is a block, and so is each run of commands around them. Nothing when no call shows a block.
std::vector<uint32_t> OpeningOf(const Track& track)
{
    std::vector<std::pair<std::size_t, std::size_t>> timeless;
    std::size_t end = 0;
    for (const Call& call : track.calls)
    {
        if (call.tick != 0)
        {
            break;
        }
        if (call.length > 0)
        {
            end = call.first;
            break;
        }

        timeless.emplace_back(call.first, call.count);
        end = call.first + call.count;
    }

    std::vector<uint32_t> opening;
    std::size_t event = 0;
    for (const auto& [first, count] : timeless)
    {
        if (first > event)
        {
            opening.push_back(static_cast<uint32_t>(first - event));
        }
        opening.push_back(static_cast<uint32_t>(count));
        event = first + count;
    }
    if (end > event)
    {
        opening.push_back(static_cast<uint32_t>(end - event));
    }

    return opening;
}

// The hints with which the rules give the calls of `track` on the bar lines of `song`: the track's opening, and the
// bars that the rules would call, or leave in the track, otherwise. Nothing when a call plays no block on those bar
// lines, or when a block before the song's start would require an exception.
std::optional<TrackHints> TrackHintsOn(const Track& track, const ConverterHints& song)
{
    TrackHints hints;
    hints.track = track.number;
    hints.opening = OpeningOf(track);
    std::vector<Block> blocks;
    try
    {
        blocks = BlocksOf(track, hints, song);
    }
    catch (const std::invalid_argument&)
    {
        return std::nullopt;
    }
    const std::optional<std::vector<bool>> actual = ActualCalls(track, blocks);
    if (!actual)
    {
        return std::nullopt;
    }

    // An exception can make another copy of a bar qualify. The rounds go on until the calls settle.
    for (int round = 0; round < kRounds; ++round)
    {
        const std::vector<bool> ruled = CalledBlocks(blocks, hints);
        bool settled = true;
        for (std::size_t i = 0; i < blocks.size(); ++i)
        {
            if (ruled[i] == (*actual)[i])
            {
                continue;
            }
            if (!blocks[i].bar)
            {
                return std::nullopt;
            }

            std::vector<uint32_t>& list = (*actual)[i] ? hints.called : hints.kept_inline;
            list.insert(std::upper_bound(list.begin(), list.end(), *blocks[i].bar), *blocks[i].bar);
            settled = false;
        }
        if (settled)
        {
            return hints;
        }
    }

    return std::nullopt;
}

// The runs of the bars that start at `starts`, from tick 0. The bars of the last run go on past the starts, and the run
// keeps one bar.
std::vector<BarRun> RunsOf(const std::vector<uint32_t>& starts)
{
    std::vector<BarRun> runs;
    for (std::size_t i = 0; i + 1 < starts.size(); ++i)
    {
        const uint32_t length = starts[i + 1] - starts[i];
        if (!runs.empty() && runs.back().length == length)
        {
            ++runs.back().count;
        }
        else
        {
            runs.push_back({length, 1});
        }
    }
    if (!runs.empty())
    {
        runs.back().count = 1;
    }

    return runs;
}

// The hints that give `song`'s calls on the bars that start at `starts`. Nothing when the bars don't fit the calls.
std::optional<ConverterHints> HintsOn(const Song& song, const std::vector<uint32_t>& starts)
{
    ConverterHints hints;
    hints.bars = RunsOf(starts);
    for (const Track& track : song.tracks)
    {
        std::optional<TrackHints> track_hints = TrackHintsOn(track, hints);
        if (!track_hints)
        {
            return std::nullopt;
        }
        if (!track_hints->opening.empty() || !track_hints->kept_inline.empty() || !track_hints->called.empty())
        {
            hints.tracks.push_back(std::move(*track_hints));
        }
    }

    return hints;
}

// The number of bars that `hints` names as exceptions to the rules.
std::size_t ExceptionsOf(const ConverterHints& hints)
{
    std::size_t exceptions = 0;
    for (const TrackHints& track : hints.tracks)
    {
        exceptions += track.kept_inline.size() + track.called.size();
    }

    return exceptions;
}

// The song's timebase, in ticks a quarter note.
uint32_t TimebaseOf(const Song& song)
{
    for (const Command& command : song.header)
    {
        if (command.kind == Kind::kTimebase && command.value.source == Source::kValue && command.value.value >= kOne &&
            command.value.value % kOne == 0)
        {
            return static_cast<uint32_t>(command.value.value / kOne);
        }
    }

    return kDefaultTimebase;
}

// The starts of bars of `length` ticks from tick 0, up to and including the first one at or after `end`.
std::vector<uint32_t> EvenBars(uint32_t length, uint32_t end)
{
    std::vector<uint32_t> starts;
    for (uint32_t tick = 0;; tick += length)
    {
        starts.push_back(tick);
        if (tick >= end)
        {
            return starts;
        }
    }
}

// The starts of the bars that the calls of a song fix. `lines` holds the calls' starts and ends, and `lengths` their
// lengths, the commonest first. `fill` gives the bars of a stretch between two lines that no length fills, and
// `tail_bar` puts one bar after the last line instead of bars of the commonest length. The starts reach `end` or past
// it.
std::vector<uint32_t> CalledBars(const std::set<uint32_t>& lines, const std::vector<uint32_t>& lengths, uint32_t end,
                                 Fill fill, bool tail_bar)
{
    const uint32_t bar = lengths.front();
    std::vector<uint32_t> starts = {0};
    const auto add = [&starts](uint32_t tick)
    {
        if (tick > starts.back())
        {
            starts.push_back(tick);
        }
    };

    // Bars of the commonest length lead up to the first line, after a short first bar.
    const uint32_t first = *lines.begin();
    for (uint32_t tick = first % bar; tick < first; tick += bar)
    {
        add(tick);
    }
    add(first);

    // The stretches between the lines.
    uint32_t previous = first;
    for (const uint32_t line : lines)
    {
        if (line <= previous)
        {
            continue;
        }

        const uint32_t span = line - previous;
        const auto fits =
            std::find_if(lengths.begin(), lengths.end(), [span](uint32_t length) { return span % length == 0; });
        const uint32_t step = fits != lengths.end() ? *fits : bar;
        uint32_t from = previous + step;
        if (fits == lengths.end() && fill == Fill::kShortFirst)
        {
            from = previous + span % bar;
        }
        if (fits != lengths.end() || fill != Fill::kOneBar)
        {
            for (uint32_t tick = from; tick < line; tick += step)
            {
                add(tick);
            }
        }
        add(line);
        previous = line;
    }

    // After the last line, bars of the commonest length or one bar reach the end.
    if (tail_bar && previous < end)
    {
        add(end);
    }
    else
    {
        for (uint32_t tick = previous + bar;; tick += bar)
        {
            add(tick);
            if (tick >= end)
            {
                break;
            }
        }
    }

    return starts;
}

// The bar lines to try for `song`, as the starts of its bars from tick 0 to past its end. Each call plays one bar, and
// the calls' starts and ends are bar lines. Bars of the calls' commonest length fill the stretches between them where
// they fit, or else bars of another length of the calls. In a stretch that none of these lengths fills, the bars have
// lengths that no call shows. The candidates differ there: bars of the commonest length with a shorter one at the end
// or at the start, or one bar for the whole stretch. For a song without calls, the candidates are bars of several
// lengths, then one bar for the whole song.
std::vector<BarCandidate> BarCandidates(const Song& song)
{
    // The song's end, and the bar lines and the lengths of its calls.
    uint32_t end = 1;
    std::set<uint32_t> lines;
    std::map<uint32_t, std::size_t> counts;
    for (const Track& track : song.tracks)
    {
        end = std::max(end, track.end_tick);
        if (!track.events.empty())
        {
            end = std::max(end, track.events.back().tick + 1);
        }
        for (const Call& call : track.calls)
        {
            lines.insert(call.tick);
            if (call.length > 0)
            {
                lines.insert(call.tick + call.length);
                ++counts[call.length];
            }
        }
    }

    std::vector<BarCandidate> candidates;
    if (counts.empty())
    {
        const uint32_t timebase = TimebaseOf(song);
        for (const uint32_t quarters : kBarsInQuarterNotes)
        {
            candidates.push_back({EvenBars(quarters * timebase, end), false});
        }
        candidates.push_back({{0, end}, true});
        return candidates;
    }

    std::vector<uint32_t> lengths;
    for (const auto& [length, count] : counts)
    {
        lengths.push_back(length);
    }
    const auto commoner = [&counts](uint32_t a, uint32_t b)
    {
        return counts.at(a) > counts.at(b);
    };
    std::stable_sort(lengths.begin(), lengths.end(), commoner);

    for (const auto& [fill, tail_bar] : {std::pair(Fill::kLongFirst, false), std::pair(Fill::kShortFirst, false),
                                         std::pair(Fill::kOneBar, false), std::pair(Fill::kOneBar, true)})
    {
        BarCandidate candidate{CalledBars(lines, lengths, end, fill, tail_bar), false};
        if (std::find(candidates.begin(), candidates.end(), candidate) == candidates.end())
        {
            candidates.push_back(std::move(candidate));
        }
    }

    return candidates;
}

// A short text of `command`, such as "wait 96", for the messages.
std::string TextOf(const Command& command)
{
    std::string text(ntdlib::commands::Name(command.kind));
    if (command.kind == Kind::kNote)
    {
        text += " " + std::to_string(command.key);
    }
    if (command.value.source == Source::kValue && command.value.value % kOne == 0)
    {
        text += " " + std::to_string(command.value.value / kOne);
    }

    return text;
}

// Where `layout` first differs from `sequence`, in words: the first command that differs, then the first target, then
// the first label.
std::string FirstDifference(const Sequence& sequence, const Sequence& layout)
{
    const std::size_t count = std::min(sequence.items.size(), layout.items.size());
    for (std::size_t i = 0; i < count; ++i)
    {
        Command held = sequence.items[i];
        Command laid = layout.items[i];
        held.target = laid.target;
        held.target_outside = laid.target_outside;
        if (!(held == laid))
        {
            return "item " + std::to_string(i) + ": " + TextOf(held) + " in the sequence, " + TextOf(laid) +
                   " in the layout";
        }
    }
    if (sequence.items.size() != layout.items.size())
    {
        return std::to_string(sequence.items.size()) + " items in the sequence, " +
               std::to_string(layout.items.size()) + " in the layout";
    }
    for (std::size_t i = 0; i < count; ++i)
    {
        if (!(sequence.items[i] == layout.items[i]))
        {
            return "item " + std::to_string(i) + ": " + TextOf(sequence.items[i]) + " to item " +
                   std::to_string(sequence.items[i].target) + " in the sequence, to item " +
                   std::to_string(layout.items[i].target) + " in the layout";
        }
    }
    for (std::size_t i = 0; i < std::min(sequence.labels.size(), layout.labels.size()); ++i)
    {
        if (!(sequence.labels[i] == layout.labels[i]))
        {
            return "label " + sequence.labels[i].name + " at item " + std::to_string(sequence.labels[i].item) +
                   ", the layout's " + layout.labels[i].name + " at item " + std::to_string(layout.labels[i].item);
        }
    }

    return std::to_string(sequence.labels.size()) + " labels in the sequence, " + std::to_string(layout.labels.size()) +
           " in the layout";
}

// The anchors of the loop of `track`: the label that a jump that runs always goes back to, and the jump. Nothing for a
// track without such a jump.
std::optional<std::pair<std::size_t, std::size_t>> LoopAnchors(const Track& track)
{
    for (std::size_t jump = 0; jump < track.anchors.size(); ++jump)
    {
        const Anchor& anchor = track.anchors[jump];
        if (!anchor.command || anchor.command->kind != Kind::kJump || anchor.command->conditional)
        {
            continue;
        }
        for (std::size_t label = 0; label < jump; ++label)
        {
            if (!track.anchors[label].command && track.anchors[label].label == anchor.target)
            {
                return std::pair(label, jump);
            }
        }
    }

    return std::nullopt;
}

// The number of `track`'s events before `tick`.
std::size_t EventsBefore(const Track& track, uint32_t tick)
{
    const auto before = [tick](const Event& event)
    {
        return event.tick < tick;
    };
    return static_cast<std::size_t>(std::find_if_not(track.events.begin(), track.events.end(), before) -
                                    track.events.begin());
}

// The tick at which the last note of `track` ends, or the tick of its last event.
uint32_t LastSound(const Track& track)
{
    uint32_t last = 0;
    for (const Event& event : track.events)
    {
        last = std::max(last, event.tick);
        if (event.command.kind == Kind::kNote && event.command.value.source == Source::kValue &&
            event.command.value.value > 0 && event.command.value.value % kOne == 0)
        {
            last = static_cast<uint32_t>(
                std::max<uint64_t>(last, event.tick + static_cast<uint64_t>(event.command.value.value / kOne)));
        }
    }

    return last;
}

} // namespace

ConvertedSong ReadConverted(const Sequence& sequence)
{
    Song song;
    try
    {
        song = ReadSong(sequence);
    }
    catch (const std::invalid_argument& error)
    {
        return {std::nullopt, error.what()};
    }

    // The hints of each candidate's bars, with the fewest exceptions first. One bar for a whole song without calls
    // comes last: it fits any such song, and it shows nothing of the song's bars.
    std::vector<std::pair<std::pair<bool, std::size_t>, ConverterHints>> ranked;
    for (const BarCandidate& candidate : BarCandidates(song))
    {
        std::optional<ConverterHints> hints = HintsOn(song, candidate.starts);
        if (hints)
        {
            ranked.emplace_back(std::pair(candidate.whole, ExceptionsOf(*hints)), std::move(*hints));
        }
    }
    const auto fewer = [](const auto& a, const auto& b)
    {
        return a.first < b.first;
    };
    std::stable_sort(ranked.begin(), ranked.end(), fewer);

    // The first hints that lay the song out as the sequence holds it.
    std::string problem = ranked.empty() ? "no bar lines fit the song's calls" : std::string();
    for (const auto& [rank, hints] : ranked)
    {
        try
        {
            const Sequence layout = LayOut(song, hints, true);
            if (layout == sequence)
            {
                return {hints, {}};
            }
            if (problem.empty())
            {
                problem = FirstDifference(sequence, layout);
            }
        }
        catch (const std::invalid_argument& error)
        {
            if (problem.empty())
            {
                problem = error.what();
            }
        }
    }

    return {std::nullopt, problem};
}

Sequence LayOutConverted(const Sequence& sequence, const ConverterHints& hints, bool subroutines)
{
    return LayOut(ReadSong(sequence), hints, subroutines);
}

Sequence SettledLayout(const Sequence& before, const Sequence& edited, ConverterHints& hints)
{
    Song song = ReadSong(edited);
    const Song old = ReadSong(before);
    for (Track& track : song.tracks)
    {
        // The splits that the track held before the edit, by their ticks.
        std::multiset<uint32_t> kept;
        for (const Track& previous : old.tracks)
        {
            if (previous.number != track.number)
            {
                continue;
            }

            for (const Event& event : previous.events)
            {
                if (event.split)
                {
                    kept.insert(event.tick);
                }
            }
        }

        // The other splits go. An anchor counts the events before it without them.
        std::vector<Event> events;
        std::vector<std::size_t> gone_before = {0};
        for (const Event& event : track.events)
        {
            const auto found = event.split ? kept.find(event.tick) : kept.end();
            const bool gone = event.split && found == kept.end();
            if (found != kept.end())
            {
                kept.erase(found);
            }
            if (!gone)
            {
                events.push_back(event);
            }
            gone_before.push_back(gone_before.back() + (gone ? 1 : 0));
        }
        for (Anchor& anchor : track.anchors)
        {
            anchor.position -= gone_before[anchor.position];
        }
        track.events = std::move(events);
    }

    // A track's blocks before the song's start have to fit its commands at tick 0. A track without hints has no entry.
    for (TrackHints& track_hints : hints.tracks)
    {
        for (const Track& track : song.tracks)
        {
            if (track.number != track_hints.track || track_hints.opening.empty())
            {
                continue;
            }

            try
            {
                BlocksOf(track, track_hints, hints);
            }
            catch (const std::invalid_argument&)
            {
                track_hints.opening.clear();
            }
        }
    }
    const auto empty = [](const TrackHints& track_hints)
    {
        return track_hints.opening.empty() && track_hints.kept_inline.empty() && track_hints.called.empty();
    };
    std::erase_if(hints.tracks, empty);

    // The song has to take its subroutines.
    LayOut(song, hints, true);

    return LayOut(song, hints, false);
}

std::vector<std::optional<uint32_t>> ItemsAfterJoins(const Sequence& split, const Sequence& joined)
{
    std::vector<std::optional<uint32_t>> items(split.items.size() + 1);

    const auto ticks = [](const Command& command)
    {
        return command.kind == Kind::kWait ? std::optional<uint32_t>(TicksOf(command.value)) : std::nullopt;
    };

    // The targets move with the joins.
    const auto same = [](Command a, const Command& b)
    {
        a.target = b.target;
        a.target_outside = b.target_outside;

        return a == b;
    };

    std::size_t j = 0;
    for (std::size_t i = 0; i < split.items.size();)
    {
        if (j < joined.items.size() && same(split.items[i], joined.items[j]))
        {
            items[i++] = static_cast<uint32_t>(j++);
            continue;
        }

        // A run of waits joined into one.
        const std::optional<uint32_t> whole = j < joined.items.size() ? ticks(joined.items[j]) : std::nullopt;
        uint64_t sum = 0;
        while (whole && i < split.items.size() && sum < *whole && ticks(split.items[i]))
        {
            sum += *ticks(split.items[i]);
            items[i++] = static_cast<uint32_t>(j);
        }
        if (!whole || sum != *whole)
        {
            throw std::invalid_argument("the layouts differ in more than their waits");
        }
        ++j;
    }
    if (j != joined.items.size())
    {
        throw std::invalid_argument("the layouts differ in more than their waits");
    }
    items.back() = static_cast<uint32_t>(j);

    return items;
}

std::vector<std::optional<uint32_t>> MatchingItems(const Sequence& from, const Sequence& to)
{
    // The header is the same in both layouts: the items before the first track. The end matches the end.
    const std::size_t header = std::min(ReadSong(from).header.size(), ReadSong(to).header.size());
    std::vector<std::optional<uint32_t>> items(from.items.size() + 1);
    for (std::size_t item = 0; item < header; ++item)
    {
        items[item] = static_cast<uint32_t>(item);
    }
    items.back() = static_cast<uint32_t>(to.items.size());

    // A label that both layouts hold.
    std::map<std::string, uint32_t> labels;
    for (const Label& label : to.labels)
    {
        labels.emplace(label.name, label.item);
    }
    for (const Label& label : from.labels)
    {
        const auto found = labels.find(label.name);
        if (label.item >= header && label.item < from.items.size() && found != labels.end())
        {
            items[label.item] = found->second;
        }
    }

    return items;
}

std::optional<uint32_t> TrackOpening(const Sequence& timeline, const ConverterHints& hints, uint8_t track)
{
    const Song song = ReadSong(timeline);
    for (const Track& candidate : song.tracks)
    {
        if (candidate.number != track)
        {
            continue;
        }

        // After the track's label and the commands that open it, and after the blocks before the song's start.
        std::size_t opening = 0;
        for (const uint32_t count : HintsOfTrack(hints, track).opening)
        {
            opening += count;
        }
        if (opening > 0 && opening <= candidate.events.size())
        {
            return candidate.events[opening - 1].item + 1;
        }
        for (const Label& label : timeline.labels)
        {
            if (label.name == candidate.label)
            {
                return label.item + static_cast<uint32_t>(candidate.prefix.size());
            }
        }
    }

    return std::nullopt;
}

std::optional<uint8_t> TrackRunFrom(const Sequence& timeline, uint32_t first)
{
    const Song song = ReadSong(timeline);
    std::map<uint32_t, uint8_t> tracks;
    for (const Track& track : song.tracks)
    {
        for (const Label& label : timeline.labels)
        {
            if (label.name == track.label)
            {
                tracks.emplace(label.item, track.number);
            }
        }
    }

    // A path through the header. A return there has no call to return to, and the player runs on past it.
    const auto header_end = static_cast<uint32_t>(song.header.size());
    std::vector<bool> visited(header_end, false);
    uint32_t item = first;
    while (item < header_end)
    {
        const Command& command = timeline.items[item];
        if (visited[item] || (command.kind == Kind::kEnd && !command.conditional) ||
            (command.kind == Kind::kJump && !command.conditional && command.target_outside))
        {
            return std::nullopt;
        }
        visited[item] = true;
        item = command.kind == Kind::kJump && !command.conditional ? command.target : item + 1;
    }

    const auto track = tracks.find(item);
    return track == tracks.end() ? std::nullopt : std::optional<uint8_t>(track->second);
}

std::optional<ConvertedStart> SongStartFrom(const Sequence& timeline, uint32_t first)
{
    const Song song = ReadSong(timeline);
    std::set<uint32_t> tracks;
    for (const Track& track : song.tracks)
    {
        for (const Label& label : timeline.labels)
        {
            if (label.name == track.label)
            {
                tracks.insert(label.item);
            }
        }
    }

    // A path through the header up to the first command that starts a track. TrackRunFrom follows the same path.
    const auto header_end = static_cast<uint32_t>(song.header.size());
    if (first > header_end)
    {
        return std::nullopt;
    }
    std::vector<bool> visited(header_end, false);
    ConvertedStart start{first, 0};
    while (start.item < header_end)
    {
        const Command& command = timeline.items[start.item];
        const bool jumps = command.kind == Kind::kJump && !command.conditional;
        if ((command.kind == Kind::kOpenTrack && !command.conditional && !command.target_outside &&
             tracks.contains(command.target)) ||
            (jumps && !command.target_outside && tracks.contains(command.target)))
        {
            return start;
        }
        if (visited[start.item] || (command.kind == Kind::kEnd && !command.conditional) ||
            (jumps && command.target_outside))
        {
            return std::nullopt;
        }
        if (command.kind == Kind::kWait)
        {
            if (command.conditional || command.value.source != Source::kValue || command.value.value < 0)
            {
                return std::nullopt;
            }
            start.tick += static_cast<uint32_t>(command.value.value / kOne);
        }
        visited[start.item] = true;
        start.item = jumps ? command.target : start.item + 1;
    }

    return tracks.contains(start.item) ? std::optional(start) : std::nullopt;
}

std::optional<std::map<std::string, uint32_t>> TrackStartsFrom(const Sequence& timeline, uint32_t first)
{
    const Song song = ReadSong(timeline);
    std::map<uint32_t, std::string> tracks;
    for (const Track& track : song.tracks)
    {
        for (const Label& label : timeline.labels)
        {
            if (label.name == track.label)
            {
                tracks.emplace(label.item, track.label);
            }
        }
    }

    // A track that the path doesn't start takes the tick where the header starts the song's first track.
    std::map<std::string, uint32_t> starts;
    const auto finish = [&]() -> std::optional<std::map<std::string, uint32_t>>
    {
        const std::optional<ConvertedStart> song_start = SongStartFrom(timeline, first);
        if (starts.empty() || !song_start)
        {
            return std::nullopt;
        }
        for (const Track& track : song.tracks)
        {
            starts.emplace(track.label, song_start->tick);
        }

        return starts;
    };

    // A path through the header, on past the commands that open tracks. SongStartFrom follows the same path.
    const auto header_end = static_cast<uint32_t>(song.header.size());
    if (first > header_end)
    {
        return std::nullopt;
    }
    std::vector<bool> visited(header_end, false);
    uint32_t item = first;
    uint32_t tick = 0;
    while (item < header_end)
    {
        const Command& command = timeline.items[item];
        const bool jumps = command.kind == Kind::kJump && !command.conditional;
        const bool opens = command.kind == Kind::kOpenTrack && !command.conditional;
        const auto track = (jumps || opens) && !command.target_outside ? tracks.find(command.target) : tracks.end();
        if (track != tracks.end())
        {
            starts.emplace(track->second, tick);
            if (jumps)
            {
                return finish();
            }
        }
        if (visited[item] || (command.kind == Kind::kEnd && !command.conditional) || (jumps && command.target_outside))
        {
            return finish();
        }
        if (command.kind == Kind::kWait)
        {
            if (command.conditional || command.value.source != Source::kValue || command.value.value < 0)
            {
                return std::nullopt;
            }
            tick += static_cast<uint32_t>(command.value.value / kOne);
        }
        visited[item] = true;
        item = jumps ? command.target : item + 1;
    }
    if (const auto run_into = tracks.find(item); run_into != tracks.end())
    {
        starts.emplace(run_into->second, tick);
    }

    return finish();
}

ConvertedTrackPlace NewTrackPlace(const Sequence& timeline, uint8_t number)
{
    const Song song = ReadSong(timeline);
    const std::string prefix = std::string(kLabelPrefix) + song.name + "_";
    ConvertedTrackPlace place{prefix + "Track_" + std::to_string(number), 0, {}};
    std::map<std::string, uint32_t> items;
    for (const Label& label : timeline.labels)
    {
        items.emplace(label.name, label.item);
    }

    std::optional<uint32_t> next;
    for (const Track& track : song.tracks)
    {
        if (track.number == number)
        {
            throw std::invalid_argument("The timeline holds a track " + std::to_string(number) +
                                        " that the sound doesn't start.");
        }

        place.tracks.push_back(track.label);
        if (!next && track.number > number)
        {
            next = items.at(track.label);
        }
    }
    place.before = next ? *next : items.at(prefix + "End");

    return place;
}

std::optional<ConvertedLoop> LoopOfConverted(const Sequence& timeline)
{
    std::optional<ConvertedLoop> loop;
    for (const Track& track : ReadSong(timeline).tracks)
    {
        const std::optional<std::pair<std::size_t, std::size_t>> anchors = LoopAnchors(track);
        if (!anchors)
        {
            continue;
        }

        const ConvertedLoop found{track.anchors[anchors->first].tick, track.anchors[anchors->second].tick};
        if (loop && *loop != found)
        {
            return std::nullopt;
        }
        loop = found;
    }

    return loop;
}

std::optional<std::vector<ConvertedLoop>> SongLoopsOf(const Sequence& timeline)
{
    std::optional<std::map<std::string, uint32_t>> starts;
    std::vector<ConvertedLoop> loops;
    for (const Track& track : ReadSong(timeline).tracks)
    {
        const std::optional<std::pair<std::size_t, std::size_t>> anchors = LoopAnchors(track);
        if (!anchors)
        {
            continue;
        }
        if (!starts)
        {
            starts = TrackStartsFrom(timeline, 0);
        }
        const auto start = starts ? starts->find(track.label) : std::map<std::string, uint32_t>::const_iterator();
        if (!starts || start == starts->end())
        {
            return std::nullopt;
        }
        loops.push_back(
            {track.anchors[anchors->first].tick + start->second, track.anchors[anchors->second].tick + start->second});
    }

    return loops;
}

Sequence WithLoop(const Sequence& timeline, const ConverterHints& hints, ConvertedLoop loop,
                  const std::map<std::string, uint32_t>& starts)
{
    if (loop.end <= loop.start)
    {
        throw std::invalid_argument("The loop has to end after its start.");
    }

    Song song = ReadSong(timeline);
    bool looped = false;
    for (Track& track : song.tracks)
    {
        const std::optional<std::pair<std::size_t, std::size_t>> anchors = LoopAnchors(track);
        if (!anchors)
        {
            continue;
        }

        // The track counts the loop's ticks from its start.
        const auto start = starts.find(track.label);
        const uint32_t offset = start != starts.end() ? start->second : 0;
        if (loop.start < offset)
        {
            throw std::invalid_argument("The loop can't start before the song's tracks start.");
        }
        const ConvertedLoop counted{loop.start - offset, loop.end - offset};

        // The label and the jump take their new ticks, each before the commands at its tick. A track can jump back to
        // the label more than once at the loop's end. Each of those jumps takes the new end. The anchors stay in the
        // order of their ticks.
        looped = true;
        const uint32_t old_end = track.anchors[anchors->second].tick;
        std::vector<std::pair<std::size_t, uint32_t>> moves = {{anchors->first, counted.start}};
        for (std::size_t anchor = anchors->second; anchor < track.anchors.size(); ++anchor)
        {
            const Anchor& jump = track.anchors[anchor];
            if (jump.tick == old_end && jump.command && jump.command->kind == Kind::kJump &&
                !jump.command->conditional && jump.target == track.anchors[anchors->first].label)
            {
                moves.emplace_back(anchor, counted.end);
            }
        }
        for (const auto& [anchor, tick] : moves)
        {
            track.anchors[anchor].tick = tick;
            track.anchors[anchor].position = EventsBefore(track, tick);
        }
        std::stable_sort(track.anchors.begin(), track.anchors.end(),
                         [](const Anchor& a, const Anchor& b) { return a.tick < b.tick; });

        // Commands after the jump end with an end, at the tick where the track ended. Without them, the track ends with
        // its jump.
        if (EventsBefore(track, counted.end) < track.events.size())
        {
            if (!track.end)
            {
                track.end = Command{.kind = Kind::kEnd};
                track.end_tick = std::max({old_end, track.end_tick, LastSound(track)});
            }
        }
        else
        {
            track.end.reset();
            track.after_end.clear();
            track.end_tick = counted.end;
        }
        track.end_tick = std::max(track.end_tick, counted.end);
    }
    if (!looped)
    {
        throw std::invalid_argument("The song has no loop.");
    }

    // A loop without a note sounds nothing. ntdlib's performer then finds no loop, and the arrangement shows none.
    const auto sounds = [&loop, &starts](const Track& track)
    {
        const auto start = starts.find(track.label);
        const uint32_t offset = start != starts.end() ? start->second : 0;
        const uint32_t from = loop.start > offset ? loop.start - offset : 0;
        const uint32_t to = loop.end > offset ? loop.end - offset : 0;
        const auto sounds_in_loop = [from, to](const Event& event)
        {
            return event.command.kind == Kind::kNote && event.command.velocity > 0 && event.tick >= from &&
                   event.tick < to;
        };
        return std::ranges::any_of(track.events, sounds_in_loop);
    };
    if (std::ranges::none_of(song.tracks, sounds))
    {
        throw std::invalid_argument("The loop would hold no note. The sound would go silent after its last note.");
    }

    return LayOut(song, hints, false);
}

Sequence EmptySong(const std::string& name)
{
    const std::string prefix = std::string(kLabelPrefix) + name + "_";

    const auto command = [](Kind kind, int64_t value)
    {
        Command made;
        made.kind = kind;
        made.value.value = value * kOne;

        return made;
    };

    Sequence song;
    song.items = {command(Kind::kAllocateTracks, 1), command(Kind::kNoteWait, 0), command(Kind::kTempo, 120),
                  command(Kind::kEnd, 0), command(Kind::kEnd, 0)};
    song.labels = {{prefix + "Begin", 0},
                   {prefix + "End", 4},
                   {prefix + "Start", kEmptySongStart},
                   {prefix + "Track_0", kEmptySongStart}};

    return song;
}

} // namespace hokuto::edit
