// SPDX-License-Identifier: MIT

// The comparisons of hokuto_check_edits: which notes an edit may move, start again or leave out.

#include "check_edits.h"

namespace hokuto::tools::edits
{

std::vector<Played> NotesOf(const Sequence& sequence, uint32_t end, std::map<Played, uint32_t>& commands)
{
    std::vector<Played> notes;
    for (const hokuto::song::Track& track : sequence.tracks)
    {
        for (const Note& note : track.notes)
        {
            if (note.start < end)
            {
                const Played& played =
                    notes.emplace_back(track.number, note.start, note.key, note.length, note.velocity);
                commands.emplace(played, note.command);
            }
        }
    }
    std::sort(notes.begin(), notes.end());

    return notes;
}

bool JoinsRareNote(const std::vector<Played>& before, const std::vector<Played>& after, uint32_t frame,
                   const Played& note)
{
    const auto joins = [&note, frame](const Played& other)
    {
        const uint32_t start = std::get<1>(note);
        const uint32_t other_start = std::get<1>(other);
        return std::get<0>(other) != std::get<0>(note) && std::get<2>(other) == std::get<2>(note) &&
               other_start <= start && other_start + frame >= start && other_start + std::get<3>(other) > start;
    };
    return std::ranges::any_of(
        after, [&](const Played& other) { return joins(other) && !std::ranges::binary_search(before, other); });
}

bool RareFrameNote(const std::vector<Played>& after, const Played& note)
{
    const auto& [track, start, key, length, velocity] = note;
    const auto sounds = [&](const Played& other)
    {
        const auto& [other_track, other_start, other_key, other_length, other_velocity] = other;
        return other_track == track && other_key == key && other_start < start && other_start + other_length > start;
    };
    const auto framed = [&](const Played& other)
    {
        const auto& [other_track, other_start, other_key, other_length, other_velocity] = other;
        if (other_track != track || other_key != key)
        {
            return false;
        }
        if (other_start == start)
        {
            return other_velocity > velocity && other_length + 1 >= length && other_length <= length + 1;
        }
        if (other_velocity != velocity)
        {
            return false;
        }
        if (other_start + 1 == start)
        {
            return other_start + other_length == start + length;
        }
        return other_start == start + 1 && other_start + other_length <= start + length + 1 &&
               std::ranges::any_of(after, sounds);
    };
    return std::ranges::any_of(after, framed);
}

bool PastCommands(const ntdlib::commands::Sequence& sequence, const std::map<uint32_t, uint32_t>& by_offset,
                  uint32_t offset)
{
    const auto item = by_offset.find(offset);
    return item == by_offset.end() || sequence.items[item->second].kind != Kind::kNote;
}

void WithComparableKeys(Sequence& read, const ntdlib::commands::Sequence& commands,
                        const hokuto::song::Console& console)
{
    if (!hokuto::edit::KonamiOf(console))
    {
        return;
    }

    const std::map<uint32_t, uint32_t> items = ItemsByOffset(commands, console);
    for (hokuto::song::Track& track : read.tracks)
    {
        for (Note& note : track.notes)
        {
            if (*hokuto::edit::KonamiOf(console) == ntdlib::konami::Revision::kDungeonDiceMonsters &&
                !ntdlib::konami::IsPsgTrack(track.number))
            {
                note.key = static_cast<uint8_t>(note.program);
                continue;
            }

            const auto item = items.find(note.command);
            if (item != items.end() && commands.items[item->second].kind == Kind::kNote &&
                !commands.items[item->second].sample && commands.items[item->second].key == 255)
            {
                note.key = 0;
            }
        }
    }
}

bool StartedAgain(const Sequence& read, const Played& note)
{
    const auto track = std::ranges::find(read.tracks, std::get<0>(note), &hokuto::song::Track::number);
    if (track == read.tracks.end())
    {
        return false;
    }

    const auto found =
        std::ranges::find_if(track->notes, [&note](const Note& other)
                             { return other.start == std::get<1>(note) && other.key == std::get<2>(note); });
    if (found == track->notes.end())
    {
        return false;
    }

    // A PSG track off centre plays notes of the track before it on the channel. They don't count.
    for (auto earlier = found; earlier != track->notes.begin();)
    {
        --earlier;
        if (earlier->command_track == found->command_track)
        {
            return earlier->command == found->command && earlier->stretch == found->stretch;
        }
    }

    return false;
}

bool StartedElsewhere(const File& file, uint32_t sequence, const Sequence& read, const Played& note,
                      std::map<int, std::set<std::pair<uint32_t, uint32_t>>>& rows)
{
    const auto track = std::ranges::find(read.tracks, std::get<0>(note), &hokuto::song::Track::number);
    if (track == read.tracks.end())
    {
        return false;
    }

    const auto found =
        std::ranges::find_if(track->notes, [&note](const Note& other)
                             { return other.start == std::get<1>(note) && other.key == std::get<2>(note); });
    if (found == track->notes.end())
    {
        return false;
    }

    if (!rows.contains(track->number))
    {
        std::set<std::pair<uint32_t, uint32_t>>& ran = rows[track->number];
        for (const hokuto::edit::CommandRow& row :
             hokuto::edit::CommandRows(file, sequence, track->stretches, track->notes, std::nullopt))
        {
            if (row.tick)
            {
                ran.emplace(row.command, *row.tick);
            }
        }
    }

    return !rows.at(track->number).contains({found->command, found->start});
}

bool JoinsPsgNote(const Sequence& read, const Played& note, const hokuto::song::Console& console)
{
    if (!hokuto::edit::KonamiOf(console) || !ntdlib::konami::EternalDuelistLayout(*hokuto::edit::KonamiOf(console)) ||
        !ntdlib::konami::IsPsgTrack(std::get<0>(note)))
    {
        return false;
    }

    const auto track = std::ranges::find(read.tracks, std::get<0>(note), &hokuto::song::Track::number);
    return track != read.tracks.end() &&
           std::ranges::any_of(
               track->notes, [&note](const Note& other)
               { return other.start <= std::get<1>(note) && std::get<1>(note) < other.start + other.length; });
}

bool OffCentrePsgNote(const Sequence& read, const Played& note, const hokuto::song::Console& console)
{
    if (!hokuto::edit::KonamiOf(console) || !ntdlib::konami::RecordHasPan(*hokuto::edit::KonamiOf(console)) ||
        !ntdlib::konami::IsPsgTrack(std::get<0>(note)))
    {
        return false;
    }

    const auto same = [&note](const Note& other)
    {
        return other.start == std::get<1>(note) && other.key == std::get<2>(note);
    };
    const auto track = std::ranges::find(read.tracks, std::get<0>(note), &hokuto::song::Track::number);
    if (track != read.tracks.end() && std::ranges::any_of(track->notes, [&same](const Note& other)
                                                          { return same(other) && other.command_track >= 0; }))
    {
        return true;
    }

    const auto previous = std::ranges::find(read.tracks, std::get<0>(note) - 1, &hokuto::song::Track::number);
    return previous != read.tracks.end() && std::ranges::any_of(previous->notes, same);
}

bool Unstarted(const Sequence& read, const Played& note)
{
    const auto& [number, start, key, length, velocity] = note;
    for (const hokuto::song::Track& track : read.tracks)
    {
        if (track.number != number)
        {
            continue;
        }

        for (const Note& unstarted : track.unstarted)
        {
            const uint32_t apart = unstarted.start > start ? unstarted.start - start : start - unstarted.start;
            if (unstarted.key == key && apart <= 1)
            {
                return true;
            }
        }
    }

    return false;
}

bool TempoUndecided(const File& file, uint32_t sequence, const Sequence& song)
{
    const ntdlib::commands::Sequence& commands = *file.sequences[sequence];
    const std::vector<uint32_t> offsets = hokuto::edit::Offsets(commands, hokuto::edit::ConsoleOf(file));
    const std::optional<uint32_t> end = song.loop ? std::optional<uint32_t>(song.loop->end) : std::nullopt;
    for (const hokuto::song::Track& track : song.tracks)
    {
        for (const hokuto::edit::CommandRow& row :
             hokuto::edit::CommandRows(commands, offsets, track.stretches, track.notes, end))
        {
            const std::optional<uint32_t> item = hokuto::edit::ItemAtOffset(offsets, row.command);
            if (item && commands.items[*item].kind == Kind::kTempo &&
                commands.items[*item].value.source != ntdlib::commands::Source::kValue)
            {
                return true;
            }
        }
    }

    return false;
}

namespace
{

// Applies `plan` to `played`, note `note` of `track`, and sets `what` to the plan's word for the note. Records a moved
// note in `moves` and `origins`, and a note that the plan lengthens to a tick in `ticks`. Returns false for a note that
// the plan removes.
bool ApplyPlan(const Plan& plan, const hokuto::song::Track& track, const Note& note, Played& played, const char*& what,
               std::set<Played>& moves, std::map<Played, Played>& origins, std::set<Played>& ticks)
{
    switch (plan.change)
    {
    case Change::kMove:
        std::get<1>(played) = static_cast<uint32_t>(static_cast<int64_t>(note.start) + plan.edit.shift);
        moves.insert(played);
        origins[played] = Played{track.number, note.start, note.key, note.length, note.velocity};
        what = "moved";
        break;

    case Change::kVelocity:
        std::get<4>(played) = plan.heard.value_or(static_cast<uint32_t>(*plan.edit.velocity));
        what = "with a new velocity";
        break;

    case Change::kRemove:
        return false;

    case Change::kLonger:
        std::get<3>(played) = note.length + plan.longer;
        what = "longer";
        if (plan.edit.length == 1u)
        {
            ticks.insert(played);
        }
        break;
    }

    return true;
}

} // namespace

std::vector<Played> Expected(const Sequence& before, const std::map<uint32_t, Plan>& plans,
                             const std::set<uint32_t>& replays, uint32_t end, std::map<Played, std::string>& edits,
                             std::set<Played>& moves, std::map<Played, Played>& origins, std::set<Played>& ticks)
{
    std::vector<Played> notes;
    for (const hokuto::song::Track& track : before.tracks)
    {
        std::optional<Change> last;
        for (const Note& note : track.notes)
        {
            Played played{track.number, note.start, note.key, note.length, note.velocity};
            const auto plan = plans.find(note.command);
            const char* what = "unedited";
            if (replays.contains(note.command) && last == Change::kRemove)
            {
                continue;
            }
            if (plan != plans.end())
            {
                last = plan->second.change;
                if (!ApplyPlan(plan->second, track, note, played, what, moves, origins, ticks))
                {
                    continue;
                }
            }

            if (std::get<1>(played) < end)
            {
                edits[played] = std::string(what) + ", from tick " + std::to_string(note.start) + ", command at " +
                                std::to_string(note.command);
                notes.push_back(played);
            }
        }
    }
    std::sort(notes.begin(), notes.end());

    return notes;
}

Played WithoutLength(Played note)
{
    std::get<3>(note) = 0;

    return note;
}

Played WithoutVelocity(Played note)
{
    std::get<4>(note) = 0;

    return note;
}

std::vector<Played> Unmatched(const std::vector<Played>& notes, const std::vector<Played>& others,
                              Played (*strip)(Played))
{
    std::vector<Played> unmatched;
    for (const Played& note : notes)
    {
        const auto same = [&](const Played& other)
        {
            return strip(other) == strip(note);
        };
        if (std::none_of(others.begin(), others.end(), same))
        {
            unmatched.push_back(note);
        }
    }

    return unmatched;
}

std::vector<Played> Without(const std::vector<Played>& a, const std::vector<Played>& b)
{
    std::vector<Played> result;
    std::set_difference(a.begin(), a.end(), b.begin(), b.end(), std::back_inserter(result));

    return result;
}

std::string Shape(const Sequence& sequence)
{
    const std::string loop =
        sequence.loop ? "loop " + std::to_string(sequence.loop->start) + " to " + std::to_string(sequence.loop->end)
                      : "no loop";
    return loop + ", " + std::to_string(sequence.length) + " ticks";
}

uint32_t FirstPassEnd(const Sequence& sequence)
{
    return sequence.loop ? std::min(sequence.loop->end, sequence.length) : sequence.length;
}

std::set<int> LegatoTracks(const ntdlib::commands::Sequence& sequence, const hokuto::song::Console& console,
                           const Sequence& song)
{
    const std::vector<uint32_t> offsets = hokuto::edit::Offsets(sequence, console);
    std::set<int> tracks;
    for (const hokuto::song::Track& track : song.tracks)
    {
        for (const hokuto::song::Stretch& stretch : track.stretches)
        {
            const std::optional<uint32_t> first = hokuto::edit::ItemAtOffset(offsets, stretch.first);
            const std::optional<uint32_t> last = hokuto::edit::ItemAtOffset(offsets, stretch.last);
            if (!first || !last)
            {
                continue;
            }

            for (uint32_t item = *first; item <= *last; ++item)
            {
                const Command& command = sequence.items[item];
                const bool on = command.value.source != ntdlib::commands::Source::kValue || command.value.value != 0;
                if ((command.kind == Kind::kTie || command.kind == Kind::kMonophonic) && on)
                {
                    tracks.insert(track.number);
                }
            }
        }
    }

    return tracks;
}

std::set<int> UnknownWaitTracks(const ntdlib::commands::Sequence& sequence, const hokuto::song::Console& console,
                                const Sequence& song)
{
    const std::vector<uint32_t> offsets = hokuto::edit::Offsets(sequence, console);
    std::set<int> tracks;
    for (const hokuto::song::Track& track : song.tracks)
    {
        for (const hokuto::song::Stretch& stretch : track.stretches)
        {
            const std::optional<uint32_t> first = hokuto::edit::ItemAtOffset(offsets, stretch.first);
            const std::optional<uint32_t> last = hokuto::edit::ItemAtOffset(offsets, stretch.last);
            if (!first || !last)
            {
                continue;
            }

            bool note_wait = stretch.note_wait;
            for (uint32_t item = *first; item <= *last; ++item)
            {
                const Command& command = sequence.items[item];
                const bool known = command.value.source == ntdlib::commands::Source::kValue && !command.conditional;
                if (command.kind == Kind::kNoteWait)
                {
                    note_wait = !known || command.value.value != 0;
                }
                if (command.kind == Kind::kNote && note_wait &&
                    (!known || command.value.value <= 0 || command.value.value % kOne != 0))
                {
                    tracks.insert(track.number);
                }
            }
        }
    }

    return tracks;
}

} // namespace hokuto::tools::edits
