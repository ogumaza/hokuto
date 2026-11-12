// SPDX-License-Identifier: MIT

// Checks the edits of commands (src/edit/commands.h) on a sound archive of the Wii, the 3DS, the Wii U, the Switch or
// the DS, or on a GBA ROM with MusicPlayer2000's or Konami's driver. From a DS cartridge image, the tool uses the
// largest sound archive, or the DSE files when the image contains no sound archive (tools/checked_archive.h). The List
// Editor makes these edits. The tool also checks the new settings that the Key Editor's lanes draw (edit::AddSettings),
// the points of the arrangement's tempo row and their drags (src/app/tempo_row.h), and the moves of settings in time
// (edit::MoveSettings). It imports the archive into a Hokuto file the way the program does. It then makes seven rounds
// of edits of each sequence. Up to 256 commands of a sequence take part in the first four, spread over the sequence.
//
// - Each command takes its values again. The file has to stay as it was.
// - A wait of no ticks goes in before each command, and then goes again. The file has to come back as it was. The round
//   runs twice: once with the code that enters at the command running the wait first, and once without. On the GBA, a
//   wait after data stays out of the second run: no code would reach it, and MusicPlayer2000's decoder keeps the bytes
//   that no track reaches after data as data. The List Editor enters at such a command. On the DS, a wait before the
//   allocation of tracks at a sequence's first command has to be refused: NITRO-Composer reads the allocation only
//   there. The next round leaves that command out. On Nintendo R&D2's driver, the entries of a table of tracks at a
//   sequence's start are no commands, and the rounds leave them out. A wait right after the table stays out of the
//   second run: no code would reach it. A note or a wait of one byte there uses the values of the last note or wait of
//   the track. After the new wait, the notes and waits that would read its values store their values in their bytes,
//   and they keep those bytes once the wait is removed. The file then has to come back with the same values, and the
//   tool counts such a wait apart. A driver without a wait command, such as Quintet's in its revisions of one-byte
//   notes, has to refuse each wait. The tool counts the refusals apart, and the next round doesn't run. Krawall is such
//   a driver: a wait ends a row and counts the rows, and the driver's bytes hold no wait alone.
// - A wait of no ticks goes in before each of up to 64 commands of each sequence, in one version of the file. Code that
//   enters at a command runs the wait first. ntdlib's performer then reads each sequence sound from the exported
//   archive. Each sound has to play the notes that it played before, at the same ticks. Brownie Brown's driver has no
//   wait of no ticks: a hold waits for the channel's length. Its sequences stay out of this round. In the round before,
//   a new hold there uses the length in force, and it has to be removed again as cleanly. A hold that uses the length
//   of a byte that no channel read leaves the byte unread once it is removed, and the removal drops the byte. The tool
//   counts such a hold apart.
// - Each value that a command takes from its bytes moves a step, within the range that the console's bytes hold, in
//   one version of the file. A command without such a value moves the time over which its setting moves instead. Notes
//   keep their values: hokuto_check_edits checks them. The exported archive, imported again, has to give back the
//   edited sequences. On the GBA, a wait that steps off MusicPlayer2000's codes splits, and the song grows. The export
//   then moves the song, and zeros can follow it at its new place (tools::HoldsSequence).
// - New settings go in on the tracks of the first sound of each sequence, in one version of the file: a volume, a pan
//   and a second volume in turn. Each track with notes takes up to four, at the starts of its first note and of a note
//   halfway through its notes, and halfway from each of them to the next note. A GBA song has no second volume, and
//   receives a volume and a pan in turn. Each setting goes into the last stretch that starts by its tick. A setting
//   that the lanes draw goes there too. ntdlib's performer then reads each sound of those sequences from the exported
//   archive. Each sound has to play the notes that it played before, at their ticks, with their keys and velocities. A
//   note can sound for another time: the arrangement ends a note once it fades below hearing. On the GBA, a note of a
//   PSG channel on a track that took a setting can fall silent, or sound where it was silent: the tool counts such a
//   sound apart (tools::PsgNotesApart). On Nintendo R&D2's driver, the tool also counts apart a sound whose notes
//   differ only in notes of a square, the wave or the noise on such a track (tools::Rd2PsgNotesApart). A loop can start
//   a whole number of passes earlier or later: the performer finds a loop where a track's state repeats, and the edit
//   can change that state. On Nintendo R&D2's driver, a loop as long can also start part of a pass away, and the tool
//   counts such sounds (PartPass). On the first sound's tracks, each new setting has to run at its tick, before the
//   notes that its stretch starts there. When the loop starts a whole number of passes earlier, the rows after the edit
//   end sooner. The tool counts the settings past their end apart. Every other command has to run at the tick at which
//   it ran before.
// - The first sound of each sequence gives the points of the tempo row. A point with a value that no later command
//   overrides in its tick has to set the tempo that ntdlib's performer plays from that tick. On the GBA, the point
//   shows the command's value, and the real tempo is about 59.73 / 60 of that. On the DS, the real tempo is about
//   0.99977 of that (TempoRules). The GBA applies a new tempo from its next frame, up to 3 ticks after the command. A
//   later command in those ticks can set the tempo before the point's tempo takes effect, or the loop's first pass can
//   end there. The tool counts such a point apart. Each change of the tempo before the end of the loop's first pass has
//   to have a point at its tick. Up to 8 commands of each sequence then take a tempo 7 BPM away through the tempo row's
//   drag, in one version of the file. The first sound of each edited sequence has to play its notes as before, and each
//   point's tempo, the new one where its command changed. ntdlib's performer plays 15 minutes at most. A sound whose
//   loop takes longer reads without a loop, and a new tempo can bring the loop inside the limit. The notes then compare
//   up to the end of the read that stopped at the limit. A sound that draws random numbers can play other notes: the
//   draws follow the frames, and a new tempo moves the frames. The tool counts such sounds apart. So does a sound with
//   a note that waits until the track's notes have ended: a note of length 0, or of a length that the sequence decides
//   as it plays, with the note wait on. Such a wait lasts a time rather than a number of ticks. So does a sound that
//   gains or loses only notes of a tick. A frame of the 3DS lasts 4.89 ms, and a tick at a new tempo can end in the
//   frame where it starts. On the GBA, so does a sound that runs the same note commands, but whose notes find other
//   channels: a new tempo moves the frames where earlier notes end and release (SameNoteCommands). So does a sound
//   whose other notes differ only on the PSG channels: a note there that lasts fewer frames can end before its envelope
//   sounds. On Nintendo R&D2's driver, so does a sound whose voices play other notes: each note that only one read
//   contains is at a tick of a note command of its track (VoicesDecide). On Rare's driver, so does a tune whose frames
//   contain other notes: the time of each delay is computed at the tempo in force when its track reads it, and a new
//   tempo moves the frames where the tracks' later ticks fall (FramesDecide). On Quintet's driver, so does a song whose
//   notes start in other frames: each note and rest lasts whole frames, and a new tempo moves a later note by up to a
//   frame for each reset of its count of ticks, and one more. The conversion can then split a command's note otherwise,
//   and number the noise channel's drums otherwise. A channel that sounds frames after its command can start a note up
//   to two more frames away, and a note shorter than two frames can sound in one read only. The tool counts those notes
//   apart (QuintetFramesDecide). A new tempo also changes the commands of the same tempo on the other channels at the
//   tick of the point (app::TempoChangeOf). Each other sound of an edited sequence that plays otherwise has to be among
//   the sounds that the tempo row lists for an edited command (edit::SoundsReaching). Each point with room to move then
//   has to move as far as its room allows, earlier and later, alone. Up to four points of each sequence then move half
//   as far, in another version of the file. Every sound of those sequences has to play its notes as before, unless it
//   draws random numbers, has a note that waits until the track's notes have ended, gains or loses only notes of a
//   tick, on the GBA finds other channels for its notes or plays other notes of a PSG channel, on Nintendo R&D2's
//   driver has voices that play other notes, on Rare's driver has frames that contain other notes, or on Quintet's
//   driver starts its notes in other frames. A tempo moves the time of the ticks, and the tick where every track's
//   state starts to repeat can move with it. A sound whose loop then starts elsewhere, as long as before, counts apart
//   when its notes start as before over both loops (SameOverLoops), after the new tempos too. On Rare's driver, each
//   track repeats the loop of its commands there, and the frames can take other notes as in FramesDecide. The first
//   sound has to run each moved command at its new tick. On Krawall, the effects of the module's rows set the speed and
//   the tempo. The tempo row shows no points for them, and the tool counts each change of the tempo apart. A sound
//   without a loop whose last change comes more than 10 s after its last sound has died away ends where that sound dies
//   away (ntdlib's EndsAtLastSound). The performer records no tempo after that end. The tool counts a point past it
//   apart, before the drags and after them.
// - Settings move in time on the tracks of the first sound of each sequence, in one version of the file. Each track
//   moves up to four commands that MoveSettings can move, spread over the commands that it ran. A command whose move
//   can change the notes stays out, such as a transpose, a program change or a random value. On the GBA, so does a
//   volume of 0, and a volume that follows a volume of 0: a track starts at volume 0, and a note at volume 0 doesn't
//   sound. On Krawall, so does an effect that sets the speed or the tempo, sends the rows elsewhere, or starts, delays,
//   cuts or slides to a note. As in the fifth round, a note of a PSG channel on a track whose volume or pan moved can
//   fall silent or sound. The tool counts such a sound apart. So does each track that ties its notes or plays them
//   monophonically. On Konami's driver, a volume that a move puts after a note in the note's tick can keep the note
//   from starting. The tool counts apart a sound that loses notes that way and gains none, here and in the fifth round.
//   Each moves half as far as LimitsOf allows: later and earlier in turn, or the other way where it has no room.
//   ntdlib's performer then reads each sound of those sequences from the exported archive. Each sound has to play its
//   notes as before, as in the fifth round. A sound with a note that waits until the track's notes have ended counts
//   apart: a setting of the envelope that moves past a note changes when that note ends. On Rare's driver, a delay that
//   a move splits can take another time: its fraction of a microsecond is cut to 8 bits. A tune whose frames take other
//   notes then counts apart (FramesDecide). On the first sound's tracks, each moved command has to run at its tick and
//   the move's, before the notes that its stretch starts there. A move past the tick where the performer stops reading
//   the sound stays out. Every other command has to run at the tick at which it ran before. A setting that moves on a
//   track that ends, past the loop's start, moves the tick where every track's state starts to repeat. The loop then
//   starts any number of ticks away, and the tool counts the sound apart when both loops last as long and the notes
//   start as before up to the later loop's end (SameOverLoops). On Krawall, a setting moves by rows, to a row whose
//   entry of its channel has no item of the setting's field. In the stretch of its move, it has to run in the row that
//   plays at its tick plus the move. In every other pass of its row, it has to run as many rows on, where the rows
//   follow on from its row. It has to join the entry of its channel there: the channel's items of the row are adjacent,
//   in the order note, volume and effect.
//
// On the DS, the driver assigns a new note the quietest channel that its instrument may use. A new setting, a moved
// setting or a new tempo changes which channel that is, and a note can then find another channel, or none. As on the
// GBA, a sound whose notes differ only in the channels that the driver found for them counts apart in each round
// (SameNoteCommands).
//
// A GBA ROM that fills its 32 MiB without free space has no room for a song that grows (tools::Roomless). On such a
// ROM, the rounds that make songs longer leave out the edits of each song that outgrows its place, and count them. With
// --filler, the songs that don't fit are written to the filler runs of the ROM instead
// (format::NativeArchive::gba_filler), and every edit is kept.
//
// A sequence whose bytes keep their places (edit::KeepsPlaces) refuses new commands, removals and changes of a
// command's size. A new setting outside the blocks of its stretch is refused too. The tool counts these refusals apart,
// and prints the number of each message of the new settings' refusals. A move inside the limits that LimitsOf gives
// can't be refused. It prints a line for each problem, and a summary. It returns 1 when a sequence or a sound differs,
// or when an edit, a read or the export fails. With a sound's name, it checks the sound's sequence alone. With --tempo,
// it makes the sixth round alone, and with --moves the seventh.
//
// With --model, the file goes into the model form before the edits (format::InModelForm), and the archive that the
// model builds has to be the game's. Each file that the edits make then has to save in both forms and open again as it
// was (tools/checked_archive.h).
//
// usage: hokuto_check_commands [--model] [--filler] [--tempo | --moves] <archive> [sound]

