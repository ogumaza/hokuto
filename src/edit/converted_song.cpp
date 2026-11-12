// SPDX-License-Identifier: MIT

// The reading of a song that NintendoWare's MIDI converter wrote (edit/converted_song.h): its header, the events and
// anchors of its tracks, and its calls.

#include "edit/converted_song.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ntdlib/commands/units.h"

#include "edit/bytecode.h"

namespace hokuto::edit
{

namespace
{

using converter_detail::Call;
using converter_detail::kLabelPrefix;
using converter_detail::kPlacePrefix;
using converter_detail::Song;
using converter_detail::TicksOf;
using converter_detail::Track;
using ntdlib::commands::Command;
using ntdlib::commands::Kind;
using ntdlib::commands::kOne;
using ntdlib::commands::Label;
using ntdlib::commands::Sequence;

} // namespace

namespace converter_detail
{

uint32_t TicksOf(const ntdlib::commands::Argument& argument)
{
    const std::optional<uint32_t> ticks = WholeTicks(argument);
    if (!ticks || argument.value / kOne > UINT32_MAX)
    {
        throw std::invalid_argument("a length that isn't a fixed number of ticks");
    }

    return *ticks;
}

} // namespace converter_detail

namespace
{

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

// Walks track `number` of `sequence` from its label at item `start` (ReadSong). `labels` contains the labels by their
// items, and `prefix` starts the labels of the song. Walk follows the track's code: calls into its subroutines and
// back, up to its end or a jump that ends it.
class TrackWalk
{
public:
    TrackWalk(const Sequence& sequence, const std::multimap<uint32_t, std::string>& labels, const std::string& prefix,
              uint8_t number, uint32_t start)
        : sequence_(sequence), labels_(labels), prefix_(prefix), start_(start), item_(start)
    {
        track_.number = number;
        track_.label = prefix + "Track_" + std::to_string(number);
        subroutines_ = track_.label + "_";
    }

    Track Walk();

private:
    // The track's labels outside its subroutines. A subroutine has a label at its first command only.
    // AddTrackLabels adds the labels at the current item as anchors at the current tick, or as start labels at the
    // track's start. Returns whether a label names the item.
    bool AddTrackLabels();

    // Takes `command`, the command at the current item. Returns whether the track ends there.
    bool Take(const Command& command);

    // Enters the subroutine that `command`, a call, calls.
    void EnterCall(const Command& command);

    // Returns from a subroutine to the command after its call.
    void Return();

    // Takes `command`, a jump. Returns whether the track ends with it.
    bool TakeJump(const Command& command);

    // Ends the track with `command`, the end at the current item. The commands after the end that nothing reaches, up
    // to the next part of the song, go with it.
    void EndTrack(const Command& command);

    // Takes `command`, a note-wait setting at the current item. The track's notes then no longer wait.
    void TakeNoteWait(const Command& command);

    // Takes `command`, an event or a command that the converter doesn't write.
    void TakeEvent(const Command& command);

    const Sequence& sequence_;
    const std::multimap<uint32_t, std::string>& labels_;
    const std::string& prefix_;
    const uint32_t start_;
    Track track_;
    std::string subroutines_;

