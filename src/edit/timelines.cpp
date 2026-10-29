// SPDX-License-Identifier: MIT

#include "edit/timelines.h"

#include <algorithm>
#include <cstddef>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/sar/layout.h"

namespace hokuto::edit
{
namespace
{

using ntdlib::commands::Kind;
using ntdlib::commands::Sequence;

// Moves the places of the native archive of `file` that name items of sequence `sequence` to the items that `items`
// gives. Throws std::invalid_argument when a place names an item that `items` doesn't give.
void MovePlaces(format::File& file, uint32_t sequence, const std::vector<std::optional<uint32_t>>& items)
{
    if (!file.native)
    {
        return;
    }

    bool given = true;
    const auto check = [&](const ntdlib::sar::Place& place)
    {
        given = given && place.offset < items.size() && items[place.offset].has_value();
    };
    ForEachItemPlace(*file.native, sequence, check);
    if (!given)
    {
        throw std::invalid_argument("A sound starts where the two layouts of the timeline hold other commands.");
    }

    const auto move_places = [&](format::NativeArchive& native)
    {
        ForEachItemPlace(native, sequence, [&](ntdlib::sar::Place& place) { place.offset = *items[place.offset]; });
    };
    file.native.Edit(move_places);
}

// Lays sequence `sequence` of `file` out with every bar in its track, by `hints`. Throws std::invalid_argument when the
// hints don't give the sequence back byte for byte, or when a sound starts where the layouts hold other commands.
void Unfold(format::File& file, uint32_t sequence, const ConverterHints& hints)
{
    const Sequence& folded = *file.sequences[sequence];
    Sequence unfolded = LayOutConverted(folded, hints, false);
    if (LayOutConverted(unfolded, hints, true) != folded)
    {
        throw std::invalid_argument("The timeline's hints don't give its sequence back.");
    }

    MovePlaces(file, sequence, MatchingItems(folded, unfolded));
    file.sequences[sequence] = std::move(unfolded);
}

// Lays timeline `sequence` of `file` out with its subroutines.
void Fold(format::File& file, uint32_t sequence, const ConverterHints& hints)
{
    const Sequence& unfolded = *file.sequences[sequence];
    Sequence folded = LayOutConverted(unfolded, hints, true);
    MovePlaces(file, sequence, MatchingItems(unfolded, folded));
    file.sequences[sequence] = std::move(folded);
}

// Makes sequence `sequence` of `file` a timeline when the converter lays it out again byte for byte, and its sounds
// start in its header or at a label. Returns whether it did.
bool TryTimeline(format::File& file, uint32_t sequence)
{
    const ConvertedSong converted = ReadConverted(*file.sequences[sequence]);
    if (!converted.hints)
    {
        return false;
    }

    format::File timeline = file;
    try
    {
        Unfold(timeline, sequence, *converted.hints);
    }
    catch (const std::invalid_argument&)
    {
        return false;
    }
    if (!timeline.timelines)
    {
        timeline.timelines.emplace();
    }
    (*timeline.timelines)[sequence] = *converted.hints;
    file = std::move(timeline);

    return true;
}

} // namespace

bool IsTimeline(const format::File& file, uint32_t sequence)
{
    return file.timelines && file.timelines->contains(sequence);
}

format::File OpenTimelines(format::File file)
{
    if (!file.timelines)
    {
        file.timelines.emplace();
        for (uint32_t sequence = 0; sequence < file.sequences.size(); ++sequence)
        {
            TryTimeline(file, sequence);
        }

        return file;
    }

    // A timeline that its hints don't give back, or whose sounds start elsewhere, becomes a program.
    const std::map<uint32_t, ConverterHints> timelines = *file.timelines;
    for (const auto& [sequence, hints] : timelines)
    {
        try
        {
            Unfold(file, sequence, hints);
        }
        catch (const std::invalid_argument&)
        {
            file.timelines->erase(sequence);
        }
    }

    return file;
}

format::File FoldTimelines(const format::File& file)
{
    if (!file.timelines || file.timelines->empty())
    {
        return file;
    }

    format::File folded = file;
    for (const auto& [sequence, hints] : *file.timelines)
    {
        Fold(folded, sequence, hints);
    }

    return folded;
}

Sequence SavedSequence(const format::File& file, uint32_t sequence)
{
    if (!IsTimeline(file, sequence))
    {
        return SequenceToEdit(file, sequence);
    }

    return LayOutConverted(*file.sequences[sequence], file.timelines->at(sequence), true);
}

Settled SettleTimelines(const format::File& before, format::File edited, std::span<const uint32_t> replaced)
{
    // The replaced sequences and the new ones leave their modes behind.
    Settled result{std::move(edited), {}, {}};
    for (const uint32_t sequence : replaced)
    {
        if (sequence < before.sequences.size() && sequence < result.file.sequences.size() &&
            *result.file.sequences[sequence] != *before.sequences[sequence])
        {
            result.replaced.push_back(sequence);
        }
    }
    for (auto sequence = static_cast<uint32_t>(before.sequences.size()); sequence < result.file.sequences.size();
         ++sequence)
    {
        result.replaced.push_back(sequence);
    }
    if (!result.file.timelines || !before.timelines)
    {
        return result;
    }
    for (const uint32_t sequence : result.replaced)
    {
        result.file.timelines->erase(sequence);
    }

    for (auto& [sequence, hints] : *result.file.timelines)
    {
        const auto old = before.timelines->find(sequence);
        if (old == before.timelines->end() || old->second != hints || sequence >= before.sequences.size() ||
            sequence >= result.file.sequences.size() ||
            result.file.sequences[sequence].SharesWith(before.sequences[sequence]))
        {
            continue;
        }

        ConverterHints settled_hints = hints;
        Sequence settled;
        try
        {
            settled = SettledLayout(*before.sequences[sequence], *result.file.sequences[sequence], settled_hints);
        }
        catch (const std::invalid_argument& error)
        {
            throw std::invalid_argument(
                "A timeline can't hold this edit: " + std::string(error.what()) +
                ". The Program button of the inspector's Sequence section lets the sequence take any edit.");
        }
        std::vector<std::optional<uint32_t>> items = ItemsAfterJoins(*result.file.sequences[sequence], settled);
        MovePlaces(result.file, sequence, items);
        result.file.sequences[sequence] = std::move(settled);
        hints = std::move(settled_hints);
        result.items[sequence] = std::move(items);
    }

    return result;
}

format::File AsProgram(const format::File& file, uint32_t sequence)
{
    if (!IsTimeline(file, sequence))
    {
        return file;
    }

    format::File program = file;
    Fold(program, sequence, file.timelines->at(sequence));
    program.timelines->erase(sequence);

    return program;
}

format::File AsTimeline(const format::File& file, uint32_t sequence)
{
    if (IsTimeline(file, sequence))
    {
        return file;
    }
    if (sequence >= file.sequences.size())
    {
        throw std::invalid_argument("The file has no such sequence.");
    }

    const ConvertedSong converted = ReadConverted(*file.sequences[sequence]);
    if (!converted.hints)
    {
        throw std::invalid_argument(
            "Hokuto edits this sequence as a program only. NintendoWare's MIDI converter didn't "
            "write it, as far as Hokuto can tell: " +
            converted.problem + ".");
    }

    format::File timeline = file;
    Unfold(timeline, sequence, *converted.hints);
    if (!timeline.timelines)
    {
        timeline.timelines.emplace();
    }
    (*timeline.timelines)[sequence] = *converted.hints;

    return timeline;
}

format::File WithNewModes(format::File file, std::span<const uint32_t> sequences)
{
    if (!file.timelines)
    {
        file.timelines.emplace();
    }
    for (const uint32_t sequence : sequences)
    {
        file.timelines->erase(sequence);
        if (sequence < file.sequences.size())
        {
            TryTimeline(file, sequence);
        }
    }

    return file;
}

std::optional<ConvertedLoop> TimelineLoop(const format::File& file, uint32_t sequence)
{
    if (!IsTimeline(file, sequence))
    {
        return std::nullopt;
    }

    // The song's tracks start after the header's waits.
    try
    {
        const std::optional<std::vector<ConvertedLoop>> loops = SongLoopsOf(*file.sequences[sequence]);
        if (!loops || loops->empty() ||
            std::ranges::any_of(*loops, [&](const ConvertedLoop& loop) { return loop != loops->front(); }))
        {
            return std::nullopt;
        }

        return loops->front();
    }
    catch (const std::invalid_argument&)
    {
        return std::nullopt;
    }
}

std::optional<ConvertedLoop> TimelineSongLoop(const format::File& file, uint32_t sequence)
{
    if (!IsTimeline(file, sequence))
    {
        return std::nullopt;
    }

    try
    {
        const std::optional<std::vector<ConvertedLoop>> loops = SongLoopsOf(*file.sequences[sequence]);
        if (!loops || loops->empty())
        {
            return std::nullopt;
        }

        const uint32_t pass = loops->front().end - loops->front().start;
        const auto other_pass = [pass](const ConvertedLoop& loop)
        {
            return loop.end - loop.start != pass;
        };
        if (pass == 0 || std::ranges::any_of(*loops, other_pass))
        {
            return std::nullopt;
        }

        const uint32_t start = std::ranges::max(*loops, {}, &ConvertedLoop::start).start;
        return ConvertedLoop{start, start + pass};
    }
    catch (const std::invalid_argument&)
    {
        return std::nullopt;
    }
}

SongEnd TimelineSongEnd(const format::File& file, uint32_t sequence)
{
    if (!IsTimeline(file, sequence))
    {
        return {};
    }

    // A song that the converter's rules can't read counts as a song that loops at an unknown tick.
    try
    {
        const std::optional<std::vector<ConvertedLoop>> loops = SongLoopsOf(*file.sequences[sequence]);
        if (!loops)
        {
            return {true, std::nullopt};
        }
        if (loops->empty())
        {
            return {};
        }

        return {true, std::ranges::max(*loops, {}, &ConvertedLoop::end).end};
    }
    catch (const std::invalid_argument&)
    {
        return {true, std::nullopt};
    }
}

song::Sequence TimelineRead(const format::File& file, uint32_t sequence, song::Sequence read)
{
    const std::optional<ConvertedLoop> loop = TimelineSongLoop(file, sequence);
    if (!loop || !read.loop || !read.changes.empty() || loop->end <= loop->start)
    {
        return read;
    }

    const uint32_t pass = loop->end - loop->start;
    const song::Loop first{loop->start, loop->end};
    const bool later = read.loop->end - read.loop->start == pass && read.loop->start > loop->start &&
                       (read.loop->start - loop->start) % pass == 0;
    return later ? song::FirstPass(std::move(read), first) : read;
}

Edited MoveLoop(const format::File& file, uint32_t sequence, ConvertedLoop loop)
{
    if (!IsTimeline(file, sequence))
    {
        throw std::invalid_argument(
            "Only a timeline's loop moves. The List Editor points a program's jumps at its "
            "labels.");
    }

    // Each track counts its ticks from where the header starts it, after the header's waits.
    const Sequence& old = *file.sequences[sequence];
    const std::optional<std::map<std::string, uint32_t>> starts = TrackStartsFrom(old, 0);
    if (!starts)
    {
        throw std::invalid_argument("The song's header doesn't say when its tracks start.");
    }

    // The sounds' starts stay at their labels, and each note keeps its place among the notes.
    Sequence moved = WithLoop(old, file.timelines->at(sequence), loop, *starts);
    Edited result{file, std::vector<std::optional<uint32_t>>(old.items.size()), {}};
    MovePlaces(result.file, sequence, MatchingItems(old, moved));
    std::vector<uint32_t> notes;
    for (uint32_t item = 0; item < moved.items.size(); ++item)
    {
        if (moved.items[item].kind == Kind::kNote)
        {
            notes.push_back(item);
        }
    }
    std::size_t note = 0;
    for (uint32_t item = 0; item < old.items.size() && note < notes.size(); ++item)
    {
        if (old.items[item].kind == Kind::kNote)
        {
            result.items[item] = notes[note++];
        }
    }
    result.file.sequences[sequence] = std::move(moved);

    return result;
}

} // namespace hokuto::edit
