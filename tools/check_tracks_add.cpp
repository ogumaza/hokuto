// SPDX-License-Identifier: MIT

// The round of hokuto_check_tracks that adds tracks, and the copies of tracks.

#include "check_tracks.h"

namespace hokuto::tools::tracks
{

namespace
{

// The velocity of the note that goes into a new track.
constexpr int kVelocity = 100;

// The longest length up to `ticks` that a note code of MusicPlayer2000 encodes, a tick at least.
uint32_t CodeLengthUpTo(uint32_t ticks)
{
    uint32_t longest = 1;
    for (int code = 1; code <= 48; code++)
    {
        const auto length = static_cast<uint32_t>(ntdlib::mp2k::ClockLength(code));
        if (length <= ticks)
        {
            longest = length;
        }
    }

    return longest;
}

// The start of the sound at `place` in `file`: its item in its sequence.
uint32_t StartOf(const File& file, const edit::SoundPlace& place)
{
    return place.start_field ? file.native->fields.at(*place.start_field).to.offset : 0;
}

} // namespace

std::vector<bool> ItemsOfTrack(const File& file, const edit::SoundPlace& place, int track)
{
    const ntdlib::commands::Sequence& sequence = *file.sequences[place.sequence];
    if (g_dse)
    {
        // A DSE track runs from its start up to the next track's start. Its commands reach no other track.
        std::vector<bool> items(sequence.items.size(), false);
        const std::vector<uint8_t> model = ntdlib::dse::TrackOpening(0, 0).bytes;
        int at = -1;
        for (std::size_t item = 0; item < items.size(); item++)
        {
            const Command& command = sequence.items[item];
            if (command.kind == Kind::kData && command.bytes.size() == model.size() &&
                std::equal(model.begin(), model.begin() + 4, command.bytes.begin()))
            {
                ++at;
            }
            items[item] = at == track;
        }

        return items;
    }
    if (!place.track_fields.empty())
    {
        const bool held = track >= 0 && static_cast<std::size_t>(track) < place.track_fields.size();
        return held ? edit::Reached(
                          sequence,
                          file.native->fields.at(place.track_fields[static_cast<std::size_t>(track)]).to.offset, false)
                    : std::vector<bool>(sequence.items.size(), false);
    }

    const uint32_t start = StartOf(file, place);
    if (track == 0)
    {
        return edit::Reached(sequence, start, false);
    }

    std::vector<bool> items(sequence.items.size(), false);
    const std::vector<bool> reached = edit::Reached(sequence, start, true);
    for (uint32_t item = 0; item < sequence.items.size(); item++)
    {
        const Command& command = sequence.items[item];
        if (!reached[item] || command.kind != Kind::kOpenTrack || command.index != track || command.target_outside)
        {
            continue;
        }

        const std::vector<bool> runs = edit::Reached(sequence, command.target, false);
        for (std::size_t i = 0; i < items.size(); i++)
        {
            items[i] = items[i] || runs[i];
        }
    }

    return items;
}

bool DrawsDseNumbers(const Command& command)
{
    if (!g_dse || command.kind != Kind::kOther)
    {
        return false;
    }

    const auto draws = [](uint8_t waveform)
    {
        return waveform == 6 || waveform == 7;
    };
    switch (command.encoding.opcode)
    {
    case 0xD5:
    case 0xD6:
        return true;
    case 0xDC:
    case 0xE4:
    case 0xEC:
    case 0xF0:
        return command.bytes.size() > 4 && draws(command.bytes[4]);
    case 0xF2:
        return command.bytes.size() > 1 && command.bytes[0] == 4 && draws(command.bytes[1]);
    default:
        return false;
    }
}

namespace
{

// Whether a program that DSE's `sound` plays contains an LFO that draws random numbers: its waveforms 6 and 7, in byte
// 3 of its settings. Each new voice of such a program changes the numbers that the others draw. A note's pitch then
// changes, and so does the end of a wave that plays once.
bool DrawsDseLfos(const Archive& archive, const song::Sequence& sound)
{
    if (!g_dse)
    {
        return false;
    }

    const auto& banks = archive.archive.dse->sounds.at(sound.sound).banks;
    for (const song::Track& track : sound.tracks)
    {
        for (const song::Note& note : track.notes)
        {
            if (note.bank_slot >= banks.size() || !banks[note.bank_slot]->instruments ||
                note.program >= banks[note.bank_slot]->instruments->programs.size())
            {
                continue;
            }
            const auto& program = banks[note.bank_slot]->instruments->programs[note.program];
            if (program && std::ranges::any_of(program->lfos, [](const auto& lfo)
                                               { return lfo[1] != 0 && (lfo[3] == 6 || lfo[3] == 7); }))
            {
                return true;
            }
        }
    }

    return false;
}

} // namespace

bool Chancy(const Command& command)
{
    const bool variables = (command.kind >= Kind::kSetVariable && command.kind <= Kind::kCompareNotEqual) ||
                           (command.kind >= Kind::kJumpIfEqual && command.kind <= Kind::kJumpIfLess);
    const bool argument =
        command.value.source != Source::kValue || (command.time && command.time->source != Source::kValue);
    return variables || argument || command.conditional || command.kind == Kind::kUserProcedure ||
           DrawsDseNumbers(command);
}

namespace
{

// Whether a command of Brownie Brown's driver turns square 1's sweep on (FA with a value other than 0). Only square 1
// has a sweep (NR10). A copy on another channel plays such notes without it, and the end of each note loses the sweep.
bool Sweeps(const Command& command)
{
    return command.kind == Kind::kSweep && command.index == 0 && command.value.value != 0;
}

} // namespace

std::optional<int> BusiestTrack(const song::Sequence& sound)
{
    const song::Track* busiest = hokuto::tools::BusiestTrack(sound);
    return busiest ? std::optional<int>(busiest->number) : std::nullopt;
}

namespace
{

// The first note of `sound`: the first of its first track with notes. Nothing for a sound without notes.
std::optional<song::Note> FirstNote(const song::Sequence& sound)
{
    for (const song::Track& track : sound.tracks)
    {
        if (!track.notes.empty())
        {
            return track.notes.front();
        }
    }

    return std::nullopt;
}

// How a sound plays its new note: the notes of the note's track, the tick of the first one, whether the note plays,
// and whether it plays a whole number of the loop's lengths sooner.
struct HeardNote
{
    std::vector<TrackNote> heard;
    uint32_t first_tick = 0;
    bool sooner = false;
    bool plays = false;
};

// How sound `sound` plays its new note `note` (track, tick and key). `read` is the sound in the reads of the file with
// the new notes, and `after` contains the reads with the new tracks.
HeardNote HearNewNote(const Archive& archive, const std::map<uint32_t, song::Sequence>& after, uint32_t sound,
                      const std::tuple<int, uint32_t, uint8_t>& note,
                      std::map<uint32_t, song::Sequence>::const_iterator read)
{
    // The note plays at its tick. In a loop, it plays in each pass that the arrangement shows. The new track can
    // move the loop by whole passes, and the passes before or after the loop's start then play the note too.
    const auto& [track, tick, key] = note;
    const std::vector<TrackNote> heard = NotesOf(read->second, track);

    // On Ubisoft Milan's driver, the byte of the note contains the volume of the channel, and the Key Editor shows the
    // velocity of that volume (song::UbiMilanVelocityOf). A note of Brownie Brown's driver plays at 127.
    const int velocity =
        archive.archive.UbiMilan()
            ? song::UbiMilanVelocityOf(track, key, song::UbiMilanDriverVelocity(track, key, kVelocity, 0))
        : g_brownie ? 127
                    : kVelocity;
    const std::optional<song::Loop>& loop = read->second.loop;
    const uint32_t period = loop ? loop->end - loop->start : 0;
    const auto same = [key, tick, period, velocity](const TrackNote& other)
    {
        const uint32_t at = std::get<0>(other);
        const uint32_t apart = at > tick ? at - tick : tick - at;
        const bool pass = apart == 0 || (period > 0 && apart % period == 0);
        return pass && std::get<1>(other) == key && std::get<3>(other) == velocity;
    };
    const auto at_tick = [tick](const TrackNote& other)
    {
        return std::get<0>(other) == tick;
    };

    // The note went in at the loop's start of the read with the new track, into the loop's body. It plays first in
    // the loop's first pass, a whole number of the loop's lengths sooner.
    uint32_t pass = 0;
    if (const auto with_track = after.find(sound); with_track != after.end() && with_track->second.loop &&
                                                   with_track->second.loop->end > with_track->second.loop->start)
    {
        pass = with_track->second.loop->end - with_track->second.loop->start;
    }
    const uint32_t first_tick = heard.empty() ? tick : std::get<0>(heard[0]);
    const bool sooner = pass > 0 && first_tick < tick && (tick - first_tick) % pass == 0;
    const bool plays =
        !heard.empty() && (std::ranges::any_of(heard, at_tick) || sooner) && std::ranges::all_of(heard, same);

    return {heard, first_tick, sooner, plays};
}

// Checks that the new note `note` (track, tick and key) of `read` plays the instrument of `first`, the sound's first
// note, and that it starts where it plays. `heard` describes how the note plays.
void CheckNewNoteStarts(const Archive& archive, std::map<uint32_t, song::Sequence>::const_iterator read,
                        const std::tuple<int, uint32_t, uint8_t>& note, const std::optional<song::Note>& first,
                        const HeardNote& heard, Totals& totals)
{
    const auto& [track, tick, key] = note;
    const uint32_t first_tick = heard.first_tick;
    const auto numbered = [track](const song::Track& candidate)
    {
        return candidate.number == track;
    };
    const auto new_track = std::ranges::find_if(read->second.tracks, numbered);
    const auto other_instrument = [&first, tick, first_tick, key](const song::Note& started)
    {
        return (started.start == tick || started.start == first_tick) && started.key == key &&
               (started.program != first->program || started.bank_slot != first->bank_slot);
    };
    if (new_track != read->second.tracks.end() && std::ranges::any_of(new_track->notes, other_instrument))
    {
        std::printf("%s: %s's new note on track %d at tick %u doesn't play program %u of slot %d\n",
                    archive.name.c_str(), read->second.name.c_str(), track, tick, first->program,
                    static_cast<int>(first->bank_slot));
        ++totals.problems;
    }

    const auto here = [tick, first_tick, key](const song::Note& started)
    {
        return (started.start == tick || started.start == first_tick) && started.key == key;
    };
    if (heard.plays && (new_track == read->second.tracks.end() || std::ranges::none_of(new_track->notes, here)))
    {
        std::printf("%s: %s's new note on track %d at tick %u never starts\n", archive.name.c_str(),
                    read->second.name.c_str(), track, tick);
        ++totals.unstarted_notes;
    }
}

// `read` without the notes of track `track`.
song::Sequence WithoutTrackNotes(const song::Sequence& read, int track)
{
    song::Sequence others = read;
    for (song::Track& other : others.tracks)
    {
        if (other.number == track)
        {
            other.notes.clear();
            other.unstarted.clear();
        }
    }

    return others;
}

// On the GBA, a later note of another track can take the note's PSG channel before the note sounds. ntdlib's
// performer then records the note nowhere (open work, item 218). Whether a note of a track other than `track` of
// `read` takes the PSG channel of a new note at `tick` that plays the instrument of `first`.
bool TakesPsgChannel(const song::Sequence& read, const song::Note& first, int track, uint32_t tick)
{
    const uint32_t length = std::clamp<uint32_t>(first.length, 1, edit::kLongestMp2kNote);
    const auto takes_channel = [&first, track, tick, length](const song::Track& other)
    {
        const auto later = [&first, tick, length](const song::Note& started)
        {
            return started.psg == first.psg && started.start > tick && started.start < tick + length;
        };
        return other.number != track && std::ranges::any_of(other.notes, later);
    };
    return read.gba && first.psg != 0 && std::ranges::any_of(read.tracks, takes_channel);
}

// On Nintendo R&D2's driver, a square, the wave or the noise plays one note at a time. The game's code can give a song
// fewer sample voices, down to one. The new track keeps the driver's starting priority, and the song's tracks can set a
// higher one. A note of another track that occupies the same kind of voice at the tick of the new note can then keep
// the voice from the new note. The new note plays the first note's instrument. Whether a note of a track other than
// `track` of `read`, sequence `sequence` of `noted`, occupies the voice of `first` at `tick`.
bool Rd2VoiceHeld(const File& noted, uint32_t sequence, const song::Sequence& read, const song::Note& first, int track,
                  uint32_t tick)
{
    const auto voice = [&noted, sequence](const song::Note& of)
    {
        return edit::Rd2VoiceTypeOf(noted, sequence, of.bank_slot, of.program, of.key);
    };
    const std::optional<uint8_t> type = voice(first);
    const auto held = [&](const song::Note& other)
    {
        return other.start <= tick && tick < other.start + std::max(other.length, 1u) && voice(other) == type;
    };
    const auto holds = [&held, track](const song::Track& other)
    {
        return other.number != track && std::ranges::any_of(other.notes, held);
    };
    return type && std::ranges::any_of(read.tracks, holds);
}

// The first two notes of `heard`, for a line about a note that doesn't play alone.
std::string HeardText(const std::vector<TrackNote>& heard)
{
    std::string there;
    for (std::size_t i = 0; i < heard.size() && i < 2; i++)
    {
        there += (i == 0 ? " (" : ", ") + std::string("key ") + std::to_string(std::get<1>(heard[i])) + " at " +
                 std::to_string(std::get<0>(heard[i])) + ", velocity " + std::to_string(std::get<3>(heard[i])) +
                 (i + 1 == heard.size() || i == 1 ? ")" : "");
    }

    return there;
}

// Checks that sound `sound` plays its new note `note` (track, tick and key), and its other notes as before. `read` is
// the sound in the reads of `noted`, the file with the new notes, and `after` contains the reads with the new tracks.
// `firsts` contains the first note of each sound.
void CheckNewNote(const Archive& archive, const File& noted, const std::map<uint32_t, song::Sequence>& after,
                  const std::map<uint32_t, std::optional<song::Note>>& firsts, uint32_t sound,
                  const std::tuple<int, uint32_t, uint8_t>& note,
                  std::map<uint32_t, song::Sequence>::const_iterator read, Totals& totals)
{
    const auto& [track, tick, key] = note;
    const HeardNote heard_note = HearNewNote(archive, after, sound, note, read);
    const std::vector<TrackNote>& heard = heard_note.heard;
    const bool plays = heard_note.plays;
    totals.sooner_notes += plays && heard_note.sooner ? 1 : 0;

    // A note that the driver started plays the instrument of the sound's first note.
    const std::optional<song::Note>& first = firsts.at(sound);
    CheckNewNoteStarts(archive, read, note, first, heard_note, totals);

    const song::Sequence others = WithoutTrackNotes(read->second, track);
    const Comparison comparison =
        Compare(after.at(sound), others, NumbersOf(after.at(sound)), std::nullopt, 0, std::nullopt, 0, true);
    const std::optional<std::string>& difference = comparison.difference;
    totals.moved_loops += comparison.moved ? 1 : 0;

    const bool taken = TakesPsgChannel(read->second, *first, track, tick);
    const uint32_t sequence = archive.places[sound]->sequence;
    // ntdlib's performer can find the loop a whole number of passes sooner, with its end at the new note's tick
    // (open work, item 221). The read then ends before the note.
    const std::optional<song::Loop>& loop = read->second.loop;
    if (heard.empty() && !difference && comparison.moved && loop && loop->end == tick)
    {
        std::printf("%s: %s's loop ends at its new note on track %d at tick %u\n", archive.name.c_str(),
                    read->second.name.c_str(), track, tick);
        ++totals.loop_end_notes;
    }
    else if (heard.empty() && !difference && taken)
    {
        std::printf("%s: %s's new note on track %d at tick %u never sounds: a later note takes its PSG channel\n",
                    archive.name.c_str(), read->second.name.c_str(), track, tick);
        ++totals.taken_notes;
    }
    else if (g_rd2 && heard.empty() && !difference && Rd2VoiceHeld(noted, sequence, read->second, *first, track, tick))
    {
        ++totals.held_channels;
    }
    else if (plays && difference &&
             VoicesDecide(
                 [&]
                 {
                     return !Compare(after.at(sound), others, NumbersOf(after.at(sound)), std::nullopt, 0, std::nullopt,
                                     0, true)
                                 .difference;
                 }))
    {
        ++totals.voice_notes;
    }
    else if (g_brownie && plays && difference &&
             !Compare(after.at(sound), others, NumbersOf(after.at(sound)), std::nullopt, 0, std::nullopt, kTickNotes,
                      true)
                  .difference)
    {
        ++totals.beat_notes;
    }
    else if (!plays || difference)
    {
        const std::string there = HeardText(heard);
        std::printf("%s: %s doesn't play its new note of key %d on track %d at tick %u alone: %zu notes there%s%s%s\n",
                    archive.name.c_str(), read->second.name.c_str(), key, track, tick, heard.size(), there.c_str(),
                    difference ? ", and " : "", difference ? difference->c_str() : "");
        ++totals.problems;
    }
    ++totals.notes;
}

// Reads the sounds of `version`, the file with the new tracks of `added`, and checks that the sounds with new tracks
// play their notes as before, and the others play as before. Adds the sounds that share a sound's file to `added`.
std::map<uint32_t, song::Sequence> CheckNewTracksPlay(const Archive& archive, const File& version,
                                                      std::map<uint32_t, int>& added, Totals& totals)
{
    for (const auto& [sharer, sound] : Sharers(archive, added))
    {
        added.emplace(sharer, added.at(sound));
    }
    const std::map<uint32_t, song::Sequence> after = ReadAll(archive, version, "new tracks", totals);
    std::set<uint32_t> edited;
    for (const auto& [sound, track] : added)
    {
        edited.insert(sound);
        const auto read = after.find(sound);
        if (read == after.end())
        {
            continue;
        }

        const song::Sequence& before = archive.songs.at(sound);
        std::set<int> numbers = NumbersOf(before);
        if (numbers.contains(track))
        {
            std::printf("%s: %s gains track %d but plays it already\n", archive.name.c_str(), before.name.c_str(),
                        track);
            ++totals.problems;
            continue;
        }

        numbers.insert(track);
        // A sound whose read shows no track ends in its first frame, and ntdlib's performer leaves out the reads of
        // that frame. A new track that rests makes the sound last longer, and the sound's other tracks then show,
        // without notes.
        if (before.tracks.empty() &&
            std::ranges::any_of(read->second.tracks,
                                [track](const song::Track& shown) { return shown.number == track; }) &&
            std::ranges::all_of(read->second.tracks, [](const song::Track& shown)
                                { return shown.notes.empty() && shown.unstarted.empty(); }))
        {
            ++totals.added_to_empty;
            continue;
        }
        const Comparison comparison = Compare(before, read->second, numbers, std::nullopt, 0, track);
        const std::optional<std::string>& difference = comparison.difference;
        totals.moved_loops += comparison.moved ? 1 : 0;
        if (before.holds)
        {
            ++totals.added_holding;
            totals.added_differing += difference ? 1 : 0;
        }
        else if (difference)
        {
            std::printf("%s: %s plays %s with a new track %d\n", archive.name.c_str(), before.name.c_str(),
                        difference->c_str(), track);
            ++totals.problems;
        }
    }
    CheckOthers(archive, after, edited, "new tracks", totals);

    return after;
}

// The file with the new notes, and the new note of each sound by its track, tick and key.
struct LoopNotes
{
    File noted;
    std::map<uint32_t, std::tuple<int, uint32_t, uint8_t>> notes;
};

// Puts a note in the new track of the first sound of each sequence of `chosen`, at the loop's start or at the start.
// `version` is the file with the new tracks of `added`, `after` contains its reads, and `firsts` contains the first
// note of each sound.
LoopNotes AddLoopNotes(const Archive& archive, const std::vector<uint32_t>& chosen, const File& version,
                       const std::map<uint32_t, int>& added, const std::map<uint32_t, song::Sequence>& after,
                       std::map<uint32_t, std::optional<song::Note>>& firsts, Totals& totals)
{
    File noted = version;
    std::set<uint32_t> sequences;
    std::map<uint32_t, std::tuple<int, uint32_t, uint8_t>> notes;
    for (const uint32_t sound : chosen)
    {
        const auto track = added.find(sound);
        const auto read = after.find(sound);
        const std::optional<song::Note>& first = firsts[sound];
        const uint32_t sequence = archive.places[sound]->sequence;
        if (track == added.end() || read == after.end() || !first || sequences.contains(sequence))
        {
            continue;
        }

        const auto numbered = [&track](const song::Track& candidate)
        {
            return candidate.number == track->second;
        };
        const auto played = std::find_if(read->second.tracks.begin(), read->second.tracks.end(), numbered);
        // The new track can move the loop by whole passes. The tool adds the note at the loop start of the read with
        // the new track: at the original loop start, a loop that moved a pass earlier ends before the note. Without a
        // loop, the tool adds the note where the tracks of the sound start: at tick 1 on Brownie Brown's driver. The
        // channels of that driver start a frame after the sound.
        uint32_t tick = 0;
        if (read->second.loop)
        {
            tick = read->second.loop->start;
        }
        else if (const auto earliest =
                     std::ranges::min_element(archive.songs.at(sound).tracks, {}, [](const song::Track& other)
                                              { return other.stretches.empty() ? 0 : other.stretches.front().tick; });
                 earliest != archive.songs.at(sound).tracks.end() && !earliest->stretches.empty())
        {
            tick = earliest->stretches.front().tick;
        }
        const std::optional<song::Stretch> stretch =
            played != read->second.tracks.end() ? edit::StretchAt(played->stretches, tick) : std::nullopt;
        if (!stretch)
        {
            std::printf("%s: %s's new track %d has no stretch at tick %u\n", archive.name.c_str(),
                        read->second.name.c_str(), track->second, tick);
            ++totals.problems;
            continue;
        }

        // MusicPlayer2000 holds a note of kLongestMp2kNote ticks at most without a tie. Metroid Fusion's sequencer
        // reads no extra length after a note (edit/mp2k.h): a note there lasts the longest length that a note code has,
        // up to the length of the first note.
        const uint32_t longest = archive.archive.gba ? edit::kLongestMp2kNote : std::numeric_limits<uint32_t>::max();
        uint32_t length = std::clamp<uint32_t>(first->length, 1, longest);
        if (archive.archive.gba && archive.archive.gba->info.metroid_short_notes)
        {
            length = CodeLengthUpTo(length);
        }
        try
        {
            // On Nintendo R&D2's driver, a note of a square or of the wave shows at the pitch that its region sets. The
            // new track plays the first note's instrument without a transpose.
            int key = first->key;
            if (g_rd2)
            {
                const std::optional<int> command =
                    edit::Rd2CommandKey(noted, sequence, first->bank_slot, first->program, 0, first->key);
                if (!command)
                {
                    std::printf("%s: no key of %s's first instrument shows at key %d\n", archive.name.c_str(),
                                read->second.name.c_str(), first->key);
                    ++totals.problems;
                    continue;
                }
                key = *command;
            }
            // On Brownie Brown's driver, the new track has the settings of the track of the first note there, and the
            // byte of the first note sets its key.
            if (g_brownie)
            {
                key = edit::CommandKey(archive.file, sequence, first->command);
            }
            edit::NewNote note{*stretch, tick, key, kVelocity, length};
            if (archive.archive.UbiMilan())
            {
                // On Ubisoft Milan's driver, the note's command names the new track's channel.
                note.index = static_cast<uint8_t>(track->second);
            }

            // A GBA ROM without room for the song that the note makes longer leaves the note out. Such a ROM leaves out
            // a new track in the same way. With filler runs, the runs can fill up.
            File with_note = edit::EditNotes(noted, sequence, {}, {&note, 1}).file;
            if (hokuto::tools::Outgrows(archive.archive, with_note, sequence))
            {
                ++totals.outgrown;
                continue;
            }
            noted = std::move(with_note);
            sequences.insert(sequence);
            notes.emplace(sound, std::tuple<int, uint32_t, uint8_t>{track->second, tick, first->key});
        }
        catch (const std::exception& error)
        {
            std::printf("%s: a note can't go into %s's new track %d: %s\n", archive.name.c_str(),
                        read->second.name.c_str(), track->second, error.what());
            ++totals.problems;
        }
    }

    return {std::move(noted), std::move(notes)};
}

// The file with a new track in each sound of `chosen` that accepts one, the new track of each sound, and the first note
// of each sound before.
struct NewTracks
{
    File version;
    std::map<uint32_t, int> added;
    std::map<uint32_t, std::optional<song::Note>> firsts;
};

// Adds a new track to each sound of `chosen`, all in one version, with the instrument of the sound's first note.
NewTracks AddChosenTracks(const Archive& archive, const std::vector<uint32_t>& chosen, Totals& totals)
{
    File version = archive.file;
    std::map<uint32_t, int> added;
    std::map<uint32_t, std::optional<song::Note>> firsts;
    for (const uint32_t sound : chosen)
    {
        const song::Sequence& before = archive.songs.at(sound);
        const std::optional<song::Note> first = FirstNote(before);
        const std::optional<edit::Program> instrument =
            first ? std::optional<edit::Program>(edit::Program{first->bank_slot, static_cast<int>(first->program)})
                  : std::nullopt;
        try
        {
            edit::TrackEdit edited = edit::AddTrack(version, archive.places, sound, before, instrument);
            if (hokuto::tools::Outgrows(archive.archive, edited.file, archive.places[sound]->sequence))
            {
                ++totals.outgrown;
                continue;
            }
            version = std::move(edited.file);
            added.emplace(sound, edited.track);
            firsts.emplace(sound, first);
            ++totals.added;
        }
        catch (const std::invalid_argument& error)
        {
            ++totals.refusals[std::string("new track: ") + error.what()];
        }
        catch (const std::runtime_error& error)
        {
            std::printf("%s: %s: new track: %s\n", archive.name.c_str(), before.name.c_str(), error.what());
            ++totals.problems;
        }
    }

    return {std::move(version), std::move(added), std::move(firsts)};
}

} // namespace

void AddTracks(const Archive& archive, const std::vector<uint32_t>& chosen, Totals& totals)
{
    auto [version, added, firsts] = AddChosenTracks(archive, chosen, totals);

    // The sounds with new tracks play their notes as before, and the others play as before.
    const std::map<uint32_t, song::Sequence> after = CheckNewTracksPlay(archive, version, added, totals);

    // A note in the new track of the first sound of each sequence, at the loop's start or at the start.
    const auto [noted, notes] = AddLoopNotes(archive, chosen, version, added, after, firsts, totals);

    // Each sound plays its new note, and its other notes as before.
    const std::map<uint32_t, song::Sequence> played = ReadAll(archive, noted, "new notes", totals);
    for (const auto& [sound, note] : notes)
    {
        const auto read = played.find(sound);
        if (read == played.end())
        {
            continue;
        }

        CheckNewNote(archive, noted, after, firsts, sound, note, read, totals);
    }
}

void CopyTracks(const Archive& archive, const std::vector<uint32_t>& chosen, Totals& totals)
{
    File version = archive.file;
    std::map<uint32_t, std::pair<int, int>> copies;
    std::set<uint32_t> apart;
    std::set<uint32_t> sweeping;
    for (const uint32_t sound : chosen)
    {
        const song::Sequence& before = archive.songs.at(sound);
        const std::optional<int> track = BusiestTrack(before);
        if (!track)
        {
            continue;
        }

        const edit::SoundPlace& place = *archive.places[sound];
        const ntdlib::commands::Sequence& sequence = *archive.file.sequences[place.sequence];
        try
        {
            edit::TrackEdit edited = edit::DuplicateTrack(version, archive.places, sound, *track);
            if (hokuto::tools::Outgrows(archive.archive, edited.file, place.sequence))
            {
                ++totals.outgrown;
                continue;
            }
            version = std::move(edited.file);
            copies.emplace(sound, std::pair<int, int>{*track, edited.track});
            if (Any(sequence, ItemsOfTrack(archive.file, place, *track), Chancy) ||
                DrawsDseLfos(archive, archive.songs.at(sound)))
            {
                apart.insert(sound);
            }
            if (g_brownie && edited.track % 4 != 0 && Any(sequence, ItemsOfTrack(archive.file, place, *track), Sweeps))
            {
                sweeping.insert(sound);
            }
            ++totals.copied;
        }
        catch (const std::invalid_argument& error)
        {
            ++totals.refusals[std::string("copy: ") + error.what()];
        }
        catch (const std::runtime_error& error)
        {
            std::printf("%s: %s: copy: %s\n", archive.name.c_str(), before.name.c_str(), error.what());
            ++totals.problems;
        }
    }

    for (const auto& [sharer, sound] : Sharers(archive, copies))
    {
        copies.emplace(sharer, copies.at(sound));
        if (apart.contains(sound))
        {
            apart.insert(sharer);
        }
        if (sweeping.contains(sound))
        {
            sweeping.insert(sharer);
        }
    }
    const std::map<uint32_t, song::Sequence> after = ReadAll(archive, version, "copies", totals);
    std::set<uint32_t> edited;
    for (const auto& [sound, tracks] : copies)
    {
        edited.insert(sound);
        const auto read = after.find(sound);
        if (read == after.end())
        {
            continue;
        }

        const auto& [original, copy] = tracks;
        const song::Sequence before = ComparableRead(archive, archive.file, sound, archive.songs.at(sound));
        const song::Sequence now = ComparableRead(archive, version, sound, read->second);
        std::set<int> numbers = NumbersOf(before);
        numbers.insert(copy);
        const Comparison comparison = Compare(before, now, numbers, copy, original);
        const std::optional<std::string>& difference = comparison.difference;
        totals.moved_loops += comparison.moved ? 1 : 0;
        if (apart.contains(sound))
        {
            ++totals.copied_apart;
            totals.copied_differing += difference ? 1 : 0;
        }
        else if (difference && !Compare(before, now, numbers, copy, original, std::nullopt, kTickNotes).difference)
        {
            ++totals.tick_notes;
        }
        else if (difference && VoicesDecide([&] { return !Compare(before, now, numbers, copy, original).difference; }))
        {
            ++totals.voice_notes;
        }
        else if (difference && archive.archive.Konami() && before.loop && !now.loop)
        {
            // A copy has no loop point, and returns to its start when the song loops. The performer then can find no
            // pass that the next pass repeats.
            ++totals.copied_without_loop;
        }
        else if (difference && sweeping.contains(sound))
        {
            ++totals.copied_without_sweep;
        }
        else if (difference)
        {
            std::printf("%s: %s plays %s with a copy of track %d\n", archive.name.c_str(), before.name.c_str(),
                        difference->c_str(), original);
            ++totals.problems;
        }
    }
    CheckOthers(archive, after, edited, "copies", totals);
}

} // namespace hokuto::tools::tracks