    // The current item and tick, the subroutine that the track is in and the item after its call, whether the command
    // before was a wait, and whether the track's notes still wait.
    uint32_t item_;
    uint32_t tick_ = 0;
    std::optional<Call> call_;
    uint32_t back_ = 0;
    bool after_wait_ = false;
    bool notes_wait_ = true;
};

Track TrackWalk::Walk()
{
    for (;;)
    {
        if (item_ >= sequence_.items.size())
        {
            throw std::invalid_argument("a track runs past the end of the sequence");
        }

        if (AddTrackLabels())
        {
            after_wait_ = false;
        }

        // A jump can run only if a comparison held. Any other command that moves the track's time or its place in the
        // code runs always.
        const Command& command = sequence_.items[item_];
        if (command.conditional && !IsEvent(command) && command.kind != Kind::kJump)
        {
            throw std::invalid_argument("a command that runs only if a comparison is true: " +
                                        std::string(ntdlib::commands::Name(command.kind)));
        }

        const bool wait = command.kind == Kind::kWait;
        if (wait && after_wait_)
        {
            track_.events.push_back({tick_, {}, true, item_});
        }

        after_wait_ = wait;
        if (Take(command))
        {
            return std::move(track_);
        }
    }
}

bool TrackWalk::AddTrackLabels()
{
    const auto [first, last] = labels_.equal_range(item_);
    for (auto label = first; label != last; ++label)
    {
        if (call_)
        {
            if (label->second != call_->name || track_.events.size() != call_->first)
            {
                throw std::invalid_argument("a label inside a subroutine");
            }

            continue;
        }

        if (item_ != start_)
        {
            track_.anchors.push_back({track_.events.size(), tick_, label->second, std::nullopt, {}});
        }
        else if (label->second != track_.label)
        {
            track_.start_labels.push_back(label->second);
        }
    }

    return first != last;
}

bool TrackWalk::Take(const Command& command)
{
    switch (command.kind)
    {
    case Kind::kWait:
        tick_ += TicksOf(command.value);
        ++item_;
        return false;

    case Kind::kCall:
        EnterCall(command);
        return false;

    case Kind::kReturn:
        Return();
        return false;

    case Kind::kJump:
        return TakeJump(command);

    case Kind::kEnd:
        EndTrack(command);
        return true;

    case Kind::kLoopStart:
    case Kind::kLoopEnd:
        if (call_)
        {
            throw std::invalid_argument("a loop inside a subroutine");
        }

        track_.anchors.push_back({track_.events.size(), tick_, {}, command, {}});
        ++item_;
        return false;

    case Kind::kNoteWait:
        TakeNoteWait(command);
        ++item_;
        return false;

    default:
        TakeEvent(command);
        ++item_;
        return false;
    }
}

void TrackWalk::EnterCall(const Command& command)
{
    const std::string name = command.target_outside ? std::string() : LabelAt(labels_, command.target);
    if (call_ || !name.starts_with(subroutines_))
    {
        throw std::invalid_argument("a call that the converter doesn't write");
    }

    call_ = Call{tick_, 0, name, track_.events.size(), 0};
    back_ = item_ + 1;
    item_ = command.target;
}

void TrackWalk::Return()
{
    if (!call_)
    {
        throw std::invalid_argument("a return outside a subroutine");
    }

    call_->length = tick_ - call_->tick;
    call_->count = track_.events.size() - call_->first;
    track_.calls.push_back(*call_);
    call_.reset();
    item_ = back_;
}

bool TrackWalk::TakeJump(const Command& command)
{
    const std::string target = command.target_outside ? std::string() : LabelAt(labels_, command.target);
    if (call_ || !(target.starts_with(prefix_) || target.starts_with(kPlacePrefix)))
    {
        throw std::invalid_argument("a jump that the converter doesn't write");
    }

    track_.anchors.push_back({track_.events.size(), tick_, {}, command, target});
    ++item_;

    // A track can end with a jump that runs always. Its subroutines, the next track or the song's end follow.
    if (!command.conditional && (item_ == sequence_.items.size() || StartsPart(labels_, item_, prefix_)))
    {
        track_.end_tick = tick_;
        return true;
    }

    return false;
}

void TrackWalk::EndTrack(const Command& command)
{
    if (call_)
    {
        throw std::invalid_argument("an end inside a subroutine");
    }

    track_.end = command;
    track_.end_tick = tick_;

    // Commands that nothing reaches can follow the end, up to the next part of the song, when a hand cut the
    // track short.
    for (uint32_t item = item_ + 1; item < sequence_.items.size() && !StartsPart(labels_, item, prefix_); ++item)
    {
        if (labels_.contains(item) || ntdlib::commands::HasTarget(sequence_.items[item].kind))
        {
            throw std::invalid_argument("a label or jump target after the end of a track");
        }

        track_.after_end.push_back(sequence_.items[item]);
    }
}

void TrackWalk::TakeNoteWait(const Command& command)
{
    // The converter turns the note wait off at the track's first command. A song edited by hand can set up the
    // track before it.
    if (call_ || command.value.value != 0 || !notes_wait_)
    {
        throw std::invalid_argument("a note-wait setting that the converter doesn't write");
    }

    notes_wait_ = false;
    if (item_ == start_)
    {
        track_.prefix.push_back(command);
    }
    else
    {
        track_.events.push_back({tick_, command, false, item_});
    }
}

void TrackWalk::TakeEvent(const Command& command)
{
    if (!IsEvent(command))
    {
        throw std::invalid_argument("a command that the converter doesn't write: " +
                                    std::string(ntdlib::commands::Name(command.kind)));
    }
    if (command.kind == Kind::kNote && notes_wait_)
    {
        throw std::invalid_argument("a note that waits for its length");
    }

    track_.events.push_back({tick_, command, false, item_});
}

// The prefix of the labels of the song that `sequence` contains: the name of its Begin label at the first item, up to
// "Begin" (ReadSong). Throws std::invalid_argument for a sequence without one song at its start.
std::string SongPrefix(const Sequence& sequence)
{
    std::string prefix;
    for (const Label& label : sequence.labels)
    {
        if (label.name.starts_with(kLabelPrefix) && label.name.ends_with("_Begin"))
        {
            if (!prefix.empty() || label.item != 0)
            {
                throw std::invalid_argument("no single song at the start of the sequence");
            }

            prefix = label.name.substr(0, label.name.size() - 5);
        }
    }
    if (prefix.empty())
    {
        throw std::invalid_argument("no converted song");
    }

    return prefix;
}

// The first item and the number of each track of the song whose labels start with `prefix`, by `items`, the item of
// each label, in the order of the file (ReadSong). Throws std::invalid_argument for a song without tracks.
std::vector<std::pair<uint32_t, uint8_t>> TrackItems(const std::map<std::string, uint32_t>& items,
                                                     const std::string& prefix)
{
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

    return tracks;
}

// Stores in `song` the header of `sequence`, items 0 to `header_end`, with the labels of the targets past it, by
// `labels` and `items` (ReadSong). Throws std::invalid_argument for a header that the converter doesn't write.
void ReadHeader(const Sequence& sequence, const std::multimap<uint32_t, std::string>& labels,
                const std::map<std::string, uint32_t>& items, uint32_t header_end, Song& song)
{
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
                throw std::invalid_argument("a header that the converter doesn't write");
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
}

// Stores in `song` the End label of `sequence` and the items after it, whose labels start with `prefix`, by `items`
// (ReadSong). Throws std::invalid_argument for a song without an End label, and for commands after it that jump into
// it.
void ReadAfterEnd(const Sequence& sequence, const std::map<std::string, uint32_t>& items, const std::string& prefix,
                  Song& song)
{
    const auto end = items.find(prefix + "End");
    if (end == items.end() || end->second >= sequence.items.size() || sequence.items[end->second].kind != Kind::kEnd)
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
                throw std::invalid_argument("commands after the song that jump into it");
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
}

} // namespace

namespace converter_detail
{

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

    const std::string prefix = SongPrefix(sequence);

    Song song;
    song.name = prefix.substr(kLabelPrefix.size(), prefix.size() - kLabelPrefix.size() - 1);
    std::map<std::string, uint32_t> items;
    for (const Label& label : sequence.labels)
    {
        if (!items.emplace(label.name, label.item).second)
        {
            throw std::invalid_argument("two labels with the same name");
        }
    }

    // The tracks, by their labels, in the order of the file. The first track follows the header.
    const std::vector<std::pair<uint32_t, uint8_t>> tracks = TrackItems(items, prefix);

    // The header, from the Begin label to the first track. A command that goes into the header keeps its target. One
    // that goes past it names a label.
    const uint32_t header_end = tracks.front().first;
    ReadHeader(sequence, labels, items, header_end, song);

    for (const auto& [item, number] : tracks)
    {
        song.tracks.push_back(TrackWalk(sequence, labels, prefix, number, item).Walk());
    }

    // The End label, and the items after it. Nothing in the song names them, and they name nothing in the song.
    ReadAfterEnd(sequence, items, prefix, song);

    return song;
}

} // namespace converter_detail

} // namespace hokuto::edit
