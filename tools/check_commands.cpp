// SPDX-License-Identifier: MIT

// Checks the edits of commands (src/edit/commands.h) on a sound archive of the Wii, the 3DS, the Wii U or the Switch.
// The List Editor makes these edits. The tool also checks the new settings that the Key Editor's lanes draw
// (edit::AddSettings), the points of the arrangement's tempo row and their drags (src/app/tempo_row.h), and the moves
// of settings in time (edit::MoveSettings). It imports the archive into a Hokuto file the way the program does. It then
// makes seven rounds of edits of each sequence. Up to 256 commands of a sequence take part in the first four, spread
// over the sequence.
//
// - Each command takes its values again. The file has to stay as it was.
// - A wait of no ticks goes in before each command, and then goes again. The file has to come back as it was. The
//   round runs twice: once with the code that enters at the command running the wait first, and once without.
// - A wait of no ticks goes in before each of up to 64 commands of each sequence, in one version of the file. Code that
//   enters at a command runs the wait first. ntdlib's performer then reads each sequence sound from the exported
//   archive. Each sound has to play the notes that it played before, at the same ticks.
// - Each value that a command takes from its bytes moves a step, within the range that the console's bytes hold, in
//   one version of the file. A command without such a value moves the time over which its setting moves instead. Notes
//   keep their values: hokuto_check_edits checks them. The exported archive, imported again, has to give back the
//   edited sequences.
// - New settings go in on the tracks of the first sound of each sequence, in one version of the file: a volume, a pan
//   and a second volume in turn. Each track with notes takes up to four, at the starts of its first note and of a note
//   halfway through its notes, and halfway from each of them to the next note. Each setting goes into the last stretch
//   that starts by its tick. A setting that the lanes draw goes there too. ntdlib's performer then reads each sound of
//   those sequences from the exported archive. Each sound has to play the notes that it played before, at their ticks,
//   with their keys and velocities. A note can sound for another time: the arrangement ends a note once it fades below
//   hearing. A loop can start a whole number of passes earlier or later: the performer finds a loop where a track's
//   state repeats, and the edit can change that state. On the first sound's tracks, each new setting has to run at its
//   tick, before the notes that its stretch starts there. When the loop starts a whole number of passes earlier, the
//   rows after the edit end sooner. The tool counts the settings past their end apart. Every other command has to run
//   at the tick at which it ran before.
// - The first sound of each sequence gives the points of the tempo row. A point with a value that no later command
//   overrides in its tick has to set the tempo that ntdlib's performer plays from that tick. Each change of the tempo
//   before the end of the loop's first pass has to have a point at its tick. Up to 8 commands of each sequence then
//   take a tempo 7 BPM away through the tempo row's drag, in one version of the file. The first sound of each edited
//   sequence has to play its notes as before, and each point's tempo, the new one where its command changed. A sound
//   that draws random numbers can play other notes: the draws follow the frames, and a new tempo moves the frames. The
//   tool counts such sounds apart. So does a sound with a note that waits until the track's notes have ended: a note of
//   length 0, or of a length that the sequence decides as it plays, with the note wait on. Such a wait lasts a time
//   rather than a number of ticks. So does a sound that gains or loses only notes of a tick. A frame of the 3DS lasts
//   4.89 ms, and a tick at a new tempo can end in the frame where it starts. Each other sound of an edited sequence
//   that plays otherwise has to be among the sounds that the tempo row lists for an edited command
//   (edit::SoundsReaching). Each point with room to move then has to move as far as its room allows, earlier and later,
//   alone. Up to four points of each sequence then move half as far, in another version of the file. Every sound of
//   those sequences has to play its notes as before, unless it draws random numbers, has a note that waits until the
//   track's notes have ended, or gains or loses only notes of a tick. The first sound has to run each moved command at
//   its new tick.
// - Settings move in time on the tracks of the first sound of each sequence, in one version of the file. Each track
//   moves up to four commands that MoveSettings can move, spread over the commands that it ran. A command whose move
//   can change the notes stays out, such as a transpose, a program change or a random value. So does each track that
//   ties its notes or plays them monophonically. Each moves half as far as LimitsOf allows: later and earlier in turn,
//   or the other way where it has no room. ntdlib's performer then reads each sound of those sequences from the
//   exported archive. Each sound has to play its notes as before, as in the fifth round. A sound with a note that waits
//   until the track's notes have ended counts apart: a setting of the envelope that moves past a note changes when that
//   note ends. On the first sound's tracks, each moved command has to run at its tick and the move's, before the notes
//   that its stretch starts there. A move past the tick where the performer stops reading the sound stays out. Every
//   other command has to run at the tick at which it ran before.
//
// A sequence whose bytes keep their places refuses new commands, removals and changes of a command's size. A new
// setting outside the blocks of its stretch is refused too. The tool counts these refusals apart, and prints the number
// of each message of the new settings' refusals. A move inside the limits that LimitsOf gives can't be refused. It
// prints a line for each problem, and a summary. It returns 1 when a sequence or a sound differs, or when an edit, a
// read or the export fails. With a sound's name, it checks the sound's sequence alone. With --tempo, it makes the sixth
// round alone, and with --moves the seventh.
//
// With --model, the file goes into the model form before the edits (format::InModelForm), and the archive that the
// model builds has to be the game's. Each file that the edits make then has to save in both forms and open again as it
// was (tools/checked_archive.h).
//
// usage: hokuto_check_commands [--model] [--tempo | --moves] <archive> [sound]

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/commands/units.h"
#include "ntdlib/nwsnd/units.h"
#include "xsflib/console.h"
#include "xsflib/files.h"

#include "app/song_view.h"
#include "app/tempo_row.h"
#include "checked_archive.h"
#include "edit/bytecode.h"
#include "edit/commands.h"
#include "edit/notes.h"
#include "edit/reach.h"
#include "edit/replace.h"
#include "format/archive.h"
#include "format/file.h"
#include "song/archive.h"
#include "song/sequence.h"

