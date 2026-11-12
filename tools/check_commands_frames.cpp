// SPDX-License-Identifier: MIT

// The checks of hokuto_check_commands that decide which notes the frames of a driver move: Rare's, Quintet's and
// Konami's drivers.

#include "check_commands.h"

namespace hokuto::tools::commands
{

namespace
{

// A note as a read plays it: its track, start, key and velocity.
using Heard = std::tuple<int, uint32_t, uint8_t, uint8_t>;

// Whether a note of the same key, on track `other_track` at tick `other_start` with `other_velocity`, partners a note
// on track `track` at tick `start` with `velocity`, where a frame runs `frame` ticks. On the same track, the partner
// starts in the same tick with another velocity, a tick away, or up to a frame before. On another track, it starts up
// to a frame before.
bool Partners(int track, uint32_t start, int velocity, int other_track, uint32_t other_start, int other_velocity,
              uint32_t frame)
{
    if (other_track == track)
    {
        return (other_start == start && other_velocity != velocity) || other_start == start + 1 ||
               other_start + 1 == start || (other_start < start && other_start + frame >= start);
    }

    return other_start <= start && other_start + frame >= start;
}

// The notes of `song` up to the end of its loop, and then the notes of the last loop of each track before there, again
// and again up to `end`. Each track repeats `loops`, or else the song's loop of `period` ticks. `apart` sets the notes
// apart as NotesOf does.
std::vector<Heard> NotesUpToLoopEnd(const hokuto::song::Sequence& song, const TrackLoops* loops, uint32_t end,
                                    uint32_t period, const hokuto::tools::NotesApart& apart)
{
    std::map<int, std::multiset<uint8_t>> first_pass;
    if (loops)
    {
        for (const auto& [number, loop] : *loops)
        {
            first_pass[number] = loop.before;
        }
    }
    std::vector<Heard> notes;
    for (const auto& [track, start, key, length, velocity] : NotesOf(song, apart))
    {
        if (start >= song.loop->end)
        {
            continue;
        }
        notes.emplace_back(track, start, key, velocity);

        const auto loop = loops ? loops->find(track) : TrackLoops::const_iterator();
        if (loops && loop == loops->end())
        {
            continue;
        }
        const uint32_t pass = loops ? loop->second.length : period;
        if (start + pass < song.loop->end)
        {
            continue;
        }
        if (loops && start == loop->second.start)
        {
            std::multiset<uint8_t>& keys = first_pass[track];
            if (const auto first = keys.find(key); first != keys.end())
            {
                keys.erase(first);
                continue;
            }
        }
        for (uint32_t tick = start + pass; tick < end; tick += pass)
        {
            notes.emplace_back(track, tick, key, velocity);
        }
    }
    std::sort(notes.begin(), notes.end());

    return notes;
}

// The notes of `in_before` and `in_after` that only one of them contains, when each has a partner within `frame` ticks
// (FramesDecide), or nothing when one has none.
std::optional<std::vector<Heard>> AllPartnered(const std::vector<Heard>& in_before, const std::vector<Heard>& in_after,
                                               uint32_t frame)
{
    std::vector<Heard> differ;
    std::ranges::set_symmetric_difference(in_before, in_after, std::back_inserter(differ));
    std::map<uint8_t, std::vector<Heard>> by_key;
    for (const std::vector<Heard>* notes : {&in_before, &in_after})
    {
        for (const Heard& note : *notes)
        {
            by_key[std::get<2>(note)].push_back(note);
        }
    }
    const auto partnered = [&](const Heard& note)
    {
        const auto& [track, start, key, velocity] = note;
        const auto partner = [&](const Heard& other)
        {
            const auto& [other_track, other_start, other_key, other_velocity] = other;
            return Partners(track, start, velocity, other_track, other_start, other_velocity, frame);
        };
        return std::ranges::any_of(by_key.at(key), partner);
    };
    if (!std::ranges::all_of(differ, partnered))
    {
        return std::nullopt;
    }

    return differ;
}

} // namespace

bool SameOverLoops(const hokuto::song::Sequence& before, const hokuto::song::Sequence& after,
                   const hokuto::tools::NotesApart& apart, const RareRepeats* rare, std::size_t* alone)
{
    if (!before.loop || !after.loop)
    {
        return false;
    }

    const uint32_t period = before.loop->end - before.loop->start;
    if (period == 0 || after.loop->end - after.loop->start != period)
    {
        return false;
    }

    // The notes of each read up to the end of its loop, and then the notes of the last loop of each track before there,
    // again and again. Without `rare`, every track repeats the song's loop.
    const uint32_t end = std::max(before.loop->end, after.loop->end);
    const std::vector<Heard> in_before = NotesUpToLoopEnd(before, rare ? &rare->before : nullptr, end, period, apart);
    const std::vector<Heard> in_after = NotesUpToLoopEnd(after, rare ? &rare->after : nullptr, end, period, apart);
    if (in_before == in_after)
    {
        return true;
    }
    if (!rare)
    {
        return false;
    }

    // FramesDecide's partners: a note of the same track and key at the same tick with another velocity, a tick away or
    // a frame before, or a note of the same key on another track that starts a frame before or less.
    const std::optional<std::vector<Heard>> differ = AllPartnered(in_before, in_after, rare->frame);
    if (!differ)
    {
        return false;
    }
    if (alone)
    {
        *alone += differ->size();
    }

    return true;
}

namespace
{

// The keys of the notes that the track at `address` of `rom`, a ROM of Rare's driver, starts at the tick of the start
// of its loop, before the loop starts. ntdlib's rare::ScanTrack uses the last loop start before the loop's end.
std::multiset<uint8_t> RareKeysBeforeLoop(const hokuto::song::RareRom& rom, uint32_t address)
{
    namespace rare = ntdlib::rare;
    std::multiset<uint8_t> at_tick;
    std::multiset<uint8_t> before;
    for (int n = 0; n < rare::kMaxTrackCommands; ++n)
    {
        rare::Event event;
        if (!rare::DecodeEvent(rom.rom, address, rom.info.format, event) || event.command == rare::kCmdEnd)
        {
            break;
        }
        address += event.size;
        if (rare::IsDelay(event.command) && event.value != 0)
        {
            at_tick.clear();
        }
        else if (event.command == rare::kCmdNoteOn || event.command == rare::kCmdNoteOnB)
        {
            at_tick.insert(event.a);
        }
        else if (event.command == rare::kCmdControl && event.a == rare::kCtrlLoopStart)
        {
            before = at_tick;
        }
        else if (event.command == rare::kCmdControl && event.a == rare::kCtrlLoopEnd)
        {
            break;
        }
    }

    return before;
}

// The loops of the tracks of the tune that sound `sound` of `archive` plays, a ROM of Rare's driver, in the driver's
// ticks (ntdlib's rare::ScanTrack).
TrackLoops RareTrackLoops(const CheckedArchive& archive, uint32_t sound)
{
    TrackLoops loops;
    const hokuto::song::RareRom& rom = *archive.Rare();
    ntdlib::rare::TuneHeader header;
    if (!ntdlib::rare::ReadTuneHeader(rom.rom, ntdlib::rare::TuneAddress(rom.rom, rom.info, rom.SongOf(sound)), header))
    {
        return loops;
    }
    for (std::size_t track = 0; track < header.tracks.size(); ++track)
    {
        const ntdlib::agb::TrackLayout layout = ntdlib::rare::ScanTrack(rom.rom, header.tracks[track], rom.info.format);
        if (layout.loops && layout.loop_end > layout.loop_start)
        {
            loops[static_cast<int>(track)] = {static_cast<uint32_t>(layout.loop_start),
                                              static_cast<uint32_t>(layout.loop_end - layout.loop_start),
                                              RareKeysBeforeLoop(rom, header.tracks[track])};
        }
    }

    return loops;
}

} // namespace

std::optional<RareRepeats> RareRepeatsOf(const CheckedArchive& archive, const CheckedArchive& edited, uint32_t sound,
                                         const hokuto::song::Sequence& before, const hokuto::song::Sequence& after)
{
    if (!archive.Rare() || !edited.Rare())
    {
        return std::nullopt;
    }

    return RareRepeats{RareTrackLoops(archive, sound), RareTrackLoops(edited, sound),
                       std::max(RareFrameTicks(before), RareFrameTicks(after))};
}

bool FramesDecide(const hokuto::song::Sequence& before, const hokuto::song::Sequence& after, bool& moved,
                  std::size_t& alone)
{
    using Heard = std::tuple<int, uint32_t, int, int>;
    std::multiset<Heard> in_before;
    std::multiset<Heard> in_after;
    std::map<int, std::vector<Heard>> by_key;
    for (const auto& [song, heard] : {std::pair(&before, &in_before), std::pair(&after, &in_after)})
    {
        for (const hokuto::song::Track& track : song->tracks)
        {
            for (const hokuto::song::Note& note : track.notes)
            {
                const Heard key{track.number, note.start, note.key, note.velocity};
                heard->insert(key);
                by_key[note.key].push_back(key);
            }
        }
    }

    const uint32_t frame = std::max(RareFrameTicks(before), RareFrameTicks(after));
    std::size_t count = 0;
    const hokuto::tools::NotesApart frames = [&](const hokuto::song::Track& track, const hokuto::song::Note& note)
    {
        const Heard key{track.number, note.start, note.key, note.velocity};
        if (in_before.count(key) == in_after.count(key))
        {
            return false;
        }

        const auto partner = [&](const Heard& other)
        {
            const auto& [number, start, other_key, velocity] = other;
            return Partners(track.number, note.start, note.velocity, number, start, velocity, frame);
        };
        const auto others = by_key.find(note.key);
        const bool apart = others != by_key.end() && std::ranges::any_of(others->second, partner);
        count += apart ? 1 : 0;
        return apart;
    };
    const bool same = SamePlaying(before, after, moved, 0, frames);
    alone += same ? count : 0;
    return same;
}

namespace
{

// The most of the performer's ticks that a frame of Quintet's driver runs in `song`. A tempo T runs T of them a frame
// (TempoRulesOf), and the GBA runs kFrameRate frames a second.
uint32_t QuintetFrameTicks(const hokuto::song::Sequence& song)
{
    double fastest = 0.0;
    for (const hokuto::song::Tempo& tempo : song.tempo)
    {
        fastest = std::max(fastest, tempo.bpm);
    }

    return static_cast<uint32_t>(std::max(1.0, std::ceil(song.timebase * fastest / 60.0 / ntdlib::agb::kFrameRate)));
}

} // namespace

std::map<int, std::vector<uint32_t>> QuintetResets(const File& file, uint32_t sequence,
                                                   const hokuto::song::Sequence& song)
{
    const ntdlib::commands::Sequence& commands = *file.sequences.at(sequence);
    const std::vector<uint32_t> offsets = hokuto::edit::Offsets(commands, hokuto::edit::ConsoleOf(file));
    std::map<int, std::vector<uint32_t>> own;
    for (const hokuto::song::Track& track : song.tracks)
    {
        std::vector<uint32_t>& ticks = own[track.number];
        for (const hokuto::song::Stretch& stretch : track.stretches)
        {
            const std::optional<uint32_t> first = hokuto::edit::ItemAtOffset(offsets, stretch.first);
            const std::optional<uint32_t> last = hokuto::edit::ItemAtOffset(offsets, stretch.last);
            for (uint32_t item = first.value_or(1); first && last && item <= *last; ++item)
            {
                if (commands.items.at(item).kind == Kind::kTempo)
                {
                    ticks.push_back(static_cast<uint32_t>(stretch.tick));
                }
            }
        }
    }

    std::map<int, std::vector<uint32_t>> resets;
    for (const hokuto::song::Track& track : song.tracks)
    {
        std::vector<uint32_t> ticks = own[track.number];
        if (track.number != ntdlib::quintet::kSquare1)
        {
            ticks.insert(ticks.end(), own[ntdlib::quintet::kSquare1].begin(), own[ntdlib::quintet::kSquare1].end());
        }
        std::ranges::sort(ticks);
        resets[track.number] = std::move(ticks);
    }

    return resets;
}

namespace
{

// A first note of a command in a stretch, with the count of the notes that the conversion split off it.
struct First
{
    const hokuto::song::Note* note = nullptr;
    std::size_t split = 0;
};

// The first note of each command of `track` in each stretch, in the order of their starts, with the notes that the
// conversion split off them.
std::vector<First> FirstNotes(const hokuto::song::Track& track)
{
    std::vector<const hokuto::song::Note*> notes;
    for (const hokuto::song::Note& note : track.notes)
    {
        notes.push_back(&note);
    }
    std::ranges::stable_sort(notes, {}, &hokuto::song::Note::start);
    std::vector<First> found;
    for (const hokuto::song::Note* note : notes)
    {
        if (!found.empty() && found.back().note->command == note->command &&
            found.back().note->stretch == note->stretch)
        {
            ++found.back().split;
            continue;
        }
        found.push_back({note, 0});
    }

    return found;
}

// Whether the first notes of `track` of the read before and of its track in `after` start the same commands, apart by
// up to the frames of `frame` ticks that `resets` allow, before `end`. Counts the notes apart in `counted`.
bool MatchTrack(const hokuto::song::Track& track, const hokuto::song::Sequence& after,
                const std::map<int, std::vector<uint32_t>>& resets, uint32_t frame, uint32_t end, QuintetApart& counted)
{
    const auto numbered = [&track](const hokuto::song::Track& other)
    {
        return other.number == track.number;
    };
    const auto other = std::ranges::find_if(after.tracks, numbered);
    const std::vector<First> from = FirstNotes(track);
    const std::vector<First> to = other == after.tracks.end() ? std::vector<First>() : FirstNotes(*other);
    const auto found = resets.find(track.number);
    const auto frames_at = [&](uint32_t tick)
    {
        const std::size_t count =
            found == resets.end()
                ? 0
                : static_cast<std::size_t>(std::ranges::upper_bound(found->second, tick) - found->second.begin());
        return frame * static_cast<uint32_t>(count + 1);
    };
    const auto near_end = [&](uint32_t tick)
    {
        return tick + frames_at(tick) + kQuintetLateFrames * frame >= end;
    };
    const auto lone = [frame](const First& first)
    {
        return first.split == 0 && first.note->length < kQuintetShortFrames * frame;
    };

    std::size_t i = 0;
    std::size_t j = 0;
    while (i < from.size() && j < to.size())
    {
        const hokuto::song::Note& a = *from[i].note;
        const hokuto::song::Note& b = *to[j].note;
        if (near_end(std::min(a.start, b.start)))
        {
            break;
        }

        // A short note of one read stands alone where the other read's note has another command.
        if (a.command != b.command && lone(from[i]))
        {
            ++counted.short_notes;
            ++i;
            continue;
        }
        if (a.command != b.command && lone(to[j]))
        {
            ++counted.short_notes;
            ++j;
            continue;
        }

        const uint32_t later = std::max(a.start, b.start);
        const uint32_t apart_by = later - std::min(a.start, b.start);
        const bool noise = track.number == ntdlib::quintet::kNoise;
        if (a.command != b.command || (!noise && a.key != b.key) ||
            apart_by > frames_at(later) + kQuintetLateFrames * frame)
        {
            return false;
        }

        counted.moved += apart_by != 0 ? 1 : 0;
        counted.resets += apart_by > frame && apart_by <= frames_at(later) ? 1 : 0;
        counted.late += apart_by > frames_at(later) ? 1 : 0;
        counted.drums += noise && a.key != b.key ? 1 : 0;
        counted.split += std::max(from[i].split, to[j].split) - std::min(from[i].split, to[j].split);
        ++i;
        ++j;
    }

    // A note past the other read's last can only start near the end.
    for (std::size_t k = i; k < from.size(); ++k)
    {
        if (!near_end(from[k].note->start))
        {
            return false;
        }
    }
    for (std::size_t k = j; k < to.size(); ++k)
    {
        if (!near_end(to[k].note->start))
        {
            return false;
        }
    }

    return true;
}

} // namespace

bool QuintetFramesDecide(const hokuto::song::Sequence& before, const hokuto::song::Sequence& after,
                         const std::map<int, std::vector<uint32_t>>& resets, QuintetApart& apart)
{
    const uint32_t frame = std::max(QuintetFrameTicks(before), QuintetFrameTicks(after));
    const auto end_of = [](const hokuto::song::Sequence& song)
    {
        return song.loop ? song.loop->end : song.length;
    };
    const uint32_t end = std::min(end_of(before), end_of(after));

    QuintetApart counted;
    for (const hokuto::song::Track& track : before.tracks)
    {
        if (!MatchTrack(track, after, resets, frame, end, counted))
        {
            return false;
        }
    }
    for (const hokuto::song::Track& track : after.tracks)
    {
        const auto numbered = [&track](const hokuto::song::Track& other)
        {
            return other.number == track.number;
        };
        if (std::ranges::none_of(before.tracks, numbered) && !track.notes.empty())
        {
            return false;
        }
    }

    apart.moved += counted.moved;
    apart.split += counted.split;
    apart.drums += counted.drums;
    apart.resets += counted.resets;
    apart.late += counted.late;
    apart.short_notes += counted.short_notes;
    return true;
}

namespace
{

// The volumes of a song of Konami's driver that follow a note in the note's tick, with only delays of 0 between them.
// In many of the driver's revisions, the trigger of a volume command replaces the start of the note in that frame, and
// the note doesn't start.
std::size_t KonamiVolumesAfterNotes(const ntdlib::commands::Sequence& commands)
{
    std::size_t volumes = 0;
    bool after_note = false;
    for (const Command& command : commands.items)
    {
        if (command.kind == Kind::kVolume && after_note)
        {
            ++volumes;
        }
        after_note =
            command.kind == Kind::kNote || (after_note && command.kind == Kind::kWait && command.value.value == 0);
    }

    return volumes;
}

// Whether `after` plays only notes that `before` plays, up to the earlier loop's end. `apart` leaves notes out of both.
bool OnlyLosesNotes(const hokuto::song::Sequence& before, const hokuto::song::Sequence& after,
                    const hokuto::tools::NotesApart& apart)
{
    const uint32_t end =
        before.loop && after.loop ? std::min(before.loop->end, after.loop->end) : std::numeric_limits<uint32_t>::max();
    const auto starts = [end, &apart](const hokuto::song::Sequence& song)
    {
        std::vector<std::tuple<int, uint32_t, uint8_t, uint8_t>> notes;
        for (const auto& [track, start, key, length, velocity] : NotesOf(song, apart))
        {
            if (start < end)
            {
                notes.emplace_back(track, start, key, velocity);
            }
        }
        std::sort(notes.begin(), notes.end());

        return notes;
    };
    const auto all = starts(before);
    const auto kept = starts(after);

    return std::includes(all.begin(), all.end(), kept.begin(), kept.end());
}

} // namespace

bool CutByVolume(const File& edited, uint32_t sequence, const hokuto::song::Sequence& before,
                 const hokuto::song::Sequence& after, const hokuto::tools::NotesApart& apart)
{
    return KonamiVolumesAfterNotes(*edited.sequences[sequence]) != 0 && OnlyLosesNotes(before, after, apart);
}

} // namespace hokuto::tools::commands