#include "check_commands.h"

namespace hokuto::tools::commands
{

namespace
{

// Up to `most` items of `sequence`, a sequence of `file`, spread evenly over it, without bytes that no command reads
// and without the entries of a table of tracks.
std::vector<uint32_t> Chosen(const File& file, const ntdlib::commands::Sequence& sequence, std::size_t most)
{
    std::vector<uint32_t> chosen;
    const std::size_t count = sequence.items.size();
    const std::size_t step = std::max<std::size_t>(1, (count + most - 1) / most);
    for (std::size_t item = 0; item < count; item += step)
    {
        if (sequence.items[item].kind != Kind::kData && !TableEntry(file, sequence.items[item]))
        {
            chosen.push_back(static_cast<uint32_t>(item));
        }
    }

    return chosen;
}

} // namespace

Command NoWait()
{
    Command wait;
    wait.kind = Kind::kWait;

    return wait;
}

CommandPlace PlaceOf(const File& file, uint32_t sequence, uint32_t item)
{
    return {hokuto::edit::Offsets(*file.sequences[sequence], hokuto::edit::ConsoleOf(file)).at(item), {}};
}

namespace
{

// Whether `edited` contains a song of sequence `sequence` that outgrows its place in a GBA ROM without room for it
// (tools::Outgrows). The totals count such a song. An export that refuses the file for another reason prints a line
// about `what` and counts a problem. Either way, the round leaves the edits of the sequence out.
bool OutgrowsOrFails(const std::string& name, const CheckedArchive& archive, const File& edited, uint32_t sequence,
                     const std::string& what, Totals& totals)
{
    try
    {
        if (hokuto::tools::Outgrows(archive, edited, sequence))
        {
            ++totals.outgrown;
            return true;
        }
    }
    catch (const std::runtime_error& error)
    {
        std::printf("%s: sequence %u: %s: %s\n", name.c_str(), sequence, what.c_str(), error.what());
        ++totals.problems;
        return true;
    }

    return false;
}

// Prints how often a round left a song's edits out on the GBA, Konami's driver included: the song outgrew its place in
// a ROM without room for it (Outgrows).
void PrintOutgrown(const std::string& name, const Totals& totals, bool gba)
{
    if (gba)
    {
        std::printf(
            "%s: apart on the GBA: %zu times, a round's edits of a song stayed out: the song outgrew its place, "
            "and the ROM has no room for it.\n",
            name.c_str(), totals.outgrown);
    }
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

} // namespace

} // namespace hokuto::tools::commands

namespace
{

using hokuto::app::TempoPointView;
using hokuto::edit::CommandPlace;
using hokuto::edit::Edited;
using hokuto::format::File;
using hokuto::tools::CheckedArchive;
using hokuto::tools::CheckedSound;
using hokuto::tools::RareFrameTicks;
using hokuto::tools::SpreadIndices;
using hokuto::tools::commands::AllocationFirst;
using hokuto::tools::commands::brownie_song;
using hokuto::tools::commands::Chosen;
using hokuto::tools::commands::InsertAndRemove;
using hokuto::tools::commands::KeepValues;
using hokuto::tools::commands::kMostCommands;
using hokuto::tools::commands::kMostWaits;
using hokuto::tools::commands::konami_song;
using hokuto::tools::commands::NewSettings;
using hokuto::tools::commands::NotesOf;
using hokuto::tools::commands::NoWait;
using hokuto::tools::commands::OutgrowsOrFails;
using hokuto::tools::commands::PlaceOf;
using hokuto::tools::commands::Played;
using hokuto::tools::commands::PrintMoveTotals;
using hokuto::tools::commands::PrintOutgrown;
using hokuto::tools::commands::PrintTempoTotals;
using hokuto::tools::commands::rd2_song;
using hokuto::tools::commands::SettingMoves;
using hokuto::tools::commands::SongsOf;
using hokuto::tools::commands::Stepped;
using hokuto::tools::commands::TempoRow;
using hokuto::tools::commands::TempoRulesOf;
using hokuto::tools::commands::Totals;
using hokuto::tools::commands::WithoutEncoding;
using ntdlib::commands::Command;
using ntdlib::commands::Kind;
using ntdlib::commands::kOne;
using ntdlib::commands::Source;

// The sequences that the sounds of `archive` play, or the sequence of the sound named `only`.
std::set<uint32_t> CheckedSequences(const CheckedArchive& archive,
                                    const std::vector<std::optional<uint32_t>>& sequences, const std::string& only)
{
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

    return checked;
}

// Runs the first two rounds on each sequence of `checked`, and returns the sequences whose bytes don't keep their
// places.
std::set<uint32_t> RunFirstRounds(const std::string& name, const File& file, const std::set<uint32_t>& checked,
                                  Totals& totals)
{
    std::set<uint32_t> movable;
    for (const uint32_t sequence : checked)
    {
        ++totals.sequences;
        const std::vector<uint32_t> chosen = Chosen(file, *file.sequences[sequence], kMostCommands);
        KeepValues(name, file, sequence, chosen, totals);
        if (hokuto::edit::KeepsPlaces(file, *file.sequences[sequence]))
        {
            ++totals.fixed;
            continue;
        }

        InsertAndRemove(name, file, sequence, chosen, totals);
        movable.insert(sequence);
    }

    return movable;
}

// Runs the third round on each sequence of `movable`, and returns the file with the waits.
File RunWaitRound(const std::string& name, const File& file, const CheckedArchive& archive,
                  const std::set<uint32_t>& movable, hokuto::song::Console console, Totals& totals)
{
    File waits = file;
    for (const uint32_t sequence : movable)
    {
        if (archive.Brownie() || !hokuto::song::Runs(console, Kind::kWait, 0))
        {
            break;
        }
        const std::vector<uint32_t> chosen = Chosen(file, *file.sequences[sequence], kMostWaits);
        const std::vector<uint32_t> offsets = hokuto::edit::Offsets(*file.sequences[sequence], console);
        File edited = waits;
        std::size_t count = 0;
        for (auto item = chosen.rbegin(); item != chosen.rend(); ++item)
        {
            if (AllocationFirst(file, *file.sequences[sequence], *item))
            {
                continue;
            }

            try
            {
                edited = hokuto::edit::InsertCommand(edited, sequence, {offsets[*item], {}}, true, NoWait()).file;
                ++count;
            }
            catch (const std::exception& error)
            {
                std::printf("%s: sequence %u: a wait can't go in before item %u: %s\n", name.c_str(), sequence, *item,
                            error.what());
                ++totals.problems;
            }
        }
        if (OutgrowsOrFails(name, archive, edited, sequence, "the waits", totals))
        {
            continue;
        }
        waits = std::move(edited);
        totals.waits += count;
    }

    return waits;
}

// Checks that each sound of the sequences of `movable` plays its notes again from `waits`, the file with the waits.
// Returns false when the file can't be exported. With `only`, only the sound of that name.
bool CheckWaitedSounds(const std::string& name, const CheckedArchive& archive,
                       const std::vector<std::optional<uint32_t>>& sequences, const std::set<uint32_t>& movable,
                       const File& waits, const std::string& only, Totals& totals)
{
    CheckedArchive after;
    try
    {
        hokuto::tools::CheckSaved(waits);
        after = hokuto::tools::BuildChecked(waits, name);
    }
    catch (const std::exception& error)
    {
        std::printf("%s: the file with the waits can't be exported: %s\n", name.c_str(), error.what());
        return false;
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
    }

    return true;
}

// Runs the fourth round on each sequence of `checked`, and returns the file with the steps. `changed` receives the
// items whose values moved, by sequence.
File RunStepRound(const std::string& name, const File& file, const CheckedArchive& archive,
                  const std::set<uint32_t>& checked, hokuto::song::Console console,
                  std::map<uint32_t, std::set<uint32_t>>& changed, Totals& totals)
{
    File values = file;
    for (const uint32_t sequence : checked)
    {
        File edited = values;
        std::set<uint32_t> items;
        std::size_t stepped_values = 0;
        std::size_t stepped_times = 0;
        for (const uint32_t item : Chosen(file, *file.sequences[sequence], kMostCommands))
        {
            const Command command = edited.sequences[sequence]->items[item];
            const std::optional<Command> stepped = Stepped(console, command);
            if (!stepped)
            {
                continue;
            }

            try
            {
                edited = hokuto::edit::ChangeCommand(edited, sequence, PlaceOf(edited, sequence, item), *stepped).file;
                items.insert(item);
                if (stepped->value != command.value)
                {
                    ++stepped_values;
                }
                else
                {
                    ++stepped_times;
                }
            }
            catch (const std::invalid_argument& error)
            {
                // The duty of a square wave, or a wave on Konami's driver, has only some valid values, and a step can
                // leave them. An instrument fits in a byte.
                const bool duty =
                    hokuto::edit::KonamiOf(console) && command.kind == Kind::kProgram && command.index != 2;
                if (!hokuto::edit::KeepsPlaces(file, *file.sequences[sequence]) && !duty)
                {
                    std::printf("%s: sequence %u: the value of item %u, a %s, can't move a step: %s\n", name.c_str(),
                                sequence, item, std::string(ntdlib::commands::Name(command.kind)).c_str(),
                                error.what());
                    ++totals.problems;
                }
                ++totals.refused;
            }
        }
        if (OutgrowsOrFails(name, archive, edited, sequence, "the steps of the values", totals))
        {
            continue;
        }
        values = std::move(edited);
        if (!items.empty())
        {
            changed[sequence] = std::move(items);
        }
        totals.values += stepped_values;
        totals.times += stepped_times;
    }

    return values;
}

// Checks that the archive that `values` exports contains the sequences of `changed` with their new values.
void CheckExportedValues(const std::string& name, const CheckedArchive& archive,
                         const std::vector<std::optional<uint32_t>>& sequences, const File& values,
                         const std::map<uint32_t, std::set<uint32_t>>& changed, Totals& totals)
{
    try
    {
        hokuto::tools::CheckSaved(values);
        const std::vector<uint8_t> exported = hokuto::format::ExportArchive(values);
        const File again = hokuto::tools::ImportChecked(exported, archive, hokuto::format::ExportGroupFiles(values));
        const std::vector<std::optional<uint32_t>> again_sequences =
            archive.gba || archive.driver ? hokuto::tools::SequencesOfChecked(again, exported, archive)
                                          : std::vector<std::optional<uint32_t>>();
        for (const auto& [sequence, items] : changed)
        {
            // A GBA ROM's import numbers the sequences by their places, and an export can move a song. A song that
            // plays the sequence finds it.
            uint32_t found = sequence;
            if (archive.gba || archive.driver)
            {
                const auto plays = std::find(sequences.begin(), sequences.end(), std::optional<uint32_t>(sequence));
                found = again_sequences.at(static_cast<std::size_t>(plays - sequences.begin())).value();
            }
            const ntdlib::commands::Sequence& expected = *values.sequences[sequence];
            const ntdlib::commands::Sequence& read = *again.sequences.at(found);
            bool same = read.labels == expected.labels && read.items.size() == expected.items.size();
            for (std::size_t item = 0; same && item < expected.items.size(); ++item)
            {
                same = items.contains(static_cast<uint32_t>(item))
                           ? WithoutEncoding(read.items[item]) == WithoutEncoding(expected.items[item])
                           : read.items[item] == expected.items[item];
            }

            // On the GBA, the bytes come back, with zeros after a song that the export moved (HoldsSequence). A wait
            // that steps off MusicPlayer2000's codes splits, and the song grows.
            if (archive.gba || archive.dse || archive.driver)
            {
                same = hokuto::tools::HoldsSequence(archive, expected, read);
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
}

// Prints the totals of the rounds, and what counts apart on the console or the driver of `archive`.
void PrintRoundTotals(const std::string& name, const CheckedArchive& archive, const Totals& totals)
{
    std::printf(
        "%s: %zu sequences, %zu of them fixed in place. %zu commands kept their values, %zu waits went in and "
        "out, %zu waits went in together, and %zu sounds played their %zu notes again. %zu values and %zu "
        "times moved a step, and %zu steps were refused. %zu new settings went into %zu sequences, %zu were "
        "refused, and %zu sounds played their %zu notes again, %zu of them with a loop a whole number of passes "
        "away. %zu settings lay past the end of a loop that came sooner, where the rows don't reach.\n",
        name.c_str(), totals.sequences, totals.fixed, totals.kept, totals.round_trips, totals.waits, totals.sounds,
        totals.notes, totals.values, totals.times, totals.refused, totals.settings, totals.setting_sequences,
        totals.settings_refused, totals.setting_sounds, totals.setting_notes, totals.moved_loops, totals.settings_past);
    if (archive.gba)
    {
        std::printf(
            "%s: apart on the GBA: %zu sounds whose notes of a PSG channel differ after the new settings. %zu "
            "waits after data, where no code would reach them, stayed out.\n",
            name.c_str(), totals.psg_sounds, totals.after_data);
    }
    if (archive.Rd2())
    {
        std::printf(
            "%s: apart on Nintendo R&D2's driver: %zu sounds whose notes of a PSG channel differ after the new "
            "settings. %zu waits right after a table of tracks, where no code would reach them, stayed out. After "
            "%zu waits went again, notes and waits gave the values in their bytes that they had taken from the "
            "track's last note or wait. %zu sounds after the new settings and %zu after the moves looped from a start "
            "part of a pass away.\n",
            name.c_str(), totals.psg_sounds, totals.after_table, totals.respelled, totals.part_passes,
            totals.move_part_passes);
    }
    if (archive.Brownie())
    {
        std::printf(
            "%s: apart on Brownie Brown's driver: %zu sounds whose notes of a PSG channel differ after the new "
            "settings. %zu new holds took the length of a byte that no channel had read, and the byte went "
            "with them. %zu sounds after the new settings and %zu after the moves looped from a start part of a pass "
            "away.\n",
            name.c_str(), totals.psg_sounds, totals.dropped_lengths, totals.part_passes, totals.move_part_passes);
    }
    if (archive.Konami())
    {
        std::printf(
            "%s: apart on Konami's driver: the notes of the PSG tracks differ in %zu sounds after the new settings and "
            "%zu after the moves. %zu and %zu that lose notes to a volume after a note in its tick. %zu moves of a "
            "call's last command were refused. %zu moves of a volume were refused: the volume would start another "
            "note. %zu moves were refused in a song whose loop cuts a track short.\n",
            name.c_str(), totals.psg_sounds, totals.move_psg, totals.setting_cuts, totals.move_cuts, totals.move_calls,
            totals.move_starts, totals.move_cut_songs);
    }
    if (totals.without_waits != 0)
    {
        std::printf("%s: apart: %zu waits were refused on a driver without a wait command.\n", name.c_str(),
                    totals.without_waits);
    }
    if (archive.ds)
    {
        std::printf(
            "%s: apart on the DS: %zu waits before the allocation of tracks at a sequence's first command were "
            "refused. %zu sounds whose notes find other channels after the new settings, %zu after the moves of "
            "settings, %zu after the new tempos and %zu after the moved tempos.\n",
            name.c_str(), totals.before_allocation, totals.setting_channels, totals.move_channels,
            totals.tempo_channels, totals.tempo_move_channels);
    }
    if (archive.Krawall())
    {
        std::printf(
            "%s: apart on Krawall: %zu changes of the tempo came from the effects of the rows. The tempo row shows no "
            "points for them.\n",
            name.c_str(), totals.tempo_effects);
    }
    if (archive.dse)
    {
        std::printf(
            "%s: apart on DSE: %zu sounds with a track that runs past its chunk, into bytes that the file holds "
            "as data, play other notes after the moves of settings.\n",
            name.c_str(), totals.move_past_end);
    }
}

// The options of the command line: a round to run alone, if any, the archive, and the sound to check, if only one.
struct Options
{
    bool tempo_only = false;
    bool moves_only = false;
    std::filesystem::path path;
    std::string only;
};

// The options of the command line's `arguments`, or nothing after the usage.
std::optional<Options> ParseArguments(std::vector<std::string> arguments)
{
    hokuto::tools::TakeModelOption(arguments);
    hokuto::tools::TakeFillerOption(arguments);
    const bool tempo_only = arguments.size() > 1 && arguments[1] == "--tempo";
    const bool moves_only = arguments.size() > 1 && arguments[1] == "--moves";
    if (tempo_only || moves_only)
    {
        arguments.erase(arguments.begin() + 1);
    }
    if (arguments.size() < 2 || arguments.size() > 3)
    {
        std::fprintf(stderr,
                     "usage: hokuto_check_commands [--model] [--filler] [--tempo | --moves] <archive> [sound]\n");
        return std::nullopt;
    }
    const std::string only = arguments.size() > 2 ? arguments[2] : std::string();

    return Options{tempo_only, moves_only, xsflib::PathFromUtf8(arguments[1]), only};
}

// The archive of a run, the Hokuto file that the archive makes, and the sequences and places of its sounds.
struct CommandsFile
{
    CheckedArchive archive;
    File file;
    std::vector<std::optional<uint32_t>> sequences;
    std::vector<std::optional<hokuto::edit::SoundPlace>> places;
};

// Opens the archive at `path` and makes its Hokuto file, with the group files beside it. Sets the driver's flags of
// the rounds. Throws where the archive can't be read.
CommandsFile OpenCommandsFile(const std::filesystem::path& path)
{
    // The archive and the group files beside it make a Hokuto file.
    CommandsFile opened;
    std::vector<uint8_t> bytes;
    xsflib::ReadFile(path, bytes);
    opened.archive = hokuto::tools::OpenChecked(path, bytes);
    konami_song = opened.archive.Konami() != nullptr;
    rd2_song = opened.archive.Rd2() != nullptr;
    brownie_song = opened.archive.Brownie() != nullptr;
    opened.file = hokuto::tools::CheckedForm(hokuto::tools::ImportChecked(bytes, opened.archive), bytes);
    opened.sequences = hokuto::tools::SequencesOfChecked(opened.file, bytes, opened.archive);
    opened.places = hokuto::tools::PlacesOfChecked(opened.file, bytes, opened.archive);

    return opened;
}

// Runs the seven rounds on the sequences of `checked` in `opened`, the archive `name`, and counts them in `totals`.
// With `only`, the rounds read only the sound of that name. Returns false when the file with the waits can't be
// exported.
bool RunRounds(const std::string& name, const CommandsFile& opened, const std::set<uint32_t>& checked,
               const std::string& only, Totals& totals)
{
    const auto& [archive, file, sequences, places] = opened;

    // The first two rounds, sequence by sequence. A sequence whose bytes keep their places takes only the first.
    const hokuto::song::Console console = hokuto::edit::ConsoleOf(file);
    const std::set<uint32_t> movable = RunFirstRounds(name, file, checked, totals);

    // The third round: the waits, from the last chosen command back. Each place then stays where it was. Brownie
    // Brown's driver has no wait of no ticks: a hold waits for the channel's length. Its sequences take none. Neither
    // do those of a driver without a wait command, such as Krawall.
    const File waits = RunWaitRound(name, file, archive, movable, console, totals);

    // Each sound of those sequences plays its notes again from the archive with the waits.
    if (!CheckWaitedSounds(name, archive, sequences, movable, waits, only, totals))
    {
        return false;
    }

    // The fourth round: a step of each chosen command's value, in one version of the file. A value change leaves every
    // item where it was.
    std::map<uint32_t, std::set<uint32_t>> changed;
    const File values = RunStepRound(name, file, archive, checked, console, changed, totals);

    // The exported archive gives the edited sequences back. A changed command can take other bytes for its value.
    CheckExportedValues(name, archive, sequences, values, changed, totals);

    // The fifth round and the seventh read every sound of the sequences of `checked` as it played before, those whose
    // bytes keep their places too.
    const std::map<uint32_t, hokuto::song::Sequence> songs = SongsOf(archive, sequences, checked, only);
    NewSettings(name, file, archive, sequences, songs, totals);
    TempoRow(name, file, archive, sequences, places, checked, totals);
    SettingMoves(name, file, archive, sequences, songs, totals);

    return true;
}

} // namespace

int main(int argc, char** argv)
{
    const std::optional<Options> options = ParseArguments(xsflib::console::Arguments(argc, argv));
    if (!options)
    {
        return 2;
    }
    const auto& [tempo_only, moves_only, path, only] = *options;

    const std::string name = xsflib::Utf8(path.filename());
    CommandsFile opened;
    try
    {
        opened = OpenCommandsFile(path);
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "%s: %s\n", name.c_str(), error.what());
        return 2;
    }
    const auto& [archive, file, sequences, places] = opened;

    // The sequences that the sounds play, or the sequence of the named sound.
    const std::set<uint32_t> checked = CheckedSequences(archive, sequences, only);

    // The sixth round and the seventh can run alone.
    Totals totals;
    if (tempo_only)
    {
        TempoRow(name, file, archive, sequences, places, checked, totals);
        PrintTempoTotals(name, totals, archive.gba != nullptr, archive.Rd2() != nullptr, archive.Rare() != nullptr,
                         TempoRulesOf(archive).quarter_ticks != 0);
        PrintOutgrown(name, totals, archive.gba != nullptr || archive.driver != nullptr);
        hokuto::tools::PrintSaves(name);
        std::printf("%s: %zu problems.\n", name.c_str(), totals.problems);
        return totals.problems == 0 ? 0 : 1;
    }
    if (moves_only)
    {
        SettingMoves(name, file, archive, sequences, SongsOf(archive, sequences, checked, only), totals);
        PrintMoveTotals(name, totals, archive.gba != nullptr);
        PrintOutgrown(name, totals, archive.gba || archive.driver);
        hokuto::tools::PrintSaves(name);
        std::printf("%s: %zu problems.\n", name.c_str(), totals.problems);
        return totals.problems == 0 ? 0 : 1;
    }
    if (!RunRounds(name, opened, checked, only, totals))
    {
        return 1;
    }

    PrintRoundTotals(name, archive, totals);
    PrintTempoTotals(name, totals, archive.gba != nullptr, archive.Rd2() != nullptr, archive.Rare() != nullptr,
                     TempoRulesOf(archive).quarter_ticks != 0);
    PrintMoveTotals(name, totals, archive.gba != nullptr);
    PrintOutgrown(name, totals, archive.gba || archive.driver);
    hokuto::tools::PrintSaves(name);
    std::printf("%s: %zu problems.\n", name.c_str(), totals.problems);

    return totals.problems == 0 ? 0 : 1;
}
