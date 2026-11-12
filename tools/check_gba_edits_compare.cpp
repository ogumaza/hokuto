// SPDX-License-Identifier: MIT

// The comparison of each song before and after the edits, for hokuto_check_gba_edits. check_gba_edits.cpp describes
// the checks.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "ntdlib/agb/gba.h"
#include "ntdlib/mp2k/sequencer.h"

#include "check_gba_edits.h"
#include "song/sequence.h"

namespace hokuto::tools::gba_edits
{

namespace
{

// A note that the edited song should play: the note before the edits, with the edit of its command.
struct Expected
{
    Note note;
    Change change = Change::kVelocity;
    uint32_t end = 0;        // where the note ended before the edits
    bool own_length = false; // the note's length came from its command before the edits
    bool contested = false;  // a note whose channel other notes may take or keep
    bool brief = false;      // a note that can start and end in one of the driver's frames
    bool quiet = false;      // a quiet note of a PSG channel
};

// The level, velocity times volume, up to which a PSG note counts as quiet: half the full level.
constexpr int kQuietLevel = 64 * 127;

// The time that a note of a PSG channel takes, before and after its edit.
struct Span
{
    std::size_t note = 0; // the note's place in the song's notes
    uint32_t start = 0;
    uint32_t end = 0;
};

// The time that `note` takes before and after the edit of `plan`, up to the length of its command at least. Another
// note can have cut it before the edit.
Span SpanOf(std::size_t place, const Note& note, const Plan& plan)
{
    const int64_t shift = plan.change == Change::kMove ? plan.edit.shift : 0;
    const int64_t longer = plan.change == Change::kLonger ? 1 : 0;
    const int64_t end = note.start + std::max(note.length, plan.ticks);
    return {place, static_cast<uint32_t>(std::min<int64_t>(note.start, note.start + shift)),
            static_cast<uint32_t>(std::max(end + longer, end + shift))};
}

// The times of the notes on each channel: the DirectSound channels, 0, and the PSG channels, 1 to 4.
struct ChannelSpans
{
    // Whether the other notes of `channel` that sound at once in `time` take every channel of its kind at some tick.
    bool Contested(int channel, const Span& time) const
    {
        const std::vector<Span> none;
        const auto found = of.find(channel);
        const std::vector<Span>& spans = found != of.end() ? found->second : none;
        std::vector<std::pair<uint32_t, int>> changes;
        for (const Span& span : spans)
        {
            if (span.note != time.note && span.start <= time.end && time.start <= span.end)
            {
                changes.emplace_back(std::max(span.start, time.start), 1);
                changes.emplace_back(std::min(span.end, time.end) + 1, -1);
            }
        }
        std::sort(changes.begin(), changes.end());
        int at_once = 0;
        int most = 0;
        for (const auto& [tick, change] : changes)
        {
            at_once += change;
            most = std::max(most, at_once);
        }

        return most >= (channel == 0 ? direct_channels : 1);
    }

