// SPDX-License-Identifier: MIT

// The check of each sound of hokuto_check_edits after the edits: the notes that the edited archive plays, compared with
// the notes before the edits and their plans, and the differences that count apart.

#include "check_edits.h"

namespace hokuto::tools::edits
{
namespace
{

// One sound's notes before and after the edits, as the comparison takes them. Expected() sets `expected`, `edits`,
// `moves`, `origins` and `ticks`. `commands` contains the offset of the command of each note after the edits, and
// `commands_before` before them. `items_after` contains the items of the edited sequence by offset on `console`.
struct Comparison
{
    hokuto::song::Console console;
    std::map<Played, std::string> edits;
    std::set<Played> moves;
    std::map<Played, Played> origins;
    std::set<Played> ticks;
    std::vector<Played> expected;
    std::map<Played, uint32_t> commands;
    std::vector<Played> actual;
    std::vector<Played> missing;
    std::map<Played, uint32_t> commands_before;
    std::vector<Played> notes_before;
    std::map<uint32_t, uint32_t> items_after;
    std::vector<Played> extra;
};

// Reads `sound`, a sound of sequence `sequence`, from the edited archive of `run`. Returns nothing, and counts the
// sound in `totals`, when the read fails.
std::optional<Sequence> ReadAfterEdits(const EditsRun& run, uint32_t sound, uint32_t sequence, Totals& totals)
{
    const std::string& sound_name = run.archive.NameOf(sound);
    std::optional<Sequence> read;
    try
    {
        read = run.after.Read(sound);
        WithComparableKeys(*read, *run.edited.sequences[sequence], hokuto::edit::ConsoleOf(run.edited));
        if (run.timelines)
        {
            // The saved archive plays the same notes, with the same loop and length.
            const Sequence from_saved = run.saved.Read(sound);
            std::map<Played, uint32_t> saved_commands;
            std::map<Played, uint32_t> commands;
            if (NotesOf(from_saved, UINT32_MAX, saved_commands) != NotesOf(*read, UINT32_MAX, commands) ||
                from_saved.loop != read->loop || from_saved.length != read->length)
            {
                std::printf("%s: %s plays otherwise from the saved archive\n", run.name.c_str(), sound_name.c_str());
                ++totals.saved;
            }
        }
    }
    catch (const std::exception& error)
    {
        std::printf("%s: %s can't be read after the edits: %s\n", run.name.c_str(), sound_name.c_str(), error.what());
        ++totals.unread;
        return std::nullopt;
    }

    return read;
}

// The notes of `sound`, a sound of sequence `sequence`, before the edits and in `read` after them, up to `end`.
Comparison CompareNotes(EditsRun& run, uint32_t sound, uint32_t sequence, const Sequence& read, uint32_t end)
{
    Comparison comparison;
    comparison.expected = Expected(*run.before[sound], run.plans[sequence], run.replays[sequence], end,
                                   comparison.edits, comparison.moves, comparison.origins, comparison.ticks);
    comparison.actual = NotesOf(read, end, comparison.commands);
    comparison.missing = Without(comparison.expected, comparison.actual);
    comparison.notes_before = NotesOf(*run.before[sound], end, comparison.commands_before);
    comparison.console = hokuto::edit::ConsoleOf(run.edited);
    comparison.items_after = ItemsByOffset(*run.edited.sequences[sequence], comparison.console);
    comparison.extra = Without(comparison.actual, comparison.expected);

    return comparison;
}

// On Brownie Brown's driver, an edit can change the frames in which a channel's envelope turns a note off and on again
// (open work, item 361). A note that starts within a tick of the end of a note of the same key on its track continues
// one run of a command. Returns whether `note`, a note of `song` on `console`, continues one.
bool ContinuesRun(const hokuto::song::Console& console, const hokuto::song::Sequence& song, const Played& note)
{
    if (!Brownie(console))
    {
        return false;
    }
    for (const hokuto::song::Track& track : song.tracks)
    {
        if (track.number != std::get<0>(note))
        {
            continue;
        }
        for (const Note& other : track.notes)
        {
            if (other.key == std::get<2>(note) && other.start < std::get<1>(note) &&
                std::get<1>(note) <= other.start + other.length + 1)
            {
                return true;
            }
        }
    }
    return false;
}

// On Konami's driver, a volume command starts a PSG note again. The note names the volume command where the last note
// command before it plays at volume 0, and else that note command (song::ReadSequence). An edit that removes a note
// command leaves the volume commands after it. They can then follow a note command at volume 0. Returns whether `note`,
// a note of sequence `sequence` after the edits of `run`, names a volume command that no note named before the edits.
bool NamesVolumeAfterEdits(EditsRun& run, uint32_t sequence, const Comparison& comparison, const Played& note)
{
    const auto item = comparison.items_after.find(comparison.commands.at(note));
    if (item == comparison.items_after.end() ||
        run.edited.sequences[sequence]->items[item->second].kind != ntdlib::commands::Kind::kVolume)
    {
        return false;
    }

    const std::map<uint32_t, uint32_t>& items = run.items[sequence];
    const std::vector<std::optional<uint32_t>>& moved = run.moved[sequence];
    const auto named = [&](const std::pair<const Played, uint32_t>& before)
    {
        const auto old = items.find(before.second);
        return old != items.end() && moved.at(old->second) == item->second;
    };

    return std::ranges::none_of(comparison.commands_before, named);
}

// The notes that `read`, sound `sound` of sequence `sequence` after the edits, plays and the plans don't expect, less
// those that count apart in `totals`.
std::vector<Played> UnexpectedNotes(EditsRun& run, uint32_t sound, uint32_t sequence, const Sequence& read,
                                    const Comparison& comparison, Totals& totals)
{
    // On Nintendo R&D2's driver, the arrangement shows no note that the driver's voices kept out. An edit can give
    // such a note a voice, or take one away, where its track runs a note command (tools::NoteCommandTicks).
    std::optional<std::set<std::pair<int, uint32_t>>> commanded_before;
    const auto voices_before = [&](const Played& note)
    {
        if (!run.archive.Rd2())
        {
            return false;
        }
        if (!commanded_before)
        {
            commanded_before = hokuto::tools::NoteCommandTicks(run.file, sequence, *run.before[sound]);
        }
        return commanded_before->contains({std::get<0>(note), std::get<1>(note)});
    };

    std::vector<Played> unexpected;
    for (const Played& note :
         Unmatched(Unmatched(comparison.extra, comparison.missing, WithoutLength), comparison.missing, WithoutVelocity))
    {
        // A move can take a note that played no instrument past a command that gives it one. No moved note that
        // played before stands at the same tick then.
        const auto moved_here = [&note, &comparison](const Played& other)
        {
            return comparison.moves.contains(other) && std::get<0>(other) == std::get<0>(note) &&
                   std::get<1>(other) == std::get<1>(note);
        };
        // A note of the same key at the same tick can hide a note of another command. That command then plays at
        // other ticks too. The hidden note shows once an edit takes the other note away.
        const auto hides = [&note](const hokuto::song::Track& track)
        {
            return track.number == std::get<0>(note) &&
                   std::ranges::any_of(track.notes, [&note](const Note& other)
                                       { return other.start == std::get<1>(note) && other.key == std::get<2>(note); });
        };
        if (run.hidden[sequence].contains(comparison.commands.at(note)) ||
            std::ranges::any_of(run.before[sound]->tracks, hides))
        {
            ++totals.hidden;
        }
        else if (Unstarted(*run.before[sound], note) || voices_before(note))
        {
            ++totals.channels;
        }
        else if (hokuto::edit::KonamiOf(comparison.console) &&
                 (StartedAgain(read, note) || !comparison.items_after.contains(comparison.commands.at(note)) ||
                  NamesVolumeAfterEdits(run, sequence, comparison, note)))
        {
            // A PSG channel also sounds for a later volume or pan when an edit takes the track's first note away.
            ++totals.started_again;
        }
        else if (JoinsPsgNote(*run.before[sound], note, comparison.console))
        {
            ++totals.joined;
        }
        else if (OffCentrePsgNote(*run.before[sound], note, comparison.console) ||
                 OffCentrePsgNote(read, note, comparison.console))
        {
            ++totals.off_centre;
        }
        else if (comparison.console.engine == hokuto::format::Engine::kDse &&
                 PastCommands(*run.edited.sequences[sequence], comparison.items_after, comparison.commands.at(note)))
        {
            ++totals.past_end;
        }
        else if (run.moved_notes[sequence].contains(comparison.commands.at(note)) &&
                 std::ranges::none_of(comparison.missing, moved_here))
        {
            ++totals.changed_moves;
        }
        else if (std::get<3>(note) <= kShortNote)
        {
            ++totals.ticks;
        }
        else if (ContinuesRun(comparison.console, read, note))
        {
            ++totals.runs;
        }
        else
        {
            unexpected.push_back(note);
        }
    }

    return unexpected;
}

// The notes of sound `sound` of sequence `sequence` that the plans expect and `read`, the sound after the edits,
// doesn't play, less those that count apart in `totals`. A moved note that `unexpected` plays at its new tick with
// another key or length leaves `unexpected`.
std::vector<Played> WrongNotes(EditsRun& run, uint32_t sound, uint32_t sequence, const Sequence& read,
                               const Comparison& comparison, std::vector<Played>& unexpected, Totals& totals)
{
    // On Nintendo R&D2's driver, an edit can take a voice from a note where its track runs a note command after the
    // edits (tools::NoteCommandTicks).
    std::optional<std::set<std::pair<int, uint32_t>>> commanded_after;
    const auto voices_after = [&](const Played& note)
    {
        if (!run.archive.Rd2())
        {
            return false;
        }
        if (!commanded_after)
        {
            commanded_after = hokuto::tools::NoteCommandTicks(run.edited, sequence, read);
        }
        return commanded_after->contains({std::get<0>(note), std::get<1>(note)});
    };

    std::map<int, std::set<std::pair<uint32_t, uint32_t>>> rows_before;
    // A moved note that another command changes plays at its new tick with another key or length.
    std::vector<Played> wrong;
    for (const Played& note :
         Unmatched(Unmatched(comparison.missing, comparison.extra, WithoutLength), comparison.extra, WithoutVelocity))
    {
        const auto same_tick = [&note](const Played& other)
        {
            return std::get<0>(other) == std::get<0>(note) && std::get<1>(other) == std::get<1>(note);
        };
        const auto another_here = [&note, &same_tick](const Played& other)
        {
            return other != note && same_tick(other);
        };
        const auto changed = std::find_if(unexpected.begin(), unexpected.end(), same_tick);
        // A moved note that another command starts stays at that command's tick. Its excuses then hold at its old
        // tick, where the read after the edits still plays it. A moved note of a PSG track that shares its channel
        // with the track before can give way to that track's notes (OffCentrePsgNote).
        const auto found_origin = comparison.origins.find(note);
        const Played* moved_from = found_origin != comparison.origins.end() ? &found_origin->second : nullptr;
        const Played* origin = nullptr;
        if (moved_from != nullptr && hokuto::edit::KonamiOf(comparison.console))
        {
            const Played& old = *moved_from;
            const auto track = std::ranges::find(read.tracks, std::get<0>(old), &hokuto::song::Track::number);
            const auto stays = [&old](const Note& other)
            {
                return other.start == std::get<1>(old) && other.key == std::get<2>(old);
            };
            origin = track != read.tracks.end() && std::ranges::any_of(track->notes, stays) ? &old : nullptr;
        }
        if (comparison.moves.contains(note) && changed != unexpected.end())
        {
            unexpected.erase(changed);
            ++totals.changed_moves;
        }
        else if ((comparison.ticks.contains(note) || std::get<3>(note) <= kShortNote) && changed == unexpected.end())
        {
            ++totals.ticks;
        }
        else if (Unstarted(read, note) || voices_after(note))
        {
            ++totals.channels;
        }
        else if (hokuto::edit::KonamiOf(comparison.console) &&
                 (StartedAgain(*run.before[sound], note) ||
                  (origin != nullptr &&
                   (StartedAgain(*run.before[sound], *origin) ||
                    StartedElsewhere(run.file, sequence, *run.before[sound], *origin, rows_before)))))
        {
            ++totals.started_again;
        }
        else if (JoinsPsgNote(read, note, comparison.console))
        {
            ++totals.joined;
        }
        else if (OffCentrePsgNote(read, note, comparison.console) ||
                 OffCentrePsgNote(*run.before[sound], note, comparison.console) ||
                 (moved_from != nullptr && OffCentrePsgNote(*run.before[sound], *moved_from, comparison.console)))
        {
            ++totals.off_centre;
        }
        else if (hokuto::edit::KonamiOf(comparison.console) && ntdlib::konami::IsPsgTrack(std::get<0>(note)) &&
                 std::get<4>(note) == static_cast<uint32_t>(hokuto::song::KonamiVelocityOf(
                                          1, *hokuto::edit::KonamiOf(comparison.console), false)))
        {
            // A PSG note's envelope starts at a sixteenth of its volume, and takes a volume of 1 to nothing.
            ++totals.silenced;
        }
        else if (comparison.console.engine == hokuto::format::Engine::kDse &&
                 comparison.commands_before.contains(note) &&
                 PastCommands(*run.file.sequences[sequence], run.items[sequence], comparison.commands_before.at(note)))
        {
            ++totals.past_end;
        }
        else if (hokuto::edit::KonamiOf(comparison.console) &&
                 (std::ranges::any_of(comparison.moves, another_here) ||
                  (comparison.moves.contains(note) && std::ranges::any_of(comparison.expected, another_here))))
        {
            // Of two notes in a tick of a track, the later command cuts the earlier one in the tick it starts. A
            // move puts a note in the tick of another note, or another note in its tick.
            ++totals.hidden;
        }
        else if (comparison.moves.contains(note) && read.loop && std::get<1>(note) >= read.loop->end)
        {
            ++totals.past_loop;
        }
        else if (ContinuesRun(comparison.console, *run.before[sound], note))
        {
            ++totals.runs;
        }
        else
        {
            wrong.push_back(note);
        }
    }

    return wrong;
}

// On Rare's driver, the performer gives each key of a MIDI channel one note at a time. A note starts a tick late where
// another note of its key still sounds, or ends at its tick on a later track (ntdlib's conversion of the driver). An
// edit of the other note can then move the note's start by a tick. Its end stays. Takes such notes out of `wrong` and
// `unexpected`, and counts them apart in `totals`. `read` is the sound after the edits, and `comparison` contains its
// notes.
void CountRareNotesApart(const Sequence& read, const Comparison& comparison, std::vector<Played>& wrong,
                         std::vector<Played>& unexpected, Totals& totals)
{
    for (auto note = wrong.begin(); note != wrong.end();)
    {
        const Played wanted = *note;
        const auto shifted = [&wanted](const Played& other)
        {
            const uint32_t start = std::get<1>(wanted);
            const uint32_t other_start = std::get<1>(other);
            return std::get<0>(other) == std::get<0>(wanted) && std::get<2>(other) == std::get<2>(wanted) &&
                   std::get<4>(other) == std::get<4>(wanted) &&
                   other_start + std::get<3>(other) == start + std::get<3>(wanted) &&
                   (other_start == start + 1 || other_start + 1 == start);
        };
        const auto found = std::ranges::find_if(unexpected, shifted);
        if (found == unexpected.end())
        {
            ++note;
            continue;
        }
        unexpected.erase(found);
        note = wrong.erase(note);
        ++totals.shifted;
    }

    // An edit can put a note in the frame where another track starts a note of its key on its channel.
    const uint32_t frame = RareFrameTicks(read);
    const auto joins = [&](const Played& note)
    {
        return JoinsRareNote(comparison.notes_before, comparison.actual, frame, note);
    };
    totals.rare_joined += static_cast<std::size_t>(std::ranges::count_if(wrong, joins));
    std::erase_if(wrong, joins);

    // A note's frame can make it louder, or start it a tick away.
    const auto framed = [&](const Played& note)
    {
        return RareFrameNote(comparison.actual, note);
    };
    totals.rare_framed += static_cast<std::size_t>(std::ranges::count_if(wrong, framed));
    std::erase_if(wrong, framed);
}

// On Quintet's driver, a removal can give the noise channel's later drums other keys. Takes the notes of `wrong` that
// come back at another key in `unexpected` out of both, and counts them apart in `totals`.
void CountQuintetDrumsApart(std::vector<Played>& wrong, std::vector<Played>& unexpected, Totals& totals)
{
    const auto renumbered = [](const Played& a, const Played& b)
    {
        return std::get<0>(a) == ntdlib::quintet::kNoise && std::get<0>(a) == std::get<0>(b) &&
               std::get<1>(a) == std::get<1>(b) && std::get<2>(a) != std::get<2>(b) &&
               std::get<3>(a) == std::get<3>(b) && std::get<4>(a) == std::get<4>(b);
    };
    std::erase_if(wrong,
                  [&](const Played& note)
                  {
                      const auto other = std::ranges::find_if(
                          unexpected, [&](const Played& candidate) { return renumbered(note, candidate); });
                      if (other == unexpected.end())
                      {
                          return false;
                      }
                      unexpected.erase(other);
                      ++totals.drums;
                      return true;
                  });
}

// Prints the line of sound `sound` of sequence `sequence`, whose read `read` after the edits misses the notes of
// `wrong` and plays those of `unexpected`, and counts it in `totals`. A sound whose differences all have one cause that
// the check names counts apart.
void ReportSound(const EditsRun& run, uint32_t sound, uint32_t sequence, const Sequence& read,
                 const Comparison& comparison, const std::vector<Played>& wrong, const std::vector<Played>& unexpected,
                 Totals& totals)
{
    // Only the notes of tracks that tie their notes or play one at a time can join others.
    const std::set<int> legato_tracks =
        LegatoTracks(*run.file.sequences[sequence], hokuto::edit::ConsoleOf(run.file), *run.before[sound]);
    const auto joins = [&legato_tracks](const Played& note)
    {
        return legato_tracks.contains(std::get<0>(note));
    };
    const bool legato = std::ranges::all_of(wrong, joins) && std::ranges::all_of(unexpected, joins);

    // A track that waits for its notes' ends can play its later notes at other ticks after an edit.
    const std::set<int> waiting_tracks =
        UnknownWaitTracks(*run.file.sequences[sequence], hokuto::edit::ConsoleOf(run.file), *run.before[sound]);
    const auto waits_for_ends = [&waiting_tracks](const Played& note)
    {
        return waiting_tracks.contains(std::get<0>(note));
    };
    const bool waits =
        !legato && std::ranges::all_of(wrong, waits_for_ends) && std::ranges::all_of(unexpected, waits_for_ends);
    const bool random = !legato && !waits && hokuto::edit::DrawsRandom(run.file, sequence, *run.before[sound]);
    const bool tempo = !legato && !waits && !random && TempoUndecided(run.file, sequence, *run.before[sound]);
    totals.legato += legato ? 1 : 0;
    totals.waits += waits ? 1 : 0;
    totals.random += random ? 1 : 0;
    totals.tempo += tempo ? 1 : 0;
    const bool apart = legato || waits || random || tempo;
    totals.differ += apart ? 0 : 1;
    totals.wrong_notes += apart ? 0 : wrong.size() + unexpected.size();
    std::printf("%s: %s%s: %zu of %zu notes differ, and %zu notes are new. Before: %s. After: %s.\n", run.name.c_str(),
                run.archive.NameOf(sound).c_str(),
                legato   ? " (legato)"
                : waits  ? " (waits for its notes' ends)"
                : random ? " (draws random numbers)"
                : tempo  ? " (tempo from a variable)"
                         : "",
                wrong.size(), comparison.expected.size(), unexpected.size(), Shape(*run.before[sound]).c_str(),
                Shape(read).c_str());
    for (const auto& [notes, what] :
         {std::pair(&std::as_const(wrong), "missing"), std::pair(&std::as_const(unexpected), "new")})
    {
        for (std::size_t i = 0; i < std::min<std::size_t>(notes->size(), 4); ++i)
        {
            const Played& note = (*notes)[i];
            const auto& [track, start, key, length, velocity] = note;
            std::printf("    %s: track %d, tick %u, key %u, %u ticks, velocity %u (%s)\n", what, track, start, key,
                        length, velocity,
                        comparison.edits.contains(note) ? comparison.edits.at(note).c_str() : "in no plan");
        }
    }
}

} // namespace

void CheckSoundAgain(EditsRun& run, uint32_t sound, uint32_t sequence, Totals& totals)
{
    const std::optional<Sequence> read = ReadAfterEdits(run, sound, sequence, totals);
    if (!read)
    {
        return;
    }

    // When the loop moves to another pass, the notes compare up to the earlier loop's end where the performer finds
    // the loop a whole number of passes away. Otherwise they compare up to the earlier end of a first pass, or of a
    // read without a loop. A difference only in a note's length or only in its velocity counts apart.
    const bool other_pass = read->loop != run.before[sound]->loop;
    const uint32_t first_passes = std::min(FirstPassEnd(*read), FirstPassEnd(*run.before[sound]));
    const uint32_t end = other_pass ? EarlierEnd(*run.before[sound], *read).value_or(first_passes) : UINT32_MAX;
    const Comparison comparison = CompareNotes(run, sound, sequence, *read, end);
    std::vector<Played> unexpected = UnexpectedNotes(run, sound, sequence, *read, comparison, totals);
    std::vector<Played> wrong = WrongNotes(run, sound, sequence, *read, comparison, unexpected, totals);

    if (hokuto::edit::IsRare(comparison.console))
    {
        CountRareNotesApart(*read, comparison, wrong, unexpected, totals);
    }
    if (hokuto::edit::IsQuintet(comparison.console))
    {
        CountQuintetDrumsApart(wrong, unexpected, totals);
    }
    totals.other_lengths +=
        comparison.missing.size() - Unmatched(comparison.missing, comparison.extra, WithoutLength).size();
    totals.other_velocities +=
        comparison.missing.size() - Unmatched(comparison.missing, comparison.extra, WithoutVelocity).size();
    totals.other_passes += other_pass ? 1 : 0;
    if (wrong.empty() && unexpected.empty())
    {
        return;
    }

    ReportSound(run, sound, sequence, *read, comparison, wrong, unexpected, totals);
}

} // namespace hokuto::tools::edits
