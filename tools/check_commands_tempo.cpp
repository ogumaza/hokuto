// SPDX-License-Identifier: MIT

// The rounds of hokuto_check_commands that add tempo changes and move the tempo's points.

#include "check_commands.h"

namespace hokuto::tools::commands
{

namespace
{

// What the tempo row shows of a sequence in the sixth round: the first sound that ntdlib's performer reads, the read,
// and the points.
struct TempoShown
{
    uint32_t sound = 0;
    hokuto::song::Sequence song;
    std::vector<TempoPointView> points;
};

} // namespace

TempoRules TempoRulesOf(const CheckedArchive& archive)
{
    if (archive.driver && archive.driver->Rules().DriverEngine() == hokuto::format::Engine::kQuintet)
    {
        // A tempo T runs T ticks of the performer a frame, 37 to one tick of the driver, at 96 ticks of the driver a
        // quarter note. The tempo row shows the real tempo, with the frames of the GBA at kFrameRate.
        return {.ratio = 1.0, .lag = 3, .quarter_ticks = 96 * hokuto::format::kQuintetSongTicks};
    }
    if (archive.gba || archive.driver)
    {
        // MusicPlayer2000 and Nintendo R&D2's driver count a tempo of 150 as a tick a frame, at 24 ticks a beat.
        return {ntdlib::agb::kFrameRate / 60.0, 3};
    }
    if (archive.dse)
    {
        // A sequence's tempo counts for the driver's nominal tick, and the driver runs its ticks by the DS's clock.
        constexpr double kTickMs = 1000.0 * ntdlib::dse::Player::kTickClocks / ntdlib::ntr::kSystemClockHz;
        return {ntdlib::dse::Driver::kTickMicroseconds / 1000.0 / kTickMs, 0, true};
    }
    if (archive.ds)
    {
        constexpr double kFrameHz = static_cast<double>(ntdlib::ntr::kSystemClockHz) / ntdlib::ntr::snd::kFrameClocks;
        return {kFrameHz * 60.0 / (240.0 * 48.0), 0};
    }

    return {};
}

namespace
{

// Whether the performer plays `played` beats a minute where a tempo command sets `bpm`. Where the ratio isn't 1, the
// performer works the tempo out by another sum. The two sums can differ in the last bits.
bool PlaysTempo(double played, double bpm, const TempoRules& rules)
{
    if (rules.quarter_ticks > 0)
    {
        // `bpm` counts frames at kFrameRate: a tempo T sets T * 60 * kFrameRate / quarter_ticks beats a minute.
        const auto tempo = static_cast<uint64_t>(
            std::llround(bpm * static_cast<double>(rules.quarter_ticks) / (60.0 * ntdlib::agb::kFrameRate)));
        return tempo > 0 && played == 60'000'000.0 / ntdlib::agb::QuarterMicros(rules.quarter_ticks, tempo);
    }
    if (rules.ratio == 1.0)
    {
        return played == bpm;
    }

    const double whole = rules.whole_microseconds ? 60'000'000.0 / std::floor(60'000'000.0 / bpm + 1e-6) : bpm;
    const double expected = whole * rules.ratio;
    return std::abs(played - expected) <= expected * 1e-12;
}

// Whether `song` plays a tempo of `bpm` from tick `tick` as PlaysTempo compares them: from up to `rules.lag` ticks
// later, before tick `next`, where a later command sets the tempo.
bool PlaysTempoFrom(const hokuto::song::Sequence& song, uint32_t tick, double bpm, const TempoRules& rules,
                    uint32_t next = std::numeric_limits<uint32_t>::max())
{
    for (uint32_t later = tick; later <= tick + rules.lag && (later == tick || later < next); ++later)
    {
        if (PlaysTempo(hokuto::song::TempoAt(song, later), bpm, rules))
        {
            return true;
        }
    }

    return false;
}

} // namespace

void PrintTempoTotals(const std::string& name, const Totals& totals, bool gba, bool rd2, bool rare, bool quintet)
{
    std::printf(
        "%s: the tempo rows of %zu sequences held %zu points with a value and %zu without. Later commands overrode %zu "
        "of the first in their ticks, and %zu points ran only if a comparison held. %zu changes of the tempo had no "
        "point. %zu commands took a new tempo, and %zu sounds played them. %zu sounds that draw random numbers played "
        "other notes, %zu with notes that wait for the track's notes to end, and %zu others gained or lost notes of a "
        "tick. %zu other sounds played the edited sequences. The tempo rows listed %zu of them, and %zu of those "
        "played otherwise. %zu points moved as far as they could, alone. %zu moved half as far, and %zu sounds played "
        "them. %zu sounds that draw random numbers played other notes after the moves, %zu with notes that wait for "
        "the track's notes to end, and %zu others gained or lost notes of a tick. The loops of %zu sounds started "
        "elsewhere after the new tempos, and of %zu after the moves, with the same notes over both loops "
        "(SameOverLoops).\n",
        name.c_str(), totals.tempo_sequences, totals.tempo_points, totals.tempo_sources, totals.tempo_overridden,
        totals.tempo_conditional, totals.tempo_unpointed, totals.tempo_edits, totals.tempo_sounds, totals.tempo_random,
        totals.tempo_waits, totals.tempo_short, totals.tempo_others, totals.tempo_listed, totals.tempo_reached,
        totals.tempo_farthest, totals.tempo_moves, totals.tempo_move_sounds, totals.tempo_move_random,
        totals.tempo_move_waits, totals.tempo_move_short, totals.tempo_repeats, totals.tempo_move_repeats);
    std::printf(
        "%s: %zu points of the tempo rows lay past the end of a sound without a loop, and %zu after the new "
        "tempos. The performer records no tempo there (PastEnd).\n",
        name.c_str(), totals.tempo_past_end, totals.tempo_drag_past_end);
    if (gba)
    {
        std::printf(
            "%s: apart on the GBA: %zu sounds whose notes find other channels after the new tempos, and %zu "
            "after the moves. %zu sounds whose notes of a PSG channel differ after the new tempos, and %zu after the "
            "moves. %zu points whose tempo didn't play before the next point or the loop's end.\n",
            name.c_str(), totals.tempo_channels, totals.tempo_move_channels, totals.tempo_psg, totals.tempo_move_psg,
            totals.tempo_overtaken);
    }
    if (rd2)
    {
        std::printf(
            "%s: apart on Nintendo R&D2's driver: %zu sounds whose voices took other notes after the new tempos, "
            "and %zu after the moves. %zu notes stood alone in one read, each at a tick of a note command of "
            "its track.\n",
            name.c_str(), totals.tempo_voices, totals.tempo_move_voices, totals.tempo_alone);
    }
    if (rare)
    {
        std::printf(
            "%s: apart on Rare's driver: %zu tunes whose frames took other notes after the new tempos, %zu "
            "after the moved tempos and %zu after the moves of settings. %zu notes stood alone in one "
            "read (FramesDecide and SameOverLoops).\n",
            name.c_str(), totals.tempo_frames, totals.tempo_move_frames, totals.move_frames, totals.frames_alone);
    }
    if (quintet)
    {
        std::printf(
            "%s: apart on Quintet's driver: %zu songs whose notes started in other frames after the new tempos "
            "and %zu after the moved tempos. %zu notes started elsewhere, %zu of them more than a frame away after "
            "resets of their counts, and %zu up to %u frames past what the resets allow. %zu notes split off a "
            "command's note in one read only, %zu drums of the noise channel took other keys, and %zu notes shorter "
            "than %u frames sounded in one read only (QuintetFramesDecide). %zu tempo commands of other channels took "
            "a point's new tempo (app::TempoChangeOf).\n",
            name.c_str(), totals.quintet_frames, totals.quintet_move_frames, totals.quintet_apart.moved,
            totals.quintet_apart.resets, totals.quintet_apart.late, kQuintetLateFrames, totals.quintet_apart.split,
            totals.quintet_apart.drums, totals.quintet_apart.short_notes, kQuintetShortFrames, totals.tempo_partners);
    }
}

namespace
{

// The rungs of the ladder that excuses a sound that plays other notes after an edit of the tempos, in their order.
enum class TempoExcuse
{
    kNone, // the sound plays the same notes
    kRandom,
    kWaits,
    kShort,
    kChannels,
    kPsg,
    kVoices,
    kFrames,
    kQuintetFrames,
    kRepeats,
    kDiffers, // no rung excuses the notes
};

// The first rung that excuses `read` against `before`. `read` is sound `sound` of `after`, and plays sequence
// `sequence` of `file` after an edit of the tempos. `before` is its read from `archive`. Counts the notes that stand
// alone in `totals`.
TempoExcuse TempoExcuseOf(const File& file, uint32_t sequence, const CheckedArchive& archive,
                          const CheckedArchive& after, uint32_t sound, const hokuto::song::Sequence& before,
                          const hokuto::song::Sequence& read, Totals& totals)
{
    bool moved = false;
    const bool same = SamePlaying(before, read, moved);
    if (!same && hokuto::edit::DrawsRandom(file, sequence, before))
    {
        return TempoExcuse::kRandom;
    }
    if (!same && WaitsForNotes(file, sequence, before))
    {
        return TempoExcuse::kWaits;
    }
    if (!same && SamePlaying(before, read, moved, kTickNotes))
    {
        return TempoExcuse::kShort;
    }
    if (!same && (archive.gba || archive.ds) && SameNoteCommands(before, read, moved))
    {
        return TempoExcuse::kChannels;
    }
    if (!same && archive.gba && SameNoteCommands(before, read, moved, true))
    {
        return TempoExcuse::kPsg;
    }
    if (!same && archive.Rd2() && VoicesDecide(file, sequence, before, read, moved, totals.tempo_alone))
    {
        return TempoExcuse::kVoices;
    }
    if (!same && archive.Rare() && FramesDecide(before, read, moved, totals.frames_alone))
    {
        return TempoExcuse::kFrames;
    }
    if (!same && TempoRulesOf(archive).quarter_ticks != 0 &&
        QuintetFramesDecide(before, read, QuintetResets(file, sequence, before), totals.quintet_apart))
    {
        return TempoExcuse::kQuintetFrames;
    }
    const std::optional<RareRepeats> rare = RareRepeatsOf(archive, after, sound, before, read);
    if (!same && SameOverLoops(before, read, {}, rare ? &*rare : nullptr, &totals.frames_alone))
    {
        return TempoExcuse::kRepeats;
    }

    return same ? TempoExcuse::kNone : TempoExcuse::kDiffers;
}

// Tries the farthest moves of tempo `point` of `sequence`, each alone, from `stretch` of the track's `stretches`.
// Counts the moves and the refusals in `totals`.
void TryFarthestTempoMoves(const std::string& name, const File& file, uint32_t sequence, const TempoPointView& point,
                           const hokuto::song::Stretch& stretch, std::span<const hokuto::song::Stretch> stretches,
                           Totals& totals)
{
    for (const int32_t farthest : {-static_cast<int32_t>(point.earlier), static_cast<int32_t>(point.later)})
    {
        const hokuto::edit::SettingMove move{point.command, stretch, farthest};
        try
        {
            if (farthest != 0)
            {
                hokuto::edit::MoveSettings(file, sequence, std::span(&move, 1), stretches);
                ++totals.tempo_farthest;
            }
        }
        catch (const std::exception& error)
        {
            std::printf("%s: sequence %u: the tempo at tick %d can't move by %d ticks: %s\n", name.c_str(), sequence,
                        point.tick, farthest, error.what());
            ++totals.problems;
        }
    }
}

// Checks that each sound of the sequences in `moved` plays its notes again from `edited`, the file with the moved
// tempos, as it played from `file`. The sound that showed a sequence's tempos is compared with its read in `shown`.
void CheckMovedTempoSounds(const std::string& name, const File& file, const File& edited, const CheckedArchive& archive,
                           const std::vector<std::optional<uint32_t>>& sequences,
                           const std::map<uint32_t, TempoShown>& shown, const std::map<uint32_t, Moved>& moved,
                           Totals& totals)
{
    CheckedArchive after;
    try
    {
        hokuto::tools::CheckSaved(edited);
        after = hokuto::tools::BuildChecked(edited, name);
    }
    catch (const std::exception& error)
    {
        std::printf("%s: the file with the moved tempos can't be exported: %s\n", name.c_str(), error.what());
        ++totals.problems;
        return;
    }
    for (uint32_t sound = 0; sound < archive.SoundCount(); ++sound)
    {
        const CheckedSound info = archive.Info(sound);
        const std::optional<uint32_t> sequence =
            info.sequence && info.file_id < sequences.size() ? sequences[info.file_id] : std::nullopt;
        const auto place = sequence ? moved.find(*sequence) : moved.end();
        if (place == moved.end())
        {
            continue;
        }

        try
        {
            const hokuto::song::Sequence before =
                sound == place->second.sound ? shown.at(*sequence).song : archive.Read(sound);
            const hokuto::song::Sequence read = after.Read(sound);
            ++totals.tempo_move_sounds;
            switch (TempoExcuseOf(file, *sequence, archive, after, sound, before, read, totals))
            {
            case TempoExcuse::kRandom:
                ++totals.tempo_move_random;
                continue;
            case TempoExcuse::kWaits:
                ++totals.tempo_move_waits;
                continue;
            case TempoExcuse::kShort:
                ++totals.tempo_move_short;
                continue;
            case TempoExcuse::kChannels:
                ++totals.tempo_move_channels;
                continue;
            case TempoExcuse::kPsg:
                ++totals.tempo_move_psg;
                continue;
            case TempoExcuse::kVoices:
                ++totals.tempo_move_voices;
                continue;
            case TempoExcuse::kFrames:
                ++totals.tempo_move_frames;
                continue;
            case TempoExcuse::kQuintetFrames:
                ++totals.quintet_move_frames;
                continue;
            case TempoExcuse::kRepeats:
                ++totals.tempo_move_repeats;
                continue;
            case TempoExcuse::kDiffers:
                std::printf("%s: %s plays other notes after the moved tempos\n", name.c_str(), info.name.c_str());
                ++totals.problems;
                continue;
            case TempoExcuse::kNone:
                break;
            }
            if (sound == place->second.sound)
            {
                totals.problems += CheckMovedRows(name, file, edited, *sequence, before, read, place->second);
            }
        }
        catch (const std::exception& error)
        {
            std::printf("%s: %s can't be read after the moved tempos: %s\n", name.c_str(), info.name.c_str(),
                        error.what());
            ++totals.problems;
        }
    }
}

// The sixth round's moves of the tempo row's points (`shown`, by sequence), in another version of `file` than its
// drags. Each point with room has to move as far as its room allows, earlier and later, alone. In each sequence, up to
// kMostMoves points then move half as far as their room allows, together: later where a point has more room later, and
// earlier otherwise. Every sound of a sequence with moves has to play its notes as before, at their ticks, from the
// exported archive. A sound that draws random numbers and plays other notes is counted apart. So is a sound that gains
// or loses only notes of a tick (kTickNotes), a sound of the GBA or the DS whose notes find other channels, a sound of
// the GBA whose notes differ only on the PSG channels (SameNoteCommands), and a sound of Nintendo R&D2's driver whose
// voices take other notes (VoicesDecide). The first sound has to run each moved command at its tick and the move's,
// and every other command at its tick.
void TempoMoves(const std::string& name, const File& file, const CheckedArchive& archive,
                const std::vector<std::optional<uint32_t>>& sequences, const std::map<uint32_t, TempoShown>& shown,
                Totals& totals)
{
    File edited = file;
    std::map<uint32_t, Moved> moved;
    for (const auto& [sequence, view] : shown)
    {
        std::vector<hokuto::edit::SettingMove> moves;
        std::map<uint32_t, int32_t> shifts;
        for (const TempoPointView& point : view.points)
        {
            const auto numbered = [&point](const hokuto::song::Track& track)
            {
                return track.number == point.track;
            };
            const auto track = std::find_if(view.song.tracks.begin(), view.song.tracks.end(), numbered);
            if (!point.fixed.empty() || point.earlier + point.later == 0 || shifts.contains(point.command) ||
                track == view.song.tracks.end() || point.stretch >= track->stretches.size())
            {
                continue;
            }

            // The farthest moves, alone.
            const hokuto::song::Stretch& stretch = track->stretches[point.stretch];
            TryFarthestTempoMoves(name, file, sequence, point, stretch, track->stretches, totals);

            const int32_t shift = point.later >= point.earlier ? static_cast<int32_t>((point.later + 1) / 2)
                                                               : -static_cast<int32_t>((point.earlier + 1) / 2);
            if (shifts.size() < kMostMoves)
            {
                shifts[point.command] = shift;
                moves.push_back({point.command, stretch, shift});
            }
        }
        if (moves.empty())
        {
            continue;
        }

        try
        {
            Edited edited_moves = hokuto::edit::MoveSettings(edited, sequence, moves, RowsOfSound(view.song));
            if (hokuto::tools::Outgrows(archive, edited_moves.file, sequence))
            {
                ++totals.outgrown;
                continue;
            }
            edited = std::move(edited_moves.file);
            moved[sequence] = Moved{view.sound, shifts, std::move(edited_moves.items), {}};
            totals.tempo_moves += moves.size();
        }
        catch (const std::exception& error)
        {
            std::printf("%s: sequence %u: the tempos can't move: %s\n", name.c_str(), sequence, error.what());
            ++totals.problems;
        }
    }
    if (moved.empty())
    {
        return;
    }

    CheckMovedTempoSounds(name, file, edited, archive, sequences, shown, moved, totals);
}

// The points against the performer's tempo. A point that a later command overrides in its tick decides nothing.
// Neither does a command that runs only if the last comparison held, nor a point before it in its tick: the command
// may not run.
bool Overridden(const std::vector<TempoPointView>& points, std::size_t index)
{
    return index + 1 < points.size() && points[index + 1].tick == points[index].tick;
}

bool Decides(const std::vector<TempoPointView>& points, std::size_t index)
{
    return points[index].bpm && !points[index].conditional && !Overridden(points, index);
}

// The tick where the tempo of point `index` of `points` stops playing in `song`: the tick of the next point, or the end
// of the loop's first pass, where the performer stops. On the GBA, that tick can come before the point's tempo takes
// effect: up to TempoRules::lag ticks after the point. `song` then never plays the point's tempo, `bpm`, alone.
uint32_t NextTick(const hokuto::song::Sequence& song, const std::vector<TempoPointView>& points, std::size_t index)
{
    const uint32_t end = song.loop ? song.loop->end : song.length;
    return index + 1 < points.size() ? std::min(static_cast<uint32_t>(points[index + 1].tick), end) : end;
}

// Whether `song` never plays `bpm`, the tempo of point `index` of `points`, alone, by `rules` (NextTick).
bool Overtaken(const hokuto::song::Sequence& song, const std::vector<TempoPointView>& points, std::size_t index,
               double bpm, const TempoRules& rules)
{
    const auto tick = static_cast<uint32_t>(points[index].tick);
    const uint32_t next = NextTick(song, points, index);
    return rules.lag > 0 && next <= tick + rules.lag && !PlaysTempoFrom(song, tick, bpm, rules, next);
}

// Checks the points of each sequence of `shown` against the performer's tempo, by `rules`, and that a point shows each
// change of the tempo.
void CheckRowPoints(const std::string& name, const CheckedArchive& archive, const std::map<uint32_t, TempoShown>& shown,
                    const TempoRules& rules, Totals& totals)
{
    const bool gba = archive.gba != nullptr;
    for (const auto& [sequence, view] : shown)
    {
        const std::vector<TempoPointView>& points = view.points;
        totals.tempo_sequences += points.empty() ? 0 : 1;
        for (std::size_t i = 0; i < points.size(); ++i)
        {
            totals.tempo_points += points[i].bpm ? 1 : 0;
            totals.tempo_sources += points[i].bpm ? 0 : 1;
            totals.tempo_overridden += points[i].bpm && Overridden(points, i) ? 1 : 0;
            totals.tempo_conditional += points[i].conditional ? 1 : 0;
            const auto tick = static_cast<uint32_t>(points[i].tick);
            if (Decides(points, i) && PastEnd(view.song, tick))
            {
                ++totals.tempo_past_end;
                continue;
            }
            if (Decides(points, i) && Overtaken(view.song, points, i, *points[i].bpm, rules))
            {
                ++totals.tempo_overtaken;
                continue;
            }
            if (Decides(points, i) &&
                !PlaysTempoFrom(view.song, tick, *points[i].bpm, rules, NextTick(view.song, points, i)))
            {
                std::printf("%s: sequence %u: the point at tick %u sets %g BPM, and the performer plays %g\n",
                            name.c_str(), sequence, tick, *points[i].bpm, hokuto::song::TempoAt(view.song, tick));
                ++totals.problems;
            }
        }

        // The performer records the tempo at each timebase command too. A record of the tempo that plays already is no
        // change. MusicPlayer2000 starts a song at kDefaultTempo, and the GBA's other drivers at their start tempos.
        // Brownie Brown's driver has no tempo, and a song starts at the tempo of the beat that the MIDI export sets for
        // it. Ubisoft Milan's driver has no tempo command, and the performer records the piece's speed at tick 0. A
        // point can come up to TempoRules::lag ticks before its change on the GBA.
        double last = gba              ? ntdlib::mp2k::kDefaultTempo
                      : archive.driver ? archive.driver->Rules().StartTempoBpm().value_or(120.0)
                                       : 120.0;
        if (rules.quarter_ticks > 0)
        {
            // Quintet's start tempo counts frames of 60 Hz, and `last` counts the frames at kFrameRate.
            last *= ntdlib::agb::kFrameRate / 60.0;
        }
        if (archive.Brownie() && !view.song.tempo.empty())
        {
            last = view.song.tempo.front().bpm / rules.ratio;
        }
        if (archive.UbiMilan() && !view.song.tempo.empty() && view.song.tempo.front().tick == 0)
        {
            last = view.song.tempo.front().bpm / rules.ratio;
        }
        for (const hokuto::song::Tempo& change : view.song.tempo)
        {
            const auto at = [&change, &rules](const TempoPointView& point)
            {
                const auto tick = static_cast<uint32_t>(point.tick);
                return tick <= change.tick && change.tick <= tick + rules.lag;
            };
            const bool past = view.song.loop && change.tick >= view.song.loop->end;
            const bool changes = !PlaysTempo(change.bpm, last, rules);
            last = change.bpm / rules.ratio;
            if (changes && !past && std::none_of(points.begin(), points.end(), at) && archive.Krawall())
            {
                ++totals.tempo_effects;
            }
            else if (changes && !past && std::none_of(points.begin(), points.end(), at))
            {
                std::printf("%s: sequence %u: the tempo changes to %g BPM at tick %u, where the row has no point\n",
                            name.c_str(), sequence, change.bpm, change.tick);
                ++totals.tempo_unpointed;
                ++totals.problems;
            }
        }
    }
}

// Drags the tempo points of each sequence of `shown` in a version of `file`, up to kMostTempos a sequence, and returns
// it. `tempos` receives the new tempos, by sequence and command.
File DragTempoPoints(const std::string& name, const File& file, const std::map<uint32_t, TempoShown>& shown,
                     std::map<uint32_t, std::map<uint32_t, double>>& tempos, Totals& totals)
{
    File edited = file;
    for (const auto& [sequence, view] : shown)
    {
        std::map<uint32_t, double>& changed = tempos[sequence];
        for (const TempoPointView& point : view.points)
        {
            if (!point.bpm || changed.contains(point.command) || changed.size() >= kMostTempos)
            {
                continue;
            }

            const auto numbered = [&point](const hokuto::song::Track& track)
            {
                return track.number == point.track;
            };
            const auto track = std::find_if(view.song.tracks.begin(), view.song.tracks.end(), numbered);
            const double bpm = *point.bpm > 200.0 ? *point.bpm - 7.0 : *point.bpm + 7.0;
            const std::optional<Command> command = hokuto::app::TempoCommand(edited, sequence, point, bpm);
            if (!command || track == view.song.tracks.end() || point.stretch >= track->stretches.size())
            {
                std::printf("%s: sequence %u: the point at tick %d leads to no tempo command\n", name.c_str(), sequence,
                            point.tick);
                ++totals.problems;
                continue;
            }

            try
            {
                // The drag of the tempo row also sets the new tempo in the commands of the same tempo on the other
                // tracks at the tick of the point (app::TempoChangeOf).
                const hokuto::app::TempoChange change =
                    hokuto::app::TempoChangeOf(edited, sequence, view.song, point, *command);
                edited = hokuto::edit::ChangeCommands(edited, sequence, change.places, change.commands).file;
                totals.tempo_partners += change.places.size() - 1;
                changed[point.command] = hokuto::app::TempoBpmOf(hokuto::edit::ConsoleOf(edited), command->value.value);
                ++totals.tempo_edits;
            }
            catch (const std::exception& error)
            {
                std::printf("%s: sequence %u: the tempo at tick %d can't change: %s\n", name.c_str(), sequence,
                            point.tick, error.what());
                ++totals.problems;
            }
        }
    }

    return edited;
}

// Checks that the first sound of each sequence of `shown` that `tempos` edits plays its notes and its new tempos from
// `after`, the archive with the new tempos, by `rules`.
void CheckFirstSounds(const std::string& name, const File& file, const CheckedArchive& archive,
                      const CheckedArchive& after, const std::map<uint32_t, TempoShown>& shown,
                      std::map<uint32_t, std::map<uint32_t, double>>& tempos, const TempoRules& rules, Totals& totals)
{
    for (const auto& [sequence, view] : shown)
    {
        const std::map<uint32_t, double>& changed = tempos[sequence];
        if (changed.empty())
        {
            continue;
        }

        ++totals.tempo_sounds;
        const std::string& sound = archive.NameOf(view.sound);
        try
        {
            const hokuto::song::Sequence read = after.Read(view.sound);
            switch (TempoExcuseOf(file, sequence, archive, after, view.sound, view.song, read, totals))
            {
            case TempoExcuse::kRandom:
                ++totals.tempo_random;
                continue;
            case TempoExcuse::kWaits:
                ++totals.tempo_waits;
                continue;
            case TempoExcuse::kShort:
                ++totals.tempo_short;
                continue;
            case TempoExcuse::kChannels:
                ++totals.tempo_channels;
                continue;
            case TempoExcuse::kPsg:
                ++totals.tempo_psg;
                continue;
            case TempoExcuse::kVoices:
                ++totals.tempo_voices;
                continue;
            case TempoExcuse::kFrames:
                ++totals.tempo_frames;
                continue;
            case TempoExcuse::kQuintetFrames:
                ++totals.quintet_frames;
                continue;
            case TempoExcuse::kRepeats:
                ++totals.tempo_repeats;
                continue;
            case TempoExcuse::kDiffers:
                std::printf("%s: %s plays other notes after the new tempos\n", name.c_str(), sound.c_str());
                ++totals.problems;
                continue;
            case TempoExcuse::kNone:
                break;
            }

            for (std::size_t i = 0; i < view.points.size(); ++i)
            {
                const TempoPointView& point = view.points[i];
                if (!Decides(view.points, i))
                {
                    continue;
                }
                if (PastEnd(read, static_cast<uint32_t>(point.tick)))
                {
                    ++totals.tempo_drag_past_end;
                    continue;
                }

                const auto found = changed.find(point.command);
                const double expected = found != changed.end() ? found->second : *point.bpm;
                const double played = hokuto::song::TempoAt(read, static_cast<uint32_t>(point.tick));
                if (Overtaken(read, view.points, i, expected, rules))
                {
                    continue;
                }
                if (!PlaysTempoFrom(read, static_cast<uint32_t>(point.tick), expected, rules,
                                    NextTick(read, view.points, i)))
                {
                    std::printf("%s: %s plays %g BPM at tick %d after the new tempos, not %g\n", name.c_str(),
                                sound.c_str(), played, point.tick, expected);
                    ++totals.problems;
                }
            }
        }
        catch (const std::exception& error)
        {
            std::printf("%s: %s can't be read after the new tempos: %s\n", name.c_str(), sound.c_str(), error.what());
            ++totals.problems;
        }
    }
}

// The other sounds that the tempo row lists for the commands of each sequence of `shown` that `tempos` edits, by
// sequence.
std::map<uint32_t, std::set<uint32_t>> ListedSounds(const std::string& name, const File& file,
                                                    std::span<const std::optional<hokuto::edit::SoundPlace>> places,
                                                    const std::map<uint32_t, TempoShown>& shown,
                                                    std::map<uint32_t, std::map<uint32_t, double>>& tempos,
                                                    Totals& totals)
{
    std::map<uint32_t, std::set<uint32_t>> listed;
    for (const auto& [sequence, view] : shown)
    {
        std::vector<uint32_t> commands;
        for (const auto& [command, bpm] : tempos[sequence])
        {
            commands.push_back(command);
        }
        try
        {
            for (const std::vector<uint32_t>& sounds : hokuto::edit::SoundsReaching(file, places, view.sound, commands))
            {
                listed[sequence].insert(sounds.begin(), sounds.end());
            }
        }
        catch (const std::exception& error)
        {
            std::printf("%s: sequence %u: the tempo row's reach can't be found: %s\n", name.c_str(), sequence,
                        error.what());
            ++totals.problems;
        }
    }

    return listed;
}

// Checks that each other sound of a sequence of `shown` that `tempos` edits plays from `after` as before, unless
// `listed` lists it.
void CheckOtherSounds(const std::string& name, const CheckedArchive& archive, const CheckedArchive& after,
                      const std::vector<std::optional<uint32_t>>& sequences,
                      const std::map<uint32_t, TempoShown>& shown,
                      std::map<uint32_t, std::map<uint32_t, double>>& tempos,
                      std::map<uint32_t, std::set<uint32_t>>& listed, Totals& totals)
{
    for (uint32_t sound = 0; sound < archive.SoundCount(); ++sound)
    {
        const CheckedSound info = archive.Info(sound);
        const std::optional<uint32_t> sequence =
            info.sequence && info.file_id < sequences.size() ? sequences[info.file_id] : std::nullopt;
        const auto view = sequence ? shown.find(*sequence) : shown.end();
        if (view == shown.end() || view->second.sound == sound || tempos[*sequence].empty())
        {
            continue;
        }

        std::optional<hokuto::song::Sequence> before;
        try
        {
            before = archive.Read(sound);
        }
        catch (const std::exception&)
        {
            continue;
        }

        ++totals.tempo_others;
        const bool listed_sound = listed[*sequence].contains(sound);
        totals.tempo_listed += listed_sound ? 1 : 0;
        try
        {
            const hokuto::song::Sequence read = after.Read(sound);
            bool moved = false;
            const bool changed = read.tempo != before->tempo || !SamePlaying(*before, read, moved);
            if (changed && listed_sound)
            {
                ++totals.tempo_reached;
            }
            else if (changed)
            {
                std::printf("%s: %s plays otherwise after the new tempos of %s, but the tempo row lists it for none\n",
                            name.c_str(), info.name.c_str(), archive.NameOf(view->second.sound).c_str());
                ++totals.problems;
            }
        }
        catch (const std::exception& error)
        {
            std::printf("%s: %s can't be read after the new tempos: %s\n", name.c_str(), info.name.c_str(),
                        error.what());
            ++totals.problems;
        }
    }
}

} // namespace

void TempoRow(const std::string& name, const File& file, const CheckedArchive& archive,
              const std::vector<std::optional<uint32_t>>& sequences,
              std::span<const std::optional<hokuto::edit::SoundPlace>> places, const std::set<uint32_t>& checked,
              Totals& totals)
{
    if (archive.Konami())
    {
        return;
    }

    std::map<uint32_t, TempoShown> shown;
    for (uint32_t sound = 0; sound < archive.SoundCount(); ++sound)
    {
        const CheckedSound info = archive.Info(sound);
        if (!info.sequence || info.file_id >= sequences.size() || !sequences[info.file_id] ||
            !checked.contains(*sequences[info.file_id]) || shown.contains(*sequences[info.file_id]))
        {
            continue;
        }

        try
        {
            const uint32_t sequence = *sequences[info.file_id];
            hokuto::song::Sequence song = archive.Read(sound);
            std::vector<TempoPointView> points = hokuto::app::TempoPoints(file, sequence, song);
            shown.emplace(sequence, TempoShown{sound, std::move(song), std::move(points)});
        }
        catch (const std::exception&)
        {
            continue;
        }
    }

    // The points against the performer's tempo.
    const TempoRules rules = TempoRulesOf(archive);
    CheckRowPoints(name, archive, shown, rules, totals);

    // The drags, in one version of the file. A value's change leaves every command in its place.
    std::map<uint32_t, std::map<uint32_t, double>> tempos;
    const File edited = DragTempoPoints(name, file, shown, tempos, totals);

    // The first sound of each edited sequence plays its notes and its tempos from the exported archive.
    CheckedArchive after;
    try
    {
        hokuto::tools::CheckSaved(edited);
        after = hokuto::tools::BuildChecked(edited, name);
    }
    catch (const std::exception& error)
    {
        std::printf("%s: the file with the new tempos can't be exported: %s\n", name.c_str(), error.what());
        ++totals.problems;
        return;
    }
    CheckFirstSounds(name, file, archive, after, shown, tempos, rules, totals);

    // The other sounds that the tempo row lists for the edited commands of each sequence.
    std::map<uint32_t, std::set<uint32_t>> listed = ListedSounds(name, file, places, shown, tempos, totals);

    // Each other sound of an edited sequence plays as before, unless the tempo row lists it.
    CheckOtherSounds(name, archive, after, sequences, shown, tempos, listed, totals);

    TempoMoves(name, file, archive, sequences, shown, totals);
}

} // namespace hokuto::tools::commands
