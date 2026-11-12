// SPDX-License-Identifier: MIT

// The helpers that the rounds of hokuto_check_commands share (check_commands.h).

#include "check_commands.h"
#include "loop_passes.h"

namespace hokuto::tools::commands
{

namespace
{

// The longest that ntdlib's performer plays a sound, in seconds (ntdlib's sequence_export/lengths.h).
constexpr double kMostSeconds = 900.0;

} // namespace

bool TableEntry(const File& file, const Command& item)
{
    const hokuto::song::DriverRules* driver = hokuto::edit::ConsoleOf(file).driver;
    return driver != nullptr && hokuto::edit::EditsOf(*driver).IsTableEntry(item);
}

bool PastEnd(const hokuto::song::Sequence& song, uint32_t tick)
{
    return !song.loop && tick > song.length;
}

bool UbiMilanConsole(const hokuto::song::Console& console)
{
    return console.driver != nullptr && console.driver->DriverEngine() == hokuto::format::Engine::kUbiMilan;
}

bool ReadsData(const File& file, uint32_t sequence, const hokuto::song::Sequence& sound)
{
    const ntdlib::commands::Sequence& commands = *file.sequences[sequence];
    const std::vector<uint32_t> offsets = hokuto::edit::Offsets(commands, hokuto::edit::ConsoleOf(file));
    for (const hokuto::song::Track& track : sound.tracks)
    {
        for (const hokuto::song::Note& note : track.notes)
        {
            const std::optional<uint32_t> item = hokuto::edit::ItemAtOffset(offsets, note.command);
            if (!item || commands.items[*item].kind != Kind::kNote)
            {
                return true;
            }
        }
    }

    return false;
}

std::vector<Played> NotesOf(const hokuto::song::Sequence& sequence, const hokuto::tools::NotesApart& apart)
{
    std::vector<Played> notes;
    for (const hokuto::song::Track& track : sequence.tracks)
    {
        for (const hokuto::song::Note& note : track.notes)
        {
            if (!apart || !apart(track, note))
            {
                notes.emplace_back(track.number, note.start, note.key, note.length, note.velocity);
            }
        }
    }
    std::sort(notes.begin(), notes.end());

    return notes;
}

namespace
{

// Whether ntdlib's performer stopped `song` at its time limit, without a loop: the tick after the song's length starts
// at the limit or past it.
bool ReachesLimit(const hokuto::song::Sequence& song)
{
    if (song.loop)
    {
        return false;
    }

    double seconds = 0.0;
    uint32_t tick = 0;
    double bpm = 120.0;
    for (const hokuto::song::Tempo& tempo : song.tempo)
    {
        if (tempo.tick > song.length)
        {
            break;
        }

        seconds += (tempo.tick - tick) * 60.0 / (bpm * song.timebase);
        tick = tempo.tick;
        bpm = tempo.bpm;
    }

    seconds += (uint64_t{song.length} + 1 - tick) * 60.0 / (bpm * song.timebase);
    return seconds >= kMostSeconds;
}

} // namespace

bool PartPass(const hokuto::song::Sequence& before, const hokuto::song::Sequence& after)
{
    if ((!rd2_song && !brownie_song) || before.loop == after.loop)
    {
        return false;
    }

    const std::optional<hokuto::tools::LoopShift> shift = hokuto::tools::ShiftOf(before, after);
    return shift && shift->apart % shift->length != 0;
}

bool SamePlaying(const hokuto::song::Sequence& before, const hokuto::song::Sequence& after, bool& moved,
                 uint32_t shortest, const hokuto::tools::NotesApart& apart)
{
    moved = after.loop != before.loop;

    // A song of Konami's driver can lose a tick or more before the loop point of its first pass, and the second pass is
    // then shorter than the first. The performer marks either one.
    const bool next_pass = konami_song && before.loop && after.loop &&
                           (after.loop->start == before.loop->end || before.loop->start == after.loop->end);
    if (moved && before.loop && after.loop && !next_pass)
    {
        const std::optional<hokuto::tools::LoopShift> shift = hokuto::tools::ShiftOf(before, after);
        if (!shift || (shift->apart % shift->length != 0 && !PartPass(before, after)))
        {
            return false;
        }
    }
    else if (moved && !next_pass && !ReachesLimit(before.loop ? after : before))
    {
        return false;
    }

    uint32_t end =
        before.loop && after.loop ? std::min(before.loop->end, after.loop->end) : std::numeric_limits<uint32_t>::max();
    for (const hokuto::song::Sequence* song : {&before, &after})
    {
        if (ReachesLimit(*song))
        {
            end = std::min(end, song->length);
        }
    }
    const auto starts = [end, shortest, &apart](const hokuto::song::Sequence& song)
    {
        std::vector<std::tuple<int, uint32_t, uint8_t, uint8_t>> notes;
        for (const auto& [track, start, key, length, velocity] : NotesOf(song, apart))
        {
            if (start < end && length >= shortest)
            {
                notes.emplace_back(track, start, key, velocity);
            }
        }

        return notes;
    };

    return starts(after) == starts(before);
}

bool VoicesDecide(const File& file, uint32_t sequence, const hokuto::song::Sequence& before,
                  const hokuto::song::Sequence& after, bool& moved, std::size_t& alone)
{
    using Heard = std::tuple<int, uint32_t, int, int>;
    const auto heard = [](const hokuto::song::Sequence& song)
    {
        std::multiset<Heard> notes;
        for (const hokuto::song::Track& track : song.tracks)
        {
            for (const hokuto::song::Note& note : track.notes)
            {
                notes.emplace(track.number, note.start, note.key, note.velocity);
            }
        }
        return notes;
    };
    const std::multiset<Heard> in_before = heard(before);
    const std::multiset<Heard> in_after = heard(after);
    const std::set<std::pair<int, uint32_t>> ticks = hokuto::tools::NoteCommandTicks(file, sequence, before);
    std::size_t count = 0;
    const hokuto::tools::NotesApart voices = [&](const hokuto::song::Track& track, const hokuto::song::Note& note)
    {
        const Heard key{track.number, note.start, note.key, note.velocity};
        const bool apart = in_before.count(key) != in_after.count(key) && ticks.contains({track.number, note.start});
        count += apart ? 1 : 0;
        return apart;
    };
    const bool same = SamePlaying(before, after, moved, 0, voices);
    alone += same ? count : 0;
    return same;
}

bool SameNoteCommands(const hokuto::song::Sequence& before, const hokuto::song::Sequence& after, bool& moved,
                      bool psg_apart)
{
    const auto commands = [](hokuto::song::Sequence song)
    {
        for (hokuto::song::Track& track : song.tracks)
        {
            track.notes.insert(track.notes.end(), track.unstarted.begin(), track.unstarted.end());
            track.unstarted.clear();
        }

        return song;
    };

    const hokuto::tools::NotesApart psg = [](const hokuto::song::Track&, const hokuto::song::Note& note)
    {
        return note.psg != 0;
    };
    return SamePlaying(commands(before), commands(after), moved, 0, psg_apart ? psg : hokuto::tools::NotesApart());
}

std::vector<hokuto::edit::CommandRow> RowsOf(const File& file, uint32_t sequence, const hokuto::song::Sequence& song,
                                             int number)
{
    const auto numbered = [number](const hokuto::song::Track& track)
    {
        return track.number == number;
    };
    const auto track = std::find_if(song.tracks.begin(), song.tracks.end(), numbered);
    if (track == song.tracks.end())
    {
        return {};
    }

    const std::optional<uint32_t> end = song.loop ? std::optional<uint32_t>(song.loop->end) : std::nullopt;
    return hokuto::edit::CommandRows(file, sequence, track->stretches, track->notes, end);
}

bool WaitsForNotes(const File& file, uint32_t sequence, const hokuto::song::Sequence& song)
{
    for (const hokuto::song::Track& track : song.tracks)
    {
        std::optional<uint32_t> stretch;
        bool note_wait = false;
        for (const hokuto::edit::CommandRow& row : RowsOf(file, sequence, song, track.number))
        {
            if (row.stretch != stretch && row.stretch < track.stretches.size())
            {
                stretch = row.stretch;
                note_wait = track.stretches[row.stretch].note_wait;
            }
            const Command& command = hokuto::edit::CommandAt(file, sequence, row.command);
            if (command.kind == Kind::kNoteWait)
            {
                note_wait = command.value.source != Source::kValue || command.value.value != 0;
            }
            else if (command.kind == Kind::kNote && note_wait &&
                     (command.value.source != Source::kValue || command.value.value <= 0))
            {
                return true;
            }
        }
    }

    return false;
}

} // namespace hokuto::tools::commands
