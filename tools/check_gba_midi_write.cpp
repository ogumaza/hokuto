// SPDX-License-Identifier: MIT

// The MIDI file that hokuto_check_gba_midi writes of each song, as the GBA community's converters write one, and the
// notes that the comparison leaves out of it. check_gba_midi.cpp describes the check.

#include <algorithm>
#include <cmath>
#include <map>
#include <optional>
#include <set>
#include <utility>
#include <vector>

#include "ntdlib/agb/gba.h"
#include "ntdlib/commands/sequence.h"
#include "ntdlib/commands/units.h"
#include "ntdlib/mp2k/commands.h"
#include "ntdlib/mp2k/driver.h"
#include "ntdlib/mp2k/sequencer.h"
#include "seqlib/midi/builder.h"

#include "check_gba_midi.h"
#include "edit/bytecode.h"
#include "edit/commands.h"
#include "edit/gba_midi.h"
#include "song/sequence.h"

namespace hokuto::tools::gba_midi
{
namespace
{

using hokuto::format::File;
using hokuto::song::Note;
using hokuto::song::Sequence;
using hokuto::song::Track;
using ntdlib::commands::kOne;

// The tempo at which MusicPlayer2000 starts a song, in beats a minute.
constexpr uint32_t kMp2kStartingTempo = 150;

// The first note after note `n` of `notes` that sounds at the same key in `transposed_keys` and starts later, if any.
std::optional<std::size_t> NextOfKey(const std::vector<std::pair<Note, bool>>& notes,
                                     const std::vector<int>& transposed_keys, std::size_t n)
{
    for (std::size_t later = n + 1; later < notes.size(); later++)
    {
        if (transposed_keys[later] == transposed_keys[n] && notes[later].first.start > notes[n].first.start)
        {
            return later;
        }
    }

    return std::nullopt;
}

// Each command of `commands` by its offset in `offsets`.
std::map<uint32_t, const ntdlib::commands::Command*> CommandsByOffset(const ntdlib::commands::Sequence& commands,
                                                                      const std::vector<uint32_t>& offsets)
{
    std::map<uint32_t, const ntdlib::commands::Command*> by_offset;
    for (uint32_t item = 0; item + 1 < offsets.size(); item++)
    {
        by_offset.emplace(offsets[item], &commands.items[item]);
    }

    return by_offset;
}

// The value of `command` in MusicPlayer2000's units.
int64_t ValueOf(const ntdlib::commands::Command& command)
{
    return ntdlib::commands::FromUnit(command.value.value, ntdlib::mp2k::ScaleOf(command.kind));
}

// Whether `row` runs before `loop_end`, the end of the loop's first pass, if any.
bool InSong(const hokuto::edit::CommandRow& row, const std::optional<uint32_t>& loop_end)
{
    return row.tick && (!loop_end || *row.tick < *loop_end);
}

// Whether track `t` of `song` plays a note command after its first voice, reported or not. `rows_of` contains the
// command rows of each track, and `by_offset` each command by its offset.
bool PlaysNotes(const Sequence& song, const std::vector<std::vector<hokuto::edit::CommandRow>>& rows_of,
                const std::map<uint32_t, const ntdlib::commands::Command*>& by_offset,
                const std::optional<uint32_t>& loop_end, std::size_t t)
{
    if (!song.tracks[t].notes.empty() || !song.tracks[t].unstarted.empty())
    {
        return true;
    }
    bool voiced = false;
    for (const hokuto::edit::CommandRow& row : rows_of[t])
    {
        const auto command = by_offset.find(row.command);
        if (InSong(row, loop_end) && command != by_offset.end())
        {
            voiced = voiced || command->second->kind == ntdlib::commands::Kind::kProgram;
            if (voiced && command->second->kind == ntdlib::commands::Kind::kNote)
            {
                return true;
            }
        }
    }
    return false;
}

// A track's settings as MidiOf writes them, and what the track's notes take from them.
struct TrackSettings
{
    std::map<std::pair<uint32_t, uint32_t>, int> steps;  // each note command's step at its tick
    std::map<std::pair<uint32_t, uint32_t>, int> shifts; // each note command's transposition at its tick
    std::vector<hokuto::edit::CommandRow> note_rows;     // the note commands after the track's first voice
    bool wide_bend = false;                              // a bend range that MIDI's 127 semitones don't hold
    std::optional<uint32_t> track_end;
};

// Writes the settings of a track's command rows `rows` to `out` on MIDI channel `channel`, in the form that the GBA
// community's converters take. `by_offset` contains each command by its offset, and the rows run up to `loop_end`.
TrackSettings WriteTrackSettings(const std::vector<hokuto::edit::CommandRow>& rows,
                                 const std::map<uint32_t, const ntdlib::commands::Command*>& by_offset,
                                 const std::optional<uint32_t>& loop_end, int channel, seqlib::midi::TrackBuilder& out,
                                 Totals& totals)
{
    TrackSettings settings;
    std::map<uint32_t, int> notes_at;
    bool voiced = false;
    int transpose = 0;
    for (const hokuto::edit::CommandRow& row : rows)
    {
        const auto command = by_offset.find(row.command);
        if (!InSong(row, loop_end) || command == by_offset.end())
        {
            continue;
        }
        const int value = static_cast<int>(ValueOf(*command->second));
        const uint32_t tick = *row.tick;
        using enum ntdlib::commands::Kind;
        switch (command->second->kind)
        {
        case kNote:
            settings.steps[{row.command, tick}] = notes_at[tick]++;
            settings.shifts[{row.command, tick}] = transpose;
            if (voiced)
            {
                settings.note_rows.push_back(row);
            }
            break;
        case kProgram:
            out.Program(tick, channel, value, notes_at[tick]);
            voiced = true;
            break;
        case kVolume:
            out.Control(tick, channel, 7, value);
            break;
        case kPan:
            out.Control(tick, channel, 10, value);
            totals.commands_left_out += value > 127;
            break;
        case kPitchBend:
            out.PitchBend(tick, channel, std::clamp(value, 0, 127) << 7);
            break;
        case kBendRange:
            out.BendRange(tick, channel, std::min(value, 127));
            totals.commands_left_out += value > 127;
            settings.wide_bend = settings.wide_bend || value > 127;
            break;
        case kLfoDepth:
            out.Control(tick, channel, 1, value);
            break;
        case kLfoSpeed:
            out.Control(tick, channel, 21, value);
            break;
        case kLfoType:
            out.Control(tick, channel, 22, value);
            break;
        case kLfoDelay:
            out.Control(tick, channel, 26, value);
            break;
        case kTune:
            out.Control(tick, channel, 24, value);
            break;
        case kPriority:
            out.Control(tick, channel, 33, value);
            break;
        case kEchoVolume:
        case kEchoLength:
            out.Control(tick, channel, 30, command->second->kind == kEchoVolume ? 8 : 9);
            out.Control(tick, channel, 29, value);
            break;
        case kTranspose:
            transpose = value;
            break;
        case kEnd:
            settings.track_end = settings.track_end.value_or(tick);
            break;
        case kNoteOff:
        case kWait:
        case kTempo:
        case kJump:
        case kCall:
        case kReturn:
        case kRepeat:
            break;
        default:
            totals.commands_left_out++;
            break;
        }
    }

    return settings;
}

// The notes of `track`, started or not, and the note commands of `note_rows` that the performer reports as neither, as
// their commands give them. `by_offset` contains each command by its offset. Counts the commands without a report.
std::vector<std::pair<Note, bool>> TrackNotes(const Track& track,
                                              const std::vector<hokuto::edit::CommandRow>& note_rows,
                                              const std::map<uint32_t, const ntdlib::commands::Command*>& by_offset,
                                              Totals& totals)
{
    std::vector<std::pair<Note, bool>> notes;
    std::set<std::pair<uint32_t, uint32_t>> reported;
    for (const Note& note : track.notes)
    {
        notes.emplace_back(note, true);
        reported.insert({note.command, note.start});
    }
    for (const Note& note : track.unstarted)
    {
        notes.emplace_back(note, false);
        reported.insert({note.command, note.start});
    }
    for (const hokuto::edit::CommandRow& row : note_rows)
    {
        const ntdlib::commands::Command& command = *by_offset.at(row.command);
        if (!reported.contains({row.command, *row.tick}))
        {
            Note note;
            note.start = *row.tick;
            note.length = static_cast<uint32_t>(command.value.value / kOne);
            note.key = command.key;
            note.velocity = static_cast<uint8_t>(command.velocity / kOne);
            note.command = row.command;
            notes.emplace_back(note, false);
            totals.unreported++;
        }
    }

    return notes;
}

// A tie that the driver never started ends at the track's next end of a tie of its key, or else where the loop,
// the track or the song ends.
uint32_t UnstartedTieEnd(const Note& tie, const Sequence& song, const std::vector<hokuto::edit::CommandRow>& rows,
                         const std::map<uint32_t, const ntdlib::commands::Command*>& by_offset,
                         const std::optional<uint32_t>& loop_end, const std::optional<uint32_t>& track_end)
{
    bool after = false;
    for (const hokuto::edit::CommandRow& row : rows)
    {
        const auto command = by_offset.find(row.command);
        if (!InSong(row, loop_end) || command == by_offset.end())
        {
            continue;
        }
        if (after && command->second->kind == ntdlib::commands::Kind::kNoteOff && command->second->key == tie.key)
        {
            return *row.tick;
        }
        after = after || (row.command == tie.command && *row.tick == tie.start);
    }
    return std::min({loop_end.value_or(song.length), track_end.value_or(song.length), song.length});
}

// Writes the tempos of `song` and the markers of its loop to `conductor`, and returns each track's command rows up to
// `loop_end`, the end of the loop's first pass. `by_offset` contains each command of `commands` by its offset in
// `offsets`.
std::vector<std::vector<hokuto::edit::CommandRow>> WriteConductor(
    const Sequence& song, const ntdlib::commands::Sequence& commands, const std::vector<uint32_t>& offsets,
    const std::map<uint32_t, const ntdlib::commands::Command*>& by_offset, const std::optional<uint32_t>& loop_end,
    seqlib::midi::TrackBuilder& conductor)
{
    conductor.SetEnd(song.length);
    std::vector<std::vector<hokuto::edit::CommandRow>> rows_of;
    bool tempo_at_start = false;
    for (const Track& track : song.tracks)
    {
        const std::vector<hokuto::edit::CommandRow>& rows =
            rows_of.emplace_back(hokuto::edit::CommandRows(commands, offsets, track.stretches, track.notes, loop_end));
        for (const hokuto::edit::CommandRow& row : rows)
        {
            const auto command = by_offset.find(row.command);
            if (InSong(row, loop_end) && command != by_offset.end() &&
                command->second->kind == ntdlib::commands::Kind::kTempo)
            {
                conductor.Tempo(
                    *row.tick,
                    static_cast<uint32_t>(std::lround(30000000.0 / std::max<int64_t>(ValueOf(*command->second), 1))));
                tempo_at_start = tempo_at_start || *row.tick == 0;
            }
        }
    }
    if (!tempo_at_start)
    {
        conductor.Tempo(0, 60000000 / kMp2kStartingTempo);
    }
    if (song.loop)
    {
        conductor.Meta(song.loop->start, 0x06, "[");
        conductor.Meta(song.loop->end, 0x06, "]");
    }

    return rows_of;
}

// Writes the notes of `track`, a track of `song`, to `out` on MIDI channel `channel`, at the steps and the
// transpositions of `settings`. `rows` contains the command rows of the track up to `loop_end`, the end of the loop's
// first pass, and `by_offset` each command by its offset. `index` is the track's place among those that the comparison
// uses, and `written` receives the notes that the comparison leaves out.
void WriteTrackNotes(const Sequence& song, const Track& track, const std::vector<hokuto::edit::CommandRow>& rows,
                     const TrackSettings& settings,
                     const std::map<uint32_t, const ntdlib::commands::Command*>& by_offset,
                     const std::optional<uint32_t>& loop_end, int channel, std::size_t index,
                     seqlib::midi::TrackBuilder& out, Written& written, Totals& totals)
{
    const auto& [steps, shifts, note_rows, wide_bend, track_end] = settings;

    // The notes, started or not, at the steps of their commands. A note command after the track's first voice that the
    // performer reports as neither, such as one whose note a later note replaced before the mixer started it, is
    // written as its command sets it. Before the track's first voice, a note plays nothing, and a MIDI file can't store
    // it: its notes play program 0 there.
    std::vector<std::pair<Note, bool>> notes = TrackNotes(track, note_rows, by_offset, totals);

    const auto step_of = [&steps](const Note& note)
    {
        const auto step = steps.find({note.command, note.start});
        return step != steps.end() ? step->second : 0;
    };
    const auto earlier = [&step_of](const auto& a, const auto& b)
    {
        return std::pair(a.first.start, step_of(a.first)) < std::pair(b.first.start, step_of(b.first));
    };
    std::stable_sort(notes.begin(), notes.end(), earlier);

    // Each note is written at the key that sounds: the key of its command and the transposition of its track. A note
    // outside the MIDI keys is written at the nearest key, and the comparison leaves it out.
    std::vector<int> transposed_keys;
    for (const auto& [note, started] : notes)
    {
        const auto shift = shifts.find({note.command, note.start});
        transposed_keys.push_back(note.key + (shift != shifts.end() ? shift->second : 0));
    }
    for (std::size_t n = 0; n < notes.size(); n++)
    {
        const auto& [note, started] = notes[n];
        const int transposed = transposed_keys[n];
        const auto key = static_cast<uint8_t>(std::clamp(transposed, 0, 127));
        if (key != note.key)
        {
            totals.transposed++;
            written.shifts[index][{note.start, note.key}] = key - note.key;
        }

        // A note lasts as long as its command says, and a tie as long as the arrangement shows it. A note whose
        // command lasts past the loop's end or the track's end ends there, and so does a tie that sounds there. A
        // note ends where the next note of its key starts: MIDI's note-offs end the earliest note of
        // their key first.
        const auto command = by_offset.find(note.command);
        const uint32_t command_ticks =
            command != by_offset.end() ? static_cast<uint32_t>(command->second->value.value / kOne) : 0;
        uint32_t ticks = command_ticks == 0 ? note.length : command_ticks;
        for (const std::optional<uint32_t> end : {loop_end, track_end})
        {
            if (end && command_ticks > 0)
            {
                ticks = std::min(ticks, *end - std::min(note.start, *end));
            }
        }
        const bool cut = ticks < command_ticks;
        const std::optional<std::size_t> later = NextOfKey(notes, transposed_keys, n);
        if (later && note.start + ticks > notes[*later].first.start)
        {
            ticks = notes[*later].first.start - note.start;
            totals.crossing++;
            written.cut_short = true;
            written.loose[index].insert({note.start, key});
        }
        if (note.velocity == 0 || transposed != key)
        {
            totals.silent += note.velocity == 0;
            totals.commands_left_out += transposed != key;
            written.left_out[index].insert({note.start, key});
        }
        else if (!started && ticks == 0)
        {
            // The comparison leaves the tie out. The MIDI file contains it up to its end.
            totals.unstarted_ties++;
            written.left_out[index].insert({note.start, key});
            const uint32_t tie_end = UnstartedTieEnd(note, song, rows, by_offset, loop_end, track_end);
            ticks = tie_end - std::min(note.start, tie_end);
            if (ticks == 0)
            {
                continue;
            }
        }

        // A note that the loop's end cuts to less than a frame's ticks can start and end in one of the driver's frames,
        // and never sound. Above a tick a frame, it sounded before: its command carried it into the next pass. A note
        // that the driver never started lasts the length of its command, and the cut in the MIDI file sets the new
        // command to that length. A bend range that MIDI can't store plays the track's notes at other pitches. The end
        // of a sample that plays once can then come at another time.
        const double frame_ticks =
            hokuto::song::TempoAt(song, note.start) * 60.0 / ntdlib::agb::kFrameRate / ntdlib::mp2k::kTickCounter;
        if (started && cut && loop_end && note.start + command_ticks > *loop_end &&
            ticks < std::ceil(frame_ticks - 1e-6))
        {
            totals.brief++;
            written.left_out[index].insert({note.start, key});
        }
        else if ((started && note.length < command_ticks) || (!started && cut))
        {
            totals.sooner++;
            written.loose[index].insert({note.start, key});
        }
        else if (wide_bend)
        {
            written.loose[index].insert({note.start, key});
        }
        if (song.loop && (command_ticks > 0 ? note.start + command_ticks > song.loop->end
                                            : note.start + note.length >= song.loop->end))
        {
            totals.past_loop++;
        }
        totals.notes++;
        totals.ties += ticks > 223;

        out.NoteOn(note.start, channel, key, std::max<int>(note.velocity, 1), false, step_of(note));
        out.NoteOff(note.start + ticks, channel, key);
    }
}

} // namespace

Written MidiOf(const File& file, uint32_t sequence, const Sequence& song, Totals& totals)
{
    // Each command by its offset.
    const ntdlib::commands::Sequence& commands = *file.sequences[sequence];
    const std::vector<uint32_t> offsets = hokuto::edit::Offsets(commands, hokuto::edit::ConsoleOf(file));
    const std::map<uint32_t, const ntdlib::commands::Command*> by_offset = CommandsByOffset(commands, offsets);

    // The commands that each track ran, at their ticks, up to the end of the loop's first pass. A command at the loop's
    // end or later stays out: the GBA community's converters leave out a MIDI file's events there. A tempo is written
    // as its command sets it. A song without a tempo at tick 0 starts at MusicPlayer2000's 150 beats a minute.
    Written written;
    seqlib::midi::FileBuilder builder(static_cast<uint16_t>(hokuto::edit::kGbaTimebase));
    seqlib::midi::TrackBuilder& conductor = builder.AddTrack();
    const std::optional<uint32_t> loop_end = song.loop ? std::optional(song.loop->end) : std::nullopt;
    const std::vector<std::vector<hokuto::edit::CommandRow>> rows_of =
        WriteConductor(song, commands, offsets, by_offset, loop_end, conductor);

    // A track is written when it plays a note command after its first voice, reported or not. The performer reports no
    // note of a track whose notes never sound, such as quiet notes of a PSG channel. Such a track still takes channels
    // from the tracks after it, and those tracks keep their places. The comparison uses the tracks with reported notes,
    // and `index` counts them.
    int channel = 0;
    std::size_t reported_tracks = 0;
    for (std::size_t t = 0; t < song.tracks.size(); t++)
    {
        const Track& track = song.tracks[t];
        if (!PlaysNotes(song, rows_of, by_offset, loop_end, t))
        {
            continue;
        }
        const bool reported_notes = !track.notes.empty() || !track.unstarted.empty();
        const std::size_t index = reported_notes ? reported_tracks++ : SIZE_MAX;
        totals.tracks++;
        seqlib::midi::TrackBuilder& out = builder.AddTrack();

        // The track's settings in the form that the GBA community's converters take. A program change between two notes
        // of a tick uses the step of the note after it. The other settings of a tick go before its notes.
        const TrackSettings settings = WriteTrackSettings(rows_of[t], by_offset, loop_end, channel, out, totals);
        // The track ends where its command to end runs, or else in a song without a loop where the arrangement ends.
        // The end cuts the releases of its notes.
        if (settings.track_end)
        {
            out.SetEnd(*settings.track_end);
        }
        else if (!song.loop)
        {
            out.SetEnd(song.length);
        }

        WriteTrackNotes(song, track, rows_of[t], settings, by_offset, loop_end, channel, index, out, written, totals);
        channel++;
    }

    written.midi = builder.Encode();
    return written;
}

} // namespace hokuto::tools::gba_midi