    std::map<int, std::vector<Span>> of;
    int direct_channels = 0;
};

// Whether a note of `song` at `tick` that lasts `length` ticks never sounds.
// A frame of the driver runs more than one tick above 150 of its units of tempo. A note shorter than a frame's
// ticks then starts and ends in one frame at some ticks, and never sounds.
bool Brief(const Sequence& song, uint32_t tick, uint32_t length)
{
    const double ticks =
        hokuto::song::TempoAt(song, tick) * 60.0 / ntdlib::agb::kFrameRate / ntdlib::mp2k::kTickCounter;
    return length > 0 && length < std::ceil(ticks - 1e-6);
}

// Whether `note` of `track`, at `velocity`, is a quiet note of a PSG channel.
// A PSG channel's envelope moves in 16 steps. A quiet note there can end in its first frames. The state that the
// note before it leaves, and the frame where it starts, then decide whether it sounds.
bool Quiet(const ChannelOf& channel_of, const hokuto::song::Track& track, const Note& note, int velocity)
{
    return channel_of(note) != 0 &&
           velocity * hokuto::song::MixAt(track, hokuto::song::Mix::kVolume, note.start) <= kQuietLevel;
}

// Where the comparison of `before` and `after` ends: the earlier loop's end where a loop moved, or nothing. Prints a
// line when the loop moved.
std::optional<uint32_t> LoopEnd(const std::string& name, const Sequence& before, const Sequence& after, Totals& totals)
{
    std::optional<uint32_t> end;
    if (before.loop != after.loop)
    {
        std::printf("%s: the loop moved\n", name.c_str());
        totals.loops++;
    }
    if (before.loop && after.loop)
    {
        end = std::min(before.loop->end, after.loop->end);
    }
    else if (before.loop || after.loop)
    {
        end = before.loop ? before.loop->end : after.loop->end;
    }

    return end;
}

// A note's place: its track, start and key.
using NotePlace = std::tuple<int, uint32_t, int>;

// The notes that a song plays after its edits, by their places, with the places of the notes that a move or a removal
// parts from another note of the same place, and of the notes that the driver never started before the edits.
struct ExpectedNotes
{
    std::multimap<NotePlace, Expected> expected;
    std::set<NotePlace> parted;
    std::set<NotePlace> unstarted_before;
};

// The notes by track, start and key. Two commands of one key on a track's tick play as one note, and the note leads
// to one of them. A move or a removal of that command parts the two, and the other then plays alone at the old
// place. A note that the driver never started before can likewise play as one with a note that a move puts on its
// tick.
ExpectedNotes ExpectedNotesOf(const Sequence& before, const std::map<uint32_t, Plan>& plans, const ChannelSpans& spans,
                              const ChannelOf& channel_of, const std::optional<uint32_t>& end, Totals& totals)
{
    ExpectedNotes notes;
    std::size_t place = 0;
    for (const hokuto::song::Track& track : before.tracks)
    {
        for (const Note& note : track.unstarted)
        {
            notes.unstarted_before.insert({track.number, note.start, note.key});
        }
        for (const Note& note : track.notes)
        {
            totals.notes++;
            const Plan& plan = plans.at(note.command);
            if (plan.change == Change::kMove || plan.change == Change::kRemove)
            {
                notes.parted.insert({track.number, note.start, note.key});
            }
            Expected want{note, plan.change, note.start + note.length, note.length == plan.ticks};
            want.contested = spans.Contested(channel_of(note), SpanOf(place++, note, plan));
            want.brief = Brief(before, note.start, plan.ticks);
            want.quiet = Quiet(channel_of, track, note, note.velocity - (plan.change == Change::kVelocity ? 1 : 0));
            switch (plan.change)
            {
            case Change::kRemove:
                continue;
            case Change::kMove:
                want.note.start = static_cast<uint32_t>(static_cast<int64_t>(note.start) + plan.edit.shift);
                break;
            case Change::kLonger:
                want.note.length = want.own_length ? *plan.edit.length : note.length;
                break;
            case Change::kVelocity:
                want.note.velocity =
                    note.velocity == plan.velocity ? static_cast<uint8_t>(*plan.edit.velocity) : note.velocity;
                break;
            }
            want.brief = want.brief || Brief(before, want.note.start,
                                             plan.change == Change::kLonger ? *plan.edit.length : plan.ticks);
            if (end && want.note.start >= *end)
            {
                totals.past_loop += plan.change == Change::kMove && note.start < *end ? 1 : 0;
                continue;
            }
            notes.expected.emplace(NotePlace{track.number, want.note.start, want.note.key}, want);
        }
    }

    return notes;
}

// The places of the notes of `song` that the driver never started.
std::set<NotePlace> UnstartedPlaces(const Sequence& song)
{
    std::set<NotePlace> places;
    for (const hokuto::song::Track& track : song.tracks)
    {
        for (const Note& note : track.unstarted)
        {
            places.insert({track.number, note.start, note.key});
        }
    }

    return places;
}

// Reports each note of `expected` that the song no longer plays through `report`. A note whose channel other notes may
// take, or one at a place of `unstarted_after`, counts apart, and so do brief and quiet notes.
void ReportMissing(const std::multimap<NotePlace, Expected>& expected, const std::set<NotePlace>& unstarted_after,
                   const std::function<void(const std::string&)>& report, Totals& totals)
{
    for (const auto& [key, want] : expected)
    {
        if (want.contested || unstarted_after.contains(key))
        {
            totals.contested++;
            continue;
        }
        if (want.brief)
        {
            totals.brief++;
            continue;
        }
        if (want.quiet)
        {
            totals.quiet++;
            continue;
        }
        report("track " + std::to_string(std::get<0>(key)) + " is missing its note at tick " +
               std::to_string(want.note.start) + ", key " + std::to_string(want.note.key));
    }
}

} // namespace

bool Compare(const std::string& name, const Sequence& before, const Sequence& after,
             const std::map<uint32_t, Plan>& plans, const ChannelOf& channel_of, int direct_channels, Totals& totals)
{
    // The time of each note on each channel: the DirectSound channels, 0, and the PSG channels, 1 to 4. A removed
    // note's time counts too.
    ChannelSpans spans;
    spans.direct_channels = direct_channels;
    std::size_t place = 0;
    for (const hokuto::song::Track& track : before.tracks)
    {
        for (const Note& note : track.notes)
        {
            spans.of[channel_of(note)].push_back(SpanOf(place++, note, plans.at(note.command)));
        }
    }

    // A loop that moved leaves only the notes before the earlier loop's end.
    const std::optional<uint32_t> end = LoopEnd(name, before, after, totals);

    // The notes by track, start and key.
    using Place = NotePlace;
    auto [expected, parted, unstarted_before] = ExpectedNotesOf(before, plans, spans, channel_of, end, totals);

    // A move can put a note on the tick of another note of the same key on its track. The two then play as one.
    std::set<Place> together;
    for (auto at = expected.begin(); at != expected.end(); at = expected.upper_bound(at->first))
    {
        if (expected.count(at->first) > 1)
        {
            together.insert(at->first);
            totals.together += expected.count(at->first);
        }
    }
    std::erase_if(expected, [&together](const auto& entry) { return together.contains(entry.first); });

    std::size_t wrong = 0;
    const auto report = [&](const std::string& what)
    {
        if (wrong++ < 5)
        {
            std::printf("%s: %s\n", name.c_str(), what.c_str());
        }
    };
    // The notes that the driver never starts after the edits: every channel of their kind was taken.
    const std::set<Place> unstarted_after = UnstartedPlaces(after);

    // A moved loop cuts the notes at the earlier loop's end.
    const auto clipped = [&end](uint32_t start, uint32_t length)
    {
        return end ? std::min(length, *end - std::min(start, *end)) : length;
    };
    for (const hokuto::song::Track& track : after.tracks)
    {
        for (const Note& note : track.notes)
        {
            const Place key{track.number, note.start, note.key};
            if ((end && note.start >= *end) || together.contains(key))
            {
                continue;
            }
            const auto found = expected.find(key);
            if (found == expected.end())
            {
                if (parted.contains(key))
                {
                    totals.together++;
                    continue;
                }
                // A note that didn't sound before the edits comes from a command whose note sounded elsewhere. Its edit
                // can have moved it a tick, or made it a tick longer.
                if (spans.Contested(channel_of(note),
                                    {SIZE_MAX, note.start == 0 ? 0 : note.start - 1, note.start + note.length + 1}))
                {
                    totals.contested++;
                    continue;
                }
                if (Brief(before, note.start, note.length) || Brief(before, note.start, note.length - 1))
                {
                    totals.brief++;
                    continue;
                }
                if (Quiet(channel_of, track, note, note.velocity))
                {
                    totals.quiet++;
                    continue;
                }
                report("track " + std::to_string(track.number) + " plays a note at tick " + std::to_string(note.start) +
                       ", key " + std::to_string(note.key) + ", that it shouldn't");
                continue;
            }

            // A note can end sooner than its command says: its track or the song ends, another note cuts it, or its
            // channel falls silent. The length of a note that ended so before the edits isn't compared. A moved note
            // can end where it ended before, and a longer one at its old length or later, where its sample ends. A
            // quieter note's channel can fall silent sooner.
            const Expected& want = found->second;
            const uint32_t played = clipped(note.start, note.length);
            const uint32_t wanted = clipped(want.note.start, want.note.length);
            const uint32_t ended = end ? std::min(want.end, *end) : want.end;
            const bool sooner = played < wanted;
            const bool apart =
                !want.own_length || (sooner && ((want.change == Change::kMove && note.start + played == ended) ||
                                                (want.change == Change::kLonger && note.start + played >= ended) ||
                                                want.change == Change::kVelocity));
            const bool length = !apart && played != wanted;
            const bool velocity = note.velocity != want.note.velocity;
            if (apart && played != wanted)
            {
                totals.lengths++;
            }

            // A note that the driver never started before plays as one with the note on its tick, louder.
            if ((length || velocity) && unstarted_before.contains(key) && !(note.velocity < want.note.velocity))
            {
                totals.together++;
                expected.erase(found);
                continue;
            }

            // A note whose channel other notes may take can end sooner. A note that the channels now let through can
            // also play on its tick with the same key, and the two then play as one.
            if ((length || velocity) && want.contested && !(played > wanted) && !(note.velocity < want.note.velocity))
            {
                totals.contested++;
                expected.erase(found);
                continue;
            }
            if (length || velocity)
            {
                report("track " + std::to_string(track.number) + "'s note at tick " + std::to_string(note.start) +
                       ", key " + std::to_string(note.key) + ", plays length " + std::to_string(played) +
                       " and velocity " + std::to_string(note.velocity) + " in place of " + std::to_string(wanted) +
                       " and " + std::to_string(want.note.velocity));
            }
            expected.erase(found);
        }
    }
    ReportMissing(expected, unstarted_after, report, totals);

    return wrong > 0;
}

} // namespace hokuto::tools::gba_edits