namespace
{

using hokuto::app::TempoPointView;
using hokuto::edit::CommandPlace;
using hokuto::edit::Edited;
using hokuto::format::File;
using hokuto::tools::CheckedArchive;
using hokuto::tools::CheckedSound;
using ntdlib::commands::Command;
using ntdlib::commands::Kind;
using ntdlib::commands::kOne;
using ntdlib::commands::Source;

// The commands of a sequence that take part in the rounds of single edits, the waits of the third round, the commands
// that take a new tempo in the sixth, and the commands of each track that move in the seventh.
constexpr std::size_t kMostCommands = 256;
constexpr std::size_t kMostWaits = 64;
constexpr std::size_t kMostTempos = 8;
constexpr std::size_t kMostMoves = 4;

// The fewest ticks that a note lasts in the sixth round's comparisons that a sound gains or loses only notes of a tick.
// A new tempo changes how many ticks a frame runs. The models of the consoles can then end a note of a tick in the
// frame where it starts, and the arrangement leaves such a note out.
constexpr uint32_t kTickNotes = 2;

// What the check counted, for the summary.
struct Totals
{
    std::size_t sequences = 0;
    std::size_t fixed = 0;
    std::size_t kept = 0;
    std::size_t round_trips = 0;
    std::size_t waits = 0;
    std::size_t sounds = 0;
    std::size_t notes = 0;
    std::size_t values = 0;
    std::size_t times = 0;
    std::size_t refused = 0;
    std::size_t settings = 0;
    std::size_t setting_sequences = 0;
    std::size_t settings_refused = 0;
    std::size_t setting_sounds = 0;
    std::size_t setting_notes = 0;
    std::size_t moved_loops = 0;
    std::size_t settings_past = 0;
    std::size_t tempo_sequences = 0;
    std::size_t tempo_points = 0;
    std::size_t tempo_sources = 0;
    std::size_t tempo_overridden = 0;
    std::size_t tempo_conditional = 0;
    std::size_t tempo_unpointed = 0;
    std::size_t tempo_edits = 0;
    std::size_t tempo_sounds = 0;
    std::size_t tempo_random = 0;
    std::size_t tempo_waits = 0;
    std::size_t tempo_others = 0;
    std::size_t tempo_listed = 0;
    std::size_t tempo_reached = 0;
    std::size_t tempo_moves = 0;
    std::size_t tempo_farthest = 0;
    std::size_t tempo_move_sounds = 0;
    std::size_t tempo_move_random = 0;
    std::size_t tempo_move_waits = 0;
    std::size_t tempo_move_short = 0;
    std::size_t tempo_short = 0;
    std::size_t moves = 0;
    std::size_t move_sequences = 0;
    std::size_t move_sounds = 0;
    std::size_t move_notes = 0;
    std::size_t move_waits = 0;
    std::size_t move_loops = 0;
    std::size_t problems = 0;
};

// The new settings of the fifth round in one sequence: the sound on whose tracks they went, and the track and the tick
// of each. `items` holds the new item of each item of the sequence, and `added` the items of the new settings.
struct Placed
{
    uint32_t sound = 0;
    std::vector<int> tracks;
    std::vector<uint32_t> ticks;
    std::vector<std::optional<uint32_t>> items;
    std::vector<uint32_t> added;
};

// The moves of the seventh round in one sequence: the sound whose tracks they took, the shift of each moved command by
// its offset, and the new item of each item of the sequence. The sixth round moves points of the tempo row the same
// way.
struct Moved
{
    uint32_t sound = 0;
    std::map<uint32_t, int32_t> shifts;
    std::vector<std::optional<uint32_t>> items;
};

// What the tempo row shows of a sequence in the sixth round: the first sound that ntdlib's performer reads, the read,
// and the points.
struct TempoShown
{
    uint32_t sound = 0;
    hokuto::song::Sequence song;
    std::vector<TempoPointView> points;
};

// Up to `most` items of `sequence`, spread evenly over it, without bytes that no command reads.
std::vector<uint32_t> Chosen(const ntdlib::commands::Sequence& sequence, std::size_t most)
{
    std::vector<uint32_t> chosen;
    const std::size_t count = sequence.items.size();
    const std::size_t step = std::max<std::size_t>(1, (count + most - 1) / most);
    for (std::size_t item = 0; item < count; item += step)
    {
        if (sequence.items[item].kind != Kind::kData)
        {
            chosen.push_back(static_cast<uint32_t>(item));
        }
    }

    return chosen;
}

// A wait of no ticks.
Command NoWait()
{
    Command wait;
    wait.kind = Kind::kWait;

    return wait;
}

// The place of item `item` of sequence `sequence` of `file`.
CommandPlace PlaceOf(const File& file, uint32_t sequence, uint32_t item)
{
    return {hokuto::edit::Offsets(*file.sequences[sequence], hokuto::edit::ConsoleOf(file)).at(item), {}};
}

// `command` with its value a step from the value that it has, or with its time a step from its time when its value
// can't move. A step is the console's smallest change. Nothing for a command with neither.
std::optional<Command> Stepped(const hokuto::song::Console& console, const Command& command)
{
    if (command.kind == Kind::kNote || command.kind == Kind::kData || command.kind == Kind::kOther)
    {
        return std::nullopt;
    }

    const auto step = [&](const ntdlib::commands::Argument& argument, const ntdlib::nwsnd::Scale& scale,
                          bool time) -> std::optional<int64_t>
    {
        const std::optional<hokuto::edit::ValueRange> range =
            hokuto::edit::ArgumentRange(console, command, Source::kValue, time);
        if (argument.source != Source::kValue || !range)
        {
            return std::nullopt;
        }

        const int64_t value = ntdlib::nwsnd::FromUnit(argument.value, scale);
        for (const int64_t next : {value + 1, value - 1})
        {
            const ntdlib::commands::Fixed unit = ntdlib::nwsnd::ToUnit(next, scale);
            if (unit != argument.value && unit >= range->lowest && unit <= range->highest)
            {
                return unit;
            }
        }

        return std::nullopt;
    };

    Command stepped = command;
    if (ntdlib::commands::UnitOf(command.kind))
    {
        const ntdlib::nwsnd::Scale scale = ntdlib::nwsnd::ScaleOf(command.kind, hokuto::song::GrammarOf(console));
        if (const std::optional<int64_t> value = step(command.value, scale, false))
        {
            stepped.value.value = *value;
            return stepped;
        }
    }
    if (command.time)
    {
        if (const std::optional<int64_t> time = step(*command.time, ntdlib::nwsnd::kTicksScale, true))
        {
            stepped.time->value = *time;
            return stepped;
        }
    }

    return std::nullopt;
}

// `command` without what its bytes held beyond its meaning.
Command WithoutEncoding(Command command)
{
    command.encoding = {};

    return command;
}

// The notes that a sound plays: each one's track, start, key, length and velocity.
using Played = std::tuple<int, uint32_t, uint8_t, uint32_t, uint8_t>;

// The notes of `sequence`, in order.
std::vector<Played> NotesOf(const hokuto::song::Sequence& sequence)
{
    std::vector<Played> notes;
    for (const hokuto::song::Track& track : sequence.tracks)
    {
        for (const hokuto::song::Note& note : track.notes)
        {
            notes.emplace_back(track.number, note.start, note.key, note.length, note.velocity);
        }
    }
    std::sort(notes.begin(), notes.end());

    return notes;
}

// The `count`th new setting of the fifth round: a volume, a pan or a second volume, in turn. None of them changes a
// note.
Command SettingNumbered(std::size_t count)
{
    constexpr std::array<std::pair<Kind, int>, 3> kSettings = {
        {{Kind::kVolume, 77}, {Kind::kPan, 20}, {Kind::kVolume2, 90}}};
    const auto& [kind, value] = kSettings[count % kSettings.size()];
    Command command;
    command.kind = kind;
    command.value = ntdlib::commands::Argument{Source::kValue, value * kOne, 0};

    return command;
}

// The ticks of the fifth round's settings on `track`: the starts of its first note and of a note halfway through its
// notes, and the ticks halfway from each of them to the next note that starts later.
std::vector<uint32_t> SettingTicks(const hokuto::song::Track& track)
{
    const std::vector<hokuto::song::Note>& notes = track.notes;
    std::set<uint32_t> ticks;
    for (const std::size_t index : {std::size_t{0}, notes.size() / 2})
    {
        if (index >= notes.size())
        {
            continue;
        }

        const uint32_t start = notes[index].start;
        const auto later = [start](const hokuto::song::Note& note)
        {
            return note.start > start;
        };
        const auto next = std::find_if(notes.begin() + static_cast<std::ptrdiff_t>(index), notes.end(), later);
        ticks.insert(start);
        if (next != notes.end() && next->start > start + 1)
        {
            ticks.insert(start + (next->start - start) / 2);
        }
    }

    return {ticks.begin(), ticks.end()};
}

// Whether `after` plays the notes of `before`: each note at its start, with its key and velocity. A new setting can
// change how long a note sounds: the arrangement ends a note once it fades below hearing. ntdlib's performer finds a
// loop where a track's state repeats, and a new setting or a wait of its own can change the state that it compares. The
// loop can then start a whole number of passes earlier or later, and the arrangement shows the passes before it. Up to
// the earlier loop's end, the notes then have to start as before. `moved` becomes true when the loop moved. Notes
// shorter than `shortest` ticks don't count.
bool SamePlaying(const hokuto::song::Sequence& before, const hokuto::song::Sequence& after, bool& moved,
                 uint32_t shortest = 0)
{
    moved = after.loop != before.loop;
    if (moved)
    {
        if (!before.loop || !after.loop)
        {
            return false;
        }

        const uint32_t period = before.loop->end - before.loop->start;
        const uint32_t shift = after.loop->start > before.loop->start ? after.loop->start - before.loop->start
                                                                      : before.loop->start - after.loop->start;
        if (period == 0 || after.loop->end - after.loop->start != period || shift % period != 0)
        {
            return false;
        }
    }

    const uint32_t end =
        before.loop ? std::min(before.loop->end, after.loop->end) : std::numeric_limits<uint32_t>::max();
    const auto starts = [end, shortest](const hokuto::song::Sequence& song)
    {
        std::vector<std::tuple<int, uint32_t, uint8_t, uint8_t>> notes;
        for (const auto& [track, start, key, length, velocity] : NotesOf(song))
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

// The rows of the track numbered `number` of `song` up to the end of the loop's first pass. Sequence `sequence` of
// `file` played `song`. None for a track that ran no commands.
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

// Whether a track of `song` ran a note that waits until the track's notes have ended: a note of length 0, or of a
// length that the sequence decides as it plays, with the note wait on. The wait lasts a time rather than a number of
// ticks, and a new tempo changes the ticks that it takes. Sequence `sequence` of `file` played `song`.
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

// The first round: each chosen command of sequence `sequence` takes its values again.
void KeepValues(const std::string& name, const File& file, uint32_t sequence, const std::vector<uint32_t>& chosen,
                Totals& totals)
{
    const ntdlib::commands::Sequence& commands = *file.sequences[sequence];
    const std::vector<uint32_t> offsets = hokuto::edit::Offsets(commands, hokuto::edit::ConsoleOf(file));
    for (const uint32_t item : chosen)
    {
        try
        {
            const Edited edited =
                hokuto::edit::ChangeCommand(file, sequence, {offsets[item], {}}, commands.items[item]);
            if (!(edited.file == file))
            {
                std::printf("%s: sequence %u: the command at %u changes when it takes its values again\n", name.c_str(),
                            sequence, offsets[item]);
                ++totals.problems;
            }
        }
        catch (const std::exception& error)
        {
            std::printf("%s: sequence %u: the command at %u refuses its values: %s\n", name.c_str(), sequence,
                        offsets[item], error.what());
            ++totals.problems;
        }
        ++totals.kept;
    }
}

// The second round: a wait goes in before each chosen command of sequence `sequence`, and goes again.
void InsertAndRemove(const std::string& name, const File& file, uint32_t sequence, const std::vector<uint32_t>& chosen,
                     Totals& totals)
{
    for (const uint32_t item : chosen)
    {
        for (const bool entered : {false, true})
        {
            try
            {
                const Edited inserted =
                    hokuto::edit::InsertCommand(file, sequence, PlaceOf(file, sequence, item), entered, NoWait());
                const CommandPlace wait = PlaceOf(inserted.file, sequence, inserted.added.at(0));
                const Edited removed = hokuto::edit::RemoveCommands(inserted.file, sequence, {&wait, 1});
                if (!(removed.file == file))
                {
                    std::printf("%s: sequence %u: a wait before item %u%s doesn't go again cleanly\n", name.c_str(),
                                sequence, item, entered ? " that code enters" : "");
                    ++totals.problems;
                }
            }
            catch (const std::exception& error)
            {
                std::printf("%s: sequence %u: a wait can't go in before item %u and go again: %s\n", name.c_str(),
                            sequence, item, error.what());
                ++totals.problems;
            }
            ++totals.round_trips;
        }
    }
}

// The fifth round's check of the commands that `before` and `after` ran, the sound of `placed` before and after its
// settings went into sequence `sequence`. `file` holds the sequence before, and `edited` after. Each new setting has to
// run at its tick, before the notes that its stretch starts there. When the loop starts a whole number of passes
// earlier, the rows after the edit end sooner. A setting past their end stays out, and `past` counts it. Every other
// command has to run at its tick. Gives the number of problems.
std::size_t CheckRows(const std::string& name, const File& file, const File& edited, uint32_t sequence,
                      const hokuto::song::Sequence& before, const hokuto::song::Sequence& after, const Placed& placed,
                      std::size_t& past)
{
    const hokuto::song::Console console = hokuto::edit::ConsoleOf(file);
    const std::vector<uint32_t> old_offsets = hokuto::edit::Offsets(*file.sequences[sequence], console);
    const ntdlib::commands::Sequence& commands = *edited.sequences[sequence];
    const std::vector<uint32_t> offsets = hokuto::edit::Offsets(commands, console);
    std::size_t problems = 0;

    // Each new setting, before the notes of its stretch at its tick.
    const uint32_t end = after.loop ? after.loop->end : std::numeric_limits<uint32_t>::max();
    for (std::size_t setting = 0; setting < placed.ticks.size(); ++setting)
    {
        const uint32_t offset = offsets.at(placed.added.at(setting));
        const uint32_t tick = placed.ticks[setting];
        if (tick >= end)
        {
            ++past;
            continue;
        }
        const std::vector<hokuto::edit::CommandRow> rows = RowsOf(edited, sequence, after, placed.tracks[setting]);
        const auto ran = [offset, tick](const hokuto::edit::CommandRow& row)
        {
            return row.command == offset && row.tick == tick;
        };
        const auto row = std::find_if(rows.begin(), rows.end(), ran);
        bool late = row == rows.end();
        for (auto earlier = rows.begin(); !late && earlier != row; ++earlier)
        {
            const std::optional<uint32_t> item = hokuto::edit::ItemAtOffset(offsets, earlier->command);
            late = earlier->stretch == row->stretch && earlier->tick == tick && item &&
                   commands.items.at(*item).kind == Kind::kNote;
        }
        if (late)
        {
            std::printf(
                "%s: sequence %u: the new setting at tick %u of track %d doesn't run at its tick before its "
                "notes\n",
                name.c_str(), sequence, tick, placed.tracks[setting]);
            ++problems;
        }
    }

    // Every other command, at its tick. A command whose tick isn't known before can run at any tick. A wait that a new
    // setting splits keeps its item in the piece after the setting. When the loop ends earlier, the rows after the edit
    // stop at its end.
    const ntdlib::commands::Sequence& old_commands = *file.sequences[sequence];
    for (const hokuto::song::Track& track : before.tracks)
    {
        std::multiset<std::pair<uint32_t, uint32_t>> ran;
        for (const hokuto::edit::CommandRow& row : RowsOf(edited, sequence, after, track.number))
        {
            if (row.tick)
            {
                ran.emplace(row.command, *row.tick);
            }
        }
        for (const hokuto::edit::CommandRow& row : RowsOf(file, sequence, before, track.number))
        {
            const std::optional<uint32_t> old_item = hokuto::edit::ItemAtOffset(old_offsets, row.command);
            if (!row.tick || *row.tick >= end || !old_item || old_commands.items.at(*old_item).kind == Kind::kWait)
            {
                continue;
            }

            const std::optional<uint32_t> item = placed.items.at(*old_item);
            const auto found = item ? ran.find({offsets.at(*item), *row.tick}) : ran.end();
            if (found == ran.end())
            {
                std::printf(
                    "%s: sequence %u: the command at %u of track %d no longer runs at tick %u after the new "
                    "settings\n",
                    name.c_str(), sequence, row.command, track.number, *row.tick);
                ++problems;
                break;
            }
            ran.erase(found);
        }
    }

    return problems;
}

// The fifth round: new settings on the tracks of the first sound of each sequence in `songs`, in one version of the
// file. `songs` holds the sounds as they played before, and `sequences` the sequence of each file of `archive`.
void NewSettings(const std::string& name, const File& file, const CheckedArchive& archive,
                 const std::vector<std::optional<uint32_t>>& sequences,
                 const std::map<uint32_t, hokuto::song::Sequence>& songs, Totals& totals)
{
    // Each setting goes in by itself first. Those that go in by themselves then go in together.
    File edited = file;
    std::map<uint32_t, Placed> placed;
    std::set<uint32_t> tried;
    std::map<std::string, std::size_t> refusals;
    std::size_t count = 0;
    for (const auto& [sound, song] : songs)
    {
        const uint32_t sequence = *sequences[archive.FileOf(sound)];
        if (!tried.insert(sequence).second)
        {
            continue;
        }

        Placed place{.sound = sound};
        std::vector<hokuto::edit::NewSetting> settings;
        for (const hokuto::song::Track& track : song.tracks)
        {
            for (const uint32_t tick : SettingTicks(track))
            {
                const std::optional<hokuto::song::Stretch> stretch = hokuto::edit::StretchAt(track.stretches, tick);
                if (!stretch)
                {
                    continue;
                }

                const hokuto::edit::NewSetting setting{*stretch, tick, SettingNumbered(count++)};
                try
                {
                    hokuto::edit::AddSettings(file, sequence, {&setting, 1});
                    settings.push_back(setting);
                    place.tracks.push_back(track.number);
                    place.ticks.push_back(tick);
                }
                catch (const std::invalid_argument& error)
                {
                    ++refusals[error.what()];
                    ++totals.settings_refused;
                }
            }
        }
        if (settings.empty())
        {
            continue;
        }

        try
        {
            hokuto::edit::Edited added = hokuto::edit::AddSettings(edited, sequence, settings);
            edited = std::move(added.file);
            place.items = std::move(added.items);
            place.added = std::move(added.added);
            totals.settings += settings.size();
            ++totals.setting_sequences;
            placed.emplace(sequence, std::move(place));
        }
        catch (const std::exception& error)
        {
            std::printf("%s: sequence %u: %zu new settings go in one at a time but not together: %s\n", name.c_str(),
                        sequence, settings.size(), error.what());
            ++totals.problems;
        }
    }
    for (const auto& [message, refused] : refusals)
    {
        std::printf("%s: %zu new settings were refused: %s\n", name.c_str(), refused, message.c_str());
    }

    // Each sound of those sequences plays its notes again from the archive with the settings.
    CheckedArchive after;
    try
    {
        hokuto::tools::CheckSaved(edited);
        after = hokuto::tools::BuildChecked(edited, name);
    }
    catch (const std::exception& error)
    {
        std::printf("%s: the file with the new settings can't be exported: %s\n", name.c_str(), error.what());
        ++totals.problems;
        return;
    }
    for (const auto& [sound, before] : songs)
    {
        const CheckedSound info = archive.Info(sound);
        const uint32_t sequence = *sequences[info.file_id];
        const auto place = placed.find(sequence);
        if (place == placed.end())
        {
            continue;
        }

        ++totals.setting_sounds;
        try
        {
            const hokuto::song::Sequence read = after.Read(sound);
            const std::vector<Played> notes = NotesOf(before);
            totals.setting_notes += notes.size();
            bool moved = false;
            if (!SamePlaying(before, read, moved))
            {
                std::printf("%s: %s plays other notes after the new settings: %zu notes before, %zu after\n",
                            name.c_str(), info.name.c_str(), notes.size(), NotesOf(read).size());
                ++totals.problems;
                continue;
            }

            totals.moved_loops += moved ? 1 : 0;
            if (place->second.sound == sound)
            {
                totals.problems +=
                    CheckRows(name, file, edited, sequence, before, read, place->second, totals.settings_past);
            }
        }
        catch (const std::exception& error)
        {
            std::printf("%s: %s can't be read after the new settings: %s\n", name.c_str(), info.name.c_str(),
                        error.what());
            ++totals.problems;
        }
    }
}

// Whether a move of `command` can change the notes that a sound plays: a transpose changes their keys, and a velocity
// range their velocities. A program change and a bank select change their instrument, and a note without a region in
// the instrument doesn't sound. A tie and a mute change which notes start. A tempo moves the frames that random draws
// follow, and a random value changes the numbers that later draws give.
bool ChangesNotes(const Command& command)
{
    const bool random =
        command.value.source == Source::kRandom || (command.time && command.time->source == Source::kRandom);
    return random || command.kind == Kind::kTranspose || command.kind == Kind::kVelocityRange ||
           command.kind == Kind::kProgram || command.kind == Kind::kBankSelect || command.kind == Kind::kTie ||
           command.kind == Kind::kMute || command.kind == Kind::kTempo;
}

// Whether `command` can turn on the tie or the monophonic mode. A note then can continue the channel of the note before
// it, and a setting that moves between them can change whether the console continues the channel or starts another.
bool Legato(const Command& command)
{
    const bool off = command.value.source == Source::kValue && command.value.value == 0;
    return (command.kind == Kind::kTie || command.kind == Kind::kMonophonic) && !off;
}

// The seventh round's moves on the tracks of `song`, the first sound of sequence `sequence` of `file`. Each track moves
// up to kMostMoves commands that MoveSettings can move, spread over the commands that it ran, each once. A command
// whose move can change the notes stays out (ChangesNotes), and so does a track that runs a command of Legato. Each
// moves half as far as LimitsOf allows: later and earlier in turn, or the other way where it has no room.
std::vector<hokuto::edit::SettingMove> MovesOf(const File& file, uint32_t sequence, const hokuto::song::Sequence& song)
{
    const ntdlib::commands::Sequence& commands = *file.sequences[sequence];
    const std::vector<uint32_t> offsets = hokuto::edit::Offsets(commands, hokuto::edit::ConsoleOf(file));
    std::vector<hokuto::edit::SettingMove> moves;
    std::set<uint32_t> taken;
    for (const hokuto::song::Track& track : song.tracks)
    {
        const std::vector<hokuto::edit::CommandRow> rows = RowsOf(file, sequence, song, track.number);
        const auto legato = [&commands, &offsets](const hokuto::edit::CommandRow& row)
        {
            const std::optional<uint32_t> item = hokuto::edit::ItemAtOffset(offsets, row.command);
            return item && Legato(commands.items.at(*item));
        };
        if (std::any_of(rows.begin(), rows.end(), legato))
        {
            continue;
        }

        std::vector<hokuto::edit::NotePlace> places;
        for (const hokuto::edit::CommandRow& row : rows)
        {
            const std::optional<uint32_t> item = hokuto::edit::ItemAtOffset(offsets, row.command);
            if (row.tick && item && !hokuto::edit::WhyFixedInTime(commands.items.at(*item)) &&
                !ChangesNotes(commands.items.at(*item)) && taken.insert(row.command).second)
            {
                places.push_back({row.command, track.stretches.at(row.stretch)});
            }
        }
        if (places.empty())
        {
            continue;
        }

        std::vector<hokuto::edit::NotePlace> chosen;
        const std::size_t count = std::min(places.size(), kMostMoves);
        for (std::size_t i = 0; i < count; ++i)
        {
            chosen.push_back(places[i * places.size() / count]);
        }
        const std::vector<hokuto::edit::NoteLimits> limits = hokuto::edit::LimitsOf(file, sequence, chosen);
        for (std::size_t i = 0; i < chosen.size(); ++i)
        {
            const std::optional<hokuto::edit::ShiftRange>& range = limits[i].shift;
            if (!range || (range->earlier == 0 && range->later == 0))
            {
                continue;
            }

            const bool later = range->earlier == 0 || (range->later > 0 && moves.size() % 2 == 0);
            const int64_t shift = later ? (int64_t{range->later} + 1) / 2 : -(int64_t{range->earlier} + 1) / 2;
            moves.push_back({chosen[i].command, chosen[i].stretch, static_cast<int32_t>(shift)});
        }
    }

    return moves;
}

// The seventh round's check of the commands that `before` and `after` ran, the sound of `moved` before and after the
// moves in sequence `sequence`. `file` holds the sequence before, and `edited` after. Each moved command has to run at
// its tick and the move's, before the notes that its stretch starts there. Every other command has to run at its tick.
// A wait that a move splits or joins can run at other ticks. When the loop ends earlier, the rows after the edit stop
// at its end. They also stop where the performer stops reading the sound, and a move past that tick stays out. Gives
// the number of problems.
std::size_t CheckMovedRows(const std::string& name, const File& file, const File& edited, uint32_t sequence,
                           const hokuto::song::Sequence& before, const hokuto::song::Sequence& after,
                           const Moved& moved)
{
    const hokuto::song::Console console = hokuto::edit::ConsoleOf(file);
    const ntdlib::commands::Sequence& old_commands = *file.sequences[sequence];
    const std::vector<uint32_t> old_offsets = hokuto::edit::Offsets(old_commands, console);
    const ntdlib::commands::Sequence& commands = *edited.sequences[sequence];
    const std::vector<uint32_t> offsets = hokuto::edit::Offsets(commands, console);
    const uint32_t end = after.loop ? after.loop->end : std::numeric_limits<uint32_t>::max();
    std::size_t problems = 0;
    for (const hokuto::song::Track& track : before.tracks)
    {
        const std::vector<hokuto::edit::CommandRow> rows = RowsOf(edited, sequence, after, track.number);
        std::multiset<std::pair<uint32_t, uint32_t>> ran;
        for (const hokuto::edit::CommandRow& row : rows)
        {
            if (row.tick)
            {
                ran.emplace(row.command, *row.tick);
            }
        }
        for (const hokuto::edit::CommandRow& row : RowsOf(file, sequence, before, track.number))
        {
            const std::optional<uint32_t> old_item = hokuto::edit::ItemAtOffset(old_offsets, row.command);
            if (!row.tick || !old_item || old_commands.items.at(*old_item).kind == Kind::kWait)
            {
                continue;
            }

            const auto shift = moved.shifts.find(row.command);
            const int64_t tick = int64_t{*row.tick} + (shift != moved.shifts.end() ? shift->second : 0);
            const std::optional<uint32_t> item = moved.items.at(*old_item);
            if (*row.tick >= end || tick >= end || tick >= after.length || !item)
            {
                continue;
            }

            const uint32_t offset = offsets.at(*item);
            const auto found = ran.find({offset, static_cast<uint32_t>(tick)});
            if (found == ran.end())
            {
                std::printf("%s: sequence %u: the command at %u of track %d doesn't run at tick %lld after the moves\n",
                            name.c_str(), sequence, row.command, track.number, static_cast<long long>(tick));
                ++problems;
                break;
            }
            ran.erase(found);
            if (shift == moved.shifts.end())
            {
                continue;
            }

            // A moved command runs before the notes that its stretch starts at its new tick.
            const auto at = [offset, tick](const hokuto::edit::CommandRow& other)
            {
                return other.command == offset && other.tick == static_cast<uint32_t>(tick);
            };
            const auto run = std::find_if(rows.begin(), rows.end(), at);
            for (auto earlier = rows.begin(); earlier != run; ++earlier)
            {
                const std::optional<uint32_t> other = hokuto::edit::ItemAtOffset(offsets, earlier->command);
                if (earlier->stretch == run->stretch && earlier->tick == run->tick && other &&
                    commands.items.at(*other).kind == Kind::kNote)
                {
                    std::printf("%s: sequence %u: the moved command at %u of track %d runs after a note at tick %lld\n",
                                name.c_str(), sequence, row.command, track.number, static_cast<long long>(tick));
                    ++problems;
                    break;
                }
            }
        }
    }

    return problems;
}

// The seventh round: settings move in time on the tracks of the first sound of each sequence in `songs`, in one version
// of the file (MovesOf). `songs` holds the sounds as they played before, and `sequences` the sequence of each file of
// `archive`. Each move goes by itself first, and then the moves of every sequence go together.
void SettingMoves(const std::string& name, const File& file, const CheckedArchive& archive,
                  const std::vector<std::optional<uint32_t>>& sequences,
                  const std::map<uint32_t, hokuto::song::Sequence>& songs, Totals& totals)
{
    File edited = file;
    std::map<uint32_t, Moved> moved;
    std::set<uint32_t> tried;
    for (const auto& [sound, song] : songs)
    {
        const uint32_t sequence = *sequences[archive.FileOf(sound)];
        if (!tried.insert(sequence).second)
        {
            continue;
        }

        std::vector<hokuto::edit::SettingMove> moves;
        Moved place{.sound = sound};
        for (const hokuto::edit::SettingMove& move : MovesOf(file, sequence, song))
        {
            try
            {
                hokuto::edit::MoveSettings(file, sequence, {&move, 1});
                moves.push_back(move);
                place.shifts.emplace(move.command, move.shift);
            }
            catch (const std::invalid_argument& error)
            {
                std::printf("%s: sequence %u: the command at %u can't move by %d ticks inside its limits: %s\n",
                            name.c_str(), sequence, move.command, move.shift, error.what());
                ++totals.problems;
            }
        }
        if (moves.empty())
        {
            continue;
        }

        try
        {
            hokuto::edit::Edited result = hokuto::edit::MoveSettings(edited, sequence, moves);
            edited = std::move(result.file);
            place.items = std::move(result.items);
            totals.moves += moves.size();
            ++totals.move_sequences;
            moved.emplace(sequence, std::move(place));
        }
        catch (const std::exception& error)
        {
            std::printf("%s: sequence %u: %zu settings move one at a time but not together: %s\n", name.c_str(),
                        sequence, moves.size(), error.what());
            ++totals.problems;
        }
    }

    // Each sound of those sequences plays its notes again from the archive with the moves.
    CheckedArchive after;
    try
    {
        hokuto::tools::CheckSaved(edited);
        after = hokuto::tools::BuildChecked(edited, name);
    }
    catch (const std::exception& error)
    {
        std::printf("%s: the file with the moved settings can't be exported: %s\n", name.c_str(), error.what());
        ++totals.problems;
        return;
    }
    for (const auto& [sound, before] : songs)
    {
        const CheckedSound info = archive.Info(sound);
        const uint32_t sequence = *sequences[info.file_id];
        const auto place = moved.find(sequence);
        if (place == moved.end())
        {
            continue;
        }

        ++totals.move_sounds;
        try
        {
            const hokuto::song::Sequence read = after.Read(sound);
            totals.move_notes += NotesOf(before).size();
            bool loop_moved = false;
            if (!SamePlaying(before, read, loop_moved) && WaitsForNotes(file, sequence, before))
            {
                ++totals.move_waits;
                continue;
            }
            if (!SamePlaying(before, read, loop_moved))
            {
                std::printf("%s: %s plays other notes after the moves: %zu notes before, %zu after\n", name.c_str(),
                            info.name.c_str(), NotesOf(before).size(), NotesOf(read).size());
                ++totals.problems;
                continue;
            }

            totals.move_loops += loop_moved ? 1 : 0;
            if (place->second.sound == sound)
            {
                totals.problems += CheckMovedRows(name, file, edited, sequence, before, read, place->second);
            }
        }
        catch (const std::exception& error)
        {
            std::printf("%s: %s can't be read after the moves: %s\n", name.c_str(), info.name.c_str(), error.what());
            ++totals.problems;
        }
    }
}

// Prints the seventh round's figures.
void PrintMoveTotals(const std::string& name, const Totals& totals)
{
    std::printf(
        "%s: %zu settings moved in %zu sequences. %zu sounds played their %zu notes again. In %zu of them, the loop "
        "moved by a whole number of passes. %zu sounds with notes that wait for the track's notes to end played other "
        "notes.\n",
        name.c_str(), totals.moves, totals.move_sequences, totals.move_sounds, totals.move_notes, totals.move_loops,
        totals.move_waits);
}

// The reads of ntdlib's performer of the sounds that play the sequences of `checked`, by the sounds' numbers
// (CheckedArchive::Read): each sound named `only`, or every sound when `only` is empty. The performer reads them from
// `archive`. `sequences` gives the sequence of each file. A sound that the performer can't read stays out.
std::map<uint32_t, hokuto::song::Sequence> SongsOf(const CheckedArchive& archive,
                                                   const std::vector<std::optional<uint32_t>>& sequences,
                                                   const std::set<uint32_t>& checked, const std::string& only)
{
    std::map<uint32_t, hokuto::song::Sequence> songs;
    for (uint32_t sound = 0; sound < archive.SoundCount(); ++sound)
    {
        const CheckedSound info = archive.Info(sound);
        if (!info.sequence || (!only.empty() && info.name != only) || info.file_id >= sequences.size() ||
            !sequences[info.file_id] || !checked.contains(*sequences[info.file_id]))
        {
            continue;
        }

        try
        {
            songs.emplace(sound, archive.Read(sound));
        }
        catch (const std::exception&)
        {
            continue;
        }
    }

    return songs;
}

// Prints the sixth round's figures.
void PrintTempoTotals(const std::string& name, const Totals& totals)
{
    std::printf(
        "%s: the tempo rows of %zu sequences held %zu points with a value and %zu without. Later commands overrode %zu "
        "of the first in their ticks, and %zu points ran only if a comparison held. %zu changes of the tempo had no "
        "point. %zu commands took a new tempo, and %zu sounds played them. %zu sounds that draw random numbers played "
        "other notes, %zu with notes that wait for the track's notes to end, and %zu others gained or lost notes of a "
        "tick. %zu other sounds played the edited sequences. The tempo rows listed %zu of them, and %zu of those "
        "played otherwise. %zu points moved as far as they could, alone. %zu moved half as far, and %zu sounds played "
        "them. %zu sounds that draw random numbers played other notes after the moves, %zu with notes that wait for "
        "the track's notes to end, and %zu others gained or lost notes of a tick.\n",
        name.c_str(), totals.tempo_sequences, totals.tempo_points, totals.tempo_sources, totals.tempo_overridden,
        totals.tempo_conditional, totals.tempo_unpointed, totals.tempo_edits, totals.tempo_sounds, totals.tempo_random,
        totals.tempo_waits, totals.tempo_short, totals.tempo_others, totals.tempo_listed, totals.tempo_reached,
        totals.tempo_farthest, totals.tempo_moves, totals.tempo_move_sounds, totals.tempo_move_random,
        totals.tempo_move_waits, totals.tempo_move_short);
}

// The sixth round's moves of the tempo row's points (`shown`, by sequence), in another version of `file` than its
// drags. Each point with room has to move as far as its room allows, earlier and later, alone. In each sequence, up to
// kMostMoves points then move half as far as their room allows, together: later where a point has more room later, and
// earlier otherwise. Every sound of a sequence with moves has to play its notes as before, at their ticks, from the
// exported archive. A sound that draws random numbers and plays other notes is counted apart. So is a sound that gains
// or loses only notes of a tick (kTickNotes). The first sound has to run each moved command at its tick and the move's,
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
            for (const int32_t farthest : {-static_cast<int32_t>(point.earlier), static_cast<int32_t>(point.later)})
            {
                const hokuto::edit::SettingMove move{point.command, stretch, farthest};
                try
                {
                    if (farthest != 0)
                    {
                        hokuto::edit::MoveSettings(file, sequence, std::span(&move, 1));
                        ++totals.tempo_farthest;
                    }
                }
                catch (const std::exception& error)
                {
                    std::printf("%s: sequence %u: the tempo at tick %d can't move by %d ticks: %s\n", name.c_str(),
                                sequence, point.tick, farthest, error.what());
                    ++totals.problems;
                }
            }

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
            Edited edited_moves = hokuto::edit::MoveSettings(edited, sequence, moves);
            edited = std::move(edited_moves.file);
            moved[sequence] = Moved{view.sound, shifts, std::move(edited_moves.items)};
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
            bool loop_moved = false;
            if (!SamePlaying(before, read, loop_moved) && hokuto::edit::DrawsRandom(file, *sequence, before))
            {
                ++totals.tempo_move_random;
                continue;
            }
            if (!SamePlaying(before, read, loop_moved) && WaitsForNotes(file, *sequence, before))
            {
                ++totals.tempo_move_waits;
                continue;
            }
            if (!SamePlaying(before, read, loop_moved) && SamePlaying(before, read, loop_moved, kTickNotes))
            {
                ++totals.tempo_move_short;
                continue;
            }
            if (!SamePlaying(before, read, loop_moved))
            {
                std::printf("%s: %s plays other notes after the moved tempos\n", name.c_str(), info.name.c_str());
                ++totals.problems;
                continue;
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

// The sixth round: the tempo row (src/app/tempo_row.h). The first sound that ntdlib's performer reads of each checked
// sequence gives the points. Each point with a value that no later command overrides in its tick has to set the tempo
// that the performer plays from that tick. Each change of the tempo before the end of the loop's first pass has a point
// at its tick, or the tool counts it. Up to kMostTempos commands of each sequence then take a tempo 7 BPM away through
// the tempo row's drag, in one version of the file. The first sound of each edited sequence has to play its notes as
// before, at their ticks, and each point's tempo, the new one where its command changed. A sound that draws random
// numbers and plays other notes is counted apart: the draws follow the frames, and a new tempo moves the frames. Each
// other sound of an edited sequence that plays otherwise has to be among the sounds that the tempo row lists for an
// edited command. `places` gives where each sound plays (edit::SoundPlaces). The points then move in time (TempoMoves).
void TempoRow(const std::string& name, const File& file, const CheckedArchive& archive,
              const std::vector<std::optional<uint32_t>>& sequences,
              std::span<const std::optional<hokuto::edit::SoundPlace>> places, const std::set<uint32_t>& checked,
              Totals& totals)
{
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

    // The points against the performer's tempo. A point that a later command overrides in its tick decides nothing.
    // Neither does a command that runs only if the last comparison held, nor a point before it in its tick: the command
    // may not run.
    const auto overridden = [](const std::vector<TempoPointView>& points, std::size_t index)
    {
        return index + 1 < points.size() && points[index + 1].tick == points[index].tick;
    };
    const auto decides = [&overridden](const std::vector<TempoPointView>& points, std::size_t index)
    {
        return points[index].bpm && !points[index].conditional && !overridden(points, index);
    };
    for (const auto& [sequence, view] : shown)
    {
        const std::vector<TempoPointView>& points = view.points;
        totals.tempo_sequences += points.empty() ? 0 : 1;
        for (std::size_t i = 0; i < points.size(); ++i)
        {
            totals.tempo_points += points[i].bpm ? 1 : 0;
            totals.tempo_sources += points[i].bpm ? 0 : 1;
            totals.tempo_overridden += points[i].bpm && overridden(points, i) ? 1 : 0;
            totals.tempo_conditional += points[i].conditional ? 1 : 0;
            const auto tick = static_cast<uint32_t>(points[i].tick);
            if (decides(points, i) && hokuto::song::TempoAt(view.song, tick) != *points[i].bpm)
            {
                std::printf("%s: sequence %u: the point at tick %u sets %g BPM, and the performer plays %g\n",
                            name.c_str(), sequence, tick, *points[i].bpm, hokuto::song::TempoAt(view.song, tick));
                ++totals.problems;
            }
        }

        // The performer records the tempo at each timebase command too. A record of the tempo that plays already is no
        // change.
        double last = 120.0;
        for (const hokuto::song::Tempo& change : view.song.tempo)
        {
            const auto at = [&change](const TempoPointView& point)
            {
                return static_cast<uint32_t>(point.tick) == change.tick;
            };
            const bool past = view.song.loop && change.tick >= view.song.loop->end;
            const bool changes = change.bpm != std::exchange(last, change.bpm);
            if (changes && !past && std::none_of(points.begin(), points.end(), at))
            {
                std::printf("%s: sequence %u: the tempo changes to %g BPM at tick %u, where the row has no point\n",
                            name.c_str(), sequence, change.bpm, change.tick);
                ++totals.tempo_unpointed;
                ++totals.problems;
            }
        }
    }

    // The drags, in one version of the file. A value's change leaves every command in its place.
    File edited = file;
    std::map<uint32_t, std::map<uint32_t, double>> tempos;
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
                const CommandPlace place{point.command, track->stretches[point.stretch]};
                edited = hokuto::edit::ChangeCommand(edited, sequence, place, *command).file;
                changed[point.command] = static_cast<double>(command->value.value) / static_cast<double>(kOne);
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
            bool moved = false;
            if (!SamePlaying(view.song, read, moved) && hokuto::edit::DrawsRandom(file, sequence, view.song))
            {
                ++totals.tempo_random;
                continue;
            }
            if (!SamePlaying(view.song, read, moved) && WaitsForNotes(file, sequence, view.song))
            {
                ++totals.tempo_waits;
                continue;
            }
            if (!SamePlaying(view.song, read, moved) && SamePlaying(view.song, read, moved, kTickNotes))
            {
                ++totals.tempo_short;
                continue;
            }
            if (!SamePlaying(view.song, read, moved))
            {
                std::printf("%s: %s plays other notes after the new tempos\n", name.c_str(), sound.c_str());
                ++totals.problems;
                continue;
            }

            for (std::size_t i = 0; i < view.points.size(); ++i)
            {
                const TempoPointView& point = view.points[i];
                if (!decides(view.points, i))
                {
                    continue;
                }

                const auto found = changed.find(point.command);
                const double expected = found != changed.end() ? found->second : *point.bpm;
                const double played = hokuto::song::TempoAt(read, static_cast<uint32_t>(point.tick));
                if (played != expected)
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

    // The other sounds that the tempo row lists for the edited commands of each sequence.
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

    // Each other sound of an edited sequence plays as before, unless the tempo row lists it.
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

    TempoMoves(name, file, archive, sequences, shown, totals);
}

} // namespace

int main(int argc, char** argv)
{
    std::vector<std::string> arguments = xsflib::console::Arguments(argc, argv);
    hokuto::tools::TakeModelOption(arguments);
    const bool tempo_only = arguments.size() > 1 && arguments[1] == "--tempo";
    const bool moves_only = arguments.size() > 1 && arguments[1] == "--moves";
    if (tempo_only || moves_only)
    {
        arguments.erase(arguments.begin() + 1);
    }
    if (arguments.size() < 2 || arguments.size() > 3)
    {
        std::fprintf(stderr, "usage: hokuto_check_commands [--model] [--tempo | --moves] <archive> [sound]\n");
        return 2;
    }
    const std::string only = arguments.size() > 2 ? arguments[2] : std::string();

    // The archive and the group files beside it make a Hokuto file.
    const std::filesystem::path path = xsflib::PathFromUtf8(arguments[1]);
    const std::string name = xsflib::Utf8(path.filename());
    std::vector<uint8_t> bytes;
    CheckedArchive archive;
    File file;
    std::vector<std::optional<uint32_t>> sequences;
    std::vector<std::optional<hokuto::edit::SoundPlace>> places;
    try
    {
        xsflib::ReadFile(path, bytes);
        archive = hokuto::tools::OpenChecked(path, bytes);
        file = hokuto::tools::CheckedForm(hokuto::tools::ImportChecked(bytes, archive), bytes);
        sequences = hokuto::tools::SequencesOfChecked(file, bytes, archive);
        places = hokuto::tools::PlacesOfChecked(file, bytes, archive);
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "%s: %s\n", name.c_str(), error.what());
        return 2;
    }

    // The sequences that the sounds play, or the sequence of the named sound.
    std::set<uint32_t> checked;
    for (uint32_t sound = 0; sound < archive.SoundCount(); ++sound)
    {
        const CheckedSound info = archive.Info(sound);
        const bool named = only.empty() || info.name == only;
        if (info.sequence && named && info.file_id < sequences.size() && sequences[info.file_id])
        {
            checked.insert(*sequences[info.file_id]);
        }
    }

    // The sixth round and the seventh can run alone.
    Totals totals;
    if (tempo_only)
    {
        TempoRow(name, file, archive, sequences, places, checked, totals);
        PrintTempoTotals(name, totals);
        hokuto::tools::PrintSaves(name);
        std::printf("%s: %zu problems.\n", name.c_str(), totals.problems);
        return totals.problems == 0 ? 0 : 1;
    }
    if (moves_only)
    {
        SettingMoves(name, file, archive, sequences, SongsOf(archive, sequences, checked, only), totals);
        PrintMoveTotals(name, totals);
        hokuto::tools::PrintSaves(name);
        std::printf("%s: %zu problems.\n", name.c_str(), totals.problems);
        return totals.problems == 0 ? 0 : 1;
    }

    // The first two rounds, sequence by sequence. A sequence whose bytes keep their places takes only the first.
    std::set<uint32_t> movable;
    const hokuto::song::Console console = hokuto::edit::ConsoleOf(file);
    for (const uint32_t sequence : checked)
    {
        ++totals.sequences;
        const std::vector<uint32_t> chosen = Chosen(*file.sequences[sequence], kMostCommands);
        KeepValues(name, file, sequence, chosen, totals);
        if (hokuto::edit::Fixed(*file.sequences[sequence]))
        {
            ++totals.fixed;
            continue;
        }

        InsertAndRemove(name, file, sequence, chosen, totals);
        movable.insert(sequence);
    }

    // The third round: the waits, from the last chosen command back. Each place then stays where it was.
    File waits = file;
    for (const uint32_t sequence : movable)
    {
        const std::vector<uint32_t> chosen = Chosen(*file.sequences[sequence], kMostWaits);
        const std::vector<uint32_t> offsets = hokuto::edit::Offsets(*file.sequences[sequence], console);
        for (auto item = chosen.rbegin(); item != chosen.rend(); ++item)
        {
            try
            {
                waits = hokuto::edit::InsertCommand(waits, sequence, {offsets[*item], {}}, true, NoWait()).file;
                ++totals.waits;
            }
            catch (const std::exception& error)
            {
                std::printf("%s: sequence %u: a wait can't go in before item %u: %s\n", name.c_str(), sequence, *item,
                            error.what());
                ++totals.problems;
            }
        }
    }

    // Each sound of those sequences plays its notes again from the archive with the waits. The fifth round reads the
    // sounds as they played before too.
    std::map<uint32_t, hokuto::song::Sequence> songs;
    CheckedArchive after;
    try
    {
        hokuto::tools::CheckSaved(waits);
        after = hokuto::tools::BuildChecked(waits, name);
    }
    catch (const std::exception& error)
    {
        std::printf("%s: the file with the waits can't be exported: %s\n", name.c_str(), error.what());
        return 1;
    }
    for (uint32_t sound = 0; sound < archive.SoundCount(); ++sound)
    {
        const CheckedSound info = archive.Info(sound);
        const bool named = only.empty() || info.name == only;
        if (!info.sequence || !named || info.file_id >= sequences.size() || !sequences[info.file_id] ||
            !movable.contains(*sequences[info.file_id]))
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

        ++totals.sounds;
        try
        {
            const hokuto::song::Sequence read = after.Read(sound);
            const std::vector<Played> notes = NotesOf(*before);
            totals.notes += notes.size();
            if (NotesOf(read) != notes || read.length != before->length || read.loop != before->loop)
            {
                std::printf("%s: %s plays other notes after the waits: %zu notes before, %zu after\n", name.c_str(),
                            info.name.c_str(), notes.size(), NotesOf(read).size());
                ++totals.problems;
            }
        }
        catch (const std::exception& error)
        {
            std::printf("%s: %s can't be read after the waits: %s\n", name.c_str(), info.name.c_str(), error.what());
            ++totals.problems;
        }
        songs.emplace(sound, std::move(*before));
    }

    // The fourth round: a step of each chosen command's value, in one version of the file. A value change leaves every
    // item where it was.
    File values = file;
    std::map<uint32_t, std::set<uint32_t>> changed;
    for (const uint32_t sequence : checked)
    {
        for (const uint32_t item : Chosen(*file.sequences[sequence], kMostCommands))
        {
            const Command command = values.sequences[sequence]->items[item];
            const std::optional<Command> stepped = Stepped(console, command);
            if (!stepped)
            {
                continue;
            }

            try
            {
                values = hokuto::edit::ChangeCommand(values, sequence, PlaceOf(values, sequence, item), *stepped).file;
                changed[sequence].insert(item);
                if (stepped->value != command.value)
                {
                    ++totals.values;
                }
                else
                {
                    ++totals.times;
                }
            }
            catch (const std::invalid_argument& error)
            {
                if (!hokuto::edit::Fixed(*file.sequences[sequence]))
                {
                    std::printf("%s: sequence %u: the value of item %u can't move a step: %s\n", name.c_str(), sequence,
                                item, error.what());
                    ++totals.problems;
                }
                ++totals.refused;
            }
        }
    }

    // The exported archive gives the edited sequences back. A changed command can take other bytes for its value.
    try
    {
        hokuto::tools::CheckSaved(values);
        const std::vector<uint8_t> exported = hokuto::format::ExportArchive(values);
        const File again = hokuto::tools::ImportChecked(exported, archive, hokuto::format::ExportGroupFiles(values));
        for (const auto& [sequence, items] : changed)
        {
            const ntdlib::commands::Sequence& expected = *values.sequences[sequence];
            const ntdlib::commands::Sequence& read = *again.sequences.at(sequence);
            bool same = read.labels == expected.labels && read.items.size() == expected.items.size();
            for (std::size_t item = 0; same && item < expected.items.size(); ++item)
            {
                same = items.contains(static_cast<uint32_t>(item))
                           ? WithoutEncoding(read.items[item]) == WithoutEncoding(expected.items[item])
                           : read.items[item] == expected.items[item];
            }
            if (!same)
            {
                std::printf("%s: sequence %u doesn't come back with its new values\n", name.c_str(), sequence);
                ++totals.problems;
            }
        }
    }
    catch (const std::exception& error)
    {
        std::printf("%s: the file with the new values can't be exported: %s\n", name.c_str(), error.what());
        ++totals.problems;
    }

    NewSettings(name, file, archive, sequences, songs, totals);
    TempoRow(name, file, archive, sequences, places, checked, totals);
    SettingMoves(name, file, archive, sequences, songs, totals);

    std::printf(
        "%s: %zu sequences, %zu of them fixed in place. %zu commands kept their values, %zu waits went in and "
        "out, %zu waits went in together, and %zu sounds played their %zu notes again. %zu values and %zu "
        "times moved a step, and %zu steps were refused. %zu new settings went into %zu sequences, %zu were "
        "refused, and %zu sounds played their %zu notes again, %zu of them with a loop a whole number of passes "
        "away. %zu settings lay past the end of a loop that came sooner, where the rows don't reach.\n",
        name.c_str(), totals.sequences, totals.fixed, totals.kept, totals.round_trips, totals.waits, totals.sounds,
        totals.notes, totals.values, totals.times, totals.refused, totals.settings, totals.setting_sequences,
        totals.settings_refused, totals.setting_sounds, totals.setting_notes, totals.moved_loops, totals.settings_past);
    PrintTempoTotals(name, totals);
    PrintMoveTotals(name, totals);
    hokuto::tools::PrintSaves(name);
    std::printf("%s: %zu problems.\n", name.c_str(), totals.problems);

    return totals.problems == 0 ? 0 : 1;
}
