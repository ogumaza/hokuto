// SPDX-License-Identifier: MIT

#include "edit/bytecode.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/nwsnd/commands.h"
#include "ntdlib/sar/layout.h"

#include "edit/converter.h"
#include "format/archive.h"
#include "song/archive.h"

namespace hokuto::edit
{

song::Console ConsoleOf(const format::File& file)
{
    if (!file.native)
    {
        throw std::invalid_argument("The file holds no archive.");
    }

    return file.native->wii ? song::Console{.wii = format::BuildOfWiiArchive(*file.native)}
                            : song::Console{.flavor = file.native->flavor};
}

const ntdlib::commands::Sequence& SequenceToEdit(const format::File& file, uint32_t sequence)
{
    if (!file.native || sequence >= file.sequences.size())
    {
        throw std::invalid_argument("Hokuto edits only the sequences of a sound archive.");
    }

    return *file.sequences[sequence];
}

std::vector<uint32_t> Offsets(const ntdlib::commands::Sequence& sequence, const song::Console& console)
{
    return ntdlib::nwsnd::EncodeSequence(sequence, song::GrammarOf(console)).offsets;
}

std::optional<uint32_t> ItemAtOffset(const std::vector<uint32_t>& offsets, uint32_t offset)
{
    if (offsets.empty())
    {
        return std::nullopt;
    }

    const auto found = std::lower_bound(offsets.begin(), offsets.end() - 1, offset);
    if (found == offsets.end() - 1 || *found != offset)
    {
        return std::nullopt;
    }

    return static_cast<uint32_t>(found - offsets.begin());
}

bool Fixed(const ntdlib::commands::Sequence& sequence)
{
    const auto fixed = [](const ntdlib::commands::Command& item)
    {
        return item.target_outside || item.kind == ntdlib::commands::Kind::kData;
    };
    return std::any_of(sequence.items.begin(), sequence.items.end(), fixed);
}

bool RunsOn(const ntdlib::commands::Command& command)
{
    using ntdlib::commands::Kind;
    const bool stops = command.kind == Kind::kJump || command.kind == Kind::kReturn || command.kind == Kind::kEnd;
    return !stops || command.conditional;
}

namespace
{

// The items of a sequence that the player reaches outside every call, and those that it reaches inside a call.
struct Flow
{
    std::vector<bool> outside;
    std::vector<bool> inside;
};

// The items of `sequence` that the player reaches from the items of `starts`, outside every call and inside a call.
// Reached gives the same items. A track starts outside every call. A return outside every call has no call to return
// to, and the player runs on past it.
Flow FlowFrom(const ntdlib::commands::Sequence& sequence, std::span<const uint32_t> starts, bool track_starts)
{
    using ntdlib::commands::Kind;
    const std::vector<ntdlib::commands::Command>& items = sequence.items;
    Flow flow{std::vector<bool>(items.size(), false), std::vector<bool>(items.size(), false)};
    std::vector<std::pair<uint32_t, bool>> next;
    for (const uint32_t start : starts)
    {
        next.emplace_back(start, false);
    }
    while (!next.empty())
    {
        const auto [item, in_call] = next.back();
        next.pop_back();
        std::vector<bool>& reached = in_call ? flow.inside : flow.outside;
        if (item >= items.size() || reached[item])
        {
            continue;
        }

        // A jump goes on only when it runs only if the last comparison held. A return inside a call and an end do the
        // same.
        reached[item] = true;
        const ntdlib::commands::Command& command = items[item];
        const bool target = ntdlib::commands::HasTarget(command.kind) && !command.target_outside;
        if (target && command.kind == Kind::kOpenTrack)
        {
            if (track_starts)
            {
                next.emplace_back(command.target, false);
            }
        }
        else if (target)
        {
            next.emplace_back(command.target, in_call || command.kind == Kind::kCall);
        }
        const bool returns = command.kind == Kind::kReturn && !command.conditional && in_call;
        if (command.kind != Kind::kData && (RunsOn(command) || (command.kind == Kind::kReturn && !returns)))
        {
            next.emplace_back(item + 1, in_call);
        }
    }

    return flow;
}

} // namespace

bool RunsInto(const format::File& file, uint32_t sequence, uint32_t item)
{
    using ntdlib::commands::Kind;
    const ntdlib::commands::Sequence& commands = *file.sequences[sequence];
    if (item == 0 || item > commands.items.size())
    {
        return false;
    }
    const ntdlib::commands::Command& before = commands.items[item - 1];
    if (RunsOn(before) || before.kind != Kind::kReturn)
    {
        return RunsOn(before);
    }

    // A return runs on where a sound reaches it outside every call. A sound starts at the bytecode's first item, or
    // where a field of the archive's tables names an item.
    std::vector<uint32_t> starts = {0};
    for (const ntdlib::sar::Field& field : file.native->fields)
    {
        if (NamesItemOf(*file.native, field.to, sequence))
        {
            starts.push_back(field.to.offset);
        }
    }

    return FlowFrom(commands, starts, true).outside[item - 1];
}

OpeningInsertion InsertAtOpening(format::File& file, uint32_t sequence, uint32_t first, int track,
                                 std::vector<ntdlib::commands::Command> commands)
{
    using ntdlib::commands::Command;
    using ntdlib::commands::Kind;
    ntdlib::commands::Sequence edited = SequenceToEdit(file, sequence);

    // A timeline's track takes its first settings after the commands that open it. Every path through the track runs
    // them, and the labels there move past them.
    const bool timeline = file.timelines && file.timelines->contains(sequence);
    bool opening_labels = track == 0;
    if (timeline)
    {
        const std::optional<uint8_t> number = TrackRunFrom(edited, first);
        const std::optional<uint32_t> opening =
            number ? TrackOpening(edited, file.timelines->at(sequence), *number) : std::nullopt;
        if (!opening)
        {
            throw std::invalid_argument("Track " + std::to_string(track) + " runs none of the timeline's tracks.");
        }
        first = *opening;
        opening_labels = false;
    }
    const uint32_t jumps = !timeline && RunsInto(file, sequence, first) ? 1 : 0;
    const auto count = static_cast<uint32_t>(commands.size());
    const auto moved = [first, jumps, count](uint32_t item, bool opening)
    {
        if (item < first)
        {
            return item;
        }

        return item == first && opening ? first + jumps : item + jumps + count;
    };

    const auto opens = [track](const Command& item)
    {
        return track != 0 && item.kind == Kind::kOpenTrack && item.index == track;
    };

    // A label at `first` that a jump or a call reaches goes on naming the command that they reach, past the new
    // commands. The label then keeps its jumps. The sound's start stays at the new commands.
    const auto reaches = [first, &opens](const Command& item)
    {
        return ntdlib::commands::HasTarget(item.kind) && !item.target_outside && item.target == first && !opens(item);
    };
    const bool labels_at_opening = opening_labels && std::none_of(edited.items.begin(), edited.items.end(), reaches);
    for (Command& item : edited.items)
    {
        if (ntdlib::commands::HasTarget(item.kind) && !item.target_outside)
        {
            item.target = moved(item.target, opens(item));
        }
    }
    for (ntdlib::commands::Label& label : edited.labels)
    {
        label.item = moved(label.item, labels_at_opening);
    }

    // The new commands, after the jump past them.
    std::vector<Command> added;
    if (jumps != 0)
    {
        added.push_back(Command{.kind = Kind::kJump, .target = first + 1 + count});
    }
    added.insert(added.end(), std::make_move_iterator(commands.begin()), std::make_move_iterator(commands.end()));
    edited.items.insert(edited.items.begin() + first, added.begin(), added.end());
    file.sequences[sequence] = std::move(edited);

    // A field of the archive's tables measures a sound's start from the bytecode's first item. That item stays first.
    const auto move_places = [&](format::NativeArchive& native)
    {
        for (ntdlib::sar::Field& field : native.fields)
        {
            if (NamesItemOf(native, field.from, sequence) && field.from.offset != 0)
            {
                field.from.offset = moved(field.from.offset, false);
            }
            if (NamesItemOf(native, field.to, sequence))
            {
                field.to.offset = moved(field.to.offset, opening_labels);
            }
        }
    };
    file.native.Edit(move_places);

    return {first, jumps + count};
}

std::vector<bool> Reached(const ntdlib::commands::Sequence& sequence, uint32_t start, bool track_starts)
{
    const Flow flow = FlowFrom(sequence, {&start, 1}, track_starts);
    std::vector<bool> reached(sequence.items.size(), false);
    for (std::size_t item = 0; item < reached.size(); item++)
    {
        reached[item] = flow.outside[item] || flow.inside[item];
    }

    return reached;
}

std::string UniqueLabel(const std::vector<ntdlib::commands::Label>& labels, const std::string& name)
{
    const auto taken = [&labels](const std::string& candidate)
    {
        const auto same = [&candidate](const ntdlib::commands::Label& label)
        {
            return label.name == candidate;
        };
        return std::any_of(labels.begin(), labels.end(), same);
    };

    std::string unique = name;
    for (int number = 2; taken(unique); number++)
    {
        unique = name + "_" + std::to_string(number);
    }

    return unique;
}

bool NamesItemOf(const format::NativeArchive& native, const ntdlib::sar::Place& place, uint32_t sequence)
{
    if (place.item && place.sequence)
    {
        return place.piece == sequence;
    }

    const auto* piece = place.item && place.piece < native.pieces->size()
                            ? std::get_if<ntdlib::sar::SequencePiece>(&(*native.pieces)[place.piece])
                            : nullptr;
    return piece != nullptr && piece->sequence == sequence;
}

} // namespace hokuto::edit
