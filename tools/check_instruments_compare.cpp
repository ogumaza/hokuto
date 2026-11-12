// SPDX-License-Identifier: MIT

// hokuto_check_instruments: the comparisons of the notes before and after an edit, and which channels decide them on
// each console.

#include "check_instruments.h"

namespace hokuto::tools::instruments
{

std::vector<PlayedNote> NotesOf(const song::Track& track)
{
    std::vector<PlayedNote> notes;
    for (const song::Note& note : track.notes)
    {
        notes.emplace_back(note.start, note.key, note.velocity, note.bank_slot, static_cast<int>(note.program));
    }

    return notes;
}

std::size_t LengthsChanged(const song::Track& before, const song::Track& after)
{
    std::size_t changed = 0;
    for (std::size_t i = 0; i < before.notes.size() && i < after.notes.size(); i++)
    {
        if (before.notes[i].length != after.notes[i].length)
        {
            changed++;
        }
    }

    return changed;
}

bool WithoutNotesOf(const std::vector<PlayedNote>& fewer, const std::vector<PlayedNote>& more, const edit::Program& to)
{
    if (fewer.size() >= more.size())
    {
        return false;
    }

    std::size_t next = 0;
    for (const PlayedNote& note : more)
    {
        if (next < fewer.size() && note == fewer[next])
        {
            next++;
        }
        else if (std::get<3>(note) != to.slot || std::get<4>(note) != to.number)
        {
            return false;
        }
    }

    return next == fewer.size();
}

std::set<int> GbaChannelsOf(const Archive& archive, uint32_t song, const song::Track& track, const edit::Program& from,
                            const edit::Program& to, uint32_t& voices)
{
    std::set<int> channels;
    ntdlib::mp2k::SongHeader header;
    if (!archive.archive.gba ||
        !ntdlib::mp2k::ReadSong(archive.archive.gba->rom, archive.archive.gba->info, static_cast<int>(song), header))
    {
        return channels;
    }

    voices = header.voices;
    for (const song::Note& note : track.notes)
    {
        if (note.bank_slot == from.slot && static_cast<int>(note.program) == from.number)
        {
            for (const int program : {from.number, to.number})
            {
                channels.insert(hokuto::tools::GbaPsgChannel(archive.archive.gba->rom, voices,
                                                             static_cast<uint32_t>(program), note.key));
            }
        }
    }

    return channels;
}

namespace
{

// The notes that an edit lost and gained: those of `comparison`'s expected notes that it didn't play, and those that
// it played and didn't expect.
struct LostAndGained
{
    std::vector<PlayedNote> lost;
    std::vector<PlayedNote> gained;
};

LostAndGained LostAndGainedOf(const Comparison& comparison)
{
    std::vector<PlayedNote> expected = comparison.expected;
    std::vector<PlayedNote> played = comparison.played;
    std::sort(expected.begin(), expected.end());
    std::sort(played.begin(), played.end());
    std::vector<PlayedNote> lost;
    std::vector<PlayedNote> gained;
    std::set_difference(expected.begin(), expected.end(), played.begin(), played.end(), std::back_inserter(lost));
    std::set_difference(played.begin(), played.end(), expected.begin(), expected.end(), std::back_inserter(gained));

    return {std::move(lost), std::move(gained)};
}

} // namespace

bool ChannelsDecide(const Archive& archive, uint32_t voices, const std::set<int>& channels,
                    const Comparison& comparison)
{
    if (channels.empty())
    {
        return false;
    }

    const auto [lost, gained] = LostAndGainedOf(comparison);
    std::set<std::pair<uint32_t, uint8_t>> lost_places;
    for (const PlayedNote& note : lost)
    {
        lost_places.emplace(std::get<0>(note), std::get<1>(note));
    }

    const auto on_channels = [&](const PlayedNote& note)
    {
        const int channel = hokuto::tools::GbaPsgChannel(archive.archive.gba->rom, voices,
                                                         static_cast<uint32_t>(std::get<4>(note)), std::get<1>(note));
        return channels.contains(channel);
    };
    return std::all_of(lost.begin(), lost.end(), on_channels) &&
           std::all_of(gained.begin(), gained.end(), on_channels) &&
           std::none_of(gained.begin(), gained.end(), [&](const PlayedNote& note)
                        { return lost_places.contains({std::get<0>(note), std::get<1>(note)}); });
}

bool DsChannelsDecide(const Comparison& comparison)
{
    if (comparison.after == nullptr)
    {
        return false;
    }

    const auto [lost, gained] = LostAndGainedOf(comparison);
    const auto unstarted = [](const song::Track& track)
    {
        return [&track](const PlayedNote& note)
        {
            return std::any_of(track.unstarted.begin(), track.unstarted.end(), [&note](const song::Note& n)
                               { return n.start == std::get<0>(note) && n.key == std::get<1>(note); });
        };
    };
    return std::all_of(lost.begin(), lost.end(), unstarted(*comparison.after)) &&
           std::all_of(gained.begin(), gained.end(), unstarted(*comparison.before));
}

bool DifferOnlyInNotesOf(std::vector<PlayedNote> expected, std::vector<PlayedNote> played, const edit::Program& to)
{
    std::sort(expected.begin(), expected.end());
    std::sort(played.begin(), played.end());
    std::vector<PlayedNote> different;
    std::set_symmetric_difference(expected.begin(), expected.end(), played.begin(), played.end(),
                                  std::back_inserter(different));
    return std::all_of(different.begin(), different.end(), [&to](const PlayedNote& note)
                       { return std::get<3>(note) == to.slot && std::get<4>(note) == to.number; });
}

std::optional<uint32_t> MovedEnd(const Comparison& comparison)
{
    std::optional<uint32_t> moved;
    for (std::size_t i = 0; comparison.after != nullptr && i < comparison.same; i++)
    {
        const song::Note& before = comparison.before->notes[i];
        const uint32_t length = comparison.after->notes[i].length;
        if (before.length != length)
        {
            const uint32_t end = before.start + std::min(before.length, length);
            moved = moved ? std::min(*moved, end) : end;
        }
    }

    return moved;
}

uint32_t DifferenceTick(const Comparison& comparison)
{
    uint32_t tick = UINT32_MAX;
    if (comparison.same < comparison.expected.size())
    {
        tick = std::get<0>(comparison.expected[comparison.same]);
    }
    if (comparison.same < comparison.played.size())
    {
        tick = std::min(tick, std::get<0>(comparison.played[comparison.same]));
    }

    return tick;
}

std::string Difference(const std::vector<PlayedNote>& expected, const std::vector<PlayedNote>& played)
{
    const std::size_t first = FirstDifferingIndex(expected, played);

    const auto text = [](const std::vector<PlayedNote>& notes, std::size_t index)
    {
        if (index >= notes.size())
        {
            return std::string("none");
        }

        const auto& [start, key, velocity, slot, program] = notes[index];
        return "key " + std::to_string(key) + " at " + std::to_string(start) + ", velocity " +
               std::to_string(velocity) + ", slot " + std::to_string(slot) + ", program " + std::to_string(program);
    };

    return std::to_string(expected.size()) + " notes expected and " + std::to_string(played.size()) + " played, note " +
           std::to_string(first) + " " + text(expected, first) + " expected and " + text(played, first) + " played";
}

} // namespace hokuto::tools::instruments
