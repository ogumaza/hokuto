// SPDX-License-Identifier: MIT

// The comparisons of hokuto_check_mix: the notes and the settings of two reads of a sound.

#include "check_mix.h"

namespace hokuto::tools::mix
{

std::vector<Played> NotesOf(const Sequence& sequence, const NotesApart& apart)
{
    std::vector<Played> notes;
    for (const Track& track : sequence.tracks)
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

bool SamePlaying(const Sequence& a, const Sequence& b)
{
    if (NotesOf(a) != NotesOf(b) || a.loop != b.loop || a.length != b.length || a.tracks.size() != b.tracks.size())
    {
        return false;
    }

    for (std::size_t i = 0; i < a.tracks.size(); ++i)
    {
        for (const Mix setting : kSettings)
        {
            if (a.tracks[i].number != b.tracks[i].number ||
                hokuto::song::MixSettings(a.tracks[i], setting) != hokuto::song::MixSettings(b.tracks[i], setting))
            {
                return false;
            }
        }
    }

    return true;
}

std::vector<Setting> AgainstDseStart(std::vector<Setting> settings)
{
    if (!settings.empty() && settings.front().tick == 0 && settings.front().value == hokuto::song::kDseDefaultVolume)
    {
        settings.erase(settings.begin());
    }
    else if (settings.empty() || settings.front().tick > 0)
    {
        settings.insert(settings.begin(), {0, hokuto::song::MixDefault(Mix::kVolume)});
    }

    return settings;
}

std::string Text(const std::vector<Setting>& settings)
{
    std::string text;
    for (std::size_t i = 0; i < settings.size() && i < 6; ++i)
    {
        text += (i == 0 ? "" : ", ") + std::to_string(settings[i].value) + " at " + std::to_string(settings[i].tick);
    }

    return settings.empty() ? "none" : settings.size() > 6 ? text + ", ..." : text;
}

namespace
{

// On the DS, whether the driver's channels decide `note`, a note of `read` that is missing from `other`: `other` lists
// it among the notes that the driver never started (song::Track::unstarted), or plays it with another length where
// either read cuts it short. A note whose length runs out in the tick where a later note claims its channel ends in
// that tick. Otherwise its release comes at the end of the frame, after the frame's ticks (ntdlib's performer). Its
// length can then differ by up to the ticks of a frame: the tempo over 240, rounded up. A note of a chord can also take
// the length of another of the chord's notes: the notes of the track that start at its tick hold the same keys and
// velocities in both reads, and the same lengths in another order.
bool ChannelsDecide(const Sequence& read, const Sequence& other, const Played& note)
{
    const auto& [number, start, key, length, velocity] = note;
    const Track* track = TrackNumbered(read, number);
    const Track* other_track = TrackNumbered(other, number);
    if (track == nullptr || other_track == nullptr)
    {
        return false;
    }

    const auto here = [&](const hokuto::song::Note& n)
    {
        return n.start == start && n.key == key;
    };
    if (std::ranges::any_of(other_track->unstarted, here))
    {
        return true;
    }
    const bool cut = std::ranges::any_of(
        track->notes, [&](const hokuto::song::Note& n) { return here(n) && n.length == length && n.cut; });
    if (std::ranges::any_of(other_track->notes, [&](const hokuto::song::Note& n)
                            { return here(n) && n.velocity == velocity && n.length != length && (cut || n.cut); }))
    {
        return true;
    }
    double fastest = 0.0;
    for (const hokuto::song::Tempo& tempo : read.tempo)
    {
        fastest = std::max(fastest, tempo.bpm);
    }
    const auto frame_ticks = static_cast<uint32_t>(std::max(1.0, std::ceil(fastest / 240.0)));
    if (std::ranges::any_of(other_track->notes,
                            [&](const hokuto::song::Note& n)
                            {
                                return here(n) && n.velocity == velocity &&
                                       std::max(n.length, length) - std::min(n.length, length) <= frame_ticks;
                            }))
    {
        return true;
    }

    const auto chord = [start](const Track& of)
    {
        std::pair<std::vector<std::pair<uint32_t, uint32_t>>, std::vector<uint32_t>> notes;
        for (const hokuto::song::Note& n : of.notes)
        {
            if (n.start == start)
            {
                notes.first.emplace_back(n.key, n.velocity);
                notes.second.push_back(n.length);
            }
        }
        std::ranges::sort(notes.first);
        std::ranges::sort(notes.second);
        return notes;
    };
    const auto notes = chord(*track);
    return notes.first.size() > 1 && notes == chord(*other_track);
}

} // namespace

bool ChannelsDecideAll(const Sequence& a, const Sequence& b)
{
    const std::vector<Played> notes_a = NotesOf(a);
    const std::vector<Played> notes_b = NotesOf(b);
    std::vector<Played> only_a;
    std::vector<Played> only_b;
    std::ranges::set_difference(notes_a, notes_b, std::back_inserter(only_a));
    std::ranges::set_difference(notes_b, notes_a, std::back_inserter(only_b));
    return std::ranges::all_of(only_a, [&](const Played& note) { return ChannelsDecide(a, b, note); }) &&
           std::ranges::all_of(only_b, [&](const Played& note) { return ChannelsDecide(b, a, note); });
}

void PrintNotesOnlyIn(const std::string& name, const Sequence& before, const Sequence& after)
{
    const std::vector<Played> notes_before = NotesOf(before);
    const std::vector<Played> notes_after = NotesOf(after);
    std::vector<Played> only_before;
    std::vector<Played> only_after;
    std::ranges::set_difference(notes_before, notes_after, std::back_inserter(only_before));
    std::ranges::set_difference(notes_after, notes_before, std::back_inserter(only_after));
    const auto print = [&](const std::vector<Played>& notes, const char* when)
    {
        for (std::size_t i = 0; i < std::min<std::size_t>(notes.size(), 4); ++i)
        {
            const auto& [track, start, key, length, velocity] = notes[i];
            std::printf("%s:   %s: track %d, tick %u, key %u, length %u, velocity %u\n", name.c_str(), when, track,
                        start, key, length, velocity);
        }
        if (notes.size() > 4)
        {
            std::printf("%s:   %s: %zu more\n", name.c_str(), when, notes.size() - 4);
        }
    };
    print(only_before, "only before");
    print(only_after, "only after");
}

std::set<int> ChangedTracks(const Sequence& before, const Sequence& after, Mix setting)
{
    std::set<int> changed;
    for (const Track& old : before.tracks)
    {
        const Track* track = TrackNumbered(after, old.number);
        if (track == nullptr || hokuto::song::MixSettings(*track, setting) != hokuto::song::MixSettings(old, setting))
        {
            changed.insert(old.number);
        }
    }

    return changed;
}

std::vector<Setting> Before(std::vector<Setting> settings, uint32_t end)
{
    std::erase_if(settings, [end](const Setting& change) { return change.tick >= end; });

    return settings;
}

Sequence Before(Sequence sequence, uint32_t end)
{
    for (Track& track : sequence.tracks)
    {
        std::erase_if(track.notes, [end](const hokuto::song::Note& note) { return note.start >= end; });
        for (hokuto::song::Note& note : track.notes)
        {
            note.length = std::min(note.length, end - note.start);
        }
        track.volume = Before(std::move(track.volume), end);
        track.pan = Before(std::move(track.pan), end);
        for (std::vector<Setting>& send : track.sends)
        {
            send = Before(std::move(send), end);
        }
    }

    return sequence;
}

std::size_t NotesFrom(const Sequence& sequence, uint32_t end)
{
    const auto late = [end](const hokuto::song::Note& note)
    {
        return note.start >= end;
    };
    std::size_t count = 0;
    for (const Track& track : sequence.tracks)
    {
        count += static_cast<std::size_t>(std::count_if(track.notes.begin(), track.notes.end(), late));
    }

    return count;
}

std::vector<const Track*> Playing(const Sequence& sequence)
{
    std::vector<const Track*> tracks;
    for (const Track& track : sequence.tracks)
    {
        if (!track.notes.empty())
        {
            tracks.push_back(&track);
        }
    }

    return tracks;
}

} // namespace hokuto::tools::mix
