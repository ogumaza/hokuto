// SPDX-License-Identifier: MIT

// The parts of a track, its mix settings at a tick, the first pass of a loop, and the map between the ticks, the
// milliseconds and the frames of a sequence as shown and as played.

#include "song/sequence_parts.h"

namespace hokuto::song
{
namespace
{

using sequence_parts::OutermostCalls;
using sequence_parts::TrackSettingsOf;

// The milliseconds that a tick lasts at `timebase` ticks a quarter note and `bpm` quarter notes a minute. A tempo of 0
// stops the ticks.
double MsPerTick(int timebase, double bpm)
{
    return bpm > 0.0 ? 60'000.0 / (bpm * timebase) : 0.0;
}

// MsOfTick and TickOfMs for `timebase` ticks a quarter note and the changes of the tempo in `tempos`.
double MsAt(int timebase, std::span<const Tempo> tempos, double tick)
{
    // Each stretch of ticks at one tempo adds its time.
    double ms = 0.0;
    double from = 0.0;
    double bpm = 120.0;
    for (const Tempo& tempo : tempos)
    {
        if (tempo.tick >= tick)
        {
            break;
        }

        ms += (tempo.tick - from) * MsPerTick(timebase, bpm);
        from = tempo.tick;
        bpm = tempo.bpm;
    }

    return ms + (tick - from) * MsPerTick(timebase, bpm);
}

double TickAt(int timebase, std::span<const Tempo> tempos, double ms)
{
    // The stretch of ticks at one tempo that holds `ms`.
    double at = 0.0;
    double from = 0.0;
    double bpm = 120.0;
    for (const Tempo& tempo : tempos)
    {
        const double next = at + (tempo.tick - from) * MsPerTick(timebase, bpm);
        if (next > ms)
        {
            break;
        }

        at = next;
        from = tempo.tick;
        bpm = tempo.bpm;
    }

    const double per_tick = MsPerTick(timebase, bpm);
    return per_tick > 0.0 ? from + (ms - at) / per_tick : from;
}

} // namespace

std::optional<std::size_t> OutermostCall(const Track& track, std::size_t stretch)
{
    std::map<std::size_t, uint32_t> returns;
    const std::vector<std::optional<std::size_t>> calls = OutermostCalls(track, returns);
    return stretch < calls.size() ? calls[stretch] : std::nullopt;
}

namespace
{

// The ticks at which the loop's start and end divide `sequence` into sections.
std::vector<uint32_t> SectionBounds(const Sequence& sequence)
{
    std::vector<uint32_t> bounds;
    if (sequence.loop)
    {
        for (const uint32_t bound : {sequence.loop->start, sequence.loop->end})
        {
            if (bound > 0 && (bounds.empty() || bound > bounds.back()))
            {
                bounds.push_back(bound);
            }
        }
    }

    return bounds;
}

// The notes of `track` in the order of their starts, each with whether the driver never started it.
std::vector<std::pair<const Note*, bool>> NotesByStart(const Track& track)
{
    std::vector<std::pair<const Note*, bool>> notes;
    notes.reserve(track.notes.size() + track.unstarted.size());
    for (std::size_t a = 0, b = 0; a < track.notes.size() || b < track.unstarted.size();)
    {
        const bool never_started =
            b < track.unstarted.size() && (a == track.notes.size() || track.unstarted[b].start < track.notes[a].start);
        notes.emplace_back(never_started ? &track.unstarted[b++] : &track.notes[a++], never_started);
    }

    return notes;
}

} // namespace

std::vector<Part> SplitParts(const Sequence& sequence, const Track& track, int grid, int gap)
{
    // The loop's start and end divide the sequence into sections. A part stays inside its section.
    const std::vector<uint32_t> bounds = SectionBounds(sequence);

    const auto section_of = [&bounds](uint32_t tick)
    {
        return static_cast<std::size_t>(std::upper_bound(bounds.begin(), bounds.end(), tick) - bounds.begin());
    };

    // The call that each note ran in, and where each call returned.
    std::map<std::size_t, uint32_t> returns;
    const std::vector<std::optional<std::size_t>> calls = OutermostCalls(track, returns);
    const auto call_of = [&calls](const Note& note)
    {
        return note.stretch < calls.size() ? calls[note.stretch] : std::nullopt;
    };

    // The notes in the order of their starts, the notes that the driver never started among them. Such a note has no
    // stretch that ran it. Such a note falls in the call of the previous part until that call returns.
    const std::vector<std::pair<const Note*, bool>> notes = NotesByStart(track);

    // Each note joins the part before it, unless it's in another section or another call, or the gap lies between them.
    // The notes of one call join its part whatever lies between them.
    const auto ticks = [&sequence](int quarters)
    {
        return static_cast<uint32_t>(std::max(sequence.timebase * std::max(quarters, 1), 1));
    };
    const uint32_t line = ticks(grid);
    const uint32_t gap_ticks = ticks(gap);
    std::vector<Part> parts;
    std::optional<std::size_t> part_call;
    std::size_t section = 0;
    uint32_t notes_end = 0;

    // Where the last part ends without a part after it: at its call's return, or at the line after its notes, inside
    // its section.
    const auto end_of_part = [&]
    {
        const auto returned = part_call ? returns.find(*part_call) : returns.end();
        const uint32_t end = returned != returns.end() ? returned->second : (notes_end + line - 1) / line * line;
        const uint32_t section_end = section < bounds.size() ? bounds[section] : std::numeric_limits<uint32_t>::max();
        return std::max(std::min(end, section_end), parts.back().start + 1);
    };
    std::size_t started = 0;
    std::size_t unstarted = 0;
    for (const auto& [entry, never_started] : notes)
    {
        // The note's place among the track's notes or among those that the driver never started.
        const Note& note = *entry;
        const std::size_t place = never_started ? unstarted++ : started++;
        const auto returned = part_call ? returns.find(*part_call) : returns.end();
        const bool in_call = part_call && (returned == returns.end() || note.start < returned->second);
        const std::optional<std::size_t> call = !never_started ? call_of(note) : in_call ? part_call : std::nullopt;
        const std::size_t note_section = section_of(note.start);
        if (!parts.empty() && note_section == section && call == part_call &&
            (call || note.start < notes_end + gap_ticks))
        {
            (never_started ? parts.back().unstarted_count : parts.back().count)++;
            notes_end = std::max(notes_end, note.start + note.length);
            continue;
        }

        // A call's part starts at the call, and another part at the line before its first note. A part after a call's
        // part starts once the call has returned, and a part before a call's part ends where the call starts.
        const uint32_t section_start = note_section > 0 ? bounds[note_section - 1] : 0;
        uint32_t start = std::max(call ? track.stretches[*call].tick : note.start / line * line, section_start);
        if (!parts.empty())
        {
            const uint32_t end = end_of_part();
            start = part_call ? std::max(start, std::min(end, note.start)) : start;
            parts.back().end = std::max(std::min(end, start), parts.back().start + 1);
        }

        section = note_section;
        part_call = call;
        const std::optional<uint32_t> subroutine =
            call ? std::optional<uint32_t>(track.stretches[*call].first) : std::nullopt;
        parts.push_back({start, 0, never_started ? started : place, never_started ? 0u : 1u, subroutine,
                         never_started ? place : unstarted, never_started ? 1u : 0u});
        notes_end = note.start + note.length;
    }

    if (!parts.empty())
    {
        parts.back().end = end_of_part();
    }

    return parts;
}

int SettingAt(const std::vector<Setting>& settings, uint32_t tick, int fallback)
{
    const auto after = std::upper_bound(settings.begin(), settings.end(), tick,
                                        [](uint32_t value, const Setting& setting) { return value < setting.tick; });
    return after == settings.begin() ? fallback : std::prev(after)->value;
}

const std::vector<Setting>& MixSettings(const Track& track, Mix setting)
{
    return TrackSettingsOf(track, setting);
}

int MixDefault(Mix setting, bool starts_silent)
{
    return setting == Mix::kVolume && !starts_silent ? 127 : 0;
}

int MixAt(const Track& track, Mix setting, uint32_t tick)
{
    return SettingAt(MixSettings(track, setting), tick, MixDefault(setting));
}

Sequence FirstPass(Sequence sequence, Loop loop)
{
    const uint32_t end = loop.end;
    sequence.loop = loop;
    sequence.length = end;
    sequence.notes_end = 0;
    std::erase_if(sequence.tempo, [end](const Tempo& tempo) { return tempo.tick >= end && tempo.tick > 0; });
    const auto later = [end](const Setting& setting)
    {
        return setting.tick >= end;
    };
    for (Track& track : sequence.tracks)
    {
        std::erase_if(track.notes, [end](const Note& note) { return note.start >= end; });
        for (Note& note : track.notes)
        {
            note.length = std::min(note.length, end - note.start);
            sequence.notes_end = std::max(sequence.notes_end, note.start + note.length);
        }

        std::erase_if(track.volume, later);
        std::erase_if(track.pan, later);
        for (std::vector<Setting>& sends : track.sends)
        {
            std::erase_if(sends, later);
        }
    }

    return sequence;
}

std::optional<Loop> FirstLoopOfConversion(const Sequence& shown, const Sequence& played)
{
    if (!shown.loop || !played.loop || shown.loop->end <= shown.loop->start || played.loop->end <= played.loop->start)
    {
        return std::nullopt;
    }

    // The passes between the two loops' starts, by time.
    const double shown_start = MsOfTick(shown, shown.loop->start);
    const double shown_pass = MsOfTick(shown, shown.loop->end) - shown_start;
    const double played_start = MsOfTick(played, played.loop->start);
    const double played_pass = MsOfTick(played, played.loop->end) - played_start;
    const uint32_t length = played.loop->end - played.loop->start;
    const double passes = played_pass > 0.0 ? std::round((played_start - shown_start) / played_pass) : 0.0;
    if (passes < 1.0 || passes * length > played.loop->start)
    {
        return std::nullopt;
    }

    // The first pass has to start at the time of `shown`'s, and last as long.
    const Loop first{played.loop->start - static_cast<uint32_t>(passes) * length,
                     played.loop->end - static_cast<uint32_t>(passes) * length};
    const double first_start = MsOfTick(played, first.start);
    const double reach = MsOfTick(played, first.start + played.timebase / 8.0) - first_start;
    const bool fits = std::abs(first_start - shown_start) <= reach && std::abs(played_pass - shown_pass) <= reach;
    return fits ? std::optional(first) : std::nullopt;
}

bool SameTiming(const Sequence& a, const Sequence& b)
{
    return a.timebase == b.timebase && a.tempo == b.tempo && a.loop == b.loop;
}

std::array<uint32_t, 32> TracksOfConversion(const Sequence& shown, const Sequence& played)
{
    // The places of each shown track's notes, by key, in the conversion's ticks (TickMap).
    const TickMap map = played.arrangement_ticks ? *played.arrangement_ticks : TickMap(shown, played);
    std::vector<std::set<std::pair<uint8_t, uint64_t>>> places;
    places.reserve(shown.tracks.size());
    for (const Track& track : shown.tracks)
    {
        std::set<std::pair<uint8_t, uint64_t>>& notes = places.emplace_back();
        for (const Note& note : track.notes)
        {
            notes.emplace(shown.driver != nullptr ? note.midi_key : note.key,
                          static_cast<uint64_t>(std::max(std::floor(map.Played(note.start)), 0.0)));
        }
    }

    // A conversion's note counts as far as an eighth of a quarter note from its place. The distance to the nearest
    // place of its key within that reach. Nothing where no place is that near.
    const uint64_t reach = static_cast<uint64_t>(std::max(played.timebase / 8, 1));
    const auto distance_to = [reach](const std::set<std::pair<uint8_t, uint64_t>>& notes,
                                     const Note& note) -> std::optional<uint64_t>
    {
        const uint64_t tick = note.start;
        std::optional<uint64_t> nearest;
        for (auto at = notes.lower_bound(std::pair(note.key, tick - std::min(tick, reach)));
             at != notes.end() && at->first == note.key && at->second <= tick + reach; ++at)
        {
            const uint64_t distance = at->second < tick ? tick - at->second : at->second - tick;
            nearest = std::min(nearest.value_or(distance), distance);
        }

        return nearest;
    };

    std::array<uint32_t, 32> tracks{};
    for (const Track& track : played.tracks)
    {
        std::size_t most = 0;
        uint64_t nearest = 0;
        std::optional<std::size_t> best;
        for (std::size_t i = 0; i < shown.tracks.size(); i++)
        {
            std::size_t shared = 0;
            uint64_t distance = 0;
            for (const Note& note : track.notes)
            {
                if (const std::optional<uint64_t> away = distance_to(places[i], note))
                {
                    shared++;
                    distance += *away;
                }
            }

            const auto number = static_cast<std::size_t>(shown.tracks[i].number) & 31;
            const bool free_track = tracks[number] == 0;
            const bool free_best = best && tracks[static_cast<std::size_t>(shown.tracks[*best].number) & 31] == 0;
            if (shared > most || (shared > 0 && shared == most &&
                                  (distance < nearest || (distance == nearest && free_track && !free_best))))
            {
                most = shared;
                nearest = distance;
                best = i;
            }
        }
        if (best)
        {
            tracks[static_cast<std::size_t>(shown.tracks[*best].number) & 31] |=
                1U << (static_cast<unsigned>(track.number) & 31);
        }
    }

    return tracks;
}

double TempoAt(const Sequence& sequence, uint32_t tick)
{
    double bpm = 120.0;
    for (const Tempo& tempo : sequence.tempo)
    {
        if (tempo.tick > tick)
        {
            break;
        }

        bpm = tempo.bpm;
    }

    return bpm;
}

double MsOfTick(const Sequence& sequence, double tick)
{
    return MsAt(sequence.timebase, sequence.tempo, tick);
}

double TickOfMs(const Sequence& sequence, double ms)
{
    return TickAt(sequence.timebase, sequence.tempo, ms);
}

bool TickMap::KeepsQuarters(const Sequence& shown, const Sequence& played, double reach) const
{
    std::set<double> quarters = {static_cast<double>(shown.length) / shown_.timebase,
                                 static_cast<double>(played.length) / played_.timebase};
    for (const Tempo& tempo : shown_.tempo)
    {
        quarters.insert(static_cast<double>(tempo.tick) / shown_.timebase);
    }

    for (const Tempo& tempo : played_.tempo)
    {
        quarters.insert(static_cast<double>(tempo.tick) / played_.timebase);
    }

    const auto keeps_quarters = [this, reach](double quarter)
    {
        const double timed =
            TickAt(played_.timebase, played_.tempo, MsAt(shown_.timebase, shown_.tempo, quarter * shown_.timebase));
        return std::abs(timed - quarter * played_.timebase) <= reach;
    };
    return std::ranges::all_of(quarters, keeps_quarters);
}

std::vector<TickMap::Point> TickMap::FixedPoints(const Sequence& shown, const Sequence& played)
{
    std::vector<Point> fixed = {{0.0, 0.0}};
    const std::optional<Loop> first_loop = FirstLoopOfConversion(shown, played);
    const std::optional<Loop>& played_loop = first_loop ? first_loop : played.loop;
    if (shown.loop && played_loop && shown.loop->start < shown.loop->end && played_loop->start < played_loop->end)
    {
        for (const Point point :
             {Point{static_cast<double>(shown.loop->start), static_cast<double>(played_loop->start)},
              Point{static_cast<double>(shown.loop->end), static_cast<double>(played_loop->end)}})
        {
            if (point.shown > fixed.back().shown && point.played > fixed.back().played)
            {
                fixed.push_back(point);
            }
        }
    }

    return fixed;
}

std::vector<TickMap::Point> TickMap::NotePoints(const Sequence& shown, const Sequence& played, double reach) const
{
    // The starts of the conversion's notes by their keys, in order.
    std::map<int, std::vector<uint32_t>> starts;
    for (const Track& track : played.tracks)
    {
        for (const Note& note : track.notes)
        {
            starts[note.key].push_back(note.start);
        }
    }

    for (auto& [key, ticks] : starts)
    {
        std::ranges::sort(ticks);
    }

    // Each note of the arrangement, and the conversion's note with its key that starts nearest its time.
    std::vector<Point> notes;
    for (const Track& track : shown.tracks)
    {
        for (const Note& note : track.notes)
        {
            const auto found = starts.find(shown.driver != nullptr ? note.midi_key : note.key);
            if (found == starts.end())
            {
                continue;
            }

            const double timed =
                TickAt(played_.timebase, played_.tempo, MsAt(shown_.timebase, shown_.tempo, note.start));
            const auto after = std::ranges::lower_bound(found->second, timed);
            std::optional<double> nearest;
            if (after != found->second.end())
            {
                nearest = *after;
            }
            if (after != found->second.begin() && (!nearest || timed - *std::prev(after) < *nearest - timed))
            {
                nearest = *std::prev(after);
            }
            if (nearest && std::abs(*nearest - timed) <= reach)
            {
                notes.push_back({static_cast<double>(note.start), *nearest});
            }
        }
    }

    const auto order = [](const Point& a, const Point& b)
    {
        return std::tie(a.shown, a.played) < std::tie(b.shown, b.played);
    };
    std::ranges::sort(notes, order);

    return notes;
}

TickMap::TickMap(const Sequence& shown, const Sequence& played)
    : shown_{std::max(shown.timebase, 1), shown.tempo}, played_{std::max(played.timebase, 1), played.tempo}
{
    // The conversion's quarter notes play at the arrangement's times: at each change of either tempo and at each end,
    // the time of a quarter note on one maps to the same quarter note on the other. Between them, the gap changes
    // evenly.
    const double reach = played_.timebase / 8.0;
    if (KeepsQuarters(shown, played, reach))
    {
        return;
    }

    // The points that the map passes through for certain: the start, and the starts and the ends of the two loops. The
    // conversion's loop counts in its first pass.
    const std::vector<Point> fixed = FixedPoints(shown, played);

    // Each note of the arrangement, and the conversion's note with its key that starts nearest its time.
    const std::vector<Point> notes = NotePoints(shown, played, reach);

    // The points in the order of both ticks. A note's point falls between the fixed points around it on both sides.
    points_ = {fixed.front()};
    std::size_t next = 1;
    for (const Point& note : notes)
    {
        while (next < fixed.size() && fixed[next].shown <= note.shown)
        {
            points_.push_back(fixed[next]);
            ++next;
        }

        const bool before_next =
            next == fixed.size() || (note.shown < fixed[next].shown && note.played < fixed[next].played);
        if (before_next && note.shown > points_.back().shown && note.played > points_.back().played)
        {
            points_.push_back(note);
        }
    }

    points_.insert(points_.end(), fixed.begin() + static_cast<std::ptrdiff_t>(next), fixed.end());
}

double TickMap::Through(const Timing& from, const Timing& to, const std::vector<Point>& points, bool shown, double tick)
{
    if (points.empty() || tick <= 0.0)
    {
        return tick * to.timebase / from.timebase;
    }

    // The last point at or before the tick, and the next.
    const auto tick_of = [shown](const Point& point)
    {
        return shown ? point.shown : point.played;
    };
    const auto other_of = [shown](const Point& point)
    {
        return shown ? point.played : point.shown;
    };
    const auto next = std::ranges::upper_bound(points, tick, std::less<>(), tick_of);
    const Point& last = *std::prev(next);
    if (tick == tick_of(last))
    {
        return other_of(last);
    }

    // The same share of the time to the next point, or the same time after the last.
    const double last_ms = MsAt(from.timebase, from.tempo, tick_of(last));
    const double other_ms = MsAt(to.timebase, to.tempo, other_of(last));
    const double ms = MsAt(from.timebase, from.tempo, tick) - last_ms;
    if (next == points.end())
    {
        return TickAt(to.timebase, to.tempo, other_ms + ms);
    }

    const double span = MsAt(from.timebase, from.tempo, tick_of(*next)) - last_ms;
    const double other_span = MsAt(to.timebase, to.tempo, other_of(*next)) - other_ms;
    const double share = span > 0.0 ? ms / span : 0.0;

    return std::clamp(TickAt(to.timebase, to.tempo, other_ms + share * other_span), other_of(last), other_of(*next));
}

double TickMap::Played(double tick) const
{
    return Through(shown_, played_, points_, true, tick);
}

double TickMap::Shown(double tick) const
{
    return Through(played_, shown_, points_, false, tick);
}

bool TickMap::ByTime() const
{
    return !points_.empty();
}

double PlayedTickOf(const Sequence& shown, const Sequence& played, double tick)
{
    return played.arrangement_ticks ? played.arrangement_ticks->Played(tick)
                                    : tick * played.timebase / std::max(shown.timebase, 1);
}

double ShownTickOf(const Sequence& shown, const Sequence& played, double tick)
{
    return played.arrangement_ticks ? played.arrangement_ticks->Shown(tick)
                                    : tick * shown.timebase / std::max(played.timebase, 1);
}

double CountedTicksOn(const Sequence& shown, const Sequence& from, const Sequence& to, double counted)
{
    // The passes of `from`'s loop that the count has finished, and the position in the next.
    double passes = 0.0;
    double position = counted;
    if (from.loop && from.loop->start < from.loop->end && counted >= from.loop->end)
    {
        const double length = from.loop->end - from.loop->start;
        passes = std::floor((counted - from.loop->end) / length) + 1.0;
        position = counted - passes * length;
    }

    const double mapped = PlayedTickOf(shown, to, ShownTickOf(shown, from, position));
    const bool loops = to.loop && to.loop->start < to.loop->end;
    return loops ? mapped + passes * (to.loop->end - to.loop->start) : mapped;
}

uint32_t PositionOfTicks(const Sequence& sequence, uint64_t ticks)
{
    const std::optional<Loop>& loop = sequence.loop;
    if (!loop || ticks < loop->end || loop->end <= loop->start)
    {
        return static_cast<uint32_t>(std::min<uint64_t>(ticks, std::numeric_limits<uint32_t>::max()));
    }

    return loop->start + static_cast<uint32_t>((ticks - loop->end) % (loop->end - loop->start));
}

uint64_t ChangeFrame(uint32_t time, FrameMs frame_ms)
{
    // The time in frames, rounded up. A time and a denominator below 2^32 and 2^25 can't overflow.
    return (static_cast<uint64_t>(time) * frame_ms.denominator + frame_ms.numerator - 1) / frame_ms.numerator;
}

uint64_t FramesIn(uint64_t ms, FrameMs frame_ms)
{
    return ms * frame_ms.denominator / frame_ms.numerator;
}

} // namespace hokuto::song
