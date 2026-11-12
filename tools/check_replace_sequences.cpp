// SPDX-License-Identifier: MIT

// The checks of sequence files of hokuto_check_replace.

#include "check_replace.h"

namespace hokuto::tools::replace
{

namespace
{

// Whether `other` contains the commands of `same` at each item that the start of `same` reaches, through the track
// starts too: the same commands at the same items, but for the types and values of biquad filters' settings. The items
// that no command reaches can differ.
bool SameReached(const Source& same, const Source& other)
{
    const std::vector<ntdlib::commands::Command>& items = same.incoming.sequence.items;
    const std::vector<ntdlib::commands::Command>& other_items = other.incoming.sequence.items;
    if (other.start != same.start || other_items.size() != items.size())
    {
        return false;
    }

    const std::vector<bool> reached = edit::Reached(same.incoming.sequence, *same.start, true);
    for (std::size_t i = 0; i < reached.size() && i < items.size(); i++)
    {
        if (reached[i] && !song::SameButBiquads(items[i], other_items[i]))
        {
            return false;
        }
    }

    return true;
}

} // namespace

std::string CrossProblem(const format::File& file, std::span<const std::optional<edit::SoundPlace>> places,
                         const sar::SoundArchive& built, std::span<const Group> groups, uint32_t sound, uint32_t other,
                         const Source& source, std::map<uint32_t, song::Sequence>& before,
                         std::optional<format::File>& replaced, std::size_t& moved)
{
    // The replacement, with the other sound's bank slots for each sound that plays the new commands.
    const uint32_t tracks = edit::TracksToAdd(file, places, sound, source.incoming.sequence, *source.start);
    replaced = edit::ReplaceSequence(file, places, sound, source.incoming.sequence, *source.start, {}, "Check", tracks,
                                     built.Sounds()[other].banks);
    std::vector<uint32_t> playing = edit::SharingStart(places, sound);
    playing.push_back(sound);
    hokuto::tools::CheckSaved(*replaced, true);
    const Archive after =
        song::LoadArchive(format::ExportArchive(*replaced), "after.bfsar", format::ExportGroupFiles(*replaced));

    // The sounds that play the new commands play the other sound's notes. The file's other sounds play as before.
    const auto played_before = [&](uint32_t which) -> const song::Sequence&
    {
        auto found = before.find(which);
        if (found == before.end())
        {
            found = before.emplace(which, Played(built, which)).first;
        }

        return found->second;
    };
    for (const uint32_t player : playing)
    {
        const song::Sequence played = Played(*after, player);
        if (played != played_before(other))
        {
            if (!SameUpToLoop(played_before(other), played))
            {
                return "doesn't play " + built.Sounds()[other].name + "'s notes from its file";
            }
            moved++;
        }
    }
    for (const uint32_t sharing : edit::SharingFile(places, sound))
    {
        if (std::ranges::find(playing, sharing) == playing.end() && Played(*after, sharing) != played_before(sharing))
        {
            return "changes how " + built.Sounds()[sharing].name + " plays";
        }
    }

    // The other files keep their bytes, and the other sounds their starts. A group's file can hold a copy of the
    // sound's file.
    const uint32_t file_id = built.Sounds()[sound].file_id;
    std::vector<uint32_t> copies = {file_id};
    for (const Group& group : groups)
    {
        if (std::ranges::find(group.loads, file_id) != group.loads.end())
        {
            copies.push_back(group.file_id);
        }
    }
    for (uint32_t id = 0; id < built.FileCount(); id++)
    {
        if (std::ranges::find(copies, id) == copies.end() &&
            !std::ranges::equal(built.FileData(id), after->FileData(id)))
        {
            return "changes file " + std::to_string(id);
        }
    }
    for (std::size_t i = 0; i < built.Sounds().size(); i++)
    {
        const bool plays = std::ranges::find(playing, static_cast<uint32_t>(i)) != playing.end();
        if (!plays && after->Sounds()[i].start_offset != built.Sounds()[i].start_offset)
        {
            return "moves the start of " + built.Sounds()[i].name;
        }
    }

    // Each copy in a group's file inside the archive, and in the group files beside it, holds the new bytes.
    std::size_t placed = 0;
    for (const Group& group : groups)
    {
        if (std::ranges::find(group.loads, file_id) == group.loads.end() || built.FileData(group.file_id).empty())
        {
            continue;
        }

        const std::string problem = GroupFileProblem(built.FileData(group.file_id), after->FileData(group.file_id),
                                                     built.ReadOptions().flavor, file_id, built.FileData(file_id),
                                                     after->FileData(file_id), "group " + HexOf(group.file_id), placed);
        if (!problem.empty())
        {
            return problem;
        }
    }

    return GroupFilesProblem(built, *replaced, file_id, after->FileData(file_id), placed);
}

std::string SameProblem(const format::File& file, std::span<const std::optional<edit::SoundPlace>> places,
                        const sar::SoundArchive& built, uint32_t sound, const Source& same, Totals& totals)
{
    const sar::SoundInfo& info = built.Sounds()[sound];
    if (!same.start)
    {
        return "starts where no command of its file starts";
    }

    const auto replaced = [&](const Source& source)
    {
        const uint32_t tracks = edit::TracksToAdd(file, places, sound, source.incoming.sequence, *source.start);
        return edit::ReplaceSequence(file, places, sound, source.incoming.sequence, *source.start, {}, "Check", tracks);
    };

    const auto offered = [&](const Source& source)
    {
        const std::vector<edit::StartingSound> starts = edit::SoundStarts(file, places, source.incoming.sequence);
        return std::ranges::find(starts, edit::StartingSound{sound, *source.start}) != starts.end();
    };
    if (!offered(same))
    {
        return "isn't offered its start in its file";
    }
    if (replaced(same) != file)
    {
        return "doesn't come back from its file";
    }

    const sar::Flavor flavor = built.ReadOptions().flavor;
    const std::span<const uint8_t> bytes = built.FileData(info.file_id);
    std::vector<uint32_t> starts = {info.start_offset};
    for (const uint32_t sharing : song::SoundsSharingFile(built, sound))
    {
        starts.push_back(built.Sounds()[sharing].start_offset);
    }
    for (const sar::Flavor console : song::OtherConsoles(flavor))
    {
        std::vector<uint8_t> other_file;
        try
        {
            other_file = song::OtherConsoleSequenceFile(built, sound, console).file;
        }
        catch (const std::runtime_error&)
        {
            if (console != sar::Flavor::kCtr)
            {
                throw;
            }
            totals.ctr_refused++;
            continue;
        }

        const std::string name = song::ConsoleName(console);
        const Source other =
            SourceOf(other_file, info.start_offset, song::Console{.engine = hokuto::format::EngineOf(flavor)});
        if (!other.start)
        {
            return "starts where no command of its file for the " + name + " starts";
        }
        if (replaced(other) == file)
        {
            if (!offered(other))
            {
                return "isn't offered its start in its file for the " + name;
            }
            continue;
        }

        // The Switch's biquad filters of types 1 to 5 take other values for another console, and come back as types 6
        // to 10 (song::MapBiquads).
        if (other.incoming.sequence != same.incoming.sequence &&
            song::SameButBiquads(other.incoming.sequence, same.incoming.sequence))
        {
            if (!offered(other))
            {
                return "isn't offered its start in its file for the " + name;
            }

            totals.other_biquads++;
            continue;
        }

        // Bytes that no command reaches can come back otherwise from another console. Bytes 8 to 11 hold the version.
        std::vector<uint8_t> back = song::ConvertSequenceFile(other_file, console, flavor, starts).file;
        std::copy(bytes.begin() + 8, bytes.begin() + 12, back.begin() + 8);
        if (std::ranges::equal(back, bytes))
        {
            return "doesn't come back from its file for the " + name;
        }
        if (!SameReached(same, other))
        {
            return "plays other commands from its file for the " + name;
        }
        totals.other_unreached++;
    }

    return {};
}

} // namespace hokuto::tools::replace
